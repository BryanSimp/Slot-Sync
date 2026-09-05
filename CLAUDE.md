# Working notes for coding sessions

`PLAN.md` is the spec. Read it before writing anything. `docs/PROTOCOL.md` is the wire
format and `docs/MEMCARD.md` the verified card layout. If you change a decision, update
the doc in the same commit as the code.

## Layout

Three parts in one repo: `server/` (the Docker hub), `dolphin/` (PC daemon), `wii/`
(libogc homebrew). `docs/` is shared. Run the server's tests from inside `server/`.

The wire protocol now has three implementations -- the server, `fake_console.py`, and
the Wii client. That is deliberate (an independent client catches wire-format bugs a
shared module would hide) and it is also the thing most likely to drift. A protocol
change means touching all three and `docs/PROTOCOL.md` in one commit.

## Rules specific to this project

- **The ingest path must stay implementable in C on an ARM kernel with no libc.**
  No TLS, no JSON, no allocation per message, no connection state. If a change makes
  the protocol harder for that client, it is the wrong change even if it is cleaner
  Python.
- **Never merge two memory cards at the byte level.** Directory and BAT blocks carry
  checksums with backup copies. Whole cards are atomic. See `PLAN.md` §5.
- **Never resolve a conflict automatically.** Reject with 409 and let a human choose.
  Silent overwrites destroying long save files is the failure this project exists to
  prevent.
- **Verify memcard offsets against Dolphin's `GCMemcard.cpp` and YAGCD.** The offsets
  sketched in `PLAN.md` are a map, not authority. Do not trust them from memory.
- **No dependencies beyond `requirements.txt` without a note in `PLAN.md` §12 saying
  why.** No ORM, no Redis, no frontend framework.

## Testing

Real Nintendont `.raw` files are the only fixtures that matter. Put them in
`server/tests/fixtures/`. If none are available yet, generate synthetic cards with
`server/scripts/make_fixture.py` and mark those tests as approximate.

`server/scripts/fake_console.py` stands in for the Wii client. It must be able to inject packet
loss and reordering, because the real client will experience both.

## Commit style

One milestone per branch. Small commits. Reference the milestone: `M2: parse directory
block and expose save names`.
