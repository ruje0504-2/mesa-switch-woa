/* SPDX-License-Identifier: MIT
 * Explicit Windows-runtime probe; never run by the default test suite.
 */
#include <initguid.h>
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
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
   /* Do not walk the loader's module list from a teardown exception. */
   fprintf(stderr, "%s: %p\n", name, address);
}

static void report_runtime_modules()
{
   const char *names[] = {"TegraGpuD3D.dll", "libvulkan_nouveau.dll", "vulkan-1.dll",
                         "ntdll.dll", "d3d11.dll", "d3d10core.dll", "dxgi.dll",
                         "kernelbase.dll", "ucrtbase.dll"};
   for (const char *name : names) {
      HMODULE module = GetModuleHandleA(name);
      if (!module) continue;
      const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)module;
      const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const char *)module + dos->e_lfanew);
      printf("Runtime module %s: base=%p size=%08lx\n", name, (void *)module,
             (unsigned long)nt->OptionalHeader.SizeOfImage);
   }
   fflush(stdout);
}

static LONG WINAPI unhandled_exception(EXCEPTION_POINTERS *exception)
{
   fprintf(stderr, "Unhandled exception: %08lx thread=%lu\n",
           exception->ExceptionRecord->ExceptionCode, GetCurrentThreadId());
   if (exception->ExceptionRecord->NumberParameters >= 2 &&
       (exception->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
        exception->ExceptionRecord->ExceptionCode == EXCEPTION_IN_PAGE_ERROR))
      fprintf(stderr, "Access: operation=%llu address=%p\n",
              (unsigned long long)exception->ExceptionRecord->ExceptionInformation[0],
              (const void *)exception->ExceptionRecord->ExceptionInformation[1]);
   exception_address("Fault", exception->ExceptionRecord->ExceptionAddress);
#if defined(_M_ARM64) || defined(__aarch64__)
   const CONTEXT *context = exception->ContextRecord;
   exception_address("LR", (const void *)(uintptr_t)context->Lr);
   fprintf(stderr, "Registers: x19=%llx x20=%llx x21=%llx x22=%llx sp=%llx fp=%llx\n",
           (unsigned long long)context->X19, (unsigned long long)context->X20,
           (unsigned long long)context->X21, (unsigned long long)context->X22,
           (unsigned long long)context->Sp, (unsigned long long)context->Fp);
   /* A bounded frame-pointer chain, without invoking the loader/unwinder.
    * Keep signed return addresses intact for offline module/PAC decoding.
    */
   const NT_TIB *tib = (const NT_TIB *)NtCurrentTeb();
   const uintptr_t low = (uintptr_t)tib->StackLimit;
   const uintptr_t high = (uintptr_t)tib->StackBase;
   uintptr_t fp = context->Fp;
   for (unsigned i = 0; i < 12 && high >= 16 && fp >= low && fp <= high - 16 && !(fp & 15); ++i) {
      const volatile uintptr_t *frame = (const volatile uintptr_t *)fp;
      const uintptr_t next = frame[0], address = frame[1];
      fprintf(stderr, "Frame %u: fp=%p return=%p\n", i, (void *)fp, (void *)address);
      if (next <= fp) break;
      fp = next;
   }
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

/* Probe negotiation separately from the FL10_0 rendering regression. A
 * successful device creation is neither feature conformance nor DWM Present.
 */
static void feature_levels(IDXGIAdapter *adapter, D3D_DRIVER_TYPE type,
                           HMODULE module, uint64_t selected)
{
   const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                     D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
   unsigned successes = 0;
   for (unsigned i = 0; i <= ARRAYSIZE(levels); ++i) {
      const bool combined = i == ARRAYSIZE(levels);
      ComPtr<ID3D11Device> device;
      ComPtr<ID3D11DeviceContext> context;
      D3D_FEATURE_LEVEL actual = {};
      const HRESULT hr = D3D11CreateDevice(adapter, type, module,
         D3D11_CREATE_DEVICE_BGRA_SUPPORT, combined ? levels : &levels[i],
         combined ? ARRAYSIZE(levels) : 1, D3D11_SDK_VERSION, &device, &actual, &context);
      printf("Feature negotiation requested=%04x combined=%u status=%08lx actual=%04x\n",
             combined ? 0 : levels[i], combined, (unsigned long)hr, actual);
      fflush(stdout);
      if (FAILED(hr)) continue;
      if (!combined && actual != levels[i])
         throw std::runtime_error("runtime returned a different explicit feature level");
      ComPtr<IDXGIDevice> dxgi;
      ComPtr<IDXGIAdapter> actual_adapter;
      check(device.As(&dxgi), "Query negotiated DXGI device");
      check(dxgi->GetAdapter(&actual_adapter), "Get negotiated adapter");
      DXGI_ADAPTER_DESC desc = {};
      check(actual_adapter->GetDesc(&desc), "Get negotiated adapter identity");
      const uint64_t luid = luid_value(desc.AdapterLuid);
      printf("Negotiated adapter %016llx vendor=%04x device=%04x\n",
             (unsigned long long)luid, desc.VendorId, desc.DeviceId);
      if (selected && luid != selected)
         throw std::runtime_error("feature probe changed the requested hardware adapter");
      if (selected) {
         HMODULE umd = GetModuleHandleA("TegraGpuD3D.dll");
         char path[MAX_PATH];
         if (!umd || !GetModuleFileNameA(umd, path, sizeof(path)))
            throw std::runtime_error("feature probe did not load the native UMD");
         printf("Negotiated UMD: %s\n", path);
      }
      check(device->GetDeviceRemovedReason(), "Negotiated device status");
      context.Reset();
      device.Reset();
      ++successes;
   }
   if (!successes) throw std::runtime_error("no requested feature level created a device");
   report_runtime_modules();
   marker("Feature negotiation recorded; rendering, DWM and scanout were not exercised");
}

/* Exercise the runtime's resource association independently of rendering.
 * Legacy shared handles and NT handles have different ownership contracts.
 */
static void shared_contract(ID3D11Device *producer, ID3D11Device *consumer, bool nt)
{
   D3D11_TEXTURE2D_DESC desc = {};
   desc.Width = desc.Height = 64;
   desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
   desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
   desc.Usage = D3D11_USAGE_DEFAULT;
   desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
   desc.MiscFlags = nt ? D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX :
                        D3D11_RESOURCE_MISC_SHARED;
   printf("Shared contract: mode=%s flags=%08x\n", nt ? "NT" : "legacy", desc.MiscFlags);
   fflush(stdout);
   ComPtr<ID3D11Texture2D> original, opened;
   check(producer->CreateTexture2D(&desc, NULL, &original), "Create shared texture");
   HANDLE handle = NULL;
   if (nt) {
      ComPtr<IDXGIResource1> resource;
      check(original.As(&resource), "Query NT resource");
      check(resource->CreateSharedHandle(NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                         NULL, &handle), "Create NT shared handle");
      ComPtr<ID3D11Device1> device1;
      HRESULT status = consumer->QueryInterface(IID_PPV_ARGS(&device1));
      if (SUCCEEDED(status))
         status = device1->OpenSharedResource1(handle, IID_PPV_ARGS(&opened));
      CloseHandle(handle);
      check(status, "Open NT shared resource");
   } else {
      ComPtr<IDXGIResource> resource;
      check(original.As(&resource), "Query legacy resource");
      check(resource->GetSharedHandle(&handle), "Get legacy shared handle");
      /* A legacy handle is not an NT handle: never CloseHandle it. */
      check(consumer->OpenSharedResource(handle, IID_PPV_ARGS(&opened)), "Open legacy shared resource");
   }
   printf("Shared contract handle=%p\n", handle);
   fflush(stdout);
   original.Reset();
   D3D11_TEXTURE2D_DESC actual = {};
   opened->GetDesc(&actual);
   if (memcmp(&actual, &desc, sizeof(desc)))
      throw std::runtime_error("opened resource descriptor differs");
   opened.Reset();
   marker(nt ? "NT resource association/open/release passed" : "Legacy resource association/open/release passed");
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
   const bool implicit = argc == 4 && !strcmp(argv[3], "--implicit-release");
   const bool shared = argc > 2 && !strcmp(argv[argc - 1], "--shared-contract");
   const bool features = argc > 2 && !strcmp(argv[argc - 1], "--feature-levels");
   const bool hardware = (argc == 3 || implicit || (argc == 4 && (shared || features))) && !strcmp(argv[1], "--luid");
   const bool software = (argc == 3 || (argc == 4 && features)) && !strcmp(argv[1], "--software");
   const bool warp = (argc == 2 || (argc == 3 && (shared || features))) && !strcmp(argv[1], "--warp");
   if (!hardware && !software && !warp) {
      fprintf(stderr, "usage: %s --luid 16-hex-digits [--implicit-release|--shared-contract|--feature-levels] | --software dll [--feature-levels] | --warp [--shared-contract|--feature-levels]\n", argv[0]);
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

      if (features) {
         feature_levels(adapter.Get(), hardware ? D3D_DRIVER_TYPE_UNKNOWN :
                        software ? D3D_DRIVER_TYPE_SOFTWARE : D3D_DRIVER_TYPE_WARP,
                        module, selected);
         if (module) FreeLibrary(module);
         return 0;
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
      report_runtime_modules();
      if (shared) {
         ComPtr<ID3D11Device> consumer;
         ComPtr<ID3D11DeviceContext> consumer_context;
         D3D_FEATURE_LEVEL consumer_level = {};
         check(D3D11CreateDevice(adapter.Get(), hardware ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_WARP,
                                NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &requested, 1,
                                D3D11_SDK_VERSION, &consumer, &consumer_level, &consumer_context),
                "Create sharing consumer");
         if (consumer_level != requested) throw std::runtime_error("unexpected consumer feature level");
         shared_contract(device.Get(), consumer.Get(), false);
         shared_contract(device.Get(), consumer.Get(), true);
         check(consumer->GetDeviceRemovedReason(), "Consumer status after sharing");
      } else {
         images(device.Get(), context.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
         images(device.Get(), context.Get(), DXGI_FORMAT_B8G8R8A8_UNORM);
      }
      check(device->GetDeviceRemovedReason(), "Device status after test");
      if (implicit) {
         /* Preserve the original ComPtr scope-exit release path and buffering
          * while retaining an exception address if it fails again.
          */
         puts("Runtime readback complete; releasing at scope exit");
      } else {
         marker("Release runtime context");
         context.Reset();
         marker("Release runtime device");
         device.Reset();
         marker("Release runtime adapter");
         adapter.Reset();
      }
   } catch (const std::exception &e) {
      fprintf(stderr, "Runtime probe failed: %s\n", e.what());
      result = 1;
   }
   if (module) FreeLibrary(module);
   if (!result)
      marker(shared ? "Windows D3D runtime resource association passed; GPU access and presentation were not exercised" :
                      "Windows D3D runtime clear/draw/readback passed; sharing and presentation were not exercised");
   return result;
}
