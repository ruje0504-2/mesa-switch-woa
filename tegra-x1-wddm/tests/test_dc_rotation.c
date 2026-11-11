/* SPDX-License-Identifier: MIT */
#include "../platform/t214_dc.h"
#include "../platform/t214_resources.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct display_model {
    uint32_t assembly[T214_GPU_DC_SIZE / 4];
    uint32_t armed[T214_GPU_DC_SIZE / 4];
    uint32_t active[T214_GPU_DC_SIZE / 4];
    uint32_t update;
    unsigned writes;
};

static uint32_t read_reg(void *context, uint32_t word)
{
    const struct display_model *m = context;
    assert(word < T214_GPU_DC_SIZE / 4);
    if ((m->assembly[T214_DC_STATE_ACCESS] & T214_DC_READ_ACTIVE) &&
        word >= 0xb80 && word <= 0xbcd)
        return m->active[word];
    return m->assembly[word];
}

static void write_reg(void *context, uint32_t word, uint32_t value)
{
    struct display_model *m = context;
    ++m->writes;
    switch (word) {
    case T214_DC_STATE_CONTROL:
        if (value & 0x300u) {
            assert(!(value & ~0x300u));
            m->update = value >> 8;
            memcpy(m->armed, m->assembly, sizeof(m->armed));
        } else {
            assert(value && value == m->update);
            m->assembly[word] |= value;
            m->update = 0;
        }
        return;
    case T214_DC_INT_STATUS:
        assert(value == T214_DC_VBLANK_INT);
        m->assembly[word] &= ~value;
        return;
    case T214_DC_STATE_ACCESS: case T214_DC_REG_ACT_CONTROL:
    case T214_DC_INT_MASK: case T214_DC_INT_ENABLE:
    case T214_DC_WIN_A_OPTIONS: case T214_DC_WIN_A_STRIDE:
    case T214_DC_WIN_A_H_OFFSET: case T214_DC_WIN_A_V_OFFSET:
    case T214_DC_WIN_A_ADDR:
        m->assembly[word] = value;
        return;
    default:
        /* Rotation must not change the inherited DSI timing or pixel format. */
        assert(!"unexpected display register write");
    }
}

static void latch(struct display_model *m, bool vblank)
{
    if ((vblank || (m->assembly[T214_DC_REG_ACT_CONTROL] & T214_DC_WIN_A_HCOUNTER)) &&
        (m->assembly[T214_DC_STATE_CONTROL] & T214_DC_WIN_A_ACT_REQ)) {
        memcpy(&m->active[0xb80], &m->armed[0xb80], (0xbce - 0xb80) * sizeof(uint32_t));
        m->assembly[T214_DC_STATE_CONTROL] &= ~T214_DC_WIN_A_ACT_REQ;
    }
    if (vblank) m->assembly[T214_DC_STATE_CONTROL] = 0;
}

static void initialize(struct display_model *m)
{
    memset(m, 0, sizeof(*m));
    uint32_t *r = m->assembly;
    r[T214_DC_COMMAND] = T214_DC_MODE_CONTINUOUS;
    r[T214_DC_OUTPUT_OPTIONS] = T214_DC_DSI_ENABLE;
    r[T214_DC_ACTIVE_SIZE] = T214_DC_PANEL_ACTIVE;
    r[T214_DC_SYNC_WIDTH] = T214_DC_PANEL_SYNC;
    r[T214_DC_BACK_PORCH] = T214_DC_PANEL_BACK;
    r[T214_DC_FRONT_PORCH] = T214_DC_PANEL_FRONT;
    r[T214_DC_WIN_A_OPTIONS] = T214_DC_WIN_ENABLE;
    r[T214_DC_WIN_A_COLOR] = T214_DC_COLOR_BGRA8888;
    r[T214_DC_WIN_A_SIZE] = T214_DC_PANEL_ACTIVE;
    r[T214_DC_WIN_A_PRESCALED] = T214_DC_PANEL_PRESCALED;
    r[T214_DC_WIN_A_DDA] = T214_DC_DDA_IDENTITY;
    r[T214_DC_WIN_A_STRIDE] = 720 * 4;
    r[T214_DC_WIN_A_ADDR] = T214_GPU_POST_FB_BASE;
    r[T214_DC_REG_ACT_CONTROL] = 0x454;
    memcpy(m->active, r, sizeof(m->active));
}

static void check_pixels(const struct display_model *m, unsigned rotation)
{
    const uint32_t *r = m->active;
    const uint32_t options = r[T214_DC_WIN_A_OPTIONS];
    const bool column = (options & 0x10) != 0;
    const int dx = options & 1 ? -4 : 4;
    const int dy = options & 4 ? -1 : 1;
    const unsigned width = rotation & 1 ? 1280 : 720;
    /* Simulate byte addressing from latched registers. The independent oracle
     * rotates pixel coordinates, so a wrong axis/origin/pitch changes pixels. */
    for (unsigned y = 0; y < 1280; ++y) {
        for (unsigned x = 0; x < 720; ++x) {
            const int h = (int)r[T214_DC_WIN_A_H_OFFSET] + dx * (int)(column ? y : x);
            const int v = (int)r[T214_DC_WIN_A_V_OFFSET] + dy * (int)(column ? x : y);
            assert(h >= 0 && v >= 0);
            const uint64_t byte = (uint64_t)v * r[T214_DC_WIN_A_STRIDE] + ((unsigned)h & ~3u);
            assert(byte + 4 <= T214_GPU_POST_FB_SIZE);
            unsigned sx = x, sy = y;
            if (rotation == 1) { sx = 1279 - y; sy = x; }
            if (rotation == 2) { sx = 719 - x; sy = 1279 - y; }
            if (rotation == 3) { sx = y; sy = 719 - x; }
            assert(byte == ((uint64_t)sy * width + sx) * 4);
        }
    }
}

int main(void)
{
    static struct display_model m;
    initialize(&m);
    struct tg_t214_dc dc = {0};
    const struct tg_t214_dc_io io = {&m, read_reg, write_reg};
    assert(tg_t214_dc_acquire(&dc, &io) == TG_T214_DC_OK);
    latch(&m, true);
    struct tg_t214_dc_scanout active;
    assert(tg_t214_dc_latched_surface(&dc, &active) && active.rotation == TG_T214_DC_IDENTITY);
    check_pixels(&m, 0);

    const uint32_t options[] = {0, 0x11, 0x5, 0x14};
    const uint32_t h_offsets[] = {0, 5119, 2879, 0};
    const uint32_t v_offsets[] = {0, 0, 1279, 719};
    for (unsigned r = 1; r < 4; ++r) {
        struct tg_t214_dc_layout layout;
        assert(tg_t214_dc_get_layout((enum tg_t214_dc_rotation)r, &layout));
        assert(layout.options == options[r] && layout.h_offset == h_offsets[r] &&
            layout.v_offset == v_offsets[r] && layout.pitch == layout.width * 4 &&
            (uint64_t)layout.pitch * layout.height == T214_GPU_POST_FB_SIZE);
        struct tg_t214_dc_scanout next = {0xc1000000 + r * 0x400000,
            (enum tg_t214_dc_rotation)r, true};
        assert(tg_t214_dc_queue_surface(&dc, &next, true) == TG_T214_DC_OK);
        assert(!(m.assembly[T214_DC_REG_ACT_CONTROL] & T214_DC_WIN_A_HCOUNTER));
        assert(!tg_t214_dc_latched_surface(&dc, &active));
        latch(&m, false); /* A line boundary cannot complete a layout change. */
        assert(!tg_t214_dc_latched_surface(&dc, &active));
        latch(&m, true);
        assert(tg_t214_dc_latched_surface(&dc, &active) && active.address == next.address &&
            active.rotation == next.rotation && active.visible);
        check_pixels(&m, r);

        next.address += 0x400000;
        assert(tg_t214_dc_queue_surface(&dc, &next, true) == TG_T214_DC_OK);
        assert(m.assembly[T214_DC_REG_ACT_CONTROL] & T214_DC_WIN_A_HCOUNTER);
        latch(&m, false); /* An address-only update can latch at HBlank. */
        assert(tg_t214_dc_latched_surface(&dc, &active) && active.address == next.address &&
            active.rotation == next.rotation);
        latch(&m, true);

        m.active[T214_DC_WIN_A_STRIDE] ^= 64;
        assert(!tg_t214_dc_latched_surface(&dc, &active));
        m.active[T214_DC_WIN_A_STRIDE] ^= 64;
        m.active[T214_DC_WIN_A_H_OFFSET] ^= 4;
        assert(!tg_t214_dc_latched_surface(&dc, &active));
        m.active[T214_DC_WIN_A_H_OFFSET] ^= 4;
        assert(m.assembly[T214_DC_STATE_ACCESS] == 0);
    }

    const unsigned writes = m.writes;
    struct tg_t214_dc_scanout bad = {0xc1000000, (enum tg_t214_dc_rotation)4, true};
    assert(tg_t214_dc_queue_surface(&dc, &bad, false) == TG_T214_DC_INVALID && m.writes == writes);
    bad.rotation = TG_T214_DC_CCW90; bad.address = 0xfff00000;
    assert(tg_t214_dc_queue_surface(&dc, &bad, false) == TG_T214_DC_INVALID && m.writes == writes);
    bad.address = 0xc1000004;
    assert(tg_t214_dc_queue_surface(&dc, &bad, false) == TG_T214_DC_INVALID && m.writes == writes);

    struct tg_t214_dc_scanout post = {T214_GPU_POST_FB_BASE, TG_T214_DC_CCW90, true};
    assert(tg_t214_dc_queue_surface(&dc, &post, false) == TG_T214_DC_OK);
    latch(&m, true);
    assert(tg_t214_dc_release(&dc) == TG_T214_DC_BUSY); /* Correct address, wrong fetch layout. */
    assert(tg_t214_dc_queue_scanout(&dc, T214_GPU_POST_FB_BASE, true) == TG_T214_DC_OK);
    assert(tg_t214_dc_release(&dc) == TG_T214_DC_BUSY);
    latch(&m, true);
    assert(tg_t214_dc_latched_surface(&dc, &active) && active.rotation == TG_T214_DC_IDENTITY);
    check_pixels(&m, 0);
    assert(tg_t214_dc_release(&dc) == TG_T214_DC_BUSY); /* Restore the original counter mode. */
    latch(&m, true);
    assert(tg_t214_dc_release(&dc) == TG_T214_DC_OK && !dc.owned);
    puts("T214 hardware fetch rotation, full-frame pixel addressing, atomic latch and portrait restore: PASS (model only)");
    return 0;
}
