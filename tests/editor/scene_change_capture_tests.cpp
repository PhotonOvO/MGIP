#include <mgif/scene_editor/SceneChangeCapture.h>
#include <mgif/scene_editor/SceneCommands.h>
#include <mgif/scene_editor/commands/EditorCommand.h>
#include <mgif/scene_editor/commands/EditorHistory.h>

#if defined(MGIF_P5_LIVE_PRODUCER_TESTS)
#include <mgif/scene_editor/GltfDocumentIO.h>
#include <mgif/scene_editor/commands/HierarchyCommands.h>
#include <mgif/scene_editor/commands/PropertyCommands.h>
#include <mgif/scene_editor/commands/TransformCommands.h>
#include <mgif/scene_editor/testing/EditorStateInspector.h>
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace demo::editor;

bool expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
  }
  return condition;
}

SceneDigest digest(std::string_view text) {
  return SceneChangeCanonicalizer::computeSha256(
      std::span<const std::uint8_t>(
          reinterpret_cast<const std::uint8_t *>(text.data()), text.size()));
}

StableSceneId id(EditorObjectKind kind, std::uint32_t slot,
                 std::uint32_t generation = 1u) {
  return StableSceneId{kind, EditorDocumentAuthority{41u}, slot, generation};
}

SceneCaptureSnapshot snapshot(std::uint64_t revision,
                              std::uint64_t activation,
                              std::uint64_t root,
                              std::optional<StableSceneId> active,
                              std::string_view documentTag,
                              std::string_view projectionTag) {
  SceneCaptureSnapshot value;
  value.authority = EditorDocumentAuthority{41u};
  value.documentRevision = revision;
  value.sceneActivationEpoch = activation;
  value.rootExternalStateSerial = root;
  value.activeScene = active;
  value.documentDigest = digest(documentTag);
  value.projectionDigest = digest(projectionTag);
  return value;
}

SceneNodeTransformValue transform(double x) {
  SceneTrsValue value;
  value.translation[0] = x;
  return SceneNodeTransformValue{value};
}

SceneFieldChange transformChange(std::uint64_t serial, double before,
                                 double after) {
  SceneFieldChange change;
  change.target = id(EditorObjectKind::node, 2u);
  change.changeClass = SceneChangeClass::Transform;
  change.action = SceneChangeAction::Update;
  change.field = SceneField::NodeLocalTransform;
  change.mutationSerial = serial;
  change.beforeValue = transform(before);
  change.afterValue = transform(after);
  return change;
}

SceneMutationEvidence evidence(const SceneCaptureSnapshot &before,
                               const SceneCaptureSnapshot &after,
                               std::uint64_t serial) {
  SceneMutationEvidence value;
  value.sourceMutation.baseDocumentRevision = before.documentRevision;
  value.sourceMutation.targetDocumentRevision = after.documentRevision;
  value.sourceMutation.mutationToken = SceneMutationToken{
      before.authority, after.documentRevision, serial};
  value.sourceMutation.baseDocumentDigest = before.documentDigest;
  value.sourceMutation.targetDocumentDigest = after.documentDigest;
  return value;
}

bool testExactOneFieldAndAtomicCamera() {
  const StableSceneId scene = id(EditorObjectKind::scene, 0u);
  const SceneCaptureSnapshot before = snapshot(3u, 7u, 5u, scene, "d3", "p3");
  const SceneCaptureSnapshot after = snapshot(4u, 7u, 5u, scene, "d4", "p4");
  SceneMutationEvidence exact = evidence(before, after, 101u);
  exact.exactChanges.push_back(transformChange(101u, 0.0, 4.0));
  const auto finalized =
      SceneChangeCapture::finalize(before, after, {exact});
  bool passed = expect(finalized.succeeded(), "one-field capture canonicalizes");
  passed &= expect(finalized.changeSet &&
                       finalized.changeSet->canonicalChanges.size() == 1u &&
                       finalized.changeSet->canonicalChanges[0].field ==
                           SceneField::NodeLocalTransform,
                   "one-field capture remains exactly one local field");

  SceneMutationEvidence camera = evidence(before, after, 102u);
  camera.exactChanges.push_back(transformChange(102u, 0.0, 1.0));
  SceneFieldChange parameters;
  parameters.target = id(EditorObjectKind::camera, 0u);
  parameters.changeClass = SceneChangeClass::Camera;
  parameters.action = SceneChangeAction::Update;
  parameters.field = SceneField::CameraParameters;
  parameters.mutationSerial = 102u;
  SceneCameraValue oldCamera;
  SceneCameraValue newCamera;
  newCamera.yfov = 0.5;
  parameters.beforeValue = oldCamera;
  parameters.afterValue = newCamera;
  camera.exactChanges.push_back(parameters);
  const auto atomic = SceneChangeCapture::finalize(before, after, {camera});
  passed &= expect(
      atomic.succeeded() && atomic.changeSet &&
          atomic.changeSet->canonicalChanges.size() == 2u &&
          atomic.changeSet->header.firstMutationSerial == 102u &&
          atomic.changeSet->header.lastMutationSerial == 102u &&
          atomic.changeSet->header.sourceMutations.size() == 1u &&
          atomic.changeSet->header.sourceMutations[0].mutationToken.serial ==
              102u,
      "camera-from-view evidence is one revision/token with two fields");
  return passed;
}

bool testIndependentClockEnvelopes() {
  const StableSceneId first = id(EditorObjectKind::scene, 0u);
  const StableSceneId second = id(EditorObjectKind::scene, 1u);
  const SceneCaptureSnapshot activationBefore =
      snapshot(8u, 2u, 9u, first, "same-doc", "old-projection");
  const SceneCaptureSnapshot activationAfter =
      snapshot(8u, 3u, 9u, second, "same-doc", "new-projection");
  const auto activation = SceneChangeCapture::finalizeActivation(
      activationBefore, activationAfter, 301u);
  bool passed = expect(
      activation.succeeded() && activation.changeSet &&
          activation.changeSet->header.domain ==
              SceneChangeDomain::SceneActivation &&
          !activation.changeSet->header.mutationToken &&
          activation.changeSet->header.sourceMutations.empty(),
      "activation advances only its isolated envelope");

  SceneCaptureSnapshot overflow = activationBefore;
  overflow.sceneActivationEpoch =
      std::numeric_limits<std::uint64_t>::max();
  SceneCaptureSnapshot wrapped = overflow;
  wrapped.activeScene = second;
  const auto overflowResult =
      SceneChangeCapture::finalizeActivation(overflow, wrapped, 302u);
  passed &= expect(
      !overflowResult.succeeded() &&
          overflowResult.validation.error ==
              SceneChangeValidationError::MonotonicOverflow,
      "activation maximum rejects without publication");

  const SceneCaptureSnapshot rootBefore =
      snapshot(11u, 4u, 17u, second, "root-doc", "root-old");
  const SceneCaptureSnapshot rootAfter =
      snapshot(11u, 4u, 18u, second, "root-doc", "root-new");
  const auto root = SceneChangeCapture::finalizeRootState(
      rootBefore, rootAfter, transform(0.0), transform(3.0), 401u);
  passed &= expect(
      root.succeeded() && root.changeSet &&
          root.changeSet->header.domain == SceneChangeDomain::SceneRootState &&
          root.changeSet->header.baseDocumentRevision ==
              root.changeSet->header.targetDocumentRevision &&
          root.changeSet->header.baseSceneActivationEpoch ==
              root.changeSet->header.targetSceneActivationEpoch &&
          root.changeSet->canonicalChanges.size() == 1u &&
          root.changeSet->canonicalChanges[0].field ==
              SceneField::SceneRootTransform,
      "root state advances only its serial and one exact root value");
  SceneCaptureSnapshot invalidRoot = rootAfter;
  ++invalidRoot.documentRevision;
  passed &= expect(
      !SceneChangeCapture::finalizeRootState(
           rootBefore, invalidRoot, transform(0.0), transform(3.0), 402u)
           .succeeded(),
      "root-state rejects unrelated document publication");
  return passed;
}

bool testDerivedEffectsAndStructuralSnapshot() {
  SceneFieldChange visibility;
  visibility.target = id(EditorObjectKind::node, 0u);
  visibility.changeClass = SceneChangeClass::Visibility;
  visibility.action = SceneChangeAction::Update;
  visibility.field = SceneField::NodeLocalVisibility;
  visibility.mutationSerial = 55u;
  visibility.beforeValue = SceneVisibilityValue{true};
  visibility.afterValue = SceneVisibilityValue{false};
  const std::vector<SceneHierarchyNodeEvidence> hierarchy{
      {id(EditorObjectKind::node, 8u), id(EditorObjectKind::node, 1u), true},
      {id(EditorObjectKind::node, 1u), id(EditorObjectKind::node, 9u), true},
      {id(EditorObjectKind::node, 3u), std::nullopt, true},
      {id(EditorObjectKind::node, 9u), std::nullopt, true},
  };
  visibility.target = id(EditorObjectKind::node, 9u);
  const SceneDerivedEffects visibilityEffects =
      SceneChangeCapture::deriveHierarchyEffects({&visibility, 1u}, hierarchy);
  bool passed = expect(
      visibilityEffects.visibilityTargets ==
          std::vector<StableSceneId>{id(EditorObjectKind::node, 1u),
                                     id(EditorObjectKind::node, 8u)} &&
          visibilityEffects.transformTargets.empty(),
      "visibility effects are parent-before-child despite stable-ID order");
  SceneFieldChange changedTransform = transformChange(55u, 0.0, 2.0);
  changedTransform.target = id(EditorObjectKind::node, 9u);
  const SceneDerivedEffects transformEffects =
      SceneChangeCapture::deriveHierarchyEffects({&changedTransform, 1u},
                                                 hierarchy);
  passed &= expect(transformEffects.transformTargets ==
                       visibilityEffects.visibilityTargets,
                   "transform effects use the same deterministic hierarchy");

  const StableSceneId scene = id(EditorObjectKind::scene, 0u);
  const SceneCaptureSnapshot before = snapshot(20u, 3u, 7u, scene, "sd0", "sp0");
  SceneCaptureSnapshot after = snapshot(21u, 3u, 8u, scene, "sd1", "sp1");
  ImmutableSceneSnapshotRef immutable;
  immutable.authority = after.authority;
  immutable.documentRevision = after.documentRevision;
  immutable.sceneActivationEpoch = after.sceneActivationEpoch;
  immutable.rootExternalStateSerial = after.rootExternalStateSerial;
  immutable.activeScene = after.activeScene;
  immutable.documentDigest = after.documentDigest;
  immutable.projectionDigest = after.projectionDigest;
  immutable.bytes = {9u, 8u, 7u, 6u};
  immutable.contentDigest =
      SceneChangeCanonicalizer::computeSha256(immutable.bytes);
  after.immutableTarget = immutable;
  SceneMutationEvidence structural = evidence(before, after, 601u);
  structural.structuralEvidenceComplete = false;
  structural.stableIdRemap.push_back(
      {std::nullopt, id(EditorObjectKind::node, 9u)});
  const auto full = SceneChangeCapture::finalize(
      before, after, {structural}, FullRebuildReason::StructuralTopology);
  passed &= expect(
      full.succeeded() && full.changeSet && full.changeSet->fullSnapshot &&
          full.changeSet->header.domain == SceneChangeDomain::FullSnapshot &&
          full.changeSet->fullSnapshot->reason ==
              FullRebuildReason::StructuralTopology &&
          full.changeSet->fullSnapshot->immutableTarget.bytes ==
              immutable.bytes &&
          full.changeSet->stableIdRemap ==
              full.changeSet->fullSnapshot->stableIdRemap,
      "incomplete hierarchy becomes an owning reason-coded full snapshot");
  structural.structuralEvidenceComplete = false;
  passed &= expect(
      !SceneChangeCapture::finalize(before, after, {structural}).succeeded(),
      "incomplete hierarchy cannot masquerade as incremental evidence");
  return passed;
}

bool testLosslessTransportValues() {
  const StableSceneId scene = id(EditorObjectKind::scene, 0u);
  const SceneCaptureSnapshot before = snapshot(30u, 4u, 1u, scene, "td0", "tp0");
  const SceneCaptureSnapshot after = snapshot(31u, 4u, 1u, scene, "td1", "tp1");
  SceneMutationEvidence source = evidence(before, after, 701u);
  source.exactChanges.push_back(transformChange(701u, 1.0, 5.0));
  source.stableIdRemap.push_back(
      {id(EditorObjectKind::material, 1u),
       id(EditorObjectKind::material, 1u, 2u)});
  MutationResult mutation;
  mutation.status = MutationStatus::applied;
  mutation.error = MutationErrorCode::none;
  mutation.changes = source;
  mutation.remap.materials.push_back(
      {editorObjectId(*source.stableIdRemap[0].oldId),
       editorObjectId(*source.stableIdRemap[0].newId)});
  mutation.transactionToken = MutationToken{before.authority, 31u, 701u};

  CommandResult command;
  command.status = CommandStatus::applied;
  command.error = mutation.error;
  command.changes = mutation.changes;
  command.remap = mutation.remap;
  command.transactionToken = mutation.transactionToken;
  HistoryResult history;
  history.status = HistoryStatus::moved;
  history.command = command;
  return expect(
      command.changes == source && history.command.changes == source &&
          history.command.remap.materials.size() == 1u &&
          history.command.transactionToken == mutation.transactionToken &&
          history.command.changes.sourceMutation.baseDocumentDigest ==
              before.documentDigest &&
          history.command.changes.sourceMutation.targetDocumentDigest ==
              after.documentDigest,
      "mutation, command, and embedded history values preserve every field");
}

bool testContiguousBatching() {
  const StableSceneId scene = id(EditorObjectKind::scene, 0u);
  const SceneCaptureSnapshot before =
      snapshot(40u, 8u, 3u, scene, "batch-d0", "batch-p0");
  const SceneCaptureSnapshot middle =
      snapshot(41u, 8u, 3u, scene, "batch-d1", "batch-p1");
  const SceneCaptureSnapshot after =
      snapshot(42u, 8u, 3u, scene, "batch-d2", "batch-p2");
  SceneMutationEvidence first = evidence(before, middle, 801u);
  first.exactChanges.push_back(transformChange(801u, 0.0, 2.0));
  SceneMutationEvidence second = evidence(middle, after, 802u);
  second.exactChanges.push_back(transformChange(802u, 2.0, 6.0));
  const auto batched = SceneChangeCapture::finalize(
      before, after, {first, second});
  bool passed = expect(
      batched.succeeded() && batched.changeSet &&
          batched.changeSet->header.sourceMutations.size() == 2u &&
          batched.changeSet->canonicalChanges.size() == 1u &&
          batched.changeSet->canonicalChanges[0].beforeValue ==
              first.exactChanges[0].beforeValue &&
          batched.changeSet->canonicalChanges[0].afterValue ==
              second.exactChanges[0].afterValue,
      "contiguous evidence batches retain their chain and final-write algebra");

  second.sourceMutation.baseDocumentDigest = digest("discontinuous");
  passed &= expect(
      !SceneChangeCapture::finalize(before, after, {first, second}).succeeded(),
      "discontinuous producer evidence cannot be batched");
  return passed;
}

#if defined(MGIF_P5_LIVE_PRODUCER_TESTS)

std::filesystem::path fixture(const char *name) {
  return std::filesystem::path(MGIF_TEST_SOURCE_DIR) / "tests" / "fixtures" /
         "gltf_editor" / name;
}

std::unique_ptr<GltfDocument> loadDocument(const char *name) {
  auto loaded = GltfDocumentIO::loadCandidate(fixture(name));
  return loaded.succeeded() ? std::move(loaded).takeCandidate() : nullptr;
}

std::uint64_t activeRootSerial(const GltfDocument &document) {
  const auto active = document.activeSceneId();
  return active ? document.sceneRootTransformState(*active).externalStateSerial
                : 0u;
}

bool noPublishedEvidence(const MutationResult &result) {
  if (!result.changedIds.empty() || !result.changes.exactChanges.empty() ||
      !result.changes.stableIdRemap.empty() || result.changes.fullSnapshot ||
      result.fullSnapshotReason || result.sceneActivationTransition ||
      result.rootStateTransition ||
      result.changes.sourceMutation.baseDocumentRevision != 0u ||
      result.changes.sourceMutation.targetDocumentRevision != 0u ||
      result.changes.sourceMutation.mutationToken.valid() ||
      sceneDigestPresent(result.changes.sourceMutation.baseDocumentDigest) ||
      sceneDigestPresent(result.changes.sourceMutation.targetDocumentDigest)) {
    return false;
  }
  for (std::size_t kind = 1u;
       kind < static_cast<std::size_t>(EditorObjectKind::count); ++kind) {
    if (!result.remap.forKind(static_cast<EditorObjectKind>(kind)).empty()) {
      return false;
    }
  }
  return true;
}

bool forwardEvidence(const CommandResult &result) {
  const SourceMutationFence &fence = result.changes.sourceMutation;
  return result.status == CommandStatus::applied &&
         result.afterRevision == result.beforeRevision + 1u &&
         fence.baseDocumentRevision == result.beforeRevision &&
         fence.targetDocumentRevision == result.afterRevision &&
         fence.mutationToken.valid() &&
         fence.mutationToken.serial == result.transactionToken.serial &&
         fence.mutationToken.revision == result.afterRevision &&
         sceneDigestPresent(fence.baseDocumentDigest) &&
         sceneDigestPresent(fence.targetDocumentDigest);
}

class ManualClock final : public EditorClock {
public:
  [[nodiscard]] EditorTimePoint now() const noexcept override { return {}; }
};

class OneShotFault final : public EditorFaultInjector {
public:
  OneShotFault(EditorFaultPoint point, std::size_t oneBasedCall) noexcept
      : m_point(point), m_oneBasedCall(oneBasedCall) {}

  [[nodiscard]] bool shouldFail(EditorFaultPoint point) noexcept override {
    return point == m_point && ++m_calls == m_oneBasedCall;
  }

private:
  EditorFaultPoint m_point;
  std::size_t m_oneBasedCall;
  std::size_t m_calls{0u};
};

bool testLiveExactFieldAndCameraCapture() {
  auto transformDocument = loadDocument("minimal_scene.gltf");
  if (!expect(transformDocument != nullptr, "live transform fixture loads")) {
    return false;
  }
  SceneCommands transformCommands(*transformDocument);
  const NodeId node = *NodeId::from(
      transformDocument->idAt(EditorObjectKind::node, 0u));
  NodeTransform transformValue = *transformCommands.nodeTransform(node);
  transformValue.translation[0] += 2.0;
  const std::uint64_t transformBase = transformDocument->revision();
  const MutationResult transformed =
      transformCommands.setNodeTransform(node, transformValue);
  bool passed = expect(
      transformed.applied() && transformed.beforeRevision == transformBase &&
          transformed.afterRevision == transformBase + 1u &&
          transformed.changedIds ==
              std::vector<EditorObjectId>{node.untyped()} &&
          transformed.changes.exactChanges.size() == 1u &&
          transformed.changes.exactChanges[0].field ==
              SceneField::NodeLocalTransform &&
          transformed.changes.exactChanges[0].target ==
              stableSceneId(node.untyped()) &&
          transformed.changes.sourceMutation.mutationToken.serial ==
              transformed.transactionToken.serial &&
          transformed.changes.sourceMutation.targetDocumentDigest ==
              transformDocument->documentDigest(),
      "live one-field transform publishes one exact local field and fence");

  const MutationResult hidden =
      transformCommands.setVisibility(node.untyped(), false);
  passed &= expect(
      hidden.applied() && hidden.changes.exactChanges.size() == 1u &&
          hidden.changes.exactChanges[0].field ==
              SceneField::NodeLocalVisibility &&
          hidden.changedIds == std::vector<EditorObjectId>{node.untyped()},
      "live visibility publishes only the edited local visibility field");

  auto cameraDocument = loadDocument("feature_preservation.gltf");
  if (!expect(cameraDocument != nullptr &&
                  cameraDocument->objectCount(EditorObjectKind::camera) != 0u &&
                  cameraDocument->objectCount(EditorObjectKind::node) != 0u,
              "live camera fixture has camera and node identities")) {
    return false;
  }
  SceneCommands cameraCommands(*cameraDocument);
  const CameraId camera = *CameraId::from(
      cameraDocument->idAt(EditorObjectKind::camera, 0u));
  const NodeId cameraNode =
      *NodeId::from(cameraDocument->idAt(EditorObjectKind::node, 0u));
  CameraProperties properties = *cameraCommands.cameraProperties(camera);
  NodeTransform nodeValue = *cameraCommands.nodeTransform(cameraNode);
  properties.yfov += 0.05;
  nodeValue.translation[1] += 0.25;
  const MutationResult cameraFromView = cameraCommands.setCameraFromView(
      camera, cameraNode, properties, nodeValue);
  std::vector<SceneField> fields;
  for (const SceneFieldChange &change : cameraFromView.changes.exactChanges) {
    fields.push_back(change.field);
  }
  std::sort(fields.begin(), fields.end());
  passed &= expect(
      cameraFromView.applied() && fields ==
          std::vector<SceneField>{SceneField::NodeLocalTransform,
                                  SceneField::CameraParameters} &&
          cameraFromView.changes.sourceMutation.targetDocumentRevision ==
              cameraFromView.afterRevision &&
          std::all_of(cameraFromView.changes.exactChanges.begin(),
                      cameraFromView.changes.exactChanges.end(),
                      [&](const SceneFieldChange &change) {
                        return change.mutationSerial ==
                               cameraFromView.transactionToken.serial;
                      }),
      "camera-from-view publishes camera and node transform atomically");
  return passed;
}

bool testLiveMaterialVariantAndHierarchyCapture() {
  auto document = loadDocument("material_extensions.gltf");
  if (!expect(document != nullptr &&
                  document->objectCount(EditorObjectKind::material) != 0u &&
                  document->objectCount(EditorObjectKind::primitive) != 0u,
              "material and variant fixture loads")) {
    return false;
  }
  SceneCommands commands(*document);
  const MaterialId material = *MaterialId::from(
      document->idAt(EditorObjectKind::material, 0u));
  MaterialProperties properties = *commands.materialProperties(material);
  properties.roughnessFactor =
      properties.roughnessFactor == 0.375 ? 0.625 : 0.375;
  const MutationResult materialResult = commands.setMaterial(material, properties);
  bool passed = expect(
      materialResult.applied() &&
          materialResult.changes.exactChanges.size() == 1u &&
          materialResult.changes.exactChanges[0].field ==
              SceneField::MaterialParameters,
      "material setter publishes only its final parameter record");

  const PrimitiveId primitive = *PrimitiveId::from(
      document->idAt(EditorObjectKind::primitive, 0u));
  const std::uint32_t variant = 0u;
  const auto current = commands.primitiveVariantMaterial(primitive, variant);
  std::optional<MaterialId> target;
  for (std::size_t index = 0u;
       index < document->objectCount(EditorObjectKind::material); ++index) {
    const MaterialId candidate = *MaterialId::from(
        document->idAt(EditorObjectKind::material, index));
    if (current != std::optional<MaterialId>{candidate}) {
      target = candidate;
      break;
    }
  }
  if (!target) {
    target = current ? std::nullopt : std::optional<MaterialId>{material};
  }
  const MutationResult variantResult =
      commands.setPrimitiveVariant(primitive, variant, target);
  passed &= expect(
      variantResult.applied() &&
          variantResult.changes.exactChanges.size() == 1u &&
          variantResult.changes.exactChanges[0].field ==
              SceneField::PrimitiveMaterialBinding &&
          std::get<ScenePrimitiveMaterialBindingValue>(
              *variantResult.changes.exactChanges[0].afterValue)
                  .material ==
              (target ? std::optional<StableSceneId>{
                            stableSceneId(target->untyped())}
                      : std::nullopt),
      "variant edit captures its resolved final primitive-material binding");

  if (variantResult.applied()) {
    const auto active = document->activeSceneId();
    const StableSceneId activeId = stableSceneId(active->untyped());
    SceneCaptureSnapshot before = snapshot(
        variantResult.beforeRevision, document->sceneActivationEpoch(),
        activeRootSerial(*document), activeId, "variant-before", "variant-p0");
    SceneCaptureSnapshot after = snapshot(
        variantResult.afterRevision, document->sceneActivationEpoch(),
        activeRootSerial(*document), activeId, "variant-after", "variant-p1");
    before.authority = document->documentAuthority();
    after.authority = document->documentAuthority();
    before.activeScene = activeId;
    after.activeScene = activeId;
    before.documentDigest =
        variantResult.changes.sourceMutation.baseDocumentDigest;
    after.documentDigest =
        variantResult.changes.sourceMutation.targetDocumentDigest;
    const auto canonical =
        SceneChangeCapture::finalize(before, after, {variantResult.changes});
    passed &= expect(
        canonical.succeeded() && canonical.changeSet &&
            canonical.changeSet->canonicalChanges.size() == 1u &&
            !std::get<ScenePrimitiveMaterialBindingValue>(
                 *canonical.changeSet->canonicalChanges[0].afterValue)
                 .sourceVariant,
        "variant provenance canonicalizes to one final binding value");
  }

  const SceneId scene = document->currentSceneId();
  const MutationResult structural = commands.addNode(scene, "P5 child");
  passed &= expect(
      structural.applied() && !structural.changes.structuralEvidenceComplete &&
          structural.fullSnapshotReason ==
              FullRebuildReason::StructuralTopology &&
          !structural.changes.stableIdRemap.empty() &&
          std::any_of(structural.changes.stableIdRemap.begin(),
                      structural.changes.stableIdRemap.end(),
                      [](const StableIdRemapEvidence &entry) {
                        return !entry.oldId && entry.newId;
                      }),
      "incomplete hierarchy capture requests a reason-coded full snapshot");
  return passed;
}

bool testLiveIndependentClocksAndFailures() {
  auto activationDocument = loadDocument("multi_scene_oracle.gltf");
  if (!expect(activationDocument != nullptr &&
                  activationDocument->objectCount(EditorObjectKind::scene) > 1u,
              "activation fixture loads two scenes")) {
    return false;
  }
  const SceneId nextScene = *SceneId::from(
      activationDocument->idAt(EditorObjectKind::scene, 1u));
  const std::uint64_t revision = activationDocument->revision();
  const std::uint64_t epoch = activationDocument->sceneActivationEpoch();
  const SceneActivationResult activation =
      activationDocument->activateCurrentScene(nextScene);
  bool passed = expect(
      activation.changed() && activation.baseEpoch == epoch &&
          activation.targetEpoch == epoch + 1u &&
          activationDocument->revision() == revision &&
          activationDocument->sceneActivationEpoch() == epoch + 1u,
      "active-scene success advances only the checked activation epoch");

  EditorObjectId foreignId = nextScene.untyped();
  ++foreignId.authority.value;
  const SceneId foreignScene = *SceneId::from(foreignId);
  const auto beforeInvalid = activationDocument->activeSceneId();
  const SceneActivationResult invalid =
      activationDocument->activateCurrentScene(foreignScene);
  passed &= expect(
      invalid.status == SceneActivationStatus::InvalidScene &&
          activationDocument->activeSceneId() == beforeInvalid &&
          activationDocument->revision() == revision &&
          activationDocument->sceneActivationEpoch() == epoch + 1u,
      "invalid activation rejects without publication");

  const SceneId firstScene = *SceneId::from(
      activationDocument->idAt(EditorObjectKind::scene, 0u));
  passed &= expect(
      activationDocument->seedSceneActivationEpochForTesting(
          std::numeric_limits<std::uint64_t>::max()),
      "activation overflow test seed succeeds");
  const SceneActivationResult overflow =
      activationDocument->activateCurrentScene(firstScene);
  passed &= expect(
      overflow.status == SceneActivationStatus::EpochExhausted &&
          activationDocument->activeSceneId() == beforeInvalid &&
          activationDocument->revision() == revision &&
          activationDocument->sceneActivationEpoch() ==
              std::numeric_limits<std::uint64_t>::max(),
      "activation overflow rejects without wrapping or publishing");

  auto rootDocument = loadDocument("minimal_scene.gltf");
  SceneCommands rootCommands(*rootDocument);
  const SceneId rootScene = rootDocument->currentSceneId();
  const std::uint64_t rootRevision = rootDocument->revision();
  const std::uint64_t rootEpoch = rootDocument->sceneActivationEpoch();
  const SceneRootTransformState rootBefore =
      rootDocument->sceneRootTransformState(rootScene);
  SceneRootTransform rootValue = rootBefore.current;
  rootValue.translation[0] += 3.0;
  const MutationResult root =
      rootCommands.previewSceneRootTransform(rootScene, rootValue);
  passed &= expect(
      root.applied() && root.rootStateTransition &&
          root.rootStateTransition->baseExternalStateSerial ==
              rootBefore.externalStateSerial &&
          root.rootStateTransition->targetExternalStateSerial ==
              rootBefore.externalStateSerial + 1u &&
          rootDocument->revision() == rootRevision &&
          rootDocument->sceneActivationEpoch() == rootEpoch &&
          root.changedIds.empty() &&
          root.changes.exactChanges.empty() &&
          !root.changes.sourceMutation.mutationToken.valid(),
      "root preview publishes a separate root-serial transition only");

  auto overflowDocument = loadDocument("minimal_scene.gltf");
  SceneCommands overflowCommands(*overflowDocument);
  const SceneId overflowScene = overflowDocument->currentSceneId();
  const auto seeded = EditorStateTestAccess::seedCounterMaximum(
      *overflowDocument,
      {ExhaustionCounterKind::externalStateSerial, std::nullopt,
       overflowScene});
  SceneRootTransform overflowValue =
      overflowDocument->sceneRootTransform(overflowScene);
  overflowValue.translation[1] += 4.0;
  const MutationResult rootOverflow =
      overflowCommands.previewSceneRootTransform(overflowScene, overflowValue);
  passed &= expect(
      seeded.seeded() && rootOverflow.rejected() &&
          rootOverflow.error ==
              MutationErrorCode::externalStateSerialExhausted &&
          noPublishedEvidence(rootOverflow) &&
          overflowDocument->sceneRootTransformState(overflowScene)
                  .externalStateSerial ==
              std::numeric_limits<std::uint64_t>::max(),
      "root-serial overflow rejects with zero publication evidence");

  auto failedCandidateDocument = loadDocument("minimal_scene.gltf");
  OneShotFault candidateFault(EditorFaultPoint::preCandidatePublish, 1u);
  SceneCommands failedCandidateCommands(*failedCandidateDocument,
                                         kDefaultEditorLimits,
                                         &candidateFault);
  const NodeId failedNode = *NodeId::from(
      failedCandidateDocument->idAt(EditorObjectKind::node, 0u));
  NodeTransform failedTransform =
      *failedCandidateCommands.nodeTransform(failedNode);
  failedTransform.translation[2] += 2.0;
  const SceneDigest candidateDigest = failedCandidateDocument->documentDigest();
  const std::uint64_t candidateRevision = failedCandidateDocument->revision();
  const MutationResult failedCandidate =
      failedCandidateCommands.setNodeTransform(failedNode, failedTransform);
  passed &= expect(
      failedCandidate.rejected() && noPublishedEvidence(failedCandidate) &&
          failedCandidateDocument->revision() == candidateRevision &&
          failedCandidateDocument->documentDigest() == candidateDigest,
      "failed candidate path publishes no document state or evidence");

  auto failedPublishDocument = loadDocument("minimal_scene.gltf");
  OneShotFault publicationFault(EditorFaultPoint::allocation, 2u);
  SceneCommands failedPublishCommands(*failedPublishDocument,
                                       kDefaultEditorLimits,
                                       &publicationFault);
  const NodeId publishNode = *NodeId::from(
      failedPublishDocument->idAt(EditorObjectKind::node, 0u));
  NodeTransform publishTransform =
      *failedPublishCommands.nodeTransform(publishNode);
  publishTransform.translation[2] += 5.0;
  const SceneDigest publishDigest = failedPublishDocument->documentDigest();
  const std::uint64_t publishRevision = failedPublishDocument->revision();
  const MutationResult failedPublish =
      failedPublishCommands.setNodeTransform(publishNode, publishTransform);
  passed &= expect(
      failedPublish.rejected() && noPublishedEvidence(failedPublish) &&
          failedPublishDocument->revision() == publishRevision &&
          failedPublishDocument->documentDigest() == publishDigest,
      "failed prepublication callback leaves candidate and evidence unpublished");
  return passed;
}

bool testLiveHistoryForwardEvidence() {
  auto document = loadDocument("minimal_scene.gltf");
  if (!expect(document != nullptr, "history fixture loads")) {
    return false;
  }
  SceneCommands commands(*document);
  EditorSelectionState selection;
  EditorCommandContext context{*document, commands, selection};
  ManualClock clock;
  EditorHistory history(context, clock);
  const NodeId node =
      *NodeId::from(document->idAt(EditorObjectKind::node, 0u));
  const NodeTransform before = *commands.nodeTransform(node);
  NodeTransform after = before;
  after.translation[0] += 7.0;
  CommandPrepareResult prepared =
      prepareTransformCommand(context, node, before, after);
  if (!expect(prepared.succeeded(), "transform history command prepares")) {
    return false;
  }
  const HistoryResult execute = history.execute(std::move(prepared.command));
  const HistoryResult undo = history.undo();
  const HistoryResult redo = history.redo();
  bool passed = expect(
      execute.status == HistoryStatus::pushed && forwardEvidence(execute.command) &&
          execute.command.changes.exactChanges.size() == 1u &&
          undo.status == HistoryStatus::moved && forwardEvidence(undo.command) &&
          undo.command.changes.exactChanges.size() == 1u &&
          redo.status == HistoryStatus::moved && forwardEvidence(redo.command) &&
          redo.command.changes.exactChanges.size() == 1u &&
          execute.command.afterRevision + 1u == undo.command.afterRevision &&
          undo.command.afterRevision + 1u == redo.command.afterRevision &&
          execute.command.changes.sourceMutation.targetDocumentDigest ==
              undo.command.changes.sourceMutation.baseDocumentDigest &&
          undo.command.changes.sourceMutation.targetDocumentDigest ==
              redo.command.changes.sourceMutation.baseDocumentDigest,
      "Execute, Undo, and Redo transport ordinary forward revision evidence");

  auto propertyDocument = loadDocument("material_extensions.gltf");
  SceneCommands propertyCommands(*propertyDocument);
  EditorSelectionState propertySelection;
  EditorCommandContext propertyContext{*propertyDocument, propertyCommands,
                                       propertySelection};
  ManualClock propertyClock;
  EditorHistory propertyHistory(propertyContext, propertyClock);
  const MaterialId propertyMaterial = *MaterialId::from(
      propertyDocument->idAt(EditorObjectKind::material, 0u));
  const MaterialSnapshot propertyBefore =
      *propertyCommands.materialSnapshot(propertyMaterial);
  MaterialProperties propertyValue =
      *propertyCommands.materialProperties(propertyMaterial);
  propertyValue.metallicFactor =
      propertyValue.metallicFactor == 0.2 ? 0.8 : 0.2;
  const MaterialSnapshot propertyAfter = *propertyCommands.prepareMaterialSnapshot(
      propertyMaterial, MaterialEdit{propertyValue});
  CommandPrepareResult propertyPrepared = prepareEditMaterialCommand(
      propertyContext, propertyBefore, propertyAfter);
  if (!expect(propertyPrepared.succeeded(),
              "property history command prepares")) {
    return false;
  }
  const HistoryResult propertyExecute =
      propertyHistory.execute(std::move(propertyPrepared.command));
  const HistoryResult propertyUndo = propertyHistory.undo();
  const HistoryResult propertyRedo = propertyHistory.redo();
  passed &= expect(
      propertyExecute.status == HistoryStatus::pushed &&
          forwardEvidence(propertyExecute.command) &&
          propertyExecute.command.changes.exactChanges.size() == 1u &&
          propertyExecute.command.changes.exactChanges[0].field ==
              SceneField::MaterialParameters &&
          propertyUndo.status == HistoryStatus::moved &&
          forwardEvidence(propertyUndo.command) &&
          propertyRedo.status == HistoryStatus::moved &&
          forwardEvidence(propertyRedo.command),
      "property Execute, Undo, and Redo preserve exact material evidence");

  auto hierarchyDocument = loadDocument("minimal_scene.gltf");
  SceneCommands hierarchyCommands(*hierarchyDocument);
  EditorSelectionState hierarchySelection;
  EditorCommandContext hierarchyContext{*hierarchyDocument,
                                         hierarchyCommands,
                                         hierarchySelection};
  ManualClock hierarchyClock;
  EditorHistory hierarchyHistory(hierarchyContext, hierarchyClock);
  const SceneId scene = hierarchyDocument->currentSceneId();
  const NodeId parent = *NodeId::from(
      hierarchyDocument->idAt(EditorObjectKind::node, 0u));
  CommandPrepareResult hierarchyPrepared = prepareAddChildNodeCommand(
      hierarchyContext, AddChildNodeRequest{scene, parent, "History child"});
  if (!expect(hierarchyPrepared.succeeded(),
              "hierarchy history command prepares")) {
    return false;
  }
  const HistoryResult hierarchyExecute =
      hierarchyHistory.execute(std::move(hierarchyPrepared.command));
  const HistoryResult hierarchyUndo = hierarchyHistory.undo();
  const HistoryResult hierarchyRedo = hierarchyHistory.redo();
  passed &= expect(
      hierarchyExecute.status == HistoryStatus::pushed &&
          forwardEvidence(hierarchyExecute.command) &&
          hierarchyExecute.command.fullSnapshotReason ==
              FullRebuildReason::StructuralTopology &&
          !hierarchyExecute.command.changes.stableIdRemap.empty() &&
          hierarchyUndo.status == HistoryStatus::moved &&
          forwardEvidence(hierarchyUndo.command) &&
          hierarchyUndo.command.fullSnapshotReason ==
              FullRebuildReason::StructuralTopology &&
          hierarchyRedo.status == HistoryStatus::moved &&
          forwardEvidence(hierarchyRedo.command) &&
          hierarchyRedo.command.fullSnapshotReason ==
              FullRebuildReason::StructuralTopology,
      "hierarchy Execute, Undo, and Redo preserve remap and snapshot reason");
  return passed;
}

#endif

} // namespace

int main() {
  bool passed = true;
  passed &= testExactOneFieldAndAtomicCamera();
  passed &= testIndependentClockEnvelopes();
  passed &= testDerivedEffectsAndStructuralSnapshot();
  passed &= testLosslessTransportValues();
  passed &= testContiguousBatching();
#if defined(MGIF_P5_LIVE_PRODUCER_TESTS)
  passed &= testLiveExactFieldAndCameraCapture();
  passed &= testLiveMaterialVariantAndHierarchyCapture();
  passed &= testLiveIndependentClocksAndFailures();
  passed &= testLiveHistoryForwardEvidence();
#endif
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
