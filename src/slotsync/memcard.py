"""GameCube memory card parser -- PLAN.md section 5.

Enough of the format to validate an upload and to show what is actually on a
card. Offsets here were checked against Dolphin's
`Source/Core/Core/HW/GCMemcard/GCMemcard.{h,cpp}` and YAGCD chapter 12 rather
than recalled; `docs/MEMCARD.md` records the layout and the two places where
those sources disagree.

Everything multi-byte is big-endian.
"""

from __future__ import annotations

import datetime as dt
import struct
from dataclasses import dataclass, field

# --- constants ------------------------------------------------------------

BLOCK_SIZE = 0x2000
FST_BLOCKS = 5  # header, directory, directory backup, BAT, BAT backup
DIR_ENTRIES = 0x7F  # 127
DENTRY_SIZE = 0x40
DENTRY_STRLEN = 0x20
BAT_MAP_ENTRIES = 0xFFB  # 4091
MBIT_TO_BLOCKS = 16

#: Block indices, as laid out in the file.
BLOCK_HEADER = 0
BLOCK_DIR = 1
BLOCK_DIR_BACKUP = 2
BLOCK_BAT = 3
BLOCK_BAT_BACKUP = 4

#: The six sizes real hardware produces, in Mbit. PLAN.md section 5 listed four;
#: Dolphin accepts these six, and rejecting a valid 1 MiB or 4 MiB card would
#: mean refusing to back up somebody's save.
VALID_MBIT = (4, 8, 16, 32, 64, 128)

#: file size in bytes -> size in Mbit
VALID_SIZES = {mbit * MBIT_TO_BLOCKS * BLOCK_SIZE: mbit for mbit in VALID_MBIT}

#: Card the project defaults to: Memory Card 251, 16 Mbit, 2 MiB.
DEFAULT_MBIT = 16

# Directory block
DIR_UPDATE_COUNTER_OFF = 0x1FFA
DIR_CHECKSUM_OFF = 0x1FFC

# Header block
HEADER_CHECKSUM_OFF = 0x01FC

# BAT block
BAT_CHECKSUM_OFF = 0x0000
BAT_UPDATE_COUNTER_OFF = 0x0004
BAT_MAP_OFF = 0x000A

#: A directory entry whose game code is all 0xFF is an empty slot.
UNUSED_GAMECODE = b"\xff\xff\xff\xff"

#: Values in the BAT map that are not a next-block pointer.
BAT_FREE = 0x0000
BAT_END_OF_CHAIN = 0xFFFF

#: Directory entry modification times count from this epoch, not from 1970.
GC_EPOCH = dt.datetime(2000, 1, 1, tzinfo=dt.UTC)

ENCODING_NAMES = {0: "cp1252", 1: "shift_jis"}


class MemcardError(ValueError):
    """The image is not a memory card we are willing to store."""


# --- checksums ------------------------------------------------------------


def checksums(data: bytes) -> tuple[int, int]:
    """The additive and inverse checksums over `data`.

    Big-endian u16 words summed mod 2**16. A result of 0xFFFF is stored as 0,
    which is what the GC BIOS and Dolphin both do.
    """
    if len(data) % 2:
        raise ValueError("checksum range must be an even number of bytes")

    total = 0
    inverse = 0
    for (word,) in struct.iter_unpack(">H", data):
        total += word
        inverse += word ^ 0xFFFF

    total &= 0xFFFF
    inverse &= 0xFFFF
    return (0 if total == 0xFFFF else total, 0 if inverse == 0xFFFF else inverse)


def _stored_checksums(block: bytes, offset: int) -> tuple[int, int]:
    return struct.unpack_from(">HH", block, offset)


def _checksum_ok(block: bytes, *, area: slice, stored_at: int) -> bool:
    return checksums(block[area]) == _stored_checksums(block, stored_at)


def header_checksum_ok(block: bytes) -> bool:
    """Header checksum covers [0x0000, 0x01FC)."""
    return _checksum_ok(
        block, area=slice(0, HEADER_CHECKSUM_OFF), stored_at=HEADER_CHECKSUM_OFF
    )


def directory_checksum_ok(block: bytes) -> bool:
    """Directory checksum covers [0x0000, 0x1FFC) -- everything before it."""
    return _checksum_ok(
        block, area=slice(0, DIR_CHECKSUM_OFF), stored_at=DIR_CHECKSUM_OFF
    )


def bat_checksum_ok(block: bytes) -> bool:
    """BAT checksum covers [0x0004, 0x2000) -- everything after it.

    Mirrored from the directory, which is exactly the detail that gets written
    backwards from memory.
    """
    return _checksum_ok(
        block,
        area=slice(BAT_UPDATE_COUNTER_OFF, BLOCK_SIZE),
        stored_at=BAT_CHECKSUM_OFF,
    )


# --- model ----------------------------------------------------------------


@dataclass(frozen=True)
class Save:
    """One entry in the card's directory: a single game's save file."""

    index: int
    game_code: str
    maker_code: str
    filename: str
    #: The two 32-byte strings at `comments_address`. These are the human
    #: readable names -- "The Legend of Zelda", "Quest Log 3" -- and are what
    #: the web UI should show.
    title: str
    subtitle: str
    first_block: int
    block_count: int
    modified: dt.datetime | None
    permissions: int
    copy_counter: int
    banner_format: int
    icon_format: int
    comments_address: int

    @property
    def size_bytes(self) -> int:
        return self.block_count * BLOCK_SIZE

    @property
    def no_copy(self) -> bool:
        return bool(self.permissions & 0x08)

    @property
    def no_move(self) -> bool:
        return bool(self.permissions & 0x10)

    @property
    def display_name(self) -> str:
        """Best human-readable label, falling back through what is available."""
        return self.title or self.filename or self.game_code


@dataclass(frozen=True)
class Card:
    """A parsed memory card image."""

    size_bytes: int
    size_mbits: int
    total_blocks: int
    data_blocks: int
    serial: bytes
    formatted_at: dt.datetime | None
    encoding: int
    device_id: int
    active_directory: int
    active_bat: int
    free_blocks: int
    saves: list[Save]
    #: Non-fatal problems. A card with warnings is stored, and the UI says so.
    warnings: list[str] = field(default_factory=list)

    @property
    def used_blocks(self) -> int:
        return self.data_blocks - self.free_blocks

    @property
    def encoding_name(self) -> str:
        return ENCODING_NAMES.get(self.encoding, f"unknown({self.encoding})")

    @property
    def is_shift_jis(self) -> bool:
        return self.encoding == 1


# --- parsing --------------------------------------------------------------


def parse(image: bytes) -> Card:
    """Parse and validate a raw memory card image.

    Raises MemcardError for anything we refuse to store. Recoverable damage
    lands in `Card.warnings` instead -- see PLAN.md section 5.
    """
    size_mbits = _validate_size(image)
    total_blocks = size_mbits * MBIT_TO_BLOCKS
    data_blocks = total_blocks - FST_BLOCKS

    header = _block(image, BLOCK_HEADER)
    if not header_checksum_ok(header):
        raise MemcardError(
            "header block checksum does not match; this is not a valid memory card image"
        )

    warnings: list[str] = []
    directory, active_directory = _pick_copy(
        image,
        BLOCK_DIR,
        directory_checksum_ok,
        DIR_UPDATE_COUNTER_OFF,
        label="directory",
        warnings=warnings,
    )
    bat, active_bat = _pick_copy(
        image,
        BLOCK_BAT,
        bat_checksum_ok,
        BAT_UPDATE_COUNTER_OFF,
        label="block allocation table",
        warnings=warnings,
    )

    declared_mbits = struct.unpack_from(">H", header, 0x0022)[0]
    if declared_mbits != size_mbits:
        warnings.append(
            f"header declares {declared_mbits} Mbit but the file is {size_mbits} Mbit"
        )

    encoding = struct.unpack_from(">H", header, 0x0024)[0]
    device_id = struct.unpack_from(">H", header, 0x0020)[0]
    free_blocks = struct.unpack_from(">H", bat, 0x0006)[0]

    saves = _parse_directory(directory, bat, image, data_blocks, encoding, warnings)

    return Card(
        size_bytes=len(image),
        size_mbits=size_mbits,
        total_blocks=total_blocks,
        data_blocks=data_blocks,
        serial=bytes(header[0x0000:0x000C]),
        formatted_at=_ostime(struct.unpack_from(">Q", header, 0x000C)[0]),
        encoding=encoding,
        device_id=device_id,
        active_directory=active_directory,
        active_bat=active_bat,
        free_blocks=free_blocks,
        saves=saves,
        warnings=warnings,
    )


def _validate_size(image: bytes) -> int:
    size_mbits = VALID_SIZES.get(len(image))
    if size_mbits is None:
        valid = ", ".join(
            f"{size // 1024} KiB"
            if size < 1024 * 1024
            else f"{size // (1024 * 1024)} MiB"
            for size in sorted(VALID_SIZES)
        )
        raise MemcardError(
            f"{len(image)} bytes is not a valid memory card size; "
            f"expected one of: {valid}"
        )
    return size_mbits


def _block(image: bytes, index: int) -> bytes:
    return image[index * BLOCK_SIZE : (index + 1) * BLOCK_SIZE]


def _pick_copy(
    image: bytes,
    first_index: int,
    check,
    counter_offset: int,
    *,
    label: str,
    warnings: list[str],
) -> tuple[bytes, int]:
    """Choose between a block and its backup copy.

    PLAN.md section 5: reject only when *both* copies of a pair are bad. One bad
    copy is a state the console repairs on next boot, so it is a warning and the
    good copy is used. This is deliberately more permissive than Dolphin, which
    fails a card when any two of the four dir/BAT blocks are bad; refusing an
    upload is how a hub loses a save, and every earlier version stays intact
    regardless. See docs/MEMCARD.md.
    """
    primary = _block(image, first_index)
    backup = _block(image, first_index + 1)
    primary_ok, backup_ok = check(primary), check(backup)

    if not primary_ok and not backup_ok:
        raise MemcardError(f"both copies of the {label} fail their checksum")

    if primary_ok and not backup_ok:
        warnings.append(f"backup copy of the {label} is damaged; using the primary")
        return primary, 0
    if backup_ok and not primary_ok:
        warnings.append(f"primary copy of the {label} is damaged; using the backup")
        return backup, 1

    # Both are intact: the live one is whichever has the higher update counter,
    # compared as a signed 16-bit value, ties going to the primary.
    primary_counter = struct.unpack_from(">h", primary, counter_offset)[0]
    backup_counter = struct.unpack_from(">h", backup, counter_offset)[0]
    if primary_counter >= backup_counter:
        return primary, 0
    return backup, 1


def _parse_directory(
    directory: bytes,
    bat: bytes,
    image: bytes,
    data_blocks: int,
    encoding: int,
    warnings: list[str],
) -> list[Save]:
    saves: list[Save] = []

    for index in range(DIR_ENTRIES):
        entry = directory[index * DENTRY_SIZE : (index + 1) * DENTRY_SIZE]
        game_code = entry[0x00:0x04]

        if game_code == UNUSED_GAMECODE or game_code == b"\x00\x00\x00\x00":
            continue

        first_block, block_count = struct.unpack_from(">HH", entry, 0x36)
        block_max = FST_BLOCKS + data_blocks

        if block_count == 0xFFFF or first_block < FST_BLOCKS or first_block >= block_max:
            warnings.append(
                f"directory entry {index} ({_ascii(game_code)}) points outside "
                f"the card and was skipped"
            )
            continue

        comments_address = struct.unpack_from(">I", entry, 0x3C)[0]
        title, subtitle = _read_comments(
            bat,
            image,
            first_block,
            block_count,
            comments_address,
            block_max,
            encoding,
            index,
            warnings,
        )

        saves.append(
            Save(
                index=index,
                game_code=_ascii(game_code),
                maker_code=_ascii(entry[0x04:0x06]),
                filename=_decode(entry[0x08:0x28], encoding),
                title=title,
                subtitle=subtitle,
                first_block=first_block,
                block_count=block_count,
                modified=_gc_time(struct.unpack_from(">I", entry, 0x28)[0]),
                permissions=entry[0x34],
                copy_counter=entry[0x35],
                banner_format=entry[0x07] & 0x03,
                icon_format=struct.unpack_from(">H", entry, 0x30)[0],
                comments_address=comments_address,
            )
        )

    return saves


def _read_comments(
    bat: bytes,
    image: bytes,
    first_block: int,
    block_count: int,
    address: int,
    block_max: int,
    encoding: int,
    index: int,
    warnings: list[str],
) -> tuple[str, str]:
    """Read the two 32-byte comment strings from a save's own data.

    `address` is an offset into the save's logical stream -- its chain of blocks
    concatenated -- not into the raw file, so the BAT chain has to be walked.
    """
    if address == 0xFFFFFFFF or address + DENTRY_STRLEN * 2 > block_count * BLOCK_SIZE:
        return "", ""

    chain = _walk_chain(bat, first_block, block_count, block_max)
    if chain is None:
        warnings.append(
            f"directory entry {index} has a broken block chain; "
            f"its name could not be read"
        )
        return "", ""

    block_index, offset_in_block = divmod(address, BLOCK_SIZE)
    if block_index >= len(chain):
        return "", ""

    # The pair may straddle a block boundary, so pull from the chained stream.
    wanted = DENTRY_STRLEN * 2
    out = bytearray()
    while wanted > 0 and block_index < len(chain):
        block = _block(image, chain[block_index])
        take = min(wanted, BLOCK_SIZE - offset_in_block)
        out += block[offset_in_block : offset_in_block + take]
        wanted -= take
        block_index += 1
        offset_in_block = 0

    if len(out) < DENTRY_STRLEN * 2:
        return "", ""

    return (
        _decode(out[:DENTRY_STRLEN], encoding),
        _decode(out[DENTRY_STRLEN:], encoding),
    )


def _walk_chain(
    bat: bytes, first_block: int, block_count: int, block_max: int
) -> list[int] | None:
    """Follow the BAT chain, returning absolute block numbers.

    None if the chain is broken, loops, or leaves the card.
    """
    chain: list[int] = []
    seen: set[int] = set()
    block = first_block

    for _ in range(block_count):
        if block in (BAT_END_OF_CHAIN, BAT_FREE):
            return None
        if block < FST_BLOCKS or block >= block_max or block in seen:
            return None
        seen.add(block)
        chain.append(block)

        map_index = block - FST_BLOCKS
        if map_index >= BAT_MAP_ENTRIES:
            return None
        block = struct.unpack_from(">H", bat, BAT_MAP_OFF + map_index * 2)[0]

    return chain


# --- small helpers --------------------------------------------------------


def _ascii(raw: bytes) -> str:
    return raw.decode("ascii", errors="replace").rstrip("\x00").strip()


def _decode(raw: bytes, encoding: int) -> str:
    """Decode a card string. Cards are CP1252 or Shift-JIS, never UTF-8."""
    text = bytes(raw).split(b"\x00", 1)[0]
    codec = ENCODING_NAMES.get(encoding, "cp1252")
    return text.decode(codec, errors="replace").strip()


def _gc_time(seconds: int) -> dt.datetime | None:
    """Directory timestamps count seconds from 2000-01-01, not the Unix epoch."""
    if seconds in (0, 0xFFFFFFFF):
        return None
    try:
        return GC_EPOCH + dt.timedelta(seconds=seconds)
    except OverflowError:
        return None


def _ostime(ticks: int) -> dt.datetime | None:
    """Header format time is an OSTime: ticks of a 40.5 MHz counter from 2000."""
    if ticks in (0, 0xFFFFFFFFFFFFFFFF):
        return None
    try:
        return GC_EPOCH + dt.timedelta(seconds=ticks / 40_500_000)
    except (OverflowError, OSError):
        return None
