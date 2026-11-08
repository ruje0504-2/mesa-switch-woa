/* SPDX-License-Identifier: MIT */
#ifndef TEGRA_GPU_SUBMISSION_H
#define TEGRA_GPU_SUBMISSION_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TG_GPU_COMMAND_BYTES 0x4000u
#define TG_GPU_COMMAND_RING 0u
#define TG_GPU_COMMAND_PUSH 0x1000u
#define TG_GPU_COMMAND_USERD 0x2000u
#define TG_GPU_COMMAND_FENCE 0x2800u
#define TG_GPU_COMMAND_RUNLIST 0x3000u
/* A driver-owned, privileged three-page VA mapping plus a physical runlist.
 * The application's packet stays USER; only the generated completion packet
 * is KERNEL. Caller owns all backing and must quiesce the hardware slot before
 * preparing it. RAMIN PDB words are preserved, not exposed in the mapped pages.
 */
bool tg_gpu_prepare_submission(void *commands, size_t bytes, uint64_t physical,
    uint64_t internal_va, void *instance, size_t instance_bytes,
    uint64_t instance_physical, uint64_t application_va, uint32_t application_bytes,
    uint32_t fence_value);
#endif
