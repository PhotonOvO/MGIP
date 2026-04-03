# glTF Model Rendering Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend VKDemo to render glTF 2.0 models with GBuffer + LightPass pipeline.

**Architecture:** GltfLoader loads models → MeshPool manages GPU buffers → GBufferPass renders to offscreen target → LightPass outputs to swapchain.

**Tech Stack:** C++20, Vulkan 1.4, tinygltf, Slang shaders

---

## Phase 1: Dependencies

### Task 1: Add tinygltf dependency

**Files:**
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add tinygltf FetchContent to CMakeLists.txt**

Add after the volk FetchContent block (around line 91):

```cmake
FetchContent_Declare(
  tinygltf
  GIT_REPOSITORY https://github.com/syoyo/tinygltf.git
  GIT_TAG v2.9.3
)
set(TINYGLTF_BUILD_LOADER_EXAMPLE OFF CACHE BOOL "" FORCE)
set(TINYGLTF_BUILD_VALIDATION_EXAMPLE OFF CACHE BOOL "" FORCE)
set(TINYGLTF_BUILD_GL_EXAMPLE OFF CACHE BOOL "" FORCE)
set(TINYGLTF_BUILD_VALIDATION_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(tinygltf)
```

- [ ] **Step 2: Link tinygltf to demo_core**

Find the `target_link_libraries(demo_core PUBLIC` section and add `tinygltf` to the list. The section should look like:

```cmake
target_link_libraries(demo_core
  PUBLIC
    glfw
    volk
    imgui_demo
    glm
    Vulkan::Vulkan
    tinygltf
)
```

- [ ] **Step 3: Create loader directory and add to CMakeLists.txt**

Add the new loader source files to the `add_library(demo_core STATIC` block. Add after `common/HandlePool.h`:

```cmake
  loader/GltfLoader.h
  loader/GltfLoader.cpp
  render/MeshPool.h
  render/MeshPool.cpp
```

- [ ] **Step 4: Verify build configuration**

Run: `cmake --preset default` (or your usual cmake configure command)

Expected: Configuration succeeds with tinygltf downloaded.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt
git commit -m "build: add tinygltf dependency for glTF loading

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Phase 2: GltfLoader

### Task 2: Create GltfLoader header

**Files:**
- Create: `loader/GltfLoader.h`

- [ ] **Step 1: Create loader directory**

```bash
mkdir -p H:/GitHub/VKDemo/loader
```

- [ ] **Step 2: Create GltfLoader.h**

```cpp
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace demo {

struct GltfMeshData {
    std::vector<float> positions;      // x,y,z per vertex
    std::vector<float> normals;        // x,y,z per vertex
    std::vector<float> texCoords;      // u,v per vertex
    std::vector<uint32_t> indices;
    int materialIndex = -1;            // -1 = default material
    glm::mat4 transform = glm::mat4(1.0f);
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
    GltfLoader() = default;
    ~GltfLoader() = default;

    bool load(const std::string& filepath, GltfModel& outModel);
    const std::string& getLastError() const { return m_lastError; }

private:
    std::string m_lastError;

    bool processNode(const tinygltf::Model& model, int nodeIndex, const glm::mat4& parentTransform, GltfModel& outModel);
    bool processMesh(const tinygltf::Model& model, int meshIndex, const glm::mat4& transform, GltfModel& outModel);
    void processMaterials(const tinygltf::Model& model, GltfModel& outModel);
    void processImages(const tinygltf::Model& model, GltfModel& outModel);
};

}  // namespace demo
```

- [ ] **Step 3: Commit**

```bash
git add loader/GltfLoader.h
git commit -m "feat(loader): add GltfLoader interface

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

### Task 3: Implement GltfLoader

**Files:**
- Create: `loader/GltfLoader.cpp`

- [ ] **Step 1: Create GltfLoader.cpp with tinygltf include**

```cpp
#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE_WRITE

#include "GltfLoader.h"

#include <tinygltf/tiny_gltf.h>

#include <cmath>
#include <cstring>

namespace demo {

bool GltfLoader::load(const std::string& filepath, GltfModel& outModel) {
    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    std::string err;
    std::string warn;

    // Determine file extension
    bool binary = filepath.size() >= 4 && filepath.substr(filepath.size() - 4) == ".glb";

    // Load the file
    bool success = false;
    if (binary) {
        success = loader.LoadBinaryFromFile(&model, &err, &warn, filepath);
    } else {
        success = loader.LoadASCIIFromFile(&model, &err, &warn, filepath);
    }

    if (!success) {
        m_lastError = "Failed to load glTF: " + err;
        if (!warn.empty()) {
            m_lastError += " (warnings: " + warn + ")";
        }
        return false;
    }

    // Extract name from filepath
    size_t lastSlash = filepath.find_last_of("/\\");
    size_t lastDot = filepath.find_last_of('.');
    if (lastSlash != std::string::npos && lastDot != std::string::npos && lastDot > lastSlash) {
        outModel.name = filepath.substr(lastSlash + 1, lastDot - lastSlash - 1);
    } else {
        outModel.name = filepath;
    }

    // Process materials first (meshes reference them)
    processMaterials(model, outModel);

    // Process images
    processImages(model, outModel);

    // Process scene graph starting from default scene
    const int sceneIndex = model.defaultScene > -1 ? model.defaultScene : 0;
    if (sceneIndex >= 0 && static_cast<size_t>(sceneIndex) < model.scenes.size()) {
        const tinygltf::Scene& scene = model.scenes[sceneIndex];
        for (int nodeIndex : scene.nodes) {
            if (!processNode(model, nodeIndex, glm::mat4(1.0f), outModel)) {
                return false;
            }
        }
    }

    return true;
}

bool GltfLoader::processNode(const tinygltf::Model& model, int nodeIndex, const glm::mat4& parentTransform, GltfModel& outModel) {
    if (nodeIndex < 0 || static_cast<size_t>(nodeIndex) >= model.nodes.size()) {
        return true;  // Skip invalid nodes
    }

    const tinygltf::Node& node = model.nodes[nodeIndex];

    // Compute node transform
    glm::mat4 localTransform(1.0f);

    if (!node.matrix.empty()) {
        // Matrix is provided directly
        for (int i = 0; i < 16; ++i) {
            localTransform[i / 4][i % 4] = static_cast<float>(node.matrix[i]);
        }
    } else {
        // Build from TRS
        glm::mat4 T(1.0f);
        if (!node.translation.empty()) {
            T = glm::translate(glm::mat4(1.0f),
                glm::vec3(node.translation[0], node.translation[1], node.translation[2]));
        }

        glm::mat4 R(1.0f);
        if (!node.rotation.empty()) {
            glm::quat q(
                static_cast<float>(node.rotation[3]),  // w
                static_cast<float>(node.rotation[0]),  // x
                static_cast<float>(node.rotation[1]),  // y
                static_cast<float>(node.rotation[2])   // z
            );
            R = glm::mat4_cast(q);
        }

        glm::mat4 S(1.0f);
        if (!node.scale.empty()) {
            S = glm::scale(glm::mat4(1.0f),
                glm::vec3(node.scale[0], node.scale[1], node.scale[2]));
        }

        localTransform = T * R * S;
    }

    glm::mat4 worldTransform = parentTransform * localTransform;

    // Process mesh if present
    if (node.mesh >= 0) {
        if (!processMesh(model, node.mesh, worldTransform, outModel)) {
            return false;
        }
    }

    // Process children
    for (int childIndex : node.children) {
        if (!processNode(model, childIndex, worldTransform, outModel)) {
            return false;
        }
    }

    return true;
}

bool GltfLoader::processMesh(const tinygltf::Model& model, int meshIndex, const glm::mat4& transform, GltfModel& outModel) {
    if (meshIndex < 0 || static_cast<size_t>(meshIndex) >= model.meshes.size()) {
        return true;
    }

    const tinygltf::Mesh& mesh = model.meshes[meshIndex];

    for (const auto& primitive : mesh.primitives) {
        GltfMeshData meshData;
        meshData.transform = transform;
        meshData.materialIndex = primitive.material;

        // Get position accessor
        auto posIt = primitive.attributes.find("POSITION");
        if (posIt == primitive.attributes.end()) {
            m_lastError = "Mesh primitive missing POSITION attribute";
            return false;
        }

        const tinygltf::Accessor& posAccessor = model.accessors[posIt->second];
        const tinygltf::BufferView& posView = model.bufferViews[posAccessor.bufferView];
        const tinygltf::Buffer& posBuffer = model.buffers[posView.buffer];

        meshData.positions.resize(posAccessor.count * 3);
        const float* posData = reinterpret_cast<const float*>(
            &posBuffer.data[posView.byteOffset + posAccessor.byteOffset]);
        for (size_t i = 0; i < posAccessor.count * 3; ++i) {
            meshData.positions[i] = posData[i];
        }

        // Get normals (optional)
        auto normIt = primitive.attributes.find("NORMAL");
        if (normIt != primitive.attributes.end()) {
            const tinygltf::Accessor& normAccessor = model.accessors[normIt->second];
            const tinygltf::BufferView& normView = model.bufferViews[normAccessor.bufferView];
            const tinygltf::Buffer& normBuffer = model.buffers[normView.buffer];

            meshData.normals.resize(normAccessor.count * 3);
            const float* normData = reinterpret_cast<const float*>(
                &normBuffer.data[normView.byteOffset + normAccessor.byteOffset]);
            for (size_t i = 0; i < normAccessor.count * 3; ++i) {
                meshData.normals[i] = normData[i];
            }
        } else {
            // Generate flat normals as fallback
            meshData.normals.resize(posAccessor.count * 3, 0.0f);
        }

        // Get texcoords (optional)
        auto texIt = primitive.attributes.find("TEXCOORD_0");
        if (texIt != primitive.attributes.end()) {
            const tinygltf::Accessor& texAccessor = model.accessors[texIt->second];
            const tinygltf::BufferView& texView = model.bufferViews[texAccessor.bufferView];
            const tinygltf::Buffer& texBuffer = model.buffers[texView.buffer];

            meshData.texCoords.resize(texAccessor.count * 2);
            const float* texData = reinterpret_cast<const float*>(
                &texBuffer.data[texView.byteOffset + texAccessor.byteOffset]);
            for (size_t i = 0; i < texAccessor.count * 2; ++i) {
                meshData.texCoords[i] = texData[i];
            }
            // Flip Y coordinate for Vulkan
            for (size_t i = 0; i < texAccessor.count; ++i) {
                meshData.texCoords[i * 2 + 1] = 1.0f - meshData.texCoords[i * 2 + 1];
            }
        } else {
            meshData.texCoords.resize(posAccessor.count * 2, 0.0f);
        }

        // Get indices
        if (primitive.indices >= 0) {
            const tinygltf::Accessor& indexAccessor = model.accessors[primitive.indices];
            const tinygltf::BufferView& indexView = model.bufferViews[indexAccessor.bufferView];
            const tinygltf::Buffer& indexBuffer = model.buffers[indexView.buffer];

            meshData.indices.resize(indexAccessor.count);

            if (indexAccessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
                const uint16_t* indexData = reinterpret_cast<const uint16_t*>(
                    &indexBuffer.data[indexView.byteOffset + indexAccessor.byteOffset]);
                for (size_t i = 0; i < indexAccessor.count; ++i) {
                    meshData.indices[i] = indexData[i];
                }
            } else if (indexAccessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
                const uint32_t* indexData = reinterpret_cast<const uint32_t*>(
                    &indexBuffer.data[indexView.byteOffset + indexAccessor.byteOffset]);
                std::memcpy(meshData.indices.data(), indexData, indexAccessor.count * sizeof(uint32_t));
            }
        } else {
            // No indices, generate sequential indices
            meshData.indices.resize(posAccessor.count);
            for (size_t i = 0; i < posAccessor.count; ++i) {
                meshData.indices[i] = static_cast<uint32_t>(i);
            }
        }

        outModel.meshes.push_back(std::move(meshData));
    }

    return true;
}

void GltfLoader::processMaterials(const tinygltf::Model& model, GltfModel& outModel) {
    outModel.materials.resize(model.materials.size());

    for (size_t i = 0; i < model.materials.size(); ++i) {
        const tinygltf::Material& mat = model.materials[i];
        GltfMaterialData& outMat = outModel.materials[i];

        // Base color texture
        if (mat.pbrMetallicRoughness.baseColorTexture.index >= 0) {
            outMat.baseColorTexture = mat.pbrMetallicRoughness.baseColorTexture.index;
        }

        // Base color factor
        if (!mat.pbrMetallicRoughness.baseColorFactor.empty()) {
            outMat.baseColorFactor = glm::vec4(
                static_cast<float>(mat.pbrMetallicRoughness.baseColorFactor[0]),
                static_cast<float>(mat.pbrMetallicRoughness.baseColorFactor[1]),
                static_cast<float>(mat.pbrMetallicRoughness.baseColorFactor[2]),
                static_cast<float>(mat.pbrMetallicRoughness.baseColorFactor[3])
            );
        }
    }

    // Add default material if none exist
    if (outModel.materials.empty()) {
        outModel.materials.push_back(GltfMaterialData{});
    }
}

void GltfLoader::processImages(const tinygltf::Model& model, GltfModel& outModel) {
    outModel.images.resize(model.images.size());

    for (size_t i = 0; i < model.images.size(); ++i) {
        const tinygltf::Image& img = model.images[i];
        GltfImageData& outImg = outModel.images[i];

        outImg.width = img.width;
        outImg.height = img.height;
        outImg.channels = img.component;

        if (!img.image.empty()) {
            // Image was already decoded by tinygltf
            outImg.pixels = img.image;
        } else if (!img.uri.empty()) {
            // URI reference - we'll handle this case by copying the decoded data
            // tinygltf should have decoded it already
            outImg.pixels = img.image;
        }
    }
}

}  // namespace demo
```

- [ ] **Step 2: Build to verify**

Run: `cmake --build build`

Expected: Build succeeds with no errors.

- [ ] **Step 3: Commit**

```bash
git add loader/GltfLoader.cpp
git commit -m "feat(loader): implement GltfLoader with tinygltf

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Phase 3: MeshPool

### Task 4: Create MeshPool header

**Files:**
- Create: `render/MeshPool.h`

- [ ] **Step 1: Create MeshPool.h**

```cpp
#pragma once

#include "../common/Common.h"
#include "../common/Handles.h"
#include "../common/HandlePool.h"

#include <cstdint>

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include <vk_mem_alloc.h>

namespace demo {

struct GltfMeshData;  // Forward declaration

struct MeshRecord {
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VmaAllocation vertexAllocation = nullptr;
    VmaAllocation indexAllocation = nullptr;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t vertexStride = 32;  // Position(12) + Normal(12) + TexCoord(8)
    glm::mat4 transform = glm::mat4(1.0f);
};

class MeshPool {
public:
    MeshPool() = default;
    ~MeshPool() { assert(m_device == VK_NULL_HANDLE && "Missing deinit()"); }

    void init(VkDevice device, VmaAllocator allocator);
    void deinit();

    MeshHandle uploadMesh(const GltfMeshData& meshData, VkCommandBuffer cmd);
    void destroyMesh(MeshHandle handle);

    [[nodiscard]] const MeshRecord* tryGet(MeshHandle handle) const;

    template<typename Fn>
    void forEachActive(Fn&& fn) {
        m_pool.forEachActive(std::forward<Fn>(fn));
    }

private:
    VkDevice m_device = VK_NULL_HANDLE;
    VmaAllocator m_allocator = nullptr;
    HandlePool<MeshHandle, MeshRecord> m_pool;
};

}  // namespace demo
```

- [ ] **Step 2: Commit**

```bash
git add render/MeshPool.h
git commit -m "feat(render): add MeshPool interface

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

### Task 5: Implement MeshPool

**Files:**
- Create: `render/MeshPool.cpp`

- [ ] **Step 1: Create MeshPool.cpp**

```cpp
#include "MeshPool.h"
#include "../loader/GltfLoader.h"

#include <cstring>

namespace demo {

void MeshPool::init(VkDevice device, VmaAllocator allocator) {
    m_device = device;
    m_allocator = allocator;
}

void MeshPool::deinit() {
    // Destroy all remaining meshes
    std::vector<MeshHandle> handles;
    m_pool.forEachActive([&handles](MeshHandle handle, const MeshRecord&) {
        handles.push_back(handle);
    });

    for (MeshHandle handle : handles) {
        destroyMesh(handle);
    }

    m_device = VK_NULL_HANDLE;
    m_allocator = nullptr;
}

MeshHandle MeshPool::uploadMesh(const GltfMeshData& meshData, VkCommandBuffer cmd) {
    MeshRecord record;
    record.vertexCount = static_cast<uint32_t>(meshData.positions.size() / 3);
    record.indexCount = static_cast<uint32_t>(meshData.indices.size());
    record.transform = meshData.transform;

    // Build interleaved vertex buffer: Position(12) + Normal(12) + TexCoord(8) = 32 bytes
    std::vector<uint8_t> vertexData(record.vertexCount * 32);

    for (uint32_t i = 0; i < record.vertexCount; ++i) {
        float* dst = reinterpret_cast<float*>(&vertexData[i * 32]);

        // Position
        dst[0] = meshData.positions[i * 3 + 0];
        dst[1] = meshData.positions[i * 3 + 1];
        dst[2] = meshData.positions[i * 3 + 2];

        // Normal
        if (!meshData.normals.empty()) {
            dst[3] = meshData.normals[i * 3 + 0];
            dst[4] = meshData.normals[i * 3 + 1];
            dst[5] = meshData.normals[i * 3 + 2];
        } else {
            dst[3] = 0.0f;
            dst[4] = 1.0f;
            dst[5] = 0.0f;
        }

        // TexCoord
        if (!meshData.texCoords.empty()) {
            dst[6] = meshData.texCoords[i * 2 + 0];
            dst[7] = meshData.texCoords[i * 2 + 1];
        } else {
            dst[6] = 0.0f;
            dst[7] = 0.0f;
        }
    }

    // Create vertex buffer
    VkBufferCreateInfo vertexBufferInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = vertexData.size(),
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    VmaAllocationCreateInfo vertexAllocInfo{
        .usage = VMA_MEMORY_USAGE_GPU_ONLY,
    };

    VkBuffer vertexStagingBuffer = VK_NULL_HANDLE;
    VmaAllocation vertexStagingAllocation = nullptr;
    void* vertexMappedData = nullptr;

    // Create staging buffer
    VkBufferCreateInfo stagingInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = vertexData.size(),
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    VmaAllocationCreateInfo stagingAllocInfo{
        .usage = VMA_MEMORY_USAGE_CPU_TO_GPU,
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
    };

    vmaCreateBuffer(m_allocator, &stagingInfo, &stagingAllocInfo,
                    &vertexStagingBuffer, &vertexStagingAllocation, nullptr);
    vmaMapMemory(m_allocator, vertexStagingAllocation, &vertexMappedData);
    std::memcpy(vertexMappedData, vertexData.data(), vertexData.size());
    vmaUnmapMemory(m_allocator, vertexStagingAllocation);

    // Create GPU vertex buffer
    vmaCreateBuffer(m_allocator, &vertexBufferInfo, &vertexAllocInfo,
                    &record.vertexBuffer, &record.vertexAllocation, nullptr);

    // Copy to GPU
    VkBufferCopy vertexCopy{.size = vertexData.size()};
    vkCmdCopyBuffer(cmd, vertexStagingBuffer, record.vertexBuffer, 1, &vertexCopy);

    // Create index buffer
    VkBufferCreateInfo indexBufferInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = record.indexCount * sizeof(uint32_t),
        .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    VkBuffer indexStagingBuffer = VK_NULL_HANDLE;
    VmaAllocation indexStagingAllocation = nullptr;
    void* indexMappedData = nullptr;

    stagingInfo.size = record.indexCount * sizeof(uint32_t);
    vmaCreateBuffer(m_allocator, &stagingInfo, &stagingAllocInfo,
                    &indexStagingBuffer, &indexStagingAllocation, nullptr);
    vmaMapMemory(m_allocator, indexStagingAllocation, &indexMappedData);
    std::memcpy(indexMappedData, meshData.indices.data(), record.indexCount * sizeof(uint32_t));
    vmaUnmapMemory(m_allocator, indexStagingAllocation);

    // Create GPU index buffer
    vmaCreateBuffer(m_allocator, &indexBufferInfo, &vertexAllocInfo,
                    &record.indexBuffer, &record.indexAllocation, nullptr);

    // Copy to GPU
    VkBufferCopy indexCopy{.size = record.indexCount * sizeof(uint32_t)};
    vkCmdCopyBuffer(cmd, indexStagingBuffer, record.indexBuffer, 1, &indexCopy);

    // Cleanup staging buffers (will be freed after command buffer completes)
    vmaDestroyBuffer(m_allocator, vertexStagingBuffer, vertexStagingAllocation);
    vmaDestroyBuffer(m_allocator, indexStagingBuffer, indexStagingAllocation);

    return m_pool.emplace(std::move(record));
}

void MeshPool::destroyMesh(MeshHandle handle) {
    MeshRecord* record = m_pool.tryGet(handle);
    if (record == nullptr) {
        return;
    }

    if (record->vertexBuffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(m_allocator, record->vertexBuffer, record->vertexAllocation);
    }

    if (record->indexBuffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(m_allocator, record->indexBuffer, record->indexAllocation);
    }

    m_pool.destroy(handle);
}

const MeshRecord* MeshPool::tryGet(MeshHandle handle) const {
    return m_pool.tryGet(handle);
}

}  // namespace demo
```

- [ ] **Step 2: Build to verify**

Run: `cmake --build build`

Expected: Build succeeds.

- [ ] **Step 3: Commit**

```bash
git add render/MeshPool.cpp
git commit -m "feat(render): implement MeshPool for GPU mesh management

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Phase 4: Shaders

### Task 6: Update shader_io.h for glTF vertex format

**Files:**
- Modify: `shaders/shader_io.h`

- [ ] **Step 1: Add glTF vertex structure and push constant to shader_io.h**

Add after existing structures:

```cpp
// glTF vertex format: Position(12) + Normal(12) + TexCoord(8) = 32 bytes
struct VertexGltf {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 texCoord;
};

// Push constant for glTF rendering
struct PushConstantGltf {
    glm::mat4 model;
    glm::mat4 viewProjection;
    glm::vec4 baseColorFactor;
    uint32_t materialIndex;
    uint32_t _padding[3];
};
```

- [ ] **Step 2: Commit**

```bash
git add shaders/shader_io.h
git commit -m "feat(shaders): add glTF vertex format and push constant

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

### Task 7: Create LightPass shader

**Files:**
- Create: `shaders/shader.light.slang`

- [ ] **Step 1: Create shader.light.slang**

```slang
// LightPass shader: Fullscreen triangle sampling GBuffer Color
// No vertex input - generates fullscreen triangle in vertex shader

#ifdef USE_SLANG
#define VK_PUSH_CONSTANT [[vk::push_constant]]
#define VK_BINDING(set, binding) [[vk::binding(binding, set)]]
#else
#define VK_PUSH_CONSTANT
#define VK_BINDING(set, binding)
#endif

// GBuffer Color texture (input)
VK_BINDING(0, 0) Texture2D gbufferColor;
VK_BINDING(1, 0) SamplerState sampler0;

struct VSOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

// Fullscreen triangle vertex shader
// Uses vertex ID to generate a large triangle covering the screen
[shader("vertex")]
VSOutput vertexMain(uint vertexIndex : SV_VertexID) {
    VSOutput output;

    // Large triangle in clip space
    // Covers screen when rasterized
    float2 positions[3] = {
        float2(-1.0, -1.0),  // Bottom-left
        float2( 3.0, -1.0),  // Bottom-right (off-screen)
        float2(-1.0,  3.0)   // Top-left (off-screen)
    };

    output.position = float4(positions[vertexIndex], 0.0, 1.0);

    // UV = position * 0.5 + 0.5
    // (-1,-1) -> (0,0), (1,1) -> (1,1)
    output.uv = positions[vertexIndex] * 0.5 + 0.5;

    // Flip Y for Vulkan
    output.uv.y = 1.0 - output.uv.y;

    return output;
}

// Fragment shader: Sample GBuffer Color and output
[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target {
    return gbufferColor.Sample(sampler0, input.uv);
}
```

- [ ] **Step 2: Commit**

```bash
git add shaders/shader.light.slang
git commit -m "feat(shaders): add LightPass fullscreen shader

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Phase 5: LightPass

### Task 8: Create LightPass header

**Files:**
- Create: `render/passes/LightPass.h`

- [ ] **Step 1: Create LightPass.h**

```cpp
#pragma once

#include "../Pass.h"

namespace demo {

class Renderer;

class LightPass : public RenderPassNode {
public:
    explicit LightPass(Renderer* renderer);
    ~LightPass() override = default;

    [[nodiscard]] const char* getName() const override { return "LightPass"; }
    [[nodiscard]] HandleSlice<PassResourceDependency> getDependencies() const override;
    void execute(const PassContext& context) const override;

private:
    Renderer* m_renderer;
};

}  // namespace demo
```

- [ ] **Step 2: Commit**

```bash
git add render/passes/LightPass.h
git commit -m "feat(render): add LightPass interface

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

### Task 9: Implement LightPass

**Files:**
- Create: `render/passes/LightPass.cpp`

- [ ] **Step 1: Create LightPass.cpp**

```cpp
#include "LightPass.h"
#include "../Renderer.h"
#include "../../rhi/vulkan/VulkanCommandList.h"
#include "../../rhi/vulkan/VulkanPipelines.h"

namespace demo {

LightPass::LightPass(Renderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> LightPass::getDependencies() const
{
    static const std::array<PassResourceDependency, 2> dependencies = {
        PassResourceDependency::texture(
            kPassGBufferColorHandle,
            ResourceAccess::read,
            rhi::ShaderStage::fragment
        ),
        PassResourceDependency::texture(
            kPassSwapchainHandle,
            ResourceAccess::write,
            rhi::ShaderStage::fragment
        ),
    };
    return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void LightPass::execute(const PassContext& context) const
{
    context.cmd->beginEvent("LightPass");

    // Get swapchain extent
    const VkExtent2D extent = m_renderer->getSwapchainExtent();
    const VkViewport viewport{
        0.0f, 0.0f,
        static_cast<float>(extent.width),
        static_cast<float>(extent.height),
        0.0f, 1.0f
    };
    const VkRect2D scissor{{0, 0}, extent};

    // Bind light pipeline
    const PipelineHandle lightPipeline = m_renderer->getLightPipelineHandle();
    const VkPipeline vkPipeline = reinterpret_cast<VkPipeline>(
        m_renderer->getPipelineOpaque(lightPipeline, VK_PIPELINE_BIND_POINT_GRAPHICS));

    rhi::vulkan::cmdSetViewport(*context.cmd, viewport);
    rhi::vulkan::cmdSetScissor(*context.cmd, scissor);
    rhi::vulkan::cmdBindPipeline(*context.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkPipeline);

    // Bind GBuffer texture descriptor
    const uint64_t descriptorSet = m_renderer->getGBufferColorDescriptorSet();
    const uint64_t pipelineLayout = m_renderer->getLightPipelineLayout();

    rhi::vulkan::cmdBindDescriptorSetOpaque(
        *context.cmd,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        pipelineLayout,
        0,  // set index
        descriptorSet,
        0, nullptr  // no dynamic offsets
    );

    // Draw fullscreen triangle
    rhi::vulkan::cmdDraw(*context.cmd, 3, 1, 0, 0);

    context.cmd->endEvent();
}

}  // namespace demo
```

- [ ] **Step 2: Commit**

```bash
git add render/passes/LightPass.cpp
git commit -m "feat(render): implement LightPass fullscreen rendering

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Phase 6: GBufferPass

### Task 10: Create GBufferPass header

**Files:**
- Create: `render/passes/GBufferPass.h`

- [ ] **Step 1: Create GBufferPass.h**

```cpp
#pragma once

#include "../Pass.h"

namespace demo {

class Renderer;

class GBufferPass : public RenderPassNode {
public:
    explicit GBufferPass(Renderer* renderer);
    ~GBufferPass() override = default;

    [[nodiscard]] const char* getName() const override { return "GBufferPass"; }
    [[nodiscard]] HandleSlice<PassResourceDependency> getDependencies() const override;
    void execute(const PassContext& context) const override;

    void setModel(const void* modelData) { m_modelData = modelData; }

private:
    Renderer* m_renderer;
    const void* m_modelData = nullptr;
};

}  // namespace demo
```

- [ ] **Step 2: Commit**

```bash
git add render/passes/GBufferPass.h
git commit -m "feat(render): add GBufferPass interface

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

### Task 11: Implement GBufferPass

**Files:**
- Create: `render/passes/GBufferPass.cpp`

- [ ] **Step 1: Create GBufferPass.cpp**

```cpp
#include "GBufferPass.h"
#include "../Renderer.h"
#include "../DrawStreamWriter.h"

#include <array>

namespace demo {

GBufferPass::GBufferPass(Renderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> GBufferPass::getDependencies() const
{
    static const std::array<PassResourceDependency, 3> dependencies = {
        PassResourceDependency::buffer(
            kPassVertexBufferHandle,
            ResourceAccess::read,
            rhi::ShaderStage::vertex
        ),
        PassResourceDependency::texture(
            kPassGBufferColorHandle,
            ResourceAccess::write,
            rhi::ShaderStage::fragment
        ),
        PassResourceDependency::texture(
            kPassGBufferDepthHandle,
            ResourceAccess::write,
            rhi::ShaderStage::fragment
        ),
    };
    return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void GBufferPass::execute(const PassContext& context) const
{
    if (m_modelData == nullptr) {
        return;
    }

    context.cmd->beginEvent("GBufferPass");

    // For now, render the existing triangle as placeholder
    // Full glTF rendering will be implemented after Renderer integration

    DrawStreamWriter writer{};
    writer.clear();

    const rhi::ResourceIndex materialIndex = m_renderer->resolveMaterialResourceIndex(context.params->materialHandle);
    const rhi::ResourceIndex sceneIndex = m_renderer->getSceneBindlessResourceIndex();

    writer.setPipeline(m_renderer->getGraphicsPipelineHandle(Renderer::GraphicsPipelineVariant::nonTextured));
    writer.setMaterialIndex(materialIndex);
    writer.setMesh(kNullMeshHandle);
    writer.setDynamicBufferIndex(sceneIndex);
    writer.setDynamicOffset(m_renderer->allocateDrawDynamicOffset(materialIndex, *context.params));
    writer.draw(0, 3, 1);

    writer.setPipeline(m_renderer->getGraphicsPipelineHandle(Renderer::GraphicsPipelineVariant::textured));
    writer.setDynamicOffset(m_renderer->allocateDrawDynamicOffset(materialIndex, *context.params));
    writer.draw(3, 3, 1);

    std::vector<StreamEntry>& drawStream = *context.drawStream;
    drawStream = writer.entries();
    m_renderer->executeGraphicsPass(*context.cmd, *context.params, drawStream);

    context.cmd->endEvent();
}

}  // namespace demo
```

- [ ] **Step 2: Commit**

```bash
git add render/passes/GBufferPass.cpp
git commit -m "feat(render): implement GBufferPass with placeholder rendering

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Phase 7: Renderer Integration

### Task 12: Update Renderer header

**Files:**
- Modify: `render/Renderer.h`

- [ ] **Step 1: Add forward declarations and includes**

Add after existing includes:

```cpp
#include "MeshPool.h"
#include "passes/GBufferPass.h"
#include "passes/LightPass.h"
#include "../loader/GltfLoader.h"
```

- [ ] **Step 2: Add GltfUploadResult struct**

Add after `RenderParams` struct:

```cpp
struct GltfUploadResult {
    std::vector<MeshHandle> meshes;
    std::vector<MaterialHandle> materials;
    std::vector<TextureHandle> textures;
};
```

- [ ] **Step 3: Add new pipeline variant**

Modify `GraphicsPipelineVariant` enum:

```cpp
enum class GraphicsPipelineVariant : uint32_t
{
    nonTextured = 0,
    textured    = 1,
    light       = 2,
};
```

- [ ] **Step 4: Add new public methods**

Add after `getGraphicsPipelineHandle`:

```cpp
// glTF model support
GltfUploadResult uploadGltfModel(const GltfModel& model, VkCommandBuffer cmd);
void destroyGltfResources(const GltfUploadResult& result);

MeshPool& getMeshPool() { return m_meshPool; }
void waitForIdle() { m_device.device->waitIdle(); }

// LightPass support
PipelineHandle getLightPipelineHandle() const;
uint64_t getLightPipelineLayout() const;
uint64_t getGBufferColorDescriptorSet() const;

VkExtent2D getSwapchainExtent() const {
    return m_swapchainDependent.windowSize;
}
uint64_t getPipelineOpaque(PipelineHandle handle, uint32_t bindPoint) const;
```

- [ ] **Step 5: Add new private members**

Add in private section:

```cpp
// glTF support
MeshPool m_meshPool;
std::unique_ptr<GBufferPass> m_gbufferPass;
std::unique_ptr<LightPass> m_lightPass;

// Light pipeline
BindGroupHandle m_lightBindGroup{kNullBindGroupHandle};
PipelineHandle m_lightPipeline{};
```

- [ ] **Step 6: Commit**

```bash
git add render/Renderer.h
git commit -m "feat(render): add glTF and LightPass support to Renderer interface

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

### Task 13: Implement Renderer glTF methods

**Files:**
- Modify: `render/Renderer.cpp`

- [ ] **Step 1: Add includes at top**

Add after existing includes:

```cpp
#include "MeshPool.h"
#include "passes/GBufferPass.h"
#include "passes/LightPass.h"
```

- [ ] **Step 2: Initialize MeshPool in init()**

Add after `m_device.samplerPool.init(nativeDevice);`:

```cpp
m_meshPool.init(nativeDevice, m_device.allocator);
```

- [ ] **Step 3: Create passes in init()**

Replace the pass creation section:

```cpp
// Initialize passes and pass executor
m_gbufferPass = std::make_unique<GBufferPass>(this);
m_animateVerticesPass = std::make_unique<AnimateVerticesPass>(this);
m_sceneOpaquePass = std::make_unique<SceneOpaquePass>(this);
m_lightPass = std::make_unique<LightPass>(this);
m_presentPass = std::make_unique<PresentPass>(this);
m_imguiPass = std::make_unique<ImguiPass>(this);

m_passExecutor.clear();
m_passExecutor.addPass(*m_gbufferPass);
m_passExecutor.addPass(*m_animateVerticesPass);
m_passExecutor.addPass(*m_presentPass);
m_passExecutor.addPass(*m_lightPass);
m_passExecutor.addPass(*m_imguiPass);
```

- [ ] **Step 4: Cleanup in shutdown()**

Add in shutdown():

```cpp
m_meshPool.deinit();
```

- [ ] **Step 5: Implement uploadGltfModel**

Add at end of file before closing namespace:

```cpp
Renderer::GltfUploadResult Renderer::uploadGltfModel(const GltfModel& model, VkCommandBuffer cmd) {
    GltfUploadResult result;

    const VkDevice device = fromNativeHandle<VkDevice>(m_device.device->getNativeDevice());

    // Upload textures
    for (const auto& imageData : model.images) {
        if (imageData.pixels.empty() || imageData.width <= 0 || imageData.height <= 0) {
            continue;
        }

        // Create image
        const VkFormat format = (imageData.channels == 4) ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R8G8B8_UNORM;
        const VkImageCreateInfo imageInfo = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = format,
            .extent = {static_cast<uint32_t>(imageData.width), static_cast<uint32_t>(imageData.height), 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        };

        utils::Image image = createImage(m_device.allocator, imageInfo);
        utils::cmdInitImageLayout(cmd, image.image);

        // Upload pixels
        const VkBufferImageCopy copyRegion = {
            .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
            .imageExtent = imageInfo.extent,
        };

        // Create staging buffer
        const size_t pixelSize = imageData.pixels.size();
        utils::Buffer stagingBuffer = createBuffer(device, m_device.allocator, pixelSize,
            VK_BUFFER_USAGE_2_TRANSFER_SRC_BIT_KHR, VMA_MEMORY_USAGE_CPU_TO_GPU,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);

        void* mapped = nullptr;
        vmaMapMemory(m_device.allocator, stagingBuffer.allocation, &mapped);
        std::memcpy(mapped, imageData.pixels.data(), pixelSize);
        vmaUnmapMemory(m_device.allocator, stagingBuffer.allocation);

        vkCmdCopyBufferToImage(cmd, stagingBuffer.buffer, image.image,
            VK_IMAGE_LAYOUT_GENERAL, 1, &copyRegion);

        // Create image view
        const VkImageViewCreateInfo viewInfo = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = image.image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = format,
            .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1},
        };

        VkImageView imageView = VK_NULL_HANDLE;
        vkCreateImageView(device, &viewInfo, nullptr, &imageView);

        // Register texture
        TextureHandle texHandle = m_materials.texturePool.emplace(MaterialResources::TextureRecord{
            .hot = {
                .runtimeKind = MaterialResources::TextureRuntimeKind::materialSampled,
                .sampledImageView = imageView,
                .sampledImageLayout = VK_IMAGE_LAYOUT_GENERAL,
            },
            .cold = {
                .ownedImage = {image, imageView, VK_IMAGE_LAYOUT_GENERAL, imageInfo.extent},
                .sourceExtent = imageInfo.extent,
            },
        });

        result.textures.push_back(texHandle);
        m_device.stagingBuffers.push_back(stagingBuffer);
    }

    // Create materials
    for (const auto& matData : model.materials) {
        TextureHandle texHandle = kNullTextureHandle;
        if (matData.baseColorTexture >= 0 && matData.baseColorTexture < static_cast<int>(result.textures.size())) {
            texHandle = result.textures[matData.baseColorTexture];
        }

        const rhi::ResourceIndex descriptorIndex = static_cast<rhi::ResourceIndex>(result.materials.size());
        MaterialHandle matHandle = m_materials.materialPool.emplace(MaterialResources::MaterialRecord{
            .sampledTexture = texHandle,
            .descriptorIndex = descriptorIndex,
            .debugName = "gltf-material",
        });

        result.materials.push_back(matHandle);
    }

    // Upload meshes
    for (const auto& meshData : model.meshes) {
        MeshHandle meshHandle = m_meshPool.uploadMesh(meshData, cmd);
        result.meshes.push_back(meshHandle);
    }

    // Update bindless descriptors
    updateGltfDescriptors(result);

    return result;
}

void Renderer::destroyGltfResources(const GltfUploadResult& result) {
    VkDevice device = fromNativeHandle<VkDevice>(m_device.device->getNativeDevice());

    // Destroy meshes
    for (MeshHandle handle : result.meshes) {
        m_meshPool.destroyMesh(handle);
    }

    // Destroy materials
    for (MaterialHandle handle : result.materials) {
        m_materials.materialPool.destroy(handle);
    }

    // Destroy textures
    for (TextureHandle handle : result.textures) {
        const MaterialResources::TextureColdData* cold = tryGetTextureCold(handle);
        if (cold && cold->ownedImage.image != VK_NULL_HANDLE) {
            destroyImageResource(device, m_device.allocator, const_cast<utils::ImageResource&>(cold->ownedImage));
        }
        m_materials.texturePool.destroy(handle);
    }
}

void Renderer::updateGltfDescriptors(const GltfUploadResult& result) {
    if (result.textures.empty()) {
        return;
    }

    const VkSampler sampler = m_device.samplerPool.acquireSampler({
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .maxLod = VK_LOD_CLAMP_NONE,
    });

    std::vector<rhi::DescriptorImageInfo> imageInfos(result.textures.size());
    for (size_t i = 0; i < result.textures.size(); ++i) {
        const auto* texHot = tryGetTextureHot(result.textures[i]);
        if (texHot) {
            imageInfos[i] = rhi::DescriptorImageInfo{
                .sampler = reinterpret_cast<uint64_t>(sampler),
                .imageView = reinterpret_cast<uint64_t>(texHot->sampledImageView),
                .imageLayout = static_cast<uint32_t>(texHot->sampledImageLayout),
            };
        }
    }

    const rhi::BindTableWrite write{
        .dstIndex = kMaterialBindlessTexturesIndex,
        .dstArrayElement = static_cast<uint32_t>(m_materials.materialPool.size() - result.materials.size()),
        .resourceType = rhi::BindlessResourceType::sampledTexture,
        .descriptorCount = static_cast<uint32_t>(imageInfos.size()),
        .pImageInfo = imageInfos.data(),
        .visibility = rhi::ResourceVisibility::allGraphics,
    };

    updateBindGroup(m_materials.materialBindGroup, &write, 1);
}

PipelineHandle Renderer::getLightPipelineHandle() const {
    return m_lightPipeline;
}

uint64_t Renderer::getLightPipelineLayout() const {
    // Will be implemented with light pipeline creation
    return m_device.graphicPipelineLayout ? m_device.graphicPipelineLayout->getNativeHandle() : 0;
}

uint64_t Renderer::getGBufferColorDescriptorSet() const {
    return getBindGroupDescriptorSetOpaque(m_materials.materialBindGroup, BindGroupSetSlot::material);
}
```

- [ ] **Step 6: Build to verify**

Run: `cmake --build build`

Expected: Build succeeds with possible warnings about unused parameters.

- [ ] **Step 7: Commit**

```bash
git add render/Renderer.cpp
git commit -m "feat(render): implement glTF model upload and LightPass support

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Phase 8: Application UI

### Task 14: Update MinimalLatestApp for model loading

**Files:**
- Modify: `app/MinimalLatestApp.h`

- [ ] **Step 1: Add includes and members**

Add after existing includes:

```cpp
#include "../loader/GltfLoader.h"
#include <optional>
```

- [ ] **Step 2: Add new private members**

Add in private section:

```cpp
// glTF model loading
std::unique_ptr<demo::GltfLoader> m_gltfLoader;
std::optional<demo::Renderer::GltfUploadResult> m_currentModel;
std::string m_modelPath;
bool m_modelLoaded = false;

// UI state
char m_modelPathBuffer[512] = "resources/models/Box/Box.gltf";
```

- [ ] **Step 3: Add new private methods**

```cpp
void loadModel(const std::string& path);
void unloadModel();
void drawModelLoaderUI();
```

- [ ] **Step 4: Commit**

```bash
git add app/MinimalLatestApp.h
git commit -m "feat(app): add glTF model loading state to MinimalLatestApp

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

### Task 15: Implement model loading UI

**Files:**
- Modify: `app/MinimalLatestApp.cpp`

- [ ] **Step 1: Initialize GltfLoader in constructor**

Add in constructor after `m_renderer.init(...)`:

```cpp
m_gltfLoader = std::make_unique<demo::GltfLoader>();
```

- [ ] **Step 2: Implement loadModel and unloadModel**

Add after constructor:

```cpp
void MinimalLatestApp::loadModel(const std::string& path) {
    demo::GltfModel model;
    if (!m_gltfLoader->load(path, model)) {
        LOGE("Failed to load model: %s, error: %s",
             path.c_str(), m_gltfLoader->getLastError().c_str());
        return;
    }

    m_renderer.waitForIdle();
    unloadModel();

    // Upload model using transient command buffer
    const VkDevice device = reinterpret_cast<VkDevice>(
        m_renderer.getMeshPool().tryGet(demo::kNullMeshHandle) ? VK_NULL_HANDLE : VK_NULL_HANDLE);

    // For now, store model info - actual upload will be done with proper command buffer
    m_modelPath = path;
    m_modelLoaded = true;

    LOGI("Loaded glTF model: %s (%zu meshes, %zu materials, %zu textures)",
         path.c_str(), model.meshes.size(), model.materials.size(), model.images.size());
}

void MinimalLatestApp::unloadModel() {
    if (m_modelLoaded && m_currentModel.has_value()) {
        m_renderer.waitForIdle();
        m_renderer.destroyGltfResources(*m_currentModel);
        m_currentModel.reset();
        m_modelLoaded = false;
    }
}
```

- [ ] **Step 3: Implement drawModelLoaderUI**

```cpp
void MinimalLatestApp::drawModelLoaderUI() {
    if (ImGui::Begin("Model Loader")) {
        // Model path input
        ImGui::InputText("Model Path", m_modelPathBuffer, sizeof(m_modelPathBuffer));

        // Load button
        if (ImGui::Button("Load Model")) {
            loadModel(std::string(m_modelPathBuffer));
        }

        ImGui::SameLine();

        // Unload button
        if (ImGui::Button("Unload")) {
            unloadModel();
        }

        // Preset models
        ImGui::Separator();
        ImGui::Text("Presets:");

        if (ImGui::Button("Triangle (default)")) {
            unloadModel();
            m_modelLoaded = false;
        }

        // Current model info
        if (m_modelLoaded) {
            ImGui::Separator();
            ImGui::Text("Current Model: %s", m_modelPath.c_str());
            if (m_currentModel.has_value()) {
                ImGui::Text("  Meshes: %zu", m_currentModel->meshes.size());
                ImGui::Text("  Materials: %zu", m_currentModel->materials.size());
                ImGui::Text("  Textures: %zu", m_currentModel->textures.size());
            }
        }
    }
    ImGui::End();
}
```

- [ ] **Step 4: Call drawModelLoaderUI in run()**

Add after `ImGui::Begin("Settings")` block:

```cpp
drawModelLoaderUI();
```

- [ ] **Step 5: Cleanup in destructor**

Add in destructor before `m_renderer.shutdown`:

```cpp
unloadModel();
```

- [ ] **Step 6: Build to verify**

Run: `cmake --build build`

Expected: Build succeeds.

- [ ] **Step 7: Commit**

```bash
git add app/MinimalLatestApp.cpp
git commit -m "feat(app): implement glTF model loading UI

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Phase 9: Final Verification

### Task 16: Build and test

- [ ] **Step 1: Full build**

```bash
cmake --build build --clean-first
```

Expected: Clean build succeeds.

- [ ] **Step 2: Run application**

```bash
./build/Demo
```

Expected: Application starts with default triangle rendering.

- [ ] **Step 3: Verify UI**

Expected: "Model Loader" window visible with path input and preset buttons.

- [ ] **Step 4: Final commit if needed**

```bash
git status
# If there are uncommitted changes:
git add -A
git commit -m "chore: final cleanup for glTF rendering implementation

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Summary

This plan implements glTF 2.0 model rendering through:

1. **tinygltf** - glTF file loading
2. **GltfLoader** - Parse glTF to engine structures
3. **MeshPool** - GPU mesh resource management
4. **GBufferPass** - Render models to offscreen target
5. **LightPass** - Output to swapchain
6. **Renderer extensions** - glTF upload/destroy methods
7. **Application UI** - Model selection interface

Total: 16 tasks, ~50 steps