#include <mgif/scene_editor/commands/EditorHistory.h>

static_assert(demo::editor::EditorHistory::kCapacity == 100);
static_assert(demo::editor::EditorHistory::kMergeWindow ==
              std::chrono::milliseconds{500});
static_assert(sizeof(demo::editor::HistoryResult) > 0);
