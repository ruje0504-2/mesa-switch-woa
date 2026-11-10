/* SPDX-License-Identifier: MIT */
#include <windows.h>
#include <d3d9.h>
#include "winddk_compat.h"
#include <d3dumddi.h>

#include "nouveau/vulkan/nvkmd/windows/adapter_abi.h"
#include "zink/zink_public.h"

extern HRESULT
d3d10_create_screen(HANDLE hRTAdapter, const D3DDDI_ADAPTERCALLBACKS *callbacks,
                    struct pipe_screen **screen);

HRESULT
d3d10_create_screen(HANDLE hRTAdapter, const D3DDDI_ADAPTERCALLBACKS *callbacks,
                    struct pipe_screen **screen)
{
   *screen = NULL;
   if (!callbacks || !callbacks->pfnQueryAdapterInfoCb)
      return E_INVALIDARG;

   struct tg_gpu_adapter_info info = {
      .version = TG_GPU_ADAPTER_ABI,
      .size = sizeof(info),
   };
   D3DDDICB_QUERYADAPTERINFO query = {
      .pPrivateDriverData = &info,
      .PrivateDriverDataSize = sizeof(info),
   };
   HRESULT hr = callbacks->pfnQueryAdapterInfoCb(hRTAdapter, &query);
   if (FAILED(hr))
      return hr;

   const uint32_t required = TG_GPU_ADAPTER_LUID_VALID | TG_GPU_ADAPTER_NATIVE_SUBMIT;
   if (info.version != TG_GPU_ADAPTER_ABI || info.size != sizeof(info) ||
       info.reserved || (info.flags & required) != required ||
       info.vendor != 0x10de || info.chipset != 0x12e ||
       !info.memory_budget_bytes || info.memory_budget_bytes > TG_GPU_MEMORY_BUDGET_BYTES)
      return DXGI_ERROR_UNSUPPORTED;

   const uint64_t luid = (uint64_t)(uint32_t)info.luid_high << 32 | info.luid_low;
   /* Zero is Zink's automatic-selection sentinel, not an adapter identity. */
   if (!luid)
      return DXGI_ERROR_UNSUPPORTED;

   *screen = zink_win32_create_screen(luid);
   return *screen ? S_OK : DXGI_ERROR_UNSUPPORTED;
}
