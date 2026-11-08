/* SPDX-License-Identifier: MIT */
#include "context.h"

#define TAG 0x43674754u

static PVOID object(SIZE_T bytes)
{ return ExAllocatePool2(POOL_FLAG_NON_PAGED, bytes, TAG); }
static TG_GPU_PROCESS *find_process(TG_GPU_CONTEXT_ENGINE *e, HANDLE h)
{
    LIST_ENTRY *l;
    for (l=e->Processes.Flink;l!=&e->Processes;l=l->Flink) {
        TG_GPU_PROCESS *p=CONTAINING_RECORD(l,TG_GPU_PROCESS,Link);
        if (p==h) return p;
    }
    return NULL;
}
static TG_GPU_CONTEXT *find_context(TG_GPU_CONTEXT_ENGINE *e, HANDLE h)
{
    LIST_ENTRY *l;
    for (l=e->Contexts.Flink;l!=&e->Contexts;l=l->Flink) {
        TG_GPU_CONTEXT *c=CONTAINING_RECORD(l,TG_GPU_CONTEXT,Link);
        if (c==h) return c;
    }
    return NULL;
}
NTSTATUS TgGpuInitializeContextEngine(TG_GPU_CONTEXT_ENGINE *e,
    const DXGKRNL_INTERFACE *dxgk, const TG_GPU_TABLE_SEGMENT *tables)
{
    if (!e || !dxgk || !tables || !tables->Memory.Handle || !tables->SegmentId ||
        dxgk->Version<DXGKDDI_INTERFACE_VERSION_WDDM2_4 ||
        !dxgk->DxgkCbAllocateContiguousMemory || !dxgk->DxgkCbFreeContiguousMemory ||
        !dxgk->DxgkCbSynchronizeExecution || !dxgk->DxgkCbNotifyInterrupt ||
        !dxgk->DxgkCbQueueDpc || !dxgk->DxgkCbNotifyDpc ||
        !dxgk->DxgkCbReserveGpuVirtualAddressRange)
        return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(e,sizeof(*e));
    e->Dxgk=*dxgk; e->Tables=tables;
    KeInitializeSpinLock(&e->Lock);
    InitializeListHead(&e->Processes); InitializeListHead(&e->Devices); InitializeListHead(&e->Contexts);
    return TgGpuAllocatePrivateMemory(dxgk,TG_GPU_COMMAND_BYTES,0x80000000,0xffffffff,&e->Commands);
}
NTSTATUS TgGpuStopContextEngine(TG_GPU_CONTEXT_ENGINE *e)
{
    KIRQL irql; NTSTATUS status;
    if (!e) return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&e->Lock,&irql);
    e->Stopping=TRUE;
    status=e->Active || e->Channel.referenced || e->Creating || !IsListEmpty(&e->Contexts) || !IsListEmpty(&e->Devices) ||
        !IsListEmpty(&e->Processes) ? STATUS_DEVICE_BUSY : STATUS_SUCCESS;
    KeReleaseSpinLock(&e->Lock,irql);
    if (!NT_SUCCESS(status)) return status;
    return TgGpuFreePrivateMemory(&e->Dxgk,&e->Commands);
}
NTSTATUS APIENTRY TgGpuDdiCreateProcess(HANDLE adapter, DXGKARG_CREATEPROCESS *a)
{
    TG_GPU_CONTEXT_ENGINE *e=adapter; TG_GPU_PROCESS *p; KIRQL irql; NTSTATUS status;
    if (!e || !a) return STATUS_INVALID_PARAMETER;
    p=object(sizeof(*p)); if (!p) return STATUS_NO_MEMORY;
    p->Engine=e; p->Runtime=a->hDxgkProcess; p->Flags=a->Flags;
    KeAcquireSpinLock(&e->Lock,&irql);
    if (e->Stopping) { KeReleaseSpinLock(&e->Lock,irql); ExFreePoolWithTag(p,TAG); return STATUS_DEVICE_NOT_READY; }
    ++e->Creating; KeReleaseSpinLock(&e->Lock,irql);
    status=TgGpuCreateInternalVa(&e->Dxgk,p->Runtime,&e->Commands,&p->Internal);
    KeAcquireSpinLock(&e->Lock,&irql); --e->Creating;
    if (!NT_SUCCESS(status)) {
        KeReleaseSpinLock(&e->Lock,irql); ExFreePoolWithTag(p,TAG); return status;
    }
    InsertTailList(&e->Processes,&p->Link);
    KeReleaseSpinLock(&e->Lock,irql);
    a->hKmdProcess=p;
    return STATUS_SUCCESS;
}
NTSTATUS APIENTRY TgGpuDdiDestroyProcess(HANDLE adapter, HANDLE process)
{
    TG_GPU_CONTEXT_ENGINE *e=adapter; TG_GPU_PROCESS *p; KIRQL irql; NTSTATUS status;
    if (!e) return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&e->Lock,&irql); p=find_process(e,process);
    if (!p || p->Devices || p->Closing) { KeReleaseSpinLock(&e->Lock,irql); return p ? STATUS_DEVICE_BUSY : STATUS_INVALID_PARAMETER; }
    p->Closing=TRUE; KeReleaseSpinLock(&e->Lock,irql);
    status=TgGpuFreePrivateMemory(&e->Dxgk,&p->Internal.Leaf);
    KeAcquireSpinLock(&e->Lock,&irql);
    if (!NT_SUCCESS(status)) { p->Closing=FALSE; KeReleaseSpinLock(&e->Lock,irql); return status; }
    RemoveEntryList(&p->Link); KeReleaseSpinLock(&e->Lock,irql);
    ExFreePoolWithTag(p,TAG); return STATUS_SUCCESS;
}
NTSTATUS APIENTRY TgGpuDdiCreateDevice(HANDLE adapter, DXGKARG_CREATEDEVICE *a)
{
    TG_GPU_CONTEXT_ENGINE *e=adapter; TG_GPU_DEVICE *d; TG_GPU_PROCESS *p; KIRQL irql;
    if (!e || !a) return STATUS_INVALID_PARAMETER;
    d=object(sizeof(*d)); if (!d) return STATUS_NO_MEMORY;
    KeAcquireSpinLock(&e->Lock,&irql); p=find_process(e,a->hKmdProcess);
    if (e->Stopping || (p && p->Closing) || (!p && (!a->Flags.SystemDevice || a->hKmdProcess))) {
        KeReleaseSpinLock(&e->Lock,irql); ExFreePoolWithTag(d,TAG); return STATUS_INVALID_PARAMETER;
    }
    d->Engine=e; d->Process=p; d->Runtime=a->hDevice;
    if (p) ++p->Devices;
    InsertTailList(&e->Devices,&d->Link); KeReleaseSpinLock(&e->Lock,irql);
    a->hDevice=d; return STATUS_SUCCESS;
}
NTSTATUS APIENTRY TgGpuDdiDestroyDevice(HANDLE device)
{
    TG_GPU_DEVICE *d=device; TG_GPU_CONTEXT_ENGINE *e; KIRQL irql;
    if (!d) return STATUS_INVALID_PARAMETER;
    e=d->Engine; KeAcquireSpinLock(&e->Lock,&irql);
    if (d->Contexts) { KeReleaseSpinLock(&e->Lock,irql); return STATUS_DEVICE_BUSY; }
    RemoveEntryList(&d->Link); if (d->Process) --d->Process->Devices;
    KeReleaseSpinLock(&e->Lock,irql); ExFreePoolWithTag(d,TAG); return STATUS_SUCCESS;
}
NTSTATUS APIENTRY TgGpuDdiCreateContext(HANDLE device, DXGKARG_CREATECONTEXT *a)
{
    TG_GPU_DEVICE *d=device; TG_GPU_CONTEXT_ENGINE *e; TG_GPU_CONTEXT *c;
    KIRQL irql; NTSTATUS status;
    if (!d || !a) return STATUS_INVALID_PARAMETER;
    /* One node/physical engine; no protected or hardware-queue capability is
     * advertised. A software context does not consume a hardware FIFO slot.
     */
    if (a->NodeOrdinal || (a->EngineAffinity & ~1u) || a->Flags.SystemProtectedContext ||
        a->Flags.HwQueueSupported || a->PrivateDriverDataSize)
        return STATUS_GRAPHICS_DRIVER_MISMATCH;
    e=d->Engine; c=object(sizeof(*c)); if (!c) return STATUS_NO_MEMORY;
    c->Device=d; c->Runtime=a->hContext; c->Flags=a->Flags; c->RootStatus=STATUS_DEVICE_NOT_READY;
    KeAcquireSpinLock(&e->Lock,&irql);
    if (e->Stopping) { KeReleaseSpinLock(&e->Lock,irql); ExFreePoolWithTag(c,TAG); return STATUS_DEVICE_NOT_READY; }
    ++d->Contexts; /* reserve the parent while allocation may sleep */
    KeReleaseSpinLock(&e->Lock,irql);
    status=TgGpuAllocatePrivateMemory(&e->Dxgk,PAGE_SIZE,0x80000000,0xffffffff,&c->Instance);
    KeAcquireSpinLock(&e->Lock,&irql);
    if (!NT_SUCCESS(status)) {
        --d->Contexts; KeReleaseSpinLock(&e->Lock,irql); ExFreePoolWithTag(c,TAG); return status;
    }
    InsertTailList(&e->Contexts,&c->Link); KeReleaseSpinLock(&e->Lock,irql);
    RtlZeroMemory(&a->ContextInfo,sizeof(a->ContextInfo));
    a->ContextInfo.DmaBufferSize=64*1024;
    a->ContextInfo.Caps.NoPatchingRequired=a->Flags.VirtualAddressing;
    /* VidMm manages residency. No system-IOMMU capability or GDI acceleration
     * is implied by creating the object. Submission/paging dispatch is separate.
     */
    a->hContext=c; return STATUS_SUCCESS;
}
NTSTATUS APIENTRY TgGpuDdiDestroyContext(HANDLE context)
{
    TG_GPU_CONTEXT *c=context; TG_GPU_CONTEXT_ENGINE *e; KIRQL irql; NTSTATUS status;
    if (!c) return STATUS_INVALID_PARAMETER;
    e=c->Device->Engine; KeAcquireSpinLock(&e->Lock,&irql);
    if (c->References || c->Closing) { KeReleaseSpinLock(&e->Lock,irql); return STATUS_DEVICE_BUSY; }
    c->Closing=TRUE; KeReleaseSpinLock(&e->Lock,irql);
    status=TgGpuFreePrivateMemory(&e->Dxgk,&c->Instance);
    KeAcquireSpinLock(&e->Lock,&irql);
    if (!NT_SUCCESS(status)) {
        c->Closing=FALSE; KeReleaseSpinLock(&e->Lock,irql); return status;
    }
    RemoveEntryList(&c->Link); --c->Device->Contexts;
    KeReleaseSpinLock(&e->Lock,irql); ExFreePoolWithTag(c,TAG); return STATUS_SUCCESS;
}
SIZE_T APIENTRY TgGpuDdiGetRootPageTableSize(HANDLE adapter, DXGKARG_GETROOTPAGETABLESIZE *a)
{ return adapter ? TgGpuRootTableSize(a) : 0; }
VOID APIENTRY TgGpuDdiSetRootPageTable(HANDLE adapter, const DXGKARG_SETROOTPAGETABLE *a)
{
    TG_GPU_CONTEXT_ENGINE *e=adapter; TG_GPU_CONTEXT *c; KIRQL irql;
    if (!e || !a) return;
    KeAcquireSpinLock(&e->Lock,&irql); c=find_context(e,a->hContext);
    if (c) {
        if (c->References || c->Closing) c->RootStatus=STATUS_GRAPHICS_ALLOCATION_BUSY;
        else {
            c->RootStatus=TgGpuResolveRootTable(e->Tables,a,&c->Root);
            if (NT_SUCCESS(c->RootStatus)) c->RootStatus=c->Device->Process ?
                TgGpuInstallInternalRoot(e->Tables,&c->Root,&c->Device->Process->Internal) : STATUS_INVALID_DEVICE_STATE;
            if (NT_SUCCESS(c->RootStatus)) c->RootStatus=TgGpuWriteRootInstance(&c->Root,&c->Instance,TRUE);
        }
        /* This DDI has no return status. Never schedule a stale root after a
         * failed change; existing work retains its own validated snapshot.
         */
        if (!NT_SUCCESS(c->RootStatus)) c->Root.Valid=FALSE;
    }
    KeReleaseSpinLock(&e->Lock,irql);
}
void TgGpuRegisterContextDdis(DRIVER_INITIALIZATION_DATA *d)
{
    d->DxgkDdiCreateProcess=TgGpuDdiCreateProcess;
    d->DxgkDdiDestroyProcess=TgGpuDdiDestroyProcess;
    d->DxgkDdiCreateDevice=TgGpuDdiCreateDevice;
    d->DxgkDdiDestroyDevice=TgGpuDdiDestroyDevice;
    d->DxgkDdiCreateContext=TgGpuDdiCreateContext;
    d->DxgkDdiDestroyContext=TgGpuDdiDestroyContext;
    d->DxgkDdiGetRootPageTableSize=TgGpuDdiGetRootPageTableSize;
    d->DxgkDdiSetRootPageTable=TgGpuDdiSetRootPageTable;
    d->DxgkDdiQueryCurrentFence=TgGpuDdiQueryCurrentFence;
    d->DxgkDdiDpcRoutine=TgGpuDdiDpcRoutine;
}
NTSTATUS APIENTRY TgGpuDdiQueryCurrentFence(HANDLE adapter, DXGKARG_QUERYCURRENTFENCE *a)
{
    TG_GPU_CONTEXT_ENGINE *e=adapter;
    if (!e || !a || a->NodeOrdinal || a->EngineOrdinal) return STATUS_INVALID_PARAMETER;
    KeMemoryBarrier(); a->CurrentFence=e->LastCompletedFence; return STATUS_SUCCESS;
}
VOID TgGpuDdiDpcRoutine(PVOID adapter)
{
    TG_GPU_CONTEXT_ENGINE *e=adapter;
    e->Dxgk.DxgkCbNotifyDpc(e->Dxgk.DeviceHandle);
}

NTSTATUS TgGpuAcquireContextWork(TG_GPU_CONTEXT_ENGINE *e,
    const DXGKARG_SUBMITCOMMANDVIRTUAL *a, TG_GPU_CONTEXT_WORK *w)
{
    TG_GPU_CONTEXT *c; KIRQL irql; UINT marker;
    if (!e || !a || !w || w->Context || a->NodeOrdinal || a->EngineOrdinal ||
        !a->DmaBufferSize || (a->DmaBufferSize & 3) || (a->DmaBufferVirtualAddress & 3))
        return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&e->Lock,&irql); c=find_context(e,a->hContext);
    if (e->Stopping || e->Active) { KeReleaseSpinLock(&e->Lock,irql); return STATUS_DEVICE_BUSY; }
    if (!c || c->Closing || !c->Device->Process || !c->Flags.VirtualAddressing || !c->Root.Valid ||
        !NT_SUCCESS(c->RootStatus) || a->DmaBufferVirtualAddress>=c->Root.VaLimit ||
        a->DmaBufferSize>c->Root.VaLimit-a->DmaBufferVirtualAddress) {
        KeReleaseSpinLock(&e->Lock,irql); return STATUS_INVALID_PARAMETER;
    }
    if (a->DmaBufferVirtualAddress<c->Device->Process->Internal.Base+TG_GPU_INTERNAL_COVERAGE &&
        a->DmaBufferVirtualAddress+a->DmaBufferSize>c->Device->Process->Internal.Base) {
        KeReleaseSpinLock(&e->Lock,irql); return STATUS_INVALID_PARAMETER;
    }
    marker=++e->Marker;
    if (!marker || marker==UINT32_MAX) marker=e->Marker=1;
    RtlZeroMemory(w,sizeof(*w));
    w->Context=c; w->Root=c->Root; w->DmaAddress=a->DmaBufferVirtualAddress;
    w->DmaBytes=a->DmaBufferSize; w->SubmissionFence=a->SubmissionFenceId; w->Expected=marker;
    w->InternalVa=c->Device->Process->Internal.Base;
    w->FencePhysicalAddress=(UINT64)e->Commands.SystemPhysicalAddress.QuadPart+TG_GPU_COMMAND_FENCE;
    /* CPU initializes only the complement, never the expected completion. */
    *(volatile UINT32 *)((UCHAR *)e->Commands.CpuAddress+TG_GPU_COMMAND_FENCE)=~marker;
    KeMemoryBarrier(); ++c->References; e->Active=w;
    KeReleaseSpinLock(&e->Lock,irql); return STATUS_SUCCESS;
}
NTSTATUS TgGpuPrepareContextWork(TG_GPU_CONTEXT_ENGINE *e,TG_GPU_CONTEXT_WORK *w)
{
    KIRQL irql;NTSTATUS status;TG_GPU_CONTEXT *c;
    if (!e || !w) return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&e->Lock,&irql);c=w->Context;
    if (e->Active!=w || !c || w->Prepared || w->Signaling || w->Notified || w->HardwareBusy || w->Attempted) {
        KeReleaseSpinLock(&e->Lock,irql);return STATUS_INVALID_DEVICE_STATE;
    }
    status=TgGpuInstallInternalRoot(e->Tables,&w->Root,&c->Device->Process->Internal);
    if (NT_SUCCESS(status) && !tg_gpu_prepare_submission(e->Commands.CpuAddress,e->Commands.Bytes,
        (UINT64)e->Commands.SystemPhysicalAddress.QuadPart,w->InternalVa,
        c->Instance.CpuAddress,c->Instance.Bytes,(UINT64)c->Instance.SystemPhysicalAddress.QuadPart,
        w->DmaAddress,w->DmaBytes,w->Expected)) status=STATUS_INVALID_PARAMETER;
    if (NT_SUCCESS(status)) { KeMemoryBarrier();w->Prepared=TRUE; }
    KeReleaseSpinLock(&e->Lock,irql);return status;
}
static BOOLEAN NotifyFence(PVOID context)
{
    TG_GPU_CONTEXT_WORK *w=context; TG_GPU_CONTEXT_ENGINE *e=w->Context->Device->Engine;
    DXGKARGCB_NOTIFY_INTERRUPT_DATA data={0};
    data.InterruptType=DXGK_INTERRUPT_DMA_COMPLETED;
    data.DmaCompleted.SubmissionFenceId=w->SubmissionFence;
    data.DmaCompleted.NodeOrdinal=0; data.DmaCompleted.EngineOrdinal=0;
    e->LastCompletedFence=w->SubmissionFence;
    KeMemoryBarrier(); w->Notified=TRUE;
    e->Dxgk.DxgkCbNotifyInterrupt(e->Dxgk.DeviceHandle,&data);
    (VOID)e->Dxgk.DxgkCbQueueDpc(e->Dxgk.DeviceHandle);
    return TRUE; /* FALSE from QueueDpc may just mean one is already queued */
}
NTSTATUS TgGpuSignalContextWork(TG_GPU_CONTEXT_ENGINE *e, TG_GPU_CONTEXT_WORK *w)
{
    KIRQL irql; NTSTATUS status; BOOLEAN invoked=FALSE;
    if (!e || !w) return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&e->Lock,&irql);
    if (e->Active!=w || !w->Context) { KeReleaseSpinLock(&e->Lock,irql); return STATUS_INVALID_PARAMETER; }
    if (!w->Submitted) { KeReleaseSpinLock(&e->Lock,irql); return STATUS_DEVICE_NOT_READY; }
    if (w->HardwareBusy) { KeReleaseSpinLock(&e->Lock,irql); return STATUS_DEVICE_BUSY; }
    if (w->Signaling) { KeReleaseSpinLock(&e->Lock,irql); return STATUS_DEVICE_BUSY; }
    if (w->Notified) { KeReleaseSpinLock(&e->Lock,irql); return STATUS_SUCCESS; }
    if (*(volatile UINT32 *)((UCHAR *)e->Commands.CpuAddress+TG_GPU_COMMAND_FENCE)!=w->Expected) {
        KeReleaseSpinLock(&e->Lock,irql); return STATUS_PENDING;
    }
    KeMemoryBarrier(); w->Signaling=TRUE; KeReleaseSpinLock(&e->Lock,irql);
    status=e->Dxgk.DxgkCbSynchronizeExecution(e->Dxgk.DeviceHandle,NotifyFence,w,0,&invoked);
    KeAcquireSpinLock(&e->Lock,&irql); w->Signaling=FALSE;
    if (w->Notified) status=STATUS_SUCCESS;
    else if (NT_SUCCESS(status)) status=STATUS_UNSUCCESSFUL;
    KeReleaseSpinLock(&e->Lock,irql); return status;
}
