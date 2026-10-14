#include <mgif/scene_editor/GltfDocumentIO.h>
#include <mgif/scene_editor/commands/EditorHistory.h>
#include <mgif/scene_editor/commands/PropertyCommands.h>
#include <mgif/scene_editor/testing/EditorStateInspector.h>

#include "support/EditorTestSupport.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace allocation_probe {

std::atomic<bool> enabled{false};
std::atomic<std::size_t> calls{0};

} // namespace allocation_probe

void *operator new(std::size_t size) {
  if (allocation_probe::enabled.load(std::memory_order_relaxed))
    allocation_probe::calls.fetch_add(1u, std::memory_order_relaxed);
  if (void *memory = std::malloc(size == 0u ? 1u : size))
    return memory;
  throw std::bad_alloc{};
}

void *operator new[](std::size_t size) { return ::operator new(size); }

void operator delete(void *memory) noexcept { std::free(memory); }

void operator delete[](void *memory) noexcept { std::free(memory); }

void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }

void operator delete[](void *memory, std::size_t) noexcept {
  std::free(memory);
}

namespace {

using namespace demo::editor;
using namespace demo::editor::testing;
using namespace std::chrono_literals;

bool expect(bool condition, std::string_view message) {
  if (!condition)
    std::cerr << "FAILED: " << message << '\n';
  return condition;
}

std::filesystem::path fixture(const char *name) {
  return std::filesystem::path(MGIF_TEST_SOURCE_DIR) / "tests" / "fixtures" /
         "gltf_editor" / name;
}

std::unique_ptr<GltfDocument>
loadDocument(const char *name = "material_extensions.gltf") {
  auto loaded = GltfDocumentIO::loadCandidate(fixture(name));
  if (!loaded.succeeded())
    return nullptr;
  return std::move(loaded).takeCandidate();
}

std::unique_ptr<GltfDocument> loadInlineDocument(std::string_view json) {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "mgif-p4-05-rich.gltf";
  {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
      return nullptr;
    output.write(json.data(), static_cast<std::streamsize>(json.size()));
    if (!output)
      return nullptr;
  }
  auto loaded = GltfDocumentIO::loadCandidate(path);
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  if (!loaded.succeeded())
    return nullptr;
  return std::move(loaded).takeCandidate();
}

class PropertyRig final {
public:
  explicit PropertyRig(std::unique_ptr<GltfDocument> source,
                       EditorLimits limits = kDefaultEditorLimits)
      : document(std::move(source)), commands(*document, limits, &fault),
        context{*document, commands, selection},
        history(context, clock, limits) {}

  std::unique_ptr<GltfDocument> document;
  ScriptedEditorFaultInjector fault;
  SceneCommands commands;
  EditorSelectionState selection;
  EditorCommandContext context;
  ManualEditorClock clock;
  EditorHistory history;
};

std::unique_ptr<PropertyRig>
makeRig(EditorLimits limits = kDefaultEditorLimits,
        const char *name = "material_extensions.gltf") {
  auto document = loadDocument(name);
  if (!document)
    return nullptr;
  return std::make_unique<PropertyRig>(std::move(document), limits);
}

std::unique_ptr<PropertyRig>
makeInlineRig(std::string_view json,
              EditorLimits limits = kDefaultEditorLimits) {
  auto document = loadInlineDocument(json);
  if (!document)
    return nullptr;
  return std::make_unique<PropertyRig>(std::move(document), limits);
}

template <EditorObjectKind Kind>
std::optional<TypedEditorObjectId<Kind>> idAt(const GltfDocument &document,
                                              std::size_t index) {
  if (index >= document.objectCount(Kind))
    return std::nullopt;
  return TypedEditorObjectId<Kind>::from(document.idAt(Kind, index));
}

ExactEditorStateSnapshot exactState(const PropertyRig &rig) {
  return exactEditorStateSnapshot(*rig.document, rig.commands, rig.selection,
                                  rig.history);
}

HistoryResult executePrepared(PropertyRig &rig, CommandPrepareResult prepared) {
  if (prepared.succeeded())
    return rig.history.execute(std::move(prepared.command));
  HistoryResult result;
  result.status = HistoryStatus::rejected;
  result.command.error = prepared.error;
  result.command.historyPolicy = HistoryPolicy::undoable;
  result.command.beforeRevision = rig.document->revision();
  result.command.afterRevision = rig.document->revision();
  result.command.message = std::move(prepared.message);
  return result;
}

bool readU64(const std::vector<std::uint8_t> &bytes, std::size_t &offset,
             std::uint64_t &value) {
  if (offset > bytes.size() || bytes.size() - offset < sizeof(value))
    return false;
  value = 0;
  for (unsigned shift = 0; shift < 64u; shift += 8u)
    value |= static_cast<std::uint64_t>(bytes[offset++]) << shift;
  return true;
}

std::optional<std::vector<std::uint8_t>>
embeddedSnapshot(const CanonicalPayloadInspection &payload) {
  std::size_t offset = 0;
  std::uint64_t size = 0;
  std::uint64_t capacity = 0;
  if (!readU64(payload.canonicalBytes, offset, size) ||
      !readU64(payload.canonicalBytes, offset, capacity) || size > capacity ||
      size > payload.canonicalBytes.size() - offset) {
    return std::nullopt;
  }
  return std::vector<std::uint8_t>(
      payload.canonicalBytes.begin() + static_cast<std::ptrdiff_t>(offset),
      payload.canonicalBytes.begin() +
          static_cast<std::ptrdiff_t>(offset + static_cast<std::size_t>(size)));
}

template <typename Snapshot>
bool completePropertyInspection(const RetainedCommandInspection &inspection,
                                EditorCommandKind kind, const Snapshot &before,
                                const Snapshot &final) {
  const SnapshotCanonicalRecord beforeRecord = before.inspectCanonicalRecord();
  const SnapshotCanonicalRecord finalRecord = final.inspectCanonicalRecord();
  const auto embeddedBefore = embeddedSnapshot(inspection.beforePayload);
  const auto embeddedFinal = embeddedSnapshot(inspection.finalPayload);
  return inspection.schemaVersion == 1u && inspection.kind == kind &&
         inspection.lifecycle == CommandLifecycleState::prepared &&
         inspection.stateRevision == 0u &&
         inspection.retainedSnapshotBytes != 0u && embeddedBefore &&
         embeddedFinal && beforeRecord.complete() && finalRecord.complete() &&
         *embeddedBefore == beforeRecord.canonicalBytes &&
         *embeddedFinal == finalRecord.canonicalBytes &&
         detail::retainedCommandInspectionHashMatches(inspection);
}

MaterialExtensionValue extensionObject(std::int32_t marker) {
  MaterialExtensionValue result;
  result.value = MaterialExtensionValue::Object{
      {"testMarker", MaterialExtensionValue{marker}}};
  return result;
}

MaterialExtensionValue emptyExtensionObject() {
  MaterialExtensionValue result;
  result.value = MaterialExtensionValue::Object{};
  return result;
}

bool materialPreservesUnknown(const PropertyRig &rig, MaterialId material,
                              const MaterialExtensionValue &unknown,
                              std::string_view extras) {
  return rig.commands.materialExtension(material, "MGIF_test_unknown") ==
             std::optional<MaterialExtensionValue>{unknown} &&
         rig.document->objectExtrasJson(material.untyped()) == extras;
}

std::optional<MaterialSnapshot>
materialAfter(PropertyRig &rig, MaterialId material, const MaterialEdit &edit) {
  return rig.commands.prepareMaterialSnapshot(material, edit);
}

bool executeMaterialEdit(PropertyRig &rig, MaterialId material,
                         const MaterialEdit &edit,
                         bool exerciseUndoRedo = false) {
  const auto before = rig.commands.materialSnapshot(material);
  const auto after = materialAfter(rig, material, edit);
  if (!before || !after)
    return false;
  const HistoryResult executed =
      executePrepared(rig, prepareEditMaterialCommand(rig.context, *before,
                                                      *after, &rig.fault));
  if (executed.status != HistoryStatus::pushed ||
      rig.commands.materialSnapshot(material) != after)
    return false;
  if (!exerciseUndoRedo)
    return true;
  return rig.history.undo().status == HistoryStatus::moved &&
         rig.commands.materialSnapshot(material) == before &&
         rig.history.redo().status == HistoryStatus::moved &&
         rig.commands.materialSnapshot(material) == after;
}

constexpr std::string_view kRichLightFixture = R"json({
  "asset":{"version":"2.0"},
  "extensionsUsed":["KHR_lights_punctual","MGIF_test_unknown"],
  "extensions":{"KHR_lights_punctual":{"lights":[{
    "name":"\u706f\u5149",
    "type":"spot",
    "color":[0.2,0.4,0.6],
    "intensity":2.5,
    "range":8.0,
    "spot":{"innerConeAngle":0.1,"outerConeAngle":0.7,
      "extensions":{"MGIF_test_unknown":{"spotMarker":7}},
      "extras":{"spotExtra":"preserve"}},
    "extensions":{"MGIF_test_unknown":{"lightMarker":9}},
    "extras":{"lightExtra":[true,3,3.0]}
  }]}},
  "scene":0,
  "scenes":[{"nodes":[0]}],
  "nodes":[{"extensions":{"KHR_lights_punctual":{"light":0}}}]
})json";

constexpr std::string_view kTwoLightFixture = R"json({
  "asset":{"version":"2.0"},
  "extensionsUsed":["KHR_lights_punctual"],
  "extensions":{"KHR_lights_punctual":{"lights":[
    {"name":"First Light","type":"point","intensity":1.0},
    {"name":"Second Light","type":"point","intensity":2.0}
  ]}},
  "scene":0,
  "scenes":[{"nodes":[0,1]}],
  "nodes":[
    {"extensions":{"KHR_lights_punctual":{"light":0}}},
    {"extensions":{"KHR_lights_punctual":{"light":1}}}
  ]
})json";

bool testProperties() {
  bool passed = true;
  passed &=
      expect(kDefaultEditorLimits.maxHistoryEntrySnapshotBytes ==
                     8ull * 1024ull * 1024ull &&
                 kDefaultEditorLimits.maxHistoryAggregateSnapshotBytes ==
                     64ull * 1024ull * 1024ull,
             "default property history budgets are exactly 8 MiB and 64 MiB");

  auto rig = makeRig();
  if (!expect(rig != nullptr, "material property fixture loads"))
    return false;
  const auto material = idAt<EditorObjectKind::material>(*rig->document, 0);
  const auto texture = idAt<EditorObjectKind::texture>(*rig->document, 0);
  if (!expect(material.has_value() && texture.has_value(),
              "material and texture targets resolve"))
    return false;
  const auto unknown =
      rig->commands.materialExtension(*material, "MGIF_test_unknown");
  const std::string extras{
      rig->document->objectExtrasJson(material->untyped())};
  auto properties = rig->commands.materialProperties(*material);
  const auto before = rig->commands.materialSnapshot(*material);
  if (!expect(unknown.has_value() && properties.has_value() &&
                  before.has_value(),
              "complete initial material state captures"))
    return false;
  properties->baseColorFactor = {0.1, 0.2, 0.3, 0.4};
  properties->metallicFactor = 0.25;
  properties->roughnessFactor = 0.75;
  properties->emissiveFactor = {0.3, 0.2, 0.1};
  properties->alphaMode = "BLEND";
  properties->alphaCutoff = 0.35;
  properties->doubleSided = true;
  const auto after = materialAfter(*rig, *material, MaterialEdit{*properties});
  if (!expect(after.has_value(),
              "complete material property snapshot prepares"))
    return false;
  CommandPrepareResult prepared =
      prepareEditMaterialCommand(rig->context, *before, *after, &rig->fault);
  if (!expect(prepared.succeeded(), "discrete material command prepares"))
    return false;
  const RetainedCommandInspection inspection =
      prepared.command->inspectRetainedState();
  const RetainedCommandInspection repeated =
      prepared.command->inspectRetainedState();
  const std::string expectedDescription =
      std::string{"Edit Material '"} + std::string(before->name()) + "'";
  passed &= expect(
      prepared.command->description() == expectedDescription &&
          prepared.command->snapshotBytes() ==
              inspection.retainedSnapshotBytes &&
          inspection.stableTarget == material->untyped() &&
          inspection.authority == rig->document->documentAuthority() &&
          inspection == repeated &&
          completePropertyInspection(
              inspection, EditorCommandKind::editMaterial, *before, *after) &&
          inspection.beforePayload != inspection.finalPayload,
      "material retained serializer is complete and deterministic");
  const std::uint64_t revision = rig->document->revision();
  passed &=
      expect(rig->history.execute(std::move(prepared.command)).status ==
                     HistoryStatus::pushed &&
                 rig->commands.materialSnapshot(*material) == after &&
                 rig->document->revision() == revision + 1u &&
                 materialPreservesUnknown(*rig, *material, *unknown, extras),
             "material execute publishes the complete final record once");
  passed &=
      expect(rig->history.undo().status == HistoryStatus::moved &&
                 rig->commands.materialSnapshot(*material) == before &&
                 materialPreservesUnknown(*rig, *material, *unknown, extras) &&
                 rig->history.redo().status == HistoryStatus::moved &&
                 rig->commands.materialSnapshot(*material) == after &&
                 materialPreservesUnknown(*rig, *material, *unknown, extras),
             "material Undo and Redo restore complete immutable records");

  for (const std::string_view alphaMode :
       {std::string_view{"MASK"}, std::string_view{"OPAQUE"},
        std::string_view{"BLEND"}}) {
    auto next = rig->commands.materialProperties(*material);
    if (!next)
      return false;
    next->alphaMode.assign(alphaMode);
    next->alphaCutoff = alphaMode == "MASK" ? 0.45 : 0.5;
    passed &= expect(executeMaterialEdit(*rig, *material, MaterialEdit{*next}),
                     "all core alpha modes execute through property commands");
  }

  for (const MaterialTextureSlot slot :
       {MaterialTextureSlot::baseColor, MaterialTextureSlot::metallicRoughness,
        MaterialTextureSlot::normal, MaterialTextureSlot::occlusion,
        MaterialTextureSlot::emissive}) {
    MaterialTextureEdit edit;
    edit.slot = slot;
    edit.value.texture = *texture;
    edit.value.texCoord = static_cast<std::uint32_t>(slot) + 1u;
    edit.value.scalar =
        slot == MaterialTextureSlot::normal
            ? 0.4
            : (slot == MaterialTextureSlot::occlusion ? 0.6 : 1.0);
    passed &=
        expect(executeMaterialEdit(*rig, *material, MaterialEdit{edit}, true) &&
                   materialPreservesUnknown(*rig, *material, *unknown, extras),
               "each material texture slot executes and restores");
  }

  const auto unchanged = rig->commands.materialSnapshot(*material);
  if (!unchanged)
    return false;
  const ExactEditorStateSnapshot beforeUnchanged = exactState(*rig);
  const HistoryResult noOp = executePrepared(
      *rig, prepareEditMaterialCommand(rig->context, *unchanged, *unchanged,
                                       &rig->fault));
  passed &= expect(noOp.status == HistoryStatus::rejected &&
                       noOp.command.status == CommandStatus::unchanged &&
                       noOp.command.error == MutationErrorCode::noOp &&
                       exactState(*rig) == beforeUnchanged,
                   "unchanged material command is a total history no-op");
  const ExactEditorStateSnapshot beforeStale = exactState(*rig);
  const CommandPrepareResult stale =
      prepareEditMaterialCommand(rig->context, *before, *after, &rig->fault);
  passed &= expect(!stale.succeeded() &&
                       stale.error == MutationErrorCode::staleTransaction &&
                       exactState(*rig) == beforeStale,
                   "stale material before snapshot rejects atomically");

  auto foreign = makeRig();
  if (!foreign)
    return false;
  const auto foreignMaterial =
      idAt<EditorObjectKind::material>(*foreign->document, 0);
  const auto foreignSnapshot =
      foreignMaterial ? foreign->commands.materialSnapshot(*foreignMaterial)
                      : std::nullopt;
  if (!foreignSnapshot)
    return false;
  const CommandPrepareResult crossDocument = prepareEditMaterialCommand(
      rig->context, *unchanged, *foreignSnapshot, &rig->fault);
  passed &=
      expect(!crossDocument.succeeded() &&
                 crossDocument.error == MutationErrorCode::crossDocument &&
                 exactState(*rig) == beforeStale,
             "foreign material snapshot rejects without write");

  auto lightRig = makeInlineRig(kRichLightFixture);
  if (!expect(lightRig != nullptr, "rich light property fixture loads"))
    return false;
  const auto light = idAt<EditorObjectKind::light>(*lightRig->document, 0);
  if (!light)
    return false;
  const std::string lightExtensions{
      lightRig->document->objectExtensionsJson(light->untyped())};
  const std::string lightExtras{
      lightRig->document->objectExtrasJson(light->untyped())};
  const auto lightBefore = lightRig->commands.lightSnapshot(*light);
  if (!lightBefore)
    return false;
  const std::string lightDescription =
      std::string{"Edit Light '"} + std::string(lightBefore->name()) + "'";
  for (const std::string_view type :
       {std::string_view{"point"}, std::string_view{"directional"},
        std::string_view{"spot"}}) {
    auto next = lightRig->commands.lightProperties(*light);
    if (!next)
      return false;
    next->type.assign(type);
    next->color = {0.6, 0.4, 0.2};
    next->intensity += 1.0;
    next->range = type == "directional" ? 0.0 : 6.0;
    next->innerConeAngle = 0.15;
    next->outerConeAngle = 0.65;
    const auto beforeType = lightRig->commands.lightSnapshot(*light);
    const auto afterType =
        lightRig->commands.prepareLightSnapshot(*light, *next);
    if (!beforeType || !afterType)
      return false;
    auto command = prepareEditLightCommand(lightRig->context, *beforeType,
                                           *afterType, &lightRig->fault);
    if (!command.succeeded())
      return false;
    const RetainedCommandInspection typed =
        command.command->inspectRetainedState();
    passed &= expect(
        command.command->description() == lightDescription &&
            completePropertyInspection(typed, EditorCommandKind::editLight,
                                       *beforeType, *afterType),
        "light serializer and UTF-8 description are deterministic");
    passed &= expect(
        lightRig->history.execute(std::move(command.command)).status ==
                HistoryStatus::pushed &&
            lightRig->history.undo().status == HistoryStatus::moved &&
            lightRig->commands.lightSnapshot(*light) == beforeType &&
            lightRig->history.redo().status == HistoryStatus::moved &&
            lightRig->commands.lightSnapshot(*light) == afterType &&
            lightRig->document->objectExtensionsJson(light->untyped()) ==
                lightExtensions &&
            lightRig->document->objectExtrasJson(light->untyped()) ==
                lightExtras,
        "all light types preserve complete unknown extensions and extras");
  }
  auto invalidLight = lightRig->commands.lightProperties(*light);
  if (!invalidLight)
    return false;
  invalidLight->range = -1.0;
  passed &=
      expect(!lightRig->commands.prepareLightSnapshot(*light, *invalidLight),
             "negative light range rejects during preparation");
  invalidLight = lightRig->commands.lightProperties(*light);
  invalidLight->type = "spot";
  invalidLight->innerConeAngle = 0.8;
  invalidLight->outerConeAngle = 0.2;
  passed &=
      expect(!lightRig->commands.prepareLightSnapshot(*light, *invalidLight),
             "inverted spot cone rejects during preparation");
  return passed;
}

bool seedExtensionDependencies(PropertyRig &rig, MaterialId material,
                               std::string_view name) {
  const MaterialExtensionValue empty = emptyExtensionObject();
  if (name != "KHR_materials_dispersion" && name != "KHR_materials_volume" &&
      name != "KHR_materials_volume_scatter") {
    return true;
  }
  MutationResult transmission = rig.commands.setMaterialExtension(
      material, "KHR_materials_transmission", empty);
  if (!transmission.applied())
    return false;
  if (name == "KHR_materials_volume")
    return true;
  MutationResult volume = rig.commands.setMaterialExtension(
      material, "KHR_materials_volume", empty);
  return volume.applied();
}

bool testOneExtension(std::string_view name) {
  auto rig = makeRig();
  if (!rig)
    return false;
  const auto material = idAt<EditorObjectKind::material>(*rig->document, 0);
  if (!material || !seedExtensionDependencies(*rig, *material, name))
    return false;
  const auto unknown =
      rig->commands.materialExtension(*material, "MGIF_test_unknown");
  const std::string extras{
      rig->document->objectExtrasJson(material->untyped())};
  if (!unknown)
    return false;

  MaterialExtensionEdit add{std::string(name), extensionObject(1)};
  if (!executeMaterialEdit(*rig, *material, MaterialEdit{add}) ||
      !materialPreservesUnknown(*rig, *material, *unknown, extras))
    return false;
  MaterialExtensionEdit edit{std::string(name), extensionObject(2)};
  if (!executeMaterialEdit(*rig, *material, MaterialEdit{edit}) ||
      rig->commands.materialExtension(*material, name) !=
          std::optional<MaterialExtensionValue>{*edit.value} ||
      !materialPreservesUnknown(*rig, *material, *unknown, extras))
    return false;
  MaterialExtensionEdit remove{std::string(name), std::nullopt};
  if (!executeMaterialEdit(*rig, *material, MaterialEdit{remove}) ||
      rig->commands.materialExtension(*material, name) ||
      !materialPreservesUnknown(*rig, *material, *unknown, extras))
    return false;
  return rig->history.undo().status == HistoryStatus::moved &&
         rig->commands.materialExtension(*material, name) ==
             std::optional<MaterialExtensionValue>{*edit.value} &&
         materialPreservesUnknown(*rig, *material, *unknown, extras) &&
         rig->history.redo().status == HistoryStatus::moved &&
         !rig->commands.materialExtension(*material, name) &&
         materialPreservesUnknown(*rig, *material, *unknown, extras);
}

bool testExtensions() {
  static constexpr std::array<std::string_view, 14> extensions{
      "KHR_materials_anisotropy",
      "KHR_materials_clearcoat",
      "KHR_materials_diffuse_transmission",
      "KHR_materials_dispersion",
      "KHR_materials_emissive_strength",
      "KHR_materials_ior",
      "KHR_materials_iridescence",
      "KHR_materials_sheen",
      "KHR_materials_specular",
      "KHR_materials_transmission",
      "KHR_materials_unlit",
      "KHR_materials_volume",
      "KHR_materials_volume_scatter",
      "KHR_materials_pbrSpecularGlossiness",
  };
  bool passed = true;
  for (const std::string_view extension : extensions) {
    const std::string message =
        std::string{"add/edit/remove command coverage for "} +
        std::string(extension);
    passed &= expect(testOneExtension(extension), message);
  }
  return passed;
}

std::optional<MaterialSnapshot> changedMaterial(PropertyRig &rig,
                                                MaterialId material,
                                                double roughness, double red) {
  auto properties = rig.commands.materialProperties(material);
  if (!properties)
    return std::nullopt;
  properties->roughnessFactor = roughness;
  properties->baseColorFactor[0] = red;
  return rig.commands.prepareMaterialSnapshot(material,
                                              MaterialEdit{*properties});
}

bool commitMaterialPreview(PropertyRig &rig, MaterialId material,
                           const MaterialSnapshot &value,
                           RetainedCommandInspection *beforeCommit = nullptr) {
  auto prepared =
      prepareContinuousMaterialCommand(rig.context, material, &rig.fault);
  if (!prepared.succeeded() ||
      prepared.command->applyPreview(rig.context, ContinuousCommandValue{value})
              .status != CommandStatus::applied) {
    return false;
  }
  if (beforeCommit)
    *beforeCommit = prepared.command->inspectRetainedState();
  return rig.history.commitExecuted(std::move(prepared.command)).succeeded();
}

bool testMerge() {
  auto rig = makeRig();
  if (!expect(rig != nullptr, "material merge fixture loads"))
    return false;
  const auto material = idAt<EditorObjectKind::material>(*rig->document, 0);
  const auto otherMaterial =
      idAt<EditorObjectKind::material>(*rig->document, 1);
  if (!material || !otherMaterial)
    return false;
  const auto initial = rig->commands.materialSnapshot(*material);
  const auto first = changedMaterial(*rig, *material, 0.2, 1.0);
  if (!initial || !first)
    return false;
  bool passed = true;
  rig->clock.set(EditorTimePoint{});
  passed &= expect(commitMaterialPreview(*rig, *material, *first) &&
                       rig->history.undoCount() == 1u,
                   "first material preview pushes at epoch zero");
  const HistoryInspection firstHistory =
      EditorStateInspector::inspect(rig->history);
  const auto second = changedMaterial(*rig, *material, 0.2, 0.3);
  if (!second)
    return false;
  rig->clock.set(EditorTimePoint{} + 100ms);
  passed &= expect(commitMaterialPreview(*rig, *material, *second) &&
                       rig->history.undoCount() == 1u,
                   "cross-field material previews merge on complete chain");
  const HistoryInspection merged = EditorStateInspector::inspect(rig->history);
  const SnapshotCanonicalRecord initialRecord =
      initial->inspectCanonicalRecord();
  const SnapshotCanonicalRecord secondRecord = second->inspectCanonicalRecord();
  passed &= expect(
      merged.undoOldestToNewest.size() == 1u &&
          embeddedSnapshot(
              merged.undoOldestToNewest[0].retainedCommand.beforePayload) ==
              std::optional<std::vector<std::uint8_t>>{
                  initialRecord.canonicalBytes} &&
          embeddedSnapshot(
              merged.undoOldestToNewest[0].retainedCommand.finalPayload) ==
              std::optional<std::vector<std::uint8_t>>{
                  secondRecord.canonicalBytes} &&
          merged.undoOldestToNewest[0].retainedCommand.beforePayload ==
              firstHistory.undoOldestToNewest[0]
                  .retainedCommand.beforePayload &&
          merged.undoOldestToNewest[0].retainedCommand.stateRevision == 2u,
      "true merge keeps oldest before and adopts newest complete final");
  passed &= expect(rig->history.undo().status == HistoryStatus::moved &&
                       rig->commands.materialSnapshot(*material) == initial &&
                       rig->history.redo().status == HistoryStatus::moved &&
                       rig->commands.materialSnapshot(*material) == second,
                   "merged material command restores complete endpoints");

  const auto otherFinal = changedMaterial(*rig, *otherMaterial, 0.4, 0.4);
  if (!otherFinal) {
    std::cerr << "FAILED: different-target material snapshot prepares\n";
    return false;
  }
  rig->clock.set(EditorTimePoint{} + 150ms);
  passed &= expect(commitMaterialPreview(*rig, *otherMaterial, *otherFinal) &&
                       rig->history.undoCount() == 2u,
                   "different material IDs split instead of merging");

  auto lightRig = makeInlineRig(kRichLightFixture);
  if (!lightRig) {
    std::cerr << "FAILED: merge light fixture loads\n";
    return false;
  }
  const auto light = idAt<EditorObjectKind::light>(*lightRig->document, 0);
  if (!light) {
    std::cerr << "FAILED: merge light target resolves\n";
    return false;
  }
  const auto lightInitial = lightRig->commands.lightSnapshot(*light);
  auto lightProperties = lightRig->commands.lightProperties(*light);
  if (!lightInitial || !lightProperties) {
    std::cerr << "FAILED: merge light state captures\n";
    return false;
  }
  lightProperties->type = "point";
  lightProperties->intensity = 3.0;
  const auto lightFirst =
      lightRig->commands.prepareLightSnapshot(*light, *lightProperties);
  auto firstLightCommand = prepareContinuousLightCommand(
      lightRig->context, *light, &lightRig->fault);
  if (!lightFirst || !firstLightCommand.succeeded()) {
    std::cerr << "FAILED: first merge light command prepares\n";
    return false;
  }
  lightRig->clock.set(EditorTimePoint{});
  const CommandResult firstLightPreview =
      firstLightCommand.command->applyPreview(
          lightRig->context, ContinuousCommandValue{*lightFirst});
  if (firstLightPreview.status != CommandStatus::applied) {
    std::cerr << "FAILED: first merge light preview status="
              << static_cast<unsigned>(firstLightPreview.status)
              << " error=" << static_cast<unsigned>(firstLightPreview.error)
              << '\n';
    return false;
  }
  const HistoryResult firstLightCommit =
      lightRig->history.commitExecuted(std::move(firstLightCommand.command));
  if (!firstLightCommit.succeeded()) {
    std::cerr << "FAILED: first merge light commit status="
              << static_cast<unsigned>(firstLightCommit.status)
              << " command-status="
              << static_cast<unsigned>(firstLightCommit.command.status)
              << " error="
              << static_cast<unsigned>(firstLightCommit.command.error) << '\n';
    return false;
  }
  lightProperties->type = "directional";
  lightProperties->intensity = 4.0;
  const auto lightSecond =
      lightRig->commands.prepareLightSnapshot(*light, *lightProperties);
  auto secondLightCommand = prepareContinuousLightCommand(
      lightRig->context, *light, &lightRig->fault);
  if (!lightSecond || !secondLightCommand.succeeded()) {
    std::cerr << "FAILED: second merge light command prepares\n";
    return false;
  }
  lightRig->clock.set(EditorTimePoint{} + 100ms);
  passed &=
      expect(secondLightCommand.command
                         ->applyPreview(lightRig->context,
                                        ContinuousCommandValue{*lightSecond})
                         .status == CommandStatus::applied &&
                 lightRig->history
                         .commitExecuted(std::move(secondLightCommand.command))
                         .status == HistoryStatus::merged &&
                 lightRig->history.undoCount() == 1u,
             "same-light compatible previews merge");

  lightProperties->intensity = 5.0;
  const auto lightThird =
      lightRig->commands.prepareLightSnapshot(*light, *lightProperties);
  auto thirdLightCommand = prepareContinuousLightCommand(
      lightRig->context, *light, &lightRig->fault);
  if (!lightThird || !thirdLightCommand.succeeded()) {
    std::cerr << "FAILED: intensity-only light command prepares\n";
    return false;
  }
  lightRig->clock.set(EditorTimePoint{} + 200ms);
  const CommandResult thirdLightPreview =
      thirdLightCommand.command->applyPreview(
          lightRig->context, ContinuousCommandValue{*lightThird});
  if (thirdLightPreview.status != CommandStatus::applied) {
    std::cerr << "FAILED: intensity-only light preview status="
              << static_cast<unsigned>(thirdLightPreview.status)
              << " error=" << static_cast<unsigned>(thirdLightPreview.error)
              << '\n';
    return false;
  }
  passed &= expect(
      lightRig->history.commitExecuted(std::move(thirdLightCommand.command))
                  .status == HistoryStatus::merged &&
          lightRig->history.undoCount() == 1u &&
          lightRig->commands.lightSnapshot(*light) == lightThird,
      "intensity-only light preview applies and merges");
  const HistoryInspection lightMerged =
      EditorStateInspector::inspect(lightRig->history);
  const SnapshotCanonicalRecord lightInitialRecord =
      lightInitial->inspectCanonicalRecord();
  const SnapshotCanonicalRecord lightThirdRecord =
      lightThird->inspectCanonicalRecord();
  passed &= expect(
      lightMerged.undoOldestToNewest.size() == 1u &&
          embeddedSnapshot(lightMerged.undoOldestToNewest[0]
                               .retainedCommand.beforePayload) ==
              std::optional<std::vector<std::uint8_t>>{
                  lightInitialRecord.canonicalBytes} &&
          embeddedSnapshot(
              lightMerged.undoOldestToNewest[0].retainedCommand.finalPayload) ==
              std::optional<std::vector<std::uint8_t>>{
                  lightThirdRecord.canonicalBytes} &&
          lightMerged.undoOldestToNewest[0].retainedCommand.stateRevision ==
              3u &&
          lightRig->history.undo().status == HistoryStatus::moved &&
          lightRig->commands.lightSnapshot(*light) == lightInitial &&
          lightRig->history.redo().status == HistoryStatus::moved &&
          lightRig->commands.lightSnapshot(*light) == lightThird,
      "light merge keeps complete oldest and intensity-only endpoints");

  auto twoLightRig = makeInlineRig(kTwoLightFixture);
  const auto firstTarget =
      twoLightRig ? idAt<EditorObjectKind::light>(*twoLightRig->document, 0)
                  : std::nullopt;
  const auto secondTarget =
      twoLightRig ? idAt<EditorObjectKind::light>(*twoLightRig->document, 1)
                  : std::nullopt;
  if (!firstTarget || !secondTarget)
    return false;
  auto firstProperties = twoLightRig->commands.lightProperties(*firstTarget);
  auto secondProperties = twoLightRig->commands.lightProperties(*secondTarget);
  if (!firstProperties || !secondProperties)
    return false;
  firstProperties->intensity = 3.0;
  secondProperties->intensity = 4.0;
  const auto firstFinal = twoLightRig->commands.prepareLightSnapshot(
      *firstTarget, *firstProperties);
  const auto secondFinal = twoLightRig->commands.prepareLightSnapshot(
      *secondTarget, *secondProperties);
  if (!firstFinal || !secondFinal)
    return false;
  auto commitLightPreview = [&](LightId target, const LightSnapshot &value,
                                EditorTimePoint time) {
    auto command = prepareContinuousLightCommand(twoLightRig->context, target,
                                                 &twoLightRig->fault);
    if (!command.succeeded() ||
        command.command
                ->applyPreview(twoLightRig->context,
                               ContinuousCommandValue{value})
                .status != CommandStatus::applied) {
      return false;
    }
    twoLightRig->clock.set(time);
    return twoLightRig->history.commitExecuted(std::move(command.command))
        .succeeded();
  };
  passed &=
      expect(commitLightPreview(*firstTarget, *firstFinal, EditorTimePoint{}) &&
                 commitLightPreview(*secondTarget, *secondFinal,
                                    EditorTimePoint{} + 100ms) &&
                 twoLightRig->history.undoCount() == 2u,
             "different light IDs split inside the merge window");

  auto falseRig = makeRig();
  if (!falseRig) {
    std::cerr << "FAILED: false-absorb fixture loads\n";
    return false;
  }
  const auto falseMaterial =
      idAt<EditorObjectKind::material>(*falseRig->document, 0);
  if (!falseMaterial) {
    std::cerr << "FAILED: false-absorb material resolves\n";
    return false;
  }
  const auto falseFirst = changedMaterial(*falseRig, *falseMaterial, 0.3, 1.0);
  if (!falseFirst) {
    std::cerr << "FAILED: first false-absorb snapshot prepares\n";
    return false;
  }
  falseRig->clock.set(EditorTimePoint{});
  if (!commitMaterialPreview(*falseRig, *falseMaterial, *falseFirst)) {
    std::cerr << "FAILED: first false-absorb preview commits\n";
    return false;
  }
  const HistoryInspection existingBefore =
      EditorStateInspector::inspect(falseRig->history);
  const auto falseSecond = changedMaterial(*falseRig, *falseMaterial, 0.3, 0.2);
  auto incoming = prepareContinuousMaterialCommand(
      falseRig->context, *falseMaterial, &falseRig->fault);
  if (!falseSecond || !incoming.succeeded() ||
      incoming.command
              ->applyPreview(falseRig->context,
                             ContinuousCommandValue{*falseSecond})
              .status != CommandStatus::applied) {
    std::cerr << "FAILED: incoming false-absorb preview prepares and applies\n";
    return false;
  }
  const RetainedCommandInspection incomingBefore =
      incoming.command->inspectRetainedState();
  falseRig->fault.reset();
  if (!falseRig->fault.failOnCall(EditorFaultPoint::allocation, 1u))
    return false;
  falseRig->clock.set(EditorTimePoint{} + 100ms);
  const HistoryResult split =
      falseRig->history.commitExecuted(std::move(incoming.command));
  const HistoryInspection existingAfter =
      EditorStateInspector::inspect(falseRig->history);
  passed &= expect(
      split.status == HistoryStatus::pushed &&
          existingAfter.undoOldestToNewest.size() == 2u &&
          existingAfter.undoOldestToNewest[0].description ==
              existingBefore.undoOldestToNewest[0].description &&
          existingAfter.undoOldestToNewest[0].snapshotBytes ==
              existingBefore.undoOldestToNewest[0].snapshotBytes &&
          existingAfter.undoOldestToNewest[0].retainedCommand ==
              existingBefore.undoOldestToNewest[0].retainedCommand &&
          existingAfter.undoOldestToNewest[1].retainedCommand == incomingBefore,
      "injected compatible false absorption preserves both commands before "
      "split");
  return passed;
}

std::optional<std::uint64_t> probeMaterialCommandBytes(PropertyRig &rig,
                                                       MaterialId material,
                                                       double roughness) {
  const auto before = rig.commands.materialSnapshot(material);
  auto properties = rig.commands.materialProperties(material);
  if (!before || !properties)
    return std::nullopt;
  properties->roughnessFactor = roughness;
  const auto after =
      rig.commands.prepareMaterialSnapshot(material, MaterialEdit{*properties});
  if (!after)
    return std::nullopt;
  auto command =
      prepareEditMaterialCommand(rig.context, *before, *after, &rig.fault);
  return command.succeeded()
             ? std::optional<std::uint64_t>{command.command->snapshotBytes()}
             : std::nullopt;
}

bool executeRoughness(PropertyRig &rig, MaterialId material, double roughness) {
  auto properties = rig.commands.materialProperties(material);
  if (!properties)
    return false;
  properties->roughnessFactor = roughness;
  return executeMaterialEdit(rig, material, MaterialEdit{*properties});
}

bool testSnapshotBudget() {
  auto probe = makeRig();
  if (!expect(probe != nullptr, "property budget probe fixture loads"))
    return false;
  const auto material = idAt<EditorObjectKind::material>(*probe->document, 0);
  if (!material)
    return false;
  const auto bytes = probeMaterialCommandBytes(*probe, *material, 0.2);
  if (!expect(bytes.has_value() && *bytes > 1u,
              "property command exposes nontrivial exact bytes"))
    return false;
  bool passed = true;

  EditorLimits exactLimits = kDefaultEditorLimits;
  exactLimits.maxHistoryEntrySnapshotBytes = *bytes;
  exactLimits.maxHistoryAggregateSnapshotBytes = *bytes;
  auto exact = makeRig(exactLimits);
  const auto exactMaterial =
      exact ? idAt<EditorObjectKind::material>(*exact->document, 0)
            : std::nullopt;
  passed &=
      expect(exactMaterial && executeRoughness(*exact, *exactMaterial, 0.2) &&
                 exact->history.retainedSnapshotBytes() == *bytes,
             "exact property entry and aggregate byte boundary admits");

  EditorLimits overLimits = exactLimits;
  overLimits.maxHistoryEntrySnapshotBytes = *bytes - 1u;
  overLimits.maxHistoryAggregateSnapshotBytes = *bytes - 1u;
  auto over = makeRig(overLimits);
  const auto overMaterial =
      over ? idAt<EditorObjectKind::material>(*over->document, 0)
           : std::nullopt;
  if (!overMaterial)
    return false;
  auto overProperties = over->commands.materialProperties(*overMaterial);
  const auto overBefore = over->commands.materialSnapshot(*overMaterial);
  if (!overProperties || !overBefore)
    return false;
  overProperties->roughnessFactor = 0.2;
  const auto overAfter = over->commands.prepareMaterialSnapshot(
      *overMaterial, MaterialEdit{*overProperties});
  auto overCommand =
      overAfter ? prepareEditMaterialCommand(over->context, *overBefore,
                                             *overAfter, &over->fault)
                : CommandPrepareResult{};
  if (!overCommand.succeeded())
    return false;
  const ExactEditorStateSnapshot beforeOver = exactState(*over);
  const HistoryResult overResult =
      over->history.execute(std::move(overCommand.command));
  passed &= expect(overResult.status == HistoryStatus::rejected &&
                       overResult.command.error ==
                           MutationErrorCode::snapshotBudgetExceeded &&
                       exactState(*over) == beforeOver,
                   "one-byte-over property entry rejects before callback");

  auto aggregateProbe = makeRig();
  if (!aggregateProbe)
    return false;
  const auto firstMaterial =
      idAt<EditorObjectKind::material>(*aggregateProbe->document, 0);
  const auto secondMaterial =
      idAt<EditorObjectKind::material>(*aggregateProbe->document, 1);
  if (!firstMaterial || !secondMaterial)
    return false;
  const auto firstBytes =
      probeMaterialCommandBytes(*aggregateProbe, *firstMaterial, 0.2);
  if (!firstBytes || !executeRoughness(*aggregateProbe, *firstMaterial, 0.2))
    return false;
  const auto secondBytes =
      probeMaterialCommandBytes(*aggregateProbe, *secondMaterial, 0.3);
  std::uint64_t aggregateBytes = 0;
  if (!secondBytes ||
      !editorCheckedAdd(*firstBytes, *secondBytes, aggregateBytes))
    return false;

  EditorLimits aggregateLimits = kDefaultEditorLimits;
  aggregateLimits.maxHistoryEntrySnapshotBytes =
      *firstBytes > *secondBytes ? *firstBytes : *secondBytes;
  aggregateLimits.maxHistoryAggregateSnapshotBytes = aggregateBytes;
  auto aggregateExact = makeRig(aggregateLimits);
  const auto aggregateFirst =
      aggregateExact
          ? idAt<EditorObjectKind::material>(*aggregateExact->document, 0)
          : std::nullopt;
  const auto aggregateSecond =
      aggregateExact
          ? idAt<EditorObjectKind::material>(*aggregateExact->document, 1)
          : std::nullopt;
  passed &= expect(
      aggregateFirst && aggregateSecond &&
          executeRoughness(*aggregateExact, *aggregateFirst, 0.2) &&
          executeRoughness(*aggregateExact, *aggregateSecond, 0.3) &&
          aggregateExact->history.retainedSnapshotBytes() == aggregateBytes,
      "exact custom aggregate boundary admits both complete commands");

  aggregateLimits.maxHistoryAggregateSnapshotBytes = aggregateBytes - 1u;
  auto aggregateOver = makeRig(aggregateLimits);
  const auto aggregateOverFirst =
      aggregateOver
          ? idAt<EditorObjectKind::material>(*aggregateOver->document, 0)
          : std::nullopt;
  const auto aggregateOverSecond =
      aggregateOver
          ? idAt<EditorObjectKind::material>(*aggregateOver->document, 1)
          : std::nullopt;
  if (!aggregateOverFirst || !aggregateOverSecond ||
      !executeRoughness(*aggregateOver, *aggregateOverFirst, 0.2))
    return false;
  auto secondProperties =
      aggregateOver->commands.materialProperties(*aggregateOverSecond);
  const auto secondBefore =
      aggregateOver->commands.materialSnapshot(*aggregateOverSecond);
  if (!secondProperties || !secondBefore)
    return false;
  secondProperties->roughnessFactor = 0.3;
  const auto secondAfter = aggregateOver->commands.prepareMaterialSnapshot(
      *aggregateOverSecond, MaterialEdit{*secondProperties});
  auto secondCommand =
      secondAfter
          ? prepareEditMaterialCommand(aggregateOver->context, *secondBefore,
                                       *secondAfter, &aggregateOver->fault)
          : CommandPrepareResult{};
  if (!secondCommand.succeeded())
    return false;
  const ExactEditorStateSnapshot beforeAggregateOver =
      exactState(*aggregateOver);
  passed &= expect(
      aggregateOver->history.execute(std::move(secondCommand.command))
                  .command.error == MutationErrorCode::snapshotBudgetExceeded &&
          exactState(*aggregateOver) == beforeAggregateOver,
      "one-byte-over aggregate rejection preserves document, redo, and "
      "history");

  auto previewProbe = makeRig();
  const auto previewMaterial =
      previewProbe
          ? idAt<EditorObjectKind::material>(*previewProbe->document, 0)
          : std::nullopt;
  auto previewProperties =
      previewMaterial
          ? previewProbe->commands.materialProperties(*previewMaterial)
          : std::nullopt;
  if (!previewMaterial || !previewProperties)
    return false;
  previewProperties->metallicFactor = 0.4;
  const auto previewFinal = previewProbe->commands.prepareMaterialSnapshot(
      *previewMaterial, MaterialEdit{*previewProperties});
  auto previewCommand = prepareContinuousMaterialCommand(
      previewProbe->context, *previewMaterial, &previewProbe->fault);
  if (!previewFinal || !previewCommand.succeeded())
    return false;
  const RetainedCommandInspection beforeProspective =
      previewCommand.command->inspectRetainedState();
  const ExactEditorStateSnapshot beforePreview = exactState(*previewProbe);
  const SnapshotByteResult prospective =
      previewCommand.command->prospectiveSnapshotBytes(
          ContinuousCommandValue{*previewFinal});
  EditorLimits previewLimits = kDefaultEditorLimits;
  previewLimits.maxHistoryEntrySnapshotBytes = prospective.bytes - 1u;
  previewLimits.maxHistoryAggregateSnapshotBytes = prospective.bytes - 1u;
  ManualEditorClock previewClock;
  EditorHistory constrained(previewProbe->context, previewClock, previewLimits);
  passed &= expect(
      prospective.succeeded() &&
          !constrained.admitProspectivePush(prospective.bytes).admitted &&
          previewCommand.command->inspectRetainedState() == beforeProspective &&
          exactState(*previewProbe) == beforePreview,
      "oversize property preview admission rejects before semantic "
      "publication");
  return passed;
}

bool allocationFreeValidation(EditorCommand &command,
                              const EditorCommandContext &context) {
  allocation_probe::calls.store(0u, std::memory_order_relaxed);
  allocation_probe::enabled.store(true, std::memory_order_release);
  const ExecutedValidation validation = command.validateExecuted(context);
  allocation_probe::enabled.store(false, std::memory_order_release);
  const std::size_t calls =
      allocation_probe::calls.load(std::memory_order_relaxed);
  if (!validation.matches() || calls != 0u) {
    std::cerr << "validation status="
              << static_cast<unsigned>(validation.status)
              << " error=" << static_cast<unsigned>(validation.error)
              << " allocations=" << calls << '\n';
    return false;
  }
  return true;
}

template <typename Prepare>
bool exercisePropertyMutationFault(PropertyRig &rig, Prepare prepare,
                                   EditorFaultPoint point,
                                   std::size_t oneBasedCall,
                                   MutationErrorCode expectedError) {
  rig.fault.reset();
  CommandPrepareResult failedExecute = prepare();
  if (!failedExecute.succeeded())
    return false;
  const ExactEditorStateSnapshot beforeExecute = exactState(rig);
  if (!rig.fault.failOnCall(point, oneBasedCall))
    return false;
  const HistoryResult executeResult =
      rig.history.execute(std::move(failedExecute.command));
  rig.fault.reset();
  if (executeResult.status != HistoryStatus::rejected ||
      executeResult.command.error != expectedError ||
      exactState(rig) != beforeExecute) {
    return false;
  }

  CommandPrepareResult command = prepare();
  if (!command.succeeded() ||
      rig.history.execute(std::move(command.command)).status !=
          HistoryStatus::pushed) {
    return false;
  }
  const ExactEditorStateSnapshot beforeUndo = exactState(rig);
  if (!rig.fault.failOnCall(point, oneBasedCall))
    return false;
  const HistoryResult undoResult = rig.history.undo();
  rig.fault.reset();
  if (undoResult.status != HistoryStatus::rejected ||
      undoResult.command.error != expectedError ||
      exactState(rig) != beforeUndo ||
      rig.history.undo().status != HistoryStatus::moved) {
    return false;
  }

  const ExactEditorStateSnapshot beforeRedo = exactState(rig);
  if (!rig.fault.failOnCall(point, oneBasedCall))
    return false;
  const HistoryResult redoResult = rig.history.redo();
  rig.fault.reset();
  return redoResult.status == HistoryStatus::rejected &&
         redoResult.command.error == expectedError &&
         exactState(rig) == beforeRedo &&
         rig.history.redo().status == HistoryStatus::moved;
}

bool testFailure() {
  bool passed = true;
  for (const std::size_t call : {1u, 2u, 3u}) {
    auto rig = makeRig();
    if (!rig)
      return false;
    const auto material = idAt<EditorObjectKind::material>(*rig->document, 0);
    const auto before =
        material ? rig->commands.materialSnapshot(*material) : std::nullopt;
    auto properties =
        material ? rig->commands.materialProperties(*material) : std::nullopt;
    if (!material || !before || !properties)
      return false;
    properties->roughnessFactor = 0.2;
    const auto after = rig->commands.prepareMaterialSnapshot(
        *material, MaterialEdit{*properties});
    if (!after || !rig->fault.failOnCall(EditorFaultPoint::allocation, call))
      return false;
    const ExactEditorStateSnapshot exact = exactState(*rig);
    const CommandPrepareResult failed =
        prepareEditMaterialCommand(rig->context, *before, *after, &rig->fault);
    passed &= expect(!failed.succeeded() &&
                         failed.error == MutationErrorCode::allocationFailed &&
                         exactState(*rig) == exact,
                     "each property factory allocation seam is no-write");
  }

  for (const std::size_t call : {1u, 2u, 3u}) {
    auto materialRig = makeRig();
    const auto material =
        materialRig
            ? idAt<EditorObjectKind::material>(*materialRig->document, 0)
            : std::nullopt;
    if (!material ||
        !materialRig->fault.failOnCall(EditorFaultPoint::allocation, call)) {
      return false;
    }
    const ExactEditorStateSnapshot materialState = exactState(*materialRig);
    const ContinuousCommandPrepareResult materialFailed =
        prepareContinuousMaterialCommand(materialRig->context, *material,
                                         &materialRig->fault);
    passed &= expect(!materialFailed.succeeded() &&
                         materialFailed.error ==
                             MutationErrorCode::allocationFailed &&
                         exactState(*materialRig) == materialState,
                     "each continuous material factory seam is no-write");

    auto lightRig = makeInlineRig(kRichLightFixture);
    const auto light =
        lightRig ? idAt<EditorObjectKind::light>(*lightRig->document, 0)
                 : std::nullopt;
    if (!light ||
        !lightRig->fault.failOnCall(EditorFaultPoint::allocation, call)) {
      return false;
    }
    const ExactEditorStateSnapshot lightState = exactState(*lightRig);
    const ContinuousCommandPrepareResult lightFailed =
        prepareContinuousLightCommand(lightRig->context, *light,
                                      &lightRig->fault);
    passed &=
        expect(!lightFailed.succeeded() &&
                   lightFailed.error == MutationErrorCode::allocationFailed &&
                   exactState(*lightRig) == lightState,
               "each continuous light factory seam is no-write");
  }

  struct MutationFaultCase final {
    EditorFaultPoint point;
    std::size_t oneBasedCall;
    MutationErrorCode expectedError;
    std::string_view label;
  };
  static constexpr std::array<MutationFaultCase, 3> mutationFaults{{
      {EditorFaultPoint::allocation, 1u, MutationErrorCode::allocationFailed,
       "first allocation"},
      {EditorFaultPoint::allocation, 2u, MutationErrorCode::allocationFailed,
       "second allocation"},
      {EditorFaultPoint::preCandidatePublish, 1u,
       MutationErrorCode::internalError, "pre-publish"},
  }};
  for (const MutationFaultCase &faultCase : mutationFaults) {
    auto materialRig = makeRig();
    const auto material =
        materialRig
            ? idAt<EditorObjectKind::material>(*materialRig->document, 0)
            : std::nullopt;
    auto properties = material
                          ? materialRig->commands.materialProperties(*material)
                          : std::nullopt;
    const auto before = material
                            ? materialRig->commands.materialSnapshot(*material)
                            : std::nullopt;
    if (!material || !properties || !before)
      return false;
    properties->roughnessFactor = 0.25;
    const auto after = materialRig->commands.prepareMaterialSnapshot(
        *material, MaterialEdit{*properties});
    if (!after)
      return false;
    const auto prepareMaterial = [&]() {
      return prepareEditMaterialCommand(materialRig->context, *before, *after,
                                        &materialRig->fault);
    };
    const std::string materialMessage =
        std::string{"material execute/Undo/Redo "} +
        std::string{faultCase.label} + " fault is atomic";
    passed &= expect(exercisePropertyMutationFault(
                         *materialRig, prepareMaterial, faultCase.point,
                         faultCase.oneBasedCall, faultCase.expectedError),
                     materialMessage);

    auto lightRig = makeInlineRig(kRichLightFixture);
    const auto light =
        lightRig ? idAt<EditorObjectKind::light>(*lightRig->document, 0)
                 : std::nullopt;
    auto lightProperties =
        light ? lightRig->commands.lightProperties(*light) : std::nullopt;
    const auto lightBefore =
        light ? lightRig->commands.lightSnapshot(*light) : std::nullopt;
    if (!light || !lightProperties || !lightBefore)
      return false;
    lightProperties->type = "point";
    lightProperties->intensity = 4.0;
    const auto lightAfter =
        lightRig->commands.prepareLightSnapshot(*light, *lightProperties);
    if (!lightAfter)
      return false;
    const auto prepareLight = [&]() {
      return prepareEditLightCommand(lightRig->context, *lightBefore,
                                     *lightAfter, &lightRig->fault);
    };
    const std::string lightMessage = std::string{"light execute/Undo/Redo "} +
                                     std::string{faultCase.label} +
                                     " fault is atomic";
    passed &= expect(exercisePropertyMutationFault(
                         *lightRig, prepareLight, faultCase.point,
                         faultCase.oneBasedCall, faultCase.expectedError),
                     lightMessage);
  }

  auto previewRig = makeRig();
  const auto previewMaterial =
      previewRig ? idAt<EditorObjectKind::material>(*previewRig->document, 0)
                 : std::nullopt;
  auto previewProperties =
      previewMaterial
          ? previewRig->commands.materialProperties(*previewMaterial)
          : std::nullopt;
  if (!previewMaterial || !previewProperties)
    return false;
  previewProperties->metallicFactor = 0.2;
  const auto previewAfter = previewRig->commands.prepareMaterialSnapshot(
      *previewMaterial, MaterialEdit{*previewProperties});
  auto preview = prepareContinuousMaterialCommand(
      previewRig->context, *previewMaterial, &previewRig->fault);
  if (!previewAfter || !preview.succeeded())
    return false;
  const RetainedCommandInspection previewBefore =
      preview.command->inspectRetainedState();
  const ExactEditorStateSnapshot exactPreview = exactState(*previewRig);
  const ContinuousCommandValue wrong{LightSnapshot{}};
  passed &= expect(
      preview.command->prospectiveSnapshotBytes(wrong).error ==
              MutationErrorCode::invalidSnapshot &&
          preview.command->applyPreview(previewRig->context, wrong).error ==
              MutationErrorCode::invalidSnapshot &&
          preview.command->inspectRetainedState() == previewBefore &&
          exactState(*previewRig) == exactPreview,
      "wrong preview variant is a strict no-write rejection");
  static constexpr std::array<MutationFaultCase, 4> previewFaults{{
      {EditorFaultPoint::allocation, 1u, MutationErrorCode::allocationFailed,
       "canonical allocation"},
      {EditorFaultPoint::allocation, 2u, MutationErrorCode::allocationFailed,
       "candidate allocation"},
      {EditorFaultPoint::allocation, 3u, MutationErrorCode::allocationFailed,
       "result allocation"},
      {EditorFaultPoint::preCandidatePublish, 1u,
       MutationErrorCode::internalError, "pre-publish"},
  }};
  for (const MutationFaultCase &faultCase : previewFaults) {
    previewRig->fault.reset();
    if (!previewRig->fault.failOnCall(faultCase.point,
                                      faultCase.oneBasedCall)) {
      return false;
    }
    const CommandResult failedPreview = preview.command->applyPreview(
        previewRig->context, ContinuousCommandValue{*previewAfter});
    previewRig->fault.reset();
    const std::string message = std::string{"preview "} +
                                std::string{faultCase.label} +
                                " fault preserves command and editor";
    passed &=
        expect(failedPreview.status == CommandStatus::rejected &&
                   failedPreview.error == faultCase.expectedError &&
                   preview.command->inspectRetainedState() == previewBefore &&
                   exactState(*previewRig) == exactPreview,
               message);
  }
  passed &=
      expect(preview.command
                     ->applyPreview(previewRig->context,
                                    ContinuousCommandValue{*previewAfter})
                     .status == CommandStatus::applied,
             "property preview remains usable after every injected fault");

  auto validationRig = makeRig();
  const auto validationMaterial =
      validationRig
          ? idAt<EditorObjectKind::material>(*validationRig->document, 0)
          : std::nullopt;
  const auto validationLight =
      validationRig ? idAt<EditorObjectKind::light>(*validationRig->document, 0)
                    : std::nullopt;
  auto validationProperties =
      validationMaterial
          ? validationRig->commands.materialProperties(*validationMaterial)
          : std::nullopt;
  auto validationLightProperties =
      validationLight
          ? validationRig->commands.lightProperties(*validationLight)
          : std::nullopt;
  const auto validationBefore =
      validationMaterial
          ? validationRig->commands.materialSnapshot(*validationMaterial)
          : std::nullopt;
  const auto validationLightBefore =
      validationLight ? validationRig->commands.lightSnapshot(*validationLight)
                      : std::nullopt;
  if (!validationMaterial || !validationLight || !validationProperties ||
      !validationLightProperties || !validationBefore || !validationLightBefore)
    return false;
  validationProperties->roughnessFactor = 0.2;
  validationLightProperties->intensity = 3.0;
  const auto validationAfter = validationRig->commands.prepareMaterialSnapshot(
      *validationMaterial, MaterialEdit{*validationProperties});
  if (!validationAfter)
    return false;
  auto materialCommand =
      prepareEditMaterialCommand(validationRig->context, *validationBefore,
                                 *validationAfter, &validationRig->fault);
  if (!materialCommand.succeeded() ||
      materialCommand.command->execute(validationRig->context).status !=
          CommandStatus::applied)
    return false;
  passed &= expect(allocationFreeValidation(*materialCommand.command,
                                            validationRig->context),
                   "executed material validation allocates zero bytes");

  const auto validationLightAfter =
      validationRig->commands.prepareLightSnapshot(*validationLight,
                                                   *validationLightProperties);
  if (!validationLightAfter)
    return false;
  auto lightCommand =
      prepareEditLightCommand(validationRig->context, *validationLightBefore,
                              *validationLightAfter, &validationRig->fault);
  if (!lightCommand.succeeded() ||
      lightCommand.command->execute(validationRig->context).status !=
          CommandStatus::applied)
    return false;
  passed &= expect(
      allocationFreeValidation(*lightCommand.command, validationRig->context),
      "executed light validation allocates zero bytes");
  return passed;
}

} // namespace

int main(int argc, char **argv) {
  if (argc > 2) {
    std::cerr << "usage: editor_history_property_tests "
                 "[properties|merge|snapshot-budget|failure]\n";
    return EXIT_FAILURE;
  }
  const std::string_view suite = argc == 2 ? argv[1] : std::string_view{};
  bool passed = true;
  if (suite.empty() || suite == "properties") {
    passed &= testProperties();
    passed &= testExtensions();
  }
  if (suite.empty() || suite == "merge")
    passed &= testMerge();
  if (suite.empty() || suite == "snapshot-budget")
    passed &= testSnapshotBudget();
  if (suite.empty() || suite == "failure")
    passed &= testFailure();
  if (!suite.empty() && suite != "properties" && suite != "merge" &&
      suite != "snapshot-budget" && suite != "failure") {
    std::cerr << "unknown suite: " << suite << '\n';
    return EXIT_FAILURE;
  }
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
