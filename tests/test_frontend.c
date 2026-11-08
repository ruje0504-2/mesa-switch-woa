/* SPDX-License-Identifier: MIT */
#include "../platform/t214_frontend.h"
#include "../platform/t214_power.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t regs[0x180000 / 4];
static unsigned writes, flushes, busy_reads, fail_flush, secure_writes, irq_masked;
static uint32_t fault_on;
static uint32_t read32(void *ctx, uint32_t address)
{
    uint32_t reg = address - 0x57000000;
    (void)ctx;
    assert(address >= 0x57000000 && reg < sizeof(regs));
    if (reg == 0x70000) {
        if (fail_flush == flushes && flushes) return 3;
        if (busy_reads) { --busy_reads; return 3; }
        return 0;
    }
    return regs[reg / 4];
}
static void write32(void *ctx, uint32_t address, uint32_t value)
{
    uint32_t reg = address - 0x57000000;
    (void)ctx;
    assert(address >= 0x57000000 && reg < sizeof(regs));
    assert(reg != 0x100d1c && reg != 0x21434);
    ++writes;
    if (reg == 0x140 || reg == 0x144) { assert(value == 0); ++irq_masked; }
    if (reg == 0x1140) assert(irq_masked == 2 && value == 0xe);
    if (reg == 0x1100) { regs[reg / 4] &= ~value; return; }
    if (reg == 0x70000) {
        assert(value == 1 && regs[0x100c10 / 4] && (regs[0x1704 / 4] & 0x80000000));
        ++flushes; busy_reads = 3;
    }
    if (reg == 0x100ce4) { assert(!regs[0x21434 / 4]); ++secure_writes; }
    regs[reg / 4] = value;
    if (reg == fault_on) {
        regs[0x120058 / 4] = 0x100;
        regs[0x122120 / 4] = address;
        regs[0x12212c / 4] = 0xbad;
    }
}
static void delay(void *ctx, uint32_t us) { (void)ctx; assert(us <= 20); }
static void reset(void)
{
    memset(regs, 0, sizeof(regs));
    writes = flushes = busy_reads = fail_flush = secure_writes = irq_masked = 0;
    fault_on = UINT32_MAX;
    regs[0x22454 / 4] = regs[0x12006c / 4] = 1;
    regs[0x21434 / 4] = 1;
    regs[0x140518 / 4] = 0xa0;
    regs[0x17e20c / 4] = 0x40300008;
    regs[0x2a04 / 4] = 0x80000000;
}
int main(void)
{
    struct tg_gpu_power_io io = {NULL,read32,write32,NULL,NULL,delay};
    struct tg_gpu_power power = {0};
    struct tg_gpu_frontend_status s = {0};
    reset();
    assert(tg_gpu_frontend_start(&io, &power, &s) == TG_GPU_FE_ARGUMENT && !writes);
    power.owned = power.ready = true;
    assert(tg_gpu_frontend_start(&io, &power, &s) == TG_GPU_FE_OK);
    assert(tg_gpu_frontend_fifo(&io, &power, &s) == TG_GPU_FE_OK);
    assert(regs[0x2a04 / 4] == 0xbfffffff && regs[0x4012c / 4] == UINT32_MAX);
    assert(tg_gpu_frontend_memory(&io, &power, &s) == TG_GPU_FE_OK);
    assert(s.stage == 4 && s.ltc_count == 1 && !secure_writes);
    assert(regs[0x17e27c / 4] == 1 && regs[0x17e000 / 4] == 1 && regs[0x100800 / 4] == 1);
    assert(regs[0x17e318 / 4] == 0x80a0 && regs[0x17e20c / 4] == 8);
    regs[0x100c10 / 4] = 0xc000046; regs[0x1704 / 4] = 0xb00c0040;
    assert(tg_gpu_frontend_flush(&io, &power, &s) == TG_GPU_FE_OK);
    assert(s.stage == 6 && s.fb_flushes == 2 && flushes == 2);

    reset(); memset(&s,0,sizeof(s)); regs[0x21434 / 4] = 0;
    assert(tg_gpu_frontend_memory(&io, &power, &s) == TG_GPU_FE_OK && secure_writes == 1);
    reset(); memset(&s,0,sizeof(s)); regs[0x12006c / 4] = 0;
    assert(tg_gpu_frontend_memory(&io, &power, &s) == TG_GPU_FE_LTC && !writes);
    reset(); memset(&s,0,sizeof(s)); regs[0x21434 / 4] = UINT32_MAX;
    assert(tg_gpu_frontend_memory(&io, &power, &s) == TG_GPU_FE_REGISTER && !secure_writes);
    reset(); memset(&s,0,sizeof(s)); fault_on = 0x17e000;
    assert(tg_gpu_frontend_memory(&io, &power, &s) == TG_GPU_FE_PRI_FAULT);
    assert(s.stage == 3 && s.pri_error_address == 0x5717e000 && s.pri_error_code == 0xbad);
    assert(!regs[0x20c / 4]); /* fail before memory-client release */
    reset(); memset(&s,0,sizeof(s));
    regs[0x100c10 / 4] = 0xc000046; regs[0x1704 / 4] = 0xb00c0040; fail_flush=2;
    assert(tg_gpu_frontend_flush(&io, &power, &s) == TG_GPU_FE_FLUSH_TIMEOUT);
    assert(s.stage == 5 && s.fb_flushes == 1 && flushes == 2);
    puts("PASS: cold frontend routing, secure policy, bounded FB flush and PRI faults (model only)");
    return 0;
}
