/* See wii_net.h. */

#include "wii_net.h"

#include <network.h>
#include <ogcsys.h>
#include <string.h>

int wii_net_init(char *ip, int ip_cap)
{
    char local[16];
    s32 result;

    memset(local, 0, sizeof(local));

    /* if_config both brings the interface up and reports the address. The
     * final argument is a retry count; the Wii's WiFi can take several seconds
     * to associate and failing on the first attempt would be needlessly
     * fragile. */
    result = if_config(local, NULL, NULL, TRUE, 20);
    if (result < 0) {
        return -1;
    }

    if (ip != NULL && ip_cap > 0) {
        strncpy(ip, local, (size_t)ip_cap - 1);
        ip[ip_cap - 1] = '\0';
    }
    return 0;
}

int wii_net_open(wii_socket *s, const char *host, uint16_t port)
{
    struct sockaddr_in *addr = (struct sockaddr_in *)&s->addr;

    s->fd = net_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s->fd < 0) {
        return -1;
    }

    /* libogc's sockaddr_in is the BSD-with-sin_len flavour, so the length byte
     * has to be filled in -- it is not the layout Linux or Windows use. */
    memset(addr, 0, sizeof(*addr));
    addr->sin_len = sizeof(*addr);
    addr->sin_family = AF_INET;
    addr->sin_port = htons(port);
    addr->sin_addr.s_addr = inet_addr(host);

    if (addr->sin_addr.s_addr == 0 || addr->sin_addr.s_addr == 0xFFFFFFFFu) {
        net_close(s->fd);
        s->fd = -1;
        return -1; /* not a dotted quad; there is no DNS on this path */
    }
    return 0;
}

void wii_net_close(wii_socket *s)
{
    if (s->fd >= 0) {
        net_close(s->fd);
        s->fd = -1;
    }
}

int wii_net_send(void *ctx, const uint8_t *data, size_t len)
{
    wii_socket *s = (wii_socket *)ctx;
    struct sockaddr_in *addr = (struct sockaddr_in *)&s->addr;

    return (int)net_sendto(s->fd, (void *)data, (s32)len, 0, (struct sockaddr *)addr,
                           sizeof(*addr));
}

int wii_net_recv(void *ctx, uint8_t *buf, size_t cap, int timeout_ms)
{
    wii_socket *s = (wii_socket *)ctx;
    struct pollsd sd;
    s32 ready;
    s32 got;

    /* net_poll rather than a non-blocking socket and a sleep loop: it waits
     * with a real timeout, which is both more accurate and kinder to a console
     * that is also running a display. */
    sd.socket = s->fd;
    sd.events = POLLIN;
    sd.revents = 0;

    ready = net_poll(&sd, 1, timeout_ms);
    if (ready < 0) {
        return -1;
    }
    if (ready == 0 || !(sd.revents & POLLIN)) {
        return 0; /* timeout, which is a normal outcome here */
    }

    got = net_recvfrom(s->fd, buf, (s32)cap, 0, NULL, NULL);
    if (got <= 0) {
        return -1;
    }
    return (int)got;
}
