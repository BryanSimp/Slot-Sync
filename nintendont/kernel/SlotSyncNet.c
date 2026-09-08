/* See SlotSyncNet.h. */

#include "SlotSyncNet.h"

#include "common.h"
#include "string.h"
#include "syscalls.h"
#include "ipc.h"
#include "debug.h"

#ifndef DEBUG_SLOTSYNC
#define netdbg(...)
#else
extern int dbgprintf(const char *fmt, ...);
#define netdbg dbgprintf
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
#define SO_STARTINTERFACE  0x1F
#define NWC_STARTUP        0x06

/* IOS's own values, which are not newlib's. */
#define SO_F_SETFL     4
#define SO_O_NONBLOCK  4
#define SO_POLLIN      0x0001

/* The option id that carries the interface's IPv4 address. sock.c singles this
 * one out by number when tracing, and stores its 4-byte result as the console's
 * current IP. */
#define SO_IFOPT_IPADDR 0x1003

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

static s32 ipTopFd = -1;
static s32 nwcFd = -1;
static u32 currentIP;

static char devName[32] ALIGNED(32);
static u32 params[8] ALIGNED(32);
static u32 ioBuf[8] ALIGNED(32);
static u32 optHdr[8] ALIGNED(32);
static u32 optLenBuf[8] ALIGNED(32);
static struct ssnet_bind_params bindParams ALIGNED(32);
static struct ssnet_sendto_params sendParams ALIGNED(32);
static struct ssnet_pollsd pollSd ALIGNED(32);
static ioctlv vec[3] ALIGNED(32);
static u8 bounceOut[SSNET_BOUNCE_SIZE] ALIGNED(32);
static u8 bounceIn[SSNET_BOUNCE_SIZE] ALIGNED(32);
static ssnet_sockaddr fromAddr ALIGNED(32);

static s32 ssnet_open_dev(const char *path)
{
	memset(devName, 0, sizeof(devName));
	strncpy(devName, path, sizeof(devName) - 1);
	sync_after_write(devName, sizeof(devName));
	return IOS_Open(devName, 0);
}

u32 ssnet_local_ip(void)
{
	return currentIP;
}

/* One SO_GETINTERFACEOPT round trip. The three-vector shape is exactly what
 * sock.c builds for the PPC side: an 8-byte {0xFFFE, option} header in, the
 * value out, and the value's length out. */
static int ssnet_get_ifopt(u32 option, void *out, u32 out_len)
{
	int ret;

	optHdr[0] = 0xFFFE;
	optHdr[1] = option;
	optLenBuf[0] = out_len;
	memset(out, 0, out_len);

	sync_after_write(optHdr, 32);
	sync_after_write(optLenBuf, 32);
	sync_after_write(out, out_len);

	vec[0].data = optHdr;
	vec[0].len = 8;
	vec[1].data = out;
	vec[1].len = out_len;
	vec[2].data = optLenBuf;
	vec[2].len = 4;
	sync_after_write(vec, sizeof(vec));

	ret = IOS_Ioctlv(ipTopFd, SO_GETINTERFACEOPT, 1, 2, vec);
	sync_before_read(out, out_len);
	return ret;
}

int ssnet_bring_up(int timeout_ms)
{
	int waited;
	int ret;

	if (ipTopFd < 0) {
		nwcFd = ssnet_open_dev("/dev/net/kd/request");
		ipTopFd = ssnet_open_dev("/dev/net/ip/top");
		if (ipTopFd < 0) {
			netdbg("SlotSync: cannot open /dev/net/ip/top: %d\r\n", ipTopFd);
			return -1;
		}
	}

	/* If the interface is already up -- because a BBA-emulated game started
	 * it, or because we did on an earlier attempt -- we are done. Asking
	 * first keeps us from restarting a live interface underneath a game. */
	if (ssnet_get_ifopt(SO_IFOPT_IPADDR, ioBuf, 4) >= 0 && ioBuf[0] != 0) {
		currentIP = ioBuf[0];
		return 0;
	}

	/* NWC24 first, then the socket interface. This is the order libogc's
	 * net_init uses and the order IOS expects; SO_STARTINTERFACE on its own
	 * comes back with the interface still unconfigured. */
	if (nwcFd >= 0) {
		memset(ioBuf, 0, sizeof(ioBuf));
		sync_after_write(ioBuf, sizeof(ioBuf));
		ret = IOS_Ioctl(nwcFd, NWC_STARTUP, NULL, 0, ioBuf, 0x20);
		netdbg("SlotSync: NWC24 startup: %d\r\n", ret);
		(void)ret; /* only read by the trace build */
	}

	ret = IOS_Ioctl(ipTopFd, SO_STARTINTERFACE, NULL, 0, NULL, 0);
	netdbg("SlotSync: SOStartInterface: %d\r\n", ret);
	(void)ret;

	/* DHCP. On WiFi this genuinely can take seconds, which is why the whole
	 * of SlotSync lives on its own thread. */
	for (waited = 0; waited < timeout_ms; waited += 200) {
		if (ssnet_get_ifopt(SO_IFOPT_IPADDR, ioBuf, 4) >= 0 && ioBuf[0] != 0) {
			currentIP = ioBuf[0];
			netdbg("SlotSync: interface up, IP %08x\r\n", currentIP);
			return 0;
		}
		mdelay(200);
	}

	netdbg("SlotSync: interface did not come up within %d ms\r\n", timeout_ms);
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

	if (ipTopFd < 0) {
		return -1;
	}

	params[0] = SSNET_AF_INET;
	params[1] = SSNET_SOCK_DGRAM;
	params[2] = SSNET_IPPROTO_UDP;
	sync_after_write(params, 32);
	ret = IOS_Ioctl(ipTopFd, SO_SOCKET, params, 12, NULL, 0);
	if (ret < 0) {
		netdbg("SlotSync: socket failed: %d\r\n", ret);
		return -1;
	}
	n->sock = ret;

	/* Bind to an ephemeral port on every interface. IOS will not route a
	 * sendto from an unbound datagram socket. */
	memset(&bindParams, 0, sizeof(bindParams));
	bindParams.socket = (u32)n->sock;
	bindParams.has_name = 1;
	bindParams.name[0] = 8;
	bindParams.name[1] = SSNET_AF_INET;
	/* port 0, address 0: let IOS choose */
	sync_after_write(&bindParams, sizeof(bindParams));
	ret = IOS_Ioctl(ipTopFd, SO_BIND, &bindParams, sizeof(bindParams), NULL, 0);
	if (ret < 0) {
		netdbg("SlotSync: bind failed: %d\r\n", ret);
		ssnet_close(n);
		return -1;
	}

	/* Non-blocking, so a lost reply can never park this thread inside IOS
	 * for longer than our own timeout. */
	params[0] = (u32)n->sock;
	params[1] = SO_F_SETFL;
	params[2] = SO_O_NONBLOCK;
	sync_after_write(params, 32);
	ret = IOS_Ioctl(ipTopFd, SO_FCNTL, params, 12, NULL, 0);
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
	sync_after_write(params, 32);
	IOS_Ioctl(ipTopFd, SO_CLOSE, params, 4, NULL, 0);
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
	sync_after_write(bounceOut, (int)((len + 31) & ~31u));

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
	sync_after_write(&sendParams, sizeof(sendParams));

	vec[0].data = bounceOut;
	vec[0].len = len;
	vec[1].data = &sendParams;
	vec[1].len = sizeof(sendParams);
	sync_after_write(vec, sizeof(vec));

	ret = IOS_Ioctlv(ipTopFd, SO_SENDTO, 2, 0, vec);

	/* Pacing lives here rather than in the portable core so the core stays
	 * the same code the host tests and the libogc wrapper run. */
	if (n->pace_every != 0) {
		if (++n->sent_since_pause >= n->pace_every) {
			n->sent_since_pause = 0;
			udelay((int)n->pace_us);
		}
	}

	if (ret < 0) {
		netdbg("SlotSync: sendto failed: %d\r\n", ret);
		return -1;
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
	sync_after_write(ioBuf, 32);
	memset(bounceIn, 0, cap);
	sync_after_write(bounceIn, (int)((cap + 31) & ~31u));
	memset(&fromAddr, 0, sizeof(fromAddr));
	sync_after_write(&fromAddr, sizeof(fromAddr));

	vec[0].data = ioBuf;
	vec[0].len = 8;
	vec[1].data = bounceIn;
	vec[1].len = cap;
	vec[2].data = &fromAddr;
	vec[2].len = 8;
	sync_after_write(vec, sizeof(vec));

	ret = IOS_Ioctlv(ipTopFd, SO_RECVFROM, 1, 2, vec);
	if (ret > 0) {
		if ((size_t)ret > cap) {
			return -1;
		}
		sync_before_read(bounceIn, (int)((cap + 31) & ~31u));
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

	if (n->sock < 0) {
		return -1;
	}
	if (cap > sizeof(bounceIn)) {
		cap = sizeof(bounceIn);
	}

	for (;;) {
		int got = ssnet_try_recv(n, buf, cap);
		if (got > 0) {
			return got;
		}
		if (waited >= timeout_ms) {
			return 0; /* a timeout is a normal outcome, per client.h */
		}

		/* Poll IOS for readiness rather than spinning on recvfrom: one
		 * ioctl per step either way, but this one sleeps inside IOS and
		 * returns early when the reply lands. */
		pollSd.socket = n->sock;
		pollSd.events = SO_POLLIN;
		pollSd.revents = 0;
		sync_after_write(&pollSd, sizeof(pollSd));
		params[0] = 0;
		params[1] = (u32)step;
		sync_after_write(params, 32);
		if (IOS_Ioctl(ipTopFd, SO_POLL, params, 8, &pollSd, sizeof(pollSd)) < 0) {
			/* Poll unavailable: fall back to sleeping ourselves. */
			mdelay(step);
		}
		waited += step;

		/* Back off from 4 ms to 32 ms. A server on the same LAN answers
		 * inside the first step; a dead one costs us a handful of
		 * ioctls rather than hundreds. */
		if (step < 32) {
			step *= 2;
		}
	}
}
