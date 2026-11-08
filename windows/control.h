/* SPDX-License-Identifier: MIT */
#ifndef TEGRA_GPU_CONTROL_H
#define TEGRA_GPU_CONTROL_H
#include <stdint.h>

/* Shared by the kernel backend and the existing LitePerformanceCheck UI.
 * METHOD_BUFFERED, FILE_DEVICE_UNKNOWN. RUN requires write access; the
 * device grants write access only to administrators and SYSTEM.
 */
#define TG_GPU_CONTROL_VERSION 4u
#define TG_GPU_IOCTL_STATUS 0x00226400u
#define TG_GPU_IOCTL_RUN    0x0022a404u
#define TG_GPU_CONTROL_DEFAULT_KHZ 729600u
#define TG_GPU_CONTROL_OC_KHZ 1228800u

/* Live PBDMA RAMFC registers are meaningful only for a valid, matching bare
 * channel. Idle/context-switch readouts can contain unrelated SRAM contents.
 * FIFO_PBDMA_STATUS fields follow Switchroot hw_fifo_gm20b.h.
 */
static inline int tg_gpu_pbdma_owns_channel(uint32_t status, uint32_t channel)
{
    return channel < 0x1000 && ((status >> 13) & 7) == 1 &&
        (status & 0x1fff) == channel;
}

struct tg_gpu_request {
    uint32_t version;
    uint32_t target_khz;
};

struct tg_gpu_status {
    uint32_t version;
    uint32_t tested;
    uint32_t target_khz;
    uint32_t measured_khz;
    uint32_t rail_uv;
    uint32_t boot0;
    uint32_t stage;
    uint32_t power_result;
    uint32_t clock_result;
    uint32_t execution_result;
    uint32_t restore_result;
    uint32_t resources_owned;
    uint32_t expected[2];
    uint32_t observed[2];
    uint32_t fifo_interrupt;
    uint32_t mmu_fault;
    uint32_t pbdma_interrupt;
    uint32_t channel_status;
    uint32_t mc_enable_before;
    uint32_t channel_instance_before;
    uint32_t mc_enable_after;
    uint32_t channel_instance_after;
    uint32_t pbdma_enable_before;
    uint32_t pbdma_enable_after;
    uint32_t pbdma_runlist_map;
    uint32_t pbdma_status;
    uint32_t userd_gp_get;
    uint32_t userd_gp_put;
    uint32_t scheduler_before;
    uint32_t scheduler_after;
    uint32_t scheduler_error;
    uint32_t runlist_base;
    uint32_t runlist_status;
    uint32_t pbdma_gp_base;
    uint32_t pbdma_gp_fetch;
    uint32_t pbdma_gp_get;
    uint32_t pbdma_gp_put;
    uint32_t pbdma_interrupt_1;
};
#endif
