#!/usr/bin/env python3
"""Static contract checks for the GPU-driven present layout boundary."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "render" / "passes" / "GPUDrivenPresentPass.cpp").read_text(
    encoding="utf-8"
)
IMGUI_SOURCE = (ROOT / "render" / "passes" / "GPUDrivenImguiPass.cpp").read_text(
    encoding="utf-8"
)
PASS_EXECUTOR_SOURCE = (ROOT / "render" / "PassExecutor.cpp").read_text(
    encoding="utf-8"
)
SWAPCHAIN_SOURCE = (ROOT / "rhi" / "vulkan" / "VulkanSwapchain.cpp").read_text(
    encoding="utf-8"
)


def transition(texture: str, before: str, after: str) -> re.Pattern[str]:
    return re.compile(
        rf"\.texture\s*=\s*{texture}\s*,\s*"
        rf"\.before\s*=\s*rhi::ResourceState::{before}\s*,\s*"
        rf"\.after\s*=\s*rhi::ResourceState::{after}",
        re.DOTALL,
    )


class GPUDrivenPresentLayoutContractTests(unittest.TestCase):
    def test_present_clears_discarded_target_before_blit(self) -> None:
        expected = (
            transition("srcTex", "General", "TransferSrc"),
            transition("dstTex", "Undefined", "General"),
            transition("dstTex", "General", "TransferDst"),
            transition("dstTex", "TransferDst", "General"),
            transition("srcTex", "TransferSrc", "General"),
        )
        for pattern in expected:
            self.assertEqual(len(pattern.findall(SOURCE)), 1, pattern.pattern)

        self.assertNotRegex(
            SOURCE, transition("dstTex", "Undefined", "TransferDst")
        )
        self.assertNotRegex(
            SOURCE, transition("srcTex", "TransferSrc", "ShaderRead")
        )

    def test_imgui_declares_sampled_read_then_restores_general(self) -> None:
        dependency = re.compile(
            r"PassResourceDependency::texture\(\s*"
            r"kPassOutputHandle\s*,\s*ResourceAccess::read\s*,\s*"
            r"rhi::ShaderStage::fragment\s*,\s*"
            r"rhi::ResourceState::ShaderRead\s*\)",
            re.DOTALL,
        )
        self.assertEqual(len(dependency.findall(IMGUI_SOURCE)), 1)
        terminal = transition("output", "ShaderRead", "General")
        self.assertEqual(len(terminal.findall(IMGUI_SOURCE)), 1)

        draw = IMGUI_SOURCE.index("executeImGuiPass")
        barrier = terminal.search(IMGUI_SOURCE)
        self.assertIsNotNone(barrier)
        submit = IMGUI_SOURCE.index("resourceBarrier", barrier.start())
        self.assertLess(draw, barrier.start())
        self.assertLess(barrier.start(), submit)
        post_barrier = IMGUI_SOURCE[barrier.start() : submit]
        self.assertRegex(post_barrier, r"\.levelCount\s*=\s*~0u")
        self.assertRegex(post_barrier, r"\.layerCount\s*=\s*~0u")
        dependency_barrier = PASS_EXECUTOR_SOURCE[
            PASS_EXECUTOR_SOURCE.index("const rhi::TextureBarrier textureBarrier") :
            PASS_EXECUTOR_SOURCE.index(
                "context.commandBuffer->resourceBarrier",
                PASS_EXECUTOR_SOURCE.index("const rhi::TextureBarrier textureBarrier"),
            )
        ]
        self.assertRegex(dependency_barrier, r"\.levelCount\s*=\s*~0u")
        self.assertRegex(dependency_barrier, r"\.layerCount\s*=\s*~0u")
        self.assertIn("getPassOutputTextureRHIHandle()", IMGUI_SOURCE)
        self.assertNotRegex(
            IMGUI_SOURCE, r"\bVk(?:Image|CommandBuffer|ImageLayout)\b"
        )

    def test_transition_and_blit_order_is_closed(self) -> None:
        to_clear = SOURCE.index("const rhi::TextureBarrier toClear[]")
        first_barrier = SOURCE.index("resourceBarrier(toClear", to_clear)
        clear = SOURCE.index("clearColorTexture", first_barrier)
        to_blit = SOURCE.index("const std::array toBlit", clear)
        second_barrier = SOURCE.index("resourceBarrier", to_blit)
        blit = SOURCE.index("blitTexture", second_barrier)
        from_blit = SOURCE.index("const rhi::TextureBarrier fromBlit[]", blit)
        final_barrier = SOURCE.index("resourceBarrier(fromBlit", from_blit)
        self.assertEqual(SOURCE.count("clearColorTexture"), 1)
        self.assertLess(to_clear, first_barrier)
        self.assertLess(first_barrier, clear)
        self.assertLess(clear, to_blit)
        self.assertLess(to_blit, second_barrier)
        self.assertLess(second_barrier, blit)
        self.assertLess(blit, from_blit)
        self.assertLess(from_blit, final_barrier)

    def test_present_uses_only_rhi_handles_for_the_boundary(self) -> None:
        self.assertIn("getPassOutputTextureRHIHandle()", SOURCE)
        self.assertIn("getCurrentSwapchainTextureRHIHandle()", SOURCE)
        self.assertNotRegex(SOURCE, r"\bVk(?:Image|CommandBuffer|ImageLayout)\b")

    def test_acquire_wait_covers_the_first_swapchain_transfer(self) -> None:
        acquire = SWAPCHAIN_SOURCE[
            SWAPCHAIN_SOURCE.index("AcquireResult VulkanSwapchain::acquireNextImage") :
            SWAPCHAIN_SOURCE.index("PresentResult VulkanSwapchain::present")
        ]
        self.assertEqual(acquire.count(".stage = SubmitStage::transfer"), 1)
        self.assertNotIn(".stage = SubmitStage::colorOutput", acquire)


if __name__ == "__main__":
    unittest.main()
