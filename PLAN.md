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

Valid card sizes, from GameCube hardware:

| Blocks (data) | Total size | Common name |
|---|---|---|
| 59 | 512 KiB | Memory Card 59 |
| 251 | 2 MiB | Memory Card 251 — **use this as the project default** |
| 1019 | 8 MiB | Memory Card 1019 |
| 2043 | 16 MiB | Memory Card 2043 |

Total file size is `(data_blocks + 5) * 8192`.

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

Reject a card if: size is not one of the four valid totals; header checksum fails;
**both** directory copies fail checksum; **both** BAT copies fail checksum. Accept and
flag (do not reject) if exactly one copy of a pair is bad — that is a normal state the
console repairs on next boot.

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
- [ ] Confirmed byte offsets for the memcard header fields, checked against Dolphin's
      source rather than recalled
- [ ] Does Nintendont's `GAMEID.raw` ever differ from a Dolphin-written raw of the same
      card size — padding, trailing bytes, header serial?
- [ ] Real observed throughput of a 2 MiB push over Wii 802.11g, to size the chunk
      timeout sensibly
