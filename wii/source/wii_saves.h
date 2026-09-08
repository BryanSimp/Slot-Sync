/* The /saves directory Nintendont writes, and what we last agreed with the
 * server about each card.
 *
 * Nintendont keeps one card image per game as GAMEID.raw, which is the same
 * unit the server versions -- so unlike the PC side there is no impedance
 * mismatch here. See PLAN.md section 5.
 */

#ifndef SLOTSYNC_WII_SAVES_H
#define SLOTSYNC_WII_SAVES_H

#include <stddef.h>
#include <stdint.h>

#define WII_MAX_CARDS 64
#define WII_GAME_ID_LEN 6

typedef struct {
    char game_id[WII_GAME_ID_LEN + 1];
    uint8_t slot;
    /* What the server called this card the last time we agreed with it. A push
     * has to name its parent (PLAN.md section 7), and this is the only way to
     * know it. */
    uint32_t version;
    uint8_t sha256[32];
    int known; /* whether the state file had an entry */
} wii_card_state;

typedef struct {
    wii_card_state cards[WII_MAX_CARDS];
    int count;
} wii_state;

/* Scan `dir` for GAMEID.raw files. Fills `out` and returns how many were found,
 * or -1 if the directory cannot be read. */
int wii_saves_scan(const char *dir, char out[][WII_GAME_ID_LEN + 1], int cap);

/* Read a whole card into `buffer`. Returns its size, or -1. */
long wii_saves_read(const char *dir, const char *game_id, uint8_t *buffer, size_t cap);

/* Write a card. Writes to a temporary file and renames, so an interrupted
 * write cannot leave the console a half-card to boot from. Returns 0 or -1. */
int wii_saves_write(const char *dir, const char *game_id, const uint8_t *data,
                    size_t len);

void wii_state_load(wii_state *state, const char *path);
int wii_state_save(const wii_state *state, const char *path);

/* Take over the versions the in-kernel sync reached during the last game.
 *
 * Nintendont's SlotSync writes `GAMEID SLOT VERSION` lines at `path` whenever
 * it pushes a card mid-session. Without reading them we would still think the
 * card descends from whatever it did before the game started, name that as the
 * parent, and be told -- correctly -- that it is a conflict. Every session that
 * used runtime sync would end in one.
 *
 * The file is removed once merged. Returns how many entries were taken, or -1
 * if there was no file, which is the normal case. */
int wii_state_merge_runtime(wii_state *state, const char *path);

/* Look up, or add, the entry for a card. Never returns NULL unless full. */
wii_card_state *wii_state_get(wii_state *state, const char *game_id, uint8_t slot);

#endif /* SLOTSYNC_WII_SAVES_H */
