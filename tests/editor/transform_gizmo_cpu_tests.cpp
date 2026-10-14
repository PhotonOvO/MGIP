#include "render/editor/TransformGizmo.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <numbers>
#include <string_view>
#include <type_traits>

namespace {

using namespace demo::editor;

static_assert(kGizmoComponentCount == 13u);
static_assert(std::is_trivially_copyable_v<GizmoAction>);
static_assert(std::is_trivially_copyable_v<GizmoDrawPacket>);
static_assert(std::is_trivially_copyable_v<GizmoInteractionState>);
static_assert(std::is_trivially_copyable_v<GizmoCameraSnapshot>);
static_assert(std::is_trivially_copyable_v<GizmoViewDepthSnapshot>);

bool expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
  }
  return condition;
}

bool near(double left, double right, double tolerance = 1.0e-8) {
  return std::isfinite(left) && std::isfinite(right) &&
         std::abs(left - right) <= tolerance;
}

bool near(OverlayVec3 left, OverlayVec3 right,
          double tolerance = 1.0e-8) {
  return near(left.x, right.x, tolerance) &&
         near(left.y, right.y, tolerance) &&
         near(left.z, right.z, tolerance);
}

double quaternionLength(OverlayQuaternion value) {
  return std::sqrt(value.x * value.x + value.y * value.y +
                   value.z * value.z + value.w * value.w);
}

bool finitePacket(const GizmoDrawPacket &packet) {
  if (!std::isfinite(packet.worldScale) || packet.worldScale <= 0.0 ||
      !std::isfinite(packet.canonicalVisualDiameter) ||
      packet.canonicalVisualDiameter <= 0.0) {
    return false;
  }
  for (const GizmoComponentDrawPacket &component : packet.components) {
    for (double element : component.model.elements) {
      if (!std::isfinite(element)) {
        return false;
      }
    }
  }
  return true;
}

bool quarterTurn(EditorViewportSurfaceTransform transform) {
  return transform == EditorViewportSurfaceTransform::rotate90 ||
         transform == EditorViewportSurfaceTransform::rotate270 ||
         transform ==
             EditorViewportSurfaceTransform::mirrorHorizontalRotate90 ||
         transform ==
             EditorViewportSurfaceTransform::mirrorHorizontalRotate270;
}

EditorViewportMetrics viewport(
    double scaleX = 1.0, double scaleY = 1.0,
    EditorProjectionKind kind = EditorProjectionKind::perspective,
    EditorViewportSurfaceTransform transform =
        EditorViewportSurfaceTransform::identity,
    EditorClipDepthRange depthRange = EditorClipDepthRange::zeroToOne,
    bool reversedDepth = true) {
  constexpr double width = 800.0;
  constexpr double height = 600.0;
  const auto sourceWidth = static_cast<std::uint32_t>(width * scaleX);
  const auto sourceHeight = static_cast<std::uint32_t>(height * scaleY);
  const PhysicalViewportExtent attachment =
      quarterTurn(transform)
          ? PhysicalViewportExtent{sourceHeight, sourceWidth}
          : PhysicalViewportExtent{sourceWidth, sourceHeight};

  EditorViewportMetrics result;
  result.logical = {{0.0, 0.0}, {width, height}};
  result.framebufferLogicalOrigin = {0.0, 0.0};
  result.physical = {{0u, 0u}, attachment};
  result.depthPhysical = result.physical;
  result.colorAttachment = attachment;
  result.depthAttachment = attachment;
  result.framebufferScale = {scaleX, scaleY};
  result.surfaceTransform = transform;
  result.viewportGeneration = 17u;
  result.projection = {23u, 0x1234u};
  result.view = {29u, 0x5678u};
  result.convention = {depthRange, reversedDepth, true};
  result.projectionKind = kind;
  if (kind == EditorProjectionKind::perspective) {
    result.verticalFieldOfViewRadians = std::numbers::pi_v<double> / 3.0;
  } else {
    result.orthographicVerticalSpan = 12.0;
  }
  return result;
}

OverlayGenerationBinding binding(const EditorViewportMetrics &metrics) {
  return {metrics.viewportGeneration, metrics.projection, metrics.view};
}

GizmoCameraSnapshot cameraSnapshot(const EditorViewportMetrics &metrics,
                                   const OverlayMatrix4 &inverse,
                                   bool ndcYDown = false) {
  return {binding(metrics), inverse, ndcYDown};
}

GizmoViewDepthSnapshot viewDepthSnapshot(
    const EditorViewportMetrics &metrics, double positiveViewDepth) {
  return {binding(metrics), positiveViewDepth};
}

OverlayMatrix4 perspective(double nearPlane, double farPlane,
                           double nearNdc, double farNdc,
                           double ySign = 1.0) {
  const double f = 1.0 / std::tan(std::numbers::pi_v<double> / 6.0);
  const double b = (nearNdc - farNdc) * nearPlane * farPlane /
                   (farPlane - nearPlane);
  const double a = b / nearPlane - nearNdc;
  OverlayMatrix4 result;
  result.elements = {f * 0.75, 0.0, 0.0, 0.0,
                     0.0, f * ySign, 0.0, 0.0,
                     0.0, 0.0, a, b,
                     0.0, 0.0, -1.0, 0.0};
  return result;
}

OverlayMatrix4 orthographic(double nearPlane, double farPlane,
                            double nearNdc, double farNdc,
                            double ySign = 1.0) {
  const double a = (nearNdc - farNdc) / (farPlane - nearPlane);
  const double b = nearNdc + a * nearPlane;
  OverlayMatrix4 result;
  result.elements = {0.25, 0.0, 0.0, 0.0,
                     0.0, (1.0 / 3.0) * ySign, 0.0, 0.0,
                     0.0, 0.0, a, b,
                     0.0, 0.0, 0.0, 1.0};
  return result;
}

GizmoRay ray(OverlayVec3 origin, OverlayVec3 direction) {
  return {origin, direction};
}

OverlayQuaternion rotationZ(double degrees) {
  const double half = degrees * std::numbers::pi_v<double> / 360.0;
  return {0.0, 0.0, std::sin(half), std::cos(half)};
}

OverlayMatrix4 parentMatrix(double radians, OverlayVec3 scale,
                            OverlayVec3 translation = {}) {
  const double cosine = std::cos(radians);
  const double sine = std::sin(radians);
  OverlayMatrix4 result;
  result.elements = {
      cosine * scale.x, -sine * scale.y, 0.0, translation.x,
      sine * scale.x, cosine * scale.y, 0.0, translation.y,
      0.0, 0.0, scale.z, translation.z,
      0.0, 0.0, 0.0, 1.0};
  return result;
}

std::optional<OverlayVec2>
projectLogical(OverlayVec3 worldPoint, const OverlayMatrix4 &projection,
               const EditorViewportMetrics &metrics) {
  const double clipX = projection.elements[0] * worldPoint.x +
                       projection.elements[1] * worldPoint.y +
                       projection.elements[2] * worldPoint.z +
                       projection.elements[3];
  const double clipY = projection.elements[4] * worldPoint.x +
                       projection.elements[5] * worldPoint.y +
                       projection.elements[6] * worldPoint.z +
                       projection.elements[7];
  const double clipW = projection.elements[12] * worldPoint.x +
                       projection.elements[13] * worldPoint.y +
                       projection.elements[14] * worldPoint.z +
                       projection.elements[15];
  if (!std::isfinite(clipX) || !std::isfinite(clipY) ||
      !std::isfinite(clipW) || clipW <= 0.0) {
    return std::nullopt;
  }
  const double ndcX = clipX / clipW;
  const double ndcY = clipY / clipW;
  const OverlayVec2 result{
      metrics.logical.origin.x +
          (ndcX + 1.0) * 0.5 * metrics.logical.extent.width,
      metrics.logical.origin.y +
          (1.0 - ndcY) * 0.5 * metrics.logical.extent.height};
  return std::isfinite(result.x) && std::isfinite(result.y)
             ? std::optional<OverlayVec2>(result)
             : std::nullopt;
}

std::optional<double>
projectedSpan(const GizmoDrawPacket &packet, OverlayVec3 localFirst,
              OverlayVec3 localSecond, const OverlayMatrix4 &projection,
              const EditorViewportMetrics &metrics) {
  const auto firstWorld =
      transformOverlayPoint(packet.components[0].model, localFirst);
  const auto secondWorld =
      transformOverlayPoint(packet.components[0].model, localSecond);
  const auto first =
      firstWorld ? projectLogical(*firstWorld, projection, metrics)
                 : std::nullopt;
  const auto second =
      secondWorld ? projectLogical(*secondWorld, projection, metrics)
                  : std::nullopt;
  if (!first || !second) {
    return std::nullopt;
  }
  return std::hypot(second->x - first->x, second->y - first->y);
}

GizmoAttachment attachment() {
  GizmoAttachment result;
  result.nodeId = {7u, 0xabcdu};
  result.documentRevision = 31u;
  result.selectionRevision = 37u;
  return result;
}

GizmoTerminalBatch batch(
    std::initializer_list<GizmoTerminalEventKind> events) {
  GizmoTerminalBatch result;
  std::uint64_t sequence = 1u;
  for (GizmoTerminalEventKind event : events) {
    result.events[result.count++] = {event, sequence++};
  }
  return result;
}

std::size_t actionCount(const GizmoActionResolution &resolution,
                        GizmoActionKind kind) {
  std::size_t count = 0u;
  for (std::uint8_t index = 0; index < resolution.actionCount; ++index) {
    count += resolution.actions[index].kind == kind ? 1u : 0u;
  }
  return count;
}

bool testGeometryIdentityAndRanges() {
  const auto first = buildGizmoGeometry();
  const auto second = buildGizmoGeometry();
  bool passed = expect(first && second,
                       "canonical geometry generation succeeds twice");
  if (!first || !second) {
    return false;
  }
  passed &= expect(first->style().nominalLogicalPixels == 50.0 &&
                       first->style().axisShaftRadius == 0.02 &&
                       first->style().axisShaftLength == 1.0 &&
                       first->style().translationConeRadius == 0.06 &&
                       first->style().translationConeHeight == 0.25 &&
                       first->style().translationConeSegments == 16u &&
                       first->style().rotationRingRadius == 1.3 &&
                       first->style().rotationTubeRadius == 0.020 &&
                       first->style().rotationRingSegments == 64u &&
                       first->style().rotationTubeSegments == 16u &&
                       first->style().scaleCubeSize == 0.15 &&
                       first->style().scaleAxisLength == 1.67 &&
                       first->style().uniformScaleMultiplier == 2.0 &&
                       first->style().translationPlaneSize == 0.22 &&
                       first->style().translationPlaneOffset == 0.5 &&
                       first->style().translationPlaneAlpha == 0.45 &&
                       !first->style().occlusionDither,
                   "canonical style freezes every reference-derived value");
  passed &= expect(first->identity() == second->identity() &&
                       first->vertices() == second->vertices() &&
                       first->indices() == second->indices() &&
                       first->ranges() == second->ranges() &&
                       first->identity().vertexCount == 4116u &&
                       first->identity().indexCount == 20124u &&
                       first->identity().semanticDigest ==
                           16606883288829366089ull,
                   "geometry bytes, ranges, and semantic identity are deterministic");
  passed &= expect(near(first->canonicalVisualDiameter(), 3.065, 1.0e-12),
                   "canonical diameter is the full atlas bound from ring to scale tip");

  std::uint32_t nextVertex = 0u;
  std::uint32_t nextIndex = 0u;
  for (std::size_t componentIndex = 0; componentIndex < kGizmoComponentCount;
       ++componentIndex) {
    const GizmoComponentRangePacket &range = first->ranges()[componentIndex];
    passed &= expect(range.component == kGizmoComponentOrder[componentIndex] &&
                         range.firstVertex == nextVertex &&
                         range.firstIndex == nextIndex &&
                         range.vertexCount > 0u && range.indexCount > 0u,
                     "all 13 components have stable contiguous nonempty ownership");
    nextVertex += range.vertexCount;
    nextIndex += range.indexCount;
    passed &= expect(finiteOverlayVector(range.localBounds.minimum) &&
                         finiteOverlayVector(range.localBounds.maximum) &&
                         range.localBounds.minimum.x <= range.localBounds.maximum.x &&
                         range.localBounds.minimum.y <= range.localBounds.maximum.y &&
                         range.localBounds.minimum.z <= range.localBounds.maximum.z,
                     "each component owns finite ordered bounds");
    for (std::uint32_t offset = 0; offset < range.indexCount; ++offset) {
      const std::uint32_t vertex =
          first->indices()[range.firstIndex + offset];
      passed &= expect(vertex >= range.firstVertex &&
                           vertex < range.firstVertex + range.vertexCount,
                       "component index never aliases another component range");
    }
  }
  passed &= expect(nextVertex == first->vertices().size() &&
                       nextIndex == first->indices().size(),
                   "ranges cover the atlas exactly once");
  for (const GizmoVertex &vertex : first->vertices()) {
    const double length = std::sqrt(vertex.normal.x * vertex.normal.x +
                                    vertex.normal.y * vertex.normal.y +
                                    vertex.normal.z * vertex.normal.z);
    passed &= expect(finiteOverlayVector(vertex.position) &&
                         finiteOverlayVector(vertex.normal) &&
                         near(length, 1.0, 1.0e-10),
                     "generated positions and unit normals are finite");
  }

  GizmoStyle custom = first->style();
  custom.rotationRingRadius = 1.5;
  const auto customGeometry = buildGizmoGeometry(custom);
  passed &= expect(customGeometry &&
                       customGeometry->identity().semanticDigest !=
                           first->identity().semanticDigest &&
                       customGeometry->ranges().size() == kGizmoComponentCount,
                   "valid custom style produces a distinct stable atlas");
  custom.rotationRingRadius = std::numeric_limits<double>::quiet_NaN();
  passed &= expect(!buildGizmoGeometry(custom),
                   "non-finite geometry style fails closed");
  custom = {};
  custom.rotationTubeSegments = 2u;
  passed &= expect(!buildGizmoGeometry(custom),
                   "underspecified torus segmentation fails closed");
  passed &= expect(gizmoComponentIndex(GizmoComponent::none) ==
                       kGizmoComponentCount,
                   "rotate-screen is absent and none is outside the 13 ranges");
  return passed;
}

bool testRayConstructionAndProjectionConventions() {
  bool passed = true;
  struct PerspectiveCase final {
    EditorClipDepthRange range;
    bool reverse;
    double nearNdc;
    double farNdc;
  };
  const std::array<PerspectiveCase, 4> cases{
      PerspectiveCase{EditorClipDepthRange::zeroToOne, false, 0.0, 1.0},
      PerspectiveCase{EditorClipDepthRange::zeroToOne, true, 1.0, 0.0},
      PerspectiveCase{EditorClipDepthRange::negativeOneToOne, false, -1.0, 1.0},
      PerspectiveCase{EditorClipDepthRange::negativeOneToOne, true, 1.0, -1.0}};
  for (const PerspectiveCase &item : cases) {
    EditorViewportMetrics metrics =
        viewport(1.0, 1.0, EditorProjectionKind::perspective,
                 EditorViewportSurfaceTransform::identity, item.range,
                 item.reverse);
    const auto inverse = invertOverlayMatrix(
        perspective(0.1, 100.0, item.nearNdc, item.farNdc));
    const auto center = inverse
                            ? makeGizmoRay({400.0, 300.0}, metrics,
                                           cameraSnapshot(metrics, *inverse))
                            : std::nullopt;
    passed &= expect(center && finiteOverlayVector(center->origin) &&
                         near(center->direction, {0.0, 0.0, -1.0}, 1.0e-9),
                     "perspective ray honors clip-depth and reverse-Z convention");
  }

  EditorViewportMetrics orthoMetrics =
      viewport(1.0, 1.0, EditorProjectionKind::orthographic);
  const auto inverseOrtho =
      invertOverlayMatrix(orthographic(0.1, 100.0, 1.0, 0.0));
  const auto orthoCenter =
      inverseOrtho
          ? makeGizmoRay({400.0, 300.0}, orthoMetrics,
                         cameraSnapshot(orthoMetrics, *inverseOrtho))
          : std::nullopt;
  const auto orthoCorner =
      inverseOrtho
          ? makeGizmoRay({700.0, 100.0}, orthoMetrics,
                         cameraSnapshot(orthoMetrics, *inverseOrtho))
          : std::nullopt;
  passed &= expect(orthoCenter && orthoCorner &&
                       near(orthoCenter->direction, orthoCorner->direction,
                            1.0e-10) &&
                       !near(orthoCenter->origin, orthoCorner->origin),
                   "orthographic rays have shifted origins and parallel directions");

  EditorViewportMetrics noFlip = viewport();
  noFlip.convention.reversedDepth = false;
  const auto inverseNoFlip =
      invertOverlayMatrix(perspective(0.1, 100.0, 0.0, 1.0, 1.0));
  const auto inverseFlip =
      invertOverlayMatrix(perspective(0.1, 100.0, 0.0, 1.0, -1.0));
  const auto topNoFlip = inverseNoFlip
                             ? makeGizmoRay(
                                   {400.0, 0.0}, noFlip,
                                   cameraSnapshot(noFlip, *inverseNoFlip))
                             : std::nullopt;
  const auto topFlip = inverseFlip
                           ? makeGizmoRay(
                                 {400.0, 0.0}, noFlip,
                                 cameraSnapshot(noFlip, *inverseFlip, true))
                           : std::nullopt;
  passed &= expect(topNoFlip && topFlip && topNoFlip->direction.y > 0.0 &&
                       near(topFlip->direction, topNoFlip->direction) &&
                       near(topFlip->origin, topNoFlip->origin),
                   "matching Vulkan viewport and projection flips preserve top-screen ray");
  const auto mismatchedFlip = inverseFlip
                                 ? makeGizmoRay(
                                       {400.0, 0.0}, noFlip,
                                       cameraSnapshot(noFlip, *inverseFlip))
                                 : std::nullopt;
  passed &= expect(mismatchedFlip && mismatchedFlip->direction.y < 0.0,
                   "Vulkan inverse projection alone cannot identify viewport Y convention");

  const OverlayVec3 visibleWorldPoint{0.5, 1.0, -5.0};
  for (const bool ndcYDown : {false, true}) {
    const auto projection =
        perspective(0.1, 100.0, 0.0, 1.0, ndcYDown ? -1.0 : 1.0);
    const auto ndc = transformOverlayPoint(projection, visibleWorldPoint);
    const auto inverse = invertOverlayMatrix(projection);
    if (!ndc || !inverse) {
      passed &= expect(false, "screen projection fixture is invalid");
      continue;
    }
    // Model the actual viewport transform, independently of makeGizmoRay.
    const LogicalViewportPoint renderedPixel{
        (ndc->x + 1.0) * 0.5 * noFlip.logical.extent.width,
        (ndcYDown ? ndc->y + 1.0 : 1.0 - ndc->y) * 0.5 *
            noFlip.logical.extent.height};
    const auto renderedRay = makeGizmoRay(
        renderedPixel, noFlip, cameraSnapshot(noFlip, *inverse, ndcYDown));
    const auto intersect = renderedRay
                               ? intersectGizmoRayPlane(
                                     *renderedRay, visibleWorldPoint,
                                     {0.0, 0.0, 1.0})
                               : std::nullopt;
    passed &= expect(renderedPixel.y < noFlip.logical.extent.height * 0.5 &&
                         intersect && near(intersect->position, visibleWorldPoint),
                     "rendered positive-Y point above center reconstructs its actual world position");
  }

  const std::array<EditorViewportSurfaceTransform, 8> transforms{
      EditorViewportSurfaceTransform::identity,
      EditorViewportSurfaceTransform::rotate90,
      EditorViewportSurfaceTransform::rotate180,
      EditorViewportSurfaceTransform::rotate270,
      EditorViewportSurfaceTransform::mirrorHorizontal,
      EditorViewportSurfaceTransform::mirrorVertical,
      EditorViewportSurfaceTransform::mirrorHorizontalRotate90,
      EditorViewportSurfaceTransform::mirrorHorizontalRotate270};
  const std::array<OverlayVec2, 8> expectedSurfaceOrigins{
      OverlayVec2{-0.5, 0.5}, OverlayVec2{0.5, 0.5},
      OverlayVec2{0.5, -0.5}, OverlayVec2{-0.5, -0.5},
      OverlayVec2{0.5, 0.5}, OverlayVec2{-0.5, -0.5},
      OverlayVec2{0.5, -0.5}, OverlayVec2{-0.5, 0.5}};
  const OverlayMatrix4 identity{};
  for (std::size_t index = 0; index < transforms.size(); ++index) {
    const EditorViewportSurfaceTransform transform = transforms[index];
    EditorViewportMetrics transformed =
        viewport(1.0, 1.0, EditorProjectionKind::orthographic, transform,
                 EditorClipDepthRange::zeroToOne, false);
    const auto value = makeGizmoRay(
        {200.0, 150.0}, transformed,
        cameraSnapshot(transformed, identity));
    passed &= expect(value && finiteOverlayVector(value->origin) &&
                         finiteOverlayVector(value->direction) &&
                         near(value->origin.x,
                              expectedSurfaceOrigins[index].x) &&
                         near(value->origin.y,
                              expectedSurfaceOrigins[index].y) &&
                         near(value->direction, {0.0, 0.0, 1.0}),
                     "every D4 surface transform maps continuous NDC exactly");
  }

  EditorViewportMetrics outsideMetrics = noFlip;
  const auto outside = inverseNoFlip
                           ? makeGizmoRay(
                                 {-50.0, 700.0}, outsideMetrics,
                                 cameraSnapshot(outsideMetrics,
                                                *inverseNoFlip))
                           : std::nullopt;
  passed &= expect(outside && finiteOverlayVector(outside->direction),
                   "captured pointer coordinates remain usable outside viewport");
  EditorViewportMetrics zero = noFlip;
  zero.logical.extent.height = 0.0;
  OverlayMatrix4 nonFinite{};
  nonFinite.elements[0] = std::numeric_limits<double>::infinity();
  passed &= expect(!makeGizmoRay({0.0, 0.0}, zero,
                                 cameraSnapshot(zero, identity)) &&
                       !makeGizmoRay(
                           {std::numeric_limits<double>::quiet_NaN(), 0.0},
                           noFlip, cameraSnapshot(noFlip, identity)) &&
                       !makeGizmoRay(
                           {0.0, 0.0}, noFlip,
                           cameraSnapshot(noFlip, nonFinite)),
                   "zero viewport and non-finite ray inputs fail explicitly");
  OverlayMatrix4 singular{};
  singular.elements.fill(0.0);
  passed &= expect(!invertOverlayMatrix(singular),
                   "singular projection cannot manufacture an inverse ray");

  GizmoCameraSnapshot staleCamera =
      cameraSnapshot(noFlip, inverseNoFlip ? *inverseNoFlip : identity);
  ++staleCamera.generation.viewportGeneration;
  passed &= expect(
      !makeGizmoRay({400.0, 300.0}, noFlip, staleCamera),
      "stale camera matrix generation cannot be relabeled as current metrics");

  EditorViewportMetrics onePixel = noFlip;
  onePixel.logical.extent = {1.0, 1.0};
  onePixel.physical = {{0u, 0u}, {1u, 1u}};
  onePixel.depthPhysical = onePixel.physical;
  onePixel.colorAttachment = {1u, 1u};
  onePixel.depthAttachment = onePixel.colorAttachment;
  const auto onePixelRay =
      inverseNoFlip
          ? makeGizmoRay({0.5, 0.5}, onePixel,
                         cameraSnapshot(onePixel, *inverseNoFlip))
          : std::nullopt;
  passed &= expect(onePixelRay &&
                       near(onePixelRay->direction, {0.0, 0.0, -1.0}),
                   "one-pixel viewport remains finite without division by zero");
  return passed;
}

bool testRayPlaneAndAllComponentHits() {
  bool passed = true;
  const GizmoGeometryAtlas &geometry = canonicalGizmoGeometry();
  const auto planeHit = intersectGizmoRayPlane(
      ray({0.0, 2.0, 0.0}, {0.0, -2.0, 0.0}), {}, {0.0, 1.0, 0.0});
  passed &= expect(planeHit && near(planeHit->distance, 2.0) &&
                       near(planeHit->position, {}),
                   "ray-plane distance uses a normalized direction");
  passed &= expect(!intersectGizmoRayPlane(
                        ray({0.0, 1.0, 0.0}, {1.0, 0.0, 0.0}), {},
                        {0.0, 1.0, 0.0}) &&
                       !intersectGizmoRayPlane(
                           ray({0.0, 1.0, 0.0}, {1.0, -1.0e-8, 0.0}), {},
                           {0.0, 1.0, 0.0}) &&
                       !intersectGizmoRayPlane(
                           ray({0.0, -1.0, 0.0}, {0.0, -1.0, 0.0}), {},
                           {0.0, 1.0, 0.0}),
                   "parallel, near-parallel, and negative-t plane hits reject");
  const auto aboveParallelCutoff = intersectGizmoRayPlane(
      ray({0.0, 1.0, 0.0}, {1.0, -2.0e-6, 0.0}), {},
      {0.0, 1.0, 0.0});
  passed &= expect(aboveParallelCutoff &&
                       aboveParallelCutoff->distance > 4.9e5 &&
                       aboveParallelCutoff->distance < 5.1e5,
                   "plane intersection accepts finite slope above 1e-6 cutoff");

  struct HitCase final {
    GizmoComponent expected;
    GizmoRay rayValue;
    GizmoVisibility visibility;
    GizmoHitKind kind;
  };
  const std::array<HitCase, 13> cases{
      HitCase{GizmoComponent::translateX, ray({0.8, 0.0, 3.0}, {0.0, 0.0, -1.0}),
              GizmoVisibility::translation, GizmoHitKind::axisShaftOrTip},
      HitCase{GizmoComponent::translateY, ray({0.0, 0.8, 3.0}, {0.0, 0.0, -1.0}),
              GizmoVisibility::translation, GizmoHitKind::axisShaftOrTip},
      HitCase{GizmoComponent::translateZ, ray({0.0, 3.0, 0.8}, {0.0, -1.0, 0.0}),
              GizmoVisibility::translation, GizmoHitKind::axisShaftOrTip},
      HitCase{GizmoComponent::translateXY, ray({0.61, 0.61, 3.0}, {0.0, 0.0, -1.0}),
              GizmoVisibility::translation, GizmoHitKind::translationPlane},
      HitCase{GizmoComponent::translateXZ, ray({0.61, 3.0, 0.61}, {0.0, -1.0, 0.0}),
              GizmoVisibility::translation, GizmoHitKind::translationPlane},
      HitCase{GizmoComponent::translateYZ, ray({3.0, 0.61, 0.61}, {-1.0, 0.0, 0.0}),
              GizmoVisibility::translation, GizmoHitKind::translationPlane},
      HitCase{GizmoComponent::rotateX, ray({3.0, 1.3, 0.0}, {-1.0, 0.0, 0.0}),
              GizmoVisibility::rotation, GizmoHitKind::rotationRing},
      HitCase{GizmoComponent::rotateY, ray({1.3, 3.0, 0.0}, {0.0, -1.0, 0.0}),
              GizmoVisibility::rotation, GizmoHitKind::rotationRing},
      HitCase{GizmoComponent::rotateZ, ray({1.3, 0.0, 3.0}, {0.0, 0.0, -1.0}),
              GizmoVisibility::rotation, GizmoHitKind::rotationRing},
      HitCase{GizmoComponent::scaleX, ray({0.8, 0.0, 3.0}, {0.0, 0.0, -1.0}),
              GizmoVisibility::scale, GizmoHitKind::axisShaftOrTip},
      HitCase{GizmoComponent::scaleY, ray({0.0, 0.8, 3.0}, {0.0, 0.0, -1.0}),
              GizmoVisibility::scale, GizmoHitKind::axisShaftOrTip},
      HitCase{GizmoComponent::scaleZ, ray({0.0, 3.0, 0.8}, {0.0, -1.0, 0.0}),
              GizmoVisibility::scale, GizmoHitKind::axisShaftOrTip},
      HitCase{GizmoComponent::uniformScale, ray({3.0, 3.0, 3.0}, {-1.0, -1.0, -1.0}),
              GizmoVisibility::scale, GizmoHitKind::uniformCenter}};
  for (const HitCase &item : cases) {
    const GizmoHit hit =
        classifyGizmoHit(item.rayValue, geometry, {}, item.visibility);
    passed &= expect(hit.component == item.expected && hit.kind == item.kind &&
                         std::isfinite(hit.rayDistance) &&
                         hit.rayDistance >= 0.0 &&
                         std::isfinite(hit.missDistance),
                     "each of the exact 13 components has a finite hit path");
  }

  const GizmoHit tie = classifyGizmoHit(
      ray({0.8, 0.0, 3.0}, {0.0, 0.0, -1.0}), geometry);
  passed &= expect(tie.component == GizmoComponent::translateX,
                   "equal-distance overlap preserves canonical component order");
  GizmoHitStyle tolerance;
  const GizmoHit ringInside = classifyGizmoHit(
      ray({1.499999, 0.0, 3.0}, {0.0, 0.0, -1.0}), geometry,
      tolerance, GizmoVisibility::rotation);
  const GizmoHit ringOutside = classifyGizmoHit(
      ray({1.5001, 0.0, 3.0}, {0.0, 0.0, -1.0}), geometry,
      tolerance, GizmoVisibility::rotation);
  passed &= expect(ringInside.component == GizmoComponent::rotateZ &&
                       !ringOutside.hit(),
                   "ring just-inside/outside tolerance is deterministic");
  const GizmoHit axisInside = classifyGizmoHit(
      ray({0.8, 0.599, 3.0}, {0.0, 0.0, -1.0}), geometry, tolerance,
      GizmoVisibility::translation);
  const GizmoHit axisOutside = classifyGizmoHit(
      ray({0.8, 0.601, 3.0}, {0.0, 0.0, -1.0}), geometry, tolerance,
      GizmoVisibility::translation);
  passed &= expect(axisInside.component == GizmoComponent::translateX &&
                       axisOutside.component != GizmoComponent::translateX,
                   "locked 0.6 axis threshold has stable inside/outside behavior");
  const GizmoHit behindAxis = classifyGizmoHit(
      ray({1.30, 0.0, 0.0}, {1.0, 0.0, 0.0}), geometry, tolerance,
      GizmoVisibility::translation);
  passed &= expect(!behindAxis.hit(),
                   "axis centerline entirely behind the ray origin never hits");
  GizmoHitStyle invalidStyle;
  invalidStyle.rotationRingTolerance = -1.0;
  passed &= expect(!classifyGizmoHit(ray({0.0, 0.0, 3.0}, {0.0, 0.0, -1.0}),
                                    geometry, {}, GizmoVisibility::none)
                         .hit() &&
                       !classifyGizmoHit(ray({}, {}), geometry).hit() &&
                       !classifyGizmoHit(
                            ray({0.0, 0.0, 3.0}, {0.0, 0.0, -1.0}),
                            geometry, invalidStyle)
                            .hit(),
                   "hidden, degenerate, and invalid-style classifiers miss");
  return passed;
}

bool testParentBasisAndConditioning() {
  bool passed = true;
  GizmoAttachment value = attachment();
  value.local.translation = {1.0, 2.0, 3.0};
  value.local.rotation = rotationZ(90.0);
  value.parentWorld = parentMatrix(std::numbers::pi_v<double> * 0.5,
                                   {-2.0, 3.0, 4.0}, {5.0, 7.0, 11.0});
  const auto global = deriveGizmoBasis(value, GizmoBasisToken::global);
  const auto local = deriveGizmoBasis(value, GizmoBasisToken::parentLocal);
  passed &= expect(global && local &&
                       near(global->pivotWorld, {-1.0, 5.0, 23.0}) &&
                       near(global->axes[0], {0.0, -1.0, 0.0}) &&
                       near(global->axes[1], {-1.0, 0.0, 0.0}) &&
                       near(global->axes[2], {0.0, 0.0, 1.0}) &&
                       !near(local->axes[0], global->axes[0]),
                   "global basis follows signed parent columns and ignores own rotation");
  passed &= expect(global && global->parentDeterminant < 0.0 &&
                       std::isfinite(global->parentConditionEstimate),
                   "negative nonuniform parent remains finite and records reflection");

  const auto world = parentLocalToWorldDirection(value, {1.0, -2.0, 3.0});
  const auto localRoundTrip =
      world ? worldToParentLocalDirection(value, *world) : std::nullopt;
  passed &= expect(world && localRoundTrip &&
                       near(*localRoundTrip, {1.0, -2.0, 3.0}, 1.0e-9),
                   "world/local direction conversion round-trips nonuniform reflection");

  GizmoAttachment sheared = attachment();
  sheared.parentWorld.elements[1] = 0.4;
  sheared.parentWorld.elements[6] = -0.25;
  const auto shearBasis = deriveGizmoBasis(sheared);
  passed &= expect(shearBasis &&
                       std::isfinite(shearBasis->parentConditionEstimate) &&
                       shearBasis->parentConditionEstimate > 1.0,
                   "finite sheared parent has an explicit conditioned basis");

  GizmoAttachment singular = attachment();
  singular.parentWorld.elements[10] = 0.0;
  GizmoAttachment nearSingular = attachment();
  nearSingular.parentWorld.elements[10] = 1.0e-12;
  GizmoAttachment illConditioned = attachment();
  illConditioned.parentWorld.elements[10] = 1.0e-11;
  GizmoAttachment nonAffine = attachment();
  nonAffine.parentWorld.elements[12] = 0.1;
  GizmoAttachment translatedProjective = attachment();
  translatedProjective.parentWorld.elements[3] = 1.0e20;
  translatedProjective.parentWorld.elements[12] = 1.0;
  passed &= expect(!deriveGizmoBasis(singular) &&
                       !deriveGizmoBasis(nearSingular) &&
                       !deriveGizmoBasis(illConditioned) &&
                       !deriveGizmoBasis(nonAffine) &&
                       !deriveGizmoBasis(translatedProjective),
                   "singular, near-singular, ill-conditioned, and projective parents fail");

  for (std::uint32_t mask = 0u; mask < 8u; ++mask) {
    GizmoAttachment reflected = attachment();
    reflected.parentWorld = parentMatrix(
        0.25, {(mask & 1u) ? -2.0 : 2.0, (mask & 2u) ? -3.0 : 3.0,
               (mask & 4u) ? -4.0 : 4.0});
    const auto basis = deriveGizmoBasis(reflected);
    passed &= expect(basis && finiteOverlayVector(basis->axes[0]) &&
                         finiteOverlayVector(basis->axes[1]) &&
                         finiteOverlayVector(basis->axes[2]),
                     "all one/two/three-axis reflection partitions stay finite");
  }
  return passed;
}

bool testFreshDocumentRevisionZeroIsRenderable() {
  GizmoAttachment value = attachment();
  value.documentRevision = 0u;
  value.selectionRevision = 1u;
  const EditorViewportMetrics metrics = viewport(1.0, 1.0);
  const GizmoViewDepthSnapshot depth{
      {metrics.viewportGeneration, metrics.projection, metrics.view}, 5.0};
  const auto packet = buildGizmoDrawPacket(
      canonicalGizmoGeometry(), value, metrics, depth, {});
  return validGizmoAttachment(value) && packet.has_value();
}

bool testTranslationSolutionsAndSnapping() {
  bool passed = true;
  GizmoAttachment value = attachment();
  const auto x = solveGizmoTranslation(
      GizmoComponent::translateX,
      ray({0.0, 0.0, 5.0}, {0.0, 0.0, -1.0}),
      ray({2.0, 3.0, 5.0}, {0.0, 0.0, -1.0}), {0.0, 0.0, -1.0}, value);
  const auto y = solveGizmoTranslation(
      GizmoComponent::translateY,
      ray({0.0, 0.0, 5.0}, {0.0, 0.0, -1.0}),
      ray({-2.0, -3.0, 5.0}, {0.0, 0.0, -1.0}), {0.0, 0.0, -1.0}, value);
  const auto z = solveGizmoTranslation(
      GizmoComponent::translateZ,
      ray({0.0, 5.0, 0.0}, {0.0, -1.0, 0.0}),
      ray({2.0, 5.0, 4.0}, {0.0, -1.0, 0.0}), {0.0, -1.0, 0.0}, value);
  passed &= expect(x && near(x->worldDelta, {2.0, 0.0, 0.0}) &&
                       y && near(y->worldDelta, {0.0, -3.0, 0.0}) &&
                       z && near(z->worldDelta, {0.0, 0.0, 4.0}),
                   "X/Y/Z axis translations project signed world deltas");

  const auto xy = solveGizmoTranslation(
      GizmoComponent::translateXY,
      ray({0.0, 0.0, 5.0}, {0.0, 0.0, -1.0}),
      ray({2.0, -3.0, 5.0}, {0.0, 0.0, -1.0}), {0.0, 0.0, -1.0}, value);
  const auto xz = solveGizmoTranslation(
      GizmoComponent::translateXZ,
      ray({0.0, 5.0, 0.0}, {0.0, -1.0, 0.0}),
      ray({-2.0, 5.0, 4.0}, {0.0, -1.0, 0.0}), {0.0, -1.0, 0.0}, value);
  const auto yz = solveGizmoTranslation(
      GizmoComponent::translateYZ,
      ray({5.0, 0.0, 0.0}, {-1.0, 0.0, 0.0}),
      ray({5.0, 3.0, -4.0}, {-1.0, 0.0, 0.0}), {-1.0, 0.0, 0.0}, value);
  passed &= expect(xy && near(xy->worldDelta, {2.0, -3.0, 0.0}) &&
                       xz && near(xz->worldDelta, {-2.0, 0.0, 4.0}) &&
                       yz && near(yz->worldDelta, {0.0, 3.0, -4.0}),
                   "XY/XZ/YZ plane translations retain both signed components");

  value.local.translation = {0.49, 0.51, -0.51};
  const auto snapped = solveGizmoTranslation(
      GizmoComponent::translateX,
      ray({0.0, 0.0, 5.0}, {0.0, 0.0, -1.0}),
      ray({0.1, 0.0, 5.0}, {0.0, 0.0, -1.0}), {0.0, 0.0, -1.0}, value,
      GizmoBasisToken::global, 1.0);
  passed &= expect(snapped &&
                       near(snapped->resultingLocalPosition, {1.0, 1.0, -1.0}) &&
                       near(snapped->localDelta, {0.51, 0.49, -0.49}),
                   "translation snap rounds the complete resulting local vector");

  GizmoAttachment reflected = attachment();
  reflected.local.translation = {1.0, 2.0, 3.0};
  reflected.parentWorld = parentMatrix(0.0, {-2.0, 3.0, 4.0});
  const OverlayVec3 pivot{-2.0, 6.0, 12.0};
  const auto parentDelta = solveGizmoTranslation(
      GizmoComponent::translateX,
      ray({pivot.x, pivot.y, pivot.z + 5.0}, {0.0, 0.0, -1.0}),
      ray({pivot.x - 2.0, pivot.y, pivot.z + 5.0}, {0.0, 0.0, -1.0}),
      {0.0, 0.0, -1.0}, reflected);
  passed &= expect(parentDelta &&
                       near(parentDelta->worldDelta, {-2.0, 0.0, 0.0}) &&
                       near(parentDelta->localDelta, {1.0, 0.0, 0.0}),
                   "negative nonuniform parent converts world drag to stable local delta");

  passed &= expect(!solveGizmoTranslation(
                        GizmoComponent::translateX,
                        ray({0.0, 0.0, 5.0}, {0.0, 0.0, -1.0}),
                        ray({1.0, 0.0, 5.0}, {0.0, 0.0, -1.0}),
                        {1.0, 0.0, 0.0}, attachment()) &&
                       !solveGizmoTranslation(
                           GizmoComponent::translateX,
                           ray({0.0, 0.0, 5.0}, {0.0, 0.0, -1.0}),
                           ray({1.0, 0.0, 5.0}, {0.0, 0.0, -1.0}),
                           {0.0, 0.0, -1.0}, attachment(),
                           GizmoBasisToken::global, 0.0),
                   "axis-parallel best-view plane and invalid snap fail closed");
  return passed;
}

bool testRotationAndScaleSolutions() {
  bool passed = true;
  GizmoAttachment value = attachment();
  const auto zPositive = solveGizmoRotation(
      GizmoComponent::rotateZ,
      ray({1.0, 0.0, 5.0}, {0.0, 0.0, -1.0}),
      ray({0.0, 1.0, 5.0}, {0.0, 0.0, -1.0}), value);
  const auto zNegative = solveGizmoRotation(
      GizmoComponent::rotateZ,
      ray({1.0, 0.0, 5.0}, {0.0, 0.0, -1.0}),
      ray({0.0, -1.0, 5.0}, {0.0, 0.0, -1.0}), value);
  const auto xPositive = solveGizmoRotation(
      GizmoComponent::rotateX,
      ray({5.0, 1.0, 0.0}, {-1.0, 0.0, 0.0}),
      ray({5.0, 0.0, 1.0}, {-1.0, 0.0, 0.0}), value);
  const auto yPositive = solveGizmoRotation(
      GizmoComponent::rotateY,
      ray({0.0, 5.0, 1.0}, {0.0, -1.0, 0.0}),
      ray({1.0, 5.0, 0.0}, {0.0, -1.0, 0.0}), value);
  passed &= expect(zPositive && near(zPositive->degrees, 90.0) &&
                       zNegative && near(zNegative->degrees, -90.0) &&
                       xPositive && near(xPositive->degrees, 90.0) &&
                       yPositive && near(yPositive->degrees, 90.0) &&
                       near(quaternionLength(zPositive->resultingLocalRotation),
                            1.0, 1.0e-10),
                   "X/Y/Z rings produce signed normalized rotation results");

  GizmoAttachment reflectedRotation = attachment();
  reflectedRotation.parentWorld = parentMatrix(0.0, {-1.0, 1.0, 1.0});
  const auto reflectedX = solveGizmoRotation(
      GizmoComponent::rotateX,
      ray({-5.0, 1.0, 0.0}, {1.0, 0.0, 0.0}),
      ray({-5.0, 0.0, 1.0}, {1.0, 0.0, 0.0}),
      reflectedRotation);
  passed &= expect(
      reflectedX && near(reflectedX->degrees, -90.0) &&
          near(reflectedX->axisWorld, {-1.0, 0.0, 0.0}) &&
          near(reflectedX->deltaLocal.x,
               std::sin(std::numbers::pi_v<double> * 0.25)) &&
          near(reflectedX->deltaLocal.w,
               std::cos(std::numbers::pi_v<double> * 0.25)),
      "single-axis parent reflection converts world signed angle to local quaternion");

  const double startAngle = 179.0 * std::numbers::pi_v<double> / 180.0;
  const double currentAngle = -179.0 * std::numbers::pi_v<double> / 180.0;
  const auto wrap = solveGizmoRotation(
      GizmoComponent::rotateZ,
      ray({std::cos(startAngle), std::sin(startAngle), 5.0}, {0.0, 0.0, -1.0}),
      ray({std::cos(currentAngle), std::sin(currentAngle), 5.0},
          {0.0, 0.0, -1.0}),
      value);
  passed &= expect(wrap && near(wrap->degrees, 2.0, 1.0e-8),
                   "signed rotation wraps continuously across +/-180 degrees");

  const double angle44 = 44.0 * std::numbers::pi_v<double> / 180.0;
  const auto snappedRotation = solveGizmoRotation(
      GizmoComponent::rotateZ,
      ray({1.0, 0.0, 5.0}, {0.0, 0.0, -1.0}),
      ray({std::cos(angle44), std::sin(angle44), 5.0}, {0.0, 0.0, -1.0}),
      value, GizmoBasisToken::global, 45.0);
  passed &= expect(snappedRotation && near(snappedRotation->degrees, 45.0),
                   "rotation delta snaps to nearest degree multiple");
  passed &= expect(!solveGizmoRotation(
                        GizmoComponent::rotateZ,
                        ray({0.0, 0.0, 5.0}, {0.0, 0.0, -1.0}),
                        ray({0.0, 1.0, 5.0}, {0.0, 0.0, -1.0}), value),
                   "rotation starting at center rejects zero radial vector");

  const auto axisScale = solveGizmoScale(
      GizmoComponent::scaleX, {2.0, -3.0, 0.0}, {10.0, 20.0},
      {30.0, 10.0});
  const auto uniformScale = solveGizmoScale(
      GizmoComponent::uniformScale, {2.0, -3.0, 0.0}, {10.0, 20.0},
      {30.0, 10.0});
  passed &= expect(axisScale && near(axisScale->factor, 1.15) &&
                       near(axisScale->resultingLocalScale, {2.3, -3.0, 0.0}) &&
                       uniformScale && near(uniformScale->factor, 1.15) &&
                       near(uniformScale->resultingLocalScale,
                            {2.3, -3.45, 0.0}),
                   "axis/uniform scale preserve zero and negative authored starts");
  const auto minimumScale = solveGizmoScale(
      GizmoComponent::uniformScale, {2.0, -3.0, 4.0}, {0.0, 0.0},
      {-10000.0, 10000.0});
  const auto snappedScale = solveGizmoScale(
      GizmoComponent::scaleY, {1.0, 1.04, 1.0}, {0.0, 0.0}, {10.0, 0.0},
      0.1);
  passed &= expect(minimumScale && near(minimumScale->factor, 0.01) &&
                       near(minimumScale->resultingLocalScale,
                            {0.02, -0.03, 0.04}) &&
                       snappedScale &&
                       near(snappedScale->resultingLocalScale.y, 1.1),
                   "scale response clamps at 0.01 and snaps affected result");
  passed &= expect(!solveGizmoScale(
                        GizmoComponent::scaleX, {1.0, 1.0, 1.0}, {},
                        {std::numeric_limits<double>::infinity(), 0.0}) &&
                       !solveGizmoScale(GizmoComponent::translateX,
                                        {1.0, 1.0, 1.0}, {}, {}),
                   "non-finite and non-scale operations reject without output");
  return passed;
}

bool testSnapMatrix() {
  bool passed = true;
  const std::array<double, 3> translationSteps{0.001, 1.0, 1000.0};
  const std::array<double, 3> rotationSteps{1.0, 45.0, 180.0};
  const std::array<double, 3> scaleSteps{0.001, 0.1, 10.0};
  for (double step : translationSteps) {
    const auto positive = snapGizmoScalar(step * 0.5, step);
    const auto negative = snapGizmoScalar(step * -0.5, step);
    passed &= expect(positive && negative && near(*positive, step) &&
                         near(*negative, -step),
                     "translation half boundaries round away from zero");
  }
  for (double step : rotationSteps) {
    const auto positive = snapGizmoScalar(step * 1.49, step);
    const auto negative = snapGizmoScalar(step * -1.5, step);
    passed &= expect(positive && negative && near(*positive, step) &&
                         near(*negative, -2.0 * step),
                     "rotation steps use nearest multiple for both signs");
  }
  for (double step : scaleSteps) {
    const auto values = snapGizmoVector(
        {step * 0.49, step * 0.5, step * -0.5}, step);
    passed &= expect(values && near(*values, {0.0, step, -step}),
                     "scale/vector snap applies C++ half-away semantics");
  }
  passed &= expect(!snapGizmoScalar(1.0, 0.0) &&
                       !snapGizmoScalar(1.0, -1.0) &&
                       !snapGizmoScalar(
                           std::numeric_limits<double>::infinity(), 1.0) &&
                       !snapGizmoScalar(
                           1.0, std::numeric_limits<double>::quiet_NaN()),
                   "invalid snap values and steps fail closed");
  return passed;
}

bool testConstantSizeAndDrawPacket() {
  bool passed = true;
  const GizmoGeometryAtlas &geometry = canonicalGizmoGeometry();
  std::optional<double> baselineWorldScale;
  std::optional<double> baselineHitSpan;
  double canonicalMinimumX = std::numeric_limits<double>::infinity();
  double canonicalMaximumX = -std::numeric_limits<double>::infinity();
  for (const GizmoComponentRangePacket &range : geometry.ranges()) {
    canonicalMinimumX =
        std::min(canonicalMinimumX, range.localBounds.minimum.x);
    canonicalMaximumX =
        std::max(canonicalMaximumX, range.localBounds.maximum.x);
  }
  const OverlayMatrix4 visualProjection =
      perspective(0.1, 100.0, 1.0, 0.0);
  const auto verifyProjectedSize =
      [&](const EditorViewportMetrics &metrics, double depth,
          std::string_view label) {
        GizmoAttachment projectedAttachment = attachment();
        projectedAttachment.local.translation = {0.0, 0.0, -depth};
        const auto projectedPacket = buildGizmoDrawPacket(
            geometry, projectedAttachment, metrics,
            viewDepthSnapshot(metrics, depth));
        const auto silhouette =
            projectedPacket
                ? projectedSpan(*projectedPacket,
                                {canonicalMinimumX, 0.0, 0.0},
                                {canonicalMaximumX, 0.0, 0.0},
                                visualProjection, metrics)
                : std::nullopt;
        const auto hitRegion =
            projectedPacket
                ? projectedSpan(*projectedPacket, {0.8, -0.6, 0.0},
                                {0.8, 0.6, 0.0}, visualProjection, metrics)
                : std::nullopt;
        const auto pivotWorld =
            projectedPacket
                ? transformOverlayPoint(
                      projectedPacket->components[0].model, {})
                : std::nullopt;
        const auto pivot =
            pivotWorld
                ? projectLogical(*pivotWorld, visualProjection, metrics)
                : std::nullopt;
        const double expectedCenterX =
            metrics.logical.origin.x + metrics.logical.extent.width * 0.5;
        const double expectedCenterY =
            metrics.logical.origin.y + metrics.logical.extent.height * 0.5;
        bool valid = projectedPacket && silhouette && hitRegion && pivot &&
                     std::abs(*silhouette - 50.0) <= 3.0 &&
                     std::abs(pivot->x - expectedCenterX) <= 2.0 &&
                     std::abs(pivot->y - expectedCenterY) <= 2.0;
        if (!baselineHitSpan && hitRegion) {
          baselineHitSpan = *hitRegion;
        } else if (baselineHitSpan && hitRegion) {
          valid &= std::abs(*hitRegion - *baselineHitSpan) <= 3.0;
        }
        passed &= expect(valid, label);
      };
  for (double dpi : std::array<double, 4>{1.0, 1.25, 1.5, 2.0}) {
    const EditorViewportMetrics metrics = viewport(dpi, dpi);
    const auto scale = computeGizmoConstantScale(
        metrics, viewDepthSnapshot(metrics, 5.0),
        geometry.canonicalVisualDiameter());
    passed &= expect(scale && std::isfinite(scale->worldScale),
                     "constant-size solution remains finite per DPI");
    verifyProjectedSize(
        metrics, 5.0,
        "projected silhouette/center/hit region meet logical-pixel tolerances per DPI");
    if (!baselineWorldScale && scale) {
      baselineWorldScale = scale->worldScale;
    } else if (scale) {
      passed &= expect(near(*baselineWorldScale, scale->worldScale, 1.0e-12),
                       "validated DPI does not double-apply framebuffer scale");
    }
  }
  const EditorViewportMetrics nonuniform = viewport(1.25, 2.0);
  const auto nonuniformScale = computeGizmoConstantScale(
      nonuniform, viewDepthSnapshot(nonuniform, 5.0),
      geometry.canonicalVisualDiameter());
  passed &= expect(nonuniformScale && baselineWorldScale &&
                       near(nonuniformScale->worldScale, *baselineWorldScale,
                            1.0e-12),
                   "nonuniform framebuffer scale preserves logical Y sizing");
  verifyProjectedSize(
      nonuniform, 5.0,
      "nonuniform DPI preserves projected logical silhouette and hit region");

  const EditorViewportMetrics perspectiveMetrics = viewport();
  const auto nearScale = computeGizmoConstantScale(
      perspectiveMetrics, viewDepthSnapshot(perspectiveMetrics, 2.0),
      geometry.canonicalVisualDiameter());
  const auto farScale = computeGizmoConstantScale(
      perspectiveMetrics, viewDepthSnapshot(perspectiveMetrics, 20.0),
      geometry.canonicalVisualDiameter());
  EditorViewportMetrics ortho =
      viewport(1.0, 1.0, EditorProjectionKind::orthographic);
  const auto orthoNear = computeGizmoConstantScale(
      ortho, viewDepthSnapshot(ortho, 2.0),
      geometry.canonicalVisualDiameter());
  const auto orthoFar = computeGizmoConstantScale(
      ortho, viewDepthSnapshot(ortho, 20.0),
      geometry.canonicalVisualDiameter());
  passed &= expect(nearScale && farScale &&
                       near(farScale->worldScale / nearScale->worldScale, 10.0) &&
                       orthoNear && orthoFar &&
                       near(orthoNear->worldScale, orthoFar->worldScale),
                   "perspective scales with depth while orthographic ignores depth magnitude");
  const auto verifyOrthographicSize =
      [&](const EditorViewportMetrics &metrics, std::string_view label) {
        OverlayMatrix4 projection;
        const double aspect =
            metrics.logical.extent.width / metrics.logical.extent.height;
        projection.elements[0] =
            2.0 / (metrics.orthographicVerticalSpan * aspect);
        projection.elements[5] =
            2.0 / metrics.orthographicVerticalSpan;
        GizmoAttachment projectedAttachment = attachment();
        const auto projectedPacket = buildGizmoDrawPacket(
            geometry, projectedAttachment, metrics,
            viewDepthSnapshot(metrics, 5.0));
        const auto silhouette =
            projectedPacket
                ? projectedSpan(*projectedPacket,
                                {canonicalMinimumX, 0.0, 0.0},
                                {canonicalMaximumX, 0.0, 0.0}, projection,
                                metrics)
                : std::nullopt;
        const auto hitRegion =
            projectedPacket
                ? projectedSpan(*projectedPacket, {0.8, -0.6, 0.0},
                                {0.8, 0.6, 0.0}, projection, metrics)
                : std::nullopt;
        const auto pivotWorld =
            projectedPacket
                ? transformOverlayPoint(
                      projectedPacket->components[0].model, {})
                : std::nullopt;
        const auto pivot =
            pivotWorld ? projectLogical(*pivotWorld, projection, metrics)
                       : std::nullopt;
        passed &= expect(
            silhouette && hitRegion && pivot &&
                std::abs(*silhouette - 50.0) <= 3.0 &&
                std::abs(pivot->x - metrics.logical.extent.width * 0.5) <= 2.0 &&
                std::abs(pivot->y - metrics.logical.extent.height * 0.5) <= 2.0 &&
                (!baselineHitSpan ||
                 std::abs(*hitRegion - *baselineHitSpan) <= 3.0),
            label);
      };
  verifyOrthographicSize(
      ortho,
      "orthographic projection measures a 50-pixel silhouette and stable hit region");
  EditorViewportMetrics orthoZoom = ortho;
  orthoZoom.orthographicVerticalSpan = 120.0;
  ++orthoZoom.projection.revision;
  verifyOrthographicSize(
      orthoZoom,
      "orthographic zoom preserves silhouette, center, and hit-region tolerances");
  verifyProjectedSize(
      perspectiveMetrics, 2.0,
      "near perspective depth projects the canonical silhouette to 50 pixels");
  verifyProjectedSize(
      perspectiveMetrics, 20.0,
      "far perspective depth projects the canonical silhouette to 50 pixels");

  EditorViewportMetrics resized = perspectiveMetrics;
  resized.logical.extent = {1200.0, 900.0};
  resized.physical = {{0u, 0u}, {1200u, 900u}};
  resized.depthPhysical = resized.physical;
  resized.colorAttachment = {1200u, 900u};
  resized.depthAttachment = resized.colorAttachment;
  ++resized.viewportGeneration;
  verifyProjectedSize(
      resized, 5.0,
      "resized viewport preserves silhouette, center, and hit-region tolerances");
  passed &= expect(!computeGizmoConstantScale(
                        perspectiveMetrics,
                        viewDepthSnapshot(perspectiveMetrics, 0.0),
                        geometry.canonicalVisualDiameter()) &&
                       !computeGizmoConstantScale(
                           perspectiveMetrics,
                           viewDepthSnapshot(perspectiveMetrics, -1.0),
                           geometry.canonicalVisualDiameter()) &&
                       !computeGizmoConstantScale(
                           perspectiveMetrics,
                           viewDepthSnapshot(perspectiveMetrics, 1.0), 0.0),
                   "behind-camera and invalid canonical sizes hide explicitly");
  GizmoViewDepthSnapshot staleDepth =
      viewDepthSnapshot(perspectiveMetrics, 5.0);
  ++staleDepth.generation.view.revision;
  passed &= expect(
      !computeGizmoConstantScale(perspectiveMetrics, staleDepth,
                                 geometry.canonicalVisualDiameter()) &&
          !buildGizmoDrawPacket(geometry, attachment(), perspectiveMetrics,
                                staleDepth),
      "stale view-depth generation cannot be relabeled as a current draw");

  GizmoAttachment value = attachment();
  value.local.translation = {1.0, 2.0, 3.0};
  GizmoDrawSettings settings;
  settings.hovered = GizmoComponent::translateX;
  settings.active = GizmoComponent::rotateZ;
  settings.snapEnabled = true;
  settings.occlusionDither = true;
  const auto packet = buildGizmoDrawPacket(
      geometry, value, perspectiveMetrics,
      viewDepthSnapshot(perspectiveMetrics, 5.0), settings);
  passed &= expect(packet && finitePacket(*packet) &&
                       packet->generation.viewportGeneration == 17u &&
                       packet->generation.projection == viewport().projection &&
                       packet->basis == GizmoBasisToken::global &&
                       packet->depth == GizmoDepthToken::occlusionDither &&
                       packet->plane == GizmoPlaneToken::checkerDiscard &&
                       packet->snap == GizmoSnapToken::enabled &&
                       packet->components[0].state ==
                           GizmoVisualStateToken::hovered &&
                       packet->components[8].state ==
                           GizmoVisualStateToken::active &&
                       packet->components[0].color == GizmoColorToken::xAxis &&
                       packet->components[1].color == GizmoColorToken::yAxis &&
                       packet->components[2].color == GizmoColorToken::zAxis &&
                       packet->components[0].modelToken ==
                           GizmoModelToken::translate &&
                       packet->components[6].modelToken ==
                           GizmoModelToken::rotate &&
                       packet->components[9].modelToken ==
                           GizmoModelToken::scale,
                   "draw packet owns generation, state, depth, snap, and color tokens");
  if (packet) {
    for (std::size_t index = 0; index < kGizmoComponentCount; ++index) {
      passed &= expect(packet->components[index].visible &&
                           packet->components[index].range ==
                               geometry.ranges()[index] &&
                           near(packet->components[index].model.elements[3], 1.0) &&
                           near(packet->components[index].model.elements[7], 2.0) &&
                           near(packet->components[index].model.elements[11], 3.0),
                       "draw packet contains all 13 immutable component ranges/models");
    }
  }
  settings.visibility = GizmoVisibility::rotation;
  const auto rotationOnly = buildGizmoDrawPacket(
      geometry, value, perspectiveMetrics,
      viewDepthSnapshot(perspectiveMetrics, 5.0), settings);
  passed &= expect(rotationOnly && !rotationOnly->components[0].visible &&
                       rotationOnly->components[6].visible &&
                       !rotationOnly->components[9].visible,
                   "visibility mask hides complete operation families");
  settings.visible = false;
  passed &= expect(!buildGizmoDrawPacket(
                        geometry, value, perspectiveMetrics,
                        viewDepthSnapshot(perspectiveMetrics, 5.0), settings),
                   "hidden gizmo emits no draw packet");
  return passed;
}

bool testActionAndTerminalModel() {
  bool passed = true;
  const GizmoAttachment value = attachment();
  const GizmoInteractionState idle = attachGizmo(value);
  const GizmoActionResolution begun =
      beginGizmoDrag(idle, value, 101u, GizmoComponent::translateX);
  passed &= expect(idle.phase == GizmoInteractionPhase::attachedIdle &&
                       begun.state.phase == GizmoInteractionPhase::dragging &&
                       begun.state.captureHeld && begun.actionCount == 1u &&
                       begun.actions[0].kind == GizmoActionKind::beginPreview &&
                       begun.actions[0].nodeId == value.nodeId &&
                       begun.actions[0].documentRevision ==
                           value.documentRevision &&
                       begun.actions[0].selectionRevision ==
                           value.selectionRevision,
                   "begin action binds stable node and revision fences");
  GizmoLocalTransform preview = value.local;
  preview.translation = {1.0, 2.0, 3.0};
  const GizmoActionResolution updated =
      updateGizmoPreview(begun.state, value, preview);
  passed &= expect(updated.actionCount == 1u &&
                       updated.actions[0].kind ==
                           GizmoActionKind::updatePreview &&
                       updated.actions[0].transactionId == 101u &&
                       updated.actions[0].translation == preview.translation &&
                       updated.state.retainedPreview == preview,
                   "preview action is pure data for the active fenced transaction");
  GizmoAttachment stale = value;
  ++stale.documentRevision;
  const GizmoActionResolution staleDetached =
      updateGizmoPreview(updated.state, stale, preview);
  passed &= expect(
      staleDetached.state.phase == GizmoInteractionPhase::detached &&
          !staleDetached.state.captureHeld &&
          !staleDetached.state.fence.nodeId.valid() &&
          staleDetached.state.lastTerminalTransactionId == 101u &&
          actionCount(staleDetached,
                      GizmoActionKind::detachRetainingPreview) == 1u &&
          actionCount(staleDetached, GizmoActionKind::releaseCapture) == 1u &&
          actionCount(staleDetached, GizmoActionKind::commitPreviewOnce) == 0u &&
          staleDetached.actions[0].translation == preview.translation,
      "stale fence detaches once, retains accepted preview, and cannot commit");
  const GizmoActionResolution staleLateRelease = resolveGizmoTerminalBatch(
      staleDetached.state,
      batch({GizmoTerminalEventKind::deliveredRelease}));
  passed &= expect(staleLateRelease.actionCount == 0u &&
                       staleLateRelease.state == staleDetached.state,
                   "release after stale detach is a no-history no-op");
  GizmoLocalTransform nonFinitePreview = preview;
  nonFinitePreview.translation.x =
      std::numeric_limits<double>::quiet_NaN();
  const GizmoActionResolution rejectedPreview =
      updateGizmoPreview(updated.state, value, nonFinitePreview);
  passed &= expect(rejectedPreview.actionCount == 0u &&
                       rejectedPreview.state == updated.state,
                   "non-finite preview rejects without changing accepted state");

  const GizmoActionResolution ordinary = resolveGizmoTerminalBatch(
      updated.state, batch({GizmoTerminalEventKind::deliveredRelease}));
  passed &= expect(ordinary.state.phase ==
                           GizmoInteractionPhase::attachedIdle &&
                       ordinary.state.lastTerminalTransactionId == 101u &&
                       actionCount(ordinary, GizmoActionKind::commitPreviewOnce) ==
                           1u &&
                       actionCount(ordinary, GizmoActionKind::releaseCapture) ==
                           1u &&
                       ordinary.actions[0].terminal ==
                           GizmoTerminalToken::commitHistoryOnce &&
                       ordinary.actions[0].translation == preview.translation,
                   "ordinary delivered release emits exactly one commit terminal");
  const GizmoActionResolution late = resolveGizmoTerminalBatch(
      ordinary.state, batch({GizmoTerminalEventKind::deliveredRelease}));
  passed &= expect(late.actionCount == 0u && late.state == ordinary.state,
                   "late or repeated release is an exact no-op");

  const GizmoActionResolution escapeOnly = resolveGizmoTerminalBatch(
      updated.state, batch({GizmoTerminalEventKind::escapePressed}));
  passed &= expect(escapeOnly.state.phase == GizmoInteractionPhase::detached &&
                       actionCount(escapeOnly,
                                   GizmoActionKind::commitPreviewOnce) == 0u &&
                       actionCount(escapeOnly,
                                   GizmoActionKind::clearSelection) == 1u &&
                       actionCount(escapeOnly,
                                   GizmoActionKind::detachRetainingPreview) == 1u &&
                       escapeOnly.actions[1].terminal ==
                           GizmoTerminalToken::noHistory &&
                       escapeOnly.actions[1].translation == preview.translation &&
                       !escapeOnly.state.fence.nodeId.valid(),
                   "O05 Escape detaches while retaining preview with no history");
  const GizmoActionResolution escapeThenRelease = resolveGizmoTerminalBatch(
      updated.state,
      batch({GizmoTerminalEventKind::escapePressed,
             GizmoTerminalEventKind::deliveredRelease}));
  const GizmoActionResolution releaseThenEscape = resolveGizmoTerminalBatch(
      updated.state,
      batch({GizmoTerminalEventKind::deliveredRelease,
             GizmoTerminalEventKind::escapePressed}));
  passed &= expect(escapeThenRelease == releaseThenEscape &&
                       escapeThenRelease.state.phase ==
                           GizmoInteractionPhase::detached &&
                       actionCount(escapeThenRelease,
                                   GizmoActionKind::commitPreviewOnce) == 0u &&
                       actionCount(escapeThenRelease,
                                   GizmoActionKind::detachRetainingPreview) == 1u,
                   "O06 both same-batch event orders resolve to one no-history terminal");

  const GizmoActionResolution priorRelease = ordinary;
  const GizmoActionResolution laterEscape = resolveGizmoTerminalBatch(
      priorRelease.state, batch({GizmoTerminalEventKind::escapePressed}));
  passed &= expect(laterEscape.state.phase == GizmoInteractionPhase::detached &&
                       actionCount(laterEscape,
                                   GizmoActionKind::clearSelection) == 1u &&
                       actionCount(laterEscape,
                                   GizmoActionKind::commitPreviewOnce) == 0u &&
                       laterEscape.state.lastTerminalTransactionId == 101u,
                   "prior-frame commit remains terminal and later Escape only clears");

  const GizmoActionResolution focusLost = resolveGizmoTerminalBatch(
      updated.state,
      batch({GizmoTerminalEventKind::focusLost,
             GizmoTerminalEventKind::suppressedPhysicalMouseUp}));
  passed &= expect(focusLost.state.phase ==
                           GizmoInteractionPhase::draggingLatched &&
                       !focusLost.state.captureHeld &&
                       focusLost.state.activeComponent == GizmoComponent::none &&
                       actionCount(focusLost,
                                   GizmoActionKind::commitPreviewOnce) == 0u &&
                       actionCount(focusLost,
                                   GizmoActionKind::latchRetainingPreview) == 1u,
                   "O07 focus loss without delivered release latches with no commit");
  const GizmoActionResolution suppressed = resolveGizmoTerminalBatch(
      focusLost.state,
      batch({GizmoTerminalEventKind::suppressedPhysicalMouseUp}));
  passed &= expect(suppressed.actionCount == 0u &&
                       suppressed.state == focusLost.state,
                   "non-delivered physical mouse-up cannot synthesize commit");
  const GizmoActionResolution forgedLateRelease = resolveGizmoTerminalBatch(
      focusLost.state, batch({GizmoTerminalEventKind::deliveredRelease}));
  passed &= expect(forgedLateRelease.actionCount == 0u &&
                       forgedLateRelease.state == focusLost.state,
                   "latched focus-loss state rejects a forged delivered release");
  for (const bool releaseFirst : {false, true}) {
    const GizmoActionResolution focusAndRelease = resolveGizmoTerminalBatch(
        updated.state,
        releaseFirst
            ? batch({GizmoTerminalEventKind::deliveredRelease,
                     GizmoTerminalEventKind::focusLost})
            : batch({GizmoTerminalEventKind::focusLost,
                     GizmoTerminalEventKind::deliveredRelease}));
    passed &= expect(
        focusAndRelease.state.phase == GizmoInteractionPhase::draggingLatched &&
            !focusAndRelease.state.captureHeld &&
            focusAndRelease.state.activeComponent == GizmoComponent::none &&
            actionCount(focusAndRelease, GizmoActionKind::latchRetainingPreview) ==
                1u &&
            actionCount(focusAndRelease, GizmoActionKind::commitPreviewOnce) ==
                0u,
        "O07 focus loss takes precedence over a same-batch delivered release");
  }
  const GizmoActionResolution clearLatch = resolveGizmoTerminalBatch(
      focusLost.state, batch({GizmoTerminalEventKind::escapePressed}));
  passed &= expect(clearLatch.state.phase == GizmoInteractionPhase::detached &&
                       actionCount(clearLatch,
                                   GizmoActionKind::detachRetainingPreview) == 1u &&
                       actionCount(clearLatch,
                                   GizmoActionKind::commitPreviewOnce) == 0u,
                   "Escape clears a latched drag without history");
  const GizmoActionResolution repeatedEscape = resolveGizmoTerminalBatch(
      clearLatch.state, batch({GizmoTerminalEventKind::escapePressed}));
  passed &= expect(repeatedEscape.actionCount == 0u &&
                       repeatedEscape.state == clearLatch.state,
                   "repeated Escape after detach is idempotent");

  GizmoTerminalBatch invalid = batch({GizmoTerminalEventKind::escapePressed,
                                      GizmoTerminalEventKind::deliveredRelease});
  invalid.events[1].sequence = invalid.events[0].sequence;
  const GizmoActionResolution invalidResolution =
      resolveGizmoTerminalBatch(updated.state, invalid);
  passed &= expect(invalidResolution.actionCount == 0u &&
                       invalidResolution.state == updated.state,
                   "non-ordered terminal batches fail closed without action");
  return passed;
}

bool testFixedSeedFinitenessProperties() {
  bool passed = true;
  std::uint64_t state = 0x9e3779b97f4a7c15ull;
  const auto sample = [&state]() {
    state ^= state << 7u;
    state ^= state >> 9u;
    state ^= state << 8u;
    return static_cast<double>(state & 0xffffu) / 65535.0;
  };
  for (std::size_t iteration = 0; iteration < 256u; ++iteration) {
    GizmoAttachment value = attachment();
    const double sx = (sample() < 0.5 ? -1.0 : 1.0) * (0.25 + sample() * 8.0);
    const double sy = (sample() < 0.5 ? -1.0 : 1.0) * (0.25 + sample() * 8.0);
    const double sz = (sample() < 0.5 ? -1.0 : 1.0) * (0.25 + sample() * 8.0);
    value.parentWorld = parentMatrix(sample() * 6.0, {sx, sy, sz},
                                     {sample() * 100.0, sample() * -100.0,
                                      sample() * 50.0});
    value.parentWorld.elements[1] += (sample() - 0.5) * 0.2;
    value.parentWorld.elements[6] += (sample() - 0.5) * 0.2;
    value.local.translation = {sample() * 10.0, sample() * -10.0,
                               sample() * 5.0};
    value.local.scale = {(sample() - 0.5) * 4.0,
                         (sample() - 0.5) * 4.0,
                         (sample() - 0.5) * 4.0};
    const auto basis = deriveGizmoBasis(value);
    passed &= expect(basis && finiteOverlayVector(basis->pivotWorld) &&
                         finiteOverlayVector(basis->axes[0]) &&
                         finiteOverlayVector(basis->axes[1]) &&
                         finiteOverlayVector(basis->axes[2]) &&
                         std::isfinite(basis->parentDeterminant) &&
                         std::isfinite(basis->parentConditionEstimate),
                     "fixed-seed finite parent property never emits NaN/Inf");
  }
  return passed;
}

} // namespace

int main() {
  bool passed = true;
  passed &= testGeometryIdentityAndRanges();
  passed &= testRayConstructionAndProjectionConventions();
  passed &= testRayPlaneAndAllComponentHits();
  passed &= testParentBasisAndConditioning();
  passed &= testFreshDocumentRevisionZeroIsRenderable();
  passed &= testTranslationSolutionsAndSnapping();
  passed &= testRotationAndScaleSolutions();
  passed &= testSnapMatrix();
  passed &= testConstantSizeAndDrawPacket();
  passed &= testActionAndTerminalModel();
  passed &= testFixedSeedFinitenessProperties();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
