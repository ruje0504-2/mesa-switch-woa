/* SPDX-License-Identifier: MIT */
#ifndef TEGRA_GPU_CHANNEL_H
#define TEGRA_GPU_CHANNEL_H
#include "../platform/t214_power.h"
#include "../platform/t214_mmu.h"
#include "../platform/t214_frontend.h"

enum tg_gpu_channel_result {
    TG_GPU_CHANNEL_OK, TG_GPU_CHANNEL_ARGUMENT, TG_GPU_CHANNEL_NOT_READY,
    TG_GPU_CHANNEL_BUSY, TG_GPU_CHANNEL_TLB, TG_GPU_CHANNEL_IO,
    TG_GPU_CHANNEL_TIMEOUT, TG_GPU_CHANNEL_FAULT, TG_GPU_CHANNEL_FLUSH
};
struct tg_gpu_channel {
    const void *owner;
    const struct tg_gpu_power *power;
    uint64_t root;
    bool referenced, submitted;
    uint32_t stage, last_register, last_value;
    uint32_t fifo_interrupt, mmu_fault, pbdma_interrupt, pbdma_interrupt_1;
    struct tg_gpu_frontend_status frontend;
};
/* Hot channel path after exclusive cold GPU/FIFO/LTC/MMU initialization and
 * clock setup. One bare channel/runlist/PBDMA (zero). Command and RAMIN bytes
 * must already be published; caller owns CPU/MMIO ordering and residency.
 * Both GP_PUT entries are preloaded, so no BAR1 user doorbell is required.
 */
enum tg_gpu_channel_result tg_gpu_channel_submit(const struct tg_gpu_power_io *,
    const struct tg_gpu_power *, uint64_t root, uint64_t instance, uint64_t runlist,
    const void *owner, struct tg_gpu_channel *);
/* Success means preempt/runlist/unbind/FB flush/TLB invalidation finished.
 * Failure retains referenced/owner: do not recycle any backing until retry
 * succeeds or the owning GPU has been drained and fully reset/powered down.
 */
enum tg_gpu_channel_result tg_gpu_channel_retire(const struct tg_gpu_power_io *,
    const struct tg_gpu_power *, const void *owner, struct tg_gpu_channel *);
/* Recovery precursor, not a release proof. Follow with MC drain and the
 * platform's full reset/power-down before dropping referenced/owner.
 */
void tg_gpu_channel_disable(const struct tg_gpu_power_io *, const struct tg_gpu_power *);
#endif
