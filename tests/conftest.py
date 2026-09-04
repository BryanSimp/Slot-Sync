"""Shared fixtures.

Card images come from `scripts/make_fixture.py`. They are structurally valid
but synthetic, so anything asserting on real-world quirks should be treated as
approximate until a genuine Nintendont dump lands in `tests/fixtures/`.
"""

from __future__ import annotations

import pytest

from make_fixture import SaveSpec, build_card
from slotsync.config import Config

TEST_TOKEN = "test-token-not-a-real-secret"
TEST_PSK = b"test-psk-not-a-real-secret"

#: 4 Mbit / 512 KiB -- the smallest real card. Keeps the suite quick; the
#: parser does not care which of the six sizes it is handed.
SMALL_MBIT = 4


def make_card(*titles: str, mbit: int = SMALL_MBIT, **kwargs) -> bytes:
    """A valid card carrying one save per title."""
    saves = [
        SaveSpec(
            game_code="GALE",
            maker_code="01",
            filename=f"file{i}",
            title=title,
            subtitle=f"subtitle {i}",
            blocks=1,
        )
        for i, title in enumerate(titles)
    ]
    return bytes(build_card(mbit=mbit, saves=saves, **kwargs))


@pytest.fixture
def config(tmp_path) -> Config:
    """A Config pointed at a throwaway data directory."""
    return Config(
        data_dir=tmp_path / "data",
        token=TEST_TOKEN,
        psk=TEST_PSK,
    )


@pytest.fixture(scope="session")
def card_a() -> bytes:
    return make_card("Zelda Quest Log")


@pytest.fixture(scope="session")
def card_b() -> bytes:
    return make_card("Mario Sunshine Save")


@pytest.fixture(scope="session")
def card_c() -> bytes:
    return make_card("Melee Records")
