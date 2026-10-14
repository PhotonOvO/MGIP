#include <mgif/scene_editor/EditorPicking.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace demo::editor;
using namespace std::chrono_literals;

template <typename T>
concept NativeHandleLeak = requires(T value) {
  value.vkHandle;
  value.d3d12Handle;
  value.metalHandle;
};

static_assert(!NativeHandleLeak<EditorPickEmission>);
static_assert(!NativeHandleLeak<EditorPickingEffect>);

bool expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
  }
  return condition;
}

class FakeClock final : public EditorClock {
public:
  [[nodiscard]] EditorTimePoint now() const noexcept override { return value; }
  void set(EditorTimePoint::duration elapsed) noexcept {
    value = EditorTimePoint{elapsed};
  }

  EditorTimePoint value{};
};

template <typename Id>
Id id(EditorObjectKind kind, EditorDocumentAuthority authority,
      std::uint32_t slot, std::uint32_t generation = 1u) {
  return Id::from(EditorObjectId{kind, slot, generation, authority})
      .value_or(Id{});
}

struct Fixture final {
  EditorDocumentAuthority authority{91u};
  SceneId scene = id<SceneId>(EditorObjectKind::scene, authority, 1u);
  SceneId otherScene = id<SceneId>(EditorObjectKind::scene, authority, 2u);
  NodeId nodeA = id<NodeId>(EditorObjectKind::node, authority, 10u);
  NodeId nodeB = id<NodeId>(EditorObjectKind::node, authority, 11u);
  PrimitiveId primitive =
      id<PrimitiveId>(EditorObjectKind::primitive, authority, 20u);

  PrimitiveOccurrenceKey keyA() const {
    return {scene, nodeA, primitive, 0u};
  }
  PrimitiveOccurrenceKey keyB() const {
    return {scene, nodeB, primitive, 0u};
  }
};

EditorViewportMetrics viewport(double dpi = 1.0,
                               std::uint64_t generation = 17u,
                               std::uint64_t projectionRevision = 23u,
                               std::uint64_t viewRevision = 29u) {
  EditorViewportMetrics result;
  result.logical = {{0.0, 0.0}, {100.0, 80.0}};
  result.framebufferLogicalOrigin = {0.0, 0.0};
  result.physical = {{0u, 0u},
                     {static_cast<std::uint32_t>(100.0 * dpi),
                      static_cast<std::uint32_t>(80.0 * dpi)}};
  result.depthPhysical = result.physical;
  result.colorAttachment = result.physical.extent;
  result.depthAttachment = result.physical.extent;
  result.framebufferScale = {dpi, dpi};
  result.viewportGeneration = generation;
  result.projection = {projectionRevision, 0xabcdu};
  result.view = {viewRevision, 0xef01u};
  result.convention = {EditorClipDepthRange::zeroToOne, true, true};
  result.projectionKind = EditorProjectionKind::perspective;
  result.verticalFieldOfViewRadians = 1.0;
  return result;
}

EditorPickFence fence(const Fixture &fixture,
                      const EditorViewportMetrics &metrics) {
  return {{fixture.authority, 31u, metrics.projection.revision, fixture.scene,
           37u, 41u},
          metrics.viewportGeneration, metrics.view.revision};
}

WorldRay ray() {
  return {{1.0, 2.0, 3.0}, {0.0, 0.0, -1.0}};
}

EditorPickRequest request(const Fixture &fixture, std::uint64_t serial,
                          PickIntent intent = PickIntent::singleSelect,
                          std::uint64_t occurrenceGeneration = 43u) {
  const EditorViewportMetrics metrics = viewport();
  return {serial, intent, fence(fixture, metrics), occurrenceGeneration,
          {4u, 5u, metrics.physical.extent.width,
           metrics.physical.extent.height},
          ray()};
}

EditorPickResult hit(const EditorPickRequest &request,
                     RenderOccurrenceId occurrence,
                     double distance = 5.0,
                     EditorWorldPoint point = {1.0, 2.0, -2.0}) {
  return {request.serial,
          request.intent,
          request.fence,
          request.occurrenceGeneration,
          PickCompletion::hit,
          PickResultReason::none,
          occurrence,
          point,
          distance};
}

EditorPickResult miss(const EditorPickRequest &request,
                      PickResultReason reason = PickResultReason::background) {
  return {request.serial,
          request.intent,
          request.fence,
          request.occurrenceGeneration,
          PickCompletion::miss,
          reason,
          std::nullopt,
          std::nullopt,
          std::nullopt};
}

EditorPickResult error(const EditorPickRequest &request) {
  EditorPickResult result = miss(request);
  result.completion = PickCompletion::error;
  result.reason = PickResultReason::backendFailure;
  return result;
}

EditorPickResult cancelled(const EditorPickRequest &request) {
  EditorPickResult result = miss(request);
  result.completion = PickCompletion::cancelled;
  result.reason = PickResultReason::cancelled;
  return result;
}

class FakeOccurrenceLookup final : public IEditorPickingOccurrenceLookup {
public:
  [[nodiscard]] std::optional<RenderOccurrenceRecord>
  resolve(RenderOccurrenceId occurrence) const noexcept override {
    for (const RenderOccurrenceRecord &record : records) {
      if (record.id == occurrence) {
        return record;
      }
    }
    return std::nullopt;
  }

  std::vector<RenderOccurrenceRecord> records;
};

EditorPickingCommitContext context(const Fixture &fixture,
                                   const FakeOccurrenceLookup &lookup) {
  EditorPickingCommitContext result;
  result.currentFence = fence(fixture, viewport());
  result.occurrenceGeneration = 43u;
  result.currentSelection = SceneSelectionValue{};
  result.selectionRevision = 7u;
  result.occurrences = &lookup;
  return result;
}

bool testStrictClassifierAndEmissionTimeSnapshot() {
  FakeClock clock;
  EditorClickClassifier classifier(clock);
  const EditorViewportMetrics firstMetrics = viewport(1.0, 17u, 23u, 29u);
  const EditorViewportMetrics laterMetrics = viewport(2.0, 18u, 24u, 30u);
  bool passed = expect(classifier.valid(), "classifier config is valid");

  clock.set(0ms);
  passed &= expect(!classifier.primaryPress({10.0, 10.0}, firstMetrics, false) &&
                       classifier.primaryDown(),
                   "first press produces no physical intent");
  classifier.primaryRelease({10.0, 10.0});
  passed &= expect(classifier.hasPendingSingle(),
                   "first release enters SINGLE_PENDING");
  clock.set(299ms);
  passed &= expect(!classifier.update({20.0, 30.0}, laterMetrics, true),
                   "T-epsilon emits no single");
  clock.set(300ms);
  passed &= expect(!classifier.update({25.0, 35.0}, laterMetrics, true),
                   "strict T boundary emits no single");
  clock.set(301ms);
  const auto single = classifier.update({40.0, 20.0}, laterMetrics, true);
  passed &= expect(single && single->intent == PickIntent::singleSelect &&
                       single->logicalPosition == LogicalViewportPoint{40.0, 20.0} &&
                       single->viewportGeneration == 18u &&
                       single->projection == laterMetrics.projection &&
                       single->view == laterMetrics.view &&
                       single->sample == ViewportSample{80u, 40u, 200u, 160u} &&
                       classifier.phase() == EditorClickClassifierPhase::idle,
                   "T+epsilon samples current pointer and current viewport");

  clock.set(1s);
  (void)classifier.primaryPress({5.0, 5.0}, firstMetrics, false);
  classifier.primaryRelease({5.0, 5.0});
  clock.set(2s);
  passed &= expect(!classifier.update({50.0, 50.0}, laterMetrics, false) &&
                       classifier.hasPendingSingle(),
                   "ineligible frames pause rather than expire pending single");
  const auto resumed = classifier.update({60.0, 40.0}, laterMetrics, true);
  passed &= expect(resumed &&
                       resumed->logicalPosition == LogicalViewportPoint{60.0, 40.0},
                   "first later eligible update emits at current pointer");

  clock.set(3s);
  (void)classifier.primaryPress({10.0, 10.0}, firstMetrics, false);
  classifier.primaryRelease({10.0, 10.0});
  clock.set(3100ms);
  const auto doublePick =
      classifier.primaryPress({12.0, 11.0}, laterMetrics, true);
  passed &= expect(doublePick &&
                       doublePick->intent == PickIntent::doubleFocus &&
                       classifier.phase() ==
                           EditorClickClassifierPhase::doubleClick &&
                       !classifier.hasPendingSingle(),
                   "recognized second press emits one immediate double");
  classifier.primaryRelease({12.0, 11.0});
  clock.set(4s);
  passed &= expect(!classifier.update({12.0, 11.0}, laterMetrics, true),
                   "double release leaves no delayed single");

  (void)classifier.primaryPress({10.0, 10.0}, firstMetrics, false);
  classifier.pointerMove({17.0, 10.0});
  classifier.primaryRelease({17.0, 10.0});
  clock.set(5s);
  passed &= expect(classifier.phase() == EditorClickClassifierPhase::idle &&
                       !classifier.update({17.0, 10.0}, firstMetrics, true),
                   "drag emits neither single nor double");
  classifier.reset();
  passed &= expect(!classifier.primaryDown() && !classifier.hasPendingSingle(),
                   "lifecycle reset clears all physical intent");
  return passed;
}

bool testDpiBoundariesAndRequestConstruction() {
  Fixture fixture;
  bool passed = true;
  for (const double dpi : {1.0, 1.25, 1.5, 2.0}) {
    FakeClock clock;
    EditorClickClassifier classifier(clock);
    const EditorViewportMetrics metrics = viewport(dpi);
    clock.set(0ms);
    (void)classifier.primaryPress({99.999, 79.999}, metrics, false);
    classifier.primaryRelease({99.999, 79.999});
    clock.set(301ms);
    const auto emission = classifier.update({99.999, 79.999}, metrics, true);
    passed &= expect(emission && emission->sample.pixelX + 1u ==
                                      emission->sample.pixelWidth &&
                         emission->sample.pixelY + 1u ==
                                      emission->sample.pixelHeight,
                     "last half-open logical point maps to last physical pixel");
    classifier.reset();
    clock.set(1s);
    passed &= expect(!classifier.primaryPress({100.0, 40.0}, metrics, false) &&
                         !classifier.primaryDown(),
                     "right edge remains outside at every tested DPI");
  }

  FakeClock clock;
  EditorClickClassifier classifier(clock);
  const EditorViewportMetrics metrics = viewport();
  clock.set(0ms);
  (void)classifier.primaryPress({20.0, 20.0}, metrics, false);
  classifier.primaryRelease({20.0, 20.0});
  clock.set(301ms);
  const auto emission = classifier.update({30.0, 40.0}, metrics, true);
  const auto built = emission
                         ? makeEditorPickRequest(9u, *emission,
                                                 fence(fixture, metrics), 43u,
                                                 ray())
                         : std::nullopt;
  passed &= expect(built && built->valid() && built->serial == 9u &&
                       built->sample == emission->sample &&
                       built->ray == ray(),
                   "emission builds the frozen P6-10 request exactly once");
  EditorPickFence staleFence = fence(fixture, metrics);
  ++staleFence.viewRevision;
  passed &= expect(emission &&
                       !makeEditorPickRequest(10u, *emission, staleFence, 43u,
                                              ray()),
                   "emission cannot be relabeled with another view revision");
  return passed;
}

bool testSerialOrderingAndHeadOfLineRelease() {
  Fixture fixture;
  const RenderOccurrenceId occurrenceA{100u, 3u};
  const RenderOccurrenceId occurrenceB{101u, 4u};
  FakeOccurrenceLookup lookup;
  lookup.records = {{occurrenceA, fixture.keyA(), true},
                    {occurrenceB, fixture.keyB(), true}};
  EditorPickingCommitContext current = context(fixture, lookup);
  EditorPickingController controller;
  const EditorPickRequest first = request(fixture, 1u);
  const EditorPickRequest second = request(fixture, 2u);
  const EditorPickRequest third = request(fixture, 3u);
  bool passed = expect(controller.trackAccepted(first).accepted() &&
                           controller.trackAccepted(second).accepted() &&
                           controller.trackAccepted(third).accepted(),
                       "three monotonic accepted requests are tracked");
  passed &= expect(controller.acceptTerminal(hit(third, occurrenceB)) &&
                       !controller.commitReady(current),
                   "serial three cannot pass an incomplete head");
  passed &= expect(controller.acceptTerminal(hit(first, occurrenceA)),
                   "head terminal is accepted after out-of-order tail");
  const auto firstEffect = controller.commitReady(current);
  passed &= expect(firstEffect && firstEffect->serial == 1u &&
                       firstEffect->disposition ==
                           EditorPickingEffectDisposition::selection &&
                       firstEffect->selection &&
                       firstEffect->selection->kind ==
                           EditorSelectionEffectKind::setPrimitive &&
                       firstEffect->browserFocus &&
                       !firstEffect->browserFocus->selectTab,
                   "head commits selection and no-tab browser reveal first");
  current.currentSelection = SceneSelectionValue{fixture.keyA()};
  ++current.selectionRevision;
  passed &= expect(!controller.commitReady(current),
                   "serial two still blocks ready serial three");
  passed &= expect(controller.acceptTerminal(cancelled(second)),
                   "cancellation is an accepted terminal");
  const auto cancelEffect = controller.commitReady(current);
  passed &= expect(cancelEffect && cancelEffect->serial == 2u &&
                       cancelEffect->disposition ==
                           EditorPickingEffectDisposition::cancelled &&
                       !cancelEffect->selection && !cancelEffect->camera,
                   "cancellation releases head-of-line without side effects");
  const auto thirdEffect = controller.commitReady(current);
  passed &= expect(thirdEffect && thirdEffect->serial == 3u &&
                       thirdEffect->selection &&
                       thirdEffect->selection->occurrence == fixture.keyB(),
                   "already-ready tail commits only after cancelled head");
  passed &= expect(!controller.acceptTerminal(hit(third, occurrenceB)) &&
                       controller.stats().ignoredDuplicateTerminals == 1u,
                   "late duplicate cannot terminate twice");
  EditorPickResult unknown = hit(third, occurrenceB);
  unknown.serial = 999u;
  passed &= expect(!controller.acceptTerminal(unknown) &&
                       controller.stats().ignoredUnknownTerminals == 1u,
                   "unknown terminal is ignored");

  const EditorPickRequest fourth = request(fixture, 4u);
  const EditorPickRequest fifth = request(fixture, 5u);
  (void)controller.trackAccepted(fourth);
  (void)controller.trackAccepted(fifth);
  (void)controller.acceptTerminal(hit(fifth, occurrenceA));
  (void)controller.acceptTerminal(error(fourth));
  const auto errorEffect = controller.commitReady(current);
  const auto afterError = controller.commitReady(current);
  passed &= expect(errorEffect && errorEffect->serial == 4u &&
                       errorEffect->disposition ==
                           EditorPickingEffectDisposition::error &&
                       afterError && afterError->serial == 5u,
                   "error also releases a ready successor");
  return passed;
}

bool testFenceMatrixAndCurrentHitPartitions() {
  Fixture fixture;
  const RenderOccurrenceId visibleId{200u, 5u};
  FakeOccurrenceLookup lookup;
  lookup.records = {{visibleId, fixture.keyA(), true}};
  const EditorPickingCommitContext baseline = context(fixture, lookup);
  const EditorPickRequest baselineRequest = request(fixture, 10u);
  std::array<EditorPickingCommitContext, 8> changed{
      baseline, baseline, baseline, baseline,
      baseline, baseline, baseline, baseline};
  changed[0].currentFence.scene.authority.value += 1u;
  changed[1].currentFence.scene.documentRevision += 1u;
  changed[2].currentFence.scene.projectionRevision += 1u;
  changed[3].currentFence.scene.scene = fixture.otherScene;
  changed[4].currentFence.scene.sceneEpoch += 1u;
  changed[5].currentFence.scene.deviceGeneration += 1u;
  changed[6].currentFence.viewportGeneration += 1u;
  changed[7].currentFence.viewRevision += 1u;
  bool passed = true;
  for (std::size_t index = 0; index < changed.size(); ++index) {
    EditorPickingController controller;
    EditorPickRequest item = baselineRequest;
    item.serial += index;
    (void)controller.trackAccepted(item);
    (void)controller.acceptTerminal(hit(item, visibleId));
    const auto effect = controller.commitReady(changed[index]);
    passed &= expect(effect && effect->disposition ==
                                   EditorPickingEffectDisposition::stale &&
                         !effect->selection && !effect->camera,
                     "each full-fence mismatch is stale and side-effect free");
  }
  EditorPickingCommitContext staleGeneration = baseline;
  ++staleGeneration.occurrenceGeneration;
  EditorPickingController generationController;
  (void)generationController.trackAccepted(baselineRequest);
  (void)generationController.acceptTerminal(hit(baselineRequest, visibleId));
  const auto generationEffect =
      generationController.commitReady(staleGeneration);
  passed &= expect(generationEffect && generationEffect->disposition ==
                                           EditorPickingEffectDisposition::stale,
                   "occurrence generation replacement is stale, not blank");

  const auto singleEffect = [&](EditorPickResult terminal,
                                EditorPickingCommitContext current) {
    EditorPickingController controller;
    (void)controller.trackAccepted(baselineRequest);
    (void)controller.acceptTerminal(terminal);
    return controller.commitReady(current);
  };
  const auto selected = singleEffect(hit(baselineRequest, visibleId), baseline);
  passed &= expect(selected && selected->selection &&
                       selected->selection->kind ==
                           EditorSelectionEffectKind::setPrimitive &&
                       selected->browserFocus &&
                       selected->browserFocus->revealParents &&
                       selected->browserFocus->scrollToTarget &&
                       !selected->browserFocus->selectTab,
                   "valid single selects exact occurrence and reveals without tab switch");
  EditorPickingCommitContext same = baseline;
  same.currentSelection = SceneSelectionValue{fixture.keyA()};
  same.soleHighlightedOccurrenceMatches = false;
  const auto toggled = singleEffect(hit(baselineRequest, visibleId), same);
  passed &= expect(toggled && toggled->selection &&
                       toggled->selection->kind ==
                           EditorSelectionEffectKind::togglePrimitive &&
                       toggled->selection->soleHighlightedOccurrenceMatches &&
                       !toggled->browserFocus,
                   "same Primitive emits clear-capable toggle independent of sole-highlight flag");
  const auto blank = singleEffect(miss(baselineRequest), baseline);
  passed &= expect(blank && blank->disposition ==
                                   EditorPickingEffectDisposition::blank &&
                       blank->selection && blank->selection->kind ==
                                               EditorSelectionEffectKind::clear,
                   "current blank single emits clear effect");
  FakeOccurrenceLookup hiddenLookup;
  hiddenLookup.records = {{visibleId, fixture.keyA(), false}};
  EditorPickingCommitContext hidden = context(fixture, hiddenLookup);
  const auto hiddenEffect =
      singleEffect(hit(baselineRequest, visibleId), hidden);
  passed &= expect(hiddenEffect && hiddenEffect->selection &&
                       hiddenEffect->selection->kind ==
                           EditorSelectionEffectKind::clear,
                   "current hidden occurrence is a blank single");
  const auto unknownEffect = singleEffect(
      hit(baselineRequest, RenderOccurrenceId{999u, 1u}), baseline);
  passed &= expect(unknownEffect && unknownEffect->selection &&
                       unknownEffect->selection->kind ==
                           EditorSelectionEffectKind::clear,
                   "current unknown occurrence is a blank single");
  return passed;
}

bool testDoubleCameraAndInvalidNoOp() {
  Fixture fixture;
  const RenderOccurrenceId occurrence{300u, 6u};
  FakeOccurrenceLookup lookup;
  lookup.records = {{occurrence, fixture.keyA(), true}};
  EditorPickingCommitContext current = context(fixture, lookup);
  const EditorPickRequest doubleRequest =
      request(fixture, 20u, PickIntent::doubleFocus);
  const auto commitOne = [&](EditorPickResult terminal) {
    EditorPickingController controller;
    (void)controller.trackAccepted(doubleRequest);
    (void)controller.acceptTerminal(terminal);
    return controller.commitReady(current);
  };
  const auto focused = commitOne(hit(doubleRequest, occurrence));
  bool passed = expect(
      focused && focused->disposition == EditorPickingEffectDisposition::camera &&
          focused->camera && focused->camera->valid() &&
          focused->camera->center == EditorWorldPoint{1.0, 2.0, -2.0} &&
          focused->camera->navigationSpeed == 5.0 &&
          focused->camera->keepEye &&
          focused->camera->nextViewRevision ==
              focused->camera->expectedViewRevision + 1u &&
          !focused->selection && !focused->browserFocus,
      "modeled double hit keeps eye, centers [1,2,-2], sets speed 5 once");
  for (EditorPickResult terminal :
       {miss(doubleRequest), error(doubleRequest), cancelled(doubleRequest)}) {
    const auto effect = commitOne(std::move(terminal));
    passed &= expect(effect && !effect->selection && !effect->camera &&
                         !effect->browserFocus,
                     "double miss/error/cancelled changes neither camera nor selection");
  }
  FakeOccurrenceLookup hiddenLookup;
  hiddenLookup.records = {{occurrence, fixture.keyA(), false}};
  current.occurrences = &hiddenLookup;
  const auto hidden = commitOne(hit(doubleRequest, occurrence));
  passed &= expect(hidden && hidden->disposition ==
                                 EditorPickingEffectDisposition::blank &&
                       !hidden->selection && !hidden->camera,
                   "double hidden hit is a no-op");

  EditorPickingController resetController;
  const EditorPickRequest pending = request(fixture, 30u);
  (void)resetController.trackAccepted(pending);
  resetController.resetPending();
  passed &= expect(resetController.pendingCount() == 0u &&
                       resetController.stats().resetPendingRequests == 1u &&
                       !resetController.acceptTerminal(miss(pending)),
                   "document/scene/viewport destruction reset rejects late terminal");
  return passed;
}

} // namespace

int main() {
  bool passed = true;
  passed &= testStrictClassifierAndEmissionTimeSnapshot();
  passed &= testDpiBoundariesAndRequestConstruction();
  passed &= testSerialOrderingAndHeadOfLineRelease();
  passed &= testFenceMatrixAndCurrentHitPartitions();
  passed &= testDoubleCameraAndInvalidNoOp();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
