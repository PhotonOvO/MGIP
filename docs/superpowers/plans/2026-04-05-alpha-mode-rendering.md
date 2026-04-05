# Alpha Mode Rendering Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement proper alpha mode handling for glTF materials: Opaque, AlphaTest/Mask, and Transparent rendering with Deferred + Forward hybrid architecture.

**Architecture:** Use Specialization Constants for AlphaTest pipeline variants, separate ForwardPass for Transparent objects with depth sorting and alpha blending.

**Tech Stack:** Vulkan, Slang shaders, VMA, existing PassExecutor framework

---

## File Structure

| File | Responsibility |
|------|----------------|
| `shaders/shader_io.h` | Alpha constants, DrawUniforms extension |
| `shaders/shader.gbuffer.slang` | Alpha test specialization constant, discard logic |
| `shaders/shader.forward.slang` | NEW: Forward rendering for transparent objects |
| `loader/GltfLoader.h` | Add alphaMode/alphaCutoff to GltfMaterialData |
| `loader/GltfLoader.cpp` | Parse glTF alpha properties |
| `render/Renderer.h` | Extend MaterialRecord, MaterialTextureIndices, pipeline handles |
| `render/Renderer.cpp` | Create GBuffer variants, Forward pipeline, material upload |
| `render/passes/ForwardPass.h` | NEW: ForwardPass declaration |
| `render/passes/ForwardPass.cpp` | NEW: ForwardPass implementation with depth sorting |
| `render/passes/GBufferPass.cpp` | Skip transparent, select pipeline variant |

---

## Task 1: Extend shader_io.h with Alpha Mode Constants

**Files:**
- Modify: `shaders/shader_io.h`

- [ ] **Step 1: Add alpha mode constants**

Add after the existing vertex location constants (around line 117):

```cpp
// Alpha mode constants (matches glTF spec)
STATIC_CONST int LAlphaOpaque   = 0;
STATIC_CONST int LAlphaMask     = 1;
STATIC_CONST int LAlphaBlend    = 2;
```

- [ ] **Step 2: Extend DrawUniforms struct**

Replace the existing DrawUniforms struct (lines 52-64) with:

```cpp
// Per-draw uniform buffer (dynamic)
struct DrawUniforms
{
  mat4 modelMatrix;
  vec4 baseColorFactor;
  int32_t baseColorTextureIndex;   // Bindless texture index, -1 = no texture
  int32_t normalTextureIndex;      // -1 = no texture
  int32_t metallicRoughnessTextureIndex;  // -1 = no texture
  int32_t occlusionTextureIndex;   // -1 = no texture
  float metallicFactor;
  float roughnessFactor;
  float normalScale;
  int32_t alphaMode;      // 0=OPAQUE, 1=MASK, 2=BLEND
  float alphaCutoff;      // Default 0.5, used for MASK mode
  float _padding[2];
};
```

- [ ] **Step 3: Commit**

```bash
git add shaders/shader_io.h
git commit -m "feat(shaders): add alpha mode constants and extend DrawUniforms"
```

---

## Task 2: Add Alpha Test to GBuffer Shader

**Files:**
- Modify: `shaders/shader.gbuffer.slang`

- [ ] **Step 1: Add specialization constant**

Add after the resource bindings section (around line 43, after the `draw` binding):

```hlsl
// Specialization constant for alpha test (constant_id = 0)
[[vk::constant_id(0)]]
const bool alphaTestEnabled = false;
```

- [ ] **Step 2: Add alpha test logic in fragmentMain**

Add after the BaseColor sampling (after line 89, before normal mapping):

```hlsl
    // 2. Alpha test (specialization constant controlled)
    if (alphaTestEnabled)
    {
        if (color.a < draw.alphaCutoff)
        {
            discard;
        }
    }
```

- [ ] **Step 3: Commit**

```bash
git add shaders/shader.gbuffer.slang
git commit -m "feat(shader): add alpha test specialization constant to GBuffer shader"
```

---

## Task 3: Create Forward Shader

**Files:**
- Create: `shaders/shader.forward.slang`

- [ ] **Step 1: Create shader.forward.slang**

```hlsl
// Forward Pass Shader: Render transparent objects with alpha blending
// Used for BLEND alpha mode materials

#include "shader_io.h"

//------------------------------------------------------------------------------
// Specialization constant (consistent with GBuffer, but not used)
//------------------------------------------------------------------------------

[[vk::constant_id(0)]]
const bool alphaTestEnabled = false;

//------------------------------------------------------------------------------
// Input Structures
//------------------------------------------------------------------------------

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

//------------------------------------------------------------------------------
// Resource Bindings (same as GBuffer)
//------------------------------------------------------------------------------

[[vk::binding(LBindTextures, LSetTextures)]]
Sampler2D inTexture[];

[[vk::binding(LBindCamera, LSetScene)]]
ConstantBuffer<CameraUniforms> camera;

[[vk::binding(LBindDrawModel, LSetDraw)]]
ConstantBuffer<DrawUniforms> draw;

//------------------------------------------------------------------------------
// Vertex Shader
//------------------------------------------------------------------------------

[shader("vertex")]
VertexOutput vertexMain(VertexInput input)
{
    VertexOutput output;

    // Transform to clip space
    float4 worldPos = mul(draw.modelMatrix, float4(input.position, 1.0));
    output.position = mul(camera.viewProjection, worldPos);

    // Pass UV
    output.uv = input.texCoord;

    // Transform normal to world space
    output.worldNormal = normalize(mul(draw.modelMatrix, float4(input.normal, 0.0)).xyz);

    return output;
}

//------------------------------------------------------------------------------
// Fragment Shader (Output with alpha for blending)
//------------------------------------------------------------------------------

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

- [ ] **Step 2: Update CMakeLists.txt to embed the shader**

Find the shader embedding section in `CMakeLists.txt`. Look for where `shader_gbuffer_slang` is defined (around line 180-200). Add after the GBuffer shader definition:

```cmake
# Forward shader
set(SHADER_FORWARD_INPUT "${CMAKE_SOURCE_DIR}/shaders/shader.forward.slang")
set(SHADER_FORWARD_OUTPUT "${CMAKE_BINARY_DIR}/shaders/shader.forward.slang.h")
add_custom_command(
    OUTPUT ${SHADER_FORWARD_OUTPUT}
    COMMAND ${CMAKE_COMMAND} -E env "PATH=${SLANG_PATH};$ENV{PATH}"
        ${SLANGC_EXECUTABLE}
        -target spirv
        -O0
        -g
        -emit-spirv-directly
        -matrix-layout-column-major
        -force-glsl-scalar-layout
        -fvk-use-entrypoint-name
        ${SHADER_FORWARD_INPUT}
        -o ${SHADER_FORWARD_OUTPUT}
    DEPENDS ${SHADER_FORWARD_INPUT}
    COMMENT "Compiling Slang forward shader to SPIR-V header"
    VERBATIM
)
list(APPEND SHADER_HEADERS ${SHADER_FORWARD_OUTPUT})
```

The shader header will be generated and included automatically via the existing shader header inclusion mechanism.

- [ ] **Step 3: Commit**

```bash
git add shaders/shader.forward.slang CMakeLists.txt
git commit -m "feat(shader): add forward shader for transparent objects"
```

---

## Task 4: Parse Alpha Mode in GltfLoader

**Files:**
- Modify: `loader/GltfLoader.h`
- Modify: `loader/GltfLoader.cpp`

- [ ] **Step 1: Add alpha fields to GltfMaterialData**

In `loader/GltfLoader.h`, add to the GltfMaterialData struct (after `doubleSided`):

```cpp
    // Alpha mode (glTF spec)
    int alphaMode = 0;            // 0=OPAQUE, 1=MASK, 2=BLEND
    float alphaCutoff = 0.5f;     // for MASK mode
```

- [ ] **Step 2: Parse alpha mode in processMaterials**

In `loader/GltfLoader.cpp`, add after the emissive section in `processMaterials` (after line 351, before `outModel.materials.push_back`):

```cpp
        // Alpha mode (glTF spec)
        std::string alphaModeStr = mat.alphaMode;
        if (alphaModeStr == "MASK") {
            data.alphaMode = 1;  // LAlphaMask
            data.alphaCutoff = static_cast<float>(mat.alphaCutoff);
        } else if (alphaModeStr == "BLEND") {
            data.alphaMode = 2;  // LAlphaBlend
        } else {
            data.alphaMode = 0;  // LAlphaOpaque (default)
        }
```

- [ ] **Step 3: Commit**

```bash
git add loader/GltfLoader.h loader/GltfLoader.cpp
git commit -m "feat(loader): parse glTF alpha mode and cutoff"
```

---

## Task 5: Extend MaterialRecord and MaterialTextureIndices

**Files:**
- Modify: `render/Renderer.h`

- [ ] **Step 1: Add alpha fields to MaterialRecord**

In `render/Renderer.h`, find the `MaterialRecord` struct (around line 353) and add after `emissiveFactor`:

```cpp
      // Alpha properties
      int32_t alphaMode = 0;        // 0=OPAQUE, 1=MASK, 2=BLEND
      float alphaCutoff = 0.5f;     // for MASK mode
```

- [ ] **Step 2: Extend MaterialTextureIndices struct**

In `render/Renderer.h`, find the `MaterialTextureIndices` struct (around line 137) and replace with:

```cpp
  // Material texture indices struct for GBuffer rendering
  struct MaterialTextureIndices {
    int32_t baseColor = -1;
    int32_t normal = -1;
    int32_t metallicRoughness = -1;
    int32_t occlusion = -1;
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    float normalScale = 1.0f;
    int32_t alphaMode = 0;    // 0=OPAQUE, 1=MASK, 2=BLEND
    float alphaCutoff = 0.5f;
  };
```

- [ ] **Step 3: Add new pipeline handle declarations**

In `render/Renderer.h`, find the pipeline handle section (around line 309-314) and update:

```cpp
  // Light pipeline
  PipelineHandle m_lightPipeline{};
  PipelineHandle m_gbufferOpaquePipeline{};      // GBuffer Opaque variant
  PipelineHandle m_gbufferAlphaTestPipeline{};   // GBuffer AlphaTest variant
  PipelineHandle m_forwardPipeline{};            // Forward pass for transparent
```

- [ ] **Step 4: Add pipeline handle getter declarations**

In `render/Renderer.h`, find the pipeline getter section (around line 115-120) and add:

```cpp
  PipelineHandle getGBufferOpaquePipelineHandle() const;
  PipelineHandle getGBufferAlphaTestPipelineHandle() const;
  PipelineHandle getForwardPipelineHandle() const;
```

- [ ] **Step 5: Commit**

```bash
git add render/Renderer.h
git commit -m "feat(render): add alpha fields to material structures and pipeline handles"
```

---

## Task 6: Create GBuffer Pipeline Variants and Forward Pipeline

**Files:**
- Modify: `render/Renderer.cpp`

- [ ] **Step 1: Update getMaterialTextureIndices to include alpha**

Find `getMaterialTextureIndices` function (around line 2308) and add before the return statement:

```cpp
  // Fill alpha properties
  result.alphaMode = material->alphaMode;
  result.alphaCutoff = material->alphaCutoff;
```

- [ ] **Step 2: Replace single GBuffer pipeline with two variants**

Find the GBuffer pipeline creation section (around line 1477-1573). Replace the entire block with:

```cpp
  // Create GBuffer pipelines (Opaque + AlphaTest variants)
  {
    VkShaderModule gbufferShaderModule = utils::createShaderModule(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()),
                                                                {shader_gbuffer_slang, std::size(shader_gbuffer_slang)});
    DBG_VK_NAME(gbufferShaderModule);

    // GBuffer vertex input: Position(12) + Normal(12) + TexCoord(8) + Tangent(16) = 48 bytes
    const std::array<rhi::VertexBindingDesc, 1> gbufferBindings{{
        {.binding = 0, .stride = 48, .inputRate = rhi::VertexInputRate::perVertex}
    }};

    const std::array<rhi::VertexAttributeDesc, 4> gbufferAttributes{{
        {.location = shaderio::LVGltfPosition, .binding = 0, .format = rhi::VertexFormat::r32g32b32Sfloat, .offset = 0},    // Position
        {.location = shaderio::LVGltfNormal,   .binding = 0, .format = rhi::VertexFormat::r32g32b32Sfloat, .offset = 12},   // Normal
        {.location = shaderio::LVGltfTexCoord, .binding = 0, .format = rhi::VertexFormat::r32g32Sfloat,    .offset = 24},   // TexCoord
        {.location = shaderio::LVGltfTangent,  .binding = 0, .format = rhi::VertexFormat::r32g32b32a32Sfloat, .offset = 32}, // Tangent
    }};

    const rhi::VertexInputLayoutDesc gbufferVertexInput{
        .bindings       = gbufferBindings.data(),
        .bindingCount   = static_cast<uint32_t>(gbufferBindings.size()),
        .attributes     = gbufferAttributes.data(),
        .attributeCount = static_cast<uint32_t>(gbufferAttributes.size()),
    };

    const std::array<rhi::DynamicState, 2> gbufferDynamicStates{{
        rhi::DynamicState::viewport,
        rhi::DynamicState::scissor,
    }};

    // No blending for GBuffer
    const std::array<rhi::BlendAttachmentState, 3> gbufferBlendStates{{
        rhi::BlendAttachmentState{.blendEnable = false, .colorWriteMask = rhi::ColorComponentFlags::all},
        rhi::BlendAttachmentState{.blendEnable = false, .colorWriteMask = rhi::ColorComponentFlags::all},
        rhi::BlendAttachmentState{.blendEnable = false, .colorWriteMask = rhi::ColorComponentFlags::all},
    }};

    // GBuffer formats: 3 color attachments + depth
    const std::array<rhi::TextureFormat, 3> gbufferColorFormats{{
        rhi::TextureFormat::rgba8Unorm,  // GBuffer0: BaseColor
        rhi::TextureFormat::rgba8Unorm,  // GBuffer1: Normal
        rhi::TextureFormat::rgba8Unorm,  // GBuffer2: Material
    }};

    // Specialization constant for alpha test
    rhi::SpecializationConstant specConstantAlphaTest(0, 0, sizeof(bool));

    std::array<rhi::PipelineShaderStageDesc, 2> gbufferShaderStages{{
        rhi::PipelineShaderStageDesc{
            .stage = rhi::ShaderStage::vertex,
            .shaderModule = reinterpret_cast<uint64_t>(gbufferShaderModule),
            .entryPoint = "vertexMain",
        },
        rhi::PipelineShaderStageDesc{
            .stage = rhi::ShaderStage::fragment,
            .shaderModule = reinterpret_cast<uint64_t>(gbufferShaderModule),
            .entryPoint = "fragmentMain",
        },
    }};

    rhi::GraphicsPipelineDesc gbufferGraphicsDesc{
        .layout            = m_device.gbufferPipelineLayout.get(),
        .shaderStages      = gbufferShaderStages.data(),
        .shaderStageCount  = static_cast<uint32_t>(gbufferShaderStages.size()),
        .vertexInput       = gbufferVertexInput,
        .rasterState       = rhi::RasterState{},
        .depthState        = rhi::DepthState{true, true, rhi::CompareOp::less},
        .blendStates       = gbufferBlendStates.data(),
        .blendStateCount   = static_cast<uint32_t>(gbufferBlendStates.size()),
        .dynamicStates     = gbufferDynamicStates.data(),
        .dynamicStateCount = static_cast<uint32_t>(gbufferDynamicStates.size()),
        .renderingInfo =
            {
                .colorFormats     = gbufferColorFormats.data(),
                .colorFormatCount = static_cast<uint32_t>(gbufferColorFormats.size()),
                .depthFormat      = rhi::TextureFormat::d32Sfloat,
            },
    };
    gbufferGraphicsDesc.rasterState.topology    = rhi::PrimitiveTopology::triangleList;
    gbufferGraphicsDesc.rasterState.cullMode    = rhi::CullMode::back;
    gbufferGraphicsDesc.rasterState.frontFace   = rhi::FrontFace::counterClockwise;
    gbufferGraphicsDesc.rasterState.sampleCount = rhi::SampleCount::count1;

    // Variant 0: Opaque (alphaTestEnabled = false)
    bool alphaTestFalse = false;
    rhi::SpecializationData specDataFalse{&alphaTestFalse, sizeof(bool)};
    gbufferShaderStages[1].specializationData = specDataFalse;
    gbufferShaderStages[1].specializationConstants = &specConstantAlphaTest;
    gbufferShaderStages[1].specializationConstantCount = 1;

    rhi::vulkan::GraphicsPipelineCreateInfo gbufferCreateInfo{
        .key =
            {
                .shaderIdentity        = rhi::vulkan::PipelineShaderIdentity::raster,
                .specializationVariant = 3,  // GBuffer Opaque variant
            },
        .desc = gbufferGraphicsDesc,
    };
    const VkPipeline gbufferOpaquePipeline =
        rhi::vulkan::createGraphicsPipeline(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()), gbufferCreateInfo);
    DBG_VK_NAME(gbufferOpaquePipeline);
    m_gbufferOpaquePipeline = registerPipeline(static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_GRAPHICS),
                                               reinterpret_cast<uint64_t>(gbufferOpaquePipeline),
                                               gbufferCreateInfo.key.specializationVariant);

    // Variant 1: AlphaTest (alphaTestEnabled = true)
    bool alphaTestTrue = true;
    rhi::SpecializationData specDataTrue{&alphaTestTrue, sizeof(bool)};
    gbufferShaderStages[1].specializationData = specDataTrue;
    gbufferCreateInfo.key.specializationVariant = 4;  // GBuffer AlphaTest variant

    const VkPipeline gbufferAlphaTestPipeline =
        rhi::vulkan::createGraphicsPipeline(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()), gbufferCreateInfo);
    DBG_VK_NAME(gbufferAlphaTestPipeline);
    m_gbufferAlphaTestPipeline = registerPipeline(static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_GRAPHICS),
                                                   reinterpret_cast<uint64_t>(gbufferAlphaTestPipeline),
                                                   gbufferCreateInfo.key.specializationVariant);

    vkDestroyShaderModule(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()), gbufferShaderModule, nullptr);
  }
```

- [ ] **Step 3: Create Forward pipeline**

Add after the GBuffer pipeline creation section:

```cpp
  // Create Forward pipeline for transparent objects
  {
    VkShaderModule forwardShaderModule = utils::createShaderModule(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()),
                                                                {shader_forward_slang, std::size(shader_forward_slang)});
    DBG_VK_NAME(forwardShaderModule);

    // Same vertex input as GBuffer
    const std::array<rhi::VertexBindingDesc, 1> forwardBindings{{
        {.binding = 0, .stride = 48, .inputRate = rhi::VertexInputRate::perVertex}
    }};

    const std::array<rhi::VertexAttributeDesc, 4> forwardAttributes{{
        {.location = shaderio::LVGltfPosition, .binding = 0, .format = rhi::VertexFormat::r32g32b32Sfloat, .offset = 0},
        {.location = shaderio::LVGltfNormal,   .binding = 0, .format = rhi::VertexFormat::r32g32b32Sfloat, .offset = 12},
        {.location = shaderio::LVGltfTexCoord, .binding = 0, .format = rhi::VertexFormat::r32g32Sfloat,    .offset = 24},
        {.location = shaderio::LVGltfTangent,  .binding = 0, .format = rhi::VertexFormat::r32g32b32a32Sfloat, .offset = 32},
    }};

    const rhi::VertexInputLayoutDesc forwardVertexInput{
        .bindings       = forwardBindings.data(),
        .bindingCount   = static_cast<uint32_t>(forwardBindings.size()),
        .attributes     = forwardAttributes.data(),
        .attributeCount = static_cast<uint32_t>(forwardAttributes.size()),
    };

    const std::array<rhi::DynamicState, 2> forwardDynamicStates{{
        rhi::DynamicState::viewport,
        rhi::DynamicState::scissor,
    }};

    // Alpha blending for transparent objects
    const rhi::BlendAttachmentState forwardBlend{
        .blendEnable = true,
        .srcBlend = rhi::BlendFactor::srcAlpha,
        .dstBlend = rhi::BlendFactor::oneMinusSrcAlpha,
        .blendOp = rhi::BlendOp::add,
        .srcBlendAlpha = rhi::BlendFactor::one,
        .dstBlendAlpha = rhi::BlendFactor::oneMinusSrcAlpha,
        .blendOpAlpha = rhi::BlendOp::add,
        .colorWriteMask = rhi::ColorComponentFlags::all,
    };

    std::array<rhi::PipelineShaderStageDesc, 2> forwardShaderStages{{
        rhi::PipelineShaderStageDesc{
            .stage = rhi::ShaderStage::vertex,
            .shaderModule = reinterpret_cast<uint64_t>(forwardShaderModule),
            .entryPoint = "vertexMain",
        },
        rhi::PipelineShaderStageDesc{
            .stage = rhi::ShaderStage::fragment,
            .shaderModule = reinterpret_cast<uint64_t>(forwardShaderModule),
            .entryPoint = "fragmentMain",
        },
    }};

    // Render to swapchain format
    const rhi::TextureFormat swapchainFormat = toPortableTextureFormat(m_swapchainDependent.swapchainImageFormat);

    rhi::GraphicsPipelineDesc forwardGraphicsDesc{
        .layout            = m_device.gbufferPipelineLayout.get(),
        .shaderStages      = forwardShaderStages.data(),
        .shaderStageCount  = static_cast<uint32_t>(forwardShaderStages.size()),
        .vertexInput       = forwardVertexInput,
        .rasterState       = rhi::RasterState{},
        .depthState        = rhi::DepthState{true, false, rhi::CompareOp::lessOrEqual},  // Test enabled, write disabled
        .blendStates       = &forwardBlend,
        .blendStateCount   = 1,
        .dynamicStates     = forwardDynamicStates.data(),
        .dynamicStateCount = static_cast<uint32_t>(forwardDynamicStates.size()),
        .renderingInfo =
            {
                .colorFormats     = &swapchainFormat,
                .colorFormatCount = 1,
                .depthFormat      = rhi::TextureFormat::d32Sfloat,
            },
    };
    forwardGraphicsDesc.rasterState.topology    = rhi::PrimitiveTopology::triangleList;
    forwardGraphicsDesc.rasterState.cullMode    = rhi::CullMode::back;
    forwardGraphicsDesc.rasterState.frontFace   = rhi::FrontFace::counterClockwise;
    forwardGraphicsDesc.rasterState.sampleCount = rhi::SampleCount::count1;

    rhi::vulkan::GraphicsPipelineCreateInfo forwardCreateInfo{
        .key =
            {
                .shaderIdentity        = rhi::vulkan::PipelineShaderIdentity::raster,
                .specializationVariant = 5,  // Forward variant
            },
        .desc = forwardGraphicsDesc,
    };
    const VkPipeline forwardPipeline =
        rhi::vulkan::createGraphicsPipeline(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()), forwardCreateInfo);
    DBG_VK_NAME(forwardPipeline);
    m_forwardPipeline = registerPipeline(static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_GRAPHICS),
                                         reinterpret_cast<uint64_t>(forwardPipeline),
                                         forwardCreateInfo.key.specializationVariant);

    vkDestroyShaderModule(fromNativeHandle<VkDevice>(m_device.device->getNativeDevice()), forwardShaderModule, nullptr);
  }
```

- [ ] **Step 4: Update material upload to store alpha properties**

Find the material upload section in `uploadGltfModel` and add after setting other material properties:

```cpp
      record.alphaMode = matData.alphaMode;
      record.alphaCutoff = matData.alphaCutoff;
```

- [ ] **Step 5: Implement pipeline handle getters**

Add at the end of the file (before the closing namespace):

```cpp
PipelineHandle Renderer::getGBufferOpaquePipelineHandle() const
{
  return m_gbufferOpaquePipeline;
}

PipelineHandle Renderer::getGBufferAlphaTestPipelineHandle() const
{
  return m_gbufferAlphaTestPipeline;
}

PipelineHandle Renderer::getForwardPipelineHandle() const
{
  return m_forwardPipeline;
}
```

- [ ] **Step 6: Remove old m_gbufferPipeline references**

Find and remove any references to `m_gbufferPipeline` (the old single pipeline handle) and replace with the new variant handles.

- [ ] **Step 7: Commit**

```bash
git add render/Renderer.cpp
git commit -m "feat(render): create GBuffer pipeline variants and Forward pipeline"
```

---

## Task 7: Create ForwardPass

**Files:**
- Create: `render/passes/ForwardPass.h`
- Create: `render/passes/ForwardPass.cpp`

- [ ] **Step 1: Create ForwardPass.h**

```cpp
#pragma once

#include "../Pass.h"

namespace demo {

class Renderer;

class ForwardPass : public RenderPassNode {
public:
    explicit ForwardPass(Renderer* renderer);
    ~ForwardPass() override = default;

    [[nodiscard]] const char* getName() const override { return "ForwardPass"; }
    [[nodiscard]] HandleSlice<PassResourceDependency> getDependencies() const override;
    void execute(const PassContext& context) const override;

private:
    Renderer* m_renderer;
};

}  // namespace demo
```

- [ ] **Step 2: Create ForwardPass.cpp**

```cpp
#include "ForwardPass.h"
#include "../Renderer.h"
#include "../MeshPool.h"
#include "../SceneResources.h"
#include "../../shaders/shader_io.h"
#include "../../rhi/vulkan/VulkanCommandList.h"
#include "../../rhi/vulkan/VulkanDescriptor.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace demo {

ForwardPass::ForwardPass(Renderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> ForwardPass::getDependencies() const
{
    static const std::array<PassResourceDependency, 2> dependencies = {
        PassResourceDependency::texture(
            kPassSwapchainHandle,
            ResourceAccess::readWrite,  // Read existing LightPass output, write blended result
            rhi::ShaderStage::fragment
        ),
        PassResourceDependency::texture(
            kPassGBufferDepthHandle,
            ResourceAccess::read,
            rhi::ShaderStage::fragment
        ),
    };
    return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void ForwardPass::execute(const PassContext& context) const
{
    if(m_renderer == nullptr || context.params == nullptr || context.transientAllocator == nullptr)
    {
        return;
    }

    context.cmd->beginEvent("ForwardPass");

    // Get swapchain image view and extent
    const VkImageView swapchainImageView = m_renderer->getCurrentSwapchainImageView();
    const VkExtent2D extent = m_renderer->getSwapchainExtent();

    if(swapchainImageView == VK_NULL_HANDLE)
    {
        context.cmd->endEvent();
        return;
    }

    // Check if we have transparent meshes to render
    if(context.gltfModel == nullptr || context.gltfModel->meshes.empty())
    {
        context.cmd->endEvent();
        return;
    }

    MeshPool& meshPool = m_renderer->getMeshPool();

    // Collect transparent meshes and sort by distance
    std::vector<std::pair<size_t, float>> transparentMeshes;

    glm::vec3 cameraPos(0.0f);
    if(context.params->cameraUniforms != nullptr)
    {
        cameraPos = context.params->cameraUniforms->cameraPosition;
    }

    for(size_t i = 0; i < context.gltfModel->meshes.size(); ++i)
    {
        MeshHandle meshHandle = context.gltfModel->meshes[i];
        const MeshRecord* mesh = meshPool.tryGet(meshHandle);
        if(mesh == nullptr) continue;

        // Check material alpha mode
        bool isTransparent = false;
        if(mesh->materialIndex >= 0 && mesh->materialIndex < static_cast<int32_t>(context.gltfModel->materials.size()))
        {
            MaterialHandle matHandle = context.gltfModel->materials[mesh->materialIndex];
            auto indices = m_renderer->getMaterialTextureIndices(matHandle, context.gltfModel);
            if(indices.alphaMode == shaderio::LAlphaBlend)
            {
                isTransparent = true;
            }
        }

        if(!isTransparent) continue;

        // Calculate distance from camera (use mesh center)
        glm::vec3 meshCenter = glm::vec3(mesh->transform[3]);
        float distance = glm::length(meshCenter - cameraPos);
        transparentMeshes.push_back({i, distance});
    }

    // No transparent meshes, skip rendering
    if(transparentMeshes.empty())
    {
        context.cmd->endEvent();
        return;
    }

    // Sort far to near (back to front)
    std::sort(transparentMeshes.begin(), transparentMeshes.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; });

    // Setup color attachment with LOAD loadOp (preserve LightPass output)
    const VkRenderingAttachmentInfo colorAttachment{
        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView   = swapchainImageView,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD,  // Preserve existing content
        .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
    };

    // Setup depth attachment (read-only, for depth test)
    SceneResources& sceneResources = m_renderer->getSceneResources();
    const VkRenderingAttachmentInfo depthAttachment{
        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView   = sceneResources.getDepthImageView(),
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD,  // Read existing depth
        .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
    };

    // Begin dynamic rendering
    const VkRenderingInfo renderingInfo{
        .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea           = {{0, 0}, extent},
        .layerCount           = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments    = &colorAttachment,
        .pDepthAttachment     = &depthAttachment,
    };
    rhi::vulkan::cmdBeginRendering(*context.cmd, renderingInfo);

    // Set viewport and scissor
    const VkViewport viewport{
        0.0f, 0.0f,
        static_cast<float>(extent.width),
        static_cast<float>(extent.height),
        0.0f, 1.0f
    };
    const VkRect2D scissor{{0, 0}, extent};
    rhi::vulkan::cmdSetViewport(*context.cmd, viewport);
    rhi::vulkan::cmdSetScissor(*context.cmd, scissor);

    // Bind Forward pipeline
    const PipelineHandle forwardPipeline = m_renderer->getForwardPipelineHandle();
    if(forwardPipeline.isNull())
    {
        rhi::vulkan::cmdEndRendering(*context.cmd);
        context.cmd->endEvent();
        return;
    }

    const VkPipeline nativePipeline = reinterpret_cast<VkPipeline>(
        m_renderer->getPipelineOpaque(forwardPipeline, static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_GRAPHICS)));
    rhi::vulkan::cmdBindPipeline(*context.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, nativePipeline);

    // Get pipeline layout for descriptor set binding
    const VkPipelineLayout pipelineLayout = reinterpret_cast<VkPipelineLayout>(
        m_renderer->getGBufferPipelineLayout());

    // Bind texture descriptor set (set 0)
    const VkDescriptorSet textureSet = reinterpret_cast<VkDescriptorSet>(
        m_renderer->getGBufferColorDescriptorSet());
    vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd),
                            VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                            shaderio::LSetTextures, 1, &textureSet, 0, nullptr);

    // Allocate CameraUniforms from transient allocator
    const uint32_t cameraAlignment = 256;
    const TransientAllocator::Allocation cameraAlloc =
        context.transientAllocator->allocate(sizeof(shaderio::CameraUniforms), cameraAlignment);

    // Setup camera matrices
    shaderio::CameraUniforms cameraData{};
    if(context.params->cameraUniforms != nullptr)
    {
        cameraData = *context.params->cameraUniforms;
    }
    else
    {
        cameraData.view = glm::lookAt(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        cameraData.projection = glm::perspective(glm::radians(45.0f),
            static_cast<float>(extent.width) / static_cast<float>(extent.height), 0.1f, 100.0f);
        cameraData.projection[1][1] *= -1.0f;
        cameraData.viewProjection = cameraData.projection * cameraData.view;
        cameraData.cameraPosition = glm::vec3(0.0f, 0.0f, 3.0f);
    }

    std::memcpy(cameraAlloc.cpuPtr, &cameraData, sizeof(cameraData));
    context.transientAllocator->flushAllocation(cameraAlloc, sizeof(cameraData));

    // Get camera descriptor set
    const BindGroupHandle cameraBindGroupHandle = m_renderer->getCameraBindGroup();
    if(!cameraBindGroupHandle.isNull())
    {
        uint64_t cameraSetOpaque = m_renderer->getBindGroupDescriptorSet(cameraBindGroupHandle, BindGroupSetSlot::shaderSpecific);
        VkDescriptorSet cameraDescriptorSet = reinterpret_cast<VkDescriptorSet>(cameraSetOpaque);

        VkDescriptorBufferInfo bufferInfo{
            .buffer = reinterpret_cast<VkBuffer>(context.transientAllocator->getBufferOpaque()),
            .offset = cameraAlloc.offset,
            .range  = sizeof(shaderio::CameraUniforms),
        };
        VkWriteDescriptorSet write{
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = cameraDescriptorSet,
            .dstBinding      = shaderio::LBindCamera,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pBufferInfo     = &bufferInfo,
        };
        vkUpdateDescriptorSets(reinterpret_cast<VkDevice>(m_renderer->getDeviceOpaque()), 1, &write, 0, nullptr);

        // Bind camera descriptor set (set 1)
        vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd),
                                VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                                shaderio::LSetScene, 1, &cameraDescriptorSet, 0, nullptr);
    }

    // Get draw descriptor set
    const BindGroupHandle drawBindGroupHandle = m_renderer->getDrawBindGroup();

    // Render transparent meshes (sorted back to front)
    for(const auto& [meshIndex, distance] : transparentMeshes)
    {
        MeshHandle meshHandle = context.gltfModel->meshes[meshIndex];
        const MeshRecord* mesh = meshPool.tryGet(meshHandle);
        if(mesh == nullptr) continue;

        // Allocate DrawUniforms
        const uint32_t drawAlignment = 256;
        const TransientAllocator::Allocation drawAlloc =
            context.transientAllocator->allocate(sizeof(shaderio::DrawUniforms), drawAlignment);

        // Setup draw uniforms
        shaderio::DrawUniforms drawData{};
        drawData.modelMatrix = mesh->transform;

        drawData.baseColorFactor = glm::vec4(1.0f);
        drawData.baseColorTextureIndex = -1;
        drawData.normalTextureIndex = -1;
        drawData.metallicRoughnessTextureIndex = -1;
        drawData.occlusionTextureIndex = -1;
        drawData.metallicFactor = 1.0f;
        drawData.roughnessFactor = 1.0f;
        drawData.normalScale = 1.0f;
        drawData.alphaMode = shaderio::LAlphaBlend;
        drawData.alphaCutoff = 0.5f;

        if(mesh->materialIndex >= 0 && mesh->materialIndex < static_cast<int32_t>(context.gltfModel->materials.size()))
        {
            MaterialHandle matHandle = context.gltfModel->materials[mesh->materialIndex];
            drawData.baseColorFactor = m_renderer->getMaterialBaseColorFactor(matHandle);

            auto texIndices = m_renderer->getMaterialTextureIndices(matHandle, context.gltfModel);
            drawData.baseColorTextureIndex = texIndices.baseColor;
            drawData.normalTextureIndex = texIndices.normal;
            drawData.metallicRoughnessTextureIndex = texIndices.metallicRoughness;
            drawData.occlusionTextureIndex = texIndices.occlusion;
            drawData.metallicFactor = texIndices.metallicFactor;
            drawData.roughnessFactor = texIndices.roughnessFactor;
            drawData.normalScale = texIndices.normalScale;
            drawData.alphaMode = texIndices.alphaMode;
            drawData.alphaCutoff = texIndices.alphaCutoff;
        }

        std::memcpy(drawAlloc.cpuPtr, &drawData, sizeof(drawData));
        context.transientAllocator->flushAllocation(drawAlloc, sizeof(drawData));

        // Update draw descriptor
        if(!drawBindGroupHandle.isNull())
        {
            uint64_t drawSetOpaque = m_renderer->getBindGroupDescriptorSet(drawBindGroupHandle, BindGroupSetSlot::shaderSpecific);
            VkDescriptorSet drawDescriptorSet = reinterpret_cast<VkDescriptorSet>(drawSetOpaque);

            VkDescriptorBufferInfo bufferInfo{
                .buffer = reinterpret_cast<VkBuffer>(context.transientAllocator->getBufferOpaque()),
                .offset = drawAlloc.offset,
                .range  = sizeof(shaderio::DrawUniforms),
            };
            VkWriteDescriptorSet write{
                .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet          = drawDescriptorSet,
                .dstBinding      = shaderio::LBindDrawModel,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .pBufferInfo     = &bufferInfo,
            };
            vkUpdateDescriptorSets(reinterpret_cast<VkDevice>(m_renderer->getDeviceOpaque()), 1, &write, 0, nullptr);

            // Bind draw descriptor set (set 2)
            vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd),
                                    VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                                    shaderio::LSetDraw, 1, &drawDescriptorSet, 0, nullptr);
        }

        // Bind vertex and index buffers
        const VkDeviceSize vertexOffset = 0;
        rhi::vulkan::cmdBindVertexBuffers(*context.cmd, 0, 1, &mesh->vertexBuffer, &vertexOffset);
        rhi::vulkan::cmdBindIndexBuffer(*context.cmd, mesh->indexBuffer, 0, VK_INDEX_TYPE_UINT32);

        // Draw indexed
        rhi::vulkan::cmdDrawIndexed(*context.cmd, mesh->indexCount, 1, 0, 0, 0);
    }

    rhi::vulkan::cmdEndRendering(*context.cmd);

    context.cmd->endEvent();
}

}  // namespace demo
```

- [ ] **Step 3: Add to CMakeLists.txt**

Add to the render source list in `CMakeLists.txt`:

```cmake
render/passes/ForwardPass.cpp
render/passes/ForwardPass.h
```

- [ ] **Step 4: Commit**

```bash
git add render/passes/ForwardPass.h render/passes/ForwardPass.cpp CMakeLists.txt
git commit -m "feat(render): add ForwardPass for transparent object rendering"
```

---

## Task 8: Integrate ForwardPass into Renderer

**Files:**
- Modify: `render/Renderer.h`
- Modify: `render/Renderer.cpp`

- [ ] **Step 1: Add ForwardPass member to Renderer**

In `render/Renderer.h`, find the pass declarations (around line 296-302) and add:

```cpp
  std::unique_ptr<ForwardPass>           m_forwardPass;
```

- [ ] **Step 2: Include ForwardPass header**

In `render/Renderer.h`, add to the includes section:

```cpp
#include "passes/ForwardPass.h"
```

- [ ] **Step 3: Create ForwardPass in init()**

In `render/Renderer.cpp`, find the pass creation section (around line 478) and add after `m_lightPass` creation:

```cpp
  m_forwardPass          = std::make_unique<ForwardPass>(this);
```

- [ ] **Step 4: Add ForwardPass to pass executor**

In `render/Renderer.cpp`, find the pass executor setup and add after `m_lightPass`:

```cpp
  m_passExecutor.addPass(m_forwardPass.get());
```

- [ ] **Step 5: Commit**

```bash
git add render/Renderer.h render/Renderer.cpp
git commit -m "feat(render): integrate ForwardPass into pass execution"
```

---

## Task 9: Update GBufferPass for Alpha Mode Handling

**Files:**
- Modify: `render/passes/GBufferPass.cpp`

- [ ] **Step 1: Skip transparent meshes**

In `GBufferPass::execute()`, inside the mesh loop (around line 189), add after getting the mesh pointer:

```cpp
                // Skip transparent meshes (handled by ForwardPass)
                if(mesh->materialIndex >= 0 && mesh->materialIndex < static_cast<int32_t>(context.gltfModel->materials.size()))
                {
                    MaterialHandle matHandle = context.gltfModel->materials[mesh->materialIndex];
                    auto indices = m_renderer->getMaterialTextureIndices(matHandle, context.gltfModel);
                    if(indices.alphaMode == shaderio::LAlphaBlend)
                    {
                        continue;  // Transparent meshes rendered in ForwardPass
                    }
                }
```

- [ ] **Step 2: Select pipeline variant based on alpha mode**

Replace the pipeline binding section (around line 104-109) with:

```cpp
            // Select pipeline variant based on alpha mode
            PipelineHandle gbufferPipeline;
            bool isAlphaTest = false;

            if(mesh->materialIndex >= 0 && mesh->materialIndex < static_cast<int32_t>(context.gltfModel->materials.size()))
            {
                MaterialHandle matHandle = context.gltfModel->materials[mesh->materialIndex];
                auto indices = m_renderer->getMaterialTextureIndices(matHandle, context.gltfModel);
                if(indices.alphaMode == shaderio::LAlphaMask)
                {
                    isAlphaTest = true;
                }
            }

            if(isAlphaTest)
            {
                gbufferPipeline = m_renderer->getGBufferAlphaTestPipelineHandle();
            }
            else
            {
                gbufferPipeline = m_renderer->getGBufferOpaquePipelineHandle();
            }

            if(gbufferPipeline.isNull())
            {
                continue;
            }

            const VkPipeline nativePipeline = reinterpret_cast<VkPipeline>(
                m_renderer->getPipelineOpaque(gbufferPipeline, static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_GRAPHICS)));
            rhi::vulkan::cmdBindPipeline(*context.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, nativePipeline);
```

- [ ] **Step 3: Update DrawUniforms to include alpha fields**

In the DrawUniforms setup section (around line 204-231), add after setting normalScale:

```cpp
                drawData.alphaMode = indices.alphaMode;
                drawData.alphaCutoff = indices.alphaCutoff;
```

- [ ] **Step 4: Commit**

```bash
git add render/passes/GBufferPass.cpp
git commit -m "feat(render): add alpha mode handling to GBufferPass"
```

---

## Task 10: Build and Test

**Files:**
- All modified files

- [ ] **Step 1: Build the project**

```bash
cd H:/GitHub/VKDemo/build
cmake --build . --config Debug 2>&1
```

Expected: Build succeeds with no errors.

- [ ] **Step 2: Run the application**

```bash
cd H:/GitHub/VKDemo/build/Debug
./Demo.exe
```

- [ ] **Step 3: Test with Sponza/Bistro models**

1. Load Sponza or Bistro model using the dropdown menu
2. Verify opaque objects render correctly (walls, floors, columns)
3. Verify alpha-test objects render correctly (leaves, vegetation) - clean edges, no artifacts
4. Verify transparent objects render correctly (curtains, glass) - proper blending, correct depth ordering

- [ ] **Step 4: Final commit**

```bash
git add -A
git commit -m "feat(render): complete alpha mode rendering implementation"
```

---

## Summary

This implementation adds:

1. **Alpha mode constants** in shader_io.h
2. **Specialization constant** for alpha test in GBuffer shader
3. **Forward shader** for transparent objects
4. **glTF alpha mode parsing** in GltfLoader
5. **Material alpha properties** in MaterialRecord and MaterialTextureIndices
6. **Two GBuffer pipeline variants** (Opaque and AlphaTest) using specialization constants
7. **Forward pipeline** with alpha blending for transparent objects
8. **ForwardPass** with depth sorting for transparent mesh rendering
9. **Updated GBufferPass** to skip transparent meshes and select correct pipeline variant

The rendering flow becomes:
```
GBufferPass (Opaque + AlphaTest) → LightPass → ForwardPass (Transparent) → PresentPass
```