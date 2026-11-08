/* SPDX-License-Identifier: MIT */
#include "../windows/context.h"
#include <ntimage.h>

static ULONG_PTR image_cookie=0x00002b992ddfa232ULL;
const IMAGE_LOAD_CONFIG_DIRECTORY64 _load_config_used={
    .Size=sizeof(IMAGE_LOAD_CONFIG_DIRECTORY64),.SecurityCookie=(ULONGLONG)&image_cookie
};
__declspec(dllimport) void __stdcall ExitProcess(ULONG);
__declspec(dllimport) HANDLE __stdcall GetStdHandle(ULONG);
__declspec(dllimport) BOOL __stdcall WriteFile(HANDLE,const void *,ULONG,ULONG *,void *);
static ULONG check;
#define CHECK(x) do { ++check; if (!(x)) ExitProcess(check); } while (0)
void *memset(void *p,int value,size_t n)
{ UCHAR *d=p; while(n--) *d++=(UCHAR)value; return p; }
void *memcpy(void *p,const void *source,size_t n)
{ UCHAR *d=p;const UCHAR *s=source;while(n--) *d++=*s++;return p; }

/* Real WDK types and backend code, with explicit OS/GPU substitutes. Nothing
 * here allocates through live Dxgkrnl or proves physical GPU completion.
 */
static _Alignas(PAGE_SIZE) UCHAR pools[64][PAGE_SIZE], memory[64][TG_GPU_TABLE_BYTES];
static BOOLEAN pool_used[64], memory_used[64], fail_pool, fail_allocate, fail_free;
static BOOLEAN fail_sync, in_sync;
static BOOLEAN fail_reserve;
static UINT interrupts, dpcs, queues, frees, expected_fence;
static KIRQL level;
static VOID NTAPI InitializeLock(PKSPIN_LOCK lock) { *lock=0; }
VOID (NTAPI * const __imp_KeInitializeSpinLock)(PKSPIN_LOCK)=InitializeLock;
static PVOID NTAPI Pool(POOL_FLAGS flags,SIZE_T bytes,ULONG tag)
{
    UINT i;
    CHECK(flags==POOL_FLAG_NON_PAGED && bytes<=PAGE_SIZE && tag!=0 && level==PASSIVE_LEVEL);
    if(fail_pool) return NULL;
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
    CHECK(offset<sizeof(memory));p.QuadPart=0x91000000+offset;return p;
}
PHYSICAL_ADDRESS (NTAPI * const __imp_MmGetPhysicalAddress)(PVOID)=Physical;
static NTSTATUS NTAPI Allocate(HANDLE device,DXGKARGCB_ALLOCATECONTIGUOUSMEMORY *a)
{
    UINT i;CHECK(device==(HANDLE)1 && level==PASSIVE_LEVEL);
    CHECK((a->NumberOfBytes==PAGE_SIZE || a->NumberOfBytes==TG_GPU_COMMAND_BYTES ||
           a->NumberOfBytes==TG_GPU_TABLE_BYTES) && a->CacheType==DXGK_MEMORY_CACHING_TYPE_NON_CACHED);
    CHECK(a->LowestAcceptableAddress.QuadPart==0x80000000 && a->HighestAcceptableAddress.QuadPart==0xffffffff);
    if(fail_allocate) return STATUS_INSUFFICIENT_RESOURCES;
    for(i=0;i<64;++i) if(!memory_used[i]) {
        memory_used[i]=TRUE;a->hMemoryHandle=(HANDLE)(ULONG_PTR)(i+1);a->pMemory=memory[i];return STATUS_SUCCESS;
    }
    return STATUS_INSUFFICIENT_RESOURCES;
}
static NTSTATUS NTAPI Free(HANDLE device,const DXGKARGCB_FREECONTIGUOUSMEMORY *a)
{
    UINT i=(UINT)(ULONG_PTR)a->hMemoryHandle-1;
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

void start(void)
{
    static TG_GPU_CONTEXT_ENGINE e,other;
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
    UINT32 *instance,*fence;UINT i,before;ULONG written;UINT64 pde;
    static const char pass[]="PASS: WDK context lifetime, privileged internal GPU VA/command ring and fence notification (controlled substitutes)\r\n";
    dxgk.Version=DXGKDDI_INTERFACE_VERSION_WDDM2_4;dxgk.DeviceHandle=(HANDLE)1;
    dxgk.DxgkCbAllocateContiguousMemory=Allocate;dxgk.DxgkCbFreeContiguousMemory=Free;
    dxgk.DxgkCbSynchronizeExecution=Synchronize;dxgk.DxgkCbNotifyInterrupt=Interrupt;
    dxgk.DxgkCbQueueDpc=Queue;dxgk.DxgkCbNotifyDpc=Dpc;
    dxgk.DxgkCbReserveGpuVirtualAddressRange=Reserve;
    CHECK(NT_SUCCESS(TgGpuInitializeContextEngine(&e,&dxgk,&tables)));
    CHECK(NT_SUCCESS(TgGpuInitializeContextEngine(&other,&dxgk,&tables)));
    TgGpuRegisterContextDdis(&ddi);
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
    CHECK(ddi.DxgkDdiDestroyProcess(&e,process.hKmdProcess)==STATUS_DEVICE_BUSY);
    create.Flags.VirtualAddressing=1;
    fail_allocate=TRUE;
    CHECK(ddi.DxgkDdiCreateContext(device.hDevice,&create)==STATUS_INSUFFICIENT_RESOURCES);
    CHECK(((TG_GPU_DEVICE *)device.hDevice)->Contexts==0);fail_allocate=FALSE;
    CHECK(NT_SUCCESS(ddi.DxgkDdiCreateContext(device.hDevice,&create)));c=create.hContext;
    CHECK(create.ContextInfo.Caps.NoPatchingRequired && !create.ContextInfo.Caps.DriverManagesResidency);
    create.hContext=(HANDLE)103;CHECK(NT_SUCCESS(ddi.DxgkDdiCreateContext(device.hDevice,&create)));c2=create.hContext;
    CHECK(c!=c2 && ((TG_GPU_DEVICE *)device.hDevice)->Contexts==2);
    CHECK(ddi.DxgkDdiDestroyDevice(device.hDevice)==STATUS_DEVICE_BUSY);
    root.hContext=c;ddi.DxgkDdiSetRootPageTable(&e,&root);
    instance=c->Instance.CpuAddress;
    fence=(UINT32 *)((UCHAR *)e.Commands.CpuAddress+TG_GPU_COMMAND_FENCE);
    CHECK(c->Root.Valid && instance[128]==0x90000807 && instance[130]==0x07fff000);
    submit.hContext=c;CHECK(NT_SUCCESS(TgGpuAcquireContextWork(&e,&submit,&work)));
    CHECK(work.Root.Valid && c->References==1 && work.FencePhysicalAddress==0x91002800);
    CHECK(*fence==~work.Expected && instance[0x800/4]==0);
    CHECK(TgGpuSignalContextWork(&e,&work)==STATUS_DEVICE_NOT_READY);
    ((UINT64 *)table)[1]=0; /* VidMm made the reserved root entry invalid */
    CHECK(NT_SUCCESS(TgGpuPrepareContextWork(&e,&work)));
    CHECK(tg_gpu_pde_small((UINT64)((TG_GPU_PROCESS *)process.hKmdProcess)->Internal.Leaf.SystemPhysicalAddress.QuadPart,&pde));
    CHECK(((UINT64 *)table)[1]==pde && ((UINT64 *)table)[0]==0);
    CHECK(work.Prepared && instance[128]==0x90000807 && instance[130]==0x07fff000);
    CHECK(TgGpuPrepareContextWork(&e,&work)==STATUS_INVALID_DEVICE_STATE);
    CHECK(TgGpuAcquireContextWork(&e,&submit,&blocked)==STATUS_DEVICE_BUSY && !blocked.Context);
    CHECK(ddi.DxgkDdiDestroyContext(c)==STATUS_DEVICE_BUSY);
    CHECK(TgGpuSignalContextWork(&e,&work)==STATUS_PENDING && interrupts==0);
    /* A busy root change invalidates future admission but does not rewrite
     * the queued snapshot or RAMIN while the hardware may still reference it.
     */
    root.NumEntries=3;ddi.DxgkDdiSetRootPageTable(&e,&root);
    CHECK(!c->Root.Valid && c->RootStatus==STATUS_GRAPHICS_ALLOCATION_BUSY);
    CHECK(work.Root.Entries==2 && instance[130]==0x07fff000);
    *fence=work.Expected; /* explicit simulated GPU write */
    fail_sync=TRUE;CHECK(TgGpuSignalContextWork(&e,&work)==STATUS_UNSUCCESSFUL && !work.Notified);
    CHECK(!interrupts && c->References==1);fail_sync=FALSE;expected_fence=7;
    CHECK(NT_SUCCESS(TgGpuSignalContextWork(&e,&work)) && work.Notified);
    CHECK(interrupts==1 && queues==1 && c->References==1);
    CHECK(NT_SUCCESS(TgGpuSignalContextWork(&e,&work)) && interrupts==1);
    ddi.DxgkDdiDpcRoutine(&e);CHECK(dpcs==1);
    CHECK(NT_SUCCESS(ddi.DxgkDdiQueryCurrentFence(&e,&query)) && query.CurrentFence==7);
    CHECK(TgGpuReleaseContextWork(&e,&work,FALSE)==STATUS_DEVICE_BUSY);
    CHECK(ddi.DxgkDdiDestroyContext(c)==STATUS_DEVICE_BUSY);
    CHECK(NT_SUCCESS(TgGpuReleaseContextWork(&e,&work,TRUE)) && !work.Context && !c->References);
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
    CHECK(TgGpuSignalContextWork(&e,&work)==STATUS_PENDING);
    CHECK(NT_SUCCESS(TgGpuReleaseContextWork(&e,&work,TRUE))); /* completed reset, no fence */
    CHECK(e.LastCompletedFence==7 && interrupts==1);
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
    WriteFile(GetStdHandle((ULONG)-11),pass,sizeof(pass)-1,&written,NULL);ExitProcess(0);
}
