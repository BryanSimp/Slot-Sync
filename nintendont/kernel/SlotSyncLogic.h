/* The parts of the runtime sync that are decisions rather than plumbing.
 *
 * Split out from SlotSync.c for one reason: none of it needs the kernel, so
 * all of it can be compiled by the host compiler and tested against real
 * inputs. What is left in SlotSync.c is file IO, threads and IOS calls, which
 * cannot be tested anywhere but on a console.
 *
 * Freestanding, allocation-free, no globals. Same rules as wii/core.
 */

#ifndef SLOTSYNC_LOGIC_H
#define SLOTSYNC_LOGIC_H

#include <stddef.h>
#include <stdint.h>

#define SSL_GAME_ID_LEN 6
/* Same six as core/memcard.h's SS_GAME_ID_LEN, which ssl_game_id writes
 * through. Kept as its own name so this header stays standalone. */
#define SSL_PSK_MAX 128

/* Longest deadline accepted from the config file, in ms. Ten minutes times
 * the timer's ~1899 ticks per ms still fits in a u32, which is what the
 * deadline comparisons in SlotSync.c rely on. */
#define SSL_MAX_DEADLINE_MS 600000u

typedef struct {
    int enabled;
    uint32_t server; /* IPv4, host order */
    uint16_t port;
    char psk[SSL_PSK_MAX];
    uint32_t psk_len;
    uint64_t device_id;
    int timeout_ms;
    int rounds;
    uint32_t quiet_ms;      /* card must be untouched this long before a push */
    uint32_t cooldown_ms;   /* minimum gap between two pushes of one card */
    uint32_t net_timeout_ms;
    uint32_t pace_every;
    uint32_t pace_us;

    /* Set when the file named a server that parsed, and a non-empty psk. */
    int have_server;
} ssl_config;

/* Fill `cfg` with defaults. Called by ssl_config_parse, and on its own when
 * there is no file. */
void ssl_config_defaults(ssl_config *cfg);

/* Parse a `key = value` config file already read into memory. `text` must be
 * NUL-terminated. Unknown keys are ignored, so the one file can serve both the
 * libogc wrapper and the kernel.
 *
 * Returns 0 if the result is usable -- runtime sync enabled, a server that
 * parsed, and a psk -- and -1 otherwise. */
int ssl_config_parse(const char *text, ssl_config *cfg);

/* Find what the launcher last agreed with the server for one card, in the
 * state file it writes: lines of `GAMEID SLOT VERSION <64 hex>`.
 *
 * Returns 0 and sets `*out` when the card has an entry. -1 means we have no
 * lineage for it, which is a reason not to push at all: naming the wrong
 * parent is the silent overwrite this project exists to prevent. */
int ssl_state_find(const char *text, const char *game_id, uint8_t slot,
                   uint32_t *out);

/* The six-character ID the server keys a card by.
 *
 * Nintendont only carries four -- ncfg->GameID is a u32 and the file it writes
 * is /saves/GALE.raw, not /saves/GALE01.raw. The maker code that makes up the
 * other two lives in the card's own directory entries, so we read it from
 * there and fall back to space padding on a card with no saves yet.
 *
 * `nin_game_id` is Nintendont's u32, most significant byte first on the wire.
 * `card` may be NULL, in which case only the fallback is produced.
 *
 * Returns 1 if the maker code came from the card, 0 if it was padded. */
int ssl_game_id(const uint8_t *card, uint32_t card_size, uint32_t nin_game_id,
                char out[SSL_GAME_ID_LEN + 1]);

/* Parse a dotted quad into a host-order address. Returns 0 on success. The
 * ingest path has no DNS by design (PLAN.md section 2), so this is the only
 * address form there is. */
int ssl_parse_ipv4(const char *text, uint32_t *out);

/* Parse a decimal or 0x-prefixed unsigned value, stopping at the first
 * character that is not a digit. `end`, if given, is left pointing there. */
uint64_t ssl_parse_u64(const char *s, const char **end);

/* Whether a card is due a push.
 *
 * Split out so the timing rules can be checked without a console clock:
 * everything is in ticks, and the caller supplies them.
 *
 *   dirty        -- a save has landed since the last push
 *   halted       -- a conflict was reported; never push this card again
 *   since_dirty  -- ticks since that save
 *   since_push   -- ticks since the last completed push, or 0 if never
 *   pushed_ever  -- whether since_push means anything yet
 */
int ssl_should_push(int dirty, int halted, uint32_t since_dirty, uint32_t since_push,
                    int pushed_ever, uint32_t quiet_ticks, uint32_t cooldown_ticks);

/* --- delta push --------------------------------------------------------- */
/*
 * What changed since the last push, so only that has to be sent --
 * docs/PROTOCOL.md, "Delta push".
 *
 * The comparison is against a table of per-block fingerprints, not against a
 * second copy of the card: a copy would cost another 2 MiB of ARM RAM, and the
 * table costs four bytes per 8 KiB block.
 *
 * **Block granularity, not chunk.** A GameCube card is erased and written in
 * 8 KiB blocks, so a game cannot change less than one; a finer table would
 * never resolve anything a coarser one misses, and would cost eight times the
 * memory to prove it. One dirty block is eight dirty chunks.
 *
 * Nintendont does track a dirty byte range of its own in GCNCard_ctx, and using
 * it would save this scan. It is a range rather than a set, so a game touching
 * the directory at the front and a save block in the middle dirties everything
 * between -- and reading it would need another hook into a file this project
 * only patches 61 lines of. Fingerprints cost one pass over a card that is
 * already being hashed for PUSH_END anyway.
 */

#define SSL_BLOCK_SIZE 8192u
#define SSL_CHUNK_SIZE 1024u

/* Fingerprint of one block. FNV-1a, which needs no table -- a 256-entry CRC
 * table is a kilobyte of kernel rodata to buy collision odds that do not
 * matter here. A collision hides a changed block, the whole-card SHA-256 in
 * PUSH_END catches that, and the cost is one wasted round rather than a wrong
 * card. */
uint32_t ssl_fingerprint(const uint8_t *data, uint32_t len);

/* Compare `card` against `fp` and write the chunk bitmap of what changed.
 *
 *   fp          -- one fingerprint per block, `fp_entries` long. **Updated in
 *                  place**, so after this returns it describes `card`, not the
 *                  card the server holds. A caller whose push then fails must
 *                  treat the table as unusable until it rebuilds it: see the
 *                  fpValid handling in SlotSync.c.
 *   dirty       -- output, one bit per 1024-byte chunk, LSB-first, based at
 *                  chunk 0 -- exactly the shape ss_push_delta wants.
 *
 * Returns the number of chunks marked, or -1 if a buffer is too small for this
 * card, which is the caller's cue to push the whole thing.
 */
int32_t ssl_delta_scan(const uint8_t *card, uint32_t size, uint32_t *fp,
                       uint32_t fp_entries, uint8_t *dirty, uint32_t dirty_bytes);

/* Whether a delta of `marked` chunks is worth asking for against a card of
 * `chunks`. A delta costs the server a read of the parent blob, so a delta
 * that saves almost nothing is worse than the whole card. */
int ssl_delta_worthwhile(uint32_t marked, uint32_t chunks);

#endif /* SLOTSYNC_LOGIC_H */
