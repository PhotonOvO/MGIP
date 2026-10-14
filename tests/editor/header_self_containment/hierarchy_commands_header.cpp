#include <mgif/scene_editor/commands/HierarchyCommands.h>

#include <type_traits>
#include <utility>

static_assert(
    std::is_same_v<decltype(demo::editor::prepareRenameNodeCommand(
                       std::declval<demo::editor::EditorCommandContext &>(),
                       demo::editor::NodeId{}, std::string{})),
                   demo::editor::CommandPrepareResult>);
static_assert(
    std::is_same_v<decltype(demo::editor::prepareReparentNodeCommand(
                       std::declval<demo::editor::EditorCommandContext &>(),
                       demo::editor::ReparentNodeRequest{})),
                   demo::editor::CommandPrepareResult>);
