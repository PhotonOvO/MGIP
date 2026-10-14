#include "../../app/editor/EditorController.h"
#include "../../scene/editor/SceneProjectionDetail.h"

#include <mgif/scene_editor/GltfDocumentIO.h>
#include <mgif/scene_editor/GltfSemanticCompare.h>
#include <mgif/scene_editor/commands/HierarchyCommands.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace demo::editor::detail {
class GltfDocumentAccess final {
public:
  [[nodiscard]] static std::string canonicalText(const GltfDocument& document) {
    std::string error;
    return document.semanticRepresentation(error);
  }
};
} // namespace demo::editor::detail

namespace demo::editor::ui {

struct EditorControllerTestAccess final {
  [[nodiscard]] static bool
  nodeNameFastPathUsed(const EditorController& controller) noexcept {
    return controller.m_nodeNameFastPathUsed;
  }

  [[nodiscard]] static bool
  hasNodeNameTransaction(const EditorController& controller) noexcept {
    return controller.m_nodeNameTransaction.has_value();
  }

  static void setGizmoFinishSequence(EditorController& controller,
                                      std::uint64_t sequence) noexcept {
    controller.m_gizmoFinishSequence = sequence;
  }

  [[nodiscard]] static std::uint64_t
  gizmoFinishSequence(const EditorController& controller) noexcept {
    return controller.m_gizmoFinishSequence;
  }
};

} // namespace demo::editor::ui

namespace {

using namespace demo::editor;
namespace ui = demo::editor::ui;

bool expect(bool condition, std::string_view message) {
  if(!condition)
    std::cerr << "FAILED: " << message << '\n';
  return condition;
}

std::filesystem::path fixture(std::string_view name) {
  return std::filesystem::path(MGIF_TEST_SOURCE_DIR) / "tests" /
         "fixtures" / "gltf_editor" / name;
}

std::vector<std::uint8_t> readBytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if(!input)
    return {};
  input.seekg(0, std::ios::end);
  const std::streamoff size = input.tellg();
  if(size <= 0)
    return {};
  input.seekg(0, std::ios::beg);
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  input.read(reinterpret_cast<char*>(bytes.data()), size);
  if(input.gcount() != size)
    return {};
  return bytes;
}

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    const auto ticks =
        std::chrono::steady_clock::now().time_since_epoch().count();
    for(std::uint32_t attempt = 0u; attempt < 100u; ++attempt) {
      std::error_code error;
      const auto candidate = std::filesystem::temp_directory_path() /
                             ("mgif-editor-transaction-" +
                              std::to_string(ticks) + "-" +
                              std::to_string(attempt));
      if(std::filesystem::create_directory(candidate, error)) {
        m_path = candidate;
        return;
      }
    }
  }

  ~TemporaryDirectory() {
    if(!m_path.empty()) {
      std::error_code ignored;
      std::filesystem::remove_all(m_path, ignored);
    }
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return m_path;
  }

private:
  std::filesystem::path m_path;
};

struct Observer final {
  std::size_t publications{0u};
  std::vector<ui::ProjectionRefresh> refreshes;
  std::vector<SceneDigest> documentDigests;
  std::vector<SceneDigest> projectionDigests;
};

ui::EditorController::Callbacks callbacksFor(Observer& observer) {
  ui::EditorController::Callbacks callbacks;
  callbacks.onPublication =
      [&](const ui::EditorController::ProjectionPublication& publication) {
        ++observer.publications;
        if(publication.projection != nullptr) {
          observer.refreshes.push_back(publication.refresh);
          observer.documentDigests.push_back(
              publication.projection->documentDigest);
          observer.projectionDigests.push_back(
              publication.projection->projectionDigest);
        }
      };
  return callbacks;
}

ui::RuntimeActionResult dispatchRename(ui::EditorController& controller,
                                       EditorObjectId id,
                                       std::string name) {
  ui::UiCommand command;
  command.sequence = 0u;
  command.source = ui::CommandSource::sceneBrowser;
  command.payload = ui::RenameObject{id, std::move(name)};
  return controller.dispatch(command);
}

ui::RuntimeActionResult dispatchHistory(ui::EditorController& controller,
                                        ui::CommandId id) {
  ui::UiCommand command;
  command.sequence = 0u;
  command.source = ui::CommandSource::shortcut;
  command.payload = ui::InvokeCommand{id, std::nullopt};
  return controller.dispatch(command);
}

ui::RuntimeActionResult dispatchSave(ui::EditorController& controller,
                                     const std::filesystem::path& path) {
  ui::UiCommand command;
  command.sequence = 0u;
  command.source = ui::CommandSource::dialog;
  command.payload = ui::InvokeCommand{ui::CommandId::saveAs, path};
  return controller.dispatch(command);
}

ui::RuntimeActionResult dispatchRebuild(ui::EditorController& controller) {
  ui::UiCommand command;
  command.sequence = 0u;
  command.source = ui::CommandSource::sceneBrowser;
  command.payload = ui::RebuildRenderNodes{};
  return controller.dispatch(command);
}

ui::RuntimeActionResult dispatchHierarchy(
    ui::EditorController& controller, ui::HierarchyAction action) {
  ui::UiCommand command;
  command.sequence = 0u;
  command.source = ui::CommandSource::sceneBrowser;
  command.payload = std::move(action);
  return controller.dispatch(command);
}

bool equivalentProjection(const SceneProjectionResult& optimized,
                          const SceneProjectionResult& full,
                          std::string_view phase) {
  bool passed = true;
  const auto check = [&](bool condition, std::string_view field) {
    passed &= expect(condition, std::string(phase) + ": " +
                                    std::string(field));
  };
  check(optimized.succeeded() && full.succeeded(),
        "both projection paths succeed");
  check(optimized.error == full.error, "error state matches");
  check(optimized.semanticHash == full.semanticHash,
        "semantic hash matches");
  check(optimized.documentRevision == full.documentRevision,
        "document revision matches");
  check(optimized.sceneActivationEpoch == full.sceneActivationEpoch,
        "scene activation epoch matches");
  check(optimized.mappingEpoch == full.mappingEpoch,
        "mapping epoch matches");
  check(optimized.activeScene == full.activeScene,
        "active scene matches");
  check(optimized.documentDigest == full.documentDigest,
        "document digest matches");
  check(optimized.projectionDigest == full.projectionDigest,
        "projection digest matches");
  check(optimized.immutableSnapshot == full.immutableSnapshot,
        "immutable canonical bytes and content digest match");
  check(optimized.mapping == full.mapping,
        "mapping entries, aliases, identities, and fence match");
  check(optimized.textureBindings == full.textureBindings,
        "texture bindings match");
  check(optimized.samplerBindings == full.samplerBindings,
        "sampler bindings match");
  check(optimized.activeVariant == full.activeVariant,
        "active variant matches");
  check(optimized.activeAnimation == full.activeAnimation,
        "active animation matches");
  check(optimized.animationTime == full.animationTime &&
            optimized.animationLoop == full.animationLoop,
        "animation runtime state matches");
  check(optimized.sceneRootTransform.baseline ==
                full.sceneRootTransform.baseline &&
            optimized.sceneRootTransform.current ==
                full.sceneRootTransform.current &&
            optimized.sceneRootTransform.hierarchyEpoch ==
                full.sceneRootTransform.hierarchyEpoch &&
            optimized.sceneRootTransform.externalStateSerial ==
                full.sceneRootTransform.externalStateSerial &&
            optimized.sceneRootTransform.frozen ==
                full.sceneRootTransform.frozen,
        "root state matches");
  check(optimized.asset.name == full.asset.name &&
            optimized.asset.sourcePath == full.asset.sourcePath &&
            optimized.asset.documentSemanticHash ==
                full.asset.documentSemanticHash &&
            optimized.asset.documentRevision == full.asset.documentRevision,
        "asset identity metadata matches");
  return passed;
}

bool testNodeNameFastPath(std::string name, std::string_view label) {
  GltfLoadResult loaded =
      GltfDocumentIO::loadCandidate(fixture("minimal_scene.gltf"));
  bool passed = expect(loaded.succeeded() && loaded.candidate() != nullptr,
                       std::string(label) + ": fixture loads");
  if(!loaded.candidate())
    return false;
  SessionPrepareResult prepared =
      EditorSessionBundle::prepare(std::move(loaded).takeCandidate());
  passed &= expect(prepared.succeeded(),
                   std::string(label) + ": session prepares");
  if(!prepared.session)
    return false;

  EditorSessionBundle& session = *prepared.session;
  const auto node = NodeId::from(
      session.document().idAt(EditorObjectKind::node, 0u));
  passed &= expect(node.has_value(), std::string(label) + ": node is stable");
  if(!node)
    return false;

  SceneProjectionResult baseline = SceneProjection::project(session.document());
  passed &= expect(baseline.succeeded(),
                   std::string(label) + ": baseline projects");
  if(!baseline)
    return false;
  detail::NodeNameProjectionDigestCache baselineCache{
      baseline.documentDigest, baseline.semanticHash};

  CommandPrepareResult rename = prepareRenameNodeCommand(
      session.editor().commandContext(), *node, std::move(name));
  passed &= expect(rename.succeeded(),
                   std::string(label) + ": rename prepares");
  if(!rename.succeeded())
    return false;
  const HistoryResult executed =
      session.editor().history().execute(std::move(rename.command));
  passed &= expect(executed.succeeded(),
                   std::string(label) + ": rename executes");

  SceneProjectionOptions options;
  options.baseMappingEpoch = baseline.mappingEpoch;
  auto optimizedRename = detail::reprojectNodeNameOnly(
      session.document(), baseline, *node, options);
  const SceneProjectionResult fullRename =
      SceneProjection::project(session.document(), options);
  passed &= expect(optimizedRename.has_value(),
                   std::string(label) + ": rename takes optimized path");
  if(!optimizedRename)
    return false;
  passed &= equivalentProjection(*optimizedRename, fullRename,
                                 std::string(label) + " rename");
  detail::NodeNameProjectionDigestCache renamedCache{
      optimizedRename->documentDigest, optimizedRename->semanticHash};

  const HistoryResult undone = session.editor().history().undo();
  passed &= expect(undone.succeeded(),
                   std::string(label) + ": undo executes");
  options.baseMappingEpoch = optimizedRename->mappingEpoch;
  auto optimizedUndo = detail::reprojectNodeNameOnly(
      session.document(), *optimizedRename, *node, options, baselineCache);
  const SceneProjectionResult fullUndo =
      SceneProjection::project(session.document(), options);
  passed &= expect(optimizedUndo.has_value(),
                   std::string(label) + ": undo takes optimized path");
  if(!optimizedUndo)
    return false;
  passed &= equivalentProjection(*optimizedUndo, fullUndo,
                                 std::string(label) + " undo");

  const HistoryResult redone = session.editor().history().redo();
  passed &= expect(redone.succeeded(),
                   std::string(label) + ": redo executes");
  options.baseMappingEpoch = optimizedUndo->mappingEpoch;
  auto optimizedRedo = detail::reprojectNodeNameOnly(
      session.document(), *optimizedUndo, *node, options, renamedCache);
  const SceneProjectionResult fullRedo =
      SceneProjection::project(session.document(), options);
  passed &= expect(optimizedRedo.has_value(),
                   std::string(label) + ": redo takes optimized path");
  if(!optimizedRedo)
    return false;
  passed &= equivalentProjection(*optimizedRedo, fullRedo,
                                 std::string(label) + " redo");
  return passed;
}

bool testNonNameMutationFallsBack() {
  GltfLoadResult loaded =
      GltfDocumentIO::loadCandidate(fixture("minimal_scene.gltf"));
  if(!loaded.succeeded() || !loaded.candidate())
    return expect(false, "fallback fixture loads");
  SessionPrepareResult prepared =
      EditorSessionBundle::prepare(std::move(loaded).takeCandidate());
  if(!prepared.session)
    return expect(false, "fallback session prepares");
  EditorSessionBundle& session = *prepared.session;
  const auto node = NodeId::from(
      session.document().idAt(EditorObjectKind::node, 0u));
  if(!node)
    return expect(false, "fallback node is stable");
  const SceneProjectionResult baseline =
      SceneProjection::project(session.document());
  auto transform = session.editor().commands().nodeTransform(*node);
  if(!baseline || !transform)
    return expect(false, "fallback baseline and transform are available");
  transform->translation[0] += 1.0;
  const MutationResult mutation =
      session.editor().commands().setNodeTransform(*node, *transform);
  if(!mutation.applied())
    return expect(false, "fallback transform mutation applies");
  SceneProjectionOptions options;
  options.baseMappingEpoch = baseline.mappingEpoch;
  return expect(!detail::reprojectNodeNameOnly(
                     session.document(), baseline, *node, options),
                "non-name mutation rejects the node-name fast path");
}

bool testAnimatedNodeNameFastPathFences() {
  GltfLoadResult loaded =
      GltfDocumentIO::loadCandidate(fixture("feature_preservation.gltf"));
  if(!loaded.succeeded() || !loaded.candidate())
    return expect(false, "animated fast-path fixture loads");
  SessionPrepareResult prepared =
      EditorSessionBundle::prepare(std::move(loaded).takeCandidate());
  if(!prepared.session)
    return expect(false, "animated fast-path session prepares");
  EditorSessionBundle& session = *prepared.session;
  const auto node = NodeId::from(
      session.document().idAt(EditorObjectKind::node, 0u));
  if(!node)
    return expect(false, "animated fast-path node is stable");

  SceneProjectionOptions options;
  options.activeAnimation = 0u;
  options.animationTime = 0.5;
  options.animationLoop = false;
  options.animationPose = SceneAnimationPose{
      0u, 0.5, false,
      {AnimationTransformSample{
          0u, std::array<double, 3>{0.5, 0.5, 0.5}, std::nullopt,
          std::nullopt}}};
  const SceneProjectionResult baseline =
      SceneProjection::project(session.document(), options);
  bool passed = expect(baseline.succeeded(),
                       "animated baseline projection succeeds");
  if(!baseline)
    return false;

  CommandPrepareResult rename = prepareRenameNodeCommand(
      session.editor().commandContext(), *node, "animated-name-fast-path");
  if(!rename.succeeded())
    return expect(false, "animated rename prepares");
  const HistoryResult executed =
      session.editor().history().execute(std::move(rename.command));
  passed &= expect(executed.succeeded() && executed.command.accepted(),
                   "animated rename executes");
  if(!executed.succeeded())
    return false;

  const detail::NodeNameProjectionDigestCache targetCache{
      executed.command.changes.sourceMutation.targetDocumentDigest,
      GltfSemanticCompare::digest(session.document())};
  options.baseMappingEpoch = baseline.mappingEpoch;
  const auto optimized = detail::reprojectNodeNameOnly(
      session.document(), baseline, *node, options, targetCache);
  const SceneProjectionResult full =
      SceneProjection::project(session.document(), options);
  passed &= expect(optimized.has_value(),
                   "coherent active animation pose permits the name fast path");
  if(optimized) {
    passed &= equivalentProjection(*optimized, full,
                                    "animated rename fast path");
  }

  SceneProjectionOptions ambiguous = options;
  ambiguous.animationPose->transforms.push_back(
      ambiguous.animationPose->transforms.front());
  passed &= expect(!detail::reprojectNodeNameOnly(
                        session.document(), baseline, *node, ambiguous,
                        targetCache),
                   "ambiguous duplicate animation samples fall back");

  EditorObjectId staleValue = node->untyped();
  ++staleValue.generation;
  const auto staleNode = NodeId::from(staleValue);
  passed &= expect(staleNode.has_value() &&
                       !detail::reprojectNodeNameOnly(
                           session.document(), baseline, *staleNode, options,
                           targetCache),
                   "stale node generation cannot use the name fast path");

  SceneProjectionResult wrongAssetRevision = baseline;
  ++wrongAssetRevision.asset.documentRevision;
  passed &= expect(!detail::reprojectNodeNameOnly(
                        session.document(), wrongAssetRevision, *node, options,
                        targetCache),
                   "asset revision mismatch invalidates the previous envelope");
  SceneProjectionResult wrongAssetSemantic = baseline;
  wrongAssetSemantic.asset.documentSemanticHash.append("-stale");
  passed &= expect(!detail::reprojectNodeNameOnly(
                        session.document(), wrongAssetSemantic, *node, options,
                        targetCache),
                   "asset semantic mismatch invalidates the previous envelope");
  return passed;
}

bool testControllerFastPathLifecycle() {
  Observer observer;
  ui::EditorController controller(callbacksFor(observer));
  std::string error;
  bool passed = expect(controller.open(fixture("minimal_scene.gltf"), &error),
                       "controller fast-path fixture opens");
  if(!controller.session() || !controller.projection())
    return false;
  const auto node = NodeId::from(
      controller.session()->document().idAt(EditorObjectKind::node, 0u));
  if(!node)
    return expect(false, "controller fast-path node is stable");

  const SceneProjectionResult baseline = *controller.projection();
  const auto renamed = dispatchRename(controller, node->untyped(),
                                      "controller-fast-path");
  passed &= expect(renamed.accepted &&
                       ui::EditorControllerTestAccess::nodeNameFastPathUsed(
                           controller) &&
                       ui::EditorControllerTestAccess::hasNodeNameTransaction(
                           controller),
                   "controller proves and retains the real rename fast path");
  SceneProjectionOptions freshOptions;
  freshOptions.baseMappingEpoch = baseline.mappingEpoch;
  const SceneProjectionResult freshRename =
      SceneProjection::project(controller.session()->document(), freshOptions);
  passed &= equivalentProjection(*controller.projection(), freshRename,
                                  "controller rename forced-fresh proof");

  const auto rebuilt = dispatchRebuild(controller);
  passed &= expect(rebuilt.accepted &&
                       !ui::EditorControllerTestAccess::nodeNameFastPathUsed(
                           controller) &&
                       !ui::EditorControllerTestAccess::hasNodeNameTransaction(
                           controller),
                   "unrelated full rebuild clears the rename hint");

  const auto renamedAgain = dispatchRename(
      controller, node->untyped(), "controller-fast-path-again");
  passed &= expect(renamedAgain.accepted &&
                       ui::EditorControllerTestAccess::hasNodeNameTransaction(
                           controller),
                   "second controller rename retains a fresh hint");
  controller.unload();
  passed &= expect(!ui::EditorControllerTestAccess::nodeNameFastPathUsed(
                         controller) &&
                       !ui::EditorControllerTestAccess::hasNodeNameTransaction(
                           controller),
                   "unload clears all rename fast-path state");

  error.clear();
  passed &= expect(controller.open(fixture("minimal_scene.gltf"), &error),
                   "controller reopens for replacement lifecycle proof");
  if(!controller.session() || !controller.projection())
    return false;
  const auto replacementNode = NodeId::from(
      controller.session()->document().idAt(EditorObjectKind::node, 0u));
  if(!replacementNode)
    return expect(false, "replacement lifecycle node is stable");
  passed &= expect(dispatchRename(controller, replacementNode->untyped(),
                                  "replacement-fast-path").accepted &&
                       ui::EditorControllerTestAccess::hasNodeNameTransaction(
                           controller),
                   "replacement lifecycle starts with a retained hint");
  GltfLoadResult replacement =
      GltfDocumentIO::loadCandidate(fixture("minimal_scene.gltf"));
  passed &= expect(replacement.succeeded() && replacement.candidate() != nullptr,
                   "replacement candidate loads");
  if(replacement.candidate()) {
    passed &= expect(controller.replaceDocument(
                         std::move(replacement).takeCandidate(), &error),
                     "replacement candidate publishes");
    passed &= expect(!ui::EditorControllerTestAccess::nodeNameFastPathUsed(
                           controller) &&
                         !ui::EditorControllerTestAccess::hasNodeNameTransaction(
                             controller),
                     "replaceDocument clears cross-authority rename state");
  }
  return passed;
}

bool testNativeGizmoPreservesUiCommandSequence() {
  Observer observer;
  ui::EditorController controller(callbacksFor(observer));
  std::string error;
  bool passed = expect(controller.open(fixture("minimal_scene.gltf"), &error),
                       "native sequence fixture opens");
  if(!controller.session() || !controller.projection())
    return false;
  auto& session = *controller.session();
  const auto scene = SceneId::from(session.document().idAt(EditorObjectKind::scene, 0u));
  const auto node = NodeId::from(session.document().idAt(EditorObjectKind::node, 0u));
  if(!scene || !node)
    return expect(false, "native sequence scene and node are stable");
  const EditorSelectionSnapshot selection{
      session.document().documentAuthority(), NodeSelection{*scene, *node, std::nullopt}};
  passed &= expect(controller.dispatch(
                       {1u, ui::CommandSource::sceneBrowser,
                        ui::SelectObject{selection}}).accepted,
                   "UI sequence 1 selects the native gizmo node");
  const auto before = controller.viewportNodeTransform(*node);
  if(!before)
    return expect(false, "native sequence baseline TRS is available");
  passed &= expect(!controller.buildFrameModel().viewportOwnsEscape,
                   "an idle selected node does not own viewport Escape");
  NodeTransform preview = *before;
  preview.translation[1] += 0.5;
  ui::EditorControllerTestAccess::setGizmoFinishSequence(
      controller, std::numeric_limits<std::uint64_t>::max());
  passed &= expect(controller.beginGizmoTransform(*node).accepted &&
                       ui::EditorControllerTestAccess::gizmoFinishSequence(controller) == 0u &&
                       controller.buildFrameModel().viewportOwnsEscape,
                   "accepted native begin resets its ordinal and owns viewport Escape");
  passed &= expect(controller.previewGizmoTransform(*node, preview).accepted &&
                       controller.finishGizmoTransform(*node, true, false, false).accepted,
                   "native preview and physical release commit succeed");
  passed &= expect(session.editor().history().undoCount() == 1u &&
                       session.editor().history().redoCount() == 0u &&
                       controller.viewportNodeTransform(*node) == preview &&
                       !controller.buildFrameModel().viewportOwnsEscape,
                   "native release retains preview, creates one history entry, and releases Escape ownership");

  passed &= expect(controller.dispatch(
                       {2u, ui::CommandSource::shortcut,
                        ui::InvokeCommand{ui::CommandId::undo, std::nullopt}}).accepted &&
                       controller.viewportNodeTransform(*node) == before &&
                       session.editor().history().undoCount() == 0u &&
                       session.editor().history().redoCount() == 1u,
                   "the next UI command sequence 2 undoes a native gesture");
  passed &= expect(controller.dispatch(
                       {3u, ui::CommandSource::shortcut,
                        ui::InvokeCommand{ui::CommandId::redo, std::nullopt}}).accepted &&
                       controller.viewportNodeTransform(*node) == preview &&
                       session.editor().history().undoCount() == 1u &&
                       session.editor().history().redoCount() == 0u,
                   "UI command sequence 3 redoes a native gesture");
  const SceneDigest beforeStale = session.document().documentDigest();
  const auto stale = controller.dispatch(
      {2u, ui::CommandSource::shortcut,
       ui::InvokeCommand{ui::CommandId::undo, std::nullopt}});
  passed &= expect(!stale.accepted && stale.error == "stale editor command sequence" &&
                       session.document().documentDigest() == beforeStale &&
                       session.editor().history().undoCount() == 1u &&
                       session.editor().history().redoCount() == 0u,
                   "native sequence isolation preserves strict stale UI rejection");
  for(int index = 0; index < 3; ++index)
    passed &= expect(controller.finishGizmoTransform(*node, true, false, false).accepted,
                     "repeated native late release remains a no-op");
  passed &= expect(controller.dispatch(
                       {4u, ui::CommandSource::shortcut,
                        ui::InvokeCommand{ui::CommandId::undo, std::nullopt}}).accepted &&
                       controller.viewportNodeTransform(*node) == before,
                   "repeated native late releases do not consume the next UI sequence");
  return passed;
}

bool testNativeEscapeAndFocusLossPreserveUiCommandSequence(bool focusLoss) {
  Observer observer;
  ui::EditorController controller(callbacksFor(observer));
  std::string error;
  bool passed = expect(controller.open(fixture("minimal_scene.gltf"), &error),
                       "native terminal sequence fixture opens");
  if(!controller.session() || !controller.projection())
    return false;
  auto& session = *controller.session();
  const auto scene = SceneId::from(session.document().idAt(EditorObjectKind::scene, 0u));
  const auto node = NodeId::from(session.document().idAt(EditorObjectKind::node, 0u));
  if(!scene || !node)
    return expect(false, "native terminal scene and node are stable");
  const EditorSelectionSnapshot selection{
      session.document().documentAuthority(), NodeSelection{*scene, *node, std::nullopt}};
  passed &= expect(controller.dispatch(
                       {1u, ui::CommandSource::sceneBrowser,
                        ui::SelectObject{selection}}).accepted,
                   "UI selection precedes native Escape or focus-loss terminals");
  const auto before = controller.viewportNodeTransform(*node);
  if(!before)
    return expect(false, "native terminal baseline TRS is available");
  NodeTransform preview = *before;
  preview.translation[1] += 0.75;
  passed &= expect(controller.beginGizmoTransform(*node).accepted &&
                       controller.previewGizmoTransform(*node, preview).accepted &&
                       controller.buildFrameModel().viewportOwnsEscape,
                   "native terminal regression starts a meaningful preview");
  if(focusLoss) {
    for(int index = 0; index < 2; ++index)
      passed &= expect(controller.finishGizmoTransform(*node, false, false, true).accepted,
                       "repeated focus loss keeps the native gesture latched");
    passed &= expect(controller.buildFrameModel().viewportOwnsEscape,
                     "a latched native gesture retains viewport Escape ownership");
    // The host-side presentation flag can briefly be cleared while the
    // continuous session remains suspended. A late physical release must
    // reassert the session's ownership instead of committing or leaking
    // Escape to the rest of the UI.
    controller.setGizmoInteraction(false, false);
    passed &= expect(controller.finishGizmoTransform(*node, true, false, false).accepted &&
                         session.editor().continuousEdit().active() &&
                         session.editor().history().undoCount() == 0u &&
                         controller.buildFrameModel().viewportOwnsEscape,
                     "latched late release reasserts ownership without history");
  }
  passed &= expect(controller.finishGizmoTransform(*node, false, true, false).accepted,
                   "Escape detaches the native gesture");
  for(int index = 0; index < 3; ++index)
    passed &= expect(controller.finishGizmoTransform(*node, true, false, false).accepted,
                     "detached native late releases remain harmless");
  passed &= expect(controller.viewportNodeTransform(*node) == preview &&
                       !session.editor().continuousEdit().active() &&
                       session.editor().history().undoCount() == 0u &&
                       session.editor().history().redoCount() == 0u &&
                       controller.buildFrameModel().selection.emptyValue() &&
                       !controller.buildFrameModel().viewportOwnsEscape,
                   "Escape and late releases retain preview without history or selection");
  passed &= expect(controller.dispatch(
                       {2u, ui::CommandSource::sceneBrowser,
                        ui::SelectObject{selection}}).accepted,
                   "next UI sequence 2 remains valid after repeated native terminals");
  const auto stale = controller.dispatch(
      {1u, ui::CommandSource::sceneBrowser, ui::SelectObject{selection}});
  passed &= expect(!stale.accepted && stale.error == "stale editor command sequence",
                   "Escape/focus loss does not weaken stale UI sequence rejection");
  return passed;
}

bool testInspectorContinuousEditDoesNotOwnViewportEscape() {
  Observer observer;
  ui::EditorController controller(callbacksFor(observer));
  std::string error;
  bool passed = expect(controller.open(fixture("minimal_scene.gltf"), &error),
                       "Inspector Escape ownership fixture opens");
  if(!controller.session())
    return false;
  const auto node = NodeId::from(
      controller.session()->document().idAt(EditorObjectKind::node, 0u));
  if(!node)
    return expect(false, "Inspector Escape ownership node is stable");
  const auto before = controller.viewportNodeTransform(*node);
  if(!before)
    return expect(false, "Inspector Escape ownership TRS is available");
  const auto edit = [&](std::uint64_t sequence, ui::EditPhase phase,
                         std::array<double, 3> translation) {
    return controller.dispatch(
        {sequence, ui::CommandSource::inspector,
         ui::PropertyEdit{node->untyped(), ui::FieldId::translation,
                          phase, translation}});
  };
  passed &= expect(edit(1u, ui::EditPhase::begin, before->translation).accepted,
                   "real Inspector continuous edit begins");
  const ui::FrameModel begun = controller.buildFrameModel();
  passed &= expect(begun.lifecycle == ui::LifecycleState::gizmoDragging &&
                       !begun.viewportOwnsEscape,
                   "shared dragging lifecycle does not grant Inspector viewport Escape ownership");
  auto translation = before->translation;
  translation[0] += 0.25;
  passed &= expect(edit(2u, ui::EditPhase::preview, translation).accepted &&
                       !controller.buildFrameModel().viewportOwnsEscape &&
                       edit(3u, ui::EditPhase::commit, translation).accepted &&
                       !controller.buildFrameModel().viewportOwnsEscape,
                   "Inspector preview and commit never claim native Escape ownership");
  return passed;
}

bool testInspectorRejectedInputsKeepProjectionAndPickingCoherent() {
  Observer observer;
  ui::EditorController controller(callbacksFor(observer));
  std::string error;
  if(!expect(controller.open(fixture("r09_shared_mesh.gltf"), &error),
             "Inspector retry fixture opens") ||
     controller.projection()->primitiveOccurrences.empty())
    return false;
  const auto occurrence = controller.projection()->primitiveOccurrences.front().key;
  const auto original = controller.viewportNodeTransform(occurrence.node);
  if(!original) return expect(false, "Inspector retry node has a transform");
  const auto edit = [&](std::uint64_t sequence, ui::EditPhase phase,
                        std::array<double, 3> value) {
    return controller.dispatch({sequence, ui::CommandSource::inspector,
        ui::PropertyEdit{occurrence.node.untyped(), ui::FieldId::translation,
                         phase, value}});
  };
  bool passed = expect(edit(1u, ui::EditPhase::begin, original->translation).accepted,
                       "Inspector gesture begins");
  passed &= expect(!edit(2u, ui::EditPhase::begin, original->translation).accepted,
                   "overlapping Inspector begin is rejected");
  auto value = original->translation;
  value[0] += 0.25;
  passed &= expect(edit(3u, ui::EditPhase::preview, value).accepted,
                   "valid preview after rejected begin retains its capture");
  auto invalid = value;
  invalid[0] = std::numeric_limits<double>::quiet_NaN();
  passed &= expect(!edit(4u, ui::EditPhase::preview, invalid).accepted &&
                       controller.viewportNodeTransform(occurrence.node)->translation == value,
                   "rejected Inspector value keeps the last valid transform");
  value[0] += 0.25;
  passed &= expect(edit(5u, ui::EditPhase::preview, value).accepted &&
                       edit(6u, ui::EditPhase::commit, value).accepted &&
                       !controller.session()->editor().continuousEdit().active() &&
                       controller.session()->editor().history().undoCount() == 1u,
                   "retry commits the original gesture exactly once");
  passed &= expect(controller.projection()->documentRevision ==
                       controller.session()->document().revision() &&
                       controller.projection()->documentDigest ==
                       controller.session()->document().documentDigest(),
                   "document and published projection stay coherent after retry");

  class ProjectionLookup final : public ISceneSelectionOccurrenceLookup {
  public:
    explicit ProjectionLookup(const SceneProjectionResult& value) : projection(value) {}
    EditorDocumentAuthority authority() const noexcept override { return projection.mapping.authority(); }
    std::uint64_t documentRevision() const noexcept override { return projection.documentRevision; }
    std::uint64_t sceneActivationEpoch() const noexcept override { return projection.sceneActivationEpoch; }
    std::uint64_t generation() const noexcept override { return projection.mappingEpoch; }
    bool contains(PrimitiveOccurrenceKey key) const noexcept override {
      return std::any_of(projection.primitiveOccurrences.begin(), projection.primitiveOccurrences.end(),
                         [&](const auto& item) { return item.key == key; });
    }
    bool visible(PrimitiveOccurrenceKey key) const noexcept override {
      return std::any_of(projection.primitiveOccurrences.begin(), projection.primitiveOccurrences.end(),
                         [&](const auto& item) { return item.key == key && item.visible; });
    }
    bool nodeHighlightEligible(NodeId node) const noexcept override {
      return std::any_of(projection.primitiveOccurrences.begin(), projection.primitiveOccurrences.end(),
                         [&](const auto& item) { return item.key.node == node && item.visible; });
    }
    const SceneProjectionResult& projection;
  } lookup(*controller.projection());
  controller.setSelectionOccurrenceLookup(&lookup);
  const EditorSelectionEffect pick{.kind = EditorSelectionEffectKind::setPrimitive,
      .cause = SelectionCause::viewportSingle, .occurrence = occurrence,
      .expectedRevision = controller.selectionRevision()};
  passed &= expect(controller.applyPickingSelection(pick).accepted,
                   "fresh picking selection succeeds after Inspector recovery");
  passed &= expect(!controller.applyPickingSelection(pick).accepted,
                   "outdated picking revision is still rejected");
  controller.setSelectionOccurrenceLookup(nullptr);
  passed &= expect(dispatchHistory(controller, ui::CommandId::undo).accepted &&
                       controller.viewportNodeTransform(occurrence.node) == original,
                   "one Undo restores the transform before the complete gesture");
  return passed;
}

bool testInspectorMouseActivationWithoutEditClosesSession() {
  struct ImGuiScope {
    ImGuiScope() {
      ImGui::CreateContext();
      auto& io = ImGui::GetIO();
      io.IniFilename = nullptr;
      io.LogFilename = nullptr;
      io.DisplaySize = ImVec2(1280, 720);
      io.DeltaTime = 1.0F / 60.0F;
      unsigned char* pixels = nullptr;
      int width = 0, height = 0;
      io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }
    ~ImGuiScope() { ImGui::DestroyContext(); }
  } scope;
  struct InspectorProbe final : ui::EditorUiProbe {
    std::optional<ui::EditorUiProbeRect> x;
    std::optional<ui::EditorUiProbeRect> y;
    bool dispatchesAccepted{true};
    void beginFrame(const ui::EditorUiProbeFrameBinding&) override {}
    void observeItem(const ui::EditorUiProbeItem& item) override {
      if(!item.valid() || !item.visible || !item.enabled) return;
      if(item.key.semantic == ui::EditorUiProbeSemantic::inspectorTranslationX) x = item.rect;
      if(item.key.semantic == ui::EditorUiProbeSemantic::inspectorTranslationY) y = item.rect;
    }
    void observeDispatch(const ui::UiCommand&, const ui::RuntimeActionResult& result) override {
      dispatchesAccepted &= result.accepted;
    }
    void endFrame() override {}
  } probe;
  ui::EditorController controller;
  std::string error;
  if(!expect(controller.open(fixture("r09_shared_mesh.gltf"), &error),
             "Inspector mouse fixture opens")) return false;
  const auto occurrence = controller.projection()->primitiveOccurrences.front().key;
  const EditorSelectionSnapshot selected{controller.session()->document().documentAuthority(),
      NodeSelection{occurrence.scene, occurrence.node, std::nullopt}};
  if(!controller.applyViewportSelection(selected, false).accepted) return false;
  const auto before = controller.viewportNodeTransform(occurrence.node);
  const auto frame = [&](ImVec2 pointer, bool down) {
    auto& io = ImGui::GetIO();
    io.AddMousePosEvent(pointer.x, pointer.y);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
    ImGui::NewFrame();
    (void)controller.render({}, &probe);
    ImGui::Render();
  };
  frame(ImVec2(-1000, -1000), false);
  frame(ImVec2(-1000, -1000), false);
  if(!expect(probe.x && probe.y, "real Inspector translation controls are visible")) return false;
  const auto center = [](const ui::EditorUiProbeRect& rect) {
    return ImVec2((rect.minimumX + rect.maximumX) * 0.5F,
                  (rect.minimumY + rect.maximumY) * 0.5F);
  };
  const auto x = center(*probe.x);
  const auto y = center(*probe.y);
  frame(x, false);
  frame(x, true);
  bool passed = expect(controller.session()->editor().continuousEdit().active(),
                       "real mouse activation opens an Inspector gesture");
  frame(x, false);
  passed &= expect(!controller.session()->editor().continuousEdit().active() &&
                       controller.session()->editor().history().undoCount() == 0u &&
                       controller.viewportNodeTransform(occurrence.node) == before,
                   "release without a value change closes the gesture without history");
  frame(y, true);
  const ImVec2 moved(y.x + 20, y.y);
  frame(moved, true);
  frame(moved, false);
  passed &= expect(probe.dispatchesAccepted &&
                       !controller.session()->editor().continuousEdit().active() &&
                       controller.session()->editor().history().undoCount() == 1u &&
                       controller.viewportNodeTransform(occurrence.node) != before,
                   "next Inspector drag previews and commits normally");
  frame(x, true);
  passed &= expect(controller.applyViewportSelection(EditorSelectionSnapshot::empty(), false).accepted,
                   "selection can change away from the active Inspector field");
  frame(x, false);
  passed &= expect(!controller.session()->editor().continuousEdit().active() &&
                       controller.session()->editor().history().undoCount() == 1u &&
                       controller.beginGizmoTransform(occurrence.node).accepted,
                   "selection change closes the old field and allows a new Gizmo gesture");
  (void)controller.finishGizmoTransform(occurrence.node, true, false, false);
  return passed;
}

bool testBaseColorProjectionMatchesFullAndRejectsUnprovenChanges() {
  std::optional<SceneCanonicalizationResult> published;
  ui::EditorController::Callbacks callbacks;
  callbacks.onPublication = [&](const auto& publication) {
    if(publication.canonical) published = *publication.canonical;
  };
  ui::EditorController controller(std::move(callbacks));
  if(!controller.open(fixture("r09_shared_mesh.gltf"))) return false;
  const SceneProjectionResult before = *controller.projection();
  const auto material = MaterialId::from(
      controller.session()->document().idAt(EditorObjectKind::material, 0));
  if(!material) return false;
  const auto edit = [&](ui::EditPhase phase, std::array<double, 4> color) {
    return controller.dispatch({0u, ui::CommandSource::inspector,
        ui::PropertyEdit{material->untyped(), ui::FieldId::baseColor, phase, color}});
  };
  if(!edit(ui::EditPhase::begin, *controller.session()->document().materialBaseColor(*material)).accepted ||
     !edit(ui::EditPhase::preview, {0.2, 0.4, 0.6, 0.8}).accepted ||
     !published || !published->changeSet) return false;
  SceneMutationEvidence evidence;
  evidence.sourceMutation = published->changeSet->header.sourceMutations.front();
  evidence.exactChanges = published->changeSet->canonicalChanges;
  SceneProjectionOptions options;
  options.baseMappingEpoch = before.mappingEpoch;
  const auto fast = detail::reprojectMaterialParameters(
      controller.session()->document(), before, evidence, options);
  if(!expect(fast.has_value(), "Base Color change can reuse existing decoded payloads")) return false;
  const auto full = SceneProjection::project(controller.session()->document(), options);
  bool passed = equivalentProjection(*fast, full, "Base Color incremental projection");
  passed &= expect(fast->immutableSnapshot.bytes == full.immutableSnapshot.bytes &&
                       controller.projection()->projectionDigest == full.projectionDigest,
                   "incremental and full projections have identical canonical bytes and digest");

  auto forged = evidence;
  std::get<SceneMaterialParametersValue>(*forged.exactChanges.front().afterValue).baseColorFactor[0] += 0.1;
  passed &= expect(!detail::reprojectMaterialParameters(controller.session()->document(), before, forged, options),
                   "Base Color evidence must match the live document value");
  forged = evidence;
  std::get<SceneMaterialParametersValue>(*forged.exactChanges.front().afterValue).roughnessFactor -= 0.1;
  passed &= expect(!detail::reprojectMaterialParameters(controller.session()->document(), before, forged, options),
                   "forged Roughness evidence cannot claim a material edit");
  return passed;
}

bool testMaterialDraftProjectionMatchesFull(bool profileSponza = false) {
  std::optional<SceneCanonicalizationResult> published;
  ui::EditorController::Callbacks callbacks;
  callbacks.onPublication = [&](const auto& publication) {
    if (publication.canonical) published = *publication.canonical;
  };
  ui::EditorController controller(std::move(callbacks));
  const auto source = profileSponza
      ? std::filesystem::path(MGIF_TEST_SOURCE_DIR) /
            "resources/GLTF_Sponza/sponza.gltf"
      : fixture("r09_shared_mesh.gltf");
  if (!controller.open(source)) return false;
  const auto material = MaterialId::from(
      controller.session()->document().idAt(EditorObjectKind::material, 0u));
  if (!material || !controller.projection()) return false;

  const auto checkDraft = [&](ui::MaterialEditDraft draft,
                              std::string_view label) {
    const SceneProjectionResult before = *controller.projection();
    published.reset();
    const auto startedAt = std::chrono::steady_clock::now();
    const auto result = controller.commitMaterialDraft(draft);
    if (profileSponza)
      std::cout << "Material draft commit_ms="
                << std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - startedAt).count()
                << std::endl;
    if (!expect(result.accepted && published && published->changeSet,
                std::string(label) + ": draft commits and publishes")) {
      if (!result.accepted) std::cerr << "draft error: " << result.error << '\n';
      return false;
    }
    const auto& changes = *published->changeSet;
    if (!expect(changes.header.sourceMutations.size() == 1u,
                std::string(label) + ": one source mutation")) return false;
    SceneMutationEvidence evidence;
    evidence.sourceMutation = changes.header.sourceMutations.front();
    evidence.exactChanges = changes.canonicalChanges;
    SceneProjectionOptions options;
    options.baseMappingEpoch = before.mappingEpoch;
    const auto incrementalStartedAt = std::chrono::steady_clock::now();
    const auto incremental = detail::reprojectMaterialParameters(
        controller.session()->document(), before, evidence, options);
    if (profileSponza)
      std::cout << "Material draft reproject_ms="
                << std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - incrementalStartedAt).count()
                << " snapshot_bytes=" << before.immutableSnapshot.bytes.size()
                << std::endl;
    if (!expect(incremental.has_value(),
                std::string(label) + ": incremental projection is eligible"))
      return false;
    const auto full = SceneProjection::project(controller.session()->document(), options);
    bool passed = equivalentProjection(*incremental, full, label);
    passed &= equivalentProjection(*controller.projection(), full,
                                   std::string(label) + " live publication");
    const auto canonicalText = detail::GltfDocumentAccess::canonicalText(
        controller.session()->document());
    const auto canonicalSpan = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(canonicalText.data()), canonicalText.size());
    passed &= expect(!canonicalText.empty() &&
                         controller.session()->document().documentDigest() ==
                             SceneChangeCanonicalizer::computeSha256(canonicalSpan),
                     std::string(label) + ": cached digest matches complete authoritative canonical text");
    const auto comparison = GltfSemanticCompare::compare(
        controller.session()->document(), controller.session()->document());
    passed &= expect(comparison.equivalent() && incremental->semanticHash == comparison.leftDigest(),
                     std::string(label) + ": cached semantic hash matches uncached canonical text");
    const auto snapshotChunks = incremental->immutableSnapshot.bytes.chunks();
    passed &= expect(incremental->asset.vertexPayload.data() == before.asset.vertexPayload.data() &&
                         incremental->asset.indexPayload.data() == before.asset.indexPayload.data() &&
                         incremental->asset.texturePayload.data() == before.asset.texturePayload.data() &&
                         std::any_of(snapshotChunks.begin(), snapshotChunks.end(), [&](const auto chunk) {
                           return chunk.data() == incremental->asset.vertexPayload.data();
                         }),
                     std::string(label) + ": projection and canonical snapshot share unchanged payloads");
    passed &= expect(SceneChangeCanonicalizer::computeSha256(incremental->immutableSnapshot.bytes) ==
                         incremental->projectionDigest,
                     std::string(label) + ": fragmented projection hashes its exact contiguous bytes");

    auto forged = evidence;
    forged.exactChanges.front().target.slot += 1u;
    passed &= expect(!detail::reprojectMaterialParameters(
                         controller.session()->document(), before, forged, options),
                     std::string(label) + ": foreign material evidence rejected");
    for (std::size_t i = 0u; i < evidence.exactChanges.size(); ++i) {
      if (!std::holds_alternative<SceneTextureBindingValue>(
              *evidence.exactChanges[i].afterValue)) continue;
      forged = evidence;
      ++std::get<SceneTextureBindingValue>(
          *forged.exactChanges[i].beforeValue).uvSet;
      ++std::get<SceneTextureBindingValue>(
          *forged.exactChanges[i].afterValue).uvSet;
      passed &= expect(!detail::reprojectMaterialParameters(
                           controller.session()->document(), before, forged,
                           options),
                       std::string(label) + ": forged UV binding rejected");
      const auto* original = std::get_if<SceneTextureBindingValue>(
          &*evidence.exactChanges[i].beforeValue);
      if (original && original->texture) {
        SceneProjectionResult corrupted = before;
        const auto binding = std::find_if(
            corrupted.textureBindings.begin(), corrupted.textureBindings.end(),
            [&](const SceneTextureProjectionBinding& entry) {
              return entry.textureId == *original->texture;
            });
        if (binding != corrupted.textureBindings.end()) {
          binding->samplerId = binding->textureId;
          passed &= expect(!detail::reprojectMaterialParameters(
                               controller.session()->document(), corrupted,
                               evidence, options),
                           std::string(label) + ": stale sampler binding rejected");
          binding->samplerId = original->sampler;
          ++binding->linearSceneAssetIndex;
          passed &= expect(!detail::reprojectMaterialParameters(
                               controller.session()->document(), corrupted,
                               evidence, options),
                           std::string(label) + ": stale image binding rejected");
        }
      }
      break;
    }
    return passed;
  };

  const EditorObjectId target = material->untyped();
  bool passed = checkDraft(ui::MaterialEditDraft{target, {
      {target, ui::FieldId::metallic, ui::EditPhase::commit, 0.35},
      {target, ui::FieldId::roughness, ui::EditPhase::commit, 0.45},
      {target, ui::FieldId::emissiveColor, ui::EditPhase::commit,
       std::array<double, 3>{0.3, 0.2, 0.1}},
      {target, ui::FieldId::normalScale, ui::EditPhase::commit, 0.7},
      {target, ui::FieldId::occlusionStrength, ui::EditPhase::commit, 0.8},
  }}, "combined material draft");
  if (!passed) return false;

  passed &= checkDraft(ui::MaterialEditDraft{target, {
      {target, ui::FieldId::normalScale, ui::EditPhase::commit, 0.6},
  }}, "normal-only draft");
  passed &= checkDraft(ui::MaterialEditDraft{target, {
      {target, ui::FieldId::occlusionStrength, ui::EditPhase::commit, 0.5},
  }}, "occlusion-only draft");
  for (const auto command : {ui::CommandId::undo, ui::CommandId::redo}) {
    const auto beforeEpoch = controller.projection()->mappingEpoch;
    const auto startedAt = std::chrono::steady_clock::now();
    const auto result = controller.dispatch({0u, ui::CommandSource::menu, ui::InvokeCommand{command}});
    if (profileSponza)
      std::cout << "Material draft " << (command == ui::CommandId::undo ? "undo_ms=" : "redo_ms=")
                << std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - startedAt).count() << std::endl;
    SceneProjectionOptions options;
    options.baseMappingEpoch = beforeEpoch;
    passed &= expect(result.accepted, "scalar-only material history restores correctly");
    const auto full = SceneProjection::project(controller.session()->document(), options);
    passed &= equivalentProjection(*controller.projection(), full, "material draft history");
  }
  if (profileSponza) return passed;
  // Alpha mode changes pipeline state and remain on the full path. Once a
  // material is masked, changing only the cutoff must also update meshes.
  const auto mode = controller.dispatch({0u, ui::CommandSource::inspector,
      ui::PropertyEdit{target, ui::FieldId::alphaMode, ui::EditPhase::commit,
                       std::int32_t{1}}});
  passed &= expect(mode.accepted, "masked material mode is accepted");
  if (mode.accepted)
    passed &= checkDraft(ui::MaterialEditDraft{target, {
        {target, ui::FieldId::alphaCutoff, ui::EditPhase::commit, 0.32},
    }}, "masked cutoff draft");
  return passed;
}

bool testBaseColorPickerChangesMaterialWithPendingPublication(bool profileSponza = false) {
  struct ImGuiScope {
    ImGuiScope() {
      ImGui::CreateContext();
      auto& io = ImGui::GetIO();
      io.IniFilename = nullptr;
      io.LogFilename = nullptr;
      io.DisplaySize = ImVec2(1280, 720);
      io.DeltaTime = 1.0F / 60.0F;
      unsigned char* pixels = nullptr;
      int width = 0, height = 0;
      io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
      ImGui::SetColorEditOptions(ImGuiColorEditFlags_Float |
          ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_PickerHueBar);
    }
    ~ImGuiScope() { ImGui::DestroyContext(); }
  } scope;
  struct ColorProbe final : ui::EditorUiProbe {
    std::optional<ui::EditorUiProbeRect> swatch;
    bool accepted{true};
    void beginFrame(const ui::EditorUiProbeFrameBinding&) override {}
    void observeItem(const ui::EditorUiProbeItem& item) override {
      if(item.key.semantic == ui::EditorUiProbeSemantic::inspectorBaseColorSwatch &&
         item.valid() && item.enabled && item.visible)
        swatch = item.rect;
    }
    void observeDispatch(const ui::UiCommand& command,
                         const ui::RuntimeActionResult& result) override {
      accepted &= result.accepted;
      if(!result.accepted) {
        const auto* property = std::get_if<ui::PropertyEdit>(&command.payload);
        std::cerr << "FAILED: color dispatch phase="
                  << (property ? static_cast<int>(property->phase) : -1)
                  << " error=" << result.error << '\n';
      }
    }
    void endFrame() override {}
  } probe;
  bool rendererBusy = false;
  std::size_t publications = 0;
  ui::EditorController::Callbacks callbacks;
  callbacks.rendererBusy = [&] { return rendererBusy; };
  callbacks.onPublication = [&](const auto&) {
    rendererBusy = true;
    ++publications;
  };
  ui::EditorController controller(std::move(callbacks));
  std::string error;
  const auto openedAt = std::chrono::steady_clock::now();
  const auto source = profileSponza
      ? std::filesystem::path(MGIF_TEST_SOURCE_DIR) / "resources/GLTF_Sponza/sponza.gltf"
      : fixture("r09_shared_mesh.gltf");
  if(!expect(controller.open(source, &error),
             "Base Color picker fixture opens")) return false;
  if(profileSponza)
    std::cout << "Color profile open_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - openedAt).count() << std::endl;
  rendererBusy = false;
  const auto key = controller.projection()->primitiveOccurrences.front().key;
  const auto address = controller.session()->document().primitiveAddress(key.primitive);
  const auto mesh = MeshId::from(controller.session()->document().idAt(
      EditorObjectKind::mesh, address->meshIndex));
  const EditorSelectionSnapshot selection{controller.session()->document().documentAuthority(),
      PrimitiveSelection{key.scene, key.node, key.primitive, *mesh}};
  if(!controller.dispatch({0u, ui::CommandSource::sceneBrowser,
                          ui::SelectObject{selection}}).accepted) return false;
  const auto material = controller.session()->editor().commands().primitiveMaterial(key.primitive);
  if(!material) return false;
  const auto color = [&] {
    const auto index = controller.projection()->mapping.sceneAssetIndex(material->untyped());
    if(!index || *index >= controller.projection()->asset.materials.size())
      return std::array<double, 4>{};
    const auto &value = controller.projection()->asset.materials[*index].baseColorFactor;
    return std::array<double, 4>{value.x, value.y, value.z, value.w};
  };
  const auto original = color();
  std::array<double, 4> firstCommitted{};
  std::array<double, 4> lastCommitted{};
  std::size_t cycle = 0;
  const auto frame = [&](ImVec2 point, bool down) {
    const auto startedAt = std::chrono::steady_clock::now();
    // The application flushes last frame's publications before drawing UI.
    rendererBusy = false;
    auto& io = ImGui::GetIO();
    io.AddMousePosEvent(point.x, point.y);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
    ImGui::NewFrame();
    (void)controller.render({}, &probe);
    ImGui::Render();
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - startedAt).count();
  };
  frame(ImVec2(-1000, -1000), false);
  frame(ImVec2(-1000, -1000), false);
  bool passed = true;
  for (; cycle < 2u; ++cycle) {
    if(!expect(probe.swatch.has_value(),
               "selected primitive exposes its Base Color swatch")) return false;
    const ImVec2 swatch((probe.swatch->minimumX + probe.swatch->maximumX) * 0.5F,
                       (probe.swatch->minimumY + probe.swatch->maximumY) * 0.5F);
    const auto before = color();
    const auto revisionBefore = controller.session()->document().revision();
    const auto publicationsBefore = publications;
    const auto historyBefore = controller.session()->editor().history().undoCount();
    frame(swatch, false);
    frame(swatch, true);
    frame(swatch, false);
    frame(swatch, false);
    if(!expect(!GImGui->OpenPopupStack.empty() && GImGui->OpenPopupStack.back().Window,
               "Base Color picker opens on both consecutive editing sessions")) return false;
    auto* popup = GImGui->OpenPopupStack.back().Window;
    const ImVec2 origin = popup->DC.CursorStartPos;
    double maximumDragMs = 0.0;
    std::optional<SceneMaterialColorPreview> finalPreview;
    for (int drag = 0; drag < 3; ++drag) {
      const ImVec2 initial(origin.x + 30.0F + 15.0F * drag,
                           origin.y + 65.0F + 10.0F * static_cast<float>(cycle));
      maximumDragMs = std::max(maximumDragMs, frame(initial, false));
      maximumDragMs = std::max(maximumDragMs, frame(initial, true));
      const auto firstPreview = controller.materialColorPreview();
      ImVec2 moved = initial;
      for (int move = 1; move <= 6; ++move) {
        moved = ImVec2(initial.x + 8.0F * move, initial.y + 4.0F * move);
        maximumDragMs = std::max(maximumDragMs, frame(moved, true));
      }
      finalPreview = controller.materialColorPreview();
      maximumDragMs = std::max(maximumDragMs, frame(moved, false));
      maximumDragMs = std::max(maximumDragMs, frame(moved, false));
      passed &= expect(firstPreview && finalPreview &&
                           firstPreview->baseColor != finalPreview->baseColor &&
                           controller.materialColorPreview() == finalPreview &&
                           finalPreview->material == *material &&
                           finalPreview->documentRevision == revisionBefore,
                       "each drag immediately changes the viewport preview and retains it on release");
      passed &= expect(!GImGui->OpenPopupStack.empty() &&
                           publications == publicationsBefore &&
                           controller.session()->document().revision() == revisionBefore &&
                           controller.session()->editor().history().undoCount() == historyBefore &&
                           color() == before,
                       "repeated drags and releases inside the open picker never commit");
    }
    // Click well outside the popup, rather than on a swatch the popup may cover.
    const ImVec2 outside(640.0F, 680.0F);
    double closeMs = frame(outside, false);
    closeMs = std::max(closeMs, frame(outside, true));
    closeMs = std::max(closeMs, frame(outside, false));
    closeMs = std::max(closeMs, frame(outside, false));
    const auto after = color();
    bool finalColorMatches = finalPreview.has_value();
    for (std::size_t component = 0; component < 4u && finalColorMatches; ++component)
      finalColorMatches = after[component] == finalPreview->baseColor[component];
    passed &= expect(probe.accepted && finalColorMatches && after != before &&
                         GImGui->OpenPopupStack.empty() &&
                         !controller.materialColorPreview() &&
                         publications == publicationsBefore + 1u &&
                         controller.session()->document().revision() == revisionBefore + 1u &&
                         controller.projection()->documentRevision == revisionBefore + 1u &&
                         controller.session()->editor().history().undoCount() == historyBefore + 1u,
                     "closing each picker commits exactly its final color once");
    if (cycle == 0u) firstCommitted = after;
    lastCommitted = after;
    if(profileSponza)
      std::cout << "Color profile session=" << cycle + 1u
                << " drags=3 max_drag_ms=" << maximumDragMs
                << " close_ms=" << closeMs
                << " publications=" << publications - publicationsBefore
                << " undo_count=" << controller.session()->editor().history().undoCount()
                << std::endl;
  }
  rendererBusy = false;
  passed &= expect(dispatchHistory(controller, ui::CommandId::undo).accepted &&
                       color() == firstCommitted &&
                       controller.session()->editor().history().undoCount() == 1u,
                   "Undo of the second session restores the first committed color");
  rendererBusy = false;
  passed &= expect(dispatchHistory(controller, ui::CommandId::redo).accepted &&
                       color() == lastCommitted && color() != original &&
                       controller.session()->editor().history().undoCount() == 2u,
                   "Redo restores the second session after consecutive picker edits");
  return passed;
}

bool testMaterialFieldSwitchDoesNotCommitPreviousField(bool profileSponza = false) {
  struct ImGuiScope {
    ImGuiScope() {
      ImGui::CreateContext();
      ImGui::GetIO().IniFilename = nullptr;
      ImGui::GetIO().DisplaySize = ImVec2(1280, 720);
      unsigned char *pixels = nullptr; int width = 0; int height = 0;
      ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }
    ~ImGuiScope() { ImGui::DestroyContext(); }
  } scope;
  struct Probe final : ui::EditorUiProbe {
    std::optional<ui::EditorUiProbeRect> metallic;
    std::optional<ui::EditorUiProbeRect> roughness;
    std::size_t publications{0};
    void beginFrame(const ui::EditorUiProbeFrameBinding&) override {}
    void observeItem(const ui::EditorUiProbeItem& item) override {
      if (!item.valid()) return;
      if (item.key.semantic == ui::EditorUiProbeSemantic::inspectorMetallic) metallic = item.rect;
      if (item.key.semantic == ui::EditorUiProbeSemantic::inspectorRoughness) roughness = item.rect;
    }
    void observeDispatch(const ui::UiCommand&, const ui::RuntimeActionResult&) override {}
    void endFrame() override {}
  } probe;
  std::size_t publications = 0;
  std::optional<SceneCanonicalizationResult> published;
  ui::EditorController::Callbacks callbacks;
  callbacks.onPublication = [&](const auto& publication) {
    ++publications;
    if (publication.canonical) published = *publication.canonical;
  };
  ui::EditorController controller(std::move(callbacks));
  std::string error;
  const auto source = profileSponza
      ? std::filesystem::path(MGIF_TEST_SOURCE_DIR) / "resources/GLTF_Sponza/sponza.gltf"
      : fixture("r09_shared_mesh.gltf");
  if (!controller.open(source, &error)) return false;
  const auto key = controller.projection()->primitiveOccurrences.front().key;
  const auto address = controller.session()->document().primitiveAddress(key.primitive);
  const auto mesh = MeshId::from(controller.session()->document().idAt(EditorObjectKind::mesh, address->meshIndex));
  if (!mesh) return false;
  const EditorSelectionSnapshot selection{controller.session()->document().documentAuthority(), PrimitiveSelection{key.scene, key.node, key.primitive, *mesh}};
  if (!controller.dispatch({0u, ui::CommandSource::sceneBrowser, ui::SelectObject{selection}}).accepted) return false;
  auto frame = [&](ImVec2 point, bool down) {
    const auto startedAt = std::chrono::steady_clock::now();
    auto& io = ImGui::GetIO();
    io.AddMousePosEvent(point.x, point.y);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
    ImGui::NewFrame(); (void)controller.render({}, &probe); ImGui::Render();
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - startedAt).count();
  };
  frame(ImVec2(-100, -100), false); frame(ImVec2(-100, -100), false);
  if (!probe.metallic || !probe.roughness) return false;
  const auto center = [](const ui::EditorUiProbeRect& r) { return ImVec2((r.minimumX+r.maximumX)*0.5f, (r.minimumY+r.maximumY)*0.5f); };
  const auto metallic = center(*probe.metallic); const auto roughness = center(*probe.roughness);
  const auto revision = controller.session()->document().revision();
  const SceneProjectionResult before = *controller.projection();
  const auto beforePublications = publications;
  published.reset();
  frame(metallic, true); frame(ImVec2(metallic.x - 40, metallic.y), true);
  frame(metallic, false);
  const auto switchMs = frame(roughness, true);
  bool passed = expect(controller.session()->document().revision() == revision && publications == beforePublications,
                       "switching from metallic to roughness keeps one material draft without committing metallic");
  const auto roughnessMs = frame(ImVec2(roughness.x - 30, roughness.y), true);
  frame(roughness, false);
  double leaveMs = 0.0;
  for (std::size_t i = 0u; i < 4u; ++i)
    leaveMs = std::max(leaveMs, frame(ImVec2(-100, -100), false));
  if (profileSponza)
    std::cout << "Material switch switch_ms=" << switchMs << " second_drag_ms=" << roughnessMs
              << " leave_ms=" << leaveMs << std::endl;
  passed &= expect(controller.session()->document().revision() == revision + 1u &&
                       publications == beforePublications + 1u &&
                       published && published->changeSet,
                   "leaving material controls commits the combined draft once");
  if (published && published->changeSet) {
    const auto& changes = *published->changeSet;
    SceneMutationEvidence evidence;
    evidence.sourceMutation = changes.header.sourceMutations.front();
    evidence.exactChanges = changes.canonicalChanges;
    SceneProjectionOptions options;
    options.baseMappingEpoch = before.mappingEpoch;
    const auto incremental = detail::reprojectMaterialParameters(
        controller.session()->document(), before, evidence, options);
    passed &= expect(incremental.has_value(),
                     "field-switch draft avoids full glTF image decode");
    if (incremental) {
      const auto full = SceneProjection::project(
          controller.session()->document(), options);
      passed &= equivalentProjection(*controller.projection(), full,
                                     "field-switch published projection");
      passed &= equivalentProjection(*incremental, full,
                                     "field-switch incremental projection");
    }
  }
  return passed;
}

bool testRejectedNativePreviewRetainsEscapeOwnership() {
  Observer observer;
  ui::EditorController controller(callbacksFor(observer));
  std::string error;
  bool passed = expect(controller.open(fixture("minimal_scene.gltf"), &error),
                       "rejected native preview fixture opens");
  if(!controller.session() || !controller.projection())
    return false;
  auto& session = *controller.session();
  const auto scene = SceneId::from(
      session.document().idAt(EditorObjectKind::scene, 0u));
  const auto node = NodeId::from(
      session.document().idAt(EditorObjectKind::node, 0u));
  if(!scene || !node)
    return expect(false, "rejected native preview scene and node are stable");
  const EditorSelectionSnapshot selection{
      session.document().documentAuthority(),
      NodeSelection{*scene, *node, std::nullopt}};
  passed &= expect(controller.dispatch(
                       {1u, ui::CommandSource::sceneBrowser,
                        ui::SelectObject{selection}}).accepted,
                   "rejected native preview begins with a selected node");
  const auto before = controller.viewportNodeTransform(*node);
  if(!before)
    return expect(false, "rejected native preview baseline TRS is available");
  NodeTransform preview = *before;
  preview.translation[1] += 0.5;
  passed &= expect(controller.beginGizmoTransform(*node).accepted &&
                       controller.previewGizmoTransform(*node, preview).accepted,
                   "native gesture publishes a valid preview before rejection");

  for(const double invalidValue : {
          std::numeric_limits<double>::quiet_NaN(),
          std::numeric_limits<double>::infinity()}) {
    const auto revision = session.document().revision();
    const auto digest = session.document().documentDigest();
    const auto projectionDigest = controller.projection()->projectionDigest;
    const auto publications = observer.publications;
    NodeTransform invalid = preview;
    invalid.translation[1] = invalidValue;
    const auto rejected = controller.previewGizmoTransform(*node, invalid);
    passed &= expect(!rejected.accepted &&
                         session.editor().continuousEdit().active() &&
                         controller.buildFrameModel().viewportOwnsEscape &&
                         controller.viewportNodeTransform(*node) == preview &&
                         session.document().revision() == revision &&
                         session.document().documentDigest() == digest &&
                         controller.projection()->projectionDigest == projectionDigest &&
                         observer.publications == publications &&
                         session.editor().history().undoCount() == 0u &&
                         session.editor().history().redoCount() == 0u,
                     "rejected native value keeps the active owner and last published preview without mutation");
    preview.translation[1] += 0.25;
    passed &= expect(controller.previewGizmoTransform(*node, preview).accepted &&
                         controller.viewportNodeTransform(*node) == preview &&
                         controller.buildFrameModel().viewportOwnsEscape,
                     "valid preview retry retains the original publication baseline");
  }

  NodeTransform lastRejected = preview;
  lastRejected.scale[0] = std::numeric_limits<double>::quiet_NaN();
  passed &= expect(!controller.previewGizmoTransform(*node, lastRejected).accepted &&
                       session.editor().continuousEdit().active() &&
                       controller.buildFrameModel().viewportOwnsEscape,
                   "Escape remains viewport-owned immediately after the last rejected preview");
  passed &= expect(controller.finishGizmoTransform(*node, false, true, false).accepted &&
                       !session.editor().continuousEdit().active() &&
                       !controller.buildFrameModel().viewportOwnsEscape &&
                       controller.buildFrameModel().selection.emptyValue() &&
                       controller.viewportNodeTransform(*node) == preview &&
                       session.editor().history().undoCount() == 0u &&
                       session.editor().history().redoCount() == 0u,
                   "Escape after rejected native previews detaches once and retains valid TRS without history");
  passed &= expect(controller.finishGizmoTransform(*node, true, false, false).accepted &&
                       !controller.buildFrameModel().viewportOwnsEscape &&
                       controller.viewportNodeTransform(*node) == preview &&
                       session.editor().history().undoCount() == 0u,
                   "late release after rejected-preview Escape cannot reacquire ownership or commit");
  return passed;
}

bool testNativeGizmoFinishSequenceExhaustion() {
  Observer observer;
  ui::EditorController controller(callbacksFor(observer));
  std::string error;
  bool passed = expect(controller.open(fixture("minimal_scene.gltf"), &error),
                       "native sequence exhaustion fixture opens");
  if(!controller.session())
    return false;
  const auto node = NodeId::from(controller.session()->document().idAt(EditorObjectKind::node, 0u));
  if(!node)
    return expect(false, "native sequence exhaustion node is stable");
  passed &= expect(controller.beginGizmoTransform(*node).accepted,
                   "native sequence exhaustion gesture begins");
  ui::EditorControllerTestAccess::setGizmoFinishSequence(
      controller, std::numeric_limits<std::uint64_t>::max());
  const SceneDigest before = controller.session()->document().documentDigest();
  const auto exhausted = controller.finishGizmoTransform(*node, true, false, false);
  passed &= expect(!exhausted.accepted && exhausted.error == "Gizmo terminal sequence is exhausted" &&
                       ui::EditorControllerTestAccess::gizmoFinishSequence(controller) ==
                           std::numeric_limits<std::uint64_t>::max() &&
                       controller.session()->document().documentDigest() == before &&
                       controller.session()->editor().history().undoCount() == 0u,
                   "exhausted native terminal ordinal fails closed without wrapping");
  return passed;
}

bool testControllerTransaction() {
  const auto sourcePath = fixture("feature_preservation.gltf");
  const auto sourceBytes = readBytes(sourcePath);
  TemporaryDirectory temporary;
  bool passed = expect(!temporary.path().empty(),
                       "temporary transaction directory is available");
  if(!passed)
    return false;

  Observer observer;
  ui::EditorController controller(callbacksFor(observer));
  std::string error;
  passed &= expect(controller.open(sourcePath, &error),
                   "fixture opens through the production controller");
  passed &= expect(error.empty() && controller.session() != nullptr &&
                       controller.projection() != nullptr,
                   "controller exposes a session and immutable projection");
  if(!controller.session() || !controller.projection())
    return false;

  const auto mapped = controller.projection()->mapping.documentIdAt(
      EditorObjectKind::node, 0u);
  passed &= expect(mapped.has_value(), "projection exposes a stable node");
  if(!mapped)
    return false;
  const std::string originalName = std::string(
      controller.session()->document().objectName(*mapped));
  const auto nodeIndex = controller.session()->document().indexOf(*mapped);
  passed &= expect(nodeIndex.has_value(),
                   "stable projected node has a document index");
  if(!nodeIndex)
    return false;
  const std::string mutatedName =
      originalName == "MGIF_P5_EDITOR_TRANSACTION"
          ? "MGIF_P5_EDITOR_TRANSACTION_1"
          : "MGIF_P5_EDITOR_TRANSACTION";
  const SceneDigest baselineDigest = controller.projection()->documentDigest;
  const SceneProjectionResult baselineProjection = *controller.projection();
  const std::uint64_t baselineRevision =
      controller.session()->document().revision();
  const std::size_t baselineUndo =
      controller.session()->editor().history().undoCount();
  passed &= expect(controller.session()->editor().history().redoCount() == 0u,
                   "fixture has no redo branch before the transaction");

  const std::size_t openPublications = observer.publications;
  const auto renamed = dispatchRename(controller, *mapped, mutatedName);
  passed &= expect(renamed.accepted &&
                       renamed.refresh == ui::ProjectionRefresh::hierarchy,
                   "real rename dispatch is accepted as a hierarchy refresh");
  passed &= expect(observer.publications == openPublications + 1u,
                   "rename emits one immutable publication");
  passed &= expect(controller.session()->document().objectName(*mapped) ==
                       mutatedName &&
                       controller.session()->document().revision() >
                           baselineRevision &&
                       controller.projection()->documentDigest != baselineDigest,
                   "rename changes the stable node and semantic digest");
  passed &= expect(controller.session()->editor().history().undoCount() ==
                       baselineUndo + 1u &&
                       controller.session()->editor().history().redoCount() == 0u,
                   "rename advances relative history counts");
  passed &= expect(!ui::EditorControllerTestAccess::nodeNameFastPathUsed(
                         controller) &&
                       !ui::EditorControllerTestAccess::hasNodeNameTransaction(
                           controller),
                   "active controller animation forces an authored-pose fallback");
  SceneProjectionOptions renameFreshOptions;
  renameFreshOptions.baseMappingEpoch = baselineProjection.mappingEpoch;
  renameFreshOptions.activeVariant = controller.projection()->activeVariant;
  const SceneProjectionResult freshControllerRename = SceneProjection::project(
      controller.session()->document(), renameFreshOptions);
  passed &= equivalentProjection(*controller.projection(), freshControllerRename,
                                  "controller animation fallback fresh proof");
  const SceneDigest mutatedDigest = controller.projection()->documentDigest;

  const auto undone = dispatchHistory(controller, ui::CommandId::undo);
  passed &= expect(undone.accepted &&
                       undone.refresh == ui::ProjectionRefresh::properties,
                   "real undo dispatch is accepted");
  passed &= expect(observer.publications == openPublications + 2u,
                   "undo emits one immutable publication");
  passed &= expect(controller.session()->document().objectName(*mapped) ==
                       originalName &&
                       controller.projection()->documentDigest == baselineDigest,
                   "undo restores the original semantic document");
  passed &= expect(controller.session()->editor().history().undoCount() ==
                       baselineUndo &&
                       controller.session()->editor().history().redoCount() == 1u,
                   "undo exposes exactly one redo branch");

  const auto redone = dispatchHistory(controller, ui::CommandId::redo);
  passed &= expect(redone.accepted &&
                       redone.refresh == ui::ProjectionRefresh::properties,
                   "real redo dispatch is accepted");
  passed &= expect(observer.publications == openPublications + 3u,
                   "redo emits one immutable publication");
  passed &= expect(controller.session()->document().objectName(*mapped) ==
                       mutatedName &&
                       controller.projection()->documentDigest == mutatedDigest,
                   "redo reapplies the mutated semantic document");
  passed &= expect(controller.session()->editor().history().undoCount() ==
                       baselineUndo + 1u &&
                       controller.session()->editor().history().redoCount() == 0u,
                   "redo restores the relative history counts");

  const auto target = temporary.path() / "sponza-transaction.glb";
  const auto saved = dispatchSave(controller, target);
  passed &= expect(saved.accepted, "explicit-path Save As dispatch is accepted");
  std::error_code fileError;
  const auto status = std::filesystem::symlink_status(target, fileError);
  const auto bytes = fileError || !std::filesystem::is_regular_file(status)
                         ? 0u
                         : std::filesystem::file_size(target, fileError);
  passed &= expect(!fileError && std::filesystem::is_regular_file(status) &&
                       !std::filesystem::is_symlink(status) && bytes > 12u,
                   "Save As publishes a non-empty regular output file");
  auto reloaded = GltfDocumentIO::loadCandidate(target);
  passed &= expect(reloaded.succeeded() && reloaded.candidate() != nullptr,
                   "Save As output reloads through GltfDocumentIO");
  if(reloaded.candidate() != nullptr) {
    passed &= expect(GltfSemanticCompare::compare(
                         controller.session()->document(),
                         *reloaded.candidate()).equivalent(),
                     "reloaded output is semantically equivalent to live document");
    const EditorObjectId reloadedNode = reloaded.candidate()->idAt(
        EditorObjectKind::node, *nodeIndex);
    passed &= expect(reloaded.candidate()->objectName(reloadedNode) ==
                         mutatedName,
                     "reloaded output contains the redo node name");
  }
  passed &= expect(std::filesystem::equivalent(
                       controller.session()->document().sourcePath(), target,
                       fileError) && !fileError,
                   "live document source path is the explicit Save As target");
  passed &= expect(!controller.session()->document().dirty() &&
                       controller.session()->document().cleanRevision() ==
                           controller.session()->document().revision(),
                   "Save As leaves the live document clean");
  passed &= expect(controller.session()->editor().history().undoCount() ==
                       baselineUndo + 1u &&
                       controller.session()->editor().history().redoCount() == 0u,
                   "Save As preserves undo history");
  passed &= expect(readBytes(sourcePath) == sourceBytes,
                   "Save As does not modify the source fixture bytes");

  const auto staleTarget = temporary.path() / "stale.glb";
  std::error_code staleCreateError;
  std::filesystem::create_directory(staleTarget, staleCreateError);
  passed &= expect(!dispatchSave(controller, staleTarget).accepted,
                   "non-regular stale Save As target is rejected");
  passed &= expect(!staleCreateError &&
                       std::filesystem::is_directory(staleTarget),
                   "rejected stale target remains unchanged");
  passed &= expect(!dispatchRename(controller, *mapped, mutatedName).accepted,
                   "no-op rename is rejected");
  passed &= expect(!dispatchSave(controller, temporary.path() / "bad.txt")
                        .accepted,
                   "unsupported Save As target is rejected");
  passed &= expect(!dispatchSave(
                        controller,
                        temporary.path() / "missing-parent" / "bad.glb")
                        .accepted,
                   "unavailable Save As directory is rejected");
  return passed;
}

bool testHierarchyClickedNodeParentContract() {
  bool passed = true;

  {
    Observer observer;
    ui::EditorController controller(callbacksFor(observer));
    std::string error;
    passed &= expect(controller.open(fixture("minimal_scene.gltf"), &error),
                     "root Add Child fixture opens");
    if(!controller.session() || !controller.projection())
      return false;
    auto& session = *controller.session();
    const auto scene = SceneId::from(
        session.document().idAt(EditorObjectKind::scene, 0u));
    const auto root = NodeId::from(
        session.document().idAt(EditorObjectKind::node, 0u));
    if(!scene || !root)
      return expect(false, "root Add Child fixture exposes scene/root IDs");

    const SceneProjectionResult beforeProjection = *controller.projection();
    const std::uint64_t beforeRevision = session.document().revision();
    const std::size_t beforeUndo = session.editor().history().undoCount();
    const std::size_t beforePublications = observer.publications;
    const SceneDigest beforeDocumentDigest =
        controller.projection()->documentDigest;
    const SceneDigest beforeProjectionDigest =
        controller.projection()->projectionDigest;
    const ui::RuntimeActionResult result = dispatchHierarchy(
        controller,
        ui::HierarchyAction{ui::HierarchyOperation::addChildNode, *scene,
                            *root, std::nullopt});
    const std::vector<NodeId> children =
        session.document().nodeChildIds(*root);
    const ui::FrameModel frame = controller.buildFrameModel();
    const auto* selected = std::get_if<NodeSelection>(
        &frame.selection.value);
    passed &= expect(result.accepted &&
                         result.refresh == ui::ProjectionRefresh::hierarchy,
                     "root Add Child dispatch succeeds with null occurrence parent");
    passed &= expect(children.size() == 1u &&
                         session.document().hasNodeOccurrence(
                             *scene, children.front(), *root) &&
                         selected != nullptr && selected->scene == *scene &&
                         selected->node == children.front() &&
                         selected->parent == *root,
                     "root Add Child appends below the clicked root");
    passed &= expect(session.document().revision() == beforeRevision + 1u &&
                         session.editor().history().undoCount() ==
                             beforeUndo + 1u &&
                         session.editor().history().redoCount() == 0u,
                     "root Add Child advances one revision and history entry");
    passed &= expect(observer.publications == beforePublications + 1u &&
                         observer.refreshes.back() ==
                             ui::ProjectionRefresh::hierarchy &&
                         observer.documentDigests.back() ==
                             controller.projection()->documentDigest &&
                         observer.projectionDigests.back() ==
                             controller.projection()->projectionDigest &&
                         controller.projection()->documentDigest !=
                             beforeDocumentDigest &&
                         controller.projection()->projectionDigest !=
                             beforeProjectionDigest,
                     "root Add Child publishes one coherent changed projection");
    SceneProjectionOptions options;
    options.baseMappingEpoch = beforeProjection.mappingEpoch;
    options.activeVariant = controller.projection()->activeVariant;
    const SceneProjectionResult fresh =
        SceneProjection::project(session.document(), options);
    passed &= equivalentProjection(*controller.projection(), fresh,
                                    "root Add Child fresh projection");
  }

  {
    Observer observer;
    ui::EditorController controller(callbacksFor(observer));
    std::string error;
    passed &= expect(
        controller.open(fixture("multi_scene_oracle.gltf"), &error),
        "nested Add Point Light fixture opens");
    if(!controller.session() || !controller.projection())
      return false;
    auto& session = *controller.session();
    const auto scene = SceneId::from(
        session.document().idAt(EditorObjectKind::scene, 0u));
    const auto grandparent = NodeId::from(
        session.document().idAt(EditorObjectKind::node, 0u));
    const auto nested = NodeId::from(
        session.document().idAt(EditorObjectKind::node, 2u));
    if(!scene || !grandparent || !nested)
      return expect(false,
                    "nested Add Point Light fixture exposes hierarchy IDs");

    const std::vector<NodeId> grandparentChildrenBefore =
        session.document().nodeChildIds(*grandparent);
    const std::vector<NodeId> nestedChildrenBefore =
        session.document().nodeChildIds(*nested);
    const std::size_t nodeCountBefore =
        session.document().objectCount(EditorObjectKind::node);
    const std::size_t lightCountBefore =
        session.document().objectCount(EditorObjectKind::light);
    const SceneProjectionResult beforeProjection = *controller.projection();
    const std::uint64_t beforeRevision = session.document().revision();
    const std::size_t beforeUndo = session.editor().history().undoCount();
    const std::size_t beforePublications = observer.publications;
    const SceneDigest beforeDocumentDigest =
        controller.projection()->documentDigest;
    const SceneDigest beforeProjectionDigest =
        controller.projection()->projectionDigest;
    const ui::RuntimeActionResult result = dispatchHierarchy(
        controller,
        ui::HierarchyAction{ui::HierarchyOperation::addPointLight, *scene,
                            *nested, *grandparent});
    const std::vector<NodeId> nestedChildren =
        session.document().nodeChildIds(*nested);
    const auto light = LightId::from(
        session.document().idAt(EditorObjectKind::light, lightCountBefore));
    const ui::FrameModel frame = controller.buildFrameModel();
    const auto* selected = std::get_if<NodeSelection>(
        &frame.selection.value);
    passed &= expect(result.accepted &&
                         result.refresh == ui::ProjectionRefresh::hierarchy,
                     "nested Add Point Light dispatch succeeds");
    passed &= expect(
        grandparentChildrenBefore ==
                session.document().nodeChildIds(*grandparent) &&
            nestedChildren.size() == nestedChildrenBefore.size() + 1u &&
            session.document().objectCount(EditorObjectKind::node) ==
                nodeCountBefore + 1u &&
            session.document().objectCount(EditorObjectKind::light) ==
                lightCountBefore + 1u &&
            light && session.editor().commands().lightNode(*light) ==
                         nestedChildren.back() &&
            selected != nullptr && selected->node == nestedChildren.back() &&
            selected->parent == *nested,
        "nested Add Point Light appends below clicked node, not grandparent");
    passed &= expect(session.document().revision() == beforeRevision + 1u &&
                         session.editor().history().undoCount() ==
                             beforeUndo + 1u &&
                         session.editor().history().redoCount() == 0u,
                     "nested Add Point Light advances one revision and history entry");
    passed &= expect(observer.publications == beforePublications + 1u &&
                         observer.refreshes.back() ==
                             ui::ProjectionRefresh::hierarchy &&
                         observer.documentDigests.back() ==
                             controller.projection()->documentDigest &&
                         observer.projectionDigests.back() ==
                             controller.projection()->projectionDigest &&
                         controller.projection()->documentDigest !=
                             beforeDocumentDigest &&
                         controller.projection()->projectionDigest !=
                             beforeProjectionDigest,
                     "nested Add Point Light publishes one coherent changed projection");
    SceneProjectionOptions options;
    options.baseMappingEpoch = beforeProjection.mappingEpoch;
    options.activeVariant = controller.projection()->activeVariant;
    const SceneProjectionResult fresh =
        SceneProjection::project(session.document(), options);
    passed &= equivalentProjection(*controller.projection(), fresh,
                                    "nested Add Point Light fresh projection");
  }

  return passed;
}

struct LifecycleObservation final {
  ui::LifecycleState state{ui::LifecycleState::noDocument};
  ui::LifecycleOperation operation{ui::LifecycleOperation::none};
  std::optional<float> progress;
  std::uint64_t revision{0};
};

struct LifecycleEvidenceEvent final {
  std::string phase;
  ui::LifecycleState state{ui::LifecycleState::noDocument};
  ui::LifecycleOperation operation{ui::LifecycleOperation::none};
  std::string operationLabel;
  std::optional<float> progress;
  std::uint64_t lifecycleRevision{0};
  std::uint64_t documentRevision{0};
  std::uint64_t cleanRevision{0};
  bool hasDocument{false};
  bool mutationEnabled{false};
  bool errorPresent{false};
  bool dismissibleError{false};
};

struct CandidateFailureEvidence final {
  SceneDigest documentBefore{};
  SceneDigest documentAfter{};
  SceneDigest projectionBefore{};
  SceneDigest projectionAfter{};
  bool activeDocumentRetained{false};
  bool frameIdentityRetained{false};
  bool errorObserved{false};
  bool dismissRecovered{false};
};

std::string_view lifecycleOperationToken(
    ui::LifecycleOperation operation) noexcept {
  switch (operation) {
  case ui::LifecycleOperation::none:
    return "none";
  case ui::LifecycleOperation::loadCandidate:
    return "loadCandidate";
  case ui::LifecycleOperation::save:
    return "save";
  case ui::LifecycleOperation::merge:
    return "merge";
  case ui::LifecycleOperation::compact:
    return "compact";
  }
  return "invalid";
}

std::string digestHex(const SceneDigest &digest) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(digest.size() * 2u);
  for (const std::uint8_t byte : digest) {
    result.push_back(hex[(byte >> 4u) & 0x0fu]);
    result.push_back(hex[byte & 0x0fu]);
  }
  return result;
}

void writeJsonString(std::ostream &output, std::string_view value) {
  static constexpr char hex[] = "0123456789abcdef";
  output.put('"');
  for (const unsigned char character : value) {
    switch (character) {
    case '"': output << "\\\""; break;
    case '\\': output << "\\\\"; break;
    case '\b': output << "\\b"; break;
    case '\f': output << "\\f"; break;
    case '\n': output << "\\n"; break;
    case '\r': output << "\\r"; break;
    case '\t': output << "\\t"; break;
    default:
      if (character < 0x20u) {
        output << "\\u00" << hex[(character >> 4u) & 0x0fu]
               << hex[character & 0x0fu];
      } else {
        output.put(static_cast<char>(character));
      }
      break;
    }
  }
  output.put('"');
}

LifecycleEvidenceEvent makeLifecycleEvidenceEvent(
    std::string phase, ui::LifecycleState state,
    const ui::OperationState &operation, const ui::FrameModel &frame) {
  return LifecycleEvidenceEvent{
      std::move(phase), state, operation.kind, operation.label,
      operation.progress, frame.lifecycleRevision, frame.revision,
      frame.cleanRevision, frame.hasDocument, frame.mutationEnabled,
      !operation.error.empty(), operation.dismissibleError};
}

std::string lifecycleEvidenceJson(
    const std::vector<LifecycleEvidenceEvent> &events,
    const CandidateFailureEvidence &candidateFailure,
    bool callbackSnapshotCoherent, bool passed) {
  std::array<bool, ui::lifecycleStateCount> stateCoverage{};
  std::array<bool, 5u> operationCoverage{};
  bool closedWorld = true;
  for (const LifecycleEvidenceEvent &event : events) {
    if (!ui::isKnownLifecycleState(event.state)) {
      closedWorld = false;
    } else {
      stateCoverage[static_cast<std::size_t>(event.state)] = true;
    }
    const std::size_t operation = static_cast<std::size_t>(event.operation);
    if (operation < operationCoverage.size())
      operationCoverage[operation] = true;
  }

  std::ostringstream output;
  output << "{\n  \"schema\": \"mgif.p8.lifecycle-evidence\",\n"
         << "  \"schemaVersion\": 1,\n"
#if defined(NDEBUG)
         << "  \"configuration\": \"Release\",\n"
#else
         << "  \"configuration\": \"Debug\",\n"
#endif
         << "  \"test\": \"editor_controller_transaction_tests\",\n"
         << "  \"passed\": " << (passed ? "true" : "false") << ",\n"
         << "  \"closedWorld\": " << (closedWorld ? "true" : "false")
         << ",\n  \"stateVocabulary\": [";
  for (std::size_t index = 0u; index < ui::lifecycleStates.size(); ++index) {
    if (index != 0u) output << ", ";
    writeJsonString(output, ui::lifecycleStateName(ui::lifecycleStates[index]));
  }
  output << "],\n  \"stateCoverage\": [\n";
  for (std::size_t index = 0u; index < ui::lifecycleStates.size(); ++index) {
    output << "    {\"state\":";
    writeJsonString(output, ui::lifecycleStateName(ui::lifecycleStates[index]));
    output << ",\"observed\":" << (stateCoverage[index] ? "true" : "false")
           << "}" << (index + 1u == ui::lifecycleStates.size() ? "\n" : ",\n");
  }
  output << "  ],\n  \"operationCoverage\": [\n";
  constexpr std::array<ui::LifecycleOperation, 4u> requiredOperations{
      ui::LifecycleOperation::loadCandidate, ui::LifecycleOperation::save,
      ui::LifecycleOperation::merge, ui::LifecycleOperation::compact};
  for (std::size_t index = 0u; index < requiredOperations.size(); ++index) {
    const ui::LifecycleOperation operation = requiredOperations[index];
    output << "    {\"kind\":";
    writeJsonString(output, lifecycleOperationToken(operation));
    output << ",\"observed\":"
           << (operationCoverage[static_cast<std::size_t>(operation)] ? "true"
                                                                      : "false")
           << "}" << (index + 1u == requiredOperations.size() ? "\n" : ",\n");
  }
  output << "  ],\n  \"candidateFailure\": {\n"
         << "    \"activeDocumentRetained\": "
         << (candidateFailure.activeDocumentRetained ? "true" : "false")
         << ",\n    \"frameIdentityRetained\": "
         << (candidateFailure.frameIdentityRetained ? "true" : "false")
         << ",\n    \"errorObserved\": "
         << (candidateFailure.errorObserved ? "true" : "false")
         << ",\n    \"dismissRecovered\": "
         << (candidateFailure.dismissRecovered ? "true" : "false")
         << ",\n    \"documentDigestBefore\": ";
  writeJsonString(output, digestHex(candidateFailure.documentBefore));
  output << ",\n    \"documentDigestAfter\": ";
  writeJsonString(output, digestHex(candidateFailure.documentAfter));
  output << ",\n    \"projectionDigestBefore\": ";
  writeJsonString(output, digestHex(candidateFailure.projectionBefore));
  output << ",\n    \"projectionDigestAfter\": ";
  writeJsonString(output, digestHex(candidateFailure.projectionAfter));
  output << "\n  },\n  \"callbackSnapshotCoherent\": "
         << (callbackSnapshotCoherent ? "true" : "false")
         << ",\n  \"transitions\": [\n";
  for (std::size_t index = 0u; index < events.size(); ++index) {
    const LifecycleEvidenceEvent &event = events[index];
    output << "    {\"sequence\":" << index << ",\"phase\":";
    writeJsonString(output, event.phase);
    output << ",\"state\":";
    writeJsonString(output, ui::lifecycleStateName(event.state));
    output << ",\"operation\":";
    writeJsonString(output, lifecycleOperationToken(event.operation));
    output << ",\"operationLabel\":";
    writeJsonString(output, event.operationLabel);
    output << ",\"progress\":";
    if (event.progress && std::isfinite(*event.progress))
      output << *event.progress;
    else
      output << "null";
    output << ",\"lifecycleRevision\":" << event.lifecycleRevision
           << ",\"documentRevision\":" << event.documentRevision
           << ",\"cleanRevision\":" << event.cleanRevision
           << ",\"hasDocument\":"
           << (event.hasDocument ? "true" : "false")
           << ",\"mutationEnabled\":"
           << (event.mutationEnabled ? "true" : "false")
           << ",\"errorPresent\":"
           << (event.errorPresent ? "true" : "false")
           << ",\"dismissibleError\":"
           << (event.dismissibleError ? "true" : "false") << "}"
           << (index + 1u == events.size() ? "\n" : ",\n");
  }
  output << "  ]\n}\n";
  return output.str();
}

bool writeLifecycleEvidenceIfRequested(
    const std::vector<LifecycleEvidenceEvent> &events,
    const CandidateFailureEvidence &candidateFailure,
    bool callbackSnapshotCoherent, bool passed, std::string *error) {
  const char *requested = std::getenv("MGIF_P8_LIFECYCLE_EVIDENCE");
  if (requested == nullptr || *requested == '\0')
    return true;

  std::filesystem::path destination(requested);
  std::error_code filesystemError;
  if (std::filesystem::is_directory(destination, filesystemError))
    destination /= "p8-lifecycle-evidence.json";
  filesystemError.clear();
  const std::filesystem::path parent = destination.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, filesystemError);
    if (filesystemError) {
      if (error) *error = "could not create lifecycle evidence directory";
      return false;
    }
  }

  std::filesystem::path temporary = destination;
  temporary += ".tmp";
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
      if (error) *error = "could not open lifecycle evidence staging file";
      return false;
    }
    const std::string json = lifecycleEvidenceJson(
        events, candidateFailure, callbackSnapshotCoherent, passed);
    output.write(json.data(), static_cast<std::streamsize>(json.size()));
    output.flush();
    if (!output) {
      if (error) *error = "could not flush lifecycle evidence staging file";
      return false;
    }
  }

  const auto targetStatus = std::filesystem::symlink_status(
      destination, filesystemError);
  if (!filesystemError && std::filesystem::exists(targetStatus) &&
      (!std::filesystem::is_regular_file(targetStatus) ||
       std::filesystem::is_symlink(targetStatus))) {
    std::filesystem::remove(temporary, filesystemError);
    if (error) *error = "lifecycle evidence target is not a regular file";
    return false;
  }
  filesystemError.clear();
  std::filesystem::remove(destination, filesystemError);
  if (filesystemError) {
    std::filesystem::remove(temporary, filesystemError);
    if (error) *error = "could not replace lifecycle evidence target";
    return false;
  }
  std::filesystem::rename(temporary, destination, filesystemError);
  if (filesystemError) {
    std::filesystem::remove(temporary, filesystemError);
    if (error) *error = "could not publish lifecycle evidence target";
    return false;
  }
  return true;
}

bool testControllerLifecycleContract() {
  TemporaryDirectory temporary;
  bool passed = expect(!temporary.path().empty(),
                       "lifecycle temporary directory is available");
  if (!passed)
    return false;

  std::vector<LifecycleObservation> observations;
  std::vector<ui::FrameModel> snapshots;
  std::vector<LifecycleEvidenceEvent> evidenceEvents;
  std::string evidencePhase = "construct";
  bool callbackSnapshotCoherent = true;
  ui::EditorController *controllerPointer = nullptr;
  ui::EditorController::Callbacks callbacks;
  callbacks.onLifecycle = [&](ui::LifecycleState state,
                              const ui::OperationState &operation) {
    const std::uint64_t revision = controllerPointer == nullptr
                                       ? 0u
                                       : controllerPointer->lifecycleRevision();
    observations.push_back(
        LifecycleObservation{state, operation.kind, operation.progress, revision});
    if (controllerPointer != nullptr) {
      const ui::FrameModel frame = controllerPointer->buildFrameModel();
      callbackSnapshotCoherent &=
          frame.lifecycle == state && frame.operation.kind == operation.kind;
      snapshots.push_back(frame);
      evidenceEvents.push_back(makeLifecycleEvidenceEvent(
          evidencePhase, state, operation, frame));
    }
  };
  ui::EditorController controller(std::move(callbacks));
  controllerPointer = &controller;

  passed &= expect(ui::lifecycleStateCount == 9u,
                   "production lifecycle vocabulary has exactly nine states");
  const std::array<std::string_view, ui::lifecycleStateCount> names{
      "NoDocument", "LoadingCandidate", "ReadyClean", "ReadyModified",
      "GizmoHover", "GizmoDragging", "Modal", "Saving/Merging/Compacting",
      "Error"};
  for (std::size_t index = 0; index < names.size(); ++index) {
    const auto state = static_cast<ui::LifecycleState>(index);
    passed &= expect(ui::lifecycleStateName(state) == names[index],
                     "lifecycle state name is closed-world and canonical");
  }
  const ui::FrameModel initialFrame = controller.buildFrameModel();
  evidenceEvents.push_back(makeLifecycleEvidenceEvent(
      "initial.no-document", initialFrame.lifecycle, initialFrame.operation,
      initialFrame));
  passed &= expect(initialFrame.lifecycle == ui::LifecycleState::noDocument,
                   "controller starts in NoDocument");
  const ui::LifecycleSnapshot initialLifecycle = controller.lifecycleSnapshot();
  passed &= expect(initialLifecycle.state == ui::LifecycleState::noDocument &&
                       !initialLifecycle.overridden,
                   "lightweight lifecycle snapshot mirrors NoDocument");

  ui::OperationState probeOperation;
  probeOperation.kind = ui::LifecycleOperation::loadCandidate;
  probeOperation.progress = 2.0F;
  evidencePhase = "override.loading";
  controller.setLifecycleOverride(ui::LifecycleState::loadingCandidate,
                                  probeOperation);
  const ui::LifecycleSnapshot clampedLifecycle = controller.lifecycleSnapshot();
  passed &= expect(clampedLifecycle.state == ui::LifecycleState::loadingCandidate &&
                       clampedLifecycle.overridden &&
                       clampedLifecycle.operation.progress &&
                       *clampedLifecycle.operation.progress == 1.0F,
                   "lifecycle override clamps observable progress");
  evidencePhase = "override.loading.clear";
  controller.clearLifecycleOverride();
  observations.clear();
  snapshots.clear();

  std::string error;
  const auto source = fixture("minimal_scene.gltf");
  evidencePhase = "open.initial";
  passed &= expect(controller.open(source, &error),
                   "lifecycle fixture opens successfully");
  if (!controller.hasDocument() || controller.projection() == nullptr)
    return false;
  passed &= expect(!observations.empty() &&
                       observations.front().state ==
                           ui::LifecycleState::loadingCandidate &&
                       observations.front().operation ==
                           ui::LifecycleOperation::loadCandidate,
                   "open publishes LoadingCandidate before candidate I/O");
  passed &= expect(observations.back().state ==
                       ui::LifecycleState::readyClean &&
                       controller.buildFrameModel().lifecycle ==
                           ui::LifecycleState::readyClean,
                   "successful open restores ReadyClean");
  passed &= expect(!snapshots.empty() &&
                       snapshots.front().lifecycle ==
                           ui::LifecycleState::loadingCandidate,
                   "transient LoadingCandidate is observable as a frame snapshot");

  observations.clear();
  snapshots.clear();
  error.clear();
  evidencePhase = "open.reload";
  passed &= expect(controller.open(source, &error),
                   "second open keeps the active candidate boundary valid");
  bool retainedDuringReload = false;
  for (const ui::FrameModel &snapshot : snapshots) {
    retainedDuringReload |=
        snapshot.lifecycle == ui::LifecycleState::loadingCandidate &&
        snapshot.hasDocument && snapshot.operation.kind ==
                                   ui::LifecycleOperation::loadCandidate &&
        snapshot.operation.label == "Loading candidate";
  }
  passed &= expect(retainedDuringReload,
                   "LoadingCandidate snapshot retains the old document frame");

  evidencePhase = "modal.open";
  controller.setLifecycleOverride(ui::LifecycleState::modal);
  passed &= expect(controller.buildFrameModel().lifecycle ==
                       ui::LifecycleState::modal,
                   "explicit modal producer is reflected by the frame snapshot");
  evidencePhase = "modal.close";
  controller.clearLifecycleOverride();
  passed &= expect(controller.buildFrameModel().lifecycle ==
                       ui::LifecycleState::readyClean,
                   "clearing modal producer restores the document state");

  evidencePhase = "gizmo.hover";
  controller.setGizmoInteraction(true, false);
  passed &= expect(controller.lifecycleState() == ui::LifecycleState::gizmoHover,
                   "gizmo hover producer publishes GizmoHover");
  evidencePhase = "gizmo.drag";
  controller.setGizmoInteraction(true, true);
  passed &= expect(controller.lifecycleState() ==
                       ui::LifecycleState::gizmoDragging,
                   "gizmo drag producer publishes GizmoDragging");
  evidencePhase = "gizmo.release";
  controller.setGizmoInteraction(false, false);
  passed &= expect(controller.lifecycleState() ==
                       ui::LifecycleState::readyClean,
                   "gizmo producer release restores ReadyClean");

  const ui::FrameModel beforeCandidateFailure = controller.buildFrameModel();
  CandidateFailureEvidence candidateFailure;
  candidateFailure.documentBefore = controller.projection()->documentDigest;
  candidateFailure.projectionBefore = controller.projection()->projectionDigest;
  const auto invalidSource = temporary.path() / "missing-candidate.gltf";
  error.clear();
  evidencePhase = "open.invalid-candidate";
  passed &= expect(!controller.open(invalidSource, &error),
                   "invalid open candidate is rejected");
  const ui::FrameModel failedOpen = controller.buildFrameModel();
  candidateFailure.documentAfter = controller.projection()->documentDigest;
  candidateFailure.projectionAfter = controller.projection()->projectionDigest;
  candidateFailure.activeDocumentRetained =
      controller.hasDocument() &&
      candidateFailure.documentAfter == candidateFailure.documentBefore &&
      candidateFailure.projectionAfter == candidateFailure.projectionBefore;
  candidateFailure.frameIdentityRetained =
      failedOpen.revision == beforeCandidateFailure.revision &&
      failedOpen.cleanRevision == beforeCandidateFailure.cleanRevision &&
      failedOpen.selection == beforeCandidateFailure.selection &&
      failedOpen.scenes.size() == beforeCandidateFailure.scenes.size();
  candidateFailure.errorObserved =
      failedOpen.lifecycle == ui::LifecycleState::error &&
      failedOpen.operation.dismissibleError &&
      !failedOpen.operation.error.empty();
  passed &= expect(failedOpen.lifecycle == ui::LifecycleState::error &&
                       failedOpen.operation.dismissibleError &&
                       !failedOpen.operation.error.empty() &&
                       candidateFailure.activeDocumentRetained &&
                       candidateFailure.frameIdentityRetained,
                   "failed open retains the active projection and enters Error");
  evidencePhase = "error.dismiss-candidate";
  controller.dismissError();
  const ui::FrameModel recoveredCandidate = controller.buildFrameModel();
  candidateFailure.dismissRecovered =
      recoveredCandidate.lifecycle == ui::LifecycleState::readyClean;
  passed &= expect(candidateFailure.dismissRecovered,
                   "dismissing load Error recovers the prior clean state");

  const auto node = controller.session()->document().idAt(EditorObjectKind::node, 0u);
  passed &= expect(node.valid(), "lifecycle fixture exposes a stable node");
  if (node.valid()) {
    evidencePhase = "mutation.rename";
    passed &= expect(dispatchRename(controller, node, "lifecycle-modified").accepted,
                     "lifecycle fixture can enter ReadyModified");
    const ui::FrameModel modifiedFrame = controller.buildFrameModel();
    evidenceEvents.push_back(makeLifecycleEvidenceEvent(
        "mutation.ready-modified", modifiedFrame.lifecycle,
        modifiedFrame.operation, modifiedFrame));
    passed &= expect(modifiedFrame.lifecycle == ui::LifecycleState::readyModified,
                     "authored mutation publishes ReadyModified");
  }

  const auto saveTarget = temporary.path() / "lifecycle-save.glb";
  const std::size_t beforeSaveObservations = observations.size();
  evidencePhase = "save.success";
  passed &= expect(dispatchSave(controller, saveTarget).accepted,
                   "Save As lifecycle operation succeeds");
  bool sawSave = false;
  for (std::size_t index = beforeSaveObservations; index < observations.size();
       ++index) {
    sawSave |= observations[index].state ==
                   ui::LifecycleState::savingMergingCompacting &&
               observations[index].operation == ui::LifecycleOperation::save;
  }
  passed &= expect(sawSave && controller.buildFrameModel().lifecycle ==
                       ui::LifecycleState::readyClean,
                   "Save As exposes the merged busy state and returns clean");

  const auto staleTarget = temporary.path() / "lifecycle-directory-target.glb";
  std::error_code directoryError;
  std::filesystem::create_directory(staleTarget, directoryError);
  const SceneDigest beforeFailedSave = controller.projection()->documentDigest;
  evidencePhase = "save.failure";
  passed &= expect(!dispatchSave(controller, staleTarget).accepted,
                   "failed Save As enters the lifecycle error path");
  const ui::FrameModel failedSave = controller.buildFrameModel();
  passed &= expect(failedSave.lifecycle == ui::LifecycleState::error &&
                       controller.projection()->documentDigest == beforeFailedSave,
                   "failed Save As preserves the active document digest");
  evidencePhase = "error.dismiss-save";
  controller.dismissError();

  ui::UiCommand mergeCommand;
  mergeCommand.source = ui::CommandSource::menu;
  mergeCommand.payload = ui::InvokeCommand{ui::CommandId::importMerge, source};
  const std::size_t beforeMergeObservations = observations.size();
  evidencePhase = "merge.success";
  const ui::RuntimeActionResult merged = controller.dispatch(mergeCommand);
  passed &= expect(merged.accepted,
                   "Import/Merge lifecycle operation succeeds");
  bool sawMergeCandidate = false;
  bool sawMerge = false;
  for (std::size_t index = beforeMergeObservations;
       index < observations.size(); ++index) {
    sawMergeCandidate |=
        observations[index].state == ui::LifecycleState::loadingCandidate &&
        observations[index].operation == ui::LifecycleOperation::loadCandidate;
    sawMerge |= observations[index].state ==
                    ui::LifecycleState::savingMergingCompacting &&
                observations[index].operation == ui::LifecycleOperation::merge;
  }
  passed &= expect(sawMergeCandidate && sawMerge,
                   "Import/Merge exposes candidate and merged busy phases");

  ui::UiCommand compactCommand;
  compactCommand.source = ui::CommandSource::menu;
  compactCommand.payload =
      ui::InvokeCommand{ui::CommandId::compactScene, std::nullopt};
  const std::size_t beforeCompactObservations = observations.size();
  evidencePhase = "compact.success";
  const ui::RuntimeActionResult compacted = controller.dispatch(compactCommand);
  passed &= expect(compacted.accepted,
                   "Compact lifecycle operation succeeds");
  bool sawCompact = false;
  for (std::size_t index = beforeCompactObservations;
       index < observations.size(); ++index) {
    sawCompact |= observations[index].state ==
                      ui::LifecycleState::savingMergingCompacting &&
                  observations[index].operation == ui::LifecycleOperation::compact;
  }
  passed &= expect(sawCompact,
                   "Compact uses the one Saving/Merging/Compacting state");

  std::array<bool, ui::lifecycleStateCount> coveredStates{};
  std::array<bool, 5u> coveredOperations{};
  bool onlyKnownStates = true;
  for (const LifecycleEvidenceEvent &event : evidenceEvents) {
    if (!ui::isKnownLifecycleState(event.state)) {
      onlyKnownStates = false;
      continue;
    }
    coveredStates[static_cast<std::size_t>(event.state)] = true;
    const std::size_t operation = static_cast<std::size_t>(event.operation);
    if (operation < coveredOperations.size())
      coveredOperations[operation] = true;
  }
  const bool allNineStates =
      onlyKnownStates &&
      std::all_of(coveredStates.begin(), coveredStates.end(),
                  [](bool covered) { return covered; });
  const bool allOperations =
      coveredOperations[static_cast<std::size_t>(
          ui::LifecycleOperation::loadCandidate)] &&
      coveredOperations[static_cast<std::size_t>(ui::LifecycleOperation::save)] &&
      coveredOperations[static_cast<std::size_t>(ui::LifecycleOperation::merge)] &&
      coveredOperations[static_cast<std::size_t>(ui::LifecycleOperation::compact)];
  passed &= expect(allNineStates,
                   "real controller producers cover exactly the nine states");
  passed &= expect(allOperations,
                   "real controller producers cover load/save/merge/compact kinds");
  passed &= expect(callbackSnapshotCoherent,
                   "lifecycle callback and rebuilt frame snapshots are coherent");

  std::string evidenceError;
  const bool evidenceWritten = writeLifecycleEvidenceIfRequested(
      evidenceEvents, candidateFailure, callbackSnapshotCoherent, passed,
      &evidenceError);
  if (!evidenceWritten && !evidenceError.empty())
    std::cerr << "FAILED: " << evidenceError << '\n';
  passed &= expect(evidenceWritten,
                   "optional P8 lifecycle evidence manifest is published");
  return passed;
}

} // namespace

int main(int argc, char** argv) {
  if(argc == 2 && std::string_view(argv[1]) == "--profile-base-color-sponza")
    return testBaseColorPickerChangesMaterialWithPendingPublication(true) ? EXIT_SUCCESS : EXIT_FAILURE;
  if(argc == 2 && std::string_view(argv[1]) == "--profile-material-draft-sponza")
    return testMaterialDraftProjectionMatchesFull(true) ? EXIT_SUCCESS : EXIT_FAILURE;
  if(argc == 2 && std::string_view(argv[1]) == "--profile-material-switch-sponza")
    return testMaterialFieldSwitchDoesNotCommitPreviousField(true) ? EXIT_SUCCESS : EXIT_FAILURE;
  bool passed = testNodeNameFastPath("", "empty-name transaction");
  passed &= testNodeNameFastPath(
      "\xE8\x8A\x82\xE7\x82\xB9", "UTF-8-name transaction");
  passed &= testNonNameMutationFallsBack();
  passed &= testAnimatedNodeNameFastPathFences();
  passed &= testControllerFastPathLifecycle();
  passed &= testNativeGizmoPreservesUiCommandSequence();
  passed &= testNativeEscapeAndFocusLossPreserveUiCommandSequence(false);
  passed &= testNativeEscapeAndFocusLossPreserveUiCommandSequence(true);
  passed &= testInspectorContinuousEditDoesNotOwnViewportEscape();
  passed &= testInspectorRejectedInputsKeepProjectionAndPickingCoherent();
  passed &= testInspectorMouseActivationWithoutEditClosesSession();
  passed &= testBaseColorPickerChangesMaterialWithPendingPublication();
  passed &= testMaterialFieldSwitchDoesNotCommitPreviousField();
  passed &= testBaseColorProjectionMatchesFullAndRejectsUnprovenChanges();
  passed &= testMaterialDraftProjectionMatchesFull();
  passed &= testRejectedNativePreviewRetainsEscapeOwnership();
  passed &= testNativeGizmoFinishSequenceExhaustion();
  passed &= testControllerTransaction();
  passed &= testHierarchyClickedNodeParentContract();
  passed &= testControllerLifecycleContract();
  if(passed)
    std::cout << "Editor controller transaction tests passed.\n";
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
