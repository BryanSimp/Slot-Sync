/* SlotSync client: push and pull a whole card over the binary UDP protocol.
 *
 * Portable and allocation-free. Every buffer is caller-owned, and the socket is
 * reached through two function pointers -- so this same code runs on libogc's
 * net_* API, on BSD sockets in the host tests, and eventually inside
 * Nintendont's ARM kernel where there is no libc to allocate from.
 *
 * See docs/PROTOCOL.md. The transfer semantics that matter:
 *
 *   - Chunks may be lost, reordered or duplicated. The server sorts it out and
 *     names the gaps in a NACK bitmap.
 *   - Nothing is committed until PUSH_END verifies, so a console that loses
 *     power mid-transfer never disturbs the server's current head.
 *   - A control message carries a fresh nonce every send, retransmissions
 *     included, or the server drops it as a replay.
 */

#ifndef SLOTSYNC_CLIENT_H
#define SLOTSYNC_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#include "protocol.h"

/* Chunks a pull asks for per round. The card arrives as an unsolicited burst
 * and the console holds only about forty datagrams of it, so a window larger
 * than the receive buffer is thrown away rather than queued. */
#define SS_PULL_WINDOW 32

/* How the client reaches the network. Both return < 0 on error; `recv` returns
 * 0 on timeout, which is a normal outcome rather than a failure. */
typedef struct {
    int (*send)(void *ctx, const uint8_t *data, size_t len);
    int (*recv)(void *ctx, uint8_t *buf, size_t cap, int timeout_ms);
    void *ctx;
} ss_transport;

typedef struct {
    ss_transport transport;
    const uint8_t *key;
    size_t key_len;
    uint64_t device_id;

    /* Nonce = 8 bytes of boot-time entropy, then a counter. docs/PROTOCOL.md
     * suggests exactly this, and it is what makes a fresh nonce free on a
     * device with no good RNG. */
    uint8_t nonce_seed[8];
    uint64_t nonce_counter;

    int timeout_ms;  /* per reply; 2000 is a sane default */
    int max_rounds;  /* retransmission rounds before giving up; 8 is sane */
    int pull_window; /* chunks per pull round; 0 means SS_PULL_WINDOW */

    /* Populated after a call, for reporting. */
    uint32_t last_head;      /* on SS_CONFLICT, the server's head version */
    uint8_t last_error_code; /* the NACK code, when one arrived */
} ss_client;

/* Result codes. Negative is failure; SS_CONFLICT is a decision for a human,
 * never something the client resolves itself (PLAN.md section 7). */
enum {
    SS_OK = 0,
    SS_ERR_TIMEOUT = -1,
    SS_ERR_TRANSPORT = -2,
    SS_ERR_PROTOCOL = -3,   /* server said something we cannot parse */
    SS_ERR_SERVER = -4,     /* NACK we cannot act on; see last_error_code */
    SS_ERR_CONFLICT = -5,   /* parent is not head; last_head has theirs */
    SS_ERR_TOO_BIG = -6,    /* card will not fit the caller's buffer */
    SS_ERR_GAVE_UP = -7     /* out of retransmission rounds */
};

void ss_client_init(ss_client *c, const ss_transport *transport, const uint8_t *key,
                    size_t key_len, uint64_t device_id, const uint8_t seed[8]);

/* Announce ourselves and learn the server's clock. Either out pointer may be
 * NULL. Doubles as a reachability check. */
int ss_hello(ss_client *c, uint64_t *server_time, uint8_t *server_version);

/* Push a whole card.
 *
 * `transfer_id` is this client's own identifier for the transfer, echoed
 * through every message of it -- at PUSH_BEGIN we cannot know the version the
 * server will assign, and it comes back in `out_version`.
 *
 * `bitmap` is scratch for tracking which chunks still need sending; it must be
 * at least (ss_chunk_count(size) + 7) / 8 bytes. */
int ss_push(ss_client *c, const char *game_id, uint8_t slot, const uint8_t *image,
            uint32_t size, uint32_t parent, uint32_t transfer_id, uint8_t *bitmap,
            size_t bitmap_cap, uint32_t *out_version);

/* Ask what version the server holds, without transferring the card.
 *
 * No new message type needed: a PULL_REQ whose payload is an all-zero bitmap
 * asks for no chunks at all, and the server still acks with the card's version
 * and size. That turns "is the server ahead of me?" into two datagrams instead
 * of two thousand.
 *
 * Returns SS_OK and fills the outputs, or SS_ERR_SERVER with last_error_code
 * SS_NACK_UNKNOWN_CARD if the server has never seen this card. */
int ss_head(ss_client *c, const char *game_id, uint8_t slot, uint32_t *out_version,
            uint32_t *out_size);

/* Pull a whole card into `out`.
 *
 * `want_version` of 0 means head. `bitmap` is scratch for tracking which chunks
 * have arrived, same size rule as ss_push. */
int ss_pull(ss_client *c, const char *game_id, uint8_t slot, uint32_t want_version,
            uint8_t *out, size_t out_cap, uint8_t *bitmap, size_t bitmap_cap,
            uint32_t *out_size, uint32_t *out_version);

/* A human-readable name for a result code, for the on-screen log. */
const char *ss_strerror(int code);

/* A human-readable name for a NACK error code. */
const char *ss_strerror_nack(uint8_t code);

#endif /* SLOTSYNC_CLIENT_H */
