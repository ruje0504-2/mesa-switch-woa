/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2014-2018 NVIDIA CORPORATION. All rights reserved.
 * Copyright 2026 ruje0504-2
 * RAMFC, BAR1, MMU and FIFO sequences follow Switchroot nvgpu 1ae0167d:
 * gk20a/fifo_gk20a.c, gm20b/fifo_gm20b.c, common/{bus,fb,mc} and hw headers.
 * Packet definitions are from Mesa's NVIDIA class headers. See NOTICE.
 */
#include "execute.h"
#include "gm20b_mmu.h"
#include "gpfifo.h"
#include "nouveau/headers/drf.h"
#include "nouveau/headers/nvidia/classes/clb06f.h"

#define GPU 0x57000000u
#define BAR1 0x58000000u
#define INST 0x40000u
#define USERD 0x41000u
#define RING 0x42000u
#define PUSH 0x43000u
#define FENCE 0x44000u
#define RUNLIST 0x45000u
#define FLUSH 0x46000u
#define DEBUG_RD 0x47000u
#define DEBUG_WR 0x48000u
#define RING_VA 0x10000u
#define PUSH_VA 0x11000u
#define FENCE_VA 0x12000u
#define USERD_VA 0x13000u

static void release_packet(uint32_t *p, uint64_t va, uint32_t value)
{
    p[0] = NVDEF(NVB06F, DMA_INCR, OPCODE, VALUE) |
           NVVAL(NVB06F, DMA_INCR, COUNT, 4) |
           NVVAL(NVB06F, DMA_INCR, ADDRESS, NVB06F_SEMAPHOREA >> 2);
    p[1] = (uint32_t)(va >> 32);
    p[2] = (uint32_t)va;
    p[3] = value;
    p[4] = NVDEF(NVB06F, SEMAPHORED, OPERATION, RELEASE) |
           NVDEF(NVB06F, SEMAPHORED, RELEASE_WFI, DIS) |
           NVDEF(NVB06F, SEMAPHORED, RELEASE_SIZE, 4BYTE);
}

bool tg_gpu_prepare_arena(void *cpu, uint64_t physical, size_t bytes,
                          const uint32_t expected[2])
{
    uint64_t *pd = (uint64_t *)cpu, *pt;
    uint32_t *instance, *userd, *push, *fence, *runlist;
    struct tg_gpu_gpfifo_entry *ring;
    uint32_t i;
    uint64_t entry;
    if (!cpu || !expected || ((uintptr_t)cpu & 0xfff) || (physical & 0xfff) ||
        bytes < TG_GPU_ARENA_BYTES || physical < 0x80000000 ||
        physical > UINT64_C(0x100000000) - TG_GPU_ARENA_BYTES ||
        expected[0] == 0xdeadbeef || expected[1] == 0xdeadbeef || expected[0] == expected[1])
        return false;
    /* All backing pages are owned by the caller, not arbitrary OS memory. */
    for (i = 0; i < TG_GPU_ARENA_BYTES / 4; ++i) ((uint32_t *)cpu)[i] = 0;
    pt = (uint64_t *)((uint8_t *)cpu + 0x20000);
    instance = (uint32_t *)((uint8_t *)cpu + INST);
    userd = (uint32_t *)((uint8_t *)cpu + USERD);
    push = (uint32_t *)((uint8_t *)cpu + PUSH);
    fence = (uint32_t *)((uint8_t *)cpu + FENCE);
    ring = (struct tg_gpu_gpfifo_entry *)((uint8_t *)cpu + RING);
    runlist = (uint32_t *)((uint8_t *)cpu + RUNLIST);
    if (!tg_gpu_pde_small(physical + 0x20000, &pd[0]) ||
        !tg_gpu_instance_pdb(physical, &entry)) return false;
    instance[128] = (uint32_t)entry | 4; /* volatile PDB */
    instance[129] = (uint32_t)(entry >> 32);
    instance[130] = 0x00fff000; /* BAR1 and channel VA limited to 16 MiB */
    if (!tg_gpu_pte_4k(physical + RING, true, true, &pt[RING_VA >> 12]) ||
        !tg_gpu_pte_4k(physical + PUSH, true, true, &pt[PUSH_VA >> 12]) ||
        !tg_gpu_pte_4k(physical + FENCE, false, true, &pt[FENCE_VA >> 12]) ||
        !tg_gpu_pte_4k(physical + USERD, false, true, &pt[USERD_VA >> 12])) return false;

    instance[2] = (uint32_t)(physical + USERD) | 3; /* USERD, sysmem NCOH */
    instance[3] = 0;
    instance[4] = 0xface; /* hardware signature */
    instance[12] = 0x102; /* acquire retry mantissa/exp 2; no acquire emitted */
    instance[18] = RING_VA;
    instance[19] = 9u << 16; /* 512 GPFIFO entries */
    instance[33] = 0x20400000; /* main/user/first/incrementing pushbuffer */
    instance[37] = 0x30000001; /* subdevice 1 active, channel DMA */
    instance[39] = 0x100; /* PB Fermi1, GP/MP Fermi0 */
    instance[43] = 0x1f; /* software engine; methods below are FIFO methods */
    instance[62] = 0x10003080; /* runlist timeslice */
    instance[63] = 0x10000010; /* pushbuffer timeslice */
    userd[0] = 0x55aa3377; /* BAR1 visibility check only */
    fence[0] = fence[1] = 0xdeadbeef;
    release_packet(push, FENCE_VA, expected[0]);
    release_packet(push + 5, FENCE_VA + 4, expected[1]);
    if (!tg_gpu_build_gpfifo(PUSH_VA, 5, false, &ring[0]) ||
        !tg_gpu_build_gpfifo(PUSH_VA + 20, 5, true, &ring[1])) return false;
    runlist[0] = runlist[1] = 0; /* standalone channel zero */
    return true;
}

static uint32_t ex_read(const struct tg_gpu_power_io *io, uint32_t reg)
{ return io->read32(io->context, GPU + reg); }
static void ex_write(const struct tg_gpu_power_io *io, uint32_t reg, uint32_t val)
{
    io->write32(io->context, GPU + reg, val);
    (void)ex_read(io, reg);
}
static bool ex_wait(const struct tg_gpu_power_io *io, uint32_t reg,
                    uint32_t mask, uint32_t value)
{
    uint32_t i;
    for (i = 0; i < 1000; ++i) {
        if ((ex_read(io, reg) & mask) == value) return true;
        io->delay_us(io->context, 5);
    }
    return false;
}
void tg_gpu_execute_snapshot(const struct tg_gpu_power_io *io, struct tg_gpu_status *s)
{
    s->fifo_interrupt = ex_read(io, 0x2100);
    s->mmu_fault = ex_read(io, 0x259c);
    s->pbdma_interrupt = ex_read(io, 0x40108);
    s->channel_status = ex_read(io, 0x800004);
}

enum tg_gpu_execute_result tg_gpu_execute(const struct tg_gpu_power_io *io,
    void *cpu, uint64_t physical, struct tg_gpu_execution *state,
    struct tg_gpu_status *s)
{
    volatile uint32_t *fence;
    uint32_t i;
    if (!io || !cpu || !state || !s || state->owned ||
        !tg_gpu_prepare_arena(cpu, physical, TG_GPU_ARENA_BYTES, s->expected))
        return TG_GPU_EXEC_ARGUMENT;
    fence = (volatile uint32_t *)((uint8_t *)cpu + FENCE);
    state->stage = 1;
    if (ex_read(io, 0x800000) & 0x80000000) return TG_GPU_EXEC_BUSY;
    state->old_asid[0] = io->read32(io->context, 0x70019aa8);
    state->old_asid[1] = io->read32(io->context, 0x70019aac);
    state->owned = true;
    /* The probe owns this reset GPU. Its GPU MMU maps only the private
     * arena; use direct bus addresses on the two GPU SMMU groups only.
     * This is not a general Windows IOMMU implementation.
     */
    io->write32(io->context, 0x70019aa8, 0);
    io->write32(io->context, 0x70019aac, 0);
    if (io->read32(io->context, 0x70019aa8) || io->read32(io->context, 0x70019aac))
        return TG_GPU_EXEC_ARGUMENT;
    ex_write(io, 0x140, 0); ex_write(io, 0x144, 0); /* polled probe, no IRQs */
    ex_write(io, 0x20c, ex_read(io, 0x20c) | 0x20100004); /* FB/XBAR/HUB */
    ex_write(io, 0x200, ex_read(io, 0x200) & ~0x100u);
    io->delay_us(io->context, 10);
    ex_write(io, 0x200, ex_read(io, 0x200) | 0x100);
    ex_write(io, 0x2140, 0); ex_write(io, 0x2528, 0);
    ex_write(io, 0x4010c, 0); ex_write(io, 0x4014c, 0);
    ex_write(io, 0x2100, UINT32_MAX);
    ex_write(io, 0x40108, UINT32_MAX); ex_write(io, 0x40148, UINT32_MAX);
    ex_write(io, 0x2a00, UINT32_MAX);
    ex_write(io, 0x2a04, ex_read(io, 0x2a04) | 0xffff);
    ex_write(io, 0x4012c, ex_read(io, 0x4012c) | 0xffff);

    state->stage = 2;
    ex_write(io, 0x100c80, ex_read(io, 0x100c80) | 0x800);
    ex_write(io, 0x100c10, (uint32_t)((physical + FLUSH) >> 8));
    ex_write(io, 0x100cc8, (uint32_t)((physical + DEBUG_WR) >> 8) | 3);
    ex_write(io, 0x100ccc, (uint32_t)((physical + DEBUG_RD) >> 8) | 3);
    for (i = 0; i < 1000 && !(ex_read(io, 0x100c80) & 0x00ff0000); ++i)
        io->delay_us(io->context, 2);
    if (i == 1000) return TG_GPU_EXEC_MMU_TIMEOUT;
    ex_write(io, 0x100cb8, (uint32_t)(physical >> 8) | 2);
    ex_write(io, 0x100cbc, 0x80000001);
    if (!ex_wait(io, 0x100c80, 0x8000, 0x8000)) return TG_GPU_EXEC_MMU_TIMEOUT;
    ex_write(io, 0x1704, (uint32_t)((physical + INST) >> 12) | 0xb0000000);
    if (!ex_wait(io, 0x1710, 3, 0)) return TG_GPU_EXEC_BAR1;
    if (io->read32(io->context, BAR1 + USERD_VA) != 0x55aa3377)
        return TG_GPU_EXEC_BAR1;
    ex_write(io, 0x2254, (USERD_VA >> 12) | 0x10000000); /* USERD BAR1 snoop */

    state->stage = 3;
    ex_write(io, 0x800000, (uint32_t)((physical + INST) >> 12) | 0xb0000000);
    ex_write(io, 0x800004, (ex_read(io, 0x800004) & ~0x400u) | 0x400);
    ex_write(io, 0x2270, (uint32_t)((physical + RUNLIST) >> 12) | 0x30000000);
    ex_write(io, 0x2274, 1); /* runlist 0, one channel */
    if (!ex_wait(io, 0x2284, 0x100000, 0)) return TG_GPU_EXEC_RUNLIST;

    state->stage = 4;
    /* BAR1 writes notify FIFO; a CPU write to USERD alone does not. */
    io->write32(io->context, BAR1 + USERD_VA + 35 * 4, 1);
    for (i = 0; i < 10000 && fence[0] != s->expected[0]; ++i) {
        if (ex_read(io, 0x259c) || ex_read(io, 0x40108)) break;
        io->delay_us(io->context, 10);
    }
    s->observed[0] = fence[0];
    if (fence[0] == s->expected[0]) {
        state->stage = 5;
        io->write32(io->context, BAR1 + USERD_VA + 35 * 4, 2);
        for (i = 0; i < 10000 && fence[1] != s->expected[1]; ++i) {
            if (ex_read(io, 0x259c) || ex_read(io, 0x40108)) break;
            io->delay_us(io->context, 10);
        }
    }
    s->observed[1] = fence[1];
    tg_gpu_execute_snapshot(io, s);
    if (s->mmu_fault || s->pbdma_interrupt) return TG_GPU_EXEC_FAULT;
    if (s->observed[0] != s->expected[0] || s->observed[1] != s->expected[1])
        return TG_GPU_EXEC_TIMEOUT;
    state->stage = 6;
    return TG_GPU_EXEC_OK;
}

void tg_gpu_execute_stop(const struct tg_gpu_power_io *io, struct tg_gpu_execution *s)
{
    if (!s->owned) return;
    ex_write(io, 0x800004, ex_read(io, 0x800004) | 0x800);
    ex_write(io, 0x2274, 0);
    (void)ex_wait(io, 0x2284, 0x100000, 0);
    /* Reset the test FIFO even if runlist removal times out; MC drain and
     * full GPU reset still must succeed before the arena is freed.
     */
    ex_write(io, 0x200, ex_read(io, 0x200) & ~0x100u);
    io->delay_us(io->context, 10);
    ex_write(io, 0x800000, 0);
    ex_write(io, 0x1704, 0);
}

bool tg_gpu_execute_release_domain(const struct tg_gpu_power_io *io, struct tg_gpu_execution *s)
{
    if (!s->owned) return true;
    io->write32(io->context, 0x70019aa8, s->old_asid[0]);
    io->write32(io->context, 0x70019aac, s->old_asid[1]);
    if (io->read32(io->context, 0x70019aa8) != s->old_asid[0] ||
        io->read32(io->context, 0x70019aac) != s->old_asid[1]) return false;
    s->owned = false;
    return true;
}
