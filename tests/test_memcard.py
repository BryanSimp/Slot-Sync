"""M2: the memory card parser -- PLAN.md section 5.

Done when a real Nintendont `.raw` lists the correct save names and block
counts, and a truncated or corrupted card is rejected with a useful error.

There is no real Nintendont dump in `tests/fixtures/` yet, so these run against
`scripts/make_fixture.py` output and are **approximate**: they prove the parser
agrees with an independent implementation of the same spec, not that both match
hardware. The generator deliberately does not import the parser's constants.
"""

from __future__ import annotations

import struct

import pytest

from make_fixture import SaveSpec, build_card
from slotsync.memcard import (
    BLOCK_SIZE,
    VALID_MBIT,
    MemcardError,
    bat_checksum_ok,
    checksums,
    directory_checksum_ok,
    header_checksum_ok,
    parse,
)

SAVES = [
    SaveSpec("GALE", "01", "zelda", "The Legend of Zelda", "Quest Log 3", blocks=3),
    SaveSpec("GM4E", "01", "sunshine", "Super Mario Sunshine", "Delfino Plaza", blocks=5),
    SaveSpec("GAFE", "01", "melee", "Super Smash Bros. Melee", "121 hours", blocks=11),
]


def card(**kwargs) -> bytes:
    kwargs.setdefault("mbit", 16)
    kwargs.setdefault("saves", SAVES)
    return bytes(build_card(**kwargs))


def corrupt(image: bytes, block: int, offset: int = 0x40) -> bytes:
    """Flip a byte inside `block`, breaking that block's checksum."""
    out = bytearray(image)
    index = block * BLOCK_SIZE + offset
    out[index] ^= 0xFF
    return bytes(out)


# --- checksums ------------------------------------------------------------


def test_checksum_of_a_hand_computed_case():
    """0x0001 + 0x0002 = 3; inverses 0xFFFE + 0xFFFD = 0x1FFFB, truncated."""
    assert checksums(b"\x00\x01\x00\x02") == (3, 0xFFFB)


def test_checksum_wraps_at_16_bits():
    assert checksums(b"\xff\xff\x00\x02") == (1, 0xFFFD)


def test_a_result_of_ffff_is_stored_as_zero():
    """The GC BIOS and Dolphin both normalise 0xFFFF to 0."""
    assert checksums(b"\xff\xff") == (0, 0)


def test_checksum_reads_big_endian_words():
    assert checksums(b"\x12\x34") != checksums(b"\x34\x12")
    assert checksums(b"\x12\x34") == (0x1234, 0xEDCB)


def test_odd_length_range_is_rejected():
    with pytest.raises(ValueError):
        checksums(b"\x00")


def test_generated_card_has_valid_checksums_in_every_block():
    image = card()
    assert header_checksum_ok(image[0:BLOCK_SIZE])
    assert directory_checksum_ok(image[BLOCK_SIZE : 2 * BLOCK_SIZE])
    assert directory_checksum_ok(image[2 * BLOCK_SIZE : 3 * BLOCK_SIZE])
    assert bat_checksum_ok(image[3 * BLOCK_SIZE : 4 * BLOCK_SIZE])
    assert bat_checksum_ok(image[4 * BLOCK_SIZE : 5 * BLOCK_SIZE])


def test_directory_and_bat_checksum_ranges_are_not_interchangeable():
    """The directory checksums sit at the end of its block and the BAT's at the
    start. Applying one rule to the other block is the classic mistake."""
    image = card()
    directory = image[BLOCK_SIZE : 2 * BLOCK_SIZE]
    bat = image[3 * BLOCK_SIZE : 4 * BLOCK_SIZE]
    assert not bat_checksum_ok(directory)
    assert not directory_checksum_ok(bat)


# --- geometry -------------------------------------------------------------


@pytest.mark.parametrize("mbit", VALID_MBIT)
def test_every_real_card_size_is_accepted(mbit):
    """Six sizes, not the four PLAN.md section 5 originally listed."""
    parsed = parse(card(mbit=mbit, saves=[SAVES[0]]))
    assert parsed.size_mbits == mbit
    assert parsed.total_blocks == mbit * 16
    assert parsed.data_blocks == mbit * 16 - 5
    assert parsed.size_bytes == mbit * 16 * BLOCK_SIZE


def test_one_mib_and_four_mib_cards_are_real():
    """These two are exactly the sizes the original four-size list dropped."""
    assert parse(card(mbit=8, saves=[])).data_blocks == 123
    assert parse(card(mbit=32, saves=[])).data_blocks == 507


@pytest.mark.parametrize(
    "size", [0, 1, 1024, 2097151, 2097153, 3 * 1024 * 1024, 2097152 + 1]
)
def test_wrong_sized_image_is_rejected(size):
    with pytest.raises(MemcardError, match="not a valid memory card size"):
        parse(b"\x00" * size)


def test_truncated_card_is_rejected_with_a_useful_error():
    truncated = card()[: 2097152 - 8192]
    with pytest.raises(MemcardError) as caught:
        parse(truncated)
    message = str(caught.value)
    assert "2088960 bytes" in message
    assert "2 MiB" in message  # tells the operator what would have been valid


# --- directory listing ----------------------------------------------------


def test_save_names_and_block_counts():
    """The M2 done condition: real save names and correct block counts."""
    parsed = parse(card())

    assert [s.display_name for s in parsed.saves] == [
        "The Legend of Zelda",
        "Super Mario Sunshine",
        "Super Smash Bros. Melee",
    ]
    assert [s.block_count for s in parsed.saves] == [3, 5, 11]
    assert [s.game_code + s.maker_code for s in parsed.saves] == [
        "GALE01",
        "GM4E01",
        "GAFE01",
    ]
    assert [s.subtitle for s in parsed.saves] == [
        "Quest Log 3",
        "Delfino Plaza",
        "121 hours",
    ]
    assert [s.filename for s in parsed.saves] == ["zelda", "sunshine", "melee"]


def test_save_sizes_follow_from_block_counts():
    parsed = parse(card())
    assert [s.size_bytes for s in parsed.saves] == [3 * 8192, 5 * 8192, 11 * 8192]


def test_free_and_used_block_accounting():
    parsed = parse(card())
    assert parsed.used_blocks == 3 + 5 + 11
    assert parsed.free_blocks == parsed.data_blocks - parsed.used_blocks


def test_empty_card_lists_no_saves():
    parsed = parse(card(saves=[]))
    assert parsed.saves == []
    assert parsed.free_blocks == parsed.data_blocks


def test_names_are_read_through_a_fragmented_block_chain():
    """Real cards fragment. A parser that assumes contiguous blocks passes
    every other test in this file."""
    contiguous = parse(card(fragment=False))
    fragmented = parse(card(fragment=True))

    assert [s.display_name for s in fragmented.saves] == [
        s.display_name for s in contiguous.saves
    ]
    # ...and the chains really were different.
    assert [s.first_block for s in fragmented.saves] != [
        s.first_block for s in contiguous.saves
    ]


def test_comments_are_found_at_a_late_offset_in_the_chain():
    """`comments_address` is an offset into the save's own stream, so a value
    past the first block has to be resolved through the BAT."""
    spec = SaveSpec(
        "GALE",
        "01",
        "late",
        "Found In Block Three",
        "ok",
        blocks=4,
        comments_address=2 * BLOCK_SIZE + 16,
    )
    parsed = parse(card(saves=[spec], fragment=True))
    assert parsed.saves[0].title == "Found In Block Three"


def test_modification_time_uses_the_2000_epoch():
    """Directory timestamps count from 2000-01-01, not the Unix epoch."""
    modified = parse(card()).saves[0].modified
    assert modified is not None
    assert modified.year == 2009  # 300_000_000 s after 2000-01-01


def test_shift_jis_cards_are_decoded_with_the_declared_codec():
    image = card(saves=[SAVES[0]], encoding=1)
    parsed = parse(image)
    assert parsed.is_shift_jis
    assert parsed.encoding_name == "shift_jis"


def test_header_fields_are_read():
    parsed = parse(card(device_id=1))
    assert parsed.device_id == 1
    assert parsed.serial == bytes(range(12))
    assert parsed.formatted_at is not None


# --- damage ---------------------------------------------------------------


def test_bad_header_checksum_is_rejected():
    with pytest.raises(MemcardError, match="header block checksum"):
        parse(corrupt(card(), block=0))


@pytest.mark.parametrize(
    ("damaged", "kept", "label"),
    [(1, 2, "directory"), (2, 1, "directory"), (3, 4, "bat"), (4, 3, "bat")],
)
def test_one_damaged_copy_is_a_warning_not_a_rejection(damaged, kept, label):
    """A single bad copy is a state the console repairs on next boot, so the
    card is stored and flagged rather than refused -- PLAN.md section 5."""
    parsed = parse(corrupt(card(), block=damaged))

    assert parsed.warnings, "damage should be reported"
    assert any("damaged" in w for w in parsed.warnings)
    # The good copy was used, so the saves still read.
    assert len(parsed.saves) == 3
    assert parsed.saves[0].display_name == "The Legend of Zelda"


def test_both_directory_copies_bad_is_a_rejection():
    image = corrupt(corrupt(card(), block=1), block=2)
    with pytest.raises(MemcardError, match="both copies of the directory"):
        parse(image)


def test_both_bat_copies_bad_is_a_rejection():
    image = corrupt(corrupt(card(), block=3), block=4)
    with pytest.raises(MemcardError, match="both copies of the block allocation"):
        parse(image)


def test_one_bad_directory_plus_one_bad_bat_is_still_accepted():
    """Dolphin fails this card -- it counts corruption across all four blocks.
    SlotSync accepts it, because a good copy of each still exists and refusing
    an upload is how a hub loses a save. See docs/MEMCARD.md."""
    image = corrupt(corrupt(card(), block=1), block=3)
    parsed = parse(image)
    assert len(parsed.warnings) == 2
    assert len(parsed.saves) == 3


def test_declared_size_disagreeing_with_the_file_is_a_warning():
    image = bytearray(card(mbit=16))
    struct.pack_into(">H", image, 0x0022, 64)  # claim 8 MiB in a 2 MiB file
    # Re-checksum the header so only the mismatch is under test.
    struct.pack_into(">HH", image, 0x01FC, *checksums(bytes(image[0:0x01FC])))

    parsed = parse(bytes(image))
    assert any("declares 64 Mbit" in w for w in parsed.warnings)


def test_directory_entry_pointing_off_the_card_is_skipped_with_a_warning():
    image = bytearray(card())
    # Point the first entry's first_block past the end of a 2 MiB card.
    struct.pack_into(">H", image, BLOCK_SIZE + 0x36, 9000)
    struct.pack_into(
        ">HH",
        image,
        BLOCK_SIZE + 0x1FFC,
        *checksums(bytes(image[BLOCK_SIZE : BLOCK_SIZE + 0x1FFC])),
    )
    # Damage the backup too, so the repaired copy is not silently preferred.
    struct.pack_into(">H", image, 2 * BLOCK_SIZE + 0x36, 9000)
    struct.pack_into(
        ">HH",
        image,
        2 * BLOCK_SIZE + 0x1FFC,
        *checksums(bytes(image[2 * BLOCK_SIZE : 2 * BLOCK_SIZE + 0x1FFC])),
    )

    parsed = parse(bytes(image))
    assert any("points outside the card" in w for w in parsed.warnings)
    assert len(parsed.saves) == 2


# --- copy selection -------------------------------------------------------


def test_the_higher_update_counter_wins():
    parsed = parse(card(dir_update_counter=1, bat_update_counter=1))
    assert parsed.active_directory == 0  # both copies identical, tie goes to 0

    # Make the backup newer by writing a distinct counter into block 2 only.
    image = bytearray(card(dir_update_counter=1))
    struct.pack_into(">h", image, 2 * BLOCK_SIZE + 0x1FFA, 9)
    struct.pack_into(
        ">HH",
        image,
        2 * BLOCK_SIZE + 0x1FFC,
        *checksums(bytes(image[2 * BLOCK_SIZE : 2 * BLOCK_SIZE + 0x1FFC])),
    )
    assert parse(bytes(image)).active_directory == 1


def test_update_counters_are_compared_as_signed_values():
    """The GC BIOS compares these signed, so a counter past 0x7FFF reads as
    negative and loses to a small positive one."""
    image = bytearray(card(dir_update_counter=1))
    struct.pack_into(">H", image, 2 * BLOCK_SIZE + 0x1FFA, 0xFFFF)  # -1 signed
    struct.pack_into(
        ">HH",
        image,
        2 * BLOCK_SIZE + 0x1FFC,
        *checksums(bytes(image[2 * BLOCK_SIZE : 2 * BLOCK_SIZE + 0x1FFC])),
    )
    # Unsigned this would be 65535 and would win; signed it is -1 and loses.
    assert parse(bytes(image)).active_directory == 0
