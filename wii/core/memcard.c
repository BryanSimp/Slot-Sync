/* See memcard.h. */

#include "memcard.h"

#include <string.h>

/* From docs/MEMCARD.md, which was verified against Dolphin's GCMemcard.cpp
 * rather than recalled: the directory is block 1, it holds 127 entries of 0x40
 * bytes, and an entry opens with a 4-byte game code then a 2-byte maker code. */
#define SS_DIR_OFFSET   0x2000
#define SS_DIR_ENTRIES  127
#define SS_DENTRY_SIZE  0x40

int ss_card_game_id(const uint8_t *card, uint32_t card_size, const char code4[4],
                    char out[SS_GAME_ID_LEN + 1])
{
    int i;

    out[0] = code4[0];
    out[1] = code4[1];
    out[2] = code4[2];
    out[3] = code4[3];
    out[4] = ' ';
    out[5] = ' ';
    out[6] = '\0';

    /* Need the header and the whole directory block to be there to look at. */
    if (card == NULL || card_size < SS_DIR_OFFSET + 0x2000) {
        return 0;
    }

    for (i = 0; i < SS_DIR_ENTRIES; i++) {
        const uint8_t *entry = card + SS_DIR_OFFSET + (uint32_t)i * SS_DENTRY_SIZE;

        /* FF FF FF FF marks an unused entry. */
        if (entry[0] == 0xFF && entry[1] == 0xFF && entry[2] == 0xFF
            && entry[3] == 0xFF) {
            continue;
        }
        /* Another game's save, on a card shared between games. */
        if (memcmp(entry, out, 4) != 0) {
            continue;
        }
        out[4] = (char)entry[4];
        out[5] = (char)entry[5];
        return 1;
    }
    return 0;
}
