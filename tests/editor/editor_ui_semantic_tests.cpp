#include "../../app/editor/SceneEditorUi.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace demo::editor;
namespace ui = demo::editor::ui;

constexpr EditorDocumentAuthority kAuthority{0x53454d414e544943ULL};

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

struct FakeHost final : ui::SceneEditorUiHost {
  std::optional<std::filesystem::path>
  nativeFileDialog(const ui::NativeDialogRequest &) override {
    ++dialogRequests;
    return std::nullopt;
  }

  std::string clipboardText() const override { return clipboard; }

  bool setClipboardText(std::string_view value) override {
    clipboard.assign(value);
    return true;
  }

  ui::RuntimeActionResult dispatch(const ui::UiCommand &command) override {
    commands.push_back(command);
    return ui::RuntimeActionResult{true, ui::ProjectionRefresh::none, {}};
  }

  ui::RuntimeActionResult cameraRequest(const ui::CameraRequest &) override {
    ++cameraRequests;
    return ui::RuntimeActionResult{true, ui::ProjectionRefresh::none, {}};
  }

  ui::NodeDropValidation
  validateNodeDrop(const ui::ReparentNode &) const override {
    return ui::NodeDropValidation{true, {}};
  }

  ui::ViewportSettings viewportSettings() const override { return viewport; }

  void setViewportSettings(const ui::ViewportSettings &value) override {
    viewport = value;
  }

  bool viewportControlsAvailable() const noexcept override {
    return viewportAvailable;
  }

  void refreshProjection(ui::ProjectionRefresh) override {}

  ui::ViewportSettings viewport;
  bool viewportAvailable{true};
  std::string clipboard;
  std::vector<ui::UiCommand> commands;
  std::size_t dialogRequests{0};
  std::size_t cameraRequests{0};
};

struct ImGuiScope final {
  ImGuiScope() {
    context = ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0F, 720.0F);
    io.DeltaTime = 1.0F / 60.0F;
    io.MousePos = ImVec2(-10000.0F, -10000.0F);
    ImGui::StyleColorsDark();

    // This test has no renderer backend to perform the usual NewFrame font
    // upload. Build the atlas explicitly so ImGui::Render remains valid in a
    // headless semantic run.
    unsigned char *fontPixels = nullptr;
    int fontWidth = 0;
    int fontHeight = 0;
    io.Fonts->GetTexDataAsRGBA32(&fontPixels, &fontWidth, &fontHeight);
  }

  ~ImGuiScope() {
    if (GImGui != nullptr && GImGui->LogEnabled)
      ImGui::LogFinish();
    ImGui::DestroyContext(context);
  }

  ImGuiContext *context{nullptr};
};

bool contains(std::string_view text, std::string_view needle) {
  return text.find(needle) != std::string_view::npos;
}

std::string renderAndCapture(ui::SceneEditorUi &editor, FakeHost &host,
                             const ui::FrameModel &model) {
  ImGuiIO &io = ImGui::GetIO();
  io.MousePos = ImVec2(-10000.0F, -10000.0F);
  io.MouseDown[0] = false;
  io.MouseDown[1] = false;
  io.MouseDown[2] = false;
  io.KeyCtrl = false;
  io.KeyShift = false;
  io.KeyAlt = false;
  io.KeySuper = false;

  ImGui::NewFrame();
  // Attach logging to ImGui's implicit Debug window. A temporary explicit
  // window would call LogFinish() when it ends and clear the buffer.
  ImGui::LogToBuffer(0);

  (void)editor.render(model, host);
  // EndFrame() clears the active log, so copy it while the frame is still
  // open, after all production windows have emitted their text.
  const std::string result = GImGui->LogBuffer.c_str();
  ImGui::Render();
  return result;
}

void openSections(const char *windowName,
                  std::initializer_list<const char *> labels) {
  ImGuiWindow *window = ImGui::FindWindowByName(windowName);
  if (window == nullptr)
    return;
  for (const char *label : labels)
    window->StateStorage.SetInt(window->GetID(label), 1);
}

ui::FrameModel makeModel() {
  const SceneId scene = typedId<EditorObjectKind::scene>(0u);
  const NodeId node = typedId<EditorObjectKind::node>(0u);
  const MeshId mesh = typedId<EditorObjectKind::mesh>(0u);
  const PrimitiveId primitive = typedId<EditorObjectKind::primitive>(0u);
  const MaterialId material = typedId<EditorObjectKind::material>(0u);
  const TextureId texture = typedId<EditorObjectKind::texture>(0u);
  const ImageId image = typedId<EditorObjectKind::image>(0u);
  const AnimationId animation = typedId<EditorObjectKind::animation>(0u);

  ui::FrameModel model;
  model.lifecycle = ui::LifecycleState::readyClean;
  model.hasDocument = true;
  model.mutationEnabled = true;
  model.debugUiAvailable = true;
  model.revision = 7u;
  model.cleanRevision = 7u;
  model.selectionRevision = 3u;
  model.canUndo = true;
  model.canRedo = true;
  model.canFitSelection = true;
  model.undoDescription = "Rename Root";
  model.redoDescription = "Delete Root";

  model.browser.asset.version = "2.0";
  model.browser.asset.generator = "semantic-test-generator";
  model.browser.asset.copyright = "semantic-test-copyright";
  model.browser.asset.minimumVersion = "2.0";
  model.browser.asset.extensions = {"EXT_semantic_test", "KHR_xmp_json_ld"};
  model.browser.asset.metadata = {
      {"XMP", "semantic metadata"}, {"LongField", "UTF-8 value"}};

  model.scenes.emplace_back(scene, 0u, "Main Scene", true);

  EditorSelectionSnapshot nodeSelection;
  nodeSelection.authority = kAuthority;
  nodeSelection.value = NodeSelection{scene, node, std::nullopt};
  model.selection = nodeSelection;

  ui::GraphRow sceneRow;
  sceneRow.key = ui::GraphRowKey{scene, scene.untyped(), std::nullopt, 0u};
  sceneRow.kind = ui::GraphRowKind::scene;
  sceneRow.documentIndex = 0u;
  sceneRow.depth = 0u;
  sceneRow.label = "Main Scene";
  sceneRow.secondary = "Current scene";
  sceneRow.hasChildren = true;
  sceneRow.defaultExpanded = true;
  sceneRow.acceptsNodeDrop = true;
  model.browser.graphRows.push_back(sceneRow);

  ui::GraphRow nodeRow;
  nodeRow.key = ui::GraphRowKey{scene, node.untyped(), std::nullopt, 0u};
  nodeRow.kind = ui::GraphRowKind::node;
  nodeRow.documentIndex = 0u;
  nodeRow.depth = 1u;
  nodeRow.label = "Root";
  nodeRow.secondary = "Node";
  nodeRow.defaultExpanded = true;
  nodeRow.selected = true;
  nodeRow.selectable = true;
  nodeRow.acceptsNodeDrop = true;
  nodeRow.selection = nodeSelection;
  model.browser.graphRows.push_back(nodeRow);

  ui::GraphRow meshRow;
  meshRow.key = ui::GraphRowKey{scene, mesh.untyped(), node, 0u};
  meshRow.kind = ui::GraphRowKind::mesh;
  meshRow.documentIndex = 0u;
  meshRow.depth = 2u;
  meshRow.label = "Mesh";
  meshRow.secondary = "1 primitive";
  meshRow.hasChildren = true;
  meshRow.defaultExpanded = true;
  meshRow.selectable = true;
  meshRow.selection.authority = kAuthority;
  meshRow.selection.value = mesh;
  model.browser.graphRows.push_back(meshRow);

  ui::GraphRow primitiveRow;
  primitiveRow.key = ui::GraphRowKey{scene, primitive.untyped(), node, 0u};
  primitiveRow.kind = ui::GraphRowKind::primitive;
  primitiveRow.documentIndex = 0u;
  primitiveRow.depth = 3u;
  primitiveRow.label = "Primitive";
  primitiveRow.secondary = "Material 0";
  primitiveRow.selectable = true;
  primitiveRow.selection.authority = kAuthority;
  primitiveRow.selection.value =
      PrimitiveSelection{scene, node, primitive, mesh, 0u};
  model.browser.graphRows.push_back(primitiveRow);

  model.browser.animation.items.push_back(
      ui::AnimationItem{animation, 0u, "Idle", 0.0, 1.0, 0.0});
  model.browser.animation.current = animation;
  model.browser.animation.playing = false;
  model.browser.animation.speed = 1.0;
  model.browser.animation.runtimeControlsAvailable = true;
  model.browser.variants.push_back(ui::VariantItem{0u, "Default", true, true});

  const std::array<ui::ResourceGroupKind, 8> groups = {
      ui::ResourceGroupKind::nodes,     ui::ResourceGroupKind::meshes,
      ui::ResourceGroupKind::materials, ui::ResourceGroupKind::cameras,
      ui::ResourceGroupKind::lights,    ui::ResourceGroupKind::textures,
      ui::ResourceGroupKind::images,    ui::ResourceGroupKind::animations};
  for (const ui::ResourceGroupKind kind : groups) {
    ui::ResourceGroup group;
    group.kind = kind;
    if (kind == ui::ResourceGroupKind::nodes) {
      ui::ResourceEntry entry;
      entry.object = node.untyped();
      entry.documentIndex = 0u;
      entry.name = "Root";
      entry.detail = "0 children";
      entry.selectable = true;
      entry.selection = nodeSelection;
      entry.selected = true;
      group.entries.push_back(std::move(entry));
    } else if (kind == ui::ResourceGroupKind::meshes) {
      ui::ResourceEntry entry;
      entry.object = mesh.untyped();
      entry.documentIndex = 0u;
      entry.name = "Mesh";
      entry.detail = "1 prim, 1 refs";
      entry.selectable = true;
      entry.selection.authority = kAuthority;
      entry.selection.value = mesh;
      group.entries.push_back(std::move(entry));
    } else if (kind == ui::ResourceGroupKind::materials) {
      ui::ResourceEntry entry;
      entry.object = material.untyped();
      entry.documentIndex = 0u;
      entry.name = "Material";
      entry.detail = "1 refs";
      entry.selectable = true;
      entry.selection.authority = kAuthority;
      entry.selection.value = material;
      group.entries.push_back(std::move(entry));
    } else if (kind == ui::ResourceGroupKind::textures) {
      ui::ResourceEntry entry;
      entry.object = texture.untyped();
      entry.documentIndex = 0u;
      entry.name = "Texture";
      entry.selectable = false;
      entry.linkedImage = image;
      group.entries.push_back(std::move(entry));
    } else if (kind == ui::ResourceGroupKind::images) {
      ui::ResourceEntry entry;
      entry.object = image.untyped();
      entry.documentIndex = 0u;
      entry.name = "Image";
      entry.selectable = false;
      group.entries.push_back(std::move(entry));
    } else if (kind == ui::ResourceGroupKind::animations) {
      ui::ResourceEntry entry;
      entry.object = animation.untyped();
      entry.documentIndex = 0u;
      entry.name = "Idle";
      entry.selectable = false;
      group.entries.push_back(std::move(entry));
    }
    model.browser.resourceGroups.push_back(std::move(group));
  }

  model.browser.debug.renderNodeCount = 1u;
  model.browser.debug.renderPrimitiveCount = 1u;
  model.browser.debug.documentNodeCount = 1u;
  model.browser.debug.meshCount = 1u;
  model.browser.debug.nodesWithMeshes = 1u;
  model.browser.debug.expectedRenderNodeCount = 1u;
  model.browser.debug.gpuSyncValidationActive = true;
  model.browser.debug.rows.push_back(
      ui::DebugRenderRow{0u, 0u, "Root", 0u, 0u, true});

  model.materials.push_back(ui::MaterialChoice{material, 0u, "Material"});
  model.inspector.selection = nodeSelection;
  model.inspector.selectionRevision = model.selectionRevision;
  model.inspector.title = "Root";
  ui::InspectorSection identity{ui::InspectorSectionKind::identity, "Node",
                                true};
  ui::InspectorField nameField;
  nameField.id = ui::FieldId::name;
  nameField.target = node.untyped();
  nameField.value = std::string("Root");
  identity.fields.push_back(std::move(nameField));
  model.inspector.sections.push_back(std::move(identity));
  ui::InspectorSection transform{ui::InspectorSectionKind::transform,
                                "Transform", true};
  for (const auto [field, value] :
       std::array<std::pair<ui::FieldId, std::array<double, 3>>, 3>{
           std::pair{ui::FieldId::translation,
                     std::array<double, 3>{0.0, 0.0, 0.0}},
           std::pair{ui::FieldId::rotationDegrees,
                     std::array<double, 3>{0.0, 0.0, 0.0}},
           std::pair{ui::FieldId::scale,
                     std::array<double, 3>{1.0, 1.0, 1.0}}}) {
    ui::InspectorField fieldModel;
    fieldModel.id = field;
    fieldModel.target = node.untyped();
    fieldModel.value = value;
    transform.fields.push_back(std::move(fieldModel));
  }
  model.inspector.sections.push_back(std::move(transform));
  return model;
}

bool testCommandDescriptorsAndEnablement() {
  struct Expected final {
    ui::CommandId id;
    std::string_view menu;
    std::string_view label;
    std::string_view shortcut;
  };
  constexpr std::array expected = {
      Expected{ui::CommandId::newScene, "File", "New Scene", "Ctrl+N"},
      Expected{ui::CommandId::open, "File", "Open", "Ctrl+O"},
      Expected{ui::CommandId::importMerge, "File", "Import/Merge Scene...", ""},
      Expected{ui::CommandId::openRecent, "File", "Open Recent", ""},
      Expected{ui::CommandId::saveAs, "File", "Save As", "Ctrl+S"},
      Expected{ui::CommandId::exit, "File", "Exit", "Ctrl+Q"},
      Expected{ui::CommandId::undo, "Edit", "Undo", "Ctrl+Z"},
      Expected{ui::CommandId::redo, "Edit", "Redo", "Ctrl+Y"},
      Expected{ui::CommandId::duplicateSelection, "Edit", "Duplicate", "Ctrl+D"},
      Expected{ui::CommandId::deleteSelection, "Edit", "Delete", "Delete"},
      Expected{ui::CommandId::fitScene, "View", "Fit Scene", "Ctrl+Shift+F"},
      Expected{ui::CommandId::fitObject, "View", "Fit Object", "Ctrl+F"},
      Expected{ui::CommandId::toggleGrid, "View", "Grid", "G"},
      Expected{ui::CommandId::toggleGizmo, "View", "Gizmo", "T"},
      Expected{ui::CommandId::toggleSnap, "View", "Snap", ""},
      Expected{ui::CommandId::showGridSnapSettings, "View", "Grid & Snap Settings...", ""},
      Expected{ui::CommandId::toggleSceneBrowser, "Windows", "Scene Browser", "F2"},
      Expected{ui::CommandId::toggleInspector, "Windows", "Inspector", "F4"},
      Expected{ui::CommandId::compactScene, "Tools", "Compact Scene", "Ctrl+K"},
      Expected{ui::CommandId::clearSelection, "", "Clear selection", "Escape"},
  };

  const auto descriptors = ui::commandDescriptors();
  bool passed = descriptors.size() == expected.size();
  if (!passed)
    std::cerr << "FAILED: command descriptor count differs\n";
  const std::size_t count = std::min(descriptors.size(), expected.size());
  for (std::size_t index = 0; index < count; ++index) {
    const auto &actual = descriptors[index];
    const auto &want = expected[index];
    const bool match = actual.id == want.id && actual.menu == want.menu &&
                       actual.label == want.label &&
                       actual.shortcut == want.shortcut;
    if (!match)
      std::cerr << "FAILED: command descriptor at index " << index << "\n";
    passed &= match;
  }

  ui::FrameModel model = makeModel();
  ui::WindowState windows;
  ui::ViewportSettings viewport;
  const auto ready = [&](ui::CommandId id) {
    return ui::evaluateCommand(id, model, windows, viewport);
  };
  for (const ui::CommandId id : {ui::CommandId::newScene, ui::CommandId::open,
                                 ui::CommandId::openRecent,
                                 ui::CommandId::importMerge,
                                 ui::CommandId::saveAs, ui::CommandId::undo,
                                 ui::CommandId::redo,
                                 ui::CommandId::duplicateSelection,
                                 ui::CommandId::deleteSelection,
                                 ui::CommandId::fitScene,
                                 ui::CommandId::fitObject,
                                 ui::CommandId::toggleGrid,
                                 ui::CommandId::toggleGizmo,
                                 ui::CommandId::toggleSnap,
                                 ui::CommandId::showGridSnapSettings,
                                 ui::CommandId::toggleSceneBrowser,
                                 ui::CommandId::toggleInspector,
                                 ui::CommandId::compactScene}) {
    if (!ready(id).enabled) {
      std::cerr << "FAILED: ready command is disabled\n";
      passed = false;
    }
  }
  if (!ready(ui::CommandId::toggleGrid).checked ||
      !ready(ui::CommandId::toggleGizmo).checked ||
      ready(ui::CommandId::toggleSnap).checked) {
    std::cerr << "FAILED: ready viewport checked state differs\n";
    passed = false;
  }

  ui::FrameModel noDocument;
  noDocument.lifecycle = ui::LifecycleState::noDocument;
  const auto noDocOpen = ui::evaluateCommand(ui::CommandId::open, noDocument,
                                             windows, viewport);
  const auto noDocSave = ui::evaluateCommand(ui::CommandId::saveAs, noDocument,
                                             windows, viewport);
  passed &= noDocOpen.enabled && !noDocSave.enabled;
  if (!noDocOpen.enabled || noDocSave.enabled)
    std::cerr << "FAILED: no-document file enablement differs\n";

  model.rendererBusy = true;
  const auto busySave =
      ui::evaluateCommand(ui::CommandId::saveAs, model, windows, viewport);
  const auto busyUndo =
      ui::evaluateCommand(ui::CommandId::undo, model, windows, viewport);
  passed &= !busySave.enabled && !busyUndo.enabled;
  if (busySave.enabled || busyUndo.enabled)
    std::cerr << "FAILED: busy mutation gate differs\n";

  model.rendererBusy = false;
  model.modalPending = true;
  model.lifecycle = ui::LifecycleState::modal;
  const auto modalDelete = ui::evaluateCommand(
      ui::CommandId::deleteSelection, model, windows, viewport);
  const auto modalExit =
      ui::evaluateCommand(ui::CommandId::exit, model, windows, viewport);
  passed &= !modalDelete.enabled && !modalExit.enabled;
  if (modalDelete.enabled || modalExit.enabled)
    std::cerr << "FAILED: modal mutation/exit gate differs\n";
  return passed;
}

bool testRenderedSemanticSurface() {
  FakeHost host;
  ui::SceneEditorUi editor;
  const ui::FrameModel model = makeModel();
  const std::string firstLog = renderAndCapture(editor, host, model);
  openSections("Scene Browser", {"Asset Info", "Animation",
                                 "Material Variants", "Nodes (1)",
                                 "Meshes (1)", "Materials (1)"});
  const std::string log = firstLog + renderAndCapture(editor, host, model);

  const ImGuiWindow *browser = ImGui::FindWindowByName("Scene Browser");
  const ImGuiWindow *inspector = ImGui::FindWindowByName("Inspector");
  bool passed = browser != nullptr && inspector != nullptr &&
                browser->LastFrameActive == GImGui->FrameCount &&
                inspector->LastFrameActive == GImGui->FrameCount &&
                browser->Size.x > 0.0F && browser->Size.y > 0.0F &&
                inspector->Size.x > 0.0F && inspector->Size.y > 0.0F;
  if (!passed)
    std::cerr << "FAILED: Scene Browser/Inspector windows were not rendered\n";

  for (const std::string_view text : {
           "Asset Info", "glTF Version: 2.0", "Generator: semantic-test-generator",
           "Copyright: semantic-test-copyright", "Min Version: 2.0",
           "EXT_semantic_test", "KHR_xmp_json_ld", "XMP", "semantic metadata",
           "Root", "Inspector", "Transform", "Translation"}) {
    if (!contains(log, text)) {
      std::cerr << "FAILED: rendered UI text missing: " << text << '\n';
      passed = false;
    }
  }
  if (editor.modalPending()) {
    std::cerr << "FAILED: clean UI unexpectedly reports a pending modal\n";
    passed = false;
  }

  ui::FrameModel busy = model;
  busy.rendererBusy = true;
  busy.mutationEnabled = false;
  busy.operation.label = "Loading candidate";
  const std::string busyLog = renderAndCapture(editor, host, busy);
  passed &= contains(busyLog, "Scene operation in progress...") &&
            contains(busyLog, "Inspector properties are read-only");
  if (!contains(busyLog, "Scene operation in progress...") ||
      !contains(busyLog, "Inspector properties are read-only"))
    std::cerr << "FAILED: busy UI gate text missing\n";

  editor.resetDocumentState();
  passed &= !editor.modalPending();
  return passed;
}

} // namespace

int main() {
  ImGuiScope imgui;
  const bool passed = testCommandDescriptorsAndEnablement() &&
                      testRenderedSemanticSurface();
  if (passed)
    std::cout << "Editor UI semantic tests passed.\n";
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
