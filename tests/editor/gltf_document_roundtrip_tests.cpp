#include <mgif/scene_editor/GltfDocumentIO.h>
#include <mgif/scene_editor/GltfSemanticCompare.h>
#include <mgif/scene_editor/SceneChangeCanonicalizer.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace demo::editor::detail {

class GltfDocumentAccess final
{
public:
	[[nodiscard]] static std::string canonicalText(const GltfDocument& document)
	{
		std::string error;
		return document.semanticRepresentation(error);
	}

	[[nodiscard]] static bool advanceRevision(GltfDocument& document) noexcept
	{
		return document.advanceRevision();
	}
};

} // namespace demo::editor::detail

namespace {

using demo::editor::EditorObjectId;
using demo::editor::EditorObjectKind;
using demo::editor::GltfDocument;
using demo::editor::GltfDocumentFormat;
using demo::editor::GltfDocumentIO;
using demo::editor::GltfSemanticCompare;

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
			/ ("mgif-p1-roundtrip-" + std::to_string(ticks) + '-'
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

bool writeText(const std::filesystem::path& path, std::string_view text)
{
	std::ofstream stream(path, std::ios::binary | std::ios::trunc);
	stream.write(text.data(), static_cast<std::streamsize>(text.size()));
	stream.close();
	return !stream.fail();
}

bool writeBytes(const std::filesystem::path& path, const std::vector<unsigned char>& bytes)
{
	std::ofstream stream(path, std::ios::binary | std::ios::trunc);
	stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	stream.close();
	return !stream.fail();
}

[[nodiscard]] std::unique_ptr<GltfDocument> load(const std::filesystem::path& path)
{
	auto result = GltfDocumentIO::loadCandidate(path);
	if(!result.succeeded())
	{
		std::cerr << "load failed for " << path.string() << ": " << result.message() << '\n';
		for(const auto& issue : result.validationReport().issues())
		{
			std::cerr << "  " << issue.path << ": " << issue.message << '\n';
		}
		return {};
	}
	std::unique_ptr<GltfDocument> document;
	if(!std::move(result).publishTo(document))
	{
		std::cerr << "candidate publication failed for " << path.string() << '\n';
	}
	return document;
}

[[nodiscard]] std::vector<EditorObjectId> captureIds(const GltfDocument& document)
{
	std::vector<EditorObjectId> ids;
	for(std::uint8_t raw = static_cast<std::uint8_t>(EditorObjectKind::scene);
		raw < static_cast<std::uint8_t>(EditorObjectKind::count);
		++raw)
	{
		const auto kind = static_cast<EditorObjectKind>(raw);
		for(std::size_t index = 0; index < document.objectCount(kind); ++index)
		{
			ids.push_back(document.idAt(kind, index));
		}
	}
	return ids;
}

bool testJsonFormattingAndDifferenceCoverage()
{
	TempDirectory temporary;
	const std::string leftJson = R"({
  "asset":{"version":"2.0","extras":{"tag":1}},
  "extensionsUsed":["MGIF_test_unknown"],
  "extensions":{"MGIF_test_unknown":{"value":7}},
  "scene":0,
  "scenes":[{"nodes":[0]}],
  "nodes":[{"name":"Node"}]
})";
	const std::string reorderedJson = R"({"nodes":[{"name":"Node"}],"scenes":[{"nodes":[0]}],"scene":0,"extensions":{"MGIF_test_unknown":{"value":7}},"extensionsUsed":["MGIF_test_unknown"],"asset":{"extras":{"tag":1},"version":"2.0"}})";
	const auto leftPath = temporary.path() / "left.gltf";
	const auto reorderedPath = temporary.path() / "reordered.gltf";
	if(!writeText(leftPath, leftJson) || !writeText(reorderedPath, reorderedJson)) return false;
	auto left = load(leftPath);
	auto reordered = load(reorderedPath);
	bool passed = expect(left && reordered, "formatting comparator inputs load");
	if(!left || !reordered) return false;
	const auto equal = GltfSemanticCompare::compare(*left, *reordered);
	passed &= expect(equal.equivalent(), "JSON whitespace and object-key order are semantically irrelevant");
	passed &= expect(!equal.leftDigest().empty() && equal.leftDigest() == equal.rightDigest(),
	                 "equivalent documents have an exact stable semantic digest");

	const struct Variant final { std::string name; std::string json; } variants[] = {
		{"collection", R"({"asset":{"version":"2.0","extras":{"tag":1}},"extensionsUsed":["MGIF_test_unknown"],"extensions":{"MGIF_test_unknown":{"value":7}},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"name":"Node"},{"name":"Extra"}]})"},
		{"reference", R"({"asset":{"version":"2.0","extras":{"tag":1}},"extensionsUsed":["MGIF_test_unknown"],"extensions":{"MGIF_test_unknown":{"value":7}},"scene":0,"scenes":[{"nodes":[1]}],"nodes":[{"name":"Node"},{"name":"Extra"}]})"},
		{"value", R"({"asset":{"version":"2.0","extras":{"tag":1}},"extensionsUsed":["MGIF_test_unknown"],"extensions":{"MGIF_test_unknown":{"value":7}},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"name":"Changed"}]})"},
		{"extension", R"({"asset":{"version":"2.0","extras":{"tag":1}},"extensionsUsed":["MGIF_test_unknown"],"extensions":{"MGIF_test_unknown":{"value":8}},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"name":"Node"}]})"},
		{"extras", R"({"asset":{"version":"2.0","extras":{"tag":2}},"extensionsUsed":["MGIF_test_unknown"],"extensions":{"MGIF_test_unknown":{"value":7}},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"name":"Node"}]})"},
	};
	for(const auto& variant : variants)
	{
		const auto path = temporary.path() / (variant.name + ".gltf");
		if(!writeText(path, variant.json)) return false;
		auto changed = load(path);
		passed &= expect(changed != nullptr, variant.name + " difference input loads");
		if(changed)
		{
			const auto comparison = GltfSemanticCompare::compare(*left, *changed);
			passed &= expect(!comparison.equivalent() && !comparison.differences().empty(),
			                 variant.name + " difference is detected");
		}
	}

	const auto payloadA = temporary.path() / "payload-a";
	const auto payloadB = temporary.path() / "payload-b";
	std::filesystem::create_directories(payloadA);
	std::filesystem::create_directories(payloadB);
	const std::string payloadJson = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[]}],"buffers":[{"uri":"payload.bin","byteLength":4}]})";
	writeText(payloadA / "document.gltf", payloadJson);
	writeText(payloadB / "document.gltf", payloadJson);
	writeBytes(payloadA / "payload.bin", {1, 2, 3, 4});
	writeBytes(payloadB / "payload.bin", {1, 2, 3, 5});
	auto bytesA = load(payloadA / "document.gltf");
	auto bytesB = load(payloadB / "document.gltf");
	passed &= expect(bytesA && bytesB, "payload comparison inputs load");
	if(bytesA && bytesB)
	{
		passed &= expect(!GltfSemanticCompare::compare(*bytesA, *bytesB).equivalent(),
		                 "same-size buffer payload difference is detected");
	}
	return passed;
}

bool roundTripFixture(std::string_view fixtureName, std::string_view extension)
{
	TempDirectory temporary;
	const auto source = fixture(fixtureName);
	auto reference = load(source);
	auto document = load(source);
	if(!reference || !document) return false;

	const std::vector<EditorObjectId> ids = captureIds(*document);
	if(document->objectCount(EditorObjectKind::scene) > 1)
	{
		const auto scene = demo::editor::SceneId::from(document->idAt(EditorObjectKind::scene, 1));
		if(!scene || !document->selectCurrentScene(*scene)) return false;
	}
	const auto selectedScene = document->currentSceneId();
	const std::uint64_t initialRevision = document->revision();
	if(!demo::editor::detail::GltfDocumentAccess::advanceRevision(*document)) return false;
	const auto target = temporary.path() / ("roundtrip" + std::string(extension));
	const auto saved = GltfDocumentIO::saveAs(*document, target);
	bool passed = expect(saved.succeeded(), std::string(fixtureName) + " saves as " + std::string(extension));
	if(!saved.succeeded())
	{
		std::cerr << saved.message() << '\n';
		for(const auto& issue : saved.validationReport().issues())
		{
			std::cerr << "  " << issue.path << ": " << issue.message << '\n';
		}
		return false;
	}
	std::error_code pathError;
	passed &= expect(std::filesystem::equivalent(document->sourcePath(), target, pathError) && !pathError,
	                 "successful Save As updates the explicit source path");
	passed &= expect(document->format() == (extension == ".glb"
		? GltfDocumentFormat::binary : GltfDocumentFormat::ascii),
		"successful Save As updates the document format");
	passed &= expect(document->revision() == initialRevision + 1
		&& document->cleanRevision() == document->revision() && !document->dirty(),
		"successful Save As marks the existing revision clean without adding a revision");
	passed &= expect(captureIds(*document) == ids && document->currentSceneId() == selectedScene,
	                 "Save As preserves stable editor IDs and current-scene runtime state");

	auto reloaded = load(target);
	passed &= expect(reloaded != nullptr, "saved document reloads through the bounded candidate path");
	if(reloaded)
	{
		const auto comparison = GltfSemanticCompare::compare(*reference, *reloaded);
		passed &= expect(comparison.equivalent(),
		                 std::string(fixtureName) + " has an empty no-op semantic diff after reload");
		passed &= expect(GltfSemanticCompare::compare(*document, *reloaded).equivalent(),
		                 "published authority matches a fresh reload");
	}
	if(extension == ".glb")
	{
		passed &= expect(document->dependencies().empty(), "GLB Save As embeds all tracked dependencies");
	}
	return passed;
}

bool testCanonicalImageEncodingAndPlaceholderCollision()
{
	TempDirectory temporary;
	std::ifstream input(fixture("external_image.payload"), std::ios::binary);
	const std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(input), {}};
	if(bytes.empty() || !writeBytes(temporary.path() / "payload.png", bytes)) return false;
	const std::string uri = "data:image/png;base64," +
		demo::editor::SceneChangeCanonicalizer::encodeBase64(bytes);
	bool passed = true;
	for(const std::string name : {"ordinary", "__MGIF_CANONICAL_IMAGE_URI_0__"})
	{
		const auto path = temporary.path() / "image.gltf";
		if(!writeText(path, "{\"asset\":{\"version\":\"2.0\"},\"images\":[{\"name\":\"" + name +
			"\",\"uri\":\"payload.png\",\"mimeType\":\"image/png\"}]}")) return false;
		auto document = load(path);
		if(!document) return false;
		const auto canonical = demo::editor::detail::GltfDocumentAccess::canonicalText(*document);
		passed &= expect(canonical.find("\"uri\":\"" + uri + "\"") != std::string::npos &&
			canonical.find("\"name\":\"" + name + "\"") != std::string::npos,
			"canonical image URI bytes and user text survive payload expansion");
		passed &= expect(document->documentDigest() == demo::editor::SceneChangeCanonicalizer::computeSha256(
			std::span{reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()}),
			"document digest still hashes the complete canonical representation");
	}
	return passed;
}

bool testRoundTripsAndPreservation()
{
	bool passed = true;
	for(const std::string_view name : {
		"minimal_scene.gltf",
		"multi_scene_oracle.gltf",
		"unknown_extension_roundtrip.gltf",
		"feature_preservation.gltf",
		"external_buffer.gltf",
		"external_image.gltf",
	})
	{
		passed &= roundTripFixture(name, ".gltf");
		passed &= roundTripFixture(name, ".glb");
	}

	auto unknown = load(fixture("unknown_extension_roundtrip.gltf"));
	passed &= expect(unknown && unknown->documentExtensionsJson().find("MGIF_test_unknown") != std::string_view::npos,
	                 "unknown root extension remains observable in authority JSON");
	if(unknown)
	{
		const EditorObjectId node = unknown->idAt(EditorObjectKind::node, 0);
		passed &= expect(unknown->assetExtrasJson().find("roundtripSentinel") != std::string_view::npos
			&& unknown->objectExtensionsJson(node).find("MGIF_test_unknown") != std::string_view::npos
			&& unknown->objectExtrasJson(node).find("roundtripSentinel") != std::string_view::npos,
			"unknown nested extensions and extras remain in the sole authority");
	}
	return passed;
}

} // namespace

int main()
{
	bool passed = true;
	passed &= testJsonFormattingAndDifferenceCoverage();
	passed &= testCanonicalImageEncodingAndPlaceholderCollision();
	passed &= testRoundTripsAndPreservation();
	return passed ? 0 : 1;
}
