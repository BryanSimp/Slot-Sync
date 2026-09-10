"""Daemon configuration.

Order of precedence: command-line flag, then environment, then
`~/.slotsync/dolphin.json`, then a default. JSON rather than TOML because
writing TOML needs a dependency and this component is deliberately stdlib-only.
"""

from __future__ import annotations

import contextlib
import json
import os
from dataclasses import dataclass, field
from pathlib import Path

CONFIG_PATH = Path.home() / ".slotsync" / "dolphin.json"

DEFAULT_SERVER = "http://localhost:8080"
DEFAULT_CARDS_DIR = Path.home() / ".slotsync" / "cards"

#: Memory Card 251. PLAN.md §5 makes it the project default, and it is what
#: most GameCube games expect to find.
DEFAULT_MBIT = 16


class ConfigError(RuntimeError):
    """The configuration cannot produce a working daemon."""


@dataclass
class Config:
    server: str = DEFAULT_SERVER
    token: str = ""
    cards_dir: Path = field(default_factory=lambda: DEFAULT_CARDS_DIR)
    user_dir: Path | None = None
    dolphin_exe: Path | None = None
    slot: str = "A"
    mbit: int = DEFAULT_MBIT
    device: int | None = None
    #: Seconds between polls in `watch`, and how long a card must sit
    #: unchanged before it is considered finished being written.
    poll_interval: float = 2.0
    settle_seconds: float = 5.0
    #: How often `watch` asks the server whether anything has moved. Much
    #: slower than the push poll: a push is watching a local file and costs
    #: nothing, while this is a request per card.
    pull_interval: float = 30.0

    def require_token(self) -> str:
        if not self.token:
            raise ConfigError(
                "no server token. Pass --token, set SLOTSYNC_TOKEN, or put "
                f'{{"token": "..."}} in {CONFIG_PATH}'
            )
        return self.token

    @classmethod
    def load(cls, **overrides) -> Config:
        """Build a config from the file, the environment, and explicit flags."""
        values: dict = {}

        if CONFIG_PATH.is_file():
            try:
                values.update(json.loads(CONFIG_PATH.read_text(encoding="utf-8")))
            except ValueError as exc:
                raise ConfigError(f"{CONFIG_PATH} is not valid JSON: {exc}") from None

        env = {
            "server": os.environ.get("SLOTSYNC_SERVER"),
            "token": os.environ.get("SLOTSYNC_TOKEN"),
            "cards_dir": os.environ.get("SLOTSYNC_CARDS_DIR"),
            "user_dir": os.environ.get("SLOTSYNC_DOLPHIN_USER_DIR"),
            "dolphin_exe": os.environ.get("SLOTSYNC_DOLPHIN_EXE"),
        }
        values.update({k: v for k, v in env.items() if v})
        values.update({k: v for k, v in overrides.items() if v is not None})

        config = cls()
        for key, value in values.items():
            if not hasattr(config, key):
                continue
            if key in ("cards_dir", "user_dir", "dolphin_exe") and value is not None:
                value = Path(value).expanduser()
            if key == "slot":
                value = str(value).upper()
            if key == "mbit":
                value = int(value)
            setattr(config, key, value)

        config.cards_dir = Path(config.cards_dir).expanduser()
        return config

    def save(self) -> Path:
        """Persist the non-secret-ish settings, so flags are needed once."""
        CONFIG_PATH.parent.mkdir(parents=True, exist_ok=True)
        payload = {
            "server": self.server,
            "token": self.token,
            "cards_dir": str(self.cards_dir),
            "slot": self.slot,
            "mbit": self.mbit,
        }
        if self.user_dir:
            payload["user_dir"] = str(self.user_dir)
        if self.dolphin_exe:
            payload["dolphin_exe"] = str(self.dolphin_exe)
        if self.device is not None:
            # Without this the web UI attributes every push from this PC to
            # nobody, while the consoles name themselves. It is not a secret
            # and it has to survive across runs to be worth anything.
            payload["device"] = self.device
        CONFIG_PATH.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")

        # The token is in here, so keep it off other users' eyes where the OS
        # makes that cheap.
        with contextlib.suppress(OSError):
            CONFIG_PATH.chmod(0o600)
        return CONFIG_PATH
