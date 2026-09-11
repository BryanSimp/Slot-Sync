/* UDP transport for the host tests. See host_socket.c. */

#ifndef SLOTSYNC_HOST_SOCKET_H
#define SLOTSYNC_HOST_SOCKET_H

#include <stddef.h>

typedef struct {
    int fd;
    /* Opaque storage for a struct sockaddr_in, so callers need no socket
     * headers. 32 bytes is comfortably enough on every platform here. */
    unsigned char addr[32];

    /* Send pacing, matching what the console's transport does: pause for
     * `pace_us` after every `pace_every` datagrams. 0 in either field sends as
     * fast as the host will go, which is the default and what the loss tests
     * want. A live test that wants to offer the same load a console offers
     * sets these. */
    unsigned pace_every;
    unsigned pace_us;
    unsigned sent_since_pause;

    /* Every datagram the client handed us, dropped ones included. A delta
     * push is only working if this stays far below the card's chunk count. */
    unsigned sent;

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
