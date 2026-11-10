/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_COMMAND_STREAM_H
#define TG_GPU_COMMAND_STREAM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct tg_gpu_command_info {
    bool uploads_mme;
    bool requires_mme; /* A call precedes any upload in this stream. */
};

/* Decode the complete linear B06F stream. Reject control-flow/subdevice
 * headers, reserved bits, overruns and host methods other than object binding
 * and WFI. Bindings must select the GM20B class for the NVK subchannel.
 * USER code cannot write the kernel Reference marker.
 * MME RAM writes must match the NVK Windows GM20B initialization sequence.
 * MME calls require a complete parameter packet for a supported slot and
 * bounded register indices. Class-specific address/privilege/residency
 * validation is separate.
 */
bool tg_gpu_inspect_command_stream(const uint32_t *, size_t bytes,
                                   struct tg_gpu_command_info *);

static inline bool tg_gpu_validate_command_stream(const uint32_t *words, size_t bytes)
{
    return tg_gpu_inspect_command_stream(words, bytes, NULL);
}
#endif
