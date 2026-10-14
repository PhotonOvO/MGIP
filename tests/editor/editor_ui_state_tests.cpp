#include "../../app/editor/EditorTestDriver.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace testing = demo::editor::testing;
namespace ui = demo::editor::ui;

std::size_t g_checks = 0;

bool expect(bool condition, std::string_view message) {
  ++g_checks;
  if (!condition)
    std::cerr << "FAILED: " << message << '\n';
  return condition;
}

ui::FrameModel readyFrame(bool modified = false) {
  ui::FrameModel frame;
  frame.hasDocument = true;
  frame.mutationEnabled = true;
  frame.revision = modified ? 2u : 1u;
  frame.cleanRevision = 1u;
  // The locked lifecycle order is stable and intentionally exercised without
  // depending on transitional enum member names.
  frame.lifecycle = static_cast<ui::LifecycleState>(modified ? 3u : 2u);
  return frame;
}

bool testStateVocabulary() {
  bool passed = true;
  const std::vector<std::string_view> expected = {
      "NoDocument", "LoadingCandidate", "ReadyClean", "ReadyModified",
      "GizmoHover", "GizmoDragging", "Modal",
      "Saving/Merging/Compacting", "Error"};
  for (std::size_t index = 0; index < expected.size(); ++index) {
    const auto state = static_cast<testing::UiState>(index);
    passed &= expect(testing::uiStateName(state) == expected[index],
                     "state vocabulary uses the exact locked spelling");
    passed &= expect(testing::parseUiState(expected[index]) == state,
                     "state parser round-trips every locked value");
  }
  passed &= expect(!testing::parseUiState("Loading").has_value(),
                   "collapsed state aliases are rejected");
  passed &= expect(!testing::parseUiState("Busy").has_value(),
                   "generic busy is not a protocol state");
  return passed;
}

bool testManifestAndEnablement() {
  bool passed = true;
  testing::EditorTestDriver driver;
  driver.setFrameModel(ui::FrameModel{});
  const auto noDocument = driver.snapshot("no-document");
  passed &= expect(noDocument.state == testing::UiState::noDocument,
                   "empty frame maps to NoDocument");
  passed &= expect(noDocument.commands.size() == 20u,
                   "manifest contains all 20 typed commands");
  if (noDocument.commands.size() == 20u) {
    passed &= expect(noDocument.commands.front().label == "New Scene",
                     "File menu starts with New Scene");
    passed &= expect(noDocument.commands[1].label == "Open",
                     "Open follows New Scene");
    passed &= expect(noDocument.commands.back().label == "Clear selection",
                     "Clear selection is the final command");
    passed &= expect(noDocument.commands[0].enabled &&
                         noDocument.commands[1].enabled,
                     "New/Open remain enabled without a document");
    passed &= expect(!noDocument.commands[4].enabled &&
                         !noDocument.commands[6].enabled,
                     "Save As and Undo are disabled without a document");
  }

  driver.setFrameModel(readyFrame(false));
  auto clean = driver.snapshot("clean");
  passed &= expect(clean.state == testing::UiState::readyClean,
                   "clean frame maps to ReadyClean");
  const auto find = [&clean](ui::CommandId id) -> const testing::CommandManifestEntry * {
    for (const auto &command : clean.commands)
      if (command.id == id)
        return &command;
    return nullptr;
  };
  const auto *save = find(ui::CommandId::saveAs);
  const auto *undo = find(ui::CommandId::undo);
  passed &= expect(save != nullptr && save->enabled,
                   "Save As is enabled for a clean document");
  passed &= expect(undo != nullptr && !undo->enabled,
                   "Undo follows history enablement");

  ui::FrameModel busy = readyFrame(true);
  busy.rendererBusy = true;
  busy.lifecycle = static_cast<ui::LifecycleState>(7u);
  driver.setFrameModel(busy);
  const auto blocked = driver.snapshot("busy");
  passed &= expect(blocked.state == testing::UiState::savingMergingCompacting,
                   "busy lifecycle presents one Saving/Merging/Compacting state");
  for (const auto &command : blocked.commands) {
    if (command.id == ui::CommandId::newScene ||
        command.id == ui::CommandId::open)
      passed &= expect(!command.enabled,
                       "destructive File commands are disabled while busy");
    else if (command.id == ui::CommandId::toggleSceneBrowser ||
             command.id == ui::CommandId::toggleInspector)
      passed &= expect(command.enabled,
                       "window visibility commands remain available while busy");
    else if (command.id != ui::CommandId::exit)
      passed &= expect(!command.enabled,
                       "document mutation commands fail closed while busy");
  }
  return passed;
}

bool testSerialization() {
  bool passed = true;
  testing::EditorTestDriver driver;
  ui::FrameModel frame = readyFrame(true);
  frame.operation.label = "save\nphase";
  frame.operation.error = "bad\\path";
  frame.operation.progress =
      static_cast<float>(std::numeric_limits<double>::quiet_NaN());
  frame.browser.asset.generator = "CJK \xE4\xB8\xAD";
  driver.setFrameModel(std::move(frame));
  const auto snapshot = driver.captureState("line\nbreak");
  const std::string json = snapshot.toJson();
  passed &= expect(json.find("\"schemaVersion\":1") != std::string::npos,
                   "state JSON carries a schema version");
  passed &= expect(json.find("\"state\":\"ReadyModified\"") !=
                       std::string::npos,
                   "lifecycle authority remains ReadyModified with an error field");
  passed &= expect(json.find("\"progress\":null") != std::string::npos,
                   "non-finite progress is serialized as null");
  passed &= expect(json.find("save\\nphase") != std::string::npos,
                   "control characters are escaped in state JSON");
  passed &= expect(json.find("line\\nbreak") != std::string::npos,
                   "capture labels are escaped in state JSON");
  const std::string stateSchema = testing::EditorTestDriver::stateSchemaJson();
  passed &= expect(stateSchema.find("Saving/Merging/Compacting") !=
                       std::string::npos,
                   "state schema retains the collapsed busy spelling");
  passed &= expect(stateSchema.find("\"Error\"") != std::string::npos,
                   "state schema enumerates Error");
  return passed;
}

} // namespace

int main() {
  bool passed = true;
  passed &= testStateVocabulary();
  passed &= testManifestAndEnablement();
  passed &= testSerialization();
  if (!passed) {
    std::cerr << "Editor UI state tests failed after " << g_checks
              << " checks\n";
    return 1;
  }
  std::cout << "Editor UI state tests passed " << g_checks << " checks\n";
  return 0;
}
