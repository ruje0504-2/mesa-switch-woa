/* SPDX-License-Identifier: MIT */
#include "command_stream.h"
#include "nvk_mme_programs.h"
#include "nouveau/headers/drf.h"
#include "nouveau/headers/nvidia/classes/clb06f.h"
#include "nouveau/headers/nvidia/classes/clb0b5.h"
#include "nouveau/headers/nvidia/classes/clb197.h"
#include "nouveau/headers/nvidia/classes/clb1c0.h"
#include "nouveau/vulkan/nvk_mme_slots.h"

/* NVK's 3D, compute and copy subchannels on GM20B. */
static const uint32_t nvk_class[NVB06F_NUMBER_OF_SUBCHANNELS] = {
    [0] = MAXWELL_B,
    [1] = MAXWELL_COMPUTE_B,
    [4] = MAXWELL_DMA_COPY_A,
};

/* Complete, CPU-visible calls emitted by NVK on Windows GM20B. */
static const uint8_t nvk_mme_call_words[NVK_MME_COUNT] = {
    [NVK_MME_SELECT_CB0] = 1,
    [NVK_MME_CLEAR] = 2,
    [NVK_MME_UPDATE_WINDOW_CLIP] = 1,
    [NVK_MME_BIND_IB] = 4,
    [NVK_MME_BIND_VB] = 4,
    [NVK_MME_SET_VB_ENABLES] = 1,
    [NVK_MME_SET_VB_STRIDE] = 1,
    [NVK_MME_SET_TESS_PARAMS] = 1,
    [NVK_MME_SET_ANTI_ALIAS] = 1,
    [NVK_MME_DRAW] = 5,
    [NVK_MME_DRAW_INDEXED] = 6,
    [NVK_MME_ADD_CS_INVOCATIONS] = 2,
    [NVK_MME_WRITE_CS_INVOCATIONS] = 2,
    [NVK_MME_SET_WRITE_MASK] = 2,
    [NVK_MME_SET_VIEWPORT_MIN_MAX_Z] = 3,
    [NVK_MME_SET_Z_CLAMP] = 1,
    [NVK_MME_SET_STATISTICS_COUNTERS] = 2,
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

static bool mme_call_allowed(uint32_t op, uint32_t method, uint32_t count,
                             const uint32_t *args)
{
    const uint32_t base = NVB197_CALL_MME_MACRO(0);
    if (op != NVB06F_DMA_ONEINCR_OPCODE_VALUE || method < base)
        return false;
    const uint32_t offset = method - base;
    const uint32_t slot = offset / 8;
    if (offset % 8 || slot >= NVK_MME_COUNT || count != nvk_mme_call_words[slot])
        return false;

    switch (slot) {
    case NVK_MME_CLEAR: {
        const uint32_t base_layer = NVVAL_GET(args[0], NVB197, CLEAR_SURFACE, RT_ARRAY_INDEX);
        const uint32_t max_layers = 2048;
        return base_layer < max_layers && args[1] && args[1] <= max_layers - base_layer;
    }
    case NVK_MME_BIND_VB:
        return args[0] < 32;
    case NVK_MME_SET_WRITE_MASK:
        return args[0] <= 8;
    case NVK_MME_SET_VIEWPORT_MIN_MAX_Z:
        return args[0] < 16;
    default:
        return true;
    }
}

bool tg_gpu_inspect_command_stream(const uint32_t *words, size_t bytes,
                                   struct tg_gpu_command_info *info)
{
    struct tg_gpu_command_info stream = {0};
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
               mme_upload_span(subchannel, method, method) ||
               (subchannel == 0 && method >= NVB197_CALL_MME_MACRO(0)))return false;
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
            stream.uploads_mme = true;
            n = start + mme_words;
            continue;
        }
        if (subchannel == 0 && last >= NVB197_CALL_MME_MACRO(0)) {
            if (!mme_call_allowed(op, method, count, words + n))
                return false;
            stream.requires_mme |= !stream.uploads_mme;
        }
        n+=count;
    }
    if (info)
        *info = stream;
    return true;
}
