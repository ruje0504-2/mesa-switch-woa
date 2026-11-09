/* SPDX-License-Identifier: MIT */
/* Explicit MMIO/GPU-write substitutes. No physical GPU is exercised here. */
#include "../platform/t214_gr_context.h"
#include "../core/gm20b_mmu.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define GPU 0x57000000u
struct model {
    uint32_t *regs,*inst,*work;
    unsigned writes,binds,saves,power,reset,flushes;
    bool no_power,no_bind,no_save,bind_fail,save_fail,no_image,bad_magic,pri;
    uint32_t busy_reg;
};
static uint32_t read32(void *context,uint32_t address)
{
    struct model *m=context;uint32_t r=address-GPU;
    assert(address>=GPU && r<0x600000 && !(r&3));
    assert(r!=0x404488 && r!=0x40448c && r!=0x400200 && r!=0x400204);
    return m->regs[r/4];
}
static void write32(void *context,uint32_t address,uint32_t value)
{
    struct model *m=context;uint32_t r=address-GPU;
    assert(address>=GPU && r<0x600000 && !(r&3));
    /* No global IRQ, PFIFO, security fuse, SMMU or power-control writes. */
    assert(r>=0x400000 || r==0x70000 || r==0x70010 || r==0x70004);
    ++m->writes;
    if(r==0x70000 || r==0x70010 || r==0x70004) {
        assert(value==1);
        if(r==0x70010) {
            ++m->flushes;
            if(m->saves && !m->no_image && !m->save_fail)
                m->work[(TG_GPU_GR_GOLD_OFFSET+256)/4]=0x12348765; /* GPU substitute */
        }
        m->regs[r/4]=r==m->busy_reg?3:0;return;
    }
    if(r==0x404170) {
        assert(value==0x12 || value==0x10);++m->power;
        m->regs[r/4]=m->no_power?value:value&~0x10u;return;
    }
    if(r==0x409614) {
        assert(value==(m->reset?0x770u:0x70u));++m->reset;
    }
    if(r==0x409840) {m->regs[0x409800/4]&=~value;return;}
    if(r==0x409504) {
        assert(m->regs[0x409500/4]==0x80080040);
        if(value==3) {
            assert(m->power==2 && m->reset==2 && !m->binds);
            assert(m->inst[132]==(TG_GPU_GR_WORK_VA|4) && m->flushes);
            ++m->binds;
            if(!m->no_bind) m->regs[0x409800/4]|=m->bind_fail?0x30:0x10;
            m->work[0xfc/4]=m->bad_magic?0:0x600dc0de; /* FECS header substitute */
        } else {
            assert(value==9 && m->binds==1 && !m->saves);
            assert(m->inst[132]==((TG_GPU_GR_WORK_VA+TG_GPU_GR_GOLD_OFFSET)|4));
            assert(m->work[(TG_GPU_GR_GOLD_OFFSET+0x1c)/4]==1);
            assert(!m->work[(TG_GPU_GR_GOLD_OFFSET+0x20)/4]);
            assert(m->work[(TG_GPU_GR_GOLD_OFFSET+256)/4]==TG_GPU_GR_GOLD_SENTINEL);
            ++m->saves;
            if(!m->no_save) m->regs[0x409800/4]|=m->save_fail?3:1;
            if(m->pri) m->regs[0x120058/4]=0x100;
        }
    }
    m->regs[r/4]=value;
}
static void delay(void *context,uint32_t us)
{(void)context;assert(us==5 || us==10);}
static void reset(struct model *m)
{
    uint32_t *regs=m->regs,*inst=m->inst,*work=m->work;
    memset(regs,0,0x600000);memset(inst,0,4096);memset(work,0,TG_GPU_GR_WORK_BYTES);
    *m=(struct model){.regs=regs,.inst=inst,.work=work};
    regs[0x120078/4]=1;regs[0x22430/4]=1;regs[0x22434/4]=2;
    regs[0x502608/4]=0x20002;regs[0x500c30/4]=3;
    regs[0x409800/4]=0x31; /* An old success/failure reply must be cleared. */
    for(unsigned i=0;i<TG_GPU_GR_CTX_CAPACITY/4;++i)
        work[TG_GPU_GR_GOLD_OFFSET/4+i]=TG_GPU_GR_GOLD_SENTINEL;
}
static enum tg_gpu_gr_context_result run(struct model *m,struct tg_gpu_gr_netlist *net,
    struct tg_gpu_gr_context_status *s)
{
    struct tg_gpu_power_io io={m,read32,write32,NULL,NULL,delay};
    struct tg_gpu_power power={.owned=true,.ready=true};
    struct tg_gpu_frontend_status fe={.stage=11,.ltc_count=2};
    struct tg_gpu_gr_status gr={.stage=15,.context_bytes=0x13800};
    struct tg_gpu_gr_work work={m->work,0x90000000,TG_GPU_GR_WORK_BYTES};
    return tg_gpu_gr_context_init(&io,&power,&fe,net,&gr,m->inst,0x80040000,&work,s);
}
int main(int argc,char **argv)
{
    struct model m={.regs=calloc(1,0x600000),.inst=calloc(1,4096),
        .work=aligned_alloc(4096,TG_GPU_GR_WORK_BYTES)};
    struct tg_gpu_gr_context_status s;
    uint32_t ctx[]={0x400080,0,0x1234},bundle[]={0xe100,0xabc},method[]={0x100,0x55};
    struct tg_gpu_gr_netlist net={0};
    assert(m.regs && m.inst && m.work);
    net.region[TG_GPU_GR_CONTEXT]=(struct tg_gpu_gr_blob){(const uint8_t *)ctx,sizeof(ctx)};
    net.region[TG_GPU_GR_BUNDLE]=(struct tg_gpu_gr_blob){(const uint8_t *)bundle,sizeof(bundle)};
    net.region[TG_GPU_GR_METHOD]=(struct tg_gpu_gr_blob){(const uint8_t *)method,sizeof(method)};
    reset(&m);assert(run(&m,&net,&s)==TG_GPU_GR_CTX_OK && s.stage==9);
    assert(s.image_magic==0x600dc0de && s.image_changed_words==1 && m.binds==1 && m.saves==1);
    assert(m.inst[132]==(TG_GPU_GR_WORK_VA|4) && !m.regs[0x409b00/4]);
    /* Independent vendor values: CB backing limits/valid selectors, state
     * limit, TPC routing and original INDEX/VALUE interpretation. */
    assert(m.regs[0x419848/4]==0x10000150 && m.regs[0x419c2c/4]==0x10000150);
    assert(m.regs[0x4064c8/4]==0x00c001c0 && m.regs[0x5030f8/4]==0xc00);
    assert(m.regs[0x405b60/4]==0x100 && m.regs[0x405ba0/4]==0x100);
    assert(m.regs[0x400080/4]==0x1234 && m.regs[0x404488/4]==0x80000100);
    assert(!m.regs[0x400208/4] && m.regs[0x404154/4]==0x800);
    for(unsigned test=0;test<9;++test) {
        reset(&m);
        if(test==0)m.no_power=true;if(test==1)m.no_bind=true;
        if(test==2)m.bind_fail=true;if(test==3)m.no_save=true;
        if(test==4)m.save_fail=true;if(test==5)m.no_image=true;
        if(test==6)m.bad_magic=true;if(test==7)m.busy_reg=0x70010;
        if(test==8)m.pri=true;
        const unsigned expected[]={TG_GPU_GR_CTX_TIMEOUT,TG_GPU_GR_CTX_TIMEOUT,
            TG_GPU_GR_CTX_FECS_REJECTED,TG_GPU_GR_CTX_TIMEOUT,TG_GPU_GR_CTX_FECS_REJECTED,
            TG_GPU_GR_CTX_IMAGE,TG_GPU_GR_CTX_IMAGE,TG_GPU_GR_CTX_VISIBILITY,TG_GPU_GR_CTX_VISIBILITY};
        assert(run(&m,&net,&s)==expected[test] && s.stage<9);
        if(test<3 || test==7)assert(!m.saves);
        assert(!m.regs[0x400208/4] && m.regs[0x404154/4]==0x800);
    }
    reset(&m);m.regs[0x502608/4]=0x20001;
    assert(run(&m,&net,&s)==TG_GPU_GR_CTX_TOPOLOGY && !m.writes);
    reset(&m);ctx[0]=0x70000;
    assert(run(&m,&net,&s)==TG_GPU_GR_CTX_FORMAT && !m.writes);ctx[0]=0x400080;
    {
        void *arena=aligned_alloc(4096,0x80000);assert(arena);memset(arena,0,0x80000);
        struct tg_gpu_gr_work work={m.work,0x90000000,TG_GPU_GR_WORK_BYTES};
        uint64_t *pt=(uint64_t *)((uint8_t *)arena+0x20000),pte;
        assert(tg_gpu_gr_context_prepare(arena,0x80000000,0x80000,&work));
        assert(tg_gpu_pte_4k(0x90000000,false,true,&pte) && pt[TG_GPU_GR_WORK_VA>>12]==pte);
        assert(m.work[TG_GPU_GR_GOLD_OFFSET/4]==TG_GPU_GR_GOLD_SENTINEL && !m.work[0]);
        assert(!tg_gpu_gr_context_prepare(arena,0x80000000,0x80000,&work));
        memset(arena,0,0x80000);work.physical=0x80040000;
        assert(!tg_gpu_gr_context_prepare(arena,0x80000000,0x80000,&work) && !pt[TG_GPU_GR_WORK_VA>>12]);
        work.physical=0xfffff000;
        assert(!tg_gpu_gr_context_prepare(arena,0x80000000,0x80000,&work));free(arena);
    }
    if(argc==2) {
        FILE *f=fopen(argv[1],"rb");assert(f && !fseek(f,0,SEEK_END));long bytes=ftell(f);
        assert(bytes>0 && !fseek(f,0,SEEK_SET));uint8_t *data=malloc((size_t)bytes);assert(data);
        assert(fread(data,1,(size_t)bytes,f)==(size_t)bytes);fclose(f);
        assert(tg_gpu_gr_netlist_parse((struct tg_gpu_gr_blob){data,(size_t)bytes},&net)==TG_GPU_GR_FW_OK);
        reset(&m);assert(run(&m,&net,&s)==TG_GPU_GR_CTX_OK);free(data);
        puts("External NETB graphics initialization streams: PASS (model only)");
    }
    free(m.regs);free(m.inst);free(m.work);
    puts("PASS: GR binding/golden-save order, owned mappings, cache visibility and failure rejection (model only)");
    return 0;
}
