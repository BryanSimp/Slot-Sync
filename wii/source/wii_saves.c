/* See wii_saves.h. */

#include "wii_saves.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int is_card_name(const char *name, char game_id[WII_GAME_ID_LEN + 1])
{
    size_t len = strlen(name);
    size_t i;

    /* GAMEID.raw and nothing else. Nintendont writes exactly that. */
    if (len != WII_GAME_ID_LEN + 4) {
        return 0;
    }
    if (strcmp(name + WII_GAME_ID_LEN, ".raw") != 0
        && strcmp(name + WII_GAME_ID_LEN, ".RAW") != 0) {
        return 0;
    }
    for (i = 0; i < WII_GAME_ID_LEN; i++) {
        char c = name[i];
        int alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                    || (c >= '0' && c <= '9');
        if (!alnum) {
            return 0;
        }
        game_id[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }
    game_id[WII_GAME_ID_LEN] = '\0';
    return 1;
}

int wii_saves_scan(const char *dir, char out[][WII_GAME_ID_LEN + 1], int cap)
{
    DIR *handle = opendir(dir);
    struct dirent *entry;
    int found = 0;

    if (handle == NULL) {
        return -1;
    }
    while ((entry = readdir(handle)) != NULL && found < cap) {
        char game_id[WII_GAME_ID_LEN + 1];
        if (is_card_name(entry->d_name, game_id)) {
            memcpy(out[found], game_id, WII_GAME_ID_LEN + 1);
            found++;
        }
    }
    closedir(handle);
    return found;
}

static void card_path(char *out, size_t cap, const char *dir, const char *game_id,
                      const char *suffix)
{
    snprintf(out, cap, "%s/%s.raw%s", dir, game_id, suffix);
}

long wii_saves_read(const char *dir, const char *game_id, uint8_t *buffer, size_t cap)
{
    char path[256];
    FILE *file;
    long size;
    size_t got;

    card_path(path, sizeof(path), dir, game_id, "");
    file = fopen(path, "rb");
    if (file == NULL) {
        return -1;
    }

    fseek(file, 0, SEEK_END);
    size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (size <= 0 || (size_t)size > cap) {
        fclose(file);
        return -1;
    }

    got = fread(buffer, 1, (size_t)size, file);
    fclose(file);
    return got == (size_t)size ? size : -1;
}

int wii_saves_write(const char *dir, const char *game_id, const uint8_t *data,
                    size_t len)
{
    char temp[256];
    char final[256];
    FILE *file;
    size_t written;

    card_path(temp, sizeof(temp), dir, game_id, ".tmp");
    card_path(final, sizeof(final), dir, game_id, "");

    file = fopen(temp, "wb");
    if (file == NULL) {
        return -1;
    }
    written = fwrite(data, 1, len, file);
    fclose(file);

    if (written != len) {
        remove(temp);
        return -1;
    }

    /* rename() will not replace an existing file on FAT via newlib, so the old
     * card goes first. The temporary file is already complete on disk at this
     * point, so the window where neither exists is as small as it can be. */
    remove(final);
    if (rename(temp, final) != 0) {
        remove(temp);
        return -1;
    }
    return 0;
}

/* --- state ------------------------------------------------------------- */
/* One line per card: GAMEID SLOT VERSION SHA256HEX */

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void wii_state_load(wii_state *state, const char *path)
{
    FILE *file;
    char line[160];

    memset(state, 0, sizeof(*state));

    file = fopen(path, "r");
    if (file == NULL) {
        return; /* no state yet is a normal first run */
    }

    while (fgets(line, (int)sizeof(line), file) != NULL
           && state->count < WII_MAX_CARDS) {
        wii_card_state *card = &state->cards[state->count];
        char hex[80];
        unsigned slot;
        unsigned version;
        int i;

        memset(card, 0, sizeof(*card));
        if (sscanf(line, "%6s %u %u %64s", card->game_id, &slot, &version, hex) != 4) {
            continue;
        }
        card->slot = (uint8_t)slot;
        card->version = (uint32_t)version;
        card->known = 1;

        for (i = 0; i < 32; i++) {
            int hi = hex_value(hex[i * 2]);
            int lo = hex_value(hex[i * 2 + 1]);
            if (hi < 0 || lo < 0) {
                break;
            }
            card->sha256[i] = (uint8_t)((hi << 4) | lo);
        }
        state->count++;
    }
    fclose(file);
}

int wii_state_save(const wii_state *state, const char *path)
{
    FILE *file = fopen(path, "w");
    int i;

    if (file == NULL) {
        return -1;
    }
    for (i = 0; i < state->count; i++) {
        const wii_card_state *card = &state->cards[i];
        int b;
        fprintf(file, "%s %u %u ", card->game_id, card->slot, card->version);
        for (b = 0; b < 32; b++) {
            fprintf(file, "%02x", card->sha256[b]);
        }
        fprintf(file, "\n");
    }
    fclose(file);
    return 0;
}

int wii_state_merge_runtime(wii_state *state, const char *path)
{
    FILE *file = fopen(path, "r");
    char line[160];
    int merged = 0;

    if (file == NULL) {
        return -1; /* no runtime sync happened, which is the normal case */
    }

    while (fgets(line, (int)sizeof(line), file) != NULL) {
        char game_id[WII_GAME_ID_LEN + 1];
        wii_card_state *card;
        unsigned slot;
        unsigned version;

        if (sscanf(line, "%6s %u %u", game_id, &slot, &version) != 3) {
            continue;
        }
        card = wii_state_get(state, game_id, (uint8_t)slot);
        if (card == NULL) {
            continue;
        }
        /* Only ever move forward. A stale runtime.txt must not walk a card's
         * lineage backwards, which would turn the next push into an overwrite
         * of versions nobody has seen. */
        if (card->known && version <= card->version) {
            continue;
        }
        card->version = (uint32_t)version;
        card->known = 1;
        /* The digest is deliberately left alone. It no longer matches the card,
         * so sync_one will push once at startup -- which is right, because play
         * almost certainly continued after the kernel's last push. */
        merged++;
    }
    fclose(file);
    remove(path);
    return merged;
}

wii_card_state *wii_state_get(wii_state *state, const char *game_id, uint8_t slot)
{
    int i;

    for (i = 0; i < state->count; i++) {
        if (state->cards[i].slot == slot
            && strcmp(state->cards[i].game_id, game_id) == 0) {
            return &state->cards[i];
        }
    }
    if (state->count >= WII_MAX_CARDS) {
        return NULL;
    }

    {
        wii_card_state *card = &state->cards[state->count++];
        memset(card, 0, sizeof(*card));
        strncpy(card->game_id, game_id, WII_GAME_ID_LEN);
        card->game_id[WII_GAME_ID_LEN] = '\0';
        card->slot = slot;
        return card;
    }
}
