/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_ADAPTER_H
#define TG_GPU_ADAPTER_H
#include "context.h"
#include "work_queue.h"
#include "resources.h"
#include "hardware.h"

/* Enough dedicated table backing for the initial single-process Vulkan
 * triangle; application storage remains dynamically allocated system memory.
 * VidMm suballocates this segment. This is not the 512 MiB application budget.
 */
#define TG_GPU_ADAPTER_TABLE_BYTES (16u*TG_GPU_TABLE_BYTES)
enum tg_gpu_adapter_state { TgGpuAdapterAdded,TgGpuAdapterStarting,TgGpuAdapterReady,TgGpuAdapterStopping };
typedef struct _TG_GPU_ADAPTER {
    /* All implemented allocation/context/query DDIs accept this same handle. */
    TG_GPU_CONTEXT_ENGINE Engine;
    PDEVICE_OBJECT PhysicalDevice;
    TG_GPU_RESOURCES Resources;
    TG_GPU_TABLE_SEGMENT Tables;
    TG_GPU_WORK_QUEUE Queue;
    TG_GPU_HARDWARE Hardware;
    enum tg_gpu_adapter_state State;
    BOOLEAN EngineReady,QueueReady;
} TG_GPU_ADAPTER;
_Static_assert(FIELD_OFFSET(TG_GPU_ADAPTER,Engine)==0,"adapter DDI handle layout");

DXGKDDI_ADD_DEVICE TgGpuDdiAddDevice;
DXGKDDI_START_DEVICE TgGpuDdiStartDevice;
DXGKDDI_STOP_DEVICE TgGpuDdiStopDevice;
DXGKDDI_REMOVE_DEVICE TgGpuDdiRemoveDevice;
/* Library hookup for the platform provider. StartDevice has already acquired
 * resources/engine/queue; no DriverEntry exposes this unfinished adapter. */
NTSTATUS TgGpuStartAdapterHardware(TG_GPU_ADAPTER *,const struct tg_gpu_power_io *,
    UCHAR PmicAddress,const struct tg_gpu_gr_payload *);
/* Populate implemented lifecycle/context/allocation/query callbacks only.
 * No DriverEntry or INF exposes this partial adapter. Submission, interrupt,
 * paging dispatch, DRIVERCAPS and TDR must be connected before registration.
 */
void TgGpuRegisterAdapterDdis(DRIVER_INITIALIZATION_DATA *);
#endif
