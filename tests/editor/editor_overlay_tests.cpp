#include <mgif/editor/EditorOverlay.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

static_assert(demo::editor::kEditorOverlayGizmoHandleCount == 13u);
static_assert(demo::editor::kEditorOverlayMaximumDrawCommandCount == 15u);
static_assert(std::is_copy_constructible_v<demo::editor::EditorOverlayFrame>);
static_assert(std::is_same_v<
              decltype(std::declval<const demo::editor::EditorOverlayFrame &>()
                           .commands()),
              std::span<const demo::editor::EditorOverlayDrawCommand>>);

namespace {

using namespace demo::editor;

bool expect(bool condition, std::string_view message) {
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
  return near(left.x, right.x, tolerance) && near(left.y, right.y, tolerance);
}

bool near(const glm::vec3 &left, const glm::vec3 &right,
          float tolerance = 1.0e-5f) {
  return near(left.x, right.x, tolerance) && near(left.y, right.y, tolerance) &&
         near(left.z, right.z, tolerance);
}

bool near(const glm::vec4 &left, const glm::vec4 &right,
          float tolerance = 1.0e-5f) {
  return near(left.x, right.x, tolerance) && near(left.y, right.y, tolerance) &&
         near(left.z, right.z, tolerance) && near(left.w, right.w, tolerance);
}

EditorOverlayViewport viewport(float logicalWidth = 800.0f,
                               float logicalHeight = 600.0f,
                               float scaleX = 1.0f, float scaleY = 1.0f,
                               std::uint32_t physicalWidth = 800u,
                               std::uint32_t physicalHeight = 600u) {
  return EditorOverlayViewport{
      glm::vec2(logicalWidth, logicalHeight), glm::vec2(scaleX, scaleY),
      EditorOverlayPixelRect{{0u, 0u}, {physicalWidth, physicalHeight}},
      EditorOverlayPixelExtent{physicalWidth, physicalHeight},
      EditorOverlayPixelExtent{physicalWidth, physicalHeight}};
}

EditorOverlayInput gizmoInput() {
  EditorOverlayInput input;
  input.viewport = viewport();
  input.gizmo.visible = true;
  input.gizmo.frame.pivotWorld = glm::vec3(1.0f, 2.0f, 3.0f);
  input.gizmo.projection = EditorGridProjection::Perspective;
  input.gizmo.viewSpaceDepth = 10.0f;
  input.gizmo.projectionScaleY = 2.0f;
  input.gizmo.nominalLogicalPixels = 50.0f;
  return input;
}

EditorOverlayInput completeInput() {
  EditorOverlayInput input = gizmoInput();
  input.grid.visible = true;
  input.grid.math.cameraPosition = glm::vec3(10.0f, 10.0f, 20.0f);
  input.grid.math.cameraBackward = glm::vec3(0.0f, 1.0f, 0.0f);
  input.grid.math.projection = EditorGridProjection::Perspective;
  input.grid.math.baseUnit = 1.0f;
  input.grid.math.lineCount = 151u;
  return input;
}

const EditorOverlayGizmoHandleCommand *handleAt(const EditorOverlayFrame &frame,
                                                std::size_t index) {
  if (index >= frame.commands().size()) {
    return nullptr;
  }
  return std::get_if<EditorOverlayGizmoHandleCommand>(
      &frame.commands()[index].payload);
}

const EditorOverlayGizmoHandleCommand *
findHandle(const EditorOverlayFrame &frame, TransformGizmoComponent component) {
  for (const EditorOverlayDrawCommand &command : frame.commands()) {
    if (const auto *handle =
            std::get_if<EditorOverlayGizmoHandleCommand>(&command.payload);
        handle && handle->component == component) {
      return handle;
    }
  }
  return nullptr;
}

glm::vec4 highlighted(glm::vec4 color) {
  for (glm::length_t channel = 0; channel < 3; ++channel) {
    const float bright = std::clamp(color[channel] * 2.5f, 0.0f, 1.0f);
    color[channel] = color[channel] * 0.1f + bright * 0.9f;
  }
  return color;
}

bool testAllHandlesAndStableColors() {
  constexpr std::array components{
      TransformGizmoComponent::TranslateX,
      TransformGizmoComponent::TranslateY,
      TransformGizmoComponent::TranslateZ,
      TransformGizmoComponent::TranslateXY,
      TransformGizmoComponent::TranslateXZ,
      TransformGizmoComponent::TranslateYZ,
      TransformGizmoComponent::RotateX,
      TransformGizmoComponent::RotateY,
      TransformGizmoComponent::RotateZ,
      TransformGizmoComponent::ScaleX,
      TransformGizmoComponent::ScaleY,
      TransformGizmoComponent::ScaleZ,
      TransformGizmoComponent::ScaleUniform,
  };
  constexpr std::array geometry{
      EditorOverlayGizmoGeometryKind::TranslationAxis,
      EditorOverlayGizmoGeometryKind::TranslationAxis,
      EditorOverlayGizmoGeometryKind::TranslationAxis,
      EditorOverlayGizmoGeometryKind::TranslationPlane,
      EditorOverlayGizmoGeometryKind::TranslationPlane,
      EditorOverlayGizmoGeometryKind::TranslationPlane,
      EditorOverlayGizmoGeometryKind::RotationRing,
      EditorOverlayGizmoGeometryKind::RotationRing,
      EditorOverlayGizmoGeometryKind::RotationRing,
      EditorOverlayGizmoGeometryKind::ScaleAxis,
      EditorOverlayGizmoGeometryKind::ScaleAxis,
      EditorOverlayGizmoGeometryKind::ScaleAxis,
      EditorOverlayGizmoGeometryKind::UniformScale,
  };
  const std::array colors{
      glm::vec4(0.89f, 0.294f, 0.369f, 1.0f),
      glm::vec4(0.45f, 0.819f, 0.263f, 1.0f),
      glm::vec4(0.25f, 0.451f, 0.851f, 1.0f),
      glm::vec4(0.78f, 0.78f, 0.25f, 0.45f),
      glm::vec4(0.78f, 0.25f, 0.78f, 0.45f),
      glm::vec4(0.25f, 0.78f, 0.78f, 0.45f),
      glm::vec4(0.89f, 0.294f, 0.369f, 1.0f),
      glm::vec4(0.45f, 0.819f, 0.263f, 1.0f),
      glm::vec4(0.25f, 0.451f, 0.851f, 1.0f),
      glm::vec4(0.89f, 0.294f, 0.369f, 1.0f),
      glm::vec4(0.45f, 0.819f, 0.263f, 1.0f),
      glm::vec4(0.25f, 0.451f, 0.851f, 1.0f),
      glm::vec4(0.65f, 0.65f, 0.65f, 1.0f),
  };

  const auto frame = buildEditorOverlayFrame(gizmoInput());
  bool passed = true;
  passed &= expect(frame && frame->commands().size() == components.size(),
                   "all visibility emits exactly thirteen handle commands");
  if (!frame) {
    return false;
  }
  for (std::size_t index = 0; index < components.size(); ++index) {
    const EditorOverlayDrawCommand &draw = frame->commands()[index];
    const auto *handle = handleAt(*frame, index);
    passed &= expect(
        handle && draw.order == index &&
            draw.depthMode == EditorOverlayDepthMode::XRaySolid &&
            draw.blendMode == EditorOverlayBlendMode::Opaque &&
            draw.helperDepthTest && draw.helperDepthWrite &&
            handle->component == components[index] &&
            handle->geometry == geometry[index] &&
            handle->interaction == EditorOverlayInteractionState::Normal &&
            near(handle->localAxis,
                 transformGizmoComponentAxis(components[index])) &&
            near(handle->color, colors[index]),
        "each handle has its stable order, geometry, axis, and color");

    EditorOverlayInput hoveredInput = gizmoInput();
    hoveredInput.gizmo.hovered = components[index];
    const auto hoveredFrame = buildEditorOverlayFrame(hoveredInput);
    const auto *hoveredHandle =
        hoveredFrame ? findHandle(*hoveredFrame, components[index]) : nullptr;
    EditorOverlayInput activeInput = gizmoInput();
    activeInput.gizmo.active = components[index];
    const auto activeFrame = buildEditorOverlayFrame(activeInput);
    const auto *activeHandle =
        activeFrame ? findHandle(*activeFrame, components[index]) : nullptr;
    passed &= expect(
        hoveredHandle && activeHandle &&
            hoveredHandle->interaction ==
                EditorOverlayInteractionState::Hovered &&
            activeHandle->interaction ==
                EditorOverlayInteractionState::Active &&
            near(hoveredHandle->color, highlighted(colors[index])) &&
            near(activeHandle->color, highlighted(colors[index])) &&
            near(hoveredHandle->color.w, colors[index].w) &&
            near(activeHandle->color.w, colors[index].w),
        "every handle supports deterministic hover and active state data");
  }
  passed &=
      expect(frame->gizmoPixelScale &&
                 near(frame->gizmoPixelScale->targetLogicalPixels, 50.0f) &&
                 near(frame->gizmoPixelScale->targetPhysicalPixelsY, 50.0f) &&
                 near(frame->gizmoPixelScale->worldScale, 5.0f / 6.0f) &&
                 frame->commands().front().order == 0u,
             "gizmo packet carries its projection-aware constant-pixel scale");
  const auto *first = handleAt(*frame, 0u);
  passed &= expect(
      first &&
          near(glm::vec3(first->localToWorld[0]),
               glm::vec3(5.0f / 6.0f, 0.0f, 0.0f)) &&
          near(glm::vec3(first->localToWorld[3]), glm::vec3(1.0f, 2.0f, 3.0f)),
      "handle model uses the shared pivot and uniform world scale");

  const EditorOverlayGizmoGeometryStyle &style = frame->gizmoGeometry;
  passed &= expect(
      near(style.shaftRadius, 0.02f) && near(style.shaftLength, 1.0f) &&
          near(style.coneRadius, 0.06f) && near(style.coneHeight, 0.25f) &&
          style.coneSegments == 16u && near(style.ringRadius, 1.3f) &&
          near(style.ringTubeRadius, 0.02f) && style.ringMajorSegments == 64u &&
          style.ringMinorSegments == 16u && near(style.scaleCubeSize, 0.15f) &&
          near(style.scaleAxisLength, 1.67f) &&
          near(style.uniformScaleMultiplier, 2.0f) &&
          near(style.planeSize, 0.22f) && near(style.planeOffset, 0.5f) &&
          near(style.planeAlpha, 0.45f),
      "geometry packet freezes every canonical handle dimension");
  return passed;
}

bool testGridVisibilityClippingAndOrdering() {
  const auto first = buildEditorOverlayFrame(completeInput());
  const auto second = buildEditorOverlayFrame(completeInput());
  bool passed = true;
  passed &= expect(first && second && first->commands().size() == 15u &&
                       second->commands().size() == 15u,
                   "grid plus all handles fits the fixed command capacity");
  if (!first || !second) {
    return false;
  }

  const auto *grid =
      std::get_if<EditorOverlayGridLinesCommand>(&first->commands()[0].payload);
  const auto *axes =
      std::get_if<EditorOverlayGridAxesCommand>(&first->commands()[1].payload);
  passed &= expect(
      grid && axes && first->commands()[0].order == 0u &&
          first->commands()[0].depthMode ==
              EditorOverlayDepthMode::SceneOccluded &&
          first->commands()[0].blendMode == EditorOverlayBlendMode::Additive &&
          !first->commands()[0].helperDepthTest &&
          !first->commands()[0].helperDepthWrite &&
          first->commands()[1].order == 1u &&
          first->commands()[1].depthMode ==
              EditorOverlayDepthMode::AlwaysVisible &&
          first->commands()[1].blendMode == EditorOverlayBlendMode::AlphaOver &&
          !first->commands()[1].helperDepthTest &&
          !first->commands()[1].helperDepthWrite &&
          grid->vertexCount == 1812u && axes->vertexCount == 6u &&
          near(grid->parameters.center, glm::vec2(10.0f, 20.0f)) &&
          near(grid->parameters.clipExtent, glm::vec2(7500.0f)) &&
          near(grid->clipMinimum, glm::vec2(-7500.0f)) &&
          near(grid->clipMaximum, glm::vec2(7500.0f)) &&
          grid->visibleLevels[0].levelIndex == 2u &&
          near(grid->visibleLevels[0].step, 100.0f) &&
          near(grid->visibleLevels[0].center, glm::vec2(0.0f)) &&
          near(grid->visibleLevels[0].clipMinimum, glm::vec2(-7500.0f)) &&
          near(grid->visibleLevels[0].clipMaximum, glm::vec2(7500.0f)) &&
          near(grid->visibleLevels[0].perspectiveAlpha, 0.4f) &&
          grid->visibleLevels[1].levelIndex == 1u &&
          near(grid->visibleLevels[1].clipMinimum,
               glm::vec2(-740.0f, -730.0f)) &&
          near(grid->visibleLevels[1].clipMaximum, glm::vec2(760.0f, 770.0f)) &&
          near(grid->visibleLevels[1].perspectiveAlpha, 0.3f) &&
          grid->visibleLevels[2].levelIndex == 0u &&
          near(grid->visibleLevels[2].clipMinimum, glm::vec2(-65.0f, -55.0f)) &&
          near(grid->visibleLevels[2].clipMaximum, glm::vec2(85.0f, 95.0f)) &&
          near(grid->visibleLevels[2].perspectiveAlpha, 0.2f) &&
          near(axes->clipExtent, glm::vec2(7500.0f)),
      "grid is clipped deterministically before always-visible axes");
  passed &= expect(
      axes && near(axes->colors[0], glm::vec4(0.619f, 0.235f, 0.290f, 1.0f)) &&
          near(axes->colors[1], glm::vec4(0.357f, 0.482f, 0.185f, 1.0f)) &&
          near(axes->colors[2], glm::vec4(0.231f, 0.357f, 0.518f, 1.0f)),
      "grid axis palette is stable and ordered X, Y, Z");

  for (std::size_t index = 0; index < first->commands().size(); ++index) {
    passed &= expect(first->commands()[index].order == index &&
                         second->commands()[index].order == index &&
                         first->commands()[index].payload.index() ==
                             second->commands()[index].payload.index(),
                     "repeated packets retain identical draw ordering");
  }
  for (std::size_t index = 2u; index < first->commands().size(); ++index) {
    const auto *left = handleAt(*first, index);
    const auto *right = handleAt(*second, index);
    passed &= expect(left && right && left->component == right->component &&
                         near(left->color, right->color),
                     "repeated handle payloads retain deterministic identity");
  }

  EditorOverlayInput rotationOnly = gizmoInput();
  rotationOnly.gizmo.visibility = TransformGizmoVisibility::Rotation;
  const auto rotation = buildEditorOverlayFrame(rotationOnly);
  passed &=
      expect(rotation && rotation->commands().size() == 3u &&
                 handleAt(*rotation, 0u) &&
                 handleAt(*rotation, 0u)->component ==
                     TransformGizmoComponent::RotateX &&
                 handleAt(*rotation, 1u)->component ==
                     TransformGizmoComponent::RotateY &&
                 handleAt(*rotation, 2u)->component ==
                     TransformGizmoComponent::RotateZ,
             "visibility filters groups without changing canonical order");

  EditorOverlayInput translationOnly = gizmoInput();
  translationOnly.gizmo.visibility = TransformGizmoVisibility::Translation;
  const auto translation = buildEditorOverlayFrame(translationOnly);
  EditorOverlayInput scaleOnly = gizmoInput();
  scaleOnly.gizmo.visibility = TransformGizmoVisibility::Scale;
  const auto scale = buildEditorOverlayFrame(scaleOnly);
  EditorOverlayInput translationAndScale = gizmoInput();
  translationAndScale.gizmo.visibility =
      TransformGizmoVisibility::Translation | TransformGizmoVisibility::Scale;
  const auto combined = buildEditorOverlayFrame(translationAndScale);
  passed &= expect(
      translation && translation->commands().size() == 6u && scale &&
          scale->commands().size() == 4u && combined &&
          combined->commands().size() == 10u &&
          handleAt(*translation, 0u)->component ==
              TransformGizmoComponent::TranslateX &&
          handleAt(*translation, 5u)->component ==
              TransformGizmoComponent::TranslateYZ &&
          handleAt(*scale, 0u)->component == TransformGizmoComponent::ScaleX &&
          handleAt(*scale, 3u)->component ==
              TransformGizmoComponent::ScaleUniform &&
          handleAt(*combined, 6u)->component == TransformGizmoComponent::ScaleX,
      "translation, scale, and combined visibility emit exact handle sets");

  EditorOverlayInput noHandles = completeInput();
  noHandles.gizmo.visibility = TransformGizmoVisibility::None;
  const auto gridOnly = buildEditorOverlayFrame(noHandles);
  passed &= expect(gridOnly && gridOnly->commands().size() == 2u &&
                       !gridOnly->gizmoPixelScale,
                   "empty gizmo visibility leaves only the ordered grid draws");
  return passed;
}

bool testHoverActiveAndDepthModes() {
  EditorOverlayInput input = gizmoInput();
  input.gizmo.hovered = TransformGizmoComponent::TranslateXY;
  input.gizmo.active = TransformGizmoComponent::RotateZ;
  const auto frame = buildEditorOverlayFrame(input);
  bool passed = true;
  const auto *hoveredHandle =
      frame ? findHandle(*frame, TransformGizmoComponent::TranslateXY)
            : nullptr;
  const auto *activeHandle =
      frame ? findHandle(*frame, TransformGizmoComponent::RotateZ) : nullptr;
  const auto *normalHandle =
      frame ? findHandle(*frame, TransformGizmoComponent::ScaleUniform)
            : nullptr;
  passed &= expect(
      hoveredHandle && activeHandle && normalHandle &&
          hoveredHandle->interaction ==
              EditorOverlayInteractionState::Hovered &&
          activeHandle->interaction == EditorOverlayInteractionState::Active &&
          normalHandle->interaction == EditorOverlayInteractionState::Normal &&
          near(hoveredHandle->color,
               highlighted(glm::vec4(0.78f, 0.78f, 0.25f, 0.45f))) &&
          near(activeHandle->color,
               highlighted(glm::vec4(0.25f, 0.451f, 0.851f, 1.0f))) &&
          near(normalHandle->color, glm::vec4(0.65f, 0.65f, 0.65f, 1.0f)) &&
          near(hoveredHandle->color.w, 0.45f),
      "hover and active retain distinct state tokens and the same brightening "
      "rule");

  input.gizmo.hovered = TransformGizmoComponent::RotateX;
  input.gizmo.active = TransformGizmoComponent::RotateX;
  const auto activeWins = buildEditorOverlayFrame(input);
  const auto *same =
      activeWins ? findHandle(*activeWins, TransformGizmoComponent::RotateX)
                 : nullptr;
  passed &=
      expect(same && same->interaction == EditorOverlayInteractionState::Active,
             "active state deterministically wins a same-component tie");

  constexpr std::array modes{
      EditorOverlayDepthMode::DepthTested,
      EditorOverlayDepthMode::XRaySolid,
      EditorOverlayDepthMode::XRayDithered,
  };
  for (const EditorOverlayDepthMode mode : modes) {
    EditorOverlayInput depthInput = gizmoInput();
    depthInput.gizmo.depthMode = mode;
    const auto depthFrame = buildEditorOverlayFrame(depthInput);
    bool allMatch = depthFrame.has_value();
    if (depthFrame) {
      for (const EditorOverlayDrawCommand &draw : depthFrame->commands()) {
        allMatch &= draw.depthMode == mode;
      }
    }
    passed &= expect(allMatch,
                     "every supported depth/x-ray mode reaches all handles");
  }

  const auto hoverColor =
      editorOverlayComponentColor(TransformGizmoComponent::TranslateX,
                                  EditorOverlayInteractionState::Hovered);
  const auto activeColor =
      editorOverlayComponentColor(TransformGizmoComponent::TranslateX,
                                  EditorOverlayInteractionState::Active);
  passed &=
      expect(hoverColor && activeColor && near(*hoverColor, *activeColor) &&
                 !editorOverlayComponentColor(TransformGizmoComponent::None) &&
                 !editorOverlayComponentColor(
                     static_cast<TransformGizmoComponent>(255u)) &&
                 !editorOverlayComponentColor(
                     TransformGizmoComponent::TranslateX,
                     static_cast<EditorOverlayInteractionState>(255u)),
             "color lookup rejects non-handles and invalid state tokens");
  return passed;
}

bool testDpiViewportAndParentScaling() {
  EditorOverlayInput oneX = gizmoInput();
  const auto base = buildEditorOverlayFrame(oneX);

  EditorOverlayInput twoX = gizmoInput();
  twoX.viewport = viewport(800.0f, 600.0f, 2.0f, 2.0f, 1600u, 1200u);
  twoX.viewport.sceneDepthExtent = {800u, 600u};
  const auto highDpi = buildEditorOverlayFrame(twoX);

  EditorOverlayInput larger = gizmoInput();
  larger.viewport = viewport(1600.0f, 1200.0f, 2.0f, 2.0f, 3200u, 2400u);
  const auto largeViewport = buildEditorOverlayFrame(larger);

  EditorOverlayInput fractionalDpi = gizmoInput();
  fractionalDpi.viewport = viewport(800.0f, 600.0f, 1.25f, 1.5f, 1000u, 900u);
  const auto fractionalFrame = buildEditorOverlayFrame(fractionalDpi);

  EditorOverlayInput farther = gizmoInput();
  farther.gizmo.viewSpaceDepth = 20.0f;
  const auto farFrame = buildEditorOverlayFrame(farther);

  bool passed = true;
  passed &= expect(
      base && highDpi && largeViewport && fractionalFrame && farFrame &&
          base->gizmoPixelScale && highDpi->gizmoPixelScale &&
          largeViewport->gizmoPixelScale && fractionalFrame->gizmoPixelScale &&
          farFrame->gizmoPixelScale &&
          near(base->gizmoPixelScale->targetPhysicalPixelsY, 50.0f) &&
          near(highDpi->gizmoPixelScale->targetPhysicalPixelsY, 100.0f) &&
          near(base->gizmoPixelScale->worldScale,
               highDpi->gizmoPixelScale->worldScale) &&
          near(base->gizmoPixelScale->worldScale,
               fractionalFrame->gizmoPixelScale->worldScale) &&
          near(largeViewport->gizmoPixelScale->worldScale,
               base->gizmoPixelScale->worldScale * 0.5f) &&
          near(farFrame->gizmoPixelScale->worldScale,
               base->gizmoPixelScale->worldScale * 2.0f) &&
          near(highDpi->physicalPixelsPerLogicalPixel, glm::vec2(2.0f)) &&
          near(highDpi->sceneDepthPixelsPerPhysicalPixel, glm::vec2(0.5f)),
      "logical size stays stable across DPI while viewport height and depth "
      "scale correctly");
  passed &= expect(
      base && base->depthConvention == EditorOverlayDepthConvention::Reverse &&
          near(base->viewProjection[0], glm::vec4(1.0f, 0.0f, 0.0f, 0.0f)),
      "packet carries backend-neutral camera/depth convention data");

  if (base && highDpi && base->gizmoPixelScale && highDpi->gizmoPixelScale) {
    const float baseProjected =
        base->gizmoPixelScale->worldScale * oneX.gizmo.projectionScaleY *
        static_cast<float>(oneX.viewport.physicalRenderArea.extent.height) /
        (2.0f * oneX.gizmo.viewSpaceDepth * oneX.viewport.framebufferScale.y);
    const float dpiProjected =
        highDpi->gizmoPixelScale->worldScale * twoX.gizmo.projectionScaleY *
        static_cast<float>(twoX.viewport.physicalRenderArea.extent.height) /
        (2.0f * twoX.gizmo.viewSpaceDepth * twoX.viewport.framebufferScale.y);
    passed &= expect(near(baseProjected, 50.0f) && near(dpiProjected, 50.0f),
                     "projecting the scale back produces fifty logical pixels");
  }

  EditorOverlayInput ortho = gizmoInput();
  ortho.gizmo.projection = EditorGridProjection::Orthographic;
  ortho.gizmo.viewSpaceDepth = 4.0f;
  const auto orthoNear = buildEditorOverlayFrame(ortho);
  ortho.gizmo.viewSpaceDepth = 400.0f;
  const auto orthoFar = buildEditorOverlayFrame(ortho);
  passed &=
      expect(orthoNear && orthoFar && orthoNear->gizmoPixelScale &&
                 orthoFar->gizmoPixelScale &&
                 near(orthoNear->gizmoPixelScale->worldScale,
                      orthoFar->gizmoPixelScale->worldScale) &&
                 near(orthoNear->gizmoPixelScale->worldScale, 1.0f / 12.0f),
             "orthographic constant-pixel scale is distance-independent");

  EditorOverlayInput flippedProjection = gizmoInput();
  flippedProjection.gizmo.projectionScaleY = -2.0f;
  const auto flippedFrame = buildEditorOverlayFrame(flippedProjection);
  passed &= expect(flippedFrame && flippedFrame->gizmoPixelScale && base &&
                       base->gizmoPixelScale &&
                       near(flippedFrame->gizmoPixelScale->worldScale,
                            base->gizmoPixelScale->worldScale),
                   "vertical projection sign does not change logical size");

  EditorOverlayInput parent = gizmoInput();
  parent.gizmo.frame.parentWorld = glm::mat4(1.0f);
  parent.gizmo.frame.parentWorld[0][0] = -2.0f;
  parent.gizmo.frame.parentWorld[1][1] = 3.0f;
  parent.gizmo.frame.parentWorld[2][2] = 0.5f;
  const auto parentFrame = buildEditorOverlayFrame(parent);
  const auto *parentHandle = parentFrame ? handleAt(*parentFrame, 0u) : nullptr;
  passed &= expect(parentHandle && parentFrame->gizmoPixelScale &&
                       near(glm::vec3(parentHandle->localToWorld[0]),
                            glm::vec3(-5.0f / 6.0f, 0.0f, 0.0f)) &&
                       near(glm::vec3(parentHandle->localToWorld[1]),
                            glm::vec3(0.0f, 5.0f / 6.0f, 0.0f)) &&
                       near(glm::vec3(parentHandle->localToWorld[2]),
                            glm::vec3(0.0f, 0.0f, 5.0f / 6.0f)),
                   "display basis preserves reflected parent direction but "
                   "removes scale magnitude");

  EditorOverlayInput offset = gizmoInput();
  offset.viewport.physicalRenderArea.origin = {100u, 50u};
  offset.viewport.colorTargetExtent = {1000u, 700u};
  const auto offsetFrame = buildEditorOverlayFrame(offset);
  passed &= expect(
      offsetFrame && offsetFrame->scissor.origin.x == 100u &&
          offsetFrame->scissor.origin.y == 50u &&
          offsetFrame->scissor.extent.width == 800u &&
          offsetFrame->scissor.extent.height == 600u,
      "validated physical render area is preserved as the exact scissor");
  return passed;
}

bool testInvalidInputsFailClosed() {
  bool passed = true;

  EditorOverlayInput empty;
  empty.viewport = viewport();
  empty.grid.math.baseUnit = std::numeric_limits<float>::quiet_NaN();
  empty.gizmo.viewSpaceDepth = std::numeric_limits<float>::quiet_NaN();
  empty.gizmo.depthMode = static_cast<EditorOverlayDepthMode>(255u);
  const auto inactive = buildEditorOverlayFrame(empty);
  passed &= expect(inactive && inactive->commands().empty(),
                   "disabled producers do not make inactive settings fatal");

  EditorOverlayInput invalid = gizmoInput();
  invalid.camera.viewProjection[0][0] = std::numeric_limits<float>::quiet_NaN();
  passed &= expect(!buildEditorOverlayFrame(invalid),
                   "non-finite view-projection data rejects");
  invalid = gizmoInput();
  invalid.camera.depthConvention =
      static_cast<EditorOverlayDepthConvention>(255u);
  passed &= expect(!buildEditorOverlayFrame(invalid),
                   "unknown depth convention rejects");
  invalid = gizmoInput();
  invalid.viewport.logicalExtent.x = std::numeric_limits<float>::quiet_NaN();
  passed &= expect(!buildEditorOverlayFrame(invalid),
                   "non-finite logical viewport extent rejects");
  invalid = gizmoInput();
  invalid.viewport.logicalExtent.y = 0.0f;
  passed &= expect(!buildEditorOverlayFrame(invalid),
                   "zero logical viewport extent rejects");
  invalid = gizmoInput();
  invalid.viewport.framebufferScale = glm::vec2(2.0f);
  passed &= expect(!buildEditorOverlayFrame(invalid),
                   "stale logical-to-physical scale rejects");
  invalid = gizmoInput();
  invalid.viewport.physicalRenderArea.extent.width = 0u;
  passed &= expect(!buildEditorOverlayFrame(invalid),
                   "zero physical render extent rejects");
  invalid = gizmoInput();
  invalid.viewport.physicalRenderArea.origin.x = 1u;
  passed &=
      expect(!buildEditorOverlayFrame(invalid),
             "render area outside its color target rejects without overflow");
  invalid = gizmoInput();
  invalid.viewport.sceneDepthExtent.height = 0u;
  passed &= expect(!buildEditorOverlayFrame(invalid),
                   "zero scene-depth extent rejects");

  invalid = completeInput();
  invalid.grid.math.lineCount = 150u;
  passed &= expect(!buildEditorOverlayFrame(invalid),
                   "invalid grid math rejects the entire packet");
  invalid = completeInput();
  invalid.grid.math.projection = static_cast<EditorGridProjection>(255u);
  passed &= expect(!buildEditorOverlayFrame(invalid),
                   "invalid grid projection rejects");
  invalid = completeInput();
  invalid.grid.math.cameraPosition.x = std::numeric_limits<float>::max();
  invalid.grid.math.cameraPosition.y = std::numeric_limits<float>::max();
  invalid.grid.math.cameraBackward = glm::vec3(-1.0f, 0.0f, 0.0f);
  passed &=
      expect(!buildEditorOverlayFrame(invalid),
             "overflowing grid clipping rejects instead of emitting infinity");

  invalid = gizmoInput();
  invalid.gizmo.frame.pivotWorld.x = std::numeric_limits<float>::infinity();
  passed &= expect(!buildEditorOverlayFrame(invalid),
                   "non-finite gizmo pivot rejects");
  invalid = gizmoInput();
  invalid.gizmo.frame.parentWorld[1] = invalid.gizmo.frame.parentWorld[0];
  passed &= expect(!buildEditorOverlayFrame(invalid),
                   "singular parent basis rejects");
  invalid = gizmoInput();
  invalid.gizmo.frame.parentWorld[0][3] = 1.0f;
  passed &= expect(!buildEditorOverlayFrame(invalid),
                   "non-affine parent matrix rejects");
  invalid = gizmoInput();
  invalid.gizmo.viewSpaceDepth = 0.0f;
  passed &= expect(!buildEditorOverlayFrame(invalid),
                   "pivot at or behind the camera rejects");
  invalid = gizmoInput();
  invalid.gizmo.projectionScaleY = 0.0f;
  passed &= expect(!buildEditorOverlayFrame(invalid),
                   "degenerate vertical projection scale rejects");
  invalid = gizmoInput();
  invalid.gizmo.nominalLogicalPixels = std::numeric_limits<float>::infinity();
  passed &= expect(!buildEditorOverlayFrame(invalid),
                   "non-finite target size rejects");
  invalid = gizmoInput();
  invalid.gizmo.projection = static_cast<EditorGridProjection>(255u);
  passed &= expect(!buildEditorOverlayFrame(invalid),
                   "invalid gizmo projection rejects");
  invalid = gizmoInput();
  invalid.gizmo.visibility = static_cast<TransformGizmoVisibility>(0x80u);
  passed &= expect(!buildEditorOverlayFrame(invalid),
                   "unknown visibility bits reject");
  invalid = gizmoInput();
  invalid.gizmo.hovered = static_cast<TransformGizmoComponent>(255u);
  passed &= expect(!buildEditorOverlayFrame(invalid),
                   "unknown hover component rejects");
  invalid = gizmoInput();
  invalid.gizmo.active = static_cast<TransformGizmoComponent>(255u);
  passed &= expect(!buildEditorOverlayFrame(invalid),
                   "unknown active component rejects");
  invalid = gizmoInput();
  invalid.gizmo.visibility = TransformGizmoVisibility::Rotation;
  invalid.gizmo.hovered = TransformGizmoComponent::TranslateX;
  passed &= expect(!buildEditorOverlayFrame(invalid),
                   "hidden hover state rejects as an inconsistent snapshot");
  invalid = gizmoInput();
  invalid.gizmo.depthMode = EditorOverlayDepthMode::SceneOccluded;
  passed &= expect(!buildEditorOverlayFrame(invalid),
                   "grid-only scene occlusion mode rejects for gizmo handles");
  invalid = gizmoInput();
  invalid.gizmo.depthMode = static_cast<EditorOverlayDepthMode>(255u);
  passed &=
      expect(!buildEditorOverlayFrame(invalid), "unknown depth mode rejects");
  invalid = completeInput();
  invalid.gizmo.projection = EditorGridProjection::Orthographic;
  passed &= expect(!buildEditorOverlayFrame(invalid),
                   "one frame cannot mix grid and gizmo camera projections");
  return passed;
}

} // namespace

int main() {
  bool passed = true;
  passed &= testAllHandlesAndStableColors();
  passed &= testGridVisibilityClippingAndOrdering();
  passed &= testHoverActiveAndDepthModes();
  passed &= testDpiViewportAndParentScaling();
  passed &= testInvalidInputsFailClosed();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
