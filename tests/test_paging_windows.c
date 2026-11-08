/* SPDX-License-Identifier: MIT */
/* Run against the real WDK types in ARM64 Windows/WinPE. No GPU is touched. */
#include "../windows/paging.h"
#include "../windows/table_segment.h"
#include <ntimage.h>

/* Same ARM64 loader metadata as the existing performance tool. */
static ULONG_PTR image_cookie = 0x00002b992ddfa232ULL;
const IMAGE_LOAD_CONFIG_DIRECTORY64 _load_config_used = {
    .Size = sizeof(IMAGE_LOAD_CONFIG_DIRECTORY64),
    .SecurityCookie = (ULONGLONG)&image_cookie
};

__declspec(dllimport) void __stdcall ExitProcess(ULONG Code);
__declspec(dllimport) HANDLE __stdcall GetStdHandle(ULONG Handle);
__declspec(dllimport) BOOL __stdcall WriteFile(HANDLE File, const void *Buffer,
                                            ULONG Bytes, ULONG *Written, void *Overlapped);
static UINT64 table[TG_GPU_TABLE_ENTRIES], scratch[TG_GPU_TABLE_ENTRIES];
static ULONG calls, failed_index;
#define CHECK(x) do { ++failed_index; if (!(x)) ExitProcess(failed_index); } while (0)

/* The kernel normally supplies this CRT primitive. Keep the test executable
 * freestanding so it can run in the stock ARM64 PE image without a UCRT.
 */
void *memset(void *destination, int value, size_t bytes)
{
    unsigned char *p = destination;
    while (bytes--) *p++ = (unsigned char)value;
    return destination;
}

/* Exercise real backend code with controlled OS allocation callbacks. These
 * are test-owned bytes, not GPU-accessible memory or a physical allocation.
 */
static _Alignas(PAGE_SIZE) UCHAR segment_bytes[TG_GPU_TABLE_BYTES * 2];
static BOOLEAN fail_allocate, fail_free;
static UINT allocate_calls, free_calls;
static PHYSICAL_ADDRESS NTAPI TestPhysical(PVOID Address)
{
    PHYSICAL_ADDRESS result;
    CHECK(Address == segment_bytes);
    result.QuadPart = 0x90000000;
    return result;
}
PHYSICAL_ADDRESS (NTAPI * const __imp_MmGetPhysicalAddress)(PVOID) = TestPhysical;

static NTSTATUS NTAPI Allocate(HANDLE Device, DXGKARGCB_ALLOCATECONTIGUOUSMEMORY *Args)
{
    ++allocate_calls;
    CHECK(Device == (HANDLE)1 && Args->NumberOfBytes == sizeof(segment_bytes));
    CHECK(Args->LowestAcceptableAddress.QuadPart == 0x80000000 &&
          Args->HighestAcceptableAddress.QuadPart == 0xffffffff);
    CHECK(Args->CacheType == DXGK_MEMORY_CACHING_TYPE_NON_CACHED &&
          Args->BoundaryAddressMultiple.QuadPart == 0);
    if (fail_allocate) return STATUS_INSUFFICIENT_RESOURCES;
    Args->hMemoryHandle = (HANDLE)2;
    Args->pMemory = segment_bytes;
    return STATUS_SUCCESS;
}
static NTSTATUS NTAPI Free(HANDLE Device, const DXGKARGCB_FREECONTIGUOUSMEMORY *Args)
{
    ++free_calls;
    CHECK(Device == (HANDLE)1 && Args->hMemoryHandle == (HANDLE)2);
    return fail_free ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;
}

static void TestSegment(void)
{
    DXGKRNL_INTERFACE dxgk = {0};
    TG_GPU_TABLE_SEGMENT segment = {0};
    DXGK_SEGMENTDESCRIPTOR3 desc;
    DXGK_PTE ptes[2] = {0};
    DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE update = {0};
    TG_GPU_PAGE_UPDATE_TARGET target = {0};
    PVOID cpu = NULL;
    UINT64 device = UINT64_MAX;
    ULONG before;
    dxgk.DeviceHandle = (HANDLE)1;
    dxgk.Version = DXGKDDI_INTERFACE_VERSION_WDDM2_4;
    dxgk.DxgkCbAllocateContiguousMemory = Allocate;
    dxgk.DxgkCbFreeContiguousMemory = Free;
    CHECK(TgGpuCreateTableSegment(&dxgk,0,sizeof(segment_bytes),&segment) == STATUS_INVALID_PARAMETER);
    CHECK(TgGpuCreateTableSegment(&dxgk,1,PAGE_SIZE,&segment) == STATUS_INVALID_PARAMETER);
    CHECK(allocate_calls == 0);
    fail_allocate = TRUE;
    CHECK(TgGpuCreateTableSegment(&dxgk,1,sizeof(segment_bytes),&segment) == STATUS_INSUFFICIENT_RESOURCES);
    CHECK(!segment.Memory.Handle && !segment.SegmentId);
    fail_allocate = FALSE;
    memset(segment_bytes,0xa5,sizeof(segment_bytes));
    CHECK(NT_SUCCESS(TgGpuCreateTableSegment(&dxgk,1,sizeof(segment_bytes),&segment)));
    CHECK(segment_bytes[0] == 0 && segment_bytes[sizeof(segment_bytes)-1] == 0);
    before = allocate_calls;
    CHECK(TgGpuCreateTableSegment(&dxgk,1,sizeof(segment_bytes),&segment) == STATUS_INVALID_DEVICE_STATE);
    CHECK(allocate_calls == before);
    CHECK(NT_SUCCESS(TgGpuDescribeTableSegment(&segment,&desc)));
    CHECK(desc.BaseAddress.QuadPart == 0x90000000 && desc.CpuTranslatedAddress.QuadPart == 0x90000000);
    CHECK(desc.Size == sizeof(segment_bytes) && desc.CommitLimit == sizeof(segment_bytes));
    CHECK(desc.Flags.CpuVisible && desc.Flags.LocalBudgetGroup && !desc.Flags.Aperture &&
          !desc.Flags.CacheCoherent && !desc.Flags.PreservedDuringStandby && !desc.Flags.PreservedDuringHibernate);
    CHECK(NT_SUCCESS(TgGpuResolveTableSegment(&segment,1,TG_GPU_TABLE_BYTES,TG_GPU_TABLE_BYTES,&device)));
    CHECK(device == 0x90020000);
    CHECK(TgGpuResolveTableSegment(&segment,0,0,PAGE_SIZE,&device) == STATUS_INVALID_ADDRESS);
    CHECK(TgGpuResolveTableSegment(&segment,1,sizeof(segment_bytes)-PAGE_SIZE,TG_GPU_TABLE_BYTES,&device) == STATUS_INVALID_ADDRESS);
    CHECK(TgGpuResolveTableSegment(&segment,1,UINT64_MAX,PAGE_SIZE,&device) == STATUS_INVALID_ADDRESS);
    CHECK(TgGpuResolveTableSegment(&segment,1,1,PAGE_SIZE,&device) == STATUS_INVALID_ADDRESS);
    CHECK(TgGpuResolveTableSegment(&segment,1,0,0,&device) == STATUS_INVALID_ADDRESS);
    CHECK(device == 0x90020000); /* failed resolves publish no address */
    CHECK(NT_SUCCESS(TgGpuMapTableSegment(&segment,0,TG_GPU_TABLE_BYTES,&cpu,&device)));
    CHECK(cpu == segment_bytes && device == 0x90000000);
    target.Entries = cpu; target.Size = TG_GPU_TABLE_BYTES;
    target.Scratch = scratch; target.ScratchEntries = TG_GPU_TABLE_ENTRIES;
    target.CpuMappingUncached = target.GpuPagesUncached = TRUE;
    target.Resolve = TgGpuResolveTableSegment; target.ResolveContext = &segment;
    update.PageTableLevel = 1; update.PageTableAddress.CpuVirtual = cpu;
    update.UpdateMode = DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;
    update.NumPageTableEntries = 1; update.pPageTableEntries = ptes;
    ptes[0].Valid = 1; ptes[0].Segment = 1; ptes[0].PageAddress = TG_GPU_TABLE_BYTES;
    CHECK(NT_SUCCESS(TgGpuUpdatePageTable(&update,&target)));
    CHECK(target.Entries[0] == UINT64_C(0x0090020300000000));
    /* Reject the whole batch when a later child table crosses the pool end. */
    update.StartIndex = 1; update.NumPageTableEntries = 2; ptes[1] = ptes[0];
    ptes[1].PageAddress = sizeof(segment_bytes)-PAGE_SIZE;
    CHECK(TgGpuUpdatePageTable(&update,&target) == STATUS_INVALID_ADDRESS);
    CHECK(target.Entries[1] == 0 && target.Entries[2] == 0);
    segment.Memory.SystemPhysicalAddress.QuadPart = 0x100000000;
    CHECK(TgGpuDescribeTableSegment(&segment,&desc) == STATUS_INVALID_PARAMETER);
    segment.Memory.SystemPhysicalAddress.QuadPart = 0xfffff000;
    CHECK(TgGpuResolveTableSegment(&segment,1,0,PAGE_SIZE,&device) == STATUS_INVALID_ADDRESS);
    segment.Memory.SystemPhysicalAddress.QuadPart = 0x90000000;
    fail_free = TRUE;
    CHECK(TgGpuDestroyTableSegment(&dxgk,&segment) == STATUS_UNSUCCESSFUL);
    CHECK(segment.SegmentId == 1 && segment.Memory.Handle == (HANDLE)2);
    fail_free = FALSE;
    CHECK(NT_SUCCESS(TgGpuDestroyTableSegment(&dxgk,&segment)));
    CHECK(!segment.SegmentId && !segment.Memory.Handle && !segment.Memory.CpuAddress && !segment.Memory.Bytes);
    CHECK(NT_SUCCESS(TgGpuDestroyTableSegment(&dxgk,&segment)) && free_calls == 2);
}

static NTSTATUS Resolve(PVOID Context, UINT Segment, UINT64 Offset, SIZE_T Bytes, UINT64 *Address)
{
    UNREFERENCED_PARAMETER(Context);
    ++calls;
    if (Segment > 1 || Offset > 0x200000 || Bytes > 0x200000 - Offset)
        return STATUS_INVALID_ADDRESS;
    *Address = (Segment ? UINT64_C(0x170000000) : UINT64_C(0x160000000)) + Offset;
    return STATUS_SUCCESS;
}

void start(void)
{
    DXGK_PTE ptes[3] = {0};
    DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE u = {0};
    TG_GPU_PAGE_UPDATE_TARGET target = {table, sizeof(table), scratch,
        TG_GPU_TABLE_ENTRIES, TRUE, TRUE, Resolve, NULL};
    DXGK_PAGE_TABLE_LEVEL_DESC desc;
    UINT64 before;
    ULONG i, written;
    static const char message[] = "PASS: real WDK PTE conversion and table-segment allocation, bounds, offsets and cleanup\r\n";
    TestSegment();
    for (i = 0; i < TG_GPU_TABLE_ENTRIES; ++i) table[i] = UINT64_C(0xa5a5a5a5a5a5a5a5);
    u.PageTableAddress.CpuVirtual = table;
    u.UpdateMode = DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;
    u.pPageTableEntries = ptes;
    u.StartIndex = 7;
    u.NumPageTableEntries = 2;
    ptes[0].Valid = 1; ptes[0].PageAddress = 0x1000; ptes[0].ReadOnly = 1;
    ptes[1].Valid = 1; ptes[1].PageAddress = 0x2000;
    CHECK(NT_SUCCESS(TgGpuUpdatePageTable(&u, &target)));
    CHECK(table[7] == UINT64_C(0x101600015));
    CHECK(table[8] == UINT64_C(0x101600021));
    CHECK(table[6] == UINT64_C(0xa5a5a5a5a5a5a5a5));
    CHECK(table[9] == UINT64_C(0xa5a5a5a5a5a5a5a5));
    CHECK(calls == 2);
    u.Flags.Repeat = 1; u.NumPageTableEntries = 3; calls = 0;
    CHECK(NT_SUCCESS(TgGpuUpdatePageTable(&u, &target)));
    CHECK(table[7] == table[8] && table[8] == table[9]);
    ptes[0].Valid = 0; ptes[0].PageAddress = UINT64_MAX;
    CHECK(NT_SUCCESS(TgGpuUpdatePageTable(&u, &target)));
    CHECK(table[7] == 0 && table[8] == 0 && table[9] == 0);
    CHECK(calls == 3); /* invalid entries never resolve stale addresses */
    u.Flags.Repeat = 0; u.NumPageTableEntries = 2;
    ptes[0].Valid = 1; ptes[0].PageAddress = 0x3000;
    ptes[1].NoExecute = 1;
    before = table[7];
    CHECK(TgGpuUpdatePageTable(&u, &target) == STATUS_NOT_SUPPORTED);
    CHECK(table[7] == before && table[8] == 0);
    ptes[1].NoExecute = 0; ptes[1].CacheCoherent = 1;
    CHECK(TgGpuUpdatePageTable(&u, &target) == STATUS_NOT_SUPPORTED);
    ptes[1].CacheCoherent = 0; ptes[1].PageAddress = 0x201000;
    CHECK(TgGpuUpdatePageTable(&u, &target) == STATUS_INVALID_ADDRESS);
    CHECK(table[7] == before && table[8] == 0);
    ptes[1].PageAddress = 1;
    CHECK(TgGpuUpdatePageTable(&u, &target) == STATUS_INVALID_PARAMETER);
    CHECK(table[7] == before && table[8] == 0);
    u.StartIndex = TG_GPU_TABLE_ENTRIES - 1;
    CHECK(TgGpuUpdatePageTable(&u, &target) == STATUS_INVALID_PARAMETER);
    u.StartIndex = 7; u.NumPageTableEntries = 1; u.PageTableLevel = 1;
    ptes[0].ReadOnly = 0;
    CHECK(TgGpuUpdatePageTable(&u, &target) == STATUS_NOT_SUPPORTED); /* system-segment table too big */
    ptes[0].Segment = 1; ptes[0].PageAddress = 0x20000;
    CHECK(NT_SUCCESS(TgGpuUpdatePageTable(&u, &target)));
    CHECK(table[7] == UINT64_C(0x0170020300000000));
    ptes[0].PageAddress = 0x1f0000;
    CHECK(TgGpuUpdatePageTable(&u, &target) == STATUS_INVALID_ADDRESS); /* whole child must fit */
    CHECK(table[7] == UINT64_C(0x0170020300000000));
    ptes[0].PageAddress = 0x20000; ptes[0].PageTablePageSize = DXGK_PTE_PAGE_TABLE_PAGE_64KB;
    CHECK(TgGpuUpdatePageTable(&u, &target) == STATUS_NOT_SUPPORTED);
    CHECK(TgGpuDescribePageTable(0, 0, &desc) == STATUS_INVALID_PARAMETER);
    CHECK(NT_SUCCESS(TgGpuDescribePageTable(0, 1, &desc)));
    CHECK(desc.PageTableSegmentId == 1 && desc.PagingProcessPageTableSegmentId == 1);
    CHECK(desc.PageTableIndexBitCount == 14 && desc.PageTableSizeInBytes == 131072);
    WriteFile(GetStdHandle((ULONG)-11), message, sizeof(message)-1, &written, NULL);
    ExitProcess(0);
}
