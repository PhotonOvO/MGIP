#include "../../app/editor/EditorUiOsReplayRuntime.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace demo::editor;
namespace ui = demo::editor::ui;
namespace os = demo::editor::testing::osreplay;

constexpr EditorDocumentAuthority kAuthority{0x4f5352554e54494dULL};

void check(bool condition, std::string_view message) {
  if (!condition)
    throw std::runtime_error(std::string(message));
}

EditorObjectId rawId(EditorObjectKind kind, std::uint32_t slot) {
  return {kind, slot, 1u, kAuthority};
}

template <EditorObjectKind Kind>
TypedEditorObjectId<Kind> typedId(std::uint32_t slot) {
  const auto result = TypedEditorObjectId<Kind>::from(rawId(Kind, slot));
  if (!result)
    std::abort();
  return *result;
}

std::string hash(char digit) { return std::string(64u, digit); }

ui::EditorUiProbeFrameBinding binding(std::uint64_t serial,
                                      std::uint64_t revision = 10u,
                                      SelectionRevision selection = 1u) {
  return ui::EditorUiProbeFrameBinding{
      .lifecycle = ui::LifecycleState::readyClean,
      .lifecycleRevision = 2u,
      .documentRevision = revision,
      .selectionRevision = selection,
      .frameSerial = serial,
      .mainViewportId = 7u,
      .viewportPosition = {10.0F, 20.0F},
      .viewportSize = {400.0F, 300.0F},
      .displaySize = {400.0F, 300.0F},
      .framebufferScale = {1.5F, 2.0F},
  };
}

os::RuntimeHostSnapshot hostSnapshot(std::uint64_t revision = 10u,
                                     SelectionRevision selection = 1u) {
  os::RuntimeHostSnapshot result;
  result.client = os::ClientProbe{
      .hwnd = 11u,
      .pid = 12u,
      .width = 600u,
      .height = 600u,
      .dpi = 144u,
      .screenRect = {100, 200, 700, 800},
  };
  result.authority.documentAuthority = kAuthority.value;
  result.authority.documentRevision = revision;
  result.authority.selectionRevision = selection;
  result.authority.documentDigest = hash('1');
  result.authority.projectionDigest = hash('2');
  result.authority.runtimeContentDigest = hash('2');
  result.authority.hierarchyOrderDigest = hash('3');
  result.authority.selectionKind = "None";
  result.rendererSettled = true;
  result.clientFocused = true;
  return result;
}

ui::EditorUiProbeItem item(ui::EditorUiProbeSemantic semantic,
                           ui::GraphRowKey row, std::string_view label,
                           std::uint64_t order,
                           ui::EditorUiProbeRect rect =
                               {20.0F, 30.0F, 40.0F, 50.0F}) {
  return ui::EditorUiProbeItem{
      .key = {semantic, row, row.object},
      .label = label,
      .order = order,
      .itemId = static_cast<std::uint32_t>(100u + order),
      .windowToken = 90u,
      .rect = rect,
      .visible = true,
      .enabled = true,
  };
}

os::ReplayPlan singleRolePlan() {
  os::ReplayPlan plan;
  plan.description = "runtime single role test";
  plan.graphRoles = {
      {"root", "main", EditorObjectKind::node, "Root", "probe-step", {},
       {}, {}, true},
  };
  os::StepSpec step;
  step.index = 0u;
  step.id = "probe-step";
  step.subcase = "main";
  step.phases = {
      {0u,
       "target",
       os::ProbePhase::target,
       {os::TargetSemantic::graphRow, "root-target", "root"},
       {os::InputKind::none, {}},
       false,
       false,
       false,
       os::CapturePoint::before},
      {1u,
       "settled",
       os::ProbePhase::settled,
       {os::TargetSemantic::graphRow, "root-target", "root"},
       {os::InputKind::none, {}},
       false,
       false,
       false,
       os::CapturePoint::after},
  };
  plan.steps.push_back(std::move(step));
  return plan;
}

struct Harness final {
  os::RuntimeHostSnapshot host{hostSnapshot()};
  std::vector<os::ReadyProbe> published;
  std::string readyHash{hash('a')};

  os::RuntimeCallbacks callbacks() {
    return os::RuntimeCallbacks{
        .snapshot = [this]() -> std::optional<os::RuntimeHostSnapshot> {
          return host;
        },
        .publishReady = [this](const os::ReadyProbe &ready) {
          published.push_back(ready);
          return os::ReadyPublication{true, readyHash, {}};
        },
    };
  }
};

os::RuntimeConfig config(os::ReplayPlan plan, std::string subcase = "main",
                         std::uint32_t start = 0u,
                         std::uint32_t stop = 0u) {
  return os::RuntimeConfig{
      .plan = std::move(plan),
      .subcase = std::move(subcase),
      .startStep = start,
      .stopStep = stop,
      .runNonce = std::string(32u, 'a'),
      .planSha256 = hash('b'),
      .executableSha256 = hash('c'),
      .sourceSha256 = hash('d'),
  };
}

os::ContinueReceipt receipt(const os::EditorUiOsReplayRuntime &runtime,
                            std::string readyHash, os::InputKind kind,
                            std::uint32_t count) {
  const os::ReadyProbe &ready = *runtime.lastReady();
  return os::ContinueReceipt{
      .runNonce = std::string(32u, 'a'),
      .caseId = ready.caseId,
      .planSha256 = hash('b'),
      .step = ready.step,
      .stepId = ready.stepId,
      .phaseId = ready.phaseId,
      .readySha256 = std::move(readyHash),
      .inputKind = kind,
      .sendInputCount = count,
  };
}

void render(os::EditorUiOsReplayRuntime &runtime,
            const ui::EditorUiProbeFrameBinding &frame,
            std::vector<ui::EditorUiProbeItem> items) {
  runtime.beginFrame(frame);
  for (const ui::EditorUiProbeItem &value : items)
    runtime.observeItem(value);
  runtime.endFrame();
}

void testRenderedTimingDpiAndContinue() {
  const SceneId scene = typedId<EditorObjectKind::scene>(0u);
  const NodeId root = typedId<EditorObjectKind::node>(0u);
  const ui::GraphRowKey rootKey{scene, root.untyped(), std::nullopt, 0u};
  Harness harness;
  os::EditorUiOsReplayRuntime runtime(config(singleRolePlan()),
                                      harness.callbacks());
  check(!runtime.failed(), runtime.error());

  render(runtime, binding(1u),
         {item(ui::EditorUiProbeSemantic::graphRow, rootKey,
               " [0] [N] Root", 1u)});
  check(harness.published.empty() && !runtime.waitingForContinue(),
        "endFrame published ready before the rendered frame boundary");
  check(runtime.onFrameRendered() && harness.published.size() == 1u &&
            runtime.waitingForContinue(),
        "onFrameRendered did not publish exactly one ready probe");
  check(!runtime.onFrameRendered() && harness.published.size() == 1u,
        "one rendered frame published ready more than once");
  const os::ReadyProbe &ready = harness.published.back();
  check(ready.target.rect.left == 115 && ready.target.rect.top == 220 &&
            ready.target.rect.right == 145 && ready.target.rect.bottom == 260,
        "logical ImGui rectangle did not map to physical screen pixels");
  check(runtime.graphRole("root") == rootKey,
        "root graph role did not bind its exact key");

  os::ContinueReceipt stale =
      receipt(runtime, hash('e'), os::InputKind::none, 0u);
  check(!runtime.acceptContinue(stale) && runtime.waitingForContinue(),
        "stale ready hash advanced the runtime");
  check(runtime.acceptContinue(
            receipt(runtime, harness.readyHash, os::InputKind::none, 0u)) &&
            runtime.currentPhase() == 1u,
        "valid continue did not advance to the settled phase");

  render(runtime, binding(2u),
         {item(ui::EditorUiProbeSemantic::graphRow, rootKey,
               " [0] [N] Root", 1u)});
  check(harness.published.size() == 1u,
        "settled ready published from endFrame");
  check(runtime.onFrameRendered() && harness.published.size() == 2u,
        "settled ready did not publish after render");
  check(!runtime.acceptContinue(
            receipt(runtime, harness.readyHash, os::InputKind::none, 1u)),
        "invalid SendInput count advanced the runtime");
  check(runtime.acceptContinue(
            receipt(runtime, harness.readyHash, os::InputKind::none, 0u)) &&
            runtime.complete(),
        "terminal continue did not complete the runtime");
}

void testConfiguredConsecutiveTargetStability() {
  const SceneId scene = typedId<EditorObjectKind::scene>(0u);
  const NodeId root = typedId<EditorObjectKind::node>(0u);
  const ui::GraphRowKey rootKey{scene, root.untyped(), std::nullopt, 0u};
  Harness harness;
  os::RuntimeConfig runtimeConfig = config(singleRolePlan());
  runtimeConfig.requiredStableFrames = 2u;
  os::EditorUiOsReplayRuntime runtime(std::move(runtimeConfig),
                                      harness.callbacks());

  render(runtime, binding(1u),
         {item(ui::EditorUiProbeSemantic::graphRow, rootKey,
               " [0] [N] Root", 1u,
               {20.0F, 30.0F, 28.0F, 48.0F})});
  check(!runtime.onFrameRendered() && harness.published.empty(),
        "first transient target frame was published");
  auto changed = item(ui::EditorUiProbeSemantic::graphRow, rootKey,
                      " [0] [N] Root", 1u,
                      {20.0F, 30.0F, 80.0F, 48.0F});
  changed.hovered = true;
  render(runtime, binding(2u), {changed});
  check(!runtime.onFrameRendered() && harness.published.empty(),
        "changed target geometry counted as stable");
  check(runtime.pendingStableFrameCount() == 1u &&
            runtime.pendingStableFrameSerial() == 2u &&
            runtime.pendingReason() == "target-geometry-stabilizing",
        "pending target stability diagnostics are incorrect");
  render(runtime, binding(3u),
         {item(ui::EditorUiProbeSemantic::graphRow, rootKey,
               " [0] [N] Root", 1u,
               {20.0F, 30.0F, 80.0F, 48.0F})});
  check(runtime.onFrameRendered() && harness.published.size() == 1u &&
            harness.published.front().renderedFrame == 3u &&
            harness.published.front().target.probeRect.maximumX == 80.0F,
        "second consecutive stable target frame was not published");
}

void testSplitRightButtonInputCounts() {
  os::ReplayPlan plan = singleRolePlan();
  plan.steps[0].phases[0].input = {os::InputKind::rightDown, {}};
  plan.steps[0].phases[1].id = "release";
  plan.steps[0].phases[1].phase = os::ProbePhase::hover;
  plan.steps[0].phases[1].input = {os::InputKind::rightUp, {}};
  Harness harness;
  os::EditorUiOsReplayRuntime runtime(config(std::move(plan)),
                                      harness.callbacks());
  const SceneId scene = typedId<EditorObjectKind::scene>(0u);
  const NodeId root = typedId<EditorObjectKind::node>(0u);
  const ui::GraphRowKey rootKey{scene, root.untyped(), std::nullopt, 0u};

  render(runtime, binding(1u),
         {item(ui::EditorUiProbeSemantic::graphRow, rootKey,
               " [0] [N] Root", 1u)});
  check(runtime.onFrameRendered(), "right-down phase did not publish");
  check(!runtime.acceptContinue(receipt(runtime, harness.readyHash,
                                       os::InputKind::rightDown, 2u)),
        "right-down accepted a batched edge count");
  check(runtime.acceptContinue(receipt(runtime, harness.readyHash,
                                      os::InputKind::rightDown, 1u)),
        "right-down rejected its single SendInput edge");

  render(runtime, binding(2u),
         {item(ui::EditorUiProbeSemantic::graphRow, rootKey,
               " [0] [N] Root", 1u)});
  check(runtime.onFrameRendered(), "right-up phase did not publish");
  check(!runtime.acceptContinue(receipt(runtime, harness.readyHash,
                                       os::InputKind::rightUp, 0u)),
        "right-up accepted a missing SendInput edge");
  check(runtime.acceptContinue(receipt(runtime, harness.readyHash,
                                      os::InputKind::rightUp, 1u)) &&
            runtime.complete(),
        "right-up rejected its single SendInput edge");
}

os::ReplayPlan constrainedRolePlan() {
  os::ReplayPlan plan;
  plan.description = "runtime constrained role test";
  plan.graphRoles = {
      {"parent", "main", EditorObjectKind::node, "Parent", "roles", {}, {},
       {}, true},
      {"parent-alias", "main", EditorObjectKind::node, "", "roles",
       "parent", {}, {}, true},
      {"child", "main", EditorObjectKind::node, "Child", "roles", {},
       "parent", {}, false},
      {"other", "main", EditorObjectKind::node, "Other", "roles", {}, {},
       "parent", true},
  };
  os::StepSpec step;
  step.index = 0u;
  step.id = "roles";
  step.subcase = "main";
  step.phases = {
      {0u, "target", os::ProbePhase::target,
       {os::TargetSemantic::graphRow, "child-target", "child"},
       {os::InputKind::none, {}}, false, false, false,
       os::CapturePoint::before},
      {1u, "settled", os::ProbePhase::settled,
       {os::TargetSemantic::graphRow, "child-target", "child"},
       {os::InputKind::none, {}}, false, false, false,
       os::CapturePoint::after},
  };
  plan.steps.push_back(std::move(step));
  return plan;
}

os::ReplayPlan dynamicRolePlan() {
  os::ReplayPlan plan;
  plan.description = "runtime dynamic role introduction test";
  plan.graphRoles = {
      {"dynamic-root", "main", EditorObjectKind::node, "Dynamic",
       "introduce", {}, {}, {}, true},
  };
  os::StepSpec bootstrap;
  bootstrap.index = 0u;
  bootstrap.id = "bootstrap";
  bootstrap.subcase = "main";
  bootstrap.phases = {
      {0u, "target", os::ProbePhase::target,
       {os::TargetSemantic::client, "client", {}},
       {os::InputKind::none, {}}, false, false, false,
       os::CapturePoint::before},
      {1u, "settled", os::ProbePhase::settled,
       {os::TargetSemantic::client, "client", {}},
       {os::InputKind::none, {}}, false, false, false,
       os::CapturePoint::after},
  };
  os::StepSpec introduce;
  introduce.index = 1u;
  introduce.id = "introduce";
  introduce.subcase = "main";
  introduce.phases = {
      {2u, "target", os::ProbePhase::target,
       {os::TargetSemantic::graphRow, "dynamic-target", "dynamic-root"},
       {os::InputKind::none, {}}, false, false, false,
       os::CapturePoint::before},
      {3u, "settled", os::ProbePhase::settled,
       {os::TargetSemantic::graphRow, "dynamic-target", "dynamic-root"},
       {os::InputKind::none, {}}, false, false, false,
       os::CapturePoint::after},
  };
  plan.steps = {std::move(bootstrap), std::move(introduce)};
  return plan;
}

void testDynamicRoleIntroduction() {
  const SceneId scene = typedId<EditorObjectKind::scene>(0u);
  const NodeId node = typedId<EditorObjectKind::node>(9u);
  const ui::GraphRowKey key{scene, node.untyped(), std::nullopt, 0u};
  Harness harness;
  os::EditorUiOsReplayRuntime runtime(
      config(dynamicRolePlan(), "main", 0u, 1u), harness.callbacks());
  for (std::uint64_t serial = 1u; serial <= 2u; ++serial) {
    render(runtime, binding(serial),
           {item(ui::EditorUiProbeSemantic::graphRow, key,
                 " [9] [N] Dynamic", 1u)});
    check(runtime.onFrameRendered(),
          "bootstrap phase did not publish its client ready");
    check(!runtime.graphRole("dynamic-root"),
          "dynamic role bound before its introduction step");
    check(runtime.acceptContinue(receipt(runtime, harness.readyHash,
                                        os::InputKind::none, 0u)),
          "bootstrap continue failed");
  }
  check(runtime.currentStep() == 1u,
        "runtime did not enter the role introduction step");
  render(runtime, binding(3u),
         {item(ui::EditorUiProbeSemantic::graphRow, key,
               " [9] [N] Dynamic", 1u)});
  check(runtime.onFrameRendered() && runtime.graphRole("dynamic-root") == key,
        "dynamic role did not bind at its introduction step");
}

void testRoleConstraintsAndAmbiguity() {
  const SceneId scene = typedId<EditorObjectKind::scene>(0u);
  const NodeId parent = typedId<EditorObjectKind::node>(0u);
  const NodeId child = typedId<EditorObjectKind::node>(1u);
  const NodeId other = typedId<EditorObjectKind::node>(2u);
  const ui::GraphRowKey parentKey{scene, parent.untyped(), std::nullopt, 0u};
  const ui::GraphRowKey childKey{scene, child.untyped(), parent, 0u};
  const ui::GraphRowKey otherKey{scene, other.untyped(), std::nullopt, 0u};
  Harness harness;
  os::EditorUiOsReplayRuntime runtime(config(constrainedRolePlan()),
                                      harness.callbacks());
  render(runtime, binding(1u),
         {item(ui::EditorUiProbeSemantic::graphRow, parentKey,
               " [0] [N] Parent", 1u),
          item(ui::EditorUiProbeSemantic::graphRow, childKey,
               " [1] [N] Child", 2u),
          item(ui::EditorUiProbeSemantic::graphRow, otherKey,
               " [2] [N] Other", 3u)});
  check(runtime.onFrameRendered() &&
            runtime.graphRole("parent") == parentKey &&
            runtime.graphRole("parent-alias") == parentKey &&
            runtime.graphRole("child") == childKey &&
            runtime.graphRole("other") == otherKey,
        "role constraints did not bind same/direct-parent/distinct/root keys");

  Harness duplicateHarness;
  os::EditorUiOsReplayRuntime duplicate(config(singleRolePlan()),
                                        duplicateHarness.callbacks());
  const NodeId second = typedId<EditorObjectKind::node>(3u);
  const ui::GraphRowKey secondKey{scene, second.untyped(), std::nullopt, 0u};
  render(duplicate, binding(1u),
         {item(ui::EditorUiProbeSemantic::graphRow, parentKey,
               " [0] [N] Root", 1u),
          item(ui::EditorUiProbeSemantic::graphRow, secondKey,
               " [3] [N] Root", 2u)});
  check(!duplicate.onFrameRendered() && duplicate.failed(),
        "ambiguous role candidates did not fail closed");

  Harness missingHarness;
  os::EditorUiOsReplayRuntime missing(config(singleRolePlan()),
                                      missingHarness.callbacks());
  render(missing, binding(1u), {});
  check(!missing.onFrameRendered() && !missing.failed() &&
            missingHarness.published.empty(),
        "missing role target published a ready probe");

  Harness invalidDpiHarness;
  invalidDpiHarness.host.client.width = 601u;
  os::EditorUiOsReplayRuntime invalidDpi(config(singleRolePlan()),
                                         invalidDpiHarness.callbacks());
  render(invalidDpi, binding(1u),
         {item(ui::EditorUiProbeSemantic::graphRow, parentKey,
               " [0] [N] Root", 1u)});
  check(!invalidDpi.onFrameRendered() &&
            invalidDpiHarness.published.empty(),
        "incoherent DPI/client extent published a target");
}

os::ReplayPlan dragPlan() {
  os::ReplayPlan plan;
  plan.description = "runtime drag dispatch test";
  plan.graphRoles = {
      {"source", "main", EditorObjectKind::node, "Source", "drag", {}, {},
       {}, true},
      {"target", "main", EditorObjectKind::node, "Target", "drag", {}, {},
       "source", true},
  };
  os::StepSpec step;
  step.index = 0u;
  step.id = "drag";
  step.subcase = "main";
  step.effect = os::EffectKind::documentNoopHistoryEnvelope;
  step.history = os::HistoryEffect::pushUndoClearRedo;
  step.expectedDispatches = {
      {ui::CommandSource::sceneBrowser, os::DispatchKind::selectObject,
       "select", "source", {}, {}, true,
       ui::ProjectionRefresh::selectionOnly},
      {ui::CommandSource::sceneBrowser, os::DispatchKind::reparentNode,
       "reparent", "source", "target", {}, true,
       ui::ProjectionRefresh::none},
  };
  step.phases = {
      {0u, "source-target", os::ProbePhase::target,
       {os::TargetSemantic::nodeDragSource, "source", "source"},
       {os::InputKind::movePointer, {}}, false, false, false,
       os::CapturePoint::before},
      {1u, "source-hover", os::ProbePhase::hover,
       {os::TargetSemantic::nodeDragSource, "source", "source"},
       {os::InputKind::leftDown, {}}, true, false, false,
       os::CapturePoint::none},
      {2u, "drag-target", os::ProbePhase::dragTarget,
       {os::TargetSemantic::hierarchyDropTarget, "target", "target"},
       {os::InputKind::movePointer, {}}, false, false, false,
       os::CapturePoint::none},
      {3u, "drop-hover", os::ProbePhase::dropHover,
       {os::TargetSemantic::hierarchyDropTarget, "target", "target"},
       {os::InputKind::leftUp, {}}, true, false, true,
       os::CapturePoint::none},
      {4u, "settled", os::ProbePhase::settled,
       {os::TargetSemantic::hierarchyDropTarget, "target", "target"},
       {os::InputKind::none, {}}, false, false, false,
       os::CapturePoint::after},
  };
  plan.steps.push_back(std::move(step));
  return plan;
}

std::vector<ui::EditorUiProbeItem>
dragItems(ui::GraphRowKey source, ui::GraphRowKey target,
          ui::EditorUiProbeSemantic targetSemantic, bool hovered = false,
          bool dropAccepted = false) {
  std::vector<ui::EditorUiProbeItem> result{
      item(ui::EditorUiProbeSemantic::graphRow, source,
           " [0] [N] Source", 1u),
      item(ui::EditorUiProbeSemantic::graphRow, target,
           " [1] [N] Target", 2u),
      item(targetSemantic,
           targetSemantic == ui::EditorUiProbeSemantic::nodeDragSource
               ? source
               : target,
           targetSemantic == ui::EditorUiProbeSemantic::nodeDragSource
               ? " [0] [N] Source"
               : " [1] [N] Target",
           3u),
  };
  result.back().hovered = hovered;
  result.back().dropAccepted = dropAccepted;
  return result;
}

void publishPhase(os::EditorUiOsReplayRuntime &runtime, Harness &harness,
                  std::uint64_t serial,
                  std::vector<ui::EditorUiProbeItem> items,
                  os::InputKind input, std::uint32_t count) {
  render(runtime, binding(serial, harness.host.authority.documentRevision,
                          harness.host.authority.selectionRevision),
         std::move(items));
  check(harness.published.empty() ||
            harness.published.back().frame.frameSerial != serial,
        "endFrame published a drag ready probe");
  check(runtime.onFrameRendered(), "drag phase did not become ready");
  check(runtime.acceptContinue(
            receipt(runtime, harness.readyHash, input, count)),
        "drag phase continue was rejected");
}

void testDragDispatchPrefixAndNoopTerminal(bool changeSelectionOnDrop) {
  const SceneId scene = typedId<EditorObjectKind::scene>(0u);
  const NodeId source = typedId<EditorObjectKind::node>(0u);
  const NodeId target = typedId<EditorObjectKind::node>(1u);
  const ui::GraphRowKey sourceKey{scene, source.untyped(), std::nullopt, 0u};
  const ui::GraphRowKey targetKey{scene, target.untyped(), std::nullopt, 0u};
  Harness harness;
  harness.host.authority.selectionKind = "Node";
  harness.host.authority.selectedObject = target.untyped();
  harness.host.authority.selectedGraphRowKey = targetKey;
  os::EditorUiOsReplayRuntime runtime(config(dragPlan()),
                                      harness.callbacks());
  publishPhase(runtime, harness, 1u,
               dragItems(sourceKey, targetKey,
                         ui::EditorUiProbeSemantic::nodeDragSource),
               os::InputKind::movePointer, 1u);
  publishPhase(runtime, harness, 2u,
               dragItems(sourceKey, targetKey,
                         ui::EditorUiProbeSemantic::nodeDragSource, true),
               os::InputKind::leftDown, 1u);

  ui::UiCommand selected;
  selected.sequence = 1u;
  selected.source = ui::CommandSource::sceneBrowser;
  selected.payload = ui::SelectObject{EditorSelectionSnapshot{
      kAuthority, NodeSelection{scene, source, std::nullopt}}};
  harness.host.authority.selectedObject = source.untyped();
  harness.host.authority.selectedGraphRowKey = sourceKey;
  runtime.beginFrame(binding(3u));
  for (const auto &value : dragItems(
           sourceKey, targetKey,
           ui::EditorUiProbeSemantic::hierarchyDropTarget))
    runtime.observeItem(value);
  runtime.observeDispatch(
      selected, {true, ui::ProjectionRefresh::selectionOnly, {}});
  runtime.endFrame();
  check(runtime.onFrameRendered() &&
            harness.published.back().observedDispatches.size() == 1u,
        "drag-target ready did not expose the exact select prefix");
  check(runtime.acceptContinue(receipt(runtime, harness.readyHash,
                                      os::InputKind::movePointer, 1u)),
        "drag-target continue failed");

  publishPhase(runtime, harness, 4u,
               dragItems(sourceKey, targetKey,
                         ui::EditorUiProbeSemantic::hierarchyDropTarget, true,
                         true),
               os::InputKind::leftUp, 1u);

  ui::UiCommand reparent;
  reparent.sequence = 2u;
  reparent.source = ui::CommandSource::sceneBrowser;
  reparent.payload = ui::ReparentNode{
      SceneNodeOccurrence{scene, source, std::nullopt, 10u}, scene, target};
  harness.host.authority.undoCount = 1u;
  harness.host.authority.undoDescription = "Move 'Source'";
  if (changeSelectionOnDrop) {
    harness.host.authority.selectedObject = target.untyped();
    harness.host.authority.selectedGraphRowKey = targetKey;
  }
  runtime.beginFrame(binding(5u));
  for (const auto &value : dragItems(
           sourceKey, targetKey,
           ui::EditorUiProbeSemantic::hierarchyDropTarget))
    runtime.observeItem(value);
  runtime.observeDispatch(reparent,
                          {true, ui::ProjectionRefresh::none, {}});
  runtime.endFrame();
  if (changeSelectionOnDrop) {
    check(!runtime.onFrameRendered() && runtime.failed(),
          "no-op reparent changed selection after the drop baseline");
    return;
  }
  check(runtime.onFrameRendered() &&
            harness.published.back().observedDispatches.size() == 2u &&
            harness.published.back().observedDispatches.back().refresh ==
                ui::ProjectionRefresh::none,
        "drag settled ready lost its no-op reparent terminal dispatch");
  check(runtime.acceptContinue(receipt(runtime, harness.readyHash,
                                      os::InputKind::none, 0u)) &&
            runtime.complete(),
        "drag runtime did not complete");
}

void testProcessRangesAndFrameOrder() {
  const os::ReplayPlan plan = os::makeU02OsReplayPlan();
  const auto unique = std::find_if(
      plan.steps.begin(), plan.steps.end(), [](const os::StepSpec &step) {
        return step.relaunchProcessBefore;
      });
  check(unique != plan.steps.end(), "frozen plan has no unique-root boundary");
  Harness mainHarness;
  os::EditorUiOsReplayRuntime mainRuntime(
      config(plan, "main", 0u, unique->index - 1u),
      mainHarness.callbacks());
  check(!mainRuntime.failed(), mainRuntime.error());
  Harness uniqueHarness;
  os::EditorUiOsReplayRuntime uniqueRuntime(
      config(plan, "unique-root", unique->index,
             static_cast<std::uint32_t>(plan.steps.size() - 1u)),
      uniqueHarness.callbacks());
  check(!uniqueRuntime.failed(), uniqueRuntime.error());
  Harness crossedHarness;
  os::EditorUiOsReplayRuntime crossed(
      config(plan, "main", 0u, unique->index), crossedHarness.callbacks());
  check(crossed.failed(), "cross-subcase process range was accepted");

  Harness orderHarness;
  os::EditorUiOsReplayRuntime order(config(singleRolePlan()),
                                    orderHarness.callbacks());
  order.beginFrame(binding(2u));
  order.endFrame();
  check(!order.onFrameRendered(), "missing target unexpectedly became ready");
  order.beginFrame(binding(2u));
  check(order.failed(), "stale frame serial did not fail closed");
}

os::ReplayPlan inspectorPlan(bool withDispatches = false) {
  os::ReplayPlan plan = singleRolePlan();
  plan.caseId = std::string(os::kU03InspectorCase);
  os::StepSpec &step = plan.steps.front();
  for (os::PhaseSpec &phase : step.phases) {
    phase.target = {os::TargetSemantic::inspectorTranslationX,
                    "translation-x", "root"};
    phase.expectedDispatchCount = 0u;
  }
  if (!withDispatches)
    return plan;
  step.effect = os::EffectKind::documentMutation;
  step.history = os::HistoryEffect::pushUndoClearRedo;
  step.expectedRevisionDelta = 1;
  step.expectedDispatches = {
      {ui::CommandSource::inspector, os::DispatchKind::propertyEdit,
       "translation", "root", {}, {}, true, ui::ProjectionRefresh::none,
       "begin", "0.0,0.0,0.0"},
      {ui::CommandSource::inspector, os::DispatchKind::propertyEdit,
       "translation", "root", {}, {}, true, ui::ProjectionRefresh::none,
       "preview", "0.8,0.0,0.0"},
      {ui::CommandSource::inspector, os::DispatchKind::propertyEdit,
       "translation", "root", {}, {}, true, ui::ProjectionRefresh::none,
       "commit", "0.8,0.0,0.0"},
  };
  step.phases.insert(step.phases.begin() + 1u, step.phases.front());
  step.phases.insert(step.phases.begin() + 2u, step.phases.front());
  for (std::size_t index = 0; index < step.phases.size(); ++index) {
    os::PhaseSpec &phase = step.phases[index];
    phase.ordinal = static_cast<std::uint32_t>(index);
    phase.expectedDispatchCount = static_cast<std::uint32_t>(index);
    if (index == 1u || index == 2u) {
      phase.id = index == 1u ? "edit-begun" : "edit-preview";
      phase.capture = os::CapturePoint::none;
    }
  }
  return plan;
}

std::vector<ui::EditorUiProbeItem> inspectorItems(ui::GraphRowKey root,
                                                bool hovered = false,
                                                bool focused = false) {
  auto field = item(ui::EditorUiProbeSemantic::inspectorTranslationX, {},
                    "Translation X", 2u, {50.0F, 60.0F, 90.0F, 80.0F});
  field.key.object = root.object;
  field.hovered = hovered;
  field.focused = focused;
  return {item(ui::EditorUiProbeSemantic::graphRow, root, "Root", 1u),
          field};
}

void testUnsupportedPassiveTargetsFailClosed() {
  for (const os::TargetSemantic semantic :
       {os::TargetSemantic::viewport, os::TargetSemantic::gizmoTranslate,
        os::TargetSemantic::gizmoRotate, os::TargetSemantic::gizmoScale,
        os::TargetSemantic::toolbarGrid, os::TargetSemantic::toolbarGizmo,
        os::TargetSemantic::toolbarSnap}) {
    os::ReplayPlan plan = inspectorPlan();
    plan.caseId = std::string(os::kU03Case);
    for (os::PhaseSpec &phase : plan.steps.front().phases)
      phase.target = {semantic, "unsupported-target", {}};
    Harness harness;
    os::EditorUiOsReplayRuntime runtime(config(plan), harness.callbacks());
    check(!runtime.failed(), runtime.error());
    render(runtime, binding(1u), {});
    check(!runtime.onFrameRendered() && runtime.failed() &&
              harness.published.empty() &&
              runtime.error().find("no real semantic observation") !=
                  std::string::npos,
          "unsupported target synthesized ready evidence from the client");
  }
}

void testClientInteractionFlagsAreObserved() {
  for (const bool requireHover : {false, true}) {
    os::ReplayPlan plan = inspectorPlan();
    for (os::PhaseSpec &phase : plan.steps.front().phases)
      phase.target = {os::TargetSemantic::client, "client", {}};
    os::PhaseSpec &phase = plan.steps.front().phases.front();
    phase.phase = os::ProbePhase::focus;
    phase.requireFocused = !requireHover;
    phase.requireHovered = requireHover;
    Harness harness;
    harness.host.clientFocused = false;
    os::EditorUiOsReplayRuntime runtime(config(plan), harness.callbacks());
    check(!runtime.failed(), runtime.error());
    render(runtime, binding(1u), {});
    check(!runtime.onFrameRendered() && !runtime.failed() &&
              harness.published.empty(),
          "client fabricated a required hover or focus flag");
    harness.host.clientFocused = true;
    render(runtime, binding(2u), {});
    if (requireHover) {
      check(!runtime.onFrameRendered() && harness.published.empty(),
            "focused client fabricated hover confirmation");
    } else {
      check(runtime.onFrameRendered() &&
                harness.published.back().target.focused &&
                !harness.published.back().target.hovered &&
                !harness.published.back().target.dropAccepted,
            "client did not preserve its actual host focus state");
    }
  }
}

void testInspectorRealItemStateAndAmbiguity() {
  const SceneId scene = typedId<EditorObjectKind::scene>(0u);
  const NodeId root = typedId<EditorObjectKind::node>(0u);
  const ui::GraphRowKey rootKey{scene, root.untyped(), std::nullopt, 0u};
  os::ReplayPlan plan = inspectorPlan();
  os::PhaseSpec &phase = plan.steps.front().phases.front();
  phase.phase = os::ProbePhase::focus;
  phase.requireHovered = true;
  phase.requireFocused = true;
  Harness harness;
  os::EditorUiOsReplayRuntime runtime(config(plan), harness.callbacks());
  check(!runtime.failed(), runtime.error());
  auto observed = inspectorItems(rootKey);
  observed.pop_back();
  render(runtime, binding(1u), observed);
  check(!runtime.onFrameRendered() && !runtime.failed() &&
            harness.published.empty() &&
            runtime.pendingReason() == "target-not-observed",
        "missing Inspector field synthesized a client-sized target");
  observed = inspectorItems(rootKey, true, true);
  observed.back().key.object = rawId(EditorObjectKind::node, 99u);
  render(runtime, binding(2u), observed);
  check(!runtime.onFrameRendered() && !runtime.failed() &&
            harness.published.empty(),
        "Inspector field for another object matched the selected role");
  render(runtime, binding(3u), inspectorItems(rootKey));
  check(!runtime.onFrameRendered() && !runtime.failed(),
        "Inspector field fabricated its required hover and focus flags");
  observed = inspectorItems(rootKey, true, true);
  observed.back().dropAccepted = true;
  render(runtime, binding(4u), observed);
  check(runtime.onFrameRendered(), runtime.error());
  const os::TargetProbe &target = harness.published.back().target;
  check(target.itemId == 102u && target.order == 2u &&
            target.windowToken == 90u && target.label == "Translation X" &&
            target.graphRowKey == rootKey && target.hovered && target.focused &&
            target.dropAccepted && target.rect.left == 160 &&
            target.rect.top == 280 && target.rect.right == 220 &&
            target.rect.bottom == 320,
        "Inspector ready evidence lost actual item identity or geometry");

  for (const bool distinctKey : {false, true}) {
    Harness duplicateHarness;
    os::EditorUiOsReplayRuntime duplicate(config(inspectorPlan()),
                                          duplicateHarness.callbacks());
    observed = inspectorItems(rootKey);
    auto duplicateField = observed.back();
    duplicateField.order = 3u;
    duplicateField.itemId = 103u;
    if (distinctKey)
      duplicateField.key.row = rootKey;
    observed.push_back(duplicateField);
    render(duplicate, binding(1u), observed);
    check(!duplicate.onFrameRendered() && duplicate.failed() &&
              duplicateHarness.published.empty(),
          "duplicate Inspector observations were accepted as one real item");
  }
}

ui::UiCommand propertyCommand(EditorObjectId target, std::uint64_t sequence,
                               ui::EditPhase phase,
                               ui::Value value) {
  return ui::UiCommand{
      .sequence = sequence,
      .source = ui::CommandSource::inspector,
      .payload = ui::PropertyEdit{target, ui::FieldId::translation, phase,
                                 std::move(value)},
  };
}

void testPropertyDispatchRejectsWrongPayloads() {
  const SceneId scene = typedId<EditorObjectKind::scene>(0u);
  const NodeId root = typedId<EditorObjectKind::node>(0u);
  const ui::GraphRowKey rootKey{scene, root.untyped(), std::nullopt, 0u};
  for (std::uint32_t variant = 0u; variant < 10u; ++variant) {
    Harness harness;
    os::EditorUiOsReplayRuntime runtime(config(inspectorPlan(true)),
                                        harness.callbacks());
    check(!runtime.failed(), runtime.error());
    publishPhase(runtime, harness, 1u, inspectorItems(rootKey),
                 os::InputKind::none, 0u);
    ui::UiCommand command = propertyCommand(
        root.untyped(), 1u, ui::EditPhase::begin,
        std::array<double, 3>{0.0, 0.0, 0.0});
    auto &payload = std::get<ui::PropertyEdit>(command.payload);
    switch (variant) {
    case 0u:
      payload.field = ui::FieldId::rotationDegrees;
      break;
    case 1u:
      payload.field = ui::FieldId::scale;
      break;
    case 2u:
      payload.phase = ui::EditPhase::preview;
      break;
    case 3u:
      payload.value = std::array<double, 3>{0.0000001, 0.0, 0.0};
      break;
    case 4u:
      payload.value = std::array<double, 3>{0.0, 1.0, 0.0};
      break;
    case 5u:
      payload.value = std::array<double, 3>{0.0, 0.0, 1.0};
      break;
    case 6u:
      payload.value = 0.0;
      break;
    case 7u:
      payload.value = std::array<double, 2>{0.0, 0.0};
      break;
    case 8u:
      payload.value = std::array<double, 3>{
          std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0};
      break;
    default:
      payload.target = rawId(EditorObjectKind::node, 99u);
      break;
    }
    runtime.beginFrame(binding(2u));
    for (const auto &value : inspectorItems(rootKey))
      runtime.observeItem(value);
    runtime.observeDispatch(command, {true, ui::ProjectionRefresh::none, {}});
    runtime.endFrame();
    check(runtime.failed() && !runtime.onFrameRendered() &&
              harness.published.size() == 1u,
          "PropertyEdit normalized a wrong actual payload into expected text");
  }
}

void testInspectorDispatchPrefixWaitsForActualCommands(bool changeSelection) {
  const SceneId scene = typedId<EditorObjectKind::scene>(0u);
  const NodeId root = typedId<EditorObjectKind::node>(0u);
  const ui::GraphRowKey rootKey{scene, root.untyped(), std::nullopt, 0u};
  Harness harness;
  os::EditorUiOsReplayRuntime runtime(config(inspectorPlan(true)),
                                      harness.callbacks());
  check(!runtime.failed(), runtime.error());
  publishPhase(runtime, harness, 1u, inspectorItems(rootKey),
               os::InputKind::none, 0u);
  std::uint64_t serial = 2u;
  for (std::uint64_t sequence = 1u; sequence <= 3u; ++sequence) {
    if (sequence == 3u) {
      ++harness.host.authority.documentRevision;
      harness.host.authority.documentDigest = hash('4');
      harness.host.authority.projectionDigest = hash('5');
      harness.host.authority.runtimeContentDigest = hash('5');
      harness.host.authority.undoCount = 1u;
      harness.host.authority.undoDescription = "Translation";
      if (changeSelection) {
        harness.host.authority.selectionKind = "Node";
        harness.host.authority.selectedObject = root.untyped();
        harness.host.authority.selectedGraphRowKey = rootKey;
      }
    }
    render(runtime, binding(serial++, harness.host.authority.documentRevision),
           inspectorItems(rootKey));
    check(!runtime.onFrameRendered() && !runtime.failed() &&
              harness.published.size() == sequence &&
              runtime.pendingReason() == "phase-or-dispatch-prefix-mismatch",
          "missing queued property dispatch published ready or failed early");
    runtime.beginFrame(
        binding(serial++, harness.host.authority.documentRevision));
    for (const auto &value : inspectorItems(rootKey))
      runtime.observeItem(value);
    const ui::EditPhase phase = sequence == 1u ? ui::EditPhase::begin
                                : sequence == 2u ? ui::EditPhase::preview
                                                 : ui::EditPhase::commit;
    runtime.observeDispatch(
        propertyCommand(root.untyped(), sequence, phase,
                        std::array<double, 3>{sequence == 1u ? 0.0 : 0.8,
                                              0.0, 0.0}),
        {true, ui::ProjectionRefresh::none, {}});
    runtime.endFrame();
    if (changeSelection && sequence == 3u) {
      check(!runtime.onFrameRendered() && runtime.failed(),
            "Inspector edit accepted an unrelated terminal selection change");
      return;
    }
    check(runtime.onFrameRendered(), runtime.error());
    const auto &observed = harness.published.back().observedDispatches;
    check(observed.size() == sequence &&
              observed.back().operation == "translation" &&
              observed.back().propertyValue ==
                  (sequence == 1u ? "0.0,0.0,0.0" : "0.8,0.0,0.0") &&
              observed.back().propertyPhase ==
                  (sequence == 1u ? "begin" : sequence == 2u ? "preview"
                                                              : "commit"),
          "Inspector evidence omitted or altered an actual property payload");
    check(runtime.acceptContinue(receipt(runtime, harness.readyHash,
                                        os::InputKind::none, 0u)),
          "Inspector property phase continue failed");
  }
  check(runtime.complete(), "Inspector full command sequence did not complete");
}

void testInspectorDispatchPrefixRejectsMissingContractAndEarlyDispatch() {
  os::ReplayPlan missingContract = inspectorPlan(true);
  missingContract.steps.front().phases[1u].expectedDispatchCount.reset();
  Harness missingHarness;
  os::EditorUiOsReplayRuntime missing(config(missingContract),
                                      missingHarness.callbacks());
  check(missing.failed() && missingHarness.published.empty(),
        "non-U02 replay accepted a phase without an explicit dispatch prefix");

  const SceneId scene = typedId<EditorObjectKind::scene>(0u);
  const NodeId root = typedId<EditorObjectKind::node>(0u);
  const ui::GraphRowKey rootKey{scene, root.untyped(), std::nullopt, 0u};
  Harness harness;
  os::EditorUiOsReplayRuntime runtime(config(inspectorPlan(true)),
                                      harness.callbacks());
  publishPhase(runtime, harness, 1u, inspectorItems(rootKey),
               os::InputKind::none, 0u);
  runtime.beginFrame(binding(2u));
  for (const auto &value : inspectorItems(rootKey))
    runtime.observeItem(value);
  runtime.observeDispatch(
      propertyCommand(root.untyped(), 1u, ui::EditPhase::begin,
                      std::array<double, 3>{0.0, 0.0, 0.0}),
      {true, ui::ProjectionRefresh::none, {}});
  runtime.observeDispatch(
      propertyCommand(root.untyped(), 2u, ui::EditPhase::preview,
                      std::array<double, 3>{0.8, 0.0, 0.0}),
      {true, ui::ProjectionRefresh::none, {}});
  runtime.endFrame();
  check(!runtime.onFrameRendered() && runtime.failed() &&
            harness.published.size() == 1u &&
            runtime.error() ==
                "actual UI dispatch exceeds the current phase prefix",
        "non-U02 replay used observed dispatch count as its expected prefix");
}

ui::GraphRowKey gizmoRootKey() {
  return {typedId<EditorObjectKind::scene>(0u),
          rawId(EditorObjectKind::node, 0u), std::nullopt, 0u};
}

os::ReplayPlan gizmoPlan(bool drag = false) {
  os::ReplayPlan plan = singleRolePlan();
  plan.caseId = std::string(os::kU03GizmoCase);
  os::StepSpec &step = plan.steps.front();
  for (os::PhaseSpec &phase : step.phases) {
    phase.target = {os::TargetSemantic::gizmoTranslateY, "translate-y", "root"};
    phase.expectedDispatchCount = 0u;
    phase.gizmo = os::GizmoExpectation{1u, false, 0u, 0u, 0u, false};
  }
  if (!drag)
    return plan;
  step.effect = os::EffectKind::documentMutation;
  step.expectedRevisionDelta = 1;
  step.history = os::HistoryEffect::pushUndoClearRedo;
  const os::PhaseSpec first = step.phases.front();
  const os::PhaseSpec last = step.phases.back();
  step.phases = {first, first, first, first, last};
  for (std::size_t index = 0u; index < step.phases.size(); ++index) {
    auto &phase = step.phases[index];
    phase.ordinal = static_cast<std::uint32_t>(index);
    if (index != 0u && index + 1u != step.phases.size())
      phase.capture = os::CapturePoint::none;
  }
  step.phases[0].input = {os::InputKind::movePointer, {}};
  step.phases[1].id = "press";
  step.phases[1].phase = os::ProbePhase::hover;
  step.phases[1].requireHovered = true;
  step.phases[1].input = {os::InputKind::leftDown, {}};
  step.phases[2].id = "drag-begun";
  step.phases[2].phase = os::ProbePhase::dragTarget;
  step.phases[2].target = {os::TargetSemantic::viewportOutside,
                          "outside", "root"};
  step.phases[2].input = {os::InputKind::movePointer, {}};
  step.phases[2].gizmo = os::GizmoExpectation{2u, true, 1u, 0u, 0u, false};
  step.phases[3].id = "release";
  step.phases[3].phase = os::ProbePhase::dragTarget;
  step.phases[3].target = step.phases[2].target;
  step.phases[3].requireHovered = true;
  step.phases[3].input = {os::InputKind::leftUp, {}};
  step.phases[3].gizmo = os::GizmoExpectation{2u, true, 1u, 1u, 0u, true};
  step.phases[4].target = {os::TargetSemantic::client, "client", {}};
  step.phases[4].gizmo = os::GizmoExpectation{1u, false, 1u, 1u, 1u, true};
  return plan;
}

os::GizmoProbe gizmoSnapshot() {
  os::GizmoProbe probe;
  probe.renderedFrame = 1u;
  probe.inputFrame = 1u;
  probe.documentRevision = 10u;
  probe.selectionRevision = 1u;
  probe.mappingEpoch = 7u;
  probe.node = gizmoRootKey().object;
  probe.viewportRect = {60.0F, 50.0F, 320.0F, 280.0F};
  probe.componentRect = {160.0F, 110.0F, 175.0F, 125.0F};
  probe.outsideRect = {350.0F, 80.0F, 370.0F, 100.0F};
  probe.pointer = {165.0F, 115.0F};
  probe.hasPointer = true;
  probe.componentValid = true;
  probe.componentEnabled = true;
  probe.hoveredComponent = 1u;
  probe.activeComponent = 255u;
  probe.phase = 1u;
  return probe;
}

void configureGizmoHost(Harness &harness) {
  harness.host.gizmo = gizmoSnapshot();
  harness.host.authority.selectionKind = "Node";
  harness.host.authority.selectedObject = gizmoRootKey().object;
  harness.host.authority.selectedGraphRowKey = gizmoRootKey();
}

bool renderGizmo(os::EditorUiOsReplayRuntime &runtime, Harness &harness,
                 std::uint64_t serial) {
  if (harness.host.gizmo) {
    auto &gizmo = *harness.host.gizmo;
    gizmo.renderedFrame = serial;
    gizmo.inputFrame = serial;
    gizmo.documentRevision = harness.host.authority.documentRevision;
    gizmo.selectionRevision = harness.host.authority.selectionRevision;
  }
  render(runtime, binding(serial, harness.host.authority.documentRevision,
                          harness.host.authority.selectionRevision),
         {item(ui::EditorUiProbeSemantic::graphRow, gizmoRootKey(), "Root", 1u)});
  return runtime.onFrameRendered();
}

void acceptGizmoPhase(os::EditorUiOsReplayRuntime &runtime, Harness &harness,
                      os::InputKind kind, std::uint32_t count) {
  check(runtime.acceptContinue(receipt(runtime, harness.readyHash, kind, count)),
        "native gizmo phase continue did not advance");
}

void testGizmoSnapshotIdentityAndGeometryFailClosed() {
  for (std::uint32_t variant = 0u; variant < 16u; ++variant) {
    Harness harness;
    configureGizmoHost(harness);
    os::EditorUiOsReplayRuntime runtime(config(gizmoPlan()), harness.callbacks());
    check(!runtime.failed(), runtime.error());
    bool expectedFailure = true;
    auto &gizmo = *harness.host.gizmo;
    switch (variant) {
    case 0u: harness.host.gizmo.reset(); expectedFailure = false; break;
    case 1u: gizmo.renderedFrame = 0u; expectedFailure = false; break;
    case 2u: gizmo.renderedFrame = 2u; break;
    case 3u: gizmo.documentRevision = 11u; break;
    case 4u: gizmo.selectionRevision = 2u; break;
    case 5u: gizmo.node.reset(); expectedFailure = false; break;
    case 6u: gizmo.node->authority = EditorDocumentAuthority{12u}; break;
    case 7u: gizmo.node = rawId(EditorObjectKind::node, 99u); break;
    case 8u: gizmo.mappingEpoch = 0u; break;
    case 9u: gizmo.inputFrame = 0u; break;
    case 10u:
      gizmo.translation[1] = std::numeric_limits<double>::quiet_NaN(); break;
    case 11u: gizmo.viewportRect.minimumX = -10.0F; break;
    case 12u: gizmo.outsideRect = gizmo.componentRect; break;
    case 13u: gizmo.componentRect = gizmo.outsideRect; break;
    case 14u: gizmo.componentEnabled = false; expectedFailure = false; break;
    default: gizmo.componentValid = false; expectedFailure = false; break;
    }
    render(runtime, binding(1u),
           {item(ui::EditorUiProbeSemantic::graphRow, gizmoRootKey(), "Root", 1u)});
    check(!runtime.onFrameRendered() && runtime.failed() == expectedFailure &&
              harness.published.empty(),
          "missing, stale, foreign, or invalid native snapshot published evidence");
  }
}

void testGizmoHoverUsesActualPointerAndComponent() {
  for (std::uint32_t variant = 0u; variant < 3u; ++variant) {
    os::ReplayPlan plan = gizmoPlan();
    auto &phase = plan.steps.front().phases.front();
    phase.phase = os::ProbePhase::hover;
    phase.requireHovered = true;
    Harness harness;
    configureGizmoHost(harness);
    os::EditorUiOsReplayRuntime runtime(config(plan), harness.callbacks());
    check(!runtime.failed(), runtime.error());
    auto &gizmo = *harness.host.gizmo;
    if (variant == 0u)
      gizmo.hasPointer = false;
    else if (variant == 1u)
      gizmo.hoveredComponent = 0u;
    else
      gizmo.pointer = {100.0F, 100.0F};
    auto row = item(ui::EditorUiProbeSemantic::graphRow, gizmoRootKey(), "Root", 1u);
    row.hovered = row.focused = row.dropAccepted = true;
    render(runtime, binding(1u), {row});
    check(!runtime.onFrameRendered() && !runtime.failed() &&
              harness.published.empty(),
          "native hover was fabricated from expected flags or an ImGui row");
    harness.host.gizmo = gizmoSnapshot();
    check(renderGizmo(runtime, harness, 2u), runtime.error());
    const auto &target = harness.published.back().target;
    check(target.observationSource == "editor-gizmo-adapter-v1" &&
              target.graphRowKey == gizmoRootKey() && target.itemId == 0u &&
              target.windowToken == 0u && target.order == 0u &&
              target.hovered && !target.dropAccepted &&
              target.rect.left == 325 && target.rect.top == 380 &&
              target.rect.right == 348 && target.rect.bottom == 410,
          "native gizmo target lost its actual geometry or claimed ImGui identity");
  }
}

void beginNativeGizmoDrag(os::EditorUiOsReplayRuntime &runtime,
                          Harness &harness) {
  check(renderGizmo(runtime, harness, 1u), runtime.error());
  acceptGizmoPhase(runtime, harness, os::InputKind::movePointer, 1u);
  check(renderGizmo(runtime, harness, 2u), runtime.error());
  acceptGizmoPhase(runtime, harness, os::InputKind::leftDown, 1u);
}

void setGizmoDragging(Harness &harness) {
  auto &gizmo = *harness.host.gizmo;
  gizmo.phase = 2u;
  gizmo.captureHeld = true;
  gizmo.activeComponent = 1u;
  gizmo.beginCount = 1u;
}

void setGizmoPreview(Harness &harness) {
  auto &gizmo = *harness.host.gizmo;
  gizmo.previewCount = 1u;
  gizmo.translation[1] = 2.0;
  gizmo.pointer = {360.0F, 90.0F};
  gizmo.hoveredComponent = 255u;
  harness.host.authority.documentRevision = 11u;
  harness.host.authority.documentDigest = hash('4');
  harness.host.authority.projectionDigest = hash('5');
  harness.host.authority.runtimeContentDigest = hash('5');
}

void setGizmoCommitted(Harness &harness) {
  auto &gizmo = *harness.host.gizmo;
  gizmo.phase = 1u;
  gizmo.captureHeld = false;
  gizmo.activeComponent = 255u;
  gizmo.commitCount = 1u;
  gizmo.terminal = "commit";
  gizmo.lastTerminalFrame = 5u;
  gizmo.lastTerminalSequence = 42u;
  harness.host.authority.undoCount = 1u;
  harness.host.authority.undoDescription = "Translate Y";
}

void testNativeGizmoSequenceAndActionPrefixes() {
  Harness harness;
  configureGizmoHost(harness);
  os::EditorUiOsReplayRuntime runtime(config(gizmoPlan(true)), harness.callbacks());
  check(!runtime.failed(), runtime.error());
  beginNativeGizmoDrag(runtime, harness);
  check(!renderGizmo(runtime, harness, 3u) && !runtime.failed() &&
            runtime.pendingReason() == "gizmo-action-prefix-mismatch",
        "dragging phase advanced without a real begin callback");
  setGizmoDragging(harness);
  check(renderGizmo(runtime, harness, 4u), runtime.error());
  acceptGizmoPhase(runtime, harness, os::InputKind::movePointer, 1u);
  check(!renderGizmo(runtime, harness, 5u) && !runtime.failed(),
        "release phase advanced without a changed preview outside the viewport");
  setGizmoPreview(harness);
  check(renderGizmo(runtime, harness, 6u), runtime.error());
  const auto &outside = harness.published.back().target;
  check(outside.hovered && outside.rect.left == 610 && outside.rect.right == 640 &&
            outside.observationSource == "editor-gizmo-adapter-v1",
        "outside-release target was not bound to its observed outside rectangle");
  acceptGizmoPhase(runtime, harness, os::InputKind::leftUp, 1u);
  check(!renderGizmo(runtime, harness, 7u) && !runtime.failed(),
        "settled phase advanced before a release commit was observed");
  setGizmoCommitted(harness);
  harness.host.gizmo->lastTerminalFrame = 8u;
  check(renderGizmo(runtime, harness, 8u), runtime.error());
  check(harness.published.back().gizmo &&
            harness.published.back().gizmo->renderedFrame == 8u &&
            harness.published.back().gizmo->translation[1] == 2.0 &&
            harness.published.back().gizmo->commitCount == 1u &&
            harness.published.back().authority.undoCount == 1u,
        "native terminal evidence omitted actual transform or commit state");
  acceptGizmoPhase(runtime, harness, os::InputKind::none, 0u);
  check(runtime.complete(), "real native gizmo sequence did not complete");
}

void testNativeGizmoRejectsExcessActionsAndWrongTerminal() {
  for (std::uint32_t variant = 0u; variant < 5u; ++variant) {
    Harness harness;
    configureGizmoHost(harness);
    os::EditorUiOsReplayRuntime runtime(config(gizmoPlan(true)), harness.callbacks());
    beginNativeGizmoDrag(runtime, harness);
    setGizmoDragging(harness);
    auto &gizmo = *harness.host.gizmo;
    switch (variant) {
    case 0u: gizmo.beginCount = 2u; break;
    case 1u: gizmo.previewCount = 1u; break;
    case 2u: gizmo.detachCount = 1u; gizmo.terminal = "detach"; break;
    case 3u: gizmo.latchCount = 1u; gizmo.terminal = "latch"; break;
    default: gizmo.commitCount = 1u; gizmo.terminal = "commit"; break;
    }
    check(!renderGizmo(runtime, harness, 3u) && runtime.failed() &&
              harness.published.size() == 2u,
          "unexpected native action or terminal was accepted");
  }
}

void testNativeGizmoWaitsForActualCaptureAndPointerState() {
  Harness harness;
  configureGizmoHost(harness);
  os::EditorUiOsReplayRuntime runtime(config(gizmoPlan(true)), harness.callbacks());
  beginNativeGizmoDrag(runtime, harness);
  setGizmoDragging(harness);
  auto &gizmo = *harness.host.gizmo;
  gizmo.captureHeld = false;
  check(!renderGizmo(runtime, harness, 3u) && !runtime.failed(),
        "begin counter fabricated native pointer capture");
  gizmo.captureHeld = true;
  gizmo.activeComponent = 0u;
  check(!renderGizmo(runtime, harness, 4u) && !runtime.failed(),
        "native X-axis capture satisfied the Y-axis drag contract");
  gizmo.activeComponent = 1u;
  check(renderGizmo(runtime, harness, 5u), runtime.error());
  acceptGizmoPhase(runtime, harness, os::InputKind::movePointer, 1u);
  setGizmoPreview(harness);
  gizmo.pointer = {200.0F, 150.0F};
  check(!renderGizmo(runtime, harness, 6u) && !runtime.failed(),
        "outside release was inferred from preview count while pointer stayed inside");
  gizmo.pointer = {360.0F, 90.0F};
  check(renderGizmo(runtime, harness, 7u), runtime.error());
}

void testNativeGizmoRejectsWrongPreviewOrTerminalTransform() {
  for (std::uint32_t variant = 0u; variant < 10u; ++variant) {
    Harness harness;
    configureGizmoHost(harness);
    os::EditorUiOsReplayRuntime runtime(config(gizmoPlan(true)), harness.callbacks());
    beginNativeGizmoDrag(runtime, harness);
    setGizmoDragging(harness);
    check(renderGizmo(runtime, harness, 3u), runtime.error());
    acceptGizmoPhase(runtime, harness, os::InputKind::movePointer, 1u);
    setGizmoPreview(harness);
    auto &gizmo = *harness.host.gizmo;
    switch (variant) {
    case 0u: gizmo.translation[0] = 0.1; break;
    case 1u: gizmo.translation[2] = 0.1; break;
    case 2u: gizmo.rotation[0] = 0.1; break;
    case 3u: gizmo.scale[0] = 0.5; break;
    case 4u: gizmo.translation[1] = 0.0; break;
    default: break;
    }
    if (variant < 5u) {
      check(!renderGizmo(runtime, harness, 4u) && runtime.failed(),
            "meaningful Y preview accepted an unchanged Y or unrelated TRS edit");
      continue;
    }
    check(renderGizmo(runtime, harness, 4u), runtime.error());
    acceptGizmoPhase(runtime, harness, os::InputKind::leftUp, 1u);
    setGizmoCommitted(harness);
    if (variant == 5u) {
      harness.host.authority.selectionKind = "None";
      harness.host.authority.selectedObject.reset();
      harness.host.authority.selectedGraphRowKey.reset();
    } else if (variant == 6u) {
      gizmo.lastTerminalSequence = 0u;
    } else if (variant == 7u) {
      gizmo.translation[1] = 1.0;
    } else if (variant == 8u) {
      gizmo.lastTerminalFrame = 4u;
    } else {
      gizmo.lastTerminalFrame = 6u;
    }
    check(!renderGizmo(runtime, harness, 5u) && runtime.failed(),
          "gizmo release accepted selection drift or missing physical terminal identity");
  }
}

void testGizmoFrameSerialDoesNotPreventStableGeometry() {
  Harness harness;
  configureGizmoHost(harness);
  auto configuration = config(gizmoPlan());
  configuration.requiredStableFrames = 2u;
  os::EditorUiOsReplayRuntime runtime(std::move(configuration), harness.callbacks());
  check(!renderGizmo(runtime, harness, 1u) && !runtime.failed() &&
            runtime.pendingReason() == "target-geometry-stabilizing",
        "native target ignored the consecutive geometry requirement");
  check(renderGizmo(runtime, harness, 2u) && harness.published.size() == 1u &&
            harness.published.back().gizmo->renderedFrame == 2u,
          "changing native frame serial prevented stable target publication");
}

os::ReplayPlan escapeGizmoPlan() {
  os::ReplayPlan plan = gizmoPlan(true);
  plan.caseId = std::string(os::kU03EscapeCase);
  auto &drag = plan.steps.front();
  drag.id = "drag-escape-only";
  plan.graphRoles.front().introducedByStep = drag.id;
  drag.history = os::HistoryEffect::unchanged;
  drag.phases[3].id = "escape";
  drag.phases[3].input = {os::InputKind::keyDown, "Escape"};
  const os::GizmoExpectation detached{0u, false, 1u, 1u, 0u, true,
                                      1u, 0u, true};
  drag.phases.back().gizmo = detached;
  os::StepSpec late;
  late.index = 1u;
  late.id = "late-release";
  late.subcase = "main";
  late.phases = {drag.phases.back(), drag.phases.back(), drag.phases.back()};
  for (std::size_t index = 0u; index < late.phases.size(); ++index) {
    auto &phase = late.phases[index];
    phase.ordinal = static_cast<std::uint32_t>(drag.phases.size() + index);
    phase.capture = os::CapturePoint::none;
  }
  late.phases[0].id = "target";
  late.phases[0].phase = os::ProbePhase::target;
  late.phases[0].capture = os::CapturePoint::before;
  late.phases[0].input = {os::InputKind::keyUp, "Escape"};
  late.phases[1].id = "release";
  late.phases[1].phase = os::ProbePhase::dragTarget;
  late.phases[1].input = {os::InputKind::leftUp, {}};
  late.phases[2].capture = os::CapturePoint::after;
  plan.steps.push_back(std::move(late));
  return plan;
}

void prepareEscapeKeyDown(os::EditorUiOsReplayRuntime &runtime, Harness &harness) {
  beginNativeGizmoDrag(runtime, harness);
  setGizmoDragging(harness);
  harness.host.gizmo->gestureNode = gizmoRootKey().object;
  check(renderGizmo(runtime, harness, 3u), runtime.error());
  acceptGizmoPhase(runtime, harness, os::InputKind::movePointer, 1u);
  setGizmoPreview(harness);
  check(renderGizmo(runtime, harness, 4u), runtime.error());
  acceptGizmoPhase(runtime, harness, os::InputKind::keyDown, 1u);
}

void configureEscapeHost(Harness &harness) {
  configureGizmoHost(harness);
  harness.host.gizmo->ndcYDown = true;
}

void setGizmoEscapeDetached(Harness &harness, std::uint64_t terminalFrame = 5u) {
  auto &gizmo = *harness.host.gizmo;
  gizmo.node.reset();
  gizmo.phase = 0u;
  gizmo.captureHeld = false;
  gizmo.componentValid = false;
  gizmo.componentEnabled = false;
  gizmo.activeComponent = 255u;
  gizmo.detachCount = 1u;
  gizmo.escapePressCount = 1u;
  gizmo.terminal = "detach";
  gizmo.lastTerminalFrame = terminalFrame;
  gizmo.lastTerminalSequence = 42u;
  gizmo.lastTerminalEvents = {{"escape-pressed", 42u}};
  harness.host.authority.selectionKind = "None";
  harness.host.authority.selectedObject.reset();
  harness.host.authority.selectedGraphRowKey.reset();
  harness.host.authority.selectionRevision = 2u;
}

void testEscapeGizmoSequenceRetainsProofAfterLateRelease() {
  Harness harness;
  configureEscapeHost(harness);
  os::EditorUiOsReplayRuntime runtime(config(escapeGizmoPlan(), "main", 0u, 1u),
                                      harness.callbacks());
  check(!runtime.failed(), runtime.error());
  prepareEscapeKeyDown(runtime, harness);
  check(!renderGizmo(runtime, harness, 5u) && !runtime.failed() &&
            runtime.pendingReason() == "gizmo-action-prefix-mismatch",
        "Escape phase advanced before actual detach callback");
  harness.host.authority.selectionKind = "None";
  harness.host.authority.selectedObject.reset();
  harness.host.authority.selectedGraphRowKey.reset();
  check(!renderGizmo(runtime, harness, 6u) && !runtime.failed(),
        "UI selection clearing fabricated a missing detach callback");
  setGizmoEscapeDetached(harness, 7u);
  check(renderGizmo(runtime, harness, 7u), runtime.error());
  const auto detached = harness.published.back();
  check(detached.gizmo && !detached.gizmo->node &&
            detached.gizmo->gestureNode == gizmoRootKey().object &&
            detached.gizmo->translation[1] == 2.0 &&
            detached.gizmo->detachCount == 1u &&
            detached.gizmo->commitCount == 0u &&
            detached.authority.undoCount == 0u &&
            detached.authority.selectionKind == "None",
        "Escape ready evidence did not retain the real no-history gesture");
  acceptGizmoPhase(runtime, harness, os::InputKind::none, 0u);
  check(renderGizmo(runtime, harness, 8u), runtime.error());
  acceptGizmoPhase(runtime, harness, os::InputKind::keyUp, 1u);
  check(renderGizmo(runtime, harness, 9u), runtime.error());
  acceptGizmoPhase(runtime, harness, os::InputKind::leftUp, 1u);
  check(renderGizmo(runtime, harness, 10u), runtime.error());
  const auto &late = *harness.published.back().gizmo;
  check(late.lastTerminalFrame == 7u && late.lastTerminalSequence == 42u &&
            late.lastTerminalEvents.size() == 1u &&
            late.lastTerminalEvents[0].kind == "escape-pressed" &&
            late.deliveredReleaseCount == 0u && late.escapePressCount == 1u &&
            late.commitCount == 0u && late.translation == detached.gizmo->translation,
        "late release replaced Escape proof or changed retained preview");
  acceptGizmoPhase(runtime, harness, os::InputKind::none, 0u);
  check(runtime.complete(), "Escape and late-release sequence did not complete");
}

void testEscapeGizmoRejectsForgedTerminalProof() {
  for (std::uint32_t variant = 0u; variant < 32u; ++variant) {
    Harness harness;
    configureEscapeHost(harness);
    os::EditorUiOsReplayRuntime runtime(config(escapeGizmoPlan()), harness.callbacks());
    check(!runtime.failed(), runtime.error());
    prepareEscapeKeyDown(runtime, harness);
    setGizmoEscapeDetached(harness);
    auto &gizmo = *harness.host.gizmo;
    switch (variant) {
    case 0u: gizmo.gestureNode.reset(); break;
    case 1u: gizmo.gestureNode->authority = EditorDocumentAuthority{9u}; break;
    case 2u: gizmo.gestureNode = rawId(EditorObjectKind::node, 1u); break;
    case 3u: gizmo.lastTerminalEvents[0].kind = "delivered-release"; break;
    case 4u: gizmo.lastTerminalEvents[0].sequence = 0u; break;
    case 5u: gizmo.lastTerminalEvents[0].sequence = 41u; break;
    case 6u: gizmo.lastTerminalFrame = 4u; break;
    case 7u: gizmo.lastTerminalFrame = 6u; break;
    case 8u: gizmo.deliveredReleaseCount = 1u; break;
    case 9u: gizmo.focusLossCount = 1u; break;
    case 10u: gizmo.suppressedReleaseCount = 1u; break;
    case 11u: gizmo.escapePressCount = 2u; break;
    case 12u: gizmo.lastTerminalEventsOverflowed = true; break;
    case 13u: gizmo.node = gizmoRootKey().object; break;
    case 14u: harness.host.authority.selectionKind = "Node"; break;
    case 15u: harness.host.authority.selectedObject = gizmoRootKey().object; break;
    case 16u: harness.host.authority.selectedGraphRowKey = gizmoRootKey(); break;
    case 17u: harness.host.authority.undoCount = 1u; break;
    case 18u: harness.host.authority.redoDescription = "Unexpected history"; break;
    case 19u: gizmo.translation[1] = 1.0; break;
    case 20u: gizmo.translation[0] = 0.1; break;
    case 21u: gizmo.rotation[0] = 0.1; break;
    case 22u: gizmo.scale[0] = 0.5; break;
    case 23u: gizmo.lastTerminalEvents.clear(); break;
    case 24u: gizmo.escapePressCount = 0u; break;
    case 25u: harness.host.authority.selectionRevision = 1u; break;
    case 26u: harness.host.authority.documentRevision = 12u; break;
    case 27u: harness.host.authority.undoDescription = "Unexpected history"; break;
    case 28u: gizmo.captureHeld = true; break;
    case 29u: gizmo.phase = 2u; break;
    case 30u: gizmo.activeComponent = 1u; break;
    default: gizmo.ndcYDown = false; break;
    }
    check(!renderGizmo(runtime, harness, 5u) && runtime.failed(),
          "Escape accepted forged identity, journal, frame, transform, or history proof");
  }
}

void testEscapeGizmoRejectsPreEscapeEventsAndLateDrift() {
  for (std::uint32_t variant = 0u; variant < 4u; ++variant) {
    Harness harness;
    configureEscapeHost(harness);
    os::EditorUiOsReplayRuntime runtime(config(escapeGizmoPlan()), harness.callbacks());
    beginNativeGizmoDrag(runtime, harness);
    setGizmoDragging(harness);
    auto &gizmo = *harness.host.gizmo;
    gizmo.gestureNode = gizmoRootKey().object;
    if (variant == 0u) gizmo.gestureNode.reset();
    if (variant == 1u) gizmo.gestureNode = rawId(EditorObjectKind::node, 1u);
    if (variant == 2u) gizmo.escapePressCount = 1u;
    if (variant == 3u) gizmo.deliveredReleaseCount = 1u;
    check(!renderGizmo(runtime, harness, 3u) && runtime.failed(),
          "pre-Escape phase accepted missing gesture identity or terminal events");
  }
  for (std::uint32_t variant = 0u; variant < 9u; ++variant) {
    Harness harness;
    configureEscapeHost(harness);
    os::EditorUiOsReplayRuntime runtime(config(escapeGizmoPlan(), "main", 0u, 1u),
                                        harness.callbacks());
    prepareEscapeKeyDown(runtime, harness);
    setGizmoEscapeDetached(harness);
    check(renderGizmo(runtime, harness, 5u), runtime.error());
    acceptGizmoPhase(runtime, harness, os::InputKind::none, 0u);
    check(renderGizmo(runtime, harness, 6u), runtime.error());
    acceptGizmoPhase(runtime, harness, os::InputKind::keyUp, 1u);
    check(renderGizmo(runtime, harness, 7u), runtime.error());
    acceptGizmoPhase(runtime, harness, os::InputKind::leftUp, 1u);
    auto &gizmo = *harness.host.gizmo;
    switch (variant) {
    case 0u: gizmo.deliveredReleaseCount = 1u; break;
    case 1u: gizmo.translation[1] += 0.1; break;
    case 2u: harness.host.authority.documentRevision += 1u; break;
    case 3u: harness.host.authority.selectionRevision += 1u; break;
    case 4u: harness.host.authority.documentDigest = hash('6'); break;
    case 5u: gizmo.lastTerminalEvents[0].sequence += 1u;
             gizmo.lastTerminalSequence += 1u; break;
    case 6u: gizmo.gestureNode = rawId(EditorObjectKind::node, 1u); break;
    case 7u: gizmo.node = gizmoRootKey().object; break;
    default: harness.host.authority.redoCount = 1u; break;
    }
    check(!renderGizmo(runtime, harness, 8u) && runtime.failed(),
          "late-release phase changed Escape authority, journal, or history");
  }
}

os::ReplayPlan escapePreludePlan(bool clientTarget = false) {
  auto plan = singleRolePlan();
  plan.caseId = std::string(os::kU03EscapeCase);
  plan.steps.front().effect = os::EffectKind::selectionOnly;
  for (auto &phase : plan.steps.front().phases) {
    phase.expectedDispatchCount = 0u;
    if (clientTarget)
      phase.target = {os::TargetSemantic::client, "client", {}};
  }
  return plan;
}

void testEscapePreludeRequiresFreshEmptyObservation() {
  for (const bool clientTarget : {false, true}) {
    for (std::uint32_t variant = 0u; variant < 32u; ++variant) {
      Harness harness;
      configureEscapeHost(harness);
      os::EditorUiOsReplayRuntime runtime(config(escapePreludePlan(clientTarget)),
                                          harness.callbacks());
      check(!runtime.failed(), runtime.error());
      bool expectedFailure = true;
      auto &gizmo = *harness.host.gizmo;
      switch (variant) {
      case 0u: harness.host.gizmo.reset(); expectedFailure = false; break;
      case 1u: gizmo.renderedFrame = 0u; expectedFailure = false; break;
      case 2u: gizmo.renderedFrame = 2u; break;
      case 3u: gizmo.documentRevision = 9u; expectedFailure = false; break;
      case 4u: gizmo.selectionRevision = 0u; expectedFailure = false; break;
      case 5u: gizmo.inputFrame = 0u; break;
      case 6u: gizmo.mappingEpoch = 0u; break;
      case 7u: gizmo.captureHeld = true; break;
      case 8u: gizmo.activeComponent = 1u; break;
      case 9u: gizmo.phase = 2u; break;
      case 10u: gizmo.beginCount = 1u; break;
      case 11u: gizmo.previewCount = 1u; break;
      case 12u: gizmo.commitCount = 1u; break;
      case 13u: gizmo.detachCount = 1u; break;
      case 14u: gizmo.latchCount = 1u; break;
      case 15u: gizmo.escapePressCount = 1u; break;
      case 16u: gizmo.deliveredReleaseCount = 1u; break;
      case 17u: gizmo.focusLossCount = 1u; break;
      case 18u: gizmo.suppressedReleaseCount = 1u; break;
      case 19u: gizmo.lastTerminalEvents = {{"escape-pressed", 1u}}; break;
      case 20u: gizmo.lastTerminalEventsOverflowed = true; break;
      case 21u: gizmo.terminal = "detach"; break;
      case 22u: gizmo.lastTerminalFrame = 1u; break;
      case 23u: gizmo.lastTerminalSequence = 1u; break;
      case 24u: gizmo.gestureNode = gizmoRootKey().object; break;
      case 25u: gizmo.node->authority = EditorDocumentAuthority{9u}; break;
      case 26u: gizmo.translation[1] = std::numeric_limits<double>::quiet_NaN(); break;
      case 27u: harness.host.authority.undoDescription = "Unexpected history"; break;
      case 28u: harness.host.authority.undoCount = 1u; break;
      case 29u: gizmo.ndcYDown = false; break;
      case 30u: gizmo.node = rawId(EditorObjectKind::node, 9u);
                expectedFailure = false; break;
      default: gizmo.documentRevision = 11u; break;
      }
      render(runtime, binding(1u),
             {item(ui::EditorUiProbeSemantic::graphRow, gizmoRootKey(), "Root", 1u)});
      check(!runtime.onFrameRendered() && runtime.failed() == expectedFailure &&
                harness.published.empty(),
            "Escape prelude certified absent, stale, active, or inconsistent observation");
    }
  }
}

void testEscapePreludeWaitsForSelectionAttachmentSynchronization() {
  Harness harness;
  configureEscapeHost(harness);
  harness.host.gizmo->node.reset();
  harness.host.gizmo->phase = 0u;
  harness.host.authority.selectionKind = "None";
  harness.host.authority.selectedObject.reset();
  harness.host.authority.selectedGraphRowKey.reset();
  os::EditorUiOsReplayRuntime runtime(config(escapePreludePlan()), harness.callbacks());
  check(renderGizmo(runtime, harness, 1u), runtime.error());
  check(harness.published.back().gizmo && !harness.published.back().gizmo->node,
        "Escape bootstrap invented a selected node");
  acceptGizmoPhase(runtime, harness, os::InputKind::none, 0u);
  harness.host.gizmo->node = gizmoRootKey().object;
  harness.host.gizmo->phase = 1u;
  check(!renderGizmo(runtime, harness, 2u) && !runtime.failed() &&
            runtime.pendingReason() == "gizmo-node-selection-synchronizing",
        "Escape click transient certified an attachment before selection caught up");
  harness.host.authority.selectionKind = "Node";
  harness.host.authority.selectedObject = gizmoRootKey().object;
  harness.host.authority.selectedGraphRowKey = gizmoRootKey();
  harness.host.authority.selectionRevision = 2u;
  check(renderGizmo(runtime, harness, 3u), runtime.error());
  acceptGizmoPhase(runtime, harness, os::InputKind::none, 0u);
  check(runtime.complete(), "Escape prelude did not accept synchronized real selection");

  Harness regressed;
  configureEscapeHost(regressed);
  regressed.host.gizmo->inputFrame = 10u;
  os::EditorUiOsReplayRuntime staleRuntime(config(escapePreludePlan()),
                                           regressed.callbacks());
  render(staleRuntime, binding(1u),
         {item(ui::EditorUiProbeSemantic::graphRow, gizmoRootKey(), "Root", 1u)});
  check(staleRuntime.onFrameRendered(), staleRuntime.error());
  acceptGizmoPhase(staleRuntime, regressed, os::InputKind::none, 0u);
  regressed.host.gizmo->renderedFrame = 2u;
  regressed.host.gizmo->inputFrame = 9u;
  render(staleRuntime, binding(2u),
         {item(ui::EditorUiProbeSemantic::graphRow, gizmoRootKey(), "Root", 1u)});
  check(!staleRuntime.onFrameRendered() && staleRuntime.failed(),
        "Escape prelude accepted a regressed input frame");
}

os::ReplayPlan sameFrameGizmoPlan(bool escapeFirst) {
  auto plan = escapeGizmoPlan();
  plan.caseId = std::string(escapeFirst ? os::kU03EscapeReleaseCase
                                      : os::kU03ReleaseEscapeCase);
  auto &drag = plan.steps.front();
  drag.id = "drag-same-frame";
  plan.graphRoles.front().introducedByStep = drag.id;
  drag.phases[3].id = "terminal-batch";
  drag.phases[3].input = {os::InputKind::escapeReleaseBatch,
                          escapeFirst ? "escape-first" : "release-first"};
  return plan;
}

void configureSameFrameHost(Harness &harness) {
  configureEscapeHost(harness);
  harness.host.gizmo->nativeCallbackSequence = 40u;
}

void renderFocusGizmo(os::EditorUiOsReplayRuntime &runtime, Harness &harness,
                      std::uint64_t serial) {
  if (harness.host.gizmo) {
    auto &gizmo = *harness.host.gizmo;
    gizmo.renderedFrame = serial;
    gizmo.inputFrame = serial;
    gizmo.documentRevision = harness.host.authority.documentRevision;
    gizmo.selectionRevision = harness.host.authority.selectionRevision;
  }
  render(runtime,
         binding(serial, harness.host.authority.documentRevision,
                 harness.host.authority.selectionRevision),
         {item(ui::EditorUiProbeSemantic::graphRow, gizmoRootKey(),
               "Selected", 1u)});
}

void setFocusLatched(Harness &harness, std::uint64_t frame,
                     std::uint64_t sequence, std::uint64_t callbackSequence,
                     std::uint64_t suppressedCount,
                     bool pointerOutside = true) {
  auto &gizmo = *harness.host.gizmo;
  gizmo.phase = 3u;
  gizmo.captureHeld = false;
  gizmo.activeComponent = 255u;
  gizmo.latchCount = 1u;
  gizmo.commitCount = 0u;
  gizmo.detachCount = 0u;
  gizmo.deliveredReleaseCount = 0u;
  gizmo.escapePressCount = 0u;
  gizmo.focusLossCount = 1u;
  gizmo.suppressedReleaseCount = suppressedCount;
  gizmo.terminal = "latch";
  gizmo.lastTerminalFrame = frame;
  gizmo.lastTerminalSequence = sequence;
  gizmo.nativeCallbackSequence = callbackSequence;
  gizmo.hasPointer = pointerOutside;
  if (pointerOutside)
    gizmo.pointer = {360.0F, 90.0F};
  gizmo.lastTerminalEvents = suppressedCount == 0u
                                 ? std::vector<os::GizmoTerminalProbe>{
                                       {"focus-lost", sequence,
                                        callbackSequence}}
                                 : std::vector<os::GizmoTerminalProbe>{
                                       {"suppressed-physical-mouse-up",
                                        sequence, callbackSequence}};
}

void testFocusLossLatestBatchJournalTiming() {
  {
    Harness forged;
    configureEscapeHost(forged);
    forged.host.gizmo->gestureNode = gizmoRootKey().object;
    const os::ReplayPlan forgedPlan =
        os::makeU03FocusOsReplayPlan(os::kU03FocusNoReleaseCase);
    os::EditorUiOsReplayRuntime forgedRuntime(
        config(forgedPlan, "main", 2u, 2u), forged.callbacks());
    renderFocusGizmo(forgedRuntime, forged, 1u);
    check(!forgedRuntime.onFrameRendered() && forgedRuntime.failed(),
          "focus-loss pre-terminal observation invented a gesture identity");
  }
  Harness harness;
  configureEscapeHost(harness);
  const os::ReplayPlan plan =
      os::makeU03FocusOsReplayPlan(os::kU03FocusNoReleaseCase);
  os::EditorUiOsReplayRuntime runtime(config(plan, "main", 2u, 2u),
                                      harness.callbacks());
  check(!runtime.failed(), runtime.error());

  // The drag target starts with a bound node and an idle gizmo.
  renderFocusGizmo(runtime, harness, 1u);
  check(runtime.onFrameRendered(), runtime.error());
  acceptGizmoPhase(runtime, harness, os::InputKind::movePointer, 1u);

  renderFocusGizmo(runtime, harness, 2u);
  check(runtime.onFrameRendered(), runtime.error());
  acceptGizmoPhase(runtime, harness, os::InputKind::leftDown, 1u);

  setGizmoDragging(harness);
  harness.host.gizmo->gestureNode = gizmoRootKey().object;
  renderFocusGizmo(runtime, harness, 3u);
  check(runtime.onFrameRendered(), runtime.error());
  acceptGizmoPhase(runtime, harness, os::InputKind::movePointer, 1u);

  setGizmoPreview(harness);
  renderFocusGizmo(runtime, harness, 4u);
  check(runtime.onFrameRendered(), runtime.error());
  acceptGizmoPhase(runtime, harness, os::InputKind::focusLoss, 0u);

  // WM_KILLFOCUS has no mouse-up callback.  The first post-loss ready must
  // contain only the focus event, not a fabricated cumulative release.
  setFocusLatched(harness, 5u, 40u, 40u, 0u, false);
  renderFocusGizmo(runtime, harness, 5u);
  check(runtime.onFrameRendered(), runtime.error());
  check(harness.published.back().gizmo->lastTerminalEvents.size() == 1u &&
            harness.published.back().gizmo->lastTerminalEvents[0].kind ==
                "focus-lost",
        "focus-loss ready incorrectly accumulated a suppressed release");
  acceptGizmoPhase(runtime, harness, os::InputKind::focusRestore, 0u);

  // After focus restoration, the callback that followed the focus marker is
  // replayed on the first independent focused frame. Its original ordinal is
  // intentionally equal to the already-published native clock, but the
  // suppression-only provenance remains explicit and must be accepted.
  setFocusLatched(harness, 6u, 41u, 40u, 1u);
  renderFocusGizmo(runtime, harness, 6u);
  check(runtime.onFrameRendered(), runtime.error());
  check(harness.published.back().gizmo->lastTerminalEvents.size() == 1u &&
            harness.published.back().gizmo->lastTerminalEvents[0].kind ==
                "suppressed-physical-mouse-up",
        "first post-restore release did not replace the latest terminal batch");
  acceptGizmoPhase(runtime, harness, os::InputKind::leftUp, 1u);

  setFocusLatched(harness, 7u, 42u, 42u, 2u);
  renderFocusGizmo(runtime, harness, 7u);
  check(runtime.onFrameRendered(), runtime.error());
  acceptGizmoPhase(runtime, harness, os::InputKind::none, 0u);
  check(runtime.complete(), "focus-loss latest-batch timing replay did not complete");
}

void testFocusTerminalRejectsRecycledCallbackOrdinal() {
  Harness harness;
  configureEscapeHost(harness);
  const os::ReplayPlan plan =
      os::makeU03FocusOsReplayPlan(os::kU03FocusNoReleaseCase);
  os::EditorUiOsReplayRuntime runtime(config(plan, "main", 2u, 2u),
                                      harness.callbacks());
  check(!runtime.failed(), runtime.error());

  // Establish the accepted preview boundary with a non-zero native callback
  // clock, then reuse that exact ordinal for the next focus terminal.
  renderFocusGizmo(runtime, harness, 1u);
  check(runtime.onFrameRendered(), runtime.error());
  acceptGizmoPhase(runtime, harness, os::InputKind::movePointer, 1u);
  renderFocusGizmo(runtime, harness, 2u);
  check(runtime.onFrameRendered(), runtime.error());
  acceptGizmoPhase(runtime, harness, os::InputKind::leftDown, 1u);
  setGizmoDragging(harness);
  harness.host.gizmo->gestureNode = gizmoRootKey().object;
  harness.host.gizmo->nativeCallbackSequence = 10u;
  renderFocusGizmo(runtime, harness, 3u);
  check(runtime.onFrameRendered(), runtime.error());
  acceptGizmoPhase(runtime, harness, os::InputKind::movePointer, 1u);

  setGizmoPreview(harness);
  harness.host.gizmo->nativeCallbackSequence = 10u;
  renderFocusGizmo(runtime, harness, 4u);
  check(runtime.onFrameRendered(), runtime.error());
  acceptGizmoPhase(runtime, harness, os::InputKind::focusLoss, 0u);

  setFocusLatched(harness, 5u, 40u, 10u, 0u, false);
  renderFocusGizmo(runtime, harness, 5u);
  check(!runtime.onFrameRendered() && runtime.failed(),
        "focus terminal reused a callback ordinal from the previous ready boundary");
}

void prepareSameFrameBatch(os::EditorUiOsReplayRuntime &runtime, Harness &harness) {
  beginNativeGizmoDrag(runtime, harness);
  setGizmoDragging(harness);
  harness.host.gizmo->gestureNode = gizmoRootKey().object;
  check(renderGizmo(runtime, harness, 3u), runtime.error());
  acceptGizmoPhase(runtime, harness, os::InputKind::movePointer, 1u);
  setGizmoPreview(harness);
  check(renderGizmo(runtime, harness, 4u), runtime.error());
  check(!runtime.acceptContinue(receipt(runtime, harness.readyHash,
                                        os::InputKind::escapeReleaseBatch, 1u)),
        "same-frame batch accepted a one-event SendInput receipt");
  acceptGizmoPhase(runtime, harness, os::InputKind::escapeReleaseBatch, 2u);
}

void setSameFrameDetached(Harness &harness, bool escapeFirst,
                          std::uint64_t terminalFrame = 5u) {
  setGizmoEscapeDetached(harness, terminalFrame);
  auto &gizmo = *harness.host.gizmo;
  gizmo.deliveredReleaseCount = 1u;
  gizmo.nativeCallbackSequence = 42u;
  gizmo.lastTerminalSequence = escapeFirst ? 101u : 102u;
  gizmo.lastTerminalEvents = {
      {escapeFirst ? "escape-pressed" : "delivered-release", 101u, 41u},
      {escapeFirst ? "delivered-release" : "escape-pressed", 102u, 42u}};
}

void testSameFrameGizmoBothNativeOrdersAndLateRelease() {
  for (const bool escapeFirst : {false, true}) {
    Harness harness;
    configureSameFrameHost(harness);
    os::EditorUiOsReplayRuntime runtime(
        config(sameFrameGizmoPlan(escapeFirst), "main", 0u, 1u), harness.callbacks());
    check(!runtime.failed(), runtime.error());
    prepareSameFrameBatch(runtime, harness);
    check(!renderGizmo(runtime, harness, 5u) && !runtime.failed(),
          "same-frame phase advanced without the actual native event batch");
    setSameFrameDetached(harness, escapeFirst, 6u);
    check(renderGizmo(runtime, harness, 6u), runtime.error());
    const auto detached = *harness.published.back().gizmo;
    check(detached.lastTerminalEvents.size() == 2u &&
              detached.lastTerminalEvents[0].callbackSequence == 41u &&
              detached.lastTerminalEvents[1].callbackSequence == 42u &&
              detached.lastTerminalSequence == (escapeFirst ? 101u : 102u) &&
              detached.detachCount == 1u && detached.commitCount == 0u &&
              harness.published.back().authority.undoCount == 0u,
          "same-frame terminal did not preserve native order and no-history detach");
    acceptGizmoPhase(runtime, harness, os::InputKind::none, 0u);
    check(renderGizmo(runtime, harness, 7u), runtime.error());
    acceptGizmoPhase(runtime, harness, os::InputKind::keyUp, 1u);
    harness.host.gizmo->nativeCallbackSequence = 43u;
    check(renderGizmo(runtime, harness, 8u), runtime.error());
    acceptGizmoPhase(runtime, harness, os::InputKind::leftUp, 1u);
    harness.host.gizmo->nativeCallbackSequence = 44u;
    check(renderGizmo(runtime, harness, 9u), runtime.error());
    const auto &after = *harness.published.back().gizmo;
    check(after.nativeCallbackSequence == 44u && after.deliveredReleaseCount == 1u &&
              after.lastTerminalFrame == 6u &&
              after.lastTerminalEvents[0].callbackSequence == 41u &&
              after.lastTerminalEvents[1].callbackSequence == 42u &&
              after.translation == detached.translation && !after.node &&
              after.gestureNode == detached.gestureNode,
          "late native callbacks replaced the retained same-frame journal");
    acceptGizmoPhase(runtime, harness, os::InputKind::none, 0u);
    check(runtime.complete(), "same-frame native order replay did not complete");
  }
}

void testSameFrameGizmoRejectsForgedNativeBatch() {
  for (const bool escapeFirst : {false, true}) {
    for (std::uint32_t variant = 0u; variant < 27u; ++variant) {
      Harness harness;
      configureSameFrameHost(harness);
      os::EditorUiOsReplayRuntime runtime(config(sameFrameGizmoPlan(escapeFirst)),
                                          harness.callbacks());
      prepareSameFrameBatch(runtime, harness);
      setSameFrameDetached(harness, escapeFirst);
      auto &gizmo = *harness.host.gizmo;
      switch (variant) {
      case 0u: std::swap(gizmo.lastTerminalEvents[0], gizmo.lastTerminalEvents[1]); break;
      case 1u: gizmo.lastTerminalEvents.pop_back(); break;
      case 2u: gizmo.lastTerminalEvents[0].callbackSequence = 0u; break;
      case 3u: gizmo.lastTerminalEvents[1].callbackSequence = 0u; break;
      case 4u: gizmo.lastTerminalEvents[1].callbackSequence = 41u; break;
      case 5u: gizmo.lastTerminalEvents[0].callbackSequence = 40u; break;
      case 6u: gizmo.nativeCallbackSequence = 41u; break;
      case 7u: gizmo.lastTerminalFrame = 4u; break;
      case 8u: gizmo.lastTerminalFrame = 6u; break;
      case 9u: gizmo.lastTerminalEvents[0].sequence = 0u; break;
      case 10u: gizmo.lastTerminalEvents[1].sequence = 101u; break;
      case 11u: gizmo.lastTerminalEvents[1].sequence = 100u; break;
      case 12u: gizmo.lastTerminalSequence = escapeFirst ? 102u : 101u; break;
      case 13u: gizmo.deliveredReleaseCount = 0u; break;
      case 14u: gizmo.deliveredReleaseCount = 2u; break;
      case 15u: gizmo.escapePressCount = 0u; break;
      case 16u: gizmo.escapePressCount = 2u; break;
      case 17u: gizmo.focusLossCount = 1u; break;
      case 18u: gizmo.suppressedReleaseCount = 1u; break;
      case 19u: gizmo.lastTerminalEventsOverflowed = true; break;
      case 20u: gizmo.terminal = "commit"; gizmo.commitCount = 1u; break;
      case 21u: gizmo.gestureNode.reset(); break;
      case 22u: gizmo.translation[1] = 1.0; break;
      case 23u: harness.host.authority.undoCount = 1u; break;
      case 24u: gizmo.lastTerminalEvents[0].callbackSequence = 43u; break;
      case 25u: gizmo.nativeCallbackSequence = 39u; break;
      default: gizmo.ndcYDown = false; break;
      }
      check(!renderGizmo(runtime, harness, 5u) && runtime.failed(),
            "same-frame replay accepted forged order, callback identity, frame, or terminal");
    }
  }
}

void testSameFrameGizmoRejectsLateJournalMutation() {
  for (const bool escapeFirst : {false, true}) {
    for (std::uint32_t variant = 0u; variant < 6u; ++variant) {
      Harness harness;
      configureSameFrameHost(harness);
      os::EditorUiOsReplayRuntime runtime(
          config(sameFrameGizmoPlan(escapeFirst), "main", 0u, 1u), harness.callbacks());
      prepareSameFrameBatch(runtime, harness);
      setSameFrameDetached(harness, escapeFirst);
      check(renderGizmo(runtime, harness, 5u), runtime.error());
      acceptGizmoPhase(runtime, harness, os::InputKind::none, 0u);
      check(renderGizmo(runtime, harness, 6u), runtime.error());
      acceptGizmoPhase(runtime, harness, os::InputKind::keyUp, 1u);
      auto &gizmo = *harness.host.gizmo;
      gizmo.nativeCallbackSequence = 100u;
      switch (variant) {
      case 0u: gizmo.lastTerminalEvents[0].callbackSequence += 1u;
               gizmo.lastTerminalEvents[1].callbackSequence += 1u; break;
      case 1u: gizmo.lastTerminalEvents[0].sequence += 2u;
               gizmo.lastTerminalEvents[1].sequence += 2u;
               gizmo.lastTerminalSequence += 2u; break;
      case 2u: gizmo.lastTerminalFrame = 6u; break;
      case 3u: gizmo.nativeCallbackSequence = 41u; break;
      case 4u: gizmo.deliveredReleaseCount = 2u; break;
      default: gizmo.translation[1] += 0.1; break;
      }
      check(!renderGizmo(runtime, harness, 7u) && runtime.failed(),
            "later callback rewrote same-frame journal or retained gesture evidence");
    }
  }
}

void testSameFramePreludeRetainsEscapeRigor() {
  for (const bool escapeFirst : {false, true}) {
    auto plan = escapePreludePlan(true);
    plan.caseId = std::string(escapeFirst ? os::kU03EscapeReleaseCase
                                        : os::kU03ReleaseEscapeCase);
    Harness missing;
    os::EditorUiOsReplayRuntime missingRuntime(config(plan), missing.callbacks());
    check(!renderGizmo(missingRuntime, missing, 1u) && !missingRuntime.failed() &&
              missingRuntime.pendingReason() == "gizmo-snapshot-unavailable",
          "same-frame bootstrap certified missing real gizmo observation");
    Harness unexpected;
    configureSameFrameHost(unexpected);
    unexpected.host.gizmo->deliveredReleaseCount = 1u;
    os::EditorUiOsReplayRuntime unexpectedRuntime(config(plan), unexpected.callbacks());
    check(!renderGizmo(unexpectedRuntime, unexpected, 1u) && unexpectedRuntime.failed(),
          "same-frame prelude accepted terminal events before its native batch");
  }
}

} // namespace

int main() {
  try {
    testRenderedTimingDpiAndContinue();
    testConfiguredConsecutiveTargetStability();
    testSplitRightButtonInputCounts();
    testDynamicRoleIntroduction();
    testRoleConstraintsAndAmbiguity();
    testDragDispatchPrefixAndNoopTerminal(false);
    testDragDispatchPrefixAndNoopTerminal(true);
    testProcessRangesAndFrameOrder();
    testUnsupportedPassiveTargetsFailClosed();
    testClientInteractionFlagsAreObserved();
    testInspectorRealItemStateAndAmbiguity();
    testPropertyDispatchRejectsWrongPayloads();
    testInspectorDispatchPrefixWaitsForActualCommands(false);
    testInspectorDispatchPrefixWaitsForActualCommands(true);
    testInspectorDispatchPrefixRejectsMissingContractAndEarlyDispatch();
    testGizmoSnapshotIdentityAndGeometryFailClosed();
    testGizmoHoverUsesActualPointerAndComponent();
    testNativeGizmoSequenceAndActionPrefixes();
    testNativeGizmoRejectsExcessActionsAndWrongTerminal();
    testNativeGizmoWaitsForActualCaptureAndPointerState();
    testNativeGizmoRejectsWrongPreviewOrTerminalTransform();
    testGizmoFrameSerialDoesNotPreventStableGeometry();
    testEscapeGizmoSequenceRetainsProofAfterLateRelease();
    testEscapeGizmoRejectsForgedTerminalProof();
    testEscapeGizmoRejectsPreEscapeEventsAndLateDrift();
    testEscapePreludeRequiresFreshEmptyObservation();
    testEscapePreludeWaitsForSelectionAttachmentSynchronization();
    testFocusLossLatestBatchJournalTiming();
    testFocusTerminalRejectsRecycledCallbackOrdinal();
    testSameFrameGizmoBothNativeOrdersAndLateRelease();
    testSameFrameGizmoRejectsForgedNativeBatch();
    testSameFrameGizmoRejectsLateJournalMutation();
    testSameFramePreludeRetainsEscapeRigor();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "Editor UI OS replay runtime tests passed.\n";
  return EXIT_SUCCESS;
}
