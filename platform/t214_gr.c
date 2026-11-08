/* SPDX-License-Identifier: MIT
 * Copyright (c) 2014-2018, NVIDIA CORPORATION. All rights reserved.
 * Adapted from Switchroot nvgpu 1ae0167d gk20a/gr_gk20a.c and gm20b/gr_gm20b.c.
 * See ../NOTICE. DMA bootstrap only; secure ACR and graphics contexts are separate.
 */
#include "t214_gr.h"
#define GPU 0x57000000u
#define FECS 0x409000u
#define GPCCS 0x41a000u
static uint32_t read_reg(const struct tg_gpu_power_io *io, uint32_t reg)
{ return io->read32(io->context, GPU + reg); }
static void write_reg(const struct tg_gpu_power_io *io, uint32_t reg, uint32_t value)
{
    io->write32(io->context, GPU + reg, value);
    (void)read_reg(io, reg);
}
/* Streaming ports advance on access; never add the normal posted-write readback. */
static void stream_word(const struct tg_gpu_power_io *io, uint32_t reg, uint32_t value)
{ io->write32(io->context, GPU + reg, value); }
static uint32_t le32(const uint8_t *p)
{ return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static bool wait_reg(const struct tg_gpu_power_io *io, struct tg_gpu_gr_status *s,
                     uint32_t reg, uint32_t mask, uint32_t expected)
{
    s->last_register = reg;
    for (unsigned i = 0; i < 10000; ++i) {
        s->last_value = read_reg(io, reg);
        if (s->last_value == UINT32_MAX) return false;
        if ((s->last_value & mask) == expected) return true;
        io->delay_us(io->context, 10);
    }
    return false;
}
static enum tg_gpu_gr_result finish(const struct tg_gpu_power_io *io,
    struct tg_gpu_frontend_status *frontend, struct tg_gpu_gr_status *s,
    enum tg_gpu_gr_result result)
{
    s->result = result;
    s->fecs_mailbox = read_reg(io, FECS + 0x800);
    s->gr_interrupt = read_reg(io, 0x400100);
    tg_gpu_frontend_snapshot(io, frontend);
    if (result == TG_GPU_GR_OK && frontend->result != TG_GPU_FE_OK)
        s->result = TG_GPU_GR_PRI_FAULT;
    return (enum tg_gpu_gr_result)s->result;
}
static bool arb_idle(const struct tg_gpu_power_io *io, struct tg_gpu_gr_status *s)
{
    return wait_reg(io,s,FECS+0xa10,0x1f,0) &&
           wait_reg(io,s,FECS+0x400,0x1000,0);
}
static bool gr_idle(const struct tg_gpu_power_io *io, struct tg_gpu_gr_status *s)
{
    /* Switchroot gr_gk20a_wait_idle: invalid context, or neither GR execution
     * nor context switching active. Reading GR_STATUS updates the engine view.
     */
    for (unsigned i = 0; i < 10000; ++i) {
        uint32_t engine, busy;
        (void)read_reg(io,0x400700);
        engine = read_reg(io,0x2640);
        busy = read_reg(io,0x40060c);
        s->last_register = 0x2640; s->last_value = engine;
        if (engine == UINT32_MAX || busy == UINT32_MAX) return false;
        if (!(engine & 0xe000) || (!(busy & 1) && !(engine & 0x8000))) return true;
        io->delay_us(io->context,10);
    }
    return false;
}
enum tg_gpu_gr_result tg_gpu_gr_boot(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power, struct tg_gpu_frontend_status *frontend,
    const struct tg_gpu_gr_netlist *netlist, const struct tg_gpu_gr_image *image,
    uint64_t gpu_va, uint64_t instance_physical, struct tg_gpu_gr_status *s)
{
    const uint32_t bases[2] = {FECS,GPCCS};
    uint32_t headers[2][17], counts[2], configs[2], inst;
    struct tg_gpu_gr_blob nonctx;
    if (!s) return TG_GPU_GR_ARGUMENT;
    *s = (struct tg_gpu_gr_status){0};
    if (!io || !io->read32 || !io->write32 || !io->delay_us || !power ||
        !power->owned || !power->ready || !frontend || frontend->result ||
        frontend->stage != 11 || !netlist || !image ||
        instance_physical < 0x80000000 || instance_physical > 0xfffff000 ||
        (instance_physical & 0xfff)) return s->result = TG_GPU_GR_ARGUMENT;
    nonctx = netlist->region[TG_GPU_GR_NONCONTEXT];
    if (!nonctx.data || !nonctx.size || nonctx.size % 8)
        return s->result = TG_GPU_GR_FIRMWARE_FORMAT;
    /* Validate the whole register stream before its first write. The caller
     * supplied the checked driver-owned NETB, never a user command stream.
     */
    for (size_t i = 0; i < nonctx.size; i += 8) {
        uint32_t reg = le32(nonctx.data+i);
        if ((reg & 3) || reg < 0x400000 || reg >= 0x600000)
            return s->result = TG_GPU_GR_FIRMWARE_FORMAT;
    }
    for (unsigned i = 0; i < 2; ++i)
        if (tg_gpu_gr_boot_header(image,i,gpu_va,headers[i],&counts[i]) != TG_GPU_GR_FW_OK)
            return s->result = TG_GPU_GR_FIRMWARE_FORMAT;
    s->stage = 1;
    s->priv_security = read_reg(io,0x21434);
    if (s->priv_security == UINT32_MAX) return finish(io,frontend,s,TG_GPU_GR_REGISTER);
    /* The secure branch requires authenticated ACR/WPR setup; do not bypass
     * it or start the non-secure bootloader when the fuse selects that path.
     */
    if (s->priv_security) return finish(io,frontend,s,TG_GPU_GR_SECURITY_REQUIRED);
    configs[0] = s->fecs_hwcfg = read_reg(io,FECS+0x108);
    configs[1] = s->gpccs_hwcfg = read_reg(io,GPCCS+0x108);
    for (unsigned i = 0; i < 2; ++i) {
        const struct tg_gpu_gr_falcon_image *f = &image->falcon[i];
        uint32_t imem = (configs[i] & 0x1ff) * 256;
        uint32_t dmem = ((configs[i] >> 9) & 0x1ff) * 256;
        uint32_t boot_size = (f->boot.size + 255u) & ~255u;
        if (configs[i] == UINT32_MAX || !imem || !dmem)
            return finish(io,frontend,s,TG_GPU_GR_REGISTER);
        if (!boot_size || (f->imem_offset & 255) || f->imem_offset > imem ||
            boot_size > imem-f->imem_offset || f->code.size > imem ||
            f->data.size > dmem || counts[i]*4 > dmem ||
            f->entry < f->imem_offset || f->entry-f->imem_offset >= f->boot.size ||
            ((gpu_va+f->boot.offset)>>8) < (f->imem_offset>>8))
            return finish(io,frontend,s,TG_GPU_GR_FIRMWARE_FORMAT);
    }
    s->stage = 2;
    /* Enable only local GR sources; the enclosing probe keeps MC CPU IRQ
     * outputs disabled. Follow reset_enable_hw: non-context pack then scrub.
     */
    write_reg(io,0x400100,UINT32_MAX); write_reg(io,0x40013c,UINT32_MAX);
    for (size_t i = 0; i < nonctx.size; i += 8)
        write_reg(io,le32(nonctx.data+i),le32(nonctx.data+i+4));
    tg_gpu_frontend_snapshot(io,frontend);
    if (frontend->result) return finish(io,frontend,s,TG_GPU_GR_PRI_FAULT);
    s->stage = 3;
    if (!wait_reg(io,s,FECS+0x10c,6,0) || !wait_reg(io,s,GPCCS+0x10c,6,0))
        return finish(io,frontend,s,TG_GPU_GR_SCRUB_TIMEOUT);
    if (!gr_idle(io,s)) return finish(io,frontend,s,TG_GPU_GR_IDLE_TIMEOUT);
    s->stage = 4;
    if (!arb_idle(io,s)) return finish(io,frontend,s,TG_GPU_GR_ARB_TIMEOUT);
    inst = (uint32_t)(instance_physical >> 12);
    write_reg(io,FECS+0xa24,0);
    write_reg(io,FECS+0xb04,inst|0x80000000); /* resolved SoC DRAM target */
    write_reg(io,FECS+0xa0c,inst);
    write_reg(io,FECS+0xa10,7);
    if (!arb_idle(io,s)) return finish(io,frontend,s,TG_GPU_GR_ARB_TIMEOUT);
    /* Upstream's current-context marker for its arbiter flush explicitly
     * uses target_m (3). Keep it distinct from the new-context DMA binding.
     */
    write_reg(io,FECS+0xb00,inst|0xb0000000);
    write_reg(io,FECS+0xa10,5);
    if (!arb_idle(io,s)) return finish(io,frontend,s,TG_GPU_GR_ARB_TIMEOUT);
    write_reg(io,FECS+0x840,UINT32_MAX);
    for (unsigned i = 0; i < 2; ++i) {
        const struct tg_gpu_gr_falcon_image *f = &image->falcon[i];
        uint32_t base = bases[i];
        s->stage = 5+i;
        write_reg(io,base+0x10c,0); /* virtual DMA without require-context gate */
        write_reg(io,base+0x1c0,0x01000000);
        for (uint32_t j = 0; j < counts[i]; ++j) stream_word(io,base+0x1c4,headers[i][j]);
        write_reg(io,base+0x110,(uint32_t)((gpu_va+f->boot.offset)>>8)-(f->imem_offset>>8));
        for (uint32_t b = 0; b < (f->boot.size+255u)/256; ++b) {
            uint32_t offset = f->imem_offset + b*256;
            write_reg(io,base+0x114,offset); write_reg(io,base+0x11c,offset);
            write_reg(io,base+0x118,0x610); /* IMEM, 256 bytes, DMA index zero */
            /* Falcon DMATRFCMD bit 1 is IDLE (set on completion), not BUSY. */
            if (!wait_reg(io,s,base+0x118,2,2)) return finish(io,frontend,s,TG_GPU_GR_DMA_TIMEOUT);
        }
        write_reg(io,base+0x104,f->entry);
        write_reg(io,base+0x100,2);
    }
    s->stage = 7;
    if (!wait_reg(io,s,FECS+0x800,UINT32_MAX,1))
        return finish(io,frontend,s,TG_GPU_GR_READY_TIMEOUT);
    s->stage = 8;
    return finish(io,frontend,s,TG_GPU_GR_OK);
}
