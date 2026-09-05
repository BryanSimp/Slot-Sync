"""The local per-game card directory, and what we know about each card.

    <cards_dir>/GALE01.raw          the card Dolphin is pointed at
    <cards_dir>/.slotsync.json      what version each card came from

The sidecar is what makes the conflict model work. A push has to say which
version it was derived from (PLAN.md §7), and the only way to know that is to
remember what we last pulled or pushed. Without it every push would have to
guess, and guessing is how saves get silently overwritten.
"""

from __future__ import annotations

import hashlib
import json
import logging
import os
import tempfile
from dataclasses import asdict, dataclass
from pathlib import Path

log = logging.getLogger("slotsync.cards")

STATE_FILE = ".slotsync.json"


@dataclass
class CardState:
    """What the server said about this card when we last agreed with it."""

    #: Version this local file was pulled from, or pushed as. 0 means the
    #: server has never seen it, so a push must claim parent 0.
    version: int = 0
    #: Digest at that moment. Differing from the file on disk now is exactly
    #: what "there are local changes" means.
    sha256: str = ""
    slot: str = "A"
    updated_at: float = 0.0


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class CardDirectory:
    """A directory of per-game `.raw` cards plus its sidecar state."""

    def __init__(self, root: Path) -> None:
        self.root = Path(root)
        self.root.mkdir(parents=True, exist_ok=True)
        self._state_path = self.root / STATE_FILE
        self._state: dict[str, CardState] = self._load()

    # --- paths ------------------------------------------------------------

    def path_for(self, game_id: str, slot: str = "A") -> Path:
        """Where a game's card lives.

        Slot A uses the bare `GAMEID.raw` that Nintendont writes, so a file can
        be moved between an SD card and here without renaming. Slot B, which is
        rare, gets a suffix.
        """
        game_id = game_id.upper()
        if slot.upper() == "B":
            return self.root / f"{game_id}-B.raw"
        return self.root / f"{game_id}.raw"

    def exists(self, game_id: str, slot: str = "A") -> bool:
        return self.path_for(game_id, slot).is_file()

    def read(self, game_id: str, slot: str = "A") -> bytes:
        return self.path_for(game_id, slot).read_bytes()

    def write(self, game_id: str, slot: str, image: bytes) -> Path:
        """Write a card atomically.

        Dolphin may be watching this file; a half-written card is worse than no
        card, so it lands via rename.
        """
        target = self.path_for(game_id, slot)
        target.parent.mkdir(parents=True, exist_ok=True)

        fd, tmp = tempfile.mkstemp(dir=target.parent, suffix=".tmp")
        try:
            with os.fdopen(fd, "wb") as handle:
                handle.write(image)
                handle.flush()
                os.fsync(handle.fileno())
            os.replace(tmp, target)
        except BaseException:
            Path(tmp).unlink(missing_ok=True)
            raise
        return target

    def local_digest(self, game_id: str, slot: str = "A") -> str | None:
        path = self.path_for(game_id, slot)
        if not path.is_file():
            return None
        return sha256_hex(path.read_bytes())

    def known_games(self) -> list[tuple[str, str]]:
        """(game_id, slot) for every card on disk."""
        found = []
        for path in sorted(self.root.glob("*.raw")):
            stem = path.stem
            if stem.endswith("-B"):
                found.append((stem[:-2].upper(), "B"))
            else:
                found.append((stem.upper(), "A"))
        return found

    # --- state ------------------------------------------------------------

    def _key(self, game_id: str, slot: str) -> str:
        return f"{game_id.upper()}:{slot.upper()}"

    def state(self, game_id: str, slot: str = "A") -> CardState:
        return self._state.get(self._key(game_id, slot), CardState(slot=slot.upper()))

    def remember(self, game_id: str, slot: str, version: int, sha256: str) -> None:
        import time

        self._state[self._key(game_id, slot)] = CardState(
            version=version, sha256=sha256, slot=slot.upper(), updated_at=time.time()
        )
        self._save()

    def has_local_changes(self, game_id: str, slot: str = "A") -> bool:
        """Whether the file on disk differs from what we last agreed with the
        server. This, not a timestamp, is what decides whether to push."""
        digest = self.local_digest(game_id, slot)
        if digest is None:
            return False
        return digest != self.state(game_id, slot).sha256

    def _load(self) -> dict[str, CardState]:
        if not self._state_path.is_file():
            return {}
        try:
            raw = json.loads(self._state_path.read_text(encoding="utf-8"))
        except (ValueError, OSError):
            log.warning(
                "sidecar state is unreadable; treating every card as unknown",
                extra={"path": str(self._state_path)},
            )
            return {}
        return {
            key: CardState(**value)
            for key, value in raw.items()
            if isinstance(value, dict)
        }

    def _save(self) -> None:
        payload = {key: asdict(value) for key, value in self._state.items()}
        fd, tmp = tempfile.mkstemp(dir=self.root, suffix=".tmp")
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as handle:
                json.dump(payload, handle, indent=2, sort_keys=True)
            os.replace(tmp, self._state_path)
        except BaseException:
            Path(tmp).unlink(missing_ok=True)
            raise
