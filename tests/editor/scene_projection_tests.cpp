#include <mgif/scene_editor/GltfDocumentIO.h>
#include <mgif/scene_editor/GltfSemanticCompare.h>
#include <mgif/scene_editor/SceneProjection.h>
#include <mgif/scene_editor/SceneCommands.h>

#include "../../loader/GltfLoader.h"
#include "../../loader/SceneCacheSerializer.h"
#include "../../scene/SceneAssetSerializer.h"
#include "../../scene/SceneAssetView.h"

#include <stb_image_write.h>

#include <chrono>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const char* message)
{
    if(!condition)
    {
        std::cerr << "FAILED: " << message << '\n';
    }
    return condition;
}

std::filesystem::path fixture(const char* name)
{
    return std::filesystem::path(MGIF_TEST_SOURCE_DIR) / "tests" / "fixtures" / "gltf_editor" / name;
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
                / ("mgif-scene-projection-depth-" + std::to_string(seed) + "-" + std::to_string(attempt));
            std::error_code error;
            if(std::filesystem::create_directory(m_path, error))
            {
                return;
            }
        }
        throw std::runtime_error("cannot create scene projection test directory");
    }

    ~TempDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return m_path;
    }

private:
    std::filesystem::path m_path;
};

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
        stream << R"({"name":"DeepNode)" << index << R"(","translation":[1,0,0])";
        if(index + 1u < nodeCount)
        {
            stream << R"(,"children":[)" << (index + 1u) << ']';
        }
        stream << '}';
    }
    stream << "]}";
    if(!stream)
    {
        throw std::runtime_error("cannot write deep scene projection input");
    }
}

bool writeEncodedImageDocument(const std::filesystem::path& path,
                               const char* firstImage,
                               const char* firstMime,
                               const char* secondImage,
                               const char* secondMime)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[]}],"images":[)"
           << R"({"name":"first","uri":")" << firstImage
           << R"(","mimeType":")" << firstMime << R"("},)"
           << R"({"name":"second","uri":")" << secondImage
           << R"(","mimeType":")" << secondMime
           << R"("}],"textures":[{"source":0},{"source":1}]})";
    return static_cast<bool>(stream);
}

bool writeEncodedFixtureImages(const std::filesystem::path& directory)
{
    static constexpr std::array<unsigned char, 16> rgbaPixels{
        255u, 0u, 0u, 255u,
        0u, 255u, 0u, 255u,
        0u, 0u, 255u, 255u,
        255u, 255u, 255u, 128u,
    };
    static constexpr std::array<unsigned char, 18> rgbPixels{
        255u, 0u, 0u,
        0u, 255u, 0u,
        0u, 0u, 255u,
        255u, 255u, 0u,
        0u, 255u, 255u,
        255u, 0u, 255u,
    };

    const std::string pngPath = (directory / "encoded.png").string();
    const std::string jpegPath = (directory / "encoded.jpg").string();
    return stbi_write_png(pngPath.c_str(), 2, 2, 4, rgbaPixels.data(), 2 * 4) != 0
        && stbi_write_jpg(jpegPath.c_str(), 3, 2, 3, rgbPixels.data(), 95) != 0;
}

bool testEncodedImageProjection()
{
    using namespace demo::editor;

    bool passed = true;
    TempDirectory temporary;
    const auto documentPath = temporary.path() / "encoded-images.gltf";
    passed &= expect(writeEncodedFixtureImages(temporary.path()),
                     "encoded PNG/JPEG projection fixtures are written");
    passed &= expect(writeEncodedImageDocument(
                         documentPath, "encoded.png", "image/png",
                         "encoded.jpg", "image/jpeg"),
                     "encoded PNG/JPEG glTF fixture is written");
    if(!passed)
    {
        return false;
    }

    auto load = GltfDocumentIO::loadCandidate(documentPath);
    std::unique_ptr<GltfDocument> document;
    passed &= expect(load.succeeded() && std::move(load).publishTo(document),
                     "encoded PNG/JPEG fixture publishes as document authority");
    if(!document)
    {
        return false;
    }

    const std::string semanticBefore = GltfSemanticCompare::digest(*document);
    const auto projection = SceneProjection::project(*document);
    passed &= expect(projection.succeeded(),
                     "encoded PNG/JPEG document projects to runtime textures");
    if(!projection.succeeded())
    {
        return false;
    }

    bool rgbaSizesValid = projection.asset.textures.size() == 2u;
    bool payloadRangesValid = true;
    std::size_t nonKtxTextureCount = 0;
    for(const demo::SceneTexture& texture : projection.asset.textures)
    {
        if(texture.isKtx2)
        {
            continue;
        }
        ++nonKtxTextureCount;
        const std::uint64_t expectedBytes = static_cast<std::uint64_t>(texture.width)
            * static_cast<std::uint64_t>(texture.height) * 4u;
        rgbaSizesValid = rgbaSizesValid && texture.width > 0u && texture.height > 0u
            && texture.payloadSize == expectedBytes;
        const std::uint64_t payloadBytes = projection.asset.texturePayload.size();
        payloadRangesValid = payloadRangesValid
            && texture.payloadOffset <= payloadBytes
            && texture.payloadSize <= payloadBytes - texture.payloadOffset;
    }
    passed &= expect(nonKtxTextureCount == 2u && rgbaSizesValid,
                     "every projected non-KTX texture has an exact width*height*4 RGBA8 payload");
    passed &= expect(payloadRangesValid
                         && demo::validateSceneAssetView(
                                demo::makeSceneAssetView(projection.asset)).valid,
                     "every projected encoded-image payload range is valid");
    passed &= expect(GltfSemanticCompare::digest(*document) == semanticBefore,
                     "runtime image decode leaves encoded document authority unchanged");
    return passed;
}

bool testMalformedEncodedImageFailsAtomically()
{
    using namespace demo::editor;

    bool passed = true;
    TempDirectory temporary;
    passed &= expect(writeEncodedFixtureImages(temporary.path()),
                     "malformed encoded-image fixture starts from valid images");
    const auto truncatedPath = temporary.path() / "truncated.png";
    std::error_code error;
    std::filesystem::copy_file(temporary.path() / "encoded.png", truncatedPath,
                               std::filesystem::copy_options::overwrite_existing, error);
    passed &= expect(!error, "malformed encoded-image fixture copies");
    if(!error)
    {
        std::filesystem::resize_file(truncatedPath, 33u, error);
    }
    passed &= expect(!error, "malformed encoded-image fixture truncates after its PNG header");

    const auto documentPath = temporary.path() / "malformed-encoded-image.gltf";
    passed &= expect(writeEncodedImageDocument(
                         documentPath, "encoded.png", "image/png",
                         "truncated.png", "image/png"),
                     "malformed encoded-image glTF fixture is written");
    if(!passed)
    {
        return false;
    }

    auto load = GltfDocumentIO::loadCandidate(documentPath);
    std::unique_ptr<GltfDocument> document;
    passed &= expect(load.succeeded() && std::move(load).publishTo(document),
                     "header-valid truncated PNG reaches the runtime decode boundary");
    if(!document)
    {
        return false;
    }

    const std::string semanticBefore = GltfSemanticCompare::digest(*document);
    demo::GltfLoader decoder;
    demo::GltfModel decoded;
    const bool decodedSuccessfully = decoder.decode(*document, decoded);
    passed &= expect(!decodedSuccessfully
                         && decoded.images.empty()
                         && decoded.textures.empty()
                         && decoder.getLastError().find("encoded image[1]")
                                != std::string::npos,
                     "truncated encoded pixels fail without partially publishing a decoded model");

    const auto projection = SceneProjection::project(*document);
    passed &= expect(!projection.succeeded()
                         && projection.asset.textures.empty()
                         && projection.asset.texturePayload.empty(),
                     "truncated encoded pixels fail without partially publishing a SceneAsset");
    passed &= expect(GltfSemanticCompare::digest(*document) == semanticBefore,
                     "failed runtime image decode leaves encoded document authority unchanged");
    return passed;
}

bool testEncodedImageDeclarationMismatchFailsClosed()
{
    using namespace demo::editor;

    bool passed = true;
    TempDirectory temporary;
    passed &= expect(writeEncodedFixtureImages(temporary.path()),
                     "encoded declaration-mismatch fixtures are written");
    std::error_code copyError;
    std::filesystem::copy_file(
        temporary.path() / "encoded.jpg", temporary.path() / "mislabeled.png",
        std::filesystem::copy_options::overwrite_existing, copyError);
    passed &= expect(!copyError,
                     "JPEG payload is copied behind a PNG declaration");
    const auto documentPath = temporary.path() / "encoded-format-mismatch.gltf";
    passed &= expect(writeEncodedImageDocument(
                         documentPath, "encoded.png", "image/png",
                         "mislabeled.png", "image/png"),
                     "encoded declaration-mismatch glTF fixture is written");
    if(!passed)
    {
        return false;
    }

    auto load = GltfDocumentIO::loadCandidate(documentPath);
    std::unique_ptr<GltfDocument> document;
    passed &= expect(load.succeeded() && std::move(load).publishTo(document),
                     "mislabeled encoded image reaches the runtime decode boundary");
    if(!document)
    {
        return false;
    }

    demo::GltfLoader decoder;
    demo::GltfModel decoded;
    passed &= expect(!decoder.decode(*document, decoded)
                         && decoded.images.empty()
                         && decoded.textures.empty()
                         && decoder.getLastError().find("declared encoded format")
                                != std::string::npos,
                     "PNG-declared JPEG bytes reject without stb format fallback or partial publication");
    return passed;
}

bool testDeepProjectionBoundary()
{
    using namespace demo::editor;

    bool passed = true;
    constexpr std::size_t deepNodeCount = 4096;
    TempDirectory temporary;
    const auto deepPath = temporary.path() / "deep-projection.gltf";
    writeDeepHierarchy(deepPath, deepNodeCount);

    GltfLoadOptions options;
    options.limits.maxNodes = deepNodeCount;
    options.limits.maxNodeDepth = deepNodeCount;
    auto load = GltfDocumentIO::loadCandidate(deepPath, options);
    passed &= expect(load.succeeded() && load.candidate() != nullptr,
                     "4096-node projection fixture loads exactly at its configured depth budget");
    if(!load.succeeded() || load.candidate() == nullptr)
    {
        return false;
    }

    std::unique_ptr<GltfDocument> document;
    passed &= expect(std::move(load).publishTo(document),
                     "4096-node projection fixture publishes");
    if(!document)
    {
        return false;
    }

    const EditorObjectId rootId = document->idAt(EditorObjectKind::node, 0);
    const EditorObjectId deepestId = document->idAt(EditorObjectKind::node, deepNodeCount - 1u);
    const std::string semanticBefore = GltfSemanticCompare::digest(*document);
    SceneCommands commands(*document, options.limits);
    const auto mutation = commands.rename(rootId, "DeepRootMutated");
    const std::string semanticAfter = GltfSemanticCompare::digest(*document);
    passed &= expect(mutation.applied() && mutation.beforeRevision == 0u
                         && mutation.afterRevision == 1u && document->revision() == 1u
                         && semanticAfter != semanticBefore,
                     "a real semantic mutation commits on the exact-depth document");

    const auto projection = SceneProjection::project(*document);
    passed &= expect(projection.succeeded() && projection.documentRevision == 1u
                         && projection.semanticHash == semanticAfter,
                     "the mutated 4096-node document projects without native recursion");
    passed &= expect(projection.asset.nodes.size() == deepNodeCount
                         && projection.asset.rootNodes.size() == 1u
                         && projection.asset.rootNodes.front() == 0u
                         && projection.asset.nodes.front().name == "DeepRootMutated",
                     "deep projection retains every node, the selected-scene root, and the committed mutation");

    bool preorderAndParentsValid = projection.asset.nodes.size() == deepNodeCount;
    for(std::size_t index = 0; preorderAndParentsValid && index < deepNodeCount; ++index)
    {
        const auto& node = projection.asset.nodes[index];
        const bool expectedChildren = index + 1u < deepNodeCount
            ? node.children.size() == 1u && node.children.front() == index + 1u
            : node.children.empty();
        preorderAndParentsValid = node.sourceNodeIndex == index
            && node.parent == (index == 0 ? -1 : static_cast<int>(index - 1u))
            && expectedChildren;
    }
    passed &= expect(preorderAndParentsValid,
                     "iterative decode preserves deterministic preorder and parent-child links");
    passed &= expect(projection.asset.nodes.size() == deepNodeCount
                         && projection.asset.nodes.back().worldTransform[3][0]
                             == static_cast<float>(deepNodeCount),
                     "iterative decode preserves accumulated world transforms at depth 4096");

    std::string mappingError;
    passed &= expect(projection.mapping.validate(*document, projection.asset, &mappingError),
                     "deep projection publishes a complete valid runtime mapping");
    const auto rootAssetIndex = projection.mapping.sceneAssetIndex(rootId);
    const auto deepestAssetIndex = projection.mapping.sceneAssetIndex(deepestId);
    const auto deepestRuntimeIndex = projection.mapping.runtimeIndex(deepestId);
    passed &= expect(rootAssetIndex && *rootAssetIndex == 0u
                         && deepestAssetIndex && *deepestAssetIndex == deepNodeCount - 1u
                         && deepestRuntimeIndex
                         && *deepestRuntimeIndex != SceneRuntimeMapping::kInvalidRuntimeIndex
                         && projection.mapping.documentIdAt(
                                EditorObjectKind::node, static_cast<std::uint32_t>(deepNodeCount - 1u))
                             == deepestId
                         && projection.mapping.documentIdForRuntime(*deepestRuntimeIndex) == deepestId,
                     "boundary node IDs round-trip through asset and runtime mappings");
    return passed;
}

bool testSharedMeshOccurrenceAliases()
{
    using namespace demo::editor;

    auto load = GltfDocumentIO::loadCandidate(fixture("r09_shared_mesh.gltf"));
    std::unique_ptr<GltfDocument> document;
    bool passed = expect(load.succeeded() && std::move(load).publishTo(document),
                         "shared-mesh projection fixture publishes");
    if(!document)
    {
        return false;
    }

    const std::uint64_t revisionBefore = document->revision();
    const std::string semanticBefore = GltfSemanticCompare::digest(*document);
    const auto projection = SceneProjection::project(*document);
    const auto repeated = SceneProjection::project(*document);
    passed &= expect(projection.succeeded() && repeated.succeeded(),
                     "multi-primitive shared mesh projects deterministically");
    if(!projection.succeeded() || !repeated.succeeded())
    {
        return false;
    }
    passed &= expect(projection.asset.meshes.size() == 5u
                         && projection.asset.primitives.size() == 5u
                         && document->objectCount(EditorObjectKind::mesh) == 2u
                         && document->objectCount(EditorObjectKind::primitive) == 3u,
                     "renderer-facing occurrences preserve unique document mesh and primitive IDs");
    passed &= expect(projection.mapping.aliases().size() == 5u,
                     "shared fixture publishes every secondary mesh and primitive coordinate");

    std::vector<std::uint32_t> firstMeshCoordinates(
        document->objectCount(EditorObjectKind::mesh),
        SceneRuntimeMapping::kInvalidIndex);
    std::vector<std::uint32_t> firstPrimitiveCoordinates(
        document->objectCount(EditorObjectKind::primitive),
        SceneRuntimeMapping::kInvalidIndex);
    for(std::uint32_t index = 0; index < projection.asset.meshes.size(); ++index)
    {
        const auto& mesh = projection.asset.meshes[index];
        const EditorObjectId expected = document->idAt(
            EditorObjectKind::mesh, mesh.sourceMeshIndex);
        const auto owner = projection.mapping.documentIdAt(EditorObjectKind::mesh, index);
        const auto runtime = projection.mapping.runtimeIndex(expected);
        passed &= expect(owner == expected && runtime
                             && mesh.runtimeIdentity == *runtime,
                         "every projected mesh occurrence reverse-maps with one stable runtime identity");
        firstMeshCoordinates[mesh.sourceMeshIndex] = std::min(
            firstMeshCoordinates[mesh.sourceMeshIndex], index);
    }
    for(std::uint32_t index = 0; index < projection.asset.primitives.size(); ++index)
    {
        const auto& primitive = projection.asset.primitives[index];
        const PrimitiveId expected = document->primitiveIdAt(PrimitiveAddress{
            primitive.documentMeshIndex, primitive.documentPrimitiveIndex})
                                         .value_or(PrimitiveId{});
        const auto owner = projection.mapping.documentIdAt(
            EditorObjectKind::primitive, index);
        const auto runtime = projection.mapping.runtimeIndex(expected.untyped());
        passed &= expect(expected.valid() && owner == expected.untyped() && runtime
                             && primitive.runtimeIdentity == *runtime,
                         "every projected primitive occurrence reverse-maps with one stable runtime identity");
        const auto sourceIndex = document->indexOf(expected.untyped());
        if(sourceIndex)
        {
            firstPrimitiveCoordinates[*sourceIndex] = std::min(
                firstPrimitiveCoordinates[*sourceIndex], index);
        }
    }
    for(std::size_t index = 0; index < firstMeshCoordinates.size(); ++index)
    {
        passed &= expect(projection.mapping.sceneAssetIndex(
                             document->idAt(EditorObjectKind::mesh, index))
                             == firstMeshCoordinates[index],
                         "mesh forward lookup retains the first canonical coordinate");
    }
    for(std::size_t index = 0; index < firstPrimitiveCoordinates.size(); ++index)
    {
        passed &= expect(projection.mapping.sceneAssetIndex(
                             document->idAt(EditorObjectKind::primitive, index))
                             == firstPrimitiveCoordinates[index],
                         "primitive forward lookup retains the first canonical coordinate");
    }
    passed &= expect(projection.mapping == repeated.mapping
                         && projection.mapping.digest() == repeated.mapping.digest()
                         && projection.projectionDigest == repeated.projectionDigest,
                     "repeated occurrence projection has stable mapping equality and digests");
    passed &= expect(projection.mapping.validate(
                         *document, demo::makeSceneAssetView(projection.asset)),
                     "shared occurrence aliases validate through the SceneAssetView boundary");
    passed &= expect(document->revision() == revisionBefore
                         && GltfSemanticCompare::digest(*document) == semanticBefore
                         && projection.semanticHash == semanticBefore,
                     "projection aliases do not mutate document revision or semantic hash");

    std::string error;
    demo::SceneAsset sourceTampered = projection.asset;
    sourceTampered.meshes[1].sourceMeshIndex = 1u;
    passed &= expect(!projection.mapping.validate(*document, sourceTampered, &error),
                     "tampered secondary mesh source identity rejects");
    sourceTampered = projection.asset;
    sourceTampered.primitives[2].documentPrimitiveIndex = 1u;
    passed &= expect(!projection.mapping.validate(*document, sourceTampered, &error),
                     "tampered secondary primitive source identity rejects");
    demo::SceneAsset runtimeTampered = projection.asset;
    runtimeTampered.meshes[1].runtimeIdentity ^= 1u;
    passed &= expect(!projection.mapping.validate(*document, runtimeTampered, &error),
                     "tampered mesh occurrence runtime identity rejects");
    runtimeTampered = projection.asset;
    runtimeTampered.primitives[2].runtimeIdentity ^= 1u;
    passed &= expect(!projection.mapping.validate(*document, runtimeTampered, &error),
                     "tampered primitive occurrence runtime identity rejects");

    auto wrongOwnerAliases = projection.mapping.aliases();
    const EditorObjectId originalOwner = wrongOwnerAliases.front().documentId;
    for(std::size_t index = 0;
        index < document->objectCount(originalOwner.kind); ++index)
    {
        const EditorObjectId candidate = document->idAt(originalOwner.kind, index);
        if(candidate != originalOwner)
        {
            wrongOwnerAliases.front().documentId = candidate;
            break;
        }
    }
    const auto wrongOwnerMapping = SceneRuntimeMapping::create(
        projection.mapping.fence(), projection.mapping.entries(),
        wrongOwnerAliases, &error);
    passed &= expect(wrongOwnerMapping
                         && !wrongOwnerMapping->validate(
                             *document, projection.asset, &error),
                     "tampered alias owner rejects against projected source data");

    auto outOfRangeAliases = projection.mapping.aliases();
    outOfRangeAliases.front().sceneAssetIndex =
        std::numeric_limits<std::uint32_t>::max() - 1u;
    const auto outOfRangeMapping = SceneRuntimeMapping::create(
        projection.mapping.fence(), projection.mapping.entries(),
        outOfRangeAliases, &error);
    passed &= expect(outOfRangeMapping
                         && !outOfRangeMapping->validate(
                             *document, projection.asset, &error),
                     "out-of-range alias rejects during document/asset validation");

    auto staleEntries = projection.mapping.entries();
    auto staleAliases = projection.mapping.aliases();
    const EditorObjectId staleTarget = staleAliases.front().documentId;
    EditorObjectId staleId = staleTarget;
    ++staleId.generation;
    for(auto& entry : staleEntries)
    {
        if(entry.documentId == staleTarget)
        {
            entry.documentId = staleId;
        }
    }
    for(auto& alias : staleAliases)
    {
        if(alias.documentId == staleTarget)
        {
            alias.documentId = staleId;
        }
    }
    const auto staleAliasMapping = SceneRuntimeMapping::create(
        projection.mapping.fence(), staleEntries, staleAliases, &error);
    passed &= expect(staleAliasMapping
                         && !staleAliasMapping->validate(
                             *document, projection.asset, &error),
                     "stale alias and primary identity reject against document authority");
    return passed;
}

bool testDuplicatePrimitiveProjection()
{
    using namespace demo::editor;

    auto load = GltfDocumentIO::loadCandidate(fixture("material_extensions.gltf"));
    std::unique_ptr<GltfDocument> document;
    bool passed = expect(load.succeeded() && std::move(load).publishTo(document),
                         "duplicate-primitive projection fixture publishes");
    if(!document)
    {
        return false;
    }
    const std::string semanticBefore = GltfSemanticCompare::digest(*document);
    const std::uint64_t revisionBefore = document->revision();
    const std::size_t primitiveCountBefore =
        document->objectCount(EditorObjectKind::primitive);
    SceneCommands commands(*document);
    const PrimitiveId source = PrimitiveId::from(
        document->idAt(EditorObjectKind::primitive, 0u)).value_or(PrimitiveId{});
    const auto duplicate = commands.duplicatePrimitive(source);
    const std::string semanticAfter = GltfSemanticCompare::digest(*document);
    passed &= expect(duplicate.applied()
                         && document->revision() == revisionBefore + 1u
                         && document->objectCount(EditorObjectKind::primitive)
                             == primitiveCountBefore + 1u
                         && semanticAfter != semanticBefore,
                     "duplicatePrimitive publishes one semantic revision and stable document ID");

    const auto projection = SceneProjection::project(*document);
    passed &= expect(projection.succeeded()
                         && projection.asset.meshes.size() == 2u
                         && projection.asset.primitives.size() == 2u,
                     "duplicatePrimitive projects both renderer-facing draws");
    if(!projection.succeeded())
    {
        return false;
    }
    const EditorObjectId meshId = document->idAt(EditorObjectKind::mesh, 0u);
    passed &= expect(projection.mapping.sceneAssetIndex(meshId) == 0u
                         && projection.mapping.documentIdAt(
                             EditorObjectKind::mesh, 0u) == meshId
                         && projection.mapping.documentIdAt(
                             EditorObjectKind::mesh, 1u) == meshId
                         && projection.asset.meshes[0].runtimeIdentity
                             == projection.asset.meshes[1].runtimeIdentity,
                     "duplicatePrimitive preserves one mesh identity across both coordinates");
    passed &= expect(projection.mapping.validate(*document, projection.asset)
                         && projection.semanticHash == semanticAfter
                         && projection.documentRevision == document->revision()
                         && GltfSemanticCompare::digest(*document) == semanticAfter,
                     "duplicatePrimitive projection preserves semantic hash and document revision");
    return passed;
}

bool testDuplicateCameraLightProjectionAliases()
{
    using namespace demo::editor;

    auto load = GltfDocumentIO::loadCandidate(fixture("feature_preservation.gltf"));
    std::unique_ptr<GltfDocument> document;
    bool passed = expect(load.succeeded() && std::move(load).publishTo(document),
                         "camera/light duplicate projection fixture publishes");
    if(!document)
    {
        return false;
    }

    const NodeId source = NodeId::from(
        document->idAt(EditorObjectKind::node, 1u)).value_or(NodeId{});
    SceneCommands commands(*document);
    const auto duplicate = commands.duplicateNode(source);
    passed &= expect(duplicate.applied()
                         && document->objectCount(EditorObjectKind::camera) == 1u
                         && document->objectCount(EditorObjectKind::light) == 1u,
                     "duplicating an owning node reuses document camera/light records");

    const auto projection = SceneProjection::project(*document);
    passed &= expect(projection.succeeded()
                         && projection.asset.cameras.size() == 2u
                         && projection.asset.lights.size() == 2u,
                     "duplicate camera/light node projects both renderer occurrences");
    if(!projection.succeeded() || projection.asset.cameras.size() != 2u
       || projection.asset.lights.size() != 2u)
    {
        return false;
    }

    const EditorObjectId cameraId = document->idAt(EditorObjectKind::camera, 0u);
    const EditorObjectId lightId = document->idAt(EditorObjectKind::light, 0u);
    passed &= expect(projection.mapping.documentIdAt(EditorObjectKind::camera, 0u) == cameraId
                         && projection.mapping.documentIdAt(EditorObjectKind::camera, 1u) == cameraId
                         && projection.mapping.documentIdAt(EditorObjectKind::light, 0u) == lightId
                         && projection.mapping.documentIdAt(EditorObjectKind::light, 1u) == lightId
                         && projection.asset.cameras[0].runtimeIdentity
                             == projection.asset.cameras[1].runtimeIdentity
                         && projection.asset.lights[0].runtimeIdentity
                             == projection.asset.lights[1].runtimeIdentity
                         && projection.ownerCapability.validate(
                             projection.asset, projection.mapping),
                     "camera/light primary and alias coordinates retain one sealed owner");

    std::string error;
    demo::SceneAsset sourceTampered = projection.asset;
    ++sourceTampered.cameras[1].sourceCameraIndex;
    passed &= expect(!projection.ownerCapability.validate(
                         sourceTampered, projection.mapping, &error),
                     "tampered camera alias source identity rejects against owner seal");
    demo::SceneAsset runtimeTampered = projection.asset;
    runtimeTampered.lights[1].runtimeIdentity ^= 1u;
    passed &= expect(!projection.ownerCapability.validate(
                         runtimeTampered, projection.mapping, &error),
                     "tampered light alias runtime identity rejects against owner seal");
    return passed;
}

bool testRuntimeAnimationPoseProjection()
{
    using namespace demo::editor;
    auto load = GltfDocumentIO::loadCandidate(fixture("feature_preservation.gltf"));
    std::unique_ptr<GltfDocument> document;
    bool passed = expect(load.succeeded() && std::move(load).publishTo(document),
                         "animation pose fixture publishes");
    if(!document)
        return false;

    const auto baseline = SceneProjection::project(*document);
    const std::uint64_t revision = document->revision();
    const std::string digest = GltfSemanticCompare::digest(*document);
    SceneProjectionOptions options;
    options.activeAnimation = 0u;
    options.animationTime = 0.5;
    options.animationLoop = false;
    options.animationPose = SceneAnimationPose{
        0u, 0.5, false,
        {AnimationTransformSample{
            0u, std::array<double, 3>{0.5, 0.5, 0.5}, std::nullopt,
            std::nullopt}}};
    const auto animated = SceneProjection::project(*document, options);
    passed &= expect(baseline.succeeded() && animated.succeeded(),
                     "runtime animation pose projection succeeds");
    if(animated.succeeded() && !animated.asset.nodes.empty())
    {
        const auto& translation = animated.asset.nodes.front().translation;
        passed &= expect(std::abs(translation.x - 0.5F) < 1.0e-5F
                             && std::abs(translation.y - 0.5F) < 1.0e-5F
                             && std::abs(translation.z - 0.5F) < 1.0e-5F,
                         "runtime animation pose updates projected node TRS");
        passed &= expect(animated.projectionDigest != baseline.projectionDigest
                             && animated.documentDigest == baseline.documentDigest
                             && animated.documentRevision == revision
                             && GltfSemanticCompare::digest(*document) == digest,
                         "runtime animation changes projection identity without document mutation");
        passed &= expect(animated.activeAnimation == std::optional<std::uint32_t>{0u}
                             && animated.animationTime == 0.5
                             && !animated.animationLoop,
                         "runtime animation state is carried by the immutable projection");
    }

    SceneProjectionOptions invalid = options;
    invalid.animationPose->transforms.front().nodeIndex = 999u;
    const auto rejected = SceneProjection::project(*document, invalid);
    passed &= expect(!rejected.succeeded()
                         && document->revision() == revision
                         && GltfSemanticCompare::digest(*document) == digest,
                     "invalid runtime animation target fails closed atomically");
    return passed;
}

bool testSceneAssetTextureSerialization()
{
    bool passed = true;
    TempDirectory temporary;
    const auto sourcePath = fixture("feature_preservation.gltf");
    const auto assetPath = temporary.path() / "texture-identity.sceneasset";

    demo::SceneAsset source;
    source.name = "texture identity";
    source.sourcePath = sourcePath.generic_string();
    source.textures.resize(1);
    source.textures.front().width = 1u;
    source.textures.front().height = 1u;
    source.textures.front().payloadOffset = 0u;
    source.textures.front().payloadSize = 4u;
    source.textures.front().uri = "embedded-image";
    source.textures.front().sourceImageIndex = 37u;
    source.texturePayload = {1u, 2u, 3u, 4u};
    source.nodes.resize(1u);
    demo::SceneCamera camera;
    camera.nodeIndex = 0;
    camera.sourceCameraIndex = 17u;
    camera.runtimeIdentity = 0x123456789abcdef0ull;
    source.cameras.push_back(camera);
    demo::SceneLight light;
    light.nodeIndex = 0;
    light.sourceLightIndex = 23u;
    light.runtimeIdentity = 0x0fedcba987654321ull;
    source.lights.push_back(light);

    demo::SceneAssetSerializer serializer;
    passed &= expect(serializer.save(assetPath, source, sourcePath),
                     "SceneAsset texture identity fixture serializes");
    demo::SceneAsset reloaded;
    passed &= expect(serializer.load(assetPath, reloaded),
                     "SceneAsset texture identity fixture deserializes");
    passed &= expect(reloaded.textures.size() == 1u
                         && reloaded.textures.front().sourceImageIndex == 37u
                         && reloaded.textures.front().uri == "embedded-image"
                         && reloaded.cameras.size() == 1u
                         && reloaded.cameras.front().sourceCameraIndex == 17u
                         && reloaded.cameras.front().runtimeIdentity
                             == 0x123456789abcdef0ull
                         && reloaded.lights.size() == 1u
                         && reloaded.lights.front().sourceLightIndex == 23u
                         && reloaded.lights.front().runtimeIdentity
                             == 0x0fedcba987654321ull
                         && reloaded.texturePayload == source.texturePayload,
                     "SceneAsset round-trip preserves texture, camera, and light source identities");

    const auto oldVersionPath = temporary.path() / "old-version.sceneasset";
    std::error_code error;
    std::filesystem::copy_file(assetPath, oldVersionPath,
                               std::filesystem::copy_options::overwrite_existing, error);
    passed &= expect(!error, "SceneAsset old-version fixture copies");
    if(!error)
    {
        std::fstream stream(oldVersionPath, std::ios::binary | std::ios::in | std::ios::out);
        std::uint32_t oldVersion = demo::SceneAssetSerializer::kCurrentVersion - 1u;
        stream.seekp(8, std::ios::beg);
        stream.write(reinterpret_cast<const char*>(&oldVersion), sizeof(oldVersion));
        stream.close();
        demo::SceneAsset ignored;
        passed &= expect(!serializer.load(oldVersionPath, ignored),
                         "SceneAsset loader deterministically rejects the previous format version");
    }

    const auto truncatedPath = temporary.path() / "truncated.sceneasset";
    std::filesystem::copy_file(assetPath, truncatedPath,
                               std::filesystem::copy_options::overwrite_existing, error);
    passed &= expect(!error, "SceneAsset truncated fixture copies");
    if(!error)
    {
        const auto size = std::filesystem::file_size(truncatedPath);
        std::filesystem::resize_file(truncatedPath, size - 1u, error);
        passed &= expect(!error, "SceneAsset truncated fixture is shortened");
        demo::SceneAsset ignored;
        passed &= expect(!serializer.load(truncatedPath, ignored),
                         "SceneAsset loader rejects a truncated texture section");
    }
    return passed;
}

bool testGltfTextureSamplerCacheSerialization()
{
    bool passed = true;
    TempDirectory temporary;
    const auto sourcePath = fixture("feature_preservation.gltf");
    const auto cachePath = temporary.path() / "texture-sampler.gltfcache";

    demo::GltfModel source;
    source.name = "typed texture model";
    source.sourcePath = sourcePath.generic_string();
    source.sourceDirectory = sourcePath.parent_path().generic_string();

    demo::GltfImageData image;
    image.width = 1;
    image.height = 1;
    image.channels = 4;
    image.pixels = {10u, 20u, 30u, 40u};
    image.uri = "image-zero.png";
    image.mimeType = "image/png";
    image.isSrgb = false;
    image.documentImageIndex = 0;
    source.images.push_back(image);

    demo::GltfImageData duplicate;
    duplicate.width = 1;
    duplicate.height = 1;
    duplicate.channels = 4;
    duplicate.ktx2Data = {0xABu, 0xCDu};
    duplicate.uri = "image-one.ktx2";
    duplicate.mimeType = "image/ktx2";
    duplicate.isKtx2 = true;
    duplicate.isSrgb = true;
    duplicate.documentImageIndex = 0;
    source.images.push_back(duplicate);

    demo::GltfTextureData texture;
    texture.sourceImageIndex = 0;
    texture.resolvedImageIndex = 0;
    texture.canonicalImageIndex = 1;
    texture.colorImageIndex = 1;
    texture.linearImageIndex = 0;
    texture.samplerIndex = 0;
    texture.usage = 3u;
    texture.name = "semantic texture";
    texture.extensionsJson = R"({"MGIF_texture": {"sentinel": 17}})";
    texture.extrasJson = R"({"textureExtra": true})";
    source.textures.push_back(texture);

    demo::GltfSamplerData sampler;
    sampler.magFilter = 9729;
    sampler.minFilter = 9987;
    sampler.wrapS = 33071;
    sampler.wrapT = 33648;
    sampler.name = "semantic sampler";
    sampler.extensionsJson = R"({"MGIF_sampler": {"sentinel": 23}})";
    sampler.extrasJson = R"({"samplerExtra": "preserve"})";
    source.samplers.push_back(sampler);

    demo::SceneCacheSerializer serializer;
    passed &= expect(serializer.saveCache(cachePath, source, sourcePath),
                     "typed texture/sampler cache fixture serializes");
    demo::GltfModel reloaded;
    passed &= expect(serializer.loadCache(cachePath, reloaded),
                     "typed texture/sampler cache fixture deserializes");
    if(reloaded.images.size() == 2u && reloaded.textures.size() == 1u
       && reloaded.samplers.size() == 1u)
    {
        const auto& loadedImage = reloaded.images[1];
        const auto& loadedTexture = reloaded.textures.front();
        const auto& loadedSampler = reloaded.samplers.front();
        passed &= expect(loadedImage.documentImageIndex == duplicate.documentImageIndex
                             && loadedImage.isSrgb == duplicate.isSrgb
                             && loadedImage.ktx2Data == duplicate.ktx2Data,
                         "cache round-trip preserves image identity and color-space payload");
        passed &= expect(loadedTexture.sourceImageIndex == texture.sourceImageIndex
                             && loadedTexture.resolvedImageIndex == texture.resolvedImageIndex
                             && loadedTexture.canonicalImageIndex == texture.canonicalImageIndex
                             && loadedTexture.colorImageIndex == texture.colorImageIndex
                             && loadedTexture.linearImageIndex == texture.linearImageIndex
                             && loadedTexture.samplerIndex == texture.samplerIndex
                             && loadedTexture.usage == texture.usage
                             && loadedTexture.name == texture.name
                             && loadedTexture.extensionsJson == texture.extensionsJson
                             && loadedTexture.extrasJson == texture.extrasJson,
                         "cache round-trip preserves every typed texture identity field");
        passed &= expect(loadedSampler.magFilter == sampler.magFilter
                             && loadedSampler.minFilter == sampler.minFilter
                             && loadedSampler.wrapS == sampler.wrapS
                             && loadedSampler.wrapT == sampler.wrapT
                             && loadedSampler.name == sampler.name
                             && loadedSampler.extensionsJson == sampler.extensionsJson
                             && loadedSampler.extrasJson == sampler.extrasJson,
                         "cache round-trip preserves every typed sampler field");
    }
    else
    {
        passed &= expect(false, "typed texture/sampler cache vectors retain their counts");
    }

    passed &= expect(serializer.isCacheValid(cachePath, sourcePath),
                     "typed texture/sampler cache validates against its source");

    std::error_code error;
    const auto oldVersionPath = temporary.path() / "old-version.gltfcache";
    std::filesystem::copy_file(cachePath, oldVersionPath,
                               std::filesystem::copy_options::overwrite_existing, error);
    passed &= expect(!error, "cache old-version fixture copies");
    if(!error)
    {
        std::fstream stream(oldVersionPath, std::ios::binary | std::ios::in | std::ios::out);
        std::uint32_t oldVersion = demo::SceneCacheSerializer::kCurrentVersion - 1u;
        stream.seekp(8, std::ios::beg);
        stream.write(reinterpret_cast<const char*>(&oldVersion), sizeof(oldVersion));
        stream.close();
        demo::GltfModel ignored;
        passed &= expect(!serializer.loadCache(oldVersionPath, ignored)
                             && !serializer.isCacheValid(oldVersionPath, sourcePath),
                         "cache loader and validator reject the previous format version");
    }

    const auto truncatedPath = temporary.path() / "truncated.gltfcache";
    std::filesystem::copy_file(cachePath, truncatedPath,
                               std::filesystem::copy_options::overwrite_existing, error);
    passed &= expect(!error, "cache truncated fixture copies");
    if(!error)
    {
        const auto size = std::filesystem::file_size(truncatedPath);
        std::filesystem::resize_file(truncatedPath, size - 1u, error);
        passed &= expect(!error, "cache truncated fixture is shortened");
        demo::GltfModel ignored;
        passed &= expect(!serializer.loadCache(truncatedPath, ignored)
                             && !serializer.isCacheValid(truncatedPath, sourcePath),
                         "cache loader and validator reject a truncated payload");
    }

    const auto trailingPath = temporary.path() / "trailing.gltfcache";
    std::filesystem::copy_file(cachePath, trailingPath,
                               std::filesystem::copy_options::overwrite_existing, error);
    passed &= expect(!error, "cache trailing-byte fixture copies");
    if(!error)
    {
        std::ofstream stream(trailingPath, std::ios::binary | std::ios::app);
        stream.put(static_cast<char>(0xA5));
        stream.close();
        demo::GltfModel ignored;
        passed &= expect(!serializer.loadCache(trailingPath, ignored)
                             && !serializer.isCacheValid(trailingPath, sourcePath),
                         "cache loader and validator reject trailing corruption");
    }
    return passed;
}

} // namespace

int main()
{
    bool passed = true;
    passed &= testEncodedImageProjection();
    passed &= testMalformedEncodedImageFailsAtomically();
    passed &= testEncodedImageDeclarationMismatchFailsClosed();
    passed &= testDeepProjectionBoundary();
    passed &= testSharedMeshOccurrenceAliases();
    passed &= testDuplicatePrimitiveProjection();
    passed &= testDuplicateCameraLightProjectionAliases();
    passed &= testRuntimeAnimationPoseProjection();
    passed &= testSceneAssetTextureSerialization();
    passed &= testGltfTextureSamplerCacheSerialization();
    passed &= expect(demo::SceneCacheSerializer::kCurrentVersion == 14,
                     "SceneCacheSerializer format version includes texture/sampler identity");
    passed &= expect(demo::SceneAssetSerializer::kCurrentVersion == 12,
                     "SceneAssetSerializer format version includes camera/light source identity");
    const auto load = demo::editor::GltfDocumentIO::loadCandidate(fixture("feature_preservation.gltf"));
    passed &= expect(load.succeeded() && load.candidate() != nullptr,
                     "feature preservation fixture loads through the P1 document authority");
    if(!load.succeeded() || load.candidate() == nullptr)
    {
        return EXIT_FAILURE;
    }

    const demo::editor::GltfDocument& document = *load.candidate();
    const std::string semanticBefore = demo::editor::GltfSemanticCompare::digest(document);
    const auto projection = demo::editor::SceneProjection::project(document);
    passed &= expect(projection.succeeded(), "document projects into a SceneAsset");
    passed &= expect(!projection.asset.nodes.empty(), "projection contains nodes");
    passed &= expect(projection.asset.primitives.size() == 1, "projection retains the primitive");
    passed &= expect(projection.asset.animations.size() == 1, "projection retains animation metadata");
    passed &= expect(projection.asset.materialVariantNames.empty(), "variant list is empty when source has none");
    passed &= expect(projection.mapping.validate(document, projection.asset), "runtime mapping invariants hold");
    passed &= expect(projection.asset.cameras.size() == 1u
                         && projection.asset.lights.size() == 1u,
                     "feature projection retains one camera and one light source record");
    if(projection.asset.cameras.size() == 1u && projection.asset.lights.size() == 1u)
    {
        const auto cameraOwner = projection.mapping.documentIdAt(
            demo::editor::EditorObjectKind::camera, 0u);
        const auto lightOwner = projection.mapping.documentIdAt(
            demo::editor::EditorObjectKind::light, 0u);
        const auto cameraSource = cameraOwner ? document.indexOf(*cameraOwner) : std::nullopt;
        const auto lightSource = lightOwner ? document.indexOf(*lightOwner) : std::nullopt;
        passed &= expect(cameraOwner && cameraSource
                             && projection.asset.cameras.front().sourceCameraIndex
                                 == *cameraSource
                             && projection.asset.cameras.front().runtimeIdentity
                                 == demo::editor::SceneRuntimeMapping::deriveProjectionIdentity(
                                     *cameraOwner),
                         "camera asset carries its projection-owned stable source identity");
        passed &= expect(lightOwner && lightSource
                             && projection.asset.lights.front().sourceLightIndex
                                 == *lightSource
                             && projection.asset.lights.front().runtimeIdentity
                                 == demo::editor::SceneRuntimeMapping::deriveProjectionIdentity(
                                     *lightOwner),
                         "light asset carries its projection-owned stable source identity");

        std::string identityError;
        demo::SceneAsset cameraTampered = projection.asset;
        cameraTampered.cameras.front().runtimeIdentity ^= 1u;
        passed &= expect(!projection.mapping.validate(
                             document, cameraTampered, &identityError),
                         "tampered camera runtime identity rejects against document authority");
        cameraTampered = projection.asset;
        ++cameraTampered.cameras.front().sourceCameraIndex;
        passed &= expect(!projection.mapping.validate(
                             document, cameraTampered, &identityError),
                         "tampered camera source index rejects against document authority");
        demo::SceneAsset lightTampered = projection.asset;
        lightTampered.lights.front().runtimeIdentity ^= 1u;
        passed &= expect(!projection.mapping.validate(
                             document, lightTampered, &identityError),
                         "tampered light runtime identity rejects against document authority");
        lightTampered = projection.asset;
        ++lightTampered.lights.front().sourceLightIndex;
        passed &= expect(!projection.mapping.validate(
                             document, lightTampered, &identityError),
                         "tampered light source index rejects against document authority");
    }
    passed &= expect(projection.semanticHash == semanticBefore, "projection preserves document semantic hash");
    passed &= expect(projection.asset.documentSemanticHash == semanticBefore,
                     "SceneAsset records the source semantic hash");

    const auto multiLoad = demo::editor::GltfDocumentIO::loadCandidate(fixture("multi_scene_oracle.gltf"));
    passed &= expect(multiLoad.succeeded() && multiLoad.candidate() != nullptr,
                     "multi-scene fixture loads through the document authority");
    if(multiLoad.succeeded() && multiLoad.candidate() != nullptr)
    {
        const auto multiProjection = demo::editor::SceneProjection::project(*multiLoad.candidate());
        passed &= expect(multiProjection.succeeded(), "multi-scene document projects");
        passed &= expect(multiProjection.asset.nodes.size() == 5,
                         "current scene projection follows the selected scene roots");
    }

    auto rootTransformLoad = demo::editor::GltfDocumentIO::loadCandidate(fixture("multi_scene_oracle.gltf"));
    passed &= expect(rootTransformLoad.succeeded() && rootTransformLoad.candidate() != nullptr,
                     "scene-root transform fixture loads");
    if(rootTransformLoad.succeeded() && rootTransformLoad.candidate() != nullptr)
    {
        std::unique_ptr<demo::editor::GltfDocument> mutableDocument;
        passed &= expect(std::move(rootTransformLoad).publishTo(mutableDocument),
                         "scene-root transform fixture publishes");
        if(mutableDocument)
        {
            demo::editor::SceneCommands commands(*mutableDocument);
            const demo::editor::SceneId scene = mutableDocument->currentSceneId();
            demo::editor::SceneRootTransform transform;
            transform.translation = {4.0, 0.0, 0.0};
            const auto preview = commands.previewSceneRootTransform(scene, transform);
            passed &= expect(preview.applied()
                             && preview.historyPolicy == demo::editor::HistoryPolicy::nonHistory
                             && mutableDocument->revision() == 0u,
                             "scene-root projection preview is non-history");
            const auto transformed = demo::editor::SceneProjection::project(*mutableDocument);
            passed &= expect(transformed.succeeded() && transformed.asset.sceneRootTransform.translation.x == 4.0f,
                             "projection exposes scene-root translation");
            const auto repeated = commands.previewSceneRootTransform(scene, transform);
            passed &= expect(repeated.domainNoop() && mutableDocument->revision() == 0u,
                             "repeated scene-root projection preview is idempotent");
            const auto reset = commands.resetSceneRootTransform(scene);
            passed &= expect(reset.applied() && mutableDocument->sceneRootTransform(scene).translation[0] == 0.0,
                             "scene-root projection reset restores identity");
        }
    }

    auto matrixLoad = demo::editor::GltfDocumentIO::loadCandidate(fixture("scene_root_matrix.gltf"));
    passed &= expect(matrixLoad.succeeded() && matrixLoad.candidate() != nullptr,
                     "matrix-authored multi-root fixture loads");
    if(matrixLoad.succeeded() && matrixLoad.candidate() != nullptr)
    {
        std::unique_ptr<demo::editor::GltfDocument> matrixDocument;
        passed &= expect(std::move(matrixLoad).publishTo(matrixDocument),
                         "matrix-authored fixture publishes");
        if(matrixDocument)
        {
            const auto matrixBefore = demo::editor::GltfSemanticCompare::digest(*matrixDocument);
            demo::editor::SceneCommands commands(*matrixDocument);
            demo::editor::SceneRootTransform transform;
            transform.translation = {1.0, 0.0, 0.0};
            const auto preview = commands.previewSceneRootTransform(matrixDocument->currentSceneId(), transform);
            passed &= expect(preview.applied(), "matrix-authored root preview applies");
            const auto projected = demo::editor::SceneProjection::project(*matrixDocument);
            passed &= expect(projected.succeeded() && projected.asset.rootNodes.size() == 2u,
                             "multi-root projection preserves both roots");
            passed &= expect(demo::editor::GltfSemanticCompare::digest(*matrixDocument) == matrixBefore,
                             "scene-root preview preserves matrix-authority semantic digest");
        }
    }

    const auto oldEntryProjection = projection;
    passed &= expect(demo::editor::SceneProjection::digest(projection.asset, projection.mapping)
                         == demo::editor::SceneProjection::digest(oldEntryProjection.asset,
                                                                   oldEntryProjection.mapping),
                     "repeated projection has a stable digest");

    const std::filesystem::path cachePath =
        std::filesystem::temp_directory_path() / "mgif_scene_projection_test.sceneasset";
    demo::SceneAssetSerializer serializer;
    passed &= expect(serializer.save(cachePath, projection.asset, fixture("feature_preservation.gltf")),
                     "projected SceneAsset serializes");
    demo::SceneAsset reloaded;
    passed &= expect(serializer.load(cachePath, reloaded), "projected SceneAsset deserializes");
    passed &= expect(reloaded.documentSemanticHash == semanticBefore,
                     "serialized SceneAsset preserves document semantic hash");
    passed &= expect(demo::validateSceneAssetView(demo::makeSceneAssetView(reloaded)).valid,
                     "reloaded SceneAsset validates");

    const std::filesystem::path authorityOnlyPath =
        std::filesystem::temp_directory_path() / "mgif_scene_projection_authority_only.gltf";
    std::error_code copyError;
    std::filesystem::copy_file(fixture("feature_preservation.gltf"), authorityOnlyPath,
                               std::filesystem::copy_options::overwrite_existing, copyError);
    passed &= expect(!copyError, "authority regression fixture copies");
    if(!copyError)
    {
        const auto authorityLoad = demo::editor::GltfDocumentIO::loadCandidate(authorityOnlyPath);
        passed &= expect(authorityLoad.succeeded() && authorityLoad.candidate() != nullptr,
                         "authority regression fixture loads");
        std::filesystem::remove(authorityOnlyPath, copyError);
        if(authorityLoad.succeeded() && authorityLoad.candidate() != nullptr)
        {
            demo::GltfLoader authorityLoader;
            demo::GltfModel decoded;
            passed &= expect(authorityLoader.decode(*authorityLoad.candidate(), decoded),
                             "GltfLoader decodes the in-memory document after source removal");
        }
    }
    std::error_code removeError;
    std::filesystem::remove(cachePath, removeError);

    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
