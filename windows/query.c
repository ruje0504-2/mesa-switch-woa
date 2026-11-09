/* SPDX-License-Identifier: MIT */
#include "query.h"
#include "work_queue.h"

static NTSTATUS buffers(const DXGKARG_QUERYADAPTERINFO *a, UINT input, UINT output)
{
    if (a->InputDataSize < input || a->OutputDataSize < output)
        return STATUS_BUFFER_TOO_SMALL;
    if ((input && !a->pInputData) || (output && !a->pOutputData))
        return STATUS_INVALID_PARAMETER;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY TgGpuDdiQueryAdapterInfo(HANDLE adapter, const DXGKARG_QUERYADAPTERINFO *a)
{
    TG_GPU_CONTEXT_ENGINE *e=adapter;
    DXGK_SEGMENTDESCRIPTOR3 table;
    NTSTATUS status;
    if (!e || !a) return STATUS_INVALID_PARAMETER;
    if (a->Flags.Value) return STATUS_NOT_SUPPORTED; /* no GPU virtualization */
    switch (a->Type) {
    case DXGKQAITYPE_UMDRIVERPRIVATE:
    case DXGKQAITYPE_QUERYSEGMENT4:
    case DXGKQAITYPE_GPUMMUCAPS:
    case DXGKQAITYPE_PAGETABLELEVELDESC:
    case DXGKQAITYPE_PHYSICALADAPTERCAPS:
        break;
    default:
        return STATUS_NOT_SUPPORTED;
    }
    /* Segment 1 backs page tables. Segment 2 gives VidMm a pageable window
     * into system memory for its DMA pool. */
    if (e->Stopping || !e->Dxgk.DeviceHandle || !e->Tables || e->Tables->SegmentId!=1)
        return STATUS_DEVICE_NOT_READY;
    status=TgGpuDescribeTableSegment(e->Tables,&table);
    if (!NT_SUCCESS(status)) return status;

    switch (a->Type) {
    case DXGKQAITYPE_UMDRIVERPRIVATE: {
        struct tg_gpu_adapter_query request;
        struct tg_gpu_adapter_info info={.version=TG_GPU_ADAPTER_ABI,.size=sizeof(info),
            .memory_budget_bytes=TG_GPU_MEMORY_BUDGET_BYTES,.vendor=0x10de,
            .chipset=0x12e,.sm=53,.gr_class=0xb197};
        status=buffers(a,sizeof(info),sizeof(info));
        if(!NT_SUCCESS(status))return status;
        /* Snapshot the request before writing: D3DKMT may alias both buffers. */
        RtlCopyMemory(&request,a->pInputData,sizeof(request));
        if(request.version!=TG_GPU_ADAPTER_ABI || request.size!=sizeof(info))
            return STATUS_GRAPHICS_DRIVER_MISMATCH;
        if(e->AdapterIdentityReady) {
            info.flags=TG_GPU_ADAPTER_LUID_VALID;
            info.luid_low=e->AdapterLuid.LowPart;info.luid_high=e->AdapterLuid.HighPart;
        }
        /* NATIVE_SUBMIT stays unset: this library has no live WDDM worker yet. */
        RtlCopyMemory(a->pOutputData,&info,sizeof(info));
        return STATUS_SUCCESS;
    }
    case DXGKQAITYPE_QUERYSEGMENT4: {
        const DXGK_QUERYSEGMENTIN4 *in=a->pInputData;
        DXGK_QUERYSEGMENTOUT4 *out=a->pOutputData;
        DXGK_SEGMENTDESCRIPTOR4 desc={0},aperture={0};
        status=buffers(a,sizeof(*in),sizeof(*out));
        if (!NT_SUCCESS(status)) return status;
        if (in->PhysicalAdapterIndex) return STATUS_INVALID_PARAMETER;
        /* WDDM's first call permits accessing only NbSegment. The other
         * members are not valid inputs until the second call.
         */
        if (!out->NbSegment) { out->NbSegment=2;return STATUS_SUCCESS; }
        if (out->NbSegment!=2 || !out->pSegmentDescriptor ||
            out->SegmentDescriptorStride<sizeof(desc)) return STATUS_INVALID_PARAMETER;
        desc.Flags=table.Flags;
        desc.BaseAddress=table.BaseAddress;
        desc.Size=table.Size;
        desc.CommitLimit=table.CommitLimit;
        desc.SystemMemoryEndAddress=table.SystemMemoryEndAddress;
        desc.CpuTranslatedAddress=table.CpuTranslatedAddress;
        /* Only our known descriptor bytes belong to this WDDM 2.4 driver;
         * preserve a larger OS stride and its extension bytes.
         */
        RtlCopyMemory(out->pSegmentDescriptor,&desc,sizeof(desc));
        aperture.Flags.Aperture=1;
        aperture.Size=aperture.CommitLimit=TG_GPU_PAGING_APERTURE_BYTES;
        RtlCopyMemory(out->pSegmentDescriptor+out->SegmentDescriptorStride,
                      &aperture,sizeof(aperture));
        out->PagingBufferSegmentId=TG_GPU_PAGING_APERTURE_SEGMENT;
        out->PagingBufferSize=TG_GPU_DMA_BUFFER_BYTES;
        out->PagingBufferPrivateDataSize=sizeof(TG_GPU_QUEUE_ENTRY);
        return STATUS_SUCCESS;
    }
    case DXGKQAITYPE_GPUMMUCAPS: {
        const DXGK_QUERYGPUMMUCAPSIN *in=a->pInputData;
        DXGK_GPUMMUCAPS caps={0};
        status=buffers(a,sizeof(*in),sizeof(caps));
        if (!NT_SUCCESS(status)) return status;
        if (in->PhysicalAdapterIndex) return STATUS_INVALID_PARAMETER;
        caps.ReadOnlyMemorySupported=1;
        caps.PageTableUpdateRequireAddressSpaceIdle=1;
        caps.PageTableUpdateMode=DXGK_PAGETABLEUPDATE_GPU_PHYSICAL;
        caps.VirtualAddressBitCount=TG_GPU_DIRECTORY_SHIFT+TG_GPU_TABLE_BITS;
        caps.PageTableLevelCount=2;
        /* No coherent, zero, NX, large or dual-PTE support. Initial CPU
         * updates remain a separate validated path in BuildPagingBuffer.
         */
        RtlCopyMemory(a->pOutputData,&caps,sizeof(caps));
        return STATUS_SUCCESS;
    }
    case DXGKQAITYPE_PAGETABLELEVELDESC: {
        const DXGK_QUERYPAGETABLELEVELDESCIN *in=a->pInputData;
        DXGK_PAGE_TABLE_LEVEL_DESC desc;
        status=buffers(a,sizeof(*in),sizeof(desc));
        if (!NT_SUCCESS(status)) return status;
        if (in->PhysicalAdapterIndex) return STATUS_INVALID_PARAMETER;
        status=TgGpuDescribePageTable(in->LevelIndex,e->Tables->SegmentId,&desc);
        if (!NT_SUCCESS(status)) return status;
        RtlCopyMemory(a->pOutputData,&desc,sizeof(desc));
        return STATUS_SUCCESS;
    }
    case DXGKQAITYPE_PHYSICALADAPTERCAPS: {
        const DXGK_QUERYPHYSICALADAPTERCAPSIN *in=a->pInputData;
        DXGK_PHYSICALADAPTERCAPS caps={0};
        status=buffers(a,sizeof(*in),sizeof(caps));
        if (!NT_SUCCESS(status)) return status;
        if (in->PhysicalAdapterIndex) return STATUS_INVALID_PARAMETER;
        caps.NumExecutionNodes=1;
        caps.PagingNodeIndex=0; /* paging shares the ordered execution node */
        caps.DxgkPhysicalAdapterHandle=e->Dxgk.DeviceHandle;
        caps.Flags.GpuMmuSupported=1;
        RtlCopyMemory(a->pOutputData,&caps,sizeof(caps));
        return STATUS_SUCCESS;
    }
    default:
        return STATUS_NOT_SUPPORTED;
    }
}

void TgGpuRegisterQueryDdi(DRIVER_INITIALIZATION_DATA *d)
{ d->DxgkDdiQueryAdapterInfo=TgGpuDdiQueryAdapterInfo; }
