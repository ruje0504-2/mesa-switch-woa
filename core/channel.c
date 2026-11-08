/* SPDX-License-Identifier: MIT
 * CCSR, runlist and preempt sequence follows Switchroot nvgpu 1ae0167d
 * fifo_gk20a.c and nouveau gk104.c/gf100.c. See ../NOTICE.
 */
#include "channel.h"
#include "../windows/control.h"
#include <stddef.h>

static uint32_t get(const struct tg_gpu_power_io *io,uint32_t reg)
{ return io->read32(io->context,0x57000000u+reg); }
static void put(const struct tg_gpu_power_io *io,uint32_t reg,uint32_t value)
{
    io->write32(io->context,0x57000000u+reg,value);
    (void)get(io,reg);
}
static bool ready(const struct tg_gpu_power_io *io,const struct tg_gpu_power *power)
{ return io && io->read32 && io->write32 && io->delay_us && power && power->owned && power->ready; }
static bool physical_page(uint64_t address)
{ return address>=UINT64_C(0x80000000) && address<=UINT64_C(0xfffff000) && !(address&0xfff); }
static bool wait_bits(const struct tg_gpu_power_io *io,struct tg_gpu_channel *c,
                      uint32_t reg,uint32_t mask,uint32_t wanted)
{
    unsigned retry;
    c->last_register=reg;
    for(retry=0;retry<20000;++retry) {
        c->last_value=get(io,reg);
        if(c->last_value==UINT32_MAX) return false;
        if((c->last_value&mask)==wanted) return true;
        io->delay_us(io->context,5);
    }
    return false;
}
static enum tg_gpu_channel_result fault(const struct tg_gpu_power_io *io,struct tg_gpu_channel *c)
{
    c->fifo_interrupt=get(io,0x2100); c->mmu_fault=get(io,0x259c);
    c->pbdma_interrupt=get(io,0x40108); c->pbdma_interrupt_1=get(io,0x40148);
    tg_gpu_frontend_snapshot(io,&c->frontend);
    return tg_gpu_fifo_has_error(c->fifo_interrupt) || c->mmu_fault || c->pbdma_interrupt || c->pbdma_interrupt_1 || c->frontend.result ?
        TG_GPU_CHANNEL_FAULT : TG_GPU_CHANNEL_OK;
}
enum tg_gpu_channel_result tg_gpu_channel_submit(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power,uint64_t root,uint64_t instance,uint64_t runlist,
    const void *owner,struct tg_gpu_channel *c)
{
    uint32_t value,inst;
    if(!c || !owner || !physical_page(root) || !physical_page(instance) || !physical_page(runlist))
        return TG_GPU_CHANNEL_ARGUMENT;
    if(!ready(io,power)) return TG_GPU_CHANNEL_NOT_READY;
    if(c->referenced) return TG_GPU_CHANNEL_BUSY;
    c->stage=1;
    /* Do not read channel SRAM while PFIFO is reset. */
    value=get(io,0x200);
    if(value==UINT32_MAX || !(value&0x100)) return TG_GPU_CHANNEL_NOT_READY;
    value=get(io,0x204);
    if(value==UINT32_MAX || !(value&1)) return TG_GPU_CHANNEL_NOT_READY;
    value=get(io,0x2390);
    if(value==UINT32_MAX || !(value&1)) return TG_GPU_CHANNEL_NOT_READY;
    value=get(io,0x800000);
    if(value==UINT32_MAX) return TG_GPU_CHANNEL_IO;
    if(value&0x80000000) return TG_GPU_CHANNEL_BUSY;
    if(fault(io,c)!=TG_GPU_CHANNEL_OK) return TG_GPU_CHANNEL_FAULT;
    c->owner=owner;c->power=power;c->root=root;c->referenced=true;c->submitted=false;
    c->stage=2;
    value=get(io,0x2630);
    if(value==UINT32_MAX) return TG_GPU_CHANNEL_IO;
    put(io,0x2630,value|1);
    if(!wait_bits(io,c,0x2630,1,1)) return TG_GPU_CHANNEL_IO;
    if(tg_gpu_tlb_invalidate(io,power,root)!=TG_GPU_TLB_OK) return TG_GPU_CHANNEL_TLB;
    c->stage=3;
    inst=(uint32_t)(instance>>12)|0xb0000000;
    put(io,0x800000,inst);
    if(!wait_bits(io,c,0x800000,0xbfffffff,inst)) return TG_GPU_CHANNEL_IO;
    value=get(io,0x800004);
    if(value==UINT32_MAX) return TG_GPU_CHANNEL_IO;
    put(io,0x800004,(value&~0xc00u)|0x400);
    put(io,0x2270,(uint32_t)(runlist>>12)|0x30000000);
    put(io,0x2274,1);
    if(!wait_bits(io,c,0x2284,0x100000,0)) return TG_GPU_CHANNEL_TIMEOUT;
    value=get(io,0x2630);
    if(value==UINT32_MAX) return TG_GPU_CHANNEL_IO;
    put(io,0x2630,value&~1u);
    if(!wait_bits(io,c,0x2630,1,0)) return TG_GPU_CHANNEL_IO;
    if(fault(io,c)!=TG_GPU_CHANNEL_OK) return TG_GPU_CHANNEL_FAULT;
    c->stage=4;c->submitted=true;
    return TG_GPU_CHANNEL_OK; /* queued, not a completed GPU fence */
}
enum tg_gpu_channel_result tg_gpu_channel_retire(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power,const void *owner,struct tg_gpu_channel *c)
{
    uint32_t value;
    if(!c || !owner) return TG_GPU_CHANNEL_ARGUMENT;
    if(!c->referenced) return TG_GPU_CHANNEL_OK;
    if(c->owner!=owner) return TG_GPU_CHANNEL_BUSY;
    if(c->power!=power) return TG_GPU_CHANNEL_ARGUMENT;
    if(!ready(io,power)) return TG_GPU_CHANNEL_NOT_READY;
    c->stage=5;
    value=get(io,0x2630);
    if(value==UINT32_MAX) return TG_GPU_CHANNEL_IO;
    put(io,0x2630,value|1);
    if(!wait_bits(io,c,0x2630,1,1)) return TG_GPU_CHANNEL_IO;
    value=get(io,0x800004);
    if(value==UINT32_MAX) return TG_GPU_CHANNEL_IO;
    put(io,0x800004,(value&~0xc00u)|0x800);
    if(!wait_bits(io,c,0x800004,1,0)) return TG_GPU_CHANNEL_IO;
    put(io,0x2634,0); /* preempt bare channel zero */
    if(!wait_bits(io,c,0x2634,0x100000,0)) return TG_GPU_CHANNEL_TIMEOUT;
    put(io,0x2274,0);
    if(!wait_bits(io,c,0x2284,0x100000,0)) return TG_GPU_CHANNEL_TIMEOUT;
    /* With no other channels in this engine, PBDMA must become invalid/idle.
     * An inactive reader's other SRAM fields are not completion evidence.
     */
    if(!wait_bits(io,c,0x3080,0xe000,0)) return TG_GPU_CHANNEL_TIMEOUT;
    put(io,0x800000,0);
    if(!wait_bits(io,c,0x800000,0x80000000,0)) return TG_GPU_CHANNEL_IO;
    c->stage=6;
    if(tg_gpu_frontend_flush(io,power,&c->frontend)!=TG_GPU_FE_OK) return TG_GPU_CHANNEL_FLUSH;
    if(tg_gpu_tlb_invalidate(io,power,c->root)!=TG_GPU_TLB_OK) return TG_GPU_CHANNEL_TLB;
    c->owner=NULL;c->referenced=false;c->submitted=false;c->stage=7;
    return TG_GPU_CHANNEL_OK;
}
void tg_gpu_channel_disable(const struct tg_gpu_power_io *io,const struct tg_gpu_power *power)
{
    uint32_t value;
    if(!ready(io,power)) return;
    value=get(io,0x2630);if(value!=UINT32_MAX) put(io,0x2630,value|1);
    put(io,0x204,0); /* exclusive GPU recovery; stop all pushbuffer readers */
    value=get(io,0x200);if(value!=UINT32_MAX) put(io,0x200,value&~0x100u);
    io->delay_us(io->context,20);
}
