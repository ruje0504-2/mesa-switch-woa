/* SPDX-License-Identifier: GPL-2.0-only */
/* Opt-in Windows hardware bring-up transport. This is a System-class KMDF
 * device, not a display miniport or Vulkan ICD. It leaves the POST display
 * alone and touches GPU registers only for an explicit administrator test.
 */
#include <ntddk.h>
#include <wdf.h>
#include <ntstrsafe.h>
#include "control.h"
#include "pmic.h"
#include "../platform/t214_resources.h"
#include "../platform/t214_clock.h"
#include "../core/execute.h"

struct gpu_map { ULONG base, size; volatile UCHAR *va; };
static const struct gpu_map map_profile[] = {
    {T214_GPU_REG_BASE, T214_GPU_REG_SIZE, NULL},
    {T214_GPU_BAR1_BASE, T214_GPU_BAR1_SIZE, NULL},
    {T214_GPU_AUX_BASE, T214_GPU_AUX_SIZE, NULL},
#define MAP_RANGE(base, size) {base, size, NULL},
    T214_GPU_EXTRA_RESOURCES(MAP_RANGE)
#undef MAP_RANGE
};

typedef struct _GPU_DEVICE {
    struct gpu_map maps[T214_GPU_RESOURCE_COUNT];
    WDFIOTARGET platform;
    struct tg_gpu_power power;
    struct tg_gpu_clock clock;
    struct tg_gpu_execution execution;
    struct tg_gpu_status last;
    NTSTATUS io_status;
    PVOID arena;
    PHYSICAL_ADDRESS arena_address;
} GPU_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(GPU_DEVICE, GpuContext)
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD GpuDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE GpuPrepare;
EVT_WDF_DEVICE_RELEASE_HARDWARE GpuRelease;
EVT_WDF_DEVICE_D0_EXIT GpuD0Exit;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL GpuControl;

static NTSTATUS GpuPlatform(GPU_DEVICE *d, struct tg_gpu_pmic_request *op)
{
    WDF_MEMORY_DESCRIPTOR in, out;
    WDF_REQUEST_SEND_OPTIONS options;
    ULONG_PTR bytes = 0;
    NTSTATUS status;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&in, op, sizeof(*op));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&out, op, sizeof(*op));
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    options.Timeout = WDF_REL_TIMEOUT_IN_MS(500);
    status = WdfIoTargetSendIoctlSynchronously(d->platform, NULL, TG_GPU_IOCTL_PMIC,
                                              &in, &out, &options, &bytes);
    if (NT_SUCCESS(status) && bytes != sizeof(*op)) status = STATUS_DEVICE_PROTOCOL_ERROR;
    if (!NT_SUCCESS(status)) d->io_status = status;
    return status;
}
static NTSTATUS GpuOpenPlatform(WDFDEVICE device)
{
    GPU_DEVICE *d = GpuContext(device);
    WDF_IO_TARGET_OPEN_PARAMS open;
    DECLARE_CONST_UNICODE_STRING(name, L"\\Device\\SwitchBacklight");
    NTSTATUS status;
    if (d->platform) return STATUS_SUCCESS;
    status = WdfIoTargetCreate(device, WDF_NO_OBJECT_ATTRIBUTES, &d->platform);
    if (!NT_SUCCESS(status)) return status;
    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&open, &name, GENERIC_READ | GENERIC_WRITE);
    open.ShareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;
    status = WdfIoTargetOpen(d->platform, &open);
    if (!NT_SUCCESS(status)) { WdfObjectDelete(d->platform); d->platform = NULL; }
    return status;
}
static VOID GpuClosePlatform(GPU_DEVICE *d)
{
    if (d->platform) { WdfIoTargetClose(d->platform); WdfObjectDelete(d->platform); d->platform = NULL; }
}
static uint32_t GpuRead(void *context, uint32_t address)
{
    GPU_DEVICE *d = context;
    ULONG i;
    if (address == 0x600060a0) {
        struct tg_gpu_pmic_request op = {0, TG_GPU_PMIC_QUERY_PLLP, 0, 0};
        return NT_SUCCESS(GpuPlatform(d, &op)) ? op.value : 0;
    }
    for (i = 0; i < T214_GPU_RESOURCE_COUNT; ++i) {
        const struct gpu_map *m = &d->maps[i];
        if (address >= m->base && address - m->base <= m->size - 4 && m->va)
            return READ_REGISTER_ULONG((PULONG)(m->va + address - m->base));
    }
    d->io_status = STATUS_DEVICE_CONFIGURATION_ERROR;
    return UINT32_MAX;
}
static void GpuWrite(void *context, uint32_t address, uint32_t value)
{
    GPU_DEVICE *d = context;
    ULONG i;
    /* Retain the old ACPI resource layout, but never write global SMMU ASIDs.
     * Direct physical probe buffers do not use the bit-34 translation tag.
     */
    if (address == 0x70019aa8 || address == 0x70019aac) {
        d->io_status = STATUS_DEVICE_CONFIGURATION_ERROR;
        return;
    }
    /* The last three resources are read-only identity/calibration fuses. */
    for (i = 0; i < T214_GPU_RESOURCE_COUNT - 3; ++i) {
        const struct gpu_map *m = &d->maps[i];
        if (address >= m->base && address - m->base <= m->size - 4 && m->va) {
            KeMemoryBarrier();
            WRITE_REGISTER_ULONG((PULONG)(m->va + address - m->base), value);
            KeMemoryBarrier();
            return;
        }
    }
    d->io_status = STATUS_DEVICE_CONFIGURATION_ERROR;
}
static bool GpuReadPmic(void *context, uint8_t address, uint8_t reg, uint8_t *value)
{
    struct tg_gpu_pmic_request op = {address, reg, 0, 0};
    if (!NT_SUCCESS(GpuPlatform(context, &op))) return false;
    *value = (uint8_t)op.value;
    return true;
}
static bool GpuWritePmic(void *context, uint8_t address, uint8_t reg, uint8_t value)
{
    struct tg_gpu_pmic_request op = {address, reg, 1, value};
    return NT_SUCCESS(GpuPlatform(context, &op));
}
static void GpuDelay(void *context, uint32_t us)
{
    LARGE_INTEGER interval;
    UNREFERENCED_PARAMETER(context);
    if (us < 1000) { KeStallExecutionProcessor(us); return; }
    interval.QuadPart = -(LONGLONG)us * 10;
    (VOID)KeDelayExecutionThread(KernelMode, FALSE, &interval);
}
static struct tg_gpu_power_io GpuIo(GPU_DEVICE *d)
{
    struct tg_gpu_power_io io = {d, GpuRead, GpuWrite, GpuReadPmic, GpuWritePmic, GpuDelay};
    return io;
}
static VOID GpuSaveResult(WDFDEVICE device)
{
    GPU_DEVICE *d = GpuContext(device);
    WDFKEY key;
    LARGE_INTEGER now;
    DECLARE_CONST_UNICODE_STRING(result_name, L"LastResult");
    DECLARE_CONST_UNICODE_STRING(time_name, L"LastResultTime");
    DECLARE_CONST_UNICODE_STRING(target_name, L"SelectedKhz");
    if (!NT_SUCCESS(WdfDriverOpenParametersRegistryKey(WdfDeviceGetDriver(device),
        KEY_SET_VALUE, WDF_NO_OBJECT_ATTRIBUTES, &key))) return;
    KeQuerySystemTime(&now);
    /* Fixed-size values overwritten after a user test; no growing log. */
    (VOID)WdfRegistryAssignValue(key, &result_name, REG_BINARY, sizeof(d->last), &d->last);
    (VOID)WdfRegistryAssignValue(key, &time_name, REG_QWORD, sizeof(now), &now);
    if (!d->last.power_result && !d->last.clock_result && !d->last.execution_result &&
        !d->last.frontend.result && !d->last.restore_result && !d->last.resources_owned)
        (VOID)WdfRegistryAssignULong(key, &target_name, d->last.target_khz);
    WdfRegistryClose(key);
}
static VOID GpuCleanup(GPU_DEVICE *d)
{
    struct tg_gpu_power_io io = GpuIo(d);
    enum tg_gpu_clock_result cr;
    enum tg_gpu_power_result pr;
    tg_gpu_execute_stop(&io, &d->execution);
    if (d->execution.owned && d->power.ready) {
        pr = tg_gpu_power_drain(&io, &d->power);
        if (pr != TG_GPU_POWER_OK) {
            d->last.restore_result = pr;
            d->last.resources_owned = 1;
            return; /* Never free memory while device writes may be live. */
        }
    }
    cr = tg_gpu_clock_stop(&io, &d->clock);
    pr = tg_gpu_power_down(&io, &d->power);
    d->last.restore_result = ((uint32_t)cr << 16) | pr;
    if (!d->power.owned) {
        /* Full reset/power-down retired the GPU's register ownership too. */
        d->clock.owned = d->clock.running = false;
        d->execution.owned = d->execution.fifo_ready = false;
        if (d->arena) {
            MmFreeContiguousMemorySpecifyCache(d->arena, TG_GPU_ARENA_BYTES, MmNonCached);
            d->arena = NULL;
        }
    }
    d->last.resources_owned = d->power.owned || d->execution.owned;
}
static NTSTATUS GpuRun(WDFDEVICE device, ULONG target)
{
    GPU_DEVICE *d = GpuContext(device);
    struct tg_gpu_power_io io = GpuIo(d);
    struct tg_gpu_pmic_request op = {0, TG_GPU_PMIC_QUERY_ADDRESS, 0, 0};
    PHYSICAL_ADDRESS low = {0}, high, boundary = {0};
    NTSTATUS status;
    uint8_t address;
    ULONG seed;
    if (target != TG_GPU_CONTROL_DEFAULT_KHZ && target != TG_GPU_CONTROL_OC_KHZ)
        return STATUS_INVALID_PARAMETER;
    if (d->power.owned || d->clock.owned || d->execution.owned)
        return STATUS_INVALID_DEVICE_STATE;
    RtlZeroMemory(&d->power, sizeof(d->power));
    RtlZeroMemory(&d->clock, sizeof(d->clock));
    RtlZeroMemory(&d->execution, sizeof(d->execution));
    status = GpuOpenPlatform(device);
    if (!NT_SUCCESS(status)) return status;
    status = GpuPlatform(d, &op);
    if (!NT_SUCCESS(status)) goto early;
    address = (uint8_t)op.value;
    op.reg = TG_GPU_PMIC_QUERY_TEMP;
    status = GpuPlatform(d, &op);
    if (!NT_SUCCESS(status)) goto early;
    if (op.value >= 80) { status = STATUS_DEVICE_NOT_READY; goto early; }
    RtlZeroMemory(&d->last, sizeof(d->last));
    d->last.version = TG_GPU_CONTROL_VERSION;
    d->last.tested = 1;
    d->last.target_khz = target;
    d->last.clock_result = d->last.execution_result = UINT32_MAX;
    d->io_status = STATUS_SUCCESS;
    seed = 0x13579bdfu ^ (ULONG)KeQueryInterruptTime();
    if (seed == 0xdeadbeef || ~seed == 0xdeadbeef) seed ^= 0x1000;
    d->last.expected[0] = seed; d->last.expected[1] = ~seed;
    high.QuadPart = 0xffffffff;
    d->arena = MmAllocateContiguousMemorySpecifyCache(TG_GPU_ARENA_BYTES,
                                                      low, high, boundary, MmNonCached);
    if (!d->arena) { status = STATUS_INSUFFICIENT_RESOURCES; goto early; }
    d->arena_address = MmGetPhysicalAddress(d->arena);
    d->last.power_result = tg_gpu_power_up(&io, address, &d->power);
    d->last.stage = 100 + d->power.stage;
    if (d->last.power_result != TG_GPU_POWER_OK) goto done;
    d->last.boot0 = GpuRead(d, T214_GPU_REG_BASE);
    if (tg_gpu_frontend_start(&io, &d->power, &d->last.frontend) != TG_GPU_FE_OK) {
        d->last.execution_result = TG_GPU_EXEC_FRONTEND;
        d->last.stage = 150;
        goto done;
    }
    d->last.clock_result = tg_gpu_clock_start(&io, &d->power, target, &d->clock);
    d->last.stage = 200 + d->clock.stage;
    d->last.measured_khz = d->clock.measured_khz;
    d->last.rail_uv = d->clock.rail_uv;
    if (d->last.clock_result != TG_GPU_CLOCK_OK) goto done;
    KeMemoryBarrier();
    d->last.execution_result = tg_gpu_execute(&io, &d->power, d->arena,
        (uint64_t)d->arena_address.QuadPart, &d->execution, &d->last);
    d->last.stage = 300 + d->execution.stage;
done:
    if (d->power.ready) tg_gpu_frontend_snapshot(&io, &d->last.frontend);
    if (d->execution.fifo_ready && d->last.execution_result != TG_GPU_EXEC_OK)
        tg_gpu_execute_snapshot(&io, &d->execution, &d->last);
    GpuCleanup(d);
    if (!d->power.owned) GpuClosePlatform(d);
    GpuSaveResult(device);
    return d->io_status;
early:
    GpuClosePlatform(d);
    return status;
}

VOID GpuControl(WDFQUEUE queue, WDFREQUEST request, size_t out_size, size_t in_size, ULONG code)
{
    WDFDEVICE device = WdfIoQueueGetDevice(queue);
    GPU_DEVICE *d = GpuContext(device);
    struct tg_gpu_status *out;
    struct tg_gpu_request *in, op;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(out_size); UNREFERENCED_PARAMETER(in_size);
    status = WdfRequestRetrieveOutputBuffer(request, sizeof(*out), (PVOID *)&out, NULL);
    if (!NT_SUCCESS(status)) { WdfRequestComplete(request, status); return; }
    if (code == TG_GPU_IOCTL_RUN) {
        status = WdfRequestRetrieveInputBuffer(request, sizeof(op), (PVOID *)&in, NULL);
        if (NT_SUCCESS(status)) {
            op = *in;
            status = op.version == TG_GPU_CONTROL_VERSION ?
                GpuRun(device, op.target_khz) : STATUS_REVISION_MISMATCH;
        }
    } else if (code == TG_GPU_IOCTL_STATUS) status = STATUS_SUCCESS;
    else status = STATUS_INVALID_DEVICE_REQUEST;
    if (NT_SUCCESS(status)) *out = d->last;
    WdfRequestCompleteWithInformation(request, status, NT_SUCCESS(status) ? sizeof(*out) : 0);
}
NTSTATUS GpuPrepare(WDFDEVICE device, WDFCMRESLIST raw, WDFCMRESLIST translated)
{
    GPU_DEVICE *d = GpuContext(device);
    ULONG i, n, found = 0;
    UNREFERENCED_PARAMETER(raw);
    RtlCopyMemory(d->maps, map_profile, sizeof(map_profile));
    for (i = 0; i < WdfCmResourceListGetCount(translated); ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r = WdfCmResourceListGetDescriptor(translated, i);
        if (!r || r->Type != CmResourceTypeMemory) continue;
        for (n = 0; n < T214_GPU_RESOURCE_COUNT; ++n)
            if (r->u.Memory.Start.QuadPart == d->maps[n].base && r->u.Memory.Length == d->maps[n].size) break;
        if (n == T214_GPU_RESOURCE_COUNT || d->maps[n].va) goto fail;
        d->maps[n].va = MmMapIoSpace(r->u.Memory.Start, r->u.Memory.Length, MmNonCached);
        if (!d->maps[n].va) goto fail;
        ++found;
    }
    if (found == T214_GPU_RESOURCE_COUNT) return STATUS_SUCCESS;
fail:
    for (n = 0; n < T214_GPU_RESOURCE_COUNT; ++n)
        if (d->maps[n].va) { MmUnmapIoSpace((PVOID)d->maps[n].va, d->maps[n].size); d->maps[n].va = NULL; }
    return STATUS_DEVICE_CONFIGURATION_ERROR;
}
NTSTATUS GpuD0Exit(WDFDEVICE device, WDF_POWER_DEVICE_STATE target)
{
    GPU_DEVICE *d = GpuContext(device);
    UNREFERENCED_PARAMETER(target);
    if (d->power.owned || d->execution.owned || d->arena) GpuCleanup(d);
    if (!d->power.owned) GpuClosePlatform(d);
    return d->power.owned ? STATUS_DEVICE_BUSY : STATUS_SUCCESS;
}
NTSTATUS GpuRelease(WDFDEVICE device, WDFCMRESLIST resources)
{
    GPU_DEVICE *d = GpuContext(device);
    ULONG i;
    UNREFERENCED_PARAMETER(resources);
    if (d->power.owned || d->execution.owned || d->arena) GpuCleanup(d);
    GpuClosePlatform(d);
    for (i = 0; i < T214_GPU_RESOURCE_COUNT; ++i)
        if (d->maps[i].va) { MmUnmapIoSpace((PVOID)d->maps[i].va, d->maps[i].size); d->maps[i].va = NULL; }
    return d->power.owned ? STATUS_DEVICE_BUSY : STATUS_SUCCESS;
}
NTSTATUS GpuDeviceAdd(WDFDRIVER driver, PWDFDEVICE_INIT init)
{
    WDFDEVICE device;
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_IO_QUEUE_CONFIG queue;
    NTSTATUS status;
    DECLARE_CONST_UNICODE_STRING(name, L"\\Device\\TegraGpuProbe");
    DECLARE_CONST_UNICODE_STRING(link, L"\\DosDevices\\TegraGpuProbe");
    DECLARE_CONST_UNICODE_STRING(sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;BU)");
    UNREFERENCED_PARAMETER(driver);
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = GpuPrepare;
    pnp.EvtDeviceReleaseHardware = GpuRelease;
    pnp.EvtDeviceD0Exit = GpuD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &pnp);
    WdfDeviceInitSetDeviceType(init, FILE_DEVICE_UNKNOWN);
    status = WdfDeviceInitAssignName(init, &name);
    if (!NT_SUCCESS(status)) return status;
    status = WdfDeviceInitAssignSDDLString(init, &sddl);
    if (!NT_SUCCESS(status)) return status;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, GPU_DEVICE);
    attr.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfDeviceCreate(&init, &attr, &device);
    if (!NT_SUCCESS(status)) return status;
    GpuContext(device)->last.version = TG_GPU_CONTROL_VERSION;
    GpuContext(device)->last.target_khz = TG_GPU_CONTROL_DEFAULT_KHZ;
    {
        WDFKEY key;
        ULONG selected;
        DECLARE_CONST_UNICODE_STRING(target_name, L"SelectedKhz");
        if (NT_SUCCESS(WdfDriverOpenParametersRegistryKey(driver, KEY_QUERY_VALUE,
            WDF_NO_OBJECT_ATTRIBUTES, &key))) {
            if (NT_SUCCESS(WdfRegistryQueryULong(key, &target_name, &selected)) &&
                (selected == TG_GPU_CONTROL_DEFAULT_KHZ || selected == TG_GPU_CONTROL_OC_KHZ))
                GpuContext(device)->last.target_khz = selected;
            WdfRegistryClose(key);
        }
    }
    status = WdfDeviceCreateSymbolicLink(device, &link);
    if (!NT_SUCCESS(status)) return status;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue, WdfIoQueueDispatchSequential);
    queue.EvtIoDeviceControl = GpuControl;
    return WdfIoQueueCreate(device, &queue, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
}
NTSTATUS DriverEntry(PDRIVER_OBJECT object, PUNICODE_STRING path)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, GpuDeviceAdd);
    return WdfDriverCreate(object, path, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
