/* SHA-256 and HMAC-SHA256.
 *
 * Freestanding on purpose: no libc beyond <string.h> and <stdint.h>, no
 * allocation, no globals. PLAN.md section 2 says the ingest path has to be
 * implementable inside Nintendont's ARM kernel, which has none of those. This
 * file is the part of the client that would move there first, so it is written
 * to those rules from the start even though the libogc wrapper is less
 * constrained.
 *
 * The protocol needs both: SHA-256 over the whole card image for PUSH_END, and
 * HMAC-SHA256 per datagram for authentication.
 */

#ifndef SLOTSYNC_SHA256_H
#define SLOTSYNC_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_DIGEST_SIZE 32
#define SHA256_BLOCK_SIZE 64

typedef struct {
    uint32_t state[8];
    uint64_t bit_length;
    uint8_t buffer[SHA256_BLOCK_SIZE];
    size_t buffered;
} sha256_ctx;

void sha256_init(sha256_ctx *ctx);
void sha256_update(sha256_ctx *ctx, const void *data, size_t len);
void sha256_final(sha256_ctx *ctx, uint8_t out[SHA256_DIGEST_SIZE]);

/* One-shot convenience. */
void sha256(const void *data, size_t len, uint8_t out[SHA256_DIGEST_SIZE]);

typedef struct {
    sha256_ctx inner;
    sha256_ctx outer;
} hmac_sha256_ctx;

void hmac_sha256_init(hmac_sha256_ctx *ctx, const void *key, size_t key_len);
void hmac_sha256_update(hmac_sha256_ctx *ctx, const void *data, size_t len);
void hmac_sha256_final(hmac_sha256_ctx *ctx, uint8_t out[SHA256_DIGEST_SIZE]);

void hmac_sha256(const void *key, size_t key_len, const void *data, size_t len,
                 uint8_t out[SHA256_DIGEST_SIZE]);

/* Constant-time compare. Used on the server's replies, so a forged one cannot
 * be found a byte at a time by timing us. */
int slotsync_memequal(const void *a, const void *b, size_t len);

#endif /* SLOTSYNC_SHA256_H */
