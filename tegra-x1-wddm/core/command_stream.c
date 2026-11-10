/* SPDX-License-Identifier: MIT */
#include "command_stream.h"
#include "nvk_mme_programs.h"
#include "nouveau/headers/drf.h"
#include "nouveau/headers/nvidia/classes/clb06f.h"
#include "nouveau/headers/nvidia/classes/clb0b5.h"
#include "nouveau/headers/nvidia/classes/clb197.h"
#include "nouveau/headers/nvidia/classes/clb1c0.h"

/* NVK's 3D, compute and copy subchannels on GM20B. */
static const uint32_t nvk_class[NVB06F_NUMBER_OF_SUBCHANNELS] = {
    [0] = MAXWELL_B,
    [1] = MAXWELL_COMPUTE_B,
    [4] = MAXWELL_DMA_COPY_A,
};

static bool method_allowed(uint32_t method)
{
    return method>=0x100 || method==NVB06F_SET_OBJECT || method==NVB06F_WFI;
}

static bool mme_upload_span(uint32_t subchannel, uint32_t first, uint32_t last)
{
    return subchannel == 0 && first <= NVB197_LOAD_MME_START_ADDRESS_RAM &&
           last >= NVB197_LOAD_MME_INSTRUCTION_RAM_POINTER;
}

bool tg_gpu_validate_command_stream(const uint32_t *words,size_t bytes)
{
    if(!words || !bytes || (bytes&3) || ((uintptr_t)words&3))return false;
    const size_t word_count = bytes / sizeof(*words);
    for(size_t n=0;n<word_count;) {
        const size_t start = n;
        uint32_t header=words[n++],op=NVVAL_GET(header,NVB06F,DMA_INCR,OPCODE);
        uint32_t method=NVVAL_GET(header,NVB06F,DMA_INCR,ADDRESS)*4;
        uint32_t count=NVVAL_GET(header,NVB06F,DMA_INCR,COUNT),last;
        if(header==NVB06F_DMA_NOP)continue;
        uint32_t subchannel=NVVAL_GET(header,NVB06F,DMA_INCR,SUBCHANNEL);
        if((header&(1u<<12)) || !nvk_class[subchannel])return false;
        if(op==NVB06F_DMA_IMMD_OPCODE_VALUE) {
            if(!method_allowed(method) || method==NVB06F_SET_OBJECT ||
               mme_upload_span(subchannel, method, method))return false;
            continue;
        }
        if((op!=NVB06F_DMA_INCR_OPCODE_VALUE && op!=NVB06F_DMA_NONINCR_OPCODE_VALUE &&
            op!=NVB06F_DMA_ONEINCR_OPCODE_VALUE) || !count || count>word_count-n)return false;
        if(method==NVB06F_SET_OBJECT && (count!=1 || words[n]!=nvk_class[subchannel]))return false;
        last=method;
        if(op==NVB06F_DMA_INCR_OPCODE_VALUE) {
            if(count-1>(0x3ffc-method)/4)return false;
            last=method+(count-1)*4;
        } else if(op==NVB06F_DMA_ONEINCR_OPCODE_VALUE && count>1) {
            if(method==0x3ffc)return false;
            last=method+4;
        }
        if(!method_allowed(method) || !method_allowed(last))return false;
        /* Incrementing packets can span a forbidden host register even if
         * their first and last methods are both individually permitted. */
        if(last!=method && method<0x100)return false;
        if (mme_upload_span(subchannel, method, last)) {
            /* NVK uploads its programs and their slot bindings together. */
            const size_t mme_words = sizeof(tg_gpu_nvk_mme_init) / sizeof(*words);
            if (mme_words > word_count - start)
                return false;
            for (size_t i = 0; i < mme_words; i++) {
                if (words[start + i] != tg_gpu_nvk_mme_init[i])
                    return false;
            }
            n = start + mme_words;
            continue;
        }
        n+=count;
    }
    return true;
}
