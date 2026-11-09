/* SPDX-License-Identifier: MIT */
#include "adapter.h"
#include "reset_backend.h"
#include "gr_firmware_io.h"

NTSTATUS APIENTRY TgGpuDdiResetFromTimeout(HANDLE adapter)
{
    TG_GPU_ADAPTER *a=adapter;NTSTATUS status;
    PAGED_CODE();
    if(!a || !a->EngineReady || (a->State!=TgGpuAdapterReady && a->State!=TgGpuAdapterResetting))
        return STATUS_INVALID_DEVICE_STATE;
    a->State=TgGpuAdapterResetting;
    status=TgGpuResetBackend(&a->Engine,&a->Queue,&a->Hardware,&a->Worker);
    a->QueueReady=a->Queue.Engine==&a->Engine;return status;
}
NTSTATUS APIENTRY TgGpuDdiRestartFromTimeout(HANDLE adapter)
{
    TG_GPU_ADAPTER *a=adapter;TG_GPU_FIRMWARE firmware={0};
    struct tg_gpu_power_io io;UCHAR address=0;NTSTATUS status;
    PAGED_CODE();
    if(!a || !a->EngineReady || a->State!=TgGpuAdapterResetting || !a->Engine.Resetting ||
       !a->Platform.Bridge || !a->Hardware.Isolated)return STATUS_INVALID_DEVICE_STATE;
    /* The OS has completed its cleanup interval. Only now load firmware,
     * preflight the platform and perform a new cold hardware initialization. */
    status=TgGpuPlatformPreflight(&a->Platform,&address);
    if(NT_SUCCESS(status))status=TgGpuFirmwareLoad(&firmware);
    if(NT_SUCCESS(status)) {
        io=TgGpuPlatformIo(&a->Platform);
        status=TgGpuRestartBackend(&a->Engine,&a->Queue,&a->Hardware,&a->Worker,
            &io,address,&firmware.payload);
        if(NT_SUCCESS(status) && !NT_SUCCESS(a->Platform.IoStatus))status=a->Platform.IoStatus;
    }
    TgGpuFirmwareFree(&firmware);a->QueueReady=a->Queue.Engine==&a->Engine;
    if(NT_SUCCESS(status))a->State=TgGpuAdapterReady;
    return status;
}
void TgGpuRegisterResetDdis(DRIVER_INITIALIZATION_DATA *d)
{
    d->DxgkDdiResetFromTimeout=TgGpuDdiResetFromTimeout;
    d->DxgkDdiRestartFromTimeout=TgGpuDdiRestartFromTimeout;
    /* Per-engine reset must also preserve the engine resubmission contract;
     * it is not implemented/advertised by these global device-loss callbacks. */
}
