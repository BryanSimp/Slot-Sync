/* SlotSync binary wire protocol v1, client side. See protocol.h. */

#include "protocol.h"

#include <string.h>

#include "sha256.h"

/* --- big-endian accessors --------------------------------------------- */
/* Written byte by byte rather than cast-and-swap: this compiles for a
 * big-endian PowerPC and is tested on a little-endian host, and the wire format
 * must not depend on which. */

void ss_put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

void ss_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

void ss_put_u64(uint8_t *p, uint64_t v)
{
    ss_put_u32(p, (uint32_t)(v >> 32));
    ss_put_u32(p + 4, (uint32_t)v);
}

uint16_t ss_get_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

uint32_t ss_get_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

uint64_t ss_get_u64(const uint8_t *p)
{
    return ((uint64_t)ss_get_u32(p) << 32) | (uint64_t)ss_get_u32(p + 4);
}

/* --- header ------------------------------------------------------------ */

void ss_header_init(ss_header *h, uint8_t msg_type, uint64_t device_id,
                    const char *game_id, uint8_t slot)
{
    size_t i;

    memset(h, 0, sizeof(*h));
    h->version = SS_VERSION;
    h->msg_type = msg_type;
    h->device_id = device_id;
    h->slot = slot;

    /* Space-padded to six, as the wire format wants. Once the source string
     * ends we stop reading it: continuing to index would run past the NUL of a
     * shorter id like "GM4E" and copy whatever follows it in memory. */
    {
        int ended = 0;
        for (i = 0; i < SS_GAME_ID_LEN; i++) {
            if (!ended && (game_id == NULL || game_id[i] == '\0')) {
                ended = 1;
            }
            h->game_id[i] = ended ? ' ' : game_id[i];
        }
    }
    h->game_id[SS_GAME_ID_LEN] = '\0';
}

static void write_header(uint8_t *out, const ss_header *h, size_t payload_len)
{
    memset(out, 0, SS_HEADER_SIZE);
    memcpy(out, SS_MAGIC, 4);
    out[4] = h->version;
    out[5] = h->msg_type;
    ss_put_u16(out + 6, h->flags);
    ss_put_u64(out + 8, h->device_id);
    memcpy(out + 16, h->game_id, SS_GAME_ID_LEN);
    out[22] = h->slot;
    out[23] = 0; /* reserved */
    ss_put_u32(out + 24, h->card_version);
    ss_put_u32(out + 28, h->parent_version);
    ss_put_u32(out + 32, h->offset);
    ss_put_u32(out + 36, (uint32_t)payload_len);
    ss_put_u32(out + 40, h->total_size);
    ss_put_u32(out + 44, h->sequence);
    memcpy(out + SS_NONCE_OFFSET, h->nonce, SS_NONCE_SIZE);
    /* The hmac field stays zero: it is what the signature is computed over. */
}

int ss_pack(uint8_t *out, size_t out_cap, const ss_header *h, const uint8_t *payload,
            size_t payload_len, const uint8_t *key, size_t key_len)
{
    hmac_sha256_ctx mac;
    uint8_t digest[SHA256_DIGEST_SIZE];

    if (payload_len > SS_MAX_PAYLOAD) {
        return -1;
    }
    if (out_cap < SS_HEADER_SIZE + payload_len) {
        return -1;
    }

    write_header(out, h, payload_len);
    if (payload_len > 0 && payload != NULL) {
        memcpy(out + SS_HEADER_SIZE, payload, payload_len);
    }

    /* Signature covers the header with its own field zeroed, then the payload,
     * so a signature cannot be lifted onto different bytes. */
    hmac_sha256_init(&mac, key, key_len);
    hmac_sha256_update(&mac, out, SS_HEADER_SIZE);
    hmac_sha256_update(&mac, out + SS_HEADER_SIZE, payload_len);
    hmac_sha256_final(&mac, digest);

    memcpy(out + SS_HMAC_OFFSET, digest, SS_HMAC_SIZE);
    return (int)(SS_HEADER_SIZE + payload_len);
}

int ss_unpack(const uint8_t *datagram, size_t len, const uint8_t *key, size_t key_len,
              ss_header *h, const uint8_t **payload, size_t *payload_len)
{
    uint8_t zeroed[SS_HEADER_SIZE];
    uint8_t expected[SHA256_DIGEST_SIZE];
    hmac_sha256_ctx mac;
    size_t body_len;
    uint32_t declared;

    if (len < SS_HEADER_SIZE || len > SS_MAX_DATAGRAM) {
        return SS_PARSE_SHORT;
    }
    if (memcmp(datagram, SS_MAGIC, 4) != 0) {
        return SS_PARSE_BAD_MAGIC;
    }
    if (datagram[4] != SS_VERSION) {
        return SS_PARSE_BAD_VERSION;
    }

    body_len = len - SS_HEADER_SIZE;

    memcpy(zeroed, datagram, SS_HEADER_SIZE);
    memset(zeroed + SS_HMAC_OFFSET, 0, SS_HMAC_SIZE);

    hmac_sha256_init(&mac, key, key_len);
    hmac_sha256_update(&mac, zeroed, SS_HEADER_SIZE);
    hmac_sha256_update(&mac, datagram + SS_HEADER_SIZE, body_len);
    hmac_sha256_final(&mac, expected);

    if (!slotsync_memequal(expected, datagram + SS_HMAC_OFFSET, SS_HMAC_SIZE)) {
        return SS_PARSE_BAD_HMAC;
    }

    /* Nothing above this line trusted a header field. */
    declared = ss_get_u32(datagram + 36);
    if ((size_t)declared != body_len) {
        return SS_PARSE_BAD_LENGTH;
    }

    memset(h, 0, sizeof(*h));
    h->version = datagram[4];
    h->msg_type = datagram[5];
    h->flags = ss_get_u16(datagram + 6);
    h->device_id = ss_get_u64(datagram + 8);
    memcpy(h->game_id, datagram + 16, SS_GAME_ID_LEN);
    h->game_id[SS_GAME_ID_LEN] = '\0';
    h->slot = datagram[22];
    h->card_version = ss_get_u32(datagram + 24);
    h->parent_version = ss_get_u32(datagram + 28);
    h->offset = ss_get_u32(datagram + 32);
    h->length = declared;
    h->total_size = ss_get_u32(datagram + 40);
    h->sequence = ss_get_u32(datagram + 44);
    memcpy(h->nonce, datagram + SS_NONCE_OFFSET, SS_NONCE_SIZE);

    if (payload != NULL) {
        *payload = datagram + SS_HEADER_SIZE;
    }
    if (payload_len != NULL) {
        *payload_len = body_len;
    }
    return SS_PARSE_OK;
}

/* --- bitmaps ----------------------------------------------------------- */

int ss_bitmap_test(const uint8_t *bitmap, size_t bitmap_len, uint32_t base,
                   uint32_t index)
{
    size_t offset;

    if (index < base) {
        return 0;
    }
    offset = (size_t)(index - base);
    if ((offset >> 3) >= bitmap_len) {
        return 0;
    }
    return (bitmap[offset >> 3] >> (offset & 7)) & 1;
}

void ss_bitmap_set(uint8_t *bitmap, size_t bitmap_len, uint32_t base, uint32_t index)
{
    size_t offset;

    if (index < base) {
        return;
    }
    offset = (size_t)(index - base);
    if ((offset >> 3) >= bitmap_len) {
        return;
    }
    bitmap[offset >> 3] |= (uint8_t)(1u << (offset & 7));
}

uint32_t ss_chunk_count(uint32_t total_size)
{
    return (total_size + SS_MAX_PAYLOAD - 1u) / SS_MAX_PAYLOAD;
}
