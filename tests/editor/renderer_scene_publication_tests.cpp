#include "../../render/editor/SceneRuntimePublisher.h"

#include <mgif/scene_editor/SceneChangeCanonicalizer.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace demo;
using namespace demo::editor;

void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

SceneDigest digest(std::uint8_t seed) {
  SceneDigest value{};
  for (std::size_t index = 0; index < value.size(); ++index) {
    value[index] = static_cast<std::uint8_t>(seed + index);
  }
  return value;
}

StableSceneId sceneId(EditorDocumentAuthority authority) {
  return {EditorObjectKind::scene, authority, 1u, 1u};
}

StableSceneId nodeId(EditorDocumentAuthority authority) {
  return {EditorObjectKind::node, authority, 2u, 1u};
}

RendererTargetFence baseFence() {
  RendererTargetFence fence;
  fence.backend = rhi::BackendType::vulkan;
  fence.capability = RendererSceneUpdateCapability::IncrementalAndSnapshot;
  fence.renderer = {101u, 7u};
  fence.device = {202u, 9u};
  fence.mappingEpoch = 3u;
  fence.authority = {77u};
  fence.appliedDocumentRevision = 10u;
  fence.sceneActivationEpoch = 4u;
  fence.rootExternalStateSerial = 6u;
  fence.activeScene = sceneId(fence.authority);
  fence.projectionDigest = digest(20u);
  return fence;
}

RendererObjectHandle rendererHandle(const RendererTargetFence &fence,
                                    std::uint32_t index = 11u) {
  return {RendererObjectKind::SceneObject, index, 5u, fence.renderer.generation,
          fence.device.generation};
}

RendererProjectionCoordinate
projectionCoordinate(const RendererTargetFence &fence,
                     std::uint32_t index = 2u) {
  return {EditorObjectKind::node, index, 19u, fence.projectionDigest};
}

RendererSceneMappingSnapshot mappingFor(const RendererTargetFence &fence) {
  RendererSceneMappingEntry entry;
  entry.stableId = nodeId(fence.authority);
  entry.projection = projectionCoordinate(fence);
  entry.rendererHandle = rendererHandle(fence);
  entry.resourceContentDigest = digest(70u);
  entry.resourceSchemaDigest = digest(90u);
  return RendererSceneMappingSnapshot{fence, {entry}};
}

RendererTargetFence targetFence(const RendererTargetFence &base,
                                const SceneChangeHeader &header) {
  RendererTargetFence target = base;
  ++target.mappingEpoch;
  target.authority = header.authority;
  target.appliedDocumentRevision = header.targetDocumentRevision;
  target.sceneActivationEpoch = header.targetSceneActivationEpoch;
  target.rootExternalStateSerial = header.targetRootExternalStateSerial;
  target.activeScene = header.targetActiveScene;
  target.projectionDigest = header.targetProjectionDigest;
  return target;
}

ApplySceneChangeRequest
makeIncrementalRequest(const RendererTargetFence &base) {
  SceneChangeSet input;
  SceneChangeHeader &header = input.header;
  header.domain = SceneChangeDomain::DocumentMutation;
  header.authority = base.authority;
  header.baseDocumentRevision = base.appliedDocumentRevision;
  header.targetDocumentRevision = base.appliedDocumentRevision + 1u;
  header.baseSceneActivationEpoch = base.sceneActivationEpoch;
  header.targetSceneActivationEpoch = base.sceneActivationEpoch;
  header.baseRootExternalStateSerial = base.rootExternalStateSerial;
  header.targetRootExternalStateSerial = base.rootExternalStateSerial;
  header.baseActiveScene = base.activeScene;
  header.targetActiveScene = base.activeScene;
  header.baseDocumentDigest = digest(1u);
  header.targetDocumentDigest = digest(2u);
  header.baseProjectionDigest = base.projectionDigest;
  header.targetProjectionDigest = digest(21u);
  header.firstMutationSerial = 44u;
  header.lastMutationSerial = 44u;
  header.mutationToken =
      SceneMutationToken{base.authority, header.targetDocumentRevision, 44u};
  header.sourceMutations.push_back(SourceMutationFence{
      header.baseDocumentRevision, header.targetDocumentRevision,
      *header.mutationToken, header.baseDocumentDigest,
      header.targetDocumentDigest});

  SceneNodeTransformValue before;
  SceneNodeTransformValue after;
  std::get<SceneTrsValue>(after.value).translation[0] = 3.0;
  input.canonicalChanges.push_back(
      {nodeId(base.authority), SceneChangeClass::Transform,
       SceneChangeAction::Update, SceneField::NodeLocalTransform, 44u,
       SceneFieldValue{before}, SceneFieldValue{after}});

  SceneCanonicalizationResult canonical =
      SceneChangeCanonicalizer::canonicalize(std::move(input));
  require(canonical.succeeded(), "incremental fixture did not canonicalize");

  ApplySceneChangeRequest request;
  request.expectedTarget = base;
  request.changes = std::move(*canonical.changeSet);
  request.canonicalBytes = std::move(canonical.canonicalBytes);
  request.canonicalIdentity = {
      request.changes.header.authority,
      request.changes.header.domain,
      request.changes.header.firstMutationSerial,
      request.changes.header.lastMutationSerial,
      request.changes.header.targetDocumentRevision,
      request.changes.header.targetSceneActivationEpoch,
      request.changes.header.targetRootExternalStateSerial};
  const RendererTargetFence target = targetFence(base, request.changes.header);
  request.targetMapping = mappingFor(target);
  request.stagePlan.submissions = {{rhi::QueueClass::transfer, 1u},
                                   {rhi::QueueClass::graphics, 2u}};
  request.stagePlan.retireResourceDiagnosticIds = {{500u}};
  return request;
}

ApplySceneChangeRequest
makeFullSnapshotRequest(const RendererTargetFence &base) {
  SceneChangeSet input;
  SceneChangeHeader &header = input.header;
  header.domain = SceneChangeDomain::FullSnapshot;
  header.authority = base.authority;
  header.baseDocumentRevision = base.appliedDocumentRevision;
  header.targetDocumentRevision = base.appliedDocumentRevision + 3u;
  header.baseSceneActivationEpoch = base.sceneActivationEpoch;
  header.targetSceneActivationEpoch = base.sceneActivationEpoch;
  header.baseRootExternalStateSerial = base.rootExternalStateSerial;
  header.targetRootExternalStateSerial = base.rootExternalStateSerial;
  header.baseActiveScene = base.activeScene;
  header.targetActiveScene = base.activeScene;
  header.baseDocumentDigest = digest(1u);
  header.targetDocumentDigest = digest(8u);
  header.baseProjectionDigest = base.projectionDigest;
  header.targetProjectionDigest = digest(30u);
  header.firstMutationSerial = 0u;
  header.lastMutationSerial = 0u;

  FullSnapshotReference full;
  full.reason = FullRebuildReason::StructuralTopology;
  full.immutableTarget.authority = base.authority;
  full.immutableTarget.documentRevision = header.targetDocumentRevision;
  full.immutableTarget.sceneActivationEpoch = header.targetSceneActivationEpoch;
  full.immutableTarget.rootExternalStateSerial =
      header.targetRootExternalStateSerial;
  full.immutableTarget.activeScene = header.targetActiveScene;
  full.immutableTarget.documentDigest = header.targetDocumentDigest;
  full.immutableTarget.projectionDigest = header.targetProjectionDigest;
  full.immutableTarget.bytes = {3u, 1u, 4u, 1u, 5u};
  full.immutableTarget.contentDigest =
      SceneChangeCanonicalizer::computeSha256(full.immutableTarget.bytes);
  full.targetSnapshotDigest = full.immutableTarget.contentDigest;
  full.stableIdRemapComplete = true;
  input.fullSnapshot = std::move(full);

  SceneCanonicalizationResult canonical =
      SceneChangeCanonicalizer::canonicalize(std::move(input));
  require(canonical.succeeded(), "snapshot fixture did not canonicalize");

  ApplySceneChangeRequest request;
  request.expectedTarget = base;
  request.changes = std::move(*canonical.changeSet);
  request.canonicalBytes = std::move(canonical.canonicalBytes);
  request.canonicalIdentity = {
      request.changes.header.authority,
      request.changes.header.domain,
      0u,
      0u,
      request.changes.header.targetDocumentRevision,
      request.changes.header.targetSceneActivationEpoch,
      request.changes.header.targetRootExternalStateSerial};
  request.targetMapping = mappingFor(targetFence(base, request.changes.header));
  request.stagePlan.submissions = {{rhi::QueueClass::graphics, 3u}};
  request.stagePlan.retireResourceDiagnosticIds = {{500u}};
  request.stagePlan.uploads.otherUploadCalls = 1u;
  request.stagePlan.uploads.otherBytesUploaded = 128u;
  request.stagePlan.uploads.otherRangesUploaded = 1u;
  return request;
}

class RecordingBackend final : public SceneTransactionBackend {
public:
  enum class TokenMode : std::uint8_t {
    Monotonic = 0,
    InvalidFirst,
    NonMonotonicSecond,
  };

  TokenMode tokenMode{TokenMode::Monotonic};
  bool failNextRetirementEnqueue{false};
  std::uint64_t calls{0};
  std::uint64_t destroyed{0};
  std::uint64_t enqueued{0};
  std::uint64_t reservations{0};
  std::uint64_t committedReservations{0};
  std::uint64_t recoveryCalls{0};
  std::vector<rhi::SubmissionToken> reservedDependencies;
  std::vector<rhi::SubmissionToken> actualSubmissionTokens;
  std::vector<rhi::SubmissionToken> enqueuedDependencies;
  std::vector<rhi::SubmissionToken> recoveryOverrides;
  std::size_t nextRecoveryOverride{0};

  rhi::RHIResult
  stageCpuGeneration(const SceneChangeSet &,
                     const RendererSceneMappingSnapshot &) noexcept override {
    ++calls;
    return rhi::RHIResult::ok();
  }

  rhi::RHIResult allocateSceneResources(
      const SceneChangeSet &,
      std::vector<SceneJournalResource> &createdResources) noexcept override {
    ++calls;
    createdResources.push_back({{900u}, {rhi::ResourceKind::Buffer, 31u, 2u}});
    return rhi::RHIResult::ok();
  }

  rhi::RHIResult stageSceneDescriptors(
      const SceneChangeSet &,
      std::span<const SceneJournalResource>) noexcept override {
    ++calls;
    return rhi::RHIResult::ok();
  }

  rhi::RHIResult recordSceneGeneration(
      const SceneChangeSet &,
      std::span<const SceneJournalResource>) noexcept override {
    ++calls;
    return rhi::RHIResult::ok();
  }

  rhi::Result<rhi::SubmissionToken>
  submitSceneGeneration(const SceneQueueSubmissionPlan &submission,
                        std::uint32_t submissionIndex) noexcept override {
    ++calls;
    require(submission.valid(), "invalid submission plan reached backend");
    rhi::SubmissionToken actual{
        {submissionIndex + 1u, 1u}, submissionIndex + 10u};
    rhi::SubmissionToken reported = actual;
    if (tokenMode == TokenMode::InvalidFirst && submissionIndex == 0u) {
      reported = {};
    } else if (tokenMode == TokenMode::NonMonotonicSecond) {
      actual = {{1u, 1u}, submissionIndex + 10u};
      reported = submissionIndex == 0u
                     ? actual
                     : rhi::SubmissionToken{{1u, 1u}, 10u};
    }
    actualSubmissionTokens.push_back(actual);
    return rhi::Result<rhi::SubmissionToken>::ok(reported);
  }

  rhi::RHIResult validateBeforePublication(
      const RendererSceneMappingSnapshot &) noexcept override {
    ++calls;
    return rhi::RHIResult::ok();
  }

  rhi::Result<SceneRetirementReservation> reserveSceneRetirement(
      std::span<const SceneJournalResource> resources,
      std::span<const rhi::SubmissionToken> dependencies) noexcept override {
    ++calls;
    ++reservations;
    require(!resources.empty(), "reservation lost the active resources");
    reservedDependencies.assign(dependencies.begin(), dependencies.end());
    return rhi::Result<SceneRetirementReservation>::ok({reservations});
  }

  void commitSceneRetirement(
      SceneRetirementReservation reservation) noexcept override {
    ++calls;
    require(reservation.valid(), "invalid retirement reservation committed");
    ++committedReservations;
  }

  void cancelSceneRetirement(SceneRetirementReservation) noexcept override {
    ++calls;
  }

  rhi::RHIResult destroyUnpublishedResources(
      std::span<const SceneJournalResource> resources) noexcept override {
    ++calls;
    destroyed += resources.size();
    return rhi::RHIResult::ok();
  }

  rhi::RHIResult enqueueResourceRetirement(
      std::span<const SceneJournalResource> resources,
      std::span<const rhi::SubmissionToken> dependencies) noexcept override {
    ++calls;
    enqueuedDependencies.assign(dependencies.begin(), dependencies.end());
    if (failNextRetirementEnqueue) {
      failNextRetirementEnqueue = false;
      return rhi::RHIResult::fail(rhi::RHIErrorCode::outOfMemory,
                                  "injected orphan enqueue failure");
    }
    enqueued += resources.size();
    return rhi::RHIResult::ok();
  }

  rhi::Result<rhi::SubmissionToken> recoverSubmissionDependency(
      std::span<const SceneJournalResource>, std::uint32_t submissionIndex,
      rhi::SubmissionToken) noexcept override {
    ++calls;
    ++recoveryCalls;
    if (nextRecoveryOverride < recoveryOverrides.size()) {
      return rhi::Result<rhi::SubmissionToken>::ok(
          recoveryOverrides[nextRecoveryOverride++]);
    }
    if (submissionIndex >= actualSubmissionTokens.size()) {
      return rhi::Result<rhi::SubmissionToken>::fail(
          rhi::RHIErrorCode::invalidState,
          "fake backend lost the actual submitted token");
    }
    return rhi::Result<rhi::SubmissionToken>::ok(
        actualSubmissionTokens[submissionIndex]);
  }
};

RendererScenePublisher makePublisher(RecordingBackend &backend,
                                     SceneDeltaCounters &counters) {
  const RendererTargetFence fence = baseFence();
  RendererSceneInitialState initial;
  initial.fence = fence;
  initial.mapping = mappingFor(fence);
  initial.activeGenerationDigest = digest(40u);
  initial.resources.push_back({{500u}, {rhi::ResourceKind::Buffer, 12u, 3u}});
  initial.lastUseSubmissionTokens.push_back({{9u, 1u}, 44u});
  return RendererScenePublisher{std::move(initial), backend, counters};
}

void testMappingSnapshotIsTypedImmutableAndBidirectional() {
  const RendererTargetFence fence = baseFence();
  const RendererSceneMappingSnapshot mapping = mappingFor(fence);
  require(mapping.validate().valid(), "valid mapping rejected");
  const auto *entry = mapping.find(nodeId(fence.authority));
  require(entry != nullptr, "stable lookup failed");
  require(mapping.reverseLookup(entry->rendererHandle) == entry->stableId,
          "renderer reverse lookup failed");
  require(mapping.reverseLookup(entry->projection) == entry->stableId,
          "projection reverse lookup failed");

  RendererSceneMappingEntry duplicate = *entry;
  duplicate.stableId.slot = 3u;
  const RendererSceneMappingSnapshot duplicateHandle{fence,
                                                     {*entry, duplicate}};
  require(duplicateHandle.validate().error ==
              RendererSceneMappingError::DuplicateRendererHandle,
          "duplicate renderer handle was not typed");
}

void assertResultFieldRules(const SceneApplyResult &result,
                            SceneApplyDisposition disposition) {
  require(result.disposition == disposition, "unexpected disposition");
  const bool shouldHaveFence =
      disposition == SceneApplyDisposition::AcceptedIncremental ||
      disposition == SceneApplyDisposition::AcceptedFullSnapshot ||
      disposition == SceneApplyDisposition::AlreadyApplied;
  require(result.resultingFence.has_value() == shouldHaveFence,
          "resulting-fence presence rule violated");
  require(result.handles.createdCount == result.handles.created.size(),
          "created handle count does not match diagnostics");
  require(result.handles.logicallyRetiredCount ==
              result.handles.logicallyRetired.size(),
          "retired handle count does not match diagnostics");
  require(result.handles.eventuallyDestroyedCount ==
              result.handles.eventuallyDestroyed.size(),
          "eventual handle count does not match diagnostics");
}

void testAcceptedIncrementalAndIdentityReplay() {
  RecordingBackend backend;
  SceneDeltaCounters counters;
  auto publisher = makePublisher(backend, counters);
  ApplySceneChangeRequest request = makeIncrementalRequest(baseFence());

  const auto oldGeneration = publisher.activeGeneration();
  const SceneApplyResult accepted = publisher.applySceneChanges(request);
  assertResultFieldRules(accepted, SceneApplyDisposition::AcceptedIncremental);
  require(accepted.error == SceneApplyError::None,
          "accepted incremental has an error");
  require(accepted.message.empty(), "accepted incremental has a message");
  require(accepted.rhiResult.succeeded(),
          "accepted incremental lost neutral RHI success");
  require(accepted.oldFence == baseFence(), "old fence is not exact");
  require(*accepted.resultingFence == request.targetMapping.fence(),
          "resulting fence is not exact");
  require(accepted.canonicalIdentity == request.canonicalIdentity,
          "canonical identity is not exact");
  require(accepted.canonicalDigest ==
              request.changes.header.canonicalChangeSetDigest,
          "canonical digest is not exact");
  require(accepted.queueSubmissionTokens.size() == 2u,
          "queue tokens are incomplete");
  require(accepted.handles.createdCount == 1u &&
              accepted.handles.logicallyRetiredCount == 1u &&
              accepted.handles.eventuallyDestroyedCount == 1u,
          "handle diagnostics are incomplete");
  require(accepted.counterDelta.sceneChangeSetsReceived == 1u &&
              accepted.counterDelta.sceneChangeSetsAcceptedIncremental == 1u,
          "accepted counters are incorrect");
  require(accepted.counterDelta ==
              sceneDeltaCounterDelta(accepted.countersBefore,
                                     accepted.countersAfter),
          "accepted counter snapshots disagree");
  require(accepted.counterDelta.sceneOperationsApplied == 1u &&
              accepted.counterDelta.nodeTransformOpsApplied == 1u,
          "logical work counters are incorrect");
  require(accepted.counterDelta.geometryUploadCalls == 0u &&
              accepted.counterDelta.geometryBytesUploaded == 0u &&
              accepted.counterDelta.textureUploadCalls == 0u &&
              accepted.counterDelta.textureBytesUploaded == 0u &&
              accepted.counterDelta.deviceIdleCalls == 0u &&
              accepted.counterDelta.partialPublicationCount == 0u,
          "continuous edit violated zero-work sentinels");
  require(oldGeneration->fence() == baseFence(),
          "old immutable generation was mutated");
  require(publisher.activeGeneration()->fence() ==
              request.targetMapping.fence(),
          "candidate generation was not published atomically");
  require(backend.committedReservations == 1u,
          "superseded generation retirement was not committed");
  require(backend.reservedDependencies.size() == 1u &&
              backend.reservedDependencies[0] ==
                  rhi::SubmissionToken{{9u, 1u}, 44u},
          "retirement did not use the superseded generation's exact token");
  require(publisher.activeGeneration()->lastUseSubmissionTokens().size() ==
              accepted.queueSubmissionTokens.size(),
          "new generation did not retain its exact submission tokens");

  const std::uint64_t callsBeforeReplay = backend.calls;
  const auto activeBeforeReplay = publisher.activeGeneration();
  const SceneDeltaCounterSnapshot countersBeforeReplay = publisher.counters();
  const SceneApplyResult replay = publisher.applySceneChanges(request);
  assertResultFieldRules(replay, SceneApplyDisposition::AlreadyApplied);
  require(replay.error == SceneApplyError::None && replay.rhiResult.succeeded(),
          "AlreadyApplied is not neutral success");
  require(replay.oldFence == activeBeforeReplay->fence() &&
              replay.resultingFence == replay.oldFence,
          "AlreadyApplied fences are not equal to current");
  require(replay.canonicalDigest ==
              request.changes.header.canonicalChangeSetDigest,
          "AlreadyApplied digest changed");
  require(replay.canonicalIdentity == request.canonicalIdentity,
          "AlreadyApplied canonical identity changed");
  require(replay.queueSubmissionTokens.empty() &&
              replay.handles.created.empty() &&
              replay.handles.logicallyRetired.empty() &&
              replay.handles.eventuallyDestroyed.empty(),
          "AlreadyApplied returned work diagnostics");
  require(replay.countersBefore == countersBeforeReplay &&
              replay.countersAfter == countersBeforeReplay &&
              sceneDeltaCounterSnapshotIsZero(replay.counterDelta),
          "AlreadyApplied changed counters");
  require(!replay.requestedFullSnapshotReason &&
              replay.placeholderResourceCount == 0u,
          "AlreadyApplied returned fallback or placeholders");
  require(backend.calls == callsBeforeReplay &&
              publisher.activeGeneration() == activeBeforeReplay,
          "AlreadyApplied changed RHI or active generation");

  ApplySceneChangeRequest conflicting = request;
  conflicting.canonicalBytes.back() ^= 0x5au;
  const SceneApplyResult conflict = publisher.applySceneChanges(conflicting);
  assertResultFieldRules(conflict, SceneApplyDisposition::Rejected);
  require(conflict.error == SceneApplyError::InvalidState,
          "identity reuse was not typed InvalidState");
  require(conflict.countersBefore == countersBeforeReplay &&
              conflict.countersAfter == countersBeforeReplay &&
              sceneDeltaCounterSnapshotIsZero(conflict.counterDelta),
          "identity reuse changed counters before rejection");
  require(backend.calls == callsBeforeReplay &&
              publisher.activeGeneration() == activeBeforeReplay,
          "identity reuse had a backend or publication side effect");
}

void testAcceptedFullSnapshot() {
  RecordingBackend backend;
  SceneDeltaCounters counters;
  auto publisher = makePublisher(backend, counters);
  const ApplySceneChangeRequest request = makeFullSnapshotRequest(baseFence());
  const SceneApplyResult result = publisher.applyFullSceneSnapshot(request);
  assertResultFieldRules(result, SceneApplyDisposition::AcceptedFullSnapshot);
  require(result.error == SceneApplyError::None, "full snapshot has an error");
  require(
      result.counterDelta.sceneChangeSetsAcceptedFullSnapshot == 1u &&
          result.counterDelta.fullSnapshotCount == 1u &&
          result.counterDelta.fullSnapshotCountByReason[fullRebuildReasonIndex(
              FullRebuildReason::StructuralTopology)] == 1u,
      "full snapshot counters are incomplete");
  require(result.counterDelta.otherUploadCalls == 1u &&
              result.counterDelta.otherBytesUploaded == 128u &&
              result.counterDelta.fullSnapshotSubmissions == 1u,
          "full snapshot work counters are incorrect");
  require(result.counterDelta.partialPublicationCount == 0u &&
              result.counterDelta.deviceIdleCalls == 0u,
          "full snapshot used partial publication or idle");
  require(publisher.activeGeneration()->activeGenerationDigest() ==
                  request.changes.fullSnapshot->targetSnapshotDigest &&
              publisher.counters().current.activeGenerationDigest ==
                  request.changes.fullSnapshot->targetSnapshotDigest,
          "full snapshot did not publish its independently validated target "
          "digest");
}

void testIncompleteFullSnapshotRemapRejectsInPreflight() {
  RecordingBackend backend;
  SceneDeltaCounters counters;
  auto publisher = makePublisher(backend, counters);
  ApplySceneChangeRequest request = makeFullSnapshotRequest(baseFence());
  request.changes.fullSnapshot->stableIdRemapComplete = false;

  const std::uint64_t callsBefore = backend.calls;
  const SceneDeltaCounterSnapshot countersBefore = publisher.counters();
  const auto activeBefore = publisher.activeGeneration();
  const std::optional<SceneApplyResult> rejection =
      publisher.preflightFullSceneSnapshot(request);

  require(rejection.has_value() &&
              rejection->disposition == SceneApplyDisposition::Rejected &&
              rejection->error == SceneApplyError::InvalidPayload &&
              rejection->message.find("stable-ID remap completeness") !=
                  std::string::npos,
          "incomplete full-snapshot remap was not rejected fail-closed");
  require(backend.calls == callsBefore && publisher.counters() == countersBefore &&
              rejection->countersBefore == countersBefore &&
              rejection->countersAfter == countersBefore &&
              sceneDeltaCounterSnapshotIsZero(rejection->counterDelta) &&
              publisher.activeGeneration() == activeBefore,
          "incomplete full-snapshot remap rejection changed state");
}

void testResourceRetentionCoalescingAndPlaceholderPolicy() {
  RecordingBackend backend;
  SceneDeltaCounters counters;
  auto publisher = makePublisher(backend, counters);
  ApplySceneChangeRequest request = makeIncrementalRequest(baseFence());
  request.stagePlan.retireResourceDiagnosticIds.clear();
  request.stagePlan.coalescedOperationCount = 2u;
  request.stagePlan.coalescedOperationsByClass[sceneChangeClassIndex(
      SceneChangeClass::Transform)] = 2u;
  request.stagePlan.placeholderResourceCount = 1u;

  const SceneApplyResult rejected = publisher.applySceneChanges(request);
  require(rejected.disposition == SceneApplyDisposition::Rejected &&
              rejected.error == SceneApplyError::InvalidPayload &&
              rejected.counterDelta.sceneChangeSetsRejected == 1u,
          "undeclared placeholder use was not rejected");
  require(backend.calls == 0u && !rejected.resultingFence,
          "placeholder validation reached the backend or returned a fence");

  request.stagePlan.placeholderPolicy = ScenePlaceholderPolicy::AllowDeclared;
  const SceneApplyResult accepted = publisher.applySceneChanges(request);
  require(accepted.disposition == SceneApplyDisposition::AcceptedIncremental &&
              accepted.placeholderResourceCount == 1u &&
              accepted.counterDelta.placeholderResourceCount == 1u,
          "declared placeholder use was not explicit in result/counters");
  require(accepted.handles.logicallyRetiredCount == 0u &&
              backend.reservations == 0u,
          "retained active resource was retired");
  const auto active = publisher.activeGeneration();
  require(active->resources().size() == 2u &&
              active->resources()[0].diagnosticId.value == 500u &&
              active->resources()[1].diagnosticId.value == 900u,
          "candidate generation did not retain and extend resources");
  require(accepted.counterDelta.sceneOperationsReceived == 3u &&
              accepted.counterDelta.sceneOperationsCoalesced == 2u &&
              accepted.counterDelta.sceneOperationsApplied == 1u &&
              accepted.counterDelta
                      .sceneOperationsReceivedByClass[sceneChangeClassIndex(
                          SceneChangeClass::Transform)] == 3u &&
              accepted.counterDelta
                      .sceneOperationsCoalescedByClass[sceneChangeClassIndex(
                          SceneChangeClass::Transform)] == 2u &&
              accepted.counterDelta
                      .sceneOperationsAppliedByClass[sceneChangeClassIndex(
                          SceneChangeClass::Transform)] == 1u,
          "coalesced operation counters are not exact");
}

void testPublicTaxonomiesAreExhaustive() {
  const std::vector<SceneApplyDisposition> dispositions{
      SceneApplyDisposition::AcceptedIncremental,
      SceneApplyDisposition::AcceptedFullSnapshot,
      SceneApplyDisposition::AlreadyApplied,
      SceneApplyDisposition::Rejected,
      SceneApplyDisposition::FailedBeforeSubmit,
      SceneApplyDisposition::FailedAfterSubmit};
  for (const SceneApplyDisposition value : dispositions) {
    require(std::string{sceneApplyDispositionName(value)} != "Unknown",
            "disposition has no stable name");
  }
  require(dispositions.size() == 6u, "disposition taxonomy changed");

  for (std::size_t index = 0; index < kSceneApplyErrorCount; ++index) {
    const auto error = static_cast<SceneApplyError>(index);
    require(std::string{sceneApplyErrorName(error)} != "Unknown",
            "error has no stable name");
    require(sceneApplyErrorIndex(error) == index,
            "error bucket is not one-to-one");
  }
  require(kSceneApplyErrorCount == 23u, "error taxonomy changed");
}

void testResourceJournalCleanupPartitions() {
  RecordingBackend backend;
  const SceneJournalResource first{{1001u},
                                   {rhi::ResourceKind::Buffer, 101u, 1u}};
  const SceneJournalResource second{{1002u},
                                    {rhi::ResourceKind::Texture, 102u, 1u}};

  SceneDeltaResourceJournal immediate;
  require(immediate.stage(first), "journal did not stage a valid resource");
  const std::array duplicateBatch{second, first};
  require(!immediate.stage(duplicateBatch) &&
              immediate.resources().size() == 1u,
          "journal batch validation partially staged a duplicate");
  const SceneJournalCleanupResult immediateCleanup =
      immediate.rollbackBeforeSubmit(backend);
  require(immediateCleanup.succeeded() &&
              immediateCleanup.physicallyDestroyed ==
                  std::vector<SceneResourceDiagnosticId>{{1001u}} &&
              immediate.state() ==
                  SceneResourceJournalState::ImmediateCleanupComplete,
          "pre-submit journal cleanup was not immediate");

  SceneDeltaResourceJournal deferred;
  require(deferred.stage(second), "journal did not stage a deferred resource");
  const rhi::SubmissionToken token{{3u, 2u}, 91u};
  require(deferred.captureSubmission(token),
          "journal did not capture a valid submission token");
  const SceneJournalCleanupResult deferredCleanup =
      deferred.rollbackAfterSubmit(backend);
  require(deferredCleanup.succeeded() &&
              deferredCleanup.logicallyRetired ==
                  std::vector<SceneResourceDiagnosticId>{{1002u}} &&
              deferredCleanup.physicallyDestroyed.empty() &&
              deferred.state() ==
                  SceneResourceJournalState::DeferredCleanupComplete,
          "post-submit journal cleanup was not deferred");
}

void testSuccessfulRejectedTokenIsRetainedAndRetired() {
  {
    RecordingBackend backend;
    backend.tokenMode = RecordingBackend::TokenMode::InvalidFirst;
    backend.failNextRetirementEnqueue = true;
    SceneDeltaCounters counters;
    auto publisher = makePublisher(backend, counters);
    const auto active = publisher.activeGeneration();
    const SceneApplyResult failed =
        publisher.applySceneChanges(makeIncrementalRequest(baseFence()));

    require(failed.disposition == SceneApplyDisposition::FailedAfterSubmit &&
                failed.error == SceneApplyError::RetirementEnqueueFailure,
            "invalid successful token did not take the retained-orphan path");
    require(failed.queueSubmissionTokens.size() == 1u &&
                !failed.queueSubmissionTokens.front().isValid() &&
                failed.message.find("queue=0:0, value=0") !=
                    std::string::npos,
            "invalid successful token was lost from result diagnostics");
    require(publisher.activeGeneration() == active &&
                publisher.hasPendingOrphanRetirement() &&
                backend.destroyed == 0u && backend.enqueued == 0u &&
                backend.recoveryCalls == 1u,
            "invalid successful token leaked or destroyed submitted resources");

    const rhi::RHIResult retry = publisher.retryPendingOrphanRetirement();
    require(retry.succeeded() &&
                !publisher.hasPendingOrphanRetirement() &&
                backend.enqueued == 1u &&
                backend.enqueuedDependencies ==
                    std::vector<rhi::SubmissionToken>{
                        backend.actualSubmissionTokens.front()},
            "retained invalid-token orphan was not eventually fence-retired");

    backend.tokenMode = RecordingBackend::TokenMode::Monotonic;
    backend.actualSubmissionTokens.clear();
    const SceneApplyResult accepted =
        publisher.applySceneChanges(makeIncrementalRequest(baseFence()));
    require(accepted.disposition ==
                SceneApplyDisposition::AcceptedIncremental &&
                accepted.queueSubmissionTokens.size() == 2u &&
                accepted.resultingFence ==
                    makeIncrementalRequest(baseFence()).targetMapping.fence(),
            "normal monotonic publication did not recover after orphan retry");
  }

  {
    RecordingBackend backend;
    backend.tokenMode = RecordingBackend::TokenMode::NonMonotonicSecond;
    backend.recoveryOverrides = {
        {{1u, 1u}, 10u}, {{1u, 1u}, 9u}, {{1u, 1u}, 11u}};
    SceneDeltaCounters counters;
    auto publisher = makePublisher(backend, counters);
    const auto active = publisher.activeGeneration();
    const SceneApplyResult failed =
        publisher.applySceneChanges(makeIncrementalRequest(baseFence()));
    require(failed.disposition == SceneApplyDisposition::FailedAfterSubmit &&
                failed.error == SceneApplyError::RetirementEnqueueFailure &&
                failed.queueSubmissionTokens ==
                    std::vector<rhi::SubmissionToken>{
                        {{1u, 1u}, 10u}, {{1u, 1u}, 10u}},
            "non-monotonic successful token was not preserved verbatim");
    require(publisher.activeGeneration() == active &&
                publisher.hasPendingOrphanRetirement() &&
                backend.recoveryCalls == 1u && backend.enqueued == 0u &&
                backend.destroyed == 0u,
            "equal recovered token did not retain the submitted orphan");

    const rhi::RHIResult lowerRetry =
        publisher.retryPendingOrphanRetirement();
    require(!lowerRetry.succeeded() &&
                publisher.activeGeneration() == active &&
                publisher.hasPendingOrphanRetirement() &&
                backend.recoveryCalls == 2u && backend.enqueued == 0u &&
                backend.destroyed == 0u,
            "lower recovered token was accepted as fence coverage");

    const rhi::RHIResult advancingRetry =
        publisher.retryPendingOrphanRetirement();
    require(advancingRetry.succeeded() &&
                publisher.activeGeneration() == active &&
                !publisher.hasPendingOrphanRetirement() &&
                backend.recoveryCalls == 3u && backend.enqueued == 1u &&
                backend.enqueuedDependencies ==
                    std::vector<rhi::SubmissionToken>{{{1u, 1u}, 11u}},
            "non-monotonic orphan did not retire behind its advancing token");
  }
}

} // namespace

int main() {
  try {
    testMappingSnapshotIsTypedImmutableAndBidirectional();
    testAcceptedIncrementalAndIdentityReplay();
    testAcceptedFullSnapshot();
    testIncompleteFullSnapshotRemapRejectsInPreflight();
    testResourceRetentionCoalescingAndPlaceholderPolicy();
    testPublicTaxonomiesAreExhaustive();
    testResourceJournalCleanupPartitions();
    testSuccessfulRejectedTokenIsRetainedAndRetired();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
