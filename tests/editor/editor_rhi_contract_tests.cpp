#include <mgif/scene_editor/EditorClock.h>
#include <mgif/scene_editor/EditorFaultInjection.h>
#include <mgif/scene_editor/SceneChangeCanonicalizer.h>

#if !defined(MGIF_DEPENDENCY_FREE_RHI_CONTRACT)
#include "render/RendererFacade.h"
#include "render/RenderTypes.h"
#include "render/editor/EditorOverlayShaderInterop.h"
#include "render/passes/EditorOverlayPass.h"
#endif
#include "render/editor/SceneRuntimePublisher.h"
#include "rhi/RHIBackend.h"
#include "rhi/RHIDebugCounters.h"
#include "rhi/RHITypes.h"
#if !defined(MGIF_DEPENDENCY_FREE_RHI_CONTRACT)
#include "scene/SceneUploadPlan.h"
#endif

#include <array>
#include <concepts>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

#if !defined(MGIF_TEST_SOURCE_DIR)
#define MGIF_TEST_SOURCE_DIR "."
#endif

using namespace demo;
using namespace demo::editor;

template<typename T>
concept MutableMappingEntries = requires(T& value) {
	{ value.entries() } -> std::same_as<std::span<RendererSceneMappingEntry>>;
};

template<typename T>
concept NativeBackendEscape = requires(T value) {
	value.vkHandle;
	value.d3d12Handle;
	value.metalHandle;
};

using ApplyMember = SceneApplyResult
	(RendererScenePublisher::*)(const ApplySceneChangeRequest&) noexcept;
using StateMember = std::shared_ptr<const RendererSceneGeneration>
	(RendererScenePublisher::*)() const noexcept;
using CountersMember = SceneDeltaCounterSnapshot
	(RendererScenePublisher::*)() const noexcept;

#if !defined(MGIF_DEPENDENCY_FREE_RHI_CONTRACT)
using InitializeFacadeMember = RendererSceneTransactionInitializationResult
	(RendererFacade::*)(RendererSceneTransactionInitialState) noexcept;
using ApplyFacadeMember = SceneApplyResult
	(RendererFacade::*)(const ApplySceneChangeRequest&) noexcept;
using ApplySnapshotFacadeMember = SceneApplyResult
	(RendererFacade::*)(const ApplyFullSceneSnapshotRequest&) noexcept;
using StateFacadeMember = RendererSceneStateSnapshot
	(RendererFacade::*)() const noexcept;
using CountersFacadeMember = SceneDeltaCounterSnapshot
	(RendererFacade::*)() const noexcept;
using PickFenceFacadeMember = std::optional<EditorPickFence>
	(RendererFacade::*)() const noexcept;
using EnqueuePickFacadeMember = PickSubmitResult
	(RendererFacade::*)(const EditorPickRequest&);
using PollPickFacadeMember = void
	(RendererFacade::*)(std::vector<EditorPickResult>&);
#endif

static_assert(std::is_abstract_v<EditorClock>);
static_assert(std::is_final_v<SystemEditorClock>);
static_assert(std::is_abstract_v<EditorFaultInjector>);
static_assert(std::is_final_v<NoEditorFaultInjector>);
static_assert(std::is_enum_v<rhi::BackendType>);
static_assert(std::is_abstract_v<SceneTransactionBackend>);
static_assert(std::constructible_from<RendererScenePublisher,
	                                  RendererSceneInitialState,
	                                  SceneTransactionBackend&,
	                                  SceneDeltaCounters&>);
static_assert(std::is_final_v<RendererTargetFence>);
static_assert(std::is_final_v<RendererSceneMappingSnapshot>);
static_assert(std::is_final_v<SceneApplyResult>);
static_assert(std::is_final_v<ApplySceneChangeRequest>);
static_assert(std::is_final_v<SceneDeltaCounterSnapshot>);
static_assert(!MutableMappingEntries<RendererSceneMappingSnapshot>);
static_assert(!NativeBackendEscape<RendererTargetFence>);
static_assert(!NativeBackendEscape<RendererObjectHandle>);
static_assert(!NativeBackendEscape<SceneApplyResult>);

static_assert(static_cast<std::uint8_t>(SceneApplyDisposition::AcceptedIncremental) == 0u);
static_assert(static_cast<std::uint8_t>(SceneApplyDisposition::AcceptedFullSnapshot) == 1u);
static_assert(static_cast<std::uint8_t>(SceneApplyDisposition::AlreadyApplied) == 2u);
static_assert(static_cast<std::uint8_t>(SceneApplyDisposition::Rejected) == 3u);
static_assert(static_cast<std::uint8_t>(SceneApplyDisposition::FailedBeforeSubmit) == 4u);
static_assert(static_cast<std::uint8_t>(SceneApplyDisposition::FailedAfterSubmit) == 5u);
static_assert(kSceneApplyErrorCount == 23u);
static_assert(kSceneChangeClassCount == 9u);
static_assert(kFullRebuildReasonCount == 12u);
static_assert(kSceneDeviceIdleReasonCount == 4u);

static_assert(std::same_as<decltype(&RendererScenePublisher::applySceneChanges),
	                       ApplyMember>);
static_assert(std::same_as<decltype(&RendererScenePublisher::applyFullSceneSnapshot),
	                       ApplyMember>);
static_assert(std::same_as<decltype(&RendererScenePublisher::activeGeneration),
	                       StateMember>);
static_assert(std::same_as<decltype(&RendererScenePublisher::counters), CountersMember>);

#if !defined(MGIF_DEPENDENCY_FREE_RHI_CONTRACT)
static_assert(std::same_as<decltype(&RendererFacade::initializeSceneTransactions),
	                       InitializeFacadeMember>);
static_assert(std::same_as<decltype(&RendererFacade::applySceneChanges),
	                       ApplyFacadeMember>);
static_assert(std::same_as<decltype(&RendererFacade::applyFullSceneSnapshot),
	                       ApplySnapshotFacadeMember>);
static_assert(std::same_as<decltype(&RendererFacade::activeSceneState),
	                       StateFacadeMember>);
static_assert(std::same_as<decltype(&RendererFacade::sceneDeltaCounters),
	                       CountersFacadeMember>);
static_assert(std::same_as<decltype(&RendererFacade::editorPickFence),
	                       PickFenceFacadeMember>);
static_assert(std::same_as<decltype(&RendererFacade::enqueueEditorPick),
	                       EnqueuePickFacadeMember>);
static_assert(std::same_as<decltype(&RendererFacade::pollEditorPickResults),
	                       PollPickFacadeMember>);

using OverlayPrepareMember = EditorOverlayPrepareResult
    (EditorOverlayPass::*)(std::uint32_t,
                           const EditorOverlayPassFrame &) noexcept;
using OverlayResolveMember = rhi::RHIResult
    (EditorOverlayPass::*)(EditorOverlayFrameTicket,
                           EditorOverlayFrameDisposition,
                           rhi::SubmissionToken) noexcept;
static_assert(std::is_final_v<EditorOverlayPass>);
static_assert(!std::is_copy_constructible_v<EditorOverlayPass>);
static_assert(std::same_as<decltype(&EditorOverlayPass::prepareFrame),
                           OverlayPrepareMember>);
static_assert(std::same_as<decltype(&EditorOverlayPass::resolveFrame),
                           OverlayResolveMember>);
static_assert(std::same_as<decltype(RenderParams{}.editorOverlayFrame),
                           const EditorOverlayFrameSnapshot *>);
static_assert(!NativeBackendEscape<EditorOverlayFrameSnapshot>);
static_assert(sizeof(EditorOverlayFrameUniforms) == 160u);
static_assert(sizeof(EditorOverlayLineInstance) == 48u);
static_assert(sizeof(EditorOverlayGizmoVertex) == 24u);
static_assert(sizeof(EditorOverlayDrawConstants) == 96u);
static_assert(sizeof(EditorOverlayDrawConstants) <=
              rhi::kMaxRootBindingSizeBytes);
#endif

constexpr bool counterContract()
{
	rhi::RHIHotPathCounters before{};
	rhi::RHIHotPathCounters after{};
	after.commandBufferBegins = 2u;
	after.encoderBegins = 3u;
	after.queueSubmits = 5u;
	after.submittedCommandBuffers = 7u;
	after.descriptorUpdates = 11u;
	after.descriptorAllocations = 13u;
	after.tableVersionAllocations = 17u;
	after.pipelineCreations = 19u;
	after.argumentLayoutCreations = 23u;
	after.textureViewCreations = 29u;
	after.commandRecordingHeapAllocations = 31u;
	after.commandRecordingNativeObjectCreations = 37u;
	const rhi::RHIHotPathCounters delta = rhi::hotPathCounterDelta(before, after);
	return delta == after && !rhi::hotPathCountersAreZero(delta) &&
	       rhi::hotPathCountersAreZero(rhi::RHIHotPathCounters{});
}

static_assert(counterContract());

#if !defined(MGIF_DEPENDENCY_FREE_RHI_CONTRACT)
[[nodiscard]] bool productionOverlaySourceContract() {
  const std::filesystem::path root(MGIF_TEST_SOURCE_DIR);
  const auto read = [](const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream),
                       std::istreambuf_iterator<char>());
  };
  const std::string minimal = read(root / "app" / "MinimalLatestApp.h");
  const std::string adapter = read(root / "app" / "editor" /
                                   "EditorInputRouter.h");
  const std::string passHeader =
      read(root / "render" / "passes" / "EditorOverlayPass.h");
  const std::string shaderInterop =
      read(root / "render" / "editor" / "EditorOverlayShaderInterop.h");
  if (minimal.empty() || adapter.empty() || passHeader.empty() ||
      shaderInterop.empty()) {
    return false;
  }
  // P6's runtime remains the input/picking authority, but its legacy ImGui
  // drawing method must not be invoked by the production app. P7 snapshots
  // enter RenderParams through the new adapter-owned value field.
  return minimal.find("m_editorViewportRuntime->drawImGuiOverlay") ==
         std::string::npos &&
         minimal.find("editorOverlayFrame") != std::string::npos &&
         minimal.find("m_editorGizmoAdapter->snapshot") !=
             std::string::npos &&
         adapter.find("DesktopGizmoIntegrationAdapter") != std::string::npos &&
         adapter.find("EditorOverlayFrameSnapshot") != std::string::npos &&
         passHeader.find("VkImage") == std::string::npos &&
         passHeader.find("VkBuffer") == std::string::npos &&
         passHeader.find("ID3D12") == std::string::npos &&
         passHeader.find("MTLTexture") == std::string::npos &&
         passHeader.find("waitIdle") == std::string::npos &&
         shaderInterop.find("VkImage") == std::string::npos &&
         shaderInterop.find("VkBuffer") == std::string::npos &&
         shaderInterop.find("ID3D12") == std::string::npos &&
         shaderInterop.find("MTLTexture") == std::string::npos;
}
#endif

} // namespace

int main()
{
	const rhi::TextureDesc tokenTarget{
		.format = rhi::TextureFormat::r32Uint,
		.usage = rhi::TextureUsageFlags::sampled |
		         rhi::TextureUsageFlags::colorAttachment,
		.extent = {64u, 32u, 1u},
	};
	const std::array tokenFormats{rhi::TextureFormat::r32Uint};
	const rhi::GraphicsPipelineDesc tokenPipeline{
		.renderingInfo = {.colorFormats = tokenFormats},
	};
	if (tokenTarget.format != rhi::TextureFormat::r32Uint ||
	    tokenTarget.extent.width != 64u ||
	    tokenPipeline.renderingInfo.colorFormats.size() != 1u ||
	    tokenPipeline.renderingInfo.colorFormats.front() !=
	        rhi::TextureFormat::r32Uint)
	{
		return EXIT_FAILURE;
	}
#if !defined(MGIF_DEPENDENCY_FREE_RHI_CONTRACT)
	if (!productionOverlaySourceContract()) {
		return EXIT_FAILURE;
	}
#endif
	return EXIT_SUCCESS;
}
