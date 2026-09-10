"""Finding Dolphin, and pointing it at the right memory card.

The whole reason this module exists is in PLAN.md §5: Nintendont keeps one card
per game, Dolphin keeps one shared card per region. To sync between them the PC
side has to use per-game cards too, which means rewriting `MemcardAPath` in
`Dolphin.ini` before a game runs.
"""

from __future__ import annotations

import logging
import os
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

from .inifile import IniFile

log = logging.getLogger("slotsync.dolphin")

#: `SlotA`/`SlotB` in [Core] are Dolphin EXIDeviceType values, and the two that
#: matter here are easy to transpose. Confirmed against a real installation:
#: a Dolphin showing `SlotA = 8` was reading and writing `GC/USA/Card A/*.gci`
#: and ignoring `MemcardAPath` entirely. Getting these the wrong way round is
#: silent -- `point_at` decides the slot is already fine, writes the path, and
#: the game goes on saving into the folder while `push` reports no changes.
EXI_NONE = 255
EXI_MEMORY_CARD = 1
EXI_MEMORY_CARD_FOLDER = 8

SLOT_KEYS = {0: ("SlotA", "MemcardAPath"), 1: ("SlotB", "MemcardBPath")}
SLOT_NAMES = {0: "A", 1: "B"}


class DolphinError(RuntimeError):
    """Something about the Dolphin install is not usable."""


@dataclass(frozen=True)
class DolphinPaths:
    """Where a Dolphin install keeps its things."""

    user_dir: Path
    executable: Path | None = None

    @property
    def config(self) -> Path:
        return self.user_dir / "Config" / "Dolphin.ini"

    @property
    def gc_dir(self) -> Path:
        return self.user_dir / "GC"


def default_user_dir() -> Path | None:
    """Dolphin's user directory for this platform, if it exists.

    Windows put it under Documents historically and under Roaming since 5.0, so
    both are checked. An explicit --user-dir always wins over this guess.
    """
    candidates: list[Path] = []

    if sys.platform == "win32":
        appdata = os.environ.get("APPDATA")
        if appdata:
            candidates.append(Path(appdata) / "Dolphin Emulator")
        userprofile = os.environ.get("USERPROFILE")
        if userprofile:
            candidates.append(Path(userprofile) / "Documents" / "Dolphin Emulator")
    elif sys.platform == "darwin":
        candidates.append(Path.home() / "Library" / "Application Support" / "Dolphin")
    else:
        xdg = os.environ.get("XDG_DATA_HOME")
        base = Path(xdg) if xdg else Path.home() / ".local" / "share"
        candidates.append(base / "dolphin-emu")
        candidates.append(Path.home() / ".dolphin-emu")

    for candidate in candidates:
        if candidate.is_dir():
            return candidate
    return None


def find_executable() -> Path | None:
    """Dolphin's binary, for the `play` command. None if we cannot find it."""
    from shutil import which

    for name in ("Dolphin", "dolphin-emu", "Dolphin.exe", "dolphin-emu.exe"):
        found = which(name)
        if found:
            return Path(found)

    if sys.platform == "win32":
        # noqa on the casing: these are the names Windows actually sets.
        for base in (
            os.environ.get("ProgramFiles", r"C:\Program Files"),  # noqa: SIM112
            os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),  # noqa: SIM112
        ):
            candidate = Path(base) / "Dolphin" / "Dolphin.exe"
            if candidate.is_file():
                return candidate
    elif sys.platform == "darwin":
        candidate = Path("/Applications/Dolphin.app/Contents/MacOS/Dolphin")
        if candidate.is_file():
            return candidate
    return None


def is_running() -> bool:
    """Whether a Dolphin process is up.

    This matters more than it looks. Dolphin rewrites Dolphin.ini when it
    exits, from the settings it loaded at startup -- so editing the file under
    a running instance gets silently reverted, and the game plays against the
    wrong card.
    """
    try:
        if sys.platform == "win32":
            out = subprocess.run(
                ["tasklist", "/FI", "IMAGENAME eq Dolphin.exe", "/NH"],
                capture_output=True,
                text=True,
                timeout=10,
            ).stdout
            return "Dolphin.exe" in out
        out = subprocess.run(
            ["pgrep", "-x", "dolphin-emu"], capture_output=True, text=True, timeout=10
        )
        return out.returncode == 0
    except (OSError, subprocess.SubprocessError):
        # Not being able to tell is not a reason to refuse; the caller warns.
        return False


class DolphinConfig:
    """Reads and rewrites the memory card settings in Dolphin.ini."""

    def __init__(self, paths: DolphinPaths) -> None:
        self.paths = paths
        if not paths.config.is_file():
            raise DolphinError(
                f"no Dolphin.ini at {paths.config}. Run Dolphin once so it "
                f"writes its config, or pass --user-dir."
            )
        self.ini = IniFile(paths.config)

    def memcard_path(self, slot: int = 0) -> Path | None:
        _, path_key = SLOT_KEYS[slot]
        value = self.ini.get("Core", path_key)
        return Path(value) if value else None

    def slot_device(self, slot: int = 0) -> int:
        slot_key, _ = SLOT_KEYS[slot]
        value = self.ini.get("Core", slot_key)
        try:
            return int(value) if value is not None else EXI_NONE
        except ValueError:
            return EXI_NONE

    def describe(self, slot: int = 0) -> str:
        device = self.slot_device(slot)
        return {
            EXI_NONE: "nothing plugged in",
            EXI_MEMORY_CARD: "memory card file",
            EXI_MEMORY_CARD_FOLDER: "GCI folder",
        }.get(device, f"EXI device {device}")

    def point_at(self, card: Path, slot: int = 0) -> bool:
        """Point a slot at `card`, enabling the slot if needed.

        Returns True if Dolphin.ini changed. Dolphin wants forward slashes even
        on Windows, and writes them that way itself.
        """
        slot_key, path_key = SLOT_KEYS[slot]
        wanted = str(Path(card).resolve()).replace("\\", "/")

        changed = self.ini.set("Core", path_key, wanted)
        if self.slot_device(slot) != EXI_MEMORY_CARD:
            # A slot set to "nothing" or to GCI-folder mode ignores the path we
            # just wrote, and the game would silently see no card.
            log.info(
                "enabling the memory card slot",
                extra={"slot": SLOT_NAMES[slot], "was": self.describe(slot)},
            )
            self.ini.set("Core", slot_key, str(EXI_MEMORY_CARD))
            changed = True

        if changed:
            backup = self.ini.save()
            if backup is not None:
                log.info("backed up Dolphin.ini", extra={"backup": str(backup)})
        return changed
