# Runtime card sync inside Nintendont

`PLAN.md` §1 calls this **phase 2**: moving the push out of the libogc wrapper and into
Nintendont's ARM kernel, so a save reaches the server *while the game is running* rather
than when you quit it.

This directory is that, as a patch against [Nintendont][nd]. It is not a fork — the hooks
into Nintendont's own files come to **61 lines across six files**, and everything else is
new sources that get copied in.

[nd]: https://github.com/FIX94/Nintendont

```
apply.sh                    graft it onto a Nintendont checkout
kernel/SlotSync.c           the engine: worker thread, when to push, lineage
kernel/SlotSyncNet.c        UDP straight onto IOS /dev/net/ip/top
kernel/SlotSyncLogic.c      every decision, with no kernel dependency, so it can be tested
patches/                    the 61 lines of hooks, and the commit they were cut against
tests/                      host build of SlotSyncLogic.c plus a live round trip
```

## Why this turned out to be tractable

The expectation going in was that this would be hard. Three things in Nintendont make it
much less so, and they are worth stating because they are what the design rests on.

**The card is already in ARM RAM.** `kernel/GCNCard.c` loads the whole `.raw` to
`0x11000000` at boot and serves every EXI read and write out of it. There is nothing to
read back off the SD card and nothing to poll — the bytes to push are sitting in a
contiguous buffer the kernel can already address.

**Nintendont already knows the moment a card changes.** `GCNCard_Write` maintains a dirty
range, the main loop calls `GCNCard_CheckChanges()` every iteration, and three seconds
later it writes the card out. That is exactly the trigger a sync needs, already debounced.

**The kernel already has a socket driver open.** `SOCKInit()` at `kernel/main.c:260` opens
`/dev/net/ip/top` on *every* boot, not just when a game wants the BBA emulation. That file,
`kernel/sock.c`, exists to forward a game's socket calls to IOS — but it is also a complete,
working, in-repo reference for what IOS expects from every ioctl we need. Every encoding in
`SlotSyncNet.c` is read off it rather than recalled.

So the work is not "get networking into the kernel". It is "open a second socket on a
driver that is already there, and pick the right moment to use it".

## How it behaves

1. `SlotSync_Init()` runs once at kernel start, after `EXIInit` (which loads the cards) and
   after `SOCKInit`. It reads `/slotsync/slotsync.cfg`, works out each card's six-character
   ID and what version it descends from, and starts a worker thread. **With no config file
   it does nothing at all** and Nintendont behaves exactly as upstream.

   The ID is worth a note. Nintendont only carries four characters — `ncfg->GameID` is a
   `u32`, which is why the file it writes is `/saves/GALE.raw` and not `GALE01.raw`. The
   server keys cards by six. The other two are the maker code, and every directory entry on
   the card starts with the game code followed by it (`docs/MEMCARD.md`), so we read them
   from there. A card with **no saves on it yet** therefore cannot be identified, and is
   skipped rather than pushed under a padded ID — a padded ID would key a different card on
   the server than the Dolphin daemon uses for the same game, and split the lineage in two.
2. The interface comes up in `SlotSync_Init`, and only the interface: link status through
   NCD, then NWC24 startup retried up to 32 times on `-29`, then `SO_STARTUP` (`0x1F`), then
   poll `SO_GETHOSTID` (`0x10`) until it answers a plausible address. That is libogc's
   `net_init_chain`, and it is the sequence that works — the one described here before was
   assembled from guesses and never brought an interface up at all. Read `SO_GETHOSTID`'s
   result **unsigned**: `192.168.x.x` is `0xC0A8…`, which is negative as an `int`.

   It takes 1.5 s on a Wii and 2.5 s on a Wii U, and `SlotSync_Init` gives it six seconds
   before letting the game boot. A console with no network therefore costs six seconds, not
   the configured twenty — the worker retries with the real timeout once the game is running.

   The socket and the HELLO are the worker's job, not init's. Doing them at init meant a
   console with an unreachable server sat through the interface timeout plus twelve HELLO
   rounds before the disc was allowed to start, which looks exactly like a hang.
3. `GCNCard_Save()` calls `SlotSync_NotifyCardSaved(slot)` once the card has reached the SD
   card. That records a timestamp and returns; it never blocks the main loop.
4. When a card has been **quiet for 4 s** and the last push was **at least 30 s ago**, the
   worker pushes the whole card. Both numbers are configurable.
5. On exit, `SlotSync_Shutdown()` gives an in-flight transfer up to 3 s to land, then
   writes `/slotsync/runtime.txt` so the launcher knows where the lineage got to.

Everything that can block is on the worker thread, at priority `0x50` — below Nintendont's
DI (`0x78`), HID (`0x78`) and socket (`0x78`) threads, deliberately.

### Conflicts are still never resolved here

`PLAN.md` §7 and `CLAUDE.md` both say a conflict is a human's decision. So:

- A push names the parent version the card's bytes actually descend from, read from the
  launcher's `state.txt`.
- If the server answers `409` (`SS_ERR_CONFLICT`), the card is **halted for the rest of the
  session** and the reason is logged. It is never retried with the server's head as the
  parent, which would be exactly the silent overwrite this project exists to prevent.
- If there is no recorded lineage at all, the worker asks the server once. Only if the
  server has *never seen the card* does it start at v0 — that overwrites nothing. Any other
  answer halts the card.

The save on the SD card is untouched in every one of those paths.

## Building

You need devkitARM (and devkitPPC, for the rest of Nintendont).

```sh
git clone https://github.com/FIX94/Nintendont
./apply.sh /path/to/Nintendont
cd /path/to/Nintendont && ./Build.sh
```

`apply.sh` is re-runnable and refuses to patch twice. The patch was cut against Nintendont
`0f69235`; against a newer tree the hooks are small enough to apply by hand if `patch`
complains.

## Configuration

The kernel reads the **same** `/slotsync/slotsync.cfg` the libogc wrapper does, and ignores
the keys that are not its business. `server`, `port` and `psk` are required. The runtime
keys, with their defaults:

| Key | Default | What it does |
|---|---|---|
| `runtime_sync` | `1` | `0` compiles in but never syncs |
| `runtime_quiet_ms` | `4000` | how long a card must be untouched before a push |
| `runtime_cooldown_ms` | `30000` | minimum gap between two pushes of one card |
| `runtime_net_timeout_ms` | `20000` | how long to wait for DHCP |
| `runtime_pace_every` | `64` | datagrams sent between pauses |
| `runtime_pace_us` | `160000` | how long to pause for |

**The pacing is about IOS, not about the server.** It was first set from the server's
`SLOTSYNC_UDP_RATE`, which was the wrong constraint: the server was never the bottleneck.
Unpaced, IOS's send path refuses roughly three quarters of a card's datagrams, and every
refusal costs a retransmission round. The shipped 400/sec is the rate at which a 2 MiB card
was measured landing with *zero* sends refused.

**Turn Nintendont's BBA emulation off.** Every hardware run of this has had it off. With it
on, `SOCKUpdateRegisters` drives IOS's socket layer from the main loop on the game's behalf
while this client is driving it too. That contention is why the client now opens its own
`/dev/net/ip/top` rather than borrowing `sock.c`'s handle — which is reason to think the two
can coexist, but nobody has actually run it that way. The network profile does not matter:
"Auto" stores 0 and skips Nintendont's own NCD setup, and bring-up here does its own.

## What is verified, and what is not

The section that used to be here was written before any of this had run on a console, and
almost every open question in it was answered the hard way. Replaced with what actually
happened.

**Verified on hardware**, across a Wii and a Wii U's vWii:

- **Runtime sync works.** Consecutive in-game pushes with correct lineage — v28 → v29 →
  v30 → v31 in one session, each attributed to the kernel's own device id in the server's
  device list, with the game still running.
- **The transfer is byte-perfect end to end.** The `.raw` on the SD card hashes to exactly
  the `sha256` the server computed from what arrived over the wire.
- **The pacing holds.** A 2 MiB push at the shipped 400/sec lands with `0 sends refused`.
- **A conflict is refused on hardware, not just in tests.** A console whose card descended
  from v26 met a head of v29, was halted for the session, and said so. Nothing was
  overwritten on either side.
- **Bring-up works on both consoles** — 1.5 s to a DHCP address on the Wii, 2.5 s on the
  Wii U. vWii was expected to be the hard case and was not.
- `tests/` — 50 host checks, plus a live round trip against a real server.

**What it cost to get there**, because the list is the useful part:

`IPPROTO_UDP` where IOS wants `IPPROTO_IP`; a missing `net_init`; a sockaddr whose length
byte was never set; a chainloader that overwrote itself; missing Homebrew Channel
privileges; four-character card names against six-character server keys; `SO_STARTUP`
confused with `SO_GETHOSTID`; `SO_GETINTERFACEOPT` option `0x1003`, which answers
successfully and always zero, instead of `0x4003`; `POLLIN` as `0x0001` instead of `0x0003`;
cache maintenance through the raw `sync_*` calls, which need 32-byte granularity and
silently did nothing on a 12-byte pollsd; a shared `device_id` between the launcher and the
kernel, which got the kernel's HELLO dropped as a replay; a `udelay` retry loop that built
an IOS message queue per call; and a refused send treated as fatal.

None of those failed loudly. Most looked exactly like "the server is unreachable".

**Not verified, and it would need someone to go and try it:**

- **BBA emulation on.** Every run has had it off. See the note in Configuration.
- **The mid-game crash is mitigated, not proven fixed.** Walking into a building while a
  push was in flight crashed the console twice. The `udelay` retry loop went, the log flush
  was gated on the worker being idle, and the transfer rate came down 4×. Several sessions
  since have pushed saves mid-game without recurrence — which is evidence, not proof.
- **Cards larger than 2 MiB in flight.** A 16 MiB card is 16384 chunks, which is exactly
  the chunk bitmap's limit, with no headroom.
- **Delta push.** Still a whole card every time; see the last section.

## Testing it

```sh
cd nintendont/tests && make check                    # unit, no server needed
make && ./test_runtime --server 192.168.1.10 9977 --psk yourkey --card fixture.raw
```

On hardware, read `/slotsync/runtime.log` on the SD card. Everything logged is held in a
fixed 8 KB buffer in RAM and written from the main thread — FatFs is built `_FS_REENTRANT 0`
and the DI thread is streaming the game off the same card, so the worker touching it wedges
the console. It is flushed every three seconds and again at exit, so a console you reset
still leaves a log behind.

Two levels of socket trace, both in `SlotSyncNet.c`. `DEBUG_SLOTSYNC` is the setup and error
path and is **on**: with it off, a bring-up that fails does so silently and is
indistinguishable from a sync with nothing to do. `DEBUG_SLOTSYNC_IO` is per datagram, and
is **off** — it is what found the from-address and cache-line bugs, but a push is two
thousand datagrams and it scrolls everything useful out of an 8 KB buffer.

The first thing to check is `SlotSync: server reachable, runtime sync armed`, which means
the hard part works. Then save in-game and watch for a new version in the web UI without
quitting.

## Known limitation: every push is a whole card

The protocol is byte-range addressed and `PLAN.md` §2 anticipates delta sync — but the
server currently starts every staging buffer as `bytearray(total_size)`, all zeros
(`server/src/slotsync/udp.py`, `_on_push_begin`). A partial push would therefore commit a
card that is mostly zeros, so this client sends all of it, every time.

That is why the cooldown defaults to 30 s. Nintendont tracks the dirty byte range in
`GCNCard_ctx` already, so the console side of a delta push is nearly free; what it needs is
for the server to seed a staging buffer from the parent version's bytes. That is a protocol
change and would have to land in the server, `fake_console.py`, `wii/core` and
`docs/PROTOCOL.md` together, so it is deliberately not done here. For a 2 MiB card it would
turn 1.6 s of radio time into well under a tenth of that.
