/* SlotSync inside Nintendont's ARM kernel: push a memory card to the server
 * while the game is still running.
 *
 * PLAN.md calls this phase 2. The libogc wrapper in wii/ syncs either side of a
 * game; this syncs during one. What makes it tractable is that Nintendont
 * already keeps the whole card image in ARM-addressable RAM (GCNCard.c, base
 * 0x11000000) and already knows the moment it changes, so there is nothing to
 * read back off the SD card and nothing to poll for.
 *
 * Everything that can block runs on a dedicated low-priority thread. The only
 * two functions the kernel's main loop calls are SlotSync_Init and
 * SlotSync_NotifyCardSaved, and neither of them waits for anything.
 */

#ifndef SLOTSYNC_H
#define SLOTSYNC_H

#include "global.h"

/* Read /slotsync/slotsync.cfg and, if it enables runtime sync, start the
 * worker thread. Safe to call when there is no config file: it becomes a
 * no-op and Nintendont behaves exactly as it does upstream.
 *
 * Call once from the kernel's startup, after EXIInit (which loads the cards)
 * and after SOCKInit. Returns 0 if the worker started, non-zero otherwise --
 * and a non-zero return is not an error worth stopping for. */
int SlotSync_Init(void);

/* The card in `slot` has just been written to the SD card. Records the moment
 * and returns immediately; the worker decides when, and whether, to push.
 * Called from GCNCard_Save. */
void SlotSync_NotifyCardSaved(int slot);

/* Call from the main loop. Writes out anything the worker logged, and any
 * handoff it owes, at the only point in a running game where FatFs is safe.
 * Rate limited internally; cheap to call every iteration. */
void SlotSync_Poll(void);

/* Stop the worker and hand back the version numbers we reached, so the libogc
 * wrapper can pick up the lineage where the kernel left it. Called on game
 * exit. */
void SlotSync_Shutdown(void);

/* True while a transfer is in flight.
 *
 * Intended as an interlock so a card is not saved out from under a push -- but
 * nothing calls it yet, in EXI.c or anywhere else, so no such interlock exists.
 * A save landing mid-push tears the image; the whole-card digest catches that,
 * the server refuses it, and ss_push_slot re-arms and retries. Wasteful rather
 * than dangerous, which is why this is still a TODO and not a defect. */
int SlotSync_Busy(void);

/* Log a line. Goes to dbgprintf and, independently of whether that reaches
 * anywhere, to /slotsync/runtime.log. Exposed so the socket layer's trace can
 * land in the same place rather than in a build nobody has. */
void SlotSync_Log(const char *fmt, ...);

#endif /* SLOTSYNC_H */
