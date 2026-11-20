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
    /* Linux: batched sends may use UDP GSO (probed at open; cleared for good
     * by the first refusal). Read and cleared by any sending thread. */
    _Atomic bool gso;
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
/* The largest datagram a drain takes: larger is truncated, hence malformed. */
#define FIPSD_UDP_RECV_MAX 65536u
#if defined(__linux__)
/* Linux drain (nostrc-u19cz.4): recvmmsg takes up to FIPSD_UDP_RECV_BATCH
 * datagrams per syscall, each with its own source, pktinfo and ECN control.
 * Each slot receives straight into a transport event the node will own, so
 * a frame reaches the dispatcher without a copy; the slot is refilled with a
 * fresh event. Datagrams longer than the slot (cap: the configured MTU) spill
 * into a per-slot tail, touched only by them, so every datagram the drain
 * took before is still taken, by a copy into an event of its size.
 * Includers define _GNU_SOURCE (struct mmsghdr). */
#define FIPSD_UDP_RECV_BATCH 32u
typedef struct udp_rx {
    struct mmsghdr m[FIPSD_UDP_RECV_BATCH];
    struct iovec iov[FIPSD_UDP_RECV_BATCH][2];
    fipsd_udp_endpoint source[FIPSD_UDP_RECV_BATCH];
    struct { _Alignas(struct cmsghdr) uint8_t data[512]; } control[FIPSD_UDP_RECV_BATCH];
    fipsd_transport_event *slot[FIPSD_UDP_RECV_BATCH]; /* cap payload bytes each */
    uint8_t *spill; /* FIPSD_UDP_RECV_BATCH tails of FIPSD_UDP_RECV_MAX - cap bytes */
    size_t cap, filled; /* filled: the slots the last call wrote */
} udp_rx;
udp_rx *fipsd_udp_rx_create(size_t cap); /* 0 < cap < FIPSD_UDP_RECV_MAX */
void fipsd_udp_rx_free(udp_rx *);
/* One gof_recvmmsg over every slot (parks until a datagram is queued): the
 * count, or -1 with errno. Datagram i has m[i].msg_len bytes, its first
 * min(len, cap) in slot[i]->bytes and the rest in iov[i][1]. */
int fipsd_udp_recv_batch(udp_rx *, gof_socket_t);
/* Batched sends (udp.c send_batch): frames per sendmmsg run, and the limits
 * of one GSO message (the segment count older kernels accept; total bytes
 * under the 64 KiB IP datagram with headers). */
#define FIPSD_UDP_SEND_BATCH 32u
#define FIPSD_UDP_GSO_SEGMENTS 64u
#define FIPSD_UDP_GSO_BYTES 65000u
#ifndef SOL_UDP
#define SOL_UDP 17
#endif
#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103 /* linux/udp.h; glibc's netinet/udp.h since 2.29 */
#endif
/* Test seam: the batch send syscall, gof_sendmmsg with a zero timeout by
 * default. Replace it only while no transport sends. */
extern int (*fipsd_udp_sendmmsg)(gof_socket_t, struct mmsghdr *, unsigned, int flags);
#endif
/* Lifecycle calls hold the channel gate, never a scheduler-blocking mutex. */
udp_socket *fipsd_udp_socket_open(udp_instance *, const fipsd_udp_endpoint *,
    const fipsd_udp_endpoint *remote, uint64_t, fips_link_direction);
void fipsd_udp_socket_close(udp_socket *);
void fipsd_udp_receive(void *);
void fipsd_udp_socket_release(udp_socket *);
#endif
