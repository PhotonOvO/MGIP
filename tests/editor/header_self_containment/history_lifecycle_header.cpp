#include <mgif/scene_editor/commands/HistoryLifecycle.h>

static_assert(
    static_cast<std::uint8_t>(demo::editor::HistoryLifecycleEvent::merge) == 0);
static_assert(static_cast<std::uint8_t>(
                  demo::editor::LifecycleCommitState::changed) == 2);
