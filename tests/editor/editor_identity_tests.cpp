#include <mgif/scene_editor/EditorObjectId.h>
#include <mgif/scene_editor/EditorLimits.h>

#include <cstdlib>
#include <iostream>
#include <limits>
#include <type_traits>
#include <unordered_set>

namespace {

bool expect(bool condition, const char* message)
{
	if(!condition)
	{
		std::cerr << "FAILED: " << message << '\n';
	}
	return condition;
}

} // namespace

static_assert(!std::is_convertible_v<demo::editor::NodeId, demo::editor::SceneId>);
static_assert(!std::is_convertible_v<demo::editor::EditorObjectId, demo::editor::NodeId>);
static_assert(std::is_trivially_copyable_v<demo::editor::EditorDocumentAuthority>);
static_assert(std::is_trivially_copyable_v<demo::editor::EditorObjectId>);
static_assert(std::is_trivially_copyable_v<demo::editor::NodeId>);

int main()
{
	using namespace demo::editor;

	bool passed = true;
	constexpr EditorDocumentAuthority authority{11u};
	const EditorObjectId rawNode{EditorObjectKind::node, 7u, 4u, authority};
	const auto node = NodeId::from(rawNode);
	passed &= expect(rawNode.valid(), "a typed nonzero authority and generation ID is valid");
	passed &= expect(node.has_value(), "a node ID accepts only node-kind values");
	passed &= expect(node->slot() == 7u, "typed ID preserves its stable slot");
	passed &= expect(node->generation() == 4u, "typed ID preserves its generation");
	passed &= expect(node->authority() == authority, "typed ID preserves its document authority");
	passed &= expect(node->untyped() == rawNode, "typed and untyped forms round-trip exactly");
	passed &= expect(!SceneId::from(rawNode).has_value(), "cross-kind conversion is rejected");
	passed &= expect(!NodeId::from(EditorObjectId{EditorObjectKind::node, 7u, 0u, authority}).has_value(),
	                 "generation zero cannot form a live typed ID");
	passed &= expect(!NodeId::from(EditorObjectId{EditorObjectKind::node, 7u, 4u, {}}).has_value(),
	                 "authority zero cannot form a live typed ID");
	passed &= expect(!EditorObjectId{EditorObjectKind::invalid, 7u, 4u, authority}.valid(),
	                 "invalid kind cannot form a live ID");
	passed &= expect(EditorObjectId{EditorObjectKind::node, 7u, 5u, authority} != rawNode,
	                 "a reused slot with a new generation never aliases the stale ID");
	const EditorObjectId anotherAuthority{
		EditorObjectKind::node,
		7u,
		4u,
		EditorDocumentAuthority{12u},
	};
	passed &= expect(anotherAuthority != rawNode,
	                 "matching kind, slot, and generation from another authority never compare equal");
	passed &= expect(EditorObjectIdHash{}(anotherAuthority) != EditorObjectIdHash{}(rawNode),
	                 "document authority contributes to deterministic ID hashing");
	passed &= expect(std::string_view(editorObjectKindName(EditorObjectKind::primitive)) == "primitive",
	                 "object-kind names are stable and deterministic");
	std::unordered_set<NodeId, EditorObjectIdHash> nodes;
	nodes.insert(*node);
	passed &= expect(nodes.contains(*node)
		&& !nodes.contains(NodeId::from(EditorObjectId{EditorObjectKind::node, 7u, 5u, authority}).value())
		&& !nodes.contains(NodeId::from(anotherAuthority).value()),
		"typed IDs provide authority/kind/slot/generation-aware hashing");
	std::uint64_t checked = 0;
	passed &= expect(editorCheckedAdd(4, 5, checked) && checked == 9
		&& !editorCheckedAdd(std::numeric_limits<std::uint64_t>::max(), 1, checked)
		&& editorCheckedMultiply(6, 7, checked) && checked == 42
		&& !editorCheckedMultiply(std::numeric_limits<std::uint64_t>::max(), 2, checked),
		"central editor limit arithmetic detects overflow before allocation");

	return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
