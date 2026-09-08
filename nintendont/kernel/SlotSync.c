/* See SlotSync.h. */

#include "SlotSync.h"
#include "SlotSyncNet.h"

#include "GCNCard.h"
#include "Config.h"
#include "common.h"
#include "string.h"
#include "syscalls.h"
#include "alloc.h"
#include "ff_utf8.h"
#include "debug.h"

#include "SlotSyncLogic.h"

#include "slotsync/client.h"
#include "slotsync/protocol.h"
#include "slotsync/sha256.h"

extern int dbgprintf(const char *fmt, ...);
#define sslog dbgprintf

/* HW_TIMER ticks per millisecond. The register increments about every 526.7 ns
 * (kernel/global.h), so 1 ms is a shade under 1899 ticks. Kept as a multiplier
 * rather than a divisor so no deadline calculation needs a division. */
#define SS_TICKS_PER_MS 1899u

#define SS_CFG_PATH     "/slotsync/slotsync.cfg"
#define SS_STATE_PATH   "/slotsync/state.txt"
#define SS_HANDOFF_PATH "/slotsync/runtime.txt"

#define SS_CFG_MAX 2048

/* A 16 MiB card is 16384 chunks, one bit each. */
#define SS_BITMAP_BYTES 2048

#ifdef GCNCARD_ENABLE_SLOT_B
#define SS_SLOTS 2
#else
#define SS_SLOTS 1
#endif


typedef struct {
	char game_id[SS_GAME_ID_LEN + 1];
	u32 parent;      /* the version our bytes descend from */
	u32 dirty;       /* a save has landed since the last push */
	u32 dirty_at;    /* HW_TIMER when it landed */
	u32 pushed_at;   /* HW_TIMER of the last completed push */
	int halted;      /* a conflict: stop touching this card entirely */
	int pushed_any;  /* whether `parent` came from us rather than the wrapper */
	int lineage;     /* we know what version our bytes descend from */
} ss_slot;

static ssl_config cfg;
static ss_slot slots[SS_SLOTS];
static ssnet net;
static ss_client client;
static u8 bitmap[SS_BITMAP_BYTES] ALIGNED(32);
static char fileBuf[SS_CFG_MAX] ALIGNED(32);

static volatile u32 workerRunning;
static volatile u32 workerStop;
static volatile u32 workerBusy;
static u32 slotSyncThread;

/* Unix seconds at init, sampled on the main thread. GetCurrentTime() mutates
 * statics that the kernel's main loop also touches, so the worker must not
 * call it -- and one sample is all the nonce seed needs from it. */
static u32 bootTime;

extern char __slotsync_stack_addr, __slotsync_stack_size;

/* Read a whole small file into fileBuf, NUL-terminated. Returns its length, or
 * -1. Everything this module reads is a few hundred bytes of text. */
static int ss_read_file(const char *path)
{
	FIL fd;
	UINT read = 0;
	u32 want;

	if (f_open_char(&fd, path, FA_READ | FA_OPEN_EXISTING) != FR_OK) {
		return -1;
	}
	want = fd.obj.objsize;
	if (want > sizeof(fileBuf) - 1) {
		want = sizeof(fileBuf) - 1;
	}
	f_lseek(&fd, 0);
	if (f_read(&fd, fileBuf, want, &read) != FR_OK) {
		f_close(&fd);
		return -1;
	}
	f_close(&fd);
	fileBuf[read] = '\0';
	return (int)read;
}





/* ------------------------------------------------------------------ */
/* Config                                                              */
/* ------------------------------------------------------------------ */

static int ss_load_config(void)
{
	if (ss_read_file(SS_CFG_PATH) < 0) {
		ssl_config_defaults(&cfg);
		sslog("SlotSync: no %s, runtime sync off\r\n", SS_CFG_PATH);
		return -1;
	}
	if (ssl_config_parse(fileBuf, &cfg) != 0) {
		if (!cfg.enabled) {
			sslog("SlotSync: runtime_sync = 0, staying out of the way\r\n");
		} else {
			sslog("SlotSync: config needs a dotted-quad `server` and a `psk`\r\n");
		}
		return -1;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Card identity and lineage                                           */
/* ------------------------------------------------------------------ */

/* The six-character ID the server keys cards by.
 *
 * Nintendont only carries four characters -- ncfg->GameID is a u32, and the
 * card it writes is /saves/GALE.raw, not /saves/GALE01.raw. The other two
 * characters are the maker code, and the card itself carries them: every
 * directory entry starts with a 4-byte game code followed by a 2-byte maker
 * code (docs/MEMCARD.md). So we take them from the first entry belonging to
 * this game, and fall back to space padding on a card with no saves yet.
 */
static int ss_resolve_game_id(int slot, char out[SS_GAME_ID_LEN + 1])
{
	const u8 *base = GCNCard_GetBase(slot);
	u32 size = GCNCard_GetSize(slot);

	if (base != NULL) {
		/* The directory block is written by the game through
		 * GCNCard_Write, so make sure we are looking at it and not at a
		 * stale cache line. */
		sync_before_read((void *)base, (int)(size < 0x4000 ? size : 0x4000));
	}
	return ssl_game_id(base, size, ConfigGetGameID(), out);
}

/* Read the version the libogc wrapper last agreed with the server for this
 * card, out of the state file it writes. Lines are
 * `GAMEID SLOT VERSION <64 hex>`; we want the third field of the matching one.
 *
 * Without this we cannot name a parent, and a push that names the wrong parent
 * is exactly the silent overwrite this project exists to prevent (PLAN.md 7).
 * So a card with no entry is left alone rather than pushed speculatively.
 */
static int ss_load_parent(const char *game_id, u8 slot, u32 *out)
{
	uint32_t version = 0;

	if (ss_read_file(SS_STATE_PATH) < 0) {
		return -1;
	}
	if (ssl_state_find(fileBuf, game_id, slot, &version) != 0) {
		return -1;
	}
	*out = (u32)version;
	return 0;
}

/* Hand the versions we reached back to the libogc wrapper.
 *
 * The wrapper decides what to push by comparing the card against the digest it
 * recorded, and names `state.txt`'s version as the parent. If it never learns
 * that the kernel moved the card on, every session that used runtime sync ends
 * in a spurious conflict. A separate file rather than a rewrite of state.txt:
 * the kernel only knows about the one game that just ran, and clobbering the
 * other 63 entries to say so would be a poor trade.
 */
static void ss_write_handoff(void)
{
	FIL fd;
	UINT wrote;
	int i;
	int lines = 0;
	char line[64];

	for (i = 0; i < SS_SLOTS; i++) {
		if (slots[i].pushed_any) {
			lines++;
		}
	}
	if (lines == 0) {
		return;
	}

	if (f_open_char(&fd, SS_HANDOFF_PATH, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
		sslog("SlotSync: cannot write %s\r\n", SS_HANDOFF_PATH);
		return;
	}
	for (i = 0; i < SS_SLOTS; i++) {
		int n;

		if (!slots[i].pushed_any) {
			continue;
		}
		n = _sprintf(line, "%s %u %u\n", slots[i].game_id, (u32)i,
			     slots[i].parent);
		if (n > 0) {
			f_write(&fd, line, (UINT)n, &wrote);
		}
	}
	f_close(&fd);
	sslog("SlotSync: wrote %s\r\n", SS_HANDOFF_PATH);
}

/* ------------------------------------------------------------------ */
/* The push                                                            */
/* ------------------------------------------------------------------ */

static int ss_should_push(int slot)
{
	ss_slot *s = &slots[slot];

	if (!GCNCard_IsEnabled(slot)) {
		return 0;
	}
	return ssl_should_push((int)s->dirty, s->halted, TimerDiffTicks(s->dirty_at),
			       s->pushed_at != 0 ? TimerDiffTicks(s->pushed_at) : 0,
			       s->pushed_at != 0, cfg.quiet_ms * SS_TICKS_PER_MS,
			       cfg.cooldown_ms * SS_TICKS_PER_MS);
}

static void ss_push_slot(int slot)
{
	ss_slot *s = &slots[slot];
	const u8 *image = GCNCard_GetBase(slot);
	u32 size = GCNCard_GetSize(slot);
	/* uint32_t, not u32: the two are both 32 bits wide but need not be the
	 * same type, and this one is passed to the core by pointer. */
	uint32_t assigned = 0;
	int rc;

	if (image == NULL || size == 0) {
		return;
	}
	if (ss_chunk_count(size) > SS_BITMAP_BYTES * 8u) {
		sslog("SlotSync: slot %c card too large to track\r\n", slot + 'A');
		s->halted = 1;
		return;
	}

	/* Clear the flag before reading the card, not after. A write that lands
	 * during the transfer then re-arms it and we push again, instead of the
	 * change being swallowed. */
	s->dirty = 0;
	sync_before_read((void *)image, (int)size);

	sslog("SlotSync: pushing %s slot %c, %u KiB, parent v%u\r\n", s->game_id,
	      slot + 'A', size / 1024, s->parent);

	workerBusy = 1;
	rc = ss_push(&client, s->game_id, (u8)slot, image, size, s->parent,
		     s->parent + 1, bitmap, sizeof(bitmap), &assigned);
	workerBusy = 0;

	if (rc == SS_OK) {
		s->parent = (u32)assigned;
		s->pushed_any = 1;
		s->pushed_at = read32(HW_TIMER);
		sslog("SlotSync: %s is now v%u\r\n", s->game_id, (u32)assigned);
		ss_write_handoff();
		return;
	}

	if (rc == SS_ERR_CONFLICT) {
		/* Someone else moved this card on. Never guess which side wins:
		 * stop syncing this card for the rest of the session and leave
		 * the choice to a human in the web UI (PLAN.md 7). The save on
		 * the SD card is untouched either way. */
		sslog("SlotSync: CONFLICT on %s -- server is on v%u, ours descends "
		      "from v%u. Runtime sync off for this card; choose in the "
		      "web UI.\r\n", s->game_id, (u32)client.last_head, s->parent);
		s->halted = 1;
		return;
	}

	/* Anything else is transient as far as we are concerned: a lost server,
	 * a torn image the checksum caught, a full staging table. Re-arm and
	 * let the cooldown space out the retry. */
	s->dirty = 1;
	s->pushed_at = read32(HW_TIMER);
	sslog("SlotSync: push of %s failed: %s (nack 0x%02x)\r\n", s->game_id,
	      ss_strerror(rc), client.last_error_code);
}

/* ------------------------------------------------------------------ */
/* Worker                                                              */
/* ------------------------------------------------------------------ */

static int ss_connect(void)
{
	u8 seed[8];
	u32 now = read32(HW_TIMER);
	int i;

	if (ssnet_bring_up((int)cfg.net_timeout_ms) != 0) {
		return -1;
	}
	if (ssnet_open(&net, cfg.server, cfg.port) != 0) {
		return -1;
	}
	net.pace_every = cfg.pace_every;
	net.pace_us = cfg.pace_us;

	/* Nonce seed. The console has no RNG worth the name, and it does not need
	 * one: the HMAC provides authenticity, so all the nonce has to be is
	 * unique inside the server's replay window (120 s, keyed by device_id --
	 * server/src/slotsync/replay.py).
	 *
	 * Unix seconds and the free-running timer, not the timer alone. The timer
	 * restarts near zero at every console power-on, so two launches at a
	 * similar offset after two power cycles can produce the same value -- and
	 * a repeated nonce gets our HELLO dropped silently. The clock the loader
	 * hands down does not restart, so together they do not repeat. */
	for (i = 0; i < 4; i++) {
		seed[i] = (u8)(bootTime >> (i * 8));
		seed[4 + i] = (u8)(now >> (i * 8));
	}

	{
		ss_transport transport;
		transport.send = ssnet_send;
		transport.recv = ssnet_recv;
		transport.ctx = &net;
		ss_client_init(&client, &transport, (const u8 *)cfg.psk, cfg.psk_len,
			       cfg.device_id, seed);
	}
	client.timeout_ms = cfg.timeout_ms;
	client.max_rounds = cfg.rounds;

	if (ss_hello(&client, NULL, NULL) != SS_OK) {
		sslog("SlotSync: server did not answer HELLO\r\n");
		ssnet_close(&net);
		return -1;
	}
	sslog("SlotSync: server reachable, runtime sync armed\r\n");
	return 0;
}

/* Settle what a card's bytes descend from, for a card the launcher never
 * recorded.
 *
 * There is exactly one case where the server can tell us safely: it has never
 * seen this card at all, so parent 0 overwrites nothing. Any other answer means
 * a lineage exists that we are not part of, and picking its head as our parent
 * would be precisely the silent overwrite of someone else's save that this
 * project exists to prevent (PLAN.md section 7). So we stop instead.
 *
 * Returns 1 when the card can now be pushed, 0 when it cannot yet, and -1 when
 * it never can.
 */
static int ss_establish_lineage(int slot)
{
	ss_slot *s = &slots[slot];
	uint32_t head = 0;
	uint32_t size = 0;
	int rc;

	rc = ss_head(&client, s->game_id, (u8)slot, &head, &size);
	if (rc == SS_OK) {
		sslog("SlotSync: %s is already at v%u on the server and this "
		      "console has no record of it. Runtime sync off for this "
		      "card -- run the launcher once so it can agree a version "
		      "with the server.\r\n", s->game_id, (u32)head);
		return -1;
	}
	if (client.last_error_code == SS_NACK_UNKNOWN_CARD) {
		s->parent = 0;
		s->lineage = 1;
		sslog("SlotSync: the server has not seen %s; starting at v0\r\n",
		      s->game_id);
		return 1;
	}
	/* Anything else is a transient failure; try again next time round. */
	return 0;
}

static u32 SlotSyncWorker(void *arg)
{
	int connected = 0;
	int backoff_ms = 2000;

	(void)arg;
	workerRunning = 1;

	while (!workerStop) {
		int slot;

		if (!connected) {
			if (ss_connect() == 0) {
				connected = 1;
				backoff_ms = 2000;
			} else {
				/* Back off to 60 s. A console with no server on
				 * the LAN should not spend the whole session
				 * retrying. */
				mdelay(backoff_ms);
				if (backoff_ms < 60000) {
					backoff_ms *= 2;
				}
				continue;
			}
		}

		for (slot = 0; slot < SS_SLOTS; slot++) {
			if (workerStop) {
				break;
			}
			if (!ss_should_push(slot)) {
				continue;
			}
			if (!slots[slot].lineage) {
				int settled = ss_establish_lineage(slot);
				if (settled < 0) {
					slots[slot].halted = 1;
				}
				if (settled <= 0) {
					continue;
				}
			}
			ss_push_slot(slot);
		}

		mdelay(250);
	}

	if (connected) {
		ssnet_close(&net);
	}
	workerRunning = 0;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Public entry points                                                 */
/* ------------------------------------------------------------------ */

int SlotSync_Init(void)
{
	int slot;
	int usable = 0;

	memset(slots, 0, sizeof(slots));
	bootTime = GetCurrentTime();
	workerStop = 0;
	workerRunning = 0;
	workerBusy = 0;

	if (ss_load_config() != 0) {
		return -1;
	}

	for (slot = 0; slot < SS_SLOTS; slot++) {
		ss_slot *s = &slots[slot];

		if (!GCNCard_IsEnabled(slot)) {
			s->halted = 1;
			continue;
		}
		if (!ss_resolve_game_id(slot, s->game_id)) {
			/* The maker code has to come off the card. Space-padding
			 * the four characters Nintendont carries would key a
			 * different card on the server than the Dolphin daemon
			 * uses for the same game, quietly splitting the lineage
			 * in two. A card with nothing saved on it yet has nothing
			 * worth pushing anyway, so wait for the first save. */
			sslog("SlotSync: slot %c has no saves yet -- nothing to "
			      "identify it by, so nothing to sync\r\n", slot + 'A');
			s->halted = 1;
			continue;
		}

		if (ss_load_parent(s->game_id, (u8)slot, &s->parent) == 0) {
			s->lineage = 1;
			sslog("SlotSync: slot %c is %s at v%u\r\n", slot + 'A',
			      s->game_id, s->parent);
		} else {
			/* No entry in the launcher's state file. We cannot name a
			 * parent yet, and guessing one risks overwriting a version
			 * we have never seen. ss_establish_lineage settles it from
			 * the server before anything is pushed. */
			sslog("SlotSync: slot %c is %s, lineage unknown\r\n",
			      slot + 'A', s->game_id);
		}
		usable++;
	}

	if (usable == 0) {
		sslog("SlotSync: nothing to sync\r\n");
		return -1;
	}

	slotSyncThread = do_thread_create(SlotSyncWorker,
					  (u32 *)&__slotsync_stack_addr,
					  (u32)(&__slotsync_stack_size), 0x50);
	thread_continue(slotSyncThread);
	return 0;
}

void SlotSync_NotifyCardSaved(int slot)
{
	if (slot < 0 || slot >= SS_SLOTS || !workerRunning) {
		return;
	}
	slots[slot].dirty = 1;
	slots[slot].dirty_at = read32(HW_TIMER);
}

int SlotSync_Busy(void)
{
	return (int)workerBusy;
}

void SlotSync_Shutdown(void)
{
	int waited;

	if (!workerRunning) {
		return;
	}
	workerStop = 1;

	/* Give an in-flight transfer a bounded moment to finish so the handoff
	 * file reflects it. Exiting a game must not hang on a dead server, so
	 * this is a wait with a deadline and no more. */
	for (waited = 0; waited < 3000 && workerRunning; waited += 50) {
		mdelay(50);
	}
	if (workerRunning) {
		sslog("SlotSync: worker still busy at exit, abandoning transfer\r\n");
	}
}
