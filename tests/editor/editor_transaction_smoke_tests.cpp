#include "../../app/editor/EditorTransactionSmoke.h"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

using Smoke = demo::editor::automation::EditorTransactionSmoke;

Smoke::Digest digest(std::uint8_t seed) {
  Smoke::Digest value{};
  value.fill(seed);
  return value;
}

Smoke::Snapshot snapshot(std::uint64_t revision, std::uint8_t seed,
                         std::uint64_t mapping, std::size_t undo,
                         std::size_t redo, std::uint64_t cleanRevision) {
  return Smoke::Snapshot{
      Smoke::ProjectionStamp{revision, digest(seed), digest(seed + 1u),
                             digest(seed + 2u), mapping},
      cleanRevision, undo, redo};
}

Smoke::DispatchEvidence dispatch() {
  return Smoke::DispatchEvidence{true, true, true, 1u};
}

bool advanceThroughMutation(Smoke& smoke, const Smoke::Snapshot& baseline,
                            const Smoke::Snapshot& mutated) {
  return smoke.observeSceneReady(true, true, true) &&
         smoke.submitMutation(baseline, mutated, dispatch(), 0u) &&
         smoke.observeMutationAccepted(mutated.projection, 1u);
}

bool advanceThroughUndo(Smoke& smoke, const Smoke::Snapshot& baseline,
                        const Smoke::Snapshot& mutated,
                        const Smoke::Snapshot& undone) {
  return advanceThroughMutation(smoke, baseline, mutated) &&
         smoke.submitUndo(undone, dispatch(), 1u) &&
         smoke.observeUndoAccepted(undone, undone.projection, true, 2u);
}

bool advanceThroughRedo(Smoke& smoke, const Smoke::Snapshot& baseline,
                        const Smoke::Snapshot& mutated,
                        const Smoke::Snapshot& undone,
                        const Smoke::Snapshot& redone) {
  return advanceThroughUndo(smoke, baseline, mutated, undone) &&
         smoke.submitRedo(redone, dispatch(), 2u) &&
         smoke.observeRedoAccepted(redone, redone.projection, true, 3u);
}

bool expect(bool condition, std::string_view message) {
  if(!condition)
    std::cerr << "FAILED: " << message << '\n';
  return condition;
}

bool testSuccessfulSchedule() {
  Smoke smoke{true};
  bool passed = true;
  passed &= expect(smoke.observeSceneReady(true, true, true),
                   "strict scene-ready arms the transaction");

  const Smoke::Snapshot baseline = snapshot(10u, 1u, 20u, 2u, 0u, 10u);
  const Smoke::Snapshot mutated = snapshot(11u, 3u, 21u, 3u, 0u, 10u);
  passed &= expect(smoke.submitMutation(baseline, mutated, dispatch(), 0u),
                   "frame 0 rename dispatch is accepted");
  passed &= expect(smoke.observeRenderedFrame(0u),
                   "counted frame 0 is accepted");
  passed &= expect(smoke.observeMutationAccepted(mutated.projection, 1u),
                   "frame 1 renderer rename acknowledgement is exact");
  passed &= expect(smoke.completeWatchdogStage(
                       Smoke::WatchdogStage::renamePublication,
                       std::chrono::seconds(35), 1u),
                   "rename publication closes one bounded watchdog segment");

  const Smoke::Snapshot undone = snapshot(12u, 1u, 22u, 2u, 1u, 10u);
  passed &= expect(smoke.submitUndo(undone, dispatch(), 1u),
                   "frame 1 undo dispatch restores semantic digest");
  passed &= expect(smoke.observeRenderedFrame(1u),
                   "counted frame 1 is accepted");
  passed &= expect(smoke.observeUndoAccepted(
                       undone, undone.projection, true, 2u),
                   "frame 2 renderer undo acknowledgement is exact");
  passed &= expect(smoke.completeWatchdogStage(
                       Smoke::WatchdogStage::undoPublication,
                       std::chrono::seconds(30), 2u),
                   "undo publication closes one bounded watchdog segment");

  const Smoke::Snapshot redone = snapshot(13u, 3u, 23u, 3u, 0u, 10u);
  passed &= expect(smoke.submitRedo(redone, dispatch(), 2u),
                   "frame 2 redo dispatch reapplies semantic digest");
  passed &= expect(smoke.observeRenderedFrame(2u),
                   "counted frame 2 is accepted");
  passed &= expect(smoke.observeRedoAccepted(
                       redone, redone.projection, true, 3u),
                   "frame 3 renderer redo acknowledgement is exact");
  passed &= expect(smoke.completeWatchdogStage(
                       Smoke::WatchdogStage::redoPublication,
                       std::chrono::seconds(30), 3u),
                   "redo publication closes one bounded watchdog segment");

  Smoke::SaveEvidence save;
  save.dispatchAccepted = true;
  save.targetAbsentAtStart = true;
  save.targetRegularFile = true;
  save.targetBytes = 128u;
  save.reloadSucceeded = true;
  save.semanticEquivalent = true;
  save.reloadedMutationPresent = true;
  save.liveSourceIsTarget = true;
  save.liveDocumentClean = true;
  save.live = redone;
  save.live.cleanRevision = redone.projection.documentRevision;
  passed &= expect(smoke.verifySave(save, 3u),
                   "frame 3 Save As evidence is complete");
  passed &= expect(smoke.completeWatchdogStage(
                       Smoke::WatchdogStage::saveVerification,
                       std::chrono::seconds(50), 3u),
                   "verified Save As closes one bounded watchdog segment");
  passed &= expect(smoke.observeRenderedFrame(3u),
                   "counted frame 3 is accepted");
  for(std::uint64_t frame = 4u; frame < 40u; ++frame)
    passed &= expect(smoke.observeRenderedFrame(frame),
                     "counted frame remains within the exact 40-frame budget");

  const auto markers = smoke.markers();
  static constexpr std::string_view expected[] = {
      "scene-ready", "automation-start", "undo-observed", "redo-observed",
      "save-complete", "frame-budget-complete", "automation-complete"};
  passed &= expect(markers.size() == std::size(expected),
                   "successful schedule emits exactly seven markers");
  for(std::size_t index = 0u; index < std::size(expected) &&
                               index < markers.size(); ++index)
    passed &= expect(Smoke::markerName(markers[index]) == expected[index],
                     "authoritative markers retain their literal order");
  passed &= expect(smoke.phase() == Smoke::Phase::complete,
                   "successful schedule reaches complete phase");
  const auto stageTimings = smoke.completedStageTimings();
  std::chrono::milliseconds totalStageTime{0};
  for(const auto elapsed : stageTimings)
    totalStageTime += elapsed;
  passed &= expect(stageTimings.size() == 4u &&
                       totalStageTime > std::chrono::seconds(60) &&
                       stageTimings[3] == std::chrono::seconds(50),
                   "segmented timings preserve each stage and allow no hidden reset");
  passed &= expect(!smoke.observeRenderedFrame(40u),
                   "frame 40 is rejected after completion");
  passed &= expect(smoke.markers().size() == std::size(expected),
                   "frame 40 cannot duplicate a success marker");
  return passed;
}

bool testRejectedEvidence() {
  bool passed = true;
  {
    Smoke smoke{true};
    passed &= expect(smoke.observeSceneReady(true, true, true),
                     "negative case arms");
    const auto baseline = snapshot(1u, 1u, 1u, 0u, 0u, 1u);
    auto mutated = snapshot(2u, 1u, 2u, 1u, 0u, 1u);
    passed &= expect(!smoke.submitMutation(baseline, mutated,
                                           Smoke::DispatchEvidence{}, 0u),
                     "rejected dispatch cannot emit automation-start");
    passed &= expect(smoke.markers().size() == 1u,
                     "rejected dispatch leaves only scene-ready");
  }
  {
    Smoke smoke{true};
    passed &= expect(smoke.observeSceneReady(true, true, true),
                     "projection mismatch case arms");
    const auto baseline = snapshot(1u, 1u, 1u, 0u, 0u, 1u);
    const auto mutated = snapshot(2u, 2u, 2u, 1u, 0u, 1u);
    passed &= expect(smoke.submitMutation(baseline, mutated, dispatch(), 0u),
                     "projection mismatch case submits mutation");
    passed &= expect(!smoke.observeMutationAccepted(
                         snapshot(2u, 9u, 2u, 1u, 0u, 1u).projection, 1u),
                     "projection digest mismatch fails closed");
    passed &= expect(smoke.markers().size() == 2u,
                     "projection mismatch emits no undo marker");
  }
  {
    Smoke smoke{true};
    passed &= expect(smoke.observeSceneReady(true, true, true),
                     "deadline case arms");
    passed &= expect(!smoke.checkDeadline(std::chrono::seconds(61), 0u),
                     "transaction timeout fails closed");
    passed &= expect(smoke.markers().size() == 1u,
                     "timeout emits no transaction success markers");
  }
  {
    Smoke smoke{true};
    passed &= expect(smoke.observeSceneReady(true, true, true),
                     "premature watchdog boundary case arms");
    passed &= expect(!smoke.completeWatchdogStage(
                         Smoke::WatchdogStage::renamePublication,
                         std::chrono::seconds(1), 0u),
                     "watchdog cannot reset before renderer acceptance");
    passed &= expect(smoke.markers().size() == 1u,
                     "premature watchdog reset emits no success marker");
  }
  {
    Smoke smoke{true};
    const auto baseline = snapshot(1u, 1u, 1u, 0u, 0u, 1u);
    const auto mutated = snapshot(2u, 2u, 2u, 1u, 0u, 1u);
    const auto undone = snapshot(3u, 1u, 3u, 0u, 1u, 1u);
    const auto redone = snapshot(4u, 2u, 4u, 1u, 0u, 1u);
    passed &= expect(smoke.observeSceneReady(true, true, true) &&
                         smoke.submitMutation(baseline, mutated, dispatch(), 0u) &&
                         smoke.observeRenderedFrame(0u) &&
                         smoke.observeMutationAccepted(mutated.projection, 1u) &&
                         smoke.submitUndo(undone, dispatch(), 1u) &&
                         smoke.observeRenderedFrame(1u) &&
                         smoke.observeUndoAccepted(
                             undone, undone.projection, true, 2u) &&
                         smoke.submitRedo(redone, dispatch(), 2u) &&
                         smoke.observeRenderedFrame(2u) &&
                         smoke.observeRedoAccepted(
                             redone, redone.projection, true, 3u),
                     "early frame-39 case reaches redo observation");
    for(std::uint64_t frame = 3u; frame < 39u; ++frame)
      passed &= expect(smoke.observeRenderedFrame(frame),
                       "pre-39 counted frames remain sequential");
    passed &= expect(!smoke.observeRenderedFrame(39u),
                     "sequential frame 39 before Save As is rejected");
    passed &= expect(smoke.markers().size() == 4u,
                     "early frame 39 emits no persistence/budget markers");
  }
  {
    Smoke smoke{true};
    const auto baseline = snapshot(1u, 1u, 1u, 0u, 0u, 1u);
    const auto mutated = snapshot(2u, 2u, 2u, 1u, 0u, 1u);
    const auto undone = snapshot(3u, 1u, 3u, 0u, 1u, 1u);
    const auto redone = snapshot(4u, 2u, 4u, 1u, 0u, 1u);
    passed &= expect(
        advanceThroughRedo(smoke, baseline, mutated, undone, redone),
        "invalid Save As case reaches redo observation");
    Smoke::SaveEvidence invalidSave;
    invalidSave.dispatchAccepted = true;
    invalidSave.targetAbsentAtStart = true;
    invalidSave.targetRegularFile = true;
    invalidSave.targetBytes = 128u;
    invalidSave.live = redone;
    invalidSave.live.cleanRevision = redone.projection.documentRevision;
    passed &= expect(!smoke.verifySave(invalidSave, 3u),
                     "incomplete reload/semantic evidence rejects Save As");
    passed &= expect(smoke.markers().size() == 4u,
                     "invalid Save As emits no save-complete marker");
  }
  return passed;
}

bool testSkippedRevisionAndMappingSteps() {
  bool passed = true;
  const auto baseline = snapshot(1u, 1u, 10u, 0u, 0u, 1u);
  const auto mutated = snapshot(2u, 2u, 11u, 1u, 0u, 1u);
  const auto undone = snapshot(3u, 1u, 12u, 0u, 1u, 1u);

  {
    Smoke smoke{true};
    passed &= expect(smoke.observeSceneReady(true, true, true),
                     "skipped rename revision case arms");
    const auto skippedRevision = snapshot(3u, 2u, 11u, 1u, 0u, 1u);
    passed &= expect(!smoke.submitMutation(baseline, skippedRevision,
                                           dispatch(), 0u),
                     "rename rejects a skipped document revision");
  }
  {
    Smoke smoke{true};
    passed &= expect(smoke.observeSceneReady(true, true, true),
                     "skipped rename epoch case arms");
    const auto skippedMapping = snapshot(2u, 2u, 12u, 1u, 0u, 1u);
    passed &= expect(!smoke.submitMutation(baseline, skippedMapping,
                                           dispatch(), 0u),
                     "rename rejects a skipped mapping epoch");
  }
  {
    Smoke smoke{true};
    passed &= expect(advanceThroughMutation(smoke, baseline, mutated),
                     "undo revision negative reaches mutation acceptance");
    const auto skippedRevision = snapshot(4u, 1u, 12u, 0u, 1u, 1u);
    passed &= expect(!smoke.submitUndo(skippedRevision, dispatch(), 1u),
                     "undo rejects a skipped document revision");
  }
  {
    Smoke smoke{true};
    passed &= expect(advanceThroughMutation(smoke, baseline, mutated),
                     "undo epoch negative reaches mutation acceptance");
    const auto skippedMapping = snapshot(3u, 1u, 13u, 0u, 1u, 1u);
    passed &= expect(!smoke.submitUndo(skippedMapping, dispatch(), 1u),
                     "undo rejects a skipped mapping epoch");
  }
  {
    Smoke smoke{true};
    passed &= expect(advanceThroughUndo(smoke, baseline, mutated, undone),
                     "redo revision negative reaches undo observation");
    const auto skippedRevision = snapshot(5u, 2u, 13u, 1u, 0u, 1u);
    passed &= expect(!smoke.submitRedo(skippedRevision, dispatch(), 2u),
                     "redo rejects a skipped document revision");
  }
  {
    Smoke smoke{true};
    passed &= expect(advanceThroughUndo(smoke, baseline, mutated, undone),
                     "redo epoch negative reaches undo observation");
    const auto skippedMapping = snapshot(4u, 2u, 14u, 1u, 0u, 1u);
    passed &= expect(!smoke.submitRedo(skippedMapping, dispatch(), 2u),
                     "redo rejects a skipped mapping epoch");
  }
  return passed;
}

} // namespace

int main() {
  const bool passed = testSuccessfulSchedule() && testRejectedEvidence() &&
                      testSkippedRevisionAndMappingSteps();
  if(passed)
    std::cout << "Editor transaction smoke contract tests passed.\n";
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
