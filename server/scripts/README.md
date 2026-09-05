# scripts

| Script | Purpose |
|---|---|
| `fake_console.py` | Stands in for the Wii client. Speaks the binary UDP protocol, pushes and pulls a card, and can inject packet loss and reordering. Also the reference for whoever writes the C client. |
| `make_fixture.py` | Generates synthetic but structurally valid memory card images for tests when a real Nintendont dump is not available. |

Both are deliberately **standalone**: neither imports `slotsync`. A generator that shared
constants with the parser it tests would agree with it even when both were wrong, and a
client that shared the server's protocol module would hide a wire-format bug from both
sides at once.

## fake_console.py

```bash
export SLOTSYNC_PSK=...           # or pass --psk

python scripts/fake_console.py hello
python scripts/fake_console.py push GALE01 A card.raw --parent 0
python scripts/fake_console.py pull GALE01 A out.raw

# the M4 done condition: survive a bad link
python scripts/fake_console.py --loss 0.2 --reorder --seed 7     push GALE01 A card.raw --parent 0
```

Link options (`--loss`, `--reorder`, `--seed`, `--pace`, `--timeout`, `--rounds`) are
global and go **before** the subcommand; `--parent`, `--transfer-id` and `--version`
belong to their subcommand.

`--parent` is the version the image was derived from, `0` for a card the server has never
seen. It is required, for the reason in `PLAN.md` §7.

## make_fixture.py

```bash
python scripts/make_fixture.py card.raw --mbit 16     --save "GALE01:zelda:The Legend of Zelda:Outset Island:11"     --save "GM4E01:sunshine:Super Mario Sunshine:Delfino Plaza:7"     --fragment
```

Save fields are `GAMEID:filename:title:subtitle:blocks`, colon-separated — so none of
them can contain a colon. Import `build_card` directly if a fixture needs
"Zelda: The Wind Waker" verbatim.

`--fragment` scatters each save's blocks so the BAT chain is non-contiguous. Real cards
fragment, and a parser that quietly assumes contiguous allocation passes every other
test.
