#ifndef FIPSD_ETH_INTERNAL_H
#define FIPSD_ETH_INTERNAL_H
#include "transport/ethernet/ethernet.h"
#include "transport/sockbuf.h"
#include <sys/types.h>
typedef struct eth_instance eth_instance;
typedef struct eth_socket {
    eth_instance *owner;
    int fd, index, error;
    uint16_t mtu, ethertype;
    uint8_t mac[6];
    uint64_t generation;
    GoChannel *stop, *recv_done, *beacon_done, *unused;
    size_t users;
    bool detached;
    uint8_t *batch;
    size_t batch_capacity, batch_len, batch_at;
    uint32_t drops_seen;
} eth_socket;
struct eth_instance {
    fipsd_transport transport;
    fipsd_transport_shared shared;
    fipsd_ethernet_config config;
    char name[128], interface_name[64];
    nsync_mu mu;
    eth_socket *socket;
    fipsd_eth_neighbors neighbors;
    fipsd_watcher *watcher;
    fipsd_runtime *runtime;
    const fipsd_node_context *node;
    GoContext *root;
    GoChannel *events;
};
/* Setup/close run on blocking executor. All reads/writes are fiber I/O.
 * Only receive owner reads resetting kernel drop counters. */
int fipsd_eth_io_open(eth_socket *, const fipsd_ethernet_config *);
int fipsd_eth_io_bound_index(eth_socket *);
ssize_t fipsd_eth_io_recv(eth_socket *, uint8_t *, size_t, uint8_t[6]);
ssize_t fipsd_eth_io_send(eth_socket *, const uint8_t[6], const uint8_t *, size_t);
uint32_t fipsd_eth_io_drops(eth_socket *);
/* Darwin BPF uses timeval32 even on 64-bit processes. Read with memcpy,
 * never an unaligned native bpf_hdr/timeval cast. Available to codec tests. */
typedef struct fipsd_eth_bpf_header {
    uint32_t sec, usec, caplen, datalen;
    uint16_t hdrlen, pad;
} fipsd_eth_bpf_header;
_Static_assert(sizeof(fipsd_eth_bpf_header) == 20, "BPF timeval32 ABI");
_Static_assert(offsetof(fipsd_eth_bpf_header, hdrlen) == 16, "BPF hdrlen ABI");
bool fipsd_eth_bpf_next(const uint8_t *, size_t, size_t *, uint16_t,
                      const uint8_t **, size_t *, uint8_t[6]);
int fipsd_eth_dispatch_frame(eth_instance *, const fips_secp *, const uint8_t *, size_t,
                             const uint8_t[6], const uint8_t **, size_t *);
void fipsd_eth_stats_recv(eth_instance *, size_t);
void fipsd_eth_stats_sent(eth_instance *, size_t);
#endif
