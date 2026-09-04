"""Shared fixtures."""

from __future__ import annotations

import pytest

from slotsync.config import Config

TEST_TOKEN = "test-token-not-a-real-secret"
TEST_PSK = b"test-psk-not-a-real-secret"


@pytest.fixture
def config(tmp_path) -> Config:
    """A Config pointed at a throwaway data directory."""
    return Config(
        data_dir=tmp_path / "data",
        token=TEST_TOKEN,
        psk=TEST_PSK,
    )
