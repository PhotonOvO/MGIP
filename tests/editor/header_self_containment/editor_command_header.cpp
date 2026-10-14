#include <mgif/scene_editor/commands/EditorCommand.h>

static_assert(sizeof(demo::editor::CommandResult) > 0);
static_assert(sizeof(demo::editor::RetainedCommandInspection) > 0);
static_assert(sizeof(demo::editor::ContinuousCommandValue) > 0);
static_assert(demo::editor::CommandStateSha256{}.size() == 32);
