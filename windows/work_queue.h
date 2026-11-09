/* SPDX-License-Identifier: MIT */
#ifndef TEGRA_GPU_WORK_QUEUE_H
#define TEGRA_GPU_WORK_QUEUE_H
#include "context.h"
#include "paging_buffer.h"
#define TG_GPU_DMA_BUFFER_BYTES (64u*1024u)
#define TG_GPU_PAGING_BUFFER_MAGIC 0x50474754u
typedef struct _TG_GPU_PAGING_BUFFER {
    TG_GPU_CONTEXT_ENGINE *Engine;
    PVOID CpuAddress;
    UINT64 GpuAddress;
    UINT BuiltBytes,Capacity,Magic;
} TG_GPU_PAGING_BUFFER;

/* Dxgkrnl supplies nonpaged private storage for each submitted DMA buffer.
 * No user pointers or caller-initialized list links are trusted. This initial
 * virtual backend accepts no UMD-private payload; the entire area belongs to
 * the kernel queue after admission. The adapter must still validate commands
 * and residency before calling the queue, and preserve OS submission order.
 * Kernel-built paging metadata retains an OS-owned immutable DMA buffer; it
 * is never interpreted as a USER GPFIFO workload or an application context.
 */
typedef struct _TG_GPU_QUEUE_ENTRY {
    LIST_ENTRY Link;
    TG_GPU_CONTEXT_WORK Work;
    TG_GPU_PAGING_BUFFER Paging;
} TG_GPU_QUEUE_ENTRY;

typedef struct _TG_GPU_WORK_QUEUE {
    TG_GPU_CONTEXT_ENGINE *Engine;
    KSPIN_LOCK Lock;
    LIST_ENTRY Pending;
    KEVENT Wake;
    TG_GPU_CONTEXT_WORK Current;
    TG_GPU_PAGING_BUFFER CurrentPaging;
    PVOID CurrentPrivate;
    NTSTATUS Failure;
    BOOLEAN Servicing, Paused;
} TG_GPU_WORK_QUEUE;

NTSTATUS TgGpuInitializeWorkQueue(TG_GPU_WORK_QUEUE *,TG_GPU_CONTEXT_ENGINE *);
/* DISPATCH-safe, allocation-free admission using pDmaBufferPrivateData. This
 * is not yet the registered SubmitCommandVirtual callback: internal failures
 * require adapter fault/preempt/TDR handling, not direct DDI propagation.
 */
NTSTATUS TgGpuQueueSubmit(TG_GPU_WORK_QUEUE *,const DXGKARG_SUBMITCOMMANDVIRTUAL *);
/* PASSIVE worker step: start/poll one item, quiesce, then notify its fence.
 * STATUS_PENDING keeps that item active. The adapter waits for Wake plus an
 * IRQ/timer timeout to call again; it must not spin on STATUS_PENDING.
 * No borrowed private-buffer pointer is touched after notifying Dxgkrnl.
 */
NTSTATUS TgGpuServiceWorkQueue(TG_GPU_WORK_QUEUE *,const struct tg_gpu_power_io *,
                             const struct tg_gpu_power *);
/* Close admission and recover the current item before canceling pending work.
 * A failed isolation keeps references. No canceled fence is reported complete.
 * The queue remains paused until the adapter performs cold reinitialization
 * and explicitly resumes it. These helpers do not replace TDR notification.
 */
NTSTATUS TgGpuResetWorkQueue(TG_GPU_WORK_QUEUE *,const struct tg_gpu_power_io *,
                           struct tg_gpu_power *,struct tg_gpu_clock *);
NTSTATUS TgGpuResumeWorkQueue(TG_GPU_WORK_QUEUE *,const struct tg_gpu_power *);
/* Detach only after the current item and the pending list have been retired. */
NTSTATUS TgGpuStopWorkQueue(TG_GPU_WORK_QUEUE *);
#endif
