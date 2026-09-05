# SlotSync

Sync GameCube memory card saves between a Wii, a Wii U, and Dolphin.

Play Wind Waker on the couch, stop, pick it up on your PC. Play on the PC, pick it back
up on the console. Same save, no SD card shuffling, no manual file copying, full version
history so a bad overwrite is recoverable.

This repo is the **server** — a Docker container that stores every card, keeps version
history, parses the memory card format to show you what is actually on each card, and
serves it all over a small web UI.

## Project layout

Three parts, one repo, a directory each.

| Directory | What it does | Status |
|---|---|---|
| [`server/`](server/) | Docker hub: storage, versioning, web UI, both ingest protocols | M0–M5 done |
| [`dolphin/`](dolphin/) | PC daemon: manages per-game cards, syncs over HTTP | In progress |
| [`wii/`](wii/) | Homebrew launcher: pulls saves, chainloads Nintendont, pushes on exit | In progress |

`docs/` is shared: [`PROTOCOL.md`](docs/PROTOCOL.md) is the wire format all three speak,
[`MEMCARD.md`](docs/MEMCARD.md) the card layout.

Later, the push logic moves from the Wii launcher into a Nintendont fork so saves sync
*during* gameplay rather than only at exit. The server's binary protocol is designed for
that client from the start.

## The one thing that surprised us

Nintendont and Dolphin disagree about what a memory card *is*.

- Nintendont writes `/saves/GAMEID.raw` — one card image **per game**.
- Dolphin uses one shared card per region, holding **every** game's saves at once.

So saves cannot move between them by copying whole cards, and pulling a single save out
of a shared card is exactly the byte-level surgery that corrupts cards (see
[`PLAN.md`](PLAN.md) §5). The way out is to make the PC side use per-game cards too:
`dolphin/` keeps a directory of them and repoints Dolphin's `MemcardAPath` before a game
runs. Both sides then speak the same unit and sync stays byte-exact.

## Quick start

```bash
cp .env.example .env
# edit .env and set SLOTSYNC_TOKEN and SLOTSYNC_PSK to real values
docker compose up -d
```

Web UI at `http://localhost:8080`. Sign in with the value of `SLOTSYNC_TOKEN`.

## What works

All five milestones in [`PLAN.md`](PLAN.md) §10 are built and tested.

| | |
|---|---|
| **Storage** | Content-addressed blobs plus a SQLite index. Identical cards from two devices store once. Versions are never deleted. |
| **Conflicts** | A push whose parent is not head is rejected with a 409 carrying the current head. Never merged, never last-write-wins. |
| **Memory cards** | Header, directory and BAT parsing, verified against Dolphin's source — see [`docs/MEMCARD.md`](docs/MEMCARD.md). Real save names in the UI, and validation on ingest. |
| **Web UI** | Card list, per-card detail with parsed saves, version history, download and restore per version, and a conflict banner. Server-rendered, no build step. |
| **Console protocol** | Binary UDP with HMAC-SHA256, chunked transfer, out-of-order and duplicate tolerance, NACK bitmaps for gaps, and chunked pull. |
| **Hardening** | Rate limiting, replay rejection on the nonce, staging expiry, size caps, and a fuzzed parser. |

Two things to know:

- The container build is **unverified** — it was written and reviewed but never
  built, because no Docker daemon was available. Everything else was exercised
  against a running server.
- Test fixtures are synthetic, from `scripts/make_fixture.py`. A real Nintendont
  `.raw` in `tests/fixtures/` is still what would settle the parser.

## Trying it without Docker

```bash
python -m venv .venv && .venv/bin/pip install \n    -r server/requirements.txt -r server/requirements-dev.txt
export SLOTSYNC_TOKEN=dev-token SLOTSYNC_PSK=dev-psk SLOTSYNC_DATA=./data
PYTHONPATH=server/src .venv/bin/python -m slotsync
```

Then, from another shell:

```bash
python server/scripts/make_fixture.py card.raw --mbit 16     --save "GALE01:zelda:The Legend of Zelda:Outset Island:11"

curl -X POST -H "Authorization: Bearer dev-token"     --data-binary @card.raw "http://localhost:8080/api/cards/GALE01/A?parent=0"

SLOTSYNC_PSK=dev-psk python server/scripts/fake_console.py pull GALE01 A pulled.raw
```

`scripts/fake_console.py` stands in for the Wii client and can inject packet loss
and reordering; see [`server/scripts/README.md`](server/scripts/README.md).

## Building it

Read [`PLAN.md`](PLAN.md). It is the spec, the milestone list, and the record of
decisions. It is written to be handed to a coding session as-is.

Protocol details are in [`docs/PROTOCOL.md`](docs/PROTOCOL.md), and the verified
GameCube memory card layout is in [`docs/MEMCARD.md`](docs/MEMCARD.md).

## Why "raw only"

Nintendont writes `/saves/GAMEID.raw`, and that file *is* a raw GameCube memory card
image — the same format Dolphin reads. There is no conversion step between the two
platforms, which is the quiet reason this project is tractable at all. SlotSync moves
`.raw` files and never converts to `.gci`, because GCI round-trips can trip
copy-protection flags on some games' saves.

## License

GPL-2.0, matching Nintendont and Dolphin, since the client work derives from both.
