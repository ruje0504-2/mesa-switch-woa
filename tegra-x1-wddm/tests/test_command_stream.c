/* SPDX-License-Identifier: MIT */
#include "../core/command_stream.h"
#include "../core/nvk_mme_programs.h"
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
    w[0]=0x20010fff;assert(tg_gpu_validate_command_stream(w,8));
    w[0]=0x20020fff;assert(!tg_gpu_validate_command_stream(w,12)); /* Method wrap. */
    w[0]=0xa0020fff;assert(!tg_gpu_validate_command_stream(w,12));
    w[0]=0x60020fff;assert(tg_gpu_validate_command_stream(w,12));
    w[0]=0x00010000;assert(!tg_gpu_validate_command_stream(w,4)); /* Subdevice. */
    w[0]=0xe0000000;assert(!tg_gpu_validate_command_stream(w,4)); /* End segment. */
    w[0]=0xc0000040;assert(!tg_gpu_validate_command_stream(w,4)); /* Reserved. */
    w[0]=0x20000040;assert(!tg_gpu_validate_command_stream(w,4)); /* Zero count. */
    test_mme_upload();
    puts("Linear B06F packets, object bindings, reference isolation and MME uploads passed");return 0;
}
