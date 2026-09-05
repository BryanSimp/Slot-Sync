"""Replay protection on the message nonce -- PLAN.md section 10, M5.

## What is protected, and what deliberately is not

Only **control** messages -- HELLO, PULL_REQ, PUSH_BEGIN, PUSH_END -- are
checked. `PUSH_CHUNK` is not, for two reasons that both matter:

1. Duplicate chunks are an explicitly supported operation. PLAN.md §4: "Chunks
   may arrive out of order and may be duplicated. Duplicates overwrite
   idempotently." Rejecting a repeat would break the retransmission path the
   protocol depends on.
2. A kernel client retransmitting a gap may well resend the datagram it already
   built, byte for byte, rather than rebuilding it. Requiring a fresh nonce per
   chunk would make the cheap implementation the broken one.

Chunks are also the only high-volume message. Tracking 2048 nonces per transfer
to protect an operation that is idempotent anyway would be paying for nothing.

## What this requires of a client

**A control message must carry a fresh nonce every time it is sent, including
retransmissions.** A client that resends PUSH_END verbatim after a lost ACK
will have it dropped as a replay. `docs/PROTOCOL.md` says so; the counter plus
boot-time entropy scheme suggested there satisfies it for free.
"""

from __future__ import annotations

import time

from .protocol import MsgType

#: Message types whose replay is worth preventing. See the module docstring for
#: why PUSH_CHUNK is not among them.
GUARDED = frozenset(
    {MsgType.HELLO, MsgType.PULL_REQ, MsgType.PUSH_BEGIN, MsgType.PUSH_END}
)


class NonceCache:
    """Recently seen nonces, per device, within a time window.

    Bounded in both directions: entries older than `ttl` are dropped, and a
    single device cannot hold more than `per_device` of them.
    """

    def __init__(self, ttl: float = 120.0, per_device: int = 512, max_devices: int = 64):
        self.ttl = ttl
        self.per_device = per_device
        self.max_devices = max_devices
        #: device_id -> {nonce: first seen}. Insertion order is age order,
        #: which is what makes the eviction below cheap.
        self._seen: dict[int, dict[bytes, float]] = {}

    def check_and_record(
        self, device_id: int, nonce: bytes, msg_type: int, now: float | None = None
    ) -> bool:
        """True if this message is fresh. False if it is a replay.

        Unguarded message types always return True without being recorded.
        """
        if msg_type not in GUARDED:
            return True

        now = time.monotonic() if now is None else now
        nonces = self._seen.get(device_id)

        if nonces is None:
            if len(self._seen) >= self.max_devices:
                self._sweep(now)
            if len(self._seen) >= self.max_devices:
                # More distinct device ids than a household has consoles. The
                # ids are authenticated, so this is not a spoofing surface, but
                # it is not a state we should grow memory for either.
                return False
            nonces = self._seen[device_id] = {}

        self._expire(nonces, now)

        if nonce in nonces:
            return False

        if len(nonces) >= self.per_device:
            # Drop the oldest. dicts keep insertion order, and entries are only
            # ever appended, so the first key is the oldest.
            del nonces[next(iter(nonces))]

        nonces[nonce] = now
        return True

    def _expire(self, nonces: dict[bytes, float], now: float) -> None:
        cutoff = now - self.ttl
        for nonce, seen in list(nonces.items()):
            if seen < cutoff:
                del nonces[nonce]
            else:
                break  # insertion-ordered, so the rest are newer

    def _sweep(self, now: float) -> None:
        for device_id, nonces in list(self._seen.items()):
            self._expire(nonces, now)
            if not nonces:
                del self._seen[device_id]

    def __len__(self) -> int:
        return sum(len(nonces) for nonces in self._seen.values())
