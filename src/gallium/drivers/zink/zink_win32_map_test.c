/* SPDX-License-Identifier: MIT */
/* Actual buffer/image transfers and usage polling with controlled memory/timelines.
 * No native GPU commands are submitted. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zink_bo.h"
#include "zink_context.h"
#include "zink_instance.h"
#include "zink_resource.h"
#include "zink_screen.h"
#include "util/u_inlines.h"
#include "util/u_transfer_helper.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static struct zink_screen screen;
static struct zink_context ctx;
static struct zink_resource resource;
static struct zink_resource_object object;
static struct zink_bo bo;
static struct zink_batch_usage use;
static unsigned char bytes[64];
static unsigned polls, memory_maps;
static bool ready;
static struct zink_instance_info instance_info;
static unsigned char image_bytes[4096], device_bytes[4096];
static unsigned invalidates, flushes, unmaps;
static bool invalidate_fails;
static VkMappedMemoryRange invalidated;
static VkDeviceSize image_base_offset = 64;

static VKAPI_ATTR void VKAPI_CALL
image_layout(VkDevice device, VkImage image, const VkImageSubresource *subresource,
             VkSubresourceLayout *layout)
{
   CHECK(image == (VkImage)(uintptr_t)1);
   CHECK(subresource->mipLevel == 0 && subresource->arrayLayer == 0);
   *layout = (VkSubresourceLayout) {
      .offset = image_base_offset, .size = sizeof(image_bytes) - image_base_offset, .rowPitch = 128,
      .arrayPitch = 1024,
      /* Vulkan leaves this undefined for non-3D images. */
      .depthPitch = resource.base.b.target == PIPE_TEXTURE_3D ? 512 : 0,
   };
}

static VKAPI_ATTR VkResult VKAPI_CALL
invalidate_image(VkDevice device, uint32_t count, const VkMappedMemoryRange *ranges)
{
   CHECK(count == 1 && ranges[0].memory == bo.mem);
   invalidates++;
   invalidated = ranges[0];
   CHECK(invalidated.offset + invalidated.size <= sizeof(image_bytes));
   if (invalidate_fails)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   /* Simulate CPU cache visibility, without replacing the texture mapper. */
   memcpy(image_bytes + invalidated.offset, device_bytes + invalidated.offset,
          invalidated.size);
   return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL
flush_image(VkDevice device, uint32_t count, const VkMappedMemoryRange *ranges)
{
   flushes++;
   return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
unmap_image(VkDevice device, VkDeviceMemory memory)
{
   CHECK(memory == bo.mem);
   unmaps++;
}

static VKAPI_ATTR VkResult VKAPI_CALL
wait_semaphores(VkDevice device, const VkSemaphoreWaitInfo *info, uint64_t timeout)
{
   CHECK(!timeout && info->semaphoreCount == 1 && info->pValues[0] == 7);
   polls++;
   return ready ? VK_SUCCESS : VK_TIMEOUT;
}

static VKAPI_ATTR VkResult VKAPI_CALL
fail_map(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset,
         VkDeviceSize size, VkMemoryMapFlags flags, void **out)
{
   memory_maps++;
   return VK_ERROR_MEMORY_MAP_FAILED;
}

static void
init(void)
{
   ctx.base.screen = &screen.base;
   screen.vk.WaitSemaphores = wait_semaphores;
   screen.vk.MapMemory = fail_map;
   screen.sem = (VkSemaphore)(uintptr_t)1;
   zink_context_resource_init(&ctx.base);
   resource.base.b.target = PIPE_BUFFER;
   resource.base.b.format = PIPE_FORMAT_R8_UINT;
   resource.base.b.width0 = sizeof(bytes);
   resource.base.b.screen = &screen.base;
   resource.base.is_shared = true; /* Never infer an unsynchronized write. */
   pipe_reference_init(&resource.base.b.reference, 1);
   resource.obj = &object;
   object.is_buffer = object.host_visible = object.coherent = true;
   object.bo = &bo;
   bo.mem = (VkDeviceMemory)(uintptr_t)1;
   bo.base.size = sizeof(bytes);
   bo.u.real.cpu_ptr = bytes;
   simple_mtx_init(&bo.lock, mtx_plain);
   use.usage = 7;
   use.submit_count = 1;
}

static void
map(unsigned access, bool succeeds)
{
   struct pipe_transfer *transfer = NULL;
   struct pipe_box box = {.width = sizeof(bytes), .height = 1, .depth = 1};
   unsigned maps_before = bo.u.real.map_count;
   /* FLUSH_EXPLICIT avoids an unrelated upload on write-unmap; this control
    * tests when CPU access is allowed, not GPU visibility of new data. */
   void *ptr = ctx.base.buffer_map(&ctx.base, &resource.base.b, 0,
      access | PIPE_MAP_DONTBLOCK | PIPE_MAP_THREAD_SAFE | PIPE_MAP_FLUSH_EXPLICIT,
      &box, &transfer);
   if (succeeds) {
      CHECK(ptr == bytes && transfer);
      CHECK(bo.u.real.map_count == maps_before + 1);
      ctx.base.buffer_unmap(&ctx.base, transfer);
   } else {
      CHECK(!ptr);
      CHECK(!transfer);
      CHECK(bo.u.real.map_count == maps_before);
   }
   CHECK(resource.base.b.reference.count == 1);
}

static void
write_contention(void)
{
   /* A submitted GPU reader conflicts with a CPU writer, but not a reader. */
   bo.reads.u = &use;
   bo.reads.submit_count = 0;
   map(PIPE_MAP_WRITE, false);
   map(PIPE_MAP_READ_WRITE, false);
   CHECK(polls == 2);
   map(PIPE_MAP_READ, true);
   CHECK(polls == 2);
   ready = true;
   map(PIPE_MAP_WRITE, true);
   CHECK(polls == 3);
}

static void
pending_cleanup(void)
{
   bo.writes.u = &use;
   bo.writes.submit_count = 0;
   for (unsigned i = 0; i < 8; ++i)
      map(PIPE_MAP_READ, false);
   CHECK(polls == 8);
   ready = true;
   map(PIPE_MAP_READ, true);
   CHECK(polls == 9);
   bo.writes.u = NULL;
   /* A failed Vulkan mapping must release the same transfer/resource refs. */
   bo.u.real.cpu_ptr = NULL;
   map(PIPE_MAP_READ, false);
   CHECK(memory_maps == 1);
}

static void
recycled_usage(void)
{
   bo.writes.u = &use;
   bo.writes.submit_count = 0;
   use.submit_count = 2;
   use.unflushed = true;
   /* The old write completed and its batch is now recording unrelated work. */
   CHECK(zink_bo_usage_check_completion(&screen, &bo, ZINK_RESOURCE_ACCESS_WRITE));
   CHECK(zink_bo_usage_check_completion_fast(&screen, &bo, ZINK_RESOURCE_ACCESS_WRITE));
   map(PIPE_MAP_READ, true);
   CHECK(!polls);
   /* A current unsubmitted write still prevents a CPU read. */
   bo.writes.submit_count = use.submit_count;
   map(PIPE_MAP_READ, false);
   CHECK(!polls);
}

static void
image_readback(enum pipe_texture_target target, enum pipe_format format,
               struct pipe_box box, unsigned offset, unsigned end,
               unsigned layer_stride, bool fail)
{
   screen.instance_info = &instance_info;
   screen.vk.GetImageSubresourceLayout = image_layout;
   screen.vk.InvalidateMappedMemoryRanges = invalidate_image;
   screen.vk.FlushMappedMemoryRanges = flush_image;
   screen.vk.UnmapMemory = unmap_image;
   screen.info.props.limits.nonCoherentAtomSize = 64;
   CHECK(zink_screen_resource_init(&screen.base));
   resource.base.b.target = target;
   resource.base.b.format = resource.internal_format = format;
   resource.base.b.width0 = resource.base.b.height0 = 32;
   resource.base.b.depth0 = target == PIPE_TEXTURE_3D ? 4 : 1;
   resource.base.b.array_size = target == PIPE_TEXTURE_2D_ARRAY ? 4 : 1;
   resource.linear = true;
   resource.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
   object.is_buffer = object.coherent = false;
   object.image = (VkImage)(uintptr_t)1;
   object.size = bo.base.size = sizeof(image_bytes);
   bo.u.real.cpu_ptr = image_bytes;
   invalidate_fails = fail;
   memset(device_bytes, 0x5a, sizeof(device_bytes));
   struct pipe_transfer *transfer = NULL;
   unsigned maps_before = bo.u.real.map_count;
   unsigned char *ptr = ctx.base.texture_map(&ctx.base, &resource.base.b, 0,
      PIPE_MAP_READ | PIPE_MAP_THREAD_SAFE | PIPE_MAP_FLUSH_EXPLICIT, &box, &transfer);
   if (fail) {
      CHECK(!ptr && !transfer);
      CHECK(bo.u.real.map_count == maps_before);
   } else {
      CHECK(ptr == image_bytes + offset && transfer);
      CHECK(transfer->stride == 128 && transfer->layer_stride == layer_stride);
      CHECK(*ptr == 0x5a && image_bytes[end - 1] == 0x5a);
      CHECK(invalidated.offset == offset / 64 * 64);
      CHECK(invalidated.offset + invalidated.size == (end + 63) / 64 * 64);
      ctx.base.texture_unmap(&ctx.base, transfer);
      /* Release the driver's persistent CPU mapping before ending the fixture. */
      zink_bo_unmap(&screen, &bo);
   }
   CHECK(invalidates == 1 && !flushes && unmaps == 1);
   CHECK(resource.base.b.reference.count == 1);
   u_transfer_helper_destroy(screen.base.transfer_helper);
}

int main(int argc, char **argv)
{
   SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
   CHECK(argc == 2);
   init();
   if (!strcmp(argv[1], "--write-contention")) write_contention();
   else if (!strcmp(argv[1], "--pending-cleanup")) pending_cleanup();
   else if (!strcmp(argv[1], "--recycled-usage")) recycled_usage();
   else if (!strcmp(argv[1], "--image-array"))
      image_readback(PIPE_TEXTURE_2D_ARRAY, PIPE_FORMAT_R8G8B8A8_UNORM,
                     (struct pipe_box){3, 1, 2, 4, 2, 2}, 2252, 3420, 1024, false);
   else if (!strcmp(argv[1], "--image-3d"))
      image_readback(PIPE_TEXTURE_3D, PIPE_FORMAT_R8G8B8A8_UNORM,
                     (struct pipe_box){3, 1, 2, 4, 2, 2}, 1228, 1884, 512, false);
   else if (!strcmp(argv[1], "--image-cache"))
      image_readback(PIPE_TEXTURE_2D, PIPE_FORMAT_R8G8B8A8_UNORM,
                     (struct pipe_box){3, 1, 0, 4, 2, 1}, 204, 348, 1024, false);
   else if (!strcmp(argv[1], "--image-compressed"))
      image_readback(PIPE_TEXTURE_2D, PIPE_FORMAT_DXT1_RGB,
                     (struct pipe_box){4, 4, 0, 8, 8, 1}, 200, 344, 1024, false);
   else if (!strcmp(argv[1], "--image-cache-failure"))
      image_readback(PIPE_TEXTURE_2D, PIPE_FORMAT_R8G8B8A8_UNORM,
                     (struct pipe_box){3, 1, 0, 4, 2, 1}, 204, 348, 1024, true);
   else if (!strcmp(argv[1], "--image-small-offset")) {
      image_base_offset = 0;
      image_readback(PIPE_TEXTURE_2D, PIPE_FORMAT_R8G8B8A8_UNORM,
                     (struct pipe_box){3, 0, 0, 14, 1, 1}, 12, 68, 1024, false);
   }
   else CHECK(false);
   simple_mtx_destroy(&bo.lock);
   puts("PASS: real Zink transfer/layout/cache control; no native GPU execution");
   return 0;
}
