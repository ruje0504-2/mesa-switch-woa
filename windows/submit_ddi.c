/* SPDX-License-Identifier: MIT */
#include "submit_ddi.h"
#include "work_queue.h"
#include "hardware.h"

NTSTATUS APIENTRY TgGpuDdiSubmitCommandVirtual(HANDLE adapter,const DXGKARG_SUBMITCOMMANDVIRTUAL *a)
{
    TG_GPU_CONTEXT_ENGINE *e=adapter;TG_GPU_QUEUE_ENTRY *reserved;TG_GPU_HARDWARE *h;
    TG_GPU_WORK_QUEUE *q;NTSTATUS status;BOOLEAN used=FALSE;KIRQL irql;
    PAGED_CODE();
    if(!e || !a || !e->Dxgk.DeviceHandle)return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&e->Lock,&irql);++e->Creating;q=e->QueueOwner;KeReleaseSpinLock(&e->Lock,irql);
    if(q) {
        status=KeWaitForSingleObject(&q->DispatchGate,Executive,KernelMode,FALSE,NULL);
        if(status!=STATUS_SUCCESS) {
            (VOID)TgGpuRequestEngineRecovery(e,NT_SUCCESS(status)?STATUS_DEVICE_BUSY:status);
            KeAcquireSpinLock(&e->Lock,&irql);--e->Creating;KeReleaseSpinLock(&e->Lock,irql);return STATUS_SUCCESS;
        }
    }
    /* Reserve before inspecting the submission. Allocation failure is an
     * internal admission failure, not a malformed-packet diagnosis. Accept
     * into recovery without touching commands or reporting completion. */
    reserved=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*reserved),TG_GPU_QUEUE_TAG);
    h=e->HardwareOwner;
    if(!reserved)status=STATUS_NO_MEMORY;
    else if(!q || q->Engine!=e || !h || h->Engine!=e || !h->Ready || !h->Power.owned || !h->Power.ready)
        status=STATUS_DEVICE_NOT_READY;
    else status=TgGpuQueueSubmitOrReject(q,a,reserved,&used);
    if(reserved && !used)ExFreePoolWithTag(reserved,TG_GPU_QUEUE_TAG);
    if(status==STATUS_INVALID_PARAMETER && used) {
        NTSTATUS drain=TgGpuDrainRejectedIdle(q);
        if(!NT_SUCCESS(drain))(VOID)TgGpuRequestEngineRecovery(e,drain);
    } else if(status!=STATUS_SUCCESS)(VOID)TgGpuRequestEngineRecovery(e,status);
    if(q)(VOID)KeReleaseMutex(&q->DispatchGate,FALSE);
    KeAcquireSpinLock(&e->Lock,&irql);--e->Creating;KeReleaseSpinLock(&e->Lock,irql);
    if(status==STATUS_INVALID_PARAMETER && used)return STATUS_INVALID_PARAMETER;
    /* Busy, OOM, lost root and transport failures must never escape this DDI:
     * Microsoft permits only SUCCESS or malformed INVALID_PARAMETER here. */
    return STATUS_SUCCESS;
}
void TgGpuRegisterSubmissionDdi(DRIVER_INITIALIZATION_DATA *d)
{d->DxgkDdiSubmitCommandVirtual=TgGpuDdiSubmitCommandVirtual;}
