/* Per-block fingerprints, and the file that keeps them across a reboot.
 *
 * A delta push sends only the chunks that changed since the version the server
 * holds (docs/PROTOCOL.md, "Delta push"). Working out which those are needs a
 * description of the parent's bytes, and the two obvious ways to get one are
 * both wrong on a console: keeping a second copy of the card costs another
 * 2 MiB of RAM, and Nintendont's own dirty range is a *span*, so one touch at
 * each end of the card dirties everything between.
 *
 * So: one 32-bit fingerprint per 8 KiB block. Four bytes per block, computed in
 * a pass over a card that is already being hashed for PUSH_END anyway.
 *
 * **Block granularity, not chunk.** A GameCube card is erased and written in
 * 8 KiB blocks, so a game cannot change less than one; a finer table would
 * never resolve anything a coarser one misses and would cost eight times the
 * memory to prove it. One dirty block is eight dirty chunks.
 *
 * ## Why the table is on disk
 *
 * Held only in RAM, a table is empty at every start -- so the first push of
 * each game session sends a whole card, and the libogc launcher, which is a
 * fresh process every time, can never delta at all. Persisting it to the SD
 * card is what makes every push after the very first one a delta, in the kernel
 * and in the launcher, across reboots.
 *
 * Both write the **same** file, for the same reason they read the same
 * slotsync.cfg: the launcher's push and the kernel's push are moves in one
 * card's lineage, and a table written by either is the one the other wants
 * next.
 *
 * ## What makes it safe to trust a file
 *
 * A table describes one specific image: the one the server committed as
 * `version`. Using it against any other image names the wrong chunks. So a
 * record carries the version, the card size and the SHA-256 of the image it
 * describes, and a caller must refuse it unless all three match what it is
 * about to push against. `ss_fp_store_find` checks the first two; the digest is
 * the caller's to compare against its own record of that version
 * (state.txt already stores exactly that).
 *
 * Getting it wrong is not corruption -- PUSH_END carries the digest of the
 * whole card, so a delta built on a stale table is refused with 0x07 and the
 * client sends the whole card instead. It costs a round, which is why the
 * checks are worth making anyway.
 */

#ifndef SLOTSYNC_FINGERPRINT_H
#define SLOTSYNC_FINGERPRINT_H

#include <stddef.h>
#include <stdint.h>

#include "protocol.h"

#define SS_FP_BLOCK_SIZE 8192u
#define SS_FP_CHUNK_SIZE 1024u
#define SS_FP_CHUNKS_PER_BLOCK (SS_FP_BLOCK_SIZE / SS_FP_CHUNK_SIZE)

/* Store file layout. Big-endian throughout, like everything else here.
 *
 *   header      16 bytes
 *   record      56 bytes, then `blocks` big-endian u32 fingerprints
 *   ...
 *
 * header
 *   0   4   magic "SSFP"
 *   4   1   format version
 *   5   1   reserved
 *   6   2   record count
 *   8   4   block size the fingerprints were taken at
 *   12  4   reserved
 *
 * record
 *   0   6   game_id, ASCII space-padded
 *   6   1   slot
 *   7   1   reserved
 *   8   4   version these fingerprints describe
 *   12  4   card size in bytes
 *   16  4   block count, which must agree with the card size
 *   20  32  sha256 of the image they describe
 *   52  4   reserved
 */
#define SS_FP_MAGIC "SSFP"
#define SS_FP_FORMAT 1
#define SS_FP_HEADER_SIZE 16
#define SS_FP_RECORD_SIZE 56
#define SS_FP_DIGEST_SIZE 32

/* Blocks a card of `size` bytes takes. 0 if the size is not usable. */
uint32_t ss_fp_blocks(uint32_t size);

/* Fingerprint of one block.
 *
 * FNV-1a, which needs no table -- a 256-entry CRC table is a kilobyte of kernel
 * rodata to buy collision odds that do not matter here. A collision hides a
 * changed block, the whole-card SHA-256 in PUSH_END catches that, and the cost
 * is one wasted round rather than a wrong card. */
uint32_t ss_fp_hash(const uint8_t *data, uint32_t len);

/* Compare `card` against `fp` and write the chunk bitmap of what changed.
 *
 *   fp     -- one fingerprint per block, `fp_entries` long. **Updated in
 *             place**, so afterwards it describes `card`, not whatever the
 *             server holds. A caller whose push then fails must not keep it.
 *   dirty  -- output, one bit per 1024-byte chunk, LSB-first, based at chunk 0:
 *             exactly the shape ss_push_delta wants.
 *
 * Returns chunks marked, or -1 if a buffer is too small for this card -- which
 * is the caller's cue to push the whole thing. */
int32_t ss_fp_scan(const uint8_t *card, uint32_t size, uint32_t *fp,
                   uint32_t fp_entries, uint8_t *dirty, uint32_t dirty_bytes);

/* Whether a delta of `marked` chunks is worth asking for against a card of
 * `chunks`. A delta costs the server a read of the parent blob, so one that
 * saves almost nothing is worse than the whole card. */
int ss_fp_worthwhile(uint32_t marked, uint32_t chunks);

/* One card's entry in the store. */
typedef struct {
    char game_id[SS_GAME_ID_LEN + 1];
    uint8_t slot;
    uint32_t version; /* the version the fingerprints describe */
    uint32_t size;    /* the card size they were taken at */
    uint32_t blocks;
    uint8_t sha256[SS_FP_DIGEST_SIZE];
} ss_fp_record;

/* --- reading ------------------------------------------------------------ */

/* Find one card's record in an encoded store.
 *
 * `store` is the whole file. Everything in it is treated as untrusted: a
 * truncated or corrupt file finds nothing rather than reading past its end.
 *
 * On success returns 0, fills `out`, and copies the fingerprints into `fp`.
 * Returns -1 when the card has no usable record -- no entry, a different block
 * size, a record whose block count disagrees with its card size, or more blocks
 * than `fp_entries` holds.
 *
 * The caller must still check `out->version` against the parent it is about to
 * push, and `out->sha256` against its own record of that version. */
int ss_fp_store_find(const uint8_t *store, size_t store_len, const char *game_id,
                     uint8_t slot, ss_fp_record *out, uint32_t *fp,
                     uint32_t fp_entries);

/* How many records an encoded store claims, or -1 if it is not one. Cheap
 * enough to call before deciding whether to read the rest of a file. */
int ss_fp_store_count(const uint8_t *store, size_t store_len);

/* Walk an encoded store, one record at a time.
 *
 * `cursor` starts at 0 and is advanced past each record. Returns 1 and fills
 * `out` (and points `values` at that record's fingerprints, still encoded) for
 * each record, 0 once there are none left, and -1 if the store is malformed --
 * at which point nothing after the bad record can be located either, so the
 * walk is over.
 *
 * A writer that has to keep other cards' tables while replacing one uses this
 * to copy them across; `values` is deliberately left encoded so that copy is a
 * memcpy rather than a decode and re-encode. */
int ss_fp_store_next(const uint8_t *store, size_t store_len, size_t *cursor,
                     ss_fp_record *out, const uint8_t **values);

/* Decode one record header. Returns 0, or -1 if the record is self-
 * inconsistent -- no blocks, or a block count that disagrees with the card size
 * it claims. Both mean it does not describe blocks we would compare.
 *
 * Exposed separately from ss_fp_store_find so a reader with no room to hold the
 * file can walk it: the kernel reads 56 bytes, decodes, and either takes the
 * fingerprints that follow or reads past them. A 16 MiB card's table is 8 KiB
 * against a 16 KiB stack, so slurping the file is not on offer there. */
int ss_fp_get_record(ss_fp_record *out, const uint8_t in[SS_FP_RECORD_SIZE]);

/* Decode `count` big-endian fingerprints from `in` into `fp`. The reader above
 * calls this per slice, so the staging buffer stays small. */
void ss_fp_get_values(uint32_t *fp, const uint8_t *in, uint32_t count);

/* --- writing ------------------------------------------------------------ */
/*
 * Serialised piecemeal rather than into one buffer, because the kernel has a
 * 16 KiB stack and a 16 MiB card's table is 8 KiB on its own. A writer emits
 * the file header, then for each card a record header followed by its
 * fingerprints in slices.
 */

/* Build the 16-byte file header for a store holding `records` cards. */
void ss_fp_put_header(uint8_t out[SS_FP_HEADER_SIZE], uint16_t records);

/* Build one 56-byte record header. */
void ss_fp_put_record(uint8_t out[SS_FP_RECORD_SIZE], const ss_fp_record *rec);

/* Serialise `count` fingerprints big-endian into `out`, which needs 4*count
 * bytes. Callers emit a table in slices to keep the staging buffer small. */
void ss_fp_put_values(uint8_t *out, const uint32_t *fp, uint32_t count);

#endif /* SLOTSYNC_FINGERPRINT_H */
