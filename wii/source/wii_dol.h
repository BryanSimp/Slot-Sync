/* Loading and running another homebrew .dol -- here, Nintendont.
 *
 * PLAN.md section 13's first open question is whether Nintendont ever returns
 * control to us. This loader does not assume either way: main.c syncs before
 * the handover and again on return, so "it never comes back" degrades to
 * "syncs on next launch" rather than breaking anything.
 */

#ifndef SLOTSYNC_WII_DOL_H
#define SLOTSYNC_WII_DOL_H

/* Load `path` and jump to it. On success this does not return.
 * Returns -1 if the file cannot be read or does not look like a DOL. */
int wii_dol_run(const char *path, int argc, char **argv);

#endif /* SLOTSYNC_WII_DOL_H */
