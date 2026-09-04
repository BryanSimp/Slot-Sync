"""SQLite index. Schema is PLAN.md section 6.

No ORM, by decision. Connections are opened per operation rather than shared:
the HTTP handlers run in a threadpool and the UDP listener runs on the event
loop, and a connection per call sidesteps sqlite3's thread affinity entirely.
At a couple of megabytes every few minutes this costs nothing.
"""

from __future__ import annotations

import sqlite3
from collections.abc import Iterator
from contextlib import contextmanager
from pathlib import Path

SCHEMA = """
CREATE TABLE IF NOT EXISTS devices (
  device_id   INTEGER PRIMARY KEY,   -- u64 from the wire protocol
  label       TEXT,
  kind        TEXT,                  -- 'wii' | 'vwii' | 'dolphin'
  first_seen  INTEGER NOT NULL,
  last_seen   INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS cards (
  game_id     TEXT NOT NULL,
  slot        INTEGER NOT NULL,
  head        INTEGER NOT NULL,      -- current card_version
  PRIMARY KEY (game_id, slot)
);

CREATE TABLE IF NOT EXISTS versions (
  game_id     TEXT NOT NULL,
  slot        INTEGER NOT NULL,
  version     INTEGER NOT NULL,
  parent      INTEGER,
  sha256      TEXT NOT NULL,
  size        INTEGER NOT NULL,
  device_id   INTEGER,
  created_at  INTEGER NOT NULL,
  note        TEXT,
  PRIMARY KEY (game_id, slot, version)
);

CREATE INDEX IF NOT EXISTS versions_by_sha ON versions (sha256);
CREATE INDEX IF NOT EXISTS versions_by_time ON versions (created_at DESC);
"""


def connect(path: Path | str) -> sqlite3.Connection:
    """Open a tuned connection. Caller closes it."""
    conn = sqlite3.connect(
        path,
        timeout=30.0,  # wait out a writer rather than raising "database is locked"
        isolation_level=None,  # explicit transactions; see `writing()`
    )
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA synchronous=NORMAL")
    conn.execute("PRAGMA foreign_keys=ON")
    return conn


def init(path: Path | str) -> None:
    """Create the schema if it is not there. Safe to call on every boot."""
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    with closing_connection(path) as conn:
        conn.executescript(SCHEMA)


@contextmanager
def closing_connection(path: Path | str) -> Iterator[sqlite3.Connection]:
    conn = connect(path)
    try:
        yield conn
    finally:
        conn.close()


@contextmanager
def writing(path: Path | str) -> Iterator[sqlite3.Connection]:
    """A write transaction that serialises against other writers.

    BEGIN IMMEDIATE takes the write lock up front, so two pushes racing for the
    same (game_id, slot) cannot both read the same head and both claim head + 1.
    """
    conn = connect(path)
    try:
        conn.execute("BEGIN IMMEDIATE")
        try:
            yield conn
        except BaseException:
            conn.execute("ROLLBACK")
            raise
        else:
            conn.execute("COMMIT")
    finally:
        conn.close()


# --- u64 device ids -------------------------------------------------------
#
# The wire protocol carries device_id as u64 (PLAN.md section 4) but SQLite's
# INTEGER is a signed 64-bit value, so ids at or above 2**63 do not survive a
# round trip unchanged. Store the two's-complement reinterpretation and convert
# at the boundary; the mapping is a bijection, so equality and lookups behave.

U64_MAX = 2**64 - 1


def u64_to_db(value: int) -> int:
    """Reinterpret a u64 as the signed integer SQLite will store."""
    if not 0 <= value <= U64_MAX:
        raise ValueError(f"device_id out of u64 range: {value}")
    return value - 2**64 if value >= 2**63 else value


def db_to_u64(value: int) -> int:
    """Inverse of `u64_to_db`."""
    return value + 2**64 if value < 0 else value
