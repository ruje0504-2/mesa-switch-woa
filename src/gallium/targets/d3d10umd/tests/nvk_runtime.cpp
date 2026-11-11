/* SPDX-License-Identifier: MIT
 * Explicit Windows-runtime probe; never run by the default test suite.
 */
#include <initguid.h>
#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
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

static void acquire_shared(IDXGIKeyedMutex *mutex, UINT64 key, const char *phase)
{
   const HRESULT status = mutex->AcquireSync(key, 5000);
   /* WAIT_TIMEOUT and WAIT_ABANDONED are positive HRESULT values. */
   if (status != S_OK) {
      fprintf(stderr, "%s: %08lx\n", phase, (unsigned long)status);
      throw std::runtime_error("shared keyed mutex was not acquired");
   }
}

static void shared_pixels(ID3D11Device *device, ID3D11DeviceContext *context,
                          ID3D11Texture2D *source, bool red, const char *phase)
{
   D3D11_TEXTURE2D_DESC desc = {};
   source->GetDesc(&desc);
   const DXGI_FORMAT format = desc.Format;
   desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = desc.MiscFlags = 0;
   desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
   ComPtr<ID3D11Texture2D> staging;
   check(device->CreateTexture2D(&desc, NULL, &staging), "Create shared readback staging");
   context->CopyResource(staging.Get(), source);
   D3D11_MAPPED_SUBRESOURCE mapped = {};
   check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map shared GPU contents");
   const unsigned char expected[4] = {
      static_cast<unsigned char>(red && format == DXGI_FORMAT_R8G8B8A8_UNORM ? 255 : 0),
      static_cast<unsigned char>(red ? 0 : 255),
      static_cast<unsigned char>(red && format == DXGI_FORMAT_B8G8R8A8_UNORM ? 255 : 0), 255};
   bool matches = mapped.RowPitch >= desc.Width * 4;
   for (UINT y = 0; matches && y < desc.Height; ++y) {
      const auto row = static_cast<const unsigned char *>(mapped.pData) + y * mapped.RowPitch;
      for (UINT x = 0; x < desc.Width; ++x) {
         if (memcmp(row + x * 4, expected, 4)) { matches = false; break; }
      }
   }
   context->Unmap(staging.Get(), 0);
   check(device->GetDeviceRemovedReason(), "Shared readback device status");
   if (!matches) throw std::runtime_error("shared GPU pixels differ");
   printf("Shared pixels: phase=%s format=%u matched=%u\n", phase, format, desc.Width * desc.Height);
   fflush(stdout);
}

/* Real contents and producer-device lifetime. Legacy handles are not NT
 * handles; keyed-mutex synchronization is used for the separate NT case. */
static void shared_contract(IDXGIAdapter *adapter, bool hardware, D3D_FEATURE_LEVEL requested,
                            ID3D11Device *consumer, DXGI_FORMAT format, bool nt)
{
   ComPtr<ID3D11Device> producer;
   ComPtr<ID3D11DeviceContext> producer_context, consumer_context;
   D3D_FEATURE_LEVEL actual_level = {};
   check(D3D11CreateDevice(adapter, hardware ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_WARP,
                          NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &requested, 1,
                          D3D11_SDK_VERSION, &producer, &actual_level, &producer_context),
         "Create sharing producer");
   if (actual_level != requested) throw std::runtime_error("unexpected producer feature level");
   consumer->GetImmediateContext(&consumer_context);
   D3D11_TEXTURE2D_DESC desc = {};
   desc.Width = desc.Height = 64;
   desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
   desc.Format = format;
   desc.Usage = D3D11_USAGE_DEFAULT;
   desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
   desc.MiscFlags = nt ? D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX :
                        D3D11_RESOURCE_MISC_SHARED;
   printf("Shared contract: mode=%s format=%u flags=%08x\n", nt ? "NT" : "legacy", format, desc.MiscFlags);
   fflush(stdout);
   ComPtr<ID3D11Texture2D> original, opened;
   ComPtr<ID3D11RenderTargetView> producer_view, consumer_view;
   ComPtr<IDXGIKeyedMutex> producer_mutex, consumer_mutex;
   check(producer->CreateTexture2D(&desc, NULL, &original), "Create shared texture");
   check(producer->CreateRenderTargetView(original.Get(), NULL, &producer_view), "Create producer shared RTV");
   if (nt) {
      check(original.As(&producer_mutex), "Get producer keyed mutex");
      acquire_shared(producer_mutex.Get(), 0, "Acquire producer shared texture");
   }
   const FLOAT red[4] = {1, 0, 0, 1}, green[4] = {0, 1, 0, 1};
   producer_context->ClearRenderTargetView(producer_view.Get(), red);
   producer_context->Flush();
   check(producer->GetDeviceRemovedReason(), "Producer GPU write status");
   if (nt) check(producer_mutex->ReleaseSync(1), "Publish producer GPU contents");
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
      check(opened.As(&consumer_mutex), "Get consumer keyed mutex");
      acquire_shared(consumer_mutex.Get(), 1, "Acquire producer GPU contents");
   } else {
      ComPtr<IDXGIResource> resource;
      check(original.As(&resource), "Query legacy resource");
      check(resource->GetSharedHandle(&handle), "Get legacy shared handle");
      check(consumer->OpenSharedResource(handle, IID_PPV_ARGS(&opened)), "Open legacy shared resource");
   }
   printf("Shared contract handle=%p\n", handle);
   fflush(stdout);
   D3D11_TEXTURE2D_DESC actual = {};
   opened->GetDesc(&actual);
   if (memcmp(&actual, &desc, sizeof(desc))) throw std::runtime_error("opened resource descriptor differs");
   shared_pixels(consumer, consumer_context.Get(), opened.Get(), true, "producer-to-consumer");
   check(consumer->CreateRenderTargetView(opened.Get(), NULL, &consumer_view), "Create consumer shared RTV");
   consumer_context->ClearRenderTargetView(consumer_view.Get(), green);
   consumer_context->Flush();
   check(consumer->GetDeviceRemovedReason(), "Consumer GPU write status");
   if (nt) {
      check(consumer_mutex->ReleaseSync(2), "Publish consumer GPU contents");
      acquire_shared(producer_mutex.Get(), 2, "Acquire consumer GPU contents");
   }
   shared_pixels(producer.Get(), producer_context.Get(), original.Get(), false, "consumer-to-producer");
   if (nt) check(producer_mutex->ReleaseSync(3), "Release producer ownership");
   producer_context->ClearState();
   producer_view.Reset(); producer_mutex.Reset(); original.Reset();
   producer_context.Reset(); producer.Reset();
   marker("Sharing producer device released");
   if (nt) acquire_shared(consumer_mutex.Get(), 3, "Acquire after producer release");
   shared_pixels(consumer, consumer_context.Get(), opened.Get(), false, "after-producer-device-release");
   if (nt) check(consumer_mutex->ReleaseSync(4), "Release consumer ownership");
   consumer_view.Reset(); consumer_mutex.Reset(); opened.Reset();
   marker(nt ? "NT shared GPU contents/lifetime passed" : "Legacy shared GPU contents/lifetime passed");
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

/* Different t/s slots and deliberately conflicting bindings catch accidental
 * conversion to the legacy combined-sampler model. No presentation is used. */
static void sampled_images(ID3D11Device *device, ID3D11DeviceContext *context,
                           DXGI_FORMAT format)
{
   struct compiler_module {
      HMODULE handle = LoadLibraryExW(L"d3dcompiler_47.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
      ~compiler_module() { if (handle) FreeLibrary(handle); }
   } compiler;
   if (!compiler.handle) throw std::runtime_error("system shader compiler unavailable");
   auto compile = reinterpret_cast<pD3DCompile>(GetProcAddress(compiler.handle, "D3DCompile"));
   if (!compile) throw std::runtime_error("D3DCompile unavailable");
   static const char source[] =
      "Texture2D<float4> image : register(t5);\n"
      "SamplerState filtering : register(s2);\n"
      "float4 main(float4 p : SV_Position) : SV_Target {\n"
      " return image.Sample(filtering, p.xy * 0.125 + 0.25).bgra;\n"
      "}\n";
   ComPtr<ID3DBlob> code, errors;
   HRESULT status = compile(source, sizeof(source) - 1, "sample-slots.hlsl", NULL, NULL,
                            "main", "ps_4_0", D3DCOMPILE_ENABLE_STRICTNESS, 0,
                            &code, &errors);
   if (FAILED(status) && errors)
      fprintf(stderr, "%.*s\n", (int)errors->GetBufferSize(), (const char *)errors->GetBufferPointer());
   check(status, "Compile SAMPLE pixel shader");
   ComPtr<ID3D11VertexShader> vs;
   ComPtr<ID3D11PixelShader> ps;
   check(device->CreateVertexShader(g_VS, sizeof(g_VS), NULL, &vs), "Create sample VS");
   check(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), NULL, &ps), "Create sample PS");
   const FLOAT vertices[3][8] = {
      {-1, -1, 0.5f, 1, 1, 1, 1, 1},
      {-1,  3, 0.5f, 1, 1, 1, 1, 1},
      { 3, -1, 0.5f, 1, 1, 1, 1, 1},
   };
   const D3D11_INPUT_ELEMENT_DESC elements[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
   };
   ComPtr<ID3D11InputLayout> layout;
   check(device->CreateInputLayout(elements, 2, g_VS, sizeof(g_VS), &layout), "Create sample layout");
   const D3D11_BUFFER_DESC bd = {sizeof(vertices), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
   const D3D11_SUBRESOURCE_DATA vertex_data = {vertices, 0, 0};
   ComPtr<ID3D11Buffer> buffer;
   check(device->CreateBuffer(&bd, &vertex_data, &buffer), "Create sample vertices");
   D3D11_TEXTURE2D_DESC desc = {};
   desc.Width = desc.Height = 64; desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
   desc.Format = format; desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_RENDER_TARGET;
   ComPtr<ID3D11Texture2D> target, staging;
   check(device->CreateTexture2D(&desc, NULL, &target), "Create sample target");
   desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
   check(device->CreateTexture2D(&desc, NULL, &staging), "Create sample staging");
   ComPtr<ID3D11RenderTargetView> view;
   check(device->CreateRenderTargetView(target.Get(), NULL, &view), "Create sample RTV");
   const uint32_t texels[3][4] = {
      {0xff0000ff, 0xff00ff00, 0xffff0000, 0xffffffff},
      {0xff804020, 0xff201080, 0xff408010, 0xff102040},
      {0xffc92111, 0xffc92111, 0xffc92111, 0xffc92111},
   };
   ComPtr<ID3D11Texture2D> textures[3];
   ComPtr<ID3D11ShaderResourceView> views[3];
   desc.Width = desc.Height = 2; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
   desc.Usage = D3D11_USAGE_IMMUTABLE; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE; desc.CPUAccessFlags = 0;
   for (unsigned i = 0; i < 3; ++i) {
      const D3D11_SUBRESOURCE_DATA data = {texels[i], 8, 16};
      check(device->CreateTexture2D(&desc, &data, &textures[i]), "Create sample texture");
      check(device->CreateShaderResourceView(textures[i].Get(), NULL, &views[i]), "Create sample SRV");
   }
   D3D11_SAMPLER_DESC sd = {};
   sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT; sd.MaxAnisotropy = 1;
   sd.ComparisonFunc = D3D11_COMPARISON_NEVER; sd.MaxLOD = D3D11_FLOAT32_MAX;
   ComPtr<ID3D11SamplerState> samplers[2];
   for (unsigned i = 0; i < 2; ++i) {
      sd.AddressU = sd.AddressV = sd.AddressW = i ? D3D11_TEXTURE_ADDRESS_CLAMP : D3D11_TEXTURE_ADDRESS_WRAP;
      check(device->CreateSamplerState(&sd, &samplers[i]), "Create sample sampler");
   }
   D3D11_RASTERIZER_DESC rd = {};
   rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
   ComPtr<ID3D11RasterizerState> raster;
   check(device->CreateRasterizerState(&rd, &raster), "Create sample rasterizer");
   const D3D11_VIEWPORT viewport = {0, 0, 64, 64, 0, 1};
   const UINT stride = sizeof(vertices[0]), offset = 0;
   ID3D11Buffer *vb = buffer.Get(); ID3D11RenderTargetView *rt = view.Get();
   context->OMSetRenderTargets(1, &rt, NULL);
   context->VSSetShader(vs.Get(), NULL, 0); context->PSSetShader(ps.Get(), NULL, 0);
   context->IASetInputLayout(layout.Get()); context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
   context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
   context->RSSetState(raster.Get()); context->RSSetViewports(1, &viewport);
   ID3D11ShaderResourceView *poison = views[2].Get();
   context->PSSetShaderResources(2, 1, &poison);
   unsigned total_mismatches = 0;
   for (unsigned pass = 0; pass < 3; ++pass) {
      const unsigned palette = pass == 2, clamp = pass == 1;
      ID3D11ShaderResourceView *texture = views[palette].Get();
      ID3D11SamplerState *filter = samplers[clamp].Get(), *wrong = samplers[!clamp].Get();
      context->PSSetShaderResources(5, 1, &texture);
      context->PSSetSamplers(2, 1, &filter); context->PSSetSamplers(5, 1, &wrong);
      const FLOAT clear[4] = {0, 0, 0, 0};
      context->ClearRenderTargetView(view.Get(), clear); context->Draw(3, 0);
      context->CopyResource(staging.Get(), target.Get()); context->Flush();
      D3D11_MAPPED_SUBRESOURCE map = {};
      check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map), "Map sampled pixels");
      if (!map.pData || map.RowPitch < 256 || (map.RowPitch & 3)) {
         context->Unmap(staging.Get(), 0); throw std::runtime_error("invalid sample mapping");
      }
      unsigned mismatches = 0;
      for (unsigned y = 0; y < 64; ++y) {
         const volatile uint32_t *row = (const volatile uint32_t *)((const char *)map.pData + y * map.RowPitch);
         for (unsigned x = 0; x < 64; ++x) {
            float u = (x + 0.5f) * 0.125f + 0.25f, v = (y + 0.5f) * 0.125f + 0.25f;
            if (!clamp) { u -= floorf(u); v -= floorf(v); }
            const unsigned tx = u >= 0.5f, ty = v >= 0.5f;
            const uint32_t rgba = texels[palette][ty * 2 + tx];
            const uint32_t expected = format == DXGI_FORMAT_B8G8R8A8_UNORM ? rgba :
               (rgba & 0xff00ff00u) | ((rgba & 0xffu) << 16) | ((rgba >> 16) & 0xffu);
            const uint32_t observed = row[x];
            if (observed != expected && mismatches++ < 8)
               printf("Sample mismatch (%u,%u): %08x expected %08x\n", x, y, observed, expected);
         }
      }
      context->Unmap(staging.Get(), 0);
      printf("Sample %s pass %u: t5/s2 palette=%u clamp=%u pixels=4096 mismatches=%u\n",
             format == DXGI_FORMAT_B8G8R8A8_UNORM ? "BGRA" : "RGBA", pass, palette, clamp, mismatches);
      fflush(stdout);
      total_mismatches += mismatches;
   }
   context->ClearState(); context->Flush();
   if (total_mismatches) throw std::runtime_error("sampled pixel mismatch");
}

int main(int argc, char **argv)
{
   const bool implicit = argc == 4 && !strcmp(argv[3], "--implicit-release");
   const bool shared = argc > 2 && !strcmp(argv[argc - 1], "--shared-contract");
   const bool features = argc > 2 && !strcmp(argv[argc - 1], "--feature-levels");
   const bool sample = argc > 2 && !strcmp(argv[argc - 1], "--sample");
   const bool hardware = (argc == 3 || implicit || (argc == 4 && (shared || features || sample))) && !strcmp(argv[1], "--luid");
   const bool software = (argc == 3 || (argc == 4 && (features || sample))) && !strcmp(argv[1], "--software");
   const bool warp = (argc == 2 || (argc == 3 && (shared || features || sample))) && !strcmp(argv[1], "--warp");
   if (!hardware && !software && !warp) {
      fprintf(stderr, "usage: %s --luid 16-hex-digits [--implicit-release|--shared-contract|--feature-levels|--sample] | --software dll [--feature-levels|--sample] | --warp [--shared-contract|--feature-levels|--sample]\n", argv[0]);
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
         shared_contract(adapter.Get(), hardware, requested, device.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, false);
         shared_contract(adapter.Get(), hardware, requested, device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, false);
         shared_contract(adapter.Get(), hardware, requested, device.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, true);
         shared_contract(adapter.Get(), hardware, requested, device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, true);
         check(device->GetDeviceRemovedReason(), "Consumer status after sharing");
      } else if (sample) {
         sampled_images(device.Get(), context.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
         sampled_images(device.Get(), context.Get(), DXGI_FORMAT_B8G8R8A8_UNORM);
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
      marker(shared ? "Windows D3D shared texture contents and producer-device lifetime passed; presentation was not exercised" :
                      "Windows D3D runtime clear/draw/readback passed; sharing and presentation were not exercised");
   return result;
}
