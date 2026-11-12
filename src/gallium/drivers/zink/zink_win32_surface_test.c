/* SPDX-License-Identifier: MIT */
/* Actual Zink surface creation with Vulkan spies; no GPU execution. */
#include <stdio.h>
#include <stdlib.h>
#include "zink_context.h"
#include "zink_resource.h"
#include "zink_screen.h"
#include "zink_surface.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static struct zink_screen screen;
static struct zink_context ctx;
static VkImageUsageFlags parent_usage, view_usage;
static VkFormat view_format;
static unsigned creates, destroys;

static VKAPI_ATTR VkResult VKAPI_CALL
create_view(VkDevice device, const VkImageViewCreateInfo *info,
            const VkAllocationCallbacks *alloc, VkImageView *out)
{
   CHECK(info->image == (VkImage)(uintptr_t)0x1000);
   CHECK(info->format == view_format && info->viewType == VK_IMAGE_VIEW_TYPE_2D);
   const VkImageViewUsageCreateInfo *usage = info->pNext;
   CHECK(!usage || (usage->sType == VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO && !usage->pNext));
   view_usage = usage ? usage->usage : parent_usage;
   *out = (VkImageView)(uintptr_t)(0x2000 + ++creates);
   return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
destroy_view(VkDevice device, VkImageView view, const VkAllocationCallbacks *alloc)
{
   CHECK(view);
   destroys++;
}

static void
check_views(enum pipe_format unorm, enum pipe_format srgb,
            bool imported, bool linear, bool attachment)
{
   struct zink_resource_object obj = {0};
   struct zink_resource res = {0};
   obj.image = (VkImage)(uintptr_t)0x1000;
   obj.is_wddm_allocation = imported;
   obj.immutable_handle = imported;
   obj.vkflags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
   obj.vkusage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT;
   if (attachment)
      obj.vkusage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
   obj.vkfeats = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
   _mesa_set_init(&obj.surface_cache, NULL, NULL, equals_surface_key);
   simple_mtx_init(&obj.surface_mtx, mtx_plain);
   res.obj = &obj;
   res.base.b.target = PIPE_TEXTURE_2D;
   res.base.b.format = unorm;
   res.base.b.array_size = 1;
   res.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
   res.linear = linear;
   parent_usage = obj.vkusage;
   const enum pipe_format formats[] = {unorm, srgb};
   for (unsigned i = 0; i < ARRAY_SIZE(formats); ++i) {
      screen.format_props_init[formats[i]] = true;
      screen.format_props[formats[i]].linearTilingFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
      screen.format_props[formats[i]].optimalTilingFeatures = obj.vkfeats;
      view_format = zink_get_format(&screen, formats[i]);
      struct pipe_surface templ = {.texture = &res.base.b, .format = formats[i]};
      unsigned before = creates;
      struct zink_surface *surface = zink_create_fb_surface(&ctx.base, &templ);
      CHECK(surface && creates == before + 1);
      VkImageUsageFlags expected = parent_usage;
      if (linear && !imported)
         expected &= ~VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
      printf("view format=%u imported=%u linear=%u parent=0x%x actual=0x%x expected=0x%x\n",
             view_format, imported, linear, parent_usage, view_usage, expected);
      fflush(stdout);
      CHECK(view_usage == expected);
      CHECK(zink_create_fb_surface(&ctx.base, &templ) == surface && creates == before + 1);
      CHECK(obj.image == (VkImage)(uintptr_t)0x1000);
   }
   zink_destroy_resource_surface_cache(&screen, &obj.surface_cache, false);
   simple_mtx_destroy(&obj.surface_mtx);
}

int main(void)
{
   ctx.base.screen = &screen.base;
   ctx.rp_draw = true;
   screen.vk.CreateImageView = create_view;
   screen.vk.DestroyImageView = destroy_view;
   const enum pipe_format formats[][2] = {
      {PIPE_FORMAT_B8G8R8A8_UNORM, PIPE_FORMAT_B8G8R8A8_SRGB},
      {PIPE_FORMAT_R8G8B8A8_UNORM, PIPE_FORMAT_R8G8B8A8_SRGB},
   };
   for (unsigned i = 0; i < ARRAY_SIZE(formats); ++i) {
      check_views(formats[i][0], formats[i][1], true, true, true);
      check_views(formats[i][0], formats[i][1], false, true, true);
      check_views(formats[i][0], formats[i][1], false, false, true);
      check_views(formats[i][0], formats[i][1], true, true, false);
   }
   CHECK(creates == 16 && destroys == creates);
   puts("PASS: actual Zink UNORM/sRGB view usage, imported storage identity, cache and ordinary linear/optimal controls; mocked Vulkan, no GPU execution");
   return 0;
}
