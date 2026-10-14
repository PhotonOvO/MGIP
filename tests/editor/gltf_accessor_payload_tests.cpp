#include <mgif/scene_editor/GltfDocumentIO.h>
#include <mgif/scene_editor/GltfValidator.h>

#include <tiny_gltf.h>

#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace demo::editor::detail {

class GltfDocumentAccess final
{
public:
	static void injectNonFinitePosition(GltfDocument& document)
	{
		auto& bytes = document.mutableAuthorityModel().buffers.at(0).data;
		const std::uint32_t nanBits = 0x7fc00000u;
		for(std::size_t byte = 0; byte < sizeof(nanBits); ++byte)
		{
			bytes.at(byte) = static_cast<unsigned char>((nanBits >> (byte * 8)) & 0xffu);
		}
		(void)document.advanceRevision();
	}
};

} // namespace demo::editor::detail

namespace {

using namespace demo::editor;

std::size_t g_checks = 0;

bool expect(bool condition, std::string_view message)
{
	++g_checks;
	if(!condition) std::cerr << "FAILED: " << message << '\n';
	return condition;
}

class TempDirectory final
{
public:
	TempDirectory()
	{
		const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
		m_path = std::filesystem::temp_directory_path()
			/ ("mgif-accessor-payload-tests-" + std::to_string(ticks));
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
	return std::filesystem::path(MGIF_TEST_SOURCE_DIR) / "tests" / "fixtures" / "gltf_editor"
		/ "payload_validation" / name / (std::string(name) + ".gltf");
}

[[nodiscard]] std::filesystem::path editorFixture(std::string_view name)
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

void appendUnsigned(std::vector<unsigned char>& bytes, std::uint64_t value, std::size_t width)
{
	for(std::size_t byte = 0; byte < width; ++byte)
	{
		bytes.push_back(static_cast<unsigned char>((value >> (byte * 8)) & 0xffu));
	}
}

void appendFloat(std::vector<unsigned char>& bytes, float value)
{
	appendUnsigned(bytes, std::bit_cast<std::uint32_t>(value), sizeof(float));
}

void storeUnsigned(std::vector<unsigned char>& bytes, std::size_t offset, std::uint64_t value, std::size_t width)
{
	for(std::size_t byte = 0; byte < width; ++byte)
	{
		bytes.at(offset + byte) = static_cast<unsigned char>((value >> (byte * 8)) & 0xffu);
	}
}

void storeFloat(std::vector<unsigned char>& bytes, std::size_t offset, float value)
{
	storeUnsigned(bytes, offset, std::bit_cast<std::uint32_t>(value), sizeof(float));
}

[[nodiscard]] std::filesystem::path writeGeneratedFixture(
	const TempDirectory& temporary,
	std::string_view name,
	std::string json,
	const std::vector<unsigned char>& payload)
{
	const std::filesystem::path jsonPath = temporary.path() / (std::string(name) + ".gltf");
	const std::filesystem::path payloadPath = temporary.path() / (std::string(name) + ".bin");
	const std::string token = "PAYLOAD_URI";
	const std::size_t position = json.find(token);
	if(position != std::string::npos) json.replace(position, token.size(), payloadPath.filename().string());
	if(!writeText(jsonPath, json) || !writeBytes(payloadPath, payload)) return {};
	return jsonPath;
}

bool expectInvalid(
	const std::filesystem::path& path,
	GltfValidationErrorCode expectedCode,
	std::string_view name,
	const GltfLoadOptions& options = {})
{
	auto result = GltfDocumentIO::loadCandidate(path, options);
	if(!expect(!result.succeeded() && result.error() == GltfLoadError::validationFailed,
	           std::string(name) + " is rejected by document validation")) return false;
	return expect(!result.validationReport().issues().empty()
		&& result.validationReport().issues().front().code == expectedCode,
		std::string(name) + " reports the expected typed validation issue");
}

bool expectValid(
	const std::filesystem::path& path,
	std::string_view name,
	const GltfLoadOptions& options = {})
{
	auto result = GltfDocumentIO::loadCandidate(path, options);
	return expect(result.succeeded() && result.validationReport().valid()
		&& result.validationReport().issues().empty(),
		std::string(name) + " remains valid");
}

bool testAdversarialMatrix()
{
	const struct InvalidCase final
	{
		std::string_view name;
		GltfValidationErrorCode code;
	} invalidCases[] = {
		{"primitive_index_oob", GltfValidationErrorCode::invalidPrimitiveIndex},
		{"primitive_topology_count_invalid", GltfValidationErrorCode::invalidPrimitiveTopology},
		{"primitive_restart_sentinel", GltfValidationErrorCode::invalidPrimitiveIndex},
		{"position_payload_nonfinite", GltfValidationErrorCode::invalidAccessorPayload},
		{"position_bounds_mismatch", GltfValidationErrorCode::accessorBoundsMismatch},
		{"attribute_count_mismatch", GltfValidationErrorCode::attributeCountMismatch},
		{"sparse_index_oob", GltfValidationErrorCode::invalidSparseIndex},
		{"sparse_indices_unsorted", GltfValidationErrorCode::invalidSparseIndex},
		{"sparse_indices_duplicate", GltfValidationErrorCode::invalidSparseIndex},
		{"sparse_values_nonfinite", GltfValidationErrorCode::invalidAccessorPayload},
		{"animation_input_nonmonotonic", GltfValidationErrorCode::invalidAnimationInput},
		{"animation_input_duplicate", GltfValidationErrorCode::invalidAnimationInput},
		{"animation_input_negative", GltfValidationErrorCode::invalidAnimationInput},
		{"animation_input_nonfinite", GltfValidationErrorCode::invalidAccessorPayload},
		{"animation_output_nonfinite", GltfValidationErrorCode::invalidAccessorPayload},
		{"joints_value_oob", GltfValidationErrorCode::invalidJointIndex},
		{"weights_negative", GltfValidationErrorCode::invalidWeight},
		{"weights_u8_sum_invalid", GltfValidationErrorCode::invalidWeight},
	};
	bool passed = true;
	for(const auto& testCase : invalidCases)
	{
		passed &= expectInvalid(fixture(testCase.name), testCase.code, testCase.name);
	}
	for(const std::string_view name : {
		"control_valid_triangle",
		"control_valid_sparse",
		"control_valid_animation",
		"control_valid_skin",
		"control_valid_quantized_weights",
		"weights_float_not_normalized_advisory",
	})
	{
		passed &= expectValid(fixture(name), name);
	}
	passed &= expectValid(editorFixture("feature_preservation.gltf"),
	                     "dense accessor with a sparse overlay");
	return passed;
}

[[nodiscard]] std::string triangleJson(std::size_t byteLength, std::size_t indexOffset,
	std::size_t indexLength, int componentType)
{
	std::ostringstream json;
	json << R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],)"
		 << R"("buffers":[{"uri":"PAYLOAD_URI","byteLength":)" << byteLength << R"(}],)"
		 << R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":52,"byteStride":16,"target":34962},)"
		 << R"({"buffer":0,"byteOffset":)" << indexOffset << R"(,"byteLength":)" << indexLength
		 << R"(,"target":34963}],)"
		 << R"("accessors":[{"bufferView":0,"byteOffset":4,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},)"
		 << R"({"bufferView":1,"componentType":)" << componentType << R"(,"count":3,"type":"SCALAR"}],)"
		 << R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}],"nodes":[{"mesh":0}]})";
	return json.str();
}

[[nodiscard]] std::vector<unsigned char> trianglePayload(
	std::size_t indexWidth,
	std::uint64_t thirdIndex)
{
	std::vector<unsigned char> payload(4, 0);
	for(const auto& position : {
		std::array<float, 3>{0.0f, 0.0f, 0.0f},
		std::array<float, 3>{1.0f, 0.0f, 0.0f},
		std::array<float, 3>{0.0f, 1.0f, 0.0f},
	})
	{
		for(float component : position) appendFloat(payload, component);
		payload.insert(payload.end(), 4, 0);
	}
	appendUnsigned(payload, 0, indexWidth);
	appendUnsigned(payload, 1, indexWidth);
	appendUnsigned(payload, thirdIndex, indexWidth);
	return payload;
}

bool testDenseOffsetsStridesAndIndexWidths()
{
	TempDirectory temporary;
	bool passed = true;
	const struct WidthCase final
	{
		std::string_view name;
		int componentType;
		std::size_t width;
		std::uint64_t maximum;
	} cases[] = {
		{"u8", 5121, 1, std::numeric_limits<std::uint8_t>::max()},
		{"u16", 5123, 2, std::numeric_limits<std::uint16_t>::max()},
		{"u32", 5125, 4, std::numeric_limits<std::uint32_t>::max()},
	};
	for(const auto& testCase : cases)
	{
		auto validPayload = trianglePayload(testCase.width, 2);
		const auto validPath = writeGeneratedFixture(
			temporary,
			std::string("valid-strided-") + std::string(testCase.name),
			triangleJson(validPayload.size(), 52, testCase.width * 3, testCase.componentType),
			validPayload);
		passed &= expectValid(validPath, std::string("valid strided ") + std::string(testCase.name));

		auto domainOverflowPayload = trianglePayload(testCase.width, 3);
		const auto domainOverflowPath = writeGeneratedFixture(
			temporary,
			std::string("domain-overflow-") + std::string(testCase.name),
			triangleJson(domainOverflowPayload.size(), 52, testCase.width * 3, testCase.componentType),
			domainOverflowPayload);
		passed &= expectInvalid(domainOverflowPath,
		                        GltfValidationErrorCode::invalidPrimitiveIndex,
		                        std::string("index at exclusive vertex boundary ") + std::string(testCase.name));

		auto sentinelPayload = trianglePayload(testCase.width, testCase.maximum);
		const auto sentinelPath = writeGeneratedFixture(
			temporary,
			std::string("sentinel-") + std::string(testCase.name),
			triangleJson(sentinelPayload.size(), 52, testCase.width * 3, testCase.componentType),
			sentinelPayload);
		passed &= expectInvalid(sentinelPath,
		                        GltfValidationErrorCode::invalidPrimitiveIndex,
		                        std::string("restart sentinel ") + std::string(testCase.name));
	}
	return passed;
}

[[nodiscard]] std::string sparseJson(std::size_t indexBytes, int indexComponentType)
{
	std::ostringstream json;
	json << R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],)"
		 << R"("buffers":[{"uri":"PAYLOAD_URI","byteLength":24}],)"
		 << R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":)" << (4 + indexBytes) << R"(},)"
		 << R"({"buffer":0,"byteOffset":8,"byteLength":16}],)"
		 << R"("accessors":[{"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,0,0],)"
		 << R"("sparse":{"count":1,"indices":{"bufferView":0,"byteOffset":4,"componentType":)"
		 << indexComponentType << R"(},"values":{"bufferView":1,"byteOffset":4}}}],)"
		 << R"("meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],"nodes":[{"mesh":0}]})";
	return json.str();
}

bool testSparseWidthsOffsetsAndSubstitution()
{
	TempDirectory temporary;
	bool passed = true;
	for(const auto& testCase : {
		std::pair{5121, std::size_t{1}},
		std::pair{5123, std::size_t{2}},
		std::pair{5125, std::size_t{4}},
	})
	{
		std::vector<unsigned char> payload(24, 0);
		storeUnsigned(payload, 4, 2, testCase.second);
		storeFloat(payload, 12, 1.0f);
		const std::string name = "valid-sparse-" + std::to_string(testCase.first);
		const auto path = writeGeneratedFixture(
			temporary, name, sparseJson(testCase.second, testCase.first), payload);
		passed &= expectValid(path, name);
	}
	return passed;
}

bool testAllComponentWidthsAndMatrixPadding()
{
	TempDirectory temporary;
	std::vector<unsigned char> payload(28, 0);
	payload[0] = 0x80;
	payload[1] = 0xff;
	storeUnsigned(payload, 2, 0x8000, 2);
	storeUnsigned(payload, 4, 0xffff, 2);
	storeUnsigned(payload, 8, 0xffffffffu, 4);
	storeFloat(payload, 12, -1.5f);
	for(std::size_t component = 0; component < 9; ++component)
	{
		const std::size_t column = component / 3;
		const std::size_t row = component % 3;
		payload[16 + column * 4 + row] = static_cast<unsigned char>(component + 1);
	}
	const std::string json = R"({
"asset":{"version":"2.0"},
"buffers":[{"uri":"PAYLOAD_URI","byteLength":28}],
"bufferViews":[
{"buffer":0,"byteOffset":0,"byteLength":1},{"buffer":0,"byteOffset":1,"byteLength":1},
{"buffer":0,"byteOffset":2,"byteLength":2},{"buffer":0,"byteOffset":4,"byteLength":2},
{"buffer":0,"byteOffset":8,"byteLength":4},{"buffer":0,"byteOffset":12,"byteLength":4},
{"buffer":0,"byteOffset":16,"byteLength":12}],
"accessors":[
{"bufferView":0,"componentType":5120,"count":1,"type":"SCALAR","min":[-128],"max":[-128]},
{"bufferView":1,"componentType":5121,"count":1,"type":"SCALAR","min":[255],"max":[255]},
{"bufferView":2,"componentType":5122,"count":1,"type":"SCALAR","min":[-32768],"max":[-32768]},
{"bufferView":3,"componentType":5123,"count":1,"type":"SCALAR","min":[65535],"max":[65535]},
{"bufferView":4,"componentType":5125,"count":1,"type":"SCALAR","min":[4294967295],"max":[4294967295]},
{"bufferView":5,"componentType":5126,"count":1,"type":"SCALAR","min":[-1.5],"max":[-1.5]},
{"bufferView":6,"componentType":5121,"count":1,"type":"MAT3",
 "min":[1,2,3,4,5,6,7,8,9],"max":[1,2,3,4,5,6,7,8,9]}]})";
	const auto path = writeGeneratedFixture(temporary, "component-boundaries", json, payload);
	return expectValid(path, "all legal component widths and MAT3 column padding");
}

[[nodiscard]] std::string accessorOnlyJson(
	std::size_t byteLength,
	std::size_t viewLength,
	std::string_view accessorMembers)
{
	std::ostringstream json;
	json << R"({"asset":{"version":"2.0"},"buffers":[{"uri":"PAYLOAD_URI","byteLength":)"
		 << byteLength << R"(}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":)"
		 << viewLength << R"(}],"accessors":[{"bufferView":0,)" << accessorMembers << "}] }";
	return json.str();
}

bool testMalformedRangesAndWorkBudget()
{
	TempDirectory temporary;
	bool passed = true;
	const auto misaligned = writeGeneratedFixture(
		temporary,
		"misaligned",
		accessorOnlyJson(3, 3, "\"byteOffset\":1,\"componentType\":5123,\"count\":1,\"type\":\"SCALAR\""),
		std::vector<unsigned char>(3, 0));
	passed &= expectInvalid(misaligned, GltfValidationErrorCode::invalidAccessor, "misaligned accessor offset");

	const auto truncated = writeGeneratedFixture(
		temporary,
		"truncated",
		accessorOnlyJson(4, 4, "\"componentType\":5125,\"count\":2,\"type\":\"SCALAR\""),
		std::vector<unsigned char>(4, 0));
	passed &= expectInvalid(truncated, GltfValidationErrorCode::invalidAccessor, "truncated accessor range");

	const auto overflow = writeGeneratedFixture(
		temporary,
		"overflow",
		accessorOnlyJson(1, 1,
			"\"byteOffset\":18446744073709551615,\"componentType\":5121,\"count\":2,\"type\":\"SCALAR\""),
		std::vector<unsigned char>(1, 0));
	passed &= expectInvalid(overflow, GltfValidationErrorCode::invalidAccessor, "overflowing accessor range");

	const std::string workJson = R"({"asset":{"version":"2.0"},
"buffers":[{"uri":"PAYLOAD_URI","byteLength":4}],
"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":4}],
"accessors":[
{"bufferView":0,"componentType":5126,"count":1,"type":"SCALAR"},
{"bufferView":0,"componentType":5126,"count":1,"type":"SCALAR"}]})";
	const auto work = writeGeneratedFixture(temporary, "work-budget", workJson, std::vector<unsigned char>(4, 0));
	GltfLoadOptions options;
	options.limits.maxAuthorityPayloadBytes = 7;
	passed &= expectInvalid(work, GltfValidationErrorCode::limitExceeded, "aggregate accessor decode work", options);
	options.limits.maxAuthorityPayloadBytes = 8;
	passed &= expectValid(work, "accessor decode work at the exact configured boundary", options);

	auto morphPayload = trianglePayload(1, 2);
	const std::string morphJson = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],
"buffers":[{"uri":"PAYLOAD_URI","byteLength":55}],
"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":52,"byteStride":16,"target":34962},
{"buffer":0,"byteOffset":52,"byteLength":3,"target":34963}],
"accessors":[
{"bufferView":0,"byteOffset":4,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},
{"bufferView":1,"componentType":5121,"count":3,"type":"SCALAR"},
{"bufferView":0,"byteOffset":4,"componentType":5126,"count":2,"type":"VEC3"}],
"meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1,"targets":[{"POSITION":2}]}]}],
"nodes":[{"mesh":0}]})";
	const auto morph = writeGeneratedFixture(temporary, "morph-count", morphJson, morphPayload);
	passed &= expectInvalid(morph,
	                        GltfValidationErrorCode::attributeCountMismatch,
	                        "morph target accessor count mismatch");
	return passed;
}

bool testPreSavePayloadValidation()
{
	auto load = GltfDocumentIO::loadCandidate(fixture("control_valid_triangle"));
	std::unique_ptr<GltfDocument> document;
	if(!expect(load.succeeded() && std::move(load).publishTo(document), "valid control publishes before save test"))
	{
		return false;
	}
	demo::editor::detail::GltfDocumentAccess::injectNonFinitePosition(*document);
	const GltfValidationReport direct = GltfValidator::validate(*document);
	bool passed = expect(!direct.valid() && !direct.issues().empty()
		&& direct.issues().front().code == GltfValidationErrorCode::invalidAccessorPayload,
		"public document validation reads the mutated accessor payload");

	TempDirectory temporary;
	const auto target = temporary.path() / "existing.gltf";
	passed &= expect(writeText(target, "ORIGINAL"), "save test target is created");
	const GltfSaveResult save = GltfDocumentIO::saveAs(*document, target);
	std::ifstream stream(target, std::ios::binary);
	const std::string bytes{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
	passed &= expect(!save.succeeded() && save.error() == GltfSaveError::invalidDocument
		&& !save.validationReport().issues().empty()
		&& save.validationReport().issues().front().code == GltfValidationErrorCode::invalidAccessorPayload
		&& bytes == "ORIGINAL",
		"pre-Save validation rejects payload corruption and preserves the target");
	return passed;
}

} // namespace

int main()
{
	bool passed = true;
	passed &= testAdversarialMatrix();
	passed &= testDenseOffsetsStridesAndIndexWidths();
	passed &= testSparseWidthsOffsetsAndSubstitution();
	passed &= testAllComponentWidthsAndMatrixPadding();
	passed &= testMalformedRangesAndWorkBudget();
	passed &= testPreSavePayloadValidation();
	if(passed)
	{
		std::cout << "Accessor payload validation passed " << g_checks
			<< " checks, including 18 normative-invalid cases and all valid/advisory controls.\n";
	}
	return passed ? 0 : 1;
}
