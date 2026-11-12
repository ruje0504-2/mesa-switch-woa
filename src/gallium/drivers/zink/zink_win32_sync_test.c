/* SPDX-License-Identifier: MIT */
/* Exercise Zink's actual import/wait/signal functions with Vulkan spies.
 * This tests recording and ownership, not GPU completion. */
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_win32.h>
#include "zink_context.h"
#include "zink_fence.h"
#include "zink_screen.h"
#include "zink_batch.h"
#include "zink_query.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static VkResult create_result, import_result;
static unsigned creates, imports, destroys, flushes;
static VkSemaphoreType created_type;
static VkExternalSemaphoreHandleTypeFlagBits imported_type;
static HANDLE imported_handle;
static const wchar_t *imported_name;
static DWORD callback_thread;
static uint64_t prepared_value;
static unsigned prepares, completions, submissions;
static bool prepare_ok = true, completion_ok = true, submitted_success;
static bool expect_submit_worker;
static bool bridge_active = true;
static VkResult submit_result = VK_SUCCESS;
static VkSemaphore bridge_wait, bridge_signal;
static unsigned idle_waits;
static struct zink_batch_state *event_batch;
static unsigned event_flushes, event_polls;
static bool event_ready;

static void
event_flush(struct pipe_context *pipe, struct pipe_fence_handle **fence, unsigned flags)
{
   CHECK(fence && flags == PIPE_FLUSH_DEFERRED);
   event_flushes++;
   struct zink_tc_fence *mfence = zink_create_tc_fence();
   CHECK(mfence);
   mfence->fence = &event_batch->fence;
   mfence->submit_count = event_batch->usage.submit_count;
   mfence->deferred_ctx = pipe;
   zink_context(pipe)->deferred_fence = &event_batch->fence;
   util_dynarray_append(&event_batch->fence.mfences, mfence);
   *fence = (struct pipe_fence_handle *)mfence;
}

static bool
event_finish(struct pipe_screen *screen, struct pipe_context *ctx,
             struct pipe_fence_handle *fence, uint64_t timeout)
{
   CHECK(!ctx && timeout == 0);
   CHECK(zink_tc_fence(fence)->fence == &event_batch->fence);
   event_polls++;
   return event_ready;
}

static void
test_query_polling(void)
{
   struct zink_screen *screen = calloc(1, sizeof(*screen));
   struct zink_context *ctx = calloc(1, sizeof(*ctx));
   event_batch = calloc(1, sizeof(*event_batch));
   CHECK(screen && ctx && event_batch);
   zink_screen_fence_init(&screen->base);
   screen->base.fence_finish = event_finish;
   ctx->base.screen = &screen->base;
   ctx->base.flush = event_flush;
   zink_context_query_init(&ctx->base);
   struct pipe_query *query = ctx->base.create_query(&ctx->base, PIPE_QUERY_GPU_FINISHED, 0);
   CHECK(query && ctx->base.get_query_result_no_flush);
   CHECK(ctx->base.end_query(&ctx->base, query));
   union pipe_query_result result = {0};
   /* QueryEnd deferred this event. A no-flush poll must neither submit it
    * nor ask fence_finish to wait on an unsubmitted fence. */
   CHECK(!ctx->base.get_query_result_no_flush(&ctx->base, query, &result));
   CHECK(event_flushes == 1 && event_polls == 0);
   /* Model an internal batch submission, without a frontend Flush call.
    * The same query must become pollable and later report completion. */
   ctx->deferred_fence = NULL;
   event_batch->usage.submit_count++;
   CHECK(!ctx->base.get_query_result_no_flush(&ctx->base, query, &result));
   CHECK(event_flushes == 1 && event_polls == 1);
   event_ready = true;
   CHECK(ctx->base.get_query_result_no_flush(&ctx->base, query, &result) && result.b);
   CHECK(event_flushes == 1 && event_polls == 2);
   /* Reusing the batch for a new deferred event must not make this old,
    * completed query appear unsubmitted again. */
   ctx->deferred_fence = &event_batch->fence;
   CHECK(ctx->base.get_query_result_no_flush(&ctx->base, query, &result) && result.b);
   CHECK(event_flushes == 1 && event_polls == 3);
   ctx->base.destroy_query(&ctx->base, query);
   CHECK(!event_batch->fence.mfences.size);
   query = ctx->base.create_query(&ctx->base, PIPE_QUERY_GPU_FINISHED, 0);
   CHECK(query && ctx->base.end_query(&ctx->base, query));
   /* Batch destruction detaches completed fence wrappers. Exercise the real
    * fence_finish path for this terminal state, not the completion spy. */
   struct zink_tc_fence *detached = *util_dynarray_element(&event_batch->fence.mfences, struct zink_tc_fence *, 0);
   detached->fence = NULL;
   util_dynarray_clear(&event_batch->fence.mfences);
   ctx->deferred_fence = NULL;
   zink_screen_fence_init(&screen->base);
   CHECK(ctx->base.get_query_result_no_flush(&ctx->base, query, &result) && result.b);
   CHECK(event_flushes == 2 && event_polls == 3);
   ctx->base.destroy_query(&ctx->base, query);
   util_dynarray_fini(&event_batch->fence.mfences);
   free(event_batch); event_batch = NULL;
   free(ctx); free(screen);
}

static VKAPI_ATTR VkResult VKAPI_CALL
queue_wait_idle(VkQueue queue)
{
   CHECK(GetCurrentThreadId() == callback_thread);
   idle_waits++;
   return VK_SUCCESS;
}

static bool
prepare_submission(void *data, uint64_t value)
{
   CHECK(data == &prepared_value && GetCurrentThreadId() == callback_thread);
   CHECK(value > prepared_value);
   prepared_value = value;
   prepares++;
   return prepare_ok;
}

static bool
complete_submission(void *data, uint64_t value, bool success)
{
   CHECK(data == &prepared_value && value == prepared_value);
   CHECK(GetCurrentThreadId() == callback_thread);
   submitted_success = success;
   completions++;
   return success && completion_ok;
}

static VKAPI_ATTR void VKAPI_CALL
pipeline_barrier(VkCommandBuffer command, VkPipelineStageFlags src,
                 VkPipelineStageFlags dst, VkDependencyFlags flags,
                 uint32_t memory_count, const VkMemoryBarrier *memory,
                 uint32_t buffer_count, const VkBufferMemoryBarrier *buffers,
                 uint32_t image_count, const VkImageMemoryBarrier *images)
{
   CHECK(command);
}

static VKAPI_ATTR VkResult VKAPI_CALL
end_command_buffer(VkCommandBuffer command)
{
   CHECK(command);
   return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL
queue_submit(VkQueue queue, uint32_t count, const VkSubmitInfo *infos, VkFence fence)
{
   CHECK((GetCurrentThreadId() != callback_thread) == expect_submit_worker);
   unsigned waits = 0, commands = 0, signals = 0, completions = 0;
   for (unsigned i = 0; i < count; ++i) {
      const VkSubmitInfo *info = &infos[i];
      const VkTimelineSemaphoreSubmitInfo *timeline = info->pNext;
      for (unsigned j = 0; j < info->waitSemaphoreCount; ++j) {
         if (info->pWaitSemaphores[j] != bridge_wait) continue;
         CHECK(timeline && timeline->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO);
         CHECK(timeline->pWaitSemaphoreValues[j] == prepared_value);
         CHECK(info->pWaitDstStageMask[j] == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
         CHECK(!commands);
         waits++;
      }
      if (info->commandBufferCount) CHECK(waits == (bridge_active ? 1u : 0u) && !signals);
      commands += info->commandBufferCount;
      for (unsigned j = 0; j < info->signalSemaphoreCount; ++j) {
         if (info->pSignalSemaphores[j] == (VkSemaphore)(uintptr_t)0x9000) {
            CHECK(signals == (bridge_active ? 1u : 0u) && i == count - 1);
            completions++;
         }
         if (info->pSignalSemaphores[j] != bridge_signal) continue;
         CHECK(timeline && timeline->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO);
         CHECK(timeline->pSignalSemaphoreValues[j] == prepared_value && commands == 3);
         signals++;
      }
   }
   CHECK(waits == (bridge_active ? 1u : 0u) && commands == 3);
   CHECK(signals == (bridge_active ? 1u : 0u) && completions == 1);
   submissions++;
   return submit_result;
}

static VKAPI_ATTR VkResult VKAPI_CALL
create_semaphore(VkDevice device, const VkSemaphoreCreateInfo *info,
                 const VkAllocationCallbacks *alloc, VkSemaphore *out)
{
   creates++;
   const VkSemaphoreTypeCreateInfo *type = info->pNext;
   CHECK(type && type->sType == VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO);
   created_type = type->semaphoreType;
   if (create_result == VK_SUCCESS)
      *out = (VkSemaphore)(uintptr_t)(0x1000 + creates);
   return create_result;
}

static VKAPI_ATTR VkResult VKAPI_CALL
import_semaphore(VkDevice device, const VkImportSemaphoreWin32HandleInfoKHR *info)
{
   imports++;
   CHECK(info->sType == VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR);
   CHECK(info->flags == 0 && info->semaphore);
   imported_type = info->handleType;
   imported_handle = info->handle;
   imported_name = info->name;
   return import_result;
}

static VKAPI_ATTR void VKAPI_CALL
destroy_semaphore(VkDevice device, VkSemaphore semaphore, const VkAllocationCallbacks *alloc)
{
   CHECK(semaphore);
   destroys++;
}

static void
flush(struct pipe_context *pipe, struct pipe_fence_handle **fence, unsigned flags)
{
   flushes++;
   CHECK(!fence);
}

static void
release_batch(struct zink_screen *screen, struct zink_batch_state *batch)
{
   util_dynarray_foreach(&batch->fences, struct zink_tc_fence *, fence)
      zink_fence_reference(screen, fence, NULL);
   util_dynarray_fini(&batch->fences);
   util_dynarray_fini(&batch->wait_semaphores);
   util_dynarray_fini(&batch->wait_semaphore_stages);
   util_dynarray_fini(&batch->wait_semaphore_values);
   util_dynarray_fini(&batch->user_signal_semaphores);
   util_dynarray_fini(&batch->user_signal_semaphore_values);
}

static struct zink_batch_state *
submission_batch(struct zink_context *ctx)
{
   struct zink_batch_state *batch = calloc(1, sizeof(*batch));
   CHECK(batch);
   batch->ctx = ctx;
   batch->cmdbuf = (VkCommandBuffer)(uintptr_t)1;
   batch->reordered_cmdbuf = (VkCommandBuffer)(uintptr_t)2;
   batch->unsynchronized_cmdbuf = (VkCommandBuffer)(uintptr_t)3;
   batch->has_work = batch->has_reordered_work = batch->has_unsync = true;
   _mesa_set_init(&batch->dmabuf_exports, NULL, _mesa_hash_pointer, _mesa_key_pointer_equal);
   cnd_init(&batch->usage.flush);
   util_queue_fence_init(&batch->flush_completed);
   ctx->bs = batch;
   return batch;
}

static void
test_submission_bridge(struct zink_context *ctx, struct zink_screen *screen)
{
   struct pipe_fence_handle *wait = NULL, *signal = NULL;
   import_result = VK_SUCCESS;
   zink_create_fence_win32(&screen->base, &wait, (HANDLE)(uintptr_t)1, NULL,
                           PIPE_FD_TYPE_TIMELINE_SEMAPHORE_D3D12);
   zink_create_fence_win32(&screen->base, &signal, (HANDLE)(uintptr_t)2, NULL,
                           PIPE_FD_TYPE_TIMELINE_SEMAPHORE_D3D12);
   CHECK(wait && signal);
   bridge_wait = zink_tc_fence(wait)->sem;
   bridge_signal = zink_tc_fence(signal)->sem;
   struct pipe_submission_sync sync = {
      .wait_fence = wait, .signal_fence = signal, .initial_value = 30,
      .data = &prepared_value,
      .prepare = prepare_submission, .submitted = complete_submission,
   };
   callback_thread = GetCurrentThreadId();
   ctx->tc = (struct threaded_context *)(uintptr_t)1;
   CHECK(!zink_set_submission_sync(&ctx->base, &sync));
   ctx->tc = NULL;
   CHECK(zink_set_submission_sync(&ctx->base, &sync));
   screen->base.fence_reference(&screen->base, &wait, NULL);
   screen->base.fence_reference(&screen->base, &signal, NULL);
   ctx->queries_disabled = true;
   screen->sem = (VkSemaphore)(uintptr_t)0x9000;
   screen->vk.EndCommandBuffer = end_command_buffer;
   screen->vk.CmdPipelineBarrier = pipeline_barrier;
   screen->vk.QueueSubmit = queue_submit;
   screen->vk.QueueWaitIdle = queue_wait_idle;
   simple_mtx_t queue_lock;
   simple_mtx_init(&queue_lock, mtx_plain);
   screen->queue_lock = &queue_lock;
   simple_mtx_init(&screen->active_batch_states_lock, mtx_plain);
   simple_mtx_init(&screen->free_batch_states_lock, mtx_plain);

   /* Invoke the actual batch submission path used by internal flushes. Its
    * wait must cover unsynchronized, reordered and normal command buffers. */
   struct zink_batch_state *batches[6];
   batches[0] = submission_batch(ctx);
   zink_end_batch(ctx);
   CHECK(prepares == 1 && completions == 1 && submissions == 1);
   CHECK(prepared_value == 31 && submitted_success && !screen->device_lost);

   CHECK(util_queue_init(&screen->flush_queue, "test", 4, 1, 0, screen));
   screen->threaded_submit = expect_submit_worker = true;
   batches[1] = submission_batch(ctx);
   zink_end_batch(ctx);
   CHECK(prepares == 2 && completions == 2 && submissions == 2);
   CHECK(prepared_value == 32 && submitted_success && !screen->device_lost && !idle_waits);

   /* Submission succeeded, but the runtime rejected its dependent wait.
    * Drain accepted work before teardown can free runtime-owned mappings. */
   completion_ok = false;
   batches[2] = submission_batch(ctx);
   zink_end_batch(ctx);
   CHECK(prepares == 3 && completions == 3 && submissions == 3);
   CHECK(submitted_success && screen->device_lost && idle_waits == 1);

   screen->device_lost = false;
   completion_ok = true;
   submit_result = VK_ERROR_DEVICE_LOST;
   batches[3] = submission_batch(ctx);
   zink_end_batch(ctx);
   CHECK(prepares == 4 && completions == 4 && submissions == 4);
   CHECK(!submitted_success && screen->device_lost && idle_waits == 1);
   util_queue_destroy(&screen->flush_queue);
   screen->threaded_submit = expect_submit_worker = false;

   /* Independent prepare-failure injection: never submit an unwaitable
    * batch and never queue a runtime wait for an unsubmitted signal. */
   screen->device_lost = false;
   prepare_ok = false;
   batches[4] = submission_batch(ctx);
   zink_end_batch(ctx);
   CHECK(prepares == 5 && completions == 4 && submissions == 4);
   CHECK(screen->device_lost && !batches[4]->fences.size && idle_waits == 2);
   ctx->submission_sync_value = UINT64_MAX;
   CHECK(!zink_submission_sync_prepare(ctx) && prepares == 5);

   unsigned before = destroys;
   CHECK(zink_set_submission_sync(&ctx->base, NULL));
   CHECK(destroys == before); /* batches retain both imported payloads */
   /* Ordinary Zink contexts still signal their internal completion when
    * the user-signal submit is empty and removed from the submit array. */
   screen->device_lost = false;
   submit_result = VK_SUCCESS;
   bridge_active = false;
   batches[5] = submission_batch(ctx);
   zink_end_batch(ctx);
   CHECK(submissions == 5 && prepares == 5 && completions == 4);
   for (unsigned i = 0; i < ARRAY_SIZE(batches); ++i) {
      release_batch(screen, batches[i]);
      cnd_destroy(&batches[i]->usage.flush);
      util_queue_fence_destroy(&batches[i]->flush_completed);
      _mesa_set_fini(&batches[i]->dmabuf_exports, NULL);
      free(batches[i]);
   }
   CHECK(destroys == before + 2);
   simple_mtx_destroy(&screen->free_batch_states_lock);
   simple_mtx_destroy(&screen->active_batch_states_lock);
   simple_mtx_destroy(&queue_lock);
}

int main(void)
{
   struct zink_screen *screen = calloc(1, sizeof(*screen));
   struct zink_context *ctx = calloc(1, sizeof(*ctx));
   struct zink_batch_state *first = calloc(1, sizeof(*first));
   struct zink_batch_state *second = calloc(1, sizeof(*second));
   CHECK(screen && ctx && first && second);
   screen->vk.CreateSemaphore = create_semaphore;
   screen->vk.ImportSemaphoreWin32HandleKHR = import_semaphore;
   screen->vk.DestroySemaphore = destroy_semaphore;
   zink_screen_fence_init(&screen->base);
   struct pipe_fence_handle *fence = NULL;
   HANDLE handle = (HANDLE)(uintptr_t)0x1234;
   zink_create_fence_win32(&screen->base, &fence, handle, NULL,
                           PIPE_FD_TYPE_TIMELINE_SEMAPHORE_D3D12);
   CHECK(!fence && !creates && !imports);
   screen->info.have_KHR_external_semaphore_win32 = true;
   zink_create_fence_win32(&screen->base, &fence, handle, NULL,
                           PIPE_FD_TYPE_TIMELINE_SEMAPHORE_D3D12);
   CHECK(fence && created_type == VK_SEMAPHORE_TYPE_TIMELINE);
   CHECK(imported_type == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT);
   CHECK(imported_handle == handle && !imported_name);

   ctx->base.screen = &screen->base;
   ctx->base.flush = flush;
   ctx->bs = first;
   zink_fence_server_sync(&ctx->base, fence, 3);
   zink_fence_server_sync(&ctx->base, fence, 9);
   zink_fence_server_sync(&ctx->base, fence, 5);
   CHECK(util_dynarray_num_elements(&first->wait_semaphores, VkSemaphore) == 1);
   CHECK(*(uint64_t *)first->wait_semaphore_values.data == 9);
   /* The old batch has not submitted yet. Its deferred-context marker must
    * not discard the next batch's timeline wait. */
   ctx->bs = second;
   zink_fence_server_sync(&ctx->base, fence, 11);
   CHECK(util_dynarray_num_elements(&second->wait_semaphores, VkSemaphore) == 1);
   CHECK(*(uint64_t *)second->wait_semaphore_values.data == 11);
   zink_fence_server_signal(&ctx->base, fence, 12);
   CHECK(flushes == 1 && second->has_work);
   CHECK(*(uint64_t *)second->user_signal_semaphore_values.data == 12);
   screen->base.fence_reference(&screen->base, &fence, NULL);
   CHECK(destroys == 0);
   release_batch(screen, first);
   CHECK(destroys == 0);
   release_batch(screen, second);
   CHECK(destroys == 1);

   zink_create_fence_win32(&screen->base, &fence, NULL, L"named-fence",
                           PIPE_FD_TYPE_TIMELINE_SEMAPHORE_VK);
   CHECK(fence && created_type == VK_SEMAPHORE_TYPE_TIMELINE);
   CHECK(imported_type == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT);
   CHECK(!imported_handle && !wcscmp(imported_name, L"named-fence"));
   unsigned before = destroys;
   zink_create_fence_win32(&screen->base, &fence, handle, NULL, PIPE_FD_TYPE_SYNCOBJ);
   CHECK(fence && destroys == before + 1 && created_type == VK_SEMAPHORE_TYPE_BINARY);
   CHECK(imported_type == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT);
   screen->base.fence_reference(&screen->base, &fence, NULL);

   before = imports;
   create_result = VK_ERROR_OUT_OF_HOST_MEMORY;
   zink_create_fence_win32(&screen->base, &fence, handle, NULL,
                           PIPE_FD_TYPE_TIMELINE_SEMAPHORE_D3D12);
   CHECK(!fence && imports == before);
   create_result = VK_SUCCESS;
   import_result = VK_ERROR_INVALID_EXTERNAL_HANDLE;
   before = destroys;
   zink_create_fence_win32(&screen->base, &fence, handle, NULL,
                           PIPE_FD_TYPE_TIMELINE_SEMAPHORE_D3D12);
   CHECK(!fence && destroys == before + 1);

   test_submission_bridge(ctx, screen);
   test_query_polling();
   free(second);
   free(first);
   free(ctx);
   free(screen);
   puts("PASS: actual Zink Win32 imports and per-batch timeline bridge, worker/callback ordering and failure/lifetime controls; Vulkan calls mocked, no GPU execution");
   puts("PASS: actual Zink event-query no-flush polling before submission, pending and completed; fence completion mocked");
   return 0;
}
