#ifndef FIPSD_UDP_INTERNAL_H
#define FIPSD_UDP_INTERNAL_H
#include "transport/udp/udp.h"
#include "transport/udp/dns.h"
#include "transport/watcher.h"
#include "transport/sockos.h"
#include <stdatomic.h>
typedef struct udp_instance udp_instance;
typedef struct udp_socket {
    udp_instance *owner;
    gof_socket_t fd;
    bool connected;
    fips_link_direction direction;
    fipsd_udp_endpoint local, remote;
    fips_transport_addr peer;
    uint64_t generation;
    GoChannel *stop, *done, *unused;
    size_t users; /* owner.mu; send leases, never descriptor ownership */
    bool detached; /* owner.mu */
    fipsd_buffer_granted recv_grant, send_grant;
} udp_socket;
typedef struct udp_route {
    fips_transport_addr peer;
    fipsd_udp_packet_info info;
    bool used;
} udp_route;
struct udp_instance {
    fipsd_transport transport;
    fipsd_transport_shared shared;
    fipsd_udp_config config;
    char name[128], bind_addr[257], interface_name[64];
    fipsd_udp_endpoint bind_to;
    fipsd_udp_dns dns;
    fipsd_udp_classifiers classifiers;
    fipsd_watcher *watcher;
    fipsd_runtime *runtime;
    GoContext *root;
    GoChannel *events, *gate, *bound;
    /* Extra bind observers (fipsd_udp_watch_bound), set before start. */
    GoChannel *bound_watchers[FIPSD_UDP_BOUND_WATCHERS];
    size_t bound_watcher_count;
    nsync_mu mu;
    udp_socket *listener, *peers[FIPSD_UDP_MAX_PEERS];
    udp_route routes[FIPSD_UDP_MAX_PEERS];
    size_t next_route;
    bool started;
};
/* Lifecycle calls hold the channel gate, never a scheduler-blocking mutex. */
udp_socket *fipsd_udp_socket_open(udp_instance *, const fipsd_udp_endpoint *,
    const fipsd_udp_endpoint *remote, uint64_t, fips_link_direction);
void fipsd_udp_socket_close(udp_socket *);
void fipsd_udp_receive(void *);
void fipsd_udp_socket_release(udp_socket *);
#endif
