/* SPDX-License-Identifier: MIT
 * CPU controls of the production DDI code. No Vulkan driver is loaded.
 */
#include "tgd3d.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <d3dcompiler.h>
#include "gallium/targets/d3d10umd/tests/tri_vs_4_0.h"
#include "gallium/targets/d3d10umd/tests/tri_ps_4_0.h"

using namespace tgd3d;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%u: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)
template<typename T> static T handle(uintptr_t n) { return (T)n; }
template<typename T> struct Stub;
template<typename R, typename... A> struct Stub<R(VKAPI_PTR *)(A...)> {
   static R VKAPI_CALL call(A...) { if constexpr (!std::is_void_v<R>) return R{}; }
};
static uint64_t completedValue;
static unsigned signals, submissions, gates, cpuWaits, cpuSignals, barriers, viewsDestroyed;
static unsigned failAt, layouts, descriptorSets, updates;
static HRESULT lastError;
static std::vector<unsigned> events;
static std::vector<uint32_t> cbBindings;
static VkDeviceSize cbOffsets[3];
static const char *shaderOutput;

static Device make_device(Adapter *adapter)
{
   Device dev;
   dev.adapter = adapter;
   adapter->vk.caps.cbSlots = D3D_CB_SLOTS;
   adapter->vk.caps.geometryShader = true;
   adapter->vk.caps.maxViewports = 16;
   dev.caps = &adapter->vk.caps;
   dev.vkdev = handle<VkDevice>(1);
   dev.queue = handle<VkQueue>(2);
   dev.fence = handle<VkSemaphore>(3);
   dev.ingress = handle<VkSemaphore>(4);
   dev.runtimeFence = 0x20;
   dev.runtimeIngress = 0x21;
   dev.hContext = handle<HANDLE>(0x3000);
   dev.hRTDevice.handle = handle<HANDLE>(0x4000);
#define X(n) dev.vk.n = Stub<PFN_vk##n>::call;
   TGD3D_VK_DEVICE_FUNCS(X)
   TGD3D_VK_OPTIONAL_DEVICE_FUNCS(X)
#undef X
   dev.um.pfnSetErrorCb = [](D3D10DDI_HRTCORELAYER, HRESULT hr) { lastError = hr; };
   dev.vk.AllocateCommandBuffers = [](VkDevice, const VkCommandBufferAllocateInfo *, VkCommandBuffer *out) {
      *out = handle<VkCommandBuffer>(0x100); return VK_SUCCESS;
   };
   dev.vk.GetSemaphoreCounterValue = [](VkDevice, VkSemaphore, uint64_t *out) {
      *out = completedValue; return VK_SUCCESS;
   };
   dev.vk.WaitSemaphores = [](VkDevice, const VkSemaphoreWaitInfo *info, uint64_t) {
      ++cpuWaits; completedValue = *info->pValues; return VK_SUCCESS;
   };
   dev.vk.SignalSemaphore = [](VkDevice, const VkSemaphoreSignalInfo *) {
      ++cpuSignals; return VK_SUCCESS;
   };
   dev.vk.CmdPipelineBarrier2 = [](VkCommandBuffer cmd, const VkDependencyInfo *info) {
      CHECK(cmd && (info->memoryBarrierCount || info->imageMemoryBarrierCount)); ++barriers;
   };
   dev.kt.pfnSignalSynchronizationObjectFromGpuCb = [](HANDLE device,
         const D3DDDICB_SIGNALSYNCHRONIZATIONOBJECTFROMGPU *info) -> HRESULT {
      CHECK(device == handle<HANDLE>(0x4000) && info->hContext == handle<HANDLE>(0x3000));
      CHECK(info->ObjectCount == 1 && info->ObjectHandleArray[0] == 0x21);
      CHECK(info->MonitoredFenceValueArray[0] == submissions + 1);
      ++signals; events.push_back(1); return failAt == 1 ? E_FAIL : S_OK;
   };
   dev.vk.QueueSubmit2 = [](VkQueue, uint32_t count, const VkSubmitInfo2 *info, VkFence) {
      CHECK(count == 1 && info->commandBufferInfoCount == 1 && info->signalSemaphoreInfoCount == 1);
      CHECK(info->pSignalSemaphoreInfos[0].value == submissions + 1);
      CHECK(info->pSignalSemaphoreInfos[0].semaphore == handle<VkSemaphore>(3));
      CHECK(info->waitSemaphoreInfoCount == (submissions ? 2u : 1u));
      if (submissions) {
         CHECK(info->pWaitSemaphoreInfos[0].semaphore == handle<VkSemaphore>(3));
         CHECK(info->pWaitSemaphoreInfos[0].value == submissions);
      }
      const auto &wait = info->pWaitSemaphoreInfos[info->waitSemaphoreInfoCount - 1];
      CHECK(wait.semaphore == handle<VkSemaphore>(4) && wait.value == submissions + 1);
      CHECK(wait.stageMask == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
      ++submissions; events.push_back(2);
      return failAt == 2 ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
   };
   dev.kt.pfnWaitForSynchronizationObjectFromGpuCb = [](HANDLE,
         const D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMGPU *info) -> HRESULT {
      CHECK(info->hContext == handle<HANDLE>(0x3000) && info->ObjectCount == 1);
      CHECK(info->ObjectHandleArray[0] == 0x20 && info->MonitoredFenceValueArray[0] == submissions);
      ++gates; events.push_back(3); return failAt == 3 ? E_FAIL : S_OK;
   };
   dev.vk.DestroyImageView = [](VkDevice, VkImageView, const VkAllocationCallbacks *) {
      ++viewsDestroyed; events.push_back(4);
   };
   dev.vk.DestroyImage = [](VkDevice, VkImage, const VkAllocationCallbacks *) { events.push_back(5); };
   dev.vk.FreeMemory = [](VkDevice, VkDeviceMemory, const VkAllocationCallbacks *) { events.push_back(6); };
   return dev;
}

static void reset()
{
   completedValue = 0;
   signals = submissions = gates = cpuWaits = cpuSignals = barriers = viewsDestroyed = 0;
   failAt = layouts = descriptorSets = updates = 0;
   lastError = S_OK; events.clear(); cbBindings.clear();
}

static void submission_contract()
{
   Adapter adapter;
   reset(); Device dev = make_device(&adapter);
   for (unsigned i = 0; i < 4; i++) {
      CHECK(cmd_begin(&dev));
      CHECK(SUCCEEDED(flush(&dev, false)));
   }
   CHECK(dev.submitted == 4 && dev.gated == 4 && !cpuWaits && !cpuSignals);
   CHECK(events == std::vector<unsigned>({1,2,3,1,2,3,1,2,3,1,2,3}));
   CHECK(barriers >= 8);
   completedValue = 4; retire(&dev);
   CHECK(dev.submittedCommands.empty());
   for (unsigned failure = 1; failure <= 3; failure++) {
      reset(); dev = make_device(&adapter); failAt = failure;
      CHECK(cmd_begin(&dev)); CHECK(FAILED(flush(&dev, false)));
      CHECK(FAILED(dev.lost) && !cpuSignals);
      CHECK(submissions == (failure >= 2) && gates == (failure == 3));
      CHECK(dev.submitted == (failure == 3));
      const unsigned previous = signals;
      CHECK(FAILED(flush(&dev, true)) && signals == previous);
   }
   std::puts("PASS: every submit is bidirectional; failures never fabricate completion");
}

static void nonblocking_map()
{
   reset(); Adapter adapter; Device dev = make_device(&adapter);
   D3D10DDI_DEVICEFUNCS table = {}; resource_fill_table(&table);
   Chunk chunk; uint8_t bytes[64] = {}; chunk.cpu = bytes;
   Resource res; res.dev = &dev; res.size = sizeof(bytes); res.slice = {&chunk, 0, sizeof(bytes)};
   CHECK(cmd_begin(&dev)); res.sliceLastWrite = res.sliceLastUse = 1;
   D3D10DDI_MAPPED_SUBRESOURCE out = {};
   table.pfnStagingResourceMap({&dev}, {&res}, 0, D3D10_DDI_MAP_READ,
                              D3D10_DDI_MAP_FLAG_DONOTWAIT, &out);
   CHECK(!out.pData && lastError == DXGI_DDI_ERR_WASSTILLDRAWING);
   CHECK(!cpuWaits && dev.submitted == 1 && SUCCEEDED(dev.lost));
   completedValue = 1; lastError = S_OK;
   table.pfnStagingResourceMap({&dev}, {&res}, 0, D3D10_DDI_MAP_READ,
                              D3D10_DDI_MAP_FLAG_DONOTWAIT, &out);
   CHECK(out.pData == bytes && out.RowPitch == sizeof(bytes) && lastError == S_OK);
   std::puts("PASS: DONOTWAIT submits progress without a CPU wait");
}

static void rotated_views()
{
   reset(); Adapter adapter; Device dev = make_device(&adapter);
   Resource a, b; a.dev = b.dev = &dev;
   a.kind = b.kind = ResKind::Image;
   a.storage.image = handle<VkImage>(10); b.storage.image = handle<VkImage>(11);
   a.storage.memory = handle<VkDeviceMemory>(20); b.storage.memory = handle<VkDeviceMemory>(21);
   a.storage.lastUse = b.storage.lastUse = 0;
   View va, vb; va.res = &a; vb.res = &b;
   va.cache[0] = {a.storage.image, handle<VkImageView>(30), false};
   vb.cache[0] = {a.storage.image, handle<VkImageView>(31), false};
   dev.views = {&va, &vb};
   defer(&dev, DeferredType::ImageView, 32, 0, a.storage.image);
   DXGI1_1_DDI_BASE_FUNCTIONS table = {}; dxgi_fill_table(&table, true);
   DXGI_DDI_HRESOURCE resources[2] = {(DXGI_DDI_HRESOURCE)&a, (DXGI_DDI_HRESOURCE)&b};
   DXGI_DDI_ARG_ROTATE_RESOURCE_IDENTITIES rotate = {};
   rotate.hDevice = (DXGI_DDI_HDEVICE)&dev; rotate.Resources = 2; rotate.pResources = resources;
   CHECK(SUCCEEDED(table.pfnRotateResourceIdentities(&rotate)));
   CHECK(b.storage.image == handle<VkImage>(10));
   storage_destroy(&dev, b.storage, true);
   CHECK(events == std::vector<unsigned>({4,4,4,5,6}) && dev.deferred.empty());
   CHECK(!va.cache[0].view && !vb.cache[0].view);
   events.clear();
   a.storage.lastUse = 2;
   va.cache[0] = {a.storage.image, handle<VkImageView>(33), false};
   storage_destroy(&dev, a.storage, false);
   CHECK(events.empty() && dev.deferred.size() == 3);
   completedValue = 1; retire(&dev); CHECK(events.empty());
   completedValue = 2; retire(&dev);
   CHECK(events == std::vector<unsigned>({4,5,6}) && dev.deferred.empty());
   std::puts("PASS: rotated cached/deferred views die before their image and memory");
}

static void descriptor_contract()
{
   reset(); Adapter adapter; Device dev = make_device(&adapter);
   dev.vk.CreateDescriptorSetLayout = [](VkDevice, const VkDescriptorSetLayoutCreateInfo *info,
                                        const VkAllocationCallbacks *, VkDescriptorSetLayout *out) {
      CHECK(!(info->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR));
      if (!layouts) CHECK(info->bindingCount == 42);
      *out = handle<VkDescriptorSetLayout>(100 + layouts++); return VK_SUCCESS;
   };
   dev.vk.CreateDescriptorPool = [](VkDevice, const VkDescriptorPoolCreateInfo *,
                                   const VkAllocationCallbacks *, VkDescriptorPool *out) {
      *out = handle<VkDescriptorPool>(200); return VK_SUCCESS;
   };
   dev.vk.AllocateDescriptorSets = [](VkDevice, const VkDescriptorSetAllocateInfo *, VkDescriptorSet *out) {
      *out = handle<VkDescriptorSet>(300 + descriptorSets++); return VK_SUCCESS;
   };
   dev.vk.UpdateDescriptorSets = [](VkDevice, uint32_t count, const VkWriteDescriptorSet *writes,
                                   uint32_t, const VkCopyDescriptorSet *) {
      CHECK(count == 3); cbBindings.clear(); ++updates;
      for (unsigned i = 0; i < count; i++) {
         CHECK(writes[i].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
         cbBindings.push_back(writes[i].dstBinding); cbOffsets[i] = writes[i].pBufferInfo->offset;
      }
   };
   CHECK(SUCCEEDED(device_layouts_create(&dev)) && layouts == 3);
   Shader shaders[3]; Chunk chunk; chunk.buffer = handle<VkBuffer>(500);
   Resource cb[3];
   for (unsigned i = 0; i < 3; i++) {
      shaders[i].dev = &dev; shaders[i].stage = (Stage)i;
      shaders[i].cbMask = 1 << 13; shaders[i].variants.push_back({0, handle<VkShaderEXT>(600 + i)});
      dev.stage[i].shader = &shaders[i];
      cb[i].slice = {&chunk, i * 256, 64}; cb[i].size = 64;
      dev.stage[i].cb[13] = &cb[i];
   }
   Resource target; target.dev = &dev; target.kind = ResKind::Image;
   target.storage.image = handle<VkImage>(700); target.storage.layoutReady = true;
   View view; view.dev = &dev; view.res = &target; view.type = ViewType::RTV;
   view.width = view.height = 32; view.layers = 1;
   view.cache[0] = {target.storage.image, handle<VkImageView>(701), false};
   dev.rtv[0] = &view; dev.viewportCount = 1; dev.viewports[0].Width = dev.viewports[0].Height = 32;
   D3D10DDI_DEVICEFUNCS table = {}; draw_fill_table(&table);
   table.pfnDraw({&dev}, 3, 0);
   CHECK(updates == 1 && cbBindings == std::vector<uint32_t>({13,27,41}));
   CHECK(cbOffsets[0] == 0 && cbOffsets[1] == 256 && cbOffsets[2] == 512);
   table.pfnDraw({&dev}, 3, 0); CHECK(updates == 1);
   cb[0].slice.offset = 768; dev.stage[0].cbDirty = 1 << 13;
   table.pfnDraw({&dev}, 3, 0);
   CHECK(updates == 2 && cbOffsets[0] == 768 && cbOffsets[1] == 256 && cbOffsets[2] == 512);
   std::puts("PASS: all 42 constant-buffer slots; immutable full snapshots after one-slot changes");
}

static uint32_t read32(const BYTE *p) { uint32_t v; memcpy(&v, p, 4); return v; }

static void query_contract()
{
   reset(); Adapter adapter; Device dev = make_device(&adapter);
   D3D10DDI_DEVICEFUNCS table = {}; query_fill_table(&table);
   dev.vk.CreateQueryPool = [](VkDevice, const VkQueryPoolCreateInfo *,
                               const VkAllocationCallbacks *, VkQueryPool *out) {
      static uintptr_t next = 1; *out = handle<VkQueryPool>(next++); return VK_SUCCESS;
   };
   dev.vk.GetQueryPoolResults = [](VkDevice, VkQueryPool, uint32_t, uint32_t count,
                                   size_t size, void *data, VkDeviceSize stride, VkQueryResultFlags) {
      CHECK(size == count * 8 && stride == 8);
      for (uint32_t i = 0; i < count; i++) static_cast<uint64_t *>(data)[i] = 1;
      return VK_SUCCESS;
   };
   alignas(Query) BYTE storage[sizeof(Query)]; D3D10DDIARG_CREATEQUERY desc = {};
   desc.Query = D3D10DDI_QUERY_OCCLUSION;
   table.pfnCreateQuery({&dev}, &desc, {storage}, {});
   table.pfnQueryBegin({&dev}, {storage});
   VkCommandBuffer cmd = cmd_draw(&dev);
   for (unsigned i = 0; i < 129; i++) { query_pass_begin(&dev, cmd); query_pass_end(&dev, cmd); }
   table.pfnQueryEnd({&dev}, {storage});
   uint64_t value = 0;
   table.pfnQueryGetData({&dev}, {storage}, &value, sizeof(value), D3D10_DDI_GET_DATA_DO_NOT_FLUSH);
   CHECK(lastError == DXGI_DDI_ERR_WASSTILLDRAWING && !submissions && !cpuWaits);
   lastError = S_OK;
   table.pfnQueryGetData({&dev}, {storage}, &value, sizeof(value), 0);
   CHECK(lastError == DXGI_DDI_ERR_WASSTILLDRAWING && submissions == 1 && !cpuWaits);
   completedValue = 1; lastError = S_OK;
   table.pfnQueryGetData({&dev}, {storage}, &value, sizeof(value), 0);
   CHECK(lastError == S_OK && value == 129);
   table.pfnDestroyQuery({&dev}, {storage}); retire(&dev);
   CHECK(dev.deferred.empty());
   std::puts("PASS: nonblocking query polling; all 129 render passes counted across pool rollover");
}

static void shader_contract(const BYTE *dxbc, bool vertex, const char *outputPath, bool sampled = false)
{
   reset(); Adapter adapter; Device dev = make_device(&adapter);
   shaderOutput = outputPath;
   dev.vk.CreateShadersEXT = [](VkDevice, uint32_t count, const VkShaderCreateInfoEXT *info,
                               const VkAllocationCallbacks *, VkShaderEXT *out) {
      CHECK(count == 1 && info->codeSize > 20 && info->codeType == VK_SHADER_CODE_TYPE_SPIRV_EXT);
      CHECK(static_cast<const uint32_t *>(info->pCode)[0] == 0x07230203);
      FILE *file = fopen(shaderOutput, "wb"); CHECK(file);
      CHECK(fwrite(info->pCode, 1, info->codeSize, file) == info->codeSize); CHECK(!fclose(file));
      *out = handle<VkShaderEXT>(1); return VK_SUCCESS;
   };
   std::vector<D3D10DDIARG_SIGNATURE_ENTRY> input, output;
   std::vector<UINT> tokens;
   for (UINT c = 0; c < read32(dxbc + 28); c++) {
      const BYTE *chunk = dxbc + read32(dxbc + 32 + 4 * c);
      const BYTE *data = chunk + 8;
      if (!memcmp(chunk, "SHDR", 4) || !memcmp(chunk, "SHEX", 4)) {
         tokens.resize(read32(chunk + 4) / 4); memcpy(tokens.data(), data, read32(chunk + 4));
      } else if (!memcmp(chunk, "ISGN", 4) || !memcmp(chunk, "OSGN", 4)) {
         auto &entries = chunk[0] == 'I' ? input : output;
         for (UINT i = 0; i < read32(data); i++) {
            const BYTE *entry = data + 8 + 24 * i;
            entries.push_back({(D3D10_SB_NAME)read32(entry + 8), read32(entry + 16), entry[20]});
         }
      }
   }
   CHECK(!tokens.empty());
   D3D10DDIARG_STAGE_IO_SIGNATURES sig = {input.data(), (UINT)input.size(), output.data(), (UINT)output.size()};
   D3D10DDI_DEVICEFUNCS table = {}; shader_fill_table(&table);
   alignas(Shader) BYTE storage[sizeof(Shader)];
   auto create = vertex ? table.pfnCreateVertexShader : table.pfnCreatePixelShader;
   create({&dev}, tokens.data(), {storage}, {}, &sig);
   CHECK(SUCCEEDED(lastError) && reinterpret_cast<Shader *>(storage)->variants.size() == 1);
   if (sampled) {
      const Shader *shader = reinterpret_cast<Shader *>(storage);
      CHECK(shader->cbMask == (1u << 13) && shader->srvMask[0] == 1 && shader->samplerMask == 1);
   }
   table.pfnDestroyShader({&dev}, {storage}); retire(&dev, true);
   std::puts(vertex ? "PASS: real SM4 VS DDI-to-SPIR-V conversion" : "PASS: real SM4 PS DDI-to-SPIR-V conversion");
}

static void sampled_shader_contract()
{
   HMODULE module = LoadLibraryW(L"d3dcompiler_47.dll"); CHECK(module);
   auto compile = reinterpret_cast<pD3DCompile>(reinterpret_cast<void *>(GetProcAddress(module, "D3DCompile")));
   CHECK(compile);
   const char *source =
      "Texture2D<float4> tex:register(t0); SamplerState samp:register(s0);"
      "cbuffer C:register(b13){float4 color;}"
      "float4 main(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target {"
      "return tex.Sample(samp,uv)*color;}";
   ID3DBlob *code = nullptr, *error = nullptr;
   HRESULT hr = compile(source, strlen(source), "tgd3d-control", nullptr, nullptr,
                        "main", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error);
   if (error) { std::fwrite(error->GetBufferPointer(), 1, error->GetBufferSize(), stderr); error->Release(); }
   CHECK(SUCCEEDED(hr) && code);
   shader_contract(static_cast<const BYTE *>(code->GetBufferPointer()), false, "tgd3d-sample.spv", true);
   code->Release(); FreeLibrary(module);
   std::puts("PASS: SM4 sampled image, sampler and constant buffer 13 reflection and SPIR-V");
}

void runtime_contracts();

int main()
{
   submission_contract(); nonblocking_map(); rotated_views(); descriptor_contract();
   query_contract();
   shader_contract(g_VS, true, "tgd3d-vs.spv"); shader_contract(g_PS, false, "tgd3d-ps.spv");
   sampled_shader_contract();
   runtime_contracts();
   std::puts("CPU controls passed; no GPU execution, WDDM runtime or physical display tested.");
}
