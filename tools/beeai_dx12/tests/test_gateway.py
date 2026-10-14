from __future__ import annotations

import json
from pathlib import Path
import tempfile
import unittest

from mgif_beeai_dx12.gateway import GatewayConfig, GatewayConfigurationError


class GatewayConfigTests(unittest.TestCase):
    def test_missing_configuration_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="beeai-gateway-") as directory:
            with self.assertRaises(GatewayConfigurationError):
                GatewayConfig.from_sources({}, codex_config=Path(directory) / "missing.toml")

    def test_environment_configuration_and_headers(self) -> None:
        config = GatewayConfig.from_sources(
            {
                "BEEAI_OPENAI_BASE_URL": "https://gateway.example/v1/",
                "BEEAI_OPENAI_MODEL": "model-a",
                "BEEAI_OPENAI_API_KEY": "top-secret",
                "BEEAI_OPENAI_HEADERS": json.dumps({"X-Tenant": "alpha"}),
            }
        )
        self.assertEqual(config.base_url, "https://gateway.example/v1")
        self.assertEqual(config.extra_headers, {"X-Tenant": "alpha"})
        self.assertNotIn("top-secret", repr(config))
        self.assertNotIn("top-secret", json.dumps(config.diagnostic()))

    def test_codex_provider_is_supported_without_exposing_token(self) -> None:
        with tempfile.TemporaryDirectory(prefix="beeai-gateway-") as directory:
            path = Path(directory) / "config.toml"
            path.write_text(
                'model_provider = "custom"\n'
                'model = "gpt-test"\n'
                '[model_providers.custom]\n'
                'base_url = "https://gateway.example/v1"\n'
                'experimental_bearer_token = "private-token"\n'
                'wire_api = "responses"\n',
                encoding="utf-8",
            )
            config = GatewayConfig.from_sources({}, codex_config=path)
        self.assertEqual(config.api_key, "private-token")
        self.assertEqual(config.source, "codex:custom")
        self.assertNotIn("private-token", repr(config))
        self.assertNotIn("private-token", json.dumps(config.diagnostic()))


if __name__ == "__main__":
    unittest.main()
