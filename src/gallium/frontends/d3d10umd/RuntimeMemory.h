/* SPDX-License-Identifier: MIT */
#ifndef D3D10_RUNTIME_MEMORY_H
#define D3D10_RUNTIME_MEMORY_H

#include "DriverIncludes.h"
#include "vulkan/util/vk_wsi_win32_memory.h"

struct RuntimePrimary;
struct Device;
struct Resource;
HRESULT RuntimeDeviceCreateContext(Device *device);
HRESULT RuntimeDeviceDestroyContext(Device *device);
HRESULT RuntimeDeviceFinish(Device *device);
HRESULT RuntimePrimaryCreate(Device *device, const D3D10DDIARG_CREATERESOURCE *desc,
                              D3D10DDI_HRTRESOURCE runtime, RuntimePrimary **out);
HRESULT RuntimePrimaryDestroy(RuntimePrimary *primary);
HRESULT RuntimePrimarySetMode(RuntimePrimary *primary, HANDLE device, UINT subresource);
HRESULT RuntimePrimaryError(RuntimePrimary *primary);
HRESULT RuntimePrimaryFinish(Device *device, Resource *resource);
const wsi_win32_memory *RuntimePrimaryMemory(RuntimePrimary *primary);

#endif
