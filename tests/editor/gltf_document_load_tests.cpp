#include <mgif/scene_editor/GltfDocumentIO.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using demo::editor::EditorFaultInjector;
using demo::editor::EditorFaultPoint;
using demo::editor::EditorObjectId;
using demo::editor::EditorObjectKind;
using demo::editor::GltfDocument;
using demo::editor::GltfDocumentIO;
using demo::editor::GltfLoadError;
using demo::editor::GltfLoadOptions;

constexpr std::array kP1SelectableKinds{
	EditorObjectKind::node,
	EditorObjectKind::primitive,
	EditorObjectKind::mesh,
	EditorObjectKind::material,
	EditorObjectKind::camera,
	EditorObjectKind::light,
};

bool expect(bool condition, const std::string& message)
{
	if(!condition)
	{
		std::cerr << "FAILED: " << message << '\n';
	}
	return condition;
}

std::vector<EditorObjectId> captureSelectableIds(const GltfDocument& document)
{
	std::vector<EditorObjectId> ids;
	ids.reserve(kP1SelectableKinds.size());
	for(const EditorObjectKind kind : kP1SelectableKinds)
	{
		ids.push_back(document.idAt(kind, 0));
	}
	return ids;
}

std::vector<EditorObjectId> captureAllIds(const GltfDocument& document)
{
	std::vector<EditorObjectId> ids;
	for(std::uint8_t rawKind = static_cast<std::uint8_t>(EditorObjectKind::scene);
		rawKind < static_cast<std::uint8_t>(EditorObjectKind::count);
		++rawKind)
	{
		const EditorObjectKind kind = static_cast<EditorObjectKind>(rawKind);
		for(std::size_t index = 0; index < document.objectCount(kind); ++index)
		{
			ids.push_back(document.idAt(kind, index));
		}
	}
	return ids;
}

bool expectResolution(const GltfDocument& document,
	                  const std::vector<EditorObjectId>& ids,
	                  bool expected,
	                  std::string_view scenario)
{
	bool passed = true;
	for(const EditorObjectId id : ids)
	{
		passed &= expect(document.contains(id) == expected,
		                 std::string(scenario) + ": " + demo::editor::editorObjectKindName(id.kind));
	}
	return passed;
}

bool expectMatchingCoordinatesWithDistinctAuthority(
	const std::vector<EditorObjectId>& first,
	const std::vector<EditorObjectId>& second,
	std::string_view scenario)
{
	bool passed = expect(first.size() == second.size(), std::string(scenario) + ": ID counts match");
	const std::size_t count = std::min(first.size(), second.size());
	for(std::size_t index = 0; index < count; ++index)
	{
		passed &= expect(first[index].kind == second[index].kind
			&& first[index].slot == second[index].slot
			&& first[index].generation == second[index].generation
			&& first[index].authority != second[index].authority,
			std::string(scenario) + ": matching local coordinates have distinct authority for "
				+ demo::editor::editorObjectKindName(first[index].kind));
	}
	return passed;
}

std::filesystem::path fixture(std::string_view name)
{
	return std::filesystem::path(MGIF_TEST_SOURCE_DIR) / "tests" / "fixtures" / "gltf_editor" / name;
}

std::string readText(const std::filesystem::path& path)
{
	std::ifstream stream(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

class TempDirectory final
{
public:
	TempDirectory()
	{
		const auto seed = std::chrono::high_resolution_clock::now().time_since_epoch().count();
		for(int attempt = 0; attempt < 100; ++attempt)
		{
			m_path = std::filesystem::temp_directory_path()
				/ ("mgif-gltf-document-load-" + std::to_string(seed) + "-" + std::to_string(attempt));
			std::error_code ec;
			if(std::filesystem::create_directory(m_path, ec) && !ec)
			{
				return;
			}
		}
		throw std::runtime_error("cannot create test directory");
	}

	~TempDirectory()
	{
		std::error_code ec;
		std::filesystem::remove_all(m_path, ec);
	}

	[[nodiscard]] const std::filesystem::path& path() const noexcept
	{
		return m_path;
	}

private:
	std::filesystem::path m_path;
};

void writeText(const std::filesystem::path& path, std::string_view text)
{
	std::ofstream stream(path, std::ios::binary | std::ios::trunc);
	stream.write(text.data(), static_cast<std::streamsize>(text.size()));
	if(!stream)
	{
		throw std::runtime_error("cannot write test input");
	}
}

void appendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
	bytes.push_back(static_cast<std::uint8_t>(value & 0xffu));
	bytes.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
	bytes.push_back(static_cast<std::uint8_t>((value >> 16u) & 0xffu));
	bytes.push_back(static_cast<std::uint8_t>((value >> 24u) & 0xffu));
}

void writeMinimalGlb(const std::filesystem::path& path)
{
	std::string json = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"name":"BinaryRoot"}]})";
	while((json.size() % 4u) != 0u)
	{
		json.push_back(' ');
	}

	std::vector<std::uint8_t> bytes;
	bytes.reserve(20u + json.size());
	appendU32(bytes, 0x46546c67u);
	appendU32(bytes, 2u);
	appendU32(bytes, static_cast<std::uint32_t>(20u + json.size()));
	appendU32(bytes, static_cast<std::uint32_t>(json.size()));
	appendU32(bytes, 0x4e4f534au);
	bytes.insert(bytes.end(), json.begin(), json.end());

	std::ofstream stream(path, std::ios::binary | std::ios::trunc);
	stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	if(!stream)
	{
		throw std::runtime_error("cannot write GLB fixture");
	}
}

void writeDeepHierarchy(const std::filesystem::path& path, std::size_t nodeCount)
{
	std::ofstream stream(path, std::ios::binary | std::ios::trunc);
	stream << R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[)";
	for(std::size_t index = 0; index < nodeCount; ++index)
	{
		if(index != 0)
		{
			stream << ',';
		}
		stream << R"({"name":"DeepNode)" << index << '"';
		if(index + 1 < nodeCount)
		{
			stream << R"(,"children":[)" << (index + 1) << ']';
		}
		stream << '}';
	}
	stream << "]}";
	if(!stream)
	{
		throw std::runtime_error("cannot write deep hierarchy input");
	}
}

class SingleFault final : public EditorFaultInjector
{
public:
	explicit SingleFault(EditorFaultPoint point) : m_point(point) {}

	bool shouldFail(EditorFaultPoint point) noexcept override
	{
		if(!m_fired && point == m_point)
		{
			m_fired = true;
			return true;
		}
		return false;
	}

private:
	EditorFaultPoint m_point;
	bool m_fired{false};
};

bool testAsciiAuthorityAndAtomicPublication()
{
	using namespace demo::editor;
	bool passed = true;

	GltfLoadResult loaded = GltfDocumentIO::loadCandidate(fixture("minimal_scene.gltf"));
	passed &= expect(loaded.succeeded(), "minimal ASCII glTF loads into a candidate");
	const GltfDocument* candidate = loaded.candidate();
	passed &= expect(candidate != nullptr, "successful load owns a candidate before publication");
	if(candidate == nullptr)
	{
		return false;
	}
	passed &= expect(candidate->format() == GltfDocumentFormat::ascii, "ASCII source format is explicit");
	passed &= expect(candidate->sourcePath() == std::filesystem::canonical(fixture("minimal_scene.gltf")),
	                 "source path is the canonical file that produced the authority model");
	passed &= expect(candidate->objectCount(EditorObjectKind::scene) == 1, "scene collection remains authoritative");
	passed &= expect(candidate->objectCount(EditorObjectKind::node) == 1, "node collection remains authoritative");
	passed &= expect(candidate->revision() == 0 && candidate->cleanRevision() == 0 && !candidate->dirty(),
	                 "newly loaded document starts at clean revision zero");
	passed &= expect(candidate->defaultSceneIndex() == std::optional<std::size_t>(0), "default scene index is explicit");
	passed &= expect(candidate->currentSceneIndex() == std::optional<std::size_t>(0), "current scene starts from the default scene");
	passed &= expect(GltfValidator::validate(*candidate).valid(), "published candidate passes the public validator");

	const EditorObjectId nodeId = candidate->idAt(EditorObjectKind::node, 0);
	const EditorObjectId sceneId = candidate->idAt(EditorObjectKind::scene, 0);
	passed &= expect(nodeId.kind == EditorObjectKind::node && nodeId.slot == 0 && nodeId.generation == 1
		&& nodeId.authority == candidate->documentAuthority(),
		"load assigns a deterministic local ID bound to a live document authority");
	passed &= expect(candidate->contains(nodeId), "fresh ID resolves in its source document");
	passed &= expect(!candidate->contains(EditorObjectId{
		EditorObjectKind::node,
		nodeId.slot,
		nodeId.generation + 1,
		nodeId.authority,
	}),
	                 "forged future generation is stale");
	passed &= expect(sceneId != nodeId
		&& candidate->objectName(sceneId) == "Minimal Scene"
		&& candidate->objectName(nodeId) == "Root"
		&& !NodeId::from(sceneId).has_value(),
		"same slot and generation in another kind remains a distinct typed object");

	std::unique_ptr<GltfDocument> active;
	passed &= expect(std::move(loaded).publishTo(active), "validated candidate publishes once");
	passed &= expect(active != nullptr, "publication installs the candidate");
	passed &= expect(!std::move(loaded).publishTo(active), "a consumed candidate cannot publish twice");
	GltfDocument* original = active.get();
	const auto originalPath = active->sourcePath();

	GltfLoadResult invalid = GltfDocumentIO::loadCandidate(fixture("invalid_child_index.gltf"));
	passed &= expect(!invalid.succeeded() && invalid.candidate() == nullptr, "invalid index never produces a candidate");
	passed &= expect(invalid.error() == GltfLoadError::validationFailed, "invalid index is a typed validation failure");
	passed &= expect(!invalid.validationReport().issues().empty()
		&& invalid.validationReport().issues().front().code == GltfValidationErrorCode::indexOutOfRange,
		"invalid child index retains a deterministic validation code");
	passed &= expect(!std::move(invalid).publishTo(active), "failed result cannot publish");
	passed &= expect(active.get() == original && active->sourcePath() == originalPath,
	                 "failed publication preserves the active document atomically");
	return passed;
}

bool testAuthoritySpecificIdentityAcrossDocumentsAndMoves()
{
	using namespace demo::editor;
	bool passed = true;
	const std::filesystem::path featurePath = fixture("feature_preservation.gltf");

	GltfLoadResult firstLoad = GltfDocumentIO::loadCandidate(featurePath);
	passed &= expect(firstLoad.succeeded() && firstLoad.candidate() != nullptr,
	                 "authority fixture A loads as a candidate");
	if(firstLoad.candidate() == nullptr)
	{
		return false;
	}
	for(const EditorObjectKind kind : kP1SelectableKinds)
	{
		passed &= expect(firstLoad.candidate()->objectCount(kind) > 0,
		                 std::string("authority fixture covers selectable kind ") + editorObjectKindName(kind));
	}
	const std::vector<EditorObjectId> firstIds = captureAllIds(*firstLoad.candidate());
	const std::vector<EditorObjectId> firstSelectableIds =
		captureSelectableIds(*firstLoad.candidate());
	for(const EditorObjectId id : firstIds)
	{
		passed &= expect(id.valid() && id.authority == firstLoad.candidate()->documentAuthority(),
		                 std::string("fixture A ID carries its authority for ") + editorObjectKindName(id.kind));
	}

	std::unique_ptr<GltfDocument> active;
	passed &= expect(std::move(firstLoad).publishTo(active), "fixture A publishes as the active document");
	if(active == nullptr)
	{
		return false;
	}
	const GltfDocument* firstAddress = active.get();
	const EditorDocumentAuthority firstAuthority = active->documentAuthority();
	const std::uint64_t firstRevision = active->revision();

	GltfLoadResult failedLoad = GltfDocumentIO::loadCandidate(fixture("invalid_child_index.gltf"));
	passed &= expect(!failedLoad.succeeded() && failedLoad.candidate() == nullptr,
	                 "invalid replacement has no candidate authority to publish");
	passed &= expect(!std::move(failedLoad).publishTo(active), "failed candidate cannot replace fixture A");
	passed &= expect(active.get() == firstAddress
		&& active->documentAuthority() == firstAuthority
		&& active->revision() == firstRevision,
		"failed candidate publication preserves the active document identity atomically");
	passed &= expectResolution(*active, firstIds, true,
	                           "failed candidate leaves every fixture A ID live");

	for(const EditorObjectId id : firstIds)
	{
		EditorObjectId forgedAuthority = id;
		forgedAuthority.authority = EditorDocumentAuthority{id.authority.value == 1u ? 2u : 1u};
		passed &= expect(forgedAuthority.valid() && !active->contains(forgedAuthority),
		                 std::string("forged authority is rejected for ") + editorObjectKindName(id.kind));

		EditorObjectId forgedGeneration = id;
		++forgedGeneration.generation;
		passed &= expect(forgedGeneration.valid() && !active->contains(forgedGeneration),
		                 std::string("forged generation is rejected for ") + editorObjectKindName(id.kind));
	}

	GltfLoadResult sameTopologyLoad = GltfDocumentIO::loadCandidate(featurePath);
	passed &= expect(sameTopologyLoad.succeeded() && sameTopologyLoad.candidate() != nullptr,
	                 "same-topology reload candidate B loads");
	if(sameTopologyLoad.candidate() == nullptr)
	{
		return false;
	}
	const std::vector<EditorObjectId> sameTopologyIds = captureAllIds(*sameTopologyLoad.candidate());
	const std::vector<EditorObjectId> sameTopologySelectableIds =
		captureSelectableIds(*sameTopologyLoad.candidate());
	passed &= expect(firstRevision == sameTopologyLoad.candidate()->revision(),
	                 "same-topology documents deliberately share the same revision");
	passed &= expectMatchingCoordinatesWithDistinctAuthority(
		firstIds, sameTopologyIds, "same-topology reload for every object");
	passed &= expectMatchingCoordinatesWithDistinctAuthority(
		firstSelectableIds, sameTopologySelectableIds, "same-topology reload for selectable objects");
	passed &= expect(std::move(sameTopologyLoad).publishTo(active),
	                 "same-topology reload candidate B replaces fixture A");
	passed &= expectResolution(*active, firstIds, false,
	                           "same-topology replacement rejects every fixture A ID");
	passed &= expectResolution(*active, sameTopologyIds, true,
	                           "same-topology replacement accepts every fixture B ID");

	TempDirectory temporary;
	std::string differentTopologyJson = readText(featurePath);
	constexpr std::string_view jointMarker = R"({ "name": "Joint" })";
	const std::size_t jointPosition = differentTopologyJson.find(jointMarker);
	passed &= expect(jointPosition != std::string::npos,
	                 "different-topology fixture locates its deterministic insertion point");
	if(jointPosition == std::string::npos)
	{
		return false;
	}
	differentTopologyJson.insert(
		jointPosition + jointMarker.size(),
		R"(,
    { "name": "Additional Isolated Node" })");
	const std::filesystem::path differentTopologyPath = temporary.path() / "different-topology.gltf";
	writeText(differentTopologyPath, differentTopologyJson);

	GltfLoadResult differentTopologyLoad = GltfDocumentIO::loadCandidate(differentTopologyPath);
	passed &= expect(differentTopologyLoad.succeeded() && differentTopologyLoad.candidate() != nullptr,
	                 "different-topology replacement candidate C loads");
	if(differentTopologyLoad.candidate() == nullptr)
	{
		return false;
	}
	const std::vector<EditorObjectId> differentTopologyIds = captureAllIds(*differentTopologyLoad.candidate());
	const std::vector<EditorObjectId> differentTopologySelectableIds =
		captureSelectableIds(*differentTopologyLoad.candidate());
	passed &= expect(differentTopologyLoad.candidate()->objectCount(EditorObjectKind::node)
		== active->objectCount(EditorObjectKind::node) + 1u,
		"candidate C has a genuinely different node topology");
	passed &= expect(differentTopologyLoad.candidate()->revision() == active->revision(),
	                 "different-topology documents deliberately share the same revision");
	passed &= expectMatchingCoordinatesWithDistinctAuthority(
		sameTopologySelectableIds, differentTopologySelectableIds, "different-topology replacement");
	passed &= expect(std::move(differentTopologyLoad).publishTo(active),
	                 "different-topology candidate C replaces fixture B");
	passed &= expectResolution(*active, sameTopologyIds, false,
	                           "different-topology replacement rejects every fixture B ID");
	passed &= expectResolution(*active, firstIds, false,
	                           "later replacement keeps every fixture A ID stale");
	passed &= expectResolution(*active, differentTopologyIds, true,
	                           "different-topology replacement accepts every fixture C ID");

	GltfLoadResult moveConstructionLoad = GltfDocumentIO::loadCandidate(featurePath);
	std::unique_ptr<GltfDocument> moveConstructionSource;
	passed &= expect(std::move(moveConstructionLoad).publishTo(moveConstructionSource),
	                 "move-construction source publishes");
	if(moveConstructionSource == nullptr)
	{
		return false;
	}
	const std::vector<EditorObjectId> moveConstructionIds =
		captureAllIds(*moveConstructionSource);
	const EditorDocumentAuthority moveConstructionAuthority =
		moveConstructionSource->documentAuthority();
	GltfDocument moveConstructed(std::move(*moveConstructionSource));
	passed &= expect(moveConstructed.documentAuthority() == moveConstructionAuthority,
	                 "move construction preserves the document authority");
	passed &= expectResolution(moveConstructed, moveConstructionIds, true,
	                           "move construction preserves every object ID");

	GltfLoadResult moveDestinationLoad = GltfDocumentIO::loadCandidate(featurePath);
	GltfLoadResult moveIncomingLoad = GltfDocumentIO::loadCandidate(featurePath);
	std::unique_ptr<GltfDocument> moveDestination;
	std::unique_ptr<GltfDocument> moveIncoming;
	passed &= expect(std::move(moveDestinationLoad).publishTo(moveDestination)
		&& std::move(moveIncomingLoad).publishTo(moveIncoming),
		"move-assignment source and destination publish");
	if(moveDestination == nullptr || moveIncoming == nullptr)
	{
		return false;
	}
	const std::vector<EditorObjectId> replacedDestinationIds = captureAllIds(*moveDestination);
	const std::vector<EditorObjectId> incomingIds = captureAllIds(*moveIncoming);
	const EditorDocumentAuthority incomingAuthority = moveIncoming->documentAuthority();
	passed &= expectMatchingCoordinatesWithDistinctAuthority(
		replacedDestinationIds, incomingIds, "move-assignment replacement");
	*moveDestination = std::move(*moveIncoming);
	passed &= expect(moveDestination->documentAuthority() == incomingAuthority,
	                 "move assignment installs the incoming document authority");
	passed &= expectResolution(*moveDestination, replacedDestinationIds, false,
	                           "move assignment rejects every ID from the replaced destination");
	passed &= expectResolution(*moveDestination, incomingIds, true,
	                           "move assignment preserves every incoming object ID");

	return passed;
}

bool testMultiSceneAndUnknownData()
{
	using namespace demo::editor;
	bool passed = true;

	GltfLoadResult multi = GltfDocumentIO::loadCandidate(fixture("multi_scene_oracle.gltf"));
	passed &= expect(multi.succeeded(), "multi-scene fixture loads");
	GltfDocument* document = multi.candidate();
	if(document == nullptr)
	{
		return false;
	}
	passed &= expect(document->objectCount(EditorObjectKind::scene) == 2, "both scenes remain in the authority model");
	passed &= expect(document->objectCount(EditorObjectKind::node) == 8, "all nodes remain in the authority model");
	const auto scene0 = SceneId::from(document->idAt(EditorObjectKind::scene, 0)).value();
	const auto scene1 = SceneId::from(document->idAt(EditorObjectKind::scene, 1)).value();
	const auto roots0 = document->sceneRootNodeIds(scene0);
	const auto roots1 = document->sceneRootNodeIds(scene1);
	passed &= expect(roots0.size() == 3 && roots1.size() == 3, "each scene exposes its own ordered roots");
	passed &= expect(roots0[2] == roots1[2], "shared root uses one stable node ID across scenes");
	passed &= expect(document->objectName(roots0[2].untyped()) == "SharedRoot", "shared stable ID resolves to the authoritative node");
	const auto defaultBefore = document->defaultSceneIndex();
	const auto revisionBefore = document->revision();
	passed &= expect(document->selectCurrentScene(scene1), "valid current scene selection succeeds");
	passed &= expect(document->currentSceneId() == scene1, "current scene selection is explicit");
	passed &= expect(document->defaultSceneIndex() == defaultBefore
		&& document->revision() == revisionBefore
		&& document->cleanRevision() == 0,
		"current scene selection does not mutate glTF or revision state");
	passed &= expect(!document->selectCurrentScene(SceneId{}), "stale scene selection is rejected");

	GltfLoadResult unknown = GltfDocumentIO::loadCandidate(fixture("unknown_extension_roundtrip.gltf"));
	passed &= expect(unknown.succeeded(), "unknown extension fixture loads without data loss");
	const GltfDocument* opaque = unknown.candidate();
	if(opaque == nullptr)
	{
		return false;
	}
	const EditorObjectId node = opaque->idAt(EditorObjectKind::node, 0);
	const EditorObjectId scene = opaque->idAt(EditorObjectKind::scene, 0);
	passed &= expect(opaque->documentExtensionsJson().find("MGIF_test_unknown") != std::string_view::npos,
	                 "document unknown extension JSON is preserved");
	passed &= expect(opaque->assetExtrasJson().find("asset-extras-must-survive") != std::string_view::npos,
	                 "asset extras JSON is preserved");
	passed &= expect(opaque->objectExtensionsJson(scene).find("MGIF_test_unknown") != std::string_view::npos,
	                 "scene unknown extension JSON is preserved");
	passed &= expect(opaque->objectExtensionsJson(node).find("enabled") != std::string_view::npos,
	                 "node unknown extension JSON is preserved");
	passed &= expect(opaque->objectExtrasJson(node).find("node-extras-must-survive") != std::string_view::npos,
	                 "node extras JSON is preserved");
	passed &= expect(opaque->documentExtensionsJson().find("EditorObjectId") == std::string_view::npos
			&& opaque->objectExtensionsJson(node).find("generation") == std::string_view::npos
			&& opaque->objectExtrasJson(node).find("documentAuthority") == std::string_view::npos,
			"editor identity never leaks into glTF extensions or extras");

	GltfLoadResult feature = GltfDocumentIO::loadCandidate(
		fixture("feature_preservation.gltf"));
	passed &= expect(feature.succeeded() && feature.candidate() != nullptr,
			"feature fixture loads for authored extension declarations");
	if(feature.candidate() != nullptr)
	{
		const std::vector<std::string> names =
			feature.candidate()->documentExtensionNames();
		const auto clearcoat = std::find(names.begin(), names.end(),
			"KHR_materials_clearcoat");
		const auto punctual = std::find(names.begin(), names.end(),
			"KHR_lights_punctual");
		passed &= expect(clearcoat != names.end() && punctual != names.end() &&
				punctual < clearcoat,
				"Asset Info extension names retain authored declaration order and payload entries");
	}
	return passed;
}

bool testDependenciesAndLimits()
{
	using namespace demo::editor;
	bool passed = true;

	GltfLoadResult external = GltfDocumentIO::loadCandidate(fixture("external_buffer.gltf"));
	passed &= expect(external.succeeded(), "contained external buffer loads");
	if(const GltfDocument* document = external.candidate())
	{
		const auto dependencies = document->dependencies();
		passed &= expect(dependencies.size() == 1, "external dependency is recorded exactly once per source object");
		if(dependencies.size() == 1)
		{
			passed &= expect(dependencies[0].kind == GltfDependencyKind::buffer
				&& dependencies[0].objectIndex == 0
				&& dependencies[0].uri == "external_buffer.bin",
				"dependency retains its kind, object index, and original URI");
			passed &= expect(dependencies[0].resolvedPath.filename() == "external_buffer.bin"
				&& dependencies[0].byteSize == 9,
				"dependency retains its contained resolved path and bounded byte count");
		}
	}

	GltfLoadOptions exactLimit;
	exactLimit.limits.maxNodes = 1;
	passed &= expect(GltfDocumentIO::loadCandidate(fixture("minimal_scene.gltf"), exactLimit).succeeded(),
	                 "collection count exactly at the configured limit succeeds");
	GltfLoadOptions overLimit = exactLimit;
	overLimit.limits.maxNodes = 0;
	const auto rejected = GltfDocumentIO::loadCandidate(fixture("minimal_scene.gltf"), overLimit);
	passed &= expect(!rejected.succeeded() && rejected.error() == GltfLoadError::validationFailed,
	                 "collection count one over the configured limit fails");
	GltfLoadOptions noIssueStorage;
	noIssueStorage.limits.maxValidationIssues = 0;
	const auto invalidWithoutStoredIssues = GltfDocumentIO::loadCandidate(
		fixture("invalid_child_index.gltf"), noIssueStorage);
	passed &= expect(!invalidWithoutStoredIssues.succeeded()
		&& invalidWithoutStoredIssues.error() == GltfLoadError::validationFailed
		&& !invalidWithoutStoredIssues.validationReport().valid()
		&& invalidWithoutStoredIssues.validationReport().issues().empty(),
		"validation failure cannot become valid when diagnostic storage is disabled");
	GltfLoadOptions sourceLimit;
	sourceLimit.limits.maxSourceBytes = 1;
	passed &= expect(GltfDocumentIO::loadCandidate(fixture("minimal_scene.gltf"), sourceLimit).error() == GltfLoadError::sourceTooLarge,
	                 "source byte limit is enforced before parsing");
	GltfLoadOptions depthLimit;
	depthLimit.limits.maxNodeDepth = 1;
	passed &= expect(!GltfDocumentIO::loadCandidate(fixture("multi_scene_oracle.gltf"), depthLimit).succeeded(),
	                 "iterative traversal rejects a hierarchy deeper than its budget");
	GltfLoadOptions dependencyLimit;
	dependencyLimit.limits.maxDependencies = 0;
	passed &= expect(!GltfDocumentIO::loadCandidate(fixture("external_buffer.gltf"), dependencyLimit).succeeded(),
	                 "dependency count is bounded before candidate construction");
	GltfLoadOptions externalByteLimit;
	externalByteLimit.limits.maxExternalFileBytes = 8;
	passed &= expect(GltfDocumentIO::loadCandidate(fixture("external_buffer.gltf"), externalByteLimit).error()
		== GltfLoadError::dependencyTooLarge,
		"external dependency is rejected before reading when it exceeds its byte limit");
	GltfLoadOptions aggregateByteLimit;
	aggregateByteLimit.limits.maxAggregateFileBytes = 8;
	passed &= expect(GltfDocumentIO::loadCandidate(fixture("external_buffer.gltf"), aggregateByteLimit).error()
		== GltfLoadError::dependencyTooLarge,
		"external dependency is rejected before reading when it exceeds the aggregate byte limit");
	return passed;
}

bool testRealSponzaValidatorCompatibility()
{
	using namespace demo::editor;
	bool passed = true;
	const auto sponza = std::filesystem::path(MGIF_TEST_SOURCE_DIR)
		/ "resources" / "GLTF_Sponza" / "sponza.gltf";
	GltfLoadResult loaded = GltfDocumentIO::loadCandidate(sponza);
	passed &= expect(loaded.succeeded() && loaded.candidate() != nullptr,
	                 "repository Sponza passes the bounded candidate load path");
	if(!loaded.candidate()) return false;

	const std::size_t materialCount = loaded.candidate()->objectCount(EditorObjectKind::material);
	passed &= expect(materialCount != 0u
		&& loaded.validationReport().valid()
		&& GltfValidator::validate(*loaded.candidate()).valid(),
		"repository Sponza passes both parsed-model and public document validation");

	GltfLoadOptions oneBelowMaterialLimit;
	oneBelowMaterialLimit.limits.maxMaterials = materialCount - 1u;
	const auto rejected = GltfDocumentIO::loadCandidate(sponza, oneBelowMaterialLimit);
	passed &= expect(!rejected.succeeded()
		&& rejected.candidate() == nullptr
		&& rejected.error() == GltfLoadError::validationFailed
		&& !rejected.validationReport().issues().empty()
		&& rejected.validationReport().issues().front().code
			== GltfValidationErrorCode::limitExceeded
		&& rejected.validationReport().issues().front().path == "materials",
		"repository Sponza one over its material budget fails with a typed path");
	return passed;
}

bool testMalformedGraphsContainersAndFaults()
{
	using namespace demo::editor;
	bool passed = true;
	const auto cycle = GltfDocumentIO::loadCandidate(fixture("invalid_cycle.gltf"));
	passed &= expect(!cycle.succeeded() && !cycle.validationReport().issues().empty()
		&& cycle.validationReport().issues().front().code == GltfValidationErrorCode::cycle,
		"cycle is rejected by bounded iterative validation with a typed code");
	const auto multiparent = GltfDocumentIO::loadCandidate(fixture("invalid_multiparent.gltf"));
	passed &= expect(!multiparent.succeeded() && !multiparent.validationReport().issues().empty()
		&& multiparent.validationReport().issues().front().code == GltfValidationErrorCode::duplicateParent,
		"multiple node parents are rejected with a typed code");

	TempDirectory temporary;
	constexpr std::size_t deepNodeCount = 4096;
	const auto deepPath = temporary.path() / "deep.gltf";
	writeDeepHierarchy(deepPath, deepNodeCount);
	GltfLoadOptions exactDepth;
	exactDepth.limits.maxNodes = deepNodeCount;
	exactDepth.limits.maxNodeDepth = deepNodeCount;
	auto exactDepthLoad = GltfDocumentIO::loadCandidate(deepPath, exactDepth);
	passed &= expect(exactDepthLoad.succeeded() && exactDepthLoad.candidate() != nullptr,
	                 "iterative traversal accepts a 4096-node chain exactly at its depth budget");
	std::unique_ptr<GltfDocument> activeDeepDocument;
	passed &= expect(std::move(exactDepthLoad).publishTo(activeDeepDocument),
	                 "the exact-depth candidate publishes as the active document");
	GltfDocument* const activeBeforeRejection = activeDeepDocument.get();
	const EditorObjectId rootBeforeRejection = activeDeepDocument
		? activeDeepDocument->idAt(EditorObjectKind::node, 0)
		: EditorObjectId{};
	const std::string rootNameBeforeRejection = activeDeepDocument
		? std::string(activeDeepDocument->objectName(rootBeforeRejection))
		: std::string{};

	const auto overDepthPath = temporary.path() / "deep-one-over.gltf";
	writeDeepHierarchy(overDepthPath, deepNodeCount + 1u);
	GltfLoadOptions overDepth = exactDepth;
	overDepth.limits.maxNodes = deepNodeCount + 1u;
	auto tooDeep = GltfDocumentIO::loadCandidate(overDepthPath, overDepth);
	passed &= expect(!tooDeep.succeeded() && tooDeep.candidate() == nullptr
		&& tooDeep.error() == GltfLoadError::validationFailed
		&& !tooDeep.validationReport().issues().empty()
		&& tooDeep.validationReport().issues().front().code == GltfValidationErrorCode::depthExceeded,
		"iterative traversal rejects a 4097-node chain one over the configured 4096 depth limit");
	passed &= expect(!std::move(tooDeep).publishTo(activeDeepDocument)
		&& activeDeepDocument.get() == activeBeforeRejection
		&& activeDeepDocument != nullptr
		&& activeDeepDocument->contains(rootBeforeRejection)
		&& std::string(activeDeepDocument->objectName(rootBeforeRejection)) == rootNameBeforeRejection,
		"one-over depth rejection cannot replace or mutate the active boundary document");

	const auto malformedPath = temporary.path() / "malformed.gltf";
	writeText(malformedPath, "{not-json");
	passed &= expect(GltfDocumentIO::loadCandidate(malformedPath).error() == GltfLoadError::parseFailed,
	                 "malformed JSON is a typed parse failure");

	const auto dataUriPath = temporary.path() / "embedded.gltf";
	writeText(dataUriPath,
	          R"({"asset":{"version":"2.0"},"buffers":[{"uri":"data:application/octet-stream;base64,AAAAAA==","byteLength":4}]})");
	const auto embedded = GltfDocumentIO::loadCandidate(dataUriPath);
	passed &= expect(embedded.succeeded() && embedded.candidate()->dependencies().empty(),
	                 "data URI payload is embedded and not recorded as an external dependency");

	const auto traversalPath = temporary.path() / "traversal.gltf";
	writeText(traversalPath,
	          R"({"asset":{"version":"2.0"},"buffers":[{"uri":"../escape.bin","byteLength":4}]})");
	const auto traversal = GltfDocumentIO::loadCandidate(traversalPath);
	passed &= expect(!traversal.succeeded() && traversal.error() == GltfLoadError::unsafeDependency,
	                 "parent traversal URI fails closed");
	const auto encodedTraversalPath = temporary.path() / "encoded-traversal.gltf";
	writeText(encodedTraversalPath,
	          R"({"asset":{"version":"2.0"},"buffers":[{"uri":"%2e%2e/escape.bin","byteLength":4}]})");
	passed &= expect(GltfDocumentIO::loadCandidate(encodedTraversalPath).error() == GltfLoadError::unsafeDependency,
	                 "percent-encoded parent traversal fails closed before filesystem access");

	const auto glbPath = temporary.path() / "minimal.glb";
	writeMinimalGlb(glbPath);
	const auto glb = GltfDocumentIO::loadCandidate(glbPath);
	passed &= expect(glb.succeeded() && glb.candidate()->format() == GltfDocumentFormat::binary,
	                 "minimal GLB loads through the binary candidate path");
	if(glb.candidate())
	{
		passed &= expect(glb.candidate()->objectName(glb.candidate()->idAt(EditorObjectKind::node, 0)) == "BinaryRoot",
		                 "GLB authority preserves node data");
	}
	const auto disguisedPath = temporary.path() / "disguised.gltf";
	std::filesystem::copy_file(glbPath, disguisedPath);
	passed &= expect(GltfDocumentIO::loadCandidate(disguisedPath).error() == GltfLoadError::invalidContainer,
	                 "suffix and magic mismatch is rejected before parsing");

	for(const EditorFaultPoint point : {
		EditorFaultPoint::parse,
		EditorFaultPoint::allocation,
		EditorFaultPoint::postParseValidation,
	})
	{
		SingleFault fault(point);
		GltfLoadOptions options;
		options.faultInjector = &fault;
		const auto result = GltfDocumentIO::loadCandidate(fixture("minimal_scene.gltf"), options);
		passed &= expect(!result.succeeded() && result.candidate() == nullptr
			&& result.error() == GltfLoadError::injectedFailure,
			"injected parse/allocation/validation failure never creates a candidate");
	}
	return passed;
}

bool testImplementationOwnership()
{
	bool passed = true;
	const std::string loader = readText(std::filesystem::path(MGIF_TEST_SOURCE_DIR) / "loader" / "GltfLoader.cpp");
	const std::string implementation = readText(
		std::filesystem::path(MGIF_TEST_SOURCE_DIR) / "loader" / "TinyGltfImplementation.cpp");
	passed &= expect(loader.find("TINYGLTF_IMPLEMENTATION") == std::string::npos
		&& loader.find("STB_IMAGE_IMPLEMENTATION") == std::string::npos
		&& loader.find("STB_IMAGE_WRITE_IMPLEMENTATION") == std::string::npos,
		"viewer loader contains declarations only");
	const auto exactlyOnce = [&implementation](std::string_view token)
	{
		const auto first = implementation.find(token);
		return first != std::string::npos && implementation.find(token, first + token.size()) == std::string::npos;
	};
	passed &= expect(exactlyOnce("TINYGLTF_IMPLEMENTATION")
		&& exactlyOnce("STB_IMAGE_IMPLEMENTATION")
		&& exactlyOnce("STB_IMAGE_WRITE_IMPLEMENTATION"),
		"main graph has one explicit TinyGLTF/STB implementation owner TU");
	return passed;
}

} // namespace

int main()
{
	bool passed = true;
	try
	{
		passed &= testAsciiAuthorityAndAtomicPublication();
		passed &= testAuthoritySpecificIdentityAcrossDocumentsAndMoves();
		passed &= testMultiSceneAndUnknownData();
		passed &= testDependenciesAndLimits();
		passed &= testRealSponzaValidatorCompatibility();
		passed &= testMalformedGraphsContainersAndFaults();
		passed &= testImplementationOwnership();
	}
	catch(const std::exception& exception)
	{
		std::cerr << "FAILED: unexpected exception: " << exception.what() << '\n';
		passed = false;
	}
	return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
