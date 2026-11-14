/* SPDX-License-Identifier: MIT */
#include "adapter.h"
#include "allocation.h"
#include "../platform/t214_resources.h"

typedef struct _FLIP_ACTION {
    TG_GPU_PANEL *Panel;
    TG_GPU_PANEL_FLIP Flip;
    NTSTATUS Status;
} FLIP_ACTION;

static bool matches(const TG_GPU_PANEL_FLIP *flip, const struct tg_t214_dc_scanout *active)
{
    return flip->Address == active->address && flip->Rotation == active->rotation &&
        (flip->Visible != FALSE) == active->visible;
}

BOOLEAN TgGpuPanelReadLatched(TG_GPU_PANEL *panel, struct tg_t214_dc_scanout *active)
{
    if (!tg_t214_dc_latched_surface(&panel->Dc, active))
        return FALSE;
    if (panel->Pending.Address && matches(&panel->Pending, active)) {
        panel->Displayed = panel->Pending;
        panel->Pending = (TG_GPU_PANEL_FLIP){0};
    } else if (!matches(&panel->Displayed, active)) {
        /* Unknown/external state must not inherit a completed PresentId. */
        panel->Displayed = (TG_GPU_PANEL_FLIP){.Address = active->address,
            .Rotation = active->rotation, .Visible = active->visible};
    }
    return TRUE;
}

static BOOLEAN program_flip(PVOID context)
{
    FLIP_ACTION *action = context;
    TG_GPU_PANEL *panel = action->Panel;
    if (!panel->Dc.owned || panel->Stopping) {
        action->Status = STATUS_DEVICE_NOT_READY;
        return TRUE;
    }
    struct tg_t214_dc_scanout active;
    const BOOLEAN latched = TgGpuPanelReadLatched(panel, &active);
    if (panel->Pending.Address) {
        action->Status = STATUS_DEVICE_BUSY;
        return TRUE;
    }
    TG_GPU_PANEL_FLIP flip = action->Flip;
    if (!flip.Address) {
        /* A disabled MPO plane supplies no allocation. Preserve its binding
         * so source visibility can re-enable the primary without another flip. */
        if (flip.Visible || !latched) {
            action->Status = STATUS_INVALID_PARAMETER;
            return TRUE;
        }
        flip.Address = active.address;
        flip.Rotation = active.rotation;
    }
    flip.Visible = flip.Visible && panel->SourceVisible;
    if (flip.Immediate && latched && matches(&flip, &active) &&
        panel->Displayed.PresentId==flip.PresentId) {
        action->Status=STATUS_SUCCESS;
        return TRUE;
    }
    const struct tg_t214_dc_scanout surface = {flip.Address,
        flip.Rotation, flip.Visible != FALSE};
    enum tg_t214_dc_result result = tg_t214_dc_queue_surface(&panel->Dc, &surface,
        flip.Immediate != FALSE);
    if (result == TG_T214_DC_OK) {
        panel->Pending = flip;
        action->Flip = flip; /* Confirm the effective visibility after queuing. */
        action->Status = flip.Immediate ? STATUS_PENDING : STATUS_SUCCESS;
    } else {
        action->Status = result == TG_T214_DC_BUSY ? STATUS_DEVICE_BUSY : STATUS_INVALID_PARAMETER;
    }
    return TRUE;
}

static BOOLEAN confirm_flip(PVOID context)
{
    FLIP_ACTION *action=context;
    struct tg_t214_dc_scanout active;
    action->Status=STATUS_PENDING;
    if (TgGpuPanelReadLatched(action->Panel, &active) && matches(&action->Flip, &active)) {
        action->Panel->Displayed=action->Flip;
        action->Panel->Pending=(TG_GPU_PANEL_FLIP){0};
        action->Status=STATUS_SUCCESS;
    }
    return TRUE;
}

static NTSTATUS flip(TG_GPU_ADAPTER *adapter, TG_GPU_PANEL_FLIP scanout, BOOLEAN mode_change)
{
    FLIP_ACTION action = {&adapter->Panel, scanout, STATUS_UNSUCCESSFUL};
    /* A modeset has no scheduled flip receipt to retire the previous primary.
     * Keep the PASSIVE-level call alive until its address and layout are active.
     * Ordinary MMIO flips retain their asynchronous VSync notification path. */
    if (mode_change && KeGetCurrentIrql() != PASSIVE_LEVEL)
        return STATUS_INVALID_PARAMETER;
    /* DxgKrnl calls the fast flip path in the interrupt synchronization
     * class. Legacy modeset and pre-present calls can arrive at lower IRQL.
     */
    if (KeGetCurrentIrql() > DISPATCH_LEVEL) {
        (VOID)program_flip(&action);
        return action.Status;
    }
    NTSTATUS status = TgGpuPanelSynchronize(adapter, program_flip, &action);
    if (!NT_SUCCESS(status))return status;
    if (!mode_change && (!scanout.Immediate || action.Status!=STATUS_PENDING))
        return action.Status;
    BOOLEAN queued = action.Status == STATUS_SUCCESS || action.Status == STATUS_PENDING;
    if (!queued && (!mode_change || action.Status != STATUS_DEVICE_BUSY))
        return action.Status;
    /* Immediate MPO requests are recalled at PASSIVE before queuing. Keep
     * their DDI lifetime until the active bank confirms completion. Address
     * changes usually take one scanline; layout/visibility changes require VBlank.
     * A modeset also drains a previously queued flip before replacing it. Its
     * Immediate bit remains clear, so even address-only modesets use VBlank. */
    const ULONGLONG deadline=KeQueryInterruptTime()+500000;
    LARGE_INTEGER interval={.QuadPart=-10000};
    for (UINT i=0;KeQueryInterruptTime()<deadline;++i) {
        if(i<100)KeStallExecutionProcessor(1);
        else (VOID)KeDelayExecutionThread(KernelMode,FALSE,&interval);
        status=TgGpuPanelSynchronize(adapter,queued ? confirm_flip : program_flip,&action);
        if(!NT_SUCCESS(status))return status;
        if (queued) {
            if(action.Status==STATUS_SUCCESS)return STATUS_SUCCESS;
        } else if (action.Status == STATUS_SUCCESS || action.Status == STATUS_PENDING) {
            queued = TRUE; /* Queuing is not a latch/completion receipt. */
        } else if (action.Status != STATUS_DEVICE_BUSY) {
            return action.Status;
        }
    }
    return mode_change ? STATUS_IO_TIMEOUT : STATUS_INVALID_PARAMETER;
}

static bool allocation_rotation(UINT rotation, enum tg_t214_dc_rotation *physical)
{
    /* Legacy primary orientation follows the VidPN/GDI counter-clockwise
     * convention. NONPREROTATED producers describe identity storage. */
    switch (rotation) {
    case D3DDDI_ROTATION_IDENTITY: *physical = TG_T214_DC_IDENTITY; return true;
    case D3DDDI_ROTATION_90: *physical = TG_T214_DC_CCW90; return true;
    case D3DDDI_ROTATION_180: *physical = TG_T214_DC_CCW180; return true;
    case D3DDDI_ROTATION_270: *physical = TG_T214_DC_CCW270; return true;
    default: return false;
    }
}

static bool storage_matches(const TG_GPU_PRIMARY_ALLOCATION_REQUEST *allocation,
    enum tg_t214_dc_rotation rotation)
{
    struct tg_t214_dc_layout layout;
    return tg_t214_dc_get_layout(rotation, &layout) &&
        allocation->Width == layout.width && allocation->Height == layout.height &&
        allocation->Pitch == layout.pitch;
}

static NTSTATUS APIENTRY set_primary(HANDLE context, const DXGKARG_SETVIDPNSOURCEADDRESS *args)
{
    TG_GPU_ADAPTER *adapter = context;
    if (!adapter || !args || args->VidPnSourceId != 0)
        return STATUS_INVALID_PARAMETER;
    if (args->Flags.FlipImmediate || args->Flags.FlipStereo || args->ContextCount > 1)
        return STATUS_NOT_SUPPORTED;
    TG_GPU_PANEL_FLIP scanout = {.Address = T214_GPU_POST_FB_BASE, .Visible = TRUE};
    if (args->hAllocation) {
        if (args->PrimarySegment != TG_GPU_SCANOUT_SEGMENT)
            return STATUS_NOT_SUPPORTED;
        TG_GPU_PRIMARY_ALLOCATION_REQUEST allocation;
        NTSTATUS status = TgGpuGetScanoutSurface(&adapter->Engine, args->hAllocation, &allocation);
        if (!NT_SUCCESS(status))
            return status;
        if (!allocation_rotation(allocation.Rotation, &scanout.Rotation) ||
            !storage_matches(&allocation, scanout.Rotation))
            return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE;
        status = TgGpuResolveScanoutAddress(&adapter->Tables,
            (UINT64)args->PrimaryAddress.QuadPart, T214_GPU_POST_FB_SIZE, &scanout.Address);
        if (!NT_SUCCESS(status))
            return status;
    } else if (!args->Flags.ModeChange) {
        return STATUS_INVALID_PARAMETER;
    }
    return flip(adapter, scanout, args->Flags.ModeChange != 0);
}

static bool full_surface(const RECT *rect, UINT width, UINT height)
{
    return rect->left == 0 && rect->top == 0 &&
        rect->right == (LONG)width && rect->bottom == (LONG)height;
}

static bool native_plane(const DXGK_MULTIPLANE_OVERLAY_ATTRIBUTES3 *plane,
    const TG_GPU_PRIMARY_ALLOCATION_REQUEST *allocation, enum tg_t214_dc_rotation *rotation)
{
    /* MPO already supplies the plane's required clockwise transform. Do not
     * add the VidPN or the allocation's mode orientation a second time. */
    switch (plane->Rotation) {
    case D3DDDI_ROTATION_IDENTITY: *rotation = TG_T214_DC_IDENTITY; break;
    case D3DDDI_ROTATION_90: *rotation = TG_T214_DC_CCW270; break;
    case D3DDDI_ROTATION_180: *rotation = TG_T214_DC_CCW180; break;
    case D3DDDI_ROTATION_270: *rotation = TG_T214_DC_CCW90; break;
    default: return false;
    }
    return storage_matches(allocation, *rotation) && !plane->Flags.Value &&
        full_surface(&plane->SrcRect, allocation->Width, allocation->Height) &&
        full_surface(&plane->DstRect, T214_DC_PANEL_WIDTH, T214_DC_PANEL_HEIGHT) &&
        (full_surface(&plane->ClipRect, T214_DC_PANEL_WIDTH, T214_DC_PANEL_HEIGHT) ||
            (!plane->ClipRect.left && !plane->ClipRect.top &&
             !plane->ClipRect.right && !plane->ClipRect.bottom)) &&
        !plane->Blend.Value &&
        plane->ColorSpaceType == D3DDDI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
}

static NTSTATUS APIENTRY set_primary_mpo(HANDLE context,
    DXGKARG_SETVIDPNSOURCEADDRESSWITHMULTIPLANEOVERLAY3 *args)
{
    TG_GPU_ADAPTER *adapter = context;
    if (!adapter || !args || args->VidPnSourceId != 0 || args->PlaneCount > 1 ||
        (args->PlaneCount && (!args->ppPlanes || !args->ppPlanes[0])))
        return STATUS_INVALID_PARAMETER;
    args->OutputFlags.Value = 0;
    if (args->InputFlags.FlipStereo || args->pPostComposition || args->pHDRMetaData)
        return STATUS_NOT_SUPPORTED;
    if (!args->PlaneCount)
        return STATUS_SUCCESS; /* Omitted planes keep their existing state. */

    DXGK_MULTIPLANE_OVERLAY_PLANE3 *plane = args->ppPlanes[0];
    plane->OutputFlags.Value = 0;
    if (plane->LayerIndex != 0 || plane->InputFlags.FlipImmediateNoTearing)
        return STATUS_INVALID_PARAMETER;
    TG_GPU_PANEL_FLIP scanout = {
        .PresentId = plane->PresentId,
        .Visible = plane->InputFlags.Enabled != 0,
        .Immediate = plane->InputFlags.FlipImmediate != 0,
    };
    if (scanout.Immediate && KeGetCurrentIrql()!=PASSIVE_LEVEL) {
        args->OutputFlags.PrePresentNeeded=1;
        return STATUS_RETRY;
    }
    if (scanout.Visible) {
        if (plane->ContextCount != 1 ||
            !plane->ppContextData || !plane->ppContextData[0])
            return STATUS_INVALID_PARAMETER;
        const DXGK_PRIMARYCONTEXTDATA *primary = plane->ppContextData[0];
        if (!primary->hAllocation || primary->SegmentId != TG_GPU_SCANOUT_SEGMENT)
            return STATUS_NOT_SUPPORTED;
        TG_GPU_PRIMARY_ALLOCATION_REQUEST allocation;
        NTSTATUS status = TgGpuGetScanoutSurface(&adapter->Engine, primary->hAllocation, &allocation);
        if (!NT_SUCCESS(status))
            return status;
        if (!native_plane(&plane->PlaneAttributes, &allocation, &scanout.Rotation))
            return STATUS_INVALID_PARAMETER;
        status = TgGpuResolveScanoutAddress(&adapter->Tables,
            (UINT64)primary->SegmentAddress.QuadPart, T214_GPU_POST_FB_SIZE, &scanout.Address);
        if (!NT_SUCCESS(status))
            return status;
    }
    NTSTATUS status=flip(adapter, scanout, FALSE);
    return status==STATUS_DEVICE_BUSY?STATUS_RETRY:status;
}

static NTSTATUS APIENTRY check_mpo(HANDLE context, DXGKARG_CHECKMULTIPLANEOVERLAYSUPPORT3 *args)
{
    PAGED_CODE();
    if (!context || !args || (args->PlaneCount && !args->ppPlanes))
        return STATUS_INVALID_PARAMETER;
    args->ReturnInfo.Value = 0;
    args->Supported = FALSE;
    if (args->PostCompositionCount || args->PlaneCount > 1)
        return STATUS_SUCCESS;
    if (!args->PlaneCount) {
        args->Supported = TRUE;
        return STATUS_SUCCESS;
    }
    const DXGK_MULTIPLANE_OVERLAY_PLANE_WITH_SOURCE2 *plane = args->ppPlanes[0];
    if (plane && plane->hAllocation && !plane->LayerIndex && !plane->VidPnSourceId) {
        TG_GPU_ADAPTER *adapter = context;
        TG_GPU_PRIMARY_ALLOCATION_REQUEST allocation;
        enum tg_t214_dc_rotation rotation;
        if (NT_SUCCESS(TgGpuGetScanoutSurface(&adapter->Engine, plane->hAllocation, &allocation)))
            args->Supported = native_plane(&plane->PlaneAttributes, &allocation, &rotation);
    }
    return STATUS_SUCCESS;
}

typedef struct _VBLANK_ACTION {
    TG_GPU_PANEL *Panel;
    DXGK_INTERRUPT_TYPE Type;
    BOOLEAN Enable;
} VBLANK_ACTION;

static BOOLEAN control_vblank(PVOID context)
{
    const VBLANK_ACTION *action = context;
    TG_GPU_PANEL *panel = action->Panel;
    if (action->Type == DXGK_INTERRUPT_CRTC_VSYNC)
        panel->NotifyVBlank = action->Enable;
    else
        panel->NotifyMpoVBlank = action->Enable;
    tg_t214_dc_set_vblank(&panel->Dc, !panel->Stopping &&
        (panel->NotifyVBlank || panel->NotifyMpoVBlank));
    return TRUE;
}

static NTSTATUS APIENTRY control_interrupt(HANDLE context, DXGK_INTERRUPT_TYPE type, BOOLEAN enable)
{
    TG_GPU_ADAPTER *adapter = context;
    PAGED_CODE();
    if (!adapter)
        return STATUS_INVALID_PARAMETER;
    if (type != DXGK_INTERRUPT_CRTC_VSYNC &&
        type != DXGK_INTERRUPT_CRTC_VSYNC_WITH_MULTIPLANE_OVERLAY2)
        return STATUS_NOT_SUPPORTED;
    if (!adapter->Panel.Dc.owned)
        return enable ? STATUS_DEVICE_NOT_READY : STATUS_SUCCESS;
    VBLANK_ACTION action = {&adapter->Panel, type, enable};
    return TgGpuPanelSynchronize(adapter, control_vblank, &action);
}

static BOOLEAN panel_interrupt(PVOID context, ULONG message)
{
    TG_GPU_ADAPTER *adapter = context;
    TG_GPU_PANEL *panel = &adapter->Panel;
    if (message || !tg_t214_dc_ack_vblank(&panel->Dc))
        return FALSE;

    struct tg_t214_dc_scanout active;
    (VOID)TgGpuPanelReadLatched(panel, &active);

    const DXGKRNL_INTERFACE *dxgk = &adapter->Engine.Dxgk;
    if (panel->NotifyMpoVBlank) {
        DXGK_MULTIPLANE_OVERLAY_VSYNC_INFO2 plane = {.PresentId = panel->Displayed.PresentId};
        DXGKARGCB_NOTIFY_INTERRUPT_DATA data = {
            .InterruptType = DXGK_INTERRUPT_CRTC_VSYNC_WITH_MULTIPLANE_OVERLAY2,
            .CrtcVsyncWithMultiPlaneOverlay2 = {
                .VidPnTargetId = panel->Boot.TargetId,
                .PhysicalAdapterMask = 1,
                .MultiPlaneOverlayVsyncInfoCount = plane.PresentId ? 1 : 0,
                .pMultiPlaneOverlayVsyncInfo = plane.PresentId ? &plane : NULL,
            },
        };
        dxgk->DxgkCbNotifyInterrupt(dxgk->DeviceHandle, &data);
    }
    if (panel->NotifyVBlank) {
        const UINT64 displayed = panel->Displayed.Address;
        DXGKARGCB_NOTIFY_INTERRUPT_DATA data = {
            .InterruptType = DXGK_INTERRUPT_CRTC_VSYNC,
            .CrtcVsync = {
                .VidPnTargetId = panel->Boot.TargetId,
                .PhysicalAddress = {.QuadPart = (LONGLONG)displayed},
            },
        };
        dxgk->DxgkCbNotifyInterrupt(dxgk->DeviceHandle, &data);
    }
    if (panel->NotifyMpoVBlank || panel->NotifyVBlank)
        (VOID)dxgk->DxgkCbQueueDpc(dxgk->DeviceHandle);
    return TRUE;
}

static NTSTATUS APIENTRY get_scanline(HANDLE context, DXGKARG_GETSCANLINE *args)
{
    const TG_GPU_ADAPTER *adapter = context;
    PAGED_CODE();
    if (!adapter || !args)
        return STATUS_INVALID_PARAMETER;
    if (args->VidPnTargetId != adapter->Panel.Boot.TargetId)
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_TARGET;
    /* VBlank IRQs prove frame boundaries. They do not read a raster counter. */
    return STATUS_NOT_SUPPORTED;
}

/* With zero hardware pointer caps, Windows composites the software cursor.
 * Keep the display DDI pair available for a hardware-pointer hide request.
 */
static NTSTATUS APIENTRY set_pointer_position(HANDLE context, const DXGKARG_SETPOINTERPOSITION *args)
{
    PAGED_CODE();
    if (!context || !args)
        return STATUS_INVALID_PARAMETER;
    if (args->VidPnSourceId != 0)
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE;
    return args->Flags.Visible ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;
}

static NTSTATUS APIENTRY set_pointer_shape(HANDLE context, const DXGKARG_SETPOINTERSHAPE *args)
{
    PAGED_CODE();
    if (!context || !args)
        return STATUS_INVALID_PARAMETER;
    if (args->VidPnSourceId != 0)
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE;
    return STATUS_NOT_IMPLEMENTED;
}

void TgGpuRegisterPanelScanout(DRIVER_INITIALIZATION_DATA *ddis)
{
    ddis->DxgkDdiInterruptRoutine = panel_interrupt;
    ddis->DxgkDdiControlInterrupt = control_interrupt;
    ddis->DxgkDdiGetScanLine = get_scanline;
    ddis->DxgkDdiSetPointerPosition = set_pointer_position;
    ddis->DxgkDdiSetPointerShape = set_pointer_shape;
    ddis->DxgkDdiSetVidPnSourceAddress = set_primary;
    ddis->DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay3 = set_primary_mpo;
    ddis->DxgkDdiCheckMultiPlaneOverlaySupport3 = check_mpo;
}
