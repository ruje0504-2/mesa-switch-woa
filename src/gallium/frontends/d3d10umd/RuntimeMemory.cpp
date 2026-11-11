/* SPDX-License-Identifier: MIT
 * Runtime ownership follows the Microsoft WDDM callback contracts; see
 * tegra-x1-wddm/docs/D3D_RUNTIME_SHARING.md. No runtime handle is cast to a
 * KMT device or NT shared-resource handle.
 */
#include "RuntimeMemory.h"
#include "State.h"
#include "nouveau/vulkan/nvkmd/windows/allocation_abi.h"
#include <stdlib.h>

struct PrimaryMapping {
   uint64_t address, size;
   bool freed;
   PrimaryMapping *next;
};

struct RuntimePrimary {
   wsi_win32_memory memory;
   LONG refs;
   SRWLOCK lock;
   HANDLE device, runtime_resource;
   D3DDDI_DEVICECALLBACKS callbacks;
   D3DKMT_HANDLE queue, fence;
   const volatile UINT64 *fence_value;
   HRESULT error, cleanup_error;
   bool cpu_locked, dead, associated;
   PrimaryMapping *mappings;
};

/* arg0: stage in high 32 bits, allocation handle in low 32 bits.
 * arg1: byte count, paging-fence value, or paging-queue handle. */
static HRESULT trace(RuntimePrimary *p, unsigned stage, HRESULT hr, uint64_t value = 0)
{
   D3D10Trace(D3D10_TRACE_RUNTIME_MEMORY, hr,
              (uint64_t)stage << 32 | p->memory.allocation, value);
   return hr;
}

static VkResult result(RuntimePrimary *p, HRESULT hr)
{
   if (SUCCEEDED(hr)) return VK_SUCCESS;
   p->error = hr;
   if (hr == E_OUTOFMEMORY) return VK_ERROR_OUT_OF_HOST_MEMORY;
   if (hr == D3DDDIERR_OUTOFVIDEOMEMORY) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   if (hr == D3DDDIERR_DEVICEREMOVED || hr == DXGI_ERROR_DEVICE_REMOVED ||
       hr == DXGI_ERROR_DEVICE_RESET) return VK_ERROR_DEVICE_LOST;
   return VK_ERROR_UNKNOWN;
}

static HRESULT wait_paging(RuntimePrimary *p, UINT64 value)
{
   if (!value || (p->fence_value && *p->fence_value >= value)) return S_OK;
   D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMCPU wait = {};
   wait.ObjectCount = 1;
   wait.ObjectHandleArray = &p->fence;
   wait.FenceValueArray = &value;
   return trace(p, 3, p->callbacks.pfnWaitForSynchronizationObjectFromCpuCb(p->device, &wait), value);
}

static void retain(void *context)
{
   InterlockedIncrement(&static_cast<RuntimePrimary *>(context)->refs);
}

static void release(void *context)
{
   auto p = static_cast<RuntimePrimary *>(context);
   if (!InterlockedDecrement(&p->refs)) {
      while (p->mappings) {
         auto next = p->mappings->next;
         free(p->mappings); p->mappings = next;
      }
      free(p);
   }
}

static HRESULT free_address(RuntimePrimary *p, uint64_t address, uint64_t bytes)
{
   D3DDDICB_FREEGPUVIRTUALADDRESS args = {};
   args.BaseAddress = address;
   args.Size = bytes;
   return trace(p, 5, p->callbacks.pfnFreeGpuVirtualAddressCb(p->device, &args), bytes);
}

static VkResult free_gpu(void *context, uint64_t address, uint64_t bytes)
{
   auto p = static_cast<RuntimePrimary *>(context);
   AcquireSRWLockExclusive(&p->lock);
   PrimaryMapping **link = &p->mappings;
   while (*link && ((*link)->address != address || (*link)->size != bytes)) link = &(*link)->next;
   auto mapping = *link;
   HRESULT hr = S_OK;
   if ((!mapping || !mapping->freed) && !p->dead) hr = free_address(p, address, bytes);
   if (FAILED(hr)) p->cleanup_error = hr;
   if (mapping) { *link = mapping->next; free(mapping); }
   VkResult status = result(p, hr);
   ReleaseSRWLockExclusive(&p->lock);
   return status;
}

static VkResult map_gpu(void *context, uint64_t address, uint64_t offset,
                        uint64_t bytes, uint8_t kind, bool read_only,
                        uint64_t reserved, uint64_t reserved_size)
{
   auto p = static_cast<RuntimePrimary *>(context);
   AcquireSRWLockExclusive(&p->lock);
   HRESULT hr = E_INVALIDARG;
   if (p->dead) {
      hr = D3DDDIERR_DEVICEREMOVED;
   } else if (bytes && !((address | offset | bytes | reserved | reserved_size) & 4095) && !kind &&
              offset <= p->memory.size && bytes <= p->memory.size - offset &&
              address >= reserved && address - reserved <= reserved_size &&
              bytes <= reserved_size - (address - reserved)) {
      PrimaryMapping *mapping = p->mappings;
      while (mapping && (mapping->address != reserved || mapping->size != reserved_size)) mapping = mapping->next;
      if (!mapping) {
         mapping = static_cast<PrimaryMapping *>(calloc(1, sizeof(*mapping)));
         if (!mapping) {
            result(p, E_OUTOFMEMORY);
            ReleaseSRWLockExclusive(&p->lock);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
         }
         mapping->address = reserved; mapping->size = reserved_size;
         mapping->next = p->mappings; p->mappings = mapping;
      }
      D3DDDI_MAPGPUVIRTUALADDRESS map = {};
      map.hPagingQueue = p->queue;
      map.BaseAddress = address;
      map.hAllocation = p->memory.allocation;
      map.OffsetInPages = offset / 4096;
      map.SizeInPages = bytes / 4096;
      map.Protection.Write = !read_only;
      hr = trace(p, 4, p->callbacks.pfnMapGpuVirtualAddressCb(p->device, &map), bytes);
      /* WDDM queues page-table updates with E_PENDING, despite its HRESULT
       * failure bit. The allocation is usable only after the paging fence. */
      if (SUCCEEDED(hr) || hr == E_PENDING) {
         if (map.VirtualAddress != address) hr = E_FAIL;
         else hr = wait_paging(p, map.PagingFenceValue);
      }
   }
   VkResult status = result(p, hr);
   ReleaseSRWLockExclusive(&p->lock);
   return status;
}

static VkResult map_cpu(void *context, void **address)
{
   auto p = static_cast<RuntimePrimary *>(context);
   AcquireSRWLockExclusive(&p->lock);
   D3DDDICB_LOCK2 lock = {};
   lock.hAllocation = p->memory.allocation;
   HRESULT hr = p->dead ? D3DDDIERR_DEVICEREMOVED :
      p->cpu_locked ? E_UNEXPECTED : trace(p, 7, p->callbacks.pfnLock2Cb(p->device, &lock));
   if (SUCCEEDED(hr)) {
      if (!lock.pData) hr = E_FAIL;
      else {
         *address = lock.pData;
         p->cpu_locked = true;
      }
   }
   VkResult status = result(p, hr);
   ReleaseSRWLockExclusive(&p->lock);
   return status;
}

static void unmap_cpu(void *context)
{
   auto p = static_cast<RuntimePrimary *>(context);
   AcquireSRWLockExclusive(&p->lock);
   if (p->cpu_locked) {
      D3DDDICB_UNLOCK2 unlock = {};
      unlock.hAllocation = p->memory.allocation;
      result(p, trace(p, 8, p->callbacks.pfnUnlock2Cb(p->device, &unlock)));
      p->cpu_locked = false;
   }
   ReleaseSRWLockExclusive(&p->lock);
}

HRESULT RuntimePrimaryDestroy(RuntimePrimary *p)
{
   if (!p) return S_OK;
   AcquireSRWLockExclusive(&p->lock);
   HRESULT hr = p->cleanup_error;
   p->dead = true;
   for (auto mapping = p->mappings; mapping; mapping = mapping->next) {
      if (!mapping->freed) {
         HRESULT next = free_address(p, mapping->address, mapping->size);
         if (SUCCEEDED(hr)) hr = next;
         mapping->freed = true;
      }
   }
   if (p->cpu_locked) {
      D3DDDICB_UNLOCK2 unlock = {};
      unlock.hAllocation = p->memory.allocation;
      HRESULT next = trace(p, 8, p->callbacks.pfnUnlock2Cb(p->device, &unlock));
      if (SUCCEEDED(hr)) hr = next;
      p->cpu_locked = false;
   }
   if (p->associated) {
      D3DDDICB_DEALLOCATE deallocate = {};
      deallocate.hResource = p->runtime_resource;
      HRESULT next = trace(p, 6, p->callbacks.pfnDeallocateCb(p->device, &deallocate));
      if (SUCCEEDED(hr)) hr = next;
      p->memory.allocation = 0;
      p->associated = false;
   }
   if (p->queue) {
      D3DDDI_DESTROYPAGINGQUEUE queue = {};
      queue.hPagingQueue = p->queue;
      HRESULT next = trace(p, 10, p->callbacks.pfnDestroyPagingQueueCb(p->device, &queue), p->queue);
      if (SUCCEEDED(hr)) hr = next;
      p->queue = 0;
   }
   ReleaseSRWLockExclusive(&p->lock);
   release(p);
   return hr;
}

HRESULT RuntimePrimaryCreate(Device *device, const D3D10DDIARG_CREATERESOURCE *desc,
                              D3D10DDI_HRTRESOURCE runtime, RuntimePrimary **out)
{
   *out = NULL;
   /* This is the KMD's existing portrait primary layout. Landscape support
    * must extend both the layout and the scanout contract together. */
   if (!desc->pPrimaryDesc || !runtime.handle || !desc->pMipInfoList ||
       desc->ResourceDimension != D3D10DDIRESOURCE_TEXTURE2D ||
       desc->Format != DXGI_FORMAT_B8G8R8A8_UNORM || desc->MipLevels != 1 ||
       desc->ArraySize != 1 || desc->SampleDesc.Count != 1 || desc->SampleDesc.Quality ||
       desc->pPrimaryDesc->VidPnSourceId != 0 ||
       desc->pMipInfoList[0].TexelWidth != 720 || desc->pMipInfoList[0].TexelHeight != 1280 ||
       desc->pPrimaryDesc->ModeDesc.Width != 720 || desc->pPrimaryDesc->ModeDesc.Height != 1280)
      return DXGI_ERROR_UNSUPPORTED;

   const auto &cb = device->KTCallbacks;
   if (!cb.pfnAllocateCb || !cb.pfnDeallocateCb || !cb.pfnSetDisplayModeCb ||
       !cb.pfnCreatePagingQueueCb || !cb.pfnDestroyPagingQueueCb ||
       !cb.pfnMakeResidentCb || !cb.pfnMapGpuVirtualAddressCb || !cb.pfnFreeGpuVirtualAddressCb ||
       !cb.pfnWaitForSynchronizationObjectFromCpuCb || !cb.pfnLock2Cb || !cb.pfnUnlock2Cb ||
       !device->pipe->screen->get_device_luid)
      return DXGI_ERROR_UNSUPPORTED;
   auto p = static_cast<RuntimePrimary *>(calloc(1, sizeof(RuntimePrimary)));
   if (!p) return E_OUTOFMEMORY;
   p->refs = 1;
   p->device = device->hDevice;
   p->runtime_resource = runtime.handle;
   p->callbacks = cb;
   InitializeSRWLock(&p->lock);
   device->pipe->screen->get_device_luid(device->pipe->screen,
                                        reinterpret_cast<char *>(&p->memory.adapter_luid));
   if (!p->memory.adapter_luid) {
      release(p);
      return DXGI_ERROR_UNSUPPORTED;
   }

   D3DDDICB_CREATEPAGINGQUEUE queue = {};
   queue.Priority = D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL;
   HRESULT hr = cb.pfnCreatePagingQueueCb(p->device, &queue);
   trace(p, 9, hr, queue.hPagingQueue);
   if (FAILED(hr)) { release(p); return hr; }
   p->queue = queue.hPagingQueue;
   p->fence = queue.hSyncObject;
   p->fence_value = static_cast<const volatile UINT64 *>(queue.FenceValueCPUVirtualAddress);
   if (!p->queue || !p->fence) { RuntimePrimaryDestroy(p); return E_FAIL; }

   struct {
      tg_gpu_allocation_request buffer;
      D3DDDIFORMAT format;
      D3DDDI_RATIONAL refresh;
      UINT reserved;
   } request = {{TG_GPU_ALLOCATION_ABI, TG_GPU_ALLOCATION_CPU_VISIBLE |
               TG_GPU_ALLOCATION_DISPLAY_PRIMARY, 720ull * 1280 * 4},
               D3DDDIFMT_A8R8G8B8,
               {desc->pPrimaryDesc->ModeDesc.RefreshRate.Numerator,
                desc->pPrimaryDesc->ModeDesc.RefreshRate.Denominator}, 0};
   static_assert(sizeof(request) == 32, "KMD primary layout");
   D3DDDI_ALLOCATIONINFO2 info = {};
   info.pPrivateDriverData = &request;
   info.PrivateDriverDataSize = sizeof(request);
   info.VidPnSourceId = desc->pPrimaryDesc->VidPnSourceId;
   info.Flags.Primary = true;
   D3DDDICB_ALLOCATE allocate = {};
   allocate.hResource = runtime.handle;
   allocate.NumAllocations = 1;
   allocate.pAllocationInfo2 = &info;
   hr = cb.pfnAllocateCb(p->device, &allocate);
   trace(p, 1, hr, request.buffer.bytes);
   if (FAILED(hr)) { RuntimePrimaryDestroy(p); return hr; }
   p->associated = true;
   p->memory.allocation = info.hAllocation;
   if (!info.hAllocation) { RuntimePrimaryDestroy(p); return E_FAIL; }
   D3DDDI_MAKERESIDENT resident = {};
   resident.hPagingQueue = p->queue;
   resident.NumAllocations = 1;
   resident.AllocationList = &p->memory.allocation;
   hr = cb.pfnMakeResidentCb(p->device, &resident);
   trace(p, 2, hr, resident.PagingFenceValue);
   if (SUCCEEDED(hr) || hr == E_PENDING) hr = wait_paging(p, resident.PagingFenceValue);
   if (FAILED(hr)) { RuntimePrimaryDestroy(p); return hr; }
   p->memory.version = WSI_WIN32_MEMORY_VERSION;
   p->memory.size = request.buffer.bytes;
   p->memory.context = p;
   p->memory.retain = retain;
   p->memory.release = release;
   p->memory.map_gpu = map_gpu;
   p->memory.free_gpu = free_gpu;
   p->memory.map_cpu = map_cpu;
   p->memory.unmap_cpu = unmap_cpu;
   *out = p;
   return S_OK;
}

HRESULT RuntimePrimarySetMode(RuntimePrimary *p, HANDLE device, UINT subresource)
{
   if (!p) return DXGI_ERROR_UNSUPPORTED;
   if (p->device != device || subresource) return E_INVALIDARG;
   AcquireSRWLockExclusive(&p->lock);
   D3DDDICB_SETDISPLAYMODE mode = {};
   mode.hPrimaryAllocation = p->memory.allocation;
   HRESULT hr = p->dead ? D3DDDIERR_DEVICEREMOVED :
      p->callbacks.pfnSetDisplayModeCb(p->device, &mode);
   result(p, hr);
   ReleaseSRWLockExclusive(&p->lock);
   return hr;
}

HRESULT RuntimePrimaryError(RuntimePrimary *p)
{
   return FAILED(p->error) ? p->error : DXGI_ERROR_UNSUPPORTED;
}

const wsi_win32_memory *RuntimePrimaryMemory(RuntimePrimary *p) { return &p->memory; }

HRESULT RuntimeDeviceCreateContext(Device *device)
{
   const auto &cb = device->KTCallbacks;
   auto screen = device->pipe->screen;
   if (!cb.pfnCreateContextVirtualCb || !cb.pfnDestroyContextCb ||
       !screen->fence_finish || !screen->fence_reference)
      return DXGI_ERROR_UNSUPPORTED;
   /* NVK's KMT context is not registered with the D3D runtime. The runtime
    * needs its own context for implicit synchronization and DXGI Present. */
   D3DDDICB_CREATECONTEXTVIRTUAL args = {};
   args.EngineAffinity = 1;
   HRESULT hr = cb.pfnCreateContextVirtualCb(device->hDevice, &args);
   if (SUCCEEDED(hr)) {
      if (!args.hContext) hr = E_FAIL;
      else device->hContext = args.hContext;
   }
   D3D10Trace(D3D10_TRACE_RUNTIME_CONTEXT, hr, 1, device->hContext != NULL);
   return hr;
}

HRESULT RuntimeDeviceDestroyContext(Device *device)
{
   if (!device->hContext) return S_OK;
   D3DDDICB_DESTROYCONTEXT args = {};
   args.hContext = device->hContext;
   HRESULT hr = device->KTCallbacks.pfnDestroyContextCb(device->hDevice, &args);
   D3D10Trace(D3D10_TRACE_RUNTIME_CONTEXT, hr, 2, 1);
   device->hContext = NULL;
   return hr;
}

HRESULT RuntimeDeviceFinish(Device *device)
{
   auto pipe = device->pipe;
   auto screen = pipe->screen;
   if (!screen->fence_finish || !screen->fence_reference) return E_FAIL;
   struct pipe_fence_handle *fence = NULL;
   /* NVK submits on a separate KMT context. Complete its work before the
    * runtime signals the context above, so an implicit signal cannot pass
    * pending rendering. A shared GPU fence can replace this synchronous
    * bridge when the two context owners expose a common timeline. */
   pipe->flush(pipe, &fence, PIPE_FLUSH_HINT_FINISH);
   bool finished = fence && screen->fence_finish(screen, pipe, fence, UINT64_MAX);
   screen->fence_reference(screen, &fence, NULL);
   HRESULT hr = finished ? S_OK : D3DDDIERR_DEVICEREMOVED;
   if (pipe->get_device_reset_status && pipe->get_device_reset_status(pipe) != PIPE_NO_RESET)
      hr = D3DDDIERR_DEVICEREMOVED;
   D3D10Trace(D3D10_TRACE_RUNTIME_CONTEXT, hr, 3, finished);
   return hr;
}

HRESULT RuntimePrimaryFinish(Device *device, Resource *resource)
{
   if (!resource->runtime_primary || !resource->resource) return S_OK;
   if (device->pipe->flush_resource)
      device->pipe->flush_resource(device->pipe, resource->resource);
   return RuntimeDeviceFinish(device);
}
