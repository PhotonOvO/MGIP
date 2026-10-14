#include <mgif/editor/EditorGridMath.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

using namespace demo::editor;

bool expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
  }
  return condition;
}

bool near(float left, float right, float tolerance = 1.0e-5f) {
  return std::isfinite(left) && std::isfinite(right) &&
         std::abs(left - right) <= tolerance;
}

bool near(const glm::vec2 &left, const glm::vec2 &right,
          float tolerance = 1.0e-5f) {
  return near(left.x, right.x, tolerance) &&
         near(left.y, right.y, tolerance);
}

EditorGridMathInput gridInput(glm::vec3 cameraPosition,
                              glm::vec3 cameraBackward,
                              EditorGridProjection projection,
                              float baseUnit = 1.0f,
                              std::uint32_t lineCount = 151u) {
  return EditorGridMathInput{cameraPosition, cameraBackward, projection,
                             baseUnit, lineCount};
}

bool testPerspectiveParameters() {
  bool passed = true;
  const auto vertical = computeEditorGridParameters(gridInput(
      glm::vec3(10.0f, 10.0f, 20.0f), glm::vec3(0.0f, 1.0f, 0.0f),
      EditorGridProjection::Perspective));
  passed &= expect(vertical && near(vertical->distanceToFloor, 10.0f) &&
                       near(vertical->level, 1.0f) &&
                       near(vertical->levelFraction, 0.0f) &&
                       near(vertical->center, glm::vec2(10.0f, 20.0f)),
                   "vertical perspective view matches floor distance and center");
  passed &= expect(vertical && vertical->steps[0] == 1.0f &&
                       vertical->steps[1] == 10.0f &&
                       vertical->steps[2] == 100.0f &&
                       vertical->steps[3] == 1000.0f &&
                       vertical->coarsestLevelIndex == 2u &&
                       vertical->visibleLevelIndices ==
                           std::array<std::uint32_t, 3>{2u, 1u, 0u} &&
                       near(vertical->clipExtent, glm::vec2(7500.0f)),
                   "perspective level maps to three adjacent decade steps");

  const auto horizon = computeEditorGridParameters(gridInput(
      glm::vec3(10.0f, 10.0f, 20.0f), glm::vec3(1.0f, 0.0f, 0.0f),
      EditorGridProjection::Perspective));
  passed &= expect(horizon && near(horizon->distanceToFloor, 10.0f) &&
                       near(horizon->center, glm::vec2(0.0f, 20.0f)),
                   "near-horizontal perspective view uses finite height fallback");

  const auto angled = computeEditorGridParameters(gridInput(
      glm::vec3(10.0f, 10.0f, 20.0f),
      glm::vec3(0.0f, 0.5f, 0.8660254038f),
      EditorGridProjection::Perspective));
  passed &= expect(angled && near(angled->distanceToFloor, 15.0f) &&
                       near(angled->center.x, 10.0f) &&
                       near(angled->center.y, 7.009619f, 1.0e-4f),
                   "angled perspective view preserves the locked blended distance");
  return passed;
}

bool testOrthographicParametersAndSnapping() {
  bool passed = true;
  const auto first = computeEditorGridParameters(gridInput(
      glm::vec3(4.0f, -2.0f, 6.0f), glm::vec3(0.0f, 0.0f, 1.0f),
      EditorGridProjection::Orthographic));
  const auto second = computeEditorGridParameters(gridInput(
      glm::vec3(4.0f, -2.0f, 6.0f), glm::vec3(1.0f, 0.0f, 0.0f),
      EditorGridProjection::Orthographic));
  passed &= expect(first && second && near(first->distanceToFloor, 2.0f) &&
                       near(first->level, std::log10(2.0f)) &&
                       near(first->center, glm::vec2(4.0f, 6.0f)) &&
                       first->center == second->center &&
                       first->level == second->level,
                   "orthographic LOD uses height and camera XZ only");

  const auto halfCenter = computeEditorGridParameters(gridInput(
      glm::vec3(0.5f, 1.0f, -0.5f), glm::vec3(0.0f, 0.0f, 1.0f),
      EditorGridProjection::Orthographic));
  passed &= expect(halfCenter &&
                       halfCenter->snappedCenters[0] ==
                           glm::vec2(1.0f, -1.0f),
                   "grid centers snap signed half steps deterministically");
  return passed;
}

bool testGridUnitAndLodBoundaries() {
  bool passed = true;
  const auto minimum = computeEditorGridParameters(gridInput(
      glm::vec3(0.0f, 0.001f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f),
      EditorGridProjection::Orthographic, kEditorGridMinimumUnit,
      kEditorGridMinimumLineCount));
  passed &= expect(minimum && minimum->baseUnit == 0.001f &&
                       minimum->level == 0.0f &&
                       minimum->steps[0] == 0.001f &&
                       minimum->lineCount == kEditorGridMinimumLineCount,
                   "minimum grid unit and line extent are accepted");

  const auto maximum = computeEditorGridParameters(gridInput(
      glm::vec3(0.0f, 1000.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f),
      EditorGridProjection::Orthographic, kEditorGridMaximumUnit,
      kEditorGridMaximumLineCount));
  passed &= expect(maximum && maximum->baseUnit == 1000.0f &&
                       maximum->level == 0.0f &&
                       maximum->steps[3] == 1000000.0f &&
                       maximum->lineCount == kEditorGridMaximumLineCount,
                   "maximum grid unit and line extent remain finite");

  const auto below = computeEditorGridParameters(gridInput(
      glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f),
      EditorGridProjection::Orthographic, 0.0f));
  const auto above = computeEditorGridParameters(gridInput(
      glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f),
      EditorGridProjection::Orthographic, 2000.0f));
  passed &= expect(below && below->baseUnit == kEditorGridMinimumUnit &&
                       above && above->baseUnit == kEditorGridMaximumUnit,
                   "finite out-of-range units clamp to the locked UI bounds");

  const auto levelOne = computeEditorGridParameters(gridInput(
      glm::vec3(0.0f, 10.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f),
      EditorGridProjection::Orthographic));
  const auto levelTwo = computeEditorGridParameters(gridInput(
      glm::vec3(0.0f, 100.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f),
      EditorGridProjection::Orthographic));
  const auto levelThree = computeEditorGridParameters(gridInput(
      glm::vec3(0.0f, 1000.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f),
      EditorGridProjection::Orthographic));
  const auto saturated = computeEditorGridParameters(gridInput(
      glm::vec3(0.0f, 1.0e8f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f),
      EditorGridProjection::Orthographic));
  passed &= expect(levelOne && near(levelOne->level, 1.0f) && levelTwo &&
                       near(levelTwo->level, 2.0f) && levelThree &&
                       near(levelThree->level, 3.0f) && saturated &&
                       near(saturated->level, 3.999f) &&
                       saturated->coarsestLevelIndex == 3u,
                   "LOD decade boundaries and upper saturation are exact");
  return passed;
}

bool testInvalidInputsAndExtents() {
  bool passed = true;
  const auto valid = [](std::uint32_t lineCount) {
    return computeEditorGridParameters(gridInput(
        glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f),
        EditorGridProjection::Orthographic, 1.0f, lineCount));
  };
  passed &= expect(!valid(9u) && !valid(10u) && !valid(12u) &&
                       !valid(502u) && !valid(503u),
                   "out-of-range and even grid line extents reject");

  EditorGridMathInput nonFinite = gridInput(
      glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f),
      EditorGridProjection::Perspective);
  nonFinite.cameraPosition.x = std::numeric_limits<float>::infinity();
  passed &= expect(!computeEditorGridParameters(nonFinite),
                   "non-finite camera positions reject");
  nonFinite = gridInput(glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f),
                        EditorGridProjection::Perspective);
  passed &= expect(!computeEditorGridParameters(nonFinite),
                   "zero camera basis rejects");
  nonFinite = gridInput(
      glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f),
      EditorGridProjection::Perspective,
      std::numeric_limits<float>::quiet_NaN());
  passed &= expect(!computeEditorGridParameters(nonFinite),
                   "non-finite grid units reject");

  const float maximum = std::numeric_limits<float>::max();
  const auto overflowingCenter = computeEditorGridParameters(gridInput(
      glm::vec3(maximum, maximum, 0.0f), glm::vec3(-1.0f, 0.0f, 0.0f),
      EditorGridProjection::Perspective));
  passed &= expect(!overflowingCenter,
                   "overflowing perspective center fails without infinity");
  return passed;
}

} // namespace

int main() {
  bool passed = true;
  passed &= testPerspectiveParameters();
  passed &= testOrthographicParametersAndSnapping();
  passed &= testGridUnitAndLodBoundaries();
  passed &= testInvalidInputsAndExtents();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
