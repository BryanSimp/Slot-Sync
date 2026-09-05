# SlotSync Server — Build Plan

This file is the working spec for the **server component only**. Read it fully before
writing code. If something here conflicts with what you discover in the wild, update
this file in the same commit as the code change.

---

## 1. What this is

SlotSync syncs GameCube memory card saves between three places:

1. **Nintendont** on Wii / vWii (Wii U), which writes raw memory card images to `/saves/GAMEID.raw`
2. **Dolphin** on a PC, which reads the same raw memory card format
3. **This server**, the hub, running in Docker

The wider project has three clients. **This repo is the hub only.** Two clients live
elsewhere and are out of scope here, but their constraints shape every decision below:

- **Homebrew wrapper** (Wii, libogc): pulls saves at launch, chainloads Nintendont, pushes on exit
- **Dolphin daemon** (PC, any language): watches the memcard directory, pushes on change

There is a **phase 2** for the console client: moving the push logic from the libogc
wrapper into Nintendont's ARM kernel so saves sync mid-game. The server must be ready
for that client on day one. See §3.

---

## 2. Hard constraints (do not design these away)

These come from the eventual in-kernel ARM client. Everything on the ingest path must
be implementable by a C client running inside Nintendont's IOS-replacement kernel with
no libc, a few KB of stack, and hard real-time deadlines.

| Constraint | Reason |
|---|---|
| **No TLS on the ingest path** | No usable crypto stack on the Starlet ARM. Auth is a pre-shared key + HMAC-SHA256. Put TLS in front of the HTTP API via a reverse proxy if you want it; never require it. |
| **No JSON on the ingest path** | Fixed-size packed binary headers, big-endian. A JSON parser in a kernel loop is bloat. |
| **Stateless, idempotent messages** | The kernel cannot hold connection state across a loop that is also servicing disc reads and controller input. Every datagram must be independently processable. |
| **Byte-range addressed** | Transfers address `(offset, length)` into the card. Whole-card upload is just "offset 0, length = card size" split into chunks. Delta sync later is the same protocol with fewer chunks — not a new one. |
| **Chunks fit in one datagram under MTU** | Max payload 1024 bytes. Do not assume jumbo frames or IP fragmentation work on a 2006 WiFi stack. |
| **Server never pushes unsolicited** | Client always initiates. No callbacks, no long-polling to the console. |

---

## 3. Two ingest paths, one data model

| Path | Port | Used by | Format |
|---|---|---|---|
| **Binary UDP** | 9977/udp | Homebrew wrapper, later the Nintendont kernel | Packed binary, HMAC-SHA256, chunked |
| **HTTP** | 8080/tcp | Dolphin daemon, web UI, `curl` during development | JSON + `application/octet-stream` bodies, bearer token |

Both write through the **same** storage and versioning layer. A card pushed from a Wii
over UDP and one pushed from Dolphin over HTTP are indistinguishable once committed.
Build the storage layer first and put both transports on top of it.

---

## 4. Binary wire protocol v1

Big-endian throughout. Header is 96 bytes, then payload.

```
off  size  field
0    4     magic, ASCII "SLOT"
4    1     version = 1
5    1     msg_type
6    2     flags (reserved, send 0)
8    8     device_id, u64, stable per physical console
16   6     game_id, ASCII, e.g. "GALE01", space-padded
22   1     slot, 0 = A, 1 = B
23   1     reserved
24   4     card_version, u32
28   4     parent_version, u32
32   4     offset, u32, byte offset into the card image
36   4     length, u32, bytes in this payload (max 1024)
40   4     total_size, u32, full card size in bytes
44   4     sequence, u32, chunk index within this transfer
48   16    nonce, random per message
64   32    hmac_sha256
96   ...   payload
```

HMAC is computed over `header[0:96]` with the `hmac` field zeroed, concatenated with
the payload, keyed by the pre-shared key.

### Message types

| Value | Name | Direction | Notes |
|---|---|---|---|
| 0x01 | `HELLO` | C→S | Announces device, gets server time and protocol version |
| 0x02 | `PULL_REQ` | C→S | Requests latest card for `(game_id, slot)` |
| 0x03 | `PUSH_BEGIN` | C→S | Declares `total_size`, `parent_version`, expected chunk count |
| 0x04 | `PUSH_CHUNK` | C→S | One payload chunk |
| 0x05 | `PUSH_END` | C→S | Payload is the SHA-256 of the full card image |
| 0x06 | `ACK` | S→C | Echoes `sequence`. For `PUSH_END`, includes assigned `card_version` |
| 0x07 | `NACK` | S→C | Payload is a 1-byte error code plus a bitmap of missing chunk indices |
| 0x08 | `PULL_CHUNK` | S→C | One chunk of the requested card |
| 0x09 | `HEARTBEAT` | C→S | Keeps NAT open during long transfers; no side effects |

### Transfer semantics

- Server holds a **staging buffer** keyed by `(device_id, game_id, slot, card_version)`
- Chunks may arrive out of order and may be duplicated. Duplicates overwrite idempotently.
- Nothing is committed until `PUSH_END` arrives **and** the SHA-256 matches
- Staging buffers expire after 120 seconds of no chunks. Log and drop.
- On missing chunks at `PUSH_END`, reply `NACK` with the missing-index bitmap so the
  client retransmits only the gaps
- Cap: max 64 concurrent staging buffers, max `total_size` of 16 MiB. Reject beyond that.

### Settled while building M4

Full detail is in [`docs/PROTOCOL.md`](docs/PROTOCOL.md); the parts that change how you
read this section:

- On the push path, **`card_version` is a transfer id the client picks** and echoes
  through `PUSH_BEGIN`/`PUSH_CHUNK`/`PUSH_END`, because at `PUSH_BEGIN` it cannot know
  the version it will be assigned. The assigned version comes back in the `PUSH_END` ack.
- A repeated `PUSH_BEGIN` restarts the transfer rather than resuming it.
- **A missing-chunk bitmap does not always fit one datagram** — a 16 MiB card needs 2048
  bitmap bytes against a 1024-byte cap. Bitmaps are windowed, with the base chunk index
  in the header's `sequence` field, so the payload stays exactly what this section
  describes. Bit order is LSB-first.
- `PULL_REQ` uses `card_version` to select a version, 0 meaning head, so a console can
  restore an older save without the web UI.
- A conflict `NACK` carries the head in `card_version`, so the client can pull it and
  surface a choice.

### Measured, which answers one of §13's open questions

Pushing a 2 MiB card unpaced over **loopback with no injected loss** lost about a third
of its 2048 chunks and needed four retransmission rounds. The datagrams were overrunning
the server's socket receive buffer, not the network. Setting `SO_RCVBUF` to 4 MiB
(`SLOTSYNC_UDP_RCVBUF`) makes the same push complete in one round with zero loss;
client-side pacing does too, independently. Both are in place. That number is a floor for
what real 802.11g will need, not a substitute for measuring it.

---

## 5. GameCube memory card format

You need enough of this to validate uploads and to list saves in the web UI. **Do not
trust the offsets below from memory — verify them against Dolphin's
`Source/Core/Core/HW/GCMemcard/GCMemcard.cpp` and the YAGCD memory card chapter before
writing the parser.** Treat this section as a map, not as authority.

Structure, in 8192-byte blocks:

| Block | Contents |
|---|---|
| 0 | Header: serial, format timestamp, card size in Mbit, encoding, checksums |
| 1 | Directory: 127 entries of 0x40 bytes, plus checksum |
| 2 | Directory backup |
| 3 | Block allocation table (BAT) |
| 4 | BAT backup |
| 5+ | Save data blocks |

Valid card sizes, from GameCube hardware. **This table originally listed four sizes and
was wrong** — Dolphin accepts six, and rejecting a valid 1 MiB or 4 MiB card would mean
refusing to back up somebody's save:

| Mbit | Blocks (data) | Total size | Common name |
|---|---|---|---|
| 4 | 59 | 512 KiB | Memory Card 59 |
| 8 | 123 | 1 MiB | Memory Card 123 |
| 16 | 251 | 2 MiB | Memory Card 251 — **use this as the project default** |
| 32 | 507 | 4 MiB | Memory Card 507 |
| 64 | 1019 | 8 MiB | Memory Card 1019 |
| 128 | 2043 | 16 MiB | Memory Card 2043 |

Total file size is `(data_blocks + 5) * 8192`.

The verified byte-level layout now lives in [`docs/MEMCARD.md`](docs/MEMCARD.md), checked
against Dolphin's `GCMemcard.{h,cpp}` and YAGCD chapter 12 as this section asks. Two
findings worth carrying here:

- **Where the sources disagree, Dolphin wins.** YAGCD places the directory update counter
  and checksums at `0x0ffa`/`0x0ffc`/`0x0ffe`, which is impossible: 127 entries of `0x40`
  run to `0x1fc0`, so `0x0ffa` lands inside entry 63. Dolphin's `0x1ffa`/`0x1ffc`/`0x1ffe`
  is the only arithmetic that fills the block exactly. Dolphin's source separately notes
  YAGCD is wrong about the banner/icon flag byte.
- **The directory and BAT checksum ranges are mirror images**, which is exactly the detail
  that gets written backwards from memory. The directory keeps its checksums at the *end*
  of its block and covers everything before them (`[0x0000, 0x1FFC)`); the BAT keeps its
  checksums at the *start* and covers everything after them (`[0x0004, 0x2000)`).

### Rules that follow from this

- **Never merge two cards at the byte level.** The directory and BAT blocks carry
  checksums, and both have backup copies that must stay consistent. Merging produces a
  card that validates structurally and corrupts on first write. Whole cards are the
  atomic unit for conflict resolution, even though *transport* is chunked.
- **Sync at the `.raw` level, never via `.gci`.** GCI extraction round-trips can trip
  copy-protection flags on some games' saves. Raw-to-raw is byte-exact.
- A directory entry gives you the game code, maker code, internal filename, comment
  offset, and block count. That is what the web UI should display — real save names,
  not just filenames.

### Validation on ingest

Reject a card if: size is not one of the six valid totals; header checksum fails;
**both** directory copies fail checksum; **both** BAT copies fail checksum. Accept and
flag (do not reject) if exactly one copy of a pair is bad — that is a normal state the
console repairs on next boot.

**This is deliberately more permissive than Dolphin**, which counts corrupt blocks across
all four of dir[0], dir[1], bat[0], bat[1] and fails the card at two or more — so one bad
directory copy *plus* one bad BAT copy is a rejection there, even though a good copy of
each survives. Dolphin's own source carries a TODO questioning that. Keep the per-pair
rule: a hub that refuses an upload is a hub that loses the save, whereas storing a
questionable card costs nothing and leaves every earlier version intact. Surface the
warnings in the UI instead.

Parsed results are recomputed on demand rather than stored. Blobs are immutable, so the
answer cannot go stale, and a card is only parsed when someone actually looks at it.

---

## 6. Storage

Content-addressed blobs plus a SQLite index. Cards dedupe naturally: a card pushed
unchanged from two devices stores once.

```
/data/blobs/<sha256[0:2]>/<sha256>.raw
/data/slotsync.db
```

Never delete a version by default. A 2 MiB card × 50 games × 20 versions is 2 GiB.
Storage is cheap and "restore the version from Tuesday" will be the most-used feature
in practice. Provide pruning as an explicit admin action only.

### Schema sketch

```sql
CREATE TABLE devices (
  device_id   INTEGER PRIMARY KEY,   -- u64 from the wire protocol
  label       TEXT,
  kind        TEXT,                  -- 'wii' | 'vwii' | 'dolphin'
  first_seen  INTEGER NOT NULL,
  last_seen   INTEGER NOT NULL
);

CREATE TABLE cards (
  game_id     TEXT NOT NULL,
  slot        INTEGER NOT NULL,
  head        INTEGER NOT NULL,      -- current card_version
  PRIMARY KEY (game_id, slot)
);

CREATE TABLE versions (
  game_id     TEXT NOT NULL,
  slot        INTEGER NOT NULL,
  version     INTEGER NOT NULL,
  parent      INTEGER,
  sha256      TEXT NOT NULL,
  size        INTEGER NOT NULL,
  device_id   INTEGER,
  created_at  INTEGER NOT NULL,
  note        TEXT,
  PRIMARY KEY (game_id, slot, version)
);
```

**`device_id` is a u64 on the wire but SQLite's `INTEGER` is signed 64-bit**, so ids at
or above 2^63 do not survive a round trip as written. Store the two's-complement
reinterpretation and convert at the boundary (`db.u64_to_db` / `db.db_to_u64`). The
mapping is a bijection, so primary-key lookups and equality still behave.

### Version numbering

Versions start at **1**. `head = 0`, and equivalently no row in `cards`, means the
server has never seen this card. A first push therefore sends `parent_version = 0`.
Pushing with a non-zero parent for a card the server does not have is a conflict, not a
create: it means the client is out of step with the server.

Rollback is **append-only**. Making version *v* head again writes a *new* version whose
blob is *v*'s, rather than truncating history. That keeps a rollback undoable, which
matters when the rollback itself was the mistake.

---

## 7. Conflict model

Monotonic `version` counter per `(game_id, slot)`, with a recorded parent.

- Client pushes with `parent_version` = the version it last pulled
- If `parent_version == head`, accept. New version is `head + 1`. Fast-forward.
- If `parent_version < head`, **reject with a conflict**. Do not merge, do not
  last-write-wins silently. Return the current head so the client can surface a choice.
- If the pushed SHA-256 equals the head's SHA-256, accept as a no-op and return the
  existing version. Idempotent retries must be free.

Conflict resolution is a **human** decision made in the web UI: keep mine, keep theirs,
or fork. Losing 40 hours of a save file to a silent overwrite is the failure mode this
whole project exists to prevent — do not optimize it away for convenience.

---

## 8. HTTP API

Bearer token in `Authorization`. Token from env, not from a file in the repo.

```
GET    /healthz                                  → 200, no auth
GET    /api/cards                                → list of cards with head version, size, last device
GET    /api/cards/{game_id}/{slot}               → detail incl. parsed directory entries
GET    /api/cards/{game_id}/{slot}/versions      → version history
GET    /api/cards/{game_id}/{slot}/latest.raw    → octet-stream, the head image
GET    /api/cards/{game_id}/{slot}/{version}.raw → octet-stream, a specific version
POST   /api/cards/{game_id}/{slot}?parent={v}    → body is the raw image; returns new version or 409
POST   /api/cards/{game_id}/{slot}/rollback/{v}  → makes {v} the new head as a fresh version
GET    /api/devices                              → known devices
GET    /                                         → web UI
```

`409 Conflict` responses must include the current head version and its SHA-256.

### Settled while building M1

- **`parent` is required on push and has no default.** Defaulting it to head would turn
  every stale push into a silent overwrite, which is the failure mode in §7. A new card
  sends `parent=0`.
- Push also accepts optional `?device={u64}` and `?note={text}`. The Dolphin daemon uses
  `device` so the card list can name what last wrote each card; the schema in §6 needs
  it and no other HTTP route supplies it.
- Status codes: `201` new version, `200` idempotent no-op (identical image),
  `400` malformed game id / slot / parent, `401` bad token, `404` unknown card or
  version, `409` conflict, `413` over `SLOTSYNC_MAX_CARD_BYTES`.
- Downloads carry `ETag: "<sha256>"`, `X-SlotSync-Version`, and a
  `Content-Disposition` filename of `GAMEID.raw` — the name Nintendont expects, so a
  download drops straight into `/saves/`.
- `{slot}` accepts `0`/`1` and `A`/`B`. The wire protocol speaks 0/1; humans and the web
  UI speak A/B. Both normalise to the integer.

---

## 9. Web UI

Server-rendered, no build step. Plain HTML plus a little vanilla JS. Do not add a
frontend toolchain to this project.

Must show: every card, its games (parsed from the directory, with real save names and
block counts), version history with timestamps and originating device, a download
button per version, a rollback button, and a clear conflict banner when one exists.

### Settled while building M3

- **The browser signs in with the same shared token, carried in a cookie.** A `<a href>`
  download or a form POST cannot set an `Authorization` header, so the API accepts either
  a bearer header (machine clients) or the cookie the login form sets. Still one secret
  and no accounts — §11 holds. The cookie is `HttpOnly` and `SameSite=Lax`, which is what
  stops a cross-site POST from riding on it; there is no separate CSRF token.
- **Conflict resolution is "keep mine" or "keep theirs". Fork is not built.** Forking
  needs a second lineage per `(game_id, slot)`, and the schema in §6 has one `head` per
  card with no room for it. Add it only if the two-way choice proves insufficient in
  practice.
- "Keep mine" is an ordinary push re-aimed at the head from the 409 — the losing version
  stays in history and can be restored, so the choice is never destructive. The page
  keeps the selected file in JavaScript across the rejection rather than the server
  parking the upload, which keeps the conflict path free of new server state.
- Everything except the upload form works with JavaScript off: rollback is a form POST
  and downloads are links.
- Route names in `web.py` are prefixed `web_`. `url_for` resolves names across the whole
  app, and `api.py` already had a `card_detail`, so unprefixed names sent the UI's
  redirects into the JSON API.

---

## 10. Milestones

Work in this order. Each milestone ends with its tests passing and a commit.

**M0 — Scaffold**
Dockerfile, compose file, healthcheck, config from env, structured logging.
*Done when:* `docker compose up` serves `GET /healthz` → 200.

**M1 — Storage and HTTP push/pull**
Blob store, SQLite schema, version logic, the `.raw` upload and download endpoints.
*Done when:* a card can be pushed and pulled back byte-identical with `curl`, and a
stale-parent push returns 409.

**M2 — Memcard parser**
Header, directory, BAT parsing. Validation on ingest. Save listing.
*Done when:* parsing a real Nintendont `.raw` lists the correct save names and block
counts, and a truncated or corrupted card is rejected with a useful error.

**M3 — Web UI**
Card list, detail, history, download, rollback, conflict banner.
*Done when:* the whole push/conflict/rollback cycle is drivable from a browser.

**M4 — Binary UDP protocol**
Listener, HMAC verification, staging buffers, reassembly, chunked pull, NACK bitmaps.
*Done when:* `scripts/fake_console.py` completes a full push and pull round-trip, and
survives the packet loss and reordering the script can inject.

**M5 — Hardening**
Rate limiting, size caps, staging expiry, replay protection on the nonce, fuzz the
memcard parser against malformed input.
*Done when:* the fuzz run is clean and no malformed datagram can crash the listener.

Settled while building it, detail in [`docs/PROTOCOL.md`](docs/PROTOCOL.md):

- **Replay protection covers control messages only** — `HELLO`, `PULL_REQ`,
  `PUSH_BEGIN`, `PUSH_END`. `PUSH_CHUNK` is deliberately exempt: §4 makes duplicate
  chunks a supported operation, and a kernel client may resend a built datagram byte for
  byte rather than rebuilding it, so rejecting repeats would break the retransmission
  path. In exchange, a control message must carry a **fresh nonce on every send,
  retransmissions included**.
- The UDP rate limit is checked **before** the HMAC, because verifying is the expensive
  part. Its budget must clear a whole card's burst — throttling a real console is worse
  than the flood it would prevent.
- Rejection logging is throttled too. Attacker-controlled input driving unbounded
  logging fills a disk the flood itself never could. Logging and NACKing keep *separate*
  budgets: sharing one let a burst of malformed datagrams suppress the rate-limit NACK.
- HTTP rate limiting applies to **failed authentications only**, so the shared token is
  not brute-forceable and correct requests are never throttled.

**The fuzzer found a real bug**, which is the argument for having written it: a directory
entry may claim a `block_count` larger than the entire card, and the parser reported it —
a 512 KiB card could list a 491 MiB save, and that number reached the web UI. Blocks are
chained rather than contiguous, so `first_block + block_count` is not an extent and
cannot be validated as one; what must hold is that a save occupies no more blocks than
the card has. Now enforced, and the entry is skipped with a warning.

---

## 11. Non-goals

Say no to all of these. They are how this project dies.

- No user accounts, no multi-tenancy. Single household, shared token.
- No TLS termination in-process. Reverse proxy if wanted.
- No cloud hosting, no external services. Runs on a LAN box or a Pi.
- No game ROM or ISO storage. Saves only.
- No `.gci` as an interchange format. See §5.
- No automatic merge of conflicting cards. See §7.
- No frontend framework. See §9.

---

## 12. Stack

Python 3.12, FastAPI, uvicorn, SQLite via stdlib `sqlite3`, `hmac`/`hashlib` from
stdlib. UDP listener on `asyncio.DatagramProtocol` in the same process as the HTTP app.
No ORM. No Redis. No Postgres. Single container, two ports.

Chosen because the memcard parser and protocol work are the interesting parts and
Python keeps them readable — not because it is fast. It does not need to be fast; a
2 MiB card every few minutes is nothing.

---

## 13. Open questions

Resolve these as you go and record the answers here.

- [ ] Does Nintendont return control to the launching `.dol` on game exit, and does it
      preserve enough state to identify which game just ran? *This determines whether
      the wrapper syncs on exit or on next launch. Spike it before writing the client.*
- [x] Confirmed byte offsets for the memcard header fields, checked against Dolphin's
      source rather than recalled. **Done** — see [`docs/MEMCARD.md`](docs/MEMCARD.md).
      Two corrections came out of it: there are six valid card sizes, not four, and
      YAGCD's directory checksum offsets are wrong where Dolphin's are right.
- [ ] Does Nintendont's `GAMEID.raw` ever differ from a Dolphin-written raw of the same
      card size — padding, trailing bytes, header serial?
- [ ] Real observed throughput of a 2 MiB push over Wii 802.11g, to size the chunk
      timeout sensibly. *Partly answered on loopback — see §4. The finding that mattered
      was not throughput but buffering: an unpaced burst overruns the receive socket long
      before it troubles the network. Still needs a real console on real WiFi.*
