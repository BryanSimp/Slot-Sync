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
/* Written by the in-kernel sync in Nintendont (nintendont/), naming the
 * versions it reached while the game was running. Read once at startup and
 * removed. */
#define WII_RUNTIME_PATH "sd:/slotsync/runtime.txt"

/* Per-block fingerprints, so a push can send only the blocks that changed --
 * core/fingerprint.h. The store is shared with Nintendont's in-kernel sync,
 * which reads it at boot and writes its own tables back through the second
 * file, exactly as it does with state.txt and runtime.txt above. */
#define WII_FP_PATH "sd:/slotsync/fingerprints.bin"
#define WII_FP_RUNTIME_PATH "sd:/slotsync/runtime-fp.bin"

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
    int conflict_timeout_ms;    /* 0 waits for a button for ever */
    int pull_window;            /* chunks asked for per pull round */
    int pace_every;             /* datagrams to send before pausing; 0 = never */
    int pace_us;                /* how long to pause for */
    int autoboot;               /* chainload without waiting for a button */
} wii_config;

void wii_config_defaults(wii_config *cfg);

/* Returns 0 on success, -1 if the file is missing or unreadable. `error` gets a
 * short human-readable reason. */
int wii_config_load(wii_config *cfg, const char *path, char *error, int error_cap);

#endif /* SLOTSYNC_WII_CONFIG_H */
