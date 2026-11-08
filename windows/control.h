/* SPDX-License-Identifier: MIT */
#ifndef TEGRA_GPU_CONTROL_H
#define TEGRA_GPU_CONTROL_H
#include <stdint.h>
#include "../platform/t214_frontend.h"
#include "../platform/t214_gr.h"
#include "../platform/t214_acr.h"

/* Shared by the kernel backend and the existing LitePerformanceCheck UI.
 * METHOD_BUFFERED, FILE_DEVICE_UNKNOWN. RUN requires write access; the
 * device grants write access only to administrators and SYSTEM.
 */
#define TG_GPU_CONTROL_VERSION 16u
#define TG_GPU_IOCTL_STATUS 0x00226400u
#define TG_GPU_IOCTL_RUN    0x0022a404u
#define TG_GPU_CONTROL_DEFAULT_KHZ 729600u
#define TG_GPU_CONTROL_OC_KHZ 1228800u

/* Switchroot FIFO_INTR_0: bind, scheduler, channel switch, FB flush, LB,
 * dropped MMU, MMU and PBDMA faults. Runlist/channel events are not faults.
 */
#define TG_GPU_FIFO_ERROR_MASK 0x39810101u
static inline int tg_gpu_fifo_has_error(uint32_t status)
{ return (status & TG_GPU_FIFO_ERROR_MASK) != 0; }

/* Live PBDMA RAMFC registers are meaningful only for a valid, matching
 * channel/group. Idle/context-switch readouts contain unrelated SRAM contents.
 * FIFO_PBDMA_STATUS fields follow Switchroot hw_fifo_gm20b.h.
 */
static inline int tg_gpu_pbdma_owns_channel(uint32_t status, uint32_t channel)
{
    return channel < 0x1000 && ((status >> 13) & 7) == 1 &&
        (status & 0x1fff) == channel;
}

/* This private slot has exactly one channel (0) in exactly one TSG (0).
 * A valid matching TSG is therefore sufficient to attribute its RAMFC here.
 * This must not be used for a group containing several channels.
 */
static inline int tg_gpu_pbdma_owns_slot(uint32_t status)
{
    return ((status >> 13) & 7) == 1 && (status & 0xfff) == 0;
}

struct tg_gpu_request {
    uint32_t version;
    uint32_t target_khz;
};

/* Fault-time PBDMA0 context. Context words are sampled only while our single
 * channel/TSG owns the slot; they do not authorize exception suppression.
 */
struct tg_gpu_pbdma_context {
    uint32_t intr_enable, intr_stall, fifo_pending;
    uint32_t target, signature, method0, data0, pb_header, header_shadow, subdevice;
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
    struct tg_gpu_frontend_status frontend;
    uint32_t bind_error;
    uint32_t chsw_error;
    uint32_t engine_status[2];
    /* BAR1 check phase: initial read, posted write, DRAM sync, zero restore,
     * ready. These scratch values are not GPU execution fence results.
     */
    uint32_t bar1_phase, bar1_expected, bar1_read, bar1_cpu;
    struct tg_gpu_gr_status gr;
    uint32_t firmware_status; /* NTSTATUS from fixed-path file loading/packing */
    uint32_t acr_firmware_result; /* tg_gpu_acr_result; not hardware authentication */
    struct tg_gpu_acr_hw_status acr;
    struct tg_gpu_pmu_status pmu;
    struct tg_gpu_pbdma_context pbdma_context;
};
#endif
