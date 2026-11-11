/* SPDX-License-Identifier: MIT */
#ifndef VK_WSI_WIN32_MEMORY_H
#define VK_WSI_WIN32_MEMORY_H

#include <vulkan/vulkan_core.h>
#include <stdbool.h>
#include <stdint.h>

/* Mesa-internal import of a runtime-owned WDDM allocation. These are not
 * Win32 external-memory handles and must never be passed to ShareObjects or
 * used with another device's paging queue. The owner retains the allocation
 * and executes each operation on the device that created/opened it.
 *
 * GPUVA reservations are process-wide. map_gpu maps the owner's allocation
 * into the supplied reservation and waits for its paging fence before
 * returning success. The owner records the whole reservation, releases it
 * before deallocating the runtime resource, and makes a later free_gpu a no-op
 * for a range already released. WDDM automatically releases allocation GPUVAs;
 * cached importer objects must not free a subsequently reused address.
 *
 * retain/release protect the owner context. The D3D frontend must finish GPU
 * use before destroying its runtime resource; importing does not change the
 * runtime's CreateResource/DestroyResource lifetime contract.
 */
#define WSI_WIN32_MEMORY_VERSION 1u
struct wsi_win32_memory {
   uint32_t version;
   uint32_t allocation;
   uint64_t adapter_luid;
   uint64_t size;
   bool read_only;
   void *context;
   void (*retain)(void *context);
   void (*release)(void *context);
   VkResult (*map_gpu)(void *context, uint64_t address, uint64_t offset,
                       uint64_t size, uint8_t kind, bool read_only,
                       uint64_t reservation_base, uint64_t reservation_size);
   VkResult (*free_gpu)(void *context, uint64_t reservation_base, uint64_t reservation_size);
   VkResult (*map_cpu)(void *context, void **address);
   void (*unmap_cpu)(void *context);
};

#endif
