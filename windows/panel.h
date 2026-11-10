/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_PANEL_H
#define TG_GPU_PANEL_H

#include <ntddk.h>
#include <dispmprt.h>
#include "../platform/t214_dc.h"

typedef struct _TG_GPU_PANEL {
    struct tg_t214_dc Dc;
    DXGK_DISPLAY_INFORMATION Boot;
} TG_GPU_PANEL;

struct _TG_GPU_ADAPTER;
extern const D3DKMDT_VIDEO_SIGNAL_INFO TgGpuPanelSignal;
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS TgGpuPanelStart(struct _TG_GPU_ADAPTER *adapter);
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS TgGpuPanelStop(struct _TG_GPU_ADAPTER *adapter);
void TgGpuRegisterPanelLifecycle(DRIVER_INITIALIZATION_DATA *ddis);
void TgGpuRegisterPanelVidPn(DRIVER_INITIALIZATION_DATA *ddis);

#endif
