/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_COMMAND_STREAM_H
#define TG_GPU_COMMAND_STREAM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct tg_gpu_command_va {
    uint64_t limit;
    uint64_t reserved_base, reserved_end;
};

enum tg_gpu_program_engine {
    TG_GPU_PROGRAM_3D,
    TG_GPU_PROGRAM_COMPUTE,
    TG_GPU_PROGRAM_ENGINES,
};

enum tg_gpu_texture_pool {
    TG_GPU_TEXTURE_HEADERS,
    TG_GPU_TEXTURE_SAMPLERS,
    TG_GPU_TEXTURE_POOLS,
};

struct tg_gpu_texture_pool_range {
    uint64_t base, end;
};

struct tg_gpu_compute_slm {
    uint64_t bytes_per_tpc;
    uint64_t va_end;
};

struct tg_gpu_command_info {
    uint64_t va_end;
    uint64_t reserved_base;
    uint64_t program_end[TG_GPU_PROGRAM_ENGINES];
    struct tg_gpu_texture_pool_range texture_pool[TG_GPU_PROGRAM_ENGINES][TG_GPU_TEXTURE_POOLS];
    bool requires_texture_pool[TG_GPU_PROGRAM_ENGINES][TG_GPU_TEXTURE_POOLS];
    struct tg_gpu_compute_slm compute_slm;
    uint64_t required_compute_slm_per_tpc; /* Before the first binding in this stream. */
    uint32_t requires_program; /* Engines using a previously set program region. */
    bool uploads_mme;
    bool requires_mme; /* A call precedes any upload in this stream. */
    bool requires_cb0; /* Uses the CB0 selected by a preceding submission. */
    uint64_t cb0_end; /* An initialized CB0 to publish on retirement. */
};

/* Decode the complete linear B06F stream. Reject control-flow/subdevice
 * headers, reserved bits, overruns and host methods other than object binding,
 * WFI and bounded semaphore acquires. Bindings select the GM20B NVK class.
 * USER code cannot write the kernel Reference marker.
 * MME RAM writes must match the NVK Windows GM20B initialization sequence.
 * MME calls require a complete parameter packet for a supported slot and
 * bounded register indices. Copy launches use virtual addresses and supply
 * their operands in the stream. Inline uploads and reports also use complete
 * packets. B197 color and depth attachments supply complete layout state.
 * Optional process bounds exclude KMD memory from these accesses.
 */
bool tg_gpu_inspect_command_stream(const uint32_t *, size_t bytes,
                                   const struct tg_gpu_command_va *,
                                   struct tg_gpu_command_info *);
/* Prepare the KMD-owned snapshot for execution, including direct QMDs. */
bool tg_gpu_prepare_command_stream(uint32_t *, size_t bytes,
                                   const struct tg_gpu_command_va *,
                                   struct tg_gpu_command_info *);
/* Visit every pool bind in a prepared immutable snapshot, including earlier
 * binds to the same engine. Payload words are never treated as headers.
 */
typedef bool (*tg_gpu_texture_pool_check)(void *, uint64_t base, uint64_t end);
bool tg_gpu_check_texture_pool_ranges(const uint32_t *, size_t bytes,
                                      tg_gpu_texture_pool_check, void *);
bool tg_gpu_command_range_allowed(const struct tg_gpu_command_va *, uint64_t address,
                                  uint64_t bytes, uint64_t *max_end);

static inline bool tg_gpu_validate_command_stream(const uint32_t *words, size_t bytes)
{
    return tg_gpu_inspect_command_stream(words, bytes, NULL, NULL);
}
#endif
