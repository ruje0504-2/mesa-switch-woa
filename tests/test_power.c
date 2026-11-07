/* SPDX-License-Identifier: GPL-2.0-only */
#include "../platform/t214_power.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct model {
    uint32_t hidrev, clocks, reset, out5, clamp, flush;
    uint8_t enable, voltage;
    unsigned int writes;
    bool fail_voltage_once, fail_mc, fail_reset;
};

static uint32_t read32(void *opaque, uint32_t address)
{
    struct model *m = opaque;
    switch (address) {
    case 0x70000804: return m->hidrev;
    case 0x600060a0: return 0x48000000; /* PLLP enabled, locked, not bypassed */
    case 0x60006280: return m->clocks;
    case 0x6000628c: return m->reset;
    case 0x6000667c: return m->out5;
    case 0x7000e6d4: return m->clamp;
    case 0x70019970: return m->flush;
    case 0x70019974: return m->fail_mc ? 0 : m->flush;
    default: assert(!"unexpected register read"); return 0;
    }
}

static void write32(void *opaque, uint32_t address, uint32_t value)
{
    struct model *m = opaque;
    ++m->writes;
    switch (address) {
    case 0x60006284: m->clocks |= value; break;
    case 0x60006288: m->clocks &= ~value; break;
    case 0x60006290: m->reset |= value; break;
    case 0x60006294:
        assert(!(m->clocks & (1u << 24))); /* upstream gate/reset ordering */
        if (!m->fail_reset) m->reset &= ~value;
        break;
    case 0x6000667c: m->out5 = value; break;
    case 0x7000e6d4:
        if (!(value & 1)) {
            assert(m->enable & 1);
            assert(m->voltage == 110);
            assert((m->clocks & 0x21000000) == 0x21000000);
            assert(m->reset & (1u << 24));
        }
        m->clamp = value;
        break;
    case 0x70019970: m->flush = value; break;
    default: assert(!"unexpected register write");
    }
}

static bool read_pmic(void *opaque, uint8_t address, uint8_t reg, uint8_t *value)
{
    struct model *m = opaque;
    assert(address == 0x33);
    if (reg == 0x06) *value = m->enable;
    else if (reg == 0x23) *value = m->voltage;
    else assert(!"unexpected PMIC register");
    return true;
}

static bool write_pmic(void *opaque, uint8_t address, uint8_t reg, uint8_t value)
{
    struct model *m = opaque;
    assert(address == 0x33);
    ++m->writes;
    if (reg == 0x06) {
        assert((value & ~1u) == (m->enable & ~1u)); /* CPU/DDR unchanged */
        m->enable = value;
    } else if (reg == 0x23) {
        if (m->fail_voltage_once) { m->fail_voltage_once = false; return false; }
        m->voltage = value;
    } else assert(!"unexpected PMIC write");
    return true;
}

static void delay_us(void *opaque, uint32_t us)
{
    (void)opaque;
    assert(us == 10 || us == 5000);
}

int main(void)
{
    const struct model initial = {
        .hidrev = 0x00012127, .clocks = 0x40, .reset = 0x01000000,
        .out5 = 0x00fc1234, .clamp = 0x81, .flush = 0x80,
        .enable = 0x50, .voltage = 70
    };
    struct model model = initial;
    struct tg_gpu_power state = {0};
    struct tg_gpu_power_io io = { &model, read32, write32, read_pmic, write_pmic, delay_us };

    assert(tg_gpu_power_up(&io, 0x33, &state) == TG_GPU_POWER_OK);
    assert(state.ready && state.owned && state.stage == TG_GPU_STAGE_READY);
    assert(model.enable == 0x51 && model.clamp == 0x80 && model.reset == 0);
    assert(tg_gpu_power_down(&io, &state) == TG_GPU_POWER_OK);
    assert(!state.owned && !state.ready);
    assert(model.enable == initial.enable && model.voltage == initial.voltage);
    assert(model.clocks == initial.clocks && model.out5 == initial.out5);
    assert(model.clamp == initial.clamp && model.reset == initial.reset);
    assert(model.flush == initial.flush);

    model = initial;
    memset(&state, 0, sizeof(state));
    model.reset = 0; /* an already owned GPU must not be reset */
    assert(tg_gpu_power_up(&io, 0x33, &state) == TG_GPU_POWER_BUSY);
    assert(model.writes == 0 && !state.owned);

    model = initial;
    model.fail_voltage_once = true;
    assert(tg_gpu_power_up(&io, 0x33, &state) == TG_GPU_POWER_PMIC_ERROR);
    assert(state.owned && !state.ready);
    assert(tg_gpu_power_down(&io, &state) == TG_GPU_POWER_OK);
    assert(model.enable == initial.enable && model.voltage == initial.voltage);

    model = initial;
    model.fail_reset = true;
    assert(tg_gpu_power_up(&io, 0x33, &state) == TG_GPU_POWER_RESET_ERROR);
    assert(!state.ready);
    assert(tg_gpu_power_down(&io, &state) == TG_GPU_POWER_OK);

    model = initial;
    model.fail_mc = true;
    assert(tg_gpu_power_up(&io, 0x33, &state) == TG_GPU_POWER_MC_TIMEOUT);
    assert(state.owned && !state.ready);
    assert(tg_gpu_power_down(&io, &state) == TG_GPU_POWER_MC_TIMEOUT);
    assert(state.owned); /* no claim that potentially live memory can be freed */
    model.fail_mc = false;
    assert(tg_gpu_power_down(&io, &state) == TG_GPU_POWER_OK);
    puts("T214 power ordering and unwind tests passed (model, not hardware).");
    return 0;
}
