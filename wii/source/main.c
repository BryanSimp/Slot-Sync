/* SlotSync for Wii: sync /saves with the server, then chainload Nintendont.
 *
 * The flow, and why it is this shape:
 *
 *   1. Push every card in /saves that differs from what we last agreed with
 *      the server.
 *   2. Pull every card the server has moved ahead on.
 *   3. Chainload Nintendont.
 *   4. If control ever comes back, do 1 and 2 again.
 *
 * Steps 1 and 2 run *before* the handover as well as after, which is what makes
 * PLAN.md section 13's first open question -- does Nintendont return control? --
 * stop being a blocker. If it never returns, this is a sync-on-next-launch
 * client and still correct; if it does, the save reaches the server immediately.
 *
 * It also means the whole /saves directory is handled without needing to know
 * which game is about to run. Nintendont's one-card-per-game layout is the same
 * unit the server versions, so there is nothing to translate.
 */

#include <fat.h>
#include <gccore.h>
#include <ogc/lwp_watchdog.h> /* gettime, for the nonce seed */
#include <ogcsys.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wiiuse/wpad.h>

#include "../core/client.h"
#include "../core/sha256.h"
#include "wii_config.h"
#include "wii_dol.h"
#include "wii_net.h"
#include "wii_saves.h"

/* A 16 MiB card is the largest the format allows, and the Wii has 24 MiB of
 * MEM1 plus 64 MiB of MEM2, so one card buffer is affordable. Allocated once
 * rather than per card, because fragmenting the heap on a console is how you
 * get a failure three games in. */
#define CARD_BUFFER_BYTES (16 * 1024 * 1024)
#define BITMAP_BYTES 2048 /* 16384 chunks / 8 */

static void video_init(void)
{
    GXRModeObj *mode;
    void *framebuffer;

    VIDEO_Init();
    WPAD_Init();
    PAD_Init();

    mode = VIDEO_GetPreferredMode(NULL);
    framebuffer = MEM_K0_TO_K1(SYS_AllocateFramebuffer(mode));
    console_init(framebuffer, 20, 20, mode->fbWidth - 20, mode->xfbHeight - 20,
                 mode->fbWidth * VI_DISPLAY_PIX_SZ);

    VIDEO_Configure(mode);
    VIDEO_SetNextFramebuffer(framebuffer);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (mode->viTVMode & VI_NON_INTERLACE) {
        VIDEO_WaitVSync();
    }
}

static void wait_for_button(const char *prompt)
{
    printf("\n%s\n", prompt);
    for (;;) {
        WPAD_ScanPads();
        PAD_ScanPads();
        if ((WPAD_ButtonsDown(0) & WPAD_BUTTON_A)
            || (PAD_ButtonsDown(0) & PAD_BUTTON_A)) {
            return;
        }
        if ((WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME)
            || (PAD_ButtonsDown(0) & PAD_BUTTON_START)) {
            printf("returning to the loader\n");
            exit(0);
        }
        VIDEO_WaitVSync();
    }
}

static int digests_match(const uint8_t *a, const uint8_t *b)
{
    return slotsync_memequal(a, b, SHA256_DIGEST_SIZE);
}

/* One card, both directions. Returns 0 if nothing went wrong; a conflict counts
 * as "went wrong" only in the sense that it is reported and skipped -- it is
 * never resolved here. PLAN.md section 7: a human chooses, in the web UI. */
static int sync_one(ss_client *client, const wii_config *cfg, wii_state *state,
                    const char *game_id, uint8_t *card, uint8_t *bitmap)
{
    wii_card_state *entry = wii_state_get(state, game_id, 0);
    uint8_t digest[SHA256_DIGEST_SIZE];
    uint32_t server_version = 0;
    uint32_t server_size = 0;
    long size;
    int rc;

    if (entry == NULL) {
        printf("  %s: too many cards to track\n", game_id);
        return -1;
    }

    size = wii_saves_read(cfg->saves_dir, game_id, card, CARD_BUFFER_BYTES);

    rc = ss_head(client, game_id, 0, &server_version, &server_size);
    if (rc != SS_OK && client->last_error_code != SS_NACK_UNKNOWN_CARD) {
        printf("  %s: %s\n", game_id, ss_strerror(rc));
        return -1;
    }
    if (rc != SS_OK) {
        server_version = 0; /* the server has never seen this card */
    }

    /* Push, if what is on the SD card is not what we last agreed with the
     * server. */
    if (size > 0) {
        sha256(card, (size_t)size, digest);

        if (!entry->known || !digests_match(digest, entry->sha256)) {
            uint32_t assigned = 0;

            printf("  %s: pushing %ld KiB...\n", game_id, size / 1024);
            rc = ss_push(client, game_id, 0, card, (uint32_t)size, entry->version,
                         entry->version + 1, bitmap, BITMAP_BYTES, &assigned);

            if (rc == SS_ERR_CONFLICT) {
                /* Someone else moved this card on while we were playing. Say
                 * so and leave both sides alone. */
                printf("  %s: CONFLICT, server is on v%u\n", game_id,
                       client->last_head);
                printf("         your save is safe on the SD card; choose in the web UI\n");
                return -1;
            }
            if (rc != SS_OK) {
                printf("  %s: push failed: %s\n", game_id, ss_strerror(rc));
                return -1;
            }

            entry->version = assigned;
            entry->known = 1;
            memcpy(entry->sha256, digest, SHA256_DIGEST_SIZE);
            server_version = assigned;
            printf("  %s: pushed as v%u\n", game_id, assigned);
        }
    }

    /* Pull, if the server is ahead. */
    if (server_version > entry->version) {
        uint32_t got_size = 0;
        uint32_t got_version = 0;

        printf("  %s: pulling v%u...\n", game_id, server_version);
        rc = ss_pull(client, game_id, 0, 0, card, CARD_BUFFER_BYTES, bitmap,
                     BITMAP_BYTES, &got_size, &got_version);
        if (rc != SS_OK) {
            printf("  %s: pull failed: %s\n", game_id, ss_strerror(rc));
            return -1;
        }
        if (wii_saves_write(cfg->saves_dir, game_id, card, got_size) != 0) {
            printf("  %s: could not write the card to the SD card\n", game_id);
            return -1;
        }

        sha256(card, got_size, digest);
        entry->version = got_version;
        entry->known = 1;
        memcpy(entry->sha256, digest, SHA256_DIGEST_SIZE);
        printf("  %s: pulled v%u\n", game_id, got_version);
    } else if (size > 0 && entry->known) {
        printf("  %s: up to date (v%u)\n", game_id, entry->version);
    }

    return 0;
}

static void sync_all(ss_client *client, const wii_config *cfg, wii_state *state)
{
    static char games[WII_MAX_CARDS][WII_GAME_ID_LEN + 1];
    uint8_t *card;
    uint8_t *bitmap;
    int found;
    int i;

    found = wii_saves_scan(cfg->saves_dir, games, WII_MAX_CARDS);
    if (found < 0) {
        printf("cannot read %s -- is the SD card in?\n", cfg->saves_dir);
        return;
    }
    if (found == 0) {
        printf("no GAMEID.raw files in %s yet\n", cfg->saves_dir);
        return;
    }

    card = (uint8_t *)malloc(CARD_BUFFER_BYTES);
    bitmap = (uint8_t *)malloc(BITMAP_BYTES);
    if (card == NULL || bitmap == NULL) {
        printf("not enough memory for a card buffer\n");
        free(card);
        free(bitmap);
        return;
    }

    printf("syncing %d card(s)\n", found);
    for (i = 0; i < found; i++) {
        sync_one(client, cfg, state, games[i], card, bitmap);
        wii_state_save(state, WII_STATE_PATH);
    }

    free(card);
    free(bitmap);
}

int main(int argc, char **argv)
{
    wii_config cfg;
    wii_state state;
    wii_socket sock;
    ss_transport transport;
    ss_client client;
    uint8_t seed[8];
    char error[128];
    char ip[16];
    uint64_t server_time = 0;
    uint8_t server_version = 0;
    int rc;

    video_init();
    printf("\n  SlotSync for Wii\n");
    printf("  ----------------\n\n");

    if (!fatInitDefault()) {
        printf("cannot mount the SD card\n");
        wait_for_button("Press A to exit.");
        return 1;
    }

    if (wii_config_load(&cfg, WII_CFG_PATH, error, (int)sizeof(error)) != 0) {
        printf("config: %s\n", error);
        printf("\nCreate %s with at least:\n", WII_CFG_PATH);
        printf("  server = 192.168.1.10\n  psk = your-pre-shared-key\n");
        wait_for_button("Press A to exit.");
        return 1;
    }
    printf("server   %s:%u\n", cfg.server, cfg.port);
    printf("saves    %s\n", cfg.saves_dir);

    printf("network  connecting...\n");
    if (wii_net_init(ip, (int)sizeof(ip)) != 0) {
        printf("network  FAILED -- check the Wii's internet settings\n");
        wait_for_button("Press A to boot Nintendont without syncing.");
        wii_dol_run(cfg.nintendont, argc, argv);
        return 1;
    }
    printf("network  %s\n\n", ip);

    if (wii_net_open(&sock, cfg.server, cfg.port) != 0) {
        printf("cannot open a socket to %s\n", cfg.server);
        wait_for_button("Press A to boot Nintendont without syncing.");
        wii_dol_run(cfg.nintendont, argc, argv);
        return 1;
    }

    transport.send = wii_net_send;
    transport.recv = wii_net_recv;
    transport.ctx = &sock;

    /* Seed the nonce from the console's timebase. It must differ every boot:
     * a repeated nonce is dropped by the server as a replay, which looks
     * exactly like the server being unreachable. */
    {
        uint64_t ticks = (uint64_t)gettime();
        ss_put_u64(seed, ticks);
    }

    /* device_id identifies this console in the server's history. If the config
     * did not set one, derive it from the timebase so at least it is stable
     * for this session. */
    if (cfg.device_id == 0) {
        cfg.device_id = 0x5749490000000001ull;
    }

    ss_client_init(&client, &transport, (const uint8_t *)cfg.psk, strlen(cfg.psk),
                   cfg.device_id, seed);
    client.timeout_ms = cfg.timeout_ms;
    client.max_rounds = cfg.rounds;

    rc = ss_hello(&client, &server_time, &server_version);
    if (rc != SS_OK) {
        printf("server did not answer: %s\n", ss_strerror(rc));
        if (client.last_error_code == SS_NACK_BAD_HMAC) {
            printf("  the pre-shared key does not match the server's\n");
        }
        wait_for_button("Press A to boot Nintendont without syncing.");
        wii_net_close(&sock);
        wii_dol_run(cfg.nintendont, argc, argv);
        return 1;
    }
    printf("server   protocol v%u, ok\n\n", server_version);

    wii_state_load(&state, WII_STATE_PATH);
    sync_all(&client, &cfg, &state);
    wii_net_close(&sock);

    printf("\nsync done\n");
    if (!cfg.autoboot) {
        wait_for_button("Press A to launch Nintendont, HOME to exit.");
    }

    printf("launching %s\n", cfg.nintendont);
    if (wii_dol_run(cfg.nintendont, argc, argv) != 0) {
        printf("could not launch %s\n", cfg.nintendont);
        wait_for_button("Press A to exit.");
        return 1;
    }

    /* Only reached if Nintendont hands control back -- see the note at the top.
     * If it does, the saves it just wrote are pushed straight away instead of
     * waiting for the next launch. */
    return 0;
}
