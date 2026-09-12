"""M4: the UDP listener -- PLAN.md sections 3 and 4.

Two layers. Most tests drive `SlotSyncProtocol` directly through a recording
transport, which is deterministic and fast. The last few bind a real socket and
run `scripts/fake_console.py` against it, including with loss and reordering
injected -- that is the milestone's actual done condition.
"""

from __future__ import annotations

import asyncio
import dataclasses
import hashlib
import struct

import pytest

import fake_console
from slotsync.protocol import (
    FLAG_DELTA,
    MAX_PAYLOAD,
    Error,
    Message,
    MsgType,
    chunk_count,
    pack,
    unpack,
    unpack_bitmap,
)
from slotsync.store import Store
from slotsync.udp import SlotSyncProtocol, start_listener

from .conftest import TEST_PSK, make_card

CARD = make_card("Zelda Quest Log")
PEER = ("192.0.2.10", 40000)


class RecordingTransport(asyncio.DatagramTransport):
    """Collects what the server would have sent."""

    def __init__(self) -> None:
        self.sent: list[tuple[bytes, tuple]] = []
        self.closed = False

    def sendto(self, data, addr=None) -> None:
        self.sent.append((data, addr))

    def close(self) -> None:
        self.closed = True

    def get_extra_info(self, name, default=None):
        return {"sockname": ("0.0.0.0", 9977), "socket": None}.get(name, default)


@pytest.fixture
def server(config):
    """A listener with a recording transport, not bound to a real port."""
    protocol = SlotSyncProtocol(config, Store(config))
    protocol.transport = RecordingTransport()
    yield protocol


def send(server, message: Message, *, key: bytes = TEST_PSK, addr=PEER) -> None:
    server.datagram_received(pack(message, key), addr)


def drain(server) -> list[Message]:
    """Every reply so far, parsed."""
    out = [unpack(data, TEST_PSK) for data, _ in server.transport.sent]
    server.transport.sent.clear()
    return out


def run(coro):
    return asyncio.run(coro)


async def deliver(server, *datagrams: bytes) -> None:
    """Feed raw datagrams the way the loop would, then let the tasks finish.

    `datagram_received` spawns a task for everything except PUSH_CHUNK, so it
    needs a running loop. In production there always is one.
    """
    for datagram in datagrams:
        server.datagram_received(datagram, PEER)
    if server._tasks:
        await asyncio.gather(*list(server._tasks), return_exceptions=True)


def push_messages(image: bytes, *, game="GALE01", slot=0, parent=0, device=1, tid=1):
    """The datagrams a client sends for one push."""
    total = len(image)
    common = {
        "device_id": device,
        "game_id": game,
        "slot": slot,
        "card_version": tid,
    }
    begin = Message(
        msg_type=MsgType.PUSH_BEGIN,
        total_size=total,
        parent_version=parent,
        **common,
    )
    chunks = [
        Message(
            msg_type=MsgType.PUSH_CHUNK,
            offset=i * MAX_PAYLOAD,
            sequence=i,
            total_size=total,
            payload=image[i * MAX_PAYLOAD : (i + 1) * MAX_PAYLOAD],
            **common,
        )
        for i in range(chunk_count(total))
    ]
    end = Message(
        msg_type=MsgType.PUSH_END,
        total_size=total,
        payload=hashlib.sha256(image).digest(),
        **common,
    )
    return begin, chunks, end


async def do_push(server, image, **kwargs):
    """Run a whole push through the server, returning the final reply."""
    begin, chunks, end = push_messages(image, **kwargs)
    await server._dispatch(begin, PEER)
    for chunk in chunks:
        server.datagram_received(pack(chunk, TEST_PSK), PEER)
    drain(server)
    await server._dispatch(end, PEER)
    return drain(server)


# --- authentication -------------------------------------------------------


def test_a_bad_hmac_is_nacked_not_ignored(server):
    """Silence would leave a client built with the wrong PSK undiagnosable."""
    send(server, Message(MsgType.HELLO, 1, "GALE01", 0), key=b"wrong-key")

    replies = drain(server)
    assert len(replies) == 1
    assert replies[0].msg_type == MsgType.NACK
    assert replies[0].payload[0] == Error.BAD_HMAC


def test_the_reply_to_a_bad_datagram_is_no_larger_than_the_request(server):
    """No amplification for a spoofed source to exploit."""
    request = pack(Message(MsgType.PUSH_CHUNK, 1, "GALE01", 0, payload=b"x" * 512), b"k")
    server.datagram_received(request, PEER)

    reply, _ = server.transport.sent[0]
    assert len(reply) <= len(request)


def test_garbage_never_takes_the_listener_down(server):
    junk = [b"", b"x", b"\x00" * 95, b"NOPE" + b"\x00" * 200, bytes(range(256))]
    heartbeat = pack(Message(MsgType.HEARTBEAT, 1, "GALE01", 0), TEST_PSK)

    run(deliver(server, *junk, heartbeat))

    # Still serving.
    assert drain(server)[-1].msg_type == MsgType.ACK


def test_an_unknown_message_type_is_nacked(server):
    run(server._dispatch(Message(0x7F, 1, "GALE01", 0), PEER))
    assert drain(server)[0].payload[0] == Error.MALFORMED_HEADER


# --- hello and heartbeat --------------------------------------------------


def test_hello_returns_server_time_and_protocol_version(server):
    run(server._dispatch(Message(MsgType.HELLO, 42, "", 0), PEER))

    reply = drain(server)[0]
    assert reply.msg_type == MsgType.ACK
    server_time, version = struct.unpack(">QB", reply.payload[:9])
    assert version == 1
    assert server_time > 1_700_000_000


def test_hello_records_the_device(server):
    run(server._dispatch(Message(MsgType.HELLO, 42, "", 0), PEER))
    assert [d.device_id for d in server.store.list_devices()] == [42]


def test_heartbeat_is_acked_and_has_no_side_effects(server):
    """It exists to keep a NAT mapping open; sending the packet is the whole
    point, so it must not touch staging expiry."""
    run(server._dispatch(*_begin(CARD)))
    drain(server)
    before = {k: v.last_chunk_at for k, v in server.staging.items()}

    run(server._dispatch(Message(MsgType.HEARTBEAT, 1, "GALE01", 0), PEER))

    assert drain(server)[0].msg_type == MsgType.ACK
    assert {k: v.last_chunk_at for k, v in server.staging.items()} == before


def _begin(image, **kwargs):
    begin, _, _ = push_messages(image, **kwargs)
    return begin, PEER


# --- push -----------------------------------------------------------------


def test_a_complete_push_commits(server):
    replies = run(do_push(server, CARD))

    assert replies[-1].msg_type == MsgType.ACK
    assert replies[-1].card_version == 1
    assert server.store.read_image(server.store.head("GALE01", 0)) == CARD


def test_the_committed_image_is_byte_identical(server):
    run(do_push(server, CARD))
    assert server.store.read_image(server.store.head("GALE01", 0)) == CARD


def test_chunks_may_arrive_in_any_order(server):
    begin, chunks, end = push_messages(CARD)
    run(server._dispatch(begin, PEER))
    for chunk in reversed(chunks):
        server.datagram_received(pack(chunk, TEST_PSK), PEER)
    drain(server)
    run(server._dispatch(end, PEER))

    assert drain(server)[-1].msg_type == MsgType.ACK
    assert server.store.read_image(server.store.head("GALE01", 0)) == CARD


def test_duplicate_chunks_are_harmless(server):
    begin, chunks, end = push_messages(CARD)
    run(server._dispatch(begin, PEER))
    for chunk in chunks + chunks:
        server.datagram_received(pack(chunk, TEST_PSK), PEER)
    drain(server)
    run(server._dispatch(end, PEER))

    assert drain(server)[-1].msg_type == MsgType.ACK


def test_chunks_are_not_acked_individually(server):
    """Acking each chunk would double the traffic; gaps are reported at
    PUSH_END instead."""
    begin, chunks, _ = push_messages(CARD)
    run(server._dispatch(begin, PEER))
    drain(server)

    for chunk in chunks:
        server.datagram_received(pack(chunk, TEST_PSK), PEER)

    assert server.transport.sent == []


def test_gaps_come_back_as_a_nack_bitmap(server):
    begin, chunks, end = push_messages(CARD)
    dropped = {3, 17, 200}
    run(server._dispatch(begin, PEER))
    for index, chunk in enumerate(chunks):
        if index not in dropped:
            server.datagram_received(pack(chunk, TEST_PSK), PEER)
    drain(server)

    run(server._dispatch(end, PEER))

    replies = drain(server)
    assert all(r.msg_type == MsgType.NACK for r in replies)
    assert replies[0].payload[0] == Error.MISSING_CHUNKS

    missing = []
    for reply in replies:
        missing += unpack_bitmap(reply.payload[1:], reply.sequence)
    assert set(missing) == dropped


def test_nothing_is_committed_until_push_end(server):
    begin, chunks, _ = push_messages(CARD)
    run(server._dispatch(begin, PEER))
    for chunk in chunks:
        server.datagram_received(pack(chunk, TEST_PSK), PEER)

    assert server.store.head("GALE01", 0) is None


def test_a_wrong_digest_is_rejected_and_the_buffer_dropped(server):
    begin, chunks, end = push_messages(CARD)
    run(server._dispatch(begin, PEER))
    for chunk in chunks:
        server.datagram_received(pack(chunk, TEST_PSK), PEER)
    drain(server)

    liar = dataclasses.replace(end, payload=hashlib.sha256(b"something else").digest())
    run(server._dispatch(liar, PEER))

    assert drain(server)[0].payload[0] == Error.CHECKSUM_MISMATCH
    assert server.store.head("GALE01", 0) is None
    assert server.staging == {}


def test_a_stale_parent_is_a_conflict_carrying_the_head(server):
    run(do_push(server, CARD))
    other = make_card("Something Else")
    run(do_push(server, other, parent=1, tid=2))

    replies = run(do_push(server, make_card("Third"), parent=1, tid=3))

    assert replies[-1].msg_type == MsgType.NACK
    assert replies[-1].payload[0] == Error.CONFLICT
    # The head rides in card_version so the client can pull it and let a human
    # choose rather than guessing.
    assert replies[-1].card_version == 2


def test_an_identical_repush_is_a_free_no_op(server):
    run(do_push(server, CARD))
    replies = run(do_push(server, CARD, tid=2))

    assert replies[-1].msg_type == MsgType.ACK
    assert replies[-1].card_version == 1
    assert len(server.store.history("GALE01", 0)) == 1


def test_a_card_that_fails_validation_is_rejected(server):
    replies = run(do_push(server, b"\x00" * 4096))
    assert replies[-1].payload[0] == Error.FAILED_VALIDATION


def test_an_oversized_declaration_is_refused_up_front(server):
    begin, _, _ = push_messages(CARD)
    run(server._dispatch(dataclasses.replace(begin, total_size=64 * 1024 * 1024), PEER))
    assert drain(server)[0].payload[0] == Error.TOO_LARGE
    assert server.staging == {}


def test_a_chunk_without_a_staging_buffer_is_nacked(server):
    _, chunks, _ = push_messages(CARD)
    server.datagram_received(pack(chunks[0], TEST_PSK), PEER)
    assert drain(server)[0].payload[0] == Error.STAGING_EXPIRED


def test_push_end_without_a_staging_buffer_is_nacked(server):
    _, _, end = push_messages(CARD)
    run(server._dispatch(end, PEER))
    assert drain(server)[0].payload[0] == Error.STAGING_EXPIRED


# --- a resent PUSH_END ----------------------------------------------------
#
# PUSH_END's reply is the one datagram nobody retransmits on a timer, so losing
# it is the failure a UDP push is most exposed to. Observed on hardware
# 2026-09-12: a committed v38 came back as STAGING_EXPIRED, the console recorded
# the push as failed, kept its old parent, and had its next save refused as a
# conflict against the version it had itself just written.


def test_a_resent_push_end_is_acked_again_rather_than_expired(server):
    begin, chunks, end = push_messages(CARD)
    run(server._dispatch(begin, PEER))
    for chunk in chunks:
        server.datagram_received(pack(chunk, TEST_PSK), PEER)
    drain(server)

    run(server._dispatch(end, PEER))
    first = drain(server)[0]
    run(server._dispatch(end, PEER))
    again = drain(server)[0]

    assert first.msg_type == MsgType.ACK
    assert again.msg_type == MsgType.ACK
    assert again.card_version == first.card_version


def test_a_resent_push_end_does_not_commit_a_second_version(server):
    _, _, end = push_messages(CARD)
    run(do_push(server, CARD))

    run(server._dispatch(end, PEER))
    run(server._dispatch(end, PEER))

    assert len(server.store.history("GALE01", 0)) == 1


def test_a_resent_push_end_replays_a_checksum_failure(server):
    """The failures have to replay too.

    A client told CHECKSUM_MISMATCH resends the whole card; one told 0x0b
    instead concludes the push failed and waits for the next save. Replaying the
    real verdict is what keeps that fallback reachable.
    """
    begin, chunks, end = push_messages(CARD)
    run(server._dispatch(begin, PEER))
    for chunk in chunks:
        server.datagram_received(pack(chunk, TEST_PSK), PEER)
    drain(server)
    liar = dataclasses.replace(end, payload=hashlib.sha256(b"not this").digest())

    run(server._dispatch(liar, PEER))
    assert drain(server)[0].payload[0] == Error.CHECKSUM_MISMATCH
    run(server._dispatch(liar, PEER))
    assert drain(server)[0].payload[0] == Error.CHECKSUM_MISMATCH


def test_a_resent_push_end_replays_a_conflict_and_its_head(server):
    run(do_push(server, CARD))
    run(do_push(server, make_card("Something Else"), parent=1, tid=2))
    _, _, end = push_messages(make_card("Third"), parent=1, tid=3)

    run(do_push(server, make_card("Third"), parent=1, tid=3))
    run(server._dispatch(end, PEER))
    again = drain(server)[0]

    assert again.msg_type == MsgType.NACK
    assert again.payload[0] == Error.CONFLICT
    # The head still rides along, so a client that lost the first reply can
    # still tell a human which version it is up against.
    assert again.card_version == 2


def test_a_new_push_begin_forgets_the_previous_verdict(server):
    """Reusing a transfer id starts a new transfer, not a second look at the old
    one -- so the remembered reply must not answer for it."""
    run(do_push(server, CARD))
    assert server.settled

    run(server._dispatch(*_begin(CARD, tid=1)))
    assert server.settled == {}


def test_a_remembered_reply_expires_on_the_sweep(server):
    run(do_push(server, CARD))
    assert len(server.settled) == 1

    settled = next(iter(server.settled.values()))
    server.sweep(now=settled.at + 1)
    assert len(server.settled) == 1
    server.sweep(now=settled.at + 121)
    assert server.settled == {}


def test_concurrent_staging_buffers_are_capped(config):
    small = dataclasses.replace(config, max_staging=2)
    server = SlotSyncProtocol(small, Store(small))
    server.transport = RecordingTransport()

    for tid in range(3):
        begin, _, _ = push_messages(CARD, tid=tid)
        run(server._dispatch(begin, PEER))

    assert len(server.staging) == 2
    assert drain(server)[-1].payload[0] == Error.RATE_LIMITED


def test_restarting_a_transfer_discards_the_old_bytes(server):
    """A repeated PUSH_BEGIN means the client is retrying. Keeping half-filled
    bytes is how you get a card that passes its checksum and is still wrong."""
    begin, chunks, _ = push_messages(CARD)
    run(server._dispatch(begin, PEER))
    server.datagram_received(pack(chunks[0], TEST_PSK), PEER)
    run(server._dispatch(begin, PEER))

    staging = next(iter(server.staging.values()))
    assert staging.received_count == 0
    assert not any(staging.data)


# --- delta push -----------------------------------------------------------
#
# docs/PROTOCOL.md, "Delta push". PUSH_BEGIN with flags bit 0 seeds the staging
# buffer from the parent version instead of zeros, PUSH_DELTA declares which
# chunks are coming, and PUSH_END still carries the digest of the client's whole
# card -- which is the thing that keeps the seeding honest.


def differing_chunks(new: bytes, old: bytes) -> list[int]:
    return [
        i
        for i in range(chunk_count(len(new)))
        if new[i * MAX_PAYLOAD : (i + 1) * MAX_PAYLOAD]
        != old[i * MAX_PAYLOAD : (i + 1) * MAX_PAYLOAD]
    ]


def delta_messages(image, parent, indices, *, game="GALE01", slot=0, device=1, tid=2):
    """The datagrams a delta push sends: begin, one manifest, the chunks, end."""
    total = len(image)
    common = {
        "device_id": device,
        "game_id": game,
        "slot": slot,
        "card_version": tid,
    }
    begin = Message(
        msg_type=MsgType.PUSH_BEGIN,
        total_size=total,
        parent_version=parent,
        flags=FLAG_DELTA,
        **common,
    )
    bitmap = bytearray((max(indices) // 8) + 1) if indices else bytearray()
    for index in indices:
        bitmap[index >> 3] |= 1 << (index & 7)
    manifest = Message(
        msg_type=MsgType.PUSH_DELTA,
        sequence=0,
        total_size=total,
        payload=bytes(bitmap),
        **common,
    )
    chunks = [
        Message(
            msg_type=MsgType.PUSH_CHUNK,
            offset=i * MAX_PAYLOAD,
            sequence=i,
            total_size=total,
            payload=image[i * MAX_PAYLOAD : (i + 1) * MAX_PAYLOAD],
            **common,
        )
        for i in indices
    ]
    end = Message(
        msg_type=MsgType.PUSH_END,
        total_size=total,
        payload=hashlib.sha256(image).digest(),
        **common,
    )
    return begin, manifest, chunks, end


async def do_delta_push(server, image, parent, indices, **kwargs):
    begin, manifest, chunks, end = delta_messages(image, parent, indices, **kwargs)
    await server._dispatch(begin, PEER)
    await server._dispatch(manifest, PEER)
    for chunk in chunks:
        server.datagram_received(pack(chunk, TEST_PSK), PEER)
    drain(server)
    await server._dispatch(end, PEER)
    return drain(server)


def _seed_head(server, image=CARD):
    """Commit `image` so a delta has something to descend from."""
    run(do_push(server, image))
    drain(server)
    return server.store.head("GALE01", 0).version


def test_a_delta_push_commits_without_sending_the_whole_card(server):
    head = _seed_head(server)
    changed = make_card("Zelda Quest Log", "Mario Sunshine Save")
    indices = differing_chunks(changed, CARD)

    assert 0 < len(indices) < chunk_count(len(changed)), "the fixture is not a delta"

    replies = run(do_delta_push(server, changed, head, indices))

    assert [r.msg_type for r in replies] == [MsgType.ACK]
    assert server.store.read_image(server.store.head("GALE01", 0)) == changed


def test_a_delta_sends_far_fewer_chunks_than_the_card_has(server):
    """The whole point. If this stops holding, the feature has stopped working."""
    changed = make_card("Zelda Quest Log", "Mario Sunshine Save")
    indices = differing_chunks(changed, CARD)

    assert len(indices) * 4 < chunk_count(len(changed))


def test_the_seeded_bytes_are_the_parents(server):
    """Chunks the client never sent come back as the parent's, byte for byte."""
    head = _seed_head(server)
    changed = bytearray(CARD)
    changed[MAX_PAYLOAD * 3 : MAX_PAYLOAD * 3 + 4] = b"HACK"
    changed = bytes(changed)

    run(do_delta_push(server, changed, head, [3]))

    stored = server.store.read_image(server.store.head("GALE01", 0))
    assert stored == changed
    assert stored[:MAX_PAYLOAD] == CARD[:MAX_PAYLOAD]


def test_a_delta_whose_parent_does_not_match_fails_the_digest(server):
    """The safety property.

    If the server's parent bytes are not the ones the client thinks it is
    building on, the assembled image hashes wrong and is refused -- so the
    byte-level merge is verified rather than assumed. That is what lets
    CLAUDE.md's "never merge two cards" rule coexist with delta push.
    """
    head = _seed_head(server)
    # The client believes the parent is `other` and sends a delta against it.
    other = make_card("Something Else Entirely")
    client_image = bytearray(other)
    client_image[MAX_PAYLOAD * 3 : MAX_PAYLOAD * 3 + 4] = b"HACK"
    client_image = bytes(client_image)

    replies = run(do_delta_push(server, client_image, head, [3]))

    assert [r.payload[0] for r in replies] == [Error.CHECKSUM_MISMATCH]
    assert server.staging == {}
    assert server.store.read_image(server.store.head("GALE01", 0)) == CARD


def test_a_delta_against_an_unknown_parent_is_refused_with_0x0c(server):
    _seed_head(server)
    begin, _, _, _ = delta_messages(CARD, 99, [0])

    run(server._dispatch(begin, PEER))

    assert [r.payload[0] for r in drain(server)] == [Error.DELTA_UNAVAILABLE]
    assert server.staging == {}


def test_a_delta_against_v0_is_refused_with_0x0c(server):
    """v0 is 'the server has never seen this card'. There is nothing to seed."""
    begin, _, _, _ = delta_messages(CARD, 0, [0])

    run(server._dispatch(begin, PEER))

    assert [r.payload[0] for r in drain(server)] == [Error.DELTA_UNAVAILABLE]


def test_a_delta_against_a_pruned_blob_is_refused_with_0x0c(server):
    head = _seed_head(server)
    server.store.blobs.delete(server.store.head("GALE01", 0).sha256)
    begin, _, _, _ = delta_messages(CARD, head, [0])

    run(server._dispatch(begin, PEER))

    assert [r.payload[0] for r in drain(server)] == [Error.DELTA_UNAVAILABLE]


def test_a_delta_against_a_differently_sized_parent_is_refused_with_0x0c(server):
    """A card that changed size has to be pushed whole; a seed would misalign."""
    head = _seed_head(server)
    bigger = make_card("Zelda Quest Log", mbit=8)
    begin, _, _, _ = delta_messages(bigger, head, [0])

    run(server._dispatch(begin, PEER))

    assert [r.payload[0] for r in drain(server)] == [Error.DELTA_UNAVAILABLE]


def test_a_gap_nack_names_only_the_declared_chunks(server):
    """Everything else is the parent's bytes and was never in flight."""
    head = _seed_head(server)
    changed = make_card("Zelda Quest Log", "Mario Sunshine Save")
    indices = differing_chunks(changed, CARD)

    begin, manifest, chunks, end = delta_messages(changed, head, indices)
    run(server._dispatch(begin, PEER))
    run(server._dispatch(manifest, PEER))
    for chunk in chunks[1:]:  # drop the first one on the floor
        server.datagram_received(pack(chunk, TEST_PSK), PEER)
    drain(server)
    run(server._dispatch(end, PEER))

    replies = drain(server)
    assert [r.payload[0] for r in replies] == [Error.MISSING_CHUNKS]
    missing = unpack_bitmap(replies[0].payload[1:], replies[0].sequence)
    assert missing == [indices[0]]


def test_a_delta_recovers_from_a_lost_chunk(server):
    head = _seed_head(server)
    changed = make_card("Zelda Quest Log", "Mario Sunshine Save")
    indices = differing_chunks(changed, CARD)

    begin, manifest, chunks, end = delta_messages(changed, head, indices)
    run(server._dispatch(begin, PEER))
    run(server._dispatch(manifest, PEER))
    for chunk in chunks[1:]:
        server.datagram_received(pack(chunk, TEST_PSK), PEER)
    run(server._dispatch(end, PEER))
    drain(server)

    server.datagram_received(pack(chunks[0], TEST_PSK), PEER)
    run(server._dispatch(end, PEER))

    assert [r.msg_type for r in drain(server)] == [MsgType.ACK]
    assert server.store.read_image(server.store.head("GALE01", 0)) == changed


def test_declaring_a_chunk_twice_is_harmless(server):
    """Why PUSH_DELTA needs no replay protection: the bit is already set."""
    head = _seed_head(server)
    changed = make_card("Zelda Quest Log", "Mario Sunshine Save")
    indices = differing_chunks(changed, CARD)

    begin, manifest, chunks, end = delta_messages(changed, head, indices)
    run(server._dispatch(begin, PEER))
    run(server._dispatch(manifest, PEER))
    run(server._dispatch(manifest, PEER))
    assert next(iter(server.staging.values())).outstanding == len(indices)

    for chunk in chunks:
        server.datagram_received(pack(chunk, TEST_PSK), PEER)
    drain(server)
    run(server._dispatch(end, PEER))

    assert [r.msg_type for r in drain(server)] == [MsgType.ACK]


def test_a_declaration_arriving_after_its_chunk_still_completes(server):
    """UDP reorders. Neither order may leave the transfer stuck."""
    head = _seed_head(server)
    changed = make_card("Zelda Quest Log", "Mario Sunshine Save")
    indices = differing_chunks(changed, CARD)

    begin, manifest, chunks, end = delta_messages(changed, head, indices)
    run(server._dispatch(begin, PEER))
    for chunk in chunks:
        server.datagram_received(pack(chunk, TEST_PSK), PEER)
    run(server._dispatch(manifest, PEER))
    drain(server)
    run(server._dispatch(end, PEER))

    assert [r.msg_type for r in drain(server)] == [MsgType.ACK]
    assert server.store.read_image(server.store.head("GALE01", 0)) == changed


def test_push_delta_on_a_whole_card_transfer_is_refused(server):
    """Without the seed, everything outside the declaration would be zeros."""
    _seed_head(server)
    begin, _, _ = push_messages(CARD, tid=5)
    run(server._dispatch(begin, PEER))
    drain(server)

    run(
        server._dispatch(
            Message(MsgType.PUSH_DELTA, 1, "GALE01", 0, card_version=5, payload=b"\x01"),
            PEER,
        )
    )

    assert [r.payload[0] for r in drain(server)] == [Error.MALFORMED_HEADER]


def test_push_delta_without_a_staging_buffer_is_nacked(server):
    run(
        server._dispatch(
            Message(MsgType.PUSH_DELTA, 1, "GALE01", 0, card_version=7, payload=b"\x01"),
            PEER,
        )
    )

    assert [r.payload[0] for r in drain(server)] == [Error.STAGING_EXPIRED]


def test_declaring_a_chunk_past_the_end_is_refused(server):
    head = _seed_head(server)
    begin, _, _, _ = delta_messages(CARD, head, [0])
    run(server._dispatch(begin, PEER))
    drain(server)

    beyond = chunk_count(len(CARD))
    bitmap = bytearray((beyond // 8) + 1)
    bitmap[beyond >> 3] |= 1 << (beyond & 7)
    run(
        server._dispatch(
            Message(
                MsgType.PUSH_DELTA,
                1,
                "GALE01",
                0,
                card_version=2,
                total_size=len(CARD),
                payload=bytes(bitmap),
            ),
            PEER,
        )
    )

    assert [r.payload[0] for r in drain(server)] == [Error.MALFORMED_HEADER]


def test_restarting_a_delta_discards_the_declaration_too(server):
    """A repeat of PUSH_BEGIN is a retry, and half a declaration is worse than
    none: it would leave the server believing chunks were already accounted
    for."""
    head = _seed_head(server)
    begin, manifest, _, _ = delta_messages(CARD, head, [0, 1, 2])
    run(server._dispatch(begin, PEER))
    run(server._dispatch(manifest, PEER))
    assert next(iter(server.staging.values())).outstanding == 3

    run(server._dispatch(begin, PEER))

    staging = next(iter(server.staging.values()))
    assert staging.outstanding == 0
    assert not any(staging.declared)
    assert staging.received_count == 0
    assert bytes(staging.data) == CARD  # seeded afresh, not zeroed


def test_a_delta_against_a_stale_parent_is_still_a_conflict(server):
    """Seeding changes nothing about who decides a conflict -- PLAN.md 7."""
    head = _seed_head(server)
    run(do_push(server, make_card("Someone Else Was Here"), parent=head, tid=3))
    drain(server)

    changed = make_card("Zelda Quest Log", "Mario Sunshine Save")
    replies = run(do_delta_push(server, changed, head, differing_chunks(changed, CARD)))

    assert [r.payload[0] for r in replies] == [Error.CONFLICT]
    assert replies[0].card_version == head + 1


# --- expiry ---------------------------------------------------------------


def test_a_quiet_staging_buffer_expires(server):
    run(server._dispatch(*_begin(CARD)))
    assert len(server.staging) == 1

    staging = next(iter(server.staging.values()))
    assert server.sweep(now=staging.last_chunk_at + 1) == 0
    assert server.sweep(now=staging.last_chunk_at + 121) == 1
    assert server.staging == {}


def test_expiry_never_disturbs_the_current_head(server):
    """A console that loses power mid-transfer must not cost anyone a save."""
    run(do_push(server, CARD))
    run(server._dispatch(*_begin(make_card("Half Sent"), tid=9)))

    staging = next(iter(server.staging.values()))
    server.sweep(now=staging.last_chunk_at + 121)

    assert server.store.read_image(server.store.head("GALE01", 0)) == CARD


# --- pull -----------------------------------------------------------------


def test_pull_returns_the_head_in_order(server):
    run(do_push(server, CARD))
    drain(server)

    run(server._dispatch(Message(MsgType.PULL_REQ, 1, "GALE01", 0), PEER))

    replies = drain(server)
    ack, chunks = replies[0], replies[1:]
    assert ack.msg_type == MsgType.ACK
    assert ack.total_size == len(CARD)
    assert ack.card_version == 1

    assert [c.sequence for c in chunks] == list(range(chunk_count(len(CARD))))
    assert b"".join(c.payload for c in chunks) == CARD


def test_pull_can_name_an_older_version(server):
    run(do_push(server, CARD))
    other = make_card("Newer")
    run(do_push(server, other, parent=1, tid=2))
    drain(server)

    run(server._dispatch(Message(MsgType.PULL_REQ, 1, "GALE01", 0, card_version=1), PEER))

    replies = drain(server)
    assert replies[0].card_version == 1
    assert b"".join(c.payload for c in replies[1:]) == CARD


def test_a_pull_bitmap_asks_for_only_the_gaps(server):
    run(do_push(server, CARD))
    drain(server)

    wanted = [2, 5, 40]
    bitmap = bytearray(64)
    for index in wanted:
        bitmap[index >> 3] |= 1 << (index & 7)

    run(
        server._dispatch(
            Message(MsgType.PULL_REQ, 1, "GALE01", 0, sequence=0, payload=bytes(bitmap)),
            PEER,
        )
    )

    chunks = [r for r in drain(server) if r.msg_type == MsgType.PULL_CHUNK]
    assert [c.sequence for c in chunks] == wanted


def test_pulling_an_unknown_card_is_nacked(server):
    run(server._dispatch(Message(MsgType.PULL_REQ, 1, "ZZZZ99", 1), PEER))
    assert drain(server)[0].payload[0] == Error.UNKNOWN_CARD


def test_pulling_an_unknown_version_is_nacked(server):
    run(do_push(server, CARD))
    drain(server)
    run(
        server._dispatch(Message(MsgType.PULL_REQ, 1, "GALE01", 0, card_version=99), PEER)
    )
    assert drain(server)[0].payload[0] == Error.UNKNOWN_CARD


# --- both transports, one store -------------------------------------------


def test_a_udp_push_is_visible_over_http(config):
    """PLAN.md section 3: a card pushed from a Wii and one pushed from Dolphin
    are indistinguishable once committed."""
    from fastapi.testclient import TestClient

    from slotsync.app import create_app

    from .conftest import TEST_TOKEN

    app = create_app(config)
    server = SlotSyncProtocol(config, app.state.store)
    server.transport = RecordingTransport()

    run(do_push(server, CARD))

    with TestClient(app) as client:
        response = client.get(
            "/api/cards/GALE01/A/latest.raw",
            headers={"Authorization": f"Bearer {TEST_TOKEN}"},
        )
    assert response.content == CARD


# --- against a real socket ------------------------------------------------


def _run_real(config, work):
    """Bind the listener and run a blocking client against it in a thread."""

    async def main():
        store = Store(config)
        transport, protocol = await start_listener(config, store)
        port = transport.get_extra_info("sockname")[1]
        try:
            return await asyncio.get_running_loop().run_in_executor(
                None, work, port
            ), store
        finally:
            transport.close()

    return asyncio.run(main())


@pytest.fixture
def udp_config(config):
    # Port 0 lets the OS pick, so the suite never fights a real server.
    return dataclasses.replace(config, udp_port=0, udp_host="127.0.0.1")


def test_fake_console_round_trips_a_card(udp_config, tmp_path):
    """The M4 done condition, first half: a real push and pull over sockets."""
    source = tmp_path / "card.raw"
    source.write_bytes(CARD)
    out = tmp_path / "out.raw"

    def work(port):
        link = fake_console.Link("127.0.0.1", port, TEST_PSK, timeout=1.0)
        try:
            fake_console.do_push(link, 1, "GALE01", 0, source, 0, 1, 8, 0.0)
            fake_console.do_pull(link, 1, "GALE01", 0, out, 0, 8)
        finally:
            link.close()

    _run_real(udp_config, work)
    assert out.read_bytes() == CARD


def test_fake_console_survives_loss_and_reordering(udp_config, tmp_path):
    """The M4 done condition, second half. Seeded so a failure reproduces."""
    source = tmp_path / "card.raw"
    source.write_bytes(CARD)
    out = tmp_path / "out.raw"

    def work(port):
        # Each retransmission round ends by waiting out a recv timeout, so
        # this number dominates the test's runtime. Loopback RTT is
        # microseconds; half a second is already enormous.
        pusher = fake_console.Link(
            "127.0.0.1", port, TEST_PSK, loss=0.2, reorder=True, seed=7, timeout=0.5
        )
        puller = fake_console.Link(
            "127.0.0.1", port, TEST_PSK, loss=0.2, seed=11, timeout=0.5
        )
        try:
            fake_console.do_push(pusher, 1, "GALE01", 0, source, 0, 1, 20, 0.0)
            assert pusher.dropped_out > 0, "the test injected no loss"
            fake_console.do_pull(puller, 1, "GALE01", 0, out, 0, 20)
        finally:
            pusher.close()
            puller.close()

    _run_real(udp_config, work)
    assert out.read_bytes() == CARD


def test_a_datagram_error_does_not_stop_the_listener(server, caplog):
    """One peer going away -- a console powered off mid-pull -- must not take
    the listener down for every other console. Without an error_received the
    transport can be torn down, leaving the process answering HTTP while UDP is
    silently dead."""
    with caplog.at_level("WARNING", logger="slotsync.udp"):
        server.error_received(ConnectionResetError("port unreachable"))

    assert any("continuing" in r.message for r in caplog.records)

    # Still serving.
    run(deliver(server, pack(Message(MsgType.HEARTBEAT, 1, "GALE01", 0), TEST_PSK)))
    assert drain(server)[-1].msg_type == MsgType.ACK


def test_losing_the_transport_is_logged_loudly(server, caplog):
    with caplog.at_level("ERROR", logger="slotsync.udp"):
        server.connection_lost(OSError("gone"))
    assert any("lost its transport" in r.message for r in caplog.records)


def test_fake_console_delta_pushes_only_what_changed(udp_config, tmp_path):
    """The third implementation of the wire format, over real sockets.

    CLAUDE.md keeps `fake_console.py` independent of the server's own protocol
    module precisely so a test like this can catch a delta encoding that only
    the server and its own parser agree on.
    """
    first = tmp_path / "v1.raw"
    first.write_bytes(CARD)
    changed = make_card("Zelda Quest Log", "Mario Sunshine Save")
    second = tmp_path / "v2.raw"
    second.write_bytes(changed)
    out = tmp_path / "out.raw"

    sent: list[int] = []

    def work(port):
        link = fake_console.Link("127.0.0.1", port, TEST_PSK, timeout=1.0)
        try:
            fake_console.do_push(link, 1, "GALE01", 0, first, 0, 1, 8, 0.0)
            before = link.sent
            fake_console.do_push(
                link, 1, "GALE01", 0, second, 1, 2, 8, 0.0, delta_from=first
            )
            sent.append(link.sent - before)
            fake_console.do_pull(link, 1, "GALE01", 0, out, 0, 8)
        finally:
            link.close()

    _, store = _run_real(udp_config, work)

    assert out.read_bytes() == changed
    assert store.head("GALE01", 0).version == 2
    # begin + manifest + end is three datagrams of overhead, and the whole card
    # is 512 chunks. Anything near that means the delta did not happen.
    assert sent[0] < chunk_count(len(changed)) // 4


def test_fake_console_falls_back_when_the_server_cannot_seed(udp_config, tmp_path):
    """A pruned parent blob must not make a card unpushable."""
    first = tmp_path / "v1.raw"
    first.write_bytes(CARD)
    changed = make_card("Zelda Quest Log", "Mario Sunshine Save")
    second = tmp_path / "v2.raw"
    second.write_bytes(changed)

    def work(port):
        link = fake_console.Link("127.0.0.1", port, TEST_PSK, timeout=1.0)
        try:
            fake_console.do_push(link, 1, "GALE01", 0, first, 0, 1, 8, 0.0)
            # Prune the parent out from under the delta.
            store = Store(udp_config)
            store.blobs.delete(store.head("GALE01", 0).sha256)
            fake_console.do_push(
                link, 1, "GALE01", 0, second, 1, 2, 8, 0.0, delta_from=first
            )
        finally:
            link.close()

    _, store = _run_real(udp_config, work)

    assert store.head("GALE01", 0).version == 2
    assert store.read_image(store.head("GALE01", 0)) == changed
