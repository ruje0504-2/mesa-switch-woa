/* SPDX-License-Identifier: MIT */
#include "../platform/t214_interrupt.h"
#include "../platform/t214_resources.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define GPU T214_GPU_REG_BASE
#define BIT T214_GPU_LIC_NONSTALL
struct model {
    uint32_t enable,mask,fifo_enable,fifo_pending,lic_enable,lic_class,force,mc;
    unsigned writes,fail_write;
    bool sticky,bad_read,gpu_off;
};
static uint32_t read_reg(void *ctx,uint32_t reg)
{
    struct model *m=ctx;
    if(reg>=GPU && reg<GPU+T214_GPU_REG_SIZE) assert(!m->gpu_off);
    if(m->bad_read) return UINT32_MAX;
    switch(reg) {
    case GPU+0x104:return m->mc;
    case GPU+0x144:return m->enable;
    case GPU+0x644:return m->mask;
    case GPU+0x2100:return m->fifo_pending;
    case GPU+0x2140:return m->fifo_enable;
    case T214_GPU_LIC_BASE:return m->force;
    case T214_GPU_LIC_BASE+12:return m->lic_enable;
    case T214_GPU_LIC_BASE+24:return m->lic_class;
    default:assert(!"unexpected register read");return UINT32_MAX;
    }
}
static void write_reg(void *ctx,uint32_t reg,uint32_t value)
{
    struct model *m=ctx;
    if(reg>=GPU && reg<GPU+T214_GPU_REG_SIZE) assert(!m->gpu_off);
    if(++m->writes==m->fail_write)return;
    switch(reg) {
    case GPU+0x144:m->enable=value;break;
    case GPU+0x644:m->mask=value;break;
    case GPU+0x2140:m->fifo_enable=value;break;
    case GPU+0x2100:
        assert(value==0x80000000u);
        if(!m->sticky)m->fifo_pending&=~value;
        break;
    case T214_GPU_LIC_BASE+8:assert(value==BIT);m->force&=~value;break;
    case T214_GPU_LIC_BASE+16:assert(value==BIT);m->lic_enable|=value;break;
    case T214_GPU_LIC_BASE+20:assert(value==BIT);m->lic_enable&=~value;break;
    case T214_GPU_LIC_BASE+24:m->lic_class=value;break;
    default:assert(!"unexpected register write");
    }
}
static struct model initial(void)
{
    struct model m={0};
    m.mask=0x01001000;m.fifo_enable=0x01000001;
    m.lic_enable=0x01234567;m.lic_class=BIT|0x20000;
    return m;
}
int main(void)
{
    struct model m=initial(),original=m;
    struct tg_gpu_power power={0};power.ready=true;
    struct tg_gpu_power_io io={0};io.context=&m;io.read32=read_reg;io.write32=write_reg;
    struct tg_gpu_irq_state q={0};struct tg_gpu_irq_status s={0};
    assert(tg_gpu_irq_arm(&io,&power,&q,&s)==TG_GPU_IRQ_OK);
    assert(q.armed&&q.saved&&m.enable==1&&m.mask==0x100);
    assert((m.lic_enable&BIT)&&!(m.lic_class&BIT));
    assert(!tg_gpu_irq_handle(&io,&q,&s));
    for(unsigned n=1;n<=2;++n) {
        m.mc=0x100;m.fifo_pending=0x81000000;m.force|=BIT;
        assert(tg_gpu_irq_handle(&io,&q,&s));
        assert(s.interrupts==n&&s.result==0&&m.fifo_pending==0x01000000&&!(m.force&BIT));
    }
    /* An unrelated owner can change every LIC/enable bit outside GPU's bit. */
    m.lic_enable^=0x100;m.lic_class^=0x8000;m.fifo_enable^=0x1000;
    assert(tg_gpu_irq_disarm(&io,&q,&s)==TG_GPU_IRQ_OK&&!q.saved&&!q.armed);
    assert(m.enable==original.enable&&m.mask==original.mask);
    assert(m.lic_enable==(original.lic_enable^0x100));
    assert(m.lic_class==(original.lic_class^0x8000));
    assert(m.fifo_enable==(original.fifo_enable^0x1000));
    unsigned writes=m.writes;assert(!tg_gpu_irq_handle(&io,&q,&s)&&m.writes==writes);

    /* Never take over an active CPU output or an inherited forced IRQ. */
    m=initial();q=(struct tg_gpu_irq_state){0};m.enable=1;
    assert(tg_gpu_irq_arm(&io,&power,&q,&s)==TG_GPU_IRQ_FOREIGN&&!m.writes&&!q.saved);
    m=initial();m.force=BIT;
    assert(tg_gpu_irq_arm(&io,&power,&q,&s)==TG_GPU_IRQ_FOREIGN&&!m.writes);
    m=initial();m.bad_read=true;
    assert(tg_gpu_irq_arm(&io,&power,&q,&s)==TG_GPU_IRQ_REGISTER&&!m.writes);
    power.ready=false;m=initial();
    assert(tg_gpu_irq_arm(&io,&power,&q,&s)==TG_GPU_IRQ_ARGUMENT&&!m.writes);power.ready=true;

    /* Any failed arm with a changed register remains restorable. */
    for(unsigned fail=1;fail<=7;++fail) {
        m=initial();q=(struct tg_gpu_irq_state){0};m.fail_write=fail;
        enum tg_gpu_irq_result r=tg_gpu_irq_arm(&io,&power,&q,&s);
        assert(q.saved); /* Even failure after a write retains its snapshot. */
        if(r==TG_GPU_IRQ_OK)assert(q.armed);
        else assert(r==TG_GPU_IRQ_REGISTER&&!q.armed);
        m.fail_write=0;
        assert(tg_gpu_irq_disarm(&io,&q,&s)==TG_GPU_IRQ_OK&&!q.saved);
        assert(m.enable==original.enable&&m.mask==original.mask&&m.fifo_enable==original.fifo_enable);
        assert(m.lic_enable==original.lic_enable&&m.lic_class==original.lic_class);
    }
    /* An unrecognized/sticky pending source is masked, never acknowledged as
     * completed work, and true FIFO error bits survive handler invocation.
     */
    for(unsigned sticky=0;sticky<2;++sticky) {
        m=initial();q=(struct tg_gpu_irq_state){0};
        assert(tg_gpu_irq_arm(&io,&power,&q,&s)==TG_GPU_IRQ_OK);
        m.mc=0x100;m.fifo_pending=sticky?0x81000000:0x01000000;m.sticky=sticky;
        assert(tg_gpu_irq_handle(&io,&q,&s)&&!s.interrupts&&!m.enable);
        assert(s.result==(sticky?TG_GPU_IRQ_STICKY:TG_GPU_IRQ_UNEXPECTED));
        assert(m.fifo_pending&0x01000000);
        m.sticky=false;assert(tg_gpu_irq_disarm(&io,&q,&s)==TG_GPU_IRQ_OK);
    }
    /* Restoration failure must not erase the original masks. */
    m=initial();q=(struct tg_gpu_irq_state){0};
    assert(tg_gpu_irq_arm(&io,&power,&q,&s)==TG_GPU_IRQ_OK);
    m.fail_write=m.writes+1;
    assert(tg_gpu_irq_disarm(&io,&q,&s)==TG_GPU_IRQ_REGISTER&&q.saved&&!q.armed);
    m.fail_write=0;assert(tg_gpu_irq_disarm(&io,&q,&s)==TG_GPU_IRQ_OK&&!q.saved);
    /* Full reset permits LIC restoration without any powered-off GPU read. */
    m=initial();q=(struct tg_gpu_irq_state){0};
    assert(tg_gpu_irq_arm(&io,&power,&q,&s)==TG_GPU_IRQ_OK);
    m.bad_read=true;
    assert(tg_gpu_irq_disarm(&io,&q,&s)==TG_GPU_IRQ_REGISTER&&q.saved);
    m.bad_read=false;m.gpu_off=true;
    assert(tg_gpu_irq_retire(&io,&q)==TG_GPU_IRQ_OK&&!q.saved);
    assert(s.restore_result==TG_GPU_IRQ_REGISTER);
    assert(m.lic_enable==original.lic_enable&&m.lic_class==original.lic_class);
    puts("GM20B nonstall IRQ routing, acknowledgement, ownership and failures: PASS (model only)");
    return 0;
}
