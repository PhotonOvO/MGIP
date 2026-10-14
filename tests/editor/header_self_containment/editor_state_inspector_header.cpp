#include <mgif/scene_editor/testing/EditorStateInspector.h>

static_assert(sizeof(demo::editor::DocumentInspection) > 0);
static_assert(sizeof(demo::editor::ReferenceRewriteProbeResult) > 0);
#if defined(MGIF_EDITOR_TESTING)
static_assert(sizeof(demo::editor::ExhaustionSeedResult) > 0);
#endif
