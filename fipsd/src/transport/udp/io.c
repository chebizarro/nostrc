#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#define __APPLE_USE_RFC_3542 1
#include "transport/udp/io.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <net/if_dl.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "transport/sockos.h"
#include "log.h"
#if defined(_WIN32)
/* Windows SDK ws2ipdef.h values; mingw-w64 defines IP_ECN/IPV6_ECN only. */
#ifndef IP_RECVECN
#define IP_RECVECN 50
#endif
#ifndef IPV6_RECVECN
#define IPV6_RECVECN 50
#endif
#endif

bool fipsd_udp_decode_control(const struct msghdr *m, fipsd_udp_packet_info *out)
{
    fipsd_udp_packet_info p = {0};
    if (!m || !out || (m->msg_flags & MSG_CTRUNC) || (m->msg_controllen && !m->msg_control)) return false;
    const uint8_t *base = m->msg_control;
    size_t at = 0, total = m->msg_controllen;
    while (at < total) {
        struct cmsghdr h;
        if (total - at < sizeof h) return false;
        memcpy(&h, base + at, sizeof h);
        if (h.cmsg_len < CMSG_LEN(0) || h.cmsg_len > total - at) return false;
        const uint8_t *data = base + at + CMSG_LEN(0);
        size_t len = h.cmsg_len - CMSG_LEN(0);
#if defined(__linux__) || defined(_WIN32)
        /* Windows IN_PKTINFO has the same ipi_addr/ipi_ifindex fields. */
        if (h.cmsg_level == IPPROTO_IP && h.cmsg_type == IP_PKTINFO) {
            struct in_pktinfo info;
            if (len < sizeof info) return false;
            memcpy(&info, data, sizeof info);
            p.family = AF_INET; p.destination.v4 = info.ipi_addr;
            p.interface_index = (unsigned)info.ipi_ifindex; p.has_destination = true;
        }
#else
        if (h.cmsg_level == IPPROTO_IP && h.cmsg_type == IP_RECVDSTADDR) {
            if (len < sizeof(struct in_addr)) return false;
            memcpy(&p.destination.v4, data, sizeof(struct in_addr)); p.family = AF_INET; p.has_destination = true;
        }
        if (h.cmsg_level == IPPROTO_IP && h.cmsg_type == IP_RECVIF) {
            struct sockaddr_dl dl = {0};
            if (len < offsetof(struct sockaddr_dl, sdl_type)) return false;
            memcpy(&dl, data, len < sizeof dl ? len : sizeof dl); p.interface_index = dl.sdl_index;
        }
#endif
        if (h.cmsg_level == IPPROTO_IPV6 && h.cmsg_type == IPV6_PKTINFO) {
            struct in6_pktinfo info;
            if (len < sizeof info) return false;
            memcpy(&info, data, sizeof info); p.family = AF_INET6;
            p.destination.v6 = info.ipi6_addr; p.interface_index = info.ipi6_ifindex; p.has_destination = true;
        }
        if (h.cmsg_level == IPPROTO_IP && (h.cmsg_type == IP_TOS || h.cmsg_type == IP_RECVTOS)) {
            if (!len) return false;
            /* Linux supplies a byte; BSD may supply an int. */
            int tos = 0;
            if (len >= sizeof tos) memcpy(&tos, data, sizeof tos); else tos = *data;
            p.ecn = (uint8_t)(tos & 3); p.has_ecn = true;
        }
        if (h.cmsg_level == IPPROTO_IPV6 && h.cmsg_type == IPV6_TCLASS) {
            int tc;
            if (len < sizeof tc) return false;
            memcpy(&tc, data, sizeof tc); p.ecn = (uint8_t)(tc & 3); p.has_ecn = true;
        }
#if defined(_WIN32)
        /* IP_RECVECN/IPV6_RECVECN deliver the ECN codepoint as an INT. */
        if ((h.cmsg_level == IPPROTO_IP && h.cmsg_type == IP_ECN) ||
            (h.cmsg_level == IPPROTO_IPV6 && h.cmsg_type == IPV6_ECN)) {
            int ecn;
            if (len < sizeof ecn) return false;
            memcpy(&ecn, data, sizeof ecn); p.ecn = (uint8_t)(ecn & 3); p.has_ecn = true;
        }
#endif
#ifdef SO_RXQ_OVFL
        if (h.cmsg_level == SOL_SOCKET && h.cmsg_type == SO_RXQ_OVFL) {
            if (len < sizeof p.kernel_drops) return false;
            memcpy(&p.kernel_drops, data, sizeof p.kernel_drops); p.has_drops = true;
        }
#endif
        size_t step = CMSG_SPACE(len);
        if (step < h.cmsg_len) return false;
        if (step >= total - at) break; /* final cmsg need not include padding */
        at += step;
    }
    *out = p; return true;
}
size_t fipsd_udp_source_control(void *buf, size_t cap, const fipsd_udp_packet_info *p)
{
    if (!p || !p->has_destination) return 0;
    struct cmsghdr *h = buf;
    size_t size;
    if (p->family == AF_INET6) size = sizeof(struct in6_pktinfo);
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32)
    else if (p->family == AF_INET) size = sizeof(struct in_pktinfo);
#else
    else if (p->family == AF_INET) size = sizeof(struct in_addr);
#endif
    else return 0;
    if (cap < CMSG_SPACE(size)) return 0;
    memset(buf, 0, CMSG_SPACE(size)); h->cmsg_len = CMSG_LEN(size);
    if (p->family == AF_INET6) {
        struct in6_pktinfo info = {.ipi6_addr = p->destination.v6, .ipi6_ifindex = p->interface_index};
        h->cmsg_level = IPPROTO_IPV6; h->cmsg_type = IPV6_PKTINFO; memcpy(CMSG_DATA(h), &info, sizeof info);
    } else {
        h->cmsg_level = IPPROTO_IP;
#if defined(__linux__) || defined(__APPLE__)
        struct in_pktinfo info = {.ipi_spec_dst = p->destination.v4, .ipi_ifindex = (int)p->interface_index};
        h->cmsg_type = IP_PKTINFO; memcpy(CMSG_DATA(h), &info, sizeof info);
#elif defined(_WIN32)
        /* WSASendMsg takes the source address in ipi_addr. */
        struct in_pktinfo info = {.ipi_addr = p->destination.v4, .ipi_ifindex = p->interface_index};
        h->cmsg_type = IP_PKTINFO; memcpy(CMSG_DATA(h), &info, sizeof info);
#else
        h->cmsg_type = IP_SENDSRCADDR; memcpy(CMSG_DATA(h), &p->destination.v4, sizeof p->destination.v4);
#endif
    }
    return CMSG_SPACE(size);
}
static bool decimal(const char *text, uint32_t limit, uint32_t *out)
{
    if (!*text) return false;
    uint32_t n = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p < '0' || *p > '9' || n > (limit - (*p - '0')) / 10) return false;
        n = n * 10 + (*p - '0');
    }
    *out = n; return true;
}
bool fipsd_udp_endpoint_numeric(const char *text, fipsd_udp_endpoint *out)
{
    if (!text || strlen(text) >= 256) return false;
    char host[256]; strcpy(host, text);
    char *port, *start = host;
    int family = AF_INET;
    if (*host == '[') {
        char *end = strchr(host, ']');
        if (!end || end[1] != ':') return false;
        *end = 0; start++; port = end + 2; family = AF_INET6;
    } else {
        port = strrchr(host, ':'); if (!port) return false; *port++ = 0;
    }
    uint32_t n;
    if (!decimal(port, 65535, &n)) return false;
    fipsd_udp_endpoint e = {0};
    if (family == AF_INET) {
        struct sockaddr_in *a = (void *)&e.address;
        a->sin_family = AF_INET; a->sin_port = htons((uint16_t)n); e.len = sizeof *a;
        if (inet_pton(AF_INET, start, &a->sin_addr) != 1) return false;
    } else {
        struct sockaddr_in6 *a = (void *)&e.address;
        a->sin6_family = AF_INET6; a->sin6_port = htons((uint16_t)n); e.len = sizeof *a;
        char *scope = strchr(start, '%');
        if (scope) {
            *scope++ = 0;
            uint32_t scope_id; /* u_long on Windows */
            if (!decimal(scope, UINT32_MAX, &scope_id)) return false;
            a->sin6_scope_id = scope_id;
        }
        if (inet_pton(AF_INET6, start, &a->sin6_addr) != 1) return false;
    }
    *out = e; return true;
}
bool fipsd_udp_endpoint_addr(const fipsd_udp_endpoint *e, fips_transport_addr *out)
{
    /* socklen_t is a signed int on Windows; e->len is never negative. */
    if (e->address.ss_family == AF_INET && (size_t)e->len >= sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *a = (const void *)&e->address;
        return fips_transport_addr_from_socket_addr((const uint8_t *)&a->sin_addr, 4, ntohs(a->sin_port), 0, out) == FIPS_OK;
    }
    if (e->address.ss_family == AF_INET6 && (size_t)e->len >= sizeof(struct sockaddr_in6)) {
        const struct sockaddr_in6 *a = (const void *)&e->address;
        return fips_transport_addr_from_socket_addr((const uint8_t *)&a->sin6_addr, 16, ntohs(a->sin6_port), a->sin6_scope_id, out) == FIPS_OK;
    }
    return false;
}
/* (const void *): Winsock takes const char *. */
static int option(gof_socket_t fd, int level, int name, int value)
{ return setsockopt(fd, level, name, (const void *)&value, sizeof value); }
#if defined(_WIN32)
/* An ICMP port/net unreachable answer would otherwise fail the next receive
 * on this socket, which every peer shares (upstream ignore_port_unreachable). */
static void ignore_unreachable(gof_socket_t fd)
{
    BOOL off = FALSE; DWORD returned = 0;
    if (WSAIoctl(fd, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &returned, NULL, NULL))
        FIPSD_WARN("udp", "SIO_UDP_CONNRESET refused (%d); port-unreachable reports reach the receiver",
            WSAGetLastError());
#ifdef SIO_UDP_NETRESET
    (void)WSAIoctl(fd, SIO_UDP_NETRESET, &off, sizeof off, NULL, 0, &returned, NULL, NULL);
#endif
}
#endif
#if defined(__linux__)
#ifndef SOL_UDP
#define SOL_UDP 17
#endif
#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif
bool fipsd_udp_gso_supported(gof_socket_t fd)
{
    int size = 0; socklen_t len = sizeof size;
    return getsockopt(fd, SOL_UDP, UDP_SEGMENT, &size, &len) == 0;
}
#endif
gof_socket_t fipsd_udp_open(const fipsd_udp_endpoint *bind_to, const char *interface_name, bool join,
    size_t recv_size, size_t send_size, fipsd_buffer_granted *recv_grant,
    fipsd_buffer_granted *send_grant, fipsd_udp_endpoint *local)
{
    int family = bind_to->address.ss_family;
    gof_socket_t fd = socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == GOF_INVALID_SOCKET) { errno = gof_socket_errno(); return GOF_INVALID_SOCKET; }
    if (fipsd_socket_prepare(fd)) goto fail_errno;
    if (family == AF_INET6 && option(fd, IPPROTO_IPV6, IPV6_V6ONLY, 1)) goto fail;
    if (interface_name) {
#if defined(__linux__)
        if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, interface_name, (socklen_t)strlen(interface_name) + 1)) goto fail;
#elif defined(__APPLE__)
        unsigned index = if_nametoindex(interface_name);
        if (!index || option(fd, family == AF_INET ? IPPROTO_IP : IPPROTO_IPV6,
                family == AF_INET ? IP_BOUND_IF : IPV6_BOUND_IF, (int)index)) goto fail;
#else
        errno = ENOTSUP; goto fail_errno;
#endif
    }
#if defined(_WIN32)
    if (join) { errno = ENOTSUP; goto fail_errno; } /* no connected peer sockets */
    if (option(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1)) goto fail;
    if (bind(fd, (const struct sockaddr *)&bind_to->address, bind_to->len)) goto fail;
    ignore_unreachable(fd);
    if (family == AF_INET) {
        if (option(fd, IPPROTO_IP, IP_PKTINFO, 1)) goto fail;
        (void)option(fd, IPPROTO_IP, IP_RECVECN, 1); /* ECN where the stack offers it */
    } else {
        if (option(fd, IPPROTO_IPV6, IPV6_PKTINFO, 1)) goto fail;
        (void)option(fd, IPPROTO_IPV6, IPV6_RECVECN, 1);
    }
#else
    if (join && (option(fd, SOL_SOCKET, SO_REUSEADDR, 1) || option(fd, SOL_SOCKET, SO_REUSEPORT, 1))) goto fail;
    if (bind(fd, (const struct sockaddr *)&bind_to->address, bind_to->len)) goto fail;
    if (!join && (option(fd, SOL_SOCKET, SO_REUSEADDR, 1) || option(fd, SOL_SOCKET, SO_REUSEPORT, 1))) goto fail;
    if (family == AF_INET) {
#if defined(__linux__)
        if (option(fd, IPPROTO_IP, IP_PKTINFO, 1)) goto fail;
#else
        if (option(fd, IPPROTO_IP, IP_RECVDSTADDR, 1) || option(fd, IPPROTO_IP, IP_RECVIF, 1)) goto fail;
#endif
        if (option(fd, IPPROTO_IP, IP_RECVTOS, 1)) goto fail;
    } else if (option(fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, 1) || option(fd, IPPROTO_IPV6, IPV6_RECVTCLASS, 1)) goto fail;
#endif
#ifdef SO_RXQ_OVFL
    if (option(fd, SOL_SOCKET, SO_RXQ_OVFL, 1)) goto fail;
#endif
    if (fipsd_sockbuf_set(fd, FIPSD_SOCKET_RECV, recv_size, recv_grant) ||
        fipsd_sockbuf_set(fd, FIPSD_SOCKET_SEND, send_size, send_grant)) goto fail;
    local->len = sizeof local->address;
    if (getsockname(fd, (struct sockaddr *)&local->address, &local->len)) goto fail;
    return fd;
fail:
    errno = gof_socket_errno(); /* errno on POSIX; the Winsock error on Windows */
fail_errno:
    fipsd_socket_close(fd);
    return GOF_INVALID_SOCKET;
}
