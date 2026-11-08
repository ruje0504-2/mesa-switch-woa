/* SPDX-License-Identifier: MIT */
#ifndef TEGRA_GPU_ALLOCATION_H
#define TEGRA_GPU_ALLOCATION_H
#include "context.h"
#include "allocation_abi.h"

/* This adapter has one explicit segment (page tables). VidMm appends its
 * implicit system-memory segment at ID 2, as in Microsoft's GPUVA sample.
 * These callbacks allocate metadata only; VidMm owns pages and GPU VA.
 */
#define TG_GPU_EXPLICIT_TABLE_SEGMENT 1u
#define TG_GPU_IMPLICIT_SYSTEM_SEGMENT 2u

void TgGpuRegisterAllocationDdis(DRIVER_INITIALIZATION_DATA *);
DXGKDDI_CREATEALLOCATION TgGpuDdiCreateAllocation;
DXGKDDI_DESTROYALLOCATION TgGpuDdiDestroyAllocation;
DXGKDDI_OPENALLOCATIONINFO TgGpuDdiOpenAllocation;
DXGKDDI_CLOSEALLOCATION TgGpuDdiCloseAllocation;
/* Check a device-specific open handle supplied by Dxgkrnl, never a UMD raw
 * pointer. This does not prove residency, pin pages or validate GPU commands.
 * The caller must hold the open object's lifetime through this operation.
 */
NTSTATUS TgGpuValidateOpenAllocationRange(TG_GPU_DEVICE *,HANDLE,
                                         UINT64 Offset,UINT64 Bytes,BOOLEAN Write);
#endif
