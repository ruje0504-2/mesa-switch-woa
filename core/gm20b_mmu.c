/* SPDX-License-Identifier: MIT */
/* GM20B entry formats adapted from Linux nouveau v6.12:
 * vmmgf100.c, vmmgk20a.c, vmmgm200.c and vmmgm20b.c.
 * Copyright 2017 Red Hat Inc. See ../NOTICE for the upstream MIT notice.
 * Copyright 2026 ruje0504-2
 */
#include "gm20b_mmu.h"

static bool
page_address_valid(uint64_t address)
{
    return address < TG_GPU_VA_LIMIT && (address & 0xfffu) == 0;
}

bool
tg_gpu_va_indices(uint64_t va, struct tg_gpu_va_indices *indices)
{
    if (!indices || !page_address_valid(va))
        return false;
    indices->directory = (uint32_t)(va >> TG_GPU_DIRECTORY_SHIFT);
    indices->small_page = (uint32_t)((va >> TG_GPU_PAGE_SHIFT) & 0x3fffu);
    return true;
}

bool
tg_gpu_pte_4k(uint64_t gpu_address, bool read_only, bool gpu_uncached,
              uint64_t *pte)
{
    if (!pte || !page_address_valid(gpu_address))
        return false;

    /* gk20a_vmm_aper(NCOH) returns 0 on GM20B. Using the discrete-GPU
     * SYS_NCOH aperture (3 << 33) here would address the wrong aperture.
     * Pitch-linear kind 0, valid, unprivileged; VOL bypasses the GPU cache.
     */
    *pte = (gpu_address >> 8) | UINT64_C(1) |
           ((uint64_t)read_only << 2) | ((uint64_t)gpu_uncached << 32);
    return true;
}

bool
tg_gpu_pde_small(uint64_t table_gpu_address, uint64_t *pde)
{
    if (!pde || !page_address_valid(table_gpu_address))
        return false;

    /* gf100_vmm_pgd_pde uses NCOH=3 in the upper (small-page) half.
     * The lower half remains absent so the SPT is selected.
     */
    *pde = (table_gpu_address << 24) | (UINT64_C(3) << 32);
    return true;
}

bool
tg_gpu_instance_pdb(uint64_t directory_gpu_address, uint64_t *pdb)
{
    if (!pdb || !page_address_valid(directory_gpu_address))
        return false;

    /* RAMIN +0x200; gm200_vmm_join_ selects 64 KiB big pages with bit 11.
     * RAMIN +0x208 must separately contain the inclusive VA limit.
     */
    *pdb = directory_gpu_address | UINT64_C(0x803);
    return true;
}
