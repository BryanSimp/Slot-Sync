/* See SlotSyncNet.h. */

#include "SlotSyncNet.h"

#include "common.h"
#include "string.h"
#include "syscalls.h"
#include "ipc.h"
#include "debug.h"

/* Two levels of socket trace, because they have very different costs.
 *
 * `netdbg` is the setup and error path -- bring-up, bind, the reason a socket
 * could not be opened. It stays on: with it off, ssnet_bring_up and ssnet_open
 * fail into a silent `return -1`, and a runtime sync that never started looks
 * exactly like one with nothing to do. It costs a dozen lines per session.
 *
 * `netio` is per datagram. It is what found the from-address bug and the
 * cache-line bug, and it is worth keeping for the next time something in here
 * behaves impossibly -- but a push is two thousand datagrams, and the log is a
 * fixed 8 KB buffer in a kernel with no allocator, so leaving it on means the
 * useful lines scroll out of the log before anyone reads them. Off by default.
 * Turn it on by defining DEBUG_SLOTSYNC_IO, and note that it also bounds
 * itself to the first thirty calls. */
#define DEBUG_SLOTSYNC 1

#ifndef DEBUG_SLOTSYNC
#define netdbg(...)
#else
#include "SlotSync.h"
#define netdbg SlotSync_Log
#endif

#ifndef DEBUG_SLOTSYNC_IO
#define netio(...)
#define SSNET_IO_TRACE 0
#else
#define netio SlotSync_Log
#define SSNET_IO_TRACE 1
#endif

/* IOS ioctl numbers for /dev/net/ip/top. Lifted from Nintendont's sock.h so
 * the two files cannot drift apart. */
#define SO_BIND            0x02
#define SO_CLOSE           0x03
#define SO_FCNTL           0x05
#define SO_POLL            0x0B
#define SO_RECVFROM        0x0C
#define SO_SENDTO          0x0D
#define SO_SOCKET          0x0F
#define SO_GETINTERFACEOPT 0x1C
/* 0x10 returns the console's IP as the ioctl's own return value -- it is
 * GETHOSTID, not a startup call. This was named SO_STARTUP and issued once
 * before the interface was ever started, so it reported "no IP" every run and
 * was read as a successful no-op. libogc polls it *after* startup, and that is
 * how it decides the interface is up. */
#define SO_GETHOSTID       0x10
/* The call libogc names SO_STARTUP and Nintendont's sock.h names
 * so_startinterface. Same ioctl. */
#define SO_STARTUP         0x1F
/* Link status, through NCD. libogc issues this before anything else. */
#define NCD_GETLINKSTATUS  0x07
#define NWC_STARTUP        0x06
/* NCD: read the saved network configuration (loader/source/main.c). */
#define NCD_GETCONFIG      0x03

/* IOS's own values, which are not newlib's. */
#define SO_F_GETFL     3
#define SO_F_SETFL     4
/* libogc's network.h: #define O_NONBLOCK 04000U -- octal, so 0x800.
 * This was 0x04, which fcntl accepts and reports success for while leaving the
 * socket blocking, which is how a read with data waiting still never returned. */
#define SO_O_NONBLOCK  0x800
/* POLLIN is POLLRDNORM|POLLRDBAND -- 0x0003, not 0x0001.
 *
 * libogc's network.h spells it out:
 *     #define POLLRDNORM 0x0001
 *     #define POLLRDBAND 0x0002
 *     #define POLLIN     (POLLRDNORM|POLLRDBAND)
 *
 * Asking for half of it meant IOS was polled for a condition that never
 * described an arriving datagram, and revents was then tested against the same
 * half value -- so a reply that had genuinely landed still read as nothing. */
#define SO_POLLRDNORM  0x0001
#define SO_POLLRDBAND  0x0002
#define SO_POLLIN      (SO_POLLRDNORM | SO_POLLRDBAND)
/* Worth seeing in the trace when they come back. */
#define SO_POLLERR     0x0020
#define SO_POLLHUP     0x0040
#define SO_POLLNVAL    0x0080

/* The option id that carries the interface's IPv4 address.
 *
 * 0x4003, read straight out of sock.c, which watches for exactly this option
 * going past and keeps its value as the console's current IP:
 *
 *     if(read32((u32)SOVecResA[i]+4) == 0x4003 && ...)
 *             ourCurrentIP = read32((u32)SOVec[i][1].data);
 *
 * This was 0x1003, which is a valid option the ioctl answers successfully with
 * a value of zero -- so the interface read as permanently down no matter what
 * state it was actually in, and every layer below was blamed in turn. */
#define SO_IFOPT_IPADDR 0x4003

/* IOS wants every ioctl buffer 32-byte aligned, and the protocol core hands us
 * datagrams built on the caller's stack. Everything crossing the IOS boundary
 * is therefore bounced through the statics below. This module is only ever
 * entered from the single SlotSync worker thread, so they need no locking --
 * if that ever stops being true, this is the thing that breaks first.
 */
#define SSNET_BOUNCE_SIZE 1152 /* SS_MAX_DATAGRAM (1120), rounded up to 32 */

typedef struct {
	u8 len;
	u8 family;
	u16 port;
	u32 addr;
} ssnet_sockaddr; /* 8 bytes, the size IOS expects */

struct ssnet_bind_params {
	u32 socket;
	u32 has_name;
	u8 name[28];
};

struct ssnet_sendto_params {
	u32 socket;
	u32 flags;
	u32 has_destaddr;
	u8 destaddr[28];
};

struct ssnet_pollsd {
	s32 socket;
	u32 events;
	u32 revents;
};

/* SOCKInit (kernel/sock.c) opens both of these at kernel start and holds them
 * for the whole session. Use those handles rather than opening our own.
 *
 * IOS hands /dev/net/kd/request out exactly once, so our own open of it always
 * returned -10 -- not "no such device", but "Nintendont already has it". And
 * the interface is started through the same handle the BBA path starts it on,
 * which is the one combination known to work in this environment. */
extern int sockFd, nwcFd;

static s32 ipTopFd = -1;   /* SOCKInit's handle: interface bring-up only */

/* Our own /dev/net/ip/top, for the socket itself.
 *
 * The bring-up has to go through SOCKInit's handle -- that is the one IOS
 * associates the interface with. But sharing it for data was the last thing
 * standing: sock.c drives that handle asynchronously from its SOCKAlarm thread,
 * and a synchronous recvfrom issued on it waits for a reply that is delivered
 * to sock.c's message queue instead of waking us. A send returns immediately so
 * it never noticed; a receive waits for ever, with the datagram sitting right
 * there and poll saying so. */
/* Our own /dev/net/ip/top for the data path.
 *
 * Tried once before and dismissed, but that test predated the fromAddr length
 * fix, so the HELLO could not have completed on any handle -- the conclusion
 * was drawn from a run that was broken for an unrelated reason.
 *
 * The timing points here: the HELLO succeeds moments after the socket is made,
 * and the push fails a minute later, after the game has started and
 * Nintendont's own socket layer has become busy -- SOCKUpdateRegisters runs
 * every main-loop iteration with BBA emulation on, against the very handle we
 * were borrowing. A send that returns 96 and never leaves is what losing that
 * race would look like. */
static s32 dataFd = -1;
static s32 ncdFd = -1;
static u32 currentIP;

/* Bounded budget for the per-datagram trace. The push resets it through
 * ssnet_trace_reset so the handshake does not spend the whole of it before the
 * interesting part. */
static int traceCalls;

void ssnet_trace_reset(void)
{
	traceCalls = 0;
}

static char devName[32] ALIGNED(32);
static u32 params[8] ALIGNED(32);
static u32 ioBuf[8] ALIGNED(32);
static struct ssnet_bind_params bindParams ALIGNED(32);
static struct ssnet_sendto_params sendParams ALIGNED(32);
/* One whole cache line, not twelve bytes.
 *
 * The pollsd itself is {s32, u32, u32} -- the layout libogc's network.h uses
 * and Nintendont's sock.c agrees with. But IOS wants ioctl buffers in 32-byte
 * units, and handing it a 12-byte one is a plausible reason for revents coming
 * back untouched while the call still reports a ready descriptor. The extra
 * words are also a window: if IOS writes the flags at some other offset, the
 * trace below will show them. */
static u32 pollBuf[8] ALIGNED(32);
#define pollSd (*(struct ssnet_pollsd *)pollBuf)
static ioctlv vec[3] ALIGNED(32);
static u8 bounceOut[SSNET_BOUNCE_SIZE] ALIGNED(32);
static u8 bounceIn[SSNET_BOUNCE_SIZE] ALIGNED(32);
static ssnet_sockaddr fromAddr ALIGNED(32);


static int ssnet_ip_plausible(u32 ip);

static s32 ssnet_open_dev(const char *path)
{
	memset(devName, 0, sizeof(devName));
	strncpy(devName, path, sizeof(devName) - 1);
	sync_after_write_align32(devName, sizeof(devName));
	return IOS_Open(devName, 0);
}

u32 ssnet_local_ip(void)
{
	return currentIP;
}

/* Ask IOS for the address now, rather than reporting the one we cached at
 * bring-up. If the interface quietly goes away once the game is loading the
 * disc hard -- which is the shape of a HELLO that always works and a push that
 * usually does not -- this is what says so. */
u32 ssnet_live_ip(void)
{
	int ret;

	if (ipTopFd < 0) {
		return 0;
	}
	ret = IOS_Ioctl(ipTopFd, SO_GETHOSTID, NULL, 0, NULL, 0);
	return ssnet_ip_plausible((u32)ret) ? (u32)ret : 0;
}


/* Does this look like an address DHCP handed out?
 *
 * SO_GETINTERFACEOPT can return success and still leave a small negative IOS
 * error in the value buffer -- 0xffffff90, which is -112, is what a console
 * with an unconfigured interface actually produced. Testing it for `!= 0` read
 * that as an IP of 255.255.255.144, declared the interface up, and left the
 * socket call to fail with a code that pointed nowhere near the real problem.
 *
 * A leased IPv4 never has 0, 127 or 255 in its top octet, and every negative
 * error code does. That is the whole check. */
static int ssnet_ip_plausible(u32 ip)
{
	u32 first = ip >> 24;

	return ip != 0 && first != 0 && first != 127 && first != 255;
}

int ssnet_bring_up(int timeout_ms)
{
	int waited;
	int ret;
	int tries;

	if (ipTopFd < 0) {
		ipTopFd = sockFd;
		if (ipTopFd < 0) {
			netdbg("SlotSync: SOCKInit has no ip/top handle\r\n");
			return -1;
		}
		netdbg("SlotSync: using SOCKInit fds: ip/top %d, kd %d\r\n",
		       ipTopFd, nwcFd);

		/* Do not open /dev/net/kd/request here -- SOCKInit holds it and a
		 * second open just returns -10. Do not probe /dev/net/wd/command
		 * either: it never returns, and this runs on the main thread
		 * during init, so it wedges the console before the game starts. */
		ncdFd = ssnet_open_dev("/dev/net/ncd/manage");
		netdbg("SlotSync: ncd/manage: %d\r\n", ncdFd);

		dataFd = ssnet_open_dev("/dev/net/ip/top");
		netdbg("SlotSync: our own ip/top: %d\r\n", dataFd);
	}

	/* Already up? GETHOSTID answers with the address itself. */
	ret = IOS_Ioctl(ipTopFd, SO_GETHOSTID, NULL, 0, NULL, 0);
	if (ssnet_ip_plausible((u32)ret)) {
		currentIP = (u32)ret;
		netdbg("SlotSync: interface was already up, IP %08x\r\n", currentIP);
		return 0;
	}

	/* The order below is libogc's net_init_chain, which is the sequence
	 * known to bring this interface up on this hardware. What was here
	 * before called GETHOSTID first believing it to be a startup, never
	 * asked NCD for link status, gave NWC24 a single attempt, and then
	 * waited on SO_GETINTERFACEOPT -- which reports nothing until the
	 * stack has actually come up. */

	/* 1. Link status through NCD. */
	if (ncdFd >= 0) {
		memset(ioBuf, 0, sizeof(ioBuf));
		sync_after_write_align32(ioBuf, sizeof(ioBuf));
		vec[0].data = ioBuf;
		vec[0].len = 0x20;
		sync_after_write_align32(vec, sizeof(vec));
		ret = IOS_Ioctlv(ncdFd, NCD_GETLINKSTATUS, 0, 1, vec);
		sync_before_read_align32(ioBuf, sizeof(ioBuf));
		netdbg("SlotSync: ncd linkstatus: %d\r\n", ret);
	}

	/* 2. NWC24 startup, retried on -29 the way libogc retries it. A single
	 *    attempt was treated as conclusive before; libogc allows 32. */
	ret = 0;
	for (tries = 0; tries < 32; tries++) {
		memset(ioBuf, 0, sizeof(ioBuf));
		sync_after_write_align32(ioBuf, sizeof(ioBuf));
		ret = IOS_Ioctl(nwcFd, NWC_STARTUP, NULL, 0, ioBuf, 0x20);
		if (ret != -29) {
			break;
		}
		mdelay(100);
	}
	netdbg("SlotSync: nwc24 startup: %d after %d tries\r\n", ret, tries);

	/* 3. Start the socket stack. */
	ret = IOS_Ioctl(ipTopFd, SO_STARTUP, NULL, 0, NULL, 0);
	netdbg("SlotSync: so_startup: %d\r\n", ret);

	/* 4. Poll GETHOSTID for the address.
	 *
	 *    Its return IS the address, so it must be read as unsigned: a
	 *    perfectly good 192.168.x.x is 0xC0A8.... and negative as an int. */
	for (waited = 0; waited < timeout_ms; waited += 100) {
		ret = IOS_Ioctl(ipTopFd, SO_GETHOSTID, NULL, 0, NULL, 0);
		if (ssnet_ip_plausible((u32)ret)) {
			currentIP = (u32)ret;
			netdbg("SlotSync: interface up after %d ms, IP %08x\r\n",
			       waited, currentIP);
			return 0;
		}
		if ((waited % 2000) == 0) {
			netdbg("SlotSync: waiting at %d ms: gethostid %08x\r\n",
			       waited, (u32)ret);
		}
		mdelay(100);
	}

	netdbg("SlotSync: interface did not come up within %d ms (gethostid %08x)\r\n",
	       timeout_ms, (u32)ret);
	return -1;
}

int ssnet_open(ssnet *n, u32 addr, u16 port)
{
	int ret;

	memset(n, 0, sizeof(*n));
	n->sock = -1;
	n->peer_addr = addr;
	n->peer_port = port;
	/* Pace by default. A 2 MiB card is 2048 datagrams; handing IOS all of
	 * them without pause overruns its receive path long before it troubles
	 * the network (PLAN.md section 4), starves the game's own IO, and runs
	 * into the server's UDP rate limit. SlotSync.c overwrites these from the
	 * config; the values here are what an unconfigured open gets. */
	n->pace_every = 8;
	n->pace_us = 5000;

	if (dataFd < 0) {
		return -1;
	}

	params[0] = SSNET_AF_INET;
	params[1] = SSNET_SOCK_DGRAM;
	params[2] = SSNET_IPPROTO_IP;
	sync_after_write_align32(params, 32);
	ret = IOS_Ioctl(dataFd, SO_SOCKET, params, 12, NULL, 0);
	if (ret < 0) {
		netdbg("SlotSync: socket failed: %d\r\n", ret);
		return -1;
	}
	n->sock = ret;

	/* Bind to a definite local port.
	 *
	 * Port 0 -- "pick one for me" -- is refused by IOS with -28, which maps
	 * to EINVAL through libogc's error table. That is not how BSD behaves,
	 * and it is what failed the first time the interface was ever actually
	 * up. So name real ports instead.
	 *
	 * And if IOS refuses all of them, carry on unbound rather than giving
	 * up: the claim that it will not route an unbound sendto was an
	 * assumption, never tested, because nothing ever got this far. */
	/* Do not bind.
	 *
	 * The server receives every HELLO -- its device list keeps advancing --
	 * and answers to whatever source port the datagram carried. Binding
	 * announces 49152, but nothing here has ever confirmed IOS actually
	 * sends from it, and a reply addressed to a port this socket does not
	 * own would look exactly like what we see: send fine, server happy,
	 * nothing ever received.
	 *
	 * An unbound UDP client gets a source port assigned by the stack at
	 * first send, and replies to it by construction. That is the ordinary
	 * way to write this and removes the assumption entirely. */
	{
		static const u16 candidates[3] = { 49152, 49153, 49154 };
		int bound = 0;
		int i;

		for (i = 0; i < 3 && !bound; i++) {
			memset(&bindParams, 0, sizeof(bindParams));
			bindParams.socket = (u32)n->sock;
			bindParams.has_name = 1;
			bindParams.name[0] = 8;
			bindParams.name[1] = SSNET_AF_INET;
			bindParams.name[2] = (u8)(candidates[i] >> 8);
			bindParams.name[3] = (u8)(candidates[i] & 0xFF);
			/* address left zero: any interface */
			sync_after_write_align32(&bindParams, sizeof(bindParams));
			ret = IOS_Ioctl(dataFd, SO_BIND, &bindParams,
					sizeof(bindParams), NULL, 0);
			if (ret >= 0) {
				bound = 1;
				netdbg("SlotSync: bound to port %u\r\n",
				       (u32)candidates[i]);
			} else {
				netdbg("SlotSync: bind port %u: %d\r\n",
				       (u32)candidates[i], ret);
			}
		}
		if (!bound) {
			netdbg("SlotSync: no bind, letting sendto assign one\r\n");
		}
	}


	/* Non-blocking, so a lost reply can never park this thread inside IOS
	 * for longer than our own timeout. */
	params[0] = (u32)n->sock;
	params[1] = SO_F_SETFL;
	params[2] = SO_O_NONBLOCK;
	sync_after_write_align32(params, 32);
	ret = IOS_Ioctl(dataFd, SO_FCNTL, params, 12, NULL, 0);
	netdbg("SlotSync: fcntl set %04x: %d\r\n", SO_O_NONBLOCK, ret);

	/* Read it back. fcntl answering 0 proves nothing -- it did that for
	 * 0x04 too, and the socket stayed blocking. */
	params[0] = (u32)n->sock;
	params[1] = SO_F_GETFL;
	params[2] = 0;
	sync_after_write_align32(params, 32);
	ret = IOS_Ioctl(dataFd, SO_FCNTL, params, 12, NULL, 0);
	netdbg("SlotSync: fcntl getfl: %08x %s\r\n", ret,
	       (ret >= 0 && (ret & SO_O_NONBLOCK)) ? "NONBLOCK set" : "still blocking");
	if (ret < 0) {
		/* Not fatal: ssnet_recv's own deadline still bounds the wait. */
		netdbg("SlotSync: fcntl O_NONBLOCK failed: %d\r\n", ret);
	}

	return 0;
}

void ssnet_close(ssnet *n)
{
	if (n->sock < 0 || ipTopFd < 0) {
		n->sock = -1;
		return;
	}
	params[0] = (u32)n->sock;
	sync_after_write_align32(params, 32);
	IOS_Ioctl(dataFd, SO_CLOSE, params, 4, NULL, 0);
	n->sock = -1;
}

int ssnet_send(void *ctx, const u8 *data, size_t len)
{
	ssnet *n = (ssnet *)ctx;
	int ret;

	if (n->sock < 0 || len > sizeof(bounceOut)) {
		return -1;
	}

	memcpy(bounceOut, data, len);
	sync_after_write_align32(bounceOut, (int)((len + 31) & ~31u));

	memset(&sendParams, 0, sizeof(sendParams));
	sendParams.socket = (u32)n->sock;
	sendParams.flags = 0;
	sendParams.has_destaddr = 1;
	sendParams.destaddr[0] = 8;
	sendParams.destaddr[1] = SSNET_AF_INET;
	sendParams.destaddr[2] = (u8)(n->peer_port >> 8);
	sendParams.destaddr[3] = (u8)(n->peer_port & 0xFF);
	sendParams.destaddr[4] = (u8)(n->peer_addr >> 24);
	sendParams.destaddr[5] = (u8)(n->peer_addr >> 16);
	sendParams.destaddr[6] = (u8)(n->peer_addr >> 8);
	sendParams.destaddr[7] = (u8)(n->peer_addr);
	sync_after_write_align32(&sendParams, sizeof(sendParams));

	vec[0].data = bounceOut;
	vec[0].len = len;
	vec[1].data = &sendParams;
	vec[1].len = sizeof(sendParams);
	sync_after_write_align32(vec, sizeof(vec));

	/* Bracket the first few, so a call that never returns can be told from
	 * one that returned an error. The log is written from the main loop
	 * every three seconds, so whichever line is last is the one that hung. */
	if (SSNET_IO_TRACE && traceCalls < 30) {
		/* Destination included: a HELLO that arrives and a PUSH_BEGIN
		 * that does not, both 96 bytes on the same socket, is what a
		 * corrupted peer address would look like -- and sendParams is a
		 * static that every send rewrites. */
		netio("SlotSync: sendto> %u bytes to %u.%u.%u.%u:%u sock %d\r\n",
		       (u32)len,
		       (n->peer_addr >> 24) & 0xFF, (n->peer_addr >> 16) & 0xFF,
		       (n->peer_addr >> 8) & 0xFF, n->peer_addr & 0xFF,
		       (u32)n->peer_port, n->sock);
	}
	/* One attempt, and no delay loop.
	 *
	 * This retried eight times with udelay(2000) between -- and udelay in
	 * this kernel creates an IOS message queue and a timer per call and
	 * tears them down again. At eight of those per refused send, across a
	 * two-thousand-datagram push, that is thousands of IOS objects churned
	 * while the game is loading a scene. The console crashed mid-push doing
	 * exactly that.
	 *
	 * The retries bought nothing in any case: a refused send is counted
	 * below as a dropped datagram, and the server asks for what it is
	 * missing. Retransmission already handles this. */
	ret = IOS_Ioctlv(dataFd, SO_SENDTO, 2, 0, vec);

	if (SSNET_IO_TRACE && traceCalls < 30) {
		netio("SlotSync: sendto< %d\r\n", ret);
		traceCalls++;
	}

	/* Pacing lives here rather than in the portable core so the core stays
	 * the same code the host tests and the libogc wrapper run. */
	if (n->pace_every != 0) {
		if (++n->sent_since_pause >= n->pace_every) {
			n->sent_since_pause = 0;
			udelay((int)n->pace_us);
		}
	}

	if (ret < 0) {
		/* A refused send is a dropped datagram, not a failed transfer.
		 *
		 * The protocol is built for loss -- the server counts what it is
		 * missing and asks for it, and a push routinely recovers from
		 * hundreds of gaps. Reporting this upward as SS_ERR_TRANSPORT
		 * abandons the whole card instead, which is how a push the
		 * server went on to commit came back here as "network error",
		 * leaving the client on a stale parent and conflicting on the
		 * next save.
		 *
		 * So: count it, say it was sent, and let retransmission do the
		 * job it already does well. */
		n->dropped++;
		return (int)len;
	}
	return ret;
}

/* One non-blocking recvfrom. Returns bytes read, 0 for "nothing waiting", or
 * a negative IOS error. */
static int ssnet_try_recv(ssnet *n, u8 *buf, size_t cap)
{
	int ret;

	ioBuf[0] = (u32)n->sock;
	ioBuf[1] = 0; /* flags */
	sync_after_write_align32(ioBuf, 32);
	memset(bounceIn, 0, cap);
	sync_after_write_align32(bounceIn, (int)((cap + 31) & ~31u));
	/* Give the from-address its length byte before the call.
	 *
	 * Every sockaddr IOS accepts carries its own size in byte 0 -- sendto's
	 * destaddr is set to 8 for exactly that reason -- and sock.c pre-fills
	 * this buffer rather than handing over a zeroed one. A recvfrom whose
	 * from-address announces a length of zero is a plausible reason for IOS
	 * to take the call and never complete it, which is what a read that
	 * blocks with a datagram waiting actually looks like. */
	memset(&fromAddr, 0, sizeof(fromAddr));
	fromAddr.len = 8;
	fromAddr.family = SSNET_AF_INET;
	sync_after_write_align32(&fromAddr, sizeof(fromAddr));

	vec[0].data = ioBuf;
	vec[0].len = 8;
	vec[1].data = bounceIn;
	vec[1].len = cap;
	vec[2].data = &fromAddr;
	vec[2].len = 8;
	sync_after_write_align32(vec, sizeof(vec));

	ret = IOS_Ioctlv(dataFd, SO_RECVFROM, 1, 2, vec);
	if (ret > 0) {
		if ((size_t)ret > cap) {
			return -1;
		}
		sync_before_read_align32(bounceIn, (int)((cap + 31) & ~31u));
		memcpy(buf, bounceIn, (size_t)ret);
		return ret;
	}
	/* Anything else means "nothing for us yet". A non-blocking socket with
	 * an empty queue and a socket in a bad state are not distinguishable
	 * here without pinning down IOS's errno values, and the distinction
	 * does not change what we do: the caller's deadline ends the wait
	 * either way, and the client layer treats a timeout as normal. */
	return 0;
}

int ssnet_recv(void *ctx, u8 *buf, size_t cap, int timeout_ms)
{
	ssnet *n = (ssnet *)ctx;
	int waited = 0;
	int step = 4;
	int pollRet;

	if (n->sock < 0) {
		return -1;
	}
	if (cap > sizeof(bounceIn)) {
		cap = sizeof(bounceIn);
	}

	/* Poll first, and only read when IOS says something is waiting.
	 *
	 * This used to try the read first and poll only if it came back empty,
	 * which relies on the socket really being non-blocking. It was not --
	 * the O_NONBLOCK fcntl is issued but its result was never checked -- so
	 * the read blocked inside IOS and the worker never came back, with the
	 * log stopping dead after a successful sendto.
	 *
	 * Asking first costs one ioctl and does not care whether the fcntl took
	 * effect, which is a better thing to depend on than an unchecked call.
	 */
	for (;;) {
		pollSd.socket = n->sock;
		pollSd.events = SO_POLLIN;
		pollSd.revents = 0;
		sync_after_write_align32(pollBuf, sizeof(pollBuf));
		params[0] = 0;
		params[1] = (u32)step;
		sync_after_write_align32(params, 32);

		if (SSNET_IO_TRACE && traceCalls < 30) {
			netio("SlotSync: poll> %d ms\r\n", step);
		}
		pollRet = IOS_Ioctl(dataFd, SO_POLL, params, 8, pollBuf,
				    sizeof(pollBuf));
		/* _align32, not the raw call.
		 *
		 * sync_before_read wants 32-byte granularity, and this struct is
		 * twelve bytes -- so the invalidate covered nothing and revents
		 * was read straight out of a stale cache line, reading 0000
		 * forever while IOS was writing it correctly all along. That is
		 * why poll answered 1 and the flags never moved. */
		sync_before_read_align32(pollBuf, sizeof(pollBuf));
		if (SSNET_IO_TRACE && traceCalls < 30) {
			netio("SlotSync: poll< %d buf %08x %08x %08x %08x\r\n",
			       pollRet, pollBuf[0], pollBuf[1], pollBuf[2],
			       pollBuf[3]);
			traceCalls++;
		}

		/* Both, and neither alone.
		 *
		 * The return value looked like the only trustworthy half for a
		 * long time, because revents read back 0000 while IOS answered
		 * 1. That was this end's fault: the invalidate before the read
		 * used the raw sync_before_read on a twelve-byte struct, so it
		 * covered nothing and revents came out of a stale cache line.
		 * With the _align32 call above, IOS fills it in correctly --
		 * pollBuf[2] goes to 1 exactly when a reply has landed.
		 *
		 * The return value on its own is not enough either: it reads 1
		 * while revents is still 0, on a socket with nothing waiting.
		 * Reading on that answers -6 forever. So gate on both. */
		if (pollRet > 0 && (pollSd.revents & SO_POLLIN)) {
			int got = ssnet_try_recv(n, buf, cap);

			if (SSNET_IO_TRACE && traceCalls < 30) {
				netio("SlotSync: recv< %d\r\n", got);
				traceCalls++;
			}
			if (got > 0) {
				return got;
			}
		} else if (pollRet < 0) {
			/* Poll unavailable. Sleep rather than reading blind:
			 * a read on a socket that turns out to be blocking is
			 * how this hung in the first place. */
			mdelay(step);
		}

		waited += step;
		if (waited >= timeout_ms) {
			return 0; /* a timeout is a normal outcome, per client.h */
		}

		/* Back off from 4 ms to 32 ms. A server on the same LAN answers
		 * inside the first step; a dead one costs us a handful of
		 * ioctls rather than hundreds. */
		if (step < 32) {
			step *= 2;
		}
	}
}
