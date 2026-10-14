#include "../../app/editor/EditorTestDriver.h"

#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace demo::editor;
namespace testing = demo::editor::testing;
namespace ui = demo::editor::ui;

std::size_t g_checks = 0;
constexpr EditorDocumentAuthority kAuthority{0x4452495645525445ULL};

bool expect(bool condition, std::string_view message) {
  ++g_checks;
  if (!condition)
    std::cerr << "FAILED: " << message << '\n';
  return condition;
}

std::size_t countOccurrences(std::string_view text, std::string_view needle) {
  std::size_t count = 0;
  for (std::size_t offset = 0;
       (offset = text.find(needle, offset)) != std::string_view::npos;
       offset += needle.size())
    ++count;
  return count;
}

ui::FrameModel readyFrame() {
  ui::FrameModel frame;
  frame.hasDocument = true;
  frame.mutationEnabled = true;
  frame.revision = 1u;
  frame.cleanRevision = 1u;
  frame.lifecycle = static_cast<ui::LifecycleState>(2u);
  return frame;
}

EditorObjectId rawId(EditorObjectKind kind, std::uint32_t slot) {
  return EditorObjectId{kind, slot, 1u, kAuthority};
}

template <EditorObjectKind Kind>
TypedEditorObjectId<Kind> typedId(std::uint32_t slot) {
  return *TypedEditorObjectId<Kind>::from(rawId(Kind, slot));
}

testing::ScenarioBindings testBindings() {
  testing::ScenarioBindings bindings;
  bindings.primaryDocument = "valid.gltf";
  bindings.invalidDocument = "missing.gltf";
  bindings.mergeDocument = "merge.gltf";
  bindings.saveTarget = "saved.glb";
  bindings.primaryScene = typedId<EditorObjectKind::scene>(0u);
  bindings.secondaryScene = typedId<EditorObjectKind::scene>(1u);
  bindings.primaryNode = typedId<EditorObjectKind::node>(0u);
  bindings.secondaryNode = typedId<EditorObjectKind::node>(1u);
  bindings.nodeTransform = NodeTransform{};
  bindings.reparentSource = SceneNodeOccurrence{
      bindings.primaryScene, bindings.primaryNode, std::nullopt, 1u};
  bindings.reparentTarget = bindings.secondaryNode;
  bindings.nodeSelection = EditorSelectionSnapshot{
      kAuthority,
      NodeSelection{bindings.primaryScene, bindings.primaryNode, std::nullopt}};
  const MeshId mesh = typedId<EditorObjectKind::mesh>(0u);
  const PrimitiveId primitive = typedId<EditorObjectKind::primitive>(0u);
  bindings.primaryMaterial = typedId<EditorObjectKind::material>(0u);
  bindings.alternateMaterial = typedId<EditorObjectKind::material>(1u);
  bindings.primitiveSelection = EditorSelectionSnapshot{
      kAuthority, PrimitiveSelection{bindings.primaryScene,
                                     bindings.primaryNode, primitive, mesh, 0u}};
  bindings.primitive = ui::PrimitiveContext{
      bindings.primaryScene, bindings.primaryNode, mesh, primitive,
      bindings.primaryMaterial, true, true, true};
  bindings.camera = typedId<EditorObjectKind::camera>(0u);
  bindings.cameraNode = bindings.primaryNode;
  bindings.cameraSelection =
      EditorSelectionSnapshot{kAuthority, *bindings.camera};
  bindings.light = typedId<EditorObjectKind::light>(0u);
  bindings.lightSelection =
      EditorSelectionSnapshot{kAuthority, *bindings.light};
  bindings.animation = typedId<EditorObjectKind::animation>(0u);
  bindings.unicodeName = "long-unicode-name";
  return bindings;
}

testing::TestAction capture(std::uint64_t step, std::string id,
                            std::string label) {
  return testing::TestAction{
      testing::TestAction::kSchemaVersion,
      step,
      std::move(id),
      ui::CommandSource::menu,
      testing::CaptureStateAction{std::move(label)}};
}

testing::TestAction invoke(std::uint64_t step, std::string id,
                           ui::CommandId command) {
  return testing::TestAction{
      testing::TestAction::kSchemaVersion,
      step,
      std::move(id),
      ui::CommandSource::shortcut,
      ui::InvokeCommand{command, std::nullopt}};
}

bool testTypedDispatchAndFailClosed() {
  bool passed = true;
  std::vector<ui::UiCommand> received;
  ui::FrameModel frame;
  testing::EditorTestDriver::Callbacks callbacks;
  callbacks.frame = [&frame] { return frame; };
  callbacks.dispatch = [&received](const ui::UiCommand &command) {
    received.push_back(command);
    return ui::RuntimeActionResult{true, ui::ProjectionRefresh::none, {}};
  };
  testing::EditorTestDriver driver(std::move(callbacks));
  driver.setFrameModel(frame);

  auto first = driver.dispatch(capture(0u, "capture", "start"));
  passed &= expect(first.accepted && first.executed,
                   "capture action is accepted and recorded");
  passed &= expect(driver.capturedStates().size() == 1u,
                   "capture action emits one immutable state sample");

  // New Scene is enabled in NoDocument and is delivered as a typed payload.
  auto newScene = driver.dispatch(invoke(1u, "new", ui::CommandId::newScene));
  passed &= expect(newScene.accepted && received.size() == 1u,
                   "enabled command reaches the typed dispatch callback");
  if (!received.empty()) {
    passed &= expect(received.back().sequence == 1u,
                     "driver assigns the monotonic sequence to UiCommand");
    passed &= expect(std::holds_alternative<ui::InvokeCommand>(
                         received.back().payload),
                     "callback receives InvokeCommand rather than text");
  }

  // Undo is disabled in the same NoDocument snapshot and must not reach the
  // callback, even though a host could otherwise accept the payload.
  auto undo = driver.dispatch(invoke(2u, "undo", ui::CommandId::undo));
  passed &= expect(undo.status == testing::ActionStatus::disabled &&
                       !undo.executed && received.size() == 1u,
                   "disabled command is rejected before host dispatch");

  // A gap is a protocol error and leaves the sequence unchanged.
  auto gap = driver.dispatch(capture(4u, "gap", "bad"));
  passed &= expect(gap.status == testing::ActionStatus::invalid &&
                       driver.capturedStates().size() == 1u,
                   "sequence gaps fail closed without side effects");
  auto next = driver.dispatch(capture(3u, "next", "after-gap"));
  passed &= expect(next.status == testing::ActionStatus::accepted &&
                       driver.capturedStates().size() == 2u,
                   "failed gap does not advance the sequence");

  testing::TestAction malformed = capture(2u, "malformed", "x");
  malformed.schemaVersion = 99u;
  auto badSchema = driver.dispatch(malformed);
  passed &= expect(badSchema.status == testing::ActionStatus::invalid,
                   "unknown action schema is rejected");
  return passed;
}

bool testScreenshotAndScripts() {
  bool passed = true;
  testing::EditorTestDriver driver;
  driver.setFrameModel(readyFrame());
  const testing::ScenarioBindings bindings = testBindings();
  const auto registration = driver.registerDefaultScripts(bindings);
  passed &= expect(registration.accepted, "all default U01-U08 scripts register");
  passed &= expect(driver.scripts().size() == 8u,
                   "registry contains exactly eight scenario scripts");
  for (const auto &script : driver.scripts()) {
    passed &= expect(script.steps.size() > 3u,
                     "default script contains semantic workflow steps");
    passed &= expect(driver.hasScript(script.id),
                     "registered script is addressable by stable id");
    bool hasDomainAction = false;
    for (const auto &step : script.steps) {
      const auto kind = testing::EditorTestDriver::kindOf(step.action.payload);
      hasDomainAction |= kind != testing::ActionKind::captureState &&
                         kind != testing::ActionKind::screenshotMarker;
      passed &= expect(step.expectedState.has_value() &&
                           step.expectedAccepted.has_value(),
                       "every default step has explicit expectations");
    }
    passed &= expect(hasDomainAction,
                     "every default script contains typed domain actions");
    passed &= expect(!script.coverageComplete() &&
                         !script.coverageGaps.empty(),
                     "visual gaps remain explicit non-claims");
  }
  passed &= expect(driver.scriptManifestJson().find(
                       "\"coverageComplete\":false") != std::string::npos,
                   "script manifest preserves visual non-claims");
  const std::string surface =
      testing::EditorTestDriver::surfaceManifestJson(driver.snapshot());
  passed &= expect(surface.find(
                       "\"menuOrder\":[\"File\",\"Edit\",\"View\","
                       "\"Windows\",\"Tools\"]") != std::string::npos,
                   "surface manifest preserves menu order");
  passed &= expect(countOccurrences(surface, "\"source\":\"toolbar\"") ==
                       5u,
                   "surface manifest has exactly five toolbar entries");
  for (const std::string_view context :
       {"\"Scene\":[", "\"Node\":[", "\"Mesh\":[",
        "\"Primitive\":[", "\"Material\":["})
    passed &= expect(surface.find(context) != std::string::npos,
                     "surface manifest includes every context owner");

  const auto unknown = driver.replay("U09-unknown");
  passed &= expect(unknown.status == testing::ReplayStatus::invalid &&
                       !unknown.passed,
                   "unknown script fails closed");

  testing::TestScript malformed;
  malformed.id = "U01-empty-new-open";
  malformed.description = "duplicate step rejection";
  malformed.steps.push_back(testing::ScriptStep{
      testing::ScriptStep::kSchemaVersion,
      "duplicate",
      capture(0u, "duplicate", "one"),
      testing::UiState::readyClean,
      true});
  malformed.steps.push_back(testing::ScriptStep{
      testing::ScriptStep::kSchemaVersion,
      "duplicate",
      capture(1u, "duplicate-2", "two"),
      testing::UiState::readyClean,
      true});
  const auto badRegistration = driver.registerScript(std::move(malformed));
  passed &= expect(!badRegistration.accepted &&
                       badRegistration.error.find("unique") !=
                           std::string::npos,
                   "duplicate step ids are rejected at registration");

  testing::EditorTestDriver disabled(false);
  disabled.setFrameModel(readyFrame());
  const auto blocked = disabled.replay("U01-empty-new-open");
  passed &= expect(blocked.status == testing::ReplayStatus::invalid,
                   "unregistered script remains invalid on a disabled driver");
  passed &= expect(disabled.registerDefaultScripts(bindings).accepted,
                   "disabled driver can register immutable script definitions");
  const auto blockedReplay = disabled.replay("U01-empty-new-open");
  passed &= expect(blockedReplay.status == testing::ReplayStatus::blocked &&
                       !blockedReplay.passed && blockedReplay.steps.empty(),
                   "disabled replay has no partial action side effects");
  return passed;
}

bool testSchemaAndExpectedResults() {
  bool passed = true;
  testing::EditorTestDriver driver;
  driver.setFrameModel(readyFrame());
  testing::TestScript script;
  script.id = "U08-large-unicode-dpi";
  script.description = "state mismatch rejection";
  script.steps.push_back(testing::ScriptStep{
      testing::ScriptStep::kSchemaVersion,
      "capture",
      capture(0u, "capture", "state"),
      testing::UiState::readyModified,
      true});
  const auto result = driver.registerScript(std::move(script));
  passed &= expect(result.accepted, "custom-shaped known scenario registers");
  const auto replay = driver.replay("U08-large-unicode-dpi");
  passed &= expect(replay.status == testing::ReplayStatus::failed &&
                       !replay.passed && !replay.steps.empty(),
                   "state expectation mismatch stops replay");
  passed &= expect(testing::EditorTestDriver::actionSchemaJson().find(
                       "\"invokeCommand\"") != std::string::npos,
                   "action schema enumerates typed invokeCommand");
  passed &= expect(testing::EditorTestDriver::scriptSchemaJson().find(
                       "\"U08-large-unicode-dpi\"") != std::string::npos,
                   "script schema enumerates U08");
  passed &= expect(replay.toJson().find("\"passed\":false") !=
                       std::string::npos,
                   "failed replay result serializes a false verdict");
  return passed;
}

} // namespace

int main() {
  bool passed = true;
  passed &= testTypedDispatchAndFailClosed();
  passed &= testScreenshotAndScripts();
  passed &= testSchemaAndExpectedResults();
  if (!passed) {
    std::cerr << "Editor UI action tests failed after " << g_checks
              << " checks\n";
    return 1;
  }
  std::cout << "Editor UI action tests passed " << g_checks << " checks\n";
  return 0;
}
