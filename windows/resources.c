/* SPDX-License-Identifier: MIT */
/* Copyright 2026 ruje0504-2 */
#include "resources.h"
#include "../platform/t214_dc.h"
#include "../platform/t214_resources.h"

static const ULONG TgGpuBases[TgGpuApertureCount] = {
    T214_GPU_REG_BASE, T214_GPU_BAR1_BASE, T214_GPU_AUX_BASE,
#define GPU_BASE(base, size) base,
    T214_GPU_EXTRA_RESOURCES(GPU_BASE)
#undef GPU_BASE
};
static const ULONG TgGpuSizes[TgGpuApertureCount] = {
    T214_GPU_REG_SIZE, T214_GPU_BAR1_SIZE, T214_GPU_AUX_SIZE,
#define GPU_SIZE(base, size) size,
    T214_GPU_EXTRA_RESOURCES(GPU_SIZE)
#undef GPU_SIZE
};
_Static_assert(TgGpuApertureCount == T214_GPU_RESOURCE_COUNT, "GPU resource count");
_Static_assert(T214_DC_WIN_A_ADDR_HI * sizeof(ULONG) < T214_GPU_DC_SIZE, "Display A aperture");
_Static_assert(T214_DC_PANEL_PITCH * T214_DC_PANEL_HEIGHT == T214_GPU_POST_FB_SIZE, "POST size");

static BOOLEAN
matches_boot_scanout(PULONG registers)
{
    const ULONG pending = T214_DC_GENERAL_ACT_REQ | T214_DC_WIN_A_ACT_REQ;
    /* The firmware leaves the controller in continuous mode. Read the
     * selected bank without changing the mux, and reject a pending latch.
     */
    return !(READ_REGISTER_ULONG(registers + T214_DC_STATE_CONTROL) & pending) &&
        (READ_REGISTER_ULONG(registers + T214_DC_COMMAND) & T214_DC_MODE_MASK) ==
            T214_DC_MODE_CONTINUOUS &&
        (READ_REGISTER_ULONG(registers + T214_DC_OUTPUT_OPTIONS) & T214_DC_DSI_ENABLE) &&
        READ_REGISTER_ULONG(registers + T214_DC_ACTIVE_SIZE) == T214_DC_PANEL_ACTIVE &&
        READ_REGISTER_ULONG(registers + T214_DC_SYNC_WIDTH) == T214_DC_PANEL_SYNC &&
        READ_REGISTER_ULONG(registers + T214_DC_BACK_PORCH) == T214_DC_PANEL_BACK &&
        READ_REGISTER_ULONG(registers + T214_DC_FRONT_PORCH) == T214_DC_PANEL_FRONT &&
        READ_REGISTER_ULONG(registers + T214_DC_WIN_A_OPTIONS) == T214_DC_WIN_ENABLE &&
        READ_REGISTER_ULONG(registers + T214_DC_WIN_A_BYTE_SWAP) == 0 &&
        READ_REGISTER_ULONG(registers + T214_DC_WIN_A_BUFFER) == 0 &&
        READ_REGISTER_ULONG(registers + T214_DC_WIN_A_COLOR) == T214_DC_COLOR_BGRA8888 &&
        READ_REGISTER_ULONG(registers + T214_DC_WIN_A_POSITION) == 0 &&
        READ_REGISTER_ULONG(registers + T214_DC_WIN_A_SIZE) == T214_DC_PANEL_ACTIVE &&
        READ_REGISTER_ULONG(registers + T214_DC_WIN_A_PRESCALED) == T214_DC_PANEL_PRESCALED &&
        READ_REGISTER_ULONG(registers + T214_DC_WIN_A_DDA) == T214_DC_DDA_IDENTITY &&
        (READ_REGISTER_ULONG(registers + T214_DC_WIN_A_STRIDE) & 0xffff) ==
            T214_DC_PANEL_PITCH &&
        READ_REGISTER_ULONG(registers + T214_DC_WIN_A_ADDR) == T214_GPU_POST_FB_BASE &&
        READ_REGISTER_ULONG(registers + T214_DC_WIN_A_ADDR_HI) == 0 &&
        READ_REGISTER_ULONG(registers + T214_DC_WIN_A_H_OFFSET) == 0 &&
        READ_REGISTER_ULONG(registers + T214_DC_WIN_A_V_OFFSET) == 0 &&
        READ_REGISTER_ULONG(registers + T214_DC_WIN_A_SURFACE_KIND) == 0 &&
        !(READ_REGISTER_ULONG(registers + T214_DC_STATE_CONTROL) & pending);
}

NTSTATUS
TgGpuReleaseResources(const DXGKRNL_INTERFACE *Dxgk,
                      TG_GPU_RESOURCES *Resources)
{
    NTSTATUS result = STATUS_SUCCESS;
    ULONG i;
    PAGED_CODE();
    if (Resources->DisplayRegisters) {
        result = Dxgk->DxgkCbUnmapMemory(Dxgk->DeviceHandle, Resources->DisplayRegisters);
        if (NT_SUCCESS(result))
            Resources->DisplayRegisters = NULL;
    }
    if(Resources->LicMapping) {
        NTSTATUS status=Dxgk->DxgkCbUnmapMemory(Dxgk->DeviceHandle,Resources->LicMapping);
        if(NT_SUCCESS(status)) Resources->LicMapping=NULL;
        else if (NT_SUCCESS(result)) result=status;
    }
    for (i = TgGpuApertureCount; i != 0; --i) {
        if (Resources->Mapping[i - 1]) {
            NTSTATUS status = Dxgk->DxgkCbUnmapMemory(
                Dxgk->DeviceHandle, Resources->Mapping[i - 1]);
            if (NT_SUCCESS(status))
                Resources->Mapping[i - 1] = NULL;
            else if (NT_SUCCESS(result))
                result = status;
        }
    }
    return result;
}

NTSTATUS
TgGpuAcquireResources(const DXGKRNL_INTERFACE *Dxgk,
                      TG_GPU_RESOURCES *Resources)
{
    DXGK_DEVICE_INFO info = {0};
    PCM_PARTIAL_RESOURCE_LIST list;
    const CM_PARTIAL_RESOURCE_DESCRIPTOR *display = NULL;
    ULONG i, memoryMask = 0, interruptCount = 0;
    BOOLEAN hasPostFramebuffer = FALSE;
    NTSTATUS status;
    PAGED_CODE();

    if(Resources->LicMapping || Resources->DisplayRegisters) return STATUS_INVALID_DEVICE_STATE;
    for (i = 0; i < TgGpuApertureCount; ++i)
        if (Resources->Mapping[i])
            return STATUS_INVALID_DEVICE_STATE;
    RtlZeroMemory(Resources, sizeof(*Resources));

    status = Dxgk->DxgkCbGetDeviceInformation(Dxgk->DeviceHandle, &info);
    if (!NT_SUCCESS(status))
        return status;
    /* GPU0 supplies render and narrow power resources. The optional POST
     * aperture is validated here. Mapping it requires display ownership. */
    if (!info.TranslatedResourceList || info.TranslatedResourceList->Count != 1)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    list = &info.TranslatedResourceList->List[0].PartialResourceList;
    for (i = 0; i < list->Count; ++i) {
        const CM_PARTIAL_RESOURCE_DESCRIPTOR *resource = &list->PartialDescriptors[i];
        if (resource->Type == CmResourceTypeMemory) {
            if (resource->u.Memory.Start.QuadPart == T214_GPU_DC_BASE) {
                if (display || resource->u.Memory.Length != T214_GPU_DC_SIZE ||
                    resource->ShareDisposition != CmResourceShareDeviceExclusive ||
                    (resource->Flags & CM_RESOURCE_MEMORY_WRITE_ONLY))
                    return STATUS_DEVICE_CONFIGURATION_ERROR;
                display = resource;
                continue;
            }
            if (resource->u.Memory.Start.QuadPart == T214_GPU_POST_FB_BASE) {
                if (hasPostFramebuffer ||
                    resource->u.Memory.Length != T214_GPU_POST_FB_SIZE ||
                    resource->ShareDisposition != CmResourceShareDeviceExclusive ||
                    (resource->Flags & (CM_RESOURCE_MEMORY_READ_ONLY |
                                        CM_RESOURCE_MEMORY_WRITE_ONLY)) != 0)
                    return STATUS_DEVICE_CONFIGURATION_ERROR;
                hasPostFramebuffer = TRUE;
                continue;
            }
            if(resource->u.Memory.Start.QuadPart==T214_GPU_LIC_BASE) {
                if(resource->u.Memory.Length!=T214_GPU_LIC_SIZE ||
                   resource->ShareDisposition!=CmResourceShareDeviceExclusive || Resources->Lic.Type)
                    return STATUS_DEVICE_CONFIGURATION_ERROR;
                Resources->Lic=*resource;
                continue;
            }
            ULONG aperture;
            for (aperture = 0; aperture < TgGpuApertureCount; ++aperture)
                if (resource->u.Memory.Start.QuadPart == TgGpuBases[aperture])
                    break;
            if (aperture == TgGpuApertureCount ||
                resource->u.Memory.Length != TgGpuSizes[aperture] ||
                (memoryMask & (1u << aperture)) != 0 ||
                resource->ShareDisposition != CmResourceShareDeviceExclusive)
                return STATUS_DEVICE_CONFIGURATION_ERROR;
            Resources->Memory[aperture] = *resource;
            memoryMask |= 1u << aperture;
        } else if (resource->Type == CmResourceTypeInterrupt) {
            if (interruptCount == 2 ||
                (resource->Flags & (CM_RESOURCE_INTERRUPT_LATCHED |
                                    CM_RESOURCE_INTERRUPT_MESSAGE)) != 0)
                return STATUS_DEVICE_CONFIGURATION_ERROR;
            /* Preserve translated Windows vectors, levels and affinity.
             * They are not the raw GSI values from _CRS. Order is stall then
             * nonstall, as emitted by the shared firmware resource profile.
             */
            Resources->Interrupt[interruptCount++] = *resource;
        }
    }
    if (memoryMask != (1u << TgGpuApertureCount) - 1u || interruptCount != 2)
        return STATUS_DEVICE_CONFIGURATION_ERROR;

    for (i = 0; i < TgGpuApertureCount; ++i) {
        status = Dxgk->DxgkCbMapMemory(Dxgk->DeviceHandle,
            Resources->Memory[i].u.Memory.Start, TgGpuSizes[i],
            FALSE, FALSE, MmNonCached, &Resources->Mapping[i]);
        if (!NT_SUCCESS(status)) {
            NTSTATUS cleanup = TgGpuReleaseResources(Dxgk, Resources);
            return NT_SUCCESS(cleanup) ? status : cleanup;
        }
    }
    if(Resources->Lic.Type) {
        status=Dxgk->DxgkCbMapMemory(Dxgk->DeviceHandle,Resources->Lic.u.Memory.Start,
            T214_GPU_LIC_SIZE,FALSE,FALSE,MmNonCached,&Resources->LicMapping);
        if(!NT_SUCCESS(status)) {
            NTSTATUS cleanup=TgGpuReleaseResources(Dxgk,Resources);
            return NT_SUCCESS(cleanup)?status:cleanup;
        }
    }
    if (display) {
        status = Dxgk->DxgkCbMapMemory(Dxgk->DeviceHandle, display->u.Memory.Start,
            T214_GPU_DC_SIZE, FALSE, FALSE, MmNonCached, &Resources->DisplayRegisters);
        if (NT_SUCCESS(status) && !matches_boot_scanout(Resources->DisplayRegisters))
            status = STATUS_DEVICE_CONFIGURATION_ERROR;
        if (!NT_SUCCESS(status)) {
            NTSTATUS cleanup = TgGpuReleaseResources(Dxgk, Resources);
            return NT_SUCCESS(cleanup) ? status : cleanup;
        }
    }
    return STATUS_SUCCESS;
}
