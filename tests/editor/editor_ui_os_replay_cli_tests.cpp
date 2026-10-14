#include "../../app/editor/EditorUiOsReplayCli.h"
#include <mgif/scene_editor/SceneChangeCanonicalizer.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace os = demo::editor::testing::osreplay;

std::size_t checks = 0u;

bool expect(bool condition, std::string_view message) {
  ++checks;
  if (!condition)
    std::cerr << "FAILED: " << message << '\n';
  return condition;
}

struct TemporaryDirectory final {
  std::filesystem::path path;

  TemporaryDirectory() {
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() /
           ("mgif-os-replay-cli-" + std::to_string(stamp));
    std::filesystem::create_directories(path);
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

std::vector<std::string> runArguments(const std::filesystem::path &evidence,
                                      const std::filesystem::path &sync,
                                      std::string_view subcase = "main") {
  const bool unique = subcase == "unique-root";
  return {
      "--editor-os-case=U02-hierarchy",
      "--editor-os-subcase=" + std::string(subcase),
      std::string("--editor-os-start-step=") + (unique ? "21" : "0"),
      std::string("--editor-os-stop-step=") + (unique ? "28" : "20"),
      "--editor-os-evidence-dir=" + evidence.generic_string(),
      "--editor-os-sync-dir=" + sync.generic_string(),
      "--editor-os-run-nonce=0123456789abcdef0123456789abcdef",
      "--editor-os-plan-sha256=" + std::string(os::kPlanContractSha256),
      "--editor-os-executable-sha256=" + std::string(64u, 'a'),
      "--editor-os-source-sha256=" + std::string(64u, 'b'),
      "--editor-os-width=1280",
      "--editor-os-height=720",
      "--editor-os-timeout-ms=180000",
  };
}

os::CliParseResult parse(const std::vector<std::string> &storage) {
  std::vector<std::string_view> arguments;
  arguments.reserve(storage.size());
  for (const std::string &value : storage)
    arguments.push_back(value);
  return os::parseEditorUiOsReplayCli(arguments);
}

bool testModesAndCapability() {
  bool passed = true;
  const auto disabled = parse({"--backend=vulkan", "--width=1280"});
  passed &= expect(disabled.accepted() &&
                       disabled.mode == os::CliMode::disabled &&
                       disabled.unconsumed.size() == 2u,
                   "unrelated application arguments remain unconsumed");

  const auto capability = parse({"--editor-os-capability"});
  passed &= expect(capability.accepted() &&
                       capability.mode == os::CliMode::capability &&
                       capability.unconsumed.empty(),
                   "capability-only mode parses");
  const std::string json = os::editorUiOsReplayCapabilityJson();
  passed &= expect(json.find("\"integration\":false") != std::string::npos &&
                       json.find("\"sendInputOnly\":true") !=
                           std::string::npos &&
                       json.find(std::string(os::kPlanContractSha256)) !=
                           std::string::npos &&
                       json.find(std::string(os::kCapabilitySourceRole)) !=
                           std::string::npos &&
                       json.find("executableSha256") == std::string::npos,
                   "capability binds plan/source role but never claims exe hash");

  passed &= expect(!parse({"--editor-os-capability", "--backend=vulkan"})
                        .accepted(),
                   "capability cannot be mixed with app arguments");
  passed &= expect(!parse({"--editor-os-capability",
                           "--editor-os-case=U02-hierarchy"})
                        .accepted(),
                   "capability cannot be mixed with run arguments");
  passed &= expect(parse({"--editor-os-capability",
                          "--editor-os-capability-case=U03-inspector-transform"})
                       .mode == os::CliMode::capability,
                   "scoped Inspector capability can be queried");
  passed &= expect(!parse({"--editor-os-capability-case=U03-inspector-transform"})
                        .accepted(), "case-only query fails closed");
  passed &= expect(parse({"--editor-os-capability",
                          "--editor-os-capability-case=U03-gizmo-escape"})
                       .mode == os::CliMode::capability,
                   "scoped Escape capability can be queried");
  for (const auto caseId : {os::kU03EscapeReleaseCase, os::kU03ReleaseEscapeCase}) {
    passed &= expect(parse({"--editor-os-capability",
                            "--editor-os-capability-case=" + std::string(caseId)})
                         .mode == os::CliMode::capability,
                     "ordered same-frame capability can be queried");
  }
  for (const auto caseId : {os::kU03FocusNoReleaseCase,
                            os::kU03FocusSameFrameCase,
                            os::kU03FocusDeliveredReleaseCase}) {
    passed &= expect(parse({"--editor-os-capability",
                            "--editor-os-capability-case=" + std::string(caseId)})
                         .mode == os::CliMode::capability,
                     "scoped focus-loss capability can be queried");
  }
#if defined(MGIF_EDITOR_OS_REPLAY)
  const auto token = os::editorUiOsReplayIntegrationToken();
  const auto partial = os::editorUiOsReplayCapabilityJson(token, os::kU03InspectorCase);
  const auto full = os::editorUiOsReplayCapabilityJson(token, os::kU03Case);
  const auto gizmo = os::editorUiOsReplayCapabilityJson(token, os::kU03GizmoCase);
  const auto escape = os::editorUiOsReplayCapabilityJson(token, os::kU03EscapeCase);
  passed &= expect(partial.find("\"integration\":true") != std::string::npos &&
                       partial.find("\"coverageComplete\":false") != std::string::npos &&
                       full.find("\"integration\":false") != std::string::npos,
                   "partial Inspector support never enables unfinished full U03");
  passed &= expect(gizmo.find("\"integration\":true") != std::string::npos &&
                       gizmo.find("\"coverageComplete\":false") != std::string::npos,
                   "native gizmo capability remains explicitly partial");
  passed &= expect(escape.find("\"case\":\"U03-gizmo-escape\"") != std::string::npos &&
                       escape.find("\"integration\":true") != std::string::npos &&
                       escape.find("\"coverageComplete\":false") != std::string::npos,
                   "native Escape capability remains explicitly partial");
  for (const auto caseId : {os::kU03EscapeReleaseCase, os::kU03ReleaseEscapeCase}) {
    const auto sameFrame = os::editorUiOsReplayCapabilityJson(token, caseId);
    passed &= expect(sameFrame.find("\"case\":\"" + std::string(caseId) + "\"") != std::string::npos &&
                         sameFrame.find("\"integration\":true") != std::string::npos &&
                         sameFrame.find("\"coverageComplete\":false") != std::string::npos,
                     "same-frame support never enables complete U03 coverage");
  }
  for (const auto caseId : {os::kU03FocusNoReleaseCase,
                            os::kU03FocusSameFrameCase,
                            os::kU03FocusDeliveredReleaseCase}) {
    const auto focus = os::editorUiOsReplayCapabilityJson(token, caseId);
    passed &= expect(focus.find("\"case\":\"" + std::string(caseId) + "\"") != std::string::npos &&
                         focus.find("\"integration\":true") != std::string::npos &&
                         focus.find("\"coverageComplete\":false") != std::string::npos,
                     "focus-loss support remains explicitly partial");
  }
#endif
  return passed;
}

bool testValidRuns() {
  bool passed = true;
  TemporaryDirectory temporary;
  const auto authority = temporary.path / "evidence-root";
  const auto app = authority / "app";
  const auto mainSync = authority / "sync";
  std::filesystem::create_directories(app);
  std::filesystem::create_directory(mainSync);
  const auto mainEvidence = app / "main";
  const auto main = parse(runArguments(mainEvidence, mainSync));
  passed &= expect(main.accepted() && main.runRequested() &&
                       main.options.subcase == "main" &&
                       main.options.startStep == 0u &&
                       main.options.stopStep == 20u &&
                       main.options.width == 1280u &&
                       main.options.height == 720u &&
                       main.options.timeoutMilliseconds == 180000u,
                   "main process run contract parses");
  passed &= expect(!std::filesystem::exists(mainEvidence) &&
                       std::filesystem::is_directory(mainSync),
                   "CLI validation leaves fresh evidence absent and sync intact");

  const auto uniqueEvidence = app / "unique-root";
  const auto uniqueSync = mainSync;
  const auto unique =
      parse(runArguments(uniqueEvidence, uniqueSync, "unique-root"));
  passed &= expect(unique.accepted() && unique.runRequested() &&
                       unique.options.startStep == 21u &&
                       unique.options.stopStep == 28u,
                   "unique-root fresh process range parses");
  const std::string trace = os::makeU03InspectorOsReplayPlan().contractTrace();
  const auto hash = demo::editor::SceneChangeCanonicalizer::computeSha256(
      std::span(reinterpret_cast<const std::uint8_t *>(trace.data()), trace.size()));
  std::string hashText;
  for (const auto byte : hash) {
    hashText += "0123456789abcdef"[byte >> 4u];
    hashText += "0123456789abcdef"[byte & 15u];
  }
  auto scoped = runArguments(mainEvidence, mainSync);
  scoped[0] = "--editor-os-case=U03-inspector-transform";
  scoped[3] = "--editor-os-stop-step=7";
  scoped[7] = "--editor-os-plan-sha256=" + hashText;
  passed &= expect(parse(scoped).runRequested(), "complete scoped Inspector run parses");
  scoped[3] = "--editor-os-stop-step=6";
  passed &= expect(!parse(scoped).accepted(), "truncated Inspector proof rejected");
  scoped[0] = "--editor-os-case=U03-transform-gizmo";
  passed &= expect(!parse(scoped).accepted(), "unsupported full U03 cannot run");
  const auto gizmoTrace = os::makeU03GizmoOsReplayPlan().contractTrace();
  const auto gizmoHash = demo::editor::SceneChangeCanonicalizer::computeSha256(
      std::span(reinterpret_cast<const std::uint8_t *>(gizmoTrace.data()), gizmoTrace.size()));
  std::string gizmoHashText;
  for (const auto byte : gizmoHash) {
    gizmoHashText += "0123456789abcdef"[byte >> 4u];
    gizmoHashText += "0123456789abcdef"[byte & 15u];
  }
  scoped[0] = "--editor-os-case=U03-gizmo-translate";
  scoped[3] = "--editor-os-stop-step=5";
  scoped[7] = "--editor-os-plan-sha256=" + gizmoHashText;
  passed &= expect(parse(scoped).runRequested(), "complete scoped gizmo run parses");
  scoped[3] = "--editor-os-stop-step=4";
  passed &= expect(!parse(scoped).accepted(), "truncated gizmo proof rejected");
  const auto escapeTrace = os::makeU03EscapeOsReplayPlan().contractTrace();
  const auto escapeHash = demo::editor::SceneChangeCanonicalizer::computeSha256(
      std::span(reinterpret_cast<const std::uint8_t *>(escapeTrace.data()), escapeTrace.size()));
  std::string escapeHashText;
  for (const auto byte : escapeHash) {
    escapeHashText += "0123456789abcdef"[byte >> 4u];
    escapeHashText += "0123456789abcdef"[byte & 15u];
  }
  scoped[0] = "--editor-os-case=U03-gizmo-escape";
  scoped[7] = "--editor-os-plan-sha256=" + escapeHashText;
  const auto escapeRun = parse(scoped);
  passed &= expect(escapeRun.runRequested() &&
                       escapeRun.options.caseId == os::kU03EscapeCase &&
                       escapeRun.options.startStep == 0u &&
                       escapeRun.options.stopStep == 4u &&
                       escapeRun.options.planSha256 == escapeHashText,
                   "complete scoped Escape run parses with its actual plan hash");
  scoped[3] = "--editor-os-stop-step=3";
  passed &= expect(!parse(scoped).accepted(), "truncated Escape proof rejected");
  scoped[3] = "--editor-os-stop-step=4";
  scoped[2] = "--editor-os-start-step=1";
  passed &= expect(!parse(scoped).accepted(), "Escape proof without bootstrap rejected");
  scoped[2] = "--editor-os-start-step=0";
  scoped[7] = "--editor-os-plan-sha256=" + gizmoHashText;
  passed &= expect(!parse(scoped).accepted(), "Translate plan hash cannot authorize Escape proof");
  for (const bool escapeFirst : {true, false}) {
    const auto plan = os::makeU03SameFrameOsReplayPlan(escapeFirst);
    const auto trace = plan.contractTrace();
    const auto digest = demo::editor::SceneChangeCanonicalizer::computeSha256(
        std::span(reinterpret_cast<const std::uint8_t *>(trace.data()), trace.size()));
    std::string hashText;
    for (const auto byte : digest) {
      hashText += "0123456789abcdef"[byte >> 4u];
      hashText += "0123456789abcdef"[byte & 15u];
    }
    scoped[0] = "--editor-os-case=" + plan.caseId;
    scoped[7] = "--editor-os-plan-sha256=" + hashText;
    passed &= expect(parse(scoped).runRequested(), "complete same-frame proof parses");
    scoped[3] = "--editor-os-stop-step=3";
    passed &= expect(!parse(scoped).accepted(), "truncated same-frame proof rejected");
    scoped[3] = "--editor-os-stop-step=4";
    scoped[7] = "--editor-os-plan-sha256=" + escapeHashText;
    passed &= expect(!parse(scoped).accepted(), "Escape-only hash cannot authorize same-frame proof");
  }
  for (const auto caseId : {os::kU03FocusNoReleaseCase,
                            os::kU03FocusSameFrameCase,
                            os::kU03FocusDeliveredReleaseCase}) {
    const auto plan = os::makeU03FocusOsReplayPlan(caseId);
    const auto trace = plan.contractTrace();
    const auto digest = demo::editor::SceneChangeCanonicalizer::computeSha256(
        std::span(reinterpret_cast<const std::uint8_t *>(trace.data()), trace.size()));
    std::string hashText;
    for (const auto byte : digest) {
      hashText += "0123456789abcdef"[byte >> 4u];
      hashText += "0123456789abcdef"[byte & 15u];
    }
    scoped[0] = "--editor-os-case=" + std::string(caseId);
    scoped[2] = "--editor-os-start-step=0";
    scoped[3] = "--editor-os-stop-step=3";
    scoped[7] = "--editor-os-plan-sha256=" + hashText;
    passed &= expect(parse(scoped).runRequested(), "complete focus-loss proof parses");
    scoped[3] = "--editor-os-stop-step=2";
    passed &= expect(!parse(scoped).accepted(), "truncated focus-loss proof rejected");
  }
  return passed;
}

bool testInvalidRuns() {
  bool passed = true;
  TemporaryDirectory temporary;
  const auto authority = temporary.path / "evidence-root";
  const auto app = authority / "app";
  const auto sync = authority / "sync";
  std::filesystem::create_directories(app);
  std::filesystem::create_directory(sync);
  const auto evidence = app / "main";

  auto missing = runArguments(evidence, sync);
  missing.pop_back();
  passed &= expect(!parse(missing).accepted(),
                   "missing required run argument fails closed");

  auto duplicate = runArguments(evidence, sync);
  duplicate.push_back("--editor-os-width=640");
  passed &= expect(!parse(duplicate).accepted(),
                   "duplicate argument fails closed");

  auto unknown = runArguments(evidence, sync);
  unknown.push_back("--editor-os-unknown=value");
  passed &= expect(!parse(unknown).accepted(),
                   "unknown OS replay argument fails closed");

  auto badRange = runArguments(evidence, sync);
  badRange[3] = "--editor-os-stop-step=19";
  passed &= expect(!parse(badRange).accepted(),
                   "unregistered process range fails closed");

  auto badHash = runArguments(evidence, sync);
  badHash[7] = "--editor-os-plan-sha256=" + std::string(64u, '0');
  passed &= expect(!parse(badHash).accepted(),
                   "foreign plan SHA fails closed");

  auto badNonce = runArguments(evidence, sync);
  badNonce[6] = "--editor-os-run-nonce=ABCDEF";
  passed &= expect(!parse(badNonce).accepted(),
                   "malformed run nonce fails closed");

  auto badExtent = runArguments(evidence, sync);
  badExtent[10] = "--editor-os-width=639";
  passed &= expect(!parse(badExtent).accepted(),
                   "out-of-range extent fails closed");

  auto relative = runArguments("relative-evidence", sync);
  passed &= expect(!parse(relative).accepted(),
                   "relative evidence path fails closed");

  std::filesystem::create_directory(evidence);
  auto existing = runArguments(evidence, sync);
  passed &= expect(!parse(existing).accepted(),
                   "pre-existing evidence directory fails closed");
  std::filesystem::remove(evidence);

  auto nested = runArguments(evidence, app);
  passed &= expect(!parse(nested).accepted(),
                   "nested evidence/sync paths fail closed");

  const auto residue = sync /
      "0123456789abcdef0123456789abcdef.000.target.ready.json";
  std::ofstream residueFile(residue);
  residueFile << "stale";
  residueFile.close();
  passed &= expect(!parse(runArguments(evidence, sync)).accepted(),
                   "current nonce residue fails closed");
  return passed;
}

} // namespace

int main(int argc, char **argv) {
  std::vector<std::string> arguments(argv + 1, argv + argc);
  if (!arguments.empty() && parse(arguments).mode == os::CliMode::capability) {
#if defined(MGIF_EDITOR_OS_REPLAY)
    std::string_view caseId = os::kU02Case;
    for (const auto &argument : arguments) {
      constexpr std::string_view prefix = "--editor-os-capability-case=";
      if (argument.starts_with(prefix))
        caseId = std::string_view(argument).substr(prefix.size());
    }
    std::cout << os::editorUiOsReplayCapabilityJson(
                     os::editorUiOsReplayIntegrationToken(), caseId)
              << '\n';
#else
    std::cout << os::editorUiOsReplayCapabilityJson() << '\n';
#endif
    return 0;
  }
  const bool passed =
      testModesAndCapability() && testValidRuns() && testInvalidRuns();
  std::cout << "editor_ui_os_replay_cli_tests: " << checks << " checks\n";
  return passed ? 0 : 1;
}
