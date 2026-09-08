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
    check(cfg.pace_every == 8, "pacing defaults on");
    /* The default must stay under the server's own UDP rate limit, which
     * defaults to 2048 datagrams/sec (SLOTSYNC_UDP_RATE). Sending faster does
     * not deliver a card sooner, it just gets chunks dropped. */
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

    check(ssl_state_find(state, "GALE01", 0, &version) == 0 && version == 7,
          "slot A version");
    check(ssl_state_find(state, "GALE01", 1, &version) == 0 && version == 3,
          "slot B of the same game is a different card");
    check(ssl_state_find(state, "GM4E01", 0, &version) == 0 && version == 12,
          "a later line");
    check(ssl_state_find(state, "GZLE01", 0, &version) != 0, "an unknown card");
    check(ssl_state_find("", "GALE01", 0, &version) != 0, "an empty state file");

    /* A six-character ID must match all six. "GALE0" is a prefix of the first
     * line's ID and must not match it. */
    check(ssl_state_find(state, "GALE0 ", 0, &version) != 0,
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
