/* See wii_config.h. */

#include "wii_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/client.h"   /* SS_PULL_WINDOW, so the default lives in one place */

void wii_config_defaults(wii_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->port = 9977;
    cfg->timeout_ms = 2000;
    cfg->rounds = 12;
    cfg->conflict_timeout_ms = 30000;
    cfg->pull_window = SS_PULL_WINDOW;
    cfg->pace_every = 16;
    cfg->pace_us = 20000;
    cfg->autoboot = 1;
    strcpy(cfg->saves_dir, WII_SAVES_DIR);
    strcpy(cfg->nintendont, "sd:/apps/Nintendont/boot.dol");
}

static char *trim(char *s)
{
    char *end;

    while (*s == ' ' || *s == '\t') {
        s++;
    }
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'
                       || end[-1] == '\n')) {
        *--end = '\0';
    }
    return s;
}

static void copy_str(char *dest, const char *src, int cap)
{
    int len = (int)strlen(src);
    if (len >= cap) {
        len = cap - 1;
    }
    memcpy(dest, src, (size_t)len);
    dest[len] = '\0';
}

int wii_config_load(wii_config *cfg, const char *path, char *error, int error_cap)
{
    FILE *file;
    char line[256];

    wii_config_defaults(cfg);

    file = fopen(path, "r");
    if (file == NULL) {
        snprintf(error, (size_t)error_cap, "cannot open %s", path);
        return -1;
    }

    while (fgets(line, (int)sizeof(line), file) != NULL) {
        char *key;
        char *value;
        char *equals;

        key = trim(line);
        if (*key == '\0' || *key == '#' || *key == ';') {
            continue;
        }
        equals = strchr(key, '=');
        if (equals == NULL) {
            continue;
        }
        *equals = '\0';
        value = trim(equals + 1);
        key = trim(key);

        if (strcmp(key, "server") == 0) {
            copy_str(cfg->server, value, WII_MAX_STR);
        } else if (strcmp(key, "port") == 0) {
            cfg->port = (uint16_t)atoi(value);
        } else if (strcmp(key, "psk") == 0) {
            copy_str(cfg->psk, value, WII_MAX_STR);
        } else if (strcmp(key, "device_id") == 0) {
            cfg->device_id = (uint64_t)strtoull(value, NULL, 0);
        } else if (strcmp(key, "saves_dir") == 0) {
            copy_str(cfg->saves_dir, value, WII_MAX_STR);
        } else if (strcmp(key, "nintendont") == 0) {
            copy_str(cfg->nintendont, value, WII_MAX_STR);
        } else if (strcmp(key, "timeout_ms") == 0) {
            cfg->timeout_ms = atoi(value);
        } else if (strcmp(key, "rounds") == 0) {
            cfg->rounds = atoi(value);
        } else if (strcmp(key, "conflict_timeout_ms") == 0) {
            cfg->conflict_timeout_ms = atoi(value);
        } else if (strcmp(key, "pull_window") == 0) {
            cfg->pull_window = atoi(value);
        } else if (strcmp(key, "pace_every") == 0) {
            cfg->pace_every = atoi(value);
        } else if (strcmp(key, "pace_us") == 0) {
            cfg->pace_us = atoi(value);
        } else if (strcmp(key, "autoboot") == 0) {
            cfg->autoboot = (strcmp(value, "0") != 0 && strcmp(value, "false") != 0);
        }
    }
    fclose(file);

    if (cfg->server[0] == '\0') {
        snprintf(error, (size_t)error_cap, "no 'server' set in %s", path);
        return -1;
    }
    if (cfg->psk[0] == '\0') {
        snprintf(error, (size_t)error_cap, "no 'psk' set in %s", path);
        return -1;
    }
    return 0;
}
