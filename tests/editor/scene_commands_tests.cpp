#include <mgif/scene_editor/GltfDocumentIO.h>
#include <mgif/scene_editor/SceneCommands.h>
#include <mgif/scene_editor/SceneEditor.h>
#include <mgif/scene_editor/GltfSemanticCompare.h>
#include <mgif/scene_editor/GltfValidator.h>
#include <mgif/scene_editor/SceneProjection.h>
#include <mgif/scene_editor/commands/HierarchyCommands.h>
#include <mgif/scene_editor/commands/HistoryLifecycle.h>
#include <mgif/scene_editor/commands/TransformCommands.h>
#include <mgif/scene_editor/testing/EditorStateInspector.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cmath>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace allocation_probe
{

std::atomic<bool> enabled{false};
std::atomic<std::size_t> calls{0};

} // namespace allocation_probe

void* operator new(std::size_t size)
{
	if(allocation_probe::enabled.load(std::memory_order_relaxed))
		allocation_probe::calls.fetch_add(1u, std::memory_order_relaxed);
	if(void* memory = std::malloc(size == 0u ? 1u : size)) return memory;
	throw std::bad_alloc{};
}

void* operator new[](std::size_t size) { return ::operator new(size); }

void operator delete(void* memory) noexcept { std::free(memory); }

void operator delete[](void* memory) noexcept { std::free(memory); }

void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace
{
using namespace demo::editor;

bool expect(bool condition, const char* message)
{
	if(!condition) std::cerr << "FAILED: " << message << '\n';
	return condition;
}

template <typename Validate>
bool expectAllocationFreeValidation(Validate&& validate, PreparedValueStatus expectedStatus,
	MutationErrorCode expectedError, const char* message)
{
	allocation_probe::calls.store(0u, std::memory_order_relaxed);
	allocation_probe::enabled.store(true, std::memory_order_release);
	const PreparedValueValidation result = std::forward<Validate>(validate)();
	allocation_probe::enabled.store(false, std::memory_order_release);
	const std::size_t calls = allocation_probe::calls.load(std::memory_order_relaxed);
	if(result.status != expectedStatus || result.error != expectedError || calls != 0u)
	{
		std::cerr << "FAILED: " << message << " status=" << static_cast<unsigned>(result.status)
				  << " error=" << static_cast<unsigned>(result.error) << " allocations=" << calls
				  << '\n';
		return false;
	}
	return true;
}

template <typename Compare>
bool expectAllocationFreeComparison(Compare&& compare, bool expected, const char* message)
{
	allocation_probe::calls.store(0u, std::memory_order_relaxed);
	allocation_probe::enabled.store(true, std::memory_order_release);
	const bool result = std::forward<Compare>(compare)();
	allocation_probe::enabled.store(false, std::memory_order_release);
	const std::size_t calls = allocation_probe::calls.load(std::memory_order_relaxed);
	if(result != expected || calls != 0u)
	{
		std::cerr << "FAILED: " << message << " result=" << result << " allocations=" << calls
				  << '\n';
		return false;
	}
	return true;
}

std::filesystem::path fixture(const char* name)
{
	return std::filesystem::path(MGIF_TEST_SOURCE_DIR) / "tests" / "fixtures" / "gltf_editor" / name;
}

struct ReplayContext final
{
	std::uint32_t seed{0};
	std::size_t step{0};
	std::string_view operation;
};

bool replayExpect(bool condition, const ReplayContext& context, std::string_view message)

{
	if(!condition)
	{
		std::cerr << "FAILED: seed=0x" << std::hex << context.seed << std::dec
			<< " step=" << context.step << " operation=" << context.operation
			<< ": " << message << '\n';
	}
	return condition;
}

struct RootStateSnapshot final
{
	SceneRootTransform baseline;
	SceneRootTransform current;
	std::uint64_t hierarchyEpoch{0};
	std::uint64_t externalStateSerial{0};
	bool frozen{false};
	std::vector<NodeId> frozenRootIds;

	bool operator==(const RootStateSnapshot& other) const
	{
		return baseline.translation == other.baseline.translation
			&& baseline.rotation == other.baseline.rotation
			&& baseline.scale == other.baseline.scale
			&& current.translation == other.current.translation
			&& current.rotation == other.current.rotation
			&& current.scale == other.current.scale
			&& hierarchyEpoch == other.hierarchyEpoch
			&& externalStateSerial == other.externalStateSerial
			&& frozen == other.frozen
			&& frozenRootIds == other.frozenRootIds;
	}
};

struct ExactDocumentSnapshot final
{
	std::string sourcePath;
	GltfDocumentFormat format{GltfDocumentFormat::ascii};
	std::uint64_t revision{0};
	std::uint64_t cleanRevision{0};
	bool dirty{false};
	std::optional<std::size_t> defaultScene;
	SceneId currentScene;
	std::string digest;
	std::string documentExtensions;
	std::string documentExtras;
	std::string assetExtensions;
	std::string assetExtras;
	std::array<std::vector<EditorObjectId>,
		static_cast<std::size_t>(EditorObjectKind::count)> ids;
	std::array<std::vector<std::string>,
		static_cast<std::size_t>(EditorObjectKind::count)> names;
	std::array<std::vector<bool>,
		static_cast<std::size_t>(EditorObjectKind::count)> visible;
	std::array<std::vector<bool>,
		static_cast<std::size_t>(EditorObjectKind::count)> hasVisibility;
	std::array<std::vector<std::string>,
		static_cast<std::size_t>(EditorObjectKind::count)> extensions;
	std::array<std::vector<std::string>,
		static_cast<std::size_t>(EditorObjectKind::count)> extras;
	std::vector<std::vector<NodeId>> sceneRoots;
	std::vector<std::vector<NodeId>> nodeChildren;
	std::vector<std::optional<PrimitiveAddress>> primitiveAddresses;
	std::vector<RootStateSnapshot> rootStates;
	std::vector<std::string> dependencies;

	bool operator==(const ExactDocumentSnapshot&) const = default;
};

ExactDocumentSnapshot exactSnapshot(const GltfDocument& document)
{
	ExactDocumentSnapshot result;
	result.sourcePath = document.sourcePath().generic_string();
	result.format = document.format();
	result.revision = document.revision();
	result.cleanRevision = document.cleanRevision();
	result.dirty = document.dirty();
	result.defaultScene = document.defaultSceneIndex();
	result.currentScene = document.currentSceneId();
	result.digest = GltfSemanticCompare::digest(document);
	result.documentExtensions = document.documentExtensionsJson();
	result.documentExtras = document.documentExtrasJson();
	result.assetExtensions = document.assetExtensionsJson();
	result.assetExtras = document.assetExtrasJson();
	for(const auto& dependency : document.dependencies())
	{
		result.dependencies.push_back(
			std::to_string(static_cast<unsigned>(dependency.kind)) + ':'
			+ std::to_string(dependency.objectIndex) + ':' + dependency.uri + ':'
			+ dependency.resolvedPath.generic_string() + ':'
			+ std::to_string(dependency.byteSize));
	}
	for(std::size_t kind = 1u;
		kind < static_cast<std::size_t>(EditorObjectKind::count); ++kind)
	{
		const auto type = static_cast<EditorObjectKind>(kind);
		const std::size_t count = document.objectCount(type);
		for(std::size_t index = 0; index < count; ++index)
		{
			const EditorObjectId id = document.idAt(type, index);
			result.ids[kind].push_back(id);
			result.names[kind].emplace_back(document.objectName(id));
			result.visible[kind].push_back(document.objectVisible(id));
			result.hasVisibility[kind].push_back(document.objectHasVisibility(id));
			result.extensions[kind].emplace_back(document.objectExtensionsJson(id));
			result.extras[kind].emplace_back(document.objectExtrasJson(id));
		}
	}
	for(const EditorObjectId id : result.ids[static_cast<std::size_t>(EditorObjectKind::scene)])
	{
		const SceneId scene = *SceneId::from(id);
		result.sceneRoots.push_back(document.sceneRootNodeIds(scene));
		const auto state = document.sceneRootTransformState(scene);
		result.rootStates.push_back(RootStateSnapshot{
			state.baseline, state.current, state.hierarchyEpoch,
			state.externalStateSerial, state.frozen,
			document.frozenSceneRootNodeIds(scene)});
	}
	for(const EditorObjectId id : result.ids[static_cast<std::size_t>(EditorObjectKind::node)])
		result.nodeChildren.push_back(document.nodeChildIds(*NodeId::from(id)));
	for(const EditorObjectId id : result.ids[static_cast<std::size_t>(EditorObjectKind::primitive)])
		result.primitiveAddresses.push_back(document.primitiveAddress(*PrimitiveId::from(id)));
	return result;
}

bool validTypedId(EditorObjectId id)
{
	switch(id.kind)
	{
		case EditorObjectKind::scene: return SceneId::from(id).has_value();
		case EditorObjectKind::node: return NodeId::from(id).has_value();
		case EditorObjectKind::primitive: return PrimitiveId::from(id).has_value();
		case EditorObjectKind::mesh: return MeshId::from(id).has_value();
		case EditorObjectKind::material: return MaterialId::from(id).has_value();
		case EditorObjectKind::texture: return TextureId::from(id).has_value();
		case EditorObjectKind::image: return ImageId::from(id).has_value();
		case EditorObjectKind::sampler: return SamplerId::from(id).has_value();
		case EditorObjectKind::camera: return CameraId::from(id).has_value();
		case EditorObjectKind::light: return LightId::from(id).has_value();
		case EditorObjectKind::accessor: return AccessorId::from(id).has_value();
		case EditorObjectKind::bufferView: return BufferViewId::from(id).has_value();
		case EditorObjectKind::buffer: return BufferId::from(id).has_value();
		case EditorObjectKind::skin: return SkinId::from(id).has_value();
		case EditorObjectKind::animation: return AnimationId::from(id).has_value();
		default: return false;
	}
}

bool validateCompleteMapping(
	const GltfDocument& document,
	const SceneProjectionResult& projection,
	const ReplayContext& context)
{
	std::string error;
	if(!replayExpect(projection.mapping.validate(document, projection.asset, &error),
	                 context, "runtime mapping validation failed: " + error)) return false;
	std::size_t expected = 0;
	for(std::size_t kind = 1u;
		kind < static_cast<std::size_t>(EditorObjectKind::count); ++kind)
	{
		const auto type = static_cast<EditorObjectKind>(kind);
		expected += document.objectCount(type);
		for(std::size_t index = 0; index < document.objectCount(type); ++index)
		{
			const EditorObjectId id = document.idAt(type, index);
			const auto entry = projection.mapping.entry(id);
			if(!replayExpect(entry.has_value(), context,
			                 "mapping omits a live typed ID")) return false;
			if(entry->sceneAssetIndex != SceneRuntimeMapping::kInvalidIndex)
			{
				if(!replayExpect(
					projection.mapping.documentIdAt(type, entry->sceneAssetIndex) == id
						&& projection.mapping.runtimeIndex(id) == entry->runtimeIndex
						&& projection.mapping.documentIdForRuntime(entry->runtimeIndex) == id,
					context, "mapping forward/reverse/runtime identity is inconsistent")) return false;
			}
			else if(!replayExpect(!projection.mapping.runtimeIndex(id), context,
			                      "unprojected ID unexpectedly has a runtime identity"))
			{
				return false;
			}
		}
	}
	if(!replayExpect(projection.mapping.entries().size() == expected,
	                 context, "mapping entry count does not cover every typed ID")) return false;
	for(std::uint32_t index = 0; index < projection.asset.meshes.size(); ++index)
	{
		const auto owner = projection.mapping.documentIdAt(EditorObjectKind::mesh, index);
		const auto identity = owner
			? projection.mapping.runtimeIndex(*owner) : std::nullopt;
		if(!replayExpect(owner && identity
				&& projection.asset.meshes[index].runtimeIdentity
					== *identity,
			context, "projected mesh occurrence lacks a stable reverse/runtime mapping")) return false;
	}
	for(std::uint32_t index = 0; index < projection.asset.primitives.size(); ++index)
	{
		const auto owner = projection.mapping.documentIdAt(
			EditorObjectKind::primitive, index);
		const auto identity = owner
			? projection.mapping.runtimeIndex(*owner) : std::nullopt;
		if(!replayExpect(owner && identity
				&& projection.asset.primitives[index].runtimeIdentity
					== *identity,
			context, "projected primitive occurrence lacks a stable reverse/runtime mapping")) return false;
	}
	return true;
}

bool validateCompleteState(GltfDocument& document, const ReplayContext& context)
{
	const auto validation = GltfValidator::validate(document);
	if(!replayExpect(validation.valid(), context,
	                 validation.issues().empty()
		                 ? "authority validation failed"
		                 : validation.issues().front().path + ": "
			                 + validation.issues().front().message)) return false;
	for(std::size_t kind = 1u;
		kind < static_cast<std::size_t>(EditorObjectKind::count); ++kind)
	{
		const auto type = static_cast<EditorObjectKind>(kind);
		std::vector<EditorObjectId> ids;
		for(std::size_t index = 0; index < document.objectCount(type); ++index)
		{
			const EditorObjectId id = document.idAt(type, index);
			if(!replayExpect(id.kind == type && validTypedId(id)
				&& document.contains(id) && document.indexOf(id) == index,
				context, "typed ID kind/authority/index round-trip failed")) return false;
			ids.push_back(id);
		}
		std::sort(ids.begin(), ids.end());
		if(!replayExpect(std::adjacent_find(ids.begin(), ids.end()) == ids.end(),
		                 context, "typed ID registry contains a duplicate")) return false;
	}

	std::vector<SceneId> scenes;
	std::vector<std::vector<NodeId>> sceneHierarchies;
	for(std::size_t sceneIndex = 0;
		sceneIndex < document.objectCount(EditorObjectKind::scene); ++sceneIndex)
	{
		const SceneId scene = *SceneId::from(
			document.idAt(EditorObjectKind::scene, sceneIndex));
		scenes.push_back(scene);
		std::vector<NodeId> stack = document.sceneRootNodeIds(scene);
		std::vector<NodeId> visited;
		while(!stack.empty())
		{
			const NodeId node = stack.back();
			stack.pop_back();
			if(!replayExpect(document.contains(node.untyped()), context,
			                 "scene hierarchy contains a stale descendant")) return false;
			if(!replayExpect(std::find(visited.begin(), visited.end(), node) == visited.end(),
			                 context, "scene hierarchy contains a cycle or duplicate occurrence")) return false;
			visited.push_back(node);
			const auto children = document.nodeChildIds(node);
			for(auto child = children.rbegin(); child != children.rend(); ++child)
				stack.push_back(*child);
		}
		sceneHierarchies.push_back(std::move(visited));
	}

	const SceneId originalScene = document.currentSceneId();
	const std::uint64_t revision = document.revision();
	const std::string digest = GltfSemanticCompare::digest(document);
	bool passed = true;
	for(std::size_t sceneNumber = 0; sceneNumber < scenes.size(); ++sceneNumber)
	{
		const SceneId scene = scenes[sceneNumber];
		if(!document.selectCurrentScene(scene))
		{
			passed = replayExpect(false, context, "could not select a live scene for projection");
			break;
		}
		const auto projection = SceneProjection::project(document);
		if(!replayExpect(projection.succeeded(), context,
		                 "fresh scene projection failed: " + projection.error)
			|| !replayExpect(projection.documentRevision == revision
				&& projection.asset.documentRevision == revision,
				context, "projection revision does not match authority")
			|| !replayExpect(projection.semanticHash == digest,
				context, "projection semantic hash does not match authority")
			|| !validateCompleteMapping(document, projection, context))
		{
			passed = false;
			break;
		}
		const auto roots = document.sceneRootNodeIds(scene);
		if(!replayExpect(projection.asset.rootNodes.size() == roots.size(),
		                 context, "projection root count differs from the selected scene"))
		{
			passed = false;
			break;
		}
		if(!replayExpect(projection.asset.nodes.size() == sceneHierarchies[sceneNumber].size(),
		                 context, "projection does not contain the full descendant hierarchy"))
		{
			passed = false;
			break;
		}
		for(const NodeId parent : sceneHierarchies[sceneNumber])
		{
			const auto projectedParent = projection.mapping.sceneAssetIndex(parent.untyped());
			if(!replayExpect(projectedParent
				&& *projectedParent < projection.asset.nodes.size(), context,
				"projected hierarchy omits a reachable document node"))
			{
				passed = false;
				break;
			}
			const auto& projectedChildren = projection.asset.nodes[*projectedParent].children;
			const auto authorityChildren = document.nodeChildIds(parent);
			if(!replayExpect(projectedChildren.size() == authorityChildren.size(), context,
				"projected descendant edge count differs from authority"))
			{
				passed = false;
				break;
			}
			for(const NodeId child : authorityChildren)
			{
				const auto projectedChild = projection.mapping.sceneAssetIndex(child.untyped());
				if(!replayExpect(projectedChild
					&& std::find(projectedChildren.begin(), projectedChildren.end(),
						*projectedChild) != projectedChildren.end(), context,
					"projected hierarchy is missing an authority parent/child edge"))
				{
					passed = false;
					break;
				}
			}
			if(!passed) break;
		}
		if(!passed) break;
		for(const NodeId root : roots)
		{
			const auto index = projection.mapping.sceneAssetIndex(root.untyped());
			if(!replayExpect(index && std::find(
				projection.asset.rootNodes.begin(), projection.asset.rootNodes.end(), *index)
					!= projection.asset.rootNodes.end(),
				context, "scene root is missing from the fresh projection"))
			{
				passed = false;
				break;
			}
		}
		if(!passed) break;
	}
	if(originalScene.valid())
		passed &= replayExpect(document.selectCurrentScene(originalScene), context,
		                       "could not restore the original current scene");
	passed &= replayExpect(document.revision() == revision
		&& GltfSemanticCompare::digest(document) == digest,
		context, "projection validation changed semantic authority");
	return passed;
}

struct IndependentIdOracle final
{
	std::array<std::vector<EditorObjectId>,
		static_cast<std::size_t>(EditorObjectKind::count)> live;

	explicit IndependentIdOracle(const GltfDocument& document)
	{
		for(std::size_t kind = 1u;
			kind < static_cast<std::size_t>(EditorObjectKind::count); ++kind)
		{
			const auto type = static_cast<EditorObjectKind>(kind);
			for(std::size_t index = 0; index < document.objectCount(type); ++index)
				live[kind].push_back(document.idAt(type, index));
			std::sort(live[kind].begin(), live[kind].end());
		}
	}

	bool accept(const MutationResult& result, const ReplayContext& context)
	{
		if(!result.applied() || result.afterRevision == result.beforeRevision) return true;
		for(std::size_t kind = 1u;
			kind < static_cast<std::size_t>(EditorObjectKind::count); ++kind)
		{
			const auto type = static_cast<EditorObjectKind>(kind);
			const auto& entries = result.remap.forKind(type);
			std::vector<EditorObjectId> oldIds;
			std::vector<EditorObjectId> nextIds;
			for(const auto& entry : entries)
			{
				if(entry.oldId.valid()) oldIds.push_back(entry.oldId);
				if(entry.newId.valid()) nextIds.push_back(entry.newId);
			}
			std::sort(oldIds.begin(), oldIds.end());
			std::sort(nextIds.begin(), nextIds.end());
			if(!replayExpect(oldIds == live[kind], context,
			                 "mutation remap does not describe the independent pre-state ID set"))
			{
				return false;
			}
			if(!replayExpect(std::adjacent_find(nextIds.begin(), nextIds.end()) == nextIds.end(),
			                 context, "mutation remap produces duplicate live IDs")) return false;
			live[kind] = std::move(nextIds);
		}
		return true;
	}

	bool verify(const GltfDocument& document, const ReplayContext& context) const
	{
		for(std::size_t kind = 1u;
			kind < static_cast<std::size_t>(EditorObjectKind::count); ++kind)
		{
			const auto type = static_cast<EditorObjectKind>(kind);
			std::vector<EditorObjectId> actual;
			for(std::size_t index = 0; index < document.objectCount(type); ++index)
				actual.push_back(document.idAt(type, index));
			std::sort(actual.begin(), actual.end());
			if(!replayExpect(actual == live[kind], context,
			                 "authority ID set diverges from independent remap oracle")) return false;
		}
		return true;
	}
};

template <typename Id>
std::optional<Id> typedAt(
	const GltfDocument& document,
	EditorObjectKind kind,
	std::uint32_t selector)
{
	const std::size_t count = document.objectCount(kind);
	if(count == 0u) return std::nullopt;
	return Id::from(document.idAt(kind, selector % count));
}

std::optional<EditorObjectId> addedId(
	const MutationResult& result,
	EditorObjectKind kind)
{
	for(const auto& entry : result.remap.forKind(kind))
		if(!entry.oldId.valid() && entry.newId.valid()) return entry.newId;
	return std::nullopt;
}

bool sameMaterial(
	const std::optional<MaterialProperties>& actual,
	const MaterialProperties& expected)
{
	return actual
		&& actual->baseColorFactor == expected.baseColorFactor
		&& actual->metallicFactor == expected.metallicFactor
		&& actual->roughnessFactor == expected.roughnessFactor
		&& actual->emissiveFactor == expected.emissiveFactor
		&& actual->alphaCutoff == expected.alphaCutoff
		&& actual->alphaMode == expected.alphaMode
		&& actual->doubleSided == expected.doubleSided;
}

bool sameCamera(
	const std::optional<CameraProperties>& actual,
	const CameraProperties& expected)
{
	return actual && actual->type == expected.type && actual->yfov == expected.yfov
		&& actual->znear == expected.znear && actual->zfar == expected.zfar
		&& actual->aspectRatio == expected.aspectRatio && actual->xmag == expected.xmag
		&& actual->ymag == expected.ymag;
}

enum class ExpectedEffectKind : std::uint8_t
{
	none,
	addScene,
	deleteScene,
	addNode,
	duplicateNode,
	deleteObject,
	reparent,
	rename,
	transform,
	meshAttachment,
	cameraAttachment,
	lightAttachment,
	visibility,
	removeVisibility,
	material,
	materialTexture,
	materialExtension,
	primitiveMaterial,
	primitiveDuplicate,
	primitiveSplit,
	meshMerge,
	camera,
	light,
	createLight,
	variant,
	rootPreview,
	rootReset,
};

struct OperationExecution final
{
	MutationResult result;
	std::string description;
	ExpectedEffectKind effect{ExpectedEffectKind::none};
	EditorObjectId target;
	EditorObjectId reference;
	std::string name;
	bool boolean{false};
	std::size_t beforePrimaryCount{0};
	std::size_t beforeSecondaryCount{0};
	std::size_t beforeTertiaryCount{0};
	std::optional<MutationErrorCode> expectedError;
	NodeTransform transform;
	MaterialProperties material;
	MaterialTextureProperties materialTexture;
	MaterialTextureSlot materialTextureSlot{MaterialTextureSlot::baseColor};
	MaterialExtensionValue materialExtension;
	CameraProperties camera;
	LightProperties light;
	std::optional<MaterialId> variantMaterial;
	std::uint32_t variant{0};
	SceneRootTransform rootTransform;
	HistoryPolicy expectedPolicy{HistoryPolicy::unclassified};
};

enum class ReplayOperation : std::uint8_t
{
	query,
	staleGuard,
	addScene,
	deleteScene,
	addNode,
	duplicateNode,
	reparentNode,
	duplicateOccurrence,
	reparentOccurrence,
	deleteOccurrence,
	rename,
	transform,
	attachMesh,
	attachCamera,
	attachLight,
	setVisibility,
	removeVisibility,
	setMaterial,
	setMaterialTexture,
	removeMaterialTexture,
	setMaterialExtension,
	removeMaterialExtension,
	rejectMaterialExtension,
	assignPrimitiveMaterial,
	duplicatePrimitive,
	splitPrimitive,
	mergeMeshes,
	setCamera,
	setLight,
	createLight,
	deleteLight,
	setVariant,
	removeVariant,
	previewRoot,
	resetRoot,
	deleteNode,
	count,
};

constexpr std::array<std::string_view,
	static_cast<std::size_t>(ReplayOperation::count)> kReplayOperationNames{
	"query", "stale_guard", "add_scene", "delete_scene", "add_node",
	"duplicate_node", "reparent_node", "duplicate_occurrence",
	"reparent_occurrence", "delete_occurrence", "rename", "transform",
	"attach_mesh", "attach_camera", "attach_light", "set_visibility",
	"remove_visibility", "set_material", "set_material_texture",
	"remove_material_texture", "set_material_extension",
	"remove_material_extension", "reject_material_extension",
	"assign_primitive_material", "duplicate_primitive", "split_primitive",
	"merge_meshes", "set_camera", "set_light", "create_light", "delete_light",
	"set_variant", "remove_variant", "preview_root", "reset_root", "delete_node",
};

constexpr HistoryPolicy replayHistoryPolicy(ReplayOperation operation)
{
	switch(operation)
	{
		case ReplayOperation::query:
		case ReplayOperation::staleGuard:
			return HistoryPolicy::notApplicable;
		case ReplayOperation::duplicateNode:
		case ReplayOperation::reparentNode:
		case ReplayOperation::duplicateOccurrence:
		case ReplayOperation::reparentOccurrence:
		case ReplayOperation::deleteOccurrence:
		case ReplayOperation::rename:
		case ReplayOperation::transform:
		case ReplayOperation::setMaterial:
		case ReplayOperation::setMaterialTexture:
		case ReplayOperation::removeMaterialTexture:
		case ReplayOperation::setMaterialExtension:
		case ReplayOperation::removeMaterialExtension:
		case ReplayOperation::rejectMaterialExtension:
		case ReplayOperation::setLight:
		case ReplayOperation::createLight:
		case ReplayOperation::deleteNode:
			return HistoryPolicy::undoable;
		case ReplayOperation::addScene:
		case ReplayOperation::deleteScene:
		case ReplayOperation::addNode:
		case ReplayOperation::attachMesh:
		case ReplayOperation::attachCamera:
		case ReplayOperation::attachLight:
		case ReplayOperation::setVisibility:
		case ReplayOperation::removeVisibility:
		case ReplayOperation::assignPrimitiveMaterial:
		case ReplayOperation::duplicatePrimitive:
		case ReplayOperation::splitPrimitive:
		case ReplayOperation::mergeMeshes:
		case ReplayOperation::setCamera:
		case ReplayOperation::deleteLight:
		case ReplayOperation::setVariant:
		case ReplayOperation::removeVariant:
		case ReplayOperation::previewRoot:
		case ReplayOperation::resetRoot:
			return HistoryPolicy::nonHistory;
		case ReplayOperation::count:
			break;
	}
	return HistoryPolicy::unclassified;
}

bool sameRootTransform(
	const SceneRootTransform& left,
	const SceneRootTransform& right)
{
	return left.translation == right.translation && left.rotation == right.rotation
		&& left.scale == right.scale;
}

bool remapEmpty(const MutationRemap& remap)
{
	for(std::size_t kind = 1u;
		kind < static_cast<std::size_t>(EditorObjectKind::count); ++kind)
	{
		if(!remap.forKind(static_cast<EditorObjectKind>(kind)).empty()) return false;
	}
	return true;
}

std::optional<EditorObjectId> anyLiveObject(
	const GltfDocument& document,
	std::uint32_t selector)
{
	constexpr std::size_t count = static_cast<std::size_t>(EditorObjectKind::count) - 1u;
	for(std::size_t offset = 0; offset < count; ++offset)
	{
		const auto kind = static_cast<EditorObjectKind>(
			1u + (static_cast<std::size_t>(selector) + offset) % count);
		if(document.objectCount(kind) != 0u)
			return document.idAt(kind, selector % document.objectCount(kind));
	}
	return std::nullopt;
}

std::optional<SceneNodeOccurrence> replayOccurrence(
	SceneCommands& commands,
	const GltfDocument& document,
	std::uint32_t selector,
	bool requireReplayName)
{
	const std::size_t sceneCount = document.objectCount(EditorObjectKind::scene);
	for(std::size_t sceneOffset = 0; sceneOffset < sceneCount; ++sceneOffset)
	{
		const SceneId scene = *SceneId::from(document.idAt(
			EditorObjectKind::scene, (selector + sceneOffset) % sceneCount));
		std::vector<NodeId> stack = document.sceneRootNodeIds(scene);
		std::reverse(stack.begin(), stack.end());
		while(!stack.empty())
		{
			const NodeId node = stack.back();
			stack.pop_back();
			const std::string_view name = document.objectName(node.untyped());
			if(!requireReplayName || name.starts_with("Replay"))
			{
				if(const auto occurrence = commands.captureNodeOccurrence(scene, node))
					return occurrence;
			}
			const auto children = document.nodeChildIds(node);
			for(auto child = children.rbegin(); child != children.rend(); ++child)
				stack.push_back(*child);
		}
	}
	return std::nullopt;
}

struct RootPair final
{
	SceneId scene;
	NodeId first;
	NodeId second;
};

std::optional<RootPair> twoRoots(
	const GltfDocument& document,
	std::uint32_t selector)
{
	const std::size_t sceneCount = document.objectCount(EditorObjectKind::scene);
	for(std::size_t offset = 0; offset < sceneCount; ++offset)
	{
		const SceneId scene = *SceneId::from(document.idAt(
			EditorObjectKind::scene, (selector + offset) % sceneCount));
		const auto roots = document.sceneRootNodeIds(scene);
		if(roots.size() >= 2u)
			return RootPair{scene, roots.front(), roots.back()};
	}
	return std::nullopt;
}

MaterialExtensionValue clearcoatValue(double factor)
{
	MaterialExtensionValue value;
	MaterialExtensionValue member;
	member.value = factor;
	value.value = MaterialExtensionValue::Object{
		{"clearcoatFactor", std::move(member)}};
	return value;
}

std::optional<std::pair<NodeId, PrimitiveId>> nodePrimitive(
	const GltfDocument& document,
	const SceneCommands& commands,
	std::uint32_t selector)
{
	const std::size_t nodeCount = document.objectCount(EditorObjectKind::node);
	for(std::size_t offset = 0; offset < nodeCount; ++offset)
	{
		const NodeId node = *NodeId::from(document.idAt(
			EditorObjectKind::node, (selector + offset) % nodeCount));
		const auto mesh = commands.nodeMesh(node);
		if(!mesh) continue;
		const auto primitives = commands.meshPrimitives(*mesh);
		if(!primitives.empty()) return std::pair{node, primitives.front()};
	}
	return std::nullopt;
}

OperationExecution executeOperation(
	SceneCommands& commands,
	GltfDocument& document,
	ReplayOperation operation,
	std::uint32_t selector,
	std::uint32_t seed,
	std::size_t step)
{
	OperationExecution execution;
	execution.description = std::string(
		kReplayOperationNames[static_cast<std::size_t>(operation)]);
	execution.expectedPolicy = replayHistoryPolicy(operation);
	const auto scene = typedAt<SceneId>(document, EditorObjectKind::scene, 0u);
	const auto node = typedAt<NodeId>(document, EditorObjectKind::node, 0u);
	const auto secondNode = typedAt<NodeId>(document, EditorObjectKind::node, 1u);
	const auto mesh = typedAt<MeshId>(document, EditorObjectKind::mesh, 0u);
	const auto material = typedAt<MaterialId>(document, EditorObjectKind::material, 0u);
	const auto otherMaterial = typedAt<MaterialId>(document, EditorObjectKind::material, 1u);
	const auto texture = typedAt<TextureId>(document, EditorObjectKind::texture, 0u);
	const auto camera = typedAt<CameraId>(document, EditorObjectKind::camera, 0u);
	const auto light = typedAt<LightId>(document, EditorObjectKind::light, 0u);
	const auto primitive = typedAt<PrimitiveId>(
		document, EditorObjectKind::primitive, selector);

	switch(operation)
	{
		case ReplayOperation::query:
		{
			execution.target = anyLiveObject(document, selector).value_or(EditorObjectId{});
			execution.result = commands.query(execution.target);
			break;
		}
		case ReplayOperation::staleGuard:
		{
			execution.target = anyLiveObject(document, selector).value_or(EditorObjectId{});
			execution.expectedError = MutationErrorCode::staleRevision;
			execution.result = commands.query(
				execution.target, MutationGuard{document.revision() + 1u, std::nullopt});
			break;
		}
		case ReplayOperation::addScene:
		{
			execution.effect = ExpectedEffectKind::addScene;
			execution.beforePrimaryCount = document.objectCount(EditorObjectKind::scene);
			execution.name = "ReplayScene_" + std::to_string(seed) + '_' + std::to_string(step);
			execution.result = commands.addScene(execution.name);
			break;
		}
		case ReplayOperation::deleteScene:
		{
			execution.effect = ExpectedEffectKind::deleteScene;
			execution.beforePrimaryCount = document.objectCount(EditorObjectKind::scene);
			if(execution.beforePrimaryCount > 2u)
				execution.target =
					document.idAt(EditorObjectKind::scene, execution.beforePrimaryCount - 1u);
			execution.result =
				commands.deleteScene(SceneId::from(execution.target).value_or(SceneId{}));
			break;
		}
		case ReplayOperation::addNode:
		{
			execution.effect = ExpectedEffectKind::addNode;
			execution.beforePrimaryCount = document.objectCount(EditorObjectKind::node);
			execution.name = "ReplayNode_" + std::to_string(seed) + '_' + std::to_string(step);
			execution.result = commands.addNode(scene.value_or(SceneId{}), execution.name);
			break;
		}
		case ReplayOperation::duplicateNode:
		{
			execution.effect = ExpectedEffectKind::duplicateNode;
			execution.beforePrimaryCount = document.objectCount(EditorObjectKind::node);
			const auto occurrence = replayOccurrence(commands, document, selector, true);
			const NodeId source = occurrence ? occurrence->node : node.value_or(NodeId{});
			execution.target = source.untyped();
			execution.result = commands.duplicateNode(source);
			break;
		}
		case ReplayOperation::reparentNode:
		{
			execution.effect = ExpectedEffectKind::reparent;
			if(const auto roots = twoRoots(document, selector))
			{
				execution.target = roots->second.untyped();
				execution.reference = roots->first.untyped();
				execution.boolean = true;
				execution.result = commands.reparentNode(roots->second, roots->first);
			}
			else
			{
				execution.target = node ? node->untyped() : EditorObjectId{};
				execution.result = commands.reparentNode(node.value_or(NodeId{}), std::nullopt);
			}
			break;
		}
		case ReplayOperation::duplicateOccurrence:
		{
			execution.effect = ExpectedEffectKind::duplicateNode;
			execution.beforePrimaryCount = document.objectCount(EditorObjectKind::node);
			const auto source = replayOccurrence(commands, document, selector, true)
				.value_or(replayOccurrence(commands, document, selector, false)
					.value_or(SceneNodeOccurrence{}));
			std::optional<SceneNodeOccurrence> parent;
			if(source.scene.valid())
			{
				const auto roots = document.sceneRootNodeIds(source.scene);
				if(!roots.empty() && roots.front() != source.node)
					parent = commands.captureNodeOccurrence(source.scene, roots.front());
			}
			execution.target = source.node.untyped();
			execution.result = commands.duplicateNodeAt(source, parent,
				(selector & 1u) ? SceneOccurrencePolicy::allScenes
					: SceneOccurrencePolicy::clickedScene);
			break;
		}
		case ReplayOperation::reparentOccurrence:
		{
			execution.effect = ExpectedEffectKind::reparent;
			const auto source = replayOccurrence(commands, document, selector, true)
				.value_or(replayOccurrence(commands, document, selector, false)
					.value_or(SceneNodeOccurrence{}));
			std::optional<SceneNodeOccurrence> parent;
			if(source.scene.valid())
			{
				const auto roots = document.sceneRootNodeIds(source.scene);
				if(!roots.empty() && roots.front() != source.node)
					parent = commands.captureNodeOccurrence(source.scene, roots.front());
			}
			execution.target = source.node.untyped();
			if(parent)
			{
				execution.reference = parent->node.untyped();
				execution.boolean = true;
			}
			execution.result = commands.reparentNodeAt(source, parent,
				(selector & 1u) ? SceneOccurrencePolicy::allScenes
					: SceneOccurrencePolicy::clickedScene);
			break;
		}
		case ReplayOperation::deleteOccurrence:
		{
			execution.effect = ExpectedEffectKind::deleteObject;
			execution.beforePrimaryCount = document.objectCount(EditorObjectKind::node);
			const auto occurrence = replayOccurrence(commands, document, selector, true)
				.value_or(replayOccurrence(commands, document, selector, false)
					.value_or(SceneNodeOccurrence{}));
			execution.target = occurrence.node.untyped();
			execution.result = commands.deleteNodeAt(occurrence,
				(selector & 1u) ? SceneOccurrencePolicy::allScenes
					: SceneOccurrencePolicy::clickedScene);
			break;
		}
		case ReplayOperation::rename:
		{
			execution.effect = ExpectedEffectKind::rename;
			execution.target = node ? node->untyped() : EditorObjectId{};
			execution.name = "ReplayRenamed_" + std::to_string(seed) + '_'
				+ std::to_string(step);
			execution.result = commands.rename(execution.target, execution.name);
			break;
		}
		case ReplayOperation::transform:
		{
			execution.effect = ExpectedEffectKind::transform;
			execution.target = node ? node->untyped() : EditorObjectId{};
			execution.transform.translation = {
				double((selector % 9u) + 1u), double((selector >> 4u) % 5u), 0.0};
			execution.result = commands.setNodeTransform(
				node.value_or(NodeId{}), execution.transform);
			break;
		}
		case ReplayOperation::attachMesh:
		{
			execution.effect = ExpectedEffectKind::meshAttachment;
			const auto occurrence = replayOccurrence(commands, document, selector, false);
			const NodeId target = step < static_cast<std::size_t>(ReplayOperation::count)
				? secondNode.value_or(node.value_or(NodeId{}))
				: (occurrence ? occurrence->node
					: secondNode.value_or(node.value_or(NodeId{})));
			execution.target = target.untyped();
			execution.reference = mesh ? mesh->untyped() : EditorObjectId{};
			execution.result = commands.attachMesh(target, mesh.value_or(MeshId{}));
			break;
		}
		case ReplayOperation::attachCamera:
		{
			execution.effect = ExpectedEffectKind::cameraAttachment;
			const auto occurrence = replayOccurrence(commands, document, selector, false);
			const NodeId target = step < static_cast<std::size_t>(ReplayOperation::count)
				? secondNode.value_or(node.value_or(NodeId{}))
				: (occurrence ? occurrence->node
					: secondNode.value_or(node.value_or(NodeId{})));
			execution.target = target.untyped();
			execution.reference = camera ? camera->untyped() : EditorObjectId{};
			execution.boolean = camera.has_value();
			execution.result = commands.attachCamera(target, camera);
			break;
		}
		case ReplayOperation::attachLight:
		{
			execution.effect = ExpectedEffectKind::lightAttachment;
			const auto occurrence = replayOccurrence(commands, document, selector, false);
			const NodeId target = step < static_cast<std::size_t>(ReplayOperation::count)
				? secondNode.value_or(node.value_or(NodeId{}))
				: (occurrence ? occurrence->node
					: secondNode.value_or(node.value_or(NodeId{})));
			execution.target = target.untyped();
			execution.reference = light ? light->untyped() : EditorObjectId{};
			execution.boolean = light.has_value();
			execution.result = commands.attachLight(target, light);
			break;
		}
		case ReplayOperation::setVisibility:
		{
			execution.effect = ExpectedEffectKind::visibility;
			execution.target = node ? node->untyped() : EditorObjectId{};
			execution.boolean = (selector & 1u) != 0u;
			execution.result = commands.setVisibility(
				execution.target, execution.boolean);
			break;
		}
		case ReplayOperation::removeVisibility:
		{
			execution.effect = ExpectedEffectKind::removeVisibility;
			execution.target = node ? node->untyped() : EditorObjectId{};
			execution.result = commands.removeVisibility(node.value_or(NodeId{}));
			break;
		}
		case ReplayOperation::setMaterial:
		{
			execution.effect = ExpectedEffectKind::material;
			execution.target = material ? material->untyped() : EditorObjectId{};
			execution.material.baseColorFactor = {
				0.2 + 0.1 * double(selector % 4u), 0.3, 0.4, 1.0};
			execution.material.metallicFactor = 0.25;
			execution.material.roughnessFactor = 0.75;
			execution.material.emissiveFactor = {0.1, 0.2, 0.3};
			execution.material.alphaMode = "MASK";
			execution.material.alphaCutoff = 0.4;
			execution.material.doubleSided = true;
			execution.result = commands.setMaterial(
				material.value_or(MaterialId{}), execution.material);
			break;
		}
		case ReplayOperation::setMaterialTexture:
		{
			execution.effect = ExpectedEffectKind::materialTexture;
			execution.target = material ? material->untyped() : EditorObjectId{};
			execution.materialTexture.texture = texture;
			execution.materialTexture.texCoord = selector % 2u;
			execution.boolean = true;
			execution.result = commands.setMaterialTexture(
				material.value_or(MaterialId{}), execution.materialTextureSlot,
				execution.materialTexture);
			break;
		}
		case ReplayOperation::removeMaterialTexture:
		{
			execution.effect = ExpectedEffectKind::materialTexture;
			execution.target = material ? material->untyped() : EditorObjectId{};
			execution.result = commands.setMaterialTexture(
				material.value_or(MaterialId{}), execution.materialTextureSlot,
				execution.materialTexture);
			break;
		}
		case ReplayOperation::setMaterialExtension:
		{
			execution.effect = ExpectedEffectKind::materialExtension;
			execution.target = material ? material->untyped() : EditorObjectId{};
			execution.materialExtension = clearcoatValue(
				(selector & 1u) ? 0.25 : 0.75);
			execution.name = "KHR_materials_clearcoat";
			execution.boolean = true;
			execution.result = commands.setMaterialExtension(
				material.value_or(MaterialId{}), execution.name,
				execution.materialExtension);
			break;
		}
		case ReplayOperation::removeMaterialExtension:
		{
			execution.effect = ExpectedEffectKind::materialExtension;
			execution.target = material ? material->untyped() : EditorObjectId{};
			execution.name = "KHR_materials_clearcoat";
			execution.result = commands.setMaterialExtension(
				material.value_or(MaterialId{}), execution.name, std::nullopt);
			break;
		}
		case ReplayOperation::rejectMaterialExtension:
		{
			execution.target = material ? material->untyped() : EditorObjectId{};
			execution.name = "KHR_materials_clearcoat";
			execution.materialExtension = clearcoatValue(-1.0);
			execution.expectedError = MutationErrorCode::validationFailed;
			execution.result = commands.setMaterialExtension(
				material.value_or(MaterialId{}), execution.name,
				execution.materialExtension);
			break;
		}
		case ReplayOperation::assignPrimitiveMaterial:
		{
			execution.effect = ExpectedEffectKind::primitiveMaterial;
			execution.target = primitive ? primitive->untyped() : EditorObjectId{};
			execution.reference = otherMaterial
				? otherMaterial->untyped() : EditorObjectId{};
			execution.boolean = otherMaterial.has_value();
			execution.result = commands.assignPrimitiveMaterial(
				primitive.value_or(PrimitiveId{}), otherMaterial);
			break;
		}
		case ReplayOperation::duplicatePrimitive:
		{
			execution.effect = ExpectedEffectKind::primitiveDuplicate;
			execution.beforePrimaryCount = document.objectCount(EditorObjectKind::primitive);
			execution.target = primitive ? primitive->untyped() : EditorObjectId{};
			execution.result = commands.duplicatePrimitive(
				primitive.value_or(PrimitiveId{}));
			break;
		}
		case ReplayOperation::splitPrimitive:
		{
			execution.effect = ExpectedEffectKind::primitiveSplit;
			execution.beforePrimaryCount = document.objectCount(EditorObjectKind::mesh);
			execution.beforeSecondaryCount = document.objectCount(EditorObjectKind::material);
			execution.beforeTertiaryCount = document.objectCount(EditorObjectKind::primitive);
			const auto pair = nodePrimitive(document, commands, 0u);
			if(pair)
			{
				execution.target = pair->first.untyped();
				if(const auto oldMesh = commands.nodeMesh(pair->first))
					execution.reference = oldMesh->untyped();
				execution.result = commands.splitPrimitive(pair->first, pair->second);
			}
			else execution.result = commands.splitPrimitive(NodeId{}, PrimitiveId{});
			break;
		}
		case ReplayOperation::mergeMeshes:
		{
			execution.effect = ExpectedEffectKind::meshMerge;
			execution.beforePrimaryCount = document.objectCount(EditorObjectKind::mesh);
			const auto pair = nodePrimitive(document, commands, 0u);
			const NodeId target = pair ? pair->first : NodeId{};
			execution.target = target.untyped();
			if(const auto oldMesh = commands.nodeMesh(target))
				execution.reference = oldMesh->untyped();
			execution.result = commands.mergeMeshes(target);
			break;
		}
		case ReplayOperation::setCamera:
		{
			execution.effect = ExpectedEffectKind::camera;
			execution.target = camera ? camera->untyped() : EditorObjectId{};
			execution.camera = camera
				? commands.cameraProperties(*camera).value_or(CameraProperties{})
				: CameraProperties{};
			execution.camera.type = (selector & 1u) ? "perspective" : "orthographic";
			execution.camera.znear = 0.1;
			execution.camera.zfar = 50.0;
			if(execution.camera.type == "perspective")
			{
				execution.camera.yfov = 0.6;
				execution.camera.aspectRatio = 1.5;
			}
			else
			{
				execution.camera.xmag = 2.0;
				execution.camera.ymag = 1.5;
			}
			execution.result = commands.setCamera(
				camera.value_or(CameraId{}), execution.camera);
			break;
		}
		case ReplayOperation::setLight:
		{
			execution.effect = ExpectedEffectKind::light;
			execution.target = light ? light->untyped() : EditorObjectId{};
			execution.light.type = "spot";
			execution.light.color = {0.4, 0.6, 0.8};
			execution.light.intensity = 3.0 + double(selector % 3u);
			execution.light.range = 12.0;
			execution.light.innerConeAngle = 0.2;
			execution.light.outerConeAngle = 0.6;
			execution.result = commands.setLight(
				light.value_or(LightId{}), execution.light);
			break;
		}
		case ReplayOperation::createLight:
		{
			execution.effect = ExpectedEffectKind::createLight;
			execution.beforePrimaryCount = document.objectCount(EditorObjectKind::light);
			execution.beforeSecondaryCount = document.objectCount(EditorObjectKind::node);
			execution.light.type = "point";
			execution.light.intensity = 2.0;
			execution.light.range = 8.0;
			execution.name = "ReplayLight_" + std::to_string(seed) + '_'
				+ std::to_string(step);
			execution.result = commands.createLight(scene.value_or(SceneId{}), std::nullopt,
				"ReplayLightNode_" + std::to_string(seed) + '_' + std::to_string(step),
				execution.name, execution.light);
			break;
		}
		case ReplayOperation::deleteLight:
		{
			execution.effect = ExpectedEffectKind::deleteObject;
			execution.beforePrimaryCount = document.objectCount(EditorObjectKind::light);
			if(execution.beforePrimaryCount > 1u)
				execution.target = document.idAt(
					EditorObjectKind::light, execution.beforePrimaryCount - 1u);
			execution.result = commands.deleteLight(
				LightId::from(execution.target).value_or(LightId{}));
			break;
		}
		case ReplayOperation::setVariant:
		case ReplayOperation::removeVariant:
		{
			execution.effect = ExpectedEffectKind::variant;
			execution.target = primitive ? primitive->untyped() : EditorObjectId{};
			execution.variant = selector % 2u;
			if(operation == ReplayOperation::setVariant)
				execution.variantMaterial = otherMaterial;
			execution.result = commands.setPrimitiveVariant(
				primitive.value_or(PrimitiveId{}), execution.variant,
				execution.variantMaterial);
			break;
		}
		case ReplayOperation::previewRoot:
		{
			execution.effect = ExpectedEffectKind::rootPreview;
			execution.target = scene ? scene->untyped() : EditorObjectId{};
			execution.rootTransform.translation = {
				double((selector % 7u) + 1u), 0.0, 0.0};
			execution.result = commands.previewSceneRootTransform(
				scene.value_or(SceneId{}), execution.rootTransform);
			break;
		}
		case ReplayOperation::resetRoot:
		{
			execution.effect = ExpectedEffectKind::rootReset;
			execution.target = scene ? scene->untyped() : EditorObjectId{};
			execution.result = commands.resetSceneRootTransform(
				scene.value_or(SceneId{}));
			break;
		}
		case ReplayOperation::deleteNode:
		{
			execution.effect = ExpectedEffectKind::deleteObject;
			execution.beforePrimaryCount = document.objectCount(EditorObjectKind::node);
			const auto occurrence = replayOccurrence(commands, document, selector, true);
			const NodeId target = occurrence ? occurrence->node : NodeId{};
			execution.target = target.untyped();
			execution.result = commands.deleteNode(target);
			break;
		}
		case ReplayOperation::count: break;
	}
	return execution;
}

bool projectedNodeMatchesTransform(
	GltfDocument& document,
	NodeId node,
	const NodeTransform& expected,
	const ReplayContext& context)
{
	const SceneId original = document.currentSceneId();
	bool found = false;
	bool matches = true;
	for(std::size_t index = 0;
		index < document.objectCount(EditorObjectKind::scene); ++index)
	{
		const SceneId scene = *SceneId::from(document.idAt(EditorObjectKind::scene, index));
		if(!document.selectCurrentScene(scene)) continue;
		const auto projection = SceneProjection::project(document);
		const auto nodeIndex = projection.mapping.sceneAssetIndex(node.untyped());
		if(!nodeIndex || *nodeIndex >= projection.asset.nodes.size()) continue;
		found = true;
		const auto& actual = projection.asset.nodes[*nodeIndex];
		matches &= std::abs(double(actual.translation.x) - expected.translation[0]) <= 1.0e-5
			&& std::abs(double(actual.translation.y) - expected.translation[1]) <= 1.0e-5
			&& std::abs(double(actual.translation.z) - expected.translation[2]) <= 1.0e-5;
	}
	if(original.valid()) matches &= document.selectCurrentScene(original);
	return replayExpect(found && matches, context,
		"projected node transform diverges from the operation oracle");
}

bool projectedNodeAttachment(
	GltfDocument& document,
	NodeId node,
	EditorObjectId reference,
	EditorObjectKind kind,
	const ReplayContext& context)
{
	const SceneId original = document.currentSceneId();
	bool foundNode = false;
	bool foundReference = false;
	bool valid = true;
	for(std::size_t index = 0;
		index < document.objectCount(EditorObjectKind::scene); ++index)
	{
		const SceneId scene = *SceneId::from(document.idAt(EditorObjectKind::scene, index));
		if(!document.selectCurrentScene(scene)) continue;
		const auto projection = SceneProjection::project(document);
		const auto projectedNode = projection.mapping.sceneAssetIndex(node.untyped());
		if(!projectedNode) continue;
		foundNode = true;
		const auto projectedReference = projection.mapping.sceneAssetIndex(reference);
		if(!projectedReference) continue;
		if(kind == EditorObjectKind::camera)
		{
			if(*projectedReference < projection.asset.cameras.size()
				&& projection.asset.cameras[*projectedReference].nodeIndex
					== static_cast<std::int32_t>(*projectedNode))
				foundReference = true;
		}
		else if(kind == EditorObjectKind::light)
		{
			if(*projectedReference < projection.asset.lights.size()
				&& projection.asset.lights[*projectedReference].nodeIndex
					== static_cast<std::int32_t>(*projectedNode))
				foundReference = true;
		}
		else valid = false;
	}
	if(original.valid()) valid &= document.selectCurrentScene(original);
	return replayExpect(foundNode && foundReference && valid, context,
		"projection does not expose the requested node attachment");
}

bool verifyResultSemantics(
	const OperationExecution& execution,
	const ExactDocumentSnapshot& before,
	const ExactDocumentSnapshot& after,
	const GltfDocument& document,
	const ReplayContext& context)
{
	const MutationResult& result = execution.result;
	bool passed = true;
	passed &= replayExpect(execution.expectedPolicy != HistoryPolicy::unclassified
		&& result.historyPolicy == execution.expectedPolicy, context,
		"operation result did not preserve its total history policy classification");
	passed &= replayExpect(result.beforeRevision == before.revision, context,
		"result beforeRevision differs from the exact pre-state");
	if(execution.expectedError)
		passed &= replayExpect(result.error == *execution.expectedError, context,
			"operation returned a different typed rejection than the oracle expected");
	if(result.rejected() || result.domainNoop())
	{
		passed &= replayExpect(after == before, context,
			"rejected/domain-noop operation changed the exact document snapshot");
		passed &= replayExpect(result.afterRevision == before.revision
			&& result.changedIds.empty() && remapEmpty(result.remap), context,
			"rejected/domain-noop result reports a revision, ID, or remap side effect");
		return passed;
	}
	passed &= replayExpect(result.applied() && result.error == MutationErrorCode::none,
		context, "operation result has an invalid status/error combination");
	passed &= replayExpect(result.transactionToken.valid()
		&& result.transactionToken.authority == document.documentAuthority(), context,
		"applied operation did not return a live authority transaction token");
	if(result.afterRevision != result.beforeRevision)
	{
		passed &= replayExpect(after.revision == before.revision + 1u
			&& result.afterRevision == after.revision
			&& result.transactionToken.revision == after.revision, context,
			"semantic mutation did not advance exactly one revision");
		passed &= replayExpect(after.digest != before.digest && after.dirty, context,
			"semantic mutation did not change the authority digest/dirty state");
	}
	else
	{
		passed &= replayExpect(after.revision == before.revision
			&& result.afterRevision == before.revision
			&& result.transactionToken.revision == before.revision
			&& after.digest == before.digest, context,
			"runtime-only mutation changed semantic revision or digest");
		passed &= replayExpect(!(after == before), context,
			"applied runtime-only mutation did not change its root-state snapshot");
	}
	return passed;
}

bool verifyAppliedEffect(
	GltfDocument& document,
	SceneCommands& commands,
	const OperationExecution& execution,
	const ReplayContext& context)
{
	if(!execution.result.applied()) return true;
	const auto added = [&](EditorObjectKind kind)
	{
		return addedId(execution.result, kind);
	};
	switch(execution.effect)
	{
		case ExpectedEffectKind::none: return true;
		case ExpectedEffectKind::addScene:
		{
			const auto id = added(EditorObjectKind::scene);
			return replayExpect(document.objectCount(EditorObjectKind::scene)
				== execution.beforePrimaryCount + 1u && id
				&& document.objectName(*id) == execution.name,
				context, "add-scene postcondition diverges from the semantic oracle");
		}
		case ExpectedEffectKind::deleteScene:
			return replayExpect(document.objectCount(EditorObjectKind::scene) + 1u
				== execution.beforePrimaryCount && !document.contains(execution.target),
				context, "delete-scene postcondition diverges from the semantic oracle");
		case ExpectedEffectKind::addNode:
		{
			const auto id = added(EditorObjectKind::node);
			return replayExpect(document.objectCount(EditorObjectKind::node)
				== execution.beforePrimaryCount + 1u && id
				&& document.objectName(*id) == execution.name,
				context, "add-node postcondition diverges from the semantic oracle");
		}
		case ExpectedEffectKind::duplicateNode:
			return replayExpect(document.objectCount(EditorObjectKind::node)
				> execution.beforePrimaryCount && added(EditorObjectKind::node),
				context, "node duplication did not publish fresh node identity");
		case ExpectedEffectKind::deleteObject:
			return replayExpect(!execution.target.valid()
				|| (!document.contains(execution.target)
					&& document.objectCount(execution.target.kind)
						< execution.beforePrimaryCount),
				context, "delete operation retained its targeted stable ID");
		case ExpectedEffectKind::reparent:
		{
			if(!execution.boolean) return true;
			const auto parent = NodeId::from(execution.reference);
			const auto target = NodeId::from(execution.target);
			const auto children = parent ? document.nodeChildIds(*parent) : std::vector<NodeId>{};
			return replayExpect(target && std::find(children.begin(), children.end(), *target)
				!= children.end(), context,
				"reparent operation did not install the expected direct parent edge");
		}
		case ExpectedEffectKind::rename:
			return replayExpect(document.contains(execution.target)
				&& document.objectName(execution.target) == execution.name,
				context, "rename postcondition diverges from the semantic oracle");
		case ExpectedEffectKind::transform:
		{
			const auto target = NodeId::from(execution.target);
			return target && projectedNodeMatchesTransform(
				document, *target, execution.transform, context);
		}
		case ExpectedEffectKind::meshAttachment:
		{
			const auto target = NodeId::from(execution.target);
			const auto reference = MeshId::from(execution.reference);
			return replayExpect(target && reference
				&& commands.nodeMesh(*target) == *reference, context,
				"mesh attachment query differs from the semantic oracle");
		}
		case ExpectedEffectKind::cameraAttachment:
		{
			const auto target = NodeId::from(execution.target);
			return target && projectedNodeAttachment(document, *target,
				execution.reference, EditorObjectKind::camera, context);
		}
		case ExpectedEffectKind::lightAttachment:
		{
			const auto target = NodeId::from(execution.target);
			return target && projectedNodeAttachment(document, *target,
				execution.reference, EditorObjectKind::light, context);
		}
		case ExpectedEffectKind::visibility:
			return replayExpect(document.objectHasVisibility(execution.target)
				&& document.objectVisible(execution.target) == execution.boolean,
				context, "visibility authority differs from the semantic oracle");
		case ExpectedEffectKind::removeVisibility:
			return replayExpect(!document.objectHasVisibility(execution.target), context,
				"visibility removal left authored extension state");
		case ExpectedEffectKind::material:
		{
			const auto target = MaterialId::from(execution.target);
			return replayExpect(target
				&& sameMaterial(commands.materialProperties(*target), execution.material),
				context, "material properties differ from the semantic oracle");
		}
		case ExpectedEffectKind::materialTexture:
		{
			const auto target = MaterialId::from(execution.target);
			const auto actual = target
				? commands.materialTexture(*target, execution.materialTextureSlot)
				: std::nullopt;
			return replayExpect(actual && (execution.boolean
				? *actual == execution.materialTexture : !actual->texture), context,
				"material texture slot differs from the semantic oracle");
		}
		case ExpectedEffectKind::materialExtension:
		{
			const auto target = MaterialId::from(execution.target);
			const auto actual = target
				? commands.materialExtension(*target, execution.name) : std::nullopt;
			return replayExpect(execution.boolean
				? actual == std::optional<MaterialExtensionValue>{execution.materialExtension}
				: !actual,
				context, "material extension differs from the semantic oracle");
		}
		case ExpectedEffectKind::primitiveMaterial:
		{
			const auto target = PrimitiveId::from(execution.target);
			const auto reference = MaterialId::from(execution.reference);
			return replayExpect(target && commands.primitiveMaterial(*target)
				== reference, context,
				"primitive material assignment differs from the semantic oracle");
		}
		case ExpectedEffectKind::primitiveDuplicate:
			return replayExpect(document.objectCount(EditorObjectKind::primitive)
				== execution.beforePrimaryCount + 1u && added(EditorObjectKind::primitive),
				context, "primitive duplication did not publish exactly one fresh identity");
		case ExpectedEffectKind::primitiveSplit:
		{
			const auto target = NodeId::from(execution.target);
			const auto oldMesh = MeshId::from(execution.reference);
			return replayExpect(target && oldMesh && commands.nodeMesh(*target)
				&& commands.nodeMesh(*target) != oldMesh
				&& document.objectCount(EditorObjectKind::mesh)
					== execution.beforePrimaryCount + 1u
				&& document.objectCount(EditorObjectKind::material)
					== execution.beforeSecondaryCount + 1u
				&& document.objectCount(EditorObjectKind::primitive)
					> execution.beforeTertiaryCount,
				context, "split workflow differs from the node/mesh/material oracle");
		}
		case ExpectedEffectKind::meshMerge:
		{
			const auto target = NodeId::from(execution.target);
			const auto oldMesh = MeshId::from(execution.reference);
			return replayExpect(target && oldMesh && commands.nodeMesh(*target)
				&& commands.nodeMesh(*target) != oldMesh
				&& document.objectCount(EditorObjectKind::mesh)
					<= execution.beforePrimaryCount,
				context, "merge workflow differs from the selected-node oracle");
		}
		case ExpectedEffectKind::camera:
		{
			const auto target = CameraId::from(execution.target);
			return replayExpect(target
				&& sameCamera(commands.cameraProperties(*target), execution.camera),
				context, "camera properties differ from the semantic oracle");
		}
		case ExpectedEffectKind::light:
		{
			const auto target = LightId::from(execution.target);
			return replayExpect(target
				&& commands.lightProperties(*target) == execution.light,
				context, "light properties differ from the semantic oracle");
		}
		case ExpectedEffectKind::createLight:
		{
			const auto lightId = added(EditorObjectKind::light);
			const auto nodeId = added(EditorObjectKind::node);
			const auto typedLight = lightId ? LightId::from(*lightId) : std::nullopt;
			return replayExpect(document.objectCount(EditorObjectKind::light)
				== execution.beforePrimaryCount + 1u
				&& document.objectCount(EditorObjectKind::node)
					== execution.beforeSecondaryCount + 1u
				&& typedLight && nodeId
				&& commands.lightProperties(*typedLight) == execution.light,
				context, "light creation did not publish the expected node/light pair");
		}
		case ExpectedEffectKind::variant:
		{
			const auto target = PrimitiveId::from(execution.target);
			return replayExpect(target
				&& commands.primitiveVariantMaterial(*target, execution.variant)
					== execution.variantMaterial,
				context, "variant replace/remove differs from the semantic oracle");
		}
		case ExpectedEffectKind::rootPreview:
		{
			const auto target = SceneId::from(execution.target);
			return replayExpect(target && sameRootTransform(
				document.sceneRootTransformState(*target).current,
				execution.rootTransform), context,
				"scene-root preview differs from the runtime-state oracle");
		}
		case ExpectedEffectKind::rootReset:
		{
			const auto target = SceneId::from(execution.target);
			const auto state = target
				? document.sceneRootTransformState(*target) : SceneRootTransformState{};
			return replayExpect(target && sameRootTransform(state.current, state.baseline),
				context, "scene-root reset did not restore its baseline");
		}
	}
	return false;
}

std::uint32_t nextRandom(std::uint32_t& state)
{
	state ^= state << 13u;
	state ^= state >> 17u;
	state ^= state << 5u;
	return state;
}

bool runReplayOracle()
{
	constexpr std::array<std::uint32_t, 10> seeds{
		0x13579BDFu, 0x2468ACE1u, 0x31415926u, 0x9E3779B9u, 0xA5A5A5A5u,
		0xC001D00Du, 0xDEADBEEFu, 0x0BADF00Du, 0x5EED1234u, 0xF00DBAADu,
	};
	constexpr std::size_t operationCount =
		static_cast<std::size_t>(ReplayOperation::count);
	bool passed = true;
	for(const std::uint32_t seed : seeds)
	{
		auto loaded = GltfDocumentIO::loadCandidate(fixture("material_extensions.gltf"));
		std::unique_ptr<GltfDocument> document;
		if(!loaded.succeeded() || !std::move(loaded).publishTo(document))
			return expect(false, "independent replay fixture publishes");
		EditorLimits limits = kDefaultEditorLimits;
		limits.maxScenes = 3u;
		limits.maxNodes = 8u;
		limits.maxNodeEdges = 20u;
		limits.maxSceneNodeOccurrences = 20u;
		limits.maxNodeDepth = 8u;
		limits.maxMeshes = 4u;
		limits.maxPrimitives = 8u;
		limits.maxMaterials = 4u;
		limits.maxLights = 3u;
		SceneCommands commands(*document, limits);
		IndependentIdOracle idOracle(*document);
		std::uint32_t state = seed;
		ReplayContext initial{seed, 0u, "initial_state"};
		if(!validateCompleteState(*document, initial)) return false;
		for(std::size_t step = 0; step < 1000u; ++step)
		{
			const std::uint32_t selector = nextRandom(state);
			const std::size_t selected = step < operationCount
				? step : nextRandom(state) % operationCount;
			const auto operation = static_cast<ReplayOperation>(selected);
			const ExactDocumentSnapshot before = exactSnapshot(*document);
			const OperationExecution execution = executeOperation(
				commands, *document, operation, selector, seed, step);
			const ReplayContext context{seed, step, execution.description};
			const ExactDocumentSnapshot after = exactSnapshot(*document);
			bool stepPassed = verifyResultSemantics(
				execution, before, after, *document, context);
			stepPassed &= idOracle.accept(execution.result, context);
			stepPassed &= idOracle.verify(*document, context);
			stepPassed &= verifyAppliedEffect(
				*document, commands, execution, context);
			stepPassed &= validateCompleteState(*document, context);
			if(!stepPassed)
			{
				passed = false;
				break;
			}
		}
	}
	return passed;
}

std::unique_ptr<GltfDocument> loadTestDocument(const char* name)
{
	auto loaded = GltfDocumentIO::loadCandidate(fixture(name));
	std::unique_ptr<GltfDocument> document;
	if(!loaded.succeeded() || !std::move(loaded).publishTo(document)) return {};
	return document;
}

std::unique_ptr<GltfDocument> loadInlineTestDocument(
	std::string_view json, std::string_view label)
{
	static std::uint32_t serial = 0u;
	const std::filesystem::path path = std::filesystem::temp_directory_path()
		/ ("mgif-p4-01s-" + std::string(label) + '-' + std::to_string(++serial) + ".gltf");
	{
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		if(!output) return {};
		output.write(json.data(), static_cast<std::streamsize>(json.size()));
		if(!output) return {};
	}
	auto loaded = GltfDocumentIO::loadCandidate(path);
	std::error_code removeError;
	std::filesystem::remove(path, removeError);
	if(!loaded.succeeded())
	{
		std::cerr << "FAILED: inline snapshot fixture did not load: "
			<< loaded.message() << '\n';
		for(const auto& issue : loaded.validationReport().issues())
			std::cerr << "  validation issue " << static_cast<unsigned>(issue.code)
				<< " at " << issue.path << ": " << issue.message << '\n';
		return {};
	}
	std::unique_ptr<GltfDocument> document;
	if(!std::move(loaded).publishTo(document)) return {};
	return document;
}

bool runAtomicCameraFromViewTest()
{
	auto document = loadTestDocument("material_extensions.gltf");
	auto foreign = loadTestDocument("material_extensions.gltf");
	if(!document || !foreign)
		return expect(false, "camera-from-view fixtures publish");
	SceneEditor editor(*document);
	SceneCommands& commands = editor.commands();
	const CameraId camera = *CameraId::from(
		document->idAt(EditorObjectKind::camera, 0));
	const NodeId node = *NodeId::from(
		document->idAt(EditorObjectKind::node, 0));
	const CameraId foreignCamera = *CameraId::from(
		foreign->idAt(EditorObjectKind::camera, 0));
	const NodeId foreignNode = *NodeId::from(
		foreign->idAt(EditorObjectKind::node, 0));

	bool passed = true;
	NodeTransform historyTransform = *commands.nodeTransform(node);
	historyTransform.translation[1] += 0.25;
	ContinuousFrameSignals finish;
	finish.applicationFrame = 1u;
	finish.inspectorDeactivatedAfterEdit = true;
	passed &= expect(editor.continuousEdit()
			.beginTransform(ContinuousEditKind::inspectorTransform, node).status
			== ContinuousStatus::began,
		"camera-from-view history seed begins");
	passed &= expect(editor.continuousEdit().preview(historyTransform).status
			== ContinuousStatus::previewApplied,
		"camera-from-view history seed previews");
	passed &= expect(editor.continuousEdit().finishFrame(finish).status
			== ContinuousStatus::committed,
		"camera-from-view history seed commits");
	const std::size_t undoCount = editor.history().undoCount();
	const std::size_t redoCount = editor.history().redoCount();
	const std::string undoDescription(editor.history().undoDescription());
	const std::string redoDescription(editor.history().redoDescription());
	const auto historyTime =
		EditorStateInspector::inspect(editor.history()).lastContinuousPush;
	const auto historyUnchanged = [&]
	{
		return editor.history().undoCount() == undoCount
			&& editor.history().redoCount() == redoCount
			&& editor.history().undoDescription() == undoDescription
			&& editor.history().redoDescription() == redoDescription
			&& EditorStateInspector::inspect(editor.history()).lastContinuousPush
				== historyTime
			;
	};
	const auto expectAtomicRejection = [&](const MutationResult& result,
		MutationErrorCode error, const ExactDocumentSnapshot& before,
		const char* message)
	{
		return expect(result.rejected() && result.error == error
				&& result.historyPolicy == HistoryPolicy::nonHistory
				&& result.beforeRevision == before.revision
				&& result.afterRevision == before.revision
				&& exactSnapshot(*document) == before && historyUnchanged(),
			message);
	};

	CameraProperties properties = *commands.cameraProperties(camera);
	NodeTransform transform = *commands.nodeTransform(node);
	properties.yfov += 0.125;
	transform.translation[0] += 2.0;
	const MutationToken staleToken = commands.token();
	std::uint64_t revision = document->revision();
	auto result = commands.setCameraFromView(camera, node, properties, transform);
	passed &= expect(result.applied()
			&& result.historyPolicy == HistoryPolicy::nonHistory
			&& result.beforeRevision == revision
			&& result.afterRevision == revision + 1u
			&& document->revision() == revision + 1u
			&& sameCamera(commands.cameraProperties(camera), properties)
			&& commands.nodeTransform(node) == transform && historyUnchanged(),
		"camera-from-view changes camera and node in one non-history revision");

	revision = document->revision();
	result = commands.setCameraFromView(camera, node, properties, transform);
	passed &= expect(result.domainNoop() && result.error == MutationErrorCode::noOp
			&& result.historyPolicy == HistoryPolicy::nonHistory
			&& result.beforeRevision == revision && result.afterRevision == revision
			&& document->revision() == revision && historyUnchanged(),
		"identical camera-from-view input is an exact non-history no-op");

	CameraProperties cameraOnly = properties;
	cameraOnly.yfov += 0.125;
	revision = document->revision();
	result = commands.setCameraFromView(camera, node, cameraOnly, transform);
	passed &= expect(result.applied() && result.beforeRevision == revision
			&& result.afterRevision == revision + 1u
			&& result.historyPolicy == HistoryPolicy::nonHistory
			&& sameCamera(commands.cameraProperties(camera), cameraOnly)
			&& commands.nodeTransform(node) == transform && historyUnchanged(),
		"camera-only camera-from-view change publishes once");
	properties = cameraOnly;

	NodeTransform transformOnly = transform;
	transformOnly.translation[2] += 3.0;
	revision = document->revision();
	result = commands.setCameraFromView(camera, node, properties, transformOnly);
	passed &= expect(result.applied() && result.beforeRevision == revision
			&& result.afterRevision == revision + 1u
			&& result.historyPolicy == HistoryPolicy::nonHistory
			&& sameCamera(commands.cameraProperties(camera), properties)
			&& commands.nodeTransform(node) == transformOnly && historyUnchanged(),
		"transform-only camera-from-view change publishes once");
	transform = transformOnly;

	CameraProperties invalidCamera = properties;
	invalidCamera.yfov = -1.0;
	NodeTransform requestedTransform = transform;
	requestedTransform.translation[0] += 9.0;
	auto before = exactSnapshot(*document);
	result = commands.setCameraFromView(
		camera, node, invalidCamera, requestedTransform);
	passed &= expectAtomicRejection(result, MutationErrorCode::invalidReference,
		before, "invalid camera preserves the valid requested transform atomically");

	CameraProperties requestedCamera = properties;
	requestedCamera.yfov += 0.25;
	NodeTransform invalidTransform = transform;
	invalidTransform.rotation = {0.0, 0.0, 0.0, 2.0};
	before = exactSnapshot(*document);
	result = commands.setCameraFromView(
		camera, node, requestedCamera, invalidTransform);
	passed &= expectAtomicRejection(result, MutationErrorCode::invalidTransform,
		before, "invalid transform preserves the valid requested camera atomically");

	before = exactSnapshot(*document);
	result = commands.setCameraFromView(camera, node, requestedCamera, requestedTransform,
		MutationGuard{before.revision + 1u, std::nullopt});
	passed &= expectAtomicRejection(result, MutationErrorCode::staleRevision,
		before, "camera-from-view stale revision is typed and atomic");
	before = exactSnapshot(*document);
	result = commands.setCameraFromView(camera, node, requestedCamera, requestedTransform,
		MutationGuard{std::nullopt, staleToken});
	passed &= expectAtomicRejection(result, MutationErrorCode::staleTransaction,
		before, "camera-from-view stale token is typed and atomic");

	before = exactSnapshot(*document);
	result = commands.setCameraFromView(
		foreignCamera, node, requestedCamera, requestedTransform);
	passed &= expectAtomicRejection(result, MutationErrorCode::crossDocument,
		before, "camera-from-view foreign camera is typed and atomic");
	before = exactSnapshot(*document);
	result = commands.setCameraFromView(
		camera, foreignNode, requestedCamera, requestedTransform);
	passed &= expectAtomicRejection(result, MutationErrorCode::crossDocument,
		before, "camera-from-view foreign node is typed and atomic");

	EditorObjectId staleCameraValue = camera.untyped();
	++staleCameraValue.generation;
	const CameraId staleCamera = *CameraId::from(staleCameraValue);
	before = exactSnapshot(*document);
	result = commands.setCameraFromView(
		staleCamera, node, requestedCamera, requestedTransform);
	passed &= expectAtomicRejection(result, MutationErrorCode::staleId,
		before, "camera-from-view stale camera is typed and atomic");
	EditorObjectId staleNodeValue = node.untyped();
	++staleNodeValue.generation;
	const NodeId staleNode = *NodeId::from(staleNodeValue);
	before = exactSnapshot(*document);
	result = commands.setCameraFromView(
		camera, staleNode, requestedCamera, requestedTransform);
	passed &= expectAtomicRejection(result, MutationErrorCode::staleId,
		before, "camera-from-view stale node is typed and atomic");
	return passed;
}

enum class TotalPolicyOperation : std::uint8_t
{
	query,
	addScene,
	deleteScene,
	addRootNode,
	addChildNode,
	duplicateNode,
	deleteNode,
	reparentNode,
	duplicateNodeAt,
	deleteNodeAt,
	reparentNodeAt,
	prepareAddChild,
	prepareAddLight,
	prepareDuplicate,
	prepareDelete,
	prepareReparent,
	commitAddChild,
	commitAddLight,
	commitDuplicate,
	commitDelete,
	commitReparent,
	executeAddChild,
	executeAddLight,
	executeDuplicate,
	executeDelete,
	executeReparent,
	restoreStructural,
	renameInvalid,
	renameScene,
	renameNode,
	renameStaleNode,
	renameForeignNode,
	renamePrimitive,
	renameMesh,
	renameMaterial,
	renameTexture,
	renameImage,
	renameSampler,
	renameCamera,
	renameLight,
	renameAccessor,
	renameBufferView,
	renameBuffer,
	renameSkin,
	renameAnimation,
	renameCount,
	setNodeTransform,
	attachMesh,
	attachCamera,
	attachLight,
	setVisibility,
	removeVisibility,
	setMaterial,
	setMaterialTexture,
	setMaterialExtension,
	setMaterialSnapshot,
	assignPrimitiveMaterial,
	duplicatePrimitive,
	splitPrimitiveNodeAware,
	splitPrimitiveCompatibility,
	mergeMeshesNodeAware,
	mergeMeshesCompatibility,
	setCamera,
	setCameraFromView,
	setLight,
	setLightSnapshot,
	createLight,
	deleteLight,
	setPrimitiveVariant,
	previewSceneRootTransform,
	resetSceneRootTransform,
	sceneEditorAddRootNode,
	count,
};

enum class TotalPolicyState : std::uint8_t
{
	applied,
	domainNoop,
	rejected,
	prepared,
};

enum class TotalGuardInput : std::uint8_t
{
	none,
	invalidToken,
	staleRevision,
	staleToken,
	foreignToken,
};

struct TotalPolicyObservation final
{
	HistoryPolicy policy{HistoryPolicy::unclassified};
	TotalPolicyState state{TotalPolicyState::rejected};
	MutationErrorCode error{MutationErrorCode::internalError};
	bool mutationResult{false};
	std::uint64_t resultBeforeRevision{0};
	std::uint64_t resultAfterRevision{0};
};

struct TotalPolicyAttempt final
{
	TotalPolicyObservation observation;
	ExactDocumentSnapshot before;
	ExactDocumentSnapshot after;
};

struct TotalPolicyContext final
{
	GltfDocument& document;
	SceneCommands& commands;
	GltfDocument& foreignDocument;
	SceneCommands& foreignCommands;
};

template <typename Invoke>
TotalPolicyAttempt captureMutationPolicy(GltfDocument& document, Invoke&& invoke)
{
	TotalPolicyAttempt attempt;
	attempt.before = exactSnapshot(document);
	const MutationResult result = std::forward<Invoke>(invoke)();
	attempt.after = exactSnapshot(document);
	attempt.observation.policy = result.historyPolicy;
	attempt.observation.error = result.error;
	attempt.observation.mutationResult = true;
	attempt.observation.resultBeforeRevision = result.beforeRevision;
	attempt.observation.resultAfterRevision = result.afterRevision;
	attempt.observation.state = result.applied() ? TotalPolicyState::applied
		: result.domainNoop() ? TotalPolicyState::domainNoop
		: TotalPolicyState::rejected;
	return attempt;
}

template <typename Invoke>
TotalPolicyAttempt captureStructuralPolicy(GltfDocument& document, Invoke&& invoke)
{
	return captureMutationPolicy(document, [&]
	{
		return std::forward<Invoke>(invoke)().mutation;
	});
}

template <typename Invoke>
TotalPolicyAttempt capturePreparePolicy(GltfDocument& document, Invoke&& invoke)
{
	TotalPolicyAttempt attempt;
	attempt.before = exactSnapshot(document);
	StructuralPrepareResult result = std::forward<Invoke>(invoke)();
	attempt.after = exactSnapshot(document);
	attempt.observation.policy = result.historyPolicy;
	attempt.observation.error = result.error;
	attempt.observation.state = result.succeeded()
		? TotalPolicyState::prepared : TotalPolicyState::rejected;
	return attempt;
}

MutationGuard totalPolicyGuard(
	const TotalPolicyContext& context, TotalGuardInput input)
{
	switch(input)
	{
		case TotalGuardInput::none: return {};
		case TotalGuardInput::invalidToken:
			return MutationGuard{std::nullopt, MutationToken{
				context.document.documentAuthority(), context.document.revision(), 0u}};
		case TotalGuardInput::staleRevision:
			return MutationGuard{context.document.revision() + 1u, std::nullopt};
		case TotalGuardInput::staleToken:
		{
			MutationToken stale = context.commands.token();
			++stale.serial;
			return MutationGuard{std::nullopt, stale};
		}
		case TotalGuardInput::foreignToken:
			return MutationGuard{std::nullopt, context.foreignCommands.token()};
	}
	return {};
}

template <typename Id>
Id totalLiveId(const GltfDocument& document, EditorObjectKind kind,
	std::size_t index = 0u)
{
	return *Id::from(document.idAt(kind, index));
}

template <typename Id>
Id totalStaleId(Id live)
{
	EditorObjectId value = live.untyped();
	++value.generation;
	return *Id::from(value);
}

enum class TotalStructuralKind : std::uint8_t
{
	addChild,
	addLight,
	duplicate,
	deleteNode,
	reparent,
};

StructuralRequest totalStructuralRequest(
	TotalPolicyContext& context, TotalStructuralKind kind)
{
	const SceneId scene = totalLiveId<SceneId>(
		context.document, EditorObjectKind::scene, 0u);
	const NodeId parent = totalLiveId<NodeId>(
		context.document, EditorObjectKind::node, 0u);
	const NodeId source = totalLiveId<NodeId>(
		context.document, EditorObjectKind::node, 1u);
	switch(kind)
	{
		case TotalStructuralKind::addChild:
			return StructuralRequest{AddChildNodeRequest{
				scene, parent, "total-policy-child"}};
		case TotalStructuralKind::addLight:
		{
			LightProperties light;
			light.intensity = 3.0;
			return StructuralRequest{AddLightRequest{
				scene, parent, "total-policy-light-node",
				"total-policy-light", light}};
		}
		case TotalStructuralKind::duplicate:
		{
			const auto sourceOccurrence =
				context.commands.captureNodeOccurrence(scene, source);
			const auto parentOccurrence =
				context.commands.captureNodeOccurrence(scene, parent);
			return StructuralRequest{DuplicateNodeRequest{
				*sourceOccurrence, parentOccurrence,
				SceneOccurrencePolicy::clickedScene}};
		}
		case TotalStructuralKind::deleteNode:
		{
			const auto occurrence =
				context.commands.captureNodeOccurrence(scene, source);
			return StructuralRequest{DeleteNodeRequest{
				*occurrence, SceneOccurrencePolicy::clickedScene}};
		}
		case TotalStructuralKind::reparent:
		{
			const auto sourceOccurrence =
				context.commands.captureNodeOccurrence(scene, source);
			const auto parentOccurrence =
				context.commands.captureNodeOccurrence(scene, parent);
			return StructuralRequest{ReparentNodeRequest{
				*sourceOccurrence, parentOccurrence,
				SceneOccurrencePolicy::clickedScene}};
		}
	}
	return StructuralRequest{AddChildNodeRequest{
		scene, parent, "unreachable-total-policy-child"}};
}

bool totalStructuralOperation(TotalPolicyOperation operation,
	TotalStructuralKind& kind, std::uint8_t& phase)
{
	const auto value = static_cast<std::uint8_t>(operation);
	const auto prepare = static_cast<std::uint8_t>(
		TotalPolicyOperation::prepareAddChild);
	const auto execute = static_cast<std::uint8_t>(
		TotalPolicyOperation::executeReparent);
	if(value < prepare || value > execute) return false;
	const std::uint8_t offset = static_cast<std::uint8_t>(value - prepare);
	phase = static_cast<std::uint8_t>(offset / 5u);
	kind = static_cast<TotalStructuralKind>(offset % 5u);
	return true;
}

TotalPolicyAttempt performTotalPolicyOperation(
	TotalPolicyContext& context,
	TotalPolicyOperation operation,
	TotalGuardInput guardInput,
	bool unchanged)
{
	TotalStructuralKind structuralKind{};
	std::uint8_t structuralPhase = 0u;
	if(totalStructuralOperation(operation, structuralKind, structuralPhase))
	{
		if(structuralPhase == 0u)
		{
			return capturePreparePolicy(context.document, [&]
			{
				return context.commands.prepareStructural(
					totalStructuralRequest(context, structuralKind),
					totalPolicyGuard(context, guardInput));
			});
		}
		if(structuralPhase == 1u)
		{
			auto prepared = context.commands.prepareStructural(
				totalStructuralRequest(context, structuralKind));
			return captureStructuralPolicy(context.document, [&]
			{
				return context.commands.commitStructural(
					std::move(prepared.prepared),
					totalPolicyGuard(context, guardInput));
			});
		}
		return captureStructuralPolicy(context.document, [&]
		{
			return context.commands.executeStructural(
				totalStructuralRequest(context, structuralKind),
				totalPolicyGuard(context, guardInput));
		});
	}

	if(operation >= TotalPolicyOperation::renameInvalid
		&& operation <= TotalPolicyOperation::renameCount)
	{
		EditorObjectId id;
		id.authority = context.document.documentAuthority();
		id.generation = 1u;
		switch(operation)
		{
			case TotalPolicyOperation::renameInvalid:
				id.kind = EditorObjectKind::invalid;
				break;
			case TotalPolicyOperation::renameScene:
				id = context.document.idAt(EditorObjectKind::scene, 0u);
				break;
			case TotalPolicyOperation::renameNode:
				id = context.document.idAt(EditorObjectKind::node, 0u);
				break;
			case TotalPolicyOperation::renameStaleNode:
				id = totalStaleId(totalLiveId<NodeId>(
					context.document, EditorObjectKind::node)).untyped();
				break;
			case TotalPolicyOperation::renameForeignNode:
				id = context.foreignDocument.idAt(EditorObjectKind::node, 0u);
				break;
			case TotalPolicyOperation::renamePrimitive:
				id = context.document.idAt(EditorObjectKind::primitive, 0u);
				break;
			case TotalPolicyOperation::renameMesh:
				id = context.document.idAt(EditorObjectKind::mesh, 0u);
				break;
			case TotalPolicyOperation::renameMaterial:
				id = context.document.idAt(EditorObjectKind::material, 0u);
				break;
			case TotalPolicyOperation::renameTexture:
				id = context.document.idAt(EditorObjectKind::texture, 0u);
				break;
			case TotalPolicyOperation::renameImage:
				id = context.document.idAt(EditorObjectKind::image, 0u);
				break;
			case TotalPolicyOperation::renameSampler:
				id.kind = EditorObjectKind::sampler;
				break;
			case TotalPolicyOperation::renameCamera:
				id = context.document.idAt(EditorObjectKind::camera, 0u);
				break;
			case TotalPolicyOperation::renameLight:
				id = context.document.idAt(EditorObjectKind::light, 0u);
				break;
			case TotalPolicyOperation::renameAccessor:
				id = context.document.idAt(EditorObjectKind::accessor, 0u);
				break;
			case TotalPolicyOperation::renameBufferView:
				id = context.document.idAt(EditorObjectKind::bufferView, 0u);
				break;
			case TotalPolicyOperation::renameBuffer:
				id = context.document.idAt(EditorObjectKind::buffer, 0u);
				break;
			case TotalPolicyOperation::renameSkin:
				id = context.document.idAt(EditorObjectKind::skin, 0u);
				break;
			case TotalPolicyOperation::renameAnimation:
				id = context.document.idAt(EditorObjectKind::animation, 0u);
				break;
			case TotalPolicyOperation::renameCount:
				id.kind = EditorObjectKind::count;
				break;
			default:
				break;
		}
		const std::string name = unchanged
			? std::string(context.document.objectName(id))
			: "total-policy-renamed";
		return captureMutationPolicy(context.document, [&]
		{
			return context.commands.rename(
				id, name, totalPolicyGuard(context, guardInput));
		});
	}

	switch(operation)
	{
		case TotalPolicyOperation::query:
		{
			const EditorObjectId id = context.document.idAt(
				EditorObjectKind::node, 0u);
			return captureMutationPolicy(context.document, [&]
			{
				return context.commands.query(
					id, totalPolicyGuard(context, guardInput));
			});
		}
		case TotalPolicyOperation::addScene:
			return captureMutationPolicy(context.document, [&]
			{
				return context.commands.addScene(
					"total-policy-scene", totalPolicyGuard(context, guardInput));
			});
		case TotalPolicyOperation::deleteScene:
		{
			const SceneId scene = totalLiveId<SceneId>(
				context.document, EditorObjectKind::scene, 1u);
			return captureMutationPolicy(context.document, [&]
			{
				return context.commands.deleteScene(
					scene, totalPolicyGuard(context, guardInput));
			});
		}
		case TotalPolicyOperation::addRootNode:
		case TotalPolicyOperation::addChildNode:
		{
			const SceneId scene = totalLiveId<SceneId>(
				context.document, EditorObjectKind::scene);
			const std::optional<NodeId> parent = operation
				== TotalPolicyOperation::addChildNode
				? std::optional<NodeId>{totalLiveId<NodeId>(
					context.document, EditorObjectKind::node)}
				: std::nullopt;
			return captureMutationPolicy(context.document, [&]
			{
				return context.commands.addNode(scene, "total-policy-node", parent,
					totalPolicyGuard(context, guardInput));
			});
		}
		case TotalPolicyOperation::duplicateNode:
		{
			const NodeId source = totalLiveId<NodeId>(
				context.document, EditorObjectKind::node, 1u);
			const NodeId parent = totalLiveId<NodeId>(
				context.document, EditorObjectKind::node, 0u);
			return captureMutationPolicy(context.document, [&]
			{
				return context.commands.duplicateNode(source, parent,
					totalPolicyGuard(context, guardInput));
			});
		}
		case TotalPolicyOperation::deleteNode:
		{
			const NodeId node = totalLiveId<NodeId>(
				context.document, EditorObjectKind::node, unchanged ? 0u : 1u);
			return captureMutationPolicy(context.document, [&]
			{
				return context.commands.deleteNode(
					node, totalPolicyGuard(context, guardInput));
			});
		}
		case TotalPolicyOperation::reparentNode:
		{
			const NodeId node = totalLiveId<NodeId>(
				context.document, EditorObjectKind::node, unchanged ? 0u : 1u);
			const std::optional<NodeId> parent = unchanged
				? std::nullopt
				: std::optional<NodeId>{totalLiveId<NodeId>(
					context.document, EditorObjectKind::node, 0u)};
			return captureMutationPolicy(context.document, [&]
			{
				return context.commands.reparentNode(node, parent,
					totalPolicyGuard(context, guardInput));
			});
		}
		case TotalPolicyOperation::duplicateNodeAt:
		case TotalPolicyOperation::deleteNodeAt:
		case TotalPolicyOperation::reparentNodeAt:
		{
			const SceneId scene = totalLiveId<SceneId>(
				context.document, EditorObjectKind::scene);
			const NodeId sourceNode = totalLiveId<NodeId>(
				context.document, EditorObjectKind::node, 1u);
			const NodeId parentNode = totalLiveId<NodeId>(
				context.document, EditorObjectKind::node, 0u);
			const SceneNodeOccurrence source =
				*context.commands.captureNodeOccurrence(scene, sourceNode);
			const SceneNodeOccurrence parent =
				*context.commands.captureNodeOccurrence(scene, parentNode);
			if(operation == TotalPolicyOperation::duplicateNodeAt)
				return captureMutationPolicy(context.document, [&]
				{
					return context.commands.duplicateNodeAt(source, parent,
						SceneOccurrencePolicy::clickedScene,
						totalPolicyGuard(context, guardInput));
				});
			if(operation == TotalPolicyOperation::deleteNodeAt)
				return captureMutationPolicy(context.document, [&]
				{
					return context.commands.deleteNodeAt(source,
						SceneOccurrencePolicy::clickedScene,
						totalPolicyGuard(context, guardInput));
				});
			return captureMutationPolicy(context.document, [&]
			{
				return context.commands.reparentNodeAt(source, parent,
					SceneOccurrencePolicy::clickedScene,
					totalPolicyGuard(context, guardInput));
			});
		}
		case TotalPolicyOperation::restoreStructural:
		{
			auto executed = context.commands.executeStructural(
				totalStructuralRequest(context, TotalStructuralKind::addChild));
			if(!executed.delta)
			{
				return captureStructuralPolicy(context.document, [&]
				{
					return context.commands.executeStructural(
						totalStructuralRequest(context, TotalStructuralKind::addChild),
						totalPolicyGuard(context, guardInput));
				});
			}
			return captureMutationPolicy(context.document, [&]
			{
				return context.commands.restoreStructural(
					*executed.delta, StructuralState::undo,
					totalPolicyGuard(context, guardInput));
			});
		}
		case TotalPolicyOperation::setNodeTransform:
		{
			const NodeId node = totalLiveId<NodeId>(
				context.document, EditorObjectKind::node);
			NodeTransform value = *context.commands.nodeTransform(node);
			if(!unchanged) value.translation[0] += 0.5;
			else (void)context.commands.setNodeTransform(node, value);
			return captureMutationPolicy(context.document, [&]
			{
				return context.commands.setNodeTransform(
					node, value, totalPolicyGuard(context, guardInput));
			});
		}
		case TotalPolicyOperation::attachMesh:
		{
			const NodeId node = totalLiveId<NodeId>(context.document,
				EditorObjectKind::node, unchanged ? 0u : 1u);
			const MeshId mesh = totalLiveId<MeshId>(
				context.document, EditorObjectKind::mesh);
			return captureMutationPolicy(context.document, [&]
			{
				return context.commands.attachMesh(
					node, mesh, totalPolicyGuard(context, guardInput));
			});
		}
		case TotalPolicyOperation::attachCamera:
		{
			const NodeId node = totalLiveId<NodeId>(context.document,
				EditorObjectKind::node, unchanged ? 0u : 1u);
			const CameraId camera = totalLiveId<CameraId>(
				context.document, EditorObjectKind::camera);
			return captureMutationPolicy(context.document, [&]
			{
				return context.commands.attachCamera(
					node, camera, totalPolicyGuard(context, guardInput));
			});
		}
		case TotalPolicyOperation::attachLight:
		{
			const NodeId node = totalLiveId<NodeId>(context.document,
				EditorObjectKind::node, unchanged ? 0u : 1u);
			const LightId light = totalLiveId<LightId>(
				context.document, EditorObjectKind::light);
			return captureMutationPolicy(context.document, [&]
			{
				return context.commands.attachLight(
					node, light, totalPolicyGuard(context, guardInput));
			});
		}
		case TotalPolicyOperation::setVisibility:
		{
			const EditorObjectId node = context.document.idAt(
				EditorObjectKind::node, 0u);
			if(unchanged)
				(void)context.commands.setVisibility(node, false);
			return captureMutationPolicy(context.document, [&]
			{
				return context.commands.setVisibility(
					node, false, totalPolicyGuard(context, guardInput));
			});
		}
		case TotalPolicyOperation::removeVisibility:
		{
			const NodeId node = totalLiveId<NodeId>(
				context.document, EditorObjectKind::node);
			if(!unchanged)
				(void)context.commands.setVisibility(node.untyped(), false);
			return captureMutationPolicy(context.document, [&]
			{
				return context.commands.removeVisibility(
					node, totalPolicyGuard(context, guardInput));
			});
		}
		case TotalPolicyOperation::setMaterial:
		{
			const MaterialId material = totalLiveId<MaterialId>(
				context.document, EditorObjectKind::material);
			MaterialProperties value = *context.commands.materialProperties(material);
			if(!unchanged) value.roughnessFactor = value.roughnessFactor == 0.25 ? 0.5 : 0.25;
			return captureMutationPolicy(context.document, [&]
			{
				return context.commands.setMaterial(
					material, value, totalPolicyGuard(context, guardInput));
			});
		}
		case TotalPolicyOperation::setMaterialTexture:
		{
			const MaterialId material = totalLiveId<MaterialId>(
				context.document, EditorObjectKind::material);
			MaterialTextureProperties value;
			if(unchanged)
				value = *context.commands.materialTexture(
					material, MaterialTextureSlot::baseColor);
			else
				value.texture = totalLiveId<TextureId>(
					context.document, EditorObjectKind::texture);
			return captureMutationPolicy(context.document, [&]
			{
				return context.commands.setMaterialTexture(material,
					MaterialTextureSlot::baseColor, value,
					totalPolicyGuard(context, guardInput));
			});
		}
		case TotalPolicyOperation::setMaterialExtension:
		{
			const MaterialId material = totalLiveId<MaterialId>(
				context.document, EditorObjectKind::material);
			MaterialExtensionValue value;
			value.value = true;
			if(unchanged)
				(void)context.commands.setMaterialExtension(
					material, "MGIF_total_policy", value);
			return captureMutationPolicy(context.document, [&]
			{
				return context.commands.setMaterialExtension(material,
					"MGIF_total_policy", value,
					totalPolicyGuard(context, guardInput));
			});
		}
		case TotalPolicyOperation::setMaterialSnapshot:
		{
			const MaterialId material = totalLiveId<MaterialId>(
				context.document, EditorObjectKind::material);
			std::optional<MaterialSnapshot> snapshot;
			if(unchanged)
				snapshot = context.commands.materialSnapshot(material);
			else
			{
				MaterialProperties changed =
					*context.commands.materialProperties(material);
				changed.roughnessFactor = changed.roughnessFactor == 0.2 ? 0.4 : 0.2;
				snapshot = context.commands.prepareMaterialSnapshot(
					material, MaterialEdit{changed});
			}
			return captureMutationPolicy(context.document, [&]
			{
				return context.commands.setMaterialSnapshot(material, *snapshot,
					totalPolicyGuard(context, guardInput));
			});
		}
		case TotalPolicyOperation::assignPrimitiveMaterial:
		{
			const PrimitiveId primitive = totalLiveId<PrimitiveId>(
				context.document, EditorObjectKind::primitive);
			const std::optional<MaterialId> material = unchanged
				? context.commands.primitiveMaterial(primitive)
				: std::optional<MaterialId>{totalLiveId<MaterialId>(
					context.document, EditorObjectKind::material, 1u)};
			return captureMutationPolicy(context.document, [&]
			{
				return context.commands.assignPrimitiveMaterial(primitive, material,
					totalPolicyGuard(context, guardInput));
			});
		}
		case TotalPolicyOperation::duplicatePrimitive:
		{
			const PrimitiveId primitive = totalLiveId<PrimitiveId>(
				context.document, EditorObjectKind::primitive);
			return captureMutationPolicy(context.document, [&]
			{
				return context.commands.duplicatePrimitive(
					primitive, totalPolicyGuard(context, guardInput));
			});
		}
		case TotalPolicyOperation::splitPrimitiveNodeAware:
		{
			const NodeId node = totalLiveId<NodeId>(
				context.document, EditorObjectKind::node);
			const PrimitiveId primitive = totalLiveId<PrimitiveId>(
				context.document, EditorObjectKind::primitive);
			return captureMutationPolicy(context.document, [&]
			{
				return context.commands.splitPrimitive(
					node, primitive, totalPolicyGuard(context, guardInput));
			});
		}
		case TotalPolicyOperation::splitPrimitiveCompatibility:
		{
			const PrimitiveId primitive = totalLiveId<PrimitiveId>(
				context.document, EditorObjectKind::primitive);
			return captureMutationPolicy(context.document, [&]
			{
				return context.commands.splitPrimitive(
					primitive, totalPolicyGuard(context, guardInput));
			});
		}
		case TotalPolicyOperation::mergeMeshesNodeAware:
		case TotalPolicyOperation::mergeMeshesCompatibility:
		{
			const NodeId selected = totalLiveId<NodeId>(
				context.document, EditorObjectKind::node, 0u);
			const NodeId peer = totalLiveId<NodeId>(
				context.document, EditorObjectKind::node, 1u);
			const PrimitiveId primitive = totalLiveId<PrimitiveId>(
				context.document, EditorObjectKind::primitive, 0u);
			(void)context.commands.splitPrimitive(selected, primitive);
			if(operation == TotalPolicyOperation::mergeMeshesNodeAware)
				return captureMutationPolicy(context.document, [&]
				{
					return context.commands.mergeMeshes(
						selected, totalPolicyGuard(context, guardInput));
				});
			const MeshId target = *context.commands.nodeMesh(peer);
			const MeshId source = *context.commands.nodeMesh(selected);
			return captureMutationPolicy(context.document, [&]
			{
				return context.commands.mergeMeshes(target, source,
					totalPolicyGuard(context, guardInput));
			});
		}
		case TotalPolicyOperation::setCamera:
		case TotalPolicyOperation::setCameraFromView:
		{
			const CameraId camera = totalLiveId<CameraId>(
				context.document, EditorObjectKind::camera);
			const NodeId node = totalLiveId<NodeId>(
				context.document, EditorObjectKind::node);
			CameraProperties properties = *context.commands.cameraProperties(camera);
			NodeTransform transform = *context.commands.nodeTransform(node);
			if(!unchanged)
			{
				properties.yfov += 0.125;
				transform.translation[0] += 0.75;
			}
			else if(operation == TotalPolicyOperation::setCameraFromView)
			{
				(void)context.commands.setNodeTransform(node, transform);
			}
			if(operation == TotalPolicyOperation::setCamera)
				return captureMutationPolicy(context.document, [&]
				{
					return context.commands.setCamera(camera, properties,
						totalPolicyGuard(context, guardInput));
				});
			return captureMutationPolicy(context.document, [&]
			{
				return context.commands.setCameraFromView(camera, node,
					properties, transform, totalPolicyGuard(context, guardInput));
			});
		}
		case TotalPolicyOperation::setLight:
		{
			const LightId light = totalLiveId<LightId>(
				context.document, EditorObjectKind::light);
			LightProperties properties = *context.commands.lightProperties(light);
			if(!unchanged) properties.intensity += 0.5;
			return captureMutationPolicy(context.document, [&]
			{
				return context.commands.setLight(
					light, properties, totalPolicyGuard(context, guardInput));
			});
		}
		case TotalPolicyOperation::setLightSnapshot:
		{
			const LightId light = totalLiveId<LightId>(
				context.document, EditorObjectKind::light);
			std::optional<LightSnapshot> snapshot;
			if(unchanged)
				snapshot = context.commands.lightSnapshot(light);
			else
			{
				LightProperties changed = *context.commands.lightProperties(light);
				changed.intensity += 0.75;
				snapshot = context.commands.prepareLightSnapshot(light, changed);
			}
			return captureMutationPolicy(context.document, [&]
			{
				return context.commands.setLightSnapshot(light, *snapshot,
					totalPolicyGuard(context, guardInput));
			});
		}
		case TotalPolicyOperation::createLight:
		{
			const SceneId scene = totalLiveId<SceneId>(
				context.document, EditorObjectKind::scene);
			LightProperties properties;
			properties.intensity = 4.0;
			return captureMutationPolicy(context.document, [&]
			{
				return context.commands.createLight(scene, std::nullopt,
					"total-policy-light-node", "total-policy-light", properties,
					totalPolicyGuard(context, guardInput));
			});
		}
		case TotalPolicyOperation::deleteLight:
		{
			const LightId light = totalLiveId<LightId>(
				context.document, EditorObjectKind::light);
			const SceneId scene = totalLiveId<SceneId>(
				context.document, EditorObjectKind::scene);
			LightProperties replacement;
			(void)context.commands.createLight(scene, std::nullopt,
				"total-policy-delete-light-node", "total-policy-delete-light-peer",
				replacement);
			return captureMutationPolicy(context.document, [&]
			{
				return context.commands.deleteLight(
					light, totalPolicyGuard(context, guardInput));
			});
		}
		case TotalPolicyOperation::setPrimitiveVariant:
		{
			const PrimitiveId primitive = totalLiveId<PrimitiveId>(
				context.document, EditorObjectKind::primitive);
			const MaterialId material = totalLiveId<MaterialId>(context.document,
				EditorObjectKind::material, unchanged ? 0u : 1u);
			const std::uint32_t variant = unchanged ? 0u : 1u;
			return captureMutationPolicy(context.document, [&]
			{
				return context.commands.setPrimitiveVariant(primitive, variant, material,
					totalPolicyGuard(context, guardInput));
			});
		}
		case TotalPolicyOperation::previewSceneRootTransform:
		{
			const SceneId scene = totalLiveId<SceneId>(
				context.document, EditorObjectKind::scene);
			SceneRootTransform transform =
				context.document.sceneRootTransformState(scene).current;
			if(!unchanged) transform.translation[0] += 1.0;
			return captureMutationPolicy(context.document, [&]
			{
				return context.commands.previewSceneRootTransform(scene, transform,
					totalPolicyGuard(context, guardInput));
			});
		}
		case TotalPolicyOperation::resetSceneRootTransform:
		{
			const SceneId scene = totalLiveId<SceneId>(
				context.document, EditorObjectKind::scene);
			if(!unchanged)
			{
				SceneRootTransform transform =
					context.document.sceneRootTransformState(scene).current;
				transform.translation[0] += 1.0;
				(void)context.commands.previewSceneRootTransform(scene, transform);
			}
			return captureMutationPolicy(context.document, [&]
			{
				return context.commands.resetSceneRootTransform(
					scene, totalPolicyGuard(context, guardInput));
			});
		}
		case TotalPolicyOperation::sceneEditorAddRootNode:
		{
			SceneEditor editor(context.document);
			const SceneId scene = totalLiveId<SceneId>(
				context.document, EditorObjectKind::scene);
			return captureMutationPolicy(context.document, [&]
			{
				return editor.addRootNode(scene, "total-policy-editor-root",
					totalPolicyGuard(context, guardInput)).mutation;
			});
		}
		default:
			break;
	}
	return captureMutationPolicy(context.document, [&]
	{
		return context.commands.query(EditorObjectId{},
			totalPolicyGuard(context, guardInput));
	});
}

struct TotalPolicyContract final
{
	TotalPolicyOperation operation;
	const char* label;
	const char* fixtureName;
	HistoryPolicy policy;
	TotalPolicyState validState;
	bool hasUnchangedInput{false};
	std::uint64_t appliedRevisionDelta{1u};
};

constexpr TotalPolicyContract kTotalPolicyContracts[] = {
	{TotalPolicyOperation::query, "query(EditorObjectId, guard)",
		"feature_preservation.gltf", HistoryPolicy::notApplicable,
		TotalPolicyState::domainNoop, true, 0u},
	{TotalPolicyOperation::addScene, "addScene", "feature_preservation.gltf",
		HistoryPolicy::nonHistory, TotalPolicyState::applied},
	{TotalPolicyOperation::deleteScene, "deleteScene", "material_extensions.gltf",
		HistoryPolicy::nonHistory, TotalPolicyState::applied},
	{TotalPolicyOperation::addRootNode, "addNode(root)", "multi_scene_oracle.gltf",
		HistoryPolicy::nonHistory, TotalPolicyState::applied},
	{TotalPolicyOperation::addChildNode, "addNode(child)", "multi_scene_oracle.gltf",
		HistoryPolicy::undoable, TotalPolicyState::applied},
	{TotalPolicyOperation::duplicateNode, "duplicateNode(legacy)",
		"multi_scene_oracle.gltf", HistoryPolicy::undoable, TotalPolicyState::applied},
	{TotalPolicyOperation::deleteNode, "deleteNode(legacy)",
		"multi_scene_oracle.gltf", HistoryPolicy::undoable, TotalPolicyState::applied},
	{TotalPolicyOperation::reparentNode, "reparentNode(legacy)",
		"multi_scene_oracle.gltf", HistoryPolicy::undoable,
		TotalPolicyState::applied, true},
	{TotalPolicyOperation::duplicateNodeAt, "duplicateNodeAt(occurrence)",
		"multi_scene_oracle.gltf", HistoryPolicy::undoable, TotalPolicyState::applied},
	{TotalPolicyOperation::deleteNodeAt, "deleteNodeAt(occurrence)",
		"multi_scene_oracle.gltf", HistoryPolicy::undoable, TotalPolicyState::applied},
	{TotalPolicyOperation::reparentNodeAt, "reparentNodeAt(occurrence)",
		"multi_scene_oracle.gltf", HistoryPolicy::undoable, TotalPolicyState::applied},

	{TotalPolicyOperation::prepareAddChild, "prepareStructural(AddChild)",
		"multi_scene_oracle.gltf", HistoryPolicy::undoable, TotalPolicyState::prepared},
	{TotalPolicyOperation::prepareAddLight, "prepareStructural(AddLight)",
		"multi_scene_oracle.gltf", HistoryPolicy::undoable, TotalPolicyState::prepared},
	{TotalPolicyOperation::prepareDuplicate, "prepareStructural(Duplicate)",
		"multi_scene_oracle.gltf", HistoryPolicy::undoable, TotalPolicyState::prepared},
	{TotalPolicyOperation::prepareDelete, "prepareStructural(Delete)",
		"multi_scene_oracle.gltf", HistoryPolicy::undoable, TotalPolicyState::prepared},
	{TotalPolicyOperation::prepareReparent, "prepareStructural(Reparent)",
		"multi_scene_oracle.gltf", HistoryPolicy::undoable, TotalPolicyState::prepared},
	{TotalPolicyOperation::commitAddChild, "commitStructural(AddChild)",
		"multi_scene_oracle.gltf", HistoryPolicy::undoable, TotalPolicyState::applied},
	{TotalPolicyOperation::commitAddLight, "commitStructural(AddLight)",
		"multi_scene_oracle.gltf", HistoryPolicy::undoable, TotalPolicyState::applied},
	{TotalPolicyOperation::commitDuplicate, "commitStructural(Duplicate)",
		"multi_scene_oracle.gltf", HistoryPolicy::undoable, TotalPolicyState::applied},
	{TotalPolicyOperation::commitDelete, "commitStructural(Delete)",
		"multi_scene_oracle.gltf", HistoryPolicy::undoable, TotalPolicyState::applied},
	{TotalPolicyOperation::commitReparent, "commitStructural(Reparent)",
		"multi_scene_oracle.gltf", HistoryPolicy::undoable, TotalPolicyState::applied},
	{TotalPolicyOperation::executeAddChild, "executeStructural(AddChild)",
		"multi_scene_oracle.gltf", HistoryPolicy::undoable, TotalPolicyState::applied},
	{TotalPolicyOperation::executeAddLight, "executeStructural(AddLight)",
		"multi_scene_oracle.gltf", HistoryPolicy::undoable, TotalPolicyState::applied},
	{TotalPolicyOperation::executeDuplicate, "executeStructural(Duplicate)",
		"multi_scene_oracle.gltf", HistoryPolicy::undoable, TotalPolicyState::applied},
	{TotalPolicyOperation::executeDelete, "executeStructural(Delete)",
		"multi_scene_oracle.gltf", HistoryPolicy::undoable, TotalPolicyState::applied},
	{TotalPolicyOperation::executeReparent, "executeStructural(Reparent)",
		"multi_scene_oracle.gltf", HistoryPolicy::undoable, TotalPolicyState::applied},
	{TotalPolicyOperation::restoreStructural, "restoreStructural",
		"multi_scene_oracle.gltf", HistoryPolicy::undoable, TotalPolicyState::applied},

	{TotalPolicyOperation::renameInvalid, "rename(invalid kind)",
		"feature_preservation.gltf", HistoryPolicy::nonHistory, TotalPolicyState::rejected},
	{TotalPolicyOperation::renameScene, "rename(scene)",
		"feature_preservation.gltf", HistoryPolicy::nonHistory,
		TotalPolicyState::applied, true},
	{TotalPolicyOperation::renameNode, "rename(node)",
		"feature_preservation.gltf", HistoryPolicy::undoable,
		TotalPolicyState::applied, true},
	{TotalPolicyOperation::renameStaleNode, "rename(stale node-shaped ID)",
		"feature_preservation.gltf", HistoryPolicy::undoable, TotalPolicyState::rejected},
	{TotalPolicyOperation::renameForeignNode, "rename(foreign node-shaped ID)",
		"feature_preservation.gltf", HistoryPolicy::undoable, TotalPolicyState::rejected},
	{TotalPolicyOperation::renamePrimitive, "rename(primitive)",
		"feature_preservation.gltf", HistoryPolicy::nonHistory, TotalPolicyState::rejected},
	{TotalPolicyOperation::renameMesh, "rename(mesh)",
		"feature_preservation.gltf", HistoryPolicy::nonHistory,
		TotalPolicyState::applied, true},
	{TotalPolicyOperation::renameMaterial, "rename(material)",
		"feature_preservation.gltf", HistoryPolicy::nonHistory,
		TotalPolicyState::applied, true},
	{TotalPolicyOperation::renameTexture, "rename(texture)",
		"feature_preservation.gltf", HistoryPolicy::nonHistory,
		TotalPolicyState::applied, true},
	{TotalPolicyOperation::renameImage, "rename(image)",
		"feature_preservation.gltf", HistoryPolicy::nonHistory,
		TotalPolicyState::applied, true},
	{TotalPolicyOperation::renameSampler, "rename(sampler unsupported/stale)",
		"feature_preservation.gltf", HistoryPolicy::nonHistory, TotalPolicyState::rejected},
	{TotalPolicyOperation::renameCamera, "rename(camera)",
		"feature_preservation.gltf", HistoryPolicy::nonHistory,
		TotalPolicyState::applied, true},
	{TotalPolicyOperation::renameLight, "rename(light)",
		"feature_preservation.gltf", HistoryPolicy::nonHistory,
		TotalPolicyState::applied, true},
	{TotalPolicyOperation::renameAccessor, "rename(accessor)",
		"feature_preservation.gltf", HistoryPolicy::nonHistory,
		TotalPolicyState::applied, true},
	{TotalPolicyOperation::renameBufferView, "rename(bufferView)",
		"feature_preservation.gltf", HistoryPolicy::nonHistory,
		TotalPolicyState::applied, true},
	{TotalPolicyOperation::renameBuffer, "rename(buffer)",
		"feature_preservation.gltf", HistoryPolicy::nonHistory,
		TotalPolicyState::applied, true},
	{TotalPolicyOperation::renameSkin, "rename(skin)",
		"feature_preservation.gltf", HistoryPolicy::nonHistory,
		TotalPolicyState::applied, true},
	{TotalPolicyOperation::renameAnimation, "rename(animation)",
		"feature_preservation.gltf", HistoryPolicy::nonHistory,
		TotalPolicyState::applied, true},
	{TotalPolicyOperation::renameCount, "rename(count kind)",
		"feature_preservation.gltf", HistoryPolicy::nonHistory, TotalPolicyState::rejected},

	{TotalPolicyOperation::setNodeTransform, "setNodeTransform",
		"material_extensions.gltf", HistoryPolicy::undoable,
		TotalPolicyState::applied, true},
	{TotalPolicyOperation::attachMesh, "attachMesh", "material_extensions.gltf",
		HistoryPolicy::nonHistory, TotalPolicyState::applied, true},
	{TotalPolicyOperation::attachCamera, "attachCamera", "material_extensions.gltf",
		HistoryPolicy::nonHistory, TotalPolicyState::applied, true},
	{TotalPolicyOperation::attachLight, "attachLight", "material_extensions.gltf",
		HistoryPolicy::nonHistory, TotalPolicyState::applied, true},
	{TotalPolicyOperation::setVisibility, "setVisibility", "material_extensions.gltf",
		HistoryPolicy::nonHistory, TotalPolicyState::applied, true},
	{TotalPolicyOperation::removeVisibility, "removeVisibility", "material_extensions.gltf",
		HistoryPolicy::nonHistory, TotalPolicyState::applied, true},
	{TotalPolicyOperation::setMaterial, "setMaterial", "material_extensions.gltf",
		HistoryPolicy::undoable, TotalPolicyState::applied, true},
	{TotalPolicyOperation::setMaterialTexture, "setMaterialTexture",
		"material_extensions.gltf", HistoryPolicy::undoable,
		TotalPolicyState::applied, true},
	{TotalPolicyOperation::setMaterialExtension, "setMaterialExtension",
		"material_extensions.gltf", HistoryPolicy::undoable,
		TotalPolicyState::applied, true},
	{TotalPolicyOperation::setMaterialSnapshot, "setMaterialSnapshot",
		"material_extensions.gltf", HistoryPolicy::undoable,
		TotalPolicyState::applied, true},
	{TotalPolicyOperation::assignPrimitiveMaterial, "assignPrimitiveMaterial",
		"material_extensions.gltf", HistoryPolicy::nonHistory,
		TotalPolicyState::applied, true},
	{TotalPolicyOperation::duplicatePrimitive, "duplicatePrimitive",
		"material_extensions.gltf", HistoryPolicy::nonHistory, TotalPolicyState::applied},
	{TotalPolicyOperation::splitPrimitiveNodeAware, "splitPrimitive(NodeId, PrimitiveId)",
		"r09_shared_mesh.gltf", HistoryPolicy::nonHistory, TotalPolicyState::applied},
	{TotalPolicyOperation::splitPrimitiveCompatibility, "splitPrimitive(PrimitiveId compatibility)",
		"material_extensions.gltf", HistoryPolicy::nonHistory, TotalPolicyState::applied},
	{TotalPolicyOperation::mergeMeshesNodeAware, "mergeMeshes(NodeId)",
		"r09_shared_mesh.gltf", HistoryPolicy::nonHistory, TotalPolicyState::applied},
	{TotalPolicyOperation::mergeMeshesCompatibility, "mergeMeshes(MeshId, MeshId compatibility)",
		"r09_shared_mesh.gltf", HistoryPolicy::nonHistory, TotalPolicyState::applied},
	{TotalPolicyOperation::setCamera, "setCamera", "material_extensions.gltf",
		HistoryPolicy::nonHistory, TotalPolicyState::applied, true},
	{TotalPolicyOperation::setCameraFromView, "setCameraFromView",
		"material_extensions.gltf", HistoryPolicy::nonHistory,
		TotalPolicyState::applied, true},
	{TotalPolicyOperation::setLight, "setLight", "material_extensions.gltf",
		HistoryPolicy::undoable, TotalPolicyState::applied, true},
	{TotalPolicyOperation::setLightSnapshot, "setLightSnapshot",
		"material_extensions.gltf", HistoryPolicy::undoable,
		TotalPolicyState::applied, true},
	{TotalPolicyOperation::createLight, "createLight", "material_extensions.gltf",
		HistoryPolicy::undoable, TotalPolicyState::applied},
	{TotalPolicyOperation::deleteLight, "deleteLight", "material_extensions.gltf",
		HistoryPolicy::nonHistory, TotalPolicyState::applied},
	{TotalPolicyOperation::setPrimitiveVariant, "setPrimitiveVariant",
		"material_extensions.gltf", HistoryPolicy::nonHistory,
		TotalPolicyState::applied, true},
	{TotalPolicyOperation::previewSceneRootTransform, "previewSceneRootTransform",
		"multi_scene_oracle.gltf", HistoryPolicy::nonHistory,
		TotalPolicyState::applied, true, 0u},
	{TotalPolicyOperation::resetSceneRootTransform, "resetSceneRootTransform",
		"multi_scene_oracle.gltf", HistoryPolicy::nonHistory,
		TotalPolicyState::applied, true, 0u},
	{TotalPolicyOperation::sceneEditorAddRootNode, "SceneEditor::addRootNode inner mutation",
		"multi_scene_oracle.gltf", HistoryPolicy::nonHistory, TotalPolicyState::applied},
};

static_assert(std::size(kTotalPolicyContracts)
	== static_cast<std::size_t>(TotalPolicyOperation::count));

const char* totalGuardLabel(TotalGuardInput input)
{
	switch(input)
	{
		case TotalGuardInput::none: return "valid";
		case TotalGuardInput::invalidToken: return "invalid-token";
		case TotalGuardInput::staleRevision: return "stale-revision";
		case TotalGuardInput::staleToken: return "stale-token";
		case TotalGuardInput::foreignToken: return "foreign-token";
	}
	return "unknown";
}

MutationErrorCode totalGuardError(TotalGuardInput input)
{
	switch(input)
	{
		case TotalGuardInput::invalidToken:
		case TotalGuardInput::staleToken:
			return MutationErrorCode::staleTransaction;
		case TotalGuardInput::staleRevision:
			return MutationErrorCode::staleRevision;
		case TotalGuardInput::foreignToken:
			return MutationErrorCode::crossDocument;
		case TotalGuardInput::none:
			return MutationErrorCode::none;
	}
	return MutationErrorCode::internalError;
}

bool verifyTotalPolicyAttempt(
	const TotalPolicyContract& contract,
	const TotalPolicyAttempt& attempt,
	TotalPolicyState expectedState,
	TotalGuardInput guardInput,
	std::string_view partition)
{
	const auto& result = attempt.observation;
	bool passed = result.policy != HistoryPolicy::unclassified
		&& result.policy == contract.policy && result.state == expectedState;
	if(guardInput != TotalGuardInput::none)
		passed = passed && result.error == totalGuardError(guardInput);
	if(expectedState == TotalPolicyState::applied)
	{
		passed = passed
			&& attempt.after.revision
				== attempt.before.revision + contract.appliedRevisionDelta
			&& attempt.after != attempt.before;
		if(result.mutationResult)
			passed = passed
				&& result.resultBeforeRevision == attempt.before.revision
				&& result.resultAfterRevision == attempt.after.revision;
	}
	else
	{
		passed = passed && attempt.after == attempt.before;
		if(result.mutationResult)
			passed = passed
				&& result.resultBeforeRevision == attempt.before.revision
				&& result.resultAfterRevision == attempt.before.revision;
	}
	if(!passed)
	{
		std::cerr << "FAILED: total HistoryPolicy " << contract.label << ' '
			<< partition << " expected-policy="
			<< static_cast<unsigned>(contract.policy) << " actual-policy="
			<< static_cast<unsigned>(result.policy) << " expected-state="
			<< static_cast<unsigned>(expectedState) << " actual-state="
			<< static_cast<unsigned>(result.state) << " error="
			<< static_cast<unsigned>(result.error) << " before-revision="
			<< attempt.before.revision << " after-revision="
			<< attempt.after.revision << '\n';
	}
	return passed;
}

bool runTotalHistoryPolicyReleaseProof()
{
	bool passed = true;
	constexpr TotalGuardInput guards[] = {
		TotalGuardInput::invalidToken,
		TotalGuardInput::staleRevision,
		TotalGuardInput::staleToken,
		TotalGuardInput::foreignToken,
	};
	const auto run = [&](const TotalPolicyContract& contract,
		TotalGuardInput guardInput, bool unchanged,
		TotalPolicyState expectedState, std::string_view partition)
	{
		auto document = loadTestDocument(contract.fixtureName);
		auto foreign = loadTestDocument(contract.fixtureName);
		if(!document || !foreign)
		{
			std::cerr << "FAILED: total HistoryPolicy fixture for "
				<< contract.label << '\n';
			return false;
		}
		SceneCommands commands(*document);
		SceneCommands foreignCommands(*foreign);
		TotalPolicyContext context{
			*document, commands, *foreign, foreignCommands};
		const TotalPolicyAttempt attempt = performTotalPolicyOperation(
			context, contract.operation, guardInput, unchanged);
		return verifyTotalPolicyAttempt(
			contract, attempt, expectedState, guardInput, partition);
	};
	for(const TotalPolicyContract& contract : kTotalPolicyContracts)
	{
		passed &= run(contract, TotalGuardInput::none, false,
			contract.validState, "valid/success");
		for(const TotalGuardInput guard : guards)
			passed &= run(contract, guard, false, TotalPolicyState::rejected,
				totalGuardLabel(guard));
		if(contract.hasUnchangedInput)
			passed &= run(contract, TotalGuardInput::none, true,
				TotalPolicyState::domainNoop, "unchanged/no-op");
	}
	return passed;
}

struct LifecyclePolicyContract final
{
	const char* label;
	HistoryLifecycleEvent event;
	LifecycleCommitState state;
	HistoryPolicy policy;
	bool clears;
};

constexpr LifecyclePolicyContract kLifecyclePolicyContracts[] = {
	{"Merge failed", HistoryLifecycleEvent::merge,
		LifecycleCommitState::failed, HistoryPolicy::clearOnSuccessfulCommit, false},
	{"Merge no-op", HistoryLifecycleEvent::merge,
		LifecycleCommitState::noOp, HistoryPolicy::clearOnSuccessfulCommit, false},
	{"Merge changed", HistoryLifecycleEvent::merge,
		LifecycleCommitState::changed, HistoryPolicy::clearOnSuccessfulCommit, true},
	{"Compact failed", HistoryLifecycleEvent::compact,
		LifecycleCommitState::failed, HistoryPolicy::clearOnSuccessfulCommit, false},
	{"Compact no-op", HistoryLifecycleEvent::compact,
		LifecycleCommitState::noOp, HistoryPolicy::clearOnSuccessfulCommit, false},
	{"Compact changed", HistoryLifecycleEvent::compact,
		LifecycleCommitState::changed, HistoryPolicy::clearOnSuccessfulCommit, true},
	{"Save As failed", HistoryLifecycleEvent::saveAs,
		LifecycleCommitState::failed, HistoryPolicy::notApplicable, false},
	{"Save As no-op", HistoryLifecycleEvent::saveAs,
		LifecycleCommitState::noOp, HistoryPolicy::notApplicable, false},
	{"Save As changed", HistoryLifecycleEvent::saveAs,
		LifecycleCommitState::changed, HistoryPolicy::notApplicable, false},
};

bool runTotalHistoryLifecyclePolicyReleaseProof()
{
	bool passed = true;
	for(const LifecyclePolicyContract& contract : kLifecyclePolicyContracts)
	{
		auto document = loadTestDocument("minimal_scene.gltf");
		if(!document)
			return expect(false, "lifecycle total-policy fixture publishes");
		SceneEditor editor(*document);
		const NodeId node = totalLiveId<NodeId>(
			*document, EditorObjectKind::node);
		NodeTransform changed = *editor.commands().nodeTransform(node);
		changed.translation[0] += 1.0;
		ContinuousFrameSignals finish;
		finish.applicationFrame = 1u;
		finish.inspectorDeactivatedAfterEdit = true;
		const bool seeded = editor.continuousEdit()
				.beginTransform(ContinuousEditKind::inspectorTransform, node).status
				== ContinuousStatus::began
			&& editor.continuousEdit().preview(changed).status
				== ContinuousStatus::previewApplied
			&& editor.continuousEdit().finishFrame(finish).status
				== ContinuousStatus::committed
			&& editor.history().undoCount() == 1u;
		if(!seeded)
		{
			std::cerr << "FAILED: lifecycle seed for " << contract.label << '\n';
			passed = false;
			continue;
		}
		const std::size_t undoBefore = editor.history().undoCount();
		const std::size_t redoBefore = editor.history().redoCount();
		const std::string undoDescription(editor.history().undoDescription());
		const std::string redoDescription(editor.history().redoDescription());
		const auto timeBefore =
			EditorStateInspector::inspect(editor.history()).lastContinuousPush;
		applyHistoryLifecycle(editor.history(), contract.event, contract.state);
		const auto timeAfter =
			EditorStateInspector::inspect(editor.history()).lastContinuousPush;
		const bool classified = contract.policy != HistoryPolicy::unclassified;
		const bool stateCorrect = contract.clears
			? editor.history().undoCount() == 0u
				&& editor.history().redoCount() == 0u
				&& editor.history().undoDescription().empty()
				&& editor.history().redoDescription().empty()
			: editor.history().undoCount() == undoBefore
				&& editor.history().redoCount() == redoBefore
				&& editor.history().undoDescription() == undoDescription
				&& editor.history().redoDescription() == redoDescription;
		if(!classified || !stateCorrect || timeAfter != timeBefore)
		{
			std::cerr << "FAILED: lifecycle total HistoryPolicy "
				<< contract.label << '\n';
			passed = false;
		}
	}
	return passed;
}

std::vector<std::uint8_t> framedCanonicalString(std::string_view value)
{
	std::vector<std::uint8_t> result(sizeof(std::uint64_t), 0u);
	const std::uint64_t size = static_cast<std::uint64_t>(value.size());
	for(unsigned shift = 0; shift < 64u; shift += 8u)
		result[shift / 8u] = static_cast<std::uint8_t>(size >> shift);
	result.insert(result.end(), value.begin(), value.end());
	return result;
}

std::optional<NodeId> appendTestNode(SceneCommands& commands, SceneId scene,
	std::string name, std::optional<NodeId> parent = std::nullopt)
{
	const MutationResult result = commands.addNode(scene, std::move(name), parent);
	const auto id = addedId(result, EditorObjectKind::node);
	return id ? NodeId::from(*id) : std::nullopt;
}

bool readCanonicalU32(const std::vector<std::uint8_t>& bytes,
	std::size_t& offset,
	std::uint32_t& value) noexcept
{
	if(offset > bytes.size() || bytes.size() - offset < sizeof(value)) return false;
	value = 0u;
	for(unsigned index = 0; index < sizeof(value); ++index)
		value |= static_cast<std::uint32_t>(bytes[offset + index]) << (index * 8u);
	offset += sizeof(value);
	return true;
}

bool readCanonicalU64(const std::vector<std::uint8_t>& bytes,
	std::size_t& offset,
	std::uint64_t& value) noexcept
{
	if(offset > bytes.size() || bytes.size() - offset < sizeof(value)) return false;
	value = 0u;
	for(unsigned index = 0; index < sizeof(value); ++index)
		value |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8u);
	offset += sizeof(value);
	return true;
}

constexpr std::uint32_t canonicalTag(char a, char b, char c, char d) noexcept
{
	return static_cast<std::uint32_t>(static_cast<unsigned char>(a))
		| (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 8u)
		| (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 16u)
		| (static_cast<std::uint32_t>(static_cast<unsigned char>(d)) << 24u);
}

bool canonicalMagicAndVersion(const StructuralDeltaCanonicalSide& side)
{
	constexpr std::array<std::uint8_t, 8> magic{'M', 'G', 'I', 'F', 'S', 'D', 'C', '1'};
	constexpr std::array<std::uint32_t, 8> tags{canonicalTag('D', 'I', 'A', 'G'),
		canonicalTag('R', 'E', 'G', 'I'),
		canonicalTag('S', 'L', 'O', 'T'),
		canonicalTag('R', 'E', 'C', 'D'),
		canonicalTag('M', 'E', 'M', 'B'),
		canonicalTag('P', 'L', 'A', 'N'),
		canonicalTag('O', 'W', 'N', 'R'),
		canonicalTag('E', 'X', 'T', 'N')};
	if(!side.complete() || side.schemaVersion != 1u || side.canonicalBytes.size() < 17u
		|| !std::equal(magic.begin(), magic.end(), side.canonicalBytes.begin()))
		return false;
	std::size_t offset = magic.size();
	std::uint32_t version = 0u;
	std::uint32_t sectionCount = 0u;
	if(!readCanonicalU32(side.canonicalBytes, offset, version)
		|| side.canonicalBytes[offset++] != static_cast<std::uint8_t>(side.side)
		|| !readCanonicalU32(side.canonicalBytes, offset, sectionCount)
		|| version != StructuralDeltaCanonicalSide::kSchemaVersion || sectionCount != tags.size())
		return false;
	for(const std::uint32_t expectedTag : tags)
	{
		std::uint32_t tag = 0u;
		std::uint64_t length = 0u;
		if(!readCanonicalU32(side.canonicalBytes, offset, tag)
			|| !readCanonicalU64(side.canonicalBytes, offset, length) || tag != expectedTag
			|| length > side.canonicalBytes.size() - offset)
			return false;
		offset += static_cast<std::size_t>(length);
	}
	return offset == side.canonicalBytes.size();
}

bool canonicalSnapshotMagicAndVersion(const SnapshotCanonicalRecord& record)
{
	constexpr std::array<std::uint8_t, 8> magic{'M', 'G', 'I', 'F', 'S', 'R', 'C', '1'};
	constexpr std::array<std::uint32_t, 3> tags{canonicalTag('M', 'E', 'T', 'A'),
		canonicalTag('R', 'E', 'F', 'S'), canonicalTag('R', 'E', 'C', 'D')};
	if(!record.complete() || record.schemaVersion != SnapshotCanonicalRecord::kSchemaVersion
		|| record.canonicalBytes.size() < 17u
		|| !std::equal(magic.begin(), magic.end(), record.canonicalBytes.begin()))
		return false;
	std::size_t offset = magic.size();
	std::uint32_t version = 0u;
	std::uint32_t sectionCount = 0u;
	if(!readCanonicalU32(record.canonicalBytes, offset, version)
		|| offset >= record.canonicalBytes.size()
		|| record.canonicalBytes[offset++] != static_cast<std::uint8_t>(record.kind)
		|| !readCanonicalU32(record.canonicalBytes, offset, sectionCount)
		|| version != SnapshotCanonicalRecord::kSchemaVersion || sectionCount != tags.size())
		return false;
	for(const std::uint32_t expectedTag : tags)
	{
		std::uint32_t tag = 0u;
		std::uint64_t length = 0u;
		if(!readCanonicalU32(record.canonicalBytes, offset, tag)
			|| !readCanonicalU64(record.canonicalBytes, offset, length) || tag != expectedTag
			|| length > record.canonicalBytes.size() - offset)
			return false;
		offset += static_cast<std::size_t>(length);
	}
	return offset == record.canonicalBytes.size();
}

std::optional<std::vector<std::uint8_t>> canonicalSnapshotSection(
	const SnapshotCanonicalRecord& record, std::uint32_t requestedTag)
{
	if(!canonicalSnapshotMagicAndVersion(record)) return std::nullopt;
	std::size_t offset = 8u + sizeof(std::uint32_t) + sizeof(std::uint8_t);
	std::uint32_t sectionCount = 0u;
	if(!readCanonicalU32(record.canonicalBytes, offset, sectionCount)) return std::nullopt;
	for(std::uint32_t sectionIndex = 0u; sectionIndex < sectionCount; ++sectionIndex)
	{
		std::uint32_t tag = 0u;
		std::uint64_t length = 0u;
		if(!readCanonicalU32(record.canonicalBytes, offset, tag)
			|| !readCanonicalU64(record.canonicalBytes, offset, length)
			|| length > record.canonicalBytes.size() - offset)
			return std::nullopt;
		if(tag == requestedTag)
		{
			return std::vector<std::uint8_t>(record.canonicalBytes.begin()
					+ static_cast<std::ptrdiff_t>(offset),
				record.canonicalBytes.begin()
					+ static_cast<std::ptrdiff_t>(offset + static_cast<std::size_t>(length)));
		}
		offset += static_cast<std::size_t>(length);
	}
	return std::nullopt;
}

bool containsCanonicalBytes(const std::vector<std::uint8_t>& bytes,
	const std::vector<std::uint8_t>& expected)
{
	return std::search(bytes.begin(), bytes.end(), expected.begin(), expected.end())
		!= bytes.end();
}

MaterialExtensionValue inspectionExtensionValue(
	MaterialExtensionValue::Storage leaf)
{
	MaterialExtensionValue value;
	MaterialExtensionValue nested;
	nested.value = std::move(leaf);
	MaterialExtensionValue integer;
	integer.value = std::int32_t{7};
	MaterialExtensionValue real;
	real.value = 7.0;
	MaterialExtensionValue typed;
	typed.value = MaterialExtensionValue::Array{
		std::move(integer), std::move(real)};
	value.value = MaterialExtensionValue::Object{{"nested", std::move(nested)},
		{"typed", std::move(typed)}};
	return value;
}

struct OneShotAllocationFault final : EditorFaultInjector
{
	bool armed{true};

	bool shouldFail(EditorFaultPoint point) noexcept override
	{
		if(armed && point == EditorFaultPoint::allocation)
		{
			armed = false;
			return true;
		}
		return false;
	}
};

struct FailOnNthFault final : EditorFaultInjector
{
	FailOnNthFault(EditorFaultPoint targetPoint, std::size_t targetCall) noexcept
		: point(targetPoint), failOnCall(targetCall)
	{
	}

	bool shouldFail(EditorFaultPoint candidate) noexcept override
	{
		if(candidate != point) return false;
		++calls;
		return calls == failOnCall;
	}

	EditorFaultPoint point;
	std::size_t failOnCall{1u};
	std::size_t calls{0u};
};

bool runNodeNameSpecializedFaultTest()
{
	const auto run = [](EditorFaultPoint point, std::size_t failOnCall,
		MutationErrorCode expectedError, std::string_view label)
	{
		auto document = loadTestDocument("minimal_scene.gltf");
		if(!document) {
			const std::string message = std::string(label) + ": fixture publishes";
			return expect(false, message.c_str());
		}
		const NodeId node = *NodeId::from(
			document->idAt(EditorObjectKind::node, 0u));
		FailOnNthFault fault(point, failOnCall);
		SceneCommands commands(*document, kDefaultEditorLimits, &fault);
		const ExactDocumentSnapshot before = exactSnapshot(*document);
		const MutationToken tokenBefore = commands.token();
		const MutationResult rejected = commands.rename(
			node.untyped(), "faulted-node-name");
		const std::string rejectionMessage = std::string(label) +
			": rejection preserves every document field, revision, digest, ID, and token";
		bool passed = expect(rejected.rejected()
				&& rejected.error == expectedError
				&& fault.calls == failOnCall
				&& exactSnapshot(*document) == before
				&& commands.token() == tokenBefore,
			rejectionMessage.c_str());
		const MutationResult retried = commands.rename(
			node.untyped(), "faulted-node-name");
		const std::string retryMessage = std::string(label) +
			": a clean retry publishes exactly once";
		passed &= expect(retried.applied()
				&& document->objectName(node.untyped()) == "faulted-node-name"
				&& document->revision() == before.revision + 1u,
			retryMessage.c_str());
		return passed;
	};

	bool passed = run(EditorFaultPoint::allocation, 1u,
		MutationErrorCode::allocationFailed,
		"node-name first allocation checkpoint");
	passed &= run(EditorFaultPoint::allocation, 2u,
		MutationErrorCode::allocationFailed,
		"node-name staged target-digest allocation checkpoint");
	passed &= run(EditorFaultPoint::preCandidatePublish, 1u,
		MutationErrorCode::internalError,
		"node-name pre-candidate publication checkpoint");
	return passed;
}

bool runMaterialPropertiesSpecializedFaultTest()
{
	const auto run = [](EditorFaultPoint point, std::size_t failOnCall,
		MutationErrorCode expectedError)
	{
		auto document = loadTestDocument("r09_shared_mesh.gltf");
		if(!document) return expect(false, "material fault fixture publishes");
		const auto material = *MaterialId::from(document->idAt(EditorObjectKind::material, 0u));
		const auto parameters = document->materialParameters(material);
		if(!parameters) return false;
		MaterialProperties edited;
		edited.baseColorFactor = parameters->baseColorFactor;
		edited.metallicFactor = 0.4;
		edited.roughnessFactor = parameters->roughnessFactor;
		edited.emissiveFactor = parameters->emissiveFactor;
		edited.alphaCutoff = parameters->alphaCutoff;
		edited.doubleSided = parameters->doubleSided;
		SceneCommands prepare(*document);
		const auto beforeMaterial = prepare.materialSnapshot(material);
		const auto afterMaterial = prepare.prepareMaterialSnapshot(material, MaterialEdit{edited});
		if(!beforeMaterial || !afterMaterial) return false;
		const auto before = exactSnapshot(*document);
		const auto digest = document->documentDigest();
		FailOnNthFault fault(point, failOnCall);
		SceneCommands commands(*document, kDefaultEditorLimits, &fault);
		const auto token = commands.token();
		const auto rejected = commands.setMaterialSnapshot(material, *afterMaterial);
		bool passed = expect(rejected.rejected() && rejected.error == expectedError &&
				fault.calls == failOnCall && exactSnapshot(*document) == before &&
				document->documentDigest() == digest && commands.token() == token &&
				commands.materialSnapshot(material) == beforeMaterial,
			"material cache failure restores authority, metadata, digests, revision, and token");
		const auto retried = commands.setMaterialSnapshot(material, *afterMaterial);
		const auto canonical = GltfSemanticCompare::compare(*document, *document);
		passed &= expect(retried.applied() && document->revision() == before.revision + 1u &&
				commands.materialSnapshot(material) == afterMaterial && canonical.equivalent() &&
				GltfSemanticCompare::digest(*document) == canonical.leftDigest(),
			"material cache retry matches complete authoritative canonicalization");
		return passed;
	};
	bool passed = run(EditorFaultPoint::allocation, 1u, MutationErrorCode::allocationFailed);
	passed &= run(EditorFaultPoint::allocation, 2u, MutationErrorCode::allocationFailed);
	passed &= run(EditorFaultPoint::preCandidatePublish, 1u, MutationErrorCode::internalError);
	return passed;
}

bool runCrossParentAppendTailTest()
{
	auto document = loadTestDocument("multi_scene_oracle.gltf");
	if(!document) return expect(false, "cross-parent append-tail fixture publishes");
	SceneCommands commands(*document);
	const SceneId scene = *SceneId::from(document->idAt(EditorObjectKind::scene, 0));
	const NodeId oldParent = *NodeId::from(document->idAt(EditorObjectKind::node, 0));
	const NodeId newParent = *NodeId::from(document->idAt(EditorObjectKind::node, 1));
	const NodeId p0 = document->nodeChildIds(oldParent).front();
	const auto moved = appendTestNode(commands, scene, "X", oldParent);
	const auto p1 = appendTestNode(commands, scene, "p1", oldParent);
	const auto q0 = appendTestNode(commands, scene, "q0", newParent);
	if(!moved || !p1 || !q0) return expect(false, "cross-parent test nodes append");
	const auto source = commands.captureNodeOccurrence(scene, *moved);
	const auto target = commands.captureNodeOccurrence(scene, newParent);
	if(!source || !target) return expect(false, "cross-parent occurrences capture");
	StructuralMutationResult executed =
		commands.executeStructural(StructuralRequest{ReparentNodeRequest{*source,
			std::optional<SceneNodeOccurrence>{*target},
			SceneOccurrencePolicy::clickedScene}});
	bool passed = expect(executed.mutation.applied() && executed.hasDelta(),
		"prepared cross-parent Reparent publishes a delta");
	if(!executed.hasDelta()) return false;
	passed &= expect(document->nodeChildIds(oldParent) == std::vector<NodeId>{p0, *p1}
			&& document->nodeChildIds(newParent) == std::vector<NodeId>{*q0, *moved},
		"cross-parent execute appends to the target tail");

	const auto undoBytes = executed.delta->inspectCanonicalSide(StructuralState::undo);
	const auto undoBytesAgain = executed.delta->inspectCanonicalSide(StructuralState::undo);
	const auto redoBytes = executed.delta->inspectCanonicalSide(StructuralState::redo);
	passed &= expect(canonicalMagicAndVersion(undoBytes) && canonicalMagicAndVersion(redoBytes),
		"canonical structural sides carry the locked schema and side marker");
	passed &= expect(undoBytes.canonicalBytes == undoBytesAgain.canonicalBytes,
		"canonical structural inspection is deterministic across repeated calls");
	passed &= expect(undoBytes.canonicalBytes != redoBytes.canonicalBytes,
		"Undo and Redo canonical structural sides remain distinguishable");
	const auto invalidState =
		executed.delta->inspectCanonicalSide(static_cast<StructuralState>(0xffu));
	passed &= expect(invalidState.status == StructuralDeltaInspectionStatus::invalidState
			&& invalidState.canonicalBytes.empty(),
		"invalid structural inspection state fails closed with no bytes");

	const auto y = appendTestNode(commands, scene, "Y", oldParent);
	const auto z = appendTestNode(commands, scene, "Z", newParent);
	if(!y || !z) return expect(false, "cross-parent foreign nodes append");
	const EditorObjectId movedIdentity = moved->untyped();
	MutationResult undo = commands.restoreStructural(*executed.delta, StructuralState::undo);
	passed &= expect(undo.applied()
			&& document->nodeChildIds(oldParent) == std::vector<NodeId>{p0, *p1, *moved, *y}
			&& document->nodeChildIds(newParent) == std::vector<NodeId>{*q0, *z},
		"cross-parent Undo uses append-tail predecessor before "
		"later foreign nodes");
	passed &= expect(document->nodeChildIds(oldParent) != std::vector<NodeId>{p0, *moved, *p1, *y},
		"cross-parent Undo does not restore the literal original sibling slot");
	MutationResult redo = commands.restoreStructural(*executed.delta, StructuralState::redo);
	passed &= expect(redo.applied()
			&& document->nodeChildIds(oldParent) == std::vector<NodeId>{p0, *p1, *y}
			&& document->nodeChildIds(newParent) == std::vector<NodeId>{*q0, *moved, *z},
		"cross-parent Redo preserves foreign order and reuses the "
		"captured predecessor");
	undo = commands.restoreStructural(*executed.delta, StructuralState::undo);
	redo = commands.restoreStructural(*executed.delta, StructuralState::redo);
	passed &= expect(undo.applied() && redo.applied() && document->contains(movedIdentity)
			&& moved->untyped() == movedIdentity,
		"repeated Reparent generations preserve the moved stable ID");

	StructuralDelta relocated(std::move(*executed.delta));
	const auto movedFrom = executed.delta->inspectCanonicalSide(StructuralState::undo);
	passed &= expect(movedFrom.status == StructuralDeltaInspectionStatus::invalidDelta
			&& movedFrom.canonicalBytes.empty() && relocated.valid(),
		"moved-from structural deltas inspect as invalid without exposing bytes");
	return passed;
}

bool runCrossSceneRootTargetTest()
{
	auto document = loadTestDocument("multi_scene_oracle.gltf");
	if(!document) return expect(false, "cross-scene root-target fixture publishes");
	SceneCommands commands(*document);
	const SceneId sourceScene = *SceneId::from(
		document->idAt(EditorObjectKind::scene, 0u));
	const SceneId targetScene = *SceneId::from(
		document->idAt(EditorObjectKind::scene, 1u));
	const NodeId sourceNode = *NodeId::from(
		document->idAt(EditorObjectKind::node, 1u));
	const auto source = commands.captureNodeOccurrence(sourceScene, sourceNode);
	if(!source) return expect(false, "cross-scene root-target occurrence captures");
	const auto sourceRootsBefore = document->sceneRootNodeIds(sourceScene);
	const auto targetRootsBefore = document->sceneRootNodeIds(targetScene);
	const StructuralMutationResult executed = commands.executeStructural(
		StructuralRequest{ReparentNodeRequest{
			*source, std::nullopt, SceneOccurrencePolicy::clickedScene,
			targetScene}});
	bool passed = expect(executed.mutation.applied() && executed.hasDelta(),
		"cross-scene root drop publishes a structural delta");
	if(!executed.hasDelta()) return false;
	const auto sourceRootsAfter = document->sceneRootNodeIds(sourceScene);
	const auto targetRootsAfter = document->sceneRootNodeIds(targetScene);
	auto expectedSourceRoots = sourceRootsBefore;
	expectedSourceRoots.erase(
		std::remove(expectedSourceRoots.begin(), expectedSourceRoots.end(), sourceNode),
		expectedSourceRoots.end());
	auto expectedTargetRoots = targetRootsBefore;
	expectedTargetRoots.push_back(sourceNode);
	passed &= expect(sourceRootsAfter == expectedSourceRoots &&
					 targetRootsAfter == expectedTargetRoots,
		"cross-scene root drop removes from source and appends to clicked target scene");

	const MutationResult undo = commands.restoreStructural(
		*executed.delta, StructuralState::undo);
	const auto undoSourceRoots = document->sceneRootNodeIds(sourceScene);
	const auto undoTargetRoots = document->sceneRootNodeIds(targetScene);
	auto expectedUndoSourceRoots = sourceRootsBefore;
	expectedUndoSourceRoots.erase(std::remove(expectedUndoSourceRoots.begin(),
		expectedUndoSourceRoots.end(), sourceNode), expectedUndoSourceRoots.end());
	expectedUndoSourceRoots.push_back(sourceNode);
	passed &= expect(undo.applied() &&
					 undoSourceRoots == expectedUndoSourceRoots &&
					 undoTargetRoots == targetRootsBefore,
		"cross-scene root drop Undo restores both scene root vectors with append-tail ordering");
	const MutationResult redo = commands.restoreStructural(
		*executed.delta, StructuralState::redo);
	passed &= expect(redo.applied() &&
					 document->sceneRootNodeIds(sourceScene) == expectedSourceRoots &&
					 document->sceneRootNodeIds(targetScene) == expectedTargetRoots,
		"cross-scene root drop Redo retains the explicit target scene");
	return passed;
}

bool runRootParentAppendTailTest()
{
	bool passed = true;
	{
		auto document = loadTestDocument("multi_scene_oracle.gltf");
		if(!document) return expect(false, "root-to-parent append-tail fixture publishes");
		SceneCommands commands(*document);
		const SceneId scene = *SceneId::from(document->idAt(EditorObjectKind::scene, 0));
		const NodeId parent = *NodeId::from(document->idAt(EditorObjectKind::node, 0));
		const NodeId moved = *NodeId::from(document->idAt(EditorObjectKind::node, 1));
		const NodeId firstRoot = document->sceneRootNodeIds(scene).front();
		const NodeId lastRoot = document->sceneRootNodeIds(scene).back();
		const NodeId firstChild = document->nodeChildIds(parent).front();
		const auto source = commands.captureNodeOccurrence(scene, moved);
		const auto target = commands.captureNodeOccurrence(scene, parent);
		if(!source || !target) return expect(false, "root-to-parent occurrences capture");
		auto executed = commands.executeStructural(StructuralRequest{ReparentNodeRequest{*source,
			std::optional<SceneNodeOccurrence>{*target},
			SceneOccurrencePolicy::clickedScene}});
		if(!executed.hasDelta()) return expect(false, "root-to-parent delta publishes");
		const auto y = appendTestNode(commands, scene, "root-Y");
		const auto z = appendTestNode(commands, scene, "child-Z", parent);
		if(!y || !z) return expect(false, "root-to-parent foreign nodes append");
		const auto undo = commands.restoreStructural(*executed.delta, StructuralState::undo);
		passed &= expect(undo.applied()
				&& document->sceneRootNodeIds(scene)
					== std::vector<NodeId>{firstRoot, lastRoot, moved, *y}
				&& document->nodeChildIds(parent) == std::vector<NodeId>{firstChild, *z},
			"root-to-parent Undo targets root append-tail before foreign roots");
		passed &= expect(document->sceneRootNodeIds(scene)
				!= std::vector<NodeId>{firstRoot, moved, lastRoot, *y},
			"root-to-parent Undo rejects literal root-slot restoration");
		const auto redo = commands.restoreStructural(*executed.delta, StructuralState::redo);
		passed &= expect(redo.applied()
				&& document->sceneRootNodeIds(scene) == std::vector<NodeId>{firstRoot, lastRoot, *y}
				&& document->nodeChildIds(parent) == std::vector<NodeId>{firstChild, moved, *z},
			"root-to-parent Redo preserves later root/child foreign order");
	}
	{
		auto document = loadTestDocument("multi_scene_oracle.gltf");
		if(!document) return expect(false, "parent-to-root append-tail fixture publishes");
		SceneCommands commands(*document);
		const SceneId scene = *SceneId::from(document->idAt(EditorObjectKind::scene, 0));
		const NodeId parent = *NodeId::from(document->idAt(EditorObjectKind::node, 1));
		const auto p0 = appendTestNode(commands, scene, "p0", parent);
		const auto moved = appendTestNode(commands, scene, "X", parent);
		const auto p1 = appendTestNode(commands, scene, "p1", parent);
		if(!p0 || !moved || !p1) return expect(false, "parent-to-root source list appends");
		const auto rootsBefore = document->sceneRootNodeIds(scene);
		const auto source = commands.captureNodeOccurrence(scene, *moved);
		if(!source) return expect(false, "parent-to-root occurrence captures");
		auto executed = commands.executeStructural(StructuralRequest{
			ReparentNodeRequest{*source, std::nullopt, SceneOccurrencePolicy::clickedScene}});
		if(!executed.hasDelta()) return expect(false, "parent-to-root delta publishes");
		const auto y = appendTestNode(commands, scene, "child-Y", parent);
		const auto z = appendTestNode(commands, scene, "root-Z");
		if(!y || !z) return expect(false, "parent-to-root foreign nodes append");
		const auto undo = commands.restoreStructural(*executed.delta, StructuralState::undo);
		passed &= expect(
			undo.applied()
				&& document->nodeChildIds(parent) == std::vector<NodeId>{*p0, *p1, *moved, *y}
				&& document->sceneRootNodeIds(scene) ==
					[&]
					{
						auto expected = rootsBefore;
						expected.push_back(*z);
						return expected;
					}(),
			"parent-to-root Undo appends to the old parent before foreign "
			"children");
		passed &=
			expect(document->nodeChildIds(parent) != std::vector<NodeId>{*p0, *moved, *p1, *y},
				"parent-to-root Undo rejects literal child-slot restoration");
		const auto redo = commands.restoreStructural(*executed.delta, StructuralState::redo);
		auto expectedRoots = rootsBefore;
		expectedRoots.push_back(*moved);
		expectedRoots.push_back(*z);
		passed &= expect(redo.applied()
				&& document->nodeChildIds(parent) == std::vector<NodeId>{*p0, *p1, *y}
				&& document->sceneRootNodeIds(scene) == expectedRoots,
			"parent-to-root Redo inserts before later foreign roots");
	}
	return passed;
}

bool runStructuralFailureAndInspectionTest()
{
	bool passed = true;
	{
		auto document = loadTestDocument("multi_scene_oracle.gltf");
		if(!document) return expect(false, "structural failure fixture publishes");
		SceneCommands setup(*document);
		const SceneId scene = *SceneId::from(document->idAt(EditorObjectKind::scene, 0));
		const NodeId oldParent = *NodeId::from(document->idAt(EditorObjectKind::node, 0));
		const NodeId newParent = *NodeId::from(document->idAt(EditorObjectKind::node, 1));
		const auto moved = appendTestNode(setup, scene, "failure-X", oldParent);
		if(!moved) return expect(false, "structural failure source appends");
		const auto source = setup.captureNodeOccurrence(scene, *moved);
		const auto target = setup.captureNodeOccurrence(scene, newParent);
		if(!source || !target) return expect(false, "structural failure occurrences capture");
		OneShotAllocationFault fault;
		SceneCommands failing(*document, kDefaultEditorLimits, &fault);
		const ExactDocumentSnapshot before = exactSnapshot(*document);
		const auto prepared =
			failing.prepareStructural(StructuralRequest{ReparentNodeRequest{*source,
				std::optional<SceneNodeOccurrence>{*target},
				SceneOccurrencePolicy::clickedScene}});
		passed &=
			expect(!prepared.succeeded() && prepared.error == MutationErrorCode::allocationFailed
					&& exactSnapshot(*document) == before,
				"allocation failure during structural preparation is an exact "
				"no-write");
	}
	{
		auto document = loadTestDocument("multi_scene_oracle.gltf");
		if(!document) return expect(false, "validation failure fixture publishes");
		EditorLimits limits = kDefaultEditorLimits;
		limits.maxNodeEdges = 3u;
		SceneCommands validating(*document, limits);
		const SceneId scene = *SceneId::from(document->idAt(EditorObjectKind::scene, 0));
		const NodeId parent = *NodeId::from(document->idAt(EditorObjectKind::node, 1));
		const ExactDocumentSnapshot before = exactSnapshot(*document);
		const auto prepared = validating.prepareStructural(
			StructuralRequest{AddChildNodeRequest{scene, parent, "validation-X"}});
		passed &=
			expect(!prepared.succeeded() && prepared.error == MutationErrorCode::validationFailed
					&& exactSnapshot(*document) == before,
				"validation failure during structural preparation is an exact "
				"no-write");
	}
	{
		auto document = loadTestDocument("multi_scene_oracle.gltf");
		if(!document) return expect(false, "stale structural restore fixture publishes");
		SceneCommands commands(*document);
		const SceneId scene = *SceneId::from(document->idAt(EditorObjectKind::scene, 0));
		const NodeId oldParent = *NodeId::from(document->idAt(EditorObjectKind::node, 0));
		const NodeId newParent = *NodeId::from(document->idAt(EditorObjectKind::node, 1));
		const auto moved = appendTestNode(commands, scene, "stale-X", oldParent);
		if(!moved) return expect(false, "stale restore source appends");
		const auto source = commands.captureNodeOccurrence(scene, *moved);
		const auto target = commands.captureNodeOccurrence(scene, newParent);
		if(!source || !target) return expect(false, "stale restore occurrences capture");
		auto executed = commands.executeStructural(StructuralRequest{ReparentNodeRequest{*source,
			std::optional<SceneNodeOccurrence>{*target},
			SceneOccurrencePolicy::clickedScene}});
		if(!executed.hasDelta()) return expect(false, "stale restore delta publishes");
		const auto foreignSource = commands.captureNodeOccurrence(scene, *moved);
		if(!foreignSource) return expect(false, "stale restore moved occurrence captures");
		const auto foreignMove = commands.reparentNodeAt(
			*foreignSource, std::nullopt, SceneOccurrencePolicy::clickedScene);
		if(!foreignMove.applied()) return expect(false, "foreign move creates stale ownership");
		const ExactDocumentSnapshot before = exactSnapshot(*document);
		const auto rejected = commands.restoreStructural(*executed.delta, StructuralState::undo);
		passed &=
			expect(rejected.rejected() && rejected.error == MutationErrorCode::staleTransaction
					&& exactSnapshot(*document) == before,
				"stale owned membership rejects structural restore with exact "
				"no-write");
	}
	{
		auto document = loadTestDocument("multi_scene_oracle.gltf");
		if(!document) return expect(false, "same-domain Reparent fixture publishes");
		SceneCommands commands(*document);
		const SceneId scene = *SceneId::from(document->idAt(EditorObjectKind::scene, 0));
		const NodeId root = document->sceneRootNodeIds(scene).front();
		const auto source = commands.captureNodeOccurrence(scene, root);
		if(!source) return expect(false, "same-domain Reparent occurrence captures");
		const ExactDocumentSnapshot before = exactSnapshot(*document);
		auto prepared = commands.prepareStructural(StructuralRequest{
			ReparentNodeRequest{*source, std::nullopt, SceneOccurrencePolicy::clickedScene}});
		passed &= expect(prepared.succeeded() && prepared.prepared->snapshotBytes() == 0u,
			"same root domain prepares the exact no-op envelope without a delta");
		auto committed = commands.commitStructural(std::move(prepared.prepared));
		passed &= expect(committed.mutation.domainNoop() && !committed.hasDelta()
				&& exactSnapshot(*document) == before,
			"same root domain commit preserves the exact document envelope");
	}
	return passed;
}

bool runCanonicalCategoryTest()
{
	bool passed = true;
	auto document = loadTestDocument("feature_preservation.gltf");
	if(!document) return expect(false, "canonical retained-category fixture publishes");
	SceneCommands commands(*document);
	const SceneId scene = *SceneId::from(document->idAt(EditorObjectKind::scene, 0));
	const NodeId parent = *NodeId::from(document->idAt(EditorObjectKind::node, 1));
	std::string embedded{"A\0B", 3u};
	auto add =
		commands.executeStructural(StructuralRequest{AddChildNodeRequest{scene, parent, embedded}});
	if(!add.hasDelta()) return expect(false, "embedded-zero structural delta publishes");
	const auto addUndo = add.delta->inspectCanonicalSide(StructuralState::undo);
	const auto addRedo = add.delta->inspectCanonicalSide(StructuralState::redo);
	const std::array<std::uint8_t, 3> embeddedBytes{'A', 0u, 'B'};
	passed &= expect(canonicalMagicAndVersion(addUndo) && canonicalMagicAndVersion(addRedo)
			&& std::search(addRedo.canonicalBytes.begin(),
				   addRedo.canonicalBytes.end(),
				   embeddedBytes.begin(),
				   embeddedBytes.end())
				!= addRedo.canonicalBytes.end(),
		"canonical records retain embedded zero bytes with explicit lengths");
	passed &= expect(addUndo.canonicalBytes != addRedo.canonicalBytes,
		"registry/slot/record/membership ownership changes canonical bytes");

	const auto duplicateSource = commands.captureNodeOccurrence(
		scene, *NodeId::from(document->idAt(EditorObjectKind::node, 0)));
	if(!duplicateSource) return expect(false, "canonical duplicate occurrence captures");
	auto duplicate = commands.executeStructural(StructuralRequest{
		DuplicateNodeRequest{*duplicateSource, std::nullopt, SceneOccurrencePolicy::clickedScene}});
	if(!duplicate.hasDelta()) return expect(false, "canonical duplicate delta publishes");
	const auto duplicateUndo = duplicate.delta->inspectCanonicalSide(StructuralState::undo);
	const auto duplicateRedo = duplicate.delta->inspectCanonicalSide(StructuralState::redo);
	passed &= expect(duplicateUndo.complete() && duplicateRedo.complete()
			&& duplicateUndo.canonicalBytes != duplicateRedo.canonicalBytes,
		"skin/animation/reference and owned collection records affect "
		"canonical bytes");

	LightProperties light;
	light.type = "point";
	auto addLight = commands.executeStructural(
		StructuralRequest{AddLightRequest{scene, std::optional<NodeId>{parent}, "", "L\0", light}});
	if(!addLight.hasDelta()) return expect(false, "canonical light delta publishes");
	const auto lightUndo = addLight.delta->inspectCanonicalSide(StructuralState::undo);
	const auto lightRedo = addLight.delta->inspectCanonicalSide(StructuralState::redo);
	passed &= expect(lightUndo.complete() && lightRedo.complete()
			&& lightUndo.canonicalBytes != lightRedo.canonicalBytes,
		"light records and extension-name ownership/order affect "
		"canonical bytes");

	SceneRootTransform preview;
	preview.translation = {3.0, 0.0, 0.0};
	const auto previewResult = commands.previewSceneRootTransform(scene, preview);
	passed &= expect(previewResult.applied(), "canonical root-state preview applies");
	auto rootStateDelta = commands.executeStructural(
		StructuralRequest{AddChildNodeRequest{scene, parent, "root-state"}});
	if(!rootStateDelta.hasDelta()) return expect(false, "canonical root-state delta publishes");
	const auto rootUndo = rootStateDelta.delta->inspectCanonicalSide(StructuralState::undo);
	const auto rootRedo = rootStateDelta.delta->inspectCanonicalSide(StructuralState::redo);
	passed &= expect(rootUndo.complete() && rootRedo.complete()
			&& rootUndo.canonicalBytes != rootRedo.canonicalBytes,
		"root runtime state, serial, epoch, and frozen roots affect "
		"canonical bytes");
	return passed;
}

constexpr std::string_view kRichMaterialFixture = R"json({
"asset":{"version":"2.0"},
"extensionsUsed":["KHR_lights_punctual","MSFT_lod","MGIF_snapshot_unknown","MGIF_tex","MGIF_spot","MGIF_light"],
"extensions":{"KHR_lights_punctual":{"lights":[{"name":"L\u0000x","type":"spot","color":[0.21,-0.0,0.43],"intensity":2.75,"range":9.5,"spot":{"innerConeAngle":0.12,"outerConeAngle":0.75,"extensions":{"MGIF_spot":{"spotMarker":11}},"extras":{"spotExtra":[1,1.0]}},"extensions":{"MGIF_light":{"lightMarker":13}},"extras":{"lightExtra":{"array":[true,5,5.0,"Q\u0000R"]}}}]}},
"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"extensions":{"KHR_lights_punctual":{"light":0}}}],
"materials":[{"name":"M\u0000x","emissiveFactor":[0.12,0.23,0.34],"alphaMode":"MASK","alphaCutoff":0.45,"doubleSided":true,"pbrMetallicRoughness":{"baseColorFactor":[0.11,0.22,0.33,0.44],"baseColorTexture":{"index":0,"texCoord":1,"extensions":{"MGIF_tex":{"baseMarker":17}},"extras":{"baseExtra":"B"}},"metallicFactor":0.55,"roughnessFactor":0.66,"metallicRoughnessTexture":{"index":1,"texCoord":2,"extras":{"metallicExtra":19}}},"normalTexture":{"index":0,"texCoord":4,"scale":0.77,"extras":{"normalExtra":23}},"occlusionTexture":{"index":1,"texCoord":5,"strength":0.44,"extensions":{"MGIF_tex":{"occlusionMarker":29}}},"emissiveTexture":{"index":0,"texCoord":3,"extras":{"emissiveExtra":31}},"extensions":{"MSFT_lod":{"ids":[1]},"MGIF_snapshot_unknown":{"index":2,"nested":{"array":[true,7,7.0,"E\u0000x"]}}},"extras":{"materialExtra":{"array":[false,3,3.0,"X\u0000Y"]}}},{"name":"LOD-one"},{"name":"LOD-two"}],
"textures":[{"source":0},{"source":0},{"source":0}],"images":[{"uri":"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAusB9Wl2nE0AAAAASUVORK5CYII="}]
})json";

bool runMaterialDuplicateContractTest()
{
	auto document = loadInlineTestDocument(
		kRichMaterialFixture, "material-duplicate-contract");
	if(!document) return expect(false, "material duplicate fixture publishes");
	SceneEditor editor(*document);
	SceneCommands& commands = editor.commands();
	const std::size_t materialCount =
		document->objectCount(EditorObjectKind::material);
	if(materialCount == 0u)
		return expect(false, "material duplicate fixture has a source material");
	std::vector<EditorObjectId> materialIds;
	materialIds.reserve(materialCount);
	for(std::size_t index = 0; index < materialCount; ++index)
		materialIds.push_back(document->idAt(EditorObjectKind::material, index));
	const MaterialId source = *MaterialId::from(materialIds.front());
	const std::string sourceName(document->objectName(source.untyped()));
	const auto sourceSnapshotBefore = commands.materialSnapshot(source);
	if(!sourceSnapshotBefore)
		return expect(false, "material duplicate source snapshot captures");

	const NodeId historyNode = *NodeId::from(
		document->idAt(EditorObjectKind::node, 0u));
	auto renamePrepared = prepareRenameNodeCommand(
		editor.commandContext(), historyNode, "duplicate-history-seed");
	if(!renamePrepared.succeeded())
		return expect(false, "material duplicate history rename prepares");
	const HistoryResult renamed =
		editor.history().execute(std::move(renamePrepared.command));
	const auto beforeTransform = commands.nodeTransform(historyNode);
	if(!beforeTransform)
		return expect(false, "material duplicate history transform captures");
	NodeTransform afterTransform = *beforeTransform;
	afterTransform.translation[0] += 0.25;
	auto transformPrepared = prepareTransformCommand(
		editor.commandContext(), historyNode, *beforeTransform, afterTransform);
	if(!transformPrepared.succeeded())
		return expect(false, "material duplicate history transform prepares");
	const HistoryResult transformed =
		editor.history().execute(std::move(transformPrepared.command));
	const HistoryResult undone = editor.history().undo();
	bool passed = expect(renamed.status == HistoryStatus::pushed
			&& transformed.status == HistoryStatus::pushed
			&& undone.status == HistoryStatus::moved
			&& editor.history().undoCount() == 1u
			&& editor.history().redoCount() == 1u
			&& commands.materialSnapshot(source) == sourceSnapshotBefore,
		"material duplicate starts with populated Undo and Redo stacks and an unchanged source");
	if(!passed) return false;

	const std::size_t undoCount = editor.history().undoCount();
	const std::size_t redoCount = editor.history().redoCount();
	const std::uint64_t retainedHistoryBytes =
		editor.history().retainedSnapshotBytes();
	const std::string undoDescription(editor.history().undoDescription());
	const std::string redoDescription(editor.history().redoDescription());
	const std::uint64_t revision = document->revision();
	const MutationResult duplicate = commands.duplicateMaterial(source);
	const auto addedMaterial = addedId(duplicate, EditorObjectKind::material);

	bool stableRemap = duplicate.remap.materials.size() == materialCount + 1u;
	for(const EditorObjectId id : materialIds)
	{
		const auto entry = std::find_if(duplicate.remap.materials.begin(),
			duplicate.remap.materials.end(),
			[id](const MutationRemapEntry& candidate)
			{
				return candidate.oldId == id;
			});
		stableRemap = stableRemap && entry != duplicate.remap.materials.end()
			&& entry->newId == id;
	}
	const std::size_t addedRemapCount = static_cast<std::size_t>(std::count_if(
		duplicate.remap.materials.begin(), duplicate.remap.materials.end(),
		[](const MutationRemapEntry& entry)
		{
			return !entry.oldId.valid() && entry.newId.valid();
		}));
	passed &= expect(duplicate.applied()
			&& duplicate.historyPolicy == HistoryPolicy::nonHistory
			&& duplicate.beforeRevision == revision
			&& duplicate.afterRevision == revision + 1u
			&& document->revision() == revision + 1u
			&& document->objectCount(EditorObjectKind::material) == materialCount + 1u
			&& addedMaterial && addedRemapCount == 1u && stableRemap,
		"material duplicate publishes one material, one revision, and a complete stable-ID remap");
	if(!addedMaterial) return false;
	const auto copiedValue = MaterialId::from(*addedMaterial);
	if(!copiedValue)
		return expect(false, "material duplicate remap publishes a typed material ID");
	const MaterialId copied = *copiedValue;
	bool originalIdsStable = copied != source
		&& copied.authority() == document->documentAuthority()
		&& document->idAt(EditorObjectKind::material, materialCount) == copied.untyped();
	for(std::size_t index = 0; index < materialIds.size(); ++index)
		originalIdsStable = originalIdsStable
			&& document->idAt(EditorObjectKind::material, index) == materialIds[index];
	passed &= expect(originalIdsStable && document->contains(source.untyped())
			&& document->contains(copied.untyped())
			&& document->indexOf(source.untyped()) == 0u
			&& document->indexOf(copied.untyped()) == materialCount,
		"material duplicate keeps every source ID stable and appends one distinct live ID");
	passed &= expect(editor.history().undoCount() == undoCount
			&& editor.history().redoCount() == redoCount
			&& editor.history().retainedSnapshotBytes() == retainedHistoryBytes
			&& editor.history().undoDescription() == undoDescription
			&& editor.history().redoDescription() == redoDescription,
		"material duplicate leaves populated Undo and Redo history unchanged");

	const std::string copiedName(document->objectName(copied.untyped()));
	const auto sourceProperties = commands.materialProperties(source);
	const auto copiedProperties = commands.materialProperties(copied);
	passed &= expect(document->objectName(source.untyped()) == sourceName
			&& copiedName == sourceName + "_copy"
			&& sourceProperties
			&& sameMaterial(copiedProperties, *sourceProperties),
		"material duplicate preserves public properties and applies only the copy name suffix");
	bool texturesMatch = true;
	for(const MaterialTextureSlot slot : {MaterialTextureSlot::baseColor,
		MaterialTextureSlot::metallicRoughness, MaterialTextureSlot::normal,
		MaterialTextureSlot::occlusion, MaterialTextureSlot::emissive})
	{
		texturesMatch = texturesMatch
			&& commands.materialTexture(source, slot)
				== commands.materialTexture(copied, slot);
	}
	bool extensionsMatch = true;
	for(const std::string_view extension : {std::string_view{"MSFT_lod"},
		std::string_view{"MGIF_snapshot_unknown"}})
	{
		extensionsMatch = extensionsMatch
			&& commands.materialExtension(source, extension)
				== commands.materialExtension(copied, extension);
	}
	passed &= expect(texturesMatch && extensionsMatch
			&& document->objectExtensionsJson(source.untyped())
				== document->objectExtensionsJson(copied.untyped())
			&& document->objectExtrasJson(source.untyped())
				== document->objectExtrasJson(copied.untyped()),
		"material duplicate preserves all public texture slots, extensions, and extras");
	passed &= expect(commands.materialSnapshot(source) == sourceSnapshotBefore,
		"material duplicate leaves the complete source material record unchanged");

	const auto copiedSnapshot = commands.materialSnapshot(copied);
	if(!copiedSnapshot)
		return expect(false, "material duplicate canonical snapshots capture");
	const auto sourceInspection = sourceSnapshotBefore->inspectCanonicalRecord();
	const auto copiedInspection = copiedSnapshot->inspectCanonicalRecord();
	const auto sourceRecord = canonicalSnapshotSection(
		sourceInspection, canonicalTag('R', 'E', 'C', 'D'));
	const auto copiedRecord = canonicalSnapshotSection(
		copiedInspection, canonicalTag('R', 'E', 'C', 'D'));
	const auto sourceReferences = canonicalSnapshotSection(
		sourceInspection, canonicalTag('R', 'E', 'F', 'S'));
	const auto copiedReferences = canonicalSnapshotSection(
		copiedInspection, canonicalTag('R', 'E', 'F', 'S'));
	const auto recordWithoutName = [](const std::vector<std::uint8_t>& record)
		-> std::optional<std::vector<std::uint8_t>>
	{
		std::size_t offset = 0u;
		std::uint64_t nameLength = 0u;
		if(!readCanonicalU64(record, offset, nameLength)
			|| nameLength > record.size() - offset) return std::nullopt;
		offset += static_cast<std::size_t>(nameLength);
		return std::vector<std::uint8_t>(record.begin()
			+ static_cast<std::ptrdiff_t>(offset), record.end());
	};
	const auto sourceRecordWithoutName = sourceRecord
		? recordWithoutName(*sourceRecord) : std::nullopt;
	const auto copiedRecordWithoutName = copiedRecord
		? recordWithoutName(*copiedRecord) : std::nullopt;
	std::size_t referenceOffset = 0u;
	std::uint64_t referenceCount = 0u;
	passed &= expect(sourceRecord && copiedRecord && sourceRecordWithoutName
			&& copiedRecordWithoutName
			&& sourceReferences && copiedReferences
			&& *sourceRecordWithoutName == *copiedRecordWithoutName
			&& *sourceReferences == *copiedReferences
			&& readCanonicalU64(*sourceReferences, referenceOffset, referenceCount)
			&& referenceCount == 3u,
		"material duplicate preserves the complete TinyGLTF record and stable references apart from the intentional copy name");
	return passed;
}

bool runSnapshotCanonicalInspectionTest()
{
	bool passed = true;
	const auto invalidMaterial = MaterialSnapshot{}.inspectCanonicalRecord();
	const auto invalidLight = LightSnapshot{}.inspectCanonicalRecord();
	passed &= expect(invalidMaterial.kind == EditorObjectKind::material
			&& invalidMaterial.status == SnapshotInspectionStatus::invalidSnapshot
			&& invalidMaterial.canonicalBytes.empty()
			&& invalidLight.kind == EditorObjectKind::light
			&& invalidLight.status == SnapshotInspectionStatus::invalidSnapshot
			&& invalidLight.canonicalBytes.empty(),
		"invalid snapshots inspect deterministically without bytes");

	auto document = loadTestDocument("feature_preservation.gltf");
	if(!document) return expect(false, "snapshot inspection fixture publishes");
	SceneCommands commands(*document);
	const MaterialId material =
		*MaterialId::from(document->idAt(EditorObjectKind::material, 0));
	const LightId light = *LightId::from(document->idAt(EditorObjectKind::light, 0));
	const auto materialBefore = commands.materialSnapshot(material);
	const auto materialFresh = commands.materialSnapshot(material);
	const auto lightBefore = commands.lightSnapshot(light);
	if(!materialBefore || !materialFresh || !lightBefore)
		return expect(false, "material and light snapshots capture");

	auto materialBytes = materialBefore->inspectCanonicalRecord();
	const auto materialBytesAgain = materialBefore->inspectCanonicalRecord();
	const auto materialFreshBytes = materialFresh->inspectCanonicalRecord();
	const auto lightBytes = lightBefore->inspectCanonicalRecord();
	passed &= expect(canonicalSnapshotMagicAndVersion(materialBytes)
			&& canonicalSnapshotMagicAndVersion(lightBytes),
		"snapshot canonical records carry the locked schema and bounded sections");
	passed &= expect(materialBytes.canonicalBytes == materialBytesAgain.canonicalBytes
			&& materialBytes.canonicalBytes == materialFreshBytes.canonicalBytes,
		"snapshot canonical inspection is deterministic across repeated and fresh captures");
	passed &= expect(materialBytes.canonicalBytes != lightBytes.canonicalBytes,
		"material and light canonical records are kind-distinct");
	const auto materialBytesSaved = materialBytes.canonicalBytes;
	materialBytes.canonicalBytes.front() ^= 0xffu;
	passed &= expect(materialBefore->inspectCanonicalRecord().canonicalBytes == materialBytesSaved
			&& materialBefore->snapshotBytes() != 0u,
		"returned canonical bytes are an isolated copy and are not retained by the snapshot");

	const auto canonicalIdMatches = [](const std::vector<std::uint8_t>& bytes,
		std::size_t& offset, EditorObjectId expected)
	{
		if(offset >= bytes.size()
			|| bytes[offset++] != static_cast<std::uint8_t>(expected.kind)) return false;
		std::uint32_t slot = 0u;
		std::uint32_t generation = 0u;
		std::uint64_t authority = 0u;
		return readCanonicalU32(bytes, offset, slot)
			&& readCanonicalU32(bytes, offset, generation)
			&& readCanonicalU64(bytes, offset, authority)
			&& slot == expected.slot && generation == expected.generation
			&& authority == expected.authority.value;
	};
	const auto metadataMatches = [&](const SnapshotCanonicalRecord& record,
		EditorDocumentAuthority authority, EditorObjectId id)
	{
		const auto metadata = canonicalSnapshotSection(record, canonicalTag('M', 'E', 'T', 'A'));
		if(!metadata || metadata->size() != 25u) return false;
		std::size_t offset = 0u;
		std::uint64_t encodedAuthority = 0u;
		return readCanonicalU64(*metadata, offset, encodedAuthority)
			&& encodedAuthority == authority.value
			&& canonicalIdMatches(*metadata, offset, id) && offset == metadata->size();
	};
	passed &= expect(metadataMatches(materialFreshBytes,
			materialBefore->authority(), material.untyped())
			&& metadataMatches(lightBytes, lightBefore->authority(), light.untyped()),
		"META exactly encodes authority and the complete stable target ID");

	const auto referenceSection = canonicalSnapshotSection(
		materialFreshBytes, canonicalTag('R', 'E', 'F', 'S'));
	const auto lightReferenceSection = canonicalSnapshotSection(
		lightBytes, canonicalTag('R', 'E', 'F', 'S'));
	std::size_t referenceOffset = 0u;
	std::size_t lightReferenceOffset = 0u;
	std::uint64_t materialReferenceCount = 0u;
	std::uint64_t lightReferenceCount = 1u;
	passed &= expect(referenceSection && lightReferenceSection
			&& readCanonicalU64(*referenceSection, referenceOffset, materialReferenceCount)
			&& readCanonicalU64(*lightReferenceSection, lightReferenceOffset, lightReferenceCount)
			&& materialReferenceCount == 1u && lightReferenceCount == 0u,
		"known material texture references retain stable-ID evidence while lights retain none");
	std::uint32_t textureSourceIndex = std::numeric_limits<std::uint32_t>::max();
	const EditorObjectId expectedTexture = document->idAt(EditorObjectKind::texture, 0u);
	passed &= expect(referenceSection && referenceOffset < referenceSection->size()
			&& (*referenceSection)[referenceOffset++]
				== static_cast<std::uint8_t>(EditorObjectKind::texture)
			&& readCanonicalU32(*referenceSection, referenceOffset, textureSourceIndex)
			&& textureSourceIndex == 0u
			&& canonicalIdMatches(*referenceSection, referenceOffset, expectedTexture)
			&& referenceOffset == referenceSection->size()
			&& lightReferenceSection && lightReferenceOffset == lightReferenceSection->size(),
		"REFS exactly encodes kind, source index, stable ID, and no trailing bytes");

	const auto baseRecord = canonicalSnapshotSection(
		materialFreshBytes, canonicalTag('R', 'E', 'C', 'D'));
	const auto baseLightRecord = canonicalSnapshotSection(
		lightBytes, canonicalTag('R', 'E', 'C', 'D'));
	if(!baseRecord || !baseLightRecord)
		return expect(false, "snapshot record sections decode");
	const auto distinctMaterial = [&](const std::optional<MaterialSnapshot>& snapshot,
		const char* message)
	{
		const auto inspected = snapshot ? snapshot->inspectCanonicalRecord()
			: SnapshotCanonicalRecord{};
		const auto record = canonicalSnapshotSection(inspected, canonicalTag('R', 'E', 'C', 'D'));
		return expect(record && *record != *baseRecord, message);
	};
	const auto distinctLight = [&](const std::optional<LightSnapshot>& snapshot,
		const char* message)
	{
		const auto inspected = snapshot ? snapshot->inspectCanonicalRecord()
			: SnapshotCanonicalRecord{};
		const auto record = canonicalSnapshotSection(inspected, canonicalTag('R', 'E', 'C', 'D'));
		return expect(record && *record != *baseLightRecord, message);
	};

	MaterialProperties properties = *commands.materialProperties(material);
	properties.baseColorFactor[0] = 0.125;
	passed &= distinctMaterial(commands.prepareMaterialSnapshot(material, MaterialEdit{properties}),
		"base-color retained state changes material canonical bytes");
	properties = *commands.materialProperties(material);
	properties.metallicFactor = 0.375;
	passed &= distinctMaterial(commands.prepareMaterialSnapshot(material, MaterialEdit{properties}),
		"metallic retained state changes material canonical bytes");
	properties = *commands.materialProperties(material);
	properties.roughnessFactor = 0.625;
	passed &= distinctMaterial(commands.prepareMaterialSnapshot(material, MaterialEdit{properties}),
		"roughness retained state changes material canonical bytes");
	properties = *commands.materialProperties(material);
	properties.emissiveFactor[1] = 0.25;
	passed &= distinctMaterial(commands.prepareMaterialSnapshot(material, MaterialEdit{properties}),
		"emissive retained state changes material canonical bytes");
	properties = *commands.materialProperties(material);
	properties.alphaCutoff = 0.25;
	properties.alphaMode = "MASK";
	properties.doubleSided = true;
	const auto changedMaterial =
		commands.prepareMaterialSnapshot(material, MaterialEdit{properties});
	passed &= distinctMaterial(changedMaterial,
		"alpha and sidedness retained state changes material canonical bytes");

	const TextureId texture =
		*TextureId::from(document->idAt(EditorObjectKind::texture, 0));
	for(const auto slot : {MaterialTextureSlot::baseColor,
		MaterialTextureSlot::metallicRoughness,
		MaterialTextureSlot::normal,
		MaterialTextureSlot::occlusion,
		MaterialTextureSlot::emissive})
	{
		MaterialTextureEdit edit;
		edit.slot = slot;
		edit.value.texture = texture;
		edit.value.texCoord = static_cast<std::uint32_t>(slot) + 2u;
		edit.value.scalar = slot == MaterialTextureSlot::normal ? 0.5
			: (slot == MaterialTextureSlot::occlusion ? 0.25 : 1.0);
		passed &= distinctMaterial(commands.prepareMaterialSnapshot(material, MaterialEdit{edit}),
			"each texture-info retained category changes material canonical bytes");
	}

	std::string embedded{"A\0B", 3u};
	MaterialExtensionEdit embeddedEdit{"MGIF_snapshot_inspection",
		inspectionExtensionValue(embedded)};
	const auto embeddedSnapshot =
		commands.prepareMaterialSnapshot(material, MaterialEdit{embeddedEdit});
	passed &= distinctMaterial(embeddedSnapshot,
		"nested extension state changes material canonical bytes");
	if(embeddedSnapshot)
	{
		const auto inspected = embeddedSnapshot->inspectCanonicalRecord();
		passed &= expect(containsCanonicalBytes(inspected.canonicalBytes,
				framedCanonicalString(embedded)),
			"embedded-zero strings retain their explicit byte length");
	}
	const auto integerSnapshot = commands.prepareMaterialSnapshot(material,
		MaterialEdit{MaterialExtensionEdit{"MGIF_snapshot_type",
			inspectionExtensionValue(std::int32_t{1})}});
	const auto realSnapshot = commands.prepareMaterialSnapshot(material,
		MaterialEdit{MaterialExtensionEdit{"MGIF_snapshot_type",
			inspectionExtensionValue(1.0)}});
	passed &= expect(integerSnapshot && realSnapshot
			&& integerSnapshot->inspectCanonicalRecord().canonicalBytes
				!= realSnapshot->inspectCanonicalRecord().canonicalBytes,
		"nested extension integer and real alternatives remain type-distinct");
	for(const std::size_t length : {std::size_t{0}, std::size_t{255}, std::size_t{256}})
	{
		const std::string boundary(length, 'x');
		const auto snapshot = commands.prepareMaterialSnapshot(material,
			MaterialEdit{MaterialExtensionEdit{"MGIF_snapshot_length",
				inspectionExtensionValue(boundary)}});
		passed &= expect(snapshot && containsCanonicalBytes(
			snapshot->inspectCanonicalRecord().canonicalBytes,
			framedCanonicalString(boundary)),
			"zero, 255, and 256 byte strings retain exact u64 framing");
	}

	LightProperties lightProperties = *commands.lightProperties(light);
	lightProperties.type = "point";
	passed &= distinctLight(commands.prepareLightSnapshot(light, lightProperties),
		"light type changes light canonical bytes");
	lightProperties = *commands.lightProperties(light);
	lightProperties.color[0] = 0.25;
	passed &= distinctLight(commands.prepareLightSnapshot(light, lightProperties),
		"light color changes light canonical bytes");
	lightProperties = *commands.lightProperties(light);
	lightProperties.intensity += 0.5;
	passed &= distinctLight(commands.prepareLightSnapshot(light, lightProperties),
		"light intensity changes light canonical bytes");
	lightProperties = *commands.lightProperties(light);
	lightProperties.range = -0.0;
	const auto negativeZeroLight = commands.prepareLightSnapshot(light, lightProperties);
	passed &= distinctLight(negativeZeroLight,
		"negative zero changes exact light numeric representation bytes");
	if(negativeZeroLight)
	{
		const std::vector<std::uint8_t> negativeZero{
			0u, 0u, 0u, 0u, 0u, 0u, 0u, 0x80u};
		passed &= expect(containsCanonicalBytes(
			negativeZeroLight->inspectCanonicalRecord().canonicalBytes, negativeZero),
			"negative zero is encoded by its exact IEEE-754 bit pattern");
	}
	lightProperties = *commands.lightProperties(light);
	lightProperties.type = "spot";
	lightProperties.innerConeAngle = 0.125;
	lightProperties.outerConeAngle = 0.75;
	passed &= distinctLight(commands.prepareLightSnapshot(light, lightProperties),
		"spot cone state changes light canonical bytes");

	const ExactDocumentSnapshot beforeInvalid = exactSnapshot(*document);
	const MutationResult invalidSnapshotRejected =
		commands.setMaterialSnapshot(material, MaterialSnapshot{});
	passed &= expect(invalidSnapshotRejected.rejected()
			&& invalidSnapshotRejected.error == MutationErrorCode::invalidSnapshot
			&& exactSnapshot(*document) == beforeInvalid,
		"invalid snapshot publication rejects with exact no-write");
	properties = *commands.materialProperties(material);
	properties.metallicFactor = std::numeric_limits<double>::quiet_NaN();
	passed &= expect(!commands.prepareMaterialSnapshot(material, MaterialEdit{properties})
			&& commands.setMaterial(material, properties).rejected()
			&& exactSnapshot(*document) == beforeInvalid,
		"non-finite material values reject with exact no-write");
	lightProperties = *commands.lightProperties(light);
	lightProperties.intensity = std::numeric_limits<double>::infinity();
	passed &= expect(!commands.prepareLightSnapshot(light, lightProperties)
			&& commands.setLight(light, lightProperties).rejected()
			&& exactSnapshot(*document) == beforeInvalid,
		"non-finite light values reject with exact no-write");

	if(!changedMaterial) return expect(false, "changed material snapshot prepares");
	OneShotAllocationFault allocationFault;
	SceneCommands failingCommands(*document, kDefaultEditorLimits, &allocationFault);
	const ExactDocumentSnapshot beforeAllocation = exactSnapshot(*document);
	const MutationResult allocationRejected =
		failingCommands.setMaterialSnapshot(material, *changedMaterial);
	passed &= expect(allocationRejected.rejected()
			&& allocationRejected.error == MutationErrorCode::allocationFailed
			&& exactSnapshot(*document) == beforeAllocation,
		"snapshot publication allocation failure is an exact no-write");

	EditorLimits oneLessLimits = kDefaultEditorLimits;
	oneLessLimits.maxHistoryEntrySnapshotBytes = changedMaterial->snapshotBytes() - 1u;
	SceneCommands oneLessCommands(*document, oneLessLimits);
	const ExactDocumentSnapshot beforeBudget = exactSnapshot(*document);
	const MutationResult budgetRejected =
		oneLessCommands.setMaterialSnapshot(material, *changedMaterial);
	passed &= expect(budgetRejected.rejected()
			&& budgetRejected.error == MutationErrorCode::snapshotBudgetExceeded
			&& exactSnapshot(*document) == beforeBudget,
		"one byte below the charged snapshot boundary rejects with exact no-write");
	EditorLimits exactLimits = kDefaultEditorLimits;
	exactLimits.maxHistoryEntrySnapshotBytes = changedMaterial->snapshotBytes();
	SceneCommands exactCommands(*document, exactLimits);
	const MutationResult exactApplied = exactCommands.setMaterialSnapshot(material, *changedMaterial);
	passed &= expect(exactApplied.applied(),
		"the exact charged snapshot byte boundary is admitted");
	const MutationToken staleToken = exactCommands.token();
	const MutationResult restored = commands.setMaterialSnapshot(material, *materialBefore);
	passed &= expect(restored.applied()
			&& commands.materialSnapshot(material) == materialBefore
			&& commands.validateMaterialSnapshot(material, *materialBefore, commands.token()).matches()
			&& !commands.validateMaterialSnapshot(material, *materialBefore, staleToken).matches(),
		"complete snapshot restoration and stale-token validation remain unchanged");
	for(unsigned generation = 0u; generation < 2u; ++generation)
	{
		const auto applyAgain = commands.setMaterialSnapshot(material, *changedMaterial);
		const auto restoreAgain = commands.setMaterialSnapshot(material, *materialBefore);
		passed &= expect(applyAgain.applied() && restoreAgain.applied()
				&& commands.materialSnapshot(material) == materialBefore,
			"repeated material snapshot generations restore the complete record");
	}
	lightProperties = *commands.lightProperties(light);
	lightProperties.intensity += 0.75;
	const auto changedLight = commands.prepareLightSnapshot(light, lightProperties);
	if(!changedLight) return expect(false, "changed light snapshot prepares");
	const auto lightApply = commands.setLightSnapshot(light, *changedLight);
	const auto lightRestore = commands.setLightSnapshot(light, *lightBefore);
	passed &= expect(lightApply.applied() && lightRestore.applied()
			&& commands.lightSnapshot(light) == lightBefore
			&& commands.validateLightSnapshot(light, *lightBefore, commands.token()).matches(),
		"complete light snapshot apply, restore, and validation remain unchanged");

	auto foreignDocument = loadTestDocument("feature_preservation.gltf");
	if(!foreignDocument) return expect(false, "foreign snapshot fixture publishes");
	SceneCommands foreignCommands(*foreignDocument);
	const MaterialId foreignMaterial =
		*MaterialId::from(foreignDocument->idAt(EditorObjectKind::material, 0));
	const ExactDocumentSnapshot foreignBefore = exactSnapshot(*foreignDocument);
	const MutationResult foreignRejected =
		foreignCommands.setMaterialSnapshot(foreignMaterial, *materialBefore);
	passed &= expect(foreignRejected.rejected()
			&& foreignRejected.error == MutationErrorCode::crossDocument
			&& exactSnapshot(*foreignDocument) == foreignBefore,
		"foreign snapshot publication rejects with exact no-write");

	auto richDocument = loadInlineTestDocument(kRichMaterialFixture, "rich");
	if(!richDocument) return expect(false, "rich immutable snapshot fixture publishes");
	SceneCommands richCommands(*richDocument);
	const MaterialId richMaterial =
		*MaterialId::from(richDocument->idAt(EditorObjectKind::material, 0));
	const LightId richLight = *LightId::from(richDocument->idAt(EditorObjectKind::light, 0));
	const auto richMaterialSnapshot = richCommands.materialSnapshot(richMaterial);
	const auto richLightSnapshot = richCommands.lightSnapshot(richLight);
	if(!richMaterialSnapshot || !richLightSnapshot)
		return expect(false, "rich material and light snapshots capture");
	const MutationToken richToken = richCommands.token();
	MutationToken staleRichToken = richToken;
	++staleRichToken.serial;
	const MaterialSnapshot invalidMaterialSnapshot;
	const LightSnapshot invalidLightSnapshot;
	MaterialExtensionValue changedUnknown =
		inspectionExtensionValue(std::string{"changed\0value", 13u});
	const auto changedRichMaterial = richCommands.prepareMaterialSnapshot(richMaterial,
		MaterialEdit{MaterialExtensionEdit{"MGIF_snapshot_unknown", std::move(changedUnknown)}});
	LightProperties changedRichLightProperties = *richCommands.lightProperties(richLight);
	changedRichLightProperties.intensity += 0.25;
	const auto changedRichLight =
		richCommands.prepareLightSnapshot(richLight, changedRichLightProperties);
	if(!changedRichMaterial || !changedRichLight)
		return expect(false, "rich validator mismatch snapshots prepare");

	passed &= expectAllocationFreeComparison([&]
		{ return *richMaterialSnapshot == *richMaterialSnapshot; }, true,
		"complete material snapshot equality allocates exactly zero times");
	passed &= expectAllocationFreeComparison([&]
		{ return *richMaterialSnapshot == *changedRichMaterial; }, false,
		"complete material snapshot mismatch allocates exactly zero times");
	passed &=
		expectAllocationFreeComparison([&] { return *richLightSnapshot == *richLightSnapshot; },
			true, "complete light snapshot equality allocates exactly zero times");
	passed &=
		expectAllocationFreeComparison([&] { return *richLightSnapshot == *changedRichLight; },
			false, "intensity participates in allocation-free complete light equality");

	passed &= expectAllocationFreeValidation(
		[&]
		{
			return richCommands.validateMaterialSnapshot(
				richMaterial, *richMaterialSnapshot, richToken);
		},
		PreparedValueStatus::matches, MutationErrorCode::none,
		"rich material validator match allocates exactly zero times");
	passed &= expectAllocationFreeValidation(
		[&]
		{
			return richCommands.validateMaterialSnapshot(
				richMaterial, *changedRichMaterial, richToken);
		},
		PreparedValueStatus::valueMismatch, MutationErrorCode::staleTransaction,
		"rich material validator mismatch allocates exactly zero times");
	passed &= expectAllocationFreeValidation([&]
		{ return richCommands.validateLightSnapshot(richLight, *richLightSnapshot, richToken); },
		PreparedValueStatus::matches, MutationErrorCode::none,
		"rich light validator match allocates exactly zero times");
	passed &= expectAllocationFreeValidation([&]
		{ return richCommands.validateLightSnapshot(richLight, *changedRichLight, richToken); },
		PreparedValueStatus::valueMismatch, MutationErrorCode::staleTransaction,
		"rich light validator mismatch allocates exactly zero times");
	passed &= expectAllocationFreeValidation(
		[&]
		{
			return richCommands.validateMaterialSnapshot(
				richMaterial, *richMaterialSnapshot, staleRichToken);
		},
		PreparedValueStatus::staleTransaction, MutationErrorCode::staleTransaction,
		"material stale-transaction validation allocates exactly zero times");
	passed &= expectAllocationFreeValidation(
		[&]
		{
			return richCommands.validateLightSnapshot(
				richLight, *richLightSnapshot, staleRichToken);
		},
		PreparedValueStatus::staleTransaction, MutationErrorCode::staleTransaction,
		"light stale-transaction validation allocates exactly zero times");
	passed &= expectAllocationFreeValidation(
		[&]
		{
			return richCommands.validateMaterialSnapshot(
				richMaterial, invalidMaterialSnapshot, richToken);
		},
		PreparedValueStatus::invalidSnapshot, MutationErrorCode::invalidSnapshot,
		"invalid material snapshot validation allocates exactly zero times");
	passed &= expectAllocationFreeValidation([&]
		{ return richCommands.validateLightSnapshot(richLight, invalidLightSnapshot, richToken); },
		PreparedValueStatus::invalidSnapshot, MutationErrorCode::invalidSnapshot,
		"invalid light snapshot validation allocates exactly zero times");
	const MutationToken baseToken = commands.token();
	passed &= expectAllocationFreeValidation(
		[&]
		{
			return commands.validateMaterialSnapshot(
				richMaterial, *richMaterialSnapshot, baseToken);
		},
		PreparedValueStatus::foreignAuthority, MutationErrorCode::crossDocument,
		"foreign material snapshot validation allocates exactly zero times");
	passed &= expectAllocationFreeValidation([&]
		{ return commands.validateLightSnapshot(richLight, *richLightSnapshot, baseToken); },
		PreparedValueStatus::foreignAuthority, MutationErrorCode::crossDocument,
		"foreign light snapshot validation allocates exactly zero times");

	const MutationResult changedRichLightApplied =
		richCommands.setLightSnapshot(richLight, *changedRichLight);
	const MutationResult richLightRestored =
		richCommands.setLightSnapshot(richLight, *richLightSnapshot);
	passed &= expect(changedRichLightApplied.applied() && richLightRestored.applied(),
		"intensity-only complete light snapshots apply and restore instead of reporting no-op");
	const auto exerciseLightField = [&](const LightProperties& properties,
										const char* equalityMessage, const char* mutationMessage)
	{
		const auto changed = richCommands.prepareLightSnapshot(richLight, properties);
		if(!changed) return expect(false, mutationMessage);
		bool result = expectAllocationFreeComparison(
			[&] { return *richLightSnapshot == *changed; }, false, equalityMessage);
		const MutationResult applied = richCommands.setLightSnapshot(richLight, *changed);
		const MutationResult restored =
			richCommands.setLightSnapshot(richLight, *richLightSnapshot);
		result &= expect(applied.applied() && restored.applied(), mutationMessage);
		return result;
	};
	LightProperties changedRange = *richCommands.lightProperties(richLight);
	changedRange.range += 0.5;
	passed &= exerciseLightField(changedRange,
		"range participates in allocation-free complete light equality",
		"range-only complete light snapshots apply and restore instead of reporting no-op");
	LightProperties changedInnerCone = *richCommands.lightProperties(richLight);
	changedInnerCone.innerConeAngle += 0.01;
	passed &= exerciseLightField(changedInnerCone,
		"inner cone angle participates in allocation-free complete light equality",
		"inner-cone-only complete light snapshots apply and restore instead of reporting no-op");
	LightProperties changedOuterCone = *richCommands.lightProperties(richLight);
	changedOuterCone.outerConeAngle -= 0.01;
	passed &= exerciseLightField(changedOuterCone,
		"outer cone angle participates in allocation-free complete light equality",
		"outer-cone-only complete light snapshots apply and restore instead of reporting no-op");
	const auto richMaterialInspection = richMaterialSnapshot->inspectCanonicalRecord();
	const auto richLightInspection = richLightSnapshot->inspectCanonicalRecord();
	const auto richMaterialRecord =
		canonicalSnapshotSection(richMaterialInspection, canonicalTag('R', 'E', 'C', 'D'));
	const auto richLightRecord =
		canonicalSnapshotSection(richLightInspection, canonicalTag('R', 'E', 'C', 'D'));
	const auto richReferences =
		canonicalSnapshotSection(richMaterialInspection, canonicalTag('R', 'E', 'F', 'S'));
	std::size_t richReferenceOffset = 0u;
	std::uint64_t richReferenceCount = 0u;
	passed &= expect(richMaterialRecord && richLightRecord && *richMaterialRecord != *baseRecord
			&& *richLightRecord != *baseLightRecord,
		"lods, legacy parameters, nested texture metadata, extensions, extras, and JSON strings "
		"are retained");
	passed &= expect(richReferences
			&& readCanonicalU64(*richReferences, richReferenceOffset, richReferenceCount)
			&& richReferenceCount == 3u,
		"stable references include the LOD and known textures without guessing unknown extension indices");
	passed &= expect(containsCanonicalBytes(richMaterialInspection.canonicalBytes,
				framedCanonicalString(std::string{"M\0x", 3u}))
			&& containsCanonicalBytes(richLightInspection.canonicalBytes,
				framedCanonicalString(std::string{"L\0x", 3u})),
		"material and light record strings preserve embedded zero bytes");

	struct RetainedCategoryVariant final
	{
		const char* label;
		const char* needle;
		const char* replacement;
		EditorObjectKind kind;
	};
	const std::array variants{
		RetainedCategoryVariant{"material name affects canonical bytes", "M\\u0000x", "N\\u0000x", EditorObjectKind::material},
		RetainedCategoryVariant{"material emissive factors affect canonical bytes", "[0.12,0.23,0.34]", "[0.13,0.23,0.34]", EditorObjectKind::material},
		RetainedCategoryVariant{"material alpha mode affects canonical bytes", "\"alphaMode\":\"MASK\"", "\"alphaMode\":\"BLEND\"", EditorObjectKind::material},
		RetainedCategoryVariant{"material alpha cutoff affects canonical bytes", "\"alphaCutoff\":0.45", "\"alphaCutoff\":0.46", EditorObjectKind::material},
		RetainedCategoryVariant{"material sidedness affects canonical bytes", "\"doubleSided\":true", "\"doubleSided\":false", EditorObjectKind::material},
		RetainedCategoryVariant{"material lod stable references affect canonical bytes", "\"ids\":[1]", "\"ids\":[2]", EditorObjectKind::material},
		RetainedCategoryVariant{"pbr factor vectors affect canonical bytes", "[0.11,0.22,0.33,0.44]", "[0.12,0.22,0.33,0.44]", EditorObjectKind::material},
		RetainedCategoryVariant{"pbr scalar factors affect canonical bytes", "\"metallicFactor\":0.55", "\"metallicFactor\":0.56", EditorObjectKind::material},
		RetainedCategoryVariant{"base texture nested metadata affects canonical bytes", "\"baseMarker\":17", "\"baseMarker\":18", EditorObjectKind::material},
		RetainedCategoryVariant{"metallic texture extras affect canonical bytes", "\"metallicExtra\":19", "\"metallicExtra\":20", EditorObjectKind::material},
		RetainedCategoryVariant{"normal texture state affects canonical bytes", "\"scale\":0.77", "\"scale\":0.78", EditorObjectKind::material},
		RetainedCategoryVariant{"occlusion texture state affects canonical bytes", "\"strength\":0.44", "\"strength\":0.43", EditorObjectKind::material},
		RetainedCategoryVariant{"emissive texture extras affect canonical bytes", "\"emissiveExtra\":31", "\"emissiveExtra\":32", EditorObjectKind::material},
		RetainedCategoryVariant{"unknown extension siblings affect canonical bytes", "\"index\":2", "\"index\":0", EditorObjectKind::material},
		RetainedCategoryVariant{"material extras affect canonical bytes", "\"materialExtra\"", "\"materialExtraChanged\"", EditorObjectKind::material},
		RetainedCategoryVariant{"light name affects canonical bytes", "L\\u0000x", "K\\u0000x", EditorObjectKind::light},
		RetainedCategoryVariant{"light color exact bits affect canonical bytes", "[0.21,-0.0,0.43]", "[0.22,-0.0,0.43]", EditorObjectKind::light},
		RetainedCategoryVariant{"light intensity affects canonical bytes", "\"intensity\":2.75", "\"intensity\":2.76", EditorObjectKind::light},
		RetainedCategoryVariant{"light range affects canonical bytes", "\"range\":9.5", "\"range\":9.6", EditorObjectKind::light},
		RetainedCategoryVariant{"spot cone angles affect canonical bytes", "\"innerConeAngle\":0.12", "\"innerConeAngle\":0.13", EditorObjectKind::light},
		RetainedCategoryVariant{"spot extensions affect canonical bytes", "\"spotMarker\":11", "\"spotMarker\":12", EditorObjectKind::light},
		RetainedCategoryVariant{"spot extras affect canonical bytes", "\"spotExtra\"", "\"spotExtraChanged\"", EditorObjectKind::light},
		RetainedCategoryVariant{"light extensions affect canonical bytes", "\"lightMarker\":13", "\"lightMarker\":14", EditorObjectKind::light},
		RetainedCategoryVariant{"light extras affect canonical bytes", "\"lightExtra\"", "\"lightExtraChanged\"", EditorObjectKind::light},
	};
	for(const RetainedCategoryVariant& variant : variants)
	{
		std::string mutated(kRichMaterialFixture);
		const std::size_t found = mutated.find(variant.needle);
		if(found == std::string::npos)
		{
			passed &= expect(false, variant.label);
			continue;
		}
		mutated.replace(found, std::string_view(variant.needle).size(), variant.replacement);
		auto variantDocument = loadInlineTestDocument(mutated, "variant");
		if(!variantDocument)
		{
			passed &= expect(false, variant.label);
			continue;
		}
		SceneCommands variantCommands(*variantDocument);
		std::optional<std::vector<std::uint8_t>> record;
		if(variant.kind == EditorObjectKind::material)
		{
			const MaterialId id =
				*MaterialId::from(variantDocument->idAt(EditorObjectKind::material, 0));
			const auto snapshot = variantCommands.materialSnapshot(id);
			if(snapshot) record = canonicalSnapshotSection(
				snapshot->inspectCanonicalRecord(), canonicalTag('R', 'E', 'C', 'D'));
			passed &= expect(record && richMaterialRecord && *record != *richMaterialRecord,
				variant.label);
		}
		else
		{
			const LightId id = *LightId::from(variantDocument->idAt(EditorObjectKind::light, 0));
			const auto snapshot = variantCommands.lightSnapshot(id);
			if(snapshot)
				record = canonicalSnapshotSection(
					snapshot->inspectCanonicalRecord(), canonicalTag('R', 'E', 'C', 'D'));
			passed &=
				expect(record && richLightRecord && *record != *richLightRecord, variant.label);
		}
	}
	auto staleLightDocument = loadTestDocument("feature_preservation.gltf");
	if(!staleLightDocument) return expect(false, "stale light validator fixture publishes");
	SceneCommands staleLightCommands(*staleLightDocument);
	const SceneId staleLightScene =
		*SceneId::from(staleLightDocument->idAt(EditorObjectKind::scene, 0));
	const LightId staleLightId =
		*LightId::from(staleLightDocument->idAt(EditorObjectKind::light, 0));
	const auto staleLightSnapshot = staleLightCommands.lightSnapshot(staleLightId);
	LightProperties replacementLightProperties;
	replacementLightProperties.type = "point";
	const MutationResult replacementLightCreated = staleLightCommands.createLight(staleLightScene,
		std::nullopt, "replacement-node", "replacement-light", replacementLightProperties);
	const MutationResult deletedStaleLight = staleLightCommands.deleteLight(staleLightId);
	passed &= expect(
		staleLightSnapshot && replacementLightCreated.applied() && deletedStaleLight.applied(),
		"stale light validator fixture retains one light and deletes the captured stable ID");
	if(staleLightSnapshot && replacementLightCreated.applied() && deletedStaleLight.applied())
	{
		const MutationToken staleLightToken = staleLightCommands.token();
		passed &= expectAllocationFreeValidation(
			[&]
			{
				return staleLightCommands.validateLightSnapshot(
					staleLightId, *staleLightSnapshot, staleLightToken);
			},
			PreparedValueStatus::staleId, MutationErrorCode::staleId,
			"stale light ID validation allocates exactly zero times");
	}
	return passed;
}

#if defined(MGIF_EDITOR_TESTING)
bool runSnapshotStaleIdAllocationTest()
{
	auto document = loadTestDocument("feature_preservation.gltf");
	if(!document) return expect(false, "stale snapshot-ID fixture publishes");
	SceneCommands commands(*document);
	const MaterialId material = *MaterialId::from(document->idAt(EditorObjectKind::material, 0));
	const LightId light = *LightId::from(document->idAt(EditorObjectKind::light, 0));
	const TextureId texture = *TextureId::from(document->idAt(EditorObjectKind::texture, 0));
	const auto materialSnapshot = commands.materialSnapshot(material);
	const auto lightSnapshot = commands.lightSnapshot(light);
	if(!materialSnapshot || !lightSnapshot)
		return expect(false, "stale snapshot-ID records capture");
	const auto textureSeed = EditorStateTestAccess::seedCounterMaximum(*document,
		ExhaustionSeedRequest{
			ExhaustionCounterKind::objectGeneration, texture.untyped(), std::nullopt});
	if(!textureSeed.seeded()) return expect(false, "texture ID generation seed succeeds");
	const MutationToken textureToken = commands.token();
	bool passed = expectAllocationFreeValidation([&]
		{ return commands.validateMaterialSnapshot(material, *materialSnapshot, textureToken); },
		PreparedValueStatus::valueMismatch, MutationErrorCode::staleTransaction,
		"stale texture stable reference validation allocates exactly zero times");
	const auto materialSeed = EditorStateTestAccess::seedCounterMaximum(*document,
		ExhaustionSeedRequest{
			ExhaustionCounterKind::objectGeneration, material.untyped(), std::nullopt});
	if(!materialSeed.seeded()) return expect(false, "material ID generation seed succeeds");
	const MutationToken materialToken = commands.token();
	passed &= expectAllocationFreeValidation([&]
		{ return commands.validateMaterialSnapshot(material, *materialSnapshot, materialToken); },
		PreparedValueStatus::staleId, MutationErrorCode::staleId,
		"stale material ID validation allocates exactly zero times");
	const auto lightSeed = EditorStateTestAccess::seedCounterMaximum(*document,
		ExhaustionSeedRequest{
			ExhaustionCounterKind::objectGeneration, light.untyped(), std::nullopt});
	if(!lightSeed.seeded()) return expect(false, "light ID generation seed succeeds");
	const MutationToken lightToken = commands.token();
	passed &= expectAllocationFreeValidation([&]
		{ return commands.validateLightSnapshot(light, *lightSnapshot, lightToken); },
		PreparedValueStatus::staleId, MutationErrorCode::staleId,
		"stale light ID validation allocates exactly zero times");
	return passed;
}
#endif
} // namespace

int main(int argc, char** argv)
{
	using namespace demo::editor;
	if(argc == 2 && std::string_view(argv[1]) == "material-duplicate")
		return runMaterialDuplicateContractTest() ? EXIT_SUCCESS : EXIT_FAILURE;
	if(argc == 2 && std::string_view(argv[1]) == "snapshot-validation")
	{
		bool focused = runSnapshotCanonicalInspectionTest();
#if defined(MGIF_EDITOR_TESTING)
		focused &= runSnapshotStaleIdAllocationTest();
#endif
		return focused ? EXIT_SUCCESS : EXIT_FAILURE;
	}
	bool passed = true;
	passed &= runNodeNameSpecializedFaultTest();
	passed &= runMaterialPropertiesSpecializedFaultTest();
	passed &= runAtomicCameraFromViewTest();
	passed &= runMaterialDuplicateContractTest();
	passed &= runTotalHistoryPolicyReleaseProof();
	passed &= runTotalHistoryLifecyclePolicyReleaseProof();
	passed &= runSnapshotCanonicalInspectionTest();
#if defined(MGIF_EDITOR_TESTING)
	passed &= runSnapshotStaleIdAllocationTest();
#endif
	auto loaded = GltfDocumentIO::loadCandidate(fixture("multi_scene_oracle.gltf"));
	passed &=
		expect(loaded.succeeded() && loaded.candidate() != nullptr, "multi-scene fixture loads");
	if(!loaded.succeeded() || loaded.candidate() == nullptr) return EXIT_FAILURE;
	std::unique_ptr<GltfDocument> document;
	passed &= expect(std::move(loaded).publishTo(document), "candidate publishes");
	EditorLimits randomizedLimits = kDefaultEditorLimits;
	randomizedLimits.maxNodes = 64u;
	SceneCommands commands(*document, randomizedLimits);
	const std::uint64_t revision = document->revision();
	const SceneId scene = *SceneId::from(document->idAt(EditorObjectKind::scene, 0));
	const NodeId a0 = *NodeId::from(document->idAt(EditorObjectKind::node, 0));
	const NodeId a1 = *NodeId::from(document->idAt(EditorObjectKind::node, 1));
	const auto sameParent = commands.reparentNode(a0, std::nullopt);
	passed &= expect(sameParent.domainNoop() && document->revision() == revision,
		"root-to-root no-op leaves document revision unchanged");
	const auto add = commands.addNode(scene, "Added", a1);
	passed &= expect(add.applied() && document->revision() == revision + 1u,
		"child append mutation advances revision");
	passed &=
		expect(document->nodeChildIds(a1).back().valid(), "new child is appended at target tail");
	const NodeId added = document->nodeChildIds(a1).back();
	const auto stale =
		commands.rename(added.untyped(), "Renamed", MutationGuard{revision, std::nullopt});
	passed &= expect(stale.rejected() && stale.error == MutationErrorCode::staleRevision,
		"stale expected revision fails closed");
	NodeTransform transform;
	transform.translation = {1.0, 2.0, 3.0};
	const auto transformResult = commands.setNodeTransform(added, transform);
	passed &= expect(transformResult.applied(), "finite TRS mutation applies");
	transform.rotation = {0.0, 0.0, 0.0, 2.0};
	const auto invalidTransform = commands.setNodeTransform(added, transform);
	passed &= expect(invalidTransform.rejected()
			&& invalidTransform.error == MutationErrorCode::invalidTransform,
		"non-normalized quaternion is rejected");
	auto featureLoad = GltfDocumentIO::loadCandidate(fixture("feature_preservation.gltf"));
	std::unique_ptr<GltfDocument> feature;
	passed &= expect(featureLoad.succeeded() && std::move(featureLoad).publishTo(feature),
		"feature fixture publishes");
	SceneCommands featureCommands(*feature);
	const auto primitiveName =
		featureCommands.rename(feature->idAt(EditorObjectKind::primitive, 0), "no-name");
	passed &= expect(
		primitiveName.rejected() && primitiveName.error == MutationErrorCode::primitiveHasNoName,
		"primitive rename is typed rejection");
	const auto cross =
		commands.rename(EditorObjectId{EditorObjectKind::node, 0, 1, {999999u}}, "cross");
	passed &= expect(cross.rejected() && cross.error == MutationErrorCode::crossDocument,
		"cross-document authority is rejected");
	const auto deleted = commands.deleteNode(added);
	passed &= expect(
		deleted.applied() && !document->contains(added.untyped()), "deleted handle becomes stale");
	passed &= runReplayOracle();
	const auto staleId = commands.query(added.untyped());
	passed &= expect(staleId.rejected() && staleId.error == MutationErrorCode::staleId,
		"deleted node ID is rejected after remap");
	const auto deleteLastScene =
		commands.deleteScene(*SceneId::from(document->idAt(EditorObjectKind::scene, 0)));
	passed &= expect(deleteLastScene.applied(), "one scene can be removed while another remains");
	const auto lastScene =
		commands.deleteScene(*SceneId::from(document->idAt(EditorObjectKind::scene, 0)));
	passed &= expect(lastScene.rejected() && lastScene.error == MutationErrorCode::lastScene,
		"deleting the last scene is rejected");
	auto minimalLoad = GltfDocumentIO::loadCandidate(fixture("minimal_scene.gltf"));
	std::unique_ptr<GltfDocument> minimal;
	passed &= expect(minimalLoad.succeeded() && std::move(minimalLoad).publishTo(minimal),
		"minimal fixture publishes");
	if(minimal)
	{
		SceneCommands minimalCommands(*minimal);
		const NodeId root = *NodeId::from(minimal->idAt(EditorObjectKind::node, 0));
		const auto uniqueDelete = minimalCommands.deleteNode(root);
		passed &= expect(uniqueDelete.domainNoop()
				&& uniqueDelete.error == MutationErrorCode::uniqueRootDeleteNoop
				&& minimal->revision() == 0u,
			"unique root delete is a document no-op");
	}
	passed &= runCrossParentAppendTailTest();
	passed &= runCrossSceneRootTargetTest();
	passed &= runRootParentAppendTailTest();
	passed &= runStructuralFailureAndInspectionTest();
	passed &= runCanonicalCategoryTest();
	return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
