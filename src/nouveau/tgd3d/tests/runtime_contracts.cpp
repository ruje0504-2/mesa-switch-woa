/* SPDX-License-Identifier: MIT
 * Real runtime owner implementation with two independent callback devices.
 */
#include "tgd3d.h"
#include "vulkan/util/vk_internal_exts.h"
#include "vulkan/util/vk_wsi_win32_memory.h"
#include "nouveau/vulkan/nvkmd/windows/allocation_abi.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace tgd3d;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%u: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)

static BYTE metadata[72], bytes[8192];
static tg_gpu_linear_allocation_request allocation;
static uint64_t pagingValues[2];
static const wsi_win32_memory *owners[2];
static unsigned allocations, deallocations, maps, frees, locks, unlocks, waits, presents;
static HRESULT presentStatus;
static bool ignoreImport;
static HANDLE rt_device(unsigned i) { return (HANDLE)(uintptr_t)(0x100 + i); }
static HANDLE rt_resource(unsigned i) { return (HANDLE)(uintptr_t)(0x200 + i); }
static unsigned index(HANDLE h) { CHECK(h == rt_device(0) || h == rt_device(1)); return (uintptr_t)h - 0x100; }

static void init(Device *dev, Adapter *adapter, unsigned i)
{
   dev->adapter = adapter; dev->nvk = true; dev->hRTDevice.handle = rt_device(i);
   adapter->luid = 0x112233;
   adapter->vk.caps.memory.memoryTypeCount = 1;
   adapter->vk.caps.memory.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
   dev->vkdev = (VkDevice)(uintptr_t)(i + 1);
   auto &cb = dev->kt;
   cb.pfnCreateContextVirtualCb = [](HANDLE h, D3DDDICB_CREATECONTEXTVIRTUAL *a) -> HRESULT {
      a->hContext = (HANDLE)(uintptr_t)(0x300 + index(h)); return S_OK;
   };
   cb.pfnDestroyContextCb = [](HANDLE h, const D3DDDICB_DESTROYCONTEXT *a) -> HRESULT {
      CHECK(a->hContext == (HANDLE)(uintptr_t)(0x300 + index(h))); return S_OK;
   };
   cb.pfnCreatePagingQueueCb = [](HANDLE h, D3DDDICB_CREATEPAGINGQUEUE *a) -> HRESULT {
      unsigned n = index(h); a->hPagingQueue = 0x40 + n; a->hSyncObject = 0x50 + n;
      a->FenceValueCPUVirtualAddress = &pagingValues[n]; return S_OK;
   };
   cb.pfnDestroyPagingQueueCb = [](HANDLE h, const D3DDDI_DESTROYPAGINGQUEUE *a) -> HRESULT {
      CHECK(a->hPagingQueue == 0x40 + index(h)); return S_OK;
   };
   cb.pfnAllocateCb = [](HANDLE h, D3DDDICB_ALLOCATE *a) -> HRESULT {
      CHECK(index(h) == 0 && a->hResource == rt_resource(0) && a->NumAllocations == 1);
      CHECK(a->PrivateDriverDataSize == sizeof(metadata));
      CHECK(a->pAllocationInfo2->PrivateDriverDataSize == sizeof(allocation));
      memcpy(metadata, a->pPrivateDriverData, sizeof(metadata));
      memcpy(&allocation, a->pAllocationInfo2->pPrivateDriverData, sizeof(allocation));
      CHECK(allocation.Width == 65 && allocation.Height == 17 && allocation.Pitch == 384);
      CHECK(allocation.Buffer.bytes == sizeof(bytes));
      a->pAllocationInfo2->hAllocation = 0x60; ++allocations; return S_OK;
   };
   cb.pfnDeallocateCb = [](HANDLE h, const D3DDDICB_DEALLOCATE *a) -> HRESULT {
      CHECK(a->hResource == rt_resource(index(h)) && !a->NumAllocations && !a->HandleList);
      ++deallocations; return S_OK;
   };
   cb.pfnMakeResidentCb = [](HANDLE h, D3DDDI_MAKERESIDENT *a) -> HRESULT {
      CHECK(a->hPagingQueue == 0x40 + index(h) && a->NumAllocations == 1);
      CHECK(*a->AllocationList == 0x60 + index(h));
      a->PagingFenceValue = 7; return E_PENDING;
   };
   cb.pfnWaitForSynchronizationObjectFromCpuCb = [](HANDLE h,
         const D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMCPU *a) -> HRESULT {
      unsigned n = index(h); CHECK(a->ObjectCount == 1 && *a->ObjectHandleArray == 0x50 + n);
      pagingValues[n] = *a->FenceValueArray; ++waits; return S_OK;
   };
   cb.pfnMapGpuVirtualAddressCb = [](HANDLE h, D3DDDI_MAPGPUVIRTUALADDRESS *a) -> HRESULT {
      CHECK(a->hPagingQueue == 0x40 + index(h) && a->hAllocation == 0x60 + index(h));
      CHECK(a->BaseAddress == 0x100000 && a->SizeInPages == 2 && !a->OffsetInPages);
      a->VirtualAddress = a->BaseAddress; a->PagingFenceValue = 8; ++maps; return E_PENDING;
   };
   cb.pfnFreeGpuVirtualAddressCb = [](HANDLE h, const D3DDDICB_FREEGPUVIRTUALADDRESS *a) -> HRESULT {
      index(h); CHECK(a->BaseAddress == 0x100000 && a->Size == sizeof(bytes)); ++frees; return S_OK;
   };
   cb.pfnLock2Cb = [](HANDLE h, D3DDDICB_LOCK2 *a) -> HRESULT {
      CHECK(a->hAllocation == 0x60 + index(h)); a->pData = bytes; ++locks; return S_OK;
   };
   cb.pfnUnlock2Cb = [](HANDLE h, const D3DDDICB_UNLOCK2 *a) -> HRESULT {
      CHECK(a->hAllocation == 0x60 + index(h)); ++unlocks; return S_OK;
   };
   dev->vk.GetImageMemoryRequirements = [](VkDevice, VkImage, VkMemoryRequirements *r) {
      *r = {sizeof(bytes), 4096, 1};
   };
   dev->vk.AllocateMemory = [](VkDevice vkdev, const VkMemoryAllocateInfo *a,
                              const VkAllocationCallbacks *, VkDeviceMemory *memory) {
      unsigned n = (uintptr_t)vkdev - 1;
      CHECK(a->memoryTypeIndex == 0 && a->allocationSize == sizeof(bytes));
      const auto *import = static_cast<const wsi_memory_allocate_info *>(a->pNext);
      CHECK(import->sType == VK_STRUCTURE_TYPE_WSI_MEMORY_ALLOCATE_INFO_MESA && import->win32_imported);
      *memory = (VkDeviceMemory)(uintptr_t)(n + 1);
      if (ignoreImport) return VK_SUCCESS;
      const auto *owner = import->win32_memory;
      CHECK(owner->adapter_luid == 0x112233 && owner->allocation == 0x60 + n);
      owner->retain(owner->context); owners[n] = owner;
      CHECK(owner->map_gpu(owner->context, 0x100000, 0, sizeof(bytes), 0, false,
                            0x100000, sizeof(bytes)) == VK_SUCCESS);
      void *cpu = nullptr; CHECK(owner->map_cpu(owner->context, &cpu) == VK_SUCCESS && cpu == bytes);
      *import->win32_imported = true;
      return VK_SUCCESS;
   };
   dev->vk.FreeMemory = [](VkDevice vkdev, VkDeviceMemory, const VkAllocationCallbacks *) {
      unsigned n = (uintptr_t)vkdev - 1;
      if (owners[n]) {
         const auto *owner = owners[n];
         CHECK(owner->free_gpu(owner->context, 0x100000, sizeof(bytes)) == VK_SUCCESS);
         owner->unmap_cpu(owner->context); owner->release(owner->context); owners[n] = nullptr;
      }
   };
   dev->vk.BindImageMemory = [](VkDevice, VkImage, VkDeviceMemory, VkDeviceSize offset) {
      CHECK(!offset); return VK_SUCCESS;
   };
   CHECK(SUCCEEDED(runtime_device_create(dev)));
}

void runtime_contracts()
{
   Adapter adapter; Device producer, consumer; init(&producer, &adapter, 0); init(&consumer, &adapter, 1);
   D3D10DDI_MIPINFO mip = {}; mip.TexelWidth = 65; mip.TexelHeight = 17; mip.TexelDepth = 1;
   D3D10DDIARG_CREATERESOURCE desc = {};
   desc.pMipInfoList = &mip; desc.ResourceDimension = D3D10DDIRESOURCE_TEXTURE2D;
   desc.Usage = D3D10_DDI_USAGE_DEFAULT; desc.BindFlags = D3D10_DDI_BIND_RENDER_TARGET | D3D10_DDI_BIND_SHADER_RESOURCE;
   desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1; desc.MipLevels = desc.ArraySize = 1;
   desc.MiscFlags = D3D10_DDI_RESOURCE_MISC_SHARED | D3D10_DDI_RESOURCE_MISC_DISCARD_ON_PRESENT;
   RuntimeResource *p = nullptr, *c = nullptr; UINT pitch = 0;
   CHECK(SUCCEEDED(runtime_shared_create(&producer, &desc, {rt_resource(0)}, &p, &pitch)) && pitch == 384);
   D3DDDI_OPENALLOCATIONINFO info = {}; info.hAllocation = 0x61;
   info.pPrivateDriverData = &allocation; info.PrivateDriverDataSize = sizeof(allocation);
   D3D10DDIARG_OPENRESOURCE open = {}; open.NumAllocations = 1; open.pOpenAllocationInfo = &info;
   open.hKMResource.handle = 0x99; open.pPrivateDriverData = metadata; open.PrivateDriverDataSize = sizeof(metadata);
   RuntimeSharedDescription description;
   CHECK(SUCCEEDED(runtime_shared_open(&consumer, &open, {rt_resource(1)}, &c, &description)));
   CHECK(allocations == 1 && description.pitch == 384 && description.width == 65 && description.height == 17);
   VkDeviceMemory pm, cm;
   CHECK(SUCCEEDED(runtime_bind_image(&producer, p, (VkImage)1, &pm)));
   CHECK(SUCCEEDED(runtime_bind_image(&consumer, c, (VkImage)2, &cm)));
   CHECK(maps == 2 && locks == 2 && waits == 4);
   bytes[123] = 0x5a;
   producer.vk.FreeMemory(producer.vkdev, pm, nullptr); CHECK(SUCCEEDED(runtime_destroy(p)));
   CHECK(deallocations == 1 && bytes[123] == 0x5a && runtime_allocation(c) == 0x61);
   CHECK(owners[1] && !owners[0]);
   consumer.vk.FreeMemory(consumer.vkdev, cm, nullptr); CHECK(SUCCEEDED(runtime_destroy(c)));
   CHECK(deallocations == 2 && frees == 2 && unlocks == 2);
   std::puts("PASS: 65x17 shared pitch/metadata, E_PENDING paging, consumer handles and independent owner teardown");

   /* The ICD must positively acknowledge importing the runtime allocation. */
   CHECK(SUCCEEDED(runtime_shared_create(&producer, &desc, {rt_resource(0)}, &p, &pitch)));
   ignoreImport = true;
   CHECK(runtime_bind_image(&producer, p, (VkImage)1, &pm) == DXGI_ERROR_UNSUPPORTED && !pm);
   ignoreImport = false;
   DXGI_DDI_BASE_CALLBACKS callbacks = {};
   callbacks.pfnPresentCb = [](HANDLE h, DXGIDDICB_PRESENT *a) -> HRESULT {
      CHECK(h == rt_device(0) && a->hContext == (HANDLE)(uintptr_t)0x300);
      CHECK(a->hSrcAllocation == 0x60 && !a->hDstAllocation && a->pDXGIContext == (void *)0x12345);
      CHECK(a->SyncIntervalOverrideValid && a->SyncIntervalOverride == DXGI_DDI_FLIP_INTERVAL_ONE);
      ++presents; return presentStatus;
   };
   producer.dxgi = &callbacks;
   Resource surface; surface.dev = &producer; surface.storage.runtime = p;
   DXGI_DDI_ARG_PRESENT present = {}; present.hDevice = (DXGI_DDI_HDEVICE)&producer;
   present.hSurfaceToPresent = (DXGI_DDI_HRESOURCE)&surface; present.Flags.Blt = 1;
   present.pDXGIContext = (void *)0x12345; present.FlipInterval = DXGI_DDI_FLIP_INTERVAL_IMMEDIATE;
   DXGI1_1_DDI_BASE_FUNCTIONS dxgi = {}; dxgi_fill_table(&dxgi, true);
   presentStatus = DXGI_STATUS_OCCLUDED;
   CHECK(dxgi.pfnPresent(&present) == DXGI_STATUS_OCCLUDED && presents == 1);
   presentStatus = DXGI_ERROR_DEVICE_REMOVED;
   CHECK(dxgi.pfnPresent(&present) == DXGI_ERROR_DEVICE_REMOVED && FAILED(producer.lost));
   CHECK(SUCCEEDED(runtime_destroy(p)));
   runtime_device_destroy(&consumer); runtime_device_destroy(&producer);
   std::puts("PASS: rejected ignored owner import; Present preserves token/status and requests interval one (no timing proof)");
}
