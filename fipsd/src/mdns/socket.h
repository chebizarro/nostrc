#ifndef FIPSD_MDNS_SOCKET_H
#define FIPSD_MDNS_SOCKET_H
/* The default mDNS I/O: one IPv4 socket on 0.0.0.0:5353 and one IPv6
 * (V6ONLY) on [::]:5353, both with SO_REUSEADDR/SO_REUSEPORT so they share
 * the port with the system responder (mDNSResponder, avahi-daemon,
 * systemd-resolved), multicast TTL/hop limit 255 (RFC 6762 Section 11),
 * multicast loop on (nodes on one host must hear each other), the arrival
 * interface read per datagram (IP_PKTINFO / IP_RECVIF, IPV6_PKTINFO), and
 * on Linux IP_MULTICAST_ALL off so only joined groups are delivered. Group
 * membership and the outgoing interface are set per interface. A family
 * whose socket cannot be opened is left out; open fails only when neither
 * opens. */
#include "mdns/mdns.h"
#include <stdatomic.h>

struct fipsd_mdns_sockets;
typedef struct fipsd_mdns_receiver { struct fipsd_mdns_sockets *owner; int index; } fipsd_mdns_receiver;
typedef struct fipsd_mdns_sockets {
    int fd[2];                 /* IPv4, IPv6; -1 when absent */
    GoChannel *rx, *done;
    atomic_bool stopping;
    int receivers;
    fipsd_mdns_receiver arg[2];
} fipsd_mdns_sockets;
void fipsd_mdns_sockets_init(fipsd_mdns_sockets *s);
fipsd_mdns_io fipsd_mdns_sockets_io(fipsd_mdns_sockets *s);
/* Close whatever open left behind when the engine never ran. */
void fipsd_mdns_sockets_release(fipsd_mdns_sockets *s);
#endif
