#include <mgif/scene_editor/GltfDocumentIO.h>
#include <mgif/scene_editor/GltfSemanticCompare.h>
#include <mgif/scene_editor/GltfValidator.h>
#include <mgif/scene_editor/SceneCommands.h>

#include <tiny_gltf.h>

#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace demo::editor::detail {

class GltfDocumentAccess final
{
public:
	static void injectNonFiniteTranslation(GltfDocument& document)
	{
		document.mutableAuthorityModel().nodes.at(0).translation = {
			std::numeric_limits<double>::infinity(), 0.0, 0.0,
		};
		(void)document.advanceRevision();
	}

	static void injectMatrixAndTrs(GltfDocument& document)
	{
		auto& node = document.mutableAuthorityModel().nodes.at(0);
		node.matrix = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
		node.translation = {0, 0, 0};
	}
};

} // namespace demo::editor::detail

namespace {

using namespace demo::editor;

bool expect(bool condition, std::string_view message)
{
	if(!condition)
	{
		std::cerr << "FAILED: " << message << '\n';
	}
	return condition;
}

class TempDirectory final
{
public:
	TempDirectory()
	{
		static std::atomic<std::uint64_t> sequence{0};
		const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
		m_path = std::filesystem::temp_directory_path()
			/ ("mgif-p1-validator-" + std::to_string(ticks) + '-'
				+ std::to_string(sequence.fetch_add(1)));
		std::filesystem::create_directories(m_path);
	}

	~TempDirectory()
	{
		std::error_code ignored;
		std::filesystem::remove_all(m_path, ignored);
	}

	[[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }

private:
	std::filesystem::path m_path;
};

[[nodiscard]] std::filesystem::path fixture(std::string_view name)
{
	return std::filesystem::path(MGIF_TEST_SOURCE_DIR) / "tests" / "fixtures" / "gltf_editor" / name;
}

[[nodiscard]] std::string readText(const std::filesystem::path& path)
{
	std::ifstream stream(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

bool writeText(const std::filesystem::path& path, std::string_view text)
{
	std::ofstream stream(path, std::ios::binary | std::ios::trunc);
	stream.write(text.data(), static_cast<std::streamsize>(text.size()));
	stream.close();
	return !stream.fail();
}

bool replaceOnce(std::string& text, std::string_view from, std::string_view to)
{
	const std::size_t position = text.find(from);
	if(position == std::string::npos) return false;
	text.replace(position, from.size(), to);
	return true;
}

[[nodiscard]] std::string withMaterialExtension(
	const std::string& original,
	std::string_view name,
	std::string_view object)
{
	std::string result = original;
	const std::string declaration = "\"KHR_materials_clearcoat\", \""
		+ std::string(name) + "\",";
	if(!replaceOnce(result, "\"KHR_materials_clearcoat\",", declaration)) return {};
	const std::string member = "\"" + std::string(name) + "\": "
		+ std::string(object)
		+ ",\n        \"MGIF_test_unknown\": { \"materialSentinel\": 17 }";
	if(!replaceOnce(result,
	                "\"MGIF_test_unknown\": { \"materialSentinel\": 17 }",
	                member)) return {};
	return result;
}

bool expectInvalid(
	const std::filesystem::path& directory,
	std::string_view name,
	std::string json,
	GltfValidationErrorCode expectedCode)
{
	const auto path = directory / (std::string(name) + ".gltf");
	if(!writeText(path, json)) return false;
	auto result = GltfDocumentIO::loadCandidate(path);
	if(!expect(!result.succeeded() && result.error() == GltfLoadError::validationFailed,
	           std::string(name) + " is rejected by document validation")) return false;
	return expect(!result.validationReport().issues().empty()
		&& result.validationReport().issues().front().code == expectedCode,
		std::string(name) + " has the expected deterministic validation code");
}

[[nodiscard]] MaterialExtensionValue number(double value)
{
	return MaterialExtensionValue{value};
}

[[nodiscard]] MaterialExtensionValue integer(std::int32_t value)
{
	return MaterialExtensionValue{value};
}

[[nodiscard]] MaterialExtensionValue array(
	std::initializer_list<MaterialExtensionValue> values)
{
	return MaterialExtensionValue{MaterialExtensionValue::Array(values)};
}

[[nodiscard]] MaterialExtensionValue object(
	std::initializer_list<std::pair<std::string, MaterialExtensionValue>> values)
{
	return MaterialExtensionValue{MaterialExtensionValue::Object(values)};
}

[[nodiscard]] MaterialExtensionValue textureInfo(bool normal = false)
{
	MaterialExtensionValue::Object values{
		{"index", integer(0)},
		{"texCoord", integer(0)},
	};
	if(normal) values.emplace_back("scale", number(1.0));
	return MaterialExtensionValue{std::move(values)};
}

struct MaterialDocumentSnapshot final
{
	std::uint64_t revision{0};
	std::string digest;
	std::array<std::vector<EditorObjectId>,
		static_cast<std::size_t>(EditorObjectKind::count)> ids;
	std::string extensions;
	std::string extras;

	bool operator==(const MaterialDocumentSnapshot&) const = default;
};

[[nodiscard]] MaterialDocumentSnapshot materialSnapshot(
	const GltfDocument& document,
	MaterialId material)
{
	MaterialDocumentSnapshot result;
	result.revision = document.revision();
	result.digest = GltfSemanticCompare::digest(document);
	for(std::size_t kind = 1u;
		kind < static_cast<std::size_t>(EditorObjectKind::count); ++kind)
	{
		const auto type = static_cast<EditorObjectKind>(kind);
		for(std::size_t index = 0; index < document.objectCount(type); ++index)
			result.ids[kind].push_back(document.idAt(type, index));
	}
	result.extensions = document.objectExtensionsJson(material.untyped());
	result.extras = document.objectExtrasJson(material.untyped());
	return result;
}

[[nodiscard]] std::unique_ptr<GltfDocument> loadDocument(
	const std::filesystem::path& path)
{
	auto load = GltfDocumentIO::loadCandidate(path);
	std::unique_ptr<GltfDocument> document;
	if(!load.succeeded() || !std::move(load).publishTo(document)) return {};
	return document;
}

bool testStandardReferenceAndValueMatrix()
{
	TempDirectory temporary;
	const std::string original = readText(fixture("feature_preservation.gltf"));
	bool passed = true;
	const struct Case final
	{
		std::string_view name;
		std::string_view from;
		std::string_view to;
		GltfValidationErrorCode code;
	} cases[] = {
		{"scene-reference", "\"nodes\": [0, 1]", "\"nodes\": [0, 99]", GltfValidationErrorCode::indexOutOfRange},
		{"node-mesh", "\"mesh\": 0", "\"mesh\": 99", GltfValidationErrorCode::indexOutOfRange},
		{"node-skin", "\"skin\": 0", "\"skin\": 99", GltfValidationErrorCode::indexOutOfRange},
		{"node-camera", "\"camera\": 0", "\"camera\": 99", GltfValidationErrorCode::indexOutOfRange},
		{"node-light", "\"light\": 0", "\"light\": 99", GltfValidationErrorCode::indexOutOfRange},
		{"primitive-material", "\"material\": 0", "\"material\": 99", GltfValidationErrorCode::indexOutOfRange},
		{"primitive-accessor", "\"POSITION\": 0", "\"POSITION\": 99", GltfValidationErrorCode::indexOutOfRange},
		{"material-texture", "\"baseColorTexture\": { \"index\": 0 }", "\"baseColorTexture\": { \"index\": 99 }", GltfValidationErrorCode::indexOutOfRange},
		{"texture-image", "\"source\": 0", "\"source\": 99", GltfValidationErrorCode::indexOutOfRange},
		{"buffer-view-buffer", "\"buffer\": 0, \"byteOffset\": 0", "\"buffer\": 99, \"byteOffset\": 0", GltfValidationErrorCode::indexOutOfRange},
		{"accessor-buffer-view", "\"bufferView\": 0,", "\"bufferView\": 99,", GltfValidationErrorCode::indexOutOfRange},
		{"skin-joint", "\"joints\": [2]", "\"joints\": [99]", GltfValidationErrorCode::indexOutOfRange},
		{"animation-output", "\"output\": 4", "\"output\": 99", GltfValidationErrorCode::indexOutOfRange},
		{"buffer-range", "\"byteLength\": 36, \"target\": 34962", "\"byteLength\": 999, \"target\": 34962", GltfValidationErrorCode::invalidBufferRange},
		{"camera-range", "\"yfov\": 0.75", "\"yfov\": -0.75", GltfValidationErrorCode::invalidValue},
		{"light-range", "\"intensity\": 2.0", "\"intensity\": -2.0", GltfValidationErrorCode::invalidValue},
		{"material-range", "[0.25, 0.5, 0.75, 1.0]", "[1.25, 0.5, 0.75, 1.0]", GltfValidationErrorCode::invalidValue},
	};
	for(const auto& testCase : cases)
	{
		std::string changed = original;
		passed &= expect(replaceOnce(changed, testCase.from, testCase.to),
		                 std::string(testCase.name) + " fixture mutation is present");
		passed &= expectInvalid(temporary.path(), testCase.name, std::move(changed), testCase.code);
	}

	std::string undeclared = original;
	replaceOnce(undeclared,
	            ",\n    \"MGIF_test_unknown\"",
	            "");
	passed &= expectInvalid(temporary.path(), "undeclared-extension", std::move(undeclared),
	                        GltfValidationErrorCode::invalidExtensionDeclaration);
	return passed;
}

bool testKnownExtensionReferenceMatrix()
{
	TempDirectory temporary;
	const std::string original = readText(fixture("feature_preservation.gltf"));
	bool passed = true;

	std::string clearcoat = original;
	replaceOnce(clearcoat,
	            "\"clearcoatTexture\": { \"index\": 0 }",
	            "\"clearcoatTexture\": { \"index\": 99 }");
	passed &= expectInvalid(temporary.path(), "clearcoat-texture", std::move(clearcoat),
	                        GltfValidationErrorCode::indexOutOfRange);

	std::string alternate = original;
	replaceOnce(alternate,
	            "\"KHR_materials_clearcoat\",",
	            "\"KHR_materials_clearcoat\", \"KHR_texture_basisu\",");
	replaceOnce(alternate,
	            "\"textures\": [{ \"name\": \"Embedded Texture\", \"source\": 0 }]",
	            "\"textures\": [{ \"name\": \"Embedded Texture\", \"source\": 0, \"extensions\": { \"KHR_texture_basisu\": { \"source\": 99 } } }]");
	passed &= expectInvalid(temporary.path(), "alternate-texture-source", std::move(alternate),
	                        GltfValidationErrorCode::indexOutOfRange);

	std::string instancing = original;
	replaceOnce(instancing,
	            "\"KHR_materials_clearcoat\",",
	            "\"KHR_materials_clearcoat\", \"EXT_mesh_gpu_instancing\",");
	replaceOnce(instancing,
	            "\"weights\": [0.25],\n      \"extras\"",
	            "\"weights\": [0.25], \"extensions\": { \"EXT_mesh_gpu_instancing\": { \"attributes\": { \"TRANSLATION\": 99 } } },\n      \"extras\"");
	passed &= expectInvalid(temporary.path(), "gpu-instancing-accessor", std::move(instancing),
	                        GltfValidationErrorCode::indexOutOfRange);

	std::string draco = original;
	replaceOnce(draco,
	            "\"KHR_materials_clearcoat\",",
	            "\"KHR_materials_clearcoat\", \"KHR_draco_mesh_compression\",");
	replaceOnce(draco,
	            "\"targets\": [{ \"POSITION\": 0 }]",
	            "\"targets\": [{ \"POSITION\": 0 }], \"extensions\": { \"KHR_draco_mesh_compression\": { \"bufferView\": 99, \"attributes\": { \"POSITION\": 0 } } }");
	passed &= expectInvalid(temporary.path(), "draco-buffer-view", std::move(draco),
	                        GltfValidationErrorCode::indexOutOfRange);

	std::string variants = original;
	replaceOnce(variants,
	            "\"KHR_materials_clearcoat\",",
	            "\"KHR_materials_clearcoat\", \"KHR_materials_variants\",");
	replaceOnce(variants,
	            "\"KHR_lights_punctual\": {",
	            "\"KHR_materials_variants\": { \"variants\": [{ \"name\": \"Variant A\" }] }, \"KHR_lights_punctual\": {");
	replaceOnce(variants,
	            "\"targets\": [{ \"POSITION\": 0 }]",
	            "\"targets\": [{ \"POSITION\": 0 }], \"extensions\": { \"KHR_materials_variants\": { \"mappings\": [{ \"material\": 99, \"variants\": [0] }] } }");
	passed &= expectInvalid(temporary.path(), "variant-material", std::move(variants),
	                        GltfValidationErrorCode::indexOutOfRange);

	std::string meshopt = original;
	replaceOnce(meshopt,
	            "\"KHR_materials_clearcoat\",",
	            "\"KHR_materials_clearcoat\", \"EXT_meshopt_compression\",");
	replaceOnce(meshopt,
	            "{ \"buffer\": 0, \"byteOffset\": 0, \"byteLength\": 36, \"target\": 34962 }",
	            "{ \"buffer\": 0, \"byteOffset\": 0, \"byteLength\": 36, \"target\": 34962, \"extensions\": { \"EXT_meshopt_compression\": { \"buffer\": 99, \"byteOffset\": 0, \"byteLength\": 36, \"byteStride\": 12, \"count\": 3, \"mode\": \"ATTRIBUTES\", \"filter\": \"NONE\" } } }");
	passed &= expectInvalid(temporary.path(), "meshopt-buffer", std::move(meshopt),
	                        GltfValidationErrorCode::indexOutOfRange);
	return passed;
}

bool testLockedMaterialExtensionSchemas()
{
	TempDirectory temporary;
	const std::string original = readText(fixture("feature_preservation.gltf"));
	bool passed = true;

	std::string clearcoatRange = original;
	passed &= expect(replaceOnce(clearcoatRange, "\"clearcoatFactor\": 0.5", "\"clearcoatFactor\": -1.0"),
	                 "clearcoat range fixture mutation is present");
	passed &= expectInvalid(temporary.path(), "clearcoat-range", std::move(clearcoatRange),
	                        GltfValidationErrorCode::invalidValue);
	std::string clearcoatType = original;
	passed &= expect(replaceOnce(clearcoatType, "\"clearcoatFactor\": 0.5", "\"clearcoatFactor\": \"bad\""),
	                 "clearcoat type fixture mutation is present");
	passed &= expectInvalid(temporary.path(), "clearcoat-type", std::move(clearcoatType),
	                        GltfValidationErrorCode::invalidValue);

	const struct ExtensionCase final
	{
		std::string_view name;
		std::string_view extension;
		std::string_view value;
	} rangeCases[] = {
		{"anisotropy-range", "KHR_materials_anisotropy", R"({"anisotropyStrength":-0.01})"},
		{"diffuse-transmission-range", "KHR_materials_diffuse_transmission", R"({"diffuseTransmissionFactor":1.01})"},
		{"dispersion-range", "KHR_materials_dispersion", R"({"dispersion":-0.01})"},
		{"emissive-strength-range", "KHR_materials_emissive_strength", R"({"emissiveStrength":-0.01})"},
		{"ior-range", "KHR_materials_ior", R"({"ior":0.5})"},
		{"iridescence-range", "KHR_materials_iridescence", R"({"iridescenceFactor":1.01})"},
		{"iridescence-order", "KHR_materials_iridescence", R"({"iridescenceThicknessMinimum":2.0,"iridescenceThicknessMaximum":1.0})"},
		{"sheen-range", "KHR_materials_sheen", R"({"sheenRoughnessFactor":1.01})"},
		{"specular-range", "KHR_materials_specular", R"({"specularFactor":1.01})"},
		{"specular-color-range", "KHR_materials_specular", R"({"specularColorFactor":[-0.01,2.0,2.0]})"},
		{"transmission-range", "KHR_materials_transmission", R"({"transmissionFactor":1.01})"},
		{"volume-range", "KHR_materials_volume", R"({"attenuationDistance":0.0})"},
		{"volume-scatter-range", "KHR_materials_volume_scatter", R"({"scatterAnisotropy":1.01})"},
		{"specular-glossiness-range", "KHR_materials_pbrSpecularGlossiness", R"({"glossinessFactor":1.01})"},
	};
	for(const auto& testCase : rangeCases)
	{
		std::string changed = withMaterialExtension(original, testCase.extension, testCase.value);
		passed &= expect(!changed.empty(), std::string(testCase.name) + " fixture mutation is present");
		passed &= expectInvalid(temporary.path(), testCase.name, std::move(changed),
		                        GltfValidationErrorCode::invalidValue);
	}

	const ExtensionCase typeCases[] = {
		{"anisotropy-type", "KHR_materials_anisotropy", R"({"anisotropyRotation":"bad"})"},
		{"diffuse-transmission-type", "KHR_materials_diffuse_transmission", R"({"diffuseTransmissionColorFactor":[0,1]})"},
		{"dispersion-type", "KHR_materials_dispersion", R"({"dispersion":true})"},
		{"emissive-strength-type", "KHR_materials_emissive_strength", R"({"emissiveStrength":"bad"})"},
		{"ior-type", "KHR_materials_ior", R"({"ior":"bad"})"},
		{"iridescence-type", "KHR_materials_iridescence", R"({"iridescenceThicknessMinimum":[true]})"},
		{"sheen-type", "KHR_materials_sheen", R"({"sheenColorFactor":[0.0,"bad",1.0]})"},
		{"specular-type", "KHR_materials_specular", R"({"specularColorFactor":false})"},
		{"transmission-type", "KHR_materials_transmission", R"({"transmissionFactor":{"bad":true}})"},
		{"volume-type", "KHR_materials_volume", R"({"thicknessFactor":"bad"})"},
		{"volume-scatter-type", "KHR_materials_volume_scatter", R"({"multiscatterColor":[0.0,1.0]})"},
		{"specular-glossiness-type", "KHR_materials_pbrSpecularGlossiness", R"({"diffuseFactor":[0.0,0.0,0.0]})"},
	};
	for(const auto& testCase : typeCases)
	{
		std::string changed = withMaterialExtension(original, testCase.extension, testCase.value);
		passed &= expect(!changed.empty(), std::string(testCase.name) + " fixture mutation is present");
		passed &= expectInvalid(temporary.path(), testCase.name, std::move(changed),
		                        GltfValidationErrorCode::invalidValue);
	}

	auto unlitDocument = loadDocument(fixture("material_extensions.gltf"));
	passed &= expect(unlitDocument != nullptr, "unlit type mutation fixture loads");
	if(unlitDocument)
	{
		SceneCommands commands(*unlitDocument);
		const MaterialId material = *MaterialId::from(
			unlitDocument->idAt(EditorObjectKind::material, 0));
		MaterialExtensionValue invalidUnlit;
		invalidUnlit.value = MaterialExtensionValue::Array{
			MaterialExtensionValue{true}};
		const auto before = materialSnapshot(*unlitDocument, material);
		const auto result = commands.setMaterialExtension(
			material, "KHR_materials_unlit", invalidUnlit);
		passed &= expect(result.rejected()
			&& result.error == MutationErrorCode::validationFailed
			&& materialSnapshot(*unlitDocument, material) == before,
			"unlit-type is rejected before public mutation publication");
	}

	const ExtensionCase textureCases[] = {
		{"anisotropy-texture", "KHR_materials_anisotropy", R"({"anisotropyTexture":{"index":99}})"},
		{"diffuse-transmission-texture", "KHR_materials_diffuse_transmission", R"({"diffuseTransmissionTexture":{"index":99}})"},
		{"diffuse-transmission-color-texture", "KHR_materials_diffuse_transmission", R"({"diffuseTransmissionColorTexture":{"index":99}})"},
		{"iridescence-texture", "KHR_materials_iridescence", R"({"iridescenceTexture":{"index":99}})"},
		{"iridescence-thickness-texture", "KHR_materials_iridescence", R"({"iridescenceThicknessTexture":{"index":99}})"},
		{"sheen-color-texture", "KHR_materials_sheen", R"({"sheenColorTexture":{"index":99}})"},
		{"sheen-roughness-texture", "KHR_materials_sheen", R"({"sheenRoughnessTexture":{"index":99}})"},
		{"specular-texture", "KHR_materials_specular", R"({"specularTexture":{"index":99}})"},
		{"specular-color-texture", "KHR_materials_specular", R"({"specularColorTexture":{"index":99}})"},
		{"transmission-texture", "KHR_materials_transmission", R"({"transmissionTexture":{"index":99}})"},
		{"volume-texture", "KHR_materials_volume", R"({"thicknessTexture":{"index":99}})"},
		{"specular-glossiness-diffuse-texture", "KHR_materials_pbrSpecularGlossiness", R"({"diffuseTexture":{"index":99}})"},
		{"specular-glossiness-texture", "KHR_materials_pbrSpecularGlossiness", R"({"specularGlossinessTexture":{"index":99}})"},
	};
	for(const auto& testCase : textureCases)
	{
		std::string changed = withMaterialExtension(original, testCase.extension, testCase.value);
		passed &= expect(!changed.empty(), std::string(testCase.name) + " fixture mutation is present");
		passed &= expectInvalid(temporary.path(), testCase.name, std::move(changed),
		                        GltfValidationErrorCode::indexOutOfRange);
	}

	for(const auto& [name, value] : {
		std::pair{"clearcoat-roughness-texture", std::string_view{"\"clearcoatRoughnessFactor\": 0.0, \"clearcoatRoughnessTexture\": { \"index\": 99 },"}},
		std::pair{"clearcoat-normal-texture", std::string_view{"\"clearcoatRoughnessFactor\": 0.0, \"clearcoatNormalTexture\": { \"index\": 99 },"}},
	})
	{
		std::string changed = original;
		passed &= expect(replaceOnce(changed, "\"clearcoatTexture\": { \"index\": 0 }",
		                            std::string(value) + " \"clearcoatTexture\": { \"index\": 0 }"),
		                 std::string(name) + " fixture mutation is present");
		passed &= expectInvalid(temporary.path(), name, std::move(changed),
		                        GltfValidationErrorCode::indexOutOfRange);
	}

	const ExtensionCase compatibilityCases[] = {
		{"dispersion-dependency", "KHR_materials_dispersion", R"({"dispersion":1.0})"},
		{"volume-dependency", "KHR_materials_volume", R"({"thicknessFactor":1.0})"},
		{"volume-scatter-dependency", "KHR_materials_volume_scatter", R"({"scatterAnisotropy":0.0})"},
		{"unlit-exclusion", "KHR_materials_unlit", R"({})"},
		{"specular-glossiness-exclusion", "KHR_materials_pbrSpecularGlossiness", R"({"glossinessFactor":0.5})"},
	};
	for(const auto& testCase : compatibilityCases)
	{
		std::string changed = withMaterialExtension(original, testCase.extension, testCase.value);
		passed &= expectInvalid(temporary.path(), testCase.name, std::move(changed),
		                        GltfValidationErrorCode::invalidValue);
	}
	std::string emissiveUnlit = withMaterialExtension(original, "KHR_materials_unlit", R"({})");
	emissiveUnlit = withMaterialExtension(emissiveUnlit, "KHR_materials_emissive_strength",
	                                    R"({"emissiveStrength":1.0})");
	passed &= expectInvalid(temporary.path(), "emissive-unlit-exclusion", std::move(emissiveUnlit),
	                        GltfValidationErrorCode::invalidValue);

	std::string texCoord = original;
	passed &= expect(replaceOnce(texCoord,
	                            "\"clearcoatTexture\": { \"index\": 0 }",
	                            "\"clearcoatTexture\": { \"index\": 0, \"texCoord\": -1 }"),
	                 "texture coordinate fixture mutation is present");
	passed &= expectInvalid(temporary.path(), "texture-coordinate-type", std::move(texCoord),
	                        GltfValidationErrorCode::invalidValue);
	std::string normalScale = original;
	passed &= expect(replaceOnce(normalScale,
	                            "\"clearcoatTexture\": { \"index\": 0 }",
	                            "\"clearcoatNormalTexture\": { \"index\": 0, \"scale\": \"bad\" }, \"clearcoatTexture\": { \"index\": 0 }"),
	                 "normal texture scale fixture mutation is present");
	passed &= expectInvalid(temporary.path(), "normal-texture-scale-type", std::move(normalScale),
	                        GltfValidationErrorCode::invalidValue);
	std::string transform = original;
	passed &= expect(replaceOnce(transform,
	                            "\"KHR_materials_clearcoat\",",
	                            "\"KHR_materials_clearcoat\", \"KHR_texture_transform\","),
	                 "texture transform declaration fixture mutation is present");
	passed &= expect(replaceOnce(transform,
	                            "\"clearcoatTexture\": { \"index\": 0 }",
	                            "\"clearcoatTexture\": { \"index\": 0, \"extensions\": { \"KHR_texture_transform\": { \"offset\": [0.0] } } }"),
	                 "texture transform fixture mutation is present");
	passed &= expectInvalid(temporary.path(), "texture-transform-shape", std::move(transform),
	                        GltfValidationErrorCode::invalidValue);
	return passed;
}

bool installMaterialDependencies(
	SceneCommands& commands,
	MaterialId material,
	std::string_view extension)
{
	if(extension != "KHR_materials_volume"
		&& extension != "KHR_materials_dispersion"
		&& extension != "KHR_materials_volume_scatter")
	{
		return true;
	}
	const auto transmission = object({
		{"transmissionFactor", number(0.5)},
		{"transmissionTexture", textureInfo()},
	});
	if(!commands.setMaterialExtension(
		material, "KHR_materials_transmission", transmission).applied())
	{
		return false;
	}
	if(extension == "KHR_materials_volume") return true;
	const auto volume = object({
		{"thicknessFactor", number(0.5)},
		{"thicknessTexture", textureInfo()},
		{"attenuationDistance", number(1.0)},
		{"attenuationColor", array({number(0.5), number(0.5), number(0.5)})},
	});
	return commands.setMaterialExtension(
		material, "KHR_materials_volume", volume).applied();
}

bool testMaterialMutationBoundariesAndRoundTrip()
{
	const double maximumFloat = static_cast<double>(std::numeric_limits<float>::max());
	const double minimumPositiveFloat = static_cast<double>(std::numeric_limits<float>::min());
	const double rotationLimit = static_cast<double>(3.14f);
	const struct BoundaryCase final
	{
		std::string_view extension;
		std::vector<MaterialExtensionValue> boundaries;
	} cases[] = {
		{"KHR_materials_anisotropy", {
			object({{"anisotropyStrength", number(0.0)}, {"anisotropyRotation", number(-rotationLimit)}, {"anisotropyTexture", textureInfo()}}),
			object({{"anisotropyStrength", number(0.5)}, {"anisotropyRotation", number(0.0)}, {"anisotropyTexture", textureInfo()}}),
			object({{"anisotropyStrength", number(1.0)}, {"anisotropyRotation", number(rotationLimit)}, {"anisotropyTexture", textureInfo()}}),
		}},
		{"KHR_materials_clearcoat", {
			object({{"clearcoatFactor", number(0.0)}, {"clearcoatRoughnessFactor", number(0.0)}, {"clearcoatTexture", textureInfo()}, {"clearcoatRoughnessTexture", textureInfo()}, {"clearcoatNormalTexture", textureInfo(true)}}),
			object({{"clearcoatFactor", number(0.5)}, {"clearcoatRoughnessFactor", number(0.5)}, {"clearcoatTexture", textureInfo()}, {"clearcoatRoughnessTexture", textureInfo()}, {"clearcoatNormalTexture", textureInfo(true)}, {"MGIF_unknown_member", integer(7)}, {"extras", object({{"sentinel", integer(9)}})}}),
			object({{"clearcoatFactor", number(1.0)}, {"clearcoatRoughnessFactor", number(1.0)}, {"clearcoatTexture", textureInfo()}, {"clearcoatRoughnessTexture", textureInfo()}, {"clearcoatNormalTexture", textureInfo(true)}}),
		}},
		{"KHR_materials_diffuse_transmission", {
			object({{"diffuseTransmissionFactor", number(0.0)}, {"diffuseTransmissionColorFactor", array({number(0.0), number(0.0), number(0.0)})}, {"diffuseTransmissionTexture", textureInfo()}, {"diffuseTransmissionColorTexture", textureInfo()}}),
			object({{"diffuseTransmissionFactor", number(0.5)}, {"diffuseTransmissionColorFactor", array({number(0.5), number(0.5), number(0.5)})}, {"diffuseTransmissionTexture", textureInfo()}, {"diffuseTransmissionColorTexture", textureInfo()}}),
			object({{"diffuseTransmissionFactor", number(1.0)}, {"diffuseTransmissionColorFactor", array({number(1.0), number(1.0), number(1.0)})}, {"diffuseTransmissionTexture", textureInfo()}, {"diffuseTransmissionColorTexture", textureInfo()}}),
		}},
		{"KHR_materials_dispersion", {
			object({{"dispersion", number(0.0)}}),
			object({{"dispersion", number(5.0)}}),
			object({{"dispersion", number(10.0)}}),
		}},
		{"KHR_materials_emissive_strength", {
			object({{"emissiveStrength", number(0.0)}}),
			object({{"emissiveStrength", number(1.0)}}),
			object({{"emissiveStrength", number(maximumFloat)}}),
		}},
		{"KHR_materials_ior", {
			object({{"ior", number(0.0)}}),
			object({{"ior", number(1.5)}}),
			object({{"ior", number(10.0)}}),
		}},
		{"KHR_materials_iridescence", {
			object({{"iridescenceFactor", number(0.0)}, {"iridescenceIor", number(1.0)}, {"iridescenceThicknessMinimum", number(0.0)}, {"iridescenceThicknessMaximum", number(0.0)}, {"iridescenceTexture", textureInfo()}, {"iridescenceThicknessTexture", textureInfo()}}),
			object({{"iridescenceFactor", number(0.5)}, {"iridescenceIor", number(5.5)}, {"iridescenceThicknessMinimum", number(500.0)}, {"iridescenceThicknessMaximum", number(500.0)}, {"iridescenceTexture", textureInfo()}, {"iridescenceThicknessTexture", textureInfo()}}),
			object({{"iridescenceFactor", number(1.0)}, {"iridescenceIor", number(10.0)}, {"iridescenceThicknessMinimum", number(1000.0)}, {"iridescenceThicknessMaximum", number(1000.0)}, {"iridescenceTexture", textureInfo()}, {"iridescenceThicknessTexture", textureInfo()}}),
		}},
		{"KHR_materials_sheen", {
			object({{"sheenColorFactor", array({number(0.0), number(0.0), number(0.0)})}, {"sheenRoughnessFactor", number(0.0)}, {"sheenColorTexture", textureInfo()}, {"sheenRoughnessTexture", textureInfo()}}),
			object({{"sheenColorFactor", array({number(0.5), number(0.5), number(0.5)})}, {"sheenRoughnessFactor", number(0.5)}, {"sheenColorTexture", textureInfo()}, {"sheenRoughnessTexture", textureInfo()}}),
			object({{"sheenColorFactor", array({number(1.0), number(1.0), number(1.0)})}, {"sheenRoughnessFactor", number(1.0)}, {"sheenColorTexture", textureInfo()}, {"sheenRoughnessTexture", textureInfo()}}),
		}},
		{"KHR_materials_specular", {
			object({{"specularFactor", number(0.0)}, {"specularColorFactor", array({number(0.0), number(0.0), number(0.0)})}, {"specularTexture", textureInfo()}, {"specularColorTexture", textureInfo()}}),
			object({{"specularFactor", number(0.5)}, {"specularColorFactor", array({number(0.5), number(0.5), number(0.5)})}, {"specularTexture", textureInfo()}, {"specularColorTexture", textureInfo()}}),
			object({{"specularFactor", number(1.0)}, {"specularColorFactor", array({number(2.0), number(2.0), number(2.0)})}, {"specularTexture", textureInfo()}, {"specularColorTexture", textureInfo()}}),
		}},
		{"KHR_materials_transmission", {
			object({{"transmissionFactor", number(0.0)}, {"transmissionTexture", textureInfo()}}),
			object({{"transmissionFactor", number(0.5)}, {"transmissionTexture", textureInfo()}}),
			object({{"transmissionFactor", number(1.0)}, {"transmissionTexture", textureInfo()}}),
		}},
		{"KHR_materials_unlit", {
			object({}),
		}},
		{"KHR_materials_volume", {
			object({{"thicknessFactor", number(0.0)}, {"thicknessTexture", textureInfo()}, {"attenuationDistance", number(minimumPositiveFloat)}, {"attenuationColor", array({number(0.0), number(0.0), number(0.0)})}}),
			object({{"thicknessFactor", number(0.5)}, {"thicknessTexture", textureInfo()}, {"attenuationDistance", number(1.0)}, {"attenuationColor", array({number(0.5), number(0.5), number(0.5)})}}),
			object({{"thicknessFactor", number(1.0)}, {"thicknessTexture", textureInfo()}, {"attenuationDistance", number(maximumFloat)}, {"attenuationColor", array({number(1.0), number(1.0), number(1.0)})}}),
		}},
		{"KHR_materials_volume_scatter", {
			object({{"multiscatterColor", array({number(0.0), number(0.0), number(0.0)})}, {"scatterAnisotropy", number(-1.0)}}),
			object({{"multiscatterColor", array({number(0.5), number(0.5), number(0.5)})}, {"scatterAnisotropy", number(0.0)}}),
			object({{"multiscatterColor", array({number(1.0), number(1.0), number(1.0)})}, {"scatterAnisotropy", number(1.0)}}),
		}},
		{"KHR_materials_pbrSpecularGlossiness", {
			object({{"diffuseFactor", array({number(0.0), number(0.0), number(0.0), number(0.0)})}, {"diffuseTexture", textureInfo()}, {"specularFactor", array({number(0.0), number(0.0), number(0.0)})}, {"glossinessFactor", number(0.0)}, {"specularGlossinessTexture", textureInfo()}}),
			object({{"diffuseFactor", array({number(0.5), number(0.5), number(0.5), number(0.5)})}, {"diffuseTexture", textureInfo()}, {"specularFactor", array({number(0.5), number(0.5), number(0.5)})}, {"glossinessFactor", number(0.5)}, {"specularGlossinessTexture", textureInfo()}}),
			object({{"diffuseFactor", array({number(1.0), number(1.0), number(1.0), number(1.0)})}, {"diffuseTexture", textureInfo()}, {"specularFactor", array({number(1.0), number(1.0), number(1.0)})}, {"glossinessFactor", number(1.0)}, {"specularGlossinessTexture", textureInfo()}}),
		}},
	};

	bool passed = true;
	const auto materialFixture = fixture("material_extensions.gltf");
	TempDirectory temporary;
	for(const auto& testCase : cases)
	{
		{
			auto document = loadDocument(materialFixture);
			if(!document) return false;
			SceneCommands commands(*document);
			const MaterialId material = *MaterialId::from(
				document->idAt(EditorObjectKind::material, 0));
			MaterialExtensionValue wrongRootType;
			wrongRootType.value = true;
			const auto before = materialSnapshot(*document, material);
			const auto result = commands.setMaterialExtension(
				material, std::string(testCase.extension), wrongRootType);
			passed &= expect(result.rejected()
				&& result.error == MutationErrorCode::validationFailed
				&& materialSnapshot(*document, material) == before,
				std::string(testCase.extension)
					+ " rejects a non-object root before publication");
		}
		for(std::size_t boundary = 0; boundary < testCase.boundaries.size(); ++boundary)
		{
			auto document = loadDocument(materialFixture);
			passed &= expect(document != nullptr,
			                 std::string(testCase.extension) + " boundary fixture loads");
			if(!document) continue;
			SceneCommands commands(*document);
			const auto material = MaterialId::from(
				document->idAt(EditorObjectKind::material, 0));
			if(!material) return false;
			passed &= expect(installMaterialDependencies(
				commands, *material, testCase.extension),
				std::string(testCase.extension) + " dependencies install");
			const auto unknownBefore = commands.materialExtension(
				*material, "MGIF_test_unknown");
			const std::string extrasBefore(
				document->objectExtrasJson(material->untyped()));
			const auto before = materialSnapshot(*document, *material);
			const auto result = commands.setMaterialExtension(
				*material, std::string(testCase.extension), testCase.boundaries[boundary]);
			passed &= expect(result.applied()
				&& result.beforeRevision == before.revision
				&& result.afterRevision == before.revision + 1u
				&& document->revision() == result.afterRevision,
				std::string(testCase.extension) + " boundary applies with exact revision semantics");
			passed &= expect(commands.materialExtension(*material, testCase.extension)
				== testCase.boundaries[boundary]
				&& commands.materialExtension(*material, "MGIF_test_unknown") == unknownBefore
				&& document->objectExtrasJson(material->untyped()) == extrasBefore
				&& GltfValidator::validate(*document).valid(),
				std::string(testCase.extension) + " boundary preserves unknown sibling data");
			const auto after = materialSnapshot(*document, *material);
			passed &= expect(after.ids == before.ids,
				std::string(testCase.extension) + " boundary preserves every typed ID");
		}

		auto document = loadDocument(materialFixture);
		if(!document) return false;
		SceneCommands commands(*document);
		const MaterialId material = *MaterialId::from(
			document->idAt(EditorObjectKind::material, 0));
		passed &= expect(installMaterialDependencies(
			commands, material, testCase.extension),
			std::string(testCase.extension) + " round-trip dependencies install");
		const auto& persisted = testCase.boundaries[testCase.boundaries.size() / 2u];
		passed &= expect(commands.setMaterialExtension(
			material, std::string(testCase.extension), persisted).applied(),
			std::string(testCase.extension) + " add applies");
		const auto withExtension = temporary.path()
			/ (std::string(testCase.extension) + "-present.gltf");
		passed &= expect(GltfDocumentIO::saveAs(*document, withExtension).succeeded(),
			std::string(testCase.extension) + " saves");
		auto reloaded = loadDocument(withExtension);
		passed &= expect(reloaded != nullptr,
			std::string(testCase.extension) + " reloads after add");
		if(!reloaded) continue;
		SceneCommands reloadedCommands(*reloaded);
		const MaterialId reloadedMaterial = *MaterialId::from(
			reloaded->idAt(EditorObjectKind::material, 0));
		passed &= expect(reloadedCommands.materialExtension(
			reloadedMaterial, testCase.extension) == persisted
			&& reloadedCommands.materialExtension(reloadedMaterial, "MGIF_test_unknown")
				== commands.materialExtension(material, "MGIF_test_unknown")
			&& reloaded->objectExtrasJson(reloadedMaterial.untyped())
				== document->objectExtrasJson(material.untyped()),
			std::string(testCase.extension) + " add/save/reload preserves value and unknown data");
		const auto removeResult = reloadedCommands.setMaterialExtension(
			reloadedMaterial, std::string(testCase.extension), std::nullopt);
		passed &= expect(removeResult.applied()
			&& removeResult.afterRevision == removeResult.beforeRevision + 1u,
			std::string(testCase.extension) + " remove applies");
		const auto withoutExtension = temporary.path()
			/ (std::string(testCase.extension) + "-removed.gltf");
		passed &= expect(GltfDocumentIO::saveAs(*reloaded, withoutExtension).succeeded(),
			std::string(testCase.extension) + " removal saves");
		auto removed = loadDocument(withoutExtension);
		passed &= expect(removed != nullptr,
			std::string(testCase.extension) + " reloads after remove");
		if(removed)
		{
			SceneCommands removedCommands(*removed);
			const MaterialId removedMaterial = *MaterialId::from(
				removed->idAt(EditorObjectKind::material, 0));
			passed &= expect(!removedCommands.materialExtension(
				removedMaterial, testCase.extension)
				&& removedCommands.materialExtension(removedMaterial, "MGIF_test_unknown")
					== commands.materialExtension(material, "MGIF_test_unknown")
				&& removed->objectExtrasJson(removedMaterial.untyped())
					== document->objectExtrasJson(material.untyped()),
				std::string(testCase.extension) + " remove/save/reload preserves unknown data");
		}
	}

	auto rejectedDocument = loadDocument(materialFixture);
	if(!rejectedDocument) return false;
	SceneCommands rejectedCommands(*rejectedDocument);
	const MaterialId rejectedMaterial = *MaterialId::from(
		rejectedDocument->idAt(EditorObjectKind::material, 0));
	const auto rejectedBefore = materialSnapshot(*rejectedDocument, rejectedMaterial);
	const auto invalidClearcoat = object({{"clearcoatFactor", number(-1.0)}});
	const auto rejected = rejectedCommands.setMaterialExtension(
		rejectedMaterial, "KHR_materials_clearcoat", invalidClearcoat);
	passed &= expect(rejected.rejected()
		&& rejected.error == MutationErrorCode::validationFailed
		&& rejected.beforeRevision == rejected.afterRevision
		&& materialSnapshot(*rejectedDocument, rejectedMaterial) == rejectedBefore,
		"clearcoatFactor=-1 is rejected before publication with an exact document snapshot");

	auto workflowDocument = loadDocument(materialFixture);
	if(!workflowDocument) return false;
	SceneCommands workflowCommands(*workflowDocument);
	const MaterialId workflowMaterial = *MaterialId::from(
		workflowDocument->idAt(EditorObjectKind::material, 0));
	passed &= expect(workflowCommands.setMaterialExtension(
		workflowMaterial, "KHR_materials_unlit", object({})).applied(),
		"unlit workflow installs on a clean material");
	const auto workflowBefore = materialSnapshot(*workflowDocument, workflowMaterial);
	const auto conflictingWorkflow = workflowCommands.setMaterialExtension(
		workflowMaterial, "KHR_materials_pbrSpecularGlossiness",
		object({{"glossinessFactor", number(0.5)}}));
	passed &= expect(conflictingWorkflow.rejected()
		&& conflictingWorkflow.error == MutationErrorCode::validationFailed
		&& materialSnapshot(*workflowDocument, workflowMaterial) == workflowBefore,
		"unlit and specular-glossiness workflows reject atomically");
	return passed;
}

bool testLimitsNonFiniteAndInvalidSave()
{
	bool passed = true;
	GltfLoadOptions depthOptions;
	depthOptions.limits.maxJsonDepth = 1;
	auto depth = GltfDocumentIO::loadCandidate(fixture("unknown_extension_roundtrip.gltf"), depthOptions);
	passed &= expect(!depth.succeeded() && depth.error() == GltfLoadError::validationFailed
		&& !depth.validationReport().issues().empty()
		&& depth.validationReport().issues().front().code == GltfValidationErrorCode::limitExceeded,
		"nested extension/extras JSON depth limit is enforced");

	GltfLoadOptions stringOptions;
	stringOptions.limits.maxStringBytes = 4;
	auto strings = GltfDocumentIO::loadCandidate(fixture("minimal_scene.gltf"), stringOptions);
	passed &= expect(!strings.succeeded() && strings.error() == GltfLoadError::validationFailed,
	                 "standard glTF strings are bounded");

	GltfLoadOptions payloadOptions;
	payloadOptions.limits.maxAuthorityPayloadBytes = 100;
	auto payload = GltfDocumentIO::loadCandidate(fixture("feature_preservation.gltf"), payloadOptions);
	passed &= expect(!payload.succeeded() && payload.error() == GltfLoadError::validationFailed,
	                 "aggregate in-memory authority payload is bounded");

	auto result = GltfDocumentIO::loadCandidate(fixture("minimal_scene.gltf"));
	std::unique_ptr<GltfDocument> document;
	if(!result.succeeded() || !std::move(result).publishTo(document)) return false;
	demo::editor::detail::GltfDocumentAccess::injectNonFiniteTranslation(*document);
	const auto validation = GltfValidator::validate(*document);
	passed &= expect(!validation.valid() && !validation.issues().empty()
		&& validation.issues().front().code == GltfValidationErrorCode::invalidTransform,
		"non-finite authority number is rejected before persistence");

	auto transformResult = GltfDocumentIO::loadCandidate(fixture("minimal_scene.gltf"));
	std::unique_ptr<GltfDocument> transformDocument;
	if(!transformResult.succeeded() || !std::move(transformResult).publishTo(transformDocument)) return false;
	demo::editor::detail::GltfDocumentAccess::injectMatrixAndTrs(*transformDocument);
	const auto transformValidation = GltfValidator::validate(*transformDocument);
	passed &= expect(!transformValidation.valid() && !transformValidation.issues().empty()
		&& transformValidation.issues().front().code == GltfValidationErrorCode::invalidTransform,
		"matrix and TRS are mutually exclusive in the mutable authority");

	TempDirectory temporary;
	const auto target = temporary.path() / "existing.gltf";
	writeText(target, "ORIGINAL");
	const auto save = GltfDocumentIO::saveAs(*document, target);
	passed &= expect(!save.succeeded() && save.error() == GltfSaveError::invalidDocument
		&& readText(target) == "ORIGINAL" && document->dirty()
		&& document->cleanRevision() != document->revision(),
		"invalid-document Save As preserves the target and never marks the document clean");
	return passed;
}

} // namespace

int main()
{
	bool passed = true;
	passed &= testStandardReferenceAndValueMatrix();
	passed &= testKnownExtensionReferenceMatrix();
	passed &= testLockedMaterialExtensionSchemas();
	passed &= testMaterialMutationBoundariesAndRoundTrip();
	passed &= testLimitsNonFiniteAndInvalidSave();
	return passed ? 0 : 1;
}
