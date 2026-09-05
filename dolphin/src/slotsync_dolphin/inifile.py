"""Minimal, surgical INI editing for Dolphin.ini.

Deliberately not `configparser`. This edits a file the user cares about and did
not ask us to reformat: configparser round-trips lose comments, normalise
spacing and can reorder keys. Here the file is a list of raw lines, and setting
a key rewrites exactly one of them. Everything else comes back byte for byte.
"""

from __future__ import annotations

import shutil
from pathlib import Path


class IniFile:
    """A line-preserving view of an INI file."""

    def __init__(self, path: Path) -> None:
        self.path = Path(path)
        self.lines: list[str] = []
        # Dolphin writes UTF-8. Paths can carry non-ASCII, so decode explicitly
        # rather than inheriting the console codepage.
        if self.path.exists():
            self.lines = self.path.read_text(encoding="utf-8").splitlines()

    # --- reading ----------------------------------------------------------

    def _section_bounds(self, section: str) -> tuple[int, int] | None:
        """Line indices [start, end) of a section's body, header excluded."""
        header = f"[{section}]"
        start = None
        for index, line in enumerate(self.lines):
            stripped = line.strip()
            if start is None:
                if stripped == header:
                    start = index + 1
            elif stripped.startswith("[") and stripped.endswith("]"):
                return start, index
        return None if start is None else (start, len(self.lines))

    def get(self, section: str, key: str) -> str | None:
        bounds = self._section_bounds(section)
        if bounds is None:
            return None

        for line in self.lines[bounds[0] : bounds[1]]:
            name, sep, value = line.partition("=")
            if sep and name.strip() == key:
                return value.strip()
        return None

    def sections(self) -> list[str]:
        return [
            line.strip()[1:-1]
            for line in self.lines
            if line.strip().startswith("[") and line.strip().endswith("]")
        ]

    # --- writing ----------------------------------------------------------

    def set(self, section: str, key: str, value: str) -> bool:
        """Set a key, creating the section or key if needed.

        Returns True if anything actually changed, so callers can skip writing
        -- and skip backing up -- when there is nothing to do.
        """
        value = str(value)
        bounds = self._section_bounds(section)

        if bounds is None:
            if self.lines and self.lines[-1].strip():
                self.lines.append("")
            self.lines.append(f"[{section}]")
            self.lines.append(f"{key} = {value}")
            return True

        start, end = bounds
        for index in range(start, end):
            name, sep, existing = self.lines[index].partition("=")
            if sep and name.strip() == key:
                if existing.strip() == value:
                    return False
                # Keep the original spacing around the delimiter.
                gap = existing[: len(existing) - len(existing.lstrip())]
                self.lines[index] = f"{name}{sep}{gap}{value}"
                return True

        # Key absent: append it at the end of the section body, skipping back
        # over blank lines so it lands with its neighbours.
        insert = end
        while insert > start and not self.lines[insert - 1].strip():
            insert -= 1
        self.lines.insert(insert, f"{key} = {value}")
        return True

    def save(self, *, backup: bool = True) -> Path | None:
        """Write the file back, optionally leaving a one-time backup.

        The backup is written once and never overwritten, so it is always the
        file as it looked before SlotSync first touched it.
        """
        made = None
        if backup and self.path.exists():
            candidate = self.path.with_suffix(self.path.suffix + ".slotsync-backup")
            if not candidate.exists():
                shutil.copy2(self.path, candidate)
                made = candidate

        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.path.write_text("\n".join(self.lines) + "\n", encoding="utf-8")
        return made
