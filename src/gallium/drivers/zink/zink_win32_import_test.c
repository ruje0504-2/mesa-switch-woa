/* SPDX-License-Identifier: MIT */
/* Actual Windows resource import and render-attachment classification.
 * Vulkan memory/image calls are substituted; no GPU execution. */
#include <stdio.h>
#include <stdlib.h>
#include "zink_bo.h"
#include "zink_context.h"
#include "zink_instance.h"
#include "zink_render_pass.h"
#include "zink_resource.h"
#include "zink_screen.h"
#include "util/u_inlines.h"
#include "util/u_transfer_helper.h"
#include "frontend/winsys_handle.h"
#include "vk_internal_exts.h"
#include "vk_wsi_win32_memory.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static struct zink_screen screen;
static struct zink_context ctx;
static struct zink_instance_info instance;
static struct wsi_win32_memory memory;
static unsigned images, destroyed_images, allocations, frees;

static VKAPI_ATTR VkResult VKAPI_CALL
image_properties(VkPhysicalDevice device, const VkPhysicalDeviceImageFormatInfo2 *info,
                 VkImageFormatProperties2 *out)
{
   out->imageFormatProperties = (VkImageFormatProperties) {
      .maxExtent = {4096, 4096, 1}, .maxMipLevels = 1, .maxArrayLayers = 1,
      .sampleCounts = VK_SAMPLE_COUNT_1_BIT, .maxResourceSize = 512ull << 20,
   };
   return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL
create_image(VkDevice device, const VkImageCreateInfo *info,
             const VkAllocationCallbacks *allocator, VkImage *out)
{
   CHECK(info->tiling == VK_IMAGE_TILING_LINEAR);
   CHECK(info->extent.width == 32 && info->extent.height == 16);
   CHECK(info->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
   *out = (VkImage)(uintptr_t)(0x1000 + ++images);
   return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
destroy_image(VkDevice device, VkImage image, const VkAllocationCallbacks *allocator)
{
   CHECK(image);
   destroyed_images++;
}

static VKAPI_ATTR void VKAPI_CALL
image_requirements(VkDevice device, VkImage image, VkMemoryRequirements *out)
{
   *out = (VkMemoryRequirements){.size = 4096, .alignment = 4096, .memoryTypeBits = 1};
}

static VKAPI_ATTR void VKAPI_CALL
image_requirements2(VkDevice device, const VkImageMemoryRequirementsInfo2 *info,
                    VkMemoryRequirements2 *out)
{
   image_requirements(device, info->image, &out->memoryRequirements);
}

static VKAPI_ATTR VkResult VKAPI_CALL
allocate_memory(VkDevice device, const VkMemoryAllocateInfo *info,
                const VkAllocationCallbacks *allocator, VkDeviceMemory *out)
{
   const VkBaseInStructure *next = info->pNext;
   while (next && next->sType != VK_STRUCTURE_TYPE_WSI_MEMORY_ALLOCATE_INFO_MESA)
      next = next->pNext;
   CHECK(next);
   const struct wsi_memory_allocate_info *wsi = (const void *)next;
   CHECK(wsi->win32_memory == &memory && wsi->win32_imported);
   *wsi->win32_imported = true;
   *out = (VkDeviceMemory)(uintptr_t)(0x2000 + ++allocations);
   return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
free_memory(VkDevice device, VkDeviceMemory mem, const VkAllocationCallbacks *allocator)
{
   CHECK(mem);
   frees++;
}

static VKAPI_ATTR VkResult VKAPI_CALL
bind_image(VkDevice device, VkImage image, VkDeviceMemory mem, VkDeviceSize offset)
{
   CHECK(image && mem && offset == 0);
   return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL
bind_image2(VkDevice device, uint32_t count, const VkBindImageMemoryInfo *infos)
{
   CHECK(count == 1);
   return bind_image(device, infos[0].image, infos[0].memory, infos[0].memoryOffset);
}

static void
check_import(enum pipe_format format)
{
   struct pipe_resource templ = {
      .target = PIPE_TEXTURE_2D, .format = format,
      .width0 = 32, .height0 = 16, .depth0 = 1, .array_size = 1,
      .bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW,
      .usage = PIPE_USAGE_DEFAULT,
   };
   struct winsys_handle handle = {
      .type = WINSYS_HANDLE_TYPE_WDDM_ALLOCATION, .format = format,
      .stride = 128, .com_obj = &memory,
   };
   struct pipe_resource *resource = screen.base.resource_from_handle(
      &screen.base, &templ, &handle, PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE);
   CHECK(resource);
   CHECK(zink_resource(resource)->obj->immutable_handle);
   ctx.fb_state.nr_cbufs = 1;
   ctx.fb_state.cbufs[0].texture = resource;
   ctx.fb_formats[0] = zink_get_format(&screen, format);
   struct zink_rt_attrib rt = {0};
   zink_init_color_attachment(&ctx, 0, &rt);
   printf("import format=%u existing_contents_discardable=%u\n", format, rt.invalid);
   fflush(stdout);
   CHECK(!rt.invalid);

   /* Explicit invalidation remains allowed after the imported contents were
    * consumed. */
   ctx.base.invalidate_resource(&ctx.base, resource);
   zink_init_color_attachment(&ctx, 0, &rt);
   CHECK(rt.invalid);
   ctx.fb_state.cbufs[0].texture = NULL;
   pipe_resource_reference(&resource, NULL);

   unsigned before = images;
   handle.stride = 0;
   CHECK(!screen.base.resource_from_handle(&screen.base, &templ, &handle, 0));
   CHECK(images == before);
}

int main(void)
{
   SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
   ctx.base.screen = &screen.base;
   screen.instance_info = &instance;
   screen.info.have_KHR_external_memory_win32 = true;
   screen.info.mem_props.memoryHeapCount = 1;
   screen.info.mem_props.memoryHeaps[0].size = 512ull << 20;
   screen.info.mem_props.memoryTypeCount = 1;
   screen.info.mem_props.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
   screen.info.props.limits.minMemoryMapAlignment = 64;
   for (unsigned i = 0; i < ZINK_HEAP_MAX; ++i)
      screen.heap_count[i] = 1;
   screen.vk.CreateImage = create_image;
   screen.vk.GetPhysicalDeviceImageFormatProperties2 = image_properties;
   screen.vk.DestroyImage = destroy_image;
   screen.vk.GetImageMemoryRequirements = image_requirements;
   screen.vk.GetImageMemoryRequirements2 = image_requirements2;
   screen.vk.AllocateMemory = allocate_memory;
   screen.vk.FreeMemory = free_memory;
   screen.vk.BindImageMemory = bind_image;
   screen.vk.BindImageMemory2 = bind_image2;
   const enum pipe_format formats[] = {
      PIPE_FORMAT_B8G8R8A8_UNORM, PIPE_FORMAT_B8G8R8A8_SRGB,
      PIPE_FORMAT_R8G8B8A8_UNORM, PIPE_FORMAT_R8G8B8A8_SRGB,
   };
   for (unsigned i = 0; i < ARRAY_SIZE(formats); ++i) {
      screen.format_props_init[formats[i]] = true;
      screen.format_props[formats[i]].optimalTilingFeatures =
         VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
   }
   CHECK(zink_bo_init(&screen));
   CHECK(zink_screen_resource_init(&screen.base));
   zink_context_resource_init(&ctx.base);
   check_import(formats[0]);
   check_import(formats[2]);
   CHECK(images == 2 && destroyed_images == images && allocations == 2 && frees == allocations);
   u_transfer_helper_destroy(screen.base.transfer_helper);
   zink_bo_deinit(&screen);
   puts("PASS: actual WDDM resource import preserves render-attachment contents; Vulkan calls substituted, no GPU execution");
   return 0;
}
