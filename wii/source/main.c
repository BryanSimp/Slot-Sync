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
#include "wii_drc.h"
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
    /* Answers false on a real Wii, where there is no GamePad to find. Asking
     * costs one pattern match against IOS's memory. */
    WiiDRC_Init();

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

/* What the person in front of the television just pressed.
 *
 * Four channels rather than one: a Wii Remote does not reliably come up as
 * player one, and on a Wii U it usually does not. The Classic Controller masks
 * are how libogc reports a Classic -- and how the Wii U GamePad arrives when
 * this runs as an injected Wii U title, which is the only way the GamePad is an
 * input device at all on that console. They share the button word with the
 * remote's own buttons, in its top sixteen bits, so one read covers both.
 *
 * A GameCube pad has no HOME, so START stands in for it.
 *
 * The Wii U GamePad is a third thing again, and neither WPAD nor PAD can see
 * it: in vWii it is not a Wii input device at all, and its state lives in IOS's
 * memory rather than on any bus libogc talks to. Launched from a Wii U menu
 * channel that is the only controller in the room, so without wii_drc.c the
 * launcher looks dead while Nintendont, which has always read it, works fine.
 *
 * GameCube controllers on a Wii U are a fourth case and are still not handled:
 * vWii has no controller ports, so a pad on the official USB adapter reaches
 * Nintendont through its own USB HID stack and nothing here. Use the GamePad or
 * a Wii Remote. */
enum { BTN_A = 1, BTN_B = 2, BTN_EXIT = 4 };

static unsigned buttons_down(void)
{
    unsigned out = 0;
    int chan;

    WPAD_ScanPads();
    PAD_ScanPads();

    if (WiiDRC_Inited() && WiiDRC_Connected()) {
        u32 drc;

        WiiDRC_ScanPads();
        drc = WiiDRC_ButtonsDown();

        if (drc & WIIDRC_BUTTON_A) {
            out |= BTN_A;
        }
        if (drc & WIIDRC_BUTTON_B) {
            out |= BTN_B;
        }
        if (drc & WIIDRC_BUTTON_HOME) {
            out |= BTN_EXIT;
        }
    }

    for (chan = 0; chan < 4; chan++) {
        u32 wii = WPAD_ButtonsDown(chan);
        u16 gcn = PAD_ButtonsDown(chan);

        if ((wii & (WPAD_BUTTON_A | WPAD_CLASSIC_BUTTON_A)) || (gcn & PAD_BUTTON_A)) {
            out |= BTN_A;
        }
        if ((wii & (WPAD_BUTTON_B | WPAD_CLASSIC_BUTTON_B)) || (gcn & PAD_BUTTON_B)) {
            out |= BTN_B;
        }
        if ((wii & (WPAD_BUTTON_HOME | WPAD_CLASSIC_BUTTON_HOME))
            || (gcn & PAD_BUTTON_START)) {
            out |= BTN_EXIT;
        }
    }
    return out;
}

/* A or B. Returns 1 for A, 0 for B; HOME/START still leaves.
 *
 * Only used for a conflict, which PLAN.md section 7 says is a human's call.
 * The console is the only place that human is standing when it happens.
 *
 * `timeout_ms` of 0 waits for ever. Anything else answers B when it runs out,
 * because there is a real case where nobody can answer at all: launched from a
 * Wii U menu channel the console is in vWii, where the GamePad is not an input
 * device, and a prompt with no reachable button is indistinguishable from a
 * hang. B is the safe side of that -- it keeps the card that is here and leaves
 * the conflict standing for the web UI. Timing out into A would overwrite a
 * card because nobody was holding a Wii Remote, which is the whole failure this
 * project exists to prevent. */
static int ask_a_or_b(int timeout_ms)
{
    u64 started = gettime();
    int last_left = -1;

    for (;;) {
        unsigned pressed = buttons_down();

        if (pressed & BTN_A) {
            return 1;
        }
        if (pressed & BTN_B) {
            return 0;
        }
        if (pressed & BTN_EXIT) {
            printf("returning to the loader\n");
            exit(0);
        }

        if (timeout_ms > 0) {
            int gone = (int)ticks_to_millisecs(diff_ticks(started, gettime()));
            int left = (timeout_ms - gone + 999) / 1000;

            if (left <= 0) {
                printf("\r         no answer -- keeping this card%*s\n", 20, "");
                return 0;
            }
            /* Redraw only when the second changes: this console's console
             * output is a framebuffer, and printing every vsync is visible. */
            if (left != last_left) {
                last_left = left;
                printf("\r         B in %d s unless you choose ", left);
            }
        }
        VIDEO_WaitVSync();
    }
}

static void wait_for_button(const char *prompt)
{
    printf("\n%s\n", prompt);
    for (;;) {
        unsigned pressed = buttons_down();

        if (pressed & BTN_A) {
            return;
        }
        if (pressed & BTN_EXIT) {
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
/* `stem` is the file name on the SD card, which is four characters for a card
 * Nintendont wrote. `game_id` is the six the server keys by. They are not the
 * same string and must not be used interchangeably: the file lives under the
 * stem, every protocol call takes the id. */
static int sync_one(ss_client *client, const wii_config *cfg, wii_state *state,
                    const char *stem, uint8_t *card, uint8_t *bitmap)
{
    char game_id[WII_GAME_ID_LEN + 1];
    wii_card_state *entry;
    uint8_t digest[SHA256_DIGEST_SIZE];
    uint32_t server_version = 0;
    uint32_t server_size = 0;
    long size;
    int rc;

    size = wii_saves_read(cfg->saves_dir, stem, card, CARD_BUFFER_BYTES);
    if (size <= 0) {
        printf("  %s: cannot read the card\n", stem);
        return -1;
    }

    /* The maker code has to come off the card, and a card with nothing saved
     * on it yet has no entry to read it from. Pushing it space-padded would
     * key a different card on the server than Dolphin uses for the same game
     * and split one lineage in two, so it waits for a first save instead. */
    if (!wii_saves_game_id(card, size, stem, game_id)) {
        printf("  %s: nothing saved on it yet, so nothing identifies it\n",
               stem);
        return 0;
    }

    entry = wii_state_get(state, game_id, 0);
    if (entry == NULL) {
        printf("  %s: too many cards to track\n", game_id);
        return -1;
    }

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
                /* Someone else moved this card on while we were playing.
                 * Never decide this here -- but the person who can decide
                 * is standing in front of the console, so ask them rather
                 * than making them go to a web UI to do the obvious thing.
                 *
                 * Taking the server's version replaces a card with local
                 * play in it, so it is copied aside first and the pull is
                 * abandoned if that copy cannot be written. */
                printf("  %s: CONFLICT -- server is on v%u, yours came from v%u\n",
                       game_id, client->last_head, entry->version);
                printf("         A: take the server's v%u (this card -> %s.raw.bak)\n",
                       client->last_head, stem);
                printf("         B: keep this card, decide later in the web UI\n");

                if (!ask_a_or_b(cfg->conflict_timeout_ms)) {
                    printf("  %s: kept yours; nothing sent\n", game_id);
                    return -1;
                }
                if (wii_saves_backup(cfg->saves_dir, stem, card,
                                     (size_t)size) != 0) {
                    printf("  %s: could not write the backup -- keeping yours\n",
                           game_id);
                    return -1;
                }
                printf("  %s: backed up to %s.raw.bak\n", game_id, stem);
                /* Fall through to the pull below, which is already written. */
                server_version = client->last_head;
            } else if (rc != SS_OK) {
                printf("  %s: push failed: %s\n", game_id, ss_strerror(rc));
                return -1;
            } else {
                /* Only when the push actually landed. On a conflict
                 * `assigned` is zero and server_version already holds the
                 * head we are about to pull. */
                entry->version = assigned;
                entry->known = 1;
                memcpy(entry->sha256, digest, SHA256_DIGEST_SIZE);
                server_version = assigned;
                printf("  %s: pushed as v%u\n", game_id, assigned);
            }
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
        if (wii_saves_write(cfg->saves_dir, stem, card, got_size) != 0) {
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
        printf("no card images in %s yet\n", cfg->saves_dir);
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
    rc = wii_net_init(ip, (int)sizeof(ip));
    if (rc != 0) {
        printf("network  FAILED (%d) -- check the Wii's internet settings\n", rc);
        wait_for_button("Press A to boot Nintendont without syncing.");
        wii_dol_run(cfg.nintendont, argc, argv);
        return 1;
    }
    printf("network  %s\n\n", ip);

    rc = wii_net_open(&sock, cfg.server, cfg.port);
    if (rc != 0) {
        /* Name the code. -6 is ENXIO from net_socket, meaning the IOS
         * socket driver never opened -- a different problem from the
         * server being unreachable, and the two printed the same line. */
        if (rc == WII_NET_EBADADDR) {
            printf("server \"%s\" is not a dotted quad\n", cfg.server);
        } else if (rc == -6) {
            printf("no socket: the IOS network driver did not open (-6)\n");
        } else {
            printf("cannot open a socket to %s (%d)\n", cfg.server, rc);
        }
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
    client.pull_window = cfg.pull_window;
    sock.pace_every = cfg.pace_every;
    sock.pace_us = cfg.pace_us;

    rc = ss_hello(&client, &server_time, &server_version);
    if (rc != SS_OK) {
        printf("server did not answer: %s\n", ss_strerror(rc));
        /* Which direction failed, and with what. SS_ERR_TRANSPORT covers
         * both a send and a receive failing, and they are different bugs. */
        if (sock.last_op != 0) {
            printf("  last %s failed with %d\n",
                   sock.last_op == 's' ? "send" :
                   sock.last_op == 'p' ? "poll" : "recv", sock.last_err);
        }
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
    {
        /* Pick up anything Nintendont's in-kernel sync pushed while the last
         * game was running, so we name the right parent below. */
        int merged = wii_state_merge_runtime(&state, WII_RUNTIME_PATH);
        if (merged > 0) {
            printf("runtime sync moved %d card(s) on during the last game\n\n",
                   merged);
            wii_state_save(&state, WII_STATE_PATH);
        }
    }
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
