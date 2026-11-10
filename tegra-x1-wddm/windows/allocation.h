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

/* Private data for the fixed-size standard primary and shadow. The NVK buffer ABI
 * continues to use only tg_gpu_allocation_request.
 */
typedef struct _TG_GPU_PRIMARY_ALLOCATION_REQUEST {
    struct tg_gpu_allocation_request Buffer;
    D3DDDIFORMAT Format;
    D3DDDI_RATIONAL RefreshRate;
    UINT Reserved;
} TG_GPU_PRIMARY_ALLOCATION_REQUEST;

void TgGpuRegisterAllocationDdis(DRIVER_INITIALIZATION_DATA *);
/* OS-supplied KMD handle, held alive by VidMm for this paging call. */
NTSTATUS TgGpuValidatePagingAllocation(TG_GPU_CONTEXT_ENGINE *,HANDLE,UINT64,UINT64);
DXGKDDI_CREATEALLOCATION TgGpuDdiCreateAllocation;
DXGKDDI_DESTROYALLOCATION TgGpuDdiDestroyAllocation;
DXGKDDI_OPENALLOCATIONINFO TgGpuDdiOpenAllocation;
DXGKDDI_CLOSEALLOCATION TgGpuDdiCloseAllocation;
DXGKDDI_GETSTANDARDALLOCATIONDRIVERDATA TgGpuDdiGetStandardAllocationDriverData;
/* Check a device-specific open handle supplied by Dxgkrnl, never a UMD raw
 * pointer. This does not prove residency, pin pages or validate GPU commands.
 * The caller must hold the open object's lifetime through this operation.
 */
NTSTATUS TgGpuValidateOpenAllocationRange(TG_GPU_DEVICE *,HANDLE,
                                         UINT64 Offset,UINT64 Bytes,BOOLEAN Write);
NTSTATUS TgGpuGetPresentSurface(TG_GPU_DEVICE *,HANDLE,BOOLEAN,
                                TG_GPU_PRIMARY_ALLOCATION_REQUEST *);
#endif
