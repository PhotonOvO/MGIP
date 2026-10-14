#include <mgif/scene_editor/SceneChangeCanonicalizer.h>

#include <cstdint>
#include <type_traits>

static_assert(demo::editor::kSceneChangeSchemaVersion == 1u);
static_assert(
    std::is_same_v<std::underlying_type_t<demo::editor::SceneChangeDomain>,
                   std::uint8_t>);
static_assert(
    std::is_same_v<std::underlying_type_t<demo::editor::SceneChangeClass>,
                   std::uint8_t>);
static_assert(
    std::is_same_v<std::underlying_type_t<demo::editor::SceneChangeAction>,
                   std::uint8_t>);

namespace {

[[maybe_unused]] demo::editor::SceneChangeSet makeSelfContainedValue() {
  demo::editor::SceneChangeSet value;
  value.header.schemaVersion = demo::editor::kSceneChangeSchemaVersion;
  return value;
}

} // namespace
