"""M0: configuration comes from the environment and fails loudly."""

from __future__ import annotations

import pytest

from slotsync.config import Config, ConfigError

GOOD = {
    "SLOTSYNC_TOKEN": "abc123",
    "SLOTSYNC_PSK": "def456",
}


def test_defaults_match_the_compose_file():
    cfg = Config.from_env(dict(GOOD))
    assert cfg.data_dir.as_posix() == "/data"
    assert cfg.http_port == 8080
    assert cfg.udp_port == 9977
    assert cfg.max_card_bytes == 16 * 1024 * 1024
    assert cfg.log_level == "INFO"


def test_env_overrides_defaults():
    cfg = Config.from_env(
        GOOD | {"SLOTSYNC_DATA": "/srv/x", "SLOTSYNC_HTTP_PORT": "9000"}
    )
    assert cfg.data_dir.as_posix() == "/srv/x"
    assert cfg.http_port == 9000


def test_derived_paths():
    cfg = Config.from_env(GOOD | {"SLOTSYNC_DATA": "/srv/x"})
    assert cfg.blobs_dir.as_posix() == "/srv/x/blobs"
    assert cfg.db_path.as_posix() == "/srv/x/slotsync.db"


def test_psk_is_bytes_for_hmac():
    assert Config.from_env(dict(GOOD)).psk == b"def456"


@pytest.mark.parametrize("missing", ["SLOTSYNC_TOKEN", "SLOTSYNC_PSK"])
def test_missing_secret_is_refused(missing):
    env = dict(GOOD)
    del env[missing]
    with pytest.raises(ConfigError, match=missing):
        Config.from_env(env)


@pytest.mark.parametrize("placeholder", ["changeme", "CHANGEME", "  changeme  ", ""])
def test_placeholder_secret_is_refused(placeholder):
    """.env.example ships `changeme`. Booting with it would be a LAN-open server."""
    with pytest.raises(ConfigError, match="SLOTSYNC_TOKEN"):
        Config.from_env(GOOD | {"SLOTSYNC_TOKEN": placeholder})


def test_non_integer_port_is_refused():
    with pytest.raises(ConfigError, match="SLOTSYNC_HTTP_PORT"):
        Config.from_env(GOOD | {"SLOTSYNC_HTTP_PORT": "eighty-eighty"})
