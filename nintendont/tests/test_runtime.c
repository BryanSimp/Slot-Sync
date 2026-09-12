/* Tests for the Nintendont runtime-sync engine, built with the host compiler.
 *
 * SlotSync.c itself cannot be tested off a console -- it is IOS ioctls, fatfs
 * and an ARM thread. What can be tested is everything it decides, which is why
 * that lives in SlotSyncLogic.c: the same source file the kernel compiles.
 *
 * Two halves, matching wii/tests/test_core.c:
 *
 *   1. Unit tests over the config, the state file, the card's directory and
 *      the push timing. These need nothing but a compiler.
 *   2. An optional live run against a real SlotSync server that drives the
 *      protocol core exactly the way SlotSync.c drives it -- including the
 *      conflict case, which is the one this project most needs to get right.
 *
 *   ./test_runtime
 *   ./test_runtime --server 127.0.0.1 9977 --psk secret [--card path.raw]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../kernel/SlotSyncLogic.h"
#include "../../wii/core/client.h"
#include "../../wii/core/fingerprint.h"
#include "../../wii/core/protocol.h"
#include "../../wii/core/sha256.h"
#include "../../wii/tests/host_socket.h"

/* Matches SlotSync.c. */
#define TICKS_PER_MS 1899u

static int failures;
static int checks;

static void check(int condition, const char *what)
{
    checks++;
    if (!condition) {
        failures++;
        printf("  FAIL  %s\n", what);
    }
}

/* ------------------------------------------------------------------ */
/* Config                                                              */
/* ------------------------------------------------------------------ */

static void test_config(void)
{
    ssl_config cfg;
    int rc;

    static const char good[] =
        "# a comment\n"
        "server     = 192.168.1.10\n"
        "port       = 9977\n"
        "psk        = hunter2\n"
        "device_id  = 0x5749490000000001\n"
        "saves_dir  = sd:/saves\n"      /* the wrapper's key; we ignore it */
        "runtime_quiet_ms = 2500\n"
        "runtime_cooldown_ms = 15000\n";

    printf("config\n");

    rc = ssl_config_parse(good, &cfg);
    check(rc == 0, "a complete config is accepted");
    check(cfg.server == 0xC0A8010AU, "server parses to a host-order u32");
    check(cfg.port == 9977, "port");
    check(strcmp(cfg.psk, "hunter2") == 0, "psk");
    check(cfg.psk_len == 7, "psk length");
    check(cfg.device_id == 0x5749490000000001ULL, "device_id parses as hex");
    check(cfg.quiet_ms == 2500, "runtime_quiet_ms");
    check(cfg.cooldown_ms == 15000, "runtime_cooldown_ms");
    check(cfg.rounds == 12, "rounds defaults");
    check(cfg.pace_every != 0 && cfg.pace_us != 0, "pacing defaults on");
    /* 400 datagrams/sec, measured on hardware as the rate at which a 2 MiB card
     * lands with zero sends refused. IOS's send path is the binding constraint,
     * not the server: unpaced it refuses about three quarters of a card. */
    check(cfg.pace_every * 1000000u / cfg.pace_us == 400u,
          "the default pace is the one hardware sustains");
    /* And it has to stay under the server's own UDP rate limit too, which
     * defaults to 2048 datagrams/sec (SLOTSYNC_UDP_RATE). */
    check(cfg.pace_every * 1000000u / cfg.pace_us < 2048u,
          "the default pace sits under the server's UDP rate limit");

    /* The wrapper's own config file must parse without runtime keys, and
     * still produce workable defaults -- one file serves both clients. */
    rc = ssl_config_parse("server = 10.0.0.2\npsk = k\n", &cfg);
    check(rc == 0, "a wrapper-only config still enables runtime sync");
    check(cfg.quiet_ms == 4000 && cfg.cooldown_ms == 30000, "defaults fill in");

    check(ssl_config_parse("server = 10.0.0.2\npsk = k\nruntime_sync = 0\n", &cfg) != 0,
          "runtime_sync = 0 disables it");
    check(ssl_config_parse("psk = k\n", &cfg) != 0, "no server is refused");
    check(ssl_config_parse("server = 10.0.0.2\n", &cfg) != 0, "no psk is refused");
    check(ssl_config_parse("server = example.local\npsk = k\n", &cfg) != 0,
          "a hostname is refused: there is no DNS on this path");

    /* A deadline long enough to overflow the tick arithmetic is clamped, not
     * accepted -- quiet_ms * TICKS_PER_MS has to stay inside a u32. */
    ssl_config_parse("server = 10.0.0.2\npsk = k\nruntime_cooldown_ms = 99999999\n",
                     &cfg);
    check(cfg.cooldown_ms == SSL_MAX_DEADLINE_MS, "an absurd deadline is clamped");
    check((unsigned long long)cfg.cooldown_ms * TICKS_PER_MS <= 0xFFFFFFFFULL,
          "the clamp keeps tick arithmetic inside a u32");
}

static void test_ipv4(void)
{
    uint32_t addr = 0;

    printf("addresses\n");
    check(ssl_parse_ipv4("0.0.0.0", &addr) == 0 && addr == 0, "0.0.0.0");
    check(ssl_parse_ipv4("255.255.255.255", &addr) == 0 && addr == 0xFFFFFFFFU,
          "broadcast");
    check(ssl_parse_ipv4("192.168.1.10", &addr) == 0 && addr == 0xC0A8010AU,
          "a normal address");
    check(ssl_parse_ipv4("192.168.1", &addr) != 0, "three octets is refused");
    check(ssl_parse_ipv4("192.168.1.256", &addr) != 0, "256 is refused");
    check(ssl_parse_ipv4("192.168.1.1.1", &addr) != 0, "five octets is refused");
    check(ssl_parse_ipv4("192.168.01.1", &addr) == 0, "a leading zero is fine");
    check(ssl_parse_ipv4("", &addr) != 0, "empty is refused");
    check(ssl_parse_ipv4("a.b.c.d", &addr) != 0, "letters are refused");
}

/* ------------------------------------------------------------------ */
/* The launcher's state file                                           */
/* ------------------------------------------------------------------ */

static void test_state(void)
{
    /* The format wii/source/wii_saves.c writes. */
    static const char state[] =
        "GALE01 0 7 "
        "0000000000000000000000000000000000000000000000000000000000000000\n"
        "GM4E01 0 12 "
        "1111111111111111111111111111111111111111111111111111111111111111\n"
        "GALE01 1 3 "
        "2222222222222222222222222222222222222222222222222222222222222222\n";
    uint32_t version = 0;

    printf("state file\n");

    check(ssl_state_find(state, "GALE01", 0, &version, NULL) == 0 && version == 7,
          "slot A version");
    check(ssl_state_find(state, "GALE01", 1, &version, NULL) == 0 && version == 3,
          "slot B of the same game is a different card");
    check(ssl_state_find(state, "GM4E01", 0, &version, NULL) == 0 && version == 12,
          "a later line");
    check(ssl_state_find(state, "GZLE01", 0, &version, NULL) != 0, "an unknown card");
    check(ssl_state_find("", "GALE01", 0, &version, NULL) != 0, "an empty state file");

    /* A six-character ID must match all six. "GALE0" is a prefix of the first
     * line's ID and must not match it. */
    check(ssl_state_find(state, "GALE0 ", 0, &version, NULL) != 0,
          "a prefix does not match");
}

/* ------------------------------------------------------------------ */
/* Card identity                                                       */
/* ------------------------------------------------------------------ */

static unsigned char *read_file(const char *path, long *out_len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long len;

    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (unsigned char *)malloc((size_t)len);
    if (buf == NULL || fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_len = len;
    return buf;
}

static void test_game_id(const char *card_path)
{
    char id[SSL_GAME_ID_LEN + 1];
    unsigned char *card;
    long len = 0;

    printf("card identity\n");

    /* No card at all: Nintendont's four characters, space padded. 'GALE' is
     * 0x47414C45. */
    check(ssl_game_id(NULL, 0, 0x47414C45U, id) == 0, "no card, no maker code");
    check(strcmp(id, "GALE  ") == 0, "falls back to space padding");

    /* A card that is all 0xFF in its directory has no entries to read. */
    {
        unsigned char *blank = (unsigned char *)malloc(0x4000);
        memset(blank, 0xFF, 0x4000);
        check(ssl_game_id(blank, 0x4000, 0x47414C45U, id) == 0,
              "a card with no saves yields no maker code");
        check(strcmp(id, "GALE  ") == 0, "and still pads");
        free(blank);
    }

    if (card_path == NULL) {
        printf("  (skipping the real-card check: no --card given)\n");
        return;
    }
    card = read_file(card_path, &len);
    if (card == NULL) {
        printf("  FAIL  cannot read %s\n", card_path);
        failures++;
        return;
    }

    /* These assume tests/fixture.raw, which carries a GALE01 save. Point
     * --card at another image and the identity block is skipped rather than
     * reported as a failure. */
    if (ssl_game_id(card, (uint32_t)len, 0x47414C45U, id) != 1) {
        printf("  (skipping: %s carries no GALE01 save)\n", card_path);
        free(card);
        return;
    }
    check(ssl_game_id(card, (uint32_t)len, 0x47414C45U, id) == 1,
          "the maker code is read from the card's directory");
    check(strcmp(id, "GALE01") == 0, "giving the full six-character ID");

    /* A different game's save on the same card must not be mistaken for ours. */
    check(ssl_game_id(card, (uint32_t)len, 0x474D3445U, id) == 0,
          "another game's entry is not borrowed");
    check(strcmp(id, "GM4E  ") == 0, "and that game falls back to padding");

    free(card);
}

/* ------------------------------------------------------------------ */
/* Push timing                                                         */
/* ------------------------------------------------------------------ */

static void test_should_push(void)
{
    const uint32_t quiet = 4000 * TICKS_PER_MS;
    const uint32_t cool = 30000 * TICKS_PER_MS;

    printf("push timing\n");

    check(!ssl_should_push(0, 0, quiet * 2, 0, 0, quiet, cool),
          "a clean card is never pushed");
    check(!ssl_should_push(1, 1, quiet * 2, 0, 0, quiet, cool),
          "a halted card is never pushed, however dirty");
    check(!ssl_should_push(1, 0, quiet / 2, 0, 0, quiet, cool),
          "a card still being written waits for quiet");
    check(ssl_should_push(1, 0, quiet, 0, 0, quiet, cool),
          "quiet exactly at the threshold is enough");
    check(ssl_should_push(1, 0, quiet * 2, 0, 0, quiet, cool),
          "a quiet dirty card is pushed");
    check(!ssl_should_push(1, 0, quiet * 2, cool / 2, 1, quiet, cool),
          "the cooldown holds off a second push");
    check(ssl_should_push(1, 0, quiet * 2, cool, 1, quiet, cool),
          "and releases at the threshold");

    /* TimerDiffTicks returns UINT_MAX when the hardware timer has wrapped,
     * which must read as "long ago" rather than "just now". */
    check(ssl_should_push(1, 0, 0xFFFFFFFFU, 0xFFFFFFFFU, 1, quiet, cool),
          "a wrapped timer reads as long ago");
}

/* ------------------------------------------------------------------ */
/* Live: the sequence SlotSync.c performs, against a real server        */
/* ------------------------------------------------------------------ */
/* Delta scan                                                          */
/* ------------------------------------------------------------------ */

#define DELTA_CARD_BYTES (64u * 1024u) /* 8 blocks, 64 chunks */

static void test_delta_scan(void)
{
    static uint8_t card[DELTA_CARD_BYTES];
    static uint32_t fp[DELTA_CARD_BYTES / SS_FP_BLOCK_SIZE];
    static uint8_t dirty[DELTA_CARD_BYTES / SS_FP_CHUNK_SIZE / 8];
    const uint32_t entries = DELTA_CARD_BYTES / SS_FP_BLOCK_SIZE;
    const uint32_t chunks = DELTA_CARD_BYTES / SS_FP_CHUNK_SIZE;
    int32_t marked;
    uint32_t i;

    printf("delta scan\n");

    for (i = 0; i < DELTA_CARD_BYTES; i++) {
        card[i] = (uint8_t)(i * 7u + 3u);
    }
    memset(fp, 0, sizeof(fp));

    /* The first scan has nothing to compare against, so everything reads as
     * changed. That is why SlotSync.c keeps fp_valid and pushes the first card
     * of a session whole. */
    marked = ss_fp_scan(card, DELTA_CARD_BYTES, fp, entries, dirty, sizeof(dirty));
    check(marked == (int32_t)chunks, "the first scan marks the whole card");

    /* And the table now describes the card, so an immediate re-scan is clean. */
    marked = ss_fp_scan(card, DELTA_CARD_BYTES, fp, entries, dirty, sizeof(dirty));
    check(marked == 0, "a card that has not changed marks nothing");
    for (i = 0; i < sizeof(dirty); i++) {
        if (dirty[i] != 0) {
            break;
        }
    }
    check(i == sizeof(dirty), "and leaves the bitmap empty");

    /* One byte anywhere in a block dirties that block, and a block is eight
     * chunks. Block granularity is the point: a GameCube card cannot be
     * written in less than a block. */
    card[SS_FP_BLOCK_SIZE * 3u + 17u] ^= 0xFFu;
    marked = ss_fp_scan(card, DELTA_CARD_BYTES, fp, entries, dirty, sizeof(dirty));
    check(marked == 8, "one changed byte marks one block, which is eight chunks");
    for (i = 0; i < chunks; i++) {
        int set = (dirty[i >> 3] >> (i & 7u)) & 1;
        int expected = (i >= 24u && i < 32u);
        if (set != expected) {
            break;
        }
    }
    check(i == chunks, "and marks exactly the chunks of that block");

    /* Two bytes in the same block are still one block. */
    card[SS_FP_BLOCK_SIZE * 5u] ^= 0x01u;
    card[SS_FP_BLOCK_SIZE * 5u + SS_FP_BLOCK_SIZE - 1u] ^= 0x80u;
    marked = ss_fp_scan(card, DELTA_CARD_BYTES, fp, entries, dirty, sizeof(dirty));
    check(marked == 8, "two changes inside one block are still one block");

    /* Changes in different blocks accumulate. */
    card[0] ^= 0x01u;
    card[SS_FP_BLOCK_SIZE * 7u] ^= 0x01u;
    marked = ss_fp_scan(card, DELTA_CARD_BYTES, fp, entries, dirty, sizeof(dirty));
    check(marked == 16, "two changed blocks are sixteen chunks");

    /* A card larger than the table is not an error: the caller pushes it
     * whole, exactly as every push did before delta existed. */
    check(ss_fp_scan(card, DELTA_CARD_BYTES, fp, entries - 1u, dirty, sizeof(dirty))
              == -1,
          "a card too big for the table declines rather than truncating");
    check(ss_fp_scan(card, DELTA_CARD_BYTES, fp, entries, dirty, 1u) == -1,
          "so does a bitmap too small to hold the answer");
    check(ss_fp_scan(NULL, DELTA_CARD_BYTES, fp, entries, dirty, sizeof(dirty)) == -1,
          "and a missing card");
    check(ss_fp_scan(card, 0, fp, entries, dirty, sizeof(dirty)) == -1,
          "and an empty one");

    /* A card whose last block is short still scans: 4 Mbit is 64 whole blocks,
     * but nothing in the format guarantees that forever. */
    {
        static uint32_t small_fp[3];
        static uint8_t small_dirty[3];
        uint32_t odd = SS_FP_BLOCK_SIZE * 2u + SS_FP_CHUNK_SIZE;

        memset(small_fp, 0, sizeof(small_fp));
        marked = ss_fp_scan(card, odd, small_fp, 3, small_dirty,
                                sizeof(small_dirty));
        check(marked == 17, "a short final block is scanned, not skipped");
    }
}

static void test_fingerprint(void)
{
    static uint8_t block[SS_FP_BLOCK_SIZE];
    uint32_t base;
    uint32_t i;
    int differed = 1;

    printf("fingerprints\n");

    for (i = 0; i < SS_FP_BLOCK_SIZE; i++) {
        block[i] = (uint8_t)(i & 0xFFu);
    }
    base = ss_fp_hash(block, SS_FP_BLOCK_SIZE);

    check(ss_fp_hash(block, SS_FP_BLOCK_SIZE) == base, "the same bytes hash alike");

    /* Every single-byte change has to move the hash, at the first byte, the
     * last, and in between. A change that did not would hide a dirty block --
     * the whole-card digest would catch it at PUSH_END, but at the cost of a
     * wasted round. */
    for (i = 0; i < SS_FP_BLOCK_SIZE; i += 37u) {
        block[i] ^= 0x01u;
        if (ss_fp_hash(block, SS_FP_BLOCK_SIZE) == base) {
            differed = 0;
        }
        block[i] ^= 0x01u;
    }
    block[SS_FP_BLOCK_SIZE - 1u] ^= 0x80u;
    if (ss_fp_hash(block, SS_FP_BLOCK_SIZE) == base) {
        differed = 0;
    }
    block[SS_FP_BLOCK_SIZE - 1u] ^= 0x80u;
    check(differed, "a one-bit change anywhere moves the fingerprint");

    check(ss_fp_hash(block, SS_FP_BLOCK_SIZE) == base, "and undoing it restores it");

    printf("delta worth\n");
    check(ss_fp_worthwhile(8, 2048), "8 of 2048 chunks is worth a delta");
    check(ss_fp_worthwhile(472, 2048), "so is Animal Crossing's 472");
    check(ss_fp_worthwhile(0, 2048), "and so is a card that did not change");
    check(!ss_fp_worthwhile(1024, 2048), "half is not");
    check(!ss_fp_worthwhile(2048, 2048), "nor is all of it");
    check(!ss_fp_worthwhile(0, 0), "nor is a card with no chunks");
}

/* ------------------------------------------------------------------ */
/* The fingerprint store, read the way the kernel reads it             */
/* ------------------------------------------------------------------ */
/*
 * SlotSync.c cannot slurp the file: a 16 MiB card's table is 8 KiB against a
 * 16 KiB stack, and the kernel has no heap to put it on. So it walks the file
 * sequentially -- header, then a record header at a time, taking the
 * fingerprints of the one card it wants a slice at a time and reading past
 * everyone else's.
 *
 * That walk is in SlotSync.c and needs FatFs, so it cannot run here. What can
 * run here is the decoding it does at each step, against bytes laid out exactly
 * as the file lays them out. If these agree, the only thing left untested in
 * the kernel's loader is f_read.
 */

static void test_kernel_store_walk(void)
{
    uint8_t store[SS_FP_HEADER_SIZE + 2 * (SS_FP_RECORD_SIZE + 64 * 4)];
    uint32_t table[64];
    ss_fp_record rec;
    size_t at = 0;
    uint32_t values[64];
    uint32_t i;
    int walked = 0;

    printf("fingerprint store, walked in slices\n");

    /* Two cards, 512 KiB each, so 64 blocks apiece. */
    ss_fp_put_header(store, 2);
    at = SS_FP_HEADER_SIZE;

    for (i = 0; i < 2; i++) {
        ss_fp_record out;
        uint32_t b;

        memset(&out, 0, sizeof(out));
        memcpy(out.game_id, i == 0 ? "GM4E01" : "GALE01", SS_GAME_ID_LEN);
        out.game_id[SS_GAME_ID_LEN] = '\0';
        out.slot = 0;
        out.version = i == 0 ? 3u : 12u;
        out.size = 512u * 1024u;
        out.blocks = ss_fp_blocks(out.size);
        memset(out.sha256, (int)(0x40 + i), SS_FP_DIGEST_SIZE);

        for (b = 0; b < out.blocks; b++) {
            values[b] = (i == 0 ? 0xAAAA0000u : 0xBBBB0000u) + b;
        }
        ss_fp_put_record(store + at, &out);
        at += SS_FP_RECORD_SIZE;
        ss_fp_put_values(store + at, values, out.blocks);
        at += out.blocks * 4u;
    }

    check(ss_fp_store_count(store, SS_FP_HEADER_SIZE) == 2,
          "the header alone says how many records follow");

    /* Now the walk, using only what the kernel has: a record header at a time,
     * and values in slices of sixteen. */
    {
        size_t offset = SS_FP_HEADER_SIZE;
        int r;

        for (r = 0; r < 2; r++) {
            uint32_t left;
            uint32_t into = 0;

            check(ss_fp_get_record(&rec, store + offset) == 0,
                  "each record header decodes on its own");
            offset += SS_FP_RECORD_SIZE;

            if (memcmp(rec.game_id, "GALE01", SS_GAME_ID_LEN) != 0) {
                offset += rec.blocks * 4u; /* read past a card we do not want */
                continue;
            }

            check(rec.version == 12u, "the wanted record carries its version");
            check(rec.size == 512u * 1024u, "and its card size");
            check(rec.blocks == 64u, "and a block count that matches it");

            left = rec.blocks;
            while (left > 0) {
                uint32_t take = left < 16u ? left : 16u;

                ss_fp_get_values(table + into, store + offset + into * 4u, take);
                into += take;
                left -= take;
            }
            for (i = 0; i < rec.blocks; i++) {
                if (table[i] != 0xBBBB0000u + i) {
                    break;
                }
            }
            check(i == rec.blocks,
                  "and its fingerprints survive being taken sixteen at a time");
            walked = 1;
        }
    }
    check(walked, "the second card is reached by reading past the first");
}

/* ------------------------------------------------------------------ */

static void live(const char *host, unsigned short port, const char *psk,
                 const char *card_path, unsigned pace_every, unsigned pace_us)
{
    host_socket sock;
    ss_transport transport;
    ss_client client;
    static uint8_t bitmap[2048];
    /* Seeded from the clock, exactly as the kernel does. A constant seed makes
     * the second run of this test inside the server's 120 s replay window fail
     * with a silent drop -- which is worth knowing, and worth not doing. */
    uint8_t seed[8];
    unsigned char *card;
    long len = 0;
    uint32_t head = 0;
    uint32_t size = 0;
    uint32_t assigned = 0;
    uint32_t parent;
    char id[SSL_GAME_ID_LEN + 1];
    int rc;

    printf("\nlive against %s:%u\n", host, port);

    {
        unsigned long long stamp = (unsigned long long)time(NULL);
        unsigned long long jitter = (unsigned long long)clock();
        int b;
        for (b = 0; b < 4; b++) {
            seed[b] = (uint8_t)(stamp >> (b * 8));
            seed[4 + b] = (uint8_t)(jitter >> (b * 8));
        }
    }

    card = read_file(card_path, &len);
    if (card == NULL) {
        printf("  FAIL  cannot read %s\n", card_path);
        failures++;
        return;
    }
    ssl_game_id(card, (uint32_t)len, 0x47414C45U, id);
    printf("  card %s, %ld KiB, id %s\n", card_path, len / 1024, id);

    if (host_socket_startup() != 0 || host_socket_open(&sock, host, port) != 0) {
        printf("  FAIL  cannot open a socket to %s:%u\n", host, port);
        failures++;
        free(card);
        return;
    }
    sock.pace_every = pace_every;
    sock.pace_us = pace_us;
    transport.send = host_socket_send;
    transport.recv = host_socket_recv;
    transport.ctx = &sock;
    ss_client_init(&client, &transport, (const uint8_t *)psk, strlen(psk),
                   0x5749490000000001ULL, seed);

    rc = ss_hello(&client, NULL, NULL);
    check(rc == SS_OK, "HELLO is answered");
    if (rc != SS_OK) {
        printf("  %s\n", ss_strerror(rc));
        goto done;
    }

    /* SlotSync_Init's job: learn the lineage before touching anything. */
    rc = ss_head(&client, id, 0, &head, &size);
    if (rc != SS_OK && client.last_error_code == SS_NACK_UNKNOWN_CARD) {
        head = 0;
        printf("  the server has not seen %s before; starting at v0\n", id);
    } else {
        check(rc == SS_OK, "a head query is answered");
        printf("  server head is v%u\n", head);
    }
    parent = head;

    /* One runtime push, exactly as ss_push_slot does it. */
    rc = ss_push(&client, id, 0, card, (uint32_t)len, parent, parent + 1, bitmap,
                 sizeof(bitmap), &assigned);
    check(rc == SS_OK, "a whole-card runtime push is accepted");
    if (rc != SS_OK) {
        printf("  %s (nack 0x%02x)\n", ss_strerror(rc), client.last_error_code);
        goto done;
    }
    printf("  pushed as v%u\n", assigned);
    check(assigned > parent, "the assigned version moves the lineage on");

    /* A second push with a changed byte, chaining off what we were just
     * assigned -- the loop a long play session performs. */
    card[0x2000 + 0x28] ^= 0x5A; /* a save's modification time */
    rc = ss_push(&client, id, 0, card, (uint32_t)len, assigned, assigned + 1, bitmap,
                 sizeof(bitmap), &head);
    check(rc == SS_OK, "a second push chains off the first");
    if (rc == SS_OK) {
        printf("  pushed again as v%u\n", head);
        check(head > assigned, "and moves it on again");
    }

    /* The case that matters most. Push naming a parent that is no longer head:
     * the server must refuse, and the client must report the refusal rather
     * than retry with head as the parent. PLAN.md section 7. */
    card[0x2000 + 0x28] ^= 0x31;
    rc = ss_push(&client, id, 0, card, (uint32_t)len, parent, parent + 99, bitmap,
                 sizeof(bitmap), &assigned);
    check(rc == SS_ERR_CONFLICT, "a stale parent is refused, not merged");
    check(client.last_head == head, "and the refusal names the real head");
    if (rc == SS_ERR_CONFLICT) {
        printf("  conflict reported: server is on v%u, as it should be\n",
               client.last_head);
    } else {
        printf("  got %s instead of a conflict\n", ss_strerror(rc));
    }

    /* Pull it back and check the server holds what we sent. */
    {
        unsigned char *back = (unsigned char *)malloc((size_t)len);
        uint32_t got_size = 0, got_version = 0;
        uint8_t a[SHA256_DIGEST_SIZE], b[SHA256_DIGEST_SIZE];

        card[0x2000 + 0x28] ^= 0x31; /* undo the conflict edit */
        rc = ss_pull(&client, id, 0, 0, back, (size_t)len, bitmap, sizeof(bitmap),
                     &got_size, &got_version);
        check(rc == SS_OK, "the card pulls back");
        if (rc == SS_OK) {
            check(got_size == (uint32_t)len, "at the size we pushed");
            check(got_version == head, "at the version we were assigned");
            sha256(card, (size_t)len, a);
            sha256(back, got_size, b);
            check(memcmp(a, b, SHA256_DIGEST_SIZE) == 0,
                  "byte for byte what the console holds");
        } else {
            printf("  %s\n", ss_strerror(rc));
        }
        free(back);
    }

    /* Delta push, driven exactly the way ss_push_slot drives it: scan the card
     * against the fingerprint table, then hand the bitmap to ss_push_delta.
     *
     * By this point `card` is byte for byte what the server holds at v`head`,
     * which is the state SlotSync.c's fp_valid stands for. */
    {
        static uint32_t fp[2048];
        static uint8_t dirty[2048];
        uint32_t chunks = ss_chunk_count((uint32_t)len);
        unsigned before;
        int32_t marked;

        memset(fp, 0, sizeof(fp));
        marked = ss_fp_scan(card, (uint32_t)len, fp, 2048, dirty, sizeof(dirty));
        check(marked >= 0, "the fingerprint table covers this card");

        /* One save write: the file's own data, and the directory block the
         * card keeps its entries in. */
        card[0x2000 + 0x28] ^= 0x11; /* a directory entry's timestamp */
        card[len / 2] ^= 0x77;       /* a data block in the middle */
        marked = ss_fp_scan(card, (uint32_t)len, fp, 2048, dirty, sizeof(dirty));
        check(marked == 16, "a save write dirties two blocks, so sixteen chunks");
        check(ss_fp_worthwhile((uint32_t)marked, chunks),
              "which is well worth a delta");

        parent = head;
        before = sock.sent;
        rc = ss_push_delta(&client, id, 0, card, (uint32_t)len, parent, parent + 1,
                           dirty, bitmap, sizeof(bitmap), &head);
        check(rc == SS_OK, "a delta push is accepted");
        if (rc != SS_OK) {
            printf("  %s (nack 0x%02x)\n", ss_strerror(rc), client.last_error_code);
        } else {
            check(head == parent + 1, "and moves the lineage on by one");
            check(sock.sent - before < chunks / 4u,
                  "sending a small fraction of the card");
            printf("  delta: %u datagrams for a %u-chunk card, now v%u\n",
                   sock.sent - before, chunks, head);
        }

        /* The assertion the whole feature rests on. The server spliced our
         * sixteen chunks onto its own copy of v%u; if its copy was not the one
         * we built the delta against, this is where it shows. */
        if (rc == SS_OK) {
            unsigned char *back = (unsigned char *)malloc((size_t)len);
            uint32_t got_size = 0, got_version = 0;

            if (back == NULL) {
                printf("  FAIL  out of memory\n");
                failures++;
            } else {
                rc = ss_pull(&client, id, 0, 0, back, (size_t)len, bitmap,
                             sizeof(bitmap), &got_size, &got_version);
                check(rc == SS_OK, "the delta result pulls back");
                if (rc == SS_OK) {
                    check(got_size == (uint32_t)len, "at the right size");
                    check(memcmp(card, back, (size_t)len) == 0,
                          "and the server assembled the card byte for byte");
                }
                free(back);
            }
        }
    }

done:
    host_socket_close(&sock);
    host_socket_cleanup();
    free(card);
}

int main(int argc, char **argv)
{
    const char *host = NULL;
    const char *psk = "changeme";
    const char *card_path = NULL;
    unsigned short port = 9977;
    /* The kernel's defaults, so a live run offers the load a console offers. */
    unsigned pace_every = 8;
    unsigned pace_us = 5000;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--server") == 0 && i + 2 < argc) {
            host = argv[++i];
            port = (unsigned short)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--psk") == 0 && i + 1 < argc) {
            psk = argv[++i];
        } else if (strcmp(argv[i], "--card") == 0 && i + 1 < argc) {
            card_path = argv[++i];
        } else if (strcmp(argv[i], "--pace") == 0 && i + 2 < argc) {
            pace_every = (unsigned)atoi(argv[++i]);
            pace_us = (unsigned)atoi(argv[++i]);
        } else {
            printf("usage: %s [--server HOST PORT] [--psk KEY] [--card FILE]"
                   " [--pace EVERY US]\n", argv[0]);
            return 2;
        }
    }

    test_config();
    test_ipv4();
    test_state();
    test_game_id(card_path);
    test_should_push();
    test_fingerprint();
    test_delta_scan();
    test_kernel_store_walk();

    if (host != NULL) {
        if (card_path == NULL) {
            printf("\n--server needs --card too\n");
            return 2;
        }
        live(host, port, psk, card_path, pace_every, pace_us);
    } else {
        printf("\n(no --server: skipping the live round trip)\n");
    }

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
