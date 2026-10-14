#include "render/editor/ScenePicking.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#if !defined(MGIF_TEST_SOURCE_DIR)
#define MGIF_TEST_SOURCE_DIR "."
#endif

namespace {

using namespace demo::editor;

template <typename T>
concept NativeHandleLeak = requires(T value) {
  value.vkHandle;
  value.d3d12Handle;
  value.metalHandle;
};

template <typename T>
concept SynchronousWaitSurface = requires(T &value) {
  value.wait();
  value.waitIdle();
};

static_assert(std::is_abstract_v<IEditorPickingPassBackend>);
static_assert(!SynchronousWaitSurface<IEditorPickingPassBackend>);
static_assert(!NativeHandleLeak<EditorPickRequest>);
static_assert(!NativeHandleLeak<EditorPickResult>);

bool expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
  }
  return condition;
}

template <typename Id>
Id id(EditorObjectKind kind, EditorDocumentAuthority authority,
      std::uint32_t slot, std::uint32_t generation = 1u) {
  return Id::from(EditorObjectId{kind, slot, generation, authority})
      .value_or(Id{});
}

struct Fixture final {
  EditorDocumentAuthority authority{71u};
  SceneId scene = id<SceneId>(EditorObjectKind::scene, authority, 1u);
  NodeId root = id<NodeId>(EditorObjectKind::node, authority, 2u);
  NodeId child = id<NodeId>(EditorObjectKind::node, authority, 3u);
  PrimitiveId primitive =
      id<PrimitiveId>(EditorObjectKind::primitive, authority, 4u);

  EditorSceneFence sceneFence() const {
    return {authority, 12u, 20u, scene, 6u, 8u};
  }

  std::vector<SceneRenderNodeRecord> nodes() const {
    return {{root, std::nullopt, 0u}, {child, root, 1u}};
  }

  std::vector<RenderOccurrenceRecord> records() const {
    return {{{40u, 3u}, {scene, root, primitive, 0u}, true},
            {{41u, 5u}, {scene, child, primitive, 0u}, true},
            {{42u, 2u}, {scene, child, primitive, 1u}, false}};
  }
};

SceneRenderOccurrenceSnapshot makeMapping(const Fixture &fixture,
                                          std::uint64_t generation = 30u) {
  SceneRenderOccurrenceMappingCreateResult result =
      SceneRenderOccurrenceMapping::create(
          fixture.sceneFence(), generation, fixture.nodes(), fixture.records());
  return result.succeeded() ? std::move(result.snapshot) : nullptr;
}

EditorPickRequest request(const Fixture &fixture, std::uint64_t serial,
                          std::uint64_t occurrenceGeneration = 30u,
                          PickIntent intent = PickIntent::singleSelect) {
  EditorPickRequest value;
  value.serial = serial;
  value.intent = intent;
  value.fence = {fixture.sceneFence(), 11u, 13u};
  value.occurrenceGeneration = occurrenceGeneration;
  value.sample = {3u, 4u, 128u, 96u};
  value.ray = {{0.0, 0.0, 0.0}, {0.0, 0.0, -1.0}};
  return value;
}

class FakeBackend final : public IEditorPickingPassBackend {
public:
  struct Retired final {
    EditorPickingPassTicket ticket;
    demo::rhi::SubmissionToken submission;

    bool operator==(const Retired &) const = default;
  };

  [[nodiscard]] EditorPickingBackendSubmission
  enqueue(const EditorPickRequest &value) noexcept override {
    ++enqueueCalls;
    submittedRequests.push_back(value);
    if (rejectNext) {
      rejectNext = false;
      return {false, {}, {}, PickResultReason::backendRejected};
    }
    const EditorPickingPassTicket ticket{nextTicket++};
    const demo::rhi::SubmissionToken submission{
        demo::rhi::QueueIdentity{3u, 2u}, nextSubmissionValue++};
    tickets.push_back(ticket);
    submissions.push_back(submission);
    return {true, ticket, submission, PickResultReason::none};
  }

  [[nodiscard]] std::optional<EditorPickingBackendReadback>
  pollOne() noexcept override {
    ++pollCalls;
    if (readbacks.empty()) {
      return std::nullopt;
    }
    EditorPickingBackendReadback result = readbacks.front();
    readbacks.erase(readbacks.begin());
    return result;
  }

  void cancel(EditorPickingPassTicket ticket) noexcept override {
    cancelled.push_back(ticket);
  }

  void retireReadback(EditorPickingPassTicket ticket,
                      demo::rhi::SubmissionToken submission) noexcept override {
    retired.push_back({ticket, submission});
  }

  void completeSample(EditorPickingPassTicket ticket,
                      FramebufferPickToken token,
                      EditorWorldPoint point = {0.0, 0.0, -5.0},
                      double distance = 5.0,
                      std::optional<demo::rhi::SubmissionToken> submission =
                          std::nullopt) {
    readbacks.push_back(
        {ticket, submission.value_or(latestSubmission(ticket)),
         EditorPickingBackendReadbackStatus::sampled, token, point, distance,
         PickResultReason::none});
  }

  void completeMiss(EditorPickingPassTicket ticket) {
    readbacks.push_back(
        {ticket, latestSubmission(ticket),
         EditorPickingBackendReadbackStatus::miss, {}, std::nullopt,
         std::nullopt, PickResultReason::background});
  }

  void completeError(EditorPickingPassTicket ticket) {
    readbacks.push_back(
        {ticket, latestSubmission(ticket),
         EditorPickingBackendReadbackStatus::error, {}, std::nullopt,
         std::nullopt, PickResultReason::backendFailure});
  }

  void completeRaw(EditorPickingPassTicket ticket,
                   demo::rhi::SubmissionToken submission,
                   EditorPickingBackendReadbackStatus status,
                   PickResultReason reason,
                   FramebufferPickToken token = {}) {
    readbacks.push_back(
        {ticket, submission, status, token, std::nullopt, std::nullopt,
         reason});
  }

  [[nodiscard]] demo::rhi::SubmissionToken
  latestSubmission(EditorPickingPassTicket ticket) const {
    for (std::size_t index = tickets.size(); index > 0; --index) {
      if (tickets[index - 1u] == ticket) {
        return submissions[index - 1u];
      }
    }
    return {};
  }

  bool rejectNext{false};
  std::uint64_t nextTicket{1u};
  std::uint64_t nextSubmissionValue{101u};
  std::uint64_t enqueueCalls{0};
  std::uint64_t pollCalls{0};
  std::vector<EditorPickRequest> submittedRequests;
  std::vector<EditorPickingPassTicket> tickets;
  std::vector<demo::rhi::SubmissionToken> submissions;
  std::vector<EditorPickingBackendReadback> readbacks;
  std::vector<EditorPickingPassTicket> cancelled;
  std::vector<Retired> retired;
};

class FixedTerminalSink final : public IScenePickingTerminalSink {
public:
  void onTerminal(const EditorPickResult &result) noexcept override {
    if (count >= results.size()) {
      std::terminate();
    }
    if (observedSnapshot != nullptr && !observedSnapshot->expired()) {
      snapshotAliveDuringCallback = true;
    }
    results[count++] = result;
  }

  std::array<EditorPickResult, 16> results{};
  std::size_t count{0};
  const std::weak_ptr<const SceneRenderOccurrenceMapping> *observedSnapshot{
      nullptr};
  bool snapshotAliveDuringCallback{false};
};

bool testRejectedSubmissionHasNoTerminalOrBackendLeak() {
  Fixture fixture;
  const SceneRenderOccurrenceSnapshot snapshot = makeMapping(fixture);
  if (!expect(snapshot != nullptr, "submission fixture mapping seals")) {
    return false;
  }
  FakeBackend backend;
  FixedTerminalSink sink;
  ScenePicking picking(backend, sink);
  bool passed = true;

  EditorPickRequest invalid = request(fixture, 1u);
  invalid.sample.pixelX = invalid.sample.pixelWidth;
  passed &= expect(!picking.submit(invalid, snapshot).accepted() &&
                       backend.enqueueCalls == 0u && picking.pendingCount() == 0u,
                   "invalid request is rejected before backend call");
  EditorPickRequest wrongGeneration = request(fixture, 1u, 31u);
  passed &= expect(!picking.submit(wrongGeneration, snapshot).accepted() &&
                       backend.enqueueCalls == 0u,
                   "snapshot generation mismatch is no-call rejection");
  EditorPickRequest wrongFence = request(fixture, 1u);
  ++wrongFence.fence.scene.projectionRevision;
  passed &= expect(!picking.submit(wrongFence, snapshot).accepted() &&
                       backend.enqueueCalls == 0u,
                   "scene fence mismatch is no-call rejection");

  backend.rejectNext = true;
  const EditorPickRequest valid = request(fixture, 1u);
  passed &= expect(!picking.submit(valid, snapshot).accepted() &&
                       backend.enqueueCalls == 1u && picking.pendingCount() == 0u,
                   "backend rejection never becomes an accepted request");
  std::vector<EditorPickResult> results;
  picking.poll(results);
  passed &= expect(results.empty(),
                   "rejected submission fabricates no terminal completion");

  passed &= expect(picking.submit(valid, snapshot).accepted() &&
                       backend.enqueueCalls == 2u && picking.pendingCount() == 1u,
                   "rejected serial may be retried because it was never accepted");
  passed &= expect(!picking.submit(valid, snapshot).accepted() &&
                       backend.enqueueCalls == 2u,
                   "accepted serial cannot be submitted twice");
  picking.cancelAll();
  picking.poll(results);
  passed &= expect(results.size() == 1u &&
                       results[0].completion == PickCompletion::cancelled &&
                       results[0].terminalPayloadValid(),
                   "the one accepted retry receives exactly one terminal cancel");
  return passed;
}

bool testOutOfOrderTerminalAndDuplicateSuppression() {
  Fixture fixture;
  const SceneRenderOccurrenceSnapshot snapshot = makeMapping(fixture);
  if (!expect(snapshot != nullptr, "out-of-order mapping seals")) {
    return false;
  }
  const std::vector<RenderOccurrenceRecord> records = fixture.records();
  FakeBackend backend;
  FixedTerminalSink sink;
  ScenePicking picking(backend, sink);
  bool passed = true;
  passed &= expect(picking.submit(request(fixture, 1u), snapshot).accepted() &&
                       picking.submit(request(fixture, 2u), snapshot).accepted() &&
                       picking.submit(request(fixture, 3u), snapshot).accepted(),
                   "three monotonic requests are accepted");
  const FramebufferPickToken token =
      snapshot->framebufferToken(records[1].id).value_or(
          FramebufferPickToken{});
  backend.completeSample(backend.tickets[2], token);
  backend.completeMiss(backend.tickets[0]);
  backend.completeError(backend.tickets[1]);

  std::vector<EditorPickResult> results;
  picking.poll(results);
  passed &= expect(results.size() == 3u && results[0].serial == 3u &&
                       results[0].completion == PickCompletion::hit &&
                       results[0].occurrence == records[1].id &&
                       results[1].serial == 1u &&
                       results[1].completion == PickCompletion::miss &&
                       results[2].serial == 2u &&
                       results[2].completion == PickCompletion::error,
                   "producer preserves backend completion order for controller buffering");
  for (const EditorPickResult &result : results) {
    passed &= expect(result.terminalPayloadValid(),
                     "each accepted request emits a closed terminal payload");
  }
  passed &= expect(picking.pendingCount() == 0u &&
                       backend.retired.size() == 3u &&
                       picking.passStats().terminalCompletions == 3u &&
                       picking.passStats().synchronousWaitCalls == 0u,
                   "all readbacks retire by token without synchronous wait");
  for (std::size_t index = 0; index < backend.retired.size(); ++index) {
    const auto ticket = backend.retired[index].ticket;
    const auto found = std::find(backend.tickets.begin(), backend.tickets.end(),
                                 ticket);
    passed &= expect(found != backend.tickets.end() &&
                         backend.retired[index].submission ==
                             backend.submissions[static_cast<std::size_t>(
                                 std::distance(backend.tickets.begin(), found))],
                     "retirement preserves exact queue submission token");
  }

  backend.completeSample(backend.tickets[2], token);
  const std::size_t before = results.size();
  picking.poll(results);
  passed &= expect(results.size() == before &&
                       picking.passStats().ignoredUnknownOrDuplicateReadbacks ==
                           1u,
                   "late duplicate completion cannot terminate a request twice");
  return passed;
}

bool testResultValidationPartitions() {
  Fixture fixture;
  const SceneRenderOccurrenceSnapshot snapshot = makeMapping(fixture);
  if (!expect(snapshot != nullptr, "partition mapping seals")) {
    return false;
  }
  const std::vector<RenderOccurrenceRecord> records = fixture.records();
  FakeBackend backend;
  FixedTerminalSink sink;
  ScenePicking picking(backend, sink);
  for (std::uint64_t serial = 1u; serial <= 7u; ++serial) {
    if (!expect(picking.submit(request(fixture, serial), snapshot).accepted(),
                "partition request accepted")) {
      return false;
    }
  }

  const FramebufferPickToken visible =
      snapshot->framebufferToken(records[0].id).value();
  const FramebufferPickToken hidden =
      snapshot->framebufferToken(records[2].id).value();
  backend.completeSample(backend.tickets[0], visible);
  backend.completeSample(backend.tickets[1], {});
  backend.completeSample(backend.tickets[2], hidden);
  backend.completeSample(
      backend.tickets[3], visible,
      {std::numeric_limits<double>::quiet_NaN(), 0.0, -5.0}, 5.0);
  backend.completeSample(backend.tickets[4], visible, {0.0, 0.0, 0.0}, 0.0);
  backend.completeMiss(backend.tickets[5]);
  backend.completeError(backend.tickets[6]);

  std::vector<EditorPickResult> results;
  picking.poll(results);
  const std::array<PickCompletion, 7> completions{
      PickCompletion::hit,  PickCompletion::miss, PickCompletion::miss,
      PickCompletion::miss, PickCompletion::miss, PickCompletion::miss,
      PickCompletion::error};
  const std::array<PickResultReason, 7> reasons{
      PickResultReason::none,
      PickResultReason::invalidToken,
      PickResultReason::hiddenOccurrence,
      PickResultReason::invalidHit,
      PickResultReason::invalidHit,
      PickResultReason::background,
      PickResultReason::backendFailure};
  bool passed = expect(results.size() == completions.size(),
                       "every partition terminates");
  for (std::size_t index = 0; index < results.size(); ++index) {
    passed &= expect(results[index].completion == completions[index] &&
                         results[index].reason == reasons[index] &&
                         results[index].terminalPayloadValid(),
                     "raw readback maps to closed hit/miss/error partition");
  }
  return passed;
}

bool testMalformedBackendStatusReasonMatrixClosesToValidTerminal() {
  Fixture fixture;
  const SceneRenderOccurrenceSnapshot snapshot = makeMapping(fixture);
  if (!expect(snapshot != nullptr, "malformed matrix mapping seals")) {
    return false;
  }
  FakeBackend backend;
  FixedTerminalSink sink;
  ScenePicking picking(backend, sink);
  for (std::uint64_t serial = 1u; serial <= 5u; ++serial) {
    if (!expect(picking.submit(request(fixture, serial), snapshot).accepted(),
                "malformed matrix request accepted")) {
      return false;
    }
  }
  backend.completeRaw(backend.tickets[0], backend.submissions[0],
                      EditorPickingBackendReadbackStatus::miss,
                      PickResultReason::backendRejected);
  backend.completeRaw(backend.tickets[1], backend.submissions[1],
                      EditorPickingBackendReadbackStatus::miss,
                      PickResultReason::cancelled);
  backend.completeRaw(backend.tickets[2], backend.submissions[2],
                      EditorPickingBackendReadbackStatus::sampled,
                      PickResultReason::background, {1u});
  backend.completeRaw(backend.tickets[3], backend.submissions[3],
                      EditorPickingBackendReadbackStatus::error,
                      PickResultReason::none);
  backend.completeRaw(backend.tickets[4], backend.submissions[4],
                      EditorPickingBackendReadbackStatus::cancelled,
                      PickResultReason::backendFailure);

  std::vector<EditorPickResult> results;
  picking.poll(results);
  bool passed = expect(results.size() == 5u,
                       "every malformed backend combination terminates once");
  for (std::size_t index = 0; index < results.size(); ++index) {
    const PickCompletion expected = index == 4u ? PickCompletion::cancelled
                                                 : PickCompletion::error;
    const PickResultReason reason =
        index == 4u ? PickResultReason::cancelled
                    : PickResultReason::backendFailure;
    passed &= expect(results[index].completion == expected &&
                         results[index].reason == reason &&
                         results[index].terminalPayloadValid(),
                     "malformed backend payload normalizes to closed terminal");
  }
  return passed;
}

bool testTicketReuseRejectsOldSubmissionReadback() {
  Fixture fixture;
  const SceneRenderOccurrenceSnapshot snapshot = makeMapping(fixture);
  if (!expect(snapshot != nullptr, "ticket ABA mapping seals")) {
    return false;
  }
  const FramebufferPickToken token =
      snapshot->framebufferToken(fixture.records()[0].id).value();
  FakeBackend backend;
  FixedTerminalSink sink;
  ScenePicking picking(backend, sink);
  bool passed = expect(picking.submit(request(fixture, 1u), snapshot).accepted(),
                       "old ticket request accepted");
  const EditorPickingPassTicket reused_ticket = backend.tickets[0];
  const demo::rhi::SubmissionToken old_submission = backend.submissions[0];
  picking.cancelAll();
  std::vector<EditorPickResult> cancelled;
  picking.poll(cancelled);
  passed &= expect(cancelled.size() == 1u &&
                       cancelled[0].completion == PickCompletion::cancelled,
                   "old request reaches terminal cancellation before reuse");

  backend.nextTicket = reused_ticket.value;
  passed &= expect(picking.submit(request(fixture, 2u), snapshot).accepted(),
                   "backend may reuse retired ticket with new submission token");
  const demo::rhi::SubmissionToken new_submission = backend.submissions.back();
  passed &= expect(new_submission != old_submission,
                   "reused ticket carries a distinct submission generation");
  backend.completeSample(reused_ticket, token, {0.0, 0.0, -5.0}, 5.0,
                         old_submission);
  backend.completeSample(reused_ticket, token, {0.0, 0.0, -5.0}, 5.0,
                         new_submission);
  std::vector<EditorPickResult> results;
  picking.poll(results);
  passed &= expect(
      results.size() == 1u && results[0].serial == 2u &&
          results[0].completion == PickCompletion::hit &&
          picking.passStats().ignoredUnknownOrDuplicateReadbacks == 1u &&
          backend.retired.size() == 2u &&
          backend.retired.back().submission == new_submission,
      "late old submission cannot terminate or retire the reused-ticket request");
  return passed;
}

bool testCancellationRetainsSnapshotUntilTerminalConsumption() {
  Fixture fixture;
  SceneRenderOccurrenceSnapshot snapshot = makeMapping(fixture);
  if (!expect(snapshot != nullptr, "lifetime mapping seals")) {
    return false;
  }
  std::weak_ptr<const SceneRenderOccurrenceMapping> retained = snapshot;
  FakeBackend backend;
  FixedTerminalSink sink;
  ScenePicking picking(backend, sink);
  bool passed = true;
  passed &= expect(picking.submit(request(fixture, 1u), snapshot).accepted() &&
                       picking.submit(request(fixture, 2u), snapshot).accepted(),
                   "lifetime requests accepted");
  snapshot.reset();
  passed &= expect(!retained.expired(),
                   "pending readbacks retain exact immutable occurrence snapshot");

  picking.cancelAuthority(fixture.authority);
  passed &= expect(!retained.expired() && backend.cancelled.size() == 2u &&
                       backend.retired.size() == 2u,
                   "cancel schedules token retirement before releasing snapshot");
  std::vector<EditorPickResult> results;
  picking.poll(results);
  passed &= expect(results.size() == 2u &&
                       results[0].completion == PickCompletion::cancelled &&
                       results[1].completion == PickCompletion::cancelled &&
                       retained.expired() && picking.pendingCount() == 0u,
                   "snapshot releases only after terminal cancellations are consumed");

  backend.completeMiss(backend.tickets[0]);
  backend.completeMiss(backend.tickets[1]);
  const std::size_t before = results.size();
  picking.poll(results);
  passed &= expect(results.size() == before &&
                       picking.passStats().ignoredUnknownOrDuplicateReadbacks ==
                           2u,
                   "late physical readbacks after cancellation are ignored");
  return passed;
}

bool testDestructionDeliversReadyAndPendingCancellation() {
  Fixture fixture;
  bool passed = true;

  SceneRenderOccurrenceSnapshot pending_snapshot = makeMapping(fixture);
  std::weak_ptr<const SceneRenderOccurrenceMapping> pending_weak =
      pending_snapshot;
  FakeBackend pending_backend;
  FixedTerminalSink pending_sink;
  pending_sink.observedSnapshot = &pending_weak;
  {
    ScenePicking picking(pending_backend, pending_sink);
    passed &= expect(
        picking.submit(request(fixture, 1u), pending_snapshot).accepted() &&
            picking.submit(request(fixture, 2u), pending_snapshot).accepted(),
        "pending destructor requests accepted");
    pending_snapshot.reset();
  }
  passed &= expect(
      pending_sink.count == 2u &&
          pending_sink.results[0].completion == PickCompletion::cancelled &&
          pending_sink.results[1].completion == PickCompletion::cancelled &&
          pending_sink.results[0].terminalPayloadValid() &&
          pending_sink.results[1].terminalPayloadValid() &&
          pending_sink.snapshotAliveDuringCallback && pending_weak.expired() &&
          pending_backend.cancelled.size() == 2u &&
          pending_backend.retired.size() == 2u,
      "destructor emits terminal cancels before releasing retained snapshots");

  SceneRenderOccurrenceSnapshot ready_snapshot = makeMapping(fixture, 31u);
  std::weak_ptr<const SceneRenderOccurrenceMapping> ready_weak = ready_snapshot;
  FakeBackend ready_backend;
  FixedTerminalSink ready_sink;
  ready_sink.observedSnapshot = &ready_weak;
  {
    ScenePicking picking(ready_backend, ready_sink);
    passed &= expect(
        picking.submit(request(fixture, 3u, 31u), ready_snapshot).accepted(),
        "ready destructor request accepted");
    ready_snapshot.reset();
    picking.cancelAll();
  }
  passed &= expect(
      ready_sink.count == 1u &&
          ready_sink.results[0].serial == 3u &&
          ready_sink.results[0].completion == PickCompletion::cancelled &&
          ready_sink.results[0].terminalPayloadValid() &&
          ready_sink.snapshotAliveDuringCallback && ready_weak.expired() &&
          ready_backend.cancelled.size() == 1u &&
          ready_backend.retired.size() == 1u,
      "ready-but-unpolled terminal is delivered exactly once during shutdown");
  return passed;
}

bool testFenceMatrixAndViewportSamples() {
  Fixture fixture;
  const EditorPickRequest baseline = request(fixture, 1u);
  bool passed = expect(baseline.valid(), "baseline full pick fence is valid");
  std::array<EditorPickFence, 8> changed{
      baseline.fence, baseline.fence, baseline.fence, baseline.fence,
      baseline.fence, baseline.fence, baseline.fence, baseline.fence};
  changed[0].scene.authority.value += 1u;
  changed[1].scene.documentRevision += 1u;
  changed[2].scene.projectionRevision += 1u;
  changed[3].scene.scene = id<SceneId>(EditorObjectKind::scene,
                                      fixture.authority, 99u);
  changed[4].scene.sceneEpoch += 1u;
  changed[5].scene.deviceGeneration += 1u;
  changed[6].viewportGeneration += 1u;
  changed[7].viewRevision += 1u;
  for (const EditorPickFence &candidate : changed) {
    passed &= expect(!sameEditorPickFence(baseline.fence, candidate),
                     "every full-fence field participates in stale detection");
  }

  EditorPickRequest edge = baseline;
  edge.sample = {127u, 95u, 128u, 96u};
  EditorPickRequest outside = edge;
  outside.sample.pixelX = outside.sample.pixelWidth;
  passed &= expect(edge.valid() && !outside.valid(),
                   "attachment edge is half-open and resize extent is request-bound");
  return passed;
}

bool testShaderContract() {
  const std::filesystem::path path =
      std::filesystem::path(MGIF_TEST_SOURCE_DIR) / "shaders" /
      "editor_picking.slang";
  std::ifstream input(path, std::ios::binary);
  const std::string source((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
  return expect(input.good() || input.eof(), "picking shader is readable") &&
         expect(source.find("editorPickingVertexMain") != std::string::npos &&
                    source.find("editorPickingFragmentMain") !=
                        std::string::npos &&
                    source.find("SV_Target0") != std::string::npos &&
                    source.find("framebufferToken") != std::string::npos &&
                    source.find("discard") != std::string::npos,
                "shader outputs snapshot token and preserves alpha cutout") &&
         expect(source.find("EditorObjectId") == std::string::npos &&
                    source.find("VkImage") == std::string::npos,
                "shader contains no stable-ID or native-handle encoding");
}

} // namespace

int main() {
  bool passed = true;
  passed &= testRejectedSubmissionHasNoTerminalOrBackendLeak();
  passed &= testOutOfOrderTerminalAndDuplicateSuppression();
  passed &= testResultValidationPartitions();
  passed &= testMalformedBackendStatusReasonMatrixClosesToValidTerminal();
  passed &= testTicketReuseRejectsOldSubmissionReadback();
  passed &= testCancellationRetainsSnapshotUntilTerminalConsumption();
  passed &= testDestructionDeliversReadyAndPendingCancellation();
  passed &= testFenceMatrixAndViewportSamples();
  passed &= testShaderContract();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
