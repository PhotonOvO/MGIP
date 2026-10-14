#include "rhi/vulkan/VulkanCommandAllocator.h"
#include "rhi/vulkan/VulkanCommandBuffer.h"
#include "rhi/vulkan/VulkanQueue.h"
#include "rhi/vulkan/VulkanResourceTable.h"

#include "volk.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace allocation_failure {

thread_local long long countdown = -1;
thread_local long long observedCalls = 0;

[[nodiscard]] bool isCheckedStlProxyAllocation(std::size_t size) noexcept
{
#if defined(_MSC_VER) && defined(_ITERATOR_DEBUG_LEVEL) && \
  _ITERATOR_DEBUG_LEVEL != 0
  return size == 2u * sizeof(void*);
#else
  (void)size;
  return false;
#endif
}

class ScopedFailure
{
public:
  explicit ScopedFailure(long long ordinal) noexcept
  {
    countdown = ordinal;
    observedCalls = 0;
  }

  ~ScopedFailure()
  {
    countdown = -1;
  }

  ScopedFailure(const ScopedFailure&) = delete;
  ScopedFailure& operator=(const ScopedFailure&) = delete;
};

}  // namespace allocation_failure

// This replacement is isolated to this test executable and remains inactive
// except while ScopedFailure brackets one production operation.
void* operator new(std::size_t size)
{
  if(allocation_failure::countdown > 0 &&
     !allocation_failure::isCheckedStlProxyAllocation(size))
  {
    ++allocation_failure::observedCalls;
    --allocation_failure::countdown;
    if(allocation_failure::countdown == 0)
      throw std::bad_alloc{};
  }

  if(void* allocation = std::malloc(size == 0 ? 1 : size))
    return allocation;
  throw std::bad_alloc{};
}

void operator delete(void* allocation) noexcept
{
  std::free(allocation);
}

void operator delete(void* allocation, std::size_t) noexcept
{
  std::free(allocation);
}

namespace {

uint32_t g_nativeAllocations = 0;
uint32_t g_nativeFrees = 0;

template <typename NativeHandle>
NativeHandle fakeNativeHandle(uintptr_t value)
{
  if constexpr(std::is_pointer_v<NativeHandle>)
    return reinterpret_cast<NativeHandle>(value);
  else
    return static_cast<NativeHandle>(value);
}

void require(bool condition, const char* message)
{
  if(!condition)
    throw std::runtime_error(message);
}

[[nodiscard]] uint32_t slotGeneration(uint32_t encodedGeneration) noexcept
{
  return encodedGeneration & 0xffffu;
}

VKAPI_ATTR VkResult VKAPI_CALL recordCreateSemaphore(
  VkDevice, const VkSemaphoreCreateInfo*, const VkAllocationCallbacks*,
  VkSemaphore* semaphore)
{
  *semaphore = fakeNativeHandle<VkSemaphore>(0x7101u);
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
  *pool = fakeNativeHandle<VkCommandPool>(0x7102u);
  return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL recordDestroyCommandPool(
  VkDevice, VkCommandPool, const VkAllocationCallbacks*)
{
}

VKAPI_ATTR VkResult VKAPI_CALL recordAllocateCommandBuffers(
  VkDevice, const VkCommandBufferAllocateInfo* info, VkCommandBuffer* buffers)
{
  ++g_nativeAllocations;
  for(uint32_t index = 0; index < info->commandBufferCount; ++index)
  {
    buffers[index] = fakeNativeHandle<VkCommandBuffer>(
      0x7200u + static_cast<uintptr_t>(g_nativeAllocations + index));
  }
  return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL recordFreeCommandBuffers(
  VkDevice, VkCommandPool, uint32_t count, const VkCommandBuffer*)
{
  g_nativeFrees += count;
}

class ScopedVulkanCallbacks
{
public:
  ScopedVulkanCallbacks()
    : m_createSemaphore(vkCreateSemaphore)
    , m_destroySemaphore(vkDestroySemaphore)
    , m_createCommandPool(vkCreateCommandPool)
    , m_destroyCommandPool(vkDestroyCommandPool)
    , m_allocateCommandBuffers(vkAllocateCommandBuffers)
    , m_freeCommandBuffers(vkFreeCommandBuffers)
  {
    vkCreateSemaphore = &recordCreateSemaphore;
    vkDestroySemaphore = &recordDestroySemaphore;
    vkCreateCommandPool = &recordCreateCommandPool;
    vkDestroyCommandPool = &recordDestroyCommandPool;
    vkAllocateCommandBuffers = &recordAllocateCommandBuffers;
    vkFreeCommandBuffers = &recordFreeCommandBuffers;
  }

  ~ScopedVulkanCallbacks()
  {
    vkCreateSemaphore = m_createSemaphore;
    vkDestroySemaphore = m_destroySemaphore;
    vkCreateCommandPool = m_createCommandPool;
    vkDestroyCommandPool = m_destroyCommandPool;
    vkAllocateCommandBuffers = m_allocateCommandBuffers;
    vkFreeCommandBuffers = m_freeCommandBuffers;
  }

  ScopedVulkanCallbacks(const ScopedVulkanCallbacks&) = delete;
  ScopedVulkanCallbacks& operator=(const ScopedVulkanCallbacks&) = delete;

private:
  PFN_vkCreateSemaphore m_createSemaphore;
  PFN_vkDestroySemaphore m_destroySemaphore;
  PFN_vkCreateCommandPool m_createCommandPool;
  PFN_vkDestroyCommandPool m_destroyCommandPool;
  PFN_vkAllocateCommandBuffers m_allocateCommandBuffers;
  PFN_vkFreeCommandBuffers m_freeCommandBuffers;
};

[[nodiscard]] demo::rhi::vulkan::BufferHotRecord makeBufferHot(uintptr_t value)
{
  return demo::rhi::vulkan::BufferHotRecord{
    .nativeBuffer = fakeNativeHandle<VkBuffer>(value),
  };
}

[[nodiscard]] uint32_t countBuffers(
  demo::rhi::vulkan::VulkanResourceTable& table)
{
  uint32_t count = 0;
  table.forEachBuffer(
    [&](demo::rhi::BufferHandle, demo::rhi::vulkan::BufferHotRecord&) {
      ++count;
    });
  return count;
}

[[nodiscard]] uint32_t countTextures(
  demo::rhi::vulkan::VulkanResourceTable& table)
{
  uint32_t count = 0;
  table.forEachTexture(
    [&](demo::rhi::TextureHandle, demo::rhi::vulkan::TextureHotRecord&) {
      ++count;
    });
  return count;
}

[[nodiscard]] uint32_t countTextureViews(
  demo::rhi::vulkan::VulkanResourceTable& table)
{
  uint32_t count = 0;
  table.forEachTextureView(
    [&](demo::rhi::TextureViewHandle,
        demo::rhi::vulkan::TextureViewHotRecord&) {
      ++count;
    });
  return count;
}

void testBufferColdResizeRollback()
{
  using namespace demo::rhi;
  using namespace demo::rhi::vulkan;

  VulkanResourceTable table;
  bool caught = false;
  try
  {
    allocation_failure::ScopedFailure failure{2};
    (void)table.registerBuffer(makeBufferHot(0x7301u), BufferColdRecord{});
  }
  catch(const std::bad_alloc&)
  {
    caught = true;
  }

  require(caught, "buffer cold resize did not rethrow std::bad_alloc");
  require(allocation_failure::observedCalls == 2,
          "buffer fault did not occur at the deterministic cold resize");
  require(countBuffers(table) == 0,
          "buffer cold resize failure retained an active hot handle");

  const VkBuffer native = fakeNativeHandle<VkBuffer>(0x7302u);
  const BufferHandle next = table.registerBuffer(
    makeBufferHot(0x7302u),
    BufferColdRecord{.debugName = std::string{"buffer-coherent"}});
  require(next.index == 1u && slotGeneration(next.generation) == 2u,
          "buffer rollback did not preserve the next reusable slot");

  const BufferHandle rolledBack{next.index, next.generation - 1u};
  require(table.resolveBuffer(rolledBack) == VK_NULL_HANDLE &&
            table.tryGetBufferHot(rolledBack) == nullptr &&
            table.tryGetBufferCold(rolledBack) == nullptr,
          "rolled-back buffer handle remained resolvable");
  const BufferColdRecord* cold = table.tryGetBufferCold(next);
  require(countBuffers(table) == 1 && table.resolveBuffer(next) == native &&
            cold != nullptr && cold->debugName == "buffer-coherent",
          "buffer registration after rollback is incoherent");

  const BufferRecord removed = table.removeBuffer(next);
  require(removed.hot.nativeBuffer == native &&
            removed.cold.debugName == "buffer-coherent" &&
            countBuffers(table) == 0 &&
            table.resolveBuffer(next) == VK_NULL_HANDLE,
          "buffer lookup/count became inconsistent after reusable-slot removal");
}

void testTextureColdResizeRollback()
{
  using namespace demo::rhi;
  using namespace demo::rhi::vulkan;

  VulkanResourceTable table;
  bool caught = false;
  try
  {
    allocation_failure::ScopedFailure failure{2};
    (void)table.registerTexture(
      fakeNativeHandle<VkImage>(0x7401u), TextureColdRecord{});
  }
  catch(const std::bad_alloc&)
  {
    caught = true;
  }

  require(caught, "texture cold resize did not rethrow std::bad_alloc");
  require(allocation_failure::observedCalls == 2,
          "texture fault did not occur at the deterministic cold resize");
  require(countTextures(table) == 0,
          "texture cold resize failure retained an active hot handle");

  const VkImage native = fakeNativeHandle<VkImage>(0x7402u);
  const TextureHandle next = table.registerTexture(
    native, TextureColdRecord{.debugName = std::string{"texture-coherent"}});
  require(next.index == 1u && slotGeneration(next.generation) == 2u,
          "texture rollback did not preserve the next reusable slot");

  const TextureHandle rolledBack{next.index, next.generation - 1u};
  require(table.resolveTexture(rolledBack) == VK_NULL_HANDLE &&
            table.tryGetTextureHot(rolledBack) == nullptr &&
            table.tryGetTextureCold(rolledBack) == nullptr,
          "rolled-back texture handle remained resolvable");
  const TextureColdRecord* cold = table.tryGetTextureCold(next);
  require(countTextures(table) == 1 && table.resolveTexture(next) == native &&
            cold != nullptr && cold->debugName == "texture-coherent",
          "texture registration after rollback is incoherent");

  const TextureRecord removed = table.removeTexture(next);
  require(removed.hot.nativeImage == native &&
            removed.cold.debugName == "texture-coherent" &&
            countTextures(table) == 0 &&
            table.resolveTexture(next) == VK_NULL_HANDLE,
          "texture lookup/count became inconsistent after reusable-slot removal");
}

void testTextureViewColdResizeRollback()
{
  using namespace demo::rhi;
  using namespace demo::rhi::vulkan;

  VulkanResourceTable table;
  bool caught = false;
  try
  {
    allocation_failure::ScopedFailure failure{2};
    (void)table.registerTextureView(
      fakeNativeHandle<VkImageView>(0x7501u), TextureViewColdRecord{});
  }
  catch(const std::bad_alloc&)
  {
    caught = true;
  }

  require(caught, "texture-view cold resize did not rethrow std::bad_alloc");
  require(allocation_failure::observedCalls == 2,
          "texture-view fault did not occur at the deterministic cold resize");
  require(countTextureViews(table) == 0,
          "texture-view cold resize failure retained an active hot handle");

  const VkImageView native = fakeNativeHandle<VkImageView>(0x7502u);
  const TextureViewHandle next = table.registerTextureView(
    native,
    TextureViewColdRecord{.debugName = std::string{"view-coherent"}});
  require(next.index == 1u && slotGeneration(next.generation) == 2u,
          "texture-view rollback did not preserve the next reusable slot");

  const TextureViewHandle rolledBack{next.index, next.generation - 1u};
  require(table.resolveTextureView(rolledBack) == VK_NULL_HANDLE &&
            table.tryGetTextureViewHot(rolledBack) == nullptr &&
            table.tryGetTextureViewCold(rolledBack) == nullptr,
          "rolled-back texture-view handle remained resolvable");
  const TextureViewColdRecord* cold = table.tryGetTextureViewCold(next);
  require(countTextureViews(table) == 1 &&
            table.resolveTextureView(next) == native && cold != nullptr &&
            cold->debugName == "view-coherent",
          "texture-view registration after rollback is incoherent");

  const TextureViewRecord removed = table.removeTextureView(next);
  require(removed.hot.nativeView == native &&
            removed.cold.debugName == "view-coherent" &&
            countTextureViews(table) == 0 &&
            table.resolveTextureView(next) == VK_NULL_HANDLE,
          "texture-view lookup/count became inconsistent after reusable-slot removal");
}

void testCommandAllocatorCreationRollback()
{
  using namespace demo::rhi;
  using namespace demo::rhi::vulkan;

  ScopedVulkanCallbacks callbacks;
  g_nativeAllocations = 0;
  g_nativeFrees = 0;

  VulkanResourceTable table;
  VulkanQueueSyncRegistry syncRegistry;
  VulkanQueue queue;
  queue.init(
    fakeNativeHandle<VkDevice>(0x7601u),
    fakeNativeHandle<VkQueue>(0x7602u), 0u, 0u,
    QueueIdentity{401u, 1u},
    QueueInfo{.queueClass = QueueClass::graphics, .available = true},
    &syncRegistry);

  for(long long ordinal = 1; ordinal <= 5; ++ordinal)
  {
    VulkanCommandAllocator allocator(
      queue, fakeNativeHandle<VkDevice>(0x7601u), &table);
    const uint32_t allocationsBefore = g_nativeAllocations;
    const uint32_t freesBefore = g_nativeFrees;
    bool caught = false;
    try
    {
      allocation_failure::ScopedFailure failure{ordinal};
      std::unique_ptr<CommandBuffer> unexpected =
        allocator.createCommandBuffer();
      unexpected.reset();
    }
    catch(const std::bad_alloc&)
    {
      caught = true;
    }

    require(caught,
            "command-buffer creation did not rethrow the injected std::bad_alloc");
    require(allocation_failure::observedCalls == ordinal,
            "command-buffer allocation ordinal was not deterministic");
    const uint32_t allocated = g_nativeAllocations - allocationsBefore;
    const uint32_t freed = g_nativeFrees - freesBefore;
    if(ordinal == 1)
    {
      require(allocated == 0 && freed == 0,
              "registry reserve failure reached native command-buffer allocation");
    }
    else
    {
      require(allocated == 1 && freed == 1,
              "post-native wrapper failure did not free exactly once");
    }

    const uint32_t recoveryAllocationsBefore = g_nativeAllocations;
    const uint32_t recoveryFreesBefore = g_nativeFrees;
    std::unique_ptr<CommandBuffer> recovered = allocator.createCommandBuffer();
    require(g_nativeAllocations == recoveryAllocationsBefore + 1 &&
              g_nativeFrees == recoveryFreesBefore,
            "allocator recovery did not retain one successful native allocation");
    recovered.reset();
    require(g_nativeFrees == recoveryFreesBefore + 1,
            "allocator recovery did not free exactly once through normal ownership");
  }

  const uint32_t allocationsBefore = g_nativeAllocations;
  const uint32_t freesBefore = g_nativeFrees;
  VulkanCommandAllocator allocator(
    queue, fakeNativeHandle<VkDevice>(0x7601u), &table);
  std::unique_ptr<CommandBuffer> success = allocator.createCommandBuffer();
  require(g_nativeAllocations == allocationsBefore + 1 &&
            g_nativeFrees == freesBefore,
          "successful command-buffer creation freed native storage early");
  success.reset();
  require(g_nativeFrees == freesBefore + 1,
          "successful command buffer did not free exactly once on destruction");
}

void runTests()
{
  std::fputs("allocation regression: buffer rollback\n", stderr);
  testBufferColdResizeRollback();
  std::fputs("allocation regression: texture rollback\n", stderr);
  testTextureColdResizeRollback();
  std::fputs("allocation regression: texture-view rollback\n", stderr);
  testTextureViewColdResizeRollback();
  std::fputs("allocation regression: command allocator rollback\n", stderr);
  testCommandAllocatorCreationRollback();
  std::fputs("allocation regression: complete\n", stderr);
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
    allocation_failure::countdown = -1;
    std::cerr << "vulkan allocation-failure test failed: " << error.what()
              << '\n';
    return EXIT_FAILURE;
  }

#ifdef NDEBUG
  std::cout << "vulkan allocation-failure regressions passed (NDEBUG)\n";
#else
  std::cout << "vulkan allocation-failure regressions passed (Debug)\n";
#endif
  return EXIT_SUCCESS;
}
