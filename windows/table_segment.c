/* SPDX-License-Identifier: MIT */
#include "table_segment.h"

#define T214_TABLE_LOW  UINT64_C(0x80000000)
#define T214_TABLE_HIGH UINT64_C(0xffffffff)

static BOOLEAN ValidSegment(const TG_GPU_TABLE_SEGMENT *Segment)
{
    UINT64 base;
    if (!Segment || !Segment->SegmentId || Segment->SegmentId > 31 ||
        !Segment->Memory.Handle || !Segment->Memory.CpuAddress ||
        ((ULONG_PTR)Segment->Memory.CpuAddress & (PAGE_SIZE - 1)) ||
        !Segment->Memory.Bytes || (Segment->Memory.Bytes % TG_GPU_TABLE_BYTES))
        return FALSE;
    base = (UINT64)Segment->Memory.SystemPhysicalAddress.QuadPart;
    return base >= T214_TABLE_LOW && base <= T214_TABLE_HIGH &&
        !(base & (PAGE_SIZE - 1)) &&
        Segment->Memory.Bytes - 1 <= T214_TABLE_HIGH - base;
}

NTSTATUS TgGpuCreateTableSegment(const DXGKRNL_INTERFACE *Dxgk,
    UINT SegmentId, SIZE_T Bytes, TG_GPU_TABLE_SEGMENT *Segment)
{
    NTSTATUS status;
    PAGED_CODE();
    if (!Segment || !SegmentId || SegmentId > 31 || !Bytes ||
        Bytes % TG_GPU_TABLE_BYTES)
        return STATUS_INVALID_PARAMETER;
    if (Segment->Memory.Handle || Segment->SegmentId)
        return STATUS_INVALID_DEVICE_STATE;
    status = TgGpuAllocatePrivateMemory(Dxgk, Bytes, T214_TABLE_LOW,
        T214_TABLE_HIGH, &Segment->Memory);
    if (!NT_SUCCESS(status)) return status;
    Segment->SegmentId = SegmentId;
    return STATUS_SUCCESS;
}

NTSTATUS TgGpuDestroyTableSegment(const DXGKRNL_INTERFACE *Dxgk,
    TG_GPU_TABLE_SEGMENT *Segment)
{
    NTSTATUS status;
    PAGED_CODE();
    if (!Segment) return STATUS_INVALID_PARAMETER;
    status = TgGpuFreePrivateMemory(Dxgk, &Segment->Memory);
    if (NT_SUCCESS(status)) Segment->SegmentId = 0;
    return status;
}

NTSTATUS TgGpuDescribeTableSegment(const TG_GPU_TABLE_SEGMENT *Segment,
    DXGK_SEGMENTDESCRIPTOR3 *Description)
{
    if (!Description || !ValidSegment(Segment)) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(Description, sizeof(*Description));
    Description->BaseAddress = Segment->Memory.SystemPhysicalAddress;
    Description->CpuTranslatedAddress = Segment->Memory.SystemPhysicalAddress;
    Description->Size = Description->CommitLimit = Segment->Memory.Bytes;
    Description->Flags.CpuVisible = 1;
    Description->Flags.PopulatedFromSystemMemory = 1;
    Description->Flags.LocalBudgetGroup = 1;
    /* A real memory segment, not an aperture. CPU mapping is uncached;
     * neither cache coherence, direct scanout nor sleep retention is claimed.
     */
    return STATUS_SUCCESS;
}

NTSTATUS TgGpuResolveTableSegment(PVOID Context, UINT SegmentId,
    UINT64 ByteOffset, SIZE_T Bytes, UINT64 *DeviceAddress)
{
    const TG_GPU_TABLE_SEGMENT *segment = Context;
    if (!DeviceAddress || !ValidSegment(segment) || SegmentId != segment->SegmentId ||
        !Bytes || (Bytes & (PAGE_SIZE - 1)) || (ByteOffset & (PAGE_SIZE - 1)) ||
        ByteOffset > segment->Memory.Bytes || Bytes > segment->Memory.Bytes - ByteOffset)
        return STATUS_INVALID_ADDRESS;
    *DeviceAddress = (UINT64)segment->Memory.SystemPhysicalAddress.QuadPart + ByteOffset;
    return STATUS_SUCCESS;
}

NTSTATUS TgGpuResolveVidMmPage(PVOID Context, UINT SegmentId,
    UINT64 ByteAddress, SIZE_T Bytes, UINT64 *DeviceAddress)
{
    const TG_GPU_TABLE_SEGMENT *segment = Context;
    /* On T214, GPU address bit 34 selects Tegra SMMU translation. This
     * physical path must leave it clear and must not truncate >4 GiB pages.
     * VidMm, not this resolver, establishes that the address is resident RAM.
     */
    const UINT64 limit = UINT64_C(1) << 34;
    if (SegmentId)
        return TgGpuResolveTableSegment(Context, SegmentId, ByteAddress, Bytes, DeviceAddress);
    if (!DeviceAddress || !ValidSegment(segment) || Bytes != PAGE_SIZE ||
        (ByteAddress & (PAGE_SIZE - 1)) || ByteAddress < T214_TABLE_LOW ||
        ByteAddress >= limit || Bytes > limit - ByteAddress)
        return STATUS_INVALID_ADDRESS;
    *DeviceAddress = ByteAddress;
    return STATUS_SUCCESS;
}

NTSTATUS TgGpuMapTableSegment(const TG_GPU_TABLE_SEGMENT *Segment,
    UINT64 ByteOffset, SIZE_T Bytes, PVOID *CpuAddress, UINT64 *DeviceAddress)
{
    NTSTATUS status;
    if (!CpuAddress || !Segment || Bytes > TG_GPU_TABLE_BYTES)
        return STATUS_INVALID_PARAMETER;
    status = TgGpuResolveTableSegment((PVOID)Segment, Segment->SegmentId,
        ByteOffset, Bytes, DeviceAddress);
    if (!NT_SUCCESS(status)) return status;
    *CpuAddress = (UCHAR *)Segment->Memory.CpuAddress + (SIZE_T)ByteOffset;
    return STATUS_SUCCESS;
}
