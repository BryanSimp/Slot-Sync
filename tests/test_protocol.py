"""M4: the binary wire format -- PLAN.md section 4, docs/PROTOCOL.md.

The field offsets are asserted against the literal numbers from the spec rather
than against the struct format string, because the struct string is the thing
most likely to be wrong.
"""

from __future__ import annotations

import struct

import pytest

from slotsync.protocol import (
    BITMAP_WINDOW,
    HEADER_SIZE,
    MAGIC,
    MAX_PAYLOAD,
    Error,
    Message,
    MsgType,
    ProtocolError,
    bitmap_windows,
    chunk_count,
    pack,
    reply_to,
    unpack,
    unpack_bitmap,
)

KEY = b"a-pre-shared-key"


def message(**overrides) -> Message:
    fields = {
        "msg_type": MsgType.PUSH_CHUNK,
        "device_id": 0xDEADBEEFCAFEBABE,
        "game_id": "GALE01",
        "slot": 1,
        "card_version": 7,
        "parent_version": 6,
        "offset": 2048,
        "total_size": 2097152,
        "sequence": 2,
        "nonce": bytes(range(16)),
        "payload": b"hello world",
    }
    return Message(**(fields | overrides))


# --- layout ---------------------------------------------------------------


def test_header_is_96_bytes():
    assert len(pack(Message(MsgType.HELLO, 0, "", 0), KEY)) == HEADER_SIZE


def test_field_offsets_match_the_spec():
    """Offsets from PLAN.md section 4, read straight out of the packed bytes."""
    raw = pack(message(), KEY)

    assert raw[0:4] == MAGIC
    assert raw[4] == 1  # version
    assert raw[5] == MsgType.PUSH_CHUNK
    assert struct.unpack_from(">H", raw, 6)[0] == 0  # flags
    assert struct.unpack_from(">Q", raw, 8)[0] == 0xDEADBEEFCAFEBABE
    assert raw[16:22] == b"GALE01"
    assert raw[22] == 1  # slot
    assert raw[23] == 0  # reserved
    assert struct.unpack_from(">I", raw, 24)[0] == 7  # card_version
    assert struct.unpack_from(">I", raw, 28)[0] == 6  # parent_version
    assert struct.unpack_from(">I", raw, 32)[0] == 2048  # offset
    assert struct.unpack_from(">I", raw, 36)[0] == 11  # length
    assert struct.unpack_from(">I", raw, 40)[0] == 2097152  # total_size
    assert struct.unpack_from(">I", raw, 44)[0] == 2  # sequence
    assert raw[48:64] == bytes(range(16))  # nonce
    assert len(raw[64:96]) == 32  # hmac
    assert raw[96:] == b"hello world"


def test_multibyte_fields_are_big_endian():
    raw = pack(message(sequence=1), KEY)
    assert raw[44:48] == b"\x00\x00\x00\x01"


def test_game_id_is_space_padded_to_six_bytes():
    raw = pack(message(game_id="GM4E"), KEY)
    assert raw[16:22] == b"GM4E  "
    assert unpack(raw, KEY).game_id == "GM4E"


def test_round_trip_preserves_every_field():
    original = message()
    assert unpack(pack(original, KEY), KEY) == original


# --- authentication -------------------------------------------------------


def test_a_tampered_payload_is_rejected():
    raw = bytearray(pack(message(), KEY))
    raw[-1] ^= 0xFF
    with pytest.raises(ProtocolError) as caught:
        unpack(bytes(raw), KEY)
    assert caught.value.code is Error.BAD_HMAC


def test_a_tampered_header_is_rejected():
    """The hmac covers the header too, so a flipped offset cannot slip through."""
    raw = bytearray(pack(message(), KEY))
    raw[32] ^= 0xFF  # offset field
    with pytest.raises(ProtocolError) as caught:
        unpack(bytes(raw), KEY)
    assert caught.value.code is Error.BAD_HMAC


def test_a_signature_cannot_be_lifted_onto_another_payload():
    signed = pack(message(payload=b"A" * 16), KEY)
    forged = signed[:HEADER_SIZE] + b"B" * 16
    with pytest.raises(ProtocolError) as caught:
        unpack(forged, KEY)
    assert caught.value.code is Error.BAD_HMAC


def test_the_wrong_key_is_rejected():
    with pytest.raises(ProtocolError) as caught:
        unpack(pack(message(), KEY), b"a-different-key")
    assert caught.value.code is Error.BAD_HMAC


def test_the_nonce_changes_the_signature():
    a = pack(message(nonce=b"\x00" * 16), KEY)
    b = pack(message(nonce=b"\x01" * 16), KEY)
    assert a[64:96] != b[64:96]


# --- malformed input ------------------------------------------------------


def test_bad_magic_is_rejected():
    raw = b"NOPE" + pack(message(), KEY)[4:]
    with pytest.raises(ProtocolError, match="bad magic"):
        unpack(raw, KEY)


def test_an_unknown_protocol_version_says_so():
    raw = bytearray(pack(message(), KEY))
    raw[4] = 99
    with pytest.raises(ProtocolError) as caught:
        unpack(bytes(raw), KEY)
    assert caught.value.code is Error.UNSUPPORTED_VERSION


@pytest.mark.parametrize("size", [0, 1, 95])
def test_a_datagram_shorter_than_the_header_is_rejected(size):
    with pytest.raises(ProtocolError, match="shorter than"):
        unpack(b"\x00" * size, KEY)


def test_an_oversized_datagram_is_rejected():
    with pytest.raises(ProtocolError) as caught:
        unpack(b"\x00" * (HEADER_SIZE + MAX_PAYLOAD + 1), KEY)
    assert caught.value.code is Error.TOO_LARGE


def test_a_lying_length_field_is_rejected():
    """Checked after the hmac, so this only catches a validly signed liar."""
    raw = bytearray(pack(message(payload=b"12345"), KEY))
    del raw[HEADER_SIZE + 2 :]  # keep the header's claim of 5, send 2
    # Re-sign so the length check, not the hmac, is what fails.
    resigned = pack(Message(MsgType.PUSH_CHUNK, 0, "GALE01", 0, payload=b"12"), KEY)
    header = bytearray(resigned[:HEADER_SIZE])
    struct.pack_into(">I", header, 36, 5)
    with pytest.raises(ProtocolError):
        unpack(bytes(header) + b"12", KEY)


def test_a_payload_over_the_cap_cannot_be_packed():
    with pytest.raises(ProtocolError, match="max is"):
        pack(message(payload=b"x" * (MAX_PAYLOAD + 1)), KEY)


def test_a_payload_at_exactly_the_cap_is_fine():
    raw = pack(message(payload=b"x" * MAX_PAYLOAD), KEY)
    assert len(unpack(raw, KEY).payload) == MAX_PAYLOAD


def test_a_bad_slot_is_rejected():
    raw = bytearray(pack(message(), KEY))
    raw[22] = 5
    with pytest.raises(ProtocolError):
        unpack(bytes(raw), KEY)


# --- replies --------------------------------------------------------------


def test_reply_carries_the_request_identity():
    request = message()
    ack = reply_to(request, MsgType.ACK)
    assert ack.msg_type == MsgType.ACK
    assert (ack.game_id, ack.slot, ack.sequence) == ("GALE01", 1, 2)
    assert ack.payload == b""


def test_reply_accepts_its_own_payload():
    """Regression: `payload` was passed both as a default and via overrides,
    so any reply carrying a body raised TypeError."""
    ack = reply_to(message(), MsgType.ACK, payload=b"body")
    assert ack.payload == b"body"


def test_reply_overrides_win_over_the_request():
    ack = reply_to(message(), MsgType.ACK, card_version=42)
    assert ack.card_version == 42


# --- chunk bitmaps --------------------------------------------------------


def test_bitmap_round_trip():
    indices = [0, 1, 7, 8, 9, 100, 1023]
    windows = bitmap_windows(indices)
    recovered = []
    for base, bitmap in windows:
        recovered += unpack_bitmap(bitmap, base)
    assert recovered == sorted(indices)


def test_bitmap_bit_order_is_lsb_first():
    """Chunk `base + n` is bit (n & 7) of byte (n >> 3) -- the cheapest form
    for the C client to walk."""
    (_, bitmap) = bitmap_windows([0])[0]
    assert bitmap[0] == 0b0000_0001

    (_, bitmap) = bitmap_windows([3])[0]
    assert bitmap[0] == 0b0000_1000

    (_, bitmap) = bitmap_windows([8])[0]
    assert bitmap[0] == 0 and bitmap[1] == 0b0000_0001


def test_an_empty_bitmap_produces_no_windows():
    assert bitmap_windows([]) == []


def test_a_large_card_needs_more_than_one_window():
    """A 16 MiB card is 16384 chunks, needing 2048 bitmap bytes against a
    1024-byte payload cap -- so the bitmap is windowed."""
    indices = list(range(16384))
    windows = bitmap_windows(indices)

    assert len(windows) > 1
    for _, bitmap in windows:
        assert len(bitmap) + 1 <= MAX_PAYLOAD  # +1 for the error-code byte

    recovered = []
    for base, bitmap in windows:
        recovered += unpack_bitmap(bitmap, base)
    assert recovered == indices


def test_windows_are_based_at_window_boundaries():
    ((base, _),) = bitmap_windows([BITMAP_WINDOW + 5])
    assert base == BITMAP_WINDOW


@pytest.mark.parametrize(
    ("total", "expected"),
    [(0, 0), (1, 1), (1024, 1), (1025, 2), (2097152, 2048), (16777216, 16384)],
)
def test_chunk_count(total, expected):
    assert chunk_count(total) == expected
