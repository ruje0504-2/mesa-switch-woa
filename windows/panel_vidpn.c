/* SPDX-License-Identifier: MIT */
#include "adapter.h"

typedef struct _PANEL_PATH {
    const DXGK_VIDPN_INTERFACE *VidPn;
    const DXGK_VIDPNTOPOLOGY_INTERFACE *Topology;
    D3DKMDT_HVIDPNTOPOLOGY Handle;
    const D3DKMDT_VIDPN_PRESENT_PATH *Path;
} PANEL_PATH;

enum panel_mode_state {
    PanelModeUnpinned, PanelModeNative, PanelModeUnsupported
};

static NTSTATUS acquire_path(TG_GPU_ADAPTER *adapter, D3DKMDT_HVIDPN vidpn, PANEL_PATH *path)
{
    if (!vidpn)
        return STATUS_SUCCESS;
    const DXGKRNL_INTERFACE *dxgk = &adapter->Engine.Dxgk;
    if (!dxgk->DxgkCbQueryVidPnInterface)
        return STATUS_DEVICE_NOT_READY;
    NTSTATUS status = dxgk->DxgkCbQueryVidPnInterface(vidpn,
        DXGK_VIDPN_INTERFACE_VERSION_V1, &path->VidPn);
    if (!NT_SUCCESS(status))
        return status;
    status = path->VidPn->pfnGetTopology(vidpn, &path->Handle, &path->Topology);
    if (!NT_SUCCESS(status))
        return status;
    SIZE_T count = 0;
    status = path->Topology->pfnGetNumPaths(path->Handle, &count);
    if (!NT_SUCCESS(status) || count == 0)
        return status;
    if (count != 1)
        return STATUS_GRAPHICS_INVALID_VIDPN_TOPOLOGY;
    return path->Topology->pfnAcquireFirstPathInfo(path->Handle, &path->Path);
}

static bool supported_path(const TG_GPU_PANEL *panel, const D3DKMDT_VIDPN_PRESENT_PATH *path)
{
    const D3DKMDT_VIDPN_PRESENT_PATH_TRANSFORMATION *x = &path->ContentTransformation;
    return path->VidPnSourceId == 0 && path->VidPnTargetId == panel->Boot.TargetId &&
        (x->Scaling == D3DKMDT_VPPS_IDENTITY || x->Scaling == D3DKMDT_VPPS_UNPINNED ||
         x->Scaling == D3DKMDT_VPPS_NOTSPECIFIED || x->Scaling == D3DKMDT_VPPS_UNINITIALIZED) &&
        (x->Rotation == D3DKMDT_VPPR_IDENTITY || x->Rotation == D3DKMDT_VPPR_UNPINNED ||
         x->Rotation == D3DKMDT_VPPR_NOTSPECIFIED || x->Rotation == D3DKMDT_VPPR_UNINITIALIZED);
}

static bool same_size(D3DKMDT_2DREGION a, D3DKMDT_2DREGION b)
{
    return a.cx == b.cx && a.cy == b.cy;
}

static bool is_native_source(const D3DKMDT_VIDPN_SOURCE_MODE *mode)
{
    const D3DKMDT_GRAPHICS_RENDERING_FORMAT *g = &mode->Format.Graphics;
    return mode->Type == D3DKMDT_RMT_GRAPHICS &&
        same_size(g->PrimSurfSize, TgGpuPanelSignal.ActiveSize) &&
        same_size(g->VisibleRegionSize, TgGpuPanelSignal.ActiveSize) &&
        g->Stride == T214_DC_PANEL_PITCH &&
        (g->PixelFormat == D3DDDIFMT_X8R8G8B8 || g->PixelFormat == D3DDDIFMT_A8R8G8B8) &&
        g->PixelValueAccessMode == D3DKMDT_PVAM_DIRECT;
}

static bool is_native_target(const D3DKMDT_VIDEO_SIGNAL_INFO *signal)
{
    return same_size(signal->ActiveSize, TgGpuPanelSignal.ActiveSize) &&
        same_size(signal->TotalSize, TgGpuPanelSignal.TotalSize) &&
        signal->PixelRate == TgGpuPanelSignal.PixelRate &&
        signal->VSyncFreq.Denominator != 0 &&
        signal->VSyncFreq.Numerator == 60ull * signal->VSyncFreq.Denominator &&
        signal->AdditionalSignalInfo.ScanLineOrdering == D3DDDI_VSSLO_PROGRESSIVE;
}

static NTSTATUS pinned_source(const DXGK_VIDPN_INTERFACE *api, D3DKMDT_HVIDPN vidpn,
    enum panel_mode_state *state)
{
    const DXGK_VIDPNSOURCEMODESET_INTERFACE *modes;
    D3DKMDT_HVIDPNSOURCEMODESET set;
    NTSTATUS status = api->pfnAcquireSourceModeSet(vidpn, 0, &set, &modes);
    if (!NT_SUCCESS(status))
        return status;
    const D3DKMDT_VIDPN_SOURCE_MODE *mode = NULL;
    status = modes->pfnAcquirePinnedModeInfo(set, &mode);
    if (status == STATUS_GRAPHICS_MODE_NOT_PINNED || (NT_SUCCESS(status) && !mode)) {
        *state = PanelModeUnpinned;
        status = STATUS_SUCCESS;
    } else if (NT_SUCCESS(status)) {
        *state = is_native_source(mode) ? PanelModeNative : PanelModeUnsupported;
        status = modes->pfnReleaseModeInfo(set, mode);
    }
    NTSTATUS release = api->pfnReleaseSourceModeSet(vidpn, set);
    return NT_SUCCESS(status) ? release : status;
}

static NTSTATUS pinned_target(const DXGK_VIDPN_INTERFACE *api, D3DKMDT_HVIDPN vidpn,
    D3DDDI_VIDEO_PRESENT_TARGET_ID target, enum panel_mode_state *state)
{
    const DXGK_VIDPNTARGETMODESET_INTERFACE *modes;
    D3DKMDT_HVIDPNTARGETMODESET set;
    NTSTATUS status = api->pfnAcquireTargetModeSet(vidpn, target, &set, &modes);
    if (!NT_SUCCESS(status))
        return status;
    const D3DKMDT_VIDPN_TARGET_MODE *mode = NULL;
    status = modes->pfnAcquirePinnedModeInfo(set, &mode);
    if (status == STATUS_GRAPHICS_MODE_NOT_PINNED || (NT_SUCCESS(status) && !mode)) {
        *state = PanelModeUnpinned;
        status = STATUS_SUCCESS;
    } else if (NT_SUCCESS(status)) {
        *state = is_native_target(&mode->VideoSignalInfo) ? PanelModeNative : PanelModeUnsupported;
        status = modes->pfnReleaseModeInfo(set, mode);
    }
    NTSTATUS release = api->pfnReleaseTargetModeSet(vidpn, set);
    return NT_SUCCESS(status) ? release : status;
}

static NTSTATUS add_source(const DXGK_VIDPN_INTERFACE *api, D3DKMDT_HVIDPN vidpn,
    D3DDDIFORMAT format)
{
    const DXGK_VIDPNSOURCEMODESET_INTERFACE *modes;
    D3DKMDT_HVIDPNSOURCEMODESET set;
    NTSTATUS status = api->pfnCreateNewSourceModeSet(vidpn, 0, &set, &modes);
    if (!NT_SUCCESS(status))
        return status;
    D3DKMDT_VIDPN_SOURCE_MODE *mode;
    status = modes->pfnCreateNewModeInfo(set, &mode);
    if (NT_SUCCESS(status)) {
        mode->Type = D3DKMDT_RMT_GRAPHICS;
        mode->Format.Graphics = (D3DKMDT_GRAPHICS_RENDERING_FORMAT){
            .PrimSurfSize = TgGpuPanelSignal.ActiveSize,
            .VisibleRegionSize = TgGpuPanelSignal.ActiveSize,
            .Stride = T214_DC_PANEL_PITCH,
            .PixelFormat = format,
            .ColorBasis = D3DKMDT_CB_SRGB,
            .PixelValueAccessMode = D3DKMDT_PVAM_DIRECT,
        };
        status = modes->pfnAddMode(set, mode);
        if (!NT_SUCCESS(status))
            (VOID)modes->pfnReleaseModeInfo(set, mode);
    }
    if (NT_SUCCESS(status))
        status = api->pfnAssignSourceModeSet(vidpn, 0, set);
    if (!NT_SUCCESS(status))
        (VOID)api->pfnReleaseSourceModeSet(vidpn, set);
    return status;
}

static NTSTATUS add_target(const DXGK_VIDPN_INTERFACE *api, D3DKMDT_HVIDPN vidpn,
    D3DDDI_VIDEO_PRESENT_TARGET_ID target)
{
    const DXGK_VIDPNTARGETMODESET_INTERFACE *modes;
    D3DKMDT_HVIDPNTARGETMODESET set;
    NTSTATUS status = api->pfnCreateNewTargetModeSet(vidpn, target, &set, &modes);
    if (!NT_SUCCESS(status))
        return status;
    D3DKMDT_VIDPN_TARGET_MODE *mode;
    status = modes->pfnCreateNewModeInfo(set, &mode);
    if (NT_SUCCESS(status)) {
        mode->VideoSignalInfo = TgGpuPanelSignal;
        mode->Preference = D3DKMDT_MP_PREFERRED;
        status = modes->pfnAddMode(set, mode);
        if (!NT_SUCCESS(status))
            (VOID)modes->pfnReleaseModeInfo(set, mode);
    }
    if (NT_SUCCESS(status))
        status = api->pfnAssignTargetModeSet(vidpn, target, set);
    if (!NT_SUCCESS(status))
        (VOID)api->pfnReleaseTargetModeSet(vidpn, set);
    return status;
}

static NTSTATUS APIENTRY is_supported(HANDLE context, DXGKARG_ISSUPPORTEDVIDPN *args)
{
    TG_GPU_ADAPTER *adapter = context;
    PAGED_CODE();
    if (!adapter || !args)
        return STATUS_INVALID_PARAMETER;
    args->IsVidPnSupported = FALSE;
    if (!adapter->Panel.Dc.owned)
        return STATUS_DEVICE_NOT_READY;
    PANEL_PATH path = {0};
    NTSTATUS status = acquire_path(adapter, args->hDesiredVidPn, &path);
    if (status == STATUS_GRAPHICS_INVALID_VIDPN_TOPOLOGY)
        return STATUS_SUCCESS;
    if (!NT_SUCCESS(status))
        return status;
    if (!path.Path) {
        args->IsVidPnSupported = TRUE; /* No output is a valid topology. */
        return STATUS_SUCCESS;
    }

    if (supported_path(&adapter->Panel, path.Path)) {
        enum panel_mode_state source, target;
        status = pinned_source(path.VidPn, args->hDesiredVidPn, &source);
        if (NT_SUCCESS(status))
            status = pinned_target(path.VidPn, args->hDesiredVidPn, path.Path->VidPnTargetId, &target);
        if (NT_SUCCESS(status))
            args->IsVidPnSupported = source != PanelModeUnsupported && target != PanelModeUnsupported;
    }
    NTSTATUS release = path.Topology->pfnReleasePathInfo(path.Handle, path.Path);
    return NT_SUCCESS(status) ? release : status;
}

static NTSTATUS APIENTRY enumerate_modes(HANDLE context, const DXGKARG_ENUMVIDPNCOFUNCMODALITY *args)
{
    TG_GPU_ADAPTER *adapter = context;
    PAGED_CODE();
    if (!adapter || !args)
        return STATUS_INVALID_PARAMETER;
    if (!adapter->Panel.Dc.owned)
        return STATUS_DEVICE_NOT_READY;
    D3DKMDT_HVIDPN vidpn = args->hConstrainingVidPn;
    PANEL_PATH path = {0};
    NTSTATUS status = acquire_path(adapter, vidpn, &path);
    if (!NT_SUCCESS(status) || !path.Path)
        return status;
    D3DDDI_VIDEO_PRESENT_TARGET_ID target = path.Path->VidPnTargetId;
    if (!supported_path(&adapter->Panel, path.Path)) {
        status = STATUS_GRAPHICS_INVALID_VIDPN_TOPOLOGY;
        goto done;
    }

    enum panel_mode_state source_mode, target_mode;
    status = pinned_source(path.VidPn, vidpn, &source_mode);
    if (NT_SUCCESS(status))
        status = pinned_target(path.VidPn, vidpn, target, &target_mode);
    if (!NT_SUCCESS(status))
        goto done;
    if (source_mode == PanelModeUnsupported || target_mode == PanelModeUnsupported) {
        status = STATUS_GRAPHICS_INVALID_VIDPN_TOPOLOGY;
        goto done;
    }

    const bool source_pivot = args->EnumPivotType == D3DKMDT_EPT_VIDPNSOURCE &&
        args->EnumPivot.VidPnSourceId == 0;
    const bool target_pivot = args->EnumPivotType == D3DKMDT_EPT_VIDPNTARGET &&
        args->EnumPivot.VidPnTargetId == target;
    if (source_mode == PanelModeUnpinned && !source_pivot)
        status = add_source(path.VidPn, vidpn, adapter->Panel.Boot.ColorFormat);
    if (NT_SUCCESS(status) && target_mode == PanelModeUnpinned && !target_pivot)
        status = add_target(path.VidPn, vidpn, target);
    if (!NT_SUCCESS(status))
        goto done;

    D3DKMDT_VIDPN_PRESENT_PATH update = *path.Path;
    const bool path_pivot = (args->EnumPivotType == D3DKMDT_EPT_SCALING ||
        args->EnumPivotType == D3DKMDT_EPT_ROTATION) &&
        args->EnumPivot.VidPnSourceId == 0 && args->EnumPivot.VidPnTargetId == target;
    if (args->EnumPivotType != D3DKMDT_EPT_SCALING || !path_pivot)
        update.ContentTransformation.ScalingSupport =
            (D3DKMDT_VIDPN_PRESENT_PATH_SCALING_SUPPORT){.Identity = 1};
    if (args->EnumPivotType != D3DKMDT_EPT_ROTATION || !path_pivot)
        update.ContentTransformation.RotationSupport =
            (D3DKMDT_VIDPN_PRESENT_PATH_ROTATION_SUPPORT){.Identity = 1, .Offset0 = 1};
    status = path.Topology->pfnUpdatePathSupportInfo(path.Handle, &update);
done:
    {
        NTSTATUS release = path.Topology->pfnReleasePathInfo(path.Handle, path.Path);
        return NT_SUCCESS(status) ? release : status;
    }
}

void TgGpuRegisterPanelVidPn(DRIVER_INITIALIZATION_DATA *ddis)
{
    ddis->DxgkDdiIsSupportedVidPn = is_supported;
    ddis->DxgkDdiEnumVidPnCofuncModality = enumerate_modes;
}
