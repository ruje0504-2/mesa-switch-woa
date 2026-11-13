/* SPDX-License-Identifier: MIT
 *
 * Host-visible buffer storage. D3D buffers and CPU-side texel uploads are
 * slices of a few large Vulkan buffers. A discard map or a full update takes
 * a fresh slice instead of waiting for the GPU or copying on it: draws that
 * were already recorded keep reading the old slice, which returns to its
 * chunk once the queue fence passes its last use.
 */
#include "tgd3d.h"

namespace tgd3d {

enum : VkDeviceSize {
   CHUNK_SIZE = 1024 * 1024,
   DEDICATED_THRESHOLD = CHUNK_SIZE / 4,
   MAX_IDLE_CHUNKS = 4,
};

static HRESULT chunk_create(Device *dev, VkDeviceSize size, bool dedicated, Chunk **out)
{
   *out = nullptr;
   Chunk *chunk = new (std::nothrow) Chunk();
   if (!chunk)
      return E_OUTOFMEMORY;
   VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
   info.size = size;
   info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
   info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
   HRESULT hr = resource_result(dev->vk.CreateBuffer(dev->vkdev, &info, nullptr, &chunk->buffer));
   if (FAILED(hr)) {
      delete chunk;
      return hr;
   }
   VkMemoryRequirements req;
   dev->vk.GetBufferMemoryRequirements(dev->vkdev, chunk->buffer, &req);
   VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
   alloc.allocationSize = req.size;
   alloc.memoryTypeIndex = dev->adapter->vk.findMemoryType(
      req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   void *cpu = nullptr;
   hr = alloc.memoryTypeIndex == UINT32_MAX ? DXGI_ERROR_UNSUPPORTED :
      resource_result(dev->vk.AllocateMemory(dev->vkdev, &alloc, nullptr, &chunk->memory));
   if (SUCCEEDED(hr))
      hr = resource_result(dev->vk.BindBufferMemory(dev->vkdev, chunk->buffer, chunk->memory, 0));
   if (SUCCEEDED(hr))
      hr = resource_result(dev->vk.MapMemory(dev->vkdev, chunk->memory, 0, VK_WHOLE_SIZE, 0, &cpu));
   if (FAILED(hr)) {
      dev->vk.DestroyBuffer(dev->vkdev, chunk->buffer, nullptr);
      if (chunk->memory)
         dev->vk.FreeMemory(dev->vkdev, chunk->memory, nullptr);
      delete chunk;
      return hr;
   }
   chunk->cpu = static_cast<uint8_t *>(cpu);
   chunk->size = size;
   chunk->dedicated = dedicated;
   dev->chunks.push_back(chunk);
   *out = chunk;
   return S_OK;
}

static void chunk_destroy(Device *dev, Chunk *chunk)
{
   for (size_t i = 0; i < dev->chunks.size(); i++) {
      if (dev->chunks[i] == chunk) {
         dev->chunks.erase(dev->chunks.begin() + i);
         break;
      }
   }
   if (dev->currentChunk == chunk)
      dev->currentChunk = nullptr;
   dev->vk.DestroyBuffer(dev->vkdev, chunk->buffer, nullptr);
   dev->vk.FreeMemory(dev->vkdev, chunk->memory, nullptr);
   delete chunk;
}

HRESULT slice_alloc(Device *dev, VkDeviceSize size, VkDeviceSize alignment, Slice *out)
{
   *out = {};
   if (FAILED(dev->lost))
      return dev->lost;
   if (!size)
      size = 1;
   if (alignment < 16)
      alignment = 16;
   if (size >= DEDICATED_THRESHOLD) {
      Chunk *chunk = nullptr;
      HRESULT hr = chunk_create(dev, (size + 4095) & ~VkDeviceSize(4095), true, &chunk);
      if (FAILED(hr))
         return hr;
      chunk->live = 1;
      chunk->used = size;
      out->chunk = chunk;
      out->size = size;
      return S_OK;
   }
   for (int attempt = 0; attempt < 2; attempt++) {
      Chunk *chunk = dev->currentChunk;
      if (chunk) {
         const VkDeviceSize offset = (chunk->used + alignment - 1) & ~(alignment - 1);
         if (offset + size <= chunk->size) {
            chunk->used = offset + size;
            chunk->live++;
            out->chunk = chunk;
            out->offset = offset;
            out->size = size;
            return S_OK;
         }
      }
      /* Prefer a chunk every slice of which has been returned. */
      dev->currentChunk = nullptr;
      for (Chunk *idle : dev->chunks) {
         if (!idle->dedicated && !idle->live && idle != chunk) {
            idle->used = 0;
            dev->currentChunk = idle;
            break;
         }
      }
      if (!dev->currentChunk) {
         HRESULT hr = chunk_create(dev, CHUNK_SIZE, false, &dev->currentChunk);
         if (FAILED(hr))
            return hr;
      }
   }
   return E_OUTOFMEMORY;
}

void slice_release(Device *dev, Slice slice, uint64_t lastUse)
{
   if (!slice)
      return;
   if (lastUse && !sequence_done(dev, lastUse)) {
      dev->deferredSlices.push_back({slice, lastUse});
      return;
   }
   Chunk *chunk = slice.chunk;
   if (--chunk->live)
      return;
   if (chunk->dedicated) {
      chunk_destroy(dev, chunk);
      return;
   }
   if (chunk == dev->currentChunk) {
      chunk->used = 0;
      return;
   }
   size_t idle = 0;
   for (Chunk *other : dev->chunks)
      idle += !other->dedicated && !other->live;
   if (idle > MAX_IDLE_CHUNKS)
      chunk_destroy(dev, chunk);
}

void upload_destroy(Device *dev)
{
   dev->deferredSlices.clear();
   while (!dev->chunks.empty())
      chunk_destroy(dev, dev->chunks.back());
}

}
