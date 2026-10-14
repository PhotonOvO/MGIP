#include "render/passes/EditorOverlayPass.h"

#include "render/editor/GizmoGeometry.h"
#include "rhi/RHICommandAllocator.h"
#include "rhi/RHISurface.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if !defined(MGIF_TEST_SOURCE_DIR)
#define MGIF_TEST_SOURCE_DIR "."
#endif

namespace {

using namespace demo;

void require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

template <typename T>
concept NativeHandleSurface = requires(T value) {
  value.vkHandle;
  value.d3d12Handle;
  value.metalHandle;
};

static_assert(!NativeHandleSurface<EditorOverlayPassFrame>);
static_assert(!NativeHandleSurface<EditorOverlayHelperDepthBinding>);
static_assert(sizeof(editor::EditorOverlayDrawConstants) == 96u);
static_assert(sizeof(editor::EditorOverlayFrameUniforms) == 160u);
static_assert(sizeof(editor::EditorOverlayLineInstance) == 48u);
static_assert(sizeof(editor::EditorOverlayGizmoVertex) == 24u);

class FakeQueue final : public rhi::Queue {
public:
  [[nodiscard]] rhi::QueueIdentity identity() const noexcept override {
    return {71u, 3u};
  }

  [[nodiscard]] rhi::QueueInfo info() const noexcept override {
    return {.queueClass = rhi::QueueClass::graphics, .available = true};
  }

  [[nodiscard]] rhi::SubmissionToken submit(const rhi::SubmitBatch &) override {
    return issue();
  }

  [[nodiscard]] bool isComplete(rhi::SubmissionToken token) const override {
    ++completionQueries;
    if (throwNextCompletionQuery) {
      throwNextCompletionQuery = false;
      throw std::runtime_error("injected completion-query failure");
    }
    return token.queue == identity() && token.value != 0u &&
           token.value <= completedValue;
  }

  void wait(rhi::SubmissionToken token) override {
    ++waitCount;
    completedValue = std::max(completedValue, token.value);
  }

  void waitIdle() override {
    ++waitIdleCount;
    completedValue = lastValue;
  }

  [[nodiscard]] rhi::SubmissionToken
  lastSubmittedToken() const noexcept override {
    return lastValue == 0u ? rhi::SubmissionToken{}
                           : rhi::SubmissionToken{identity(), lastValue};
  }

  [[nodiscard]] rhi::SubmissionToken issue() {
    return {identity(), ++lastValue};
  }

  void complete(rhi::SubmissionToken token) {
    require(token.queue == identity(), "fake completion queue differs");
    completedValue = std::max(completedValue, token.value);
  }

  mutable std::uint64_t completionQueries{0u};
  mutable bool throwNextCompletionQuery{false};
  std::uint64_t waitCount{0u};
  std::uint64_t waitIdleCount{0u};
  std::uint64_t lastValue{100u};
  std::uint64_t completedValue{0u};
};

struct BufferRecord final {
  rhi::BufferDesc desc{};
  std::string name;
  std::vector<std::byte> bytes;
  bool alive{true};
};

struct TextureRecord final {
  rhi::TextureDesc desc{};
  std::string name;
  bool alive{true};
};

struct ViewRecord final {
  rhi::TextureViewCreateDesc desc{};
  std::string name;
  bool alive{true};
};

struct LayoutRecord final {
  std::vector<rhi::ArgumentBinding> bindings;
  std::string name;
  bool alive{true};
};

struct TableRecord final {
  rhi::ArgumentTableCreateDesc desc{};
  std::string name;
  std::vector<rhi::ArgumentWrite> writes;
  bool alive{true};
};

struct ShaderRecord final {
  rhi::ShaderIRFormat format{rhi::ShaderIRFormat::unknown};
  std::vector<std::byte> bytes;
  std::string name;
  bool alive{true};
};

struct PipelineStageRecord final {
  rhi::ShaderStage stage{rhi::ShaderStage::none};
  rhi::ShaderLibraryHandle library{};
  std::string entryPoint;
};

struct PipelineRecord final {
  std::uint32_t variant{0u};
  std::vector<PipelineStageRecord> stages;
  std::vector<rhi::VertexBindingDesc> vertexBindings;
  std::vector<rhi::VertexAttributeDesc> vertexAttributes;
  rhi::RasterState raster{};
  rhi::DepthState depth{};
  std::vector<rhi::BlendAttachmentState> blends;
  std::vector<rhi::DynamicState> dynamicStates;
  std::vector<rhi::TextureFormat> colorFormats;
  rhi::TextureFormat depthFormat{rhi::TextureFormat::undefined};
  std::vector<rhi::PipelineArgumentSlotDesc> argumentSlots;
  std::vector<rhi::RootBindingDesc> rootBindings;
  bool alive{true};
};

class FakeCommandAllocator final : public rhi::CommandAllocator {
public:
  [[nodiscard]] rhi::QueueClass queueClass() const noexcept override {
    return rhi::QueueClass::graphics;
  }
  void reset(rhi::SubmissionToken) override {}
  [[nodiscard]] std::unique_ptr<rhi::CommandBuffer>
  createCommandBuffer() override {
    return nullptr;
  }
};

class FakeDevice final : public rhi::Device {
public:
  void init(const rhi::DeviceCreateInfo &) override {}
  void deinit() override {}
  [[nodiscard]] rhi::BackendInfo getBackendInfo() const override {
    return {.type = rhi::BackendType::vulkan, .apiName = "overlay-fake"};
  }
  [[nodiscard]] const char *getDeviceName() const override {
    return "EditorOverlayFakeDevice";
  }
  [[nodiscard]] const rhi::PhysicalDeviceInfo &
  getPhysicalDeviceInfo() const override {
    return physicalInfo;
  }
  [[nodiscard]] const rhi::DeviceFeatureInfo &
  getEnabledFeatureInfo() const override {
    return featureInfo;
  }
  [[nodiscard]] rhi::CapabilityReport queryCapabilities() const override {
    return {};
  }
  [[nodiscard]] bool supports(rhi::CapabilityTier) const override {
    return true;
  }
  [[nodiscard]] const rhi::MemoryProperties &
  getPhysicalMemoryProperties() const override {
    return memoryProperties;
  }
  [[nodiscard]] rhi::Queue *getQueue(rhi::QueueClass queueClass) override {
    return queueClass == rhi::QueueClass::graphics ? &queue : nullptr;
  }
  [[nodiscard]] std::unique_ptr<rhi::CommandAllocator>
  createCommandAllocator(rhi::QueueClass queueClass) override {
    return queueClass == rhi::QueueClass::graphics
               ? std::make_unique<FakeCommandAllocator>()
               : nullptr;
  }
  void waitIdle() override { ++waitIdleCount; }
  void initSurface(rhi::Surface &, const rhi::WindowHandle &) override {}
  [[nodiscard]] std::unique_ptr<rhi::Swapchain>
  createSwapchain(rhi::Surface &, bool) override {
    return nullptr;
  }
  [[nodiscard]] bool isFormatSupported(
      rhi::TextureFormat, rhi::FormatFeatureFlag) const override {
    return formatSupported;
  }

  rhi::TextureViewHandle
  createTextureView(const rhi::TextureViewCreateDesc &desc) override {
    if (failNextView) {
      failNextView = false;
      return {};
    }
    const rhi::TextureViewHandle handle{{nextView++, 1u}};
    if (views.size() <= handle.index) {
      views.resize(handle.index + 1u);
    }
    views[handle.index] = {desc, desc.debugName ? desc.debugName : "", true};
    return handle;
  }
  void destroyTextureView(rhi::TextureViewHandle handle) override {
    views.at(handle.index).alive = false;
    destroyed.push_back(
        {rhi::ResourceKind::TextureView, handle.index, handle.generation});
  }
  rhi::TextureHandle createTexture(const rhi::TextureDesc &desc) override {
    if (failNextTexture) {
      failNextTexture = false;
      return {};
    }
    const rhi::TextureHandle handle{nextTexture++, 1u};
    if (textures.size() <= handle.index) {
      textures.resize(handle.index + 1u);
    }
    textures[handle.index] = {desc, desc.debugName ? desc.debugName : "", true};
    return handle;
  }
  void destroyTexture(rhi::TextureHandle handle) override {
    textures.at(handle.index).alive = false;
    destroyed.push_back(
        {rhi::ResourceKind::Texture, handle.index, handle.generation});
  }
  rhi::BufferHandle createBuffer(const rhi::BufferDesc &desc) override {
    if (failNextBuffer) {
      failNextBuffer = false;
      return {};
    }
    require(desc.size != 0u &&
                desc.size <= (std::numeric_limits<std::size_t>::max)(),
            "fake buffer size is invalid");
    const rhi::BufferHandle handle{nextBuffer++, 1u};
    if (buffers.size() <= handle.index) {
      buffers.resize(handle.index + 1u);
    }
    BufferRecord record;
    record.desc = desc;
    record.name = desc.debugName ? desc.debugName : "";
    record.bytes.resize(static_cast<std::size_t>(desc.size));
    buffers[handle.index] = std::move(record);
    return handle;
  }
  void destroyBuffer(rhi::BufferHandle handle) override {
    buffers.at(handle.index).alive = false;
    destroyed.push_back(
        {rhi::ResourceKind::Buffer, handle.index, handle.generation});
  }
  [[nodiscard]] rhi::GpuPtr
  getBufferGpuAddress(rhi::BufferHandle handle) const override {
    return {0x100000u + static_cast<std::uint64_t>(handle.index) * 0x1000u};
  }
  void *mapBuffer(rhi::BufferHandle handle) override {
    return buffers.at(handle.index).bytes.data();
  }
  void unmapBuffer(rhi::BufferHandle) override { ++unmapCalls; }
  rhi::Result<rhi::MappedBufferRange>
  mapBufferRange(rhi::BufferHandle handle,
                 const rhi::BufferMapDesc &desc) override {
    if (failNextMap) {
      failNextMap = false;
      return rhi::Result<rhi::MappedBufferRange>::fail(
          rhi::RHIErrorCode::backendFailure, "injected map failure");
    }
    BufferRecord &record = buffers.at(handle.index);
    const std::uint64_t size =
        desc.size == 0u ? record.bytes.size() - desc.offset : desc.size;
    if (desc.offset > record.bytes.size() ||
        size > record.bytes.size() - desc.offset) {
      return rhi::Result<rhi::MappedBufferRange>::fail(
          rhi::RHIErrorCode::invalidArgument, "fake map range is invalid");
    }
    return rhi::Result<rhi::MappedBufferRange>::ok({
        .data = record.bytes.data() + desc.offset,
        .offset = desc.offset,
        .size = size,
        .coherent = true,
        .persistent = false,
    });
  }
  rhi::RHIResult flushMappedBufferRange(rhi::BufferHandle, std::uint64_t,
                                        std::uint64_t) override {
    ++flushCalls;
    if (failNextFlush) {
      failNextFlush = false;
      return rhi::RHIResult::fail(rhi::RHIErrorCode::backendFailure,
                                  "injected flush failure");
    }
    return rhi::RHIResult::ok();
  }
  rhi::SamplerHandle createSampler(const rhi::SamplerDesc &desc) override {
    const rhi::SamplerHandle handle{nextSampler++, 1u};
    samplers.push_back(desc);
    return handle;
  }
  void destroySampler(rhi::SamplerHandle handle) override {
    destroyed.push_back(
        {rhi::ResourceKind::Sampler, handle.index, handle.generation});
  }
  rhi::ArgumentLayoutHandle
  createArgumentLayout(const rhi::ArgumentLayoutDesc &desc) override {
    const rhi::ArgumentLayoutHandle handle{nextLayout++, 1u};
    if (layouts.size() <= handle.index) {
      layouts.resize(handle.index + 1u);
    }
    layouts[handle.index] = {
        std::vector<rhi::ArgumentBinding>(desc.bindings.begin(),
                                          desc.bindings.end()),
        desc.debugName ? desc.debugName : "", true};
    return handle;
  }
  void destroyArgumentLayout(rhi::ArgumentLayoutHandle handle) override {
    layouts.at(handle.index).alive = false;
    destroyed.push_back({rhi::ResourceKind::ArgumentLayout, handle.index,
                         handle.generation});
  }
  rhi::ArgumentTableHandle
  createArgumentTable(const rhi::ArgumentTableCreateDesc &desc) override {
    const rhi::ArgumentTableHandle handle{nextTable++, 1u};
    if (tables.size() <= handle.index) {
      tables.resize(handle.index + 1u);
    }
    tables[handle.index] = {desc, desc.debugName ? desc.debugName : "", {},
                            true};
    return handle;
  }
  void destroyArgumentTable(rhi::ArgumentTableHandle handle) override {
    tables.at(handle.index).alive = false;
    destroyed.push_back({rhi::ResourceKind::ArgumentTable, handle.index,
                         handle.generation});
  }
  void updateArgumentTable(rhi::ArgumentTableHandle table,
                           rhi::ArgumentWriteBatch writes) override {
    if (failNextTableUpdate) {
      failNextTableUpdate = false;
      throw std::runtime_error("injected table update failure");
    }
    tables.at(table.index).writes.assign(writes.begin(), writes.end());
    tableUpdateBatches.push_back(table);
  }
  [[nodiscard]] rhi::ArgumentLayoutHandle
  getArgumentTableLayout(rhi::ArgumentTableHandle table) const override {
    return tables.at(table.index).desc.layout;
  }
  rhi::PipelineHandle
  createGraphicsPipeline(const rhi::GraphicsPipelineDesc &desc) override {
    if (failPipelineCreationAt != 0u &&
        pipelinesCreated + 1u == failPipelineCreationAt) {
      ++pipelinesCreated;
      return {};
    }
    const rhi::PipelineHandle handle{nextPipeline++, 1u};
    if (pipelines.size() <= handle.index) {
      pipelines.resize(handle.index + 1u);
    }
    PipelineRecord record;
    record.variant = desc.specializationVariant;
    for (const rhi::ShaderEntry &stage : desc.shaderStages) {
      record.stages.push_back(
          {stage.stage, stage.library, std::string(stage.entryPoint)});
    }
    record.vertexBindings.assign(desc.vertexInput.bindings.begin(),
                                 desc.vertexInput.bindings.end());
    record.vertexAttributes.assign(desc.vertexInput.attributes.begin(),
                                   desc.vertexInput.attributes.end());
    record.raster = desc.rasterState;
    record.depth = desc.depthState;
    record.blends.assign(desc.blendStates.begin(), desc.blendStates.end());
    record.dynamicStates.assign(desc.dynamicStates.begin(),
                                desc.dynamicStates.end());
    record.colorFormats.assign(desc.renderingInfo.colorFormats.begin(),
                               desc.renderingInfo.colorFormats.end());
    record.depthFormat = desc.renderingInfo.depthFormat;
    record.argumentSlots.assign(desc.bindingSchema.argumentSlots.begin(),
                                desc.bindingSchema.argumentSlots.end());
    record.rootBindings.assign(desc.bindingSchema.rootBindings.begin(),
                               desc.bindingSchema.rootBindings.end());
    pipelines[handle.index] = std::move(record);
    ++pipelinesCreated;
    return handle;
  }
  rhi::PipelineHandle
  createComputePipeline(const rhi::ComputePipelineDesc &) override {
    return {};
  }
  void destroyPipeline(rhi::PipelineHandle handle) override {
    pipelines.at(handle.index).alive = false;
    destroyed.push_back(
        {rhi::ResourceKind::Pipeline, handle.index, handle.generation});
  }
  rhi::QueryPoolHandle createQueryPool(std::uint32_t) override { return {}; }
  void destroyQueryPool(rhi::QueryPoolHandle) override {}
  std::uint64_t getQueryPoolResult(rhi::QueryPoolHandle,
                                   std::uint32_t) override {
    return 0u;
  }
  bool getQueryPoolResultsWithAvailability(rhi::QueryPoolHandle,
                                           std::uint32_t,
                                           std::span<std::uint64_t>) override {
    return false;
  }
  rhi::ShaderLibraryHandle
  createShaderLibrary(const rhi::ShaderLibraryDesc &desc) override {
    const rhi::ShaderLibraryHandle handle{nextShader++, 1u};
    if (shaders.size() <= handle.index) {
      shaders.resize(handle.index + 1u);
    }
    shaders[handle.index] = {
        desc.format,
        std::vector<std::byte>(desc.data.begin(), desc.data.end()),
        desc.debugName ? desc.debugName : "", true};
    return handle;
  }
  void destroyShaderLibrary(rhi::ShaderLibraryHandle handle) override {
    shaders.at(handle.index).alive = false;
  }

  [[nodiscard]] rhi::BufferHandle bufferNamed(std::string_view name) const {
    for (std::size_t index = 1u; index < buffers.size(); ++index) {
      if (buffers[index].alive && buffers[index].name == name) {
        return {static_cast<std::uint32_t>(index), 1u};
      }
    }
    return {};
  }

  [[nodiscard]] std::size_t livePipelineCount() const {
    return static_cast<std::size_t>(std::count_if(
        std::next(pipelines.begin()), pipelines.end(),
        [](const PipelineRecord &record) { return record.alive; }));
  }

  FakeQueue queue;
  bool formatSupported{true};
  bool failNextTexture{false};
  bool failNextView{false};
  bool failNextBuffer{false};
  bool failNextMap{false};
  bool failNextFlush{false};
  bool failNextTableUpdate{false};
  std::uint64_t failPipelineCreationAt{0u};
  std::uint64_t pipelinesCreated{0u};
  std::uint64_t waitIdleCount{0u};
  std::uint64_t flushCalls{0u};
  std::uint64_t unmapCalls{0u};
  std::vector<BufferRecord> buffers{1u};
  std::vector<TextureRecord> textures{1u};
  std::vector<ViewRecord> views{1u};
  std::vector<rhi::SamplerDesc> samplers;
  std::vector<LayoutRecord> layouts{1u};
  std::vector<TableRecord> tables{1u};
  std::vector<PipelineRecord> pipelines{1u};
  std::vector<ShaderRecord> shaders{1u};
  std::vector<rhi::ArgumentTableHandle> tableUpdateBatches;
  std::vector<rhi::ResourceHandle> destroyed;

private:
  std::uint32_t nextBuffer{1u};
  std::uint32_t nextTexture{1u};
  std::uint32_t nextView{1u};
  std::uint32_t nextSampler{1u};
  std::uint32_t nextLayout{1u};
  std::uint32_t nextTable{1u};
  std::uint32_t nextPipeline{1u};
  std::uint32_t nextShader{1u};
  rhi::PhysicalDeviceInfo physicalInfo{};
  rhi::DeviceFeatureInfo featureInfo{};
  rhi::MemoryProperties memoryProperties{};
};

struct DrawRecord final {
  rhi::PipelineHandle pipeline{};
  rhi::ArgumentTableHandle table{};
  std::vector<std::byte> constants;
  rhi::BufferHandle vertexBuffer{};
  std::uint64_t vertexOffset{0u};
  bool indexed{false};
  rhi::DrawDesc draw{};
  rhi::DrawIndexedDesc indexedDraw{};
};

class FakeRenderEncoder final : public rhi::RenderEncoder {
public:
  void reset() {
    currentPipeline = {};
    currentTable = {};
    currentConstants.clear();
    currentVertexBuffer = {};
    currentVertexOffset = 0u;
    draws.clear();
  }
  void setPipeline(rhi::PipelineHandle pipeline) override {
    currentPipeline = pipeline;
  }
  void setArgumentTable(rhi::ShaderStage, std::uint32_t slot,
                        rhi::ArgumentTableHandle table) override {
    require(!currentPipeline.isNull(),
            "argument table was bound before a graphics pipeline");
    argumentSlots.push_back(slot);
    currentTable = table;
  }
  void setDynamicBuffer(rhi::ShaderStage, std::uint32_t, rhi::BufferHandle,
                        std::uint64_t, std::uint64_t) override {}
  void setRootConstants(rhi::ShaderStage, std::uint32_t slot,
                        std::span<const std::byte> data) override {
    rootSlots.push_back(slot);
    currentConstants.assign(data.begin(), data.end());
  }
  void setRootPointer(rhi::ShaderStage, std::uint32_t,
                      rhi::GpuPtr) override {}
  void setViewport(const rhi::Viewport &value) override { viewport = value; }
  void setScissor(const rhi::Rect2D &value) override { scissor = value; }
  void bindVertexBuffers(
      std::uint32_t,
      std::span<const rhi::VertexBufferBinding> bindings) override {
    require(bindings.size() == 1u, "fake expects one vertex binding");
    currentVertexBuffer = bindings[0].buffer;
    currentVertexOffset = bindings[0].offset;
  }
  void bindIndexBuffer(rhi::BufferHandle, std::uint64_t,
                       rhi::IndexFormat) override {}
  void readInputAttachment(std::uint32_t) override {}
  void draw(const rhi::DrawDesc &desc) override {
    draws.push_back({currentPipeline, currentTable, currentConstants,
                     currentVertexBuffer, currentVertexOffset, false, desc,
                     {}});
  }
  void drawIndexed(const rhi::DrawIndexedDesc &desc) override {
    DrawRecord record{currentPipeline, currentTable, currentConstants,
                      currentVertexBuffer, currentVertexOffset, true};
    record.indexedDraw = desc;
    draws.push_back(std::move(record));
  }
  void drawIndexedIndirect(const rhi::DrawIndirectDesc &) override {}
  void drawIndexedIndirectCount(const rhi::DrawIndirectCountDesc &) override {}
  void drawIndirect(const rhi::DrawIndirectDesc &) override {}
  void drawMeshTasks(std::uint32_t, std::uint32_t, std::uint32_t) override {}
  void drawMeshTasksIndirect(const rhi::DrawIndirectDesc &) override {}

  rhi::PipelineHandle currentPipeline{};
  rhi::ArgumentTableHandle currentTable{};
  std::vector<std::byte> currentConstants;
  rhi::BufferHandle currentVertexBuffer{};
  std::uint64_t currentVertexOffset{0u};
  rhi::Viewport viewport{};
  rhi::Rect2D scissor{};
  std::vector<std::uint32_t> argumentSlots;
  std::vector<std::uint32_t> rootSlots;
  std::vector<DrawRecord> draws;
};

struct RenderPassRecord final {
  rhi::Rect2D area{};
  rhi::RenderTargetDesc color{};
  rhi::DepthTargetDesc depth{};
};

class FakeComputeEncoder final : public rhi::ComputeEncoder {
public:
  void setPipeline(rhi::PipelineHandle) override {}
  void setArgumentTable(std::uint32_t, rhi::ArgumentTableHandle) override {}
  void setRootConstants(std::uint32_t, std::span<const std::byte>) override {}
  void setRootPointer(std::uint32_t, rhi::GpuPtr) override {}
  void dispatch(const rhi::DispatchDesc &) override {}
  void dispatchIndirect(const rhi::DispatchIndirectDesc &) override {}
  void copyBuffer(rhi::BufferHandle, std::uint64_t, rhi::BufferHandle,
                  std::uint64_t, std::uint64_t) override {}
  void copyBufferToTexture(const rhi::BufferTextureCopyDesc &) override {}
  void copyTextureToBuffer(const rhi::BufferTextureCopyDesc &) override {}
  void blitTexture(const rhi::TextureBlitDesc &) override {}
  void fillBuffer(rhi::BufferHandle, std::uint64_t, std::uint64_t,
                  std::uint32_t) override {}
};

class FakeCommandBuffer final : public rhi::CommandBuffer {
public:
  void begin(rhi::CommandAllocator &) override {
    stateValue = rhi::CommandBufferState::recording;
  }
  void end() override { stateValue = rhi::CommandBufferState::executable; }
  [[nodiscard]] rhi::CommandBufferState state() const noexcept override {
    return stateValue;
  }
  rhi::RenderEncoder *beginRenderPass(const rhi::RenderPassDesc &desc) override {
    if (returnNullEncoder) {
      return nullptr;
    }
    require(desc.colorTargets.size() == 1u && desc.depthTarget != nullptr,
            "overlay pass attachments are incomplete");
    passes.push_back(
        {desc.renderArea, desc.colorTargets[0], *desc.depthTarget});
    encoder.reset();
    return &encoder;
  }
  rhi::ComputeEncoder *beginComputePass() override { return &compute; }
  void endEncoding() override { ++endEncodingCalls; }
  void barrier(rhi::StageFlags, rhi::StageFlags,
               rhi::HazardFlags) override {}
  void resourceBarrier(std::span<const rhi::TextureBarrier>,
                       std::span<const rhi::BufferBarrier>,
                       std::span<const rhi::AliasingBarrier>) override {}
  void clearColorTexture(rhi::TextureHandle,
                         const rhi::TextureSubresourceRange &,
                         const rhi::ClearColorValue &) override {}
  void beginEvent(const char *name) override {
    events.emplace_back(name ? name : "");
  }
  void endEvent() override { ++endEventCalls; }
  void resetQueryPool(rhi::QueryPoolHandle, std::uint32_t,
                      std::uint32_t) override {}
  void writeTimestamp(rhi::QueryPoolHandle, std::uint32_t, bool) override {}

  bool returnNullEncoder{false};
  rhi::CommandBufferState stateValue{rhi::CommandBufferState::recording};
  FakeRenderEncoder encoder;
  FakeComputeEncoder compute;
  std::vector<RenderPassRecord> passes;
  std::vector<std::string> events;
  std::uint64_t endEncodingCalls{0u};
  std::uint64_t endEventCalls{0u};
};

editor::EditorViewportMetrics metrics(std::uint64_t generation = 9u,
                                      bool reverse = true) {
  editor::EditorViewportMetrics value;
  value.logical = {{0.0, 0.0}, {800.0, 600.0}};
  value.framebufferLogicalOrigin = {0.0, 0.0};
  value.physical = {{0u, 0u}, {800u, 600u}};
  value.depthPhysical = {{100u, 50u}, {400u, 300u}};
  value.colorAttachment = {1024u, 768u};
  value.depthAttachment = {1024u, 768u};
  value.framebufferScale = {1.0, 1.0};
  value.viewportGeneration = generation;
  value.projection = {generation + 1u, generation + 2u};
  value.view = {generation + 3u, generation + 4u};
  value.convention = {editor::EditorClipDepthRange::zeroToOne, reverse, true};
  value.projectionKind = editor::EditorProjectionKind::perspective;
  value.verticalFieldOfViewRadians = 1.0;
  require(value.valid(), "test viewport metrics are invalid");
  return value;
}

editor::OverlayGenerationBinding generationFor(
    const editor::EditorViewportMetrics &viewport) {
  return {viewport.viewportGeneration, viewport.projection, viewport.view};
}

struct PacketFixture final {
  editor::GridDrawPacket grid{};
  std::array<editor::GridLinePacket, 2u> lines{};
  std::array<editor::GridLinePacket, 2u> axes{};
  editor::GizmoDrawPacket gizmo{};
};

editor::GizmoColorToken colorFor(editor::GizmoComponent component) {
  switch (component) {
  case editor::GizmoComponent::translateX:
  case editor::GizmoComponent::rotateX:
  case editor::GizmoComponent::scaleX:
    return editor::GizmoColorToken::xAxis;
  case editor::GizmoComponent::translateY:
  case editor::GizmoComponent::rotateY:
  case editor::GizmoComponent::scaleY:
    return editor::GizmoColorToken::yAxis;
  case editor::GizmoComponent::translateZ:
  case editor::GizmoComponent::rotateZ:
  case editor::GizmoComponent::scaleZ:
    return editor::GizmoColorToken::zAxis;
  case editor::GizmoComponent::translateXY:
    return editor::GizmoColorToken::xyPlane;
  case editor::GizmoComponent::translateXZ:
    return editor::GizmoColorToken::xzPlane;
  case editor::GizmoComponent::translateYZ:
    return editor::GizmoColorToken::yzPlane;
  case editor::GizmoComponent::uniformScale:
    return editor::GizmoColorToken::uniform;
  case editor::GizmoComponent::none:
    return editor::GizmoColorToken::uniform;
  }
  return editor::GizmoColorToken::uniform;
}

PacketFixture packets(const editor::EditorViewportMetrics &viewport,
                      bool dither = false) {
  PacketFixture fixture;
  fixture.grid.generation = generationFor(viewport);
  fixture.grid.enabled = true;
  fixture.grid.generatedLineCount = 4u;
  fixture.lines[0] = {{-10.0, 0.0, -1.0},
                      {10.0, 0.0, -1.0},
                      editor::GridLineKind::parallelX,
                      editor::GridBlendToken::additive,
                      0u, 1.0, 1.0, 1.0, 1.0, 0.5};
  fixture.lines[1] = {{1.0, 0.0, -10.0},
                      {1.0, 0.0, 10.0},
                      editor::GridLineKind::parallelZ,
                      editor::GridBlendToken::additive,
                      1u, 1.0, 1.0, 1.0, 1.0, 0.4};
  fixture.axes[0] = {{-10.0, 0.0, 0.0},
                     {10.0, 0.0, 0.0},
                     editor::GridLineKind::xAxis,
                     editor::GridBlendToken::alphaOver,
                     0xffu, 1.0, 1.0, 1.0, 1.0, 1.0};
  fixture.axes[1] = {{0.0, 0.0, -10.0},
                     {0.0, 0.0, 10.0},
                     editor::GridLineKind::zAxis,
                     editor::GridBlendToken::alphaOver,
                     0xffu, 1.0, 1.0, 1.0, 1.0, 1.0};

  const editor::GizmoGeometryAtlas &atlas =
      editor::canonicalGizmoGeometry();
  fixture.gizmo.generation = generationFor(viewport);
  fixture.gizmo.depth = dither ? editor::GizmoDepthToken::occlusionDither
                               : editor::GizmoDepthToken::xRay;
  fixture.gizmo.plane = editor::GizmoPlaneToken::checkerDiscard;
  fixture.gizmo.worldScale = 1.0;
  fixture.gizmo.canonicalVisualDiameter = atlas.canonicalVisualDiameter();
  for (std::size_t index = 0u; index < editor::kGizmoComponentCount;
       ++index) {
    auto &draw = fixture.gizmo.components[index];
    draw.range = atlas.ranges()[index];
    draw.visible = true;
    draw.color = colorFor(draw.range.component);
    draw.state = index == 0u ? editor::GizmoVisualStateToken::hovered
                             : editor::GizmoVisualStateToken::normal;
  }
  return fixture;
}

EditorOverlayPassTargets targets() {
  return {
      .outputTexture = {500u, 7u},
      .outputView = {{501u, 7u}},
      .outputFormat = rhi::TextureFormat::bgra8Unorm,
      .outputSamples = rhi::SampleCount::count1,
      .sceneDepthTexture = {600u, 8u},
      .sceneDepthView = {{601u, 8u}},
      .sceneDepthFormat = rhi::TextureFormat::d32Sfloat,
      .sceneDepthSamples = rhi::SampleCount::count1,
  };
}

EditorOverlayPassFrame frame(std::uint64_t serial,
                             const editor::EditorViewportMetrics &viewport,
                             const PacketFixture &fixture) {
  EditorOverlayPassFrame value;
  value.frameSerial = serial;
  value.viewport = viewport;
  value.gridVisible = true;
  value.grid = fixture.grid;
  value.gridLines = fixture.lines;
  value.gridAxes = fixture.axes;
  value.gizmoVisible = true;
  value.gizmo = fixture.gizmo;
  value.targets = targets();
  return value;
}

void initializePass(EditorOverlayPass &pass, FakeDevice &device,
                    std::uint32_t frameSlots = 2u) {
  static const std::array<std::byte, 8u> shader{
      std::byte{3}, std::byte{2}, std::byte{35}, std::byte{7},
      std::byte{0}, std::byte{0}, std::byte{1}, std::byte{0}};
  const rhi::RHIResult initialized = pass.initialize(
      device, {.frameSlotCount = frameSlots,
               .shaderFormat = rhi::ShaderIRFormat::spirv,
               .shaderData = shader});
  require(initialized.succeeded(), "overlay pass initialization failed");
}

const PipelineRecord &pipelineVariant(const FakeDevice &device,
                                      std::uint32_t variant) {
  const auto found = std::find_if(
      device.pipelines.begin(), device.pipelines.end(),
      [variant](const PipelineRecord &record) {
        return record.alive && record.variant == variant;
      });
  require(found != device.pipelines.end(), "pipeline variant is missing");
  return *found;
}

void testInitializationAbiAndDependencies() {
  FakeDevice device;
  EditorOverlayPass pass;
  initializePass(pass, device);
  require(pass.initialized(), "initialized flag is false");
  require(device.shaders.size() == 2u &&
              device.shaders[1].format == rhi::ShaderIRFormat::spirv &&
              device.shaders[1].bytes.size() == 8u,
          "shader library descriptor was not copied");
  require(device.samplers.size() == 1u &&
              device.samplers[0].minFilter == rhi::Filter::nearest &&
              device.samplers[0].addressModeU ==
                  rhi::AddressMode::clampToEdge,
          "scene-depth sampler differs");
  require(device.layouts.size() == 2u &&
              device.layouts[1].bindings.size() == 2u &&
              device.layouts[1].bindings[0].binding ==
                  editor::kEditorOverlayFrameBinding &&
              device.layouts[1].bindings[0].type ==
                  rhi::ArgumentType::uniformBuffer &&
              device.layouts[1].bindings[1].type ==
                  rhi::ArgumentType::combinedImageSampler,
          "argument layout differs");
  require(device.tables.size() == 3u &&
              device.tables[1].desc.lifetime ==
                  rhi::ArgumentTableLifetime::frameLocal &&
              device.tables[2].desc.lifetime ==
                  rhi::ArgumentTableLifetime::frameLocal,
          "per-frame argument tables differ");
  require(device.bufferNamed("EditorOverlayFrameUniform").isNull() == false &&
              device.buffers[device.bufferNamed("EditorOverlayFrameUniform").index]
                      .desc.size == 256u,
          "frame uniform backing capacity is not CBV-aligned");

  const rhi::BufferHandle vertices =
      device.bufferNamed("EditorOverlayGizmoVertices");
  const rhi::BufferHandle indices =
      device.bufferNamed("EditorOverlayGizmoIndices");
  require(!vertices.isNull() && !indices.isNull() &&
              device.buffers[vertices.index].desc.size == 4116u * 24u &&
              device.buffers[indices.index].desc.size == 20124u * 4u,
          "canonical atlas upload sizes differ");
  require(device.livePipelineCount() == 0u && device.textures.size() == 1u,
          "surface resources were created eagerly");

  const auto dependencies = pass.getDependencies();
  require(dependencies.count == 3u, "overlay dependency count differs");
  require(dependencies.data[0].textureHandle == kPassOutputHandle &&
              dependencies.data[0].access == ResourceAccess::readWrite &&
              dependencies.data[0].stages == rhi::StageFlags::rasterColorOut &&
              dependencies.data[0].hazards == rhi::HazardFlags::textureWrites &&
              dependencies.data[0].requiredState ==
                  rhi::ResourceState::ColorAttachment,
          "output dependency differs");
  require(dependencies.data[1].textureHandle == kPassSceneDepthHandle &&
              dependencies.data[1].access == ResourceAccess::read &&
              dependencies.data[1].requiredState ==
                  rhi::ResourceState::ShaderRead,
          "scene-depth dependency differs");
  require(dependencies.data[2].textureHandle ==
                  kPassEditorOverlayHelperDepthHandle &&
              dependencies.data[2].access == ResourceAccess::write &&
              dependencies.data[2].stages == rhi::StageFlags::rasterDepthOut &&
              dependencies.data[2].hazards == rhi::HazardFlags::depthStencil &&
              dependencies.data[2].requiredState ==
                  rhi::ResourceState::DepthStencilAttachment,
          "helper-depth dependency differs");
  require(device.waitIdleCount == 0u && device.queue.waitIdleCount == 0u,
          "initialization waited for idle");
  require(pass.shutdown().succeeded(), "initial shutdown failed");
}

void testReverseRecordingAndPipelineFamilies() {
  FakeDevice device;
  EditorOverlayPass pass;
  initializePass(pass, device);
  const auto viewport = metrics();
  const PacketFixture fixture = packets(viewport);
  const EditorOverlayPrepareResult prepared =
      pass.prepareFrame(0u, frame(1u, viewport, fixture));
  require(prepared.succeeded() && prepared.ticket.valid() &&
              prepared.helperDepth.valid() &&
              prepared.helperDepth.extent.width == 1024u &&
              prepared.helperDepth.extent.height == 768u &&
              prepared.helperDepth.initialState ==
                  rhi::ResourceState::Undefined,
          "reverse frame preparation failed");
  require(device.livePipelineCount() == 8u &&
              pass.diagnostics().pipelineCreations == 8u &&
              pass.diagnostics().helperDepthCreations == 1u,
          "fixed pipeline/helper family differs");

  const PipelineRecord &grid = pipelineVariant(device, 0x7601u);
  const PipelineRecord &axes = pipelineVariant(device, 0x7602u);
  const PipelineRecord &forward = pipelineVariant(device, 0x7611u);
  const PipelineRecord &reverse = pipelineVariant(device, 0x7621u);
  require(grid.vertexBindings.size() == 1u &&
              grid.vertexBindings[0].stride == 48u &&
              grid.vertexBindings[0].inputRate ==
                  rhi::VertexInputRate::perInstance &&
              grid.blends[0].blendEnable &&
              grid.blends[0].dstColorBlendFactor == rhi::BlendFactor::one,
          "Grid pipeline differs");
  require(axes.blends[0].blendEnable &&
              axes.blends[0].dstColorBlendFactor ==
                  rhi::BlendFactor::oneMinusSrcAlpha,
          "axes pipeline differs");
  require(forward.depth.depthTestEnable &&
              forward.depth.depthWriteEnable &&
              forward.depth.depthCompareOp == rhi::CompareOp::lessOrEqual &&
              reverse.depth.depthCompareOp ==
                  rhi::CompareOp::greaterOrEqual,
          "forward/reverse helper-depth states differ");
  for (auto pipeline = std::next(device.pipelines.begin());
       pipeline != device.pipelines.end(); ++pipeline) {
    const PipelineRecord &record = *pipeline;
    if (!record.alive) {
      continue;
    }
    require(record.argumentSlots.size() == 1u &&
                record.argumentSlots[0].slot ==
                    editor::kEditorOverlayArgumentSlot &&
                record.rootBindings.size() == 1u &&
                record.rootBindings[0].slot ==
                    editor::kEditorOverlayRootSlot &&
                record.rootBindings[0].size == 96u &&
                record.depthFormat == rhi::TextureFormat::d32Sfloat,
            "pipeline binding/rendering schema differs");
  }

  require(!device.tableUpdateBatches.empty(),
          "frame descriptor table was not updated");
  const TableRecord &table =
      device.tables[device.tableUpdateBatches.back().index];
  require(table.writes.size() == 2u &&
              table.writes[1].type ==
                  rhi::ArgumentType::combinedImageSampler &&
              table.writes[1].accessIntent ==
                  rhi::ArgumentAccessIntent::sampledRead &&
              table.writes[1].textureView == targets().sceneDepthView,
          "scene-depth descriptor is not sampled-read");

  FakeCommandBuffer command;
  PassContext context{};
  context.frameIndex = 0u;
  context.commandBuffer = &command;
  pass.execute(context);
  require(command.passes.size() == 1u &&
              command.passes[0].color.loadOp == rhi::LoadOp::load &&
              command.passes[0].color.storeOp == rhi::StoreOp::store &&
              command.passes[0].depth.loadOp == rhi::LoadOp::clear &&
              command.passes[0].depth.storeOp == rhi::StoreOp::store &&
              command.passes[0].depth.clearValue.depth == 0.0f,
          "reverse render pass load/store/clear differs");
  require(command.encoder.viewport.x == 0.0f &&
              command.encoder.viewport.width == 800.0f &&
              command.encoder.scissor.extent.width == 800u,
          "physical viewport/scissor differs");
  require(command.encoder.draws.size() == 15u &&
              !command.encoder.draws[0].indexed &&
              command.encoder.draws[0].draw.vertexCount == 6u &&
              command.encoder.draws[0].draw.instanceCount == 2u &&
              !command.encoder.draws[1].indexed &&
              command.encoder.draws[1].draw.instanceCount == 2u,
          "Grid/axes draw order differs");
  const editor::GizmoGeometryAtlas &atlas =
      editor::canonicalGizmoGeometry();
  for (std::size_t index = 0u; index < editor::kGizmoComponentCount;
       ++index) {
    const DrawRecord &draw = command.encoder.draws[index + 2u];
    require(draw.indexed && draw.indexedDraw.indexCount ==
                                atlas.ranges()[index].indexCount &&
                draw.indexedDraw.firstIndex ==
                    atlas.ranges()[index].firstIndex &&
                draw.constants.size() == 96u,
            "Gizmo indexed draw differs");
    editor::EditorOverlayDrawConstants constants{};
    std::memcpy(&constants, draw.constants.data(), sizeof(constants));
    const bool plane = index >= 3u && index <= 5u;
    require(constants.flags.x == editor::kEditorOverlayDrawGizmo &&
                ((constants.flags.w &
                  editor::kEditorOverlayFlagPlaneChecker) != 0u) == plane,
            "Gizmo push flags differ");
    const PipelineRecord &pipeline = device.pipelines[draw.pipeline.index];
    require(pipeline.variant == (plane ? 0x7623u : 0x7621u),
            "reverse Gizmo pipeline selection differs");
  }
  require(pass.diagnostics().gridDraws == 1u &&
              pass.diagnostics().axesDraws == 1u &&
              pass.diagnostics().gizmoDraws == 13u &&
              pass.diagnostics().deviceIdleCalls == 0u &&
              pass.diagnostics().nativeEscapeCalls == 0u,
          "recording diagnostics differ");

  const rhi::SubmissionToken baseline = device.queue.lastSubmittedToken();
  require(!pass.resolveFrame(prepared.ticket,
                             EditorOverlayFrameDisposition::submitted,
                             baseline)
               .succeeded(),
          "old submission token was accepted");
  require(!pass.resolveFrame(
               prepared.ticket, EditorOverlayFrameDisposition::submitted,
               {device.queue.identity(), baseline.value + 2u})
               .succeeded(),
          "unpublished future submission token was accepted");
  const rhi::SubmissionToken token = device.queue.issue();
  require(pass.resolveFrame(prepared.ticket,
                            EditorOverlayFrameDisposition::submitted,
                            token)
              .succeeded(),
          "submitted frame resolution failed");
  require(pass.helperDepthBinding().initialState ==
              rhi::ResourceState::DepthStencilAttachment,
          "submitted helper state was not published");
  const auto busy = pass.prepareFrame(0u, frame(2u, viewport, fixture));
  require(!busy.succeeded() && busy.error == EditorOverlayPassError::frameBusy,
          "pending slot was rewritten");
  device.queue.throwNextCompletionQuery = true;
  require(pass.collectCompletedFrames() == 0u &&
              pass.diagnostics().lastError ==
                  EditorOverlayPassError::resourceFailure,
          "completion-query exception escaped or released the slot");
  device.queue.complete(token);
  require(pass.collectCompletedFrames() == 1u,
          "completed slot was not collected");
  require(pass.shutdown().succeeded(), "reverse pass shutdown failed");
  require(device.waitIdleCount == 0u && device.queue.waitIdleCount == 0u,
          "recording lifecycle waited for idle");
}

void testForwardDepthDitherAndFrameSlotReuse() {
  FakeDevice device;
  EditorOverlayPass pass;
  initializePass(pass, device);
  const auto forwardViewport = metrics(20u, false);
  const PacketFixture dither = packets(forwardViewport, true);
  const auto first = pass.prepareFrame(0u, frame(1u, forwardViewport, dither));
  require(first.succeeded(), "forward frame preparation failed");
  FakeCommandBuffer command;
  PassContext context{};
  context.frameIndex = 0u;
  context.commandBuffer = &command;
  pass.execute(context);
  require(command.passes[0].depth.clearValue.depth == 1.0f,
          "forward helper clear differs");
  for (std::size_t index = 2u; index < command.encoder.draws.size(); ++index) {
    const auto &record =
        device.pipelines[command.encoder.draws[index].pipeline.index];
    const bool plane = index >= 5u && index <= 7u;
    require(record.variant == (plane ? 0x7613u : 0x7612u),
            "forward dither/plane pipeline differs");
  }
  const auto token = device.queue.issue();
  require(pass.resolveFrame(first.ticket, EditorOverlayFrameDisposition::submitted,
                            token)
              .succeeded(),
          "forward submission failed");

  // A different slot remains writable while slot zero is pending.
  const auto secondViewport = metrics(21u, false);
  const PacketFixture secondPackets = packets(secondViewport, true);
  const auto second =
      pass.prepareFrame(1u, frame(2u, secondViewport, secondPackets));
  require(second.succeeded(), "independent frame slot was blocked");
  require(pass.resolveFrame(second.ticket,
                            EditorOverlayFrameDisposition::notSubmitted)
              .succeeded(),
          "not-submitted frame did not return idle");
  device.queue.complete(token);
  require(pass.collectCompletedFrames() == 1u,
          "forward slot completion failed");
  require(pass.shutdown().succeeded(), "forward shutdown failed");
}

void testResizeSurfaceLossAndFailurePartitions() {
  FakeDevice device;
  EditorOverlayPass pass;
  initializePass(pass, device);
  auto viewport = metrics(30u);
  PacketFixture fixture = packets(viewport);
  auto value = frame(1u, viewport, fixture);
  auto prepared = pass.prepareFrame(0u, value);
  require(prepared.succeeded(), "resize baseline prepare failed");
  FakeCommandBuffer command;
  PassContext context{};
  context.frameIndex = 0u;
  context.commandBuffer = &command;
  pass.execute(context);
  require(pass.resolveFrame(prepared.ticket,
                            EditorOverlayFrameDisposition::notSubmitted)
              .succeeded(),
          "resize baseline abandon failed");
  const std::uint64_t helperBefore = pass.diagnostics().helperDepthCreations;
  const std::uint64_t pipelinesBefore = pass.diagnostics().pipelineCreations;

  // A viewport-only change retains helper depth and pipelines.
  viewport.viewportGeneration = 31u;
  viewport.projection = {32u, 33u};
  viewport.view = {34u, 35u};
  fixture = packets(viewport);
  value = frame(2u, viewport, fixture);
  prepared = pass.prepareFrame(0u, value);
  require(prepared.succeeded() &&
              pass.diagnostics().helperDepthCreations == helperBefore &&
              pass.diagnostics().pipelineCreations == pipelinesBefore,
          "viewport-only frame recreated surface resources");
  require(pass.releaseSurfaceResources().error.code ==
              rhi::RHIErrorCode::invalidState,
          "surface release ignored unresolved frame");
  require(pass.resolveFrame(prepared.ticket,
                            EditorOverlayFrameDisposition::notSubmitted)
              .succeeded(),
          "viewport-only abandon failed");

  // Full attachment resize replaces only helper depth.
  viewport.colorAttachment = {1280u, 720u};
  fixture = packets(viewport);
  value = frame(3u, viewport, fixture);
  prepared = pass.prepareFrame(0u, value);
  require(prepared.succeeded() &&
              pass.diagnostics().helperDepthCreations == helperBefore + 1u &&
              pass.diagnostics().pipelineCreations == pipelinesBefore &&
              pass.helperDepthBinding().extent.width == 1280u,
          "attachment resize did not replace only helper depth");
  require(pass.resolveFrame(prepared.ticket,
                            EditorOverlayFrameDisposition::notSubmitted)
              .succeeded(),
          "resize abandon failed");

  const auto helper = pass.helperDepthBinding();
  device.failNextTexture = true;
  viewport.colorAttachment = {1400u, 800u};
  fixture = packets(viewport);
  value = frame(4u, viewport, fixture);
  const auto failedCandidate = pass.prepareFrame(0u, value);
  require(!failedCandidate.succeeded() &&
              pass.helperDepthBinding().texture == helper.texture &&
              pass.helperDepthBinding().extent.width == helper.extent.width &&
              pass.helperDepthBinding().extent.height == helper.extent.height,
          "failed resize replaced the live helper");

  device.failNextView = true;
  viewport.colorAttachment = {1500u, 900u};
  fixture = packets(viewport);
  value = frame(4u, viewport, fixture);
  const auto failedViewCandidate = pass.prepareFrame(0u, value);
  require(!failedViewCandidate.succeeded() &&
              pass.helperDepthBinding().texture == helper.texture &&
              !device.textures.back().alive,
          "failed helper view did not preserve the live surface");

  viewport.colorAttachment = {helper.extent.width, helper.extent.height};
  fixture = packets(viewport);
  value = frame(4u, viewport, fixture);
  value.targets.outputFormat = rhi::TextureFormat::rgba8Unorm;
  const std::size_t livePipelines = device.livePipelineCount();
  device.failPipelineCreationAt = device.pipelinesCreated + 3u;
  const auto failedPipelineCandidate = pass.prepareFrame(0u, value);
  device.failPipelineCreationAt = 0u;
  require(!failedPipelineCandidate.succeeded() &&
              pass.helperDepthBinding().texture == helper.texture &&
              device.livePipelineCount() == livePipelines,
          "partial pipeline candidate replaced or leaked the live family");

  value.targets.outputFormat = targets().outputFormat;
  device.failNextMap = true;
  require(!pass.prepareFrame(0u, value).succeeded(),
          "injected uniform mapping failure passed");
  const std::uint64_t unmapsBeforeFlushFailure = device.unmapCalls;
  device.failNextFlush = true;
  require(!pass.prepareFrame(0u, value).succeeded() &&
              device.unmapCalls == unmapsBeforeFlushFailure + 1u,
          "uniform flush failure did not fail closed and unmap");

  // Descriptor failure poisons the table and the next call rebuilds it.
  device.failNextTableUpdate = true;
  const auto descriptorFailure = pass.prepareFrame(0u, value);
  require(!descriptorFailure.succeeded(),
          "injected descriptor failure passed");
  const std::uint64_t rebuilds = pass.diagnostics().frameTableRebuilds;
  const auto recovered = pass.prepareFrame(0u, value);
  require(recovered.succeeded() &&
              pass.diagnostics().frameTableRebuilds == rebuilds + 1u,
          "poisoned table was not rebuilt");

  FakeCommandBuffer noEncoder;
  noEncoder.returnNullEncoder = true;
  context.commandBuffer = &noEncoder;
  pass.execute(context);
  const auto invalidSubmission = pass.resolveFrame(
      recovered.ticket, EditorOverlayFrameDisposition::submitted, {});
  require(!invalidSubmission.succeeded(),
          "failed recording accepted an invalid submission token");
  require(pass.resolveFrame(recovered.ticket,
                            EditorOverlayFrameDisposition::notSubmitted)
              .succeeded(),
          "failed recording could not be abandoned");

  value.frameSerial = 5u;
  const auto submittedFailure = pass.prepareFrame(0u, value);
  require(submittedFailure.succeeded(),
          "post-submit recording-failure frame preparation failed");
  pass.execute(context);
  const rhi::SubmissionToken failedRecordingToken = device.queue.issue();
  require(!pass.resolveFrame(submittedFailure.ticket,
                             EditorOverlayFrameDisposition::notSubmitted)
               .succeeded(),
          "queue progress was incorrectly treated as an abandoned frame");
  require(pass.resolveFrame(submittedFailure.ticket,
                            EditorOverlayFrameDisposition::submitted,
                            failedRecordingToken)
              .succeeded() &&
              pass.diagnostics().lastError ==
                  EditorOverlayPassError::recordingFailure,
          "real submit did not retire a failed recording conservatively");
  value.frameSerial = 6u;
  require(pass.prepareFrame(0u, value).error ==
              EditorOverlayPassError::frameBusy,
          "failed recording slot was reused before its submit completed");
  device.queue.complete(failedRecordingToken);
  require(pass.collectCompletedFrames() == 1u,
          "failed recording submission did not complete");

  value.targets.sceneDepthSamples = rhi::SampleCount::count4;
  value.frameSerial = 6u;
  require(pass.prepareFrame(0u, value).error ==
              EditorOverlayPassError::invalidArgument,
          "multisampled scene depth was accepted by a Sampler2D shader");
  value.targets.sceneDepthSamples = rhi::SampleCount::count1;

  require(pass.releaseSurfaceResources().succeeded() &&
              !pass.helperDepthBinding().valid() &&
              device.livePipelineCount() == 0u,
          "surface loss retained active surface resources");
  const auto borrowedOutput = targets().outputTexture;
  const auto borrowedDepth = targets().sceneDepthTexture;
  require(std::none_of(device.destroyed.begin(), device.destroyed.end(),
                       [&](const rhi::ResourceHandle &resource) {
                         return resource.index == borrowedOutput.index ||
                                resource.index == borrowedDepth.index;
                       }),
          "pass destroyed a borrowed attachment");
  require(pass.shutdown().succeeded(), "failure-partition shutdown failed");
  require(device.waitIdleCount == 0u && device.queue.waitIdleCount == 0u,
          "resize/failure path waited for idle");
}

void testNoDrawStaleGenerationAndShutdownGuard() {
  FakeDevice device;
  EditorOverlayPass pass;
  initializePass(pass, device, 1u);
  const auto viewport = metrics(40u);
  PacketFixture fixture = packets(viewport);
  EditorOverlayPassFrame empty = frame(1u, viewport, fixture);
  empty.gridVisible = false;
  empty.grid = {};
  empty.gridLines = {};
  empty.gridAxes = {};
  empty.gizmoVisible = false;
  empty.gizmo = {};
  empty.targets = {};
  const auto prepared = pass.prepareFrame(0u, empty);
  require(prepared.succeeded() && !prepared.helperDepth.valid(),
          "empty overlay frame was rejected");
  FakeCommandBuffer command;
  PassContext context{};
  context.frameIndex = 0u;
  context.commandBuffer = &command;
  pass.execute(context);
  require(command.passes.empty(), "empty frame began a render pass");
  require(!pass.shutdown().succeeded(),
          "shutdown ignored an unresolved prepared frame");
  require(pass.resolveFrame(prepared.ticket,
                            EditorOverlayFrameDisposition::notSubmitted)
              .succeeded(),
          "empty frame abandon failed");

  PacketFixture stale = packets(viewport);
  ++stale.grid.generation.viewportGeneration;
  const auto rejected = pass.prepareFrame(0u, frame(2u, viewport, stale));
  require(!rejected.succeeded() &&
              rejected.error == EditorOverlayPassError::invalidArgument,
          "stale Grid generation was accepted");
  require(pass.shutdown().succeeded(), "guarded shutdown failed");
}

void testShaderSourceContract() {
  const std::filesystem::path source =
      std::filesystem::path(MGIF_TEST_SOURCE_DIR) / "shaders" /
      "editor_overlay.slang";
  std::ifstream stream(source, std::ios::binary);
  require(stream.good(), "overlay shader source is missing");
  const std::string text((std::istreambuf_iterator<char>(stream)),
                         std::istreambuf_iterator<char>());
  for (const std::string_view token : {
           "editorOverlayLineVertexMain", "editorOverlayGizmoVertexMain",
           "editorOverlayFragmentMain", "kEditorOverlayFlagPlaneChecker",
           "kEditorOverlayFlagOcclusionDither", "sceneDepth - epsilon",
           "sceneDepth + epsilon", "clipLineToViewVolume",
           "directionPx / framebufferScale",
           "normalLogical * framebufferScale", "sourceFramebufferScale.yx",
           "depthPixel / depthExtent"}) {
    require(text.find(token) != std::string::npos,
            "overlay shader contract token is missing");
  }
  for (const std::string_view forbidden : {"VkImage", "VkBuffer",
                                           "ID3D12", "MTLTexture",
                                           "waitIdle", "depthPixel + 0.5",
                                           "max(abs(firstClip.w)"}) {
    require(text.find(forbidden) == std::string::npos,
            "overlay shader exposes a forbidden native/wait token");
  }
}

} // namespace

int main() {
  try {
    testInitializationAbiAndDependencies();
    testReverseRecordingAndPipelineFamilies();
    testForwardDepthDitherAndFrameSlotReuse();
    testResizeSurfaceLossAndFailurePartitions();
    testNoDrawStaleGenerationAndShutdownGuard();
    testShaderSourceContract();
    std::cout << "Editor overlay RHI tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
}
