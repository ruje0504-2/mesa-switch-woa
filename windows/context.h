/* SPDX-License-Identifier: MIT */
#ifndef TEGRA_GPU_CONTEXT_H
#define TEGRA_GPU_CONTEXT_H
#include "root_table.h"

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
};
struct _TG_GPU_DEVICE {
    LIST_ENTRY Link;
    TG_GPU_CONTEXT_ENGINE *Engine;
    TG_GPU_PROCESS *Process;
    HANDLE Runtime;
    UINT Contexts;
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
    volatile UINT LastCompletedFence;
    UINT Marker;
    BOOLEAN Stopping;
};
/* The single physical execution slot is serialized; the number of software
 * contexts is limited only by allocation failures, not a fixed slot array.
 * Keep this nonpaged record alive until ReleaseContextWork succeeds.
 */
struct _TG_GPU_CONTEXT_WORK {
    TG_GPU_CONTEXT *Context;
    TG_GPU_ROOT_TABLE Root;
    UINT64 DmaAddress, FencePhysicalAddress;
    UINT DmaBytes, SubmissionFence, Expected;
    BOOLEAN Signaling, Notified;
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
/* Poll the GPU-owned fence in private uncached memory. Only the expected
 * hardware value can notify VidSch. Still retain RAMIN/root ownership after
 * notification: completion alone does not evict a channel's loaded context.
 * STATUS_PENDING is not completion, even though NT_SUCCESS accepts it.
 */
NTSTATUS TgGpuSignalContextWork(TG_GPU_CONTEXT_ENGINE *, TG_GPU_CONTEXT_WORK *);
/* The hardware backend must stop/unbind the channel and drain its references,
 * or complete a GPU reset, BEFORE passing HardwareRetired=TRUE. Reset failure
 * retains ownership. Aborted work must not manufacture a completed fence.
 */
NTSTATUS TgGpuReleaseContextWork(TG_GPU_CONTEXT_ENGINE *, TG_GPU_CONTEXT_WORK *,
                               BOOLEAN HardwareRetired);
#endif
