# slotsync-wii

Homebrew launcher for Wii and vWii. Syncs `/saves` with a SlotSync server over
the binary UDP protocol, then chainloads Nintendont.

## How it is put together

Two layers, and the split is the point:

| | |
|---|---|
| [`core/`](core/) | The protocol: SHA-256, HMAC, the 96-byte header, push/pull with retransmission. **No libogc, no libc beyond `<string.h>`, no allocation, no globals.** |
| [`source/`](source/) | The console: libogc networking, SD card, Nintendont chainload, the on-screen log. |

`core/` reaches the network through two function pointers, so the same code runs
on libogc's `net_*` API on the console and on BSD sockets in the host tests.
That means **the protocol implementation can be tested against a real server
without a Wii** — see below — and it is also the piece that moves into
Nintendont's ARM kernel for phase 2, which is why it obeys `PLAN.md` §2's
constraints already.

The protocol is written from [`docs/PROTOCOL.md`](../docs/PROTOCOL.md) rather
than translated from the server's Python, for the same reason
`server/scripts/fake_console.py` is standalone: two implementations that agree
are evidence, one that agrees with itself is not.

## Building

Needs devkitPPC and libogc. **libfat is also required** and is not part of
libogc:

```bash
dkp-pacman -S libfat-ogc
```

Then:

```bash
make          # -> boot.dol
make check    # build and run the core's tests on this machine, no console needed
```

Copy `boot.dol` to `sd:/apps/slotsync/boot.dol` alongside a `meta.xml`.

## Configuration

`sd:/slotsync/slotsync.cfg`, plain `key = value`:

```ini
server     = 192.168.1.10
port       = 9977
psk        = the-same-value-as-SLOTSYNC_PSK
device_id  = 0x5749490000000001
saves_dir  = sd:/saves
nintendont = sd:/apps/Nintendont/boot.dol
autoboot   = 1
```

`server` must be a dotted quad. There is no DNS on this path, deliberately —
`PLAN.md` §2 keeps the ingest path implementable by a kernel client with no
resolver.

## What it does at launch

1. Push every `GAMEID.raw` in `/saves` that differs from what it last agreed
   with the server.
2. Pull every card the server has moved ahead on.
3. Chainload Nintendont.
4. If control ever comes back, do 1 and 2 again.

Syncing *before* the handover as well as after is what makes `PLAN.md` §13's
first open question — does Nintendont return control to the launching `.dol`? —
stop being a blocker. If it never returns, this is a sync-on-next-launch client
and still correct. If it does, the save reaches the server immediately.

It also means no game list is needed. Nintendont keeps one card image per game,
which is exactly the unit the server versions, so the whole `/saves` directory
is handled without knowing which game is about to run. (The PC side has no such
luck — see [`../dolphin/README.md`](../dolphin/README.md).)

## Conflicts

If another device pushed while you were playing, the push is refused and the
screen says so. Nothing is overwritten and your save stays on the SD card; you
choose in the web UI. `PLAN.md` §7 — never merged, never last-write-wins.

## Testing without a console

```bash
make check                                    # vectors and wire format only

# the real thing: the C the console runs, against the server it will talk to
cd tests && make && ./test_core \
    --server 192.168.1.10 9977 --psk your-psk --loss 20
```

The unit half checks SHA-256 against FIPS 180-4, HMAC against RFC 4231, and
every header offset against `docs/PROTOCOL.md`. The live half pushes a 2 MiB
card, pulls it back, compares it byte for byte, and confirms a stale parent is
refused — with packet loss injectable in both directions.

## State

`sd:/slotsync/state.txt` records, per card, the version and digest last agreed
with the server. That is what lets a push name its parent, which is the whole
basis of the conflict model. Delete it and every card looks new, which will
produce conflicts rather than data loss.
