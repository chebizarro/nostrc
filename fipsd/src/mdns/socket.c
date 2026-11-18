/* mDNS multicast sockets on libgo fiber I/O. */
#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#define __APPLE_USE_RFC_3542 1
#include "fiber_errno.h"
#include "mdns/socket.h"
#include "log.h"
#include "transport/udp/io.h"
#include <libgo/fiber.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdalign.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MODULE "mdns"
static const uint8_t group4[4] = {224, 0, 0, 251};
static const uint8_t group6[16] = {0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xfb};

static int option(int fd, int level, int name, int value) { return setsockopt(fd, level, name, &value, sizeof value); }
static int open_family(int family)
{
    int fd = socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return -1;
    struct sockaddr_storage ss; memset(&ss, 0, sizeof ss);
    socklen_t len;
    if (fcntl(fd, F_SETFL, O_NONBLOCK) || fcntl(fd, F_SETFD, FD_CLOEXEC)) goto fail;
    if (option(fd, SOL_SOCKET, SO_REUSEADDR, 1) || option(fd, SOL_SOCKET, SO_REUSEPORT, 1)) goto fail;
    if (family == AF_INET) {
        struct sockaddr_in *a = (struct sockaddr_in *)&ss;
        a->sin_family = AF_INET; a->sin_port = htons(FIPSD_MDNS_PORT); len = sizeof *a;
        unsigned char ttl = 255, loop = 1;
        if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl) ||
            setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop) || option(fd, IPPROTO_IP, IP_TTL, 255)) goto fail;
#if defined(__linux__)
        if (option(fd, IPPROTO_IP, IP_PKTINFO, 1)) goto fail;
#ifdef IP_MULTICAST_ALL
        (void)option(fd, IPPROTO_IP, IP_MULTICAST_ALL, 0);
#endif
#else
        if (option(fd, IPPROTO_IP, IP_RECVDSTADDR, 1) || option(fd, IPPROTO_IP, IP_RECVIF, 1)) goto fail;
#endif
    } else {
        struct sockaddr_in6 *a = (struct sockaddr_in6 *)&ss;
        a->sin6_family = AF_INET6; a->sin6_port = htons(FIPSD_MDNS_PORT); len = sizeof *a;
        if (option(fd, IPPROTO_IPV6, IPV6_V6ONLY, 1) || option(fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, 255) ||
            option(fd, IPPROTO_IPV6, IPV6_UNICAST_HOPS, 255) || option(fd, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, 1) ||
            option(fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, 1)) goto fail;
    }
    if (bind(fd, (struct sockaddr *)&ss, len)) goto fail;
    return fd;
fail:;
    int e = errno; close(fd); errno = e;
    return -1;
}
static void receiver(void *arg)
{
    const fipsd_mdns_receiver *r = arg;
    fipsd_mdns_sockets *s = r->owner;
    int index = r->index;
    int fd = s->fd[index];
    gof_set_name(index ? "mdns-rx6" : "mdns-rx4");
    uint8_t *buf = malloc(FIPSD_MDNS_MAX_DATAGRAM);
    while (buf && !atomic_load(&s->stopping)) {
        struct sockaddr_storage src; memset(&src, 0, sizeof src);
        struct iovec iov = {.iov_base = buf, .iov_len = FIPSD_MDNS_MAX_DATAGRAM};
        alignas(struct cmsghdr) uint8_t control[256];
        struct msghdr msg = {.msg_name = &src, .msg_namelen = sizeof src, .msg_iov = &iov, .msg_iovlen = 1,
                             .msg_control = control, .msg_controllen = sizeof control};
        ssize_t n = gof_recvmsg(fd, &msg, 0, -1);
        if (n < 0) {
            if (fipsd_io_errno() == ECANCELED || atomic_load(&s->stopping)) break;
            continue;
        }
        if (atomic_load(&s->stopping)) break;
        fipsd_udp_packet_info info;
        uint32_t ifindex = fipsd_udp_decode_control(&msg, &info) ? info.interface_index : 0;
        fipsd_mdns_datagram *d = malloc(sizeof *d + (size_t)n);
        if (!d) continue;
        memset(d, 0, sizeof *d);
        d->ifindex = ifindex; d->len = (size_t)n; memcpy(d->bytes, buf, (size_t)n);
        if (src.ss_family == AF_INET) {
            const struct sockaddr_in *a = (const struct sockaddr_in *)&src;
            d->src.ip.width = 4; memcpy(d->src.ip.bytes, &a->sin_addr, 4); d->src.port = ntohs(a->sin_port);
        } else if (src.ss_family == AF_INET6) {
            const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)&src;
            d->src.ip.width = 16; memcpy(d->src.ip.bytes, &a->sin6_addr, 16); d->src.port = ntohs(a->sin6_port);
            d->src.scope_id = a->sin6_scope_id ? a->sin6_scope_id : ifindex;
        }
        if (go_channel_try_send(s->rx, d) != 0) free(d);
    }
    free(buf);
    (void)go_channel_try_send(s->done, s);
}
static int sockets_open(void *user, fipsd_runtime *rt, GoChannel *rx)
{
    fipsd_mdns_sockets *s = user;
    s->rx = rx;
    s->done = go_channel_create(2);
    if (!s->done) return -1;
    for (int i = 0; i < 2; ++i) {
        s->fd[i] = open_family(i ? AF_INET6 : AF_INET);
        if (s->fd[i] < 0) FIPSD_WARN(MODULE, "mDNS %s socket unavailable: %s", i ? "IPv6" : "IPv4", strerror(errno));
        else if (gof_io_reset_fd(s->fd[i]) != 0) { close(s->fd[i]); s->fd[i] = -1; }
    }
    if (s->fd[0] < 0 && s->fd[1] < 0) return -1;
    for (int i = 0; i < 2; ++i) {
        if (s->fd[i] < 0) continue;
        s->arg[i].owner = s; s->arg[i].index = i;
        if (fipsd_runtime_spawn(rt, receiver, &s->arg[i]) == 0) ++s->receivers;
        else { close(s->fd[i]); s->fd[i] = -1; }
    }
    return s->receivers ? 0 : -1;
}
static bool sockets_join(void *user, uint32_t ifindex, int family, const uint8_t v4[4])
{
    fipsd_mdns_sockets *s = user;
    int fd = s->fd[family == AF_INET6];
    if (fd < 0) return false;
    int rc;
    if (family == AF_INET) {
#if defined(__linux__)
        struct ip_mreqn m; memset(&m, 0, sizeof m);
        memcpy(&m.imr_multiaddr, group4, 4); memcpy(&m.imr_address, v4, 4); m.imr_ifindex = (int)ifindex;
#else
        struct ip_mreq m; memset(&m, 0, sizeof m);
        memcpy(&m.imr_multiaddr, group4, 4); memcpy(&m.imr_interface, v4, 4);
#endif
        rc = setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof m);
    } else {
        struct ipv6_mreq m; memset(&m, 0, sizeof m);
        memcpy(&m.ipv6mr_multiaddr, group6, 16); m.ipv6mr_interface = ifindex;
        rc = setsockopt(fd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &m, sizeof m);
    }
    return rc == 0 || errno == EADDRINUSE;
}
static void sockets_leave(void *user, uint32_t ifindex, int family, const uint8_t v4[4])
{
    fipsd_mdns_sockets *s = user;
    int fd = s->fd[family == AF_INET6];
    if (fd < 0) return;
    if (family == AF_INET) {
#if defined(__linux__)
        struct ip_mreqn m; memset(&m, 0, sizeof m);
        memcpy(&m.imr_multiaddr, group4, 4); memcpy(&m.imr_address, v4, 4); m.imr_ifindex = (int)ifindex;
#else
        struct ip_mreq m; memset(&m, 0, sizeof m);
        memcpy(&m.imr_multiaddr, group4, 4); memcpy(&m.imr_interface, v4, 4);
#endif
        (void)setsockopt(fd, IPPROTO_IP, IP_DROP_MEMBERSHIP, &m, sizeof m);
    } else {
        struct ipv6_mreq m; memset(&m, 0, sizeof m);
        memcpy(&m.ipv6mr_multiaddr, group6, 16); m.ipv6mr_interface = ifindex;
        (void)setsockopt(fd, IPPROTO_IPV6, IPV6_LEAVE_GROUP, &m, sizeof m);
    }
}
static bool sockets_send(void *user, uint32_t ifindex, int family, const uint8_t v4[4], const fipsd_mdns_sockaddr *dst,
                         const uint8_t *buf, size_t len)
{
    fipsd_mdns_sockets *s = user;
    int fd = s->fd[family == AF_INET6];
    if (fd < 0) return false;
    struct sockaddr_storage ss; memset(&ss, 0, sizeof ss);
    socklen_t sl;
    if (family == AF_INET) {
#if defined(__linux__)
        struct ip_mreqn m; memset(&m, 0, sizeof m);
        memcpy(&m.imr_address, v4, 4); m.imr_ifindex = (int)ifindex;
        if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &m, sizeof m)) return false;
#else
        struct in_addr a; memcpy(&a, v4, 4);
        if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &a, sizeof a)) return false;
#endif
        struct sockaddr_in *to = (struct sockaddr_in *)&ss;
        to->sin_family = AF_INET; sl = sizeof *to;
        if (dst) { memcpy(&to->sin_addr, dst->ip.bytes, 4); to->sin_port = htons(dst->port); }
        else { memcpy(&to->sin_addr, group4, 4); to->sin_port = htons(FIPSD_MDNS_PORT); }
    } else {
        unsigned index = ifindex;
        if (setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_IF, &index, sizeof index)) return false;
        struct sockaddr_in6 *to = (struct sockaddr_in6 *)&ss;
        to->sin6_family = AF_INET6; sl = sizeof *to;
        if (dst) {
            memcpy(&to->sin6_addr, dst->ip.bytes, 16); to->sin6_port = htons(dst->port);
            to->sin6_scope_id = fipsd_mdns_ipv6_is_unicast_link_local(dst->ip.bytes) ? (dst->scope_id ? dst->scope_id : ifindex) : 0;
        } else { memcpy(&to->sin6_addr, group6, 16); to->sin6_port = htons(FIPSD_MDNS_PORT); to->sin6_scope_id = ifindex; }
    }
    struct iovec iov = {.iov_base = (void *)buf, .iov_len = len};
    struct msghdr msg = {.msg_name = &ss, .msg_namelen = sl, .msg_iov = &iov, .msg_iovlen = 1};
    ssize_t n;
    do n = gof_in_fiber() ? gof_sendmsg(fd, &msg, 0, 1000) : sendmsg(fd, &msg, 0); while (n < 0 && fipsd_io_errno() == EINTR);
    return n == (ssize_t)len;
}
static void sockets_close(void *user)
{
    fipsd_mdns_sockets *s = user;
    atomic_store(&s->stopping, true);
    for (int i = 0; i < 2; ++i) if (s->fd[i] >= 0) (void)gof_io_cancel_fd(s->fd[i]);
    for (; s->receivers > 0; --s->receivers) { void *v; (void)go_channel_receive(s->done, &v); }
    for (int i = 0; i < 2; ++i) if (s->fd[i] >= 0) { (void)gof_io_reset_fd(s->fd[i]); close(s->fd[i]); s->fd[i] = -1; }
}
void fipsd_mdns_sockets_init(fipsd_mdns_sockets *s)
{
    memset(s, 0, sizeof *s);
    s->fd[0] = s->fd[1] = -1;
    atomic_init(&s->stopping, false);
}
fipsd_mdns_io fipsd_mdns_sockets_io(fipsd_mdns_sockets *s)
{
    return (fipsd_mdns_io){.user = s, .open = sockets_open, .join = sockets_join, .leave = sockets_leave,
                           .send = sockets_send, .close = sockets_close};
}
void fipsd_mdns_sockets_release(fipsd_mdns_sockets *s)
{
    for (int i = 0; i < 2; ++i) if (s->fd[i] >= 0) { close(s->fd[i]); s->fd[i] = -1; }
    if (s->done) { go_channel_unref(s->done); s->done = NULL; }
}
