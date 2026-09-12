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
    c->pull_window = SS_PULL_WINDOW;
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

/* One PUSH_DELTA window: the chunks this transfer will send, from `base`.
 *
 * The payload is the caller's own bitmap, sliced -- no copy and no second
 * buffer. That works because a window is 8192 chunks, so its base is always a
 * whole number of bytes into the bitmap, and because the wire's bit order is
 * the one the bitmap already uses. It is the reason docs/PROTOCOL.md picked
 * LSB-first.
 *
 * Acked, unlike a chunk. A lost declaration would otherwise leave the server
 * believing the delta was empty, and that only surfaces at PUSH_END as a digest
 * failure -- a whole-card restart bought with one dropped datagram.
 */
static int declare_window(ss_client *c, const char *game_id, uint8_t slot,
                          uint32_t transfer_id, uint32_t size, const uint8_t *dirty,
                          uint32_t base, size_t len)
{
    ss_datagram buffer;
    ss_header h;
    ss_header in;
    const uint8_t *payload;
    size_t payload_len;
    int rc;

    /* Trailing zero bytes declare nothing; a window that is all zeros need not
     * be sent at all. */
    while (len > 0 && dirty[(base >> 3) + len - 1] == 0) {
        len--;
    }
    if (len == 0) {
        return SS_OK;
    }

    ss_header_init(&h, SS_PUSH_DELTA, c->device_id, game_id, slot);
    h.card_version = transfer_id;
    h.total_size = size;
    h.sequence = base;

    rc = control_exchange(c, &h, dirty + (base >> 3), len, buffer, sizeof(buffer), &in,
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
    return SS_OK;
}

/* Declare every window of `dirty`. One datagram covers 8192 chunks, so a card
 * up to 8 MiB needs one and a 16 MiB card needs two. */
static int declare_delta(ss_client *c, const char *game_id, uint8_t slot,
                         uint32_t transfer_id, uint32_t size, const uint8_t *dirty,
                         uint32_t chunks)
{
    const uint32_t window = (uint32_t)SS_MAX_PAYLOAD * 8u;
    uint32_t base;

    for (base = 0; base < chunks; base += window) {
        uint32_t left = chunks - base;
        size_t len = (size_t)((left < window ? left : window) + 7u) / 8u;
        int rc = declare_window(c, game_id, slot, transfer_id, size, dirty, base, len);

        if (rc != SS_OK) {
            return rc;
        }
    }
    return SS_OK;
}

/* Whether `dirty` has a bit set at or past `chunks`.
 *
 * Those bits would be declared to the server, which refuses a chunk index past
 * the end of the transfer -- so catch a caller's mistake here rather than as a
 * malformed-header NACK halfway through a push. */
static int declares_past_the_end(const uint8_t *dirty, uint32_t chunks)
{
    uint32_t bit;

    for (bit = chunks; (bit & 7u) != 0; bit++) {
        if (dirty[bit >> 3] & (1u << (bit & 7u))) {
            return 1;
        }
    }
    return 0;
}

/* One attempt at a push, delta or whole.
 *
 * `dirty` NULL means send the whole card. Otherwise it names the chunks that
 * changed, PUSH_BEGIN carries the delta flag, and the server seeds its staging
 * buffer from `parent` instead of from zeros. Either way PUSH_END carries the
 * digest of the whole image, so the server's seeding is verified rather than
 * trusted -- see client.h and docs/PROTOCOL.md. */
static int push_once(ss_client *c, const char *game_id, uint8_t slot,
                     const uint8_t *image, uint32_t size, uint32_t parent,
                     uint32_t transfer_id, const uint8_t *dirty, uint8_t *bitmap,
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
    int delta;
    int began;
    int attempt;
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
    if (dirty != NULL && declares_past_the_end(dirty, chunks)) {
        return SS_ERR_PROTOCOL;
    }

    sha256(image, size, digest);
    /* Kept for the caller: a fingerprint table that outlives the push has to
     * record the digest of the image it describes, and hashing a 2 MiB card
     * twice to learn something we just computed would be silly. */
    memcpy(c->last_digest, digest, SHA256_DIGEST_SIZE);

    /* PUSH_BEGIN declares the transfer. control_exchange may resend it if the
     * reply is lost; that is safe because a repeat restarts the transfer
     * server-side and no chunks have been sent yet.
     *
     * Two attempts, not one: a server that cannot seed from `parent` answers
     * NACK 0x0C, and the whole card goes instead. Without that fallback a
     * pruned blob would make the card unpushable. */
    delta = dirty != NULL;
    began = 0;
    for (attempt = 0; attempt < 2 && !began; attempt++) {
        ss_header_init(&h, SS_PUSH_BEGIN, c->device_id, game_id, slot);
        h.flags = delta ? SS_FLAG_DELTA : 0;
        h.card_version = transfer_id;
        h.parent_version = parent;
        h.total_size = size;

        rc = control_exchange(c, &h, NULL, 0, buffer, sizeof(buffer), &in, &payload,
                              &payload_len);
        if (rc != SS_OK) {
            return rc;
        }
        if (in.msg_type == SS_ACK) {
            began = 1;
            break;
        }
        if (in.msg_type != SS_NACK) {
            return SS_ERR_PROTOCOL;
        }
        if (delta && payload_len > 0 && payload[0] == SS_NACK_DELTA_UNAVAILABLE) {
            delta = 0;
            continue;
        }
        return note_nack(c, &in, payload, payload_len);
    }
    if (!began) {
        return SS_ERR_PROTOCOL;
    }

    if (delta) {
        rc = declare_delta(c, game_id, slot, transfer_id, size, dirty, chunks);
        if (rc != SS_OK) {
            return rc;
        }
    }

    /* First round sends everything the transfer is responsible for: the delta,
     * or the whole card. */
    memset(bitmap, 0, needed);
    if (delta) {
        memcpy(bitmap, dirty, needed);
    } else {
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
         *
         * On a delta the server names only what was declared -- but we hold the
         * whole image either way, so a gap outside the declaration would be
         * answered from it just the same. */
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

int ss_push(ss_client *c, const char *game_id, uint8_t slot, const uint8_t *image,
            uint32_t size, uint32_t parent, uint32_t transfer_id, uint8_t *bitmap,
            size_t bitmap_cap, uint32_t *out_version)
{
    return push_once(c, game_id, slot, image, size, parent, transfer_id, NULL, bitmap,
                     bitmap_cap, out_version);
}

int ss_push_delta(ss_client *c, const char *game_id, uint8_t slot,
                  const uint8_t *image, uint32_t size, uint32_t parent,
                  uint32_t transfer_id, const uint8_t *dirty, uint8_t *bitmap,
                  size_t bitmap_cap, uint32_t *out_version)
{
    int rc = push_once(c, game_id, slot, image, size, parent, transfer_id, dirty, bitmap,
                       bitmap_cap, out_version);

    /* A delta that assembled into something we did not mean.
     *
     * The server seeded from its copy of `parent` and our chunks landed on top,
     * and the result does not hash to the card we are holding -- so its copy of
     * the parent is not the image our dirty bitmap was computed against. That
     * is exactly the check that makes delta push safe (PLAN.md 5), and here it
     * has fired.
     *
     * It costs a round, not a card: nothing was committed, and we hold the
     * whole image. Send all of it rather than hand the failure back, because
     * the caller's only sensible response would be to do this anyway -- and a
     * caller that did not would leave the card stuck, retrying the same bad
     * delta on every save.
     *
     * Only once, and only for a delta. A whole-card push that fails its digest
     * is a torn image or a broken link, and resending it is not a fix. */
    if (dirty != NULL && rc == SS_ERR_SERVER
        && c->last_error_code == SS_NACK_CHECKSUM) {
        rc = push_once(c, game_id, slot, image, size, parent, transfer_id, NULL, bitmap,
                       bitmap_cap, out_version);
    }
    return rc;
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
    size_t request_len;
    uint32_t request_base = 0;
    uint32_t outstanding = 0;
    uint32_t previous_missing = 0xFFFFFFFFu;
    uint32_t window;
    uint32_t budget = 0;
    uint32_t rounds = 0;
    uint32_t i;
    int initialised = 0;
    int stalled = 0;
    int rc;

    /* Ask for a windowful at a time, never for the whole card at once.
     *
     * A PULL_REQ is answered with a burst, and the console's receive buffer
     * holds about forty datagrams. Asking for a 2 MiB card's 2048 chunks in one
     * request means the burst overruns that buffer and the round salvages one
     * bufferful: measured on the real console, forty-five chunks per round,
     * which needs fifty rounds the client is not given. A window the buffer can
     * hold turns the same transfer into sixty-four rounds that each land whole.
     */
    window = c->pull_window > 0 ? (uint32_t)c->pull_window : SS_PULL_WINDOW;
    if (window > (uint32_t)SS_MAX_PAYLOAD * 8u) {
        window = (uint32_t)SS_MAX_PAYLOAD * 8u;
    }

    /* The first round cannot consult the received bitmap, because the card's
     * size is not known until the server's ack. So it asks for the first window
     * and lets the server clip whatever runs past the end of a smaller card. */
    memset(request, 0, sizeof(request));
    for (i = 0; i < window; i++) {
        ss_bitmap_set(request, sizeof(request), request_base, i);
    }
    request_len = (size_t)((window - 1u) / 8u) + 1u;

    for (;;) {
        ss_header_init(&h, SS_PULL_REQ, c->device_id, game_id, slot);
        h.card_version = want_version;
        h.sequence = request_base;

        rc = emit(c, &h, request, request_len);
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

                /* Four passes over the card, plus the caller's allowance for
                 * rounds that achieve nothing, is a ceiling no healthy transfer
                 * comes near. It exists so that a server which answers but
                 * never completes cannot hold the console forever. */
                budget = (chunks / window + 1u) * 4u + (uint32_t)c->max_rounds;

                /* How much of what we asked for can actually arrive: chunks
                 * past the end of the card never will. */
                outstanding = 0;
                for (i = 0; i < chunks; i++) {
                    if (ss_bitmap_test(request, request_len, request_base, i)) {
                        outstanding++;
                    }
                }
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
            if (!ss_bitmap_test(bitmap, bitmap_cap, 0, in.sequence)) {
                memcpy(out + in.offset, payload, payload_len);
                ss_bitmap_set(bitmap, bitmap_cap, 0, in.sequence);
                if (outstanding > 0
                    && ss_bitmap_test(request, request_len, request_base, in.sequence)) {
                    outstanding--;
                }
            }

            /* The window is full. Stopping here rather than waiting out the
             * per-reply timeout is what makes many small rounds affordable:
             * without it every round would cost two idle seconds. */
            if (outstanding == 0) {
                break;
            }
        }

        if (budget != 0 && ++rounds > budget) {
            return SS_ERR_GAVE_UP;
        }

        if (!initialised) {
            /* Nothing came back at all. Ask again with the same window. */
            if (++stalled >= c->max_rounds) {
                return SS_ERR_GAVE_UP;
            }
            continue;
        }

        {
            uint32_t first_missing;
            uint32_t missing = count_missing(bitmap, bitmap_cap, chunks, &first_missing);
            uint32_t picked = 0;
            uint32_t highest;

            if (missing == 0) {
                if (out_size != NULL) {
                    *out_size = total;
                }
                if (out_version != NULL) {
                    *out_version = version;
                }
                return SS_OK;
            }

            /* The give-up rule counts rounds that achieve nothing, not rounds.
             * A window that lands completely is progress however many are left,
             * so a whole card now takes as many rounds as it takes. */
            if (missing < previous_missing) {
                stalled = 0;
            } else if (++stalled >= c->max_rounds) {
                return SS_ERR_GAVE_UP;
            }
            previous_missing = missing;

            /* The next window starts at the first gap and names up to `window`
             * of the chunks still absent. Sizing the payload to the last bit
             * actually set keeps the usual sequential case down to a few bytes.
             */
            request_base = first_missing;
            highest = first_missing;
            memset(request, 0, sizeof(request));
            for (i = first_missing; i < chunks && picked < window; i++) {
                if (ss_bitmap_test(bitmap, bitmap_cap, 0, i)) {
                    continue;
                }
                if (i - request_base >= (uint32_t)SS_MAX_PAYLOAD * 8u) {
                    break; /* past what one request bitmap can express */
                }
                ss_bitmap_set(request, sizeof(request), request_base, i);
                highest = i;
                picked++;
            }
            request_len = (size_t)((highest - request_base) / 8u) + 1u;
            outstanding = picked;

            /* Name the version explicitly from now on, so a push landing
             * mid-pull cannot switch us to a different card. */
            want_version = version;
        }
    }
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
