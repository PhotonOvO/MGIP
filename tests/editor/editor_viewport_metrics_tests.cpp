#include "render/editor/EditorViewportMetrics.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <string_view>
#include <type_traits>

namespace {

using namespace demo::editor;

static_assert(std::is_trivially_copyable_v<EditorViewportMetrics>);
static_assert(std::is_trivially_copyable_v<PhysicalViewportPixel>);

bool expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
  }
  return condition;
}

bool close(double left, double right, double epsilon = 1.0e-12) {
  return std::abs(left - right) <= epsilon;
}

EditorViewportMetrics metrics() {
  EditorViewportMetrics result;
  result.logical = {{10.0, 20.0}, {200.0, 100.0}};
  result.framebufferLogicalOrigin = {0.0, 0.0};
  result.physical = {{12u, 25u}, {251u, 125u}};
  result.depthPhysical = result.physical;
  result.colorAttachment = {400u, 300u};
  result.depthAttachment = result.colorAttachment;
  result.framebufferScale = {1.25, 1.25};
  result.viewportGeneration = 9u;
  result.projection = {12u, 0x1234u};
  result.view = {17u, 0x5678u};
  result.convention = {EditorClipDepthRange::zeroToOne, true, true};
  result.projectionKind = EditorProjectionKind::perspective;
  result.verticalFieldOfViewRadians = 1.0;
  return result;
}

bool testHalfOpenMappingAndDpiBoundSubrect() {
  const EditorViewportMetrics value = metrics();
  bool passed = expect(value.valid(), "baseline viewport metrics are valid");
  passed &= expect(
      value.logicalToPhysical(value.logical.origin) ==
          PhysicalViewportPixel{12u, 25u, 0u, 0u},
      "logical minimum maps to floor-scaled physical subrect origin");

  const double right =
      value.logical.origin.x + value.logical.extent.width;
  const double bottom =
      value.logical.origin.y + value.logical.extent.height;
  const LogicalViewportPoint last{
      std::nextafter(right, -std::numeric_limits<double>::infinity()),
      std::nextafter(bottom, -std::numeric_limits<double>::infinity())};
  passed &= expect(
      value.logicalToPhysical(last) ==
          PhysicalViewportPixel{262u, 149u, 250u, 124u},
      "last half-open point maps to the last DPI-rounded physical pixel");
  passed &= expect(
      !value.logicalToPhysical({right, value.logical.origin.y}) &&
          !value.logicalToPhysical({value.logical.origin.x, bottom}),
      "right and bottom logical boundaries are excluded");
  passed &= expect(
      value.logicalToPhysical({110.0, 70.0}) ==
          PhysicalViewportPixel{137u, 87u, 125u, 62u},
      "logical point is normalized once after validated DPI rounding");

  EditorViewportMetrics mismatch = value;
  mismatch.physical.extent.width += 1u;
  passed &= expect(!mismatch.valid(),
                   "arbitrary full-attachment extent cannot masquerade as dock DPI mapping");
  mismatch = value;
  mismatch.physical.origin.x += 1u;
  passed &= expect(!mismatch.valid(),
                   "physical origin must match framebuffer logical origin and scale");
  return passed;
}

bool testDpiAndNonUniformScalePartitions() {
  const std::array<double, 4> scales{1.0, 1.25, 1.5, 2.0};
  const std::array<PhysicalViewportExtent, 4> extents{
      PhysicalViewportExtent{101u, 51u},
      PhysicalViewportExtent{127u, 64u},
      PhysicalViewportExtent{152u, 77u},
      PhysicalViewportExtent{202u, 102u}};
  bool passed = true;
  for (std::size_t index = 0; index < scales.size(); ++index) {
    EditorViewportMetrics value = metrics();
    value.logical = {{0.0, 0.0}, {101.0, 51.0}};
    value.physical = {{0u, 0u}, extents[index]};
    value.depthPhysical = value.physical;
    value.colorAttachment = extents[index];
    value.depthAttachment = extents[index];
    value.framebufferScale = {scales[index], scales[index]};
    const std::optional<PhysicalViewportPixel> center =
        value.logicalToPhysical({50.5, 25.5});
    passed &= expect(
        value.valid() && center &&
            center->localX == extents[index].width / 2u &&
            center->localY == extents[index].height / 2u,
        "100/125/150/200 percent DPI consumes scale exactly once");
  }

  EditorViewportMetrics non_uniform = metrics();
  non_uniform.logical = {{0.0, 0.0}, {101.0, 51.0}};
  non_uniform.framebufferScale = {1.25, 2.0};
  non_uniform.physical = {{0u, 0u}, {127u, 102u}};
  non_uniform.depthPhysical = non_uniform.physical;
  non_uniform.colorAttachment = {127u, 102u};
  non_uniform.depthAttachment = non_uniform.colorAttachment;
  passed &= expect(non_uniform.valid(),
                   "non-uniform X/Y framebuffer scale has exact per-axis contract");
  non_uniform.physical.extent.height = 103u;
  passed &= expect(!non_uniform.valid(),
                   "per-axis scale/extent mismatch fails closed");
  return passed;
}

bool testSurfaceTransforms() {
  struct Case final {
    EditorViewportSurfaceTransform transform;
    PhysicalViewportExtent attachment;
    PhysicalViewportRect expectedRect;
    PhysicalViewportPixel expectedMinimum;
  };
  const std::array<Case, 8> cases{
      Case{EditorViewportSurfaceTransform::identity,
           {200u, 100u}, {{10u, 20u}, {100u, 50u}},
           {10u, 20u, 0u, 0u}},
      Case{EditorViewportSurfaceTransform::rotate90,
           {100u, 200u}, {{30u, 10u}, {50u, 100u}},
           {79u, 10u, 49u, 0u}},
      Case{EditorViewportSurfaceTransform::rotate180,
           {200u, 100u}, {{90u, 30u}, {100u, 50u}},
           {189u, 79u, 99u, 49u}},
      Case{EditorViewportSurfaceTransform::rotate270,
           {100u, 200u}, {{20u, 90u}, {50u, 100u}},
           {20u, 189u, 0u, 99u}},
      Case{EditorViewportSurfaceTransform::mirrorHorizontal,
           {200u, 100u}, {{90u, 20u}, {100u, 50u}},
           {189u, 20u, 99u, 0u}},
      Case{EditorViewportSurfaceTransform::mirrorVertical,
           {200u, 100u}, {{10u, 30u}, {100u, 50u}},
           {10u, 79u, 0u, 49u}},
      Case{EditorViewportSurfaceTransform::mirrorHorizontalRotate90,
           {100u, 200u}, {{30u, 90u}, {50u, 100u}},
           {79u, 189u, 49u, 99u}},
      Case{EditorViewportSurfaceTransform::mirrorHorizontalRotate270,
           {100u, 200u}, {{20u, 10u}, {50u, 100u}},
           {20u, 10u, 0u, 0u}}};

  bool passed = true;
  for (const Case &item : cases) {
    EditorViewportMetrics value = metrics();
    value.logical = {{10.0, 20.0}, {100.0, 50.0}};
    value.framebufferScale = {1.0, 1.0};
    value.surfaceTransform = item.transform;
    value.colorAttachment = item.attachment;
    value.depthAttachment = item.attachment;
    value.physical = item.expectedRect;
    value.depthPhysical = item.expectedRect;
    passed &= expect(
        value.valid() &&
            value.logicalToPhysical(value.logical.origin) ==
                item.expectedMinimum,
        "backend-neutral surface transform maps rect and local pixel exactly");
  }
  EditorViewportMetrics unequal_quarter_turn = metrics();
  unequal_quarter_turn.logical = {{10.0, 20.0}, {100.0, 50.0}};
  unequal_quarter_turn.framebufferScale = {1.0, 1.0};
  unequal_quarter_turn.surfaceTransform =
      EditorViewportSurfaceTransform::rotate90;
  unequal_quarter_turn.colorAttachment = {100u, 200u};
  unequal_quarter_turn.physical = {{30u, 10u}, {50u, 100u}};
  unequal_quarter_turn.depthAttachment = {40u, 60u};
  unequal_quarter_turn.depthPhysical = {{2u, 4u}, {25u, 50u}};
  passed &= expect(
      unequal_quarter_turn.valid() &&
          unequal_quarter_turn.logicalToDepthPhysical(
              unequal_quarter_turn.logical.origin) ==
              PhysicalViewportPixel{26u, 4u, 24u, 0u},
      "quarter-turn keeps explicit unequal depth mapping in the same D4 orientation");
  EditorViewportMetrics unknown = metrics();
  unknown.surfaceTransform =
      static_cast<EditorViewportSurfaceTransform>(0xffu);
  passed &= expect(!unknown.valid(),
                   "unknown surface transform is never treated as identity");
  return passed;
}

bool testInvalidMetricsFailClosed() {
  bool passed = true;
  const auto rejected = [&passed](EditorViewportMetrics value,
                                  std::string_view label) {
    passed &= expect(!value.valid() &&
                         !value.logicalToPhysical(value.logical.origin),
                     label);
  };

  EditorViewportMetrics value = metrics();
  value.logical.extent.width = 0.0;
  rejected(value, "zero logical width rejects");
  value = metrics();
  value.logical.extent.height =
      std::numeric_limits<double>::quiet_NaN();
  rejected(value, "non-finite logical height rejects");
  value = metrics();
  value.logical.origin.x = std::numeric_limits<double>::infinity();
  rejected(value, "non-finite logical origin rejects");
  value = metrics();
  value.logical.origin.x = 1.0e300;
  value.logical.extent.width = 1.0;
  rejected(value, "finite precision collapse of origin plus extent rejects");
  value = metrics();
  value.physical.extent.width = 0u;
  rejected(value, "zero physical width rejects");
  value = metrics();
  value.framebufferScale.x = 0.0;
  rejected(value, "non-positive framebuffer scale rejects");
  value = metrics();
  value.framebufferScale.y =
      std::numeric_limits<double>::quiet_NaN();
  rejected(value, "non-finite framebuffer scale rejects");
  value = metrics();
  value.depthAttachment = {300u, 200u};
  value.depthPhysical = {{6u, 12u}, {126u, 63u}};
  passed &= expect(
      value.valid() &&
          value.logicalToDepthPhysical(value.logical.origin) ==
              PhysicalViewportPixel{6u, 12u, 0u, 0u} &&
          value.logicalToDepthPhysical({110.0, 70.0}) ==
              PhysicalViewportPixel{69u, 43u, 63u, 31u},
      "unequal color/depth targets carry an explicit normalized depth mapping");
  value.depthAttachment = {131u, 74u};
  rejected(value, "viewport outside the smaller depth target rejects");
  value = metrics();
  value.viewportGeneration = 0u;
  rejected(value, "zero viewport generation rejects");
  value = metrics();
  value.projection = {};
  rejected(value, "missing projection identity rejects");
  value = metrics();
  value.view = {};
  rejected(value, "missing view identity rejects");
  value = metrics();
  value.convention.depthRange = static_cast<EditorClipDepthRange>(0xffu);
  rejected(value, "unknown clip convention rejects");
  value = metrics();
  value.verticalFieldOfViewRadians = std::numbers::pi_v<double>;
  rejected(value, "degenerate perspective projection rejects");
  return passed;
}

bool testProjectionAwareConstantLogicalPixels() {
  EditorViewportMetrics one_x = metrics();
  one_x.logical = {{0.0, 0.0}, {800.0, 400.0}};
  one_x.physical = {{0u, 0u}, {800u, 400u}};
  one_x.depthPhysical = one_x.physical;
  one_x.colorAttachment = {800u, 400u};
  one_x.depthAttachment = one_x.colorAttachment;
  one_x.framebufferScale = {1.0, 1.0};

  EditorViewportMetrics two_x = one_x;
  two_x.framebufferScale = {2.0, 2.0};
  two_x.physical.extent = {1600u, 800u};
  two_x.depthPhysical = two_x.physical;
  two_x.colorAttachment = {1600u, 800u};
  two_x.depthAttachment = two_x.colorAttachment;
  two_x.viewportGeneration = 10u;

  const std::optional<ConstantLogicalPixelScale> scale_one =
      one_x.constantLogicalPixelScale(24.0, 5.0);
  const std::optional<ConstantLogicalPixelScale> scale_two =
      two_x.constantLogicalPixelScale(24.0, 5.0);
  bool passed = expect(
      scale_one && scale_two &&
          close(scale_one->worldUnitsPerLogicalPixel,
                scale_two->worldUnitsPerLogicalPixel) &&
          close(scale_one->worldSize, scale_two->worldSize) &&
          scale_one->viewportGeneration == 9u &&
          scale_two->viewportGeneration == 10u,
      "constant logical size remains stable across validated DPI changes");
  passed &= expect(
      !one_x.constantLogicalPixelScale(24.0, 0.0) &&
          !one_x.constantLogicalPixelScale(24.0, -1.0) &&
          !one_x.constantLogicalPixelScale(0.0, 5.0),
      "constant-size input rejects zero/negative depth and pixel count");

  EditorViewportMetrics orthographic = one_x;
  orthographic.projectionKind = EditorProjectionKind::orthographic;
  orthographic.verticalFieldOfViewRadians = 0.0;
  orthographic.orthographicVerticalSpan = 20.0;
  orthographic.projection = {13u, 0x9999u};
  const std::optional<ConstantLogicalPixelScale> ortho =
      orthographic.constantLogicalPixelScale(10.0, 1.0);
  passed &= expect(
      orthographic.valid() && ortho &&
          close(ortho->worldUnitsPerLogicalPixel, 20.0 / 400.0) &&
          close(ortho->worldSize, 0.5) &&
          !orthographic.constantLogicalPixelScale(10.0, 0.0),
      "orthographic size is stable but still rejects behind-camera attachment");
  return passed;
}

} // namespace

int main() {
  bool passed = true;
  passed &= testHalfOpenMappingAndDpiBoundSubrect();
  passed &= testDpiAndNonUniformScalePartitions();
  passed &= testSurfaceTransforms();
  passed &= testInvalidMetricsFailClosed();
  passed &= testProjectionAwareConstantLogicalPixels();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
