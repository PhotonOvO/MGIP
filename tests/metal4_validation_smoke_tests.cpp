#include "../rhi/RHICommandBuffer.h"
#include "../rhi/RHIDebugCounters.h"
#include "../rhi/metal/Metal4Availability.h"
#include "../rhi/metal/MetalCommon.h"
#include "../rhi/metal/MetalDevice.h"
#include "../rhi/metal/MetalResourceTable.h"

#include "_autogen/Metal4ValidationSmoke.metallib.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <sstream>
#include <span>
#include <stdexcept>
#include <vector>

#include <mach/mach_time.h>

struct GLFWwindow;
extern "C" __attribute__((weak)) void* glfwGetCocoaWindow(GLFWwindow*)
{
  return nullptr;
}

namespace {

using namespace demo::rhi;
using namespace demo::rhi::metal;

constexpr int kSkipReturnCode = 77;

void verifyBarrierVisibilityMapping()
{
  if(toMetalVisibility(HazardFlags::readBeforeWrite) != MTL4::VisibilityOptionNone)
    throw std::runtime_error("Metal4 WAR barriers must not flush GPU caches");
  if(toMetalVisibility(HazardFlags::textureWrites) != MTL4::VisibilityOptionDevice ||
     toMetalVisibility(HazardFlags::textureWrites | HazardFlags::readBeforeWrite) !=
       MTL4::VisibilityOptionDevice)
  {
    throw std::runtime_error("Metal4 write barriers must flush to device visibility");
  }
}

void enableMetalValidation()
{
  // Metal reads these settings while it creates the first device.
  const auto setDefault = [](const char* name, const char* value) {
    if(std::getenv(name) == nullptr)
      setenv(name, value, 0);
  };
  setDefault("MTL_DEBUG_LAYER", "1");
  setDefault("MTL_DEBUG_LAYER_ERROR_MODE", "assert");
  setDefault("MTL_SHADER_VALIDATION", "1");
  setDefault("MTL_SHADER_VALIDATION_DEFAULT_STATE", "all");
  setDefault("MTL_SHADER_VALIDATION_REPORT_TO_STDERR", "1");
}

ShaderLibraryHandle compileSmokeLibrary(MetalDevice& device)
{
  return device.createShaderLibrary(ShaderLibraryDesc{
    .format = ShaderIRFormat::metalLibrary,
    .data = std::as_bytes(std::span{mgip_metal4_validation_smoke_lib}),
    .debugName = "Metal4 validation smoke library",
  });
}

void verifyTimestampPeriod(const MetalDevice& device)
{
  mach_timebase_info_data_t timebase{};
  if(mach_timebase_info(&timebase) != KERN_SUCCESS || timebase.denom == 0)
    throw std::runtime_error("Metal4 timestamp smoke could not query the Mach timebase");

  const float expected = static_cast<float>(timebase.numer) /
                         static_cast<float>(timebase.denom);
  const float actual = device.getTimestampPeriodNs();
  if(!std::isfinite(actual) ||
     std::abs(actual - expected) > expected * 1.0e-6f)
  {
    throw std::runtime_error("Metal4 timestamp period does not use the Mach timebase");
  }
}

void runComputeBlitSmoke(MetalDevice& device)
{
  constexpr uint32_t sourceWidth = 128;
  constexpr uint32_t sourceHeight = 128;
  constexpr uint32_t mipWidth = 64;
  constexpr uint32_t mipHeight = 64;
  constexpr uint64_t sourceBytes = sourceWidth * sourceHeight * 4u;
  constexpr uint64_t mipBytes = mipWidth * mipHeight * 4u;

  const BufferHandle upload = device.createBuffer(BufferDesc{
    .size = sourceBytes,
    .usage = BufferUsageFlags::transferSrc,
    .memoryUsage = MemoryUsage::cpuToGpu,
    .debugName = "Metal4 blit smoke upload",
  });
  const BufferHandle readback = device.createBuffer(BufferDesc{
    .size = mipBytes,
    .usage = BufferUsageFlags::transferDst,
    .memoryUsage = MemoryUsage::gpuToCpu,
    .debugName = "Metal4 blit smoke readback",
  });
  const TextureHandle texture = device.createTexture(TextureDesc{
    .dimension = TextureDimension::e2D,
    .format = TextureFormat::rgba8Unorm,
    .usage = TextureUsageFlags::transferSrc | TextureUsageFlags::transferDst,
    .extent = {sourceWidth, sourceHeight, 1},
    .mipLevels = 2,
    .arrayLayers = 1,
    .sampleCount = SampleCount::count1,
    .memoryUsage = MemoryUsage::gpuOnly,
    .debugName = "Metal4 blit smoke texture",
  });

  auto* uploadBytes = static_cast<uint8_t*>(device.mapBuffer(upload));
  if(uploadBytes == nullptr)
    throw std::runtime_error("Metal4 blit smoke upload buffer is not mapped");
  for(uint32_t y = 0; y < sourceHeight; ++y)
  {
    for(uint32_t x = 0; x < sourceWidth; ++x)
    {
      const size_t offset = (static_cast<size_t>(y) * sourceWidth + x) * 4u;
      uploadBytes[offset + 0] = static_cast<uint8_t>(x);
      uploadBytes[offset + 1] = static_cast<uint8_t>(y);
      uploadBytes[offset + 2] = 0x5a;
      uploadBytes[offset + 3] = 0xff;
    }
  }
  device.unmapBuffer(upload);

  Queue* queue = device.getQueue(QueueClass::graphics);
  auto allocator = device.createCommandAllocator(QueueClass::graphics);
  auto commands = allocator->createCommandBuffer();
  commands->begin(*allocator);

  const TextureSubresourceRange mip0{
    .aspect = TextureAspect::color,
    .baseMipLevel = 0,
    .levelCount = 1,
    .baseArrayLayer = 0,
    .layerCount = 1,
  };
  const TextureSubresourceRange mip1{
    .aspect = TextureAspect::color,
    .baseMipLevel = 1,
    .levelCount = 1,
    .baseArrayLayer = 0,
    .layerCount = 1,
  };
  const TextureBarrier uploadBarrier{
    .texture = texture,
    .before = ResourceState::Undefined,
    .after = ResourceState::TransferDst,
    .range = mip0,
  };
  commands->resourceBarrier(std::span{&uploadBarrier, 1}, {});
  ComputeEncoder* encoder = commands->beginComputePass();
  encoder->copyBufferToTexture(BufferTextureCopyDesc{
    .buffer = upload,
    .texture = texture,
    .aspect = TextureAspect::color,
    .mipLevel = 0,
    .baseArrayLayer = 0,
    .layerCount = 1,
    .width = sourceWidth,
    .height = sourceHeight,
    .depth = 1,
  });
  commands->endEncoding();

  const std::array<TextureBarrier, 2> blitBarriers{{
    TextureBarrier{
      .texture = texture,
      .before = ResourceState::TransferDst,
      .after = ResourceState::TransferSrc,
      .range = mip0,
    },
    TextureBarrier{
      .texture = texture,
      .before = ResourceState::Undefined,
      .after = ResourceState::TransferDst,
      .range = mip1,
    },
  }};
  commands->resourceBarrier(blitBarriers, {});
  encoder = commands->beginComputePass();
  encoder->blitTexture(TextureBlitDesc{
    .srcTexture = texture,
    .dstTexture = texture,
    .aspect = TextureAspect::color,
    .srcMipLevel = 0,
    .dstMipLevel = 1,
    .srcBaseArrayLayer = 0,
    .dstBaseArrayLayer = 0,
    .layerCount = 1,
    .srcOffsets = {{static_cast<int32_t>(sourceWidth), 0, 0},
                   {0, static_cast<int32_t>(sourceHeight), 1}},
    .dstOffsets = {{0, 0, 0},
                   {static_cast<int32_t>(mipWidth),
                    static_cast<int32_t>(mipHeight), 1}},
  });
  commands->endEncoding();

  const TextureBarrier readbackBarrier{
    .texture = texture,
    .before = ResourceState::TransferDst,
    .after = ResourceState::TransferSrc,
    .range = mip1,
  };
  commands->resourceBarrier(std::span{&readbackBarrier, 1}, {});
  encoder = commands->beginComputePass();
  encoder->copyTextureToBuffer(BufferTextureCopyDesc{
    .buffer = readback,
    .texture = texture,
    .aspect = TextureAspect::color,
    .mipLevel = 1,
    .baseArrayLayer = 0,
    .layerCount = 1,
    .width = mipWidth,
    .height = mipHeight,
    .depth = 1,
  });
  commands->endEncoding();
  commands->end();

  CommandBuffer* submitted = commands.get();
  const SubmissionToken completion = queue->submit(SubmitBatch{
    .commandBuffers = std::span<CommandBuffer* const>{&submitted, 1},
    .debugLabel = "Metal4 validation compute blit smoke",
  });
  queue->wait(completion);

  const auto* result = static_cast<const uint8_t*>(device.mapBuffer(readback));
  if(result == nullptr || result[3] != 0xff || result[2] != 0x5a ||
     result[0] <= result[(mipWidth - 1u) * 4u])
  {
    std::ostringstream message;
    message << "Metal4 scaled/flipped mip blit produced invalid data";
    if(result != nullptr)
    {
      const size_t last = (mipWidth - 1u) * 4u;
      message << ": first rgba=(" << static_cast<uint32_t>(result[0]) << ','
              << static_cast<uint32_t>(result[1]) << ','
              << static_cast<uint32_t>(result[2]) << ','
              << static_cast<uint32_t>(result[3]) << "), last rgba=("
              << static_cast<uint32_t>(result[last]) << ','
              << static_cast<uint32_t>(result[last + 1]) << ','
              << static_cast<uint32_t>(result[last + 2]) << ','
              << static_cast<uint32_t>(result[last + 3]) << ')';
    }
    throw std::runtime_error(message.str());
  }
  device.unmapBuffer(readback);
  allocator->reset(completion);
  commands.reset();
  allocator.reset();
  device.destroyTexture(texture);
  device.destroyBuffer(readback);
  device.destroyBuffer(upload);
}

void runExecutableArgumentTableUpdateSmoke(MetalDevice& device)
{
  const ShaderLibraryHandle library = compileSmokeLibrary(device);
  const std::array<ArgumentBinding, 2> bindings{{
    ArgumentBinding{
      .binding = 0,
      .type = ArgumentType::storageBuffer,
      .visibility = ShaderStage::compute,
    },
    ArgumentBinding{
      .binding = 1,
      .type = ArgumentType::storageBuffer,
      .visibility = ShaderStage::compute,
    },
  }};
  const ArgumentLayoutHandle layout = device.createArgumentLayout(
    ArgumentLayoutDesc{.bindings = bindings,
                       .debugName = "Metal4 executable table smoke layout"});
  const ArgumentTableHandle table = device.createArgumentTable(
    ArgumentTableCreateDesc{
      .layout = layout,
      .lifetime = ArgumentTableLifetime::persistent,
      .debugName = "Metal4 executable table smoke table",
    });
  const PipelineArgumentSlotDesc tableSlot{
    .slot = 0,
    .layout = layout,
    .visibility = ShaderStage::compute,
  };
  const PipelineHandle pipeline = device.createComputePipeline(ComputePipelineDesc{
    .shaderStage = ShaderEntry{
      .stage = ShaderStage::compute,
      .library = library,
      .entryPoint = "argumentTableSnapshotSmoke",
    },
    .bindingSchema = PipelineBindingSchemaDesc{
      .argumentSlots = std::span{&tableSlot, 1},
    },
    .threadgroupSize = {1, 1, 1},
  });

  const auto createValueBuffer = [&device](uint32_t value, const char* name) {
    const BufferHandle buffer = device.createBuffer(BufferDesc{
      .size = sizeof(uint32_t),
      .usage = BufferUsageFlags::storage,
      .memoryUsage = MemoryUsage::cpuToGpu,
      .allowGpuAddress = true,
      .allowArgumentTableBinding = true,
      .debugName = name,
    });
    auto* mapped = static_cast<uint32_t*>(device.mapBuffer(buffer));
    if(mapped == nullptr)
      throw std::runtime_error("Metal4 argument-table source buffer is not mapped");
    *mapped = value;
    device.unmapBuffer(buffer);
    return buffer;
  };
  const BufferHandle oldSource = createValueBuffer(0x11111111u,
                                                    "Metal4 old table source");
  const BufferHandle replacementSource = createValueBuffer(
    0x2468ace0u, "Metal4 replacement table source");
  const BufferHandle reusedSource = createValueBuffer(
    0x13579bdfu, "Metal4 reused table source");
  const BufferHandle output = device.createBuffer(BufferDesc{
    .size = sizeof(uint32_t),
    .usage = BufferUsageFlags::storage,
    .memoryUsage = MemoryUsage::gpuToCpu,
    .allowGpuAddress = true,
    .allowArgumentTableBinding = true,
    .debugName = "Metal4 table smoke output",
  });
  std::array<ArgumentWrite, 2> writes{{
    ArgumentWrite{
      .binding = 0,
      .type = ArgumentType::storageBuffer,
      .buffer = oldSource,
      .size = sizeof(uint32_t),
      .accessIntent = ArgumentAccessIntent::sampledRead,
    },
    ArgumentWrite{
      .binding = 1,
      .type = ArgumentType::storageBuffer,
      .buffer = output,
      .size = sizeof(uint32_t),
      .accessIntent = ArgumentAccessIntent::readWrite,
    },
  }};
  resetHotPathCounters(BackendType::metal);
  device.updateArgumentTable(table, writes);
  const auto tableVersion = [&device, table]() {
    const MetalArgumentTableRecord* record =
      device.resourceTable().tryGetArgumentTable(table);
    if(record == nullptr)
      throw std::runtime_error("Metal4 argument-table record is unavailable");
    return record->version;
  };
  const uint64_t initialVersion = tableVersion();
  if(snapshotHotPathCounters(BackendType::metal).descriptorUpdates != 2u)
    throw std::runtime_error("Metal4 initial argument-table writes were not counted");
  device.updateArgumentTable(table, writes);
  if(tableVersion() != initialVersion ||
     snapshotHotPathCounters(BackendType::metal).descriptorUpdates != 2u)
  {
    throw std::runtime_error(
      "Metal4 duplicate argument-table writes changed the table version");
  }

  Queue* queue = device.getQueue(QueueClass::graphics);
  auto allocator = device.createCommandAllocator(QueueClass::graphics);
  auto commands = allocator->createCommandBuffer();
  commands->begin(*allocator);
  ComputeEncoder* encoder = commands->beginComputePass();
  encoder->setPipeline(pipeline);
  encoder->setArgumentTable(0, table);
  encoder->dispatch(DispatchDesc{.groupCountX = 1, .groupCountY = 1,
                                 .groupCountZ = 1});
  commands->endEncoding();
  commands->end();

  writes[0].buffer = replacementSource;
  device.updateArgumentTable(table, writes);
  if(tableVersion() != initialVersion + 1u ||
     snapshotHotPathCounters(BackendType::metal).descriptorUpdates != 3u)
  {
    throw std::runtime_error(
      "Metal4 argument-table updates did not isolate the changed binding");
  }
  device.destroyBuffer(oldSource);

  CommandBuffer* submitted = commands.get();
  const SubmissionToken completion = queue->submit(SubmitBatch{
    .commandBuffers = std::span<CommandBuffer* const>{&submitted, 1},
    .debugLabel = "Metal4 executable argument-table update smoke",
  });
  queue->wait(completion);
  const auto* result = static_cast<const uint32_t*>(device.mapBuffer(output));
  if(result == nullptr || *result != 0x2468ace0u)
    throw std::runtime_error("Metal4 executable argument-table update used stale data");
  device.unmapBuffer(output);
  const uint64_t allocatedSnapshots =
    snapshotHotPathCounters(BackendType::metal).tableVersionAllocations;
  if(allocatedSnapshots != 2u)
    throw std::runtime_error("Metal4 executable table replay allocation count changed");
  allocator->reset(completion);

  writes[0].buffer = reusedSource;
  device.updateArgumentTable(table, writes);
  commands->begin(*allocator);
  encoder = commands->beginComputePass();
  encoder->setPipeline(pipeline);
  encoder->setArgumentTable(0, table);
  encoder->dispatch(DispatchDesc{.groupCountX = 1, .groupCountY = 1,
                                 .groupCountZ = 1});
  commands->endEncoding();
  commands->end();
  if(snapshotHotPathCounters(BackendType::metal).tableVersionAllocations !=
     allocatedSnapshots)
  {
    throw std::runtime_error(
      "Metal4 allocator reset did not reuse an argument-table snapshot");
  }

  submitted = commands.get();
  const SubmissionToken reusedCompletion = queue->submit(SubmitBatch{
    .commandBuffers = std::span<CommandBuffer* const>{&submitted, 1},
    .debugLabel = "Metal4 reused argument-table snapshot smoke",
  });
  queue->wait(reusedCompletion);
  result = static_cast<const uint32_t*>(device.mapBuffer(output));
  if(result == nullptr || *result != 0x13579bdfu)
    throw std::runtime_error("Metal4 reused argument-table snapshot used stale data");
  device.unmapBuffer(output);
  allocator->reset(reusedCompletion);
  commands.reset();
  allocator.reset();

  device.destroyBuffer(output);
  device.destroyBuffer(reusedSource);
  device.destroyBuffer(replacementSource);
  device.destroyPipeline(pipeline);
  device.destroyArgumentTable(table);
  device.destroyArgumentLayout(layout);
  device.destroyShaderLibrary(library);
}

void runOffscreenRenderSmoke(MetalDevice& device)
{
  constexpr uint32_t width = 64;
  constexpr uint32_t height = 64;
  constexpr uint64_t readbackBytes = width * height * 4u;
  constexpr uint32_t expectedColor = 0xff3366ccu;

  const ShaderLibraryHandle library = compileSmokeLibrary(device);
  const ArgumentBinding colorBinding{
    .binding = 0,
    .type = ArgumentType::storageBuffer,
    .visibility = ShaderStage::fragment,
  };
  const ArgumentLayoutHandle layout = device.createArgumentLayout(
    ArgumentLayoutDesc{.bindings = std::span{&colorBinding, 1},
                       .debugName = "Metal4 offscreen color layout"});
  const ArgumentTableHandle table = device.createArgumentTable(
    ArgumentTableCreateDesc{
      .layout = layout,
      .lifetime = ArgumentTableLifetime::persistent,
      .debugName = "Metal4 offscreen color table",
    });
  const PipelineArgumentSlotDesc tableSlot{
    .slot = 0,
    .layout = layout,
    .visibility = ShaderStage::fragment,
  };
  const std::array<ShaderEntry, 2> stages{{
    ShaderEntry{
      .stage = ShaderStage::vertex,
      .library = library,
      .entryPoint = "offscreenSmokeVertex",
    },
    ShaderEntry{
      .stage = ShaderStage::fragment,
      .library = library,
      .entryPoint = "offscreenSmokeFragment",
    },
  }};
  const TextureFormat colorFormat = TextureFormat::rgba8Unorm;
  const PipelineHandle pipeline = device.createGraphicsPipeline(
    GraphicsPipelineDesc{
      .shaderStages = stages,
      .rasterState = {
        .topology = PrimitiveTopology::triangleList,
        .polygonMode = PolygonMode::fill,
        .cullMode = CullMode::none,
        .sampleCount = SampleCount::count1,
      },
      .renderingInfo = {
        .colorFormats = std::span{&colorFormat, 1},
      },
      .bindingSchema = {
        .argumentSlots = std::span{&tableSlot, 1},
      },
    });
  const auto createColorBuffer = [&device](uint32_t color, const char* name) {
    const BufferHandle buffer = device.createBuffer(BufferDesc{
      .size = sizeof(uint32_t),
      .usage = BufferUsageFlags::storage,
      .memoryUsage = MemoryUsage::cpuToGpu,
      .allowGpuAddress = true,
      .allowArgumentTableBinding = true,
      .debugName = name,
    });
    auto* mapped = static_cast<uint32_t*>(device.mapBuffer(buffer));
    if(mapped == nullptr)
      throw std::runtime_error("Metal4 offscreen color buffer is not mapped");
    *mapped = color;
    device.unmapBuffer(buffer);
    return buffer;
  };
  const BufferHandle oldColor = createColorBuffer(
    0xff0000ffu, "Metal4 offscreen old color");
  const BufferHandle replacementColor = createColorBuffer(
    expectedColor, "Metal4 offscreen replacement color");
  const BufferHandle readback = device.createBuffer(BufferDesc{
    .size = readbackBytes,
    .usage = BufferUsageFlags::transferDst,
    .memoryUsage = MemoryUsage::gpuToCpu,
    .debugName = "Metal4 offscreen readback",
  });
  const TextureHandle target = device.createTexture(TextureDesc{
    .dimension = TextureDimension::e2D,
    .format = colorFormat,
    .usage = TextureUsageFlags::colorAttachment | TextureUsageFlags::transferSrc,
    .extent = {width, height, 1},
    .mipLevels = 1,
    .arrayLayers = 1,
    .sampleCount = SampleCount::count1,
    .memoryUsage = MemoryUsage::gpuOnly,
    .debugName = "Metal4 offscreen target",
  });

  ArgumentWrite colorWrite{
    .binding = 0,
    .type = ArgumentType::storageBuffer,
    .buffer = oldColor,
    .size = sizeof(uint32_t),
    .accessIntent = ArgumentAccessIntent::sampledRead,
  };
  device.updateArgumentTable(table, std::span{&colorWrite, 1});

  Queue* queue = device.getQueue(QueueClass::graphics);
  auto allocator = device.createCommandAllocator(QueueClass::graphics);
  auto commands = allocator->createCommandBuffer();
  commands->begin(*allocator);
  const TextureSubresourceRange colorRange{
    .aspect = TextureAspect::color,
    .baseMipLevel = 0,
    .levelCount = 1,
    .baseArrayLayer = 0,
    .layerCount = 1,
  };
  const TextureBarrier renderBarrier{
    .texture = target,
    .before = ResourceState::Undefined,
    .after = ResourceState::ColorAttachment,
    .range = colorRange,
  };
  commands->resourceBarrier(std::span{&renderBarrier, 1}, {});
  const RenderTargetDesc colorTarget{
    .texture = target,
    .state = ResourceState::ColorAttachment,
    .loadOp = LoadOp::clear,
    .storeOp = StoreOp::store,
    .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
  };
  RenderEncoder* render = commands->beginRenderPass(RenderPassDesc{
    .renderArea = {{0, 0}, {width, height}},
    .colorTargets = std::span{&colorTarget, 1},
  });
  render->setPipeline(pipeline);
  render->setArgumentTable(ShaderStage::fragment, 0, table);
  render->setViewport(Viewport{0.0f, 0.0f, static_cast<float>(width),
                               static_cast<float>(height), 0.0f, 1.0f});
  render->setScissor(Rect2D{{0, 0}, {width, height}});
  render->draw(DrawDesc{.vertexCount = 3});
  commands->endEncoding();

  const TextureBarrier readbackBarrier{
    .texture = target,
    .before = ResourceState::ColorAttachment,
    .after = ResourceState::TransferSrc,
    .range = colorRange,
  };
  commands->resourceBarrier(std::span{&readbackBarrier, 1}, {});
  ComputeEncoder* copy = commands->beginComputePass();
  copy->copyTextureToBuffer(BufferTextureCopyDesc{
    .buffer = readback,
    .texture = target,
    .aspect = TextureAspect::color,
    .width = width,
    .height = height,
    .depth = 1,
  });
  commands->endEncoding();
  commands->end();

  colorWrite.buffer = replacementColor;
  device.updateArgumentTable(table, std::span{&colorWrite, 1});
  device.destroyBuffer(oldColor);

  CommandBuffer* submitted = commands.get();
  const SubmissionToken completion = queue->submit(SubmitBatch{
    .commandBuffers = std::span<CommandBuffer* const>{&submitted, 1},
    .debugLabel = "Metal4 validation offscreen render smoke",
  });
  queue->wait(completion);

  const auto* pixels = static_cast<const uint8_t*>(device.mapBuffer(readback));
  const size_t center = (static_cast<size_t>(height / 2u) * width + width / 2u) * 4u;
  if(pixels == nullptr || pixels[center] != 0xccu ||
     pixels[center + 1] != 0x66u || pixels[center + 2] != 0x33u ||
     pixels[center + 3] != 0xffu)
  {
    std::ostringstream message;
    message << "Metal4 offscreen render produced invalid data";
    if(pixels != nullptr)
    {
      message << ": center rgba=(" << static_cast<uint32_t>(pixels[center])
              << ',' << static_cast<uint32_t>(pixels[center + 1]) << ','
              << static_cast<uint32_t>(pixels[center + 2]) << ','
              << static_cast<uint32_t>(pixels[center + 3]) << ')';
    }
    throw std::runtime_error(message.str());
  }
  device.unmapBuffer(readback);
  allocator->reset(completion);
  commands.reset();
  allocator.reset();

  device.destroyTexture(target);
  device.destroyBuffer(readback);
  device.destroyBuffer(replacementColor);
  device.destroyPipeline(pipeline);
  device.destroyArgumentTable(table);
  device.destroyArgumentLayout(layout);
  device.destroyShaderLibrary(library);
}

void runIndexedIndirectRenderSmoke(MetalDevice& device)
{
  constexpr uint32_t width = 96;
  constexpr uint32_t height = 32;
  constexpr uint32_t regionWidth = width / 3u;
  constexpr uint64_t readbackBytes = width * height * 4u;

  struct Vertex
  {
    float x;
    float y;
  };
  struct IndexedIndirectArguments
  {
    uint32_t indexCount;
    uint32_t instanceCount;
    uint32_t indexStart;
    int32_t baseVertex;
    uint32_t baseInstance;
  };
  static_assert(sizeof(IndexedIndirectArguments) == 20);

  const ShaderLibraryHandle library = compileSmokeLibrary(device);
  const VertexBindingDesc vertexBinding{
    .binding = 0,
    .stride = sizeof(Vertex),
  };
  const VertexAttributeDesc vertexAttribute{
    .location = 0,
    .binding = 0,
    .format = VertexFormat::r32g32Sfloat,
  };
  const std::array<ShaderEntry, 2> stages{{
    ShaderEntry{
      .stage = ShaderStage::vertex,
      .library = library,
      .entryPoint = "indexedSmokeVertex",
    },
    ShaderEntry{
      .stage = ShaderStage::fragment,
      .library = library,
      .entryPoint = "indexedSmokeFragment",
    },
  }};
  const TextureFormat colorFormat = TextureFormat::rgba8Unorm;
  const PipelineHandle pipeline = device.createGraphicsPipeline(
    GraphicsPipelineDesc{
      .shaderStages = stages,
      .vertexInput = {
        .bindings = std::span{&vertexBinding, 1},
        .attributes = std::span{&vertexAttribute, 1},
      },
      .rasterState = {
        .topology = PrimitiveTopology::triangleList,
        .polygonMode = PolygonMode::fill,
        .cullMode = CullMode::none,
        .sampleCount = SampleCount::count1,
      },
      .renderingInfo = {
        .colorFormats = std::span{&colorFormat, 1},
      },
      .supportsGpuGeneratedCommands = true,
    });
  const ArgumentBinding indirectBinding{
    .binding = 0,
    .type = ArgumentType::storageBuffer,
    .visibility = ShaderStage::compute,
  };
  const ArgumentLayoutHandle indirectLayout = device.createArgumentLayout(
    ArgumentLayoutDesc{
      .bindings = std::span{&indirectBinding, 1},
      .debugName = "Metal4 indexed smoke indirect layout",
    });
  const ArgumentTableHandle indirectTable = device.createArgumentTable(
    ArgumentTableCreateDesc{
      .layout = indirectLayout,
      .lifetime = ArgumentTableLifetime::persistent,
      .debugName = "Metal4 indexed smoke indirect table",
    });
  const PipelineArgumentSlotDesc indirectTableSlot{
    .slot = 0,
    .layout = indirectLayout,
    .visibility = ShaderStage::compute,
  };
  const PipelineHandle indirectWriter = device.createComputePipeline(
    ComputePipelineDesc{
      .shaderStage = ShaderEntry{
        .stage = ShaderStage::compute,
        .library = library,
        .entryPoint = "writeIndexedIndirectSmoke",
      },
      .bindingSchema = PipelineBindingSchemaDesc{
        .argumentSlots = std::span{&indirectTableSlot, 1},
      },
      .threadgroupSize = {1, 1, 1},
    });

  const BufferHandle vertices = device.createBuffer(BufferDesc{
    .size = sizeof(Vertex) * 5u,
    .usage = BufferUsageFlags::vertex,
    .memoryUsage = MemoryUsage::cpuToGpu,
    .allowGpuAddress = true,
    .debugName = "Metal4 indexed smoke vertices",
  });
  const BufferHandle indices = device.createBuffer(BufferDesc{
    .size = sizeof(uint32_t) * 5u,
    .usage = BufferUsageFlags::index,
    .memoryUsage = MemoryUsage::cpuToGpu,
    .allowGpuAddress = true,
    .debugName = "Metal4 indexed smoke indices",
  });
  const BufferHandle indirect = device.createBuffer(BufferDesc{
    .size = sizeof(IndexedIndirectArguments) * 3u,
    .usage = BufferUsageFlags::indirect,
    .memoryUsage = MemoryUsage::cpuToGpu,
    .allowGpuAddress = true,
    .allowIndirectArgument = true,
    .debugName = "Metal4 indexed smoke indirect arguments",
  });
  const BufferHandle count = device.createBuffer(BufferDesc{
    .size = sizeof(uint32_t),
    .usage = BufferUsageFlags::storage | BufferUsageFlags::indirect,
    .memoryUsage = MemoryUsage::cpuToGpu,
    .allowGpuAddress = true,
    .allowIndirectArgument = true,
    .debugName = "Metal4 indexed smoke draw count",
  });
  const BufferHandle readback = device.createBuffer(BufferDesc{
    .size = readbackBytes,
    .usage = BufferUsageFlags::transferDst,
    .memoryUsage = MemoryUsage::gpuToCpu,
    .debugName = "Metal4 indexed smoke readback",
  });
  const TextureHandle target = device.createTexture(TextureDesc{
    .dimension = TextureDimension::e2D,
    .format = colorFormat,
    .usage = TextureUsageFlags::colorAttachment | TextureUsageFlags::transferSrc,
    .extent = {width, height, 1},
    .mipLevels = 1,
    .arrayLayers = 1,
    .sampleCount = SampleCount::count1,
    .memoryUsage = MemoryUsage::gpuOnly,
    .debugName = "Metal4 indexed smoke target",
  });

  const std::array<Vertex, 5> vertexData{{
    {8.0f, 8.0f},
    {8.0f, 8.0f},
    {-1.0f, -1.0f},
    {3.0f, -1.0f},
    {-1.0f, 3.0f},
  }};
  const std::array<uint32_t, 5> indexData{{99u, 99u, 0u, 1u, 2u}};
  const std::array<IndexedIndirectArguments, 3> indirectData{{
    {},
    {},
    {},
  }};
  auto* mappedVertices = device.mapBuffer(vertices);
  auto* mappedIndices = device.mapBuffer(indices);
  auto* mappedIndirect = device.mapBuffer(indirect);
  auto* mappedCount = static_cast<uint32_t*>(device.mapBuffer(count));
  if(mappedVertices == nullptr || mappedIndices == nullptr ||
     mappedIndirect == nullptr || mappedCount == nullptr)
    throw std::runtime_error("Metal4 indexed smoke upload buffer is not mapped");
  std::memcpy(mappedVertices, vertexData.data(), sizeof(vertexData));
  std::memcpy(mappedIndices, indexData.data(), sizeof(indexData));
  std::memcpy(mappedIndirect, indirectData.data(), sizeof(indirectData));
  *mappedCount = 1u;
  device.unmapBuffer(vertices);
  device.unmapBuffer(indices);
  device.unmapBuffer(indirect);
  device.unmapBuffer(count);
  const ArgumentWrite indirectWrite{
    .binding = 0,
    .type = ArgumentType::storageBuffer,
    .buffer = indirect,
    .size = sizeof(indirectData),
    .accessIntent = ArgumentAccessIntent::readWrite,
  };
  device.updateArgumentTable(indirectTable, std::span{&indirectWrite, 1});

  Queue* queue = device.getQueue(QueueClass::graphics);
  auto allocator = device.createCommandAllocator(QueueClass::graphics);
  auto commands = allocator->createCommandBuffer();
  commands->begin(*allocator);
  const TextureSubresourceRange colorRange{
    .aspect = TextureAspect::color,
    .baseMipLevel = 0,
    .levelCount = 1,
    .baseArrayLayer = 0,
    .layerCount = 1,
  };
  const TextureBarrier renderBarrier{
    .texture = target,
    .before = ResourceState::Undefined,
    .after = ResourceState::ColorAttachment,
    .range = colorRange,
  };
  commands->resourceBarrier(std::span{&renderBarrier, 1}, {});
  ComputeEncoder* writer = commands->beginComputePass();
  writer->setPipeline(indirectWriter);
  writer->setArgumentTable(0, indirectTable);
  writer->dispatch(DispatchDesc{.groupCountX = 1, .groupCountY = 1,
                                .groupCountZ = 1});
  commands->endEncoding();
  commands->barrier(StageFlags::compute, StageFlags::commandInput,
                    HazardFlags::drawArguments | HazardFlags::bufferWrites);
  const DrawIndirectCountDesc countDraw{
    .argsBuffer = indirect,
    .argsOffset = sizeof(IndexedIndirectArguments) * 2u,
    .countBuffer = count,
    .countBufferOffset = 0,
    .maxDrawCount = 1,
    .stride = sizeof(IndexedIndirectArguments),
  };
  const DrawIndirectDesc fixedDraw{
    .argsBuffer = indirect,
    .offset = sizeof(IndexedIndirectArguments),
    .drawCount = 1,
    .stride = sizeof(IndexedIndirectArguments),
  };
  commands->prepareIndexedIndirect(PrepareIndexedIndirectDesc{
    .draw = fixedDraw,
    .indexBuffer = indices,
    .indexBufferOffset = sizeof(uint32_t),
    .indexFormat = IndexFormat::uint32,
    .topology = PrimitiveTopology::triangleList,
  });
  commands->prepareIndexedIndirectCount(PrepareIndexedIndirectCountDesc{
    .draw = countDraw,
    .indexBuffer = indices,
    .indexBufferOffset = sizeof(uint32_t),
    .indexFormat = IndexFormat::uint32,
    .topology = PrimitiveTopology::triangleList,
  });
  const RenderTargetDesc colorTarget{
    .texture = target,
    .state = ResourceState::ColorAttachment,
    .loadOp = LoadOp::clear,
    .storeOp = StoreOp::store,
    .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
  };
  RenderEncoder* render = commands->beginRenderPass(RenderPassDesc{
    .renderArea = {{0, 0}, {width, height}},
    .colorTargets = std::span{&colorTarget, 1},
  });
  render->setPipeline(pipeline);
  render->setScissor(Rect2D{{0, 0}, {width, height}});
  render->bindVertexBuffer(0, vertices, 0);
  render->bindIndexBuffer(indices, sizeof(uint32_t), IndexFormat::uint32);
  render->setViewport(Viewport{0.0f, 0.0f, static_cast<float>(regionWidth),
                               static_cast<float>(height), 0.0f, 1.0f});
  render->drawIndexed(DrawIndexedDesc{
    .indexCount = 3,
    .firstIndex = 1,
    .vertexOffset = 2,
    .firstInstance = 7,
  });
  render->setViewport(Viewport{static_cast<float>(regionWidth), 0.0f,
                               static_cast<float>(regionWidth),
                               static_cast<float>(height), 0.0f, 1.0f});
  render->drawIndexedIndirect(fixedDraw);
  render->setViewport(Viewport{static_cast<float>(regionWidth * 2u), 0.0f,
                               static_cast<float>(regionWidth),
                               static_cast<float>(height), 0.0f, 1.0f});
  render->drawIndexedIndirectCount(countDraw);
  commands->endEncoding();

  const TextureBarrier readbackBarrier{
    .texture = target,
    .before = ResourceState::ColorAttachment,
    .after = ResourceState::TransferSrc,
    .range = colorRange,
  };
  commands->resourceBarrier(std::span{&readbackBarrier, 1}, {});
  ComputeEncoder* copy = commands->beginComputePass();
  copy->copyTextureToBuffer(BufferTextureCopyDesc{
    .buffer = readback,
    .texture = target,
    .aspect = TextureAspect::color,
    .width = width,
    .height = height,
    .depth = 1,
  });
  commands->endEncoding();
  commands->end();

  CommandBuffer* submitted = commands.get();
  const SubmissionToken completion = queue->submit(SubmitBatch{
    .commandBuffers = std::span<CommandBuffer* const>{&submitted, 1},
    .debugLabel = "Metal4 indexed indirect render smoke",
  });
  queue->wait(completion);

  const auto* pixels = static_cast<const uint8_t*>(device.mapBuffer(readback));
  const auto regionIsGreen = [pixels, width, height](uint32_t x) {
    if(pixels == nullptr)
      return false;
    const size_t offset =
      (static_cast<size_t>(height / 2u) * width + x) * 4u;
    return pixels[offset] == 0u && pixels[offset + 1] == 0xffu &&
           pixels[offset + 2] == 0u && pixels[offset + 3] == 0xffu;
  };
  const bool directPassed = regionIsGreen(regionWidth / 2u);
  const bool indirectPassed = regionIsGreen(regionWidth + regionWidth / 2u);
  const bool countPassed = regionIsGreen(regionWidth * 2u + regionWidth / 2u);
  if(!directPassed || !indirectPassed || !countPassed)
  {
    std::ostringstream message;
    message << "Metal4 indexed render mismatch: direct="
            << (directPassed ? "pass" : "fail") << ", indirect="
            << (indirectPassed ? "pass" : "fail") << ", count="
            << (countPassed ? "pass" : "fail");
    throw std::runtime_error(message.str());
  }
  device.unmapBuffer(readback);
  allocator->reset(completion);
  commands.reset();
  allocator.reset();

  device.destroyTexture(target);
  device.destroyBuffer(readback);
  device.destroyBuffer(count);
  device.destroyBuffer(indirect);
  device.destroyBuffer(indices);
  device.destroyBuffer(vertices);
  device.destroyPipeline(indirectWriter);
  device.destroyArgumentTable(indirectTable);
  device.destroyArgumentLayout(indirectLayout);
  device.destroyPipeline(pipeline);
  device.destroyShaderLibrary(library);
}

void runIndirectTexturePipelineCompatibilitySmoke(MetalDevice& device)
{
  const ShaderLibraryHandle library = compileSmokeLibrary(device);
  const TextureFormat colorFormat = TextureFormat::rgba8Unorm;
  const auto createPipeline = [&](const char* fragmentEntry) {
    const std::array<ShaderEntry, 2> stages{{
      ShaderEntry{
        .stage = ShaderStage::vertex,
        .library = library,
        .entryPoint = "offscreenSmokeVertex",
      },
      ShaderEntry{
        .stage = ShaderStage::fragment,
        .library = library,
        .entryPoint = fragmentEntry,
      },
    }};
    return device.createGraphicsPipeline(GraphicsPipelineDesc{
      .shaderStages = stages,
      .rasterState = {
        .topology = PrimitiveTopology::triangleList,
        .polygonMode = PolygonMode::fill,
        .cullMode = CullMode::none,
        .sampleCount = SampleCount::count1,
      },
      .renderingInfo = {
        .colorFormats = std::span{&colorFormat, 1},
      },
      .supportsGpuGeneratedCommands = true,
    });
  };

  const PipelineHandle topLevel = createPipeline(
    "icbTopLevelTextureSmokeFragment");
  const PipelineHandle argumentBuffer = createPipeline(
    "icbArgumentBufferSmokeFragment");
  const MetalPipelineRecord* topLevelRecord =
    device.resourceTable().tryGetPipeline(topLevel);
  const MetalPipelineRecord* argumentBufferRecord =
    device.resourceTable().tryGetPipeline(argumentBuffer);
  if(topLevelRecord == nullptr || argumentBufferRecord == nullptr)
    throw std::runtime_error("Metal ICB texture PSO probe returned a stale pipeline");
  std::cout << "Metal4 ICB top-level texture PSO: "
            << (topLevelRecord->supportsGpuGeneratedCommands ? "supported" : "rejected")
            << std::endl;
  if(!argumentBufferRecord->supportsGpuGeneratedCommands)
  {
    throw std::runtime_error(
      "Metal ICB fragment ParameterBlock did not produce an ICB-compatible PSO");
  }

  device.destroyPipeline(argumentBuffer);
  device.destroyPipeline(topLevel);
  device.destroyShaderLibrary(library);
}

void runDepthAttachmentAndSamplingSmoke(MetalDevice& device)
{
  constexpr uint32_t width = 64;
  constexpr uint32_t height = 64;
  constexpr uint64_t readbackBytes = width * height * 4u;
  const TextureFormat colorFormat = TextureFormat::rgba8Unorm;
  const TextureFormat depthFormat = TextureFormat::d32Sfloat;

  const ShaderLibraryHandle library = compileSmokeLibrary(device);
  const ArgumentBinding depthBinding{
    .binding = 0,
    .type = ArgumentType::sampledTexture,
    .visibility = ShaderStage::fragment,
  };
  const ArgumentLayoutHandle depthLayout = device.createArgumentLayout(
    ArgumentLayoutDesc{
      .bindings = std::span{&depthBinding, 1},
      .debugName = "Metal4 depth sampling layout",
    });
  const ArgumentTableHandle depthTable = device.createArgumentTable(
    ArgumentTableCreateDesc{
      .layout = depthLayout,
      .lifetime = ArgumentTableLifetime::persistent,
      .debugName = "Metal4 depth sampling table",
    });
  const PipelineArgumentSlotDesc depthTableSlot{
    .slot = 0,
    .layout = depthLayout,
    .visibility = ShaderStage::fragment,
  };

  const auto createPipeline = [&](const char* vertexEntry,
                                  const char* fragmentEntry,
                                  bool depthTest,
                                  bool depthWrite,
                                  bool samplesDepth) {
    const std::array<ShaderEntry, 2> stages{{
      ShaderEntry{
        .stage = ShaderStage::vertex,
        .library = library,
        .entryPoint = vertexEntry,
      },
      ShaderEntry{
        .stage = ShaderStage::fragment,
        .library = library,
        .entryPoint = fragmentEntry,
      },
    }};
    return device.createGraphicsPipeline(GraphicsPipelineDesc{
      .shaderStages = stages,
      .rasterState = {
        .topology = PrimitiveTopology::triangleList,
        .polygonMode = PolygonMode::fill,
        .cullMode = CullMode::none,
        .sampleCount = SampleCount::count1,
      },
      .depthState = {
        .depthTestEnable = depthTest,
        .depthWriteEnable = depthWrite,
        .depthCompareOp = CompareOp::greater,
      },
      .renderingInfo = {
        .colorFormats = std::span{&colorFormat, 1},
        .depthFormat = depthTest ? depthFormat : TextureFormat::undefined,
      },
      .bindingSchema = samplesDepth
        ? PipelineBindingSchemaDesc{
            .argumentSlots = std::span{&depthTableSlot, 1},
          }
        : PipelineBindingSchemaDesc{},
    });
  };
  const PipelineHandle writePipeline = createPipeline(
    "depthWriteSmokeVertex", "depthWriteSmokeFragment", true, true, false);
  const PipelineHandle rejectPipeline = createPipeline(
    "depthRejectSmokeVertex", "depthRejectSmokeFragment", true, true, false);
  const PipelineHandle samplePipeline = createPipeline(
    "offscreenSmokeVertex", "depthSampleSmokeFragment", false, false, true);

  const TextureHandle colorTarget = device.createTexture(TextureDesc{
    .dimension = TextureDimension::e2D,
    .format = colorFormat,
    .usage = TextureUsageFlags::colorAttachment | TextureUsageFlags::transferSrc,
    .extent = {width, height, 1},
    .mipLevels = 1,
    .arrayLayers = 1,
    .sampleCount = SampleCount::count1,
    .memoryUsage = MemoryUsage::gpuOnly,
    .debugName = "Metal4 depth test color target",
  });
  const TextureHandle sampledTarget = device.createTexture(TextureDesc{
    .dimension = TextureDimension::e2D,
    .format = colorFormat,
    .usage = TextureUsageFlags::colorAttachment | TextureUsageFlags::transferSrc,
    .extent = {width, height, 1},
    .mipLevels = 1,
    .arrayLayers = 1,
    .sampleCount = SampleCount::count1,
    .memoryUsage = MemoryUsage::gpuOnly,
    .debugName = "Metal4 depth sample color target",
  });
  const TextureHandle depthTarget = device.createTexture(TextureDesc{
    .dimension = TextureDimension::e2D,
    .format = depthFormat,
    .usage = TextureUsageFlags::depthAttachment | TextureUsageFlags::sampled,
    .extent = {width, height, 1},
    .mipLevels = 1,
    .arrayLayers = 1,
    .sampleCount = SampleCount::count1,
    .memoryUsage = MemoryUsage::gpuOnly,
    .debugName = "Metal4 sampled depth target",
  });
  const TextureViewHandle depthView = device.createTextureView(
    TextureViewCreateDesc{
      .image = depthTarget,
      .format = depthFormat,
      .viewType = ImageViewType::e2D,
      .aspect = TextureAspect::depth,
      .debugName = "Metal4 sampled depth view",
    });
  const BufferHandle depthTestReadback = device.createBuffer(BufferDesc{
    .size = readbackBytes,
    .usage = BufferUsageFlags::transferDst,
    .memoryUsage = MemoryUsage::gpuToCpu,
    .debugName = "Metal4 depth test readback",
  });
  const BufferHandle depthSampleReadback = device.createBuffer(BufferDesc{
    .size = readbackBytes,
    .usage = BufferUsageFlags::transferDst,
    .memoryUsage = MemoryUsage::gpuToCpu,
    .debugName = "Metal4 depth sample readback",
  });
  device.updateArgumentTable(depthTable, std::array{
    ArgumentWrite{
      .binding = 0,
      .type = ArgumentType::sampledTexture,
      .textureView = depthView,
    },
  });

  Queue* queue = device.getQueue(QueueClass::graphics);
  auto allocator = device.createCommandAllocator(QueueClass::graphics);
  auto commands = allocator->createCommandBuffer();
  commands->begin(*allocator);
  const TextureSubresourceRange colorRange{
    .aspect = TextureAspect::color,
    .baseMipLevel = 0,
    .levelCount = 1,
    .baseArrayLayer = 0,
    .layerCount = 1,
  };
  const TextureSubresourceRange depthRange{
    .aspect = TextureAspect::depth,
    .baseMipLevel = 0,
    .levelCount = 1,
    .baseArrayLayer = 0,
    .layerCount = 1,
  };
  const std::array<TextureBarrier, 3> initialBarriers{{
    TextureBarrier{
      .texture = colorTarget,
      .before = ResourceState::Undefined,
      .after = ResourceState::ColorAttachment,
      .range = colorRange,
    },
    TextureBarrier{
      .texture = sampledTarget,
      .before = ResourceState::Undefined,
      .after = ResourceState::ColorAttachment,
      .range = colorRange,
    },
    TextureBarrier{
      .texture = depthTarget,
      .before = ResourceState::Undefined,
      .after = ResourceState::DepthStencilAttachment,
      .range = depthRange,
    },
  }};
  commands->resourceBarrier(initialBarriers, {});

  const auto drawDepthPass = [&](PipelineHandle pipeline, LoadOp colorLoad,
                                 LoadOp depthLoad) {
    const RenderTargetDesc colorAttachment{
      .texture = colorTarget,
      .state = ResourceState::ColorAttachment,
      .loadOp = colorLoad,
      .storeOp = StoreOp::store,
      .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
    };
    const DepthTargetDesc depthAttachment{
      .texture = depthTarget,
      .state = ResourceState::DepthStencilAttachment,
      .loadOp = depthLoad,
      .storeOp = StoreOp::store,
      .clearValue = {0.0f, 0},
    };
    RenderEncoder* render = commands->beginRenderPass(RenderPassDesc{
      .renderArea = {{0, 0}, {width, height}},
      .colorTargets = std::span{&colorAttachment, 1},
      .depthTarget = &depthAttachment,
    });
    render->setPipeline(pipeline);
    render->setViewport(Viewport{0.0f, 0.0f, static_cast<float>(width),
                                 static_cast<float>(height), 0.0f, 1.0f});
    render->setScissor(Rect2D{{0, 0}, {width, height}});
    render->draw(DrawDesc{.vertexCount = 3});
    commands->endEncoding();
  };
  drawDepthPass(writePipeline, LoadOp::clear, LoadOp::clear);
  drawDepthPass(rejectPipeline, LoadOp::load, LoadOp::load);

  const TextureBarrier depthReadBarrier{
    .texture = depthTarget,
    .before = ResourceState::DepthStencilAttachment,
    .after = ResourceState::ShaderRead,
    .range = depthRange,
  };
  commands->resourceBarrier(std::span{&depthReadBarrier, 1}, {});
  const RenderTargetDesc sampleAttachment{
    .texture = sampledTarget,
    .state = ResourceState::ColorAttachment,
    .loadOp = LoadOp::clear,
    .storeOp = StoreOp::store,
    .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
  };
  RenderEncoder* sampleRender = commands->beginRenderPass(RenderPassDesc{
    .renderArea = {{0, 0}, {width, height}},
    .colorTargets = std::span{&sampleAttachment, 1},
  });
  sampleRender->setPipeline(samplePipeline);
  sampleRender->setArgumentTable(ShaderStage::fragment, 0, depthTable);
  sampleRender->setViewport(Viewport{0.0f, 0.0f, static_cast<float>(width),
                                     static_cast<float>(height), 0.0f, 1.0f});
  sampleRender->setScissor(Rect2D{{0, 0}, {width, height}});
  sampleRender->draw(DrawDesc{.vertexCount = 3});
  commands->endEncoding();

  const std::array<TextureBarrier, 2> readbackBarriers{{
    TextureBarrier{
      .texture = colorTarget,
      .before = ResourceState::ColorAttachment,
      .after = ResourceState::TransferSrc,
      .range = colorRange,
    },
    TextureBarrier{
      .texture = sampledTarget,
      .before = ResourceState::ColorAttachment,
      .after = ResourceState::TransferSrc,
      .range = colorRange,
    },
  }};
  commands->resourceBarrier(readbackBarriers, {});
  ComputeEncoder* copy = commands->beginComputePass();
  copy->copyTextureToBuffer(BufferTextureCopyDesc{
    .buffer = depthTestReadback,
    .texture = colorTarget,
    .aspect = TextureAspect::color,
    .width = width,
    .height = height,
    .depth = 1,
  });
  copy->copyTextureToBuffer(BufferTextureCopyDesc{
    .buffer = depthSampleReadback,
    .texture = sampledTarget,
    .aspect = TextureAspect::color,
    .width = width,
    .height = height,
    .depth = 1,
  });
  commands->endEncoding();
  commands->end();

  CommandBuffer* submitted = commands.get();
  const SubmissionToken completion = queue->submit(SubmitBatch{
    .commandBuffers = std::span<CommandBuffer* const>{&submitted, 1},
    .debugLabel = "Metal4 depth attachment and sampling smoke",
  });
  queue->wait(completion);

  const size_t center =
    (static_cast<size_t>(height / 2u) * width + width / 2u) * 4u;
  const auto* depthTestPixels =
    static_cast<const uint8_t*>(device.mapBuffer(depthTestReadback));
  if(depthTestPixels == nullptr || depthTestPixels[center] != 0xffu ||
     depthTestPixels[center + 1] != 0u || depthTestPixels[center + 2] != 0u ||
     depthTestPixels[center + 3] != 0xffu)
  {
    throw std::runtime_error(
      "Metal4 reverse-Z depth write/test did not preserve the nearer draw");
  }
  device.unmapBuffer(depthTestReadback);

  const auto* depthSamplePixels =
    static_cast<const uint8_t*>(device.mapBuffer(depthSampleReadback));
  const uint8_t sampledDepth =
    depthSamplePixels != nullptr ? depthSamplePixels[center] : 0u;
  if(depthSamplePixels == nullptr || sampledDepth < 126u || sampledDepth > 129u ||
     depthSamplePixels[center + 1] != sampledDepth ||
     depthSamplePixels[center + 2] != sampledDepth ||
     depthSamplePixels[center + 3] != 0xffu)
  {
    std::ostringstream message;
    message << "Metal4 depth sampling produced invalid data";
    if(depthSamplePixels != nullptr)
      message << ": center depth byte=" << static_cast<uint32_t>(sampledDepth);
    throw std::runtime_error(message.str());
  }
  device.unmapBuffer(depthSampleReadback);
  allocator->reset(completion);
  commands.reset();
  allocator.reset();

  device.destroyBuffer(depthSampleReadback);
  device.destroyBuffer(depthTestReadback);
  device.destroyTextureView(depthView);
  device.destroyTexture(depthTarget);
  device.destroyTexture(sampledTarget);
  device.destroyTexture(colorTarget);
  device.destroyPipeline(samplePipeline);
  device.destroyPipeline(rejectPipeline);
  device.destroyPipeline(writePipeline);
  device.destroyArgumentTable(depthTable);
  device.destroyArgumentLayout(depthLayout);
  device.destroyShaderLibrary(library);
}

}  // namespace

int main()
{
  enableMetalValidation();
  const Metal4Availability availability = queryMetal4Availability();
  if(!availability.sdkHasCoreApi ||
     !availability.runtimeHasCoreApi ||
     !availability.deviceAvailable)
  {
    std::cout << "Metal4 validation smoke skipped: macOS 26 Metal 4 is unavailable\n";
    return kSkipReturnCode;
  }

  try
  {
    verifyBarrierVisibilityMapping();
    MetalDevice device;
    DeviceCreateInfo createInfo{};
    createInfo.enableValidationLayers = true;
    device.init(createInfo);
    std::cout << "Metal4 validation smoke: timestamp" << std::endl;
    verifyTimestampPeriod(device);
    std::cout << "Metal4 validation smoke: compute blit" << std::endl;
    runComputeBlitSmoke(device);
    std::cout << "Metal4 validation smoke: executable argument table" << std::endl;
    runExecutableArgumentTableUpdateSmoke(device);
    std::cout << "Metal4 validation smoke: offscreen render" << std::endl;
    runOffscreenRenderSmoke(device);
    std::cout << "Metal4 validation smoke: indexed indirect" << std::endl;
    runIndexedIndirectRenderSmoke(device);
    std::cout << "Metal4 validation smoke: ICB texture compatibility" << std::endl;
    runIndirectTexturePipelineCompatibilitySmoke(device);
    std::cout << "Metal4 validation smoke: depth attachment" << std::endl;
    runDepthAttachmentAndSamplingSmoke(device);
    device.waitIdle();
    std::cout << "Metal4 validation smoke passed on " << device.getDeviceName()
              << '\n';
    device.deinit();
    return 0;
  }
  catch(const std::exception& error)
  {
    std::cerr << "Metal4 validation smoke failed: " << error.what() << '\n';
    return 1;
  }
}
