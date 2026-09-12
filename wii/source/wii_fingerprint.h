/* The launcher's half of the fingerprint store -- core/fingerprint.h.
 *
 * The store lets a push send only the blocks that changed. Nintendont's
 * in-kernel sync reads this same file at boot and writes its own tables back
 * through `runtime-fp.bin`, exactly as it already does with state.txt and
 * runtime.txt: the launcher's store covers every card it tracks, the kernel
 * knows about the one game that just ran, and neither should clobber the
 * other's view.
 *
 * Held only in RAM the table would be empty at every start, so the launcher --
 * a fresh process every launch -- could never delta at all, and the kernel
 * would open each session with a whole card. Persisting it is what makes every
 * push after the very first one a delta on both sides.
 *
 * Unlike core/, this file gets to use stdio and malloc: it runs under libogc
 * with a heap, not in an ARM kernel with a 16 KiB stack.
 */

#ifndef SLOTSYNC_WII_FINGERPRINT_H
#define SLOTSYNC_WII_FINGERPRINT_H

#include <stddef.h>
#include <stdint.h>

#include "fingerprint.h"

/* A store file read into memory. `bytes` is NULL for a store that does not
 * exist yet, which is a normal first run rather than an error. */
typedef struct {
    uint8_t *bytes;
    size_t len;
} wii_fp_store;

/* Read a store file. Always succeeds: a missing or unreadable file gives an
 * empty store, because there is nothing a caller could usefully do differently
 * and pushing whole is the right answer either way. */
void wii_fp_store_read(wii_fp_store *store, const char *path);
void wii_fp_store_free(wii_fp_store *store);

/* Look one card up. Returns 0 and fills `rec` and `fp` when the store holds a
 * usable record for it, -1 otherwise.
 *
 * The caller must still check `rec->version` against the parent it is about to
 * push and `rec->sha256` against its own record of that version -- see
 * wii_fp_usable, which is that check. */
int wii_fp_store_get(const wii_fp_store *store, const char *game_id, uint8_t slot,
                     ss_fp_record *rec, uint32_t *fp, uint32_t fp_entries);

/* Whether `rec` describes the image at `version` whose digest is `sha256`, for
 * a card of `size` bytes. A table that describes any other image names the
 * wrong chunks, so all of it has to line up. */
int wii_fp_usable(const ss_fp_record *rec, uint32_t version, uint32_t size,
                  const uint8_t sha256[SS_FP_DIGEST_SIZE]);

/* Write one card's table into the store at `path`, keeping every other card's.
 *
 * Rewrites the file through a temporary and renames, so an interrupted write
 * cannot leave a half store behind -- the same care wii_saves_write takes, for
 * the weaker reason that a corrupt store only costs a whole-card push. Returns
 * 0, or -1 if it could not be written. */
int wii_fp_store_put(const char *path, const ss_fp_record *rec, const uint32_t *fp);

/* Take over the tables the in-kernel sync left in `incoming`, then remove it.
 *
 * Returns how many records were merged, or -1 if there was no file -- which is
 * the normal case, since it only exists after a session that used runtime
 * sync. */
int wii_fp_merge_runtime(const char *path, const char *incoming);

#endif /* SLOTSYNC_WII_FINGERPRINT_H */
