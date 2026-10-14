#include <mgif/scene_editor/SceneRenderOccurrenceMapping.h>

#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

using namespace demo::editor;

template <typename T>
concept NativeHandleLeak = requires(T value) {
  value.vkHandle;
  value.d3d12Handle;
  value.metalHandle;
};

static_assert(!NativeHandleLeak<RenderOccurrenceId>);
static_assert(!NativeHandleLeak<EditorPickFence>);
static_assert(std::is_same_v<SceneRenderOccurrenceSnapshot,
                             std::shared_ptr<const SceneRenderOccurrenceMapping>>);

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
  EditorDocumentAuthority authority{41u};
  SceneId scene = id<SceneId>(EditorObjectKind::scene, authority, 1u);
  NodeId root = id<NodeId>(EditorObjectKind::node, authority, 10u);
  NodeId child = id<NodeId>(EditorObjectKind::node, authority, 11u);
  NodeId grandchild = id<NodeId>(EditorObjectKind::node, authority, 12u);
  PrimitiveId primitive =
      id<PrimitiveId>(EditorObjectKind::primitive, authority, 20u);
  PrimitiveId otherPrimitive =
      id<PrimitiveId>(EditorObjectKind::primitive, authority, 21u);

  EditorSceneFence fence() const {
    return {authority, 5u, 7u, scene, 3u, 9u};
  }

  std::vector<SceneRenderNodeRecord> nodes() const {
    return {{root, std::nullopt, 0u},
            {child, root, 1u},
            {grandchild, child, 2u}};
  }

  std::vector<RenderOccurrenceRecord> records() const {
    return {
        {{100u, 1u}, {scene, root, primitive, 0u}, true},
        {{101u, 4u}, {scene, child, primitive, 0u}, true},
        {{102u, 2u}, {scene, child, primitive, 1u}, false},
        {{103u, 8u}, {scene, grandchild, otherPrimitive, 0u}, true},
    };
  }
};

SceneRenderOccurrenceSnapshot mapping(const Fixture &fixture,
                                      std::uint64_t generation = 17u) {
  SceneRenderOccurrenceMappingCreateResult created =
      SceneRenderOccurrenceMapping::create(
          fixture.fence(), generation, fixture.nodes(), fixture.records());
  return created.succeeded() ? std::move(created.snapshot) : nullptr;
}

bool testDefinitionReuseReverseLookupAndTokens() {
  Fixture fixture;
  const SceneRenderOccurrenceSnapshot snapshot = mapping(fixture);
  if (!expect(snapshot != nullptr, "canonical occurrence snapshot seals")) {
    return false;
  }

  const std::vector<RenderOccurrenceRecord> records = fixture.records();
  bool passed = true;
  passed &= expect(snapshot->fence() == fixture.fence() &&
                       snapshot->generation() == 17u &&
                       snapshot->records().size() == records.size() &&
                       snapshot->nodes().size() == fixture.nodes().size(),
                   "snapshot preserves exact fence, generation, and topology");
  passed &= expect(snapshot->resolve(records[1].id) == records[1] &&
                       snapshot->occurrence(records[2].key) == records[2].id &&
                       snapshot->occurrences(fixture.child).size() == 2u &&
                       snapshot->occurrences(fixture.child)[0] == records[1].id &&
                       snapshot->occurrences(fixture.child)[1] == records[2].id,
                   "one definition reused by node/ordinal has deterministic reverse lookup");

  for (std::size_t index = 0; index < records.size(); ++index) {
    const FramebufferPickToken expected{
        static_cast<std::uint32_t>(index + 1u)};
    passed &= expect(snapshot->framebufferToken(records[index].id) == expected &&
                         snapshot->resolveFramebufferToken(expected) ==
                             records[index],
                     "framebuffer token indexes only its sealed record order");
  }
  passed &= expect(!snapshot->resolveFramebufferToken({}) &&
                       !snapshot->resolveFramebufferToken({999u}) &&
                       !snapshot->resolve({records[0].id.slot,
                                           records[0].id.generation + 1u}),
                   "background, out-of-range, and stale generation fail closed");
  return passed;
}

bool testAllocationFailureReturnsStaticErrorWithoutPublishing() {
  Fixture fixture;
  std::vector<SceneRenderNodeRecord> nodes = fixture.nodes();
  std::vector<RenderOccurrenceRecord> records = fixture.records();
  testing::failNextOccurrenceMappingAllocation();
  const SceneRenderOccurrenceMappingCreateResult result =
      SceneRenderOccurrenceMapping::create(
          fixture.fence(), 17u, std::move(nodes), std::move(records));
  return expect(
      result.error == SceneRenderOccurrenceMappingError::allocationFailure &&
          !result.snapshot && !result.message.empty(),
      "allocation failure returns static error view without partial snapshot");
}

bool testRetainedSnapshotPreventsTokenAlias() {
  Fixture fixture;
  SceneRenderOccurrenceSnapshot first = mapping(fixture, 21u);
  if (!expect(first != nullptr, "first retained snapshot seals")) {
    return false;
  }
  std::weak_ptr<const SceneRenderOccurrenceMapping> retained = first;
  const FramebufferPickToken token =
      first->framebufferToken(fixture.records()[0].id).value_or(
          FramebufferPickToken{});

  std::vector<RenderOccurrenceRecord> replacement = fixture.records();
  for (RenderOccurrenceRecord &record : replacement) {
    ++record.id.generation;
  }
  SceneRenderOccurrenceMappingCreateResult created =
      SceneRenderOccurrenceMapping::create(fixture.fence(), 22u,
                                           fixture.nodes(), replacement);
  bool passed = expect(created.succeeded(), "replacement snapshot seals");
  passed &= expect(first->resolveFramebufferToken(token)->id !=
                           created.snapshot->resolveFramebufferToken(token)->id,
                       "same framebuffer value has meaning only in retained snapshot");
  first.reset();
  passed &= expect(retained.expired(),
                   "snapshot lifetime is ordinary immutable shared ownership");
  return passed;
}

bool testInvalidMappingsFailClosed() {
  Fixture fixture;
  bool passed = true;
  auto reject = [&](std::vector<SceneRenderNodeRecord> nodes,
                    std::vector<RenderOccurrenceRecord> records,
                    SceneRenderOccurrenceMappingError expected,
                    std::string_view label) {
    const SceneRenderOccurrenceMappingCreateResult result =
        SceneRenderOccurrenceMapping::create(fixture.fence(), 31u,
                                             std::move(nodes),
                                             std::move(records));
    passed &= expect(!result.succeeded() && result.error == expected, label);
  };

  std::vector<RenderOccurrenceRecord> duplicateSlot = fixture.records();
  duplicateSlot[1].id.slot = duplicateSlot[0].id.slot;
  duplicateSlot[1].id.generation += 1u;
  reject(fixture.nodes(), std::move(duplicateSlot),
         SceneRenderOccurrenceMappingError::duplicateId,
         "two live generations cannot occupy one snapshot slot");

  std::vector<RenderOccurrenceRecord> reversedOrdinal = fixture.records();
  std::swap(reversedOrdinal[1], reversedOrdinal[2]);
  reject(fixture.nodes(), std::move(reversedOrdinal),
         SceneRenderOccurrenceMappingError::nonCanonicalOrdinal,
         "definition ordinals must match deterministic publication order");

  std::vector<SceneRenderNodeRecord> duplicateNode = fixture.nodes();
  duplicateNode.push_back({fixture.child, std::nullopt, 0u});
  reject(std::move(duplicateNode), fixture.records(),
         SceneRenderOccurrenceMappingError::duplicateNode,
         "duplicate node topology is rejected");

  std::vector<SceneRenderNodeRecord> invalidParent = fixture.nodes();
  invalidParent[2].parent = fixture.root;
  reject(std::move(invalidParent), fixture.records(),
         SceneRenderOccurrenceMappingError::invalidNodeTopology,
         "forged non-preorder parent is rejected");

  std::vector<RenderOccurrenceRecord> unknownNode = fixture.records();
  unknownNode[0].key.node =
      id<NodeId>(EditorObjectKind::node, fixture.authority, 999u);
  reject(fixture.nodes(), std::move(unknownNode),
         SceneRenderOccurrenceMappingError::unknownNode,
         "occurrence cannot reference a valid-looking unknown topology node");

  EditorDocumentAuthority foreignAuthority{88u};
  std::vector<RenderOccurrenceRecord> foreign = fixture.records();
  foreign[0].key.node =
      id<NodeId>(EditorObjectKind::node, foreignAuthority, 10u);
  reject(fixture.nodes(), std::move(foreign),
         SceneRenderOccurrenceMappingError::foreignAuthority,
         "foreign occurrence authority is rejected");

  const SceneRenderOccurrenceMappingCreateResult empty =
      SceneRenderOccurrenceMapping::create(fixture.fence(), 32u, {}, {});
  passed &= expect(empty.succeeded() && empty.snapshot->records().empty(),
                   "empty scene seals without manufacturing token zero");
  return passed;
}

bool testHighlightDerivation() {
  Fixture fixture;
  const SceneRenderOccurrenceSnapshot snapshot = mapping(fixture);
  if (!expect(snapshot != nullptr, "highlight snapshot seals")) {
    return false;
  }
  const std::vector<RenderOccurrenceRecord> records = fixture.records();
  bool passed = true;

  const auto root = snapshot->highlight(SceneSelectionValue{fixture.root}, 4u);
  passed &= expect(root && root->valid() &&
                       root->occurrences ==
                           std::vector<RenderOccurrenceId>{records[0].id,
                                                           records[1].id,
                                                           records[3].id},
                   "Node highlight expands visible subtree in document preorder");
  const auto child =
      snapshot->highlight(SceneSelectionValue{fixture.child}, 5u);
  passed &= expect(child &&
                       child->occurrences ==
                           std::vector<RenderOccurrenceId>{records[1].id,
                                                           records[3].id},
                   "child Node highlight cannot escape its sealed subtree");

  const auto exact = snapshot->highlight(
      SceneSelectionValue{records[1].key}, 6u);
  const auto hidden = snapshot->highlight(
      SceneSelectionValue{records[2].key}, 7u);
  passed &= expect(exact && exact->occurrences ==
                                std::vector<RenderOccurrenceId>{records[1].id} &&
                       hidden && hidden->occurrences.empty(),
                   "Primitive highlight is exact and effective-hidden is empty");
  PrimitiveOccurrenceKey unknownPrimitive = records[1].key;
  unknownPrimitive.instanceOrdinal = 9u;
  passed &= expect(
      !snapshot->highlight(SceneSelectionValue{unknownPrimitive}, 8u),
      "unknown Primitive occurrence cannot masquerade as hidden highlight");

  const MeshId mesh =
      id<MeshId>(EditorObjectKind::mesh, fixture.authority, 50u);
  passed &= expect(snapshot->highlight(SceneSelectionValue{mesh}, 9u)
                           ->occurrences.empty() &&
                       snapshot->highlight(SceneSelectionValue{}, 0u)
                           ->occurrences.empty(),
                   "other kinds and empty selection produce no outline");
  const NodeId unknown =
      id<NodeId>(EditorObjectKind::node, fixture.authority, 777u);
  passed &= expect(!snapshot->highlight(SceneSelectionValue{unknown}, 10u),
                   "unknown Node topology never becomes empty-but-valid highlight");
  const NodeId foreign = id<NodeId>(EditorObjectKind::node,
                                    EditorDocumentAuthority{99u}, 10u);
  passed &= expect(!snapshot->highlight(SceneSelectionValue{foreign}, 11u),
                   "foreign selection cannot derive highlight");
  return passed;
}

} // namespace

int main() {
  bool passed = true;
  passed &= testAllocationFailureReturnsStaticErrorWithoutPublishing();
  passed &= testDefinitionReuseReverseLookupAndTokens();
  passed &= testRetainedSnapshotPreventsTokenAlias();
  passed &= testInvalidMappingsFailClosed();
  passed &= testHighlightDerivation();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
