/* SPDX-License-Identifier: MIT */
#ifndef TEGRA_GPU_EXECUTE_H
#define TEGRA_GPU_EXECUTE_H
#include "../platform/t214_power.h"
#include "../windows/control.h"
#include <stddef.h>

#define TG_GPU_ARENA_BYTES 0x80000u
#define TG_GPU_GR_SURFACE_OFFSET 0x50000u
#define TG_GPU_GR_SURFACE_BYTES 0x10000u
#define TG_GPU_GR_SURFACE_VA 0x20000u
#define TG_GPU_GR_INSTANCE_OFFSET 0x49000u
#define TG_GPU_ACR_INSTANCE_OFFSET 0x4a000u
#define TG_GPU_ACR_SURFACE_OFFSET 0x60000u
#define TG_GPU_ACR_SURFACE_BYTES 0x20000u
#define TG_GPU_ACR_SURFACE_VA 0x40000u
enum tg_gpu_execute_result {
    TG_GPU_EXEC_OK, TG_GPU_EXEC_ARGUMENT, TG_GPU_EXEC_BUSY,
    TG_GPU_EXEC_MMU_TIMEOUT, TG_GPU_EXEC_BAR1, TG_GPU_EXEC_RUNLIST,
    TG_GPU_EXEC_TIMEOUT, TG_GPU_EXEC_FAULT, TG_GPU_EXEC_FIFO_INIT,
    TG_GPU_EXEC_PBDMA_INIT, TG_GPU_EXEC_SCHEDULER, TG_GPU_EXEC_FRONTEND,
    TG_GPU_EXEC_GR_BOOT, TG_GPU_EXEC_ACR_BOOT, TG_GPU_EXEC_PMU_BOOT
};

/* A single diagnostic channel using caller-owned contiguous, uncached
 * memory below 4 GiB. Commands release two PBDMA semaphores; this proves
 * FIFO DMA execution, not GR firmware, shader execution or Vulkan drawing.
 * No user-provided command stream or physical address is accepted.
 */
struct tg_gpu_execution {
    bool owned;
    bool fifo_ready;
    bool pbdma_owned;
    bool gr_owned;
    bool acr_owned;
    uint32_t old_pbdma_enable;
    uint32_t ce_reset_mask;
    uint32_t stage;
};

bool tg_gpu_prepare_arena(void *cpu, uint64_t device_address, size_t bytes,
                          const uint32_t expected[2]);
/* Called only after prepare_arena, before any GPU mapping becomes live. */
bool tg_gpu_prepare_gr_arena(void *cpu, uint64_t device_address, size_t bytes,
                            const struct tg_gpu_gr_payload *firmware);
bool tg_gpu_prepare_acr_arena(void *cpu, uint64_t device_address, size_t bytes);
enum tg_gpu_execute_result tg_gpu_execute(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power, void *uncached_cpu,
    uint64_t device_address, const struct tg_gpu_gr_payload *firmware,
    struct tg_gpu_execution *state,
    struct tg_gpu_status *result);
/* Stop channel fetch before the caller drains MC, stops the PLL and powers
 * down. Clear execution ownership/free the arena only AFTER power_down
 * confirms no ownership. The probe does not change any Tegra SMMU domain.
 */
void tg_gpu_execute_stop(const struct tg_gpu_power_io *io, struct tg_gpu_execution *state);
void tg_gpu_execute_snapshot(const struct tg_gpu_power_io *io,
    const struct tg_gpu_execution *state, struct tg_gpu_status *result);
#endif
