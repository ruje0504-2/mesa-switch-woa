/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_T214_GR_H
#define TG_GPU_T214_GR_H
#include "t214_power.h"
#include "t214_frontend.h"
#include "../core/gr_firmware.h"
enum tg_gpu_gr_result {
    TG_GPU_GR_OK, TG_GPU_GR_ARGUMENT, TG_GPU_GR_SECURITY_REQUIRED,
    TG_GPU_GR_REGISTER, TG_GPU_GR_SCRUB_TIMEOUT, TG_GPU_GR_ARB_TIMEOUT,
    TG_GPU_GR_DMA_TIMEOUT, TG_GPU_GR_READY_TIMEOUT, TG_GPU_GR_PRI_FAULT,
    TG_GPU_GR_FIRMWARE_FORMAT, TG_GPU_GR_IDLE_TIMEOUT
};
struct tg_gpu_gr_status {
    uint32_t stage, result, priv_security, last_register, last_value;
    uint32_t fecs_hwcfg, gpccs_hwcfg, fecs_mailbox, gr_interrupt;
};
/* Non-secure Switchroot DMA bootstrap. Caller owns a cold GR with its reset/
 * access preamble complete, a valid instance/PDB and an immutable image mapped
 * read-only at gpu_va. No runnable channels may exist. This call never changes
 * security fuses, starts a secure Falcon or frees DMA memory. On any return,
 * buffers stay owned until the caller drains/resets/powers down the whole GPU.
 * A firmware-ready handshake is not a graphics context or shader-execution pass.
 */
enum tg_gpu_gr_result tg_gpu_gr_boot(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power, struct tg_gpu_frontend_status *frontend,
    const struct tg_gpu_gr_netlist *netlist, const struct tg_gpu_gr_image *image,
    uint64_t gpu_va, uint64_t instance_physical, struct tg_gpu_gr_status *status);
#endif
