# RHI Native Interface Encapsulation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Encapsulate all native Vulkan interfaces in RHI layer, enabling Pass layer to use only RHI types.

**Architecture:** Extend CommandList interface with index buffer and BindGroup methods; refactor BindTableLayout/BindTable into unified BindGroupLayout/BindGroup; Pipeline internally manages PipelineLayout; Renderer exposes only RHI handles.

**Tech Stack:** C++20, Vulkan 1.3, existing RHI abstraction layer

---

## Phase 1: RHI Type Extensions

### Task 1: Add IndexFormat enum to RHITypes.h

**Files:**
- Modify: `rhi/RHITypes.h:248` (after VertexFormat enum)

- [ ] **Step 1: Add IndexFormat enum**

Add after `VertexFormat` enum (around line 264):

```cpp
enum class IndexFormat : uint8_t
{
  uint16 = 0,
  uint32 = 1,
};
```

- [ ] **Step 2: Build to verify**

Run: `cmake --build build --target Demo`
Expected: Build succeeds

- [ ] **Step 3: Commit**

```bash
git add rhi/RHITypes.h
git commit -m "feat(rhi): add IndexFormat enum for index buffer abstraction"
```

---

### Task 2: Add TextureViewDesc to RHICommandList.h

**Files:**
- Modify: `rhi/RHICommandList.h:34` (after TextureBarrierDesc)

- [ ] **Step 1: Add TextureViewDesc struct**

Add after `TextureBarrierDesc` struct:

```cpp
struct TextureViewDesc
{
  TextureHandle  texture{};
  TextureAspect  aspect{TextureAspect::color};
  uint32_t       baseMipLevel{0};
  uint32_t       mipLevelCount{1};
  uint32_t       baseArrayLayer{0};
  uint32_t       arrayLayerCount{1};
};
```

- [ ] **Step 2: Build to verify**

Run: `cmake --build build --target Demo`
Expected: Build succeeds

- [ ] **Step 3: Commit**

```bash
git add rhi/RHICommandList.h
git commit -m "feat(rhi): add TextureViewDesc for texture view creation"
```

---

### Task 3: Update RenderTargetDesc with TextureViewHandle

**Files:**
- Modify: `rhi/RHICommandList.h:36-43` (RenderTargetDesc and DepthTargetDesc)

- [ ] **Step 1: Add TextureViewHandle to RenderTargetDesc**

Replace the `RenderTargetDesc` struct:

```cpp
struct RenderTargetDesc
{
  TextureHandle     texture{};
  TextureViewHandle view{};            // Texture view for rendering
  ResourceState     state{ResourceState::general};
  LoadOp            loadOp{LoadOp::load};
  StoreOp           storeOp{StoreOp::store};
  ClearColorValue   clearColor{};
};
```

- [ ] **Step 2: Add TextureViewHandle to DepthTargetDesc**

Replace the `DepthTargetDesc` struct:

```cpp
struct DepthTargetDesc
{
  TextureHandle          texture{};
  TextureViewHandle      view{};            // Texture view for rendering
  ResourceState          state{ResourceState::general};
  LoadOp                 loadOp{LoadOp::load};
  StoreOp                storeOp{StoreOp::store};
  ClearDepthStencilValue clearValue{};
};
```

- [ ] **Step 3: Build to verify**

Run: `cmake --build build --target Demo`
Expected: Build succeeds (may have unused field warnings)

- [ ] **Step 4: Commit**

```bash
git add rhi/RHICommandList.h
git commit -m "feat(rhi): add TextureViewHandle to render target descriptors"
```

---

## Phase 2: CommandList Interface Extension

### Task 4: Add bindIndexBuffer to CommandList

**Files:**
- Modify: `rhi/RHICommandList.h:87` (after bindVertexBuffers)

- [ ] **Step 1: Add bindIndexBuffer virtual method**

Add after `bindVertexBuffers`:

```cpp
virtual void bindIndexBuffer(BufferHandle buffer, uint64_t offset, IndexFormat format) = 0;
```

- [ ] **Step 2: Build to verify**

Run: `cmake --build build --target Demo`
Expected: Build fails with "no member named 'bindIndexBuffer'" in VulkanCommandList

- [ ] **Step 3: Commit**

```bash
git add rhi/RHICommandList.h
git commit -m "feat(rhi): add bindIndexBuffer to CommandList interface"
```

---

### Task 5: Add drawIndexed to CommandList

**Files:**
- Modify: `rhi/RHICommandList.h:90` (after draw method)

- [ ] **Step 1: Add drawIndexed virtual method**

Add after the `draw` method:

```cpp
virtual void drawIndexed(uint32_t indexCount, uint32_t instanceCount,
                         uint32_t firstIndex, int32_t vertexOffset,
                         uint32_t firstInstance) = 0;
```

- [ ] **Step 2: Build to verify**

Run: `cmake --build build --target Demo`
Expected: Build fails with "no member named 'drawIndexed'" in VulkanCommandList

- [ ] **Step 3: Commit**

```bash
git add rhi/RHICommandList.h
git commit -m "feat(rhi): add drawIndexed to CommandList interface"
```

---

### Task 6: Add bindBindGroup to CommandList

**Files:**
- Modify: `rhi/RHICommandList.h:82-86` (modify bindBindTable or add new method)

- [ ] **Step 1: Check existing bindBindTable signature**

The existing `bindBindTable` uses `BindTableHandle`. Add a new method using `BindGroupHandle`:

```cpp
virtual void bindBindGroup(uint32_t slot, BindGroupHandle bindGroup,
                           const uint32_t* dynamicOffsets,
                           uint32_t dynamicOffsetCount) = 0;
```

- [ ] **Step 2: Build to verify**

Run: `cmake --build build --target Demo`
Expected: Build fails with "no member named 'bindBindGroup'" in VulkanCommandList

- [ ] **Step 3: Commit**

```bash
git add rhi/RHICommandList.h
git commit -m "feat(rhi): add bindBindGroup to CommandList interface"
```

---

### Task 7: Implement bindIndexBuffer in VulkanCommandList

**Files:**
- Modify: `rhi/vulkan/VulkanCommandList.h:35` (add declaration)
- Modify: `rhi/vulkan/VulkanCommandList.cpp:361` (add implementation)

- [ ] **Step 1: Add declaration to VulkanCommandList.h**

Add after `bindVertexBuffers` declaration:

```cpp
void bindIndexBuffer(BufferHandle buffer, uint64_t offset, IndexFormat format) override;
```

- [ ] **Step 2: Add implementation to VulkanCommandList.cpp**

Add after `bindVertexBuffers` implementation:

```cpp
void VulkanCommandList::bindIndexBuffer(BufferHandle buffer, uint64_t offset, IndexFormat format)
{
  ensureCommandBuffer(m_commandBuffer);
  const VkBuffer nativeBuffer = reinterpret_cast<VkBuffer>(buffer.index);
  const VkIndexType indexType = format == IndexFormat::uint32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
  vkCmdBindIndexBuffer(m_commandBuffer, nativeBuffer, offset, indexType);
}
```

- [ ] **Step 3: Build to verify**

Run: `cmake --build build --target Demo`
Expected: Build succeeds

- [ ] **Step 4: Commit**

```bash
git add rhi/vulkan/VulkanCommandList.h rhi/vulkan/VulkanCommandList.cpp
git commit -m "feat(rhi): implement bindIndexBuffer in VulkanCommandList"
```

---

### Task 8: Implement drawIndexed in VulkanCommandList

**Files:**
- Modify: `rhi/vulkan/VulkanCommandList.h:39` (add declaration)
- Modify: `rhi/vulkan/VulkanCommandList.cpp:371` (add implementation)

- [ ] **Step 1: Add declaration to VulkanCommandList.h**

Add after `draw` declaration:

```cpp
void drawIndexed(uint32_t indexCount, uint32_t instanceCount,
                 uint32_t firstIndex, int32_t vertexOffset,
                 uint32_t firstInstance) override;
```

- [ ] **Step 2: Add implementation to VulkanCommandList.cpp**

Add after `draw` implementation:

```cpp
void VulkanCommandList::drawIndexed(uint32_t indexCount, uint32_t instanceCount,
                                    uint32_t firstIndex, int32_t vertexOffset,
                                    uint32_t firstInstance)
{
  ensureCommandBuffer(m_commandBuffer);
  vkCmdDrawIndexed(m_commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}
```

- [ ] **Step 3: Build to verify**

Run: `cmake --build build --target Demo`
Expected: Build succeeds

- [ ] **Step 4: Commit**

```bash
git add rhi/vulkan/VulkanCommandList.h rhi/vulkan/VulkanCommandList.cpp
git commit -m "feat(rhi): implement drawIndexed in VulkanCommandList"
```

---

### Task 9: Implement bindBindGroup in VulkanCommandList

**Files:**
- Modify: `rhi/vulkan/VulkanCommandList.h:36` (add declaration)
- Modify: `rhi/vulkan/VulkanCommandList.cpp:356` (add implementation)

- [ ] **Step 1: Add declaration to VulkanCommandList.h**

Add after `bindBindTable` declaration:

```cpp
void bindBindGroup(uint32_t slot, BindGroupHandle bindGroup,
                   const uint32_t* dynamicOffsets,
                   uint32_t dynamicOffsetCount) override;
```

- [ ] **Step 2: Add implementation to VulkanCommandList.cpp**

This requires tracking the current pipeline to get the layout. Add a member variable first:

```cpp
// In VulkanCommandList.h private section:
PipelineHandle m_currentPipeline{};
uint64_t m_currentPipelineLayout{0};  // Cached VkPipelineLayout
```

Then implement:

```cpp
void VulkanCommandList::bindBindGroup(uint32_t slot, BindGroupHandle bindGroup,
                                      const uint32_t* dynamicOffsets,
                                      uint32_t dynamicOffsetCount)
{
  ensureCommandBuffer(m_commandBuffer);
  // This will be properly implemented after Pipeline tracks BindGroupLayout
  // For now, this is a placeholder that uses the helper function
  // The actual implementation needs Pipeline to provide VkPipelineLayout
}
```

Note: This will be revisited in Phase 4 after Pipeline changes.

- [ ] **Step 3: Build to verify**

Run: `cmake --build build --target Demo`
Expected: Build succeeds (placeholder implementation)

- [ ] **Step 4: Commit**

```bash
git add rhi/vulkan/VulkanCommandList.h rhi/vulkan/VulkanCommandList.cpp
git commit -m "feat(rhi): add bindBindGroup placeholder to VulkanCommandList"
```

---

### Task 10: Fix beginRenderPass to use TextureViewHandle

**Files:**
- Modify: `rhi/vulkan/VulkanCommandList.cpp:162-202` (beginRenderPass implementation)

- [ ] **Step 1: Add helper function to get VkImageView from TextureViewHandle**

Add a helper function declaration in VulkanCommandList.h:

```cpp
VkImageView getVkImageViewFromHandle(TextureViewHandle view) const;
```

And a simple implementation (will be properly connected to Renderer's view pool later):

```cpp
VkImageView VulkanCommandList::getVkImageViewFromHandle(TextureViewHandle view) const
{
  // This is a temporary implementation
  // The actual implementation will query a view registry
  return reinterpret_cast<VkImageView>(static_cast<uintptr_t>(view.index));
}
```

- [ ] **Step 2: Update beginRenderPass to use view handles**

Replace the imageView assignments in beginRenderPass:

```cpp
void VulkanCommandList::beginRenderPass(const RenderPassDesc& desc)
{
  ensureCommandBuffer(m_commandBuffer);
  std::vector<VkRenderingAttachmentInfo> colorAttachments(desc.colorTargetCount);
  for(uint32_t i = 0; i < desc.colorTargetCount; ++i)
  {
    colorAttachments[i] = VkRenderingAttachmentInfo{
        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView   = getVkImageViewFromHandle(desc.colorTargets[i].view),
        .imageLayout = toVkImageLayout(desc.colorTargets[i].state),
        .loadOp      = toVkLoadOp(desc.colorTargets[i].loadOp),
        .storeOp     = toVkStoreOp(desc.colorTargets[i].storeOp),
        .clearValue  = {{{desc.colorTargets[i].clearColor.r, desc.colorTargets[i].clearColor.g,
                          desc.colorTargets[i].clearColor.b, desc.colorTargets[i].clearColor.a}}},
    };
  }

  VkRenderingAttachmentInfo depthAttachment{};
  VkRenderingInfo           renderingInfo{
      .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .renderArea           = {{desc.renderArea.offset.x, desc.renderArea.offset.y},
                               {desc.renderArea.extent.width, desc.renderArea.extent.height}},
      .layerCount           = 1,
      .colorAttachmentCount = desc.colorTargetCount,
      .pColorAttachments    = colorAttachments.data(),
  };

  if(desc.depthTarget != nullptr)
  {
    depthAttachment = VkRenderingAttachmentInfo{
        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView   = getVkImageViewFromHandle(desc.depthTarget->view),
        .imageLayout = toVkImageLayout(desc.depthTarget->state),
        .loadOp      = toVkLoadOp(desc.depthTarget->loadOp),
        .storeOp     = toVkStoreOp(desc.depthTarget->storeOp),
        .clearValue  = {.depthStencil = {desc.depthTarget->clearValue.depth, desc.depthTarget->clearValue.stencil}},
    };
    renderingInfo.pDepthAttachment = &depthAttachment;
  }

  vkCmdBeginRendering(m_commandBuffer, &renderingInfo);
}
```

- [ ] **Step 3: Build to verify**

Run: `cmake --build build --target Demo`
Expected: Build succeeds

- [ ] **Step 4: Commit**

```bash
git add rhi/vulkan/VulkanCommandList.h rhi/vulkan/VulkanCommandList.cpp
git commit -m "fix(rhi): use TextureViewHandle in beginRenderPass"
```

---

## Phase 3: BindGroup System Refactoring

### Task 11: Rename BindTableLayout to BindGroupLayout (alias)

**Files:**
- Modify: `rhi/RHIDescriptor.h:86-95` (BindTableLayout class)

- [ ] **Step 1: Add BindGroupLayout alias and new types**

Add to RHIDescriptor.h before the BindTableLayout class:

```cpp
// Unified BindGroupLayout - supports both bindless arrays and per-draw resources
struct BindGroupLayoutEntry
{
  uint32_t           binding{0};            // Binding slot index
  ShaderStage        visibility{ShaderStage::none};
  BindlessResourceType type{BindlessResourceType::sampledTexture};
  uint32_t           count{1};              // 1 = single resource, >1 = bindless array
};

struct BindGroupLayoutDesc
{
  const BindGroupLayoutEntry* entries{nullptr};
  uint32_t                    entryCount{0};
};

// Forward declaration for handle type
class BindGroupLayout;

// Alias for backwards compatibility during transition
using BindTableLayout = BindGroupLayout;
```

Note: We'll keep `BindTableLayout` as an alias for backwards compatibility during migration.

- [ ] **Step 2: Build to verify**

Run: `cmake --build build --target Demo`
Expected: Build fails (BindGroupLayout not defined)

- [ ] **Step 3: Commit**

```bash
git add rhi/RHIDescriptor.h
git commit -m "feat(rhi): add BindGroupLayout types and alias"
```

---

### Task 12: Rename BindTable to BindGroup (alias)

**Files:**
- Modify: `rhi/RHIDescriptor.h:132-143` (BindTable class)

- [ ] **Step 1: Add BindGroup types before BindTable class**

Add to RHIDescriptor.h:

```cpp
// BindGroup entry for resource binding
struct BindGroupEntry
{
  uint32_t     binding{0};
  BufferHandle buffer{};
  TextureHandle texture{};
  uint64_t     offset{0};
  uint64_t     range{0};
};

struct BindGroupDesc
{
  BindGroupLayoutHandle layout{};
  const BindGroupEntry* entries{nullptr};
  uint32_t              entryCount{0};
};

class BindGroup
{
public:
  virtual ~BindGroup() = default;

  virtual void init(void* nativeDevice, const BindGroupDesc& desc) = 0;
  virtual void deinit() = 0;
  virtual void update(uint32_t entryIndex, const BindGroupEntry& entry) = 0;
  virtual uint64_t getNativeHandle() const = 0;
};

// Alias for backwards compatibility
using BindTable = BindGroup;
```

- [ ] **Step 2: Build to verify**

Run: `cmake --build build --target Demo`
Expected: Build fails (need to update Vulkan implementation)

- [ ] **Step 3: Commit**

```bash
git add rhi/RHIDescriptor.h
git commit -m "feat(rhi): add BindGroup types and alias"
```

---

### Task 13: Add BindGroupLayoutHandle to RHIHandles.h

**Files:**
- Modify: `rhi/RHIHandles.h:38` (after BindLayoutTag)

- [ ] **Step 1: Add BindGroupLayoutTag and handle**

Add tags and handles:

```cpp
struct BindGroupLayoutTag;
struct BindGroupTag;

using BindGroupLayoutHandle = Handle<BindGroupLayoutTag>;
using BindGroupHandle       = Handle<BindGroupTag>;
```

- [ ] **Step 2: Build to verify**

Run: `cmake --build build --target Demo`
Expected: Build succeeds

- [ ] **Step 3: Commit**

```bash
git add rhi/RHIHandles.h
git commit -m "feat(rhi): add BindGroupLayoutHandle and BindGroupHandle"
```

---

## Phase 4: Pipeline BindGroupLayout Association

### Task 14: Extend GraphicsPipelineDesc with BindGroupLayouts

**Files:**
- Modify: `rhi/RHIPipeline.h:131-144` (GraphicsPipelineDesc)

- [ ] **Step 1: Add bindGroupLayouts to GraphicsPipelineDesc**

Add to GraphicsPipelineDesc:

```cpp
struct GraphicsPipelineDesc
{
  const PipelineLayout*          layout{nullptr};
  const PipelineShaderStageDesc* shaderStages{nullptr};
  uint32_t                       shaderStageCount{0};
  VertexInputLayoutDesc          vertexInput{};
  RasterState                    rasterState{};
  DepthState                     depthState{};
  const BlendAttachmentState*    blendStates{nullptr};
  uint32_t                       blendStateCount{0};
  const DynamicState*            dynamicStates{nullptr};
  uint32_t                       dynamicStateCount{0};
  PipelineRenderingInfo          renderingInfo{};
  
  // New: BindGroupLayouts for each slot
  const BindGroupLayoutHandle*   bindGroupLayouts{nullptr};
  uint32_t                       bindGroupLayoutCount{0};
};
```

- [ ] **Step 2: Build to verify**

Run: `cmake --build build --target Demo`
Expected: Build succeeds

- [ ] **Step 3: Commit**

```bash
git add rhi/RHIPipeline.h
git commit -m "feat(rhi): add bindGroupLayouts to GraphicsPipelineDesc"
```

---

## Phase 5: Renderer Interface Changes

### Task 15: Add BindGroup creation methods to Renderer

**Files:**
- Modify: `render/Renderer.h:106-161` (public interface section)

- [ ] **Step 1: Add BindGroup creation methods**

Add to Renderer public interface:

```cpp
// BindGroup creation
BindGroupLayoutHandle createBindGroupLayout(const rhi::BindGroupLayoutDesc& desc);
BindGroupHandle createBindGroup(const rhi::BindGroupDesc& desc);
void destroyBindGroupLayout(BindGroupLayoutHandle handle);
void destroyBindGroup(BindGroupHandle handle);
```

- [ ] **Step 2: Build to verify**

Run: `cmake --build build --target Demo`
Expected: Build fails (implementation needed)

- [ ] **Step 3: Commit**

```bash
git add render/Renderer.h
git commit -m "feat(render): add BindGroup creation methods to Renderer interface"
```

---

### Task 16: Implement createBindGroupLayout in Renderer

**Files:**
- Modify: `render/Renderer.cpp` (add implementation)

- [ ] **Step 1: Add BindGroupLayout handle pool to Renderer private section**

Add to MaterialResources or a new section:

```cpp
HandlePool<BindGroupLayoutHandle, std::unique_ptr<rhi::BindGroupLayout>> m_bindGroupLayoutPool;
HandlePool<BindGroupHandle, std::unique_ptr<rhi::BindGroup>> m_bindGroupPool;
```

- [ ] **Step 2: Implement createBindGroupLayout**

```cpp
BindGroupLayoutHandle Renderer::createBindGroupLayout(const rhi::BindGroupLayoutDesc& desc)
{
  auto layout = std::make_unique<rhi::vulkan::VulkanBindGroupLayout>();
  // Convert BindGroupLayoutDesc to BindTableLayoutEntry format
  std::vector<rhi::BindTableLayoutEntry> tableEntries;
  tableEntries.reserve(desc.entryCount);
  for(uint32_t i = 0; i < desc.entryCount; ++i)
  {
    tableEntries.push_back(rhi::BindTableLayoutEntry{
        .logicalIndex   = desc.entries[i].binding,
        .resourceType   = desc.entries[i].type,
        .descriptorCount = desc.entries[i].count,
        .visibility     = static_cast<rhi::ResourceVisibility>(desc.entries[i].visibility),
    });
  }
  layout->init(m_device.device->getNativeDevice(), tableEntries);
  return m_bindGroupLayoutPool.allocate(std::move(layout));
}
```

- [ ] **Step 3: Build to verify**

Run: `cmake --build build --target Demo`
Expected: Build fails (BindGroupHandle implementation needed)

- [ ] **Step 4: Commit**

```bash
git add render/Renderer.h render/Renderer.cpp
git commit -m "feat(render): implement createBindGroupLayout"
```

---

### Task 17: Add RHI accessor methods to Renderer

**Files:**
- Modify: `render/Renderer.h:163-165` (replace native accessors)

- [ ] **Step 1: Add RHI accessor methods**

Replace native accessors with RHI versions:

```cpp
// Remove these:
// VkImageView getCurrentSwapchainImageView() const;
// uint64_t getLightPipelineLayout() const;
// uint64_t getGBufferPipelineLayout() const;
// uint64_t getGBufferColorDescriptorSet() const;

// Add these:
TextureViewHandle getCurrentSwapchainView() const;
TextureViewHandle getGBufferView(uint32_t index) const;
TextureViewHandle getDepthView() const;
BindGroupHandle getGlobalBindlessGroup() const;
```

- [ ] **Step 2: Build to verify**

Run: `cmake --build build --target Demo`
Expected: Build fails (implementation needed)

- [ ] **Step 3: Commit**

```bash
git add render/Renderer.h
git commit -m "feat(render): add RHI accessor methods, deprecate native accessors"
```

---

## Phase 6: Pass Layer Refactoring

### Task 18: Update PassContext with globalBindlessGroup

**Files:**
- Modify: `render/Pass.h:18-30` (PassContext struct)

- [ ] **Step 1: Add globalBindlessGroup to PassContext**

Add to PassContext:

```cpp
struct PassContext
{
  rhi::CommandList*   cmd{nullptr};
  TransientAllocator* transientAllocator{nullptr};
  uint32_t            frameIndex{0};
  uint32_t            passIndex{0};
  const RenderParams* params{nullptr};
  std::vector<StreamEntry>* drawStream{nullptr};
  const GltfUploadResult*   gltfModel{nullptr};
  
  // New: Global bindless resource (bind once at pass start)
  BindGroupHandle     globalBindlessGroup{};
};
```

- [ ] **Step 2: Build to verify**

Run: `cmake --build build --target Demo`
Expected: Build succeeds

- [ ] **Step 3: Commit**

```bash
git add render/Pass.h
git commit -m "feat(render): add globalBindlessGroup to PassContext"
```

---

### Task 19: Refactor GBufferPass to use RHI interfaces

**Files:**
- Modify: `render/passes/GBufferPass.cpp`

- [ ] **Step 1: Remove Vulkan includes**

Remove:
```cpp
#include "../../rhi/vulkan/VulkanCommandList.h"
#include "../../rhi/vulkan/VulkanDescriptor.h"
```

- [ ] **Step 2: Replace VkRenderingAttachmentInfo with RenderTargetDesc**

Replace the Vulkan struct setup with RHI types:

```cpp
// Setup MRT color attachments
std::array<rhi::RenderTargetDesc, 3> colorTargets{};
for(uint32_t i = 0; i < 3; ++i)
{
  colorTargets[i] = {
    .texture = {}, // Will be filled from SceneResources
    .view = m_renderer->getGBufferView(i),
    .state = rhi::ResourceState::colorAttachment,
    .loadOp = rhi::LoadOp::clear,
    .storeOp = rhi::StoreOp::store,
    .clearColor = {0.0f, 0.0f, 0.0f, 0.0f},
  };
}

rhi::DepthTargetDesc depthTarget{
  .texture = {},
  .view = m_renderer->getDepthView(),
  .state = rhi::ResourceState::depthAttachment,
  .loadOp = rhi::LoadOp::clear,
  .storeOp = rhi::StoreOp::store,
  .clearValue = {1.0f, 0},
};

rhi::RenderPassDesc passDesc{
  .renderArea = {{0, 0}, extent},
  .colorTargets = colorTargets.data(),
  .colorTargetCount = 3,
  .depthTarget = &depthTarget,
};
context.cmd->beginRenderPass(passDesc);
```

- [ ] **Step 3: Replace vkCmdBindDescriptorSets with bindBindGroup**

Replace:
```cpp
vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd), ...)
```

With:
```cpp
context.cmd->bindBindGroup(shaderio::LSetTextures, context.globalBindlessGroup, nullptr, 0);
context.cmd->bindBindGroup(shaderio::LSetScene, cameraBindGroup, &cameraDynamicOffset, 1);
context.cmd->bindBindGroup(shaderio::LSetDraw, drawBindGroup, &drawDynamicOffset, 1);
```

- [ ] **Step 4: Replace Vulkan draw commands with RHI**

Replace:
```cpp
rhi::vulkan::cmdBindVertexBuffers(...)
rhi::vulkan::cmdBindIndexBuffer(...)
rhi::vulkan::cmdDrawIndexed(...)
```

With:
```cpp
context.cmd->bindVertexBuffers(0, &vertexBuffer, &offset, 1);
context.cmd->bindIndexBuffer(mesh->indexBuffer, 0, rhi::IndexFormat::uint32);
context.cmd->drawIndexed(mesh->indexCount, 1, 0, 0, 0);
```

- [ ] **Step 5: Build to verify**

Run: `cmake --build build --target Demo`
Expected: Build fails (some methods may not be implemented yet)

- [ ] **Step 6: Commit**

```bash
git add render/passes/GBufferPass.cpp
git commit -m "refactor(pass): use RHI interfaces in GBufferPass"
```

---

### Task 20: Refactor ForwardPass to use RHI interfaces

**Files:**
- Modify: `render/passes/ForwardPass.cpp`

- [ ] **Step 1: Remove Vulkan includes**

Remove:
```cpp
#include "../../rhi/vulkan/VulkanCommandList.h"
#include "../../rhi/vulkan/VulkanDescriptor.h"
```

- [ ] **Step 2: Replace Vulkan types with RHI types**

Same pattern as GBufferPass - replace VkRenderingAttachmentInfo with rhi::RenderTargetDesc, etc.

- [ ] **Step 3: Replace Vulkan commands with RHI**

Replace vkCmdBindDescriptorSets with bindBindGroup, etc.

- [ ] **Step 4: Build to verify**

Run: `cmake --build build --target Demo`
Expected: Build may have errors due to incomplete implementation

- [ ] **Step 5: Commit**

```bash
git add render/passes/ForwardPass.cpp
git commit -m "refactor(pass): use RHI interfaces in ForwardPass"
```

---

### Task 21: Refactor LightPass to use RHI interfaces

**Files:**
- Modify: `render/passes/LightPass.cpp`

- [ ] **Step 1: Remove Vulkan includes**

Remove:
```cpp
#include "../../rhi/vulkan/VulkanCommandList.h"
```

- [ ] **Step 2: Replace Vulkan types with RHI types**

Same pattern as GBufferPass.

- [ ] **Step 3: Replace Vulkan commands with RHI**

Replace vkCmdBindDescriptorSets with bindBindGroup, etc.

- [ ] **Step 4: Build to verify**

Run: `cmake --build build --target Demo`
Expected: Build may have errors

- [ ] **Step 5: Commit**

```bash
git add render/passes/LightPass.cpp
git commit -m "refactor(pass): use RHI interfaces in LightPass"
```

---

## Phase 7: Integration and Testing

### Task 22: Build and fix compilation errors

**Files:**
- Various files as needed

- [ ] **Step 1: Full build**

Run: `cmake --build build --target Demo`
Expected: May have compilation errors

- [ ] **Step 2: Fix each error iteratively**

Fix compilation errors one by one until build succeeds.

- [ ] **Step 3: Commit**

```bash
git add -A
git commit -m "fix: resolve compilation errors after RHI encapsulation"
```

---

### Task 23: Run and verify rendering

**Files:**
- N/A

- [ ] **Step 1: Run the demo**

Run: `./build/out/Demo.exe`
Expected: No validation errors, correct rendering

- [ ] **Step 2: Check for Vulkan validation errors**

Run with validation layers enabled and verify no errors.

- [ ] **Step 3: Fix any runtime issues**

Address any validation errors or rendering issues.

- [ ] **Step 4: Final commit**

```bash
git add -A
git commit -m "fix: resolve runtime issues after RHI encapsulation"
```

---

## Summary

| Phase | Tasks | Key Changes |
|-------|-------|-------------|
| 1 | 1-3 | RHI types: IndexFormat, TextureViewDesc, RenderTargetDesc |
| 2 | 4-10 | CommandList: bindIndexBuffer, drawIndexed, bindBindGroup |
| 3 | 11-13 | BindGroup system: BindGroupLayout, BindGroup |
| 4 | 14 | Pipeline: BindGroupLayout association |
| 5 | 15-17 | Renderer: RHI accessors, BindGroup creation |
| 6 | 18-21 | Pass: RHI-only interfaces |
| 7 | 22-23 | Integration: Build and test |

Total: 23 tasks