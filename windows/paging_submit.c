/* SPDX-License-Identifier: MIT */
#include "context.h"
#include "paging_buffer.h"

NTSTATUS TgGpuExecutePagingWork(TG_GPU_CONTEXT_ENGINE *e,TG_GPU_CONTEXT_WORK *w,
    const void *buffer,SIZE_T bytes,const struct tg_gpu_power_io *io,const struct tg_gpu_power *power)
{
    KIRQL irql;NTSTATUS status;SIZE_T completed;
    if(!e || !w || w->Engine!=e || !w->Paging || w->Context)return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&e->Lock,&irql);
    if(w->CompletionReady) {
        status=e->Notification==w?STATUS_SUCCESS:STATUS_INVALID_DEVICE_STATE;
        KeReleaseSpinLock(&e->Lock,irql);return status;
    }
    if(w->Attempted || w->HardwareBusy) {
        KeReleaseSpinLock(&e->Lock,irql);return STATUS_INVALID_DEVICE_STATE;
    }
    if(e->Active || e->Notification || e->Channel.referenced) {
        KeReleaseSpinLock(&e->Lock,irql);return STATUS_PENDING;
    }
    e->Active=w;w->Attempted=w->HardwareBusy=TRUE;
    KeReleaseSpinLock(&e->Lock,irql);
    /* Earlier graphics work has retired before this slot is acquired. A full
     * cache synchronization precedes changing its mappings. Invalidation is
     * performed in the encoded record order; a partial result is not a fence.
     */
    struct tg_gpu_frontend_status sync={0};
    status=tg_gpu_frontend_sync_memory(io,power,&sync)==TG_GPU_FE_OK?STATUS_SUCCESS:STATUS_DEVICE_DATA_ERROR;
    if(NT_SUCCESS(status))status=TgGpuExecutePagingBuffer(e->Tables,buffer,bytes,TRUE,io,power,&completed);
    KeAcquireSpinLock(&e->Lock,&irql);w->HardwareBusy=FALSE;
    if(status==STATUS_SUCCESS) {
        e->Active=NULL;e->Notification=w;w->CompletionReady=TRUE;
    }
    /* Failure retains the slot and all OS residency until verified reset. */
    KeReleaseSpinLock(&e->Lock,irql);return status;
}
