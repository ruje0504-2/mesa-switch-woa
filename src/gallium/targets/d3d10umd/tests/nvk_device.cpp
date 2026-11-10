/* SPDX-License-Identifier: MIT */
/* Real DDI frontend with a controlled Gallium backend; no GPU execution. */
#include "DriverIncludes.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { \
   fprintf(stderr, "line %u: %s\n", __LINE__, #x); exit(1); \
} } while (0)

extern "C" HRESULT APIENTRY OpenAdapter10_2(D3D10DDIARG_OPENADAPTER *);

static pipe_screen screen = {};
static pipe_context context = {};
static unsigned mode, shaders, contexts, screen_destroys;
static unsigned vs, fs;
static HRESULT resource_error;
static bool allocate_resource;
static unsigned resources;
static pipe_resource buffer = {};
static bool device_removed, remove_during_map;
static unsigned maps, unmaps;
static pipe_transfer transfer = {};
static unsigned char mapped_bytes[16];

static void APIENTRY set_error(D3D10DDI_HRTCORELAYER, HRESULT error)
{
   resource_error = error;
}

static void *create_shader(pipe_context *, const pipe_shader_state *state, unsigned stage)
{
   CHECK(state->tokens);
   if (mode == stage)
      return NULL;
   ++shaders;
   return stage == 1 ? &vs : &fs;
}

extern "C" HRESULT
d3d10_create_screen(HANDLE, const D3DDDI_ADAPTERCALLBACKS *, pipe_screen **result)
{
   screen.destroy = [](pipe_screen *) { ++screen_destroys; };
   screen.resource_create = [](pipe_screen *s, const pipe_resource *templ) -> pipe_resource * {
      if (!allocate_resource)
         return NULL;
      CHECK(!resources);
      ++resources;
      buffer = *templ;
      buffer.screen = s;
      pipe_reference_init(&buffer.reference, 1);
      return &buffer;
   };
   screen.resource_destroy = [](pipe_screen *, pipe_resource *) { CHECK(resources); --resources; };
   screen.is_format_supported = [](pipe_screen *, pipe_format, pipe_texture_target, unsigned, unsigned, unsigned) { return true; };
   screen.context_create = [](pipe_screen *screen, void *, unsigned) -> pipe_context * {
      if (!mode)
         return NULL;
      ++contexts;
      context.screen = screen;
      context.destroy = [](pipe_context *) { CHECK(contexts); --contexts; };
      context.create_vs_state = [](pipe_context *p, const pipe_shader_state *s) { return create_shader(p, s, 1); };
      context.create_fs_state = [](pipe_context *p, const pipe_shader_state *s) { return create_shader(p, s, 2); };
      context.delete_vs_state = context.delete_fs_state = [](pipe_context *, void *s) {
         CHECK(shaders && (s == &vs || s == &fs)); --shaders;
      };
      context.bind_vs_state = context.bind_fs_state = [](pipe_context *, void *) {};
      context.bind_blend_state = context.bind_rasterizer_state = context.bind_vs_state;
      context.bind_depth_stencil_alpha_state = context.bind_vertex_elements_state = context.bind_vs_state;
      context.flush = [](pipe_context *, pipe_fence_handle **, unsigned) {};
      context.set_stencil_ref = [](pipe_context *, pipe_stencil_ref) {};
      context.set_constant_buffer = [](pipe_context *, mesa_shader_stage, unsigned, const pipe_constant_buffer *) {};
      context.set_vertex_buffers = [](pipe_context *, unsigned, const pipe_vertex_buffer *) {};
      context.set_framebuffer_state = [](pipe_context *, const pipe_framebuffer_state *) {};
      context.set_sample_mask = [](pipe_context *, unsigned) {};
      context.set_sampler_views = [](pipe_context *, mesa_shader_stage, unsigned, unsigned, unsigned, pipe_sampler_view **) {};
      context.get_device_reset_status = [](pipe_context *) {
         return device_removed ? PIPE_GUILTY_CONTEXT_RESET : PIPE_NO_RESET;
      };
      context.buffer_map = context.texture_map = [](pipe_context *, pipe_resource *, unsigned, unsigned, const pipe_box *, pipe_transfer **out) -> void * {
         ++maps;
         if (!remove_during_map) return NULL;
         device_removed = true;
         *out = &transfer;
         return mapped_bytes;
      };
      context.buffer_unmap = context.texture_unmap = [](pipe_context *, pipe_transfer *t) {
         CHECK(t == &transfer); ++unmaps;
      };
      return &context;
   };
   *result = &screen;
   return S_OK;
}

int main(void)
{
   D3D10_2DDI_ADAPTERFUNCS adapter_functions = {};
   D3D10DDIARG_OPENADAPTER open = {};
   open.pAdapterFuncs_2 = &adapter_functions;
   CHECK(OpenAdapter10_2(&open) == S_OK && open.hAdapter.pDrvPrivate);
   UINT count = 0;
   CHECK(adapter_functions.pfnGetSupportedVersions(open.hAdapter, &count, NULL) == S_OK && count);
   UINT64 versions[8] = {};
   CHECK(count <= ARRAYSIZE(versions));
   CHECK(adapter_functions.pfnGetSupportedVersions(open.hAdapter, &count, versions) == S_OK);
   bool found = false;
   for (unsigned i = 0; i < count; ++i)
      found |= versions[i] == D3D10_0_DDI_SUPPORTED;
   CHECK(found);

   D3D10DDIARG_CALCPRIVATEDEVICESIZE size = {};
   size.Interface = D3D10_0_DDI_INTERFACE_VERSION;
   const SIZE_T bytes = adapter_functions.pfnCalcPrivateDeviceSize(open.hAdapter, &size);
   CHECK(bytes);
   D3DDDI_DEVICECALLBACKS kernel_callbacks = {};
   D3D10DDI_CORELAYER_DEVICECALLBACKS runtime_callbacks = {};
   runtime_callbacks.pfnSetErrorCb = set_error;
   DXGI_DDI_BASE_FUNCTIONS dxgi = {};
   D3D10DDI_DEVICEFUNCS functions = {};
   D3D10DDIARG_CREATEDEVICE create = {};
   create.Interface = size.Interface;
   create.hDrvDevice.pDrvPrivate = calloc(1, bytes);
   CHECK(create.hDrvDevice.pDrvPrivate);
   create.pDeviceFuncs = &functions;
   create.pKTCallbacks = &kernel_callbacks;
   create.pUMCallbacks = &runtime_callbacks;
   create.DXGIBaseDDI.pDXGIDDIBaseFunctions = &dxgi;
   for (mode = 0; mode < 3; ++mode) {
      CHECK(adapter_functions.pfnCreateDevice(open.hAdapter, &create) == E_OUTOFMEMORY);
      CHECK(!shaders && !contexts && !functions.pfnDestroyDevice);
   }
   CHECK(adapter_functions.pfnCreateDevice(open.hAdapter, &create) == S_OK);
   CHECK(contexts == 1 && shaders == 2 && functions.pfnCreateResource && functions.pfnDraw);
   D3D10DDI_MIPINFO mip = {};
   mip.TexelWidth = 16;
   mip.TexelHeight = mip.TexelDepth = 1;
   D3D10DDIARG_CREATERESOURCE resource = {};
   resource.ResourceDimension = D3D10DDIRESOURCE_BUFFER;
   resource.pMipInfoList = &mip;
   resource.MipLevels = resource.ArraySize = resource.SampleDesc.Count = 1;
   unsigned char initial_data[16] = {};
   D3D10_DDIARG_SUBRESOURCE_UP initial = {};
   initial.pSysMem = initial_data;
   resource.pInitialDataUP = &initial;
   D3D10DDI_HRESOURCE handle = {};
   handle.pDrvPrivate = calloc(1, functions.pfnCalcPrivateResourceSize(create.hDrvDevice, &resource));
   CHECK(handle.pDrvPrivate);
   functions.pfnCreateResource(create.hDrvDevice, &resource, handle, {});
   CHECK(resource_error == E_OUTOFMEMORY && !resources);
   allocate_resource = true;
   resource_error = S_OK;
   functions.pfnCreateResource(create.hDrvDevice, &resource, handle, {});
   CHECK(resource_error == E_OUTOFMEMORY && !resources); /* Initial buffer map fails. */
   resource.ResourceDimension = D3D10DDIRESOURCE_TEXTURE2D;
   resource.Format = DXGI_FORMAT_R8_UNORM;
   resource_error = S_OK;
   functions.pfnCreateResource(create.hDrvDevice, &resource, handle, {});
   CHECK(resource_error == E_OUTOFMEMORY && !resources); /* Initial texture map fails. */
   resource.pInitialDataUP = NULL;
   resource.Usage = D3D10_DDI_USAGE_STAGING;
   resource.MapFlags = D3D10_DDI_CPU_ACCESS_READ;
   resource_error = S_OK;
   functions.pfnCreateResource(create.hDrvDevice, &resource, handle, {});
   CHECK(resource_error == S_OK && resources == 1);
   const unsigned previous_maps = maps;
   device_removed = true;
   D3D10DDI_MAPPED_SUBRESOURCE mapped;
   memset(&mapped, 0xa5, sizeof(mapped));
   functions.pfnStagingResourceMap(create.hDrvDevice, handle, 0, D3D10_DDI_MAP_READ, 0, &mapped);
   CHECK(resource_error == D3DDDIERR_DEVICEREMOVED && !mapped.pData && !mapped.RowPitch &&
         !mapped.DepthPitch && maps == previous_maps);
   resource_error = S_OK; device_removed = false; remove_during_map = true;
   functions.pfnStagingResourceMap(create.hDrvDevice, handle, 0, D3D10_DDI_MAP_READ, 0, &mapped);
   CHECK(resource_error == D3DDDIERR_DEVICEREMOVED && !mapped.pData &&
         maps == previous_maps + 1 && unmaps == 1);
   functions.pfnDestroyResource(create.hDrvDevice, handle);
   CHECK(!resources && unmaps == 1);
   free(handle.pDrvPrivate);
   DXGI_DDI_ARG_PRESENT present = {};
   present.hDevice = reinterpret_cast<DXGI_DDI_HDEVICE>(create.hDrvDevice.pDrvPrivate);
   CHECK(dxgi.pfnPresent(&present) == DXGI_ERROR_UNSUPPORTED);
   functions.pfnDestroyDevice(create.hDrvDevice);
   CHECK(!contexts && !shaders);
   free(create.hDrvDevice.pDrvPrivate);
   CHECK(adapter_functions.pfnCloseAdapter(open.hAdapter) == S_OK && screen_destroys == 1);
   puts("D3D UMD adapter/device negotiation, failure cleanup and destruction pass (controlled Gallium backend)");
   return 0;
}
