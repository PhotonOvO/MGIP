#include "../../app/editor/EditorController.h"
#include "../../app/editor/EditorTestDriver.h"
#include "../../app/editor/MaterialClipboardCodec.h"

#include <mgif/scene_editor/GltfDocument.h>
#include <mgif/scene_editor/SceneProjection.h>
#include <mgif/scene_editor/commands/EditorHistory.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace demo::editor;
namespace testing = demo::editor::testing;
namespace ui = demo::editor::ui;

std::size_t g_checks = 0;

bool expect(bool condition, std::string_view message) {
  ++g_checks;
  if (!condition)
    std::cerr << "FAILED: " << message << '\n';
  return condition;
}

std::filesystem::path fixture(std::string_view name) {
  return std::filesystem::path(MGIF_TEST_SOURCE_DIR) / "tests" / "fixtures" /
         "gltf_editor" / name;
}

std::string quote(std::string_view value) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string output{"\""};
  for (const unsigned char character : value) {
    switch (character) {
    case '"':
      output += "\\\"";
      break;
    case '\\':
      output += "\\\\";
      break;
    case '\n':
      output += "\\n";
      break;
    case '\r':
      output += "\\r";
      break;
    case '\t':
      output += "\\t";
      break;
    default:
      if (character < 0x20U) {
        output += "\\u00";
        output.push_back(hex[(character >> 4U) & 0x0fU]);
        output.push_back(hex[character & 0x0fU]);
      } else {
        output.push_back(static_cast<char>(character));
      }
      break;
    }
  }
  output.push_back('"');
  return output;
}

std::string digestHex(const SceneDigest &digest) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string output;
  output.reserve(digest.size() * 2u);
  for (const std::uint8_t value : digest) {
    output.push_back(hex[value >> 4u]);
    output.push_back(hex[value & 0x0fu]);
  }
  return output;
}

std::string selectionName(EditorSelectionKind kind) {
  switch (kind) {
  case EditorSelectionKind::none:
    return "None";
  case EditorSelectionKind::node:
    return "Node";
  case EditorSelectionKind::primitive:
    return "Primitive";
  case EditorSelectionKind::mesh:
    return "Mesh";
  case EditorSelectionKind::material:
    return "Material";
  case EditorSelectionKind::camera:
    return "Camera";
  case EditorSelectionKind::light:
    return "Light";
  }
  return "None";
}

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    for (std::uint32_t attempt = 0; attempt < 100u; ++attempt) {
      std::error_code error;
      const auto candidate =
          std::filesystem::temp_directory_path() /
          ("mgif-editor-ui-replay-" + std::to_string(stamp) + "-" +
           std::to_string(attempt));
      if (std::filesystem::create_directory(candidate, error)) {
        m_path = candidate;
        break;
      }
    }
  }

  ~TemporaryDirectory() {
    if (m_path.empty())
      return;
    std::error_code ignored;
    std::filesystem::remove_all(m_path, ignored);
  }

  const std::filesystem::path &path() const noexcept { return m_path; }

private:
  std::filesystem::path m_path;
};

struct Clipboard final : IOsTextClipboard {
  std::optional<std::string> readText() const override { return text; }
  bool writeText(std::string_view value) override {
    text = std::string(value);
    return true;
  }
  std::optional<std::string> text;
};

struct LifecycleEvidence final {
  ui::LifecycleState state{ui::LifecycleState::noDocument};
  ui::LifecycleOperation operation{ui::LifecycleOperation::none};
  std::string label;
  std::string error;
  std::optional<float> progress;
};

struct Runtime final {
  Runtime() {
    ui::EditorController::Callbacks callbacks;
    callbacks.clipboard = &clipboard;
    callbacks.onLifecycle = [this](ui::LifecycleState state,
                                   const ui::OperationState &operation) {
      lifecycle.push_back(
          LifecycleEvidence{state, operation.kind, operation.label,
                            operation.error, operation.progress});
    };
    callbacks.onPublication =
        [this](const ui::EditorController::ProjectionPublication &publication) {
          ++publicationCount;
          if (publication.canonical != nullptr)
            ++canonicalPublicationCount;
        };
    callbacks.onError = [this](std::string_view error) {
      errors.emplace_back(error);
    };
    callbacks.cameraRequest = [this](const ui::CameraRequest &request) {
      cameraRequests.push_back(request);
      return ui::RuntimeActionResult{true, ui::ProjectionRefresh::none, {}};
    };
    callbacks.requestExit = [this] { exitRequested = true; };
    callbacks.viewportControlsAvailable = [] { return true; };
    controller =
        std::make_unique<ui::EditorController>(std::move(callbacks));
  }

  testing::AuthoritySummary authority() const {
    testing::AuthoritySummary summary;
    summary.lifecycleRevision = controller->lifecycleRevision();
    const ui::FrameModel frame = controller->buildFrameModel();
    summary.selectionRevision = frame.selectionRevision;
    summary.selectionKind = selectionName(frame.selection.kind());
    summary.hasDocument = controller->hasDocument();
    summary.hasProjection = controller->projection() != nullptr;
    if (const EditorSessionBundle *session = controller->session()) {
      const GltfDocument &document = session->document();
      summary.documentRevision = document.revision();
      summary.cleanRevision = document.cleanRevision();
      summary.sceneActivationEpoch = document.sceneActivationEpoch();
      summary.sourcePath = document.sourcePath().generic_string();
      summary.documentDigest = digestHex(document.documentDigest());
      summary.undoCount = session->editor().history().undoCount();
      summary.redoCount = session->editor().history().redoCount();
    }
    if (const SceneProjectionResult *projection = controller->projection()) {
      summary.mappingEpoch = projection->mappingEpoch;
      summary.semanticHash = projection->semanticHash;
      summary.projectionDigest = digestHex(projection->projectionDigest);
    }
    return summary;
  }

  testing::EditorTestDriver::Callbacks driverCallbacks() {
    testing::EditorTestDriver::Callbacks callbacks;
    callbacks.dispatch = [this](const ui::UiCommand &command) {
      return controller->dispatch(command);
    };
    callbacks.control = [this](const testing::ControllerAction &action) {
      switch (action.kind) {
      case testing::ControllerActionKind::setGizmoHover:
        controller->setGizmoHover(action.hovered);
        return ui::RuntimeActionResult{true, ui::ProjectionRefresh::none, {}};
      case testing::ControllerActionKind::beginGizmoTransform:
        return controller->beginGizmoTransform(*action.node);
      case testing::ControllerActionKind::previewGizmoTransform:
        return controller->previewGizmoTransform(*action.node,
                                                 *action.transform);
      case testing::ControllerActionKind::finishGizmoTransform:
        return controller->finishGizmoTransform(
            *action.node, action.physicalRelease, action.escapePressed,
            action.focusLost);
      case testing::ControllerActionKind::dismissError:
        controller->dismissError();
        return ui::RuntimeActionResult{true, ui::ProjectionRefresh::none, {}};
      case testing::ControllerActionKind::activateSceneByIndex:
        if (!action.sceneIndex || controller->session() == nullptr) {
          return ui::RuntimeActionResult{
              false, ui::ProjectionRefresh::none,
              "scene activation index is unavailable"};
        }
        if (*action.sceneIndex >= controller->session()->document().objectCount(
                                    EditorObjectKind::scene)) {
          return ui::RuntimeActionResult{
              false, ui::ProjectionRefresh::none,
              "scene activation index is out of range"};
        }
        if (const auto scene = SceneId::from(controller->session()->document().idAt(
                EditorObjectKind::scene, *action.sceneIndex))) {
          return controller->dispatch(ui::UiCommand{
              0u, ui::CommandSource::sceneBrowser,
              ui::ActivateScene{*scene}});
        }
        return ui::RuntimeActionResult{
            false, ui::ProjectionRefresh::none,
            "scene activation id is invalid"};
      }
      return ui::RuntimeActionResult{
          false, ui::ProjectionRefresh::none,
          "invalid controller action kind"};
    };
    callbacks.frame = [this] { return controller->buildFrameModel(); };
    callbacks.persistent = [this] { return controller->persistentState(); };
    callbacks.authority = [this] { return authority(); };
    callbacks.stateCaptured = [this](const testing::StateSnapshot &) {
      ++stateCaptureCount;
    };
    callbacks.screenshot = [this](const testing::ScreenshotMarker &marker) {
      boundaryMarkers.push_back(marker);
    };
    return callbacks;
  }

  Clipboard clipboard;
  std::unique_ptr<ui::EditorController> controller;
  std::vector<LifecycleEvidence> lifecycle;
  std::vector<std::string> errors;
  std::vector<ui::CameraRequest> cameraRequests;
  std::vector<testing::ScreenshotMarker> boundaryMarkers;
  std::size_t publicationCount{0};
  std::size_t canonicalPublicationCount{0};
  std::size_t stateCaptureCount{0};
  bool exitRequested{false};
};

template <EditorObjectKind Kind>
std::optional<TypedEditorObjectId<Kind>>
typedAt(const GltfDocument &document, std::size_t index) {
  if (index >= document.objectCount(Kind))
    return std::nullopt;
  return TypedEditorObjectId<Kind>::from(document.idAt(Kind, index));
}

EditorSelectionSnapshot nodeSelection(const GltfDocument &document,
                                      SceneId scene, NodeId node) {
  const std::vector<NodeId> parents =
      document.sceneNodeParentPath(scene, node);
  return EditorSelectionSnapshot{
      document.documentAuthority(),
      NodeSelection{scene, node,
                    parents.empty()
                        ? std::nullopt
                        : std::optional<NodeId>{parents.back()}}};
}

bool populateBindings(std::string_view scenario, Runtime &runtime,
                      const TemporaryDirectory &temporary,
                      testing::ScenarioBindings &bindings,
                      std::string *error) {
  bindings.primaryDocument =
      fixture(scenario == "U01-empty-new-open" ? "multi_scene_oracle.gltf"
                                               : "minimal_scene.gltf");
  bindings.invalidDocument = temporary.path() / "missing-document.gltf";
  bindings.mergeDocument = fixture("minimal_scene.gltf");
  bindings.saveTarget = temporary.path() / "saved.glb";
  bindings.unicodeName =
      "Driver Unicode \xE4\xB8\xAD\xE6\x96\x87 "
      "\xF0\x9F\x9A\x80 combining-e\xCC\x81";
  if (scenario == "U01-empty-new-open")
    return true;
  if (!runtime.controller->hasDocument() || runtime.controller->session() == nullptr) {
    if (error)
      *error = "scenario has no live controller document";
    return false;
  }

  GltfDocument &document = runtime.controller->session()->document();
  SceneCommands &commands =
      runtime.controller->session()->editor().commands();
  const auto scene = typedAt<EditorObjectKind::scene>(document, 0u);
  const std::size_t nodeIndex =
      scenario == "U02-hierarchy" ? 2u : 0u;
  const auto node = typedAt<EditorObjectKind::node>(document, nodeIndex);
  if (!scene || !node) {
    if (error)
      *error = "fixture lacks primary scene/node";
    return false;
  }
  bindings.primaryScene = *scene;
  bindings.primaryNode = *node;
  bindings.nodeSelection = nodeSelection(document, *scene, *node);
  bindings.nodeTransform = commands.nodeTransform(*node);
  bindings.secondaryScene =
      typedAt<EditorObjectKind::scene>(document, 1u);
  bindings.secondaryNode =
      typedAt<EditorObjectKind::node>(document, 1u);

  if (scenario == "U02-hierarchy") {
    bindings.reparentSource = commands.captureNodeOccurrence(*scene, *node);
    bindings.reparentTarget = bindings.secondaryNode;
  }

  if (const auto primitive =
          typedAt<EditorObjectKind::primitive>(document, 0u)) {
    const auto mesh = typedAt<EditorObjectKind::mesh>(document, 0u);
    if (mesh) {
      const auto material =
          typedAt<EditorObjectKind::material>(document, 0u);
      bindings.primitiveSelection = EditorSelectionSnapshot{
          document.documentAuthority(),
          PrimitiveSelection{*scene, *node, *primitive, *mesh, 0u}};
      bindings.primitive = ui::PrimitiveContext{
          *scene, *node, *mesh, *primitive, material, true, true, true};
    }
  }
  bindings.primaryMaterial =
      typedAt<EditorObjectKind::material>(document, 0u);
  bindings.alternateMaterial =
      typedAt<EditorObjectKind::material>(document, 1u);
  bindings.camera = typedAt<EditorObjectKind::camera>(document, 0u);
  if (bindings.camera) {
    bindings.cameraNode = commands.cameraNode(*bindings.camera);
    bindings.cameraSelection =
        EditorSelectionSnapshot{document.documentAuthority(), *bindings.camera};
  }
  bindings.light = typedAt<EditorObjectKind::light>(document, 0u);
  if (bindings.light) {
    bindings.lightSelection =
        EditorSelectionSnapshot{document.documentAuthority(), *bindings.light};
  }
  bindings.animation =
      typedAt<EditorObjectKind::animation>(document, 0u);
  return true;
}

std::string fixtureFor(std::string_view scenario) {
  if (scenario == "U02-hierarchy")
    return "multi_scene_oracle.gltf";
  if (scenario == "U03-transform-gizmo" ||
      scenario == "U05-camera-light-vis" ||
      scenario == "U06-merge-compact-save")
    return "feature_preservation.gltf";
  if (scenario == "U04-material")
    return "material_extensions.gltf";
  if (scenario == "U08-large-unicode-dpi")
    return "unicode_names.gltf";
  return "minimal_scene.gltf";
}

std::string lifecycleJson(const std::vector<LifecycleEvidence> &events) {
  std::string output{"["};
  for (std::size_t index = 0; index < events.size(); ++index) {
    if (index != 0)
      output.push_back(',');
    const LifecycleEvidence &event = events[index];
    output += "{\"order\":" + std::to_string(index) + ",\"state\":";
    output += quote(ui::lifecycleStateName(event.state));
    output += ",\"operation\":";
    output += quote(ui::lifecycleOperationName(event.operation));
    output += ",\"label\":" + quote(event.label);
    output += ",\"error\":" + quote(event.error);
    output += ",\"progress\":";
    if (event.progress)
      output += std::to_string(*event.progress);
    else
      output += "null";
    output += "}";
  }
  output += "]";
  return output;
}

struct ScenarioEvidence final {
  std::string id;
  std::string fixture;
  testing::TestScript script;
  testing::ReplayResult result;
  std::string surfaceManifest;
  std::vector<LifecycleEvidence> lifecycle;
  std::size_t publications{0};
  std::size_t canonicalPublications{0};
  std::size_t cameraRequests{0};
  std::size_t boundaryMarkers{0};
  std::string clipboardText;
  bool exitRequested{false};
  bool auditPassed{false};
  std::string failure;
};

bool hasLifecycle(const ScenarioEvidence &evidence, ui::LifecycleState state,
                  std::optional<ui::LifecycleOperation> operation =
                      std::nullopt) {
  return std::any_of(
      evidence.lifecycle.begin(), evidence.lifecycle.end(),
      [state, operation](const LifecycleEvidence &event) {
        return event.state == state &&
               (!operation || event.operation == *operation);
      });
}

ScenarioEvidence runScenario(std::string_view id,
                             const TemporaryDirectory &temporary) {
  ScenarioEvidence evidence;
  evidence.id = std::string(id);
  evidence.fixture = id == "U01-empty-new-open" ? "none"
                                                 : fixtureFor(id);
  Runtime runtime;
  if (id != "U01-empty-new-open") {
    std::string openError;
    if (!runtime.controller->open(fixture(evidence.fixture), &openError)) {
      evidence.failure = "fixture open failed: " + openError;
      return evidence;
    }
  }

  testing::ScenarioBindings bindings;
  std::string bindingError;
  if (!populateBindings(id, runtime, temporary, bindings, &bindingError)) {
    evidence.failure = bindingError;
    return evidence;
  }
  if (id == "U06-merge-compact-save") {
    bindings.saveTarget = temporary.path() / "u06-saved.glb";
  } else if (id == "U07-modified-failures") {
    const std::filesystem::path blocker = temporary.path() / "not-a-directory";
    {
      std::ofstream stream(blocker, std::ios::binary | std::ios::trunc);
      stream << "block";
    }
    bindings.saveTarget = blocker / "save.gltf";
  }

  std::string scriptError;
  auto script = testing::EditorTestDriver::defaultScript(
      id, bindings, &scriptError);
  if (!script) {
    evidence.failure = scriptError;
    return evidence;
  }
  testing::EditorTestDriver driver(runtime.driverCallbacks());
  const auto registration = driver.registerScript(*script);
  if (!registration.accepted) {
    evidence.failure = registration.error;
    return evidence;
  }
  evidence.script = *script;
  evidence.surfaceManifest =
      testing::EditorTestDriver::surfaceManifestJson(driver.snapshot("surface"));
  evidence.result = driver.replay(id);
  evidence.lifecycle = runtime.lifecycle;
  evidence.publications = runtime.publicationCount;
  evidence.canonicalPublications = runtime.canonicalPublicationCount;
  evidence.cameraRequests = runtime.cameraRequests.size();
  evidence.boundaryMarkers = runtime.boundaryMarkers.size();
  evidence.clipboardText = runtime.clipboard.text.value_or("");
  evidence.exitRequested = runtime.exitRequested;

  bool valid = evidence.result.passed &&
               evidence.result.steps.size() == evidence.script.steps.size() &&
               evidence.boundaryMarkers == 1u;
  bool hasDomainAction = false;
  for (std::size_t index = 0; index < evidence.script.steps.size(); ++index) {
    const auto kind = testing::EditorTestDriver::kindOf(
        evidence.script.steps[index].action.payload);
    hasDomainAction |= kind != testing::ActionKind::captureState &&
                       kind != testing::ActionKind::screenshotMarker;
    if (index < evidence.result.steps.size()) {
      valid &= evidence.result.steps[index].beforeState.authority.has_value();
      valid &= evidence.result.steps[index].state.authority.has_value();
    }
    valid &= evidence.script.steps[index].expectedState.has_value();
    valid &= evidence.script.steps[index].expectedAccepted.has_value();
  }
  valid &= hasDomainAction;
  const auto stepResult = [&evidence](std::string_view actionId)
      -> const testing::ActionResult * {
    const auto found = std::find_if(
        evidence.result.steps.begin(), evidence.result.steps.end(),
        [actionId](const testing::ActionResult &result) {
          return result.actionId == actionId;
        });
    return found == evidence.result.steps.end() ? nullptr : &*found;
  };
  const auto changedDocument = [](const testing::ActionResult *result) {
    return result != nullptr && result->beforeState.authority &&
           result->state.authority &&
           result->beforeState.authority->documentDigest !=
               result->state.authority->documentDigest;
  };
  const auto preservedAuthority = [](const testing::ActionResult *result) {
    return result != nullptr && result->beforeState.authority &&
           result->state.authority &&
           result->beforeState.authority->hasDocument &&
           result->state.authority->hasDocument &&
           result->beforeState.authority->documentRevision ==
               result->state.authority->documentRevision &&
           result->beforeState.authority->documentDigest ==
               result->state.authority->documentDigest &&
           result->beforeState.authority->projectionDigest ==
               result->state.authority->projectionDigest &&
           result->beforeState.authority->undoCount ==
               result->state.authority->undoCount &&
           result->beforeState.authority->redoCount ==
               result->state.authority->redoCount;
  };
  if (id == "U01-empty-new-open")
    valid &= hasLifecycle(evidence, ui::LifecycleState::loadingCandidate) &&
             stepResult("open-valid") != nullptr &&
             stepResult("open-valid")->state.authority &&
             stepResult("open-valid")->state.authority->hasDocument &&
             stepResult("open-valid")->state.authority->hasProjection;
  if (id == "U02-hierarchy") {
    const auto *move = stepResult("cross-parent-drop");
    valid &= changedDocument(move) && move->state.authority->undoCount == 1u &&
             changedDocument(stepResult("undo-duplicate")) &&
             changedDocument(stepResult("redo-duplicate"));
  }
  if (id == "U03-transform-gizmo") {
    valid &= hasLifecycle(evidence, ui::LifecycleState::gizmoHover) &&
             hasLifecycle(evidence, ui::LifecycleState::gizmoDragging) &&
             changedDocument(stepResult("gizmo-preview")) &&
             stepResult("gizmo-release") != nullptr &&
             stepResult("gizmo-release")->state.authority &&
             stepResult("gizmo-release")->state.authority->undoCount == 1u;
  }
  if (id == "U04-material") {
    valid &= changedDocument(stepResult("assign-alternate")) &&
             changedDocument(stepResult("paste-canonical")) &&
             evidence.clipboardText == "gltf_material:0";
  }
  if (id == "U05-camera-light-vis") {
    valid &= evidence.cameraRequests == 1u &&
             changedDocument(stepResult("edit-camera-yfov")) &&
             changedDocument(stepResult("edit-light-intensity")) &&
             changedDocument(stepResult("hide-node"));
  }
  if (id == "U06-merge-compact-save") {
    valid &= hasLifecycle(evidence,
                          ui::LifecycleState::savingMergingCompacting,
                          ui::LifecycleOperation::merge);
    valid &= hasLifecycle(evidence,
                          ui::LifecycleState::savingMergingCompacting,
                          ui::LifecycleOperation::compact);
    valid &= hasLifecycle(evidence,
                          ui::LifecycleState::savingMergingCompacting,
                          ui::LifecycleOperation::save);
    const auto *save = stepResult("save-as");
    const auto *reload = stepResult("reload-saved");
    valid &= changedDocument(stepResult("merge-document")) && save != nullptr &&
             save->state.authority &&
             save->state.authority->documentRevision ==
                 save->state.authority->cleanRevision &&
             reload != nullptr && reload->beforeState.authority &&
             reload->state.authority &&
             reload->beforeState.authority->documentDigest ==
                 reload->state.authority->documentDigest;
  }
  if (id == "U07-modified-failures") {
    valid &= hasLifecycle(evidence, ui::LifecycleState::error) &&
             preservedAuthority(stepResult("merge-invalid")) &&
             preservedAuthority(stepResult("save-invalid")) &&
             preservedAuthority(stepResult("open-invalid"));
  }
  if (id == "U08-large-unicode-dpi") {
    const auto *rename = stepResult("rename-long-unicode");
    valid &= changedDocument(rename) && rename->state.authority &&
             rename->state.authority->selectionKind == "Node";
  }
  evidence.auditPassed = valid;
  if (!valid && evidence.failure.empty())
    evidence.failure = "real controller replay or evidence invariant failed";
  return evidence;
}

std::string scenarioJson(const ScenarioEvidence &evidence) {
  std::string output{"{\"id\":"};
  output += quote(evidence.id);
  output += ",\"fixture\":" + quote(evidence.fixture);
  output += ",\"auditPassed\":";
  output += evidence.auditPassed ? "true" : "false";
  output += ",\"failure\":" + quote(evidence.failure);
  output += ",\"script\":" + evidence.script.toJson();
  output += ",\"result\":" + evidence.result.toJson();
  output += ",\"surfaceManifest\":" + evidence.surfaceManifest;
  output += ",\"lifecycle\":" + lifecycleJson(evidence.lifecycle);
  output += ",\"publications\":" + std::to_string(evidence.publications);
  output += ",\"canonicalPublications\":" +
            std::to_string(evidence.canonicalPublications);
  output += ",\"cameraRequests\":" +
            std::to_string(evidence.cameraRequests);
  output += ",\"clipboardText\":" + quote(evidence.clipboardText);
  output += ",\"boundaryMarkers\":" +
            std::to_string(evidence.boundaryMarkers);
  output += ",\"screenshotFiles\":0,\"exitRequested\":";
  output += evidence.exitRequested ? "true" : "false";
  output += "}";
  return output;
}

std::optional<std::filesystem::path> outputPath(int argc, char **argv) {
  if (argc == 1)
    return std::nullopt;
  if (argc == 3 && std::string_view(argv[1]) == "--output")
    return std::filesystem::path(argv[2]);
  return std::filesystem::path{};
}

} // namespace

int main(int argc, char **argv) {
  const auto requestedOutput = outputPath(argc, argv);
  if (requestedOutput && requestedOutput->empty()) {
    std::cerr << "usage: editor_ui_replay_tests [--output <json-path>]\n";
    return 2;
  }

  TemporaryDirectory temporary;
  if (temporary.path().empty()) {
    std::cerr << "FAILED: could not create temporary replay directory\n";
    return 1;
  }
  static constexpr std::array<std::string_view, 8> ids = {
      "U01-empty-new-open", "U02-hierarchy", "U03-transform-gizmo",
      "U04-material", "U05-camera-light-vis", "U06-merge-compact-save",
      "U07-modified-failures", "U08-large-unicode-dpi"};
  std::vector<ScenarioEvidence> evidence;
  evidence.reserve(ids.size());
  bool passed = true;
  for (const std::string_view id : ids) {
    evidence.push_back(runScenario(id, temporary));
    passed &= expect(evidence.back().auditPassed,
                     std::string(id) + " real controller replay");
  }

  std::string json =
      "{\"schemaVersion\":1,\"kind\":\"mgif.editor-ui-controller-replay\",";
  json += "\"controller\":\"EditorController\",\"mockHost\":false,";
  json += "\"screenshotEvidence\":false,\"markerMeaning\":";
  json += quote("boundary-only-not-image-evidence");
  json += ",\"scenarioCount\":8,\"executionPassed\":";
  json += passed ? "true" : "false";
  json += ",\"acceptanceVerdict\":";
  json += quote("INCOMPLETE_VISUAL_COVERAGE");
  json += ",\"actionSchema\":" +
          testing::EditorTestDriver::actionSchemaJson();
  json += ",\"stateSchema\":" +
          testing::EditorTestDriver::stateSchemaJson();
  json += ",\"scriptSchema\":" +
          testing::EditorTestDriver::scriptSchemaJson();
  json += ",\"scenarios\":[";
  for (std::size_t index = 0; index < evidence.size(); ++index) {
    if (index != 0)
      json.push_back(',');
    json += scenarioJson(evidence[index]);
  }
  json += "]}";

  if (requestedOutput) {
    std::error_code directoryError;
    if (!requestedOutput->parent_path().empty())
      std::filesystem::create_directories(requestedOutput->parent_path(),
                                          directoryError);
    std::ofstream output(*requestedOutput, std::ios::binary | std::ios::trunc);
    output.write(json.data(), static_cast<std::streamsize>(json.size()));
    output.put('\n');
    if (!output || directoryError) {
      std::cerr << "FAILED: could not write replay evidence\n";
      return 1;
    }
  } else {
    std::cout << json << '\n';
  }

  if (!passed) {
    for (const ScenarioEvidence &scenario : evidence) {
      if (!scenario.auditPassed)
        std::cerr << scenario.id << ": " << scenario.failure << '\n';
    }
    return 1;
  }
  std::cout << "Editor UI controller replay passed " << g_checks
            << " scenario checks; visual verdict remains INCOMPLETE\n";
  return 0;
}
