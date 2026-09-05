/* SHA-256 (FIPS 180-4) and HMAC-SHA256 (RFC 2104). See sha256.h. */

#include "sha256.h"

#include <string.h>

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define BSIG0(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define BSIG1(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SSIG0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))
#define SSIG1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

static void sha256_block(sha256_ctx *ctx, const uint8_t block[SHA256_BLOCK_SIZE])
{
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;
    int i;

    /* Byte order is explicit rather than a cast: the PowerPC this runs on is
     * big-endian and the host that tests it is little-endian, and the answer
     * must not depend on which. */
    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | ((uint32_t)block[i * 4 + 3]);
    }
    for (i = 16; i < 64; i++) {
        w[i] = SSIG1(w[i - 2]) + w[i - 7] + SSIG0(w[i - 15]) + w[i - 16];
    }

    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

    for (i = 0; i < 64; i++) {
        uint32_t t1 = h + BSIG1(e) + CH(e, f, g) + K[i] + w[i];
        uint32_t t2 = BSIG0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

void sha256_init(sha256_ctx *ctx)
{
    ctx->state[0] = 0x6a09e667u; ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u; ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu; ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu; ctx->state[7] = 0x5be0cd19u;
    ctx->bit_length = 0;
    ctx->buffered = 0;
}

void sha256_update(sha256_ctx *ctx, const void *data, size_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;

    ctx->bit_length += (uint64_t)len * 8u;

    if (ctx->buffered > 0) {
        size_t want = SHA256_BLOCK_SIZE - ctx->buffered;
        size_t take = len < want ? len : want;
        memcpy(ctx->buffer + ctx->buffered, bytes, take);
        ctx->buffered += take;
        bytes += take;
        len -= take;
        if (ctx->buffered == SHA256_BLOCK_SIZE) {
            sha256_block(ctx, ctx->buffer);
            ctx->buffered = 0;
        }
    }

    while (len >= SHA256_BLOCK_SIZE) {
        sha256_block(ctx, bytes);
        bytes += SHA256_BLOCK_SIZE;
        len -= SHA256_BLOCK_SIZE;
    }

    if (len > 0) {
        memcpy(ctx->buffer, bytes, len);
        ctx->buffered = len;
    }
}

void sha256_final(sha256_ctx *ctx, uint8_t out[SHA256_DIGEST_SIZE])
{
    uint64_t bits = ctx->bit_length;
    size_t i;

    ctx->buffer[ctx->buffered++] = 0x80;
    if (ctx->buffered > SHA256_BLOCK_SIZE - 8) {
        memset(ctx->buffer + ctx->buffered, 0, SHA256_BLOCK_SIZE - ctx->buffered);
        sha256_block(ctx, ctx->buffer);
        ctx->buffered = 0;
    }
    memset(ctx->buffer + ctx->buffered, 0, SHA256_BLOCK_SIZE - 8 - ctx->buffered);

    for (i = 0; i < 8; i++) {
        ctx->buffer[SHA256_BLOCK_SIZE - 1 - i] = (uint8_t)(bits >> (8 * i));
    }
    sha256_block(ctx, ctx->buffer);

    for (i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(ctx->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

void sha256(const void *data, size_t len, uint8_t out[SHA256_DIGEST_SIZE])
{
    sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, out);
}

/* --- HMAC ------------------------------------------------------------- */

void hmac_sha256_init(hmac_sha256_ctx *ctx, const void *key, size_t key_len)
{
    uint8_t padded[SHA256_BLOCK_SIZE];
    uint8_t pad[SHA256_BLOCK_SIZE];
    size_t i;

    memset(padded, 0, sizeof(padded));
    if (key_len > SHA256_BLOCK_SIZE) {
        sha256(key, key_len, padded);
    } else {
        memcpy(padded, key, key_len);
    }

    for (i = 0; i < SHA256_BLOCK_SIZE; i++) {
        pad[i] = (uint8_t)(padded[i] ^ 0x36);
    }
    sha256_init(&ctx->inner);
    sha256_update(&ctx->inner, pad, SHA256_BLOCK_SIZE);

    for (i = 0; i < SHA256_BLOCK_SIZE; i++) {
        pad[i] = (uint8_t)(padded[i] ^ 0x5c);
    }
    sha256_init(&ctx->outer);
    sha256_update(&ctx->outer, pad, SHA256_BLOCK_SIZE);

    /* The key is gone from the stack before we return. */
    memset(padded, 0, sizeof(padded));
    memset(pad, 0, sizeof(pad));
}

void hmac_sha256_update(hmac_sha256_ctx *ctx, const void *data, size_t len)
{
    sha256_update(&ctx->inner, data, len);
}

void hmac_sha256_final(hmac_sha256_ctx *ctx, uint8_t out[SHA256_DIGEST_SIZE])
{
    uint8_t inner_digest[SHA256_DIGEST_SIZE];
    sha256_final(&ctx->inner, inner_digest);
    sha256_update(&ctx->outer, inner_digest, SHA256_DIGEST_SIZE);
    sha256_final(&ctx->outer, out);
    memset(inner_digest, 0, sizeof(inner_digest));
}

void hmac_sha256(const void *key, size_t key_len, const void *data, size_t len,
                 uint8_t out[SHA256_DIGEST_SIZE])
{
    hmac_sha256_ctx ctx;
    hmac_sha256_init(&ctx, key, key_len);
    hmac_sha256_update(&ctx, data, len);
    hmac_sha256_final(&ctx, out);
}

int slotsync_memequal(const void *a, const void *b, size_t len)
{
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;
    uint8_t diff = 0;
    size_t i;

    for (i = 0; i < len; i++) {
        diff |= (uint8_t)(x[i] ^ y[i]);
    }
    return diff == 0;
}
