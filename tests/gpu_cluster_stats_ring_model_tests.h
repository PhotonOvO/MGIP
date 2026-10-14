// Included by gpu_scene_registry_model_tests.cpp so this focused native model
// exercises the production light-resource state without adding a CMake target.

	GPUDrivenLightResources::ClusterStats makeClusterStats(uint32_t seed)
	{
		return GPUDrivenLightResources::ClusterStats{
			.maxOccupancy = seed + 1u,
			.overflowClusterCount = seed + 2u,
			.appendedLightReferences = seed + 3u,
			.testedPointLights = seed + 4u,
			.testedSpotLights = seed + 5u,
		};
	}

	void writeClusterStats(FakeDevice& device,
	                       rhi::BufferHandle buffer,
	                       const GPUDrivenLightResources::ClusterStats& stats)
	{
		FakeDevice::BufferStorage& storage = device.buffer(buffer);
		expect(storage.bytes.size() >= sizeof(stats), "cluster-stats buffer is undersized");
		std::memcpy(storage.bytes.data(), &stats, sizeof(stats));
	}

	GPUDrivenLightResources::ClusterStats readClusterStats(const FakeDevice& device,
	                                                       rhi::BufferHandle buffer)
	{
		return readElement<GPUDrivenLightResources::ClusterStats>(device, buffer, 0u);
	}

	void expectZeroStats(const GPUDrivenLightResources::ClusterStats& stats, const char* message)
	{
		expect(stats.maxOccupancy == 0u
		       && stats.overflowClusterCount == 0u
		       && stats.appendedLightReferences == 0u
		       && stats.testedPointLights == 0u
		       && stats.testedSpotLights == 0u,
		       message);
	}

	void expectDiagnostics(const GPUDrivenLightResources::Diagnostics& diagnostics,
	                       const GPUDrivenLightResources::ClusterStats& expected,
	                       const char* message)
	{
		expect(diagnostics.maxOccupancy == expected.maxOccupancy
		       && diagnostics.overflowClusterCount == expected.overflowClusterCount
		       && diagnostics.appendedLightReferences == expected.appendedLightReferences,
		       message);
	}

	void runClusterStatsRingModel()
	{
		constexpr uint32_t frameCount = 3u;
		FakeDevice device;
		GPUDrivenLightResources resources;
		resources.init(device, GPUDrivenLightResources::CreateInfo{
			.maxPointLights = 1u,
			.maxSpotLights = 1u,
			.frameCount = frameCount,
		});

		std::array<rhi::BufferHandle, frameCount> statsBuffers{};
		std::array<rhi::BufferHandle, frameCount> readbacks{};
		for (uint32_t slot = 0u; slot < frameCount; ++slot)
		{
			statsBuffers[slot] = resources.getClusterStatsBuffer(slot);
			readbacks[slot] = resources.getClusterStatsReadbackBuffer(slot);
			expect(!statsBuffers[slot].isNull(), "cluster-stats ring is missing a producer buffer");
			expect(!readbacks[slot].isNull(), "cluster-stats ring is missing a readback buffer");
			for (uint32_t priorSlot = 0u; priorSlot < slot; ++priorSlot)
			{
				expect(statsBuffers[slot] != statsBuffers[priorSlot],
				       "cluster-stats producer slots alias one another");
				expect(readbacks[slot] != readbacks[priorSlot],
				       "cluster-stats readback slots alias one another");
			}
			expectZeroStats(readClusterStats(device, readbacks[slot]),
			                "cluster-stats readback allocation was not initialized to zero");
			expect(device.buffer(readbacks[slot]).mapCount == 1u,
			       "cluster-stats readback initialization must map each physical slot once");
		}

		FrameScheduler scheduler;
		scheduler.init(device, frameCount);
		FakeQueue& queue = device.graphicsQueue();
		FakeCommandBuffer* activeCommandBuffer = nullptr;
		uint32_t producerMarkCount = 0u;

		const auto beginSlot = [&](uint32_t expectedSlot)
		{
			expect(scheduler.currentFrameIndex() == expectedSlot,
			       "frame scheduler did not select the expected physical slot");
			activeCommandBuffer = dynamic_cast<FakeCommandBuffer*>(&scheduler.beginFrame());
			expect(activeCommandBuffer != nullptr,
			       "frame scheduler did not return the fake command buffer");
			expect(activeCommandBuffer->copies.empty(),
			       "reused command buffer retained an older cluster-stats copy");
			resources.cacheClusterStats(expectedSlot);
			expect(queue.waitIdleCount == 0u && device.waitIdleCount == 0u,
			       "cluster-stats cache used a global idle wait");
		};

		const auto submitSlot = [&](uint32_t expectedSlot,
		                            bool recordStatsCopy,
		                            GPUDrivenLightResources::ClusterStats completedStats = {})
		{
			expect(activeCommandBuffer != nullptr,
			       "cluster-stats ring submit did not follow a begun physical slot");
			expect(scheduler.currentFrameIndex() == expectedSlot,
			       "cluster-stats producer was attributed to the wrong physical slot");
			expect(activeCommandBuffer->copies.empty(),
			       "cluster-stats command buffer was not clean before producer recording");

			if (recordStatsCopy)
			{
				// Model the dispatch output in the physical source slot, then record the
				// same source-to-readback copy used by the production clustered pass.
				writeClusterStats(device, statsBuffers[expectedSlot], completedStats);
				rhi::ComputeEncoder* copy = activeCommandBuffer->beginComputePass();
				expect(copy != nullptr, "cluster-stats producer could not begin its copy encoder");
				copy->copyBuffer(statsBuffers[expectedSlot], 0u, readbacks[expectedSlot], 0u,
				                 sizeof(GPUDrivenLightResources::ClusterStats));
				activeCommandBuffer->endEncoding();
			}

			const size_t expectedCopyCount = recordStatsCopy ? 1u : 0u;
			expect(activeCommandBuffer->copies.size() == expectedCopyCount,
			       "cluster-stats producer recorded an unexpected number of readback copies");
			const bool producerCopyRecorded = activeCommandBuffer->copies.size() == 1u;
			if (producerCopyRecorded)
			{
				const CopyRecord& recordedCopy = activeCommandBuffer->copies.front();
				expect(recordedCopy.source == statsBuffers[expectedSlot],
				       "cluster-stats copy used the wrong physical producer slot");
				expect(recordedCopy.destination == readbacks[expectedSlot],
				       "cluster-stats copy used the wrong physical readback slot");
				expect(recordedCopy.sourceOffset == 0u && recordedCopy.destinationOffset == 0u,
				       "cluster-stats copy used a nonzero buffer offset");
				expect(recordedCopy.size == sizeof(GPUDrivenLightResources::ClusterStats),
				       "cluster-stats copy used the wrong byte count");
			}

			const size_t waitsBeforeSubmit = queue.waits.size();
			const size_t submissionsBeforeSubmit = queue.submissions.size();
			const uint32_t marksBeforeSubmit = producerMarkCount;
			const rhi::SubmissionToken token = scheduler.submitFrame(*activeCommandBuffer);
			expect(queue.waits.size() == waitsBeforeSubmit,
			       "frame submit serialized on a queue wait");
			expect(queue.submissions.size() == submissionsBeforeSubmit + 1u
			       && queue.submissions.back() == token,
			       "cluster-stats frame did not produce exactly one submission token");
			expect(producerMarkCount == marksBeforeSubmit,
			       "cluster-stats producer was marked before submit returned successfully");
			if (producerCopyRecorded)
			{
				FakeCommandBuffer* submittedCommandBuffer = activeCommandBuffer;
				queue.completeOnWait(token, [submittedCommandBuffer]
				{
					submittedCommandBuffer->executeCopies();
				});
			}
			resources.markClusterStatsProducerSubmitted(expectedSlot, producerCopyRecorded);
			++producerMarkCount;
			expect(producerMarkCount == marksBeforeSubmit + 1u
			       && static_cast<size_t>(producerMarkCount) == queue.submissions.size(),
			       "cluster-stats submission did not receive exactly one post-submit producer mark");
			activeCommandBuffer = nullptr;
			return token;
		};

		const GPUDrivenLightResources::ClusterStats poison = makeClusterStats(900u);
		writeClusterStats(device, readbacks[0], poison);
		const uint32_t firstSlotMapsBefore = device.buffer(readbacks[0]).mapCount;
		beginSlot(0u);
		expect(queue.waits.empty(), "first use of a physical slot waited on a nonexistent submission");
		expectDiagnostics(resources.getDiagnostics(), {},
		                  "first-use diagnostics exposed unproduced readback bytes");
		expect(device.buffer(readbacks[0]).mapCount == firstSlotMapsBefore,
		       "first use mapped a slot without a submitted stats producer");

		const GPUDrivenLightResources::ClusterStats statsA = makeClusterStats(10u);
		const GPUDrivenLightResources::ClusterStats statsB = makeClusterStats(20u);
		const GPUDrivenLightResources::ClusterStats statsC = makeClusterStats(30u);
		const GPUDrivenLightResources::ClusterStats statsD = makeClusterStats(40u);
		const rhi::SubmissionToken tokenA = submitSlot(0u, true, statsA);
		expect(readClusterStats(device, readbacks[0]).maxOccupancy == poison.maxOccupancy,
		       "slot 0 copy executed before its submission token retired");

		beginSlot(1u);
		expect(queue.waits.empty(), "first use of slot 1 waited on another slot's producer");
		expectDiagnostics(resources.getDiagnostics(), {},
		                  "slot 1 published statistics before its first producer");
		const rhi::SubmissionToken tokenB = submitSlot(1u, true, statsB);

		beginSlot(2u);
		expect(queue.waits.empty(), "first use of slot 2 waited on another slot's producer");
		expectDiagnostics(resources.getDiagnostics(), {},
		                  "slot 2 published statistics before its first producer");
		const rhi::SubmissionToken tokenC = submitSlot(2u, true, statsC);
		expect(queue.waitIdleCount == 0u && device.waitIdleCount == 0u,
		       "cluster-stats ring introduced a global queue idle wait");

		beginSlot(0u);
		expect(queue.waits.size() == 1u && queue.waits.back() == tokenA,
		       "slot 0 reuse did not wait for slot 0's exact producer token");
		expectDiagnostics(resources.getDiagnostics(), statsA,
		                  "slot 0 did not cache completed statistics at fixed ring latency");
		const uint32_t slotZeroMapsAfterA = device.buffer(readbacks[0]).mapCount;
		const rhi::SubmissionToken tokenD = submitSlot(0u, true, statsD);
		expectDiagnostics(resources.getDiagnostics(), statsA,
		                  "a new submission overwrote cached diagnostics before its completion");
		expect(readClusterStats(device, readbacks[0]).maxOccupancy == statsA.maxOccupancy,
		       "slot 0 readback was overwritten before its replacement producer completed");

		beginSlot(1u);
		expect(queue.waits.size() == 2u && queue.waits.back() == tokenB,
		       "slot 1 reuse did not wait for slot 1's exact producer token");
		expectDiagnostics(resources.getDiagnostics(), statsB,
		                  "slot 1 did not publish its completed producer");
		const uint32_t slotOneMapsAfterB = device.buffer(readbacks[1]).mapCount;
		const rhi::SubmissionToken tokenNoCopyB = submitSlot(1u, false);
		expectDiagnostics(resources.getDiagnostics(), {},
		                  "a disabled/no-copy frame retained older clustered statistics");

		beginSlot(2u);
		expect(queue.waits.size() == 3u && queue.waits.back() == tokenC,
		       "slot 2 reuse did not wait for slot 2's exact producer token");
		expectDiagnostics(resources.getDiagnostics(), statsC,
		                  "slot 2 did not publish its completed producer");
		(void)submitSlot(2u, false);

		beginSlot(0u);
		expect(queue.waits.size() == 4u && queue.waits.back() == tokenD,
		       "replacement producer was not retired on the same physical slot");
		expectDiagnostics(resources.getDiagnostics(), statsD,
		                  "replacement statistics did not retain frame-count latency");
		expect(device.buffer(readbacks[0]).mapCount == slotZeroMapsAfterA + 1u,
		       "replacement producer was not read exactly once after completion");
		resources.cacheClusterStats(0u);
		expectDiagnostics(resources.getDiagnostics(), {},
		                  "a consumed producer was published twice");
		expect(device.buffer(readbacks[0]).mapCount == slotZeroMapsAfterA + 1u,
		       "a consumed producer caused a second host read");
		(void)submitSlot(0u, false);

		beginSlot(1u);
		expect(queue.waits.size() == 5u && queue.waits.back() == tokenNoCopyB,
		       "no-copy slot reuse waited on the wrong physical token");
		expectDiagnostics(resources.getDiagnostics(), {},
		                  "no-copy slot reuse republished stale readback bytes");
		expect(device.buffer(readbacks[1]).mapCount == slotOneMapsAfterB,
		       "no-copy slot reuse performed an invalid host read");
		(void)submitSlot(1u, false);

		expect(queue.waitIdleCount == 0u && device.waitIdleCount == 0u,
		       "cluster-stats retirement used a global wait instead of exact slot tokens");
		scheduler.shutdown();
		resources.deinit();
		expect(queue.waitIdleCount == 0u && device.waitIdleCount == 0u,
		       "cluster-stats shutdown introduced a global idle wait");
		expect(resources.getFrameCount() == 0u && !resources.getDiagnostics().initialized,
		       "cluster-stats producer validity survived resource shutdown");
		expectDiagnostics(resources.getDiagnostics(), {},
		                  "cluster-stats diagnostics survived resource shutdown");

		resources.init(device, GPUDrivenLightResources::CreateInfo{
			.maxPointLights = 1u,
			.maxSpotLights = 1u,
			.frameCount = 1u,
		});
		const rhi::BufferHandle reinitializedReadback = resources.getClusterStatsReadbackBuffer(0u);
		writeClusterStats(device, reinitializedReadback, poison);
		const uint32_t reinitializedMaps = device.buffer(reinitializedReadback).mapCount;
		resources.cacheClusterStats(0u);
		expectDiagnostics(resources.getDiagnostics(), {},
		                  "reinitialized slot inherited an old producer-valid bit");
		expect(device.buffer(reinitializedReadback).mapCount == reinitializedMaps,
		       "reinitialized slot performed a first-use host read");
		resources.deinit();
		expect(queue.waitIdleCount == 0u && device.waitIdleCount == 0u,
		       "cluster-stats recreate path introduced a global idle wait");
	}
