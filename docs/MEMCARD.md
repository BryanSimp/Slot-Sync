# GameCube memory card format, as verified

`PLAN.md` §5 says its offsets are a map, not authority, and to check them against
Dolphin and YAGCD before writing a parser. This file is the result of doing that, so
the next session does not have to repeat it.

**Sources**

- Dolphin, `Source/Core/Core/HW/GCMemcard/GCMemcard.h` and `.cpp` (read at `master`,
  September 2026). Struct definitions carry `static_assert(sizeof(...) == BLOCK_SIZE)`,
  so the layouts below are self-checking.
- YAGCD chapter 12, *Memory card*.

**Where the two disagree, Dolphin wins.** Two concrete cases, both recorded below:
YAGCD places the directory update counter and checksums at `0x0ffa`/`0x0ffc`/`0x0ffe`,
which is arithmetically impossible, and Dolphin's own source notes YAGCD is wrong about
the banner/icon flag byte.

---

## Constants

| Name | Value |
|---|---|
| `BLOCK_SIZE` | `0x2000` (8192) |
| `MC_FST_BLOCKS` | 5 — header, dir, dir backup, BAT, BAT backup |
| `DIRLEN` | `0x7F` (127 directory entries) |
| `DENTRY_SIZE` | `0x40` (64) |
| `DENTRY_STRLEN` | `0x20` (32) |
| `BAT_SIZE` | `0xFFB` (4091 map entries) |
| `MBIT_TO_BLOCKS` | 16 — `(1024*1024) / (BLOCK_SIZE * 8)` |

Everything multi-byte is **big-endian**.

## Block order in the file

| Block | Contents |
|---|---|
| 0 | Header |
| 1 | Directory |
| 2 | Directory backup |
| 3 | Block allocation table |
| 4 | BAT backup |
| 5+ | Save data |

Confirmed by the read order in `GCMemcard::Open`.

## Valid card sizes

Dolphin accepts **six**, not the four listed in `PLAN.md` §5:

| Mbit | Total blocks | Data blocks | File size |
|---|---|---|---|
| 4 | 64 | 59 | 524288 (512 KiB) |
| 8 | 128 | 123 | 1048576 (1 MiB) |
| 16 | 256 | 251 | 2097152 (2 MiB) |
| 32 | 512 | 507 | 4194304 (4 MiB) |
| 64 | 1024 | 1019 | 8388608 (8 MiB) |
| 128 | 2048 | 2043 | 16777216 (16 MiB) |

`BytesToMegabits` rejects any size that is not an exact multiple, so only these pass.

## Header block (block 0, `0x2000` bytes)

| Offset | Size | Field |
|---|---|---|
| `0x0000` | 12 | serial |
| `0x000C` | 8 | format time, OSTime |
| `0x0014` | 4 | SRAM bias at format |
| `0x0018` | 4 | SRAM language |
| `0x001C` | 4 | VI DTV status |
| `0x0020` | 2 | device id — 0 formatted in slot A, 1 in slot B |
| `0x0022` | 2 | size in Mbit |
| `0x0024` | 2 | encoding — 0 CP1252, 1 Shift-JIS |
| `0x0026` | 468 | unused, `0xFF` |
| `0x01FA` | 2 | update counter |
| `0x01FC` | 2 | checksum |
| `0x01FE` | 2 | inverse checksum |
| `0x0200` | 7680 | unused, `0xFF` |

Checksum covers **`[0x0000, 0x01FC)`** — 508 bytes.

## Directory block (blocks 1 and 2)

| Offset | Size | Field |
|---|---|---|
| `0x0000` | 8128 | 127 × `DEntry`, `0x40` each |
| `0x1FC0` | 58 | padding, `0xFF` |
| `0x1FFA` | 2 | update counter, **signed** s16 |
| `0x1FFC` | 2 | checksum |
| `0x1FFE` | 2 | inverse checksum |

Checksum covers **`[0x0000, 0x1FFC)`** — 8188 bytes.

The arithmetic is forced: `127 * 0x40 = 0x1FC0`, `+ 0x3A padding = 0x1FFA`, `+2 = 0x1FFC`,
`+2 = 0x1FFE`, ending exactly at `0x2000`. YAGCD's `0x0ffa` would land inside directory
entry 63, so it cannot be right.

### Directory entry (`0x40` bytes)

| Offset | Size | Field |
|---|---|---|
| `0x00` | 4 | game code, ASCII — `FF FF FF FF` means the slot is unused |
| `0x04` | 2 | maker code, ASCII |
| `0x06` | 1 | unused, `0xFF` |
| `0x07` | 1 | banner format (bits 0-1), icon animation order (bit 2) |
| `0x08` | 32 | filename, NUL-padded |
| `0x28` | 4 | modification time, seconds since 2000-01-01 |
| `0x2C` | 4 | banner/icon image offset |
| `0x30` | 2 | icon format, 2 bits per icon |
| `0x32` | 2 | animation speed, 2 bits per icon |
| `0x34` | 1 | permissions — bit 2 public, bit 3 no-copy, bit 4 no-move |
| `0x35` | 1 | copy counter |
| `0x36` | 2 | first block, absolute block number, ≥ 5 |
| `0x38` | 2 | block count |
| `0x3A` | 2 | unused, `0xFFFF` |
| `0x3C` | 4 | comments address, byte offset **into the save's own data** |

Dolphin's comment on `0x07`: "YAGCD is wrong about the meaning of these. '0' and '3'
both mean no banner. '1' means paletted, '2' means direct colour."

## BAT block (blocks 3 and 4)

| Offset | Size | Field |
|---|---|---|
| `0x0000` | 2 | checksum |
| `0x0002` | 2 | inverse checksum |
| `0x0004` | 2 | update counter, **signed** s16 |
| `0x0006` | 2 | free blocks |
| `0x0008` | 2 | last allocated block |
| `0x000A` | 8184 | map, 4091 × u16 |

Checksum covers **`[0x0004, 0x2000)`** — 8188 bytes.

Note the asymmetry, which is easy to get backwards: the **directory** keeps its
checksums at the *end* and covers everything before them; the **BAT** keeps its
checksums at the *start* and covers everything after them.

### Chain walking

`m_map` is indexed by `block - MC_FST_BLOCKS`, i.e. the entry for absolute block 5 is
`m_map[0]`. Values:

- `0x0000` — block is free
- `0xFFFF` — end of chain
- anything else — the next absolute block number in the chain

A save's logical data is its chain of blocks concatenated in order. `comments_address`
is an offset into that logical stream, not into the raw file.

## Checksum algorithm

```c
u16 csum = 0, inv_csum = 0;
for (size_t i = 0; i < size; i += 2) {
  const u16 d = read_big_endian_u16(&data[i]);
  csum     += d;
  inv_csum += (u16)(d ^ 0xffff);
}
if (csum == 0xffff) csum = 0;
if (inv_csum == 0xffff) inv_csum = 0;
```

Read as big-endian u16 words, sum mod 2^16, and store big-endian. The `0xFFFF -> 0`
normalisation applies to both values, and is byte-order agnostic because `0xFFFF` is a
palindrome. Dolphin byte-swaps the accumulator before storing it into a natively-ordered
struct field, which comes to the same thing as storing the sum big-endian.

## Which copy is live

```c
active_directory = dir[0].update_counter >= dir[1].update_counter ? 0 : 1;
active_bat       = bat[0].update_counter >= bat[1].update_counter ? 0 : 1;
```

Compared as **signed** values, ties going to copy 0. Dolphin notes the GC BIOS does the
same and does not guard against overflow.

## Dolphin's validity rule, and where we differ

Dolphin counts corrupt blocks across all four of dir[0], dir[1], bat[0], bat[1] and
fails the card if **two or more** are bad — so one bad directory copy *plus* one bad BAT
copy is a rejection, even though a good copy of each still exists. Its own source
carries a TODO questioning this.

SlotSync deliberately does not copy that rule. See `PLAN.md` §5 for the reasoning: a hub
that refuses an upload is a hub that loses the save, whereas storing a questionable card
costs nothing and keeps every earlier version intact.
