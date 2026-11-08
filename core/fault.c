/* SPDX-License-Identifier: MIT
 * Copyright (c) 2014-2018, NVIDIA CORPORATION. All rights reserved.
 * Follows Switchroot nvgpu 1ae0167d gm20b_fifo_init_pbdma_intr_descs,
 * gk20a_fifo_handle_pbdma_intr and gk20a_fifo_isr. See ../NOTICE.
 */
#include "fault.h"
#include "../windows/control.h"
#define GPU 0x57000000u
#define ENGINE_EVENT 0x01000000u
#define PBDMA_SUMMARY 0x20000000u
static void read_status(const struct tg_gpu_power_io *io, struct tg_gpu_fault_status *s)
{
    s->fifo = io->read32(io->context, GPU + 0x2100);
    s->mmu = io->read32(io->context, GPU + 0x259c);
    s->pbdma = io->read32(io->context, GPU + 0x40108);
    s->pbdma1 = io->read32(io->context, GPU + 0x40148);
}
bool tg_gpu_fault_poll(const struct tg_gpu_power_io *io, struct tg_gpu_fault_status *s)
{
    read_status(io, s);
    /* ENGINE (bit 24) is absent from GM20B's fatal/restartable masks. The
     * upstream ISR returns NO_RC for that lone bit, then W1C-acks it and the
     * FIFO PBDMA summary. Do not clear or mask concurrent real/unknown faults.
     * Read all sources again: failed W1C or a new fault must still stop work.
     */
    if (s->pbdma == ENGINE_EVENT && !s->pbdma1 && !s->mmu &&
        !tg_gpu_fifo_has_error(s->fifo & ~PBDMA_SUMMARY)) {
        io->write32(io->context, GPU + 0x40108, ENGINE_EVENT);
        (void)io->read32(io->context, GPU + 0x40108);
        io->write32(io->context, GPU + 0x2100, PBDMA_SUMMARY);
        (void)io->read32(io->context, GPU + 0x2100);
        read_status(io, s);
    }
    return tg_gpu_fifo_has_error(s->fifo) || s->mmu || s->pbdma || s->pbdma1;
}
