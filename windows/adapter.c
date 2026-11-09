/* SPDX-License-Identifier: MIT
 * WDDM lifecycle follows Microsoft's render/compute-only samples and WDK.
 * No display ownership, power-policy change or GPU MMIO occurs in this module.
 */
#include "adapter.h"
#include "allocation.h"
#include "query.h"
#define TAG 0x44674754u

static BOOLEAN mapped(const TG_GPU_ADAPTER *a)
{
    if(a->Resources.LicMapping)return TRUE;
    for(UINT i=0;i<TgGpuApertureCount;++i)if(a->Resources.Mapping[i])return TRUE;
    return FALSE;
}
NTSTATUS APIENTRY TgGpuDdiAddDevice(PDEVICE_OBJECT physical,PVOID *out)
{
    TG_GPU_ADAPTER *a;
    PAGED_CODE();
    if(!physical || !out)return STATUS_INVALID_PARAMETER;
    *out=NULL;
    a=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*a),TAG);
    if(!a)return STATUS_NO_MEMORY;
    a->PhysicalDevice=physical;*out=a;
    return STATUS_SUCCESS;
}
NTSTATUS APIENTRY TgGpuDdiStopDevice(PVOID context)
{
    TG_GPU_ADAPTER *a=context;NTSTATUS status;
    PAGED_CODE();
    if(!a)return STATUS_INVALID_PARAMETER;
    if(a->State==TgGpuAdapterAdded)return STATUS_SUCCESS;
    a->State=TgGpuAdapterStopping;
    if(a->EngineReady) {
        KIRQL irql;
        KeAcquireSpinLock(&a->Engine.Lock,&irql);
        a->Engine.Stopping=TRUE;a->Engine.AdapterIdentityReady=FALSE;
        KeReleaseSpinLock(&a->Engine.Lock,irql);
    }
    if(a->QueueReady) {
        status=TgGpuStopWorkQueue(&a->Queue);
        if(!NT_SUCCESS(status))return status;
        a->QueueReady=FALSE;
    }
    if(a->EngineReady) {
        status=TgGpuStopContextEngine(&a->Engine);
        if(!NT_SUCCESS(status))return status;
        a->EngineReady=FALSE;
    }
    if(a->Tables.Memory.Handle) {
        status=TgGpuDestroyTableSegment(&a->Engine.Dxgk,&a->Tables);
        if(!NT_SUCCESS(status))return status;
    }
    status=TgGpuReleaseResources(&a->Engine.Dxgk,&a->Resources);
    if(!NT_SUCCESS(status))return status;
    a->State=TgGpuAdapterAdded;
    return STATUS_SUCCESS;
}
NTSTATUS APIENTRY TgGpuDdiStartDevice(PVOID context,DXGK_START_INFO *start,
    DXGKRNL_INTERFACE *dxgk,ULONG *sources,ULONG *children)
{
    TG_GPU_ADAPTER *a=context;NTSTATUS status,cleanup;
    PAGED_CODE();
    if(!a || !start || !dxgk || !sources || !children || !dxgk->DeviceHandle ||
       dxgk->Version<DXGKDDI_INTERFACE_VERSION_WDDM2_4 || dxgk->Size<sizeof(*dxgk) ||
       !dxgk->DxgkCbGetDeviceInformation || !dxgk->DxgkCbMapMemory || !dxgk->DxgkCbUnmapMemory ||
       !dxgk->DxgkCbAllocateContiguousMemory || !dxgk->DxgkCbFreeContiguousMemory ||
       !dxgk->DxgkCbSynchronizeExecution || !dxgk->DxgkCbNotifyInterrupt ||
       !dxgk->DxgkCbQueueDpc || !dxgk->DxgkCbNotifyDpc ||
       !dxgk->DxgkCbReserveGpuVirtualAddressRange ||
       !dxgk->DxgkCbAcquireHandleData || !dxgk->DxgkCbReleaseHandleData)
        return STATUS_INVALID_PARAMETER;
    if(a->State!=TgGpuAdapterAdded || a->EngineReady || a->QueueReady ||
       a->Tables.Memory.Handle || mapped(a))return STATUS_INVALID_DEVICE_STATE;
    *sources=*children=0; /* Render only; retain the existing POST display. */
    a->State=TgGpuAdapterStarting;a->Engine.Dxgk=*dxgk;
    status=TgGpuAcquireResources(dxgk,&a->Resources);
    if(!NT_SUCCESS(status))goto fail;
    if(!a->Resources.LicMapping) {status=STATUS_DEVICE_CONFIGURATION_ERROR;goto fail;}
    status=TgGpuCreateTableSegment(dxgk,TG_GPU_EXPLICIT_TABLE_SEGMENT,
                                   TG_GPU_ADAPTER_TABLE_BYTES,&a->Tables);
    if(!NT_SUCCESS(status))goto fail;
    status=TgGpuInitializeContextEngine(&a->Engine,dxgk,&a->Tables);
    if(!NT_SUCCESS(status))goto fail;
    a->EngineReady=TRUE;
    status=TgGpuInitializeWorkQueue(&a->Queue,&a->Engine);
    if(!NT_SUCCESS(status))goto fail;
    a->QueueReady=TRUE;
    /* This publishes OS identity only. It must not claim a native submission
     * path until the hardware worker, paging and recovery DDIs are connected.
     */
    a->Engine.AdapterLuid=start->AdapterLuid;a->Engine.AdapterIdentityReady=TRUE;
    a->State=TgGpuAdapterReady;
    return STATUS_SUCCESS;
fail:
    cleanup=TgGpuDdiStopDevice(a);
    return NT_SUCCESS(cleanup)?status:cleanup;
}
NTSTATUS APIENTRY TgGpuDdiRemoveDevice(PVOID context)
{
    TG_GPU_ADAPTER *a=context;
    PAGED_CODE();
    if(!a)return STATUS_INVALID_PARAMETER;
    if(a->State!=TgGpuAdapterAdded || a->EngineReady || a->QueueReady ||
       a->Tables.Memory.Handle || mapped(a))return STATUS_DEVICE_BUSY;
    ExFreePoolWithTag(a,TAG);return STATUS_SUCCESS;
}
void TgGpuRegisterAdapterDdis(DRIVER_INITIALIZATION_DATA *d)
{
    d->DxgkDdiAddDevice=TgGpuDdiAddDevice;d->DxgkDdiStartDevice=TgGpuDdiStartDevice;
    d->DxgkDdiStopDevice=TgGpuDdiStopDevice;d->DxgkDdiRemoveDevice=TgGpuDdiRemoveDevice;
    TgGpuRegisterContextDdis(d);TgGpuRegisterAllocationDdis(d);TgGpuRegisterQueryDdi(d);
}
