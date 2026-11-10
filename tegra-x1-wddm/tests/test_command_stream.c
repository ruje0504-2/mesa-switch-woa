/* SPDX-License-Identifier: MIT */
#include "../core/command_stream.h"
#include "../core/nvk_mme_programs.h"
#include "nouveau/headers/drf.h"
#include "nouveau/headers/nvidia/classes/clb06f.h"
#include "nouveau/headers/nvidia/classes/clb0b5.h"
#include "nouveau/headers/nvidia/classes/clb197.h"
#include "nouveau/headers/nvidia/classes/clb1c0.h"
#include "nouveau/vulkan/nvk_mme_slots.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_mme_upload(void)
{
    enum { MME_WORDS = sizeof(tg_gpu_nvk_mme_init) / sizeof(uint32_t) };
    uint32_t stream[2 + MME_WORDS + 1] = {0x20010000, 0xb197};
    memcpy(stream + 2, tg_gpu_nvk_mme_init, sizeof(tg_gpu_nvk_mme_init));
    assert(tg_gpu_validate_command_stream(stream, sizeof(stream)));
    assert(!tg_gpu_validate_command_stream(stream, sizeof(stream) - 2 * sizeof(uint32_t)));

    stream[2 + MME_WORDS] = 0x80000045; /* Immediate write to instruction RAM pointer. */
    assert(!tg_gpu_validate_command_stream(stream, sizeof(stream)));
    stream[2 + MME_WORDS] = 0;

    stream[3] ^= 1; /* First macro slot. */
    assert(!tg_gpu_validate_command_stream(stream, sizeof(stream)));
    stream[3] ^= 1;
    stream[7] ^= 1; /* First instruction. */
    assert(!tg_gpu_validate_command_stream(stream, sizeof(stream)));

    const uint32_t span[] = {0x20060044, 0, 0, 0, 0, 0, 0}; /* 0x110 through 0x124. */
    const uint32_t oneinc[] = {0xa0020044, 0, 0}; /* 0x110 and 0x114. */
    const uint32_t nonincr[] = {0x60010046, 0}; /* Instruction RAM alone. */
    assert(!tg_gpu_validate_command_stream(span, sizeof(span)));
    assert(!tg_gpu_validate_command_stream(oneinc, sizeof(oneinc)));
    assert(!tg_gpu_validate_command_stream(nonincr, sizeof(nonincr)));
}

static bool check_mme_call(uint32_t slot, uint32_t count, uint32_t first, uint32_t second)
{
    uint32_t packet[7] = {
        0xa0000000u | (count << 16) | (NVB197_CALL_MME_MACRO(slot) >> 2),
        first, second,
    };
    assert(count < sizeof(packet) / sizeof(*packet));
    return tg_gpu_validate_command_stream(packet, (count + 1) * sizeof(*packet));
}

static void test_mme_calls(void)
{
    assert(check_mme_call(NVK_MME_DRAW, 5, 0, 3));
    assert(check_mme_call(NVK_MME_DRAW_INDEXED, 6, 0, 3));
    assert(!check_mme_call(NVK_MME_DRAW_INDEXED, 5, 0, 3));
    assert(!check_mme_call(NVK_MME_SET_PRIV_REG, 3, 0, 0));
    assert(!check_mme_call(NVK_MME_COUNT, 1, 0, 0));

    assert(check_mme_call(NVK_MME_CLEAR, 2, 2047u << 10, 1));
    assert(!check_mme_call(NVK_MME_CLEAR, 1, 0x3c, 0));
    assert(!check_mme_call(NVK_MME_CLEAR, 2, 2047u << 10, 2));
    assert(check_mme_call(NVK_MME_BIND_VB, 4, 31, 0));
    assert(!check_mme_call(NVK_MME_BIND_VB, 4, 32, 0));
    assert(check_mme_call(NVK_MME_SET_WRITE_MASK, 2, 8, 0));
    assert(!check_mme_call(NVK_MME_SET_WRITE_MASK, 2, 9, 0));
    assert(check_mme_call(NVK_MME_SET_VIEWPORT_MIN_MAX_Z, 3, 15, 0));
    assert(!check_mme_call(NVK_MME_SET_VIEWPORT_MIN_MAX_Z, 3, 16, 0));

    const uint32_t immediate = 0x80000e00; /* CALL_MME_MACRO(0) requires ONEINCR. */
    const uint32_t data_only[] = {0xa0010e01, 0}; /* CALL_MME_DATA(0). */
    const uint32_t span[] = {0x20020dff, 0, 0}; /* Increment into the call range. */
    assert(!tg_gpu_validate_command_stream(&immediate, sizeof(immediate)));
    assert(!tg_gpu_validate_command_stream(data_only, sizeof(data_only)));
    assert(!tg_gpu_validate_command_stream(span, sizeof(span)));
}

static void test_mme_order(void)
{
    const uint32_t call[] = {0xa0010000u | (NVB197_CALL_MME_MACRO(NVK_MME_SELECT_CB0) >> 2), 0};
    uint32_t stream[(sizeof(tg_gpu_nvk_mme_init) + sizeof(call)) / sizeof(uint32_t)];
    struct tg_gpu_command_info info;

    assert(tg_gpu_inspect_command_stream(call, sizeof(call), NULL, &info));
    assert(info.requires_mme && !info.uploads_mme);

    memcpy(stream, tg_gpu_nvk_mme_init, sizeof(tg_gpu_nvk_mme_init));
    memcpy(stream + sizeof(tg_gpu_nvk_mme_init) / sizeof(*stream), call, sizeof(call));
    assert(tg_gpu_inspect_command_stream(stream, sizeof(stream), NULL, &info));
    assert(!info.requires_mme && info.uploads_mme);

    memcpy(stream, call, sizeof(call));
    memcpy(stream + sizeof(call) / sizeof(*stream), tg_gpu_nvk_mme_init, sizeof(tg_gpu_nvk_mme_init));
    assert(tg_gpu_inspect_command_stream(stream, sizeof(stream), NULL, &info));
    assert(info.requires_mme && info.uploads_mme);
}

#define INCR(subchannel, method, count) (0x20000000u | ((subchannel) << 13) | ((count) << 16) | ((method) >> 2))
#define IMMD(subchannel, method, data) (0x80000000u | ((subchannel) << 13) | ((data) << 16) | ((method) >> 2))
#define ONEINCR(subchannel, method, count) (0xa0000000u | ((subchannel) << 13) | ((count) << 16) | ((method) >> 2))
#define CE_INCR(method, count) INCR(4, NVB0B5_##method, count)
#define CE_IMMD(method, data) IMMD(4, NVB0B5_##method, data)

static void test_copy_stream(void)
{
    const uint32_t dma = NVDEF(NVB0B5, LAUNCH_DMA, DATA_TRANSFER_TYPE, NON_PIPELINED) |
        NVDEF(NVB0B5, LAUNCH_DMA, FLUSH_ENABLE, TRUE);
    const uint32_t multi = NVDEF(NVB0B5, LAUNCH_DMA, MULTI_LINE_ENABLE, TRUE);
    const uint32_t src_pitch = NVDEF(NVB0B5, LAUNCH_DMA, SRC_MEMORY_LAYOUT, PITCH);
    const uint32_t dst_pitch = NVDEF(NVB0B5, LAUNCH_DMA, DST_MEMORY_LAYOUT, PITCH);
    const uint32_t remap = NVDEF(NVB0B5, LAUNCH_DMA, REMAP_ENABLE, TRUE);
    struct tg_gpu_command_va va = {0x50460, 0x20000, 0x30000};
    struct tg_gpu_command_info info;
    uint32_t pitch[] = {
        CE_INCR(OFFSET_IN_UPPER, 8), 0, 0x10000, 0, 0x50000, 256, 512, 96, 3,
        CE_IMMD(LAUNCH_DMA, dma | multi | src_pitch | dst_pitch),
    };
    assert(tg_gpu_inspect_command_stream(pitch, sizeof(pitch), &va, &info));
    assert(info.va_end == va.limit && info.reserved_base == va.reserved_base);
    va.limit--;
    assert(!tg_gpu_inspect_command_stream(pitch, sizeof(pitch), &va, &info));
    va.limit++;
    pitch[4] = 0x1fff0;
    assert(!tg_gpu_inspect_command_stream(pitch, sizeof(pitch), &va, &info));
    pitch[4] = 0x50000;

    uint32_t tiled[] = {
        CE_INCR(OFFSET_IN_UPPER, 8), 0, 0x10000, 0, 0x50000, 128, 128, 64, 3,
        CE_INCR(SET_SRC_BLOCK_SIZE, 6), 0x1110, 128, 13, 3, 2, (5u << 16) | 32,
        CE_IMMD(LAUNCH_DMA, dma | multi | dst_pitch),
    };
    assert(tg_gpu_inspect_command_stream(tiled, sizeof(tiled), &va, &info));
    assert(info.va_end == 0x50140);
    const struct tg_gpu_command_va tile_padding = {0x60000, 0x11f00, 0x12000};
    assert(!tg_gpu_inspect_command_stream(tiled, sizeof(tiled), &tile_padding, &info));
    tiled[14] = 3; /* Layer must be below depth. */
    assert(!tg_gpu_inspect_command_stream(tiled, sizeof(tiled), &va, &info));

    const uint32_t fill[] = {
        CE_INCR(SET_REMAP_CONST_A, 1), 0x12345678,
        CE_INCR(SET_REMAP_COMPONENTS, 1), 0x34444,
        CE_INCR(OFFSET_OUT_UPPER, 6), 0, 0x50000, 512, 512, 64, 2,
        CE_IMMD(LAUNCH_DMA, dma | multi | src_pitch | dst_pitch | remap),
    };
    assert(tg_gpu_inspect_command_stream(fill, sizeof(fill), &va, &info));
    assert(info.va_end == 0x50300);

    const uint32_t release[] = {
        CE_INCR(SET_SEMAPHORE_A, 3), 0, 0x50000, 1,
        CE_IMMD(LAUNCH_DMA, NVDEF(NVB0B5, LAUNCH_DMA, SEMAPHORE_TYPE, RELEASE_FOUR_WORD_SEMAPHORE)),
    };
    assert(tg_gpu_inspect_command_stream(release, sizeof(release), &va, &info));
    assert(info.va_end == 0x50010);

    const uint32_t barrier[] = {
        CE_INCR(LINE_LENGTH_IN, 2), 0, 0,
        CE_IMMD(LAUNCH_DMA, dma | src_pitch | dst_pitch | remap),
    };
    assert(tg_gpu_inspect_command_stream(barrier, sizeof(barrier), &va, &info));
    assert(!info.va_end);

    const uint32_t phys_method[] = {CE_INCR(SET_SRC_PHYS_MODE, 1), 0};
    assert(!tg_gpu_validate_command_stream(phys_method, sizeof(phys_method)));
    assert(!tg_gpu_validate_command_stream(pitch + 9, sizeof(uint32_t))); /* Missing operands. */
    pitch[3] = 0xff;
    pitch[4] = 0xfffffff0;
    assert(!tg_gpu_validate_command_stream(pitch, sizeof(pitch))); /* 40-bit end overflow. */
    const uint32_t phys_launch[] = {
        CE_INCR(LAUNCH_DMA, 1), NVDEF(NVB0B5, LAUNCH_DMA, DST_TYPE, PHYSICAL),
    };
    assert(!tg_gpu_validate_command_stream(phys_launch, sizeof(phys_launch)));
}

static void test_inline_upload(void)
{
    const uint32_t launch = NVDEF(NVB197, LAUNCH_DMA, DST_MEMORY_LAYOUT, PITCH) |
                            NVDEF(NVB197, LAUNCH_DMA, COMPLETION_TYPE, FLUSH_ONLY);
    const struct tg_gpu_command_va va = {0x50000, 0x20000, 0x30000};
    struct tg_gpu_command_info info;
    uint32_t upload[] = {
        INCR(0, NVB197_LINE_LENGTH_IN, 4), 8, 1, 0, 0x1fff8,
        ONEINCR(0, NVB197_LAUNCH_DMA, 3), launch, 0x20010014, 0x55aa, 0,
    };
    assert(tg_gpu_inspect_command_stream(upload, sizeof(upload), &va, &info));
    assert(info.va_end == va.reserved_base);
    upload[0] = INCR(1, NVB197_LINE_LENGTH_IN, 4);
    assert(!tg_gpu_validate_command_stream(upload, sizeof(upload))); /* Wrong launch subchannel. */
    upload[5] = ONEINCR(1, NVB197_LAUNCH_DMA, 3);
    assert(tg_gpu_inspect_command_stream(upload, sizeof(upload), &va, &info));
    assert(!tg_gpu_validate_command_stream(upload, sizeof(upload) - 2 * sizeof(uint32_t)));
    assert(!tg_gpu_validate_command_stream(upload + 5, 4 * sizeof(uint32_t)));
    upload[1] = 4;
    assert(!tg_gpu_validate_command_stream(upload, sizeof(upload))); /* Length and payload differ. */
    upload[1] = 8;
    upload[4] += 4;
    assert(!tg_gpu_inspect_command_stream(upload, sizeof(upload), &va, &info));
    upload[4] -= 4;
    upload[6] = NVDEF(NVB197, LAUNCH_DMA, COMPLETION_TYPE, RELEASE_SEMAPHORE);
    assert(!tg_gpu_validate_command_stream(upload, sizeof(upload)));
}

static void test_report_and_acquire(void)
{
    const struct tg_gpu_command_va va = {0x50000, 0x20000, 0x30000};
    struct tg_gpu_command_info info;
    uint32_t report[] = {
        INCR(0, NVB197_SET_REPORT_SEMAPHORE_A, 4), 0, 0x4fff0, 0,
        NVDEF(NVB197, SET_REPORT_SEMAPHORE_D, OPERATION, REPORT_ONLY) |
        NVDEF(NVB197, SET_REPORT_SEMAPHORE_D, PIPELINE_LOCATION, ALL) |
        NVDEF(NVB197, SET_REPORT_SEMAPHORE_D, REPORT, ZPASS_PIXEL_CNT64),
    };
    assert(tg_gpu_inspect_command_stream(report, sizeof(report), &va, &info));
    assert(info.va_end == va.limit);
    report[4] |= NVDEF(NVB197, SET_REPORT_SEMAPHORE_D, STRUCTURE_SIZE, ONE_WORD);
    assert(!tg_gpu_validate_command_stream(report, sizeof(report)));
    report[0] = INCR(1, NVB197_SET_REPORT_SEMAPHORE_A, 4);
    assert(!tg_gpu_validate_command_stream(report, sizeof(report))); /* A graphics query on compute. */
    report[4] = 0; /* Compute release with a four-word report. */
    assert(tg_gpu_inspect_command_stream(report, sizeof(report), &va, &info));
    report[2] = va.reserved_base;
    assert(!tg_gpu_inspect_command_stream(report, sizeof(report), &va, &info));

    uint32_t macro[] = {ONEINCR(0, NVB197_CALL_MME_MACRO(NVK_MME_WRITE_CS_INVOCATIONS), 2), 0, 0x4fff8};
    assert(tg_gpu_inspect_command_stream(macro, sizeof(macro), &va, &info));
    assert(info.requires_mme && info.va_end == va.limit);
    macro[2] += 4;
    assert(!tg_gpu_inspect_command_stream(macro, sizeof(macro), &va, &info));

    const uint32_t acquire_flags = NVDEF(NVB06F, SEMAPHORED, ACQUIRE_SWITCH, ENABLED) |
                                   NVDEF(NVB06F, SEMAPHORED, RELEASE_SIZE, 4BYTE);
    uint32_t wait[] = {
        INCR(4, NVB06F_SEMAPHOREA, 4), 0, 0x4fffc, 1,
        acquire_flags | NVDEF(NVB06F, SEMAPHORED, OPERATION, ACQUIRE),
    };
    assert(tg_gpu_inspect_command_stream(wait, sizeof(wait), &va, &info));
    assert(info.va_end == va.limit);
    wait[4] = acquire_flags | NVDEF(NVB06F, SEMAPHORED, OPERATION, ACQ_GEQ);
    assert(tg_gpu_inspect_command_stream(wait, sizeof(wait), &va, &info));
    wait[4] = acquire_flags | NVDEF(NVB06F, SEMAPHORED, OPERATION, RELEASE);
    assert(!tg_gpu_validate_command_stream(wait, sizeof(wait)));
}

static void test_texture_pools(void)
{
    const struct tg_gpu_command_va va = {0x100050000, 0x100020000, 0x100030000};
    struct tg_gpu_command_info info;
    uint32_t pool[] = {INCR(0, NVB197_SET_TEX_HEADER_POOL_A, 3), 1, 0x40000, 2047};
    assert(tg_gpu_inspect_command_stream(pool, sizeof(pool), &va, &info));
    assert(info.va_end == va.limit);
    pool[3]++;
    assert(!tg_gpu_inspect_command_stream(pool, sizeof(pool), &va, &info));

    pool[0] = INCR(1, NVB1C0_SET_TEX_SAMPLER_POOL_A, 3);
    pool[2] = 0x1ffe0;
    pool[3] = 0;
    assert(tg_gpu_inspect_command_stream(pool, sizeof(pool), &va, &info));
    assert(info.va_end == va.reserved_base);
    pool[3] = 1;
    assert(!tg_gpu_inspect_command_stream(pool, sizeof(pool), &va, &info));
    pool[3] = DRF_MASK(NVB1C0_SET_TEX_SAMPLER_POOL_C_MAXIMUM_INDEX) + 1;
    assert(!tg_gpu_validate_command_stream(pool, sizeof(pool)));
    pool[3] = 0;
    pool[2] |= 4;
    assert(!tg_gpu_validate_command_stream(pool, sizeof(pool)));
    pool[2] &= ~4u;
    pool[1] = 0x100;
    assert(!tg_gpu_validate_command_stream(pool, sizeof(pool)));
    pool[1] = 1;
    pool[0] = ONEINCR(1, NVB1C0_SET_TEX_SAMPLER_POOL_A, 3);
    assert(!tg_gpu_validate_command_stream(pool, sizeof(pool)));

    const uint32_t standalone = IMMD(0, NVB197_SET_TEX_HEADER_POOL_C, 0);
    const uint32_t partial[] = {INCR(1, NVB1C0_SET_TEX_SAMPLER_POOL_B, 2), 0x10000, 0};
    assert(!tg_gpu_validate_command_stream(&standalone, sizeof(standalone)));
    assert(!tg_gpu_validate_command_stream(partial, sizeof(partial)));
}

static void test_compute_dispatch(void)
{
    struct tg_gpu_command_va va = {0x100050000, 0x100020080, 0x100030000};
    struct tg_gpu_command_info info;
    const uint32_t launch = NVDEF(NVB1C0, SEND_SIGNALING_PCAS_B, INVALIDATE, TRUE) |
                            NVDEF(NVB1C0, SEND_SIGNALING_PCAS_B, SCHEDULE, TRUE);
    uint32_t dispatch[] = {
        INCR(1, NVB1C0_SEND_PCAS_A, 1), 0x10004ff,
        IMMD(1, NVB1C0_SEND_SIGNALING_PCAS_B, launch),
    };
    assert(tg_gpu_inspect_command_stream(dispatch, sizeof(dispatch), &va, &info));
    assert(info.va_end == va.limit);
    va.limit--;
    assert(!tg_gpu_inspect_command_stream(dispatch, sizeof(dispatch), &va, &info));
    va.limit++;
    dispatch[1] = 0x1000200;
    assert(!tg_gpu_inspect_command_stream(dispatch, sizeof(dispatch), &va, &info));
    dispatch[1] = 0x10004ff;
    assert(!tg_gpu_validate_command_stream(dispatch + 2, sizeof(uint32_t)));
    dispatch[2] = IMMD(1, NVB1C0_SEND_SIGNALING_PCAS_B, launch ^ 1u);
    assert(!tg_gpu_validate_command_stream(dispatch, sizeof(dispatch)));
    dispatch[2] = IMMD(0, NVB1C0_SEND_SIGNALING_PCAS_B, launch);
    assert(!tg_gpu_validate_command_stream(dispatch, sizeof(dispatch)));

    const uint32_t debug[] = {
        INCR(1, NVB1C0_SEND_PCAS_A, 1), 0x10004ff,
        INCR(1, NVB1C0_SEND_SIGNALING_PCAS_B, 1), launch,
    };
    assert(tg_gpu_inspect_command_stream(debug, sizeof(debug), &va, &info));
    assert(info.va_end == va.limit);
    assert(!tg_gpu_validate_command_stream(debug, sizeof(debug) - sizeof(uint32_t)));
    const uint32_t other_pcas[] = {INCR(1, NVB1C0_SEND_PCAS_B, 1), 0};
    assert(!tg_gpu_validate_command_stream(other_pcas, sizeof(other_pcas)));
}

int main(void)
{
    uint32_t w[16]={0x20010000,0xb197,0x20020100,0x20010014,0xdeadbeef,0};
    assert(tg_gpu_validate_command_stream(w,24)); /* Data may resemble a header. */
    assert(!tg_gpu_validate_command_stream(NULL,4));
    assert(!tg_gpu_validate_command_stream(w,0) && !tg_gpu_validate_command_stream(w,23));
    assert(!tg_gpu_validate_command_stream((void *)((char *)w+1),4));
    const uint32_t classes[8]={[0]=0xb197,[1]=0xb1c0,[4]=0xb0b5};
    for(unsigned sub=0;sub<8;sub++)for(unsigned op=1;op<=5;op++) {
        if(op==2)continue;
        w[0]=(op<<29)|(sub<<13)|(1<<16)|(0x50>>2);w[1]=123;
        assert(!tg_gpu_validate_command_stream(w,op==4?4:8));
        w[0]=(w[0]&~0xfffu)|(0x100>>2);
        assert(tg_gpu_validate_command_stream(w,op==4?4:8)==(classes[sub]!=0));
        if(op!=4)assert(!tg_gpu_validate_command_stream(w,4));
        w[0]|=1u<<12;assert(!tg_gpu_validate_command_stream(w,op==4?4:8));
    }
    for(unsigned sub=0;sub<8;sub++) {
        w[0]=0x20010000 | (sub<<13);
        w[1]=classes[sub];
        assert(tg_gpu_validate_command_stream(w,8)==(classes[sub]!=0));
        w[1]=classes[sub] | (1u<<16);
        assert(!tg_gpu_validate_command_stream(w,8));
    }
    w[0]=0x20018000;w[1]=0xb197; /* A copy subchannel must bind the copy class. */
    assert(!tg_gpu_validate_command_stream(w,8));
    w[0]=0x80010000;assert(!tg_gpu_validate_command_stream(w,4)); /* Immediate SET_OBJECT. */
    w[0]=0x60020000;w[1]=w[2]=0xb197;
    assert(!tg_gpu_validate_command_stream(w,12)); /* One object binding per packet. */
    w[0]=0x20030000;assert(!tg_gpu_validate_command_stream(w,16)); /* Host span. */
    /* Check method wrap on the compute subchannel. */
    w[0]=0x20012fff;assert(tg_gpu_validate_command_stream(w,8));
    w[0]=0x20022fff;assert(!tg_gpu_validate_command_stream(w,12));
    w[0]=0xa0022fff;assert(!tg_gpu_validate_command_stream(w,12));
    w[0]=0x60022fff;assert(tg_gpu_validate_command_stream(w,12));
    w[0]=0x00010000;assert(!tg_gpu_validate_command_stream(w,4)); /* Subdevice. */
    w[0]=0xe0000000;assert(!tg_gpu_validate_command_stream(w,4)); /* End segment. */
    w[0]=0xc0000040;assert(!tg_gpu_validate_command_stream(w,4)); /* Reserved. */
    w[0]=0x20000040;assert(!tg_gpu_validate_command_stream(w,4)); /* Zero count. */
    test_mme_upload();
    test_mme_calls();
    test_mme_order();
    test_copy_stream();
    test_inline_upload();
    test_report_and_acquire();
    test_texture_pools();
    test_compute_dispatch();
    puts("B06F packets, GM20B MME and command address ranges passed");return 0;
}
