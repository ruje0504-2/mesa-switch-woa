/* SPDX-License-Identifier: MIT
 *
 * Queries. Results become available when the queue fence passes the
 * submission that recorded the query; polling them never blocks.
 */
#include "tgd3d.h"

#include <string.h>

namespace tgd3d {

static inline Query *Q(D3D10DDI_HQUERY h) { return static_cast<Query *>(h.pDrvPrivate); }

static bool pass_query(const Query *q)
{
   return q->type == D3D10DDI_QUERY_OCCLUSION || q->type == D3D10DDI_QUERY_OCCLUSIONPREDICATE ||
          q->type == D3D10DDI_QUERY_PIPELINESTATS;
}

static const VkQueryPipelineStatisticFlags statistics =
   VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT |
   VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT |
   VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
   VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_INVOCATIONS_BIT |
   VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_PRIMITIVES_BIT |
   VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT |
   VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT |
   VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;

static bool pool_create(Device *dev, Query *q)
{
   VkQueryPoolCreateInfo info = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
   if (q->type == D3D10DDI_QUERY_TIMESTAMP) {
      info.queryType = VK_QUERY_TYPE_TIMESTAMP;
      info.queryCount = 1;
   } else if (q->type == D3D10DDI_QUERY_PIPELINESTATS) {
      info.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
      info.queryCount = QUERY_SLOTS;
      info.pipelineStatistics = statistics;
   } else {
      info.queryType = VK_QUERY_TYPE_OCCLUSION;
      info.queryCount = QUERY_SLOTS;
   }
   if (dev->vk.CreateQueryPool(dev->vkdev, &info, nullptr, &q->pool) != VK_SUCCESS)
      return false;
   dev->vk.ResetQueryPool(dev->vkdev, q->pool, 0, info.queryCount);
   return true;
}

void query_pass_begin(Device *dev, VkCommandBuffer cmd)
{
   for (Query *q : dev->activeQueries) {
      if (FAILED(q->error))
         continue;
      if (q->used == QUERY_SLOTS) {
         VkQueryPool previous = q->pool;
         if (!pool_create(dev, q)) {
            q->pool = previous;
            q->error = E_OUTOFMEMORY;
            TG_ERROR(dev, q->error);
            continue;
         }
         q->fullPools.push_back(previous);
         q->used = 0;
      }
      const VkQueryControlFlags flags = q->type == D3D10DDI_QUERY_OCCLUSION &&
         dev->caps->occlusionQueryPrecise ? VK_QUERY_CONTROL_PRECISE_BIT : 0;
      dev->vk.CmdBeginQuery(cmd, q->pool, q->used, flags);
      q->inPass = true;
   }
}

void query_pass_end(Device *dev, VkCommandBuffer cmd)
{
   for (Query *q : dev->activeQueries) {
      if (!q->inPass)
         continue;
      dev->vk.CmdEndQuery(cmd, q->pool, q->used++);
      q->inPass = false;
   }
}

static SIZE_T APIENTRY CalcPrivateQuerySize(D3D10DDI_HDEVICE, const D3D10DDIARG_CREATEQUERY *)
{
   return sizeof(Query);
}

static void APIENTRY CreateQuery(D3D10DDI_HDEVICE hDevice, const D3D10DDIARG_CREATEQUERY *args,
                                 D3D10DDI_HQUERY handle, D3D10DDI_HRTQUERY)
{
   Device *dev = D(hDevice);
   Query *q = new (handle.pDrvPrivate) Query();
   q->dev = dev;
   q->type = args->Query;
   q->misc = args->MiscFlags;
   q->state = QueryState::Idle;
   q->pool = VK_NULL_HANDLE;
   q->seq = 0;
   q->used = 0;
   q->inPass = false;
   HRESULT hr = S_OK;
   switch (q->type) {
   case D3D10DDI_QUERY_EVENT:
   case D3D10DDI_QUERY_TIMESTAMPDISJOINT:
      break;
   case D3D10DDI_QUERY_TIMESTAMP:
      if (!dev->caps->timestampValidBits)
         hr = E_NOTIMPL;
      else if (!pool_create(dev, q))
         hr = E_OUTOFMEMORY;
      break;
   case D3D10DDI_QUERY_PIPELINESTATS:
      if (!dev->caps->pipelineStatistics) {
         hr = E_NOTIMPL;
         break;
      }
      /* fallthrough */
   case D3D10DDI_QUERY_OCCLUSION:
   case D3D10DDI_QUERY_OCCLUSIONPREDICATE:
      if (!pool_create(dev, q))
         hr = E_OUTOFMEMORY;
      break;
   default:
      /* Stream-output statistics and performance counters are not offered. */
      hr = E_NOTIMPL;
      break;
   }
   if (FAILED(hr)) {
      q->error = hr;
      TG_ERROR(dev, hr);
   }
}

static void query_deactivate(Device *dev, Query *q)
{
   for (size_t i = 0; i < dev->activeQueries.size(); i++) {
      if (dev->activeQueries[i] == q) {
         end_rendering(dev);     /* closes the slot opened for the current pass */
         dev->activeQueries.erase(dev->activeQueries.begin() + i);
         return;
      }
   }
}

static void APIENTRY DestroyQuery(D3D10DDI_HDEVICE hDevice, D3D10DDI_HQUERY handle)
{
   Device *dev = D(hDevice);
   Query *q = Q(handle);
   query_deactivate(dev, q);
   defer(dev, DeferredType::QueryPool, (uint64_t)q->pool, dev->lastSeq());
   for (VkQueryPool pool : q->fullPools)
      defer(dev, DeferredType::QueryPool, (uint64_t)pool, dev->lastSeq());
   q->~Query();
}

static void APIENTRY QueryBegin(D3D10DDI_HDEVICE hDevice, D3D10DDI_HQUERY handle)
{
   Device *dev = D(hDevice);
   Query *q = Q(handle);
   if (!pass_query(q)) {
      q->state = QueryState::Active;
      return;
   }
   query_deactivate(dev, q);
   for (VkQueryPool pool : q->fullPools)
      defer(dev, DeferredType::QueryPool, (uint64_t)pool, dev->lastSeq());
   q->fullPools.clear();
   q->error = S_OK;
   if (q->used) {
      /* Earlier slots may still be written by queued commands. */
      defer(dev, DeferredType::QueryPool, (uint64_t)q->pool, dev->lastSeq());
      q->pool = VK_NULL_HANDLE;
      if (!pool_create(dev, q)) {
         q->error = E_OUTOFMEMORY;
         TG_ERROR(dev, E_OUTOFMEMORY);
         return;
      }
   }
   q->used = 0;
   q->state = QueryState::Active;
   end_rendering(dev);            /* the next pass opens a slot for this query */
   dev->activeQueries.push_back(q);
}

static void APIENTRY QueryEnd(D3D10DDI_HDEVICE hDevice, D3D10DDI_HQUERY handle)
{
   Device *dev = D(hDevice);
   Query *q = Q(handle);
   /* Deferred clears precede this event/timestamp in the D3D command stream. */
   flush_pending_clears(dev, nullptr);
   if (pass_query(q)) {
      query_deactivate(dev, q);
   } else if (q->type == D3D10DDI_QUERY_TIMESTAMP && q->pool) {
      if (q->seq) {
         defer(dev, DeferredType::QueryPool, (uint64_t)q->pool, dev->lastSeq());
         q->pool = VK_NULL_HANDLE;
         if (!pool_create(dev, q)) {
            q->error = E_OUTOFMEMORY;
            TG_ERROR(dev, E_OUTOFMEMORY);
            return;
         }
      }
      VkCommandBuffer cmd = cmd_begin(dev);
      if (!cmd)
         return;
      dev->vk.CmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, q->pool, 0);
   }
   /* Ready once everything recorded so far has executed. */
   q->seq = dev->cmd && dev->cmdHasWork ? dev->recordingSeq() : dev->submitted;
   q->state = QueryState::Ended;
}

static void APIENTRY QueryGetData(D3D10DDI_HDEVICE hDevice, D3D10DDI_HQUERY handle, VOID *data,
                                  UINT size, UINT flags)
{
   Device *dev = D(hDevice);
   Query *q = Q(handle);
   if (FAILED(dev->lost) || FAILED(q->error)) {
      TG_ERROR(dev, FAILED(dev->lost) ? dev->lost : q->error);
      return;
   }
   if (q->seq > dev->submitted && !(flags & D3D10_DDI_GET_DATA_DO_NOT_FLUSH)) {
      HRESULT hr = flush(dev, false);
      if (FAILED(hr)) { TG_ERROR(dev, hr); return; }
   }
   if (q->state != QueryState::Ended || !sequence_done(dev, q->seq)) {
      /* Not an error: the runtime turns this into S_FALSE for the caller. */
      if (dev->um.pfnSetErrorCb)
         dev->um.pfnSetErrorCb(dev->hRTCoreLayer, DXGI_DDI_ERR_WASSTILLDRAWING);
      return;
   }
   if (!data)
      return;
   switch (q->type) {
   case D3D10DDI_QUERY_TIMESTAMPDISJOINT: {
      D3D10_DDI_QUERY_DATA_TIMESTAMP_DISJOINT disjoint = {UINT64_C(1000000000), FALSE};
      if (size >= sizeof(disjoint))
         memcpy(data, &disjoint, sizeof(disjoint));
      break;
   }
   case D3D10DDI_QUERY_EVENT:
      if (size >= sizeof(BOOL))
         *static_cast<BOOL *>(data) = TRUE;
      break;
   case D3D10DDI_QUERY_TIMESTAMP: {
      uint64_t ticks = 0;
      VkResult result = dev->vk.GetQueryPoolResults(dev->vkdev, q->pool, 0, 1, sizeof(ticks),
                                                    &ticks, sizeof(ticks), VK_QUERY_RESULT_64_BIT);
      if (result != VK_SUCCESS) {
         TG_ERROR(dev, result == VK_NOT_READY ? DXGI_DDI_ERR_WASSTILLDRAWING : DXGI_ERROR_DEVICE_REMOVED);
         return;
      }
      const uint64_t ns = (uint64_t)((double)ticks * dev->caps->timestampPeriod);
      if (size >= sizeof(ns))
         memcpy(data, &ns, sizeof(ns));
      break;
   }
   case D3D10DDI_QUERY_OCCLUSION:
   case D3D10DDI_QUERY_OCCLUSIONPREDICATE: {
      uint64_t total = 0;
      for (size_t p = 0; p <= q->fullPools.size(); p++) {
         const bool full = p < q->fullPools.size();
         std::vector<uint64_t> slots(full ? QUERY_SLOTS : q->used);
         if (slots.empty()) continue;
         VkResult result = dev->vk.GetQueryPoolResults(dev->vkdev, full ? q->fullPools[p] : q->pool,
            0, (uint32_t)slots.size(), slots.size() * 8, slots.data(), 8, VK_QUERY_RESULT_64_BIT);
         if (result != VK_SUCCESS) {
            TG_ERROR(dev, result == VK_NOT_READY ? DXGI_DDI_ERR_WASSTILLDRAWING : DXGI_ERROR_DEVICE_REMOVED);
            return;
         }
         for (uint64_t v : slots) total += v;
      }
      if (q->type == D3D10DDI_QUERY_OCCLUSION) {
         if (size >= sizeof(total))
            memcpy(data, &total, sizeof(total));
      } else if (size >= sizeof(BOOL)) {
         *static_cast<BOOL *>(data) = total != 0;
      }
      break;
   }
   case D3D10DDI_QUERY_PIPELINESTATS: {
      D3D10_DDI_QUERY_DATA_PIPELINE_STATISTICS out = {};
      for (size_t p = 0; p <= q->fullPools.size(); p++) {
         const bool full = p < q->fullPools.size();
         const uint32_t count = full ? QUERY_SLOTS : q->used;
         if (!count) continue;
         std::vector<uint64_t> slots((size_t)count * 8);
         VkResult result = dev->vk.GetQueryPoolResults(dev->vkdev, full ? q->fullPools[p] : q->pool,
            0, count, slots.size() * 8, slots.data(), 64, VK_QUERY_RESULT_64_BIT);
         if (result != VK_SUCCESS) {
            TG_ERROR(dev, result == VK_NOT_READY ? DXGI_DDI_ERR_WASSTILLDRAWING : DXGI_ERROR_DEVICE_REMOVED);
            return;
         }
         for (uint32_t i = 0; i < count; i++) {
            /* Vulkan returns the enabled statistics in bit order. */
            const uint64_t *v = &slots[(size_t)i * 8];
            out.IAVertices += v[0];
            out.IAPrimitives += v[1];
            out.VSInvocations += v[2];
            out.GSInvocations += v[3];
            out.GSPrimitives += v[4];
            out.CInvocations += v[5];
            out.CPrimitives += v[6];
            out.PSInvocations += v[7];
         }
      }
      if (size >= sizeof(out))
         memcpy(data, &out, sizeof(out));
      break;
   }
   default:
      break;
   }
}

static void APIENTRY CheckCounterInfo(D3D10DDI_HDEVICE, D3D10DDI_COUNTER_INFO *info)
{
   memset(info, 0, sizeof(*info));     /* no device-dependent counters */
}

static void APIENTRY CheckCounter(D3D10DDI_HDEVICE hDevice, D3D10DDI_QUERY, D3D10DDI_COUNTER_TYPE *,
                                  UINT *, LPSTR, UINT *, LPSTR, UINT *, LPSTR, UINT *)
{
   TG_ERROR(D(hDevice), DXGI_ERROR_UNSUPPORTED);
}

void query_fill_table(D3D10DDI_DEVICEFUNCS *t)
{
   t->pfnCalcPrivateQuerySize = CalcPrivateQuerySize;
   t->pfnCreateQuery = CreateQuery;
   t->pfnDestroyQuery = DestroyQuery;
   t->pfnQueryBegin = QueryBegin;
   t->pfnQueryEnd = QueryEnd;
   t->pfnQueryGetData = QueryGetData;
   t->pfnCheckCounterInfo = CheckCounterInfo;
   t->pfnCheckCounter = CheckCounter;
}

}
