#include "../../app/editor/EditorUiOsReplayPlan.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <string_view>

int main() {
  namespace os = demo::editor::testing::osreplay;
  const os::ReplayPlan plan = os::makeU02OsReplayPlan();
  const os::PlanValidation validation = plan.validate();
  if (!validation.valid || plan.steps.size() != 29u ||
      plan.phaseCount() != 91u || plan.graphRoles.size() != 9u) {
    std::cerr << (validation.error.empty() ? "invalid frozen OS replay plan"
                                           : validation.error)
              << '\n';
    return EXIT_FAILURE;
  }
  struct ContextExpectation final {
    std::string_view id;
    std::string_view graphRole;
    os::TargetSemantic popupSemantic;
    std::string_view popupRole;
    std::uint32_t firstOrdinal;
  };
  constexpr std::array<ContextExpectation, 6u> expectedContexts{{
      {"scene-context-open", "primary-scene",
       os::TargetSemantic::sceneAddNode, "menu:add-node", 2u},
      {"node-context-add-child", "primary-node",
       os::TargetSemantic::nodeAddChildMenu, "menu:add-child", 9u},
      {"node-context-rename", "primary-node", os::TargetSemantic::nodeRename,
       "menu:rename", 19u},
      {"node-context-duplicate", "primary-node",
       os::TargetSemantic::nodeDuplicate, "menu:duplicate", 32u},
      {"node-context-delete", "regular-delete-node",
       os::TargetSemantic::nodeDelete, "menu:delete", 58u},
      {"unique-root-context-delete", "unique-root-node",
       os::TargetSemantic::nodeDelete, "menu:delete", 75u},
  }};

  std::size_t rightDownCount = 0u;
  std::size_t rightUpCount = 0u;
  std::size_t rightClickCount = 0u;
  for (const os::StepSpec &step : plan.steps) {
    for (const os::PhaseSpec &phase : step.phases) {
      rightDownCount += phase.input.kind == os::InputKind::rightDown ? 1u : 0u;
      rightUpCount += phase.input.kind == os::InputKind::rightUp ? 1u : 0u;
      rightClickCount +=
          phase.input.kind == os::InputKind::rightClick ? 1u : 0u;
    }
  }
  if (rightDownCount != expectedContexts.size() ||
      rightUpCount != expectedContexts.size() || rightClickCount != 0u)
    return EXIT_FAILURE;

  const auto exactPhase = [](const os::PhaseSpec &phase, std::uint32_t ordinal,
                             std::string_view id, os::ProbePhase probePhase,
                             os::TargetSemantic targetSemantic,
                             std::string_view role,
                             std::string_view graphRowRole,
                             os::InputKind inputKind, bool requireHovered,
                             os::CapturePoint capture) {
    return phase.ordinal == ordinal && phase.id == id &&
           phase.phase == probePhase &&
           phase.target.semantic == targetSemantic &&
           phase.target.role == role &&
           phase.target.graphRowRole == graphRowRole &&
           phase.input.kind == inputKind && phase.input.value.empty() &&
           phase.requireHovered == requireHovered && !phase.requireFocused &&
           !phase.requireDropAccepted && phase.capture == capture;
  };
  for (const ContextExpectation &expected : expectedContexts) {
    const auto found = std::find_if(
        plan.steps.begin(), plan.steps.end(), [&](const os::StepSpec &step) {
          return step.id == expected.id;
        });
    if (found == plan.steps.end() ||
        found->effect != os::EffectKind::presentationOnly ||
        found->expectedRevisionDelta != 0 ||
        found->history != os::HistoryEffect::unchanged ||
        found->expectHierarchyOrderDigestChange || found->relaunchProcessBefore ||
        !found->expectedDispatches.empty() || found->phases.size() != 4u ||
        !exactPhase(found->phases[0], expected.firstOrdinal, "target",
                    os::ProbePhase::target, os::TargetSemantic::graphRow,
                    expected.graphRole, expected.graphRole,
                    os::InputKind::movePointer, false,
                    os::CapturePoint::before) ||
        !exactPhase(found->phases[1], expected.firstOrdinal + 1u, "hover",
                    os::ProbePhase::hover, os::TargetSemantic::graphRow,
                    expected.graphRole, expected.graphRole,
                    os::InputKind::rightDown, true,
                    os::CapturePoint::none) ||
        !exactPhase(found->phases[2], expected.firstOrdinal + 2u, "release",
                    os::ProbePhase::hover, os::TargetSemantic::graphRow,
                    expected.graphRole, expected.graphRole,
                    os::InputKind::rightUp, true, os::CapturePoint::none) ||
        !exactPhase(found->phases[3], expected.firstOrdinal + 3u, "settled",
                    os::ProbePhase::settled, expected.popupSemantic,
                    expected.popupRole, expected.graphRole,
                    os::InputKind::none, false, os::CapturePoint::after))
      return EXIT_FAILURE;
  }
  const std::string json = plan.toJson();
  if (json.find("\"main-process-boundary\"") == std::string::npos ||
      json.find("\"unique-root-select\"") == std::string::npos ||
      json.find("\"win32-send-input\"") == std::string::npos)
    return EXIT_FAILURE;

  const os::ReplayPlan u03 = os::makeU03OsReplayPlan();
  const os::PlanValidation u03Validation = u03.validate();
  if (!u03Validation.valid || u03.caseId != os::kU03Case ||
      u03.steps.size() != 20u || u03.graphRoles.size() != 2u ||
      u03.phaseCount() < u03.steps.size() * 2u) {
    std::cerr << (u03Validation.error.empty() ? "invalid U03 OS replay plan"
                                               : u03Validation.error)
              << '\n';
    return EXIT_FAILURE;
  }
  const auto findU03 = [&](std::string_view id) {
    return std::find_if(u03.steps.begin(), u03.steps.end(),
                        [&](const os::StepSpec &step) { return step.id == id; });
  };
  const auto trs = findU03("inspector-translation-x");
  if (trs == u03.steps.end() || trs->expectedDispatches.size() != 3u ||
      trs->expectedDispatches[0].kind != os::DispatchKind::propertyEdit ||
      trs->expectedDispatches[0].propertyPhase != "begin" ||
      trs->expectedDispatches[1].propertyPhase != "preview" ||
      trs->expectedDispatches[2].propertyPhase != "commit" ||
      trs->expectedDispatches[0].operation != "translation" ||
      trs->expectedDispatches[0].propertyValue != "0.0,0.0,0.0" ||
      trs->expectedDispatches[2].propertyValue != "0.8,0.0,0.0")
    return EXIT_FAILURE;
  const auto escape = findU03("gizmo-escape-only");
  const auto sameFrame = findU03("gizmo-escape-release-same-frame");
  if (escape == u03.steps.end() || sameFrame == u03.steps.end() ||
      escape->effect != os::EffectKind::selectionOnly ||
      sameFrame->effect != os::EffectKind::selectionOnly ||
      !escape->expectedDispatches.empty() ||
      !sameFrame->expectedDispatches.empty())
    return EXIT_FAILURE;
  const auto inspector = os::makeU03InspectorOsReplayPlan();
  if (!inspector.validate().valid || inspector.steps.size() != 8u ||
      inspector.phaseCount() != 26u || inspector.caseId != os::kU03InspectorCase)
    return EXIT_FAILURE;
  for (const auto &step : inspector.steps) {
    for (const auto &phase : step.phases) {
      if (!phase.expectedDispatchCount ||
          (phase.target.semantic != os::TargetSemantic::client &&
           !os::editorProbeSemantic(phase.target.semantic)))
        return EXIT_FAILURE;
    }
  }
  for (const auto field : {2u, 3u, 4u}) {
    const auto &step = inspector.steps[field];
    if (step.expectedDispatches.size() != 3u ||
        step.phases[2].input.value != "80,0" ||
        step.phases[2].expectedDispatchCount != 1u ||
        step.phases[3].expectedDispatchCount != 2u ||
        step.phases[4].expectedDispatchCount != 3u)
      return EXIT_FAILURE;
  }
  for (const auto field : {2u, 3u, 4u}) {
    auto invalid = inspector;
    invalid.steps[field].phases[2].expectedDispatchCount.reset();
    if (invalid.validate().valid)
      return EXIT_FAILURE;
    invalid = inspector;
    invalid.steps[field].phases[2].input.value = "80,0,999";
    if (invalid.validate().valid)
      return EXIT_FAILURE;
    invalid = inspector;
    invalid.steps[field].expectedDispatches[1].propertyValue.clear();
    if (invalid.validate().valid)
      return EXIT_FAILURE;
  }
  const auto gizmo = os::makeU03GizmoOsReplayPlan();
  if (!gizmo.validate().valid || gizmo.steps.size() != 6u || gizmo.phaseCount() != 16u ||
      gizmo.caseId != os::kU03GizmoCase || !gizmo.steps[1].phases.back().gizmo ||
      gizmo.steps[2].phases.size() != 5u ||
      gizmo.steps[2].phases[0].target.semantic != os::TargetSemantic::gizmoTranslateY ||
      gizmo.steps[2].phases[2].target.semantic != os::TargetSemantic::viewportOutside ||
      gizmo.steps[2].phases[3].gizmo->previewCount != 1u ||
      gizmo.steps[2].phases[4].gizmo->commitCount != 1u)
    return EXIT_FAILURE;
  auto invalidGizmo = gizmo;
  invalidGizmo.steps[2].phases[2].gizmo->captureHeld = false;
  if (invalidGizmo.validate().valid) return EXIT_FAILURE;
  const auto escapePlan = os::makeU03EscapeOsReplayPlan();
  if (!escapePlan.validate().valid || escapePlan.steps.size() != 5u ||
      escapePlan.phaseCount() != 15u || escapePlan.caseId != os::kU03EscapeCase ||
      escapePlan.steps[2].history != os::HistoryEffect::unchanged ||
      escapePlan.steps[2].phases[3].input.kind != os::InputKind::keyDown ||
      escapePlan.steps[2].phases[3].input.value != "Escape" ||
      escapePlan.steps[2].phases.back().gizmo->commitCount != 0u ||
      escapePlan.steps[2].phases.back().gizmo->detachCount != 1u ||
      !escapePlan.steps[2].phases.back().gizmo->noSelection ||
      escapePlan.steps[3].phases[0].input.kind != os::InputKind::keyUp ||
      escapePlan.steps[3].phases[1].input.kind != os::InputKind::leftUp)
    return EXIT_FAILURE;
  for (const bool escapeFirst : {true, false}) {
    auto sameFrame = os::makeU03SameFrameOsReplayPlan(escapeFirst);
    if (!sameFrame.validate().valid || sameFrame.steps.size() != 5u ||
        sameFrame.phaseCount() != 15u ||
        sameFrame.caseId != (escapeFirst ? os::kU03EscapeReleaseCase
                                         : os::kU03ReleaseEscapeCase) ||
        sameFrame.steps[2].id != "drag-same-frame" ||
        sameFrame.steps[2].phases[3].id != "terminal-batch" ||
        sameFrame.steps[2].phases[3].input.kind != os::InputKind::escapeReleaseBatch ||
        sameFrame.steps[2].phases[3].input.value !=
            (escapeFirst ? "escape-first" : "release-first") ||
        sameFrame.steps[2].phases.back().gizmo->detachCount != 1u ||
        sameFrame.steps[2].phases.back().gizmo->commitCount != 0u)
      return EXIT_FAILURE;
    sameFrame.steps[2].phases[3].input.value =
        escapeFirst ? "release-first" : "escape-first";
    if (sameFrame.validate().valid) return EXIT_FAILURE;
  }
  for (const auto caseId : {os::kU03FocusNoReleaseCase,
                             os::kU03FocusSameFrameCase,
                             os::kU03FocusDeliveredReleaseCase}) {
    const auto focus = os::makeU03FocusOsReplayPlan(caseId);
    const auto validation = focus.validate();
    if (!validation.valid || focus.caseId != caseId || focus.steps.size() != 4u ||
        focus.phaseCount() != 14u ||
        focus.steps[2].phases.size() != 7u ||
        focus.steps[2].phases[0].gizmo->previewCount != 0u ||
        focus.steps[2].phases[3].gizmo->previewCount != 1u ||
        focus.steps[2].phases[3].gizmo->phase != 2u ||
        !focus.steps[3].phases.back().gizmo.has_value())
      return EXIT_FAILURE;
    if (caseId == os::kU03FocusNoReleaseCase) {
      if (focus.steps[2].phases[3].input.kind != os::InputKind::focusLoss ||
          focus.steps[2].phases[4].input.kind != os::InputKind::focusRestore ||
          focus.steps[2].phases[4].gizmo->focusLossCount != 1u ||
          focus.steps[2].phases[4].gizmo->suppressedReleaseCount != 0u ||
          focus.steps[2].phases[4].gizmo->pointerOutside.has_value() ||
          focus.steps[2].phases[5].gizmo->suppressedReleaseCount != 1u ||
          !focus.steps[2].phases[5].gizmo->pointerOutside.value_or(false) ||
          focus.steps[2].phases[5].input.kind != os::InputKind::leftUp ||
          focus.steps[2].phases[4].gizmo->latchCount != 1u ||
          focus.steps[2].phases[6].gizmo->suppressedReleaseCount != 2u)
        return EXIT_FAILURE;
    } else if (caseId == os::kU03FocusSameFrameCase) {
      if (focus.steps[2].phases[3].input.kind != os::InputKind::leftUpFocusLoss ||
          focus.steps[2].phases[4].input.kind != os::InputKind::focusRestore ||
          focus.steps[2].phases[4].gizmo->suppressedReleaseCount != 1u ||
          focus.steps[2].phases[4].gizmo->pointerOutside.has_value() ||
          !focus.steps[2].phases[5].gizmo->pointerOutside.value_or(false))
        return EXIT_FAILURE;
    } else if (focus.steps[2].phases[3].input.kind != os::InputKind::leftUp ||
               focus.steps[2].phases[4].input.kind != os::InputKind::focusLoss ||
               focus.steps[2].phases[4].gizmo->commitCount != 1u ||
               focus.steps[2].phases[4].gizmo->deliveredReleaseCount != 1u ||
               focus.steps[2].phases[5].gizmo->pointerOutside.has_value())
      return EXIT_FAILURE;
  }
  {
    auto invalidFocus =
        os::makeU03FocusOsReplayPlan(os::kU03FocusNoReleaseCase);
    invalidFocus.steps[2].phases[4].gizmo->deliveredReleaseCount = 1u;
    if (invalidFocus.validate().valid) return EXIT_FAILURE;
    invalidFocus =
        os::makeU03FocusOsReplayPlan(os::kU03FocusNoReleaseCase);
    invalidFocus.steps[2].phases[4].gizmo->noSelection = true;
    if (invalidFocus.validate().valid) return EXIT_FAILURE;
    invalidFocus =
        os::makeU03FocusOsReplayPlan(os::kU03FocusNoReleaseCase);
    invalidFocus.steps[2].phases[4].gizmo->commitCount = 1u;
    if (invalidFocus.validate().valid) return EXIT_FAILURE;
    invalidFocus =
        os::makeU03FocusOsReplayPlan(os::kU03FocusDeliveredReleaseCase);
    invalidFocus.steps[2].phases[4].gizmo->latchCount = 1u;
    if (invalidFocus.validate().valid) return EXIT_FAILURE;
  }
  std::cout << "Editor UI OS replay plan tests passed.\n";
  return EXIT_SUCCESS;
}
