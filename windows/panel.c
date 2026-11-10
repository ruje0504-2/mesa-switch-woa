/* SPDX-License-Identifier: MIT */
#include "adapter.h"
#include "../platform/t214_resources.h"

const D3DKMDT_VIDEO_SIGNAL_INFO TgGpuPanelSignal = {
    .VideoStandard = D3DKMDT_VSS_OTHER,
    .TotalSize = {1000, 1300},
    .ActiveSize = {T214_DC_PANEL_WIDTH, T214_DC_PANEL_HEIGHT},
    .VSyncFreq = {60, 1},
    .HSyncFreq = {78000, 1},
    .PixelRate = 78000000,
    .AdditionalSignalInfo = {.ScanLineOrdering = D3DDDI_VSSLO_PROGRESSIVE},
};

static uint32_t read_register(void *registers, uint32_t word)
{
    return READ_REGISTER_ULONG((PULONG)registers + word);
}

static void write_register(void *registers, uint32_t word, uint32_t value)
{
    WRITE_REGISTER_ULONG((PULONG)registers + word, value);
}

typedef struct _PANEL_ACTION {
    TG_GPU_ADAPTER *Adapter;
    enum tg_t214_dc_result Result;
} PANEL_ACTION;

static NTSTATUS synchronize(TG_GPU_ADAPTER *adapter, PKSYNCHRONIZE_ROUTINE routine,
    PANEL_ACTION *action)
{
    const DXGKRNL_INTERFACE *dxgk = &adapter->Engine.Dxgk;
    BOOLEAN invoked = FALSE;
    NTSTATUS status = dxgk->DxgkCbSynchronizeExecution(dxgk->DeviceHandle,
        routine, action, 0, &invoked);
    return NT_SUCCESS(status) && !invoked ? STATUS_UNSUCCESSFUL : status;
}

static BOOLEAN acquire_dc(PVOID context)
{
    PANEL_ACTION *action = context;
    TG_GPU_ADAPTER *adapter = action->Adapter;
    const struct tg_t214_dc_io io = {adapter->Resources.DisplayRegisters,
        read_register, write_register};
    action->Result = tg_t214_dc_acquire(&adapter->Panel.Dc, &io);
    return TRUE;
}

NTSTATUS TgGpuPanelStart(TG_GPU_ADAPTER *adapter)
{
    PAGED_CODE();
    if (!adapter)
        return STATUS_INVALID_PARAMETER;
    if (adapter->Panel.Dc.owned)
        return STATUS_INVALID_DEVICE_STATE;
    RtlZeroMemory(&adapter->Panel.Boot, sizeof(adapter->Panel.Boot));
    if (!adapter->Resources.PanelAvailable || !adapter->Resources.DisplayRegisters)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    const DXGKRNL_INTERFACE *dxgk = &adapter->Engine.Dxgk;
    if (!dxgk->DeviceHandle || !dxgk->DxgkCbAcquirePostDisplayOwnership ||
        !dxgk->DxgkCbSynchronizeExecution)
        return STATUS_INVALID_DEVICE_STATE;
    DXGK_DISPLAY_INFORMATION boot = {0};
    NTSTATUS status = dxgk->DxgkCbAcquirePostDisplayOwnership(dxgk->DeviceHandle, &boot);
    if (!NT_SUCCESS(status))
        return status;
    if (boot.Width != T214_DC_PANEL_WIDTH || boot.Height != T214_DC_PANEL_HEIGHT ||
        boot.Pitch != T214_DC_PANEL_PITCH || boot.PhysicAddress.QuadPart != T214_GPU_POST_FB_BASE ||
        (boot.ColorFormat != D3DDDIFMT_X8R8G8B8 && boot.ColorFormat != D3DDDIFMT_A8R8G8B8))
        return STATUS_DEVICE_CONFIGURATION_ERROR;

    PANEL_ACTION action = {adapter, TG_T214_DC_INVALID};
    status = synchronize(adapter, acquire_dc, &action);
    if (!NT_SUCCESS(status))
        return status;
    if (action.Result != TG_T214_DC_OK)
        return action.Result == TG_T214_DC_BUSY ? STATUS_DEVICE_BUSY :
            STATUS_DEVICE_CONFIGURATION_ERROR;
    if (boot.TargetId == D3DDDI_ID_UNINITIALIZED)
        boot.TargetId = 0;
    adapter->Panel.Boot = boot;
    return STATUS_SUCCESS;
}

static BOOLEAN restore_dc(PVOID context)
{
    PANEL_ACTION *action = context;
    struct tg_t214_dc *dc = &action->Adapter->Panel.Dc;
    if (tg_t214_dc_active_address(dc) != T214_GPU_POST_FB_BASE) {
        action->Result = tg_t214_dc_queue_flip(dc, T214_GPU_POST_FB_BASE);
        if (action->Result != TG_T214_DC_OK)
            return TRUE;
    }
    action->Result = tg_t214_dc_release(dc);
    return TRUE;
}

NTSTATUS TgGpuPanelStop(TG_GPU_ADAPTER *adapter)
{
    PAGED_CODE();
    if (!adapter)
        return STATUS_INVALID_PARAMETER;
    if (!adapter->Panel.Dc.owned)
        return STATUS_SUCCESS;
    PANEL_ACTION action = {adapter, TG_T214_DC_INVALID};
    const ULONGLONG deadline = KeQueryInterruptTime() + 5000000;
    LARGE_INTEGER delay = {.QuadPart = -10000};
    do {
        NTSTATUS status = synchronize(adapter, restore_dc, &action);
        if (!NT_SUCCESS(status))
            return status;
        if (action.Result == TG_T214_DC_OK)
            return STATUS_SUCCESS;
        if (action.Result != TG_T214_DC_BUSY)
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        (VOID)KeDelayExecutionThread(KernelMode, FALSE, &delay);
    } while (KeQueryInterruptTime() < deadline);
    return STATUS_IO_TIMEOUT;
}

static NTSTATUS APIENTRY stop_device(PVOID context)
{
    NTSTATUS status = TgGpuPanelStop(context);
    return NT_SUCCESS(status) ? TgGpuDdiStopDevice(context) : status;
}

static NTSTATUS APIENTRY remove_device(PVOID context)
{
    NTSTATUS status = TgGpuPanelStop(context);
    return NT_SUCCESS(status) ? TgGpuDdiRemoveDevice(context) : status;
}

static NTSTATUS stop_and_release(PVOID context, D3DDDI_VIDEO_PRESENT_TARGET_ID target,
    PDXGK_DISPLAY_INFORMATION info)
{
    TG_GPU_ADAPTER *adapter = context;
    if (!adapter || !info)
        return STATUS_INVALID_PARAMETER;
    if (!adapter->Panel.Boot.Width || target != adapter->Panel.Boot.TargetId)
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_TARGET;
    const DXGK_DISPLAY_INFORMATION boot = adapter->Panel.Boot;
    NTSTATUS status = stop_device(context);
    if (NT_SUCCESS(status))
        *info = boot;
    return status;
}

static NTSTATUS query_children(PVOID context, PDXGK_CHILD_DESCRIPTOR children, ULONG bytes)
{
    TG_GPU_ADAPTER *adapter = context;
    if (!adapter || !children)
        return STATUS_INVALID_PARAMETER;
    if (!adapter->Panel.Dc.owned)
        return STATUS_DEVICE_NOT_READY;
    if (bytes < 2 * sizeof(*children))
        return STATUS_BUFFER_TOO_SMALL;
    RtlZeroMemory(children, 2 * sizeof(*children));
    children[0].ChildDeviceType = TypeVideoOutput;
    children[0].ChildCapabilities.HpdAwareness = HpdAwarenessAlwaysConnected;
    children[0].ChildCapabilities.Type.VideoOutput.InterfaceTechnology = D3DKMDT_VOT_INTERNAL;
    children[0].ChildCapabilities.Type.VideoOutput.MonitorOrientationAwareness = D3DKMDT_MOA_NONE;
    children[0].ChildUid = adapter->Panel.Boot.TargetId;
    children[0].AcpiUid = adapter->Panel.Boot.AcpiId;
    return STATUS_SUCCESS;
}

static NTSTATUS query_child_status(PVOID context, PDXGK_CHILD_STATUS status, BOOLEAN nondestructive)
{
    TG_GPU_ADAPTER *adapter = context;
    UNREFERENCED_PARAMETER(nondestructive);
    if (!adapter || !status || status->ChildUid != adapter->Panel.Boot.TargetId)
        return STATUS_INVALID_PARAMETER;
    if (status->Type != StatusConnection)
        return STATUS_NOT_SUPPORTED;
    status->HotPlug.Connected = adapter->Panel.Dc.owned;
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY recommend_monitor_modes(HANDLE context, const DXGKARG_RECOMMENDMONITORMODES *args)
{
    TG_GPU_ADAPTER *adapter = context;
    PAGED_CODE();
    if (!adapter || !args || !args->pMonitorSourceModeSetInterface)
        return STATUS_INVALID_PARAMETER;
    if (!adapter->Panel.Dc.owned || args->VideoPresentTargetId != adapter->Panel.Boot.TargetId)
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_TARGET;
    const DXGK_MONITORSOURCEMODESET_INTERFACE *modes = args->pMonitorSourceModeSetInterface;
    D3DKMDT_MONITOR_SOURCE_MODE *mode;
    NTSTATUS status = modes->pfnCreateNewModeInfo(args->hMonitorSourceModeSet, &mode);
    if (!NT_SUCCESS(status))
        return status;
    mode->VideoSignalInfo = TgGpuPanelSignal;
    mode->ColorBasis = D3DKMDT_CB_SRGB;
    mode->ColorCoeffDynamicRanges = (D3DKMDT_COLOR_COEFF_DYNAMIC_RANGES){8, 8, 8, 0};
    mode->Origin = D3DKMDT_MCO_DRIVER;
    mode->Preference = D3DKMDT_MP_PREFERRED;
    status = modes->pfnAddMode(args->hMonitorSourceModeSet, mode);
    if (!NT_SUCCESS(status))
        (VOID)modes->pfnReleaseModeInfo(args->hMonitorSourceModeSet, mode);
    return status == STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET ? STATUS_SUCCESS : status;
}

void TgGpuRegisterPanelLifecycle(DRIVER_INITIALIZATION_DATA *ddis)
{
    ddis->DxgkDdiStopDevice = stop_device;
    ddis->DxgkDdiRemoveDevice = remove_device;
    ddis->DxgkDdiStopDeviceAndReleasePostDisplayOwnership = stop_and_release;
    ddis->DxgkDdiQueryChildRelations = query_children;
    ddis->DxgkDdiQueryChildStatus = query_child_status;
    ddis->DxgkDdiRecommendMonitorModes = recommend_monitor_modes;
    TgGpuRegisterPanelVidPn(ddis);
}
