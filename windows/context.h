/* SPDX-License-Identifier: MIT */
#ifndef TEGRA_GPU_CONTEXT_H
#define TEGRA_GPU_CONTEXT_H
#include "root_table.h"
#include "internal_va.h"
#include "../core/channel.h"
#include "../platform/t214_clock.h"

typedef struct _TG_GPU_CONTEXT_ENGINE TG_GPU_CONTEXT_ENGINE;
typedef struct _TG_GPU_PROCESS TG_GPU_PROCESS;
typedef struct _TG_GPU_DEVICE TG_GPU_DEVICE;
typedef struct _TG_GPU_CONTEXT TG_GPU_CONTEXT;
typedef struct _TG_GPU_CONTEXT_WORK TG_GPU_CONTEXT_WORK;

struct _TG_GPU_PROCESS {
    LIST_ENTRY Link;
    TG_GPU_CONTEXT_ENGINE *Engine;
    HANDLE Runtime;
    UINT Devices;
    DXGK_CREATEPROCESSFLAGS Flags;
    TG_GPU_INTERNAL_VA Internal;
    BOOLEAN Closing;
};
struct _TG_GPU_DEVICE {
    LIST_ENTRY Link;
    TG_GPU_CONTEXT_ENGINE *Engine;
    TG_GPU_PROCESS *Process;
    HANDLE Runtime;
    UINT Contexts;
    UINT OpenAllocations;
};
struct _TG_GPU_CONTEXT {
    LIST_ENTRY Link;
    TG_GPU_DEVICE *Device;
    HANDLE Runtime;
    TG_GPU_PRIVATE_MEMORY Instance;
    TG_GPU_ROOT_TABLE Root;
    DXGK_CREATECONTEXTFLAGS Flags;
    NTSTATUS RootStatus;
    UINT References;
    BOOLEAN Closing;
};
struct _TG_GPU_CONTEXT_ENGINE {
    DXGKRNL_INTERFACE Dxgk;
    const TG_GPU_TABLE_SEGMENT *Tables;
    KSPIN_LOCK Lock;
    LIST_ENTRY Processes, Devices, Contexts;
    TG_GPU_CONTEXT_WORK *Active;
    TG_GPU_CONTEXT_WORK *Notification;
    PVOID QueueOwner;
    TG_GPU_PRIVATE_MEMORY Commands;
    struct tg_gpu_channel Channel;
    volatile UINT LastCompletedFence;
    UINT Marker;
    UINT Creating;
    UINT AllocationCount,ResourceCount;
    BOOLEAN Stopping;
};
/* The single physical execution slot is serialized; the number of software
 * contexts is limited only by allocation failures, not a fixed slot array.
 * Keep this record alive until retirement or verified reset succeeds.
 */
struct _TG_GPU_CONTEXT_WORK {
    TG_GPU_CONTEXT_ENGINE *Engine;
    TG_GPU_CONTEXT *Context;
    TG_GPU_ROOT_TABLE Root;
    UINT64 DmaAddress, FencePhysicalAddress, InternalVa;
    UINT DmaBytes, SubmissionFence, Expected;
    BOOLEAN Signaling, Notified, Prepared, HardwareBusy, Submitted, Attempted, Retired, CompletionReady;
};

NTSTATUS TgGpuInitializeContextEngine(TG_GPU_CONTEXT_ENGINE *, const DXGKRNL_INTERFACE *,
                                     const TG_GPU_TABLE_SEGMENT *);
/* Stops admission; fails until contexts/devices/processes and work retire. */
NTSTATUS TgGpuStopContextEngine(TG_GPU_CONTEXT_ENGINE *);
/* Populate only these implemented entries in a caller-owned initialization
 * table. This module does not register a display adapter by itself.
 */
void TgGpuRegisterContextDdis(DRIVER_INITIALIZATION_DATA *);

DXGKDDI_CREATEPROCESS TgGpuDdiCreateProcess;
DXGKDDI_DESTROYPROCESS TgGpuDdiDestroyProcess;
DXGKDDI_CREATEDEVICE TgGpuDdiCreateDevice;
DXGKDDI_DESTROYDEVICE TgGpuDdiDestroyDevice;
DXGKDDI_CREATECONTEXT TgGpuDdiCreateContext;
DXGKDDI_DESTROYCONTEXT TgGpuDdiDestroyContext;
DXGKDDI_GETROOTPAGETABLESIZE TgGpuDdiGetRootPageTableSize;
DXGKDDI_SETROOTPAGETABLE TgGpuDdiSetRootPageTable;
DXGKDDI_QUERYCURRENTFENCE TgGpuDdiQueryCurrentFence;
DXGKDDI_DPC_ROUTINE TgGpuDdiDpcRoutine;

/* Submission worker helpers, not SubmitCommandVirtual DDIs. In particular,
 * STATUS_DEVICE_BUSY must stay inside the software queue: the actual DDI may
 * return only the statuses allowed by Dxgkrnl's submission contract.
 * The caller still validates command packets and VidMm residency before
 * programming hardware; a bounded virtual address is not that validation.
 */
NTSTATUS TgGpuAcquireContextWork(TG_GPU_CONTEXT_ENGINE *,
    const DXGKARG_SUBMITCOMMANDVIRTUAL *, TG_GPU_CONTEXT_WORK *);
/* Queue admission retains the context/root without touching shared commands,
 * RAMIN or the fence. Activation occurs only when this record reaches the head.
 * A successful capture must be activated/retired or canceled exactly once.
 */
NTSTATUS TgGpuCaptureContextWork(TG_GPU_CONTEXT_ENGINE *,
    const DXGKARG_SUBMITCOMMANDVIRTUAL *, TG_GPU_CONTEXT_WORK *);
NTSTATUS TgGpuActivateContextWork(TG_GPU_CONTEXT_ENGINE *, TG_GPU_CONTEXT_WORK *);
NTSTATUS TgGpuCancelCapturedWork(TG_GPU_CONTEXT_ENGINE *, TG_GPU_CONTEXT_WORK *);
/* After acquisition and before hardware binding, repair the reserved PDE and
 * encode the USER workload plus a driver-generated KERNEL completion packet.
 * This performs no MMIO. The hardware backend still binds RAMIN, invalidates
 * the root TLB and submits the runlist before polling completion.
 */
NTSTATUS TgGpuPrepareContextWork(TG_GPU_CONTEXT_ENGINE *, TG_GPU_CONTEXT_WORK *);
/* Worker path after cold GPU/FIFO/LTC/MMU/clock initialization. Queuing is
 * distinct from completion. These helpers are not yet registered submission
 * DDIs; their internal failure statuses must not be returned directly to
 * Dxgkrnl's SubmitCommandVirtual callback.
 */
NTSTATUS TgGpuSubmitContextWork(TG_GPU_CONTEXT_ENGINE *, TG_GPU_CONTEXT_WORK *,
    const struct tg_gpu_power_io *, const struct tg_gpu_power *);
/* Check the GPU-owned fence without notification. STATUS_PENDING is not a pass. */
NTSTATUS TgGpuContextWorkFinished(TG_GPU_CONTEXT_ENGINE *, TG_GPU_CONTEXT_WORK *);
/* Quiesce before notifying VidSch, which may immediately recycle allocations
 * and private submission buffers. Keep the work reference until notification.
 */
NTSTATUS TgGpuQuiesceContextWork(TG_GPU_CONTEXT_ENGINE *, TG_GPU_CONTEXT_WORK *,
    const struct tg_gpu_power_io *, const struct tg_gpu_power *);
/* Only a matching GPU fence plus successful retirement can notify VidSch.
 * Drop context ownership before notification; it may immediately cause object
 * destruction. A failed synchronization retains an engine-only notification
 * record for retry/reset and prevents the next work from overtaking its fence.
 * STATUS_PENDING is not completion, even though NT_SUCCESS accepts it.
 */
NTSTATUS TgGpuSignalContextWork(TG_GPU_CONTEXT_ENGINE *, TG_GPU_CONTEXT_WORK *);
NTSTATUS TgGpuCancelContextNotification(TG_GPU_CONTEXT_ENGINE *, TG_GPU_CONTEXT_WORK *);
/* Releases only after the actual channel-retirement sequence succeeds.
 * Timeout keeps the context/RAMIN/commands pinned. Aborts do not notify a
 * completed fence. An unsubmitted work item can be canceled without MMIO.
 */
NTSTATUS TgGpuRetireContextWork(TG_GPU_CONTEXT_ENGINE *, TG_GPU_CONTEXT_WORK *,
    const struct tg_gpu_power_io *, const struct tg_gpu_power *);
/* Recovery: disable fetch, drain MC, stop clocks and power down. Hardware
 * clamp/reset readback is required before releasing a failed submission.
 * Reinitialize the GPU before subsequent submissions. TDR reporting remains
 * the adapter's responsibility; this routine never invents fence completion.
 */
NTSTATUS TgGpuResetContextWork(TG_GPU_CONTEXT_ENGINE *, TG_GPU_CONTEXT_WORK *,
    const struct tg_gpu_power_io *, struct tg_gpu_power *, struct tg_gpu_clock *);
#endif
