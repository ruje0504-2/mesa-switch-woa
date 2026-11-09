/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_WINDOWS_WORKER_H
#define TG_GPU_WINDOWS_WORKER_H
#include "work_queue.h"
#include "hardware.h"
typedef struct _TG_GPU_WORKER {
    TG_GPU_WORK_QUEUE *Queue;
    TG_GPU_HARDWARE *Hardware;
    HANDLE Thread;
    KEVENT Stop;
    NTSTATUS Failure;
} TG_GPU_WORKER;
/* PASSIVE, serialized adapter lifetime. Queue/hardware/IO must remain alive
 * until Stop joins and closes the kernel handle. Faults pause admission;
 * reporting/recovering a WDDM TDR is still separate. */
NTSTATUS TgGpuStartWorker(TG_GPU_WORKER *,TG_GPU_WORK_QUEUE *,TG_GPU_HARDWARE *);
NTSTATUS TgGpuStopWorker(TG_GPU_WORKER *);
#endif
