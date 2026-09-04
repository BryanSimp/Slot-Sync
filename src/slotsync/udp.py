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
- The server never sends unsolicited. Every datagram out is a reply.
"""

from __future__ import annotations

import asyncio
import contextlib
import hashlib
import logging
import socket
import struct
import time
from dataclasses import dataclass, field

from . import __version__
from .config import Config
from .protocol import (
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
    received_count: int = 0
    last_chunk_at: float = field(default_factory=time.monotonic)

    @property
    def complete(self) -> bool:
        return self.received_count >= self.expected

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
        self.last_chunk_at = time.monotonic()
        return True

    def missing(self) -> list[int]:
        return [
            index
            for index in range(self.expected)
            if not self.received[index >> 3] & (1 << (index & 7))
        ]


class SlotSyncProtocol(asyncio.DatagramProtocol):
    """Server side of the binary protocol."""

    def __init__(self, config: Config, store: Store) -> None:
        self.config = config
        self.store = store
        self.key = config.psk
        self.transport: asyncio.DatagramTransport | None = None
        self.staging: dict[tuple, Staging] = {}
        self._tasks: set[asyncio.Task] = set()
        self._sweeper: asyncio.Task | None = None

    # --- lifecycle --------------------------------------------------------

    def connection_made(self, transport) -> None:
        self.transport = transport
        self._sweeper = asyncio.create_task(self._sweep_forever())

    def connection_lost(self, exc) -> None:
        if self._sweeper is not None:
            self._sweeper.cancel()
        for task in list(self._tasks):
            task.cancel()

    def close(self) -> None:
        if self.transport is not None:
            self.transport.close()

    # --- receive ----------------------------------------------------------

    def datagram_received(self, data: bytes, addr) -> None:
        try:
            message = unpack(data, self.key)
        except ProtocolError as exc:
            self._reject(data, addr, exc)
            return
        except Exception:
            # A malformed datagram must never take the listener down.
            log.exception("undecodable datagram", extra={"peer": _peer(addr)})
            return

        # PUSH_CHUNK is the hot path and is pure memory work, so it is handled
        # inline rather than paying for a task per chunk.
        if message.msg_type == MsgType.PUSH_CHUNK:
            self._on_push_chunk(message, addr)
            return

        task = asyncio.create_task(self._dispatch(message, addr))
        self._tasks.add(task)
        task.add_done_callback(self._tasks.discard)

    def _reject(self, data: bytes, addr, exc: ProtocolError) -> None:
        """NACK an unusable datagram.

        A bad-HMAC datagram cannot be trusted, so the reply echoes nothing from
        it beyond the sequence number, and the reply is no larger than the
        request -- there is no amplification here for a spoofed source to use.
        Answering at all is worth it: a client built with the wrong PSK is
        otherwise met with silence.
        """
        log.warning(
            "rejected datagram",
            extra={"peer": _peer(addr), "code": exc.code.name, "detail": str(exc)},
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

        expected = chunk_count(message.total_size)
        # A repeated PUSH_BEGIN restarts the transfer: the client is retrying,
        # and keeping half-filled bytes from a previous attempt is how you get a
        # card that passes its checksum and is still wrong.
        self.staging[key] = Staging(
            total_size=message.total_size,
            parent_version=message.parent_version,
            device_id=message.device_id,
            game_id=message.game_id,
            slot=message.slot,
            addr=addr,
            data=bytearray(message.total_size),
            expected=expected,
            received=bytearray((expected + 7) // 8),
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
            },
        )
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
                    "expected": staging.expected,
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
            del self.staging[key]
            self._nack(message, addr, Error.CHECKSUM_MISMATCH)
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
            del self.staging[key]
            # card_version carries the head, so the client can pull it and let
            # a human choose rather than guessing.
            self._nack(message, addr, Error.CONFLICT, card_version=exc.head_version)
            return
        except TooLargeError:
            del self.staging[key]
            self._nack(message, addr, Error.TOO_LARGE)
            return
        except ValidationError as exc:
            log.warning(
                "udp push failed validation",
                extra={"peer": _peer(addr), "detail": str(exc)},
            )
            del self.staging[key]
            self._nack(message, addr, Error.FAILED_VALIDATION)
            return

        del self.staging[key]
        self._send(reply_to(message, MsgType.ACK, card_version=result.version), addr)

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
