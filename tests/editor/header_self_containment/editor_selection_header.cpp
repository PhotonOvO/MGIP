#include <mgif/scene_editor/EditorSelection.h>

namespace {

static_assert(sizeof(demo::editor::EditorSelectionSnapshot) > 0);
static_assert(sizeof(demo::editor::EditorSelectionState) > 0);
static_assert(std::variant_size_v<demo::editor::EditorSelectionValue> == 7);
static_assert(
    static_cast<std::uint8_t>(demo::editor::EditorSelectionKind::none) == 0);
static_assert(
    static_cast<std::uint8_t>(demo::editor::EditorSelectionKind::light) == 6);
static_assert(noexcept(demo::editor::EditorSelectionSnapshot::empty()));
static_assert(noexcept(demo::editor::EditorSelectionSnapshot{}.kind()));
static_assert(noexcept(demo::editor::EditorSelectionSnapshot{}.emptyValue()));

} // namespace
