"""Binary UDP listener -- PLAN.md sections 3 and 4.

The console ingest path. Runs on the same event loop as the HTTP app and writes
through the same `store.Store`, so a card pushed from a Wii and one pushed from
Dolphin are indistinguishable once committed.

Shape of the thing:

- Every datagram is authenticated before any header field is trusted, and is
  independently processable. The client keeps no connection state because the
  eventual kernel client cannot.
- A push accumulates into a staging buffer. Chunks may arrive out of order and
  may be duplicated; duplicates overwrite the same bytes harmlessly. Nothing is
  committed until PUSH_END arrives *and* the SHA-256 matches, so a client that
  loses power mid-transfer never disturbs the current head.
- A *delta* push seeds that buffer from the parent version rather than from
  zeros, so the client only sends the chunks that changed. The PUSH_END digest
  still covers the client's whole card, which is what keeps the seeding honest:
  bytes the server supplied that the client did not expect fail the digest and
  are refused, exactly as a torn whole-card push is.
- The server never sends unsolicited. Every datagram out is a reply.
"""

from __future__ import annotations

import asyncio
import contextlib
import hashlib
import logging
import socket
import struct
import sys
import time
from dataclasses import dataclass, field

from . import __version__
from .config import Config
from .protocol import (
    FLAG_DELTA,
    MAX_PAYLOAD,
    Error,
    Message,
    MsgType,
    ProtocolError,
    bitmap_windows,
    chunk_count,
    pack,
    reply_to,
    unpack,
    unpack_bitmap,
)
from .ratelimit import RateLimiter
from .replay import NonceCache
from .store import (
    ConflictError,
    NotFoundError,
    Store,
    TooLargeError,
    ValidationError,
)

log = logging.getLogger("slotsync.udp")


@dataclass
class Staging:
    """A push in progress.

    Keyed by (device_id, game_id, slot, card_version) -- PLAN.md section 4.
    `card_version` is the client's own identifier for the transfer, echoed in
    every message of it; the version the server actually assigns comes back in
    the PUSH_END ack.
    """

    total_size: int
    parent_version: int
    device_id: int
    game_id: str
    slot: int
    addr: tuple
    data: bytearray
    expected: int
    #: One bit per chunk index, LSB-first, so a duplicate is free to detect.
    received: bytearray
    #: True when `data` was seeded from the parent version rather than zeroed,
    #: so only the chunks named in PUSH_DELTA are ever in flight.
    is_delta: bool = False
    #: On a delta, the chunks the client said it would send. One bit each, same
    #: shape as `received`. Unused on a whole-card push, where every chunk is
    #: expected.
    declared: bytearray = field(default_factory=bytearray)
    received_count: int = 0
    #: Declared chunks that have not arrived. Maintained incrementally rather
    #: than recomputed, because a declaration and the chunk it names may arrive
    #: in either order and each has to be able to settle the other.
    outstanding: int = 0
    last_chunk_at: float = field(default_factory=time.monotonic)

    @property
    def complete(self) -> bool:
        if self.is_delta:
            return self.outstanding == 0
        return self.received_count >= self.expected

    def declare(self, sequence: int) -> bool:
        """Record that the client intends to send chunk `sequence`.

        Idempotent: declaring twice sets a bit that is already set, which is why
        PUSH_DELTA needs no replay protection.
        """
        if sequence >= self.expected:
            return False
        byte, bit = sequence >> 3, 1 << (sequence & 7)
        if self.declared[byte] & bit:
            return True
        self.declared[byte] |= bit
        if not self.received[byte] & bit:
            self.outstanding += 1
        return True

    def store_chunk(self, sequence: int, offset: int, payload: bytes) -> bool:
        """Record one chunk. False if it does not fit the declared transfer."""
        if sequence >= self.expected:
            return False
        if offset + len(payload) > self.total_size:
            return False

        self.data[offset : offset + len(payload)] = payload
        byte, bit = sequence >> 3, 1 << (sequence & 7)
        if not self.received[byte] & bit:
            self.received[byte] |= bit
            self.received_count += 1
            if self.is_delta and self.declared[byte] & bit:
                self.outstanding -= 1
        self.last_chunk_at = time.monotonic()
        return True

    def missing(self) -> list[int]:
        """Chunks still owed.

        On a delta that is the declared-but-unreceived set: everything else is
        the parent's bytes and was never in flight, so it cannot be missing.
        """
        if self.is_delta:
            return [
                index
                for index in range(self.expected)
                if self.declared[index >> 3] & (1 << (index & 7))
                and not self.received[index >> 3] & (1 << (index & 7))
            ]
        return [
            index
            for index in range(self.expected)
            if not self.received[index >> 3] & (1 << (index & 7))
        ]


@dataclass
class Settled:
    """The reply a finished transfer earned, kept so it can be given again.

    PUSH_END is the last datagram of a push, which makes its reply the one
    datagram nobody retransmits on a timer -- if it is lost, the client asks
    again and the staging buffer it is asking about is already gone. Deleting
    that buffer promptly is right: it holds a whole card. Forgetting what it
    *decided* is not.

    Without this the server answers STAGING_EXPIRED to the second PUSH_END, and
    a push that committed reads to the client as a push that failed -- observed
    on hardware 2026-09-12, where a committed v38 came back as `nack 0x0b`, the
    console kept its old parent, and its next save was refused as a conflict
    against a version it had itself just written.

    So the outcome outlives the buffer: ACK with the committed version, or the
    same NACK, replayed verbatim for as long as the client could still be
    asking. Only the reply is kept -- tens of bytes against a staging buffer's
    megabytes -- so this is cheap to hold for every push in flight.
    """

    msg_type: int
    card_version: int
    payload: bytes
    at: float = field(default_factory=time.monotonic)


class SlotSyncProtocol(asyncio.DatagramProtocol):
    """Server side of the binary protocol."""

    def __init__(self, config: Config, store: Store) -> None:
        self.config = config
        self.store = store
        self.key = config.psk
        self.transport: asyncio.DatagramTransport | None = None
        self.staging: dict[tuple, Staging] = {}
        #: Outcomes of transfers that have finished, so a resent PUSH_END is
        #: answered the way the first one was. Swept on the same timer as
        #: `staging`; see `Settled`.
        self.settled: dict[tuple, Settled] = {}
        self._tasks: set[asyncio.Task] = set()
        self._sweeper: asyncio.Task | None = None

        # Checked before the HMAC, since verifying is the expensive part and an
        # unauthenticated flood should not get to spend it.
        self.limiter = RateLimiter(config.udp_burst, config.udp_rate)
        #: NACKing every dropped datagram would turn a flood into two floods,
        #: and logging every one would turn it into a log flood. Separate
        #: budgets, so neither starves the other.
        self._limit_notified: dict[object, float] = {}
        self._log_throttle: dict[object, float] = {}
        self.nonces = NonceCache(ttl=config.nonce_ttl)

    # --- lifecycle --------------------------------------------------------

    def connection_made(self, transport) -> None:
        self.transport = transport
        self._sweeper = asyncio.create_task(self._sweep_forever())

    def error_received(self, exc: Exception) -> None:
        """A datagram-level error, most often an ICMP port-unreachable.

        Implementing this is not optional in practice. The server streams a
        whole card as thousands of datagrams; if the client stops listening
        part-way -- a console powered off mid-pull -- the OS reports the
        unreachable port back on this socket. Without a handler asyncio can
        treat that as fatal and tear the transport down, which leaves the
        process alive and answering HTTP while the UDP listener is silently
        dead. Logging and carrying on is the correct response: one peer going
        away is not a reason to stop serving every other console.
        """
        log.warning("datagram error, continuing", extra={"detail": str(exc)})

    def connection_lost(self, exc) -> None:
        if exc is not None:
            log.error("udp listener lost its transport", extra={"detail": str(exc)})
        if self._sweeper is not None:
            self._sweeper.cancel()
        for task in list(self._tasks):
            task.cancel()

    def close(self) -> None:
        if self.transport is not None:
            self.transport.close()

    # --- receive ----------------------------------------------------------

    def datagram_received(self, data: bytes, addr) -> None:
        source = addr[0] if isinstance(addr, tuple) else addr
        if not self.limiter.allow(source):
            self._on_rate_limited(data, addr, source)
            return

        try:
            message = unpack(data, self.key)
        except ProtocolError as exc:
            self._reject(data, addr, exc)
            return
        except Exception:
            # A malformed datagram must never take the listener down.
            log.exception("undecodable datagram", extra={"peer": _peer(addr)})
            return

        if not self.nonces.check_and_record(
            message.device_id, message.nonce, message.msg_type
        ):
            # A correct client never reuses a nonce, retransmissions included --
            # see replay.py. Dropping silently rather than replying keeps a
            # replay flood from becoming an outbound one.
            log.warning(
                "dropped a replayed message",
                extra={"peer": _peer(addr), "msg_type": int(message.msg_type)},
            )
            return

        # PUSH_CHUNK is the hot path and is pure memory work, so it is handled
        # inline rather than paying for a task per chunk.
        if message.msg_type == MsgType.PUSH_CHUNK:
            self._on_push_chunk(message, addr)
            return

        task = asyncio.create_task(self._dispatch(message, addr))
        self._tasks.add(task)
        task.add_done_callback(self._tasks.discard)

    @staticmethod
    def _throttle(seen: dict, addr, interval: float = 1.0) -> bool:
        """True at most once per `interval` per source.

        Each caller passes its own dict. They must not share one: a burst of
        rejected datagrams would otherwise consume the budget and suppress the
        rate-limit NACK, leaving a throttled client with no idea why it is
        being ignored.
        """
        source = addr[0] if isinstance(addr, tuple) else addr
        now = time.monotonic()

        if now - seen.get(source, 0.0) < interval:
            return False

        seen[source] = now
        if len(seen) > 1024:
            for key in [k for k, at in seen.items() if now - at > 60.0]:
                del seen[key]
        return True

    def _should_log(self, addr) -> bool:
        return self._throttle(self._log_throttle, addr)

    def _on_rate_limited(self, data: bytes, addr, source) -> None:
        """Drop a datagram from a source that is over budget.

        A NACK goes back at most once a second per source: enough for a real
        client to learn why it is being ignored, not enough to turn an inbound
        flood into an outbound one.
        """
        if not self._throttle(self._limit_notified, addr):
            return

        log.warning("rate limited", extra={"peer": _peer(addr)})
        self._send(
            Message(
                msg_type=MsgType.NACK,
                device_id=0,
                game_id="",
                slot=0,
                payload=bytes([Error.RATE_LIMITED]),
            ),
            addr,
        )

    def _reject(self, data: bytes, addr, exc: ProtocolError) -> None:
        """NACK an unusable datagram.

        A bad-HMAC datagram cannot be trusted, so the reply echoes nothing from
        it beyond the sequence number, and the reply is no larger than the
        request -- there is no amplification here for a spoofed source to use.
        Answering at all is worth it: a client built with the wrong PSK is
        otherwise met with silence.
        """
        # Log at most once a second per source. The input here is entirely
        # attacker-controlled, so an unthrottled warning turns a datagram flood
        # into a log flood -- which fills a disk the flood itself never could.
        if self._should_log(addr):
            log.warning(
                "rejected datagram",
                extra={"peer": _peer(addr), "code": exc.code.name, "detail": str(exc)},
            )
        else:
            log.debug(
                "rejected datagram",
                extra={"peer": _peer(addr), "code": exc.code.name},
            )
        sequence = 0
        if len(data) >= 48:
            with contextlib.suppress(struct.error):
                sequence = struct.unpack_from(">I", data, 44)[0]

        self._send(
            Message(
                msg_type=MsgType.NACK,
                device_id=0,
                game_id="",
                slot=0,
                sequence=sequence,
                payload=bytes([exc.code]),
            ),
            addr,
        )

    async def _dispatch(self, message: Message, addr) -> None:
        handlers = {
            MsgType.HELLO: self._on_hello,
            MsgType.HEARTBEAT: self._on_heartbeat,
            MsgType.PUSH_BEGIN: self._on_push_begin,
            MsgType.PUSH_DELTA: self._on_push_delta,
            MsgType.PUSH_END: self._on_push_end,
            MsgType.PULL_REQ: self._on_pull_req,
        }
        handler = handlers.get(message.msg_type)
        if handler is None:
            log.warning(
                "unhandled message type",
                extra={"peer": _peer(addr), "msg_type": message.msg_type},
            )
            self._nack(message, addr, Error.MALFORMED_HEADER)
            return

        try:
            result = handler(message, addr)
            if asyncio.iscoroutine(result):
                await result
        except Exception:
            log.exception(
                "handler failed",
                extra={"peer": _peer(addr), "msg_type": message.msg_type},
            )

    # --- handlers ---------------------------------------------------------

    async def _on_hello(self, message: Message, addr) -> None:
        """Announce: the client learns server time and protocol version."""
        await asyncio.to_thread(self.store.touch_device, message.device_id, kind="wii")
        # u64 unix seconds, then the protocol version this server speaks.
        payload = struct.pack(">QB", int(time.time()), message.version)
        self._send(reply_to(message, MsgType.ACK, payload=payload), addr)

    def _on_heartbeat(self, message: Message, addr) -> None:
        """Keeps a NAT mapping alive. No side effects, by design -- sending the
        packet is the whole point, so it must not touch staging expiry."""
        self._send(reply_to(message, MsgType.ACK), addr)

    async def _on_push_begin(self, message: Message, addr) -> None:
        if message.total_size > self.config.max_card_bytes:
            self._nack(message, addr, Error.TOO_LARGE)
            return
        if message.total_size == 0:
            self._nack(message, addr, Error.MALFORMED_HEADER)
            return

        key = _key(message)
        if key not in self.staging and len(self.staging) >= self.config.max_staging:
            log.warning(
                "staging buffers exhausted",
                extra={"peer": _peer(addr), "in_flight": len(self.staging)},
            )
            self._nack(message, addr, Error.RATE_LIMITED)
            return

        is_delta = bool(message.flags & FLAG_DELTA)
        if is_delta:
            seed = await self._seed_from_parent(message)
            if seed is None:
                # Not the client's fault and not a conflict: it retries as a
                # whole-card push. A client without that fallback would find a
                # card unpushable the moment its parent blob was pruned.
                self._nack(message, addr, Error.DELTA_UNAVAILABLE)
                return
        else:
            seed = bytearray(message.total_size)

        expected = chunk_count(message.total_size)
        # A repeated PUSH_BEGIN restarts the transfer: the client is retrying,
        # and keeping half-filled bytes from a previous attempt is how you get a
        # card that passes its checksum and is still wrong. The delta
        # declaration is discarded with them, for the same reason.
        #
        # A remembered reply goes too. A client that has reached PUSH_END
        # retransmits *that*, never PUSH_BEGIN, so a PUSH_BEGIN arriving after a
        # transfer settled is a new attempt that happens to reuse the id -- and
        # answering it later with the previous attempt's verdict would be a lie.
        self.settled.pop(key, None)
        self.staging[key] = Staging(
            total_size=message.total_size,
            parent_version=message.parent_version,
            device_id=message.device_id,
            game_id=message.game_id,
            slot=message.slot,
            addr=addr,
            data=seed,
            expected=expected,
            received=bytearray((expected + 7) // 8),
            is_delta=is_delta,
            declared=bytearray((expected + 7) // 8),
        )
        log.info(
            "push begins",
            extra={
                "peer": _peer(addr),
                "game_id": message.game_id,
                "slot": message.slot,
                "total_size": message.total_size,
                "chunks": expected,
                "parent": message.parent_version,
                "delta": is_delta,
            },
        )
        self._send(reply_to(message, MsgType.ACK), addr)

    async def _seed_from_parent(self, message: Message) -> bytearray | None:
        """The parent version's bytes, for a delta push to overwrite parts of.

        None means this delta cannot be served -- no such version, a pruned
        blob, or a card that has changed size since. All three are answered with
        NACK 0x0C and are the client's cue to send the whole card instead.
        """
        if message.parent_version == 0:
            # v0 is "the card the server has never seen". There is nothing to
            # seed from, and a delta against it would commit mostly zeros.
            return None
        try:
            version = self.store.get_version(
                message.game_id, message.slot, message.parent_version
            )
            image = await asyncio.to_thread(self.store.read_image, version)
        except (NotFoundError, ValidationError) as exc:
            log.info(
                "delta refused, cannot seed",
                extra={
                    "game_id": message.game_id,
                    "slot": message.slot,
                    "parent": message.parent_version,
                    "detail": str(exc),
                },
            )
            return None

        if len(image) != message.total_size:
            log.info(
                "delta refused, the card changed size",
                extra={
                    "game_id": message.game_id,
                    "slot": message.slot,
                    "parent": message.parent_version,
                    "parent_size": len(image),
                    "total_size": message.total_size,
                },
            )
            return None
        return bytearray(image)

    def _on_push_delta(self, message: Message, addr) -> None:
        """One window of the chunk bitmap a delta push will send.

        Acked, unlike a chunk: a lost declaration would otherwise leave the
        server believing the delta was empty, and that is only discovered at
        PUSH_END as a digest failure -- costing a whole-card restart for one
        dropped datagram.
        """
        staging = self.staging.get(_key(message))
        if staging is None:
            self._nack(message, addr, Error.STAGING_EXPIRED)
            return
        if not staging.is_delta:
            # The transfer was not opened with the delta flag, so seeding never
            # happened and everything outside the declaration would be zeros.
            self._nack(message, addr, Error.MALFORMED_HEADER)
            return

        for sequence in unpack_bitmap(message.payload, message.sequence):
            if not staging.declare(sequence):
                self._nack(message, addr, Error.MALFORMED_HEADER)
                return

        staging.last_chunk_at = time.monotonic()
        self._send(reply_to(message, MsgType.ACK), addr)

    def _on_push_chunk(self, message: Message, addr) -> None:
        """One chunk. Silent on success: acking each one would double traffic,
        and PUSH_END is where gaps get reported."""
        staging = self.staging.get(_key(message))
        if staging is None:
            self._nack(message, addr, Error.STAGING_EXPIRED)
            return
        if not staging.store_chunk(message.sequence, message.offset, message.payload):
            self._nack(message, addr, Error.MALFORMED_HEADER)

    async def _on_push_end(self, message: Message, addr) -> None:
        key = _key(message)
        staging = self.staging.get(key)
        if staging is None:
            # Already decided, and the client is asking again because our reply
            # did not arrive. Give the same answer: the decision was made once,
            # against bytes we have since dropped, and re-deciding is not on the
            # table. See `Settled`.
            settled = self.settled.get(key)
            if settled is not None:
                log.info(
                    "replaying the reply to a resent PUSH_END",
                    extra={
                        "peer": _peer(addr),
                        "game_id": message.game_id,
                        "slot": message.slot,
                        "acked": settled.msg_type == MsgType.ACK,
                        "version": settled.card_version,
                    },
                )
                self._send(
                    reply_to(
                        message,
                        settled.msg_type,
                        card_version=settled.card_version,
                        payload=settled.payload,
                    ),
                    addr,
                )
                return
            self._nack(message, addr, Error.STAGING_EXPIRED)
            return

        if not staging.complete:
            missing = staging.missing()
            log.info(
                "push incomplete, asking for gaps",
                extra={
                    "peer": _peer(addr),
                    "game_id": staging.game_id,
                    "missing": len(missing),
                    # On a delta only the declared chunks were ever owed, so
                    # `expected` on its own reads as far worse than it is.
                    "expected": staging.expected,
                    "delta": staging.is_delta,
                    "outstanding": staging.outstanding,
                },
            )
            self._nack_missing(message, addr, missing)
            return

        image = bytes(staging.data)
        if hashlib.sha256(image).digest() != message.payload:
            # Every chunk arrived and the whole still hashes wrong, so the
            # buffer is poisoned; drop it and make the client start over.
            log.warning(
                "push failed its digest",
                extra={"peer": _peer(addr), "game_id": staging.game_id},
            )
            self._settle(message, addr, key, Error.CHECKSUM_MISMATCH)
            return

        try:
            result = await asyncio.to_thread(
                self.store.push,
                staging.game_id,
                staging.slot,
                image,
                parent=staging.parent_version,
                device_id=staging.device_id,
                note="pushed over udp",
            )
        except ConflictError as exc:
            log.info(
                "udp push rejected as a conflict",
                extra={
                    "peer": _peer(addr),
                    "game_id": staging.game_id,
                    "parent": staging.parent_version,
                    "head": exc.head_version,
                },
            )
            # card_version carries the head, so the client can pull it and let
            # a human choose rather than guessing.
            self._settle(
                message, addr, key, Error.CONFLICT, card_version=exc.head_version
            )
            return
        except TooLargeError:
            self._settle(message, addr, key, Error.TOO_LARGE)
            return
        except ValidationError as exc:
            log.warning(
                "udp push failed validation",
                extra={"peer": _peer(addr), "detail": str(exc)},
            )
            self._settle(message, addr, key, Error.FAILED_VALIDATION)
            return

        self._settle(message, addr, key, None, card_version=result.version)

    def _settle(
        self,
        message: Message,
        addr,
        key: tuple,
        code: Error | None,
        *,
        card_version: int = 0,
    ) -> None:
        """Finish a transfer: drop its buffer, remember the reply, send it.

        `code` None is an ACK; anything else is that NACK. Every terminal answer
        to PUSH_END goes through here, so the retransmission of any of them --
        not just the successful one -- gets the answer it earned rather than
        STAGING_EXPIRED. That matters for the failures too: a client told
        CHECKSUM_MISMATCH resends the whole card, and a client told 0x0b instead
        does not.
        """
        msg_type = MsgType.ACK if code is None else MsgType.NACK
        payload = b"" if code is None else bytes([code])

        self.staging.pop(key, None)
        self.settled[key] = Settled(
            msg_type=msg_type, card_version=card_version, payload=payload
        )
        self._send(
            reply_to(message, msg_type, card_version=card_version, payload=payload),
            addr,
        )

    async def _on_pull_req(self, message: Message, addr) -> None:
        """Send a card back, chunked and paced.

        `card_version` selects a version; 0 means head. A payload, if present,
        is a bitmap of the chunks the client still wants, based at the header's
        `sequence` -- so a retry costs only the gaps.
        """
        try:
            version = (
                self.store.head(message.game_id, message.slot)
                if message.card_version == 0
                else self.store.get_version(
                    message.game_id, message.slot, message.card_version
                )
            )
        except (NotFoundError, ValidationError):
            version = None

        if version is None:
            self._nack(message, addr, Error.UNKNOWN_CARD)
            return

        try:
            image = await asyncio.to_thread(self.store.read_image, version)
        except NotFoundError:
            self._nack(message, addr, Error.UNKNOWN_CARD)
            return

        total = len(image)
        chunks = chunk_count(total)
        wanted = (
            unpack_bitmap(message.payload, message.sequence)
            if message.payload
            else range(chunks)
        )

        self._send(
            reply_to(
                message,
                MsgType.ACK,
                card_version=version.version,
                total_size=total,
            ),
            addr,
        )

        sent = 0
        for sequence in wanted:
            if sequence >= chunks:
                continue
            offset = sequence * MAX_PAYLOAD
            self._send(
                Message(
                    msg_type=MsgType.PULL_CHUNK,
                    device_id=message.device_id,
                    game_id=version.game_id,
                    slot=version.slot,
                    card_version=version.version,
                    offset=offset,
                    total_size=total,
                    sequence=sequence,
                    payload=image[offset : offset + MAX_PAYLOAD],
                ),
                addr,
            )
            sent += 1
            # Pace the burst: a 2 MiB card is 2048 datagrams and the receiving
            # WiFi stack is from 2006.
            if sent % self.config.pull_burst == 0:
                await asyncio.sleep(self.config.pull_burst_delay)

        log.info(
            "pull served",
            extra={
                "peer": _peer(addr),
                "game_id": version.game_id,
                "slot": version.slot,
                "version": version.version,
                "chunks": sent,
            },
        )

    # --- send -------------------------------------------------------------

    def _send(self, message: Message, addr) -> None:
        if self.transport is None:
            return
        self.transport.sendto(pack(message, self.key), addr)

    def _nack(self, message: Message, addr, code: Error, **overrides) -> None:
        self._send(
            reply_to(message, MsgType.NACK, payload=bytes([code]), **overrides), addr
        )

    def _nack_missing(self, message: Message, addr, missing: list[int]) -> None:
        """NACK 0x06, one datagram per bitmap window.

        A 16 MiB card needs 2048 bitmap bytes against a 1024-byte payload cap,
        so the bitmap is windowed and the base index rides in `sequence`.
        """
        for base, bitmap in bitmap_windows(missing):
            self._send(
                reply_to(
                    message,
                    MsgType.NACK,
                    sequence=base,
                    payload=bytes([Error.MISSING_CHUNKS]) + bitmap,
                ),
                addr,
            )

    # --- expiry -----------------------------------------------------------

    async def _sweep_forever(self) -> None:
        interval = max(1.0, self.config.staging_ttl / 8)
        try:
            while True:
                await asyncio.sleep(interval)
                self.sweep()
        except asyncio.CancelledError:
            pass

    def sweep(self, now: float | None = None) -> int:
        """Drop staging buffers that have gone quiet. Log and drop, per §4."""
        now = time.monotonic() if now is None else now
        dead = [
            key
            for key, staging in self.staging.items()
            if now - staging.last_chunk_at > self.config.staging_ttl
        ]
        for key in dead:
            staging = self.staging.pop(key)
            log.info(
                "staging buffer expired",
                extra={
                    "game_id": staging.game_id,
                    "slot": staging.slot,
                    "received": staging.received_count,
                    "expected": staging.expected,
                },
            )

        # Remembered replies go on the same timer, and for the same reason it
        # was chosen: a transfer cannot outlive the staging TTL, so a client
        # still retransmitting PUSH_END after one has given up anyway. They are
        # tens of bytes each, so this is about tidiness rather than memory.
        for key in [
            key
            for key, settled in self.settled.items()
            if now - settled.at > self.config.staging_ttl
        ]:
            del self.settled[key]

        return len(dead)


def _key(message: Message) -> tuple:
    return (message.device_id, message.game_id, message.slot, message.card_version)


def _peer(addr) -> str:
    try:
        return f"{addr[0]}:{addr[1]}"
    except (TypeError, IndexError):
        return str(addr)


async def start_listener(
    config: Config, store: Store
) -> tuple[asyncio.DatagramTransport, SlotSyncProtocol]:
    """Bind the UDP listener on the running loop."""
    loop = asyncio.get_running_loop()
    transport, protocol = await loop.create_datagram_endpoint(
        lambda: SlotSyncProtocol(config, store),
        local_addr=(config.udp_host, config.udp_port),
    )

    # Absorb a whole card's worth of burst. Without this the kernel silently
    # drops a large fraction of an unpaced 2048-datagram push and the client
    # pays for it in retransmission rounds.
    sock = transport.get_extra_info("socket")
    actual = None
    if sock is not None:
        with contextlib.suppress(OSError):
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, config.udp_rcvbuf)
            actual = sock.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF)

        if sys.platform == "win32":
            # Windows reports an ICMP port-unreachable from a previous sendto as
            # an error on the *next* receive, which is not how UDP is supposed
            # to behave for an unconnected socket and can knock the listener
            # over when a console disappears mid-pull. SIO_UDP_CONNRESET turns
            # that off. Deployment is Linux in Docker, where the behaviour does
            # not arise, but people develop against this on Windows.
            SIO_UDP_CONNRESET = 0x9800000C
            with contextlib.suppress(OSError, AttributeError):
                sock.ioctl(SIO_UDP_CONNRESET, False)
    log.info(
        "udp listener bound",
        extra={
            "host": config.udp_host,
            "port": transport.get_extra_info("sockname")[1],
            "rcvbuf": actual,
            "version": __version__,
        },
    )
    return transport, protocol
