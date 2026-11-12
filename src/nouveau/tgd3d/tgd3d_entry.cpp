/* SPDX-License-Identifier: MIT
 *
 * OpenAdapter entry points exported to the Direct3D runtime.
 */
#include "tgd3d.h"

#include <string.h>

using namespace tgd3d;

static HRESULT APIENTRY CloseAdapter(D3D10DDI_HADAPTER hAdapter)
{
   Adapter *adapter = static_cast<Adapter *>(hAdapter.pDrvPrivate);
   adapter->vk.close();
   delete adapter;
   return S_OK;
}

/* Only what is implemented is listed: the Direct3D 10.0 interface in its
 * three revisions. Direct3D 10.1 and 11 device tables are not provided. */
static const UINT64 supported_versions[] = {
   D3D10_0_DDI_SUPPORTED,
   D3D10_0_x_DDI_SUPPORTED,
   D3D10_0_7_DDI_SUPPORTED,
};

static HRESULT APIENTRY GetSupportedVersions(D3D10DDI_HADAPTER, UINT32 *count, UINT64 *versions)
{
   const UINT32 available = sizeof(supported_versions) / sizeof(supported_versions[0]);
   if (versions && *count < available)
      return E_OUTOFMEMORY;
   *count = available;
   if (versions)
      memcpy(versions, supported_versions, sizeof(supported_versions));
   return S_OK;
}

static HRESULT APIENTRY GetCaps(D3D10DDI_HADAPTER, const D3D10_2DDIARG_GETCAPS *args)
{
   /* No free-threading, command lists or optional pipeline capability. */
   memset(args->pData, 0, args->DataSize);
   return S_OK;
}

static HRESULT open_adapter(D3D10DDIARG_OPENADAPTER *args)
{
   trace(TGD3D_TRACE_OPEN_ADAPTER, S_OK, args->Interface, args->Version);
   if (!args->pAdapterCallbacks || !args->pAdapterCallbacks->pfnQueryAdapterInfoCb)
      return E_INVALIDARG;
   tg_gpu_adapter_info info = {};
   info.version = TG_GPU_ADAPTER_ABI;
   info.size = sizeof(info);
   D3DDDICB_QUERYADAPTERINFO query = {};
   query.pPrivateDriverData = &info;
   query.PrivateDriverDataSize = sizeof(info);
   HRESULT hr = args->pAdapterCallbacks->pfnQueryAdapterInfoCb(args->hRTAdapter.handle, &query);
   trace(TGD3D_TRACE_ADAPTER_INFO, hr, info.flags, info.memory_budget_bytes);
   if (FAILED(hr))
      return hr;
   /* The kernel driver must be the matching Tegra miniport with native
    * submission enabled; anything else is not this driver's adapter. */
   const uint32_t required = TG_GPU_ADAPTER_LUID_VALID | TG_GPU_ADAPTER_NATIVE_SUBMIT;
   const uint64_t luid = (uint64_t)(uint32_t)info.luid_high << 32 | info.luid_low;
   if (info.version != TG_GPU_ADAPTER_ABI || info.size != sizeof(info) || info.reserved ||
       (info.flags & required) != required || info.vendor != 0x10de || info.chipset != 0x12e ||
       !info.memory_budget_bytes || info.memory_budget_bytes > TG_GPU_MEMORY_BUDGET_BYTES || !luid)
      return DXGI_ERROR_UNSUPPORTED;

   Adapter *adapter = new (std::nothrow) Adapter();
   if (!adapter)
      return E_OUTOFMEMORY;
   adapter->hRTAdapter = args->hRTAdapter.handle;
   adapter->callbacks = args->pAdapterCallbacks;
   adapter->info = info;
   adapter->luid = luid;
   hr = adapter->vk.open(luid);
   trace(TGD3D_TRACE_VULKAN, hr, luid,
         (uint64_t)adapter->vk.nvk << 32 | adapter->vk.caps.apiVersion);
   if (FAILED(hr)) {
      trace_message(adapter->vk.failure);
      delete adapter;
      return hr;
   }
   args->hAdapter.pDrvPrivate = adapter;
   args->pAdapterFuncs->pfnCalcPrivateDeviceSize = CalcPrivateDeviceSize;
   args->pAdapterFuncs->pfnCreateDevice = CreateDevice;
   args->pAdapterFuncs->pfnCloseAdapter = CloseAdapter;
   return S_OK;
}

extern "C" HRESULT APIENTRY OpenAdapter10(D3D10DDIARG_OPENADAPTER *args)
{
   switch (args->Interface) {
   case D3D10_0_DDI_INTERFACE_VERSION:
   case D3D10_0_x_DDI_INTERFACE_VERSION:
   case D3D10_0_7_DDI_INTERFACE_VERSION:
      return open_adapter(args);
   default:
      trace(TGD3D_TRACE_OPEN_ADAPTER, E_FAIL, args->Interface, args->Version);
      return E_FAIL;
   }
}

extern "C" HRESULT APIENTRY OpenAdapter10_2(D3D10DDIARG_OPENADAPTER *args)
{
   HRESULT hr = open_adapter(args);
   if (SUCCEEDED(hr)) {
      args->pAdapterFuncs_2->pfnGetSupportedVersions = GetSupportedVersions;
      args->pAdapterFuncs_2->pfnGetCaps = GetCaps;
   }
   return hr;
}
