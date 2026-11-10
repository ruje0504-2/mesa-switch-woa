/* SPDX-License-Identifier: MIT
 * Explicit Windows-runtime probe; never run by the default test suite.
 */
#include <initguid.h>
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdexcept>

#include "tri_vs_4_0.h"
#include "tri_ps_4_0.h"

using Microsoft::WRL::ComPtr;

static void exception_address(const char *name, const void *address)
{
   HMODULE module = NULL;
   char path[MAX_PATH] = "<unknown>";
   if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                          GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          static_cast<const char *>(address), &module))
      GetModuleFileNameA(module, path, sizeof(path));
   fprintf(stderr, "%s: %p %s+0x%llx\n", name, address, path,
           (unsigned long long)((uintptr_t)address - (uintptr_t)module));
}

static LONG WINAPI unhandled_exception(EXCEPTION_POINTERS *exception)
{
   fprintf(stderr, "Unhandled exception: %08lx\n", exception->ExceptionRecord->ExceptionCode);
   exception_address("Fault", exception->ExceptionRecord->ExceptionAddress);
#if defined(_M_ARM64) || defined(__aarch64__)
   exception_address("LR", (const void *)(uintptr_t)exception->ContextRecord->Lr);
#endif
   fflush(stderr);
   return EXCEPTION_EXECUTE_HANDLER;
}

static void marker(const char *message)
{
   puts(message);
   fflush(stdout);
}

static void check(HRESULT status, const char *operation)
{
   printf("%s: %08lx\n", operation, (unsigned long)status);
   fflush(stdout);
   if (FAILED(status)) throw std::runtime_error(operation);
}

static uint64_t luid_value(LUID luid)
{
   return (uint64_t)(uint32_t)luid.HighPart << 32 | luid.LowPart;
}

static void images(ID3D11Device *device, ID3D11DeviceContext *context, DXGI_FORMAT format)
{
   const FLOAT vertices[3][8] = {
      {-0.75f, -0.75f, 0.5f, 1, 1, 0, 0, 1},
      { 0.00f,  0.75f, 0.5f, 1, 1, 0, 0, 1},
      { 0.75f, -0.75f, 0.5f, 1, 1, 0, 0, 1},
   };
   D3D11_TEXTURE2D_DESC desc = {};
   desc.Width = desc.Height = 64; desc.MipLevels = desc.ArraySize = 1;
   desc.Format = format; desc.SampleDesc.Count = 1;
   desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_RENDER_TARGET;
   ComPtr<ID3D11Texture2D> target, staging;
   check(device->CreateTexture2D(&desc, NULL, &target), "Create runtime target");
   desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0;
   desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
   check(device->CreateTexture2D(&desc, NULL, &staging), "Create runtime staging");
   ComPtr<ID3D11RenderTargetView> view;
   check(device->CreateRenderTargetView(target.Get(), NULL, &view), "Create runtime view");

   ComPtr<ID3D11VertexShader> vs;
   ComPtr<ID3D11PixelShader> ps;
   check(device->CreateVertexShader(g_VS, sizeof(g_VS), NULL, &vs), "Create runtime VS");
   check(device->CreatePixelShader(g_PS, sizeof(g_PS), NULL, &ps), "Create runtime PS");
   const D3D11_INPUT_ELEMENT_DESC elements[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
   };
   ComPtr<ID3D11InputLayout> layout;
   check(device->CreateInputLayout(elements, 2, g_VS, sizeof(g_VS), &layout), "Create runtime input layout");
   const D3D11_BUFFER_DESC buffer_desc = {sizeof(vertices), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
   const D3D11_SUBRESOURCE_DATA initial = {vertices, 0, 0};
   ComPtr<ID3D11Buffer> buffer;
   check(device->CreateBuffer(&buffer_desc, &initial, &buffer), "Create runtime vertex buffer");
   D3D11_RASTERIZER_DESC raster_desc = {};
   raster_desc.FillMode = D3D11_FILL_SOLID; raster_desc.CullMode = D3D11_CULL_NONE;
   raster_desc.DepthClipEnable = TRUE;
   ComPtr<ID3D11RasterizerState> raster;
   check(device->CreateRasterizerState(&raster_desc, &raster), "Create runtime rasterizer");
   const D3D11_VIEWPORT viewport = {0, 0, 64, 64, 0, 1};
   const UINT stride = sizeof(vertices[0]), offset = 0;
   ID3D11Buffer *vb = buffer.Get();
   ID3D11RenderTargetView *rt = view.Get();
   context->OMSetRenderTargets(1, &rt, NULL);
   context->VSSetShader(vs.Get(), NULL, 0);
   context->PSSetShader(ps.Get(), NULL, 0);
   context->IASetInputLayout(layout.Get());
   context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
   context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
   context->RSSetState(raster.Get());
   context->RSSetViewports(1, &viewport);

   const bool bgra = format == DXGI_FORMAT_B8G8R8A8_UNORM;
   const uint32_t red = bgra ? 0xffff0000 : 0xff0000ff;
   const uint32_t blue = bgra ? 0xff0000ff : 0xffff0000;
   for (unsigned pass = 0; pass < 2; ++pass) {
      const FLOAT color[4] = {0, 0, 1, 1};
      context->ClearRenderTargetView(view.Get(), color);
      if (pass) context->Draw(3, 0);
      context->CopyResource(staging.Get(), target.Get());
      context->Flush();
      D3D11_MAPPED_SUBRESOURCE map = {};
      check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map), "Map runtime pixels");
      unsigned mismatches = 0, red_count = 0;
      if (!map.pData || map.RowPitch < 256 || (map.RowPitch & 3)) {
         context->Unmap(staging.Get(), 0);
         throw std::runtime_error("invalid runtime mapping");
      }
      for (int y = 0; y < 64; ++y) {
         const volatile uint32_t *row = (const volatile uint32_t *)((const char *)map.pData + y * map.RowPitch);
         for (int x = 0; x < 64; ++x) {
            const int px = 2*x+1, py = 2*y+1;
            const bool inside = pass && 48*(py-112)+96*(px-16) > 0 &&
                                48*(py-16)-96*(px-64) > 0 && -96*(py-112) > 0;
            const uint32_t expected = inside ? red : blue;
            red_count += inside;
            const uint32_t observed = row[x];
            if (observed != expected && mismatches++ < 8)
               printf("Mismatch (%d,%d): %08x expected %08x\n", x, y, observed, expected);
         }
      }
      context->Unmap(staging.Get(), 0);
      printf("Runtime %s pass %u: red=%u blue=%u mismatches=%u pitch=%u\n",
             bgra ? "BGRA" : "RGBA", pass, red_count, 4096-red_count, mismatches, map.RowPitch);
      fflush(stdout);
      if (mismatches) throw std::runtime_error("runtime pixel mismatch");
   }
   context->ClearState();
   context->Flush();
}

int main(int argc, char **argv)
{
   const bool hardware = argc == 3 && !strcmp(argv[1], "--luid");
   const bool software = argc == 3 && !strcmp(argv[1], "--software");
   const bool warp = argc == 2 && !strcmp(argv[1], "--warp");
   if (!hardware && !software && !warp) {
      fprintf(stderr, "usage: %s --luid 16-hex-digits | --software dll | --warp\n", argv[0]);
      return 2;
   }
   SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
   SetUnhandledExceptionFilter(unhandled_exception);
   HMODULE module = NULL;
   int result = 0;
   try {
      ComPtr<IDXGIAdapter> adapter;
      uint64_t selected = 0;
      if (hardware) {
         char *end;
         selected = strtoull(argv[2], &end, 16);
         if (strlen(argv[2]) != 16 || *end || !selected)
            throw std::runtime_error("invalid hardware LUID");
         ComPtr<IDXGIFactory> factory;
         check(CreateDXGIFactory(__uuidof(IDXGIFactory), (void **)factory.GetAddressOf()), "CreateDXGIFactory");
         for (UINT i = 0; ; ++i) {
            ComPtr<IDXGIAdapter> candidate;
            const HRESULT status = factory->EnumAdapters(i, &candidate);
            if (status == DXGI_ERROR_NOT_FOUND) break;
            check(status, "EnumAdapters");
            DXGI_ADAPTER_DESC desc = {};
            check(candidate->GetDesc(&desc), "Get adapter description");
            printf("Adapter %016llx vendor=%04x device=%04x\n",
                   (unsigned long long)luid_value(desc.AdapterLuid), desc.VendorId, desc.DeviceId);
            /* Windows exposes the ACPI SWOA0F01 identity here, not PCI IDs.
             * Select by LUID; the native UMD validates the private KMD identity.
             */
            if (luid_value(desc.AdapterLuid) == selected) {
               adapter = candidate;
               break;
            }
         }
         if (!adapter) throw std::runtime_error("requested adapter LUID not found");
      } else if (software) {
         module = LoadLibraryA(argv[2]);
         if (!module) throw std::runtime_error("software UMD load failed");
      }

      ComPtr<ID3D11Device> device;
      ComPtr<ID3D11DeviceContext> context;
      const D3D_FEATURE_LEVEL requested = D3D_FEATURE_LEVEL_10_0;
      D3D_FEATURE_LEVEL actual = {};
      check(D3D11CreateDevice(adapter.Get(), hardware ? D3D_DRIVER_TYPE_UNKNOWN :
                             software ? D3D_DRIVER_TYPE_SOFTWARE : D3D_DRIVER_TYPE_WARP,
                             module, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &requested, 1,
                             D3D11_SDK_VERSION, &device, &actual, &context), "D3D11CreateDevice");
      printf("Runtime feature level=%04x mode=%s\n", actual, hardware ? "native" : software ? "software UMD" : "WARP control");
      if (actual != requested) throw std::runtime_error("unexpected feature level");
      if (hardware) {
         ComPtr<IDXGIDevice> dxgi;
         ComPtr<IDXGIAdapter> actual_adapter;
         check(device.As(&dxgi), "Query runtime DXGI device");
         check(dxgi->GetAdapter(&actual_adapter), "Get runtime adapter");
         DXGI_ADAPTER_DESC desc = {};
         check(actual_adapter->GetDesc(&desc), "Get runtime identity");
         if (luid_value(desc.AdapterLuid) != selected)
            throw std::runtime_error("runtime changed adapter");
         HMODULE umd = GetModuleHandleA("TegraGpuD3D.dll");
         char path[MAX_PATH];
         if (!umd || !GetModuleFileNameA(umd, path, sizeof(path)))
            throw std::runtime_error("NVK D3D UMD was not loaded");
         printf("Runtime UMD: %s\n", path);
      }
      images(device.Get(), context.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
      images(device.Get(), context.Get(), DXGI_FORMAT_B8G8R8A8_UNORM);
      check(device->GetDeviceRemovedReason(), "Device status after readback");
      marker("Release runtime context");
      context.Reset();
      marker("Release runtime device");
      device.Reset();
      marker("Release runtime adapter");
      adapter.Reset();
      marker("Windows D3D runtime clear/draw/readback passed; sharing and presentation were not exercised");
   } catch (const std::exception &e) {
      fprintf(stderr, "Runtime probe failed: %s\n", e.what());
      result = 1;
   }
   if (module) FreeLibrary(module);
   return result;
}
