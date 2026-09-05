/* UDP transport for the host tests. See host_socket.c. */

#ifndef SLOTSYNC_HOST_SOCKET_H
#define SLOTSYNC_HOST_SOCKET_H

#include <stddef.h>

typedef struct {
    int fd;
    /* Opaque storage for a struct sockaddr_in, so callers need no socket
     * headers. 32 bytes is comfortably enough on every platform here. */
    unsigned char addr[32];

    /* Injected link damage, for the loss/reordering tests. */
    int loss_percent;
    unsigned int rng;
    int dropped_out;
    int dropped_in;
} host_socket;

int host_socket_startup(void);
void host_socket_cleanup(void);

int host_socket_open(host_socket *s, const char *host, unsigned short port);
void host_socket_close(host_socket *s);

/* Match the ss_transport signatures. */
int host_socket_send(void *ctx, const unsigned char *data, size_t len);
int host_socket_recv(void *ctx, unsigned char *buf, size_t cap, int timeout_ms);

#endif /* SLOTSYNC_HOST_SOCKET_H */
