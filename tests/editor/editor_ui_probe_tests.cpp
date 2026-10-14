#include "../../app/editor/EditorController.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace demo::editor;
namespace ui = demo::editor::ui;

constexpr EditorDocumentAuthority kAuthority{0x50524f4245554932ULL};

EditorObjectId rawId(EditorObjectKind kind, std::uint32_t slot) {
  return EditorObjectId{kind, slot, 1u, kAuthority};
}

template <EditorObjectKind Kind>
TypedEditorObjectId<Kind> typedId(std::uint32_t slot) {
  const auto value = TypedEditorObjectId<Kind>::from(rawId(Kind, slot));
  if (!value)
    std::abort();
  return *value;
}

void check(bool condition, std::string_view message) {
  if (!condition)
    throw std::runtime_error(std::string(message));
}

struct FakeHost final : ui::SceneEditorUiHost {
  std::optional<std::filesystem::path>
  nativeFileDialog(const ui::NativeDialogRequest &) override {
    return std::nullopt;
  }

  std::string clipboardText() const override { return {}; }
  bool setClipboardText(std::string_view) override { return true; }

  ui::RuntimeActionResult dispatch(const ui::UiCommand &command) override {
    commands.push_back(command);
    return dispatchResult;
  }

  ui::RuntimeActionResult cameraRequest(const ui::CameraRequest &) override {
    return {false, ui::ProjectionRefresh::none, "camera unavailable"};
  }

  ui::NodeDropValidation
  validateNodeDrop(const ui::ReparentNode &) const override {
    ++dropValidationCount;
    return dropValidation;
  }

  ui::ViewportSettings viewportSettings() const override { return viewport; }
  void setViewportSettings(const ui::ViewportSettings &value) override {
    viewport = value;
  }
  bool viewportControlsAvailable() const noexcept override { return true; }
  void refreshProjection(ui::ProjectionRefresh) override {}

  ui::ViewportSettings viewport;
  ui::RuntimeActionResult dispatchResult{
      false, ui::ProjectionRefresh::hierarchy, "probe dispatch result"};
  ui::NodeDropValidation dropValidation{true, {}};
  mutable std::size_t dropValidationCount{0};
  std::vector<ui::UiCommand> commands;
};

struct CapturedItem final {
  ui::EditorUiProbeItemKey key;
  std::string label;
  std::uint64_t order{0};
  std::uint32_t itemId{0};
  std::uint32_t windowToken{0};
  std::uint32_t popupToken{0};
  std::uint32_t modalToken{0};
  ui::EditorUiProbeCoordinateSpace rectCoordinateSpace{
      ui::EditorUiProbeCoordinateSpace::imguiScreenLogical};
  ui::EditorUiProbeRect rect;
  bool visible{false};
  bool hovered{false};
  bool active{false};
  bool focused{false};
  bool enabled{false};
  bool dropAccepted{false};
};

struct CapturedDispatch final {
  ui::UiCommand command;
  ui::RuntimeActionResult result;
};

struct CapturedFrame final {
  ui::EditorUiProbeFrameBinding binding;
  std::vector<CapturedItem> items;
  std::vector<CapturedDispatch> dispatches;
  bool ended{false};
};

class FailClosedProbe final : public ui::EditorUiProbe {
public:
  void requireOnNextFrame(ui::EditorUiProbeItemKey key) {
    nextRequired.push_back(std::move(key));
  }

  void beginFrame(const ui::EditorUiProbeFrameBinding &binding) override {
    if (active)
      fail("nested probe frame");
    active = true;
    frames.push_back(CapturedFrame{.binding = binding});
    required = std::move(nextRequired);
    nextRequired.clear();
    if (binding.frameSerial == 0u || binding.mainViewportId == 0u ||
        binding.viewportSize.x <= 0.0F || binding.viewportSize.y <= 0.0F ||
        binding.displaySize.x <= 0.0F || binding.displaySize.y <= 0.0F ||
        binding.framebufferScale.x <= 0.0F ||
        binding.framebufferScale.y <= 0.0F) {
      fail("invalid frame coordinate binding");
    }
  }

  void observeItem(const ui::EditorUiProbeItem &item) override {
    if (!active || frames.empty()) {
      fail("item outside probe frame");
      return;
    }
    CapturedFrame &frame = frames.back();
    if (!item.valid() || item.windowToken == 0u || item.label.empty() ||
        item.rectCoordinateSpace !=
            ui::EditorUiProbeCoordinateSpace::imguiScreenLogical) {
      fail("invalid UI item observation");
    }
    if (item.order != frame.items.size() + 1u)
      fail("non-monotonic item order");
    if (std::any_of(frame.items.begin(), frame.items.end(),
                    [&](const CapturedItem &captured) {
                      return captured.key == item.key;
                    })) {
      fail("duplicate semantic item key");
    }
    frame.items.push_back(CapturedItem{
        .key = item.key,
        .label = std::string(item.label),
        .order = item.order,
        .itemId = item.itemId,
        .windowToken = item.windowToken,
        .popupToken = item.popupToken,
        .modalToken = item.modalToken,
        .rectCoordinateSpace = item.rectCoordinateSpace,
        .rect = item.rect,
        .visible = item.visible,
        .hovered = item.hovered,
        .active = item.active,
        .focused = item.focused,
        .enabled = item.enabled,
        .dropAccepted = item.dropAccepted,
    });
  }

  void observeDispatch(const ui::UiCommand &command,
                       const ui::RuntimeActionResult &result) override {
    if (!active || frames.empty()) {
      fail("dispatch outside probe frame");
      return;
    }
    frames.back().dispatches.push_back({command, result});
  }

  void endFrame() override {
    if (!active || frames.empty()) {
      fail("end outside probe frame");
      return;
    }
    for (const ui::EditorUiProbeItemKey &key : required) {
      if (std::none_of(frames.back().items.begin(), frames.back().items.end(),
                       [&](const CapturedItem &item) {
                         return item.key == key;
                       })) {
        fail("required semantic item is missing");
      }
    }
    frames.back().ended = true;
    required.clear();
    active = false;
  }

  [[nodiscard]] const CapturedItem *latestItem(
      ui::EditorUiProbeSemantic semantic,
      const ui::GraphRowKey &row = {}) const {
    for (auto frame = frames.rbegin(); frame != frames.rend(); ++frame) {
      const auto found = std::find_if(
          frame->items.rbegin(), frame->items.rend(),
          [&](const CapturedItem &item) {
            return item.key.semantic == semantic &&
                   (row.object.valid() ? item.key.row == row : true);
          });
      if (found != frame->items.rend())
        return &*found;
    }
    return nullptr;
  }

  [[nodiscard]] const CapturedItem *currentFrameItem(
      ui::EditorUiProbeSemantic semantic,
      const ui::GraphRowKey &row = {}) const {
    if (frames.empty())
      return nullptr;
    const auto found = std::find_if(
        frames.back().items.rbegin(), frames.back().items.rend(),
        [&](const CapturedItem &item) {
          return item.key.semantic == semantic &&
                 (row.object.valid() ? item.key.row == row : true);
        });
    return found == frames.back().items.rend() ? nullptr : &*found;
  }

  [[nodiscard]] bool failed() const noexcept { return !failure.empty(); }
  [[nodiscard]] std::string_view failureMessage() const noexcept {
    return failure;
  }

  std::vector<CapturedFrame> frames;

private:
  void fail(std::string_view message) {
    if (failure.empty())
      failure.assign(message);
  }

  bool active{false};
  std::string failure;
  std::vector<ui::EditorUiProbeItemKey> nextRequired;
  std::vector<ui::EditorUiProbeItemKey> required;
};

struct ImGuiScope final {
  ImGuiScope() {
    context = ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280.0F, 720.0F);
    io.DisplayFramebufferScale = ImVec2(1.5F, 2.0F);
    io.DeltaTime = 1.0F / 60.0F;
    io.AddMousePosEvent(-10000.0F, -10000.0F);
    ImGui::StyleColorsDark();
    unsigned char *pixels = nullptr;
    int width = 0;
    int height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
  }

  ~ImGuiScope() { ImGui::DestroyContext(context); }

  ImGuiContext *context{nullptr};
};

struct ModelFixture final {
  ui::FrameModel model;
  ui::GraphRowKey sceneRow;
  ui::GraphRowKey rootRow;
  ui::GraphRowKey sharedChildRow;
  ui::GraphRowKey repeatedChildRow;
};

ModelFixture makeModel() {
  const SceneId scene = typedId<EditorObjectKind::scene>(0u);
  const NodeId root = typedId<EditorObjectKind::node>(0u);
  const NodeId sharedChild = typedId<EditorObjectKind::node>(1u);

  ModelFixture fixture;
  fixture.sceneRow = {scene, scene.untyped(), std::nullopt, 0u};
  fixture.rootRow = {scene, root.untyped(), std::nullopt, 0u};
  fixture.sharedChildRow = {scene, sharedChild.untyped(), root, 0u};
  fixture.repeatedChildRow = {scene, sharedChild.untyped(), std::nullopt, 1u};

  ui::FrameModel &model = fixture.model;
  model.lifecycle = ui::LifecycleState::readyModified;
  model.lifecycleRevision = 17u;
  model.hasDocument = true;
  model.mutationEnabled = true;
  model.revision = 41u;
  model.cleanRevision = 40u;
  model.selectionRevision = 9u;
  model.canUndo = true;
  model.canRedo = true;
  model.scenes.push_back({scene, 0u, "Probe Scene", true});
  model.selection.authority = kAuthority;
  model.selection.value = NodeSelection{scene, root, std::nullopt};

  const auto append = [&](ui::GraphRowKey key, ui::GraphRowKind kind,
                          std::uint32_t index, std::uint32_t depth,
                          std::string label, bool hasChildren,
                          bool selectable) {
    ui::GraphRow row;
    row.key = key;
    row.kind = kind;
    row.documentIndex = index;
    row.depth = depth;
    row.label = std::move(label);
    row.hasChildren = hasChildren;
    row.defaultExpanded = true;
    row.selectable = selectable;
    row.enabled = true;
    row.acceptsNodeDrop = kind == ui::GraphRowKind::scene ||
                          kind == ui::GraphRowKind::node;
    if (selectable) {
      row.selection.authority = kAuthority;
      row.selection.value = NodeSelection{
          key.scene, *NodeId::from(key.object), key.directParent};
    }
    model.browser.graphRows.push_back(std::move(row));
  };
  append(fixture.sceneRow, ui::GraphRowKind::scene, 0u, 0u,
         "Probe Scene", true, false);
  append(fixture.rootRow, ui::GraphRowKind::node, 0u, 1u, "Root", true,
         true);
  append(fixture.sharedChildRow, ui::GraphRowKind::node, 1u, 2u,
         "Shared Child", false, true);
  append(fixture.repeatedChildRow, ui::GraphRowKind::node, 1u, 1u,
         "Shared Child", false, true);
  return fixture;
}

ui::RenderResult renderFrame(ui::SceneEditorUi &editor, FakeHost &host,
                             const ui::FrameModel &model,
                             ui::EditorUiProbe *probe) {
  ImGui::NewFrame();
  const ui::RenderResult result = editor.render(model, host, probe);
  ImGui::Render();
  return result;
}

ui::RenderResult renderForcedDragFrame(
    ui::SceneEditorUi &editor, FakeHost &host, const ui::FrameModel &model,
    ui::EditorUiProbe *probe, std::uint32_t sourceItemId,
    const ui::EditorUiProbeRect &targetRect) {
  ImGuiIO &io = ImGui::GetIO();
  io.AddMousePosEvent((targetRect.minimumX + targetRect.maximumX) * 0.5F,
                      (targetRect.minimumY + targetRect.maximumY) * 0.5F);
  io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
  ImGui::NewFrame();
  ImGui::SetActiveID(sourceItemId, ImGui::FindWindowByName("Scene Browser"));
  GImGui->ActiveIdMouseButton = ImGuiMouseButton_Left;
  io.MouseDown[ImGuiMouseButton_Left] = true;
  io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] =
      (io.MouseDragThreshold + 10.0F) * (io.MouseDragThreshold + 10.0F);
  const ui::RenderResult result = editor.render(model, host, probe);
  ImGui::Render();
  return result;
}

void queueMouse(const ui::EditorUiProbeRect &rect, int button, bool down) {
  ImGuiIO &io = ImGui::GetIO();
  io.AddMousePosEvent((rect.minimumX + rect.maximumX) * 0.5F,
                      (rect.minimumY + rect.maximumY) * 0.5F);
  io.AddMouseButtonEvent(button, down);
}

void queueMousePosition(const ui::EditorUiProbeRect &rect) {
  ImGui::GetIO().AddMousePosEvent(
      (rect.minimumX + rect.maximumX) * 0.5F,
      (rect.minimumY + rect.maximumY) * 0.5F);
}

void testFailClosedConsumer() {
  const SceneId scene = typedId<EditorObjectKind::scene>(0u);
  const NodeId node = typedId<EditorObjectKind::node>(0u);
  const ui::GraphRowKey row{scene, node.untyped(), std::nullopt, 0u};
  const ui::EditorUiProbeItemKey key{
      ui::EditorUiProbeSemantic::graphRow, row, node.untyped()};
  const ui::EditorUiProbeFrameBinding binding{
      .lifecycle = ui::LifecycleState::readyClean,
      .lifecycleRevision = 1u,
      .documentRevision = 2u,
      .selectionRevision = 3u,
      .frameSerial = 4u,
      .mainViewportId = 5u,
      .viewportSize = {100.0F, 100.0F},
      .displaySize = {100.0F, 100.0F},
      .framebufferScale = {1.0F, 1.0F},
  };
  const ui::EditorUiProbeItem valid{
      .key = key,
      .label = "Root",
      .order = 1u,
      .itemId = 7u,
      .windowToken = 8u,
      .rect = {1.0F, 2.0F, 20.0F, 22.0F},
      .visible = true,
      .enabled = true,
  };

  FailClosedProbe missing;
  missing.requireOnNextFrame(key);
  missing.beginFrame(binding);
  missing.endFrame();
  check(missing.failed(), "missing semantic did not fail closed");

  FailClosedProbe duplicate;
  duplicate.beginFrame(binding);
  duplicate.observeItem(valid);
  ui::EditorUiProbeItem second = valid;
  second.order = 2u;
  duplicate.observeItem(second);
  duplicate.endFrame();
  check(duplicate.failed(), "duplicate semantic did not fail closed");

  FailClosedProbe invalidRect;
  invalidRect.beginFrame(binding);
  ui::EditorUiProbeItem invalid = valid;
  invalid.rect.maximumX = invalid.rect.minimumX;
  invalidRect.observeItem(invalid);
  invalidRect.endFrame();
  check(invalidRect.failed(), "invalid item rect did not fail closed");
}

void testGraphBindingAndDispatchObservation() {
  ImGuiScope imgui;
  FakeHost host;
  ui::SceneEditorUi editor;
  ui::PersistentState state;
  state.windows.sceneBrowser = true;
  state.windows.inspector = false;
  editor.setState(state);
  const ModelFixture fixture = makeModel();
  FailClosedProbe probe;
  for (const ui::GraphRowKey &row :
       {fixture.sceneRow, fixture.rootRow, fixture.sharedChildRow,
        fixture.repeatedChildRow}) {
    probe.requireOnNextFrame(
        {ui::EditorUiProbeSemantic::graphRow, row, row.object});
    probe.requireOnNextFrame(
        {ui::EditorUiProbeSemantic::hierarchyDropTarget, row, row.object});
    if (row.object.kind == EditorObjectKind::node) {
      probe.requireOnNextFrame(
          {ui::EditorUiProbeSemantic::nodeDragSource, row, row.object});
    }
  }

  (void)renderFrame(editor, host, fixture.model, &probe);
  check(!probe.failed(), probe.failureMessage());
  check(probe.frames.size() == 1u && probe.frames.front().ended,
        "probe frame was not closed");
  const ui::EditorUiProbeFrameBinding &binding = probe.frames.front().binding;
  check(binding.lifecycle == fixture.model.lifecycle &&
            binding.lifecycleRevision == fixture.model.lifecycleRevision &&
            binding.documentRevision == fixture.model.revision &&
            binding.selectionRevision == fixture.model.selectionRevision,
        "frame did not bind lifecycle/document/selection revisions");
  check(binding.viewportSize == ui::EditorUiProbePoint{1280.0F, 720.0F} &&
            binding.displaySize == ui::EditorUiProbePoint{1280.0F, 720.0F} &&
            binding.framebufferScale == ui::EditorUiProbePoint{1.5F, 2.0F},
        "frame did not preserve logical/display/framebuffer coordinates");

  const CapturedItem *firstOccurrence = probe.latestItem(
      ui::EditorUiProbeSemantic::graphRow, fixture.sharedChildRow);
  const CapturedItem *secondOccurrence = probe.latestItem(
      ui::EditorUiProbeSemantic::graphRow, fixture.repeatedChildRow);
  const CapturedItem *idleDropTarget = probe.latestItem(
      ui::EditorUiProbeSemantic::hierarchyDropTarget,
      fixture.repeatedChildRow);
  check(firstOccurrence != nullptr && secondOccurrence != nullptr &&
            idleDropTarget != nullptr && !idleDropTarget->dropAccepted &&
            firstOccurrence->visible && secondOccurrence->visible &&
            firstOccurrence->enabled && secondOccurrence->enabled &&
            firstOccurrence->label == " [1] [N] Shared Child" &&
            secondOccurrence->label == " [1] [N] Shared Child" &&
            firstOccurrence->itemId != secondOccurrence->itemId &&
            firstOccurrence->key.row == fixture.sharedChildRow &&
            secondOccurrence->key.row == fixture.repeatedChildRow,
        "graph observations did not preserve exact occurrence keys");

  ImGuiIO &io = ImGui::GetIO();
  io.AddKeyEvent(ImGuiMod_Ctrl, true);
  io.AddKeyEvent(ImGuiKey_D, true);
  const ui::RenderResult dispatched =
      renderFrame(editor, host, fixture.model, &probe);
  io.AddKeyEvent(ImGuiKey_D, false);
  io.AddKeyEvent(ImGuiMod_Ctrl, false);
  check(dispatched.submittedCommandCount == 1u && host.commands.size() == 1u &&
            probe.frames.back().dispatches.size() == 1u,
        "shortcut dispatch was not observed exactly once");
  const CapturedDispatch &observation = probe.frames.back().dispatches.front();
  const auto *invoke = std::get_if<ui::InvokeCommand>(&observation.command.payload);
  check(invoke != nullptr &&
            invoke->id == ui::CommandId::duplicateSelection &&
            observation.command.sequence == host.commands.front().sequence &&
            observation.command.source == host.commands.front().source &&
            observation.result.accepted == host.dispatchResult.accepted &&
            observation.result.refresh == host.dispatchResult.refresh &&
            observation.result.error == host.dispatchResult.error,
        "probe dispatch differs from the actual command/result pair");

  const std::size_t observedFrames = probe.frames.size();
  const std::size_t dispatchedCommands = host.commands.size();
  (void)renderFrame(editor, host, fixture.model, nullptr);
  check(probe.frames.size() == observedFrames &&
            host.commands.size() == dispatchedCommands,
        "default null probe changed production rendering or emitted callbacks");
}

void testNativeViewportEscapeOwnership() {
  for (const bool viewportOwner : {false, true}) {
    for (const bool modal : {false, true}) {
      ImGuiScope imgui;
      FakeHost host;
      host.dispatchResult = {true, ui::ProjectionRefresh::selectionOnly, {}};
      ui::SceneEditorUi editor;
      ui::PersistentState state;
      state.windows.inspector = false;
      editor.setState(state);
      ModelFixture fixture = makeModel();
      check(!fixture.model.viewportOwnsEscape,
            "viewport Escape ownership must default to false");
      // The shared dragging lifecycle can also represent Inspector editing.
      fixture.model.lifecycle = modal ? ui::LifecycleState::modal
                                      : ui::LifecycleState::gizmoDragging;
      fixture.model.modalPending = modal;
      fixture.model.viewportOwnsEscape = viewportOwner;
      FailClosedProbe probe;
      (void)renderFrame(editor, host, fixture.model, &probe);
      ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
      const auto result = renderFrame(editor, host, fixture.model, &probe);
      const std::size_t expected = !modal && !viewportOwner ? 1u : 0u;
      check(result.submittedCommandCount == expected &&
                host.commands.size() == expected &&
                probe.frames.back().dispatches.size() == expected,
            "Escape UI dispatch did not respect explicit viewport ownership and modal priority");
      if (expected != 0u) {
        const auto *selection = std::get_if<ui::SelectObject>(
            &host.commands.front().payload);
        check(selection != nullptr && selection->selection.emptyValue() &&
                  host.commands.front().source == ui::CommandSource::shortcut,
              "non-native Escape stopped issuing the ordinary clear-selection command");
      }
      ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
      (void)renderFrame(editor, host, fixture.model, &probe);
      check(host.commands.size() == expected && !probe.failed(),
            "Escape key release emitted another UI dispatch or invalid probe frame");
    }
  }
}

void testTrickledNativeEscapeIsConsumedExactlyOnce() {
  for (const bool nativeHandled : {false, true}) {
    ImGuiScope imgui;
    FakeHost host;
    host.dispatchResult = {true, ui::ProjectionRefresh::selectionOnly, {}};
    ui::SceneEditorUi editor;
    ui::PersistentState state;
    state.windows.sceneBrowser = false;
    state.windows.inspector = false;
    editor.setState(state);
    ModelFixture fixture = makeModel();
    fixture.model.viewportOwnsEscape = true;
    FailClosedProbe probe;
    ImGuiIO &io = ImGui::GetIO();
    check(io.ConfigInputTrickleEventQueue,
          "native Escape regression requires the production trickle setting");
    (void)renderFrame(editor, host, fixture.model, &probe);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    (void)renderFrame(editor, host, fixture.model, &probe);
    check(ImGui::IsMouseDown(ImGuiMouseButton_Left),
          "native Escape regression did not establish real queued mouse-down");

    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    io.AddKeyEvent(ImGuiKey_Escape, true);
    io.AddKeyEvent(ImGuiKey_Escape, false);
    if (nativeHandled)
      editor.noteViewportEscapeHandled();
    (void)renderFrame(editor, host, fixture.model, &probe);
    check(ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
              !ImGui::IsKeyPressed(ImGuiKey_Escape, false) &&
              host.commands.empty(),
          "release-first native batch did not actually defer ImGui Escape");
    fixture.model.viewportOwnsEscape = false;
    (void)renderFrame(editor, host, fixture.model, &probe);
    const std::size_t delayedCommands = nativeHandled ? 0u : 1u;
    check(ImGui::IsKeyPressed(ImGuiKey_Escape, false) &&
              host.commands.size() == delayedCommands &&
              probe.frames.back().dispatches.size() == delayedCommands,
          "delayed ImGui Escape duplicated an acknowledged native terminal");
    (void)renderFrame(editor, host, fixture.model, &probe);
    check(!ImGui::IsKeyDown(ImGuiKey_Escape) &&
              host.commands.size() == delayedCommands,
          "trickled Escape key-up emitted another UI command");
    io.AddKeyEvent(ImGuiKey_Escape, true);
    (void)renderFrame(editor, host, fixture.model, &probe);
    check(host.commands.size() == delayedCommands + 1u,
          "acknowledging native Escape suppressed a later independent Escape");
    const auto *clear = std::get_if<ui::SelectObject>(&host.commands.back().payload);
    check(clear != nullptr && clear->selection.emptyValue() &&
              host.commands.back().source == ui::CommandSource::shortcut,
          "independent Escape did not retain the ordinary clear-selection command");
    io.AddKeyEvent(ImGuiKey_Escape, false);
    (void)renderFrame(editor, host, fixture.model, &probe);
    check(!probe.failed(), probe.failureMessage());
  }
}

void testNativeEscapeAcknowledgmentSurvivesDetachAndModal() {
  for (const bool modal : {false, true}) {
    ImGuiScope imgui;
    FakeHost host;
    host.dispatchResult = {true, ui::ProjectionRefresh::selectionOnly, {}};
    ui::SceneEditorUi editor;
    const ModelFixture fixture = makeModel();
    FailClosedProbe probe;
    (void)renderFrame(editor, host, fixture.model, &probe);
    editor.noteViewportEscapeHandled();
    ui::FrameModel detached = fixture.model;
    detached.selection = EditorSelectionSnapshot::empty();
    ++detached.selectionRevision;
    (void)renderFrame(editor, host, detached, &probe);
    ui::FrameModel delayed = fixture.model;
    if (modal) {
      delayed.lifecycle = ui::LifecycleState::modal;
      delayed.modalPending = true;
    }
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
    (void)renderFrame(editor, host, delayed, &probe);
    check(ImGui::IsKeyPressed(ImGuiKey_Escape, false) && host.commands.empty(),
          "selection detach discarded the pending native Escape acknowledgment");
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
    (void)renderFrame(editor, host, fixture.model, &probe);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
    (void)renderFrame(editor, host, fixture.model, &probe);
    check(host.commands.size() == 1u,
          "modal early-return failed to consume its matched native Escape marker");
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
    (void)renderFrame(editor, host, fixture.model, &probe);
    check(!probe.failed(), probe.failureMessage());
  }
}

void testNativeEscapeAcknowledgmentsCountAndReset() {
  for (const int resetMode : {0, 1, 2, 3}) {
    ImGuiScope imgui;
    FakeHost host;
    host.dispatchResult = {true, ui::ProjectionRefresh::selectionOnly, {}};
    ui::SceneEditorUi editor;
    const ModelFixture fixture = makeModel();
    FailClosedProbe probe;
    (void)renderFrame(editor, host, fixture.model, &probe);
    editor.noteViewportEscapeHandled();
    if (resetMode == 0) {
      editor.noteViewportEscapeHandled();
      for (int index = 0; index < 2; ++index) {
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
      }
      std::size_t observedPresses = 0u;
      for (int frame = 0; frame < 4; ++frame) {
        (void)renderFrame(editor, host, fixture.model, &probe);
        observedPresses += ImGui::IsKeyPressed(ImGuiKey_Escape, false) ? 1u : 0u;
      }
      check(observedPresses == 2u && host.commands.empty(),
            "multiple actual native Escape presses were collapsed to one marker");
    } else if (resetMode == 1) {
      editor.discardViewportEscapeHandling();
    } else if (resetMode == 2) {
      ImGui::GetIO().AddFocusEvent(false);
      (void)renderFrame(editor, host, fixture.model, &probe);
      ImGui::GetIO().AddFocusEvent(true);
      (void)renderFrame(editor, host, fixture.model, &probe);
    } else {
      editor.resetDocumentState();
    }
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
    (void)renderFrame(editor, host, fixture.model, &probe);
    check(host.commands.size() == 1u,
          "consumed or discarded native acknowledgment suppressed an unrelated Escape");
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
    (void)renderFrame(editor, host, fixture.model, &probe);
    check(!probe.failed(), probe.failureMessage());
  }
}

void testDropAcceptanceObservation() {
  for (const bool accepted : {false, true}) {
    ImGuiScope imgui;
    FakeHost host;
    host.dropValidation = {
        accepted, accepted ? std::string{} : std::string{"rejected target"}};
    ui::SceneEditorUi editor;
    ui::PersistentState state;
    state.windows.inspector = false;
    editor.setState(state);
    const ModelFixture fixture = makeModel();
    FailClosedProbe probe;
    (void)renderFrame(editor, host, fixture.model, &probe);
    const CapturedItem *source = probe.latestItem(
        ui::EditorUiProbeSemantic::graphRow, fixture.rootRow);
    const CapturedItem *target = probe.latestItem(
        ui::EditorUiProbeSemantic::graphRow, fixture.repeatedChildRow);
    check(source != nullptr && target != nullptr,
          "drag source or target row is missing");
    const ui::EditorUiProbeRect targetRect = target->rect;

    (void)renderForcedDragFrame(editor, host, fixture.model, &probe,
                                source->itemId, targetRect);
    const CapturedItem *observed = probe.latestItem(
        ui::EditorUiProbeSemantic::hierarchyDropTarget,
        fixture.repeatedChildRow);
    check(host.dropValidationCount != 0u,
          "drag did not reach host drop validation");
    check(observed != nullptr && observed->dropAccepted == accepted,
          accepted ? "accepted drop target was not observed"
                   : "rejected drop target was reported as accepted");
    check(observed != nullptr && observed->hovered,
          "drop target hover was hidden by the active drag source");
    queueMouse(targetRect, ImGuiMouseButton_Left, false);
    (void)renderFrame(editor, host, fixture.model, &probe);
    check(!probe.failed(), probe.failureMessage());
  }
}

ModelFixture makeInspectorModel() {
  ModelFixture fixture = makeModel();
  const EditorObjectId target = fixture.rootRow.object;
  fixture.model.inspector.selection = fixture.model.selection;
  fixture.model.inspector.title = "Root";
  ui::InspectorSection transform{ui::InspectorSectionKind::transform,
                                 "TRANSFORM", true};
  transform.fields.push_back(
      {ui::FieldId::translation, target, std::array<double, 3>{1.0, 2.0, 3.0}});
  transform.fields.push_back(
      {ui::FieldId::rotationDegrees, target,
       std::array<double, 3>{10.0, 20.0, 30.0}});
  transform.fields.push_back(
      {ui::FieldId::scale, target, std::array<double, 3>{1.0, 1.0, 1.0}});
  fixture.model.inspector.sections.push_back(std::move(transform));
  return fixture;
}

std::uint32_t inspectorComponentId(EditorObjectId target, ui::FieldId field,
                                   const char *label, int component) {
  ImGuiID seed = ImHashStr("Inspector");
  for (const int part : {static_cast<int>(target.kind),
                         static_cast<int>(target.slot),
                         static_cast<int>(target.generation),
                         static_cast<int>(target.authority.value & 0xffffffffULL),
                         static_cast<int>(target.authority.value >> 32U),
                         static_cast<int>(field)}) {
    seed = ImHashData(&part, sizeof(part), seed);
  }
  seed = ImHashStr(label, 0u, seed);
  seed = ImHashData(&component, sizeof(component), seed);
  return ImHashStr("", 0u, seed);
}

void testInspectorVectorProbeSemantics() {
  ImGuiScope imgui;
  FakeHost host;
  ui::SceneEditorUi editor;
  ui::PersistentState state;
  state.windows.inspector = true;
  state.windows.sceneBrowser = false;
  editor.setState(state);
  ModelFixture fixture = makeInspectorModel();
  const EditorObjectId target = fixture.rootRow.object;

  FailClosedProbe probe;
  (void)renderFrame(editor, host, fixture.model, &probe);
  const std::array<ui::EditorUiProbeSemantic, 9> semantics{
      ui::EditorUiProbeSemantic::inspectorTranslationX,
      ui::EditorUiProbeSemantic::inspectorTranslationY,
      ui::EditorUiProbeSemantic::inspectorTranslationZ,
      ui::EditorUiProbeSemantic::inspectorRotationX,
      ui::EditorUiProbeSemantic::inspectorRotationY,
      ui::EditorUiProbeSemantic::inspectorRotationZ,
      ui::EditorUiProbeSemantic::inspectorScaleX,
      ui::EditorUiProbeSemantic::inspectorScaleY,
      ui::EditorUiProbeSemantic::inspectorScaleZ};
  std::vector<std::uint32_t> ids;
  const std::array<ui::FieldId, 3> fields{
      ui::FieldId::translation, ui::FieldId::rotationDegrees, ui::FieldId::scale};
  const std::array<const char *, 3> labels{"Translation", "Rotation", "Scale"};
  for (std::size_t index = 0; index < semantics.size(); ++index) {
    const auto semantic = semantics[index];
    const CapturedItem *item = probe.latestItem(semantic);
    check(item != nullptr && item->rect.valid() && item->visible &&
              item->enabled && item->key.row.object == EditorObjectId{} &&
              item->key.object == target,
          "Inspector vector component probe is missing or malformed");
    check(item->itemId == inspectorComponentId(
                              target, fields[index / 3u], labels[index / 3u],
                              static_cast<int>(index % 3u)),
          "Inspector component ID differs from pinned DragScalarN");
    ids.push_back(item->itemId);
  }
  std::sort(ids.begin(), ids.end());
  check(std::adjacent_find(ids.begin(), ids.end()) == ids.end(),
        "Inspector vector component IDs are not distinct");
  for (std::size_t field = 0; field < fields.size(); ++field) {
    const auto &first = probe.latestItem(semantics[field * 3u])->rect;
    const auto &second = probe.latestItem(semantics[field * 3u + 1u])->rect;
    const auto &third = probe.latestItem(semantics[field * 3u + 2u])->rect;
    const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
    const float total = third.maximumX - first.minimumX - 2.0F * spacing;
    const float split1 = std::trunc(total / 3.0F);
    const float split2 = std::trunc(total * 2.0F / 3.0F);
    check(first.maximumX - first.minimumX == split1 &&
              second.maximumX - second.minimumX == split2 - split1 &&
              third.maximumX - third.minimumX == total - split2 &&
              second.minimumX - first.maximumX == spacing &&
              third.minimumX - second.maximumX == spacing &&
              first.minimumY == second.minimumY &&
              first.minimumY == third.minimumY,
          "Inspector component widths differ from pinned rounded splits");
  }
  fixture.model.rendererBusy = true;
  (void)renderFrame(editor, host, fixture.model, &probe);
  for (const auto semantic : semantics) {
    const CapturedItem *item = probe.currentFrameItem(semantic);
    check(item != nullptr && !item->enabled,
          "Inspector probe ignored its outer busy disabled scope");
  }
  check(!probe.failed(), probe.failureMessage());
}

void testInspectorVectorMouseEdit() {
  struct DragCase final {
    ui::EditorUiProbeSemantic semantic;
    ui::FieldId field;
    std::array<double, 3> original;
    std::array<double, 3> edited;
  };
  const std::array<DragCase, 3> cases{{
      {ui::EditorUiProbeSemantic::inspectorTranslationX, ui::FieldId::translation,
       {1.0, 2.0, 3.0}, {1.2, 2.0, 3.0}},
      {ui::EditorUiProbeSemantic::inspectorRotationY,
       ui::FieldId::rotationDegrees, {10.0, 20.0, 30.0}, {10.0, 22.0, 30.0}},
      {ui::EditorUiProbeSemantic::inspectorScaleZ, ui::FieldId::scale,
       {1.0, 1.0, 1.0}, {1.0, 1.0, 1.2}},
  }};
  for (const DragCase &drag : cases) {
    ImGuiScope imgui;
    FakeHost host;
    host.dispatchResult = {true, ui::ProjectionRefresh::none, {}};
    ui::SceneEditorUi editor;
    ui::PersistentState state;
    state.windows.inspector = true;
    state.windows.sceneBrowser = false;
    editor.setState(state);
    const ModelFixture fixture = makeInspectorModel();
    FailClosedProbe probe;
    (void)renderFrame(editor, host, fixture.model, &probe);
    const CapturedItem *item = probe.currentFrameItem(drag.semantic);
    check(item != nullptr, "Inspector mouse edit target is missing");
    const ui::EditorUiProbeRect rect = item->rect;
    queueMousePosition(rect);
    (void)renderFrame(editor, host, fixture.model, &probe);
    (void)renderFrame(editor, host, fixture.model, &probe);
    check(probe.currentFrameItem(drag.semantic)->hovered,
          "Inspector component did not observe actual mouse hover");

    queueMouse(rect, ImGuiMouseButton_Left, true);
    (void)renderFrame(editor, host, fixture.model, &probe);
    check(probe.currentFrameItem(drag.semantic)->active &&
              probe.currentFrameItem(drag.semantic)->focused,
          "Inspector component did not observe actual mouse activation");
    check(host.commands.size() == 1u,
          "Inspector mouse down did not dispatch only begin");
    const auto *begin = std::get_if<ui::PropertyEdit>(&host.commands[0].payload);
    check(begin != nullptr && begin->target == fixture.rootRow.object &&
              begin->field == drag.field && begin->phase == ui::EditPhase::begin &&
              std::get<std::array<double, 3>>(begin->value) == drag.original,
          "Inspector begin did not retain the original vector");

    const float movedX = (rect.minimumX + rect.maximumX) * 0.5F + 20.0F;
    const float movedY = (rect.minimumY + rect.maximumY) * 0.5F;
    ImGui::GetIO().AddMousePosEvent(movedX, movedY);
    (void)renderFrame(editor, host, fixture.model, &probe);
    check(host.commands.size() == 2u,
          "Inspector mouse move did not dispatch exactly one preview");
    const auto *preview = std::get_if<ui::PropertyEdit>(&host.commands[1].payload);
    check(preview != nullptr && preview->target == fixture.rootRow.object &&
              preview->field == drag.field &&
              preview->phase == ui::EditPhase::preview &&
              std::get<std::array<double, 3>>(preview->value) == drag.edited,
          "Inspector preview did not report the actual modified vector");

    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    (void)renderFrame(editor, host, fixture.model, &probe);
    check(host.commands.size() == 3u,
          "Inspector mouse release did not dispatch exactly one commit");
    const auto *commit = std::get_if<ui::PropertyEdit>(&host.commands[2].payload);
    check(commit != nullptr && commit->target == fixture.rootRow.object &&
              commit->field == drag.field &&
              commit->phase == ui::EditPhase::commit &&
              std::get<std::array<double, 3>>(commit->value) == drag.edited,
          "Inspector commit did not preserve the exact preview vector");
    (void)renderFrame(editor, host, fixture.model, &probe);
    check(host.commands.size() == 3u && !probe.failed(),
          "Inspector edit emitted duplicate terminal dispatches");
  }
}

const CapturedItem &openNodeContext(ui::SceneEditorUi &editor, FakeHost &host,
                                    const ModelFixture &fixture,
                                    FailClosedProbe &probe) {
  (void)renderFrame(editor, host, fixture.model, &probe);
  const CapturedItem *row = probe.latestItem(
      ui::EditorUiProbeSemantic::graphRow, fixture.rootRow);
  check(row != nullptr, "node row probe is missing");
  const ui::EditorUiProbeRect rowRect = row->rect;
  queueMouse(rowRect, ImGuiMouseButton_Right, true);
  (void)renderFrame(editor, host, fixture.model, &probe);
  queueMouse(rowRect, ImGuiMouseButton_Right, false);
  (void)renderFrame(editor, host, fixture.model, &probe);
  const CapturedItem *rename = probe.latestItem(
      ui::EditorUiProbeSemantic::nodeRename, fixture.rootRow);
  check(rename != nullptr && rename->popupToken != 0u &&
            rename->windowToken != 0u && rename->enabled &&
            rename->key.row == fixture.rootRow,
        "node context popup did not expose exact-row semantic items");
  return *rename;
}

void testContextAndModalSemantics() {
  {
    ImGuiScope imgui;
    FakeHost host;
    ui::SceneEditorUi editor;
    ui::PersistentState state;
    state.windows.inspector = true;
    editor.setState(state);
    const ModelFixture fixture = makeModel();
    FailClosedProbe probe;
    (void)renderFrame(editor, host, fixture.model, &probe);
    const CapturedItem *sceneRow = probe.latestItem(
        ui::EditorUiProbeSemantic::graphRow, fixture.sceneRow);
    check(sceneRow != nullptr, "scene row probe is missing");
    const ui::EditorUiProbeRect sceneRect = sceneRow->rect;
    queueMouse(sceneRect, ImGuiMouseButton_Right, true);
    (void)renderFrame(editor, host, fixture.model, &probe);
    (void)renderFrame(editor, host, fixture.model, &probe);
    queueMouse(sceneRect, ImGuiMouseButton_Right, false);
    (void)renderFrame(editor, host, fixture.model, &probe);
    for (const ui::EditorUiProbeSemantic semantic : {
             ui::EditorUiProbeSemantic::sceneAddNode,
             ui::EditorUiProbeSemantic::sceneAddPointLight,
             ui::EditorUiProbeSemantic::sceneAddDirectionalLight,
             ui::EditorUiProbeSemantic::sceneAddSpotLight}) {
      const CapturedItem *item = probe.latestItem(semantic, fixture.sceneRow);
      check(item != nullptr && item->rect.valid() && item->popupToken != 0u &&
                item->windowToken != 0u &&
                item->key.row == fixture.sceneRow,
            "scene context semantic is missing or invalid");
    }
    (void)renderFrame(editor, host, fixture.model, &probe);
    check(probe.currentFrameItem(ui::EditorUiProbeSemantic::sceneAddNode,
                                 fixture.sceneRow) != nullptr,
          "scene context popup did not persist after its release frame");
    check(!probe.failed(), probe.failureMessage());
  }

  {
    ImGuiScope imgui;
    FakeHost host;
    ui::SceneEditorUi editor;
    ui::PersistentState state;
    state.windows.inspector = true;
    editor.setState(state);
    const ModelFixture fixture = makeModel();
    FailClosedProbe probe;
    (void)openNodeContext(editor, host, fixture, probe);
    for (const ui::EditorUiProbeSemantic semantic : {
             ui::EditorUiProbeSemantic::nodeDuplicate,
             ui::EditorUiProbeSemantic::nodeDelete,
             ui::EditorUiProbeSemantic::nodeAddChildMenu,
             ui::EditorUiProbeSemantic::nodeRename}) {
      const CapturedItem *item = probe.latestItem(semantic, fixture.rootRow);
      check(item != nullptr && item->rect.valid() && item->popupToken != 0u,
            "node context semantic is missing or invalid");
    }
    const CapturedItem *opener = probe.latestItem(
        ui::EditorUiProbeSemantic::nodeAddChildMenu, fixture.rootRow);
    const ui::EditorUiProbeRect openerRect = opener->rect;
    ImGui::GetIO().DeltaTime = 0.5F;
    for (int frame = 0; frame < 3; ++frame) {
      queueMousePosition(openerRect);
      (void)renderFrame(editor, host, fixture.model, &probe);
    }
    for (const ui::EditorUiProbeSemantic semantic : {
             ui::EditorUiProbeSemantic::nodeAddChildNode,
             ui::EditorUiProbeSemantic::nodeAddChildPointLight,
             ui::EditorUiProbeSemantic::nodeAddChildDirectionalLight,
             ui::EditorUiProbeSemantic::nodeAddChildSpotLight}) {
      const CapturedItem *item = probe.latestItem(semantic, fixture.rootRow);
      check(item != nullptr && item->rect.valid() && item->popupToken != 0u &&
                item->key.row == fixture.rootRow,
            "Add Child submenu semantic is missing or invalid");
    }
    check(!probe.failed(), probe.failureMessage());
  }

  {
    ImGuiScope imgui;
    FakeHost host;
    ui::SceneEditorUi editor;
    ui::PersistentState state;
    state.windows.inspector = true;
    editor.setState(state);
    const ModelFixture fixture = makeModel();
    FailClosedProbe probe;
    const CapturedItem &rename =
        openNodeContext(editor, host, fixture, probe);
    const ui::EditorUiProbeRect renameRect = rename.rect;
    queueMousePosition(renameRect);
    (void)renderFrame(editor, host, fixture.model, &probe);
    (void)renderFrame(editor, host, fixture.model, &probe);
    queueMouse(renameRect, ImGuiMouseButton_Left, true);
    (void)renderFrame(editor, host, fixture.model, &probe);
    queueMouse(renameRect, ImGuiMouseButton_Left, false);
    (void)renderFrame(editor, host, fixture.model, &probe);
    for (int frame = 0; frame < 4; ++frame) {
      auto modalModel = fixture.model;
      modalModel.lifecycle = ui::LifecycleState::modal;
      modalModel.modalPending = true;
      modalModel.mutationEnabled = false;
      (void)renderFrame(editor, host, modalModel, &probe);
      check(editor.modalPending() &&
                probe.currentFrameItem(ui::EditorUiProbeSemantic::renameInput,
                                       fixture.rootRow) != nullptr,
            "rename modal disappeared while its request remained pending");
    }
    for (const ui::EditorUiProbeSemantic semantic : {
             ui::EditorUiProbeSemantic::renameInput,
             ui::EditorUiProbeSemantic::renameAccept,
             ui::EditorUiProbeSemantic::renameCancel}) {
      const CapturedItem *item =
          probe.currentFrameItem(semantic, fixture.rootRow);
      check(item != nullptr, "rename modal semantic is missing");
      check(item->modalToken != 0u && item->windowToken != 0u &&
                item->key.row == fixture.rootRow &&
                item->key.object == fixture.rootRow.object,
            "rename modal semantic lost its exact source row");
    }
    check(!probe.failed(), probe.failureMessage());
  }

  {
    ImGuiScope imgui;
    FakeHost host;
    ui::SceneEditorUi editor;
    ui::PersistentState state;
    state.windows.inspector = true;
    editor.setState(state);
    const ModelFixture fixture = makeModel();
    FailClosedProbe probe;
    (void)openNodeContext(editor, host, fixture, probe);
    const CapturedItem *deleteItem = probe.latestItem(
        ui::EditorUiProbeSemantic::nodeDelete, fixture.rootRow);
    check(deleteItem != nullptr, "delete context semantic is missing");
    const ui::EditorUiProbeRect deleteRect = deleteItem->rect;
    queueMousePosition(deleteRect);
    (void)renderFrame(editor, host, fixture.model, &probe);
    (void)renderFrame(editor, host, fixture.model, &probe);
    queueMouse(deleteRect, ImGuiMouseButton_Left, true);
    (void)renderFrame(editor, host, fixture.model, &probe);
    queueMouse(deleteRect, ImGuiMouseButton_Left, false);
    (void)renderFrame(editor, host, fixture.model, &probe);
    for (int frame = 0; frame < 4; ++frame) {
      auto modalModel = fixture.model;
      modalModel.lifecycle = ui::LifecycleState::modal;
      modalModel.modalPending = true;
      modalModel.mutationEnabled = false;
      (void)renderFrame(editor, host, modalModel, &probe);
      check(editor.modalPending() &&
                probe.currentFrameItem(ui::EditorUiProbeSemantic::deleteAccept,
                                       fixture.rootRow) != nullptr,
            "delete modal disappeared while its request remained pending");
    }
    for (const ui::EditorUiProbeSemantic semantic : {
             ui::EditorUiProbeSemantic::deleteAccept,
             ui::EditorUiProbeSemantic::deleteCancel}) {
      const CapturedItem *item =
          probe.currentFrameItem(semantic, fixture.rootRow);
      check(item != nullptr, "delete modal semantic is missing");
      check(item->modalToken != 0u && item->key.row == fixture.rootRow &&
                item->key.object == fixture.rootRow.object,
            "delete modal semantic lost its exact source row");
    }
    check(!probe.failed(), probe.failureMessage());
  }
}

using ControllerRenderSignature =
    ui::RenderResult (ui::EditorController::*)(ui::LayoutState,
                                               ui::EditorUiProbe *);
static_assert(std::is_same_v<
              decltype(static_cast<ControllerRenderSignature>(
                  &ui::EditorController::render)),
              ControllerRenderSignature>);

} // namespace

int main() {
  try {
    testFailClosedConsumer();
    testGraphBindingAndDispatchObservation();
    testNativeViewportEscapeOwnership();
    testTrickledNativeEscapeIsConsumedExactlyOnce();
    testNativeEscapeAcknowledgmentSurvivesDetachAndModal();
    testNativeEscapeAcknowledgmentsCountAndReset();
    testDropAcceptanceObservation();
    testInspectorVectorProbeSemantics();
    testInspectorVectorMouseEdit();
    testContextAndModalSemantics();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "Editor UI probe tests passed.\n";
  return EXIT_SUCCESS;
}
