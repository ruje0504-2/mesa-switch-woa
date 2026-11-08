/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_GR_FIRMWARE_IO_H
#define TG_GPU_GR_FIRMWARE_IO_H
#include <ntddk.h>
#include "../core/gr_firmware.h"
typedef struct TG_GPU_FIRMWARE {
    struct tg_gpu_gr_payload payload;
    PVOID netlist, surface;
} TG_GPU_FIRMWARE;
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS TgGpuFirmwareLoad(TG_GPU_FIRMWARE *firmware);
_IRQL_requires_(PASSIVE_LEVEL)
VOID TgGpuFirmwareFree(TG_GPU_FIRMWARE *firmware);
#endif
