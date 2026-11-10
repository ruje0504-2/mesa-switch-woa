/* SPDX-License-Identifier: MIT */
#include "command_stream.h"
#include "copy_stream.h"
#include "gm20b_mmu.h"
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

bool tg_gpu_command_range_allowed(const struct tg_gpu_command_va *va, uint64_t address,
                                  uint64_t bytes, uint64_t *max_end)
{
    const uint64_t limit = va ? va->limit : TG_GPU_VA_LIMIT;
    if (!address || !bytes || address >= limit || bytes > limit - address)
        return false;
    const uint64_t end = address + bytes;
    if (va && va->reserved_base < va->reserved_end &&
        address < va->reserved_end && end > va->reserved_base)
        return false;
    if (end > *max_end)
        *max_end = end;
    return true;
}

static bool word_span(const struct tg_gpu_command_va *va, uint32_t hi, uint32_t lo,
                      uint32_t bytes, uint32_t alignment, uint64_t *max_end)
{
    return hi <= UINT8_MAX && !(lo & (alignment - 1)) &&
           tg_gpu_command_range_allowed(va, ((uint64_t)hi << 32) | lo, bytes, max_end);
}

static bool host_acquire(const uint32_t *args, const struct tg_gpu_command_va *va,
                         uint64_t *max_end)
{
    const uint32_t flags = NVDEF(NVB06F, SEMAPHORED, ACQUIRE_SWITCH, ENABLED) |
                           NVDEF(NVB06F, SEMAPHORED, RELEASE_SIZE, 4BYTE);
    const uint32_t equal = flags | NVDEF(NVB06F, SEMAPHORED, OPERATION, ACQUIRE);
    const uint32_t geq = flags | NVDEF(NVB06F, SEMAPHORED, OPERATION, ACQ_GEQ);
    return (args[3] == equal || args[3] == geq) &&
           word_span(va, args[0], args[1], 4, 4, max_end);
}

/* The B197 graphics and B1C0 compute engines share these I2M methods. */
static bool inline_upload_span(uint32_t first, uint32_t last)
{
    return (first <= NVB197_LOAD_INLINE_DATA && last >= NVB197_LINE_LENGTH_IN) ||
           (first <= NVB197_SET_I2M_SEMAPHORE_C && last >= NVB197_SET_I2M_SEMAPHORE_A);
}

static size_t inline_upload(uint32_t subchannel, const uint32_t *args, size_t remaining,
                            const struct tg_gpu_command_va *va, uint64_t *max_end)
{
    if (remaining < 6 || !args[0] || (args[0] & 3) || args[1] != 1)
        return 0;
    const uint32_t bytes = args[0], words = bytes / 4;
    if (words + 1 > DRF_MASK(NVB06F_DMA_ONEINCR_COUNT) || words > remaining - 6)
        return 0;
    const uint32_t header = NVDEF(NVB06F, DMA_ONEINCR, OPCODE, VALUE) |
                            NVVAL(NVB06F, DMA_ONEINCR, SUBCHANNEL, subchannel) |
                            NVVAL(NVB06F, DMA_ONEINCR, COUNT, words + 1) |
                            NVVAL(NVB06F, DMA_ONEINCR, ADDRESS, NVB197_LAUNCH_DMA >> 2);
    const uint32_t launch = NVDEF(NVB197, LAUNCH_DMA, DST_MEMORY_LAYOUT, PITCH) |
                            NVDEF(NVB197, LAUNCH_DMA, COMPLETION_TYPE, FLUSH_ONLY);
    if (args[4] != header || args[5] != launch ||
        !word_span(va, args[2], args[3], bytes, 4, max_end))
        return 0;
    return 6 + words;
}

static bool report_span(uint32_t first, uint32_t last)
{
    return first <= NVB197_SET_REPORT_SEMAPHORE_D && last >= NVB197_SET_REPORT_SEMAPHORE_A;
}

static bool graphics_report_control(uint32_t control, uint32_t bytes)
{
    const uint32_t allowed = DRF_SMASK(NVB197_SET_REPORT_SEMAPHORE_D_OPERATION) |
                             DRF_SMASK(NVB197_SET_REPORT_SEMAPHORE_D_FLUSH_DISABLE) |
                             DRF_SMASK(NVB197_SET_REPORT_SEMAPHORE_D_RELEASE) |
                             DRF_SMASK(NVB197_SET_REPORT_SEMAPHORE_D_PIPELINE_LOCATION) |
                             DRF_SMASK(NVB197_SET_REPORT_SEMAPHORE_D_REPORT) |
                             DRF_SMASK(NVB197_SET_REPORT_SEMAPHORE_D_SUB_REPORT) |
                             DRF_SMASK(NVB197_SET_REPORT_SEMAPHORE_D_STRUCTURE_SIZE);
#define LOCATION_BIT(name) (1u << NVB197_SET_REPORT_SEMAPHORE_D_PIPELINE_LOCATION_##name)
    const uint32_t locations = LOCATION_BIT(NONE) | LOCATION_BIT(DATA_ASSEMBLER) |
        LOCATION_BIT(VERTEX_SHADER) | LOCATION_BIT(TESSELATION_INIT_SHADER) |
        LOCATION_BIT(TESSELATION_SHADER) | LOCATION_BIT(GEOMETRY_SHADER) |
        LOCATION_BIT(STREAMING_OUTPUT) | LOCATION_BIT(VPC) | LOCATION_BIT(ZCULL) |
        LOCATION_BIT(PIXEL_SHADER) | LOCATION_BIT(DEPTH_TEST) | LOCATION_BIT(ALL);
#undef LOCATION_BIT
    const uint32_t location = NVVAL_GET(control, NVB197, SET_REPORT_SEMAPHORE_D, PIPELINE_LOCATION);
    if ((control & ~allowed) || !(locations & (1u << location)))
        return false;

    const uint32_t operation = NVVAL_GET(control, NVB197, SET_REPORT_SEMAPHORE_D, OPERATION);
    const uint32_t report = NVVAL_GET(control, NVB197, SET_REPORT_SEMAPHORE_D, REPORT);
    const uint32_t sub_report = NVVAL_GET(control, NVB197, SET_REPORT_SEMAPHORE_D, SUB_REPORT);
    if (operation == NVB197_SET_REPORT_SEMAPHORE_D_OPERATION_RELEASE)
        return !report && !sub_report;
    if (operation != NVB197_SET_REPORT_SEMAPHORE_D_OPERATION_REPORT_ONLY)
        return false;

#define REPORT_BIT(name) (1u << NVB197_SET_REPORT_SEMAPHORE_D_REPORT_##name)
    const uint32_t queries = REPORT_BIT(ZPASS_PIXEL_CNT64) | REPORT_BIT(DA_VERTICES_GENERATED) |
        REPORT_BIT(DA_PRIMITIVES_GENERATED) | REPORT_BIT(VS_INVOCATIONS) | REPORT_BIT(GS_INVOCATIONS) |
        REPORT_BIT(GS_PRIMITIVES_GENERATED) | REPORT_BIT(CLIPPER_INVOCATIONS) |
        REPORT_BIT(CLIPPER_PRIMITIVES_GENERATED) | REPORT_BIT(PS_INVOCATIONS) |
        REPORT_BIT(TI_INVOCATIONS) | REPORT_BIT(TS_INVOCATIONS);
    const uint32_t streams = REPORT_BIT(VTG_PRIMITIVES_OUT) | REPORT_BIT(STREAMING_PRIMITIVES_SUCCEEDED) |
                             REPORT_BIT(STREAMING_PRIMITIVES_NEEDED);
#undef REPORT_BIT
    if (report == NVB197_SET_REPORT_SEMAPHORE_D_REPORT_STREAMING_BYTE_COUNT)
        return bytes == 4 && sub_report < 4;
    return bytes == 16 && ((sub_report == 0 && (queries & (1u << report))) ||
                           (sub_report < 4 && (streams & (1u << report))));
}

static bool engine_report(uint32_t subchannel, const uint32_t *args,
                          const struct tg_gpu_command_va *va, uint64_t *max_end)
{
    const uint32_t control = args[3];
    const uint32_t bytes = NVVAL_GET(control, NVB197, SET_REPORT_SEMAPHORE_D, STRUCTURE_SIZE) ? 4 : 16;
    const uint32_t compute_release = DRF_SMASK(NVB1C0_SET_REPORT_SEMAPHORE_D_STRUCTURE_SIZE) |
                                     DRF_SMASK(NVB1C0_SET_REPORT_SEMAPHORE_D_FLUSH_DISABLE);
    const bool valid = subchannel == 0 ? graphics_report_control(control, bytes) :
                                        !(control & ~compute_release);
    return valid && word_span(va, args[0], args[1], bytes, bytes, max_end);
}

/* B197 and B1C0 share the pool methods and 32-byte descriptor stride. */
static uint32_t texture_pool_index_mask(uint32_t first, uint32_t last)
{
    if (first <= NVB197_SET_TEX_HEADER_POOL_C && last >= NVB197_SET_TEX_HEADER_POOL_A)
        return DRF_MASK(NVB197_SET_TEX_HEADER_POOL_C_MAXIMUM_INDEX);
    if (first <= NVB197_SET_TEX_SAMPLER_POOL_C && last >= NVB197_SET_TEX_SAMPLER_POOL_A)
        return DRF_MASK(NVB197_SET_TEX_SAMPLER_POOL_C_MAXIMUM_INDEX);
    return 0;
}

static bool pcas_span(uint32_t first, uint32_t last)
{
    return first <= NVB1C0_SEND_SIGNALING_PCAS_B && last >= NVB1C0_SEND_PCAS_A;
}

static size_t compute_dispatch(const uint32_t *args, size_t remaining,
                               const struct tg_gpu_command_va *va, uint64_t *max_end)
{
    const uint32_t launch = NVDEF(NVB1C0, SEND_SIGNALING_PCAS_B, INVALIDATE, TRUE) |
                            NVDEF(NVB1C0, SEND_SIGNALING_PCAS_B, SCHEDULE, TRUE);
    const uint32_t method = NVB1C0_SEND_SIGNALING_PCAS_B >> 2;
    const uint32_t immediate = NVDEF(NVB06F, DMA_IMMD, OPCODE, VALUE) |
                               NVVAL(NVB06F, DMA_IMMD, SUBCHANNEL, 1) |
                               NVVAL(NVB06F, DMA_IMMD, ADDRESS, method) |
                               NVVAL(NVB06F, DMA_IMMD, DATA, launch);
    const uint32_t incrementing = NVDEF(NVB06F, DMA_INCR, OPCODE, VALUE) |
                                  NVVAL(NVB06F, DMA_INCR, SUBCHANNEL, 1) |
                                  NVVAL(NVB06F, DMA_INCR, ADDRESS, method) |
                                  NVVAL(NVB06F, DMA_INCR, COUNT, 1);
    if (remaining < 2)
        return 0;
    size_t used;
    if (args[1] == immediate)
        used = 2;
    else if (remaining >= 3 && args[1] == incrementing && args[2] == launch)
        used = 3;
    else
        return 0;

    /* SEND_PCAS_A encodes the address in 256-byte units.
     * NVK's QMD 0.6 is 64 dwords.
     */
    return tg_gpu_command_range_allowed(va, (uint64_t)args[0] << 8, 256, max_end) ? used : 0;
}

static bool mme_upload_span(uint32_t subchannel, uint32_t first, uint32_t last)
{
    return subchannel == 0 && first <= NVB197_LOAD_MME_START_ADDRESS_RAM &&
           last >= NVB197_LOAD_MME_INSTRUCTION_RAM_POINTER;
}

static uint32_t vertex_fallback_pair(uint32_t first, uint32_t last)
{
    if (first <= NVB197_SET_VERTEX_STREAM_SUBSTITUTE_B && last >= NVB197_SET_VERTEX_STREAM_SUBSTITUTE_A)
        return NVB197_SET_VERTEX_STREAM_SUBSTITUTE_A;
    if (first <= TG_GPU_NVK_MME_ZERO_ADDR_LO && last >= TG_GPU_NVK_MME_ZERO_ADDR_HI)
        return TG_GPU_NVK_MME_ZERO_ADDR_HI;
    return 0;
}

static bool direct_input_binding_span(uint32_t first, uint32_t last)
{
    if (first <= NVB197_SET_INDEX_BUFFER_E && last >= NVB197_SET_INDEX_BUFFER_A)
        return true;
    if (first <= NVB197_SET_VERTEX_STREAM_LIMIT_B_B(15) && last >= NVB197_SET_VERTEX_STREAM_LIMIT_A_A(0))
        return true;

    /* Format and frequency writes remain separate from the address pair
     * in each vertex stream's register block.
     */
    const uint32_t base = NVB197_SET_VERTEX_STREAM_A_FORMAT(0);
    const uint32_t stride = NVB197_SET_VERTEX_STREAM_A_FORMAT(1) - base;
    if (last < base || first > NVB197_SET_VERTEX_STREAM_B_LOCATION_B(15))
        return false;
    uint32_t index = first > base ? (first - base) / stride : 0;
    if (first > NVB197_SET_VERTEX_STREAM_A_LOCATION_B(index))
        index++;
    return last >= NVB197_SET_VERTEX_STREAM_A_LOCATION_A(index);
}

static bool input_binding_range(const uint32_t *args, const struct tg_gpu_command_va *va,
                                bool zero_addr_ready, uint64_t *max_end)
{
    /* The pre-Turing macros substitute the scratch address for an empty
     * range and encode the inclusive limit as that address minus one.
     */
    if (!args[2] || (!args[0] && !args[1]))
        return zero_addr_ready;
    return word_span(va, args[0], args[1], args[2], 1, max_end);
}

static bool mme_call_allowed(uint32_t op, uint32_t method, uint32_t count,
                             const uint32_t *args, const struct tg_gpu_command_va *va,
                             bool zero_addr_ready, uint64_t *max_end)
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
    case NVK_MME_BIND_IB:
        return input_binding_range(args, va, zero_addr_ready, max_end);
    case NVK_MME_BIND_VB:
        return args[0] < 32 && input_binding_range(args + 1, va, zero_addr_ready, max_end);
    case NVK_MME_SET_WRITE_MASK:
        return args[0] <= 8;
    case NVK_MME_SET_VIEWPORT_MIN_MAX_Z:
        return args[0] < 16;
    case NVK_MME_WRITE_CS_INVOCATIONS:
        /* This macro writes two adjacent one-word reports. */
        return word_span(va, args[0], args[1], 8, 4, max_end);
    default:
        return true;
    }
}

bool tg_gpu_inspect_command_stream(const uint32_t *words, size_t bytes,
                                   const struct tg_gpu_command_va *va,
                                   struct tg_gpu_command_info *info)
{
    struct tg_gpu_command_info stream = {.reserved_base = va ? va->reserved_base : 0};
    struct tg_gpu_copy_state copy = {0};
    bool zero_addr_ready = false;
    if(!words || !bytes || (bytes&3) || ((uintptr_t)words&3))return false;
    if (va && (!va->limit || va->limit > TG_GPU_VA_LIMIT ||
               va->reserved_base > va->reserved_end || va->reserved_end > va->limit))
        return false;
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
               (subchannel == 0 && (method >= NVB197_CALL_MME_MACRO(0) ||
                                    direct_input_binding_span(method, method) ||
                                    vertex_fallback_pair(method, method))))return false;
            if (subchannel <= 1 && (inline_upload_span(method, method) ||
                                    report_span(method, method) ||
                                    texture_pool_index_mask(method, method)))
                return false;
            if (subchannel == 1 && pcas_span(method, method))
                return false;
            if (subchannel == 4 && method >= 0x100 &&
                !tg_gpu_copy_method(&copy, method, NVVAL_GET(header, NVB06F, DMA_IMMD, DATA),
                                    va, &stream.va_end))
                return false;
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
        if (method <= NVB06F_SEMAPHORED && last >= NVB06F_SEMAPHOREA) {
            if (op != NVB06F_DMA_INCR_OPCODE_VALUE || method != NVB06F_SEMAPHOREA ||
                count != 4 || !host_acquire(words + n, va, &stream.va_end))
                return false;
            n += count;
            continue;
        }
        if(!method_allowed(method) || !method_allowed(last))return false;
        /* Incrementing packets can span a forbidden host register even if
         * their first and last methods are both individually permitted. */
        if(last!=method && method<0x100)return false;
        if (subchannel <= 1 && inline_upload_span(method, last)) {
            if (op != NVB06F_DMA_INCR_OPCODE_VALUE || method != NVB197_LINE_LENGTH_IN || count != 4)
                return false;
            const size_t used = inline_upload(subchannel, words + n, word_count - n,
                                              va, &stream.va_end);
            if (!used)
                return false;
            n += used;
            continue;
        }
        if (subchannel <= 1 && report_span(method, last) &&
            (op != NVB06F_DMA_INCR_OPCODE_VALUE || method != NVB197_SET_REPORT_SEMAPHORE_A ||
             count != 4 || !engine_report(subchannel, words + n, va, &stream.va_end)))
            return false;
        const uint32_t pool_mask = subchannel <= 1 ? texture_pool_index_mask(method, last) : 0;
        if (pool_mask) {
            const uint32_t descriptor_bytes = 32;
            if (op != NVB06F_DMA_INCR_OPCODE_VALUE || count != 3 ||
                (method != NVB197_SET_TEX_HEADER_POOL_A && method != NVB197_SET_TEX_SAMPLER_POOL_A) ||
                (words[n + 2] & ~pool_mask) ||
                !word_span(va, words[n], words[n + 1], (words[n + 2] + 1) * descriptor_bytes,
                           descriptor_bytes, &stream.va_end))
                return false;
        }
        if (subchannel == 1 && pcas_span(method, last)) {
            if (op != NVB06F_DMA_INCR_OPCODE_VALUE || method != NVB1C0_SEND_PCAS_A || count != 1)
                return false;
            const size_t used = compute_dispatch(words + n, word_count - n, va, &stream.va_end);
            if (!used)
                return false;
            n += used;
            continue;
        }
        if (subchannel == 0) {
            if (direct_input_binding_span(method, last))
                return false;
            const uint32_t pair = vertex_fallback_pair(method, last);
            if (pair) {
                const uint32_t page_bytes = 1u << TG_GPU_PAGE_SHIFT;
                if (op != NVB06F_DMA_INCR_OPCODE_VALUE || method != pair || count != 2 ||
                    !word_span(va, words[n], words[n + 1], page_bytes, page_bytes, &stream.va_end))
                    return false;
                zero_addr_ready |= pair == TG_GPU_NVK_MME_ZERO_ADDR_HI;
            }
        }
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
            if (!mme_call_allowed(op, method, count, words + n, va, zero_addr_ready, &stream.va_end))
                return false;
            stream.requires_mme |= !stream.uploads_mme;
        }
        if (subchannel == 4) {
            if (method == NVB06F_SET_OBJECT)
                copy.written = 0;
            if (method >= 0x100) {
                for (uint32_t i = 0; i < count; i++) {
                    if (!tg_gpu_copy_method(&copy, method, words[n + i], va, &stream.va_end))
                        return false;
                    if (method < last)
                        method += 4;
                }
            }
        }
        n+=count;
    }
    if (info)
        *info = stream;
    return true;
}
