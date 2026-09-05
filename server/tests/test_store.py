"""M1: versioning and the conflict model -- PLAN.md section 7."""

from __future__ import annotations

import pytest

from slotsync.store import (
    ConflictError,
    NotFoundError,
    Store,
    TooLargeError,
    ValidationError,
    normalise_game_id,
    normalise_slot,
)

CARD_A = b"A" * 4096
CARD_B = b"B" * 4096
CARD_C = b"C" * 4096


def push(store, *args, **kwargs):
    """Push without format validation.

    These tests are about version arithmetic, not the memory card format, so
    they use short byte strings as stand-in images. Validation on ingest gets
    its own tests below and is exercised for real over HTTP in test_api.py.
    """
    return store.push(*args, validate=False, **kwargs)


@pytest.fixture
def store(config) -> Store:
    return Store(config)


# --- identifiers ----------------------------------------------------------


@pytest.mark.parametrize(
    ("raw", "expected"),
    [
        ("GALE01", "GALE01"),
        ("gale01", "GALE01"),
        ("  GALE01  ", "GALE01"),
        ("GALE01\x00", "GALE01"),  # wire protocol pads with NULs or spaces
        ("GM4E", "GM4E"),
    ],
)
def test_game_id_normalisation(raw, expected):
    assert normalise_game_id(raw) == expected


@pytest.mark.parametrize(
    "raw", ["", "   ", "TOOLONG7", "GALE-1", "../etc", "GA/E01", None]
)
def test_bad_game_id_is_refused(raw):
    """game_id reaches URLs, so the anchored pattern is also a path guard."""
    with pytest.raises(ValidationError):
        normalise_game_id(raw)


@pytest.mark.parametrize(
    ("raw", "expected"),
    [(0, 0), (1, 1), ("0", 0), ("1", 1), ("A", 0), ("B", 1), ("a", 0), ("b", 1)],
)
def test_slot_normalisation(raw, expected):
    assert normalise_slot(raw) == expected


@pytest.mark.parametrize("raw", [2, -1, "C", "", "AB"])
def test_bad_slot_is_refused(raw):
    with pytest.raises(ValidationError):
        normalise_slot(raw)


# --- push rules -----------------------------------------------------------


def test_first_push_creates_version_1(store):
    result = push(store, "GALE01", 0, CARD_A, parent=0)
    assert (result.version, result.outcome) == (1, "created")
    assert store.head("GALE01", 0).version == 1


def test_fast_forward_increments(store):
    push(store, "GALE01", 0, CARD_A, parent=0)
    result = push(store, "GALE01", 0, CARD_B, parent=1)
    assert (result.version, result.outcome) == (2, "created")


def test_identical_image_is_a_free_no_op(store):
    """Retries must cost nothing; the UDP client depends on it."""
    push(store, "GALE01", 0, CARD_A, parent=0)
    result = push(store, "GALE01", 0, CARD_A, parent=0)
    assert (result.version, result.outcome) == (1, "unchanged")
    assert len(store.history("GALE01", 0)) == 1


def test_retry_after_a_successful_push_is_still_free(store):
    """The retry carries a now-stale parent but identical content.

    Content equality is checked before the parent, precisely so this case does
    not look like a conflict.
    """
    push(store, "GALE01", 0, CARD_A, parent=0)
    push(store, "GALE01", 0, CARD_B, parent=1)
    result = push(store, "GALE01", 0, CARD_B, parent=1)
    assert (result.version, result.outcome) == (2, "unchanged")


def test_stale_parent_is_a_conflict(store):
    push(store, "GALE01", 0, CARD_A, parent=0)
    push(store, "GALE01", 0, CARD_B, parent=1)

    with pytest.raises(ConflictError) as caught:
        push(store, "GALE01", 0, CARD_C, parent=1)

    assert caught.value.head_version == 2
    assert caught.value.head_sha256 == store.head("GALE01", 0).sha256


def test_conflict_leaves_head_untouched(store):
    push(store, "GALE01", 0, CARD_A, parent=0)
    with pytest.raises(ConflictError):
        push(store, "GALE01", 0, CARD_B, parent=99)
    assert store.read_image(store.head("GALE01", 0)) == CARD_A


def test_parent_ahead_of_head_is_also_a_conflict(store):
    push(store, "GALE01", 0, CARD_A, parent=0)
    with pytest.raises(ConflictError):
        push(store, "GALE01", 0, CARD_B, parent=7)


def test_first_push_with_a_parent_is_a_conflict(store):
    """Claiming a parent for a card the server has never seen is not a
    fast-forward; it means the client is out of step."""
    with pytest.raises(ConflictError) as caught:
        push(store, "GALE01", 0, CARD_A, parent=3)
    assert caught.value.head_version == 0
    assert caught.value.head_sha256 is None


def test_slots_are_independent(store):
    push(store, "GALE01", "A", CARD_A, parent=0)
    result = push(store, "GALE01", "B", CARD_B, parent=0)
    assert result.version == 1
    assert store.read_image(store.head("GALE01", "A")) == CARD_A
    assert store.read_image(store.head("GALE01", "B")) == CARD_B


def test_empty_body_is_refused(store):
    with pytest.raises(ValidationError):
        push(store, "GALE01", 0, b"", parent=0)


def test_oversized_card_is_refused(config):
    store = Store(config)
    with pytest.raises(TooLargeError):
        push(store, "GALE01", 0, b"x" * (config.max_card_bytes + 1), parent=0)


# --- reads ----------------------------------------------------------------


def test_image_round_trips_byte_identical(store):
    payload = bytes(range(256)) * 64
    push(store, "GALE01", 0, payload, parent=0)
    assert store.read_image(store.head("GALE01", 0)) == payload


def test_history_is_newest_first(store):
    push(store, "GALE01", 0, CARD_A, parent=0)
    push(store, "GALE01", 0, CARD_B, parent=1)
    push(store, "GALE01", 0, CARD_C, parent=2)
    assert [v.version for v in store.history("GALE01", 0)] == [3, 2, 1]


def test_version_records_its_parent(store):
    push(store, "GALE01", 0, CARD_A, parent=0)
    push(store, "GALE01", 0, CARD_B, parent=1)
    first, second = store.get_version("GALE01", 0, 1), store.get_version("GALE01", 0, 2)
    assert first.parent is None
    assert second.parent == 1


def test_missing_version_is_not_found(store):
    push(store, "GALE01", 0, CARD_A, parent=0)
    with pytest.raises(NotFoundError):
        store.get_version("GALE01", 0, 9)


def test_head_of_unknown_card_is_none(store):
    assert store.head("ZZZZ99", 0) is None


def test_list_cards_reports_head_and_count(store):
    push(store, "GALE01", 0, CARD_A, parent=0)
    push(store, "GALE01", 0, CARD_B, parent=1)
    push(store, "GM4E01", 1, CARD_C, parent=0)

    cards = {(c.game_id, c.slot): c for c in store.list_cards()}
    assert cards[("GALE01", 0)].head == 2
    assert cards[("GALE01", 0)].versions == 2
    assert cards[("GM4E01", 1)].head == 1
    assert cards[("GM4E01", 1)].size == len(CARD_C)


# --- dedupe ---------------------------------------------------------------


def test_identical_cards_on_different_slots_store_one_blob(store):
    """Content addressing means a card pushed from two places costs one copy."""
    push(store, "GALE01", "A", CARD_A, parent=0)
    push(store, "GM4E01", "B", CARD_A, parent=0)

    blobs = list(store.blobs.root.rglob("*.raw"))
    assert len(blobs) == 1


# --- rollback -------------------------------------------------------------


def test_rollback_adds_a_version_rather_than_rewriting_history(store):
    push(store, "GALE01", 0, CARD_A, parent=0)
    push(store, "GALE01", 0, CARD_B, parent=1)

    result = store.rollback("GALE01", 0, 1)

    assert result.version == 3
    assert [v.version for v in store.history("GALE01", 0)] == [3, 2, 1]
    assert store.read_image(store.head("GALE01", 0)) == CARD_A
    assert store.get_version("GALE01", 0, 3).note == "rollback to version 1"


def test_rollback_is_itself_undoable(store):
    push(store, "GALE01", 0, CARD_A, parent=0)
    push(store, "GALE01", 0, CARD_B, parent=1)
    store.rollback("GALE01", 0, 1)
    store.rollback("GALE01", 0, 2)
    assert store.read_image(store.head("GALE01", 0)) == CARD_B


def test_rollback_to_current_head_is_a_no_op(store):
    push(store, "GALE01", 0, CARD_A, parent=0)
    result = store.rollback("GALE01", 0, 1)
    assert result.outcome == "unchanged"
    assert len(store.history("GALE01", 0)) == 1


def test_rollback_to_unknown_version_is_not_found(store):
    push(store, "GALE01", 0, CARD_A, parent=0)
    with pytest.raises(NotFoundError):
        store.rollback("GALE01", 0, 42)


def test_push_after_rollback_uses_the_new_head(store):
    push(store, "GALE01", 0, CARD_A, parent=0)
    push(store, "GALE01", 0, CARD_B, parent=1)
    store.rollback("GALE01", 0, 1)  # head is now 3
    assert push(store, "GALE01", 0, CARD_C, parent=3).version == 4


# --- devices --------------------------------------------------------------


def test_device_ids_survive_the_full_u64_range(store):
    """SQLite INTEGER is signed; ids above 2**63 are stored reinterpreted."""
    big = 0xDEADBEEFCAFEBABE
    push(store, "GALE01", 0, CARD_A, parent=0, device_id=big)
    assert store.head("GALE01", 0).device_id == big


def test_touch_device_records_first_and_last_seen(store):
    store.touch_device(7, kind="wii", label="living room", now=100)
    store.touch_device(7, now=200)

    device = store.list_devices()[0]
    assert (device.device_id, device.first_seen, device.last_seen) == (7, 100, 200)
    assert (device.kind, device.label) == ("wii", "living room")


# --- validation on ingest -------------------------------------------------


def test_push_validates_the_card_format_by_default(store):
    """PLAN.md section 5: validation happens on ingest, for both transports."""
    with pytest.raises(ValidationError, match="not a valid memory card size"):
        store.push("GALE01", 0, b"not a memory card", parent=0)


def test_a_valid_card_is_accepted_through_the_validating_path(store, card_a):
    assert store.push("GALE01", 0, card_a, parent=0).version == 1


def test_a_card_with_one_damaged_copy_is_still_stored(store, card_a):
    """Refusing an upload is how a hub loses a save."""
    damaged = bytearray(card_a)
    damaged[1 * 8192 + 0x40] ^= 0xFF  # break the primary directory copy
    assert store.push("GALE01", 0, bytes(damaged), parent=0).version == 1


def test_inspect_reports_the_saves_on_a_stored_card(store, card_a):
    store.push("GALE01", 0, card_a, parent=0)
    card = store.inspect_version(store.head("GALE01", 0))
    assert [s.display_name for s in card.saves] == ["Zelda Quest Log"]
