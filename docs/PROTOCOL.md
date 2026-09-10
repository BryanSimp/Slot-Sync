# SlotSync Binary Protocol v1

Transport for consoles. Big-endian. UDP, default port 9977.

This exists because the eventual client is C code running inside Nintendont's ARM
kernel, where there is no TLS, no JSON parser worth having, a few KB of stack, and hard
real-time deadlines. Every design choice below follows from that. See `PLAN.md` §2.

## Header

96 bytes, then payload.

```
off  size  field
0    4     magic, ASCII "SLOT"
4    1     version = 1
5    1     msg_type
6    2     flags (reserved, send 0)
8    8     device_id, u64
16   6     game_id, ASCII, space-padded, e.g. "GALE01"
22   1     slot, 0 = A, 1 = B
23   1     reserved
24   4     card_version, u32
28   4     parent_version, u32
32   4     offset, u32, byte offset into the card image
36   4     length, u32, payload bytes, max 1024
40   4     total_size, u32, full card size in bytes
44   4     sequence, u32, chunk index within the transfer
48   16    nonce
64   32    hmac_sha256
96   ...   payload
```

`hmac_sha256` covers `header[0:96]` with the hmac field zeroed, concatenated with the
payload, keyed by the pre-shared key.

## Message types

| Value | Name | Dir | Payload |
|---|---|---|---|
| 0x01 | HELLO | C→S | none |
| 0x02 | PULL_REQ | C→S | none |
| 0x03 | PUSH_BEGIN | C→S | none; `total_size` and `parent_version` are in the header |
| 0x04 | PUSH_CHUNK | C→S | up to 1024 bytes of card data at `offset` |
| 0x05 | PUSH_END | C→S | 32-byte SHA-256 of the whole card image |
| 0x06 | ACK | S→C | for PUSH_END, the assigned `card_version` |
| 0x07 | NACK | S→C | 1-byte error code, then a bitmap of missing chunk indices |
| 0x08 | PULL_CHUNK | S→C | up to 1024 bytes at `offset` |
| 0x09 | HEARTBEAT | C→S | none |

## Error codes

```
0x01  bad hmac
0x02  unsupported version
0x03  malformed header
0x04  unknown game_id / slot
0x05  conflict: parent_version is not head
0x06  missing chunks (bitmap follows)
0x07  checksum mismatch on PUSH_END
0x08  card failed format validation
0x09  too large
0x0A  rate limited
0x0B  staging buffer expired
```

## Push sequence

```
C→S  PUSH_BEGIN   total_size=2097152, parent_version=7
S→C  ACK
C→S  PUSH_CHUNK   sequence=0    offset=0
C→S  PUSH_CHUNK   sequence=1    offset=1024
     ...                                        (2048 chunks for a 2 MiB card)
C→S  PUSH_END     payload = sha256 of the image
S→C  ACK          card_version=8
```

On `PUSH_END` with gaps, the server replies `NACK 0x06` with a bitmap. The client
retransmits only the missing indices, then sends `PUSH_END` again. Chunks are idempotent
— a duplicate overwrites the same buffer region with the same bytes, harmlessly.

Nothing is committed until `PUSH_END` verifies. A client that loses power mid-transfer
leaves a staging buffer that expires after 120 seconds and is dropped. The previous head
is never touched.

## Pull sequence

```
C→S  PULL_REQ
S→C  ACK          card_version=8, total_size=2097152
S→C  PULL_CHUNK   sequence=0
     ...
S→C  PULL_CHUNK   sequence=2047
```

The client tracks which sequences it received and re-sends `PULL_REQ` with a bitmap in
the payload to request only gaps. Server never retransmits unprompted.

## Notes for the kernel client

- The 96-byte header is fixed-size and aligned so it can be filled in place in a static
  buffer. No allocation per message.
- One HMAC-SHA256 per 1024-byte chunk is the main CPU cost. Measure it before committing
  to per-chunk auth in the kernel build; if it blows the loop budget, the fallback is
  authenticating only PUSH_BEGIN and PUSH_END and accepting chunk-level spoofing on a
  trusted LAN.
- Nonce is for replay protection. The server keeps a short window of recently seen
  nonces per device. A kernel client can use a simple counter plus boot-time entropy.

---

# Settled while implementing M4

Everything above held up. These are the details it did not pin down, decided while
building the server and `scripts/fake_console.py`.

## `card_version` is the transfer id on a push

`PUSH_BEGIN`, `PUSH_CHUNK` and `PUSH_END` all carry the staging key
`(device_id, game_id, slot, card_version)`, but at `PUSH_BEGIN` the client cannot know
what version it will be assigned. So on the push path **`card_version` is an identifier
the client chooses and echoes through the whole transfer**. The version the server
actually assigns comes back in the `PUSH_END` ack, in the same field.

A repeated `PUSH_BEGIN` for the same key **restarts** the transfer and discards whatever
had accumulated. A client that resends `PUSH_BEGIN` is retrying, and keeping half-filled
bytes from an earlier attempt is how you produce a card that passes its checksum and is
still wrong.

## `HELLO` ack payload

```
off  size  field
0    8     server time, u64, unix seconds
8    1     protocol version the server speaks
```

## Bitmaps are windowed

A bitmap covering a whole 16 MiB card needs 2048 bytes, against a 1024-byte payload cap.
So a bitmap covers a window, and **the header's `sequence` field carries the chunk index
the bitmap starts at**. The server sends as many `NACK 0x06` datagrams as the gaps need;
the client accumulates them until they stop arriving.

Bit order is **LSB-first**: chunk `base + n` is `bitmap[n >> 3] & (1 << (n & 7))`. That
is the cheapest form for the kernel client to walk.

The same encoding runs the other way. A `PULL_REQ` carrying a payload is asking only for
the chunks flagged in it, based at `sequence`; a `PULL_REQ` with no payload asks for the
whole card.

## `PULL_REQ` can name a version

`card_version` selects which version to send. **0 means head.** Anything else asks for
that specific version, which is how a console restores an older save without the web UI.

## A conflict `NACK` carries the head

`NACK 0x05` puts the server's current head version in `card_version`, so the client can
pull it and let a human choose. Never merge, never last-write-wins — `PLAN.md` §7.

## Bad HMAC is answered, not ignored

A datagram that fails its HMAC gets a `NACK 0x01`. Nothing from it is trusted or echoed
beyond the sequence number, and the reply is never larger than the request, so a spoofed
source gets no amplification. Silence would leave a client built with the wrong PSK with
no way to tell that apart from a dead server.

## Chunks are not acked individually

Only `PUSH_END` produces a reply. Acking every chunk would double the traffic for no
benefit, since `PUSH_END` is where gaps are reported anyway.

## Measured: pace the burst, and size the receive buffer

Pushing a 2 MiB card as fast as the socket accepts it, over **loopback with no injected
loss**, lost ~713 of 2048 chunks — about a third — and took four retransmission rounds.
Nothing was dropped on the wire; the datagrams overran the server's socket receive
buffer.

Two fixes, both applied:

- The server sets `SO_RCVBUF` to 4 MiB (`SLOTSYNC_UDP_RCVBUF`). With that alone the same
  unpaced push completes in **one round with zero loss**.
- Clients should still pace. `fake_console.py --pace 0.001` also gets a clean single
  round, and the real console will be on 802.11g rather than loopback.

The server paces its own pull the same way, 32 chunks per burst with a 2 ms gap
(`SLOTSYNC_PULL_BURST`, `SLOTSYNC_PULL_BURST_DELAY`).

## Measured on the console: the client must window what it asks for

The same overrun happens in the other direction, and there the receive buffer is not
ours to enlarge. A Wii asking for a whole 2 MiB card in one `PULL_REQ` gets a 2048-chunk
burst and keeps about **forty-five of them**; the rest overrun IOS's socket receive
buffer while the client is still authenticating the first few. Every round then salvages
one bufferful, so the transfer converges at roughly 45 chunks per round and a 2 MiB card
never finishes inside any sane round budget. From a real session, the gap counts falling
2048, 2013, 1947, 1870 ... 987 over the client's 24 rounds, then giving up.

So a client asks for `pull_window` chunks per round (32 by default) rather than for
everything it is missing, and stops listening as soon as that window has landed instead
of waiting out the per-reply timeout. Sixty-four small rounds that each complete beat
twenty-four large ones that each lose 97%.

Two consequences worth knowing:

- The server re-reads the whole blob per `PULL_REQ`, so a windowed pull costs it one
  read per round rather than one per card. On a LAN-sized deployment the page cache
  absorbs this; a larger one would want the read hoisted or cached.
- `max_rounds` on the client now bounds **rounds that achieve nothing**, not rounds. A
  window that lands is progress no matter how much is left, so the round budget no
  longer has to be larger than the card.

---

# Hardening (M5)

## A control message needs a fresh nonce every time

The server remembers recently seen nonces per device and drops a repeat. This applies to
**`HELLO`, `PULL_REQ`, `PUSH_BEGIN` and `PUSH_END` only**.

So a client must generate a new nonce for every control datagram it sends, **including
retransmissions**. Resending a `PUSH_END` verbatim after a lost ACK will be dropped as a
replay. The counter-plus-boot-entropy scheme suggested above satisfies this for free.

`PUSH_CHUNK` is deliberately **not** covered, for two reasons:

- Duplicate chunks are an explicitly supported operation (§4: "duplicates overwrite
  idempotently"). Rejecting a repeat would break the retransmission path the protocol
  depends on.
- A kernel client resending a gap may well resend the datagram it already built, byte for
  byte, rather than rebuilding it. Requiring a fresh nonce per chunk would make the cheap
  implementation the broken one.

Chunks are also the only high-volume message; tracking 2048 nonces per transfer to guard
an operation that is idempotent anyway would buy nothing.

## Rate limiting

A per-source token bucket, checked **before** the HMAC is verified — verifying is the
expensive part, and an unauthenticated flood should not get to spend it. The budget
(`SLOTSYNC_UDP_BURST`, `SLOTSYNC_UDP_RATE`) has to clear a whole card's burst: throttling
a real console is worse than the flood it would prevent.

Over-budget datagrams are dropped. A `NACK 0x0A` goes back at most once per second per
source, so an inbound flood cannot become an outbound one. Rejection logging is throttled
separately and at the same rate — sharing one budget between the two would let a burst of
malformed datagrams suppress the rate-limit NACK, leaving a throttled client with no idea
why it was being ignored.

## Caps

| Limit | Default | Setting |
|---|---|---|
| Datagram | 1120 bytes (96 header + 1024 payload) | fixed by the protocol |
| Card image | 16 MiB | `SLOTSYNC_MAX_CARD_BYTES` |
| Pushes in flight | 64 | `SLOTSYNC_MAX_STAGING` |
| Staging lifetime | 120 s without a chunk | `SLOTSYNC_STAGING_TTL` |
| Nonces remembered | 512 per device, 64 devices, 120 s | `SLOTSYNC_NONCE_TTL` |

An oversized `PUSH_BEGIN` is refused before a buffer is allocated, so `total_size` is
never a lever on server memory.
