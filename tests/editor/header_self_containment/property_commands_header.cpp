#include <mgif/scene_editor/commands/PropertyCommands.h>

#include <type_traits>

namespace {

static_assert(std::is_move_constructible_v<demo::editor::CommandPrepareResult>);
static_assert(
    std::is_move_constructible_v<demo::editor::ContinuousCommandPrepareResult>);

} // namespace
