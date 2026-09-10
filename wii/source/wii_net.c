/* See wii_net.h. */

#include "wii_net.h"

#include <network.h>
#include <ogcsys.h>
#include <string.h>
#include <unistd.h>

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
        return (int)result;
    }

    /* if_config leaves the interface up and DHCP has handed back an address by
     * here -- but that is not the same as the socket driver being open. It
     * drives the configuration ioctls itself and only starts the socket layer
     * asynchronously, so /dev/net/ip/top can still be closed when it returns.
     *
     * net_socket() reads that descriptor before anything else and answers -6
     * (ENXIO) if it is closed. On screen that is indistinguishable from the
     * server being unreachable, even though the console has a working address
     * and the server is fine.
     *
     * net_init() is the call that opens it, and it returns 0 immediately when
     * the descriptor is already valid -- so this costs nothing when if_config
     * did leave it up, and fixes the case where it did not. */
    result = net_init();
    if (result < 0) {
        return (int)result;
    }

    if (ip != NULL && ip_cap > 0) {
        strncpy(ip, local, (size_t)ip_cap - 1);
        ip[ip_cap - 1] = '\0';
    }
    return 0;
}

/* IOS's sockaddr is 8 bytes -- {u8 len; u8 family; u16 port; u32 addr} -- and
 * libogc's struct sockaddr_in opens with exactly those four fields, so its
 * first 8 bytes already are the struct IOS wants.
 *
 * This has to be passed as the address length rather than sizeof(sockaddr_in).
 * net_sendto forces to->sin_len = tolen and then memcpy's sin_len bytes
 * straight into the ioctl, so passing 16 hands IOS a sockaddr claiming to be
 * 16 bytes long and it rejects the send. Nintendont's own IOS-native path sets
 * this same byte to 8 (SlotSyncNet.c, sendParams.destaddr[0]). */
#define WII_IOS_SOCKADDR_LEN 8

int wii_net_open(wii_socket *s, const char *host, uint16_t port)
{
    struct sockaddr_in *addr = (struct sockaddr_in *)&s->addr;

    s->last_err = 0;
    s->last_op = 0;
    s->sent_since_pause = 0;

    /* IPPROTO_IP (0), not IPPROTO_UDP (17). libogc hands all three arguments
     * to IOS's SO_SOCKET unchanged, and IOS answers -68 for 17 -- which the
     * error table turns into -EPROTONOSUPPORT (-123). SOCK_DGRAM is what
     * selects UDP; the protocol field has to be 0. devkitPro's own Wii UDP
     * example and Nintendont's http.c both pass 0. */
    s->fd = net_socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s->fd < 0) {
        return s->fd; /* negative libogc code; -6 is ENXIO, driver not open */
    }

    /* libogc's sockaddr_in is the BSD-with-sin_len flavour, so the length byte
     * has to be filled in -- it is not the layout Linux or Windows use. */
    memset(addr, 0, sizeof(*addr));
    addr->sin_len = WII_IOS_SOCKADDR_LEN;
    addr->sin_family = AF_INET;
    addr->sin_port = htons(port);
    addr->sin_addr.s_addr = inet_addr(host);

    if (addr->sin_addr.s_addr == 0 || addr->sin_addr.s_addr == 0xFFFFFFFFu) {
        net_close(s->fd);
        s->fd = -1;
        return WII_NET_EBADADDR; /* no DNS on this path, by design */
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

    s32 sent;

    /* Pause before the send that would overrun, not after it. A refused send
     * still costs the caller a retransmission round, so the pause has to come
     * while there is still room. */
    if (s->pace_every > 0 && ++s->sent_since_pause >= s->pace_every) {
        s->sent_since_pause = 0;
        if (s->pace_us > 0) {
            usleep((useconds_t)s->pace_us);
        }
    }

    sent = net_sendto(s->fd, (void *)data, (s32)len, 0, (struct sockaddr *)addr,
                      WII_IOS_SOCKADDR_LEN);
    if (sent < 0) {
        s->last_err = (int)sent;
        s->last_op = 's';
    }
    return (int)sent;
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
        s->last_err = (int)ready;
        s->last_op = 'p';
        return -1;
    }
    if (ready == 0 || !(sd.revents & POLLIN)) {
        return 0; /* timeout, which is a normal outcome here */
    }

    got = net_recvfrom(s->fd, buf, (s32)cap, 0, NULL, NULL);
    if (got <= 0) {
        s->last_err = (int)got;
        s->last_op = 'r';
        return -1;
    }
    return (int)got;
}
