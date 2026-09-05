"""M5: rate limiting, replay rejection, and the caps -- PLAN.md section 10."""

from __future__ import annotations

import dataclasses

import pytest
from fastapi.testclient import TestClient

from slotsync.app import create_app
from slotsync.protocol import Message, MsgType, pack
from slotsync.ratelimit import RateLimiter, TokenBucket
from slotsync.replay import GUARDED, NonceCache
from slotsync.store import Store
from slotsync.udp import SlotSyncProtocol

from .conftest import TEST_PSK, TEST_TOKEN, make_card
from .test_udp import PEER, RecordingTransport, drain, push_messages, run

CARD = make_card("Hardening")


# --- token bucket ---------------------------------------------------------


def test_a_bucket_starts_full_and_drains():
    bucket = TokenBucket(capacity=3, rate=1, now=0.0)
    assert [bucket.take(now=0.0) for _ in range(4)] == [True, True, True, False]


def test_a_bucket_refills_at_its_rate():
    bucket = TokenBucket(capacity=3, rate=2, now=0.0)
    for _ in range(3):
        bucket.take(now=0.0)

    assert bucket.take(now=0.4) is False  # 0.8 tokens
    assert bucket.take(now=0.5) is True  # 1.0 token


def test_a_bucket_never_refills_past_capacity():
    bucket = TokenBucket(capacity=3, rate=100, now=0.0)
    bucket.take(now=0.0)
    assert [bucket.take(now=60.0) for _ in range(4)] == [True, True, True, False]


def test_limiter_keeps_keys_independent():
    limiter = RateLimiter(capacity=1, rate=0)
    assert limiter.allow("a", now=0.0) is True
    assert limiter.allow("b", now=0.0) is True
    assert limiter.allow("a", now=0.0) is False


def test_limiter_forgets_idle_keys_rather_than_growing_without_bound():
    """The key table is itself an attack surface if a source can be spoofed."""
    limiter = RateLimiter(capacity=1, rate=0, max_keys=4, idle_timeout=10.0)
    for key in range(4):
        limiter.allow(key, now=0.0)
    assert len(limiter) == 4

    limiter.allow("fresh", now=100.0)  # sweeps the four idle keys first
    assert len(limiter) == 1


def test_limiter_refuses_when_every_key_is_active():
    limiter = RateLimiter(capacity=1, rate=0, max_keys=2, idle_timeout=10.0)
    limiter.allow("a", now=0.0)
    limiter.allow("b", now=0.0)
    assert limiter.allow("c", now=0.0) is False


def test_reset_clears_a_key():
    limiter = RateLimiter(capacity=1, rate=0)
    limiter.allow("a", now=0.0)
    assert limiter.allow("a", now=0.0) is False
    limiter.reset("a")
    assert limiter.allow("a", now=0.0) is True


# --- replay ---------------------------------------------------------------


def test_a_repeated_control_nonce_is_a_replay():
    cache = NonceCache()
    nonce = b"\x01" * 16
    assert cache.check_and_record(1, nonce, MsgType.PUSH_END, now=0.0) is True
    assert cache.check_and_record(1, nonce, MsgType.PUSH_END, now=0.0) is False


def test_devices_do_not_share_a_nonce_space():
    cache = NonceCache()
    nonce = b"\x01" * 16
    assert cache.check_and_record(1, nonce, MsgType.HELLO, now=0.0) is True
    assert cache.check_and_record(2, nonce, MsgType.HELLO, now=0.0) is True


def test_a_nonce_is_forgotten_after_the_window():
    cache = NonceCache(ttl=100.0)
    nonce = b"\x01" * 16
    cache.check_and_record(1, nonce, MsgType.HELLO, now=0.0)
    assert cache.check_and_record(1, nonce, MsgType.HELLO, now=101.0) is True


def test_chunk_replay_is_deliberately_allowed():
    """PLAN.md section 4 makes duplicate chunks a supported operation, and a
    kernel client may resend a built datagram byte for byte. Rejecting repeats
    would break the retransmission path the protocol depends on."""
    cache = NonceCache()
    nonce = b"\x01" * 16
    for _ in range(5):
        assert cache.check_and_record(1, nonce, MsgType.PUSH_CHUNK, now=0.0) is True
    assert MsgType.PUSH_CHUNK not in GUARDED


def test_the_nonce_table_stays_bounded_per_device():
    cache = NonceCache(per_device=8)
    for i in range(100):
        cache.check_and_record(1, i.to_bytes(16, "big"), MsgType.HELLO, now=0.0)
    assert len(cache) <= 8


def test_a_replayed_datagram_is_dropped_by_the_listener(config):
    server = SlotSyncProtocol(config, Store(config))
    server.transport = RecordingTransport()

    hello = pack(Message(MsgType.HELLO, 7, "", 0, nonce=b"\x09" * 16), TEST_PSK)

    run(_deliver_once(server, hello))
    first = len(server.transport.sent)

    run(_deliver_once(server, hello))
    assert len(server.transport.sent) == first, "the replay drew a second reply"


async def _deliver_once(server, datagram):
    import asyncio

    server.datagram_received(datagram, PEER)
    if server._tasks:
        await asyncio.gather(*list(server._tasks), return_exceptions=True)


def test_a_fresh_nonce_on_a_retransmission_is_accepted(config):
    """The requirement this places on clients: a control message carries a new
    nonce every time it is sent, retransmissions included."""
    server = SlotSyncProtocol(config, Store(config))
    server.transport = RecordingTransport()

    for nonce in (b"\x01" * 16, b"\x02" * 16):
        run(
            _deliver_once(
                server, pack(Message(MsgType.HELLO, 7, "", 0, nonce=nonce), TEST_PSK)
            )
        )

    assert len(server.transport.sent) == 2


# --- caps -----------------------------------------------------------------


def test_a_push_larger_than_the_cap_never_allocates_a_buffer(config):
    server = SlotSyncProtocol(config, Store(config))
    server.transport = RecordingTransport()

    begin, _, _ = push_messages(CARD)
    run(server._dispatch(dataclasses.replace(begin, total_size=1 << 30), PEER))

    assert server.staging == {}
    assert drain(server)[0].payload[0] == 0x09  # TOO_LARGE


def test_staging_buffers_are_capped(config):
    small = dataclasses.replace(config, max_staging=3)
    server = SlotSyncProtocol(small, Store(small))
    server.transport = RecordingTransport()

    for tid in range(10):
        begin, _, _ = push_messages(CARD, tid=tid)
        run(server._dispatch(begin, PEER))

    assert len(server.staging) == 3


# --- http auth ------------------------------------------------------------


@pytest.fixture
def client(config) -> TestClient:
    tight = dataclasses.replace(config, auth_attempts=5, auth_refill=0.0)
    with TestClient(create_app(tight)) as test_client:
        yield test_client


def test_repeated_wrong_tokens_are_throttled(client):
    """A shared token on a LAN should not be brute-forceable."""
    bad = {"Authorization": "Bearer wrong"}
    codes = [client.get("/api/cards", headers=bad).status_code for _ in range(8)]

    assert codes[0] == 401
    assert 429 in codes
    assert codes[-1] == 429


def test_a_throttled_response_says_when_to_retry(client):
    bad = {"Authorization": "Bearer wrong"}
    for _ in range(8):
        response = client.get("/api/cards", headers=bad)
    assert response.status_code == 429
    assert response.headers["Retry-After"] == "30"
    assert response.json()["error"] == "too_many_requests"


def test_the_correct_token_works_even_while_throttled(client):
    """Locking out the right token punishes the household member who mistyped
    it and does nothing to an attacker, who does not have it either way."""
    bad = {"Authorization": "Bearer wrong"}
    good = {"Authorization": f"Bearer {TEST_TOKEN}"}

    for _ in range(20):
        client.get("/api/cards", headers=bad)
    assert client.get("/api/cards", headers=bad).status_code == 429

    assert client.get("/api/cards", headers=good).status_code == 200


def test_a_correct_token_clears_the_tally(client):
    bad = {"Authorization": "Bearer wrong"}
    good = {"Authorization": f"Bearer {TEST_TOKEN}"}

    for _ in range(3):
        client.get("/api/cards", headers=bad)
    assert client.get("/api/cards", headers=good).status_code == 200

    # The budget is fresh again, so a wrong token gets 401 rather than 429.
    assert client.get("/api/cards", headers=bad).status_code == 401


def test_successful_requests_are_never_throttled(client):
    good = {"Authorization": f"Bearer {TEST_TOKEN}"}
    codes = {client.get("/api/cards", headers=good).status_code for _ in range(50)}
    assert codes == {200}
