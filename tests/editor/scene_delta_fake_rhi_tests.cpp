#include "../../render/editor/SceneRuntimePublisher.h"

#include <mgif/scene_editor/SceneChangeCanonicalizer.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
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
  return {EditorObjectKind::node, authority, 2u, 3u};
}

RendererTargetFence
makeFence(std::uint64_t mappingEpoch = 8u,
          RendererSceneUpdateCapability capability =
              RendererSceneUpdateCapability::IncrementalAndSnapshot) {
  RendererTargetFence fence;
  fence.backend = rhi::BackendType::vulkan;
  fence.capability = capability;
  fence.renderer = {300u, 4u};
  fence.device = {400u, 5u};
  fence.mappingEpoch = mappingEpoch;
  fence.authority = {55u};
  fence.appliedDocumentRevision = 12u;
  fence.sceneActivationEpoch = 7u;
  fence.rootExternalStateSerial = 9u;
  fence.activeScene = sceneId(fence.authority);
  fence.projectionDigest = digest(15u);
  return fence;
}

RendererSceneMappingEntry mappingEntry(const RendererTargetFence &fence) {
  RendererSceneMappingEntry entry;
  entry.stableId = nodeId(fence.authority);
  entry.projection = {EditorObjectKind::node, 5u, 6u, fence.projectionDigest};
  entry.rendererHandle = {RendererObjectKind::SceneObject, 17u, 2u,
                          fence.renderer.generation, fence.device.generation};
  entry.resourceContentDigest = digest(80u);
  entry.resourceSchemaDigest = digest(100u);
  return entry;
}

RendererSceneMappingSnapshot mappingFor(const RendererTargetFence &fence) {
  return RendererSceneMappingSnapshot{fence, {mappingEntry(fence)}};
}

RendererTargetFence resultingFence(const RendererTargetFence &base,
                                   const SceneChangeHeader &header) {
  RendererTargetFence result = base;
  ++result.mappingEpoch;
  result.authority = header.authority;
  result.appliedDocumentRevision = header.targetDocumentRevision;
  result.sceneActivationEpoch = header.targetSceneActivationEpoch;
  result.rootExternalStateSerial = header.targetRootExternalStateSerial;
  result.activeScene = header.targetActiveScene;
  result.projectionDigest = header.targetProjectionDigest;
  return result;
}

ApplySceneChangeRequest makeRequest(const RendererTargetFence &base) {
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
  header.targetProjectionDigest = digest(16u);
  header.firstMutationSerial = 71u;
  header.lastMutationSerial = 71u;
  header.mutationToken =
      SceneMutationToken{base.authority, header.targetDocumentRevision, 71u};
  header.sourceMutations.push_back(SourceMutationFence{
      header.baseDocumentRevision, header.targetDocumentRevision,
      *header.mutationToken, header.baseDocumentDigest,
      header.targetDocumentDigest});

  SceneVisibilityValue before{true};
  SceneVisibilityValue after{false};
  input.canonicalChanges.push_back(
      {nodeId(base.authority), SceneChangeClass::Visibility,
       SceneChangeAction::Update, SceneField::NodeLocalVisibility, 71u,
       SceneFieldValue{before}, SceneFieldValue{after}});

  SceneCanonicalizationResult canonical =
      SceneChangeCanonicalizer::canonicalize(std::move(input));
  require(canonical.succeeded(), "fake-RHI fixture did not canonicalize");

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
  request.targetMapping =
      mappingFor(resultingFence(base, request.changes.header));
  request.stagePlan.submissions = {{rhi::QueueClass::transfer, 10u},
                                   {rhi::QueueClass::graphics, 11u}};
  request.stagePlan.retireResourceDiagnosticIds = {{501u}};
  return request;
}

enum class FailPoint : std::uint8_t {
  None = 0,
  CpuAllocation,
  RhiAllocation,
  Descriptor,
  Recording,
  DeviceLostRecording,
  FirstSubmit,
  LaterSubmit,
  Prepublication,
  RetirementReservation,
  PrepublicationAndOrphanEnqueue,
  SuccessfulInvalidTokenAndOrphanEnqueue,
  SuccessfulNonMonotonicToken,
};

class FakeRhi final : public SceneTransactionBackend {
public:
  FailPoint failPoint{FailPoint::None};
  std::uint64_t calls{0};
  std::uint64_t cpuCalls{0};
  std::uint64_t allocationCalls{0};
  std::uint64_t descriptorCalls{0};
  std::uint64_t recordingCalls{0};
  std::uint64_t submitCalls{0};
  std::uint64_t prepublicationCalls{0};
  std::uint64_t reservationCalls{0};
  std::uint64_t reservationCommits{0};
  std::uint64_t reservationCancels{0};
  std::uint64_t destroyCalls{0};
  std::uint64_t destroyedResources{0};
  std::uint64_t retirementEnqueueCalls{0};
  std::uint64_t enqueuedResources{0};
  std::uint64_t recoveryCalls{0};
  std::vector<rhi::SubmissionToken> submittedTokens;
  std::vector<rhi::SubmissionToken> actualSubmissionTokens;
  std::vector<rhi::SubmissionToken> enqueuedDependencies;
  std::vector<rhi::SubmissionToken> recoveryOverrides;
  std::size_t nextRecoveryOverride{0};

  rhi::RHIResult
  stageCpuGeneration(const SceneChangeSet &,
                     const RendererSceneMappingSnapshot &) noexcept override {
    ++calls;
    ++cpuCalls;
    return failPoint == FailPoint::CpuAllocation
               ? rhi::RHIResult::fail(rhi::RHIErrorCode::outOfMemory,
                                      "injected CPU allocation failure")
               : rhi::RHIResult::ok();
  }

  rhi::RHIResult allocateSceneResources(
      const SceneChangeSet &,
      std::vector<SceneJournalResource> &createdResources) noexcept override {
    ++calls;
    ++allocationCalls;
    createdResources.push_back({{901u}, {rhi::ResourceKind::Buffer, 91u, 6u}});
    return failPoint == FailPoint::RhiAllocation
               ? rhi::RHIResult::fail(rhi::RHIErrorCode::outOfMemory,
                                      "injected RHI allocation failure")
               : rhi::RHIResult::ok();
  }

  rhi::RHIResult stageSceneDescriptors(
      const SceneChangeSet &,
      std::span<const SceneJournalResource>) noexcept override {
    ++calls;
    ++descriptorCalls;
    return failPoint == FailPoint::Descriptor
               ? rhi::RHIResult::fail(rhi::RHIErrorCode::backendFailure,
                                      "injected descriptor staging failure")
               : rhi::RHIResult::ok();
  }

  rhi::RHIResult recordSceneGeneration(
      const SceneChangeSet &,
      std::span<const SceneJournalResource>) noexcept override {
    ++calls;
    ++recordingCalls;
    if (failPoint == FailPoint::DeviceLostRecording) {
      return rhi::RHIResult::fail(rhi::RHIErrorCode::deviceLost,
                                  "injected device loss");
    }
    return failPoint == FailPoint::Recording
               ? rhi::RHIResult::fail(rhi::RHIErrorCode::backendFailure,
                                      "injected recording failure")
               : rhi::RHIResult::ok();
  }

  rhi::Result<rhi::SubmissionToken>
  submitSceneGeneration(const SceneQueueSubmissionPlan &submission,
                        std::uint32_t submissionIndex) noexcept override {
    ++calls;
    ++submitCalls;
    require(submission.valid(), "invalid submission plan reached fake RHI");
    if ((failPoint == FailPoint::FirstSubmit && submissionIndex == 0u) ||
        (failPoint == FailPoint::LaterSubmit && submissionIndex == 1u)) {
      return rhi::Result<rhi::SubmissionToken>::fail(
          rhi::RHIErrorCode::backendFailure,
          "injected queue submission failure");
    }
    rhi::SubmissionToken actual{{submissionIndex + 2u, 3u},
                                submissionIndex + 100u};
    rhi::SubmissionToken reported = actual;
    if (failPoint == FailPoint::SuccessfulInvalidTokenAndOrphanEnqueue &&
        submissionIndex == 0u) {
      reported = {};
    } else if (failPoint == FailPoint::SuccessfulNonMonotonicToken) {
      actual = {{2u, 3u}, submissionIndex + 100u};
      reported = submissionIndex == 0u
                     ? actual
                     : rhi::SubmissionToken{{2u, 3u}, 100u};
    }
    actualSubmissionTokens.push_back(actual);
    submittedTokens.push_back(reported);
    return rhi::Result<rhi::SubmissionToken>::ok(reported);
  }

  rhi::RHIResult validateBeforePublication(
      const RendererSceneMappingSnapshot &) noexcept override {
    ++calls;
    ++prepublicationCalls;
    return failPoint == FailPoint::Prepublication ||
                   failPoint == FailPoint::PrepublicationAndOrphanEnqueue
               ? rhi::RHIResult::fail(
                     rhi::RHIErrorCode::invalidState,
                     "injected prepublication invariant failure")
               : rhi::RHIResult::ok();
  }

  rhi::Result<SceneRetirementReservation> reserveSceneRetirement(
      std::span<const SceneJournalResource>,
      std::span<const rhi::SubmissionToken>) noexcept override {
    ++calls;
    ++reservationCalls;
    return failPoint == FailPoint::RetirementReservation
               ? rhi::Result<SceneRetirementReservation>::fail(
                     rhi::RHIErrorCode::backendFailure,
                     "injected retirement reservation failure")
               : rhi::Result<SceneRetirementReservation>::ok({72u});
  }

  void commitSceneRetirement(
      SceneRetirementReservation reservation) noexcept override {
    ++calls;
    require(reservation.valid(), "fake committed an invalid reservation");
    ++reservationCommits;
  }

  void cancelSceneRetirement(
      SceneRetirementReservation reservation) noexcept override {
    ++calls;
    require(reservation.valid(), "fake cancelled an invalid reservation");
    ++reservationCancels;
  }

  rhi::RHIResult destroyUnpublishedResources(
      std::span<const SceneJournalResource> resources) noexcept override {
    ++calls;
    ++destroyCalls;
    destroyedResources += resources.size();
    return rhi::RHIResult::ok();
  }

  rhi::RHIResult enqueueResourceRetirement(
      std::span<const SceneJournalResource> resources,
      std::span<const rhi::SubmissionToken> dependencies) noexcept override {
    ++calls;
    ++retirementEnqueueCalls;
    require(!dependencies.empty(),
            "submitted orphan retirement lost exact queue tokens");
    enqueuedDependencies.assign(dependencies.begin(), dependencies.end());
    if (failPoint == FailPoint::PrepublicationAndOrphanEnqueue ||
        failPoint == FailPoint::SuccessfulInvalidTokenAndOrphanEnqueue) {
      return rhi::RHIResult::fail(rhi::RHIErrorCode::backendFailure,
                                  "injected orphan retirement enqueue failure");
    }
    enqueuedResources += resources.size();
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
          "fake RHI lost the actual submission dependency");
    }
    return rhi::Result<rhi::SubmissionToken>::ok(
        actualSubmissionTokens[submissionIndex]);
  }
};

std::unique_ptr<RendererScenePublisher>
makePublisher(const RendererTargetFence &fence, FakeRhi &backend,
              SceneDeltaCounters &counters) {
  RendererSceneInitialState initial;
  initial.fence = fence;
  initial.mapping = mappingFor(fence);
  initial.activeGenerationDigest = digest(60u);
  initial.resources.push_back({{501u}, {rhi::ResourceKind::Buffer, 51u, 4u}});
  initial.lastUseSubmissionTokens.push_back({{7u, 2u}, 88u});
  return std::make_unique<RendererScenePublisher>(std::move(initial), backend,
                                                  counters);
}

void assertNoPublishedWork(
    const SceneApplyResult &result, const RendererScenePublisher &publisher,
    const std::shared_ptr<const RendererSceneGeneration> &oldGeneration) {
  require(!result.resultingFence, "failed/rejected work returned a fence");
  require(publisher.activeGeneration() == oldGeneration,
          "failed/rejected work replaced the active generation");
  require(publisher.activeGeneration()->fence() == result.oldFence,
          "failed/rejected work changed the active fence");
  require(publisher.activeGeneration()->resources().size() == 1u &&
              publisher.activeGeneration()->resources()[0].diagnosticId.value ==
                  501u,
          "staged handle became active");
  require(result.counterDelta.sceneOperationsApplied == 0u,
          "failed/rejected work appeared applied");
  require(result.counterDelta.partialPublicationCount == 0u,
          "partial-publication sentinel changed");
  require(result.counterDelta.deviceIdleCalls == 0u,
          "rollback used device idle");
  require(result.placeholderResourceCount == 0u,
          "failed/rejected work used a placeholder");
}

using RequestMutation = std::function<void(ApplySceneChangeRequest &)>;

std::optional<FullRebuildReason> expectedFallback(SceneApplyError error) {
  switch (error) {
  case SceneApplyError::StaleRevision:
  case SceneApplyError::RevisionGap:
    return FullRebuildReason::MissedRevision;
  case SceneApplyError::SceneActivationMismatch:
    return FullRebuildReason::SceneSwitch;
  case SceneApplyError::RootSerialMismatch:
    return FullRebuildReason::ExplicitCallerRequest;
  case SceneApplyError::ProjectionMismatch:
  case SceneApplyError::MappingMismatch:
  case SceneApplyError::RendererMismatch:
  case SceneApplyError::StaleObjectGeneration:
    return FullRebuildReason::MappingMismatch;
  case SceneApplyError::DeviceMismatch:
  case SceneApplyError::DeviceLost:
    return FullRebuildReason::DeviceRecovery;
  case SceneApplyError::UnsupportedCapability:
    return FullRebuildReason::UnsupportedIncrementalCapability;
  case SceneApplyError::PublicationInvariant:
  case SceneApplyError::RetirementEnqueueFailure:
    return FullRebuildReason::ExplicitCallerRequest;
  default:
    return std::nullopt;
  }
}

SceneApplyResult runNoCallRejection(const RendererTargetFence &fence,
                                    const RequestMutation &mutate,
                                    SceneApplyError expectedError) {
  FakeRhi backend;
  SceneDeltaCounters counters;
  auto publisher = makePublisher(fence, backend, counters);
  ApplySceneChangeRequest request = makeRequest(fence);
  mutate(request);
  const auto oldGeneration = publisher->activeGeneration();
  const SceneApplyResult result = publisher->applySceneChanges(request);
  require(result.disposition == SceneApplyDisposition::Rejected,
          "validation failure was not Rejected");
  require(result.error == expectedError, "validation error category changed");
  require(!result.message.empty(), "logical rejection lost its message");
  require(result.rhiResult.succeeded(),
          "logical rejection did not preserve neutral RHI success");
  require(backend.calls == 0u,
          "fence/canonical validation called the fake backend");
  require(result.queueSubmissionTokens.empty() &&
              result.handles.created.empty() &&
              result.handles.logicallyRetired.empty() &&
              result.handles.eventuallyDestroyed.empty(),
          "logical rejection returned work diagnostics");
  require(result.canonicalIdentity == request.canonicalIdentity &&
              result.canonicalDigest ==
                  request.changes.header.canonicalChangeSetDigest,
          "logical rejection lost canonical identity fields");
  require(result.counterDelta.sceneChangeSetsReceived == 1u &&
              result.counterDelta.sceneChangeSetsRejected == 1u &&
              result.counterDelta.sceneChangeSetsByError[sceneApplyErrorIndex(
                  expectedError)] == 1u,
          "logical rejection counters are not exact");
  require(result.counterDelta == sceneDeltaCounterDelta(result.countersBefore,
                                                        result.countersAfter),
          "logical rejection counter snapshots disagree");
  require(result.requestedFullSnapshotReason == expectedFallback(expectedError),
          "logical rejection fallback reason is not exact");
  assertNoPublishedWork(result, *publisher, oldGeneration);
  return result;
}

void testEveryFenceMismatchRejectsBeforeBackendCalls(
    std::array<bool, kSceneApplyErrorCount> &seen) {
  const RendererTargetFence fence = makeFence();
  const auto run = [&](const RequestMutation &mutation, SceneApplyError error) {
    const SceneApplyResult result = runNoCallRejection(fence, mutation, error);
    seen[sceneApplyErrorIndex(result.error)] = true;
  };

  run(
      [](auto &request) {
        request.expectedTarget.backend = rhi::BackendType::d3d12;
      },
      SceneApplyError::RendererMismatch);
  run([](auto &request) { ++request.expectedTarget.renderer.value; },
      SceneApplyError::RendererMismatch);
  run([](auto &request) { ++request.expectedTarget.renderer.generation; },
      SceneApplyError::RendererMismatch);
  run([](auto &request) { ++request.expectedTarget.device.value; },
      SceneApplyError::DeviceMismatch);
  run([](auto &request) { ++request.expectedTarget.device.generation; },
      SceneApplyError::DeviceMismatch);
  run([](auto &request) { ++request.expectedTarget.mappingEpoch; },
      SceneApplyError::MappingMismatch);
  run([](auto &request) { ++request.expectedTarget.authority.value; },
      SceneApplyError::AuthorityMismatch);
  run([](auto &request) { --request.expectedTarget.appliedDocumentRevision; },
      SceneApplyError::StaleRevision);
  run([](auto &request) { ++request.expectedTarget.appliedDocumentRevision; },
      SceneApplyError::RevisionGap);
  run([](auto &request) { ++request.expectedTarget.sceneActivationEpoch; },
      SceneApplyError::SceneActivationMismatch);
  run([](auto &request) { request.expectedTarget.activeScene.reset(); },
      SceneApplyError::SceneActivationMismatch);
  run([](auto &request) { ++request.expectedTarget.rootExternalStateSerial; },
      SceneApplyError::RootSerialMismatch);
  run(
      [](auto &request) {
        request.expectedTarget.projectionDigest = digest(99u);
      },
      SceneApplyError::ProjectionMismatch);
  run(
      [](auto &request) {
        request.expectedTarget.capability =
            RendererSceneUpdateCapability::Unsupported;
      },
      SceneApplyError::UnsupportedCapability);
  run(
      [](auto &request) {
        request.expectedTarget.capability =
            RendererSceneUpdateCapability::DeviceLost;
      },
      SceneApplyError::DeviceLost);
}

void testCanonicalMappingAndStateRejections(
    std::array<bool, kSceneApplyErrorCount> &seen) {
  const RendererTargetFence fence = makeFence();
  const auto run = [&](const RequestMutation &mutation, SceneApplyError error) {
    const SceneApplyResult result = runNoCallRejection(fence, mutation, error);
    seen[sceneApplyErrorIndex(result.error)] = true;
  };

  run([](auto &request) { ++request.changes.header.schemaVersion; },
      SceneApplyError::InvalidSchema);
  run([](auto &request) { request.canonicalBytes.back() ^= 0x80u; },
      SceneApplyError::InvalidDigest);
  run([](auto &request) { request.canonicalBytes.clear(); },
      SceneApplyError::InvalidPayload);
  run(
      [](auto &request) {
        const RendererTargetFence target = request.targetMapping.fence();
        request.targetMapping = RendererSceneMappingSnapshot{target, {}};
      },
      SceneApplyError::StaleObjectGeneration);
  run(
      [](auto &request) {
        RendererTargetFence wrong = request.targetMapping.fence();
        ++wrong.mappingEpoch;
        request.targetMapping = mappingFor(wrong);
      },
      SceneApplyError::MappingMismatch);

  const RendererTargetFence overflowFence =
      makeFence(std::numeric_limits<std::uint64_t>::max());
  const SceneApplyResult overflow = runNoCallRejection(
      overflowFence, [](auto &) {}, SceneApplyError::InvalidState);
  seen[sceneApplyErrorIndex(overflow.error)] = true;
}

struct FailureExpectation final {
  FailPoint point{FailPoint::None};
  SceneApplyError error{SceneApplyError::None};
  SceneApplyDisposition disposition{SceneApplyDisposition::Rejected};
  rhi::RHIErrorCode rhiError{rhi::RHIErrorCode::none};
  std::size_t expectedTokens{0};
  std::uint64_t expectedDestroyed{0};
  std::uint64_t expectedEnqueued{0};
};

SceneApplyResult runFailure(const FailureExpectation &expectation,
                            std::array<bool, kSceneApplyErrorCount> &seen) {
  const RendererTargetFence fence = makeFence();
  FakeRhi backend;
  backend.failPoint = expectation.point;
  SceneDeltaCounters counters;
  auto publisher = makePublisher(fence, backend, counters);
  const ApplySceneChangeRequest request = makeRequest(fence);
  const auto oldGeneration = publisher->activeGeneration();
  const SceneApplyResult result = publisher->applySceneChanges(request);
  require(result.disposition == expectation.disposition,
          "injected failure disposition changed");
  require(result.error == expectation.error,
          "injected failure error category changed");
  require(!result.message.empty(), "injected failure lost its message");
  require(result.rhiResult.error.code == expectation.rhiError,
          "backend-neutral RHI error was not preserved");
  require(result.rhiResult.error.message != nullptr,
          "backend-neutral RHI error message was not preserved");
  require(result.queueSubmissionTokens.size() == expectation.expectedTokens,
          "failure did not return exact successful queue tokens");
  require(result.queueSubmissionTokens == backend.submittedTokens,
          "failure queue token values are not exact");
  require(result.canonicalIdentity == request.canonicalIdentity &&
              result.canonicalDigest ==
                  request.changes.header.canonicalChangeSetDigest &&
              result.oldFence == fence,
          "failure result lost identity, digest, or old fence");
  require(backend.destroyedResources == expectation.expectedDestroyed,
          "pre-submit cleanup was not immediate and exact");
  require(backend.enqueuedResources == expectation.expectedEnqueued,
          "submitted orphan cleanup was not deferred and exact");
  const std::uint64_t expectedCreated =
      expectation.point == FailPoint::CpuAllocation ? 0u : 1u;
  require(result.handles.createdCount == expectedCreated &&
              result.counterDelta.resourcesCreated == expectedCreated &&
              result.counterDelta.resourcesPhysicallyDestroyed ==
                  expectation.expectedDestroyed,
          "created/immediately-destroyed resource diagnostics are not exact");
  if (expectation.disposition == SceneApplyDisposition::FailedAfterSubmit) {
    require(result.handles.logicallyRetiredCount == expectedCreated &&
                result.handles.eventuallyDestroyedCount == expectedCreated &&
                result.counterDelta.resourcesLogicallyRetired ==
                    expectedCreated &&
                result.counterDelta.resourcesOrphaned == expectedCreated,
            "submitted orphan lifetime diagnostics are not exact");
  } else {
    require(result.handles.logicallyRetiredCount == 0u &&
                result.handles.eventuallyDestroyedCount == expectedCreated,
            "pre-submit cleanup lifetime diagnostics are not exact");
  }
  require(result.counterDelta.sceneChangeSetsReceived == 1u &&
              result.counterDelta.rollbackCount == 1u,
          "failure rollback counters are incomplete");
  if (expectation.disposition == SceneApplyDisposition::FailedBeforeSubmit) {
    require(result.counterDelta.sceneChangeSetsFailedBeforeSubmit == 1u &&
                result.counterDelta.sceneChangeSetsFailedAfterSubmit == 0u,
            "pre-submit failure counter is incorrect");
  } else {
    require(result.counterDelta.sceneChangeSetsFailedAfterSubmit == 1u &&
                result.counterDelta.sceneChangeSetsFailedBeforeSubmit == 0u,
            "post-submit failure counter is incorrect");
  }
  require(result.counterDelta
                  .sceneChangeSetsByError[sceneApplyErrorIndex(result.error)] ==
              1u,
          "failure reason bucket is incorrect");
  require(result.counterDelta == sceneDeltaCounterDelta(result.countersBefore,
                                                        result.countersAfter),
          "failure counter snapshots disagree");
  require(result.requestedFullSnapshotReason == expectedFallback(result.error),
          "failure fallback reason is not exact");
  require(result.handles.createdCount == result.handles.created.size() &&
              result.handles.logicallyRetiredCount ==
                  result.handles.logicallyRetired.size() &&
              result.handles.eventuallyDestroyedCount ==
                  result.handles.eventuallyDestroyed.size(),
          "failure handle diagnostics are internally inconsistent");
  assertNoPublishedWork(result, *publisher, oldGeneration);
  seen[sceneApplyErrorIndex(result.error)] = true;

  if (expectation.point == FailPoint::PrepublicationAndOrphanEnqueue) {
    require(result.counterDelta.publicationFailureCount == 1u &&
                result.counterDelta.retirementEnqueueFailureCount == 1u,
            "combined prepublication/retirement failures were collapsed");
    require(publisher->hasPendingOrphanRetirement(),
            "failed orphan enqueue did not retain journal ownership");
    backend.failPoint = FailPoint::None;
    const rhi::RHIResult retried = publisher->retryPendingOrphanRetirement();
    require(retried.succeeded() && !publisher->hasPendingOrphanRetirement(),
            "retained orphan journal did not retry successfully");
  } else {
    require(!publisher->hasPendingOrphanRetirement(),
            "successful cleanup retained an orphan journal");
  }
  return result;
}

void testAllInjectedFailures(std::array<bool, kSceneApplyErrorCount> &seen) {
  const std::vector<FailureExpectation> failures{
      {FailPoint::CpuAllocation, SceneApplyError::AllocationFailure,
       SceneApplyDisposition::FailedBeforeSubmit,
       rhi::RHIErrorCode::outOfMemory, 0u, 0u, 0u},
      {FailPoint::RhiAllocation, SceneApplyError::AllocationFailure,
       SceneApplyDisposition::FailedBeforeSubmit,
       rhi::RHIErrorCode::outOfMemory, 0u, 1u, 0u},
      {FailPoint::Descriptor, SceneApplyError::DescriptorStagingFailure,
       SceneApplyDisposition::FailedBeforeSubmit,
       rhi::RHIErrorCode::backendFailure, 0u, 1u, 0u},
      {FailPoint::Recording, SceneApplyError::RecordingFailure,
       SceneApplyDisposition::FailedBeforeSubmit,
       rhi::RHIErrorCode::backendFailure, 0u, 1u, 0u},
      {FailPoint::DeviceLostRecording, SceneApplyError::DeviceLost,
       SceneApplyDisposition::FailedBeforeSubmit, rhi::RHIErrorCode::deviceLost,
       0u, 1u, 0u},
      {FailPoint::FirstSubmit, SceneApplyError::SubmissionFailure,
       SceneApplyDisposition::FailedBeforeSubmit,
       rhi::RHIErrorCode::backendFailure, 0u, 1u, 0u},
      {FailPoint::LaterSubmit, SceneApplyError::SubmissionFailure,
       SceneApplyDisposition::FailedAfterSubmit,
       rhi::RHIErrorCode::backendFailure, 1u, 0u, 1u},
      {FailPoint::Prepublication, SceneApplyError::PublicationInvariant,
       SceneApplyDisposition::FailedAfterSubmit,
       rhi::RHIErrorCode::invalidState, 2u, 0u, 1u},
      {FailPoint::RetirementReservation,
       SceneApplyError::RetirementEnqueueFailure,
       SceneApplyDisposition::FailedAfterSubmit,
       rhi::RHIErrorCode::backendFailure, 2u, 0u, 1u},
      {FailPoint::PrepublicationAndOrphanEnqueue,
       SceneApplyError::RetirementEnqueueFailure,
       SceneApplyDisposition::FailedAfterSubmit,
       rhi::RHIErrorCode::backendFailure, 2u, 0u, 0u}};

  for (const FailureExpectation &failure : failures) {
    const SceneApplyResult result = runFailure(failure, seen);
    switch (failure.error) {
    case SceneApplyError::AllocationFailure:
      require(result.counterDelta.allocationFailureCount == 1u,
              "allocation failure counter changed");
      break;
    case SceneApplyError::DescriptorStagingFailure:
      require(result.counterDelta.descriptorStagingFailureCount == 1u,
              "descriptor failure counter changed");
      break;
    case SceneApplyError::RecordingFailure:
      require(result.counterDelta.recordingFailureCount == 1u,
              "recording failure counter changed");
      break;
    case SceneApplyError::SubmissionFailure:
      require(result.counterDelta.submissionFailureCount == 1u,
              "submission failure counter changed");
      break;
    case SceneApplyError::PublicationInvariant:
      require(result.counterDelta.publicationFailureCount == 1u,
              "publication failure counter changed");
      break;
    case SceneApplyError::RetirementEnqueueFailure:
      require(result.counterDelta.retirementEnqueueFailureCount == 1u,
              "retirement enqueue failure counter changed");
      break;
    default:
      break;
    }
  }
}

void testSuccessMarksNoneAndNeverIdles(
    std::array<bool, kSceneApplyErrorCount> &seen) {
  const RendererTargetFence fence = makeFence();
  FakeRhi backend;
  SceneDeltaCounters counters;
  auto publisher = makePublisher(fence, backend, counters);
  const SceneApplyResult result =
      publisher->applySceneChanges(makeRequest(fence));
  require(result.disposition == SceneApplyDisposition::AcceptedIncremental &&
              result.error == SceneApplyError::None &&
              result.rhiResult.succeeded(),
          "fake-RHI success did not publish");
  require(result.resultingFence &&
              result.queueSubmissionTokens == backend.submittedTokens,
          "success result lost fence or exact tokens");
  require(result.counterDelta.partialPublicationCount == 0u &&
              result.counterDelta.deviceIdleCalls == 0u &&
              result.counterDelta.geometryUploadCalls == 0u &&
              result.counterDelta.textureUploadCalls == 0u,
          "continuous fake-RHI success violated zero rules");
  seen[sceneApplyErrorIndex(SceneApplyError::None)] = true;
}

void testSuccessfulTokenCaptureRejectionIsFenceSafe() {
  const RendererTargetFence fence = makeFence();
  {
    FakeRhi backend;
    backend.failPoint =
        FailPoint::SuccessfulInvalidTokenAndOrphanEnqueue;
    SceneDeltaCounters counters;
    auto publisher = makePublisher(fence, backend, counters);
    const auto active = publisher->activeGeneration();
    const SceneApplyResult failed =
        publisher->applySceneChanges(makeRequest(fence));
    require(failed.disposition == SceneApplyDisposition::FailedAfterSubmit &&
                failed.error == SceneApplyError::RetirementEnqueueFailure &&
                failed.queueSubmissionTokens.size() == 1u &&
                !failed.queueSubmissionTokens.front().isValid(),
            "fake-RHI invalid successful token was lost or misclassified");
    require(failed.message.find("queue=0:0, value=0") !=
                    std::string::npos &&
                publisher->activeGeneration() == active &&
                publisher->hasPendingOrphanRetirement() &&
                backend.recoveryCalls == 1u &&
                backend.destroyedResources == 0u &&
                backend.enqueuedResources == 0u,
            "fake-RHI invalid-token orphan was not retained with diagnostics");

    backend.failPoint = FailPoint::None;
    require(publisher->retryPendingOrphanRetirement().succeeded() &&
                !publisher->hasPendingOrphanRetirement() &&
                backend.enqueuedResources == 1u &&
                backend.enqueuedDependencies ==
                    std::vector<rhi::SubmissionToken>{
                        backend.actualSubmissionTokens.front()},
            "fake-RHI invalid-token orphan did not retire on its real fence");

    const std::size_t reportedBefore = backend.submittedTokens.size();
    const SceneApplyResult accepted =
        publisher->applySceneChanges(makeRequest(fence));
    require(accepted.disposition ==
                SceneApplyDisposition::AcceptedIncremental &&
                accepted.queueSubmissionTokens ==
                    std::vector<rhi::SubmissionToken>(
                        backend.submittedTokens.begin() +
                            static_cast<std::ptrdiff_t>(reportedBefore),
                        backend.submittedTokens.end()),
            "normal fake-RHI monotonic path did not recover after retry");
  }

  {
    FakeRhi backend;
    backend.failPoint = FailPoint::SuccessfulNonMonotonicToken;
    backend.recoveryOverrides = {
        {{2u, 3u}, 100u}, {{2u, 3u}, 99u}, {{2u, 3u}, 101u}};
    SceneDeltaCounters counters;
    auto publisher = makePublisher(fence, backend, counters);
    const auto active = publisher->activeGeneration();
    const SceneApplyResult failed =
        publisher->applySceneChanges(makeRequest(fence));
    require(failed.disposition == SceneApplyDisposition::FailedAfterSubmit &&
                failed.error == SceneApplyError::RetirementEnqueueFailure &&
                failed.queueSubmissionTokens ==
                    std::vector<rhi::SubmissionToken>{
                        {{2u, 3u}, 100u}, {{2u, 3u}, 100u}} &&
                publisher->activeGeneration() == active &&
                publisher->hasPendingOrphanRetirement() &&
                backend.recoveryCalls == 1u && backend.retirementEnqueueCalls == 0u &&
                backend.destroyedResources == 0u,
            "fake-RHI equal recovery token did not retain the orphan");

    require(!publisher->retryPendingOrphanRetirement().succeeded() &&
                publisher->activeGeneration() == active &&
                publisher->hasPendingOrphanRetirement() &&
                backend.recoveryCalls == 2u &&
                backend.retirementEnqueueCalls == 0u &&
                backend.destroyedResources == 0u,
            "fake-RHI lower recovery token was accepted as coverage");

    require(publisher->retryPendingOrphanRetirement().succeeded() &&
                publisher->activeGeneration() == active &&
                !publisher->hasPendingOrphanRetirement() &&
                backend.recoveryCalls == 3u &&
                backend.retirementEnqueueCalls == 1u &&
                backend.enqueuedDependencies ==
                    std::vector<rhi::SubmissionToken>{{{2u, 3u}, 101u}} &&
                backend.destroyedResources == 0u,
            "fake-RHI non-monotonic orphan did not retire behind its fence");
  }
}

void testEveryErrorWasObserved(
    const std::array<bool, kSceneApplyErrorCount> &seen) {
  for (std::size_t index = 0; index < seen.size(); ++index) {
    require(seen[index],
            sceneApplyErrorName(static_cast<SceneApplyError>(index)));
  }
}

} // namespace

int main() {
  try {
    std::array<bool, kSceneApplyErrorCount> seen{};
    testEveryFenceMismatchRejectsBeforeBackendCalls(seen);
    testCanonicalMappingAndStateRejections(seen);
    testAllInjectedFailures(seen);
    testSuccessMarksNoneAndNeverIdles(seen);
    testSuccessfulTokenCaptureRejectionIsFenceSafe();
    testEveryErrorWasObserved(seen);
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
