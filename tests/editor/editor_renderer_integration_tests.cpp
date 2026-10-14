#define MGIF_RENDERER_OWNER_TESTING 1

#include "../../render/RendererFacade.h"

#include "../../render/BatchUploadContext.h"
#include "../../rhi/RHICommandBuffer.h"
#include "../../rhi/RHIDebugCounters.h"
#include "../../app/editor/EditorRendererAdapter.h"

#include <mgif/scene_editor/SceneChangeCanonicalizer.h>
#include <mgif/scene_editor/SceneRuntimeMapping.h>

#include "../../scene/SceneUploadPlanner.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace demo;
using namespace demo::editor;
namespace editor_ui = demo::editor::ui;

template <typename Capability>
concept PublicProjectionOwnerIssuer = requires(
	const GltfDocument& document,
	const SceneAsset& asset,
	const SceneRuntimeMapping& mapping,
	std::string* error)
{
	Capability::fromDocument(document, asset, mapping, error);
};

static_assert(!PublicProjectionOwnerIssuer<SceneProjectionOwnerCapability>,
	"production callers must not be able to mint projection-owner capabilities");

void require(bool condition, const char* message)
{
	if (!condition)
	{
		throw std::runtime_error(message);
	}
}

SceneProjectionOwnerCapability ownerCapabilityFor(
	const SceneAsset& asset, const SceneRuntimeMapping& mapping)
{
	std::string error;
	SceneProjectionOwnerCapability capability =
		SceneProjectionOwnerCapability::makeForTesting(asset, mapping, &error);
	require(capability.valid(), error.empty()
		? "failed to create synthetic projection-owner capability"
		: error.c_str());
	return capability;
}

SceneProjectionOwnerCapability ownerCapabilityFor(
	const SceneAsset& asset, const RendererSceneMappingSnapshot& mapping)
{
	require(mapping.projectionAuthority() != nullptr,
	        "synthetic renderer mapping has no projection authority");
	return ownerCapabilityFor(asset, *mapping.projectionAuthority());
}

class TransactionFakeQueue final : public rhi::Queue
{
public:
	[[nodiscard]] rhi::QueueIdentity identity() const noexcept override
	{
		return {91u, 7u};
	}

	[[nodiscard]] rhi::QueueInfo info() const noexcept override
	{
		return {.queueClass = rhi::QueueClass::graphics, .available = true};
	}

	[[nodiscard]] rhi::SubmissionToken submit(const rhi::SubmitBatch& batch) override
	{
		if (batch.commandBuffers.size() != 1u || batch.commandBuffers.front() == nullptr)
		{
			throw std::runtime_error("transaction fake queue requires one command buffer");
		}
		if (failNextSubmit)
		{
			failNextSubmit = false;
			throw std::runtime_error("injected transaction fake queue submit failure");
		}
		m_lastValue += 11u;
		const rhi::SubmissionToken token{identity(), m_lastValue};
		submissions.push_back(token);
		if (onSubmit) onSubmit();
		if (reportInvalidNextToken)
		{
			reportInvalidNextToken = false;
			return {};
		}
		return token;
	}

	[[nodiscard]] bool isComplete(rhi::SubmissionToken token) const override
	{
		completionQueries.push_back(token);
		return token.queue == identity() && token.value != 0u &&
		       token.value <= m_completedValue;
	}

	void wait(rhi::SubmissionToken token) override
	{
		if (token.queue != identity() || token.value > m_lastValue)
		{
			throw std::runtime_error("transaction fake queue wait token is invalid");
		}
		m_completedValue = std::max(m_completedValue, token.value);
	}

	void waitIdle() override
	{
		++waitIdleCount;
		m_completedValue = m_lastValue;
	}

	[[nodiscard]] rhi::SubmissionToken lastSubmittedToken() const noexcept override
	{
		return m_lastValue == 0u ? rhi::SubmissionToken{}
		                         : rhi::SubmissionToken{identity(), m_lastValue};
	}

	void completeThrough(rhi::SubmissionToken token)
	{
		if (token.queue != identity() || token.value > m_lastValue)
		{
			throw std::runtime_error("transaction fake queue completion token is invalid");
		}
		m_completedValue = std::max(m_completedValue, token.value);
	}

	void completeAll() noexcept { m_completedValue = m_lastValue; }

	std::vector<rhi::SubmissionToken> submissions;
	mutable std::vector<rhi::SubmissionToken> completionQueries;
	std::function<void()> onSubmit;
	bool failNextSubmit{false};
	bool reportInvalidNextToken{false};
	uint64_t waitIdleCount{0u};

private:
	uint64_t m_lastValue{1000u};
	uint64_t m_completedValue{0u};
};

class TransactionFakeDevice;

enum class TransactionFakeEventKind : uint8_t
{
	textureBarrier,
	bufferCopy,
	textureCopy,
};

struct TransactionFakeEvent
{
	TransactionFakeEventKind kind{TransactionFakeEventKind::bufferCopy};
	size_t index{0u};
};

struct TransactionFakeDescriptorWrite
{
	rhi::ArgumentTableHandle table{};
	rhi::ArgumentWrite write{};
};

struct TransactionFakeCapture
{
	std::vector<std::pair<rhi::BufferHandle, rhi::BufferDesc>> buffers;
	std::vector<std::pair<rhi::TextureHandle, rhi::TextureDesc>> textures;
	std::vector<std::pair<rhi::TextureViewHandle, rhi::TextureViewCreateDesc>> views;
	std::vector<TransactionFakeDescriptorWrite> descriptorWrites;
	std::vector<rhi::TextureBarrier> textureBarriers;
	std::vector<rhi::BufferTextureCopyDesc> textureCopies;
	std::vector<TransactionFakeEvent> events;
	uint64_t bufferCopyCount{0u};
	bool failNextBufferCopy{false};
};

class TransactionFakeComputeEncoder final : public rhi::ComputeEncoder
{
public:
	explicit TransactionFakeComputeEncoder(TransactionFakeCapture* capture = nullptr)
		: m_capture(capture)
	{
	}

	void setPipeline(rhi::PipelineHandle) override {}
	void setArgumentTable(uint32_t, rhi::ArgumentTableHandle) override {}
	void setRootConstants(uint32_t, std::span<const std::byte>) override {}
	void setRootPointer(uint32_t, rhi::GpuPtr) override {}
	void dispatch(const rhi::DispatchDesc&) override {}
	void dispatchIndirect(const rhi::DispatchIndirectDesc&) override {}
	void copyBuffer(rhi::BufferHandle, uint64_t, rhi::BufferHandle, uint64_t,
	                uint64_t) override
	{
		if (m_capture != nullptr && m_capture->failNextBufferCopy)
		{
			m_capture->failNextBufferCopy = false;
			throw std::runtime_error("injected transaction fake buffer-copy failure");
		}
		++copyCount;
		if (m_capture != nullptr)
		{
			++m_capture->bufferCopyCount;
			m_capture->events.push_back({
				TransactionFakeEventKind::bufferCopy,
				static_cast<size_t>(m_capture->bufferCopyCount - 1u),
			});
		}
	}
	void copyBufferToTexture(const rhi::BufferTextureCopyDesc& copy) override
	{
		++copyCount;
		if (m_capture != nullptr)
		{
			m_capture->textureCopies.push_back(copy);
			m_capture->events.push_back({
				TransactionFakeEventKind::textureCopy,
				m_capture->textureCopies.size() - 1u,
			});
		}
	}
	void copyTextureToBuffer(const rhi::BufferTextureCopyDesc&) override {}
	void blitTexture(const rhi::TextureBlitDesc&) override {}
	void fillBuffer(rhi::BufferHandle, uint64_t, uint64_t, uint32_t) override {}
	uint64_t copyCount{0u};

private:
	TransactionFakeCapture* m_capture{nullptr};
};

class TransactionFakeCommandBuffer final : public rhi::CommandBuffer
{
public:
	explicit TransactionFakeCommandBuffer(TransactionFakeCapture* capture = nullptr)
		: compute(capture), m_capture(capture)
	{
	}

	void begin(rhi::CommandAllocator&) override
	{
		m_state = rhi::CommandBufferState::recording;
	}
	void end() override { m_state = rhi::CommandBufferState::executable; }
	[[nodiscard]] rhi::CommandBufferState state() const noexcept override
	{
		return m_state;
	}
	rhi::RenderEncoder* beginRenderPass(const rhi::RenderPassDesc&) override
	{
		return nullptr;
	}
	rhi::ComputeEncoder* beginComputePass() override { return &compute; }
	void endEncoding() override {}
	void barrier(rhi::StageFlags, rhi::StageFlags, rhi::HazardFlags) override {}
	void resourceBarrier(std::span<const rhi::TextureBarrier> textureBarriers,
	                     std::span<const rhi::BufferBarrier>,
	                     std::span<const rhi::AliasingBarrier>) override
	{
		if (m_capture == nullptr) return;
		for (const rhi::TextureBarrier& barrier : textureBarriers)
		{
			m_capture->textureBarriers.push_back(barrier);
			m_capture->events.push_back({
				TransactionFakeEventKind::textureBarrier,
				m_capture->textureBarriers.size() - 1u,
			});
		}
	}
	void clearColorTexture(rhi::TextureHandle,
	                       const rhi::TextureSubresourceRange&,
	                       const rhi::ClearColorValue&) override {}
	void beginEvent(const char*) override {}
	void endEvent() override {}
	void resetQueryPool(rhi::QueryPoolHandle, uint32_t, uint32_t) override {}
	void writeTimestamp(rhi::QueryPoolHandle, uint32_t, bool) override {}

	TransactionFakeComputeEncoder compute;

private:
	rhi::CommandBufferState m_state{rhi::CommandBufferState::idle};
	TransactionFakeCapture* m_capture{nullptr};
};

class TransactionFakeAllocator final : public rhi::CommandAllocator
{
public:
	explicit TransactionFakeAllocator(TransactionFakeCapture* capture = nullptr)
		: m_capture(capture)
	{
	}

	[[nodiscard]] rhi::QueueClass queueClass() const noexcept override
	{
		return rhi::QueueClass::graphics;
	}
	void reset(rhi::SubmissionToken) override {}
	[[nodiscard]] std::unique_ptr<rhi::CommandBuffer> createCommandBuffer() override
	{
		return std::make_unique<TransactionFakeCommandBuffer>(m_capture);
	}

private:
	TransactionFakeCapture* m_capture{nullptr};
};

class TransactionFakeDevice final : public rhi::Device
{
public:
	struct BufferStorage
	{
		std::vector<std::byte> bytes;
		bool alive{true};
	};

	void init(const rhi::DeviceCreateInfo&) override {}
	void deinit() override {}
	rhi::BackendInfo getBackendInfo() const override
	{
		return {.type = rhi::BackendType::vulkan, .apiName = "transaction-fake"};
	}
	const char* getDeviceName() const override { return "TransactionFakeDevice"; }
	const rhi::PhysicalDeviceInfo& getPhysicalDeviceInfo() const override
	{
		return physicalInfo;
	}
	const rhi::DeviceFeatureInfo& getEnabledFeatureInfo() const override
	{
		return featureInfo;
	}
	rhi::CapabilityReport queryCapabilities() const override { return {}; }
	bool supports(rhi::CapabilityTier) const override { return true; }
	const rhi::MemoryProperties& getPhysicalMemoryProperties() const override
	{
		return memoryProperties;
	}
	rhi::Queue* getQueue(rhi::QueueClass queueClass) override
	{
		return queueClass == rhi::QueueClass::graphics ? &graphics : nullptr;
	}
	std::unique_ptr<rhi::CommandAllocator> createCommandAllocator(
		rhi::QueueClass queueClass) override
	{
		return queueClass == rhi::QueueClass::graphics
			? std::make_unique<TransactionFakeAllocator>(&capture)
			: nullptr;
	}
	void waitIdle() override { ++waitIdleCount; }
	void initSurface(rhi::Surface&, const rhi::WindowHandle&) override {}
	std::unique_ptr<rhi::Swapchain> createSwapchain(rhi::Surface&, bool) override
	{
		return nullptr;
	}
	bool isFormatSupported(rhi::TextureFormat, rhi::FormatFeatureFlag) const override
	{
		return true;
	}

	rhi::TextureViewHandle createTextureView(
		const rhi::TextureViewCreateDesc& desc) override
	{
		++createdCount;
		const rhi::TextureViewHandle handle{{nextView++, 1u}};
		capture.views.emplace_back(handle, desc);
		return handle;
	}
	void destroyTextureView(rhi::TextureViewHandle handle) override
	{
		recordDestroy(rhi::ResourceKind::TextureView, handle.index, handle.generation);
	}
	rhi::TextureHandle createTexture(const rhi::TextureDesc& desc) override
	{
		++createdCount;
		const rhi::TextureHandle handle{nextTexture++, 1u};
		capture.textures.emplace_back(handle, desc);
		return handle;
	}
	void destroyTexture(rhi::TextureHandle handle) override
	{
		recordDestroy(rhi::ResourceKind::Texture, handle.index, handle.generation);
	}
	rhi::BufferHandle createBuffer(const rhi::BufferDesc& desc) override
	{
		if (desc.size == 0u || desc.size > std::numeric_limits<size_t>::max())
		{
			throw std::runtime_error("transaction fake buffer size is invalid");
		}
		const rhi::BufferHandle handle{nextBuffer++, 1u};
		if (buffers.size() <= handle.index) buffers.resize(handle.index + 1u);
		buffers[handle.index].bytes.resize(static_cast<size_t>(desc.size));
		buffers[handle.index].alive = true;
		capture.buffers.emplace_back(handle, desc);
		++createdCount;
		return handle;
	}
	void destroyBuffer(rhi::BufferHandle handle) override
	{
		if (handle.index < buffers.size()) buffers[handle.index].alive = false;
		recordDestroy(rhi::ResourceKind::Buffer, handle.index, handle.generation);
	}
	rhi::GpuPtr getBufferGpuAddress(rhi::BufferHandle handle) const override
	{
		return {0x100000u + static_cast<uint64_t>(handle.index) * 0x1000u};
	}
	void* mapBuffer(rhi::BufferHandle handle) override
	{
		return buffers.at(handle.index).bytes.data();
	}
	void unmapBuffer(rhi::BufferHandle) override {}
	rhi::SamplerHandle createSampler(const rhi::SamplerDesc&) override
	{
		++createdCount;
		return {nextSampler++, 1u};
	}
	void destroySampler(rhi::SamplerHandle handle) override
	{
		recordDestroy(rhi::ResourceKind::Sampler, handle.index, handle.generation);
	}
	rhi::ArgumentLayoutHandle createArgumentLayout(
		const rhi::ArgumentLayoutDesc&) override
	{
		++createdCount;
		return {nextLayout++, 1u};
	}
	void destroyArgumentLayout(rhi::ArgumentLayoutHandle handle) override
	{
		recordDestroy(rhi::ResourceKind::ArgumentLayout, handle.index, handle.generation);
	}
	rhi::ArgumentTableHandle createArgumentTable(
		const rhi::ArgumentTableCreateDesc&) override
	{
		++createdCount;
		return {nextTable++, 1u};
	}
	void destroyArgumentTable(rhi::ArgumentTableHandle handle) override
	{
		recordDestroy(rhi::ResourceKind::ArgumentTable, handle.index, handle.generation);
	}
	void updateArgumentTable(rhi::ArgumentTableHandle table,
	                         rhi::ArgumentWriteBatch writes) override
	{
		descriptorWriteCount += writes.size();
		for (const rhi::ArgumentWrite& write : writes)
		{
			capture.descriptorWrites.push_back({table, write});
		}
	}
	rhi::ArgumentLayoutHandle getArgumentTableLayout(
		rhi::ArgumentTableHandle) const override { return {1u, 1u}; }
	rhi::PipelineHandle createGraphicsPipeline(const rhi::GraphicsPipelineDesc&) override
	{
		return {};
	}
	rhi::PipelineHandle createComputePipeline(const rhi::ComputePipelineDesc&) override
	{
		return {};
	}
	void destroyPipeline(rhi::PipelineHandle) override {}
	rhi::QueryPoolHandle createQueryPool(uint32_t) override { return {}; }
	void destroyQueryPool(rhi::QueryPoolHandle) override {}
	uint64_t getQueryPoolResult(rhi::QueryPoolHandle, uint32_t) override { return 0u; }
	bool getQueryPoolResultsWithAvailability(
		rhi::QueryPoolHandle, uint32_t, std::span<uint64_t>) override { return false; }
	rhi::ShaderLibraryHandle createShaderLibrary(
		const rhi::ShaderLibraryDesc&) override { return {}; }
	void destroyShaderLibrary(rhi::ShaderLibraryHandle) override {}

	void recordDestroy(rhi::ResourceKind kind, uint32_t index, uint32_t generation)
	{
		destroyed.push_back({kind, index, generation});
		++destroyedCount;
	}

	TransactionFakeQueue graphics;
	uint64_t createdCount{0u};
	uint64_t destroyedCount{0u};
	uint64_t waitIdleCount{0u};
	uint64_t descriptorWriteCount{0u};
	TransactionFakeCapture capture;
	std::vector<rhi::ResourceHandle> destroyed;

private:
	uint32_t nextBuffer{1u};
	uint32_t nextTexture{1u};
	uint32_t nextView{1u};
	uint32_t nextSampler{1u};
	uint32_t nextLayout{1u};
	uint32_t nextTable{1u};
	std::vector<BufferStorage> buffers{1u};
	rhi::PhysicalDeviceInfo physicalInfo{};
	rhi::DeviceFeatureInfo featureInfo{};
	rhi::MemoryProperties memoryProperties{};
};

bool exactMatrixEqual(const std::optional<glm::mat4>& left,
	                  const std::optional<glm::mat4>& right) noexcept
{
	if (left.has_value() != right.has_value()) return false;
	if (!left) return true;
	for (size_t column = 0u; column < 4u; ++column)
	{
		for (size_t row = 0u; row < 4u; ++row)
		{
			if ((*left)[column][row] != (*right)[column][row]) return false;
		}
	}
	return true;
}

bool exactMaterialEqual(const std::optional<SceneMaterial>& left,
	                    const std::optional<SceneMaterial>& right) noexcept
{
	if (left.has_value() != right.has_value()) return false;
	if (!left) return true;
	return left->baseColorFactor == right->baseColorFactor &&
	       left->metallicFactor == right->metallicFactor &&
	       left->roughnessFactor == right->roughnessFactor &&
	       left->normalScale == right->normalScale &&
	       left->occlusionStrength == right->occlusionStrength &&
	       left->emissiveFactor == right->emissiveFactor &&
	       left->_padding == right->_padding &&
	       left->baseColorTexture == right->baseColorTexture &&
	       left->normalTexture == right->normalTexture &&
	       left->metallicRoughnessTexture == right->metallicRoughnessTexture &&
	       left->occlusionTexture == right->occlusionTexture &&
	       left->emissiveTexture == right->emissiveTexture &&
	       left->alphaMode == right->alphaMode &&
	       left->alphaCutoff == right->alphaCutoff &&
	       left->materialWorkflow == right->materialWorkflow &&
	       left->doubleSided == right->doubleSided &&
	       left->name == right->name &&
	       left->extensionsJson == right->extensionsJson &&
	       left->extrasJson == right->extrasJson;
}

bool exactCameraEqual(const std::optional<SceneCamera>& left,
	                  const std::optional<SceneCamera>& right) noexcept
{
	if (left.has_value() != right.has_value()) return false;
	if (!left) return true;
	return left->name == right->name &&
	       left->nodeIndex == right->nodeIndex &&
	       left->type == right->type &&
	       left->aspectRatio == right->aspectRatio &&
	       left->verticalFieldOfViewRadians ==
	           right->verticalFieldOfViewRadians &&
	       left->nearPlane == right->nearPlane &&
	       left->farPlane == right->farPlane &&
	       left->horizontalMagnification == right->horizontalMagnification &&
	       left->verticalMagnification == right->verticalMagnification &&
	       left->sourceCameraIndex == right->sourceCameraIndex &&
	       left->runtimeIdentity == right->runtimeIdentity;
}

bool exactLightEqual(const std::optional<SceneLight>& left,
	                 const std::optional<SceneLight>& right) noexcept
{
	if (left.has_value() != right.has_value()) return false;
	if (!left) return true;
	return left->name == right->name &&
	       left->nodeIndex == right->nodeIndex &&
	       left->type == right->type &&
	       left->enabled == right->enabled &&
	       left->color == right->color &&
	       left->intensity == right->intensity &&
	       left->range == right->range &&
	       left->innerConeAngle == right->innerConeAngle &&
	       left->outerConeAngle == right->outerConeAngle &&
	       left->sourceLightIndex == right->sourceLightIndex &&
	       left->runtimeIdentity == right->runtimeIdentity;
}

bool exactConcreteStateEqual(
	const SceneTransactionConcreteStateSnapshot& left,
	const SceneTransactionConcreteStateSnapshot& right) noexcept
{
	return left.initialized == right.initialized &&
	       left.resourceBearing == right.resourceBearing &&
	       left.generation == right.generation &&
	       left.descriptorGeneration == right.descriptorGeneration &&
	       left.resourceCount == right.resourceCount &&
	       left.pendingRetiredGenerationCount ==
	           right.pendingRetiredGenerationCount &&
	       left.immediatelyDestroyedResourceCount ==
	           right.immediatelyDestroyedResourceCount &&
	       left.eventuallyDestroyedResourceCount ==
	           right.eventuallyDestroyedResourceCount &&
	       left.queueSubmissionCount == right.queueSubmissionCount &&
	       left.lastSubmissionToken == right.lastSubmissionToken &&
	       left.fence == right.fence && left.mapping == right.mapping &&
	       left.meshHandles == right.meshHandles &&
	       left.textureHandles == right.textureHandles &&
	       left.materialHandles == right.materialHandles &&
	       left.sceneObjectHandles == right.sceneObjectHandles &&
	       left.cameraHandles == right.cameraHandles &&
	       left.lightHandles == right.lightHandles &&
	       exactMatrixEqual(left.firstNodeWorldTransform,
	                        right.firstNodeWorldTransform) &&
	       left.firstNodeVisibility == right.firstNodeVisibility &&
	       exactMaterialEqual(left.firstMaterial, right.firstMaterial) &&
	       left.firstPrimitiveMaterialIndex == right.firstPrimitiveMaterialIndex &&
	       exactCameraEqual(left.firstCamera, right.firstCamera) &&
	       exactLightEqual(left.firstLight, right.firstLight) &&
	       left.rendererObjectCount == right.rendererObjectCount &&
	       left.rendererDrawCount == right.rendererDrawCount &&
	       left.deferredRegistryUpdateCount == right.deferredRegistryUpdateCount &&
	       exactMatrixEqual(left.firstRendererWorldTransform,
	                        right.firstRendererWorldTransform) &&
	       left.firstRendererVisibility == right.firstRendererVisibility &&
	       left.firstRendererMaterialIndex == right.firstRendererMaterialIndex &&
	       left.firstPlanInstanceMaterialIndex ==
	           right.firstPlanInstanceMaterialIndex &&
	       left.firstPlanDrawMaterialIndex ==
	           right.firstPlanDrawMaterialIndex &&
	       left.rendererStateMatchesGeneration ==
	           right.rendererStateMatchesGeneration;
}

struct RendererTransactionObservation
{
	RendererSceneStateSnapshot scene;
	SceneTransactionConcreteStateSnapshot concrete;
	SceneDeltaCounterSnapshot counters;
	uint64_t visibleGeneration{0u};
	uint64_t createdCount{0u};
	uint64_t destroyedCount{0u};
	uint64_t descriptorWriteCount{0u};
	uint64_t deviceWaitIdleCount{0u};
	uint64_t queueWaitIdleCount{0u};
	uint64_t bufferCopyCount{0u};
	size_t bufferCreateCalls{0u};
	size_t textureCreateCalls{0u};
	size_t viewCreateCalls{0u};
	size_t descriptorWriteCalls{0u};
	size_t textureBarrierCalls{0u};
	size_t textureCopyCalls{0u};
	size_t captureEventCount{0u};
	std::vector<rhi::ResourceHandle> destroyedResources;
	std::vector<rhi::SubmissionToken> submissions;
	std::vector<rhi::SubmissionToken> completionQueries;
	rhi::SubmissionToken lastSubmittedToken{};
};

RendererTransactionObservation observeRendererTransaction(
	RendererFacade& facade,
	const TransactionFakeDevice& fake)
{
	return {
		.scene = facade.activeSceneState(),
		.concrete = facade.sceneTransactionConcreteState(),
		.counters = facade.sceneDeltaCounters(),
		.visibleGeneration = facade.sceneTransactionVisibleGeneration(),
		.createdCount = fake.createdCount,
		.destroyedCount = fake.destroyedCount,
		.descriptorWriteCount = fake.descriptorWriteCount,
		.deviceWaitIdleCount = fake.waitIdleCount,
		.queueWaitIdleCount = fake.graphics.waitIdleCount,
		.bufferCopyCount = fake.capture.bufferCopyCount,
		.bufferCreateCalls = fake.capture.buffers.size(),
		.textureCreateCalls = fake.capture.textures.size(),
		.viewCreateCalls = fake.capture.views.size(),
		.descriptorWriteCalls = fake.capture.descriptorWrites.size(),
		.textureBarrierCalls = fake.capture.textureBarriers.size(),
		.textureCopyCalls = fake.capture.textureCopies.size(),
		.captureEventCount = fake.capture.events.size(),
		.destroyedResources = fake.destroyed,
		.submissions = fake.graphics.submissions,
		.completionQueries = fake.graphics.completionQueries,
		.lastSubmittedToken = fake.graphics.lastSubmittedToken(),
	};
}

bool exactObservationEqual(const RendererTransactionObservation& left,
	                       const RendererTransactionObservation& right) noexcept
{
	return left.scene == right.scene &&
	       exactConcreteStateEqual(left.concrete, right.concrete) &&
	       left.counters == right.counters &&
	       left.visibleGeneration == right.visibleGeneration &&
	       left.createdCount == right.createdCount &&
	       left.destroyedCount == right.destroyedCount &&
	       left.descriptorWriteCount == right.descriptorWriteCount &&
	       left.deviceWaitIdleCount == right.deviceWaitIdleCount &&
	       left.queueWaitIdleCount == right.queueWaitIdleCount &&
	       left.bufferCopyCount == right.bufferCopyCount &&
	       left.bufferCreateCalls == right.bufferCreateCalls &&
	       left.textureCreateCalls == right.textureCreateCalls &&
	       left.viewCreateCalls == right.viewCreateCalls &&
	       left.descriptorWriteCalls == right.descriptorWriteCalls &&
	       left.textureBarrierCalls == right.textureBarrierCalls &&
	       left.textureCopyCalls == right.textureCopyCalls &&
	       left.captureEventCount == right.captureEventCount &&
	       left.destroyedResources == right.destroyedResources &&
	       left.submissions == right.submissions &&
	       left.completionQueries == right.completionQueries &&
	       left.lastSubmittedToken == right.lastSubmittedToken;
}

SceneDigest digest(uint8_t seed)
{
	SceneDigest result{};
	for (size_t index = 0; index < result.size(); ++index)
	{
		result[index] = static_cast<uint8_t>(seed + index);
	}
	return result;
}

StableSceneId sceneId(EditorDocumentAuthority authority)
{
	return {EditorObjectKind::scene, authority, 1u, 1u};
}

StableSceneId alternateSceneId(EditorDocumentAuthority authority)
{
	return {EditorObjectKind::scene, authority, 8u, 1u};
}

StableSceneId nodeId(EditorDocumentAuthority authority)
{
	return {EditorObjectKind::node, authority, 2u, 1u};
}

StableSceneId alternateNodeId(EditorDocumentAuthority authority)
{
	return {EditorObjectKind::node, authority, 16u, 1u};
}

StableSceneId primitiveId(EditorDocumentAuthority authority)
{
	return {EditorObjectKind::primitive, authority, 3u, 1u};
}

StableSceneId alternatePrimitiveId(EditorDocumentAuthority authority)
{
	return {EditorObjectKind::primitive, authority, 11u, 1u};
}

StableSceneId meshId(EditorDocumentAuthority authority)
{
	return {EditorObjectKind::mesh, authority, 4u, 1u};
}

StableSceneId materialId(EditorDocumentAuthority authority)
{
	return {EditorObjectKind::material, authority, 5u, 1u};
}

StableSceneId cameraId(EditorDocumentAuthority authority)
{
	return {EditorObjectKind::camera, authority, 6u, 1u};
}

StableSceneId alternateCameraId(EditorDocumentAuthority authority)
{
	return {EditorObjectKind::camera, authority, 17u, 1u};
}

StableSceneId lightId(EditorDocumentAuthority authority)
{
	return {EditorObjectKind::light, authority, 7u, 1u};
}

StableSceneId alternateLightId(EditorDocumentAuthority authority)
{
	return {EditorObjectKind::light, authority, 18u, 1u};
}

StableSceneId imageId(EditorDocumentAuthority authority)
{
	return {EditorObjectKind::image, authority, 9u, 1u};
}

StableSceneId textureId(EditorDocumentAuthority authority)
{
	return {EditorObjectKind::texture, authority, 10u, 1u};
}

StableSceneId alternateImageId(EditorDocumentAuthority authority)
{
	return {EditorObjectKind::image, authority, 12u, 1u};
}

StableSceneId alternateTextureId(EditorDocumentAuthority authority)
{
	return {EditorObjectKind::texture, authority, 13u, 1u};
}

StableSceneId samplerId(EditorDocumentAuthority authority)
{
	return {EditorObjectKind::sampler, authority, 14u, 1u};
}

StableSceneId alternateSamplerId(EditorDocumentAuthority authority)
{
	return {EditorObjectKind::sampler, authority, 15u, 1u};
}

RendererTargetFence baseFence(rhi::BackendType backend = rhi::BackendType::vulkan,
	                           RendererSceneUpdateCapability capability =
		                           RendererSceneUpdateCapability::IncrementalAndSnapshot)
{
	RendererTargetFence fence;
	fence.backend = backend;
	fence.capability = capability;
	fence.renderer = {101u, 7u};
	fence.device = {202u, 9u};
	fence.mappingEpoch = 3u;
	fence.authority = {77u};
	fence.appliedDocumentRevision = 10u;
	fence.sceneActivationEpoch = 4u;
	fence.rootExternalStateSerial = 6u;
	fence.activeScene = sceneId(fence.authority);
	fence.projectionDigest = digest(20u);
	return fence;
}

RendererTargetFence targetFence(const RendererTargetFence& base,
	                             const SceneChangeHeader& header)
{
	RendererTargetFence result = base;
	++result.mappingEpoch;
	result.authority = header.authority;
	result.appliedDocumentRevision = header.targetDocumentRevision;
	result.sceneActivationEpoch = header.targetSceneActivationEpoch;
	result.rootExternalStateSerial = header.targetRootExternalStateSerial;
	result.activeScene = header.targetActiveScene;
	result.projectionDigest = header.targetProjectionDigest;
	return result;
}

SceneDigest projectionIdentityDigest(StableSceneId id, uint64_t runtimeIndex)
{
	std::vector<uint8_t> bytes;
	bytes.reserve(25u);
	const auto append = [&bytes](uint64_t value, unsigned count)
	{
		for (unsigned index = 0u; index < count; ++index)
		{
			bytes.push_back(static_cast<uint8_t>(value & 0xffu));
			value >>= 8u;
		}
	};
	append(static_cast<uint8_t>(id.kind), 1u);
	append(id.authority.value, 8u);
	append(id.slot, 4u);
	append(id.generation, 4u);
	append(runtimeIndex, 8u);
	return SceneChangeCanonicalizer::computeSha256(bytes);
}

RendererSceneMappingSnapshot rendererMappingSnapshot(
	const RendererTargetFence& fence,
	std::vector<RendererSceneMappingEntry> entries,
	const RendererSceneMappingSnapshot* activeMapping = nullptr)
{
	std::sort(entries.begin(), entries.end(),
	          [](const auto& left, const auto& right)
	          {
		          return left.stableId < right.stableId;
	          });
	std::array<uint32_t, 8u> nextHandleIndices{};
	nextHandleIndices.fill(1u);
	if (activeMapping != nullptr)
	{
		for (const RendererSceneMappingEntry& active : activeMapping->entries())
		{
			const size_t kindIndex = static_cast<size_t>(active.rendererHandle.kind);
			require(kindIndex < nextHandleIndices.size() &&
			        active.rendererHandle.index != std::numeric_limits<uint32_t>::max(),
			        "active renderer mapping fixture exhausted its handle namespace");
			nextHandleIndices[kindIndex] = std::max(
				nextHandleIndices[kindIndex], active.rendererHandle.index + 1u);
		}
	}
	std::vector<SceneRuntimeMappingEntry> authorityEntries;
	authorityEntries.reserve(entries.size());
	for (RendererSceneMappingEntry& entry : entries)
	{
		const EditorObjectId documentId = editorObjectId(entry.stableId);
		const uint64_t identity =
			SceneRuntimeMapping::deriveProjectionIdentity(documentId);
		const bool projectionBacked =
			entry.stableId.kind == EditorObjectKind::node ||
			entry.stableId.kind == EditorObjectKind::primitive ||
			entry.stableId.kind == EditorObjectKind::mesh ||
			entry.stableId.kind == EditorObjectKind::material ||
			entry.stableId.kind == EditorObjectKind::image ||
			entry.stableId.kind == EditorObjectKind::camera ||
			entry.stableId.kind == EditorObjectKind::light ||
			entry.stableId.kind == EditorObjectKind::animation;
		if (entry.stableId.kind == EditorObjectKind::scene)
		{
			entry.projection.sceneAssetIndex = entry.stableId.slot;
		}
		entry.projection.kind = entry.stableId.kind;
		entry.projection.runtimeIndex = identity;
		entry.projection.identityDigest =
			projectionIdentityDigest(entry.stableId, identity);
		const size_t kindIndex = static_cast<size_t>(entry.rendererHandle.kind);
		require(kindIndex < nextHandleIndices.size(),
		        "renderer mapping fixture has an invalid renderer kind");
		const RendererSceneMappingEntry* active =
			activeMapping != nullptr ? activeMapping->find(entry.stableId) : nullptr;
		if (active != nullptr && active->rendererHandle.kind == entry.rendererHandle.kind)
		{
			entry.rendererHandle = active->rendererHandle;
		}
		else
		{
			entry.rendererHandle.index = nextHandleIndices[kindIndex]++;
			entry.rendererHandle.generation = 1u;
			entry.rendererHandle.rendererGeneration = fence.renderer.generation;
			entry.rendererHandle.deviceGeneration = fence.device.generation;
		}
		authorityEntries.push_back(SceneRuntimeMappingEntry{
			.documentId = documentId,
			.sceneAssetIndex = projectionBacked
				? entry.projection.sceneAssetIndex
				: SceneRuntimeMapping::kInvalidIndex,
			.projectionIdentity = projectionBacked
				? identity
				: SceneRuntimeMapping::kInvalidProjectionIdentity,
			.runtimeIndex = projectionBacked
				? identity
				: SceneRuntimeMapping::kInvalidRuntimeIndex,
		});
	}
	std::string error;
	std::optional<SceneRuntimeMapping> authority = SceneRuntimeMapping::create(
		SceneRuntimeMappingFence{
			.authority = fence.authority,
			.appliedDocumentRevision = fence.appliedDocumentRevision,
			.activeScene = fence.activeScene,
			.sceneActivationEpoch = fence.sceneActivationEpoch,
			.rootExternalStateSerial = fence.rootExternalStateSerial,
			.projectionDigest = fence.projectionDigest,
			.mappingEpoch = fence.mappingEpoch,
		},
		std::move(authorityEntries), &error);
	require(authority.has_value(), error.c_str());
	return RendererSceneMappingSnapshot{
		fence, std::move(entries), std::move(*authority)};
}

RendererSceneMappingSnapshot outerMappingMutation(
	const RendererSceneMappingSnapshot& mapping,
	const std::function<void(std::vector<RendererSceneMappingEntry>&)>& mutate)
{
	require(mapping.projectionAuthority() != nullptr,
	        "mapping mutation fixture lost its trusted projection authority");
	std::vector<RendererSceneMappingEntry> entries(
		mapping.entries().begin(), mapping.entries().end());
	mutate(entries);
	return RendererSceneMappingSnapshot{
		mapping.fence(), std::move(entries), *mapping.projectionAuthority()};
}

RendererSceneMappingSnapshot mutateOuterEntry(
	const RendererSceneMappingSnapshot& mapping,
	StableSceneId sourceId,
	const std::function<void(RendererSceneMappingEntry&)>& mutate)
{
	return outerMappingMutation(
		mapping,
		[&](std::vector<RendererSceneMappingEntry>& entries)
		{
			auto found = std::find_if(
				entries.begin(), entries.end(),
				[&](const RendererSceneMappingEntry& entry)
				{
					return entry.stableId == sourceId;
				});
			require(found != entries.end(),
			        "mapping mutation fixture lost its source identity");
			mutate(*found);
		});
}

RendererSceneMappingSnapshot withoutOuterRecord(
	const RendererSceneMappingSnapshot& mapping,
	StableSceneId sourceId)
{
	return outerMappingMutation(
		mapping,
		[&](std::vector<RendererSceneMappingEntry>& entries)
		{
			const auto found = std::find_if(
				entries.begin(), entries.end(),
				[&](const RendererSceneMappingEntry& entry)
				{
					return entry.stableId == sourceId;
				});
			require(found != entries.end(),
			        "missing-record fixture could not find its source identity");
			entries.erase(found);
		});
}

RendererSceneMappingSnapshot withSwappedOuterAssociations(
	const RendererSceneMappingSnapshot& mapping,
	StableSceneId firstId,
	StableSceneId secondId)
{
	return outerMappingMutation(
		mapping,
		[&](std::vector<RendererSceneMappingEntry>& entries)
		{
			auto first = std::find_if(
				entries.begin(), entries.end(),
				[&](const RendererSceneMappingEntry& entry)
				{
					return entry.stableId == firstId;
				});
			auto second = std::find_if(
				entries.begin(), entries.end(),
				[&](const RendererSceneMappingEntry& entry)
				{
					return entry.stableId == secondId;
				});
			require(first != entries.end() && second != entries.end(),
			        "association-swap fixture lost a source identity");
			std::swap(first->projection, second->projection);
			std::swap(first->rendererHandle, second->rendererHandle);
		});
}

RendererSceneMappingSnapshot withCoherentlySwappedProjectionAuthority(
	const RendererSceneMappingSnapshot& mapping,
	StableSceneId firstId,
	StableSceneId secondId)
{
	require(mapping.projectionAuthority() != nullptr,
	        "coherent-swap fixture requires projection authority");
	std::vector<RendererSceneMappingEntry> outer(
		mapping.entries().begin(), mapping.entries().end());
	auto firstOuter = std::find_if(
		outer.begin(), outer.end(),
		[&](const RendererSceneMappingEntry& entry)
		{
			return entry.stableId == firstId;
		});
	auto secondOuter = std::find_if(
		outer.begin(), outer.end(),
		[&](const RendererSceneMappingEntry& entry)
		{
			return entry.stableId == secondId;
		});
	require(firstOuter != outer.end() && secondOuter != outer.end(),
	        "coherent-swap fixture lost an outer source");
	std::swap(firstOuter->projection.sceneAssetIndex,
	          secondOuter->projection.sceneAssetIndex);

	const SceneRuntimeMapping& original = *mapping.projectionAuthority();
	std::vector<SceneRuntimeMappingEntry> authority(
		original.entries().begin(), original.entries().end());
	const EditorObjectId firstDocumentId = editorObjectId(firstId);
	const EditorObjectId secondDocumentId = editorObjectId(secondId);
	auto firstAuthority = std::find_if(
		authority.begin(), authority.end(),
		[&](const SceneRuntimeMappingEntry& entry)
		{
			return entry.documentId == firstDocumentId;
		});
	auto secondAuthority = std::find_if(
		authority.begin(), authority.end(),
		[&](const SceneRuntimeMappingEntry& entry)
		{
			return entry.documentId == secondDocumentId;
		});
	require(firstAuthority != authority.end() &&
	        secondAuthority != authority.end(),
	        "coherent-swap fixture lost an authority source");
	std::swap(firstAuthority->sceneAssetIndex,
	          secondAuthority->sceneAssetIndex);

	std::string error;
	std::optional<SceneRuntimeMapping> forged = SceneRuntimeMapping::create(
		original.fence(), std::move(authority),
		std::vector<SceneRuntimeMappingAlias>(original.aliases().begin(),
		                                      original.aliases().end()),
		&error);
	require(forged.has_value(), error.c_str());
	RendererSceneMappingSnapshot result{
		mapping.fence(), std::move(outer), std::move(*forged)};
	require(result.validate().valid(),
	        "coherent swap must reach renderer asset-identity validation");
	return result;
}

RendererSceneMappingSnapshot withExtraneousConcreteRecord(
	const RendererSceneMappingSnapshot& mapping)
{
	const RendererTargetFence fence = mapping.fence();
	return outerMappingMutation(
		mapping,
		[&](std::vector<RendererSceneMappingEntry>& entries)
		{
			const StableSceneId forged{
				EditorObjectKind::scene, fence.authority, 99u, 1u};
			const uint64_t identity =
				SceneRuntimeMapping::deriveProjectionIdentity(editorObjectId(forged));
			entries.push_back(RendererSceneMappingEntry{
				.stableId = forged,
				.projection = {
					.kind = forged.kind,
					.sceneAssetIndex = forged.slot,
					.runtimeIndex = identity,
					.identityDigest = projectionIdentityDigest(forged, identity),
				},
				.rendererHandle = {
					.kind = RendererObjectKind::SceneObject,
					.index = 1000u,
					.generation = 1u,
					.rendererGeneration = fence.renderer.generation,
					.deviceGeneration = fence.device.generation,
				},
			});
		});
}

RendererSceneMappingSnapshot mappingFor(const RendererTargetFence& fence)
{
	const auto entry = [&](StableSceneId stableId,
	                       uint32_t sceneAssetIndex,
	                       RendererObjectKind rendererKind)
	{
		return RendererSceneMappingEntry{
			.stableId = stableId,
			.projection = {
				.kind = stableId.kind,
				.sceneAssetIndex = sceneAssetIndex,
			},
			.rendererHandle = {
				.kind = rendererKind,
			},
		};
	};
	std::vector<RendererSceneMappingEntry> entries;
	entries.push_back(entry(sceneId(fence.authority), 0u,
	                        RendererObjectKind::SceneObject));
	entries.push_back(entry(alternateSceneId(fence.authority), 1u,
	                        RendererObjectKind::SceneObject));
	entries.push_back(entry(nodeId(fence.authority), 0u,
	                        RendererObjectKind::SceneObject));
	entries.push_back(entry(primitiveId(fence.authority), 0u,
	                        RendererObjectKind::SceneObject));
	entries.push_back(entry(meshId(fence.authority), 0u,
	                        RendererObjectKind::Geometry));
	entries.push_back(entry(materialId(fence.authority), 0u,
	                        RendererObjectKind::Material));
	entries.push_back(entry(cameraId(fence.authority), 0u,
	                        RendererObjectKind::Camera));
	entries.push_back(entry(lightId(fence.authority), 0u,
	                        RendererObjectKind::Light));
	entries.push_back(entry(imageId(fence.authority), 0u,
	                        RendererObjectKind::Texture));
	entries.push_back(entry(textureId(fence.authority), 0u,
	                        RendererObjectKind::Texture));
	return rendererMappingSnapshot(fence, std::move(entries));
}

RendererSceneMappingSnapshot multiCameraLightMappingFor(
	const RendererTargetFence& fence,
	const RendererSceneMappingSnapshot* activeMapping = nullptr)
{
	const RendererSceneMappingSnapshot base = mappingFor(fence);
	std::vector<RendererSceneMappingEntry> entries(
		base.entries().begin(), base.entries().end());
	entries.push_back(RendererSceneMappingEntry{
		.stableId = alternateCameraId(fence.authority),
		.projection = {
			.kind = EditorObjectKind::camera,
			.sceneAssetIndex = 1u,
		},
		.rendererHandle = {
			.kind = RendererObjectKind::Camera,
		},
	});
	entries.push_back(RendererSceneMappingEntry{
		.stableId = alternateLightId(fence.authority),
		.projection = {
			.kind = EditorObjectKind::light,
			.sceneAssetIndex = 1u,
		},
		.rendererHandle = {
			.kind = RendererObjectKind::Light,
		},
	});
	return rendererMappingSnapshot(fence, std::move(entries), activeMapping);
}

RendererSceneMappingSnapshot unusedConcreteMappingFor(
	const RendererTargetFence& fence,
	const RendererSceneMappingSnapshot& activeMapping)
{
	const RendererSceneMappingSnapshot base = mappingFor(fence);
	std::vector<RendererSceneMappingEntry> entries(
		base.entries().begin(), base.entries().end());
	entries.push_back(RendererSceneMappingEntry{
		.stableId = alternateNodeId(fence.authority),
		.projection = {
			.kind = EditorObjectKind::node,
			.sceneAssetIndex = 1u,
		},
		.rendererHandle = {
			.kind = RendererObjectKind::SceneObject,
		},
	});
	entries.push_back(RendererSceneMappingEntry{
		.stableId = alternatePrimitiveId(fence.authority),
		.projection = {
			.kind = EditorObjectKind::primitive,
			.sceneAssetIndex = 1u,
		},
		.rendererHandle = {
			.kind = RendererObjectKind::SceneObject,
		},
	});
	return rendererMappingSnapshot(fence, std::move(entries), &activeMapping);
}

RendererSceneMappingSnapshot withForgedConcreteGeneration(
	const RendererSceneMappingSnapshot& mapping,
	StableSceneId sourceId)
{
	std::vector<RendererSceneMappingEntry> entries(
		mapping.entries().begin(), mapping.entries().end());
	const auto entry = std::find_if(
		entries.begin(), entries.end(),
		[&](const RendererSceneMappingEntry& candidate)
		{
			return candidate.stableId == sourceId;
		});
	require(entry != entries.end(),
	        "forged concrete-generation fixture lost its source identity");
	entry->rendererHandle.generation =
		entry->rendererHandle.generation == std::numeric_limits<uint32_t>::max()
			? entry->rendererHandle.generation - 1u
			: entry->rendererHandle.generation + 1u;
	require(mapping.projectionAuthority() != nullptr,
	        "forged concrete-generation fixture lost projection authority");
	RendererSceneMappingSnapshot forged{
		mapping.fence(), std::move(entries), *mapping.projectionAuthority()};
	require(forged.validate().valid(),
	        "forged concrete generation must retain a legal mapping fence");
	return forged;
}

RendererSceneMappingSnapshot attachmentMappingFor(
	const RendererTargetFence& fence)
{
	const RendererSceneMappingSnapshot base = mappingFor(fence);
	std::vector<RendererSceneMappingEntry> entries(
		base.entries().begin(), base.entries().end());
	entries.push_back(RendererSceneMappingEntry{
		.stableId = alternateNodeId(fence.authority),
		.projection = {
			.kind = EditorObjectKind::node,
			.sceneAssetIndex = 1u,
			.runtimeIndex = 20u,
			.identityDigest = digest(50u),
		},
		.rendererHandle = {
			.kind = RendererObjectKind::SceneObject,
			.index = 20u,
			.generation = 11u,
			.rendererGeneration = fence.renderer.generation,
			.deviceGeneration = fence.device.generation,
		},
	});
	return rendererMappingSnapshot(fence, std::move(entries));
}

RendererSceneMappingSnapshot semanticTextureMappingFor(
	const RendererTargetFence& fence,
	uint32_t colorSceneAssetIndex = 1u,
	uint32_t linearSceneAssetIndex = 0u)
{
	const RendererSceneMappingSnapshot base = mappingFor(fence);
	std::vector<RendererSceneMappingEntry> entries(
		base.entries().begin(), base.entries().end());
	const auto texture = std::find_if(
		entries.begin(), entries.end(), [&](const RendererSceneMappingEntry& entry)
		{
			return entry.stableId == textureId(fence.authority);
		});
	require(texture != entries.end(),
	        "semantic texture mapping fixture lost its primary texture");
	texture->projection.colorSceneAssetIndex = colorSceneAssetIndex;
	texture->projection.linearSceneAssetIndex = linearSceneAssetIndex;
	entries.push_back(RendererSceneMappingEntry{
		.stableId = alternateTextureId(fence.authority),
		.projection = {
			.kind = EditorObjectKind::texture,
			.sceneAssetIndex = 0u,
			.runtimeIndex = 20u,
			.identityDigest = digest(50u),
			.colorSceneAssetIndex = colorSceneAssetIndex,
			.linearSceneAssetIndex = linearSceneAssetIndex,
		},
		.rendererHandle = {
			.kind = RendererObjectKind::Texture,
			.index = 20u,
			.generation = 11u,
			.rendererGeneration = fence.renderer.generation,
			.deviceGeneration = fence.device.generation,
		},
	});
	entries.push_back(RendererSceneMappingEntry{
		.stableId = samplerId(fence.authority),
		.projection = {
			.kind = EditorObjectKind::sampler,
			.sceneAssetIndex = 0u,
			.runtimeIndex = 21u,
			.identityDigest = digest(51u),
		},
		.rendererHandle = {
			.kind = RendererObjectKind::Sampler,
			.index = 21u,
			.generation = 12u,
			.rendererGeneration = fence.renderer.generation,
			.deviceGeneration = fence.device.generation,
		},
	});
	entries.push_back(RendererSceneMappingEntry{
		.stableId = alternateSamplerId(fence.authority),
		.projection = {
			.kind = EditorObjectKind::sampler,
			.sceneAssetIndex = 1u,
			.runtimeIndex = 22u,
			.identityDigest = digest(52u),
		},
		.rendererHandle = {
			.kind = RendererObjectKind::Sampler,
			.index = 22u,
			.generation = 13u,
			.rendererGeneration = fence.renderer.generation,
			.deviceGeneration = fence.device.generation,
		},
	});
	return rendererMappingSnapshot(fence, std::move(entries));
}

RendererSceneMappingSnapshot multiPrimitiveMappingFor(
	const RendererTargetFence& fence)
{
	const RendererSceneMappingSnapshot base = mappingFor(fence);
	std::vector<RendererSceneMappingEntry> entries(
		base.entries().begin(), base.entries().end());
	entries.push_back(RendererSceneMappingEntry{
		.stableId = alternatePrimitiveId(fence.authority),
		.projection = {
			.kind = EditorObjectKind::primitive,
			.sceneAssetIndex = 1u,
			.runtimeIndex = 20u,
			.identityDigest = digest(50u),
		},
		.rendererHandle = {
			.kind = RendererObjectKind::SceneObject,
			.index = 20u,
			.generation = 11u,
			.rendererGeneration = fence.renderer.generation,
			.deviceGeneration = fence.device.generation,
		},
	});
	return rendererMappingSnapshot(fence, std::move(entries));
}

SceneAsset sceneAssetFixture(
	uint64_t documentRevision,
	EditorDocumentAuthority authority = {77u})
{
	SceneAsset asset;
	asset.documentRevision = documentRevision;
	SceneNode node;
	node.sourceNodeIndex = 0u;
	node.runtimeIdentity =
		SceneRuntimeMapping::deriveProjectionIdentity(
			editorObjectId(nodeId(authority)));
	node.meshRefs.push_back(0u);
	asset.nodes.push_back(std::move(node));
	asset.materials.push_back(SceneMaterial{});
	asset.meshes.push_back(SceneMesh{
		.vertexCount = 3u,
		.indexCount = 3u,
		.materialIndex = 0u,
		.localBoundsMin = {-1.0f, -1.0f, 0.0f},
		.localBoundsMax = {1.0f, 1.0f, 0.0f},
	});
	ScenePrimitive primitive{
		.meshIndex = 0u,
		.nodeIndex = 0u,
		.materialIndex = 0,
	};
	primitive.runtimeIdentity =
		SceneRuntimeMapping::deriveProjectionIdentity(
			editorObjectId(primitiveId(authority)));
	asset.primitives.push_back(std::move(primitive));
	asset.cameras.push_back(SceneCamera{
		.nodeIndex = 0,
		.sourceCameraIndex = 0u,
		.runtimeIdentity = SceneRuntimeMapping::deriveProjectionIdentity(
			editorObjectId(cameraId(authority))),
	});
	asset.lights.push_back(SceneLight{
		.nodeIndex = 0,
		.sourceLightIndex = 0u,
		.runtimeIdentity = SceneRuntimeMapping::deriveProjectionIdentity(
			editorObjectId(lightId(authority))),
	});
	asset.textures.push_back(SceneTexture{
		.width = 1u,
		.height = 1u,
		.mipLevels = 1u,
		.format = rhi::TextureFormat::rgba8Unorm,
		.payloadOffset = 0u,
		.payloadSize = 4u,
	});
	asset.texturePayload = {255u, 255u, 255u, 255u};
	std::vector<uint8_t> vertices(3u * kSceneUploadVertexStride);
	const std::array<glm::vec3, 3> positions{{
		{-1.0f, -1.0f, 0.0f},
		{1.0f, -1.0f, 0.0f},
		{0.0f, 1.0f, 0.0f},
	}};
	for (size_t index = 0; index < positions.size(); ++index)
	{
		std::memcpy(vertices.data() + index * kSceneUploadVertexStride, &positions[index], sizeof(glm::vec3));
	}
	const std::array<uint32_t, 3> indices{{0u, 1u, 2u}};
	std::vector<uint8_t> indexBytes(sizeof(indices));
	std::memcpy(indexBytes.data(), indices.data(), sizeof(indices));
	asset.vertexPayload = std::move(vertices);
	asset.indexPayload = std::move(indexBytes);
	asset.rootNodes.push_back(0u);
	asset.sceneRootTransformApplyToRoot.push_back(1u);
	return asset;
}

SceneAsset multiCameraLightSceneAssetFixture(
	uint64_t documentRevision,
	EditorDocumentAuthority authority = {77u})
{
	SceneAsset asset = sceneAssetFixture(documentRevision, authority);
	SceneCamera secondCamera;
	secondCamera.name = "alternate camera";
	secondCamera.nodeIndex = 0;
	secondCamera.verticalFieldOfViewRadians = 0.9f;
	secondCamera.sourceCameraIndex = 1u;
	secondCamera.runtimeIdentity = SceneRuntimeMapping::deriveProjectionIdentity(
		editorObjectId(alternateCameraId(authority)));
	asset.cameras.push_back(std::move(secondCamera));
	SceneLight secondLight;
	secondLight.name = "alternate light";
	secondLight.nodeIndex = 0;
	secondLight.intensity = 3.0f;
	secondLight.sourceLightIndex = 1u;
	secondLight.runtimeIdentity = SceneRuntimeMapping::deriveProjectionIdentity(
		editorObjectId(alternateLightId(authority)));
	asset.lights.push_back(std::move(secondLight));
	return asset;
}

SceneAsset unusedConcreteSceneAssetFixture(
	uint64_t documentRevision,
	EditorDocumentAuthority authority = {77u})
{
	SceneAsset asset = sceneAssetFixture(documentRevision, authority);
	SceneNode unusedNode;
	unusedNode.sourceNodeIndex = 1u;
	unusedNode.runtimeIdentity = SceneRuntimeMapping::deriveProjectionIdentity(
		editorObjectId(alternateNodeId(authority)));
	asset.nodes.push_back(std::move(unusedNode));
	ScenePrimitive unusedPrimitive{
		.meshIndex = 0u,
		.nodeIndex = 0u,
		.documentPrimitiveIndex = 1u,
		.materialIndex = 0,
	};
	unusedPrimitive.runtimeIdentity = SceneRuntimeMapping::deriveProjectionIdentity(
		editorObjectId(alternatePrimitiveId(authority)));
	asset.primitives.push_back(std::move(unusedPrimitive));
	return asset;
}

SceneAsset semanticTextureSceneAssetFixture(
	uint64_t documentRevision,
	EditorDocumentAuthority authority = {77u})
{
	SceneAsset asset = sceneAssetFixture(documentRevision, authority);
	asset.textures.front().sourceImageIndex = 0u;
	SceneTexture colorTexture = asset.textures.front();
	colorTexture.format = rhi::TextureFormat::rgba8Srgb;
	colorTexture.payloadOffset = 4u;
	colorTexture.sourceImageIndex = 0u;
	asset.textures.push_back(std::move(colorTexture));
	auto textureBytes = asset.texturePayload.vector();
	textureBytes.insert(textureBytes.end(), {32u, 64u, 128u, 255u});
	asset.texturePayload = std::move(textureBytes);
	return asset;
}

SceneAsset multiPrimitiveSceneAssetFixture(
	uint64_t documentRevision,
	EditorDocumentAuthority authority = {77u})
{
	SceneAsset asset = sceneAssetFixture(documentRevision, authority);
	const std::vector<uint8_t> firstVertices = asset.vertexPayload;
	const std::vector<uint8_t> firstIndices = asset.indexPayload;
	SceneMesh secondMesh = asset.meshes.front();
	secondMesh.vertexOffset = static_cast<uint32_t>(asset.vertexPayload.size());
	secondMesh.indexOffset = static_cast<uint32_t>(asset.indexPayload.size());
	secondMesh.sourcePrimitiveIndex = 1u;
	auto vertices = firstVertices;
	auto indices = firstIndices;
	vertices.insert(vertices.end(), firstVertices.begin(), firstVertices.end());
	indices.insert(indices.end(), firstIndices.begin(), firstIndices.end());
	asset.vertexPayload = std::move(vertices);
	asset.indexPayload = std::move(indices);
	asset.meshes.push_back(secondMesh);
	asset.nodes.front().meshRefs.push_back(1u);
	ScenePrimitive secondPrimitive{
		.meshIndex = 1u,
		.nodeIndex = 0u,
		.documentPrimitiveIndex = 1u,
		.materialIndex = 0,
	};
	secondPrimitive.runtimeIdentity =
		SceneRuntimeMapping::deriveProjectionIdentity(
			editorObjectId(alternatePrimitiveId(authority)));
	asset.primitives.push_back(std::move(secondPrimitive));
	return asset;
}

struct TestKtx2Header
{
	uint8_t identifier[12]{};
	uint32_t format{0u};
	uint32_t typeSize{1u};
	uint32_t width{0u};
	uint32_t height{0u};
	uint32_t depth{0u};
	uint32_t layerCount{0u};
	uint32_t faceCount{1u};
	uint32_t levelCount{0u};
	uint32_t supercompression{0u};
	uint32_t dfdOffset{0u};
	uint32_t dfdLength{0u};
	uint32_t kvdOffset{0u};
	uint32_t kvdLength{0u};
	uint64_t sgdOffset{0u};
	uint64_t sgdLength{0u};
};

struct TestKtx2Level
{
	uint64_t offset{0u};
	uint64_t length{0u};
	uint64_t uncompressedLength{0u};
};

static_assert(sizeof(TestKtx2Header) == 80u);
static_assert(sizeof(TestKtx2Level) == 24u);

std::vector<uint8_t> makeBc7Ktx2(
	uint32_t width,
	uint32_t height,
	std::span<const uint64_t> mipByteSizes)
{
	static constexpr std::array<uint8_t, 12> identifier{{
		0xABu, 0x4Bu, 0x54u, 0x58u, 0x20u, 0x32u,
		0x30u, 0xBBu, 0x0Du, 0x0Au, 0x1Au, 0x0Au,
	}};
	TestKtx2Header header{};
	std::copy(identifier.begin(), identifier.end(), header.identifier);
	header.format = 145u;
	header.width = width;
	header.height = height;
	header.levelCount = static_cast<uint32_t>(mipByteSizes.size());
	const uint64_t levelTableEnd = sizeof(TestKtx2Header) +
		static_cast<uint64_t>(mipByteSizes.size()) * sizeof(TestKtx2Level);
	require(levelTableEnd <= std::numeric_limits<size_t>::max(),
	        "test KTX2 level table is too large");
	std::vector<TestKtx2Level> levels(mipByteSizes.size());
	uint64_t totalSize = levelTableEnd;
	for (size_t mip = 0u; mip < mipByteSizes.size(); ++mip)
	{
		levels[mip] = {totalSize, mipByteSizes[mip], mipByteSizes[mip]};
		require(mipByteSizes[mip] <= std::numeric_limits<uint64_t>::max() - totalSize,
		        "test KTX2 payload size overflowed");
		totalSize += mipByteSizes[mip];
	}
	require(totalSize <= std::numeric_limits<size_t>::max(),
	        "test KTX2 payload is too large");
	std::vector<uint8_t> bytes(static_cast<size_t>(totalSize), 0xA5u);
	std::memcpy(bytes.data(), &header, sizeof(header));
	if (!levels.empty())
	{
		std::memcpy(bytes.data() + sizeof(header), levels.data(),
		            levels.size() * sizeof(TestKtx2Level));
	}
	return bytes;
}

TestKtx2Level testKtx2Level(const std::vector<uint8_t>& bytes, uint32_t mip)
{
	TestKtx2Level level{};
	const size_t offset = sizeof(TestKtx2Header) +
		static_cast<size_t>(mip) * sizeof(TestKtx2Level);
	require(offset <= bytes.size() && sizeof(level) <= bytes.size() - offset,
	        "test KTX2 level access is out of range");
	std::memcpy(&level, bytes.data() + offset, sizeof(level));
	return level;
}

void setTestKtx2Level(std::vector<uint8_t>& bytes, uint32_t mip, TestKtx2Level level)
{
	const size_t offset = sizeof(TestKtx2Header) +
		static_cast<size_t>(mip) * sizeof(TestKtx2Level);
	require(offset <= bytes.size() && sizeof(level) <= bytes.size() - offset,
	        "test KTX2 level update is out of range");
	std::memcpy(bytes.data() + offset, &level, sizeof(level));
}

void setSingleRawTexture(
	ApplyFullSceneSnapshotRequest& request,
	uint32_t width,
	uint32_t height,
	std::vector<uint8_t> payload)
{
	request.candidateAsset.texturePayload = std::move(payload);
	SceneTexture& texture = request.candidateAsset.textures.at(0u);
	texture.width = width;
	texture.height = height;
	texture.mipLevels = 1u;
	texture.format = rhi::TextureFormat::rgba8Unorm;
	texture.payloadOffset = 0u;
	texture.payloadSize = request.candidateAsset.texturePayload.size();
	texture.isKtx2 = false;
	TextureUploadPlan& plan = request.candidatePlan.textures.at(0u);
	plan.format = texture.format;
	plan.width = width;
	plan.height = height;
	plan.mipLevels = 1u;
	plan.payloadKind = TexturePayloadKind::RawRgba8;
	plan.payloadSrcOffset = 0u;
	plan.payloadByteSize = texture.payloadSize;
}

void setSingleKtxTexture(
	ApplyFullSceneSnapshotRequest& request,
	uint32_t width,
	uint32_t height,
	uint32_t mipLevels,
	std::vector<uint8_t> payload)
{
	request.candidateAsset.texturePayload = std::move(payload);
	SceneTexture& texture = request.candidateAsset.textures.at(0u);
	texture.width = width;
	texture.height = height;
	texture.mipLevels = mipLevels;
	texture.format = rhi::TextureFormat::bc7UnormBlock;
	texture.payloadOffset = 0u;
	texture.payloadSize = request.candidateAsset.texturePayload.size();
	texture.isKtx2 = true;
	TextureUploadPlan& plan = request.candidatePlan.textures.at(0u);
	plan.format = texture.format;
	plan.width = width;
	plan.height = height;
	plan.mipLevels = mipLevels;
	plan.payloadKind = TexturePayloadKind::Ktx2Container;
	plan.payloadSrcOffset = 0u;
	plan.payloadByteSize = texture.payloadSize;
}

SceneUploadPlanHeader uploadHeader(const RendererTargetFence& fence);

SceneUploadPlan uploadPlanFixture(const RendererTargetFence& fence)
{
	SceneUploadPlan plan;
	plan.header = uploadHeader(fence);
	plan.meshes.push_back(MeshUploadPlan{
		.sourceId = primitiveId(fence.authority),
		.meshIndex = 0u,
		.vertexByteSize = 3u * kSceneUploadVertexStride,
		.indexByteSize = 3u * sizeof(uint32_t),
		.vertexCount = 3u,
		.indexCount = 3u,
		.vertexStride = kSceneUploadVertexStride,
		.localBoundsMin = {-1.0f, -1.0f, 0.0f},
		.localBoundsMax = {1.0f, 1.0f, 0.0f},
		.primitiveIndex = 0u,
		.nodeSourceId = nodeId(fence.authority),
	});
	plan.textures.push_back(TextureUploadPlan{
		.sourceId = imageId(fence.authority),
		.textureIndex = 0u,
		.format = rhi::TextureFormat::rgba8Unorm,
		.width = 1u,
		.height = 1u,
		.mipLevels = 1u,
		.payloadByteSize = 4u,
	});
	plan.materials.push_back(MaterialCreatePlan{
		.sourceId = materialId(fence.authority),
		.materialIndex = 0u,
	});
	plan.transforms.worldTransforms.push_back(glm::mat4(1.0f));
	plan.transforms.sourceIds.push_back(nodeId(fence.authority));
	plan.instances.instances.push_back(SceneDrawInstance{
		.sourceId = primitiveId(fence.authority),
		.instanceIndex = 0u,
		.nodeIndex = 0u,
		.meshIndex = 0u,
		.materialIndex = 0u,
		.worldTransform = glm::mat4(1.0f),
		.primitiveIndex = 0u,
		.nodeSourceId = nodeId(fence.authority),
	});
	plan.cullRecords.push_back(InstanceCullRecord{
		.sourceId = primitiveId(fence.authority),
		.instanceIndex = 0u,
		.meshIndex = 0u,
		.nodeSourceId = nodeId(fence.authority),
	});
	plan.descriptorUpdates.push_back(DescriptorUpdatePlan{
		.sourceId = materialId(fence.authority),
		.materialIndex = 0u,
	});
	plan.drawCommands.push_back(DrawCommandBuildPlan{
		.sourceId = primitiveId(fence.authority),
		.instanceIndex = 0u,
		.meshIndex = 0u,
		.materialIndex = 0u,
		.nodeSourceId = nodeId(fence.authority),
	});
	plan.requiredResources = {
		{primitiveId(fence.authority), SceneRequiredResourceKind::Mesh, false,
		 nodeId(fence.authority)},
		{imageId(fence.authority), SceneRequiredResourceKind::Texture, false},
		{materialId(fence.authority), SceneRequiredResourceKind::Material, false},
	};
	return plan;
}

SceneUploadPlanHeader uploadHeader(const RendererTargetFence& fence)
{
	return SceneUploadPlanHeader{
		.authority = fence.authority,
		.documentRevision = fence.appliedDocumentRevision,
		.activeScene = fence.activeScene,
		.sceneActivationEpoch = fence.sceneActivationEpoch,
		.rootExternalStateSerial = fence.rootExternalStateSerial,
		.projectionDigest = fence.projectionDigest,
		.mappingEpoch = fence.mappingEpoch,
	};
}

RendererSceneTransactionInitialState initialState(const RendererTargetFence& fence)
{
	RendererSceneTransactionInitialState result;
	result.publication.fence = fence;
	result.publication.mapping = mappingFor(fence);
	result.publication.activeGenerationDigest = digest(60u);
	result.publication.generation = 1u;
	result.publication.descriptorGeneration = 1u;
	result.activeAsset = sceneAssetFixture(fence.appliedDocumentRevision,
	                                      fence.authority);
	result.activePlan = uploadPlanFixture(fence);
	result.ownerCapability = ownerCapabilityFor(
		result.activeAsset, result.publication.mapping);
	return result;
}

RendererSceneTransactionInitialState multiCameraLightInitialState(
	const RendererTargetFence& fence)
{
	RendererSceneTransactionInitialState result = initialState(fence);
	result.publication.mapping = multiCameraLightMappingFor(fence);
	result.activeAsset = multiCameraLightSceneAssetFixture(
		fence.appliedDocumentRevision, fence.authority);
	result.ownerCapability = ownerCapabilityFor(
		result.activeAsset, result.publication.mapping);
	return result;
}

RendererSceneTransactionInitialState attachmentInitialState(
	const RendererTargetFence& fence)
{
	RendererSceneTransactionInitialState result = initialState(fence);
	result.publication.mapping = attachmentMappingFor(fence);
	SceneNode alternateNode;
	alternateNode.sourceNodeIndex = 1u;
	alternateNode.runtimeIdentity =
		SceneRuntimeMapping::deriveProjectionIdentity(
			editorObjectId(alternateNodeId(fence.authority)));
	result.activeAsset.nodes.push_back(std::move(alternateNode));
	result.activeAsset.rootNodes.push_back(1u);
	result.activeAsset.sceneRootTransformApplyToRoot.push_back(1u);
	result.activePlan.transforms.worldTransforms.push_back(glm::mat4(1.0f));
	result.activePlan.transforms.sourceIds.push_back(
		alternateNodeId(fence.authority));
	result.ownerCapability = ownerCapabilityFor(
		result.activeAsset, result.publication.mapping);
	return result;
}

ApplySceneChangeRequest incrementalRequest(const RendererTargetFence& base,
	                                       uint64_t mutationSerial,
	                                       std::vector<SceneFieldChange> changes = {})
{
	SceneChangeSet input;
	SceneChangeHeader& header = input.header;
	header.domain = SceneChangeDomain::DocumentMutation;
	header.authority = base.authority;
	header.baseDocumentRevision = base.appliedDocumentRevision;
	header.targetDocumentRevision = base.appliedDocumentRevision + 1u;
	header.baseSceneActivationEpoch = base.sceneActivationEpoch;
	header.targetSceneActivationEpoch = base.sceneActivationEpoch;
	header.baseRootExternalStateSerial = base.rootExternalStateSerial;
	header.targetRootExternalStateSerial = base.rootExternalStateSerial;
	header.baseActiveScene = base.activeScene;
	header.targetActiveScene = base.activeScene;
	header.baseDocumentDigest = digest(1u);
	header.targetDocumentDigest = digest(2u);
	header.baseProjectionDigest = base.projectionDigest;
	header.targetProjectionDigest = digest(21u);
	header.firstMutationSerial = mutationSerial;
	header.lastMutationSerial = mutationSerial;
	header.mutationToken = SceneMutationToken{
		base.authority,
		header.targetDocumentRevision,
		mutationSerial,
	};
	header.sourceMutations.push_back(SourceMutationFence{
		header.baseDocumentRevision,
		header.targetDocumentRevision,
		*header.mutationToken,
		header.baseDocumentDigest,
		header.targetDocumentDigest,
	});

	if (changes.empty())
	{
		SceneNodeTransformValue before;
		SceneNodeTransformValue after;
		std::get<SceneTrsValue>(after.value).translation[0] = 3.0;
		changes.push_back(SceneFieldChange{
			.target = nodeId(base.authority),
			.changeClass = SceneChangeClass::Transform,
			.action = SceneChangeAction::Update,
			.field = SceneField::NodeLocalTransform,
			.mutationSerial = mutationSerial,
			.beforeValue = SceneFieldValue{before},
			.afterValue = SceneFieldValue{after},
		});
	}
	input.canonicalChanges = std::move(changes);

	SceneCanonicalizationResult canonical =
		SceneChangeCanonicalizer::canonicalize(std::move(input));
	require(canonical.succeeded(), "incremental fixture failed to canonicalize");

	ApplySceneChangeRequest request;
	request.expectedTarget = base;
	request.changes = std::move(*canonical.changeSet);
	request.canonicalBytes = std::move(canonical.canonicalBytes);
	request.canonicalIdentity = {
		request.changes.header.authority,
		request.changes.header.domain,
		request.changes.header.firstMutationSerial,
		request.changes.header.lastMutationSerial,
		request.changes.header.targetDocumentRevision,
		request.changes.header.targetSceneActivationEpoch,
		request.changes.header.targetRootExternalStateSerial,
	};
	request.targetMapping = mappingFor(targetFence(base, request.changes.header));
	request.ownerCapability = ownerCapabilityFor(
		sceneAssetFixture(request.targetMapping.fence().appliedDocumentRevision,
		                   request.targetMapping.fence().authority),
		request.targetMapping);
	return request;
}

ApplySceneChangeRequest identityTransitionRequest(
	const RendererTargetFence& base,
	SceneChangeDomain domain,
	uint64_t mutationSerial)
{
	SceneChangeSet input;
	SceneChangeHeader& header = input.header;
	header.domain = domain;
	header.authority = base.authority;
	header.baseDocumentRevision = base.appliedDocumentRevision;
	header.targetDocumentRevision = base.appliedDocumentRevision;
	header.baseSceneActivationEpoch = base.sceneActivationEpoch;
	header.targetSceneActivationEpoch = base.sceneActivationEpoch;
	header.baseRootExternalStateSerial = base.rootExternalStateSerial;
	header.targetRootExternalStateSerial = base.rootExternalStateSerial;
	header.baseActiveScene = base.activeScene;
	header.targetActiveScene = base.activeScene;
	header.baseDocumentDigest = digest(80u);
	header.targetDocumentDigest = header.baseDocumentDigest;
	header.baseProjectionDigest = base.projectionDigest;
	header.targetProjectionDigest = digest(static_cast<uint8_t>(81u + mutationSerial));
	header.firstMutationSerial = mutationSerial;
	header.lastMutationSerial = mutationSerial;

	if (domain == SceneChangeDomain::SceneActivation)
	{
		++header.targetSceneActivationEpoch;
		header.targetActiveScene = base.activeScene == sceneId(base.authority)
			                           ? std::optional{alternateSceneId(base.authority)}
			                           : std::optional{sceneId(base.authority)};
	}
	else
	{
		require(domain == SceneChangeDomain::SceneRootState,
		        "identity transition fixture domain is invalid");
		++header.targetRootExternalStateSerial;
		SceneNodeTransformValue before;
		SceneNodeTransformValue after;
		std::get<SceneTrsValue>(after.value).translation[1] = 2.0;
		input.canonicalChanges.push_back(SceneFieldChange{
			.target = *base.activeScene,
			.changeClass = SceneChangeClass::Transform,
			.action = SceneChangeAction::Update,
			.field = SceneField::SceneRootTransform,
			.mutationSerial = mutationSerial,
			.beforeValue = SceneFieldValue{before},
			.afterValue = SceneFieldValue{after},
		});
	}

	SceneCanonicalizationResult canonical =
		SceneChangeCanonicalizer::canonicalize(std::move(input));
	require(canonical.succeeded(), "identity transition fixture failed to canonicalize");

	ApplySceneChangeRequest request;
	request.expectedTarget = base;
	request.changes = std::move(*canonical.changeSet);
	request.canonicalBytes = std::move(canonical.canonicalBytes);
	request.canonicalIdentity = {
		request.changes.header.authority,
		request.changes.header.domain,
		request.changes.header.firstMutationSerial,
		request.changes.header.lastMutationSerial,
		request.changes.header.targetDocumentRevision,
		request.changes.header.targetSceneActivationEpoch,
		request.changes.header.targetRootExternalStateSerial,
	};
	request.targetMapping = mappingFor(targetFence(base, request.changes.header));
	request.ownerCapability = ownerCapabilityFor(
		sceneAssetFixture(request.targetMapping.fence().appliedDocumentRevision,
		                   request.targetMapping.fence().authority),
		request.targetMapping);
	return request;
}

ApplySceneChangeRequest fullSnapshotTransaction(const RendererTargetFence& base)
{
	SceneChangeSet input;
	SceneChangeHeader& header = input.header;
	header.domain = SceneChangeDomain::FullSnapshot;
	header.authority = base.authority;
	header.baseDocumentRevision = base.appliedDocumentRevision;
	header.targetDocumentRevision = base.appliedDocumentRevision + 2u;
	header.baseSceneActivationEpoch = base.sceneActivationEpoch;
	header.targetSceneActivationEpoch = base.sceneActivationEpoch;
	header.baseRootExternalStateSerial = base.rootExternalStateSerial;
	header.targetRootExternalStateSerial = base.rootExternalStateSerial;
	header.baseActiveScene = base.activeScene;
	header.targetActiveScene = base.activeScene;
	header.baseDocumentDigest = digest(2u);
	header.targetDocumentDigest = digest(8u);
	header.baseProjectionDigest = base.projectionDigest;
	header.targetProjectionDigest = digest(30u);

	FullSnapshotReference snapshot;
	snapshot.reason = FullRebuildReason::StructuralTopology;
	snapshot.immutableTarget.authority = base.authority;
	snapshot.immutableTarget.documentRevision = header.targetDocumentRevision;
	snapshot.immutableTarget.sceneActivationEpoch = header.targetSceneActivationEpoch;
	snapshot.immutableTarget.rootExternalStateSerial =
		header.targetRootExternalStateSerial;
	snapshot.immutableTarget.activeScene = header.targetActiveScene;
	snapshot.immutableTarget.documentDigest = header.targetDocumentDigest;
	snapshot.immutableTarget.projectionDigest = header.targetProjectionDigest;
	snapshot.immutableTarget.bytes = {3u, 1u, 4u, 1u, 5u};
	snapshot.immutableTarget.contentDigest =
		SceneChangeCanonicalizer::computeSha256(snapshot.immutableTarget.bytes);
	snapshot.targetSnapshotDigest = snapshot.immutableTarget.contentDigest;
	snapshot.stableIdRemapComplete = true;
	input.fullSnapshot = std::move(snapshot);

	SceneCanonicalizationResult canonical =
		SceneChangeCanonicalizer::canonicalize(std::move(input));
	require(canonical.succeeded(), "snapshot fixture failed to canonicalize");

	ApplySceneChangeRequest request;
	request.expectedTarget = base;
	request.changes = std::move(*canonical.changeSet);
	request.canonicalBytes = std::move(canonical.canonicalBytes);
	request.canonicalIdentity = {
		request.changes.header.authority,
		request.changes.header.domain,
		0u,
		0u,
		request.changes.header.targetDocumentRevision,
		request.changes.header.targetSceneActivationEpoch,
		request.changes.header.targetRootExternalStateSerial,
	};
	request.targetMapping = mappingFor(targetFence(base, request.changes.header));
	request.ownerCapability = ownerCapabilityFor(
		sceneAssetFixture(request.targetMapping.fence().appliedDocumentRevision,
		                   request.targetMapping.fence().authority),
		request.targetMapping);
	return request;
}

ApplyFullSceneSnapshotRequest fullSnapshotRequest(const RendererTargetFence& base)
{
	ApplyFullSceneSnapshotRequest request;
	request.transaction = fullSnapshotTransaction(base);
	const RendererTargetFence& target = request.transaction.targetMapping.fence();
	request.candidateAsset = sceneAssetFixture(target.appliedDocumentRevision);
	request.candidatePlan = uploadPlanFixture(target);
	request.ownerCapability = ownerCapabilityFor(
		request.candidateAsset, request.transaction.targetMapping);
	request.transaction.ownerCapability = request.ownerCapability;
	return request;
}

ApplyFullSceneSnapshotRequest multiCameraLightFullSnapshotRequest(
	const RendererTargetFence& base,
	const RendererSceneMappingSnapshot* activeMapping = nullptr)
{
	ApplyFullSceneSnapshotRequest request = fullSnapshotRequest(base);
	const RendererTargetFence target = request.transaction.targetMapping.fence();
	request.transaction.targetMapping = multiCameraLightMappingFor(
		target, activeMapping);
	request.candidateAsset = multiCameraLightSceneAssetFixture(
		target.appliedDocumentRevision, target.authority);
	request.ownerCapability = ownerCapabilityFor(
		request.candidateAsset, request.transaction.targetMapping);
	request.transaction.ownerCapability = request.ownerCapability;
	return request;
}

void requireZeroContinuousWork(const SceneApplyResult& result,
	                           const char* message)
{
	require(result.disposition == SceneApplyDisposition::AcceptedIncremental &&
	        result.error == SceneApplyError::None && result.resultingFence &&
	        result.counterDelta.geometryUploadCalls == 0u &&
	        result.counterDelta.geometryBytesUploaded == 0u &&
	        result.counterDelta.geometryRangesUploaded == 0u &&
	        result.counterDelta.textureUploadCalls == 0u &&
	        result.counterDelta.textureBytesUploaded == 0u &&
	        result.counterDelta.textureSubresourcesUploaded == 0u &&
	        result.counterDelta.deviceIdleCalls == 0u &&
	        std::all_of(result.counterDelta.deviceIdleCallsByReason.begin(),
	                    result.counterDelta.deviceIdleCallsByReason.end(),
	                    [](uint64_t count) { return count == 0u; }) &&
	        result.counterDelta.fullSnapshotCount == 0u &&
	        result.counterDelta.partialPublicationCount == 0u,
	        message);
}

SceneRuntimeMapping projectionMappingFor(const RendererTargetFence& fence,
	                                     bool includeMaterial = true)
{
	const auto entry = [&](StableSceneId id, uint32_t index, uint64_t identity)
	{
		return SceneRuntimeMappingEntry{
			.documentId = editorObjectId(id),
			.sceneAssetIndex = index,
			.projectionIdentity = identity,
			.runtimeIndex = identity,
		};
	};
	std::vector<SceneRuntimeMappingEntry> entries{
		entry(nodeId(fence.authority), 0u, 100u),
		entry(meshId(fence.authority), 0u, 101u),
		entry(imageId(fence.authority), 0u, 102u),
		entry(primitiveId(fence.authority), 0u, 104u),
	};
	if (includeMaterial)
	{
		entries.push_back(entry(materialId(fence.authority), 0u, 103u));
	}
	std::string error;
	const std::optional<SceneRuntimeMapping> mapping = SceneRuntimeMapping::create(
		SceneRuntimeMappingFence{
			.authority = fence.authority,
			.appliedDocumentRevision = fence.appliedDocumentRevision,
			.activeScene = fence.activeScene,
			.sceneActivationEpoch = fence.sceneActivationEpoch,
			.rootExternalStateSerial = fence.rootExternalStateSerial,
			.projectionDigest = fence.projectionDigest,
			.mappingEpoch = fence.mappingEpoch,
		},
		std::move(entries), &error);
	require(mapping.has_value(), error.c_str());
	return *mapping;
}

SceneRuntimeMapping semanticTextureProjectionMappingFor(
	const RendererTargetFence& fence)
{
	const auto projected = [&](StableSceneId id, uint32_t index)
	{
		const uint64_t identity =
			SceneRuntimeMapping::deriveProjectionIdentity(editorObjectId(id));
		return SceneRuntimeMappingEntry{
			.documentId = editorObjectId(id),
			.sceneAssetIndex = index,
			.projectionIdentity = identity,
			.runtimeIndex = identity,
		};
	};
	const auto unprojected = [&](StableSceneId id)
	{
		return SceneRuntimeMappingEntry{
			.documentId = editorObjectId(id),
		};
	};
	std::vector<SceneRuntimeMappingEntry> entries{
		projected(sceneId(fence.authority), 0u),
		projected(nodeId(fence.authority), 0u),
		projected(primitiveId(fence.authority), 0u),
		projected(meshId(fence.authority), 0u),
		projected(materialId(fence.authority), 0u),
		projected(cameraId(fence.authority), 0u),
		projected(lightId(fence.authority), 0u),
		projected(imageId(fence.authority), 0u),
		projected(alternateImageId(fence.authority), 1u),
		unprojected(textureId(fence.authority)),
		unprojected(alternateTextureId(fence.authority)),
		unprojected(samplerId(fence.authority)),
		unprojected(alternateSamplerId(fence.authority)),
	};
	std::string error;
	const std::optional<SceneRuntimeMapping> mapping = SceneRuntimeMapping::create(
		SceneRuntimeMappingFence{
			.authority = fence.authority,
			.appliedDocumentRevision = fence.appliedDocumentRevision,
			.activeScene = fence.activeScene,
			.sceneActivationEpoch = fence.sceneActivationEpoch,
			.rootExternalStateSerial = fence.rootExternalStateSerial,
			.projectionDigest = fence.projectionDigest,
			.mappingEpoch = fence.mappingEpoch,
		},
		std::move(entries), &error);
	require(mapping.has_value(), error.c_str());
	return *mapping;
}

SceneProjectionResult semanticTextureProjectionFixture(
	const RendererTargetFence& fence,
	const SceneMaterial& material)
{
	SceneProjectionResult result;
	result.asset = semanticTextureSceneAssetFixture(
		fence.appliedDocumentRevision, fence.authority);
	result.asset.materials.front() = material;
	result.mapping = semanticTextureProjectionMappingFor(fence);
	result.ownerCapability = ownerCapabilityFor(result.asset, result.mapping);
	result.semanticHash = "semantic-texture-projection";
	result.documentRevision = fence.appliedDocumentRevision;
	result.sceneActivationEpoch = fence.sceneActivationEpoch;
	result.mappingEpoch = fence.mappingEpoch;
	result.activeScene = fence.activeScene;
	result.documentDigest = digest(static_cast<uint8_t>(
		90u + fence.appliedDocumentRevision % 40u));
	result.projectionDigest = fence.projectionDigest;
	result.textureBindings = {
		SceneTextureProjectionBinding{
			.textureId = textureId(fence.authority),
			.sourceImageId = imageId(fence.authority),
			.sourceImageIndex = 0u,
			.sceneAssetIndex = 0u,
			.colorSceneAssetIndex = 1u,
			.linearSceneAssetIndex = 0u,
			.samplerIndex = 0,
			.samplerId = samplerId(fence.authority),
		},
		SceneTextureProjectionBinding{
			.textureId = alternateTextureId(fence.authority),
			.sourceImageId = imageId(fence.authority),
			.sourceImageIndex = 0u,
			.sceneAssetIndex = 0u,
			.colorSceneAssetIndex = 1u,
			.linearSceneAssetIndex = 0u,
			.samplerIndex = 1,
			.samplerId = alternateSamplerId(fence.authority),
		},
	};
	result.samplerBindings = {
		SceneSamplerProjectionBinding{samplerId(fence.authority), 1u},
		SceneSamplerProjectionBinding{alternateSamplerId(fence.authority), 1u},
	};
	result.immutableSnapshot.authority = fence.authority;
	result.immutableSnapshot.documentRevision = fence.appliedDocumentRevision;
	result.immutableSnapshot.sceneActivationEpoch = fence.sceneActivationEpoch;
	result.immutableSnapshot.rootExternalStateSerial =
		fence.rootExternalStateSerial;
	result.immutableSnapshot.activeScene = fence.activeScene;
	result.immutableSnapshot.documentDigest = result.documentDigest;
	result.immutableSnapshot.projectionDigest = result.projectionDigest;
	result.immutableSnapshot.bytes = {
		'S', 'E', 'M', 'A', 'N', 'T', 'I', 'C',
		static_cast<uint8_t>(fence.appliedDocumentRevision & 0xffu),
	};
	result.immutableSnapshot.contentDigest =
		SceneChangeCanonicalizer::computeSha256(
			result.immutableSnapshot.bytes);
	return result;
}

SceneProjectionResult duplicatedCameraProjectionFixture(
	const RendererTargetFence& fence)
{
	SceneProjectionResult result =
		semanticTextureProjectionFixture(fence, SceneMaterial{});
	SceneCamera duplicate = result.asset.cameras.front();
	duplicate.name = "duplicated camera";
	duplicate.sourceCameraIndex = 1u;
	duplicate.runtimeIdentity = SceneRuntimeMapping::deriveProjectionIdentity(
		editorObjectId(alternateCameraId(fence.authority)));
	result.asset.cameras.push_back(std::move(duplicate));

	std::vector<SceneRuntimeMappingEntry> entries(
		result.mapping.entries().begin(), result.mapping.entries().end());
	const StableSceneId duplicateId = alternateCameraId(fence.authority);
	const uint64_t identity =
		SceneRuntimeMapping::deriveProjectionIdentity(editorObjectId(duplicateId));
	entries.push_back(SceneRuntimeMappingEntry{
		.documentId = editorObjectId(duplicateId),
		.sceneAssetIndex = 1u,
		.projectionIdentity = identity,
		.runtimeIndex = identity,
	});
	std::string error;
	std::optional<SceneRuntimeMapping> mapping = SceneRuntimeMapping::create(
		result.mapping.fence(), std::move(entries),
		std::vector<SceneRuntimeMappingAlias>(result.mapping.aliases().begin(),
		                                      result.mapping.aliases().end()),
		&error);
	require(mapping.has_value(), error.c_str());
	result.mapping = std::move(*mapping);
	result.ownerCapability = ownerCapabilityFor(result.asset, result.mapping);
	result.semanticHash = "duplicated-camera-projection";
	return result;
}

SceneProjectionResult multiCameraLightProjectionFixture(
	const RendererTargetFence& fence)
{
	SceneProjectionResult result =
		semanticTextureProjectionFixture(fence, SceneMaterial{});
	SceneCamera secondCamera;
	secondCamera.name = "alternate camera";
	secondCamera.nodeIndex = 0;
	secondCamera.verticalFieldOfViewRadians = 0.9f;
	secondCamera.sourceCameraIndex = 1u;
	secondCamera.runtimeIdentity = SceneRuntimeMapping::deriveProjectionIdentity(
		editorObjectId(alternateCameraId(fence.authority)));
	result.asset.cameras.push_back(std::move(secondCamera));
	SceneLight secondLight;
	secondLight.name = "alternate light";
	secondLight.nodeIndex = 0;
	secondLight.intensity = 3.0f;
	secondLight.sourceLightIndex = 1u;
	secondLight.runtimeIdentity = SceneRuntimeMapping::deriveProjectionIdentity(
		editorObjectId(alternateLightId(fence.authority)));
	result.asset.lights.push_back(std::move(secondLight));

	std::vector<SceneRuntimeMappingEntry> entries(
		result.mapping.entries().begin(), result.mapping.entries().end());
	const auto projected = [](StableSceneId id, uint32_t coordinate)
	{
		const uint64_t identity =
			SceneRuntimeMapping::deriveProjectionIdentity(editorObjectId(id));
		return SceneRuntimeMappingEntry{
			.documentId = editorObjectId(id),
			.sceneAssetIndex = coordinate,
			.projectionIdentity = identity,
			.runtimeIndex = identity,
		};
	};
	entries.push_back(projected(alternateCameraId(fence.authority), 1u));
	entries.push_back(projected(alternateLightId(fence.authority), 1u));
	std::string error;
	std::optional<SceneRuntimeMapping> mapping = SceneRuntimeMapping::create(
		result.mapping.fence(), std::move(entries),
		std::vector<SceneRuntimeMappingAlias>(result.mapping.aliases().begin(),
		                                      result.mapping.aliases().end()),
		&error);
	require(mapping.has_value(), error.c_str());
	result.mapping = std::move(*mapping);
	result.ownerCapability = ownerCapabilityFor(result.asset, result.mapping);
	return result;
}

SceneProjectionResult withCoherentlySwappedProjectionCoordinates(
	SceneProjectionResult projection,
	StableSceneId firstId,
	StableSceneId secondId)
{
	std::vector<SceneRuntimeMappingEntry> entries(
		projection.mapping.entries().begin(), projection.mapping.entries().end());
	const EditorObjectId firstDocumentId = editorObjectId(firstId);
	const EditorObjectId secondDocumentId = editorObjectId(secondId);
	auto first = std::find_if(
		entries.begin(), entries.end(),
		[&](const SceneRuntimeMappingEntry& entry)
		{
			return entry.documentId == firstDocumentId;
		});
	auto second = std::find_if(
		entries.begin(), entries.end(),
		[&](const SceneRuntimeMappingEntry& entry)
		{
			return entry.documentId == secondDocumentId;
		});
	require(first != entries.end() && second != entries.end(),
	        "adapter coherent-swap fixture lost a projection source");
	std::swap(first->sceneAssetIndex, second->sceneAssetIndex);
	std::string error;
	std::optional<SceneRuntimeMapping> mapping = SceneRuntimeMapping::create(
		projection.mapping.fence(), std::move(entries),
		std::vector<SceneRuntimeMappingAlias>(projection.mapping.aliases().begin(),
		                                      projection.mapping.aliases().end()),
		&error);
	require(mapping.has_value(), error.c_str());
	projection.mapping = std::move(*mapping);
	return projection;
}

void setMaterialTextureCoordinate(
	SceneMaterial& material,
	SceneTextureSemantic semantic,
	int32_t sceneAssetIndex)
{
	switch (semantic)
	{
	case SceneTextureSemantic::BaseColor:
		material.baseColorTexture = sceneAssetIndex;
		break;
	case SceneTextureSemantic::MetallicRoughness:
		material.metallicRoughnessTexture = sceneAssetIndex;
		break;
	case SceneTextureSemantic::Normal:
		material.normalTexture = sceneAssetIndex;
		break;
	case SceneTextureSemantic::Occlusion:
		material.occlusionTexture = sceneAssetIndex;
		break;
	case SceneTextureSemantic::Emissive:
		material.emissiveTexture = sceneAssetIndex;
		break;
	}
}

SceneCanonicalizationResult semanticTextureBindingCanonical(
	const SceneProjectionResult& base,
	const SceneProjectionResult& target,
	uint64_t mutationSerial,
	const SceneTextureBindingValue& before,
	const SceneTextureBindingValue& after)
{
	SceneChangeSet input;
	SceneChangeHeader& header = input.header;
	header.domain = SceneChangeDomain::DocumentMutation;
	header.authority = base.mapping.authority();
	header.baseDocumentRevision = base.documentRevision;
	header.targetDocumentRevision = target.documentRevision;
	header.baseSceneActivationEpoch = base.sceneActivationEpoch;
	header.targetSceneActivationEpoch = target.sceneActivationEpoch;
	header.baseRootExternalStateSerial =
		base.mapping.rootExternalStateSerial();
	header.targetRootExternalStateSerial =
		target.mapping.rootExternalStateSerial();
	header.baseActiveScene = base.activeScene;
	header.targetActiveScene = target.activeScene;
	header.baseDocumentDigest = base.documentDigest;
	header.targetDocumentDigest = target.documentDigest;
	header.baseProjectionDigest = base.projectionDigest;
	header.targetProjectionDigest = target.projectionDigest;
	header.firstMutationSerial = mutationSerial;
	header.lastMutationSerial = mutationSerial;
	header.mutationToken = SceneMutationToken{
		header.authority,
		header.targetDocumentRevision,
		mutationSerial,
	};
	header.sourceMutations.push_back(SourceMutationFence{
		header.baseDocumentRevision,
		header.targetDocumentRevision,
		*header.mutationToken,
		header.baseDocumentDigest,
		header.targetDocumentDigest,
	});
	input.canonicalChanges.push_back(SceneFieldChange{
		.target = materialId(header.authority),
		.changeClass = SceneChangeClass::MaterialBinding,
		.action = SceneChangeAction::Update,
		.field = SceneField::MaterialTextureBinding,
		.mutationSerial = mutationSerial,
		.beforeValue = SceneFieldValue{before},
		.afterValue = SceneFieldValue{after},
	});
	return SceneChangeCanonicalizer::canonicalize(std::move(input));
}

SceneRuntimeMapping multiPrimitiveProjectionMappingFor(
	const RendererTargetFence& fence)
{
	const auto entry = [&](StableSceneId id, uint32_t index, uint64_t identity)
	{
		return SceneRuntimeMappingEntry{
			.documentId = editorObjectId(id),
			.sceneAssetIndex = index,
			.projectionIdentity = identity,
			.runtimeIndex = identity,
		};
	};
	std::vector<SceneRuntimeMappingEntry> entries{
		entry(nodeId(fence.authority), 0u, 200u),
		entry(primitiveId(fence.authority), 0u, 201u),
		entry(alternatePrimitiveId(fence.authority), 1u, 202u),
		entry(imageId(fence.authority), 0u, 203u),
		entry(materialId(fence.authority), 0u, 204u),
	};
	std::string error;
	const std::optional<SceneRuntimeMapping> mapping = SceneRuntimeMapping::create(
		SceneRuntimeMappingFence{
			.authority = fence.authority,
			.appliedDocumentRevision = fence.appliedDocumentRevision,
			.activeScene = fence.activeScene,
			.sceneActivationEpoch = fence.sceneActivationEpoch,
			.rootExternalStateSerial = fence.rootExternalStateSerial,
			.projectionDigest = fence.projectionDigest,
			.mappingEpoch = fence.mappingEpoch,
		},
		std::move(entries), &error);
	require(mapping.has_value(), error.c_str());
	return *mapping;
}

SceneUploadPlan unboundUploadPlanFixture(const RendererTargetFence& fence)
{
	SceneUploadPlan plan = uploadPlanFixture(fence);
	plan.header = {};
	plan.meshes[0].sourceId = {};
	plan.meshes[0].nodeSourceId = {};
	plan.textures[0].sourceId = {};
	plan.materials[0].sourceId = {};
	plan.transforms.sourceIds.clear();
	plan.instances.instances[0].sourceId = {};
	plan.instances.instances[0].nodeSourceId = {};
	plan.cullRecords[0].sourceId = {};
	plan.cullRecords[0].nodeSourceId = {};
	plan.descriptorUpdates[0].sourceId = {};
	plan.drawCommands[0].sourceId = {};
	plan.drawCommands[0].nodeSourceId = {};
	plan.requiredResources.clear();
	return plan;
}

void testSemanticTextureMappingContract()
{
	const RendererTargetFence fence = baseFence();
	const RendererSceneMappingSnapshot mapping =
		semanticTextureMappingFor(fence);
	const RendererSceneMappingValidation validation = mapping.validate();
	const RendererSceneMappingEntry* primary =
		mapping.find(textureId(fence.authority));
	const RendererSceneMappingEntry* sharedSamplerTexture =
		mapping.find(alternateTextureId(fence.authority));
	const RendererSceneMappingEntry* firstSampler =
		mapping.find(samplerId(fence.authority));
	const RendererSceneMappingEntry* secondSampler =
		mapping.find(alternateSamplerId(fence.authority));
	require(validation.valid() && primary != nullptr &&
	        sharedSamplerTexture != nullptr && firstSampler != nullptr &&
	        secondSampler != nullptr,
	        "semantic texture mapping rejected shared-image sampler aliases");
	require(primary->stableId.slot !=
	            primary->projection.sceneAssetIndexFor(
	                SceneTextureSemantic::BaseColor) &&
	        primary->projection.sceneAssetIndexFor(
	            SceneTextureSemantic::BaseColor) == 1u &&
	        primary->projection.sceneAssetIndexFor(
	            SceneTextureSemantic::Emissive) == 1u &&
	        primary->projection.sceneAssetIndexFor(
	            SceneTextureSemantic::MetallicRoughness) == 0u &&
	        primary->projection.sceneAssetIndexFor(
	            SceneTextureSemantic::Normal) == 0u &&
	        primary->projection.sceneAssetIndexFor(
	            SceneTextureSemantic::Occlusion) == 0u,
	        "texture semantic coordinates collapsed to the glTF texture slot");
	require(primary->rendererHandle != sharedSamplerTexture->rendererHandle &&
	        primary->projection.sceneAssetIndexFor(
	            SceneTextureSemantic::BaseColor) ==
	            sharedSamplerTexture->projection.sceneAssetIndexFor(
	                SceneTextureSemantic::BaseColor) &&
	        firstSampler->rendererHandle != secondSampler->rendererHandle &&
	        mapping.reverseLookup(primary->rendererHandle) ==
	            std::optional{primary->stableId} &&
	        mapping.reverseLookup(sharedSamplerTexture->rendererHandle) ==
	            std::optional{sharedSamplerTexture->stableId} &&
	        mapping.reverseLookup(primary->projection) ==
	            std::optional{primary->stableId} &&
	        mapping.reverseLookup(sharedSamplerTexture->projection) ==
	            std::optional{sharedSamplerTexture->stableId},
	        "shared-image texture/sampler handles lost identity or reverse lookup");

	RendererProjectionCoordinate fallback = primary->projection;
	fallback.colorSceneAssetIndex = SceneRuntimeMapping::kInvalidIndex;
	fallback.linearSceneAssetIndex = SceneRuntimeMapping::kInvalidIndex;
	require(fallback.sceneAssetIndexFor(SceneTextureSemantic::BaseColor) ==
	            fallback.sceneAssetIndex &&
	        fallback.sceneAssetIndexFor(SceneTextureSemantic::Emissive) ==
	            fallback.sceneAssetIndex &&
	        fallback.sceneAssetIndexFor(SceneTextureSemantic::Normal) ==
	            fallback.sceneAssetIndex,
	        "absent semantic coordinates did not use the canonical fallback");
	RendererProjectionCoordinate invalidNonTexture =
		mapping.find(imageId(fence.authority))->projection;
	invalidNonTexture.colorSceneAssetIndex = 1u;
	require(!invalidNonTexture.valid(),
	        "non-texture mapping accepted semantic image coordinates");

	RendererTargetFence nextFence = fence;
	++nextFence.appliedDocumentRevision;
	++nextFence.mappingEpoch;
	nextFence.projectionDigest = digest(21u);
	const RendererSceneMappingSnapshot next =
		semanticTextureMappingFor(nextFence);
	const RendererSceneMappingEntry* nextPrimary =
		next.find(primary->stableId);
	const RendererSceneMappingEntry* nextShared =
		next.find(sharedSamplerTexture->stableId);
	require(next.validate().valid() && nextPrimary != nullptr &&
	        nextShared != nullptr &&
	        nextPrimary->rendererHandle == primary->rendererHandle &&
	        nextShared->rendererHandle == sharedSamplerTexture->rendererHandle &&
	        nextPrimary->projection.identityDigest ==
	            primary->projection.identityDigest &&
	        nextShared->projection.identityDigest ==
	            sharedSamplerTexture->projection.identityDigest,
	        "semantic coordinates changed stable handles or identity digests");

	std::vector<RendererSceneMappingEntry> duplicateEntries(
		mapping.entries().begin(), mapping.entries().end());
	auto duplicatePrimary = std::find_if(
		duplicateEntries.begin(), duplicateEntries.end(),
		[&](const RendererSceneMappingEntry& entry)
		{
			return entry.stableId == primary->stableId;
		});
	auto duplicateShared = std::find_if(
		duplicateEntries.begin(), duplicateEntries.end(),
		[&](const RendererSceneMappingEntry& entry)
		{
			return entry.stableId == sharedSamplerTexture->stableId;
		});
	require(duplicatePrimary != duplicateEntries.end() &&
	        duplicateShared != duplicateEntries.end(),
	        "duplicate semantic mapping fixture lost texture entries");
	duplicateShared->projection = duplicatePrimary->projection;
	const RendererSceneMappingValidation duplicateValidation =
		RendererSceneMappingSnapshot{fence, std::move(duplicateEntries)}.validate();
	require(duplicateValidation.error ==
	            RendererSceneMappingError::DuplicateProjectionCoordinate,
	        "semantic mapping bypassed duplicate-coordinate validation");
}

void testSemanticTextureCoordinatesAcrossAdapterTransactions()
{
	auto device = std::make_unique<TransactionFakeDevice>();
	TransactionFakeDevice* fake = device.get();
	RendererFacade facade(std::move(device));
	editor_ui::EditorRendererAdapter adapter(rhi::BackendType::vulkan);
	adapter.bindRendererIdentity({101u, 7u}, {202u, 9u});
	RendererTargetFence projectionFence = baseFence();
	SceneMaterial material;
	SceneProjectionResult current =
		semanticTextureProjectionFixture(projectionFence, material);
	std::string enqueueError;
	require(adapter.enqueue(
	            editor_ui::EditorController::ProjectionPublication{
	                .projection = &current,
	                .refresh = editor_ui::ProjectionRefresh::fullScene,
	            },
		            &enqueueError),
		        enqueueError.c_str());
	editor_ui::EditorRendererAdapter::FlushResult bootstrap = adapter.flush(facade);
	require(bootstrap.accepted && bootstrap.fullSnapshot &&
	        bootstrap.projection.has_value() && bootstrap.resultingFence &&
	        facade.activeSceneState().fence == bootstrap.resultingFence &&
	        fake->capture.views.size() == 2u,
	        "adapter bootstrap did not return the renderer-accepted fence");
	require(adapter.activeGenerationDigest() != SceneDigest{} &&
	        adapter.activeGenerationDigest() ==
	            facade.activeSceneState().activeGenerationDigest,
	        "adapter bootstrap digest differs from the renderer generation");
	current = std::move(*bootstrap.projection);

	const rhi::TextureViewHandle linearView = fake->capture.views[0].first;
	const rhi::TextureViewHandle colorView = fake->capture.views[1].first;
	const auto descriptorViewAt = [&](uint32_t arrayElement)
		-> rhi::TextureViewHandle
	{
		const auto found = std::find_if(
			fake->capture.descriptorWrites.begin(),
			fake->capture.descriptorWrites.end(),
			[&](const TransactionFakeDescriptorWrite& captured)
			{
				return captured.write.arrayElement == arrayElement;
			});
		return found != fake->capture.descriptorWrites.end()
			? found->write.textureView
			: rhi::TextureViewHandle{};
	};
	require(descriptorViewAt(0u) == linearView &&
	        descriptorViewAt(1u) == colorView &&
	        descriptorViewAt(2u) == colorView &&
	        descriptorViewAt(3u) == colorView,
	        "shared-image texture handles did not receive deterministic semantic views");

	const uint64_t createdAfterBootstrap = fake->createdCount;
	const uint64_t destroyedAfterBootstrap = fake->destroyedCount;
	const uint64_t descriptorWritesAfterBootstrap = fake->descriptorWriteCount;
	const size_t submissionsAfterBootstrap = fake->graphics.submissions.size();
	std::array<std::optional<StableSceneId>, 5> boundTextures{};
	std::array<std::optional<StableSceneId>, 5> boundSamplers{};
	std::array<int32_t, 5> expectedCoordinates{{-1, -1, -1, -1, -1}};
	uint64_t mutationSerial = 800u;

	const auto applyBinding = [&](SceneTextureSemantic semantic,
	                              std::optional<StableSceneId> texture,
	                              std::optional<StableSceneId> sampler)
	{
		const size_t semanticIndex = static_cast<size_t>(semantic);
		SceneTextureBindingValue before;
		before.semantic = semantic;
		before.texture = boundTextures[semanticIndex];
		before.sampler = boundSamplers[semanticIndex];
		SceneTextureBindingValue after = before;
		after.texture = texture;
		after.sampler = sampler;

		RendererTargetFence targetFence = projectionFence;
		++targetFence.appliedDocumentRevision;
		++targetFence.mappingEpoch;
		targetFence.projectionDigest = digest(static_cast<uint8_t>(
			30u + targetFence.appliedDocumentRevision));
		SceneMaterial targetMaterial = current.asset.materials.front();
		const bool colorSemantic =
			semantic == SceneTextureSemantic::BaseColor ||
			semantic == SceneTextureSemantic::Emissive;
		const int32_t selectedCoordinate = texture
			? (colorSemantic ? 1 : 0)
			: -1;
		setMaterialTextureCoordinate(
			targetMaterial, semantic, selectedCoordinate);
		SceneProjectionResult target =
			semanticTextureProjectionFixture(targetFence, targetMaterial);
		SceneCanonicalizationResult canonical = semanticTextureBindingCanonical(
			current, target, mutationSerial++, before, after);
		require(canonical.succeeded(),
		        "semantic texture edit failed to canonicalize");
		require(adapter.enqueue(
		            editor_ui::EditorController::ProjectionPublication{
		                .projection = &target,
		                .refresh = editor_ui::ProjectionRefresh::properties,
		                .canonical = &canonical,
		            },
		            &enqueueError),
		        enqueueError.c_str());
		editor_ui::EditorRendererAdapter::FlushResult published = adapter.flush(facade);
		require(published.accepted && !published.fullSnapshot &&
		        published.projection.has_value() && published.resultingFence &&
		        facade.activeSceneState().fence == published.resultingFence,
		        published.message.c_str());
		require(adapter.activeGenerationDigest() != SceneDigest{} &&
		        adapter.activeGenerationDigest() ==
		            facade.activeSceneState().activeGenerationDigest,
		        "adapter incremental digest differs from the renderer generation");
		current = std::move(*published.projection);
		projectionFence = targetFence;
		boundTextures[semanticIndex] = texture;
		boundSamplers[semanticIndex] = sampler;
		expectedCoordinates[semanticIndex] = selectedCoordinate;

		const SceneTransactionConcreteStateSnapshot concrete =
			facade.sceneTransactionConcreteState();
		require(concrete.firstMaterial.has_value(),
		        "semantic texture edit lost its material record");
		const SceneMaterial& active = *concrete.firstMaterial;
		require(active.baseColorTexture == expectedCoordinates[0] &&
		        active.metallicRoughnessTexture == expectedCoordinates[1] &&
		        active.normalTexture == expectedCoordinates[2] &&
		        active.occlusionTexture == expectedCoordinates[3] &&
		        active.emissiveTexture == expectedCoordinates[4],
		        "renderer transaction selected the wrong semantic texture coordinate");
	};

	applyBinding(SceneTextureSemantic::BaseColor,
	             textureId(projectionFence.authority),
	             samplerId(projectionFence.authority));
	applyBinding(SceneTextureSemantic::Emissive,
	             alternateTextureId(projectionFence.authority),
	             alternateSamplerId(projectionFence.authority));
	applyBinding(SceneTextureSemantic::MetallicRoughness,
	             textureId(projectionFence.authority),
	             samplerId(projectionFence.authority));
	applyBinding(SceneTextureSemantic::Normal,
	             alternateTextureId(projectionFence.authority),
	             alternateSamplerId(projectionFence.authority));
	applyBinding(SceneTextureSemantic::Occlusion,
	             textureId(projectionFence.authority),
	             samplerId(projectionFence.authority));
	applyBinding(SceneTextureSemantic::BaseColor, std::nullopt, std::nullopt);
	applyBinding(SceneTextureSemantic::BaseColor,
	             textureId(projectionFence.authority),
	             samplerId(projectionFence.authority));
	require(fake->createdCount == createdAfterBootstrap &&
	        fake->destroyedCount == destroyedAfterBootstrap &&
	        fake->descriptorWriteCount == descriptorWritesAfterBootstrap &&
	        fake->graphics.submissions.size() == submissionsAfterBootstrap,
	        "semantic texture edits performed resource or submission work");

	RendererTargetFence invalidFence = projectionFence;
	++invalidFence.appliedDocumentRevision;
	++invalidFence.mappingEpoch;
	invalidFence.projectionDigest = digest(79u);
	SceneProjectionResult invalidProjection =
		semanticTextureProjectionFixture(
			invalidFence, current.asset.materials.front());
	invalidProjection.textureBindings.front().colorSceneAssetIndex = 99u;
	SceneTextureBindingValue invalidBefore;
	invalidBefore.semantic = SceneTextureSemantic::BaseColor;
	invalidBefore.texture = textureId(projectionFence.authority);
	invalidBefore.sampler = samplerId(projectionFence.authority);
	SceneTextureBindingValue invalidAfter = invalidBefore;
	invalidAfter.texture = alternateTextureId(projectionFence.authority);
	invalidAfter.sampler = alternateSamplerId(projectionFence.authority);
	SceneCanonicalizationResult invalidCanonical = semanticTextureBindingCanonical(
		current, invalidProjection, mutationSerial, invalidBefore, invalidAfter);
	require(invalidCanonical.succeeded() &&
	        adapter.enqueue(
	            editor_ui::EditorController::ProjectionPublication{
	                .projection = &invalidProjection,
	                .refresh = editor_ui::ProjectionRefresh::properties,
	                .canonical = &invalidCanonical,
	            },
	            &enqueueError),
	        "invalid semantic coordinate fixture could not reach the adapter");
	const RendererSceneStateSnapshot beforeInvalid = facade.activeSceneState();
	const editor_ui::EditorRendererAdapter::FlushResult invalidResult =
		adapter.flush(facade);
	require(!invalidResult.accepted &&
	        facade.activeSceneState() == beforeInvalid &&
	        adapter.activeGenerationDigest() ==
	            beforeInvalid.activeGenerationDigest &&
	        fake->createdCount == createdAfterBootstrap &&
	        fake->descriptorWriteCount == descriptorWritesAfterBootstrap &&
	        fake->graphics.submissions.size() == submissionsAfterBootstrap,
	        "explicit invalid semantic coordinate mutated the active generation");
	adapter.reset(facade);
	require(adapter.activeGenerationDigest() == SceneDigest{} &&
	        adapter.activeGenerationDigest() ==
	            facade.activeSceneState().activeGenerationDigest,
	        "adapter reset retained an active generation digest");
}

void testAdapterRetiresRemovedHandleAcrossDuplicateUndoRedo()
{
	auto device = std::make_unique<TransactionFakeDevice>();
	RendererFacade facade(std::move(device));
	editor_ui::EditorRendererAdapter adapter(rhi::BackendType::vulkan);
	adapter.bindRendererIdentity({101u, 7u}, {202u, 9u});
	RendererTargetFence projectionFence = baseFence();
	std::string enqueueError;

	const auto flushFullSnapshot = [&](SceneProjectionResult& projection,
	                                  uint64_t expectedMappingEpoch,
	                                  const char* failureMessage)
	{
		require(adapter.enqueue(
		            editor_ui::EditorController::ProjectionPublication{
		                .projection = &projection,
		                .refresh = editor_ui::ProjectionRefresh::fullScene,
		            },
		            &enqueueError),
		        enqueueError.c_str());
		editor_ui::EditorRendererAdapter::FlushResult result =
			adapter.flush(facade);
		require(result.accepted && result.fullSnapshot &&
		        result.resultingFence &&
		        result.resultingFence->appliedDocumentRevision ==
		            projection.documentRevision &&
		        result.resultingFence->projectionDigest ==
		            projection.projectionDigest &&
		        result.resultingFence->mappingEpoch == expectedMappingEpoch &&
		        adapter.activeFence() == result.resultingFence &&
		        facade.activeSceneState().fence == result.resultingFence &&
		        !adapter.requiresAuthoritativeRebuild(),
		        result.message.empty() ? failureMessage : result.message.c_str());
		require(adapter.activeGenerationDigest() != SceneDigest{} &&
		        adapter.activeGenerationDigest() ==
		            facade.activeSceneState().activeGenerationDigest,
		        "adapter full-snapshot digest differs from the renderer generation");
		return result;
	};
	const auto mappedHandle = [&](StableSceneId id)
	{
		const SceneTransactionConcreteStateSnapshot concrete =
			facade.sceneTransactionConcreteState();
		const RendererSceneMappingEntry* entry =
			concrete.mapping.find(id);
		require(entry != nullptr, "expected stable ID is absent from concrete mapping");
		return entry->rendererHandle;
	};

	SceneProjectionResult bootstrapProjection =
		semanticTextureProjectionFixture(projectionFence, SceneMaterial{});
	flushFullSnapshot(bootstrapProjection, 2u,
	                  "adapter bootstrap full snapshot was rejected");
	const StableSceneId survivorId = cameraId(projectionFence.authority);
	const StableSceneId duplicateId =
		alternateCameraId(projectionFence.authority);
	const RendererObjectHandle survivorHandle = mappedHandle(survivorId);

	++projectionFence.appliedDocumentRevision;
	++projectionFence.mappingEpoch;
	projectionFence.projectionDigest = digest(61u);
	SceneProjectionResult duplicateProjection =
		duplicatedCameraProjectionFixture(projectionFence);
	flushFullSnapshot(duplicateProjection, 3u,
	                  "camera duplicate full snapshot was rejected");
	const RendererObjectHandle duplicateHandle = mappedHandle(duplicateId);
	require(mappedHandle(survivorId) == survivorHandle,
	        "camera duplicate changed the surviving camera handle");

	++projectionFence.appliedDocumentRevision;
	++projectionFence.mappingEpoch;
	projectionFence.projectionDigest = digest(62u);
	SceneProjectionResult undoProjection =
		semanticTextureProjectionFixture(projectionFence, SceneMaterial{});
	flushFullSnapshot(undoProjection, 4u,
	                  "camera duplicate undo full snapshot was rejected");
	const SceneTransactionConcreteStateSnapshot undoConcrete =
		facade.sceneTransactionConcreteState();
	require(undoConcrete.mapping.find(duplicateId) == nullptr &&
	        mappedHandle(survivorId) == survivorHandle,
	        "camera duplicate undo retained the removed object or changed its survivor");

	++projectionFence.appliedDocumentRevision;
	++projectionFence.mappingEpoch;
	projectionFence.projectionDigest = digest(63u);
	SceneProjectionResult redoProjection =
		duplicatedCameraProjectionFixture(projectionFence);
	flushFullSnapshot(redoProjection, 5u,
	                  "camera duplicate redo full snapshot was rejected");
	const RendererObjectHandle redoHandle = mappedHandle(duplicateId);
	require(mappedHandle(survivorId) == survivorHandle &&
	        redoHandle != duplicateHandle &&
	        redoHandle.kind == duplicateHandle.kind &&
	        redoHandle.index > duplicateHandle.index,
	        "camera duplicate redo reused a retired handle or changed its survivor");
}

void testAdapterCameraLightSourceIdentityPreflight()
{
	const RendererTargetFence initialFence = baseFence();
	const std::array<std::pair<StableSceneId, StableSceneId>, 2u> initialPairs{{
		{cameraId(initialFence.authority),
		 alternateCameraId(initialFence.authority)},
		{lightId(initialFence.authority),
		 alternateLightId(initialFence.authority)},
	}};
	for (const auto& [firstId, secondId] : initialPairs)
	{
		auto device = std::make_unique<TransactionFakeDevice>();
		TransactionFakeDevice* fake = device.get();
		RendererFacade facade(std::move(device));
		editor_ui::EditorRendererAdapter adapter(rhi::BackendType::vulkan);
		adapter.bindRendererIdentity({101u, 7u}, {202u, 9u});
		SceneProjectionResult valid =
			multiCameraLightProjectionFixture(initialFence);
		SceneProjectionResult forged = withCoherentlySwappedProjectionCoordinates(
			valid, firstId, secondId);
		std::string error;
		const RendererTransactionObservation before =
			observeRendererTransaction(facade, *fake);
		const bool queued = adapter.enqueue(
			editor_ui::EditorController::ProjectionPublication{
				.projection = &forged,
				.refresh = editor_ui::ProjectionRefresh::fullScene,
			},
			&error);
		const RendererTransactionObservation after =
			observeRendererTransaction(facade, *fake);
		require(!queued && !error.empty() && !adapter.flush(facade).consumed &&
		        !adapter.initialized() && exactObservationEqual(before, after),
		        firstId.kind == EditorObjectKind::camera
			        ? "adapter bootstrap camera swap entered the queue or had effects"
			        : "adapter bootstrap light swap entered the queue or had effects");
		require(adapter.enqueue(
		            editor_ui::EditorController::ProjectionPublication{
		                .projection = &valid,
		                .refresh = editor_ui::ProjectionRefresh::fullScene,
		            },
		            &error),
		        error.c_str());
		const editor_ui::EditorRendererAdapter::FlushResult recovered =
			adapter.flush(facade);
		require(recovered.accepted && adapter.initialized(),
		        "adapter did not recover after source-identity bootstrap rejection");
	}

	auto device = std::make_unique<TransactionFakeDevice>();
	TransactionFakeDevice* fake = device.get();
	RendererFacade facade(std::move(device));
	editor_ui::EditorRendererAdapter adapter(rhi::BackendType::vulkan);
	adapter.bindRendererIdentity({101u, 7u}, {202u, 9u});
	SceneProjectionResult initial =
		multiCameraLightProjectionFixture(initialFence);
	std::string error;
	require(adapter.enqueue(
	            editor_ui::EditorController::ProjectionPublication{
	                .projection = &initial,
	                .refresh = editor_ui::ProjectionRefresh::fullScene,
	            },
	            &error) &&
	        adapter.flush(facade).accepted,
	        "adapter authority-change fixture could not initialize");
	RendererTargetFence replacementFence = initialFence;
	replacementFence.authority = EditorDocumentAuthority{88u};
	replacementFence.activeScene = sceneId(replacementFence.authority);
	replacementFence.projectionDigest = digest(88u);
	SceneProjectionResult replacement =
		multiCameraLightProjectionFixture(replacementFence);
	SceneProjectionResult forgedReplacement =
		withCoherentlySwappedProjectionCoordinates(
			replacement,
			cameraId(replacementFence.authority),
			alternateCameraId(replacementFence.authority));
	const RendererTransactionObservation beforeReplacement =
		observeRendererTransaction(facade, *fake);
	const bool replacementQueued = adapter.enqueue(
		editor_ui::EditorController::ProjectionPublication{
			.projection = &forgedReplacement,
			.refresh = editor_ui::ProjectionRefresh::fullScene,
		},
		&error);
	const RendererTransactionObservation afterReplacement =
		observeRendererTransaction(facade, *fake);
	require(!replacementQueued && !error.empty() &&
	        !adapter.flush(facade).consumed && adapter.initialized() &&
	        exactObservationEqual(beforeReplacement, afterReplacement),
	        "adapter authority-change source forgery queued or reset active state");
	require(adapter.enqueue(
	            editor_ui::EditorController::ProjectionPublication{
	                .projection = &replacement,
	                .refresh = editor_ui::ProjectionRefresh::fullScene,
	            },
	            &error),
	        error.c_str());
	const editor_ui::EditorRendererAdapter::FlushResult recoveredReplacement =
		adapter.flush(facade);
	require(recoveredReplacement.accepted && adapter.initialized() &&
	        recoveredReplacement.resultingFence &&
	        recoveredReplacement.resultingFence->authority ==
		        replacementFence.authority,
	        recoveredReplacement.message.empty()
		        ? "adapter did not recover with a valid replacement authority"
		        : recoveredReplacement.message.c_str());
}

void testProjectionOwnerCapabilityAttackMatrix()
{
	const RendererTargetFence fence = baseFence();
	const auto swapAssetTokens = [](SceneAsset& asset, EditorObjectKind kind)
	{
		if (kind == EditorObjectKind::camera)
		{
			std::swap(asset.cameras[0].sourceCameraIndex,
			          asset.cameras[1].sourceCameraIndex);
			std::swap(asset.cameras[0].runtimeIdentity,
			          asset.cameras[1].runtimeIdentity);
		}
		else
		{
			std::swap(asset.lights[0].sourceLightIndex,
			          asset.lights[1].sourceLightIndex);
			std::swap(asset.lights[0].runtimeIdentity,
			          asset.lights[1].runtimeIdentity);
		}
	};
	const auto associationIds = [&](EditorObjectKind kind)
	{
		return kind == EditorObjectKind::camera
			? std::pair{cameraId(fence.authority),
			            alternateCameraId(fence.authority)}
			: std::pair{lightId(fence.authority),
			            alternateLightId(fence.authority)};
	};

	using InitialMutation =
		std::function<void(RendererSceneTransactionInitialState&)>;
	const std::vector<std::pair<const char*, InitialMutation>> initializationCases{
		{"camera source-index initialization forgery had effects",
		 [](RendererSceneTransactionInitialState& initial)
		 {
			 std::swap(initial.activeAsset.cameras[0].sourceCameraIndex,
			           initial.activeAsset.cameras[1].sourceCameraIndex);
		 }},
		{"light source-index initialization forgery had effects",
		 [](RendererSceneTransactionInitialState& initial)
		 {
			 std::swap(initial.activeAsset.lights[0].sourceLightIndex,
			           initial.activeAsset.lights[1].sourceLightIndex);
		 }},
		{"coherent camera initialization forgery had effects",
		 [&](RendererSceneTransactionInitialState& initial)
		 {
			 const auto [first, second] = associationIds(EditorObjectKind::camera);
			 initial.publication.mapping = withCoherentlySwappedProjectionAuthority(
				 initial.publication.mapping, first, second);
			 swapAssetTokens(initial.activeAsset, EditorObjectKind::camera);
		 }},
		{"coherent light initialization forgery had effects",
		 [&](RendererSceneTransactionInitialState& initial)
		 {
			 const auto [first, second] = associationIds(EditorObjectKind::light);
			 initial.publication.mapping = withCoherentlySwappedProjectionAuthority(
				 initial.publication.mapping, first, second);
			 swapAssetTokens(initial.activeAsset, EditorObjectKind::light);
		 }},
	};
	for (const auto& [message, mutate] : initializationCases)
	{
		auto device = std::make_unique<TransactionFakeDevice>();
		TransactionFakeDevice* fake = device.get();
		RendererFacade facade(std::move(device));
		RendererSceneTransactionInitialState initial =
			multiCameraLightInitialState(fence);
		mutate(initial);
		const RendererTransactionObservation before =
			observeRendererTransaction(facade, *fake);
		const RendererSceneTransactionInitializationResult rejected =
			facade.initializeSceneTransactions(std::move(initial));
		const RendererTransactionObservation after =
			observeRendererTransaction(facade, *fake);
		require(!rejected.initialized &&
		        rejected.error == SceneApplyError::StaleObjectGeneration &&
		        exactObservationEqual(before, after), message);
		const RendererSceneTransactionInitializationResult recovered =
			facade.initializeSceneTransactions(multiCameraLightInitialState(fence));
		require(recovered.initialized && recovered.error == SceneApplyError::None,
		        "valid initialization did not recover after owner-capability rejection");
	}

	using SnapshotMutation = std::function<void(
		ApplyFullSceneSnapshotRequest&, const RendererTargetFence&)>;
	const std::vector<std::pair<const char*, SnapshotMutation>> snapshotCases{
		{"camera source-index snapshot forgery had effects",
		 [](ApplyFullSceneSnapshotRequest& request, const RendererTargetFence&)
		 {
			 std::swap(request.candidateAsset.cameras[0].sourceCameraIndex,
			           request.candidateAsset.cameras[1].sourceCameraIndex);
		 }},
		{"light source-index snapshot forgery had effects",
		 [](ApplyFullSceneSnapshotRequest& request, const RendererTargetFence&)
		 {
			 std::swap(request.candidateAsset.lights[0].sourceLightIndex,
			           request.candidateAsset.lights[1].sourceLightIndex);
		 }},
		{"duplicate camera source-index snapshot forgery had effects",
		 [](ApplyFullSceneSnapshotRequest& request, const RendererTargetFence&)
		 {
			 request.candidateAsset.cameras[1].sourceCameraIndex =
				 request.candidateAsset.cameras[0].sourceCameraIndex;
		 }},
		{"duplicate light source-index snapshot forgery had effects",
		 [](ApplyFullSceneSnapshotRequest& request, const RendererTargetFence&)
		 {
			 request.candidateAsset.lights[1].sourceLightIndex =
				 request.candidateAsset.lights[0].sourceLightIndex;
		 }},
		{"missing camera token snapshot forgery had effects",
		 [](ApplyFullSceneSnapshotRequest& request, const RendererTargetFence&)
		 {
			 request.candidateAsset.cameras[0].runtimeIdentity = 0u;
		 }},
		{"sentinel light source-index snapshot forgery had effects",
		 [](ApplyFullSceneSnapshotRequest& request, const RendererTargetFence&)
		 {
			 request.candidateAsset.lights[0].sourceLightIndex =
				 std::numeric_limits<uint32_t>::max();
		 }},
		{"duplicate camera token snapshot forgery had effects",
		 [](ApplyFullSceneSnapshotRequest& request, const RendererTargetFence&)
		 {
			 request.candidateAsset.cameras[1].runtimeIdentity =
				 request.candidateAsset.cameras[0].runtimeIdentity;
		 }},
		{"mixed-kind camera token snapshot forgery had effects",
		 [](ApplyFullSceneSnapshotRequest& request, const RendererTargetFence& target)
		 {
			 request.candidateAsset.cameras[0].runtimeIdentity =
				 SceneRuntimeMapping::deriveProjectionIdentity(
					 editorObjectId(lightId(target.authority)));
		 }},
		{"stale camera token snapshot forgery had effects",
		 [](ApplyFullSceneSnapshotRequest& request, const RendererTargetFence& target)
		 {
			 StableSceneId stale = cameraId(target.authority);
			 ++stale.generation;
			 request.candidateAsset.cameras[0].runtimeIdentity =
				 SceneRuntimeMapping::deriveProjectionIdentity(editorObjectId(stale));
		 }},
		{"coherent camera asset-mapping snapshot forgery had effects",
		 [&](ApplyFullSceneSnapshotRequest& request, const RendererTargetFence& target)
		 {
			 request.transaction.targetMapping =
				 withCoherentlySwappedProjectionAuthority(
					 request.transaction.targetMapping, cameraId(target.authority),
					 alternateCameraId(target.authority));
			 swapAssetTokens(request.candidateAsset, EditorObjectKind::camera);
		 }},
		{"coherent light asset-mapping snapshot forgery had effects",
		 [&](ApplyFullSceneSnapshotRequest& request, const RendererTargetFence& target)
		 {
			 request.transaction.targetMapping =
				 withCoherentlySwappedProjectionAuthority(
					 request.transaction.targetMapping, lightId(target.authority),
					 alternateLightId(target.authority));
			 swapAssetTokens(request.candidateAsset, EditorObjectKind::light);
		 }},
	};
	for (const auto& [message, mutate] : snapshotCases)
	{
		auto device = std::make_unique<TransactionFakeDevice>();
		TransactionFakeDevice* fake = device.get();
		RendererFacade facade(std::move(device));
		require(facade.initializeSceneTransactions(
		            multiCameraLightInitialState(fence)).initialized,
		        "owner-capability snapshot fixture initialization failed");
		ApplyFullSceneSnapshotRequest hostile =
			multiCameraLightFullSnapshotRequest(fence);
		mutate(hostile, hostile.transaction.targetMapping.fence());
		const RendererTransactionObservation before =
			observeRendererTransaction(facade, *fake);
		const SceneApplyResult rejected = facade.applyFullSceneSnapshot(hostile);
		const RendererTransactionObservation after =
			observeRendererTransaction(facade, *fake);
		require(rejected.disposition == SceneApplyDisposition::Rejected &&
		        rejected.error == SceneApplyError::StaleObjectGeneration &&
		        sceneDeltaCounterSnapshotIsZero(rejected.counterDelta) &&
		        exactObservationEqual(before, after), message);
		const SceneApplyResult recovered = facade.applyFullSceneSnapshot(
			multiCameraLightFullSnapshotRequest(fence));
		require(recovered.disposition ==
		            SceneApplyDisposition::AcceptedFullSnapshot &&
		        recovered.error == SceneApplyError::None,
		        "valid snapshot did not recover after owner-capability rejection");
	}

	const std::array coherentKinds{
		EditorObjectKind::camera, EditorObjectKind::light};
	for (EditorObjectKind kind : coherentKinds)
	{
		for (const bool coherent : {false, true})
		{
			auto device = std::make_unique<TransactionFakeDevice>();
			TransactionFakeDevice* fake = device.get();
			RendererFacade facade(std::move(device));
			require(facade.initializeSceneTransactions(
			            multiCameraLightInitialState(fence)).initialized,
			        "owner-capability canonical fixture initialization failed");
			const ApplyFullSceneSnapshotRequest canonical =
				multiCameraLightFullSnapshotRequest(fence);
			const SceneApplyResult accepted =
				facade.applyFullSceneSnapshot(canonical);
			require(accepted.disposition ==
			            SceneApplyDisposition::AcceptedFullSnapshot,
			        "owner-capability canonical fixture publication failed");
			ApplyFullSceneSnapshotRequest hostile = canonical;
			if (coherent)
			{
				const auto [first, second] = associationIds(kind);
				hostile.transaction.targetMapping =
					withCoherentlySwappedProjectionAuthority(
						hostile.transaction.targetMapping, first, second);
				swapAssetTokens(hostile.candidateAsset, kind);
			}
			else if (kind == EditorObjectKind::camera)
			{
				std::swap(hostile.candidateAsset.cameras[0].sourceCameraIndex,
				          hostile.candidateAsset.cameras[1].sourceCameraIndex);
			}
			else
			{
				std::swap(hostile.candidateAsset.lights[0].sourceLightIndex,
				          hostile.candidateAsset.lights[1].sourceLightIndex);
			}
			const RendererTransactionObservation before =
				observeRendererTransaction(facade, *fake);
			const SceneApplyResult rejected =
				facade.applyFullSceneSnapshot(hostile);
			const RendererTransactionObservation after =
				observeRendererTransaction(facade, *fake);
			require(rejected.disposition == SceneApplyDisposition::Rejected &&
			        rejected.error == SceneApplyError::StaleObjectGeneration &&
			        sceneDeltaCounterSnapshotIsZero(rejected.counterDelta) &&
			        exactObservationEqual(before, after),
			        "hostile canonical replay bypassed projection-owner authority");
			const RendererTransactionObservation beforeReplay =
				observeRendererTransaction(facade, *fake);
			const SceneApplyResult replay =
				facade.applyFullSceneSnapshot(canonical);
			const RendererTransactionObservation afterReplay =
				observeRendererTransaction(facade, *fake);
			require(replay.disposition == SceneApplyDisposition::AlreadyApplied &&
			        replay.error == SceneApplyError::None &&
			        sceneDeltaCounterSnapshotIsZero(replay.counterDelta) &&
			        exactObservationEqual(beforeReplay, afterReplay),
			        "valid canonical replay did not recover with zero effects");
		}
	}

	using ProjectionMutation =
		std::function<void(SceneProjectionResult&, EditorObjectKind)>;
	const std::array<std::pair<const char*, ProjectionMutation>, 2u>
		adapterMutationFamilies{{
			{"source-index",
			 [](SceneProjectionResult& projection, EditorObjectKind kind)
			 {
				 if (kind == EditorObjectKind::camera)
					 std::swap(projection.asset.cameras[0].sourceCameraIndex,
					           projection.asset.cameras[1].sourceCameraIndex);
				 else
					 std::swap(projection.asset.lights[0].sourceLightIndex,
					           projection.asset.lights[1].sourceLightIndex);
			 }},
			{"coherent asset-mapping",
			 [&](SceneProjectionResult& projection, EditorObjectKind kind)
			 {
				 const auto [first, second] = associationIds(kind);
				 projection = withCoherentlySwappedProjectionCoordinates(
					 std::move(projection), first, second);
				 swapAssetTokens(projection.asset, kind);
			 }},
		}};
	for (EditorObjectKind kind : coherentKinds)
	{
		for (const auto& [_, mutate] : adapterMutationFamilies)
		{
			auto device = std::make_unique<TransactionFakeDevice>();
			TransactionFakeDevice* fake = device.get();
			RendererFacade facade(std::move(device));
			editor_ui::EditorRendererAdapter adapter(rhi::BackendType::vulkan);
			adapter.bindRendererIdentity({101u, 7u}, {202u, 9u});
			SceneProjectionResult hostile =
				multiCameraLightProjectionFixture(fence);
			mutate(hostile, kind);
			const RendererTransactionObservation before =
				observeRendererTransaction(facade, *fake);
			std::string error;
			SceneApplyError typedError = SceneApplyError::None;
			const bool enqueued = adapter.enqueue(
				editor_ui::EditorController::ProjectionPublication{
					.projection = &hostile,
					.refresh = editor_ui::ProjectionRefresh::fullScene,
				},
				&error, &typedError);
			const auto flushed = adapter.flush(facade);
			const RendererTransactionObservation after =
				observeRendererTransaction(facade, *fake);
			require(!enqueued && !error.empty() &&
			        typedError == SceneApplyError::StaleObjectGeneration &&
			        !adapter.hasPending() &&
			        !flushed.consumed && !flushed.accepted &&
			        !adapter.initialized() &&
			        exactObservationEqual(before, after),
			        "hostile adapter bootstrap entered the queue or had effects");
			SceneProjectionResult valid =
				multiCameraLightProjectionFixture(fence);
			require(adapter.enqueue(
			            editor_ui::EditorController::ProjectionPublication{
				            .projection = &valid,
				            .refresh = editor_ui::ProjectionRefresh::fullScene,
			            },
			            &error) && adapter.flush(facade).accepted,
			        "adapter did not recover after owner-capability rejection");
		}
	}

	{
		auto device = std::make_unique<TransactionFakeDevice>();
		TransactionFakeDevice* fake = device.get();
		RendererFacade facade(std::move(device));
		editor_ui::EditorRendererAdapter adapter(rhi::BackendType::vulkan);
		adapter.bindRendererIdentity({101u, 7u}, {202u, 9u});
		SceneProjectionResult hostile =
			multiCameraLightProjectionFixture(fence);
		std::swap(hostile.asset.cameras[0].sourceCameraIndex,
		          hostile.asset.cameras[1].sourceCameraIndex);
		const RendererTransactionObservation before =
			observeRendererTransaction(facade, *fake);
		std::string error;
		SceneApplyError typedError = SceneApplyError::None;
		const bool accepted = adapter.enqueue(
			editor_ui::EditorController::ProjectionPublication{
				.projection = &hostile,
				.refresh = editor_ui::ProjectionRefresh::selectionOnly,
			},
			&error, &typedError);
		const RendererTransactionObservation after =
			observeRendererTransaction(facade, *fake);
		require(!accepted && !error.empty() &&
		        typedError == SceneApplyError::StaleObjectGeneration &&
		        !adapter.hasPending() &&
		        exactObservationEqual(before, after),
		        "selection-only publication bypassed projection-owner preflight");
	}

	for (EditorObjectKind kind : coherentKinds)
	{
		auto device = std::make_unique<TransactionFakeDevice>();
		TransactionFakeDevice* fake = device.get();
		RendererFacade facade(std::move(device));
		editor_ui::EditorRendererAdapter adapter(rhi::BackendType::vulkan);
		adapter.bindRendererIdentity({101u, 7u}, {202u, 9u});
		SceneProjectionResult initial =
			multiCameraLightProjectionFixture(fence);
		std::string error;
		require(adapter.enqueue(
		            editor_ui::EditorController::ProjectionPublication{
			            .projection = &initial,
			            .refresh = editor_ui::ProjectionRefresh::fullScene,
		            },
		            &error) && adapter.flush(facade).accepted,
		        "adapter authority-replacement fixture initialization failed");
		RendererTargetFence replacementFence = fence;
		replacementFence.authority = EditorDocumentAuthority{
			kind == EditorObjectKind::camera ? 188u : 189u};
		replacementFence.activeScene = sceneId(replacementFence.authority);
		replacementFence.projectionDigest = digest(
			static_cast<uint8_t>(replacementFence.authority.value));
		SceneProjectionResult valid =
			multiCameraLightProjectionFixture(replacementFence);
		SceneProjectionResult hostile = valid;
		const StableSceneId first = kind == EditorObjectKind::camera
			? cameraId(replacementFence.authority)
			: lightId(replacementFence.authority);
		const StableSceneId second = kind == EditorObjectKind::camera
			? alternateCameraId(replacementFence.authority)
			: alternateLightId(replacementFence.authority);
		hostile = withCoherentlySwappedProjectionCoordinates(
			std::move(hostile), first, second);
		swapAssetTokens(hostile.asset, kind);
		const RendererTransactionObservation before =
			observeRendererTransaction(facade, *fake);
		SceneApplyError typedError = SceneApplyError::None;
		const bool enqueued = adapter.enqueue(
			editor_ui::EditorController::ProjectionPublication{
				.projection = &hostile,
				.refresh = editor_ui::ProjectionRefresh::fullScene,
			},
			&error, &typedError);
		const auto flushed = adapter.flush(facade);
		const RendererTransactionObservation after =
			observeRendererTransaction(facade, *fake);
		require(!enqueued && !error.empty() &&
		        typedError == SceneApplyError::StaleObjectGeneration &&
		        !flushed.consumed &&
		        !flushed.accepted && adapter.initialized() &&
		        exactObservationEqual(before, after),
		        "hostile adapter authority replacement reset or changed active state");
		require(adapter.enqueue(
		            editor_ui::EditorController::ProjectionPublication{
			            .projection = &valid,
			            .refresh = editor_ui::ProjectionRefresh::fullScene,
		            },
		            &error), error.c_str());
		const auto recovered = adapter.flush(facade);
		require(recovered.accepted && recovered.resultingFence &&
		        recovered.resultingFence->authority == replacementFence.authority,
		        "valid adapter authority replacement did not recover");
	}

	const auto forgedIncremental = [&](RendererFacade& facade,
	                                   uint64_t mutationSerial,
	                                   EditorObjectKind kind)
	{
		ApplySceneChangeRequest request =
			incrementalRequest(fence, mutationSerial);
		const RendererTargetFence target = request.targetMapping.fence();
		const RendererSceneMappingSnapshot active =
			facade.sceneTransactionConcreteState().mapping;
		request.targetMapping = multiCameraLightMappingFor(target, &active);
		request.ownerCapability = ownerCapabilityFor(
			multiCameraLightSceneAssetFixture(
				target.appliedDocumentRevision, target.authority),
			request.targetMapping);
		const auto [first, second] = associationIds(kind);
		request.targetMapping = withCoherentlySwappedProjectionAuthority(
			request.targetMapping, first, second);
		return request;
	};
	for (EditorObjectKind kind : coherentKinds)
	{
		auto device = std::make_unique<TransactionFakeDevice>();
		TransactionFakeDevice* fake = device.get();
		RendererFacade facade(std::move(device));
		require(facade.initializeSceneTransactions(
		            multiCameraLightInitialState(fence)).initialized,
		        "direct incremental authority fixture initialization failed");
		ApplySceneChangeRequest hostile = forgedIncremental(
			facade, kind == EditorObjectKind::camera ? 7301u : 7302u, kind);
		const RendererTransactionObservation before =
			observeRendererTransaction(facade, *fake);
		const SceneApplyResult rejected = facade.applySceneChanges(hostile);
		const RendererTransactionObservation after =
			observeRendererTransaction(facade, *fake);
		require(rejected.disposition == SceneApplyDisposition::Rejected &&
		        rejected.error == SceneApplyError::StaleObjectGeneration &&
		        sceneDeltaCounterSnapshotIsZero(rejected.counterDelta) &&
		        exactObservationEqual(before, after),
		        "direct incremental projection authority replacement had effects");
	}

	for (EditorObjectKind kind : coherentKinds)
	{
		auto device = std::make_unique<TransactionFakeDevice>();
		TransactionFakeDevice* fake = device.get();
		RendererFacade facade(std::move(device));
		RendererSceneTransactionInitialState forged =
			multiCameraLightInitialState(fence);
		const auto [first, second] = associationIds(kind);
		forged.publication.mapping = withCoherentlySwappedProjectionAuthority(
			forged.publication.mapping, first, second);
		swapAssetTokens(forged.activeAsset, kind);
		const RendererTransactionObservation beforeInitialization =
			observeRendererTransaction(facade, *fake);
		const RendererSceneTransactionInitializationResult initialization =
			facade.initializeSceneTransactions(std::move(forged));
		const RendererTransactionObservation afterInitialization =
			observeRendererTransaction(facade, *fake);
		require(!initialization.initialized &&
		        initialization.error == SceneApplyError::StaleObjectGeneration &&
		        exactObservationEqual(beforeInitialization, afterInitialization),
		        "forged active association was installed during initialization");
		require(facade.initializeSceneTransactions(
		            multiCameraLightInitialState(fence)).initialized,
		        "valid active state did not recover after forged initialization");
		ApplySceneChangeRequest continuation = forgedIncremental(
			facade, kind == EditorObjectKind::camera ? 7401u : 7402u, kind);
		const RendererTransactionObservation beforeContinuation =
			observeRendererTransaction(facade, *fake);
		const SceneApplyResult rejected =
			facade.applySceneChanges(continuation);
		const RendererTransactionObservation afterContinuation =
			observeRendererTransaction(facade, *fake);
		require(rejected.disposition == SceneApplyDisposition::Rejected &&
		        rejected.error == SceneApplyError::StaleObjectGeneration &&
		        sceneDeltaCounterSnapshotIsZero(rejected.counterDelta) &&
		        exactObservationEqual(beforeContinuation, afterContinuation),
		        "forged-active incremental continuation had effects");
	}
}

void testSceneUploadProjectionBinding()
{
	const RendererTargetFence fence = baseFence();
	const SceneAsset asset = sceneAssetFixture(fence.appliedDocumentRevision);
	const SceneRuntimeMapping mapping = projectionMappingFor(fence);
	SceneUploadPlan plan = unboundUploadPlanFixture(fence);
	const SceneUploadPlanValidationResult bound = bindSceneUploadPlanProjection(
		makeSceneAssetView(asset), mapping, plan);
	require(bound.valid && plan.header == uploadHeader(fence) &&
	        plan.meshes[0].sourceId == primitiveId(fence.authority) &&
	        plan.meshes[0].nodeSourceId == nodeId(fence.authority) &&
	        plan.textures[0].sourceId == imageId(fence.authority) &&
	        plan.materials[0].sourceId == materialId(fence.authority) &&
	        plan.transforms.sourceIds ==
		        std::vector<StableSceneId>{nodeId(fence.authority)} &&
	        plan.instances.instances[0].sourceId == primitiveId(fence.authority) &&
	        plan.instances.instances[0].nodeSourceId == nodeId(fence.authority) &&
	        plan.cullRecords[0].sourceId == primitiveId(fence.authority) &&
	        plan.cullRecords[0].nodeSourceId == nodeId(fence.authority) &&
	        plan.descriptorUpdates[0].sourceId == materialId(fence.authority) &&
	        plan.drawCommands[0].sourceId == primitiveId(fence.authority) &&
	        plan.drawCommands[0].nodeSourceId == nodeId(fence.authority) &&
	        plan.requiredResources.size() == 3u,
	        "projection mapping did not bind the complete upload plan");

	SceneUploadPlan incomplete = unboundUploadPlanFixture(fence);
	const SceneUploadPlanValidationResult rejected = bindSceneUploadPlanProjection(
		makeSceneAssetView(asset), projectionMappingFor(fence, false), incomplete);
	require(!rejected.valid &&
	        rejected.code == SceneUploadPlanError::MissingStableId &&
	        incomplete.header.emptyLegacyEnvelope() &&
	        incomplete.materials[0].sourceId == StableSceneId{} &&
	        incomplete.requiredResources.empty(),
	        "missing projection identity published a partial upload-plan binding");
}

SceneUploadPlan multiPrimitiveUploadPlanFixture(
	const RendererTargetFence& fence,
	const SceneAsset& asset)
{
	SceneUploadPlanner planner;
	SceneUploadPlan plan = planner.build(makeSceneAssetView(asset)).plan;
	const SceneUploadPlanValidationResult bound = bindSceneUploadPlanProjection(
		makeSceneAssetView(asset), multiPrimitiveProjectionMappingFor(fence), plan);
	require(bound.valid, bound.error.c_str());
	return plan;
}

void testMultiPrimitiveDrawableIdentity()
{
	const RendererTargetFence fence = baseFence();
	const SceneAsset asset =
		multiPrimitiveSceneAssetFixture(fence.appliedDocumentRevision);
	const RendererSceneMappingSnapshot publicMapping =
		multiPrimitiveMappingFor(fence);
	const RendererSceneMappingValidation mappingValidation =
		publicMapping.validate();
	const RendererSceneMappingEntry* firstPrimitive =
		publicMapping.find(primitiveId(fence.authority));
	const RendererSceneMappingEntry* secondPrimitive =
		publicMapping.find(alternatePrimitiveId(fence.authority));
	require(mappingValidation.valid() && firstPrimitive != nullptr &&
	        secondPrimitive != nullptr &&
	        firstPrimitive->rendererHandle != secondPrimitive->rendererHandle &&
	        firstPrimitive->projection != secondPrimitive->projection &&
	        publicMapping.reverseLookup(firstPrimitive->rendererHandle) ==
		        std::optional{firstPrimitive->stableId} &&
	        publicMapping.reverseLookup(secondPrimitive->rendererHandle) ==
		        std::optional{secondPrimitive->stableId} &&
	        publicMapping.reverseLookup(firstPrimitive->projection) ==
		        std::optional{firstPrimitive->stableId} &&
	        publicMapping.reverseLookup(secondPrimitive->projection) ==
		        std::optional{secondPrimitive->stableId},
	        "multi-primitive public mapping is not one-to-one");

	const SceneUploadPlan plan = multiPrimitiveUploadPlanFixture(fence, asset);
	require(plan.meshes.size() == 2u &&
	        plan.instances.instances.size() == 2u &&
	        plan.cullRecords.size() == 2u &&
	        plan.drawCommands.size() == 2u &&
	        plan.meshes[0].sourceId == primitiveId(fence.authority) &&
	        plan.meshes[1].sourceId == alternatePrimitiveId(fence.authority) &&
	        plan.meshes[0].nodeSourceId == nodeId(fence.authority) &&
	        plan.meshes[1].nodeSourceId == nodeId(fence.authority) &&
	        plan.instances.instances[0].sourceIdentity() ==
		        plan.meshes[0].sourceIdentity() &&
	        plan.instances.instances[1].sourceIdentity() ==
		        plan.meshes[1].sourceIdentity() &&
	        plan.instances.instances[0].sourceIdentity() !=
		        plan.instances.instances[1].sourceIdentity() &&
	        plan.cullRecords[0].sourceIdentity() ==
		        plan.instances.instances[0].sourceIdentity() &&
	        plan.cullRecords[1].sourceIdentity() ==
		        plan.instances.instances[1].sourceIdentity() &&
	        plan.drawCommands[0].sourceIdentity() ==
		        plan.instances.instances[0].sourceIdentity() &&
	        plan.drawCommands[1].sourceIdentity() ==
		        plan.instances.instances[1].sourceIdentity() &&
	        SceneUploadPlanner::validate(makeSceneAssetView(asset), plan).valid,
	        "multi-primitive planner binding lost drawable occurrence identity");

	SceneUploadPlan duplicate = plan;
	duplicate.meshes[1].sourceId = duplicate.meshes[0].sourceId;
	duplicate.instances.instances[1].sourceId =
		duplicate.instances.instances[0].sourceId;
	duplicate.cullRecords[1].sourceId = duplicate.cullRecords[0].sourceId;
	duplicate.drawCommands[1].sourceId = duplicate.drawCommands[0].sourceId;
	const SceneUploadPlanValidationResult duplicateValidation =
		SceneUploadPlanner::validate(makeSceneAssetView(asset), duplicate);
	require(!duplicateValidation.valid &&
	        duplicateValidation.code == SceneUploadPlanError::DuplicateStableId,
	        "duplicate drawable occurrence identity passed upload-plan validation");

	auto device = std::make_unique<TransactionFakeDevice>();
	TransactionFakeDevice* fake = device.get();
	RendererFacade facade(std::move(device));
	require(facade.initializeSceneTransactions(initialState(fence)).initialized,
	        "multi-primitive renderer fixture initialization failed");
	ApplyFullSceneSnapshotRequest request = fullSnapshotRequest(fence);
	const RendererTargetFence target = request.transaction.targetMapping.fence();
	request.transaction.targetMapping = multiPrimitiveMappingFor(target);
	request.candidateAsset =
		multiPrimitiveSceneAssetFixture(target.appliedDocumentRevision);
	request.candidatePlan =
		multiPrimitiveUploadPlanFixture(target, request.candidateAsset);
	request.ownerCapability = ownerCapabilityFor(
		request.candidateAsset, request.transaction.targetMapping);
	request.transaction.ownerCapability = request.ownerCapability;
	const SceneApplyResult published = facade.applyFullSceneSnapshot(request);
	const SceneTransactionConcreteStateSnapshot concrete =
		facade.sceneTransactionConcreteState();
	require(published.disposition ==
	            SceneApplyDisposition::AcceptedFullSnapshot &&
	        published.error == SceneApplyError::None &&
	        concrete.rendererObjectCount == 2u &&
	        concrete.rendererDrawCount == 2u &&
	        concrete.rendererStateMatchesGeneration &&
	        fake->graphics.submissions.size() == 1u,
	        "multi-primitive full snapshot did not publish two unique renderer draws");
}

void testFacadeIncrementalDuplicateAndSnapshot()
{
	const RendererTargetFence initialFence = baseFence();
	RendererSceneTransactionInitialState incomplete = initialState(initialFence);
	incomplete.activePlan.requiredResources.pop_back();
	RendererFacade incompleteFacade(rhi::BackendType::vulkan);
	const RendererSceneTransactionInitializationResult incompleteResult =
		incompleteFacade.initializeSceneTransactions(std::move(incomplete));
	require(!incompleteResult.initialized &&
	        incompleteResult.error == SceneApplyError::InvalidPayload &&
	        !incompleteFacade.activeSceneState().initialized,
	        "incomplete required-resource plan published initial scene state");

	RendererFacade facade(rhi::BackendType::vulkan);
	const RendererSceneTransactionInitializationResult initialized =
		facade.initializeSceneTransactions(initialState(initialFence));
	require(initialized.initialized && initialized.error == SceneApplyError::None,
	        "facade transaction initialization failed");

	const RendererSceneStateSnapshot initial = facade.activeSceneState();
	require(initial.initialized && initial.fence == initialFence &&
	        initial.generation == 1u && initial.nodeCount == 1u,
	        "initial facade scene state is incomplete");

	const rhi::RHIHotPathCounters rhiBefore =
		rhi::snapshotHotPathCounters(rhi::BackendType::vulkan);
	const ApplySceneChangeRequest transform = incrementalRequest(initialFence, 44u);
	const SceneApplyResult applied = facade.applySceneChanges(transform);
	require(applied.disposition == SceneApplyDisposition::AcceptedIncremental &&
	        applied.error == SceneApplyError::None && applied.resultingFence,
	        "canonical transform was not accepted incrementally");
	require(applied.counterDelta.geometryUploadCalls == 0u &&
	        applied.counterDelta.geometryBytesUploaded == 0u &&
	        applied.counterDelta.textureUploadCalls == 0u &&
	        applied.counterDelta.textureBytesUploaded == 0u &&
	        applied.counterDelta.deviceIdleCalls == 0u &&
	        applied.counterDelta.fullSnapshotCount == 0u,
	        "continuous transform violated zero-work counters");
	require(rhi::hotPathCountersAreZero(rhi::hotPathCounterDelta(
	            rhiBefore,
	            rhi::snapshotHotPathCounters(rhi::BackendType::vulkan))),
	        "logical transform touched the RHI hot path");

	const RendererSceneStateSnapshot afterTransform = facade.activeSceneState();
	require(afterTransform.generation == initial.generation + 1u &&
	        afterTransform.fence == applied.resultingFence,
	        "accepted transform did not publish one generation");

	const SceneDeltaCounterSnapshot countersBeforeDuplicate =
		facade.sceneDeltaCounters();
	const SceneApplyResult duplicate = facade.applySceneChanges(transform);
	require(duplicate.disposition == SceneApplyDisposition::AlreadyApplied &&
	        duplicate.error == SceneApplyError::None &&
	        duplicate.oldFence == *duplicate.resultingFence &&
	        sceneDeltaCounterSnapshotIsZero(duplicate.counterDelta) &&
	        facade.sceneDeltaCounters() == countersBeforeDuplicate &&
	        facade.activeSceneState() == afterTransform,
	        "exact duplicate changed state or counters");

	ApplySceneChangeRequest tampered = transform;
	tampered.canonicalBytes.back() ^= 0x80u;
	const SceneApplyResult identityReuse = facade.applySceneChanges(tampered);
	require(identityReuse.disposition == SceneApplyDisposition::Rejected &&
	        identityReuse.error == SceneApplyError::InvalidState &&
	        facade.activeSceneState() == afterTransform,
	        "canonical identity reuse with different bytes was not side-effect free");

	ApplySceneChangeRequest stale = incrementalRequest(initialFence, 45u);
	stale.expectedTarget = *applied.resultingFence;
	const SceneApplyResult staleResult = facade.applySceneChanges(stale);
	require(staleResult.disposition == SceneApplyDisposition::Rejected &&
	        staleResult.error == SceneApplyError::StaleRevision,
	        "stale revision did not return the typed stale rejection");
	require(facade.activeSceneState() == afterTransform,
	        "stale revision changed active generation or fence");

	ApplyFullSceneSnapshotRequest snapshot =
		fullSnapshotRequest(*applied.resultingFence);
	const SceneApplyResult snapshotApplied = facade.applyFullSceneSnapshot(snapshot);
	require(snapshotApplied.disposition == SceneApplyDisposition::AcceptedFullSnapshot &&
	        snapshotApplied.error == SceneApplyError::None &&
	        snapshotApplied.resultingFence,
	        "immutable logical full snapshot was not accepted");
	const RendererSceneStateSnapshot afterSnapshot = facade.activeSceneState();
	require(afterSnapshot.generation == afterTransform.generation + 1u &&
	        afterSnapshot.fence == snapshotApplied.resultingFence &&
	        afterSnapshot.nodeCount == 1u &&
	        snapshotApplied.counterDelta.fullSnapshotCount == 1u &&
	        snapshotApplied.counterDelta.deviceIdleCalls == 0u,
	        "full snapshot did not publish exactly one idle-free generation");

	const SceneDeltaCounterSnapshot beforeSnapshotDuplicate =
		facade.sceneDeltaCounters();
	const SceneApplyResult snapshotDuplicate =
		facade.applyFullSceneSnapshot(snapshot);
	require(snapshotDuplicate.disposition == SceneApplyDisposition::AlreadyApplied &&
	        snapshotDuplicate.error == SceneApplyError::None &&
	        snapshotDuplicate.oldFence == *snapshotDuplicate.resultingFence &&
	        sceneDeltaCounterSnapshotIsZero(snapshotDuplicate.counterDelta) &&
	        facade.sceneDeltaCounters() == beforeSnapshotDuplicate &&
	        facade.activeSceneState() == afterSnapshot,
	        "exact full-snapshot replay performed candidate work or changed state");

	ApplyFullSceneSnapshotRequest incompleteSnapshot =
		fullSnapshotRequest(*snapshotApplied.resultingFence);
	incompleteSnapshot.candidatePlan.materials.clear();
	const RendererSceneStateSnapshot beforeIncomplete = facade.activeSceneState();
	const SceneApplyResult incompleteSnapshotResult =
		facade.applyFullSceneSnapshot(incompleteSnapshot);
	require(incompleteSnapshotResult.disposition == SceneApplyDisposition::Rejected &&
	        incompleteSnapshotResult.error == SceneApplyError::InvalidPayload &&
	        facade.activeSceneState() == beforeIncomplete,
	        "incomplete full-snapshot resources changed active state");

	ApplyFullSceneSnapshotRequest invalidSnapshot =
		fullSnapshotRequest(*snapshotApplied.resultingFence);
	++invalidSnapshot.candidatePlan.header.mappingEpoch;
	const RendererSceneStateSnapshot beforeInvalid = facade.activeSceneState();
	const SceneApplyResult invalid = facade.applyFullSceneSnapshot(invalidSnapshot);
	require(invalid.disposition == SceneApplyDisposition::Rejected &&
	        invalid.error == SceneApplyError::MappingMismatch &&
	        facade.activeSceneState() == beforeInvalid,
	        "mismatched snapshot plan was not rejected before publication");
}

void testFastPathsAndIndependentClocks()
{
	const RendererTargetFence initialFence = baseFence();
	RendererFacade facade(rhi::BackendType::vulkan);
	require(facade.initializeSceneTransactions(initialState(initialFence)).initialized,
	        "fast-path facade initialization failed");
	RendererTargetFence current = initialFence;
	uint64_t expectedGeneration = facade.activeSceneState().generation;

	const auto apply = [&](ApplySceneChangeRequest request, const char* message)
	{
		const SceneApplyResult result = facade.applySceneChanges(request);
		requireZeroContinuousWork(result, message);
		current = *result.resultingFence;
		++expectedGeneration;
		require(facade.activeSceneState().generation == expectedGeneration &&
		        facade.activeSceneState().fence == current,
		        "fast-path publication did not advance exactly one generation");
		return result;
	};

	SceneVisibilityValue visibleBefore{true};
	SceneVisibilityValue visibleAfter{false};
	apply(incrementalRequest(current, 90u, {SceneFieldChange{
		.target = nodeId(current.authority),
		.changeClass = SceneChangeClass::Visibility,
		.action = SceneChangeAction::Update,
		.field = SceneField::NodeLocalVisibility,
		.mutationSerial = 90u,
		.beforeValue = SceneFieldValue{visibleBefore},
		.afterValue = SceneFieldValue{visibleAfter},
	}}), "visibility edit violated the continuous fast-path contract");

	SceneMaterialParametersValue materialBefore;
	SceneMaterialParametersValue materialAfter;
	materialAfter.roughnessFactor = 0.25;
	materialAfter.doubleSided = true;
	apply(incrementalRequest(current, 91u, {SceneFieldChange{
		.target = materialId(current.authority),
		.changeClass = SceneChangeClass::MaterialParameters,
		.action = SceneChangeAction::Update,
		.field = SceneField::MaterialParameters,
		.mutationSerial = 91u,
		.beforeValue = SceneFieldValue{materialBefore},
		.afterValue = SceneFieldValue{materialAfter},
	}}), "material-parameter edit violated the continuous fast-path contract");

	ScenePrimitiveMaterialBindingValue bindingBefore;
	ScenePrimitiveMaterialBindingValue bindingAfter;
	bindingAfter.material = materialId(current.authority);
	apply(incrementalRequest(current, 92u, {SceneFieldChange{
		.target = primitiveId(current.authority),
		.changeClass = SceneChangeClass::MaterialBinding,
		.action = SceneChangeAction::Update,
		.field = SceneField::PrimitiveMaterialBinding,
		.mutationSerial = 92u,
		.beforeValue = SceneFieldValue{bindingBefore},
		.afterValue = SceneFieldValue{bindingAfter},
	}}), "primitive binding violated the continuous fast-path contract");

	SceneCameraValue cameraBefore;
	SceneCameraValue cameraAfter;
	cameraAfter.yfov = 0.9;
	apply(incrementalRequest(current, 93u, {SceneFieldChange{
		.target = cameraId(current.authority),
		.changeClass = SceneChangeClass::Camera,
		.action = SceneChangeAction::Update,
		.field = SceneField::CameraParameters,
		.mutationSerial = 93u,
		.beforeValue = SceneFieldValue{cameraBefore},
		.afterValue = SceneFieldValue{cameraAfter},
	}}), "camera edit violated the continuous fast-path contract");

	SceneLightValue lightBefore;
	SceneLightValue lightAfter;
	lightAfter.intensity = 4.0;
	apply(incrementalRequest(current, 94u, {SceneFieldChange{
		.target = lightId(current.authority),
		.changeClass = SceneChangeClass::Light,
		.action = SceneChangeAction::Update,
		.field = SceneField::LightParameters,
		.mutationSerial = 94u,
		.beforeValue = SceneFieldValue{lightBefore},
		.afterValue = SceneFieldValue{lightAfter},
	}}), "light edit violated the continuous fast-path contract");

	SceneCameraValue cameraFromViewBefore = cameraAfter;
	SceneCameraValue cameraFromViewAfter = cameraAfter;
	cameraFromViewAfter.yfov = 1.05;
	SceneNodeTransformValue transformBefore;
	SceneNodeTransformValue transformAfter;
	std::get<SceneTrsValue>(transformAfter.value).translation[2] = -5.0;
	const SceneApplyResult cameraFromView = apply(
		incrementalRequest(current, 95u, {
			SceneFieldChange{
				.target = cameraId(current.authority),
				.changeClass = SceneChangeClass::Camera,
				.action = SceneChangeAction::Update,
				.field = SceneField::CameraParameters,
				.mutationSerial = 95u,
				.beforeValue = SceneFieldValue{cameraFromViewBefore},
				.afterValue = SceneFieldValue{cameraFromViewAfter},
			},
			SceneFieldChange{
				.target = nodeId(current.authority),
				.changeClass = SceneChangeClass::Transform,
				.action = SceneChangeAction::Update,
				.field = SceneField::NodeLocalTransform,
				.mutationSerial = 95u,
				.beforeValue = SceneFieldValue{transformBefore},
				.afterValue = SceneFieldValue{transformAfter},
			},
		}), "atomic camera-from-view violated the continuous fast-path contract");
	require(cameraFromView.counterDelta.sceneOperationsApplied == 2u &&
	        cameraFromView.counterDelta.cameraOpsApplied == 1u &&
	        cameraFromView.counterDelta.nodeTransformOpsApplied == 1u,
	        "camera-from-view was not one atomic two-field transaction");

	const uint64_t revisionBeforeActivation = current.appliedDocumentRevision;
	const uint64_t rootBeforeActivation = current.rootExternalStateSerial;
	apply(identityTransitionRequest(current, SceneChangeDomain::SceneActivation, 96u),
	      "scene activation violated the incremental transaction contract");
	require(current.appliedDocumentRevision == revisionBeforeActivation &&
	        current.rootExternalStateSerial == rootBeforeActivation &&
	        current.sceneActivationEpoch == initialFence.sceneActivationEpoch + 1u &&
	        current.activeScene == alternateSceneId(current.authority),
	        "scene activation changed an independent document/root clock");

	const uint64_t revisionBeforeRoot = current.appliedDocumentRevision;
	const uint64_t activationBeforeRoot = current.sceneActivationEpoch;
	apply(identityTransitionRequest(current, SceneChangeDomain::SceneRootState, 97u),
	      "scene root state violated the incremental transaction contract");
	require(current.appliedDocumentRevision == revisionBeforeRoot &&
	        current.sceneActivationEpoch == activationBeforeRoot &&
	        current.rootExternalStateSerial == rootBeforeActivation + 1u,
	        "scene root state changed an independent document/activation clock");

	SceneHierarchyValue hierarchyBefore;
	SceneHierarchyValue hierarchyAfter;
	hierarchyAfter.attachments.push_back(cameraId(current.authority));
	const ApplySceneChangeRequest hierarchy = incrementalRequest(current, 98u, {
		SceneFieldChange{
			.target = nodeId(current.authority),
			.changeClass = SceneChangeClass::Hierarchy,
			.action = SceneChangeAction::Update,
			.field = SceneField::HierarchyAttachment,
			.mutationSerial = 98u,
			.beforeValue = SceneFieldValue{hierarchyBefore},
			.afterValue = SceneFieldValue{hierarchyAfter},
		},
	});
	const RendererSceneStateSnapshot beforeHierarchy = facade.activeSceneState();
	const SceneApplyResult hierarchyResult = facade.applySceneChanges(hierarchy);
	require(hierarchyResult.disposition == SceneApplyDisposition::Rejected &&
	        hierarchyResult.error == SceneApplyError::InvalidPayload &&
	        hierarchyResult.requestedFullSnapshotReason ==
		        FullRebuildReason::StructuralTopology &&
	        facade.activeSceneState() == beforeHierarchy,
	        "hierarchy work was not rejected with an explicit structural snapshot reason");

	require(rhi::hotPathCountersAreZero(rhi::snapshotHotPathCounters(
	            rhi::BackendType::vulkan)),
	        "logical fast paths touched the Vulkan RHI counter surface");
}

void testCameraAndLightAttachmentPublication()
{
	const RendererTargetFence initialFence = baseFence();
	RendererFacade facade(rhi::BackendType::vulkan);
	require(facade.initializeSceneTransactions(
	            attachmentInitialState(initialFence)).initialized,
	        "attachment fixture initialization failed");

	SceneCameraValue cameraBefore;
	SceneCameraValue cameraAfter = cameraBefore;
	cameraAfter.yfov = 0.9;
	SceneLightValue lightBefore;
	SceneLightValue lightAfter = lightBefore;
	lightAfter.type = editor::SceneLightType::Directional;
	lightAfter.intensity = 3.0;
	const SceneObjectAttachmentValue cameraAttachmentBefore{
		.node = nodeId(initialFence.authority),
	};
	const SceneObjectAttachmentValue cameraAttachmentAfter{
		.node = alternateNodeId(initialFence.authority),
	};
	const SceneObjectAttachmentValue lightAttachmentBefore =
		cameraAttachmentBefore;
	const SceneObjectAttachmentValue lightAttachmentAfter =
		cameraAttachmentAfter;

	ApplySceneChangeRequest reattach = incrementalRequest(initialFence, 600u, {
		SceneFieldChange{
			.target = cameraId(initialFence.authority),
			.changeClass = SceneChangeClass::Camera,
			.action = SceneChangeAction::Update,
			.field = SceneField::CameraParameters,
			.mutationSerial = 600u,
			.beforeValue = SceneFieldValue{cameraBefore},
			.afterValue = SceneFieldValue{cameraAfter},
		},
		SceneFieldChange{
			.target = cameraId(initialFence.authority),
			.changeClass = SceneChangeClass::Camera,
			.action = SceneChangeAction::Update,
			.field = SceneField::CameraAttachment,
			.mutationSerial = 600u,
			.beforeValue = SceneFieldValue{cameraAttachmentBefore},
			.afterValue = SceneFieldValue{cameraAttachmentAfter},
		},
		SceneFieldChange{
			.target = lightId(initialFence.authority),
			.changeClass = SceneChangeClass::Light,
			.action = SceneChangeAction::Update,
			.field = SceneField::LightParameters,
			.mutationSerial = 600u,
			.beforeValue = SceneFieldValue{lightBefore},
			.afterValue = SceneFieldValue{lightAfter},
		},
		SceneFieldChange{
			.target = lightId(initialFence.authority),
			.changeClass = SceneChangeClass::Light,
			.action = SceneChangeAction::Update,
			.field = SceneField::LightAttachment,
			.mutationSerial = 600u,
			.beforeValue = SceneFieldValue{lightAttachmentBefore},
			.afterValue = SceneFieldValue{lightAttachmentAfter},
		},
	});
	reattach.targetMapping = attachmentMappingFor(
		reattach.targetMapping.fence());
	reattach.ownerCapability = ownerCapabilityFor(
		sceneAssetFixture(reattach.targetMapping.fence().appliedDocumentRevision,
		                   reattach.targetMapping.fence().authority),
		reattach.targetMapping);
	const SceneApplyResult reattached = facade.applySceneChanges(reattach);
	requireZeroContinuousWork(
		reattached, "camera/light reattachment violated the continuous fast path");
	SceneTransactionConcreteStateSnapshot concrete =
		facade.sceneTransactionConcreteState();
	require(concrete.firstCamera && concrete.firstCamera->nodeIndex == 1 &&
	        concrete.firstCamera->verticalFieldOfViewRadians == 0.9f &&
	        concrete.firstLight && concrete.firstLight->nodeIndex == 1 &&
	        concrete.firstLight->type == demo::SceneLightType::directional &&
	        concrete.firstLight->intensity == 3.0f,
	        "camera/light attachment edit retained the old runtime node binding");

	const RendererTargetFence current = *reattached.resultingFence;
	SceneCameraValue nextCamera = cameraAfter;
	nextCamera.yfov = 1.0;
	SceneLightValue nextLight = lightAfter;
	nextLight.intensity = 4.0;
	ApplySceneChangeRequest parametersOnly = incrementalRequest(current, 601u, {
		SceneFieldChange{
			.target = cameraId(current.authority),
			.changeClass = SceneChangeClass::Camera,
			.action = SceneChangeAction::Update,
			.field = SceneField::CameraParameters,
			.mutationSerial = 601u,
			.beforeValue = SceneFieldValue{cameraAfter},
			.afterValue = SceneFieldValue{nextCamera},
		},
		SceneFieldChange{
			.target = lightId(current.authority),
			.changeClass = SceneChangeClass::Light,
			.action = SceneChangeAction::Update,
			.field = SceneField::LightParameters,
			.mutationSerial = 601u,
			.beforeValue = SceneFieldValue{lightAfter},
			.afterValue = SceneFieldValue{nextLight},
		},
	});
	parametersOnly.targetMapping = attachmentMappingFor(
		parametersOnly.targetMapping.fence());
	parametersOnly.ownerCapability = ownerCapabilityFor(
		sceneAssetFixture(parametersOnly.targetMapping.fence().appliedDocumentRevision,
		                   parametersOnly.targetMapping.fence().authority),
		parametersOnly.targetMapping);
	const SceneApplyResult parametersPublished =
		facade.applySceneChanges(parametersOnly);
	requireZeroContinuousWork(
		parametersPublished,
		"camera/light parameter edit violated the continuous fast path");
	concrete = facade.sceneTransactionConcreteState();
	require(concrete.firstCamera && concrete.firstCamera->nodeIndex == 1 &&
	        concrete.firstCamera->verticalFieldOfViewRadians == 1.0f &&
	        concrete.firstLight && concrete.firstLight->nodeIndex == 1 &&
	        concrete.firstLight->intensity == 4.0f,
	        "camera/light parameter edit discarded the authoritative node binding");
}

void testTextureUploadFootprintValidation()
{
	const rhi::TextureDesc rgba8{
		.dimension = rhi::TextureDimension::e2D,
		.format = rhi::TextureFormat::rgba8Unorm,
		.extent = {1024u, 1024u, 1u},
		.mipLevels = 1u,
		.arrayLayers = 1u,
	};
	const rhi::BufferTextureCopyDesc fullRgba8{
		.aspect = rhi::TextureAspect::color,
		.width = 1024u,
		.height = 1024u,
		.depth = 1u,
	};
	constexpr uint64_t rgba8Bytes = 4u * 1024u * 1024u;
	const TextureUploadValidationResult exact = validateTextureUploadRegion(
		rgba8, fullRgba8, 0u, rgba8Bytes, rgba8Bytes);
	require(exact.valid() && exact.requiredBytes == rgba8Bytes &&
	        exact.relativeEnd == rgba8Bytes && exact.aggregateEnd == rgba8Bytes,
	        "exact RGBA8 texture footprint was not accepted");
	const TextureUploadValidationResult shortRgba8 = validateTextureUploadRegion(
		rgba8, fullRgba8, 0u, rgba8Bytes - 1u, rgba8Bytes);
	require(shortRgba8.error == TextureUploadValidationError::sliceRangeExceeded,
	        "undersized RGBA8 slice was not rejected");

	const rhi::TextureDesc overflowTexture{
		.dimension = rhi::TextureDimension::e3D,
		.format = rhi::TextureFormat::rgba16Sfloat,
		.extent = {UINT32_MAX, UINT32_MAX, UINT32_MAX},
		.mipLevels = 1u,
		.arrayLayers = 1u,
	};
	const rhi::BufferTextureCopyDesc overflowRegion{
		.aspect = rhi::TextureAspect::color,
		.width = UINT32_MAX,
		.height = UINT32_MAX,
		.depth = UINT32_MAX,
	};
	require(validateTextureUploadRegion(
	            overflowTexture, overflowRegion, 0u, UINT64_MAX, UINT64_MAX).error ==
	            TextureUploadValidationError::arithmeticOverflow,
	        "overflowing 3D texture footprint was not rejected");

	const rhi::TextureDesc bc7{
		.dimension = rhi::TextureDimension::e2D,
		.format = rhi::TextureFormat::bc7UnormBlock,
		.extent = {7u, 5u, 1u},
		.mipLevels = 1u,
		.arrayLayers = 1u,
	};
	const rhi::BufferTextureCopyDesc bc7Region{
		.aspect = rhi::TextureAspect::color,
		.width = 7u,
		.height = 5u,
		.depth = 1u,
	};
	const TextureUploadValidationResult bc7Exact = validateTextureUploadRegion(
		bc7, bc7Region, 0u, 64u, 64u);
	require(bc7Exact.valid() && bc7Exact.requiredBytes == 64u,
	        "BC7 edge blocks did not round to the exact footprint");
	rhi::BufferTextureCopyDesc misalignedBc7 = bc7Region;
	misalignedBc7.textureOffset.x = 1;
	misalignedBc7.width = 6u;
	require(validateTextureUploadRegion(
	            bc7, misalignedBc7, 0u, 64u, 64u).error ==
	            TextureUploadValidationError::compressedRegionMisaligned,
	        "misaligned BC7 region was not rejected");

	const rhi::TextureDesc arrayTexture{
		.dimension = rhi::TextureDimension::e2DArray,
		.format = rhi::TextureFormat::rgba8Unorm,
		.extent = {4u, 4u, 1u},
		.mipLevels = 1u,
		.arrayLayers = 3u,
	};
	const rhi::BufferTextureCopyDesc arrayRegion{
		.aspect = rhi::TextureAspect::color,
		.layerCount = 3u,
		.width = 4u,
		.height = 4u,
		.depth = 1u,
	};
	const TextureUploadValidationResult arrayExact = validateTextureUploadRegion(
		arrayTexture, arrayRegion, 0u, 192u, 192u);
	require(arrayExact.valid() && arrayExact.requiredBytes == 192u,
	        "array-layer texture footprint was not multiplied exactly");

	const rhi::TextureDesc volumeTexture{
		.dimension = rhi::TextureDimension::e3D,
		.format = rhi::TextureFormat::rgba8Unorm,
		.extent = {4u, 4u, 3u},
		.mipLevels = 1u,
		.arrayLayers = 1u,
	};
	const rhi::BufferTextureCopyDesc volumeRegion{
		.aspect = rhi::TextureAspect::color,
		.width = 4u,
		.height = 4u,
		.depth = 3u,
	};
	const TextureUploadValidationResult volumeExact = validateTextureUploadRegion(
		volumeTexture, volumeRegion, 0u, 192u, 192u);
	require(volumeExact.valid() && volumeExact.requiredBytes == 192u,
	        "3D depth texture footprint was not multiplied exactly");

	const rhi::TextureDesc mipTexture{
		.dimension = rhi::TextureDimension::e2D,
		.format = rhi::TextureFormat::rgba8Unorm,
		.extent = {16u, 8u, 1u},
		.mipLevels = 5u,
		.arrayLayers = 1u,
	};
	const rhi::BufferTextureCopyDesc mipRegion{
		.aspect = rhi::TextureAspect::color,
		.mipLevel = 3u,
		.width = 2u,
		.height = 1u,
		.depth = 1u,
	};
	const TextureUploadValidationResult mipExact = validateTextureUploadRegion(
		mipTexture, mipRegion, 0u, 8u, 8u);
	require(mipExact.valid() && mipExact.requiredBytes == 8u,
	        "mip-relative texture footprint was not computed exactly");
	const rhi::TextureDesc depthTexture{
		.dimension = rhi::TextureDimension::e2D,
		.format = rhi::TextureFormat::d32Sfloat,
		.extent = {1u, 1u, 1u},
		.mipLevels = 1u,
		.arrayLayers = 1u,
	};
	require(validateTextureUploadRegion(
	            depthTexture,
	            rhi::BufferTextureCopyDesc{
		            .aspect = rhi::TextureAspect::color,
		            .width = 1u,
		            .height = 1u,
		            .depth = 1u,
	            },
	            0u, 4u, 4u).error == TextureUploadValidationError::aspectMismatch,
	        "depth/color aspect mismatch was not rejected");
	require(validateTextureUploadRegion(
	            rhi::TextureDesc{
		            .dimension = rhi::TextureDimension::e2D,
		            .format = rhi::TextureFormat::rgba8Unorm,
		            .extent = {2u, 2u, 1u},
		            .mipLevels = 1u,
		            .arrayLayers = 1u,
	            },
	            rhi::BufferTextureCopyDesc{
		            .aspect = rhi::TextureAspect::color,
		            .width = 2u,
		            .height = 2u,
		            .depth = 1u,
	            },
	            64u, 65u, 128u).error ==
	            TextureUploadValidationError::aggregateRangeExceeded,
	        "aggregate staging overrun was not rejected");

	TransactionFakeDevice fake;
	BatchUploadContext batch;
	batch.init(fake, 128u);
	const std::vector<BatchUploadContext::Slice> slices = batch.reserveSlices(
		std::vector<uint64_t>{64u, 64u}, 1u);
	const rhi::TextureDesc sliceTexture{
		.dimension = rhi::TextureDimension::e2D,
		.format = rhi::TextureFormat::rgba8Unorm,
		.extent = {4u, 4u, 1u},
		.mipLevels = 1u,
		.arrayLayers = 1u,
	};
	const rhi::BufferTextureCopyDesc firstRegion{
		.texture = {31u, 1u},
		.aspect = rhi::TextureAspect::color,
		.width = 4u,
		.height = 4u,
		.depth = 1u,
	};
	rhi::BufferTextureCopyDesc secondRegion = firstRegion;
	secondRegion.texture = {32u, 1u};
	require(batch.recordTextureUpload(
	            slices[0], firstRegion.texture, sliceTexture, firstRegion).valid() &&
	        batch.recordTextureUpload(
	            slices[1], secondRegion.texture, sliceTexture, secondRegion).valid(),
	        "valid reserved texture slices were rejected");
	rhi::BufferTextureCopyDesc crossingRegion = secondRegion;
	crossingRegion.bufferOffset = 1u;
	require(batch.recordTextureUpload(
	            slices[1], crossingRegion.texture, sliceTexture, crossingRegion).error ==
	            TextureUploadValidationError::sliceRangeExceeded,
	        "cross-slice texture copy was not rejected before recording");
	TransactionFakeCommandBuffer commandBuffer(&fake.capture);
	batch.executeUploads(commandBuffer);
	require(fake.capture.textureCopies.size() == 2u &&
	        fake.capture.textureCopies[0].bufferOffset == slices[0].offset &&
	        fake.capture.textureCopies[1].bufferOffset == slices[1].offset &&
	        fake.capture.textureCopies[0].bufferOffset + 64u <=
	            slices[0].offset + slices[0].size &&
	        fake.capture.textureCopies[1].bufferOffset + 64u <=
	            slices[1].offset + slices[1].size,
	        "recorded texture copies did not remain inside their reserved slices");
	batch.destroy();
}

void requireMalformedTexturePlanHasZeroRhiEffects(
	ApplyFullSceneSnapshotRequest request,
	const char* message)
{
	auto device = std::make_unique<TransactionFakeDevice>();
	TransactionFakeDevice* fake = device.get();
	RendererFacade facade(std::move(device));
	const RendererTargetFence initialFence = baseFence();
	require(facade.initializeSceneTransactions(initialState(initialFence)).initialized,
	        "malformed texture fixture initialization failed");
	const RendererSceneStateSnapshot before = facade.activeSceneState();
	const uint64_t visibleBefore = facade.sceneTransactionVisibleGeneration();
	const uint64_t createdBefore = fake->createdCount;
	const uint64_t destroyedBefore = fake->destroyedCount;
	const uint64_t descriptorWritesBefore = fake->descriptorWriteCount;
	const size_t buffersBefore = fake->capture.buffers.size();
	const size_t texturesBefore = fake->capture.textures.size();
	const size_t viewsBefore = fake->capture.views.size();
	const size_t barriersBefore = fake->capture.textureBarriers.size();
	const size_t copiesBefore = fake->capture.textureCopies.size();
	const size_t eventsBefore = fake->capture.events.size();
	const size_t submissionsBefore = fake->graphics.submissions.size();
	const SceneApplyResult result = facade.applyFullSceneSnapshot(request);
	require(result.disposition == SceneApplyDisposition::Rejected &&
	        result.error == SceneApplyError::InvalidPayload &&
	        result.message.find("texture") != std::string::npos &&
	        facade.activeSceneState() == before &&
	        facade.sceneTransactionVisibleGeneration() == visibleBefore &&
	        fake->createdCount == createdBefore &&
	        fake->destroyedCount == destroyedBefore &&
	        fake->descriptorWriteCount == descriptorWritesBefore &&
	        fake->capture.buffers.size() == buffersBefore &&
	        fake->capture.textures.size() == texturesBefore &&
	        fake->capture.views.size() == viewsBefore &&
	        fake->capture.textureBarriers.size() == barriersBefore &&
	        fake->capture.textureCopies.size() == copiesBefore &&
	        fake->capture.events.size() == eventsBefore &&
	        fake->graphics.submissions.size() == submissionsBefore &&
	        fake->waitIdleCount == 0u && fake->graphics.waitIdleCount == 0u,
	        message);
}

void testMalformedTexturePlansHaveZeroRhiEffects()
{
	const RendererTargetFence fence = baseFence();
	ApplyFullSceneSnapshotRequest encodedSized = fullSnapshotRequest(fence);
	std::vector<uint8_t> encodedPayload(32u, 0u);
	const std::array<uint8_t, 8> pngIdentifier{{
		0x89u, 0x50u, 0x4Eu, 0x47u, 0x0Du, 0x0Au, 0x1Au, 0x0Au,
	}};
	std::copy(pngIdentifier.begin(), pngIdentifier.end(), encodedPayload.begin());
	setSingleRawTexture(encodedSized, 8u, 8u, std::move(encodedPayload));
	requireMalformedTexturePlanHasZeroRhiEffects(
		std::move(encodedSized),
		"encoded-sized raw RGBA8 payload produced RHI effects");

	ApplyFullSceneSnapshotRequest overflow = fullSnapshotRequest(fence);
	setSingleRawTexture(overflow, UINT32_MAX, UINT32_MAX,
	                    std::vector<uint8_t>(4u, 0u));
	requireMalformedTexturePlanHasZeroRhiEffects(
		std::move(overflow),
		"overflowing raw RGBA8 plan produced RHI effects");

	const std::array<uint64_t, 2> undersizedMipBytes{{48u, 16u}};
	ApplyFullSceneSnapshotRequest undersizedKtx = fullSnapshotRequest(fence);
	setSingleKtxTexture(
		undersizedKtx, 8u, 8u, 2u,
		makeBc7Ktx2(8u, 8u, undersizedMipBytes));
	requireMalformedTexturePlanHasZeroRhiEffects(
		std::move(undersizedKtx),
		"undersized BC7 KTX mip produced RHI effects");

	const std::array<uint64_t, 2> validMipBytes{{64u, 16u}};
	std::vector<uint8_t> outOfRangeBytes = makeBc7Ktx2(8u, 8u, validMipBytes);
	TestKtx2Level outOfRangeLevel = testKtx2Level(outOfRangeBytes, 1u);
	outOfRangeLevel.offset = static_cast<uint64_t>(outOfRangeBytes.size()) + 1u;
	setTestKtx2Level(outOfRangeBytes, 1u, outOfRangeLevel);
	ApplyFullSceneSnapshotRequest outOfRangeKtx = fullSnapshotRequest(fence);
	setSingleKtxTexture(outOfRangeKtx, 8u, 8u, 2u, std::move(outOfRangeBytes));
	requireMalformedTexturePlanHasZeroRhiEffects(
		std::move(outOfRangeKtx),
		"out-of-range KTX mip produced RHI effects");

	std::vector<uint8_t> overlappingBytes = makeBc7Ktx2(8u, 8u, validMipBytes);
	const TestKtx2Level firstLevel = testKtx2Level(overlappingBytes, 0u);
	TestKtx2Level overlappingLevel = testKtx2Level(overlappingBytes, 1u);
	overlappingLevel.offset = firstLevel.offset + 32u;
	setTestKtx2Level(overlappingBytes, 1u, overlappingLevel);
	ApplyFullSceneSnapshotRequest overlappingKtx = fullSnapshotRequest(fence);
	setSingleKtxTexture(overlappingKtx, 8u, 8u, 2u, std::move(overlappingBytes));
	requireMalformedTexturePlanHasZeroRhiEffects(
		std::move(overlappingKtx),
		"overlapping KTX mip ranges produced RHI effects");

	ApplyFullSceneSnapshotRequest rangeMismatch = fullSnapshotRequest(fence);
	setSingleKtxTexture(
		rangeMismatch, 8u, 8u, 2u,
		makeBc7Ktx2(8u, 8u, validMipBytes));
	rangeMismatch.candidateAsset.textures[0].payloadOffset = 1u;
	rangeMismatch.candidateAsset.textures[0].payloadSize -= 1u;
	requireMalformedTexturePlanHasZeroRhiEffects(
		std::move(rangeMismatch),
		"plan/asset KTX payload-range mismatch produced RHI effects");
}

void testValidKtxTextureCopyAndBarrierContract()
{
	auto device = std::make_unique<TransactionFakeDevice>();
	TransactionFakeDevice* fake = device.get();
	RendererFacade facade(std::move(device));
	const RendererTargetFence initialFence = baseFence();
	require(facade.initializeSceneTransactions(initialState(initialFence)).initialized,
	        "valid KTX transaction fixture initialization failed");
	const std::array<uint64_t, 2> mipBytes{{64u, 16u}};
	ApplyFullSceneSnapshotRequest request = fullSnapshotRequest(initialFence);
	setSingleKtxTexture(
		request, 8u, 8u, 2u, makeBc7Ktx2(8u, 8u, mipBytes));
	uint64_t visibleDuringSubmit = UINT64_MAX;
	fake->graphics.onSubmit = [&]
	{
		visibleDuringSubmit = facade.sceneTransactionVisibleGeneration();
	};
	const uint64_t visibleBefore = facade.sceneTransactionVisibleGeneration();
	const SceneApplyResult result = facade.applyFullSceneSnapshot(request);
	require(result.disposition == SceneApplyDisposition::AcceptedFullSnapshot &&
	        result.error == SceneApplyError::None &&
	        result.queueSubmissionTokens.size() == 1u &&
	        fake->graphics.submissions.size() == 1u &&
	        result.queueSubmissionTokens[0] == fake->graphics.submissions[0] &&
	        visibleDuringSubmit == visibleBefore &&
	        facade.sceneTransactionVisibleGeneration() == visibleBefore + 1u,
	        "valid KTX transaction did not preserve exact-token atomic publication");

	const auto texture = std::find_if(
		fake->capture.textures.begin(), fake->capture.textures.end(),
		[](const auto& captured)
		{
			return captured.second.format == rhi::TextureFormat::bc7UnormBlock;
		});
	require(texture != fake->capture.textures.end(),
	        "valid KTX transaction did not create its BC7 texture");
	const rhi::TextureHandle textureHandle = texture->first;
	const rhi::TextureDesc& textureDesc = texture->second;
	const auto view = std::find_if(
		fake->capture.views.begin(), fake->capture.views.end(),
		[&](const auto& captured) { return captured.second.image == textureHandle; });
	require(view != fake->capture.views.end() &&
	        view->second.aspect == rhi::TextureAspect::color &&
	        view->second.levelCount == 2u,
	        "valid KTX texture view lost color-aspect mip coverage");
	const bool descriptorPublished = std::any_of(
		fake->capture.descriptorWrites.begin(), fake->capture.descriptorWrites.end(),
		[&](const TransactionFakeDescriptorWrite& captured)
		{
			return captured.write.type == rhi::ArgumentType::combinedImageSampler &&
			       captured.write.textureView == view->first;
		});
	require(descriptorPublished,
	        "valid KTX texture view was not written to the private descriptor table");

	std::vector<size_t> textureCopyIndices;
	for (size_t index = 0u; index < fake->capture.textureCopies.size(); ++index)
	{
		if (fake->capture.textureCopies[index].texture == textureHandle)
		{
			textureCopyIndices.push_back(index);
		}
	}
	require(textureCopyIndices.size() == 2u,
	        "valid two-mip KTX transaction did not record exactly two copies");
	uint64_t previousEnd = 0u;
	for (size_t ordinal = 0u; ordinal < textureCopyIndices.size(); ++ordinal)
	{
		const rhi::BufferTextureCopyDesc& copy =
			fake->capture.textureCopies[textureCopyIndices[ordinal]];
		const auto buffer = std::find_if(
			fake->capture.buffers.begin(), fake->capture.buffers.end(),
			[&](const auto& captured) { return captured.first == copy.buffer; });
		require(buffer != fake->capture.buffers.end(),
		        "valid KTX copy did not reference a captured staging buffer");
		const TextureUploadValidationResult footprint = validateTextureUploadRegion(
			textureDesc, copy, 0u, buffer->second.size, buffer->second.size);
		require(footprint.valid() && footprint.requiredBytes == mipBytes[ordinal] &&
		        copy.bufferOffset >= previousEnd,
		        "valid KTX mip copy escaped or overlapped its staging subrange");
		previousEnd = copy.bufferOffset + footprint.requiredBytes;
	}

	std::optional<size_t> beginBarrierEvent;
	std::optional<size_t> firstCopyEvent;
	std::optional<size_t> lastCopyEvent;
	std::optional<size_t> endBarrierEvent;
	for (size_t eventIndex = 0u; eventIndex < fake->capture.events.size(); ++eventIndex)
	{
		const TransactionFakeEvent& event = fake->capture.events[eventIndex];
		if (event.kind == TransactionFakeEventKind::textureBarrier)
		{
			const rhi::TextureBarrier& barrier = fake->capture.textureBarriers[event.index];
			if (barrier.texture != textureHandle) continue;
			if (barrier.before == rhi::ResourceState::Undefined &&
				barrier.after == rhi::ResourceState::TransferDst &&
				barrier.range.aspect == rhi::TextureAspect::color &&
				barrier.range.levelCount == 2u)
			{
				beginBarrierEvent = eventIndex;
			}
			if (barrier.before == rhi::ResourceState::TransferDst &&
				barrier.after == rhi::ResourceState::ShaderRead &&
				barrier.range.aspect == rhi::TextureAspect::color &&
				barrier.range.levelCount == 2u)
			{
				endBarrierEvent = eventIndex;
			}
		}
		else if (event.kind == TransactionFakeEventKind::textureCopy &&
		         fake->capture.textureCopies[event.index].texture == textureHandle)
		{
			if (!firstCopyEvent) firstCopyEvent = eventIndex;
			lastCopyEvent = eventIndex;
		}
	}
	require(beginBarrierEvent && firstCopyEvent && lastCopyEvent && endBarrierEvent &&
	        *beginBarrierEvent < *firstCopyEvent &&
	        *lastCopyEvent < *endBarrierEvent &&
	        fake->waitIdleCount == 0u && fake->graphics.waitIdleCount == 0u,
	        "valid KTX command order did not preserve color upload barriers or zero idle");
}

void testMetalRejectsBeforeMutation()
{
	RendererFacade invalidFacade(rhi::BackendType::metal);
	const RendererSceneTransactionInitializationResult invalid =
		invalidFacade.initializeSceneTransactions(initialState(baseFence(
			rhi::BackendType::metal,
			RendererSceneUpdateCapability::IncrementalAndSnapshot)));
	require(!invalid.initialized &&
	        invalid.error == SceneApplyError::UnsupportedCapability,
	        "unverified Metal capability was accepted at initialization");

	const RendererTargetFence unsupportedFence = baseFence(
		rhi::BackendType::metal,
		RendererSceneUpdateCapability::Unsupported);
	RendererFacade facade(rhi::BackendType::metal);
	require(facade.initializeSceneTransactions(initialState(unsupportedFence)).initialized,
	        "unsupported Metal fence could not initialize immutable state");
	const RendererSceneStateSnapshot before = facade.activeSceneState();
	const SceneApplyResult result =
		facade.applySceneChanges(incrementalRequest(unsupportedFence, 70u));
	require(result.disposition == SceneApplyDisposition::Rejected &&
	        result.error == SceneApplyError::UnsupportedCapability &&
	        facade.activeSceneState() == before,
	        "Metal update did not reject before mutation");
}

void testConcreteMappingProvenanceGuards()
{
	using InitialMutation =
		std::function<void(RendererSceneTransactionInitialState&)>;
	const auto mutateInitialEntry = [](
		RendererSceneTransactionInitialState& initial,
		StableSceneId sourceId,
		const std::function<void(RendererSceneMappingEntry&)>& mutate)
	{
		initial.publication.mapping = mutateOuterEntry(
			initial.publication.mapping, sourceId, mutate);
	};
	const std::vector<std::pair<const char*, InitialMutation>> initializationCases{
		{"forged initialization handle generation had observable effects",
		 [=](RendererSceneTransactionInitialState& initial)
		 {
			 mutateInitialEntry(initial, nodeId(initial.publication.fence.authority),
			                    [](RendererSceneMappingEntry& entry)
			                    {
				                    ++entry.rendererHandle.generation;
			                    });
		 }},
		{"forged initialization handle index had observable effects",
		 [=](RendererSceneTransactionInitialState& initial)
		 {
			 mutateInitialEntry(initial, nodeId(initial.publication.fence.authority),
			                    [](RendererSceneMappingEntry& entry)
			                    {
				                    entry.rendererHandle.index += 1000u;
			                    });
		 }},
		{"forged initialization handle kind had observable effects",
		 [=](RendererSceneTransactionInitialState& initial)
		 {
			 mutateInitialEntry(initial, nodeId(initial.publication.fence.authority),
			                    [](RendererSceneMappingEntry& entry)
			                    {
				                    entry.rendererHandle.kind = RendererObjectKind::Camera;
				                    entry.rendererHandle.index = 1000u;
			                    });
		 }},
		{"forged initialization renderer generation had observable effects",
		 [=](RendererSceneTransactionInitialState& initial)
		 {
			 mutateInitialEntry(initial, nodeId(initial.publication.fence.authority),
			                    [](RendererSceneMappingEntry& entry)
			                    {
				                    ++entry.rendererHandle.rendererGeneration;
			                    });
		 }},
		{"forged initialization device generation had observable effects",
		 [=](RendererSceneTransactionInitialState& initial)
		 {
			 mutateInitialEntry(initial, nodeId(initial.publication.fence.authority),
			                    [](RendererSceneMappingEntry& entry)
			                    {
				                    ++entry.rendererHandle.deviceGeneration;
			                    });
		 }},
		{"forged initialization source generation had observable effects",
		 [=](RendererSceneTransactionInitialState& initial)
		 {
			 mutateInitialEntry(initial, nodeId(initial.publication.fence.authority),
			                    [](RendererSceneMappingEntry& entry)
			                    {
				                    ++entry.stableId.generation;
			                    });
		 }},
		{"missing initialization concrete record had observable effects",
		 [](RendererSceneTransactionInitialState& initial)
		 {
			 initial.publication.mapping = withoutOuterRecord(
				 initial.publication.mapping,
				 lightId(initial.publication.fence.authority));
		 }},
		{"ambiguous initialization concrete record had observable effects",
		 [](RendererSceneTransactionInitialState& initial)
		 {
			 const RendererTargetFence fence = initial.publication.fence;
			 initial.publication.mapping = outerMappingMutation(
				 initial.publication.mapping,
				 [&](std::vector<RendererSceneMappingEntry>& entries)
				 {
					 const StableSceneId forged{
						 EditorObjectKind::scene, fence.authority, 99u, 1u};
					 const uint64_t identity =
						 SceneRuntimeMapping::deriveProjectionIdentity(
							 editorObjectId(forged));
					 entries.push_back(RendererSceneMappingEntry{
						 .stableId = forged,
						 .projection = {
							 .kind = forged.kind,
							 .sceneAssetIndex = forged.slot,
							 .runtimeIndex = identity,
							 .identityDigest = projectionIdentityDigest(
								 forged, identity),
						 },
						 .rendererHandle = {
							 .kind = RendererObjectKind::SceneObject,
							 .index = 1000u,
							 .generation = 1u,
							 .rendererGeneration = fence.renderer.generation,
							 .deviceGeneration = fence.device.generation,
						 },
					 });
				 });
		 }},
	};
	for (const auto& [message, mutate] : initializationCases)
	{
		auto device = std::make_unique<TransactionFakeDevice>();
		TransactionFakeDevice* fake = device.get();
		RendererFacade facade(std::move(device));
		RendererSceneTransactionInitialState initial = initialState(baseFence());
		mutate(initial);
		const RendererTransactionObservation before =
			observeRendererTransaction(facade, *fake);
		const RendererSceneTransactionInitializationResult result =
			facade.initializeSceneTransactions(std::move(initial));
		const RendererTransactionObservation after =
			observeRendererTransaction(facade, *fake);
		require(!result.initialized &&
		        result.error == SceneApplyError::StaleObjectGeneration &&
		        exactObservationEqual(before, after), message);
	}
	const RendererTargetFence coherentBase = baseFence();
	const std::array<std::pair<StableSceneId, StableSceneId>, 2u>
		coherentAssociationPairs{{
			{cameraId(coherentBase.authority),
			 alternateCameraId(coherentBase.authority)},
			{lightId(coherentBase.authority),
			 alternateLightId(coherentBase.authority)},
		}};
	for (const auto& [firstId, secondId] : coherentAssociationPairs)
	{
		auto device = std::make_unique<TransactionFakeDevice>();
		TransactionFakeDevice* fake = device.get();
		RendererFacade facade(std::move(device));
		RendererSceneTransactionInitialState initial =
			multiCameraLightInitialState(coherentBase);
		initial.publication.mapping =
			withCoherentlySwappedProjectionAuthority(
				initial.publication.mapping, firstId, secondId);
		const RendererTransactionObservation before =
			observeRendererTransaction(facade, *fake);
		const RendererSceneTransactionInitializationResult rejected =
			facade.initializeSceneTransactions(std::move(initial));
		const RendererTransactionObservation after =
			observeRendererTransaction(facade, *fake);
		require(!rejected.initialized &&
		        rejected.error == SceneApplyError::StaleObjectGeneration &&
		        exactObservationEqual(before, after),
		        firstId.kind == EditorObjectKind::camera
			        ? "coherent initialization camera swap had observable effects"
			        : "coherent initialization light swap had observable effects");
		const RendererSceneTransactionInitializationResult recovered =
			facade.initializeSceneTransactions(
				multiCameraLightInitialState(coherentBase));
		require(recovered.initialized && recovered.error == SceneApplyError::None,
		        "valid initialization did not recover after coherent swap rejection");
	}
	{
		auto resetDevice = std::make_unique<TransactionFakeDevice>();
		TransactionFakeDevice* resetFake = resetDevice.get();
		RendererFacade resetFacade(std::move(resetDevice));
		const RendererTargetFence resetFence = baseFence();
		require(resetFacade.initializeSceneTransactions(
		            initialState(resetFence)).initialized,
		        "renderer-generation reset control could not initialize");
		resetFacade.resetSceneTransactions();
		const RendererTransactionObservation beforeStaleReset =
			observeRendererTransaction(resetFacade, *resetFake);
		const RendererSceneTransactionInitializationResult staleReset =
			resetFacade.initializeSceneTransactions(initialState(resetFence));
		const RendererTransactionObservation afterStaleReset =
			observeRendererTransaction(resetFacade, *resetFake);
		require(!staleReset.initialized &&
		        staleReset.error == SceneApplyError::StaleObjectGeneration &&
		        exactObservationEqual(beforeStaleReset, afterStaleReset),
		        "pre-reset concrete generations were accepted after lifecycle reset");
		RendererSceneTransactionInitialState currentGeneration =
			initialState(resetFence);
		currentGeneration.publication.mapping = outerMappingMutation(
			currentGeneration.publication.mapping,
			[](std::vector<RendererSceneMappingEntry>& entries)
			{
				for (RendererSceneMappingEntry& entry : entries)
				{
					entry.rendererHandle.generation = 2u;
				}
			});
		require(resetFacade.initializeSceneTransactions(
		            std::move(currentGeneration)).initialized,
		        "renderer-owned post-reset concrete generation was rejected");
	}

	auto device = std::make_unique<TransactionFakeDevice>();
	TransactionFakeDevice* fake = device.get();
	RendererFacade facade(std::move(device));
	RendererTargetFence current = baseFence();
	require(facade.initializeSceneTransactions(initialState(current)).initialized,
	        "provenance snapshot fixture initialization failed");
	const auto requireSnapshotRejected = [&](ApplyFullSceneSnapshotRequest request,
	                                         const char* message)
	{
		const RendererTransactionObservation before =
			observeRendererTransaction(facade, *fake);
		const SceneApplyResult result = facade.applyFullSceneSnapshot(request);
		const RendererTransactionObservation after =
			observeRendererTransaction(facade, *fake);
		require(result.disposition == SceneApplyDisposition::Rejected &&
		        result.error == SceneApplyError::StaleObjectGeneration &&
		        sceneDeltaCounterSnapshotIsZero(result.counterDelta) &&
		        exactObservationEqual(before, after), message);
	};
	const auto forgedSnapshotEntry = [&](
		StableSceneId sourceId,
		const std::function<void(RendererSceneMappingEntry&)>& mutate)
	{
		ApplyFullSceneSnapshotRequest request = fullSnapshotRequest(current);
		request.transaction.targetMapping = mutateOuterEntry(
			request.transaction.targetMapping, sourceId, mutate);
		return request;
	};
	requireSnapshotRejected(
		forgedSnapshotEntry(nodeId(current.authority),
		                    [](RendererSceneMappingEntry& entry)
		                    {
			                    ++entry.rendererHandle.generation;
		                    }),
		"forged full-snapshot handle generation had observable effects");
	requireSnapshotRejected(
		forgedSnapshotEntry(nodeId(current.authority),
		                    [](RendererSceneMappingEntry& entry)
		                    {
			                    entry.rendererHandle.index += 1000u;
		                    }),
		"forged full-snapshot handle index had observable effects");
	requireSnapshotRejected(
		forgedSnapshotEntry(nodeId(current.authority),
		                    [](RendererSceneMappingEntry& entry)
		                    {
			                    entry.rendererHandle.kind = RendererObjectKind::Camera;
			                    entry.rendererHandle.index = 1000u;
		                    }),
		"forged full-snapshot handle kind had observable effects");
	requireSnapshotRejected(
		forgedSnapshotEntry(nodeId(current.authority),
		                    [](RendererSceneMappingEntry& entry)
		                    {
			                    ++entry.stableId.generation;
		                    }),
		"forged full-snapshot source generation had observable effects");
	{
		ApplyFullSceneSnapshotRequest request = fullSnapshotRequest(current);
		request.transaction.targetMapping = withoutOuterRecord(
			request.transaction.targetMapping, cameraId(current.authority));
		requireSnapshotRejected(
			std::move(request),
			"missing full-snapshot concrete record had observable effects");
	}
	{
		ApplyFullSceneSnapshotRequest request = fullSnapshotRequest(current);
		const RendererSceneMappingSnapshot& mapping =
			request.transaction.targetMapping;
		request.transaction.targetMapping = RendererSceneMappingSnapshot{
			mapping.fence(),
			std::vector<RendererSceneMappingEntry>(
				mapping.entries().begin(), mapping.entries().end())};
		requireSnapshotRejected(
			std::move(request),
			"full snapshot without projection authority had observable effects");
	}
	{
		ApplyFullSceneSnapshotRequest request = fullSnapshotRequest(current);
		request.transaction.targetMapping = withExtraneousConcreteRecord(
			request.transaction.targetMapping);
		requireSnapshotRejected(
			std::move(request),
			"ambiguous full-snapshot concrete record had observable effects");
	}
	{
		ApplyFullSceneSnapshotRequest request = fullSnapshotRequest(current);
		const RendererTargetFence target = request.transaction.targetMapping.fence();
		const RendererSceneMappingSnapshot complete = unusedConcreteMappingFor(
			target, facade.sceneTransactionConcreteState().mapping);
		request.candidateAsset = unusedConcreteSceneAssetFixture(
			target.appliedDocumentRevision, target.authority);
		request.transaction.targetMapping = withoutOuterRecord(
			complete, alternateNodeId(target.authority));
		requireSnapshotRejected(
			std::move(request),
			"missing unused node mapping had observable effects");
	}
	{
		ApplyFullSceneSnapshotRequest request = fullSnapshotRequest(current);
		const RendererTargetFence target = request.transaction.targetMapping.fence();
		const RendererSceneMappingSnapshot complete = unusedConcreteMappingFor(
			target, facade.sceneTransactionConcreteState().mapping);
		request.candidateAsset = unusedConcreteSceneAssetFixture(
			target.appliedDocumentRevision, target.authority);
		request.transaction.targetMapping = withoutOuterRecord(
			complete, alternatePrimitiveId(target.authority));
		requireSnapshotRejected(
			std::move(request),
			"missing unused primitive mapping had observable effects");
	}

	ApplyFullSceneSnapshotRequest acceptedSnapshot = fullSnapshotRequest(current);
	const SceneApplyResult accepted =
		facade.applyFullSceneSnapshot(acceptedSnapshot);
	require(accepted.disposition == SceneApplyDisposition::AcceptedFullSnapshot &&
	        accepted.error == SceneApplyError::None && accepted.resultingFence,
	        "valid provenance full-snapshot control was rejected");
	current = *accepted.resultingFence;
	ApplyFullSceneSnapshotRequest forgedReplay = acceptedSnapshot;
	forgedReplay.transaction.targetMapping = withForgedConcreteGeneration(
		forgedReplay.transaction.targetMapping, nodeId(current.authority));
	requireSnapshotRejected(
		std::move(forgedReplay),
		"forged canonical full-snapshot replay bypassed provenance validation");

	ApplySceneChangeRequest acceptedIncremental =
		incrementalRequest(current, 910u);
	const SceneApplyResult incrementalAccepted =
		facade.applySceneChanges(acceptedIncremental);
	require(incrementalAccepted.disposition ==
	            SceneApplyDisposition::AcceptedIncremental &&
	        incrementalAccepted.error == SceneApplyError::None &&
	        incrementalAccepted.resultingFence,
	        "valid provenance incremental control was rejected");
	current = *incrementalAccepted.resultingFence;
	const auto requireIncrementalRejected = [&](ApplySceneChangeRequest request,
	                                            const char* message)
	{
		const RendererTransactionObservation before =
			observeRendererTransaction(facade, *fake);
		const SceneApplyResult result = facade.applySceneChanges(request);
		const RendererTransactionObservation after =
			observeRendererTransaction(facade, *fake);
		require(result.disposition == SceneApplyDisposition::Rejected &&
		        result.error == SceneApplyError::StaleObjectGeneration &&
		        sceneDeltaCounterSnapshotIsZero(result.counterDelta) &&
		        exactObservationEqual(before, after), message);
	};
	ApplySceneChangeRequest forgedIncrementalReplay = acceptedIncremental;
	forgedIncrementalReplay.targetMapping = withForgedConcreteGeneration(
		forgedIncrementalReplay.targetMapping, lightId(current.authority));
	requireIncrementalRejected(
		std::move(forgedIncrementalReplay),
		"unchanged light forgery bypassed canonical incremental replay validation");
	ApplySceneChangeRequest unchangedForgery = incrementalRequest(current, 911u);
	unchangedForgery.targetMapping = mutateOuterEntry(
		unchangedForgery.targetMapping, lightId(current.authority),
		[](RendererSceneMappingEntry& entry)
		{
			entry.rendererHandle.index += 1000u;
		});
	requireIncrementalRejected(
		std::move(unchangedForgery),
		"unchanged-entry forgery with a different changed object had observable effects");

	auto multiDevice = std::make_unique<TransactionFakeDevice>();
	TransactionFakeDevice* multiFake = multiDevice.get();
	RendererFacade multiFacade(std::move(multiDevice));
	const RendererTargetFence multiBase = baseFence();
	require(multiFacade.initializeSceneTransactions(
	            multiCameraLightInitialState(multiBase)).initialized,
	        "multi-camera/light provenance fixture initialization failed");
	const auto requireMultiSnapshotRejected = [&](ApplyFullSceneSnapshotRequest request,
	                                              const char* message)
	{
		const RendererTransactionObservation before =
			observeRendererTransaction(multiFacade, *multiFake);
		const SceneApplyResult result = multiFacade.applyFullSceneSnapshot(request);
		const RendererTransactionObservation after =
			observeRendererTransaction(multiFacade, *multiFake);
		require(result.disposition == SceneApplyDisposition::Rejected &&
		        result.error == SceneApplyError::StaleObjectGeneration &&
		        sceneDeltaCounterSnapshotIsZero(result.counterDelta) &&
		        exactObservationEqual(before, after), message);
	};
	{
		ApplyFullSceneSnapshotRequest request =
			multiCameraLightFullSnapshotRequest(multiBase);
		request.transaction.targetMapping = withSwappedOuterAssociations(
			request.transaction.targetMapping,
			cameraId(multiBase.authority),
			alternateCameraId(multiBase.authority));
		requireMultiSnapshotRejected(
			std::move(request),
			"multi-camera source-to-asset swap had observable effects");
	}
	{
		ApplyFullSceneSnapshotRequest request =
			multiCameraLightFullSnapshotRequest(multiBase);
		request.transaction.targetMapping = withSwappedOuterAssociations(
			request.transaction.targetMapping,
			lightId(multiBase.authority),
			alternateLightId(multiBase.authority));
		requireMultiSnapshotRejected(
			std::move(request),
			"multi-light source-to-asset swap had observable effects");
	}
	for (const auto& [firstId, secondId] : coherentAssociationPairs)
	{
		ApplyFullSceneSnapshotRequest request =
			multiCameraLightFullSnapshotRequest(multiBase);
		request.transaction.targetMapping =
			withCoherentlySwappedProjectionAuthority(
				request.transaction.targetMapping, firstId, secondId);
		requireMultiSnapshotRejected(
			std::move(request),
			firstId.kind == EditorObjectKind::camera
				? "coherent full-snapshot camera swap had observable effects"
				: "coherent full-snapshot light swap had observable effects");
	}
	ApplyFullSceneSnapshotRequest canonicalControl =
		multiCameraLightFullSnapshotRequest(multiBase);
	const SceneApplyResult multiAccepted =
		multiFacade.applyFullSceneSnapshot(canonicalControl);
	const SceneTransactionConcreteStateSnapshot multiConcrete =
		multiFacade.sceneTransactionConcreteState();
	require(multiAccepted.disposition ==
	            SceneApplyDisposition::AcceptedFullSnapshot &&
	        multiAccepted.error == SceneApplyError::None &&
	        multiAccepted.resultingFence &&
	        multiConcrete.cameraHandles.size() == 2u &&
	        multiConcrete.lightHandles.size() == 2u,
	        "valid multi-camera/light provenance control was rejected");
	const RendererTransactionObservation beforeExactReplay =
		observeRendererTransaction(multiFacade, *multiFake);
	const SceneApplyResult exactReplay =
		multiFacade.applyFullSceneSnapshot(canonicalControl);
	const RendererTransactionObservation afterExactReplay =
		observeRendererTransaction(multiFacade, *multiFake);
	require(exactReplay.disposition == SceneApplyDisposition::AlreadyApplied &&
	        exactReplay.error == SceneApplyError::None &&
	        sceneDeltaCounterSnapshotIsZero(exactReplay.counterDelta) &&
	        exactObservationEqual(beforeExactReplay, afterExactReplay),
	        "valid canonical full-snapshot replay changed observable state");
	for (const auto& [firstId, secondId] : coherentAssociationPairs)
	{
		ApplyFullSceneSnapshotRequest forgedReplay = canonicalControl;
		forgedReplay.transaction.targetMapping =
			withCoherentlySwappedProjectionAuthority(
				forgedReplay.transaction.targetMapping, firstId, secondId);
		requireMultiSnapshotRejected(
			std::move(forgedReplay),
			firstId.kind == EditorObjectKind::camera
				? "coherent canonical camera replay bypassed source identity"
				: "coherent canonical light replay bypassed source identity");
	}
	const RendererTransactionObservation beforeRecoveredReplay =
		observeRendererTransaction(multiFacade, *multiFake);
	const SceneApplyResult recoveredReplay =
		multiFacade.applyFullSceneSnapshot(canonicalControl);
	const RendererTransactionObservation afterRecoveredReplay =
		observeRendererTransaction(multiFacade, *multiFake);
	require(recoveredReplay.disposition == SceneApplyDisposition::AlreadyApplied &&
	        recoveredReplay.error == SceneApplyError::None &&
	        sceneDeltaCounterSnapshotIsZero(recoveredReplay.counterDelta) &&
	        exactObservationEqual(beforeRecoveredReplay, afterRecoveredReplay),
	        "valid canonical replay did not recover after coherent swap rejection");
	ApplyFullSceneSnapshotRequest recoveryUpdate =
		multiCameraLightFullSnapshotRequest(
			*multiAccepted.resultingFence, &multiConcrete.mapping);
	const SceneApplyResult recoveryApplied =
		multiFacade.applyFullSceneSnapshot(recoveryUpdate);
	require(recoveryApplied.disposition ==
	            SceneApplyDisposition::AcceptedFullSnapshot &&
	        recoveryApplied.error == SceneApplyError::None,
	        "valid canonical update did not apply after coherent swap rejection");
}

void testLiveResourceSnapshotAndCpuCandidatePublication()
{
	auto device = std::make_unique<TransactionFakeDevice>();
	TransactionFakeDevice* fake = device.get();
	RendererFacade facade(std::move(device));
	RendererTargetFence current = baseFence();
	require(facade.initializeSceneTransactions(initialState(current)).initialized,
	        "live transaction fake device initialization failed");
	const uint64_t initialVisibleGeneration =
		facade.sceneTransactionVisibleGeneration();
	uint64_t generationObservedDuringSubmit = 0u;
	fake->graphics.onSubmit = [&]
	{
		generationObservedDuringSubmit = facade.sceneTransactionVisibleGeneration();
	};

	ApplyFullSceneSnapshotRequest unsupportedQueue = fullSnapshotRequest(current);
	unsupportedQueue.transaction.stagePlan.submissions = {
		{rhi::QueueClass::transfer, 699u},
	};
	const RendererSceneStateSnapshot beforeUnsupportedQueue = facade.activeSceneState();
	const uint64_t createdBeforeUnsupportedQueue = fake->createdCount;
	const uint64_t destroyedBeforeUnsupportedQueue = fake->destroyedCount;
	const size_t submissionsBeforeUnsupportedQueue = fake->graphics.submissions.size();
	const SceneApplyResult unsupportedQueueResult =
		facade.applyFullSceneSnapshot(unsupportedQueue);
	require(unsupportedQueueResult.disposition == SceneApplyDisposition::Rejected &&
	        unsupportedQueueResult.error == SceneApplyError::InvalidPayload &&
	        facade.activeSceneState() == beforeUnsupportedQueue &&
	        fake->createdCount == createdBeforeUnsupportedQueue &&
	        fake->destroyedCount == destroyedBeforeUnsupportedQueue &&
	        fake->graphics.submissions.size() == submissionsBeforeUnsupportedQueue,
	        "non-graphics scene submission was not rejected before RHI effects");

	ApplyFullSceneSnapshotRequest snapshot = fullSnapshotRequest(current);
	const SceneApplyResult published = facade.applyFullSceneSnapshot(snapshot);
	require(published.disposition == SceneApplyDisposition::AcceptedFullSnapshot &&
	        published.error == SceneApplyError::None && published.resultingFence &&
	        published.queueSubmissionTokens.size() == 1u &&
	        fake->graphics.submissions.size() == 1u &&
	        published.queueSubmissionTokens.front() == fake->graphics.submissions.front(),
	        "live resource snapshot did not return the exact queue token");
	require(generationObservedDuringSubmit == initialVisibleGeneration &&
	        facade.sceneTransactionVisibleGeneration() == initialVisibleGeneration + 1u,
	        "candidate generation became visible before atomic publication");
	SceneTransactionConcreteStateSnapshot concrete =
		facade.sceneTransactionConcreteState();
	require(concrete.initialized && concrete.resourceBearing &&
	        concrete.resourceCount >= 10u && concrete.meshHandles.size() == 1u &&
	        concrete.materialHandles.size() == 1u &&
	        concrete.textureHandles.size() >= 1u &&
	        concrete.sceneObjectHandles.size() == 4u &&
	        concrete.cameraHandles.size() == 1u &&
	        concrete.lightHandles.size() == 1u &&
	        concrete.mapping.validate().valid() &&
	        fake->descriptorWriteCount >= 1u && fake->waitIdleCount == 0u,
	        "published resource generation lacks private mesh/material/texture/descriptor state");
	const SceneDeltaCounterSnapshot countersBeforeReplay = facade.sceneDeltaCounters();
	const uint64_t createdBeforeReplay = fake->createdCount;
	const uint64_t destroyedBeforeReplay = fake->destroyedCount;
	const size_t submissionsBeforeReplay = fake->graphics.submissions.size();
	const SceneApplyResult replay = facade.applyFullSceneSnapshot(snapshot);
	require(replay.disposition == SceneApplyDisposition::AlreadyApplied &&
	        sceneDeltaCounterSnapshotIsZero(replay.counterDelta) &&
	        facade.sceneDeltaCounters() == countersBeforeReplay &&
	        fake->createdCount == createdBeforeReplay &&
	        fake->destroyedCount == destroyedBeforeReplay &&
	        fake->graphics.submissions.size() == submissionsBeforeReplay,
	        "exact live snapshot replay mutated counters, RHI, mapping, or journals");
	ApplyFullSceneSnapshotRequest collision = snapshot;
	collision.transaction.canonicalBytes.back() ^= 0x40u;
	const SceneApplyResult collisionResult = facade.applyFullSceneSnapshot(collision);
	require(collisionResult.disposition == SceneApplyDisposition::Rejected &&
	        collisionResult.error == SceneApplyError::InvalidState &&
	        facade.sceneDeltaCounters() == countersBeforeReplay &&
	        fake->createdCount == createdBeforeReplay &&
	        fake->destroyedCount == destroyedBeforeReplay &&
	        fake->graphics.submissions.size() == submissionsBeforeReplay,
	        "canonical identity collision produced observable effects");
	current = *published.resultingFence;

	const auto requireForgedHandleRejected =
		[&](ApplySceneChangeRequest request,
		     StableSceneId sourceId,
		     const char* message)
		{
			request.targetMapping = withForgedConcreteGeneration(
				request.targetMapping, sourceId);
			const RendererTransactionObservation before =
				observeRendererTransaction(facade, *fake);
			const SceneApplyResult rejected = facade.applySceneChanges(request);
			const RendererTransactionObservation after =
				observeRendererTransaction(facade, *fake);
			require(rejected.disposition == SceneApplyDisposition::Rejected &&
			        rejected.error == SceneApplyError::StaleObjectGeneration &&
			        sceneDeltaCounterSnapshotIsZero(rejected.counterDelta) &&
			        exactObservationEqual(before, after), message);
		};

	requireForgedHandleRejected(
		incrementalRequest(current, 495u),
		nodeId(current.authority),
		"fence-valid stale scene-object generation had observable effects");
	SceneCameraValue staleCameraBefore;
	SceneCameraValue staleCameraAfter = staleCameraBefore;
	staleCameraAfter.yfov = 0.9;
	requireForgedHandleRejected(
		incrementalRequest(current, 496u, {SceneFieldChange{
			.target = cameraId(current.authority),
			.changeClass = SceneChangeClass::Camera,
			.action = SceneChangeAction::Update,
			.field = SceneField::CameraParameters,
			.mutationSerial = 496u,
			.beforeValue = SceneFieldValue{staleCameraBefore},
			.afterValue = SceneFieldValue{staleCameraAfter},
		}}),
		cameraId(current.authority),
		"fence-valid stale camera generation had observable effects");
	SceneLightValue staleLightBefore;
	SceneLightValue staleLightAfter = staleLightBefore;
	staleLightAfter.intensity = 2.0;
	requireForgedHandleRejected(
		incrementalRequest(current, 497u, {SceneFieldChange{
			.target = lightId(current.authority),
			.changeClass = SceneChangeClass::Light,
			.action = SceneChangeAction::Update,
			.field = SceneField::LightParameters,
			.mutationSerial = 497u,
			.beforeValue = SceneFieldValue{staleLightBefore},
			.afterValue = SceneFieldValue{staleLightAfter},
		}}),
		lightId(current.authority),
		"fence-valid stale light generation had observable effects");

	ApplySceneChangeRequest invalidIncrementalSubmission =
		incrementalRequest(current, 499u);
	invalidIncrementalSubmission.stagePlan.submissions = {
		{rhi::QueueClass::graphics, 698u},
	};
	const RendererSceneStateSnapshot beforeInvalidIncremental =
		facade.activeSceneState();
	const uint64_t descriptorWritesBeforeInvalidIncremental =
		fake->descriptorWriteCount;
	const uint64_t createdBeforeInvalidIncremental = fake->createdCount;
	const uint64_t destroyedBeforeInvalidIncremental = fake->destroyedCount;
	const size_t submissionsBeforeInvalidIncremental =
		fake->graphics.submissions.size();
	const SceneApplyResult invalidIncrementalResult =
		facade.applySceneChanges(invalidIncrementalSubmission);
	require(invalidIncrementalResult.disposition == SceneApplyDisposition::Rejected &&
	        invalidIncrementalResult.error == SceneApplyError::InvalidPayload &&
	        facade.activeSceneState() == beforeInvalidIncremental &&
	        fake->descriptorWriteCount == descriptorWritesBeforeInvalidIncremental &&
	        fake->createdCount == createdBeforeInvalidIncremental &&
	        fake->destroyedCount == destroyedBeforeInvalidIncremental &&
	        fake->graphics.submissions.size() == submissionsBeforeInvalidIncremental,
	        "incremental submission plan reached RHI staging");

	SceneNodeTransformValue transformBefore;
	SceneNodeTransformValue transformAfter;
	std::get<SceneTrsValue>(transformAfter.value).translation = {7.0, 2.0, -3.0};
	SceneVisibilityValue visibilityBefore{true};
	SceneVisibilityValue visibilityAfter{false};
	SceneMaterialParametersValue materialBefore;
	SceneMaterialParametersValue materialAfter;
	materialAfter.baseColorFactor = {0.2, 0.3, 0.4, 0.5};
	materialAfter.metallicFactor = 0.6;
	materialAfter.roughnessFactor = 0.25;
	materialAfter.doubleSided = true;
	SceneTextureBindingValue textureBindingBefore;
	textureBindingBefore.semantic = SceneTextureSemantic::Emissive;
	SceneTextureBindingValue textureBindingAfter = textureBindingBefore;
	textureBindingAfter.texture = textureId(current.authority);
	ScenePrimitiveMaterialBindingValue bindingBefore;
	bindingBefore.material = materialId(current.authority);
	ScenePrimitiveMaterialBindingValue bindingAfter;
	SceneCameraValue cameraBefore;
	SceneCameraValue cameraAfter;
	cameraAfter.projection = SceneCameraProjection::Orthographic;
	cameraAfter.xmag = 4.0;
	cameraAfter.ymag = 3.0;
	SceneLightValue lightBefore;
	SceneLightValue lightAfter;
	lightAfter.type = editor::SceneLightType::Spot;
	lightAfter.color = {0.1, 0.5, 0.9};
	lightAfter.intensity = 8.0;
	lightAfter.range = 12.0;
	lightAfter.innerConeAngle = 0.2;
	lightAfter.outerConeAngle = 0.7;
	const size_t submissionsBeforeCpuEdit = fake->graphics.submissions.size();
	const uint64_t descriptorWritesBeforeCpuEdit = fake->descriptorWriteCount;
	const uint64_t createdBeforeCpuEdit = fake->createdCount;
	const uint64_t destroyedBeforeCpuEdit = fake->destroyedCount;
	const SceneApplyResult cpuPublished = facade.applySceneChanges(
		incrementalRequest(current, 500u, {
			SceneFieldChange{
				.target = nodeId(current.authority),
				.changeClass = SceneChangeClass::Transform,
				.action = SceneChangeAction::Update,
				.field = SceneField::NodeLocalTransform,
				.mutationSerial = 500u,
				.beforeValue = SceneFieldValue{transformBefore},
				.afterValue = SceneFieldValue{transformAfter},
			},
			SceneFieldChange{
				.target = nodeId(current.authority),
				.changeClass = SceneChangeClass::Visibility,
				.action = SceneChangeAction::Update,
				.field = SceneField::NodeLocalVisibility,
				.mutationSerial = 500u,
				.beforeValue = SceneFieldValue{visibilityBefore},
				.afterValue = SceneFieldValue{visibilityAfter},
			},
			SceneFieldChange{
				.target = materialId(current.authority),
				.changeClass = SceneChangeClass::MaterialParameters,
				.action = SceneChangeAction::Update,
				.field = SceneField::MaterialParameters,
				.mutationSerial = 500u,
				.beforeValue = SceneFieldValue{materialBefore},
				.afterValue = SceneFieldValue{materialAfter},
			},
			SceneFieldChange{
				.target = materialId(current.authority),
				.changeClass = SceneChangeClass::MaterialBinding,
				.action = SceneChangeAction::Update,
				.field = SceneField::MaterialTextureBinding,
				.mutationSerial = 500u,
				.beforeValue = SceneFieldValue{textureBindingBefore},
				.afterValue = SceneFieldValue{textureBindingAfter},
			},
			SceneFieldChange{
				.target = primitiveId(current.authority),
				.changeClass = SceneChangeClass::MaterialBinding,
				.action = SceneChangeAction::Update,
				.field = SceneField::PrimitiveMaterialBinding,
				.mutationSerial = 500u,
				.beforeValue = SceneFieldValue{bindingBefore},
				.afterValue = SceneFieldValue{bindingAfter},
			},
			SceneFieldChange{
				.target = cameraId(current.authority),
				.changeClass = SceneChangeClass::Camera,
				.action = SceneChangeAction::Update,
				.field = SceneField::CameraParameters,
				.mutationSerial = 500u,
				.beforeValue = SceneFieldValue{cameraBefore},
				.afterValue = SceneFieldValue{cameraAfter},
			},
			SceneFieldChange{
				.target = lightId(current.authority),
				.changeClass = SceneChangeClass::Light,
				.action = SceneChangeAction::Update,
				.field = SceneField::LightParameters,
				.mutationSerial = 500u,
				.beforeValue = SceneFieldValue{lightBefore},
				.afterValue = SceneFieldValue{lightAfter},
			},
		}));
	requireZeroContinuousWork(cpuPublished,
	                          "live CPU candidate edit violated zero-upload/idle counters");
	concrete = facade.sceneTransactionConcreteState();
	require(fake->graphics.submissions.size() == submissionsBeforeCpuEdit &&
	        fake->descriptorWriteCount == descriptorWritesBeforeCpuEdit &&
	        fake->createdCount == createdBeforeCpuEdit &&
	        fake->destroyedCount == destroyedBeforeCpuEdit &&
	        concrete.firstNodeWorldTransform &&
	        (*concrete.firstNodeWorldTransform)[3][0] == 7.0f &&
	        concrete.firstNodeVisibility && !*concrete.firstNodeVisibility &&
	        concrete.firstMaterial &&
	        concrete.firstMaterial->roughnessFactor == 0.25f &&
	        concrete.firstMaterial->doubleSided &&
	        concrete.firstMaterial->emissiveTexture == 0 &&
	        concrete.firstPrimitiveMaterialIndex &&
	        *concrete.firstPrimitiveMaterialIndex == -1 &&
	        concrete.firstCamera &&
	        concrete.firstCamera->type == demo::SceneCameraType::orthographic &&
	        concrete.firstCamera->horizontalMagnification == 4.0f &&
	        concrete.firstLight &&
	        concrete.firstLight->type == demo::SceneLightType::spot &&
	        concrete.firstLight->intensity == 8.0f &&
	        concrete.rendererObjectCount == 1u &&
	        concrete.rendererDrawCount == 1u &&
	        concrete.deferredRegistryUpdateCount == 1u &&
	        concrete.firstRendererWorldTransform &&
	        (*concrete.firstRendererWorldTransform)[3][0] == 7.0f &&
	        concrete.firstRendererVisibility && !*concrete.firstRendererVisibility &&
	        concrete.firstRendererMaterialIndex &&
	        *concrete.firstRendererMaterialIndex == UINT32_MAX &&
	        concrete.firstPlanInstanceMaterialIndex &&
	        *concrete.firstPlanInstanceMaterialIndex == UINT32_MAX &&
	        concrete.firstPlanDrawMaterialIndex &&
	        *concrete.firstPlanDrawMaterialIndex == UINT32_MAX &&
	        concrete.rendererStateMatchesGeneration &&
	        fake->waitIdleCount == 0u,
	        "published CPU candidate records do not match the immutable mapping state");

	current = *cpuPublished.resultingFence;
	const uint64_t descriptorWritesBeforeRootEdit = fake->descriptorWriteCount;
	const uint64_t createdBeforeRootEdit = fake->createdCount;
	const uint64_t destroyedBeforeRootEdit = fake->destroyedCount;
	const size_t submissionsBeforeRootEdit = fake->graphics.submissions.size();
	const SceneApplyResult rootPublished = facade.applySceneChanges(
		identityTransitionRequest(current, SceneChangeDomain::SceneRootState, 501u));
	requireZeroContinuousWork(rootPublished,
	                          "scene-root transform violated zero-upload/idle counters");
	concrete = facade.sceneTransactionConcreteState();
	require(rootPublished.resultingFence &&
	        fake->descriptorWriteCount == descriptorWritesBeforeRootEdit &&
	        fake->createdCount == createdBeforeRootEdit &&
	        fake->destroyedCount == destroyedBeforeRootEdit &&
	        fake->graphics.submissions.size() == submissionsBeforeRootEdit &&
	        concrete.firstNodeWorldTransform &&
	        (*concrete.firstNodeWorldTransform)[3][0] == 7.0f &&
	        (*concrete.firstNodeWorldTransform)[3][1] == 4.0f &&
	        concrete.firstRendererWorldTransform &&
	        (*concrete.firstRendererWorldTransform)[3][0] == 7.0f &&
	        (*concrete.firstRendererWorldTransform)[3][1] == 4.0f &&
	        concrete.rendererStateMatchesGeneration,
	        "scene-root transform did not reach the concrete renderer generation");
	current = *rootPublished.resultingFence;
	const rhi::SubmissionToken replacedGenerationToken =
		published.queueSubmissionTokens.front();
	const uint64_t destroyedBeforeReplacement = fake->destroyedCount;
	const SceneApplyResult replacement =
		facade.applyFullSceneSnapshot(fullSnapshotRequest(current));
	require(replacement.disposition == SceneApplyDisposition::AcceptedFullSnapshot &&
	        replacement.queueSubmissionTokens.size() == 1u &&
	        fake->destroyedCount == destroyedBeforeReplacement &&
	        facade.sceneTransactionConcreteState().pendingRetiredGenerationCount >= 1u,
	        "replaced generation was destroyed before its exact queue token completed");
	const rhi::SubmissionToken replacementToken =
		replacement.queueSubmissionTokens.front();
	require(replacementToken != replacedGenerationToken &&
		        fake->graphics.lastSubmittedToken() == replacementToken,
		        "replacement publication did not expose its exact newer queue token");
	// Make the superseded generation complete, then submit a request that is
	// rejected during bridge plan validation. The rejection must not collect
	// the already-complete retirement or mutate any RHI/counter state.
	fake->graphics.completeThrough(replacedGenerationToken);
	const RendererSceneStateSnapshot beforeRejectedCompletedRetirement =
		facade.activeSceneState();
	const SceneTransactionConcreteStateSnapshot
		concreteBeforeRejectedCompletedRetirement =
			facade.sceneTransactionConcreteState();
	const SceneDeltaCounterSnapshot countersBeforeRejectedCompletedRetirement =
		facade.sceneDeltaCounters();
	const uint64_t createdBeforeRejectedCompletedRetirement = fake->createdCount;
	const uint64_t destroyedBeforeRejectedCompletedRetirement = fake->destroyedCount;
	const uint64_t descriptorBeforeRejectedCompletedRetirement =
		fake->descriptorWriteCount;
	const size_t submissionsBeforeRejectedCompletedRetirement =
		fake->graphics.submissions.size();
	const std::vector<rhi::SubmissionToken>
		completionQueriesBeforeRejectedCompletedRetirement =
			fake->graphics.completionQueries;
	const uint64_t deviceIdleBeforeRejectedCompletedRetirement =
		fake->waitIdleCount;
	const uint64_t queueIdleBeforeRejectedCompletedRetirement =
		fake->graphics.waitIdleCount;
	ApplyFullSceneSnapshotRequest rejectedAfterCompletion =
		fullSnapshotRequest(*replacement.resultingFence);
	rejectedAfterCompletion.transaction.targetMapping =
		withForgedConcreteGeneration(
			rejectedAfterCompletion.transaction.targetMapping,
			nodeId(replacement.resultingFence->authority));
	const SceneApplyResult rejectedAfterCompletionResult =
		facade.applyFullSceneSnapshot(rejectedAfterCompletion);
	const SceneTransactionConcreteStateSnapshot
		concreteAfterRejectedCompletedRetirement =
			facade.sceneTransactionConcreteState();
	require(rejectedAfterCompletionResult.disposition ==
		            SceneApplyDisposition::Rejected &&
		        rejectedAfterCompletionResult.error ==
		            SceneApplyError::StaleObjectGeneration &&
		        sceneDeltaCounterSnapshotIsZero(
		            rejectedAfterCompletionResult.counterDelta) &&
		        facade.activeSceneState() == beforeRejectedCompletedRetirement &&
		        facade.sceneDeltaCounters() ==
		            countersBeforeRejectedCompletedRetirement &&
		        fake->createdCount == createdBeforeRejectedCompletedRetirement &&
		        fake->destroyedCount == destroyedBeforeRejectedCompletedRetirement &&
		        fake->descriptorWriteCount == descriptorBeforeRejectedCompletedRetirement &&
		        fake->graphics.submissions.size() ==
		            submissionsBeforeRejectedCompletedRetirement &&
		        fake->graphics.completionQueries ==
		            completionQueriesBeforeRejectedCompletedRetirement &&
		        fake->waitIdleCount == deviceIdleBeforeRejectedCompletedRetirement &&
		        fake->graphics.waitIdleCount == queueIdleBeforeRejectedCompletedRetirement &&
		        concreteAfterRejectedCompletedRetirement.generation ==
		            concreteBeforeRejectedCompletedRetirement.generation &&
		        concreteAfterRejectedCompletedRetirement.descriptorGeneration ==
		            concreteBeforeRejectedCompletedRetirement.descriptorGeneration &&
		        concreteAfterRejectedCompletedRetirement.resourceCount ==
		            concreteBeforeRejectedCompletedRetirement.resourceCount &&
		        concreteAfterRejectedCompletedRetirement.pendingRetiredGenerationCount ==
		            concreteBeforeRejectedCompletedRetirement.pendingRetiredGenerationCount &&
		        concreteAfterRejectedCompletedRetirement.immediatelyDestroyedResourceCount ==
		            concreteBeforeRejectedCompletedRetirement.immediatelyDestroyedResourceCount &&
		        concreteAfterRejectedCompletedRetirement.eventuallyDestroyedResourceCount ==
		            concreteBeforeRejectedCompletedRetirement.eventuallyDestroyedResourceCount &&
		        concreteAfterRejectedCompletedRetirement.queueSubmissionCount ==
		            concreteBeforeRejectedCompletedRetirement.queueSubmissionCount &&
		        concreteAfterRejectedCompletedRetirement.lastSubmissionToken ==
		            concreteBeforeRejectedCompletedRetirement.lastSubmissionToken &&
		        concreteAfterRejectedCompletedRetirement.fence ==
		            concreteBeforeRejectedCompletedRetirement.fence,
		        "completed retirement was drained before a rejected plan request");
	// A subsequent accepted request is the maintenance boundary and may now
	// retire the completed old generation while preserving the newer fence.
	const SceneApplyResult acceptedAfterRejection =
		facade.applyFullSceneSnapshot(
			fullSnapshotRequest(*replacement.resultingFence));
	require(acceptedAfterRejection.disposition ==
		            SceneApplyDisposition::AcceptedFullSnapshot &&
		        acceptedAfterRejection.queueSubmissionTokens.size() == 1u &&
		        fake->destroyedCount > destroyedBeforeRejectedCompletedRetirement &&
		        facade.activeSceneState().fence ==
		            acceptedAfterRejection.resultingFence,
		        "accepted request did not drain the previously completed retirement");
	const rhi::SubmissionToken acceptedAfterRejectionToken =
		acceptedAfterRejection.queueSubmissionTokens.front();
	fake->graphics.completionQueries.clear();
	require(facade.collectSceneTransactionGarbage() == 0u &&
	        std::find(fake->graphics.completionQueries.begin(),
	                  fake->graphics.completionQueries.end(),
	                  replacementToken) != fake->graphics.completionQueries.end() &&
	        std::find(fake->graphics.completionQueries.begin(),
	                  fake->graphics.completionQueries.end(),
	                  acceptedAfterRejectionToken) ==
	            fake->graphics.completionQueries.end(),
	        "later accepted retirement was not fenced by its replaced generation token");
	fake->graphics.completeThrough(replacementToken);
	const SceneDeltaCounterSnapshot countersBeforeCollection =
		facade.sceneDeltaCounters();
	const uint64_t collected = facade.collectSceneTransactionGarbage();
	const SceneDeltaCounterSnapshot countersAfterCollection =
		facade.sceneDeltaCounters();
	require(collected > 0u &&
	        !fake->graphics.isComplete(acceptedAfterRejectionToken) &&
	        fake->destroyedCount > destroyedBeforeReplacement &&
	        countersAfterCollection.resourcesPhysicallyDestroyed ==
	            countersBeforeCollection.resourcesPhysicallyDestroyed + collected &&
	        countersAfterCollection.retirementBacklog + collected ==
	            countersBeforeCollection.retirementBacklog &&
	        fake->waitIdleCount == 0u && fake->graphics.waitIdleCount == 0u,
	        "token-complete replaced generation did not retire without device idle");
}

void testLiveResourceFailureMatrix()
{
	{
		auto device = std::make_unique<TransactionFakeDevice>();
		TransactionFakeDevice* fake = device.get();
		RendererFacade facade(std::move(device));
		const RendererTargetFence initialFence = baseFence();
		require(facade.initializeSceneTransactions(initialState(initialFence)).initialized,
		        "recording-failure transaction initialization failed");
		const RendererSceneStateSnapshot before = facade.activeSceneState();
		fake->capture.failNextBufferCopy = true;
		const SceneApplyResult result =
			facade.applyFullSceneSnapshot(fullSnapshotRequest(initialFence));
		require(result.disposition == SceneApplyDisposition::FailedBeforeSubmit &&
		        result.error == SceneApplyError::RecordingFailure &&
		        facade.activeSceneState() == before &&
		        facade.sceneTransactionVisibleGeneration() == before.generation &&
		        fake->graphics.submissions.empty() &&
		        fake->createdCount != 0u && fake->destroyedCount == fake->createdCount &&
		        fake->waitIdleCount == 0u && fake->graphics.waitIdleCount == 0u,
		        "RHI buffer-copy failure did not roll back the private candidate");
	}

	{
		auto device = std::make_unique<TransactionFakeDevice>();
		TransactionFakeDevice* fake = device.get();
		RendererFacade facade(std::move(device));
		const RendererTargetFence initialFence = baseFence();
		require(facade.initializeSceneTransactions(initialState(initialFence)).initialized,
		        "submission-failure transaction initialization failed");
		const RendererSceneStateSnapshot before = facade.activeSceneState();
		fake->graphics.failNextSubmit = true;
		const SceneApplyResult result =
			facade.applyFullSceneSnapshot(fullSnapshotRequest(initialFence));
		require(result.disposition == SceneApplyDisposition::FailedBeforeSubmit &&
		        result.error == SceneApplyError::DeviceLost &&
		        facade.activeSceneState() == before &&
		        facade.sceneTransactionVisibleGeneration() == before.generation &&
		        fake->graphics.submissions.empty() &&
		        fake->createdCount != 0u && fake->destroyedCount == fake->createdCount &&
		        fake->waitIdleCount == 0u && fake->graphics.waitIdleCount == 0u,
		        "RHI queue-submit failure did not roll back the private candidate");
	}

	struct FailureCase
	{
		SceneTransactionFailurePoint point;
		uint32_t submissionIndex;
		SceneApplyDisposition disposition;
		SceneApplyError error;
		bool submitted;
	};
	const std::array cases{
		FailureCase{SceneTransactionFailurePoint::cpuStaging, 0u,
		            SceneApplyDisposition::FailedBeforeSubmit,
		            SceneApplyError::AllocationFailure, false},
		FailureCase{SceneTransactionFailurePoint::resourceAllocation, 0u,
		            SceneApplyDisposition::FailedBeforeSubmit,
		            SceneApplyError::AllocationFailure, false},
		FailureCase{SceneTransactionFailurePoint::descriptorStaging, 0u,
		            SceneApplyDisposition::FailedBeforeSubmit,
		            SceneApplyError::DescriptorStagingFailure, false},
		FailureCase{SceneTransactionFailurePoint::commandRecording, 0u,
		            SceneApplyDisposition::FailedBeforeSubmit,
		            SceneApplyError::RecordingFailure, false},
		FailureCase{SceneTransactionFailurePoint::queueSubmission, 0u,
		            SceneApplyDisposition::FailedBeforeSubmit,
		            SceneApplyError::DeviceLost, false},
		FailureCase{SceneTransactionFailurePoint::queueSubmission, 1u,
		            SceneApplyDisposition::FailedAfterSubmit,
		            SceneApplyError::DeviceLost, true},
		FailureCase{SceneTransactionFailurePoint::prePublication, 0u,
		            SceneApplyDisposition::FailedAfterSubmit,
		            SceneApplyError::PublicationInvariant, true},
		FailureCase{SceneTransactionFailurePoint::retirementEnqueue, 0u,
		            SceneApplyDisposition::FailedAfterSubmit,
		            SceneApplyError::RetirementEnqueueFailure, true},
	};

	for (const FailureCase& failure : cases)
	{
		auto device = std::make_unique<TransactionFakeDevice>();
		TransactionFakeDevice* fake = device.get();
		RendererFacade facade(std::move(device));
		const RendererTargetFence initialFence = baseFence();
		require(facade.initializeSceneTransactions(initialState(initialFence)).initialized,
		        "failure-matrix transaction initialization failed");
		const RendererSceneStateSnapshot before = facade.activeSceneState();
		const SceneDeltaCounterSnapshot countersBefore = facade.sceneDeltaCounters();
		ApplyFullSceneSnapshotRequest request = fullSnapshotRequest(initialFence);
		if (failure.point == SceneTransactionFailurePoint::queueSubmission &&
			failure.submissionIndex == 1u)
		{
			request.transaction.stagePlan.submissions = {
				{rhi::QueueClass::graphics, 701u},
				{rhi::QueueClass::graphics, 702u},
			};
		}
		facade.setSceneTransactionFailurePoint(
			failure.point, failure.submissionIndex);
		const SceneApplyResult result = facade.applyFullSceneSnapshot(request);
		require(result.disposition == failure.disposition &&
		        result.error == failure.error &&
		        facade.activeSceneState() == before &&
		        facade.sceneTransactionVisibleGeneration() == before.generation &&
		        fake->waitIdleCount == 0u &&
		        (failure.submitted == !fake->graphics.submissions.empty()),
		        "adversarial live transaction failure changed active state or classification");
		require(result.counterDelta.partialPublicationCount == 0u &&
		        result.counterDelta.deviceIdleCalls == 0u &&
		        facade.sceneDeltaCounters().sceneChangeSetsReceived ==
		            countersBefore.sceneChangeSetsReceived + 1u,
		        "failure stage violated counter atomicity");
		if (!failure.submitted && failure.point != SceneTransactionFailurePoint::cpuStaging)
		{
			require(fake->destroyedCount == fake->createdCount,
			        "pre-submit failure did not immediately destroy every candidate resource");
		}
		if (failure.submitted)
		{
			const uint64_t destroyedBeforeCompletion = fake->destroyedCount;
			fake->graphics.completeAll();
			require(facade.collectSceneTransactionGarbage() > 0u &&
			        fake->destroyedCount > destroyedBeforeCompletion,
			        "post-submit orphan was not retired against its exact token");
		}
	}

	auto device = std::make_unique<TransactionFakeDevice>();
	TransactionFakeDevice* fake = device.get();
	RendererFacade facade(std::move(device));
	RendererTargetFence current = baseFence();
	require(facade.initializeSceneTransactions(initialState(current)).initialized,
	        "retirement-reservation fixture initialization failed");
	const SceneApplyResult first =
		facade.applyFullSceneSnapshot(fullSnapshotRequest(current));
	require(first.accepted() && first.resultingFence,
	        "retirement-reservation fixture could not publish its first resource generation");
	current = *first.resultingFence;
	const RendererSceneStateSnapshot activeBeforeReservationFailure =
		facade.activeSceneState();
	facade.setSceneTransactionFailurePoint(
		SceneTransactionFailurePoint::retirementReservation);
	const SceneApplyResult reservationFailure =
		facade.applyFullSceneSnapshot(fullSnapshotRequest(current));
	require(reservationFailure.disposition == SceneApplyDisposition::FailedAfterSubmit &&
	        reservationFailure.error == SceneApplyError::RetirementEnqueueFailure &&
	        facade.activeSceneState() == activeBeforeReservationFailure &&
	        fake->waitIdleCount == 0u,
	        "retirement reservation failure partially published the replacement");
	fake->graphics.completeAll();
	require(facade.collectSceneTransactionGarbage() > 0u,
	        "reservation-failed candidate orphan was not eventually destroyed");
}

void testSuccessfulInvalidQueueTokenRetiresOnRecoveredFence()
{
	auto device = std::make_unique<TransactionFakeDevice>();
	TransactionFakeDevice* fake = device.get();
	RendererFacade facade(std::move(device));
	const RendererTargetFence fence = baseFence();
	require(facade.initializeSceneTransactions(initialState(fence)).initialized,
	        "invalid-token renderer fixture initialization failed");
	const RendererSceneStateSnapshot active = facade.activeSceneState();
	fake->graphics.reportInvalidNextToken = true;
	const SceneApplyResult failed =
		facade.applyFullSceneSnapshot(fullSnapshotRequest(fence));
	require(failed.disposition == SceneApplyDisposition::FailedAfterSubmit &&
	        failed.error == SceneApplyError::PublicationInvariant &&
	        failed.queueSubmissionTokens.size() == 1u &&
	        !failed.queueSubmissionTokens.front().isValid() &&
	        failed.message.find("queue=0:0, value=0") != std::string::npos &&
	        facade.activeSceneState() == active &&
	        fake->graphics.submissions.size() == 1u &&
	        fake->destroyedCount == 0u,
	        "successful invalid queue token was lost or published unsafely");

	const rhi::SubmissionToken actualToken = fake->graphics.submissions.front();
	require(facade.collectSceneTransactionGarbage() == 0u &&
	        fake->destroyedCount == 0u,
	        "invalid-token orphan retired before its recovered fence completed");
	fake->graphics.completeThrough(actualToken);
	require(facade.collectSceneTransactionGarbage() > 0u &&
	        fake->destroyedCount > 0u && fake->waitIdleCount == 0u &&
	        fake->graphics.waitIdleCount == 0u,
	        "invalid-token orphan did not retire on its recovered queue fence");

	const SceneApplyResult accepted =
		facade.applyFullSceneSnapshot(fullSnapshotRequest(fence));
	require(accepted.disposition == SceneApplyDisposition::AcceptedFullSnapshot &&
	        accepted.resultingFence &&
	        facade.activeSceneState().fence == accepted.resultingFence,
	        "normal renderer publication did not recover after invalid-token retirement");
}

} // namespace

int main()
{
	try
	{
		testSemanticTextureMappingContract();
		testSemanticTextureCoordinatesAcrossAdapterTransactions();
		testAdapterRetiresRemovedHandleAcrossDuplicateUndoRedo();
		testAdapterCameraLightSourceIdentityPreflight();
		testProjectionOwnerCapabilityAttackMatrix();
		testTextureUploadFootprintValidation();
		testFacadeIncrementalDuplicateAndSnapshot();
		testFastPathsAndIndependentClocks();
		testCameraAndLightAttachmentPublication();
		testMetalRejectsBeforeMutation();
		testConcreteMappingProvenanceGuards();
		testMalformedTexturePlansHaveZeroRhiEffects();
		testValidKtxTextureCopyAndBarrierContract();
		testLiveResourceSnapshotAndCpuCandidatePublication();
		testLiveResourceFailureMatrix();
		testSuccessfulInvalidQueueTokenRetiresOnRecoveredFence();
		testSceneUploadProjectionBinding();
		testMultiPrimitiveDrawableIdentity();
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
