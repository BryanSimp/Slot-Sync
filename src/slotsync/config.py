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

    #: Off unless explicitly enabled, so importing the app in a test never
    #: binds a UDP port. `from_env` turns it on.
    enable_udp: bool = False

    #: Seconds a staging buffer survives without a chunk -- PLAN.md section 4.
    staging_ttl: float = 120.0

    #: Concurrent staging buffers. A cap, not a target: each one holds a whole
    #: card in memory.
    max_staging: int = 64

    #: Chunks sent back-to-back during a pull before yielding. A 2 MiB card is
    #: 2048 datagrams and a 2006 WiFi stack will drop most of them if they all
    #: arrive at once, so the burst is paced.
    pull_burst: int = 32
    pull_burst_delay: float = 0.002

    #: UDP receive buffer. A 2 MiB card arrives as 2048 datagrams; with the
    #: default socket buffer the kernel drops roughly a third of an unpaced
    #: burst before the listener ever sees them. The protocol recovers via
    #: NACK, but paying for it in retransmissions is silly when the fix is one
    #: setsockopt. The OS may clamp this.
    udp_rcvbuf: int = 4 * 1024 * 1024

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
            enable_udp=True,
            staging_ttl=_float(src, "SLOTSYNC_STAGING_TTL", 120.0),
            max_staging=_int(src, "SLOTSYNC_MAX_STAGING", 64),
            pull_burst=_int(src, "SLOTSYNC_PULL_BURST", 32),
            pull_burst_delay=_float(src, "SLOTSYNC_PULL_BURST_DELAY", 0.002),
            udp_rcvbuf=_int(src, "SLOTSYNC_UDP_RCVBUF", 4 * 1024 * 1024),
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


def _float(src, name: str, default: float) -> float:
    raw = src.get(name)
    if raw is None or raw == "":
        return default
    try:
        return float(raw)
    except ValueError as exc:
        raise ConfigError(f"{name} must be a number, got {raw!r}") from exc
