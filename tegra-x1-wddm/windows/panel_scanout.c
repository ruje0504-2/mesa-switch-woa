/* SPDX-License-Identifier: MIT */
#include "adapter.h"
#include "allocation.h"
#include "panel_snapshot.h"
#include "../platform/t214_resources.h"

typedef struct _FLIP_ACTION {
    TG_GPU_PANEL *Panel;
    TG_GPU_PANEL_FLIP Flip;
    NTSTATUS Status;
} FLIP_ACTION;

static bool matches(const TG_GPU_PANEL_FLIP *flip, const struct tg_t214_dc_scanout *active)
{
    return flip->Address == active->address &&
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
            .Visible = active->visible};
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
    }
    flip.Visible = flip.Visible && panel->SourceVisible && panel->TargetActive;
    if (flip.Immediate && latched && matches(&flip, &active) &&
        panel->Displayed.PresentId==flip.PresentId) {
        action->Status=STATUS_SUCCESS;
        return TRUE;
    }
    const struct tg_t214_dc_scanout surface = {flip.Address, flip.Visible != FALSE};
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

static bool storage_matches(const TG_GPU_PRIMARY_ALLOCATION_REQUEST *allocation)
{
    return allocation->Rotation == D3DDDI_ROTATION_IDENTITY &&
        allocation->Width == T214_DC_PANEL_WIDTH && allocation->Height == T214_DC_PANEL_HEIGHT &&
        allocation->Pitch == T214_DC_PANEL_PITCH;
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
        if (!storage_matches(&allocation))
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
    const TG_GPU_PRIMARY_ALLOCATION_REQUEST *allocation)
{
    /* The UMD has already rotated pixels into portrait storage. */
    return plane->Rotation == D3DDDI_ROTATION_IDENTITY && storage_matches(allocation) &&
        !plane->Flags.Value &&
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
        if (!native_plane(&plane->PlaneAttributes, &allocation))
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
        if (NT_SUCCESS(TgGpuGetScanoutSurface(&adapter->Engine, plane->hAllocation, &allocation)))
            args->Supported = native_plane(&plane->PlaneAttributes, &allocation);
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

typedef struct _SNAPSHOT_ACTION {
    TG_GPU_ADAPTER *Adapter;
    struct tg_gpu_panel_snapshot Data;
    NTSTATUS Status;
} SNAPSHOT_ACTION;

static BOOLEAN snapshot_panel(PVOID context)
{
    SNAPSHOT_ACTION *action = context;
    TG_GPU_ADAPTER *adapter = action->Adapter;
    TG_GPU_PANEL *panel = &adapter->Panel;
    struct tg_gpu_panel_snapshot *s = &action->Data;
    if (!panel->Dc.owned || panel->Stopping) {
        action->Status = STATUS_DEVICE_NOT_READY;
        return TRUE;
    }
    const struct tg_t214_dc_io *io = &panel->Dc.io;
    s->luid_low = adapter->Engine.AdapterLuid.LowPart;
    s->luid_high = adapter->Engine.AdapterLuid.HighPart;
    s->path_rotation = panel->PathRotation;
    s->source_visible = panel->SourceVisible;
    s->target_active = panel->TargetActive;
    s->pending_address = panel->Pending.Address;
    s->pending_present = panel->Pending.PresentId;
    s->displayed_address = panel->Displayed.Address;
    s->displayed_present = panel->Displayed.PresentId;
    s->state_control_before = io->read32(io->context, T214_DC_STATE_CONTROL);
    const UINT access = io->read32(io->context, T214_DC_STATE_ACCESS);
    io->write32(io->context, T214_DC_STATE_ACCESS, access | T214_DC_READ_ACTIVE);
    s->address = (UINT64)io->read32(io->context, T214_DC_WIN_A_ADDR) |
        (UINT64)io->read32(io->context, T214_DC_WIN_A_ADDR_HI) << 32;
    s->options = io->read32(io->context, T214_DC_WIN_A_OPTIONS);
    s->pitch = io->read32(io->context, T214_DC_WIN_A_STRIDE) & 0xffffu;
    s->h_offset = io->read32(io->context, T214_DC_WIN_A_H_OFFSET);
    s->v_offset = io->read32(io->context, T214_DC_WIN_A_V_OFFSET);
    s->surface_kind = io->read32(io->context, T214_DC_WIN_A_SURFACE_KIND);
    s->output_size = io->read32(io->context, T214_DC_WIN_A_SIZE);
    s->prescaled_size = io->read32(io->context, T214_DC_WIN_A_PRESCALED);
    s->dda = io->read32(io->context, T214_DC_WIN_A_DDA);
    s->panel_active = io->read32(io->context, T214_DC_ACTIVE_SIZE);
    io->write32(io->context, T214_DC_STATE_ACCESS, access);
    (void)io->read32(io->context, T214_DC_STATE_ACCESS);
    s->interrupt_status = io->read32(io->context, T214_DC_INT_STATUS);
    s->underflow = io->read32(io->context, T214_DC_WIN_A_UNDERFLOW);
    /* Never acknowledge an interrupt, advance Pending or trigger an activation.
     * The display synchronization lock prevents other CPU writers; ACT_REQ
     * brackets catch an in-flight hardware bank transition. */
    s->state_control_after = io->read32(io->context, T214_DC_STATE_CONTROL);
    if (!((s->state_control_before | s->state_control_after) &
          (T214_DC_GENERAL_ACT_REQ | T214_DC_WIN_A_ACT_REQ)))
        s->flags |= TG_GPU_PANEL_SNAPSHOT_STABLE;
    if (!(s->options & T214_DC_WIN_TRANSFORM_BITS) && s->pitch == T214_DC_PANEL_PITCH &&
        !s->h_offset && !s->v_offset && !s->surface_kind) {
        s->rotation = 0; /* fixed native portrait scanout */
        s->width = T214_DC_PANEL_WIDTH;
        s->height = T214_DC_PANEL_HEIGHT;
        s->flags |= TG_GPU_PANEL_SNAPSHOT_LAYOUT;
    }
    const TG_GPU_PRIVATE_MEMORY *memory = &adapter->Tables.Scanout;
    const UINT64 base = (UINT64)memory->SystemPhysicalAddress.QuadPart;
    if ((s->flags & (TG_GPU_PANEL_SNAPSHOT_STABLE | TG_GPU_PANEL_SNAPSHOT_LAYOUT)) ==
            (TG_GPU_PANEL_SNAPSHOT_STABLE | TG_GPU_PANEL_SNAPSHOT_LAYOUT) &&
        memory->Handle && memory->CpuAddress && memory->Bytes >= T214_GPU_POST_FB_SIZE &&
        s->address >= base && !(s->address & (PAGE_SIZE - 1)) &&
        s->address - base <= memory->Bytes - T214_GPU_POST_FB_SIZE) {
        const volatile UCHAR *pixels = (volatile UCHAR *)memory->CpuAddress + (s->address - base);
        for (UINT i = 0; i < 4; ++i) {
            s->pixels[i].x = s->width * ((i & 1) ? 3 : 1) / 4;
            s->pixels[i].y = s->height * ((i & 2) ? 3 : 1) / 4;
            s->pixels[i].bgra = *(const volatile UINT32 *)(pixels +
                s->pixels[i].y * s->pitch + s->pixels[i].x * 4);
        }
        s->flags |= TG_GPU_PANEL_SNAPSHOT_PIXELS;
    }
    action->Status = STATUS_SUCCESS;
    return TRUE;
}

static NTSTATUS APIENTRY panel_escape(HANDLE context, const DXGKARG_ESCAPE *args)
{
    PAGED_CODE();
    /* HardwareAccess requests WDDM's second-level adapter serialization.
     * Debug privilege is also required: the active desktop is not necessarily
     * owned by the caller's process or interactive session. */
    if (!context || !args || args->Flags.Value != 1 || args->hDevice || args->hContext ||
        !args->pPrivateDriverData || args->PrivateDriverDataSize != sizeof(struct tg_gpu_panel_snapshot))
        return STATUS_INVALID_PARAMETER;
    if (!SeSinglePrivilegeCheck(RtlConvertLongToLuid(SE_DEBUG_PRIVILEGE), ExGetPreviousMode()))
        return STATUS_PRIVILEGE_NOT_HELD;
    struct tg_gpu_panel_snapshot request;
    RtlCopyMemory(&request, args->pPrivateDriverData, sizeof(request));
    if (request.magic != TG_GPU_PANEL_SNAPSHOT_MAGIC || request.version != TG_GPU_PANEL_SNAPSHOT_VERSION ||
        request.size != sizeof(request))
        return STATUS_GRAPHICS_DRIVER_MISMATCH;
    SNAPSHOT_ACTION action = {.Adapter = context, .Data = {
        .magic = TG_GPU_PANEL_SNAPSHOT_MAGIC, .version = TG_GPU_PANEL_SNAPSHOT_VERSION,
        .size = sizeof(struct tg_gpu_panel_snapshot)}, .Status = STATUS_UNSUCCESSFUL};
    NTSTATUS status = TgGpuPanelSynchronize(context, snapshot_panel, &action);
    if (NT_SUCCESS(status)) status = action.Status;
    if (NT_SUCCESS(status)) RtlCopyMemory(args->pPrivateDriverData, &action.Data, sizeof(action.Data));
    return status;
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
    ddis->DxgkDdiEscape = panel_escape;
}
