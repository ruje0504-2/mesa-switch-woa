/* SPDX-License-Identifier: MIT */
#ifndef TEGRA_GPU_TABLE_SEGMENT_H
#define TEGRA_GPU_TABLE_SEGMENT_H
#include "private_memory.h"
#include "paging.h"

/* VidMm owns allocation/residency within this CPU-visible memory segment.
 * The adapter owns its contiguous, uncached backing until StopDevice. This
 * is not a second page allocator and never changes global Tegra SMMU ASIDs.
 * The initial T214 backend uses untagged physical memory below 4 GiB.
 */
typedef struct _TG_GPU_TABLE_SEGMENT {
    TG_GPU_PRIVATE_MEMORY Memory;
    UINT SegmentId;
} TG_GPU_TABLE_SEGMENT;

/* StartDevice, before publishing any segment/page-table description. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS TgGpuCreateTableSegment(const DXGKRNL_INTERFACE *Dxgk,
    UINT SegmentId, SIZE_T Bytes, TG_GPU_TABLE_SEGMENT *Segment);

/* Caller must stop new paging operations and retire all GPU references
 * (completed fences or a successful drain/reset) before freeing the pool.
 * Failed frees retain the allocation and segment ID for cleanup retry.
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS TgGpuDestroyTableSegment(const DXGKRNL_INTERFACE *Dxgk,
    TG_GPU_TABLE_SEGMENT *Segment);

NTSTATUS TgGpuDescribeTableSegment(const TG_GPU_TABLE_SEGMENT *Segment,
    DXGK_SEGMENTDESCRIPTOR3 *Description);

/* TG_GPU_RESOLVE_PAGE callback for this segment only. System-memory pages
 * and other segments require their own pinned/resident mapping resolver.
 * DXGK_PTE::PageAddress is a byte OFFSET, not a PFN or GPU virtual address.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS TgGpuResolveTableSegment(PVOID Context, UINT SegmentId,
    UINT64 ByteOffset, SIZE_T Bytes, UINT64 *DeviceAddress);

/* WDDM 2.4 physical-address paging path. VidMm PTE segment zero contains a
 * resident system-memory BYTE address, not the implicit allocation segment
 * number or a PFN. Nonzero entries use the dedicated table segment above.
 * Caller holds VidMm residency through execution/fence completion. This
 * resolver neither pins pages nor accepts application-supplied addresses.
 * A future logical-DMA/IOMMU path requires a different advertised contract.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS TgGpuResolveVidMmPage(PVOID Context, UINT SegmentId,
    UINT64 ByteAddress, SIZE_T Bytes, UINT64 *DeviceAddress);

/* Access through the pool's own uncached mapping. A VidMm CPU_VIRTUAL update
 * can carry a different alias; do not compare that VA to this VA as proof
 * of allocation ownership. The adapter must validate its physical span.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS TgGpuMapTableSegment(const TG_GPU_TABLE_SEGMENT *Segment,
    UINT64 ByteOffset, SIZE_T Bytes, PVOID *CpuAddress, UINT64 *DeviceAddress);
#endif
