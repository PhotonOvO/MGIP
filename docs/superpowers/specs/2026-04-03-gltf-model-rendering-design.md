# glTF Model Rendering Design

> **Date**: 2026-04-03
> **Status**: Draft
> **Author**: Claude

---

## 1. Overview

### 1.1 Goal

Extend the existing VKDemo renderer to support glTF 2.0 model rendering with a simple GBuffer + LightPass pipeline.

### 1.2 Scope

- **In Scope**:
  - glTF 2.0 core features: meshes, materials, base color textures
  - GBufferPass: render models to GBuffer (Color + Depth)
  - LightPass: output GBuffer Color to Swapchain
  - UI for model selection

- **Out of Scope**:
  - PBR materials (metallic, roughness, normal maps)
  - Skeletal animation
  - Morph targets
  - Complex lighting (point lights, shadows, etc.)

### 1.3 Key Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| GBuffer Layout | Simple (Color + Depth) | YAGNI: meets current needs |
| glTF Loader | tinygltf | Single-header, proven library |
| Lighting Model | Pass-through | First iteration: no actual lighting |
| Model Selection | UI-based | Flexible for testing |

---

## 2. Architecture

### 2.1 Module Dependency Graph

```
MinimalLatestApp
    │
    ├── GltfLoader ──────┐
    │       │            │
    │       ▼            ▼
    ├── MeshPool ◄──── Renderer
    │                    │
    │                    ├── GBufferPass ──► SceneResources (Color+Depth)
    │                    │         │
    │                    │         └── DrawStreamWriter
    │                    │
    │                    └── LightPass ──► Swapchain
    │                              │
    │                              └── Sample GBuffer Color
    │
    └── PresentPass ──► Swapchain (dynamic rendering begin)
```

### 2.2 Pass Execution Order

```
Frame Rendering:
    1. GBufferPass      → GBuffer (Color + Depth)
    2. PresentPass      → Begin Swapchain dynamic rendering
    3. LightPass        → Fullscreen quad → Swapchain
    4. ImguiPass        → UI → Swapchain, end dynamic rendering
```

### 2.3 Resource Lifetime

| Resource | Lifetime | Owner |
|----------|----------|-------|
| MeshPool | Device lifetime | Renderer |
| GltfLoader | Application lifetime | MinimalLatestApp |
| GltfUploadResult | Model lifetime | MinimalLatestApp |
| Light Pipeline | Device lifetime | Renderer |

---

## 3. Component Specifications

### 3.1 GltfLoader

**File**: `loader/GltfLoader.h`, `loader/GltfLoader.cpp`

**Responsibility**: Load glTF files and return normalized data structures.

**Interface**:

```cpp
namespace demo {

struct GltfMeshData {
    std::vector<float> positions;      // x,y,z per vertex
    std::vector<float> normals;        // x,y,z per vertex
    std::vector<float> texCoords;      // u,v per vertex
    std::vector<uint32_t> indices;
    int materialIndex = -1;            // -1 = default material
};

struct GltfMaterialData {
    int baseColorTexture = -1;         // -1 = no texture
    glm::vec4 baseColorFactor = glm::vec4(1.0f);
};

struct GltfImageData {
    std::vector<uint8_t> pixels;
    int width = 0;
    int height = 0;
    int channels = 0;
};

struct GltfModel {
    std::vector<GltfMeshData> meshes;
    std::vector<GltfMaterialData> materials;
    std::vector<GltfImageData> images;
    std::string name;
};

class GltfLoader {
public:
    bool load(const std::string& filepath, GltfModel& outModel);
    const std::string& getLastError() const { return m_lastError; }
private:
    std::string m_lastError;
};

}  // namespace demo
```

**Implementation Notes**:
- Use tinygltf library (single-header)
- Convert all indices to uint32_t
- Flip Y coordinate for UV (glTF convention vs Vulkan)
- Handle missing normals/texCoords gracefully

---

### 3.2 MeshPool

**File**: `render/MeshPool.h`, `render/MeshPool.cpp`

**Responsibility**: Manage GPU mesh resources with Handle-based access.

**Interface**:

```cpp
namespace demo {

struct MeshRecord {
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VmaAllocation vertexAllocation = nullptr;
    VmaAllocation indexAllocation = nullptr;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t vertexStride = 0;
    glm::mat4 transform = glm::mat4(1.0f);
};

class MeshPool {
public:
    void init(VkDevice device, VmaAllocator allocator);
    void deinit();

    MeshHandle uploadMesh(const GltfMeshData& meshData, VkCommandBuffer cmd);
    void destroyMesh(MeshHandle handle);
    const MeshRecord* tryGet(MeshHandle handle) const;

    template<typename Fn>
    void forEachActive(Fn&& fn);

private:
    VkDevice m_device = VK_NULL_HANDLE;
    VmaAllocator m_allocator = nullptr;
    HandlePool<MeshHandle, MeshRecord> m_pool;
};

}  // namespace demo
```

**Vertex Format**:

```
Interleaved vertex buffer:
[Position(12B) | Normal(12B) | TexCoord(8B)] per vertex
Stride = 32 bytes
```

---

### 3.3 GBufferPass

**File**: `render/passes/GBufferPass.h`, `render/passes/GBufferPass.cpp`

**Responsibility**: Render glTF meshes to GBuffer using DrawStreamWriter.

**Interface**:

```cpp
namespace demo {

class GBufferPass : public RenderPassNode {
public:
    explicit GBufferPass(Renderer* renderer);

    [[nodiscard]] const char* getName() const override { return "GBufferPass"; }
    [[nodiscard]] HandleSlice<PassResourceDependency> getDependencies() const override;
    void execute(const PassContext& context) const override;

    void setModel(const Renderer::GltfUploadResult* modelData);

private:
    Renderer* m_renderer;
    const Renderer::GltfUploadResult* m_modelData = nullptr;
};

}  // namespace demo
```

**Dependencies**:

```cpp
// Buffer: Vertex input
PassResourceDependency::buffer(kPassVertexBufferHandle, ResourceAccess::read, rhi::ShaderStage::vertex)

// Texture: GBuffer Color output
PassResourceDependency::texture(kPassGBufferColorHandle, ResourceAccess::write, rhi::ShaderStage::fragment)

// Texture: GBuffer Depth output
PassResourceDependency::texture(kPassGBufferDepthHandle, ResourceAccess::write, rhi::ShaderStage::fragment)
```

**Execution Flow**:

1. Iterate over meshes in model data
2. For each mesh:
   - Set pipeline (textured or non-textured variant)
   - Set material index
   - Set mesh handle
   - Set dynamic UBO offset
   - Emit draw call
3. Pass DrawStream to Renderer::executeGraphicsPass()

---

### 3.4 LightPass

**File**: `render/passes/LightPass.h`, `render/passes/LightPass.cpp`

**Responsibility**: Sample GBuffer Color and output to Swapchain.

**Interface**:

```cpp
namespace demo {

class LightPass : public RenderPassNode {
public:
    explicit LightPass(Renderer* renderer);

    [[nodiscard]] const char* getName() const override { return "LightPass"; }
    [[nodiscard]] HandleSlice<PassResourceDependency> getDependencies() const override;
    void execute(const PassContext& context) const override;

private:
    Renderer* m_renderer;
};

}  // namespace demo
```

**Dependencies**:

```cpp
// Texture: GBuffer Color input
PassResourceDependency::texture(kPassGBufferColorHandle, ResourceAccess::read, rhi::ShaderStage::fragment)

// Texture: Swapchain output
PassResourceDependency::texture(kPassSwapchainHandle, ResourceAccess::write, rhi::ShaderStage::fragment)
```

**Execution Flow**:

1. Set viewport/scissor to swapchain extent
2. Bind light pipeline
3. Bind GBuffer Color as input texture
4. Draw fullscreen triangle (3 vertices)
5. Output goes directly to swapchain (within PresentPass's render pass)

**Fullscreen Triangle Technique**:

```
Vertices (clip space):
  (-1, -1) ─────── (3, -1)
      │            ／
      │         ／
      │      ／
      │   ／
  (-1, 3)

UV calculation in shader: uv = position.xy * 0.5 + 0.5
```

---

### 3.5 Renderer Extensions

**File**: `render/Renderer.h`, `render/Renderer.cpp`

**New Methods**:

```cpp
class Renderer {
public:
    // glTF resource management
    GltfUploadResult uploadGltfModel(const GltfModel& model, VkCommandBuffer cmd);
    void destroyGltfResources(const GltfUploadResult& result);

    // Accessors
    MeshPool& getMeshPool() { return m_meshPool; }
    void waitForIdle() { m_device.device->waitIdle(); }

    // LightPass support
    PipelineHandle getLightPipelineHandle() const;
    uint64_t getLightPipelineLayout() const;
    uint64_t getGBufferColorDescriptorSet() const;

    // Extended pipeline variants
    enum class GraphicsPipelineVariant : uint32_t {
        nonTextured = 0,
        textured    = 1,
        light       = 2,
    };

private:
    // New members
    MeshPool m_meshPool;
    std::unique_ptr<GBufferPass> m_gbufferPass;
    std::unique_ptr<LightPass> m_lightPass;
    struct LightPipelineResources { ... } m_lightPipeline;
};
```

**GltfUploadResult Structure**:

```cpp
struct GltfUploadResult {
    std::vector<MeshHandle> meshes;
    std::vector<MaterialHandle> materials;
    std::vector<TextureHandle> textures;
};
```

---

### 3.6 Application UI

**File**: `app/MinimalLatestApp.h`, `app/MinimalLatestApp.cpp`

**New Members**:

```cpp
class MinimalLatestApp {
private:
    // glTF support
    std::unique_ptr<demo::GltfLoader> m_gltfLoader;
    std::optional<demo::Renderer::GltfUploadResult> m_currentModel;
    std::string m_modelPath;
    bool m_modelLoaded = false;

    // UI state
    char m_modelPathBuffer[256] = "";
    bool m_showModelInfo = true;
};
```

**New Methods**:

```cpp
void loadModel(const std::string& path);
void unloadModel();
void drawModelSelectorUI();
```

**UI Layout**:

```
┌─ Model Loader ─────────────────────────┐
│ Model Path: [________________]         │
│ [Load Model] [Unload]                  │
│                                        │
│ Presets:                               │
│ [Box.gltf] [Avocado.gltf] [Helmet.gltf]│
│                                        │
│ Current Model: resources/Box.gltf      │
│   Meshes: 1                            │
│   Materials: 1                         │
│   Textures: 1                          │
└────────────────────────────────────────┘
```

---

## 4. Shader Specifications

### 4.1 shader_io.h Extensions

```cpp
// GBuffer vertex format
struct VertexGltf {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 texCoord : TEXCOORD0;
};

// Push constant for GBuffer pass
struct PushConstantGltf {
    float4x4 model;
    float4x4 viewProjection;
    float4 baseColorFactor;
    uint materialIndex;
    uint3 _padding;
};
```

### 4.2 shader.rast.slang Modifications

- Add VertexGltf input layout
- Support interleaved vertex buffer (Position + Normal + UV)
- Use material index from push constant for bindless texture lookup

### 4.3 shader.light.slang (New)

```slang
// Fullscreen triangle, sample GBuffer Color

[[vk::binding(0, 0)]] Texture2D gbufferColor;
[[vk::binding(1, 0)]] SamplerState sampler0;

struct VSOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput vertexMain(uint vertexIndex : SV_VertexID) {
    VSOutput output;

    float2 positions[3] = {
        float2(-1.0, -1.0),
        float2( 3.0, -1.0),
        float2(-1.0,  3.0)
    };

    output.position = float4(positions[vertexIndex], 0.0, 1.0);
    output.uv = positions[vertexIndex] * 0.5 + 0.5;

    return output;
}

float4 fragmentMain(VSOutput input) : SV_Target {
    return gbufferColor.Sample(sampler0, input.uv);
}
```

---

## 5. File Changes Summary

| Operation | File Path | Description |
|-----------|-----------|-------------|
| ADD | `loader/GltfLoader.h` | glTF loader interface |
| ADD | `loader/GltfLoader.cpp` | tinygltf wrapper implementation |
| ADD | `render/MeshPool.h` | Mesh resource pool interface |
| ADD | `render/MeshPool.cpp` | Mesh upload/management |
| ADD | `render/passes/GBufferPass.h` | GBuffer Pass interface |
| ADD | `render/passes/GBufferPass.cpp` | GBuffer Pass implementation |
| ADD | `render/passes/LightPass.h` | LightPass interface |
| ADD | `render/passes/LightPass.cpp` | LightPass implementation |
| ADD | `shaders/shader.light.slang` | LightPass shader |
| MODIFY | `shaders/shader_io.h` | Add VertexGltf, PushConstantGltf |
| MODIFY | `shaders/shader.rast.slang` | Support glTF vertex format |
| MODIFY | `render/Renderer.h` | Add glTF methods, MeshPool, new passes |
| MODIFY | `render/Renderer.cpp` | Implement glTF upload, new passes |
| MODIFY | `app/MinimalLatestApp.h` | Add model loading state |
| MODIFY | `app/MinimalLatestApp.cpp` | Implement model loading UI |
| MODIFY | `CMakeLists.txt` | Add tinygltf, new source files |

---

## 6. Dependencies

### 6.1 tinygltf

```cmake
# CMakeLists.txt
FetchContent_Declare(
  tinygltf
  GIT_REPOSITORY https://github.com/syoyo/tinygltf.git
  GIT_TAG release
)
FetchContent_MakeAvailable(tinygltf)

target_link_libraries(demo_core PUBLIC tinygltf)
```

**Why tinygltf**:
- Single-header library
- No external dependencies (except stb_image, already included)
- Supports both .gltf and .glb formats
- Widely used, well-tested

---

## 7. Testing Plan

### 7.1 Test Models

Use glTF 2.0 sample models from Khronos repository:

| Model | Features | Purpose |
|-------|----------|---------|
| Box.gltf | Simple mesh, no texture | Basic rendering |
| Avocado.gltf | Textured mesh | Texture loading |
| DamagedHelmet.gltf | Multiple materials | Material switching |
| CesiumMan.gltf | Skinned mesh | Future: skeletal animation |

### 7.2 Test Cases

1. **Load/Unload Cycle**: Verify no resource leaks
2. **Multiple Model Loading**: Switch between models
3. **Missing Resources**: Handle missing textures gracefully
4. **Invalid Files**: Error handling for corrupt glTF files

---

## 8. Future Extensions

These are explicitly **out of scope** for this implementation but documented for future reference:

1. **PBR Materials**: Add metallic, roughness, normal maps
2. **Multiple Render Targets**: Split GBuffer into BaseColor, Normal, MaterialParams
3. **Lighting System**: Point lights, directional lights, shadows
4. **Skeletal Animation**: GPU skinning in compute pass
5. **Instancing**: Draw multiple instances of same mesh

---

## 9. Risks and Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| tinygltf API changes | Medium | Pin to specific git tag |
| Large model memory | High | Implement model size limit |
| Missing textures | Low | Use fallback texture (checkerboard) |
| Complex meshes | Medium | Add mesh simplification warning |

---

## 10. Acceptance Criteria

- [ ] Can load .gltf and .glb files via UI
- [ ] Models render with correct textures
- [ ] Can switch between multiple models
- [ ] No resource leaks on load/unload
- [ ] FPS remains > 60 for models with < 100k triangles
- [ ] LightPass correctly samples GBuffer to screen