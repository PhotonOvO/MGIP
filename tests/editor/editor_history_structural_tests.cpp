#include <mgif/scene_editor/GltfDocumentIO.h>
#include <mgif/scene_editor/GltfSemanticCompare.h>
#include <mgif/scene_editor/SceneEditor.h>
#include <mgif/scene_editor/commands/EditorHistory.h>
#include <mgif/scene_editor/commands/HierarchyCommands.h>
#include <mgif/scene_editor/testing/EditorStateInspector.h>

#include "support/EditorTestSupport.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace demo::editor;
using namespace demo::editor::testing;

bool expect(bool condition, std::string_view message) {
  if (!condition)
    std::cerr << "FAILED: " << message << '\n';
  return condition;
}

std::filesystem::path fixture(const char *name) {
  return std::filesystem::path(MGIF_TEST_SOURCE_DIR) / "tests" / "fixtures" /
         "gltf_editor" / name;
}

std::unique_ptr<GltfDocument>
loadDocument(const char *name = "minimal_scene.gltf") {
  auto loaded = GltfDocumentIO::loadCandidate(fixture(name));
  if (!loaded.succeeded())
    return nullptr;
  return std::move(loaded).takeCandidate();
}

class StructuralRig final {
public:
  explicit StructuralRig(std::unique_ptr<GltfDocument> source,
                         EditorLimits limits = kDefaultEditorLimits,
                         EditorFaultInjector *faultInjector = nullptr)
      : document(std::move(source)), commands(*document, limits, faultInjector),
        context{*document, commands, selection},
        history(context, clock, limits) {}

  std::unique_ptr<GltfDocument> document;
  SceneCommands commands;
  EditorSelectionState selection;
  EditorCommandContext context;
  ManualEditorClock clock;
  EditorHistory history;
};

std::unique_ptr<StructuralRig>
makeRig(const char *name = "minimal_scene.gltf",
        EditorLimits limits = kDefaultEditorLimits,
        EditorFaultInjector *faultInjector = nullptr) {
  auto document = loadDocument(name);
  if (!document)
    return nullptr;
  return std::make_unique<StructuralRig>(std::move(document), limits,
                                         faultInjector);
}

SceneId sceneAt(const GltfDocument &document, std::size_t index) {
  return *SceneId::from(document.idAt(EditorObjectKind::scene, index));
}

NodeId nodeAt(const GltfDocument &document, std::size_t index) {
  return *NodeId::from(document.idAt(EditorObjectKind::node, index));
}

LightId lightAt(const GltfDocument &document, std::size_t index) {
  return *LightId::from(document.idAt(EditorObjectKind::light, index));
}

std::optional<EditorObjectId> createdId(const MutationRemap &remap,
                                        EditorObjectKind kind) {
  const auto &entries = remap.forKind(kind);
  const auto found = std::find_if(
      entries.begin(), entries.end(), [kind](const MutationRemapEntry &entry) {
        return !entry.oldId.valid() && entry.newId.kind == kind;
      });
  return found == entries.end() ? std::nullopt
                                : std::optional<EditorObjectId>{found->newId};
}

std::optional<NodeSelection>
selectedNode(const EditorSelectionState &selection) noexcept {
  if (const auto *node =
          std::get_if<NodeSelection>(&selection.snapshot().value))
    return *node;
  return std::nullopt;
}

bool selectNode(StructuralRig &rig, SceneId scene, NodeId node,
                std::optional<NodeId> parent) {
  return rig.selection.select(
      *rig.document,
      EditorSelectionSnapshot{rig.document->documentAuthority(),
                              NodeSelection{scene, node, parent}});
}

bool selectionIsEmpty(const EditorSelectionState &selection) noexcept {
  return selection.snapshot() == EditorSelectionSnapshot::empty();
}

std::optional<SceneNodeOccurrence> occurrence(StructuralRig &rig, SceneId scene,
                                              NodeId node) {
  return rig.commands.captureNodeOccurrence(scene, node);
}

std::optional<NodeId>
directAddNode(StructuralRig &rig, SceneId scene, std::string name,
              std::optional<NodeId> parent = std::nullopt) {
  const MutationResult result =
      rig.commands.addNode(scene, std::move(name), parent);
  const auto id = createdId(result.remap, EditorObjectKind::node);
  return result.applied() && id ? NodeId::from(*id) : std::nullopt;
}

HistoryResult executePrepared(StructuralRig &rig,
                              CommandPrepareResult prepared) {
  if (!prepared.succeeded()) {
    HistoryResult result;
    result.status = HistoryStatus::rejected;
    result.command.error = prepared.error;
    result.command.historyPolicy = HistoryPolicy::undoable;
    result.command.beforeRevision = rig.document->revision();
    result.command.afterRevision = rig.document->revision();
    result.command.message = std::move(prepared.message);
    return result;
  }
  return rig.history.execute(std::move(prepared.command));
}

std::optional<ObjectSlotInspection> slotFor(const GltfDocument &document,
                                            EditorObjectId id) {
  const DocumentInspection inspection = EditorStateInspector::inspect(document);
  const auto &slots = inspection.slots[static_cast<std::size_t>(id.kind)];
  if (id.slot >= slots.size())
    return std::nullopt;
  return slots[id.slot];
}

std::vector<NodeId> nodeRegistryIds(const GltfDocument &document) {
  std::vector<NodeId> ids;
  ids.reserve(document.objectCount(EditorObjectKind::node));
  for (std::size_t index = 0;
       index < document.objectCount(EditorObjectKind::node); ++index) {
    const auto id = NodeId::from(document.idAt(EditorObjectKind::node, index));
    if (!id)
      return {};
    ids.push_back(*id);
  }
  return ids;
}

std::vector<LightId> lightRegistryIds(const GltfDocument &document) {
  std::vector<LightId> ids;
  ids.reserve(document.objectCount(EditorObjectKind::light));
  for (std::size_t index = 0;
       index < document.objectCount(EditorObjectKind::light); ++index) {
    const auto id =
        LightId::from(document.idAt(EditorObjectKind::light, index));
    if (!id)
      return {};
    ids.push_back(*id);
  }
  return ids;
}

std::optional<MutationResult> directDeleteNode(StructuralRig &rig,
                                               SceneId scene, NodeId node) {
  const auto captured = occurrence(rig, scene, node);
  if (!captured)
    return std::nullopt;
  return rig.commands.deleteNodeAt(*captured,
                                   SceneOccurrencePolicy::clickedScene);
}

bool completeStructuralInspection(const RetainedCommandInspection &inspection,
                                  EditorCommandKind kind,
                                  CommandLifecycleState lifecycle,
                                  std::uint64_t stateRevision) {
  return inspection.schemaVersion == 1u && inspection.kind == kind &&
         inspection.lifecycle == lifecycle &&
         inspection.stateRevision == stateRevision &&
         inspection.retainedSnapshotBytes != 0u &&
         inspection.undoStructuralSide.present &&
         !inspection.undoStructuralSide.canonicalBytes.empty() &&
         inspection.redoStructuralSide.present &&
         !inspection.redoStructuralSide.canonicalBytes.empty() &&
         inspection.undoStructuralSide.canonicalBytes !=
             inspection.redoStructuralSide.canonicalBytes &&
         detail::retainedCommandInspectionHashMatches(inspection);
}

bool testRename() {
  auto rig = makeRig();
  if (!expect(rig != nullptr, "rename fixture loads"))
    return false;
  const SceneId scene = sceneAt(*rig->document, 0);
  const NodeId root = nodeAt(*rig->document, 0);
  if (!expect(selectNode(*rig, scene, root, std::nullopt),
              "rename selection publishes"))
    return false;

  CommandPrepareResult prepared =
      prepareRenameNodeCommand(rig->context, root, "Renamed Root");
  bool passed = expect(prepared.succeeded(), "rename command prepares");
  if (!prepared.succeeded())
    return false;
  const RetainedCommandInspection before =
      prepared.command->inspectRetainedState();
  passed &= expect(
      prepared.command->description() == "Rename 'Root'" &&
          prepared.command->historyPolicy() == HistoryPolicy::undoable &&
          before.kind == EditorCommandKind::renameNode &&
          before.lifecycle == CommandLifecycleState::prepared &&
          before.stateRevision == 0u && !before.undoStructuralSide.present &&
          !before.redoStructuralSide.present &&
          detail::retainedCommandInspectionHashMatches(before),
      "prepared rename has complete canonical retained state");
  const std::uint64_t revision = rig->document->revision();
  const HistoryResult execute =
      rig->history.execute(std::move(prepared.command));
  passed &=
      expect(execute.status == HistoryStatus::pushed &&
                 execute.command.status == CommandStatus::applied &&
                 rig->document->objectName(root.untyped()) == "Renamed Root" &&
                 rig->document->revision() == revision + 1u &&
                 selectedNode(rig->selection) ==
                     NodeSelection{scene, root, std::nullopt},
             "rename executes once and preserves selection");
  HistoryInspection inspection = EditorStateInspector::inspect(rig->history);
  passed &= expect(
      inspection.undoOldestToNewest.size() == 1u &&
          inspection.undoOldestToNewest[0].description == "Rename 'Root'" &&
          inspection.undoOldestToNewest[0].retainedCommand.lifecycle ==
              CommandLifecycleState::executed &&
          inspection.undoOldestToNewest[0].retainedCommand.stateRevision ==
              1u &&
          detail::retainedCommandInspectionHashMatches(
              inspection.undoOldestToNewest[0].retainedCommand),
      "executed rename inspection advances lifecycle and SHA");
  passed &= expect(rig->history.undo().status == HistoryStatus::moved &&
                       rig->document->objectName(root.untyped()) == "Root" &&
                       rig->document->revision() == revision + 2u,
                   "rename Undo restores captured old value");
  inspection = EditorStateInspector::inspect(rig->history);
  passed &= expect(
      inspection.redoOldestToNewest.size() == 1u &&
          inspection.redoOldestToNewest[0].retainedCommand.lifecycle ==
              CommandLifecycleState::undone &&
          inspection.redoOldestToNewest[0].retainedCommand.stateRevision == 2u,
      "rename Undo inspection advances state revision");
  passed &=
      expect(rig->history.redo().status == HistoryStatus::moved &&
                 rig->document->objectName(root.untyped()) == "Renamed Root" &&
                 rig->document->revision() == revision + 3u,
             "rename Redo restores captured final value");

  auto unchanged = makeRig();
  if (!expect(unchanged != nullptr, "unchanged rename fixture loads"))
    return false;
  const NodeId unchangedRoot = nodeAt(*unchanged->document, 0);
  const ExactEditorStateSnapshot unchangedBefore =
      exactEditorStateSnapshot(*unchanged->document, unchanged->commands,
                               unchanged->selection, unchanged->history);
  const HistoryResult unchangedResult = executePrepared(
      *unchanged,
      prepareRenameNodeCommand(unchanged->context, unchangedRoot, "Root"));
  passed &= expect(
      unchangedResult.status == HistoryStatus::rejected &&
          unchangedResult.command.status == CommandStatus::unchanged &&
          exactEditorStateSnapshot(*unchanged->document, unchanged->commands,
                                   unchanged->selection,
                                   unchanged->history) == unchangedBefore,
      "unchanged rename creates no history and writes nothing");

  auto stale = makeRig();
  if (!expect(stale != nullptr, "stale rename fixture loads"))
    return false;
  const NodeId staleRoot = nodeAt(*stale->document, 0);
  prepared = prepareRenameNodeCommand(stale->context, staleRoot, "Prepared");
  if (!expect(prepared.succeeded(), "stale rename command prepares"))
    return false;
  const MutationResult foreign =
      stale->commands.rename(staleRoot.untyped(), "Foreign");
  const ExactEditorStateSnapshot staleBefore = exactEditorStateSnapshot(
      *stale->document, stale->commands, stale->selection, stale->history);
  const HistoryResult staleResult =
      stale->history.execute(std::move(prepared.command));
  passed &= expect(
      foreign.applied() && staleResult.status == HistoryStatus::rejected &&
          staleResult.command.error == MutationErrorCode::staleTransaction &&
          exactEditorStateSnapshot(*stale->document, stale->commands,
                                   stale->selection,
                                   stale->history) == staleBefore,
      "stale rename execute is an exact no-write");

  ScriptedEditorFaultInjector fault;
  (void)fault.failOnCall(EditorFaultPoint::allocation, 1u);
  auto allocation = makeRig();
  if (!expect(allocation != nullptr, "rename allocation fixture loads"))
    return false;
  const ExactEditorStateSnapshot allocationBefore =
      exactEditorStateSnapshot(*allocation->document, allocation->commands,
                               allocation->selection, allocation->history);
  const CommandPrepareResult allocationFailure = prepareRenameNodeCommand(
      allocation->context, nodeAt(*allocation->document, 0), "Never", &fault);
  passed &= expect(
      !allocationFailure.succeeded() &&
          allocationFailure.error == MutationErrorCode::allocationFailed &&
          exactEditorStateSnapshot(*allocation->document, allocation->commands,
                                   allocation->selection,
                                   allocation->history) == allocationBefore,
      "rename factory allocation failure is exact");
  return passed;
}

bool testAddChildAndInspection() {
  auto rig = makeRig();
  if (!expect(rig != nullptr, "child Add fixture loads"))
    return false;
  const SceneId scene = sceneAt(*rig->document, 0);
  const NodeId parent = nodeAt(*rig->document, 0);
  CommandPrepareResult prepared = prepareAddChildNodeCommand(
      rig->context, AddChildNodeRequest{scene, parent, "Child"});
  bool passed =
      expect(prepared.succeeded() &&
                 prepared.command->description() == "Add Node 'Child'",
             "child Add command prepares with exact description");
  if (!prepared.succeeded())
    return false;
  const std::uint64_t retainedBytes = prepared.command->snapshotBytes();
  const std::uint64_t revision = rig->document->revision();
  const HistoryResult execute =
      rig->history.execute(std::move(prepared.command));
  const auto selected = selectedNode(rig->selection);
  if (!expect(execute.status == HistoryStatus::pushed && selected.has_value(),
              "child Add executes and selects a node"))
    return false;
  const NodeId child = selected->node;
  passed &= expect(selected == NodeSelection{scene, child, parent} &&
                       rig->document->nodeChildIds(parent).back() == child &&
                       rig->document->objectName(child.untyped()) == "Child" &&
                       rig->document->revision() == revision + 1u &&
                       rig->history.retainedSnapshotBytes() == retainedBytes,
                   "child Add appends and charges the exact retained bytes");
  HistoryInspection inspection = EditorStateInspector::inspect(rig->history);
  if (!expect(inspection.undoOldestToNewest.size() == 1u,
              "child Add inspection exposes one entry"))
    return false;
  const RetainedCommandInspection executeInspection =
      inspection.undoOldestToNewest[0].retainedCommand;
  passed &= expect(
      completeStructuralInspection(executeInspection,
                                   EditorCommandKind::addChildNode,
                                   CommandLifecycleState::executed, 1u) &&
          executeInspection.executeSelection == rig->selection.snapshot() &&
          executeInspection.undoSelection == EditorSelectionSnapshot::empty() &&
          executeInspection.redoSelection == rig->selection.snapshot(),
      "child Add retains independent sides and selection envelope");

  passed &= expect(rig->history.undo().status == HistoryStatus::moved &&
                       !rig->document->contains(child.untyped()) &&
                       selectionIsEmpty(rig->selection) &&
                       rig->document->revision() == revision + 2u,
                   "child Add Undo clears selection and tombstones the child");
  const auto tombstone = slotFor(*rig->document, child.untyped());
  passed &=
      expect(tombstone && !tombstone->live &&
                 tombstone->generation == child.generation() + 1u,
             "child Add Undo retains the exact next-generation tombstone");
  const auto foreign = directAddNode(*rig, scene, "Foreign", parent);
  if (!expect(foreign.has_value(), "later foreign child appends"))
    return false;
  const std::uint64_t beforeRedo = rig->document->revision();
  passed &= expect(
      rig->history.redo().status == HistoryStatus::moved &&
          rig->document->contains(child.untyped()) &&
          rig->document->nodeChildIds(parent) ==
              std::vector<NodeId>{child, *foreign} &&
          selectedNode(rig->selection) == NodeSelection{scene, child, parent} &&
          rig->document->revision() == beforeRedo + 1u,
      "child Add Redo resurrects the exact ID before foreign tail");
  inspection = EditorStateInspector::inspect(rig->history);
  passed &= expect(
      inspection.undoOldestToNewest[0].retainedCommand.stateRevision == 3u &&
          inspection.undoOldestToNewest[0]
                  .retainedCommand.undoStructuralSide.canonicalBytes ==
              executeInspection.undoStructuralSide.canonicalBytes &&
          inspection.undoOldestToNewest[0]
                  .retainedCommand.redoStructuralSide.canonicalBytes ==
              executeInspection.redoStructuralSide.canonicalBytes &&
          inspection.undoOldestToNewest[0].retainedCommand.canonicalSha256 !=
              executeInspection.canonicalSha256,
      "child Add lifecycle changes SHA without changing either retained side");
  return passed;
}

bool testAddLights() {
  bool passed = true;
  const std::array<std::string_view, 3> types{"point", "directional", "spot"};
  for (const std::string_view type : types) {
    for (const bool childPlacement : {false, true}) {
      auto rig = makeRig();
      if (!expect(rig != nullptr, "Add Light fixture loads"))
        return false;
      const SceneId scene = sceneAt(*rig->document, 0);
      const NodeId root = nodeAt(*rig->document, 0);
      const std::optional<NodeId> parent =
          childPlacement ? std::optional<NodeId>{root} : std::nullopt;
      LightProperties properties;
      properties.type = type;
      properties.intensity = childPlacement ? 3.5 : 2.5;
      const std::string lightName = std::string(type) + " Fixture";
      const std::size_t nodeCount =
          rig->document->objectCount(EditorObjectKind::node);
      const std::size_t lightCount =
          rig->document->objectCount(EditorObjectKind::light);
      CommandPrepareResult prepared = prepareAddLightCommand(
          rig->context,
          AddLightRequest{scene, parent, std::string(type) + " Owner",
                          lightName, properties});
      const std::string expectedDescription =
          "Add " + std::string(type) + " Light '" + lightName + "'";
      passed &=
          expect(prepared.succeeded() &&
                     prepared.command->description() == expectedDescription,
                 "Add Light prepares the exact type/name description");
      if (!prepared.succeeded())
        return false;
      const std::uint64_t revision = rig->document->revision();
      const HistoryResult execute =
          rig->history.execute(std::move(prepared.command));
      const auto selection = selectedNode(rig->selection);
      if (!expect(execute.status == HistoryStatus::pushed &&
                      selection.has_value() &&
                      rig->document->objectCount(EditorObjectKind::node) ==
                          nodeCount + 1u &&
                      rig->document->objectCount(EditorObjectKind::light) ==
                          lightCount + 1u,
                  "Add Light publishes one owning node and definition"))
        return false;
      const NodeId owner = selection->node;
      const LightId light = lightAt(*rig->document, lightCount);
      const auto stored = rig->commands.lightProperties(light);
      const HistoryInspection inspected =
          EditorStateInspector::inspect(rig->history);
      passed &= expect(
          selection == NodeSelection{scene, owner, parent} && stored &&
              *stored == properties &&
              rig->document->hasNodeOccurrence(scene, owner, parent),
          "Add Light retains typed ownership and node/light relationship");
      passed &=
          expect(rig->document->revision() == revision + 1u &&
                     inspected.undoOldestToNewest.size() == 1u &&
                     completeStructuralInspection(
                         inspected.undoOldestToNewest[0].retainedCommand,
                         EditorCommandKind::addLight,
                         CommandLifecycleState::executed, 1u),
                 "Add Light retains exact revision and both structural sides");

      passed &= expect(rig->history.undo().status == HistoryStatus::moved &&
                           !rig->document->contains(owner.untyped()) &&
                           !rig->document->contains(light.untyped()) &&
                           selectionIsEmpty(rig->selection) &&
                           rig->document->revision() == revision + 2u,
                       "Add Light Undo tombstones owner and definition");
      const auto ownerSlot = slotFor(*rig->document, owner.untyped());
      const auto lightSlot = slotFor(*rig->document, light.untyped());
      passed &= expect(ownerSlot && lightSlot && !ownerSlot->live &&
                           !lightSlot->live &&
                           ownerSlot->generation == owner.generation() + 1u &&
                           lightSlot->generation == light.generation() + 1u,
                       "Add Light Undo retains both exact tombstones");

      if (type == "point" && !childPlacement) {
        const MutationResult visible =
            rig->commands.setVisibility(root.untyped(), false);
        const std::string rootExtension(
            rig->document->objectExtensionsJson(root.untyped()));
        LightProperties laterProperties;
        laterProperties.type = "directional";
        const MutationResult later = rig->commands.createLight(
            scene, std::nullopt, "Later Owner", "Later Light", laterProperties);
        const auto laterNodeId = createdId(later.remap, EditorObjectKind::node);
        const auto laterLightId =
            createdId(later.remap, EditorObjectKind::light);
        if (!expect(visible.applied() && later.applied() && laterNodeId &&
                        laterLightId,
                    "foreign visibility and later light publish after Undo"))
          return false;
        const NodeId laterNode = *NodeId::from(*laterNodeId);
        const LightId laterLight = *LightId::from(*laterLightId);
        const std::uint64_t beforeRedo = rig->document->revision();
        passed &= expect(
            rig->history.redo().status == HistoryStatus::moved &&
                rig->document->contains(owner.untyped()) &&
                rig->document->contains(light.untyped()) &&
                rig->document->contains(laterNode.untyped()) &&
                rig->document->contains(laterLight.untyped()) &&
                rig->document->sceneRootNodeIds(scene) ==
                    std::vector<NodeId>{root, owner, laterNode} &&
                lightAt(*rig->document, 0) == light &&
                lightAt(*rig->document, 1) == laterLight &&
                rig->document->objectExtensionsJson(root.untyped()) ==
                    rootExtension &&
                selectedNode(rig->selection) ==
                    NodeSelection{scene, owner, std::nullopt} &&
                rig->document->revision() == beforeRedo + 1u,
            "Add Light Redo composes owned names/records before foreign tails");
      } else {
        passed &= expect(rig->history.redo().status == HistoryStatus::moved &&
                             rig->document->contains(owner.untyped()) &&
                             rig->document->contains(light.untyped()) &&
                             lightAt(*rig->document, lightCount) == light &&
                             selectedNode(rig->selection) ==
                                 NodeSelection{scene, owner, parent} &&
                             rig->document->revision() == revision + 3u,
                         "Add Light Redo restores exact IDs and selection");
      }
    }
  }
  return passed;
}

bool testDuplicateDeepAndD20() {
  auto rig = makeRig("feature_preservation.gltf");
  if (!expect(rig != nullptr, "deep Duplicate fixture loads"))
    return false;
  const SceneId scene = sceneAt(*rig->document, 0);
  const NodeId original = nodeAt(*rig->document, 0);
  const auto source = occurrence(*rig, scene, original);
  if (!expect(source.has_value(), "deep Duplicate occurrence captures"))
    return false;
  const std::size_t nodesBefore =
      rig->document->objectCount(EditorObjectKind::node);
  const std::size_t skinsBefore =
      rig->document->objectCount(EditorObjectKind::skin);
  const std::size_t animationsBefore =
      rig->document->objectCount(EditorObjectKind::animation);
  const std::uint64_t revision = rig->document->revision();
  const HistoryResult duplicate = executePrepared(
      *rig, prepareDuplicateNodeCommand(
                rig->context,
                DuplicateNodeRequest{*source, std::nullopt,
                                     SceneOccurrencePolicy::clickedScene}));
  const auto selected = selectedNode(rig->selection);
  if (!expect(duplicate.status == HistoryStatus::pushed && selected,
              "deep Duplicate executes and selects its copy"))
    return false;
  const NodeId copy = selected->node;
  bool passed = expect(
      copy != original && selected->scene == scene && !selected->parent &&
          rig->document->hasNodeOccurrence(scene, copy, std::nullopt) &&
          rig->document->objectCount(EditorObjectKind::node) > nodesBefore &&
          rig->document->objectCount(EditorObjectKind::skin) > skinsBefore &&
          rig->document->objectCount(EditorObjectKind::animation) ==
              animationsBefore &&
          rig->document->revision() == revision + 1u,
      "deep Duplicate retains copied nodes, skin/joints, and animation "
      "records");
  const HistoryInspection inspected =
      EditorStateInspector::inspect(rig->history);
  passed &= expect(
      inspected.undoOldestToNewest.size() == 1u &&
          inspected.undoOldestToNewest[0].description ==
              "Duplicate 'Skinned Morph Mesh'" &&
          completeStructuralInspection(
              inspected.undoOldestToNewest[0].retainedCommand,
              EditorCommandKind::duplicateNode, CommandLifecycleState::executed,
              1u),
      "deep Duplicate retained inspection includes independent directions");
  const std::size_t copiedNodes =
      rig->document->objectCount(EditorObjectKind::node) - nodesBefore;
  passed &=
      expect(rig->history.undo().status == HistoryStatus::moved &&
                 !rig->document->contains(copy.untyped()) &&
                 selectedNode(rig->selection) ==
                     NodeSelection{scene, original, std::nullopt} &&
                 rig->document->revision() == revision + 2u,
             "deep Duplicate Undo tombstones copies and selects original");
  const auto unrelated = directAddNode(*rig, scene, "Later Root");
  if (!expect(unrelated.has_value(), "later root survives Duplicate history"))
    return false;
  passed &= expect(
      rig->history.redo().status == HistoryStatus::moved &&
          rig->document->contains(copy.untyped()) &&
          selectedNode(rig->selection) ==
              NodeSelection{scene, copy, std::nullopt} &&
          rig->document->objectCount(EditorObjectKind::node) ==
              nodesBefore + copiedNodes + 1u &&
          rig->document->sceneRootNodeIds(scene).back() == *unrelated,
      "deep Duplicate Redo resurrects exact copy IDs around foreign root");

  auto lightRig = makeRig("feature_preservation.gltf");
  if (!expect(lightRig != nullptr, "light attachment Duplicate fixture loads"))
    return false;
  const SceneId lightScene = sceneAt(*lightRig->document, 0);
  const NodeId lightOwner = nodeAt(*lightRig->document, 1);
  const auto lightSource = occurrence(*lightRig, lightScene, lightOwner);
  if (!expect(lightSource.has_value(), "light owner occurrence captures"))
    return false;
  const std::size_t lightDefinitions =
      lightRig->document->objectCount(EditorObjectKind::light);
  const std::string lightExtension(
      lightRig->document->objectExtensionsJson(lightOwner.untyped()));
  const HistoryResult lightDuplicate = executePrepared(
      *lightRig,
      prepareDuplicateNodeCommand(
          lightRig->context,
          DuplicateNodeRequest{*lightSource, std::nullopt,
                               SceneOccurrencePolicy::clickedScene}));
  const auto lightCopySelection = selectedNode(lightRig->selection);
  passed &= expect(
      lightDuplicate.status == HistoryStatus::pushed && lightCopySelection &&
          lightRig->document->objectCount(EditorObjectKind::light) ==
              lightDefinitions &&
          lightRig->document->objectExtensionsJson(
              lightCopySelection->node.untyped()) == lightExtension,
      "Duplicate copies a light attachment without cloning its definition");

  for (const SceneOccurrencePolicy policy :
       {SceneOccurrencePolicy::clickedScene,
        SceneOccurrencePolicy::allScenes}) {
    auto d20 = makeRig("multi_scene_oracle.gltf");
    if (!expect(d20 != nullptr, "D20 Duplicate fixture loads"))
      return false;
    const SceneId scene0 = sceneAt(*d20->document, 0);
    const SceneId scene1 = sceneAt(*d20->document, 1);
    const NodeId shared = nodeAt(*d20->document, 6);
    const auto sharedOccurrence = occurrence(*d20, scene0, shared);
    if (!expect(sharedOccurrence.has_value(), "D20 shared occurrence captures"))
      return false;
    const auto roots0 = d20->document->sceneRootNodeIds(scene0);
    const auto roots1 = d20->document->sceneRootNodeIds(scene1);
    const HistoryResult result = executePrepared(
        *d20, prepareDuplicateNodeCommand(
                  d20->context, DuplicateNodeRequest{*sharedOccurrence,
                                                     std::nullopt, policy}));
    const auto copySelection = selectedNode(d20->selection);
    if (!expect(result.status == HistoryStatus::pushed && copySelection,
                "D20 Duplicate command executes"))
      return false;
    const NodeId d20Copy = copySelection->node;
    const bool clicked = policy == SceneOccurrencePolicy::clickedScene;
    passed &= expect(
        d20->document->sceneRootNodeIds(scene0).size() == roots0.size() + 1u &&
            d20->document->sceneRootNodeIds(scene0).back() == d20Copy &&
            (clicked ? d20->document->sceneRootNodeIds(scene1) == roots1
                     : d20->document->sceneRootNodeIds(scene1).size() ==
                           roots1.size() + 1u) &&
            (clicked ||
             d20->document->sceneRootNodeIds(scene1).back() == d20Copy),
        "D20 clicked/all-scenes Duplicate routes every scene exactly");
    passed &= expect(d20->document->selectCurrentScene(scene1),
                     "D20 switches current scene before Undo");
    passed &= expect(
        d20->history.undo().status == HistoryStatus::moved &&
            d20->document->sceneRootNodeIds(scene0) == roots0 &&
            d20->document->sceneRootNodeIds(scene1) == roots1 &&
            d20->document->currentSceneId() == scene1 &&
            selectedNode(d20->selection) ==
                NodeSelection{scene0, shared, std::nullopt},
        "D20 Undo uses captured occurrence without rerouting current scene");
    passed &= expect(
        d20->history.redo().status == HistoryStatus::moved &&
            d20->document->currentSceneId() == scene1 &&
            selectedNode(d20->selection) ==
                NodeSelection{scene0, d20Copy, std::nullopt},
        "D20 Redo restores the same copy and preserves live current scene");
  }
  return passed;
}

bool testDeleteAndUniqueRootEnvelope() {
  auto rig = makeRig("feature_preservation.gltf");
  if (!expect(rig != nullptr, "ordinary Delete fixture loads"))
    return false;
  const SceneId scene = sceneAt(*rig->document, 0);
  const NodeId target = nodeAt(*rig->document, 1);
  const LightId light = lightAt(*rig->document, 0);
  const auto targetOccurrence = occurrence(*rig, scene, target);
  if (!expect(targetOccurrence && selectNode(*rig, scene, target, std::nullopt),
              "ordinary Delete occurrence and selection capture"))
    return false;
  const std::string attachment(
      rig->document->objectExtensionsJson(target.untyped()));
  const std::size_t lightsBefore =
      rig->document->objectCount(EditorObjectKind::light);
  const std::uint64_t revision = rig->document->revision();
  const HistoryResult execute = executePrepared(
      *rig, prepareDeleteNodeCommand(
                rig->context,
                DeleteNodeRequest{*targetOccurrence,
                                  SceneOccurrencePolicy::clickedScene}));
  bool passed = expect(
      execute.status == HistoryStatus::pushed &&
          !rig->document->contains(target.untyped()) &&
          rig->document->contains(light.untyped()) &&
          rig->document->objectCount(EditorObjectKind::light) == lightsBefore &&
          lightAt(*rig->document, 0) == light &&
          selectionIsEmpty(rig->selection) &&
          rig->document->revision() == revision + 1u,
      "ordinary Delete removes its node but preserves the light definition");
  const HistoryInspection inspection =
      EditorStateInspector::inspect(rig->history);
  passed &=
      expect(inspection.undoOldestToNewest.size() == 1u &&
                 inspection.undoOldestToNewest[0].description ==
                     "Delete 'Camera And Light'" &&
                 completeStructuralInspection(
                     inspection.undoOldestToNewest[0].retainedCommand,
                     EditorCommandKind::deleteNode,
                     CommandLifecycleState::executed, 1u),
             "ordinary Delete retains complete directions and description");
  const auto deletedSlot = slotFor(*rig->document, target.untyped());
  passed &= expect(deletedSlot && !deletedSlot->live &&
                       deletedSlot->generation == target.generation() + 1u,
                   "ordinary Delete publishes the exact tombstone");
  passed &= expect(rig->history.undo().status == HistoryStatus::moved &&
                       rig->document->contains(target.untyped()) &&
                       rig->document->contains(light.untyped()) &&
                       rig->document->objectExtensionsJson(target.untyped()) ==
                           attachment &&
                       selectedNode(rig->selection) ==
                           NodeSelection{scene, target, std::nullopt} &&
                       rig->document->revision() == revision + 2u,
                   "ordinary Delete Undo restores exact node/light attachment "
                   "and selection");
  passed &=
      expect(rig->history.redo().status == HistoryStatus::moved &&
                 !rig->document->contains(target.untyped()) &&
                 rig->document->contains(light.untyped()) &&
                 selectionIsEmpty(rig->selection) &&
                 rig->document->revision() == revision + 3u,
             "ordinary Delete Redo reuses its tombstone and clears selection");

  auto unique = makeRig();
  if (!expect(unique != nullptr, "unique-root Delete fixture loads"))
    return false;
  const SceneId uniqueScene = sceneAt(*unique->document, 0);
  const NodeId root = nodeAt(*unique->document, 0);
  HistoryResult seed =
      executePrepared(*unique, prepareRenameNodeCommand(unique->context, root,
                                                        "Temporary Root"));
  if (!expect(seed.status == HistoryStatus::pushed &&
                  unique->history.undo().status == HistoryStatus::moved &&
                  unique->history.canRedo(),
              "unique-root envelope starts with seeded redo"))
    return false;
  if (!expect(selectNode(*unique, uniqueScene, root, std::nullopt),
              "unique-root original selection captures"))
    return false;
  const EditorSelectionSnapshot originalSelection =
      unique->selection.snapshot();
  const auto rootOccurrence = occurrence(*unique, uniqueScene, root);
  if (!expect(rootOccurrence.has_value(), "unique-root occurrence captures"))
    return false;
  const ExactDocumentSnapshot uniqueDocumentBefore =
      exactDocumentSnapshot(*unique->document, unique->commands);
  const HistoryResult envelope = executePrepared(
      *unique, prepareDeleteNodeCommand(
                   unique->context,
                   DeleteNodeRequest{*rootOccurrence,
                                     SceneOccurrencePolicy::clickedScene}));
  passed &= expect(
      envelope.status == HistoryStatus::pushed &&
          envelope.command.status == CommandStatus::historyEnvelopeNoop &&
          envelope.command.error == MutationErrorCode::uniqueRootDeleteNoop &&
          exactDocumentSnapshot(*unique->document, unique->commands) ==
              uniqueDocumentBefore &&
          selectionIsEmpty(unique->selection) &&
          unique->history.undoCount() == 1u &&
          unique->history.redoCount() == 0u &&
          unique->history.undoDescription() == "Delete 'Root'",
      "unique-root Delete changes only selection/history and clears redo");
  const HistoryInspection envelopeInspection =
      EditorStateInspector::inspect(unique->history);
  passed &= expect(
      envelopeInspection.undoOldestToNewest.size() == 1u &&
          envelopeInspection.undoOldestToNewest[0].retainedCommand.kind ==
              EditorCommandKind::deleteNode &&
          envelopeInspection.undoOldestToNewest[0].retainedCommand.lifecycle ==
              CommandLifecycleState::executed &&
          envelopeInspection.undoOldestToNewest[0]
                  .retainedCommand.stateRevision == 1u &&
          !envelopeInspection.undoOldestToNewest[0]
               .retainedCommand.undoStructuralSide.present &&
          !envelopeInspection.undoOldestToNewest[0]
               .retainedCommand.redoStructuralSide.present &&
          detail::retainedCommandInspectionHashMatches(
              envelopeInspection.undoOldestToNewest[0].retainedCommand),
      "unique-root envelope retains canonical no-delta state");
  passed &=
      expect(unique->history.undo().status == HistoryStatus::moved &&
                 unique->selection.snapshot() == originalSelection &&
                 exactDocumentSnapshot(*unique->document, unique->commands) ==
                     uniqueDocumentBefore,
             "unique-root Undo restores the exact original selection only");
  passed &=
      expect(unique->history.redo().status == HistoryStatus::moved &&
                 selectionIsEmpty(unique->selection) &&
                 exactDocumentSnapshot(*unique->document, unique->commands) ==
                     uniqueDocumentBefore,
             "unique-root Redo clears selection without document publication");
  return passed;
}

bool testReparentAndAnchoredOrdering() {
  bool passed = true;
  for (const std::size_t sceneIndex : {0u, 1u}) {
    auto envelope = makeRig("multi_scene_oracle.gltf");
    if (!expect(envelope != nullptr, "root Move envelope fixture loads"))
      return false;
    const SceneId scene = sceneAt(*envelope->document, sceneIndex);
    const NodeId root = sceneIndex == 0u ? nodeAt(*envelope->document, 1)
                                         : nodeAt(*envelope->document, 4);
    if (!expect(occurrence(*envelope, scene, root) &&
                    selectNode(*envelope, scene, root, std::nullopt),
                "root Move envelope occurrence and selection capture"))
      return false;
    HistoryResult seed =
        executePrepared(*envelope, prepareRenameNodeCommand(envelope->context,
                                                            root, "Move Seed"));
    if (!expect(seed.status == HistoryStatus::pushed &&
                    envelope->history.undo().status == HistoryStatus::moved,
                "root Move envelope seeds redo"))
      return false;
    const auto source = occurrence(*envelope, scene, root);
    if (!expect(source.has_value(),
                "root Move envelope occurrence refreshes after seeded Undo"))
      return false;
    const ExactDocumentSnapshot before =
        exactDocumentSnapshot(*envelope->document, envelope->commands);
    const EditorSelectionSnapshot selection = envelope->selection.snapshot();
    const HistoryResult result = executePrepared(
        *envelope,
        prepareReparentNodeCommand(
            envelope->context,
            ReparentNodeRequest{*source, std::nullopt,
                                SceneOccurrencePolicy::clickedScene}));
    passed &= expect(
        result.status == HistoryStatus::pushed &&
            result.command.status == CommandStatus::historyEnvelopeNoop &&
            result.command.error == MutationErrorCode::noOp &&
            exactDocumentSnapshot(*envelope->document, envelope->commands) ==
                before &&
            envelope->selection.snapshot() == selection &&
            envelope->history.undoCount() == 1u &&
            !envelope->history.canRedo() &&
            envelope->history.undoDescription().starts_with("Move '") &&
            envelope->history.undo().status == HistoryStatus::moved &&
            envelope->selection.snapshot() == selection &&
            envelope->history.redo().status == HistoryStatus::moved &&
            envelope->selection.snapshot() == selection,
        "current/non-current root Move envelope changes history only");
  }

  auto sameParent = makeRig("multi_scene_oracle.gltf");
  if (!expect(sameParent != nullptr, "same-parent Move fixture loads"))
    return false;
  const SceneId sameScene = sceneAt(*sameParent->document, 0);
  const NodeId parent = nodeAt(*sameParent->document, 0);
  const NodeId child = nodeAt(*sameParent->document, 2);
  const auto childOccurrence = occurrence(*sameParent, sameScene, child);
  const auto parentOccurrence = occurrence(*sameParent, sameScene, parent);
  if (!expect(childOccurrence && parentOccurrence,
              "same-parent Move occurrences capture"))
    return false;
  const ExactDocumentSnapshot sameBefore =
      exactDocumentSnapshot(*sameParent->document, sameParent->commands);
  const HistoryResult sameResult = executePrepared(
      *sameParent,
      prepareReparentNodeCommand(
          sameParent->context,
          ReparentNodeRequest{*childOccurrence, *parentOccurrence,
                              SceneOccurrencePolicy::clickedScene}));
  passed &= expect(
      sameResult.status == HistoryStatus::pushed &&
          sameResult.command.status == CommandStatus::historyEnvelopeNoop &&
          exactDocumentSnapshot(*sameParent->document, sameParent->commands) ==
              sameBefore,
      "same-parent Move is the authorized zero-revision envelope");

  auto cross = makeRig("multi_scene_oracle.gltf");
  if (!expect(cross != nullptr, "cross-parent Move fixture loads"))
    return false;
  const SceneId scene0 = sceneAt(*cross->document, 0);
  const SceneId scene1 = sceneAt(*cross->document, 1);
  const NodeId oldParent = nodeAt(*cross->document, 0);
  const NodeId newParent = nodeAt(*cross->document, 1);
  const NodeId p0 = cross->document->nodeChildIds(oldParent).front();
  const auto moved = directAddNode(*cross, scene0, "X", oldParent);
  const auto p1 = directAddNode(*cross, scene0, "p1", oldParent);
  const auto q0 = directAddNode(*cross, scene0, "q0", newParent);
  if (!expect(moved && p1 && q0,
              "cross-parent Move source/anchor nodes append"))
    return false;
  const auto moveSource = occurrence(*cross, scene0, *moved);
  const auto moveTarget = occurrence(*cross, scene0, newParent);
  if (!expect(moveSource && moveTarget &&
                  selectNode(*cross, scene0, *moved, oldParent),
              "cross-parent Move occurrences and selection capture"))
    return false;
  const EditorSelectionSnapshot moveSelection = cross->selection.snapshot();
  const std::uint64_t beforeExecute = cross->document->revision();
  const HistoryResult moveResult = executePrepared(
      *cross, prepareReparentNodeCommand(
                  cross->context,
                  ReparentNodeRequest{*moveSource, *moveTarget,
                                      SceneOccurrencePolicy::clickedScene}));
  passed &= expect(moveResult.status == HistoryStatus::pushed,
                   "cross-parent Execute pushes history");
  passed &= expect(cross->document->nodeChildIds(oldParent) ==
                           std::vector<NodeId>{p0, *p1} &&
                       cross->document->nodeChildIds(newParent) ==
                           std::vector<NodeId>{*q0, *moved},
                   "cross-parent Execute appends at target tail");
  passed &= expect(cross->selection.snapshot() == moveSelection,
                   "cross-parent Execute preserves exact selection");
  passed &= expect(cross->document->revision() == beforeExecute + 1u,
                   "cross-parent Execute advances one revision");
  const HistoryInspection crossInspection =
      EditorStateInspector::inspect(cross->history);
  passed &= expect(
      crossInspection.undoOldestToNewest.size() == 1u &&
          crossInspection.undoOldestToNewest[0].description == "Move 'X'" &&
          completeStructuralInspection(
              crossInspection.undoOldestToNewest[0].retainedCommand,
              EditorCommandKind::reparentNode, CommandLifecycleState::executed,
              1u) &&
          crossInspection.undoOldestToNewest[0]
                  .retainedCommand.executeSelection == moveSelection &&
          crossInspection.undoOldestToNewest[0].retainedCommand.undoSelection ==
              moveSelection &&
          crossInspection.undoOldestToNewest[0].retainedCommand.redoSelection ==
              moveSelection,
      "cross-parent Move inspection retains unchanged selection envelope");
  const auto y = directAddNode(*cross, scene0, "Y", oldParent);
  const auto z = directAddNode(*cross, scene0, "Z", newParent);
  if (!expect(y && z && cross->document->selectCurrentScene(scene1),
              "cross-parent foreign tails and current scene publish"))
    return false;
  const std::uint64_t beforeUndo = cross->document->revision();
  const HistoryResult crossUndo = cross->history.undo();
  passed &= expect(crossUndo.status == HistoryStatus::moved,
                   "cross-parent Undo moves the retained entry");
  passed &=
      expect(cross->document->nodeChildIds(oldParent) ==
                     std::vector<NodeId>{p0, *p1, *moved, *y} &&
                 cross->document->nodeChildIds(newParent) ==
                     std::vector<NodeId>{*q0, *z},
             "cross-parent Undo uses append-tail anchors before foreign tails");
  passed &= expect(
      cross->selection.snapshot() == moveSelection &&
          cross->document->currentSceneId() == scene1 &&
          cross->document->revision() == beforeUndo + 1u,
      "cross-parent Undo preserves selection/current scene and advances once");
  const HistoryResult crossRedo = cross->history.redo();
  passed &= expect(crossRedo.status == HistoryStatus::moved,
                   "cross-parent Redo moves the retained entry");
  passed &= expect(cross->document->nodeChildIds(oldParent) ==
                           std::vector<NodeId>{p0, *p1, *y} &&
                       cross->document->nodeChildIds(newParent) ==
                           std::vector<NodeId>{*q0, *moved, *z},
                   "cross-parent Redo preserves foreign order");
  passed &= expect(cross->selection.snapshot() == moveSelection &&
                       cross->document->currentSceneId() == scene1,
                   "cross-parent Redo preserves selection and current scene");

  return passed;
}

bool testAnchoredComposerMatrix() {
  bool passed = true;

  {
    auto rig = makeRig();
    if (!expect(rig != nullptr, "two-anchor Delete matrix fixture loads"))
      return false;
    const SceneId scene = sceneAt(*rig->document, 0);
    const NodeId parent = nodeAt(*rig->document, 0);
    const auto a = directAddNode(*rig, scene, "two anchor a", parent);
    const auto x = directAddNode(*rig, scene, "two anchor X", parent);
    const auto b = directAddNode(*rig, scene, "two anchor b", parent);
    if (!expect(a && x && b && selectNode(*rig, scene, *x, parent),
                "two-anchor Delete baseline and selection publish"))
      return false;
    const EditorSelectionSnapshot originalSelection = rig->selection.snapshot();
    const auto target = occurrence(*rig, scene, *x);
    if (!expect(target.has_value(), "two-anchor Delete occurrence captures"))
      return false;
    const HistoryResult execute = executePrepared(
        *rig,
        prepareDeleteNodeCommand(
            rig->context,
            DeleteNodeRequest{*target, SceneOccurrencePolicy::clickedScene}));
    const auto later = directAddNode(*rig, scene, "two anchor later", parent);
    if (!expect(execute.status == HistoryStatus::pushed && later &&
                    selectionIsEmpty(rig->selection),
                "two-anchor Delete executes before later foreign insert"))
      return false;
    const HistoryInspection retained =
        EditorStateInspector::inspect(rig->history);
    passed &= expect(
        retained.undoOldestToNewest.size() == 1u &&
            retained.undoOldestToNewest[0].retainedCommand.kind ==
                EditorCommandKind::deleteNode &&
            retained.undoOldestToNewest[0].retainedCommand.executeSelection ==
                EditorSelectionSnapshot::empty() &&
            retained.undoOldestToNewest[0].retainedCommand.undoSelection ==
                originalSelection,
        "two-anchor Delete retains the clear/restore selection envelope");
    passed &= expect(
        rig->document->nodeChildIds(parent) ==
                std::vector<NodeId>{*a, *b, *later} &&
            rig->history.undo().status == HistoryStatus::moved &&
            rig->document->nodeChildIds(parent) ==
                std::vector<NodeId>{*a, *x, *b, *later} &&
            nodeRegistryIds(*rig->document) ==
                std::vector<NodeId>{parent, *a, *x, *b, *later} &&
            rig->selection.snapshot() == originalSelection,
        "anchored composer matrix: Delete two-anchor restores [a,X,b,Y]");
    passed &= expect(
        rig->document->objectName(a->untyped()) == "two anchor a" &&
            rig->document->objectName(b->untyped()) == "two anchor b" &&
            rig->document->objectName(later->untyped()) == "two anchor later" &&
            rig->history.redo().status == HistoryStatus::moved &&
            rig->document->nodeChildIds(parent) ==
                std::vector<NodeId>{*a, *b, *later} &&
            selectionIsEmpty(rig->selection),
        "Delete preserves baseline and later foreign IDs/records on Redo");

    const auto aChild = occurrence(*rig, scene, *a);
    if (!expect(aChild.has_value(),
                "reordered-anchor source occurrence captures"))
      return false;
    const MutationResult moveAOut = rig->commands.reparentNodeAt(
        *aChild, std::nullopt, SceneOccurrencePolicy::clickedScene);
    const auto aRoot = occurrence(*rig, scene, *a);
    const auto parentOccurrence = occurrence(*rig, scene, parent);
    if (!expect(moveAOut.applied() && aRoot && parentOccurrence,
                "reordered-anchor baseline moves out"))
      return false;
    const MutationResult moveABack = rig->commands.reparentNodeAt(
        *aRoot, *parentOccurrence, SceneOccurrencePolicy::clickedScene);
    if (!expect(moveABack.applied() && rig->document->nodeChildIds(parent) ==
                                           std::vector<NodeId>{*b, *later, *a},
                "reordered-anchor baseline moves behind its successor"))
      return false;
    const ExactEditorStateSnapshot conflictBefore = exactEditorStateSnapshot(
        *rig->document, rig->commands, rig->selection, rig->history);
    const HistoryResult rejected = rig->history.undo();
    passed &= expect(
        rejected.status == HistoryStatus::rejected &&
            rejected.command.error == MutationErrorCode::staleTransaction &&
            exactEditorStateSnapshot(*rig->document, rig->commands,
                                     rig->selection,
                                     rig->history) == conflictBefore,
        "anchored composer matrix: reordered anchors reject atomically");
  }

  {
    auto rig = makeRig();
    if (!expect(rig != nullptr, "successor-only Delete matrix fixture loads"))
      return false;
    const SceneId scene = sceneAt(*rig->document, 0);
    const NodeId parent = nodeAt(*rig->document, 0);
    const auto x = directAddNode(*rig, scene, "successor X", parent);
    const auto b = directAddNode(*rig, scene, "successor b", parent);
    if (!expect(x && b && selectNode(*rig, scene, *x, parent),
                "successor-only Delete baseline publishes"))
      return false;
    const auto target = occurrence(*rig, scene, *x);
    if (!expect(target.has_value(),
                "successor-only Delete occurrence captures"))
      return false;
    const HistoryResult execute = executePrepared(
        *rig,
        prepareDeleteNodeCommand(
            rig->context,
            DeleteNodeRequest{*target, SceneOccurrencePolicy::clickedScene}));
    const auto later = directAddNode(*rig, scene, "successor later", parent);
    if (!expect(execute.status == HistoryStatus::pushed && later,
                "successor-only Delete and later foreign insert publish"))
      return false;
    passed &= expect(
        rig->document->nodeChildIds(parent) ==
                std::vector<NodeId>{*b, *later} &&
            rig->history.undo().status == HistoryStatus::moved &&
            rig->document->nodeChildIds(parent) ==
                std::vector<NodeId>{*x, *b, *later} &&
            selectedNode(rig->selection) == NodeSelection{scene, *x, parent},
        "anchored composer matrix: Delete successor-only restores [X,b,Y]");
    passed &= expect(
        rig->document->objectName(b->untyped()) == "successor b" &&
            rig->document->objectName(later->untyped()) == "successor later" &&
            rig->history.redo().status == HistoryStatus::moved &&
            rig->document->nodeChildIds(parent) ==
                std::vector<NodeId>{*b, *later} &&
            selectionIsEmpty(rig->selection),
        "successor-only Delete preserves foreign records and Redo envelope");
  }

  {
    auto rig = makeRig();
    if (!expect(rig != nullptr, "predecessor-only Add matrix fixture loads"))
      return false;
    const SceneId scene = sceneAt(*rig->document, 0);
    const NodeId parent = nodeAt(*rig->document, 0);
    const auto a = directAddNode(*rig, scene, "predecessor a", parent);
    if (!expect(a.has_value(), "predecessor-only Add baseline publishes"))
      return false;
    const HistoryResult execute = executePrepared(
        *rig,
        prepareAddChildNodeCommand(
            rig->context, AddChildNodeRequest{scene, parent, "predecessor X"}));
    const auto selected = selectedNode(rig->selection);
    if (!expect(execute.status == HistoryStatus::pushed && selected,
                "predecessor-only Add executes and selects X"))
      return false;
    const NodeId x = selected->node;
    if (!expect(rig->document->nodeChildIds(parent) ==
                        std::vector<NodeId>{*a, x} &&
                    rig->history.undo().status == HistoryStatus::moved &&
                    selectionIsEmpty(rig->selection),
                "predecessor-only Add Undo clears its envelope"))
      return false;
    const auto later = directAddNode(*rig, scene, "predecessor later", parent);
    if (!expect(later.has_value(),
                "predecessor-only Add later foreign child publishes"))
      return false;
    passed &= expect(
        rig->history.redo().status == HistoryStatus::moved &&
            rig->document->nodeChildIds(parent) ==
                std::vector<NodeId>{*a, x, *later} &&
            nodeRegistryIds(*rig->document) ==
                std::vector<NodeId>{parent, *a, x, *later} &&
            selectedNode(rig->selection) == NodeSelection{scene, x, parent} &&
            rig->document->objectName(a->untyped()) == "predecessor a" &&
            rig->document->objectName(later->untyped()) == "predecessor later",
        "anchored composer matrix: Add predecessor-only restores [a,X,Y]");
  }

  {
    auto rig = makeRig();
    if (!expect(rig != nullptr, "no-anchor Add matrix fixture loads"))
      return false;
    const SceneId scene = sceneAt(*rig->document, 0);
    const NodeId parent = nodeAt(*rig->document, 0);
    const HistoryResult execute = executePrepared(
        *rig,
        prepareAddChildNodeCommand(
            rig->context, AddChildNodeRequest{scene, parent, "no anchor X"}));
    const auto selected = selectedNode(rig->selection);
    if (!expect(execute.status == HistoryStatus::pushed && selected,
                "no-anchor Add executes and selects X"))
      return false;
    const NodeId x = selected->node;
    if (!expect(rig->history.undo().status == HistoryStatus::moved &&
                    rig->document->nodeChildIds(parent).empty() &&
                    selectionIsEmpty(rig->selection),
                "no-anchor Add Undo removes X exactly"))
      return false;
    const auto later = directAddNode(*rig, scene, "no anchor later", parent);
    if (!expect(later.has_value(), "no-anchor later foreign child publishes"))
      return false;
    passed &= expect(
        rig->history.redo().status == HistoryStatus::moved &&
            rig->document->nodeChildIds(parent) ==
                std::vector<NodeId>{x, *later} &&
            selectedNode(rig->selection) == NodeSelection{scene, x, parent} &&
            rig->document->objectName(later->untyped()) == "no anchor later",
        "anchored composer matrix: Add no-anchor uses boundary zero [X,Y]");
  }

  {
    auto rig = makeRig();
    if (!expect(rig != nullptr, "no-anchor Duplicate matrix fixture loads"))
      return false;
    const SceneId scene = sceneAt(*rig->document, 0);
    const NodeId targetParent = nodeAt(*rig->document, 0);
    const auto source = directAddNode(*rig, scene, "duplicate source");
    const auto sourceOccurrence =
        source ? occurrence(*rig, scene, *source) : std::nullopt;
    const auto parentOccurrence = occurrence(*rig, scene, targetParent);
    if (!expect(source && sourceOccurrence && parentOccurrence,
                "no-anchor Duplicate source/target occurrences capture"))
      return false;
    const HistoryResult execute = executePrepared(
        *rig, prepareDuplicateNodeCommand(
                  rig->context,
                  DuplicateNodeRequest{*sourceOccurrence, *parentOccurrence,
                                       SceneOccurrencePolicy::clickedScene}));
    const auto selectedCopy = selectedNode(rig->selection);
    if (!expect(execute.status == HistoryStatus::pushed && selectedCopy &&
                    selectedCopy->parent == targetParent,
                "no-anchor Duplicate executes and selects its copy"))
      return false;
    const NodeId copy = selectedCopy->node;
    if (!expect(rig->history.undo().status == HistoryStatus::moved &&
                    rig->document->nodeChildIds(targetParent).empty() &&
                    selectedNode(rig->selection) ==
                        NodeSelection{scene, *source, std::nullopt},
                "no-anchor Duplicate Undo restores original selection"))
      return false;
    const auto later =
        directAddNode(*rig, scene, "duplicate later", targetParent);
    if (!expect(later.has_value(),
                "no-anchor Duplicate later foreign child publishes"))
      return false;
    passed &= expect(
        rig->history.redo().status == HistoryStatus::moved &&
            rig->document->nodeChildIds(targetParent) ==
                std::vector<NodeId>{copy, *later} &&
            selectedNode(rig->selection) ==
                NodeSelection{scene, copy, targetParent} &&
            rig->document->contains(source->untyped()) &&
            rig->document->objectName(source->untyped()) ==
                "duplicate source" &&
            rig->document->objectName(later->untyped()) == "duplicate later",
        "anchored composer matrix: Duplicate no-anchor restores copy before Y");
  }

  {
    auto rig = makeRig();
    if (!expect(rig != nullptr,
                "fallback-anchor Add Light matrix fixture loads"))
      return false;
    const SceneId scene = sceneAt(*rig->document, 0);
    const NodeId parent = nodeAt(*rig->document, 0);
    LightProperties baselineProperties;
    baselineProperties.type = "point";
    const MutationResult baseline0 =
        rig->commands.createLight(scene, parent, "fallback owner 0",
                                  "fallback light 0", baselineProperties);
    baselineProperties.type = "directional";
    const MutationResult baseline1 =
        rig->commands.createLight(scene, parent, "fallback owner 1",
                                  "fallback light 1", baselineProperties);
    const auto baselineOwner0Id =
        createdId(baseline0.remap, EditorObjectKind::node);
    const auto baselineLight0Id =
        createdId(baseline0.remap, EditorObjectKind::light);
    const auto baselineOwner1Id =
        createdId(baseline1.remap, EditorObjectKind::node);
    const auto baselineLight1Id =
        createdId(baseline1.remap, EditorObjectKind::light);
    if (!expect(baseline0.applied() && baseline1.applied() &&
                    baselineOwner0Id && baselineLight0Id && baselineOwner1Id &&
                    baselineLight1Id,
                "fallback-anchor baseline node/light records publish"))
      return false;
    const NodeId baselineOwner0 = *NodeId::from(*baselineOwner0Id);
    const LightId baselineLight0 = *LightId::from(*baselineLight0Id);
    const NodeId baselineOwner1 = *NodeId::from(*baselineOwner1Id);
    const LightId baselineLight1 = *LightId::from(*baselineLight1Id);

    LightProperties addedProperties;
    addedProperties.type = "spot";
    addedProperties.intensity = 7.0;
    const HistoryResult execute = executePrepared(
        *rig, prepareAddLightCommand(
                  rig->context,
                  AddLightRequest{scene, parent, "fallback owner X",
                                  "fallback light X", addedProperties}));
    const auto selectedOwner = selectedNode(rig->selection);
    if (!expect(execute.status == HistoryStatus::pushed && selectedOwner,
                "fallback-anchor Add Light executes and selects owner"))
      return false;
    const NodeId owner = selectedOwner->node;
    const LightId light = lightAt(*rig->document, 2u);
    if (!expect(rig->document->nodeChildIds(parent) ==
                        std::vector<NodeId>{baselineOwner0, baselineOwner1,
                                            owner} &&
                    lightRegistryIds(*rig->document) ==
                        std::vector<LightId>{baselineLight0, baselineLight1,
                                             light} &&
                    rig->history.undo().status == HistoryStatus::moved &&
                    selectionIsEmpty(rig->selection),
                "fallback-anchor Add Light Undo clears both owned records"))
      return false;

    const MutationResult removeNearestLight =
        rig->commands.deleteLight(baselineLight1);
    const auto nearestOwnerOccurrence = occurrence(*rig, scene, baselineOwner1);
    const MutationResult moveNearestOwner =
        nearestOwnerOccurrence
            ? rig->commands.reparentNodeAt(*nearestOwnerOccurrence,
                                           std::nullopt,
                                           SceneOccurrencePolicy::clickedScene)
            : MutationResult{};
    const MutationResult renameFallbackOwner = rig->commands.rename(
        baselineOwner0.untyped(), "fallback owner 0 edited");
    const MutationResult renameFallbackLight = rig->commands.rename(
        baselineLight0.untyped(), "fallback light 0 edited");
    LightProperties laterProperties;
    laterProperties.type = "point";
    const MutationResult later =
        rig->commands.createLight(scene, parent, "fallback later owner",
                                  "fallback later light", laterProperties);
    const auto laterOwnerId = createdId(later.remap, EditorObjectKind::node);
    const auto laterLightId = createdId(later.remap, EditorObjectKind::light);
    if (!expect(removeNearestLight.applied() && moveNearestOwner.applied() &&
                    renameFallbackOwner.applied() &&
                    renameFallbackLight.applied() && later.applied() &&
                    laterOwnerId && laterLightId,
                "nearest anchors disappear and fallback/later records publish"))
      return false;
    const NodeId laterOwner = *NodeId::from(*laterOwnerId);
    const LightId laterLight = *LightId::from(*laterLightId);
    const std::uint64_t beforeRedo = rig->document->revision();
    passed &= expect(
        rig->history.redo().status == HistoryStatus::moved &&
            rig->document->revision() == beforeRedo + 1u &&
            rig->document->nodeChildIds(parent) ==
                std::vector<NodeId>{baselineOwner0, owner, laterOwner} &&
            lightRegistryIds(*rig->document) ==
                std::vector<LightId>{baselineLight0, light, laterLight} &&
            rig->document->sceneRootNodeIds(scene) ==
                std::vector<NodeId>{parent, baselineOwner1} &&
            rig->document->objectName(baselineOwner0.untyped()) ==
                "fallback owner 0 edited" &&
            rig->document->objectName(baselineLight0.untyped()) ==
                "fallback light 0 edited" &&
            rig->document->objectName(laterOwner.untyped()) ==
                "fallback later owner" &&
            rig->document->objectName(laterLight.untyped()) ==
                "fallback later light" &&
            selectedNode(rig->selection) == NodeSelection{scene, owner, parent},
        "anchored composer matrix: Add Light missing-nearest uses fallback "
        "anchors in node and light registries");
  }

  {
    auto rig = makeRig();
    if (!expect(rig != nullptr,
                "same-boundary deterministic tie matrix fixture loads"))
      return false;
    const SceneId scene = sceneAt(*rig->document, 0);
    const NodeId originalRoot = nodeAt(*rig->document, 0);
    const auto a = directAddNode(*rig, scene, "tie a");
    const auto x = directAddNode(*rig, scene, "tie X");
    const auto b = directAddNode(*rig, scene, "tie b");
    const auto u = x ? directAddNode(*rig, scene, "tie U", *x) : std::nullopt;
    const auto c = directAddNode(*rig, scene, "tie c");
    if (!expect(a && x && b && u && c &&
                    nodeRegistryIds(*rig->document) ==
                        std::vector<NodeId>{originalRoot, *a, *x, *b, *u, *c} &&
                    selectNode(*rig, scene, *x, std::nullopt),
                "same-boundary tie captures noncontiguous owned runs X/U"))
      return false;
    const auto target = occurrence(*rig, scene, *x);
    if (!expect(target.has_value(),
                "same-boundary tie Delete occurrence captures"))
      return false;
    const HistoryResult execute = executePrepared(
        *rig,
        prepareDeleteNodeCommand(
            rig->context,
            DeleteNodeRequest{*target, SceneOccurrencePolicy::clickedScene}));
    const auto removeSeparator = directDeleteNode(*rig, scene, *b);
    const auto later = directAddNode(*rig, scene, "tie later");
    const MutationResult renameA =
        rig->commands.rename(a->untyped(), "tie a edited");
    const MutationResult renameC =
        rig->commands.rename(c->untyped(), "tie c edited");
    if (!expect(execute.status == HistoryStatus::pushed && removeSeparator &&
                    removeSeparator->applied() && later && renameA.applied() &&
                    renameC.applied(),
                "same-boundary separator removal and foreign edits publish"))
      return false;
    const std::uint64_t beforeUndo = rig->document->revision();
    passed &= expect(
        rig->history.undo().status == HistoryStatus::moved &&
            rig->document->revision() == beforeUndo + 1u &&
            nodeRegistryIds(*rig->document) ==
                std::vector<NodeId>{originalRoot, *a, *x, *u, *c, *later} &&
            rig->document->sceneRootNodeIds(scene) ==
                std::vector<NodeId>{originalRoot, *a, *x, *c, *later} &&
            rig->document->nodeChildIds(*x) == std::vector<NodeId>{*u} &&
            selectedNode(rig->selection) ==
                NodeSelection{scene, *x, std::nullopt} &&
            rig->document->objectName(a->untyped()) == "tie a edited" &&
            rig->document->objectName(c->untyped()) == "tie c edited" &&
            rig->document->objectName(later->untyped()) == "tie later",
        "anchored composer matrix: same-boundary tie emits X then U by target "
        "ordinal and preserves foreign records");
    passed &=
        expect(rig->history.redo().status == HistoryStatus::moved &&
                   nodeRegistryIds(*rig->document) ==
                       std::vector<NodeId>{originalRoot, *a, *c, *later} &&
                   rig->document->sceneRootNodeIds(scene) ==
                       std::vector<NodeId>{originalRoot, *a, *c, *later} &&
                   selectionIsEmpty(rig->selection),
               "same-boundary Delete Redo removes only owned X/U and clears "
               "selection");
  }

  {
    auto rig = makeRig();
    if (!expect(rig != nullptr, "anchored Reparent matrix fixture loads"))
      return false;
    const SceneId scene = sceneAt(*rig->document, 0);
    const NodeId oldParent = nodeAt(*rig->document, 0);
    const auto newParent = directAddNode(*rig, scene, "move target");
    const auto a = directAddNode(*rig, scene, "move a", oldParent);
    const auto x = directAddNode(*rig, scene, "move X", oldParent);
    const auto b = directAddNode(*rig, scene, "move b", oldParent);
    const auto q = newParent ? directAddNode(*rig, scene, "move q", *newParent)
                             : std::nullopt;
    const auto source = x ? occurrence(*rig, scene, *x) : std::nullopt;
    const auto target =
        newParent ? occurrence(*rig, scene, *newParent) : std::nullopt;
    if (!expect(newParent && a && x && b && q && source && target &&
                    selectNode(*rig, scene, *x, oldParent),
                "anchored Reparent baseline and selection publish"))
      return false;
    const EditorSelectionSnapshot selection = rig->selection.snapshot();
    const HistoryResult execute = executePrepared(
        *rig, prepareReparentNodeCommand(
                  rig->context,
                  ReparentNodeRequest{*source, *target,
                                      SceneOccurrencePolicy::clickedScene}));
    const auto y = directAddNode(*rig, scene, "move later old", oldParent);
    const auto z = directAddNode(*rig, scene, "move later new", *newParent);
    if (!expect(execute.status == HistoryStatus::pushed && y && z,
                "anchored Reparent execute and later foreign tails publish"))
      return false;
    passed &= expect(rig->document->nodeChildIds(oldParent) ==
                             std::vector<NodeId>{*a, *b, *y} &&
                         rig->document->nodeChildIds(*newParent) ==
                             std::vector<NodeId>{*q, *x, *z} &&
                         rig->history.undo().status == HistoryStatus::moved &&
                         rig->document->nodeChildIds(oldParent) ==
                             std::vector<NodeId>{*a, *b, *x, *y} &&
                         rig->document->nodeChildIds(*newParent) ==
                             std::vector<NodeId>{*q, *z} &&
                         rig->selection.snapshot() == selection,
                     "anchored composer matrix: Reparent Undo uses append-tail "
                     "predecessor anchor and preserves later foreign tails");
    passed &= expect(
        rig->history.redo().status == HistoryStatus::moved &&
            rig->document->nodeChildIds(oldParent) ==
                std::vector<NodeId>{*a, *b, *y} &&
            rig->document->nodeChildIds(*newParent) ==
                std::vector<NodeId>{*q, *x, *z} &&
            rig->selection.snapshot() == selection &&
            rig->document->objectName(a->untyped()) == "move a" &&
            rig->document->objectName(b->untyped()) == "move b" &&
            rig->document->objectName(y->untyped()) == "move later old" &&
            rig->document->objectName(z->untyped()) == "move later new",
        "anchored composer matrix: Reparent Redo uses predecessor-only target "
        "anchor and preserves all foreign records");
  }

  return passed;
}

bool testFailureAtomicity() {
  bool passed = true;
  {
    ScriptedEditorFaultInjector factoryFault;
    (void)factoryFault.failOnCall(EditorFaultPoint::allocation, 1u);
    auto rig = makeRig();
    if (!expect(rig != nullptr, "factory fault fixture loads"))
      return false;
    const SceneId scene = sceneAt(*rig->document, 0);
    const NodeId root = nodeAt(*rig->document, 0);
    const ExactEditorStateSnapshot before = exactEditorStateSnapshot(
        *rig->document, rig->commands, rig->selection, rig->history);
    const CommandPrepareResult rejected = prepareAddChildNodeCommand(
        rig->context, AddChildNodeRequest{scene, root, "never"}, &factoryFault);
    passed &= expect(
        !rejected.succeeded() &&
            rejected.error == MutationErrorCode::allocationFailed &&
            exactEditorStateSnapshot(*rig->document, rig->commands,
                                     rig->selection, rig->history) == before,
        "hierarchy factory allocation failure is an exact no-write");
  }
  {
    ScriptedEditorFaultInjector preparationFault;
    (void)preparationFault.failOnCall(EditorFaultPoint::allocation, 1u);
    auto rig =
        makeRig("minimal_scene.gltf", kDefaultEditorLimits, &preparationFault);
    if (!expect(rig != nullptr, "preparation fault fixture loads"))
      return false;
    const ExactEditorStateSnapshot before = exactEditorStateSnapshot(
        *rig->document, rig->commands, rig->selection, rig->history);
    const CommandPrepareResult rejected = prepareAddChildNodeCommand(
        rig->context, AddChildNodeRequest{sceneAt(*rig->document, 0),
                                          nodeAt(*rig->document, 0), "never"});
    passed &= expect(
        !rejected.succeeded() &&
            rejected.error == MutationErrorCode::allocationFailed &&
            exactEditorStateSnapshot(*rig->document, rig->commands,
                                     rig->selection, rig->history) == before,
        "structural preparation allocation failure is an exact no-write");
  }
  {
    auto rig = makeRig();
    if (!expect(rig != nullptr, "stale execute fixture loads"))
      return false;
    const SceneId scene = sceneAt(*rig->document, 0);
    const NodeId root = nodeAt(*rig->document, 0);
    CommandPrepareResult prepared = prepareAddChildNodeCommand(
        rig->context, AddChildNodeRequest{scene, root, "Prepared"});
    if (!expect(prepared.succeeded(), "stale Add command prepares"))
      return false;
    const MutationResult foreign =
        rig->commands.rename(root.untyped(), "Foreign Root");
    const ExactEditorStateSnapshot before = exactEditorStateSnapshot(
        *rig->document, rig->commands, rig->selection, rig->history);
    const HistoryResult rejected =
        rig->history.execute(std::move(prepared.command));
    passed &= expect(
        foreign.applied() && rejected.status == HistoryStatus::rejected &&
            rejected.command.error == MutationErrorCode::staleTransaction &&
            exactEditorStateSnapshot(*rig->document, rig->commands,
                                     rig->selection, rig->history) == before,
        "stale structural Execute preserves document, selection, and stacks");
  }
  {
    auto rig = makeRig();
    if (!expect(rig != nullptr, "Undo conflict fixture loads"))
      return false;
    const SceneId scene = sceneAt(*rig->document, 0);
    const NodeId root = nodeAt(*rig->document, 0);
    const HistoryResult execute = executePrepared(
        *rig, prepareAddChildNodeCommand(
                  rig->context, AddChildNodeRequest{scene, root, "Owned"}));
    const auto ownedSelection = selectedNode(rig->selection);
    if (!expect(execute.status == HistoryStatus::pushed && ownedSelection,
                "Undo conflict command executes"))
      return false;
    const MutationResult foreign =
        rig->commands.rename(ownedSelection->node.untyped(), "Foreign Rename");
    const ExactEditorStateSnapshot before = exactEditorStateSnapshot(
        *rig->document, rig->commands, rig->selection, rig->history);
    const HistoryResult rejected = rig->history.undo();
    passed &= expect(
        foreign.applied() && rejected.status == HistoryStatus::rejected &&
            rejected.command.error == MutationErrorCode::staleTransaction &&
            exactEditorStateSnapshot(*rig->document, rig->commands,
                                     rig->selection, rig->history) == before,
        "owned-record conflict rejects Undo without command lifecycle write");
  }
  {
    auto rig = makeRig("multi_scene_oracle.gltf");
    if (!expect(rig != nullptr, "Redo conflict fixture loads"))
      return false;
    const SceneId scene = sceneAt(*rig->document, 0);
    const NodeId target = nodeAt(*rig->document, 1);
    const auto targetOccurrence = occurrence(*rig, scene, target);
    if (!expect(targetOccurrence.has_value(),
                "Redo conflict occurrence captures"))
      return false;
    const HistoryResult execute = executePrepared(
        *rig, prepareDeleteNodeCommand(
                  rig->context,
                  DeleteNodeRequest{*targetOccurrence,
                                    SceneOccurrencePolicy::clickedScene}));
    if (!expect(execute.status == HistoryStatus::pushed &&
                    rig->history.undo().status == HistoryStatus::moved,
                "Redo conflict Delete executes and undoes"))
      return false;
    const MutationResult foreign =
        rig->commands.rename(target.untyped(), "Foreign Rename");
    const ExactEditorStateSnapshot before = exactEditorStateSnapshot(
        *rig->document, rig->commands, rig->selection, rig->history);
    const HistoryResult rejected = rig->history.redo();
    passed &= expect(
        foreign.applied() && rejected.status == HistoryStatus::rejected &&
            rejected.command.error == MutationErrorCode::staleTransaction &&
            exactEditorStateSnapshot(*rig->document, rig->commands,
                                     rig->selection, rig->history) == before,
        "owned-record conflict rejects Redo and preserves source stack");
  }
  {
    EditorLimits limits = kDefaultEditorLimits;
    limits.maxHistoryEntrySnapshotBytes = 1u;
    auto rig = makeRig("minimal_scene.gltf", limits);
    if (!expect(rig != nullptr, "snapshot budget fixture loads"))
      return false;
    const ExactEditorStateSnapshot before = exactEditorStateSnapshot(
        *rig->document, rig->commands, rig->selection, rig->history);
    const CommandPrepareResult rejected = prepareAddChildNodeCommand(
        rig->context,
        AddChildNodeRequest{sceneAt(*rig->document, 0),
                            nodeAt(*rig->document, 0), "Oversize"});
    passed &= expect(
        !rejected.succeeded() &&
            rejected.error == MutationErrorCode::snapshotBudgetExceeded &&
            exactEditorStateSnapshot(*rig->document, rig->commands,
                                     rig->selection, rig->history) == before,
        "structural snapshot budget rejects before publication");
  }
  return passed;
}

bool testRelationshipAndLimitRejections() {
  bool passed = true;
  {
    auto rig = makeRig("multi_scene_oracle.gltf");
    if (!expect(rig != nullptr, "cycle rejection fixture loads"))
      return false;
    const SceneId scene = sceneAt(*rig->document, 0);
    const NodeId ancestor = nodeAt(*rig->document, 0);
    const NodeId descendant = nodeAt(*rig->document, 2);
    const auto source = occurrence(*rig, scene, ancestor);
    const auto target = occurrence(*rig, scene, descendant);
    if (!expect(source && target, "cycle occurrences capture"))
      return false;
    const ExactEditorStateSnapshot before = exactEditorStateSnapshot(
        *rig->document, rig->commands, rig->selection, rig->history);
    const CommandPrepareResult rejected = prepareReparentNodeCommand(
        rig->context, ReparentNodeRequest{*source, *target,
                                          SceneOccurrencePolicy::clickedScene});
    passed &= expect(
        !rejected.succeeded() && rejected.error == MutationErrorCode::cycle &&
            exactEditorStateSnapshot(*rig->document, rig->commands,
                                     rig->selection, rig->history) == before,
        "cycle rejection preserves complete state");
  }
  {
    auto rig = makeRig("multi_scene_oracle.gltf");
    if (!expect(rig != nullptr, "all-scenes mismatch fixture loads"))
      return false;
    const SceneId scene = sceneAt(*rig->document, 0);
    const NodeId shared = nodeAt(*rig->document, 6);
    const NodeId localParent = nodeAt(*rig->document, 0);
    const auto source = occurrence(*rig, scene, shared);
    const auto parent = occurrence(*rig, scene, localParent);
    if (!expect(source && parent, "all-scenes mismatch occurrences capture"))
      return false;
    const ExactEditorStateSnapshot before = exactEditorStateSnapshot(
        *rig->document, rig->commands, rig->selection, rig->history);
    const CommandPrepareResult rejected = prepareDuplicateNodeCommand(
        rig->context, DuplicateNodeRequest{*source, *parent,
                                           SceneOccurrencePolicy::allScenes});
    passed &= expect(
        !rejected.succeeded() &&
            rejected.error == MutationErrorCode::multiScenePolicy &&
            exactEditorStateSnapshot(*rig->document, rig->commands,
                                     rig->selection, rig->history) == before,
        "all-scenes parent-domain mismatch rejects exactly");
  }
  {
    auto local = makeRig();
    auto foreign = makeRig();
    if (!expect(local != nullptr && foreign != nullptr,
                "cross-document rejection fixtures load"))
      return false;
    const SceneId foreignScene = sceneAt(*foreign->document, 0);
    const NodeId foreignRoot = nodeAt(*foreign->document, 0);
    const auto foreignOccurrence =
        occurrence(*foreign, foreignScene, foreignRoot);
    if (!expect(foreignOccurrence.has_value(),
                "foreign occurrence captures in its authority"))
      return false;
    const ExactEditorStateSnapshot before = exactEditorStateSnapshot(
        *local->document, local->commands, local->selection, local->history);
    const CommandPrepareResult rejected = prepareDeleteNodeCommand(
        local->context, DeleteNodeRequest{*foreignOccurrence,
                                          SceneOccurrencePolicy::clickedScene});
    passed &= expect(!rejected.succeeded() &&
                         rejected.error == MutationErrorCode::crossDocument &&
                         exactEditorStateSnapshot(
                             *local->document, local->commands,
                             local->selection, local->history) == before,
                     "cross-document occurrence is a typed exact rejection");
  }
  {
    auto rig = makeRig("multi_scene_oracle.gltf");
    if (!expect(rig != nullptr, "stale occurrence fixture loads"))
      return false;
    const SceneId scene = sceneAt(*rig->document, 0);
    const NodeId node = nodeAt(*rig->document, 1);
    const auto stale = occurrence(*rig, scene, node);
    if (!expect(stale.has_value(), "stale occurrence initially captures"))
      return false;
    const MutationResult foreign =
        rig->commands.rename(node.untyped(), "Revision Advance");
    const ExactEditorStateSnapshot before = exactEditorStateSnapshot(
        *rig->document, rig->commands, rig->selection, rig->history);
    const CommandPrepareResult rejected = prepareDuplicateNodeCommand(
        rig->context,
        DuplicateNodeRequest{*stale, std::nullopt,
                             SceneOccurrencePolicy::clickedScene});
    passed &= expect(foreign.applied() && !rejected.succeeded(),
                     "revision-bound stale occurrence rejects");
    passed &= expect(rejected.error == MutationErrorCode::staleOccurrence,
                     "stale occurrence retains its typed error");
    passed &=
        expect(exactEditorStateSnapshot(*rig->document, rig->commands,
                                        rig->selection, rig->history) == before,
               "stale occurrence rejection performs no further writes");
  }
  {
    auto document = loadDocument("feature_preservation.gltf");
    if (!expect(document != nullptr, "limit rejection fixture loads"))
      return false;
    EditorLimits limits = kDefaultEditorLimits;
    limits.maxNodes = document->objectCount(EditorObjectKind::node);
    StructuralRig rig(std::move(document), limits);
    const SceneId scene = sceneAt(*rig.document, 0);
    const NodeId node = nodeAt(*rig.document, 0);
    const auto source = occurrence(rig, scene, node);
    if (!expect(source.has_value(), "limit rejection occurrence captures"))
      return false;
    const ExactEditorStateSnapshot before = exactEditorStateSnapshot(
        *rig.document, rig.commands, rig.selection, rig.history);
    const CommandPrepareResult rejected = prepareDuplicateNodeCommand(
        rig.context, DuplicateNodeRequest{*source, std::nullopt,
                                          SceneOccurrencePolicy::clickedScene});
    passed &= expect(
        !rejected.succeeded() &&
            rejected.error == MutationErrorCode::limitExceeded &&
            exactEditorStateSnapshot(*rig.document, rig.commands, rig.selection,
                                     rig.history) == before,
        "deep Duplicate node limit rejects before history publication");
  }
  return passed;
}

bool sameRootRuntime(const ExactRootStateSnapshot &left,
                     const ExactRootStateSnapshot &right) noexcept {
  return left.baseline.translation == right.baseline.translation &&
         left.baseline.rotation == right.baseline.rotation &&
         left.baseline.scale == right.baseline.scale &&
         left.current.translation == right.current.translation &&
         left.current.rotation == right.current.rotation &&
         left.current.scale == right.current.scale &&
         left.externalStateSerial == right.externalStateSerial &&
         left.frozen == right.frozen &&
         left.frozenRootIds == right.frozenRootIds;
}

bool testExternalRootStateComposition() {
  auto document = loadDocument();
  if (!expect(document != nullptr, "root-state composition fixture loads"))
    return false;
  ManualEditorClock clock;
  SceneEditor editor(*document, clock);
  EditorCommandContext context{*document, editor.commands(),
                               editor.selection()};
  const SceneId scene = sceneAt(*document, 0);
  const NodeId root = nodeAt(*document, 0);
  SceneRootTransform preview;
  preview.translation = {4.0, 5.0, 6.0};
  const MutationResult previewResult =
      editor.commands().previewSceneRootTransform(scene, preview);
  if (!expect(previewResult.applied(), "scene-root preview publishes"))
    return false;
  CommandPrepareResult prepared = prepareAddChildNodeCommand(
      context, AddChildNodeRequest{scene, root, "History Child"});
  if (!expect(prepared.succeeded(),
              "history structural entry prepares after preview"))
    return false;
  const HistoryResult execute =
      editor.history().execute(std::move(prepared.command));
  const auto childSelection = selectedNode(editor.selection());
  if (!expect(execute.status == HistoryStatus::pushed && childSelection,
              "history structural entry publishes after preview"))
    return false;
  const NodeId child = childSelection->node;
  const EditorMutationResult later = editor.addRootNode(scene, "Later Root");
  const auto laterSelection = selectedNode(editor.selection());
  if (!expect(later.mutation.applied() && laterSelection &&
                  !laterSelection->parent,
              "later root publishes through the non-history selection adapter"))
    return false;
  const NodeId laterRoot = laterSelection->node;
  const ExactDocumentSnapshot afterLater =
      exactDocumentSnapshot(*document, editor.commands());
  if (!expect(afterLater.rootStates.size() == 1u,
              "later root snapshot exposes one root runtime state"))
    return false;
  const ExactRootStateSnapshot runtime = afterLater.rootStates[0];
  const std::uint64_t serial = runtime.externalStateSerial;
  const bool previewRepresented =
      runtime.current.translation == preview.translation ||
      runtime.baseline.translation == preview.translation || runtime.frozen;
  bool passed =
      expect(serial != 0u && previewRepresented,
             "later hierarchy retains preview representation and serial");
  passed &=
      expect(editor.history().undo().status == HistoryStatus::moved &&
                 !document->contains(child.untyped()) &&
                 document->contains(laterRoot.untyped()) &&
                 document->sceneRootNodeIds(scene) ==
                     std::vector<NodeId>{root, laterRoot},
             "history Undo removes only owned child and preserves later root");
  const ExactDocumentSnapshot afterUndo =
      exactDocumentSnapshot(*document, editor.commands());
  passed &= expect(afterUndo.rootStates.size() == 1u &&
                       sameRootRuntime(afterUndo.rootStates[0], runtime),
                   "history Undo preserves later absolute root runtime state");
  passed &=
      expect(editor.history().redo().status == HistoryStatus::moved &&
                 document->contains(child.untyped()) &&
                 document->contains(laterRoot.untyped()) &&
                 document->sceneRootNodeIds(scene) ==
                     std::vector<NodeId>{root, laterRoot},
             "history Redo restores owned child without disturbing later root");
  const ExactDocumentSnapshot afterRedo =
      exactDocumentSnapshot(*document, editor.commands());
  passed &= expect(afterRedo.rootStates.size() == 1u &&
                       sameRootRuntime(afterRedo.rootStates[0], runtime),
                   "history Redo also preserves external root runtime state");
  return passed;
}

#if defined(MGIF_EDITOR_TESTING)
bool testMaximumCounters() {
  constexpr std::uint32_t u32max = std::numeric_limits<std::uint32_t>::max();
  constexpr std::uint64_t u64max = std::numeric_limits<std::uint64_t>::max();
  bool passed = true;
  const CheckedCounterTransition generationBefore =
      EditorStateTestAccess::checkedGenerationSuccessor(u32max - 1u);
  const CheckedCounterTransition generationMax =
      EditorStateTestAccess::checkedGenerationSuccessor(u32max);
  const CheckedCounterTransition revisionBefore =
      EditorStateTestAccess::checkedRevisionSuccessor(u64max - 1u);
  const CheckedCounterTransition revisionMax =
      EditorStateTestAccess::checkedRevisionSuccessor(u64max);
  const CheckedCounterTransition epochBefore =
      EditorStateTestAccess::checkedHierarchyEpochSuccessor(u64max - 1u);
  const CheckedCounterTransition epochMax =
      EditorStateTestAccess::checkedHierarchyEpochSuccessor(u64max);
  const CheckedCounterTransition externalBefore =
      EditorStateTestAccess::checkedExternalSerialSuccessor(u64max - 1u);
  const CheckedCounterTransition externalMax =
      EditorStateTestAccess::checkedExternalSerialSuccessor(u64max);
  passed &= expect(
      generationBefore.canAdvance() && generationBefore.next == u32max &&
          generationMax.error == MutationErrorCode::generationExhausted &&
          generationMax.next == u32max && revisionBefore.canAdvance() &&
          revisionBefore.next == u64max &&
          revisionMax.error == MutationErrorCode::revisionExhausted &&
          revisionMax.next == u64max && epochBefore.canAdvance() &&
          epochBefore.next == u64max &&
          epochMax.error == MutationErrorCode::hierarchyEpochExhausted &&
          epochMax.next == u64max && externalBefore.canAdvance() &&
          externalBefore.next == u64max &&
          externalMax.error ==
              MutationErrorCode::externalStateSerialExhausted &&
          externalMax.next == u64max,
      "all four checked successor helpers reject maxima without wrap");

  {
    auto rig = makeRig();
    if (!expect(rig != nullptr, "invalid exhaustion seed fixture loads"))
      return false;
    const ExactEditorStateSnapshot before = exactEditorStateSnapshot(
        *rig->document, rig->commands, rig->selection, rig->history);
    const ExhaustionSeedResult invalid =
        EditorStateTestAccess::seedCounterMaximum(
            *rig->document,
            ExhaustionSeedRequest{ExhaustionCounterKind::documentRevision,
                                  nodeAt(*rig->document, 0).untyped(),
                                  std::nullopt});
    passed &= expect(
        invalid.status == ExhaustionSeedStatus::invalidTargetShape &&
            exactEditorStateSnapshot(*rig->document, rig->commands,
                                     rig->selection, rig->history) == before,
        "invalid exhaustion seed shape is an exact no-op");
  }
  {
    auto rig = makeRig();
    if (!expect(rig != nullptr, "generation maximum fixture loads"))
      return false;
    const SceneId scene = sceneAt(*rig->document, 0);
    const NodeId root = nodeAt(*rig->document, 0);
    const auto added = directAddNode(*rig, scene, "Generation Max", root);
    if (!expect(added.has_value(), "generation maximum target appends"))
      return false;
    const ExhaustionSeedResult seeded =
        EditorStateTestAccess::seedCounterMaximum(
            *rig->document,
            ExhaustionSeedRequest{ExhaustionCounterKind::objectGeneration,
                                  added->untyped(), std::nullopt});
    if (!expect(seeded.seeded() && seeded.seededObject,
                "generation maximum seeds exact live object"))
      return false;
    const NodeId maximum = *NodeId::from(*seeded.seededObject);
    const auto maximumOccurrence = occurrence(*rig, scene, maximum);
    if (!expect(maximumOccurrence.has_value(),
                "maximum-generation occurrence captures"))
      return false;
    const ExactEditorStateSnapshot before = exactEditorStateSnapshot(
        *rig->document, rig->commands, rig->selection, rig->history);
    const HistoryResult rejected = executePrepared(
        *rig, prepareDeleteNodeCommand(
                  rig->context,
                  DeleteNodeRequest{*maximumOccurrence,
                                    SceneOccurrencePolicy::clickedScene}));
    passed &= expect(
        rejected.status == HistoryStatus::rejected &&
            rejected.command.error == MutationErrorCode::generationExhausted &&
            rig->document->contains(maximum.untyped()) &&
            exactEditorStateSnapshot(*rig->document, rig->commands,
                                     rig->selection, rig->history) == before,
        "maximum generation rejects command preparation/publication exactly");
  }
  {
    auto rig = makeRig();
    if (!expect(rig != nullptr, "revision maximum fixture loads"))
      return false;
    const ExhaustionSeedResult seeded =
        EditorStateTestAccess::seedCounterMaximum(
            *rig->document,
            ExhaustionSeedRequest{ExhaustionCounterKind::documentRevision,
                                  std::nullopt, std::nullopt});
    if (!expect(seeded.seeded(), "revision maximum seeds"))
      return false;
    const ExactEditorStateSnapshot before = exactEditorStateSnapshot(
        *rig->document, rig->commands, rig->selection, rig->history);
    const HistoryResult rejected = executePrepared(
        *rig, prepareRenameNodeCommand(rig->context, nodeAt(*rig->document, 0),
                                       "Never"));
    passed &= expect(
        rejected.status == HistoryStatus::rejected &&
            rejected.command.error == MutationErrorCode::revisionExhausted &&
            rig->document->revision() == u64max &&
            exactEditorStateSnapshot(*rig->document, rig->commands,
                                     rig->selection, rig->history) == before,
        "maximum document revision rejects Rename without wrap/publication");
  }
  {
    auto rig = makeRig();
    if (!expect(rig != nullptr, "hierarchy epoch maximum fixture loads"))
      return false;
    const SceneId scene = sceneAt(*rig->document, 0);
    const ExhaustionSeedResult seeded =
        EditorStateTestAccess::seedCounterMaximum(
            *rig->document,
            ExhaustionSeedRequest{ExhaustionCounterKind::hierarchyEpoch,
                                  std::nullopt, scene});
    if (!expect(seeded.seeded(), "hierarchy epoch maximum seeds"))
      return false;
    const ExactEditorStateSnapshot before = exactEditorStateSnapshot(
        *rig->document, rig->commands, rig->selection, rig->history);
    const HistoryResult rejected = executePrepared(
        *rig,
        prepareAddChildNodeCommand(
            rig->context,
            AddChildNodeRequest{scene, nodeAt(*rig->document, 0), "Never"}));
    passed &= expect(
        rejected.status == HistoryStatus::rejected &&
            rejected.command.error ==
                MutationErrorCode::hierarchyEpochExhausted &&
            rig->document->sceneRootTransformState(scene).hierarchyEpoch ==
                u64max &&
            exactEditorStateSnapshot(*rig->document, rig->commands,
                                     rig->selection, rig->history) == before,
        "maximum hierarchy epoch rejects structural command exactly");
  }
  {
    auto rig = makeRig();
    if (!expect(rig != nullptr, "external serial maximum fixture loads"))
      return false;
    const SceneId scene = sceneAt(*rig->document, 0);
    const ExhaustionSeedResult seeded =
        EditorStateTestAccess::seedCounterMaximum(
            *rig->document,
            ExhaustionSeedRequest{ExhaustionCounterKind::externalStateSerial,
                                  std::nullopt, scene});
    if (!expect(seeded.seeded(), "external serial maximum seeds"))
      return false;
    const ExactEditorStateSnapshot before = exactEditorStateSnapshot(
        *rig->document, rig->commands, rig->selection, rig->history);
    SceneRootTransform transform;
    transform.translation = {1.0, 2.0, 3.0};
    const MutationResult rejected =
        rig->commands.previewSceneRootTransform(scene, transform);
    passed &= expect(
        rejected.rejected() &&
            rejected.error == MutationErrorCode::externalStateSerialExhausted &&
            rig->document->sceneRootTransformState(scene).externalStateSerial ==
                u64max &&
            exactEditorStateSnapshot(*rig->document, rig->commands,
                                     rig->selection, rig->history) == before,
        "maximum external serial rejects root publication without wrap");
  }
  return passed;
}
#endif

const ReferenceLocationInspection *
findLocation(const ReferenceRewriteProbeResult &result,
             std::string_view suffix) {
  const auto found = std::find_if(
      result.locations.begin(), result.locations.end(),
      [suffix](const ReferenceLocationInspection &location) {
        return std::string_view{location.jsonPointer}.ends_with(suffix);
      });
  return found == result.locations.end() ? nullptr : &*found;
}

std::unique_ptr<GltfDocument> loadInlineDocument(std::string_view json,
                                                 std::string_view stem) {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      (std::string(stem) + "-ctx_98e76ef198b9.gltf");
  {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
      return nullptr;
    output.write(json.data(), static_cast<std::streamsize>(json.size()));
    if (!output)
      return nullptr;
  }
  auto loaded = GltfDocumentIO::loadCandidate(path);
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  if (!loaded.succeeded()) {
    std::cerr << "inline fixture load failed: " << loaded.message() << '\n';
    for (const GltfValidationIssue &issue : loaded.validationReport().issues())
      std::cerr << "  " << issue.path << ": " << issue.message << '\n';
    return nullptr;
  }
  return std::move(loaded).takeCandidate();
}

bool inlineDocumentRejected(std::string_view json, std::string_view stem) {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      (std::string(stem) + "-ctx_98e76ef198b9.gltf");
  {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
      return false;
    output.write(json.data(), static_cast<std::streamsize>(json.size()));
    if (!output)
      return false;
  }
  const auto loaded = GltfDocumentIO::loadCandidate(path);
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  return !loaded.succeeded();
}

constexpr std::string_view kReferenceFixture = R"JSON({
  "asset": {"version": "2.0", "generator": "MGIF P4 reference matrix"},
  "extensionsUsed": [
    "KHR_lights_punctual",
    "KHR_materials_clearcoat",
    "KHR_draco_mesh_compression",
    "MSFT_texture_dds",
    "MGIF_test_unknown"
  ],
  "extensions": {
    "KHR_lights_punctual": {
      "lights": [
        {"name": "Light Zero", "type": "directional", "intensity": 2.0},
        {"name": "Light One", "type": "point", "intensity": 3.0}
      ]
    },
    "MGIF_test_unknown": {"orderedMeaning": [3, 1, 2]}
  },
  "scene": 0,
  "scenes": [
    {"name": "Scene Zero", "nodes": [0, 1]},
    {"name": "Scene One", "nodes": [0, 1]}
  ],
  "nodes": [
    {"name": "Mesh", "mesh": 0, "skin": 0, "children": [2]},
    {
      "name": "Camera Light",
      "camera": 0,
      "extensions": {"KHR_lights_punctual": {"light": 0}}
    },
    {"name": "Joint"}
  ],
  "meshes": [
    {
      "name": "Mesh Zero",
      "primitives": [
        {
          "attributes": {"POSITION": 0},
          "indices": 1,
          "material": 0,
          "extensions": {
            "KHR_draco_mesh_compression": {
              "bufferView": 0,
              "attributes": {"POSITION": 7},
              "unknownSibling": {"index": 0}
            }
          }
        },
        {"attributes": {"POSITION": 0}, "indices": 1, "material": 0}
      ]
    },
    {"name": "Mesh One", "primitives": [
      {"attributes": {"POSITION": 0}, "indices": 1, "material": 1}
    ]}
  ],
  "materials": [
    {
      "name": "Material Zero",
      "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}},
      "extensions": {
        "KHR_materials_clearcoat": {
          "clearcoatFactor": 0.5,
          "clearcoatTexture": {"index": 0},
          "unknownSibling": {"index": 0}
        }
      }
    },
    {"name": "Material One"}
  ],
  "textures": [
    {
      "name": "Texture Zero",
      "source": 0,
      "sampler": 0,
      "extensions": {
        "MSFT_texture_dds": {
          "source": 1,
          "unknownSibling": {"index": 0}
        }
      }
    },
    {"name": "Texture One", "source": 0, "sampler": 1}
  ],
  "images": [
    {"name": "Image Zero", "bufferView": 7, "mimeType": "image/png"},
    {"name": "Image One", "bufferView": 7, "mimeType": "image/png"}
  ],
  "samplers": [
    {"name": "Sampler Zero", "magFilter": 9729},
    {"name": "Sampler One", "minFilter": 9728}
  ],
  "skins": [
    {"name": "Skin Zero", "inverseBindMatrices": 2, "skeleton": 2, "joints": [2]},
    {"name": "Skin One", "inverseBindMatrices": 2, "skeleton": 2, "joints": [2]}
  ],
  "animations": [
    {
      "name": "Animation Zero",
      "samplers": [{"input": 3, "output": 4, "interpolation": "LINEAR"}],
      "channels": [{"sampler": 0, "target": {"node": 0, "path": "translation"}}]
    },
    {
      "name": "Animation One",
      "samplers": [{"input": 3, "output": 4, "interpolation": "LINEAR"}],
      "channels": [{"sampler": 0, "target": {"node": 0, "path": "translation"}}]
    }
  ],
  "cameras": [
    {"name": "Camera Zero", "type": "perspective", "perspective": {"yfov": 0.75, "znear": 0.1}},
    {"name": "Camera One", "type": "perspective", "perspective": {"yfov": 0.5, "znear": 0.1}}
  ],
  "buffers": [
    {
      "name": "Buffer Zero",
      "byteLength": 160,
      "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIAAAAAAIA/AAAAAAAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAAAAAAIA/AAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAQAAAQEABAAAAAAAAQAAAQEAAAIBATUdJRg=="
    },
    {
      "name": "Buffer One",
      "byteLength": 160,
      "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIAAAAAAIA/AAAAAAAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAAAAAAIA/AAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAQAAAQEABAAAAAAAAQAAAQEAAAIBATUdJRg=="
    }
  ],
  "bufferViews": [
    {"buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962},
    {"buffer": 0, "byteOffset": 36, "byteLength": 6, "target": 34963},
    {"buffer": 0, "byteOffset": 44, "byteLength": 64},
    {"buffer": 0, "byteOffset": 108, "byteLength": 8},
    {"buffer": 0, "byteOffset": 116, "byteLength": 24},
    {"buffer": 0, "byteOffset": 140, "byteLength": 1},
    {"buffer": 0, "byteOffset": 144, "byteLength": 12},
    {"buffer": 0, "byteOffset": 156, "byteLength": 4}
  ],
  "accessors": [
    {
      "bufferView": 0,
      "componentType": 5126,
      "count": 3,
      "type": "VEC3",
      "min": [0.0, 0.0, 0.0],
      "max": [2.0, 3.0, 4.0],
      "sparse": {
        "count": 1,
        "indices": {"bufferView": 5, "componentType": 5121},
        "values": {"bufferView": 6}
      }
    },
    {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"},
    {"bufferView": 2, "componentType": 5126, "count": 1, "type": "MAT4"},
    {"bufferView": 3, "componentType": 5126, "count": 2, "type": "SCALAR", "min": [0.0], "max": [1.0]},
    {"bufferView": 4, "componentType": 5126, "count": 2, "type": "VEC3"}
  ]
})JSON";

constexpr std::string_view kInvalidDdsFixture = R"JSON({
  "asset": {"version": "2.0"},
  "extensionsUsed": ["MSFT_texture_dds"],
  "scene": 0,
  "scenes": [{"nodes": [0]}],
  "nodes": [{}],
  "textures": [{"extensions": {"MSFT_texture_dds": {"source": 2}}}],
  "images": [
    {"uri": "data:application/octet-stream;base64,AA=="},
    {"uri": "data:application/octet-stream;base64,AA=="}
  ]
})JSON";

constexpr std::string_view kInvalidDracoAttributeFixture = R"JSON({
  "asset": {"version": "2.0"},
  "extensionsUsed": ["KHR_draco_mesh_compression"],
  "scene": 0,
  "scenes": [{"nodes": [0]}],
  "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [{
    "attributes": {"POSITION": 0},
    "extensions": {"KHR_draco_mesh_compression": {
      "bufferView": 0, "attributes": {"POSITION": -1}
    }}
  }]}],
  "buffers": [{"byteLength": 36, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA"}],
  "bufferViews": [{"buffer": 0, "byteLength": 36}],
  "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"}]
})JSON";

constexpr std::string_view kInvalidDracoViewFixture = R"JSON({
  "asset": {"version": "2.0"},
  "extensionsUsed": ["KHR_draco_mesh_compression"],
  "scene": 0,
  "scenes": [{"nodes": [0]}],
  "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [{
    "attributes": {"POSITION": 0},
    "extensions": {"KHR_draco_mesh_compression": {
      "bufferView": 1, "attributes": {"POSITION": 7}
    }}
  }]}],
  "buffers": [{"byteLength": 36, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA"}],
  "bufferViews": [{"buffer": 0, "byteLength": 36}],
  "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"}]
})JSON";

bool testReferenceRewriteMatrix() {
  auto document = loadInlineDocument(kReferenceFixture, "mgif-p4-04-reference");
  if (!expect(document != nullptr,
              "complete reference-probe fixture loads and validates"))
    return false;
  SceneCommands commands(*document);
  const ExactDocumentSnapshot source =
      exactDocumentSnapshot(*document, commands);
  bool passed = true;
  for (std::size_t raw = 1u;
       raw < static_cast<std::size_t>(EditorObjectKind::count); ++raw) {
    const EditorObjectKind kind = static_cast<EditorObjectKind>(raw);
    const std::size_t count = document->objectCount(kind);
    passed &= expect(count >= 2u,
                     "every reference-probe target has nonidentity capacity");
    if (count < 2u)
      continue;
    std::vector<std::optional<std::uint32_t>> map(count);
    for (std::size_t index = 0; index < count; ++index)
      map[index] = static_cast<std::uint32_t>(count - index - 1u);
    const ReferenceRewriteProbeResult positive =
        EditorStateInspector::probeReferenceRewrite(*document, kind, map);
    passed &=
        expect(positive.status == ReferenceRewriteProbeStatus::rewritten &&
                   positive.candidateSemanticDigest.size() == 64u &&
                   exactDocumentSnapshot(*document, commands) == source,
               "each registry row rewrites a private candidate only");
    map[0] = map[1];
    const ReferenceRewriteProbeResult negative =
        EditorStateInspector::probeReferenceRewrite(*document, kind, map);
    passed &= expect(
        negative.status == ReferenceRewriteProbeStatus::invalidMap &&
            exactDocumentSnapshot(*document, commands) == source,
        "each registry row rejects a duplicate map without source mutation");
  }

  std::vector<std::optional<std::uint32_t>> imageMap(
      document->objectCount(EditorObjectKind::image));
  for (std::size_t index = 0; index < imageMap.size(); ++index)
    imageMap[index] = static_cast<std::uint32_t>(imageMap.size() - index - 1u);
  const ReferenceRewriteProbeResult image =
      EditorStateInspector::probeReferenceRewrite(
          *document, EditorObjectKind::image, imageMap);
  const ReferenceLocationInspection *dds =
      findLocation(image, "/extensions/MSFT_texture_dds/source");
  passed &= expect(
      dds && dds->before == 1 && dds->after == 0 && dds->rewritten &&
          !dds->codecLocal &&
          std::none_of(image.locations.begin(), image.locations.end(),
                       [](const ReferenceLocationInspection &location) {
                         return location.jsonPointer.find("unknownSibling") !=
                                std::string::npos;
                       }),
      "DDS source rewrites while unknown siblings remain opaque");

  std::vector<std::optional<std::uint32_t>> accessorMap(
      document->objectCount(EditorObjectKind::accessor));
  for (std::size_t index = 0; index < accessorMap.size(); ++index)
    accessorMap[index] =
        static_cast<std::uint32_t>(accessorMap.size() - index - 1u);
  const ReferenceRewriteProbeResult accessor =
      EditorStateInspector::probeReferenceRewrite(
          *document, EditorObjectKind::accessor, accessorMap);
  const ReferenceLocationInspection *dracoAttribute = findLocation(
      accessor, "/extensions/KHR_draco_mesh_compression/attributes/POSITION");
  passed &= expect(dracoAttribute && dracoAttribute->before == 7 &&
                       dracoAttribute->after == 7 &&
                       dracoAttribute->codecLocal && !dracoAttribute->rewritten,
                   "Draco codec-local attributes remain exact");

  std::vector<std::optional<std::uint32_t>> viewMap(
      document->objectCount(EditorObjectKind::bufferView));
  for (std::size_t index = 0; index < viewMap.size(); ++index)
    viewMap[index] = static_cast<std::uint32_t>(viewMap.size() - index - 1u);
  const ReferenceRewriteProbeResult view =
      EditorStateInspector::probeReferenceRewrite(
          *document, EditorObjectKind::bufferView, viewMap);
  const ReferenceLocationInspection *dracoView =
      findLocation(view, "/extensions/KHR_draco_mesh_compression/bufferView");
  passed &=
      expect(dracoView && dracoView->before == 0 && dracoView->after == 7 &&
                 dracoView->rewritten && !dracoView->codecLocal,
             "only Draco bufferView participates in view remapping");
  passed &= expect(
      inlineDocumentRejected(kInvalidDdsFixture, "mgif-p4-04-invalid-dds") &&
          inlineDocumentRejected(kInvalidDracoAttributeFixture,
                                 "mgif-p4-04-invalid-draco-attribute") &&
          inlineDocumentRejected(kInvalidDracoViewFixture,
                                 "mgif-p4-04-invalid-draco-view"),
      "invalid DDS source, Draco attribute, and Draco view reject validation");
  passed &= expect(exactDocumentSnapshot(*document, commands) == source,
                   "all reference probes preserve the exact source snapshot");
  return passed;
}

} // namespace

int main() {
  bool passed = true;
  passed &= testRename();
  passed &= testAddChildAndInspection();
  passed &= testAddLights();
  passed &= testDuplicateDeepAndD20();
  passed &= testDeleteAndUniqueRootEnvelope();
  passed &= testReparentAndAnchoredOrdering();
  passed &= testAnchoredComposerMatrix();
  passed &= testFailureAtomicity();
  passed &= testRelationshipAndLimitRejections();
  passed &= testExternalRootStateComposition();
#if defined(MGIF_EDITOR_TESTING)
  passed &= testMaximumCounters();
#endif
  passed &= testReferenceRewriteMatrix();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
