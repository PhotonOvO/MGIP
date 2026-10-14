#include <mgif/scene_editor/EditorFaultInjection.h>
#include <mgif/scene_editor/GltfDocumentIO.h>
#include <mgif/scene_editor/GltfSemanticCompare.h>
#include <mgif/scene_editor/GltfValidator.h>
#include <mgif/scene_editor/SceneCompactor.h>

#include <tiny_gltf.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace demo::editor::detail {

class GltfDocumentAccess final
{
public:
	[[nodiscard]] static tinygltf::Model& model(GltfDocument& document) noexcept
	{
		return document.mutableAuthorityModel();
	}

	[[nodiscard]] static const tinygltf::Model& model(const GltfDocument& document) noexcept
	{
		return document.authorityModel();
	}

	[[nodiscard]] static std::unique_ptr<GltfDocument>
	cloneWithDependencies(const GltfDocument& document, std::vector<GltfDependency> dependencies)
	{
		return std::unique_ptr<GltfDocument>(
			new GltfDocument(std::make_unique<tinygltf::Model>(document.authorityModel()),
							 document.sourcePath(), document.format(), std::move(dependencies)));
	}
};

} // namespace demo::editor::detail

namespace {

using namespace demo::editor;

bool expect(bool condition, std::string_view message)
{
	if (!condition)
		std::cerr << "FAILED: " << message << '\n';
	return condition;
}

class TempDirectory final
{
public:
	TempDirectory()
	{
		static std::atomic<std::uint64_t> sequence{0};
		const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
		m_path = std::filesystem::temp_directory_path() / ("mgif-scene-compactor-" + std::to_string(ticks) +
														   '-' + std::to_string(sequence.fetch_add(1)));
		std::filesystem::create_directories(m_path);
	}

	~TempDirectory()
	{
		std::error_code ignored;
		std::filesystem::remove_all(m_path, ignored);
	}

	[[nodiscard]] const std::filesystem::path& path() const noexcept
	{
		return m_path;
	}

private:
	std::filesystem::path m_path;
};

[[nodiscard]] std::filesystem::path fixture(std::string_view name)
{
	return std::filesystem::path(MGIF_TEST_SOURCE_DIR) / "tests" / "fixtures" / "gltf_editor" / name;
}

[[nodiscard]] std::unique_ptr<GltfDocument> load(const std::filesystem::path& path)
{
	auto result = GltfDocumentIO::loadCandidate(path);
	if (!result.succeeded())
	{
		std::cerr << "load failed: " << result.message() << '\n';
		for (const GltfValidationIssue& issue : result.validationReport().issues())
			std::cerr << "  " << issue.path << ": " << issue.message << '\n';
		return {};
	}
	std::unique_ptr<GltfDocument> document;
	if (!std::move(result).publishTo(document))
		return {};
	return document;
}

void shift(int& value, int amount = 1) noexcept
{
	if (value >= 0)
		value += amount;
}

void shiftValue(tinygltf::Value& value, int amount = 1)
{
	if (value.IsInt() && value.GetNumberAsInt() >= 0)
		value = tinygltf::Value(value.GetNumberAsInt() + amount);
}

tinygltf::Value object(std::initializer_list<std::pair<const std::string, tinygltf::Value>> values)
{
	tinygltf::Value::Object result;
	for (const auto& [name, value] : values)
		result.emplace(name, value);
	return tinygltf::Value(std::move(result));
}

tinygltf::Value array(std::initializer_list<tinygltf::Value> values)
{
	return tinygltf::Value(tinygltf::Value::Array(values));
}

void declareExtension(tinygltf::Model& model, std::string name)
{
	if (std::find(model.extensionsUsed.begin(), model.extensionsUsed.end(), name) ==
		model.extensionsUsed.end())
		model.extensionsUsed.push_back(std::move(name));
}

void insertOrphanBuffer(tinygltf::Model& model)
{
	model.buffers.insert(model.buffers.begin(), model.buffers.front());
	model.buffers.front().name = "orphan-buffer";
	for (tinygltf::BufferView& view : model.bufferViews)
	{
		shift(view.buffer);
		const auto meshopt = view.extensions.find("EXT_meshopt_compression");
		if (meshopt != view.extensions.end() && meshopt->second.IsObject() && meshopt->second.Has("buffer"))
			shiftValue(meshopt->second.Get<tinygltf::Value::Object>()["buffer"]);
	}
}

void insertOrphanBufferView(tinygltf::Model& model)
{
	model.bufferViews.insert(model.bufferViews.begin(), model.bufferViews.front());
	model.bufferViews.front().name = "orphan-buffer-view";
	for (tinygltf::Accessor& accessor : model.accessors)
	{
		shift(accessor.bufferView);
		if (accessor.sparse.isSparse)
		{
			shift(accessor.sparse.indices.bufferView);
			shift(accessor.sparse.values.bufferView);
		}
	}
	for (tinygltf::Image& image : model.images)
		shift(image.bufferView);
	for (tinygltf::AudioSource& source : model.audioSources)
		shift(source.bufferView);
}

void insertOrphanAccessor(tinygltf::Model& model)
{
	model.accessors.insert(model.accessors.begin(), model.accessors.front());
	model.accessors.front().name = "orphan-accessor";
	for (tinygltf::Mesh& mesh : model.meshes)
		for (tinygltf::Primitive& primitive : mesh.primitives)
		{
			shift(primitive.indices);
			for (auto& [semantic, accessor] : primitive.attributes)
			{
				(void)semantic;
				shift(accessor);
			}
			for (auto& target : primitive.targets)
				for (auto& [semantic, accessor] : target)
				{
					(void)semantic;
					shift(accessor);
				}
		}
	for (tinygltf::Skin& skin : model.skins)
		shift(skin.inverseBindMatrices);
	for (tinygltf::Animation& animation : model.animations)
		for (tinygltf::AnimationSampler& sampler : animation.samplers)
		{
			shift(sampler.input);
			shift(sampler.output);
		}
}

void insertOrphanImage(tinygltf::Model& model)
{
	model.images.insert(model.images.begin(), model.images.front());
	model.images.front().name = "orphan-image";
	for (tinygltf::Texture& texture : model.textures)
	{
		shift(texture.source);
		for (const std::string_view name :
			 {std::string_view{"KHR_texture_basisu"}, std::string_view{"EXT_texture_webp"},
			  std::string_view{"EXT_texture_avif"}, std::string_view{"MSFT_texture_dds"}})
		{
			const auto extension = texture.extensions.find(std::string(name));
			if (extension != texture.extensions.end() && extension->second.IsObject() &&
				extension->second.Has("source"))
				shiftValue(extension->second.Get<tinygltf::Value::Object>()["source"]);
		}
	}
}

void shiftMaterialTextures(tinygltf::Material& material)
{
	shift(material.pbrMetallicRoughness.baseColorTexture.index);
	shift(material.pbrMetallicRoughness.metallicRoughnessTexture.index);
	shift(material.normalTexture.index);
	shift(material.occlusionTexture.index);
	shift(material.emissiveTexture.index);
	for (auto& [name, parameter] : material.values)
	{
		(void)name;
		const auto index = parameter.json_double_value.find("index");
		if (index != parameter.json_double_value.end())
			index->second += 1.0;
	}
	for (auto& [name, parameter] : material.additionalValues)
	{
		(void)name;
		const auto index = parameter.json_double_value.find("index");
		if (index != parameter.json_double_value.end())
			index->second += 1.0;
	}
	for (auto& [name, extension] : material.extensions)
	{
		(void)name;
		if (!extension.IsObject())
			continue;
		for (auto& [member, value] : extension.Get<tinygltf::Value::Object>())
			if (value.IsObject() && value.Has("index"))
				shiftValue(value.Get<tinygltf::Value::Object>()["index"]);
	}
}

void insertTextureMaterialMeshOrphans(tinygltf::Model& model)
{
	for (tinygltf::Material& material : model.materials)
		shiftMaterialTextures(material);
	model.textures.insert(model.textures.begin(), model.textures.front());
	model.textures.front().name = "orphan-texture";

	// Index 0 is orphaned, index 1 is retained through the retained texture.
	model.samplers.resize(2);
	model.samplers[0].name = "orphan-sampler";
	model.samplers[1].name = "retained-sampler";
	model.textures[1].sampler = 1;

	model.materials.insert(model.materials.begin(), model.materials.front());
	model.materials.front().name = "orphan-material";
	model.materials.insert(model.materials.begin() + 1, model.materials.back());
	model.materials[1].name = "lod-material";
	model.materials[2].lods = {1};
	model.materials[2].extensions["MSFT_lod"] = object({
		{"ids", array({tinygltf::Value(1)})},
		{"opaqueSentinel", tinygltf::Value(73)},
	});
	declareExtension(model, "MSFT_lod");
	for (tinygltf::Mesh& mesh : model.meshes)
		for (tinygltf::Primitive& primitive : mesh.primitives)
			shift(primitive.material, 2);

	model.meshes.insert(model.meshes.begin(), model.meshes.front());
	model.meshes.front().name = "orphan-mesh";
	for (tinygltf::Node& node : model.nodes)
		shift(node.mesh);
}

void insertCameraLightNodeOrphans(tinygltf::Model& model)
{
	model.cameras.insert(model.cameras.begin(), model.cameras.front());
	model.cameras.front().name = "orphan-camera";
	for (tinygltf::Node& node : model.nodes)
		shift(node.camera);

	model.lights.insert(model.lights.begin(), model.lights.front());
	model.lights.front().name = "orphan-light";
	for (tinygltf::Node& node : model.nodes)
	{
		shift(node.light);
		const auto light = node.extensions.find("KHR_lights_punctual");
		if (light != node.extensions.end() && light->second.IsObject() && light->second.Has("light"))
			shiftValue(light->second.Get<tinygltf::Value::Object>()["light"]);
	}
	model.extensions["KHR_lights_punctual"] = object({
		{"lights",
		 array({
			 object({{"name", tinygltf::Value("orphan-light")}, {"type", tinygltf::Value("directional")}}),
			 object({{"name", tinygltf::Value("Default Color Light")},
					 {"type", tinygltf::Value("directional")},
					 {"opaqueSentinel", tinygltf::Value(91)}}),
		 })},
	});

	for (tinygltf::Scene& scene : model.scenes)
		for (int& node : scene.nodes)
			shift(node, 2);
	for (tinygltf::Node& node : model.nodes)
	{
		for (int& child : node.children)
			shift(child, 2);
		for (int& lod : node.lods)
			shift(lod, 2);
	}
	for (tinygltf::Skin& skin : model.skins)
	{
		shift(skin.skeleton, 2);
		for (int& joint : skin.joints)
			shift(joint, 2);
	}
	for (tinygltf::Animation& animation : model.animations)
		for (tinygltf::AnimationChannel& channel : animation.channels)
			shift(channel.target_node, 2);
	tinygltf::Node orphan;
	orphan.name = "orphan-node";
	tinygltf::Node lod;
	lod.name = "lod-node";
	model.nodes.insert(model.nodes.begin(), orphan);
	model.nodes.insert(model.nodes.begin() + 1, lod);
	model.nodes[2].lods = {1};
	model.nodes[2].extensions["MSFT_lod"] = object({
		{"ids", array({tinygltf::Value(1)})},
		{"opaqueSentinel", tinygltf::Value(37)},
	});
}

void addKnownExtensionReferences(tinygltf::Model& model)
{
	// Retained resources are at index 1 after one orphan was inserted.
	model.textures[1].extensions["KHR_texture_basisu"] =
		object({{"source", tinygltf::Value(1)}, {"opaqueSentinel", tinygltf::Value(11)}});
	declareExtension(model, "KHR_texture_basisu");

	model.nodes[2].extensions["EXT_mesh_gpu_instancing"] = object({
		{"attributes", object({{"TRANSLATION", tinygltf::Value(1)}})},
		{"opaqueSentinel", tinygltf::Value(13)},
	});
	declareExtension(model, "EXT_mesh_gpu_instancing");

	tinygltf::Primitive& primitive = model.meshes[1].primitives[0];
	primitive.extensions["KHR_draco_mesh_compression"] = object({
		{"bufferView", tinygltf::Value(1)},
		{"attributes", object({{"POSITION", tinygltf::Value(0)}})},
		{"opaqueSentinel", tinygltf::Value(17)},
	});
	declareExtension(model, "KHR_draco_mesh_compression");

	model.extensions["KHR_materials_variants"] = object({
		{"variants", array({object({{"name", tinygltf::Value("variant-zero")}})})},
		{"opaqueSentinel", tinygltf::Value(19)},
	});
	primitive.extensions["KHR_materials_variants"] = object({
		{"mappings", array({object({
						 {"material", tinygltf::Value(2)},
						 {"variants", array({tinygltf::Value(0)})},
						 {"opaqueSentinel", tinygltf::Value(23)},
					 })})},
	});
	declareExtension(model, "KHR_materials_variants");

	model.bufferViews[1].extensions["EXT_meshopt_compression"] = object({
		{"buffer", tinygltf::Value(1)},
		{"byteOffset", tinygltf::Value(0)},
		{"byteLength", tinygltf::Value(36)},
		{"byteStride", tinygltf::Value(12)},
		{"count", tinygltf::Value(3)},
		{"mode", tinygltf::Value("ATTRIBUTES")},
		{"filter", tinygltf::Value("NONE")},
		{"opaqueSentinel", tinygltf::Value(29)},
	});
	declareExtension(model, "EXT_meshopt_compression");
}

void addAudioReferences(tinygltf::Model& model)
{
	model.audioSources.resize(2);
	model.audioSources[0].name = "orphan-audio-source";
	model.audioSources[1].name = "retained-audio-source";
	for (tinygltf::AudioSource& source : model.audioSources)
	{
		source.bufferView = 1;
		source.mimeType = "audio/mpeg";
	}
	model.audioEmitters.resize(2);
	model.audioEmitters[0].name = "orphan-audio-emitter";
	model.audioEmitters[0].source = 0;
	model.audioEmitters[1].name = "retained-audio-emitter";
	model.audioEmitters[1].source = 1;
	model.nodes[2].emitter = 1;
	model.nodes[2].extensions["KHR_audio"] =
		object({{"emitter", tinygltf::Value(1)}, {"opaqueSentinel", tinygltf::Value(31)}});
	model.scenes[0].audioEmitters = {1};
	model.scenes[0].extensions["KHR_audio"] =
		object({{"emitters", array({tinygltf::Value(1)})}, {"opaqueSentinel", tinygltf::Value(41)}});
	model.extensions["KHR_audio"] = object({
		{"emitters",
		 array({
			 object({{"name", tinygltf::Value("orphan-audio-emitter")}, {"source", tinygltf::Value(0)}}),
			 object({{"name", tinygltf::Value("retained-audio-emitter")},
					 {"source", tinygltf::Value(1)},
					 {"opaqueSentinel", tinygltf::Value(43)}}),
		 })},
		{"sources", array({
						object({{"name", tinygltf::Value("orphan-audio-source")},
								{"bufferView", tinygltf::Value(1)},
								{"mimeType", tinygltf::Value("audio/mpeg")}}),
						object({{"name", tinygltf::Value("retained-audio-source")},
								{"bufferView", tinygltf::Value(1)},
								{"mimeType", tinygltf::Value("audio/mpeg")},
								{"opaqueSentinel", tinygltf::Value(47)}}),
					})},
	});
	declareExtension(model, "KHR_audio");
}

[[nodiscard]] std::unique_ptr<GltfDocument> makeFixture(const std::filesystem::path& directory,
														bool unsafeExtension = false)
{
	auto staging = load(fixture("feature_preservation.gltf"));
	if (!staging)
		return {};
	tinygltf::Model& model = demo::editor::detail::GltfDocumentAccess::model(*staging);
	insertOrphanBuffer(model);
	insertOrphanBufferView(model);
	insertOrphanAccessor(model);
	insertOrphanImage(model);
	insertTextureMaterialMeshOrphans(model);
	insertCameraLightNodeOrphans(model);
	addKnownExtensionReferences(model);
	addAudioReferences(model);
	model.extras = object({{"opaqueRootSentinel", tinygltf::Value(101)}});
	if (unsafeExtension)
	{
		model.extensions["MGIF_unsafe_indexed"] =
			object({{"node", tinygltf::Value(2)}, {"opaqueSentinel", tinygltf::Value(103)}});
		declareExtension(model, "MGIF_unsafe_indexed");
	}
	const GltfValidationReport validation = GltfValidator::validate(*staging);
	if (!validation.valid())
	{
		std::cerr << "staging fixture invalid\n";
		for (const GltfValidationIssue& issue : validation.issues())
			std::cerr << "  " << issue.path << ": " << issue.message << '\n';
		return {};
	}
	const auto path = directory / (unsafeExtension ? "unsafe.glb" : "compact.glb");
	tinygltf::TinyGLTF writer;
	if (!writer.WriteGltfSceneToFile(&model, path.string(), true, true, true, true))
	{
		std::cerr << "fixture write failed\n";
		return {};
	}
	auto loaded = load(path);
	if (!loaded)
		return {};
	// TinyGLTF owns typed light/audio arrays separately from their root Value.
	// Inject opaque schema siblings after reload so compaction preservation is
	// tested without asking the fixture writer to round-trip unknown members.
	tinygltf::Model& loadedModel = demo::editor::detail::GltfDocumentAccess::model(*loaded);
	auto& lights = loadedModel.extensions["KHR_lights_punctual"]
					   .Get<tinygltf::Value::Object>()["lights"]
					   .Get<tinygltf::Value::Array>();
	lights[1].Get<tinygltf::Value::Object>()["opaqueSentinel"] = tinygltf::Value(91);
	auto& audio = loadedModel.extensions["KHR_audio"].Get<tinygltf::Value::Object>();
	audio["emitters"].Get<tinygltf::Value::Array>()[1].Get<tinygltf::Value::Object>()["opaqueSentinel"] =
		tinygltf::Value(43);
	audio["sources"].Get<tinygltf::Value::Array>()[1].Get<tinygltf::Value::Object>()["opaqueSentinel"] =
		tinygltf::Value(47);
	return loaded;
}

struct Snapshot final
{
	std::uint64_t revision{0};
	std::string digest;
	std::array<std::vector<EditorObjectId>, static_cast<std::size_t>(EditorObjectKind::count)> ids;

	bool operator==(const Snapshot&) const = default;
};

[[nodiscard]] Snapshot snapshot(const GltfDocument& document)
{
	Snapshot result;
	result.revision = document.revision();
	result.digest = GltfSemanticCompare::digest(document);
	for (std::size_t raw = 1; raw < static_cast<std::size_t>(EditorObjectKind::count); ++raw)
	{
		const auto kind = static_cast<EditorObjectKind>(raw);
		for (std::size_t index = 0; index < document.objectCount(kind); ++index)
			result.ids[raw].push_back(document.idAt(kind, index));
	}
	return result;
}

class ScriptedFaultInjector final : public EditorFaultInjector
{
public:
	void failOn(EditorFaultPoint point, std::size_t call) noexcept
	{
		m_point = point;
		m_call = call;
		m_count = 0;
	}

	[[nodiscard]] bool shouldFail(EditorFaultPoint point) noexcept override
	{
		if (point != m_point)
			return false;
		return ++m_count == m_call;
	}

private:
	EditorFaultPoint m_point{EditorFaultPoint::count};
	std::size_t m_call{std::numeric_limits<std::size_t>::max()};
	std::size_t m_count{0};
};

bool expectMap(const SceneCompactionResult& result, SceneCompactionCollectionKind kind, std::size_t oldIndex,
			   std::optional<std::size_t> expected, std::string_view message)
{
	return expect(result.collection(kind).newIndex(oldIndex) == expected, message);
}

bool testCompactionAndIdempotency()
{
	TempDirectory temporary;
	auto document = makeFixture(temporary.path());
	if (!expect(document != nullptr, "full compaction fixture loads"))
		return false;
	bool passed = true;
	const std::uint64_t revision = document->revision();
	const EditorObjectId removedNode = document->idAt(EditorObjectKind::node, 0);
	const EditorObjectId retainedLodNode = document->idAt(EditorObjectKind::node, 1);
	const EditorObjectId retainedRootNode = document->idAt(EditorObjectKind::node, 2);
	const EditorObjectId removedPrimitive = document->idAt(EditorObjectKind::primitive, 0);
	const EditorObjectId retainedPrimitive = document->idAt(EditorObjectKind::primitive, 1);
	const EditorObjectId retainedMesh = document->idAt(EditorObjectKind::mesh, 1);
	const EditorObjectId retainedMaterial = document->idAt(EditorObjectKind::material, 2);
	const EditorObjectId retainedTexture = document->idAt(EditorObjectKind::texture, 1);
	const EditorObjectId retainedImage = document->idAt(EditorObjectKind::image, 1);
	const EditorObjectId retainedSampler = document->idAt(EditorObjectKind::sampler, 1);
	const EditorObjectId retainedCamera = document->idAt(EditorObjectKind::camera, 1);
	const EditorObjectId retainedLight = document->idAt(EditorObjectKind::light, 1);
	const EditorObjectId retainedAccessor = document->idAt(EditorObjectKind::accessor, 1);
	const EditorObjectId retainedView = document->idAt(EditorObjectKind::bufferView, 1);
	const EditorObjectId retainedBuffer = document->idAt(EditorObjectKind::buffer, 1);

	const SceneCompactionResult compacted = SceneCompactor::compact(*document);
	passed &= expect(compacted.applied(), "compaction publishes the valid candidate");
	passed &= expect(compacted.beforeRevision == revision && compacted.afterRevision == revision + 1 &&
						 document->revision() == revision + 1,
					 "successful compaction advances the revision exactly once");
	passed &=
		expectMap(compacted, SceneCompactionCollectionKind::node, 0, std::nullopt, "orphan node is removed");
	passed &=
		expectMap(compacted, SceneCompactionCollectionKind::node, 1, 0, "node LOD is retained and shifted");
	passed &= expectMap(compacted, SceneCompactionCollectionKind::primitive, 0, std::nullopt,
						"primitive owned by an orphan mesh is removed");
	for (const auto kind :
		 {SceneCompactionCollectionKind::mesh, SceneCompactionCollectionKind::texture,
		  SceneCompactionCollectionKind::image, SceneCompactionCollectionKind::sampler,
		  SceneCompactionCollectionKind::camera, SceneCompactionCollectionKind::light,
		  SceneCompactionCollectionKind::accessor, SceneCompactionCollectionKind::bufferView,
		  SceneCompactionCollectionKind::buffer, SceneCompactionCollectionKind::audioEmitter,
		  SceneCompactionCollectionKind::audioSource})
	{
		passed &= expectMap(compacted, kind, 0, std::nullopt,
							"the first orphan in each indexed resource collection is removed");
		passed &=
			expectMap(compacted, kind, 1, 0, "the retained resource is deterministically shifted to zero");
	}
	passed &= expectMap(compacted, SceneCompactionCollectionKind::material, 0, std::nullopt,
						"orphan material is removed");
	passed &=
		expectMap(compacted, SceneCompactionCollectionKind::material, 1, 0, "material LOD remains reachable");
	passed &= expectMap(compacted, SceneCompactionCollectionKind::material, 2, 1,
						"primitive material remains reachable after its LOD");
	passed &= expect(compacted.collection(SceneCompactionCollectionKind::scene).beforeCount ==
							 compacted.collection(SceneCompactionCollectionKind::scene).afterCount &&
						 compacted.collection(SceneCompactionCollectionKind::skin).beforeCount ==
							 compacted.collection(SceneCompactionCollectionKind::skin).afterCount &&
						 compacted.collection(SceneCompactionCollectionKind::animation).beforeCount ==
							 compacted.collection(SceneCompactionCollectionKind::animation).afterCount,
					 "all scenes, skins, and animations remain semantic reachability roots");

	passed &= expect(!document->contains(removedNode) && !document->indexOf(removedNode) &&
						 !document->contains(removedPrimitive),
					 "removed stable IDs become stale tombstones");
	passed &= expect(document->idAt(EditorObjectKind::node, 0) == retainedLodNode &&
						 document->idAt(EditorObjectKind::node, 1) == retainedRootNode &&
						 document->idAt(EditorObjectKind::primitive, 0) == retainedPrimitive &&
						 document->idAt(EditorObjectKind::mesh, 0) == retainedMesh &&
						 document->idAt(EditorObjectKind::material, 1) == retainedMaterial &&
						 document->idAt(EditorObjectKind::texture, 0) == retainedTexture &&
						 document->idAt(EditorObjectKind::image, 0) == retainedImage &&
						 document->idAt(EditorObjectKind::sampler, 0) == retainedSampler &&
						 document->idAt(EditorObjectKind::camera, 0) == retainedCamera &&
						 document->idAt(EditorObjectKind::light, 0) == retainedLight &&
						 document->idAt(EditorObjectKind::accessor, 0) == retainedAccessor &&
						 document->idAt(EditorObjectKind::bufferView, 0) == retainedView &&
						 document->idAt(EditorObjectKind::buffer, 0) == retainedBuffer,
					 "all retained stable IDs and generations survive index remapping");

	const tinygltf::Model& model = demo::editor::detail::GltfDocumentAccess::model(*document);
	passed &= expect(GltfValidator::validate(*document).valid(), "published compacted authority validates");
	passed &= expect(model.scenes[0].nodes == std::vector<int>({1, 2}) &&
						 model.nodes[1].lods == std::vector<int>({0}) && model.nodes[1].mesh == 0 &&
						 model.nodes[1].skin == 0 && model.nodes[2].camera == 0 && model.nodes[2].light == 0,
					 "scene, node LOD, mesh, skin, camera, and light references are remapped");
	passed &= expect(model.skins[0].skeleton == 3 && model.skins[0].joints == std::vector<int>({3}) &&
						 model.animations[0].channels[0].target_node == 1,
					 "skin and animation node references are remapped");
	const tinygltf::Primitive& primitive = model.meshes[0].primitives[0];
	passed &= expect(primitive.material == 1 && primitive.indices == 1 &&
						 primitive.attributes.at("POSITION") == 0 && primitive.targets[0].at("POSITION") == 0,
					 "primitive material and accessor references are remapped");
	passed &= expect(model.materials[1].lods == std::vector<int>({0}) &&
						 model.materials[1].pbrMetallicRoughness.baseColorTexture.index == 0 &&
						 model.textures[0].source == 0 && model.textures[0].sampler == 0 &&
						 model.images[0].bufferView == 7,
					 "material, texture, image, and sampler chains are remapped");
	passed &=
		expect(model.accessors[0].bufferView == 0 && model.accessors[0].sparse.indices.bufferView == 5 &&
				   model.accessors[0].sparse.values.bufferView == 6 && model.bufferViews[0].buffer == 0,
			   "accessor, sparse, buffer-view, and buffer references are remapped");
	passed &=
		expect(model.audioEmitters.size() == 1 && model.audioSources.size() == 1 &&
				   model.audioEmitters[0].source == 0 && model.audioSources[0].bufferView == 0 &&
				   model.nodes[1].emitter == 0 && model.scenes[0].audioEmitters == std::vector<int>({0}),
			   "KHR_audio typed references are compacted and remapped");
	passed &= expect(
		model.nodes[1]
					.extensions.at("EXT_mesh_gpu_instancing")
					.Get("attributes")
					.Get("TRANSLATION")
					.GetNumberAsInt() == 0 &&
			primitive.extensions.at("KHR_draco_mesh_compression").Get("bufferView").GetNumberAsInt() == 0 &&
			primitive.extensions.at("KHR_materials_variants")
					.Get("mappings")
					.Get(0)
					.Get("material")
					.GetNumberAsInt() == 1 &&
			model.textures[0].extensions.at("KHR_texture_basisu").Get("source").GetNumberAsInt() == 0 &&
			model.bufferViews[0].extensions.at("EXT_meshopt_compression").Get("buffer").GetNumberAsInt() == 0,
		"known extension accessor, view, material, image, and "
		"buffer references are remapped");
	passed &= expect(
		model.nodes[1].extensions.at("MSFT_lod").Get("opaqueSentinel").GetNumberAsInt() == 37 &&
			model.materials[1].extensions.at("MSFT_lod").Get("opaqueSentinel").GetNumberAsInt() == 73 &&
			model.extras.Get("opaqueRootSentinel").GetNumberAsInt() == 101,
		"opaque known-extension siblings and extras are preserved");
	const tinygltf::Value& rootAudio = model.extensions.at("KHR_audio");
	passed &= expect(rootAudio.Get("emitters").ArrayLen() == 1 &&
						 rootAudio.Get("emitters").Get(0).Get("source").GetNumberAsInt() == 0 &&
						 rootAudio.Get("emitters").Get(0).Get("opaqueSentinel").GetNumberAsInt() == 43 &&
						 rootAudio.Get("sources").ArrayLen() == 1 &&
						 rootAudio.Get("sources").Get(0).Get("bufferView").GetNumberAsInt() == 0 &&
						 rootAudio.Get("sources").Get(0).Get("opaqueSentinel").GetNumberAsInt() == 47 &&
						 model.extensions.at("KHR_lights_punctual").Get("lights").ArrayLen() == 1 &&
						 model.extensions.at("KHR_lights_punctual")
								 .Get("lights")
								 .Get(0)
								 .Get("opaqueSentinel")
								 .GetNumberAsInt() == 91,
					 "typed root extension arrays keep opaque siblings while references are "
					 "remapped");
	const auto roundTripPath = temporary.path() / "compacted-roundtrip.glb";
	const GltfSaveResult saved = GltfDocumentIO::saveAs(*document, roundTripPath);
	passed &= expect(saved.succeeded(), "compacted authority saves after candidate validation");
	if (saved.succeeded())
	{
		auto reloaded = load(roundTripPath);
		passed &= expect(reloaded != nullptr && GltfValidator::validate(*reloaded).valid() &&
							 GltfSemanticCompare::compare(*document, *reloaded).equivalent(),
						 "compacted authority reloads with equivalent typed references and "
						 "payloads");
	}

	const Snapshot beforeSecond = snapshot(*document);
	const SceneCompactionResult second = SceneCompactor::compact(*document);
	passed &= expect(second.unchanged() && second.beforeRevision == revision + 1 &&
						 second.afterRevision == revision + 1 && snapshot(*document) == beforeSecond,
					 "a second Compact is an exact no-op");
	return passed;
}

bool expectRollback(SceneCompactionStatus expected, GltfDocument& document, EditorFaultInjector* faults,
					std::string_view message, const EditorLimits& limits = kDefaultEditorLimits)
{
	const Snapshot before = snapshot(document);
	const SceneCompactionResult result = SceneCompactor::compact(document, limits, faults);
	return expect(result.status == expected && result.beforeRevision == before.revision &&
					  result.afterRevision == before.revision && snapshot(document) == before,
				  message);
}

bool testRollbackBoundaries()
{
	TempDirectory temporary;
	bool passed = true;
	{
		auto document = makeFixture(temporary.path());
		if (!document)
			return false;
		demo::editor::detail::GltfDocumentAccess::model(*document).scenes[0].nodes[0] = 999;
		passed &= expectRollback(SceneCompactionStatus::invalidDocument, *document, nullptr,
								 "invalid core references reject without publication");
	}
	for (const std::size_t allocationCall : {std::size_t{1}, std::size_t{2}})
	{
		auto document = makeFixture(temporary.path());
		if (!document)
			return false;
		ScriptedFaultInjector faults;
		faults.failOn(EditorFaultPoint::allocation, allocationCall);
		passed &= expectRollback(SceneCompactionStatus::allocationFailed, *document, &faults,
								 allocationCall == 1
									 ? "candidate allocation fault rolls back exactly"
									 : "pre-publication result allocation fault rolls back exactly");
	}
	{
		auto document = makeFixture(temporary.path());
		if (!document)
			return false;
		ScriptedFaultInjector faults;
		faults.failOn(EditorFaultPoint::preCandidatePublish, 1);
		passed &= expectRollback(SceneCompactionStatus::injectedFailure, *document, &faults,
								 "pre-candidate publication fault rolls back exactly");
	}
	{
		auto document = makeFixture(temporary.path(), true);
		if (!document)
			return false;
		passed &= expectRollback(SceneCompactionStatus::unsafeExtensionReference, *document, nullptr,
								 "unknown index-bearing extension fails closed without publication");
	}
	{
		auto document = makeFixture(temporary.path());
		if (!document)
			return false;
		EditorLimits limits = kDefaultEditorLimits;
		limits.maxNodes = 1;
		passed &= expectRollback(SceneCompactionStatus::limitExceeded, *document, nullptr,
								 "EditorLimits rejection preserves the original document", limits);
	}
	{
		auto source = makeFixture(temporary.path());
		if (!source)
			return false;
		auto document = demo::editor::detail::GltfDocumentAccess::cloneWithDependencies(
			*source, {GltfDependency{GltfDependencyKind::image, 1, "tracked-image.bin",
									 temporary.path() / "tracked-image.bin", 4}});
		passed &= expectRollback(SceneCompactionStatus::dependencyRemapUnsupported, *document, nullptr,
								 "tracked external dependency index changes fail "
								 "closed without publication");
	}
	return passed;
}

} // namespace

int main()
{
	bool passed = true;
	passed &= testCompactionAndIdempotency();
	passed &= testRollbackBoundaries();
	return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
