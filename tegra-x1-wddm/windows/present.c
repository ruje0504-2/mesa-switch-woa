/* SPDX-License-Identifier: MIT
 * Fixed-panel GDI shadow copies. B0B5 pitch-linear encoding follows NVK's
 * copy engine; VidMm owns the virtual mappings and allocation residency.
 */
#include "present.h"
#include "allocation.h"
#include "work_queue.h"
#include "../platform/t214_resources.h"
#include "../platform/t214_dc.h"
#include "nouveau/headers/drf.h"
#include "nouveau/headers/nvidia/classes/clb06f.h"
#include "nouveau/headers/nvidia/classes/clb0b5.h"

#define PRESENT_MAGIC 0x50724754u
#define SNAPSHOT_TAG 0x53674754u
typedef struct {
    UINT Magic,Bytes;
    TG_GPU_CONTEXT *Context;
    PVOID CpuAddress;
    UINT64 GpuAddress;
} PRESENT_BUFFER;
C_ASSERT(sizeof(PRESENT_BUFFER)<=sizeof(TG_GPU_QUEUE_ENTRY));

static UINT header(UINT method,UINT count)
{
    return NVDEF(NVB06F,DMA_INCR,OPCODE,VALUE) |
        NVVAL(NVB06F,DMA_INCR,SUBCHANNEL,4) |
        NVVAL(NVB06F,DMA_INCR,ADDRESS,method>>2) |
        NVVAL(NVB06F,DMA_INCR,COUNT,count);
}
static BOOLEAN rectangle(const RECT *r,UINT width,UINT height)
{
    return r->left>=0 && r->top>=0 && r->right>r->left && r->bottom>r->top &&
        r->right<=(LONG)width && r->bottom<=(LONG)height;
}
static BOOLEAN surface_va(TG_GPU_CONTEXT *c,UINT64 va)
{
    const UINT64 internal=c->Device->Process->Internal.Base;
    return va && !(va&3) && va<TG_GPU_VA_LIMIT && T214_GPU_POST_FB_SIZE<=TG_GPU_VA_LIMIT-va &&
        (va+T214_GPU_POST_FB_SIZE<=internal || va>=internal+TG_GPU_INTERNAL_COVERAGE);
}

NTSTATUS APIENTRY TgGpuDdiPresent(HANDLE context,DXGKARG_PRESENT *a)
{
    TG_GPU_CONTEXT *c=context;
    PAGED_CODE();
    if(!c || !a)return STATUS_INVALID_PARAMETER;
    /* NoWait keeps the GPU pipeline running while the scheduler performs
     * the VSync MMIO flip. This no-DMA path emits no GPU wait commands. */
    const DXGK_PRESENTFLAGS flips={.Flip=1,.FlipWithNoWait=1,
        .FlipWithMultiPlaneOverlay=1,.RedirectedFlip=1};
    if(a->Flags.Flip && !(a->Flags.Value&~flips.Value) && !a->pDmaBuffer && !a->MultipassOffset)
        return STATUS_SUCCESS; /* Panel DDIs perform the scheduled MMIO flip. */
    const DXGK_PRESENTFLAGS blt={.Blt=1};
    if(a->Flags.Value!=blt.Value || !c->Flags.GdiContext || !c->Flags.VirtualAddressing ||
       !c->Device->Process)return STATUS_NOT_SUPPORTED;
    if(!a->pAllocationInfo || !a->pDmaBuffer || !a->pDmaBufferPrivateData ||
       ((ULONG_PTR)a->pDmaBuffer&3) || !a->DmaBufferGpuVirtualAddress ||
       !a->SubRectCnt || !a->pDstSubRects || a->MultipassOffset>=a->SubRectCnt)
        return STATUS_INVALID_PARAMETER;
    if(a->DmaBufferPrivateDataSize<sizeof(TG_GPU_QUEUE_ENTRY))
        return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    /* Virtual contexts use the 32-byte present records, not the legacy
     * 24-byte patch allocation list. See SHADOW_PRESENT.md for the checked
     * WDK layout and the installed Dxgkrnl producer. */
    const DXGK_PRESENTALLOCATIONINFO *src=&a->pAllocationInfo[DXGK_PRESENT_SOURCE_INDEX];
    const DXGK_PRESENTALLOCATIONINFO *dst=&a->pAllocationInfo[DXGK_PRESENT_DESTINATION_INDEX];
    TG_GPU_PRIMARY_ALLOCATION_REQUEST source,destination;
    NTSTATUS status=TgGpuGetPresentSurface(c->Device,src->hDeviceSpecificAllocation,FALSE,&source);
    if(!NT_SUCCESS(status))return status;
    status=TgGpuGetPresentSurface(c->Device,dst->hDeviceSpecificAllocation,TRUE,&destination);
    if(!NT_SUCCESS(status))return status;
    if(!rectangle(&a->SrcRect,source.Width,source.Height) ||
       !rectangle(&a->DstRect,destination.Width,destination.Height) ||
       a->SrcRect.right-a->SrcRect.left!=a->DstRect.right-a->DstRect.left ||
       a->SrcRect.bottom-a->SrcRect.top!=a->DstRect.bottom-a->DstRect.top)
        return STATUS_INVALID_PARAMETER;
    if(src->PhysicalAdapterIndex || dst->PhysicalAdapterIndex ||
       !surface_va(c,src->AllocationVirtualAddress) ||
       !surface_va(c,dst->AllocationVirtualAddress))return STATUS_INVALID_PARAMETER;
    const BOOLEAN opaque=source.Format==D3DDDIFMT_X8R8G8B8 && destination.Format==D3DDDIFMT_A8R8G8B8;
    const UINT packet_words=opaque?17:13;
    /* Screen-to-screen overlap is not advertised. Shadow/primary ranges are
     * distinct; reject overlap before emitting any commands. */
    if(src->AllocationVirtualAddress<dst->AllocationVirtualAddress+T214_GPU_POST_FB_SIZE &&
       dst->AllocationVirtualAddress<src->AllocationVirtualAddress+T214_GPU_POST_FB_SIZE)return STATUS_NOT_SUPPORTED;
    UINT *begin=a->pDmaBuffer,*p=begin,available=a->DmaSize/sizeof(UINT);
    UINT next=a->MultipassOffset;
    for(;next<a->SubRectCnt;++next) {
        const RECT *r=&a->pDstSubRects[next];
        if(!rectangle(r,destination.Width,destination.Height) ||
           r->left<a->DstRect.left || r->top<a->DstRect.top ||
           r->right>a->DstRect.right || r->bottom>a->DstRect.bottom)return STATUS_INVALID_PARAMETER;
        if(available<packet_words)break;
        const UINT64 from=src->AllocationVirtualAddress+(UINT64)(r->top-a->DstRect.top+a->SrcRect.top)*source.Pitch+
            (UINT64)(r->left-a->DstRect.left+a->SrcRect.left)*4;
        const UINT64 to=dst->AllocationVirtualAddress+(UINT64)r->top*destination.Pitch+(UINT64)r->left*4;
        *p++=header(0,1);*p++=MAXWELL_DMA_COPY_A;
        if(opaque) {
            /* GDI supplies XRGB shadows for ARGB primaries. Preserve BGR
             * bytes and define alpha as 255; X is not a valid alpha value. */
            *p++=header(NVB0B5_SET_REMAP_CONST_A,3);*p++=0xff;*p++=0;
            *p++=NVDEF(NVB0B5,SET_REMAP_COMPONENTS,DST_X,SRC_X) |
                NVDEF(NVB0B5,SET_REMAP_COMPONENTS,DST_Y,SRC_Y) |
                NVDEF(NVB0B5,SET_REMAP_COMPONENTS,DST_Z,SRC_Z) |
                NVDEF(NVB0B5,SET_REMAP_COMPONENTS,DST_W,CONST_A) |
                NVDEF(NVB0B5,SET_REMAP_COMPONENTS,COMPONENT_SIZE,ONE) |
                NVDEF(NVB0B5,SET_REMAP_COMPONENTS,NUM_SRC_COMPONENTS,FOUR) |
                NVDEF(NVB0B5,SET_REMAP_COMPONENTS,NUM_DST_COMPONENTS,FOUR);
        }
        *p++=header(NVB0B5_OFFSET_IN_UPPER,8);
        *p++=(UINT)(from>>32);*p++=(UINT)from;*p++=(UINT)(to>>32);*p++=(UINT)to;
        *p++=source.Pitch;*p++=destination.Pitch;
        *p++=(UINT)(r->right-r->left)*(opaque?1:4);*p++=(UINT)(r->bottom-r->top);
        *p++=header(NVB0B5_LAUNCH_DMA,1);
        *p++=NVDEF(NVB0B5,LAUNCH_DMA,DATA_TRANSFER_TYPE,PIPELINED) |
            NVDEF(NVB0B5,LAUNCH_DMA,FLUSH_ENABLE,TRUE) |
            NVDEF(NVB0B5,LAUNCH_DMA,SRC_MEMORY_LAYOUT,PITCH) |
            NVDEF(NVB0B5,LAUNCH_DMA,DST_MEMORY_LAYOUT,PITCH) |
            NVDEF(NVB0B5,LAUNCH_DMA,MULTI_LINE_ENABLE,TRUE) |
            (opaque?NVDEF(NVB0B5,LAUNCH_DMA,REMAP_ENABLE,TRUE):0);
        available-=packet_words;
    }
    if(p!=begin) {
        PRESENT_BUFFER *private_data=a->pDmaBufferPrivateData;
        RtlZeroMemory(private_data,sizeof(TG_GPU_QUEUE_ENTRY));
        *private_data=(PRESENT_BUFFER){PRESENT_MAGIC,(UINT)((UCHAR *)p-(UCHAR *)begin),
            c,begin,a->DmaBufferGpuVirtualAddress};
        a->pDmaBuffer=p;
        a->pDmaBufferPrivateData=(UCHAR *)private_data+sizeof(TG_GPU_QUEUE_ENTRY);
        a->MultipassOffset=next;
    }
    return next==a->SubRectCnt?STATUS_SUCCESS:STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
}

NTSTATUS TgGpuCapturePresentSnapshot(TG_GPU_CONTEXT_ENGINE *e,
    const DXGKARG_SUBMITCOMMANDVIRTUAL *a,TG_GPU_COMMAND_SNAPSHOT **out)
{
    TG_GPU_CONTEXT_WORK work={0};
    PAGED_CODE();
    if(!out)return STATUS_INVALID_PARAMETER;
    *out=NULL;
    if(!a || a->DmaBufferUmdPrivateDataSize>a->DmaBufferPrivateDataSize || !a->pDmaBufferPrivateData ||
       a->DmaBufferPrivateDataSize!=sizeof(TG_GPU_QUEUE_ENTRY) ||
       a->DmaBufferSize>TG_GPU_COMMAND_CAPTURE_BYTES)return STATUS_INVALID_PARAMETER;
    /* Resolve the OS context through the engine list before interpreting a
     * KMD pointer. Application UMD submissions cannot use this GDI path. */
    NTSTATUS status=TgGpuCaptureContextWork(e,a,NULL,&work);
    if(!NT_SUCCESS(status))return status;
    const PRESENT_BUFFER *p=a->pDmaBufferPrivateData;
    if(!work.Context->Flags.GdiContext || p->Magic!=PRESENT_MAGIC || p->Context!=work.Context ||
       !p->CpuAddress || p->GpuAddress!=a->DmaBufferVirtualAddress || p->Bytes!=a->DmaBufferSize)
        status=STATUS_INVALID_PARAMETER;
    else {
        TG_GPU_COMMAND_SNAPSHOT *s=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*s)+p->Bytes,SNAPSHOT_TAG);
        if(!s)status=STATUS_NO_MEMORY;
        else {
            const struct tg_gpu_command_va va={
                work.Root.VaLimit,work.InternalVa,work.InternalVa+TG_GPU_INTERNAL_COVERAGE
            };
            s->Bytes=p->Bytes;
            RtlCopyMemory(s->Words,p->CpuAddress,s->Bytes);
            if (!tg_gpu_prepare_command_stream(s->Words,s->Bytes,&va,&s->Info)) {
                TgGpuFreeCommandSnapshot(s);
                status=STATUS_INVALID_PARAMETER;
            } else {
                *out=s;
            }
        }
    }
    (VOID)TgGpuCancelCapturedWork(e,&work);
    return status;
}
