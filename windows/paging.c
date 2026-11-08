/* SPDX-License-Identifier: MIT */
#include "paging.h"

_Static_assert(sizeof(DXGK_PTE) == 16, "Windows PTE layout changed");

static NTSTATUS EncodePage(UINT Level, const DXGK_PTE *Pte,
                          const TG_GPU_PAGE_UPDATE_TARGET *Target, UINT64 *Encoded)
{
    UINT64 address;
    NTSTATUS status;
    /* Invalid entries may carry stale addresses; do not resolve them. */
    if (!Pte->Valid) { *Encoded = 0; return STATUS_SUCCESS; }
    if (Pte->Zero || Pte->CacheCoherent || Pte->NoExecute || Pte->LargePage ||
        Pte->PhysicalAdapterIndex || Pte->Reserved ||
        (Level == 1 && (Pte->ReadOnly || Pte->PageTablePageSize != DXGK_PTE_PAGE_TABLE_PAGE_4KB)))
        return STATUS_NOT_SUPPORTED;
    /* DXGK_PTE carries an aligned byte address (low 12 bits zero), not a
     * shifted PFN. System addresses still go through the DMA resolver.
     */
    if (Pte->PageAddress & (PAGE_SIZE - 1))
        return STATUS_INVALID_PARAMETER;
    /* A level-1 entry points to a contiguous table in the dedicated table
     * segment. Segment zero cannot promise this for a 128 KiB allocation.
     */
    if (Level == 1 && !Pte->Segment) return STATUS_NOT_SUPPORTED;
    status = Target->Resolve(Target->ResolveContext, (UINT)Pte->Segment,
        Pte->PageAddress,
        Level ? TG_GPU_TABLE_BYTES : PAGE_SIZE, &address);
    if (!NT_SUCCESS(status)) return status;
    if (address >= TG_GPU_VA_LIMIT ||
        (Level ? TG_GPU_TABLE_BYTES : PAGE_SIZE) > TG_GPU_VA_LIMIT - address)
        return STATUS_INVALID_PARAMETER;
    if (Level == 0) {
        if (!tg_gpu_pte_4k(address, Pte->ReadOnly != 0, Target->GpuPagesUncached != FALSE, Encoded))
            return STATUS_INVALID_PARAMETER;
    } else if (!tg_gpu_pde_small(address, Encoded)) return STATUS_INVALID_PARAMETER;
    return STATUS_SUCCESS;
}

NTSTATUS TgGpuEncodePageTable(const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE *Update,
                             const TG_GPU_PAGE_UPDATE_TARGET *Target)
{
    UINT i, count;
    SIZE_T entries;
    NTSTATUS status;
    if (!Update || !Target || !Target->Resolve ||
        !Target->Scratch || !Target->Size || Target->Size > TG_GPU_TABLE_BYTES ||
        (Target->Size & (PAGE_SIZE - 1)) || ((ULONG_PTR)Target->Scratch & 7) ||
        Update->PageTableLevel > 1 || Update->Reserved0)
        return STATUS_INVALID_PARAMETER;
    if (Update->Flags.Use64KBPages ||
        Update->Flags.Reserved || Update->DriverProtection || Update->pPageTableEntries64KB)
        return STATUS_NOT_SUPPORTED;
    if (!Update->PageTableLevel && Target->Size != TG_GPU_TABLE_BYTES)
        return STATUS_INVALID_PARAMETER;
    entries = Target->Size / sizeof(UINT64);
    count = Update->NumPageTableEntries;
    if (Update->StartIndex > entries || count > entries - Update->StartIndex ||
        count > Target->ScratchEntries || (count && !Update->pPageTableEntries))
        return STATUS_INVALID_PARAMETER;
    /* VidMm owns input entries and the caller pins both target and resolved
     * addresses for the full operation. Scratch avoids partial publication
     * if a later segment translation or attribute is rejected.
     */
    for (i = 0; i < count; ++i) {
        status = EncodePage(Update->PageTableLevel,
            &Update->pPageTableEntries[Update->Flags.Repeat ? 0 : i], Target, &Target->Scratch[i]);
        if (!NT_SUCCESS(status)) return status;
    }
    return STATUS_SUCCESS;
}

NTSTATUS TgGpuUpdatePageTable(const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE *Update,
                              const TG_GPU_PAGE_UPDATE_TARGET *Target)
{
    UINT i;
    NTSTATUS status;
    if (!Update || !Target || !Target->Entries || ((ULONG_PTR)Target->Entries & 7) ||
        Update->PageTableAddress.CpuVirtual != (PVOID)Target->Entries ||
        Update->UpdateMode != DXGK_PAGETABLEUPDATE_CPU_VIRTUAL)
        return STATUS_INVALID_PARAMETER;
    if (!Target->CpuMappingUncached) return STATUS_NOT_SUPPORTED;
    status = TgGpuEncodePageTable(Update, Target);
    if (!NT_SUCCESS(status)) return status;
    KeMemoryBarrier();
    for (i = 0; i < Update->NumPageTableEntries; ++i)
        Target->Entries[Update->StartIndex + i] = Target->Scratch[i];
    KeMemoryBarrier();
    return STATUS_SUCCESS;
}

NTSTATUS TgGpuDescribePageTable(UINT Level, UINT TableSegment,
                               DXGK_PAGE_TABLE_LEVEL_DESC *Description)
{
    if (!Description || Level > 1 || !TableSegment || TableSegment > 31)
        return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(Description, sizeof(*Description));
    Description->PageTableIndexBitCount = TG_GPU_TABLE_BITS;
    Description->PageTableSegmentId = TableSegment;
    Description->PagingProcessPageTableSegmentId = TableSegment;
    Description->PageTableSizeInBytes = TG_GPU_TABLE_BYTES;
    Description->PageTableAlignmentInBytes = PAGE_SIZE;
    return STATUS_SUCCESS;
}
