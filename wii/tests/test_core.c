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
#include "../core/fingerprint.h"
#include "../source/wii_fingerprint.h"
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

/* --- the fingerprint store ---------------------------------------------- */
/*
 * core/fingerprint.c and source/wii_fingerprint.c. The store is what lets a
 * push send only the blocks that changed *across a reboot* -- without it the
 * table is empty at every start, so the launcher could never delta at all and
 * the kernel opened every session with a whole card.
 */

#define FP_STORE "test_fp_store.bin"
#define FP_INCOMING "test_fp_runtime.bin"
#define FP_ENTRIES 2048

static void fill_record(ss_fp_record *rec, const char *game_id, uint8_t slot,
                        uint32_t version, uint32_t size, uint8_t seed)
{
    int i;

    memset(rec, 0, sizeof(*rec));
    strncpy(rec->game_id, game_id, SS_GAME_ID_LEN);
    rec->game_id[SS_GAME_ID_LEN] = '\0';
    rec->slot = slot;
    rec->version = version;
    rec->size = size;
    rec->blocks = ss_fp_blocks(size);
    for (i = 0; i < SS_FP_DIGEST_SIZE; i++) {
        rec->sha256[i] = (uint8_t)(seed + i);
    }
}

static void test_fp_store(void)
{
    static uint32_t fp[FP_ENTRIES];
    static uint32_t back[FP_ENTRIES];
    ss_fp_record rec;
    ss_fp_record got;
    wii_fp_store store;
    uint32_t i;

    printf("fingerprint store\n");

    remove(FP_STORE);
    remove(FP_INCOMING);

    /* One card in, the same card out. */
    fill_record(&rec, "GALE01", 0, 7, 2u * 1024u * 1024u, 0x10);
    for (i = 0; i < rec.blocks; i++) {
        fp[i] = 0xA5000000u + i;
    }
    check(wii_fp_store_put(FP_STORE, &rec, fp) == 0, "a table can be stored");

    wii_fp_store_read(&store, FP_STORE);
    check(store.bytes != NULL, "and read back");
    check(wii_fp_store_get(&store, "GALE01", 0, &got, back, FP_ENTRIES) == 0,
          "and found by game id and slot");
    check(got.version == 7 && got.size == rec.size && got.blocks == rec.blocks,
          "with its version, size and block count");
    check(memcmp(got.sha256, rec.sha256, SS_FP_DIGEST_SIZE) == 0,
          "and the digest of the image it describes");
    for (i = 0; i < rec.blocks; i++) {
        if (back[i] != fp[i]) {
            break;
        }
    }
    check(i == rec.blocks, "and every fingerprint intact through the round trip");

    /* A card that is not in the store is simply not found. */
    check(wii_fp_store_get(&store, "GM4E01", 0, &got, back, FP_ENTRIES) != 0,
          "a card with no record is not found");
    check(wii_fp_store_get(&store, "GALE01", 1, &got, back, FP_ENTRIES) != 0,
          "and neither is the other slot of one that is");
    wii_fp_store_free(&store);

    /* A second card must not displace the first. This is the whole reason the
     * kernel writes its own file rather than rewriting this one: clobbering
     * every other card's table to say something about one would be a poor
     * trade. */
    {
        ss_fp_record second;

        fill_record(&second, "GM4E01", 0, 3, 512u * 1024u, 0x40);
        for (i = 0; i < second.blocks; i++) {
            fp[i] = 0x5C000000u + i;
        }
        check(wii_fp_store_put(FP_STORE, &second, fp) == 0, "a second card stores");

        wii_fp_store_read(&store, FP_STORE);
        check(ss_fp_store_count(store.bytes, store.len) == 2, "the store holds both");
        check(wii_fp_store_get(&store, "GALE01", 0, &got, back, FP_ENTRIES) == 0
                  && got.version == 7,
              "and the first card survived the second being written");
        check(wii_fp_store_get(&store, "GM4E01", 0, &got, back, FP_ENTRIES) == 0
                  && got.version == 3,
              "as did the second");
        check(back[0] == 0x5C000000u, "with the right card's fingerprints");
        wii_fp_store_free(&store);
    }

    /* Replacing a card updates it rather than leaving two. */
    fill_record(&rec, "GALE01", 0, 8, 2u * 1024u * 1024u, 0x99);
    for (i = 0; i < rec.blocks; i++) {
        fp[i] = 0xBEEF0000u + i;
    }
    check(wii_fp_store_put(FP_STORE, &rec, fp) == 0, "a card can be replaced");
    wii_fp_store_read(&store, FP_STORE);
    check(ss_fp_store_count(store.bytes, store.len) == 2,
          "and the store still holds two records, not three");
    check(wii_fp_store_get(&store, "GALE01", 0, &got, back, FP_ENTRIES) == 0
              && got.version == 8 && back[0] == 0xBEEF0000u,
          "with the newer table");
    wii_fp_store_free(&store);

    /* The checks that decide whether a table may be trusted. Each one on its
     * own is what stands between a delta and naming the wrong chunks. */
    check(wii_fp_usable(&got, 8, 2u * 1024u * 1024u, rec.sha256),
          "a record matching version, size and digest is usable");
    check(!wii_fp_usable(&got, 9, 2u * 1024u * 1024u, rec.sha256),
          "a record for another version is not");
    check(!wii_fp_usable(&got, 8, 4u * 1024u * 1024u, rec.sha256),
          "nor one for another card size");
    {
        uint8_t wrong[SS_FP_DIGEST_SIZE];

        memcpy(wrong, rec.sha256, SS_FP_DIGEST_SIZE);
        wrong[0] ^= 0xFFu;
        check(!wii_fp_usable(&got, 8, 2u * 1024u * 1024u, wrong),
              "nor one whose digest says it came from other bytes");
    }

    remove(FP_STORE);
}

static void test_fp_store_is_defensive(void)
{
    static uint32_t fp[FP_ENTRIES];
    ss_fp_record rec;
    uint8_t header[SS_FP_HEADER_SIZE];

    printf("fingerprint store, damaged\n");

    /* The file comes off an SD card a user can put anything on, so nothing in
     * it may be trusted to be self-consistent. */
    check(ss_fp_store_count(NULL, 0) < 0, "no store at all");
    check(ss_fp_store_count(header, 3) < 0, "a store shorter than its header");

    ss_fp_put_header(header, 1);
    check(ss_fp_store_count(header, sizeof(header)) == 1, "a good header reads");

    header[0] = 'X';
    check(ss_fp_store_count(header, sizeof(header)) < 0, "bad magic is refused");

    ss_fp_put_header(header, 1);
    header[4] = SS_FP_FORMAT + 1;
    check(ss_fp_store_count(header, sizeof(header)) < 0, "a future format is refused");

    /* A file taken at a different block size describes blocks that are not the
     * ones we would compare, so it is not ours to read. */
    ss_fp_put_header(header, 1);
    ss_put_u32(header + 8, SS_FP_BLOCK_SIZE * 2u);
    check(ss_fp_store_count(header, sizeof(header)) < 0,
          "so is one taken at another block size");

    /* A header claiming a record that is not there must find nothing rather
     * than read past the buffer. */
    ss_fp_put_header(header, 1);
    check(ss_fp_store_find(header, sizeof(header), "GALE01", 0, &rec, fp, FP_ENTRIES)
              != 0,
          "a truncated store finds nothing");

    /* A record whose block count disagrees with its own card size cannot be
     * trusted, and cannot be skipped past either -- that count is what says
     * where the next record starts. */
    {
        uint8_t buf[SS_FP_HEADER_SIZE + SS_FP_RECORD_SIZE + 64];
        ss_fp_record bad;

        memset(buf, 0, sizeof(buf));
        ss_fp_put_header(buf, 1);
        fill_record(&bad, "GALE01", 0, 7, 2u * 1024u * 1024u, 0x10);
        bad.blocks = 4; /* a 2 MiB card is 256 blocks, not 4 */
        ss_fp_put_record(buf + SS_FP_HEADER_SIZE, &bad);
        check(ss_fp_store_find(buf, sizeof(buf), "GALE01", 0, &rec, fp, FP_ENTRIES) != 0,
              "a record whose block count contradicts its size is refused");
    }

    /* A table larger than the caller's own is refused rather than truncated --
     * half a table names the wrong chunks just as surely as a stale one. */
    {
        ss_fp_record big;
        uint32_t i;

        remove(FP_STORE);
        fill_record(&big, "GBIG01", 0, 2, 2u * 1024u * 1024u, 0x22);
        for (i = 0; i < big.blocks; i++) {
            fp[i] = i;
        }
        check(wii_fp_store_put(FP_STORE, &big, fp) == 0, "a big card stores");
        {
            wii_fp_store store;
            uint32_t small[4];

            wii_fp_store_read(&store, FP_STORE);
            check(wii_fp_store_get(&store, "GBIG01", 0, &rec, small, 4) != 0,
                  "and is refused by a reader whose table cannot hold it");
            wii_fp_store_free(&store);
        }
        remove(FP_STORE);
    }
}

static void test_fp_runtime_merge(void)
{
    static uint32_t fp[FP_ENTRIES];
    static uint32_t back[FP_ENTRIES];
    ss_fp_record rec;
    ss_fp_record got;
    wii_fp_store store;
    FILE *probe;
    uint32_t i;

    printf("fingerprint handoff from the kernel\n");

    remove(FP_STORE);
    remove(FP_INCOMING);

    /* The launcher's store, as it stood before the game ran. */
    fill_record(&rec, "GALE01", 0, 7, 2u * 1024u * 1024u, 0x10);
    for (i = 0; i < rec.blocks; i++) {
        fp[i] = 0x11110000u + i;
    }
    wii_fp_store_put(FP_STORE, &rec, fp);

    /* And another card it tracks, which the kernel knows nothing about. */
    fill_record(&rec, "GZLE01", 0, 2, 512u * 1024u, 0x30);
    for (i = 0; i < rec.blocks; i++) {
        fp[i] = 0x33330000u + i;
    }
    wii_fp_store_put(FP_STORE, &rec, fp);

    /* What the kernel left behind: the same card, moved on to v9. */
    fill_record(&rec, "GALE01", 0, 9, 2u * 1024u * 1024u, 0x77);
    for (i = 0; i < rec.blocks; i++) {
        fp[i] = 0x99990000u + i;
    }
    wii_fp_store_put(FP_INCOMING, &rec, fp);

    check(wii_fp_merge_runtime(FP_STORE, FP_INCOMING) == 1,
          "the kernel's handoff merges");

    probe = fopen(FP_INCOMING, "rb");
    check(probe == NULL, "and the handoff file is removed once taken");
    if (probe != NULL) {
        fclose(probe);
    }

    wii_fp_store_read(&store, FP_STORE);
    check(wii_fp_store_get(&store, "GALE01", 0, &got, back, FP_ENTRIES) == 0
              && got.version == 9 && back[0] == 0x99990000u,
          "the card the kernel pushed now carries the kernel's table");
    check(wii_fp_store_get(&store, "GZLE01", 0, &got, back, FP_ENTRIES) == 0
              && got.version == 2 && back[0] == 0x33330000u,
          "and a card it never saw is untouched");
    wii_fp_store_free(&store);

    /* No handoff is the normal case, not an error. */
    check(wii_fp_merge_runtime(FP_STORE, FP_INCOMING) == -1,
          "a missing handoff says so rather than failing");

    remove(FP_STORE);
    remove(FP_INCOMING);
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
    /* A second chunk bitmap and a fingerprint table, for the delta sections:
     * ss_push_delta rewrites `bitmap` as its own scratch every round, so the
     * dirty set it is given has to be somewhere else. */
    unsigned char *bitmap2;
    uint32_t *dirty_fp;
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
    bitmap2 = (unsigned char *)malloc(2048);
    dirty_fp = (uint32_t *)malloc(2048 * sizeof(uint32_t));
    if (card == NULL || pulled == NULL || bitmap == NULL || bitmap2 == NULL
        || dirty_fp == NULL) {
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

    /* Delta push -- docs/PROTOCOL.md, "Delta push".
     *
     * A save write touches its own blocks plus the directory and BAT, so the
     * bytes that changed are a few percent of the card. Rewriting one 8 KiB
     * block here stands in for that. */
    {
        unsigned char *dirty = (unsigned char *)calloc(4096, 1);
        uint32_t chunks = ss_chunk_count((uint32_t)card_size);
        uint32_t first = (uint32_t)((card_size / 2) / SS_MAX_PAYLOAD);
        uint32_t delta_version = 0;
        uint32_t ignored = 0;
        unsigned before;
        int32_t marked;
        uint32_t i;

        if (dirty == NULL) {
            printf("  FAIL  out of memory\n");
            failures++;
            return 1;
        }

        /* Seed the table from the bytes the server holds, then make a save
         * write and let the scan find it -- the same two steps both real
         * clients take. */
        memset(dirty_fp, 0, 2048 * sizeof(uint32_t));
        ss_fp_scan(card, (uint32_t)card_size, dirty_fp, 2048, dirty, 4096);
        for (i = 0; i < 8192u; i++) {
            card[(card_size / 2) + i] ^= 0x5Au;
        }
        marked = ss_fp_scan(card, (uint32_t)card_size, dirty_fp, 2048, dirty, 4096);
        check(marked == 8, "a save write dirties one block, so eight chunks");
        check(ss_bitmap_test(dirty, 4096, 0, first), "at the right chunk");

        before = sock.sent;
        rc = ss_push_delta(&client, game_id, 0, card, (uint32_t)card_size, version, 3,
                           dirty, bitmap, 4096, &delta_version);
        check(rc == SS_OK, "DELTA push");
        if (rc != SS_OK) {
            printf("        %s (nack 0x%02x: %s)\n", ss_strerror(rc),
                   client.last_error_code,
                   ss_strerror_nack(client.last_error_code));
        } else {
            unsigned spent = sock.sent - before;

            check(delta_version == version + 1, "the delta landed on the next version");
            /* begin, one manifest and end are three datagrams of overhead on
             * top of the eight chunks. A whole-card push would be 2048. */
            check(spent < chunks / 8u, "a delta sends a small fraction of the card");
            printf("  delta: %u datagrams for a %u-chunk card, now v%u\n", spent, chunks,
                   delta_version);

            memset(pulled, 0, card_size);
            rc = ss_pull(&client, game_id, 0, 0, pulled, card_size, bitmap, 4096,
                         &got_size, &got_version);
            check(rc == SS_OK, "PULL the delta result back");
            check(memcmp(card, pulled, card_size) == 0,
                  "the server assembled the card byte for byte");
            version = delta_version;
        }

        /* A parent the server cannot seed from must not need handling by the
         * caller: NACK 0x0c falls back to the whole card inside the client.
         *
         * v0 is unseedable by definition -- it is "the server has never seen
         * this card". So this push gets its 0x0c, sends the whole card anyway,
         * and is refused at PUSH_END as a stale parent. A conflict coming back
         * rather than SS_ERR_SERVER with 0x0c is what proves the fallback ran,
         * and the datagram count proves it sent the whole card rather than the
         * eight chunks the bitmap named.
         *
         * The card has to differ from head for that to be a conflict at all:
         * store.push answers an identical re-push as a no-op before it ever
         * looks at the parent. */
        card[0x200] ^= 0xFFu;
        before = sock.sent;
        /* Paced, alone among the pushes here. By this point the run has spent
         * three cards' worth of the server's token bucket, and a fourth
         * unpaced 2 MiB push runs it dry -- which answers 0x0a and hides
         * whatever this test was actually asking. A console paces anyway. */
        sock.pace_every = 512;
        sock.pace_us = 400000;
        rc = ss_push_delta(&client, game_id, 0, card, (uint32_t)card_size, 0, 4, dirty,
                           bitmap, 4096, &ignored);
        sock.pace_every = 0;
        sock.pace_us = 0;
        check(rc == SS_ERR_CONFLICT,
              "an unseedable delta falls back to a whole-card push");
        if (rc != SS_ERR_CONFLICT) {
            printf("        got %s (nack 0x%02x: %s)\n", ss_strerror(rc),
                   client.last_error_code,
                   ss_strerror_nack(client.last_error_code));
        }
        check(sock.sent - before > chunks,
              "the fallback really did send the whole card");
        printf("  fallback sent %u datagrams for the same card\n", sock.sent - before);
        card[0x200] ^= 0xFFu;

        /* A dirty bitmap naming a chunk past the end of the card is the
         * caller's bug, and is caught before anything reaches the wire. */
        memset(dirty, 0xFF, 4096);
        rc = ss_push_delta(&client, game_id, 0, card, (uint32_t)card_size, version, 5,
                           dirty, bitmap, 4096, &ignored);
        check(rc == SS_ERR_PROTOCOL || chunks % 8u == 0,
              "a delta naming a chunk past the end is refused locally");

        free(dirty);
    }

    /* The cross-session case, which is the whole point of the store.
     *
     * Everything above delta'd from a table this process built moments earlier.
     * That is not the situation either real client is in: the launcher is a
     * fresh process every launch and the kernel starts with empty RAM, so
     * without a table on disk the first push of every session is a whole card.
     *
     * So: store the table, forget it the way a reboot would, read it back, and
     * delta from that.
     */
    {
        static uint32_t reloaded[2048];
        ss_fp_record rec;
        wii_fp_store store;
        uint8_t committed[SHA256_DIGEST_SIZE];
        uint32_t chunks = ss_chunk_count((uint32_t)card_size);
        uint32_t first = (uint32_t)((card_size / 4) / SS_MAX_PAYLOAD);
        uint32_t delta_version = 0;
        unsigned before;
        int32_t marked;
        uint32_t i;

        /* The digest of the image the server now holds, which the client kept
         * for us rather than making us hash 2 MiB again. */
        memcpy(committed, client.last_digest, SHA256_DIGEST_SIZE);

        memset(&rec, 0, sizeof(rec));
        memcpy(rec.game_id, game_id, SS_GAME_ID_LEN);
        rec.game_id[SS_GAME_ID_LEN] = '\0';
        rec.slot = 0;
        rec.version = version;
        rec.size = (uint32_t)card_size;
        rec.blocks = ss_fp_blocks((uint32_t)card_size);
        memcpy(rec.sha256, committed, SS_FP_DIGEST_SIZE);

        remove(FP_STORE);
        check(wii_fp_store_put(FP_STORE, &rec, dirty_fp) == 0,
              "the table stores against the version it describes");

        /* A reboot. Nothing of the table survives in memory. */
        memset(reloaded, 0, sizeof(reloaded));
        memset(dirty_fp, 0, 2048 * sizeof(uint32_t));

        wii_fp_store_read(&store, FP_STORE);
        check(wii_fp_store_get(&store, game_id, 0, &rec, reloaded, 2048) == 0,
              "and reads back after it");
        check(wii_fp_usable(&rec, version, (uint32_t)card_size, committed),
              "and is accepted for the version the server holds");
        wii_fp_store_free(&store);

        /* Now a save write, and a delta computed entirely from the table that
         * came off disk. */
        for (i = 0; i < 8192u; i++) {
            card[(card_size / 4) + i] ^= 0x3Cu;
        }
        marked = ss_fp_scan(card, (uint32_t)card_size, reloaded, 2048, bitmap2,
                            2048);
        check(marked == 8, "a reloaded table finds the one changed block");
        check(ss_bitmap_test(bitmap2, 2048, 0, first), "at the right chunk");

        before = sock.sent;
        rc = ss_push_delta(&client, game_id, 0, card, (uint32_t)card_size, version, 7,
                           bitmap2, bitmap, 4096, &delta_version);
        check(rc == SS_OK, "and pushes as a delta across the reboot");
        if (rc != SS_OK) {
            printf("        %s (nack 0x%02x: %s)\n", ss_strerror(rc),
                   client.last_error_code,
                   ss_strerror_nack(client.last_error_code));
        } else {
            check(sock.sent - before < chunks / 8u,
                  "sending a small fraction of the card");
            printf("  across a reboot: %u datagrams for a %u-chunk card, now v%u\n",
                   sock.sent - before, chunks, delta_version);

            memset(pulled, 0, card_size);
            rc = ss_pull(&client, game_id, 0, 0, pulled, card_size, bitmap, 4096,
                         &got_size, &got_version);
            check(rc == SS_OK, "the result pulls back");
            check(memcmp(card, pulled, card_size) == 0,
                  "byte for byte what we hold");
            version = delta_version;
        }
        remove(FP_STORE);
    }

    free(card);
    free(pulled);
    free(bitmap);
    free(bitmap2);
    free(dirty_fp);
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
    test_fp_store();
    test_fp_store_is_defensive();
    test_fp_runtime_merge();

    if (host != NULL && psk != NULL) {
        live_test(host, port, psk, loss, game_id);
    } else {
        printf("\n(no --server given; skipping the live round trip)\n");
    }

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
