/* SPDX-License-Identifier: MIT */
#include "../core/command_stream.h"
#include "../core/nvk_mme_programs.h"
#include "nouveau/headers/nvidia/classes/clb197.h"
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

    assert(tg_gpu_inspect_command_stream(call, sizeof(call), &info));
    assert(info.requires_mme && !info.uploads_mme);

    memcpy(stream, tg_gpu_nvk_mme_init, sizeof(tg_gpu_nvk_mme_init));
    memcpy(stream + sizeof(tg_gpu_nvk_mme_init) / sizeof(*stream), call, sizeof(call));
    assert(tg_gpu_inspect_command_stream(stream, sizeof(stream), &info));
    assert(!info.requires_mme && info.uploads_mme);

    memcpy(stream, call, sizeof(call));
    memcpy(stream + sizeof(call) / sizeof(*stream), tg_gpu_nvk_mme_init, sizeof(tg_gpu_nvk_mme_init));
    assert(tg_gpu_inspect_command_stream(stream, sizeof(stream), &info));
    assert(info.requires_mme && info.uploads_mme);
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
    /* Check method wrap on the copy subchannel, outside the 3D MME range. */
    w[0]=0x20018fff;assert(tg_gpu_validate_command_stream(w,8));
    w[0]=0x20028fff;assert(!tg_gpu_validate_command_stream(w,12));
    w[0]=0xa0028fff;assert(!tg_gpu_validate_command_stream(w,12));
    w[0]=0x60028fff;assert(tg_gpu_validate_command_stream(w,12));
    w[0]=0x00010000;assert(!tg_gpu_validate_command_stream(w,4)); /* Subdevice. */
    w[0]=0xe0000000;assert(!tg_gpu_validate_command_stream(w,4)); /* End segment. */
    w[0]=0xc0000040;assert(!tg_gpu_validate_command_stream(w,4)); /* Reserved. */
    w[0]=0x20000040;assert(!tg_gpu_validate_command_stream(w,4)); /* Zero count. */
    test_mme_upload();
    test_mme_calls();
    test_mme_order();
    puts("Linear B06F packets, object bindings, reference isolation and MME validation passed");return 0;
}
