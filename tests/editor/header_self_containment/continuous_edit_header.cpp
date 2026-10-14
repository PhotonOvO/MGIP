#include <mgif/scene_editor/commands/ContinuousEdit.h>

#include <type_traits>

namespace {

static_assert(
    std::is_nothrow_destructible_v<demo::editor::ContinuousEditSession>);
static_assert(std::is_move_constructible_v<demo::editor::ContinuousResult>);

} // namespace
