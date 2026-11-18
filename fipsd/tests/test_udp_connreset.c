/* Windows UDP (nostrc-iq1n9.6.2, upstream ignore_port_unreachable): one peer
 * whose port is closed must not fail the transport's socket, which every peer
 * shares. A plain socket shows the failure first: after a datagram to a closed
 * loopback port, its next receive is WSAECONNRESET instead of the datagram
 * another peer sent. The socket fipsd_udp_open makes (SIO_UDP_CONNRESET and
 * SIO_UDP_NETRESET off) receives that datagram, both with a plain receive and
 * through libgo's completion port as the transport's receive fiber does, and
 * reports no reset afterwards. Skips (77) when the host reports no reset on the
 * plain socket either, since then nothing would be proven. */
#include "transport/udp/io.h"
#include <libgo/fiber.h>
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>

static struct sockaddr_in loopback(uint16_t port)
{
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(port);
    return a;
}
static SOCKET bound(uint16_t *port)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); assert(s != INVALID_SOCKET);
    struct sockaddr_in a = loopback(0); int len = (int)sizeof a;
    assert(!bind(s, (struct sockaddr *)&a, sizeof a) && !getsockname(s, (struct sockaddr *)&a, &len));
    if (port) *port = ntohs(a.sin_port);
    return s;
}
static uint16_t closed_port(void) { uint16_t port; closesocket(bound(&port)); return port; }
static void send_to(SOCKET s, uint16_t port, const char *text)
{
    struct sockaddr_in a = loopback(port);
    assert(sendto(s, text, (int)strlen(text), 0, (struct sockaddr *)&a, sizeof a) == (int)strlen(text));
}
/* One receive with a bound: the byte count, or -1 and the Winsock error. */
static int receive(SOCKET s, char *buf, int cap, DWORD ms, int *wsa)
{
    assert(!setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof ms));
    int n = recv(s, buf, cap, 0);
    *wsa = n < 0 ? WSAGetLastError() : 0;
    return n;
}
typedef struct fiber_rx { gof_socket_t fd; ssize_t n[2]; int error[2]; char buf[64]; } fiber_rx;
static void fiber_receive(void *arg)
{
    fiber_rx *r = arg;
    char spare[64];
    for (unsigned i = 0; i < 2; ++i) {
        struct sockaddr_storage from; socklen_t len = sizeof from;
        r->n[i] = gof_recvfrom(r->fd, i ? spare : r->buf, sizeof r->buf, 0, (struct sockaddr *)&from, &len, i ? 500 : 5000);
        r->error[i] = r->n[i] < 0 ? errno : 0;
    }
}
int main(void)
{
    WSADATA wsa_data; assert(!WSAStartup(MAKEWORD(2, 2), &wsa_data));
    uint16_t dead = closed_port();
    char buf[64]; int wsa = 0;
    /* The failure the ioctls prevent, on a socket without them. */
    uint16_t plain_port; SOCKET plain = bound(&plain_port), peer = bound(NULL);
    send_to(plain, dead, "probe");
    send_to(peer, plain_port, "hello");
    bool reset = false;
    for (unsigned i = 0; i < 2; ++i) {
        int n = receive(plain, buf, sizeof buf, 1000, &wsa);
        if (n < 0 && wsa == WSAECONNRESET) reset = true;
    }
    closesocket(plain);
    if (!reset) {
        puts("SKIP: this host reported no WSAECONNRESET for a closed loopback port on a plain socket");
        return 77;
    }
    puts("plain socket: the closed port's ICMP unreachable failed a receive (WSAECONNRESET)");
    /* The transport's socket. */
    fipsd_udp_endpoint at, local; fipsd_buffer_granted rg = {0}, sg = {0};
    assert(fipsd_udp_endpoint_numeric("127.0.0.1:0", &at));
    gof_socket_t fd = fipsd_udp_open(&at, NULL, false, 1u << 20, 1u << 20, &rg, &sg, &local);
    assert(fd != GOF_INVALID_SOCKET);
    uint16_t port = ntohs(((const struct sockaddr_in *)&local.address)->sin_port);
    send_to(fd, dead, "probe");
    send_to(peer, port, "hello");
    int n = receive(fd, buf, sizeof buf, 5000, &wsa);
    assert(n == 5 && !memcmp(buf, "hello", 5));
    n = receive(fd, buf, sizeof buf, 500, &wsa);
    assert(n < 0 && wsa == WSAETIMEDOUT); /* and no reset queued behind it */
    puts("fipsd_udp_open socket, plain receive: the other peer's datagram, no reset");
    /* Through the completion port, as the transport's receive fiber reads. */
    send_to(fd, dead, "probe");
    send_to(peer, port, "again");
    fiber_rx r = {.fd = fd};
    gof_init(0);
    assert(gof_spawn(fiber_receive, &r, 0));
    gof_run();
    assert(r.n[0] == 5 && !memcmp(r.buf, "again", 5));
    assert(r.n[1] == -1 && r.error[1] == ETIMEDOUT);
    puts("fipsd_udp_open socket, gof_recvfrom: the other peer's datagram, then ETIMEDOUT (no ECONNRESET): ok");
    closesocket(fd); closesocket(peer);
    WSACleanup();
    return 0;
}
