/* A UDP transport for the host tests.
 *
 * The Wii build uses libogc's net_* calls; this uses BSD sockets (Winsock on
 * Windows). Both plug into the same two function pointers in ss_transport,
 * which is the whole reason the core is written that way -- the protocol code
 * that ships to the console is the same code these tests exercise against a
 * real server.
 */

#include "host_socket.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#define CLOSE_SOCKET closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#define CLOSE_SOCKET close
#endif

/* socket() returns an unsigned SOCKET on Windows whose failure value casts to
 * -1 as an int, so one signed check covers both platforms. */
#define HOST_FD_INVALID (-1)

int host_socket_startup(void)
{
#ifdef _WIN32
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0 ? 0 : -1;
#else
    return 0;
#endif
}

void host_socket_cleanup(void)
{
#ifdef _WIN32
    WSACleanup();
#endif
}

int host_socket_open(host_socket *s, const char *host, unsigned short port)
{
    struct sockaddr_in *addr = (struct sockaddr_in *)&s->addr;

    /* Zero the whole struct rather than each field: callers declare one on the
     * stack, so anything left unset here is read as garbage. Set the knobs --
     * loss_percent, pace_every, pace_us -- after this returns. */
    memset(s, 0, sizeof(*s));

    s->fd = (int)socket(AF_INET, SOCK_DGRAM, 0);
    if (s->fd == HOST_FD_INVALID) {
        return -1;
    }

    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons(port);
    addr->sin_addr.s_addr = inet_addr(host);
    if (addr->sin_addr.s_addr == INADDR_NONE) {
        CLOSE_SOCKET(s->fd);
        return -1;
    }

    s->rng = 12345u;
    return 0;
}

void host_socket_close(host_socket *s)
{
    if (s->fd != HOST_FD_INVALID) {
        CLOSE_SOCKET(s->fd);
        s->fd = HOST_FD_INVALID;
    }
}

/* Deterministic, so an injected-loss failure reproduces. */
static int should_drop(host_socket *s)
{
    if (s->loss_percent <= 0) {
        return 0;
    }
    s->rng = s->rng * 1103515245u + 12345u;
    return (int)((s->rng >> 16) % 100u) < s->loss_percent;
}

/* Pause between bursts, so a live test can offer the same load the console's
 * paced transport offers rather than flooding loopback. */
static void host_socket_pace(host_socket *s)
{
    if (s->pace_every == 0 || s->pace_us == 0) {
        return;
    }
    if (++s->sent_since_pause < s->pace_every) {
        return;
    }
    s->sent_since_pause = 0;
#ifdef _WIN32
    Sleep((s->pace_us + 999) / 1000);
#else
    {
        /* select() with no descriptors is the sub-second sleep that is
         * already available here -- nanosleep would need a feature-test
         * macro that -std=c99 -Wpedantic has switched off. */
        struct timeval tv;
        tv.tv_sec = (long)(s->pace_us / 1000000u);
        tv.tv_usec = (long)(s->pace_us % 1000000u);
        select(0, NULL, NULL, NULL, &tv);
    }
#endif
}

int host_socket_send(void *ctx, const unsigned char *data, size_t len)
{
    host_socket *s = (host_socket *)ctx;
    struct sockaddr_in *addr = (struct sockaddr_in *)&s->addr;

    host_socket_pace(s);
    s->sent++;

    if (should_drop(s)) {
        s->dropped_out++;
        return (int)len; /* the caller must not be able to tell */
    }
    return (int)sendto(s->fd, (const char *)data, (int)len, 0, (struct sockaddr *)addr,
                       sizeof(*addr));
}

int host_socket_recv(void *ctx, unsigned char *buf, size_t cap, int timeout_ms)
{
    host_socket *s = (host_socket *)ctx;

    for (;;) {
        fd_set readable;
        struct timeval tv;
        int ready;
        int got;

        FD_ZERO(&readable);
        FD_SET((unsigned int)s->fd, &readable);
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        ready = select(s->fd + 1, &readable, NULL, NULL, &tv);
        if (ready < 0) {
            return -1;
        }
        if (ready == 0) {
            return 0; /* timeout, which is normal */
        }

        got = (int)recvfrom(s->fd, (char *)buf, (int)cap, 0, NULL, NULL);
        if (got <= 0) {
            return -1;
        }
        if (should_drop(s)) {
            s->dropped_in++;
            continue; /* pretend it never arrived */
        }
        return got;
    }
}
