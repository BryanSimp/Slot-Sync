"""Versioning and conflict logic -- PLAN.md section 7.

Both transports go through here. A card pushed from a Wii over UDP and one
pushed from Dolphin over HTTP are indistinguishable once committed.

The rules, in the order they are applied:

1. If the pushed image already equals head, it is a free no-op. Retries cost
   nothing, which the UDP client depends on.
2. If `parent` is head, fast-forward: the new version is head + 1.
3. Otherwise reject as a conflict. Never merge, never last-write-wins. A human
   picks in the web UI.
"""

from __future__ import annotations

import logging
import re
import time
from dataclasses import dataclass
from typing import Literal

from .blobs import BlobStore, sha256_hex
from .config import Config
from .db import closing_connection, db_to_u64, init, u64_to_db, writing
from .memcard import Card, MemcardError
from .memcard import parse as parse_memcard

log = logging.getLogger("slotsync.store")

#: GameCube game codes are six alphanumerics, e.g. GALE01. Anchored, so this
#: doubles as the guard keeping `game_id` out of filesystem paths and URLs it
#: has no business reaching.
GAME_ID_RE = re.compile(r"^[A-Z0-9]{1,6}$")

SLOT_NAMES = {0: "A", 1: "B"}


class StoreError(Exception):
    """Base for errors both transports must translate."""


class ValidationError(StoreError):
    """The request is malformed. HTTP 400, UDP NACK 0x03."""


class NotFoundError(StoreError):
    """No such card, version, or blob. HTTP 404, UDP NACK 0x04."""


class TooLargeError(StoreError):
    """Image exceeds the configured cap. HTTP 413, UDP NACK 0x09."""


class ConflictError(StoreError):
    """Push parent is not head. HTTP 409, UDP NACK 0x05.

    Carries the current head so the client can surface a choice rather than
    guess -- PLAN.md sections 7 and 8.
    """

    def __init__(self, head_version: int, head_sha256: str | None) -> None:
        super().__init__(f"parent is not head; head is version {head_version}")
        self.head_version = head_version
        self.head_sha256 = head_sha256


@dataclass(frozen=True)
class Version:
    """One committed card image."""

    game_id: str
    slot: int
    version: int
    parent: int | None
    sha256: str
    size: int
    device_id: int | None
    created_at: int
    note: str | None

    @property
    def slot_name(self) -> str:
        return SLOT_NAMES.get(self.slot, str(self.slot))


@dataclass(frozen=True)
class CardSummary:
    """A card as the listing endpoint sees it."""

    game_id: str
    slot: int
    head: int
    sha256: str
    size: int
    device_id: int | None
    updated_at: int
    versions: int

    @property
    def slot_name(self) -> str:
        return SLOT_NAMES.get(self.slot, str(self.slot))


@dataclass(frozen=True)
class Device:
    device_id: int
    label: str | None
    kind: str | None
    first_seen: int
    last_seen: int


@dataclass(frozen=True)
class PushResult:
    version: int
    #: "created" for a new version, "unchanged" for an idempotent retry.
    outcome: Literal["created", "unchanged"]

    @property
    def created(self) -> bool:
        return self.outcome == "created"


def normalise_game_id(raw: str) -> str:
    """Validate and canonicalise a game code.

    The wire protocol space-pads to six bytes; HTTP callers send it bare. Both
    land on the same stripped, uppercase form.
    """
    value = (raw or "").strip().strip("\x00").strip().upper()
    if not GAME_ID_RE.match(value):
        raise ValidationError(f"game_id must be 1-6 alphanumeric characters, got {raw!r}")
    return value


def normalise_slot(raw: str | int) -> int:
    """Accept 0/1 from the wire and A/B from humans and URLs."""
    if isinstance(raw, bool):
        raise ValidationError(f"slot must be 0, 1, A or B, got {raw!r}")
    if isinstance(raw, int):
        value = raw
    else:
        text = str(raw).strip().upper()
        if text in ("A", "B"):
            return 0 if text == "A" else 1
        try:
            value = int(text)
        except ValueError:
            raise ValidationError(f"slot must be 0, 1, A or B, got {raw!r}") from None
    if value not in (0, 1):
        raise ValidationError(f"slot must be 0 (A) or 1 (B), got {raw!r}")
    return value


class Store:
    """Storage and versioning. Transport-agnostic."""

    def __init__(self, config: Config) -> None:
        self.config = config
        self.db_path = config.db_path
        self.blobs = BlobStore(config.blobs_dir)
        init(self.db_path)

    # --- reads ------------------------------------------------------------

    def head(self, game_id: str, slot: int | str) -> Version | None:
        """Current version of a card, or None if it has never been pushed."""
        game_id, slot = normalise_game_id(game_id), normalise_slot(slot)
        with closing_connection(self.db_path) as conn:
            return self._head(conn, game_id, slot)

    def get_version(self, game_id: str, slot: int | str, version: int) -> Version:
        game_id, slot = normalise_game_id(game_id), normalise_slot(slot)
        with closing_connection(self.db_path) as conn:
            row = conn.execute(
                "SELECT * FROM versions WHERE game_id=? AND slot=? AND version=?",
                (game_id, slot, version),
            ).fetchone()
        if row is None:
            raise NotFoundError(
                f"no version {version} of {game_id} slot {SLOT_NAMES.get(slot, slot)}"
            )
        return _version_from_row(row)

    def history(self, game_id: str, slot: int | str) -> list[Version]:
        """Every version, newest first."""
        game_id, slot = normalise_game_id(game_id), normalise_slot(slot)
        with closing_connection(self.db_path) as conn:
            rows = conn.execute(
                "SELECT * FROM versions WHERE game_id=? AND slot=? ORDER BY version DESC",
                (game_id, slot),
            ).fetchall()
        return [_version_from_row(row) for row in rows]

    def list_cards(self) -> list[CardSummary]:
        with closing_connection(self.db_path) as conn:
            rows = conn.execute(
                """
                SELECT c.game_id, c.slot, c.head, v.sha256, v.size, v.device_id,
                       v.created_at,
                       (SELECT COUNT(*) FROM versions n
                         WHERE n.game_id = c.game_id AND n.slot = c.slot) AS versions
                  FROM cards c
                  JOIN versions v
                    ON v.game_id = c.game_id AND v.slot = c.slot
                   AND v.version = c.head
                 ORDER BY v.created_at DESC, c.game_id, c.slot
                """
            ).fetchall()
        return [
            CardSummary(
                game_id=row["game_id"],
                slot=row["slot"],
                head=row["head"],
                sha256=row["sha256"],
                size=row["size"],
                device_id=(
                    None if row["device_id"] is None else db_to_u64(row["device_id"])
                ),
                updated_at=row["created_at"],
                versions=row["versions"],
            )
            for row in rows
        ]

    def read_image(self, version: Version) -> bytes:
        try:
            return self.blobs.get(version.sha256)
        except FileNotFoundError:
            raise NotFoundError(
                f"blob {version.sha256} is missing from the store"
            ) from None

    @staticmethod
    def inspect(data: bytes) -> Card:
        """Parse an image, translating parser errors into a ValidationError.

        Parsed results are recomputed rather than stored: blobs are immutable,
        so the answer never goes stale, and a card is only parsed when someone
        actually looks at it.
        """
        try:
            return parse_memcard(data)
        except MemcardError as exc:
            raise ValidationError(str(exc)) from exc

    def inspect_version(self, version: Version) -> Card:
        return self.inspect(self.read_image(version))

    def list_devices(self) -> list[Device]:
        with closing_connection(self.db_path) as conn:
            rows = conn.execute(
                "SELECT * FROM devices ORDER BY last_seen DESC"
            ).fetchall()
        return [
            Device(
                device_id=db_to_u64(row["device_id"]),
                label=row["label"],
                kind=row["kind"],
                first_seen=row["first_seen"],
                last_seen=row["last_seen"],
            )
            for row in rows
        ]

    # --- writes -----------------------------------------------------------

    def push(
        self,
        game_id: str,
        slot: int | str,
        data: bytes,
        *,
        parent: int,
        device_id: int | None = None,
        note: str | None = None,
        now: int | None = None,
        validate: bool = True,
    ) -> PushResult:
        """Commit a whole card image. See the rules at the top of this module."""
        game_id, slot = normalise_game_id(game_id), normalise_slot(slot)

        if not data:
            raise ValidationError("empty body; expected a raw memory card image")
        if len(data) > self.config.max_card_bytes:
            raise TooLargeError(
                f"card is {len(data)} bytes, cap is {self.config.max_card_bytes}"
            )
        if parent < 0:
            raise ValidationError(f"parent must not be negative, got {parent}")

        if validate:
            # Validation on ingest -- PLAN.md section 5. A card that fails here
            # is structurally not a memory card, so storing it would only mean
            # handing corruption back to a console later.
            card = self.inspect(data)
            if card.warnings:
                log.warning(
                    "accepting a card with damage",
                    extra={
                        "game_id": game_id,
                        "slot": slot,
                        "warnings": card.warnings,
                    },
                )

        digest = sha256_hex(data)
        created_at = int(time.time()) if now is None else now

        # The head read, the blob write and the insert all sit inside one
        # IMMEDIATE transaction, so two concurrent pushes cannot both observe
        # the same head and both claim head + 1. Holding the write lock across
        # a 2 MiB file write costs a few milliseconds, which is nothing at this
        # traffic level and buys a much simpler correctness argument.
        with writing(self.db_path) as conn:
            current = self._head(conn, game_id, slot)
            head_version = 0 if current is None else current.version

            if current is not None and current.sha256 == digest:
                log.info(
                    "push is a no-op, image already at head",
                    extra={"game_id": game_id, "slot": slot, "version": head_version},
                )
                return PushResult(version=head_version, outcome="unchanged")

            if parent != head_version:
                log.info(
                    "push rejected as a conflict",
                    extra={
                        "game_id": game_id,
                        "slot": slot,
                        "parent": parent,
                        "head": head_version,
                    },
                )
                raise ConflictError(
                    head_version, None if current is None else current.sha256
                )

            self.blobs.put(data)
            new_version = head_version + 1
            self._insert_version(
                conn,
                game_id=game_id,
                slot=slot,
                version=new_version,
                parent=head_version or None,
                sha256=digest,
                size=len(data),
                device_id=device_id,
                created_at=created_at,
                note=note,
            )

        log.info(
            "committed version",
            extra={
                "game_id": game_id,
                "slot": slot,
                "version": new_version,
                "sha256": digest,
                "size": len(data),
                "device_id": device_id,
            },
        )
        return PushResult(version=new_version, outcome="created")

    def rollback(
        self,
        game_id: str,
        slot: int | str,
        to_version: int,
        *,
        device_id: int | None = None,
        now: int | None = None,
    ) -> PushResult:
        """Make `to_version` the head again, as a fresh version.

        History is append-only: a rollback adds a version rather than discarding
        the ones after it, so the rollback is itself undoable.
        """
        game_id, slot = normalise_game_id(game_id), normalise_slot(slot)
        created_at = int(time.time()) if now is None else now

        with writing(self.db_path) as conn:
            row = conn.execute(
                "SELECT * FROM versions WHERE game_id=? AND slot=? AND version=?",
                (game_id, slot, to_version),
            ).fetchone()
            if row is None:
                raise NotFoundError(
                    f"no version {to_version} of {game_id} "
                    f"slot {SLOT_NAMES.get(slot, slot)}"
                )
            target = _version_from_row(row)

            current = self._head(conn, game_id, slot)
            head_version = 0 if current is None else current.version

            if current is not None and current.sha256 == target.sha256:
                return PushResult(version=head_version, outcome="unchanged")

            new_version = head_version + 1
            self._insert_version(
                conn,
                game_id=game_id,
                slot=slot,
                version=new_version,
                parent=head_version or None,
                sha256=target.sha256,
                size=target.size,
                device_id=device_id,
                created_at=created_at,
                note=f"rollback to version {to_version}",
            )

        log.info(
            "rolled back",
            extra={
                "game_id": game_id,
                "slot": slot,
                "version": new_version,
                "restored_from": to_version,
            },
        )
        return PushResult(version=new_version, outcome="created")

    def touch_device(
        self,
        device_id: int,
        *,
        kind: str | None = None,
        label: str | None = None,
        now: int | None = None,
    ) -> None:
        """Record that a device spoke to us."""
        seen = int(time.time()) if now is None else now
        stored = u64_to_db(device_id)
        with writing(self.db_path) as conn:
            conn.execute(
                """
                INSERT INTO devices (device_id, label, kind, first_seen, last_seen)
                     VALUES (?, ?, ?, ?, ?)
                ON CONFLICT (device_id) DO UPDATE SET
                     last_seen = excluded.last_seen,
                     kind      = COALESCE(excluded.kind, devices.kind),
                     label     = COALESCE(excluded.label, devices.label)
                """,
                (stored, label, kind, seen, seen),
            )

    # --- internals --------------------------------------------------------

    @staticmethod
    def _head(conn, game_id: str, slot: int) -> Version | None:
        row = conn.execute(
            """
            SELECT v.* FROM cards c
              JOIN versions v
                ON v.game_id = c.game_id AND v.slot = c.slot AND v.version = c.head
             WHERE c.game_id = ? AND c.slot = ?
            """,
            (game_id, slot),
        ).fetchone()
        return None if row is None else _version_from_row(row)

    @staticmethod
    def _insert_version(conn, **f) -> None:
        conn.execute(
            """
            INSERT INTO versions
                   (game_id, slot, version, parent, sha256, size, device_id,
                    created_at, note)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            (
                f["game_id"],
                f["slot"],
                f["version"],
                f["parent"],
                f["sha256"],
                f["size"],
                None if f["device_id"] is None else u64_to_db(f["device_id"]),
                f["created_at"],
                f["note"],
            ),
        )
        conn.execute(
            """
            INSERT INTO cards (game_id, slot, head) VALUES (?, ?, ?)
            ON CONFLICT (game_id, slot) DO UPDATE SET head = excluded.head
            """,
            (f["game_id"], f["slot"], f["version"]),
        )


def _version_from_row(row) -> Version:
    return Version(
        game_id=row["game_id"],
        slot=row["slot"],
        version=row["version"],
        parent=row["parent"],
        sha256=row["sha256"],
        size=row["size"],
        device_id=None if row["device_id"] is None else db_to_u64(row["device_id"]),
        created_at=row["created_at"],
        note=row["note"],
    )
