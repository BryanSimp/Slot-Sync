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
#include "slotsync/fingerprint.h"
#include "slotsync/protocol.h"
#include "slotsync/sha256.h"

extern int dbgprintf(const char *fmt, ...);

/* Diagnostics for a client with nowhere to print.
 *
 * dbgprintf reaches a USB Gecko, and /ndebug.log only when Nintendont's own
 * NIN_CFG_LOG is on AND a hardware register check inside it passes. On a
 * console with neither, a runtime sync that never got off the ground is
 * indistinguishable from one that started and had nothing to push -- which is
 * exactly the position this was debugged from. So everything logged is also
 * kept in RAM and written to /slotsync/runtime.log, which depends on neither.
 *
 * Flushed at shutdown and nowhere else. Nintendont builds FatFs with
 * _FS_REENTRANT 0, so it is not thread safe -- and the DI thread is streaming
 * the game off the same card. Writing this log from the worker mid-game wedged
 * the console at the Nintendo screen, which is a far worse bug than the one it
 * was added to diagnose. Everything is therefore held in RAM until exit. */
#define sslog SlotSync_Log

#define SS_LOG_PATH "/slotsync/runtime.log"
#define SS_LOG_MAX  8192

static char logBuf[SS_LOG_MAX];
static u32 logLen;
static int logDirty;

void SlotSync_Log(const char *fmt, ...)
{
	char line[256];
	va_list args;
	int n;

	va_start(args, fmt);
	n = _vsprintf(line, fmt, args);
	va_end(args);
	if (n <= 0) {
		return;
	}

	dbgprintf("%s", line);

	if (logLen + (u32)n < SS_LOG_MAX) {
		memcpy(logBuf + logLen, line, (u32)n);
		logLen += (u32)n;
		logDirty = 1;
	}
}

static void ss_log_flush(void)
{
	FIL fd;
	UINT wrote;

	if (!logDirty || logLen == 0) {
		return;
	}
	logDirty = 0;
	if (f_open_char(&fd, SS_LOG_PATH, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
		return;
	}
	f_write(&fd, logBuf, (UINT)logLen, &wrote);
	f_close(&fd);
}

/* HW_TIMER ticks per millisecond. The register increments about every 526.7 ns
 * (kernel/global.h), so 1 ms is a shade under 1899 ticks. Kept as a multiplier
 * rather than a divisor so no deadline calculation needs a division. */
#define SS_TICKS_PER_MS 1899u

#define SS_CFG_PATH     "/slotsync/slotsync.cfg"
#define SS_STATE_PATH   "/slotsync/state.txt"
#define SS_HANDOFF_PATH "/slotsync/runtime.txt"

/* Per-block fingerprints, so the first push of a session can be a delta too.
 *
 * Two files for the same reason state.txt and runtime.txt are two files: the
 * launcher's store covers every card it tracks, and this kernel knows about the
 * one game that just ran. Rewriting the first from here would clobber the other
 * 63 cards' tables to say something about one. So we read the launcher's and
 * write our own, and the launcher merges it on the way back -- exactly as it
 * already does with runtime.txt. */
#define SS_FP_PATH         "/slotsync/fingerprints.bin"
#define SS_FP_HANDOFF_PATH "/slotsync/runtime-fp.bin"

/* Staging for the fingerprint file. Big enough for a record header and to move
 * values a slice at a time, small enough to sit on a 16 KiB stack -- a 16 MiB
 * card's table is 8 KiB, so it is never read or written in one go. */
#define SS_FP_SLICE 512

#define SS_CFG_MAX 2048

/* How long SlotSync_Init may wait for DHCP before letting the game boot.
 * See the note at the call site. */
#define SS_INIT_NET_BUDGET_MS 6000

/* A 16 MiB card is 16384 chunks, one bit each. */
#define SS_BITMAP_BYTES 2048

/* Per-block fingerprints for the delta push -- SlotSyncLogic.h.
 *
 * One u32 per 8 KiB block, so 2048 entries is 8 KiB of BSS and covers a 16 MiB
 * card. With both slots enabled each gets half, which caps a delta-capable card
 * at 8 MiB per slot; a card over its share scans as -1 and is pushed whole,
 * which is what every push did before this existed. Raise the number if that
 * ever bites, and pay the BSS. */
#define SS_FP_ENTRIES 2048

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
	/* Digest of the image the server holds as `parent`. From state.txt at
	 * init, from the push itself after that. It is what says which image a
	 * fingerprint table describes, so it rides with the table. */
	u8 parent_sha[SS_FP_DIGEST_SIZE];
	/* The fingerprint table describes exactly the bytes the server committed
	 * as `parent`. Only then can a delta be built against it.
	 *
	 * Cleared before every push and set again only when that push is
	 * confirmed. ssl_delta_scan updates the table in place, so between the
	 * scan and the ack the table describes bytes the server has not accepted
	 * -- and a delta built on those would name the wrong chunks. The cost of
	 * a failed push is therefore one whole-card push afterwards, which is
	 * what the failure would have cost anyway. */
	int fp_valid;
} ss_slot;

static ssl_config cfg;
static ss_slot slots[SS_SLOTS];
static ssnet net;
static ss_client client;
static u8 bitmap[SS_BITMAP_BYTES] ALIGNED(32);
/* What changed since the last confirmed push. Separate from `bitmap`, which
 * ss_push_delta uses as its own scratch and rewrites every round. */
static u8 dirtyMap[SS_BITMAP_BYTES] ALIGNED(32);
static u32 fingerprints[SS_FP_ENTRIES];
static char fileBuf[SS_CFG_MAX] ALIGNED(32);

/* Whether ss_connect has succeeded. File scope because SlotSync_Init makes the
 * first attempt itself -- see the comment where it does. */
static int netConnected;

/* A push moved a card on; the handoff file is owed. Written at shutdown by
 * the main thread, never by the worker. */
static volatile int handoffPending;

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
static int ss_load_parent(const char *game_id, u8 slot, u32 *out, u8 *sha)
{
	uint32_t version = 0;

	if (ss_read_file(SS_STATE_PATH) < 0) {
		return -1;
	}
	if (ssl_state_find(fileBuf, game_id, slot, &version, sha) != 0) {
		return -1;
	}
	*out = (u32)version;
	return 0;
}

/* ------------------------------------------------------------------ */
/* The fingerprint store                                               */
/* ------------------------------------------------------------------ */

/* Whether a buffer is all zeros. A state file written before digests were
 * recorded leaves one zeroed, and that has to read as "no digest to check
 * against" rather than as a digest that happens to be zero. */
static int ss_all_zero(const u8 *p, u32 len)
{
	u32 i;

	for (i = 0; i < len; i++) {
		if (p[i] != 0) {
			return 0;
		}
	}
	return 1;
}

/* One slot's slice of the shared table. */
static u32 *ss_fp_table(int slot)
{
	return fingerprints + (u32)slot * (SS_FP_ENTRIES / SS_SLOTS);
}

static u32 ss_fp_share(void)
{
	return SS_FP_ENTRIES / SS_SLOTS;
}

/* Read `want` bytes, or fail. FatFs short-reads at end of file rather than
 * erroring, and a truncated store has to read as "no record" rather than as a
 * record made of whatever was left in the buffer. */
static int ss_fp_read(FIL *fd, u8 *buf, u32 want)
{
	UINT got = 0;

	if (f_read(fd, buf, (UINT)want, &got) != FR_OK || (u32)got != want) {
		return -1;
	}
	return 0;
}

/* Read past `bytes` of a record we do not want, a slice at a time. Sequential
 * reads only: f_lseek would do this in one call, but reading forward needs
 * nothing from FatFs beyond what the rest of this file already uses. */
static int ss_fp_skip(FIL *fd, u32 bytes, u8 *buf)
{
	while (bytes > 0) {
		u32 take = bytes < SS_FP_SLICE ? bytes : SS_FP_SLICE;

		if (ss_fp_read(fd, buf, take) != 0) {
			return -1;
		}
		bytes -= take;
	}
	return 0;
}

/* Load one card's fingerprints out of the launcher's store.
 *
 * This is what makes the *first* push of a session a delta. Held only in RAM
 * the table is empty at every boot, so every session opened with a whole card:
 * 5 s of a 2 MiB card, 41 s of a 16 MiB one, while the game is running.
 *
 * Returns 0 when the table now describes the image the server holds as
 * `parent`, and -1 otherwise -- no file, no record, or a record for some other
 * version, size or image. Every one of those is a reason to push whole, not a
 * reason to complain.
 */
static int ss_load_fingerprints(int slot, const char *game_id, u32 parent, u32 size,
				const u8 *parent_sha)
{
	FIL fd;
	u8 buf[SS_FP_SLICE];
	u32 *table = ss_fp_table(slot);
	int records;
	int i;
	int found = -1;

	if (parent == 0 || size == 0) {
		return -1; /* nothing to descend from */
	}
	if (f_open_char(&fd, SS_FP_PATH, FA_READ | FA_OPEN_EXISTING) != FR_OK) {
		return -1; /* no store yet is the normal first run */
	}
	if (ss_fp_read(&fd, buf, SS_FP_HEADER_SIZE) != 0) {
		f_close(&fd);
		return -1;
	}
	records = ss_fp_store_count(buf, SS_FP_HEADER_SIZE);
	if (records < 0) {
		f_close(&fd);
		sslog("SlotSync: %s is not a fingerprint store\r\n", SS_FP_PATH);
		return -1;
	}

	for (i = 0; i < records && found != 0; i++) {
		ss_fp_record rec;
		u32 values;

		if (ss_fp_read(&fd, buf, SS_FP_RECORD_SIZE) != 0) {
			break;
		}
		/* A self-inconsistent record cannot be skipped past either: its
		 * block count is what says where the next one starts. */
		if (ss_fp_get_record(&rec, buf) != 0) {
			break;
		}
		values = rec.blocks * 4u;

		if (rec.slot != (u8)slot
		    || memcmp(rec.game_id, game_id, SS_GAME_ID_LEN) != 0) {
			if (ss_fp_skip(&fd, values, buf) != 0) {
				break;
			}
			continue;
		}

		/* Ours. Every one of these has to hold, because a table that
		 * describes any image but the server's `parent` names the wrong
		 * chunks. Being wrong costs a round rather than a card -- the
		 * PUSH_END digest refuses it -- but a round mid-game is the
		 * thing this whole change exists to avoid. */
		if (rec.version != parent || rec.size != size
		    || rec.blocks > ss_fp_share()) {
			break;
		}
		/* The digest is the strongest of the checks: it says the record
		 * was taken from the same bytes state.txt calls this version.
		 * A state file written before digests were recorded leaves it
		 * zeroed, and then the version and size have to carry it. */
		if (!ss_all_zero(parent_sha, SS_FP_DIGEST_SIZE)
		    && memcmp(rec.sha256, parent_sha, SS_FP_DIGEST_SIZE) != 0) {
			sslog("SlotSync: %s fingerprints are for other bytes at "
			      "v%u; pushing whole\r\n", game_id, (u32)parent);
			break;
		}

		{
			u32 left = rec.blocks;
			u32 at = 0;

			while (left > 0) {
				u32 take = left < (SS_FP_SLICE / 4u) ? left
								    : (SS_FP_SLICE / 4u);

				if (ss_fp_read(&fd, buf, take * 4u) != 0) {
					break;
				}
				ss_fp_get_values(table + at, buf, take);
				at += take;
				left -= take;
			}
			found = left == 0 ? 0 : -1;
		}
	}

	f_close(&fd);
	return found;
}

/* Hand our tables to the launcher, so its next push is a delta as well.
 *
 * Only slots that pushed *and* whose table is confirmed: a table written after
 * a push the server did not accept describes bytes nobody holds, and would cost
 * the launcher the wasted round we just saved it.
 */
static void ss_write_fp_handoff(void)
{
	FIL fd;
	UINT wrote;
	u8 buf[SS_FP_SLICE];
	int i;
	int cards = 0;

	for (i = 0; i < SS_SLOTS; i++) {
		if (slots[i].pushed_any && slots[i].fp_valid) {
			cards++;
		}
	}
	if (cards == 0) {
		return;
	}

	if (f_open_char(&fd, SS_FP_HANDOFF_PATH, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
		sslog("SlotSync: cannot write %s\r\n", SS_FP_HANDOFF_PATH);
		return;
	}

	ss_fp_put_header(buf, (u16)cards);
	f_write(&fd, buf, SS_FP_HEADER_SIZE, &wrote);

	for (i = 0; i < SS_SLOTS; i++) {
		ss_slot *s = &slots[i];
		ss_fp_record rec;
		const u32 *table;
		u32 size;
		u32 left;
		u32 at = 0;

		if (!s->pushed_any || !s->fp_valid) {
			continue;
		}
		size = GCNCard_GetSize(i);
		if (size == 0) {
			continue;
		}

		memcpy(rec.game_id, s->game_id, SS_GAME_ID_LEN + 1);
		rec.slot = (u8)i;
		rec.version = s->parent;
		rec.size = size;
		rec.blocks = ss_fp_blocks(size);
		memcpy(rec.sha256, s->parent_sha, SS_FP_DIGEST_SIZE);

		ss_fp_put_record(buf, &rec);
		f_write(&fd, buf, SS_FP_RECORD_SIZE, &wrote);

		table = ss_fp_table(i);
		left = rec.blocks;
		while (left > 0) {
			u32 take = left < (SS_FP_SLICE / 4u) ? left : (SS_FP_SLICE / 4u);

			ss_fp_put_values(buf, table + at, take);
			f_write(&fd, buf, (UINT)(take * 4u), &wrote);
			at += take;
			left -= take;
		}
		sslog("SlotSync: handed %s v%u's fingerprints to the launcher\r\n",
		      s->game_id, (u32)s->parent);
	}
	f_close(&fd);
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

/* Defined below; the push re-establishes the connection before transferring. */
static int ss_connect(void);

static void ss_push_slot(int slot)
{
	ss_slot *s = &slots[slot];
	const u8 *image = GCNCard_GetBase(slot);
	u32 size = GCNCard_GetSize(slot);
	/* uint32_t, not u32: the two are both 32 bits wide but need not be the
	 * same type, and this one is passed to the core by pointer. */
	uint32_t assigned = 0;
	int useDelta = 0;
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

	/* What changed since the last confirmed push.
	 *
	 * The scan always runs, because it is what keeps the fingerprint table
	 * current; whether its answer is usable is a separate question, and
	 * fp_valid is what answers it. The table is invalid before the first
	 * push of a session and after any push that did not land, so the first
	 * push of a card always goes whole. */
	{
		int32_t marked = ss_fp_scan(image, size, ss_fp_table(slot),
					    ss_fp_share(), dirtyMap,
					    (u32)sizeof(dirtyMap));

		useDelta = s->fp_valid && marked >= 0
			   && ss_fp_worthwhile((u32)marked, ss_chunk_count(size));

		/* Not valid again until this push is confirmed: the scan above
		 * has already moved the table on to bytes the server has not
		 * accepted. */
		s->fp_valid = 0;

		if (useDelta) {
			sslog("SlotSync: pushing %s slot %c, %u of %u chunks "
			      "changed, parent v%u\r\n", s->game_id, slot + 'A',
			      (u32)marked, ss_chunk_count(size), s->parent);
		} else {
			sslog("SlotSync: pushing %s slot %c, %u KiB whole, "
			      "parent v%u\r\n", s->game_id, slot + 'A',
			      size / 1024, s->parent);
		}
	}

	/* Fresh trace budget, so that if the per-datagram trace is ever turned
	 * back on it spends itself on the push rather than on the handshake
	 * that precedes it. */
	ssnet_trace_reset();

	{
		u32 live = ssnet_live_ip();

		sslog("SlotSync: interface now %u.%u.%u.%u%s\r\n",
		      (live >> 24) & 0xFF, (live >> 16) & 0xFF,
		      (live >> 8) & 0xFF, live & 0xFF,
		      live == 0 ? " -- GONE" : "");
	}

	/* Build a fresh socket for every push.
	 *
	 * The interface is up and IOS answers GETHOSTID at this moment, so the
	 * stack is alive -- but this socket has been sitting idle since the
	 * HELLO, thirty to sixty seconds ago while the game loaded and was
	 * played. A HELLO that always works because it is the first thing the
	 * socket ever does, and a push that usually does not because it is the
	 * next thing minutes later, is what an idle socket quietly going stale
	 * looks like: sendto still reports the bytes accepted and nothing
	 * leaves.
	 *
	 * A socket and a HELLO cost one round trip against a card transfer of
	 * two thousand, so this is cheap insurance even if the diagnosis is
	 * wrong. */
	ssnet_close(&net);
	netConnected = 0;
	if (ss_connect() != 0) {
		sslog("SlotSync: could not reconnect for the push\r\n");
		s->dirty = 1;
		s->pushed_at = read32(HW_TIMER);
		return;
	}
	netConnected = 1;

	workerBusy = 1;
	/* ss_push_delta with a NULL bitmap is exactly ss_push, so there is one
	 * call rather than two paths that can drift apart. If the server cannot
	 * seed from our parent it answers 0x0c and the client sends the whole
	 * card itself; nothing here has to handle that. */
	rc = ss_push_delta(&client, s->game_id, (u8)slot, image, size, s->parent,
			   s->parent + 1, useDelta ? dirtyMap : NULL, bitmap,
			   sizeof(bitmap), &assigned);
	workerBusy = 0;

	/* A lost commit ACK is not a failed push.
	 *
	 * The server commits the version and drops the staging buffer in the
	 * same breath. If its final ACK does not reach us we retransmit
	 * PUSH_END, it finds no staging, and answers STAGING_EXPIRED -- so a
	 * transfer that is already safely committed reports as a failure. The
	 * card then keeps its old parent, pushes again on the next save, and is
	 * told 409 by a server that is simply ahead of us.
	 *
	 * A server from M13 onwards remembers the reply and gives it again, so
	 * this never fires against one -- docs/PROTOCOL.md, "A repeated PUSH_END
	 * gets the same answer". It stays for the older ones, and because it is
	 * cheap. Do not mistake it for the fix: it needs one more round trip to
	 * survive immediately after a round trip did not, which is exactly when
	 * that is least likely. On 2026-09-12 it did not survive, and a
	 * committed v38 still went down as a failure.
	 *
	 * Asking where the head is settles it, as far as anything here can. If
	 * it sits exactly where this push would have put it and holds a card the
	 * size of ours, the bytes are almost certainly the ones we sent -- but
	 * "almost" is doing real work in a house with two consoles, so the
	 * server-side replay is what this actually rests on. */
	if (rc != SS_OK && client.last_error_code == SS_NACK_STAGING_EXPIRED) {
		uint32_t head = 0;
		uint32_t head_size = 0;

		if (ss_head(&client, s->game_id, (u8)slot, &head, &head_size) == SS_OK
		    && head == s->parent + 1 && head_size == size) {
			sslog("SlotSync: %s: commit ACK lost, but the server is on "
			      "v%u -- our push landed\r\n", s->game_id, (u32)head);
			assigned = head;
			rc = SS_OK;
		}
	}

	if (rc == SS_OK) {
		s->parent = (u32)assigned;
		s->pushed_any = 1;
		s->pushed_at = read32(HW_TIMER);
		/* Confirmed, so the table now describes what the server holds
		 * and the next push can be a delta against it -- this session's
		 * and, once the handoff is written, the launcher's. */
		s->fp_valid = 1;
		memcpy(s->parent_sha, client.last_digest, SS_FP_DIGEST_SIZE);
		sslog("SlotSync: %s is now v%u (%u sends refused)\r\n",
		      s->game_id, (u32)assigned, net.dropped);
		/* Do NOT write the handoff here.
		 *
		 * This runs on the worker, and Nintendont builds FatFs with
		 * _FS_REENTRANT 0 while the DI thread streams the game off the
		 * same card -- writing from here is what wedged the console when
		 * the log tried it. It never fired before only because no push
		 * had ever succeeded.
		 *
		 * Shutdown writes it instead, on the main thread. The file
		 * exists for the next launcher run, so exit is soon enough. A
		 * reset skips it and the launcher then pushes against a stale
		 * parent and is told 409 -- which is a refusal, not a loss. */
		handoffPending = 1;
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
	int rc;

	if (ssnet_bring_up((int)cfg.net_timeout_ms) != 0) {
		sslog("SlotSync: network did not come up\r\n");
		return -1;
	}
	if (ssnet_open(&net, cfg.server, cfg.port) != 0) {
		sslog("SlotSync: cannot open a socket to the server\r\n");
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
		/* A device id of our own, derived from the configured one.
		 *
		 * The launcher and this client are two different clients on one
		 * console, and they were sharing an identity. The server's
		 * replay cache is keyed per device with a 120 s window, and the
		 * launcher says HELLO seconds before the game starts -- so a
		 * nonce these two happen to agree on inside that window is
		 * dropped silently, which is indistinguishable from the server
		 * being unreachable and blocks the read for ever.
		 *
		 * Separating them also settles which client pushed a version,
		 * which has cost several rounds of guessing from the outside. */
		ss_client_init(&client, &transport, (const u8 *)cfg.psk, cfg.psk_len,
			       cfg.device_id ^ 0x8000000000000000ull, seed);
	}
	client.timeout_ms = cfg.timeout_ms;
	client.max_rounds = cfg.rounds;

	/* A short deadline for the HELLO only.
	 *
	 * The configured 12 rounds of 2 s means up to 24 s before a failed
	 * HELLO reports anything, and a session shorter than that ends with the
	 * log stopping mid-attempt, which reads as a hang. The server is on the
	 * same LAN and answers in milliseconds when it answers at all.
	 *
	 * It is restored below before anything else runs. Leaving it clamped
	 * gave the push four rounds of one second to move two megabytes and
	 * wait on a server allocating a staging buffer for it -- which timed
	 * out, and looked like the push failing rather than like an impatient
	 * client. */
	if (client.timeout_ms > 1000) {
		client.timeout_ms = 1000;
	}
	if (client.max_rounds > 4) {
		client.max_rounds = 4;
	}

	/* Say so before blocking. A HELLO retries for rounds x timeout -- 24 s
	 * at the shipped settings -- and without this line an attempt still in
	 * flight and one that failed look identical in the log. */
	sslog("SlotSync: id %08x%08x psk %u bytes, boot %u, tick %08x\r\n",
	      (u32)((cfg.device_id ^ 0x8000000000000000ull) >> 32),
	      (u32)(cfg.device_id ^ 0x8000000000000000ull),
	      (u32)cfg.psk_len, bootTime, now);
	sslog("SlotSync: saying hello to %u.%u.%u.%u:%u\r\n",
	      (cfg.server >> 24) & 0xFF, (cfg.server >> 16) & 0xFF,
	      (cfg.server >> 8) & 0xFF, cfg.server & 0xFF, (u32)cfg.port);

	rc = ss_hello(&client, NULL, NULL);
	if (rc != SS_OK) {
		sslog("SlotSync: no HELLO answer: rc %d, nack 0x%02x\r\n",
		      rc, (u32)client.last_error_code);
		ssnet_close(&net);
		return -1;
	}
	/* Back to what the config asks for, now that the handshake is done. */
	client.timeout_ms = cfg.timeout_ms;
	client.max_rounds = cfg.rounds;

	sslog("SlotSync: server reachable, runtime sync armed (%d ms x %d)\r\n",
	      client.timeout_ms, client.max_rounds);
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
	int backoff_ms = 2000;

	(void)arg;
	workerRunning = 1;

	while (!workerStop) {
		int slot;

		if (!netConnected) {
			int rc = ss_connect();

			if (rc == 0) {
				netConnected = 1;
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

	if (netConnected) {
		ssnet_close(&net);
		netConnected = 0;
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

		if (ss_load_parent(s->game_id, (u8)slot, &s->parent,
				   s->parent_sha) == 0) {
			s->lineage = 1;
			sslog("SlotSync: slot %c is %s at v%u\r\n", slot + 'A',
			      s->game_id, s->parent);

			/* The launcher's fingerprints for that exact version,
			 * if it left any. This is what makes the *first* push
			 * of the session a delta rather than a whole card --
			 * 5 s of a 2 MiB card, 41 s of a 16 MiB one, paid once
			 * per session for no reason other than the table
			 * starting empty. */
			if (ss_load_fingerprints(slot, s->game_id, s->parent,
						 GCNCard_GetSize(slot),
						 s->parent_sha) == 0) {
				s->fp_valid = 1;
				sslog("SlotSync: slot %c can delta from v%u "
				      "straight away\r\n", slot + 'A', s->parent);
			}
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

	/* Make the first connection attempt here, on the main thread, before the
	 * game is running.
	 *
	 * Not for the connection's sake -- the worker retries on its own -- but
	 * because this is the only point at which the log can be written. FatFs
	 * is built _FS_REENTRANT 0 and the DI thread owns it from the moment the
	 * game starts, so the worker can never touch the card; and the shutdown
	 * flush is on Nintendont's clean-exit path, which a reset skips. Without
	 * this, a console that is reset reports nothing at all, which is the
	 * position every attempt at diagnosing this has started from.
	 *
	 * Bounded, because it delays the game's boot by however long it takes to
	 * fail. */
	{
		/* Bring the interface up here, and nothing else.
		 *
		 * This is the only point at which the log can be written -- the
		 * worker must never touch FatFs while the game runs -- so the
		 * step whose failures needed observing happens here. It is fast
		 * once it works: 1.5 s to a DHCP address.
		 *
		 * The socket and the HELLO stay on the worker. Doing those here
		 * too meant init could sit for the interface timeout plus twelve
		 * two-second HELLO rounds before the game was allowed to start,
		 * which reads as a console wedged after "Init CARD ... Done!".
		 * ss_connect calls bring_up again and it short-circuits on the
		 * address this leaves behind. */
		{
			/* A short budget, not the configured one.
			 *
			 * Everything here happens before the game is allowed to
			 * boot, and on a console with no network at all the full
			 * runtime_net_timeout_ms of 20 s is spent right here, in
			 * silence. Nobody playing offline should pay that.
			 *
			 * Bring-up takes 1.5 s on the Wii and 2.5 s on the Wii U
			 * when it works, so this is generous for the case that
			 * succeeds and quick for the one that cannot. Nothing is
			 * lost by giving up early: ss_connect on the worker tries
			 * again with the real timeout, off the boot path, and if the
			 * interface came up in the meantime bring-up short-circuits
			 * on GETHOSTID. */
			int up = ssnet_bring_up(SS_INIT_NET_BUDGET_MS);

			sslog("SlotSync: network %s at init\r\n",
			      up == 0 ? "up" : "down, worker will retry");
		}
		ss_log_flush();
	}

	slotSyncThread = do_thread_create(SlotSyncWorker,
					  (u32 *)&__slotsync_stack_addr,
					  (u32)(&__slotsync_stack_size), 0x50);
	thread_continue(slotSyncThread);
	return 0;
}

/* Called from Nintendont's main loop.
 *
 * The worker cannot touch FatFs -- _FS_REENTRANT 0, and the DI thread owns the
 * card -- so everything it logs sits in RAM until a main-thread caller writes
 * it out. Tying that to card saves and clean exits meant a reset threw the
 * whole session away, and which of the two ways a run ended decided whether
 * anything could be diagnosed at all.
 *
 * Rate limited, and a no-op unless something was actually logged, so a quiet
 * session costs one comparison per iteration. */
void SlotSync_Poll(void)
{
	static u32 lastFlush;

	if (!workerRunning) {
		return;
	}
	/* Not while a transfer is running.
	 *
	 * This writes the log from the main loop every three seconds, and a
	 * push keeps it dirty the whole time -- so a two-thousand-datagram
	 * transfer came with repeated FatFs writes to the very card the DI
	 * thread is streaming the game from. The console crashed twice while
	 * loading a scene mid-push, and this is the part of that picture we put
	 * there. Diagnostics are not worth a crash now that the transfers work.
	 *
	 * The handoff still goes out, because that one matters. */
	if (workerBusy) {
		return;
	}
	if (!logDirty && !handoffPending) {
		return;
	}
	if (lastFlush != 0 && TimerDiffTicks(lastFlush) < 3000u * SS_TICKS_PER_MS) {
		return;
	}
	lastFlush = read32(HW_TIMER);

	if (handoffPending) {
		handoffPending = 0;
		ss_write_handoff();
		ss_write_fp_handoff();
	}
	ss_log_flush();
}

void SlotSync_NotifyCardSaved(int slot)
{
	if (slot < 0 || slot >= SS_SLOTS || !workerRunning) {
		return;
	}
	slots[slot].dirty = 1;
	slots[slot].dirty_at = read32(HW_TIMER);

	/* Get the log and any owed handoff onto the card while we are here.
	 *
	 * This runs on the main thread, called from GCNCard_Save the moment it
	 * has finished writing the card out -- so FatFs is in use by this very
	 * thread already and there is no safer point in a running game.
	 *
	 * Doing it here rather than only at shutdown is what makes both files
	 * survive a reset. Nintendont's exit combo is B+Z+R+Down and is easy to
	 * miss, and everything the worker did was being lost with it.
	 *
	 * The push for this save has not happened yet, so what lands is the
	 * previous one's outcome. A session with two saves in it therefore
	 * reports the first push, which is all the diagnosis needs. */
	if (handoffPending) {
		handoffPending = 0;
		ss_write_handoff();
		ss_write_fp_handoff();
	}
	ss_log_flush();
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

	/* Main thread, worker stopped: the one safe moment to touch the card. */
	if (handoffPending) {
		handoffPending = 0;
		ss_write_handoff();
		ss_write_fp_handoff();
	}
	ss_log_flush();
}
