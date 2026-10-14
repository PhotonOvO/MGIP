#include <mgif/scene_editor/GltfDocumentIO.h>
#include <mgif/scene_editor/SceneEditor.h>
#include <mgif/scene_editor/commands/PropertyCommands.h>
#include <mgif/scene_editor/commands/TransformCommands.h>
#include <mgif/scene_editor/testing/EditorStateInspector.h>

#include "support/EditorTestSupport.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>

namespace {

using namespace demo::editor;
using namespace demo::editor::testing;
using namespace std::chrono_literals;

bool expect(bool condition, std::string_view message) {
  if (!condition)
    std::cerr << "FAILED: " << message << '\n';
  return condition;
}

std::filesystem::path fixture(const char *name) {
  return std::filesystem::path(MGIF_TEST_SOURCE_DIR) / "tests" / "fixtures" /
         "gltf_editor" / name;
}

std::unique_ptr<GltfDocument> loadDocument(const char *name) {
  auto loaded = GltfDocumentIO::loadCandidate(fixture(name));
  if (!loaded.succeeded())
    return nullptr;
  return std::move(loaded).takeCandidate();
}

class EditorRig final {
public:
  EditorRig(std::unique_ptr<GltfDocument> source, EditorLimits limits)
      : document(std::move(source)), editor(std::make_unique<SceneEditor>(
                                         *document, clock, limits, &fault)) {}

  std::unique_ptr<GltfDocument> document;
  ManualEditorClock clock;
  ScriptedEditorFaultInjector fault;
  std::unique_ptr<SceneEditor> editor;
};

std::unique_ptr<EditorRig> makeRig(const char *name = "minimal_scene.gltf",
                                   EditorLimits limits = kDefaultEditorLimits) {
  auto document = loadDocument(name);
  if (!document)
    return nullptr;
  return std::make_unique<EditorRig>(std::move(document), limits);
}

template <EditorObjectKind Kind>
std::optional<TypedEditorObjectId<Kind>> firstId(const GltfDocument &document) {
  return TypedEditorObjectId<Kind>::from(document.idAt(Kind, 0));
}

std::optional<NodeId> firstNode(const EditorRig &rig) {
  return firstId<EditorObjectKind::node>(*rig.document);
}

std::optional<SceneId> firstScene(const EditorRig &rig) {
  return firstId<EditorObjectKind::scene>(*rig.document);
}

bool selectRoot(EditorRig &rig, NodeId node) {
  const auto scene = firstScene(rig);
  if (!scene)
    return false;
  EditorSelectionSnapshot selection;
  selection.authority = rig.document->documentAuthority();
  selection.value = NodeSelection{*scene, node, std::nullopt};
  return rig.editor->selection().select(*rig.document, selection);
}

NodeTransform translated(NodeTransform value, double x) {
  value.translation[0] = x;
  return value;
}

ExactEditorStateSnapshot snapshot(const EditorRig &rig) {
  return exactEditorStateSnapshot(*rig.document, rig.editor->commands(),
                                  rig.editor->selection(),
                                  rig.editor->history());
}

ContinuousFrameSignals inspectorFinish(std::uint64_t frame) {
  ContinuousFrameSignals signals;
  signals.applicationFrame = frame;
  signals.inspectorDeactivatedAfterEdit = true;
  return signals;
}

ContinuousFrameSignals physicalRelease(std::uint64_t frame) {
  ContinuousFrameSignals signals;
  signals.applicationFrame = frame;
  signals.physicalReleaseDelivered = true;
  return signals;
}

bool testDiscreteTransform() {
  auto rig = makeRig();
  if (!expect(rig != nullptr, "discrete transform fixture loads"))
    return false;
  const auto node = firstNode(*rig);
  const auto initial =
      node ? rig->editor->commands().nodeTransform(*node) : std::nullopt;
  if (!expect(node.has_value() && initial.has_value(),
              "discrete transform target resolves"))
    return false;
  bool passed = true;
  passed &= expect(selectRoot(*rig, *node), "discrete target can be selected");
  const EditorSelectionSnapshot selected = rig->editor->selection().snapshot();
  EditorCommandContext context{*rig->document, rig->editor->commands(),
                               rig->editor->selection()};
  NodeTransform after = translated(*initial, 3.0);
  after.scale = {0.0, -2.0, 4.0};
  CommandPrepareResult prepared =
      prepareTransformCommand(context, *node, *initial, after, &rig->fault);
  if (!expect(prepared.succeeded(), "discrete transform prepares"))
    return false;

  const RetainedCommandInspection preparedInspection =
      prepared.command->inspectRetainedState();
  passed &= expect(
      prepared.command->description() == "Transform 'Root'" &&
          preparedInspection.kind == EditorCommandKind::transform &&
          preparedInspection.lifecycle == CommandLifecycleState::prepared &&
          preparedInspection.stateRevision == 0 &&
          preparedInspection.retainedSnapshotBytes ==
              prepared.command->snapshotBytes() &&
          preparedInspection.stableTarget == node->untyped() &&
          preparedInspection.authority == rig->document->documentAuthority() &&
          preparedInspection.executeSelection == selected &&
          preparedInspection.undoSelection == selected &&
          preparedInspection.redoSelection == selected &&
          detail::retainedCommandInspectionHashMatches(preparedInspection),
      "prepared transform exposes complete canonical retained state");

  const std::uint64_t revisionBefore = rig->document->revision();
  HistoryResult executed =
      rig->editor->history().execute(std::move(prepared.command));
  passed &=
      expect(executed.status == HistoryStatus::pushed &&
                 rig->editor->commands().nodeTransform(*node) == after &&
                 rig->document->revision() == revisionBefore + 1 &&
                 rig->editor->selection().snapshot() == selected &&
                 rig->editor->history().undoDescription() == "Transform 'Root'",
             "discrete transform executes once and preserves selection");
  const HistoryInspection afterExecute =
      EditorStateInspector::inspect(rig->editor->history());
  passed &= expect(
      afterExecute.undoOldestToNewest.size() == 1 &&
          afterExecute.undoOldestToNewest[0].retainedCommand.lifecycle ==
              CommandLifecycleState::executed &&
          afterExecute.undoOldestToNewest[0].retainedCommand.stateRevision ==
              1 &&
          afterExecute.undoOldestToNewest[0].retainedCommand.preparedToken ==
              rig->editor->commands().token(),
      "successful execute replaces token and advances retained state once");

  passed &=
      expect(rig->editor->history().undo().status == HistoryStatus::moved &&
                 rig->editor->commands().nodeTransform(*node) == *initial &&
                 rig->editor->selection().snapshot() == selected,
             "transform Undo restores exact initial value");
  const HistoryInspection afterUndo =
      EditorStateInspector::inspect(rig->editor->history());
  passed &= expect(
      afterUndo.redoOldestToNewest[0].retainedCommand.lifecycle ==
              CommandLifecycleState::undone &&
          afterUndo.redoOldestToNewest[0].retainedCommand.stateRevision == 2,
      "Undo advances retained lifecycle revision");
  passed &=
      expect(rig->editor->history().redo().status == HistoryStatus::moved &&
                 rig->editor->commands().nodeTransform(*node) == after &&
                 rig->editor->selection().snapshot() == selected,
             "transform Redo restores exact final value");

  const auto beforeInvalid = snapshot(*rig);
  NodeTransform invalid = after;
  invalid.rotation = {0.0, 0.0, 0.0, 2.0};
  auto invalidCommand =
      prepareTransformCommand(context, *node, after, invalid, &rig->fault);
  passed &= expect(invalidCommand.succeeded() &&
                       rig->editor->history()
                               .execute(std::move(invalidCommand.command))
                               .status == HistoryStatus::rejected &&
                       snapshot(*rig) == beforeInvalid,
                   "invalid quaternion rejection is fully failure-atomic");

  auto staleBefore = prepareTransformCommand(
      context, *node, *initial, translated(after, 4.0), &rig->fault);
  passed &= expect(
      !staleBefore.succeeded() &&
          staleBefore.error == MutationErrorCode::staleTransaction &&
          snapshot(*rig) == beforeInvalid,
      "factory rejects a mismatched immutable before value without mutation");
  return passed;
}

bool commitContinuousTransform(
    EditorRig &rig, NodeId node, const NodeTransform &value,
    std::uint64_t frame,
    ContinuousEditKind kind = ContinuousEditKind::inspectorTransform) {
  ContinuousEditSession &session = rig.editor->continuousEdit();
  return session.beginTransform(kind, node).status == ContinuousStatus::began &&
         session.preview(value).status == ContinuousStatus::previewApplied &&
         session.finishFrame(kind == ContinuousEditKind::gizmoTransform
                                 ? physicalRelease(frame)
                                 : inspectorFinish(frame))
                 .status == ContinuousStatus::committed;
}

bool testMergeBoundariesAndFalseAbsorb() {
  auto rig = makeRig();
  if (!expect(rig != nullptr, "merge boundary fixture loads"))
    return false;
  const auto node = firstNode(*rig);
  const auto initial =
      node ? rig->editor->commands().nodeTransform(*node) : std::nullopt;
  if (!expect(node.has_value() && initial.has_value(),
              "merge boundary target resolves"))
    return false;
  bool passed = true;

  rig->clock.set(EditorTimePoint{});
  passed &= expect(
      commitContinuousTransform(*rig, *node, translated(*initial, 1.0), 1),
      "first continuous transform commits");
  const HistoryInspection first =
      EditorStateInspector::inspect(rig->editor->history());
  const CanonicalPayloadInspection oldestBefore =
      first.undoOldestToNewest[0].retainedCommand.beforePayload;

  rig->clock.set(EditorTimePoint{} + 499ms);
  passed &=
      expect(commitContinuousTransform(*rig, *node, translated(*initial, 2.0),
                                       2, ContinuousEditKind::gizmoTransform) &&
                 rig->editor->history().undoCount() == 1,
             "499ms Inspector/Gizmo transform commands merge");
  const HistoryInspection at499 =
      EditorStateInspector::inspect(rig->editor->history());
  passed &=
      expect(at499.undoOldestToNewest[0].retainedCommand.beforePayload ==
                     oldestBefore &&
                 at499.undoOldestToNewest[0].retainedCommand.finalPayload !=
                     first.undoOldestToNewest[0].retainedCommand.finalPayload &&
                 at499.undoOldestToNewest[0].retainedCommand.stateRevision == 2,
             "merge retains oldest before and adopts newest final");

  rig->clock.set(EditorTimePoint{} + 999ms);
  passed &= expect(
      commitContinuousTransform(*rig, *node, translated(*initial, 3.0), 3) &&
          rig->editor->history().undoCount() == 2,
      "500ms real transform boundary splits");
  rig->clock.set(EditorTimePoint{} + 1500ms);
  passed &= expect(
      commitContinuousTransform(*rig, *node, translated(*initial, 4.0), 4) &&
          rig->editor->history().undoCount() == 3,
      "501ms real transform boundary splits");

  auto falseRig = makeRig();
  if (!expect(falseRig != nullptr, "false-absorb transform fixture loads"))
    return false;
  const auto falseNode = firstNode(*falseRig);
  const auto falseInitial =
      falseNode ? falseRig->editor->commands().nodeTransform(*falseNode)
                : std::nullopt;
  if (!expect(falseNode.has_value() && falseInitial.has_value(),
              "false-absorb target resolves"))
    return false;
  EditorCommandContext context{*falseRig->document,
                               falseRig->editor->commands(),
                               falseRig->editor->selection()};
  auto older =
      prepareContinuousTransformCommand(context, *falseNode, &falseRig->fault);
  passed &= expect(older.succeeded(), "false-absorb older prepares");
  if (!older.succeeded())
    return false;
  passed &=
      expect(older.command
                     ->applyPreview(context, ContinuousCommandValue{translated(
                                                 *falseInitial, 1.0)})
                     .status == CommandStatus::applied,
             "false-absorb older preview applies");
  const RetainedCommandInspection olderBefore =
      older.command->inspectRetainedState();
  falseRig->clock.set(EditorTimePoint{});
  passed &= expect(falseRig->editor->history()
                           .commitExecuted(std::move(older.command))
                           .status == HistoryStatus::pushed,
                   "false-absorb older pushes");

  auto newer =
      prepareContinuousTransformCommand(context, *falseNode, &falseRig->fault);
  passed &= expect(newer.succeeded(), "false-absorb newer prepares");
  if (!newer.succeeded())
    return false;
  passed &=
      expect(newer.command
                     ->applyPreview(context, ContinuousCommandValue{translated(
                                                 *falseInitial, 2.0)})
                     .status == CommandStatus::applied,
             "false-absorb newer preview applies");
  const RetainedCommandInspection newerBefore =
      newer.command->inspectRetainedState();
  falseRig->fault.reset();
  passed &= expect(falseRig->fault.failOnCall(EditorFaultPoint::allocation, 1),
                   "false-absorb fault arms");
  falseRig->clock.set(EditorTimePoint{} + 1ms);
  passed &= expect(falseRig->editor->history()
                           .commitExecuted(std::move(newer.command))
                           .status == HistoryStatus::pushed,
                   "false absorption falls back to an intact split push");
  const HistoryInspection falseAfter =
      EditorStateInspector::inspect(falseRig->editor->history());
  passed &= expect(
      falseAfter.undoOldestToNewest.size() == 2 &&
          falseAfter.undoOldestToNewest[0].retainedCommand == olderBefore &&
          falseAfter.undoOldestToNewest[1].retainedCommand == newerBefore &&
          olderBefore.canonicalSha256 != newerBefore.canonicalSha256,
      "false absorption preserves typed inspection of both candidates");
  return passed;
}

bool testBudgetAndPreviewAtomicity() {
  auto discovery = makeRig();
  if (!expect(discovery != nullptr, "budget discovery fixture loads"))
    return false;
  const auto discoveryNode = firstNode(*discovery);
  if (!expect(discoveryNode.has_value(), "budget discovery node resolves"))
    return false;
  EditorCommandContext discoveryContext{*discovery->document,
                                        discovery->editor->commands(),
                                        discovery->editor->selection()};
  auto sized = prepareContinuousTransformCommand(
      discoveryContext, *discoveryNode, &discovery->fault);
  if (!expect(sized.succeeded(), "budget discovery command prepares"))
    return false;
  const std::uint64_t bytes = sized.command->snapshotBytes();
  if (!expect(bytes > 1, "transform snapshot byte count is nontrivial"))
    return false;
  bool passed = true;

  EditorLimits belowLimits = kDefaultEditorLimits;
  belowLimits.maxHistoryEntrySnapshotBytes = bytes - 1;
  belowLimits.maxHistoryAggregateSnapshotBytes = bytes * 4;
  auto below = makeRig("minimal_scene.gltf", belowLimits);
  if (!expect(below != nullptr, "below-budget fixture loads"))
    return false;
  const auto belowNode = firstNode(*below);
  const auto belowInitial =
      belowNode ? below->editor->commands().nodeTransform(*belowNode)
                : std::nullopt;
  if (!expect(belowNode.has_value() && belowInitial.has_value(),
              "below-budget target resolves"))
    return false;
  passed &=
      expect(below->editor->continuousEdit()
                     .beginTransform(ContinuousEditKind::inspectorTransform,
                                     *belowNode)
                     .status == ContinuousStatus::began,
             "below-budget session begins before preview admission");
  const auto beforeRejectedPreview = snapshot(*below);
  passed &=
      expect(below->editor->continuousEdit()
                         .preview(translated(*belowInitial, 1.0))
                         .error == MutationErrorCode::snapshotBudgetExceeded &&
                 below->editor->continuousEdit().state() ==
                     ContinuousEditState::previewing &&
                 snapshot(*below) == beforeRejectedPreview,
             "per-entry admission rejects before preview mutation");

  EditorLimits exactLimits = kDefaultEditorLimits;
  exactLimits.maxHistoryEntrySnapshotBytes = bytes;
  exactLimits.maxHistoryAggregateSnapshotBytes = bytes;
  auto exact = makeRig("minimal_scene.gltf", exactLimits);
  if (!expect(exact != nullptr, "exact-budget fixture loads"))
    return false;
  const auto exactNode = firstNode(*exact);
  const auto exactInitial =
      exactNode ? exact->editor->commands().nodeTransform(*exactNode)
                : std::nullopt;
  if (!expect(exactNode.has_value() && exactInitial.has_value(),
              "exact-budget target resolves"))
    return false;
  passed &=
      expect(commitContinuousTransform(*exact, *exactNode,
                                       translated(*exactInitial, 1.0), 1) &&
                 exact->editor->history().retainedSnapshotBytes() == bytes,
             "exact per-entry and aggregate budgets admit the command");

  EditorLimits aggregateLimits = kDefaultEditorLimits;
  aggregateLimits.maxHistoryEntrySnapshotBytes = bytes;
  aggregateLimits.maxHistoryAggregateSnapshotBytes = bytes * 2 - 1;
  auto aggregate = makeRig("minimal_scene.gltf", aggregateLimits);
  if (!expect(aggregate != nullptr, "aggregate budget fixture loads"))
    return false;
  const auto aggregateNode = firstNode(*aggregate);
  const auto aggregateInitial =
      aggregateNode
          ? aggregate->editor->commands().nodeTransform(*aggregateNode)
          : std::nullopt;
  if (!expect(aggregateNode.has_value() && aggregateInitial.has_value(),
              "aggregate budget target resolves"))
    return false;
  aggregate->clock.set(EditorTimePoint{});
  passed &=
      expect(commitContinuousTransform(*aggregate, *aggregateNode,
                                       translated(*aggregateInitial, 1.0), 1),
             "aggregate budget first command commits");
  passed &=
      expect(aggregate->editor->continuousEdit()
                     .beginTransform(ContinuousEditKind::inspectorTransform,
                                     *aggregateNode)
                     .status == ContinuousStatus::began,
             "aggregate budget second session begins");
  const auto beforeAggregateReject = snapshot(*aggregate);
  passed &= expect(
      aggregate->editor->continuousEdit()
                  .preview(translated(*aggregateInitial, 2.0))
                  .error == MutationErrorCode::snapshotBudgetExceeded &&
          snapshot(*aggregate) == beforeAggregateReject,
      "conservative aggregate admission rejects before a possible merge");

  auto atomic = makeRig();
  if (!expect(atomic != nullptr, "preview atomicity fixture loads"))
    return false;
  const auto atomicNode = firstNode(*atomic);
  const auto atomicInitial =
      atomicNode ? atomic->editor->commands().nodeTransform(*atomicNode)
                 : std::nullopt;
  if (!expect(atomicNode.has_value() && atomicInitial.has_value(),
              "preview atomicity target resolves"))
    return false;
  EditorCommandContext atomicContext{*atomic->document,
                                     atomic->editor->commands(),
                                     atomic->editor->selection()};
  auto command = prepareContinuousTransformCommand(atomicContext, *atomicNode,
                                                   &atomic->fault);
  if (!expect(command.succeeded(), "preview atomicity command prepares"))
    return false;
  const RetainedCommandInspection prepared =
      command.command->inspectRetainedState();
  const auto beforeWrongValue = snapshot(*atomic);
  const ContinuousCommandValue wrongValue{MaterialSnapshot{}};
  passed &= expect(
      command.command->prospectiveSnapshotBytes(wrongValue).error ==
              MutationErrorCode::invalidSnapshot &&
          command.command->applyPreview(atomicContext, wrongValue).error ==
              MutationErrorCode::invalidSnapshot &&
          command.command->inspectRetainedState() == prepared &&
          snapshot(*atomic) == beforeWrongValue,
      "transform command rejects a wrong variant alternative without write");
  NodeTransform invalid = *atomicInitial;
  invalid.translation[0] = std::numeric_limits<double>::infinity();
  const auto beforeInvalid = snapshot(*atomic);
  passed &= expect(
      command.command
                  ->applyPreview(atomicContext, ContinuousCommandValue{invalid})
                  .status == CommandStatus::rejected &&
          command.command->inspectRetainedState() == prepared &&
          snapshot(*atomic) == beforeInvalid,
      "failed preview preserves document and immutable command state");

  const NodeTransform good = translated(*atomicInitial, 1.0);
  passed &= expect(
      command.command->applyPreview(atomicContext, ContinuousCommandValue{good})
              .status == CommandStatus::applied,
      "valid preview applies after a failed preview");
  const RetainedCommandInspection lastGood =
      command.command->inspectRetainedState();
  passed &=
      expect(lastGood.preparedToken == atomic->editor->commands().token() &&
                 lastGood.preparedToken != prepared.preparedToken &&
                 lastGood.stateRevision == prepared.stateRevision + 1,
             "successful preview alone replaces token and final state");
  atomic->fault.reset();
  passed &=
      expect(atomic->fault.failOnCall(EditorFaultPoint::preCandidatePublish, 1),
             "preview publication fault arms");
  const auto beforeInjectedFailure = snapshot(*atomic);
  passed &= expect(
      command.command
                  ->applyPreview(
                      atomicContext,
                      ContinuousCommandValue{translated(*atomicInitial, 2.0)})
                  .status == CommandStatus::rejected &&
          command.command->inspectRetainedState() == lastGood &&
          snapshot(*atomic) == beforeInjectedFailure,
      "injected preview failure retains last good token/final and full state");
  return passed;
}

bool testVariantDispatch() {
  auto rig = makeRig("material_extensions.gltf");
  if (!expect(rig != nullptr, "variant dispatch fixture loads"))
    return false;
  const auto material = firstId<EditorObjectKind::material>(*rig->document);
  const auto light = firstId<EditorObjectKind::light>(*rig->document);
  if (!expect(material.has_value() && light.has_value(),
              "material and light targets resolve"))
    return false;
  auto materialProperties =
      rig->editor->commands().materialProperties(*material);
  auto lightProperties = rig->editor->commands().lightProperties(*light);
  auto initialMaterial = rig->editor->commands().materialSnapshot(*material);
  auto initialLight = rig->editor->commands().lightSnapshot(*light);
  if (!expect(materialProperties.has_value() && lightProperties.has_value() &&
                  initialMaterial.has_value() && initialLight.has_value(),
              "complete property snapshots resolve"))
    return false;
  materialProperties->roughnessFactor = 0.25;
  lightProperties->intensity = 7.0;
  const auto finalMaterial = rig->editor->commands().prepareMaterialSnapshot(
      *material, MaterialEdit{*materialProperties});
  const auto finalLight =
      rig->editor->commands().prepareLightSnapshot(*light, *lightProperties);
  if (!expect(finalMaterial.has_value() && finalLight.has_value(),
              "complete property preview values prepare"))
    return false;
  bool passed = true;
  EditorCommandContext context{*rig->document, rig->editor->commands(),
                               rig->editor->selection()};
  auto sizedMaterial =
      prepareContinuousMaterialCommand(context, *material, &rig->fault);
  auto sizedLight = prepareContinuousLightCommand(context, *light, &rig->fault);
  if (!expect(sizedMaterial.succeeded() && sizedLight.succeeded(),
              "public property factories expose continuous base commands"))
    return false;
  const std::uint64_t materialBytes = sizedMaterial.command->snapshotBytes();
  const std::uint64_t lightBytes = sizedLight.command->snapshotBytes();

  ContinuousEditSession &session = rig->editor->continuousEdit();
  passed &=
      expect(session.beginMaterial(*material).status == ContinuousStatus::began,
             "material continuous factory begins through public base");
  const auto beforeWrongMaterial = snapshot(*rig);
  passed &= expect(
      session.preview(*finalLight).error ==
              MutationErrorCode::invalidSnapshot &&
          session.state() == ContinuousEditState::previewing &&
          snapshot(*rig) == beforeWrongMaterial,
      "material session rejects the wrong variant alternative without write");
  passed &= expect(session.preview(*finalMaterial).status ==
                           ContinuousStatus::previewApplied &&
                       session.finishFrame(inspectorFinish(1)).status ==
                           ContinuousStatus::committed &&
                       rig->editor->commands().materialSnapshot(*material) ==
                           finalMaterial &&
                       rig->editor->history().undoCount() == 1,
                   "material preview commits through variant-only dispatch");

  passed &= expect(session.beginLight(*light).status == ContinuousStatus::began,
                   "light continuous factory begins through public base");
  const auto beforeWrongLight = snapshot(*rig);
  passed &= expect(
      session.preview(*finalMaterial).error ==
              MutationErrorCode::invalidSnapshot &&
          session.state() == ContinuousEditState::previewing &&
          snapshot(*rig) == beforeWrongLight,
      "light session rejects the wrong variant alternative without write");
  passed &= expect(
      session.preview(*finalLight).status == ContinuousStatus::previewApplied &&
          session.finishFrame(inspectorFinish(2)).status ==
              ContinuousStatus::committed &&
          rig->editor->commands().lightSnapshot(*light) == finalLight &&
          rig->editor->history().undoCount() == 2,
      "light preview commits through variant-only dispatch");
  const HistoryInspection history =
      EditorStateInspector::inspect(rig->editor->history());
  passed &=
      expect(history.undoOldestToNewest[0].retainedCommand.kind ==
                     EditorCommandKind::editMaterial &&
                 history.undoOldestToNewest[1].retainedCommand.kind ==
                     EditorCommandKind::editLight,
             "variant dispatch retains concrete typed property command kinds");

  if (!expect(materialBytes > 1 && lightBytes > 1,
              "property retained byte counts are nontrivial"))
    return false;
  EditorLimits limits = kDefaultEditorLimits;
  limits.maxHistoryEntrySnapshotBytes =
      (materialBytes < lightBytes ? materialBytes : lightBytes) - 1;
  auto budget = makeRig("material_extensions.gltf", limits);
  if (!expect(budget != nullptr, "property budget fixture loads"))
    return false;
  const auto budgetMaterial =
      firstId<EditorObjectKind::material>(*budget->document);
  const auto budgetLight = firstId<EditorObjectKind::light>(*budget->document);
  auto budgetMaterialProperties =
      budgetMaterial
          ? budget->editor->commands().materialProperties(*budgetMaterial)
          : std::nullopt;
  auto budgetLightProperties =
      budgetLight ? budget->editor->commands().lightProperties(*budgetLight)
                  : std::nullopt;
  if (!expect(budgetMaterial.has_value() && budgetLight.has_value() &&
                  budgetMaterialProperties.has_value() &&
                  budgetLightProperties.has_value(),
              "property budget targets resolve"))
    return false;
  budgetMaterialProperties->metallicFactor = 0.5;
  budgetLightProperties->intensity = 11.0;
  const auto budgetMaterialFinal =
      budget->editor->commands().prepareMaterialSnapshot(
          *budgetMaterial, MaterialEdit{*budgetMaterialProperties});
  const auto budgetLightFinal = budget->editor->commands().prepareLightSnapshot(
      *budgetLight, *budgetLightProperties);
  if (!expect(budgetMaterialFinal.has_value() && budgetLightFinal.has_value(),
              "property budget values prepare"))
    return false;
  ContinuousEditSession &budgetSession = budget->editor->continuousEdit();
  passed &= expect(budgetSession.beginMaterial(*budgetMaterial).status ==
                       ContinuousStatus::began,
                   "oversize material session begins");
  const auto beforeMaterialBudget = snapshot(*budget);
  passed &= expect(budgetSession.preview(*budgetMaterialFinal).error ==
                           MutationErrorCode::snapshotBudgetExceeded &&
                       snapshot(*budget) == beforeMaterialBudget,
                   "oversize material preview rejects before mutation");
  passed &= expect(budgetSession.finishFrame(inspectorFinish(10)).status ==
                           ContinuousStatus::committed &&
                       budget->editor->history().undoCount() == 0,
                   "unchanged oversize material session ends without history");
  passed &= expect(budgetSession.beginLight(*budgetLight).status ==
                       ContinuousStatus::began,
                   "oversize light session begins");
  const auto beforeLightBudget = snapshot(*budget);
  passed &= expect(
      budgetSession.preview(*budgetLightFinal).error ==
              MutationErrorCode::snapshotBudgetExceeded &&
          snapshot(*budget) == beforeLightBudget &&
          budgetSession.finishFrame(inspectorFinish(11)).status ==
              ContinuousStatus::committed &&
          budget->editor->history().undoCount() == 0,
      "oversize light preview rejects before mutation and adds no history");
  return passed;
}

bool runEscapeCase(bool setReleaseFirst) {
  auto rig = makeRig();
  if (!rig)
    return false;
  const auto node = firstNode(*rig);
  const auto initial =
      node ? rig->editor->commands().nodeTransform(*node) : std::nullopt;
  if (!node || !initial || !selectRoot(*rig, *node))
    return false;
  ContinuousEditSession &session = rig->editor->continuousEdit();
  if (session.beginTransform(ContinuousEditKind::gizmoTransform, *node)
              .status != ContinuousStatus::began ||
      session.preview(translated(*initial, 1.0)).status !=
          ContinuousStatus::previewApplied)
    return false;
  const NodeTransform preview = *rig->editor->commands().nodeTransform(*node);
  ContinuousFrameSignals signals;
  signals.applicationFrame = 7;
  if (setReleaseFirst) {
    signals.physicalReleaseDelivered = true;
    signals.escapePressed = true;
  } else {
    signals.escapePressed = true;
    signals.physicalReleaseDelivered = true;
  }
  const ContinuousResult result = session.finishFrame(signals);
  const auto afterAbandon = snapshot(*rig);
  const bool base =
      result.status == ContinuousStatus::abandonedRetainingPreview &&
      result.state == ContinuousEditState::abandoned &&
      rig->editor->commands().nodeTransform(*node) == preview &&
      rig->editor->selection().snapshot().emptyValue() &&
      rig->editor->history().undoCount() == 0;
  ContinuousFrameSignals repeatEscape;
  repeatEscape.applicationFrame = 8;
  repeatEscape.escapePressed = true;
  ContinuousFrameSignals lateRelease = physicalRelease(9);
  return base &&
         session.finishFrame(repeatEscape).status == ContinuousStatus::noOp &&
         session.finishFrame(lateRelease).status == ContinuousStatus::noOp &&
         snapshot(*rig) == afterAbandon;
}

bool testEscapeArbitration() {
  bool passed = true;
  auto escapeOnly = makeRig();
  if (!expect(escapeOnly != nullptr, "Escape-only fixture loads"))
    return false;
  const auto node = firstNode(*escapeOnly);
  const auto initial =
      node ? escapeOnly->editor->commands().nodeTransform(*node) : std::nullopt;
  if (!expect(node.has_value() && initial.has_value() &&
                  selectRoot(*escapeOnly, *node),
              "Escape-only target selects"))
    return false;
  ContinuousEditSession &session = escapeOnly->editor->continuousEdit();
  passed &=
      expect(session.beginTransform(ContinuousEditKind::gizmoTransform, *node)
                         .status == ContinuousStatus::began &&
                 session.preview(translated(*initial, 1.0)).status ==
                     ContinuousStatus::previewApplied,
             "Escape-only gizmo preview applies");
  ContinuousFrameSignals escape;
  escape.applicationFrame = 1;
  escape.escapePressed = true;
  const ContinuousResult escaped = session.finishFrame(escape);
  passed &= expect(
      escaped.status == ContinuousStatus::abandonedRetainingPreview &&
          escapeOnly->editor->commands().nodeTransform(*node) ==
              translated(*initial, 1.0) &&
          escapeOnly->editor->selection().snapshot().emptyValue() &&
          escapeOnly->editor->history().undoCount() == 0,
      "O05 Escape retains preview, clears selection, and adds no history");
  passed &= expect(runEscapeCase(false),
                   "O06 Escape then release resolves to Escape");
  passed &=
      expect(runEscapeCase(true), "O06 release then Escape resolves to Escape");
  return passed;
}

bool testFocusAndFailureArbitration() {
  bool passed = true;
  auto suspended = makeRig();
  if (!expect(suspended != nullptr, "focus-loss fixture loads"))
    return false;
  const auto node = firstNode(*suspended);
  const auto initial =
      node ? suspended->editor->commands().nodeTransform(*node) : std::nullopt;
  if (!expect(node.has_value() && initial.has_value() &&
                  selectRoot(*suspended, *node),
              "focus-loss target selects"))
    return false;
  const EditorSelectionSnapshot selected =
      suspended->editor->selection().snapshot();
  ContinuousEditSession &session = suspended->editor->continuousEdit();
  passed &=
      expect(session.beginTransform(ContinuousEditKind::gizmoTransform, *node)
                         .status == ContinuousStatus::began &&
                 session.preview(translated(*initial, 1.0)).status ==
                     ContinuousStatus::previewApplied,
             "focus-loss preview applies");
  ContinuousFrameSignals lost;
  lost.applicationFrame = 10;
  lost.focusLost = true;
  lost.windowFocused = false;
  const ContinuousResult lostResult = session.finishFrame(lost);
  passed &=
      expect(lostResult.status == ContinuousStatus::suspended &&
                 lostResult.state == ContinuousEditState::suspendedUnfocused &&
                 suspended->editor->selection().snapshot() == selected &&
                 suspended->editor->history().undoCount() == 0 &&
                 suspended->editor->commands().nodeTransform(*node) ==
                     translated(*initial, 1.0),
             "O07 focus loss retains preview, selection, and attachment");
  ContinuousFrameSignals synthetic;
  synthetic.applicationFrame = 11;
  synthetic.windowFocused = false;
  synthetic.syntheticRelease = true;
  passed &= expect(session.finishFrame(synthetic).status ==
                           ContinuousStatus::suspended &&
                       suspended->editor->history().undoCount() == 0,
                   "unfocused synthetic release is ignored");
  passed &=
      expect(session.finishFrame(physicalRelease(12)).status ==
                     ContinuousStatus::noOp &&
                 suspended->editor->history().undoCount() == 0,
             "refocused late physical release cannot resume suspended history");
  ContinuousFrameSignals detached;
  detached.applicationFrame = 13;
  detached.selectionDetached = true;
  passed &= expect(session.finishFrame(detached).status ==
                           ContinuousStatus::abandonedRetainingPreview &&
                       suspended->editor->selection().snapshot().emptyValue() &&
                       session.finishFrame(physicalRelease(14)).status ==
                           ContinuousStatus::noOp,
                   "selection detach abandons suspended interaction and late "
                   "release no-ops");

  auto erasedRelease = makeRig();
  if (!expect(erasedRelease != nullptr, "queue-erased release fixture loads"))
    return false;
  const auto erasedNode = firstNode(*erasedRelease);
  const auto erasedInitial =
      erasedNode ? erasedRelease->editor->commands().nodeTransform(*erasedNode)
                 : std::nullopt;
  if (!erasedNode || !erasedInitial || !selectRoot(*erasedRelease, *erasedNode))
    return false;
  ContinuousEditSession &erasedSession =
      erasedRelease->editor->continuousEdit();
  passed &= expect(
      erasedSession
                  .beginTransform(ContinuousEditKind::gizmoTransform,
                                  *erasedNode)
                  .status == ContinuousStatus::began &&
          erasedSession.preview(translated(*erasedInitial, 1.0)).status ==
              ContinuousStatus::previewApplied,
      "queue-erased release preview applies");
  ContinuousFrameSignals erased;
  erased.applicationFrame = 20;
  erased.physicalReleaseDelivered = true;
  erased.focusLost = true;
  erased.windowFocused = false;
  passed &= expect(
      erasedSession.finishFrame(erased).status == ContinuousStatus::suspended &&
          erasedRelease->editor->history().undoCount() == 0,
      "same-frame focus loss suppresses a queued physical release");

  auto preceding = makeRig();
  if (!expect(preceding != nullptr, "preceding-frame release fixture loads"))
    return false;
  const auto precedingNode = firstNode(*preceding);
  const auto precedingInitial =
      precedingNode
          ? preceding->editor->commands().nodeTransform(*precedingNode)
          : std::nullopt;
  if (!precedingNode || !precedingInitial ||
      !selectRoot(*preceding, *precedingNode))
    return false;
  const EditorSelectionSnapshot precedingSelection =
      preceding->editor->selection().snapshot();
  ContinuousEditSession &precedingSession = preceding->editor->continuousEdit();
  passed &= expect(
      precedingSession
                  .beginTransform(ContinuousEditKind::gizmoTransform,
                                  *precedingNode)
                  .status == ContinuousStatus::began &&
          precedingSession.preview(translated(*precedingInitial, 1.0)).status ==
              ContinuousStatus::previewApplied &&
          precedingSession.finishFrame(physicalRelease(30)).status ==
              ContinuousStatus::committed &&
          preceding->editor->history().undoCount() == 1,
      "focused physical release commits at its frame end");
  ContinuousFrameSignals laterLoss;
  laterLoss.applicationFrame = 31;
  laterLoss.focusLost = true;
  laterLoss.windowFocused = false;
  passed &= expect(precedingSession.finishFrame(laterLoss).status ==
                           ContinuousStatus::noOp &&
                       preceding->editor->history().undoCount() == 1 &&
                       preceding->editor->selection().snapshot() ==
                           precedingSelection,
                   "later focus loss does not alter a preceding-frame commit");

  auto failures = makeRig();
  if (!expect(failures != nullptr, "continuous failure fixture loads"))
    return false;
  const auto failureNode = firstNode(*failures);
  const auto failureInitial =
      failureNode ? failures->editor->commands().nodeTransform(*failureNode)
                  : std::nullopt;
  if (!failureNode || !failureInitial || !selectRoot(*failures, *failureNode))
    return false;
  ContinuousEditSession &failureSession = failures->editor->continuousEdit();
  failures->fault.reset();
  passed &= expect(failures->fault.failOnCall(EditorFaultPoint::allocation, 1),
                   "factory allocation fault arms");
  const auto beforeFactoryFault = snapshot(*failures);
  passed &=
      expect(failureSession
                         .beginTransform(ContinuousEditKind::gizmoTransform,
                                         *failureNode)
                         .error == MutationErrorCode::allocationFailed &&
                 failureSession.state() == ContinuousEditState::idle &&
                 snapshot(*failures) == beforeFactoryFault,
             "injected factory failure preserves idle state and full editor");
  failures->fault.reset();
  passed &= expect(
      failureSession
              .beginTransform(ContinuousEditKind::gizmoTransform, *failureNode)
              .status == ContinuousStatus::began,
      "failure session begins after transient factory failure");
  failures->fault.reset();
  passed &= expect(failures->fault.failOnCall(EditorFaultPoint::allocation, 1),
                   "preview allocation fault arms");
  const auto beforePreviewFault = snapshot(*failures);
  passed &= expect(
      failureSession.preview(translated(*failureInitial, 1.0)).error ==
              MutationErrorCode::allocationFailed &&
          failureSession.state() == ContinuousEditState::previewing &&
          snapshot(*failures) == beforePreviewFault,
      "injected preview failure preserves state and keeps session usable");
  failures->fault.reset();
  passed &=
      expect(failureSession.preview(translated(*failureInitial, 1.0)).status ==
                 ContinuousStatus::previewApplied,
             "session accepts a later good preview");
  failures->fault.reset();
  passed &= expect(
      failures->fault.failOnCall(EditorFaultPoint::preCandidatePublish, 1),
      "finalization fault arms");
  const auto beforeFinalizeFault = snapshot(*failures);
  const std::size_t clockCalls = failures->clock.nowCallCount();
  const ContinuousResult finalizationFailure =
      failureSession.finishFrame(physicalRelease(40));
  passed &= expect(
      finalizationFailure.status == ContinuousStatus::rejected &&
          finalizationFailure.state == ContinuousEditState::abandoned &&
          finalizationFailure.error == MutationErrorCode::internalError &&
          snapshot(*failures) == beforeFinalizeFault &&
          failures->clock.nowCallCount() == clockCalls &&
          failures->editor->history().undoCount() == 0 &&
          failureSession.finishFrame(physicalRelease(41)).status ==
              ContinuousStatus::noOp,
      "injected finalization failure retains preview and preserves "
      "history/time");
  return passed;
}

bool testInitialFinalEquality() {
  auto many = makeRig();
  if (!expect(many != nullptr, "many-preview fixture loads"))
    return false;
  const auto manyNode = firstNode(*many);
  const auto manyInitial =
      manyNode ? many->editor->commands().nodeTransform(*manyNode)
               : std::nullopt;
  if (!expect(manyNode.has_value() && manyInitial.has_value(),
              "many-preview target resolves"))
    return false;
  ContinuousEditSession &manySession = many->editor->continuousEdit();
  bool passed = true;
  passed &=
      expect(manySession.beginTransform(ContinuousEditKind::inspectorTransform,
                                        *manyNode)
                         .status == ContinuousStatus::began &&
                 manySession.preview(translated(*manyInitial, 1.0)).status ==
                     ContinuousStatus::previewApplied &&
                 manySession.preview(translated(*manyInitial, 2.0)).status ==
                     ContinuousStatus::previewApplied &&
                 manySession.preview(translated(*manyInitial, 3.0)).status ==
                     ContinuousStatus::previewApplied &&
                 manySession.finishFrame(inspectorFinish(1)).status ==
                     ContinuousStatus::committed &&
                 many->editor->history().undoCount() == 1 &&
                 many->editor->commands().nodeTransform(*manyNode) ==
                     translated(*manyInitial, 3.0),
             "many successful previews create exactly one retained command");

  auto rig = makeRig();
  if (!expect(rig != nullptr, "initial/final equality fixture loads"))
    return false;
  const auto node = firstNode(*rig);
  const auto initial =
      node ? rig->editor->commands().nodeTransform(*node) : std::nullopt;
  if (!expect(node.has_value() && initial.has_value(),
              "initial/final equality target resolves"))
    return false;
  ContinuousEditSession &session = rig->editor->continuousEdit();
  const std::uint64_t beforeRevision = rig->document->revision();
  passed &= expect(
      session.beginTransform(ContinuousEditKind::inspectorTransform, *node)
                  .status == ContinuousStatus::began &&
          session.preview(*initial).status ==
              ContinuousStatus::previewUnchanged &&
          session.finishFrame(inspectorFinish(1)).status ==
              ContinuousStatus::committed &&
          rig->editor->history().undoCount() == 0 &&
          rig->document->revision() == beforeRevision,
      "pure unchanged final creates zero history and zero revision");
  passed &= expect(
      session.beginTransform(ContinuousEditKind::inspectorTransform, *node)
                  .status == ContinuousStatus::began &&
          session.preview(translated(*initial, 1.0)).status ==
              ContinuousStatus::previewApplied &&
          session.preview(*initial).status ==
              ContinuousStatus::previewApplied &&
          session.finishFrame(inspectorFinish(2)).status ==
              ContinuousStatus::committed &&
          rig->editor->history().undoCount() == 0 &&
          rig->editor->commands().nodeTransform(*node) == *initial &&
          rig->document->revision() == beforeRevision + 2,
      "immutable initial/final equality creates zero history without "
      "rewinding preview revisions");
  return passed;
}

} // namespace

int main() {
  bool passed = true;
  passed &= testDiscreteTransform();
  passed &= testMergeBoundariesAndFalseAbsorb();
  passed &= testBudgetAndPreviewAtomicity();
  passed &= testVariantDispatch();
  passed &= testEscapeArbitration();
  passed &= testFocusAndFailureArbitration();
  passed &= testInitialFinalEquality();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
