/* SPDX-License-Identifier: MIT */
#ifndef TEGRA_GPU_PAGING_BUFFER_H
#define TEGRA_GPU_PAGING_BUFFER_H
#include "root_table.h"

typedef struct _TG_GPU_PAGING_BUILD {
    const TG_GPU_TABLE_SEGMENT *Tables;
    SIZE_T TableBytes;
    const TG_GPU_ROOT_TABLE *Root;
    TG_GPU_RESOLVE_PAGE Resolve;
    PVOID ResolveContext;
    UINT64 *Scratch;
    SIZE_T ScratchEntries;
    BOOLEAN GpuPagesUncached;
    BOOLEAN InitialMappingUncached;
} TG_GPU_PAGING_BUILD;

/* Build private kernel paging records into the supplied DMA buffer. Ordinary
 * GPU_PHYSICAL updates have no hardware side effects here. The mandatory
 * paging-process bootstrap (CPU_VIRTUAL, NULL pDmaBuffer) is applied now,
 * through a validated alias of the owned, uncached table segment.
 * TableBytes is the actual target allocation size, including resized roots.
 * The adapter validates hProcess/hAllocation before calling this helper.
 */
NTSTATUS TgGpuBuildPagingBuffer(DXGKARG_BUILDPAGINGBUFFER *Args,
                               const TG_GPU_PAGING_BUILD *Build);

/* Submit-worker path, not a GPU command packet parser or a registered DDI.
 * Retire earlier GPU work and hold affected address spaces idle/serialized.
 * Keep segment backing and referenced pages resident for the whole queued
 * operation. Buffer must remain private and immutable until it is retired.
 * Publish uncached PTEs and issue real MMIO TLB invalidations in record order.
 * Notify the WDDM fence only after SUCCESS for the complete submitted buffer.
 * On failure, CompletedBytes is diagnostic progress, NOT a completed fence;
 * keep old pages pinned until recovery drains/resets the GPU successfully.
 */
NTSTATUS TgGpuExecutePagingBuffer(const TG_GPU_TABLE_SEGMENT *Tables,
    const void *Buffer, SIZE_T Bytes, BOOLEAN AddressSpaceIdle,
    const struct tg_gpu_power_io *Io, const struct tg_gpu_power *Power,
    SIZE_T *CompletedBytes);
#endif
