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
