#!/usr/bin/env python3
"""Stand-in for the Wii client. Speaks the binary UDP protocol.

Two jobs. It is the M4 test harness -- it can inject packet loss and
reordering, because the real client will meet both on 2006 WiFi. And it is a
reference for whoever writes the C client, which is why it is written against
`docs/PROTOCOL.md` with its own struct definitions rather than importing
`slotsync.protocol`: sharing the server's code would hide a wire-format bug
from both sides at once.

    python scripts/fake_console.py --psk KEY hello
    python scripts/fake_console.py --psk KEY push GALE01 A card.raw --parent 0
    python scripts/fake_console.py --psk KEY pull GALE01 A out.raw
    python scripts/fake_console.py --psk KEY push GALE01 A card.raw --parent 0 \\
        --loss 0.2 --reorder
"""

from __future__ import annotations

import argparse
import hashlib
import hmac
import os
import random
import socket
import struct
import sys
import time
from pathlib import Path

MAGIC = b"SLOT"
VERSION = 1
HEADER = struct.Struct(">4sBBHQ6sBBIIIIII16s32s")
HEADER_SIZE = 96
HMAC_OFFSET = 64
MAX_PAYLOAD = 1024

HELLO, PULL_REQ, PUSH_BEGIN, PUSH_CHUNK, PUSH_END = 0x01, 0x02, 0x03, 0x04, 0x05
ACK, NACK, PULL_CHUNK, HEARTBEAT = 0x06, 0x07, 0x08, 0x09

ERRORS = {
    0x01: "bad hmac",
    0x02: "unsupported version",
    0x03: "malformed header",
    0x04: "unknown game_id / slot",
    0x05: "conflict: parent_version is not head",
    0x06: "missing chunks",
    0x07: "checksum mismatch",
    0x08: "card failed validation",
    0x09: "too large",
    0x0A: "rate limited",
    0x0B: "staging buffer expired",
}


class ProtocolError(RuntimeError):
    pass


# --- wire -----------------------------------------------------------------


def build(
    key: bytes,
    msg_type: int,
    *,
    device_id=0,
    game_id="",
    slot=0,
    card_version=0,
    parent_version=0,
    offset=0,
    total_size=0,
    sequence=0,
    payload=b"",
) -> bytes:
    """Pack and sign one datagram."""
    header = HEADER.pack(
        MAGIC,
        VERSION,
        msg_type,
        0,
        device_id,
        game_id.encode("ascii")[:6].ljust(6, b" "),
        slot,
        0,
        card_version,
        parent_version,
        offset,
        len(payload),
        total_size,
        sequence,
        os.urandom(16),
        b"\x00" * 32,
    )
    signature = sign(header, payload, key)
    return header[:HMAC_OFFSET] + signature + header[HMAC_OFFSET + 32 :] + payload


def sign(header: bytes, payload: bytes, key: bytes) -> bytes:
    zeroed = header[:HMAC_OFFSET] + b"\x00" * 32 + header[HMAC_OFFSET + 32 :]
    mac = hmac.new(key, zeroed, "sha256")
    mac.update(payload)
    return mac.digest()


def parse(datagram: bytes, key: bytes) -> dict:
    """Unpack and verify a datagram from the server."""
    if len(datagram) < HEADER_SIZE:
        raise ProtocolError(f"short datagram: {len(datagram)} bytes")

    header, payload = datagram[:HEADER_SIZE], datagram[HEADER_SIZE:]
    fields = HEADER.unpack(header)
    if fields[0] != MAGIC:
        raise ProtocolError(f"bad magic {fields[0]!r}")
    if not hmac.compare_digest(fields[15], sign(header, payload, key)):
        raise ProtocolError("server reply failed hmac -- wrong PSK?")

    return {
        "msg_type": fields[2],
        "device_id": fields[4],
        "game_id": fields[5].decode("ascii").strip(),
        "slot": fields[6],
        "card_version": fields[8],
        "parent_version": fields[9],
        "offset": fields[10],
        "total_size": fields[12],
        "sequence": fields[13],
        "payload": payload,
    }


def bits_set(bitmap: bytes, base: int) -> list[int]:
    """Chunk indices flagged in a windowed bitmap. LSB-first."""
    return [
        base + i * 8 + bit
        for i, byte in enumerate(bitmap)
        if byte
        for bit in range(8)
        if byte & (1 << bit)
    ]


def build_bitmap(indices, base: int, count: int) -> bytes:
    bitmap = bytearray((count + 7) // 8)
    for index in indices:
        offset = index - base
        if 0 <= offset < count:
            bitmap[offset >> 3] |= 1 << (offset & 7)
    return bytes(bitmap)


# --- link -----------------------------------------------------------------


class Link:
    """A UDP socket that can be told to behave like bad WiFi."""

    def __init__(
        self,
        host,
        port,
        key,
        *,
        loss=0.0,
        reorder=False,
        timeout=2.0,
        seed=None,
        verbose=False,
    ):
        self.addr = (host, port)
        self.key = key
        self.loss = loss
        self.reorder = reorder
        self.verbose = verbose
        self.random = random.Random(seed)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(timeout)
        self.sent = self.dropped_out = self.dropped_in = 0

    def close(self):
        self.sock.close()

    def send(self, datagram: bytes, *, lossy=True) -> None:
        """Send, unless this is one of the packets the link eats."""
        self.sent += 1
        if lossy and self.loss and self.random.random() < self.loss:
            self.dropped_out += 1
            return
        self.sock.sendto(datagram, self.addr)

    def send_many(self, datagrams: list[bytes], *, pace=0.0) -> None:
        """Send a batch, optionally shuffled to simulate reordering."""
        batch = list(datagrams)
        if self.reorder:
            self.random.shuffle(batch)
        for datagram in batch:
            self.send(datagram)
            if pace:
                time.sleep(pace)

    def recv(self, *, lossy=True) -> dict | None:
        """Receive one message, or None on timeout."""
        while True:
            try:
                datagram, _ = self.sock.recvfrom(65535)
            except TimeoutError:
                return None
            except OSError:
                return None

            if lossy and self.loss and self.random.random() < self.loss:
                self.dropped_in += 1
                continue
            return parse(datagram, self.key)

    def expect(self, msg_type: int, what: str) -> dict:
        message = self.recv(lossy=False)
        if message is None:
            raise ProtocolError(f"timed out waiting for {what}")
        if message["msg_type"] == NACK:
            code = message["payload"][0] if message["payload"] else 0
            raise ProtocolError(
                f"{what}: NACK 0x{code:02x} {ERRORS.get(code, 'unknown')}"
            )
        if message["msg_type"] != msg_type:
            raise ProtocolError(
                f"{what}: expected type 0x{msg_type:02x}, got 0x{message['msg_type']:02x}"
            )
        return message


# --- operations -----------------------------------------------------------


def do_hello(link: Link, device_id: int) -> int:
    link.send(build(link.key, HELLO, device_id=device_id), lossy=False)
    reply = link.expect(ACK, "HELLO")
    server_time, version = struct.unpack(">QB", reply["payload"][:9])
    stamp = time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(server_time))
    print(f"server time {stamp} UTC, protocol v{version}")
    return 0


def do_push(
    link: Link,
    device_id,
    game_id,
    slot,
    path: Path,
    parent: int,
    transfer_id: int,
    rounds: int,
    pace: float,
) -> int:
    image = path.read_bytes()
    total = len(image)
    chunks = (total + MAX_PAYLOAD - 1) // MAX_PAYLOAD
    digest = hashlib.sha256(image).digest()

    def head(msg_type, **kw):
        return dict(
            device_id=device_id,
            game_id=game_id,
            slot=slot,
            card_version=transfer_id,
            **kw,
        )

    print(f"pushing {path.name}: {total} bytes, {chunks} chunks, parent={parent}")

    link.send(
        build(
            link.key,
            PUSH_BEGIN,
            total_size=total,
            parent_version=parent,
            **head(PUSH_BEGIN),
        ),
        lossy=False,
    )
    link.expect(ACK, "PUSH_BEGIN")

    def chunk_datagram(index: int) -> bytes:
        offset = index * MAX_PAYLOAD
        return build(
            link.key,
            PUSH_CHUNK,
            offset=offset,
            sequence=index,
            total_size=total,
            payload=image[offset : offset + MAX_PAYLOAD],
            **head(PUSH_CHUNK),
        )

    outstanding = list(range(chunks))
    for attempt in range(1, rounds + 1):
        link.send_many([chunk_datagram(i) for i in outstanding], pace=pace)

        link.send(
            build(link.key, PUSH_END, total_size=total, payload=digest, **head(PUSH_END)),
            lossy=False,
        )

        reply = link.recv(lossy=False)
        if reply is None:
            raise ProtocolError("timed out waiting for the PUSH_END reply")

        if reply["msg_type"] == ACK:
            print(
                f"committed as version {reply['card_version']} "
                f"after {attempt} round(s); "
                f"dropped {link.dropped_out} out / {link.dropped_in} in"
            )
            return 0

        code = reply["payload"][0] if reply["payload"] else 0
        if code != 0x06:
            raise ProtocolError(f"PUSH_END: NACK 0x{code:02x} {ERRORS.get(code, '?')}")

        # NACK 0x06 carries a windowed bitmap of the gaps; the base index is in
        # the header's sequence field. Collect every window the server sent.
        outstanding = bits_set(reply["payload"][1:], reply["sequence"])
        while True:
            more = link.recv(lossy=False)
            if more is None:
                break
            if more["msg_type"] == NACK and more["payload"][:1] == b"\x06":
                outstanding += bits_set(more["payload"][1:], more["sequence"])
            elif more["msg_type"] == ACK:
                print(f"committed as version {more['card_version']}")
                return 0

        outstanding = sorted(set(outstanding))
        print(
            f"  round {attempt}: server is missing {len(outstanding)} chunk(s), resending"
        )

    raise ProtocolError(f"gave up after {rounds} rounds")


def do_pull(
    link: Link, device_id, game_id, slot, path: Path, version: int, rounds: int
) -> int:
    wanted_payload, base = b"", 0
    received: dict[int, bytes] = {}
    total = chunks = 0
    card_version = version

    for attempt in range(1, rounds + 1):
        link.send(
            build(
                link.key,
                PULL_REQ,
                device_id=device_id,
                game_id=game_id,
                slot=slot,
                card_version=card_version,
                sequence=base,
                payload=wanted_payload,
            ),
            lossy=False,
        )
        ack = link.expect(ACK, "PULL_REQ")
        card_version, total = ack["card_version"], ack["total_size"]
        chunks = (total + MAX_PAYLOAD - 1) // MAX_PAYLOAD
        if attempt == 1:
            print(f"pulling version {card_version}: {total} bytes, {chunks} chunks")

        while len(received) < chunks:
            message = link.recv()
            if message is None:
                break  # server finished, or the link ate the rest
            if message["msg_type"] == PULL_CHUNK:
                received[message["sequence"]] = message["payload"]

        if len(received) >= chunks:
            break

        missing = sorted(set(range(chunks)) - received.keys())
        print(f"  round {attempt}: missing {len(missing)} chunk(s), asking again")
        base = (missing[0] // ((MAX_PAYLOAD - 1) * 8)) * ((MAX_PAYLOAD - 1) * 8)
        wanted_payload = build_bitmap(missing, base, (MAX_PAYLOAD - 1) * 8)
    else:
        raise ProtocolError(f"gave up after {rounds} rounds")

    image = b"".join(received[i] for i in range(chunks))[:total]
    path.write_bytes(image)
    print(
        f"wrote {path} -- {len(image)} bytes, sha256 "
        f"{hashlib.sha256(image).hexdigest()[:16]}…; "
        f"dropped {link.dropped_out} out / {link.dropped_in} in"
    )
    return 0


# --- cli ------------------------------------------------------------------


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9977)
    parser.add_argument(
        "--psk",
        default=os.environ.get("SLOTSYNC_PSK", ""),
        help="pre-shared key; defaults to $SLOTSYNC_PSK",
    )
    parser.add_argument("--device", type=lambda v: int(v, 0), default=0x57494900_00000001)
    parser.add_argument(
        "--loss",
        type=float,
        default=0.0,
        help="drop this fraction of datagrams, each way",
    )
    parser.add_argument(
        "--reorder", action="store_true", help="shuffle chunks within each send batch"
    )
    parser.add_argument("--seed", type=int, default=None)
    parser.add_argument("--timeout", type=float, default=2.0)
    parser.add_argument(
        "--rounds", type=int, default=8, help="retransmission rounds before giving up"
    )
    parser.add_argument(
        "--pace", type=float, default=0.0, help="seconds to wait between chunks"
    )

    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("hello")

    push = sub.add_parser("push")
    push.add_argument("game_id")
    push.add_argument("slot")
    push.add_argument("path", type=Path)
    push.add_argument(
        "--parent",
        type=int,
        required=True,
        help="version this image came from; 0 for a new card",
    )
    push.add_argument(
        "--transfer-id",
        type=int,
        default=1,
        help="client-chosen id echoed through the transfer",
    )

    pull = sub.add_parser("pull")
    pull.add_argument("game_id")
    pull.add_argument("slot")
    pull.add_argument("path", type=Path)
    pull.add_argument("--version", type=int, default=0, help="0 means head")

    args = parser.parse_args(argv)
    if not args.psk:
        parser.error("no PSK: pass --psk or set SLOTSYNC_PSK")

    slot = {"A": 0, "B": 1}.get(str(getattr(args, "slot", "A")).upper())
    if slot is None and hasattr(args, "slot"):
        slot = int(args.slot)

    link = Link(
        args.host,
        args.port,
        args.psk.encode(),
        loss=args.loss,
        reorder=args.reorder,
        timeout=args.timeout,
        seed=args.seed,
    )
    try:
        if args.command == "hello":
            return do_hello(link, args.device)
        if args.command == "push":
            return do_push(
                link,
                args.device,
                args.game_id,
                slot,
                args.path,
                args.parent,
                args.transfer_id,
                args.rounds,
                args.pace,
            )
        return do_pull(
            link, args.device, args.game_id, slot, args.path, args.version, args.rounds
        )
    except ProtocolError as exc:
        print(f"fake_console: {exc}", file=sys.stderr)
        return 1
    finally:
        link.close()


if __name__ == "__main__":
    sys.exit(main())
