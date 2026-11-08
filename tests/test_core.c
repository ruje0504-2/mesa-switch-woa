/* SPDX-License-Identifier: MIT */
#include "../core/gm20b_mmu.h"
#include "../core/report.h"
#include "../core/identity.h"
#include "../core/gpfifo.h"
#include "../core/execute.h"
#include "../core/submission.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
void test_execute_init(void);

static void test_mmu(void)
{
    struct tg_gpu_va_indices indices;
    uint64_t entry;

    assert(tg_gpu_va_indices(0, &indices));
    assert(indices.directory == 0 && indices.small_page == 0);
    assert(tg_gpu_va_indices(0x03fff000, &indices));
    assert(indices.directory == 0 && indices.small_page == 16383);
    assert(tg_gpu_va_indices(0x04000000, &indices));
    assert(indices.directory == 1 && indices.small_page == 0);
    assert(tg_gpu_va_indices(UINT64_C(0xfffffff000), &indices));
    assert(indices.directory == 16383 && indices.small_page == 16383);
    assert(!tg_gpu_va_indices(TG_GPU_VA_LIMIT, &indices));
    assert(!tg_gpu_va_indices(1, &indices));

    /* Physical addresses above 4 GiB must survive encoding. NCOH has a
     * different aperture selector in leaf PTEs and in directory entries.
     */
    assert(tg_gpu_pte_4k(UINT64_C(0x170001000), false, false, &entry));
    assert(entry == UINT64_C(0x0000000001700011));
    assert(tg_gpu_pte_4k(UINT64_C(0x170001000), true, true, &entry));
    assert(entry == UINT64_C(0x0000000101700015));
    assert(tg_gpu_pde_small(UINT64_C(0x170020000), &entry));
    assert(entry == UINT64_C(0x0170020300000000));
    assert(tg_gpu_instance_pdb(UINT64_C(0x170040000), &entry));
    assert(entry == UINT64_C(0x170040803));

    entry = UINT64_C(0xdeadbeef);
    assert(!tg_gpu_pte_4k(TG_GPU_VA_LIMIT, false, false, &entry));
    assert(!tg_gpu_pte_4k(0x1234, false, false, &entry));
    assert(!tg_gpu_pde_small(0x1234, &entry));
    assert(!tg_gpu_instance_pdb(TG_GPU_VA_LIMIT, &entry));
    assert(entry == UINT64_C(0xdeadbeef));
    assert(!tg_gpu_pte_4k(0x1000, false, false, NULL));
}

static void test_report(void)
{
    uint32_t words[TG_GPU_REPORT_WORDS + 1];
    uint32_t unchanged[TG_GPU_REPORT_WORDS + 1];
    memset(words, 0xa5, sizeof(words));
    memcpy(unchanged, words, sizeof(words));
    assert(!tg_gpu_build_report(words, TG_GPU_REPORT_WORDS - 1,
                               0x10000, 0x13579bdf));
    assert(!tg_gpu_build_report(words, TG_GPU_REPORT_WORDS,
                               0x10001, 0x13579bdf));
    assert(!tg_gpu_build_report(words, TG_GPU_REPORT_WORDS,
                               TG_GPU_VA_LIMIT, 0x13579bdf));
    assert(memcmp(words, unchanged, sizeof(words)) == 0);
    assert(tg_gpu_build_report(words, TG_GPU_REPORT_WORDS,
                              UINT64_C(0x1234567000), 0x13579bdf));
    /* SET_REPORT_SEMAPHORE_A/B/C: high address, low address, payload. */
    assert(words[2] == 0x12);
    assert(words[3] == 0x34567000);
    assert(words[4] == 0x13579bdf);
    assert(words[TG_GPU_REPORT_WORDS] == 0xa5a5a5a5);
    assert(tg_gpu_build_report(words, TG_GPU_REPORT_WORDS,
                              UINT64_C(0xfffffffffc), 0x2468ace0));
    assert(words[2] == 0xff && words[3] == 0xfffffffc);
    assert(words[4] == 0x2468ace0);
}

static void test_private_channel(void)
{
    static _Alignas(4096) uint8_t arena[TG_GPU_ARENA_BYTES];
    const uint32_t values[2] = {0x13579bdf, 0x2468ace0};
    uint64_t *pde = (uint64_t *)arena;
    uint64_t *pte = (uint64_t *)(arena + 0x20000);
    uint32_t *ramfc = (uint32_t *)(arena + 0x40000);
    uint32_t *commands = (uint32_t *)(arena + 0x43000);
    uint32_t *fence = (uint32_t *)(arena + 0x44000);
    uint32_t *runlist = (uint32_t *)(arena + 0x45000);
    uint32_t *userd = (uint32_t *)(arena + 0x41000);
    assert(tg_gpu_prepare_arena(arena, 0xc0000000, sizeof(arena), values));
    assert(pde[0] == UINT64_C(0x00c0020300000000));
    assert(pde[1] == 0 && pte[0] == 0 && pte[20] == 0);
    assert((pte[16] & 5) == 5 && (pte[17] & 5) == 5); /* read-only commands */
    assert((pte[18] & 5) == 1 && (pte[19] & 5) == 1); /* writable fence/userd */
    assert(ramfc[128] == 0xc0000807 && ramfc[130] == 0x00fff000);
    assert(ramfc[2] == 0xc0041003 && ramfc[18] == 0x10000);
    assert(ramfc[0] == 0 && ramfc[5] == 0 && userd[35] == 0 && userd[34] == 0);
    assert(commands[0] == 0x20040004 && commands[4] == 0x01100002);
    assert(commands[2] == 0x12000 && commands[3] == values[0]);
    assert(commands[7] == 0x12004 && commands[8] == values[1]);
    assert(fence[0] == 0xdeadbeef && fence[1] == 0xdeadbeef);
    /* Vendor runlist: one-channel TSG, default timeslice, then CHID zero. */
    assert(runlist[0] == 0x0600e000 && runlist[1] == 0);
    assert(runlist[2] == 0 && runlist[3] == 0);
    assert(tg_gpu_prepare_arena(arena, 0xfffb0000, sizeof(arena), values));
    assert(runlist[0] == 0x0600e000 && runlist[1] == 0);
    assert(!tg_gpu_prepare_arena(arena, 0xc0000001, sizeof(arena), values));
    assert(!tg_gpu_prepare_arena(arena, 0xffff0000, sizeof(arena), values));
    assert(!tg_gpu_prepare_arena(arena, 0xc0000000, 4096, values));
}

static void test_submission(void)
{
    static _Alignas(4096) uint32_t command[TG_GPU_COMMAND_BYTES/4], instance[4096/4];
    uint32_t *push=command+TG_GPU_COMMAND_PUSH/4, *userd=command+TG_GPU_COMMAND_USERD/4;
    uint32_t *runlist=command+TG_GPU_COMMAND_RUNLIST/4;
    memset(instance,0xa5,sizeof(instance));
    assert(tg_gpu_prepare_submission(command,sizeof(command),0xc0000000,0x4000000,
        instance,sizeof(instance),0xc0040000,0x20000000,40,0x13579bdf));
    assert(command[0]==0x20000000 && command[1]==0x2800); /* USER application */
    assert(command[2]==0x4001000 && command[3]==0x80001500); /* KERNEL completion, wait */
    assert(push[0]==0x20040004 && push[1]==0 && push[2]==0x4002800 && push[3]==0x13579bdf);
    assert(push[4]==0x01000002); /* WFI enabled, not a FIFO-consumption-only fence */
    assert(command[TG_GPU_COMMAND_FENCE/4]==0xeca86420 && userd[35]==2);
    assert(instance[0]==2 && instance[2]==0xc0002003 && instance[18]==0x4000000 && instance[57]==0x20);
    assert(instance[128]==0xa5a5a5a5 && instance[131]==0xa5a5a5a5);
    assert(runlist[0]==0x0600e000 && runlist[1]==0);
    assert(runlist[2]==0 && runlist[3]==0);
    assert(!tg_gpu_prepare_submission(command,sizeof(command),UINT64_C(1)<<34,0x4000000,
        instance,sizeof(instance),0xc0040000,0x20000000,40,0x2468ace0));
    assert(!tg_gpu_prepare_submission(command,sizeof(command),0xc0000000,0x4000000,
        instance,sizeof(instance),0xc0040000,0x20000000,0x800000,0x2468ace0));
    assert(command[TG_GPU_COMMAND_FENCE/4]==0xeca86420 && push[3]==0x13579bdf);
}

int main(void)
{
    struct tg_gpu_gpfifo_entry entry;
    struct tg_gpu_identity identity;
    _Static_assert(sizeof(struct tg_gpu_status) == 236, "control ABI 8 size");
    assert(tg_gpu_fifo_has_error(1) && tg_gpu_fifo_has_error(0x100) && tg_gpu_fifo_has_error(0x10000));
    assert(!tg_gpu_fifo_has_error(0xc0000000)); /* ordinary notification events */
    assert(tg_gpu_identify(0x12e000b1, &identity));
    assert(identity.chipset == TG_GPU_CHIP_GM20B_B);
    assert(identity.major_revision == 0xb && identity.minor_revision == 1);
    assert(tg_gpu_identify(0x12b000a1, &identity));
    assert(identity.chipset == TG_GPU_CHIP_GM20B);
    assert(!tg_gpu_identify(0xffffffff, &identity));
    assert(!tg_gpu_identify(0, &identity));
    assert(tg_gpu_pbdma_owns_channel(0x2000, 0));
    assert(!tg_gpu_pbdma_owns_channel(0x2001, 0));
    assert(!tg_gpu_pbdma_owns_channel(0x3000, 0)); /* TSG, not our bare channel */
    assert(!tg_gpu_pbdma_owns_channel(0x10000004, 0)); /* idle/stale SRAM */
    assert(!tg_gpu_pbdma_owns_channel(0xa000, 0)); /* context load in progress */
    assert(tg_gpu_pbdma_owns_slot(0x2000));
    assert(tg_gpu_pbdma_owns_slot(0x3000)); /* valid single-channel TSG zero */
    assert(!tg_gpu_pbdma_owns_slot(0x3001)); /* a different TSG */
    assert(!tg_gpu_pbdma_owns_slot(0x10040001)); /* physical idle readout */
    assert(!tg_gpu_pbdma_owns_slot(0xb000)); /* TSG context load */
    assert(!tg_gpu_pbdma_owns_slot(UINT32_MAX));
    assert(tg_gpu_build_gpfifo(UINT64_C(0x1234567000), 6, false, &entry));
    assert(entry.low == 0x34567000 && entry.high == 0x00001812);
    assert(tg_gpu_build_gpfifo(UINT64_C(0x1234567000), 6, true, &entry));
    assert(entry.high == 0x80001812);
    assert(!tg_gpu_build_gpfifo(UINT64_C(0xfffffffffc), 2, false, &entry));
    assert(!tg_gpu_build_gpfifo(0x10001, 6, false, &entry));
    assert(!tg_gpu_build_gpfifo(0x10000, 0x200000, false, &entry));
    assert(!tg_gpu_build_gpfifo(0x10000, 0, false, &entry));
    assert(entry.low == 0x34567000 && entry.high == 0x80001812);
    test_mmu();
    test_report();
    test_private_channel();
    test_submission();
    test_execute_init();
    puts("GM20B format tests passed (host only; no GPU execution).");
    return 0;
}
