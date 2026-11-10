/* SPDX-License-Identifier: MIT */
#include "adapter.h"
#include "../platform/t214_resources.h"

typedef struct _FLIP_ACTION {
    TG_GPU_PANEL *Panel;
    TG_GPU_PANEL_FLIP Flip;
    NTSTATUS Status;
} FLIP_ACTION;

static BOOLEAN program_flip(PVOID context)
{
    FLIP_ACTION *action = context;
    TG_GPU_PANEL *panel = action->Panel;
    if (!panel->Dc.owned || panel->Stopping) {
        action->Status = STATUS_DEVICE_NOT_READY;
        return TRUE;
    }
    if (panel->Pending.PresentId) {
        action->Status = STATUS_DEVICE_BUSY;
        return TRUE;
    }
    enum tg_t214_dc_result result = tg_t214_dc_queue_scanout(&panel->Dc,
        action->Flip.Address, action->Flip.Visible != FALSE);
    if (result == TG_T214_DC_OK) {
        panel->Pending = action->Flip;
        action->Status = STATUS_SUCCESS;
    } else {
        action->Status = result == TG_T214_DC_BUSY ? STATUS_DEVICE_BUSY : STATUS_INVALID_PARAMETER;
    }
    return TRUE;
}

static NTSTATUS flip(TG_GPU_ADAPTER *adapter, TG_GPU_PANEL_FLIP scanout)
{
    FLIP_ACTION action = {&adapter->Panel, scanout, STATUS_UNSUCCESSFUL};
    /* DxgKrnl calls the fast flip path in the interrupt synchronization
     * class. Legacy modeset and pre-present calls can arrive at lower IRQL.
     */
    if (KeGetCurrentIrql() > DISPATCH_LEVEL) {
        (VOID)program_flip(&action);
        return action.Status;
    }
    NTSTATUS status = TgGpuPanelSynchronize(adapter, program_flip, &action);
    return NT_SUCCESS(status) ? action.Status : status;
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
        NTSTATUS status = TgGpuResolveScanoutSegment(&adapter->Tables,
            (UINT64)args->PrimaryAddress.QuadPart, T214_GPU_POST_FB_SIZE, &scanout.Address);
        if (!NT_SUCCESS(status))
            return status;
    } else if (!args->Flags.ModeChange) {
        return STATUS_INVALID_PARAMETER;
    }
    return flip(adapter, scanout);
}

static bool full_panel(const RECT *rect)
{
    return rect->left == 0 && rect->top == 0 &&
        rect->right == T214_DC_PANEL_WIDTH && rect->bottom == T214_DC_PANEL_HEIGHT;
}

static bool native_plane(const DXGK_MULTIPLANE_OVERLAY_ATTRIBUTES3 *plane)
{
    return !plane->Flags.Value && full_panel(&plane->SrcRect) && full_panel(&plane->DstRect) &&
        (full_panel(&plane->ClipRect) ||
            (!plane->ClipRect.left && !plane->ClipRect.top &&
             !plane->ClipRect.right && !plane->ClipRect.bottom)) &&
        plane->Rotation == D3DDDI_ROTATION_IDENTITY &&
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
    if (plane->LayerIndex != 0 || plane->InputFlags.FlipImmediate ||
        plane->InputFlags.FlipImmediateNoTearing)
        return STATUS_NOT_SUPPORTED;
    TG_GPU_PANEL_FLIP scanout = {
        .Address = T214_GPU_POST_FB_BASE,
        .PresentId = plane->PresentId,
        .Visible = plane->InputFlags.Enabled != 0,
    };
    if (scanout.Visible) {
        if (!native_plane(&plane->PlaneAttributes) || plane->ContextCount != 1 ||
            !plane->ppContextData || !plane->ppContextData[0])
            return STATUS_INVALID_PARAMETER;
        const DXGK_PRIMARYCONTEXTDATA *primary = plane->ppContextData[0];
        if (!primary->hAllocation || primary->SegmentId != TG_GPU_SCANOUT_SEGMENT)
            return STATUS_NOT_SUPPORTED;
        NTSTATUS status = TgGpuResolveScanoutSegment(&adapter->Tables,
            (UINT64)primary->SegmentAddress.QuadPart, T214_GPU_POST_FB_SIZE, &scanout.Address);
        if (!NT_SUCCESS(status))
            return status;
    }
    return flip(adapter, scanout);
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
    if (plane && plane->hAllocation && !plane->LayerIndex && !plane->VidPnSourceId)
        args->Supported = native_plane(&plane->PlaneAttributes);
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

    UINT64 address;
    bool visible;
    if (tg_t214_dc_latched_scanout(&panel->Dc, &address, &visible)) {
        if (address == panel->Pending.Address && visible == (panel->Pending.Visible != FALSE)) {
            panel->Displayed = panel->Pending;
            panel->Pending = (TG_GPU_PANEL_FLIP){0};
        } else if (address != panel->Displayed.Address || visible != (panel->Displayed.Visible != FALSE)) {
            panel->Displayed = (TG_GPU_PANEL_FLIP){0};
        }
    }

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
        DXGKARGCB_NOTIFY_INTERRUPT_DATA data = {
            .InterruptType = DXGK_INTERRUPT_CRTC_VSYNC,
            .CrtcVsync = {.VidPnTargetId = panel->Boot.TargetId},
        };
        address = tg_t214_dc_active_address(&panel->Dc);
        const UINT64 base = (UINT64)adapter->Tables.Scanout.SystemPhysicalAddress.QuadPart;
        data.CrtcVsync.PhysicalAddress.QuadPart = address >= base &&
            address - base < adapter->Tables.Scanout.Bytes ? (LONGLONG)(address - base) : 0;
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
