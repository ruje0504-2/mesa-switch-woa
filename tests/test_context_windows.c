/* SPDX-License-Identifier: MIT */
#include "../windows/context.h"
#include "../windows/work_queue.h"
#include "../windows/allocation.h"
#include "../windows/query.h"
#include "../windows/adapter.h"
#include "../platform/t214_resources.h"
#include <ntimage.h>

static ULONG_PTR image_cookie=0x00002b992ddfa232ULL;
const IMAGE_LOAD_CONFIG_DIRECTORY64 _load_config_used={
    .Size=sizeof(IMAGE_LOAD_CONFIG_DIRECTORY64),.SecurityCookie=(ULONGLONG)&image_cookie
};
__declspec(dllimport) void __stdcall ExitProcess(ULONG);
__declspec(dllimport) HANDLE __stdcall GetStdHandle(ULONG);
__declspec(dllimport) BOOL __stdcall WriteFile(HANDLE,const void *,ULONG,ULONG *,void *);
#define CHECK(x) do { if (!(x)) ExitProcess(__LINE__); } while (0)
void *memset(void *p,int value,size_t n)
{ UCHAR *d=p; while(n--) *d++=(UCHAR)value; return p; }
void *memcpy(void *p,const void *source,size_t n)
{ UCHAR *d=p;const UCHAR *s=source;while(n--) *d++=*s++;return p; }

/* Real WDK types and backend code, with explicit OS/GPU substitutes. Nothing
 * here allocates through live Dxgkrnl or proves physical GPU completion.
 */
static _Alignas(PAGE_SIZE) UCHAR pools[64][PAGE_SIZE], memory[64][TG_GPU_TABLE_BYTES];
static BOOLEAN pool_used[64], memory_used[64], fail_pool, fail_allocate, fail_free;
static UINT pool_fail_after=UINT32_MAX,memory_fail_after=UINT32_MAX;
static _Alignas(PAGE_SIZE) UCHAR adapter_table[TG_GPU_ADAPTER_TABLE_BYTES];
static BOOLEAN adapter_table_used,adapter_table_fail_free;
static BOOLEAN fail_sync, in_sync;
static BOOLEAN fail_reserve;
static UINT interrupts, dpcs, queues, frees, expected_fence;
static TG_GPU_QUEUE_ENTRY *reuse_private;
static TG_GPU_CONTEXT *notify_context;
static UINT notify_references;
static KIRQL level;
static VOID NTAPI InitializeLock(PKSPIN_LOCK lock) { *lock=0; }
VOID (NTAPI * const __imp_KeInitializeSpinLock)(PKSPIN_LOCK)=InitializeLock;
static VOID NTAPI InitializeEvent(PRKEVENT event,EVENT_TYPE type,BOOLEAN state)
{ CHECK(event && type==SynchronizationEvent);event->Header.SignalState=state; }
static LONG NTAPI SetEvent(PRKEVENT event,KPRIORITY priority,BOOLEAN wait)
{ CHECK(event && !priority && !wait);event->Header.SignalState=1;return 0; }
VOID (NTAPI * const __imp_KeInitializeEvent)(PRKEVENT,EVENT_TYPE,BOOLEAN)=InitializeEvent;
LONG (NTAPI * const __imp_KeSetEvent)(PRKEVENT,KPRIORITY,BOOLEAN)=SetEvent;
static PVOID NTAPI Pool(POOL_FLAGS flags,SIZE_T bytes,ULONG tag)
{
    UINT i;
    CHECK(flags==POOL_FLAG_NON_PAGED && bytes<=PAGE_SIZE && tag!=0 && level==PASSIVE_LEVEL);
    if(fail_pool) return NULL;
    if(pool_fail_after!=UINT32_MAX) {
        if(!pool_fail_after) return NULL;
        --pool_fail_after;
    }
    for(i=0;i<64;++i) if(!pool_used[i]) {
        pool_used[i]=TRUE; memset(pools[i],0,PAGE_SIZE); return pools[i];
    }
    return NULL;
}
static VOID NTAPI FreePool(PVOID p,ULONG tag)
{
    UINT i; CHECK(tag!=0 && level==PASSIVE_LEVEL);
    for(i=0;i<64;++i) if(p==pools[i]) { CHECK(pool_used[i]);pool_used[i]=FALSE;return; }
    CHECK(FALSE);
}
PVOID (NTAPI * const __imp_ExAllocatePool2)(POOL_FLAGS,SIZE_T,ULONG)=Pool;
VOID (NTAPI * const __imp_ExFreePoolWithTag)(PVOID,ULONG)=FreePool;
static KIRQL NTAPI Lock(PKSPIN_LOCK lock)
{
    KIRQL previous=level;CHECK(!*lock && !in_sync);*lock=1;level=DISPATCH_LEVEL;return previous;
}
static VOID NTAPI Unlock(PKSPIN_LOCK lock,KIRQL previous)
{ CHECK(*lock==1 && level==DISPATCH_LEVEL);*lock=0;level=previous; }
KIRQL (NTAPI * const __imp_KeAcquireSpinLockRaiseToDpc)(PKSPIN_LOCK)=Lock;
VOID (NTAPI * const __imp_KeReleaseSpinLock)(PKSPIN_LOCK,KIRQL)=Unlock;
static PHYSICAL_ADDRESS NTAPI Physical(PVOID address)
{
    ULONG_PTR offset=(ULONG_PTR)address-(ULONG_PTR)memory;
    PHYSICAL_ADDRESS p;
    ULONG_PTR table_offset=(ULONG_PTR)address-(ULONG_PTR)adapter_table;
    if(table_offset<sizeof(adapter_table)){p.QuadPart=0x92000000+table_offset;return p;}
    CHECK(offset<sizeof(memory));p.QuadPart=0x91000000+offset;return p;
}
PHYSICAL_ADDRESS (NTAPI * const __imp_MmGetPhysicalAddress)(PVOID)=Physical;
static NTSTATUS NTAPI Allocate(HANDLE device,DXGKARGCB_ALLOCATECONTIGUOUSMEMORY *a)
{
    UINT i;CHECK(device==(HANDLE)1 && level==PASSIVE_LEVEL);
    CHECK((a->NumberOfBytes==PAGE_SIZE || a->NumberOfBytes==TG_GPU_COMMAND_BYTES ||
           a->NumberOfBytes==TG_GPU_TABLE_BYTES || a->NumberOfBytes==TG_GPU_ADAPTER_TABLE_BYTES) && a->CacheType==DXGK_MEMORY_CACHING_TYPE_NON_CACHED);
    CHECK(a->LowestAcceptableAddress.QuadPart==0x80000000 && a->HighestAcceptableAddress.QuadPart==0xffffffff);
    if(fail_allocate) return STATUS_INSUFFICIENT_RESOURCES;
    if(memory_fail_after!=UINT32_MAX){if(!memory_fail_after)return STATUS_INSUFFICIENT_RESOURCES;--memory_fail_after;}
    if(a->NumberOfBytes==TG_GPU_ADAPTER_TABLE_BYTES){
        CHECK(!adapter_table_used);adapter_table_used=TRUE;
        a->hMemoryHandle=(HANDLE)256;a->pMemory=adapter_table;return STATUS_SUCCESS;
    }
    for(i=0;i<64;++i) if(!memory_used[i]) {
        memory_used[i]=TRUE;a->hMemoryHandle=(HANDLE)(ULONG_PTR)(i+1);a->pMemory=memory[i];return STATUS_SUCCESS;
    }
    return STATUS_INSUFFICIENT_RESOURCES;
}
static NTSTATUS NTAPI Free(HANDLE device,const DXGKARGCB_FREECONTIGUOUSMEMORY *a)
{
    UINT i=(UINT)(ULONG_PTR)a->hMemoryHandle-1;
    if(a->hMemoryHandle==(HANDLE)256){
        CHECK(device==(HANDLE)1 && level==PASSIVE_LEVEL && adapter_table_used);
        if(fail_free || adapter_table_fail_free)return STATUS_UNSUCCESSFUL;
        adapter_table_used=FALSE;++frees;return STATUS_SUCCESS;
    }
    CHECK(device==(HANDLE)1 && level==PASSIVE_LEVEL && i<64 && memory_used[i]);
    if(fail_free) return STATUS_UNSUCCESSFUL;
    memory_used[i]=FALSE;++frees;return STATUS_SUCCESS;
}
static NTSTATUS NTAPI Synchronize(HANDLE device,PKSYNCHRONIZE_ROUTINE routine,
                                  PVOID context,ULONG message,PBOOLEAN result)
{
    CHECK(device==(HANDLE)1 && !message && level==PASSIVE_LEVEL);
    if(fail_sync) return STATUS_UNSUCCESSFUL;
    in_sync=TRUE;*result=routine(context);in_sync=FALSE;return STATUS_SUCCESS;
}
static VOID NTAPI Interrupt(HANDLE device,DXGKARGCB_NOTIFY_INTERRUPT_DATA *data)
{
    CHECK(device==(HANDLE)1 && in_sync && data->InterruptType==DXGK_INTERRUPT_DMA_COMPLETED);
    CHECK(data->DmaCompleted.NodeOrdinal==0 && data->DmaCompleted.EngineOrdinal==0);
    CHECK(data->DmaCompleted.SubmissionFenceId==expected_fence);++interrupts;
    if(notify_context) CHECK(notify_context->References==notify_references);
    if(reuse_private) {
        /* Model immediate OS reuse after fence notification. The worker must
         * not retain or dereference anything inside this buffer afterward.
         */
        memset(reuse_private,0xa5,sizeof(*reuse_private));reuse_private=NULL;
    }
}
static BOOLEAN NTAPI Queue(HANDLE device)
{ CHECK(device==(HANDLE)1 && in_sync);++queues;return FALSE; /* DPC already queued */ }
static VOID NTAPI Dpc(HANDLE device)
{ CHECK(device==(HANDLE)1 && !in_sync);++dpcs; }
static NTSTATUS NTAPI Reserve(HANDLE device,DXGKARGCB_RESERVEGPUVIRTUALADDRESSRANGE *a)
{
    CHECK(device==(HANDLE)1 && level==PASSIVE_LEVEL && a->hDxgkProcess==(HANDLE)100);
    CHECK(a->SizeInBytes==TG_GPU_INTERNAL_COVERAGE && a->Alignment==TG_GPU_INTERNAL_COVERAGE);
    CHECK(!a->Flags && !a->BaseAddress);
    if(fail_reserve) return STATUS_INSUFFICIENT_RESOURCES;
    a->StartVirtualAddress=TG_GPU_INTERNAL_COVERAGE;return STATUS_SUCCESS;
}

/* The allocation handle table below is a Dxgkrnl test substitute, not a KMD
 * table. Release can destroy the referenced object synchronously, as the OS
 * contract allows. Its callbacks also enforce APC-or-lower/no-spinlock use.
 */
static HANDLE os_allocation[8];
static UINT os_references[8],handle_acquires,handle_releases,destroy_on_release;
static TG_GPU_CONTEXT_ENGINE *release_engine;
static VOID *APIENTRY AcquireHandle(const DXGKARGCB_GETHANDLEDATA *a,DXGKARG_RELEASE_HANDLE *release)
{
    CHECK(a && release && level==PASSIVE_LEVEL && a->Type==DXGK_HANDLE_ALLOCATION && !a->Flags.Value);
    *release=NULL;
    if(!a->hObject || a->hObject>=8 || !os_allocation[a->hObject]) return NULL;
    ++os_references[a->hObject];++handle_acquires;
    *release=(HANDLE)(ULONG_PTR)a->hObject;return os_allocation[a->hObject];
}
static VOID APIENTRY ReleaseHandle(const DXGKARGCB_RELEASEHANDLEDATA a)
{
    UINT id=(UINT)(ULONG_PTR)a.ReleaseHandle;HANDLE allocation;
    DXGKARG_DESTROYALLOCATION destroy={.NumAllocations=1};
    CHECK(level==PASSIVE_LEVEL && a.Type==DXGK_HANDLE_ALLOCATION && id>0 && id<8 && os_references[id]);
    --os_references[id];++handle_releases;
    if(destroy_on_release==id && !os_references[id]) {
        allocation=os_allocation[id];destroy.pAllocationList=&allocation;
        CHECK(release_engine && NT_SUCCESS(TgGpuDdiDestroyAllocation(release_engine,&destroy)));
        os_allocation[id]=NULL;destroy_on_release=0;
    }
}
static UINT PoolsInUse(void)
{ UINT i,count=0;for(i=0;i<64;++i) count+=pool_used[i]!=FALSE;return count; }

static void TestAllocations(TG_GPU_CONTEXT_ENGINE *e,TG_GPU_CONTEXT_ENGINE *other,
                            DRIVER_INITIALIZATION_DATA *ddi,TG_GPU_DEVICE *device)
{
    struct tg_gpu_allocation_request desc[2]={
        {TG_GPU_ALLOCATION_ABI,TG_GPU_ALLOCATION_CPU_VISIBLE,4097},
        {TG_GPU_ALLOCATION_ABI,0,65536}
    };
    DXGK_ALLOCATIONINFO info[2]={
        {.pPrivateDriverData=&desc[0],.PrivateDriverDataSize=sizeof(desc[0])},
        {.pPrivateDriverData=&desc[1],.PrivateDriverDataSize=sizeof(desc[1])}
    };
    DXGKARG_CREATEALLOCATION create={.NumAllocations=2,.pAllocationInfo=info,.Flags.Resource=1};
    DXGKARG_DESTROYALLOCATION destroy={0};
    DXGK_OPENALLOCATIONINFO oi[2]={{.hAllocation=1},{.hAllocation=2}},oi2={.hAllocation=1};
    DXGKARG_OPENALLOCATION open={.NumAllocations=2,.pOpenAllocation=oi,.Flags.Create=1};
    DXGKARG_OPENALLOCATION open2={.NumAllocations=1,.pOpenAllocation=&oi2,.Flags.ReadOnly=1};
    DXGKARG_CLOSEALLOCATION close={0};
    DXGKARG_CREATEDEVICE second={.hDevice=(HANDLE)202,.hKmdProcess=device->Process};
    HANDLE resource,handles[2],opened[2],readonly;UINT before=PoolsInUse(),i;
    CHECK(NT_SUCCESS(ddi->DxgkDdiCreateDevice(e,&second)));
    ++before;
    desc[1].version=99;
    CHECK(ddi->DxgkDdiCreateAllocation(e,&create)==STATUS_GRAPHICS_DRIVER_MISMATCH);
    CHECK(!info[0].hAllocation && !info[1].hAllocation && !create.hResource && PoolsInUse()==before);
    desc[1].version=TG_GPU_ALLOCATION_ABI;
    desc[1].bytes=UINT64_MAX;
    CHECK(ddi->DxgkDdiCreateAllocation(e,&create)==STATUS_INVALID_PARAMETER);
    desc[1].bytes=65536;info[0].FlagsWddm2.CreateInVpr=1;
    CHECK(ddi->DxgkDdiCreateAllocation(e,&create)==STATUS_INVALID_PARAMETER);
    info[0].FlagsWddm2.Value=0x80000800;
    for(i=0;i<3;++i) {
        pool_fail_after=i;
        CHECK(ddi->DxgkDdiCreateAllocation(e,&create)==STATUS_NO_MEMORY);
        CHECK(!info[0].hAllocation && !info[1].hAllocation && !create.hResource);
        CHECK(!e->AllocationCount && !e->ResourceCount && !e->Creating && PoolsInUse()==before);
    }
    pool_fail_after=UINT32_MAX;
    CHECK(NT_SUCCESS(ddi->DxgkDdiCreateAllocation(e,&create)));
    resource=create.hResource;handles[0]=info[0].hAllocation;handles[1]=info[1].hAllocation;
    CHECK(resource && handles[0] && handles[1] && e->AllocationCount==2 && e->ResourceCount==1);
    CHECK(info[0].Size==8192 && info[1].Size==65536 && info[0].Alignment==PAGE_SIZE);
    CHECK(info[0].PreferredSegment.SegmentId0==2 && info[0].SupportedWriteSegmentSet==2);
    CHECK(info[0].FlagsWddm2.Value==0x80000881 && info[1].FlagsWddm2.Value==0x80);
    CHECK(!info[0].FlagsWddm2.AccessedPhysically && !info[0].FlagsWddm2.Cached &&
          !info[0].FlagsWddm2.ExplicitResidencyNotification && info[0].AllocationPriority==D3DDDI_ALLOCATIONPRIORITY_NORMAL);
    /* Appending failure must not free or alter the existing resource. */
    pool_fail_after=1;
    CHECK(ddi->DxgkDdiCreateAllocation(e,&create)==STATUS_NO_MEMORY);
    CHECK(create.hResource==resource && e->AllocationCount==2 && e->ResourceCount==1 && PoolsInUse()==before+3);
    pool_fail_after=UINT32_MAX;
    os_allocation[1]=handles[0];os_allocation[2]=handles[1];
    /* Each failure unwinds acquired OS references and all device-open objects. */
    pool_fail_after=1;
    CHECK(ddi->DxgkDdiOpenAllocation(device,&open)==STATUS_NO_MEMORY);
    CHECK(!oi[0].hDeviceSpecificAllocation && !oi[1].hDeviceSpecificAllocation);
    CHECK(!os_references[1] && !os_references[2] && !device->OpenAllocations);
    pool_fail_after=UINT32_MAX;oi[1].hAllocation=7;
    CHECK(ddi->DxgkDdiOpenAllocation(device,&open)==STATUS_INVALID_PARAMETER);
    CHECK(!os_references[1] && !device->OpenAllocations && PoolsInUse()==before+3);
    oi[1].hAllocation=2;
    CHECK(NT_SUCCESS(ddi->DxgkDdiOpenAllocation(device,&open)));
    opened[0]=oi[0].hDeviceSpecificAllocation;opened[1]=oi[1].hDeviceSpecificAllocation;
    CHECK(os_references[1]==1 && os_references[2]==1 && device->OpenAllocations==2);
    CHECK(NT_SUCCESS(ddi->DxgkDdiOpenAllocation(second.hDevice,&open2)));readonly=oi2.hDeviceSpecificAllocation;
    CHECK(os_references[1]==2);
    CHECK(TgGpuValidateOpenAllocationRange(device,opened[0],8191,1,TRUE)==STATUS_SUCCESS);
    CHECK(TgGpuValidateOpenAllocationRange(device,opened[0],8192,1,FALSE)==STATUS_INVALID_BUFFER_SIZE);
    CHECK(TgGpuValidateOpenAllocationRange(device,opened[0],UINT64_MAX,2,FALSE)==STATUS_INVALID_BUFFER_SIZE);
    CHECK(TgGpuValidateOpenAllocationRange(second.hDevice,readonly,0,4,TRUE)==STATUS_ACCESS_DENIED);
    CHECK(TgGpuValidateOpenAllocationRange(device,readonly,0,4,FALSE)==STATUS_INVALID_PARAMETER);
    CHECK(ddi->DxgkDdiDestroyDevice(device)==STATUS_DEVICE_BUSY);
    destroy.NumAllocations=2;destroy.pAllocationList=handles;destroy.hResource=resource;destroy.Flags.DestroyResource=1;
    CHECK(ddi->DxgkDdiDestroyAllocation(e,&destroy)==STATUS_DEVICE_BUSY);
    CHECK(ddi->DxgkDdiDestroyAllocation(other,&destroy)==STATUS_INVALID_PARAMETER);
    close.NumAllocations=2;close.pOpenHandleList=opened;
    CHECK(ddi->DxgkDdiCloseAllocation(second.hDevice,&close)==STATUS_INVALID_PARAMETER);
    opened[1]=opened[0];
    CHECK(ddi->DxgkDdiCloseAllocation(device,&close)==STATUS_INVALID_PARAMETER && device->OpenAllocations==2);
    opened[1]=oi[1].hDeviceSpecificAllocation;
    CHECK(NT_SUCCESS(ddi->DxgkDdiCloseAllocation(device,&close)));
    CHECK(!device->OpenAllocations && os_references[1]==1 && !os_references[2]);
    close.NumAllocations=1;close.pOpenHandleList=&readonly;
    CHECK(NT_SUCCESS(ddi->DxgkDdiCloseAllocation(second.hDevice,&close)) && !os_references[1]);
    /* A failed bulk destroy has no partial side effects. */
    handles[1]=handles[0];
    CHECK(ddi->DxgkDdiDestroyAllocation(e,&destroy)==STATUS_INVALID_PARAMETER && e->AllocationCount==2);
    handles[1]=os_allocation[2];destroy.NumAllocations=1;
    CHECK(ddi->DxgkDdiDestroyAllocation(e,&destroy)==STATUS_DEVICE_BUSY && e->AllocationCount==2);
    destroy.NumAllocations=2;
    CHECK(NT_SUCCESS(ddi->DxgkDdiDestroyAllocation(e,&destroy)));
    CHECK(!e->AllocationCount && !e->ResourceCount && PoolsInUse()==before);
    os_allocation[1]=os_allocation[2]=NULL;
    /* Model the owner requesting destruction while a second device still has
     * an open reference. The final ReleaseHandle destroys the allocation.
     */
    create.NumAllocations=1;create.hResource=NULL;create.Flags.Value=0;
    CHECK(NT_SUCCESS(ddi->DxgkDdiCreateAllocation(e,&create)));
    os_allocation[3]=info[0].hAllocation;oi2.hAllocation=3;
    CHECK(NT_SUCCESS(ddi->DxgkDdiOpenAllocation(second.hDevice,&open2)));
    readonly=oi2.hDeviceSpecificAllocation;destroy_on_release=3;release_engine=e;
    CHECK(NT_SUCCESS(ddi->DxgkDdiCloseAllocation(second.hDevice,&close)));
    CHECK(!destroy_on_release && !os_allocation[3] && !e->AllocationCount && !e->Creating);
    CHECK(handle_acquires==handle_releases);
    CHECK(NT_SUCCESS(ddi->DxgkDdiDestroyDevice(second.hDevice)));
    CHECK(PoolsInUse()==before-1);
}

/* Register-state substitute for the hot channel path, not a GPU emulator.
 * It never writes the completion word. It checks submission/teardown order
 * and can withhold preemption or reset acknowledgement to exercise lifetime.
 */
static struct {
    UINT32 mc,pb,map,sched,inst,ctrl,preempt,pbdma,runlist,runbase,root,expected_root;
    UINT32 clamp,reset,clocks,out5,mc_flush;
    UINT tlbs,flushes,reads,writes;
    UINT32 fifo_fault;
    UINT32 userd_put;
    UINT doorbells;
    UCHAR enable,voltage;
    BOOLEAN fail_preempt,fail_reset,fail_tlb;
} hw={.mc=0x100,.pb=1,.map=1,.sched=1,.enable=0x41,.voltage=110,.expected_root=0x900000};
static UINT32 Read(void *context,UINT32 address)
{
    CHECK(context==&hw && level==PASSIVE_LEVEL);++hw.reads;
    switch(address) {
    case 0x57000200:return hw.mc;
    case 0x57000204:return hw.pb;
    case 0x57002390:return hw.map;
    case 0x57001704:return 0x80090000;
    case 0x57002254:return 0x10000013;
    case 0x5801308c:return hw.userd_put;
    case 0x57002630:return hw.sched;
    case 0x57800000:return hw.inst;
    case 0x57800004:return hw.ctrl;
    case 0x57002270:return hw.runbase;
    case 0x57002274:return hw.runlist;
    case 0x57002284:return 0;
    case 0x57002634:return hw.preempt;
    case 0x57003080:return hw.pbdma;
    case 0x57100c80:return hw.fail_tlb ? 0x10000 : 0x18000;
    case 0x57070000:return 0;
    case 0x57002100:return hw.fifo_fault;
    case 0x5700259c:case 0x57040108:case 0x57040148:
    case 0x57001100:case 0x57120058:case 0x5712005c:return 0;
    case 0x7000e6d4:return hw.clamp;
    case 0x6000628c:return hw.reset;
    case 0x60006280:return hw.clocks;
    case 0x6000667c:return hw.out5;
    case 0x70019970:return hw.mc_flush;
    case 0x70019974:return hw.mc_flush&4;
    default:CHECK(FALSE);return UINT32_MAX;
    }
}
static void Write(void *context,UINT32 address,UINT32 value)
{
    CHECK(context==&hw && level==PASSIVE_LEVEL);++hw.writes;
    switch(address) {
    case 0x5801308c:
        CHECK(hw.inst && hw.ctrl && hw.runlist==2 && !hw.sched && value==2);
        hw.userd_put=value;++hw.doorbells;break;
    case 0x57000200:hw.mc=value;break;
    case 0x57000204:hw.pb=value;break;
    case 0x57002630:hw.sched=value;break;
    case 0x57800000:
        CHECK(hw.sched&1);
        if(value) CHECK(hw.tlbs && hw.root==hw.expected_root);
        else CHECK(!hw.runlist && !hw.pbdma && !hw.preempt);
        hw.inst=value;break;
    case 0x57800004:
        if(value&0x400) { CHECK(hw.inst&0x80000000);hw.ctrl=1; }
        if(value&0x800) hw.ctrl=0;
        break;
    case 0x57002270:hw.runbase=value;break;
    case 0x57002274:
        CHECK(hw.sched&1);
        if(value) {
            CHECK(value==2 && hw.ctrl && hw.inst && hw.runbase==0x00091003);
            hw.pbdma=0x3000; /* valid TSG zero */
        } else CHECK(!hw.ctrl && !hw.preempt);
        hw.runlist=value;break;
    case 0x57002634:
        CHECK(value==0x01000000 && !hw.ctrl && (hw.sched&1));
        hw.preempt=hw.fail_preempt ? 0x100000 : 0;
        if(!hw.preempt) hw.pbdma=0;
        break;
    case 0x57100cb8:hw.root=value;break;
    case 0x57100cbc:CHECK(value==0x80000001);++hw.tlbs;break;
    case 0x57070000:CHECK(value==1 && !hw.inst);++hw.flushes;break;
    case 0x7000e6d4:hw.clamp=value;break;
    case 0x60006290:if(!hw.fail_reset) hw.reset|=value;break;
    case 0x60006288:hw.clocks&=~value;break;
    case 0x60006284:hw.clocks|=value;break;
    case 0x6000667c:hw.out5=value;break;
    case 0x70019970:hw.mc_flush=value;break;
    default:CHECK(FALSE);
    }
}
static void Delay(void *context,UINT32 us)
{ CHECK(context==&hw && level==PASSIVE_LEVEL && us); }
static bool ReadPmic(void *context,UINT8 address,UINT8 reg,UINT8 *value)
{
    CHECK(context==&hw && level==PASSIVE_LEVEL && address==0x33 && (reg==6 || reg==0x23));
    *value=reg==6 ? hw.enable : hw.voltage;return true;
}
static bool WritePmic(void *context,UINT8 address,UINT8 reg,UINT8 value)
{
    CHECK(context==&hw && level==PASSIVE_LEVEL && address==0x33 && (reg==6 || reg==0x23));
    if(reg==6) hw.enable=value;else hw.voltage=value;return true;
}

#include "resource_fixture.h"

static __declspec(noinline) void TestAdapterQueries(TG_GPU_CONTEXT_ENGINE *e,DRIVER_INITIALIZATION_DATA *d)
{
    DXGKARG_QUERYADAPTERINFO a={0};
    DXGK_QUERYSEGMENTIN4 input={0};
    DXGK_QUERYSEGMENTOUT4 segment,before;
    struct { DXGK_SEGMENTDESCRIPTOR4 desc;UCHAR extension[32]; } output;
    DXGK_GPUMMUCAPS mmu;
    DXGK_QUERYPAGETABLELEVELDESCIN level={0};
    DXGK_PAGE_TABLE_LEVEL_DESC desc;
    DXGK_PHYSICALADAPTERCAPS physical;
    UINT i;UINT64 address;
    CHECK(d->DxgkDdiQueryAdapterInfo==TgGpuDdiQueryAdapterInfo);
    a.Type=DXGKQAITYPE_QUERYSEGMENT4;a.pInputData=&input;a.InputDataSize=sizeof(input);
    a.pOutputData=&segment;a.OutputDataSize=sizeof(segment);
    memset(&segment,0xa5,sizeof(segment));segment.NbSegment=0;
    before=segment;
    CHECK(NT_SUCCESS(d->DxgkDdiQueryAdapterInfo(e,&a)) && segment.NbSegment==1);
    /* Count query must not consume its poison descriptor pointer/stride or
     * write any other member (including padding).
     */
    for(i=sizeof(UINT);i<sizeof(segment);++i)
        CHECK(((UCHAR *)&segment)[i]==((UCHAR *)&before)[i]);
    memset(&output,0xa5,sizeof(output));segment.pSegmentDescriptor=(BYTE *)&output;
    segment.SegmentDescriptorStride=sizeof(output);
    CHECK(NT_SUCCESS(d->DxgkDdiQueryAdapterInfo(e,&a)));
    CHECK(segment.NbSegment==1 && segment.SegmentDescriptorStride==sizeof(output));
    CHECK(segment.PagingBufferSegmentId==0 && segment.PagingBufferSize==TG_GPU_DMA_BUFFER_BYTES);
    CHECK(segment.PagingBufferPrivateDataSize==sizeof(TG_GPU_QUEUE_ENTRY));
    CHECK(output.desc.BaseAddress.QuadPart==e->Tables->Memory.SystemPhysicalAddress.QuadPart);
    CHECK(output.desc.Size==e->Tables->Memory.Bytes && output.desc.Flags.CpuVisible);
    CHECK(output.desc.CpuTranslatedAddress.QuadPart==output.desc.BaseAddress.QuadPart);
    CHECK(output.desc.Flags.PopulatedFromSystemMemory && !output.desc.Flags.Aperture &&
          !output.desc.Flags.CacheCoherent && !output.desc.Flags.PreservedDuringStandby &&
          !output.desc.Flags.PreservedDuringHibernate && !output.desc.Flags.DirectFlip);
    for(i=0;i<sizeof(output.extension);++i) CHECK(output.extension[i]==0xa5);
    CHECK(NT_SUCCESS(TgGpuResolveTableSegment((PVOID)e->Tables,1,0,TG_GPU_TABLE_BYTES,&address)));
    CHECK(address==(UINT64)output.desc.BaseAddress.QuadPart);
    segment.SegmentDescriptorStride=sizeof(output.desc)-1;
    CHECK(d->DxgkDdiQueryAdapterInfo(e,&a)==STATUS_INVALID_PARAMETER);
    segment.SegmentDescriptorStride=sizeof(output);input.PhysicalAdapterIndex=1;
    CHECK(d->DxgkDdiQueryAdapterInfo(e,&a)==STATUS_INVALID_PARAMETER);
    input.PhysicalAdapterIndex=0;a.OutputDataSize=sizeof(segment)-1;
    CHECK(d->DxgkDdiQueryAdapterInfo(e,&a)==STATUS_BUFFER_TOO_SMALL);
    a.OutputDataSize=sizeof(segment);a.pOutputData=NULL;
    CHECK(d->DxgkDdiQueryAdapterInfo(e,&a)==STATUS_INVALID_PARAMETER);

    a.Type=DXGKQAITYPE_GPUMMUCAPS;a.pOutputData=&mmu;a.OutputDataSize=sizeof(mmu);
    memset(&mmu,0xa5,sizeof(mmu));
    CHECK(NT_SUCCESS(d->DxgkDdiQueryAdapterInfo(e,&a)));
    CHECK(mmu.Value==0x21 && mmu.VirtualAddressBitCount==40 && mmu.PageTableLevelCount==2);
    CHECK(mmu.PageTableUpdateMode==DXGK_PAGETABLEUPDATE_GPU_PHYSICAL);
    CHECK(!mmu.LeafPageTableSizeFor64KPagesInBytes && !mmu.LegacyBehaviors.SourcePageTableVaInTransfer);
    a.InputDataSize=sizeof(input)-1;
    CHECK(d->DxgkDdiQueryAdapterInfo(e,&a)==STATUS_BUFFER_TOO_SMALL);
    a.InputDataSize=sizeof(input);input.PhysicalAdapterIndex=1;
    CHECK(d->DxgkDdiQueryAdapterInfo(e,&a)==STATUS_INVALID_PARAMETER);
    input.PhysicalAdapterIndex=0;
    a.Type=DXGKQAITYPE_PAGETABLELEVELDESC;a.pInputData=&level;a.InputDataSize=sizeof(level);
    a.pOutputData=&desc;a.OutputDataSize=sizeof(desc);
    for(level.LevelIndex=0;level.LevelIndex<mmu.PageTableLevelCount;++level.LevelIndex) {
        CHECK(NT_SUCCESS(d->DxgkDdiQueryAdapterInfo(e,&a)));
        CHECK(desc.PageTableIndexBitCount==14 && desc.PageTableSegmentId==1);
        CHECK(desc.PagingProcessPageTableSegmentId==1 && desc.PageTableSizeInBytes==TG_GPU_TABLE_BYTES);
        CHECK(desc.PageTableAlignmentInBytes==PAGE_SIZE);
    }
    CHECK(d->DxgkDdiQueryAdapterInfo(e,&a)==STATUS_INVALID_PARAMETER);
    level.LevelIndex=0;level.PhysicalAdapterIndex=1;
    CHECK(d->DxgkDdiQueryAdapterInfo(e,&a)==STATUS_INVALID_PARAMETER);
    a.Type=DXGKQAITYPE_PHYSICALADAPTERCAPS;a.pInputData=&input;a.InputDataSize=sizeof(input);
    a.pOutputData=&physical;a.OutputDataSize=sizeof(physical);
    memset(&physical,0xa5,sizeof(physical));
    CHECK(NT_SUCCESS(d->DxgkDdiQueryAdapterInfo(e,&a)));
    CHECK(physical.NumExecutionNodes==1 && physical.PagingNodeIndex==0);
    CHECK(physical.DxgkPhysicalAdapterHandle==e->Dxgk.DeviceHandle && physical.Flags.Value==2);
    CHECK(!physical.VPRPagingNode);
    input.PhysicalAdapterIndex=1;
    CHECK(d->DxgkDdiQueryAdapterInfo(e,&a)==STATUS_INVALID_PARAMETER);
    input.PhysicalAdapterIndex=0;e->Stopping=TRUE;
    CHECK(d->DxgkDdiQueryAdapterInfo(e,&a)==STATUS_DEVICE_NOT_READY);e->Stopping=FALSE;
    a.Flags.Value=1;CHECK(d->DxgkDdiQueryAdapterInfo(e,&a)==STATUS_NOT_SUPPORTED);a.Flags.Value=0;
    a.Type=DXGKQAITYPE_DRIVERCAPS;
    memset(&physical,0xa5,sizeof(physical));
    CHECK(d->DxgkDdiQueryAdapterInfo(e,&a)==STATUS_NOT_SUPPORTED);
    for(i=0;i<sizeof(physical);++i) CHECK(((UCHAR *)&physical)[i]==0xa5);
    struct {struct tg_gpu_adapter_info info;UCHAR guard[16];} abi;
    struct tg_gpu_adapter_info request={.version=TG_GPU_ADAPTER_ABI,.size=sizeof(request)};
    memset(&abi,0xa5,sizeof(abi));
    a.Type=DXGKQAITYPE_UMDRIVERPRIVATE;a.pInputData=&request;a.InputDataSize=sizeof(request);
    a.pOutputData=&abi;a.OutputDataSize=sizeof(abi);
    CHECK(NT_SUCCESS(d->DxgkDdiQueryAdapterInfo(e,&a)));
    CHECK(abi.info.version==1 && abi.info.size==48 && !abi.info.reserved);
    CHECK(abi.info.memory_budget_bytes==UINT64_C(536870912));
    CHECK(abi.info.vendor==0x10de && abi.info.chipset==0x12e && abi.info.sm==53 && abi.info.gr_class==0xb197);
    CHECK(abi.info.flags==(e->AdapterIdentityReady?TG_GPU_ADAPTER_LUID_VALID:0u));
    CHECK(!(abi.info.flags&TG_GPU_ADAPTER_NATIVE_SUBMIT));
    CHECK(abi.info.luid_low==(e->AdapterIdentityReady?e->AdapterLuid.LowPart:0u));
    CHECK(abi.info.luid_high==(e->AdapterIdentityReady?e->AdapterLuid.HighPart:0));
    for(i=0;i<sizeof(abi.guard);++i)CHECK(abi.guard[i]==0xa5);
    a.OutputDataSize=sizeof(abi.info)-1;
    CHECK(d->DxgkDdiQueryAdapterInfo(e,&a)==STATUS_BUFFER_TOO_SMALL);
    a.OutputDataSize=sizeof(abi);a.InputDataSize=sizeof(request)-1;
    CHECK(d->DxgkDdiQueryAdapterInfo(e,&a)==STATUS_BUFFER_TOO_SMALL);
    a.InputDataSize=sizeof(request);++request.version;
    CHECK(d->DxgkDdiQueryAdapterInfo(e,&a)==STATUS_GRAPHICS_DRIVER_MISMATCH);
    --request.version;--request.size;
    CHECK(d->DxgkDdiQueryAdapterInfo(e,&a)==STATUS_GRAPHICS_DRIVER_MISMATCH);
    ++request.size;
    /* Actual KMT in/out shape: the request header aliases the reply. */
    abi.info=request;a.pInputData=&abi.info;
    CHECK(NT_SUCCESS(d->DxgkDdiQueryAdapterInfo(e,&a)));
    CHECK(abi.info.memory_budget_bytes==UINT64_C(536870912));
    CHECK(d->DxgkDdiQueryAdapterInfo(NULL,&a)==STATUS_INVALID_PARAMETER);
    CHECK(d->DxgkDdiQueryAdapterInfo(e,NULL)==STATUS_INVALID_PARAMETER);
}

static __declspec(noinline) void TestAdapterLifecycle(const DXGKRNL_INTERFACE *callbacks)
{
    DXGKRNL_INTERFACE dxgk=*callbacks;DRIVER_INITIALIZATION_DATA d={0};
    DXGK_START_INFO start={.RequiredDmaQueueEntry=32,.AdapterLuid={0x12345678,-3}};
    TG_GPU_ADAPTER *a=NULL;ULONG sources=5,children=6;UINT before=hw.reads+hw.writes;
    dxgk.Size=sizeof(dxgk);dxgk.DxgkCbGetDeviceInformation=ResourceInfo;
    dxgk.DxgkCbMapMemory=ResourceMap;dxgk.DxgkCbUnmapMemory=ResourceUnmap;
    TgGpuRegisterAdapterDdis(&d);
    CHECK(d.DxgkDdiAddDevice==TgGpuDdiAddDevice && d.DxgkDdiStartDevice==TgGpuDdiStartDevice);
    CHECK(d.DxgkDdiStopDevice==TgGpuDdiStopDevice && d.DxgkDdiRemoveDevice==TgGpuDdiRemoveDevice);
    CHECK(d.DxgkDdiSubmitCommandVirtual==NULL && d.DxgkDdiInterruptRoutine==NULL);
    CHECK(d.DxgkDdiAddDevice(NULL,(PVOID *)&a)==STATUS_INVALID_PARAMETER && !a);
    fail_pool=TRUE;
    CHECK(d.DxgkDdiAddDevice((PDEVICE_OBJECT)1,(PVOID *)&a)==STATUS_NO_MEMORY && !a);
    fail_pool=FALSE;
    CHECK(NT_SUCCESS(d.DxgkDdiAddDevice((PDEVICE_OBJECT)1,(PVOID *)&a)));
    CHECK(a && a->PhysicalDevice==(PDEVICE_OBJECT)1 && a->State==TgGpuAdapterAdded);
    CHECK(NT_SUCCESS(d.DxgkDdiStopDevice(a)));
    --dxgk.Size;
    CHECK(d.DxgkDdiStartDevice(a,&start,&dxgk,&sources,&children)==STATUS_INVALID_PARAMETER);
    ++dxgk.Size;
    CHECK(sources==5 && children==6 && a->State==TgGpuAdapterAdded);
    ResourceList(FALSE); /* Native completion requires the declared LIC resource. */
    CHECK(d.DxgkDdiStartDevice(a,&start,&dxgk,&sources,&children)==STATUS_DEVICE_CONFIGURATION_ERROR);
    CHECK(!sources && !children && !resource_live && a->State==TgGpuAdapterAdded);
    ResourceList(TRUE);resource_fail_map=4;resource_fail_unmap=2;
    CHECK(d.DxgkDdiStartDevice(a,&start,&dxgk,&sources,&children)==STATUS_UNSUCCESSFUL);
    CHECK(resource_live==2 && a->State==TgGpuAdapterStopping);
    CHECK(d.DxgkDdiRemoveDevice(a)==STATUS_DEVICE_BUSY);
    CHECK(d.DxgkDdiStartDevice(a,&start,&dxgk,&sources,&children)==STATUS_INVALID_DEVICE_STATE);
    resource_fail_unmap=0;CHECK(NT_SUCCESS(d.DxgkDdiStopDevice(a)) && !resource_live);
    for(UINT n=0;n<2;++n) {
        ResourceList(TRUE);memory_fail_after=n;
        CHECK(d.DxgkDdiStartDevice(a,&start,&dxgk,&sources,&children)==STATUS_INSUFFICIENT_RESOURCES);
        CHECK(a->State==TgGpuAdapterAdded && !adapter_table_used && !resource_live);
    }
    memory_fail_after=UINT32_MAX;
    ResourceList(TRUE);CHECK(NT_SUCCESS(d.DxgkDdiStartDevice(a,&start,&dxgk,&sources,&children)));
    CHECK(!sources && !children && a->State==TgGpuAdapterReady && a->EngineReady && a->QueueReady);
    CHECK(a->Engine.Tables==&a->Tables && a->Queue.Engine==&a->Engine && a->Engine.QueueOwner==&a->Queue);
    CHECK(a->Tables.Memory.Bytes==2u*1024*1024 && adapter_table_used);
    CHECK(a->Engine.AdapterIdentityReady && a->Engine.AdapterLuid.LowPart==0x12345678);
    TestAdapterQueries(&a->Engine,&d);
    CHECK(d.DxgkDdiRemoveDevice(a)==STATUS_DEVICE_BUSY);
    CHECK(d.DxgkDdiStartDevice(a,&start,&dxgk,&sources,&children)==STATUS_INVALID_DEVICE_STATE);
    DXGKARG_CREATEDEVICE device={.hDevice=(HANDLE)500};device.Flags.SystemDevice=1;
    CHECK(NT_SUCCESS(d.DxgkDdiCreateDevice(a,&device)));
    CHECK(d.DxgkDdiStopDevice(a)==STATUS_DEVICE_BUSY && a->EngineReady && !a->QueueReady);
    CHECK(a->Engine.Stopping && !a->Engine.AdapterIdentityReady && resource_live && adapter_table_used);
    CHECK(NT_SUCCESS(d.DxgkDdiDestroyDevice(device.hDevice)));
    fail_free=TRUE;CHECK(d.DxgkDdiStopDevice(a)==STATUS_UNSUCCESSFUL && a->Engine.Commands.Handle);
    fail_free=FALSE;adapter_table_fail_free=TRUE;
    CHECK(d.DxgkDdiStopDevice(a)==STATUS_UNSUCCESSFUL && !a->EngineReady && adapter_table_used);
    CHECK(resource_live && a->State==TgGpuAdapterStopping);
    adapter_table_fail_free=FALSE;resource_fail_unmap=5;
    CHECK(d.DxgkDdiStopDevice(a)==STATUS_UNSUCCESSFUL && resource_live==16 && !adapter_table_used);
    resource_fail_unmap=0;
    CHECK(NT_SUCCESS(d.DxgkDdiStopDevice(a)) && !resource_live && a->State==TgGpuAdapterAdded);
    ResourceList(TRUE);CHECK(NT_SUCCESS(d.DxgkDdiStartDevice(a,&start,&dxgk,&sources,&children)));
    CHECK(NT_SUCCESS(d.DxgkDdiStopDevice(a)));
    CHECK(NT_SUCCESS(d.DxgkDdiRemoveDevice(a)));
    CHECK(hw.reads+hw.writes==before); /* This module never touches hardware. */
    for(UINT i=0;i<64;++i)CHECK(!pool_used[i] && !memory_used[i]);
    CHECK(!adapter_table_used && !resource_live);
}

void start(void)
{
    static TG_GPU_CONTEXT_ENGINE e,other;
    static TG_GPU_WORK_QUEUE queue;
    static TG_GPU_QUEUE_ENTRY entries[4];
    static _Alignas(PAGE_SIZE) UCHAR table[TG_GPU_TABLE_BYTES];
    TG_GPU_TABLE_SEGMENT tables={.Memory={(HANDLE)4,table,{.QuadPart=0x90000000},sizeof(table)},.SegmentId=1};
    DXGKRNL_INTERFACE dxgk={0};DRIVER_INITIALIZATION_DATA ddi={0};
    DXGKARG_CREATEPROCESS process={.hDxgkProcess=(HANDLE)100},failed={0};
    DXGKARG_CREATEDEVICE device={.hDevice=(HANDLE)101};
    DXGKARG_CREATECONTEXT create={.hContext=(HANDLE)102,.EngineAffinity=1};
    DXGKARG_SETROOTPAGETABLE root={.Address={1,0,0},.NumEntries=2};
    DXGKARG_SUBMITCOMMANDVIRTUAL submit={.DmaBufferVirtualAddress=0x10000,.DmaBufferSize=40,.SubmissionFenceId=7};
    DXGKARG_QUERYCURRENTFENCE query={0};
    TG_GPU_CONTEXT *c,*c2; TG_GPU_CONTEXT_WORK work={0},blocked={0};
    struct tg_gpu_power power={.owned=true,.ready=true,.stage=TG_GPU_STAGE_READY,
        .pmic_address=0x33,.original_enable=0x40,.original_voltage=50};
    struct tg_gpu_clock clock={0};
    struct tg_gpu_power_io io={.context=&hw,.read32=Read,.write32=Write,.delay_us=Delay,
        .read_pmic=ReadPmic,.write_pmic=WritePmic};
    UINT32 *instance,*fence;UINT i,before;ULONG written;UINT64 pde;
    struct tg_gpu_channel rejected={0};
    static const char pass[]="PASS: WDK adapter lifecycle/LUID/512MiB ABI, BAR1 notifications, allocation rollback, ordered queue and retirement (controlled substitutes; no live WDDM or GPU pass)\r\n";
    hw.fifo_fault=0x100;before=hw.writes;
    CHECK(tg_gpu_channel_submit(&io,&power,0x90000000,0x91004000,0x91003000,0x13000,&e,&rejected)==TG_GPU_CHANNEL_FAULT);
    CHECK(!rejected.referenced && hw.writes==before && rejected.fifo_interrupt==0x100);
    hw.fifo_fault=0;
    dxgk.Version=DXGKDDI_INTERFACE_VERSION_WDDM2_4;dxgk.DeviceHandle=(HANDLE)1;
    dxgk.DxgkCbAllocateContiguousMemory=Allocate;dxgk.DxgkCbFreeContiguousMemory=Free;
    dxgk.DxgkCbSynchronizeExecution=Synchronize;dxgk.DxgkCbNotifyInterrupt=Interrupt;
    dxgk.DxgkCbQueueDpc=Queue;dxgk.DxgkCbNotifyDpc=Dpc;
    dxgk.DxgkCbReserveGpuVirtualAddressRange=Reserve;
    dxgk.DxgkCbAcquireHandleData=AcquireHandle;dxgk.DxgkCbReleaseHandleData=ReleaseHandle;
    CHECK(NT_SUCCESS(TgGpuInitializeContextEngine(&e,&dxgk,&tables)));
    CHECK(NT_SUCCESS(TgGpuInitializeContextEngine(&other,&dxgk,&tables)));
    TgGpuRegisterContextDdis(&ddi);
    TestAdapterQueries(&e,&ddi);
    fail_pool=TRUE;CHECK(ddi.DxgkDdiCreateProcess(&e,&failed)==STATUS_NO_MEMORY && !failed.hKmdProcess);fail_pool=FALSE;
    fail_reserve=TRUE;CHECK(ddi.DxgkDdiCreateProcess(&e,&process)==STATUS_INSUFFICIENT_RESOURCES);fail_reserve=FALSE;
    CHECK(NT_SUCCESS(ddi.DxgkDdiCreateProcess(&e,&process)));
    CHECK(((TG_GPU_PROCESS *)process.hKmdProcess)->Internal.Base==TG_GPU_INTERNAL_COVERAGE);
    {
        UINT64 *pt=((TG_GPU_PROCESS *)process.hKmdProcess)->Internal.Leaf.CpuAddress;
        CHECK(pt[0]==UINT64_C(0x100910007) && pt[1]==UINT64_C(0x100910017));
        CHECK(pt[2]==UINT64_C(0x100910023) && pt[3]==0 && pt[TG_GPU_TABLE_ENTRIES-1]==0);
    }
    device.hKmdProcess=process.hKmdProcess;
    CHECK(ddi.DxgkDdiCreateDevice(&other,&device)==STATUS_INVALID_PARAMETER);
    CHECK(NT_SUCCESS(ddi.DxgkDdiCreateDevice(&e,&device)));
    TestAllocations(&e,&other,&ddi,device.hDevice);
    CHECK(ddi.DxgkDdiDestroyProcess(&e,process.hKmdProcess)==STATUS_DEVICE_BUSY);
    create.Flags.VirtualAddressing=1;
    fail_allocate=TRUE;
    CHECK(ddi.DxgkDdiCreateContext(device.hDevice,&create)==STATUS_INSUFFICIENT_RESOURCES);
    CHECK(((TG_GPU_DEVICE *)device.hDevice)->Contexts==0);fail_allocate=FALSE;
    CHECK(NT_SUCCESS(ddi.DxgkDdiCreateContext(device.hDevice,&create)));c=create.hContext;
    CHECK(create.ContextInfo.Caps.NoPatchingRequired && !create.ContextInfo.Caps.DriverManagesResidency);
    CHECK(create.ContextInfo.DmaBufferPrivateDataSize==sizeof(TG_GPU_QUEUE_ENTRY));
    create.hContext=(HANDLE)103;CHECK(NT_SUCCESS(ddi.DxgkDdiCreateContext(device.hDevice,&create)));c2=create.hContext;
    CHECK(c!=c2 && ((TG_GPU_DEVICE *)device.hDevice)->Contexts==2);
    CHECK(ddi.DxgkDdiDestroyDevice(device.hDevice)==STATUS_DEVICE_BUSY);
    root.hContext=c;ddi.DxgkDdiSetRootPageTable(&e,&root);
    instance=c->Instance.CpuAddress;
    fence=(UINT32 *)((UCHAR *)e.Commands.CpuAddress+TG_GPU_COMMAND_FENCE);
    CHECK(c->Root.Valid && instance[128]==0x90000804 && instance[130]==0x07fff000);
    submit.hContext=c;CHECK(NT_SUCCESS(TgGpuAcquireContextWork(&e,&submit,&work)));
    CHECK(work.Root.Valid && c->References==1 && work.FencePhysicalAddress==0x91002800);
    CHECK(*fence==~work.Expected && instance[0x800/4]==0);
    CHECK(TgGpuSignalContextWork(&e,&work)==STATUS_DEVICE_NOT_READY);
    ((UINT64 *)table)[1]=0; /* VidMm made the reserved root entry invalid */
    CHECK(NT_SUCCESS(TgGpuPrepareContextWork(&e,&work)));
    CHECK(tg_gpu_pde_small((UINT64)((TG_GPU_PROCESS *)process.hKmdProcess)->Internal.Leaf.SystemPhysicalAddress.QuadPart,&pde));
    CHECK(((UINT64 *)table)[1]==pde && ((UINT64 *)table)[0]==0);
    CHECK(work.Prepared && instance[128]==0x90000804 && instance[130]==0x07fff000);
    CHECK(TgGpuPrepareContextWork(&e,&work)==STATUS_INVALID_DEVICE_STATE);
    CHECK(TgGpuAcquireContextWork(&e,&submit,&blocked)==STATUS_DEVICE_BUSY && !blocked.Context);
    CHECK(ddi.DxgkDdiDestroyContext(c)==STATUS_DEVICE_BUSY);
    CHECK(TgGpuSignalContextWork(&e,&work)==STATUS_DEVICE_NOT_READY && !interrupts);
    before=hw.reads+hw.writes;
    CHECK(TgGpuSubmitContextWork(&e,&work,&io,&power)==STATUS_DEVICE_NOT_READY);
    CHECK(!work.Attempted && !e.Channel.referenced && hw.reads+hw.writes==before);
    e.UserdBar1Offset=0x13000;e.UserdBar1Ready=TRUE; /* explicit cold-backend substitute */
    hw.fifo_fault=0x40000000; /* normal runlist event cannot reject the channel */
    CHECK(NT_SUCCESS(TgGpuSubmitContextWork(&e,&work,&io,&power)));
    hw.fifo_fault=0;
    CHECK(work.Submitted && e.Channel.referenced && !hw.sched && hw.runlist==2);
    CHECK(e.Channel.bound && e.Channel.submitted && hw.userd_put==2 && hw.doorbells==1);
    CHECK(hw.inst==((UINT32)((UINT64)c->Instance.SystemPhysicalAddress.QuadPart>>12)|0x80000000));
    before=hw.writes;
    CHECK(TgGpuSubmitContextWork(&e,&work,&io,&power)==STATUS_INVALID_DEVICE_STATE && hw.writes==before);
    CHECK(TgGpuContextWorkFinished(&e,&work)==STATUS_PENDING && interrupts==0);
    /* A busy root change invalidates future admission but does not rewrite
     * the queued snapshot or RAMIN while the hardware may still reference it.
     */
    root.NumEntries=3;ddi.DxgkDdiSetRootPageTable(&e,&root);
    CHECK(!c->Root.Valid && c->RootStatus==STATUS_GRAPHICS_ALLOCATION_BUSY);
    CHECK(work.Root.Entries==2 && instance[130]==0x07fff000);
    *fence=work.Expected; /* explicit simulated GPU write */
    CHECK(TgGpuContextWorkFinished(&e,&work)==STATUS_SUCCESS);
    CHECK(TgGpuSignalContextWork(&e,&work)==STATUS_DEVICE_NOT_READY);
    hw.fail_preempt=TRUE;
    CHECK(TgGpuQuiesceContextWork(&e,&work,&io,&power)==STATUS_IO_TIMEOUT);
    CHECK(e.Channel.referenced && hw.inst && hw.runlist && c->References==1 && !interrupts);
    CHECK(ddi.DxgkDdiDestroyContext(c)==STATUS_DEVICE_BUSY);
    hw.fail_preempt=FALSE;
    CHECK(NT_SUCCESS(TgGpuQuiesceContextWork(&e,&work,&io,&power)) && work.Retired && c->References==1);
    CHECK(!e.Channel.referenced && !hw.inst && !hw.runlist && hw.flushes==2 && hw.tlbs==2);
    fail_sync=TRUE;CHECK(TgGpuSignalContextWork(&e,&work)==STATUS_UNSUCCESSFUL && !work.Notified);
    CHECK(!interrupts && !c->References && !work.Context && e.Notification==&work);
    CHECK(TgGpuInitializeWorkQueue(&queue,&e)==STATUS_DEVICE_BUSY && !e.QueueOwner);
    CHECK(TgGpuCaptureContextWork(&e,&submit,&work)==STATUS_DEVICE_BUSY);
    fail_sync=FALSE;expected_fence=7;notify_context=c;notify_references=0;
    CHECK(NT_SUCCESS(TgGpuSignalContextWork(&e,&work)) && work.Notified);
    CHECK(interrupts==1 && queues==1 && !c->References && !e.Notification);
    CHECK(NT_SUCCESS(TgGpuSignalContextWork(&e,&work)) && interrupts==1);
    ddi.DxgkDdiDpcRoutine(&e);CHECK(dpcs==1);
    CHECK(NT_SUCCESS(ddi.DxgkDdiQueryCurrentFence(&e,&query)) && query.CurrentFence==7);
    CHECK(NT_SUCCESS(TgGpuRetireContextWork(&e,&work,&io,&power)) && !work.Context && !c->References);
    CHECK(!e.Channel.referenced && !hw.inst && !hw.runlist && hw.flushes==2 && hw.tlbs==2);
    CHECK(TgGpuAcquireContextWork(&e,&submit,&work)==STATUS_INVALID_PARAMETER);
    ddi.DxgkDdiSetRootPageTable(&e,&root);CHECK(c->Root.Valid && instance[130]==0x0bfff000);
    root.Address.SegmentOffset=sizeof(table)-PAGE_SIZE;root.NumEntries=TG_GPU_TABLE_ENTRIES;
    ddi.DxgkDdiSetRootPageTable(&e,&root);CHECK(!c->Root.Valid);
    CHECK(TgGpuAcquireContextWork(&e,&submit,&work)==STATUS_INVALID_PARAMETER);
    root.Address.SegmentOffset=0;root.NumEntries=2;ddi.DxgkDdiSetRootPageTable(&e,&root);
    submit.DmaBufferVirtualAddress=TG_GPU_INTERNAL_COVERAGE;
    CHECK(TgGpuAcquireContextWork(&e,&submit,&work)==STATUS_INVALID_PARAMETER); /* KMD-only range */
    submit.DmaBufferVirtualAddress=c->Root.VaLimit-4;
    CHECK(TgGpuAcquireContextWork(&e,&submit,&work)==STATUS_INVALID_PARAMETER);
    submit.DmaBufferVirtualAddress=0x10000;submit.SubmissionFenceId=8;
    CHECK(NT_SUCCESS(TgGpuAcquireContextWork(&e,&submit,&work)));
    CHECK(NT_SUCCESS(TgGpuPrepareContextWork(&e,&work)));
    CHECK(NT_SUCCESS(TgGpuSubmitContextWork(&e,&work,&io,&power)));
    CHECK(TgGpuContextWorkFinished(&e,&work)==STATUS_PENDING);
    hw.fail_reset=TRUE;
    CHECK(TgGpuResetContextWork(&e,&work,&io,&power,&clock)==STATUS_DEVICE_DATA_ERROR);
    /* power_down now stops at the failed reset readback, before changing its
     * ready/clock/rail state. All DMA backing must remain owned for retry. */
    CHECK(e.Channel.referenced && c->References==1 && power.owned && power.ready);
    CHECK(!work.Submitted && hw.clamp==1 && !hw.reset);
    hw.fail_reset=FALSE;
    CHECK(NT_SUCCESS(TgGpuResetContextWork(&e,&work,&io,&power,&clock)));
    CHECK(!e.Channel.referenced && !c->References && !work.Context && !power.owned && !e.UserdBar1Ready);
    CHECK(hw.enable==0x40 && hw.voltage==50 && (hw.reset&0x01000000));
    CHECK(e.LastCompletedFence==7 && interrupts==1);
    /* Cancellation before any hardware attempt performs no MMIO and does not
     * manufacture a completed submission fence either.
     */
    before=hw.reads+hw.writes;
    CHECK(NT_SUCCESS(TgGpuAcquireContextWork(&e,&submit,&work)));
    CHECK(NT_SUCCESS(TgGpuRetireContextWork(&e,&work,NULL,NULL)));
    CHECK(hw.reads+hw.writes==before && e.LastCompletedFence==7);
    /* Start a fresh controlled cold GPU and submit multiple contexts before
     * any fence completes. Admission must not overwrite the active marker.
     */
    power.owned=power.ready=true;e.UserdBar1Ready=TRUE;hw.mc=0x100;hw.pb=hw.map=hw.sched=1;
    hw.clamp=hw.reset=hw.inst=hw.ctrl=hw.preempt=hw.pbdma=hw.runlist=0;
    hw.enable=0x41;hw.voltage=110;
    root.hContext=c2;root.Address.SegmentOffset=PAGE_SIZE;
    ddi.DxgkDdiSetRootPageTable(&e,&root);CHECK(c2->Root.Valid);
    CHECK(NT_SUCCESS(TgGpuInitializeWorkQueue(&queue,&e)));
    submit.DmaBufferPrivateDataSize=sizeof(entries[0]);submit.pDmaBufferPrivateData=&entries[0];
    submit.DmaBufferUmdPrivateDataSize=4;
    CHECK(TgGpuQueueSubmit(&queue,&submit)==STATUS_INVALID_PARAMETER && !c->References);
    submit.DmaBufferUmdPrivateDataSize=0;
    for(i=0;i<4;++i) {
        submit.hContext=(i&1) ? c2 : c;
        submit.pDmaBufferPrivateData=&entries[i];submit.SubmissionFenceId=10+i;
        memset(&entries[i],0xcc,sizeof(entries[i]));
        CHECK(NT_SUCCESS(TgGpuQueueSubmit(&queue,&submit)));
    }
    CHECK(c->References==2 && c2->References==2 && !e.Active);
    CHECK(TgGpuQueueSubmit(&queue,&submit)==STATUS_DEVICE_BUSY);
    CHECK(TgGpuAcquireContextWork(&e,&submit,&blocked)==STATUS_DEVICE_BUSY);
    CHECK(TgGpuServiceWorkQueue(&queue,&io,&power)==STATUS_PENDING);
    CHECK(queue.Current.Context==c && queue.Current.SubmissionFence==10 && *fence==~queue.Current.Expected);
    CHECK(TgGpuServiceWorkQueue(&queue,&io,&power)==STATUS_PENDING && interrupts==1);
    *fence=queue.Current.Expected;expected_fence=10;notify_context=c;notify_references=1;
    reuse_private=&entries[0];
    CHECK(TgGpuServiceWorkQueue(&queue,&io,&power)==STATUS_SUCCESS && !reuse_private);
    CHECK(c->References==1 && c2->References==2 && e.LastCompletedFence==10 && !e.Active);
    CHECK(((UCHAR *)&entries[0])[sizeof(entries[0])-1]==0xa5);
    hw.expected_root=0x900010;
    CHECK(TgGpuServiceWorkQueue(&queue,&io,&power)==STATUS_PENDING && queue.Current.Context==c2);
    *fence=queue.Current.Expected;expected_fence=11;notify_context=c2;notify_references=1;
    reuse_private=&entries[1];
    CHECK(TgGpuServiceWorkQueue(&queue,&io,&power)==STATUS_SUCCESS && !reuse_private);
    CHECK(c->References==1 && c2->References==1 && e.LastCompletedFence==11);
    hw.expected_root=0x900000;hw.fail_tlb=TRUE;
    CHECK(TgGpuServiceWorkQueue(&queue,&io,&power)==STATUS_IO_TIMEOUT);
    CHECK(queue.Failure==STATUS_IO_TIMEOUT && e.Channel.referenced && queue.Current.Context==c);
    CHECK(TgGpuServiceWorkQueue(&queue,&io,&power)==STATUS_IO_TIMEOUT && c2->References==1);
    hw.fail_tlb=FALSE;hw.fail_reset=TRUE;
    CHECK(TgGpuResetWorkQueue(&queue,&io,&power,&clock)==STATUS_DEVICE_DATA_ERROR);
    CHECK(queue.Paused && c->References==1 && c2->References==1);
    hw.fail_reset=FALSE;
    CHECK(NT_SUCCESS(TgGpuResetWorkQueue(&queue,&io,&power,&clock)));
    CHECK(!c->References && !c2->References && !queue.Current.Context && IsListEmpty(&queue.Pending));
    CHECK(e.LastCompletedFence==11 && interrupts==3 && !e.Active && !e.Notification);
    CHECK(TgGpuResumeWorkQueue(&queue,&power)==STATUS_DEVICE_NOT_READY);
    power.owned=power.ready=true;
    CHECK(!e.UserdBar1Ready && TgGpuResumeWorkQueue(&queue,&power)==STATUS_DEVICE_NOT_READY);
    /* Model the adapter's cold reinitialization, then fail only synchronized
     * notification. Hardware/context ownership is already retired: reset
     * cancels the engine-only notice without another register access.
     */
    power.owned=power.ready=true;e.UserdBar1Ready=TRUE;hw.mc=0x100;hw.pb=hw.map=hw.sched=1;
    hw.clamp=hw.reset=hw.inst=hw.ctrl=hw.preempt=hw.pbdma=hw.runlist=0;
    CHECK(NT_SUCCESS(TgGpuResumeWorkQueue(&queue,&power)));
    submit.hContext=c;submit.SubmissionFenceId=14;submit.pDmaBufferPrivateData=&entries[0];
    CHECK(NT_SUCCESS(TgGpuQueueSubmit(&queue,&submit)));
    CHECK(TgGpuServiceWorkQueue(&queue,&io,&power)==STATUS_PENDING);
    *fence=queue.Current.Expected;fail_sync=TRUE;
    CHECK(TgGpuServiceWorkQueue(&queue,&io,&power)==STATUS_UNSUCCESSFUL);
    CHECK(!c->References && !queue.Current.Context && e.Notification==&queue.Current);
    fail_sync=FALSE;before=hw.reads+hw.writes;
    CHECK(NT_SUCCESS(TgGpuResetWorkQueue(&queue,&io,&power,&clock)));
    CHECK(hw.reads+hw.writes==before && !e.Notification && e.LastCompletedFence==11 && interrupts==3);
    CHECK(NT_SUCCESS(TgGpuStopWorkQueue(&queue)) && !e.QueueOwner);
    notify_context=NULL;
    before=frees;fail_free=TRUE;CHECK(ddi.DxgkDdiDestroyContext(c)==STATUS_UNSUCCESSFUL);
    CHECK(c->Instance.Handle && frees==before && !c->Closing);fail_free=FALSE;
    CHECK(NT_SUCCESS(ddi.DxgkDdiDestroyContext(c)));
    CHECK(TgGpuStopContextEngine(&e)==STATUS_DEVICE_BUSY);
    CHECK(ddi.DxgkDdiCreateProcess(&e,&failed)==STATUS_DEVICE_NOT_READY);
    CHECK(NT_SUCCESS(ddi.DxgkDdiDestroyContext(c2)));
    CHECK(NT_SUCCESS(ddi.DxgkDdiDestroyDevice(device.hDevice)));
    CHECK(NT_SUCCESS(ddi.DxgkDdiDestroyProcess(&e,process.hKmdProcess)));
    CHECK(NT_SUCCESS(TgGpuStopContextEngine(&e)));
    CHECK(NT_SUCCESS(TgGpuStopContextEngine(&other)));
    for(i=0;i<64;++i) CHECK(!pool_used[i] && !memory_used[i]);
    TestAdapterLifecycle(&dxgk);
    WriteFile(GetStdHandle((ULONG)-11),pass,sizeof(pass)-1,&written,NULL);ExitProcess(0);
}
