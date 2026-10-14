#include "app/editor/AutomationAssetIdentity.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>

#if !defined(MGIF_TEST_SOURCE_DIR)
#error "MGIF_TEST_SOURCE_DIR must identify the repository root"
#endif

namespace {

bool expect(bool condition, const char* message)
{
  if(!condition)
  {
    std::cerr << "FAILED: " << message << '\n';
  }
  return condition;
}

class ScopedCurrentPath
{
public:
  explicit ScopedCurrentPath(const std::filesystem::path& path)
  {
    m_original = std::filesystem::current_path(m_error);
    if(!m_error)
    {
      std::filesystem::current_path(path, m_error);
    }
  }

  ~ScopedCurrentPath()
  {
    if(m_error)
    {
      return;
    }
    std::error_code ignored;
    std::filesystem::current_path(m_original, ignored);
  }

  [[nodiscard]] bool succeeded() const noexcept
  {
    return !m_error;
  }

private:
  std::filesystem::path m_original;
  std::error_code m_error;
};

} // namespace

static_assert(noexcept(demo::editor::automation::sameAssetIdentity({}, {})));
static_assert(noexcept(demo::editor::automation::isSceneReadyModel({})));
static_assert(noexcept(demo::editor::automation::sceneReadyMarkerModel()));

int main()
{
  using namespace demo::editor::automation;

  bool passed = true;
  passed &= expect(sceneReadyMarkerModel()
                       == std::string_view{"resources/GLTF_Sponza/sponza.gltf"},
                   "scene-ready marker keeps the strict relative Sponza identity");

  const std::filesystem::path sourceRoot{MGIF_TEST_SOURCE_DIR};
  ScopedCurrentPath currentPath{sourceRoot};
  passed &= expect(currentPath.succeeded(),
                   "test enters the same source-relative filesystem context as the app");
  if(!currentPath.succeeded())
  {
    return EXIT_FAILURE;
  }

  std::error_code error;
  const std::filesystem::path canonicalSponza =
      std::filesystem::canonical(std::filesystem::path{sceneReadyMarkerModel()}, error);
  passed &= expect(!error && !canonicalSponza.empty(),
                   "repository Sponza resolves to a canonical absolute path");
  const std::string canonicalSponzaText = canonicalSponza.string();
  passed &= expect(isSceneReadyModel(canonicalSponzaText),
                   "relative Sponza request and canonical loaded path share automation identity");

  error.clear();
  const std::filesystem::path canonicalBistro =
      std::filesystem::canonical("resources/GLTF_Bistro/bistro.gltf", error);
  passed &= expect(!error && !canonicalBistro.empty(),
                   "different repository asset resolves for the negative fixture");
  const std::string canonicalBistroText = canonicalBistro.string();
  passed &= expect(!isSceneReadyModel(canonicalBistroText),
                   "a different canonical asset is not the Sponza automation identity");

  const std::string missingRelative =
      "resources/__mgif_missing_scene_ready_fixture__/sponza.gltf";
  const std::string missingAbsolute = (sourceRoot / missingRelative).string();
  passed &= expect(sameAssetIdentity(missingRelative, missingAbsolute),
                   "missing-file equivalence failure uses deterministic normalized fallback");

  const char malformedBytes[] = {'r', 'e', 's', '\0', 'x'};
  const std::string_view malformedPath{malformedBytes, sizeof(malformedBytes)};
  passed &= expect(!sameAssetIdentity(malformedPath, canonicalSponzaText)
                       && !sameAssetIdentity({}, canonicalSponzaText),
                   "invalid filesystem inputs fail closed through the noexcept API");

  if(passed)
  {
    std::cout << "Automation asset identity regression contract passed.\n";
  }
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
