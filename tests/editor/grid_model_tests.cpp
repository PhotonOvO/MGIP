#include "render/editor/GridModel.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>
#include <type_traits>

namespace {

using namespace demo::editor;

static_assert(kGridDecadeCount == 4u);
static_assert(kGridActiveLodCount == 3u);
static_assert(std::is_trivially_copyable_v<GridLinePacket>);
static_assert(std::is_trivially_copyable_v<GridDrawPacket>);

bool expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
  }
  return condition;
}

bool near(double left, double right, double tolerance = 1.0e-9) {
  return std::isfinite(left) && std::isfinite(right) &&
         std::abs(left - right) <= tolerance;
}

bool near(OverlayVec2 left, OverlayVec2 right,
          double tolerance = 1.0e-9) {
  return near(left.x, right.x, tolerance) &&
         near(left.y, right.y, tolerance);
}

bool finite(OverlayVec3 value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

EditorViewportMetrics viewport(
    EditorProjectionKind kind = EditorProjectionKind::perspective) {
  EditorViewportMetrics result;
  result.logical = {{0.0, 0.0}, {800.0, 600.0}};
  result.framebufferLogicalOrigin = {0.0, 0.0};
  result.physical = {{0u, 0u}, {800u, 600u}};
  result.depthPhysical = result.physical;
  result.colorAttachment = {800u, 600u};
  result.depthAttachment = result.colorAttachment;
  result.framebufferScale = {1.0, 1.0};
  result.viewportGeneration = 41u;
  result.projection = {43u, 0x1234u};
  result.view = {47u, 0x5678u};
  result.convention = {EditorClipDepthRange::zeroToOne, true, true};
  result.projectionKind = kind;
  if (kind == EditorProjectionKind::perspective) {
    result.verticalFieldOfViewRadians = 1.0;
  } else {
    result.orthographicVerticalSpan = 20.0;
  }
  return result;
}

GridModelSettings settings(double baseUnit = 1.0,
                           std::uint32_t lineCount =
                               kGridDefaultLineCount) {
  GridModelSettings result;
  result.enabled = true;
  result.baseUnit = baseUnit;
  result.lineCount = lineCount;
  return result;
}

GridModelInput input(OverlayVec3 cameraPosition,
                     OverlayVec3 cameraForward = {0.0, -1.0, 0.0}) {
  return {cameraPosition, cameraForward};
}

bool validAlpha(double value) {
  return std::isfinite(value) && value >= 0.0 && value <= 1.0;
}

double lineCoordinate(const GridLinePacket &line) {
  return line.kind == GridLineKind::parallelX ? line.start.z : line.start.x;
}

bool testPerspectiveAndOrthographicParameters() {
  const GridModelResult perspective = buildGridModel(
      settings(), input({10.0, 10.0, 20.0}), viewport());
  bool passed = expect(perspective.status == GridModelStatus::ready &&
                           perspective.draw.enabled &&
                           near(perspective.draw.distanceToFloor, 10.0) &&
                           near(perspective.draw.center, {10.0, 20.0}) &&
                           perspective.draw.baseUnit == 1.0 &&
                           perspective.draw.decadeSteps ==
                               std::array<double, 4>{1.0, 10.0, 100.0,
                                                     1000.0},
                       "vertical perspective uses camera height and four decades");
  passed &= expect(
      perspective.draw.activeLods[0].levelIndex == 2u &&
          perspective.draw.activeLods[1].levelIndex == 1u &&
          perspective.draw.activeLods[2].levelIndex == 0u &&
          perspective.draw.activeLods[0].step == 100.0 &&
          perspective.draw.activeLods[1].step == 10.0 &&
          perspective.draw.activeLods[2].step == 1.0,
      "perspective level activates three unique adjacent LODs coarse-to-fine");

  const GridModelResult angled = buildGridModel(
      settings(), input({10.0, 10.0, 20.0}, {0.866025403784, -0.5, 0.0}),
      viewport());
  passed &= expect(angled.status == GridModelStatus::ready &&
                       near(angled.draw.distanceToFloor, 15.0, 1.0e-8) &&
                       near(angled.draw.center.x,
                            10.0 + 0.866025403784 * 15.0, 1.0e-8) &&
                       near(angled.draw.center.y, 20.0),
                   "angled perspective blends floor-ray distance and height");
  const GridModelResult horizon = buildGridModel(
      settings(), input({10.0, 10.0, 20.0}, {1.0, 0.0, 0.0}), viewport());
  passed &= expect(horizon.status == GridModelStatus::ready &&
                       near(horizon.draw.distanceToFloor, 10.0) &&
                       near(horizon.draw.center, {20.0, 20.0}),
                   "near-horizontal perspective uses finite height fallback");

  const GridModelResult orthographicA = buildGridModel(
      settings(), input({4.0, -2.0, 6.0}, {0.0, 0.0, 1.0}),
      viewport(EditorProjectionKind::orthographic));
  const GridModelResult orthographicB = buildGridModel(
      settings(), input({4.0, -2.0, 6.0}, {1.0, 0.0, 0.0}),
      viewport(EditorProjectionKind::orthographic));
  passed &= expect(orthographicA.status == GridModelStatus::ready &&
                       orthographicB.status == GridModelStatus::ready &&
                       near(orthographicA.draw.distanceToFloor, 2.0) &&
                       near(orthographicA.draw.center, {4.0, 6.0}) &&
                       orthographicA.draw.distanceToFloor ==
                           orthographicB.draw.distanceToFloor &&
                       orthographicA.draw.center == orthographicB.draw.center,
                   "orthographic LOD uses height and camera XZ independent of view angle");
  return passed;
}

bool testBaseUnitLodAndSignedSnapping() {
  bool passed = true;
  const GridModelResult minimum = buildGridModel(
      settings(kGridMinimumBaseUnit),
      input({0.5 * kGridMinimumBaseUnit, kGridMinimumBaseUnit,
             -0.5 * kGridMinimumBaseUnit}),
      viewport(EditorProjectionKind::orthographic));
  passed &= expect(minimum.status == GridModelStatus::ready &&
                       minimum.draw.baseUnit == kGridMinimumBaseUnit &&
                       minimum.draw.decadeSteps ==
                           std::array<double, 4>{0.001, 0.01, 0.1, 1.0} &&
                       near(minimum.draw.activeLods[2].center,
                            {0.001, -0.001}, 1.0e-12),
                   "minimum unit and signed half centers snap away from zero");
  const GridModelResult maximum = buildGridModel(
      settings(kGridMaximumBaseUnit),
      input({500.0, 1000.0, -500.0}),
      viewport(EditorProjectionKind::orthographic));
  passed &= expect(maximum.status == GridModelStatus::ready &&
                       maximum.draw.baseUnit == kGridMaximumBaseUnit &&
                       maximum.draw.decadeSteps ==
                           std::array<double, 4>{1000.0, 10000.0,
                                                 100000.0, 1000000.0},
                   "maximum base unit keeps all four decades finite");

  const GridModelResult below = buildGridModel(
      settings(0.0), input({0.0, 1.0, 0.0}),
      viewport(EditorProjectionKind::orthographic));
  const GridModelResult above = buildGridModel(
      settings(2000.0), input({0.0, 1.0, 0.0}),
      viewport(EditorProjectionKind::orthographic));
  passed &= expect(below.status == GridModelStatus::ready &&
                       below.draw.baseUnit == kGridMinimumBaseUnit &&
                       above.status == GridModelStatus::ready &&
                       above.draw.baseUnit == kGridMaximumBaseUnit,
                   "finite UI overflow clamps to the locked base-unit range");

  for (double height : std::array<double, 4>{1.0, 10.0, 100.0, 1000.0}) {
    const GridModelResult boundary = buildGridModel(
        settings(), input({0.0, height, 0.0}),
        viewport(EditorProjectionKind::orthographic));
    passed &= expect(boundary.status == GridModelStatus::ready &&
                         boundary.draw.activeLods[0].levelIndex ==
                             (height < 100.0 ? 2u : 3u) &&
                         boundary.draw.activeLods[0].levelIndex ==
                             boundary.draw.activeLods[1].levelIndex + 1u &&
                         boundary.draw.activeLods[1].levelIndex ==
                             boundary.draw.activeLods[2].levelIndex + 1u,
                     "LOD decade boundaries keep three adjacent unique levels");
  }
  return passed;
}

bool testLineOrderingDedupAndFades() {
  const GridModelResult result = buildGridModel(
      settings(), input({10.0, 10.0, 20.0}), viewport());
  bool passed = expect(result.status == GridModelStatus::ready &&
                           !result.lines.empty() && result.axisCount == 2u &&
                           result.draw.generatedLineCount ==
                               result.lines.size() + result.axisCount &&
                           result.draw.generatedLineCount <=
                               kGridDefaultLineCount * 2u *
                                       kGridActiveLodCount +
                                   2u,
                       "default 151-line extent produces a bounded nonempty model");
  passed &= expect(result.axes[0].kind == GridLineKind::xAxis &&
                       result.axes[1].kind == GridLineKind::zAxis &&
                       result.axes[0].blend == GridBlendToken::alphaOver &&
                       result.axes[1].blend == GridBlendToken::alphaOver &&
                       result.axes[0].alpha == 1.0 &&
                       result.axes[1].alpha == 1.0,
                   "axes are separate alpha-over lines that remain on top");

  std::uint8_t previousLevel = result.draw.activeLods[0].levelIndex;
  GridLineKind previousKind = GridLineKind::parallelX;
  double previousCoordinate = -std::numeric_limits<double>::infinity();
  bool first = true;
  for (const GridLinePacket &line : result.lines) {
    const double expectedLength =
        result.draw.decadeSteps[line.lodIndex] *
        static_cast<double>(kGridDefaultLineCount - 1u);
    const double actualLength =
        std::hypot(line.end.x - line.start.x,
                   line.end.z - line.start.z);
    passed &= expect(finite(line.start) && finite(line.end) &&
                         line.blend == GridBlendToken::additive &&
                         (line.kind == GridLineKind::parallelX ||
                          line.kind == GridLineKind::parallelZ) &&
                         validAlpha(line.edgeAlpha) &&
                         validAlpha(line.angleAlpha) &&
                         validAlpha(line.distanceAlpha) &&
                         validAlpha(line.lodAlpha) && validAlpha(line.alpha) &&
                         std::abs(lineCoordinate(line)) > 1.0e-10 &&
                         near(actualLength, expectedLength, 1.0e-8),
                     "each LOD line uses its own finite extent without axis duplicates");
    if (!first) {
      const bool newLevel = line.lodIndex != previousLevel;
      const bool newKind = !newLevel && line.kind != previousKind;
      if (newLevel) {
        passed &= expect(line.lodIndex < previousLevel,
                         "line LOD groups are ordered coarse-to-fine");
        previousCoordinate = -std::numeric_limits<double>::infinity();
      } else if (newKind) {
        passed &= expect(previousKind == GridLineKind::parallelX &&
                             line.kind == GridLineKind::parallelZ,
                         "each LOD orders X-parallel lines before Z-parallel lines");
        previousCoordinate = -std::numeric_limits<double>::infinity();
      } else {
        passed &= expect(lineCoordinate(line) > previousCoordinate,
                         "coordinates are strictly ascending within each line group");
      }
    }
    previousLevel = line.lodIndex;
    previousKind = line.kind;
    previousCoordinate = lineCoordinate(line);
    first = false;
  }

  for (std::size_t left = 0; left < result.lines.size(); ++left) {
    for (std::size_t right = left + 1u; right < result.lines.size(); ++right) {
      if (result.lines[left].kind == result.lines[right].kind) {
        passed &= expect(!near(lineCoordinate(result.lines[left]),
                               lineCoordinate(result.lines[right]), 1.0e-10),
                         "coarser/finer coincident lines are emitted only once");
      }
    }
  }

  const GridModelResult horizon = buildGridModel(
      settings(), input({0.0, 10.0, 0.0}, {1.0, 0.0, 0.0}), viewport());
  passed &= expect(horizon.status == GridModelStatus::ready &&
                       !horizon.lines.empty() &&
                       horizon.lines.front().angleAlpha == 0.0 &&
                       horizon.lines.front().alpha == 0.0,
                   "edge-on view applies a finite zero angle fade");
  return passed;
}

bool testDeterminismAndGenerationBinding() {
  const GridModelSettings valueSettings = settings();
  const GridModelInput valueInput = input({12.5, 7.25, -33.0},
                                          {0.25, -0.75, 0.4});
  const EditorViewportMetrics metrics = viewport();
  const GridModelResult first =
      buildGridModel(valueSettings, valueInput, metrics);
  const GridModelResult second =
      buildGridModel(valueSettings, valueInput, metrics);
  bool passed = expect(first.status == GridModelStatus::ready &&
                           first.draw == second.draw &&
                           first.lines == second.lines &&
                           first.axes == second.axes &&
                           first.axisCount == second.axisCount &&
                           first.semanticDigest == 11163764098860648324ull &&
                           first.semanticDigest == second.semanticDigest,
                       "grid ordering, packets, and semantic digest are deterministic");
  passed &= expect(first.draw.generation.viewportGeneration ==
                           metrics.viewportGeneration &&
                       first.draw.generation.projection == metrics.projection &&
                       first.draw.generation.view == metrics.view,
                   "grid packet binds immutable viewport/projection/view generations");
  return passed;
}

bool testDisabledAndInvalidInputs() {
  bool passed = true;
  GridModelSettings disabled;
  const GridModelResult off = buildGridModel(
      disabled,
      input({std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0}, {}),
      EditorViewportMetrics{});
  passed &= expect(off.status == GridModelStatus::disabled && off.empty() &&
                       !off.draw.enabled && off.semanticDigest == 0u,
                   "grid off returns an empty disabled result before consuming inputs");

  const auto rejected = [&passed](GridModelSettings testSettings,
                                  GridModelInput testInput,
                                  EditorViewportMetrics testMetrics,
                                  std::string_view label) {
    const GridModelResult result =
        buildGridModel(testSettings, testInput, testMetrics);
    passed &= expect(result.status == GridModelStatus::invalidInput &&
                         result.empty() && !result.draw.enabled &&
                         result.semanticDigest == 0u,
                     label);
  };
  GridModelSettings invalidSettings = settings();
  invalidSettings.baseUnit = std::numeric_limits<double>::quiet_NaN();
  rejected(invalidSettings, input({0.0, 1.0, 0.0}), viewport(),
           "non-finite base unit returns an empty invalid result");
  invalidSettings = settings();
  invalidSettings.lineCount = 150u;
  rejected(invalidSettings, input({0.0, 1.0, 0.0}), viewport(),
           "even line extent returns an empty invalid result");
  invalidSettings = settings();
  invalidSettings.lineCount = kGridMaximumLineCount + 2u;
  rejected(invalidSettings, input({0.0, 1.0, 0.0}), viewport(),
           "unbounded line count returns an empty invalid result");
  invalidSettings = settings();
  invalidSettings.distanceFadeMultiplier = 0.0;
  rejected(invalidSettings, input({0.0, 1.0, 0.0}), viewport(),
           "nonpositive distance fade returns an empty invalid result");
  rejected(settings(), input({0.0, 1.0, 0.0}, {}), viewport(),
           "zero camera direction returns an empty invalid result");
  rejected(settings(),
           input({std::numeric_limits<double>::infinity(), 1.0, 0.0}),
           viewport(),
           "non-finite camera position returns an empty invalid result");
  EditorViewportMetrics invalidMetrics = viewport();
  invalidMetrics.viewportGeneration = 0u;
  rejected(settings(), input({0.0, 1.0, 0.0}), invalidMetrics,
           "invalid immutable viewport returns an empty invalid result");
  rejected(settings(),
           input({std::numeric_limits<double>::max(),
                  std::numeric_limits<double>::max(), 0.0},
                 {1.0, 0.0, 0.0}),
           viewport(),
           "overflowing perspective center fails before emitting infinity");
  rejected(settings(),
           input({1.0e308, 1.0, 0.0}, {0.0, -1.0, 0.0}),
           viewport(EditorProjectionKind::orthographic),
           "extreme finite center fails when spacing is below its representable ULP");
  return passed;
}

bool testLargeRepresentableCentersPreserveFineLods() {
  bool passed = true;
  for (const double center : {1.0e12, -1.0e12}) {
    GridModelSettings value = settings();
    value.baseUnit = 0.001;
    const GridModelResult result = buildGridModel(
        value, input({center, 1.0, 0.0}, {0.0, -1.0, 0.0}),
        viewport(EditorProjectionKind::orthographic));
    std::array<std::uint32_t, kGridActiveLodCount> parallelZCounts{};
    for (const GridLinePacket &line : result.lines) {
      if (line.kind != GridLineKind::parallelZ) {
        continue;
      }
      for (std::size_t lodIndex = 0; lodIndex < kGridActiveLodCount;
           ++lodIndex) {
        if (line.lodIndex == result.draw.activeLods[lodIndex].levelIndex) {
          ++parallelZCounts[lodIndex];
          break;
        }
      }
    }
    const bool retained =
        result.status == GridModelStatus::ready && result.draw.enabled &&
        parallelZCounts[0] > 0u && parallelZCounts[1] > 0u &&
        parallelZCounts[2] > 0u;
    if (!retained) {
      std::cerr << "large-center diagnostic center=" << center
                << " status=" << static_cast<unsigned>(result.status)
                << " counts=" << parallelZCounts[0] << ','
                << parallelZCounts[1] << ',' << parallelZCounts[2] << '\n';
    }
    passed &= expect(
        retained,
        "large representable positive/negative centers retain every fine LOD");
  }
  return passed;
}

} // namespace

int main() {
  bool passed = true;
  passed &= testPerspectiveAndOrthographicParameters();
  passed &= testBaseUnitLodAndSignedSnapping();
  passed &= testLineOrderingDedupAndFades();
  passed &= testDeterminismAndGenerationBinding();
  passed &= testDisabledAndInvalidInputs();
  passed &= testLargeRepresentableCentersPreserveFineLods();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
