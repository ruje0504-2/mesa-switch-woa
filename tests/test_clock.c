/* SPDX-License-Identifier: MIT */
#include "../platform/t214_clock.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t registers[0x140000 / 4];
static bool counter_bad, calibration_timeout;
static uint32_t adc_fuse;
static uint32_t writes;
static uint32_t read_reg(void *ctx, uint32_t address)
{
    (void)ctx;
    if (address == 0x7000f930) return 1400;
    if (address == 0x7000fb04) return adc_fuse;
    assert(address >= 0x57000000 && address < 0x57140000);
    return registers[(address - 0x57000000) / 4];
}
static void write_reg(void *ctx, uint32_t address, uint32_t value)
{
    (void)ctx;
    assert(address >= 0x57000000 && address < 0x57140000);
    ++writes;
    registers[(address - 0x57000000) / 4] = value;
    if (address == 0x57137000 && (value & 1) && !calibration_timeout) {
        registers[0x137014 / 4] |= 1u << 31;
        registers[0x137018 / 4] |= 20u << 24;
    }
    if (address == 0x57134124 && value == 0x00110320) {
        /* Counter observes GPCCLK (half the PLL), not GPC2CLK. */
        assert((registers[0x137004 / 4] & 0x3fffff) == 0x12601);
        assert((registers[0x13700c / 4] & 0xff) == 0xf0);
        assert((registers[0x137014 / 4] & 0x2fff0000) == 0x2e010000);
        assert((registers[0x132820 / 4] & 0x7f) == 20);
        registers[0x134128 / 4] = counter_bad ? 3200 : 15200;
    }
}
static bool read_pmic(void *ctx, uint8_t address, uint8_t reg, uint8_t *value)
{
    (void)ctx;
    assert(address == 0x33);
    *value = reg == 0x23 ? 110 : 0x41;
    return true;
}
static void delay(void *ctx, uint32_t us) { (void)ctx; (void)us; }
static void reset(void)
{
    memset(registers, 0, sizeof(registers));
    registers[0] = 0x12e000b1;
    registers[0x137000 / 4] = 2;
    registers[0x20798 / 4] = 0x105;
    registers[0x20160 / 4] = 0x00220000;
    adc_fuse = 0;
    counter_bad = calibration_timeout = false;
    writes = 0;
}
int main(void)
{
    struct tg_gpu_power p = {.owned = true, .ready = true, .pmic_address = 0x33};
    struct tg_gpu_power_io io = {NULL, read_reg, write_reg, read_pmic, NULL, delay};
    struct tg_gpu_clock c = {0};
    assert(tg_gpu_clock_required_uv(0) == UINT32_MAX);
    assert(tg_gpu_clock_required_uv(5001) == UINT32_MAX);
    assert(tg_gpu_clock_required_uv(1400) == 800000);
    assert(tg_gpu_clock_required_uv(100) > 800000);
    reset();
    assert(tg_gpu_clock_start(&io, &p, &c) == TG_GPU_CLOCK_OK);
    assert(c.measured_khz == TG_GPU_FIXED_KHZ && c.counter == 15200);
    assert(c.external_calibration == 20 && c.stage == 5);
    assert(registers[0x20798 / 4] == 0x105);
    assert(registers[0x20160 / 4] == 0x00220000);
    assert(tg_gpu_clock_stop(&io, &c) == TG_GPU_CLOCK_OK);
    assert(!c.owned && !c.running && registers[0x137000 / 4] == 2);
    assert((registers[0x132820 / 4] & 0x1007f) == 0);
    reset(); memset(&c, 0, sizeof(c));
    /* Fused slope 10mV, intercept 600mV gives the same 800mV calibration. */
    adc_fuse = (1u << 30) | (10u << 24) | (600u << 4);
    assert(tg_gpu_clock_start(&io, &p, &c) == TG_GPU_CLOCK_OK);
    assert(tg_gpu_clock_stop(&io, &c) == TG_GPU_CLOCK_OK);
    reset(); memset(&c, 0, sizeof(c)); counter_bad = true;
    assert(tg_gpu_clock_start(&io, &p, &c) == TG_GPU_CLOCK_COUNTER_ERROR);
    assert(c.owned);
    assert(tg_gpu_clock_stop(&io, &c) == TG_GPU_CLOCK_OK);
    reset(); memset(&c, 0, sizeof(c)); calibration_timeout = true;
    assert(tg_gpu_clock_start(&io, &p, &c) == TG_GPU_CLOCK_CALIBRATION_ERROR);
    assert(tg_gpu_clock_stop(&io, &c) == TG_GPU_CLOCK_OK);
    reset(); memset(&c, 0, sizeof(c)); registers[0x137000 / 4] = 1;
    assert(tg_gpu_clock_start(&io, &p, &c) == TG_GPU_CLOCK_BAD_STATE);
    assert(!writes && !c.owned);
    puts("T214 C1 NA clock ordering, counter and unwind tests passed (host model).");
    return 0;
}
