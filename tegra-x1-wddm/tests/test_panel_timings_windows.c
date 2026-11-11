/* SPDX-License-Identifier: MIT
 * Real WDK layout, controlled display visibility; no hardware execution.
 */
#include "../windows/adapter.h"
#include <ntimage.h>

__declspec(dllimport) void __stdcall ExitProcess(ULONG code);
#define CHECK(x) do { if (!(x)) ExitProcess(__LINE__); } while (0)
static ULONG_PTR cookie = 0x2b992ddfa232ULL;
const IMAGE_LOAD_CONFIG_DIRECTORY64 _load_config_used = {
    .Size = sizeof(IMAGE_LOAD_CONFIG_DIRECTORY64),
    .SecurityCookie = (ULONGLONG)&cookie,
};

const D3DKMDT_VIDEO_SIGNAL_INFO TgGpuPanelSignal = {
    .TotalSize={1000,1300},.ActiveSize={720,1280},.PixelRate=78000000,
    .VSyncFreq={60,1},.AdditionalSignalInfo={.ScanLineOrdering=D3DDDI_VSSLO_PROGRESSIVE},
};
static NTSTATUS visibility_status;
static ULONG visibility_calls;
static BOOLEAN visible;

NTSTATUS TgGpuPanelSetVisibility(TG_GPU_ADAPTER *adapter, BOOLEAN enabled)
{
    CHECK(adapter && adapter->Panel.Dc.owned);
    ++visibility_calls;
    if (NT_SUCCESS(visibility_status)) visible = enabled;
    return visibility_status;
}

static KIRQL current_irql(VOID) { return PASSIVE_LEVEL; }
KIRQL (*__imp_KeGetCurrentIrql)(VOID) = current_irql;

void *memset(void *dest, int value, size_t bytes)
{
    unsigned char *d = dest;
    for (size_t i = 0; i < bytes; ++i) d[i] = (unsigned char)value;
    return dest;
}

void *memcpy(void *dest, const void *source, size_t bytes)
{
    unsigned char *d = dest;
    const unsigned char *s = source;
    for (size_t i = 0; i < bytes; ++i) d[i] = s[i];
    return dest;
}

static ULONGLONG check_result(const DXGK_SET_TIMING_PATH_INFO *path,
    ULONGLONG previous, UINT target, UINT input)
{
    /* A zero ID or status makes Windows replace a successful timing with
     * LinkConfigurationFailed. Preserve all input and reserved fields. */
    CHECK(path->TargetState.ConnectionChangeId > previous);
    CHECK(path->TargetState.ConnectionStatus == 14);
    CHECK(path->TargetState.TargetId == target && !path->TargetState.Reserved);
    CHECK(path->VidPnTargetId == target && path->InputFlags == input);
    CHECK(!path->OutputFlags && path->DiagnosticInfo == 0x00ffffff);
    return path->TargetState.ConnectionChangeId;
}

static D3DKMDT_VIDPN_PRESENT_PATH test_path,updated_path;
static D3DKMDT_VIDPN_SOURCE_MODE source_mode;
static D3DKMDT_VIDPN_TARGET_MODE target_mode;
static UINT path_refs,source_refs,target_refs,source_sets,target_sets;
static BOOLEAN source_pinned=TRUE;
#define VIDPN ((D3DKMDT_HVIDPN)1)
#define TOPOLOGY ((D3DKMDT_HVIDPNTOPOLOGY)2)
#define SOURCE_SET ((D3DKMDT_HVIDPNSOURCEMODESET)3)
#define TARGET_SET ((D3DKMDT_HVIDPNTARGETMODESET)4)
static NTSTATUS APIENTRY get_count(D3DKMDT_HVIDPNTOPOLOGY topology,SIZE_T *count)
{ CHECK(topology==TOPOLOGY);*count=1;return STATUS_SUCCESS; }
static NTSTATUS APIENTRY get_path(D3DKMDT_HVIDPNTOPOLOGY topology,const D3DKMDT_VIDPN_PRESENT_PATH **path)
{ CHECK(topology==TOPOLOGY && !path_refs++);*path=&test_path;return STATUS_SUCCESS; }
static NTSTATUS APIENTRY release_path(D3DKMDT_HVIDPNTOPOLOGY topology,const D3DKMDT_VIDPN_PRESENT_PATH *path)
{ CHECK(topology==TOPOLOGY && path==&test_path && path_refs--==1);return STATUS_SUCCESS; }
static NTSTATUS APIENTRY update_path(D3DKMDT_HVIDPNTOPOLOGY topology,const D3DKMDT_VIDPN_PRESENT_PATH *path)
{ CHECK(topology==TOPOLOGY && path_refs==1);updated_path=*path;return STATUS_SUCCESS; }
static const DXGK_VIDPNTOPOLOGY_INTERFACE topology_api={.pfnGetNumPaths=get_count,
    .pfnAcquireFirstPathInfo=get_path,.pfnReleasePathInfo=release_path,.pfnUpdatePathSupportInfo=update_path};
static NTSTATUS APIENTRY get_topology(D3DKMDT_HVIDPN vidpn,D3DKMDT_HVIDPNTOPOLOGY *topology,
    const DXGK_VIDPNTOPOLOGY_INTERFACE **api)
{ CHECK(vidpn==VIDPN);*topology=TOPOLOGY;*api=&topology_api;return STATUS_SUCCESS; }
static NTSTATUS APIENTRY source_info(D3DKMDT_HVIDPNSOURCEMODESET set,const D3DKMDT_VIDPN_SOURCE_MODE **mode)
{
    CHECK(set==SOURCE_SET && source_sets==1);
    if(!source_pinned){*mode=NULL;return STATUS_GRAPHICS_MODE_NOT_PINNED;}
    CHECK(!source_refs++);*mode=&source_mode;return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY release_source_info(D3DKMDT_HVIDPNSOURCEMODESET set,const D3DKMDT_VIDPN_SOURCE_MODE *mode)
{ CHECK(set==SOURCE_SET && mode==&source_mode && source_refs--==1);return STATUS_SUCCESS; }
static NTSTATUS APIENTRY target_info(D3DKMDT_HVIDPNTARGETMODESET set,const D3DKMDT_VIDPN_TARGET_MODE **mode)
{ CHECK(set==TARGET_SET && target_sets==1 && !target_refs++);*mode=&target_mode;return STATUS_SUCCESS; }
static NTSTATUS APIENTRY release_target_info(D3DKMDT_HVIDPNTARGETMODESET set,const D3DKMDT_VIDPN_TARGET_MODE *mode)
{ CHECK(set==TARGET_SET && mode==&target_mode && target_refs--==1);return STATUS_SUCCESS; }
static const DXGK_VIDPNSOURCEMODESET_INTERFACE source_api={
    .pfnAcquirePinnedModeInfo=source_info,.pfnReleaseModeInfo=release_source_info};
static const DXGK_VIDPNTARGETMODESET_INTERFACE target_api={
    .pfnAcquirePinnedModeInfo=target_info,.pfnReleaseModeInfo=release_target_info};
static NTSTATUS APIENTRY get_source(D3DKMDT_HVIDPN vidpn,D3DDDI_VIDEO_PRESENT_SOURCE_ID id,
    D3DKMDT_HVIDPNSOURCEMODESET *set,const DXGK_VIDPNSOURCEMODESET_INTERFACE **api)
{ CHECK(vidpn==VIDPN && !id && !source_sets++);*set=SOURCE_SET;*api=&source_api;return STATUS_SUCCESS; }
static NTSTATUS APIENTRY get_target(D3DKMDT_HVIDPN vidpn,D3DDDI_VIDEO_PRESENT_TARGET_ID id,
    D3DKMDT_HVIDPNTARGETMODESET *set,const DXGK_VIDPNTARGETMODESET_INTERFACE **api)
{ CHECK(vidpn==VIDPN && id==7 && !target_sets++);*set=TARGET_SET;*api=&target_api;return STATUS_SUCCESS; }
static NTSTATUS APIENTRY release_source(D3DKMDT_HVIDPN vidpn,D3DKMDT_HVIDPNSOURCEMODESET set)
{ CHECK(vidpn==VIDPN && set==SOURCE_SET && !source_refs && source_sets--==1);return STATUS_SUCCESS; }
static NTSTATUS APIENTRY release_target(D3DKMDT_HVIDPN vidpn,D3DKMDT_HVIDPNTARGETMODESET set)
{ CHECK(vidpn==VIDPN && set==TARGET_SET && !target_refs && target_sets--==1);return STATUS_SUCCESS; }
static const DXGK_VIDPN_INTERFACE vidpn_api={.Version=DXGK_VIDPN_INTERFACE_VERSION_V1,
    .pfnGetTopology=get_topology,.pfnAcquireSourceModeSet=get_source,.pfnReleaseSourceModeSet=release_source,
    .pfnAcquireTargetModeSet=get_target,.pfnReleaseTargetModeSet=release_target};
static NTSTATUS APIENTRY query_vidpn(D3DKMDT_HVIDPN vidpn,DXGK_VIDPN_INTERFACE_VERSION version,
    const DXGK_VIDPN_INTERFACE **api)
{ CHECK(vidpn==VIDPN && version==DXGK_VIDPN_INTERFACE_VERSION_V1);*api=&vidpn_api;return STATUS_SUCCESS; }

static void test_rotation(TG_GPU_ADAPTER *adapter,const DRIVER_INITIALIZATION_DATA *d)
{
    adapter->Panel.Boot.TargetId=7;
    adapter->Panel.PathRotation=D3DKMDT_VPPR_IDENTITY;
    adapter->Engine.Dxgk.DxgkCbQueryVidPnInterface=query_vidpn;
    test_path=(D3DKMDT_VIDPN_PRESENT_PATH){.VidPnTargetId=7,
        .ContentTransformation={.Scaling=D3DKMDT_VPPS_IDENTITY,.Rotation=D3DKMDT_VPPR_UNPINNED}};
    source_mode=(D3DKMDT_VIDPN_SOURCE_MODE){.Type=D3DKMDT_RMT_GRAPHICS,
        .Format.Graphics={.PrimSurfSize={720,1280},.VisibleRegionSize={720,1280},.Stride=2880,
        .PixelFormat=D3DDDIFMT_A8R8G8B8,.PixelValueAccessMode=D3DKMDT_PVAM_DIRECT}};
    target_mode.VideoSignalInfo=TgGpuPanelSignal;
    DXGKARG_ENUMVIDPNCOFUNCMODALITY enumerate={.hConstrainingVidPn=VIDPN};
    CHECK(d->DxgkDdiEnumVidPnCofuncModality(adapter,&enumerate)==STATUS_SUCCESS);
    D3DKMDT_VIDPN_PRESENT_PATH_ROTATION_SUPPORT support=updated_path.ContentTransformation.RotationSupport;
    CHECK(support.Identity && support.Rotate90 && support.Rotate180 && support.Rotate270 &&
        support.Offset0 && !support.Offset90 && !support.Offset180 && !support.Offset270);
    /* A rotation pivot's support must remain untouched. */
    enumerate.EnumPivotType=D3DKMDT_EPT_ROTATION;enumerate.EnumPivot.VidPnTargetId=7;
    test_path.ContentTransformation.RotationSupport=(D3DKMDT_VIDPN_PRESENT_PATH_ROTATION_SUPPORT){.Rotate90=1};
    CHECK(d->DxgkDdiEnumVidPnCofuncModality(adapter,&enumerate)==STATUS_SUCCESS);
    support=updated_path.ContentTransformation.RotationSupport;
    CHECK(support.Rotate90 && !support.Identity && !support.Rotate180 && !support.Rotate270 && !support.Offset0);
    DXGKARG_QUERYVIDPNHWCAPABILITY caps={.hFunctionalVidPn=VIDPN,.TargetId=7};
    CHECK(d->DxgkDdiQueryVidPnHWCapability(adapter,&caps)==STATUS_SUCCESS && !caps.VidPnHWCaps.DriverRotation);
    for(UINT r=1;r<=4;++r) {
        test_path.ContentTransformation.Rotation=(D3DKMDT_VIDPN_PRESENT_PATH_ROTATION)r;
        DXGKARG_ISSUPPORTEDVIDPN check={.hDesiredVidPn=VIDPN};
        CHECK(d->DxgkDdiIsSupportedVidPn(adapter,&check)==STATUS_SUCCESS && check.IsVidPnSupported);
        DXGKARG_COMMITVIDPN commit={.hFunctionalVidPn=VIDPN};
        CHECK(d->DxgkDdiCommitVidPn(adapter,&commit)==STATUS_SUCCESS && visible && (UINT)adapter->Panel.PathRotation==r);
        DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH update={.VidPnPresentPathInfo=test_path};
        CHECK(d->DxgkDdiUpdateActiveVidPnPresentPath(adapter,&update)==STATUS_SUCCESS);
        update.VidPnPresentPathInfo.ContentTransformation.Rotation=(D3DKMDT_VIDPN_PRESENT_PATH_ROTATION)(r%4+1);
        CHECK(d->DxgkDdiUpdateActiveVidPnPresentPath(adapter,&update)==
            STATUS_GRAPHICS_PATH_CONTENT_GEOMETRY_TRANSFORMATION_NOT_SUPPORTED && (UINT)adapter->Panel.PathRotation==r);
        CHECK(!path_refs && !source_refs && !target_refs && !source_sets && !target_sets);
    }
    const D3DKMDT_VIDPN_PRESENT_PATH_ROTATION saved=adapter->Panel.PathRotation;
    test_path.ContentTransformation.Rotation=D3DKMDT_VPPR_IDENTITY;
    DXGKARG_COMMITVIDPN commit={.hFunctionalVidPn=VIDPN};
    visibility_status=STATUS_IO_DEVICE_ERROR;
    CHECK(d->DxgkDdiCommitVidPn(adapter,&commit)==visibility_status && adapter->Panel.PathRotation==saved);
    visibility_status=STATUS_SUCCESS;source_pinned=FALSE;
    CHECK(d->DxgkDdiCommitVidPn(adapter,&commit)==STATUS_GRAPHICS_INVALID_VIDPN_TOPOLOGY);
    source_pinned=TRUE;
    test_path.ContentTransformation.Rotation=D3DKMDT_VPPR_IDENTITY_OFFSET90;
    DXGKARG_ISSUPPORTEDVIDPN check={.hDesiredVidPn=VIDPN};
    CHECK(d->DxgkDdiIsSupportedVidPn(adapter,&check)==STATUS_SUCCESS && !check.IsVidPnSupported);
    CHECK(!path_refs && !source_refs && !target_refs && !source_sets && !target_sets);
}

void start(void)
{
    static TG_GPU_ADAPTER adapter;
    DRIVER_INITIALIZATION_DATA d = {0};
    TgGpuRegisterPanelVidPn(&d);
    adapter.Panel.Dc.owned = true;
    adapter.Panel.Boot.TargetId = 7;
    DXGK_SET_TIMING_PATH_INFO path = {.VidPnTargetId = 7,
        .Input = {.Active = 1, .PreserveInherited = 1, .VidPnPathUpdates = 1}};
    DXGK_SET_TIMING_RESULTS results = {.Value = UINT32_MAX};
    DXGKARG_SETTIMINGSFROMVIDPN args = {
        .pResultsFlags = &results, .PathCount = 1, .pSetTimingPathInfo = &path,
    };
    UINT input = path.InputFlags;
    CHECK(d.DxgkDdiSetTimingsFromVidPn(&adapter, &args) == STATUS_SUCCESS);
    CHECK(visible && visibility_calls == 1 && !results.Value);
    ULONGLONG id = check_result(&path, 0, 7, input);

    path.Input.Active = 0;
    path.Input.VidPnPathUpdates = 3; /* Remove the path, keep the panel connected. */
    input = path.InputFlags;
    CHECK(d.DxgkDdiSetTimingsFromVidPn(&adapter, &args) == STATUS_SUCCESS);
    CHECK(!visible && visibility_calls == 2);
    id = check_result(&path, id, 7, input);

    visibility_status = STATUS_IO_DEVICE_ERROR;
    CHECK(d.DxgkDdiSetTimingsFromVidPn(&adapter, &args) == visibility_status);
    CHECK(path.TargetState.ConnectionChangeId == id && visibility_calls == 3);
    visibility_status = STATUS_SUCCESS;

    path.VidPnTargetId = 8;
    CHECK(d.DxgkDdiSetTimingsFromVidPn(&adapter, &args) == STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_TARGET);
    CHECK(visibility_calls == 3 && path.TargetState.ConnectionChangeId == id);
    adapter.Panel.Boot.TargetId = 8;
    CHECK(d.DxgkDdiSetTimingsFromVidPn(&adapter, &args) == STATUS_SUCCESS);
    (void)check_result(&path, id, 8, input);

    args.PathCount = 0;
    args.pSetTimingPathInfo = NULL;
    results.Value = UINT32_MAX;
    CHECK(d.DxgkDdiSetTimingsFromVidPn(&adapter, &args) == STATUS_SUCCESS && !results.Value);
    CHECK(visibility_calls == 4);
    test_rotation(&adapter,&d);
    ExitProcess(0);
}
