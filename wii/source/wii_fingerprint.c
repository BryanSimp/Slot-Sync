/* See wii_fingerprint.h. */

#include "wii_fingerprint.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void wii_fp_store_read(wii_fp_store *store, const char *path)
{
    FILE *file;
    long size;

    memset(store, 0, sizeof(*store));

    file = fopen(path, "rb");
    if (file == NULL) {
        return; /* no store yet is a normal first run */
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return;
    }
    size = ftell(file);
    if (size < SS_FP_HEADER_SIZE || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return;
    }

    store->bytes = (uint8_t *)malloc((size_t)size);
    if (store->bytes == NULL) {
        fclose(file);
        return;
    }
    if (fread(store->bytes, 1, (size_t)size, file) != (size_t)size) {
        free(store->bytes);
        store->bytes = NULL;
        fclose(file);
        return;
    }
    store->len = (size_t)size;
    fclose(file);
}

void wii_fp_store_free(wii_fp_store *store)
{
    free(store->bytes);
    store->bytes = NULL;
    store->len = 0;
}

int wii_fp_store_get(const wii_fp_store *store, const char *game_id, uint8_t slot,
                     ss_fp_record *rec, uint32_t *fp, uint32_t fp_entries)
{
    if (store->bytes == NULL) {
        return -1;
    }
    return ss_fp_store_find(store->bytes, store->len, game_id, slot, rec, fp,
                            fp_entries);
}

int wii_fp_usable(const ss_fp_record *rec, uint32_t version, uint32_t size,
                  const uint8_t sha256[SS_FP_DIGEST_SIZE])
{
    if (rec->version != version || rec->size != size) {
        return 0;
    }
    /* The digest is the strongest of the three: it says the table was taken
     * from the same bytes we call this version, not merely from something that
     * carried the same number. */
    return memcmp(rec->sha256, sha256, SS_FP_DIGEST_SIZE) == 0;
}

/* Count the records a rewrite will hold: everyone already there except the one
 * being replaced, plus the new one. */
static int count_after_put(const wii_fp_store *store, const ss_fp_record *rec)
{
    ss_fp_record other;
    const uint8_t *values;
    size_t cursor = 0;
    int count = 1;

    if (store->bytes == NULL) {
        return count;
    }
    while (ss_fp_store_next(store->bytes, store->len, &cursor, &other, &values) == 1) {
        if (other.slot == rec->slot
            && strncmp(other.game_id, rec->game_id, SS_GAME_ID_LEN) == 0) {
            continue;
        }
        count++;
    }
    return count;
}

static int write_record(FILE *file, const ss_fp_record *rec, const uint8_t *values,
                        const uint32_t *fp)
{
    uint8_t header[SS_FP_RECORD_SIZE];

    ss_fp_put_record(header, rec);
    if (fwrite(header, 1, sizeof(header), file) != sizeof(header)) {
        return -1;
    }

    if (values != NULL) {
        /* Copying another card's record across: its fingerprints are already
         * encoded, so they move as bytes rather than through a decode and a
         * re-encode that could only introduce a difference. */
        size_t len = (size_t)rec->blocks * 4u;
        return fwrite(values, 1, len, file) == len ? 0 : -1;
    }

    {
        uint8_t slice[512];
        uint32_t left = rec->blocks;
        uint32_t at = 0;

        while (left > 0) {
            uint32_t take = left < (uint32_t)(sizeof(slice) / 4u)
                                ? left
                                : (uint32_t)(sizeof(slice) / 4u);

            ss_fp_put_values(slice, fp + at, take);
            if (fwrite(slice, 1, take * 4u, file) != take * 4u) {
                return -1;
            }
            at += take;
            left -= take;
        }
    }
    return 0;
}

int wii_fp_store_put(const char *path, const ss_fp_record *rec, const uint32_t *fp)
{
    wii_fp_store store;
    char temp[256];
    FILE *file;
    uint8_t header[SS_FP_HEADER_SIZE];
    ss_fp_record other;
    const uint8_t *values;
    size_t cursor = 0;
    int records;
    int failed = 0;

    if (rec->blocks == 0 || rec->blocks != ss_fp_blocks(rec->size)) {
        return -1; /* would not be readable back */
    }

    wii_fp_store_read(&store, path);
    records = count_after_put(&store, rec);

    if ((size_t)snprintf(temp, sizeof(temp), "%s.tmp", path) >= sizeof(temp)) {
        wii_fp_store_free(&store);
        return -1;
    }
    file = fopen(temp, "wb");
    if (file == NULL) {
        wii_fp_store_free(&store);
        return -1;
    }

    ss_fp_put_header(header, (uint16_t)records);
    if (fwrite(header, 1, sizeof(header), file) != sizeof(header)) {
        failed = 1;
    }

    /* Ours first, so a store that somehow ends up with a duplicate resolves to
     * the newest table rather than the stalest one. */
    if (!failed && write_record(file, rec, NULL, fp) != 0) {
        failed = 1;
    }

    while (!failed && store.bytes != NULL
           && ss_fp_store_next(store.bytes, store.len, &cursor, &other, &values) == 1) {
        if (other.slot == rec->slot
            && strncmp(other.game_id, rec->game_id, SS_GAME_ID_LEN) == 0) {
            continue;
        }
        if (write_record(file, &other, values, NULL) != 0) {
            failed = 1;
        }
    }

    fclose(file);
    wii_fp_store_free(&store);

    if (failed) {
        remove(temp);
        return -1;
    }
    /* rename onto an existing file fails on some libc builds, so clear the way
     * first -- the same dance wii_saves_write does. */
    remove(path);
    if (rename(temp, path) != 0) {
        remove(temp);
        return -1;
    }
    return 0;
}

int wii_fp_merge_runtime(const char *path, const char *incoming)
{
    wii_fp_store from;
    ss_fp_record rec;
    const uint8_t *values;
    size_t cursor = 0;
    int merged = 0;

    wii_fp_store_read(&from, incoming);
    if (from.bytes == NULL) {
        /* No runtime sync happened, which is the normal case. Still remove a
         * file that exists but would not parse, so a corrupt one is not
         * reconsidered on every launch. */
        remove(incoming);
        return -1;
    }

    while (ss_fp_store_next(from.bytes, from.len, &cursor, &rec, &values) == 1) {
        uint32_t *fp = (uint32_t *)malloc((size_t)rec.blocks * sizeof(uint32_t));

        if (fp == NULL) {
            break;
        }
        ss_fp_get_values(fp, values, rec.blocks);
        if (wii_fp_store_put(path, &rec, fp) == 0) {
            merged++;
        }
        free(fp);
    }

    wii_fp_store_free(&from);
    remove(incoming);
    return merged;
}
