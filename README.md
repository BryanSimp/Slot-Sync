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
| [`dolphin/`](dolphin/) | PC daemon: manages per-game cards, syncs over HTTP | M6 done |
| [`wii/`](wii/) | Homebrew launcher: pulls saves, chainloads Nintendont, pushes on exit | M7 — builds, needs `libfat-ogc` |

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
docker compose up -d --build
```

Saves live in the `slotsync-data` named volume, not in the checkout, so
redeploying the stack cannot take your version history with it.

### On Portainer, from the container registry

Pushing to `main` builds `server/Dockerfile` and publishes it to GitHub
Container Registry as **`ghcr.io/bryansimp/slot-sync:latest`**, for both
`linux/amd64` and `linux/arm64` — see
[`.github/workflows/publish.yml`](.github/workflows/publish.yml). The tests gate
the publish, so a red build never becomes an image.

The Portainer host then only needs to *pull*. It needs no build toolchain, no
checkout, and no access to the source.

**1. Add the registry.** Portainer → **Registries → Add registry → Custom**:

| Field | Value |
|---|---|
| Name | `ghcr` |
| Registry URL | `ghcr.io` |
| Authentication | on |
| Username | your GitHub username |
| Password | a token with **`read:packages`** scope |

That step is only needed while the package is private. A package inherits its
repository's visibility, so if you make it public — GitHub → your profile →
Packages → `slot-sync` → Package settings → Change visibility — Portainer can
pull it with no credentials at all.

**2. Deploy the stack.** Portainer → **Stacks → Add stack → Web editor**, and
paste [`docker-compose.ghcr.yml`](docker-compose.ghcr.yml). Add two stack
environment variables:

| Name | Value |
|---|---|
| `SLOTSYNC_TOKEN` | `openssl rand -hex 32` |
| `SLOTSYNC_PSK` | `openssl rand -hex 32` |

The container refuses to start if either is missing, which is deliberate — see
`server/src/slotsync/config.py`. Keep both: the Dolphin daemon needs the token
and the Wii client needs the PSK.

**If the stack fails with `port is already allocated`**, something on the host
already has that port — 8080 especially is contested on a home server. Add a
stack variable rather than editing the compose:

| Name | Example |
|---|---|
| `SLOTSYNC_HTTP_BIND` | `8081` |
| `SLOTSYNC_UDP_BIND` | `9977` |

Only the host side moves; the container still listens on 8080 and 9977
internally. If you move the UDP one, the port in the Wii's
`sd:/slotsync/slotsync.cfg` has to match.

### Behind Traefik

If you already run Traefik, [`docker-compose.traefik.yml`](docker-compose.traefik.yml)
routes the web UI through it and publishes **no HTTP host port at all**, which
sidesteps port crowding entirely. The console's UDP port is still published
directly.

That split is the one `PLAN.md` §2 describes: TLS in front of the HTTP API via a
reverse proxy is fine and expected, but the console path stays plain UDP with an
HMAC, because there is no usable TLS stack on the Wii's ARM co-processor. Traefik
can route UDP, but there is nothing to gain by putting it in that path.

Set these as stack variables to match your install:

| Name | Typical |
|---|---|
| `TRAEFIK_NETWORK` | `traefik`, `proxy`, or **`traefik_default`** |
| `TRAEFIK_ENTRYPOINT` | `web` (:80) or `websecure` (:443) |
| `SLOTSYNC_HOST` | `slotsync.yourdomain` |

`network ... declared as external, but could not be found` means
`TRAEFIK_NETWORK` does not match a real network. Compose prefixes networks with
the stack name, so a Traefik deployed as a stack called `traefik` has a network
called `traefik_default` — that catches most people. Ask Docker rather than
guessing:

```bash
docker inspect $(docker ps -qf name=traefik)     --format '{{range $k,$v := .NetworkSettings.Networks}}{{println $k}}{{end}}'
```

Whatever that prints is the value to use. Copying it off a service Traefik
already routes is just as reliable.

To update later, re-pull the image and redeploy the stack; the `slotsync-data`
volume carries the save history across.

### On Portainer, building from source instead

If you would rather Portainer build it: **Stacks → Add stack → Repository**,
compose path `docker-compose.yml`, and turn on Authentication with a token that
has `repo` scope while this repo is private. Same two environment variables.

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

## The whole cycle, end to end

This is the walkthrough from `PLAN.md` §10b M8, and every step below has been run.

**On the server** — `docker compose up -d`, with `SLOTSYNC_TOKEN` and `SLOTSYNC_PSK`
set in `.env`. Both clients need those two values and nothing else.

**On the PC**

```bash
python -m slotsync_dolphin setup --server http://your-nas:8080 --token "$SLOTSYNC_TOKEN"
python -m slotsync_dolphin play GALE01 --exec ~/games/windwaker.iso
```

`play` pulls the card, repoints Dolphin's `MemcardAPath` at it, launches, waits, and
pushes when you quit. A game the server has never seen gets a blank card formatted for
it first.

**On the console** — put `boot.dol` in `sd:/apps/slotsync/`, write
`sd:/slotsync/slotsync.cfg` with the server address and the PSK, and launch it from the
Homebrew Channel. It syncs every card in `/saves` and then chainloads Nintendont.

**What you get**

```
GALE01 slot A, head v3
  v3  parent=2    f9fd9037c6  console (UDP)
  v2  parent=1    87e6eaaf72  dolphin (HTTP)
  v1  parent=None a0411e0a19  formatted 16 Mbit
```

A save written on the PC arrives on the console byte-identical, and vice versa. The web
UI at `http://localhost:8080` shows that history with real save names parsed out of the
card, a download button per version, and a restore button.

**When both sides moved**, the second push is refused rather than merged: the console
prints the conflict on screen, `slotsync-dolphin` exits with status 3, and neither save
is touched. You pick in the web UI. That is the whole point of the project — see
[`PLAN.md`](PLAN.md) §7.

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
