/* SPDX-License-Identifier: MIT */
/* Direct DDI smoke test with real KMT/NVK. This is not the D3D11 runtime. */
#include <windows.h>
#include "winddk_compat.h"
#include <d3d10_1.h>
#include <d3d10umddi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdexcept>
#include <vector>

#include "nouveau/vulkan/nvkmd/windows/kmt_abi.h"
#include "nouveau/vulkan/nvkmd/windows/adapter_abi.h"

static LONG (WINAPI *query_kmt)(const tg_kmt_query *);
static LONG (WINAPI *close_kmt)(const tg_kmt_close *);
static HRESULT ddi_error;

static void require(bool ok, const char *operation)
{
   if (!ok)
      throw std::runtime_error(operation);
}

static void step(const char *operation)
{
   printf("%s: %08lx\n", operation, (unsigned long)ddi_error);
   fflush(stdout);
   require(SUCCEEDED(ddi_error), operation);
}

static HRESULT APIENTRY query_adapter(HANDLE adapter, const D3DDDICB_QUERYADAPTERINFO *args)
{
   const tg_kmt_query query = {(uint32_t)(uintptr_t)adapter, TG_KMT_UMDRIVERPRIVATE,
                             args->pPrivateDriverData, args->PrivateDriverDataSize};
   LONG status = query_kmt(&query);
   printf("KMT adapter query: %08lx\n", (unsigned long)status);
   if (status >= 0 && args->PrivateDriverDataSize == sizeof(tg_gpu_adapter_info)) {
      const auto *info = static_cast<const tg_gpu_adapter_info *>(args->pPrivateDriverData);
      printf("KMD identity: vendor=%04x chipset=%x flags=%x LUID=%08x%08x budget=%llu\n",
             info->vendor, info->chipset, info->flags, (uint32_t)info->luid_high, info->luid_low,
             (unsigned long long)info->memory_budget_bytes);
   }
   fflush(stdout);
   return status >= 0 ? S_OK : HRESULT_FROM_NT(status);
}

static void APIENTRY set_error(D3D10DDI_HRTCORELAYER, HRESULT error)
{
   ddi_error = error;
   fprintf(stderr, "DDI SetError: %08lx\n", (unsigned long)error);
   fflush(stderr);
}

struct Run {
   HMODULE gdi = NULL, umd = NULL;
   uint32_t kmt_adapter = 0;
   D3D10DDIARG_OPENADAPTER adapter = {};
   D3D10_2DDI_ADAPTERFUNCS adapters = {};
   D3D10DDI_HDEVICE device = {};
   D3D10DDI_DEVICEFUNCS functions = {};
   D3D10DDI_HRESOURCE target = {}, staging = {};
   D3D10DDI_HRENDERTARGETVIEW view = {};
   bool device_live = false, target_live = false, staging_live = false, view_live = false, mapped = false;

   ~Run()
   {
      if (mapped) functions.pfnStagingResourceUnmap(device, staging, 0);
      if (view_live) functions.pfnDestroyRenderTargetView(device, view);
      if (staging_live) functions.pfnDestroyResource(device, staging);
      if (target_live) functions.pfnDestroyResource(device, target);
      if (device_live) functions.pfnDestroyDevice(device);
      free(view.pDrvPrivate); free(staging.pDrvPrivate); free(target.pDrvPrivate); free(device.pDrvPrivate);
      if (adapter.hAdapter.pDrvPrivate) {
         HRESULT status = adapters.pfnCloseAdapter(adapter.hAdapter);
         printf("CloseAdapter: %08lx\n", (unsigned long)status);
         if (FAILED(status)) ddi_error = status;
      }
      if (kmt_adapter) {
         const tg_kmt_close close = {kmt_adapter};
         LONG status = close_kmt(&close);
         printf("KMT close: %08lx\n", (unsigned long)status);
         if (status < 0) ddi_error = HRESULT_FROM_NT(status);
      }
      if (umd) FreeLibrary(umd);
      if (gdi) FreeLibrary(gdi);
      fflush(stdout);
   }
};

static void *private_storage(SIZE_T bytes)
{
   require(bytes != 0, "zero private storage size");
   void *ptr = calloc(1, bytes);
   require(ptr != NULL, "private storage allocation");
   return ptr;
}

static void run(Run &r, const char *umd_path, uint64_t luid)
{
   r.gdi = LoadLibraryExW(L"gdi32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
   require(r.gdi != NULL, "load system gdi32");
   auto enumerate = reinterpret_cast<LONG (WINAPI *)(tg_kmt_enum *)>(GetProcAddress(r.gdi, "D3DKMTEnumAdapters3"));
   query_kmt = reinterpret_cast<decltype(query_kmt)>(GetProcAddress(r.gdi, "D3DKMTQueryAdapterInfo"));
   close_kmt = reinterpret_cast<decltype(close_kmt)>(GetProcAddress(r.gdi, "D3DKMTCloseAdapter"));
   require(enumerate && query_kmt && close_kmt, "KMT entry points");
   tg_kmt_enum list = {TG_KMT_INCLUDE_COMPUTE_ONLY, 0, NULL};
   require(enumerate(&list) >= 0 && list.count, "enumerate adapter count");
   std::vector<tg_kmt_adapter> devices(list.count);
   list.adapters = devices.data();
   require(enumerate(&list) >= 0, "enumerate adapters");
   require(list.count <= devices.size(), "adapter count grew");
   for (uint32_t i = 0; i < list.count; ++i) {
      const auto &d = devices[i];
      const uint64_t found = (uint64_t)(uint32_t)d.luid.high << 32 | d.luid.low;
      if (found == luid && !r.kmt_adapter) {
         r.kmt_adapter = d.handle;
      } else {
         const tg_kmt_close close = {d.handle};
         require(close_kmt(&close) >= 0, "close unselected adapter");
      }
   }
   require(r.kmt_adapter != 0, "requested LUID absent");
   printf("GM20B requested LUID: %016llx\n", (unsigned long long)luid);
   r.umd = LoadLibraryA(umd_path);
   require(r.umd != NULL, "load NVK UMD");
   auto open = reinterpret_cast<PFND3D10DDI_OPENADAPTER>(GetProcAddress(r.umd, "OpenAdapter10_2"));
   require(open != NULL && !GetProcAddress(r.umd, "D3DKMTCreateDevice"), "hardware UMD exports");
   D3DDDI_ADAPTERCALLBACKS callbacks = {};
   callbacks.pfnQueryAdapterInfoCb = query_adapter;
   r.adapter.hRTAdapter.handle = (HANDLE)(uintptr_t)r.kmt_adapter;
   r.adapter.pAdapterCallbacks = &callbacks;
   r.adapter.pAdapterFuncs_2 = &r.adapters;
   ddi_error = open(&r.adapter);
   step("OpenAdapter10_2");
   UINT count = 0;
   require(SUCCEEDED(r.adapters.pfnGetSupportedVersions(r.adapter.hAdapter, &count, NULL)) && count,
           "query DDI versions");
   std::vector<UINT64> versions(count);
   require(SUCCEEDED(r.adapters.pfnGetSupportedVersions(r.adapter.hAdapter, &count, versions.data())),
           "read DDI versions");
   bool supported = false;
   for (UINT i = 0; i < count; ++i) supported |= versions[i] == D3D10_0_DDI_SUPPORTED;
   require(supported, "D3D10 DDI support");

   D3D10DDIARG_CALCPRIVATEDEVICESIZE size = {};
   size.Interface = D3D10_0_DDI_INTERFACE_VERSION;
   size.Version = (UINT)D3D10_0_DDI_SUPPORTED;
   r.device.pDrvPrivate = private_storage(r.adapters.pfnCalcPrivateDeviceSize(r.adapter.hAdapter, &size));
   D3DDDI_DEVICECALLBACKS kernel_callbacks = {};
   D3D10DDI_CORELAYER_DEVICECALLBACKS runtime_callbacks = {};
   runtime_callbacks.pfnSetErrorCb = set_error;
   DXGI_DDI_BASE_FUNCTIONS dxgi = {};
   D3D10DDIARG_CREATEDEVICE create = {};
   create.Interface = size.Interface; create.Version = size.Version;
   create.hDrvDevice = r.device; create.pDeviceFuncs = &r.functions;
   create.pKTCallbacks = &kernel_callbacks; create.pUMCallbacks = &runtime_callbacks;
   create.DXGIBaseDDI.pDXGIDDIBaseFunctions = &dxgi;
   ddi_error = r.adapters.pfnCreateDevice(r.adapter.hAdapter, &create);
   step("CreateDevice"); r.device_live = true;

   const UINT width = 64, height = 64;
   D3D10DDI_MIPINFO mip = {};
   mip.TexelWidth = mip.PhysicalWidth = width;
   mip.TexelHeight = mip.PhysicalHeight = height;
   mip.TexelDepth = mip.PhysicalDepth = 1;
   D3D10DDIARG_CREATERESOURCE desc = {};
   desc.pMipInfoList = &mip; desc.ResourceDimension = D3D10DDIRESOURCE_TEXTURE2D;
   desc.Usage = D3D10_DDI_USAGE_DEFAULT; desc.BindFlags = D3D10_DDI_BIND_RENDER_TARGET;
   desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
   desc.SampleDesc.Count = desc.MipLevels = desc.ArraySize = 1;
   r.target.pDrvPrivate = private_storage(r.functions.pfnCalcPrivateResourceSize(r.device, &desc));
   r.functions.pfnCreateResource(r.device, &desc, r.target, {});
   step("Create render target"); r.target_live = true;
   desc.Usage = D3D10_DDI_USAGE_STAGING; desc.BindFlags = 0;
   desc.MapFlags = D3D10_DDI_CPU_ACCESS_READ | D3D10_DDI_CPU_ACCESS_WRITE;
   r.staging.pDrvPrivate = private_storage(r.functions.pfnCalcPrivateResourceSize(r.device, &desc));
   r.functions.pfnCreateResource(r.device, &desc, r.staging, {});
   step("Create staging texture"); r.staging_live = true;
   D3D10DDIARG_CREATERENDERTARGETVIEW view = {};
   view.hDrvResource = r.target; view.ResourceDimension = desc.ResourceDimension; view.Format = desc.Format;
   view.Tex2D.ArraySize = 1;
   r.view.pDrvPrivate = private_storage(r.functions.pfnCalcPrivateRenderTargetViewSize(r.device, &view));
   r.functions.pfnCreateRenderTargetView(r.device, &view, r.view, {});
   step("Create render target view"); r.view_live = true;

   for (unsigned pass = 0; pass < 3; ++pass) {
      D3D10DDI_MAPPED_SUBRESOURCE map = {};
      r.functions.pfnStagingResourceMap(r.device, r.staging, 0, D3D10_DDI_MAP_WRITE, 0, &map);
      step("Map staging write"); require(map.pData && map.RowPitch >= width * 4 && !(map.RowPitch & 3), "write mapping");
      r.mapped = true;
      for (UINT y = 0; y < height; ++y) {
         volatile uint32_t *row = (volatile uint32_t *)((char *)map.pData + y * map.RowPitch);
         for (UINT x = 0; x < width; ++x) row[x] = 0x5a5a5a5a;
      }
      r.functions.pfnStagingResourceUnmap(r.device, r.staging, 0); r.mapped = false;
      step("Unmap staging write");
      FLOAT color[4] = {0, 0, 0, 1}; color[pass] = 1;
      r.functions.pfnClearRenderTargetView(r.device, r.view, color); step("ClearRenderTargetView");
      r.functions.pfnResourceCopy(r.device, r.staging, r.target); step("ResourceCopy");
      r.functions.pfnFlush(r.device); step("Flush");
      map = {};
      r.functions.pfnStagingResourceMap(r.device, r.staging, 0, D3D10_DDI_MAP_READ, 0, &map);
      step("Map staging read"); require(map.pData && map.RowPitch >= width * 4 && !(map.RowPitch & 3), "read mapping");
      r.mapped = true;
      const uint32_t expected = 0xff000000 | (0xffu << (pass * 8));
      unsigned mismatches = 0;
      for (UINT y = 0; y < height; ++y) {
         const volatile uint32_t *row = (const volatile uint32_t *)((char *)map.pData + y * map.RowPitch);
         for (UINT x = 0; x < width; ++x) mismatches += row[x] != expected;
      }
      printf("Pass %u: expected=%08x words=%u mismatches=%u pitch=%u\n",
             pass, expected, width * height, mismatches, map.RowPitch); fflush(stdout);
      r.functions.pfnStagingResourceUnmap(r.device, r.staging, 0); r.mapped = false;
      step("Unmap staging read"); require(!mismatches, "pixel mismatch");
   }
}

int main(int argc, char **argv)
{
   if (argc != 4) {
      fprintf(stderr, "Usage: %s <absolute-UMD.dll> <absolute-NVK-ICD.json> <16-digit-LUID>\n", argv[0]);
      return 2;
   }
   char *end = NULL;
   uint64_t luid = strtoull(argv[3], &end, 16);
   if (strlen(argv[3]) != 16 || !end || *end || !luid) return 2;
   SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
   if (!SetEnvironmentVariableA("VK_DRIVER_FILES", argv[2]) ||
       !SetEnvironmentVariableA("NVK_I_WANT_A_BROKEN_VULKAN_DRIVER", "1")) return 2;
   try {
      Run state;
      run(state, argv[1], luid);
   } catch (const std::exception &error) {
      fprintf(stderr, "FAIL: %s\n", error.what());
      return 1;
   }
   if (FAILED(ddi_error)) return 1;
   puts("Native DDI clear/copy/readback passed; Windows D3D runtime and presentation were not exercised");
   return 0;
}
