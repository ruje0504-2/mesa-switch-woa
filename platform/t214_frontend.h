/* SPDX-License-Identifier: MIT */
#ifndef TEGRA_GPU_T214_FRONTEND_H
#define TEGRA_GPU_T214_FRONTEND_H
#include <stdint.h>

struct tg_gpu_power_io;
struct tg_gpu_power;
enum tg_gpu_frontend_result {
    TG_GPU_FE_OK, TG_GPU_FE_ARGUMENT, TG_GPU_FE_REGISTER,
    TG_GPU_FE_LTC, TG_GPU_FE_FLUSH_TIMEOUT, TG_GPU_FE_PRI_FAULT,
    TG_GPU_FE_L2_TIMEOUT
};
/* Fixed-size diagnostic appended to the control ABI; overwritten per run. */
struct tg_gpu_frontend_status {
    uint32_t stage, result, ltc_count, fb_flushes;
    uint32_t bus_interrupt, pri_status0, pri_status1;
    uint32_t pri_error_address, pri_error_code;
    uint32_t l2_flushes, l2_invalidations;
};

/* Cold, exclusively owned GPU only. Order: start -> GPC clock -> FIFO reset
 * -> fifo -> memory -> MMU/BAR1 binding -> flush -> runnable channel.
 * These controls are retired by the caller's full GPU reset/power-down.
 */
enum tg_gpu_frontend_result tg_gpu_frontend_start(const struct tg_gpu_power_io *,
    const struct tg_gpu_power *, struct tg_gpu_frontend_status *);
enum tg_gpu_frontend_result tg_gpu_frontend_fifo(const struct tg_gpu_power_io *,
    const struct tg_gpu_power *, struct tg_gpu_frontend_status *);
enum tg_gpu_frontend_result tg_gpu_frontend_memory(const struct tg_gpu_power_io *,
    const struct tg_gpu_power *, struct tg_gpu_frontend_status *);
enum tg_gpu_frontend_result tg_gpu_frontend_flush(const struct tg_gpu_power_io *,
    const struct tg_gpu_power *, struct tg_gpu_frontend_status *);
/* FB flush only orders writes into L2. Drain dirty lines to DRAM, then
 * invalidate clean lines before comparing CPU and BAR1 aliases. The caller
 * serializes these operations and owns the entire GPU during this probe.
 */
enum tg_gpu_frontend_result tg_gpu_frontend_sync_memory(const struct tg_gpu_power_io *,
    const struct tg_gpu_power *, struct tg_gpu_frontend_status *);
void tg_gpu_frontend_snapshot(const struct tg_gpu_power_io *,
    struct tg_gpu_frontend_status *);
#endif
