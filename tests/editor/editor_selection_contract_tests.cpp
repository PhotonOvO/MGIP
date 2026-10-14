#include <mgif/scene_editor/EditorSelection.h>

#include <mgif/scene_editor/GltfDocumentIO.h>
#include <mgif/scene_editor/SceneCommands.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if !defined(MGIF_TEST_SOURCE_DIR)
#define MGIF_TEST_SOURCE_DIR "."
#endif

namespace {

using namespace demo::editor;

bool expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
  }
  return condition;
}

std::filesystem::path fixture(const char *name) {
  return std::filesystem::path(MGIF_TEST_SOURCE_DIR) / "tests" / "fixtures" /
         "gltf_editor" / name;
}

std::unique_ptr<GltfDocument> loadDocument(const char *name) {
  GltfLoadResult loaded = GltfDocumentIO::loadCandidate(fixture(name));
  return loaded.succeeded() ? std::move(loaded).takeCandidate() : nullptr;
}

template <typename Id>
Id idAt(const GltfDocument &document, EditorObjectKind kind,
        std::size_t index) {
  return Id::from(document.idAt(kind, index)).value_or(Id{});
}

EditorSelectionSnapshot snapshot(const GltfDocument &document,
                                 EditorSelectionValue value) {
  return EditorSelectionSnapshot{document.documentAuthority(),
                                 std::move(value)};
}

bool testRevisionedAuthorityAndIdempotence() {
  auto document = loadDocument("feature_preservation.gltf");
  if (!expect(document != nullptr, "feature selection fixture loads")) {
    return false;
  }

  const SceneId scene = idAt<SceneId>(*document, EditorObjectKind::scene, 0u);
  const NodeId node = idAt<NodeId>(*document, EditorObjectKind::node, 0u);
  const PrimitiveId primitive =
      idAt<PrimitiveId>(*document, EditorObjectKind::primitive, 0u);
  const MeshId mesh = idAt<MeshId>(*document, EditorObjectKind::mesh, 0u);
  const MaterialId material =
      idAt<MaterialId>(*document, EditorObjectKind::material, 0u);
  const CameraId camera =
      idAt<CameraId>(*document, EditorObjectKind::camera, 0u);
  const LightId light = idAt<LightId>(*document, EditorObjectKind::light, 0u);
  if (!expect(scene.valid() && node.valid() && primitive.valid() &&
                  mesh.valid() && material.valid() && camera.valid() &&
                  light.valid(),
              "fixture exposes every selectable stable ID kind")) {
    return false;
  }

  const std::array<EditorSelectionSnapshot, 6> values{
      snapshot(*document, NodeSelection{scene, node, std::nullopt}),
      snapshot(*document, PrimitiveSelection{scene, node, primitive, mesh}),
      snapshot(*document, mesh),
      snapshot(*document, material),
      snapshot(*document, camera),
      snapshot(*document, light)};
  const std::array<EditorSelectionKind, 6> kinds{
      EditorSelectionKind::node,   EditorSelectionKind::primitive,
      EditorSelectionKind::mesh,   EditorSelectionKind::material,
      EditorSelectionKind::camera, EditorSelectionKind::light};

  EditorSelectionState authority;
  bool passed = true;
  for (std::size_t index = 0; index < values.size(); ++index) {
    const EditorSelectionSnapshot previous = authority.snapshot();
    const std::uint64_t previousRevision = authority.revision();
    const EditorSelectionTransition changed =
        authority.transition(*document, values[index]);
    passed &= expect(changed.changed() && changed.event.has_value(),
                     "a new selection emits exactly one transition event");
    const EditorSelectionEvent emitted =
        changed.event.value_or(EditorSelectionEvent{});
    passed &= expect(
        authority.revision() == previousRevision + 1u &&
            emitted.revision == authority.revision() &&
            emitted.previous == previous && emitted.current == values[index] &&
            authority.snapshot().kind() == kinds[index],
        "the single authority publishes a coherent revision and event");

    const EditorSelectionEvent last =
        authority.lastEvent().value_or(EditorSelectionEvent{});
    const EditorSelectionTransition repeated =
        authority.transition(*document, values[index]);
    passed &= expect(
        repeated.status == EditorSelectionTransitionStatus::unchanged &&
            !repeated.event && authority.revision() == previousRevision + 1u &&
            authority.lastEvent() && *authority.lastEvent() == last,
        "selecting the same value is revision- and event-idempotent");
  }

  const std::uint64_t selectedRevision = authority.revision();
  const EditorSelectionTransition cleared = authority.clearTransition();
  const EditorSelectionTransition repeatedClear = authority.clearTransition();
  passed &= expect(
      cleared.changed() && cleared.event &&
          cleared.event->current == EditorSelectionSnapshot::empty() &&
          authority.revision() == selectedRevision + 1u &&
          repeatedClear.status == EditorSelectionTransitionStatus::unchanged &&
          !repeatedClear.event,
      "clear is an idempotent transition through the same authority");
  return passed;
}

bool testOccurrenceIdentityAndStaleSelection() {
  bool passed = true;
  auto multiScene = loadDocument("multi_scene_oracle.gltf");
  auto sharedMesh = loadDocument("r09_shared_mesh.gltf");
  if (!expect(multiScene != nullptr && sharedMesh != nullptr,
              "occurrence selection fixtures load")) {
    return false;
  }

  const SceneId scene0 =
      idAt<SceneId>(*multiScene, EditorObjectKind::scene, 0u);
  const SceneId scene1 =
      idAt<SceneId>(*multiScene, EditorObjectKind::scene, 1u);
  const NodeId sharedNode =
      idAt<NodeId>(*multiScene, EditorObjectKind::node, 6u);
  EditorSelectionState nodeAuthority;
  const EditorSelectionSnapshot firstOccurrence =
      snapshot(*multiScene, NodeSelection{scene0, sharedNode, std::nullopt});
  const EditorSelectionSnapshot secondOccurrence =
      snapshot(*multiScene, NodeSelection{scene1, sharedNode, std::nullopt});
  passed &= expect(
      nodeAuthority.transition(*multiScene, firstOccurrence).changed() &&
          nodeAuthority.transition(*multiScene, secondOccurrence).changed() &&
          nodeAuthority.revision() == 2u && firstOccurrence != secondOccurrence,
      "the same node in two scenes remains two selectable occurrences");

  const SceneId scene = idAt<SceneId>(*sharedMesh, EditorObjectKind::scene, 0u);
  const NodeId node0 = idAt<NodeId>(*sharedMesh, EditorObjectKind::node, 0u);
  const NodeId node1 = idAt<NodeId>(*sharedMesh, EditorObjectKind::node, 1u);
  const MeshId mesh = idAt<MeshId>(*sharedMesh, EditorObjectKind::mesh, 0u);
  const PrimitiveId primitive =
      idAt<PrimitiveId>(*sharedMesh, EditorObjectKind::primitive, 0u);
  EditorSelectionState primitiveAuthority;
  const EditorSelectionSnapshot primitiveOnNode0 =
      snapshot(*sharedMesh, PrimitiveSelection{scene, node0, primitive, mesh});
  const EditorSelectionSnapshot primitiveOnNode1 =
      snapshot(*sharedMesh, PrimitiveSelection{scene, node1, primitive, mesh});
  passed &= expect(
      primitiveAuthority.transition(*sharedMesh, primitiveOnNode0).changed() &&
          primitiveAuthority.transition(*sharedMesh, primitiveOnNode1)
              .changed() &&
          primitiveAuthority.revision() == 2u &&
          primitiveOnNode0 != primitiveOnNode1,
      "shared mesh/primitive stable IDs retain their owning node occurrence");

  EditorObjectId staleRaw = node1.untyped();
  ++staleRaw.generation;
  const NodeId staleNode = NodeId::from(staleRaw).value_or(NodeId{});
  const EditorSelectionSnapshot stale =
      snapshot(*sharedMesh, NodeSelection{scene, staleNode, std::nullopt});
  const EditorSelectionSnapshot beforeReject = primitiveAuthority.snapshot();
  const std::uint64_t beforeRejectRevision = primitiveAuthority.revision();
  const EditorSelectionTransition rejected =
      primitiveAuthority.transition(*sharedMesh, stale);
  passed &= expect(
      rejected.status == EditorSelectionTransitionStatus::rejectedStale &&
          !rejected.event && primitiveAuthority.snapshot() == beforeReject &&
          primitiveAuthority.revision() == beforeRejectRevision,
      "a stale generation fails closed without changing authority state");

  EditorSelectionState deletedAuthority;
  passed &= expect(
      deletedAuthority.select(
          *sharedMesh,
          snapshot(*sharedMesh, NodeSelection{scene, node1, std::nullopt})),
      "a live node can be selected before deletion");
  const std::uint64_t beforeDeletionRevision = deletedAuthority.revision();
  SceneCommands commands(*sharedMesh);
  const MutationResult deletion = commands.deleteNode(node1);
  const SelectionRestoreStatus revalidated =
      deletedAuthority.revalidate(*sharedMesh);
  passed &= expect(
      deletion.applied() &&
          revalidated == SelectionRestoreStatus::clearedStale &&
          deletedAuthority.snapshot() == EditorSelectionSnapshot::empty() &&
          deletedAuthority.revision() == beforeDeletionRevision + 1u,
      "revalidation clears a selected object deleted after selection");

  auto foreign = loadDocument("r09_shared_mesh.gltf");
  EditorSelectionState foreignAuthority;
  const EditorSelectionTransition foreignResult =
      foreignAuthority.transition(*foreign, primitiveOnNode0);
  passed &= expect(
      foreignResult.status ==
              EditorSelectionTransitionStatus::rejectedForeignAuthority &&
          foreignAuthority.revision() == 0u &&
          foreignAuthority.snapshot() == EditorSelectionSnapshot::empty(),
      "cross-document stable IDs cannot enter another selection authority");
  return passed;
}

bool testStrictMaterialClipboardCodec() {
  bool passed = true;
  const std::array<std::string_view, 10> malformed{"",
                                                   "material:1",
                                                   "gltf_material:",
                                                   "gltf_material:-1",
                                                   "gltf_material:+1",
                                                   "gltf_material: 1",
                                                   "gltf_material:1 ",
                                                   "gltf_material:abc",
                                                   "gltf_material:1tail",
                                                   "gltf_material:1.5"};
  for (const std::string_view value : malformed) {
    passed &= expect(!decodeMaterialClipboardIndex(value),
                     "malformed material clipboard text fails closed");
  }

  const std::string overflow =
      std::string{kMaterialClipboardPrefix} +
      "999999999999999999999999999999999999999999999999";
  passed &= expect(!decodeMaterialClipboardIndex(overflow),
                   "overflowing material clipboard indexes fail closed");
  passed &=
      expect(encodeMaterialClipboardIndex(0u) == "gltf_material:0" &&
                 encodeMaterialClipboardIndex(42u) == "gltf_material:42" &&
                 decodeMaterialClipboardIndex("gltf_material:00") == 0u &&
                 decodeMaterialClipboardIndex(encodeMaterialClipboardIndex(
                     std::numeric_limits<std::size_t>::max())) ==
                     std::numeric_limits<std::size_t>::max(),
             "the codec emits exact ASCII and accepts complete decimal digits");

  auto twoMaterials = loadDocument("r09_shared_mesh.gltf");
  auto oneMaterial = loadDocument("feature_preservation.gltf");
  if (!expect(twoMaterials != nullptr && oneMaterial != nullptr,
              "clipboard validation fixtures load")) {
    return false;
  }
  const MaterialClipboardResolution valid =
      resolveMaterialClipboard(*twoMaterials, "gltf_material:1");
  const MaterialClipboardResolution crossDocument =
      resolveMaterialClipboard(*oneMaterial, "gltf_material:1");
  const MaterialClipboardResolution malformedValue =
      resolveMaterialClipboard(*twoMaterials, "gltf_material:1tail");
  passed &= expect(
      valid.status == MaterialClipboardStatus::resolved &&
          valid.pasteEnabled() && valid.applicable() &&
          valid.documentIndex == 1u && valid.material &&
          valid.material->authority() == twoMaterials->documentAuthority(),
      "an in-range index resolves to the active document stable ID");
  passed &=
      expect(crossDocument.status == MaterialClipboardStatus::outOfRange &&
                 crossDocument.pasteEnabled() && !crossDocument.applicable() &&
                 crossDocument.documentIndex == 1u && !crossDocument.material,
             "the same clipboard index is revalidated in each document");
  passed &=
      expect(malformedValue.status == MaterialClipboardStatus::malformed &&
                 !malformedValue.pasteEnabled() &&
                 !malformedValue.documentIndex && !malformedValue.material,
             "malformed clipboard text never enables Paste or retains an ID");
  return passed;
}

} // namespace

int main() {
  bool passed = true;
  passed &= testRevisionedAuthorityAndIdempotence();
  passed &= testOccurrenceIdentityAndStaleSelection();
  passed &= testStrictMaterialClipboardCodec();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
