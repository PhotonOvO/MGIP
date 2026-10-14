#include <mgif/editor/TransformGizmoMath.h>

#include <glm/gtc/matrix_transform.hpp>

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

bool near(const glm::vec3 &left, const glm::vec3 &right,
          float tolerance = 1.0e-5f) {
  return near(left.x, right.x, tolerance) &&
         near(left.y, right.y, tolerance) &&
         near(left.z, right.z, tolerance);
}

bool finite(const glm::vec3 &value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

TransformGizmoRay ray(glm::vec3 origin, glm::vec3 direction) {
  return TransformGizmoRay{origin, direction};
}

bool testSnapRounding() {
  bool passed = true;
  const auto positiveHalf = snapTransformGizmoValue(0.5f, 1.0f);
  const auto negativeHalf = snapTransformGizmoValue(-0.5f, 1.0f);
  const auto positiveOneAndHalf = snapTransformGizmoValue(1.5f, 1.0f);
  const auto negativeOneAndHalf = snapTransformGizmoValue(-1.5f, 1.0f);
  passed &= expect(positiveHalf && *positiveHalf == 1.0f,
                   "positive half-step snap rounds away from zero");
  passed &= expect(negativeHalf && *negativeHalf == -1.0f,
                   "negative half-step snap rounds away from zero");
  passed &= expect(positiveOneAndHalf && *positiveOneAndHalf == 2.0f &&
                       negativeOneAndHalf && *negativeOneAndHalf == -2.0f,
                   "signed one-and-a-half steps retain std::round semantics");

  const auto vector =
      snapTransformGizmoVector(glm::vec3(-0.5f, 0.5f, -1.5f), 1.0f);
  passed &= expect(vector && *vector == glm::vec3(-1.0f, 1.0f, -2.0f),
                   "vector snap applies the signed rule independently");
  passed &= expect(!snapTransformGizmoValue(1.0f, 0.0f) &&
                       !snapTransformGizmoValue(1.0f, -1.0f) &&
                       !snapTransformGizmoValue(
                           std::numeric_limits<float>::infinity(), 1.0f) &&
                       !snapTransformGizmoValue(
                           1.0f, std::numeric_limits<float>::quiet_NaN()),
                   "invalid snap inputs fail closed");
  return passed;
}

bool testRayConstructionAndPlaneIntersection() {
  bool passed = true;
  const glm::mat4 view = glm::lookAtRH(glm::vec3(0.0f, 0.0f, 5.0f),
                                       glm::vec3(0.0f),
                                       glm::vec3(0.0f, 1.0f, 0.0f));
  glm::mat4 projection = glm::perspectiveRH_ZO(
      glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
  projection[1][1] *= -1.0f;
  const auto centerRay = makeTransformGizmoRay(
      glm::vec2(50.0f), view, projection, glm::vec2(100.0f));
  passed &= expect(centerRay && finite(centerRay->origin) &&
                       finite(centerRay->direction) &&
                       near(centerRay->direction,
                            glm::vec3(0.0f, 0.0f, -1.0f), 1.0e-4f),
                   "perspective center mouse produces a finite forward ray");

  glm::mat4 orthographic =
      glm::orthoRH_ZO(-2.0f, 2.0f, -2.0f, 2.0f, 0.1f, 100.0f);
  orthographic[1][1] *= -1.0f;
  const auto orthographicCenter = makeTransformGizmoRay(
      glm::vec2(50.0f), view, orthographic, glm::vec2(100.0f));
  const auto orthographicCorner = makeTransformGizmoRay(
      glm::vec2(75.0f, 25.0f), view, orthographic,
      glm::vec2(100.0f));
  passed &= expect(
      orthographicCenter && orthographicCorner &&
          finite(orthographicCenter->origin) &&
          near(orthographicCenter->direction,
               orthographicCorner->direction, 1.0e-5f) &&
          !near(orthographicCenter->origin,
                orthographicCorner->origin, 1.0e-5f),
      "orthographic mouse rays keep parallel directions and shifted origins");

  glm::mat4 projectiveView = view;
  projectiveView[0][3] = 0.5f;
  passed &= expect(
      !makeTransformGizmoRay(glm::vec2(0.0f), view, projection,
                             glm::vec2(0.0f, 100.0f)) &&
          !makeTransformGizmoRay(glm::vec2(0.0f), view, glm::mat4(0.0f),
                                 glm::vec2(100.0f)) &&
          !makeTransformGizmoRay(glm::vec2(0.0f), projectiveView, projection,
                                 glm::vec2(100.0f)),
      "invalid viewport, projection, and non-affine view reject");

  const auto hit = intersectTransformGizmoRayPlane(
      ray(glm::vec3(0.0f, 2.0f, 0.0f),
          glm::vec3(0.0f, -2.0f, 0.0f)),
      glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
  passed &= expect(hit && near(hit->distance, 2.0f) &&
                       near(hit->position, glm::vec3(0.0f)),
                   "ray-plane distance uses a normalized ray direction");
  passed &= expect(
      !intersectTransformGizmoRayPlane(
          ray(glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f)),
          glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f)) &&
          !intersectTransformGizmoRayPlane(
              ray(glm::vec3(0.0f, 1.0f, 0.0f),
                  glm::vec3(1.0f, -1.0e-8f, 0.0f)),
              glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f)) &&
          !intersectTransformGizmoRayPlane(
              ray(glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f)),
              glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f)) &&
          !intersectTransformGizmoRayPlane(
              ray(glm::vec3(0.0f, -1.0f, 0.0f),
                  glm::vec3(0.0f, -1.0f, 0.0f)),
              glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f)),
      "parallel, near-parallel, zero-length, and behind-plane rays miss");
  return passed;
}

bool testHitClassification() {
  bool passed = true;
  const TransformGizmoHit axis = classifyTransformGizmoHit(
      ray(glm::vec3(0.8f, 0.0f, 3.0f), glm::vec3(0.0f, 0.0f, -1.0f)),
      {}, TransformGizmoVisibility::Translation);
  passed &= expect(axis.component == TransformGizmoComponent::TranslateX &&
                       axis.kind == TransformGizmoHitKind::Axis &&
                       axis.rayDistance > 0.0f,
                   "translation-axis bounds classify as an axis hit");

  const TransformGizmoHit plane = classifyTransformGizmoHit(
      ray(glm::vec3(0.5f, 0.5f, 3.0f), glm::vec3(0.0f, 0.0f, -1.0f)),
      {}, TransformGizmoVisibility::Translation);
  passed &= expect(plane.component == TransformGizmoComponent::TranslateXY &&
                       plane.kind == TransformGizmoHitKind::Plane,
                   "offset XY bounds classify as a plane hit");

  const TransformGizmoHit uniformScale = classifyTransformGizmoHit(
      ray(glm::vec3(0.15f, 0.15f, 3.0f),
          glm::vec3(0.0f, 0.0f, -1.0f)),
      {}, TransformGizmoVisibility::Scale);
  passed &= expect(
      uniformScale.component == TransformGizmoComponent::ScaleUniform &&
          uniformScale.kind == TransformGizmoHitKind::UniformScale,
      "center cube classifies as uniform scale without overlapping axis boxes");

  const TransformGizmoHit ring = classifyTransformGizmoHit(
      ray(glm::vec3(1.3f, 0.0f, 3.0f), glm::vec3(0.0f, 0.0f, -1.0f)),
      {}, TransformGizmoVisibility::Rotation);
  passed &= expect(ring.component == TransformGizmoComponent::RotateZ &&
                       ring.kind == TransformGizmoHitKind::Ring &&
                       near(ring.missDistance, 0.0f),
                   "rotation plane and radial tolerance classify a ring hit");

  TransformGizmoHitStyle invalidStyle;
  invalidStyle.translationPlaneSize = -1.0f;
  passed &= expect(!classifyTransformGizmoHit(
                        ray(glm::vec3(0.5f, 0.5f, 3.0f),
                            glm::vec3(0.0f, 0.0f, -1.0f)),
                        invalidStyle)
                        .hit() &&
                       !classifyTransformGizmoHit(
                            ray(glm::vec3(0.0f), glm::vec3(0.0f)))
                            .hit() &&
                       !classifyTransformGizmoHit(
                            ray(glm::vec3(0.8f, 0.0f, 3.0f),
                                glm::vec3(0.0f, 0.0f, -1.0f)),
                            {}, TransformGizmoVisibility::None)
                            .hit(),
                   "invalid extents, degenerate rays, and hidden components miss");
  return passed;
}

bool testTranslationAndParentSpace() {
  bool passed = true;
  const TransformGizmoRay start =
      ray(glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f, 0.0f, -1.0f));
  const TransformGizmoRay current =
      ray(glm::vec3(2.0f, 3.0f, 5.0f), glm::vec3(0.0f, 0.0f, -1.0f));
  const auto axis = computeTransformGizmoTranslationDelta(
      TransformGizmoComponent::TranslateX, start, current,
      glm::vec3(0.0f, 0.0f, 1.0f));
  passed &= expect(axis && near(axis->world, glm::vec3(2.0f, 0.0f, 0.0f)) &&
                       near(axis->local, glm::vec3(2.0f, 0.0f, 0.0f)),
                   "axis translation projects the drag onto its world axis");

  const auto plane = computeTransformGizmoTranslationDelta(
      TransformGizmoComponent::TranslateXY, start, current,
      glm::vec3(0.0f, 0.0f, 1.0f));
  passed &= expect(plane && near(plane->world, glm::vec3(2.0f, 3.0f, 0.0f)) &&
                       near(plane->local, glm::vec3(2.0f, 3.0f, 0.0f)),
                   "plane translation retains both in-plane components");
  passed &= expect(!computeTransformGizmoTranslationDelta(
                        TransformGizmoComponent::TranslateX, start, current,
                        glm::vec3(1.0f, 0.0f, 0.0f)),
                   "axis-parallel view direction rejects a degenerate drag plane");

  TransformGizmoFrame parentFrame;
  parentFrame.pivotWorld = glm::vec3(3.0f, 13.0f, 23.0f);
  parentFrame.parentWorld = glm::mat4(1.0f);
  parentFrame.parentWorld[0][0] = -2.0f;
  parentFrame.parentWorld[1][1] = 3.0f;
  parentFrame.parentWorld[2][2] = 4.0f;
  parentFrame.parentWorld[3] = glm::vec4(5.0f, 7.0f, 11.0f, 1.0f);
  const TransformGizmoRay parentStart =
      ray(parentFrame.pivotWorld + glm::vec3(0.0f, 0.0f, 20.0f),
          glm::vec3(0.0f, 0.0f, -1.0f));
  const TransformGizmoRay parentCurrent =
      ray(parentFrame.pivotWorld + glm::vec3(-2.0f, 0.0f, 20.0f),
          glm::vec3(0.0f, 0.0f, -1.0f));
  const auto parentDelta = computeTransformGizmoTranslationDelta(
      TransformGizmoComponent::TranslateX, parentStart, parentCurrent,
      glm::vec3(0.0f, 0.0f, 1.0f), parentFrame);
  passed &= expect(parentDelta && finite(parentDelta->world) &&
                       finite(parentDelta->local) &&
                       near(parentDelta->world,
                            glm::vec3(-2.0f, 0.0f, 0.0f)) &&
                       near(parentDelta->local,
                            glm::vec3(1.0f, 0.0f, 0.0f)),
                   "negative nonuniform parent scale yields a finite local delta");

  TransformGizmoFrame singular = parentFrame;
  singular.parentWorld[2][2] = 0.0f;
  passed &= expect(!computeTransformGizmoTranslationDelta(
                        TransformGizmoComponent::TranslateX, parentStart,
                        parentCurrent, glm::vec3(0.0f, 0.0f, 1.0f), singular),
                   "singular parent basis fails without emitting non-finite values");
  return passed;
}

bool testRotationAndScaleDeltas() {
  bool passed = true;
  const auto positiveRotation = computeTransformGizmoRotationDeltaDegrees(
      TransformGizmoComponent::RotateZ,
      ray(glm::vec3(1.0f, 0.0f, 5.0f), glm::vec3(0.0f, 0.0f, -1.0f)),
      ray(glm::vec3(0.0f, 1.0f, 5.0f), glm::vec3(0.0f, 0.0f, -1.0f)));
  const auto negativeRotation = computeTransformGizmoRotationDeltaDegrees(
      TransformGizmoComponent::RotateZ,
      ray(glm::vec3(1.0f, 0.0f, 5.0f), glm::vec3(0.0f, 0.0f, -1.0f)),
      ray(glm::vec3(0.0f, -1.0f, 5.0f), glm::vec3(0.0f, 0.0f, -1.0f)));
  passed &= expect(positiveRotation && near(*positiveRotation, 90.0f) &&
                       negativeRotation && near(*negativeRotation, -90.0f),
                   "ring drag computes stable signed rotation angles");
  passed &= expect(!computeTransformGizmoRotationDeltaDegrees(
                        TransformGizmoComponent::RotateZ,
                        ray(glm::vec3(0.0f, 0.0f, 5.0f),
                            glm::vec3(0.0f, 0.0f, -1.0f)),
                        ray(glm::vec3(0.0f, 1.0f, 5.0f),
                            glm::vec3(0.0f, 0.0f, -1.0f))),
                   "rotation starting at the pivot rejects its zero radial vector");

  TransformGizmoFrame parentFrame;
  parentFrame.pivotWorld = glm::vec3(3.0f, 13.0f, 23.0f);
  parentFrame.parentWorld = glm::mat4(1.0f);
  parentFrame.parentWorld[0][0] = -2.0f;
  parentFrame.parentWorld[1][1] = 3.0f;
  parentFrame.parentWorld[2][2] = 4.0f;
  const auto parentRotation = computeTransformGizmoRotationDeltaDegrees(
      TransformGizmoComponent::RotateX,
      ray(parentFrame.pivotWorld + glm::vec3(5.0f, 1.0f, 0.0f),
          glm::vec3(-1.0f, 0.0f, 0.0f)),
      ray(parentFrame.pivotWorld + glm::vec3(5.0f, 0.0f, 1.0f),
          glm::vec3(-1.0f, 0.0f, 0.0f)),
      parentFrame);
  passed &= expect(parentRotation && std::isfinite(*parentRotation) &&
                       near(*parentRotation, -90.0f),
                   "negative parent axis preserves a finite signed ring result");

  const glm::vec3 nonuniformNegativeScale(-2.0f, 3.0f, 4.0f);
  const auto axisScale = computeTransformGizmoScaleDelta(
      TransformGizmoComponent::ScaleX, nonuniformNegativeScale,
      glm::vec2(100.0f), glm::vec2(120.0f, 100.0f));
  passed &= expect(axisScale && near(axisScale->factor, 1.1f) &&
                       near(axisScale->value,
                            glm::vec3(-2.2f, 3.0f, 4.0f)),
                   "axis scale changes one signed nonuniform component");

  const auto uniformScale = computeTransformGizmoScaleDelta(
      TransformGizmoComponent::ScaleUniform, nonuniformNegativeScale,
      glm::vec2(100.0f), glm::vec2(120.0f, 80.0f));
  passed &= expect(uniformScale && near(uniformScale->factor, 1.2f) &&
                       near(uniformScale->value,
                            glm::vec3(-2.4f, 3.6f, 4.8f)),
                   "uniform scale preserves signs and nonuniform ratios");

  const auto clampedScale = computeTransformGizmoScaleDelta(
      TransformGizmoComponent::ScaleUniform, nonuniformNegativeScale,
      glm::vec2(0.0f), glm::vec2(-1000.0f, 1000.0f));
  passed &= expect(clampedScale && near(clampedScale->factor, 0.01f) &&
                       near(clampedScale->value,
                            glm::vec3(-0.02f, 0.03f, 0.04f)),
                   "scale factor retains the reference positive minimum");

  passed &= expect(!computeTransformGizmoScaleDelta(
                        TransformGizmoComponent::TranslateX,
                        nonuniformNegativeScale, glm::vec2(0.0f),
                        glm::vec2(1.0f)) &&
                       !computeTransformGizmoScaleDelta(
                            TransformGizmoComponent::ScaleUniform,
                            glm::vec3(std::numeric_limits<float>::max()),
                            glm::vec2(0.0f), glm::vec2(100.0f, 0.0f)),
                   "invalid component and overflowing scale previews fail closed");
  return passed;
}

} // namespace

int main() {
  bool passed = true;
  passed &= testSnapRounding();
  passed &= testRayConstructionAndPlaneIntersection();
  passed &= testHitClassification();
  passed &= testTranslationAndParentSpace();
  passed &= testRotationAndScaleDeltas();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
