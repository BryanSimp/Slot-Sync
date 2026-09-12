/* See fingerprint.h. */

#include "fingerprint.h"

#include <string.h>

uint32_t ss_fp_blocks(uint32_t size)
{
    if (size == 0) {
        return 0;
    }
    return (size + SS_FP_BLOCK_SIZE - 1u) / SS_FP_BLOCK_SIZE;
}

uint32_t ss_fp_hash(const uint8_t *data, uint32_t len)
{
    /* FNV-1a, 32 bit. The multiplier is odd, so each step is invertible and a
     * change to any byte always changes the result -- which is the property
     * that matters. Offset basis and prime are the published ones. */
    uint32_t hash = 2166136261u;
    uint32_t i;

    for (i = 0; i < len; i++) {
        hash ^= (uint32_t)data[i];
        hash *= 16777619u;
    }
    return hash;
}

int32_t ss_fp_scan(const uint8_t *card, uint32_t size, uint32_t *fp,
                   uint32_t fp_entries, uint8_t *dirty, uint32_t dirty_bytes)
{
    uint32_t blocks;
    uint32_t chunks;
    uint32_t needed;
    uint32_t marked = 0;
    uint32_t block;

    if (card == NULL || fp == NULL || dirty == NULL || size == 0) {
        return -1;
    }

    blocks = ss_fp_blocks(size);
    chunks = (size + SS_FP_CHUNK_SIZE - 1u) / SS_FP_CHUNK_SIZE;
    needed = (chunks + 7u) / 8u;

    /* Too big for the table this caller carries. Not an error -- the card goes
     * whole, exactly as every push did before delta existed. */
    if (blocks > fp_entries || dirty_bytes < needed) {
        return -1;
    }

    memset(dirty, 0, needed);

    for (block = 0; block < blocks; block++) {
        uint32_t offset = block * SS_FP_BLOCK_SIZE;
        uint32_t left = size - offset;
        uint32_t len = left < SS_FP_BLOCK_SIZE ? left : SS_FP_BLOCK_SIZE;
        uint32_t hash = ss_fp_hash(card + offset, len);
        uint32_t first;
        uint32_t chunk;

        if (hash == fp[block]) {
            continue;
        }
        fp[block] = hash;

        first = offset / SS_FP_CHUNK_SIZE;
        for (chunk = first; chunk < first + SS_FP_CHUNKS_PER_BLOCK && chunk < chunks;
             chunk++) {
            dirty[chunk >> 3] |= (uint8_t)(1u << (chunk & 7u));
            marked++;
        }
    }

    return (int32_t)marked;
}

int ss_fp_worthwhile(uint32_t marked, uint32_t chunks)
{
    /* Half. A delta costs the server one read of the parent blob, and below
     * that ratio the read is cheap against the datagrams it saves; above it,
     * the card may as well go whole and leave the blob alone. The real cases
     * are nowhere near the line -- one save is a few percent of a card -- so
     * this only has to catch "most of the card changed", not be tuned. */
    return chunks > 0 && marked * 2u < chunks;
}

/* --- the store ---------------------------------------------------------- */

int ss_fp_store_count(const uint8_t *store, size_t store_len)
{
    if (store == NULL || store_len < SS_FP_HEADER_SIZE) {
        return -1;
    }
    if (memcmp(store, SS_FP_MAGIC, 4) != 0) {
        return -1;
    }
    if (store[4] != SS_FP_FORMAT) {
        return -1;
    }
    /* A file taken at a different block size describes blocks that are not the
     * ones we would compare, so it is not ours to read. Saying so here is what
     * makes SS_FP_BLOCK_SIZE safe to change later. */
    if (ss_get_u32(store + 8) != SS_FP_BLOCK_SIZE) {
        return -1;
    }
    return (int)ss_get_u16(store + 6);
}

int ss_fp_get_record(ss_fp_record *out, const uint8_t in[SS_FP_RECORD_SIZE])
{
    memcpy(out->game_id, in, SS_GAME_ID_LEN);
    out->game_id[SS_GAME_ID_LEN] = '\0';
    out->slot = in[6];
    out->version = ss_get_u32(in + 8);
    out->size = ss_get_u32(in + 12);
    out->blocks = ss_get_u32(in + 16);
    memcpy(out->sha256, in + 20, SS_FP_DIGEST_SIZE);

    /* A record whose block count disagrees with its own card size was not
     * written by this code, or was written for a card that has since changed
     * size. Either way it describes blocks that are not the ones we compare. */
    if (out->blocks == 0 || out->blocks != ss_fp_blocks(out->size)) {
        return -1;
    }
    return 0;
}

void ss_fp_get_values(uint32_t *fp, const uint8_t *in, uint32_t count)
{
    uint32_t i;

    for (i = 0; i < count; i++) {
        fp[i] = ss_get_u32(in + i * 4u);
    }
}

int ss_fp_store_next(const uint8_t *store, size_t store_len, size_t *cursor,
                     ss_fp_record *out, const uint8_t **values)
{
    size_t offset;

    if (store == NULL || cursor == NULL || out == NULL || values == NULL) {
        return -1;
    }
    if (*cursor == 0) {
        if (ss_fp_store_count(store, store_len) < 0) {
            return -1;
        }
        *cursor = SS_FP_HEADER_SIZE;
    }
    offset = *cursor;

    /* Every step below is bounds-checked against store_len before it reads.
     * The file comes off an SD card a user can put anything on, so a truncated
     * or scrambled one has to end the walk rather than run off the buffer. */
    if (store_len < offset || store_len - offset < SS_FP_RECORD_SIZE) {
        return store_len == offset ? 0 : -1;
    }
    if (ss_fp_get_record(out, store + offset) != 0) {
        return -1;
    }
    offset += SS_FP_RECORD_SIZE;

    if (out->blocks > (uint32_t)(store_len - offset) / 4u) {
        return -1; /* the file claims more fingerprints than it carries */
    }
    *values = store + offset;
    *cursor = offset + (size_t)out->blocks * 4u;
    return 1;
}

int ss_fp_store_find(const uint8_t *store, size_t store_len, const char *game_id,
                     uint8_t slot, ss_fp_record *out, uint32_t *fp,
                     uint32_t fp_entries)
{
    ss_fp_record rec;
    const uint8_t *values;
    size_t cursor = 0;
    int records;
    int i;

    if (out == NULL || fp == NULL || game_id == NULL) {
        return -1;
    }
    records = ss_fp_store_count(store, store_len);
    if (records < 0) {
        return -1;
    }

    for (i = 0; i < records; i++) {
        if (ss_fp_store_next(store, store_len, &cursor, &rec, &values) != 1) {
            return -1;
        }
        if (rec.slot == slot && strncmp(rec.game_id, game_id, SS_GAME_ID_LEN) == 0) {
            if (rec.blocks > fp_entries) {
                return -1; /* the caller's table cannot hold it */
            }
            ss_fp_get_values(fp, values, rec.blocks);
            *out = rec;
            return 0;
        }
    }
    return -1;
}

void ss_fp_put_header(uint8_t out[SS_FP_HEADER_SIZE], uint16_t records)
{
    memset(out, 0, SS_FP_HEADER_SIZE);
    memcpy(out, SS_FP_MAGIC, 4);
    out[4] = SS_FP_FORMAT;
    ss_put_u16(out + 6, records);
    ss_put_u32(out + 8, SS_FP_BLOCK_SIZE);
}

void ss_fp_put_record(uint8_t out[SS_FP_RECORD_SIZE], const ss_fp_record *rec)
{
    size_t i;
    int ended = 0;

    memset(out, 0, SS_FP_RECORD_SIZE);
    /* Space-padded to six, as the wire format writes it. Stop reading the
     * source once it ends: indexing past the NUL of a shorter id like "GM4E"
     * would copy whatever follows it in memory. */
    for (i = 0; i < SS_GAME_ID_LEN; i++) {
        if (!ended && rec->game_id[i] == '\0') {
            ended = 1;
        }
        out[i] = ended ? ' ' : (uint8_t)rec->game_id[i];
    }
    out[6] = rec->slot;
    ss_put_u32(out + 8, rec->version);
    ss_put_u32(out + 12, rec->size);
    ss_put_u32(out + 16, rec->blocks);
    memcpy(out + 20, rec->sha256, SS_FP_DIGEST_SIZE);
}

void ss_fp_put_values(uint8_t *out, const uint32_t *fp, uint32_t count)
{
    uint32_t i;

    for (i = 0; i < count; i++) {
        ss_put_u32(out + i * 4u, fp[i]);
    }
}
