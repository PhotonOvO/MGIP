#include <mgif/scene_editor/GltfDocumentIO.h>
#include <mgif/scene_editor/GltfSemanticCompare.h>
#include <mgif/scene_editor/SceneCommands.h>
#include <mgif/scene_editor/SceneEditor.h>

#include <cstdlib>
#include <array>
#include <filesystem>
#include <iostream>
#include <limits>

namespace {
bool expect(bool condition, const char* message)
{
	if(!condition) std::cerr << "FAILED: " << message << '\n';
	return condition;
}

template<typename Id>
Id stale(Id id)
{
	auto value = id.untyped();
	value.generation = value.generation == std::numeric_limits<std::uint32_t>::max()
		? 1u : value.generation + 1u;
	return *Id::from(value);
}

template<typename Id>
Id foreign(Id id)
{
	auto value = id.untyped();
	value.authority.value = value.authority.value == std::numeric_limits<std::uint64_t>::max()
		? 1u : value.authority.value + 1u;
	return *Id::from(value);
}

std::filesystem::path fixture(const char* name)
{
	return std::filesystem::path(MGIF_TEST_SOURCE_DIR) / "tests" / "fixtures" / "gltf_editor" / name;
}

bool sameRootTransform(
	const demo::editor::SceneRootTransform& left,
	const demo::editor::SceneRootTransform& right)
{
	return left.translation == right.translation
		&& left.rotation == right.rotation
		&& left.scale == right.scale;
}
}

int main()
{
	using namespace demo::editor;
	bool passed = true;
	auto loaded = GltfDocumentIO::loadCandidate(fixture("feature_preservation.gltf"));
	passed &= expect(loaded.succeeded() && loaded.candidate() != nullptr, "feature fixture loads");
	if(!loaded.succeeded() || loaded.candidate() == nullptr) return EXIT_FAILURE;
	std::unique_ptr<GltfDocument> document;
	passed &= expect(std::move(loaded).publishTo(document), "candidate publishes");
	SceneEditor editor(*document);
	const SceneId activeScene = document->currentSceneId();
	const std::uint64_t rootRevision = document->revision();
	const std::uint64_t rootCleanRevision = document->cleanRevision();
	const SceneRootTransform rootIdentity = document->sceneRootTransform(activeScene);
	passed &= expect(rootIdentity.translation == std::array<double, 3>{0.0, 0.0, 0.0},
		"scene-root baseline starts at identity");
	SceneRootTransform previewTransform;
	previewTransform.translation = {3.0, -2.0, 5.0};
	previewTransform.scale = {2.0, 2.0, 2.0};
	const auto preview = editor.commands().previewSceneRootTransform(activeScene, previewTransform);
	passed &= expect(preview.applied() && preview.historyPolicy == HistoryPolicy::nonHistory
		&& document->revision() == rootRevision
		&& document->cleanRevision() == rootCleanRevision && !document->dirty(),
		"scene-root preview is explicit non-history runtime state");
	for(int cycle = 0; cycle < 1000; ++cycle)
	{
		const auto repeated = editor.commands().previewSceneRootTransform(activeScene, previewTransform);
		passed &= expect((repeated.domainNoop() || repeated.applied()) && document->revision() == rootRevision,
			"repeated scene-root preview does not drift or advance revision");
	}
	const auto reset = editor.commands().resetSceneRootTransform(activeScene);
	passed &= expect(reset.applied() && document->sceneRootTransform(activeScene).translation == rootIdentity.translation
		&& document->revision() == rootRevision, "scene-root reset returns to baseline without history");
	const auto activePreview = editor.commands().previewSceneRootTransform(activeScene, previewTransform);
	passed &= expect(activePreview.applied(), "scene-root preview can be re-applied before topology edit");
	const auto addedRoot = editor.commands().addNode(activeScene, "Root After Preview");
	passed &= expect(addedRoot.applied() && document->sceneRootTransformState(activeScene).frozen,
		"topology edit freezes scene-root state");
	const auto rebuiltPreview = editor.commands().previewSceneRootTransform(activeScene, previewTransform);
	passed &= expect(rebuiltPreview.applied() && !document->sceneRootTransformState(activeScene).frozen,
		"next scene-root preview rebuilds frozen baseline");
	const auto rebuiltReset = editor.commands().resetSceneRootTransform(activeScene);
	passed &= expect(rebuiltReset.applied() && !document->sceneRootTransformState(activeScene).frozen,
		"reset after topology edit unfreezes scene-root state");
	const MeshId mesh = *MeshId::from(document->idAt(EditorObjectKind::mesh, 0));
	const NodeId node = *NodeId::from(document->idAt(EditorObjectKind::node, 0));
	const auto attach = editor.commands().attachMesh(node, mesh);
	passed &= expect(attach.domainNoop() || attach.applied(), "mesh attachment is a valid command result");
	const auto visible = editor.commands().setVisibility(node.untyped(), false);
	passed &= expect(visible.applied() && !document->objectVisible(node.untyped()), "visibility is explicit runtime state");
	const auto visibleAgain = editor.commands().setVisibility(node.untyped(), false);
	passed &= expect(visibleAgain.domainNoop() && document->revision() == visible.afterRevision,
		"repeated visibility command is a domain no-op");
	const MaterialId material = *MaterialId::from(document->idAt(EditorObjectKind::material, 0));
	MaterialProperties properties;
	properties.metallicFactor = 0.25;
	properties.doubleSided = true;
	const auto materialResult = editor.commands().setMaterial(material, properties);
	passed &= expect(materialResult.applied(), "material core edit applies");
	const auto materialSnapshot = editor.commands().materialSnapshot(material);
	MutationToken foreignToken = editor.commands().token();
	foreignToken.authority.value = foreignToken.authority.value
		== std::numeric_limits<std::uint64_t>::max()
		? 1u : foreignToken.authority.value + 1u;
	passed &= expect(materialSnapshot
		&& editor.commands().validateMaterialSnapshot(
			material, *materialSnapshot, foreignToken).status
			== PreparedValueStatus::foreignAuthority,
		"material snapshot validation rejects a foreign expected token");
	if(document->objectCount(EditorObjectKind::light) != 0u)
	{
		const LightId snapshotLight = *LightId::from(document->idAt(
			EditorObjectKind::light, 0u));
		const auto lightSnapshot = editor.commands().lightSnapshot(snapshotLight);
		passed &= expect(lightSnapshot
			&& editor.commands().validateLightSnapshot(
				snapshotLight, *lightSnapshot, foreignToken).status
				== PreparedValueStatus::foreignAuthority,
			"light snapshot validation rejects a foreign expected token");
	}
	const PrimitiveId primitive = *PrimitiveId::from(document->idAt(EditorObjectKind::primitive, 0));
	passed &= expect(document->ownsPrimitiveOccurrence(activeScene, node, mesh, primitive),
		"primitive relationship predicate accepts the live owning tuple");
	passed &= expect(!document->ownsPrimitiveOccurrence(stale(activeScene), node, mesh, primitive),
		"primitive relationship predicate rejects a stale scene ID");
	passed &= expect(!document->ownsPrimitiveOccurrence(foreign(activeScene), node, mesh, primitive),
		"primitive relationship predicate rejects a foreign scene ID");
	passed &= expect(!document->ownsPrimitiveOccurrence(activeScene, stale(node), mesh, primitive),
		"primitive relationship predicate rejects a stale node ID");
	passed &= expect(!document->ownsPrimitiveOccurrence(activeScene, foreign(node), mesh, primitive),
		"primitive relationship predicate rejects a foreign node ID");
	passed &= expect(!document->ownsPrimitiveOccurrence(activeScene, node, stale(mesh), primitive),
		"primitive relationship predicate rejects a stale mesh ID");
	passed &= expect(!document->ownsPrimitiveOccurrence(activeScene, node, foreign(mesh), primitive),
		"primitive relationship predicate rejects a foreign mesh ID");
	passed &= expect(!document->ownsPrimitiveOccurrence(activeScene, node, mesh, stale(primitive)),
		"primitive relationship predicate rejects a stale primitive ID");
	passed &= expect(!document->ownsPrimitiveOccurrence(activeScene, node, mesh, foreign(primitive)),
		"primitive relationship predicate rejects a foreign primitive ID");

	auto structuralLoad = GltfDocumentIO::loadCandidate(fixture("minimal_scene.gltf"));
	passed &= expect(structuralLoad.succeeded() && structuralLoad.candidate() != nullptr,
		"structural root-state fixture loads");
	if(structuralLoad.succeeded() && structuralLoad.candidate() != nullptr)
	{
		auto structuralDocument = std::move(structuralLoad).takeCandidate();
		SceneCommands structuralCommands(*structuralDocument);
		const SceneId structuralScene = structuralDocument->currentSceneId();
		SceneRootTransform structuralPreview;
		structuralPreview.translation = {6.0, -1.0, 2.0};
		const auto previewResult = structuralCommands.previewSceneRootTransform(
			structuralScene, structuralPreview);
		const std::uint64_t historySerial = structuralDocument->sceneRootTransformState(
			structuralScene).externalStateSerial;
		const std::size_t nodesBefore = structuralDocument->objectCount(EditorObjectKind::node);
		const std::size_t lightsBefore = structuralDocument->objectCount(EditorObjectKind::light);
		auto prepared = structuralCommands.prepareStructural(AddLightRequest{
			structuralScene, std::nullopt, "Owned Root Light", "Owned Light", {}});
		auto committed = structuralCommands.commitStructural(std::move(prepared.prepared));
		passed &= expect(previewResult.applied() && committed.mutation.applied()
			&& committed.hasDelta()
			&& structuralDocument->sceneRootTransformState(
				structuralScene).externalStateSerial == historySerial,
			"history-owned structural commit retains the active-preview external serial");
		if(committed.hasDelta())
		{
			const NodeId ownedNode = *NodeId::from(structuralDocument->idAt(
				EditorObjectKind::node, nodesBefore));
			const LightId ownedLight = *LightId::from(structuralDocument->idAt(
				EditorObjectKind::light, lightsBefore));
			const auto foreignResult = structuralCommands.createLight(
				structuralScene, std::nullopt, "Foreign Root Light", "Foreign Light", {});
			const NodeId foreignNode = *NodeId::from(structuralDocument->idAt(
				EditorObjectKind::node,
				structuralDocument->objectCount(EditorObjectKind::node) - 1u));
			const LightId foreignLight = *LightId::from(structuralDocument->idAt(
				EditorObjectKind::light,
				structuralDocument->objectCount(EditorObjectKind::light) - 1u));
			const auto externalState = structuralDocument->sceneRootTransformState(
				structuralScene);
			passed &= expect(foreignResult.applied()
				&& externalState.externalStateSerial == historySerial + 1u,
				"later root light advances only the affected scene external serial");
			const auto undo = structuralCommands.restoreStructural(
				*committed.delta, StructuralState::undo);
			const auto stateAfterUndo = structuralDocument->sceneRootTransformState(
				structuralScene);
			passed &= expect(undo.applied()
				&& !structuralDocument->contains(ownedNode.untyped())
				&& !structuralDocument->contains(ownedLight.untyped())
				&& structuralDocument->contains(foreignNode.untyped())
				&& structuralDocument->contains(foreignLight.untyped())
				&& sameRootTransform(stateAfterUndo.baseline, externalState.baseline)
				&& sameRootTransform(stateAfterUndo.current, externalState.current)
				&& stateAfterUndo.frozen == externalState.frozen
				&& stateAfterUndo.externalStateSerial == externalState.externalStateSerial,
				"structural Undo preserves later root light and absolute root runtime state");
			const auto redo = structuralCommands.restoreStructural(
				*committed.delta, StructuralState::redo);
			const auto stateAfterRedo = structuralDocument->sceneRootTransformState(
				structuralScene);
			passed &= expect(redo.applied()
				&& structuralDocument->contains(ownedNode.untyped())
				&& structuralDocument->contains(ownedLight.untyped())
				&& structuralDocument->contains(foreignNode.untyped())
				&& structuralDocument->contains(foreignLight.untyped())
				&& sameRootTransform(stateAfterRedo.baseline, stateAfterUndo.baseline)
				&& sameRootTransform(stateAfterRedo.current, stateAfterUndo.current)
				&& stateAfterRedo.frozen == stateAfterUndo.frozen
				&& stateAfterRedo.externalStateSerial == stateAfterUndo.externalStateSerial,
				"structural Redo resurrects exact owned IDs without replacing later state");
			const auto finalUndo = structuralCommands.restoreStructural(
				*committed.delta, StructuralState::undo);
			passed &= expect(finalUndo.applied()
				&& structuralDocument->contains(foreignLight.untyped())
				&& structuralDocument->objectCount(EditorObjectKind::light) == lightsBefore + 1u,
				"extension-name recomposition retains the later foreign light on repeated Undo");
		}
	}
	auto multiSceneLoad = GltfDocumentIO::loadCandidate(fixture("multi_scene_oracle.gltf"));
	passed &= expect(multiSceneLoad.succeeded() && multiSceneLoad.candidate() != nullptr,
		"per-scene external-serial fixture loads");
	if(multiSceneLoad.succeeded() && multiSceneLoad.candidate() != nullptr)
	{
		auto multiSceneDocument = std::move(multiSceneLoad).takeCandidate();
		SceneCommands multiSceneCommands(*multiSceneDocument);
		const SceneId changedScene = *SceneId::from(multiSceneDocument->idAt(
			EditorObjectKind::scene, 0u));
		const SceneId unchangedScene = *SceneId::from(multiSceneDocument->idAt(
			EditorObjectKind::scene, 1u));
		const NodeId changedParent = multiSceneDocument->sceneRootNodeIds(
			changedScene).front();
		const auto changedBefore = multiSceneDocument->sceneRootTransformState(changedScene);
		const auto unchangedBefore = multiSceneDocument->sceneRootTransformState(unchangedScene);
		const auto childAdd = multiSceneCommands.addNode(
			changedScene, "Scene-local Child", changedParent);
		const auto changedAfter = multiSceneDocument->sceneRootTransformState(changedScene);
		const auto unchangedAfter = multiSceneDocument->sceneRootTransformState(unchangedScene);
		passed &= expect(childAdd.applied()
			&& changedAfter.externalStateSerial == changedBefore.externalStateSerial + 1u
			&& unchangedAfter.externalStateSerial == unchangedBefore.externalStateSerial,
			"hierarchy publication advances only affected per-scene external serials");
	}
	auto foreignSceneLoad = GltfDocumentIO::loadCandidate(fixture("minimal_scene.gltf"));
	passed &= expect(foreignSceneLoad.succeeded() && foreignSceneLoad.candidate() != nullptr,
		"later-scene composition fixture loads");
	if(foreignSceneLoad.succeeded() && foreignSceneLoad.candidate() != nullptr)
	{
		auto foreignSceneDocument = std::move(foreignSceneLoad).takeCandidate();
		SceneCommands foreignSceneCommands(*foreignSceneDocument);
		const SceneId capturedScene = foreignSceneDocument->currentSceneId();
		const NodeId capturedRoot = foreignSceneDocument->sceneRootNodeIds(
			capturedScene).front();
		auto prepared = foreignSceneCommands.prepareStructural(AddChildNodeRequest{
			capturedScene, capturedRoot, "Captured Child"});
		auto committed = foreignSceneCommands.commitStructural(std::move(prepared.prepared));
		passed &= expect(committed.mutation.applied() && committed.hasDelta(),
			"later-scene test captures a history-owned structural delta");
		if(committed.hasDelta())
		{
			const auto addScene = foreignSceneCommands.addScene("Later Scene");
			const SceneId laterScene = *SceneId::from(foreignSceneDocument->idAt(
				EditorObjectKind::scene,
				foreignSceneDocument->objectCount(EditorObjectKind::scene) - 1u));
			const auto addRoot = foreignSceneCommands.addNode(laterScene, "Later Root");
			const NodeId laterRoot = foreignSceneDocument->sceneRootNodeIds(laterScene).front();
			SceneRootTransform laterTransform;
			laterTransform.translation = {-4.0, 3.0, 1.0};
			const auto laterPreview = foreignSceneCommands.previewSceneRootTransform(
				laterScene, laterTransform);
			const auto laterState = foreignSceneDocument->sceneRootTransformState(laterScene);
			passed &= expect(addScene.applied() && addRoot.applied() && laterPreview.applied()
				&& foreignSceneDocument->selectCurrentScene(laterScene),
				"later scene establishes independent root state after delta capture");
			const auto undo = foreignSceneCommands.restoreStructural(
				*committed.delta, StructuralState::undo);
			const auto afterUndo = foreignSceneDocument->sceneRootTransformState(laterScene);
			const auto redo = foreignSceneCommands.restoreStructural(
				*committed.delta, StructuralState::redo);
			const auto afterRedo = foreignSceneDocument->sceneRootTransformState(laterScene);
			passed &= expect(undo.applied() && redo.applied()
				&& foreignSceneDocument->contains(laterScene.untyped())
				&& foreignSceneDocument->hasNodeOccurrence(
					laterScene, laterRoot, std::nullopt)
				&& foreignSceneDocument->currentSceneId() == laterScene
				&& sameRootTransform(afterUndo.baseline, laterState.baseline)
				&& sameRootTransform(afterUndo.current, laterState.current)
				&& afterUndo.externalStateSerial == laterState.externalStateSerial
				&& sameRootTransform(afterRedo.baseline, afterUndo.baseline)
				&& sameRootTransform(afterRedo.current, afterUndo.current)
				&& afterRedo.externalStateSerial == afterUndo.externalStateSerial,
				"Undo/Redo preserves a later scene, current-scene choice, and root state");
		}
	}
	const auto assignment = editor.commands().assignPrimitiveMaterial(primitive, material);
	passed &= expect(assignment.domainNoop() || assignment.applied(), "primitive material assignment has typed result");
	const auto savePath = std::filesystem::temp_directory_path() / "mgif_p3_mutation_roundtrip.gltf";
	const auto save = GltfDocumentIO::saveAs(*document, savePath);
	passed &= expect(save.succeeded(), "save after mutation succeeds");
	if(save.succeeded())
	{
		const auto reload = GltfDocumentIO::loadCandidate(savePath);
		passed &= expect(reload.succeeded() && reload.candidate() != nullptr,
			"saved mutation reloads through document IO");
		if(reload.succeeded() && reload.candidate() != nullptr)
			passed &= expect(GltfSemanticCompare::digest(*document) == GltfSemanticCompare::digest(*reload.candidate()),
				"save-after-mutation semantic digest round-trips");
	}
	std::error_code error;
	std::filesystem::remove(savePath, error);
	return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
