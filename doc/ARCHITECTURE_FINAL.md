# Vulkan Demo - Final Runtime Architecture

## 1. Scope

This document captures the implemented architecture under `Demo/` after Task 24 cutover.

The active graphics submission path is now:

`SceneOpaquePass -> DrawStreamWriter -> DrawStream -> DrawStreamDecoder -> Renderer::recordGraphicCommands`

Direct runtime submission from pass-owned `DrawPacket[]` is no longer used in active flow.

## 2. Module Graph

```text
app/MinimalLatestApp.cpp
  -> render/Renderer
       -> render/PassExecutor
            -> passes/AnimateVerticesPass
            -> passes/SceneOpaquePass
            -> passes/PresentPass
            -> passes/ImguiPass
       -> render/DrawStreamWriter   (pass-side recording)
       -> render/DrawStreamDecoder  (backend decode)
       -> render/BindGroups
       -> render/TransientAllocator
       -> gfx/Context
       -> gfx/ResourceAllocator
       -> gfx/Swapchain
       -> gfx/GBuffer
       -> gfx/SamplerPool
```

## 3. Lifetime Graph

### Device lifetime (`Renderer::DeviceLifetimeResources`)
- Vulkan context/device/queue
- VMA allocator + sampler pool
- shader pipelines and layouts
- persistent GPU buffers (vertex, points)
- transient command pool for setup/rebuild helpers

### Swapchain-dependent lifetime (`Renderer::SwapchainDependentResources`)
- swapchain images/views and WSI state
- GBuffer color/depth targets sized to viewport

### Per-frame lifetime (`Renderer::PerFrameResources::FrameData`)
- command pool + command buffer
- timeline signal bookkeeping (`lastSignalValue`)
- per-frame transient allocator storage
- per-frame draw-dynamic bind group

### Per-pass scratch (`Renderer::PerPassResources`)
- `DrawStream drawStream`
- `std::vector<DrawStreamDecoder::DecodedDraw> decodedDraws`

## 4. Pass Sequence

Execution order in `Renderer::init()` registration:

1. `AnimateVerticesPass` (compute)
2. `SceneOpaquePass` (geometry into GBuffer)
3. `PresentPass` (begin swapchain dynamic rendering)
4. `ImguiPass` (record UI in active rendering scope and end swapchain rendering)

Runtime frame sequence:

1. Wait timeline for frame slot
2. Acquire swapchain image
3. Begin command recording
4. Execute pass graph in fixed order
5. Submit + signal timeline/binary semaphores
6. Present and advance frame ring

## 5. Handle/Pool Model

Typed handles are defined in `common/Handles.h` and carried through pools:

- `HandlePool<PipelineHandle, PipelineRecord>` in renderer device resources
- `HandlePool<TextureHandle, TextureRecord>` and `HandlePool<MaterialHandle, MaterialRecord>`
- `HandlePool<BindGroupHandle, BindGroupResource>` for descriptor layouts/sets

Handle-based access rules:
- runtime validates stale handles via pool lookups before use
- pass graph shares resources via stable pseudo-handles (`kPass*Handle` constants in `Pass.h`)

## 6. Bind-Group Model

`render/BindGroups.h` defines explicit bind-group descriptors and set slots.

Active slots:
- `BindGroupSetSlot::material` (global material textures)
- `BindGroupSetSlot::drawDynamic` (per-frame dynamic UBO descriptor set)

Shader-set compatibility is mapped by `Renderer::mapSetSlotToLegacyShaderSet(...)`.

Descriptor ownership:
- material set is global (`m_materials.materialBindGroup`)
- draw-dynamic set is per-frame (`FrameData::sceneBindGroup`)

## 7. Pipeline Model

Pipelines are prebuilt and registered with typed handles:

- graphics non-textured variant
- graphics textured variant
- compute variant

`Renderer::getPipeline(...)` enforces bind-point correctness at use sites.

Graphics pipeline selection in scene pass is explicit by variant enum.

## 8. Transient Allocator Model

`render/TransientAllocator` provides a per-frame linear allocation arena for draw-dynamic payloads.

Rules:
- reset once per frame slot after timeline wait
- allocate per draw during stream recording in `SceneOpaquePass`
- bind via dynamic UBO offset through per-frame draw-dynamic descriptor set

## 9. Draw Stream Model

### Recording
`SceneOpaquePass` records draw intent through `DrawStreamWriter`:

- canonical pre-draw state order is preserved by writer implementation
- stream uses implicit dirty-mask encoding (state entries emitted only when changed)
- state entries include authoritative dynamic buffer + dynamic offset for each draw

### Decoding
`DrawStreamDecoder` reconstructs effective draw state from the entry sequence:

- tracks current pipeline/material/mesh/dynamicBuffer/dynamicOffset
- validates that all required state has been established before any draw
- emits `DecodedDraw` records consumed by backend command recording

### Submission
`Renderer::recordGraphicCommands(...)` now consumes decoded draw-stream state as primary runtime backend.

Decoded dynamic state is consumed as-is; backend no longer overwrites `dynamicBuffer` / `dynamicOffset` after decode.

`DrawPacket` remains in source tree only as legacy/oracle structure and is not the active pass-to-backend submission path.

## 10. Extension Points

1. **Additional render passes**
   - Add new `PassNode` implementations
   - Register order in `Renderer::init()`
   - Declare dependencies in `getDependencies()`

2. **Draw stream opcodes**
   - Extend `StreamEntryType`
   - update writer emit logic and decoder state machine together
   - keep deterministic decode invariants

3. **Material model growth**
   - expand material record payload
   - extend descriptor writes and bind-group layout generation

4. **Mesh binding model**
   - currently mesh handle is decoded and carried but not yet used for indexed mesh registries
   - integrate mesh pools + vertex/index binding via decoded mesh state

## 11. Recommended Expansion Order

1. Expand mesh model and consume decoded `mesh` handle in backend
2. Add explicit per-pass resource structs where pass-local state grows
3. Add draw-stream validation tooling (stream dump + decode assertions in debug builds)
4. Add additional scene/render passes (lighting/post) on top of pass dependencies
5. Replace legacy shader-set mapping with direct bind-group slot contracts
