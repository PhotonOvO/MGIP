#include <mgif/scene_editor/EditorInputRouter.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>
#include <utility>

namespace {

using namespace demo::editor;
using namespace std::chrono_literals;

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
      std::uint32_t slot) {
  return Id::from(EditorObjectId{kind, slot, 1u, authority}).value_or(Id{});
}

struct Fixture final {
  EditorDocumentAuthority authority{77u};
  SceneId scene = id<SceneId>(EditorObjectKind::scene, authority, 1u);
};

EditorViewportMetrics viewport(double dpi = 1.0,
                               std::uint64_t generation = 17u) {
  EditorViewportMetrics result;
  result.logical = {{10.0, 20.0}, {100.0, 50.0}};
  result.framebufferLogicalOrigin = {0.0, 0.0};
  result.framebufferScale = {dpi, dpi};
  const auto startX = static_cast<std::uint32_t>(std::floor(10.0 * dpi));
  const auto startY = static_cast<std::uint32_t>(std::floor(20.0 * dpi));
  const auto endX = static_cast<std::uint32_t>(std::ceil(110.0 * dpi));
  const auto endY = static_cast<std::uint32_t>(std::ceil(70.0 * dpi));
  result.physical = {
      {startX, startY}, {endX - startX, endY - startY}};
  result.depthPhysical = result.physical;
  result.colorAttachment = {
      result.physical.origin.x + result.physical.extent.width,
      result.physical.origin.y + result.physical.extent.height};
  result.depthAttachment = result.colorAttachment;
  result.viewportGeneration = generation;
  result.projection = {23u, 0x1234u};
  result.view = {29u, 0x5678u};
  result.convention = {EditorClipDepthRange::zeroToOne, true, true};
  result.projectionKind = EditorProjectionKind::perspective;
  result.verticalFieldOfViewRadians = 1.0;
  return result;
}

EditorInputRoutingContext idleContext(double dpi = 1.0,
                                      std::uint64_t generation = 17u) {
  Fixture fixture;
  EditorInputRoutingContext result;
  result.viewport = viewport(dpi, generation);
  result.lifecycle = {fixture.authority, fixture.scene, 31u, generation};
  return result;
}

EditorPointerButtonEvent primaryPress(
    LogicalViewportPoint point = {35.0, 32.5},
    bool recognizedDouble = false) {
  return {point, EditorPointerButton::primary, EditorInputAction::press,
          recognizedDouble, true};
}

EditorPointerButtonEvent primaryRelease(
    LogicalViewportPoint point = {35.0, 32.5}, bool delivered = true) {
  return {point, EditorPointerButton::primary, EditorInputAction::release,
          false, delivered};
}

EditorKeyEvent escapePress() {
  return {EditorInputKey::escape, EditorInputAction::press};
}

EditorKeyEvent escapeRelease() {
  return {EditorInputKey::escape, EditorInputAction::release};
}

struct Harness final {
  FakeClock clock;
  EditorInputRouter router{clock};
  std::uint64_t sequence{1u};

  template <typename Payload>
  EditorInputDecision send(Payload payload,
                           const EditorInputRoutingContext &context,
                           std::uint64_t frame = 1u) {
    EditorInputEvent event;
    event.sequence = {sequence++};
    event.frame = {frame};
    event.payload = std::move(payload);
    return router.route(event, context);
  }

  EditorInputDecision close(const EditorInputRoutingContext &context,
                            std::uint64_t frame = 1u) {
    return send(EditorInputFrameCloseEvent{}, context, frame);
  }
};

EditorInputRoutingContext gizmoContext() {
  EditorInputRoutingContext result = idleContext();
  result.gizmoActive = true;
  result.viewportPickingEnabled = true;
  result.cameraNavigationEnabled = true;
  return result;
}

bool testLockedPriorityAndTypedCoordinates() {
  bool passed = true;
  {
    Harness harness;
    EditorInputRoutingContext all = gizmoContext();
    all.modalActive = true;
    all.imguiPointerCaptured = true;
    const EditorInputDecision decision = harness.send(primaryPress(), all);
    passed &= expect(decision.consumer == EditorInputConsumer::modal &&
                         !decision.effects.beginGizmoDrag &&
                         !decision.effects.pick,
                     "modal owns pointer ahead of every other consumer");
  }
  {
    Harness harness;
    EditorInputRoutingContext context = gizmoContext();
    context.imguiPointerCaptured = true;
    const EditorInputDecision decision = harness.send(primaryPress(), context);
    passed &= expect(decision.consumer == EditorInputConsumer::activeGizmo &&
                         decision.effects.beginGizmoDrag,
                     "active gizmo owns pointer ahead of ImGui and picking");
  }
  {
    Harness harness;
    EditorInputRoutingContext context = idleContext();
    context.imguiPointerCaptured = true;
    context.viewportPickingEnabled = true;
    context.cameraNavigationEnabled = true;
    const EditorInputDecision decision = harness.send(primaryPress(), context);
    passed &= expect(
        decision.consumer == EditorInputConsumer::imguiCapturedWidget,
        "ImGui capture owns pointer ahead of picking and camera");
  }
  {
    Harness harness;
    EditorInputRoutingContext context = idleContext(2.0);
    context.viewportPickingEnabled = true;
    context.cameraNavigationEnabled = true;
    const EditorInputDecision decision = harness.send(primaryPress(), context);
    passed &= expect(
        decision.consumer == EditorInputConsumer::viewportPicking &&
            decision.coordinates && decision.coordinates->insideViewport() &&
            decision.coordinates->physical &&
            decision.coordinates->physical->x == 70u &&
            decision.coordinates->physical->y == 65u &&
            decision.coordinates->physical->localX == 50u &&
            decision.coordinates->physical->localY == 25u,
        "eligible picking owns primary input and converts DPI exactly once");
  }
  {
    Harness harness;
    EditorInputRoutingContext context = idleContext();
    context.cameraNavigationEnabled = true;
    const EditorPointerButtonEvent secondary{
        {35.0, 32.5}, EditorPointerButton::secondary,
        EditorInputAction::press, false, true};
    const EditorInputDecision decision = harness.send(secondary, context);
    passed &= expect(decision.consumer ==
                           EditorInputConsumer::cameraNavigation,
                     "camera receives only otherwise unowned viewport input");
  }
  return passed;
}

bool testClassifierThroughSoleRouter() {
  Harness harness;
  EditorInputRoutingContext context = idleContext();
  context.viewportPickingEnabled = true;
  context.cameraNavigationEnabled = true;
  bool passed = true;

  harness.clock.set(0ms);
  const EditorInputDecision press = harness.send(primaryPress(), context);
  const EditorInputDecision release = harness.send(primaryRelease(), context);
  passed &= expect(press.consumer == EditorInputConsumer::viewportPicking &&
                       !press.effects.pick && !release.effects.pick &&
                       harness.router.clickPhase() ==
                           EditorClickClassifierPhase::singlePending,
                   "first release leaves one pending classifier intent");
  (void)harness.send(EditorPointerMoveEvent{{60.0, 40.0}}, context, 2u);
  harness.clock.set(300ms);
  const EditorInputDecision atBoundary =
      harness.send(EditorInputTickEvent{}, context, 2u);
  harness.clock.set(301ms);
  const EditorInputDecision afterBoundary =
      harness.send(EditorInputTickEvent{}, context, 3u);
  passed &= expect(!atBoundary.effects.pick && afterBoundary.effects.pick &&
                       afterBoundary.effects.pick->intent ==
                           PickIntent::singleSelect &&
                       afterBoundary.effects.pick->logicalPosition ==
                           LogicalViewportPoint{60.0, 40.0},
                   "router preserves strict T and emission-time pointer");

  harness.clock.set(1s);
  (void)harness.send(primaryPress(), context, 4u);
  (void)harness.send(primaryRelease(), context, 4u);
  harness.clock.set(1100ms);
  const EditorInputDecision doublePress = harness.send(
      primaryPress({36.0, 33.0}, true), context, 5u);
  passed &= expect(doublePress.effects.pick &&
                       doublePress.effects.pick->intent ==
                           PickIntent::doubleFocus &&
                       harness.router.clickPhase() ==
                           EditorClickClassifierPhase::doubleClick,
                   "recognized second press emits double immediately");
  (void)harness.send(primaryRelease({36.0, 33.0}), context, 5u);
  harness.clock.set(2s);
  passed &= expect(!harness.send(EditorInputTickEvent{}, context, 6u)
                         .effects.pick,
                   "double leaves no delayed single");

  (void)harness.send(primaryPress(), context, 7u);
  (void)harness.send(EditorPointerMoveEvent{{50.0, 50.0}}, context, 7u);
  (void)harness.send(primaryRelease({50.0, 50.0}), context, 7u);
  harness.clock.set(3s);
  passed &= expect(!harness.send(EditorInputTickEvent{}, context, 8u)
                         .effects.pick,
                   "drag through sole router emits no pick");

  for (const double dpi : {1.0, 1.25, 1.5, 2.0}) {
    Harness dpiHarness;
    EditorInputRoutingContext dpiContext = idleContext(dpi);
    dpiContext.viewportPickingEnabled = true;
    const EditorInputDecision inside = dpiHarness.send(
        primaryPress({109.999, 69.999}), dpiContext);
    passed &= expect(inside.coordinates && inside.coordinates->physical &&
                         inside.coordinates->physical->localX + 1u ==
                             dpiContext.viewport.physical.extent.width &&
                         inside.coordinates->physical->localY + 1u ==
                             dpiContext.viewport.physical.extent.height,
                     "100/125/150/200 percent last pixel remains half-open");
    Harness edgeHarness;
    const EditorInputDecision edge = edgeHarness.send(
        primaryPress({110.0, 40.0}), dpiContext);
    passed &= expect(edge.consumer == EditorInputConsumer::router &&
                         edge.coordinates &&
                         !edge.coordinates->insideViewport(),
                     "right edge is excluded at every DPI");
  }
  return passed;
}

bool testGizmoCaptureOutsideAndOrdinaryRelease() {
  Harness harness;
  EditorInputRoutingContext context = gizmoContext();
  bool passed = true;
  const EditorInputDecision press = harness.send(primaryPress(), context);
  const EditorInputDecision move = harness.send(
      EditorPointerMoveEvent{{500.0, 500.0}}, idleContext());
  const EditorInputDecision release = harness.send(
      primaryRelease({500.0, 500.0}), idleContext());
  passed &= expect(press.effects.beginGizmoDrag &&
                       move.consumer == EditorInputConsumer::activeGizmo &&
                       move.effects.updateGizmoDrag && move.coordinates &&
                       !move.coordinates->insideViewport() &&
                       release.consumer == EditorInputConsumer::activeGizmo &&
                       !release.effects.commitGizmoDrag &&
                       harness.router.hasPointerCapture(),
                   "gizmo captures outside and defers release to batch close");
  const EditorInputDecision close = harness.close(idleContext());
  passed &= expect(close.effects.commitGizmoDrag && close.effects.detachGizmo &&
                       close.effects.releasePointerCapture &&
                       !close.effects.clearSelection &&
                       !harness.router.gizmoDragActive() &&
                       !harness.router.hasPointerCapture(),
                   "ordinary release commits exactly once at frame close");
  const EditorInputDecision repeated = harness.send(
      primaryRelease({500.0, 500.0}), idleContext(), 2u);
  passed &= expect(repeated.status == EditorInputRouteStatus::rejectedState &&
                       !repeated.effects.commitGizmoDrag,
                   "late repeated release cannot double commit");
  return passed;
}

EditorInputEffects sameBatchEffects(bool escapeFirst) {
  Harness harness;
  EditorInputRoutingContext context = gizmoContext();
  (void)harness.send(primaryPress(), context);
  (void)harness.send(EditorPointerMoveEvent{{50.0, 40.0}}, context);
  if (escapeFirst) {
    (void)harness.send(escapePress(), idleContext());
    (void)harness.send(primaryRelease({500.0, 500.0}), idleContext());
  } else {
    (void)harness.send(primaryRelease({500.0, 500.0}), idleContext());
    (void)harness.send(escapePress(), idleContext());
  }
  return harness.close(idleContext()).effects;
}

bool testO05O06AndPriorFrameRelease() {
  bool passed = true;
  const EditorInputEffects escapeThenRelease = sameBatchEffects(true);
  const EditorInputEffects releaseThenEscape = sameBatchEffects(false);
  passed &= expect(escapeThenRelease == releaseThenEscape &&
                       escapeThenRelease.clearSelection &&
                       escapeThenRelease.detachGizmo &&
                       escapeThenRelease.releasePointerCapture &&
                       !escapeThenRelease.commitGizmoDrag,
                   "O06 both same-batch orders are one no-history terminal");

  Harness escapeOnly;
  const EditorInputRoutingContext gizmo = gizmoContext();
  (void)escapeOnly.send(primaryPress(), gizmo);
  (void)escapeOnly.send(EditorPointerMoveEvent{{45.0, 36.0}}, gizmo);
  (void)escapeOnly.send(escapePress(), idleContext());
  const EditorInputDecision escaped = escapeOnly.close(idleContext());
  passed &= expect(escaped.effects.clearSelection &&
                       escaped.effects.detachGizmo &&
                       !escaped.effects.commitGizmoDrag,
                   "O05 Escape-only retains preview and emits no commit");

  Harness priorFrame;
  (void)priorFrame.send(primaryPress(), gizmo, 1u);
  (void)priorFrame.send(primaryRelease(), idleContext(), 1u);
  const EditorInputDecision committed = priorFrame.close(idleContext(), 1u);
  const EditorInputDecision laterEscape =
      priorFrame.send(escapePress(), idleContext(), 2u);
  (void)priorFrame.send(escapeRelease(), idleContext(), 2u);
  const EditorInputDecision laterClose = priorFrame.close(idleContext(), 2u);
  passed &= expect(committed.effects.commitGizmoDrag &&
                       laterEscape.effects.clearSelection &&
                       !laterEscape.effects.commitGizmoDrag &&
                       !laterClose.effects.commitGizmoDrag,
                   "prior-frame release commits once; later Escape cannot undo or duplicate");
  return passed;
}

bool testO07ModalTakeoverAndLifecycleReset() {
  bool passed = true;
  const EditorInputRoutingContext gizmo = gizmoContext();
  Harness focus;
  (void)focus.send(primaryPress(), gizmo);
  const EditorInputDecision lost = focus.send(
      EditorFocusEvent{EditorFocusAction::lost}, idleContext());
  const EditorInputDecision suppressed = focus.send(
      primaryRelease({500.0, 500.0}, false), idleContext());
  const EditorInputDecision latched = focus.close(idleContext());
  passed &= expect(lost.consumer == EditorInputConsumer::activeGizmo &&
                       !suppressed.effects.commitGizmoDrag &&
                       latched.effects.latchGizmo &&
                       latched.effects.releasePointerCapture &&
                       !latched.effects.commitGizmoDrag &&
                       focus.router.gizmoDragLatched(),
                   "O07 focus loss plus suppressed mouse-up latches with no history");
  (void)focus.send(escapePress(), idleContext(), 2u);
  const EditorInputDecision clearLatch = focus.close(idleContext(), 2u);
  passed &= expect(clearLatch.effects.clearSelection &&
                       clearLatch.effects.detachGizmo &&
                       !clearLatch.effects.commitGizmoDrag &&
                       !focus.router.gizmoDragLatched(),
                   "Escape clears latched drag without history");

  // A defensive mixed batch must follow the same focus-first rule as the
  // native Gizmo resolver, regardless of whether the delivered-up arrives
  // before or after the focus callback.
  for (const bool releaseFirst : {false, true}) {
    Harness mixed;
    (void)mixed.send(primaryPress(), gizmo);
    if (releaseFirst) {
      (void)mixed.send(primaryRelease({500.0, 500.0}), idleContext());
      (void)mixed.send(EditorFocusEvent{EditorFocusAction::lost}, idleContext());
    } else {
      (void)mixed.send(EditorFocusEvent{EditorFocusAction::lost}, idleContext());
      (void)mixed.send(primaryRelease({500.0, 500.0}), idleContext());
    }
    const EditorInputDecision mixedClose = mixed.close(idleContext());
    passed &= expect(mixedClose.effects.latchGizmo &&
                         !mixedClose.effects.commitGizmoDrag &&
                         mixed.router.gizmoDragLatched(),
                     "focus loss wins over a same-batch delivered release");
  }

  Harness modal;
  (void)modal.send(primaryPress(), gizmo);
  EditorInputRoutingContext modalContext = idleContext();
  modalContext.modalActive = true;
  (void)modal.send(EditorPointerMoveEvent{{40.0, 40.0}}, modalContext);
  const EditorInputDecision modalClose = modal.close(modalContext);
  passed &= expect(modalClose.consumer == EditorInputConsumer::modal &&
                       modalClose.effects.detachGizmo &&
                       !modalClose.effects.clearSelection &&
                       !modalClose.effects.commitGizmoDrag,
                   "modal takeover detaches without selection or history side effect");

  Harness lifecycle;
  EditorInputRoutingContext pick = idleContext();
  pick.viewportPickingEnabled = true;
  lifecycle.clock.set(0ms);
  (void)lifecycle.send(primaryPress(), pick);
  (void)lifecycle.send(primaryRelease(), pick);
  lifecycle.clock.set(1s);
  EditorInputRoutingContext replaced = idleContext(1.0, 18u);
  replaced.viewportPickingEnabled = true;
  ++replaced.lifecycle.sceneEpoch;
  const EditorInputDecision resetByFence =
      lifecycle.send(EditorInputTickEvent{}, replaced, 2u);
  const EditorInputDecision laterTick =
      lifecycle.send(EditorInputTickEvent{}, replaced, 2u);
  passed &= expect(resetByFence.routed() && !resetByFence.effects.pick &&
                       !laterTick.effects.pick &&
                       lifecycle.router.clickPhase() ==
                           EditorClickClassifierPhase::idle,
                   "document/scene/viewport replacement resets pending click");

  Harness explicitReset;
  (void)explicitReset.send(primaryPress(), gizmo);
  const EditorInputDecision reset = explicitReset.send(
      EditorInputLifecycleResetEvent{}, gizmo);
  passed &= expect(reset.effects.detachGizmo &&
                       reset.effects.releasePointerCapture &&
                       !reset.effects.commitGizmoDrag &&
                       !explicitReset.router.gizmoDragActive(),
                   "explicit destruction reset detaches active input fail-closed");
  return passed;
}

// Regression: a right-drag is the fly camera's look gesture. The router must
// route it to cameraNavigation without reporting pointer capture, because the
// host gates camera input on EditorViewportRuntime::cameraNavigationSuppressed(),
// which consults hasPointerCapture(). Reporting capture made that gate true for
// the whole drag, so rotation was dropped and the disabled cursor released.
// Picking and Gizmo ownership must still report capture.
bool testCameraNavigationDragDoesNotReportPointerCapture() {
  bool passed = true;
  {
    Harness harness;
    EditorInputRoutingContext context = idleContext();
    context.cameraNavigationEnabled = true;
    const EditorPointerButtonEvent secondaryPress{
        {35.0, 32.5}, EditorPointerButton::secondary, EditorInputAction::press,
        false, true};
    const EditorInputDecision press =
        harness.send(secondaryPress, context, 1u);
    passed &= expect(press.consumer == EditorInputConsumer::cameraNavigation &&
                         !press.effects.beginGizmoDrag && !press.effects.pick &&
                         !harness.router.hasPointerCapture() &&
                         harness.router.buttonDown(EditorPointerButton::secondary),
                     "camera-owned secondary press is held without capture");
    const EditorInputDecision move = harness.send(
        EditorPointerMoveEvent{{90.0, 60.0}}, context, 2u);
    passed &= expect(move.consumer == EditorInputConsumer::cameraNavigation &&
                         !harness.router.hasPointerCapture(),
                     "camera keeps ownership across held-drag moves");
    const EditorInputDecision release = harness.send(
        EditorPointerButtonEvent{{90.0, 60.0}, EditorPointerButton::secondary,
                                 EditorInputAction::release, false, true},
        context, 3u);
    passed &= expect(release.consumer == EditorInputConsumer::cameraNavigation &&
                         !harness.router.hasPointerCapture() &&
                         !harness.router.buttonDown(
                             EditorPointerButton::secondary),
                     "camera-owned secondary release clears the button");
  }
  {
    // The camera gate stays closed for every other consumer, so the fix cannot
    // widen into a general suppression hole.
    Harness harness;
    EditorInputRoutingContext context = idleContext();
    context.cameraNavigationEnabled = true;
    context.viewportPickingEnabled = true;
    const EditorInputDecision pickPress =
        harness.send(primaryPress(), context, 1u);
    passed &= expect(pickPress.consumer == EditorInputConsumer::viewportPicking &&
                         harness.router.hasPointerCapture(),
                     "picking-owned primary press still reports capture");
  }
  {
    Harness harness;
    EditorInputRoutingContext context = gizmoContext();
    context.cameraNavigationEnabled = true;
    const EditorInputDecision primaryPressEvent =
        harness.send(primaryPress(), context, 1u);
    passed &= expect(primaryPressEvent.effects.beginGizmoDrag &&
                         harness.router.hasPointerCapture(),
                     "Gizmo-owned primary press still reports capture");
    const EditorInputDecision move = harness.send(
        EditorPointerMoveEvent{{500.0, 500.0}}, idleContext(), 2u);
    passed &= expect(move.consumer == EditorInputConsumer::activeGizmo &&
                         move.effects.updateGizmoDrag &&
                         harness.router.hasPointerCapture(),
                     "active Gizmo drag keeps capture across frames");
  }
  {
    // A camera-owned button must not mask ImGui's own capture request.
    Harness harness;
    EditorInputRoutingContext context = idleContext();
    context.cameraNavigationEnabled = true;
    harness.send(
        EditorPointerButtonEvent{{35.0, 32.5}, EditorPointerButton::secondary,
                                 EditorInputAction::press, false, true},
        context, 1u);
    EditorInputRoutingContext captured = context;
    captured.imguiPointerCaptured = true;
    harness.send(EditorPointerMoveEvent{{40.0, 40.0}}, captured, 2u);
    passed &= expect(!harness.router.hasPointerCapture(),
                     "camera-owned drag does not fabricate capture under ImGui");
  }
  return passed;
}

bool testStaleAndMalformedEventsFailClosed() {
  Harness harness;
  const EditorInputRoutingContext context = idleContext();
  bool passed = true;
  const EditorInputDecision first =
      harness.send(EditorInputTickEvent{}, context, 2u);
  EditorInputEvent stale;
  stale.sequence = {harness.sequence++};
  stale.frame = {1u};
  stale.payload = EditorInputTickEvent{};
  const EditorInputDecision staleResult = harness.router.route(stale, context);
  const EditorInputDecision replay = harness.router.route(stale, context);
  const EditorInputDecision invalid = harness.send(
      EditorPointerMoveEvent{{std::numeric_limits<double>::quiet_NaN(), 0.0}},
      context, 2u);
  passed &= expect(first.status == EditorInputRouteStatus::ignored &&
                       staleResult.status ==
                           EditorInputRouteStatus::rejectedStale &&
                       replay.status == EditorInputRouteStatus::rejectedStale &&
                       invalid.status ==
                           EditorInputRouteStatus::rejectedInvalid &&
                       !harness.router.hasPointerCapture(),
                   "stale, replayed, and malformed events fail closed");
  return passed;
}

} // namespace

int main() {
  bool passed = true;
  passed &= testLockedPriorityAndTypedCoordinates();
  passed &= testClassifierThroughSoleRouter();
  passed &= testGizmoCaptureOutsideAndOrdinaryRelease();
  passed &= testO05O06AndPriorFrameRelease();
  passed &= testO07ModalTakeoverAndLifecycleReset();
  passed &= testCameraNavigationDragDoesNotReportPointerCapture();
  passed &= testStaleAndMalformedEventsFailClosed();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
