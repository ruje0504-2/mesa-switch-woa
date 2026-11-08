/* SPDX-License-Identifier: MIT */
#include "../core/execute.h"
#include <assert.h>
#include <string.h>

struct fifo_model {
    uint32_t mc, instance, asid[2];
    unsigned writes, channel_reads, channel_writes, ticks, enabled_at;
    bool fail_enable, unreadable_channel;
};
static uint32_t read32(void *context, uint32_t address)
{
    struct fifo_model *m = context;
    if (address == 0x57000200) return m->mc;
    if (address == 0x57800000 || address == 0x57800004) {
        /* Reading a reset FIFO cannot establish channel ownership. */
        assert((m->mc & 0x100) && m->ticks - m->enabled_at >= 20);
        ++m->channel_reads;
        return m->unreadable_channel ? UINT32_MAX :
            address == 0x57800000 ? m->instance : 0;
    }
    if (address == 0x70019aa8) return m->asid[0];
    if (address == 0x70019aac) return m->asid[1];
    /* No fake completion: the model deliberately stops at the MMU wait. */
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
    } else if (address == 0x57800000 || address == 0x57800004) {
        assert((m->mc & 0x100) && m->ticks - m->enabled_at >= 20);
        ++m->channel_writes;
        if (address == 0x57800000) m->instance = value;
    } else if (address == 0x70019aa8) m->asid[0] = value;
    else if (address == 0x70019aac) m->asid[1] = value;
}
static void delay(void *context, uint32_t us)
{ ((struct fifo_model *)context)->ticks += us; }

void test_execute_init(void)
{
    static _Alignas(4096) uint8_t arena[TG_GPU_ARENA_BYTES];
    struct fifo_model m = {.instance=0x80000400, .asid={0x80000011,0x80000012}};
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
    assert(result.observed[0] == 0xdeadbeef && result.observed[1] == 0xdeadbeef);
    tg_gpu_execute_stop(&io,&state);
    assert(!(m.mc & 0x100) && !state.fifo_ready);
    assert(tg_gpu_execute_release_domain(&io,&state));
    assert(m.asid[0] == 0x80000011 && m.asid[1] == 0x80000012 && !state.owned);

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
}
