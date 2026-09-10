/* UDP for Nintendont's ARM kernel, straight onto IOS /dev/net/ip/top.
 *
 * Nintendont already opens that device unconditionally in SOCKInit() so the
 * BBA emulation can forward a game's socket calls to it. We borrow the same
 * device and open a socket of our own. Nothing here goes through the
 * PPC-facing proxy in sock.c: the game's sockets and ours are independent
 * IOS file descriptors that happen to share one driver handle.
 *
 * Every ioctl encoding below is read off Nintendont's own sock.c rather than
 * recalled, because that file is the one authority on this machine for what
 * IOS expects. See nintendont/README.md for the derivation of each one.
 */

#ifndef SLOTSYNC_NET_H
#define SLOTSYNC_NET_H

#include <stddef.h>

#include "global.h"

/* IOS socket constants. These are IOS's values, not newlib's -- notably
 * SOCK_DGRAM is 2 here and O_NONBLOCK is 4. */
#define SSNET_AF_INET     2
#define SSNET_SOCK_DGRAM  2
/* 0, not 17. IOS takes the protocol argument raw and rejects IPPROTO_UDP with
 * -68 (EPROTONOSUPPORT); SOCK_DGRAM is what makes the socket UDP. Confirmed on
 * hardware -- 17 is what made the launcher fail to open a socket at all. */
#define SSNET_IPPROTO_IP  0

typedef struct {
	s32 sock;         /* IOS socket fd, or < 0 when closed */
	u32 peer_addr;    /* server IPv4, host order */
	u16 peer_port;
	u32 dropped;      /* sends IOS refused; recovered by retransmission */
	u32 sent_since_pause; /* pacing counter, see ssnet_send */
	u32 pace_every;   /* datagrams between pauses; 0 disables pacing */
	u32 pace_us;      /* microseconds to pause for */
} ssnet;

/* Bring the Wii's network interface up, if it is not already.
 *
 * Returns 0 once an IP address is assigned, -1 on failure. Blocking, and
 * potentially for several seconds on WiFi, so it must only be called from the
 * SlotSync worker thread -- never from the kernel main loop.
 */
int ssnet_bring_up(int timeout_ms);

/* The interface's current IPv4 address in host order, or 0 if it has none. */
u32 ssnet_local_ip(void);

/* The address IOS reports right now, or 0 if the interface has gone. */
u32 ssnet_live_ip(void);

/* Open a bound UDP socket aimed at `addr`:`port` (addr in host order). */
int ssnet_open(ssnet *n, u32 addr, u16 port);
void ssnet_close(ssnet *n);

/* Start the per-datagram trace's budget over. Does nothing unless the build
 * defines DEBUG_SLOTSYNC_IO, so callers need no #ifdef of their own. */
void ssnet_trace_reset(void);

/* ss_transport callbacks. Signatures match client.h exactly so they can be
 * installed as function pointers with no shim. */
int ssnet_send(void *ctx, const u8 *data, size_t len);
int ssnet_recv(void *ctx, u8 *buf, size_t cap, int timeout_ms);

#endif /* SLOTSYNC_NET_H */
