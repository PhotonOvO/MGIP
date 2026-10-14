#include <mgif/scene_editor/EditorHistoryPolicy.h>

#include <type_traits>

static_assert(std::is_enum_v<demo::editor::HistoryPolicy>);
static_assert(static_cast<unsigned>(demo::editor::HistoryPolicy::unclassified) == 0u);
