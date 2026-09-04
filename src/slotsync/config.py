"""Configuration, read from the environment.

Everything the server needs comes from env vars so the container can be
configured by `docker-compose.yml` alone. See PLAN.md section 8: the token lives
in the environment, never in a file in the repo.
"""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path

#: Values that mean "the operator never edited .env.example". Refusing to start
#: on these is cheaper than discovering a LAN-open server later.
PLACEHOLDER_SECRETS = frozenset({"", "changeme", "change-me", "changethis", "secret"})


class ConfigError(RuntimeError):
    """Raised when the environment cannot produce a usable configuration."""


@dataclass(frozen=True)
class Config:
    """Resolved server configuration."""

    data_dir: Path
    token: str
    psk: bytes
    http_host: str = "0.0.0.0"
    http_port: int = 8080
    udp_host: str = "0.0.0.0"
    udp_port: int = 9977
    max_card_bytes: int = 16 * 1024 * 1024
    log_level: str = "INFO"

    @property
    def blobs_dir(self) -> Path:
        return self.data_dir / "blobs"

    @property
    def db_path(self) -> Path:
        return self.data_dir / "slotsync.db"

    @classmethod
    def from_env(cls, env: dict[str, str] | None = None) -> Config:
        """Build a Config from `env` (defaults to `os.environ`).

        Raises ConfigError with an actionable message rather than letting a
        missing secret surface as a confusing failure much later.
        """
        src = os.environ if env is None else env

        token = _require_secret(src, "SLOTSYNC_TOKEN")
        psk = _require_secret(src, "SLOTSYNC_PSK")

        return cls(
            data_dir=Path(src.get("SLOTSYNC_DATA", "/data")),
            token=token,
            psk=psk.encode("utf-8"),
            http_host=src.get("SLOTSYNC_HTTP_HOST", "0.0.0.0"),
            http_port=_int(src, "SLOTSYNC_HTTP_PORT", 8080),
            udp_host=src.get("SLOTSYNC_UDP_HOST", "0.0.0.0"),
            udp_port=_int(src, "SLOTSYNC_UDP_PORT", 9977),
            max_card_bytes=_int(src, "SLOTSYNC_MAX_CARD_BYTES", 16 * 1024 * 1024),
            log_level=src.get("SLOTSYNC_LOG_LEVEL", "INFO").upper(),
        )


def _require_secret(src, name: str) -> str:
    value = src.get(name, "").strip()
    if value.lower() in PLACEHOLDER_SECRETS:
        raise ConfigError(
            f"{name} is unset or still the placeholder value. "
            f"Generate one with: openssl rand -hex 32"
        )
    return value


def _int(src, name: str, default: int) -> int:
    raw = src.get(name)
    if raw is None or raw == "":
        return default
    try:
        return int(raw)
    except ValueError as exc:
        raise ConfigError(f"{name} must be an integer, got {raw!r}") from exc
