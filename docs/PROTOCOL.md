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
6    2     flags, bit 0 = DELTA (see below); send 0 on every other message
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
| 0x0A | PUSH_DELTA | C→S | bitmap of the chunks this push will send; base in `sequence` |

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
0x0C  delta unavailable: cannot seed from that parent
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

---

# Delta push (M11)

Every push above sends the whole card. On the kernel client that is 2048 datagrams for a
2 MiB card and 16384 for a 16 MiB one, at a paced 400 datagrams/sec — 5.1 s and 41 s of
transfer while a game is running. A save write touches its own data blocks plus one
directory block and one BAT block, so the bytes that actually changed are typically 2–5%
of the card.

A delta push sends only those chunks. Same protocol, fewer chunks — `PLAN.md` §2 said it
would be.

## Sequence

```
C→S  PUSH_BEGIN   flags=0x0001, total_size=2097152, parent_version=7
S→C  ACK                                   (or NACK 0x0C — fall back to a whole-card push)
C→S  PUSH_DELTA   sequence=0, payload = bitmap of the chunks that changed
S→C  ACK          sequence=0
C→S  PUSH_CHUNK   only the declared chunks
C→S  PUSH_END     payload = sha256 of the WHOLE card image, not of the delta
S→C  ACK          card_version=8
```

`PUSH_BEGIN` with **flags bit 0** asks the server to seed the staging buffer with the
bytes of `parent_version` instead of zeros. Everything the client does not send is
therefore the parent's bytes.

## Why this does not violate "never merge two cards"

`CLAUDE.md` forbids merging two memory cards at the byte level, and seeding a buffer from
one card and overwriting parts of it with another is exactly that shape. What makes it
legal is the digest:

**`PUSH_END` still carries the SHA-256 of the client's whole card image.** The client
computes it over its own complete 2 MiB, not over what it sent. If the server's copy of
`parent_version` differs from the client's copy in even one byte — a divergence the
client does not know about, a blob that rotted, a bug in either fingerprint — the
assembled image hashes wrong and is refused with `0x07`, exactly as a torn whole-card
push is today.

So the merge is *verified*, never assumed. The client is still committing to a specific
whole card; the wire is just not carrying the parts the server already has. Nothing about
the conflict rule changes: `parent_version` must still be head at commit time or the push
is refused with `0x05`, and a human still chooses.

## `PUSH_DELTA`, 0x0A

Payload is a bare bitmap — no leading error byte, the same shape a `PULL_REQ` payload
has — based at the header's `sequence`. It declares the chunks this transfer will send.

One datagram covers 8192 chunks, so cards up to 8 MiB need one and a 16 MiB card needs
two. Each is acked with the same `sequence`, so a lost declaration is retried rather than
discovered at `PUSH_END`. Without the ack, one lost datagram would make the server
believe the delta was empty, fail the digest, and cost a whole-card restart.

`PUSH_DELTA` is **not** nonce-guarded, for the same reason `PUSH_CHUNK` is not: declaring
a chunk twice sets a bit that was already set. A client may resend the datagram it
already built.

A repeated `PUSH_BEGIN` restarts the transfer and discards the declaration along with the
chunks.

## Gaps

`NACK 0x06` names only chunks the client declared. Everything else is the parent's bytes
and was never in flight, so it cannot be missing.

A client always holds the whole image, so it can satisfy any gap the server names,
including one it never declared. A delta push therefore degrades into a whole-card push
without any special handling.

## `NACK 0x0C`, delta unavailable

The server cannot seed from `parent_version`:

- it has no such version for this card,
- the blob has been pruned, or
- that version's card is a different size from `total_size`.

None of these are errors on the client's part, and none are conflicts. The client retries
immediately as a whole-card push. **A client that implements delta push must implement
this fallback**, or a pruned blob makes a card unpushable.

## What it costs the server

`PUSH_BEGIN` now reads the parent blob — up to 16 MiB off disk — before it acks, on a
thread so the loop keeps serving. Peak memory is unchanged: the staging buffer was always
`total_size` bytes, it is just no longer zeros.

## Measured

Chunks sent for one save, by the size of the save:

| Save | Blocks | Chunks sent | 2 MiB card | 16 MiB card |
|---|---|---|---|---|
| Sonic Adventure 2 | 2 | 32 | 1.6% | 0.2% |
| Wind Waker | 3 | 40 | 2.0% | 0.2% |
| Metroid Prime | 4 | 48 | 2.3% | 0.3% |
| Melee | 11 | 104 | 5.1% | 0.6% |
| Animal Crossing | 57 | 472 | 23% | 2.9% |

Data blocks plus one directory block and one BAT block, at `docs/MEMCARD.md`'s 8 KiB
block size. At the kernel client's 400 datagrams/sec that turns a 5.1 s push of a 2 MiB
card into 0.26 s, and a 41 s push of a 16 MiB card into the same 0.26 s — the cost stops
scaling with the card and starts scaling with the save.

## Delta pull is not implemented

The kernel client never pulls a card; it only asks `ss_head`, which is two datagrams. So
delta pull would only help the libogc launcher, and it is left undone.

The wire format already has room for it and no new message type is needed: `PULL_REQ`
already takes a bitmap of wanted chunks, and its `parent_version` field is unused. A
client naming the version it already holds there, with flags bit 0 set, would let the
server diff two blobs it has on disk and send a manifest plus only the differing chunks.
