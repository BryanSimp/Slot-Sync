# slotsync-dolphin

The PC side. Keeps a directory of per-game GameCube memory cards, syncs them
with a SlotSync server, and points Dolphin at the right one.

**No dependencies.** Stdlib Python 3.12+, on purpose — this runs on a gaming PC,
where "a script you can just run" beats one that needs a virtualenv first.

## Why it works this way

Nintendont writes `/saves/GAMEID.raw`, one card image per game. Dolphin uses one
shared card per region — a single 16 MiB file holding every game's saves at once.
Those are different units, so saves cannot move between console and PC by copying
whole cards.

Lifting an individual save out of the shared card would mean rewriting its
directory and BAT blocks, which both carry checksums and backup copies. That is
the byte-level surgery [`PLAN.md`](../PLAN.md) §5 forbids, and the reason §11
rejects `.gci` as an interchange format.

So this daemon makes the PC speak the console's unit instead: one card per game,
with `MemcardAPath` in `Dolphin.ini` repointed before each game runs. Sync stays
byte-exact raw-to-raw, and nothing ever edits the inside of a card.

## Setup

```bash
python -m slotsync_dolphin setup \
    --server http://your-nas:8080 \
    --token "$SLOTSYNC_TOKEN" \
    --device 0x5043000000000001
```

That writes `~/.slotsync/dolphin.json` so later commands need no flags. Cards
live in `~/.slotsync/cards/` unless you pass `--cards-dir`.

`--device` is any u64 you like, distinct per machine. It is what makes the web
UI say which box pushed a version; without it every push from a PC is
attributed to nobody while the consoles name themselves.

## Use

```bash
# the whole cycle: pull, point Dolphin at the card, launch, push on exit
python -m slotsync_dolphin play GALE01 --exec ~/games/windwaker.iso

# or do it by hand
python -m slotsync_dolphin use GALE01     # pull + repoint, then start Dolphin yourself
python -m slotsync_dolphin push GALE01    # when you are done

# what is where
python -m slotsync_dolphin status
python -m slotsync_dolphin list GALE01    # the saves on the card, as the server parses them

# push cards as they change, for people who launch Dolphin their own way
python -m slotsync_dolphin watch
```

`play` pushes in a `finally` block, so a crashed emulator still gets you the save
that did make it to disk.


## Pushing while you play

Double-click `slotsync-watch.cmd`, or run `watch`, and leave it going -- or put
a shortcut to it in `shell:startup` and forget about it. It syncs both ways:

- **Pushes** whenever a card stops changing, so an in-game save reaches the
  server without quitting Dolphin, the same way the Nintendont kernel client
  does it on the console.
- **Pulls** whatever the server has moved on, but only while Dolphin is closed.
  Dolphin holds the card in memory and writes it back out, so a card replaced
  underneath a running instance is undone at the next in-game save -- and the
  save that replaced it goes with it. Closed is also exactly when a pull is
  useful: the gap between finishing on a console and starting on the PC.

A card with unpushed local play is left alone rather than pulled over. That is
a conflict, and conflicts are a human's decision.

Verified end to end: a card changed underneath the watcher was pushed about ten
seconds later, and the version the server committed hashes identically to the
file on disk.

It never overwrites. If another device moved the card on while you were playing,
the push is refused as a conflict, logged, and your card is left alone -- settle
it in the web UI.

## Things worth knowing

**Close Dolphin before changing cards.** Dolphin rewrites `Dolphin.ini` when it
exits, using the settings it loaded at startup — so an edit made underneath a
running instance is silently reverted and you play against the wrong card. The
daemon refuses rather than let that happen. `--force` overrides it if you know
the running instance is a different one.

**`Dolphin.ini` is backed up once**, to `Dolphin.ini.slotsync-backup`, before the
first change. Edits are surgical: one line is rewritten and the rest of the file
comes back byte for byte, comments and spacing included.

**A slot switched off gets switched on.** `SlotA = 255` means nothing is plugged
in, and a memory card path is ignored in that state — the game would just see no
card. Pointing a slot at a card sets it to `8`.

**Conflicts are refused, never merged.** If another device pushed while you were
playing, `push` exits with status 3 and leaves your card on disk. Open the web UI
to compare the two versions and pick one. Nothing is overwritten either way.

**A game the server has never seen gets a blank card**, formatted server-side at
`--mbit` (16 by default, Memory Card 251). The card's serial is synthetic, which
is safe only because whole cards move together — a save is always read back
against the serial it was written under.

## Tests

```bash
cd dolphin && python -m pytest
```

The integration tests run the **real** server in a thread rather than mocking
it — that is the point of the monorepo, and it is how the header-casing bug in
the HTTP client got caught.
