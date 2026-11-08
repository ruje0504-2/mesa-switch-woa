/* SPDX-License-Identifier: MIT */
/* Run against the real WDK types in ARM64 Windows/WinPE. No GPU is touched. */
#include "../windows/paging.h"
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
    static const char message[] = "PASS: real WDK PTE conversion, offsets, repeat, rejection and table-segment limits\r\n";
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
