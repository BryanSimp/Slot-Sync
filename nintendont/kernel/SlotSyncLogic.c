/* See SlotSyncLogic.h. */

#include "SlotSyncLogic.h"

#include <string.h>

/* Directory block offsets, from docs/MEMCARD.md -- verified against Dolphin's
 * GCMemcard.cpp rather than recalled. */
#define SSL_DIR_OFFSET   0x2000
#define SSL_DIR_ENTRIES  127
#define SSL_DENTRY_SIZE  0x40

static int ssl_is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

uint64_t ssl_parse_u64(const char *s, const char **end)
{
    uint64_t value = 0;
    int base = 10;

    while (*s == ' ' || *s == '\t') {
        s++;
    }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    }
    for (;;) {
        int digit;
        char c = *s;

        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (base == 16 && c >= 'a' && c <= 'f') {
            digit = c - 'a' + 10;
        } else if (base == 16 && c >= 'A' && c <= 'F') {
            digit = c - 'A' + 10;
        } else {
            break;
        }
        value = value * (uint64_t)base + (uint64_t)digit;
        s++;
    }
    if (end != NULL) {
        *end = s;
    }
    return value;
}

int ssl_parse_ipv4(const char *text, uint32_t *out)
{
    uint32_t addr = 0;
    int octet = 0;
    int digits = 0;
    int value = 0;

    for (;;) {
        char c = *text++;

        if (c >= '0' && c <= '9') {
            value = value * 10 + (c - '0');
            if (value > 255 || ++digits > 3) {
                return -1;
            }
        } else if (c == '.' || c == '\0') {
            if (digits == 0 || octet > 3) {
                return -1;
            }
            addr = (addr << 8) | (uint32_t)value;
            octet++;
            value = 0;
            digits = 0;
            if (c == '\0') {
                break;
            }
        } else {
            return -1;
        }
    }

    if (octet != 4) {
        return -1;
    }
    *out = addr;
    return 0;
}

static const char *ssl_next_line(const char *p)
{
    while (*p != '\0' && *p != '\n') {
        p++;
    }
    return (*p == '\n') ? p + 1 : p;
}

/* If `line` starts with `key` followed by `=`, return the value. A key must be
 * followed by space or '=', so `port` does not match a line reading
 * `port_forward = 1`. */
static const char *ssl_match_key(const char *line, const char *key)
{
    size_t i;

    while (*line == ' ' || *line == '\t') {
        line++;
    }
    for (i = 0; key[i] != '\0'; i++) {
        if (line[i] != key[i]) {
            return NULL;
        }
    }
    line += i;
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    if (*line != '=') {
        return NULL;
    }
    line++;
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    return line;
}

static void ssl_copy_value(const char *value, char *out, size_t cap)
{
    size_t n = 0;

    while (value[n] != '\0' && value[n] != '\n' && value[n] != '\r'
           && n < cap - 1) {
        n++;
    }
    while (n > 0 && ssl_is_space(value[n - 1])) {
        n--;
    }
    memcpy(out, value, n);
    out[n] = '\0';
}

static uint32_t ssl_clamp_ms(uint64_t value, uint32_t fallback)
{
    if (value == 0) {
        return fallback;
    }
    if (value > SSL_MAX_DEADLINE_MS) {
        return SSL_MAX_DEADLINE_MS;
    }
    return (uint32_t)value;
}

void ssl_config_defaults(ssl_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->enabled = 1;
    cfg->port = 9977;
    cfg->timeout_ms = 2000;
    cfg->rounds = 12;
    /* Long enough that the several writes a game makes while saving land as
     * one push, short enough that a save reaches the server while you are
     * still on the results screen. */
    cfg->quiet_ms = 4000;
    cfg->cooldown_ms = 30000;
    cfg->net_timeout_ms = 20000;
    /* Datagrams per pause, and how long to pause. 8 per 5 ms is 1600/sec,
     * which sits under the server's own default UDP rate limit of 2048/sec
     * (SLOTSYNC_UDP_RATE) with room to spare. Going faster does not make a
     * card arrive sooner: it makes the server drop chunks and the client
     * retransmit them. Raise both only alongside the server's limit. */
    cfg->pace_every = 8;
    cfg->pace_us = 5000;
}

int ssl_config_parse(const char *text, ssl_config *cfg)
{
    const char *p;
    char scratch[SSL_PSK_MAX];

    ssl_config_defaults(cfg);

    for (p = text; *p != '\0'; p = ssl_next_line(p)) {
        const char *v;

        if ((v = ssl_match_key(p, "server")) != NULL) {
            ssl_copy_value(v, scratch, sizeof(scratch));
            if (ssl_parse_ipv4(scratch, &cfg->server) == 0) {
                cfg->have_server = 1;
            }
        } else if ((v = ssl_match_key(p, "port")) != NULL) {
            cfg->port = (uint16_t)ssl_parse_u64(v, NULL);
        } else if ((v = ssl_match_key(p, "psk")) != NULL) {
            ssl_copy_value(v, cfg->psk, sizeof(cfg->psk));
            cfg->psk_len = (uint32_t)strlen(cfg->psk);
        } else if ((v = ssl_match_key(p, "device_id")) != NULL) {
            cfg->device_id = ssl_parse_u64(v, NULL);
        } else if ((v = ssl_match_key(p, "timeout_ms")) != NULL) {
            cfg->timeout_ms = (int)ssl_clamp_ms(ssl_parse_u64(v, NULL), 2000);
        } else if ((v = ssl_match_key(p, "rounds")) != NULL) {
            cfg->rounds = (int)ssl_parse_u64(v, NULL);
        } else if ((v = ssl_match_key(p, "runtime_sync")) != NULL) {
            cfg->enabled = (int)ssl_parse_u64(v, NULL);
        } else if ((v = ssl_match_key(p, "runtime_quiet_ms")) != NULL) {
            cfg->quiet_ms = ssl_clamp_ms(ssl_parse_u64(v, NULL), 4000);
        } else if ((v = ssl_match_key(p, "runtime_cooldown_ms")) != NULL) {
            cfg->cooldown_ms = ssl_clamp_ms(ssl_parse_u64(v, NULL), 30000);
        } else if ((v = ssl_match_key(p, "runtime_net_timeout_ms")) != NULL) {
            cfg->net_timeout_ms = ssl_clamp_ms(ssl_parse_u64(v, NULL), 20000);
        } else if ((v = ssl_match_key(p, "runtime_pace_every")) != NULL) {
            cfg->pace_every = (uint32_t)ssl_parse_u64(v, NULL);
        } else if ((v = ssl_match_key(p, "runtime_pace_us")) != NULL) {
            cfg->pace_us = (uint32_t)ssl_parse_u64(v, NULL);
        }
    }

    if (cfg->rounds <= 0) {
        cfg->rounds = 12;
    }
    if (!cfg->enabled || !cfg->have_server || cfg->psk_len == 0) {
        return -1;
    }
    return 0;
}

int ssl_state_find(const char *text, const char *game_id, uint8_t slot,
                   uint32_t *out)
{
    const char *p;

    for (p = text; *p != '\0'; p = ssl_next_line(p)) {
        const char *cursor = p;
        uint64_t line_slot;
        int i;

        while (*cursor == ' ' || *cursor == '\t') {
            cursor++;
        }
        for (i = 0; i < SSL_GAME_ID_LEN; i++) {
            if (cursor[i] != game_id[i]) {
                break;
            }
        }
        if (i != SSL_GAME_ID_LEN || !ssl_is_space(cursor[SSL_GAME_ID_LEN])) {
            continue;
        }
        cursor += SSL_GAME_ID_LEN;

        line_slot = ssl_parse_u64(cursor, &cursor);
        if ((uint8_t)line_slot != slot) {
            continue;
        }
        *out = (uint32_t)ssl_parse_u64(cursor, &cursor);
        return 0;
    }
    return -1;
}

int ssl_game_id(const uint8_t *card, uint32_t card_size, uint32_t nin_game_id,
                char out[SSL_GAME_ID_LEN + 1])
{
    int i;

    out[0] = (char)((nin_game_id >> 24) & 0xFF);
    out[1] = (char)((nin_game_id >> 16) & 0xFF);
    out[2] = (char)((nin_game_id >> 8) & 0xFF);
    out[3] = (char)(nin_game_id & 0xFF);
    out[4] = ' ';
    out[5] = ' ';
    out[6] = '\0';

    /* Need at least the header and directory blocks to look at. */
    if (card == NULL || card_size < SSL_DIR_OFFSET + 0x2000) {
        return 0;
    }

    for (i = 0; i < SSL_DIR_ENTRIES; i++) {
        const uint8_t *entry = card + SSL_DIR_OFFSET + (uint32_t)i * SSL_DENTRY_SIZE;

        /* FF FF FF FF marks an unused entry. */
        if (entry[0] == 0xFF && entry[1] == 0xFF && entry[2] == 0xFF
            && entry[3] == 0xFF) {
            continue;
        }
        /* Another game's save, on a card shared between games. */
        if (memcmp(entry, out, 4) != 0) {
            continue;
        }
        out[4] = (char)entry[4];
        out[5] = (char)entry[5];
        return 1;
    }
    return 0;
}

int ssl_should_push(int dirty, int halted, uint32_t since_dirty, uint32_t since_push,
                    int pushed_ever, uint32_t quiet_ticks, uint32_t cooldown_ticks)
{
    if (halted || !dirty) {
        return 0;
    }
    /* Wait for the card to go quiet. A game writing a save touches it several
     * times in a row, and pushing between those writes only means pushing a
     * torn image for the server to reject. */
    if (since_dirty < quiet_ticks) {
        return 0;
    }
    if (pushed_ever && since_push < cooldown_ticks) {
        return 0;
    }
    return 1;
}
