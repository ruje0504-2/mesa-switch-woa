/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_T214_GR_CONTEXT_H
#define TG_GPU_T214_GR_CONTEXT_H
#include "t214_gr.h"

/* A second, caller-owned uncached allocation, separate from the accepted FIFO
 * arena. Only the exclusive cold diagnostic maps it. Never accept these
 * addresses or commands from an application. Retain it through reset/MC drain.
 */
#define TG_GPU_GR_WORK_BYTES 0x100000u
#define TG_GPU_GR_WORK_VA 0x100000u
#define TG_GPU_GR_CTX_OFFSET 0u
#define TG_GPU_GR_GOLD_OFFSET 0x20000u
#define TG_GPU_GR_BUNDLE_OFFSET 0x40000u
#define TG_GPU_GR_PAGEPOOL_OFFSET 0x42000u
#define TG_GPU_GR_ATTRIB_OFFSET 0x50000u
#define TG_GPU_GR_CTX_CAPACITY 0x20000u
#define TG_GPU_GR_GOLD_SENTINEL 0xa5a5a5a5u
struct tg_gpu_gr_work {
    void *cpu;
    uint64_t physical;
    size_t bytes;
};
enum tg_gpu_gr_context_result {
    TG_GPU_GR_CTX_OK, TG_GPU_GR_CTX_ARGUMENT, TG_GPU_GR_CTX_FORMAT,
    TG_GPU_GR_CTX_TOPOLOGY, TG_GPU_GR_CTX_REGISTER, TG_GPU_GR_CTX_TIMEOUT,
    TG_GPU_GR_CTX_FECS_REJECTED, TG_GPU_GR_CTX_VISIBILITY,
    TG_GPU_GR_CTX_IMAGE, TG_GPU_GR_CTX_PRI_FAULT
};
struct tg_gpu_gr_context_status {
    uint32_t stage, result, context_bytes, gpcs, tpcs, zculls;
    uint32_t last_register, last_value, bind_mailbox, save_mailbox;
    uint32_t image_magic, image_changed_words, image_hash;
};
/* Install the owned work pages into the diagnostic leaf BEFORE BAR1/TLB bind.
 * Zero the normal context and put an independent sentinel in the golden image.
 */
bool tg_gpu_gr_context_prepare(void *arena, uint64_t arena_physical,
    size_t arena_bytes, const struct tg_gpu_gr_work *work);
/* FECS and GPC-MMU must already be ready; no runnable channel may exist.
 * Save the hardware's initialized register state, require a FECS success reply,
 * flush L2, and inspect the GPU-written image. This is not shader execution.
 */
enum tg_gpu_gr_context_result tg_gpu_gr_context_init(
    const struct tg_gpu_power_io *io, const struct tg_gpu_power *power,
    struct tg_gpu_frontend_status *frontend, const struct tg_gpu_gr_netlist *netlist,
    struct tg_gpu_gr_status *gr, uint32_t *instance, uint64_t instance_physical,
    const struct tg_gpu_gr_work *work, struct tg_gpu_gr_context_status *status);
#endif
