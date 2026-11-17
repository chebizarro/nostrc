#ifndef FIPSD_UDP_IO_H
#define FIPSD_UDP_IO_H
#ifdef __APPLE__
#ifndef __APPLE_USE_RFC_3542
#define __APPLE_USE_RFC_3542 1
#endif
#endif
#include "transport/sockbuf.h"
#include "libgo/fiber.h"
#include <fips/transport/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

typedef struct fipsd_udp_endpoint {
    struct sockaddr_storage address;
    socklen_t len;
} fipsd_udp_endpoint;
typedef struct fipsd_udp_packet_info {
    int family;
    union { struct in_addr v4; struct in6_addr v6; } destination;
    unsigned interface_index;
    uint8_t ecn;
    bool has_destination, has_ecn, has_drops;
    uint32_t kernel_drops;
} fipsd_udp_packet_info;
/* Pure, bounds-checked ancillary decoder; false on truncated/malformed control. */
bool fipsd_udp_decode_control(const struct msghdr *, fipsd_udp_packet_info *);
/* Caller supplies aligned storage, at least 128 bytes. */
size_t fipsd_udp_source_control(void *, size_t, const fipsd_udp_packet_info *);
bool fipsd_udp_endpoint_addr(const fipsd_udp_endpoint *, fips_transport_addr *);
bool fipsd_udp_endpoint_numeric(const char *, fipsd_udp_endpoint *);
/* Setup/teardown on blocking executor, not the scheduler. Listener reuse is
 * enabled AFTER bind, so a second daemon cannot steal the configured port.
 * Windows has neither SO_REUSEPORT nor non-stealing SO_REUSEADDR, so it binds
 * exclusively and opens no connected (join) sockets. GOF_INVALID_SOCKET on
 * failure, with errno. */
gof_socket_t fipsd_udp_open(const fipsd_udp_endpoint *, const char *interface_name,
    bool join, size_t recv_size, size_t send_size,
    fipsd_buffer_granted *, fipsd_buffer_granted *, fipsd_udp_endpoint *local);
#endif
