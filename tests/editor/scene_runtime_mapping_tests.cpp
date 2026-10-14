#include <mgif/scene_editor/SceneChangeCanonicalizer.h>
#include <mgif/scene_editor/SceneRuntimeMapping.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace demo::editor;

bool expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
  }
  return condition;
}

SceneDigest digest(std::string_view text) {
  return SceneChangeCanonicalizer::computeSha256(
      std::span<const std::uint8_t>(
          reinterpret_cast<const std::uint8_t *>(text.data()), text.size()));
}

EditorObjectId id(EditorObjectKind kind, std::uint32_t slot,
                  std::uint32_t generation = 1u) {
  return EditorObjectId{kind, slot, generation, EditorDocumentAuthority{77u}};
}

SceneRuntimeMappingFence fence() {
  SceneRuntimeMappingFence value;
  value.authority = EditorDocumentAuthority{77u};
  value.appliedDocumentRevision = 12u;
  value.activeScene = stableSceneId(id(EditorObjectKind::scene, 0u));
  value.sceneActivationEpoch = 4u;
  value.rootExternalStateSerial = 9u;
  value.projectionDigest = digest("projection");
  value.mappingEpoch = 6u;
  return value;
}

SceneRuntimeMappingEntry entry(EditorObjectId value, std::uint32_t coordinate,
                               std::uint64_t identity) {
  return SceneRuntimeMappingEntry{value, coordinate, identity, identity};
}

SceneRuntimeMappingAlias alias(EditorObjectId value,
                               std::uint32_t coordinate) {
  return SceneRuntimeMappingAlias{value, value.kind, coordinate};
}

bool testImmutableOrderingAndReverseLookup() {
  std::vector<SceneRuntimeMappingEntry> shuffled{
      entry(id(EditorObjectKind::material, 2u), 1u, 300u),
      entry(id(EditorObjectKind::node, 3u), 1u, 200u),
      entry(id(EditorObjectKind::node, 1u), 0u, 100u),
      {id(EditorObjectKind::texture, 0u),
       SceneRuntimeMapping::kInvalidIndex,
       SceneRuntimeMapping::kInvalidProjectionIdentity,
       SceneRuntimeMapping::kInvalidRuntimeIndex},
  };
  std::string error;
  const std::vector<SceneRuntimeMappingEntry> callerOwned = shuffled;
  std::vector<SceneRuntimeMappingAlias> aliases{
      alias(id(EditorObjectKind::node, 1u), 4u),
      alias(id(EditorObjectKind::node, 1u), 2u),
  };
  const std::vector<SceneRuntimeMappingAlias> callerAliases = aliases;
  const auto first =
      SceneRuntimeMapping::create(fence(), shuffled, aliases, &error);
  bool passed = expect(shuffled == callerOwned,
                       "mapping factory does not mutate caller-owned values");
  passed &= expect(aliases == callerAliases,
                   "mapping factory does not mutate caller-owned aliases");
  std::reverse(shuffled.begin(), shuffled.end());
  std::reverse(aliases.begin(), aliases.end());
  const auto second =
      SceneRuntimeMapping::create(fence(), shuffled, aliases, &error);
  passed &= expect(first && second && *first == *second &&
                       first->entries() == second->entries() &&
                       first->aliases() == second->aliases(),
                   "mapping and alias order are independent of input order");
  passed &= expect(first && first->authority() == EditorDocumentAuthority{77u} &&
                       first->appliedDocumentRevision() == 12u &&
                       first->sceneActivationEpoch() == 4u &&
                       first->rootExternalStateSerial() == 9u &&
                       first->mappingEpoch() == 6u &&
                       first->projectionDigest() == digest("projection"),
                   "immutable mapping retains the complete projection fence");
  passed &= expect(
      first &&
          first->documentIdAt(EditorObjectKind::node, 0u) ==
              id(EditorObjectKind::node, 1u) &&
          first->documentIdAt(EditorObjectKind::node, 2u) ==
              id(EditorObjectKind::node, 1u) &&
          first->documentIdAt(EditorObjectKind::node, 4u) ==
              id(EditorObjectKind::node, 1u) &&
          first->documentIdForProjection(200u) ==
              id(EditorObjectKind::node, 3u) &&
          first->projectionIdentity(id(EditorObjectKind::material, 2u)) ==
              300u &&
          first->runtimeIndex(id(EditorObjectKind::material, 2u)) == 300u &&
          !first->projectionIdentity(id(EditorObjectKind::texture, 0u)) &&
          !first->sceneAssetIndex(id(EditorObjectKind::texture, 0u)) &&
          !first->documentIdAt(EditorObjectKind::texture, 0u) &&
          first->digest() == second->digest(),
      "forward, reverse, alias, and unprojected lookups are exact");
  return passed;
}

bool testAliasDigestAndTopologySensitivity() {
  const std::vector<SceneRuntimeMappingEntry> entries{
      entry(id(EditorObjectKind::node, 1u), 0u, 100u),
  };
  std::string error;
  const auto oneAlias = SceneRuntimeMapping::create(
      fence(), entries, {alias(id(EditorObjectKind::node, 1u), 1u)},
      &error);
  const auto twoAliases = SceneRuntimeMapping::create(
      fence(), entries,
      {alias(id(EditorObjectKind::node, 1u), 2u),
       alias(id(EditorObjectKind::node, 1u), 1u)},
      &error);
  const auto differentAlias = SceneRuntimeMapping::create(
      fence(), entries, {alias(id(EditorObjectKind::node, 1u), 2u)},
      &error);
  const auto reordered = SceneRuntimeMapping::create(
      fence(), entries,
      {alias(id(EditorObjectKind::node, 1u), 1u),
       alias(id(EditorObjectKind::node, 1u), 2u)},
      &error);
  const auto latePrimary = SceneRuntimeMapping::create(
      fence(), {entry(id(EditorObjectKind::node, 1u), 5u, 100u)},
      {alias(id(EditorObjectKind::node, 1u), 3u),
       alias(id(EditorObjectKind::node, 1u), 1u)},
      &error);
  return expect(
      oneAlias && differentAlias && twoAliases && reordered && latePrimary &&
          *twoAliases == *reordered &&
          *oneAlias != *differentAlias &&
          oneAlias->digest() != differentAlias->digest() &&
          oneAlias->digest() != twoAliases->digest() &&
          twoAliases->digest() == reordered->digest() &&
          latePrimary->sceneAssetIndex(id(EditorObjectKind::node, 1u)) == 1u &&
          latePrimary->documentIdAt(EditorObjectKind::node, 1u) ==
              id(EditorObjectKind::node, 1u) &&
          latePrimary->documentIdAt(EditorObjectKind::node, 3u) ==
              id(EditorObjectKind::node, 1u) &&
          latePrimary->documentIdAt(EditorObjectKind::node, 5u) ==
              id(EditorObjectKind::node, 1u),
      "mapping equality, digest, and first-coordinate canonicalization include alias topology");
}

bool testInvalidAndReusedSlots() {
  std::string error;
  const auto reused = SceneRuntimeMapping::create(
      fence(),
      {entry(id(EditorObjectKind::node, 5u, 1u), 0u, 10u),
       entry(id(EditorObjectKind::node, 5u, 2u), 1u, 11u)},
      &error);
  bool passed = expect(!reused && !error.empty(),
                       "two generations of one stable slot reject");
  const auto duplicateCoordinate = SceneRuntimeMapping::create(
      fence(),
      {entry(id(EditorObjectKind::node, 1u), 0u, 10u),
       entry(id(EditorObjectKind::node, 2u), 0u, 11u)},
      &error);
  passed &= expect(!duplicateCoordinate,
                   "duplicate kind/SceneAsset coordinate rejects");
  const auto duplicateIdentity = SceneRuntimeMapping::create(
      fence(),
      {entry(id(EditorObjectKind::node, 1u), 0u, 10u),
       entry(id(EditorObjectKind::node, 2u), 1u, 10u)},
      &error);
  passed &= expect(!duplicateIdentity,
                   "duplicate projection identity rejects");
  SceneRuntimeMappingEntry mismatched =
      entry(id(EditorObjectKind::node, 1u), 0u, 10u);
  mismatched.runtimeIndex = 20u;
  passed &= expect(!SceneRuntimeMapping::create(fence(), {mismatched}, &error),
                   "compatibility alias cannot disagree with projection identity");
  SceneRuntimeMappingFence invalidFence = fence();
  invalidFence.mappingEpoch = 0u;
  passed &= expect(!SceneRuntimeMapping::create(
                        invalidFence,
                        {entry(id(EditorObjectKind::node, 1u), 0u, 10u)},
                        &error),
                   "zero mapping epoch rejects");
  SceneRuntimeMappingFence foreignActive = fence();
  foreignActive.activeScene->authority = EditorDocumentAuthority{99u};
  passed &= expect(
      !SceneRuntimeMapping::create(
          foreignActive,
          {entry(id(EditorObjectKind::node, 1u), 0u, 10u)}, &error),
      "foreign active-scene identity rejects before mapping publication");
  const std::vector<SceneRuntimeMappingEntry> aliasEntries{
      entry(id(EditorObjectKind::node, 1u), 0u, 10u),
      entry(id(EditorObjectKind::node, 2u), 1u, 11u),
      {id(EditorObjectKind::node, 3u),
       SceneRuntimeMapping::kInvalidIndex,
       SceneRuntimeMapping::kInvalidProjectionIdentity,
       SceneRuntimeMapping::kInvalidRuntimeIndex},
  };
  passed &= expect(
      !SceneRuntimeMapping::create(
          fence(), aliasEntries,
          {alias(id(EditorObjectKind::node, 4u), 2u)}, &error),
      "alias without a matching primary entry rejects");
  passed &= expect(
      !SceneRuntimeMapping::create(
          fence(), aliasEntries,
          {alias(id(EditorObjectKind::node, 3u), 2u)}, &error),
      "alias whose primary is unprojected rejects");
  SceneRuntimeMappingAlias foreignAlias =
      alias(id(EditorObjectKind::node, 1u), 2u);
  foreignAlias.documentId.authority = EditorDocumentAuthority{99u};
  passed &= expect(
      !SceneRuntimeMapping::create(fence(), aliasEntries, {foreignAlias},
                                   &error),
      "foreign alias rejects");
  SceneRuntimeMappingAlias mismatchedKind =
      alias(id(EditorObjectKind::node, 1u), 2u);
  mismatchedKind.kind = EditorObjectKind::mesh;
  passed &= expect(
      !SceneRuntimeMapping::create(fence(), aliasEntries, {mismatchedKind},
                                   &error),
      "alias kind mismatch rejects");
  passed &= expect(
      !SceneRuntimeMapping::create(
          fence(), aliasEntries,
          {alias(id(EditorObjectKind::node, 1u), 2u),
           alias(id(EditorObjectKind::node, 1u), 2u)},
          &error),
      "duplicate alias coordinate rejects");
  passed &= expect(
      !SceneRuntimeMapping::create(
          fence(), aliasEntries,
          {alias(id(EditorObjectKind::node, 1u), 2u),
           alias(id(EditorObjectKind::node, 2u), 2u)},
          &error),
      "conflicting alias coordinate owners reject");
  passed &= expect(
      !SceneRuntimeMapping::create(
          fence(), aliasEntries,
          {alias(id(EditorObjectKind::node, 1u), 0u)}, &error),
      "alias cannot repeat its primary coordinate");
  return passed;
}

} // namespace

int main() {
  bool passed = true;
  passed &= testImmutableOrderingAndReverseLookup();
  passed &= testAliasDigestAndTopologySensitivity();
  passed &= testInvalidAndReusedSlots();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
