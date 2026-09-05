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
} wii_socket;

/* Bring up the network. Returns 0 on success and writes the console's own IP
 * into `ip` for display. libogc's if_config both configures and reports. */
int wii_net_init(char *ip, int ip_cap);

int wii_net_open(wii_socket *s, const char *host, uint16_t port);
void wii_net_close(wii_socket *s);

/* ss_transport signatures. */
int wii_net_send(void *ctx, const uint8_t *data, size_t len);
int wii_net_recv(void *ctx, uint8_t *buf, size_t cap, int timeout_ms);

#endif /* SLOTSYNC_WII_NET_H */
