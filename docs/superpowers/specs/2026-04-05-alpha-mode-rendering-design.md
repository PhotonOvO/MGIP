# Alpha Mode Rendering Design

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement proper alpha mode handling for glTF materials: Opaque, AlphaTest/Mask, and Transparent rendering with Deferred + Forward hybrid architecture.

**Architecture:** Use Specialization Constants for AlphaTest pipeline variants, separate ForwardPass for Transparent objects with depth sorting and alpha blending.

**Tech Stack:** Vulkan, Slang shaders, VMA, existing PassExecutor framework

---

## Overview

glTF materials define three alpha modes:
- **OPAQUE**: No alpha handling, render to GBuffer
- **MASK (AlphaTest)**: Discard pixels below alphaCutoff, render to GBuffer
- **BLEND (Transparent)**: Alpha blending, requires separate Forward pass

Current implementation treats all materials as opaque. This design adds proper handling for all three modes.

---

## 1. Alpha Mode Classification

### Rendering Pipeline Flow

```
glTF Material.alphaMode
├── OPAQUE     → GBufferPass (Pipeline Variant 0, no alpha test)
├── MASK       → GBufferPass (Pipeline Variant 1, alphaTestEnabled=true)
└── BLEND      → ForwardPass (separate pass, blending enabled)
```

### Pass Execution Order

```
Before: GBufferPass → LightPass → PresentPass → ImguiPass
After:  GBufferPass → LightPass → ForwardPass → PresentPass → ImguiPass
```

### Transparent Object Handling

- Filter meshes where `alphaMode == BLEND`
- Sort by distance from camera (far to near)
- Render with standard alpha blending
- Depth test enabled, depth write disabled

---

## 2. Shader Changes

### shader_io.h

Add alpha mode constants and extend DrawUniforms:

```cpp
// Alpha mode enum (matches glTF spec)
STATIC_CONST int LAlphaOpaque   = 0;
STATIC_CONST int LAlphaMask     = 1;
STATIC_CONST int LAlphaBlend    = 2;

struct DrawUniforms
{
  mat4 modelMatrix;
  vec4 baseColorFactor;
  int32_t baseColorTextureIndex;   // -1 = no texture
  int32_t normalTextureIndex;      // -1 = no texture
  int32_t metallicRoughnessTextureIndex;  // -1 = no texture
  int32_t occlusionTextureIndex;   // -1 = no texture
  float metallicFactor;
  float roughnessFactor;
  float normalScale;
  int32_t alphaMode;      // NEW: 0=OPAQUE, 1=MASK, 2=BLEND
  float alphaCutoff;      // NEW: default 0.5, for MASK mode
  float _padding[2];
};
```

### shader.gbuffer.slang

Add specialization constant for alpha test:

```hlsl
// Specialization constant for alpha test (constant_id = 0)
[[vk::constant_id(0)]]
const bool alphaTestEnabled = false;

[shader("fragment")]
void fragmentMain(
    VertexOutput input,
    out float4 outGBuffer0 : SV_Target0,
    out float4 outGBuffer1 : SV_Target1,
    out float4 outGBuffer2 : SV_Target2
) {
    // 1. BaseColor sampling (existing code)
    float4 color = draw.baseColorFactor;
    if(draw.baseColorTextureIndex >= 0)
    {
        float4 texColor = inTexture[draw.baseColorTextureIndex].Sample(input.uv);
        color = float4(texColor.rgb * draw.baseColorFactor.rgb, texColor.a * draw.baseColorFactor.a);
    }

    // 2. Alpha test (specialization constant controlled, no runtime branch)
    if (alphaTestEnabled)
    {
        if (color.a < draw.alphaCutoff)
        {
            discard;
        }
    }

    // 3. Normal mapping (existing code)
    // ...

    // 4. PBR values (existing code)
    // ...

    // Pack outputs
    outGBuffer0 = float4(color.rgb, 0.0);
    outGBuffer1 = float4(worldNormal * 0.5 + 0.5, 0.0);
    outGBuffer2 = float4(metallic, roughness, ao, 0.0);
}
```

### shader.forward.slang (NEW FILE)

Forward rendering shader for transparent objects:

```hlsl
#include "shader_io.h"

// Specialization constant (not used for forward, but keep consistent structure)
[[vk::constant_id(0)]]
const bool alphaTestEnabled = false;

struct VertexInput
{
    [[vk::location(LVGltfPosition)]] float3 position : POSITION;
    [[vk::location(LVGltfNormal)]]   float3 normal   : NORMAL;
    [[vk::location(LVGltfTexCoord)]] float2 texCoord : TEXCOORD0;
    [[vk::location(LVGltfTangent)]]  float4 tangent  : TANGENT;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
};

// Resource bindings (same as GBuffer)
[[vk::binding(LBindTextures, LSetTextures)]]
Sampler2D inTexture[];

[[vk::binding(LBindCamera, LSetScene)]]
ConstantBuffer<CameraUniforms> camera;

[[vk::binding(LBindDrawModel, LSetDraw)]]
ConstantBuffer<DrawUniforms> draw;

[shader("vertex")]
VertexOutput vertexMain(VertexInput input)
{
    VertexOutput output;
    float4 worldPos = mul(draw.modelMatrix, float4(input.position, 1.0));
    output.position = mul(camera.viewProjection, worldPos);
    output.uv = input.texCoord;
    output.worldNormal = normalize(mul(draw.modelMatrix, float4(input.normal, 0.0)).xyz);
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target
{
    // BaseColor sampling
    float4 color = draw.baseColorFactor;
    if(draw.baseColorTextureIndex >= 0)
    {
        float4 texColor = inTexture[draw.baseColorTextureIndex].Sample(input.uv);
        color = float4(texColor.rgb * draw.baseColorFactor.rgb, texColor.a * draw.baseColorFactor.a);
    }

    // Output with alpha (blending handled by pipeline state)
    return color;
}
```

---

## 3. Data Structure Changes

### GltfLoader.h - GltfMaterialData

```cpp
struct GltfMaterialData {
    // ... existing fields ...

    // Alpha mode (glTF spec)
    int alphaMode = 0;            // 0=OPAQUE, 1=MASK, 2=BLEND
    float alphaCutoff = 0.5f;     // for MASK mode
};
```

### Renderer.h - MaterialRecord

```cpp
struct MaterialRecord
{
    // ... existing texture handles and factors ...

    // Alpha properties
    int32_t alphaMode = 0;        // NEW: OPAQUE/MASK/BLEND
    float alphaCutoff = 0.5f;     // NEW: for MASK mode
};
```

---

## 4. Pass Architecture

### ForwardPass (NEW FILE)

**render/passes/ForwardPass.h:**

```cpp
#pragma once

#include "../Pass.h"

namespace demo {

class Renderer;

class ForwardPass : public RenderPassNode
{
public:
    explicit ForwardPass(Renderer* renderer);

    const char* getName() const override { return "ForwardPass"; }
    HandleSlice<PassResourceDependency> getDependencies() const override;
    void execute(const PassContext& context) const override;

private:
    Renderer* m_renderer;
};

}  // namespace demo
```

**render/passes/ForwardPass.cpp:**

Dependencies:
- Read `kPassSwapchainHandle` (LightPass output)
- Read `kPassGBufferDepthHandle` (for depth test)

Execute flow:
1. Filter transparent meshes (`alphaMode == BLEND`)
2. Sort by distance from camera (far to near)
3. Bind Forward pipeline with blending
4. Set depth state: test enabled, write disabled
5. Render each mesh with DrawUniforms update

### Depth Sorting Algorithm

```cpp
// In ForwardPass::execute()
std::vector<std::pair<size_t, float>> transparentMeshDistances;

glm::vec3 cameraPos = context.params->cameraUniforms->cameraPosition;

for (size_t i = 0; i < context.gltfModel->meshes.size(); ++i)
{
    MeshHandle meshHandle = context.gltfModel->meshes[i];
    const MeshRecord* mesh = meshPool.tryGet(meshHandle);
    if (mesh == nullptr) continue;

    // Check material alpha mode
    if (mesh->materialIndex >= 0)
    {
        MaterialHandle matHandle = context.gltfModel->materials[mesh->materialIndex];
        auto indices = m_renderer->getMaterialTextureIndices(matHandle, context.gltfModel);
        if (indices.alphaMode != LAlphaBlend) continue;
    }

    // Calculate distance from camera
    glm::vec3 meshCenter = glm::vec3(mesh->transform[3]);
    float distance = glm::length(meshCenter - cameraPos);
    transparentMeshDistances.push_back({i, distance});
}

// Sort far to near
std::sort(transparentMeshDistances.begin(), transparentMeshDistances.end(),
    [](const auto& a, const auto& b) { return a.second > b.second; });
```

---

## 5. Pipeline Variants

### GBuffer Pipeline Creation

Create two variants using specialization constants:

```cpp
// Variant 0: Opaque (alphaTestEnabled = false)
bool alphaTestFalse = false;
rhi::SpecializationConstant specConstantAlphaTest(0, 0, sizeof(bool));
rhi::SpecializationData specDataFalse{&alphaTestFalse, sizeof(bool)};
shaderStages[1].specializationData = specDataFalse;
shaderStages[1].specializationConstants = &specConstantAlphaTest;
shaderStages[1].specializationConstantCount = 1;
graphicsCreateInfo.key.specializationVariant = 0;
VkPipeline gbufferOpaquePipeline = createGraphicsPipeline(...);
m_gbufferOpaquePipeline = registerPipeline(..., 0);

// Variant 1: AlphaTest (alphaTestEnabled = true)
bool alphaTestTrue = true;
rhi::SpecializationData specDataTrue{&alphaTestTrue, sizeof(bool)};
shaderStages[1].specializationData = specDataTrue;
graphicsCreateInfo.key.specializationVariant = 1;
VkPipeline gbufferAlphaTestPipeline = createGraphicsPipeline(...);
m_gbufferAlphaTestPipeline = registerPipeline(..., 1);
```

### Forward Pipeline Creation

```cpp
// Blend state for transparent objects
rhi::BlendAttachmentState forwardBlend{
    .blendEnable = true,
    .srcBlend = rhi::BlendFactor::srcAlpha,
    .dstBlend = rhi::BlendFactor::oneMinusSrcAlpha,
    .blendOp = rhi::BlendOp::add,
    .srcBlendAlpha = rhi::BlendFactor::one,
    .dstBlendAlpha = rhi::BlendFactor::oneMinusSrcAlpha,
    .blendOpAlpha = rhi::BlendOp::add,
    .colorWriteMask = rhi::ColorComponentFlags::all,
};

// Depth: test enabled, write disabled
rhi::DepthState forwardDepth{
    .depthTestEnable = true,
    .depthWriteEnable = false,  // Preserve LightPass depth
    .compareOp = rhi::CompareOp::lessOrEqual,
};

// Use same pipeline layout as GBuffer (shared descriptor sets)
rhi::GraphicsPipelineDesc forwardDesc{
    .layout = m_device.gbufferPipelineLayout.get(),
    .shaderStages = forwardShaderStages.data(),
    // ... vertex input same as GBuffer ...
    .blendStates = &forwardBlend,
    .blendStateCount = 1,
    .depthState = forwardDepth,
    // ... rendering info for swapchain format ...
};
```

---

## 6. GltfLoader Changes

Parse alphaMode from glTF material:

```cpp
// In GltfLoader::processMaterials()

// Alpha mode
std::string alphaModeStr = material.alphaMode;
if (alphaModeStr == "MASK") {
    matData.alphaMode = 1;  // LAlphaMask
    matData.alphaCutoff = static_cast<float>(material.alphaCutoff);
} else if (alphaModeStr == "BLEND") {
    matData.alphaMode = 2;  // LAlphaBlend
} else {
    matData.alphaMode = 0;  // LAlphaOpaque (default)
}
```

---

## 7. Renderer Integration

### Material Upload

When uploading glTF materials, store alpha properties:

```cpp
MaterialHandle Renderer::uploadMaterial(const GltfMaterialData& matData, ...)
{
    MaterialRecord record;
    // ... existing texture handles ...

    record.alphaMode = matData.alphaMode;
    record.alphaCutoff = matData.alphaCutoff;

    return m_materials.materialPool.emplace(std::move(record));
}
```

### Pipeline Selection in GBufferPass

```cpp
// In GBufferPass::execute()
for (size_t i = 0; i < context.gltfModel->meshes.size(); ++i)
{
    MeshHandle meshHandle = context.gltfModel->meshes[i];
    const MeshRecord* mesh = meshPool.tryGet(meshHandle);

    // Skip transparent meshes (handled by ForwardPass)
    if (mesh->materialIndex >= 0)
    {
        MaterialHandle matHandle = context.gltfModel->materials[mesh->materialIndex];
        auto indices = m_renderer->getMaterialTextureIndices(matHandle, context.gltfModel);
        if (indices.alphaMode == LAlphaBlend) continue;
    }

    // Select pipeline variant based on alpha mode
    PipelineHandle gbufferPipeline;
    if (indices.alphaMode == LAlphaMask)
    {
        gbufferPipeline = m_renderer->getGBufferAlphaTestPipelineHandle();
    }
    else
    {
        gbufferPipeline = m_renderer->getGBufferOpaquePipelineHandle();
    }

    // ... render mesh ...
}
```

---

## 8. API Additions

### Renderer.h

```cpp
// Pipeline handles for alpha variants
PipelineHandle getGBufferOpaquePipelineHandle() const;
PipelineHandle getGBufferAlphaTestPipelineHandle() const;
PipelineHandle getForwardPipelineHandle() const;

// Alpha mode query (extend MaterialTextureIndices)
struct MaterialTextureIndices {
    int32_t baseColor = -1;
    int32_t normal = -1;
    int32_t metallicRoughness = -1;
    int32_t occlusion = -1;
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    float normalScale = 1.0f;
    int32_t alphaMode = 0;    // NEW
    float alphaCutoff = 0.5f; // NEW
};
```

---

## 9. Testing

### Test Materials

Verify with Sponza/Bistro models:
- **Opaque**: Walls, floors, columns → GBufferPass
- **MASK**: Leaves, vegetation with alpha test → GBufferPass AlphaTest variant
- **BLEND**: Curtains, glass panels → ForwardPass with blending

### Visual Verification

- Opaque objects: Correct GBuffer output, deferred lighting
- AlphaTest objects: Clean edges at alphaCutoff, no artifacts
- Transparent objects: Proper blending, correct depth ordering
- No double-rendering: Transparent objects only in ForwardPass, not GBufferPass

---

## Files Modified

| File | Change |
|------|--------|
| shaders/shader_io.h | Add alpha constants, extend DrawUniforms |
| shaders/shader.gbuffer.slang | Add alphaTestEnabled specialization constant, discard logic |
| shaders/shader.forward.slang | NEW: Forward rendering shader |
| loader/GltfLoader.h | Add alphaMode, alphaCutoff to GltfMaterialData |
| loader/GltfLoader.cpp | Parse glTF alphaMode/alphaCutoff |
| render/Renderer.h | Add alpha fields to MaterialRecord, new pipeline handles |
| render/Renderer.cpp | Create GBuffer variants, Forward pipeline, material upload changes |
| render/passes/ForwardPass.h | NEW: ForwardPass declaration |
| render/passes/ForwardPass.cpp | NEW: ForwardPass implementation |
| render/passes/GBufferPass.cpp | Skip transparent meshes, select pipeline variant |

---

## Implementation Order

1. shader_io.h: Add alpha constants, extend DrawUniforms
2. shader.gbuffer.slang: Add specialization constant and discard logic
3. shader.forward.slang: Create forward shader
4. GltfLoader: Parse alphaMode/alphaCutoff
5. MaterialRecord: Add alpha fields
6. Renderer: Create pipeline variants, Forward pipeline
7. ForwardPass: Create and integrate into pass execution
8. GBufferPass: Skip transparent, select variant
9. Testing with Sponza/Bistro