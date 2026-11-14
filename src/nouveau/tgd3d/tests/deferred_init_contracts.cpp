/* SPDX-License-Identifier: MIT
 * Exercise real deferred submission code with explicit runtime/Vulkan stubs.
 * No WDDM device, Vulkan driver or GPU is used by this control.
 */
#include "tgd3d.h"
#include <cstdio>
#include <cstdlib>
#include <type_traits>

using namespace tgd3d;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%u: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)

#ifdef TGD3D_DEFERRED_INIT
namespace {
template<typename T> T handle(uintptr_t n) { return (T)n; }
template<typename T> struct Stub;
template<typename R, typename... A> struct Stub<R(VKAPI_PTR *)(A...)> {
   static R VKAPI_CALL call(A...) { if constexpr (!std::is_void_v<R>) return R{}; }
};
unsigned semCreates, objectCreates, imports, semDestroys, objectDestroys;
unsigned submits, signals, gates, waits, failImport;
uint64_t done, initialValue;
DWORD caller;
VkResult waitResult;
HRESULT reported;
unsigned layoutStep, failLayoutStep, layoutDestroyed;

Device make_device(Adapter &adapter)
{
   semCreates = objectCreates = imports = semDestroys = objectDestroys = 0;
   submits = signals = gates = waits = failImport = 0;
   done = initialValue = 0;
   waitResult = VK_SUCCESS; reported = S_OK; caller = GetCurrentThreadId();
   Device dev;
   dev.adapter = &adapter; dev.caps = &adapter.vk.caps; dev.nvk = true;
   dev.vkdev = handle<VkDevice>(1); dev.queue = handle<VkQueue>(2);
   dev.hRTDevice.handle = handle<HANDLE>(0x4000); dev.hContext = handle<HANDLE>(0x3000);
#define X(n) dev.vk.n = Stub<PFN_vk##n>::call;
   TGD3D_VK_DEVICE_FUNCS(X)
   TGD3D_VK_OPTIONAL_DEVICE_FUNCS(X)
#undef X
   dev.um.pfnSetErrorCb = [](D3D10DDI_HRTCORELAYER, HRESULT hr) { reported = hr; };
   dev.vk.CreateSemaphore = [](VkDevice, const VkSemaphoreCreateInfo *info,
                               const VkAllocationCallbacks *, VkSemaphore *out) {
      CHECK(static_cast<const VkSemaphoreTypeCreateInfo *>(info->pNext)->semaphoreType ==
            VK_SEMAPHORE_TYPE_TIMELINE);
      *out = handle<VkSemaphore>(3 + semCreates++); return VK_SUCCESS;
   };
   dev.vk.DestroySemaphore = [](VkDevice, VkSemaphore semaphore, const VkAllocationCallbacks *) {
      CHECK(semaphore); ++semDestroys;
   };
   dev.vk.GetSemaphoreCounterValue = [](VkDevice, VkSemaphore semaphore, uint64_t *value) {
      CHECK(semaphore == handle<VkSemaphore>(3)); *value = done; return VK_SUCCESS;
   };
   dev.vk.WaitSemaphores = [](VkDevice, const VkSemaphoreWaitInfo *info, uint64_t) {
      CHECK(GetCurrentThreadId() == caller && info->semaphoreCount == 1);
      CHECK(info->pSemaphores[0] == handle<VkSemaphore>(3)); ++waits;
      if (waitResult == VK_SUCCESS) done = info->pValues[0];
      return waitResult;
   };
   dev.vk.SignalSemaphore = [](VkDevice, const VkSemaphoreSignalInfo *) -> VkResult {
      CHECK(false); return VK_ERROR_UNKNOWN; /* no fabricated CPU completion */
   };
   dev.vk.AllocateCommandBuffers = [](VkDevice, const VkCommandBufferAllocateInfo *, VkCommandBuffer *out) {
      *out = handle<VkCommandBuffer>(0x100); return VK_SUCCESS;
   };
   dev.shareObjects = [](UINT count, const D3DKMT_HANDLE *, void *, DWORD, HANDLE *out) -> LONG {
      CHECK(count == 1 && GetCurrentThreadId() == caller);
      *out = CreateEventW(nullptr, FALSE, FALSE, nullptr); CHECK(*out); return 0;
   };
   dev.kt.pfnCreateSynchronizationObject2Cb = [](HANDLE, D3DDDICB_CREATESYNCHRONIZATIONOBJECT2 *info) -> HRESULT {
      CHECK(GetCurrentThreadId() == caller && info->Info.Type == D3DDDI_MONITORED_FENCE);
      CHECK(info->Info.MonitoredFence.InitialFenceValue == submits && done >= submits);
      initialValue = info->Info.MonitoredFence.InitialFenceValue;
      info->Info.MonitoredFence.FenceValueCPUVirtualAddress = &initialValue;
      info->hSyncObject = 0x20 + objectCreates++; return S_OK;
   };
   dev.kt.pfnDestroySynchronizationObjectCb = [](HANDLE,
         const D3DDDICB_DESTROYSYNCHRONIZATIONOBJECT *info) -> HRESULT {
      CHECK(GetCurrentThreadId() == caller && info->hSyncObject); ++objectDestroys; return S_OK;
   };
   dev.vk.ImportSemaphoreWin32HandleKHR = [](VkDevice, const VkImportSemaphoreWin32HandleInfoKHR *info) {
      CHECK(GetCurrentThreadId() == caller && info->handle && info->semaphore != handle<VkSemaphore>(3));
      CHECK(info->handleType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT);
      ++imports; return imports == failImport ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_SUCCESS;
   };
   dev.kt.pfnSignalSynchronizationObjectFromGpuCb = [](HANDLE,
         const D3DDDICB_SIGNALSYNCHRONIZATIONOBJECTFROMGPU *info) -> HRESULT {
      CHECK(GetCurrentThreadId() == caller && info->MonitoredFenceValueArray[0] == submits + 1);
      CHECK(info->ObjectHandleArray[0] == 0x21); ++signals; return S_OK;
   };
   dev.kt.pfnWaitForSynchronizationObjectFromGpuCb = [](HANDLE,
         const D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMGPU *info) -> HRESULT {
      CHECK(GetCurrentThreadId() == caller && info->MonitoredFenceValueArray[0] == submits);
      CHECK(info->ObjectHandleArray[0] == 0x20); ++gates; return S_OK;
   };
   dev.vk.QueueSubmit2 = [](VkQueue, uint32_t count, const VkSubmitInfo2 *info, VkFence) {
      CHECK(count == 1 && info->commandBufferInfoCount == 1);
      const bool bridged = info->signalSemaphoreInfoCount == 2;
      CHECK(info->signalSemaphoreInfoCount == (bridged ? 2u : 1u));
      CHECK(info->pSignalSemaphoreInfos[0].semaphore == handle<VkSemaphore>(3));
      CHECK(info->pSignalSemaphoreInfos[0].value == submits + 1);
      CHECK(info->waitSemaphoreInfoCount == unsigned(submits != 0) + unsigned(bridged));
      if (submits) {
         CHECK(info->pWaitSemaphoreInfos[0].semaphore == handle<VkSemaphore>(3));
         CHECK(info->pWaitSemaphoreInfos[0].value == submits);
      }
      if (bridged) {
         CHECK(signals == gates + 1);
         CHECK(info->pSignalSemaphoreInfos[1].semaphore == handle<VkSemaphore>(4));
         CHECK(info->pSignalSemaphoreInfos[1].value == submits + 1);
         CHECK(info->pWaitSemaphoreInfos[info->waitSemaphoreInfoCount - 1].semaphore == handle<VkSemaphore>(5));
         CHECK(info->pWaitSemaphoreInfos[info->waitSemaphoreInfoCount - 1].value == submits + 1);
      }
      ++submits; return VK_SUCCESS;
   };
   return dev;
}
}
#endif

void deferred_init_contracts()
{
#ifdef TGD3D_DEFERRED_INIT
   HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, TGD3D_DESKTOP_READY_EVENT);
   CHECK(ready && GetLastError() != ERROR_ALREADY_EXISTS);
   Adapter adapter;
   Device dev = make_device(adapter);
   CHECK(SUCCEEDED(sync_create(&dev)) && dev.bootstrapSync && semCreates == 1 && !objectCreates);
   CHECK(cmd_begin(&dev) && SUCCEEDED(flush(&dev, true)));
   CHECK(submits == 1 && waits == 1 && !imports && !signals && !gates);
   CHECK(SetEvent(ready));
   CHECK(cmd_begin(&dev) && SUCCEEDED(flush(&dev, false)));
   CHECK(submits == 2 && waits == 1 && !imports); /* ready, but nonblocking */
   CHECK(SUCCEEDED(flush(&dev, true)));
   CHECK(!dev.bootstrapSync && waits == 2 && imports == 2 && semCreates == 3);
   CHECK(!semDestroys && dev.fence == handle<VkSemaphore>(3));
   CHECK(dev.submitted == 2 && dev.gated == 2 && initialValue == 2);
   CHECK(cmd_begin(&dev) && SUCCEEDED(flush(&dev, false)));
   CHECK(submits == 3 && signals == 1 && gates == 1 && waits == 2);
   done = 3; retire(&dev); sync_destroy(&dev);
   CHECK(semDestroys == 3 && objectDestroys == 2 && !dev.desktopReadyEvent);

   for (unsigned fail : {1u, 2u}) {
      dev = make_device(adapter);
      CHECK(SUCCEEDED(sync_create(&dev)));
      CHECK(cmd_begin(&dev) && SUCCEEDED(flush(&dev, false)));
      failImport = fail;
      CHECK(flush(&dev, true) == E_OUTOFMEMORY && reported == E_OUTOFMEMORY);
      CHECK(dev.bootstrapSync && dev.fence == handle<VkSemaphore>(3));
      CHECK(!dev.bridgeFence && !dev.ingress && !dev.runtimeFence && !dev.runtimeIngress);
      CHECK(semDestroys == fail && objectDestroys == fail && !signals && !gates);
      CHECK(done == 1 && dev.submitted == 1);
      retire(&dev); sync_destroy(&dev);
      CHECK(semDestroys == fail + 1 && objectDestroys == fail);
   }
   dev = make_device(adapter);
   CHECK(SUCCEEDED(sync_create(&dev)));
   CHECK(cmd_begin(&dev) && SUCCEEDED(flush(&dev, false)));
   waitResult = VK_TIMEOUT;
   CHECK(FAILED(flush(&dev, true)) && FAILED(dev.lost));
   CHECK(!imports && !objectCreates && done == 0 && dev.submittedCommands.size() == 1);
   CHECK(!semDestroys && dev.fence == handle<VkSemaphore>(3));
   /* Explicitly complete the simulated GPU before releasing its objects. */
   done = 1; retire(&dev); sync_destroy(&dev);
   CloseHandle(ready);
   std::puts("PASS: desktop-gated bridge, preserved local timeline, nonblocking polling, partial-import cleanup and timeout ownership");
   for (unsigned fail = 1; fail <= 5; fail++) {
      dev = make_device(adapter);
      layoutStep = layoutDestroyed = 0; failLayoutStep = fail;
      dev.vk.CreateDescriptorSetLayout = [](VkDevice, const VkDescriptorSetLayoutCreateInfo *,
            const VkAllocationCallbacks *, VkDescriptorSetLayout *out) {
         if (++layoutStep == failLayoutStep) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
         *out = handle<VkDescriptorSetLayout>(layoutStep); return VK_SUCCESS;
      };
      dev.vk.CreatePipelineLayout = [](VkDevice, const VkPipelineLayoutCreateInfo *,
            const VkAllocationCallbacks *, VkPipelineLayout *out) {
         if (++layoutStep == failLayoutStep) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
         *out = handle<VkPipelineLayout>(layoutStep); return VK_SUCCESS;
      };
      dev.vk.CreateSampler = [](VkDevice, const VkSamplerCreateInfo *,
            const VkAllocationCallbacks *, VkSampler *out) {
         if (++layoutStep == failLayoutStep) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
         *out = handle<VkSampler>(layoutStep); return VK_SUCCESS;
      };
      dev.vk.DestroyDescriptorSetLayout = [](VkDevice, VkDescriptorSetLayout, const VkAllocationCallbacks *) { ++layoutDestroyed; };
      dev.vk.DestroyPipelineLayout = [](VkDevice, VkPipelineLayout, const VkAllocationCallbacks *) { ++layoutDestroyed; };
      dev.vk.DestroySampler = [](VkDevice, VkSampler, const VkAllocationCallbacks *) { ++layoutDestroyed; };
      CHECK(device_layouts_create(&dev) == E_OUTOFMEMORY && layoutDestroyed == fail - 1);
      CHECK(!dev.pipelineLayout && !dev.defaultSampler);
      for (auto layout : dev.setLayouts) CHECK(!layout);
      layoutStep = failLayoutStep = 0;
      CHECK(SUCCEEDED(device_layouts_create(&dev)) && layoutStep == 5);
      CHECK(SUCCEEDED(device_layouts_create(&dev)) && layoutStep == 5); /* reuse */
      device_layouts_destroy(&dev);
      CHECK(layoutDestroyed == fail + 4);
   }
   std::puts("PASS: lazy layouts clean every partial creation failure and retry without leaked or duplicate objects");
#endif
}
