/* Tests for the portable core, built with the host compiler.
 *
 * Two halves:
 *
 *   1. Unit tests against published vectors (FIPS 180-4, RFC 4231) and against
 *      the byte offsets in docs/PROTOCOL.md. These need nothing but a compiler.
 *   2. An optional live round trip against a running SlotSync server, with
 *      packet loss injectable. That is the real proof: the C the console will
 *      run, talking to the Python the server runs.
 *
 *   ./test_core
 *   ./test_core --server 127.0.0.1 9977 --psk secret [--loss 20]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../core/client.h"
#include "../core/protocol.h"
#include "../core/sha256.h"
#include "host_socket.h"

static int failures = 0;
static int checks = 0;

static void check(int condition, const char *what)
{
    checks++;
    if (!condition) {
        failures++;
        printf("  FAIL  %s\n", what);
    }
}

static void check_hex(const unsigned char *got, const char *expected_hex, const char *what)
{
    char hex[129];
    size_t len = strlen(expected_hex) / 2;
    size_t i;

    for (i = 0; i < len && i < 64; i++) {
        sprintf(hex + i * 2, "%02x", got[i]);
    }
    hex[len * 2] = '\0';

    checks++;
    if (strcmp(hex, expected_hex) != 0) {
        failures++;
        printf("  FAIL  %s\n        got      %s\n        expected %s\n", what, hex,
               expected_hex);
    }
}

/* --- sha256, against FIPS 180-4 ---------------------------------------- */

static void test_sha256(void)
{
    unsigned char digest[SHA256_DIGEST_SIZE];
    unsigned char big[1000];
    sha256_ctx ctx;
    int i;

    printf("sha256\n");

    sha256("", 0, digest);
    check_hex(digest, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
              "empty string");

    sha256("abc", 3, digest);
    check_hex(digest, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
              "abc");

    sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, digest);
    check_hex(digest, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
              "two-block message");

    /* A million 'a', the classic long-message vector, fed in awkward pieces so
     * the buffering path is exercised rather than only whole blocks. */
    memset(big, 'a', sizeof(big));
    sha256_init(&ctx);
    for (i = 0; i < 1000; i++) {
        sha256_update(&ctx, big, sizeof(big));
    }
    sha256_final(&ctx, digest);
    check_hex(digest, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
              "one million 'a'");

    /* Same input, split at every awkward boundary, must agree. */
    sha256_init(&ctx);
    sha256_update(&ctx, "a", 1);
    sha256_update(&ctx, "bc", 2);
    sha256_final(&ctx, digest);
    check_hex(digest, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
              "abc fed in two pieces");
}

/* --- hmac, against RFC 4231 -------------------------------------------- */

static void test_hmac(void)
{
    unsigned char digest[SHA256_DIGEST_SIZE];
    unsigned char key[131];

    printf("hmac-sha256\n");

    memset(key, 0x0b, 20);
    hmac_sha256(key, 20, "Hi There", 8, digest);
    check_hex(digest, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
              "RFC 4231 case 1");

    hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, digest);
    check_hex(digest, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
              "RFC 4231 case 2");

    /* A key longer than the block size is hashed first. */
    memset(key, 0xaa, 131);
    hmac_sha256(key, 131, "Test Using Larger Than Block-Size Key - Hash Key First", 54,
                digest);
    check_hex(digest, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54",
              "RFC 4231 case 6, oversized key");

    check(slotsync_memequal("abc", "abc", 3), "memequal on equal input");
    check(!slotsync_memequal("abc", "abd", 3), "memequal on differing input");
}

/* --- the wire format, against docs/PROTOCOL.md ------------------------- */

static void test_protocol(void)
{
    unsigned char buffer[SS_MAX_DATAGRAM];
    const unsigned char key[] = "a-pre-shared-key";
    ss_header out;
    ss_header in;
    const unsigned char *payload;
    size_t payload_len;
    int packed;

    printf("wire format\n");

    ss_header_init(&out, SS_PUSH_CHUNK, 0xDEADBEEFCAFEBABEull, "GALE01", 1);
    out.card_version = 7;
    out.parent_version = 6;
    out.offset = 2048;
    out.total_size = 2097152;
    out.sequence = 2;
    memset(out.nonce, 0x11, SS_NONCE_SIZE);

    packed = ss_pack(buffer, sizeof(buffer), &out, (const unsigned char *)"hello world",
                     11, key, sizeof(key) - 1);
    check(packed == SS_HEADER_SIZE + 11, "packed length");

    /* Offsets read straight out of the bytes, as the spec lists them. */
    check(memcmp(buffer + 0, "SLOT", 4) == 0, "magic at 0");
    check(buffer[4] == 1, "version at 4");
    check(buffer[5] == SS_PUSH_CHUNK, "msg_type at 5");
    check(ss_get_u64(buffer + 8) == 0xDEADBEEFCAFEBABEull, "device_id at 8");
    check(memcmp(buffer + 16, "GALE01", 6) == 0, "game_id at 16");
    check(buffer[22] == 1, "slot at 22");
    check(buffer[23] == 0, "reserved at 23");
    check(ss_get_u32(buffer + 24) == 7, "card_version at 24");
    check(ss_get_u32(buffer + 28) == 6, "parent_version at 28");
    check(ss_get_u32(buffer + 32) == 2048, "offset at 32");
    check(ss_get_u32(buffer + 36) == 11, "length at 36");
    check(ss_get_u32(buffer + 40) == 2097152, "total_size at 40");
    check(ss_get_u32(buffer + 44) == 2, "sequence at 44");
    check(buffer[48] == 0x11, "nonce at 48");
    check(memcmp(buffer + SS_HEADER_SIZE, "hello world", 11) == 0, "payload at 96");

    /* Big-endian on the wire regardless of the host we compiled for. */
    check(buffer[44] == 0 && buffer[45] == 0 && buffer[46] == 0 && buffer[47] == 2,
          "multi-byte fields are big-endian");

    check(ss_unpack(buffer, (size_t)packed, key, sizeof(key) - 1, &in, &payload,
                    &payload_len) == SS_PARSE_OK,
          "round trip parses");
    check(in.device_id == out.device_id, "device_id survives");
    check(in.card_version == 7 && in.sequence == 2, "fields survive");
    check(payload_len == 11 && memcmp(payload, "hello world", 11) == 0,
          "payload survives");

    /* Tampering. */
    buffer[SS_HEADER_SIZE] ^= 0xFF;
    check(ss_unpack(buffer, (size_t)packed, key, sizeof(key) - 1, &in, &payload,
                    &payload_len) == SS_PARSE_BAD_HMAC,
          "tampered payload rejected");
    buffer[SS_HEADER_SIZE] ^= 0xFF;

    buffer[32] ^= 0xFF;
    check(ss_unpack(buffer, (size_t)packed, key, sizeof(key) - 1, &in, &payload,
                    &payload_len) == SS_PARSE_BAD_HMAC,
          "tampered header rejected");
    buffer[32] ^= 0xFF;

    check(ss_unpack(buffer, (size_t)packed, (const unsigned char *)"wrong", 5, &in,
                    &payload, &payload_len) == SS_PARSE_BAD_HMAC,
          "wrong key rejected");

    buffer[0] = 'X';
    check(ss_unpack(buffer, (size_t)packed, key, sizeof(key) - 1, &in, &payload,
                    &payload_len) == SS_PARSE_BAD_MAGIC,
          "bad magic rejected");
    buffer[0] = 'S';

    check(ss_unpack(buffer, 10, key, sizeof(key) - 1, &in, &payload, &payload_len)
              == SS_PARSE_SHORT,
          "runt datagram rejected");

    /* A short game id is space-padded, as the format wants. */
    ss_header_init(&out, SS_HELLO, 1, "GM4E", 0);
    packed = ss_pack(buffer, sizeof(buffer), &out, NULL, 0, key, sizeof(key) - 1);
    check(memcmp(buffer + 16, "GM4E  ", 6) == 0, "short game id is space padded");
    check(packed == SS_HEADER_SIZE, "empty payload gives a bare header");
}

static void test_bitmaps(void)
{
    unsigned char bitmap[16];

    printf("chunk bitmaps\n");

    memset(bitmap, 0, sizeof(bitmap));
    ss_bitmap_set(bitmap, sizeof(bitmap), 0, 0);
    check(bitmap[0] == 0x01, "bit order is LSB first");

    memset(bitmap, 0, sizeof(bitmap));
    ss_bitmap_set(bitmap, sizeof(bitmap), 0, 3);
    check(bitmap[0] == 0x08, "bit 3");

    memset(bitmap, 0, sizeof(bitmap));
    ss_bitmap_set(bitmap, sizeof(bitmap), 0, 8);
    check(bitmap[0] == 0x00 && bitmap[1] == 0x01, "bit 8 lands in byte 1");

    memset(bitmap, 0, sizeof(bitmap));
    ss_bitmap_set(bitmap, sizeof(bitmap), 100, 103);
    check(ss_bitmap_test(bitmap, sizeof(bitmap), 100, 103), "windowed set and test");
    check(!ss_bitmap_test(bitmap, sizeof(bitmap), 100, 104), "neighbour not set");
    check(!ss_bitmap_test(bitmap, sizeof(bitmap), 100, 99), "below the base is absent");

    /* Out of range must not write past the buffer. */
    ss_bitmap_set(bitmap, sizeof(bitmap), 0, 100000);
    check(1, "out-of-range set does not crash");

    check(ss_chunk_count(0) == 0, "chunk count of nothing");
    check(ss_chunk_count(1) == 1, "chunk count of one byte");
    check(ss_chunk_count(1024) == 1, "chunk count of exactly one chunk");
    check(ss_chunk_count(1025) == 2, "chunk count just over");
    check(ss_chunk_count(2097152) == 2048, "chunk count of a 2 MiB card");
    check(ss_chunk_count(16777216) == 16384, "chunk count of a 16 MiB card");
}

/* --- live round trip ---------------------------------------------------- */

/* Build a structurally valid, empty memory card so the server's validation on
 * ingest accepts it. Mirrors memcard.format_card(); see docs/MEMCARD.md. */
static void checksums(const unsigned char *data, size_t len, unsigned short *sum,
                      unsigned short *inv)
{
    unsigned short total = 0;
    unsigned short inverse = 0;
    size_t i;

    for (i = 0; i + 1 < len; i += 2) {
        unsigned short word = (unsigned short)((data[i] << 8) | data[i + 1]);
        total = (unsigned short)(total + word);
        inverse = (unsigned short)(inverse + (unsigned short)(word ^ 0xFFFF));
    }
    *sum = (total == 0xFFFF) ? 0 : total;
    *inv = (inverse == 0xFFFF) ? 0 : inverse;
}

#define BLOCK_SIZE 0x2000

static unsigned char *build_card(int mbit, size_t *out_size, unsigned char marker)
{
    size_t total_blocks = (size_t)mbit * 16u;
    size_t data_blocks = total_blocks - 5u;
    size_t size = total_blocks * BLOCK_SIZE;
    unsigned char *image = (unsigned char *)malloc(size);
    unsigned short sum;
    unsigned short inv;

    if (image == NULL) {
        return NULL;
    }
    memset(image, 0xFF, size);

    /* header */
    memset(image, 0xFF, BLOCK_SIZE);
    memset(image, 0x00, 12);
    image[11] = marker; /* makes each generated card distinct */
    ss_put_u64(image + 0x000C, 0);
    ss_put_u32(image + 0x0014, 0);
    ss_put_u32(image + 0x0018, 0);
    ss_put_u32(image + 0x001C, 0);
    ss_put_u16(image + 0x0020, 0);
    ss_put_u16(image + 0x0022, (unsigned short)mbit);
    ss_put_u16(image + 0x0024, 0);
    ss_put_u16(image + 0x01FA, 0);
    checksums(image, 0x01FC, &sum, &inv);
    ss_put_u16(image + 0x01FC, sum);
    ss_put_u16(image + 0x01FE, inv);

    /* directory, and its backup */
    {
        unsigned char *dir = image + BLOCK_SIZE;
        memset(dir, 0xFF, BLOCK_SIZE);
        ss_put_u16(dir + 0x1FFA, 0);
        checksums(dir, 0x1FFC, &sum, &inv);
        ss_put_u16(dir + 0x1FFC, sum);
        ss_put_u16(dir + 0x1FFE, inv);
        memcpy(image + 2 * BLOCK_SIZE, dir, BLOCK_SIZE);
    }

    /* block allocation table, and its backup */
    {
        unsigned char *bat = image + 3 * BLOCK_SIZE;
        memset(bat, 0x00, BLOCK_SIZE);
        ss_put_u16(bat + 0x0004, 0);
        ss_put_u16(bat + 0x0006, (unsigned short)data_blocks);
        ss_put_u16(bat + 0x0008, 4);
        checksums(bat + 4, BLOCK_SIZE - 4, &sum, &inv);
        ss_put_u16(bat + 0x0000, sum);
        ss_put_u16(bat + 0x0002, inv);
        memcpy(image + 4 * BLOCK_SIZE, bat, BLOCK_SIZE);
    }

    memset(image + 5 * BLOCK_SIZE, 0xFF, data_blocks * BLOCK_SIZE);
    *out_size = size;
    return image;
}

static int live_test(const char *host, unsigned short port, const char *psk, int loss,
                     const char *game_id)
{
    host_socket sock;
    ss_transport transport;
    ss_client client;
    unsigned char seed[8];
    unsigned char *card;
    unsigned char *pulled;
    unsigned char *bitmap;
    size_t card_size = 0;
    uint32_t version = 0;
    uint32_t got_size = 0;
    uint32_t got_version = 0;
    uint64_t server_time = 0;
    uint8_t server_version = 0;
    int rc;

    printf("\nlive round trip against %s:%u (loss %d%%)\n", host, port, loss);

    if (host_socket_startup() != 0 || host_socket_open(&sock, host, port) != 0) {
        printf("  FAIL  cannot open a socket\n");
        failures++;
        return 1;
    }
    sock.loss_percent = loss;

    transport.send = host_socket_send;
    transport.recv = host_socket_recv;
    transport.ctx = &sock;

    /* The seed must differ per run. A constant one replays the same nonces
     * after a restart, and the server drops them silently for the length of
     * its replay window -- which looks exactly like the server being down.
     * The real console derives this from its boot-time counter; here the wall
     * clock and the pid do the same job. */
    {
        unsigned long long stamp = (unsigned long long)time(NULL);
        unsigned long long salt = (unsigned long long)(size_t)&sock;
        ss_put_u64(seed, stamp ^ (salt << 16));
    }
    ss_client_init(&client, &transport, (const unsigned char *)psk, strlen(psk),
                   0x5749490000000001ull, seed);
    client.timeout_ms = 1500;
    client.max_rounds = 30;

    rc = ss_hello(&client, &server_time, &server_version);
    check(rc == SS_OK, "HELLO");
    if (rc != SS_OK) {
        printf("        %s (nack 0x%02x)\n", ss_strerror(rc), client.last_error_code);
        host_socket_close(&sock);
        return 1;
    }
    check(server_version == 1, "server speaks protocol v1");
    printf("  server time %llu, protocol v%u\n", (unsigned long long)server_time,
           server_version);

    card = build_card(16, &card_size, 0x42);
    pulled = (unsigned char *)malloc(card_size);
    bitmap = (unsigned char *)malloc(4096);
    if (card == NULL || pulled == NULL || bitmap == NULL) {
        printf("  FAIL  out of memory\n");
        failures++;
        return 1;
    }

    rc = ss_push(&client, game_id, 0, card, (uint32_t)card_size, 0, 1, bitmap, 4096,
                 &version);
    check(rc == SS_OK, "PUSH a 2 MiB card");
    if (rc != SS_OK) {
        printf("        %s (nack 0x%02x: %s)\n", ss_strerror(rc), client.last_error_code,
               ss_strerror_nack(client.last_error_code));
    } else {
        printf("  pushed as version %u (dropped %d out, %d in)\n", version,
               sock.dropped_out, sock.dropped_in);
    }

    memset(pulled, 0, card_size);
    rc = ss_pull(&client, game_id, 0, 0, pulled, card_size, bitmap, 4096, &got_size,
                 &got_version);
    check(rc == SS_OK, "PULL it back");
    if (rc != SS_OK) {
        printf("        %s (nack 0x%02x: %s)\n", ss_strerror(rc), client.last_error_code,
               ss_strerror_nack(client.last_error_code));
    } else {
        check((size_t)got_size == card_size, "pulled size matches");
        check(memcmp(card, pulled, card_size) == 0, "pulled card is byte identical");
        printf("  pulled version %u, %u bytes (dropped %d out, %d in)\n", got_version,
               got_size, sock.dropped_out, sock.dropped_in);
    }

    /* The cheap head query: two datagrams instead of two thousand. */
    {
        uint32_t head_version = 0;
        uint32_t head_size = 0;
        rc = ss_head(&client, game_id, 0, &head_version, &head_size);
        check(rc == SS_OK, "HEAD query");
        check(head_version == version, "head query agrees with the push");
        check((size_t)head_size == card_size, "head query reports the card size");

        rc = ss_head(&client, "ZZZZ99", 1, &head_version, &head_size);
        check(rc == SS_ERR_SERVER && client.last_error_code == SS_NACK_UNKNOWN_CARD,
              "head query on an unknown card says so");
    }

    /* A stale parent must be refused rather than merged. */
    {
        size_t other_size = 0;
        unsigned char *other = build_card(16, &other_size, 0x99);
        uint32_t ignored = 0;

        rc = ss_push(&client, game_id, 0, other, (uint32_t)other_size, 0, 2, bitmap, 4096,
                     &ignored);
        check(rc == SS_ERR_CONFLICT, "a stale parent is a conflict");
        check(client.last_head == version, "the conflict carries the server's head");
        if (rc == SS_ERR_CONFLICT) {
            printf("  conflict reported head=%u, as it should\n", client.last_head);
        }
        free(other);
    }

    free(card);
    free(pulled);
    free(bitmap);
    host_socket_close(&sock);
    host_socket_cleanup();
    return 0;
}

/* --- main --------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *host = NULL;
    const char *psk = NULL;
    const char *game_id = "GTST01";
    unsigned short port = 9977;
    int loss = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--server") == 0 && i + 2 < argc) {
            host = argv[++i];
            port = (unsigned short)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--psk") == 0 && i + 1 < argc) {
            psk = argv[++i];
        } else if (strcmp(argv[i], "--loss") == 0 && i + 1 < argc) {
            loss = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--game") == 0 && i + 1 < argc) {
            game_id = argv[++i];
        }
    }

    test_sha256();
    test_hmac();
    test_protocol();
    test_bitmaps();

    if (host != NULL && psk != NULL) {
        live_test(host, port, psk, loss, game_id);
    } else {
        printf("\n(no --server given; skipping the live round trip)\n");
    }

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
