/* SPDX-License-Identifier: MIT
 * Copyright (c) 2014-2018, NVIDIA CORPORATION. All rights reserved.
 *
 * GM20B register sequences and force-run gating values adapted from
 * switch-l4t-kernel-nvgpu 1ae0167: common/{bus,priv_ring,ltc,fb,clock_gating},
 * gk20a/fifo_gk20a.c and gk20a/mm_gk20a.c.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#include "t214_frontend.h"
#include "t214_power.h"
#include <stddef.h>

#define GPU 0x57000000u
static uint32_t get(const struct tg_gpu_power_io *io, uint32_t reg)
{ return io->read32(io->context, GPU + reg); }
static void put(const struct tg_gpu_power_io *io, uint32_t reg, uint32_t value)
{
    io->write32(io->context, GPU + reg, value);
    (void)get(io, reg);
}
static bool owned(const struct tg_gpu_power_io *io, const struct tg_gpu_power *p,
                  const struct tg_gpu_frontend_status *s)
{ return io && io->read32 && io->write32 && io->delay_us && p && p->owned && p->ready && s; }

void tg_gpu_frontend_snapshot(const struct tg_gpu_power_io *io,
                             struct tg_gpu_frontend_status *s)
{
    s->bus_interrupt = get(io, 0x1100);
    s->pri_status0 = get(io, 0x120058);
    s->pri_status1 = get(io, 0x12005c);
    s->pri_error_address = s->pri_error_code = 0;
    /* SYS error info is valid only with the corresponding PRI status bit. */
    if (s->pri_status0 != UINT32_MAX && (s->pri_status0 & 0x100)) {
        s->pri_error_address = get(io, 0x122120);
        s->pri_error_code = get(io, 0x12212c);
    }
    if (s->result == TG_GPU_FE_OK &&
        ((s->bus_interrupt & 0xe) || (s->pri_status0 & 0x107) || s->pri_status1))
        s->result = TG_GPU_FE_PRI_FAULT;
}
static enum tg_gpu_frontend_result finish(const struct tg_gpu_power_io *io,
    struct tg_gpu_frontend_status *s, enum tg_gpu_frontend_result result)
{
    s->result = result;
    tg_gpu_frontend_snapshot(io, s);
    /* Keep the first specific failure; retain PRI evidence alongside it. */
    if (result != TG_GPU_FE_OK) s->result = result;
    return (enum tg_gpu_frontend_result)s->result;
}

enum tg_gpu_frontend_result tg_gpu_frontend_start(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power, struct tg_gpu_frontend_status *s)
{
    if (!owned(io, power, s)) return TG_GPU_FE_ARGUMENT;
    *s = (struct tg_gpu_frontend_status){0};
    s->stage = 1;
    /* Poll local fault sources; never enable the CPU IRQ lines in this probe. */
    put(io, 0x140, 0); put(io, 0x144, 0);
    put(io, 0x1100, 0xe); put(io, 0x1140, 0xe);
    put(io, 0x12004c, 4); put(io, 0x122204, 2);
    io->delay_us(io->context, 10);
    return finish(io, s, TG_GPU_FE_OK);
}

enum tg_gpu_frontend_result tg_gpu_frontend_fifo(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power, struct tg_gpu_frontend_status *s)
{
    uint32_t timeout;
    if (!owned(io, power, s)) return TG_GPU_FE_ARGUMENT;
    s->stage = 2;
    /* No PMU clock-gating governor yet: use the upstream disable profiles. */
    put(io, 0x26ac, 0x1fffe); put(io, 0x26a4, 0);
    timeout = get(io, 0x2a04);
    put(io, 0x2a04, timeout | 0x3fffffff);
    put(io, 0x4012c, UINT32_MAX); /* full PBDMA timeout field, not 16 bits */
    return finish(io, s, TG_GPU_FE_OK);
}

enum tg_gpu_frontend_result tg_gpu_frontend_memory(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power, struct tg_gpu_frontend_status *s)
{
    static const uint32_t force_run[][2] = {
        {0x100d14,0xfffffffe}, {0x100c9c,0x1fe},
        {0x100d10,0}, {0x100d30,0}, {0x100d3c,0}, {0x100d48,0}, {0x100c98,0},
        {0x17e050,0xfffffffe}, {0x17e35c,0xfffffffe},
        {0x17e030,0}, {0x17e040,0}, {0x17e3e0,0}, {0x17e3c8,0}
    };
    uint32_t count, maximum, config, intr, elpg, priv_security;
    size_t i;
    if (!owned(io, power, s)) return TG_GPU_FE_ARGUMENT;
    s->stage = 3;
    maximum = get(io, 0x22454);
    count = get(io, 0x12006c);
    if (maximum == UINT32_MAX || count == UINT32_MAX || !maximum ||
        !(count & 0x1f) || (count & 0x1f) > maximum)
        return finish(io, s, TG_GPU_FE_LTC);
    s->ltc_count = count & 0x1f;
    config = get(io, 0x140518);
    intr = get(io, 0x17e20c);
    if (config == UINT32_MAX || intr == UINT32_MAX)
        return finish(io, s, TG_GPU_FE_REGISTER);
    put(io, 0x17e27c, s->ltc_count);
    put(io, 0x17e000, s->ltc_count);
    put(io, 0x17e318, config | 0x8000);
    put(io, 0x17e20c, intr & ~0x40300000u);
    if (finish(io, s, TG_GPU_FE_OK) != TG_GPU_FE_OK) return (enum tg_gpu_frontend_result)s->result;

    s->stage = 4;
    elpg = get(io, 0x20c);
    priv_security = get(io, 0x21434);
    if (elpg == UINT32_MAX || priv_security == UINT32_MAX)
        return finish(io, s, TG_GPU_FE_REGISTER);
    put(io, 0x20c, elpg | 0x20100004); /* FB/XBAR/HUB, not MC_ENABLE */
    for (i = 0; i < sizeof(force_run) / sizeof(force_run[0]); ++i)
        put(io, force_run[i][0], force_run[i][1]);
    /* 0x100d1c is deliberately excluded: upstream marks it a PRI error. */
    put(io, 0x100800, s->ltc_count);
    /* Match the non-secure branch only; never alter fuses or secure policy. */
    if (!priv_security) put(io, 0x100ce4, UINT32_MAX);
    return finish(io, s, TG_GPU_FE_OK);
}

enum tg_gpu_frontend_result tg_gpu_frontend_flush(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power, struct tg_gpu_frontend_status *s)
{
    unsigned pass, retry;
    uint32_t value;
    if (!owned(io, power, s)) return TG_GPU_FE_ARGUMENT;
    s->stage = 5;
    s->fb_flushes = 0;
    /* Upstream MMU setup issues two FB flushes after BAR1 binding. This is an
     * internal L2 sysmembar, not proof of GPU execution or a DRAM readback.
     * The caller must first program an owned NISO flush-scratch address.
     */
    for (pass = 0; pass < 2; ++pass) {
        put(io, 0x70000, 1);
        for (retry = 0; retry < 100; ++retry) {
            value = get(io, 0x70000);
            if (value == UINT32_MAX) return finish(io, s, TG_GPU_FE_REGISTER);
            if (!(value & 3)) break;
            io->delay_us(io->context, 5);
        }
        if (retry == 100) return finish(io, s, TG_GPU_FE_FLUSH_TIMEOUT);
        ++s->fb_flushes;
    }
    s->stage = 6;
    return finish(io, s, TG_GPU_FE_OK);
}

enum tg_gpu_frontend_result tg_gpu_frontend_sync_memory(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power, struct tg_gpu_frontend_status *s)
{
    static const uint32_t registers[] = {0x70010, 0x70004};
    static const unsigned limits[] = {2000, 200};
    unsigned pass, retry;
    uint32_t value;
    if (!owned(io, power, s)) return TG_GPU_FE_ARGUMENT;
    if (tg_gpu_frontend_flush(io, power, s) != TG_GPU_FE_OK)
        return (enum tg_gpu_frontend_result)s->result;
    /* Switchroot gk20a_mm_l2_flush(true): dirty writeback precedes clean-line
     * invalidation. FB flush alone expressly does not guarantee DRAM visibility.
     */
    for (pass = 0; pass < 2; ++pass) {
        s->stage = 7 + pass;
        put(io, registers[pass], 1);
        for (retry = 0; retry < limits[pass]; ++retry) {
            value = get(io, registers[pass]);
            if (value == UINT32_MAX) return finish(io, s, TG_GPU_FE_REGISTER);
            if (!(value & 3)) break;
            io->delay_us(io->context, 5);
        }
        if (retry == limits[pass]) return finish(io, s, TG_GPU_FE_L2_TIMEOUT);
        if (pass) ++s->l2_invalidations; else ++s->l2_flushes;
    }
    s->stage = 9;
    return finish(io, s, TG_GPU_FE_OK);
}
