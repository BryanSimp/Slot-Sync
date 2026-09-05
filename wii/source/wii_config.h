/* Configuration, read from the SD card at /slotsync/slotsync.cfg.
 *
 * A plain `key = value` text file, because the alternative on a console with no
 * keyboard is recompiling to change an IP address.
 */

#ifndef SLOTSYNC_WII_CONFIG_H
#define SLOTSYNC_WII_CONFIG_H

#include <stdint.h>

#define WII_CFG_PATH "sd:/slotsync/slotsync.cfg"
#define WII_SAVES_DIR "sd:/saves"
#define WII_STATE_PATH "sd:/slotsync/state.txt"

#define WII_MAX_STR 128

typedef struct {
    char server[WII_MAX_STR];   /* dotted-quad IPv4; no DNS on this path */
    uint16_t port;
    char psk[WII_MAX_STR];
    uint64_t device_id;
    char saves_dir[WII_MAX_STR];
    char nintendont[WII_MAX_STR]; /* .dol to chainload */
    int timeout_ms;
    int rounds;
    int autoboot;               /* chainload without waiting for a button */
} wii_config;

void wii_config_defaults(wii_config *cfg);

/* Returns 0 on success, -1 if the file is missing or unreadable. `error` gets a
 * short human-readable reason. */
int wii_config_load(wii_config *cfg, const char *path, char *error, int error_cap);

#endif /* SLOTSYNC_WII_CONFIG_H */
