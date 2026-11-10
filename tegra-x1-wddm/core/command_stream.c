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
                      uint64_t bytes, uint32_t alignment, uint64_t *max_end)
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

/* Byte widths for the renderable Maxwell formats in NIL's format table. */
static uint32_t color_target_element_bytes(uint32_t format)
{
    if (format >= NVB197_SET_COLOR_TARGET_FORMAT_V_RF32_GF32_BF32_AF32 &&
        format <= NVB197_SET_COLOR_TARGET_FORMAT_V_RU32_GU32_BU32_X32)
        return 16;
    if (format >= NVB197_SET_COLOR_TARGET_FORMAT_V_R16_G16_B16_A16 &&
        format <= NVB197_SET_COLOR_TARGET_FORMAT_V_RF16_GF16_BF16_X16)
        return 8;
    if ((format >= NVB197_SET_COLOR_TARGET_FORMAT_V_A8R8G8B8 &&
         format <= NVB197_SET_COLOR_TARGET_FORMAT_V_AU2BU10GU10RU10) ||
        (format >= NVB197_SET_COLOR_TARGET_FORMAT_V_A8B8G8R8 &&
         format <= NVB197_SET_COLOR_TARGET_FORMAT_V_BF10GF11RF11) ||
        (format >= NVB197_SET_COLOR_TARGET_FORMAT_V_RS32 &&
         format <= NVB197_SET_COLOR_TARGET_FORMAT_V_X8RL8GL8BL8) ||
        format == NVB197_SET_COLOR_TARGET_FORMAT_V_X8B8G8R8 ||
        format == NVB197_SET_COLOR_TARGET_FORMAT_V_X8BL8GL8RL8)
        return 4;
    if ((format >= NVB197_SET_COLOR_TARGET_FORMAT_V_R5G6B5 &&
         format <= NVB197_SET_COLOR_TARGET_FORMAT_V_RF16) ||
        format == NVB197_SET_COLOR_TARGET_FORMAT_V_X1R5G5B5)
        return 2;
    if (format >= NVB197_SET_COLOR_TARGET_FORMAT_V_R8 && format <= NVB197_SET_COLOR_TARGET_FORMAT_V_A8)
        return 1;
    return 0;
}

static const uint8_t depth_target_element_bytes[32] = {
    [NVB197_SET_ZT_FORMAT_V_Z16] = 2,
    [NVB197_SET_ZT_FORMAT_V_Z24S8] = 4,
    [NVB197_SET_ZT_FORMAT_V_X8Z24] = 4,
    [NVB197_SET_ZT_FORMAT_V_S8Z24] = 4,
    [NVB197_SET_ZT_FORMAT_V_S8] = 1,
    [NVB197_SET_ZT_FORMAT_V_ZF32] = 4,
    [NVB197_SET_ZT_FORMAT_V_ZF32_X24S8] = 8,
};

static uint64_t tiled_target_layer_bytes(uint32_t width, uint32_t height,
                                         uint32_t element_bytes, uint32_t block_height)
{
    const uint64_t row_bytes = (uint64_t)width * element_bytes;
    const uint32_t row_mask = (8u << block_height) - 1;
    /* NVK programs width as a GOB-aligned stride in samples. */
    return row_bytes % 64 ? 0 : row_bytes * ((height + row_mask) & ~row_mask);
}

static uint64_t target_array_bytes(uint64_t layer_bytes, uint32_t pitch_words,
                                   uint32_t layers, uint32_t alignment)
{
    const uint64_t pitch = (uint64_t)pitch_words * 4;
    if (!layer_bytes || !layers || (layers > 1 && (pitch < layer_bytes || pitch % alignment)))
        return 0;
    return (layers - 1) * pitch + layer_bytes;
}

static bool color_target_span(uint32_t first, uint32_t last)
{
    const uint32_t base = NVB197_SET_COLOR_TARGET_A(0);
    const uint32_t stride = NVB197_SET_COLOR_TARGET_A(1) - base;
    if (last < base || first > NVB197_SET_COLOR_TARGET_LAYER(7))
        return false;
    const uint32_t slot = first > base ? (first - base) / stride : 0;
    return first <= NVB197_SET_COLOR_TARGET_LAYER(slot) || last >= NVB197_SET_COLOR_TARGET_A(slot + 1);
}

static bool color_target(const uint32_t *args, const struct tg_gpu_command_va *va,
                         uint64_t *max_end)
{
    const uint32_t width = args[2], height = args[3], format = args[4], layout = args[5];
    const uint32_t layers = args[6], pitch_words = args[7], layer = args[8];
    if (width > DRF_MASK(NVB197_SET_COLOR_TARGET_WIDTH_V) ||
        height > DRF_MASK(NVB197_SET_COLOR_TARGET_HEIGHT_V) ||
        layers > DRF_MASK(NVB197_SET_COLOR_TARGET_THIRD_DIMENSION_V) || layer)
        return false;
    if (format == NVB197_SET_COLOR_TARGET_FORMAT_V_DISABLED)
        return !args[0] && !args[1] && !height && !layout && !pitch_words;

    const uint32_t element_bytes = color_target_element_bytes(format);
    if (!element_bytes || !width || !height || !layers)
        return false;
    if (layout == NVDEF(NVB197, SET_COLOR_TARGET_MEMORY, LAYOUT, PITCH))
        return layers == 1 && !pitch_words && !(width % 128) &&
               word_span(va, args[0], args[1], (uint64_t)width * height, 128, max_end);

    const uint32_t mask = DRF_SMASK(NVB197_SET_COLOR_TARGET_MEMORY_BLOCK_HEIGHT) |
                          DRF_SMASK(NVB197_SET_COLOR_TARGET_MEMORY_BLOCK_DEPTH) |
                          DRF_SMASK(NVB197_SET_COLOR_TARGET_MEMORY_THIRD_DIMENSION_CONTROL);
    const uint32_t block_height = NVVAL_GET(layout, NVB197, SET_COLOR_TARGET_MEMORY, BLOCK_HEIGHT);
    const uint32_t block_depth = NVVAL_GET(layout, NVB197, SET_COLOR_TARGET_MEMORY, BLOCK_DEPTH);
    const bool volume = NVVAL_GET(layout, NVB197, SET_COLOR_TARGET_MEMORY, THIRD_DIMENSION_CONTROL);
    if ((layout & ~mask) || block_height > 5 || block_depth > 5 || (!volume && block_depth))
        return false;

    const uint32_t alignment = 512u << block_height;
    const uint64_t plane_bytes = tiled_target_layer_bytes(width, height, element_bytes, block_height);
    if (!volume)
        return word_span(va, args[0], args[1], target_array_bytes(plane_bytes, pitch_words, layers, alignment),
                         alignment, max_end);

    const uint32_t depth_mask = (1u << block_depth) - 1;
    const uint32_t padded_depth = (layers + depth_mask) & ~depth_mask;
    return plane_bytes <= TG_GPU_VA_LIMIT / padded_depth &&
           word_span(va, args[0], args[1], plane_bytes * padded_depth, alignment, max_end);
}

static bool depth_target_span(uint32_t first, uint32_t last)
{
    return (first <= NVB197_SET_ZT_ARRAY_PITCH && last >= NVB197_SET_ZT_A) ||
           (first <= NVB197_SET_ZT_SIZE_C && last >= NVB197_SET_ZT_SIZE_A) ||
           (first <= NVB197_SET_ZT_LAYER && last >= NVB197_SET_ZT_LAYER);
}

/* Read a single B197 method written by P_IMMD, which can fall back to INCR. */
static size_t read_3d_word(const uint32_t *words, size_t remaining,
                           uint32_t method, uint32_t *value)
{
    if (!remaining)
        return 0;
    const uint32_t address = method >> 2;
    const uint32_t immediate = NVDEF(NVB06F, DMA_IMMD, OPCODE, VALUE) |
                               NVVAL(NVB06F, DMA_IMMD, ADDRESS, address);
    if ((words[0] & ~DRF_SMASK(NVB06F_DMA_IMMD_DATA)) == immediate) {
        *value = NVVAL_GET(words[0], NVB06F, DMA_IMMD, DATA);
        return 1;
    }
    const uint32_t incrementing = NVDEF(NVB06F, DMA_INCR, OPCODE, VALUE) |
                                  NVVAL(NVB06F, DMA_INCR, ADDRESS, address) |
                                  NVVAL(NVB06F, DMA_INCR, COUNT, 1);
    if (remaining < 2 || words[0] != incrementing)
        return 0;
    *value = words[1];
    return 2;
}

static size_t color_target_setup(uint32_t method, uint32_t count, const uint32_t *args,
                                  size_t remaining, const struct tg_gpu_command_va *va,
                                  uint64_t *max_end)
{
    if (count == 9)
        return color_target(args, va, max_end) ? 9 : 0;
    if (count != 1)
        return 0;

    /* The private clear diagnostic writes the same nine registers singly. */
    uint32_t target[9] = {args[0]};
    size_t used = 1;
    for (uint32_t i = 1; i < 9; i++) {
        const size_t words = read_3d_word(args + used, remaining - used,
                                          method + i * 4, &target[i]);
        if (!words)
            return 0;
        used += words;
    }
    return color_target(target, va, max_end) ? used : 0;
}

static size_t depth_target(const uint32_t *args, size_t remaining,
                            const struct tg_gpu_command_va *va, uint64_t *max_end)
{
    const uint32_t format = args[2], block = args[3], pitch_words = args[4];
    const uint32_t block_height = NVVAL_GET(block, NVB197, SET_ZT_BLOCK_SIZE, HEIGHT);
    if (format >= sizeof(depth_target_element_bytes) || !depth_target_element_bytes[format] ||
        (block & ~DRF_SMASK(NVB197_SET_ZT_BLOCK_SIZE_HEIGHT)) || block_height > 5)
        return 0;

    uint32_t enabled, layer;
    const size_t select_words = read_3d_word(args + 5, remaining - 5, NVB197_SET_ZT_SELECT, &enabled);
    const size_t size_pos = 5 + select_words;
    const uint32_t size_header = NVDEF(NVB06F, DMA_INCR, OPCODE, VALUE) |
                                 NVVAL(NVB06F, DMA_INCR, ADDRESS, NVB197_SET_ZT_SIZE_A >> 2) |
                                 NVVAL(NVB06F, DMA_INCR, COUNT, 3);
    if (!select_words || enabled != 1 || remaining - size_pos < 4 || args[size_pos] != size_header)
        return 0;
    const uint32_t width = args[size_pos + 1], height = args[size_pos + 2], layers = args[size_pos + 3];
    const size_t layer_pos = size_pos + 4;
    const size_t layer_words = read_3d_word(args + layer_pos, remaining - layer_pos, NVB197_SET_ZT_LAYER, &layer);
    if (!layer_words || width > DRF_MASK(NVB197_SET_ZT_SIZE_A_WIDTH) ||
        height > DRF_MASK(NVB197_SET_ZT_SIZE_B_HEIGHT) ||
        layers > DRF_MASK(NVB197_SET_ZT_SIZE_C_THIRD_DIMENSION) || layer >= layers)
        return 0;

    const uint32_t alignment = 512u << block_height;
    const uint64_t plane_bytes = tiled_target_layer_bytes(width, height, depth_target_element_bytes[format],
                                                          block_height);
    const uint64_t bytes = target_array_bytes(plane_bytes, pitch_words, layers, alignment);
    return word_span(va, args[0], args[1], bytes, alignment, max_end) ? layer_pos + layer_words : 0;
}

static bool cb0_scratch_span(uint32_t first, uint32_t last)
{
    return first <= TG_GPU_NVK_MME_CB0_ADDR_LO && last >= TG_GPU_NVK_MME_CB0_ADDR_HI;
}

static bool cbuf_data_span(uint32_t first, uint32_t last)
{
    return first < NVB197_BIND_GROUP_RESERVED_A(0) && last >= NVB197_SET_CONSTANT_BUFFER_SELECTOR_A;
}

static uint32_t cbuf_group_in_span(uint32_t first, uint32_t last)
{
    const uint32_t base = NVB197_BIND_GROUP_CONSTANT_BUFFER(0);
    const uint32_t stride = NVB197_BIND_GROUP_CONSTANT_BUFFER(1) - base;
    const uint32_t offset = first > base ? first - base : 0;
    const uint32_t group = base + ((offset + stride - 1) / stride) * stride;
    return group <= last && group <= NVB197_BIND_GROUP_CONSTANT_BUFFER(4) ? group : 0;
}

static bool cbuf_binding_value(uint32_t value, bool direct)
{
    const uint32_t mask = DRF_SMASK(NVB197_BIND_GROUP_CONSTANT_BUFFER_VALID) |
                          DRF_SMASK(NVB197_BIND_GROUP_CONSTANT_BUFFER_SHADER_SLOT);
    const uint32_t slot = NVVAL_GET(value, NVB197, BIND_GROUP_CONSTANT_BUFFER, SHADER_SLOT);
    const bool valid = NVVAL_GET(value, NVB197, BIND_GROUP_CONSTANT_BUFFER, VALID);
    return !(value & ~mask) && slot < 16 && (direct ? valid && slot : !valid || !slot);
}

static size_t direct_cbuf_binding(const uint32_t *args, size_t remaining,
                                  const struct tg_gpu_command_va *va, uint64_t *max_end)
{
    const uint32_t alignment = 256;
    const uint32_t bytes = args[0];
    if (remaining < 6 || bytes > 65536 || (bytes & (alignment - 1)))
        return 0;

    const uint32_t method = NVVAL_GET(args[3], NVB06F, DMA_INCR, ADDRESS) * 4;
    if (!cbuf_group_in_span(method, method))
        return 0;
    uint32_t bind;
    const size_t bind_words = read_3d_word(args + 3, remaining - 3, method, &bind);
    const size_t next = 3 + bind_words;
    const uint32_t restore = NVDEF(NVB06F, DMA_ONEINCR, OPCODE, VALUE) |
                             NVVAL(NVB06F, DMA_ONEINCR, COUNT, 1) |
                             NVVAL(NVB06F, DMA_ONEINCR, ADDRESS, NVB197_CALL_MME_MACRO(NVK_MME_SELECT_CB0) >> 2);
    if (!bind_words || !cbuf_binding_value(bind, true) || remaining - next < 2 ||
        args[next] != restore || args[next + 1] ||
        !word_span(va, args[1], args[2], bytes, alignment, max_end))
        return 0;
    /* Leave SELECT_CB0 to the normal MME validation below. */
    return next;
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
    case NVK_MME_SELECT_CB0:
        return !args[0];
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
    uint64_t cb0_setup_end = 0;
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
               mme_upload_span(subchannel, method, method))return false;
            if (subchannel == 0 &&
                (method >= NVB197_CALL_MME_MACRO(0) || direct_input_binding_span(method, method) ||
                 vertex_fallback_pair(method, method) || color_target_span(method, method) ||
                 depth_target_span(method, method) || (method == NVB197_SET_ZT_SELECT && count) ||
                 cb0_scratch_span(method, method) || cbuf_data_span(method, method)))
                return false;
            if (subchannel == 0 && cbuf_group_in_span(method, method)) {
                const uint32_t value = NVVAL_GET(header, NVB06F, DMA_IMMD, DATA);
                if (!cbuf_binding_value(value, false))
                    return false;
                stream.requires_cb0 |= (value & DRF_SMASK(NVB197_BIND_GROUP_CONSTANT_BUFFER_VALID)) && !stream.cb0_end;
            }
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
            if (color_target_span(method, last)) {
                const uint32_t base = NVB197_SET_COLOR_TARGET_A(0);
                const uint32_t stride = NVB197_SET_COLOR_TARGET_A(1) - base;
                if (op != NVB06F_DMA_INCR_OPCODE_VALUE || method < base || (method - base) % stride)
                    return false;
                const size_t used = color_target_setup(method, count, words + n, word_count - n,
                                                        va, &stream.va_end);
                if (!used)
                    return false;
                n += used;
                continue;
            }
            if (depth_target_span(method, last)) {
                if (op != NVB06F_DMA_INCR_OPCODE_VALUE || method != NVB197_SET_ZT_A || count != 5)
                    return false;
                const size_t used = depth_target(words + n, word_count - n, va, &stream.va_end);
                if (!used)
                    return false;
                n += used;
                continue;
            }
            if (method <= NVB197_SET_ZT_SELECT && last >= NVB197_SET_ZT_SELECT &&
                (op != NVB06F_DMA_INCR_OPCODE_VALUE || method != NVB197_SET_ZT_SELECT || count != 1 || words[n]))
                return false;
            if (cbuf_data_span(method, last)) {
                if (op == NVB06F_DMA_INCR_OPCODE_VALUE && method == NVB197_SET_CONSTANT_BUFFER_SELECTOR_A && count == 3) {
                    const size_t used = direct_cbuf_binding(words + n, word_count - n, va, &stream.va_end);
                    if (!used)
                        return false;
                    n += used;
                    continue;
                }
                if (op != NVB06F_DMA_ONEINCR_OPCODE_VALUE || method != NVB197_LOAD_CONSTANT_BUFFER_OFFSET ||
                    count < 2 || (words[n] & 3) || words[n] >= TG_GPU_NVK_DRAW_CB0_BYTES ||
                    count - 1 > (TG_GPU_NVK_DRAW_CB0_BYTES - words[n]) / 4)
                    return false;
                stream.requires_cb0 |= !stream.cb0_end;
            }
            const uint32_t group = cbuf_group_in_span(method, last);
            if (group) {
                if (op != NVB06F_DMA_INCR_OPCODE_VALUE || method != group || count != 1 ||
                    !cbuf_binding_value(words[n], false))
                    return false;
                stream.requires_cb0 |= (words[n] & DRF_SMASK(NVB197_BIND_GROUP_CONSTANT_BUFFER_VALID)) && !stream.cb0_end;
            }
            if (cb0_scratch_span(method, last)) {
                if (!stream.uploads_mme || cb0_setup_end ||
                    op != NVB06F_DMA_INCR_OPCODE_VALUE || method != TG_GPU_NVK_MME_CB0_ADDR_HI || count != 2 ||
                    !word_span(va, words[n], words[n + 1], TG_GPU_NVK_DRAW_CB0_BYTES, 256, &cb0_setup_end))
                    return false;
                if (cb0_setup_end > stream.va_end)
                    stream.va_end = cb0_setup_end;
            }
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
            const uint32_t slot = (method - NVB197_CALL_MME_MACRO(0)) / 8;
            if (slot == NVK_MME_SELECT_CB0 && cb0_setup_end)
                stream.cb0_end = cb0_setup_end;
            if (slot == NVK_MME_SELECT_CB0 || slot == NVK_MME_SET_ANTI_ALIAS ||
                slot == NVK_MME_DRAW || slot == NVK_MME_DRAW_INDEXED)
                stream.requires_cb0 |= !stream.cb0_end;
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
    if (cb0_setup_end && !stream.cb0_end)
        return false;
    if (info)
        *info = stream;
    return true;
}
