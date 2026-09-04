#!/usr/bin/env python3
"""Generate structurally valid GameCube memory card images for tests.

A real Nintendont dump is the fixture that matters; this stands in until one is
available, and tests built on it should be treated as approximate.

Deliberately standalone: the offsets and the checksum routine below are written
out again from `docs/MEMCARD.md` rather than imported from `slotsync.memcard`.
A generator that shares constants with the parser would agree with it even when
both are wrong, which would make the tests worthless.

    python scripts/make_fixture.py out.raw --mbit 16 \\
        --save GALE01:zelda:"The Legend of Zelda":"Quest Log 3":5 \\
        --fragment
"""

from __future__ import annotations

import argparse
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

BLOCK_SIZE = 0x2000
FST_BLOCKS = 5
DIR_ENTRIES = 127
DENTRY_SIZE = 0x40
STRLEN = 0x20
BAT_MAP_ENTRIES = 4091
MBIT_TO_BLOCKS = 16
VALID_MBIT = (4, 8, 16, 32, 64, 128)


def checksums(data: bytes) -> tuple[int, int]:
    """Additive and inverse checksums: big-endian u16 words, 0xFFFF stored as 0."""
    total = inverse = 0
    for (word,) in struct.iter_unpack(">H", data):
        total += word
        inverse += word ^ 0xFFFF
    total &= 0xFFFF
    inverse &= 0xFFFF
    return (0 if total == 0xFFFF else total, 0 if inverse == 0xFFFF else inverse)


@dataclass
class SaveSpec:
    game_code: str = "GALE"
    maker_code: str = "01"
    filename: str = "save"
    title: str = "Test Save"
    subtitle: str = "slot 1"
    blocks: int = 1
    comments_address: int = 0


def _pad(text: str, length: int) -> bytes:
    raw = text.encode("cp1252", errors="replace")[:length]
    return raw + b"\x00" * (length - len(raw))


def build_card(
    mbit: int = 16,
    saves: list[SaveSpec] | None = None,
    *,
    device_id: int = 0,
    encoding: int = 0,
    fragment: bool = False,
    dir_update_counter: int = 1,
    bat_update_counter: int = 1,
) -> bytearray:
    """Build a formatted card image with `saves` written into it."""
    if mbit not in VALID_MBIT:
        raise ValueError(f"{mbit} Mbit is not a real card size; pick one of {VALID_MBIT}")

    saves = saves or []
    total_blocks = mbit * MBIT_TO_BLOCKS
    data_blocks = total_blocks - FST_BLOCKS

    # A freshly formatted card is 0xFF everywhere it is not structured.
    image = bytearray(b"\xff" * (total_blocks * BLOCK_SIZE))

    # --- allocate data blocks to saves ---------------------------------
    needed = sum(s.blocks for s in saves)
    if needed > data_blocks:
        raise ValueError(f"saves need {needed} blocks, card has {data_blocks}")

    free_list = list(range(FST_BLOCKS, FST_BLOCKS + data_blocks))
    if fragment:
        # Interleave so no save gets a contiguous run. Real cards fragment, and
        # a parser that quietly assumes contiguity passes every other test.
        free_list = free_list[::2] + free_list[1::2]

    chains: list[list[int]] = []
    cursor = 0
    for spec in saves:
        chains.append(free_list[cursor : cursor + spec.blocks])
        cursor += spec.blocks

    # --- header block --------------------------------------------------
    header = bytearray(b"\xff" * BLOCK_SIZE)
    header[0x0000:0x000C] = bytes(range(12))  # serial
    struct.pack_into(">Q", header, 0x000C, 0x0000_0001_0000_0000)  # format time
    struct.pack_into(">I", header, 0x0014, 0)  # sram bias
    struct.pack_into(">I", header, 0x0018, 0)  # sram language
    struct.pack_into(">I", header, 0x001C, 0)  # dtv status
    struct.pack_into(">H", header, 0x0020, device_id)
    struct.pack_into(">H", header, 0x0022, mbit)
    struct.pack_into(">H", header, 0x0024, encoding)
    struct.pack_into(">H", header, 0x01FA, 0)  # update counter
    struct.pack_into(">HH", header, 0x01FC, *checksums(bytes(header[0x0000:0x01FC])))
    image[0:BLOCK_SIZE] = header

    # --- directory block -----------------------------------------------
    directory = bytearray(b"\xff" * BLOCK_SIZE)
    for i, (spec, chain) in enumerate(zip(saves, chains, strict=True)):
        off = i * DENTRY_SIZE
        directory[off + 0x00 : off + 0x04] = _pad(spec.game_code, 4)
        directory[off + 0x04 : off + 0x06] = _pad(spec.maker_code, 2)
        directory[off + 0x06] = 0xFF
        directory[off + 0x07] = 0x02  # banner: direct colour
        directory[off + 0x08 : off + 0x28] = _pad(spec.filename, STRLEN)
        struct.pack_into(">I", directory, off + 0x28, 300_000_000)  # modified
        struct.pack_into(">I", directory, off + 0x2C, 0)  # image offset
        struct.pack_into(">H", directory, off + 0x30, 0)  # icon format
        struct.pack_into(">H", directory, off + 0x32, 0)  # animation speed
        directory[off + 0x34] = 0x04  # public
        directory[off + 0x35] = 0  # copy counter
        struct.pack_into(">H", directory, off + 0x36, chain[0])
        struct.pack_into(">H", directory, off + 0x38, spec.blocks)
        struct.pack_into(">H", directory, off + 0x3A, 0xFFFF)
        struct.pack_into(">I", directory, off + 0x3C, spec.comments_address)

    directory[0x1FC0:0x1FFA] = b"\xff" * 0x3A
    struct.pack_into(">h", directory, 0x1FFA, dir_update_counter)
    struct.pack_into(">HH", directory, 0x1FFC, *checksums(bytes(directory[0:0x1FFC])))

    image[BLOCK_SIZE : 2 * BLOCK_SIZE] = directory
    image[2 * BLOCK_SIZE : 3 * BLOCK_SIZE] = directory  # backup copy

    # --- block allocation table ----------------------------------------
    bat = bytearray(BLOCK_SIZE)  # zeroed: 0x0000 means "free"
    for chain in chains:
        for current, following in zip(chain, chain[1:], strict=False):
            struct.pack_into(">H", bat, 0x000A + (current - FST_BLOCKS) * 2, following)
        struct.pack_into(">H", bat, 0x000A + (chain[-1] - FST_BLOCKS) * 2, 0xFFFF)

    used = sum(len(c) for c in chains)
    struct.pack_into(">h", bat, 0x0004, bat_update_counter)
    struct.pack_into(">H", bat, 0x0006, data_blocks - used)  # free blocks
    last_allocated = max((max(c) for c in chains), default=FST_BLOCKS - 1)
    struct.pack_into(">H", bat, 0x0008, last_allocated)
    struct.pack_into(">HH", bat, 0x0000, *checksums(bytes(bat[0x0004:BLOCK_SIZE])))

    image[3 * BLOCK_SIZE : 4 * BLOCK_SIZE] = bat
    image[4 * BLOCK_SIZE : 5 * BLOCK_SIZE] = bat  # backup copy

    # --- save data ------------------------------------------------------
    for spec, chain in zip(saves, chains, strict=True):
        payload = bytearray(b"\x00" * (spec.blocks * BLOCK_SIZE))
        addr = spec.comments_address
        payload[addr : addr + STRLEN] = _pad(spec.title, STRLEN)
        payload[addr + STRLEN : addr + STRLEN * 2] = _pad(spec.subtitle, STRLEN)

        # Scatter the payload across the chain, which may not be contiguous.
        for i, block in enumerate(chain):
            start = block * BLOCK_SIZE
            image[start : start + BLOCK_SIZE] = payload[
                i * BLOCK_SIZE : (i + 1) * BLOCK_SIZE
            ]

    return image


def _parse_save(text: str) -> SaveSpec:
    """GAMEID:filename:title:subtitle:blocks -- all but the first optional.

    Fields are colon-separated, so none of them can contain a colon. Call
    `build_card` directly if a fixture needs "Zelda: The Wind Waker" verbatim.
    """
    parts = text.split(":")
    game = parts[0]
    if len(game) != 6:
        raise argparse.ArgumentTypeError(
            f"game id must be 6 characters (4 game + 2 maker), got {game!r}"
        )
    spec = SaveSpec(game_code=game[:4], maker_code=game[4:])
    if len(parts) > 1 and parts[1]:
        spec.filename = parts[1]
    if len(parts) > 2 and parts[2]:
        spec.title = parts[2]
    if len(parts) > 3 and parts[3]:
        spec.subtitle = parts[3]
    if len(parts) > 4 and parts[4]:
        spec.blocks = int(parts[4])
    return spec


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("out", type=Path, help="path to write the .raw image to")
    parser.add_argument(
        "--mbit",
        type=int,
        default=16,
        choices=VALID_MBIT,
        help="card size in Mbit; 16 is Memory Card 251, the project default",
    )
    parser.add_argument(
        "--save",
        action="append",
        type=_parse_save,
        default=[],
        metavar="GAMEID:filename:title:subtitle:blocks",
        help="add a save; repeatable",
    )
    parser.add_argument("--slot", type=int, choices=(0, 1), default=0)
    parser.add_argument(
        "--shift-jis", action="store_true", help="mark the card as Shift-JIS"
    )
    parser.add_argument(
        "--fragment",
        action="store_true",
        help="scatter each save's blocks so the BAT chain is non-contiguous",
    )
    args = parser.parse_args(argv)

    image = build_card(
        mbit=args.mbit,
        saves=args.save,
        device_id=args.slot,
        encoding=1 if args.shift_jis else 0,
        fragment=args.fragment,
    )
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(image)

    print(
        f"wrote {args.out} -- {len(image)} bytes, {args.mbit} Mbit, "
        f"{len(args.save)} save(s)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
