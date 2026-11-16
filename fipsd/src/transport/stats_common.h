#ifndef FIPSD_TRANSPORT_STATS_H
#define FIPSD_TRANSPORT_STATS_H
#include <stdatomic.h>
#include <fips/transport/types.h>
typedef struct fipsd_transport_stats {
    _Atomic uint64_t packets_sent, packets_recv, bytes_sent, bytes_recv;
    _Atomic uint64_t dropped, errors, inbound, outbound;
    _Atomic uint64_t ecn_recv, ce_recv, pktinfo_recv, kernel_drops;
    _Atomic uint64_t classifier_drops, malformed, pool_full;
} fipsd_transport_stats;
void fipsd_transport_stats_init(fipsd_transport_stats *);
void fipsd_transport_stats_sent(fipsd_transport_stats *, size_t);
void fipsd_transport_stats_recv(fipsd_transport_stats *, size_t);
fips_link_stats fipsd_transport_stats_snapshot(const fipsd_transport_stats *);
/* Called exactly once per successfully added/removed pool entry. */
void fipsd_transport_stats_pool(fipsd_transport_stats *, fips_link_direction, bool added);
#endif
