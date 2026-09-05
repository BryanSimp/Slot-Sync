/* SlotSync client. See client.h. */

#include "client.h"

#include <string.h>

#include "sha256.h"

/* Every datagram is built in one of these. 1120 bytes on the stack is fine on
 * the Wii; in the kernel build it becomes a static buffer, which is why nothing
 * here allocates. */
typedef uint8_t ss_datagram[SS_MAX_DATAGRAM];

static void next_nonce(ss_client *c, uint8_t out[SS_NONCE_SIZE])
{
    /* Seed then counter. Uniqueness is what matters -- the server drops a
     * repeated control nonce, retransmissions included. */
    memcpy(out, c->nonce_seed, 8);
    ss_put_u64(out + 8, ++c->nonce_counter);
}

void ss_client_init(ss_client *c, const ss_transport *transport, const uint8_t *key,
                    size_t key_len, uint64_t device_id, const uint8_t seed[8])
{
    memset(c, 0, sizeof(*c));
    c->transport = *transport;
    c->key = key;
    c->key_len = key_len;
    c->device_id = device_id;
    c->timeout_ms = 2000;
    c->max_rounds = 8;
    if (seed != NULL) {
        memcpy(c->nonce_seed, seed, 8);
    }
}

/* Build, sign and send one message. */
static int emit(ss_client *c, ss_header *h, const uint8_t *payload, size_t payload_len)
{
    ss_datagram buffer;
    int packed;

    next_nonce(c, h->nonce);
    packed = ss_pack(buffer, sizeof(buffer), h, payload, payload_len, c->key, c->key_len);
    if (packed < 0) {
        return SS_ERR_PROTOCOL;
    }
    if (c->transport.send(c->transport.ctx, buffer, (size_t)packed) < 0) {
        return SS_ERR_TRANSPORT;
    }
    return SS_OK;
}

/* Wait for one authenticated reply. Returns SS_OK, SS_ERR_TIMEOUT, or an error.
 * Datagrams that fail to authenticate are dropped and we keep waiting, because
 * on a shared network they may not be for us. */
static int await_reply(ss_client *c, uint8_t *buffer, size_t cap, ss_header *h,
                       const uint8_t **payload, size_t *payload_len)
{
    int attempts;

    for (attempts = 0; attempts < 16; attempts++) {
        int got = c->transport.recv(c->transport.ctx, buffer, cap, c->timeout_ms);
        if (got < 0) {
            return SS_ERR_TRANSPORT;
        }
        if (got == 0) {
            return SS_ERR_TIMEOUT;
        }
        if (ss_unpack(buffer, (size_t)got, c->key, c->key_len, h, payload, payload_len)
            == SS_PARSE_OK) {
            return SS_OK;
        }
    }
    return SS_ERR_TIMEOUT;
}

/* Send a control message and wait for its reply, retrying if either direction
 * loses it.
 *
 * Without this a single dropped datagram ends the whole operation, which on
 * console WiFi is not a rare event -- at 20% loss a bare HELLO fails more than
 * a third of the time. Each attempt is re-emitted rather than resent verbatim,
 * so it carries a fresh nonce and is not dropped as a replay. */
static int control_exchange(ss_client *c, ss_header *h, const uint8_t *payload,
                            size_t payload_len, uint8_t *buffer, size_t cap,
                            ss_header *in, const uint8_t **in_payload,
                            size_t *in_payload_len)
{
    int attempt;

    for (attempt = 0; attempt < c->max_rounds; attempt++) {
        int rc = emit(c, h, payload, payload_len);
        if (rc != SS_OK) {
            return rc;
        }
        rc = await_reply(c, buffer, cap, in, in_payload, in_payload_len);
        if (rc != SS_ERR_TIMEOUT) {
            return rc;
        }
    }
    return SS_ERR_TIMEOUT;
}

/* Record a NACK's code and, for a conflict, the head it carries. */
static int note_nack(ss_client *c, const ss_header *h, const uint8_t *payload,
                     size_t payload_len)
{
    c->last_error_code = payload_len > 0 ? payload[0] : 0;

    if (c->last_error_code == SS_NACK_CONFLICT) {
        c->last_head = h->card_version;
        return SS_ERR_CONFLICT;
    }
    return SS_ERR_SERVER;
}

/* --- hello -------------------------------------------------------------- */

int ss_hello(ss_client *c, uint64_t *server_time, uint8_t *server_version)
{
    ss_datagram buffer;
    ss_header out;
    ss_header in;
    const uint8_t *payload;
    size_t payload_len;
    int rc;

    ss_header_init(&out, SS_HELLO, c->device_id, "", 0);
    rc = control_exchange(c, &out, NULL, 0, buffer, sizeof(buffer), &in, &payload,
                          &payload_len);
    if (rc != SS_OK) {
        return rc;
    }
    if (in.msg_type == SS_NACK) {
        return note_nack(c, &in, payload, payload_len);
    }
    if (in.msg_type != SS_ACK) {
        return SS_ERR_PROTOCOL;
    }

    /* Payload is u64 unix seconds then the protocol version. */
    if (payload_len >= 9) {
        if (server_time != NULL) {
            *server_time = ss_get_u64(payload);
        }
        if (server_version != NULL) {
            *server_version = payload[8];
        }
    }
    return SS_OK;
}

/* --- push --------------------------------------------------------------- */

static int send_chunks(ss_client *c, const char *game_id, uint8_t slot,
                       uint32_t transfer_id, const uint8_t *image, uint32_t size,
                       const uint8_t *bitmap, size_t bitmap_cap, uint32_t chunks)
{
    ss_header h;
    uint32_t index;

    for (index = 0; index < chunks; index++) {
        uint32_t offset;
        uint32_t remaining;
        uint32_t take;
        int rc;

        if (!ss_bitmap_test(bitmap, bitmap_cap, 0, index)) {
            continue; /* already acknowledged by its absence from the gap list */
        }

        offset = index * SS_MAX_PAYLOAD;
        remaining = size - offset;
        take = remaining < SS_MAX_PAYLOAD ? remaining : SS_MAX_PAYLOAD;

        ss_header_init(&h, SS_PUSH_CHUNK, c->device_id, game_id, slot);
        h.card_version = transfer_id;
        h.offset = offset;
        h.sequence = index;
        h.total_size = size;

        rc = emit(c, &h, image + offset, take);
        if (rc != SS_OK) {
            return rc;
        }
    }
    return SS_OK;
}

int ss_push(ss_client *c, const char *game_id, uint8_t slot, const uint8_t *image,
            uint32_t size, uint32_t parent, uint32_t transfer_id, uint8_t *bitmap,
            size_t bitmap_cap, uint32_t *out_version)
{
    ss_datagram buffer;
    ss_header h;
    ss_header in;
    const uint8_t *payload;
    size_t payload_len;
    uint8_t digest[SHA256_DIGEST_SIZE];
    uint32_t chunks;
    size_t needed;
    int round;
    int rc;

    if (size == 0) {
        return SS_ERR_PROTOCOL;
    }
    chunks = ss_chunk_count(size);
    needed = ((size_t)chunks + 7u) / 8u;
    if (bitmap_cap < needed) {
        return SS_ERR_TOO_BIG;
    }

    sha256(image, size, digest);

    /* PUSH_BEGIN declares the transfer. control_exchange may resend it if the
     * reply is lost; that is safe because a repeat restarts the transfer
     * server-side and no chunks have been sent yet. */
    ss_header_init(&h, SS_PUSH_BEGIN, c->device_id, game_id, slot);
    h.card_version = transfer_id;
    h.parent_version = parent;
    h.total_size = size;

    rc = control_exchange(c, &h, NULL, 0, buffer, sizeof(buffer), &in, &payload,
                          &payload_len);
    if (rc != SS_OK) {
        return rc;
    }
    if (in.msg_type == SS_NACK) {
        return note_nack(c, &in, payload, payload_len);
    }
    if (in.msg_type != SS_ACK) {
        return SS_ERR_PROTOCOL;
    }

    /* First round sends everything. */
    memset(bitmap, 0, needed);
    {
        uint32_t i;
        for (i = 0; i < chunks; i++) {
            ss_bitmap_set(bitmap, bitmap_cap, 0, i);
        }
    }

    for (round = 0; round < c->max_rounds; round++) {
        rc = send_chunks(c, game_id, slot, transfer_id, image, size, bitmap, bitmap_cap,
                         chunks);
        if (rc != SS_OK) {
            return rc;
        }

        ss_header_init(&h, SS_PUSH_END, c->device_id, game_id, slot);
        h.card_version = transfer_id;
        h.parent_version = parent;
        h.total_size = size;
        rc = emit(c, &h, digest, SHA256_DIGEST_SIZE);
        if (rc != SS_OK) {
            return rc;
        }

        rc = await_reply(c, buffer, sizeof(buffer), &in, &payload, &payload_len);
        if (rc == SS_ERR_TIMEOUT) {
            continue; /* the reply was lost; resend and ask again */
        }
        if (rc != SS_OK) {
            return rc;
        }

        if (in.msg_type == SS_ACK) {
            if (out_version != NULL) {
                *out_version = in.card_version;
            }
            return SS_OK;
        }
        if (in.msg_type != SS_NACK) {
            return SS_ERR_PROTOCOL;
        }
        if (payload_len < 1 || payload[0] != SS_NACK_MISSING_CHUNKS) {
            return note_nack(c, &in, payload, payload_len);
        }

        /* Gaps. The bitmap is windowed with its base in `sequence`, and the
         * server may send several windows, so gather them all before resending.
         */
        memset(bitmap, 0, needed);
        for (;;) {
            uint32_t base = in.sequence;
            size_t i;
            for (i = 1; i < payload_len; i++) {
                int bit;
                for (bit = 0; bit < 8; bit++) {
                    if (payload[i] & (1u << bit)) {
                        uint32_t index = base + (uint32_t)((i - 1) * 8 + (size_t)bit);
                        if (index < chunks) {
                            ss_bitmap_set(bitmap, bitmap_cap, 0, index);
                        }
                    }
                }
            }

            rc = await_reply(c, buffer, sizeof(buffer), &in, &payload, &payload_len);
            if (rc == SS_ERR_TIMEOUT) {
                break; /* no more windows */
            }
            if (rc != SS_OK) {
                return rc;
            }
            if (in.msg_type == SS_ACK) {
                /* The gaps were filled by a retransmission already in flight. */
                if (out_version != NULL) {
                    *out_version = in.card_version;
                }
                return SS_OK;
            }
            if (in.msg_type != SS_NACK || payload_len < 1
                || payload[0] != SS_NACK_MISSING_CHUNKS) {
                return note_nack(c, &in, payload, payload_len);
            }
        }
    }

    return SS_ERR_GAVE_UP;
}

/* --- head --------------------------------------------------------------- */

int ss_head(ss_client *c, const char *game_id, uint8_t slot, uint32_t *out_version,
            uint32_t *out_size)
{
    ss_datagram buffer;
    ss_header h;
    ss_header in;
    const uint8_t *payload;
    size_t payload_len;
    /* An all-zero bitmap: every chunk explicitly not wanted. */
    uint8_t none[8];
    int rc;

    memset(none, 0, sizeof(none));

    ss_header_init(&h, SS_PULL_REQ, c->device_id, game_id, slot);
    h.card_version = 0; /* head */
    h.sequence = 0;

    rc = control_exchange(c, &h, none, sizeof(none), buffer, sizeof(buffer), &in,
                          &payload, &payload_len);
    if (rc != SS_OK) {
        return rc;
    }
    if (in.msg_type == SS_NACK) {
        return note_nack(c, &in, payload, payload_len);
    }
    if (in.msg_type != SS_ACK) {
        return SS_ERR_PROTOCOL;
    }

    if (out_version != NULL) {
        *out_version = in.card_version;
    }
    if (out_size != NULL) {
        *out_size = in.total_size;
    }
    return SS_OK;
}

/* --- pull --------------------------------------------------------------- */

static uint32_t count_missing(const uint8_t *bitmap, size_t bitmap_cap, uint32_t chunks,
                              uint32_t *first_missing)
{
    uint32_t missing = 0;
    uint32_t i;

    *first_missing = 0;
    for (i = 0; i < chunks; i++) {
        if (!ss_bitmap_test(bitmap, bitmap_cap, 0, i)) {
            if (missing == 0) {
                *first_missing = i;
            }
            missing++;
        }
    }
    return missing;
}

int ss_pull(ss_client *c, const char *game_id, uint8_t slot, uint32_t want_version,
            uint8_t *out, size_t out_cap, uint8_t *bitmap, size_t bitmap_cap,
            uint32_t *out_size, uint32_t *out_version)
{
    ss_datagram buffer;
    ss_header h;
    ss_header in;
    const uint8_t *payload;
    size_t payload_len;
    uint8_t request[SS_MAX_PAYLOAD];
    uint32_t total = 0;
    uint32_t version = 0;
    uint32_t chunks = 0;
    size_t needed = 0;
    size_t request_len = 0;
    uint32_t request_base = 0;
    int initialised = 0;
    int round;
    int rc;

    for (round = 0; round < c->max_rounds; round++) {
        ss_header_init(&h, SS_PULL_REQ, c->device_id, game_id, slot);
        h.card_version = want_version;
        h.sequence = request_base;

        rc = emit(c, &h, request_len ? request : NULL, request_len);
        if (rc != SS_OK) {
            return rc;
        }

        /* The server acks and then streams chunks straight after, so the reply
         * to a PULL_REQ is not a single datagram -- it is a burst. One receive
         * loop therefore handles the ack and the chunks together. Treating them
         * separately means a lost ack leaves the retry staring at a chunk it
         * did not expect. */
        for (;;) {
            rc = await_reply(c, buffer, sizeof(buffer), &in, &payload, &payload_len);
            if (rc == SS_ERR_TIMEOUT) {
                break; /* the burst is over */
            }
            if (rc != SS_OK) {
                return rc;
            }
            if (in.msg_type == SS_NACK) {
                return note_nack(c, &in, payload, payload_len);
            }
            if (in.msg_type != SS_ACK && in.msg_type != SS_PULL_CHUNK) {
                continue;
            }

            /* Both message types carry the card's size and version, so a lost
             * ack costs nothing: the first chunk to arrive tells us the same
             * thing. */
            if (!initialised) {
                total = in.total_size;
                version = in.card_version;
                if (total == 0) {
                    return SS_ERR_PROTOCOL;
                }
                if ((size_t)total > out_cap) {
                    return SS_ERR_TOO_BIG;
                }
                chunks = ss_chunk_count(total);
                needed = ((size_t)chunks + 7u) / 8u;
                if (bitmap_cap < needed) {
                    return SS_ERR_TOO_BIG;
                }
                memset(bitmap, 0, needed);
                initialised = 1;
            }

            if (in.msg_type != SS_PULL_CHUNK) {
                continue;
            }

            /* Every chunk is authenticated and its offset is covered by the
             * signature, so a chunk that lands here is both genuine and
             * correctly placed. That is why no separate digest over the whole
             * reassembled image is needed. */
            if ((size_t)in.offset + payload_len > (size_t)total) {
                continue;
            }
            memcpy(out + in.offset, payload, payload_len);
            ss_bitmap_set(bitmap, bitmap_cap, 0, in.sequence);
        }

        if (!initialised) {
            continue; /* nothing came back at all; ask again */
        }

        {
            uint32_t first_missing;
            uint32_t missing = count_missing(bitmap, bitmap_cap, chunks, &first_missing);
            uint32_t window;
            uint32_t i;

            if (missing == 0) {
                if (out_size != NULL) {
                    *out_size = total;
                }
                if (out_version != NULL) {
                    *out_version = version;
                }
                return SS_OK;
            }

            /* Ask again for just the gaps. One window per round is enough: the
             * next round re-checks and asks for whatever is still absent. */
            window = (uint32_t)(SS_MAX_PAYLOAD * 8u);
            request_base = (first_missing / window) * window;
            request_len = SS_MAX_PAYLOAD;
            memset(request, 0, request_len);
            for (i = 0; i < chunks; i++) {
                if (!ss_bitmap_test(bitmap, bitmap_cap, 0, i)) {
                    ss_bitmap_set(request, request_len, request_base, i);
                }
            }
            /* Name the version explicitly from now on, so a push landing
             * mid-pull cannot switch us to a different card. */
            want_version = version;
        }
    }

    return SS_ERR_GAVE_UP;
}

/* --- strings ------------------------------------------------------------ */

const char *ss_strerror(int code)
{
    switch (code) {
    case SS_OK:            return "ok";
    case SS_ERR_TIMEOUT:   return "timed out";
    case SS_ERR_TRANSPORT: return "network error";
    case SS_ERR_PROTOCOL:  return "unexpected reply";
    case SS_ERR_SERVER:    return "server refused";
    case SS_ERR_CONFLICT:  return "conflict: someone else changed this card";
    case SS_ERR_TOO_BIG:   return "card too large for the buffer";
    case SS_ERR_GAVE_UP:   return "gave up retransmitting";
    default:               return "unknown error";
    }
}

const char *ss_strerror_nack(uint8_t code)
{
    switch (code) {
    case SS_NACK_BAD_HMAC:            return "bad hmac (wrong pre-shared key?)";
    case SS_NACK_UNSUPPORTED_VERSION: return "unsupported protocol version";
    case SS_NACK_MALFORMED:           return "malformed header";
    case SS_NACK_UNKNOWN_CARD:        return "no such card on the server";
    case SS_NACK_CONFLICT:       return "conflict: parent is not head";
    case SS_NACK_MISSING_CHUNKS:      return "missing chunks";
    case SS_NACK_CHECKSUM:            return "checksum mismatch";
    case SS_NACK_VALIDATION:          return "card failed format validation";
    case SS_NACK_TOO_LARGE:           return "card too large";
    case SS_NACK_RATE_LIMITED:        return "rate limited";
    case SS_NACK_STAGING_EXPIRED:     return "staging buffer expired";
    default:                         return "unknown NACK";
    }
}
