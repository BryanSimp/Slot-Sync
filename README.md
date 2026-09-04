# SlotSync

Sync GameCube memory card saves between a Wii, a Wii U, and Dolphin.

Play Wind Waker on the couch, stop, pick it up on your PC. Play on the PC, pick it back
up on the console. Same save, no SD card shuffling, no manual file copying, full version
history so a bad overwrite is recoverable.

This repo is the **server** — a Docker container that stores every card, keeps version
history, parses the memory card format to show you what is actually on each card, and
serves it all over a small web UI.

## Project layout

SlotSync has three parts. They live in separate repos.

| Part | What it does | Status |
|---|---|---|
| **slotsync** (this repo) | Docker hub: storage, versioning, web UI, both ingest protocols | In progress |
| **slotsync-wii** | Homebrew launcher: pulls saves, chainloads Nintendont, pushes on exit | Not started |
| **slotsync-dolphin** | PC daemon: watches Dolphin's memcard directory, syncs on change | Not started |

Later, the push logic moves from the Wii launcher into a Nintendont fork so saves sync
*during* gameplay rather than only at exit. The server's binary protocol is designed for
that client from the start.

## Quick start

```bash
cp .env.example .env
# edit .env and set SLOTSYNC_TOKEN and SLOTSYNC_PSK to real values
docker compose up -d
```

Web UI at `http://localhost:8080`.

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
