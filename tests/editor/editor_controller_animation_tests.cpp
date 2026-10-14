#include "../../app/editor/EditorController.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace demo::editor;
namespace editor_ui = demo::editor::ui;

std::size_t g_checks = 0;

bool expect(bool condition, std::string_view message) {
  ++g_checks;
  if (!condition)
    std::cerr << "FAILED: " << message << '\n';
  return condition;
}

bool near(double actual, double expected, double tolerance = 1.0e-6) {
  return std::abs(actual - expected) <= tolerance;
}

std::filesystem::path fixture(std::string_view name) {
  return std::filesystem::path(MGIF_TEST_SOURCE_DIR) / "tests" / "fixtures" /
         "gltf_editor" / name;
}

class TempDirectory final {
public:
  TempDirectory() {
    const auto ticks =
        std::chrono::steady_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 100; ++attempt) {
      std::error_code error;
      const std::filesystem::path candidate =
          std::filesystem::temp_directory_path() /
          ("mgif-editor-controller-animation-" + std::to_string(ticks) + "-" +
           std::to_string(attempt));
      if (std::filesystem::create_directory(candidate, error)) {
        m_path = candidate;
        return;
      }
    }
  }

  ~TempDirectory() {
    if (m_path.empty())
      return;
    std::error_code error;
    std::filesystem::remove_all(m_path, error);
  }

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return m_path;
  }

private:
  std::filesystem::path m_path;
};

std::filesystem::path
writeUnsupportedAnimationFixture(const TempDirectory &temporary) {
  if (temporary.path().empty())
    return {};
  const std::filesystem::path payloadPath =
      temporary.path() / "unsupported-animation.bin";
  const std::filesystem::path documentPath =
      temporary.path() / "unsupported-animation.gltf";
  std::array<float, 20> payload{};
  payload[0] = 0.0F;
  payload[1] = 1.0F;
  std::ofstream payloadStream(payloadPath, std::ios::binary | std::ios::trunc);
  payloadStream.write(reinterpret_cast<const char *>(payload.data()),
                      static_cast<std::streamsize>(sizeof(payload)));
  payloadStream.close();
  if (payloadStream.fail())
    return {};

  std::ofstream documentStream(documentPath,
                               std::ios::binary | std::ios::trunc);
  documentStream << R"({
  "asset": { "version": "2.0" },
  "scene": 0,
  "scenes": [{ "nodes": [0] }],
  "nodes": [{ "name": "Cubic Target" }],
  "buffers": [{ "uri": "unsupported-animation.bin", "byteLength": 80 }],
  "bufferViews": [
    { "buffer": 0, "byteOffset": 0, "byteLength": 8 },
    { "buffer": 0, "byteOffset": 8, "byteLength": 72 }
  ],
  "accessors": [
    { "bufferView": 0, "componentType": 5126, "count": 2, "type": "SCALAR", "min": [0], "max": [1] },
    { "bufferView": 1, "componentType": 5126, "count": 6, "type": "VEC3" }
  ],
  "animations": [{
    "name": "Unsupported Cubic",
    "samplers": [{ "input": 0, "output": 1, "interpolation": "CUBICSPLINE" }],
    "channels": [{ "sampler": 0, "target": { "node": 0, "path": "translation" } }]
  }]
})";
  documentStream.close();
  return documentStream.fail() ? std::filesystem::path{} : documentPath;
}

struct ProjectionObservation final {
  editor_ui::ProjectionRefresh refresh{editor_ui::ProjectionRefresh::none};
  std::uint64_t documentRevision{0};
  SceneDigest documentDigest{};
  SceneDigest projectionDigest{};
  std::optional<std::uint32_t> activeAnimation;
  double animationTime{0.0};
  bool animationLoop{true};
};

struct ProjectionObserver final {
  void onProjection(const SceneProjectionResult &projection,
                    editor_ui::ProjectionRefresh refresh) {
    ++projectionCalls;
    lastProjectionAddress = &projection;
    events.push_back(ProjectionObservation{
        refresh,
        projection.documentRevision,
        projection.documentDigest,
        projection.projectionDigest,
        projection.activeAnimation,
        projection.animationTime,
        projection.animationLoop,
    });
  }

  void onPublication(
      const editor_ui::EditorController::ProjectionPublication &publication) {
    ++publicationCalls;
    callbacksConsistent &= publication.projection != nullptr;
    callbacksConsistent &= publication.projection == lastProjectionAddress;
    callbacksConsistent &= !events.empty();
    if (publication.projection != nullptr && !events.empty()) {
      const ProjectionObservation &event = events.back();
      callbacksConsistent &= publication.refresh == event.refresh;
      callbacksConsistent &=
          publication.projection->projectionDigest == event.projectionDigest;
    }
    if (publication.canonical != nullptr)
      ++canonicalPublicationCalls;
  }

  std::size_t projectionCalls{0};
  std::size_t publicationCalls{0};
  std::size_t canonicalPublicationCalls{0};
  bool callbacksConsistent{true};
  const SceneProjectionResult *lastProjectionAddress{nullptr};
  std::vector<ProjectionObservation> events;
};

bool expectCallbackCount(const ProjectionObserver &observer,
                         std::size_t expected, std::string_view scenario) {
  return expect(observer.projectionCalls == expected &&
                    observer.publicationCalls == expected,
                std::string(scenario) +
                    " publishes the expected callback count");
}

editor_ui::RuntimeActionResult
dispatchAnimation(editor_ui::EditorController &controller,
                  std::uint64_t &sequence,
                  editor_ui::AnimationOperation operation,
                  std::optional<AnimationId> animation, double value = 0.0) {
  editor_ui::UiCommand command;
  command.sequence = sequence++;
  command.source = editor_ui::CommandSource::sceneBrowser;
  command.payload = editor_ui::AnimationAction{operation, animation, value};
  return controller.dispatch(command);
}

bool expectProjectionState(const editor_ui::EditorController &controller,
                           std::optional<std::uint32_t> activeAnimation,
                           double time, bool loop, std::string_view scenario) {
  const SceneProjectionResult *projection = controller.projection();
  return expect(projection != nullptr && projection->succeeded() &&
                    projection->activeAnimation == activeAnimation &&
                    near(projection->animationTime, time) &&
                    projection->animationLoop == loop,
                std::string(scenario) +
                    " exposes the expected immutable projection state");
}

bool expectUiState(const editor_ui::EditorController &controller,
                   AnimationId animation, bool playing, double speed,
                   double time, std::string_view scenario) {
  const editor_ui::FrameModel model = controller.buildFrameModel();
  const auto item =
      std::find_if(model.browser.animation.items.begin(),
                   model.browser.animation.items.end(),
                   [&](const editor_ui::AnimationItem &candidate) {
                     return candidate.id == animation;
                   });
  return expect(model.browser.animation.current == animation &&
                    model.browser.animation.playing == playing &&
                    near(model.browser.animation.speed, speed) &&
                    model.browser.animation.runtimeControlsAvailable &&
                    item != model.browser.animation.items.end() &&
                    near(item->currentTime, time),
                std::string(scenario) +
                    " exposes the expected animation UI state");
}

editor_ui::EditorController::Callbacks
makeCallbacks(ProjectionObserver &observer, std::vector<std::string> &errors) {
  editor_ui::EditorController::Callbacks callbacks;
  callbacks.onProjection = [&](const SceneProjectionResult &projection,
                               editor_ui::ProjectionRefresh refresh) {
    observer.onProjection(projection, refresh);
  };
  callbacks.onPublication =
      [&](const editor_ui::EditorController::ProjectionPublication
              &publication) { observer.onPublication(publication); };
  callbacks.onError = [&](std::string_view error) {
    errors.emplace_back(error);
  };
  return callbacks;
}

bool testEditorControllerRuntimeAnimation() {
  ProjectionObserver observer;
  std::vector<std::string> errors;
  editor_ui::EditorController controller(makeCallbacks(observer, errors));

  std::string openError;
  bool passed =
      expect(controller.open(fixture("feature_preservation.gltf"), &openError),
             "animation fixture opens through EditorController");
  passed &= expect(openError.empty() && controller.hasDocument() &&
                       controller.session() != nullptr &&
                       controller.projection() != nullptr,
                   "open publishes a live CPU editor session and projection");
  passed &= expectCallbackCount(observer, 1u, "open");
  if (!controller.session() || !controller.projection())
    return false;

  GltfDocument &document = controller.session()->document();
  const auto animation =
      AnimationId::from(document.idAt(EditorObjectKind::animation, 0u));
  passed &= expect(animation.has_value() &&
                       document.objectCount(EditorObjectKind::animation) == 1u,
                   "fixture exposes one stable animation ID");
  if (!animation)
    return false;

  const std::uint64_t documentRevision = document.revision();
  const SceneDigest documentDigest = document.documentDigest();
  const SceneDigest baselineProjectionDigest =
      controller.projection()->projectionDigest;
  passed &=
      expectProjectionState(controller, 0u, 0.0, true, "baseline");
  passed &= expectUiState(controller, *animation, true, 1.0, 0.0, "baseline");

  std::uint64_t sequence = 1u;
  const editor_ui::RuntimeActionResult selected = dispatchAnimation(
      controller, sequence, editor_ui::AnimationOperation::select, animation);
  passed &=
      expect(selected.accepted &&
                 selected.refresh == editor_ui::ProjectionRefresh::fullScene &&
                 selected.error.empty(),
             "select accepts the stable animation ID");
  passed &= expectCallbackCount(observer, 2u, "select");
  passed &= expectProjectionState(controller, 0u, 0.0, true, "select");
  passed &= expectUiState(controller, *animation, true, 1.0, 0.0, "select");
  passed &= expect(
      controller.projection()->projectionDigest != baselineProjectionDigest,
      "select changes projection identity without editing the document");

  const std::size_t callbacksBeforePause = observer.projectionCalls;
  const editor_ui::RuntimeActionResult paused = dispatchAnimation(
      controller, sequence, editor_ui::AnimationOperation::setPlaying,
      animation, 0.0);
  passed &= expect(paused.accepted &&
                       paused.refresh == editor_ui::ProjectionRefresh::none,
                   "pause updates runtime state without rebuilding projection");
  passed &= expectCallbackCount(observer, callbacksBeforePause, "pause");
  passed &= expectUiState(controller, *animation, false, 1.0, 0.0, "pause");

  const editor_ui::RuntimeActionResult pausedTick =
      controller.updateAnimation(0.25);
  passed &= expect(pausedTick.accepted &&
                       pausedTick.refresh == editor_ui::ProjectionRefresh::none,
                   "paused update is an accepted runtime no-op");
  passed &=
      expectCallbackCount(observer, callbacksBeforePause, "paused update");
  passed &= expectProjectionState(controller, 0u, 0.0, true, "paused update");

  const editor_ui::RuntimeActionResult resumed = dispatchAnimation(
      controller, sequence, editor_ui::AnimationOperation::setPlaying,
      animation, 1.0);
  passed &= expect(resumed.accepted &&
                       resumed.refresh == editor_ui::ProjectionRefresh::none,
                   "play resumes without publishing an unchanged pose");
  passed &= expectCallbackCount(observer, callbacksBeforePause, "play");

  const editor_ui::RuntimeActionResult tick = controller.updateAnimation(0.25);
  passed &= expect(tick.accepted &&
                       tick.refresh == editor_ui::ProjectionRefresh::fullScene,
                   "playing update publishes an advanced pose");
  passed &= expectCallbackCount(observer, callbacksBeforePause + 1u,
                                "playing update");
  passed &= expectProjectionState(controller, 0u, 0.25, true, "playing update");
  passed &=
      expectUiState(controller, *animation, true, 1.0, 0.25, "playing update");
  const auto animatedNode = std::find_if(
      controller.projection()->asset.nodes.begin(),
      controller.projection()->asset.nodes.end(),
      [](const auto &node) { return node.sourceNodeIndex == 0u; });
  passed &= expect(
      animatedNode != controller.projection()->asset.nodes.end() &&
          near(animatedNode->translation.x, 0.25) &&
          near(animatedNode->translation.y, 0.5) &&
          near(animatedNode->translation.z, 0.75),
      "playing update applies the sampled translation to the CPU projection");

  const editor_ui::RuntimeActionResult pausedForStep = dispatchAnimation(
      controller, sequence, editor_ui::AnimationOperation::setPlaying,
      animation, 0.0);
  passed &= expect(pausedForStep.accepted, "pause before step succeeds");
  const std::size_t callbacksBeforeStep = observer.projectionCalls;
  const editor_ui::RuntimeActionResult stepped = dispatchAnimation(
      controller, sequence, editor_ui::AnimationOperation::step, animation);
  constexpr double steppedTime = 0.25 + 1.0 / 60.0;
  passed &=
      expect(stepped.accepted &&
                 stepped.refresh == editor_ui::ProjectionRefresh::fullScene,
             "step advances one fixed frame while playback is paused");
  passed &= expectCallbackCount(observer, callbacksBeforeStep + 1u, "step");
  passed &= expectProjectionState(controller, 0u, steppedTime, true, "step");
  passed &=
      expectUiState(controller, *animation, false, 1.0, steppedTime, "step");

  const std::size_t callbacksBeforeReset = observer.projectionCalls;
  const editor_ui::RuntimeActionResult reset = dispatchAnimation(
      controller, sequence, editor_ui::AnimationOperation::reset, animation);
  passed &= expect(reset.accepted &&
                       reset.refresh == editor_ui::ProjectionRefresh::fullScene,
                   "reset publishes the clip start pose");
  passed &= expectCallbackCount(observer, callbacksBeforeReset + 1u, "reset");
  passed &= expectProjectionState(controller, 0u, 0.0, true, "reset");
  passed &= expectUiState(controller, *animation, false, 1.0, 0.0, "reset");

  const std::array invalidSpeeds{
      -0.01,
      100.01,
      std::numeric_limits<double>::infinity(),
      std::numeric_limits<double>::quiet_NaN(),
  };
  const std::size_t callbacksBeforeSpeed = observer.projectionCalls;
  for (const double invalidSpeed : invalidSpeeds) {
    const editor_ui::RuntimeActionResult rejected = dispatchAnimation(
        controller, sequence, editor_ui::AnimationOperation::setSpeed,
        animation, invalidSpeed);
    passed &= expect(!rejected.accepted && !rejected.error.empty(),
                     "invalid speed is rejected with an explicit error");
  }
  passed &= expectCallbackCount(observer, callbacksBeforeSpeed,
                                "invalid speed validation");
  passed &= expectUiState(controller, *animation, false, 1.0, 0.0,
                          "invalid speed validation");

  const editor_ui::RuntimeActionResult speed = dispatchAnimation(
      controller, sequence, editor_ui::AnimationOperation::setSpeed, animation,
      2.0);
  passed &= expect(
      speed.accepted && speed.refresh == editor_ui::ProjectionRefresh::none,
      "supported speed updates runtime state without a projection");
  passed &=
      expectCallbackCount(observer, callbacksBeforeSpeed, "supported speed");
  passed &=
      expectUiState(controller, *animation, false, 2.0, 0.0, "supported speed");

  std::size_t expectedCallbacks = callbacksBeforeSpeed;
  const editor_ui::RuntimeActionResult clampedLow = dispatchAnimation(
      controller, sequence, editor_ui::AnimationOperation::setTime, animation,
      -50.0);
  ++expectedCallbacks;
  passed &= expect(clampedLow.accepted, "timeline accepts a finite low value");
  passed &=
      expectCallbackCount(observer, expectedCallbacks, "low timeline clamp");
  passed &=
      expectProjectionState(controller, 0u, 0.0, false, "low timeline clamp");

  const editor_ui::RuntimeActionResult clampedHigh = dispatchAnimation(
      controller, sequence, editor_ui::AnimationOperation::setTime, animation,
      50.0);
  ++expectedCallbacks;
  passed &=
      expect(clampedHigh.accepted, "timeline accepts a finite high value");
  passed &=
      expectCallbackCount(observer, expectedCallbacks, "high timeline clamp");
  passed &=
      expectProjectionState(controller, 0u, 1.0, false, "high timeline clamp");
  passed &= expectUiState(controller, *animation, false, 2.0, 1.0,
                          "high timeline clamp");

  const SceneDigest beforeInvalidTime =
      controller.projection()->projectionDigest;
  const editor_ui::RuntimeActionResult invalidTime = dispatchAnimation(
      controller, sequence, editor_ui::AnimationOperation::setTime, animation,
      std::numeric_limits<double>::quiet_NaN());
  passed &= expect(!invalidTime.accepted && !invalidTime.error.empty(),
                   "non-finite timeline input is rejected");
  passed &= expectCallbackCount(observer, expectedCallbacks,
                                "non-finite timeline input");
  passed &=
      expect(controller.projection()->projectionDigest == beforeInvalidTime,
             "invalid timeline input leaves the last projection intact");

  const editor_ui::RuntimeActionResult wrapStart = dispatchAnimation(
      controller, sequence, editor_ui::AnimationOperation::setTime, animation,
      0.75);
  ++expectedCallbacks;
  passed &= expect(wrapStart.accepted, "wrap setup publishes a clamped pose");
  passed &= expectCallbackCount(observer, expectedCallbacks, "wrap setup");
  passed &= expectProjectionState(controller, 0u, 0.75, false, "wrap setup");
  const editor_ui::RuntimeActionResult playingForWrap = dispatchAnimation(
      controller, sequence, editor_ui::AnimationOperation::setPlaying,
      animation, 1.0);
  passed &= expect(playingForWrap.accepted, "play before wrap succeeds");
  const editor_ui::RuntimeActionResult wrapped =
      controller.updateAnimation(0.25);
  ++expectedCallbacks;
  passed &=
      expect(wrapped.accepted &&
                 wrapped.refresh == editor_ui::ProjectionRefresh::fullScene,
             "playing update publishes a wrapped pose");
  passed &= expectCallbackCount(observer, expectedCallbacks, "loop wrap");
  passed &= expectProjectionState(controller, 0u, 0.25, true, "loop wrap");
  passed &= expectUiState(controller, *animation, true, 2.0, 0.25, "loop wrap");

  const SceneDigest beforeInvalidAnimation =
      controller.projection()->projectionDigest;
  const editor_ui::RuntimeActionResult missingAnimation =
      dispatchAnimation(controller, sequence,
                        editor_ui::AnimationOperation::select, std::nullopt);
  passed &=
      expect(!missingAnimation.accepted && !missingAnimation.error.empty(),
             "missing animation selection fails closed");
  passed &= expectCallbackCount(observer, expectedCallbacks,
                                "missing animation selection");

  editor_ui::EditorController foreignController;
  std::string foreignError;
  passed &= expect(foreignController.open(fixture("feature_preservation.gltf"),
                                          &foreignError),
                   "foreign animation fixture opens");
  std::optional<AnimationId> foreignAnimation;
  if (foreignController.session()) {
    foreignAnimation =
        AnimationId::from(foreignController.session()->document().idAt(
            EditorObjectKind::animation, 0u));
  }
  passed &= expect(foreignAnimation.has_value() &&
                       foreignAnimation->authority() != animation->authority(),
                   "foreign animation ID has distinct document authority");
  if (foreignAnimation) {
    const editor_ui::RuntimeActionResult rejectedForeign = dispatchAnimation(
        controller, sequence, editor_ui::AnimationOperation::select,
        foreignAnimation);
    passed &=
        expect(!rejectedForeign.accepted && !rejectedForeign.error.empty(),
               "cross-document animation selection fails closed");
  }
  passed &= expectCallbackCount(observer, expectedCallbacks,
                                "cross-document animation selection");
  passed &= expect(controller.projection()->projectionDigest ==
                       beforeInvalidAnimation,
                   "invalid animation IDs preserve the last valid projection");
  passed &= expectProjectionState(controller, 0u, 0.25, true,
                                  "invalid animation rejection");

  const editor_ui::RuntimeActionResult invalidAdvance =
      controller.updateAnimation(std::numeric_limits<double>::quiet_NaN());
  passed &= expect(!invalidAdvance.accepted && !invalidAdvance.error.empty(),
                   "invalid playback delta fails closed and pauses playback");
  passed &= expectCallbackCount(observer, expectedCallbacks,
                                "invalid playback delta");
  passed &= expectUiState(controller, *animation, false, 2.0, 0.25,
                          "invalid playback delta");
  const editor_ui::RuntimeActionResult tickAfterFailure =
      controller.updateAnimation(0.25);
  passed &=
      expect(tickAfterFailure.accepted &&
                 tickAfterFailure.refresh == editor_ui::ProjectionRefresh::none,
             "playback remains paused after an invalid advance");
  passed &= expectCallbackCount(observer, expectedCallbacks,
                                "post-failure paused update");

  passed &= expect(
      document.revision() == documentRevision &&
          document.documentDigest() == documentDigest &&
          controller.projection()->documentRevision == documentRevision &&
          controller.projection()->documentDigest == documentDigest,
      "all runtime animation controls preserve document revision and digest");
  const bool everyPublicationPreservedAuthority = std::all_of(
      observer.events.begin(), observer.events.end(),
      [&](const ProjectionObservation &event) {
        return event.documentRevision == documentRevision &&
               event.documentDigest == documentDigest &&
               event.refresh == editor_ui::ProjectionRefresh::fullScene;
      });
  passed &= expect(everyPublicationPreservedAuthority,
                   "every callback carries the unchanged document authority");
  passed &= expect(expectedCallbacks == 9u && observer.callbacksConsistent &&
                       observer.canonicalPublicationCalls == 0u,
                   "exactly nine synchronous runtime projections publish "
                   "through both callbacks");
  passed &=
      expect(errors.empty(), "valid projections and explicit command "
                             "rejections emit no controller error callback");

  ProjectionObserver invalidObserver;
  std::vector<std::string> invalidErrors;
  TempDirectory temporary;
  const std::filesystem::path unsupportedFixture =
      writeUnsupportedAnimationFixture(temporary);
  passed &= expect(!unsupportedFixture.empty(),
                   "unsupported animation fixture is generated");
  if (!unsupportedFixture.empty()) {
    editor_ui::EditorController invalidController(
        makeCallbacks(invalidObserver, invalidErrors));
    std::string invalidOpenError;
    passed &=
        expect(invalidController.open(unsupportedFixture, &invalidOpenError),
               "controller opens a structurally valid unsupported clip");
    passed &=
        expect(invalidOpenError.empty() && invalidController.session() &&
                   invalidController.projection(),
               "unsupported clip still has a valid immutable scene projection");
    passed &=
        expectCallbackCount(invalidObserver, 1u, "invalid clip activation");
    if (invalidController.session() && invalidController.projection()) {
      GltfDocument &invalidAuthority = invalidController.session()->document();
      const auto invalidAnimation = AnimationId::from(
          invalidAuthority.idAt(EditorObjectKind::animation, 0u));
      const std::uint64_t invalidRevision = invalidAuthority.revision();
      const SceneDigest invalidDigest = invalidAuthority.documentDigest();
      const SceneDigest invalidProjectionDigest =
          invalidController.projection()->projectionDigest;
      std::uint64_t invalidSequence = 1u;
      const editor_ui::RuntimeActionResult invalidSelection = dispatchAnimation(
          invalidController, invalidSequence,
          editor_ui::AnimationOperation::select, invalidAnimation);
      passed &=
          expect(invalidAnimation.has_value() && !invalidSelection.accepted &&
                     !invalidSelection.error.empty(),
                 "invalid animation data is rejected before sampling");
      passed &= expectCallbackCount(invalidObserver, 1u,
                                    "invalid animation rejection");
      passed &= expect(invalidAuthority.revision() == invalidRevision &&
                           invalidAuthority.documentDigest() == invalidDigest &&
                           invalidController.projection()->documentRevision ==
                               invalidRevision &&
                           invalidController.projection()->documentDigest ==
                               invalidDigest &&
                           invalidController.projection()->projectionDigest ==
                               invalidProjectionDigest,
                       "invalid animation rejection preserves authority and "
                       "projection atomically");
      const editor_ui::FrameModel invalidModel =
          invalidController.buildFrameModel();
      passed &= expect(
          !invalidModel.browser.animation.runtimeControlsAvailable,
          "invalid animation disables runtime controls in the frame model");
      const editor_ui::RuntimeActionResult invalidTick =
          invalidController.updateAnimation(0.25);
      passed &=
          expect(invalidTick.accepted &&
                     invalidTick.refresh == editor_ui::ProjectionRefresh::none,
                 "unselected invalid animation remains a fail-closed no-op");
      passed &=
          expect(invalidErrors.empty(), "invalid animation rejection does not "
                                        "emit an unsolicited error callback");
    }
  }
  return passed;
}

bool testEditorControllerExitRequest() {
  std::size_t exitRequests = 0u;
  editor_ui::EditorController::Callbacks callbacks;
  callbacks.requestExit = [&]() { ++exitRequests; };
  editor_ui::EditorController controller(std::move(callbacks));

  const editor_ui::RuntimeActionResult result = controller.dispatch(
      editor_ui::UiCommand{1u, editor_ui::CommandSource::shortcut,
                           editor_ui::InvokeCommand{editor_ui::CommandId::exit,
                                                    std::nullopt}});
  bool passed = expect(result.accepted && result.error.empty() &&
                           result.refresh == editor_ui::ProjectionRefresh::none,
                       "Exit is accepted without requiring an open document");
  passed &= expect(exitRequests == 1u,
                   "Exit forwards exactly one close request to the app host");
  return passed;
}

bool testEditorControllerP1LifecycleAndViewport() {
  std::size_t resetCallbacks = 0u;
  std::size_t publications = 0u;
  bool viewportControls = false;
  editor_ui::EditorController::Callbacks callbacks;
  callbacks.onDocumentReset = [&]() { ++resetCallbacks; };
  callbacks.onPublication =
      [&](const editor_ui::EditorController::ProjectionPublication &publication) {
        if (publication.projection != nullptr)
          ++publications;
      };
  callbacks.viewportControlsAvailable = [&]() { return viewportControls; };
  editor_ui::EditorController controller(std::move(callbacks));

  std::string error;
  bool passed = expect(
      controller.open(fixture("feature_preservation.gltf"), &error) &&
          error.empty(),
      "P1 lifecycle fixture opens");
  passed &= expect(controller.hasDocument() && resetCallbacks == 0u &&
                       publications == 1u,
                   "opening a document does not invoke the reset callback");

  controller.setViewportInteraction(true, false);
  editor_ui::FrameModel model = controller.buildFrameModel();
  passed &= expect(model.viewportHovered && !model.viewportFocused,
                   "central viewport hover state reaches the frame model");
  controller.setViewportInteraction(false, true);
  model = controller.buildFrameModel();
  passed &= expect(!model.viewportHovered && model.viewportFocused,
                   "central viewport focus state reaches the frame model");
  passed &= expect(!controller.viewportControlsAvailable(),
                   "unconnected viewport capability is fail-closed");

  viewportControls = true;
  passed &= expect(controller.viewportControlsAvailable(),
                   "host can explicitly opt into viewport controls");

  const editor_ui::RuntimeActionResult newScene = controller.dispatch(
      editor_ui::UiCommand{1u, editor_ui::CommandSource::shortcut,
                           editor_ui::InvokeCommand{
                               editor_ui::CommandId::newScene, std::nullopt}});
  passed &= expect(newScene.accepted && !controller.hasDocument() &&
                       resetCallbacks == 1u && publications == 1u,
                   "New Scene clears the document and invokes one reset callback");
  model = controller.buildFrameModel();
  passed &= expect(!model.viewportHovered && !model.viewportFocused &&
                       model.lifecycle == editor_ui::LifecycleState::noDocument,
                   "New Scene clears document-scoped viewport state");
  error.clear();
  passed &= expect(controller.open(fixture("feature_preservation.gltf"),
                                   &error) &&
                       controller.hasDocument() && resetCallbacks == 1u &&
                       publications == 2u,
                   "Open after New publishes one fresh bootstrap document");
  return passed;
}

bool testEditorControllerP1AssignmentAndShortcutContracts() {
  editor_ui::EditorController::Callbacks callbacks;
  callbacks.viewportControlsAvailable = []() { return true; };
  editor_ui::EditorController controller(std::move(callbacks));
  std::string error;
  bool passed = expect(
      controller.open(fixture("feature_preservation.gltf"), &error),
      "assignment contract fixture opens");
  passed &= expect(
      editor_ui::commandDescriptor(editor_ui::CommandId::importMerge).label ==
          "Import/Merge Scene...",
      "Import/Merge Scene menu label retains the reference ellipsis");
  if (!controller.hasDocument())
    return false;

  const editor_ui::RuntimeActionResult focusOnly = controller.dispatch(
      editor_ui::UiCommand{
          1u, editor_ui::CommandSource::sceneBrowser,
          editor_ui::MaterialAction{editor_ui::MaterialOperation::focusAssignment,
                                    std::nullopt, std::nullopt, {}}});
  passed &= expect(!focusOnly.accepted && !focusOnly.error.empty(),
                   "focusAssignment without a primitive is not an accepted no-op");

  const editor_ui::FrameModel model = controller.buildFrameModel();
  const auto primitiveRow = std::find_if(
      model.browser.graphRows.begin(), model.browser.graphRows.end(),
      [](const editor_ui::GraphRow &row) {
        return row.kind == editor_ui::GraphRowKind::primitive;
      });
  passed &= expect(primitiveRow != model.browser.graphRows.end(),
                   "fixture exposes a primitive graph row");
  if (primitiveRow != model.browser.graphRows.end()) {
    const auto context = editor_ui::SceneEditorUi::primitiveAssignmentContext(
        model, *primitiveRow);
    const auto *selection =
        std::get_if<PrimitiveSelection>(&primitiveRow->selection.value);
    passed &= expect(context.has_value() && selection != nullptr &&
                         context->scene == selection->scene &&
                         context->node == selection->node &&
                         context->mesh == selection->mesh &&
                         context->primitive == selection->primitive &&
                         context->canAssign,
                     "primitive assignment context preserves the clicked row occurrence");
  }

  editor_ui::FrameModel hovered = model;
  hovered.viewportHovered = true;
  passed &= expect(editor_ui::SceneEditorUi::viewportShortcutEligible(
                       hovered, false, false, true, true),
                   "G/T shortcut is eligible for a hovered viewport");
  hovered.viewportHovered = false;
  hovered.viewportFocused = true;
  passed &= expect(editor_ui::SceneEditorUi::viewportShortcutEligible(
                       hovered, false, false, true, true),
                   "G/T shortcut is eligible for a focused viewport");
  hovered.viewportFocused = false;
  passed &= expect(!editor_ui::SceneEditorUi::viewportShortcutEligible(
                       hovered, false, false, true, true),
                   "G/T shortcut is blocked without viewport hover/focus");
  hovered.viewportHovered = true;
  passed &= expect(!editor_ui::SceneEditorUi::viewportShortcutEligible(
                       hovered, false, true, true, true),
                   "G/T shortcut yields to text input");
  passed &= expect(!editor_ui::SceneEditorUi::viewportShortcutEligible(
                       hovered, true, false, true, true),
                   "G/T shortcut yields to modal state");
  passed &= expect(!editor_ui::SceneEditorUi::viewportShortcutEligible(
                       hovered, false, false, true, false),
                   "G/T shortcut is disabled when the viewport seam is unavailable");
  passed &= expect(
      !editor_ui::SceneEditorUi::viewportCommandAvailable(
          editor_ui::CommandId::toggleGrid, false) &&
          !editor_ui::SceneEditorUi::viewportCommandAvailable(
              editor_ui::CommandId::toggleGizmo, false) &&
          !editor_ui::SceneEditorUi::viewportCommandAvailable(
              editor_ui::CommandId::toggleSnap, false) &&
          !editor_ui::SceneEditorUi::viewportCommandAvailable(
              editor_ui::CommandId::showGridSnapSettings, false),
      "unconnected grid, gizmo, snap and settings controls are all disabled");
  passed &= expect(editor_ui::SceneEditorUi::viewportCommandAvailable(
                       editor_ui::CommandId::fitScene, false),
                   "viewport capability gating does not disable unrelated commands");
  return passed;
}

bool testEditorControllerCrossSceneRootDropTarget() {
  ProjectionObserver observer;
  std::vector<std::string> errors;
  editor_ui::EditorController controller(makeCallbacks(observer, errors));
  std::string openError;
  bool passed = expect(
      controller.open(fixture("multi_scene_oracle.gltf"), &openError) &&
          openError.empty() && controller.session() != nullptr,
      "cross-scene root-drop fixture opens through EditorController");
  if (!controller.session())
    return false;

  GltfDocument &document = controller.session()->document();
  const SceneId sourceScene = *SceneId::from(
      document.idAt(EditorObjectKind::scene, 0u));
  const SceneId targetScene = *SceneId::from(
      document.idAt(EditorObjectKind::scene, 1u));
  const NodeId sourceNode = *NodeId::from(
      document.idAt(EditorObjectKind::node, 1u));
  const auto source = controller.session()->editor().commands()
                          .captureNodeOccurrence(sourceScene, sourceNode);
  if (!source)
    return expect(false, "cross-scene root-drop source occurrence captures");
  const auto sourceRootsBefore = document.sceneRootNodeIds(sourceScene);
  const auto targetRootsBefore = document.sceneRootNodeIds(targetScene);

  const editor_ui::RuntimeActionResult result = controller.dispatch(
      editor_ui::UiCommand{
          1u, editor_ui::CommandSource::sceneBrowser,
          editor_ui::ReparentNode{*source, targetScene, std::nullopt}});
  passed &= expect(result.accepted && result.error.empty(),
                   "controller accepts a cross-scene root drop");
  auto expectedSourceRoots = sourceRootsBefore;
  expectedSourceRoots.erase(
      std::remove(expectedSourceRoots.begin(), expectedSourceRoots.end(),
                  sourceNode),
      expectedSourceRoots.end());
  auto expectedTargetRoots = targetRootsBefore;
  expectedTargetRoots.push_back(sourceNode);
  passed &= expect(
      document.sceneRootNodeIds(sourceScene) == expectedSourceRoots &&
          document.sceneRootNodeIds(targetScene) == expectedTargetRoots,
      "controller routes the root drop to its explicit target scene");
  passed &= expect(errors.empty(),
                   "cross-scene root drop emits no controller error");
  return passed;
}

} // namespace

int main() {
  const bool passed = testEditorControllerRuntimeAnimation() &&
                      testEditorControllerExitRequest() &&
                      testEditorControllerP1LifecycleAndViewport() &&
                      testEditorControllerP1AssignmentAndShortcutContracts() &&
                      testEditorControllerCrossSceneRootDropTarget();
  if (!passed)
    return 1;
  std::cout << "EditorController animation tests passed (" << g_checks
            << " checks)\n";
  return 0;
}
