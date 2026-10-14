#include <mgif/scene_editor/SceneChangeCanonicalizer.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace demo::editor;

constexpr EditorDocumentAuthority kAuthority{0x12345678u};

bool expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
  }
  return condition;
}

SceneDigest digestOf(std::string_view text) {
  return SceneChangeCanonicalizer::computeSha256(std::span{
      reinterpret_cast<const std::uint8_t *>(text.data()), text.size()});
}

StableSceneId id(EditorObjectKind kind, std::uint32_t slot,
                 std::uint32_t generation = 1u) {
  return StableSceneId{kind, kAuthority, slot, generation};
}

SceneDigest revisionDigest(std::uint64_t revision) {
  return digestOf("document-" + std::to_string(revision));
}

SceneChangeHeader documentHeader(std::uint64_t baseRevision = 10u,
                                 std::uint64_t stepCount = 1u,
                                 std::uint64_t firstSerial = 100u) {
  SceneChangeHeader header;
  header.domain = SceneChangeDomain::DocumentMutation;
  header.authority = kAuthority;
  header.baseDocumentRevision = baseRevision;
  header.targetDocumentRevision = baseRevision + stepCount;
  header.baseSceneActivationEpoch = 7u;
  header.targetSceneActivationEpoch = 7u;
  header.baseRootExternalStateSerial = 3u;
  header.targetRootExternalStateSerial = 3u;
  header.baseActiveScene = id(EditorObjectKind::scene, 1u);
  header.targetActiveScene = header.baseActiveScene;
  header.firstMutationSerial = firstSerial;
  header.lastMutationSerial = firstSerial + stepCount - 1u;
  header.baseDocumentDigest = revisionDigest(baseRevision);
  header.targetDocumentDigest = revisionDigest(baseRevision + stepCount);
  header.baseProjectionDigest = digestOf("projection-base");
  header.targetProjectionDigest = digestOf("projection-target");
  for (std::uint64_t step = 0u; step < stepCount; ++step) {
    SourceMutationFence source;
    source.baseDocumentRevision = baseRevision + step;
    source.targetDocumentRevision = baseRevision + step + 1u;
    source.mutationToken = SceneMutationToken{
        kAuthority, source.targetDocumentRevision, firstSerial + step};
    source.baseDocumentDigest = revisionDigest(source.baseDocumentRevision);
    source.targetDocumentDigest = revisionDigest(source.targetDocumentRevision);
    header.sourceMutations.push_back(source);
  }
  header.mutationToken = header.sourceMutations.back().mutationToken;
  return header;
}

SceneChangeHeader activationHeader() {
  SceneChangeHeader header;
  header.domain = SceneChangeDomain::SceneActivation;
  header.authority = kAuthority;
  header.baseDocumentRevision = 12u;
  header.targetDocumentRevision = 12u;
  header.baseSceneActivationEpoch = 8u;
  header.targetSceneActivationEpoch = 9u;
  header.baseRootExternalStateSerial = 4u;
  header.targetRootExternalStateSerial = 4u;
  header.baseActiveScene = id(EditorObjectKind::scene, 1u);
  header.targetActiveScene = id(EditorObjectKind::scene, 2u);
  header.firstMutationSerial = 200u;
  header.lastMutationSerial = 200u;
  header.baseDocumentDigest = revisionDigest(12u);
  header.targetDocumentDigest = header.baseDocumentDigest;
  header.baseProjectionDigest = digestOf("activation-base");
  header.targetProjectionDigest = digestOf("activation-target");
  return header;
}

SceneChangeHeader rootHeader() {
  SceneChangeHeader header = activationHeader();
  header.domain = SceneChangeDomain::SceneRootState;
  header.baseSceneActivationEpoch = 9u;
  header.targetSceneActivationEpoch = 9u;
  header.targetActiveScene = header.baseActiveScene;
  header.baseRootExternalStateSerial = 4u;
  header.targetRootExternalStateSerial = 5u;
  header.firstMutationSerial = 201u;
  header.lastMutationSerial = 201u;
  header.baseProjectionDigest = digestOf("root-base");
  header.targetProjectionDigest = digestOf("root-target");
  return header;
}

SceneChangeHeader snapshotHeader() {
  SceneChangeHeader header;
  header.domain = SceneChangeDomain::FullSnapshot;
  header.authority = kAuthority;
  header.baseDocumentRevision = 12u;
  header.targetDocumentRevision = 18u;
  header.baseSceneActivationEpoch = 9u;
  header.targetSceneActivationEpoch = 11u;
  header.baseRootExternalStateSerial = 5u;
  header.targetRootExternalStateSerial = 8u;
  header.baseActiveScene = id(EditorObjectKind::scene, 1u);
  header.targetActiveScene = id(EditorObjectKind::scene, 2u);
  header.firstMutationSerial = 202u;
  header.lastMutationSerial = 202u;
  header.baseDocumentDigest = revisionDigest(12u);
  header.targetDocumentDigest = revisionDigest(18u);
  header.baseProjectionDigest = digestOf("snapshot-base");
  header.targetProjectionDigest = digestOf("snapshot-target");
  return header;
}

FullSnapshotReference fullSnapshotFor(const SceneChangeHeader &header) {
  FullSnapshotReference full;
  full.reason = FullRebuildReason::StructuralTopology;
  full.immutableTarget.schemaVersion = kImmutableSceneSnapshotSchemaVersion;
  full.immutableTarget.authority = header.authority;
  full.immutableTarget.documentRevision = header.targetDocumentRevision;
  full.immutableTarget.sceneActivationEpoch = header.targetSceneActivationEpoch;
  full.immutableTarget.rootExternalStateSerial =
      header.targetRootExternalStateSerial;
  full.immutableTarget.activeScene = header.targetActiveScene;
  full.immutableTarget.documentDigest = header.targetDocumentDigest;
  full.immutableTarget.projectionDigest = header.targetProjectionDigest;
  full.immutableTarget.bytes = {0x4du, 0x47u, 0x49u, 0x46u, 0x01u};
  full.immutableTarget.contentDigest =
      SceneChangeCanonicalizer::computeSha256(full.immutableTarget.bytes);
  full.targetSnapshotDigest = full.immutableTarget.contentDigest;
  full.stableIdRemap = {
      StableIdRemapEvidence{std::nullopt, id(EditorObjectKind::node, 9u)},
      StableIdRemapEvidence{id(EditorObjectKind::node, 7u), std::nullopt},
  };
  full.stableIdRemapComplete = true;
  return full;
}

SceneNodeTransformValue transform(double x) {
  SceneTrsValue trs;
  trs.translation[0] = x;
  return SceneNodeTransformValue{trs};
}

SceneTopologyValue topology(EditorObjectKind kind, std::string_view schema) {
  SceneTopologyValue value;
  value.objectKind = kind;
  value.schemaDigest = digestOf(schema);
  value.elementCount = 1u;
  return value;
}

SceneFieldChange update(StableSceneId target, SceneChangeClass changeClass,
                        SceneField field, std::uint64_t serial,
                        SceneFieldValue before, SceneFieldValue after) {
  return SceneFieldChange{
      target, changeClass,       SceneChangeAction::Update, field,
      serial, std::move(before), std::move(after)};
}

SceneFieldChange create(StableSceneId target, SceneChangeClass changeClass,
                        SceneField field, std::uint64_t serial,
                        SceneFieldValue after) {
  return SceneFieldChange{
      target, changeClass,  SceneChangeAction::Create, field,
      serial, std::nullopt, std::move(after)};
}

SceneFieldChange erase(StableSceneId target, SceneChangeClass changeClass,
                       SceneField field, std::uint64_t serial,
                       SceneFieldValue before) {
  return SceneFieldChange{target,      changeClass, SceneChangeAction::Delete,
                          field,       serial,      std::move(before),
                          std::nullopt};
}

std::vector<SceneFieldChange> rootChanges(const SceneChangeHeader &header) {
  return {update(*header.targetActiveScene, SceneChangeClass::Transform,
                 SceneField::SceneRootTransform, header.firstMutationSerial,
                 transform(0.0), transform(1.0))};
}

SceneCanonicalizationResult
canonicalize(SceneChangeHeader header,
             std::vector<SceneFieldChange> changes = {},
             std::vector<StableIdRemapEvidence> remap = {}) {
  SceneChangeSet set;
  set.header = std::move(header);
  set.canonicalChanges = std::move(changes);
  set.stableIdRemap = std::move(remap);
  return SceneChangeCanonicalizer::canonicalize(std::move(set));
}

bool testSha256AndEmptyInput() {
  bool passed = true;
  constexpr std::array<std::uint8_t, 3> abc{'a', 'b', 'c'};
  const SceneDigest expected{0xbau, 0x78u, 0x16u, 0xbfu, 0x8fu, 0x01u, 0xcfu,
                             0xeau, 0x41u, 0x41u, 0x40u, 0xdeu, 0x5du, 0xaeu,
                             0x22u, 0x23u, 0xb0u, 0x03u, 0x61u, 0xa3u, 0x96u,
                             0x17u, 0x7au, 0x9cu, 0xb4u, 0x10u, 0xffu, 0x61u,
                             0xf2u, 0x00u, 0x15u, 0xadu};
  passed &= expect(SceneChangeCanonicalizer::computeSha256(abc) == expected,
                   "SHA-256 uses the standard canonical digest");
  constexpr std::array<std::uint8_t, 6> text{'f', 'o', 'o', 'b', 'a', 'r'};
  constexpr std::array<std::string_view, 7> base64{
      "", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"};
  for (std::size_t size = 0; size <= text.size(); ++size)
    passed &= expect(SceneChangeCanonicalizer::encodeBase64(std::span{text}.first(size)) == base64[size],
                     "Base64 keeps exact canonical bytes and padding without line breaks");
  const auto hexDigest = [](const SceneDigest& digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string text;
    for (const auto byte : digest) {
      text += digits[byte >> 4u];
      text += digits[byte & 0x0fu];
    }
    return text;
  };
  passed &= expect(hexDigest(SceneChangeCanonicalizer::computeSha256({})) ==
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
      "SHA-256 preserves the standard empty-input digest");
  const std::vector<std::uint8_t> largeInput(16u * 1024u * 1024u + 23u, 'a');
  passed &= expect(hexDigest(SceneChangeCanonicalizer::computeSha256(largeInput)) ==
      "fd431f0fa0edb5a9d722d5914998c51adabfe8b0c7f8d31ef1c401230c603e94",
      "SHA-256 remains exact across native streaming chunks");
  const auto empty = canonicalize(documentHeader());
  std::vector<std::uint8_t> streamInput(337u);
  for (std::size_t i = 0u; i < streamInput.size(); ++i)
    streamInput[i] = static_cast<std::uint8_t>(i * 73u + 29u);
  const auto whole = std::span<const std::uint8_t>(streamInput);
  for (const std::size_t cut : {0u, 1u, 63u, 64u, 65u, 129u, 257u, 337u}) {
    const std::array parts{whole.first(cut), whole.subspan(cut, 0u), whole.subspan(cut)};
    passed &= expect(SceneChangeCanonicalizer::computeSha256Parts(parts) ==
                         SceneChangeCanonicalizer::computeSha256(whole),
                     "fragmented SHA-256 preserves byte ordering across block boundaries");
    const std::array prefixParts{whole.first(cut)};
    const auto prefix = SceneChangeCanonicalizer::captureSha256Prefix(prefixParts);
    passed &= expect(prefix.valid() &&
                         SceneChangeCanonicalizer::computeSha256WithPrefix(prefix, whole.subspan(cut)) ==
                             SceneChangeCanonicalizer::computeSha256(whole) &&
                         SceneChangeCanonicalizer::computeSha256WithPrefix(prefix, {}) ==
                             SceneChangeCanonicalizer::computeSha256(whole.first(cut)) &&
                         SceneChangeCanonicalizer::computeSha256WithPrefix(prefix, whole.subspan(cut)) ==
                             SceneChangeCanonicalizer::computeSha256(whole),
                     "reused SHA-256 prefix state is immutable and retains partial blocks");
  }
  passed &= expect(
      empty.succeeded() && empty.changeSet->canonicalChanges.empty(),
      "empty exact evidence has an explicit deterministic canonical set");
  constexpr std::array<std::uint8_t, 8> magic{'M', 'G', 'I', 'F',
                                              'S', 'C', 'N', '1'};
  passed &= expect(
      empty.canonicalBytes.size() > 12u &&
          std::equal(magic.begin(), magic.end(),
                     empty.canonicalBytes.begin()) &&
          empty.canonicalBytes[8] == 1u && empty.canonicalBytes[9] == 0u &&
          empty.canonicalBytes[10] == 0u && empty.canonicalBytes[11] == 0u,
      "canonical serialization freezes schema tag and little-endian widths");
  return passed;
}

bool testSharedImmutableByteOwnership() {
  std::vector<std::uint8_t> producer{1u, 2u, 3u, 4u};
  demo::ImmutableByteBuffer bytes(producer);
  auto retained = bytes;
  producer[0] = 99u;
  bool passed = expect(bytes.front() == 1u && retained.data() == bytes.data(),
                       "immutable copies share owned bytes independently of producer storage");
  auto chunks = demo::ImmutableByteBuffer::concatenate({
      demo::ImmutableByteBuffer{9u}, bytes, demo::ImmutableByteBuffer{},
      demo::ImmutableByteBuffer{8u, 7u}});
  const auto originalChunks = chunks.chunks();
  bytes.clear();
  auto queued = chunks;
  chunks = {0u};
  const demo::ImmutableByteBuffer flat{9u, 1u, 2u, 3u, 4u, 8u, 7u};
  passed &= expect(queued == flat && queued.size() == 7u &&
                       originalChunks.size() == 3u &&
                       originalChunks[1].data() == retained.data(),
                   "queued fragmented snapshots retain payload owners without copying their bytes");
  passed &= expect(queued.vector() == flat.vector() &&
                       SceneChangeCanonicalizer::computeSha256Parts(queued.chunks()) ==
                           SceneChangeCanonicalizer::computeSha256(flat),
                   "contiguous full-snapshot consumption preserves fragmented bytes and digest");
  return passed;
}

bool testShuffledDeterminismAndWriteChains() {
  const StableSceneId firstNode = id(EditorObjectKind::node, 2u);
  const StableSceneId secondNode = id(EditorObjectKind::node, 3u);
  std::vector<SceneFieldChange> ordered{
      update(firstNode, SceneChangeClass::Transform,
             SceneField::NodeLocalTransform, 100u, transform(0.0),
             transform(1.0)),
      update(firstNode, SceneChangeClass::Transform,
             SceneField::NodeLocalTransform, 101u, transform(1.0),
             transform(2.0)),
      update(secondNode, SceneChangeClass::Visibility,
             SceneField::NodeLocalVisibility, 101u, SceneVisibilityValue{true},
             SceneVisibilityValue{false}),
  };
  auto shuffled = ordered;
  std::reverse(shuffled.begin(), shuffled.end());
  const auto canonicalA = canonicalize(documentHeader(10u, 2u, 100u), ordered);
  const auto canonicalB = canonicalize(documentHeader(10u, 2u, 100u), shuffled);
  bool passed = true;
  passed &= expect(canonicalA.succeeded() && canonicalB.succeeded(),
                   "shuffled logical evidence canonicalizes");
  passed &=
      expect(canonicalA.canonicalBytes == canonicalB.canonicalBytes &&
                 canonicalA.changeSet->header.canonicalChangeSetDigest ==
                     canonicalB.changeSet->header.canonicalChangeSetDigest,
             "input order cannot affect canonical bytes or digest");
  std::array<std::size_t, 3> permutation{0u, 1u, 2u};
  do {
    std::vector<SceneFieldChange> permuted;
    for (std::size_t index : permutation) {
      permuted.push_back(ordered[index]);
    }
    const auto candidate =
        canonicalize(documentHeader(10u, 2u, 100u), std::move(permuted));
    passed &=
        expect(candidate.succeeded() &&
                   candidate.canonicalBytes == canonicalA.canonicalBytes,
               "every field-order permutation has one canonical byte stream");
  } while (std::next_permutation(permutation.begin(), permutation.end()));
  passed &= expect(canonicalA.changeSet->canonicalChanges.size() == 2u,
                   "last-write chain coalesces to one final field update");
  const auto first = std::find_if(
      canonicalA.changeSet->canonicalChanges.begin(),
      canonicalA.changeSet->canonicalChanges.end(),
      [firstNode](const auto &change) { return change.target == firstNode; });
  passed &=
      expect(first != canonicalA.changeSet->canonicalChanges.end() &&
                 first->beforeValue == SceneFieldValue{transform(0.0)} &&
                 first->afterValue == SceneFieldValue{transform(2.0)} &&
                 first->mutationSerial == 101u,
             "coalescing retains first before, final after, and last serial");

  auto duplicate = ordered;
  duplicate.push_back(ordered.front());
  const auto deduplicated =
      canonicalize(documentHeader(10u, 2u, 100u), duplicate);
  passed &= expect(deduplicated.succeeded() &&
                       deduplicated.canonicalBytes == canonicalA.canonicalBytes,
                   "exact duplicate input is idempotent");

  auto discontinuous = ordered;
  discontinuous[1].beforeValue = transform(7.0);
  const auto rejected =
      canonicalize(documentHeader(10u, 2u, 100u), discontinuous);
  passed &= expect(!rejected.succeeded() &&
                       rejected.validation.error ==
                           SceneChangeValidationError::DiscontinuousWriteChain,
                   "discontinuous intermediate values reject without output");
  auto sameSerialConflict =
      std::vector<SceneFieldChange>{ordered.front(), ordered.front()};
  sameSerialConflict.back().afterValue = transform(9.0);
  const auto conflicting = canonicalize(documentHeader(), sameSerialConflict);
  passed &= expect(
      !conflicting.succeeded() &&
          conflicting.validation.error ==
              SceneChangeValidationError::ConflictingWrite,
      "same-serial conflicting values reject without input-order tie-breaking");
  const auto signedZero = canonicalize(
      documentHeader(), {update(firstNode, SceneChangeClass::Transform,
                                SceneField::NodeLocalTransform, 100u,
                                transform(0.0), transform(-0.0))});
  passed &=
      expect(signedZero.succeeded() &&
                 signedZero.changeSet->canonicalChanges.size() == 1u,
             "canonical IEEE bits distinguish positive and negative zero");
  return passed;
}

bool testEnvelopeRules() {
  bool passed = true;
  passed &= expect(canonicalize(documentHeader()).succeeded(),
                   "valid unbatched document envelope succeeds");
  passed &= expect(canonicalize(documentHeader(10u, 3u, 100u)).succeeded(),
                   "complete contiguous batched document envelope succeeds");

  auto missingStep = documentHeader(10u, 3u, 100u);
  missingStep.sourceMutations.erase(missingStep.sourceMutations.begin() + 1);
  passed &= expect(!canonicalize(missingStep).succeeded(),
                   "document envelope missing one source revision rejects");
  auto brokenDigest = documentHeader(10u, 2u, 100u);
  brokenDigest.sourceMutations[1].baseDocumentDigest = digestOf("wrong");
  passed &= expect(!canonicalize(brokenDigest).succeeded(),
                   "discontinuous source document digest rejects");

  const auto batchedHeader = documentHeader(10u, 2u, 100u);
  SceneMutationEvidence firstEvidence;
  firstEvidence.sourceMutation = batchedHeader.sourceMutations[0];
  SceneMutationEvidence secondEvidence;
  secondEvidence.sourceMutation = batchedHeader.sourceMutations[1];
  passed &= expect(
      SceneChangeCanonicalizer::canonicalizeEvidence(
          batchedHeader, {firstEvidence, secondEvidence})
          .succeeded(),
      "producer evidence batches through the complete ordered source chain");
  passed &= expect(!SceneChangeCanonicalizer::canonicalizeEvidence(
                        batchedHeader, {secondEvidence, firstEvidence})
                        .succeeded(),
                   "reordered producer evidence source chain rejects");
  auto brokenToken = documentHeader(10u, 2u, 100u);
  brokenToken.sourceMutations[1].mutationToken.serial =
      brokenToken.sourceMutations[0].mutationToken.serial;
  passed &= expect(!canonicalize(brokenToken).succeeded(),
                   "duplicate or reordered source token serial rejects");
  auto interleavedToken = documentHeader(10u, 2u, 100u);
  interleavedToken.sourceMutations[1].mutationToken.serial += 4u;
  interleavedToken.lastMutationSerial =
      interleavedToken.sourceMutations[1].mutationToken.serial;
  interleavedToken.mutationToken =
      interleavedToken.sourceMutations[1].mutationToken;
  passed &= expect(canonicalize(interleavedToken).succeeded(),
                   "complete revision chain permits strictly ordered global "
                   "tokens interleaved with another authority");
  auto missingToken = documentHeader();
  missingToken.mutationToken.reset();
  passed &=
      expect(!canonicalize(missingToken).succeeded(),
             "unbatched document envelope requires its exact mutation token");
  auto rootJump = documentHeader();
  rootJump.targetRootExternalStateSerial += 2u;
  passed &= expect(!canonicalize(rootJump).succeeded(),
                   "document mutation root serial cannot jump");
  auto frozenRoot = documentHeader();
  frozenRoot.targetRootExternalStateSerial++;
  passed &= expect(
      canonicalize(frozenRoot, {update(*frozenRoot.targetActiveScene,
                                       SceneChangeClass::Transform,
                                       SceneField::SceneRootTransform, 100u,
                                       transform(0.0), transform(1.0))})
          .succeeded(),
      "document mutation may freeze one exact root state in the same commit");
  auto activationChanged = documentHeader();
  activationChanged.targetSceneActivationEpoch++;
  passed &= expect(!canonicalize(activationChanged).succeeded(),
                   "document mutation cannot change activation epoch");

  passed &= expect(canonicalize(activationHeader()).succeeded(),
                   "valid scene activation envelope succeeds");
  auto invalidActivation = activationHeader();
  invalidActivation.targetDocumentRevision++;
  passed &= expect(!canonicalize(invalidActivation).succeeded(),
                   "scene activation cannot change document revision");
  invalidActivation = activationHeader();
  invalidActivation.targetActiveScene = invalidActivation.baseActiveScene;
  passed &= expect(!canonicalize(invalidActivation).succeeded(),
                   "idempotent scene activation is not a published transition");
  invalidActivation = activationHeader();
  invalidActivation.targetActiveScene = StableSceneId{};
  passed &= expect(
      !canonicalize(invalidActivation).succeeded(),
      "active-scene absence cannot be represented by a fabricated invalid ID");
  invalidActivation = activationHeader();
  invalidActivation.mutationToken =
      SceneMutationToken{kAuthority, invalidActivation.targetDocumentRevision,
                         invalidActivation.firstMutationSerial};
  passed &= expect(!canonicalize(invalidActivation).succeeded(),
                   "scene activation carries no document mutation token");
  auto noActive = activationHeader();
  noActive.targetActiveScene.reset();
  passed &= expect(canonicalize(noActive).succeeded(),
                   "optional absence is the valid no-active-scene identity");

  passed &=
      expect(canonicalize(rootHeader(), rootChanges(rootHeader())).succeeded(),
             "valid scene root-state envelope succeeds");
  auto invalidRoot = rootHeader();
  invalidRoot.targetSceneActivationEpoch++;
  passed &=
      expect(!canonicalize(invalidRoot, rootChanges(invalidRoot)).succeeded(),
             "scene root state cannot change activation epoch");
  invalidRoot = rootHeader();
  invalidRoot.targetDocumentDigest = digestOf("changed-document");
  passed &=
      expect(!canonicalize(invalidRoot, rootChanges(invalidRoot)).succeeded(),
             "scene root state cannot change document digest");
  invalidRoot = rootHeader();
  invalidRoot.targetActiveScene = id(EditorObjectKind::scene, 3u);
  passed &=
      expect(!canonicalize(invalidRoot, rootChanges(invalidRoot)).succeeded(),
             "scene root state cannot change active scene identity");
  auto rootWithUnrelatedField = rootChanges(rootHeader());
  rootWithUnrelatedField.push_back(
      update(id(EditorObjectKind::node, 4u), SceneChangeClass::Visibility,
             SceneField::NodeLocalVisibility, rootHeader().firstMutationSerial,
             SceneVisibilityValue{true}, SceneVisibilityValue{false}));
  passed &= expect(
      !canonicalize(rootHeader(), rootWithUnrelatedField).succeeded(),
      "scene root-state envelope cannot carry unrelated field mutations");
  auto idempotentRoot = rootChanges(rootHeader());
  idempotentRoot.front().afterValue = idempotentRoot.front().beforeValue;
  passed &= expect(!canonicalize(rootHeader(), idempotentRoot).succeeded(),
                   "root clock cannot advance for an idempotent root value");
  passed &=
      expect(!canonicalize(rootHeader()).succeeded(),
             "scene root state cannot omit its exact immutable root transform");

  SceneChangeSet fullSet;
  fullSet.header = snapshotHeader();
  fullSet.fullSnapshot = fullSnapshotFor(fullSet.header);
  const auto full = SceneChangeCanonicalizer::canonicalize(fullSet);
  passed &= expect(
      full.succeeded() && full.changeSet->canonicalChanges.empty() &&
          full.changeSet->stableIdRemap.size() == 2u,
      "full snapshot may jump clocks with complete immutable target and remap");
  auto initialSnapshot = fullSet;
  initialSnapshot.header.firstMutationSerial = 0u;
  initialSnapshot.header.lastMutationSerial = 0u;
  passed &= expect(
      SceneChangeCanonicalizer::canonicalize(initialSnapshot).succeeded(),
      "full snapshot without a source mutation may use an empty serial range");
  auto missingTarget = fullSet;
  missingTarget.fullSnapshot->immutableTarget.bytes.clear();
  passed &=
      expect(!SceneChangeCanonicalizer::canonicalize(missingTarget).succeeded(),
             "full snapshot requires a complete immutable target");
  auto incompleteRemap = fullSet;
  incompleteRemap.fullSnapshot->stableIdRemapComplete = false;
  passed &= expect(
      !SceneChangeCanonicalizer::canonicalize(incompleteRemap).succeeded(),
      "full snapshot requires explicit complete remap evidence");
  auto wrongSnapshotHash = fullSet;
  auto corruptedBytes = wrongSnapshotHash.fullSnapshot->immutableTarget.bytes.vector();
  corruptedBytes[0] ^= 1u;
  wrongSnapshotHash.fullSnapshot->immutableTarget.bytes = std::move(corruptedBytes);
  passed &= expect(
      !SceneChangeCanonicalizer::canonicalize(wrongSnapshotHash).succeeded(),
      "full snapshot bytes must match the verified target snapshot digest");
  auto wrongSnapshotFence = fullSet;
  wrongSnapshotFence.fullSnapshot->immutableTarget.documentRevision++;
  passed &= expect(
      !SceneChangeCanonicalizer::canonicalize(wrongSnapshotFence).succeeded(),
      "full snapshot immutable target must bind every target envelope "
      "identity");
  auto backwardsSnapshot = fullSet;
  backwardsSnapshot.header.targetDocumentRevision = 11u;
  backwardsSnapshot.header.targetDocumentDigest = revisionDigest(11u);
  backwardsSnapshot.fullSnapshot = fullSnapshotFor(backwardsSnapshot.header);
  passed &= expect(
      !SceneChangeCanonicalizer::canonicalize(backwardsSnapshot).succeeded(),
      "full snapshot cannot move a monotonic envelope backwards");
  auto incrementalWithSnapshot = fullSet;
  incrementalWithSnapshot.header = documentHeader();
  passed &=
      expect(!SceneChangeCanonicalizer::canonicalize(incrementalWithSnapshot)
                  .succeeded(),
             "incremental domain cannot hide a full snapshot");
  return passed;
}

bool testSchemaIdentityAndFieldValidation() {
  bool passed = true;
  auto header = documentHeader();
  header.schemaVersion++;
  auto result = canonicalize(header);
  passed &= expect(!result.succeeded() &&
                       result.validation.error ==
                           SceneChangeValidationError::InvalidSchema &&
                       !result.changeSet,
                   "unsupported schema rejects without output");

  header = documentHeader();
  header.authority = {};
  result = canonicalize(header);
  passed &= expect(!result.succeeded() &&
                       result.validation.error ==
                           SceneChangeValidationError::InvalidAuthority,
                   "invalid document authority rejects");

  header = documentHeader();
  header.baseProjectionDigest = {};
  result = canonicalize(header);
  passed &= expect(!result.succeeded() &&
                       result.validation.error ==
                           SceneChangeValidationError::InvalidContentHash,
                   "absent mandatory envelope digest rejects");

  auto invalidTarget = id(EditorObjectKind::node, 2u);
  invalidTarget.generation = 0u;
  result = canonicalize(documentHeader(),
                        {update(invalidTarget, SceneChangeClass::Transform,
                                SceneField::NodeLocalTransform, 100u,
                                transform(0.0), transform(1.0))});
  passed &= expect(!result.succeeded() &&
                       result.validation.error ==
                           SceneChangeValidationError::InvalidStableId,
                   "invalid object kind/generation identity rejects");

  StableSceneId foreign = id(EditorObjectKind::node, 2u);
  foreign.authority = EditorDocumentAuthority{0x999u};
  result = canonicalize(documentHeader(),
                        {update(foreign, SceneChangeClass::Transform,
                                SceneField::NodeLocalTransform, 100u,
                                transform(0.0), transform(1.0))});
  passed &= expect(!result.succeeded() &&
                       result.validation.error ==
                           SceneChangeValidationError::CrossAuthority,
                   "cross-authority stable identity rejects");

  result = canonicalize(
      documentHeader(),
      {update(id(EditorObjectKind::node, 2u), SceneChangeClass::Transform,
              SceneField::NodeLocalTransform, 100u, SceneVisibilityValue{true},
              SceneVisibilityValue{false})});
  passed &= expect(!result.succeeded() &&
                       result.validation.error ==
                           SceneChangeValidationError::InvalidFieldValue,
                   "tagged field payload type mismatch rejects");
  return passed;
}

bool testCheckedOverflow() {
  bool passed = true;
  auto activation = activationHeader();
  activation.baseSceneActivationEpoch =
      std::numeric_limits<std::uint64_t>::max();
  activation.targetSceneActivationEpoch = 0u;
  auto result = canonicalize(activation);
  passed &= expect(!result.succeeded() &&
                       result.validation.error ==
                           SceneChangeValidationError::MonotonicOverflow,
                   "activation epoch overflow rejects before publication");
  auto root = rootHeader();
  root.baseRootExternalStateSerial = std::numeric_limits<std::uint64_t>::max();
  root.targetRootExternalStateSerial = 0u;
  result = canonicalize(root, rootChanges(root));
  passed &= expect(
      !result.succeeded() && result.validation.error ==
                                 SceneChangeValidationError::MonotonicOverflow,
      "root external-state serial overflow rejects before publication");
  auto document = documentHeader();
  document.baseDocumentRevision = std::numeric_limits<std::uint64_t>::max();
  document.targetDocumentRevision = 0u;
  document.sourceMutations.front().baseDocumentRevision =
      document.baseDocumentRevision;
  document.sourceMutations.front().targetDocumentRevision = 0u;
  document.sourceMutations.front().mutationToken.revision = 0u;
  document.mutationToken = document.sourceMutations.front().mutationToken;
  result = canonicalize(document);
  passed &= expect(!result.succeeded() &&
                       result.validation.error ==
                           SceneChangeValidationError::MonotonicOverflow,
                   "document revision overflow rejects before publication");
  auto serialOverflow = documentHeader(10u, 2u, 100u);
  serialOverflow.sourceMutations[0].mutationToken.serial =
      std::numeric_limits<std::uint64_t>::max();
  serialOverflow.sourceMutations[1].mutationToken.serial = 0u;
  serialOverflow.firstMutationSerial =
      std::numeric_limits<std::uint64_t>::max();
  serialOverflow.lastMutationSerial = std::numeric_limits<std::uint64_t>::max();
  serialOverflow.mutationToken.reset();
  result = canonicalize(serialOverflow);
  passed &= expect(
      !result.succeeded() && result.validation.error ==
                                 SceneChangeValidationError::MonotonicOverflow,
      "source mutation serial overflow rejects before publication");
  return passed;
}

bool testActionAlgebraAndGenerationRecreate() {
  bool passed = true;
  const StableSceneId node = id(EditorObjectKind::node, 4u);
  auto header = documentHeader(10u, 2u, 100u);
  const SceneFieldChange created =
      create(node, SceneChangeClass::Transform, SceneField::NodeLocalTransform,
             100u, transform(1.0));
  const SceneFieldChange changed =
      update(node, SceneChangeClass::Transform, SceneField::NodeLocalTransform,
             101u, transform(1.0), transform(2.0));
  const auto createUpdate = canonicalize(header, {changed, created});
  passed &=
      expect(createUpdate.succeeded() &&
                 createUpdate.changeSet->canonicalChanges.size() == 1u &&
                 createUpdate.changeSet->canonicalChanges.front().action ==
                     SceneChangeAction::Create &&
                 createUpdate.changeSet->canonicalChanges.front().afterValue ==
                     SceneFieldValue{transform(2.0)},
             "create plus updates folds into one complete create");
  const auto canceled = canonicalize(
      header, {
                  created,
                  erase(node, SceneChangeClass::Transform,
                        SceneField::NodeLocalTransform, 101u, transform(1.0)),
              });
  passed &= expect(canceled.succeeded() &&
                       canceled.changeSet->canonicalChanges.empty(),
                   "unpublished unreferenced create/delete cancels");
  const auto duplicateCreate =
      canonicalize(header, {created, create(node, SceneChangeClass::Transform,
                                            SceneField::NodeLocalTransform,
                                            101u, transform(2.0))});
  passed &= expect(!duplicateCreate.succeeded() &&
                       duplicateCreate.validation.error ==
                           SceneChangeValidationError::DuplicateCreate,
                   "two distinct creates of one field reject");
  const auto updateDelete = canonicalize(
      header, {
                  update(node, SceneChangeClass::Transform,
                         SceneField::NodeLocalTransform, 100u, transform(0.0),
                         transform(1.0)),
                  erase(node, SceneChangeClass::Transform,
                        SceneField::NodeLocalTransform, 101u, transform(1.0)),
              });
  passed &=
      expect(updateDelete.succeeded() &&
                 updateDelete.changeSet->canonicalChanges.size() == 1u &&
                 updateDelete.changeSet->canonicalChanges.front().action ==
                     SceneChangeAction::Delete &&
                 updateDelete.changeSet->canonicalChanges.front().beforeValue ==
                     SceneFieldValue{transform(0.0)},
             "update plus delete folds to delete with the original complete "
             "before value");

  const StableSceneId oldNode = id(EditorObjectKind::node, 8u, 2u);
  const StableSceneId newNode = id(EditorObjectKind::node, 8u, 3u);
  const auto recreated =
      canonicalize(header,
                   {
                       create(newNode, SceneChangeClass::Topology,
                              SceneField::TopologyMetadata, 101u,
                              topology(EditorObjectKind::node, "new-node")),
                       erase(oldNode, SceneChangeClass::Topology,
                             SceneField::TopologyMetadata, 100u,
                             topology(EditorObjectKind::node, "old-node")),
                   },
                   {{oldNode, newNode}});
  passed &= expect(
      recreated.succeeded() &&
          recreated.changeSet->canonicalChanges.size() == 2u &&
          recreated.changeSet->canonicalChanges[0].target == oldNode &&
          recreated.changeSet->canonicalChanges[0].action ==
              SceneChangeAction::Delete &&
          recreated.changeSet->canonicalChanges[1].target == newNode &&
          recreated.changeSet->canonicalChanges[1].action ==
              SceneChangeAction::Create,
      "generation-changing recreate preserves old delete before new create");
  const auto missingRecreateRemap = canonicalize(
      header,
      {create(newNode, SceneChangeClass::Topology, SceneField::TopologyMetadata,
              101u, topology(EditorObjectKind::node, "new-node")),
       erase(oldNode, SceneChangeClass::Topology, SceneField::TopologyMetadata,
             100u, topology(EditorObjectKind::node, "old-node"))});
  passed &= expect(!missingRecreateRemap.succeeded() &&
                       missingRecreateRemap.validation.error ==
                           SceneChangeValidationError::IncompleteRemap,
                   "generation-changing recreate requires complete stable-ID "
                   "remap evidence");

  auto invalidPresence = created;
  invalidPresence.beforeValue = transform(0.0);
  const auto invalid = canonicalize(documentHeader(), {invalidPresence});
  passed &= expect(!invalid.succeeded() &&
                       invalid.validation.error ==
                           SceneChangeValidationError::InvalidActionValues,
                   "Create before/after presence is exact");
  invalidPresence =
      update(node, SceneChangeClass::Transform, SceneField::NodeLocalTransform,
             100u, transform(0.0), transform(1.0));
  invalidPresence.beforeValue.reset();
  passed &=
      expect(!canonicalize(documentHeader(), {invalidPresence}).succeeded(),
             "Update requires complete before and after values");
  invalidPresence = erase(node, SceneChangeClass::Transform,
                          SceneField::NodeLocalTransform, 100u, transform(0.0));
  invalidPresence.afterValue = transform(1.0);
  passed &=
      expect(!canonicalize(documentHeader(), {invalidPresence}).succeeded(),
             "Delete requires complete before and absent after values");
  return passed;
}

bool testCameraAtomicEvidenceAndClassification() {
  bool passed = true;
  SceneCameraValue beforeCamera;
  SceneCameraValue afterCamera = beforeCamera;
  afterCamera.yfov = 0.9;
  SceneMutationEvidence evidence;
  evidence.sourceMutation = documentHeader().sourceMutations.front();
  evidence.exactChanges = {
      update(id(EditorObjectKind::camera, 2u), SceneChangeClass::Camera,
             SceneField::CameraParameters, 100u, beforeCamera, afterCamera),
      update(id(EditorObjectKind::node, 5u), SceneChangeClass::Transform,
             SceneField::NodeLocalTransform, 100u, transform(0.0),
             transform(3.0)),
  };
  const auto result = SceneChangeCanonicalizer::canonicalizeEvidence(
      documentHeader(), {evidence});
  passed &= expect(result.succeeded() &&
                       result.changeSet->canonicalChanges.size() == 2u &&
                       std::all_of(result.changeSet->canonicalChanges.begin(),
                                   result.changeSet->canonicalChanges.end(),
                                   [](const auto &change) {
                                     return change.mutationSerial == 100u;
                                   }),
                   "camera-from-view is one token/revision with camera and "
                   "node-transform evidence");
  const auto camera = std::find_if(
      result.changeSet->canonicalChanges.begin(),
      result.changeSet->canonicalChanges.end(), [](const auto &change) {
        return change.target.kind == EditorObjectKind::camera;
      });
  passed &=
      expect(camera != result.changeSet->canonicalChanges.end() &&
                 camera->changeClass == SceneChangeClass::Camera,
             "camera parameters retain first-class Camera classification");
  auto misclassified =
      update(id(EditorObjectKind::camera, 2u), SceneChangeClass::Light,
             SceneField::CameraParameters, 100u, beforeCamera, afterCamera);
  const auto invalid = canonicalize(documentHeader(), {misclassified});
  passed &= expect(!invalid.succeeded() &&
                       invalid.validation.error ==
                           SceneChangeValidationError::FieldClassMismatch,
                   "camera fields cannot be hidden under another class");

  ScenePrimitiveMaterialBindingValue variantBefore;
  variantBefore.material = id(EditorObjectKind::material, 1u);
  variantBefore.sourceVariant = 4u;
  ScenePrimitiveMaterialBindingValue variantAfter;
  variantAfter.material = id(EditorObjectKind::material, 2u);
  variantAfter.sourceVariant = 7u;
  const auto variant = canonicalize(
      documentHeader(), {update(id(EditorObjectKind::primitive, 3u),
                                SceneChangeClass::MaterialBinding,
                                SceneField::PrimitiveMaterialBinding, 100u,
                                variantBefore, variantAfter)});
  passed &= expect(
      variant.succeeded() &&
          !std::get<ScenePrimitiveMaterialBindingValue>(
               *variant.changeSet->canonicalChanges.front().afterValue)
               .sourceVariant,
      "variant convenience input resolves to final primitive-material binding");
  return passed;
}

bool testPayloadRangeAndHashValidation() {
  bool passed = true;
  SceneMaterialExtensionValue extension;
  extension.extensionName = "KHR_materials_test";
  extension.canonicalValueBytes = {1u, 2u, 3u};
  extension.contentDigest = digestOf("wrong");
  auto invalid = canonicalize(
      documentHeader(),
      {
          update(id(EditorObjectKind::material, 2u),
                 SceneChangeClass::MaterialParameters,
                 SceneField::MaterialExtension, 100u, extension, extension),
      });
  passed &= expect(!invalid.succeeded() &&
                       invalid.validation.error ==
                           SceneChangeValidationError::InvalidContentHash,
                   "material extension payload hash mismatch rejects");

  SceneBufferRangeValue range;
  range.logicalOffset = std::numeric_limits<std::uint64_t>::max();
  range.byteSize = 1u;
  range.schemaDigest = digestOf("geometry-schema");
  range.bytes = {7u};
  range.contentDigest = SceneChangeCanonicalizer::computeSha256(range.bytes);
  invalid = canonicalize(
      documentHeader(),
      {
          update(id(EditorObjectKind::buffer, 2u), SceneChangeClass::Topology,
                 SceneField::GeometryBufferRange, 100u, range, range),
      });
  passed &= expect(!invalid.succeeded() &&
                       invalid.validation.error ==
                           SceneChangeValidationError::InvalidRange,
                   "overflowing payload range rejects");

  SceneTexturePayloadValue texture;
  texture.width = 0u;
  texture.height = 1u;
  texture.formatTag = 1u;
  texture.schemaDigest = digestOf("texture-schema");
  texture.bytes = {9u};
  texture.contentDigest =
      SceneChangeCanonicalizer::computeSha256(texture.bytes);
  invalid = canonicalize(
      documentHeader(),
      {
          create(id(EditorObjectKind::image, 2u), SceneChangeClass::Topology,
                 SceneField::TexturePayload, 100u, texture),
      });
  passed &= expect(!invalid.succeeded() &&
                       invalid.validation.error ==
                           SceneChangeValidationError::InvalidPayload,
                   "invalid texture payload dimensions reject");

  auto nanTransform = transform(0.0);
  std::get<SceneTrsValue>(nanTransform.value).translation[0] =
      std::numeric_limits<double>::quiet_NaN();
  invalid = canonicalize(
      documentHeader(),
      {
          update(id(EditorObjectKind::node, 2u), SceneChangeClass::Transform,
                 SceneField::NodeLocalTransform, 100u, transform(0.0),
                 nanTransform),
      });
  passed &= expect(!invalid.succeeded() &&
                       invalid.validation.error ==
                           SceneChangeValidationError::InvalidRange,
                   "non-finite numeric payload rejects");
  return passed;
}

bool testImmutableValuesRemapAndSnapshot() {
  bool passed = true;
  std::vector<std::uint8_t> sourceBytes{4u, 5u, 6u};
  SceneMaterialExtensionValue extension;
  extension.extensionName = "EXT_owned";
  extension.canonicalValueBytes = sourceBytes;
  extension.contentDigest =
      SceneChangeCanonicalizer::computeSha256(sourceBytes);
  SceneFieldValue ownedValue = extension;
  sourceBytes[0] = 99u;
  passed &= expect(
      std::get<SceneMaterialExtensionValue>(ownedValue)
              .canonicalValueBytes.front() == 4u,
      "field payload owns immutable bytes independently of producer storage");

  SceneChangeSet fullSet;
  fullSet.header = snapshotHeader();
  fullSet.fullSnapshot = fullSnapshotFor(fullSet.header);
  auto retainedCopy = fullSet.fullSnapshot->immutableTarget;
  auto replacedBytes = fullSet.fullSnapshot->immutableTarget.bytes.vector();
  replacedBytes[0] = 0u;
  fullSet.fullSnapshot->immutableTarget.bytes = std::move(replacedBytes);
  passed &= expect(retainedCopy.bytes.front() == 0x4du,
                   "immutable snapshot value owns its deferred lifetime");
  fullSet.fullSnapshot = fullSnapshotFor(fullSet.header);
  const auto canonical = SceneChangeCanonicalizer::canonicalize(fullSet);
  passed &=
      expect(canonical.succeeded() &&
                 canonical.changeSet->stableIdRemap.front().oldId.has_value(),
             "complete remap is retained in stable old/create/delete order");

  auto invalidRemap = fullSet;
  invalidRemap.fullSnapshot->stableIdRemap.push_back(
      {std::nullopt, std::nullopt});
  passed &=
      expect(!SceneChangeCanonicalizer::canonicalize(invalidRemap).succeeded(),
             "remap entry missing both identities rejects");
  return passed;
}

bool testCanonicalizeEvidenceFullSnapshotRemapUnion() {
  bool passed = true;
  auto header = snapshotHeader();
  const SceneChangeHeader sourceHeader = documentHeader();
  header.baseDocumentRevision = sourceHeader.baseDocumentRevision;
  header.targetDocumentRevision = sourceHeader.targetDocumentRevision;
  header.firstMutationSerial = sourceHeader.firstMutationSerial;
  header.lastMutationSerial = sourceHeader.lastMutationSerial;
  header.baseDocumentDigest = sourceHeader.baseDocumentDigest;
  header.targetDocumentDigest = sourceHeader.targetDocumentDigest;
  header.mutationToken = sourceHeader.mutationToken;
  header.sourceMutations = sourceHeader.sourceMutations;

  const StableSceneId snapshotOld = id(EditorObjectKind::node, 30u);
  const StableSceneId snapshotNew = id(EditorObjectKind::node, 30u, 2u);
  const StableSceneId evidenceOld = id(EditorObjectKind::node, 20u);
  const StableSceneId evidenceNew = id(EditorObjectKind::node, 20u, 2u);
  const StableIdRemapEvidence snapshotRemap{snapshotOld, snapshotNew};
  const StableIdRemapEvidence evidenceRemap{evidenceOld, evidenceNew};

  FullSnapshotReference snapshot = fullSnapshotFor(header);
  snapshot.stableIdRemap = {snapshotRemap};
  SceneMutationEvidence evidence;
  evidence.sourceMutation = header.sourceMutations.front();
  evidence.stableIdRemap = {evidenceRemap};
  const auto unionResult = SceneChangeCanonicalizer::canonicalizeEvidence(
      header, {evidence}, snapshot);
  const std::vector<StableIdRemapEvidence> expectedUnion{evidenceRemap,
                                                         snapshotRemap};
  passed &= expect(
      unionResult.succeeded() &&
          unionResult.changeSet->stableIdRemap == expectedUnion &&
          unionResult.changeSet->fullSnapshot->stableIdRemap == expectedUnion,
      "snapshot and mutation evidence produce one stable normalized remap "
      "union");
  passed &= expect(snapshot.stableIdRemap ==
                           std::vector<StableIdRemapEvidence>{snapshotRemap} &&
                       evidence.stableIdRemap ==
                           std::vector<StableIdRemapEvidence>{evidenceRemap},
                   "canonicalizeEvidence preserves caller-owned remap values");
  if (unionResult.succeeded()) {
    passed &= expect(SceneChangeCanonicalizer::validateCanonical(
                         *unionResult.changeSet, unionResult.canonicalBytes)
                         .valid(),
                     "canonical remap union validates against its bytes");
    auto tamperedBytes = unionResult.canonicalBytes;
    tamperedBytes.back() ^= 1u;
    passed &= expect(!SceneChangeCanonicalizer::validateCanonical(
                          *unionResult.changeSet, tamperedBytes)
                          .valid(),
                     "tampered canonical remap-union bytes reject");
  }

  const auto canonicalizeRemaps =
      [&](std::vector<StableIdRemapEvidence> snapshotRemaps,
          std::vector<StableIdRemapEvidence> evidenceRemaps) {
        FullSnapshotReference candidateSnapshot = fullSnapshotFor(header);
        candidateSnapshot.stableIdRemap = std::move(snapshotRemaps);
        SceneMutationEvidence candidateEvidence;
        candidateEvidence.sourceMutation = header.sourceMutations.front();
        candidateEvidence.stableIdRemap = std::move(evidenceRemaps);
        return SceneChangeCanonicalizer::canonicalizeEvidence(
            header, {std::move(candidateEvidence)},
            std::move(candidateSnapshot));
      };

  const auto duplicate = canonicalizeRemaps({snapshotRemap}, {snapshotRemap});
  passed &= expect(duplicate.succeeded() &&
                       duplicate.changeSet->stableIdRemap.size() == 1u &&
                       duplicate.changeSet->stableIdRemap ==
                           duplicate.changeSet->fullSnapshot->stableIdRemap,
                   "duplicate snapshot and mutation remaps normalize once in "
                   "both locations");

  const auto oldConflict = canonicalizeRemaps(
      {snapshotRemap}, {{snapshotOld, id(EditorObjectKind::node, 30u, 3u)}});
  passed &=
      expect(!oldConflict.succeeded() &&
                 oldConflict.validation.error ==
                     SceneChangeValidationError::IncompleteRemap,
             "one old identity mapping to distinct new identities rejects");

  const auto newConflict = canonicalizeRemaps(
      {snapshotRemap}, {{id(EditorObjectKind::node, 31u), snapshotNew}});
  passed &=
      expect(!newConflict.succeeded() &&
                 newConflict.validation.error ==
                     SceneChangeValidationError::IncompleteRemap,
             "distinct old identities mapping to one new identity reject");
  return passed;
}

bool testDependencyOrderingAndStructuralFailures() {
  bool passed = true;
  auto header = documentHeader();
  const StableSceneId material = id(EditorObjectKind::material, 20u);
  const StableSceneId primitive = id(EditorObjectKind::primitive, 1u);
  ScenePrimitiveMaterialBindingValue detached;
  ScenePrimitiveMaterialBindingValue attached;
  attached.material = material;
  const auto backingBeforeBinding = canonicalize(
      header,
      {
          create(primitive, SceneChangeClass::MaterialBinding,
                 SceneField::PrimitiveMaterialBinding, 100u, attached),
          create(material, SceneChangeClass::Topology,
                 SceneField::TopologyMetadata, 100u,
                 topology(EditorObjectKind::material, "material")),
      },
      {{std::nullopt, material}});
  passed &= expect(
      backingBeforeBinding.succeeded() &&
          backingBeforeBinding.changeSet->canonicalChanges.front().target ==
              material,
      "backing creation is ordered before binding publication");

  const auto detachBeforeDelete =
      canonicalize(header,
                   {
                       erase(material, SceneChangeClass::Topology,
                             SceneField::TopologyMetadata, 100u,
                             topology(EditorObjectKind::material, "material")),
                       update(primitive, SceneChangeClass::MaterialBinding,
                              SceneField::PrimitiveMaterialBinding, 100u,
                              attached, detached),
                   },
                   {{material, std::nullopt}});
  passed &= expect(
      detachBeforeDelete.succeeded() &&
          detachBeforeDelete.changeSet->canonicalChanges.front().target ==
              primitive,
      "detach is ordered before backing deletion");

  SceneTopologyValue primitiveTopology =
      topology(EditorObjectKind::primitive, "primitive");
  primitiveTopology.dependencies = {material};
  const auto reverseRemoval = canonicalize(
      header,
      {
          erase(material, SceneChangeClass::Topology,
                SceneField::TopologyMetadata, 100u,
                topology(EditorObjectKind::material, "material")),
          erase(primitive, SceneChangeClass::Topology,
                SceneField::TopologyMetadata, 100u, primitiveTopology),
      },
      {{material, std::nullopt}, {primitive, std::nullopt}});
  passed &=
      expect(reverseRemoval.succeeded() &&
                 reverseRemoval.changeSet->canonicalChanges.front().target ==
                     primitive,
             "reverse dependency retirement removes dependent before backing");

  auto survivorHeader = documentHeader(10u, 2u, 100u);
  const auto survivorReference = canonicalize(
      survivorHeader,
      {
          create(material, SceneChangeClass::Topology,
                 SceneField::TopologyMetadata, 100u,
                 topology(EditorObjectKind::material, "material")),
          erase(material, SceneChangeClass::Topology,
                SceneField::TopologyMetadata, 101u,
                topology(EditorObjectKind::material, "material")),
          create(primitive, SceneChangeClass::MaterialBinding,
                 SceneField::PrimitiveMaterialBinding, 101u, attached),
      });
  passed &= expect(!survivorReference.succeeded() &&
                       survivorReference.validation.error ==
                           SceneChangeValidationError::NonClosedHierarchy,
                   "create/delete cancellation rejects while a survivor "
                   "references the target");

  const StableSceneId parent = id(EditorObjectKind::node, 30u);
  const StableSceneId child = id(EditorObjectKind::node, 2u);
  SceneHierarchyValue beforeHierarchy;
  SceneHierarchyValue afterHierarchy;
  afterHierarchy.parent = parent;
  const auto hierarchy =
      canonicalize(header, {
                               update(child, SceneChangeClass::Transform,
                                      SceneField::NodeLocalTransform, 100u,
                                      transform(0.0), transform(2.0)),
                               update(parent, SceneChangeClass::Transform,
                                      SceneField::NodeLocalTransform, 100u,
                                      transform(0.0), transform(1.0)),
                               update(child, SceneChangeClass::Hierarchy,
                                      SceneField::HierarchyParent, 100u,
                                      beforeHierarchy, afterHierarchy),
                           });
  passed &=
      expect(hierarchy.succeeded(), "closed hierarchy evidence canonicalizes");
  if (hierarchy.succeeded()) {
    const auto parentTransform =
        std::find_if(hierarchy.changeSet->canonicalChanges.begin(),
                     hierarchy.changeSet->canonicalChanges.end(),
                     [parent](const auto &change) {
                       return change.target == parent &&
                              change.changeClass == SceneChangeClass::Transform;
                     });
    const auto childTransform =
        std::find_if(hierarchy.changeSet->canonicalChanges.begin(),
                     hierarchy.changeSet->canonicalChanges.end(),
                     [child](const auto &change) {
                       return change.target == child &&
                              change.changeClass == SceneChangeClass::Transform;
                     });
    passed &= expect(parentTransform < childTransform,
                     "derived transform work is parent before child despite "
                     "stable-ID order");
  }

  SceneHierarchyValue aAfter;
  SceneHierarchyValue bAfter;
  const StableSceneId a = id(EditorObjectKind::node, 40u);
  const StableSceneId b = id(EditorObjectKind::node, 41u);
  aAfter.parent = b;
  bAfter.parent = a;
  const auto cycle = canonicalize(
      header,
      {
          update(a, SceneChangeClass::Hierarchy, SceneField::HierarchyParent,
                 100u, SceneHierarchyValue{}, aAfter),
          update(b, SceneChangeClass::Hierarchy, SceneField::HierarchyParent,
                 100u, SceneHierarchyValue{}, bAfter),
      });
  passed &= expect(!cycle.succeeded() &&
                       cycle.validation.error ==
                           SceneChangeValidationError::DependencyCycle,
                   "hierarchy dependency cycle rejects");

  SceneMutationEvidence incomplete;
  incomplete.sourceMutation = documentHeader().sourceMutations.front();
  incomplete.structuralEvidenceComplete = false;
  const auto incompleteResult = SceneChangeCanonicalizer::canonicalizeEvidence(
      documentHeader(), {incomplete});
  passed &= expect(!incompleteResult.succeeded() &&
                       incompleteResult.validation.error ==
                           SceneChangeValidationError::InvalidFullSnapshot,
                   "incomplete structural evidence requires an explicit "
                   "reason-coded snapshot");

  auto fallbackHeader = snapshotHeader();
  fallbackHeader.baseDocumentRevision = 10u;
  fallbackHeader.baseDocumentDigest = revisionDigest(10u);
  fallbackHeader.targetDocumentRevision = 11u;
  fallbackHeader.targetDocumentDigest = revisionDigest(11u);
  fallbackHeader.sourceMutations = documentHeader().sourceMutations;
  fallbackHeader.firstMutationSerial = 100u;
  fallbackHeader.lastMutationSerial = 100u;
  fallbackHeader.mutationToken = documentHeader().mutationToken;
  SceneMutationEvidence fallbackEvidence;
  fallbackEvidence.sourceMutation = fallbackHeader.sourceMutations.front();
  fallbackEvidence.structuralEvidenceComplete = false;
  fallbackEvidence.exactChanges.push_back(
      create(id(EditorObjectKind::node, 50u), SceneChangeClass::Topology,
             SceneField::TopologyMetadata, 100u,
             topology(EditorObjectKind::node, "partial-structural-evidence")));
  const FullSnapshotReference fallbackSnapshot =
      fullSnapshotFor(fallbackHeader);
  const auto fallback = SceneChangeCanonicalizer::canonicalizeEvidence(
      fallbackHeader, {fallbackEvidence}, fallbackSnapshot);
  passed &= expect(fallback.succeeded() &&
                       fallback.changeSet->canonicalChanges.empty() &&
                       fallback.changeSet->fullSnapshot->reason ==
                           FullRebuildReason::StructuralTopology,
                   "incomplete structural evidence publishes only the explicit "
                   "full snapshot");
  return passed;
}

bool testCanonicalDigestAndRecanonicalization() {
  bool passed = true;
  const auto first = canonicalize(
      documentHeader(),
      {
          update(id(EditorObjectKind::node, 2u), SceneChangeClass::Visibility,
                 SceneField::NodeLocalVisibility, 100u,
                 SceneVisibilityValue{true}, SceneVisibilityValue{false}),
      });
  passed &= expect(first.succeeded(), "canonical digest fixture succeeds");
  if (!first.succeeded()) {
    return false;
  }
  const auto second = SceneChangeCanonicalizer::canonicalize(*first.changeSet);
  passed &= expect(
      second.succeeded() && second.canonicalBytes == first.canonicalBytes &&
          second.changeSet == first.changeSet,
      "recanonicalizing canonical input is byte- and digest-idempotent");
  passed &= expect(SceneChangeCanonicalizer::validateCanonical(
                       *first.changeSet, first.canonicalBytes)
                       .valid(),
                   "canonical byte stream validates against its value");
  auto tampered = *first.changeSet;
  tampered.header.canonicalChangeSetDigest[0] ^= 0xffu;
  const auto invalidDigest = SceneChangeCanonicalizer::canonicalize(tampered);
  passed &= expect(!invalidDigest.succeeded() &&
                       invalidDigest.validation.error ==
                           SceneChangeValidationError::CanonicalDigestMismatch,
                   "tampered canonical digest rejects");
  auto tamperedBytes = first.canonicalBytes;
  tamperedBytes.back() ^= 1u;
  passed &= expect(!SceneChangeCanonicalizer::validateCanonical(
                        *first.changeSet, tamperedBytes)
                        .valid(),
                   "tampered canonical byte stream rejects");
  return passed;
}

static_assert(
    std::is_same_v<std::underlying_type_t<SceneChangeDomain>, std::uint8_t>);
static_assert(
    std::is_same_v<std::underlying_type_t<SceneChangeClass>, std::uint8_t>);
static_assert(
    std::is_same_v<std::underlying_type_t<SceneChangeAction>, std::uint8_t>);
static_assert(static_cast<std::uint8_t>(SceneChangeDomain::FullSnapshot) == 3u);
static_assert(static_cast<std::uint8_t>(SceneChangeClass::FullRebuild) == 8u);
static_assert(static_cast<std::uint8_t>(SceneChangeAction::Delete) == 2u);
static_assert(
    static_cast<std::uint8_t>(FullRebuildReason::ExplicitCallerRequest) == 11u);
static_assert(!std::is_pointer_v<SceneFieldValue>);
static_assert(!std::is_pointer_v<ImmutableSceneSnapshotRef>);

} // namespace

int main() {
  bool passed = true;
  passed &= testSha256AndEmptyInput();
  passed &= testSharedImmutableByteOwnership();
  passed &= testShuffledDeterminismAndWriteChains();
  passed &= testEnvelopeRules();
  passed &= testSchemaIdentityAndFieldValidation();
  passed &= testCheckedOverflow();
  passed &= testActionAlgebraAndGenerationRecreate();
  passed &= testCameraAtomicEvidenceAndClassification();
  passed &= testPayloadRangeAndHashValidation();
  passed &= testImmutableValuesRemapAndSnapshot();
  passed &= testCanonicalizeEvidenceFullSnapshotRemapUnion();
  passed &= testDependencyOrderingAndStructuralFailures();
  passed &= testCanonicalDigestAndRecanonicalization();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
