/* libogc UDP transport for the SlotSync client.
 *
 * Fills in the two function pointers in ss_transport so the portable core in
 * ../core runs unchanged on the console. The host tests use the BSD-socket
 * equivalent in ../tests/host_socket.c; the core cannot tell them apart.
 */

#ifndef SLOTSYNC_WII_NET_H
#define SLOTSYNC_WII_NET_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int fd;
    unsigned char addr[32]; /* opaque struct sockaddr_in */
    /* The last send or recv that actually failed, for the on-screen report.
     * The core's transport contract is only "negative means failure", so
     * without this a send failing and a recv failing look identical. */
    int last_err;
    char last_op; /* 's' sendto, 'p' poll, 'r' recvfrom, 0 if none failed */
    /* A card is two thousand datagrams and the IOS send path has room for a
     * few hundred; past that it simply refuses. Pausing every `pace_every`
     * sends for `pace_us` keeps the transfer inside what the console can
     * actually put on the wire, which costs a second and saves five rounds
     * of retransmission. 0 disables it. */
    int pace_every;
    int pace_us;
    int sent_since_pause;
} wii_socket;

/* `host` was not a dotted quad. Distinct from the negative libogc/IOS codes
 * these functions otherwise pass through, which are all small negatives. */
#define WII_NET_EBADADDR (-1000)

/* Bring up the network. Returns 0 on success and writes the console's own IP
 * into `ip` for display. On failure returns the negative libogc error code
 * rather than a flat -1, because the two ways this fails need different
 * fixes and the screen is the only place to say which happened. */
int wii_net_init(char *ip, int ip_cap);

/* 0 on success, WII_NET_EBADADDR for a non-dotted-quad `host`, otherwise the
 * negative code from net_socket. -6 (ENXIO) there means the IOS socket driver
 * is not open, which is not the same as the server being unreachable. */
int wii_net_open(wii_socket *s, const char *host, uint16_t port);
void wii_net_close(wii_socket *s);

/* ss_transport signatures. */
int wii_net_send(void *ctx, const uint8_t *data, size_t len);
int wii_net_recv(void *ctx, uint8_t *buf, size_t cap, int timeout_ms);

#endif /* SLOTSYNC_WII_NET_H */
