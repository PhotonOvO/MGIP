#include <mgif/scene_editor/GltfDocumentIO.h>
#include <mgif/scene_editor/GltfSemanticCompare.h>
#include <mgif/scene_editor/SceneCommands.h>
#include <mgif/scene_editor/SceneProjection.h>

#include "support/EditorTestSupport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace demo::editor;

bool expect(bool condition, const char* message)
{
	if(!condition) std::cerr << "FAILED: " << message << '\n';
	return condition;
}

std::filesystem::path fixture(const char* name)
{
	return std::filesystem::path(MGIF_TEST_SOURCE_DIR)
		/ "tests" / "fixtures" / "gltf_editor" / name;
}

std::unique_ptr<GltfDocument> loadDocument(const std::filesystem::path& path)
{
	auto load = GltfDocumentIO::loadCandidate(path);
	std::unique_ptr<GltfDocument> document;
	if(!load.succeeded())
	{
		std::cerr << "LOAD FAILED: " << path.string() << ": " << load.message() << '\n';
		for(const auto& issue : load.validationReport().issues())
			std::cerr << "  " << issue.path << ": " << issue.message << '\n';
		return {};
	}
	if(!std::move(load).publishTo(document)) return {};
	return document;
}

struct Snapshot final
{
	std::uint64_t revision{0};
	std::string digest;
	std::array<std::vector<EditorObjectId>,
		static_cast<std::size_t>(EditorObjectKind::count)> ids;

	bool operator==(const Snapshot&) const = default;
};

Snapshot snapshot(const GltfDocument& document)
{
	Snapshot result;
	result.revision = document.revision();
	result.digest = GltfSemanticCompare::digest(document);
	for(std::size_t kind = 1; kind < static_cast<std::size_t>(EditorObjectKind::count); ++kind)
	{
		const auto type = static_cast<EditorObjectKind>(kind);
		result.ids[kind].reserve(document.objectCount(type));
		for(std::size_t index = 0; index < document.objectCount(type); ++index)
			result.ids[kind].push_back(document.idAt(type, index));
	}
	return result;
}

std::optional<EditorObjectId> addedId(
	const std::vector<MutationRemapEntry>& entries, EditorObjectKind kind)
{
	for(const auto& entry : entries)
	{
		if(!entry.oldId.valid() && entry.newId.valid() && entry.newId.kind == kind)
			return entry.newId;
	}
	return std::nullopt;
}

std::vector<EditorObjectId> addedIds(
	const std::vector<MutationRemapEntry>& entries, EditorObjectKind kind)
{
	std::vector<EditorObjectId> result;
	for(const auto& entry : entries)
	{
		if(!entry.oldId.valid() && entry.newId.valid() && entry.newId.kind == kind)
			result.push_back(entry.newId);
	}
	return result;
}

std::vector<std::string> nodeNames(
	const GltfDocument& document, const std::vector<NodeId>& nodes)
{
	std::vector<std::string> result;
	result.reserve(nodes.size());
	for(const NodeId node : nodes) result.emplace_back(document.objectName(node.untyped()));
	return result;
}

bool completeMapping(const GltfDocument& document, const SceneProjectionResult& projection)
{
	std::size_t expected = 0;
	for(std::size_t kind = 1; kind < static_cast<std::size_t>(EditorObjectKind::count); ++kind)
	{
		const auto type = static_cast<EditorObjectKind>(kind);
		expected += document.objectCount(type);
		for(std::size_t index = 0; index < document.objectCount(type); ++index)
		{
			const EditorObjectId id = document.idAt(type, index);
			const auto entry = projection.mapping.entry(id);
			if(!entry) return false;
			if(entry->sceneAssetIndex != SceneRuntimeMapping::kInvalidIndex)
			{
				if(projection.mapping.documentIdAt(type, entry->sceneAssetIndex) != id
					|| projection.mapping.runtimeIndex(id) != entry->runtimeIndex
					|| projection.mapping.documentIdForRuntime(entry->runtimeIndex) != id)
					return false;
			}
			else if(projection.mapping.runtimeIndex(id))
			{
				return false;
			}
		}
	}
	return projection.mapping.entries().size() == expected
		&& projection.mapping.validate(document, projection.asset);
}

bool matrixNear(const glm::mat4& left, const glm::mat4& right)
{
	for(std::size_t column = 0; column < 4; ++column)
		for(std::size_t row = 0; row < 4; ++row)
			if(std::abs(left[column][row] - right[column][row]) > 1.0e-5f) return false;
	return true;
}

std::filesystem::path writeVariantFixture()
{
	const auto path = std::filesystem::temp_directory_path() / "mgif_p3_variants.gltf";
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	output << R"json({
  "asset":{"version":"2.0"},
  "extensionsUsed":["KHR_materials_variants"],
  "extensions":{"KHR_materials_variants":{"variants":[{"name":"A"},{"name":"B"}]}},
  "scene":0,
  "scenes":[{"nodes":[0]}],
  "nodes":[{"mesh":0}],
  "meshes":[{"primitives":[{
    "attributes":{"POSITION":0},"indices":1,"material":0,
    "extensions":{"KHR_materials_variants":{"mappings":[{"material":0,"variants":[0]}]}}
  }]}],
  "materials":[{"name":"M0"},{"name":"M1"}],
  "buffers":[{"byteLength":160,"uri":"data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIAAAAAAIA/AAAAAAAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAAAAAAIA/AAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAQAAAQEABAAAAAAAAQAAAQEAAAIBATUdJRg=="}],
  "bufferViews":[
    {"buffer":0,"byteOffset":0,"byteLength":36,"target":34962},
    {"buffer":0,"byteOffset":36,"byteLength":6,"target":34963}
  ],
  "accessors":[
    {"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},
    {"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}
  ]
})json";
	return path;
}

bool testAtomicityAndTokens()
{
	bool passed = true;
	auto document = loadDocument(fixture("feature_preservation.gltf"));
	passed &= expect(document != nullptr, "atomicity fixture loads");
	if(!document) return false;
	testing::ScriptedEditorFaultInjector faults;
	SceneCommands commands(*document, kDefaultEditorLimits, &faults);
	SceneCommands otherOwner(*document);
	passed &= expect(commands.token().valid() && otherOwner.token().valid()
		&& commands.token().serial != otherOwner.token().serial,
		"command owners receive distinct transaction nonces");
	const EditorObjectId node = document->idAt(EditorObjectKind::node, 0);
	MutationGuard wrongOwner;
	wrongOwner.expectedToken = commands.token();
	const Snapshot wrongOwnerBefore = snapshot(*document);
	const auto wrongOwnerResult = otherOwner.rename(node, "wrong-owner", wrongOwner);
	passed &= expect(wrongOwnerResult.error == MutationErrorCode::staleTransaction
		&& snapshot(*document) == wrongOwnerBefore,
		"cross-owner token is rejected without observable change");

	for(const std::size_t allocationCall : {1u, 2u})
	{
		faults.reset();
		passed &= expect(faults.failOnCall(EditorFaultPoint::allocation, allocationCall),
			"allocation fault schedule is accepted");
		const Snapshot before = snapshot(*document);
		const auto result = commands.rename(node, "allocation-failure");
		passed &= expect(result.rejected() && result.error == MutationErrorCode::allocationFailed
			&& snapshot(*document) == before,
			allocationCall == 1u
				? "candidate allocation failure is atomic"
				: "pre-publication result allocation failure is atomic");
	}
	faults.reset();
	MutationGuard accepted;
	const MutationToken oldToken = commands.token();
	accepted.expectedToken = oldToken;
	const auto applied = commands.rename(node, "token-advanced", accepted);
	passed &= expect(applied.applied() && applied.transactionToken.valid()
		&& applied.transactionToken.serial != oldToken.serial,
		"successful mutation advances the transaction nonce");
	const Snapshot staleBefore = snapshot(*document);
	const auto stale = commands.rename(node, "stale-token", accepted);
	passed &= expect(stale.error == MutationErrorCode::staleTransaction
		&& snapshot(*document) == staleBefore,
		"consumed token cannot be replayed");
	return passed;
}

bool testVisibilityRoundTrip()
{
	bool passed = true;
	auto document = loadDocument(fixture("feature_preservation.gltf"));
	if(!document) return expect(false, "visibility fixture loads");
	SceneCommands commands(*document);
	const NodeId node = *NodeId::from(document->idAt(EditorObjectKind::node, 0));
	const auto hidden = commands.setVisibility(node.untyped(), false);
	passed &= expect(hidden.applied() && hidden.afterRevision == hidden.beforeRevision + 1u
		&& document->objectHasVisibility(node.untyped()) && !document->objectVisible(node.untyped())
		&& document->dirty(), "visibility authors KHR_node_visibility and advances revision");
	const auto hiddenProjection = SceneProjection::project(*document);
	const auto projectedNode = std::find_if(hiddenProjection.asset.nodes.begin(),
		hiddenProjection.asset.nodes.end(), [](const demo::SceneNode& candidate)
		{ return candidate.sourceNodeIndex == 0u; });
	passed &= expect(hiddenProjection && projectedNode != hiddenProjection.asset.nodes.end()
		&& !projectedNode->localVisible && !projectedNode->effectiveVisible,
		"authored visibility is authoritative in the next projection");
	const Snapshot hiddenSnapshot = snapshot(*document);
	const auto repeated = commands.setVisibility(node.untyped(), false);
	passed &= expect(repeated.domainNoop() && snapshot(*document) == hiddenSnapshot,
		"repeated authored visibility is an exact no-op");
	const auto path = std::filesystem::temp_directory_path() / "mgif_p3_visibility.gltf";
	passed &= expect(GltfDocumentIO::saveAs(*document, path).succeeded(),
		"authored visibility saves");
	auto reloaded = loadDocument(path);
	passed &= expect(reloaded != nullptr, "authored visibility reloads");
	if(reloaded)
	{
		const NodeId reloadedNode = *NodeId::from(reloaded->idAt(EditorObjectKind::node, 0));
		passed &= expect(reloaded->objectHasVisibility(reloadedNode.untyped())
			&& !reloaded->objectVisible(reloadedNode.untyped()),
			"KHR_node_visibility round-trips through save/reload");
		SceneCommands reloadCommands(*reloaded);
		const auto removed = reloadCommands.removeVisibility(reloadedNode);
		passed &= expect(removed.applied() && !reloaded->objectHasVisibility(reloadedNode.untyped())
			&& reloaded->objectVisible(reloadedNode.untyped()),
			"removing visibility restores the authored default");
		const Snapshot removedSnapshot = snapshot(*reloaded);
		passed &= expect(reloadCommands.removeVisibility(reloadedNode).domainNoop()
			&& snapshot(*reloaded) == removedSnapshot,
			"repeated visibility removal is an exact no-op");
	}
	std::error_code error;
	std::filesystem::remove(path, error);
	return passed;
}

bool testRemapAndProperties()
{
	bool passed = true;
	auto document = loadDocument(fixture("feature_preservation.gltf"));
	if(!document) return expect(false, "remap fixture loads");
	SceneCommands commands(*document);
	const NodeId source = *NodeId::from(document->idAt(EditorObjectKind::node, 0));
	const std::size_t skinCount = document->objectCount(EditorObjectKind::skin);
	const auto duplicate = commands.duplicateNode(source);
	const auto duplicateId = addedId(duplicate.remap.nodes, EditorObjectKind::node);
	passed &= expect(duplicate.applied() && duplicateId
		&& document->objectCount(EditorObjectKind::skin) == skinCount + 1u
		&& addedId(duplicate.remap.skins, EditorObjectKind::skin),
		"node duplicate clones skin identity and remaps the cloned node");
	auto projection = SceneProjection::project(*document);
	passed &= expect(projection && projection.asset.animations.size() == 1u
		&& projection.asset.animations[0].channels.size() == 2u,
		"node duplicate appends remapped animation channels");
	if(duplicateId)
	{
		const NodeId duplicateNode = *NodeId::from(*duplicateId);
		passed &= expect(commands.deleteNode(duplicateNode).applied(),
			"duplicated node can be deleted");
		projection = SceneProjection::project(*document);
		passed &= expect(projection && projection.asset.animations.size() == 1u
			&& projection.asset.animations[0].channels.size() == 1u,
			"node delete removes only channels targeting the deleted node");
	}
	auto deleteAnimationDocument = loadDocument(fixture("feature_preservation.gltf"));
	SceneCommands deleteAnimationCommands(*deleteAnimationDocument);
	const NodeId animatedNode = *NodeId::from(
		deleteAnimationDocument->idAt(EditorObjectKind::node, 0));
	passed &= expect(deleteAnimationCommands.deleteNode(animatedNode).applied()
		&& deleteAnimationDocument->objectCount(EditorObjectKind::animation) == 0u,
		"deleting the final targeted node removes the empty animation and stable ID");
	auto deleteJointDocument = loadDocument(fixture("feature_preservation.gltf"));
	SceneCommands deleteJointCommands(*deleteJointDocument);
	const NodeId joint = *NodeId::from(deleteJointDocument->idAt(EditorObjectKind::node, 2));
	passed &= expect(deleteJointCommands.deleteNode(joint).applied()
		&& deleteJointDocument->objectCount(EditorObjectKind::skin) == 0u,
		"deleting the final skin joint removes the empty skin and stable ID");

	const MaterialId material = *MaterialId::from(document->idAt(EditorObjectKind::material, 0));
	const TextureId texture = *TextureId::from(document->idAt(EditorObjectKind::texture, 0));
	MaterialTextureProperties textureEdit;
	textureEdit.texture = texture;
	textureEdit.texCoord = 1;
	const auto textureResult = commands.setMaterialTexture(
		material, MaterialTextureSlot::baseColor, textureEdit);
	passed &= expect(textureResult.applied()
		&& commands.materialTexture(material, MaterialTextureSlot::baseColor) == textureEdit,
		"material texture slot edits and queries exact texture/texCoord state");
	MaterialExtensionValue factor;
	factor.value = 0.75;
	MaterialExtensionValue enabled;
	enabled.value = true;
	MaterialExtensionValue extension;
	extension.value = MaterialExtensionValue::Object{
		{"factor", factor}, {"enabled", enabled}};
	passed &= expect(commands.setMaterialExtension(
		material, "MGIF_test_edit", extension).applied()
		&& commands.materialExtension(material, "MGIF_test_edit") == extension,
		"material extension value tree round-trips through edit/query");
	passed &= expect(commands.setMaterialExtension(
		material, "MGIF_test_edit", std::nullopt).applied()
		&& !commands.materialExtension(material, "MGIF_test_edit"),
		"material extension removal is observable");

	const SceneId scene = document->currentSceneId();
	const std::size_t lightCount = document->objectCount(EditorObjectKind::light);
	LightProperties light;
	light.type = "spot";
	light.intensity = 4.0;
	light.range = 8.0;
	light.innerConeAngle = 0.2;
	light.outerConeAngle = 0.6;
	const auto created = commands.createLight(scene, std::nullopt,
		"Created Light Node", "Created Light", light);
	const auto createdLight = addedId(created.remap.lights, EditorObjectKind::light);
	passed &= expect(created.applied() && createdLight
		&& document->objectCount(EditorObjectKind::light) == lightCount + 1u,
		"light creation authors both node and light objects");
	if(createdLight)
	{
		const LightId id = *LightId::from(*createdLight);
		passed &= expect(commands.lightProperties(id) == light,
			"created light properties query exactly");
		const Snapshot lightSnapshot = snapshot(*document);
		passed &= expect(commands.setLight(id, light).domainNoop()
			&& snapshot(*document) == lightSnapshot,
			"repeated light edit is an exact no-op");
		passed &= expect(commands.deleteLight(id).applied() && !document->contains(id.untyped())
			&& document->objectCount(EditorObjectKind::light) == lightCount,
			"light deletion remaps the light collection and detaches references");
	}
	projection = SceneProjection::project(*document);
	passed &= expect(projection && completeMapping(*document, projection),
		"projection after remap/property mutations covers every typed ID");
	for(std::size_t kind = 1; kind < static_cast<std::size_t>(EditorObjectKind::count); ++kind)
	{
		const auto type = static_cast<EditorObjectKind>(kind);
		for(std::size_t index = 0; index < document->objectCount(type); ++index)
		{
			const auto query = commands.query(document->idAt(type, index));
			passed &= expect(query.domainNoop() && query.error == MutationErrorCode::none,
				"every live typed ID supports a bounded no-effect query");
		}
	}
	return passed;
}

bool testSplitMergeWorkflow()
{
	bool passed = true;
	auto document = loadDocument(fixture("r09_shared_mesh.gltf"));
	if(!document) return expect(false, "R09 shared-mesh fixture loads");
	SceneCommands commands(*document);
	const NodeId selected = *NodeId::from(document->idAt(EditorObjectKind::node, 0));
	const NodeId peer = *NodeId::from(document->idAt(EditorObjectKind::node, 1));
	const NodeId noMaterialNode = *NodeId::from(
		document->idAt(EditorObjectKind::node, 2));
	const MeshId sharedMesh = *MeshId::from(document->idAt(EditorObjectKind::mesh, 0));
	const MeshId noMaterialMesh = *MeshId::from(
		document->idAt(EditorObjectKind::mesh, 1));
	const MaterialId firstMaterial = *MaterialId::from(
		document->idAt(EditorObjectKind::material, 0));
	const MaterialId secondMaterial = *MaterialId::from(
		document->idAt(EditorObjectKind::material, 1));
	const auto originalPrimitives = commands.meshPrimitives(sharedMesh);
	passed &= expect(originalPrimitives.size() == 2u
		&& commands.nodeMesh(selected) == sharedMesh
		&& commands.nodeMesh(peer) == sharedMesh
		&& commands.meshReferenceCount(sharedMesh) == 2u,
		"R09 fixture starts with two nodes sharing an ordered two-primitive mesh");

	const Snapshot beforeSplit = snapshot(*document);
	const auto split = commands.splitPrimitive(selected, originalPrimitives.front());
	const auto copiedMeshValue = addedId(split.remap.meshes, EditorObjectKind::mesh);
	const auto copiedMaterialValue = addedId(
		split.remap.materials, EditorObjectKind::material);
	const auto copiedPrimitiveValues = addedIds(
		split.remap.primitives, EditorObjectKind::primitive);
	passed &= expect(split.applied() && split.beforeRevision == beforeSplit.revision
		&& split.afterRevision == beforeSplit.revision + 1u && copiedMeshValue
		&& copiedMaterialValue && copiedPrimitiveValues.size() == 2u,
		"Split publishes one revision and fresh mesh, material, and primitive identities");
	if(!copiedMeshValue || !copiedMaterialValue || copiedPrimitiveValues.size() != 2u)
		return false;
	const MeshId copiedMesh = *MeshId::from(*copiedMeshValue);
	const MaterialId copiedMaterial = *MaterialId::from(*copiedMaterialValue);
	const PrimitiveId copiedFirst = *PrimitiveId::from(copiedPrimitiveValues[0]);
	const PrimitiveId copiedSecond = *PrimitiveId::from(copiedPrimitiveValues[1]);
	const auto copiedPrimitives = commands.meshPrimitives(copiedMesh);
	passed &= expect(copiedPrimitives == std::vector<PrimitiveId>{copiedFirst, copiedSecond}
		&& commands.nodeMesh(selected) == copiedMesh
		&& commands.nodeMesh(peer) == sharedMesh
		&& commands.meshReferenceCount(sharedMesh) == 1u
		&& commands.meshReferenceCount(copiedMesh) == 1u,
		"Split redirects only the selected node and preserves primitive order");
	passed &= expect(document->contains(originalPrimitives[0].untyped())
		&& document->contains(originalPrimitives[1].untyped())
		&& document->primitiveAddress(originalPrimitives[0]) == PrimitiveAddress{0u, 0u}
		&& document->primitiveAddress(originalPrimitives[1]) == PrimitiveAddress{0u, 1u}
		&& commands.primitiveMaterial(originalPrimitives[0]) == firstMaterial
		&& commands.primitiveMaterial(originalPrimitives[1]) == secondMaterial
		&& commands.primitiveMaterial(copiedFirst) == copiedMaterial
		&& commands.primitiveMaterial(copiedSecond) == secondMaterial,
		"Split preserves original IDs and changes only the target copied primitive material");
	passed &= expect(commands.materialReferenceCount(copiedMaterial) == 1u,
		"Split material copy has exactly one primitive reference");

	const auto originalProperties = commands.materialProperties(firstMaterial);
	auto copiedProperties = commands.materialProperties(copiedMaterial);
	if(copiedProperties)
	{
		copiedProperties->baseColorFactor = {0.2, 0.3, 0.4, 1.0};
		const auto edited = commands.setMaterial(copiedMaterial, *copiedProperties);
		const auto currentOriginal = commands.materialProperties(firstMaterial);
		const auto currentCopy = commands.materialProperties(copiedMaterial);
		passed &= expect(edited.applied() && originalProperties && currentOriginal
			&& currentCopy
			&& currentOriginal->baseColorFactor == originalProperties->baseColorFactor
			&& currentCopy->baseColorFactor == copiedProperties->baseColorFactor
			&& currentCopy->baseColorFactor != currentOriginal->baseColorFactor,
			"editing the split material leaves the shared source material unchanged");
	}
	else
	{
		passed &= expect(false, "copied material properties are queryable");
	}

	const auto splitPath = std::filesystem::temp_directory_path()
		/ "mgif_p3_r09_split.gltf";
	passed &= expect(GltfDocumentIO::saveAs(*document, splitPath).succeeded(),
		"split document saves");
	auto splitReload = loadDocument(splitPath);
	if(splitReload)
	{
		SceneCommands reloadCommands(*splitReload);
		const NodeId reloadSelected = *NodeId::from(
			splitReload->idAt(EditorObjectKind::node, 0));
		const NodeId reloadPeer = *NodeId::from(
			splitReload->idAt(EditorObjectKind::node, 1));
		passed &= expect(splitReload->objectCount(EditorObjectKind::mesh) == 3u
			&& splitReload->objectCount(EditorObjectKind::material) == 3u
			&& splitReload->objectCount(EditorObjectKind::primitive) == 5u
			&& reloadCommands.nodeMesh(reloadSelected)
			&& reloadCommands.nodeMesh(reloadPeer)
			&& reloadCommands.nodeMesh(reloadSelected) != reloadCommands.nodeMesh(reloadPeer),
			"Split mesh, material, and selected-node redirect survive save/reload");
	}
	else
	{
		passed &= expect(false, "split document reloads");
	}

	const Snapshot noCandidateBefore = snapshot(*document);
	const auto noCandidate = commands.mergeMeshes(noMaterialNode);
	passed &= expect(noCandidate.domainNoop() && noCandidate.error == MutationErrorCode::noOp
		&& snapshot(*document) == noCandidateBefore,
		"Merge without an equivalent referenced mesh is a zero-change no-op");
	const Snapshot noMaterialBefore = snapshot(*document);
	const auto noMaterialSplit = commands.splitPrimitive(
		noMaterialNode, commands.meshPrimitives(noMaterialMesh).front());
	passed &= expect(noMaterialSplit.domainNoop()
		&& noMaterialSplit.error == MutationErrorCode::noOp
		&& snapshot(*document) == noMaterialBefore,
		"Split without a target material is an atomic zero-change no-op");

	const auto merged = commands.mergeMeshes(selected);
	passed &= expect(merged.applied() && commands.nodeMesh(selected) == sharedMesh
		&& commands.nodeMesh(peer) == sharedMesh
		&& commands.meshReferenceCount(sharedMesh) == 2u
		&& !document->contains(copiedMesh.untyped())
		&& !document->contains(copiedFirst.untyped())
		&& !document->contains(copiedSecond.untyped())
		&& document->objectCount(EditorObjectKind::mesh) == 2u
		&& document->objectCount(EditorObjectKind::primitive) == 3u,
		"Merge redirects the selected node and removes only its orphan mesh copy");
	passed &= expect(document->contains(copiedMaterial.untyped())
		&& commands.materialReferenceCount(copiedMaterial) == 0u
		&& document->contains(originalPrimitives[0].untyped())
		&& document->contains(originalPrimitives[1].untyped())
		&& document->primitiveAddress(originalPrimitives[0]) == PrimitiveAddress{0u, 0u}
		&& document->primitiveAddress(originalPrimitives[1]) == PrimitiveAddress{0u, 1u},
		"Merge preserves survivor IDs and does not delete the independent material resource");
	const auto mergedPath = std::filesystem::temp_directory_path()
		/ "mgif_p3_r09_merged.gltf";
	passed &= expect(GltfDocumentIO::saveAs(*document, mergedPath).succeeded(),
		"merged document saves");
	auto mergedReload = loadDocument(mergedPath);
	if(mergedReload)
	{
		SceneCommands reloadCommands(*mergedReload);
		const NodeId reloadSelected = *NodeId::from(
			mergedReload->idAt(EditorObjectKind::node, 0));
		const NodeId reloadPeer = *NodeId::from(
			mergedReload->idAt(EditorObjectKind::node, 1));
		passed &= expect(mergedReload->objectCount(EditorObjectKind::mesh) == 2u
			&& mergedReload->objectCount(EditorObjectKind::material) == 3u
			&& mergedReload->objectCount(EditorObjectKind::primitive) == 3u
			&& reloadCommands.nodeMesh(reloadSelected) == reloadCommands.nodeMesh(reloadPeer),
			"merged references and orphan removal survive save/reload");
	}
	else
	{
		passed &= expect(false, "merged document reloads");
	}

	auto retained = loadDocument(fixture("r09_shared_mesh.gltf"));
	SceneCommands retainedCommands(*retained);
	const NodeId retainedSelected = *NodeId::from(
		retained->idAt(EditorObjectKind::node, 0));
	const NodeId retainedOther = *NodeId::from(retained->idAt(EditorObjectKind::node, 2));
	const MeshId retainedOriginal = *MeshId::from(
		retained->idAt(EditorObjectKind::mesh, 0));
	const PrimitiveId retainedPrimitive = retainedCommands.meshPrimitives(
		retainedOriginal).front();
	const auto retainedSplit = retainedCommands.splitPrimitive(
		retainedSelected, retainedPrimitive);
	const MeshId retainedCopy = *MeshId::from(
		*addedId(retainedSplit.remap.meshes, EditorObjectKind::mesh));
	passed &= expect(retainedCommands.attachMesh(retainedOther, retainedCopy).applied()
		&& retainedCommands.mergeMeshes(retainedSelected).applied()
		&& retained->contains(retainedCopy.untyped())
		&& retainedCommands.meshReferenceCount(retainedCopy) == 1u
		&& retainedCommands.nodeMesh(retainedOther) == retainedCopy,
		"Merge retains a source mesh that is still referenced by another node");

	auto other = loadDocument(fixture("r09_shared_mesh.gltf"));
	SceneCommands otherCommands(*other);
	const NodeId foreignNode = *NodeId::from(other->idAt(EditorObjectKind::node, 0));
	const MeshId foreignMesh = *MeshId::from(other->idAt(EditorObjectKind::mesh, 0));
	const PrimitiveId foreignPrimitive = otherCommands.meshPrimitives(foreignMesh).front();
	const Snapshot crossBefore = snapshot(*document);
	passed &= expect(commands.splitPrimitive(foreignNode, originalPrimitives.front()).error
			== MutationErrorCode::crossDocument
		&& commands.splitPrimitive(selected, foreignPrimitive).error
			== MutationErrorCode::crossDocument
		&& commands.mergeMeshes(foreignNode).error == MutationErrorCode::crossDocument
		&& commands.mergeMeshes(sharedMesh, foreignMesh).error
			== MutationErrorCode::crossDocument
		&& snapshot(*document) == crossBefore,
		"R09 node, primitive, and mesh cross-document inputs reject atomically");

	auto compatibility = loadDocument(fixture("r09_shared_mesh.gltf"));
	SceneCommands compatibilityCommands(*compatibility);
	const MeshId compatibilityMesh = *MeshId::from(
		compatibility->idAt(EditorObjectKind::mesh, 0));
	const PrimitiveId compatibilityPrimitive = compatibilityCommands.meshPrimitives(
		compatibilityMesh).front();
	const Snapshot compatibilityBefore = snapshot(*compatibility);
	passed &= expect(compatibilityCommands.splitPrimitive(compatibilityPrimitive).error
			== MutationErrorCode::multiScenePolicy
		&& snapshot(*compatibility) == compatibilityBefore,
		"legacy Split refuses to guess among nodes sharing the source mesh");

	const auto projection = SceneProjection::project(*document);
	passed &= expect(projection && completeMapping(*document, projection),
		"projection after R09 split/merge covers every surviving typed ID");
	std::error_code error;
	std::filesystem::remove(splitPath, error);
	std::filesystem::remove(mergedPath, error);
	return passed;
}

bool testVariants()
{
	bool passed = true;
	const auto path = writeVariantFixture();
	auto document = loadDocument(path);
	passed &= expect(document != nullptr, "variant fixture loads");
	if(document)
	{
		SceneCommands commands(*document);
		const PrimitiveId primitive = *PrimitiveId::from(
			document->idAt(EditorObjectKind::primitive, 0));
		const MaterialId first = *MaterialId::from(
			document->idAt(EditorObjectKind::material, 0));
		const MaterialId second = *MaterialId::from(
			document->idAt(EditorObjectKind::material, 1));
		passed &= expect(commands.primitiveVariantMaterial(primitive, 0) == first,
			"variant query returns the initial material");
		passed &= expect(commands.setPrimitiveVariant(primitive, 0, second).applied()
			&& commands.primitiveVariantMaterial(primitive, 0) == second,
			"variant replacement removes the old mapping and installs the new one");
		const Snapshot replaced = snapshot(*document);
		passed &= expect(commands.setPrimitiveVariant(primitive, 0, second).domainNoop()
			&& snapshot(*document) == replaced,
			"repeated variant replacement is an exact no-op");
		passed &= expect(commands.setPrimitiveVariant(primitive, 0, std::nullopt).applied()
			&& !commands.primitiveVariantMaterial(primitive, 0),
			"null variant material removes the mapping");
		const Snapshot removed = snapshot(*document);
		passed &= expect(commands.setPrimitiveVariant(primitive, 0, std::nullopt).domainNoop()
			&& snapshot(*document) == removed,
			"repeated variant removal is an exact no-op");
	}
	std::error_code error;
	std::filesystem::remove(path, error);
	return passed;
}

bool testD20Occurrences()
{
	bool passed = true;
	const auto d20Path = std::filesystem::temp_directory_path()
		/ "mgif_p3_d20_all_scenes.gltf";
	const auto d20SharedPath = std::filesystem::temp_directory_path()
		/ "mgif_p3_d20_shared_operations.gltf";
	{
		auto document = loadDocument(fixture("multi_scene_oracle.gltf"));
		if(!document) return expect(false, "D20 all-scenes fixture loads");
		SceneCommands commands(*document);
		const SceneId scene0 = *SceneId::from(
			document->idAt(EditorObjectKind::scene, 0));
		const SceneId scene1 = *SceneId::from(
			document->idAt(EditorObjectKind::scene, 1));
		const NodeId sharedRoot = *NodeId::from(
			document->idAt(EditorObjectKind::node, 6));
		const NodeId sharedChild = *NodeId::from(
			document->idAt(EditorObjectKind::node, 7));
		const auto occurrence = commands.captureNodeOccurrence(scene0, sharedRoot);
		const Snapshot before = snapshot(*document);
		const auto removed = occurrence
			? commands.deleteNodeAt(*occurrence, SceneOccurrencePolicy::allScenes)
			: MutationResult{};
		const auto scene0Names = nodeNames(*document, document->sceneRootNodeIds(scene0));
		passed &= expect(occurrence && removed.applied()
			&& removed.beforeRevision == before.revision
			&& removed.afterRevision == before.revision + 1u
			&& !document->contains(sharedRoot.untyped())
			&& !document->contains(sharedChild.untyped())
			&& std::find(scene0Names.begin(), scene0Names.end(), "SharedRoot")
				== scene0Names.end(),
			"all-scenes Delete removes every shared-root occurrence and stable ID");
		const auto scene1Names = nodeNames(*document, document->sceneRootNodeIds(scene1));
		passed &= expect(std::find(scene1Names.begin(), scene1Names.end(), "SharedRoot")
			== scene1Names.end() && GltfDocumentIO::saveAs(*document, d20Path).succeeded(),
			"all-scenes Delete updates every scene and saves");
	}
	if(auto reloaded = loadDocument(d20Path))
	{
		const SceneId scene0 = *SceneId::from(reloaded->idAt(EditorObjectKind::scene, 0));
		const SceneId scene1 = *SceneId::from(reloaded->idAt(EditorObjectKind::scene, 1));
		const auto names0 = nodeNames(*reloaded, reloaded->sceneRootNodeIds(scene0));
		const auto names1 = nodeNames(*reloaded, reloaded->sceneRootNodeIds(scene1));
		passed &= expect(std::find(names0.begin(), names0.end(), "SharedRoot") == names0.end()
			&& std::find(names1.begin(), names1.end(), "SharedRoot") == names1.end(),
			"all-scenes Delete survives save/reload in both scenes");
	}
	else
	{
		passed &= expect(false, "all-scenes Delete document reloads");
	}

	for(const std::size_t clicked : {0u, 1u})
	{
		{
			auto document = loadDocument(fixture("multi_scene_oracle.gltf"));
			SceneCommands commands(*document);
			const SceneId clickedScene = *SceneId::from(
				document->idAt(EditorObjectKind::scene, clicked));
			const SceneId otherScene = *SceneId::from(
				document->idAt(EditorObjectKind::scene, 1u - clicked));
			const NodeId sharedRoot = *NodeId::from(
				document->idAt(EditorObjectKind::node, 6));
			const NodeId stableUnique = *NodeId::from(document->idAt(
				EditorObjectKind::node, clicked == 0u ? 3u : 0u));
			const auto otherNamesBefore = nodeNames(
				*document, document->sceneRootNodeIds(otherScene));
			const auto occurrence = commands.captureNodeOccurrence(clickedScene, sharedRoot);
			const auto removed = occurrence
				? commands.deleteNodeAt(*occurrence, SceneOccurrencePolicy::clickedScene)
				: MutationResult{};
			const auto clickedNames = nodeNames(
				*document, document->sceneRootNodeIds(clickedScene));
			const auto otherNames = nodeNames(*document, document->sceneRootNodeIds(otherScene));
			passed &= expect(removed.applied()
				&& std::find(clickedNames.begin(), clickedNames.end(), "SharedRoot")
					== clickedNames.end()
				&& otherNames == otherNamesBefore
				&& !document->contains(sharedRoot.untyped())
				&& document->contains(stableUnique.untyped()),
				clicked == 0u
					? "clicked-scene Delete isolates Scene Zero and preserves unrelated IDs"
					: "clicked-scene Delete isolates Scene One and preserves unrelated IDs");
		}

		{
			auto document = loadDocument(fixture("multi_scene_oracle.gltf"));
			SceneCommands commands(*document);
			const SceneId clickedScene = *SceneId::from(
				document->idAt(EditorObjectKind::scene, clicked));
			const SceneId otherScene = *SceneId::from(
				document->idAt(EditorObjectKind::scene, 1u - clicked));
			const NodeId sharedRoot = *NodeId::from(
				document->idAt(EditorObjectKind::node, 6));
			const NodeId sharedChild = *NodeId::from(
				document->idAt(EditorObjectKind::node, 7));
			const auto source = commands.captureNodeOccurrence(clickedScene, sharedChild);
			const auto target = commands.captureNodeOccurrence(clickedScene, sharedRoot);
			const auto duplicated = source && target
				? commands.duplicateNodeAt(*source, *target,
					SceneOccurrencePolicy::clickedScene)
				: MutationResult{};
			const auto otherRoots = document->sceneRootNodeIds(otherScene);
			const auto otherShared = std::find_if(otherRoots.begin(), otherRoots.end(),
				[&](NodeId node)
				{ return document->objectName(node.untyped()) == "SharedRoot"; });
			passed &= expect(duplicated.applied()
				&& document->nodeChildIds(sharedRoot).size() == 2u
				&& otherShared != otherRoots.end()
				&& document->nodeChildIds(*otherShared).size() == 1u
				&& document->contains(sharedRoot.untyped())
				&& document->contains(sharedChild.untyped()),
				clicked == 0u
					? "clicked-scene Duplicate isolates a shared Scene Zero parent"
					: "clicked-scene Duplicate isolates a shared Scene One parent");
		}

		auto document = loadDocument(fixture("multi_scene_oracle.gltf"));
		SceneCommands commands(*document);
		const SceneId clickedScene = *SceneId::from(
			document->idAt(EditorObjectKind::scene, clicked));
		const SceneId otherScene = *SceneId::from(
			document->idAt(EditorObjectKind::scene, 1u - clicked));
		passed &= expect(document->selectCurrentScene(otherScene),
			"D20 routing test selects the opposite current scene");
		const NodeId sharedRoot = *NodeId::from(
			document->idAt(EditorObjectKind::node, 6));
		const NodeId sharedChild = *NodeId::from(
			document->idAt(EditorObjectKind::node, 7));
		const NodeId target = *NodeId::from(document->idAt(
			EditorObjectKind::node, clicked == 0u ? 0u : 3u));
		const auto sourceOccurrence = commands.captureNodeOccurrence(
			clickedScene, sharedChild);
		const auto targetOccurrence = commands.captureNodeOccurrence(clickedScene, target);
		const auto otherNamesBefore = nodeNames(
			*document, document->sceneRootNodeIds(otherScene));
		const auto moved = sourceOccurrence && targetOccurrence
			? commands.reparentNodeAt(*sourceOccurrence, *targetOccurrence,
				SceneOccurrencePolicy::clickedScene)
			: MutationResult{};
		const auto targetChildren = document->nodeChildIds(target);
		const auto sharedChildren = document->nodeChildIds(sharedRoot);
		const auto otherNamesAfter = nodeNames(
			*document, document->sceneRootNodeIds(otherScene));
		passed &= expect(sourceOccurrence && targetOccurrence && moved.applied()
			&& std::find(targetChildren.begin(), targetChildren.end(), sharedChild)
				!= targetChildren.end()
			&& sharedChildren.empty() && otherNamesAfter == otherNamesBefore
			&& document->contains(sharedRoot.untyped())
			&& document->contains(sharedChild.untyped()),
			clicked == 0u
				? "clicked-scene Reparent isolates Scene Zero while preserving Scene One"
				: "clicked-scene Reparent isolates Scene One while preserving Scene Zero");
		const auto otherRoots = document->sceneRootNodeIds(otherScene);
		const auto foundOtherShared = std::find_if(otherRoots.begin(), otherRoots.end(),
			[&](NodeId node)
			{ return document->objectName(node.untyped()) == "SharedRoot"; });
		passed &= expect(foundOtherShared != otherRoots.end()
			&& document->nodeChildIds(*foundOtherShared).size() == 1u
			&& document->objectName(
				document->nodeChildIds(*foundOtherShared).front().untyped()) == "SharedChild",
			"copy-on-write preserves the unclicked shared-parent topology");

		const auto clickedPath = std::filesystem::temp_directory_path()
			/ ("mgif_p3_d20_clicked_" + std::to_string(clicked) + ".gltf");
		passed &= expect(GltfDocumentIO::saveAs(*document, clickedPath).succeeded(),
			"clicked-scene Reparent saves");
		if(auto reloaded = loadDocument(clickedPath))
		{
			const SceneId reloadedClicked = *SceneId::from(
				reloaded->idAt(EditorObjectKind::scene, clicked));
			const SceneId reloadedOther = *SceneId::from(
				reloaded->idAt(EditorObjectKind::scene, 1u - clicked));
			const auto clickedRoots = reloaded->sceneRootNodeIds(reloadedClicked);
			const auto reloadedTarget = std::find_if(clickedRoots.begin(), clickedRoots.end(),
				[&](NodeId node)
				{ return reloaded->objectName(node.untyped()) == (clicked == 0u ? "A0" : "B0"); });
			const auto reloadedShared = std::find_if(clickedRoots.begin(), clickedRoots.end(),
				[&](NodeId node)
				{ return reloaded->objectName(node.untyped()) == "SharedRoot"; });
			const auto otherReloadedRoots = reloaded->sceneRootNodeIds(reloadedOther);
			const auto otherReloadedShared = std::find_if(
				otherReloadedRoots.begin(), otherReloadedRoots.end(), [&](NodeId node)
				{ return reloaded->objectName(node.untyped()) == "SharedRoot"; });
			passed &= expect(reloadedTarget != clickedRoots.end()
				&& reloadedShared != clickedRoots.end()
				&& otherReloadedShared != otherReloadedRoots.end()
				&& nodeNames(*reloaded, reloaded->nodeChildIds(*reloadedTarget))
					== std::vector<std::string>{
						clicked == 0u ? "AChild" : "BChild", "SharedChild"}
				&& reloaded->nodeChildIds(*reloadedShared).empty()
				&& nodeNames(*reloaded, reloaded->nodeChildIds(*otherReloadedShared))
					== std::vector<std::string>{"SharedChild"},
				"clicked-scene Reparent topology survives save/reload");
		}
		else
		{
			passed &= expect(false, "clicked-scene Reparent document reloads");
		}
		std::error_code clickedError;
		std::filesystem::remove(clickedPath, clickedError);
	}

	{
		auto document = loadDocument(fixture("multi_scene_oracle.gltf"));
		SceneCommands commands(*document);
		const SceneId scene0 = *SceneId::from(
			document->idAt(EditorObjectKind::scene, 0));
		const SceneId scene1 = *SceneId::from(
			document->idAt(EditorObjectKind::scene, 1));
		const NodeId sharedRoot = *NodeId::from(
			document->idAt(EditorObjectKind::node, 6));
		const NodeId sharedChild = *NodeId::from(
			document->idAt(EditorObjectKind::node, 7));
		const auto childOccurrence = commands.captureNodeOccurrence(scene0, sharedChild);
		const auto rootOccurrence = commands.captureNodeOccurrence(scene0, sharedRoot);
		const auto duplicated = childOccurrence && rootOccurrence
			? commands.duplicateNodeAt(*childOccurrence, *rootOccurrence,
				SceneOccurrencePolicy::allScenes)
			: MutationResult{};
		const auto duplicateId = addedId(duplicated.remap.nodes, EditorObjectKind::node);
		passed &= expect(duplicated.applied() && duplicateId
			&& document->nodeChildIds(sharedRoot).size() == 2u
			&& commands.captureNodeOccurrence(scene0, *NodeId::from(*duplicateId))
			&& commands.captureNodeOccurrence(scene1, *NodeId::from(*duplicateId)),
			"all-scenes Duplicate creates one consistently shared occurrence");
		const auto refreshedChild = commands.captureNodeOccurrence(scene0, sharedChild);
		const auto moved = refreshedChild
			? commands.reparentNodeAt(*refreshedChild, std::nullopt,
				SceneOccurrencePolicy::allScenes)
			: MutationResult{};
		const auto roots0 = document->sceneRootNodeIds(scene0);
		const auto roots1 = document->sceneRootNodeIds(scene1);
		passed &= expect(moved.applied()
			&& std::find(roots0.begin(), roots0.end(), sharedChild) != roots0.end()
			&& std::find(roots1.begin(), roots1.end(), sharedChild) != roots1.end()
			&& document->contains(sharedChild.untyped()),
			"all-scenes Reparent preserves the source ID across every scene root");
		passed &= expect(GltfDocumentIO::saveAs(*document, d20SharedPath).succeeded(),
			"all-scenes Duplicate and Reparent save");
		if(auto reloaded = loadDocument(d20SharedPath))
		{
			bool topologyMatches = true;
			for(const std::size_t scene : {0u, 1u})
			{
				const SceneId id = *SceneId::from(
					reloaded->idAt(EditorObjectKind::scene, scene));
				const auto roots = reloaded->sceneRootNodeIds(id);
				const auto shared = std::find_if(roots.begin(), roots.end(),
					[&](NodeId node)
					{ return reloaded->objectName(node.untyped()) == "SharedRoot"; });
				topologyMatches = topologyMatches && shared != roots.end()
					&& std::count_if(roots.begin(), roots.end(), [&](NodeId node)
					{
						return reloaded->objectName(node.untyped()) == "SharedChild";
					}) == 1
					&& nodeNames(*reloaded, reloaded->nodeChildIds(*shared))
						== std::vector<std::string>{"SharedChild"};
			}
			passed &= expect(topologyMatches,
				"all-scenes Duplicate and Reparent survive save/reload in both scenes");
		}
		else
		{
			passed &= expect(false,
				"all-scenes Duplicate and Reparent document reloads");
		}

		const auto cycleSource = commands.captureNodeOccurrence(scene0, sharedRoot);
		const auto cycleTarget = commands.captureNodeOccurrence(scene0, *NodeId::from(*duplicateId));
		const Snapshot cycleBefore = snapshot(*document);
		const auto cycle = cycleSource && cycleTarget
			? commands.reparentNodeAt(*cycleSource, *cycleTarget,
				SceneOccurrencePolicy::clickedScene)
			: MutationResult{};
		passed &= expect(cycle.error == MutationErrorCode::cycle
			&& cycle.changedIds.empty() && cycle.remap.nodes.empty()
			&& snapshot(*document) == cycleBefore,
			"D20 cycle rejection leaves revision, IDs, remap, and document unchanged");
	}

	{
		auto document = loadDocument(fixture("multi_scene_oracle.gltf"));
		SceneCommands commands(*document);
		const SceneId scene0 = *SceneId::from(
			document->idAt(EditorObjectKind::scene, 0));
		const NodeId sharedChild = *NodeId::from(
			document->idAt(EditorObjectKind::node, 7));
		const NodeId sceneOnlyTarget = *NodeId::from(
			document->idAt(EditorObjectKind::node, 0));
		const auto source = commands.captureNodeOccurrence(scene0, sharedChild);
		const auto target = commands.captureNodeOccurrence(scene0, sceneOnlyTarget);
		const Snapshot inconsistentBefore = snapshot(*document);
		const auto inconsistent = source && target
			? commands.reparentNodeAt(*source, *target, SceneOccurrencePolicy::allScenes)
			: MutationResult{};
		passed &= expect(inconsistent.error == MutationErrorCode::multiScenePolicy
			&& inconsistent.changedIds.empty() && inconsistent.remap.nodes.empty()
			&& snapshot(*document) == inconsistentBefore,
			"all-scenes shared-parent mismatch rejects before publication");
		const auto staleOccurrence = source;
		passed &= expect(commands.rename(sceneOnlyTarget.untyped(), "A0 renamed").applied(),
			"stale-occurrence setup mutation applies");
		const Snapshot staleBefore = snapshot(*document);
		const auto stale = staleOccurrence
			? commands.deleteNodeAt(*staleOccurrence, SceneOccurrencePolicy::clickedScene)
			: MutationResult{};
		passed &= expect(stale.error == MutationErrorCode::staleOccurrence
			&& stale.changedIds.empty() && stale.remap.nodes.empty()
			&& snapshot(*document) == staleBefore,
			"stale occurrence rejection has no revision, ID, or remap side effect");
	}

	{
		auto document = loadDocument(fixture("multi_scene_oracle.gltf"));
		SceneCommands commands(*document);
		const SceneId scene0 = *SceneId::from(
			document->idAt(EditorObjectKind::scene, 0));
		const SceneId scene1 = *SceneId::from(
			document->idAt(EditorObjectKind::scene, 1));
		const NodeId firstRoot = *NodeId::from(
			document->idAt(EditorObjectKind::node, 0));
		const NodeId secondRoot = *NodeId::from(
			document->idAt(EditorObjectKind::node, 1));
		const NodeId sharedRoot = *NodeId::from(
			document->idAt(EditorObjectKind::node, 6));
		passed &= expect(commands.deleteNode(firstRoot).applied()
			&& commands.deleteNode(secondRoot).applied()
			&& document->selectCurrentScene(scene1),
			"D20 unique-root case selects a different current scene");
		const auto occurrence = commands.captureNodeOccurrence(scene0, sharedRoot);
		const Snapshot before = snapshot(*document);
		const auto rejected = occurrence
			? commands.deleteNodeAt(*occurrence, SceneOccurrencePolicy::allScenes)
			: MutationResult{};
		passed &= expect(rejected.domainNoop()
			&& rejected.error == MutationErrorCode::uniqueRootDeleteNoop
			&& rejected.changedIds.empty() && rejected.remap.nodes.empty()
			&& snapshot(*document) == before,
			"all-scenes Delete honors every affected unique root, not current scene");
	}

	{
		auto document = loadDocument(fixture("multi_scene_oracle.gltf"));
		auto foreign = loadDocument(fixture("multi_scene_oracle.gltf"));
		SceneCommands commands(*document);
		SceneCommands foreignCommands(*foreign);
		const SceneId foreignScene = *SceneId::from(
			foreign->idAt(EditorObjectKind::scene, 0));
		const NodeId foreignNode = *NodeId::from(
			foreign->idAt(EditorObjectKind::node, 6));
		const auto foreignOccurrence = foreignCommands.captureNodeOccurrence(
			foreignScene, foreignNode);
		const Snapshot crossBefore = snapshot(*document);
		const auto cross = foreignOccurrence
			? commands.deleteNodeAt(*foreignOccurrence, SceneOccurrencePolicy::clickedScene)
			: MutationResult{};
		passed &= expect(cross.error == MutationErrorCode::crossDocument
			&& snapshot(*document) == crossBefore,
			"cross-document occurrence rejects atomically");
	}

	{
		auto document = loadDocument(fixture("multi_scene_oracle.gltf"));
		SceneCommands commands(*document);
		const SceneId scene0 = *SceneId::from(
			document->idAt(EditorObjectKind::scene, 0));
		const NodeId sharedRoot = *NodeId::from(
			document->idAt(EditorObjectKind::node, 6));
		const auto occurrence = commands.captureNodeOccurrence(scene0, sharedRoot);
		EditorLimits limits = kDefaultEditorLimits;
		limits.maxSceneNodeOccurrences = 1u;
		SceneCommands limitedCommands(*document, limits);
		const Snapshot before = snapshot(*document);
		const auto bounded = occurrence
			? limitedCommands.deleteNodeAt(
				*occurrence, SceneOccurrencePolicy::allScenes)
			: MutationResult{};
		passed &= expect(bounded.error == MutationErrorCode::limitExceeded
			&& bounded.changedIds.empty() && bounded.remap.nodes.empty()
			&& snapshot(*document) == before,
			"D20 occurrence traversal rejects atomically at its configured bound");
	}

	for(const EditorFaultPoint point : {
		EditorFaultPoint::allocation, EditorFaultPoint::preCandidatePublish})
	{
		auto document = loadDocument(fixture("multi_scene_oracle.gltf"));
		testing::ScriptedEditorFaultInjector faults;
		SceneCommands commands(*document, kDefaultEditorLimits, &faults);
		const SceneId scene0 = *SceneId::from(
			document->idAt(EditorObjectKind::scene, 0));
		const NodeId sharedRoot = *NodeId::from(
			document->idAt(EditorObjectKind::node, 6));
		const auto occurrence = commands.captureNodeOccurrence(scene0, sharedRoot);
		passed &= expect(faults.failOnCall(point, 1u),
			"D20 rollback fault schedule is accepted");
		const Snapshot before = snapshot(*document);
		const auto rejected = occurrence
			? commands.deleteNodeAt(*occurrence, SceneOccurrencePolicy::allScenes)
			: MutationResult{};
		passed &= expect(rejected.rejected() && rejected.changedIds.empty()
			&& rejected.remap.nodes.empty() && snapshot(*document) == before,
			point == EditorFaultPoint::allocation
				? "all-scenes candidate allocation failure restores every scene"
				: "all-scenes pre-publication failure restores every scene");
	}

	std::error_code error;
	std::filesystem::remove(d20Path, error);
	std::filesystem::remove(d20SharedPath, error);
	return passed;
}

bool testOccurrencesRootsAndLimits()
{
	bool passed = true;
	auto document = loadDocument(fixture("multi_scene_oracle.gltf"));
	if(!document) return expect(false, "multi-scene fixture loads");
	SceneCommands commands(*document);
	const SceneId scene0 = *SceneId::from(document->idAt(EditorObjectKind::scene, 0));
	const SceneId scene1 = *SceneId::from(document->idAt(EditorObjectKind::scene, 1));
	const NodeId sharedRoot = *NodeId::from(document->idAt(EditorObjectKind::node, 6));

	SceneRootTransform transform;
	transform.translation = {2.0, 0.0, 0.0};
	passed &= expect(commands.previewSceneRootTransform(scene0, transform).applied()
		&& commands.previewSceneRootTransform(scene1, transform).applied(),
		"matching per-scene transforms can be prepared for a shared root");
	const auto added = commands.addNode(scene0, "Baked New Root");
	const auto addedNodeValue = addedId(added.remap.nodes, EditorObjectKind::node);
	passed &= expect(added.applied() && addedNodeValue,
		"topology edit with matching shared-root transforms applies");
	if(addedNodeValue)
	{
		const NodeId addedNode = *NodeId::from(*addedNodeValue);
		passed &= expect(document->sceneRootNodeFrozen(scene0, addedNode)
			&& document->sceneRootNodeFrozen(scene0, sharedRoot)
			&& document->sceneRootNodeFrozen(scene1, sharedRoot),
			"new and shared roots receive per-root frozen identities");
		const auto projection = SceneProjection::project(*document);
		const auto sourceIndex = document->indexOf(addedNode.untyped());
		const auto projected = sourceIndex ? std::find_if(projection.asset.nodes.begin(),
			projection.asset.nodes.end(), [&](const demo::SceneNode& node)
			{ return node.sourceNodeIndex == *sourceIndex; }) : projection.asset.nodes.end();
		passed &= expect(projection && projected != projection.asset.nodes.end()
			&& std::abs(projected->translation.x - 2.0f) <= 1.0e-5f,
			"new root receives the scene transform exactly once before freeze");
	}

	auto mismatch = loadDocument(fixture("multi_scene_oracle.gltf"));
	SceneCommands mismatchCommands(*mismatch);
	SceneRootTransform otherTransform = transform;
	otherTransform.translation[0] = 3.0;
	const SceneId mismatchScene0 = *SceneId::from(mismatch->idAt(EditorObjectKind::scene, 0));
	const SceneId mismatchScene1 = *SceneId::from(mismatch->idAt(EditorObjectKind::scene, 1));
	const auto firstMismatchPreview =
		mismatchCommands.previewSceneRootTransform(mismatchScene0, transform);
	const auto secondMismatchPreview =
		mismatchCommands.previewSceneRootTransform(mismatchScene1, otherTransform);
	passed &= expect(firstMismatchPreview.applied() && secondMismatchPreview.applied(),
		"different per-scene previews remain runtime-only until topology changes");
	const Snapshot mismatchBefore = snapshot(*mismatch);
	const auto mismatchResult = mismatchCommands.addNode(mismatchScene0, "Rejected Root");
	passed &= expect(mismatchResult.error == MutationErrorCode::multiScenePolicy
		&& snapshot(*mismatch) == mismatchBefore,
		"different transforms on a shared root reject the candidate atomically");

	auto matrixDocument = loadDocument(fixture("scene_root_matrix.gltf"));
	const auto matrixBefore = SceneProjection::project(*matrixDocument);
	SceneCommands matrixCommands(*matrixDocument);
	const auto identityTopology = matrixCommands.addNode(
		matrixDocument->currentSceneId(), "Identity Root");
	const auto matrixAfter = SceneProjection::project(*matrixDocument);
	passed &= expect(identityTopology.applied()
		&& !matrixDocument->sceneRootTransformState(matrixDocument->currentSceneId()).frozen
		&& matrixBefore && matrixAfter && !matrixBefore.asset.nodes.empty()
		&& !matrixAfter.asset.nodes.empty()
		&& matrixNear(matrixBefore.asset.nodes[0].localTransform,
			matrixAfter.asset.nodes[0].localTransform),
		"identity scene-root bake does not rewrite an existing matrix root");

	auto limited = loadDocument(fixture("minimal_scene.gltf"));
	EditorLimits limits = kDefaultEditorLimits;
	limits.maxNodes = limited->objectCount(EditorObjectKind::node);
	SceneCommands limitedCommands(*limited, limits);
	const Snapshot limitBefore = snapshot(*limited);
	const auto limitResult = limitedCommands.addNode(limited->currentSceneId(), "Too Many");
	passed &= expect(limitResult.error == MutationErrorCode::limitExceeded
		&& snapshot(*limited) == limitBefore,
		"capacity rejection leaves revision, digest, and all IDs unchanged");
	return passed;
}

} // namespace

int main()
{
	bool passed = true;
	passed &= testAtomicityAndTokens();
	passed &= testVisibilityRoundTrip();
	passed &= testRemapAndProperties();
	passed &= testSplitMergeWorkflow();
	passed &= testVariants();
	passed &= testD20Occurrences();
	passed &= testOccurrencesRootsAndLimits();
	return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
