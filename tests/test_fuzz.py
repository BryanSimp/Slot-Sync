"""M5: fuzzing -- PLAN.md section 10.

Done when the fuzz run is clean and no malformed datagram can crash the
listener.

Two contracts under test:

- `memcard.parse` either returns a Card or raises `MemcardError`. Anything else
  -- IndexError, struct.error, a runaway allocation -- is a bug, because the
  caller is the ingest path and the input is attacker-controlled.
- `SlotSyncProtocol.datagram_received` never raises, whatever it is handed.

The mutation strategies deliberately repair checksums after corrupting a field.
Random bit-flipping mostly bounces off the header checksum and never reaches
the directory walker or the BAT chain, which is where the interesting bugs
would be.

Set SLOTSYNC_FUZZ_ITERATIONS for a longer run than the suite's default.
"""

from __future__ import annotations

import asyncio
import os
import random
import struct

import pytest

from slotsync.memcard import (
    BLOCK_SIZE,
    MemcardError,
    checksums,
    parse,
)
from slotsync.protocol import (
    HEADER_SIZE,
    MAX_PAYLOAD,
    Error,
    Message,
    MsgType,
    ProtocolError,
    pack,
    unpack,
)
from slotsync.store import Store
from slotsync.udp import SlotSyncProtocol

from .conftest import TEST_PSK, make_card
from .test_udp import PEER, RecordingTransport

ITERATIONS = int(os.environ.get("SLOTSYNC_FUZZ_ITERATIONS", "400"))
SEEDS = [1, 2, 3, 5, 8, 13]

VALID = make_card("Fuzz Target")


# --- mutation strategies --------------------------------------------------


def _refix_header(image: bytearray) -> None:
    struct.pack_into(">HH", image, 0x01FC, *checksums(bytes(image[0:0x01FC])))


def _refix_directory(image: bytearray, block: int) -> None:
    base = block * BLOCK_SIZE
    struct.pack_into(
        ">HH",
        image,
        base + 0x1FFC,
        *checksums(bytes(image[base : base + 0x1FFC])),
    )


def _refix_bat(image: bytearray, block: int) -> None:
    base = block * BLOCK_SIZE
    struct.pack_into(
        ">HH",
        image,
        base,
        *checksums(bytes(image[base + 4 : base + BLOCK_SIZE])),
    )


def mutate_random_bytes(rng: random.Random) -> bytes:
    """Whole-cloth garbage at a plausible length."""
    size = rng.choice([0, 1, 2, 15, 512, 8192, 8191, 8193, 524288, 2097152])
    return bytes(rng.getrandbits(8) for _ in range(min(size, 65536))) + b"\x00" * max(
        0, size - 65536
    )


def mutate_bit_flips(rng: random.Random) -> bytes:
    """A valid card with a handful of bytes scrambled."""
    image = bytearray(VALID)
    for _ in range(rng.randint(1, 32)):
        image[rng.randrange(len(image))] = rng.getrandbits(8)
    return bytes(image)


def mutate_resized(rng: random.Random) -> bytes:
    """Valid content at an invalid length."""
    image = bytearray(VALID)
    delta = rng.choice([-8192, -1024, -1, 1, 1024, 8192])
    return bytes(image[: len(image) + delta] if delta < 0 else image + b"\x00" * delta)


def mutate_directory_entry(rng: random.Random) -> bytes:
    """Hostile values in a directory entry, with checksums repaired.

    This is the strategy that actually reaches the directory walker: block
    pointers off the end of the card, absurd block counts, comment offsets past
    the save.
    """
    image = bytearray(VALID)
    for block in (1, 2):
        base = block * BLOCK_SIZE
        entry = base + rng.randrange(4) * 0x40

        image[entry : entry + 4] = bytes(rng.getrandbits(8) for _ in range(4))
        struct.pack_into(">H", image, entry + 0x36, rng.getrandbits(16))  # first_block
        struct.pack_into(">H", image, entry + 0x38, rng.getrandbits(16))  # block_count
        struct.pack_into(">I", image, entry + 0x3C, rng.getrandbits(32))  # comments
        _refix_directory(image, block)

    _refix_header(image)
    return bytes(image)


def mutate_bat(rng: random.Random) -> bytes:
    """Scramble the block chain: loops, off-card pointers, early terminators."""
    image = bytearray(VALID)
    for block in (3, 4):
        base = block * BLOCK_SIZE
        for _ in range(rng.randint(1, 40)):
            index = rng.randrange(4091)
            struct.pack_into(">H", image, base + 0x000A + index * 2, rng.getrandbits(16))
        struct.pack_into(">H", image, base + 0x0006, rng.getrandbits(16))  # free
        _refix_bat(image, block)

    _refix_header(image)
    return bytes(image)


def mutate_header_fields(rng: random.Random) -> bytes:
    """Lie about the card's own geometry and encoding, checksum repaired."""
    image = bytearray(VALID)
    struct.pack_into(">H", image, 0x0020, rng.getrandbits(16))  # device id
    struct.pack_into(">H", image, 0x0022, rng.getrandbits(16))  # size in Mbit
    struct.pack_into(">H", image, 0x0024, rng.getrandbits(16))  # encoding
    struct.pack_into(">Q", image, 0x000C, rng.getrandbits(64))  # format time
    _refix_header(image)
    return bytes(image)


MEMCARD_STRATEGIES = [
    mutate_random_bytes,
    mutate_bit_flips,
    mutate_resized,
    mutate_directory_entry,
    mutate_bat,
    mutate_header_fields,
]


# --- the memcard parser ---------------------------------------------------


@pytest.mark.parametrize("seed", SEEDS)
def test_the_parser_only_ever_returns_a_card_or_raises_memcard_error(seed):
    rng = random.Random(seed)

    for iteration in range(ITERATIONS):
        strategy = rng.choice(MEMCARD_STRATEGIES)
        image = strategy(rng)

        try:
            card = parse(image)
        except MemcardError:
            continue  # the documented rejection path
        except Exception as exc:  # noqa: BLE001 -- that is the whole point
            raise AssertionError(
                f"seed={seed} iteration={iteration} strategy={strategy.__name__} "
                f"raised {type(exc).__name__}: {exc}"
            ) from exc

        # If it parsed, the result has to be coherent enough to render.
        assert card.size_bytes == len(image)
        assert card.data_blocks == card.total_blocks - 5
        for save in card.saves:
            assert isinstance(save.display_name, str)
            assert 0 <= save.first_block < card.total_blocks
            assert save.block_count >= 0


@pytest.mark.parametrize("seed", SEEDS[:3])
def test_a_hostile_directory_cannot_report_a_save_bigger_than_the_card(seed):
    """Found by this fuzzer: an entry claiming 60000 blocks on a 64-block card
    was reported as a 491 MiB save, and that number reached the web UI.

    Blocks are chained rather than contiguous, so `first_block + block_count`
    is not an extent and asserting on it would be wrong. What must hold is that
    a save occupies no more blocks than the card has, and starts inside it.
    """
    rng = random.Random(seed)

    for _ in range(ITERATIONS // 4):
        try:
            card = parse(mutate_directory_entry(rng))
        except MemcardError:
            continue
        for save in card.saves:
            assert 1 <= save.block_count <= card.data_blocks
            assert 5 <= save.first_block < card.total_blocks
            assert save.size_bytes <= card.size_bytes


def test_a_truncated_card_at_every_block_boundary_is_rejected_cleanly():
    for blocks in range(0, len(VALID) // BLOCK_SIZE):
        with pytest.raises(MemcardError):
            parse(VALID[: blocks * BLOCK_SIZE])


def test_an_empty_and_a_one_byte_image_are_rejected():
    for image in (b"", b"\x00"):
        with pytest.raises(MemcardError):
            parse(image)


# --- the datagram parser --------------------------------------------------


def mutate_datagram(rng: random.Random) -> bytes:
    choice = rng.randrange(5)

    if choice == 0:
        size = rng.randrange(0, HEADER_SIZE + MAX_PAYLOAD + 64)
        return bytes(rng.getrandbits(8) for _ in range(size))

    valid = pack(
        Message(
            msg_type=rng.choice(list(MsgType)),
            device_id=rng.getrandbits(64),
            game_id="GALE01",
            slot=rng.randrange(2),
            card_version=rng.getrandbits(32),
            parent_version=rng.getrandbits(32),
            offset=rng.getrandbits(32),
            total_size=rng.getrandbits(32),
            sequence=rng.getrandbits(32),
            payload=bytes(rng.getrandbits(8) for _ in range(rng.randrange(64))),
        ),
        TEST_PSK,
    )

    if choice == 1:
        return valid  # correctly signed, absurd field values
    if choice == 2:
        raw = bytearray(valid)
        raw[rng.randrange(len(raw))] = rng.getrandbits(8)
        return bytes(raw)
    if choice == 3:
        return valid[: rng.randrange(len(valid) + 1)]
    return valid + bytes(rng.getrandbits(8) for _ in range(rng.randrange(64)))


@pytest.mark.parametrize("seed", SEEDS)
def test_unpack_only_raises_protocol_error(seed):
    rng = random.Random(seed)

    for iteration in range(ITERATIONS):
        datagram = mutate_datagram(rng)
        try:
            unpack(datagram, TEST_PSK)
        except ProtocolError:
            continue
        except Exception as exc:  # noqa: BLE001
            raise AssertionError(
                f"seed={seed} iteration={iteration} raised {type(exc).__name__}: {exc}"
            ) from exc


@pytest.mark.parametrize("seed", SEEDS)
def test_no_malformed_datagram_can_crash_the_listener(seed, config):
    """The M5 done condition. Signed-but-absurd datagrams are included, so this
    reaches the handlers and not just the parser."""
    rng = random.Random(seed)
    server = SlotSyncProtocol(config, Store(config))
    server.transport = RecordingTransport()

    async def run() -> None:
        for iteration in range(ITERATIONS):
            datagram = mutate_datagram(rng)
            try:
                server.datagram_received(datagram, PEER)
            except Exception as exc:  # noqa: BLE001
                raise AssertionError(
                    f"seed={seed} iteration={iteration} raised "
                    f"{type(exc).__name__}: {exc}"
                ) from exc

            if server._tasks:
                results = await asyncio.gather(
                    *list(server._tasks), return_exceptions=True
                )
                for result in results:
                    assert not isinstance(result, BaseException), result

            server.transport.sent.clear()

    asyncio.run(run())


def test_a_flood_is_rate_limited_rather_than_processed(config):
    """Every datagram costs an HMAC, so the budget is spent before the check."""
    server = SlotSyncProtocol(config, Store(config))
    server.transport = RecordingTransport()

    junk = b"\x00" * 200
    flood = config.udp_burst * 3
    for _ in range(flood):
        server.datagram_received(junk, PEER)

    # Most of the flood was dropped before it cost an HMAC verification, and
    # the replies did not keep pace with it either.
    assert len(server.transport.sent) < flood // 2

    codes = {unpack(data, TEST_PSK).payload[0] for data, _ in server.transport.sent}
    assert Error.RATE_LIMITED in codes


def test_a_flood_cannot_flood_the_log(config, caplog):
    """Attacker-controlled input must not drive unbounded logging: that fills a
    disk the flood itself never could."""
    server = SlotSyncProtocol(config, Store(config))
    server.transport = RecordingTransport()

    with caplog.at_level("WARNING", logger="slotsync.udp"):
        for _ in range(2000):
            server.datagram_received(b"\x00" * 200, PEER)

    # One source, one second: at most a couple of lines.
    assert len(caplog.records) <= 2
