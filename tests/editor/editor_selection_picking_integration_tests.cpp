#include "render/editor/ScenePicking.h"
#include "app/editor/EditorController.h"
#include "app/editor/EditorRendererAdapter.h"
#include "app/editor/EditorViewportRuntime.h"
#include "app/editor/MaterialClipboardCodec.h"
#include "scene/SceneUploadPlanner.h"

#include <mgif/scene_editor/EditorInputRouter.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace demo::editor::ui {

struct EditorViewportRuntimeTestAccess final {
  static bool coherent(const EditorViewportRuntime &runtime) noexcept {
    return runtime.coherentPickPublication().has_value();
  }

  static std::uint64_t rawSceneEpoch(
      const EditorViewportRuntime &runtime) noexcept {
    const auto publication = runtime.coherentPickPublication();
    return publication ? publication->rawSceneActivationEpoch : 0u;
  }

  static EditorPickingTrackResult track(
      EditorViewportRuntime &runtime, const EditorPickRequest &request) {
    return runtime.m_picking.trackAccepted(request);
  }

  static void resolve(EditorViewportRuntime &runtime) noexcept {
    runtime.resolveReadyPicks();
  }

  static std::size_t pending(const EditorViewportRuntime &runtime) noexcept {
    return runtime.m_picking.pendingCount();
  }
};

} // namespace demo::editor::ui

namespace {

using namespace demo;
using namespace demo::editor;
namespace ui = demo::editor::ui;
namespace rhi = demo::rhi;

bool expect(bool condition, std::string_view message) {
  if (!condition) std::cerr << "FAILED: " << message << '\n';
  return condition;
}

template <typename Id>
Id id(EditorObjectKind kind, EditorDocumentAuthority authority,
      std::uint32_t slot) {
  return Id::from(EditorObjectId{kind, slot, 1u, authority}).value_or(Id{});
}

struct Fixture final {
  EditorDocumentAuthority authority{91u};
  SceneId scene = id<SceneId>(EditorObjectKind::scene, authority, 1u);
  NodeId root = id<NodeId>(EditorObjectKind::node, authority, 2u);
  NodeId child = id<NodeId>(EditorObjectKind::node, authority, 3u);
  PrimitiveId primitive =
      id<PrimitiveId>(EditorObjectKind::primitive, authority, 4u);
  EditorSceneFence sceneFence{authority, 8u, 11u, scene, 5u, 7u};
  EditorPickFence pickFence{sceneFence, 13u, 17u};
  PrimitiveOccurrenceKey key{scene, child, primitive, 0u};
  RenderOccurrenceRecord record{{1u, 7u}, key, true};

  SceneRenderOccurrenceSnapshot mapping() const {
    auto result = SceneRenderOccurrenceMapping::create(
        sceneFence, 19u, {{root, std::nullopt, 0u}, {child, root, 1u}},
        {record});
    return result.succeeded() ? result.snapshot : nullptr;
  }

  EditorPickRequest request(std::uint64_t serial,
                            PickIntent intent = PickIntent::singleSelect) const {
    return {serial, intent, pickFence, 19u, {4u, 5u, 32u, 24u},
            {{0.0, 0.0, 0.0}, {0.0, 0.0, 1.0}}};
  }
};

class Backend final : public IEditorPickingPassBackend {
public:
  EditorPickingBackendSubmission enqueue(
      const EditorPickRequest &) noexcept override {
    const EditorPickingPassTicket ticket{++m_next};
    const rhi::SubmissionToken submission{{1u, 1u}, m_next};
    m_pending.push_back({ticket, submission});
    return {true, ticket, submission, PickResultReason::none};
  }

  std::optional<EditorPickingBackendReadback> pollOne() noexcept override {
    if (!m_ready) return std::nullopt;
    auto result = m_ready;
    m_ready.reset();
    return result;
  }

  void cancel(EditorPickingPassTicket ticket) noexcept override {
    for (const Entry &entry : m_pending) {
      if (entry.ticket == ticket) m_cancelled.push_back(ticket);
    }
  }

  void retireReadback(EditorPickingPassTicket ticket,
                      rhi::SubmissionToken submission) noexcept override {
    for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {
      if (it->ticket == ticket && it->submission == submission) {
        m_pending.erase(it);
        return;
      }
    }
  }

  void completeHit(FramebufferPickToken token, double distance) {
    const Entry &entry = m_pending.front();
    m_ready = EditorPickingBackendReadback{
        entry.ticket, entry.submission,
        EditorPickingBackendReadbackStatus::sampled, token,
        EditorWorldPoint{0.0, 0.0, distance}, distance,
        PickResultReason::none};
  }

  void completeMiss() {
    const Entry &entry = m_pending.front();
    m_ready = EditorPickingBackendReadback{
        entry.ticket, entry.submission,
        EditorPickingBackendReadbackStatus::miss, {}, std::nullopt,
        std::nullopt, PickResultReason::background};
  }

private:
  struct Entry {
    EditorPickingPassTicket ticket;
    rhi::SubmissionToken submission;
  };
  std::uint64_t m_next{0u};
  std::vector<Entry> m_pending;
  std::vector<EditorPickingPassTicket> m_cancelled;
  std::optional<EditorPickingBackendReadback> m_ready;
};

class Sink final : public IScenePickingTerminalSink {
public:
  void onTerminal(const EditorPickResult &) noexcept override { ++count; }
  std::uint64_t count{0u};
};

class Lookup final : public IEditorPickingOccurrenceLookup {
public:
  explicit Lookup(SceneRenderOccurrenceSnapshot snapshot)
      : m_snapshot(std::move(snapshot)) {}
  std::optional<RenderOccurrenceRecord> resolve(
      RenderOccurrenceId occurrence) const noexcept override {
    return m_snapshot ? m_snapshot->resolve(occurrence) : std::nullopt;
  }

private:
  SceneRenderOccurrenceSnapshot m_snapshot;
};

bool runTerminal(ScenePicking &picking, Backend &backend,
                 EditorPickingController &controller,
                 const EditorPickRequest &request,
                 SceneRenderOccurrenceSnapshot snapshot, bool hit,
                 const EditorPickingCommitContext &context,
                 EditorPickingEffect &effect) {
  const PickSubmitResult submitted = picking.submit(request, snapshot);
  if (!submitted.accepted() ||
      !controller.trackAccepted(request).accepted()) return false;
  if (hit) {
    backend.completeHit(snapshot->framebufferToken(snapshot->records()[0].id)
                            .value_or(FramebufferPickToken{}),
                        5.0);
  } else {
    backend.completeMiss();
  }
  std::vector<EditorPickResult> terminals;
  picking.poll(terminals);
  if (terminals.size() != 1u || !controller.acceptTerminal(terminals[0]))
    return false;
  const auto committed = controller.commitReady(context);
  if (!committed) return false;
  effect = *committed;
  return true;
}

bool testSelectionPickingHighlightAndFocus() {
  Fixture fixture;
  const SceneRenderOccurrenceSnapshot snapshot = fixture.mapping();
  if (!expect(snapshot != nullptr, "occurrence snapshot seals")) return false;
  Backend backend;
  Sink sink;
  ScenePicking picking(backend, sink);
  EditorPickingController controller;
  Lookup lookup(snapshot);
  EditorPickingCommitContext context{
      fixture.pickFence, 19u, SceneSelectionValue{}, 0u, false, &lookup};
  EditorPickingEffect effect;
  bool passed = runTerminal(picking, backend, controller, fixture.request(1u),
                            snapshot, true, context, effect);
  passed &= expect(
      effect.disposition == EditorPickingEffectDisposition::selection &&
          effect.selection &&
          effect.selection->kind == EditorSelectionEffectKind::setPrimitive &&
          effect.selection->occurrence == fixture.key && effect.browserFocus &&
          !effect.browserFocus->selectTab,
      "single hit emits typed selection and no-tab Browser reveal");

  context.currentSelection = SceneSelectionValue{fixture.key};
  context.selectionRevision = 1u;
  passed &= runTerminal(picking, backend, controller, fixture.request(2u),
                        snapshot, true, context, effect);
  passed &= expect(effect.selection &&
                       effect.selection->kind ==
                           EditorSelectionEffectKind::togglePrimitive,
                   "same primitive emits exact occurrence toggle");

  passed &= runTerminal(picking, backend, controller, fixture.request(3u),
                        snapshot, false, context, effect);
  passed &= expect(effect.disposition == EditorPickingEffectDisposition::blank &&
                       effect.selection &&
                       effect.selection->kind == EditorSelectionEffectKind::clear,
                   "blank single emits clear");

  context.currentSelection = {};
  context.selectionRevision = 2u;
  passed &= runTerminal(
      picking, backend, controller,
      fixture.request(4u, PickIntent::doubleFocus), snapshot, true, context,
      effect);
  passed &= expect(effect.disposition == EditorPickingEffectDisposition::camera &&
                       effect.camera && effect.camera->keepEye &&
                       effect.camera->center == EditorWorldPoint{0.0, 0.0, 5.0} &&
                       effect.camera->navigationSpeed == 5.0 &&
                       effect.camera->nextViewRevision ==
                           fixture.pickFence.viewRevision + 1u,
                   "double hit keeps eye and advances exactly one view revision");

  const auto highlight = snapshot->highlight(SceneSelectionValue{fixture.root}, 3u);
  passed &= expect(highlight && highlight->fence == fixture.sceneFence &&
                       highlight->selectionRevision == 3u &&
                       highlight->occurrences ==
                           std::vector<RenderOccurrenceId>{fixture.record.id},
                   "highlight and Inspector occurrence identity agree");
  return passed;
}

std::filesystem::path fixture(std::string_view name) {
  return std::filesystem::path(MGIF_TEST_SOURCE_DIR) / "tests" / "fixtures" /
         "gltf_editor" / name;
}

EditorViewportMetrics metrics();

class Clipboard final : public IOsTextClipboard {
public:
  std::optional<std::string> readText() const override {
    ++readCount;
    return text;
  }
  bool writeText(std::string_view value) override {
    ++writeCount;
    if (rejectWrites) return false;
    text = std::string{value};
    return true;
  }

  mutable std::size_t readCount{0u};
  std::size_t writeCount{0u};
  std::optional<std::string> text;
  bool rejectWrites{false};
};

class SelectionOccurrences final : public ISceneSelectionOccurrenceLookup {
public:
  SelectionOccurrences(const GltfDocument &document,
                       PrimitiveOccurrenceKey occurrence) noexcept
      : m_authority(document.documentAuthority()),
        m_documentRevision(document.revision()),
        m_sceneActivationEpoch(document.sceneActivationEpoch()),
        m_occurrence(occurrence) {}

  EditorDocumentAuthority authority() const noexcept override {
    return m_authority;
  }
  std::uint64_t documentRevision() const noexcept override {
    return m_documentRevision;
  }
  std::uint64_t sceneActivationEpoch() const noexcept override {
    return m_sceneActivationEpoch;
  }
  std::uint64_t generation() const noexcept override { return 41u; }
  bool contains(PrimitiveOccurrenceKey occurrence) const noexcept override {
    return occurrence == m_occurrence;
  }
  bool visible(PrimitiveOccurrenceKey occurrence) const noexcept override {
    return contains(occurrence);
  }
  bool nodeHighlightEligible(NodeId node) const noexcept override {
    return node == m_occurrence.node;
  }

private:
  EditorDocumentAuthority m_authority{};
  std::uint64_t m_documentRevision{0u};
  std::uint64_t m_sceneActivationEpoch{0u};
  PrimitiveOccurrenceKey m_occurrence{};
};

// A renderer publication contains only the active scene.  Keep this test
// lookup deliberately narrower than the document-backed lookup used by
// Scene Browser/Inspector selection so the two ownership boundaries remain
// observable in one regression.
class CurrentSceneRendererLookup final
    : public ISceneSelectionOccurrenceLookup {
public:
  CurrentSceneRendererLookup(const GltfDocument &document,
                             const SceneProjectionResult &projection)
      : m_authority(document.documentAuthority()),
        m_documentRevision(document.revision()),
        m_sceneActivationEpoch(document.sceneActivationEpoch()),
        m_scene(document.currentSceneId()) {
    for (const SceneProjectionPrimitiveOccurrence &occurrence :
         projection.primitiveOccurrences) {
      if (occurrence.key.scene == m_scene)
        m_entries.push_back({occurrence.key, occurrence.visible});
    }
  }

  EditorDocumentAuthority authority() const noexcept override {
    return m_authority;
  }
  std::uint64_t documentRevision() const noexcept override {
    return m_documentRevision;
  }
  std::uint64_t sceneActivationEpoch() const noexcept override {
    return m_sceneActivationEpoch;
  }
  std::uint64_t generation() const noexcept override { return 73u; }
  bool contains(PrimitiveOccurrenceKey occurrence) const noexcept override {
    return find(occurrence) != nullptr;
  }
  bool visible(PrimitiveOccurrenceKey occurrence) const noexcept override {
    const Entry *entry = find(occurrence);
    return entry != nullptr && entry->visible;
  }
  bool nodeHighlightEligible(NodeId node) const noexcept override {
    return std::any_of(m_entries.begin(), m_entries.end(),
                       [node](const Entry &entry) {
                         return entry.key.node == node;
                       });
  }

private:
  struct Entry final {
    PrimitiveOccurrenceKey key;
    bool visible{false};
  };

  const Entry *find(PrimitiveOccurrenceKey occurrence) const noexcept {
    const auto found = std::find_if(
        m_entries.begin(), m_entries.end(),
        [occurrence](const Entry &entry) { return entry.key == occurrence; });
    return found == m_entries.end() ? nullptr : &*found;
  }

  EditorDocumentAuthority m_authority{};
  std::uint64_t m_documentRevision{0u};
  std::uint64_t m_sceneActivationEpoch{0u};
  SceneId m_scene{};
  std::vector<Entry> m_entries;
};

ui::RuntimeActionResult dispatchMaterial(
    ui::EditorController &controller, ui::MaterialAction action) {
  ui::UiCommand command;
  command.source = ui::CommandSource::inspector;
  command.payload = std::move(action);
  return controller.dispatch(command);
}

bool testProductionControllerRuntimeAndAdapter() {
  Clipboard clipboard;
  ui::EditorRendererAdapter adapter(rhi::BackendType::vulkan);
  adapter.bindRendererIdentity({17u, 3u}, {29u, 5u});
  bool publicationAccepted = true;
  std::size_t publicationCount = 0u;
  ui::EditorController::Callbacks callbacks;
  callbacks.clipboard = &clipboard;
  callbacks.onPublication =
      [&](const ui::EditorController::ProjectionPublication &publication) {
        ++publicationCount;
        std::string error;
        publicationAccepted &= adapter.enqueue(publication, &error);
      };
  callbacks.rendererBusy = [&]() { return adapter.hasPending(); };
  callbacks.rendererPublicationFailed = [&]() {
    return adapter.requiresAuthoritativeRebuild();
  };
  ui::EditorController controller(std::move(callbacks));
  std::string openError;
  bool passed = expect(controller.open(fixture("r09_shared_mesh.gltf"),
                                       &openError),
                       "production controller opens integration fixture");
  if (!passed) return false;
  passed &= expect(publicationAccepted && publicationCount == 1u &&
                       adapter.hasPending() && !adapter.activeFence() &&
                       controller.projection() != nullptr &&
                       controller.projection()->mappingEpoch == 1u,
                   "controller publication queues once and remains unaccepted until renderer flush");
  passed &= expect(controller.buildFrameModel().rendererBusy,
                   "controller UI busy reflects publication pending, not picking state");

  GltfDocument &document = controller.session()->document();
  const SceneId scene = document.currentSceneId();
  const auto node = NodeId::from(document.idAt(EditorObjectKind::node, 0u));
  const auto mesh = MeshId::from(document.idAt(EditorObjectKind::mesh, 0u));
  const auto primitive =
      PrimitiveId::from(document.idAt(EditorObjectKind::primitive, 0u));
  const auto material =
      MaterialId::from(document.idAt(EditorObjectKind::material, 0u));
  passed &= expect(scene.valid() && node && mesh && primitive && material &&
                       controller.projection() != nullptr &&
                       !controller.projection()->primitiveOccurrences.empty(),
                   "typed production fixture identities are available");
  if (!scene.valid() || !node || !mesh || !primitive || !material ||
      controller.projection()->primitiveOccurrences.empty()) {
    return false;
  }

  SceneAsset materialAsset = controller.projection()->asset;
  SceneUploadPlan materialPlan =
      SceneUploadPlanner{}.build(makeSceneAssetView(materialAsset)).plan;
  passed &= expect(!materialAsset.materials.empty() &&
                       !materialPlan.instances.instances.empty() &&
                       !materialPlan.drawCommands.empty() &&
                       SceneUploadPlanner::validate(
                           makeSceneAssetView(materialAsset), materialPlan)
                           .valid,
                   "valid material zero passes upload-plan validation");
  if (materialAsset.materials.empty() ||
      materialPlan.instances.instances.empty() ||
      materialPlan.drawCommands.empty()) {
    return false;
  }
  SceneUploadPlan oneMaterialOutOfRange = materialPlan;
  oneMaterialOutOfRange.instances.instances.front().materialIndex =
      static_cast<std::uint32_t>(materialAsset.materials.size());
  oneMaterialOutOfRange.drawCommands.front().materialIndex =
      static_cast<std::uint32_t>(materialAsset.materials.size());
  passed &= expect(
      !SceneUploadPlanner::validate(makeSceneAssetView(materialAsset),
                                    oneMaterialOutOfRange)
           .valid,
      "non-sentinel material index beyond a nonempty array rejects");

  materialAsset.materials.clear();
  for (ScenePrimitive &value : materialAsset.primitives) {
    value.materialIndex = -1;
  }
  materialPlan.materials.clear();
  materialPlan.descriptorUpdates.clear();
  std::erase_if(materialPlan.requiredResources,
                [](const SceneRequiredResource &resource) {
                  return resource.kind == SceneRequiredResourceKind::Material;
                });
  for (SceneDrawInstance &instance : materialPlan.instances.instances) {
    instance.materialIndex = std::numeric_limits<std::uint32_t>::max();
  }
  for (DrawCommandBuildPlan &draw : materialPlan.drawCommands) {
    draw.materialIndex = std::numeric_limits<std::uint32_t>::max();
  }
  passed &= expect(
      SceneUploadPlanner::validate(makeSceneAssetView(materialAsset),
                                   materialPlan)
          .valid,
      "UINT32_MAX no-material sentinel passes with an empty material array");
  SceneUploadPlan emptyMaterialOutOfRange = materialPlan;
  emptyMaterialOutOfRange.instances.instances.front().materialIndex = 0u;
  emptyMaterialOutOfRange.drawCommands.front().materialIndex = 0u;
  passed &= expect(
      !SceneUploadPlanner::validate(makeSceneAssetView(materialAsset),
                                    emptyMaterialOutOfRange)
           .valid,
      "material zero rejects for an empty material array");
  SceneUploadPlan mismatchedSentinel = materialPlan;
  mismatchedSentinel.drawCommands.front().materialIndex = 0u;
  passed &= expect(
      !SceneUploadPlanner::validate(makeSceneAssetView(materialAsset),
                                    mismatchedSentinel)
           .valid,
      "draw and instance no-material sentinels cannot diverge");

  const ui::FrameModel sharedMeshModel = controller.buildFrameModel();
  bool everyCanonicalOccurrenceHasExactBrowserRow = true;
  for (const SceneProjectionPrimitiveOccurrence &projected :
       controller.projection()->primitiveOccurrences) {
    const ui::GraphRowKey expected{
        projected.key.scene, projected.key.primitive.untyped(),
        projected.key.node, projected.key.instanceOrdinal};
    everyCanonicalOccurrenceHasExactBrowserRow &=
        std::count_if(sharedMeshModel.browser.graphRows.begin(),
                      sharedMeshModel.browser.graphRows.end(),
                      [&](const ui::GraphRow &row) {
                        return row.kind == ui::GraphRowKind::primitive &&
                               row.key == expected;
                      }) == 1;
  }
  passed &= expect(
      controller.projection()->primitiveOccurrences.size() >= 2u &&
          everyCanonicalOccurrenceHasExactBrowserRow,
      "shared Primitive canonical ordinals address exact Browser rows");

  const EditorSelectionSnapshot nodeSelection{
      document.documentAuthority(), NodeSelection{scene, *node, std::nullopt}};
  passed &= expect(controller.applyViewportSelection(nodeSelection, false).accepted &&
                       std::get_if<NodeId>(&controller.sceneSelection().value()) !=
                           nullptr &&
                       controller.buildFrameModel().canFitSelection,
                   "production selection bridge publishes typed Node selection and Fit eligibility");
  const EditorSelectionSnapshot materialSelection{
      document.documentAuthority(), *material};
  passed &= expect(
      controller.applyViewportSelection(materialSelection, false).accepted &&
          !controller.buildFrameModel().canFitSelection,
      "Fit Selection rejects non-render-occurrence material selection");

  const PrimitiveOccurrenceKey occurrence =
      controller.projection()->primitiveOccurrences.front().key;
  SelectionOccurrences occurrences(document, occurrence);
  controller.setSelectionOccurrenceLookup(&occurrences);
  const EditorSelectionEffect selectionEffect{
      .kind = EditorSelectionEffectKind::setPrimitive,
      .cause = SelectionCause::viewportSingle,
      .occurrence = occurrence,
      .expectedRevision = controller.selectionRevision(),
  };
  passed &= expect(controller.applyPickingSelection(selectionEffect).accepted,
                   "production picking effect enters sole selection authority");
  const std::vector<SelectionChanged> changes =
      controller.drainSelectionChanges();
  passed &= expect(changes.size() == 3u && changes.back().browserFocus &&
                       !changes.back().browserFocus->selectTab &&
                       controller.drainSelectionChanges().empty(),
                   "selection events drain exactly once with no-tab Browser reveal");
  ui::PersistentState persistent = controller.ui().state();
  persistent.browserTab = ui::BrowserTab::debug;
  controller.ui().setState(persistent);
  const std::array<EditorObjectId, 3> browserPath{
      scene.untyped(), node->untyped(), mesh->untyped()};
  const ui::GraphRowKey exactBrowserRow{
      scene, primitive->untyped(), *node, occurrence.instanceOrdinal};
  controller.ui().requestFocus(*changes.back().browserFocus,
                               browserPath,
                               primitive->untyped(), exactBrowserRow);
  passed &= expect(controller.ui().state().browserTab == ui::BrowserTab::debug,
                   "Browser reveal expands path without changing active tab");

  const ui::PrimitiveContext primitiveContext{
      scene, *node, *mesh, *primitive, std::nullopt, true, true, true};
  const ui::FrameModel clipboardModel = controller.buildFrameModel();
  const ui::ClipboardMaterial malformedClipboard =
      ui::SceneEditorUi::parseMaterialClipboard("gltf_material:bad",
                                                clipboardModel.materials);
  const ui::ClipboardMaterial outOfRangeClipboard =
      ui::SceneEditorUi::parseMaterialClipboard("gltf_material:999999",
                                                clipboardModel.materials);
  const ui::ClipboardMaterial validClipboard =
      ui::SceneEditorUi::parseMaterialClipboard("gltf_material:0",
                                                clipboardModel.materials);
  passed &= expect(
      malformedClipboard.state == ui::ClipboardMaterialState::malformed &&
          outOfRangeClipboard.state ==
              ui::ClipboardMaterialState::outOfRange &&
          validClipboard.state == ui::ClipboardMaterialState::validIndex &&
          validClipboard.material == material,
      "clipboard preview preserves malformed, enabled out-of-range, and named valid states");
  clipboard.text = "gltf_material:0";
  const std::size_t beforePasteReads = clipboard.readCount;
  passed &= expect(
      dispatchMaterial(
          controller,
          ui::MaterialAction{ui::MaterialOperation::pasteReference,
                             primitiveContext, std::nullopt, {}})
          .accepted &&
          clipboard.readCount == beforePasteReads + 1u,
      "Paste performs exactly one click-time OS read and active-document resolve");
  clipboard.rejectWrites = true;
  passed &= expect(
      !dispatchMaterial(
           controller,
           ui::MaterialAction{ui::MaterialOperation::copyReference,
                              std::nullopt, *material, {}})
           .accepted &&
          clipboard.writeCount == 1u,
      "clipboard write failure is a typed command rejection");

  std::size_t drains = 0u;
  std::size_t highlights = 0u;
  bool runtimeBusy = true;
  ui::EditorViewportRuntime::Callbacks runtimeCallbacks;
  runtimeCallbacks.rendererBusy = [&]() { return runtimeBusy; };
  runtimeCallbacks.selectionValue = []() { return SceneSelectionValue{}; };
  runtimeCallbacks.selectionRevision = []() { return SelectionRevision{0u}; };
  runtimeCallbacks.drainSelectionChanges = [&]() {
    ++drains;
    return std::vector<SelectionChanged>{};
  };
  runtimeCallbacks.setHighlight =
      [&](const SceneSelectionValue &, SelectionRevision) { ++highlights; };
  ui::EditorViewportRuntime runtime(std::move(runtimeCallbacks));
  ui::EditorViewportRuntime::FrameInput input;
  input.viewport = metrics();
  input.windowFocused = true;
  input.cameraNavigationEnabled = false;
  runtime.beginFrame(input, {1u});
  (void)runtime.tick();
  runtime.endFrame();
  passed &= expect(runtime.cameraNavigationSuppressed() && drains == 1u &&
                       highlights == 1u,
                   "production runtime honors busy/camera gate and closes selection/highlight once");
  const PhysicalViewportExtent dpi =
      ui::EditorViewportRuntime::physicalExtentForLogical(
          {101.0, 51.0}, {1.25, 1.25});
  passed &= expect(dpi == PhysicalViewportExtent{127u, 64u},
                   "fractional DPI extent uses frozen right/bottom ceil coverage");
  controller.setSelectionOccurrenceLookup(nullptr);
  return passed;
}

bool testNonCurrentScenePrimitiveSelectionFocusAndStats() {
  GltfLoadResult loaded =
      GltfDocumentIO::loadCandidate(fixture("r09_shared_mesh.gltf"));
  if (!expect(loaded.succeeded(),
              "multi-scene shared-mesh source fixture loads")) {
    return false;
  }
  std::unique_ptr<GltfDocument> document = std::move(loaded).takeCandidate();
  if (!expect(document != nullptr, "shared-mesh candidate owns a document")) {
    return false;
  }

  const auto idAt = [&document](EditorObjectKind kind,
                                std::size_t index) -> EditorObjectId {
    return document->idAt(kind, index);
  };
  const SceneId currentScene =
      SceneId::from(idAt(EditorObjectKind::scene, 0u)).value_or(SceneId{});
  const MeshId sharedMesh =
      MeshId::from(idAt(EditorObjectKind::mesh, 0u)).value_or(MeshId{});
  const PrimitiveId sharedPrimitive = PrimitiveId::from(
      idAt(EditorObjectKind::primitive, 0u)).value_or(PrimitiveId{});
  const NodeId currentNode =
      NodeId::from(idAt(EditorObjectKind::node, 0u)).value_or(NodeId{});
  if (!expect(currentScene.valid() && sharedMesh.valid() &&
                  sharedPrimitive.valid() && currentNode.valid(),
              "source fixture exposes current scene shared primitive IDs")) {
    return false;
  }

  SceneId nonCurrentScene;
  NodeId nonCurrentNode;
  {
    SceneCommands commands(*document);
    const MutationResult addedScene = commands.addScene("Non-current Scene");
    if (!expect(addedScene.applied(),
                "test fixture adds a second authored scene")) {
      return false;
    }
    nonCurrentScene = SceneId::from(
                          idAt(EditorObjectKind::scene,
                               document->objectCount(EditorObjectKind::scene) -
                                   1u))
                          .value_or(SceneId{});
    const MutationResult addedNode =
        commands.addNode(nonCurrentScene, "Non-current Shared");
    if (!expect(addedNode.applied(),
                "test fixture adds a root in the non-current scene")) {
      return false;
    }
    nonCurrentNode = NodeId::from(
                         idAt(EditorObjectKind::node,
                              document->objectCount(EditorObjectKind::node) -
                                  1u))
                         .value_or(NodeId{});
    const MutationResult attached =
        commands.attachMesh(nonCurrentNode, sharedMesh);
    if (!expect(attached.applied(),
                "test fixture attaches the existing mesh to the second root")) {
      return false;
    }
  }
  if (!expect(nonCurrentScene.valid() && nonCurrentNode.valid() &&
                  document->currentSceneId() == currentScene &&
                  document->ownsPrimitiveOccurrence(
                      nonCurrentScene, nonCurrentNode, sharedMesh,
                      sharedPrimitive),
              "second scene owns the shared primitive without becoming current")) {
    return false;
  }

  ui::EditorController controller;
  std::string error;
  if (!expect(controller.replaceDocument(std::move(document), &error),
              "controller publishes the constructed multi-scene document")) {
    return false;
  }
  const GltfDocument &live = controller.session()->document();
  const SceneProjectionResult *projection = controller.projection();
  if (!expect(projection != nullptr, "multi-scene projection is available")) {
    return false;
  }

  const PrimitiveOccurrenceKey currentKey{
      currentScene, currentNode, sharedPrimitive, 0u};
  const PrimitiveOccurrenceKey nonCurrentKey{
      nonCurrentScene, nonCurrentNode, sharedPrimitive, 0u};
  const auto projectedCurrent = std::find_if(
      projection->primitiveOccurrences.begin(),
      projection->primitiveOccurrences.end(),
      [currentKey](const SceneProjectionPrimitiveOccurrence &occurrence) {
        return occurrence.key == currentKey;
      });
  const auto projectedNonCurrent = std::find_if(
      projection->primitiveOccurrences.begin(),
      projection->primitiveOccurrences.end(),
      [nonCurrentKey](const SceneProjectionPrimitiveOccurrence &occurrence) {
        return occurrence.key == nonCurrentKey;
      });
  bool passed = true;
  passed &= expect(projectedCurrent != projection->primitiveOccurrences.end() &&
                       projectedNonCurrent ==
                           projection->primitiveOccurrences.end() &&
                       std::all_of(
                           projection->primitiveOccurrences.begin(),
                           projection->primitiveOccurrences.end(),
                           [currentScene](
                               const SceneProjectionPrimitiveOccurrence &value) {
                             return value.key.scene == currentScene;
                           }),
                   "renderer-facing projection contains only the current scene");

  CurrentSceneRendererLookup rendererLookup(live, *projection);
  passed &= expect(rendererLookup.contains(currentKey) &&
                       !rendererLookup.contains(nonCurrentKey),
                   "renderer occurrence lookup remains current-scene scoped");

  const EditorSelectionSnapshot nonCurrentSelection{
      live.documentAuthority(),
      PrimitiveSelection{nonCurrentScene, nonCurrentNode, sharedPrimitive,
                         sharedMesh, 0u}};
  ui::UiCommand browserSelection;
  browserSelection.source = ui::CommandSource::sceneBrowser;
  browserSelection.payload = ui::SelectObject{nonCurrentSelection};
  const ui::RuntimeActionResult selected = controller.dispatch(browserSelection);
  const std::vector<SelectionChanged> selectionChanges =
      controller.drainSelectionChanges();
  passed &= expect(
      selected.accepted && selectionChanges.size() == 1u &&
          selectionChanges.back().after == SceneSelectionValue{nonCurrentKey} &&
          controller.sceneSelection().value() ==
              SceneSelectionValue{nonCurrentKey},
      "Scene Browser accepts a non-current primitive occurrence through document authority");

  const ui::FrameModel model = controller.buildFrameModel();
  passed &= expect(model.selection == nonCurrentSelection &&
                       model.inspector.selection == nonCurrentSelection &&
                       model.inspector.primitive &&
                       model.inspector.primitive->scene == nonCurrentScene &&
                       model.inspector.primitive->node == nonCurrentNode &&
                       model.inspector.primitive->mesh == sharedMesh &&
                       model.inspector.primitive->primitive == sharedPrimitive,
                   "non-current selection retains its scene occurrence in Inspector context");

  const ui::GraphRowKey nonCurrentRow{
      nonCurrentScene, sharedPrimitive.untyped(), nonCurrentNode, 0u};
  const auto browserRow = std::find_if(
      model.browser.graphRows.begin(), model.browser.graphRows.end(),
      [nonCurrentRow](const ui::GraphRow &row) {
        return row.kind == ui::GraphRowKind::primitive &&
               row.key == nonCurrentRow;
      });
  passed &= expect(browserRow != model.browser.graphRows.end() &&
                       browserRow->selected &&
                       browserRow->selection == nonCurrentSelection,
                   "Scene Browser keeps the exact non-current primitive row selected");

  const auto inspectorField = [&](ui::FieldId field)
      -> std::optional<std::uint32_t> {
    for (const ui::InspectorSection &section : model.inspector.sections) {
      for (const ui::InspectorField &value : section.fields) {
        if (value.id != field)
          continue;
        if (const auto *number = std::get_if<std::uint32_t>(&value.value))
          return *number;
      }
    }
    return std::nullopt;
  };
  const std::optional<PrimitiveGeometryStats> expectedStats =
      live.primitiveGeometryStats(sharedPrimitive);
  passed &= expect(expectedStats && expectedStats->vertexCount == 3u &&
                       expectedStats->indexCount == 3u &&
                       inspectorField(ui::FieldId::vertexCount) == 3u &&
                       inspectorField(ui::FieldId::triangleCount) == 1u,
                   "non-current Inspector primitive statistics come from document authority");

  passed &= expect(
      !controller.sceneSelection().highlightEligible(live, &rendererLookup),
      "document-valid non-current selection is not treated as a current renderer highlight");

  // Exercise the same focus payload consumed by SceneEditorUi.  The request
  // carries the scene-qualified occurrence and an exact row key; accepting it
  // must not switch away from the caller's active Browser tab.
  SelectionOccurrences documentOccurrence(live, nonCurrentKey);
  SceneSelection focusSelection(live.documentAuthority());
  const bool focusChanged = focusSelection.setPrimitive(
      live, documentOccurrence, nonCurrentKey, SelectionCause::hierarchy, true);
  const std::vector<SelectionChanged> focusChanges =
      focusSelection.drainChanges();
  passed &= expect(
      focusChanged && focusChanges.size() == 1u && focusChanges.back().browserFocus &&
          focusChanges.back().browserFocus->target ==
              SceneSelectionValue{nonCurrentKey} &&
          focusChanges.back().browserFocus->revealParents &&
          focusChanges.back().browserFocus->scrollToTarget &&
          !focusChanges.back().browserFocus->selectTab,
      "non-current primitive focus payload retains scene and occurrence identity");
  if (!focusChanges.empty() && focusChanges.back().browserFocus) {
    ui::PersistentState persistent = controller.ui().state();
    persistent.browserTab = ui::BrowserTab::debug;
    controller.ui().setState(persistent);
    const std::array<EditorObjectId, 3> browserPath{
        nonCurrentScene.untyped(), nonCurrentNode.untyped(),
        sharedMesh.untyped()};
    controller.ui().requestFocus(*focusChanges.back().browserFocus,
                                 browserPath, sharedPrimitive.untyped(),
                                 nonCurrentRow);
    passed &= expect(controller.ui().browserTab() == ui::BrowserTab::debug,
                     "non-current Browser focus reveal does not force a tab switch");
  }
  return passed;
}

class Clock final : public EditorClock {
public:
  EditorTimePoint now() const noexcept override { return value; }
  EditorTimePoint value{};
};

EditorViewportMetrics metrics() {
  EditorViewportMetrics result;
  result.logical = {{0.0, 0.0}, {100.0, 100.0}};
  result.framebufferLogicalOrigin = {0.0, 0.0};
  result.physical = {{0u, 0u}, {100u, 100u}};
  result.depthPhysical = result.physical;
  result.colorAttachment = {100u, 100u};
  result.depthAttachment = {100u, 100u};
  result.framebufferScale = {1.0, 1.0};
  result.viewportGeneration = 1u;
  result.projection = {1u, 1u};
  result.view = {1u, 1u};
  result.convention = {EditorClipDepthRange::zeroToOne, true, true};
  result.verticalFieldOfViewRadians = 1.0;
  return result;
}

bool testCoherentRuntimePickPublication() {
  ui::EditorController controller;
  std::string openError;
  bool passed = expect(controller.open(fixture("r09_shared_mesh.gltf"),
                                       &openError),
                       "coherent-pick fixture opens");
  if (!passed || controller.projection() == nullptr) return false;

  SceneProjectionResult projection = *controller.projection();
  passed &= expect(projection.sceneActivationEpoch == 0u &&
                       projection.activeScene &&
                       !projection.primitiveOccurrences.empty(),
                   "initial CPU projection exposes raw scene epoch zero");
  if (!projection.activeScene || projection.primitiveOccurrences.empty()) {
    return false;
  }
  const auto scene = SceneId::from(editorObjectId(*projection.activeScene));
  if (!expect(scene.has_value(), "active scene converts to a typed ID")) {
    return false;
  }

  constexpr std::uint64_t rendererMappingEpoch = 2u;
  constexpr std::uint64_t occurrenceGeneration = 23u;
  EditorViewportMetrics baseMetrics = metrics();
  baseMetrics.projection = {rendererMappingEpoch, projection.mappingEpoch};
  const EditorPickFence baseFence{
      {projection.mapping.authority(), projection.documentRevision,
       rendererMappingEpoch, *scene, projection.sceneActivationEpoch + 1u, 7u},
      baseMetrics.viewportGeneration, baseMetrics.view.revision};
  const PrimitiveOccurrenceKey key =
      projection.primitiveOccurrences.front().key;
  const RenderOccurrenceRecord record{{1u, 7u}, key, true};

  std::optional<std::pair<EditorPickFence, std::uint64_t>> publication =
      std::pair{baseFence, occurrenceGeneration};
  std::vector<EditorPickResult> terminals;
  const ISceneSelectionOccurrenceLookup *boundLookup = nullptr;
  std::size_t selectionEffects = 0u;
  std::size_t cameraEffects = 0u;
  std::size_t resolverCalls = 0u;

  ui::EditorViewportRuntime::Callbacks callbacks;
  callbacks.pickPublication = [&]() { return publication; };
  callbacks.pollPickResults = [&](std::vector<EditorPickResult> &out) {
    out.insert(out.end(), terminals.begin(), terminals.end());
    terminals.clear();
  };
  callbacks.applySelection = [&](const EditorSelectionEffect &) {
    ++selectionEffects;
    return ui::RuntimeActionResult{true, ui::ProjectionRefresh::selectionOnly,
                                   {}};
  };
  callbacks.applyCameraFocus = [&](const EditorCameraFocusEffect &) {
    ++cameraEffects;
    return true;
  };
  callbacks.resolveOccurrence =
      [&](const EditorPickFence &expectedFence, std::uint64_t expectedGeneration,
          RenderOccurrenceId occurrence) -> std::optional<RenderOccurrenceRecord> {
    ++resolverCalls;
    return publication && publication->first == expectedFence &&
                   publication->second == expectedGeneration &&
                   occurrence == record.id
               ? std::optional<RenderOccurrenceRecord>{record}
               : std::nullopt;
  };
  callbacks.resolveOccurrenceKey =
      [&](const EditorPickFence &expectedFence, std::uint64_t expectedGeneration,
          PrimitiveOccurrenceKey occurrence)
          -> std::optional<RenderOccurrenceRecord> {
    ++resolverCalls;
    return publication && publication->first == expectedFence &&
                   publication->second == expectedGeneration && occurrence == key
               ? std::optional<RenderOccurrenceRecord>{record}
               : std::nullopt;
  };
  callbacks.nodeHighlightEligible =
      [&](const EditorPickFence &expectedFence, std::uint64_t expectedGeneration,
          NodeId node) {
    return publication && publication->first == expectedFence &&
           publication->second == expectedGeneration && node == key.node;
  };
  callbacks.bindSelectionOccurrences =
      [&](const ISceneSelectionOccurrenceLookup *lookup) { boundLookup = lookup; };
  callbacks.selectionValue = [] { return SceneSelectionValue{}; };
  callbacks.selectionRevision = [] { return SelectionRevision{0u}; };

  ui::EditorViewportRuntime runtime(std::move(callbacks));
  ui::EditorViewportRuntime::FrameInput input;
  input.viewport = baseMetrics;
  input.projection = &projection;
  input.showGrid = false;
  input.showGizmo = false;
  runtime.beginFrame(input, {1u});
  passed &= expect(ui::EditorViewportRuntimeTestAccess::coherent(runtime) &&
                       ui::EditorViewportRuntimeTestAccess::rawSceneEpoch(runtime) ==
                           0u &&
                       baseFence.scene.sceneEpoch == 1u && boundLookup != nullptr &&
                       boundLookup->authority() == projection.mapping.authority() &&
                       boundLookup->documentRevision() ==
                           projection.documentRevision &&
                       boundLookup->sceneActivationEpoch() == 0u &&
                       boundLookup->generation() == occurrenceGeneration &&
                       boundLookup->contains(key) && boundLookup->visible(key) &&
                       boundLookup->nodeHighlightEligible(key.node),
                   "raw epoch zero coherently binds to one-based pick epoch one");

  std::uint64_t serial = 1u;
  const auto request = [&](PickIntent intent) {
    return EditorPickRequest{
        serial++, intent, baseFence, occurrenceGeneration,
        {4u, 5u, 32u, 24u},
        {{0.0, 0.0, 0.0}, {0.0, 0.0, 1.0}}};
  };
  const auto terminalFor = [&](const EditorPickRequest &value,
                               PickCompletion completion) {
    EditorPickResult terminal;
    terminal.serial = value.serial;
    terminal.intent = value.intent;
    terminal.fence = value.fence;
    terminal.occurrenceGeneration = value.occurrenceGeneration;
    terminal.completion = completion;
    if (completion == PickCompletion::hit) {
      terminal.reason = PickResultReason::none;
      terminal.occurrence = record.id;
      terminal.worldHit = EditorWorldPoint{0.0, 0.0, 5.0};
      terminal.rayDistance = 5.0;
    } else {
      terminal.reason = PickResultReason::background;
    }
    return terminal;
  };
  const auto resetBaseline = [&] {
    publication = std::pair{baseFence, occurrenceGeneration};
    runtime.updateSceneSnapshot(&projection, SceneSelectionValue{}, 0u);
    runtime.updateViewSnapshot(baseMetrics, glm::mat4(1.0f), glm::mat4(1.0f),
                               glm::mat4(1.0f), glm::vec3(0.0f));
    selectionEffects = 0u;
    cameraEffects = 0u;
    resolverCalls = 0u;
  };
  const auto resolveOne = [&](const EditorPickRequest &value,
                              PickCompletion completion) {
    passed &= expect(
        ui::EditorViewportRuntimeTestAccess::track(runtime, value).accepted(),
        "runtime tracks coherent test request");
    terminals.push_back(terminalFor(value, completion));
    ui::EditorViewportRuntimeTestAccess::resolve(runtime);
    passed &= expect(ui::EditorViewportRuntimeTestAccess::pending(runtime) == 0u,
                     "runtime consumes one ready terminal");
  };

  resetBaseline();
  resolveOne(request(PickIntent::singleSelect), PickCompletion::hit);
  passed &= expect(selectionEffects == 1u && cameraEffects == 0u &&
                       resolverCalls == 1u,
                   "coherent baseline hit reaches the selection authority");

  SceneProjectionResult documentChanged = projection;
  ++documentChanged.documentRevision;
  SceneProjectionResult mappingChanged = projection;
  ++mappingChanged.mappingEpoch;
  SceneProjectionResult sceneChanged = projection;
  ++sceneChanged.sceneActivationEpoch;
  SceneProjectionResult exhaustedSceneEpoch = projection;
  exhaustedSceneEpoch.sceneActivationEpoch =
      std::numeric_limits<std::uint64_t>::max();
  SceneProjectionResult inactiveScene = projection;
  inactiveScene.activeScene.reset();
  EditorViewportMetrics viewportChanged = baseMetrics;
  ++viewportChanged.viewportGeneration;
  EditorViewportMetrics viewChanged = baseMetrics;
  ++viewChanged.view.revision;
  ++viewChanged.view.value;

  const auto staleCase = [&](const EditorPickRequest &value,
                             PickCompletion completion, auto mutate,
                             std::string_view message) {
    resetBaseline();
    const EditorPickingTrackResult tracked =
        ui::EditorViewportRuntimeTestAccess::track(runtime, value);
    passed &= expect(tracked.accepted(), "runtime tracks stale test request");
    mutate();
    terminals.push_back(terminalFor(value, completion));
    ui::EditorViewportRuntimeTestAccess::resolve(runtime);
    passed &= expect(selectionEffects == 0u && cameraEffects == 0u &&
                         resolverCalls == 0u &&
                         ui::EditorViewportRuntimeTestAccess::pending(runtime) ==
                             0u,
                     message);
  };

  staleCase(request(PickIntent::singleSelect), PickCompletion::hit,
            [&] { runtime.updateSceneSnapshot(&documentChanged, {}, 0u); },
            "document-revision change makes old hit stale with zero effect");
  staleCase(request(PickIntent::singleSelect), PickCompletion::miss,
            [&] { runtime.updateSceneSnapshot(&mappingChanged, {}, 0u); },
            "CPU mapping change makes old miss stale with zero clear");
  staleCase(request(PickIntent::singleSelect), PickCompletion::miss,
            [&] { runtime.updateSceneSnapshot(&sceneChanged, {}, 0u); },
            "raw scene-epoch change makes old miss stale with zero clear");
  staleCase(request(PickIntent::singleSelect), PickCompletion::hit,
            [&] { runtime.updateSceneSnapshot(&exhaustedSceneEpoch, {}, 0u); },
            "maximum raw scene epoch fails closed without overflow");
  staleCase(request(PickIntent::singleSelect), PickCompletion::miss,
            [&] { runtime.updateSceneSnapshot(&inactiveScene, {}, 0u); },
            "active-scene change makes old miss stale with zero clear");
  staleCase(request(PickIntent::singleSelect), PickCompletion::hit,
            [&] {
              runtime.updateViewSnapshot(viewportChanged, glm::mat4(1.0f),
                                         glm::mat4(1.0f), glm::mat4(1.0f),
                                         glm::vec3(0.0f));
            },
            "viewport-generation change makes old hit stale with zero effect");
  staleCase(request(PickIntent::doubleFocus), PickCompletion::hit,
            [&] {
              runtime.updateViewSnapshot(viewChanged, glm::mat4(1.0f),
                                         glm::mat4(1.0f), glm::mat4(1.0f),
                                         glm::vec3(0.0f));
            },
            "view change makes old double hit stale with zero camera effect");
  staleCase(request(PickIntent::singleSelect), PickCompletion::miss,
            [&] {
              EditorPickFence changed = baseFence;
              ++changed.scene.sceneEpoch;
              publication = std::pair{changed, occurrenceGeneration};
            },
            "renderer scene-fence change makes old miss stale with zero clear");
  staleCase(request(PickIntent::singleSelect), PickCompletion::hit,
            [&] { publication = std::pair{baseFence, occurrenceGeneration + 1u}; },
            "occurrence-generation change makes old hit stale with zero resolve");
  return passed;
}

bool sameFrameO06(bool escapeFirst) {
  Fixture fixture;
  Clock clock;
  EditorInputRouter router(clock);
  EditorInputRoutingContext context;
  context.viewport = metrics();
  context.lifecycle = {fixture.authority, fixture.scene, 5u, 1u};
  context.gizmoActive = true;
  context.viewportPickingEnabled = true;
  EditorInputEventSequence sequence{1u};
  const EditorInputFrameId frame{1u};
  auto route = [&](EditorInputEventPayload payload) {
    return router.route({sequence, frame, std::move(payload)}, context);
  };
  const LogicalViewportPoint point{50.0, 50.0};
  EditorInputDecision press = route(EditorPointerButtonEvent{
      point, EditorPointerButton::primary, EditorInputAction::press});
  ++sequence.value;
  if (!press.effects.beginGizmoDrag) return false;
  if (escapeFirst) {
    (void)route(EditorKeyEvent{EditorInputKey::escape,
                               EditorInputAction::press});
    ++sequence.value;
    (void)route(EditorPointerButtonEvent{
        point, EditorPointerButton::primary, EditorInputAction::release});
  } else {
    (void)route(EditorPointerButtonEvent{
        point, EditorPointerButton::primary, EditorInputAction::release});
    ++sequence.value;
    (void)route(EditorKeyEvent{EditorInputKey::escape,
                               EditorInputAction::press});
  }
  ++sequence.value;
  const EditorInputDecision close = route(EditorInputFrameCloseEvent{});
  return close.effects.detachGizmo && close.effects.clearSelection &&
         !close.effects.commitGizmoDrag;
}

bool priorFrameReleaseDoesNotLeakIntoEscape() {
  Fixture fixture;
  Clock clock;
  EditorInputRouter router(clock);
  EditorInputRoutingContext context;
  context.viewport = metrics();
  context.lifecycle = {fixture.authority, fixture.scene, 5u, 1u};
  context.gizmoActive = true;
  context.viewportPickingEnabled = true;
  EditorInputEventSequence sequence{1u};
  auto route = [&](EditorInputFrameId frame, EditorInputEventPayload payload) {
    const EditorInputDecision result =
        router.route({sequence, frame, std::move(payload)}, context);
    ++sequence.value;
    return result;
  };
  const LogicalViewportPoint point{50.0, 50.0};
  const EditorInputFrameId first{1u};
  if (!route(first, EditorPointerButtonEvent{
                        point, EditorPointerButton::primary,
                        EditorInputAction::press})
           .effects.beginGizmoDrag) {
    return false;
  }
  (void)route(first, EditorPointerButtonEvent{
                         point, EditorPointerButton::primary,
                         EditorInputAction::release});
  const EditorInputDecision committed =
      route(first, EditorInputFrameCloseEvent{});
  if (!committed.effects.commitGizmoDrag ||
      !committed.effects.detachGizmo) {
    return false;
  }
  const EditorInputFrameId second{2u};
  const EditorInputDecision escape =
      route(second,
            EditorKeyEvent{EditorInputKey::escape, EditorInputAction::press});
  const EditorInputDecision closed =
      route(second, EditorInputFrameCloseEvent{});
  return escape.effects.clearSelection && !closed.effects.commitGizmoDrag &&
         !closed.effects.detachGizmo;
}

bool focusLostLateReleaseStaysLatchedUntilEscape() {
  Fixture fixture;
  Clock clock;
  EditorInputRouter router(clock);
  EditorInputRoutingContext context;
  context.viewport = metrics();
  context.lifecycle = {fixture.authority, fixture.scene, 5u, 1u};
  context.gizmoActive = true;
  context.viewportPickingEnabled = true;
  EditorInputEventSequence sequence{1u};
  auto route = [&](EditorInputFrameId frame, EditorInputEventPayload payload) {
    const EditorInputDecision result =
        router.route({sequence, frame, std::move(payload)}, context);
    ++sequence.value;
    return result;
  };
  const LogicalViewportPoint point{50.0, 50.0};
  const EditorInputFrameId first{1u};
  if (!route(first, EditorPointerButtonEvent{
                        point, EditorPointerButton::primary,
                        EditorInputAction::press})
           .effects.beginGizmoDrag) {
    return false;
  }
  (void)route(first, EditorFocusEvent{EditorFocusAction::lost});
  const EditorInputDecision latched =
      route(first, EditorInputFrameCloseEvent{});
  if (!latched.effects.latchGizmo || !router.gizmoDragLatched()) {
    return false;
  }

  const EditorInputFrameId second{2u};
  (void)route(second, EditorFocusEvent{EditorFocusAction::gained});
  (void)route(second, EditorPointerButtonEvent{
                          point, EditorPointerButton::primary,
                          EditorInputAction::release, false, false});
  const EditorInputDecision lateClose =
      route(second, EditorInputFrameCloseEvent{});
  if (lateClose.effects.commitGizmoDrag || lateClose.effects.detachGizmo ||
      !router.gizmoDragLatched()) {
    return false;
  }

  const EditorInputFrameId third{3u};
  (void)route(third,
              EditorKeyEvent{EditorInputKey::escape,
                             EditorInputAction::press});
  const EditorInputDecision detached =
      route(third, EditorInputFrameCloseEvent{});
  return detached.effects.detachGizmo && detached.effects.clearSelection &&
         !detached.effects.commitGizmoDrag && !router.gizmoDragLatched();
}

} // namespace

int main() {
  bool passed = testSelectionPickingHighlightAndFocus();
  passed &= testProductionControllerRuntimeAndAdapter();
  passed &= testNonCurrentScenePrimitiveSelectionFocusAndStats();
  passed &= testCoherentRuntimePickPublication();
  passed &= expect(sameFrameO06(true),
                   "Escape then release is no-history detach");
  passed &= expect(sameFrameO06(false),
                   "release then Escape is no-history detach");
  passed &= expect(priorFrameReleaseDoesNotLeakIntoEscape(),
                   "prior-frame release cannot commit during later Escape");
  passed &= expect(focusLostLateReleaseStaysLatchedUntilEscape(),
                   "focus-lost late release stays non-history latched until Escape");
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
