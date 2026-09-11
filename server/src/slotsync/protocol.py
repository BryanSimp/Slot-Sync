"""Binary wire protocol v1 -- PLAN.md section 4, docs/PROTOCOL.md.

Fixed 96-byte big-endian header, then payload. No TLS, no JSON, no per-message
allocation on the client side: everything here has to be implementable in C
inside Nintendont's ARM kernel with a few KB of stack.

That constraint is why this module is pure functions over bytes with no state.
The header struct is fixed-size so a kernel client can fill one in place in a
static buffer, and every message is independently processable.
"""

from __future__ import annotations

import hmac
import struct
from dataclasses import dataclass, replace
from enum import IntEnum

MAGIC = b"SLOT"
VERSION = 1

HEADER_SIZE = 96
HMAC_OFFSET = 64
HMAC_SIZE = 32
NONCE_SIZE = 16

#: One chunk must fit in a single datagram under a 1500-byte MTU, with room for
#: the 96-byte header and the IP/UDP headers on top. Do not raise this: a 2006
#: WiFi stack cannot be assumed to handle fragmentation.
MAX_PAYLOAD = 1024

MAX_DATAGRAM = HEADER_SIZE + MAX_PAYLOAD

#: Header flags. Bit 0 on a PUSH_BEGIN asks the server to seed the staging
#: buffer from `parent_version` rather than from zeros, so the client only has
#: to send the chunks that changed -- docs/PROTOCOL.md, "Delta push".
#: Every other message sends flags = 0.
FLAG_DELTA = 0x0001

#: `>` means big-endian and, importantly, no alignment padding.
_HEADER = struct.Struct(">4sBBHQ6sBBIIIIII16s32s")
assert _HEADER.size == HEADER_SIZE


class MsgType(IntEnum):
    HELLO = 0x01
    PULL_REQ = 0x02
    PUSH_BEGIN = 0x03
    PUSH_CHUNK = 0x04
    PUSH_END = 0x05
    ACK = 0x06
    NACK = 0x07
    PULL_CHUNK = 0x08
    HEARTBEAT = 0x09
    PUSH_DELTA = 0x0A


class Error(IntEnum):
    BAD_HMAC = 0x01
    UNSUPPORTED_VERSION = 0x02
    MALFORMED_HEADER = 0x03
    UNKNOWN_CARD = 0x04
    CONFLICT = 0x05
    MISSING_CHUNKS = 0x06
    CHECKSUM_MISMATCH = 0x07
    FAILED_VALIDATION = 0x08
    TOO_LARGE = 0x09
    RATE_LIMITED = 0x0A
    STAGING_EXPIRED = 0x0B
    DELTA_UNAVAILABLE = 0x0C


ERROR_TEXT = {
    Error.BAD_HMAC: "bad hmac",
    Error.UNSUPPORTED_VERSION: "unsupported protocol version",
    Error.MALFORMED_HEADER: "malformed header",
    Error.UNKNOWN_CARD: "unknown game_id / slot",
    Error.CONFLICT: "conflict: parent_version is not head",
    Error.MISSING_CHUNKS: "missing chunks",
    Error.CHECKSUM_MISMATCH: "checksum mismatch on PUSH_END",
    Error.FAILED_VALIDATION: "card failed format validation",
    Error.TOO_LARGE: "too large",
    Error.RATE_LIMITED: "rate limited",
    Error.STAGING_EXPIRED: "staging buffer expired",
    Error.DELTA_UNAVAILABLE: "cannot seed a delta from that parent version",
}


class ProtocolError(ValueError):
    """The datagram is not something we can act on."""

    def __init__(self, message: str, code: Error = Error.MALFORMED_HEADER) -> None:
        super().__init__(message)
        self.code = code


@dataclass(frozen=True)
class Message:
    """One parsed datagram."""

    msg_type: int
    device_id: int
    game_id: str
    slot: int
    card_version: int = 0
    parent_version: int = 0
    offset: int = 0
    total_size: int = 0
    sequence: int = 0
    flags: int = 0
    nonce: bytes = b"\x00" * NONCE_SIZE
    payload: bytes = b""
    version: int = VERSION

    @property
    def length(self) -> int:
        return len(self.payload)


def pack(message: Message, key: bytes) -> bytes:
    """Serialise and sign a message.

    The HMAC covers the header with its own field zeroed, concatenated with the
    payload, so a signature cannot be lifted onto a different payload.
    """
    if len(message.payload) > MAX_PAYLOAD:
        raise ProtocolError(
            f"payload is {len(message.payload)} bytes, max is {MAX_PAYLOAD}"
        )

    game_id = message.game_id.encode("ascii", errors="replace")[:6]
    header = _HEADER.pack(
        MAGIC,
        message.version,
        message.msg_type,
        message.flags,
        message.device_id,
        game_id.ljust(6, b" "),
        message.slot,
        0,  # reserved
        message.card_version,
        message.parent_version,
        message.offset,
        len(message.payload),
        message.total_size,
        message.sequence,
        message.nonce,
        b"\x00" * HMAC_SIZE,
    )
    signature = _sign(header, message.payload, key)
    signed = header[:HMAC_OFFSET] + signature + header[HMAC_OFFSET + HMAC_SIZE :]
    return signed + message.payload


def unpack(datagram: bytes, key: bytes) -> Message:
    """Parse and authenticate a datagram.

    Raises ProtocolError with the wire error code that should be NACKed. The
    HMAC is checked before any field is trusted, so nothing below the signature
    check may act on header contents.
    """
    if len(datagram) < HEADER_SIZE:
        raise ProtocolError(
            f"datagram is {len(datagram)} bytes, shorter than the {HEADER_SIZE}-byte "
            f"header"
        )
    if len(datagram) > MAX_DATAGRAM:
        raise ProtocolError(
            f"datagram is {len(datagram)} bytes, max is {MAX_DATAGRAM}",
            Error.TOO_LARGE,
        )

    header = datagram[:HEADER_SIZE]
    payload = datagram[HEADER_SIZE:]
    (
        magic,
        version,
        msg_type,
        flags,
        device_id,
        game_id,
        slot,
        _reserved,
        card_version,
        parent_version,
        offset,
        length,
        total_size,
        sequence,
        nonce,
        signature,
    ) = _HEADER.unpack(header)

    if magic != MAGIC:
        raise ProtocolError(f"bad magic {magic!r}, expected {MAGIC!r}")
    if version != VERSION:
        raise ProtocolError(
            f"protocol version {version}, this server speaks {VERSION}",
            Error.UNSUPPORTED_VERSION,
        )

    expected = _sign(header, payload, key)
    if not hmac.compare_digest(signature, expected):
        raise ProtocolError("hmac does not match", Error.BAD_HMAC)

    # Only past the signature check is anything in the header trustworthy.
    if length != len(payload):
        raise ProtocolError(
            f"header declares {length} payload bytes, datagram carries {len(payload)}"
        )
    if slot not in (0, 1):
        raise ProtocolError(f"slot must be 0 or 1, got {slot}")

    return Message(
        msg_type=msg_type,
        device_id=device_id,
        game_id=game_id.decode("ascii", errors="replace").strip().strip("\x00").strip(),
        slot=slot,
        card_version=card_version,
        parent_version=parent_version,
        offset=offset,
        total_size=total_size,
        sequence=sequence,
        flags=flags,
        nonce=nonce,
        payload=payload,
        version=version,
    )


def _sign(header: bytes, payload: bytes, key: bytes) -> bytes:
    """HMAC-SHA256 over header[0:96] with the hmac field zeroed, plus payload."""
    zeroed = (
        header[:HMAC_OFFSET] + b"\x00" * HMAC_SIZE + header[HMAC_OFFSET + HMAC_SIZE :]
    )
    mac = hmac.new(key, zeroed, "sha256")
    mac.update(payload)
    return mac.digest()


def reply_to(request: Message, msg_type: int, **overrides) -> Message:
    """A response carrying the request's identity, so the client can match it.

    Defaults are merged rather than passed alongside `**overrides`, so a caller
    supplying its own `payload` replaces the default instead of colliding with
    it.
    """
    fields = {"msg_type": msg_type, "payload": b"", "nonce": b"\x00" * NONCE_SIZE}
    fields.update(overrides)
    return replace(request, **fields)


# --- chunk bitmaps --------------------------------------------------------
#
# A NACK naming missing chunks cannot always fit the whole card: a 16 MiB card
# is 16384 chunks, which needs 2048 bitmap bytes against a 1024-byte payload
# cap. So a bitmap is windowed -- the header's `sequence` field carries the
# chunk index the bitmap starts at, and the server sends as many NACKs as the
# window needs. That keeps the payload exactly what PLAN.md section 4 describes,
# a one-byte error code followed by a bitmap.
#
# Bit order is LSB-first: chunk `base + n` is `bitmap[n >> 3] & (1 << (n & 7))`,
# which is the cheapest form for the C client to walk.

#: Payload is one error-code byte plus bitmap, inside MAX_PAYLOAD.
MAX_BITMAP_BYTES = MAX_PAYLOAD - 1
BITMAP_WINDOW = MAX_BITMAP_BYTES * 8


def pack_bitmap(indices, base: int, count: int) -> bytes:
    """Bitmap of `indices` in window [base, base + count)."""
    bitmap = bytearray((count + 7) // 8)
    for index in indices:
        offset = index - base
        if 0 <= offset < count:
            bitmap[offset >> 3] |= 1 << (offset & 7)
    return bytes(bitmap)


def unpack_bitmap(bitmap: bytes, base: int) -> list[int]:
    """Chunk indices set in `bitmap`, offset by `base`."""
    out = []
    for byte_index, byte in enumerate(bitmap):
        if not byte:
            continue
        for bit in range(8):
            if byte & (1 << bit):
                out.append(base + byte_index * 8 + bit)
    return out


def bitmap_windows(indices: list[int]) -> list[tuple[int, bytes]]:
    """Split `indices` into (base, bitmap) pairs that each fit one datagram."""
    if not indices:
        return []

    windows = []
    ordered = sorted(indices)
    start = 0
    while start < len(ordered):
        base = (ordered[start] // BITMAP_WINDOW) * BITMAP_WINDOW
        limit = base + BITMAP_WINDOW
        end = start
        while end < len(ordered) and ordered[end] < limit:
            end += 1
        windows.append((base, pack_bitmap(ordered[start:end], base, BITMAP_WINDOW)))
        start = end
    return windows


def chunk_count(total_size: int) -> int:
    """Chunks a transfer of `total_size` bytes takes."""
    return (total_size + MAX_PAYLOAD - 1) // MAX_PAYLOAD
