---
name: rhi-native-interface-encapsulation
description: Encapsulate native Vulkan interfaces in RHI layer for cross-platform abstraction
type: project
---

# RHI Native Interface Encapsulation Design

## Problem Statement

Current Pass layer code directly uses Vulkan native types and APIs:
- `VkRenderingAttachmentInfo`, `VkRenderingInfo`, `VkViewport`, `VkRect2D`
- `VkPipelineLayout`, `VkDescriptorSet`, `VkPipeline`, `VkBuffer`
- `vkCmdBindDescriptorSets`, `reinterpret_cast` for opaque handles

This violates the RHI abstraction layer design intent and prevents future cross-platform support (Metal, D3D12, WebGPU).

## Design Goals

1. Pass layer uses only RHI interfaces - no native types exposed
2. PipelineLayout hidden from upper layers - Pipeline owns it internally
3. Unified BindGroupLayout - supports both bindless array and per-draw resources
4. Modern mobile-friendly architecture - aligned with WebGPU/Metal patterns

## Architecture Overview

```
┌─────────────────────────────────────────────────┐
│                    Pass Layer                    │
│  execute(context) {                             │
│    cmd->bindPipeline(pipeline);                 │
│    cmd->bindBindGroup(0, globalBindless);       │
│    cmd->bindBindGroup(1, cameraGroup, offset);  │
│    cmd->bindBindGroup(2, drawGroup, offset);    │
│    cmd->bindIndexBuffer(...);                   │
│    cmd->drawIndexed(...);                       │
│  }                                              │
└─────────────────────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────┐
│                    RHI Layer                     │
│  CommandList, Pipeline, BindGroup, BindGroupLayout│
│  - Fully encapsulated, no native types exposed   │
└─────────────────────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────┐
│                 Vulkan/Metal/D3D12               │
│  Platform implementations                        │
└─────────────────────────────────────────────────┘
```

## Detailed Design

### 1. CommandList Interface Extension

**New methods in RHICommandList.h:**

```cpp
class CommandList {
public:
  // Existing methods...
  
  // New: Index buffer binding
  virtual void bindIndexBuffer(BufferHandle buffer, uint64_t offset, IndexFormat format) = 0;
  
  // New: Indexed drawing
  virtual void drawIndexed(uint32_t indexCount, uint32_t instanceCount,
                           uint32_t firstIndex, int32_t vertexOffset,
                           uint32_t firstInstance) = 0;
  
  // Extended: BindGroup binding with dynamic offsets
  virtual void bindBindGroup(uint32_t slot, BindGroupHandle bindGroup,
                             const uint32_t* dynamicOffsets,
                             uint32_t dynamicOffsetCount) = 0;
};
```

**New enum in RHITypes.h:**

```cpp
enum class IndexFormat : uint8_t {
  uint16 = 0,
  uint32 = 1,
};
```

### 2. BindGroupLayout - Unified Design

BindGroupLayout handles both scenarios:
- **Bindless array**: `count > 1` for global texture pool
- **Per-draw resource**: `count = 1` for UBOs

```cpp
struct BindGroupLayoutEntry {
  uint32_t binding;           // slot index (0, 1, 2...)
  ShaderStage visibility;     // vertex/fragment/compute
  BindlessResourceType type;  // sampledTexture/uniformBufferDynamic/...
  uint32_t count;             // 1 = single resource, >1 = bindless array
};

struct BindGroupLayoutDesc {
  const BindGroupLayoutEntry* entries;
  uint32_t entryCount;
};

class BindGroupLayout {
public:
  virtual void init(void* nativeDevice, const BindGroupLayoutDesc& desc) = 0;
  virtual void deinit() = 0;
  virtual uint64_t getNativeHandle() const = 0;
};
```

**BindGroup:**

```cpp
struct BindGroupEntry {
  uint32_t binding;
  BufferHandle buffer;
  BufferHandle* buffers;      // for bindless array (count > 1)
  TextureHandle texture;
  TextureHandle* textures;    // for bindless array
  uint64_t offset;
  uint64_t range;
};

struct BindGroupDesc {
  BindGroupLayoutHandle layout;
  const BindGroupEntry* entries;
  uint32_t entryCount;
};

class BindGroup {
public:
  virtual void init(void* nativeDevice, const BindGroupDesc& desc) = 0;
  virtual void deinit() = 0;
  virtual void update(uint32_t entryIndex, const BindGroupEntry& entry) = 0;
  virtual uint64_t getNativeHandle() const = 0;
};
```

### 3. Pipeline Associated with BindGroupLayout

**PipelineDesc extension:**

```cpp
struct PipelineDesc {
  ShaderHandle shader;
  VertexInputLayoutDesc vertexInput;
  RasterState rasterState;
  DepthState depthState;
  const BlendAttachmentState* blendStates;
  uint32_t blendStateCount;
  
  // New: BindGroupLayouts for each slot
  const BindGroupLayoutHandle* bindGroupLayouts;
  uint32_t bindGroupLayoutCount;
};
```

**Pipeline internal behavior:**

- **Vulkan**: Creates `VkPipelineLayout` from BindGroupLayout sequence, caches it internally
- **Metal**: Associates Argument Buffer layouts at creation
- **D3D12**: Associates Root Signature at creation

**PipelineLayout NOT exposed to upper layers:**

```cpp
// Removed from Renderer:
// VkPipelineLayout getGBufferPipelineLayout();

// Upper layers only need:
PipelineHandle pipeline = renderer.createPipeline(desc);
cmd.bindPipeline(pipeline);
cmd.bindBindGroup(0, bindGroup);  // Pipeline internally provides layout
```

### 4. GlobalBindlessTable

```cpp
class GlobalBindlessTable {
public:
  void init(DeviceHandle device, uint32_t maxTextures, uint32_t maxSamplers);
  void deinit();
  
  // Register texture to bindless pool, returns shader index
  uint32_t registerTexture(TextureHandle texture, SamplerHandle sampler);
  void updateTexture(uint32_t index, TextureHandle texture);
  
  // Get BindGroup for binding
  BindGroupHandle getBindGroup() const;
  
private:
  BindGroupLayoutHandle m_layout;
  BindGroupHandle m_bindGroup;
  std::vector<TextureHandle> m_textures;
  uint32_t m_maxTextures;
};
```

### 5. RenderPassDesc Completion

**Current issue:** `imageView = VK_NULL_HANDLE` in VulkanCommandList::beginRenderPass

**Solution:** Add TextureViewHandle to target descriptors

```cpp
struct RenderTargetDesc {
  TextureHandle texture{};
  TextureViewHandle view{};        // New: texture view
  ResourceState state{ResourceState::general};
  LoadOp loadOp{LoadOp::load};
  StoreOp storeOp{StoreOp::store};
  ClearColorValue clearColor{};
};

struct DepthTargetDesc {
  TextureHandle texture{};
  TextureViewHandle view{};        // New: depth view
  ResourceState state{ResourceState::general};
  LoadOp loadOp{LoadOp::load};
  StoreOp storeOp{StoreOp::store};
  ClearDepthStencilValue clearValue{};
};
```

**New TextureViewHandle:**

```cpp
struct TextureViewHandle {
  uint32_t index{0};
  uint32_t generation{0};
  bool isNull() const { return index == 0 && generation == 0; }
};
```

**TextureView creation:**

```cpp
struct TextureViewDesc {
  TextureHandle texture{};
  TextureAspect aspect{TextureAspect::color};
  uint32_t baseMipLevel{0};
  uint32_t mipLevelCount{1};
  uint32_t baseArrayLayer{0};
  uint32_t arrayLayerCount{1};
};
```

### 6. PassContext Update

```cpp
struct PassContext {
  rhi::CommandList* cmd{nullptr};
  TransientAllocator* transientAllocator{nullptr};
  uint32_t frameIndex{0};
  uint32_t passIndex{0};
  const RenderParams* params{nullptr};
  std::vector<StreamEntry>* drawStream{nullptr};
  const GltfUploadResult* gltfModel{nullptr};
  
  // New: Global bindless resource (bind once at pass start)
  BindGroupHandle globalBindlessGroup{};
};
```

### 7. Renderer Interface Changes

**Removed (Vulkan native):**
```cpp
// VkPipelineLayout getGBufferPipelineLayout();
// VkDescriptorSet getGBufferColorDescriptorSet();
// VkImageView getCurrentSwapchainImageView();
```

**Added (RHI):**
```cpp
class Renderer {
public:
  PipelineHandle getGBufferPipeline();
  PipelineHandle getForwardPipeline();
  PipelineHandle getLightPipeline();
  
  BindGroupHandle getGlobalBindlessGroup();
  BindGroupHandle getCameraBindGroup(uint32_t frameIndex);
  BindGroupHandle getDrawBindGroup(uint32_t frameIndex);
  
  TextureViewHandle getCurrentSwapchainView();
  TextureViewHandle getGBufferView(uint32_t index);
  TextureViewHandle getDepthView();
  
  PipelineHandle createPipeline(const PipelineDesc& desc);
  BindGroupLayoutHandle createBindGroupLayout(const BindGroupLayoutDesc& desc);
  BindGroupHandle createBindGroup(const BindGroupDesc& desc);
};
```

### 8. BindGroupLayout Registration in Renderer

```cpp
void Renderer::initBindGroupLayouts() {
  // Slot 0: Global Bindless (textures + samplers)
  BindGroupLayoutEntry bindlessEntries[] = {
    { .binding = 0, .visibility = ShaderStage::fragment,
      .type = BindlessResourceType::sampledTexture, .count = 1000 },
    { .binding = 1, .visibility = ShaderStage::fragment,
      .type = BindlessResourceType::sampler, .count = 100 }
  };
  BindGroupLayoutDesc bindlessLayout = { bindlessEntries, 2 };
  m_bindlessLayout = createBindGroupLayout(bindlessLayout);
  
  // Slot 1: Camera UBO (dynamic)
  BindGroupLayoutEntry cameraEntries[] = {
    { .binding = 0, .visibility = ShaderStage::vertex | ShaderStage::fragment,
      .type = BindlessResourceType::uniformBufferDynamic, .count = 1 }
  };
  BindGroupLayoutDesc cameraLayout = { cameraEntries, 1 };
  m_cameraLayout = createBindGroupLayout(cameraLayout);
  
  // Slot 2: Draw UBO (dynamic)
  m_drawLayout = createBindGroupLayout(cameraLayout);
  
  // Create Pipeline with layouts
  BindGroupLayoutHandle gbufferLayouts[] = { m_bindlessLayout, m_cameraLayout, m_drawLayout };
  PipelineDesc gbufferDesc = {
    .shader = gbufferShader,
    .bindGroupLayouts = gbufferLayouts,
    .bindGroupLayoutCount = 3,
    // ... other fields
  };
  m_gbufferPipeline = createPipeline(gbufferDesc);
}
```

## Pass Layer Usage Example

```cpp
void GBufferPass::execute(const PassContext& context) const {
  context.cmd->beginEvent("GBufferPass");
  
  // Setup render pass
  RenderTargetDesc colorTargets[3] = {
    { .texture = gbuffer0, .view = gbuffer0View, .state = ResourceState::colorAttachment, .loadOp = LoadOp::clear },
    { .texture = gbuffer1, .view = gbuffer1View, .state = ResourceState::colorAttachment, .loadOp = LoadOp::clear },
    { .texture = gbuffer2, .view = gbuffer2View, .state = ResourceState::colorAttachment, .loadOp = LoadOp::clear },
  };
  DepthTargetDesc depthTarget = {
    { .texture = depth, .view = depthView, .state = ResourceState::depthAttachment, .loadOp = LoadOp::clear },
  };
  RenderPassDesc passDesc = {
    .renderArea = { .extent = extent },
    .colorTargets = colorTargets,
    .colorTargetCount = 3,
    .depthTarget = &depthTarget,
  };
  
  context.cmd->beginRenderPass(passDesc);
  context.cmd->bindPipeline(m_gbufferPipeline);
  
  // Bind global bindless (once)
  context.cmd->bindBindGroup(0, context.globalBindlessGroup);
  
  // Per-draw loop
  for(const auto& mesh : meshes) {
    Allocation cameraAlloc = transientAllocator->allocate(sizeof(CameraUniforms), 256);
    Allocation drawAlloc = transientAllocator->allocate(sizeof(DrawUniforms), 256);
    
    BindGroupHandle cameraGroup = m_renderer->getCameraBindGroup(context.frameIndex);
    BindGroupHandle drawGroup = m_renderer->getDrawBindGroup(context.frameIndex);
    
    context.cmd->bindBindGroup(1, cameraGroup, &cameraAlloc.offset, 1);
    context.cmd->bindBindGroup(2, drawGroup, &drawAlloc.offset, 1);
    
    context.cmd->bindVertexBuffers(0, &mesh.vertexBuffer, &offset, 1);
    context.cmd->bindIndexBuffer(mesh.indexBuffer, 0, IndexFormat::uint32);
    context.cmd->drawIndexed(mesh.indexCount, 1, 0, 0, 0);
  }
  
  context.cmd->endRenderPass();
  context.cmd->endEvent();
}
```

## File Change Summary

| File | Change Type |
|-----|-------------|
| `rhi/RHIHandles.h` | Add TextureViewHandle, BindGroupLayoutHandle |
| `rhi/RHITypes.h` | Add IndexFormat enum |
| `rhi/RHICommandList.h` | Extend interface (bindIndexBuffer, drawIndexed, bindBindGroup) |
| `rhi/RHIDescriptor.h` | Refactor BindGroupLayout/BindGroup with unified design |
| `rhi/vulkan/VulkanCommandList.h/cpp` | Implement new interfaces, fix beginRenderPass |
| `rhi/vulkan/VulkanDescriptor.h/cpp` | Implement new BindGroup system |
| `rhi/vulkan/VulkanPipelines.h/cpp` | Pipeline associates BindGroupLayout |
| `render/Renderer.h/cpp` | Remove native interfaces, add RHI interfaces |
| `render/Pass.h` | PassContext add globalBindlessGroup |
| `render/passes/GBufferPass.cpp` | Remove Vulkan types, use RHI |
| `render/passes/ForwardPass.cpp` | Remove Vulkan types, use RHI |
| `render/passes/LightPass.cpp` | Remove Vulkan types, use RHI |

## Cross-Platform Compatibility

This design aligns with modern mobile rendering architectures:

| Platform | PipelineLayout Equivalent | Bindless Support |
|----------|--------------------------|-----------------|
| Vulkan | VkPipelineLayout (hidden) | Descriptor indexing (VK 1.2+) |
| Metal | None (Argument Buffer) | Argument Buffer (A9+) |
| D3D12 | Root Signature (hidden) | Descriptor Heap |
| WebGPU | None | setBindGroup(slot, group) |

**Mobile GPU Bindless Support:**
- ARM Mali (Bifrost/Valhall): descriptor indexing supported
- Qualcomm Adreno (600+): descriptor indexing supported
- Apple A9+: Argument Buffer supported