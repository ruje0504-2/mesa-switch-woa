/* SPDX-License-Identifier: MIT */
#include "../core/execute.h"
#include <assert.h>
#include <string.h>

struct fifo_model {
    uint32_t mc, instance, asid[2], pb, runlist_map, put;
    uint32_t sched, runlist_base, stall;
    unsigned writes, channel_reads, channel_writes, asid_writes, ticks, enabled_at;
    bool fail_enable, unreadable_channel, fail_pb_enable, allow_mmu, runlist;
    bool fail_sched_allow, hce_fault;
};
static uint32_t read32(void *context, uint32_t address)
{
    struct fifo_model *m = context;
    if (address == 0x57000200) return m->mc;
    if (address == 0x57000204) return m->pb;
    if (address == 0x57002390) return m->runlist_map;
    if (address == 0x57002630) return m->sched;
    if (address == 0x57002280) return m->runlist_base;
    if (address == 0x57002284) return m->runlist ? 1 : 0;
    if (address == 0x5704013c) return m->stall;
    if (address == 0x57040148 && m->hce_fault && m->put) return 8;
    if (address == 0x57100c80 && m->allow_mmu) return 0x18000;
    if (address == 0x58013000 && m->allow_mmu) return 0x55aa3377;
    if (address == 0x5801308c) return m->put;
    if (address == 0x57800000 || address == 0x57800004) {
        /* Reading a reset FIFO cannot establish channel ownership. */
        assert((m->mc & 0x100) && m->ticks - m->enabled_at >= 20);
        ++m->channel_reads;
        return m->unreadable_channel ? UINT32_MAX :
            address == 0x57800000 ? m->instance : 0;
    }
    if (address == 0x70019aa8) return m->asid[0];
    if (address == 0x70019aac) return m->asid[1];
    /* No fake GPU completion: the MMU or first semaphore wait times out. */
    return 0;
}
static void write32(void *context, uint32_t address, uint32_t value)
{
    struct fifo_model *m = context;
    ++m->writes;
    if (address == 0x57000200) {
        if (value & 0x100) {
            assert(m->ticks >= 20);
            m->enabled_at = m->ticks;
        }
        m->mc = m->fail_enable ? value & ~0x100u : value;
    } else if (address == 0x57000204) {
        assert(m->mc & 0x100);
        if (!(value & 1) && (m->pb & 1)) assert(!m->runlist && !m->instance);
        m->pb = m->fail_pb_enable ? value & ~1u : value;
    } else if (address == 0x57002274) {
        assert(m->sched & 1);
        if (value) assert((m->pb & 1) && m->instance);
        m->runlist = value != 0;
    } else if (address == 0x57002270) {
        assert(m->sched & 1);
        m->runlist_base = value;
    } else if (address == 0x57002630) {
        if (!(value & 1)) assert(m->runlist && m->instance);
        m->sched = m->fail_sched_allow ? value | 1 : value;
    } else if (address == 0x5704013c) {
        m->stall = value;
    } else if (address == 0x5801308c) {
        assert((m->pb & 1) && m->runlist && !(m->sched & 1));
        assert(!(m->stall & 0x10000100));
        m->put = value;
    } else if (address == 0x57800000 || address == 0x57800004) {
        assert((m->mc & 0x100) && m->ticks - m->enabled_at >= 20);
        ++m->channel_writes;
        if (address == 0x57800000) m->instance = value;
    } else if (address == 0x70019aa8 || address == 0x70019aac) {
        /* Reproduce the failed assumption: an ASID read need not become zero.
         * Direct physical GPU DMA must leave this global state untouched.
         */
        ++m->asid_writes;
    }
}
static void delay(void *context, uint32_t us)
{ ((struct fifo_model *)context)->ticks += us; }

void test_execute_init(void)
{
    static _Alignas(4096) uint8_t arena[TG_GPU_ARENA_BYTES];
    struct fifo_model m = {.instance=0x80000400, .asid={0x80000011,0x80000012}, .pb=2, .runlist_map=1};
    struct tg_gpu_power_io io = {&m,read32,write32,NULL,NULL,delay};
    struct tg_gpu_power power = {0};
    struct tg_gpu_execution state = {0};
    struct tg_gpu_status result = {.expected={0x13579bdf,0x2468ace0}};
    assert(tg_gpu_execute(&io,&power,arena,0xc0000000,&state,&result) == TG_GPU_EXEC_ARGUMENT);
    assert(m.writes == 0 && m.channel_reads == 0);
    power.owned = power.ready = true;
    assert(tg_gpu_execute(&io,&power,arena,0xc0000000,&state,&result) == TG_GPU_EXEC_MMU_TIMEOUT);
    assert(state.stage == 2 && state.owned && state.fifo_ready);
    assert(result.mc_enable_before == 0 && result.channel_instance_before == UINT32_MAX);
    assert(result.mc_enable_after == 0x100 && result.channel_instance_after == 0);
    assert(result.pbdma_enable_before == 2 && result.pbdma_enable_after == 3);
    assert(result.pbdma_runlist_map == 1 && state.pbdma_owned);
    assert(result.observed[0] == 0xdeadbeef && result.observed[1] == 0xdeadbeef);
    tg_gpu_execute_stop(&io,&state);
    assert(!(m.mc & 0x100) && !state.fifo_ready);
    assert(m.pb == 2 && !state.pbdma_owned);
    assert(m.asid_writes == 0 && m.asid[0] == 0x80000011 && m.asid[1] == 0x80000012);

    memset(&m,0,sizeof(m)); memset(&state,0,sizeof(state));
    /* Never admit an address carrying GM20B's SMMU translation tag. */
    assert(tg_gpu_execute(&io,&power,arena,UINT64_C(1)<<34,&state,&result) == TG_GPU_EXEC_ARGUMENT);
    assert(m.writes == 0 && m.channel_reads == 0 && !state.owned);

    memset(&m,0,sizeof(m)); memset(&state,0,sizeof(state)); m.fail_enable=true;
    assert(tg_gpu_execute(&io,&power,arena,0xc0000000,&state,&result) == TG_GPU_EXEC_FIFO_INIT);
    assert(state.stage == 1 && !state.fifo_ready && m.channel_reads == 0);
    tg_gpu_execute_stop(&io,&state);
    assert(m.channel_writes == 0);

    memset(&m,0,sizeof(m)); memset(&state,0,sizeof(state)); m.unreadable_channel=true;
    assert(tg_gpu_execute(&io,&power,arena,0xc0000000,&state,&result) == TG_GPU_EXEC_FIFO_INIT);
    assert(result.channel_instance_after == UINT32_MAX && m.channel_writes == 0);
    tg_gpu_execute_stop(&io,&state);
    assert(m.channel_writes == 0);

    memset(&m,0,sizeof(m)); memset(&state,0,sizeof(state));
    m.runlist_map=1; m.fail_pb_enable=true;
    assert(tg_gpu_execute(&io,&power,arena,0xc0000000,&state,&result) == TG_GPU_EXEC_PBDMA_INIT);
    assert(state.stage == 1 && result.pbdma_enable_after == 0 && !m.put);
    tg_gpu_execute_stop(&io,&state);

    memset(&m,0,sizeof(m)); memset(&state,0,sizeof(state)); m.allow_mmu=true;
    assert(tg_gpu_execute(&io,&power,arena,0xc0000000,&state,&result) == TG_GPU_EXEC_PBDMA_INIT);
    assert(!state.pbdma_owned && !m.put); /* no PBDMA serves runlist zero */
    tg_gpu_execute_stop(&io,&state);

    memset(&m,0,sizeof(m)); memset(&state,0,sizeof(state));
    m.runlist_map=1; m.allow_mmu=true; m.sched=0xa5; m.stall=UINT32_MAX;
    assert(tg_gpu_execute(&io,&power,arena,0xc0000000,&state,&result) == TG_GPU_EXEC_TIMEOUT);
    assert(state.stage == 4 && result.userd_gp_put == 1 && result.userd_gp_get == 0);
    assert(result.observed[0] == 0xdeadbeef && result.observed[1] == 0xdeadbeef);
    assert(result.scheduler_before == 0xa5 && result.scheduler_after == 0xa4);
    assert(result.runlist_status == 1 && result.runlist_base == 0x300c0045);
    assert(m.stall == 0xeffffeff && result.pbdma_interrupt_1 == 0);
    tg_gpu_execute_stop(&io,&state);
    assert(!m.pb && !m.runlist && !m.instance && !state.pbdma_owned);
    assert(m.sched == 0xa5);

    memset(&m,0,sizeof(m)); memset(&state,0,sizeof(state));
    m.runlist_map=1; m.allow_mmu=true; m.fail_sched_allow=true;
    assert(tg_gpu_execute(&io,&power,arena,0xc0000000,&state,&result) == TG_GPU_EXEC_SCHEDULER);
    assert(state.stage == 3 && !m.put && (result.scheduler_after & 1));
    tg_gpu_execute_stop(&io,&state);
    assert(!m.pb && !m.runlist && !m.instance);

    memset(&m,0,sizeof(m)); memset(&state,0,sizeof(state));
    m.runlist_map=1; m.allow_mmu=true; m.hce_fault=true;
    assert(tg_gpu_execute(&io,&power,arena,0xc0000000,&state,&result) == TG_GPU_EXEC_FAULT);
    assert(state.stage == 4 && result.pbdma_interrupt_1 == 8);
    assert(result.observed[0] == 0xdeadbeef && result.observed[1] == 0xdeadbeef);
    tg_gpu_execute_stop(&io,&state);
}
