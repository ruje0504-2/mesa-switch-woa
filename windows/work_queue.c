/* SPDX-License-Identifier: MIT */
#include "work_queue.h"

NTSTATUS TgGpuInitializeWorkQueue(TG_GPU_WORK_QUEUE *q,TG_GPU_CONTEXT_ENGINE *e)
{
    KIRQL irql;
    if(!q || !e || !e->Commands.Handle) return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&e->Lock,&irql);
    if(e->Stopping || e->QueueOwner || e->Active || e->Notification) {
        KeReleaseSpinLock(&e->Lock,irql);return STATUS_DEVICE_BUSY;
    }
    RtlZeroMemory(q,sizeof(*q));q->Engine=e;
    KeInitializeSpinLock(&q->Lock);InitializeListHead(&q->Pending);
    KeInitializeEvent(&q->Wake,SynchronizationEvent,FALSE);e->QueueOwner=q;
    KeReleaseSpinLock(&e->Lock,irql);return STATUS_SUCCESS;
}
NTSTATUS TgGpuQueueSubmit(TG_GPU_WORK_QUEUE *q,const DXGKARG_SUBMITCOMMANDVIRTUAL *a)
{
    TG_GPU_QUEUE_ENTRY *entry;LIST_ENTRY *l;KIRQL irql,engine_irql;NTSTATUS status;
    if(!q || !q->Engine || !a || !a->pDmaBufferPrivateData ||
        ((ULONG_PTR)a->pDmaBufferPrivateData&(TYPE_ALIGNMENT(TG_GPU_QUEUE_ENTRY)-1)) ||
        a->DmaBufferPrivateDataSize<sizeof(*entry) || a->DmaBufferUmdPrivateDataSize ||
        (a->Flags.Value&~1u)) return STATUS_INVALID_PARAMETER;
    entry=a->pDmaBufferPrivateData;
    KeAcquireSpinLock(&q->Lock,&irql);
    if(q->Paused || !NT_SUCCESS(q->Failure)) {
        KeReleaseSpinLock(&q->Lock,irql);return STATUS_DEVICE_NOT_READY;
    }
    if(q->CurrentPrivate==entry) { KeReleaseSpinLock(&q->Lock,irql);return STATUS_DEVICE_BUSY; }
    for(l=q->Pending.Flink;l!=&q->Pending;l=l->Flink)
        if(CONTAINING_RECORD(l,TG_GPU_QUEUE_ENTRY,Link)==entry) {
            KeReleaseSpinLock(&q->Lock,irql);return STATUS_DEVICE_BUSY;
        }
    if(a->Flags.Paging) {
        const TG_GPU_PAGING_BUFFER *p=&entry->Paging;UINT64 offset;
        if(a->NodeOrdinal || a->EngineOrdinal || p->Magic!=TG_GPU_PAGING_BUFFER_MAGIC ||
           p->Engine!=q->Engine || !p->CpuAddress || ((ULONG_PTR)p->CpuAddress&7) ||
           p->Capacity>TG_GPU_DMA_BUFFER_BYTES || p->BuiltBytes>p->Capacity ||
           a->DmaBufferVirtualAddress<p->GpuAddress) {
            KeReleaseSpinLock(&q->Lock,irql);return STATUS_INVALID_PARAMETER;
        }
        offset=a->DmaBufferVirtualAddress-p->GpuAddress;
        if(offset>p->BuiltBytes || a->DmaBufferSize>p->BuiltBytes-offset ||
           (offset&7) || (a->DmaBufferSize&7)) {
            KeReleaseSpinLock(&q->Lock,irql);return STATUS_INVALID_PARAMETER;
        }
        status=TgGpuValidatePagingBuffer(q->Engine->Tables,(UCHAR *)p->CpuAddress+(SIZE_T)offset,a->DmaBufferSize);
        if(NT_SUCCESS(status)) {
            KeAcquireSpinLock(&q->Engine->Lock,&engine_irql);
            status=q->Engine->Stopping?STATUS_DEVICE_NOT_READY:STATUS_SUCCESS;
            if(NT_SUCCESS(status)) {
                RtlZeroMemory(&entry->Work,sizeof(entry->Work));
                entry->Work.Engine=q->Engine;entry->Work.Paging=TRUE;
                entry->Work.SubmissionFence=a->SubmissionFenceId;
                entry->Work.DmaAddress=offset;entry->Work.DmaBytes=a->DmaBufferSize;
            }
            KeReleaseSpinLock(&q->Engine->Lock,engine_irql);
        }
    } else {
        RtlZeroMemory(entry,sizeof(*entry));
        status=TgGpuCaptureContextWork(q->Engine,a,&entry->Work);
    }
    if(NT_SUCCESS(status)) InsertTailList(&q->Pending,&entry->Link);
    KeReleaseSpinLock(&q->Lock,irql);
    if(NT_SUCCESS(status)) (VOID)KeSetEvent(&q->Wake,IO_NO_INCREMENT,FALSE);
    return status;
}
NTSTATUS TgGpuServiceWorkQueue(TG_GPU_WORK_QUEUE *q,const struct tg_gpu_power_io *io,
                             const struct tg_gpu_power *power)
{
    TG_GPU_QUEUE_ENTRY *entry;TG_GPU_CONTEXT_WORK *w;KIRQL irql;NTSTATUS status;
    BOOLEAN wake=FALSE;
    if(!q || !q->Engine) return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&q->Lock,&irql);
    if(q->Servicing) { KeReleaseSpinLock(&q->Lock,irql);return STATUS_DEVICE_BUSY; }
    if(q->Paused || !NT_SUCCESS(q->Failure)) {
        status=q->Paused ? STATUS_DEVICE_NOT_READY : q->Failure;
        KeReleaseSpinLock(&q->Lock,irql);return status;
    }
    q->Servicing=TRUE;w=&q->Current;
    if(!w->Engine && !IsListEmpty(&q->Pending)) {
        entry=CONTAINING_RECORD(RemoveHeadList(&q->Pending),TG_GPU_QUEUE_ENTRY,Link);
        *w=entry->Work;q->CurrentPrivate=entry;q->CurrentPaging=entry->Paging;
        /* Transfer the retained reference to queue-owned storage. Dxgkrnl may
         * reuse its original buffer immediately from the completion callback.
         */
        entry->Work.Context=NULL;
    }
    KeReleaseSpinLock(&q->Lock,irql);
    status=STATUS_SUCCESS;
    if(!w->Engine) goto done;
    if(w->Paging) {
        status=TgGpuExecutePagingWork(q->Engine,w,
            (UCHAR *)q->CurrentPaging.CpuAddress+(SIZE_T)w->DmaAddress,w->DmaBytes,io,power);
        if(status!=STATUS_SUCCESS)goto done;
        goto notify;
    }
    if(w->CompletionReady)goto notify;
    if(!w->Expected) {
        status=TgGpuActivateContextWork(q->Engine,w);
        if(status==STATUS_DEVICE_BUSY) { status=STATUS_PENDING;goto done; }
        if(!NT_SUCCESS(status)) goto done;
    }
    if(!w->Prepared) {
        status=TgGpuPrepareContextWork(q->Engine,w);
        if(!NT_SUCCESS(status)) goto done;
    }
    if(!w->Attempted) {
        status=TgGpuSubmitContextWork(q->Engine,w,io,power);
        if(!NT_SUCCESS(status)) goto done;
    }
    status=TgGpuObserveContextWork(q->Engine,w,io,power);
    if(status!=STATUS_SUCCESS) goto done;
    if(!w->Retired) {
        status=TgGpuQuiesceContextWork(q->Engine,w,io,power);
        if(!NT_SUCCESS(status)) goto done;
    }
notify:
    KeAcquireSpinLock(&q->Lock,&irql);q->CurrentPrivate=NULL;KeReleaseSpinLock(&q->Lock,irql);
    status=TgGpuSignalContextWork(q->Engine,w);
    if(status!=STATUS_SUCCESS) goto done;
    RtlZeroMemory(w,sizeof(*w));RtlZeroMemory(&q->CurrentPaging,sizeof(q->CurrentPaging));
    KeAcquireSpinLock(&q->Lock,&irql);wake=!IsListEmpty(&q->Pending);KeReleaseSpinLock(&q->Lock,irql);
done:
    KeAcquireSpinLock(&q->Lock,&irql);
    if(!NT_SUCCESS(status)) q->Failure=status;
    q->Servicing=FALSE;KeReleaseSpinLock(&q->Lock,irql);
    if(wake) (VOID)KeSetEvent(&q->Wake,IO_NO_INCREMENT,FALSE);
    return status;
}
NTSTATUS TgGpuResetWorkQueue(TG_GPU_WORK_QUEUE *q,const struct tg_gpu_power_io *io,
                           struct tg_gpu_power *power,struct tg_gpu_clock *clock)
{
    TG_GPU_QUEUE_ENTRY *entry;TG_GPU_CONTEXT_WORK *w;KIRQL irql;NTSTATUS status=STATUS_SUCCESS;
    if(!q || !q->Engine) return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&q->Lock,&irql);q->Paused=TRUE;
    if(q->Servicing) { KeReleaseSpinLock(&q->Lock,irql);return STATUS_DEVICE_BUSY; }
    q->Servicing=TRUE;w=&q->Current;KeReleaseSpinLock(&q->Lock,irql);
    if(w->CompletionReady && !w->Notified) {
        status=TgGpuCancelContextNotification(q->Engine,w);
        if(!NT_SUCCESS(status)) goto done;
        RtlZeroMemory(w,sizeof(*w));q->CurrentPrivate=NULL;
    }
    if(w->Paging && w->Attempted && !w->CompletionReady) {
        status=TgGpuResetContextWork(q->Engine,w,io,power,clock);
        if(!NT_SUCCESS(status))goto done;
    } else if(w->Context) {
        if(!w->Expected) status=TgGpuCancelCapturedWork(q->Engine,w);
        else if(!w->Attempted) status=TgGpuRetireContextWork(q->Engine,w,NULL,NULL);
        else status=TgGpuResetContextWork(q->Engine,w,io,power,clock);
        if(!NT_SUCCESS(status)) goto done;
    }
    RtlZeroMemory(w,sizeof(*w));q->CurrentPrivate=NULL;
    RtlZeroMemory(&q->CurrentPaging,sizeof(q->CurrentPaging));
    /* Admission is paused and this worker is exclusive. Clear each borrowed
     * queue entry before the adapter reports reset/preemption to Dxgkrnl.
     */
    while(!IsListEmpty(&q->Pending)) {
        entry=CONTAINING_RECORD(q->Pending.Flink,TG_GPU_QUEUE_ENTRY,Link);
        status=entry->Work.Paging?STATUS_SUCCESS:TgGpuCancelCapturedWork(q->Engine,&entry->Work);
        if(!NT_SUCCESS(status)) goto done;
        RemoveEntryList(&entry->Link);RtlZeroMemory(entry,sizeof(*entry));
    }
done:
    KeAcquireSpinLock(&q->Lock,&irql);q->Failure=status;q->Servicing=FALSE;KeReleaseSpinLock(&q->Lock,irql);
    return status;
}
NTSTATUS TgGpuResumeWorkQueue(TG_GPU_WORK_QUEUE *q,const struct tg_gpu_power *power)
{
    KIRQL irql;NTSTATUS status=STATUS_SUCCESS;
    if(!q || !q->Engine || !power) return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&q->Lock,&irql);
    if(q->Servicing || q->Current.Engine || !IsListEmpty(&q->Pending) || !NT_SUCCESS(q->Failure))
        status=STATUS_DEVICE_BUSY;
    else if(!power->owned || !power->ready || !q->Engine->UserdBar1Ready) status=STATUS_DEVICE_NOT_READY;
    else q->Paused=FALSE;
    KeReleaseSpinLock(&q->Lock,irql);return status;
}
NTSTATUS TgGpuStopWorkQueue(TG_GPU_WORK_QUEUE *q)
{
    KIRQL irql,engine_irql;TG_GPU_CONTEXT_ENGINE *e;
    if(!q || !q->Engine) return STATUS_INVALID_PARAMETER;
    e=q->Engine;KeAcquireSpinLock(&q->Lock,&irql);q->Paused=TRUE;
    if(q->Servicing || q->Current.Engine || !IsListEmpty(&q->Pending)) {
        KeReleaseSpinLock(&q->Lock,irql);return STATUS_DEVICE_BUSY;
    }
    KeAcquireSpinLock(&e->Lock,&engine_irql);e->QueueOwner=NULL;KeReleaseSpinLock(&e->Lock,engine_irql);
    q->Engine=NULL;KeReleaseSpinLock(&q->Lock,irql);return STATUS_SUCCESS;
}
