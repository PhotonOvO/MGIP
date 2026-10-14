#include <mgif/scene_editor/StructuralDelta.h>

#include <type_traits>
#include <utility>

static_assert(std::is_move_constructible_v<demo::editor::StructuralDelta>);
static_assert(!std::is_copy_constructible_v<demo::editor::StructuralDelta>);
static_assert(std::is_same_v<decltype(std::declval<const demo::editor::StructuralDelta&>()
									 .inspectCanonicalSide(demo::editor::StructuralState::undo)),
	demo::editor::StructuralDeltaCanonicalSide>);
static_assert(noexcept(std::declval<const demo::editor::StructuralDelta&>().inspectCanonicalSide(
	demo::editor::StructuralState::redo)));
static_assert(demo::editor::StructuralDeltaCanonicalSide::kSchemaVersion == 1u);
