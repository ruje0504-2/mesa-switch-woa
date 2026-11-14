/* SPDX-License-Identifier: MIT
 * Real WDK DDIs and display backend; explicit allocation/MMIO/time substitutes.
 * This does not drive a physical GPU or demonstrate visible hardware rotation.
 */
#include "../windows/adapter.h"
#include "../windows/allocation.h"
#include "../platform/t214_resources.h"
#include <ntimage.h>

__declspec(dllimport) void __stdcall ExitProcess(ULONG);
__declspec(dllimport) HANDLE __stdcall GetStdHandle(ULONG);
__declspec(dllimport) BOOL __stdcall WriteFile(HANDLE,const void *,ULONG,ULONG *,void *);
static ULONG_PTR cookie=0x2b992ddfa232ULL;
const IMAGE_LOAD_CONFIG_DIRECTORY64 _load_config_used={
    .Size=sizeof(IMAGE_LOAD_CONFIG_DIRECTORY64),.SecurityCookie=(ULONGLONG)&cookie};
#define CHECK(x) do { if (!(x)) ExitProcess(__LINE__); } while (0)
void *memset(void *p,int value,size_t n)
{ UCHAR *d=p;while(n--)*d++=(UCHAR)value;return p; }
void *memcpy(void *p,const void *s,size_t n)
{ UCHAR *d=p;const UCHAR *b=s;while(n--)*d++=*b++;return p; }
#define DC_CHECK CHECK
#include "dc_model.h"

KUSER_SHARED_DATA ki_user_shared_data;
static struct display_model model;
static TG_GPU_ADAPTER adapter;
static KIRQL level;
static BOOLEAN auto_latch=TRUE;
static UINT stalls, notifications, sync_calls;
static ULONGLONG reported_present;

static KIRQL NTAPI current_irql(VOID) { return level; }
KIRQL (NTAPI *const __imp_KeGetCurrentIrql)(VOID)=current_irql;
static void advance(ULONGLONG ticks)
{
    /* The test variants use the WDK's relocatable shared-time support. */
    *(volatile ULONGLONG *)&ki_user_shared_data.InterruptTime+=ticks;
    if(auto_latch)latch(&model,true);
}
static VOID NTAPI stall(ULONG us)
{ CHECK(level==PASSIVE_LEVEL && us==1);++stalls;advance(1000); }
static NTSTATUS NTAPI delay(KPROCESSOR_MODE mode,BOOLEAN alert,PLARGE_INTEGER interval)
{ CHECK(level==PASSIVE_LEVEL && mode==KernelMode && !alert && interval->QuadPart==-10000);advance(10000);return STATUS_SUCCESS; }
VOID (NTAPI *const __imp_KeStallExecutionProcessor)(ULONG)=stall;
NTSTATUS (NTAPI *const __imp_KeDelayExecutionThread)(KPROCESSOR_MODE,BOOLEAN,PLARGE_INTEGER)=delay;
static NTSTATUS APIENTRY synchronize(HANDLE handle,PKSYNCHRONIZE_ROUTINE routine,
    PVOID context,ULONG message,PBOOLEAN result)
{
    CHECK(handle==&adapter && !message && level<=DISPATCH_LEVEL);
    KIRQL old=level;level=3;++sync_calls;*result=routine(context);level=old;
    return STATUS_SUCCESS;
}
static VOID APIENTRY interrupt(HANDLE handle,DXGKARGCB_NOTIFY_INTERRUPT_DATA *data)
{
    CHECK(handle==&adapter && level==3 && data->InterruptType==DXGK_INTERRUPT_CRTC_VSYNC_WITH_MULTIPLANE_OVERLAY2);
    CHECK(data->CrtcVsyncWithMultiPlaneOverlay2.VidPnTargetId==7);
    const UINT count=data->CrtcVsyncWithMultiPlaneOverlay2.MultiPlaneOverlayVsyncInfoCount;
    CHECK(count<=1);
    reported_present=count?data->CrtcVsyncWithMultiPlaneOverlay2.pMultiPlaneOverlayVsyncInfo[0].PresentId:0;
    ++notifications;
}
static BOOLEAN APIENTRY queue_dpc(HANDLE handle)
{ CHECK(handle==&adapter && level==3);return TRUE; }

/* Lifecycle registration/start are linked but deliberately not exercised. */
NTSTATUS TgGpuCreateScanoutSegment(const DXGKRNL_INTERFACE *dxgk,TG_GPU_TABLE_SEGMENT *tables)
{ (void)dxgk;(void)tables;CHECK(FALSE);return STATUS_NOT_IMPLEMENTED; }
NTSTATUS APIENTRY TgGpuDdiGetStandardAllocationDriverData(HANDLE context,DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA *args)
{ (void)context;(void)args;CHECK(FALSE);return STATUS_NOT_IMPLEMENTED; }
void TgGpuRegisterPanelVidPn(DRIVER_INITIALIZATION_DATA *ddis)
{ (void)ddis;CHECK(FALSE); }
NTSTATUS TgGpuDdiStopDevice(PVOID context)
{ (void)context;CHECK(FALSE);return STATUS_NOT_IMPLEMENTED; }
NTSTATUS TgGpuDdiRemoveDevice(PVOID context)
{ (void)context;CHECK(FALSE);return STATUS_NOT_IMPLEMENTED; }

static TG_GPU_PRIMARY_ALLOCATION_REQUEST allocations[4];
/* Real metadata ownership/rejection is covered by test-context. This fixture
 * supplies four known immutable KMD handles to exercise display DDIs. */
NTSTATUS TgGpuGetScanoutSurface(TG_GPU_CONTEXT_ENGINE *engine,HANDLE handle,
    TG_GPU_PRIMARY_ALLOCATION_REQUEST *out)
{
    CHECK(engine==&adapter.Engine && out);
    for(UINT i=0;i<4;++i)if(handle==&allocations[i]){*out=allocations[i];return STATUS_SUCCESS;}
    return STATUS_INVALID_PARAMETER;
}
NTSTATUS TgGpuResolveScanoutAddress(const TG_GPU_TABLE_SEGMENT *tables,
    UINT64 address,UINT64 bytes,UINT64 *out)
{
    CHECK(tables==&adapter.Tables && bytes==T214_GPU_POST_FB_SIZE);
    if((address&4095) || address<0x98000000 || address>0x99000000-bytes)
        return STATUS_INVALID_ADDRESS;
    *out=address;return STATUS_SUCCESS;
}

static void signal_vblank(const DRIVER_INITIALIZATION_DATA *d)
{
    model.assembly[T214_DC_INT_STATUS]|=T214_DC_VBLANK_INT;
    KIRQL old=level;level=3;
    CHECK(d->DxgkDdiInterruptRoutine(&adapter,0));
    level=old;
}
static void active_is(enum tg_t214_dc_rotation rotation,UINT64 address,BOOLEAN visible)
{
    struct tg_t214_dc_scanout active;
    CHECK(tg_t214_dc_latched_surface(&adapter.Panel.Dc,&active));
    CHECK(active.rotation==rotation && active.address==address && active.visible==(visible!=FALSE));
    const UINT options[]={0,0x11,5,0x14};
    CHECK((model.active[T214_DC_WIN_A_OPTIONS]&0x15)==options[rotation]);
    CHECK(model.active[T214_DC_WIN_A_STRIDE]==((rotation&1)?5120u:2880u));
}

void start(void)
{
    initialize(&model);
    const struct tg_t214_dc_io io={&model,read_reg,write_reg};
    CHECK(tg_t214_dc_acquire(&adapter.Panel.Dc,&io)==TG_T214_DC_OK);
    latch(&model,true);
    adapter.Panel.Boot.TargetId=7;
    adapter.Panel.SourceVisible=TRUE;
    adapter.Panel.Displayed=(TG_GPU_PANEL_FLIP){.Address=T214_GPU_POST_FB_BASE,.Visible=TRUE};
    adapter.Engine.Dxgk=(DXGKRNL_INTERFACE){.DeviceHandle=&adapter,
        .DxgkCbSynchronizeExecution=synchronize,.DxgkCbNotifyInterrupt=interrupt,.DxgkCbQueueDpc=queue_dpc};
    DRIVER_INITIALIZATION_DATA d={0};TgGpuRegisterPanelScanout(&d);
    CHECK(d.DxgkDdiControlInterrupt(&adapter,DXGK_INTERRUPT_CRTC_VSYNC_WITH_MULTIPLANE_OVERLAY2,TRUE)==STATUS_SUCCESS);
    for(UINT r=0;r<4;++r) {
        const BOOLEAN quarter=(r&1)!=0;
        allocations[r]=(TG_GPU_PRIMARY_ALLOCATION_REQUEST){
            .Buffer={TG_GPU_ALLOCATION_ABI,TG_GPU_ALLOCATION_CPU_VISIBLE|TG_GPU_ALLOCATION_DISPLAY_PRIMARY,T214_GPU_POST_FB_SIZE},
            .Format=D3DDDIFMT_A8R8G8B8,.Rotation=r+1,.Width=quarter?1280:720,.Height=quarter?720:1280,
            .Pitch=quarter?5120:2880};
        DXGKARG_SETVIDPNSOURCEADDRESS a={.PrimarySegment=TG_GPU_SCANOUT_SEGMENT,
            .PrimaryAddress={.QuadPart=0x98000000+r*0x400000},.hAllocation=&allocations[r],.ContextCount=1};
        const UINT before=sync_calls;
        level=3;CHECK(d.DxgkDdiSetVidPnSourceAddress(&adapter,&a)==STATUS_SUCCESS);level=PASSIVE_LEVEL;
        CHECK(sync_calls==before && adapter.Panel.Pending.Rotation==(enum tg_t214_dc_rotation)r);
        CHECK(d.DxgkDdiSetVidPnSourceAddress(&adapter,&a)==STATUS_DEVICE_BUSY);
        latch(&model,false);
        CHECK(adapter.Panel.Pending.Address && model.assembly[T214_DC_STATE_CONTROL]);
        latch(&model,true);signal_vblank(&d);
        active_is((enum tg_t214_dc_rotation)r,(UINT64)a.PrimaryAddress.QuadPart,TRUE);
        CHECK(!adapter.Panel.Pending.Address && adapter.Panel.Displayed.Rotation==(enum tg_t214_dc_rotation)r);
    }

    DXGK_PRIMARYCONTEXTDATA primary={.hAllocation=&allocations[1],.SegmentId=TG_GPU_SCANOUT_SEGMENT,
        .SegmentAddress={.QuadPart=0x98000000}};
    DXGK_PRIMARYCONTEXTDATA *contexts[]={&primary};
    DXGK_MULTIPLANE_OVERLAY_PLANE3 plane={.ContextCount=1,.ppContextData=contexts,
        .InputFlags={.Enabled=1},.PlaneAttributes={.DstRect={0,0,720,1280},
        .ColorSpaceType=D3DDDI_COLOR_SPACE_RGB_FULL_G22_NONE_P709}};
    DXGK_MULTIPLANE_OVERLAY_PLANE3 *planes[]={&plane};
    DXGKARG_SETVIDPNSOURCEADDRESSWITHMULTIPLANEOVERLAY3 args={.PlaneCount=1,.ppPlanes=planes};
    const UINT physical[]={0,3,2,1}; /* MPO is clockwise; backend is CCW. */
    for(UINT r=0;r<4;++r) {
        const UINT slot=(r&1)?1:0;
        primary.hAllocation=&allocations[slot];
        plane.PlaneAttributes.SrcRect=(RECT){0,0,(LONG)allocations[slot].Width,(LONG)allocations[slot].Height};
        plane.PlaneAttributes.Rotation=(D3DDDI_ROTATION)(r+1);
        plane.PresentId=100+r;
        DXGK_MULTIPLANE_OVERLAY_PLANE_WITH_SOURCE2 check_plane={.hAllocation=primary.hAllocation,
            .PlaneAttributes=plane.PlaneAttributes};
        DXGK_MULTIPLANE_OVERLAY_PLANE_WITH_SOURCE2 *check_planes[]={&check_plane};
        DXGKARG_CHECKMULTIPLANEOVERLAYSUPPORT3 check={.PlaneCount=1,.ppPlanes=check_planes};
        UINT writes=model.writes;
        CHECK(d.DxgkDdiCheckMultiPlaneOverlaySupport3(&adapter,&check)==STATUS_SUCCESS && check.Supported && writes==model.writes);
        CHECK(d.DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay3(&adapter,&args)==STATUS_SUCCESS);
        signal_vblank(&d);CHECK(reported_present!=plane.PresentId);
        latch(&model,true);
        model.active[T214_DC_WIN_A_STRIDE]^=64;
        signal_vblank(&d);CHECK(reported_present!=plane.PresentId && adapter.Panel.Pending.Address);
        model.active[T214_DC_WIN_A_STRIDE]^=64;
        signal_vblank(&d);CHECK(reported_present==plane.PresentId && !adapter.Panel.Pending.Address);
        active_is((enum tg_t214_dc_rotation)physical[r],0x98000000,TRUE);
        check_plane.PlaneAttributes.SrcRect.right--;
        CHECK(d.DxgkDdiCheckMultiPlaneOverlaySupport3(&adapter,&check)==STATUS_SUCCESS && !check.Supported);
    }
    /* Same address, different quarter turn: address equality is insufficient. */
    plane.PlaneAttributes.Rotation=D3DDDI_ROTATION_90;plane.PresentId=200;
    CHECK(d.DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay3(&adapter,&args)==STATUS_SUCCESS);
    latch(&model,false);signal_vblank(&d);CHECK(reported_present!=200);
    latch(&model,true);signal_vblank(&d);CHECK(reported_present==200);
    active_is(TG_T214_DC_CCW270,0x98000000,TRUE);

    UINT writes=model.writes;
    allocations[1].Pitch=2880;
    CHECK(d.DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay3(&adapter,&args)==STATUS_INVALID_PARAMETER && model.writes==writes);
    allocations[1].Pitch=5120;
    primary.SegmentAddress.QuadPart=0x98fff000;
    CHECK(d.DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay3(&adapter,&args)==STATUS_INVALID_ADDRESS && model.writes==writes);
    primary.SegmentAddress.QuadPart=0x98400000;
    plane.InputFlags.FlipImmediate=1;plane.PresentId=201;
    level=DISPATCH_LEVEL;
    CHECK(d.DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay3(&adapter,&args)==STATUS_RETRY && args.OutputFlags.PrePresentNeeded);
    CHECK(model.writes==writes);level=PASSIVE_LEVEL;
    CHECK(d.DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay3(&adapter,&args)==STATUS_SUCCESS && stalls && !args.OutputFlags.Value);
    CHECK(adapter.Panel.Displayed.PresentId==201 && !adapter.Panel.Pending.Address);
    active_is(TG_T214_DC_CCW270,0x98400000,TRUE);

    /* No latch means no premature completion, including a rotation at the
     * same address. A later retry can finish after the exact state is active. */
    auto_latch=FALSE;plane.PresentId=202;plane.PlaneAttributes.Rotation=D3DDDI_ROTATION_270;
    CHECK(d.DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay3(&adapter,&args)==STATUS_INVALID_PARAMETER);
    CHECK(adapter.Panel.Displayed.PresentId==201 && adapter.Panel.Pending.PresentId==202);
    auto_latch=TRUE;latch(&model,true);
    CHECK(d.DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay3(&adapter,&args)==STATUS_SUCCESS);
    CHECK(adapter.Panel.Displayed.PresentId==202 && !adapter.Panel.Pending.Address);

    /* Visibility changes stop/start the assigned primary. They do not replace
     * it with POST: no new flip is required to recover the same surface. */
    plane.InputFlags.FlipImmediate=0;plane.PresentId=203;
    CHECK(d.DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay3(&adapter,&args)==STATUS_SUCCESS);
    CHECK(TgGpuPanelSetVisibility(&adapter,FALSE)==STATUS_SUCCESS && !adapter.Panel.Pending.Address);
    active_is(TG_T214_DC_CCW90,0x98400000,FALSE);
    signal_vblank(&d);CHECK(reported_present==203);
    CHECK(TgGpuPanelSetVisibility(&adapter,TRUE)==STATUS_SUCCESS);
    active_is(TG_T214_DC_CCW90,0x98400000,TRUE);
    signal_vblank(&d);CHECK(reported_present==203);

    /* MPO disable also has no replacement allocation: only its enable bit
     * changes. Source visibility must still know which primary to show. */
    plane.InputFlags.Enabled=0;plane.ContextCount=0;plane.ppContextData=NULL;
    plane.PresentId=204;
    CHECK(d.DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay3(&adapter,&args)==STATUS_SUCCESS);
    latch(&model,true);signal_vblank(&d);
    active_is(TG_T214_DC_CCW90,0x98400000,FALSE);
    CHECK(TgGpuPanelSetVisibility(&adapter,TRUE)==STATUS_SUCCESS);
    active_is(TG_T214_DC_CCW90,0x98400000,TRUE);
    plane.InputFlags.Enabled=1;plane.ContextCount=1;plane.ppContextData=contexts;

    /* A replacement primary can be bound while the source is hidden. Keep
     * fetch disabled until the separate source-visibility DDI enables it. */
    CHECK(TgGpuPanelSetVisibility(&adapter,FALSE)==STATUS_SUCCESS);
    static DXGKARG_SETVIDPNSOURCEADDRESS hidden={.PrimarySegment=TG_GPU_SCANOUT_SEGMENT,
        .Flags={.ModeChange=1},.PrimaryAddress={.QuadPart=0x98800000},.hAllocation=&allocations[2]};
    CHECK(d.DxgkDdiSetVidPnSourceAddress(&adapter,&hidden)==STATUS_SUCCESS);
    active_is(TG_T214_DC_CCW180,0x98800000,FALSE);
    CHECK(TgGpuPanelSetVisibility(&adapter,TRUE)==STATUS_SUCCESS);
    active_is(TG_T214_DC_CCW180,0x98800000,TRUE);

    CHECK(TgGpuPanelSetVisibility(&adapter,FALSE)==STATUS_SUCCESS);
    plane.PresentId=205;
    CHECK(d.DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay3(&adapter,&args)==STATUS_SUCCESS);
    latch(&model,true);
    const UINT hidden_notifications=notifications;
    signal_vblank(&d);
    CHECK(notifications==hidden_notifications+1 && reported_present==205);
    active_is(TG_T214_DC_CCW90,0x98400000,FALSE);
    CHECK(TgGpuPanelSetVisibility(&adapter,TRUE)==STATUS_SUCCESS);
    active_is(TG_T214_DC_CCW90,0x98400000,TRUE);

    /* A blanking timeout does not pretend the old scanout has stopped. */
    auto_latch=FALSE;
    CHECK(TgGpuPanelSetVisibility(&adapter,FALSE)==STATUS_IO_TIMEOUT);
    CHECK(model.active[T214_DC_WIN_A_OPTIONS]&T214_DC_WIN_ENABLE);
    auto_latch=TRUE;latch(&model,true);
    CHECK(TgGpuPanelSetVisibility(&adapter,FALSE)==STATUS_SUCCESS);
    active_is(TG_T214_DC_CCW90,0x98400000,FALSE);
    CHECK(TgGpuPanelSetVisibility(&adapter,TRUE)==STATUS_SUCCESS);
    active_is(TG_T214_DC_CCW90,0x98400000,TRUE);

    /* Unlike a scheduled flip, a modeset returns only after the new primary
     * is active. Reuse the same two slots across repeated quarter turns. */
    const UINT orientations[] = {0,1,0,1,0};
    DXGKARG_SETVIDPNSOURCEADDRESS modeset = {
        .PrimarySegment=TG_GPU_SCANOUT_SEGMENT,.Flags={.ModeChange=1}};
    for (UINT i=0;i<sizeof(orientations)/sizeof(orientations[0]);++i) {
        UINT r=orientations[i];
        modeset.hAllocation=&allocations[r];
        modeset.PrimaryAddress.QuadPart=0x98000000+r*0x400000;
        CHECK(d.DxgkDdiSetVidPnSourceAddress(&adapter,&modeset)==STATUS_SUCCESS);
        CHECK(!adapter.Panel.Pending.Address);
        active_is((enum tg_t214_dc_rotation)r,(UINT64)modeset.PrimaryAddress.QuadPart,TRUE);
        CHECK(!(model.assembly[T214_DC_REG_ACT_CONTROL]&T214_DC_WIN_A_HCOUNTER));
    }
    /* Drain an older queued MPO flip instead of returning DEVICE_BUSY to the
     * synchronous mode change, then confirm the replacement's full layout. */
    plane.PresentId=206;
    CHECK(d.DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay3(&adapter,&args)==STATUS_SUCCESS);
    CHECK(adapter.Panel.Pending.Address);
    CHECK(d.DxgkDdiSetVidPnSourceAddress(&adapter,&modeset)==STATUS_SUCCESS);
    CHECK(!adapter.Panel.Pending.Address);
    active_is(TG_T214_DC_IDENTITY,0x98000000,TRUE);

    writes=model.writes;level=DISPATCH_LEVEL;
    CHECK(d.DxgkDdiSetVidPnSourceAddress(&adapter,&modeset)==STATUS_INVALID_PARAMETER && model.writes==writes);
    level=PASSIVE_LEVEL;auto_latch=FALSE;
    modeset.hAllocation=&allocations[1];modeset.PrimaryAddress.QuadPart=0x98400000;
    CHECK(d.DxgkDdiSetVidPnSourceAddress(&adapter,&modeset)==STATUS_IO_TIMEOUT);
    CHECK(adapter.Panel.Pending.Address==0x98400000 && adapter.Panel.Displayed.Address==0x98000000);
    auto_latch=TRUE;modeset.hAllocation=&allocations[0];modeset.PrimaryAddress.QuadPart=0x98000000;
    CHECK(d.DxgkDdiSetVidPnSourceAddress(&adapter,&modeset)==STATUS_SUCCESS);
    CHECK(!adapter.Panel.Pending.Address);
    active_is(TG_T214_DC_IDENTITY,0x98000000,TRUE);

    const struct tg_t214_dc_scanout rotated_post={T214_GPU_POST_FB_BASE,TG_T214_DC_CCW90,true};
    CHECK(tg_t214_dc_queue_surface(&adapter.Panel.Dc,&rotated_post,false)==TG_T214_DC_OK);
    latch(&model,true);
    CHECK(TgGpuPanelStop(&adapter)==STATUS_SUCCESS && !adapter.Panel.Dc.owned);
    CHECK(model.active[T214_DC_WIN_A_STRIDE]==2880 && !(model.active[T214_DC_WIN_A_OPTIONS]&0x15));
    CHECK(notifications && level==PASSIVE_LEVEL);
    const char text[]="PASS: WDDM legacy/MPO rotation, source visibility and hidden rebinding, modeset retirement, full-layout latch and POST recovery (model only)\r\n";
    ULONG written;WriteFile(GetStdHandle((ULONG)-11),text,sizeof(text)-1,&written,NULL);
    ExitProcess(0);
}
