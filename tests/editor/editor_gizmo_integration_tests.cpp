#include "app/editor/EditorInputRouter.h"
#include "app/editor/EditorNativeGizmoInput.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {

using namespace demo;
using namespace demo::editor;
using namespace demo::editor::ui;

void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

EditorViewportMetrics viewport() {
  EditorViewportMetrics metrics;
  metrics.logical = {{0.0, 0.0}, {100.0, 100.0}};
  metrics.framebufferLogicalOrigin = {0.0, 0.0};
  metrics.physical = {{0u, 0u}, {100u, 100u}};
  metrics.depthPhysical = metrics.physical;
  metrics.colorAttachment = {100u, 100u};
  metrics.depthAttachment = {100u, 100u};
  metrics.framebufferScale = {1.0, 1.0};
  metrics.viewportGeneration = 7u;
  metrics.projection = {8u, 9u};
  metrics.view = {10u, 11u};
  metrics.convention = {EditorClipDepthRange::zeroToOne, true, true};
  metrics.projectionKind = EditorProjectionKind::perspective;
  metrics.verticalFieldOfViewRadians = 1.0;
  require(metrics.valid(), "integration viewport is invalid");
  return metrics;
}

OverlayMatrix4 identity() { return {}; }

NodeId node() {
  return *NodeId::from(EditorObjectId{
      EditorObjectKind::node, 3u, 1u, EditorDocumentAuthority{77u}});
}

DesktopGizmoFrameInput frameInput() {
  const EditorViewportMetrics metrics = viewport();
  const NodeId selected = node();
  GizmoAttachment attachment;
  attachment.nodeId = stableGizmoNodeId(selected);
  attachment.documentRevision = 12u;
  attachment.selectionRevision = 13u;
  DesktopGizmoFrameInput input;
  input.frameSerial = 1u;
  input.documentRevision = 12u;
  input.selectionRevision = 13u;
  input.mappingEpoch = 14u;
  input.viewport = metrics;
  input.viewProjection = identity();
  input.camera.generation = {metrics.viewportGeneration, metrics.projection,
                              metrics.view};
  input.camera.inverseViewProjection = identity();
  input.viewDepth.generation = input.camera.generation;
  input.viewDepth.positiveViewDepth = 1.0;
  input.gridSettings.enabled = true;
  input.gridSettings.baseUnit = 1.0;
  input.gridInput.cameraPosition = {0.0, 5.0, 5.0};
  input.gridInput.cameraForward = {0.0, -0.7, -0.7};
  input.gizmoSettings.visible = true;
  input.gizmoSettings.visibility = GizmoVisibility::all;
  input.viewDirectionWorld = {0.0, 0.0, -1.0};
  input.selectedNode = selected;
  input.attachment = attachment;
  return input;
}

EditorInputEvent event(std::uint64_t sequence, std::uint64_t frame,
                       EditorInputEventPayload payload) {
  return {{sequence}, {frame}, std::move(payload)};
}

void testNativeTerminalQueueOrderAndFiltering() {
  using Queue = NativeGizmoTerminalQueue;
  for (const bool escapeFirst : {true, false}) {
    Queue queue;
    queue.observePrimaryButton(true, false);
    queue.observeEscape(true, false);
    queue.observeEscape(false, false);
    queue.observePrimaryButton(false, false);
    require(queue.callbackSequence() == 4u && queue.pendingCount() == 0u,
            "inactive callbacks were enqueued or lost their native ordinals");
    if (escapeFirst) {
      queue.observeEscape(true, true);
      queue.observePrimaryButton(false, true);
    } else {
      queue.observePrimaryButton(false, true);
      queue.observeEscape(true, true);
    }
    queue.observeEscape(false, true);
    require(queue.pendingCount() == 2u && queue.callbackSequence() == 7u,
            "key-up was queued as a terminal or did not advance callback ordinal");
    const Queue::Batch batch = queue.take();
    require(batch.count == 2u && !batch.overflowed && !batch.focusInterrupted &&
                batch.events[0] == Queue::Event{
                    escapeFirst ? Queue::Kind::escapePressed
                                : Queue::Kind::primaryReleased, 5u} &&
                batch.events[1] == Queue::Event{
                    escapeFirst ? Queue::Kind::primaryReleased
                                : Queue::Kind::escapePressed, 6u},
            "native terminal FIFO reordered actual callback arrival");
    require(queue.take() == Queue::Batch{} && queue.pendingCount() == 0u &&
                queue.callbackSequence() == 7u,
            "taking native terminals reset ordinals or replayed prior events");
    queue.observePrimaryButton(true, true);
    require(queue.pendingCount() == 0u && queue.callbackSequence() == 8u,
            "primary press became a terminal or failed to advance its ordinal");
  }
}

void testNativeTerminalQueueFocusInterruption() {
  NativeGizmoTerminalQueue queue;
  queue.observePrimaryButton(true, false);
  queue.observeFocus(false, true);
  queue.observePrimaryButton(false, true);
  queue.observeFocus(true);
  require(queue.focusInterrupted(),
          "active focus loss did not expose the interruption fence");
  const auto interrupted = queue.take();
  require(interrupted.focusInterrupted && !interrupted.overflowed &&
              interrupted.count == 2u &&
              interrupted.events[0].kind ==
                  NativeGizmoTerminalQueue::Kind::focusLost &&
              interrupted.events[0].callbackSequence == 2u &&
              interrupted.events[1].kind ==
                  NativeGizmoTerminalQueue::Kind::primaryReleased &&
              interrupted.events[1].callbackSequence == 3u &&
              queue.callbackSequence() == 4u,
          "focus loss did not retain its trailing release callback order");
  require(!queue.focusInterrupted(),
          "taking a native batch leaked its interruption fence");
  queue.observeEscape(true, true);
  const auto focused = queue.take();
  require(!focused.focusInterrupted && !focused.overflowed &&
              focused.count == 1u && focused.events[0].callbackSequence == 5u,
          "focus interruption leaked beyond the batch without preserving ordinal");

  // A release callback can precede the focus callback in the same native
  // queue.  Preserve that raw order; the host uses the batch fence to mark
  // the release suppressed.
  NativeGizmoTerminalQueue releaseFirst;
  releaseFirst.observePrimaryButton(false, true);
  releaseFirst.observeFocus(false, true);
  const auto releaseFirstBatch = releaseFirst.take();
  require(releaseFirstBatch.focusInterrupted &&
              releaseFirstBatch.count == 2u &&
              releaseFirstBatch.events[0] == NativeGizmoTerminalQueue::Event{
                  NativeGizmoTerminalQueue::Kind::primaryReleased, 1u} &&
              releaseFirstBatch.events[1] == NativeGizmoTerminalQueue::Event{
                  NativeGizmoTerminalQueue::Kind::focusLost, 2u},
          "release-before-focus callback order was reconstructed incorrectly");

  NativeGizmoTerminalQueue discarded;
  discarded.observeFocus(false, true);
  discarded.observePrimaryButton(false, true);
  const std::uint64_t clockBeforeDiscard = discarded.callbackSequence();
  discarded.discardPending();
  require(discarded.pendingCount() == 0u && !discarded.focusInterrupted() &&
              discarded.callbackSequence() == clockBeforeDiscard,
          "lifetime reset did not discard stale callbacks without rewinding the clock");
}

void testNativeTerminalQueueOverflowAndExhaustion() {
  using Queue = NativeGizmoTerminalQueue;
  Queue queue;
  for (std::size_t index = 0u; index <= Queue::kCapacity; ++index) {
    queue.observeEscape(true, true);
    queue.observeEscape(false, true);
  }
  const Queue::Batch overflow = queue.take();
  require(overflow.overflowed && overflow.count == Queue::kCapacity &&
              queue.callbackSequence() == 2u * (Queue::kCapacity + 1u),
          "bounded callback FIFO overflow silently claimed complete coverage");
  for (std::size_t index = 0u; index < overflow.count; ++index)
    require(overflow.events[index].callbackSequence == index * 2u + 1u,
            "overflow changed the retained native callback prefix order");
  queue.observeEscape(true, true);
  const Queue::Batch recovered = queue.take();
  require(!recovered.overflowed && recovered.count == 1u &&
              recovered.events[0].callbackSequence == 67u,
          "batch reset discarded callback lifetime or retained old overflow flags");

  const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
  Queue exhausted(maximum - 1u);
  exhausted.observeEscape(true, true);
  exhausted.observePrimaryButton(false, true);
  const Queue::Batch last = exhausted.take();
  require(last.overflowed && last.count == 1u &&
              last.events[0].callbackSequence == maximum &&
              exhausted.callbackSequence() == maximum,
          "callback ordinal exhaustion wrapped or duplicated an event identity");
  exhausted.observeEscape(false, false);
  const Queue::Batch stillExhausted = exhausted.take();
  require(stillExhausted.overflowed && stillExhausted.count == 0u &&
              exhausted.callbackSequence() == maximum,
          "inactive callback hid ordinal exhaustion after taking the failed batch");
}

void testSnapshotAndFence() {
  std::uint32_t begins = 0u;
  std::uint32_t previews = 0u;
  std::uint32_t finishes = 0u;
  DesktopGizmoCallbacks callbacks;
  callbacks.beginTransform = [&](NodeId) {
    ++begins;
    return true;
  };
  callbacks.previewTransform = [&](NodeId, const GizmoLocalTransform &) {
    ++previews;
    return true;
  };
  callbacks.finishTransform = [&](NodeId, DesktopGizmoTerminal) {
    ++finishes;
    return true;
  };
  callbacks.clearSelection = [] {};
  DesktopGizmoIntegrationAdapter adapter(std::move(callbacks));
  adapter.beginFrame(frameInput());

  require(!adapter.route(event(
                 1u, 99u, EditorPointerMoveEvent{{50.0, 50.0}})),
          "stale adapter event was consumed");
  require(adapter.route(event(
                 1u, 1u, EditorPointerMoveEvent{{50.0, 50.0}})) ||
              !adapter.pointerCaptured(),
          "adapter route returned an incoherent state");
  adapter.closeFrame();
  require(adapter.snapshot().has_value(), "adapter did not publish a snapshot");
  require(adapter.snapshot()->frameSerial == 1u &&
              adapter.snapshot()->documentRevision == 12u &&
              adapter.snapshot()->selectionRevision == 13u &&
              adapter.snapshot()->mappingEpoch == 14u,
          "snapshot fence fields differ");
  require(adapter.snapshot()->grid.enabled &&
              adapter.snapshot()->grid.generatedLineCount ==
                  adapter.snapshot()->gridLines.size() +
                      adapter.snapshot()->gridAxes.size(),
          "snapshot did not contain the P7 Grid packets");
  require(adapter.snapshot()->gizmoVisible &&
              std::any_of(adapter.snapshot()->gizmo.components.begin(),
                          adapter.snapshot()->gizmo.components.end(),
                          [](const auto &draw) { return draw.visible; }),
          "snapshot did not contain the P7 Gizmo packet");
  require(begins == 0u && previews == 0u && finishes == 0u,
          "hover-only frame mutated the transform transaction");
}

void testTerminalBatchIsIdempotent() {
  std::uint32_t finishes = 0u;
  DesktopGizmoCallbacks callbacks;
  callbacks.beginTransform = [](NodeId) { return true; };
  callbacks.previewTransform = [](NodeId, const GizmoLocalTransform &) {
    return true;
  };
  callbacks.finishTransform = [&](NodeId, DesktopGizmoTerminal) {
    ++finishes;
    return true;
  };
  callbacks.clearSelection = [] {};
  DesktopGizmoIntegrationAdapter adapter(std::move(callbacks));
  DesktopGizmoFrameInput input = frameInput();
  input.frameSerial = 2u;
  adapter.beginFrame(std::move(input));
  // A release without a consumed press is a no-op, and must not manufacture a
  // history terminal or capture state.
  require(!adapter.route(event(
                 1u, 2u,
                 EditorPointerButtonEvent{{200.0, 200.0},
                                           EditorPointerButton::primary,
                                           EditorInputAction::release, false,
                                           true})),
          "outside release was incorrectly consumed");
  adapter.closeFrame();
  require(finishes == 0u && !adapter.pointerCaptured(),
          "outside release changed adapter terminal state");

  input = frameInput();
  input.frameSerial = 3u;
  adapter.beginFrame(std::move(input));
  require(!adapter.route(event(
                 1u, 3u,
                 EditorPointerButtonEvent{{200.0, 200.0},
                                           EditorPointerButton::primary,
                                           EditorInputAction::press, false,
                                           true})),
          "outside press was incorrectly consumed");
  adapter.closeFrame();
  require(finishes == 0u, "outside press created a terminal callback");
}

void testRevisionZeroAttachment() {
  DesktopGizmoFrameInput input = frameInput();
  input.documentRevision = 0u;
  input.attachment->documentRevision = 0u;
  DesktopGizmoIntegrationAdapter adapter;
  adapter.beginFrame(std::move(input));
  adapter.closeFrame();
  require(adapter.snapshot().has_value() && adapter.snapshot()->gizmoVisible,
          "revision-zero clean document still publishes a Gizmo packet");
}

void testUiOwnershipAndStableIdentity() {
  DesktopGizmoIntegrationAdapter adapter;
  DesktopGizmoFrameInput input = frameInput();
  input.modalActive = true;
  adapter.beginFrame(input);
  require(!adapter.route(event(
                 1u, 1u, EditorPointerMoveEvent{{50.0, 50.0}})),
          "modal UI did not retain pointer ownership");
  adapter.closeFrame();

  const NodeId first = node();
  const auto second = *NodeId::from(EditorObjectId{
      EditorObjectKind::node, 3u, 1u, EditorDocumentAuthority{77u + (1ull << 32u)}});
  require(stableGizmoNodeId(first) != stableGizmoNodeId(second),
          "Gizmo identity discarded low authority bits");
}

void testSnapAndDragBaseline() {
  std::vector<GizmoLocalTransform> previews;
  DesktopGizmoCallbacks callbacks;
  callbacks.beginTransform = [](NodeId) { return true; };
  callbacks.previewTransform = [&](NodeId, const GizmoLocalTransform &value) {
    previews.push_back(value);
    return true;
  };
  callbacks.finishTransform = [](NodeId, DesktopGizmoTerminal) { return true; };
  DesktopGizmoIntegrationAdapter adapter(std::move(callbacks));
  DesktopGizmoFrameInput input = frameInput();
  input.gizmoSettings.snapEnabled = false;
  input.gizmoSettings.hovered = GizmoComponent::translateX;
  adapter.beginFrame(input);

  // The identity inverse-view-projection fixture gives a deterministic center
  // ray; use a visible edge point for a translation-axis press and repeat the
  // same move twice. The second preview must equal the first (no accumulation).
  const auto press = event(
      1u, 1u, EditorPointerButtonEvent{{50.0, 50.0},
                                        EditorPointerButton::primary,
                                        EditorInputAction::press, false, true});
  if (adapter.route(press)) {
    (void)adapter.route(event(2u, 1u, EditorPointerMoveEvent{{52.0, 50.0}}));
    (void)adapter.route(event(3u, 1u, EditorPointerMoveEvent{{52.0, 50.0}}));
    require(previews.size() >= 1u, "accepted drag emitted no preview");
    if (previews.size() >= 2u) {
      require(previews[0] == previews[1],
              "drag preview accumulated from the previous frame");
    }
  }
  adapter.closeFrame();
}

void testStaleFenceDetaches() {
  std::uint32_t finishes = 0u;
  DesktopGizmoCallbacks callbacks;
  callbacks.beginTransform = [](NodeId) { return true; };
  callbacks.previewTransform = [](NodeId, const GizmoLocalTransform &) {
    return true;
  };
  callbacks.finishTransform = [&](NodeId, DesktopGizmoTerminal terminal) {
    if (terminal == DesktopGizmoTerminal::detachRetainingPreview) ++finishes;
    return true;
  };
  DesktopGizmoIntegrationAdapter adapter(std::move(callbacks));
  DesktopGizmoFrameInput input = frameInput();
  adapter.beginFrame(input);
  const auto press = event(
      1u, 1u, EditorPointerButtonEvent{{50.0, 50.0},
                                        EditorPointerButton::primary,
                                        EditorInputAction::press, false, true});
  if (adapter.route(press)) {
    input.frameSerial = 2u;
    input.mappingEpoch += 1u;
    adapter.beginFrame(input);
    require(adapter.phase() != GizmoInteractionPhase::dragging &&
                adapter.phase() != GizmoInteractionPhase::draggingLatched &&
                finishes == 1u,
            "stale mapping fence did not detach the active Gizmo");
    require(adapter.interactionObservation().acceptedDetachCount == 1u &&
                adapter.interactionObservation().lastTerminalSequence == 0u,
            "stale fence detach did not retain its non-input observation");
  }
  adapter.closeFrame();
}

bool beginDrag(DesktopGizmoIntegrationAdapter &adapter,
               std::uint64_t frameSerial);

void testAcceptedProjectionRevisionAdvance() {
  std::uint32_t finishes = 0u;
  DesktopGizmoCallbacks callbacks;
  callbacks.beginTransform = [](NodeId) { return true; };
  callbacks.previewTransform = [](NodeId, const GizmoLocalTransform &) {
    return true;
  };
  callbacks.finishTransform = [&](NodeId, DesktopGizmoTerminal) {
    ++finishes;
    return true;
  };
  DesktopGizmoIntegrationAdapter adapter(std::move(callbacks));
  DesktopGizmoFrameInput input = frameInput();
  require(beginDrag(adapter, 1u), "revision-advance press was not consumed");
  (void)adapter.route(event(2u, 1u, EditorPointerMoveEvent{{52.0, 50.0}}));
  adapter.closeFrame();

  input.frameSerial = 2u;
  input.documentRevision = 13u;
  input.mappingEpoch = 15u;
  input.attachment->documentRevision = 13u;
  input.attachment->selectionRevision = input.selectionRevision;
  adapter.beginFrame(input);
  require(adapter.phase() == GizmoInteractionPhase::dragging && finishes == 0u,
          "accepted preview revision advance was treated as stale");
  adapter.reset();
}

struct TerminalLog final {
  std::vector<DesktopGizmoTerminal> terminals;
  std::uint32_t clearCalls{0u};
};

DesktopGizmoCallbacks terminalCallbacks(TerminalLog &log) {
  DesktopGizmoCallbacks callbacks;
  callbacks.beginTransform = [](NodeId) { return true; };
  callbacks.previewTransform = [](NodeId, const GizmoLocalTransform &) {
    return true;
  };
  callbacks.finishTransform = [&](NodeId, DesktopGizmoTerminal terminal) {
    log.terminals.push_back(terminal);
    return true;
  };
  callbacks.clearSelection = [&] { ++log.clearCalls; };
  return callbacks;
}

bool beginDrag(DesktopGizmoIntegrationAdapter &adapter,
               std::uint64_t frameSerial) {
  DesktopGizmoFrameInput input = frameInput();
  input.frameSerial = frameSerial;
  adapter.beginFrame(input);
  return adapter.route(event(
      1u, frameSerial,
      EditorPointerButtonEvent{{50.0, 50.0},
                                EditorPointerButton::primary,
                                EditorInputAction::press, false, true}));
}

void testTerminalSemantics() {
  // Ordinary release commits once even when it is delivered outside the
  // viewport after an inside-viewport press.
  {
    TerminalLog log;
    DesktopGizmoIntegrationAdapter adapter(terminalCallbacks(log));
    require(beginDrag(adapter, 1u), "normal Gizmo press was not consumed");
    require(adapter.route(event(
                2u, 1u, EditorPointerMoveEvent{{200.0, 200.0}})),
            "outside drag move was not consumed");
    require(adapter.route(event(
                3u, 1u,
                EditorPointerButtonEvent{{200.0, 200.0},
                                          EditorPointerButton::primary,
                                          EditorInputAction::release, false,
                                          true})),
            "outside delivered release was not consumed");
    adapter.closeFrame();
    require(log.terminals.size() == 1u &&
                log.terminals[0] == DesktopGizmoTerminal::commit &&
                log.clearCalls == 0u,
            "ordinary outside release did not commit exactly once");
    require(adapter.interactionObservation().acceptedCommitCount == 1u &&
                adapter.interactionObservation().lastTerminalSequence == 3u,
            "ordinary release observation did not bind its delivered event");
  }

  // O05 Escape-only detaches without history; a late release is a no-op.
  {
    TerminalLog log;
    DesktopGizmoIntegrationAdapter adapter(terminalCallbacks(log));
    require(beginDrag(adapter, 1u), "O05 press was not consumed");
    require(adapter.route(event(
                2u, 1u,
                EditorKeyEvent{EditorInputKey::escape,
                               EditorInputAction::press})),
            "O05 Escape was not consumed");
    adapter.closeFrame();
    require(log.terminals.size() == 1u &&
                log.terminals[0] == DesktopGizmoTerminal::detachRetainingPreview &&
                log.clearCalls == 1u && !adapter.pointerCaptured(),
            "O05 did not detach without history");
    require(adapter.interactionObservation().acceptedDetachCount == 1u &&
                adapter.interactionObservation().acceptedCommitCount == 0u &&
                adapter.interactionObservation().lastTerminalSequence == 2u,
            "O05 observation did not report accepted no-history detach");
    const auto escapeObservation = adapter.interactionObservation();
    require(escapeObservation.gestureNode == node() &&
                escapeObservation.escapePressCount == 1u &&
                escapeObservation.deliveredReleaseCount == 0u &&
                escapeObservation.focusLossCount == 0u &&
                escapeObservation.suppressedReleaseCount == 0u &&
                escapeObservation.lastTerminalFrame == 1u &&
                escapeObservation.lastTerminalEventCount == 1u &&
                !escapeObservation.lastTerminalEventsOverflowed &&
                escapeObservation.lastTerminalEvents[0] ==
                    GizmoTerminalEvent{GizmoTerminalEventKind::escapePressed, 2u},
            "O05 journal did not preserve the actual Escape-only input");
    require(!adapter.route(event(
                3u, 1u,
                EditorPointerButtonEvent{{50.0, 50.0},
                                          EditorPointerButton::primary,
                                          EditorInputAction::release, false,
                                          true})),
            "O05 late release was consumed");
    require(log.terminals.size() == 1u, "O05 late release added a terminal");
    adapter.refreshAttachment(12u, 14u, 15u, std::nullopt, std::nullopt);
    DesktopGizmoFrameInput afterDetach = frameInput();
    afterDetach.frameSerial = 2u;
    afterDetach.selectionRevision = 14u;
    afterDetach.mappingEpoch = 15u;
    afterDetach.selectedNode.reset();
    afterDetach.attachment.reset();
    adapter.beginFrame(afterDetach);
    require(!adapter.route(event(
                4u, 2u, EditorKeyEvent{EditorInputKey::escape,
                                       EditorInputAction::release})) &&
                !adapter.route(event(
                    5u, 2u, EditorPointerButtonEvent{
                                {50.0, 50.0}, EditorPointerButton::primary,
                                EditorInputAction::release, false, true})),
            "detached gesture consumed key-up or late mouse-up");
    adapter.closeFrame();
    adapter.closeFrame();
    const auto detachedObservation = adapter.interactionObservation();
    require(!detachedObservation.node &&
                detachedObservation.gestureNode == escapeObservation.gestureNode &&
                detachedObservation.escapePressCount == 1u &&
                detachedObservation.deliveredReleaseCount == 0u &&
                detachedObservation.acceptedCommitCount == 0u &&
                detachedObservation.acceptedDetachCount == 1u &&
                detachedObservation.lastTerminalFrame == 1u &&
                detachedObservation.lastTerminalEventCount == 1u &&
                detachedObservation.lastTerminalEvents ==
                    escapeObservation.lastTerminalEvents,
            "selection clearing or late packets erased the Escape-only proof");
  }

  // O06 is order-independent over the complete input batch.
  for (const bool escapeFirst : {true, false}) {
    TerminalLog log;
    DesktopGizmoIntegrationAdapter adapter(terminalCallbacks(log));
    require(beginDrag(adapter, escapeFirst ? 1u : 2u),
            "O06 press was not consumed");
    const std::uint64_t frameSerial = escapeFirst ? 1u : 2u;
    const auto escape = event(
        escapeFirst ? 2u : 3u, frameSerial,
        EditorKeyEvent{EditorInputKey::escape, EditorInputAction::press});
    const auto release = event(
        escapeFirst ? 3u : 2u, frameSerial,
        EditorPointerButtonEvent{{50.0, 50.0},
                                  EditorPointerButton::primary,
                                  EditorInputAction::release, false, true});
    if (escapeFirst) {
      require(adapter.route(escape), "O06 Escape-first was not consumed");
      require(adapter.route(release), "O06 release was not consumed");
    } else {
      require(adapter.route(release), "O06 release-first was not consumed");
      require(adapter.route(escape), "O06 Escape-second was not consumed");
    }
    adapter.closeFrame();
    require(log.terminals.size() == 1u &&
                log.terminals[0] == DesktopGizmoTerminal::detachRetainingPreview &&
                log.clearCalls == 1u,
            "O06 order did not resolve to one no-history detach");
    require(adapter.interactionObservation().acceptedDetachCount == 1u &&
                adapter.interactionObservation().acceptedCommitCount == 0u &&
                adapter.interactionObservation().lastTerminalSequence ==
                    escape.sequence.value,
            "O06 observation did not bind the winning Escape event");
    const auto observed = adapter.interactionObservation();
    require(observed.gestureNode == node() && observed.escapePressCount == 1u &&
                observed.deliveredReleaseCount == 1u &&
                observed.suppressedReleaseCount == 0u &&
                observed.lastTerminalFrame == frameSerial &&
                observed.lastTerminalEventCount == 2u &&
                !observed.lastTerminalEventsOverflowed &&
                observed.lastTerminalEvents[0] ==
                    GizmoTerminalEvent{
                        escapeFirst ? GizmoTerminalEventKind::escapePressed
                                    : GizmoTerminalEventKind::deliveredRelease,
                        2u} &&
                observed.lastTerminalEvents[1] ==
                    GizmoTerminalEvent{
                        escapeFirst ? GizmoTerminalEventKind::deliveredRelease
                                    : GizmoTerminalEventKind::escapePressed,
                        3u},
            "O06 journal reordered or synthesized the real terminal input batch");
  }

  // O07 focus loss latches the interaction. A suppressed physical up does
  // not commit; a later Escape clears/detaches without history.
  {
    TerminalLog log;
    DesktopGizmoIntegrationAdapter adapter(terminalCallbacks(log));
    require(beginDrag(adapter, 1u), "O07 press was not consumed");
    require(adapter.route(event(
                2u, 1u,
                EditorFocusEvent{EditorFocusAction::lost})),
            "O07 focus loss was not consumed");
    adapter.closeFrame();
    require(adapter.phase() == GizmoInteractionPhase::draggingLatched &&
                adapter.interactionObservation().activeComponent ==
                    GizmoComponent::none &&
                log.terminals.size() == 1u &&
                log.terminals[0] == DesktopGizmoTerminal::latchRetainingPreview,
            "O07 did not latch without history");
    require(adapter.interactionObservation().acceptedLatchCount == 1u &&
                adapter.interactionObservation().lastTerminalSequence == 2u,
            "O07 observation did not bind its focus-loss latch");
    require(adapter.interactionObservation().focusLossCount == 1u &&
                adapter.interactionObservation().lastTerminalEventCount == 1u &&
                adapter.interactionObservation().lastTerminalEvents[0] ==
                    GizmoTerminalEvent{GizmoTerminalEventKind::focusLost, 2u},
            "O07 journal did not preserve its actual focus-loss event");

    DesktopGizmoFrameInput next = frameInput();
    next.frameSerial = 2u;
    adapter.beginFrame(next);
    require(adapter.route(event(
                3u, 2u,
                EditorPointerButtonEvent{{50.0, 50.0},
                                          EditorPointerButton::primary,
                                          EditorInputAction::release, false,
                                          false})),
            "O07 suppressed physical up did not remain owned");
    adapter.closeFrame();
    require(adapter.phase() == GizmoInteractionPhase::draggingLatched &&
                log.terminals.size() == 1u,
            "O07 suppressed release committed unexpectedly");
    require(adapter.interactionObservation().acceptedCommitCount == 0u &&
                adapter.interactionObservation().acceptedLatchCount == 1u,
            "O07 suppressed release manufactured an accepted terminal");
    require(adapter.interactionObservation().deliveredReleaseCount == 0u &&
                adapter.interactionObservation().suppressedReleaseCount == 1u &&
                adapter.interactionObservation().lastTerminalFrame == 2u &&
                adapter.interactionObservation().lastTerminalEventCount == 1u &&
                adapter.interactionObservation().lastTerminalEvents[0] ==
                    GizmoTerminalEvent{
                        GizmoTerminalEventKind::suppressedPhysicalMouseUp, 3u},
            "O07 journal relabeled a suppressed release as delivered");

    next.frameSerial = 3u;
    adapter.beginFrame(next);
    require(adapter.route(event(
                4u, 3u,
                EditorKeyEvent{EditorInputKey::escape,
                               EditorInputAction::press})),
            "O07 Escape was not consumed");
    adapter.closeFrame();
    require(log.terminals.size() == 2u &&
                log.terminals[1] == DesktopGizmoTerminal::detachRetainingPreview &&
                log.clearCalls == 1u,
            "O07 Escape did not detach without history");
  }

  // O07's same-frame release/focus partition must retain callback order while
  // classifying the physical up as suppressed. The focus event is the
  // terminal that latches the drag; the release must never commit it.
  {
    TerminalLog log;
    DesktopGizmoIntegrationAdapter adapter(terminalCallbacks(log));
    require(beginDrag(adapter, 10u),
            "O07 same-frame focus fixture did not begin");
    require(adapter.route(event(
                11u, 10u,
                EditorPointerButtonEvent{{50.0, 50.0},
                                          EditorPointerButton::primary,
                                          EditorInputAction::release, false,
                                          false})),
            "O07 same-frame suppressed release was not consumed");
    require(adapter.route(event(
                12u, 10u,
                EditorFocusEvent{EditorFocusAction::lost})),
            "O07 same-frame focus loss was not consumed");
    adapter.closeFrame();
    const auto observed = adapter.interactionObservation();
    require(adapter.phase() == GizmoInteractionPhase::draggingLatched &&
                observed.activeComponent == GizmoComponent::none &&
                log.terminals.size() == 1u &&
                log.terminals[0] == DesktopGizmoTerminal::latchRetainingPreview &&
                observed.deliveredReleaseCount == 0u &&
                observed.suppressedReleaseCount == 1u &&
                observed.focusLossCount == 1u &&
                observed.lastTerminalEventCount == 2u &&
                observed.lastTerminalEvents[0] ==
                    GizmoTerminalEvent{
                        GizmoTerminalEventKind::suppressedPhysicalMouseUp, 11u} &&
                observed.lastTerminalEvents[1] ==
                    GizmoTerminalEvent{GizmoTerminalEventKind::focusLost, 12u},
            "O07 same-frame release/focus order committed or was rewritten");
  }
}

void testReadOnlyComponentProbe() {
  for (const auto transform : {
           EditorViewportSurfaceTransform::identity,
           EditorViewportSurfaceTransform::rotate90,
           EditorViewportSurfaceTransform::rotate180,
           EditorViewportSurfaceTransform::rotate270,
           EditorViewportSurfaceTransform::mirrorHorizontal,
           EditorViewportSurfaceTransform::mirrorVertical,
           EditorViewportSurfaceTransform::mirrorHorizontalRotate90,
           EditorViewportSurfaceTransform::mirrorHorizontalRotate270}) {
    DesktopGizmoIntegrationAdapter adapter;
    DesktopGizmoFrameInput input = frameInput();
    input.viewport.framebufferScale = {2.0, 2.0};
    input.viewport.physical.extent = {200u, 200u};
    input.viewport.depthPhysical.extent = {200u, 200u};
    input.viewport.colorAttachment = {200u, 200u};
    input.viewport.depthAttachment = {200u, 200u};
    input.viewport.surfaceTransform = transform;
    require(input.viewport.valid(), "probe scaled viewport is invalid");
    adapter.beginFrame(input);
    require(!adapter.componentProbe(GizmoComponent::translateY),
            "component probe invented an unrendered target");
    adapter.closeFrame();
    const auto before = adapter.interactionObservation();
    const auto target = adapter.componentProbe(GizmoComponent::translateY);
    require(target.has_value(), "rendered translation Y has no confirmed target");
    require(target->frameSerial == input.frameSerial &&
                target->documentRevision == input.documentRevision &&
                target->selectionRevision == input.selectionRevision &&
                target->mappingEpoch == input.mappingEpoch &&
                target->node == node() && target->enabled && !target->hovered &&
                !target->active && !target->captureHeld,
            "component probe reported fabricated identity or interaction flags");
    const double physicalX = target->point.x * 2.0;
    const double physicalY = target->point.y * 2.0;
    require(physicalX == std::round(physicalX) &&
                physicalY == std::round(physicalY),
            "component target was not quantized to the native cursor grid");
    require(adapter.componentProbe(GizmoComponent::translateY) == target &&
                adapter.interactionObservation() == before,
            "read-only component probing mutated the adapter");
    require(adapter.route(event(1u, 1u, EditorPointerMoveEvent{target->point})) &&
                adapter.hoveredComponent() == GizmoComponent::translateY,
            "projected target did not hit its actual component");
    adapter.closeFrame();
    require(adapter.componentProbe(GizmoComponent::translateY)->hovered &&
                adapter.interactionObservation().hasPointer &&
                adapter.interactionObservation().pointer == target->point &&
                adapter.interactionObservation().logicalViewport ==
                    input.viewport.logical &&
                adapter.interactionObservation().localTransform ==
                    std::optional(input.attachment->local),
            "component observation did not retain real hover/pointer/transform");
  }

  for (int blockedCase = 0; blockedCase < 5; ++blockedCase) {
    DesktopGizmoIntegrationAdapter adapter;
    DesktopGizmoFrameInput input = frameInput();
    if (blockedCase == 0) input.gizmoSettings.visible = false;
    if (blockedCase == 1) input.attachment.reset();
    if (blockedCase == 2) input.selectedNode.reset();
    if (blockedCase == 3) input.gizmoSettings.visibility = GizmoVisibility::rotation;
    if (blockedCase == 4) input.attachment->selectionRevision += 1u;
    adapter.beginFrame(input);
    adapter.closeFrame();
    require(!adapter.componentProbe(GizmoComponent::translateY),
            "component probe accepted hidden or stale attachment geometry");
  }

  DesktopGizmoIntegrationAdapter adapter;
  auto input = frameInput();
  input.imguiPointerCaptured = true;
  input.imguiKeyboardCaptured = true;
  adapter.beginFrame(input);
  adapter.closeFrame();
  auto target = adapter.componentProbe(GizmoComponent::translateY);
  require(target && target->enabled && !target->hovered,
          "current ImGui ownership hid a target available after pointer move");
  input.frameSerial = 2u;
  input.modalActive = true;
  adapter.beginFrame(input);
  adapter.closeFrame();
  target = adapter.componentProbe(GizmoComponent::translateY);
  require(target && !target->enabled,
          "modal target probe did not report disabled state");
}

void testAcceptedInteractionObservations() {
  bool acceptBegin = false;
  bool acceptPreview = false;
  bool acceptTerminal = false;
  std::uint32_t previewCalls = 0u;
  DesktopGizmoCallbacks callbacks;
  callbacks.beginTransform = [&](NodeId) { return acceptBegin; };
  callbacks.previewTransform = [&](NodeId, const GizmoLocalTransform &) {
    ++previewCalls;
    return acceptPreview;
  };
  callbacks.finishTransform = [&](NodeId, DesktopGizmoTerminal) {
    return acceptTerminal;
  };
  DesktopGizmoIntegrationAdapter adapter(std::move(callbacks));
  adapter.beginFrame(frameInput());
  adapter.closeFrame();
  const auto target = adapter.componentProbe(GizmoComponent::translateY);
  require(target.has_value(), "observation test has no translation Y target");
  const auto press = [&](std::uint64_t sequence) {
    return event(sequence, 1u,
                 EditorPointerButtonEvent{target->point,
                                           EditorPointerButton::primary,
                                           EditorInputAction::press, false, true});
  };
  require(!adapter.route(press(1u)) &&
              adapter.interactionObservation().acceptedBeginCount == 0u &&
              !adapter.interactionObservation().gestureNode,
          "rejected begin was recorded as accepted");
  acceptBegin = true;
  require(adapter.route(press(2u)), "accepted begin was not consumed");
  adapter.closeFrame();
  auto observation = adapter.interactionObservation();
  require(observation.acceptedBeginCount == 1u && observation.captureHeld &&
              observation.gestureNode == node() &&
              observation.phase == GizmoInteractionPhase::dragging &&
              observation.activeComponent == GizmoComponent::translateY &&
              adapter.componentProbe(GizmoComponent::translateY)->active,
          "accepted begin observation lost actual capture/component state");
  const LogicalViewportPoint moved{target->point.x, target->point.y - 10.0};
  require(adapter.route(event(3u, 1u, EditorPointerMoveEvent{moved})),
          "rejected preview event was not owned by the active drag");
  require(adapter.interactionObservation().acceptedPreviewChangeCount == 0u,
          "rejected preview was counted as accepted");
  acceptPreview = true;
  require(adapter.route(event(4u, 1u, EditorPointerMoveEvent{moved})),
          "accepted preview event was not consumed");
  const auto changed = adapter.interactionObservation();
  require(changed.acceptedPreviewChangeCount == 1u && changed.localTransform &&
              changed.localTransform != observation.localTransform,
          "changed preview did not publish its actual transform/count");
  require(adapter.route(event(5u, 1u, EditorPointerMoveEvent{moved})) &&
              adapter.interactionObservation().acceptedPreviewChangeCount == 1u &&
              previewCalls == 3u,
          "duplicate preview advanced change count or changed callback behavior");
  require(adapter.route(event(
              6u, 1u, EditorPointerButtonEvent{
                          moved, EditorPointerButton::primary,
                          EditorInputAction::release, false, true})),
          "rejected commit event was not consumed");
  adapter.closeFrame();
  require(adapter.interactionObservation().acceptedCommitCount == 0u &&
              !adapter.interactionObservation().lastTerminal,
          "rejected terminal was recorded as accepted");

  acceptTerminal = true;
  auto next = frameInput();
  next.frameSerial = 2u;
  adapter.beginFrame(next);
  adapter.closeFrame();
  const auto nextTarget = adapter.componentProbe(GizmoComponent::translateY);
  require(nextTarget && adapter.route(event(
                            7u, 2u, EditorPointerButtonEvent{
                                        nextTarget->point,
                                        EditorPointerButton::primary,
                                        EditorInputAction::press, false, true})),
          "second observed begin did not start");
  require(adapter.route(event(
              8u, 2u, EditorPointerButtonEvent{
                          {200.0, 200.0}, EditorPointerButton::primary,
                          EditorInputAction::release, false, true})),
          "observed outside release was not consumed");
  adapter.closeFrame();
  observation = adapter.interactionObservation();
  require(observation.acceptedBeginCount == 1u &&
              observation.acceptedCommitCount == 1u &&
              observation.acceptedPreviewChangeCount == 1u &&
              observation.deliveredReleaseCount == 2u &&
              observation.escapePressCount == 0u &&
              observation.gestureNode == node() &&
              observation.lastTerminalEventCount == 1u &&
              observation.lastTerminalEvents[0] ==
                  GizmoTerminalEvent{GizmoTerminalEventKind::deliveredRelease, 8u} &&
              observation.lastTerminal == DesktopGizmoTerminal::commit &&
              observation.lastTerminalFrame == 2u &&
              observation.lastTerminalSequence == 8u &&
              !observation.captureHeld && observation.hasPointer &&
              observation.pointer == LogicalViewportPoint{200.0, 200.0},
          "accepted terminal counters did not retain real outside release");
  adapter.closeFrame();
  require(adapter.interactionObservation() == observation,
          "repeated frame close duplicated observed terminal");
  adapter.reset();
  require(adapter.interactionObservation() == DesktopGizmoInteractionObservation{},
          "explicit adapter reset did not clear observation lifetime");
}

void testRejectedTerminalRetainsOwnership() {
  DesktopGizmoCallbacks callbacks;
  bool acceptTerminal = false;
  std::uint32_t clearCalls = 0u;
  callbacks.beginTransform = [](NodeId) { return true; };
  callbacks.previewTransform = [](NodeId, const GizmoLocalTransform &) {
    return true;
  };
  callbacks.finishTransform = [&](NodeId, DesktopGizmoTerminal) {
    return acceptTerminal;
  };
  callbacks.clearSelection = [&] { ++clearCalls; };
  DesktopGizmoIntegrationAdapter adapter(std::move(callbacks));
  require(beginDrag(adapter, 1u), "rejected-terminal fixture did not begin");
  require(adapter.route(event(
              2u, 1u,
              EditorPointerButtonEvent{{200.0, 200.0},
                                        EditorPointerButton::primary,
                                        EditorInputAction::release, false,
                                        true})),
          "rejected terminal release was not consumed");
  adapter.closeFrame();
  auto observation = adapter.interactionObservation();
  require(adapter.phase() == GizmoInteractionPhase::dragging &&
              adapter.pointerCaptured() &&
              observation.acceptedCommitCount == 0u && clearCalls == 0u,
          "rejected terminal released Gizmo ownership or cleared selection");

  // A later terminal can retry the authoritative controller operation without
  // manufacturing a second begin transaction.
  acceptTerminal = true;
  auto next = frameInput();
  next.frameSerial = 2u;
  adapter.beginFrame(next);
  require(adapter.route(event(
              3u, 2u,
              EditorPointerButtonEvent{{200.0, 200.0},
                                        EditorPointerButton::primary,
                                        EditorInputAction::release, false,
                                        true})),
          "retry terminal release was not consumed");
  adapter.closeFrame();
  observation = adapter.interactionObservation();
  require(adapter.phase() == GizmoInteractionPhase::attachedIdle &&
              !adapter.pointerCaptured() &&
              observation.acceptedCommitCount == 1u,
          "accepted retry did not finalize the retained Gizmo transaction");

  // The no-history Escape path must obey the same rule and must not clear the
  // selection while its authoritative finish callback is rejected.
  require(beginDrag(adapter, 3u), "rejected-detach fixture did not begin");
  acceptTerminal = false;
  require(adapter.route(event(
              4u, 3u,
              EditorKeyEvent{EditorInputKey::escape,
                             EditorInputAction::press})),
          "rejected Escape terminal was not consumed");
  adapter.closeFrame();
  require(adapter.phase() == GizmoInteractionPhase::dragging &&
              adapter.pointerCaptured() && clearCalls == 0u,
          "rejected Escape released ownership or cleared selection");
  acceptTerminal = true;
  next = frameInput();
  next.frameSerial = 4u;
  adapter.beginFrame(next);
  require(adapter.route(event(
              5u, 4u,
              EditorKeyEvent{EditorInputKey::escape,
                             EditorInputAction::press})),
          "accepted Escape retry was not consumed");
  adapter.closeFrame();
  require(adapter.phase() == GizmoInteractionPhase::detached &&
              !adapter.pointerCaptured() && clearCalls == 1u,
          "accepted Escape retry did not detach and clear selection");
}

void testDocumentRevisionRollbackDetaches() {
  std::uint32_t detaches = 0u;
  DesktopGizmoCallbacks callbacks;
  callbacks.beginTransform = [](NodeId) { return true; };
  callbacks.previewTransform = [](NodeId, const GizmoLocalTransform &) {
    return true;
  };
  callbacks.finishTransform = [&](NodeId, DesktopGizmoTerminal terminal) {
    if (terminal == DesktopGizmoTerminal::detachRetainingPreview) ++detaches;
    return true;
  };
  DesktopGizmoIntegrationAdapter adapter(std::move(callbacks));
  require(beginDrag(adapter, 1u), "revision rollback fixture did not begin");
  auto rollback = frameInput();
  rollback.frameSerial = 2u;
  rollback.documentRevision = 11u;
  rollback.attachment->documentRevision = 11u;
  adapter.beginFrame(rollback);
  require(adapter.phase() != GizmoInteractionPhase::dragging &&
              adapter.phase() != GizmoInteractionPhase::draggingLatched &&
              detaches == 1u,
          "document revision rollback retained an active Gizmo fence");

  // The same monotonic rule applies to an explicit revision refresh while a
  // gesture is active.
  require(beginDrag(adapter, 3u), "refresh rollback fixture did not begin");
  adapter.refreshRevisions(10u, 13u, 14u);
  require(adapter.phase() != GizmoInteractionPhase::dragging &&
              adapter.phase() != GizmoInteractionPhase::draggingLatched &&
              detaches == 2u,
          "refresh revision rollback retained an active Gizmo fence");

  require(beginDrag(adapter, 4u),
          "attachment-refresh rollback fixture did not begin");
  auto refreshedAttachment = *frameInput().attachment;
  refreshedAttachment.documentRevision = 10u;
  adapter.refreshAttachment(10u, 13u, 14u, node(), refreshedAttachment);
  require(adapter.phase() != GizmoInteractionPhase::dragging &&
              adapter.phase() != GizmoInteractionPhase::draggingLatched &&
              detaches == 3u,
          "attachment revision rollback retained an active Gizmo fence");
}

void testTerminalJournalOverflowIsExplicit() {
  TerminalLog log;
  DesktopGizmoIntegrationAdapter adapter(terminalCallbacks(log));
  require(beginDrag(adapter, 1u), "journal overflow fixture did not begin");
  for (std::uint64_t sequence = 2u; sequence <= 10u; ++sequence) {
    require(adapter.route(event(
                sequence, 1u, EditorKeyEvent{EditorInputKey::escape,
                                            EditorInputAction::press})),
            "active gesture did not consume actual Escape event");
  }
  adapter.closeFrame();
  const auto observation = adapter.interactionObservation();
  require(observation.escapePressCount == 9u &&
              observation.lastTerminalEventCount == 8u &&
              observation.lastTerminalEventsOverflowed &&
              observation.lastTerminalEvents.front().sequence == 2u &&
              observation.lastTerminalEvents.back().sequence == 9u,
          "bounded terminal journal silently claimed complete event coverage");
}

void testNativeCallbackProvenanceSurvivesDetach() {
  using Queue = NativeGizmoTerminalQueue;
  for (const bool escapeFirst : {true, false}) {
    TerminalLog log;
    DesktopGizmoIntegrationAdapter adapter(terminalCallbacks(log));
    require(beginDrag(adapter, 1u), "native provenance gesture did not begin");
    Queue queue;
    queue.observePrimaryButton(true, false);
    if (escapeFirst) {
      queue.observeEscape(true, true);
      queue.observePrimaryButton(false, true);
    } else {
      queue.observePrimaryButton(false, true);
      queue.observeEscape(true, true);
    }
    queue.observeEscape(false, true);
    const Queue::Batch batch = queue.take();
    require(batch.count == 2u && !batch.overflowed && !batch.focusInterrupted,
            "native provenance fixture did not retain both terminal callbacks");
    for (std::size_t index = 0u; index < batch.count; ++index) {
      const auto &callback = batch.events[index];
      const EditorInputEventPayload payload =
          callback.kind == Queue::Kind::escapePressed
              ? EditorInputEventPayload{EditorKeyEvent{
                    EditorInputKey::escape, EditorInputAction::press}}
              : EditorInputEventPayload{EditorPointerButtonEvent{
                    {50.0, 50.0}, EditorPointerButton::primary,
                    EditorInputAction::release, false, true}};
      require(adapter.route(event(100u + index, 1u, payload),
                            callback.callbackSequence),
              "native callback terminal was not consumed by the active adapter");
    }
    adapter.closeFrame();
    const auto detached = adapter.interactionObservation();
    require(detached.acceptedDetachCount == 1u &&
                detached.acceptedCommitCount == 0u &&
                detached.lastTerminalEventCount == 2u &&
                detached.lastTerminalEvents[0].sequence == 100u &&
                detached.lastTerminalEvents[1].sequence == 101u &&
                detached.lastTerminalCallbackSequences[0] == 2u &&
                detached.lastTerminalCallbackSequences[1] == 3u &&
                detached.lastTerminalEvents[0].kind ==
                    (escapeFirst ? GizmoTerminalEventKind::escapePressed
                                 : GizmoTerminalEventKind::deliveredRelease) &&
                detached.lastTerminalEvents[1].kind ==
                    (escapeFirst ? GizmoTerminalEventKind::deliveredRelease
                                 : GizmoTerminalEventKind::escapePressed),
            "adapter conflated generated route ordinals with native callback order");
    for (std::size_t index = 2u;
         index < detached.lastTerminalCallbackSequences.size(); ++index)
      require(detached.lastTerminalCallbackSequences[index] == 0u,
              "unused native callback provenance was not zero initialized");

    DesktopGizmoFrameInput next = frameInput();
    next.frameSerial = 2u;
    next.selectedNode.reset();
    next.attachment.reset();
    adapter.beginFrame(next);
    require(!adapter.route(event(
                102u, 2u, EditorKeyEvent{EditorInputKey::escape,
                                         EditorInputAction::release}), 4u) &&
                !adapter.route(event(
                    103u, 2u, EditorPointerButtonEvent{
                                {50.0, 50.0}, EditorPointerButton::primary,
                                EditorInputAction::release, false, true}), 5u),
            "detached key-up or late release regained native ownership");
    adapter.closeFrame();
    const auto persisted = adapter.interactionObservation();
    require(persisted.lastTerminalCallbackSequences ==
                detached.lastTerminalCallbackSequences &&
                persisted.lastTerminalEvents == detached.lastTerminalEvents,
            "later frames or detached inputs erased callback provenance");
    require(beginDrag(adapter, 3u), "subsequent provenance gesture did not begin");
    require(adapter.route(event(
                104u, 3u, EditorKeyEvent{EditorInputKey::escape,
                                         EditorInputAction::press})),
            "legacy event failed after native provenance gesture");
    adapter.closeFrame();
    require(adapter.interactionObservation().lastTerminalCallbackSequences ==
                std::array<std::uint64_t, 8>{},
            "new legacy batch inherited previous callback provenance");
  }
}

void testBackendYConventionMatchesRendering() {
  std::optional<LogicalViewportPoint> defaultTarget;
  std::optional<GizmoLocalTransform> defaultPreview;
  for (const bool ndcYDown : {false, true}) {
    DesktopGizmoFrameInput input = frameInput();
    input.camera.ndcYDown = ndcYDown;
    if (ndcYDown) {
      input.viewProjection.elements[5] = -1.0;
      input.camera.inverseViewProjection.elements[5] = -1.0;
    }
    DesktopGizmoCallbacks callbacks;
    callbacks.beginTransform = [](NodeId) { return true; };
    callbacks.previewTransform = [](NodeId, const GizmoLocalTransform &) {
      return true;
    };
    DesktopGizmoIntegrationAdapter adapter(std::move(callbacks));
    adapter.beginFrame(input);
    adapter.closeFrame();
    const auto target = adapter.componentProbe(GizmoComponent::translateY);
    const double centerY = input.viewport.logical.origin.y +
                           input.viewport.logical.extent.height * 0.5;
    require(target && target->point.y < centerY,
            "translation Y probe does not lie on the rendered upward axis");
    require(adapter.interactionObservation().ndcYDown == ndcYDown,
            "interaction evidence omitted the actual backend Y convention");
    if (defaultTarget) {
      require(target->point == *defaultTarget,
              "matching Vulkan projection/viewport changed the rendered target");
    } else {
      defaultTarget = target->point;
    }
    require(adapter.route(event(1u, 1u, EditorPointerMoveEvent{target->point})) &&
                adapter.hoveredComponent() == GizmoComponent::translateY,
            "rendered upward translation axis did not hit its component");
    require(adapter.route(event(
                2u, 1u, EditorPointerButtonEvent{
                            target->point, EditorPointerButton::primary,
                            EditorInputAction::press, false, true})),
            "backend Y convention drag did not begin");
    const LogicalViewportPoint moved{target->point.x, target->point.y - 10.0};
    require(adapter.route(event(3u, 1u, EditorPointerMoveEvent{moved})),
            "backend Y convention upward move was not consumed");
    adapter.closeFrame();
    const auto observation = adapter.interactionObservation();
    require(observation.localTransform &&
                observation.localTransform->translation.y > 0.0 &&
                observation.acceptedPreviewChangeCount == 1u,
            "upward cursor motion did not produce positive world Y");
    if (defaultPreview) {
      require(observation.localTransform == defaultPreview,
              "Vulkan and default backend produced different upward drag values");
    } else {
      defaultPreview = observation.localTransform;
    }
    const auto ndc = transformOverlayPoint(
        input.viewProjection, observation.localTransform->translation);
    require(ndc.has_value(), "dragged pivot could not be projected");
    // Vulkan positive-height VkViewport maps -1 to the top; default D3D maps
    // +1 to the top. This independently checks the actual rendered motion.
    const double renderedY = input.viewport.logical.origin.y +
                             (ndcYDown ? ndc->y + 1.0 : 1.0 - ndc->y) * 0.5 *
                                 input.viewport.logical.extent.height;
    require(std::abs(renderedY - (centerY - 10.0)) < 1.0e-9,
            "rendered selected pivot did not follow the upward cursor delta");
  }
}

} // namespace

int main() {
  try {
    testNativeTerminalQueueOrderAndFiltering();
    testNativeTerminalQueueFocusInterruption();
    testNativeTerminalQueueOverflowAndExhaustion();
    testSnapshotAndFence();
    testRevisionZeroAttachment();
    testTerminalBatchIsIdempotent();
    testUiOwnershipAndStableIdentity();
    testSnapAndDragBaseline();
    testStaleFenceDetaches();
    testAcceptedProjectionRevisionAdvance();
    testTerminalSemantics();
    testReadOnlyComponentProbe();
    testAcceptedInteractionObservations();
    testRejectedTerminalRetainsOwnership();
    testDocumentRevisionRollbackDetaches();
    testTerminalJournalOverflowIsExplicit();
    testNativeCallbackProvenanceSurvivesDetach();
    testBackendYConventionMatchesRendering();
    std::cout << "Editor Gizmo integration tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
}
