from __future__ import annotations

import json
from pathlib import Path
import tempfile
import unittest

from mgif_beeai_dx12.agent_tools import create_source_tools


class AgentToolTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory(prefix="beeai-tools-")
        self.addCleanup(self.temp_dir.cleanup)
        self.root = Path(self.temp_dir.name).resolve()
        (self.root / "rhi" / "d3d12").mkdir(parents=True)
        (self.root / "rhi" / "d3d12" / "Buffer.cpp").write_text(
            "first line\nDX12_NEEDLE\nlast line\n", encoding="utf-8"
        )

    async def test_search_source_uses_workspace_relative_root_in_results_and_audit(self) -> None:
        audit: list[dict[str, object]] = []
        tools = create_source_tools(self.root, audit)
        search = next(tool for tool in tools if tool.name == "search_source")

        output = await search.run({"query": "dx12_needle", "root": "rhi", "max_results": 5})
        payload = json.loads(output.get_text_content())

        self.assertEqual(payload["matches"][0]["path"], "rhi/d3d12/Buffer.cpp")
        self.assertEqual(payload["matches"][0]["line"], 2)
        self.assertEqual(audit[0]["root"], "rhi")
        self.assertEqual(audit[0]["match_count"], 1)


if __name__ == "__main__":
    unittest.main()
