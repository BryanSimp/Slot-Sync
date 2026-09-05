"""Fixtures.

The integration tests run the *real* server in a thread rather than mocking it.
That is the point of a monorepo: the daemon and the hub can be tested against
each other, and a wire-level mismatch -- like the header-casing bug these tests
were written after -- shows up here instead of on somebody's PC.
"""

from __future__ import annotations

import socket
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

import pytest
import uvicorn
from make_fixture import SaveSpec, build_card
from slotsync.app import create_app
from slotsync.config import Config as ServerConfig

from slotsync_dolphin.config import Config

TOKEN = "test-token-not-a-real-secret"


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def make_card(*titles: str, mbit: int = 4) -> bytes:
    """A structurally valid card carrying one save per title."""
    saves = [
        SaveSpec(
            game_code="GALE",
            maker_code="01",
            filename=f"file{i}",
            title=title,
            subtitle=f"sub {i}",
            blocks=1,
        )
        for i, title in enumerate(titles)
    ]
    return bytes(build_card(mbit=mbit, saves=saves))


@pytest.fixture(scope="session")
def live_server(tmp_path_factory) -> str:
    """The real SlotSync server, on a random port, for the session."""
    data_dir = tmp_path_factory.mktemp("server-data")
    port = free_port()
    app = create_app(
        ServerConfig(data_dir=data_dir, token=TOKEN, psk=b"psk", enable_udp=False)
    )

    server = uvicorn.Server(
        uvicorn.Config(
            app,
            host="127.0.0.1",
            port=port,
            log_config=None,
            access_log=False,
            # SlotSync has no WebSocket endpoints. Loading the implementation
            # only drags in deprecation warnings which, under this suite's
            # warnings-as-errors, would kill the server thread.
            ws="none",
        )
    )

    # Capture whatever kills the thread. A fixture that just times out tells
    # you the server did not start; this tells you why.
    failure: list[BaseException] = []

    def run() -> None:
        try:
            server.run()
        except BaseException as exc:  # noqa: BLE001
            failure.append(exc)

    thread = threading.Thread(target=run, daemon=True)
    thread.start()

    base = f"http://127.0.0.1:{port}"
    deadline = time.time() + 20
    while time.time() < deadline:
        if failure:
            raise RuntimeError(f"the test server died: {failure[0]!r}") from failure[0]
        try:
            urllib.request.urlopen(f"{base}/healthz", timeout=1)
            break
        except (urllib.error.URLError, OSError):
            time.sleep(0.05)
    else:
        raise RuntimeError("the test server never came up")

    yield base

    server.should_exit = True
    thread.join(timeout=10)


@pytest.fixture
def dolphin_ini(tmp_path) -> Path:
    """A Dolphin.ini with the shape a real one has, including a slot that is
    switched off and no MemcardAPath at all."""
    path = tmp_path / "dolphin" / "Config" / "Dolphin.ini"
    path.parent.mkdir(parents=True)
    path.write_text(
        "[Analytics]\n"
        "Enabled = False\n"
        "\n"
        "[Core]\n"
        "CPUThread = True\n"
        "SlotA = 255\n"
        "SlotB = 255\n"
        "SkipIPL = True\n"
        "\n"
        "[Display]\n"
        "Fullscreen = False\n",
        encoding="utf-8",
    )
    return path


@pytest.fixture
def config(tmp_path, live_server, dolphin_ini) -> Config:
    return Config(
        server=live_server,
        token=TOKEN,
        cards_dir=tmp_path / "cards",
        user_dir=dolphin_ini.parent.parent,
        slot="A",
        mbit=4,
        settle_seconds=0.0,
    )


@pytest.fixture(autouse=True)
def _never_touch_the_real_dolphin(monkeypatch):
    """The daemon refuses to edit Dolphin.ini while Dolphin is running.

    Whoever runs these tests may well have Dolphin open, and the tests point at
    a throwaway config anyway, so the check is stubbed out rather than making
    the suite depend on what is running on the machine.
    """
    monkeypatch.setattr("slotsync_dolphin.sync.is_running", lambda: False)
