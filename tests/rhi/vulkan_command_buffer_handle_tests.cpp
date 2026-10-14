#include "../../rhi/vulkan/VulkanCommandBuffer.h"
#include "../../rhi/vulkan/VulkanCommandAllocator.h"
#include "../../rhi/vulkan/VulkanQueue.h"
#include "../../rhi/vulkan/VulkanResourceTable.h"

#include "volk.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

template <typename NativeHandle>
NativeHandle fakeNativeHandle(uintptr_t value)
{
  if constexpr(std::is_pointer_v<NativeHandle>)
    return reinterpret_cast<NativeHandle>(value);
  else
    return static_cast<NativeHandle>(value);
}

struct RecordingProbe
{
  uint32_t callbacks{0};
  uint32_t queueSubmits{0};
  uint32_t commandBufferFrees{0};
  uint32_t dynamicOffsetCount{0};
  uint32_t firstDynamicOffset{0};
  VkBuffer firstBuffer{VK_NULL_HANDLE};
  VkBuffer secondBuffer{VK_NULL_HANDLE};
  VkImage firstImage{VK_NULL_HANDLE};
  VkImage secondImage{VK_NULL_HANDLE};
  VkImageView firstView{VK_NULL_HANDLE};
  VkPipeline pipeline{VK_NULL_HANDLE};
  VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
  VkDescriptorSet descriptorSet{VK_NULL_HANDLE};
  VkQueryPool queryPool{VK_NULL_HANDLE};
};

RecordingProbe g_probe;

void record(VkBuffer first, VkBuffer second = VK_NULL_HANDLE)
{
  ++g_probe.callbacks;
  g_probe.firstBuffer = first;
  g_probe.secondBuffer = second;
}

VKAPI_ATTR void VKAPI_CALL recordDrawIndexedIndirect(
  VkCommandBuffer, VkBuffer buffer, VkDeviceSize, uint32_t, uint32_t)
{
  record(buffer);
}

VKAPI_ATTR void VKAPI_CALL recordDrawIndexedIndirectCount(
  VkCommandBuffer, VkBuffer args, VkDeviceSize, VkBuffer count,
  VkDeviceSize, uint32_t, uint32_t)
{
  record(args, count);
}

VKAPI_ATTR void VKAPI_CALL recordDrawIndirect(
  VkCommandBuffer, VkBuffer buffer, VkDeviceSize, uint32_t, uint32_t)
{
  record(buffer);
}

VKAPI_ATTR void VKAPI_CALL recordDrawMeshTasksIndirect(
  VkCommandBuffer, VkBuffer buffer, VkDeviceSize, uint32_t, uint32_t)
{
  record(buffer);
}

VKAPI_ATTR void VKAPI_CALL recordDispatchIndirect(
  VkCommandBuffer, VkBuffer buffer, VkDeviceSize)
{
  record(buffer);
}

VKAPI_ATTR void VKAPI_CALL recordCopyBuffer(
  VkCommandBuffer, VkBuffer source, VkBuffer destination, uint32_t,
  const VkBufferCopy*)
{
  record(source, destination);
}

VKAPI_ATTR void VKAPI_CALL recordCopyBufferToImage(
  VkCommandBuffer, VkBuffer source, VkImage destination, VkImageLayout, uint32_t,
  const VkBufferImageCopy*)
{
  record(source);
  g_probe.firstImage = destination;
}

VKAPI_ATTR void VKAPI_CALL recordCopyImageToBuffer(
  VkCommandBuffer, VkImage source, VkImageLayout, VkBuffer destination, uint32_t,
  const VkBufferImageCopy*)
{
  record(destination);
  g_probe.firstImage = source;
}

VKAPI_ATTR void VKAPI_CALL recordBlitImage(
  VkCommandBuffer, VkImage source, VkImageLayout, VkImage destination,
  VkImageLayout, uint32_t, const VkImageBlit*, VkFilter)
{
  ++g_probe.callbacks;
  g_probe.firstImage = source;
  g_probe.secondImage = destination;
}

VKAPI_ATTR void VKAPI_CALL recordFillBuffer(
  VkCommandBuffer, VkBuffer buffer, VkDeviceSize, VkDeviceSize, uint32_t)
{
  record(buffer);
}

VKAPI_ATTR void VKAPI_CALL recordBindPipeline(
  VkCommandBuffer, VkPipelineBindPoint, VkPipeline pipeline)
{
  ++g_probe.callbacks;
  g_probe.pipeline = pipeline;
}

VKAPI_ATTR void VKAPI_CALL recordBindDescriptorSets(
  VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout layout, uint32_t, uint32_t,
  const VkDescriptorSet* sets, uint32_t dynamicOffsetCount,
  const uint32_t* dynamicOffsets)
{
  ++g_probe.callbacks;
  g_probe.pipelineLayout = layout;
  g_probe.descriptorSet = sets != nullptr ? sets[0] : VK_NULL_HANDLE;
  g_probe.dynamicOffsetCount = dynamicOffsetCount;
  g_probe.firstDynamicOffset =
    dynamicOffsetCount != 0 && dynamicOffsets != nullptr ? dynamicOffsets[0] : 0;
}

VKAPI_ATTR void VKAPI_CALL recordPushConstants(
  VkCommandBuffer, VkPipelineLayout layout, VkShaderStageFlags, uint32_t,
  uint32_t, const void*)
{
  ++g_probe.callbacks;
  g_probe.pipelineLayout = layout;
}

VKAPI_ATTR VkResult VKAPI_CALL recordCreateSemaphore(
  VkDevice, const VkSemaphoreCreateInfo*, const VkAllocationCallbacks*,
  VkSemaphore* semaphore)
{
  *semaphore = fakeNativeHandle<VkSemaphore>(0xF001);
  return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL recordDestroySemaphore(
  VkDevice, VkSemaphore, const VkAllocationCallbacks*)
{
}

VKAPI_ATTR VkResult VKAPI_CALL recordCreateCommandPool(
  VkDevice, const VkCommandPoolCreateInfo*, const VkAllocationCallbacks*,
  VkCommandPool* pool)
{
  *pool = fakeNativeHandle<VkCommandPool>(0xF002);
  return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL recordDestroyCommandPool(
  VkDevice, VkCommandPool, const VkAllocationCallbacks*)
{
}

VKAPI_ATTR VkResult VKAPI_CALL recordAllocateCommandBuffers(
  VkDevice, const VkCommandBufferAllocateInfo* info, VkCommandBuffer* buffers)
{
  for(uint32_t index = 0; index < info->commandBufferCount; ++index)
    buffers[index] = fakeNativeHandle<VkCommandBuffer>(0xF100 + index);
  return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL recordFreeCommandBuffers(
  VkDevice, VkCommandPool, uint32_t, const VkCommandBuffer*)
{
  ++g_probe.commandBufferFrees;
}

VKAPI_ATTR VkResult VKAPI_CALL recordBeginCommandBuffer(
  VkCommandBuffer, const VkCommandBufferBeginInfo*)
{
  return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL recordEndCommandBuffer(VkCommandBuffer)
{
  return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL recordResetCommandPool(
  VkDevice, VkCommandPool, VkCommandPoolResetFlags)
{
  return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL recordQueueSubmit2(
  VkQueue, uint32_t, const VkSubmitInfo2*, VkFence)
{
  ++g_probe.queueSubmits;
  return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL recordBeginRendering(
  VkCommandBuffer, const VkRenderingInfo* renderingInfo)
{
  ++g_probe.callbacks;
  if(renderingInfo != nullptr && renderingInfo->colorAttachmentCount != 0 &&
     renderingInfo->pColorAttachments != nullptr)
    g_probe.firstView = renderingInfo->pColorAttachments[0].imageView;
}

VKAPI_ATTR void VKAPI_CALL recordEndRendering(VkCommandBuffer)
{
}

VKAPI_ATTR void VKAPI_CALL recordPipelineBarrier2(
  VkCommandBuffer, const VkDependencyInfo* dependencyInfo)
{
  ++g_probe.callbacks;
  if(dependencyInfo != nullptr && dependencyInfo->imageMemoryBarrierCount != 0 &&
     dependencyInfo->pImageMemoryBarriers != nullptr)
    g_probe.firstImage = dependencyInfo->pImageMemoryBarriers[0].image;
}

VKAPI_ATTR void VKAPI_CALL recordClearColorImage(
  VkCommandBuffer, VkImage image, VkImageLayout, const VkClearColorValue*,
  uint32_t, const VkImageSubresourceRange*)
{
  ++g_probe.callbacks;
  g_probe.firstImage = image;
}

VKAPI_ATTR void VKAPI_CALL recordResetQueryPool(
  VkCommandBuffer, VkQueryPool pool, uint32_t, uint32_t)
{
  ++g_probe.callbacks;
  g_probe.queryPool = pool;
}

VKAPI_ATTR void VKAPI_CALL recordWriteTimestamp2(
  VkCommandBuffer, VkPipelineStageFlags2, VkQueryPool pool, uint32_t)
{
  ++g_probe.callbacks;
  g_probe.queryPool = pool;
}

class ScopedCommandCallbacks
{
public:
  ScopedCommandCallbacks()
    : m_drawIndexedIndirect(vkCmdDrawIndexedIndirect)
    , m_drawIndexedIndirectCount(vkCmdDrawIndexedIndirectCount)
    , m_drawIndirect(vkCmdDrawIndirect)
    , m_drawMeshTasksIndirect(vkCmdDrawMeshTasksIndirectEXT)
    , m_dispatchIndirect(vkCmdDispatchIndirect)
    , m_copyBuffer(vkCmdCopyBuffer)
    , m_copyBufferToImage(vkCmdCopyBufferToImage)
    , m_copyImageToBuffer(vkCmdCopyImageToBuffer)
    , m_blitImage(vkCmdBlitImage)
    , m_fillBuffer(vkCmdFillBuffer)
    , m_bindPipeline(vkCmdBindPipeline)
    , m_bindDescriptorSets(vkCmdBindDescriptorSets)
    , m_pushConstants(vkCmdPushConstants)
    , m_beginRendering(vkCmdBeginRendering)
    , m_endRendering(vkCmdEndRendering)
    , m_pipelineBarrier2(vkCmdPipelineBarrier2)
    , m_clearColorImage(vkCmdClearColorImage)
    , m_resetQueryPool(vkCmdResetQueryPool)
    , m_writeTimestamp2(vkCmdWriteTimestamp2)
    , m_createSemaphore(vkCreateSemaphore)
    , m_destroySemaphore(vkDestroySemaphore)
    , m_createCommandPool(vkCreateCommandPool)
    , m_destroyCommandPool(vkDestroyCommandPool)
    , m_allocateCommandBuffers(vkAllocateCommandBuffers)
    , m_freeCommandBuffers(vkFreeCommandBuffers)
    , m_beginCommandBuffer(vkBeginCommandBuffer)
    , m_endCommandBuffer(vkEndCommandBuffer)
    , m_resetCommandPool(vkResetCommandPool)
    , m_queueSubmit2(vkQueueSubmit2)
  {
    vkCmdDrawIndexedIndirect = &recordDrawIndexedIndirect;
    vkCmdDrawIndexedIndirectCount = &recordDrawIndexedIndirectCount;
    vkCmdDrawIndirect = &recordDrawIndirect;
    vkCmdDrawMeshTasksIndirectEXT = &recordDrawMeshTasksIndirect;
    vkCmdDispatchIndirect = &recordDispatchIndirect;
    vkCmdCopyBuffer = &recordCopyBuffer;
    vkCmdCopyBufferToImage = &recordCopyBufferToImage;
    vkCmdCopyImageToBuffer = &recordCopyImageToBuffer;
    vkCmdBlitImage = &recordBlitImage;
    vkCmdFillBuffer = &recordFillBuffer;
    vkCmdBindPipeline = &recordBindPipeline;
    vkCmdBindDescriptorSets = &recordBindDescriptorSets;
    vkCmdPushConstants = &recordPushConstants;
    vkCmdBeginRendering = &recordBeginRendering;
    vkCmdEndRendering = &recordEndRendering;
    vkCmdPipelineBarrier2 = &recordPipelineBarrier2;
    vkCmdClearColorImage = &recordClearColorImage;
    vkCmdResetQueryPool = &recordResetQueryPool;
    vkCmdWriteTimestamp2 = &recordWriteTimestamp2;
    vkCreateSemaphore = &recordCreateSemaphore;
    vkDestroySemaphore = &recordDestroySemaphore;
    vkCreateCommandPool = &recordCreateCommandPool;
    vkDestroyCommandPool = &recordDestroyCommandPool;
    vkAllocateCommandBuffers = &recordAllocateCommandBuffers;
    vkFreeCommandBuffers = &recordFreeCommandBuffers;
    vkBeginCommandBuffer = &recordBeginCommandBuffer;
    vkEndCommandBuffer = &recordEndCommandBuffer;
    vkResetCommandPool = &recordResetCommandPool;
    vkQueueSubmit2 = &recordQueueSubmit2;
  }

  ~ScopedCommandCallbacks()
  {
    vkCmdDrawIndexedIndirect = m_drawIndexedIndirect;
    vkCmdDrawIndexedIndirectCount = m_drawIndexedIndirectCount;
    vkCmdDrawIndirect = m_drawIndirect;
    vkCmdDrawMeshTasksIndirectEXT = m_drawMeshTasksIndirect;
    vkCmdDispatchIndirect = m_dispatchIndirect;
    vkCmdCopyBuffer = m_copyBuffer;
    vkCmdCopyBufferToImage = m_copyBufferToImage;
    vkCmdCopyImageToBuffer = m_copyImageToBuffer;
    vkCmdBlitImage = m_blitImage;
    vkCmdFillBuffer = m_fillBuffer;
    vkCmdBindPipeline = m_bindPipeline;
    vkCmdBindDescriptorSets = m_bindDescriptorSets;
    vkCmdPushConstants = m_pushConstants;
    vkCmdBeginRendering = m_beginRendering;
    vkCmdEndRendering = m_endRendering;
    vkCmdPipelineBarrier2 = m_pipelineBarrier2;
    vkCmdClearColorImage = m_clearColorImage;
    vkCmdResetQueryPool = m_resetQueryPool;
    vkCmdWriteTimestamp2 = m_writeTimestamp2;
    vkCreateSemaphore = m_createSemaphore;
    vkDestroySemaphore = m_destroySemaphore;
    vkCreateCommandPool = m_createCommandPool;
    vkDestroyCommandPool = m_destroyCommandPool;
    vkAllocateCommandBuffers = m_allocateCommandBuffers;
    vkFreeCommandBuffers = m_freeCommandBuffers;
    vkBeginCommandBuffer = m_beginCommandBuffer;
    vkEndCommandBuffer = m_endCommandBuffer;
    vkResetCommandPool = m_resetCommandPool;
    vkQueueSubmit2 = m_queueSubmit2;
  }

  ScopedCommandCallbacks(const ScopedCommandCallbacks&) = delete;
  ScopedCommandCallbacks& operator=(const ScopedCommandCallbacks&) = delete;

private:
  PFN_vkCmdDrawIndexedIndirect m_drawIndexedIndirect;
  PFN_vkCmdDrawIndexedIndirectCount m_drawIndexedIndirectCount;
  PFN_vkCmdDrawIndirect m_drawIndirect;
  PFN_vkCmdDrawMeshTasksIndirectEXT m_drawMeshTasksIndirect;
  PFN_vkCmdDispatchIndirect m_dispatchIndirect;
  PFN_vkCmdCopyBuffer m_copyBuffer;
  PFN_vkCmdCopyBufferToImage m_copyBufferToImage;
  PFN_vkCmdCopyImageToBuffer m_copyImageToBuffer;
  PFN_vkCmdBlitImage m_blitImage;
  PFN_vkCmdFillBuffer m_fillBuffer;
  PFN_vkCmdBindPipeline m_bindPipeline;
  PFN_vkCmdBindDescriptorSets m_bindDescriptorSets;
  PFN_vkCmdPushConstants m_pushConstants;
  PFN_vkCmdBeginRendering m_beginRendering;
  PFN_vkCmdEndRendering m_endRendering;
  PFN_vkCmdPipelineBarrier2 m_pipelineBarrier2;
  PFN_vkCmdClearColorImage m_clearColorImage;
  PFN_vkCmdResetQueryPool m_resetQueryPool;
  PFN_vkCmdWriteTimestamp2 m_writeTimestamp2;
  PFN_vkCreateSemaphore m_createSemaphore;
  PFN_vkDestroySemaphore m_destroySemaphore;
  PFN_vkCreateCommandPool m_createCommandPool;
  PFN_vkDestroyCommandPool m_destroyCommandPool;
  PFN_vkAllocateCommandBuffers m_allocateCommandBuffers;
  PFN_vkFreeCommandBuffers m_freeCommandBuffers;
  PFN_vkBeginCommandBuffer m_beginCommandBuffer;
  PFN_vkEndCommandBuffer m_endCommandBuffer;
  PFN_vkResetCommandPool m_resetCommandPool;
  PFN_vkQueueSubmit2 m_queueSubmit2;
};

static_assert(std::is_default_constructible_v<
              demo::rhi::vulkan::VulkanResourceTable>);
static_assert(!std::is_copy_constructible_v<
              demo::rhi::vulkan::VulkanResourceTable>);
static_assert(!std::is_copy_assignable_v<
              demo::rhi::vulkan::VulkanResourceTable>);
static_assert(!std::is_move_constructible_v<
              demo::rhi::vulkan::VulkanResourceTable>);
static_assert(!std::is_move_assignable_v<
              demo::rhi::vulkan::VulkanResourceTable>);

void require(bool condition, const std::string& message)
{
  if(!condition)
    throw std::runtime_error(message);
}

void resetProbePayload()
{
  g_probe.firstBuffer = VK_NULL_HANDLE;
  g_probe.secondBuffer = VK_NULL_HANDLE;
  g_probe.firstImage = VK_NULL_HANDLE;
  g_probe.secondImage = VK_NULL_HANDLE;
  g_probe.firstView = VK_NULL_HANDLE;
  g_probe.pipeline = VK_NULL_HANDLE;
  g_probe.pipelineLayout = VK_NULL_HANDLE;
  g_probe.descriptorSet = VK_NULL_HANDLE;
  g_probe.queryPool = VK_NULL_HANDLE;
  g_probe.dynamicOffsetCount = 0;
  g_probe.firstDynamicOffset = 0;
}

template <typename Fn>
void expectRejectedWithoutRecording(const char* label, Fn&& fn)
{
  const uint32_t before = g_probe.callbacks;
  bool rejected = false;
  try
  {
    std::forward<Fn>(fn)();
  }
  catch(const std::runtime_error&)
  {
    rejected = true;
  }
  require(rejected, std::string(label) + " did not reject the invalid handle");
  require(g_probe.callbacks == before,
          std::string(label) + " invoked a Vulkan recording callback before rejection");
}

template <typename Fn>
void expectRejectedWithoutSubmit(const char* label, Fn&& fn)
{
  const uint32_t before = g_probe.queueSubmits;
  bool rejected = false;
  try
  {
    std::forward<Fn>(fn)();
  }
  catch(const std::runtime_error&)
  {
    rejected = true;
  }
  require(rejected, std::string(label) + " did not reject submission");
  require(g_probe.queueSubmits == before,
          std::string(label) + " called vkQueueSubmit2 before rejection");
}

class QueueHarness
{
public:
  QueueHarness(
    demo::rhi::vulkan::VulkanResourceTable& table,
    demo::rhi::QueueIdentity identity = {71, 1})
  {
    m_queue.init(
      fakeNativeHandle<VkDevice>(0xE001), fakeNativeHandle<VkQueue>(0xE002),
      0, 0, identity,
      demo::rhi::QueueInfo{
        .queueClass = demo::rhi::QueueClass::graphics,
        .available = true,
      },
      &m_syncRegistry);
    m_allocator = std::make_unique<demo::rhi::vulkan::VulkanCommandAllocator>(
      m_queue, fakeNativeHandle<VkDevice>(0xE001), &table);
  }

  [[nodiscard]] demo::rhi::vulkan::VulkanCommandAllocator& allocator()
  {
    return *m_allocator;
  }

  [[nodiscard]] demo::rhi::vulkan::VulkanQueue& queue()
  {
    return m_queue;
  }

private:
  demo::rhi::vulkan::VulkanQueueSyncRegistry m_syncRegistry;
  demo::rhi::vulkan::VulkanQueue m_queue;
  std::unique_ptr<demo::rhi::vulkan::VulkanCommandAllocator> m_allocator;
};

demo::rhi::SubmissionToken submitOne(
  QueueHarness& harness, demo::rhi::CommandBuffer& commandBuffer)
{
  const std::array<demo::rhi::CommandBuffer*, 1> commandBuffers{{&commandBuffer}};
  return harness.queue().submit(
    demo::rhi::SubmitBatch{.commandBuffers = commandBuffers});
}

template <typename Fn>
void expectRecorded(
  const char* label, VkBuffer first, VkBuffer second, Fn&& fn)
{
  const uint32_t before = g_probe.callbacks;
  resetProbePayload();
  std::forward<Fn>(fn)();
  require(g_probe.callbacks == before + 1,
          std::string(label) + " did not invoke exactly one recording callback");
  require(g_probe.firstBuffer == first && g_probe.secondBuffer == second,
          std::string(label) + " recorded the wrong native buffer");
}

template <typename Fn>
void expectBufferImageRecorded(
  const char* label, VkBuffer buffer, VkImage image, Fn&& fn)
{
  const uint32_t before = g_probe.callbacks;
  resetProbePayload();
  std::forward<Fn>(fn)();
  require(g_probe.callbacks == before + 1,
          std::string(label) + " did not invoke exactly one recording callback");
  require(g_probe.firstBuffer == buffer && g_probe.firstImage == image,
          std::string(label) + " recorded the wrong native buffer/image");
}

template <typename Fn>
void expectImagesRecorded(
  const char* label, VkImage first, VkImage second, Fn&& fn)
{
  const uint32_t before = g_probe.callbacks;
  resetProbePayload();
  std::forward<Fn>(fn)();
  require(g_probe.callbacks == before + 1,
          std::string(label) + " did not invoke exactly one recording callback");
  require(g_probe.firstImage == first && g_probe.secondImage == second,
          std::string(label) + " recorded the wrong native image");
}

template <typename NativeHandle, typename Fn>
void expectNativeRecorded(
  const char* label, NativeHandle expected, NativeHandle RecordingProbe::*member, Fn&& fn)
{
  const uint32_t before = g_probe.callbacks;
  resetProbePayload();
  std::forward<Fn>(fn)();
  require(g_probe.callbacks == before + 1,
          std::string(label) + " did not invoke exactly one recording callback");
  require(g_probe.*member == expected,
          std::string(label) + " recorded the wrong native handle");
}

template <typename Fn>
void expectOneCallback(const char* label, Fn&& fn)
{
  const uint32_t before = g_probe.callbacks;
  resetProbePayload();
  std::forward<Fn>(fn)();
  require(g_probe.callbacks == before + 1,
          std::string(label) + " did not invoke exactly one recording callback");
}

template <typename Fn>
void exerciseSingleBufferPath(
  const char* label, demo::rhi::BufferHandle valid,
  demo::rhi::BufferHandle stale, VkBuffer native, Fn&& fn)
{
  expectRejectedWithoutRecording(label, [&] { fn(demo::rhi::BufferHandle{}); });
  expectRejectedWithoutRecording(label, [&] { fn(stale); });
  expectRecorded(label, native, VK_NULL_HANDLE, [&] { fn(valid); });
}

void testRenderIndirectPaths(
  demo::rhi::vulkan::VulkanResourceTable& table,
  demo::rhi::BufferHandle valid, demo::rhi::BufferHandle other,
  demo::rhi::BufferHandle stale, VkBuffer native, VkBuffer otherNative)
{
  using namespace demo::rhi;
  using namespace demo::rhi::vulkan;

  VulkanCommandBuffer commandBuffer;
  commandBuffer.setTarget(fakeNativeHandle<VkCommandBuffer>(0x100), &table);
  VulkanRenderEncoder encoder;
  encoder.prepare(
    fakeNativeHandle<VkCommandBuffer>(0x100), &table, &commandBuffer);

  exerciseSingleBufferPath(
    "drawIndexedIndirect", valid, stale, native,
    [&](BufferHandle handle) {
      encoder.drawIndexedIndirect(DrawIndirectDesc{.argsBuffer = handle});
    });
  exerciseSingleBufferPath(
    "drawIndirect", valid, stale, native,
    [&](BufferHandle handle) {
      encoder.drawIndirect(DrawIndirectDesc{.argsBuffer = handle});
    });
  exerciseSingleBufferPath(
    "drawMeshTasksIndirect", valid, stale, native,
    [&](BufferHandle handle) {
      encoder.drawMeshTasksIndirect(DrawIndirectDesc{.argsBuffer = handle});
    });
  const PFN_vkCmdDrawMeshTasksIndirectEXT meshIndirectCallback =
    vkCmdDrawMeshTasksIndirectEXT;
  vkCmdDrawMeshTasksIndirectEXT = nullptr;
  expectRejectedWithoutRecording(
    "drawMeshTasksIndirect callback-null stale",
    [&] { encoder.drawMeshTasksIndirect(DrawIndirectDesc{.argsBuffer = stale}); });
  const uint32_t beforeCallbackNull = g_probe.callbacks;
  encoder.drawMeshTasksIndirect(DrawIndirectDesc{.argsBuffer = valid});
  require(g_probe.callbacks == beforeCallbackNull,
          "drawMeshTasksIndirect callback-null unexpectedly recorded");
  vkCmdDrawMeshTasksIndirectEXT = meshIndirectCallback;

  const auto drawCount = [&](BufferHandle args, BufferHandle count) {
    encoder.drawIndexedIndirectCount(DrawIndirectCountDesc{
      .argsBuffer = args,
      .countBuffer = count,
      .maxDrawCount = 1,
    });
  };
  expectRejectedWithoutRecording(
    "drawIndexedIndirectCount null args", [&] { drawCount({}, other); });
  expectRejectedWithoutRecording(
    "drawIndexedIndirectCount stale args", [&] { drawCount(stale, other); });
  expectRejectedWithoutRecording(
    "drawIndexedIndirectCount null count", [&] { drawCount(valid, {}); });
  expectRejectedWithoutRecording(
    "drawIndexedIndirectCount stale count", [&] { drawCount(valid, stale); });
  expectRecorded(
    "drawIndexedIndirectCount", native, otherNative,
    [&] { drawCount(valid, other); });
}

void testComputeAndTransferPaths(
  demo::rhi::vulkan::VulkanResourceTable& table,
  demo::rhi::BufferHandle valid, demo::rhi::BufferHandle other,
  demo::rhi::BufferHandle stale, demo::rhi::TextureHandle texture,
  VkBuffer native, VkBuffer otherNative)
{
  using namespace demo::rhi;
  using namespace demo::rhi::vulkan;

  VulkanCommandBuffer commandBuffer;
  commandBuffer.setTarget(fakeNativeHandle<VkCommandBuffer>(0x200), &table);
  ComputeEncoder* encoder = commandBuffer.beginComputePass();

  exerciseSingleBufferPath(
    "dispatchIndirect", valid, stale, native,
    [&](BufferHandle handle) {
      encoder->dispatchIndirect(DispatchIndirectDesc{.argsBuffer = handle});
    });

  const auto copyBuffer = [&](BufferHandle source, BufferHandle destination) {
    encoder->copyBuffer(source, 0, destination, 0, 4);
  };
  expectRejectedWithoutRecording(
    "copyBuffer null source", [&] { copyBuffer({}, other); });
  expectRejectedWithoutRecording(
    "copyBuffer stale source", [&] { copyBuffer(stale, other); });
  expectRejectedWithoutRecording(
    "copyBuffer null destination", [&] { copyBuffer(valid, {}); });
  expectRejectedWithoutRecording(
    "copyBuffer stale destination", [&] { copyBuffer(valid, stale); });
  expectRecorded(
    "copyBuffer", native, otherNative,
    [&] { copyBuffer(valid, other); });

  exerciseSingleBufferPath(
    "copyBufferToTexture", valid, stale, native,
    [&](BufferHandle handle) {
      encoder->copyBufferToTexture(BufferTextureCopyDesc{
        .buffer = handle,
        .texture = texture,
        .width = 1,
        .height = 1,
      });
    });
  exerciseSingleBufferPath(
    "copyTextureToBuffer", valid, stale, native,
    [&](BufferHandle handle) {
      encoder->copyTextureToBuffer(BufferTextureCopyDesc{
        .buffer = handle,
        .texture = texture,
        .width = 1,
        .height = 1,
      });
    });
  exerciseSingleBufferPath(
    "fillBuffer", valid, stale, native,
    [&](BufferHandle handle) { encoder->fillBuffer(handle, 0, 4, 0); });

  commandBuffer.endEncoding();
}

void testTextureTransferPaths(
  demo::rhi::vulkan::VulkanResourceTable& table,
  demo::rhi::BufferHandle buffer, demo::rhi::TextureHandle valid,
  demo::rhi::TextureHandle other, demo::rhi::TextureHandle stale,
  demo::rhi::TextureHandle foreign, VkBuffer nativeBuffer,
  VkImage nativeImage, VkImage otherImage)
{
  using namespace demo::rhi;
  using namespace demo::rhi::vulkan;

  VulkanCommandBuffer commandBuffer;
  commandBuffer.setTarget(fakeNativeHandle<VkCommandBuffer>(0x400), &table);
  ComputeEncoder* encoder = commandBuffer.beginComputePass();

  const auto copyToTexture = [&](TextureHandle texture) {
    encoder->copyBufferToTexture(BufferTextureCopyDesc{
      .buffer = buffer,
      .texture = texture,
      .width = 1,
      .height = 1,
    });
  };
  expectRejectedWithoutRecording(
    "copyBufferToTexture null texture", [&] { copyToTexture({}); });
  expectRejectedWithoutRecording(
    "copyBufferToTexture stale texture", [&] { copyToTexture(stale); });
  expectRejectedWithoutRecording(
    "copyBufferToTexture foreign texture", [&] { copyToTexture(foreign); });
  expectBufferImageRecorded(
    "copyBufferToTexture valid texture", nativeBuffer, nativeImage,
    [&] { copyToTexture(valid); });

  const auto copyFromTexture = [&](TextureHandle texture) {
    encoder->copyTextureToBuffer(BufferTextureCopyDesc{
      .buffer = buffer,
      .texture = texture,
      .width = 1,
      .height = 1,
    });
  };
  expectRejectedWithoutRecording(
    "copyTextureToBuffer null texture", [&] { copyFromTexture({}); });
  expectRejectedWithoutRecording(
    "copyTextureToBuffer stale texture", [&] { copyFromTexture(stale); });
  expectRejectedWithoutRecording(
    "copyTextureToBuffer foreign texture", [&] { copyFromTexture(foreign); });
  expectBufferImageRecorded(
    "copyTextureToBuffer valid texture", nativeBuffer, nativeImage,
    [&] { copyFromTexture(valid); });

  const auto blit = [&](TextureHandle source, TextureHandle destination) {
    encoder->blitTexture(TextureBlitDesc{
      .srcTexture = source,
      .dstTexture = destination,
      .srcOffsets = {{0, 0, 0}, {1, 1, 1}},
      .dstOffsets = {{0, 0, 0}, {1, 1, 1}},
    });
  };
  expectRejectedWithoutRecording(
    "blitTexture null source", [&] { blit({}, other); });
  expectRejectedWithoutRecording(
    "blitTexture stale source", [&] { blit(stale, other); });
  expectRejectedWithoutRecording(
    "blitTexture null destination", [&] { blit(valid, {}); });
  expectRejectedWithoutRecording(
    "blitTexture stale destination", [&] { blit(valid, stale); });
  expectRejectedWithoutRecording(
    "blitTexture foreign destination", [&] { blit(valid, foreign); });
  expectImagesRecorded(
    "blitTexture", nativeImage, otherImage, [&] { blit(valid, other); });

  TextureHotRecord* hot = const_cast<TextureHotRecord*>(table.tryGetTextureHot(valid));
  require(hot != nullptr, "valid texture fixture disappeared");
  hot->nativeImage = VK_NULL_HANDLE;
  expectRejectedWithoutRecording(
    "copyBufferToTexture null-native texture", [&] { copyToTexture(valid); });
  hot->nativeImage = nativeImage;

  commandBuffer.endEncoding();
}

void testTextureViewPaths(
  demo::rhi::vulkan::VulkanResourceTable& table,
  demo::rhi::TextureViewHandle valid, demo::rhi::TextureViewHandle stale,
  demo::rhi::TextureViewHandle foreign,
  demo::rhi::TextureViewHandle viewWithStaleParent,
  demo::rhi::TextureHandle parentTexture,
  demo::rhi::TextureHandle otherTexture,
  demo::rhi::TextureHandle staleTexture, VkImageView nativeView)
{
  using namespace demo::rhi;
  using namespace demo::rhi::vulkan;

  const auto beginWithViews = [&](std::span<const RenderTargetDesc> colors,
                                  const DepthTargetDesc* depth,
                                  std::span<const InputAttachmentDesc> inputs) {
    VulkanCommandBuffer commandBuffer;
    commandBuffer.setTarget(fakeNativeHandle<VkCommandBuffer>(0x500), &table);
    commandBuffer.beginRenderPass(RenderPassDesc{
      .renderArea = {.extent = {1, 1}},
      .colorTargets = colors,
      .depthTarget = depth,
      .inputAttachments = inputs,
    });
  };

  const std::array<RenderTargetDesc, 1> nullColors{{{.view = {}}}};
  expectRejectedWithoutRecording(
    "beginRenderPass null color view",
    [&] { beginWithViews(nullColors, nullptr, {}); });
  const std::array<RenderTargetDesc, 1> staleColors{{{.view = stale}}};
  expectRejectedWithoutRecording(
    "beginRenderPass stale color view",
    [&] { beginWithViews(staleColors, nullptr, {}); });
  const std::array<RenderTargetDesc, 1> foreignColors{{{.view = foreign}}};
  expectRejectedWithoutRecording(
    "beginRenderPass foreign color view",
    [&] { beginWithViews(foreignColors, nullptr, {}); });
  const std::array<RenderTargetDesc, 1> staleParentColors{{{
    .view = viewWithStaleParent,
  }}};
  expectRejectedWithoutRecording(
    "beginRenderPass stale parent texture",
    [&] { beginWithViews(staleParentColors, nullptr, {}); });
  const std::array<RenderTargetDesc, 1> staleTargetTextureColors{{{
    .texture = staleTexture,
    .view = valid,
  }}};
  expectRejectedWithoutRecording(
    "beginRenderPass stale target texture",
    [&] { beginWithViews(staleTargetTextureColors, nullptr, {}); });
  const std::array<RenderTargetDesc, 1> mismatchedColors{{{
    .texture = otherTexture,
    .view = valid,
  }}};
  expectRejectedWithoutRecording(
    "beginRenderPass mismatched color texture/view",
    [&] { beginWithViews(mismatchedColors, nullptr, {}); });

  const std::array<RenderTargetDesc, 2> lateStaleColors{{
    {.view = valid},
    {.view = stale},
  }};
  expectRejectedWithoutRecording(
    "beginRenderPass late stale color view",
    [&] { beginWithViews(lateStaleColors, nullptr, {}); });

  const std::array<RenderTargetDesc, 1> validColors{{{.view = valid}}};
  const DepthTargetDesc staleDepth{.view = stale};
  expectRejectedWithoutRecording(
    "beginRenderPass stale depth view",
    [&] { beginWithViews(validColors, &staleDepth, {}); });
  const DepthTargetDesc mismatchedDepth{
    .texture = otherTexture,
    .view = valid,
  };
  expectRejectedWithoutRecording(
    "beginRenderPass mismatched depth texture/view",
    [&] { beginWithViews({}, &mismatchedDepth, {}); });
  const std::array<InputAttachmentDesc, 1> staleInputs{{{.view = stale}}};
  expectRejectedWithoutRecording(
    "beginRenderPass stale input view",
    [&] { beginWithViews(validColors, nullptr, staleInputs); });
  expectNativeRecorded(
    "beginRenderPass valid view", nativeView, &RecordingProbe::firstView,
    [&] { beginWithViews(validColors, nullptr, {}); });
  const std::array<RenderTargetDesc, 1> matchingColors{{{
    .texture = parentTexture,
    .view = valid,
  }}};
  expectNativeRecorded(
    "beginRenderPass matching texture/view", nativeView,
    &RecordingProbe::firstView,
    [&] { beginWithViews(matchingColors, nullptr, {}); });

  TextureViewHotRecord* hot =
    const_cast<TextureViewHotRecord*>(table.tryGetTextureViewHot(valid));
  require(hot != nullptr, "valid texture-view fixture disappeared");
  hot->nativeView = VK_NULL_HANDLE;
  expectRejectedWithoutRecording(
    "beginRenderPass null-native view",
    [&] { beginWithViews(validColors, nullptr, {}); });
  hot->nativeView = nativeView;
}

void testPipelineAndArgumentTablePaths(
  demo::rhi::vulkan::VulkanResourceTable& table,
  demo::rhi::BufferHandle validBuffer,
  demo::rhi::BufferHandle staleBuffer,
  demo::rhi::BufferHandle foreignBuffer,
  demo::rhi::PipelineHandle graphicsPipeline,
  demo::rhi::PipelineHandle computePipeline,
  demo::rhi::PipelineHandle stalePipeline,
  demo::rhi::PipelineHandle foreignPipeline,
  demo::rhi::ArgumentTableHandle argumentTable,
  demo::rhi::ArgumentTableHandle staleArgumentTable,
  demo::rhi::ArgumentTableHandle foreignArgumentTable,
  demo::rhi::ArgumentTableHandle tableWithStaleSampler,
  demo::rhi::ArgumentTableHandle tableWithStaleView,
  VkPipeline nativeGraphics, VkPipeline nativeCompute,
  VkDescriptorSet nativeSet)
{
  using namespace demo::rhi;
  using namespace demo::rhi::vulkan;

  VulkanCommandBuffer commandBuffer;
  commandBuffer.setTarget(fakeNativeHandle<VkCommandBuffer>(0x600), &table);
  VulkanRenderEncoder renderEncoder;
  renderEncoder.prepare(
    fakeNativeHandle<VkCommandBuffer>(0x600), &table, &commandBuffer);

  expectRejectedWithoutRecording(
    "render setPipeline null", [&] { renderEncoder.setPipeline({}); });
  expectRejectedWithoutRecording(
    "render setPipeline stale", [&] { renderEncoder.setPipeline(stalePipeline); });
  expectRejectedWithoutRecording(
    "render setPipeline foreign", [&] { renderEncoder.setPipeline(foreignPipeline); });
  expectRejectedWithoutRecording(
    "render setPipeline wrong bind point",
    [&] { renderEncoder.setPipeline(computePipeline); });
  expectNativeRecorded(
    "render setPipeline valid", nativeGraphics, &RecordingProbe::pipeline,
    [&] { renderEncoder.setPipeline(graphicsPipeline); });

  PipelineRecord* pipelineRecord =
    const_cast<PipelineRecord*>(table.tryGetPipeline(graphicsPipeline));
  require(pipelineRecord != nullptr, "valid pipeline fixture disappeared");
  pipelineRecord->nativePipeline = VK_NULL_HANDLE;
  expectRejectedWithoutRecording(
    "render setPipeline null-native",
    [&] { renderEncoder.setPipeline(graphicsPipeline); });
  pipelineRecord->nativePipeline = nativeGraphics;
  const VkPipelineLayout nativeLayout = pipelineRecord->nativeLayout;
  pipelineRecord->nativeLayout = VK_NULL_HANDLE;
  expectRejectedWithoutRecording(
    "render setPipeline null layout",
    [&] { renderEncoder.setPipeline(graphicsPipeline); });
  pipelineRecord->nativeLayout = nativeLayout;

  const auto bindArgumentTable = [&] {
    renderEncoder.setArgumentTable(ShaderStage::fragment, 0, argumentTable);
  };
  const auto requireBoundOffset = [&](uint32_t count, uint32_t offset,
                                      const char* label) {
    expectNativeRecorded(
      label, nativeSet, &RecordingProbe::descriptorSet, bindArgumentTable);
    require(g_probe.dynamicOffsetCount == count,
            std::string(label) + " recorded the wrong dynamic-offset count");
    require(count == 0 || g_probe.firstDynamicOffset == offset,
            std::string(label) + " recorded the wrong dynamic offset");
  };

  expectRejectedWithoutRecording(
    "setDynamicBuffer stale",
    [&] { renderEncoder.setDynamicBuffer(
      ShaderStage::allGraphics, 0, staleBuffer, 17, 4); });
  requireBoundOffset(0, 0, "setDynamicBuffer stale leaves no offset");
  expectRejectedWithoutRecording(
    "setDynamicBuffer foreign",
    [&] { renderEncoder.setDynamicBuffer(
      ShaderStage::allGraphics, 0, foreignBuffer, 19, 4); });
  requireBoundOffset(0, 0, "setDynamicBuffer foreign leaves no offset");
  expectRejectedWithoutRecording(
    "setDynamicBuffer malformed non-null",
    [&] { renderEncoder.setDynamicBuffer(
      ShaderStage::allGraphics, 0, BufferHandle{validBuffer.index, 0}, 21, 4); });
  requireBoundOffset(0, 0, "setDynamicBuffer malformed leaves no offset");

  BufferHotRecord* bufferRecord =
    const_cast<BufferHotRecord*>(table.tryGetBufferHot(validBuffer));
  require(bufferRecord != nullptr, "valid dynamic-buffer fixture disappeared");
  const VkBuffer validNativeBuffer = bufferRecord->nativeBuffer;
  bufferRecord->nativeBuffer = VK_NULL_HANDLE;
  expectRejectedWithoutRecording(
    "setDynamicBuffer null-native",
    [&] { renderEncoder.setDynamicBuffer(
      ShaderStage::allGraphics, 0, validBuffer, 23, 4); });
  requireBoundOffset(0, 0, "setDynamicBuffer null-native leaves no offset");
  bufferRecord->nativeBuffer = validNativeBuffer;

  renderEncoder.setDynamicBuffer(
    ShaderStage::allGraphics, 0, {}, 29, 4);
  requireBoundOffset(1, 29, "setDynamicBuffer optional-null offset-only");
  renderEncoder.setDynamicBuffer(
    ShaderStage::allGraphics, 0, validBuffer, 31, 4);
  requireBoundOffset(1, 31, "setDynamicBuffer valid buffer");

  expectRejectedWithoutRecording(
    "render setArgumentTable null",
    [&] { renderEncoder.setArgumentTable(ShaderStage::fragment, 0, {}); });
  expectRejectedWithoutRecording(
    "render setArgumentTable stale",
    [&] { renderEncoder.setArgumentTable(
      ShaderStage::fragment, 0, staleArgumentTable); });
  expectRejectedWithoutRecording(
    "render setArgumentTable foreign",
    [&] { renderEncoder.setArgumentTable(
      ShaderStage::fragment, 0, foreignArgumentTable); });
  expectRejectedWithoutRecording(
    "render setArgumentTable stale sampler",
    [&] { renderEncoder.setArgumentTable(
      ShaderStage::fragment, 0, tableWithStaleSampler); });
  expectRejectedWithoutRecording(
    "render setArgumentTable stale texture view",
    [&] { renderEncoder.setArgumentTable(
      ShaderStage::fragment, 0, tableWithStaleView); });
  expectNativeRecorded(
    "render setArgumentTable valid", nativeSet, &RecordingProbe::descriptorSet,
    [&] { renderEncoder.setArgumentTable(
      ShaderStage::fragment, 0, argumentTable); });

  ArgumentTableRecord* tableRecord = table.tryGetArgumentTableMutable(argumentTable);
  require(tableRecord != nullptr, "valid argument-table fixture disappeared");
  tableRecord->nativeSet = VK_NULL_HANDLE;
  expectRejectedWithoutRecording(
    "render setArgumentTable null-native",
    [&] { renderEncoder.setArgumentTable(ShaderStage::fragment, 0, argumentTable); });
  tableRecord->nativeSet = nativeSet;

  const BufferHandle cachedValidationBuffer = table.registerBuffer(
    BufferHotRecord{.nativeBuffer = fakeNativeHandle<VkBuffer>(0x323)},
    BufferColdRecord{.desc = BufferDesc{.size = 64}});
  const VkDescriptorSet cachedValidationNativeSet =
    fakeNativeHandle<VkDescriptorSet>(0x324);
  const ArgumentTableHandle cachedValidationTable = table.registerArgumentTable(
    cachedValidationNativeSet, {}, ArgumentTableLifetime::persistent, false);
  table.recordArgumentTableResource(
    cachedValidationTable, 0, 0, residencyResource(cachedValidationBuffer));
  expectNativeRecorded(
    "render setArgumentTable primes validation cache",
    cachedValidationNativeSet, &RecordingProbe::descriptorSet,
    [&] { renderEncoder.setArgumentTable(
      ShaderStage::fragment, 0, cachedValidationTable); });
  const ArgumentTableRecord* cachedValidationRecord =
    table.tryGetArgumentTable(cachedValidationTable);
  require(cachedValidationRecord != nullptr &&
          cachedValidationRecord->validatedResourceEpoch != 0u,
          "argument-table validation did not publish its resource epoch");
  const std::uint64_t validatedEpoch =
    cachedValidationRecord->validatedResourceEpoch;
  expectNativeRecorded(
    "render setArgumentTable reuses validation cache",
    cachedValidationNativeSet, &RecordingProbe::descriptorSet,
    [&] { renderEncoder.setArgumentTable(
      ShaderStage::fragment, 0, cachedValidationTable); });
  require(cachedValidationRecord->validatedResourceEpoch == validatedEpoch,
          "unchanged argument-table validation unexpectedly advanced its epoch");
  (void)table.removeBuffer(cachedValidationBuffer);
  expectRejectedWithoutRecording(
    "render setArgumentTable invalidates cached resource validation",
    [&] { renderEncoder.setArgumentTable(
      ShaderStage::fragment, 0, cachedValidationTable); });
  (void)table.removeArgumentTable(cachedValidationTable);

  VulkanComputeEncoder computeEncoder;
  computeEncoder.prepare(
    fakeNativeHandle<VkCommandBuffer>(0x601), &table, &commandBuffer);
  expectRejectedWithoutRecording(
    "compute setPipeline stale", [&] { computeEncoder.setPipeline(stalePipeline); });
  expectNativeRecorded(
    "compute setPipeline valid", nativeCompute, &RecordingProbe::pipeline,
    [&] { computeEncoder.setPipeline(computePipeline); });
  expectRejectedWithoutRecording(
    "compute setArgumentTable stale",
    [&] { computeEncoder.setArgumentTable(0, staleArgumentTable); });
  expectNativeRecorded(
    "compute setArgumentTable valid", nativeSet, &RecordingProbe::descriptorSet,
    [&] { computeEncoder.setArgumentTable(0, argumentTable); });
}

void testTextureAndQueryCommandPaths(
  demo::rhi::vulkan::VulkanResourceTable& table,
  demo::rhi::TextureHandle valid, demo::rhi::TextureHandle stale,
  demo::rhi::TextureHandle foreign, VkImage nativeImage,
  demo::rhi::QueryPoolHandle queryPool,
  demo::rhi::QueryPoolHandle staleQueryPool,
  demo::rhi::QueryPoolHandle foreignQueryPool, VkQueryPool nativeQueryPool)
{
  using namespace demo::rhi;
  using namespace demo::rhi::vulkan;

  VulkanCommandBuffer commandBuffer;
  commandBuffer.setTarget(fakeNativeHandle<VkCommandBuffer>(0x700), &table);
  const TextureSubresourceRange range{};
  const ClearColorValue clear{};
  expectRejectedWithoutRecording(
    "clearColorTexture null", [&] { commandBuffer.clearColorTexture({}, range, clear); });
  expectRejectedWithoutRecording(
    "clearColorTexture stale", [&] { commandBuffer.clearColorTexture(stale, range, clear); });
  expectRejectedWithoutRecording(
    "clearColorTexture foreign", [&] { commandBuffer.clearColorTexture(foreign, range, clear); });
  expectNativeRecorded(
    "clearColorTexture valid", nativeImage, &RecordingProbe::firstImage,
    [&] { commandBuffer.clearColorTexture(valid, range, clear); });

  std::vector<TextureBarrier> barriers;
  barriers.reserve(17);
  for(uintptr_t index = 0; index < 16; ++index)
  {
    const TextureHandle handle = table.registerTexture(
      fakeNativeHandle<VkImage>(0x800 + index), TextureColdRecord{.owned = false});
    barriers.push_back(TextureBarrier{
      .texture = handle,
      .before = ResourceState::Undefined,
      .after = ResourceState::TransferDst,
    });
  }
  barriers.push_back(TextureBarrier{
    .texture = stale,
    .before = ResourceState::Undefined,
    .after = ResourceState::TransferDst,
  });
  expectRejectedWithoutRecording(
    "resourceBarrier late stale texture",
    [&] { commandBuffer.resourceBarrier(barriers, {}); });

  const std::array<TextureBarrier, 1> validBarrier{{{
    .texture = valid,
    .before = ResourceState::Undefined,
    .after = ResourceState::TransferDst,
  }}};
  expectNativeRecorded(
    "resourceBarrier valid texture", nativeImage, &RecordingProbe::firstImage,
    [&] { commandBuffer.resourceBarrier(validBarrier, {}); });

#ifndef NDEBUG
  VulkanCommandBuffer atomicStateCommandBuffer;
  atomicStateCommandBuffer.setTarget(
    fakeNativeHandle<VkCommandBuffer>(0x702), &table);
  const std::array<TextureBarrier, 2> invalidTransitionSequence{{
    {
      .texture = valid,
      .before = ResourceState::Undefined,
      .after = ResourceState::TransferDst,
    },
    {
      .texture = valid,
      .before = ResourceState::ShaderRead,
      .after = ResourceState::TransferSrc,
    },
  }};
  expectRejectedWithoutRecording(
    "resourceBarrier late invalid transition",
    [&] { atomicStateCommandBuffer.resourceBarrier(invalidTransitionSequence, {}); });
  const std::array<TextureBarrier, 1> provesStateRollback{{{
    .texture = valid,
    .before = ResourceState::ShaderRead,
    .after = ResourceState::TransferDst,
  }}};
  expectOneCallback(
    "resourceBarrier state rollback",
    [&] { atomicStateCommandBuffer.resourceBarrier(provesStateRollback, {}); });
#endif

  VulkanCommandBuffer largeBarrierCommandBuffer;
  largeBarrierCommandBuffer.setTarget(
    fakeNativeHandle<VkCommandBuffer>(0x703), &table);
  std::vector<TextureBarrier> largeBarrierSet;
  largeBarrierSet.reserve(257);
  for(uintptr_t index = 0; index < 257; ++index)
  {
    const TextureHandle handle = table.registerTexture(
      fakeNativeHandle<VkImage>(0x1000 + index),
      TextureColdRecord{.owned = false});
    largeBarrierSet.push_back(TextureBarrier{
      .texture = handle,
      .before = ResourceState::Undefined,
      .after = ResourceState::TransferDst,
    });
  }
  const uint32_t beforeLargeBarrier = g_probe.callbacks;
  largeBarrierCommandBuffer.resourceBarrier(largeBarrierSet, {});
  require(g_probe.callbacks == beforeLargeBarrier + 17,
          "resourceBarrier did not record every preflighted batch");

  VulkanCommandBuffer aliasCommandBuffer;
  aliasCommandBuffer.setTarget(fakeNativeHandle<VkCommandBuffer>(0x701), &table);
  const std::array<AliasingBarrier, 1> staleAlias{{{
    .before = AliasingResource::fromTexture(valid),
    .after = AliasingResource::fromTexture(stale),
  }}};
  expectRejectedWithoutRecording(
    "resourceBarrier stale alias after valid texture",
    [&] { aliasCommandBuffer.resourceBarrier(validBarrier, {}, staleAlias); });
  const std::array<AliasingBarrier, 1> validAlias{{{
    .after = AliasingResource::fromTexture(valid),
  }}};
  expectOneCallback(
    "resourceBarrier valid texture alias",
    [&] { aliasCommandBuffer.resourceBarrier({}, {}, validAlias); });

  expectRejectedWithoutRecording(
    "resetQueryPool null", [&] { commandBuffer.resetQueryPool({}, 0, 1); });
  expectRejectedWithoutRecording(
    "resetQueryPool stale",
    [&] { commandBuffer.resetQueryPool(staleQueryPool, 0, 1); });
  expectRejectedWithoutRecording(
    "resetQueryPool foreign",
    [&] { commandBuffer.resetQueryPool(foreignQueryPool, 0, 1); });
  expectNativeRecorded(
    "resetQueryPool valid", nativeQueryPool, &RecordingProbe::queryPool,
    [&] { commandBuffer.resetQueryPool(queryPool, 0, 1); });
  expectNativeRecorded(
    "resetQueryPool last valid", nativeQueryPool, &RecordingProbe::queryPool,
    [&] { commandBuffer.resetQueryPool(queryPool, 3, 1); });
  expectRejectedWithoutRecording(
    "resetQueryPool one-past-end",
    [&] { commandBuffer.resetQueryPool(queryPool, 4, 1); });
  expectRejectedWithoutRecording(
    "resetQueryPool range past end",
    [&] { commandBuffer.resetQueryPool(queryPool, 3, 2); });
  expectRejectedWithoutRecording(
    "resetQueryPool overflowing range",
    [&] { commandBuffer.resetQueryPool(
      queryPool, (std::numeric_limits<uint32_t>::max)(), 2); });

  const uint32_t beforeZeroReset = g_probe.callbacks;
  commandBuffer.resetQueryPool(queryPool, 4, 0);
  require(g_probe.callbacks == beforeZeroReset,
          "zero-count query reset unexpectedly recorded a callback");
  expectRejectedWithoutRecording(
    "zero-count query reset starts past pool",
    [&] { commandBuffer.resetQueryPool(queryPool, 5, 0); });

  expectRejectedWithoutRecording(
    "writeTimestamp null", [&] { commandBuffer.writeTimestamp({}, 0, false); });
  expectRejectedWithoutRecording(
    "writeTimestamp stale",
    [&] { commandBuffer.writeTimestamp(staleQueryPool, 0, false); });
  expectRejectedWithoutRecording(
    "writeTimestamp foreign",
    [&] { commandBuffer.writeTimestamp(foreignQueryPool, 0, false); });
  expectNativeRecorded(
    "writeTimestamp valid", nativeQueryPool, &RecordingProbe::queryPool,
    [&] { commandBuffer.writeTimestamp(queryPool, 0, false); });
  expectNativeRecorded(
    "writeTimestamp last valid", nativeQueryPool, &RecordingProbe::queryPool,
    [&] { commandBuffer.writeTimestamp(queryPool, 3, false); });
  expectRejectedWithoutRecording(
    "writeTimestamp one-past-end",
    [&] { commandBuffer.writeTimestamp(queryPool, 4, false); });

  const QueryPoolHandle emptyPool = table.registerQueryPool(
    fakeNativeHandle<VkQueryPool>(0x320), 0);
  const uint32_t beforeEmptyReset = g_probe.callbacks;
  commandBuffer.resetQueryPool(emptyPool, 0, 0);
  require(g_probe.callbacks == beforeEmptyReset,
          "empty query-pool zero reset unexpectedly recorded a callback");
  expectRejectedWithoutRecording(
    "empty query-pool reset",
    [&] { commandBuffer.resetQueryPool(emptyPool, 0, 1); });
  expectRejectedWithoutRecording(
    "empty query-pool timestamp",
    [&] { commandBuffer.writeTimestamp(emptyPool, 0, false); });

  QueryPoolRecord* queryRecord =
    const_cast<QueryPoolRecord*>(table.tryGetQueryPool(queryPool));
  require(queryRecord != nullptr, "valid query-pool fixture disappeared");
  queryRecord->nativePool = VK_NULL_HANDLE;
  expectRejectedWithoutRecording(
    "resetQueryPool null-native",
    [&] { commandBuffer.resetQueryPool(queryPool, 0, 1); });
  queryRecord->nativePool = nativeQueryPool;
}

void testSubmissionTransactions()
{
  using namespace demo::rhi;
  using namespace demo::rhi::vulkan;

  VulkanResourceTable table;
  QueueHarness harness(table, QueueIdentity{81, 1});

  const auto createCommand = [&]() {
    std::unique_ptr<CommandBuffer> command =
      harness.allocator().createCommandBuffer();
    command->begin(harness.allocator());
    return command;
  };

  const VkBuffer submittedNative = fakeNativeHandle<VkBuffer>(0xD001);
  const BufferHandle submittedBuffer = table.registerBuffer(
    BufferHotRecord{.nativeBuffer = submittedNative},
    BufferColdRecord{.desc = BufferDesc{.size = 64}});
  std::unique_ptr<CommandBuffer> submittedCommand = createCommand();
  ComputeEncoder* submittedEncoder = submittedCommand->beginComputePass();
  submittedEncoder->fillBuffer(submittedBuffer, 0, 4, 0);
  submittedCommand->endEncoding();
  submittedCommand->end();
  const uint32_t beforeSuccessfulSubmit = g_probe.queueSubmits;
  const SubmissionToken submittedToken = submitOne(harness, *submittedCommand);
  require(g_probe.queueSubmits == beforeSuccessfulSubmit + 1,
          "valid submission did not call vkQueueSubmit2 exactly once");
  require(submittedCommand->state() == CommandBufferState::submitted,
          "valid submission did not update command-buffer state");
  const BufferHotRecord* submittedHot = table.tryGetBufferHot(submittedBuffer);
  require(submittedHot != nullptr && submittedHot->pendingUses.count == 1 &&
            submittedHot->pendingUses.tokens[0] == submittedToken,
          "valid submission did not record its retirement dependency");
  const BufferRecord retiredBuffer = table.removeBuffer(submittedBuffer);
  require(retiredBuffer.hot.pendingUses.count == 1 &&
            retiredBuffer.hot.pendingUses.tokens[0] == submittedToken,
          "buffer retirement lost the committed submission dependency");

  const TextureHandle submittedParent = table.registerTexture(
    fakeNativeHandle<VkImage>(0xD003), TextureColdRecord{.owned = false});
  const TextureViewHandle submittedView = table.registerTextureView(
    fakeNativeHandle<VkImageView>(0xD004),
    TextureViewColdRecord{.parentTexture = submittedParent, .owned = false});
  std::unique_ptr<CommandBuffer> submittedViewCommand = createCommand();
  const std::array<RenderTargetDesc, 1> submittedViewTarget{{{
    .view = submittedView,
  }}};
  submittedViewCommand->beginRenderPass(RenderPassDesc{
    .renderArea = {.extent = {1, 1}},
    .colorTargets = submittedViewTarget,
  });
  submittedViewCommand->endEncoding();
  submittedViewCommand->end();
  const SubmissionToken submittedViewToken =
    submitOne(harness, *submittedViewCommand);
  const TextureViewHotRecord* submittedViewHot =
    table.tryGetTextureViewHot(submittedView);
  const TextureHotRecord* submittedParentHot =
    table.tryGetTextureHot(submittedParent);
  require(submittedViewHot != nullptr && submittedParentHot != nullptr &&
            submittedViewHot->pendingUses.count == 1 &&
            submittedParentHot->pendingUses.count == 1 &&
            submittedViewHot->pendingUses.tokens[0] == submittedViewToken &&
            submittedParentHot->pendingUses.tokens[0] == submittedViewToken,
          "valid texture-view submission did not retain its parent transactionally");

  const BufferHandle dynamicBuffer = table.registerBuffer(
    BufferHotRecord{.nativeBuffer = fakeNativeHandle<VkBuffer>(0xD005)},
    BufferColdRecord{.desc = BufferDesc{.size = 64}});
  const PipelineHandle dynamicPipeline = table.registerPipeline(
    VK_PIPELINE_BIND_POINT_GRAPHICS,
    fakeNativeHandle<VkPipeline>(0xD006), 0,
    fakeNativeHandle<VkPipelineLayout>(0xD007), {}, false, false);
  const ArgumentTableHandle dynamicTable = table.registerArgumentTable(
    fakeNativeHandle<VkDescriptorSet>(0xD008), {},
    ArgumentTableLifetime::persistent, false);
  const BufferHandle staleDynamicBuffer = table.registerBuffer(
    BufferHotRecord{.nativeBuffer = fakeNativeHandle<VkBuffer>(0xD009)},
    BufferColdRecord{.desc = BufferDesc{.size = 64}});
  (void)table.removeBuffer(staleDynamicBuffer);
  std::unique_ptr<CommandBuffer> dynamicCommand = createCommand();
  auto* vkDynamicCommand = dynamic_cast<VulkanCommandBuffer*>(dynamicCommand.get());
  require(vkDynamicCommand != nullptr, "Vulkan command fixture has wrong type");
  VulkanRenderEncoder dynamicEncoder;
  dynamicEncoder.prepare(
    vkDynamicCommand->nativeHandle(), &table, vkDynamicCommand);
  dynamicEncoder.setPipeline(dynamicPipeline);
  expectRejectedWithoutRecording(
    "transactional setDynamicBuffer stale",
    [&] { dynamicEncoder.setDynamicBuffer(
      ShaderStage::allGraphics, 0, staleDynamicBuffer, 35, 4); });
  dynamicEncoder.setDynamicBuffer(
    ShaderStage::allGraphics, 0, dynamicBuffer, 37, 4);
  dynamicEncoder.setArgumentTable(ShaderStage::fragment, 0, dynamicTable);
  dynamicCommand->end();
  const SubmissionToken dynamicToken = submitOne(harness, *dynamicCommand);
  const BufferHotRecord* dynamicHot = table.tryGetBufferHot(dynamicBuffer);
  require(dynamicHot != nullptr && dynamicHot->pendingUses.count == 1 &&
            dynamicHot->pendingUses.tokens[0] == dynamicToken,
          "valid dynamic buffer did not record a retirement dependency");

  const BufferHandle saturatedBuffer = table.registerBuffer(
    BufferHotRecord{.nativeBuffer = fakeNativeHandle<VkBuffer>(0xD002)},
    BufferColdRecord{.desc = BufferDesc{.size = 64}});
  table.markBufferSubmitted(saturatedBuffer, SubmissionToken{{91, 1}, 1});
  table.markBufferSubmitted(saturatedBuffer, SubmissionToken{{92, 1}, 1});
  table.markBufferSubmitted(saturatedBuffer, SubmissionToken{{93, 1}, 1});
  std::unique_ptr<CommandBuffer> saturatedCommand = createCommand();
  ComputeEncoder* saturatedEncoder = saturatedCommand->beginComputePass();
  saturatedEncoder->fillBuffer(saturatedBuffer, 0, 4, 0);
  saturatedCommand->endEncoding();
  saturatedCommand->end();
  expectRejectedWithoutSubmit(
    "direct token-capacity preflight",
    [&] { (void)submitOne(harness, *saturatedCommand); });
  require(saturatedCommand->state() == CommandBufferState::executable,
          "rejected token-capacity submit changed command-buffer state");
  require(table.tryGetBufferHot(saturatedBuffer)->pendingUses.count == 3,
          "rejected token-capacity submit changed retirement dependencies");

  const TextureHandle staleParent = table.registerTexture(
    fakeNativeHandle<VkImage>(0xD010), TextureColdRecord{.owned = false});
  const TextureViewHandle staleParentView = table.registerTextureView(
    fakeNativeHandle<VkImageView>(0xD011),
    TextureViewColdRecord{.parentTexture = staleParent, .owned = false});
  std::unique_ptr<CommandBuffer> staleParentCommand = createCommand();
  const std::array<RenderTargetDesc, 1> staleParentTarget{{{
    .view = staleParentView,
  }}};
  staleParentCommand->beginRenderPass(RenderPassDesc{
    .renderArea = {.extent = {1, 1}},
    .colorTargets = staleParentTarget,
  });
  staleParentCommand->endEncoding();
  staleParentCommand->end();
  (void)table.removeTexture(staleParent);
  expectRejectedWithoutSubmit(
    "stale texture-view parent preflight",
    [&] { (void)submitOne(harness, *staleParentCommand); });
  require(table.tryGetTextureViewHot(staleParentView)->pendingUses.count == 0,
          "stale parent rejection partially marked its texture view");

  const TextureHandle saturatedParent = table.registerTexture(
    fakeNativeHandle<VkImage>(0xD012), TextureColdRecord{.owned = false});
  table.markTextureSubmitted(saturatedParent, SubmissionToken{{94, 1}, 1});
  table.markTextureSubmitted(saturatedParent, SubmissionToken{{95, 1}, 1});
  table.markTextureSubmitted(saturatedParent, SubmissionToken{{96, 1}, 1});
  const TextureViewHandle saturatedParentView = table.registerTextureView(
    fakeNativeHandle<VkImageView>(0xD013),
    TextureViewColdRecord{.parentTexture = saturatedParent, .owned = false});
  std::unique_ptr<CommandBuffer> saturatedParentCommand = createCommand();
  const std::array<RenderTargetDesc, 1> saturatedParentTarget{{{
    .view = saturatedParentView,
  }}};
  saturatedParentCommand->beginRenderPass(RenderPassDesc{
    .renderArea = {.extent = {1, 1}},
    .colorTargets = saturatedParentTarget,
  });
  saturatedParentCommand->endEncoding();
  saturatedParentCommand->end();
  expectRejectedWithoutSubmit(
    "texture-view parent token-capacity preflight",
    [&] { (void)submitOne(harness, *saturatedParentCommand); });
  require(table.tryGetTextureViewHot(saturatedParentView)->pendingUses.count == 0,
          "parent token-capacity rejection partially marked its texture view");
  require(table.tryGetTextureHot(saturatedParent)->pendingUses.count == 3,
          "parent token-capacity rejection changed parent dependencies");

  const PipelineHandle tablePipeline = table.registerPipeline(
    VK_PIPELINE_BIND_POINT_GRAPHICS,
    fakeNativeHandle<VkPipeline>(0xD020), 0,
    fakeNativeHandle<VkPipelineLayout>(0xD021), {}, false, false);
  const BufferHandle tableBuffer = table.registerBuffer(
    BufferHotRecord{.nativeBuffer = fakeNativeHandle<VkBuffer>(0xD022)},
    BufferColdRecord{.desc = BufferDesc{.size = 64}});
  const ArgumentTableHandle argumentTable = table.registerArgumentTable(
    fakeNativeHandle<VkDescriptorSet>(0xD023), {},
    ArgumentTableLifetime::persistent, false);
  table.recordArgumentTableResource(
    argumentTable, 0, 0, residencyResource(tableBuffer));
  std::unique_ptr<CommandBuffer> argumentCommand = createCommand();
  auto* vkArgumentCommand =
    dynamic_cast<VulkanCommandBuffer*>(argumentCommand.get());
  require(vkArgumentCommand != nullptr, "Vulkan command fixture has wrong type");
  VulkanRenderEncoder argumentEncoder;
  argumentEncoder.prepare(
    vkArgumentCommand->nativeHandle(), &table, vkArgumentCommand);
  argumentEncoder.setPipeline(tablePipeline);
  argumentEncoder.setArgumentTable(ShaderStage::fragment, 0, argumentTable);
  argumentCommand->end();
  (void)table.removeBuffer(tableBuffer);
  expectRejectedWithoutSubmit(
    "argument-table transitive stale-resource preflight",
    [&] { (void)submitOne(harness, *argumentCommand); });
  require(table.tryGetArgumentTable(argumentTable)->pendingUses.count == 0,
          "transitive stale-resource rejection partially marked its table");
  require(table.tryGetPipeline(tablePipeline)->pendingUses.count == 0,
          "transitive stale-resource rejection partially marked its pipeline");

  const BufferHandle optionalMeshBuffer = table.registerBuffer(
    BufferHotRecord{.nativeBuffer = fakeNativeHandle<VkBuffer>(0xD030)},
    BufferColdRecord{.desc = BufferDesc{.size = 64}});
  std::unique_ptr<CommandBuffer> optionalMeshCommand = createCommand();
  auto* vkOptionalMeshCommand =
    dynamic_cast<VulkanCommandBuffer*>(optionalMeshCommand.get());
  require(vkOptionalMeshCommand != nullptr, "Vulkan command fixture has wrong type");
  VulkanRenderEncoder optionalMeshEncoder;
  optionalMeshEncoder.prepare(
    vkOptionalMeshCommand->nativeHandle(), &table, vkOptionalMeshCommand);
  const PFN_vkCmdDrawMeshTasksIndirectEXT meshCallback =
    vkCmdDrawMeshTasksIndirectEXT;
  vkCmdDrawMeshTasksIndirectEXT = nullptr;
  try
  {
    optionalMeshEncoder.drawMeshTasksIndirect(
      DrawIndirectDesc{.argsBuffer = optionalMeshBuffer});
  }
  catch(...)
  {
    vkCmdDrawMeshTasksIndirectEXT = meshCallback;
    throw;
  }
  vkCmdDrawMeshTasksIndirectEXT = meshCallback;
  (void)table.removeBuffer(optionalMeshBuffer);
  optionalMeshCommand->end();
  const uint32_t beforeOptionalSubmit = g_probe.queueSubmits;
  (void)submitOne(harness, *optionalMeshCommand);
  require(g_probe.queueSubmits == beforeOptionalSubmit + 1,
          "callback-null mesh path retained a buffer it did not record");

  std::unique_ptr<CommandBuffer> duplicateCommand = createCommand();
  duplicateCommand->end();
  const std::array<CommandBuffer*, 2> duplicateCommands{{
    duplicateCommand.get(), duplicateCommand.get(),
  }};
  expectRejectedWithoutSubmit(
    "duplicate command-buffer batch",
    [&] { (void)harness.queue().submit(
      SubmitBatch{.commandBuffers = duplicateCommands}); });
  require(duplicateCommand->state() == CommandBufferState::executable,
          "duplicate batch rejection changed command-buffer state");

  QueueHarness otherQueue(table, QueueIdentity{82, 1});
  expectRejectedWithoutSubmit(
    "wrong allocator queue identity",
    [&] { (void)submitOne(otherQueue, *duplicateCommand); });
  require(duplicateCommand->state() == CommandBufferState::executable,
          "wrong-queue rejection changed command-buffer state");
}

void testUnregisteredCommandBufferSubmission()
{
  using namespace demo::rhi;
  using namespace demo::rhi::vulkan;

  VulkanResourceTable table;
  QueueHarness harness(table, QueueIdentity{83, 1});
  const BufferHandle buffer = table.registerBuffer(
    BufferHotRecord{.nativeBuffer = fakeNativeHandle<VkBuffer>(0xE100)},
    BufferColdRecord{.desc = BufferDesc{.size = 64}});
  const uint32_t freesBefore = g_probe.commandBufferFrees;

  {
    VulkanCommandBuffer commandBuffer(
      harness.allocator(), fakeNativeHandle<VkCommandBuffer>(0xE101), &table);
    require(!harness.allocator().owns(commandBuffer),
            "direct command buffer unexpectedly joined allocator ownership");
    commandBuffer.begin(harness.allocator());
    ComputeEncoder* encoder = commandBuffer.beginComputePass();
    encoder->fillBuffer(buffer, 0, 4, 0);
    commandBuffer.endEncoding();
    commandBuffer.end();

    expectRejectedWithoutSubmit(
      "allocator-unregistered command buffer",
      [&] { (void)submitOne(harness, commandBuffer); });
    require(commandBuffer.state() == CommandBufferState::executable,
            "unregistered rejection changed command-buffer state");
    const BufferHotRecord* hot = table.tryGetBufferHot(buffer);
    require(hot != nullptr && hot->pendingUses.count == 0,
            "unregistered rejection changed resource retirement state");
    require(!harness.queue().lastSubmittedToken().isValid(),
            "unregistered rejection advanced the queue token");

    harness.allocator().reset({});
    require(commandBuffer.state() == CommandBufferState::executable,
            "allocator reset mutated an unowned command buffer");
  }

  require(g_probe.commandBufferFrees == freesBefore,
          "unregistered command-buffer destruction freed allocator storage");
}

void testPipelineCapacityStateRollback()
{
  using namespace demo::rhi;
  using namespace demo::rhi::vulkan;

  VulkanResourceTable table;
  const VkDescriptorSet nativeSet =
    fakeNativeHandle<VkDescriptorSet>(0xE200);
  const ArgumentTableHandle argumentTable = table.registerArgumentTable(
    nativeSet, {}, ArgumentTableLifetime::persistent, false);
  const std::array<std::byte, 4> constants{};
  const auto bindingsFor = [](uint32_t slot) {
    return std::vector<PipelineRecord::RootBindingLowering>{{
      .slot = slot,
      .offset = 0,
      .size = 4,
      .kind = static_cast<uint32_t>(RootBindingKind::constants),
    }};
  };

  VulkanCommandBuffer renderCommand;
  renderCommand.setTarget(fakeNativeHandle<VkCommandBuffer>(0xE201), &table);
  VulkanRenderEncoder renderEncoder;
  renderEncoder.prepare(
    renderCommand.nativeHandle(), &table, &renderCommand);
  VkPipelineLayout acceptedRenderLayout = VK_NULL_HANDLE;
  for(uint32_t index = 0; index < 64; ++index)
  {
    acceptedRenderLayout =
      fakeNativeHandle<VkPipelineLayout>(0xE300 + index);
    const PipelineHandle pipeline = table.registerPipeline(
      VK_PIPELINE_BIND_POINT_GRAPHICS,
      fakeNativeHandle<VkPipeline>(0xE400 + index), 0,
      acceptedRenderLayout, bindingsFor(7), false, false);
    renderEncoder.setPipeline(pipeline);
  }
  const PipelineHandle rejectedRenderPipeline = table.registerPipeline(
    VK_PIPELINE_BIND_POINT_GRAPHICS,
    fakeNativeHandle<VkPipeline>(0xE500), 0,
    fakeNativeHandle<VkPipelineLayout>(0xE501), bindingsFor(8), false, false);
  expectRejectedWithoutRecording(
    "render 65th unique pipeline",
    [&] { renderEncoder.setPipeline(rejectedRenderPipeline); });

  const uint32_t beforeRenderTable = g_probe.callbacks;
  resetProbePayload();
  renderEncoder.setArgumentTable(
    ShaderStage::fragment, 0, argumentTable);
  require(g_probe.callbacks == beforeRenderTable + 1 &&
            g_probe.descriptorSet == nativeSet &&
            g_probe.pipelineLayout == acceptedRenderLayout,
          "render pipeline rejection leaked its layout into argument-table state");
  const uint32_t beforeRenderConstants = g_probe.callbacks;
  resetProbePayload();
  renderEncoder.setRootConstants(
    ShaderStage::fragment, 7, constants);
  require(g_probe.callbacks == beforeRenderConstants + 1 &&
            g_probe.pipelineLayout == acceptedRenderLayout,
          "render pipeline rejection leaked its handle into root-binding state");

  VulkanCommandBuffer computeCommand;
  computeCommand.setTarget(fakeNativeHandle<VkCommandBuffer>(0xE202), &table);
  VulkanComputeEncoder computeEncoder;
  computeEncoder.prepare(
    computeCommand.nativeHandle(), &table, &computeCommand);
  VkPipelineLayout acceptedComputeLayout = VK_NULL_HANDLE;
  for(uint32_t index = 0; index < 64; ++index)
  {
    acceptedComputeLayout =
      fakeNativeHandle<VkPipelineLayout>(0xE600 + index);
    const PipelineHandle pipeline = table.registerPipeline(
      VK_PIPELINE_BIND_POINT_COMPUTE,
      fakeNativeHandle<VkPipeline>(0xE700 + index), 0,
      acceptedComputeLayout, bindingsFor(7), false, false);
    computeEncoder.setPipeline(pipeline);
  }
  const PipelineHandle rejectedComputePipeline = table.registerPipeline(
    VK_PIPELINE_BIND_POINT_COMPUTE,
    fakeNativeHandle<VkPipeline>(0xE800), 0,
    fakeNativeHandle<VkPipelineLayout>(0xE801), bindingsFor(8), false, false);
  expectRejectedWithoutRecording(
    "compute 65th unique pipeline",
    [&] { computeEncoder.setPipeline(rejectedComputePipeline); });

  const uint32_t beforeComputeTable = g_probe.callbacks;
  resetProbePayload();
  computeEncoder.setArgumentTable(0, argumentTable);
  require(g_probe.callbacks == beforeComputeTable + 1 &&
            g_probe.descriptorSet == nativeSet &&
            g_probe.pipelineLayout == acceptedComputeLayout,
          "compute pipeline rejection leaked its layout into argument-table state");
  const uint32_t beforeComputeConstants = g_probe.callbacks;
  resetProbePayload();
  computeEncoder.setRootConstants(7, constants);
  require(g_probe.callbacks == beforeComputeConstants + 1 &&
            g_probe.pipelineLayout == acceptedComputeLayout,
          "compute pipeline rejection leaked its handle into root-binding state");
}

void runTests()
{
  using namespace demo::rhi;
  using namespace demo::rhi::vulkan;

  ScopedCommandCallbacks callbacks;
  VulkanResourceTable table;
  VulkanResourceTable foreignTable;

  const BufferHandle stale = table.registerBuffer(
    BufferHotRecord{.nativeBuffer = fakeNativeHandle<VkBuffer>(0x301)},
    BufferColdRecord{.desc = BufferDesc{.size = 64}});
  (void)table.removeBuffer(stale);

  const VkBuffer native = fakeNativeHandle<VkBuffer>(0x302);
  const BufferHandle valid = table.registerBuffer(
    BufferHotRecord{.nativeBuffer = native},
    BufferColdRecord{.desc = BufferDesc{.size = 64}});
  require(valid.index == stale.index && valid.generation != stale.generation,
          "stale fixture did not exercise a reused slot with a new generation");

  const VkBuffer otherNative = fakeNativeHandle<VkBuffer>(0x303);
  const BufferHandle other = table.registerBuffer(
    BufferHotRecord{.nativeBuffer = otherNative},
    BufferColdRecord{.desc = BufferDesc{.size = 64}});
  const BufferHandle foreignBuffer = foreignTable.registerBuffer(
    BufferHotRecord{.nativeBuffer = fakeNativeHandle<VkBuffer>(0x304)},
    BufferColdRecord{.desc = BufferDesc{.size = 64}});
  const TextureHandle staleTexture = table.registerTexture(
    fakeNativeHandle<VkImage>(0x305), TextureColdRecord{.owned = false});
  (void)table.removeTexture(staleTexture);
  const VkImage nativeImage = fakeNativeHandle<VkImage>(0x306);
  const TextureHandle texture = table.registerTexture(
    nativeImage, TextureColdRecord{.owned = false});
  require(texture.index == staleTexture.index &&
          texture.generation != staleTexture.generation,
          "stale texture fixture did not reuse a slot with a new generation");
  const VkImage otherImage = fakeNativeHandle<VkImage>(0x307);
  const TextureHandle otherTexture = table.registerTexture(
    otherImage, TextureColdRecord{.owned = false});
  const TextureHandle foreignTexture = foreignTable.registerTexture(
    fakeNativeHandle<VkImage>(0x308), TextureColdRecord{.owned = false});

  const TextureViewHandle staleView = table.registerTextureView(
    fakeNativeHandle<VkImageView>(0x308),
    TextureViewColdRecord{.parentTexture = texture, .owned = false});
  (void)table.removeTextureView(staleView);
  const VkImageView nativeView = fakeNativeHandle<VkImageView>(0x309);
  const TextureViewHandle validView = table.registerTextureView(
    nativeView,
    TextureViewColdRecord{.parentTexture = texture, .owned = false});
  require(validView.index == staleView.index &&
          validView.generation != staleView.generation,
          "stale texture-view fixture did not reuse a slot with a new generation");
  const TextureViewHandle foreignView = foreignTable.registerTextureView(
    fakeNativeHandle<VkImageView>(0x30A),
    TextureViewColdRecord{.parentTexture = foreignTexture, .owned = false});
  const TextureHandle staleParent = table.registerTexture(
    fakeNativeHandle<VkImage>(0x30B), TextureColdRecord{.owned = false});
  const TextureViewHandle viewWithStaleParent = table.registerTextureView(
    fakeNativeHandle<VkImageView>(0x30C),
    TextureViewColdRecord{.parentTexture = staleParent, .owned = false});
  (void)table.removeTexture(staleParent);

  const PipelineHandle stalePipeline = table.registerPipeline(
    VK_PIPELINE_BIND_POINT_GRAPHICS, fakeNativeHandle<VkPipeline>(0x30D), 0,
    fakeNativeHandle<VkPipelineLayout>(0x30E), {}, false, false);
  table.destroyPipeline(stalePipeline);
  const VkPipeline nativeGraphics = fakeNativeHandle<VkPipeline>(0x30F);
  const PipelineHandle graphicsPipeline = table.registerPipeline(
    VK_PIPELINE_BIND_POINT_GRAPHICS, nativeGraphics, 0,
    fakeNativeHandle<VkPipelineLayout>(0x310), {}, false, false);
  require(graphicsPipeline.index == stalePipeline.index &&
          graphicsPipeline.generation != stalePipeline.generation,
          "stale pipeline fixture did not reuse a slot with a new generation");
  const VkPipeline nativeCompute = fakeNativeHandle<VkPipeline>(0x311);
  const PipelineHandle computePipeline = table.registerPipeline(
    VK_PIPELINE_BIND_POINT_COMPUTE, nativeCompute, 0,
    fakeNativeHandle<VkPipelineLayout>(0x312), {}, false, false);
  const PipelineHandle foreignPipeline = foreignTable.registerPipeline(
    VK_PIPELINE_BIND_POINT_GRAPHICS, fakeNativeHandle<VkPipeline>(0x313), 0,
    fakeNativeHandle<VkPipelineLayout>(0x314), {}, false, false);

  const ArgumentTableHandle staleArgumentTable = table.registerArgumentTable(
    fakeNativeHandle<VkDescriptorSet>(0x315), {},
    ArgumentTableLifetime::persistent, false);
  (void)table.removeArgumentTable(staleArgumentTable);
  const VkDescriptorSet nativeSet = fakeNativeHandle<VkDescriptorSet>(0x316);
  const ArgumentTableHandle argumentTable = table.registerArgumentTable(
    nativeSet, {}, ArgumentTableLifetime::persistent, false);
  require(argumentTable.index == staleArgumentTable.index &&
          argumentTable.generation != staleArgumentTable.generation,
          "stale argument-table fixture did not reuse a slot with a new generation");
  const ArgumentTableHandle foreignArgumentTable =
    foreignTable.registerArgumentTable(
      fakeNativeHandle<VkDescriptorSet>(0x317), {},
      ArgumentTableLifetime::persistent, false);
  const SamplerHandle staleSampler = table.registerSampler(
    fakeNativeHandle<VkSampler>(0x318));
  const ArgumentTableHandle tableWithStaleSampler = table.registerArgumentTable(
    fakeNativeHandle<VkDescriptorSet>(0x319), {},
    ArgumentTableLifetime::persistent, false);
  table.recordArgumentTableResource(
    tableWithStaleSampler, 0, 0,
    ResidencyResource{
      ResidencyResourceKind::sampler, staleSampler.index, staleSampler.generation});
  (void)table.removeSampler(staleSampler);
  const TextureViewHandle staleArgumentView = table.registerTextureView(
    fakeNativeHandle<VkImageView>(0x31A),
    TextureViewColdRecord{.parentTexture = texture, .owned = false});
  const ArgumentTableHandle tableWithStaleView = table.registerArgumentTable(
    fakeNativeHandle<VkDescriptorSet>(0x31B), {},
    ArgumentTableLifetime::persistent, false);
  table.recordArgumentTableResource(
    tableWithStaleView, 0, 0, residencyResource(staleArgumentView));
  (void)table.removeTextureView(staleArgumentView);
  const SamplerHandle validSampler = table.registerSampler(
    fakeNativeHandle<VkSampler>(0x31C));
  table.recordArgumentTableResource(
    argumentTable, 0, 0,
    ResidencyResource{
      ResidencyResourceKind::sampler, validSampler.index, validSampler.generation});
  table.recordArgumentTableResource(
    argumentTable, 1, 0, residencyResource(validView));

  const QueryPoolHandle staleQueryPool = table.registerQueryPool(
    fakeNativeHandle<VkQueryPool>(0x31D), 4);
  (void)table.removeQueryPool(staleQueryPool);
  const VkQueryPool nativeQueryPool = fakeNativeHandle<VkQueryPool>(0x31E);
  const QueryPoolHandle queryPool = table.registerQueryPool(nativeQueryPool, 4);
  require(queryPool.index == staleQueryPool.index &&
          queryPool.generation != staleQueryPool.generation,
          "stale query-pool fixture did not reuse a slot with a new generation");
  const QueryPoolHandle foreignQueryPool = foreignTable.registerQueryPool(
    fakeNativeHandle<VkQueryPool>(0x31F), 4);

  testRenderIndirectPaths(
    table, valid, other, stale, native, otherNative);
  testComputeAndTransferPaths(
    table, valid, other, stale, texture, native, otherNative);
  testTextureTransferPaths(
    table, valid, texture, otherTexture, staleTexture, foreignTexture,
    native, nativeImage, otherImage);
  testTextureViewPaths(
    table, validView, staleView, foreignView, viewWithStaleParent,
    texture, otherTexture, staleTexture, nativeView);
  testPipelineAndArgumentTablePaths(
    table, valid, stale, foreignBuffer,
    graphicsPipeline, computePipeline, stalePipeline, foreignPipeline,
    argumentTable, staleArgumentTable, foreignArgumentTable,
    tableWithStaleSampler, tableWithStaleView,
    nativeGraphics, nativeCompute, nativeSet);
  testTextureAndQueryCommandPaths(
    table, texture, staleTexture, foreignTexture, nativeImage,
    queryPool, staleQueryPool, foreignQueryPool, nativeQueryPool);
  testSubmissionTransactions();
  testUnregisteredCommandBufferSubmission();
  testPipelineCapacityStateRollback();
}

}  // namespace

int main()
{
  try
  {
    runTests();
  }
  catch(const std::exception& error)
  {
    std::cerr << "vulkan command-buffer handle test failed: " << error.what() << '\n';
    return 1;
  }
  std::cout << "vulkan command-buffer handle tests passed\n";
  return 0;
}
