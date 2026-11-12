/* SPDX-License-Identifier: MIT */
/* Actual buffer transfer and usage polling with controlled memory/timelines.
 * No native GPU commands are submitted. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zink_bo.h"
#include "zink_context.h"
#include "zink_resource.h"
#include "zink_screen.h"
#include "util/u_inlines.h"

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

int main(int argc, char **argv)
{
   SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
   CHECK(argc == 2);
   init();
   if (!strcmp(argv[1], "--write-contention")) write_contention();
   else if (!strcmp(argv[1], "--pending-cleanup")) pending_cleanup();
   else if (!strcmp(argv[1], "--recycled-usage")) recycled_usage();
   else CHECK(false);
   simple_mtx_destroy(&bo.lock);
   puts("PASS: real Zink buffer map/poll ownership control; no native GPU execution");
   return 0;
}
