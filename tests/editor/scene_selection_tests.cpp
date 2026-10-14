#include <mgif/scene_editor/EditorSelectionTypes.h>

#include "app/editor/SceneSelection.h"

#include <mgif/scene_editor/GltfDocumentIO.h>
#include <mgif/scene_editor/SceneCommands.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#if !defined(MGIF_TEST_SOURCE_DIR)
#define MGIF_TEST_SOURCE_DIR "."
#endif

namespace {

using namespace demo::editor;

static_assert(std::is_copy_constructible_v<PrimitiveOccurrenceKey>);
static_assert(std::is_copy_constructible_v<SceneSelectionValue>);
static_assert(std::is_copy_constructible_v<SelectionChanged>);

bool expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
  }
  return condition;
}

std::filesystem::path fixture(const char *name) {
  return std::filesystem::path(MGIF_TEST_SOURCE_DIR) / "tests" / "fixtures" /
         "gltf_editor" / name;
}

std::unique_ptr<GltfDocument> loadDocument(const char *name) {
  GltfLoadResult loaded = GltfDocumentIO::loadCandidate(fixture(name));
  return loaded.succeeded() ? std::move(loaded).takeCandidate() : nullptr;
}

template <typename Id>
Id idAt(const GltfDocument &document, EditorObjectKind kind,
        std::size_t index) {
  return Id::from(document.idAt(kind, index)).value_or(Id{});
}

NodeId withGeneration(NodeId source, std::uint32_t generation) {
  EditorObjectId raw = source.untyped();
  raw.generation = generation;
  return NodeId::from(raw).value_or(NodeId{});
}

class FakeOccurrenceLookup final : public ISceneSelectionOccurrenceLookup {
public:
  struct Entry final {
    PrimitiveOccurrenceKey key;
    bool isVisible{true};
  };

  FakeOccurrenceLookup(const GltfDocument &document,
                       std::uint64_t generation)
      : m_authority(document.documentAuthority()),
        m_documentRevision(document.revision()),
        m_sceneActivationEpoch(document.sceneActivationEpoch()),
        m_generation(generation) {}

  void add(PrimitiveOccurrenceKey key, bool visible = true) {
    m_entries.push_back({key, visible});
  }

  void allowNodeHighlight(NodeId node) {
    m_highlightedNodes.push_back(node);
  }

  [[nodiscard]] EditorDocumentAuthority authority() const noexcept override {
    return m_authority;
  }

  [[nodiscard]] std::uint64_t documentRevision() const noexcept override {
    return m_documentRevision;
  }

  [[nodiscard]] std::uint64_t sceneActivationEpoch() const noexcept override {
    return m_sceneActivationEpoch;
  }

  [[nodiscard]] std::uint64_t generation() const noexcept override {
    return m_generation;
  }

  [[nodiscard]] bool
  contains(PrimitiveOccurrenceKey occurrence) const noexcept override {
    for (const Entry &entry : m_entries) {
      if (entry.key == occurrence) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] bool
  visible(PrimitiveOccurrenceKey occurrence) const noexcept override {
    for (const Entry &entry : m_entries) {
      if (entry.key == occurrence) {
        return entry.isVisible;
      }
    }
    return false;
  }

  [[nodiscard]] bool
  nodeHighlightEligible(NodeId node) const noexcept override {
    for (const NodeId candidate : m_highlightedNodes) {
      if (candidate == node) {
        return true;
      }
    }
    return false;
  }

private:
  EditorDocumentAuthority m_authority;
  std::uint64_t m_documentRevision{0};
  std::uint64_t m_sceneActivationEpoch{0};
  std::uint64_t m_generation{0};
  std::vector<Entry> m_entries;
  std::vector<NodeId> m_highlightedNodes;
};

void bindRemap(SelectionRemap &remap, const SceneSelection &selection,
               const GltfDocument &target,
               std::uint64_t targetOccurrenceGeneration = 0u) {
  remap.sourceAuthority = selection.authority();
  remap.targetAuthority = target.documentAuthority();
  remap.sourceDocumentRevision = selection.documentRevision();
  remap.targetDocumentRevision = target.revision();
  remap.sourceSceneActivationEpoch = selection.sceneActivationEpoch();
  remap.targetSceneActivationEpoch = target.sceneActivationEpoch();
  remap.sourceOccurrenceGeneration = selection.occurrenceGeneration();
  remap.targetOccurrenceGeneration = targetOccurrenceGeneration;
}

bool testSixKindsRevisionOrderAndIdempotence() {
  auto document = loadDocument("feature_preservation.gltf");
  if (!expect(document != nullptr, "six-kind fixture loads")) {
    return false;
  }
  const SceneId scene =
      idAt<SceneId>(*document, EditorObjectKind::scene, 0u);
  const NodeId node = idAt<NodeId>(*document, EditorObjectKind::node, 0u);
  const PrimitiveId primitive =
      idAt<PrimitiveId>(*document, EditorObjectKind::primitive, 0u);
  const MeshId mesh = idAt<MeshId>(*document, EditorObjectKind::mesh, 0u);
  const MaterialId material =
      idAt<MaterialId>(*document, EditorObjectKind::material, 0u);
  const CameraId camera =
      idAt<CameraId>(*document, EditorObjectKind::camera, 0u);
  const LightId light =
      idAt<LightId>(*document, EditorObjectKind::light, 0u);
  if (!expect(scene.valid() && node.valid() && primitive.valid() &&
                  mesh.valid() && material.valid() && camera.valid() &&
                  light.valid(),
              "fixture supplies six authority-safe selection kinds")) {
    return false;
  }

  const PrimitiveOccurrenceKey occurrence{scene, node, primitive, 0u};
  FakeOccurrenceLookup occurrences(*document, 7u);
  occurrences.add(occurrence);
  occurrences.allowNodeHighlight(node);
  const std::array<SceneSelectionValue, 6> values{
      SceneSelectionValue{node}, SceneSelectionValue{occurrence},
      SceneSelectionValue{mesh}, SceneSelectionValue{material},
      SceneSelectionValue{camera}, SceneSelectionValue{light}};
  const std::array<SceneSelectionKind, 6> kinds{
      SceneSelectionKind::node, SceneSelectionKind::primitive,
      SceneSelectionKind::mesh, SceneSelectionKind::material,
      SceneSelectionKind::camera, SceneSelectionKind::light};

  SceneSelection selection(document->documentAuthority());
  bool passed = true;
  for (std::size_t index = 0; index < values.size(); ++index) {
    const SelectionRevision before = selection.revision();
    const bool changed = kinds[index] == SceneSelectionKind::primitive
                             ? selection.setPrimitive(
                                   *document, occurrences, occurrence,
                                   SelectionCause::programmatic)
                             : selection.set(*document, values[index],
                                             SelectionCause::programmatic);
    passed &= expect(changed && selection.revision() == before + 1u &&
                         sceneSelectionKind(selection.value()) == kinds[index],
                     "each distinct kind advances exactly one revision");
    const bool repeated = kinds[index] == SceneSelectionKind::primitive
                              ? selection.setPrimitive(
                                    *document, occurrences, occurrence,
                                    SelectionCause::programmatic)
                              : selection.set(*document, values[index],
                                              SelectionCause::programmatic);
    passed &= expect(
        !repeated && selection.lastSetStatus() ==
                         SceneSelectionSetStatus::unchanged &&
            selection.revision() == before + 1u,
        "identical set is revision- and event-idempotent");
    const bool should_highlight =
        kinds[index] == SceneSelectionKind::node ||
        kinds[index] == SceneSelectionKind::primitive;
    passed &= expect(
        selection.highlightEligible(*document, &occurrences) ==
            should_highlight,
        "only Node and exact visible Primitive derive highlight eligibility");
  }

  const std::vector<SelectionChanged> ordered = selection.drainChanges();
  passed &= expect(ordered.size() == values.size(),
                   "one queued event exists for every accepted set");
  for (std::size_t index = 0; index < ordered.size(); ++index) {
    passed &= expect(ordered[index].revision == index + 1u &&
                         ordered[index].after == values[index] &&
                         !ordered[index].browserFocus,
                     "frame-end drain preserves exact event order");
  }

  const SelectionRevision selected_revision = selection.revision();
  passed &= expect(selection.clear(SelectionCause::escape) &&
                       !selection.clear(SelectionCause::escape) &&
                       selection.revision() == selected_revision + 1u,
                   "clear and repeated empty clear are idempotent");
  const std::vector<SelectionChanged> cleared = selection.drainChanges();
  passed &= expect(cleared.size() == 1u &&
                       cleared[0].cause == SelectionCause::escape &&
                       sceneSelectionKind(cleared[0].after) ==
                           SceneSelectionKind::none,
                   "Escape clear publishes one ordered event");
  return passed;
}

bool testPrimitiveOccurrenceValidationToggleAndFocus() {
  auto document = loadDocument("r09_shared_mesh.gltf");
  if (!expect(document != nullptr, "shared primitive fixture loads")) {
    return false;
  }
  const SceneId scene =
      idAt<SceneId>(*document, EditorObjectKind::scene, 0u);
  const NodeId node0 = idAt<NodeId>(*document, EditorObjectKind::node, 0u);
  const NodeId node1 = idAt<NodeId>(*document, EditorObjectKind::node, 1u);
  const PrimitiveId primitive =
      idAt<PrimitiveId>(*document, EditorObjectKind::primitive, 0u);
  const PrimitiveOccurrenceKey first{scene, node0, primitive, 0u};
  const PrimitiveOccurrenceKey reusedNode{scene, node1, primitive, 0u};
  const PrimitiveOccurrenceKey reusedInstance{scene, node1, primitive, 1u};
  const PrimitiveOccurrenceKey unknownOrdinal{scene, node1, primitive, 2u};
  FakeOccurrenceLookup lookup(*document, 11u);
  lookup.add(first);
  lookup.add(reusedNode);
  lookup.add(reusedInstance);

  SceneSelection selection(document->documentAuthority());
  bool passed = true;
  passed &= expect(
      selection.setPrimitive(*document, lookup, first,
                             SelectionCause::viewportSingle, true),
      "mapping-validated viewport occurrence is selected");
  std::vector<SelectionChanged> events = selection.drainChanges();
  passed &= expect(
      events.size() == 1u && events[0].browserFocus &&
          events[0].browserFocus->target == SceneSelectionValue{first} &&
          events[0].browserFocus->revealParents &&
          events[0].browserFocus->scrollToTarget &&
          !events[0].browserFocus->selectTab,
      "viewport selection reveals without changing Browser tab");
  FakeOccurrenceLookup refreshed_lookup(*document, 12u);
  refreshed_lookup.add(first);
  refreshed_lookup.add(reusedNode);
  refreshed_lookup.add(reusedInstance);
  const SelectionRevision before_refresh = selection.revision();
  passed &= expect(
      !selection.setPrimitive(*document, refreshed_lookup, first,
                              SelectionCause::programmatic) &&
          selection.revision() == before_refresh &&
          selection.occurrenceGeneration() == refreshed_lookup.generation() &&
          selection.pendingChangeCount() == 0u,
      "same occurrence refreshes validated mapping fence without selection event");
  passed &= expect(
      selection.toggleViewportPrimitive(*document, refreshed_lookup, reusedNode,
                                        true) &&
          selection.value() == SceneSelectionValue{reusedNode},
      "same primitive definition on another node stays distinct");
  passed &= expect(
      selection.toggleViewportPrimitive(*document, refreshed_lookup,
                                        reusedInstance,
                                        true) &&
          selection.value() == SceneSelectionValue{reusedInstance},
      "instance ordinal participates in exact occurrence identity");
  const SelectionRevision before_reject = selection.revision();
  passed &= expect(
      !selection.toggleViewportPrimitive(*document, refreshed_lookup,
                                         unknownOrdinal, true) &&
          selection.lastSetStatus() == SceneSelectionSetStatus::rejectedStale &&
          selection.revision() == before_reject &&
          selection.value() == SceneSelectionValue{reusedInstance},
      "unknown instance ordinal is a no-effect stale occurrence");
  passed &= expect(
      selection.toggleViewportPrimitive(*document, refreshed_lookup,
                                        reusedInstance, true) &&
          sceneSelectionKind(selection.value()) == SceneSelectionKind::none,
      "explicit same-occurrence toggle clears exactly once");
  return passed;
}

bool testAuthorityGenerationAndRevisionBoundRemap() {
  auto source = loadDocument("r09_shared_mesh.gltf");
  auto replacement = loadDocument("r09_shared_mesh.gltf");
  if (!expect(source != nullptr && replacement != nullptr,
              "authority/remap fixtures load")) {
    return false;
  }
  const NodeId source_node0 =
      idAt<NodeId>(*source, EditorObjectKind::node, 0u);
  const NodeId source_node1 =
      idAt<NodeId>(*source, EditorObjectKind::node, 1u);
  const NodeId replacement_node1 =
      idAt<NodeId>(*replacement, EditorObjectKind::node, 1u);

  bool passed = true;
  SceneSelection default_selection;
  const NodeId stale_default = withGeneration(
      source_node0,
      static_cast<std::uint32_t>(source_node0.generation() + 1u));
  passed &= expect(
      !default_selection.set(*source, SceneSelectionValue{stale_default},
                             SelectionCause::programmatic) &&
          !default_selection.authority().valid() &&
          default_selection.set(*replacement,
                                SceneSelectionValue{replacement_node1},
                                SelectionCause::hierarchy),
      "rejected stale request cannot pollute a default authority binding");

  SceneSelection selection(source->documentAuthority());
  passed &= expect(selection.set(*source, SceneSelectionValue{source_node1},
                                 SelectionCause::hierarchy),
                   "live source node is selected");
  const NodeId stale = withGeneration(
      source_node1,
      static_cast<std::uint32_t>(source_node1.generation() + 1u));
  passed &= expect(
      !selection.set(*source, SceneSelectionValue{stale},
                     SelectionCause::programmatic) &&
          selection.lastSetStatus() == SceneSelectionSetStatus::rejectedStale,
      "same slot with stale generation never aliases");
  passed &= expect(
      !selection.set(*replacement, SceneSelectionValue{replacement_node1},
                     SelectionCause::hierarchy) &&
          selection.lastSetStatus() ==
              SceneSelectionSetStatus::rejectedForeignAuthority,
      "foreign authority cannot enter the bound service");

  SceneSelection live_remap(source->documentAuthority());
  passed &= expect(live_remap.set(*source, SceneSelectionValue{source_node0},
                                  SelectionCause::hierarchy),
                   "same-document remap source is selected");
  SelectionRemap current_remap;
  bindRemap(current_remap, live_remap, *source);
  current_remap.ids.push_back(
      {source_node0.untyped(), source_node1.untyped()});
  current_remap.complete = true;
  passed &= expect(
      live_remap.reconcile(*source, &current_remap) &&
          live_remap.value() == SceneSelectionValue{source_node1},
      "current mutation envelope applies explicit remap before live retention");

  const std::uint64_t identity_source_revision =
      live_remap.documentRevision();
  const std::uint64_t identity_source_scene_epoch =
      live_remap.sceneActivationEpoch();
  SceneCommands source_commands(*source);
  const MutationResult revision_advance =
      source_commands.setVisibility(source_node0.untyped(), false);
  SelectionRemap identity_remap;
  identity_remap.sourceAuthority = live_remap.authority();
  identity_remap.targetAuthority = source->documentAuthority();
  identity_remap.sourceDocumentRevision = identity_source_revision;
  identity_remap.targetDocumentRevision = source->revision();
  identity_remap.sourceSceneActivationEpoch = identity_source_scene_epoch;
  identity_remap.targetSceneActivationEpoch = source->sceneActivationEpoch();
  identity_remap.sourceOccurrenceGeneration = 0u;
  identity_remap.targetOccurrenceGeneration = 0u;
  identity_remap.ids.push_back(
      {source_node1.untyped(), source_node1.untyped()});
  identity_remap.complete = true;
  passed &= expect(
      revision_advance.applied() &&
          !live_remap.reconcile(*source, &identity_remap) &&
          live_remap.value() == SceneSelectionValue{source_node1} &&
          live_remap.documentRevision() == source->revision() &&
          live_remap.pendingChangeCount() == 2u,
      "identity remap advances internal fence without selection revision/event");

  SelectionRemap stale_replay;
  bindRemap(stale_replay, live_remap, *source);
  stale_replay.ids.push_back(
      {source_node1.untyped(), source_node0.untyped()});
  stale_replay.complete = true;
  const MutationResult second_revision =
      source_commands.setVisibility(source_node0.untyped(), true);
  passed &= expect(
      second_revision.applied() &&
          !live_remap.reconcile(*source, &stale_replay) &&
          live_remap.value() == SceneSelectionValue{source_node1} &&
          live_remap.documentRevision() == source->revision(),
      "stale same-authority remap replay is ignored without changing selection");

  SelectionRemap duplicate;
  bindRemap(duplicate, live_remap, *source);
  duplicate.ids.push_back(
      {source_node1.untyped(), source_node0.untyped()});
  duplicate.ids.push_back(
      {source_node1.untyped(), source_node0.untyped()});
  duplicate.complete = true;
  passed &= expect(
      live_remap.reconcile(*source, &duplicate) &&
          sceneSelectionKind(live_remap.value()) == SceneSelectionKind::none,
      "duplicate matching remap evidence fails closed by clearing");

  SelectionRemap replacement_remap;
  bindRemap(replacement_remap, selection, *replacement);
  replacement_remap.ids.push_back(
      {source_node1.untyped(), replacement_node1.untyped()});
  replacement_remap.complete = true;
  passed &= expect(
      selection.reconcile(*replacement, &replacement_remap, nullptr,
                          SelectionCause::documentReplacement) &&
          selection.authority() == replacement->documentAuthority() &&
          selection.value() == SceneSelectionValue{replacement_node1},
      "revision-bound cross-document remap preserves stable identity");

  SceneSelection deleted(source->documentAuthority());
  const NodeId deleted_node =
      idAt<NodeId>(*source, EditorObjectKind::node, 1u);
  passed &= expect(deleted.set(*source, SceneSelectionValue{deleted_node},
                               SelectionCause::hierarchy),
                   "node is selected before deletion");
  const MutationResult deletion = source_commands.deleteNode(deleted_node);
  passed &= expect(
      deletion.applied() && deleted.reconcile(*source, nullptr) &&
          sceneSelectionKind(deleted.value()) == SceneSelectionKind::none,
      "deleted selection clears without vector-index fallback");
  return passed;
}

bool testPrimitiveRemapAndEffectiveVisibility() {
  auto source = loadDocument("r09_shared_mesh.gltf");
  auto replacement = loadDocument("r09_shared_mesh.gltf");
  if (!expect(source != nullptr && replacement != nullptr,
              "primitive remap fixtures load")) {
    return false;
  }
  const auto occurrence = [](const GltfDocument &document,
                             std::uint32_t ordinal) {
    return PrimitiveOccurrenceKey{
        idAt<SceneId>(document, EditorObjectKind::scene, 0u),
        idAt<NodeId>(document, EditorObjectKind::node, 0u),
        idAt<PrimitiveId>(document, EditorObjectKind::primitive, 0u),
        ordinal};
  };
  const PrimitiveOccurrenceKey before = occurrence(*source, 0u);
  const PrimitiveOccurrenceKey after = occurrence(*replacement, 0u);
  FakeOccurrenceLookup source_lookup(*source, 41u);
  source_lookup.add(before);
  FakeOccurrenceLookup replacement_lookup(*replacement, 52u);
  replacement_lookup.add(after);

  SceneSelection selection(source->documentAuthority());
  bool passed = true;
  passed &= expect(selection.setPrimitive(
                       *source, source_lookup, before,
                       SelectionCause::viewportSingle),
                   "validated Primitive is selected before remap");
  SelectionRemap remap;
  bindRemap(remap, selection, *replacement,
            replacement_lookup.generation());
  remap.primitiveOccurrences.push_back({before, after});
  remap.complete = true;
  passed &= expect(
      selection.reconcile(*replacement, &remap, &replacement_lookup,
                          SelectionCause::undoRedoRestore) &&
          selection.value() == SceneSelectionValue{after} &&
          selection.occurrenceGeneration() == replacement_lookup.generation(),
      "Primitive remap binds exact occurrence and mapping generation");

  const NodeId hidden_node =
      idAt<NodeId>(*replacement, EditorObjectKind::node, 0u);
  SceneCommands commands(*replacement);
  const MutationResult hidden =
      commands.setVisibility(hidden_node.untyped(), false);
  FakeOccurrenceLookup hidden_lookup(*replacement, 53u);
  hidden_lookup.add(after, false);
  SceneSelection hidden_selection(replacement->documentAuthority());
  passed &= expect(
      hidden.applied() &&
          hidden_selection.set(*replacement, SceneSelectionValue{hidden_node},
                               SelectionCause::hierarchy) &&
          !hidden_selection.highlightEligible(*replacement, &hidden_lookup) &&
          !hidden_selection.reconcile(*replacement, nullptr, &hidden_lookup),
      "hidden live selection stays inspectable but cannot highlight");

  SceneSelection picked(replacement->documentAuthority());
  passed &= expect(
      !SceneSelection::pickable(*replacement, hidden_lookup, after) &&
          !picked.setPrimitive(*replacement, hidden_lookup, after,
                               SelectionCause::viewportSingle) &&
          picked.lastSetStatus() == SceneSelectionSetStatus::rejectedHidden,
      "effective-hidden occurrence cannot be newly picked");
  const PrimitiveOccurrenceKey unknown = occurrence(*replacement, 99u);
  passed &= expect(
      !picked.setPrimitive(*replacement, hidden_lookup, unknown,
                           SelectionCause::viewportSingle) &&
          picked.lastSetStatus() == SceneSelectionSetStatus::rejectedStale,
      "unknown occurrence ordinal fails closed even when definitions are live");

  const NodeId locally_visible_child =
      idAt<NodeId>(*replacement, EditorObjectKind::node, 1u);
  SceneSelection ancestor_hidden(replacement->documentAuthority());
  passed &= expect(
      replacement->objectVisible(locally_visible_child.untyped()) &&
          ancestor_hidden.set(*replacement,
                              SceneSelectionValue{locally_visible_child},
                              SelectionCause::hierarchy) &&
          !ancestor_hidden.highlightEligible(*replacement, &hidden_lookup),
      "occurrence snapshot overrides local visibility for hidden-ancestor case");
  return passed;
}

} // namespace

int main() {
  bool passed = true;
  passed &= testSixKindsRevisionOrderAndIdempotence();
  passed &= testPrimitiveOccurrenceValidationToggleAndFocus();
  passed &= testAuthorityGenerationAndRevisionBoundRemap();
  passed &= testPrimitiveRemapAndEffectiveVisibility();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
