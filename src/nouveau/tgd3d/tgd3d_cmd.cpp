/* SPDX-License-Identifier: MIT
 *
 * Command recording, submission and the render-queue fence.
 *
 * NVK submits on its own kernel context while the D3D runtime signals its
 * synchronization objects and queues Present on the context registered by
 * this driver. One monitored fence is shared by both kernel devices: every
 * Vulkan submission signals the next value from the GPU, and before anything
 * the runtime can observe (Flush, Present, SetDisplayMode, resolve of a
 * shared surface) the driver queues a GPU wait for that value on the runtime
 * context. The CPU never waits for a frame; it only waits when more than
 * TGD3D_MAX_IN_FLIGHT submissions are unfinished, when the application reads
 * GPU output back, or when storage must be destroyed synchronously.
 */
#include "tgd3d.h"

namespace tgd3d {

enum { TGD3D_MAX_IN_FLIGHT = 2 };

typedef NTSTATUS(APIENTRY *PFN_ShareObjects)(UINT, const D3DKMT_HANDLE *, void *, DWORD, HANDLE *);

void Device::setError(HRESULT hr, const char *file, int line)
{
   trace_error(hr, file, (unsigned)line);
   if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == D3DDDIERR_DEVICEREMOVED)
      lost = hr;
   if (um.pfnSetErrorCb)
      um.pfnSetErrorCb(hRTCoreLayer, hr);
}

uint64_t Device::completed()
{
   if (runtimeFenceValue)
      return *runtimeFenceValue;      /* shared monitored fence: no system call */
   uint64_t value = 0;
   if (vk.GetSemaphoreCounterValue(vkdev, fence, &value) != VK_SUCCESS)
      return 0;
   return value;
}

bool sequence_done(Device *dev, uint64_t seq)
{
   return seq == 0 || dev->completed() >= seq;
}

/* ------------------------------------------------------------------ */

HRESULT sync_create(Device *dev)
{
   VkSemaphoreTypeCreateInfo type = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
   type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
   VkSemaphoreCreateInfo info = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
   if (dev->vk.CreateSemaphore(dev->vkdev, &info, nullptr, &dev->fence) != VK_SUCCESS)
      return E_OUTOFMEMORY;

   if (!dev->kt.pfnCreateSynchronizationObject2Cb || !dev->kt.pfnDestroySynchronizationObjectCb ||
       !dev->kt.pfnWaitForSynchronizationObjectFromGpuCb)
      return dev->nvk ? DXGI_ERROR_UNSUPPORTED : S_OK;

   /* The fence belongs to the runtime's device so its context can wait for
    * it; NVK opens the same kernel object through the NT handle. */
   D3DDDICB_CREATESYNCHRONIZATIONOBJECT2 create = {};
   create.Info.Type = D3DDDI_MONITORED_FENCE;
   create.Info.Flags.Shared = dev->nvk;
   create.Info.Flags.NtSecuritySharing = dev->nvk;
   create.Info.MonitoredFence.InitialFenceValue = 0;
   create.Info.MonitoredFence.EngineAffinity = 1;
   HRESULT hr = dev->kt.pfnCreateSynchronizationObject2Cb(dev->hRTDevice.handle, &create);
   trace(TGD3D_TRACE_RUNTIME_CONTEXT, hr, 3, create.hSyncObject != 0);
   if (FAILED(hr))
      return hr;
   if (!create.hSyncObject)
      return E_FAIL;
   dev->runtimeFence = create.hSyncObject;
   if (!dev->nvk)
      return S_OK;     /* test build: the semaphore above is the only timeline */

   if (!create.Info.MonitoredFence.FenceValueCPUVirtualAddress)
      return E_FAIL;
   HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
   auto share = gdi ? reinterpret_cast<PFN_ShareObjects>(
      reinterpret_cast<void *>(GetProcAddress(gdi, "D3DKMTShareObjects"))) : nullptr;
   if (!share)
      return DXGI_ERROR_UNSUPPORTED;
   OBJECT_ATTRIBUTES attributes = {};
   attributes.Length = sizeof(attributes);
   HANDLE handle = nullptr;
   NTSTATUS status = share(1, &dev->runtimeFence, &attributes, GENERIC_ALL, &handle);
   trace(TGD3D_TRACE_RUNTIME_CONTEXT, HRESULT_FROM_NT(status), 4, 0);
   if (status < 0 || !handle)
      return HRESULT_FROM_NT(status);
   VkImportSemaphoreWin32HandleInfoKHR import = {
      VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
   import.semaphore = dev->fence;
   import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
   import.handle = handle;
   VkResult result = dev->vk.ImportSemaphoreWin32HandleKHR(dev->vkdev, &import);
   CloseHandle(handle);       /* the import keeps its own reference */
   if (result != VK_SUCCESS) {
      trace(TGD3D_TRACE_RUNTIME_CONTEXT, E_FAIL, 5, (uint64_t)(int64_t)result);
      return E_FAIL;
   }
   dev->runtimeFenceValue =
      static_cast<const volatile UINT64 *>(create.Info.MonitoredFence.FenceValueCPUVirtualAddress);
   return S_OK;
}

void sync_destroy(Device *dev)
{
   /* The Vulkan side closes its handle first; the runtime object last. */
   if (dev->fence)
      dev->vk.DestroySemaphore(dev->vkdev, dev->fence, nullptr);
   dev->fence = VK_NULL_HANDLE;
   dev->runtimeFenceValue = nullptr;
   if (dev->runtimeFence && dev->kt.pfnDestroySynchronizationObjectCb) {
      D3DDDICB_DESTROYSYNCHRONIZATIONOBJECT destroy = {};
      destroy.hSyncObject = dev->runtimeFence;
      dev->kt.pfnDestroySynchronizationObjectCb(dev->hRTDevice.handle, &destroy);
   }
   dev->runtimeFence = 0;
}

HRESULT cmd_create(Device *dev)
{
   VkCommandPoolCreateInfo info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
   info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
   info.queueFamilyIndex = dev->caps->queueFamily;
   return dev->vk.CreateCommandPool(dev->vkdev, &info, nullptr, &dev->cmdPool) == VK_SUCCESS ?
      S_OK : E_OUTOFMEMORY;
}

void cmd_destroy(Device *dev)
{
   for (auto pool : dev->framePools)
      dev->vk.DestroyDescriptorPool(dev->vkdev, pool, nullptr);
   for (auto pool : dev->freePools)
      dev->vk.DestroyDescriptorPool(dev->vkdev, pool, nullptr);
   for (auto &submitted : dev->submittedCommands)
      for (auto pool : submitted.pools)
         dev->vk.DestroyDescriptorPool(dev->vkdev, pool, nullptr);
   dev->framePools.clear();
   dev->freePools.clear();
   dev->submittedCommands.clear();
   if (dev->cmdPool)
      dev->vk.DestroyCommandPool(dev->vkdev, dev->cmdPool, nullptr);
   dev->cmdPool = VK_NULL_HANDLE;
   dev->cmd = VK_NULL_HANDLE;
}

/* ------------------------------------------------------------------ */

static VkCommandBuffer ensure_cmd(Device *dev)
{
   if (dev->cmd)
      return dev->cmd;
   VkCommandBuffer cmd = VK_NULL_HANDLE;
   if (!dev->freeCommands.empty()) {
      cmd = dev->freeCommands.back();
      dev->freeCommands.pop_back();
   } else {
      VkCommandBufferAllocateInfo info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      info.commandPool = dev->cmdPool;
      info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      info.commandBufferCount = 1;
      if (dev->vk.AllocateCommandBuffers(dev->vkdev, &info, &cmd) != VK_SUCCESS) {
         TG_ERROR(dev, E_OUTOFMEMORY);
         return VK_NULL_HANDLE;
      }
   }
   VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
   dev->vk.BeginCommandBuffer(cmd, &begin);
   dev->cmd = cmd;
   dev->cmdHasWork = false;
   /* A new command buffer inherits no state. */
   dev->dirty = DIRTY_ALL;
   for (auto &stage : dev->stage) {
      stage.cbDirty = ~0u;
      stage.bound = VK_NULL_HANDLE;
   }
   dev->textureSet = dev->texelBufferSet = VK_NULL_HANDLE;
   dev->barrierSerial = dev->writeSerial;   /* earlier submissions are ordered by the queue */
   dev->firstCommandQpc = trace_timing_enabled() ? qpc_now() : 0;
   return cmd;
}

void end_rendering(Device *dev)
{
   if (!dev->rendering)
      return;
   query_pass_end(dev, dev->cmd);
   dev->vk.CmdEndRendering(dev->cmd);
   dev->rendering = false;
}

VkCommandBuffer cmd_draw(Device *dev)
{
   VkCommandBuffer cmd = ensure_cmd(dev);
   dev->cmdHasWork = true;
   return cmd;
}

VkCommandBuffer cmd_begin(Device *dev)
{
   end_rendering(dev);
   flush_pending_clears(dev, nullptr);
   VkCommandBuffer cmd = ensure_cmd(dev);
   dev->cmdHasWork = true;
   return cmd;
}

void image_use(Device *dev, ImageStorage &storage, bool write)
{
   storage.lastUse = dev->recordingSeq();
   if (!storage.layoutReady) {
      /* Every image lives in GENERAL. Imported runtime allocations already
       * hold another owner's pixels, which the transition must keep. */
      end_rendering(dev);
      VkCommandBuffer cmd = ensure_cmd(dev);
      dev->cmdHasWork = true;
      VkImageMemoryBarrier2 barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
      barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
      barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
      barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
      barrier.oldLayout = storage.preinitialized ? VK_IMAGE_LAYOUT_PREINITIALIZED :
                                                   VK_IMAGE_LAYOUT_UNDEFINED;
      barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
      barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.image = storage.image;
      barrier.subresourceRange = {storage.aspects, 0, VK_REMAINING_MIP_LEVELS, 0,
                                  VK_REMAINING_ARRAY_LAYERS};
      VkDependencyInfo info = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      info.imageMemoryBarrierCount = 1;
      info.pImageMemoryBarriers = &barrier;
      dev->vk.CmdPipelineBarrier2(cmd, &info);
      storage.layoutReady = true;
   }
   if (write)
      storage.writeSerial = ++dev->writeSerial;
}

void buffer_written(Device *dev)
{
   dev->bufferWriteSerial = ++dev->writeSerial;
}

void barrier_for_read(Device *dev, uint64_t writeSerial)
{
   if (writeSerial <= dev->barrierSerial)
      return;
   end_rendering(dev);
   VkCommandBuffer cmd = ensure_cmd(dev);
   dev->cmdHasWork = true;
   VkMemoryBarrier2 barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
   barrier.srcStageMask = barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
   barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
   barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
   VkDependencyInfo info = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
   info.memoryBarrierCount = 1;
   info.pMemoryBarriers = &barrier;
   dev->vk.CmdPipelineBarrier2(cmd, &info);
   dev->barrierSerial = dev->writeSerial;
}

/* Clears recorded before a draw become that render pass's load operation.
 * A clear of anything else is executed as its own one-attachment pass. */
void flush_pending_clears(Device *dev, const View *except_bound)
{
   if (dev->pendingClears.empty())
      return;
   std::vector<PendingClear> clears;
   clears.swap(dev->pendingClears);
   for (const auto &clear : clears) {
      if (except_bound) {
         bool bound = clear.view == dev->dsv;
         for (View *rtv : dev->rtv)
            bound |= clear.view == rtv;
         if (bound) {
            dev->pendingClears.push_back(clear);
            continue;
         }
      }
      View *view = clear.view;
      end_rendering(dev);
      image_use(dev, view->res->storage, true);
      VkCommandBuffer cmd = ensure_cmd(dev);
      dev->cmdHasWork = true;
      VkRenderingAttachmentInfo attachment = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
      attachment.imageView = view_get(view);
      attachment.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
      attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      attachment.clearValue = clear.value;
      VkRenderingAttachmentInfo keep = attachment;
      keep.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      VkRenderingInfo info = {VK_STRUCTURE_TYPE_RENDERING_INFO};
      info.renderArea = {{0, 0}, {view->width, view->height}};
      info.layerCount = view->layers;
      if (view->type == ViewType::RTV) {
         info.colorAttachmentCount = 1;
         info.pColorAttachments = &attachment;
      } else {
         const uint16_t flags = view->formatFlags;
         if (flags & FMT_DEPTH)
            info.pDepthAttachment = (clear.aspects & VK_IMAGE_ASPECT_DEPTH_BIT) ? &attachment : &keep;
         if (flags & FMT_STENCIL)
            info.pStencilAttachment = (clear.aspects & VK_IMAGE_ASPECT_STENCIL_BIT) ? &attachment : &keep;
      }
      dev->vk.CmdBeginRendering(cmd, &info);
      dev->vk.CmdEndRendering(cmd);
      dev->stats.renderPasses++;
   }
}

/* ------------------------------------------------------------------ */

void defer(Device *dev, DeferredType type, uint64_t handle, uint64_t seq)
{
   if (handle)
      dev->deferred.push_back({type, seq, handle});
}

static void destroy_deferred(Device *dev, const Deferred &d)
{
   switch (d.type) {
   case DeferredType::Image:
      dev->vk.DestroyImage(dev->vkdev, (VkImage)d.handle, nullptr);
      break;
   case DeferredType::Memory:
      dev->vk.FreeMemory(dev->vkdev, (VkDeviceMemory)d.handle, nullptr);
      break;
   case DeferredType::ImageView:
      dev->vk.DestroyImageView(dev->vkdev, (VkImageView)d.handle, nullptr);
      break;
   case DeferredType::BufferView:
      dev->vk.DestroyBufferView(dev->vkdev, (VkBufferView)d.handle, nullptr);
      break;
   case DeferredType::Sampler:
      dev->vk.DestroySampler(dev->vkdev, (VkSampler)d.handle, nullptr);
      break;
   case DeferredType::Shader:
      dev->vk.DestroyShaderEXT(dev->vkdev, (VkShaderEXT)d.handle, nullptr);
      break;
   case DeferredType::QueryPool:
      dev->vk.DestroyQueryPool(dev->vkdev, (VkQueryPool)d.handle, nullptr);
      break;
   case DeferredType::Runtime: {
      HRESULT hr = runtime_destroy(reinterpret_cast<RuntimeResource *>((uintptr_t)d.handle));
      if (FAILED(hr))
         TG_ERROR(dev, hr);
      break;
   }
   }
}

void retire(Device *dev, bool force)
{
   const uint64_t done = force ? UINT64_MAX : dev->completed();
   while (!dev->submittedCommands.empty() && dev->submittedCommands.front().seq <= done) {
      SubmittedCommands &submitted = dev->submittedCommands.front();
      dev->vk.ResetCommandBuffer(submitted.cmd, 0);
      dev->freeCommands.push_back(submitted.cmd);
      for (auto pool : submitted.pools) {
         dev->vk.ResetDescriptorPool(dev->vkdev, pool, 0);
         dev->freePools.push_back(pool);
      }
      dev->submittedCommands.pop_front();
   }
   /* Deferred objects are queued in sequence order per kind, but a resource
    * destroyed early may carry an older sequence than its neighbours. */
   for (size_t i = 0; i < dev->deferred.size();) {
      if (dev->deferred[i].seq <= done) {
         Deferred d = dev->deferred[i];
         dev->deferred.erase(dev->deferred.begin() + i);
         destroy_deferred(dev, d);
      } else {
         i++;
      }
   }
   for (size_t i = 0; i < dev->deferredSlices.size();) {
      if (dev->deferredSlices[i].seq <= done) {
         Slice slice = dev->deferredSlices[i].slice;
         dev->deferredSlices.erase(dev->deferredSlices.begin() + i);
         slice_release(dev, slice, 0);
      } else {
         i++;
      }
   }
}

static HRESULT wait_cpu(Device *dev, uint64_t seq, tgd3d_wait_reason reason)
{
   if (dev->completed() >= seq)
      return S_OK;
   const uint64_t begin = qpc_now();
   VkSemaphoreWaitInfo wait = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
   wait.semaphoreCount = 1;
   wait.pSemaphores = &dev->fence;
   wait.pValues = &seq;
   VkResult result = VK_TIMEOUT;
   /* The driver sleeps on the fence's kernel event. A fence that does not
    * advance for this long means the queue is gone, not slow. */
   for (int second = 0; second < 30 && result == VK_TIMEOUT; second++)
      result = dev->vk.WaitSemaphores(dev->vkdev, &wait, UINT64_C(1000000000));
   const uint64_t end = qpc_now();
   if (reason == TGD3D_WAIT_BACKPRESSURE)
      dev->stats.backpressureNs += end - begin;
   const uint64_t qpc[4] = {begin, end, 0, 0};
   const uint64_t value[6] = {(uint64_t)reason, seq, 0, 0, 0, 0};
   trace_timing(TGD3D_TIMING_CPU_WAIT, result == VK_SUCCESS ? S_OK : E_FAIL, dev->submitted,
                dev->completed(), qpc, value);
   if (result != VK_SUCCESS) {
      TG_ERROR(dev, DXGI_ERROR_DEVICE_REMOVED);
      return DXGI_ERROR_DEVICE_REMOVED;
   }
   return S_OK;
}

HRESULT wait_sequence(Device *dev, uint64_t seq)
{
   if (seq > dev->submitted) {
      HRESULT hr = flush(dev, false);
      if (FAILED(hr))
         return hr;
   }
   HRESULT hr = wait_cpu(dev, seq, TGD3D_WAIT_READBACK);
   retire(dev);
   return hr;
}

static HRESULT gate_runtime(Device *dev)
{
   if (dev->gated >= dev->submitted || !dev->runtimeFence || !dev->hContext)
      return S_OK;
   /* Queued, not waited: the runtime context executes nothing after this
    * point until the render queue has signalled `submitted`. */
   D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMGPU wait = {};
   wait.hContext = dev->hContext;
   wait.ObjectCount = 1;
   wait.ObjectHandleArray = &dev->runtimeFence;
   wait.MonitoredFenceValueArray = &dev->submitted;
   HRESULT hr = dev->kt.pfnWaitForSynchronizationObjectFromGpuCb(dev->hRTDevice.handle, &wait);
   if (FAILED(hr))
      return hr;
   dev->gated = dev->submitted;
   return S_OK;
}

HRESULT flush(Device *dev, bool gate)
{
   if (FAILED(dev->lost))
      return dev->lost;
   flush_pending_clears(dev, nullptr);
   if (dev->cmd && dev->cmdHasWork) {
      end_rendering(dev);
      const uint64_t submit_begin = trace_timing_enabled() ? qpc_now() : 0;
      dev->vk.EndCommandBuffer(dev->cmd);
      const uint64_t value = dev->submitted + 1;
      VkSemaphoreSubmitInfo signal = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
      signal.semaphore = dev->fence;
      signal.value = value;
      signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
      VkCommandBufferSubmitInfo buffer = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
      buffer.commandBuffer = dev->cmd;
      VkSubmitInfo2 submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
      submit.commandBufferInfoCount = 1;
      submit.pCommandBufferInfos = &buffer;
      submit.signalSemaphoreInfoCount = 1;
      submit.pSignalSemaphoreInfos = &signal;
      VkResult result = dev->vk.QueueSubmit2(dev->queue, 1, &submit, VK_NULL_HANDLE);
      SubmittedCommands done = {dev->cmd, value, {}};
      done.pools.swap(dev->framePools);
      dev->submittedCommands.push_back(done);
      dev->cmd = VK_NULL_HANDLE;
      dev->cmdHasWork = false;
      dev->submitted = value;
      dev->stats.submits++;
      if (submit_begin) {
         const uint64_t qpc[4] = {dev->firstCommandQpc, submit_begin, qpc_now(), 0};
         const uint64_t values[6] = {dev->stats.draws, dev->stats.renderPasses,
            dev->stats.uploadBytes, dev->stats.copyBytes, dev->stats.readbackBytes, 0};
         trace_timing(TGD3D_TIMING_SUBMIT, result == VK_SUCCESS ? S_OK : E_FAIL, value,
                      dev->completed(), qpc, values);
      }
      dev->stats.draws = dev->stats.renderPasses = 0;
      dev->stats.uploadBytes = dev->stats.copyBytes = dev->stats.readbackBytes = 0;
      if (result != VK_SUCCESS) {
         /* Nothing will signal this value from the GPU. Release every
          * waiter, including a runtime context already gated on it. */
         VkSemaphoreSignalInfo release = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
         release.semaphore = dev->fence;
         release.value = value;
         dev->vk.SignalSemaphore(dev->vkdev, &release);
         TG_ERROR(dev, DXGI_ERROR_DEVICE_REMOVED);
         return DXGI_ERROR_DEVICE_REMOVED;
      }
   }
   HRESULT hr = gate ? gate_runtime(dev) : S_OK;
   retire(dev);
   if (SUCCEEDED(hr) && dev->submitted > TGD3D_MAX_IN_FLIGHT &&
       dev->completed() < dev->submitted - TGD3D_MAX_IN_FLIGHT) {
      hr = wait_cpu(dev, dev->submitted - TGD3D_MAX_IN_FLIGHT, TGD3D_WAIT_BACKPRESSURE);
      retire(dev);
   }
   return hr;
}

}
