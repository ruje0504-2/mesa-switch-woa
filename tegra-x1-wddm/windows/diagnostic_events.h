/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_DIAGNOSTIC_EVENTS_H
#define TG_GPU_DIAGNOSTIC_EVENTS_H
#include <ntddk.h>

/* Provider c27c1f9b-62e8-4e21-b29b-6bb2d585c6d0, event 1/version 1.
 * Fixed-width payload; no referenced command data or pointer dereferences. */
enum tg_gpu_recovery_source {
    TgGpuRecoveryUnspecified, TgGpuRecoverySubmitGate, TgGpuRecoverySubmit,
    TgGpuRecoveryRejectDrain, TgGpuRecoveryPreempt, TgGpuRecoveryWorker,
    TgGpuRecoveryLegacySubmit
};
typedef struct _TG_GPU_RECOVERY_EVENT {
    ULONG Version,Source;
    LUID AdapterLuid;
    NTSTATUS Status;
    ULONG Flags;
    ULONGLONG Context;
    ULONG DmaBytes,PrivateBytes,UmdPrivateBytes,SubmissionFence;
    ULONG LastCompletedFence,GraphicsGeneration;
    ULONG CachedFifo,CachedMmu,CachedPbdma,CachedPbdma1;
} TG_GPU_RECOVERY_EVENT;
C_ASSERT(sizeof(TG_GPU_RECOVERY_EVENT)==72);

/* Event 2/version 1: queue state at preemption, failure and reset boundaries.
 * Addresses are opaque identities; the trace never reads command memory. */
enum tg_gpu_execution_source {
    TgGpuExecutionPreempt=1, TgGpuExecutionWorkerFailure,
    TgGpuExecutionSubmitFailure, TgGpuExecutionResetBegin, TgGpuExecutionResetEnd
};
enum tg_gpu_execution_stage {
    TgGpuExecutionIdle, TgGpuExecutionSoftware, TgGpuExecutionPaging,
    TgGpuExecutionMappings, TgGpuExecutionActivate, TgGpuExecutionPrepare,
    TgGpuExecutionSubmit, TgGpuExecutionGpuFence, TgGpuExecutionRetire,
    TgGpuExecutionCpuFence, TgGpuExecutionNotify, TgGpuExecutionPreemptPrefix
};
typedef struct _TG_GPU_EXECUTION_EVENT {
    ULONG Version,Source;
    LUID AdapterLuid;
    NTSTATUS Status;
    ULONG RequestFence,RequestFlags,Stage;
    ULONGLONG RequestContext,Context,DmaAddress;
    ULONG SubmissionFence,LastCompletedFence,Expected,Observed;
    ULONG DmaBytes,WorkFlags,QueueFlags;
    NTSTATUS QueueFailure;
    ULONG GraphicsGeneration,ChannelStage,ChannelRegister,ChannelValue;
    ULONG CachedFifo,CachedMmu,CachedPbdma,CachedPbdma1;
    ULONG CachedMc,CachedGr,CachedFecsError,CachedFecsHost;
    ULONG PollCount,RequestDmaBytes,PrivateBytes,UmdPrivateBytes;
} TG_GPU_EXECUTION_EVENT;
C_ASSERT(sizeof(TG_GPU_EXECUTION_EVENT)==152);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS TgGpuTraceInitialize(VOID);
_IRQL_requires_(PASSIVE_LEVEL)
VOID TgGpuTraceShutdown(VOID);
/* EtwWrite accepts nonpaged system-memory payloads at elevated IRQL. */
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID TgGpuTraceRecovery(const TG_GPU_RECOVERY_EVENT *);
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID TgGpuTraceExecution(const TG_GPU_EXECUTION_EVENT *);
#endif
