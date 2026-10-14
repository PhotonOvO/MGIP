from __future__ import annotations

from dataclasses import dataclass
import json
import os
from pathlib import Path
import tomllib
from typing import Mapping
from urllib.parse import urlparse


class GatewayConfigurationError(ValueError):
    """Raised when no usable OpenAI-compatible gateway is configured."""


@dataclass(frozen=True, repr=False)
class GatewayConfig:
    base_url: str
    model_id: str
    api_key: str
    extra_headers: dict[str, str]
    source: str

    @classmethod
    def from_sources(
        cls,
        env: Mapping[str, str] | None = None,
        *,
        codex_config: str | Path | None = None,
    ) -> "GatewayConfig":
        values = os.environ if env is None else env
        base_url = values.get("BEEAI_OPENAI_BASE_URL") or values.get("OPENAI_API_BASE")
        model_id = values.get("BEEAI_OPENAI_MODEL") or values.get("OPENAI_CHAT_MODEL")
        api_key = values.get("BEEAI_OPENAI_API_KEY") or values.get("OPENAI_API_KEY")
        headers = _parse_headers(values.get("BEEAI_OPENAI_HEADERS") or values.get("OPENAI_API_HEADERS"))
        if base_url and model_id and api_key:
            return cls._validated(base_url, model_id, api_key, headers, "environment")

        path = Path(codex_config) if codex_config else Path.home() / ".codex" / "config.toml"
        if not path.is_file():
            missing = _missing(base_url=base_url, model_id=model_id, api_key=api_key)
            raise GatewayConfigurationError(f"missing gateway configuration: {', '.join(missing)}")

        with path.open("rb") as stream:
            document = tomllib.load(stream)
        provider_id = str(document.get("model_provider") or "")
        providers = document.get("model_providers") or {}
        provider = providers.get(provider_id) if isinstance(providers, dict) else None
        if not provider_id or not isinstance(provider, dict):
            raise GatewayConfigurationError("Codex config does not define the selected model provider")

        base_url = base_url or _string(provider.get("base_url"))
        model_id = model_id or _string(document.get("model"))
        api_key = api_key or _resolve_codex_key(provider, values)
        if not headers:
            headers = _coerce_headers(provider.get("http_headers"))
        missing = _missing(base_url=base_url, model_id=model_id, api_key=api_key)
        if missing:
            raise GatewayConfigurationError(f"missing gateway configuration: {', '.join(missing)}")
        return cls._validated(base_url, model_id, api_key, headers, f"codex:{provider_id}")

    @classmethod
    def _validated(
        cls,
        base_url: str,
        model_id: str,
        api_key: str,
        headers: dict[str, str],
        source: str,
    ) -> "GatewayConfig":
        parsed = urlparse(base_url)
        if parsed.scheme not in {"http", "https"} or not parsed.netloc:
            raise GatewayConfigurationError("gateway base URL must be an absolute HTTP(S) URL")
        if not model_id.strip() or not api_key.strip():
            raise GatewayConfigurationError("gateway model and API key must be non-empty")
        return cls(base_url.rstrip("/"), model_id.strip(), api_key, headers, source)

    @property
    def host(self) -> str:
        return urlparse(self.base_url).netloc

    def diagnostic(self) -> dict[str, object]:
        return {
            "configured": True,
            "host": self.host,
            "model_id": self.model_id,
            "source": self.source,
            "auth_present": bool(self.api_key),
            "extra_header_names": sorted(self.extra_headers),
        }

    def redact(self, value: object) -> str:
        redacted = str(value)
        secrets = [self.api_key, *self.extra_headers.values()]
        for secret in secrets:
            if secret:
                redacted = redacted.replace(secret, "[REDACTED]")
        return redacted

    def create_chat_model(self):
        from beeai_framework.adapters.openai import OpenAIChatModel

        settings = {"extra_headers": self.extra_headers} if self.extra_headers else None
        return OpenAIChatModel(
            self.model_id,
            api_key=self.api_key,
            base_url=self.base_url,
            settings=settings or {},
            allow_parallel_tool_calls=False,
        )

    def __repr__(self) -> str:
        return f"GatewayConfig(host={self.host!r}, model_id={self.model_id!r}, source={self.source!r})"


def _string(value: object) -> str | None:
    return value if isinstance(value, str) and value else None


def _resolve_codex_key(provider: dict[str, object], env: Mapping[str, str]) -> str | None:
    direct = _string(provider.get("experimental_bearer_token"))
    if direct:
        return direct
    env_name = _string(provider.get("env_key"))
    return env.get(env_name) if env_name else None


def _parse_headers(raw: str | None) -> dict[str, str]:
    if not raw:
        return {}
    try:
        return _coerce_headers(json.loads(raw))
    except (json.JSONDecodeError, TypeError) as exc:
        raise GatewayConfigurationError("gateway headers must be a JSON object") from exc


def _coerce_headers(value: object) -> dict[str, str]:
    if value is None:
        return {}
    if not isinstance(value, dict) or any(not isinstance(k, str) or not isinstance(v, str) for k, v in value.items()):
        raise GatewayConfigurationError("gateway headers must contain string keys and values")
    return dict(value)


def _missing(**values: str | None) -> list[str]:
    labels = {"base_url": "base URL", "model_id": "model ID", "api_key": "API key"}
    return [labels[name] for name, value in values.items() if not value]
