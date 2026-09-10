/* Working out the six-character ID the server keys a card by.
 *
 * Nintendont names its files with the four characters it carries -- ncfg->GameID
 * is a u32, which is why the file it writes is /saves/GC6E.raw and not
 * GC6E01.raw. The server, and the Dolphin daemon, key cards by six. The other
 * two are the maker code, and every directory entry on the card opens with the
 * game code followed by it (docs/MEMCARD.md), so they are read off the card
 * rather than guessed.
 *
 * A card with nothing saved on it yet cannot be identified this way. Space
 * padding the four characters would key a different card on the server than the
 * same game gets from Dolphin, quietly splitting one lineage in two -- so a 0
 * return means "skip this card", not "close enough". PLAN.md section 7.
 *
 * No libc beyond <string.h>, no allocation: this is core/, and it has to stay
 * buildable for the kernel client (CLAUDE.md).
 */

#ifndef SLOTSYNC_MEMCARD_H
#define SLOTSYNC_MEMCARD_H

#include <stdint.h>

#define SS_GAME_ID_LEN 6

/* Writes the six-character ID for the game whose four-character code is `code4`
 * into `out`, which is always left NUL-terminated. Returns 1 when the maker
 * code was read off the card, and 0 when no entry on the card belongs to this
 * game, in which case `out` is space-padded and must not be used as a key. */
int ss_card_game_id(const uint8_t *card, uint32_t card_size, const char code4[4],
                    char out[SS_GAME_ID_LEN + 1]);

#endif /* SLOTSYNC_MEMCARD_H */
