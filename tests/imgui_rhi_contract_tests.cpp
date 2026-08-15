#include "../render/ImGuiRhiTypes.h"
#include "../rhi/RHIEncoder.h"

#include <cassert>
#include <cstdint>

namespace
{
  void testTextureIdRoundTrip()
  {
    const demo::ui::TextureHandle handle{37u, 9u};
    const uint64_t encoded = demo::ui::encodeTextureId(handle);
    const demo::ui::TextureHandle decoded = demo::ui::decodeTextureId(encoded);

    assert(encoded != 0u);
    assert(decoded.index == handle.index);
    assert(decoded.generation == handle.generation);
    assert(demo::ui::decodeTextureId(0u).isNull());
  }

  void testProjectionUsesBackendClipSpaceConvention()
  {
    constexpr demo::ui::Float2 displayPos{100.0f, 50.0f};
    constexpr demo::ui::Float2 displaySize{800.0f, 600.0f};
    const demo::ui::Projection vulkanProjection = demo::ui::makeProjection(
      displayPos, displaySize, demo::rhi::BackendType::vulkan);

    assert(vulkanProjection.scaleX == 2.0f / displaySize.x);
    assert(vulkanProjection.scaleY == 2.0f / displaySize.y);
    assert(vulkanProjection.translateX == -1.0f - displayPos.x * vulkanProjection.scaleX);
    assert(vulkanProjection.translateY == -1.0f - displayPos.y * vulkanProjection.scaleY);

    const demo::ui::Projection metalProjection = demo::ui::makeProjection(
      displayPos, displaySize, demo::rhi::BackendType::metal);
    assert(metalProjection.scaleX == vulkanProjection.scaleX);
    assert(metalProjection.scaleY == -2.0f / displaySize.y);
    assert(metalProjection.translateX == vulkanProjection.translateX);
    assert(metalProjection.translateY == 1.0f - displayPos.y * metalProjection.scaleY);

    const demo::ui::Projection d3d12Projection = demo::ui::makeProjection(
      displayPos, displaySize, demo::rhi::BackendType::d3d12);
    assert(d3d12Projection.scaleY == metalProjection.scaleY);
    assert(d3d12Projection.translateY == metalProjection.translateY);
  }

  void testScissorTransformsAndClamps()
  {
    const auto scissor = demo::ui::makeScissor(
      {-20.0f, 10.0f, 90.0f, 80.0f},
      {10.0f, 5.0f},
      {2.0f, 2.0f},
      128u,
      96u);

    assert(scissor.has_value());
    assert(scissor->offset.x == 0);
    assert(scissor->offset.y == 10);
    assert(scissor->extent.width == 128u);
    assert(scissor->extent.height == 86u);
  }

  void testResizeUsesRenderTargetAsAuthoritativeFramebuffer()
  {
    constexpr demo::ui::Float2 staleDisplaySize{1280.0f, 720.0f};
    constexpr demo::rhi::Extent2D resizedTarget{640u, 360u};
    const demo::ui::Float2 framebufferScale =
      demo::ui::makeFramebufferScale(staleDisplaySize, resizedTarget);

    assert(framebufferScale.x == 0.5f);
    assert(framebufferScale.y == 0.5f);

    const auto scissor = demo::ui::makeScissor(
      {0.0f, 0.0f, staleDisplaySize.x, staleDisplaySize.y},
      {0.0f, 0.0f},
      framebufferScale,
      resizedTarget.width,
      resizedTarget.height);
    assert(scissor.has_value());
    assert(scissor->offset.x == 0);
    assert(scissor->offset.y == 0);
    assert(scissor->extent.width == resizedTarget.width);
    assert(scissor->extent.height == resizedTarget.height);
  }

  void testScissorRejectsEmptyRect()
  {
    const auto scissor = demo::ui::makeScissor(
      {20.0f, 20.0f, 10.0f, 30.0f},
      {0.0f, 0.0f},
      {1.0f, 1.0f},
      128u,
      96u);

    assert(!scissor.has_value());
  }

  void testBufferGrowthIsMonotonic()
  {
    assert(demo::ui::nextBufferCapacity(0u, 100u) >= 100u);
    assert(demo::ui::nextBufferCapacity(256u, 257u) >= 257u);
    assert(demo::ui::nextBufferCapacity(256u, 200u) == 256u);
  }

  void testTextureCopyCarriesDestinationOffset()
  {
    const demo::rhi::BufferTextureCopyDesc copy{
      .textureOffset = {4, 8, 0},
      .width = 16u,
      .height = 32u,
    };

    assert(copy.textureOffset.x == 4);
    assert(copy.textureOffset.y == 8);
  }
}

int main()
{
  static_assert(demo::rhi::VertexFormat::r8g8b8a8Unorm != demo::rhi::VertexFormat::undefined);

  testTextureIdRoundTrip();
  testProjectionUsesBackendClipSpaceConvention();
  testScissorTransformsAndClamps();
  testResizeUsesRenderTargetAsAuthoritativeFramebuffer();
  testScissorRejectsEmptyRect();
  testBufferGrowthIsMonotonic();
  testTextureCopyCarriesDestinationOffset();
  return 0;
}
