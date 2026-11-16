#include "transport/stats_common.h"
void fipsd_transport_stats_init(fipsd_transport_stats *s)
{
    atomic_init(&s->packets_sent, 0); atomic_init(&s->packets_recv, 0);
    atomic_init(&s->bytes_sent, 0); atomic_init(&s->bytes_recv, 0);
    atomic_init(&s->dropped, 0); atomic_init(&s->errors, 0);
    atomic_init(&s->inbound, 0); atomic_init(&s->outbound, 0);
    atomic_init(&s->ecn_recv, 0); atomic_init(&s->ce_recv, 0);
    atomic_init(&s->pktinfo_recv, 0); atomic_init(&s->kernel_drops, 0);
    atomic_init(&s->classifier_drops, 0); atomic_init(&s->malformed, 0); atomic_init(&s->pool_full, 0);
}
void fipsd_transport_stats_sent(fipsd_transport_stats *s, size_t n)
{ atomic_fetch_add(&s->packets_sent, 1); atomic_fetch_add(&s->bytes_sent, n); }
void fipsd_transport_stats_recv(fipsd_transport_stats *s, size_t n)
{ atomic_fetch_add(&s->packets_recv, 1); atomic_fetch_add(&s->bytes_recv, n); }
fips_link_stats fipsd_transport_stats_snapshot(const fipsd_transport_stats *s)
{
    return (fips_link_stats){.packets_sent = atomic_load(&s->packets_sent),
        .packets_recv = atomic_load(&s->packets_recv), .bytes_sent = atomic_load(&s->bytes_sent),
        .bytes_recv = atomic_load(&s->bytes_recv)};
}
void fipsd_transport_stats_pool(fipsd_transport_stats *s, fips_link_direction d, bool added)
{
    _Atomic uint64_t *n = d == FIPS_LINK_INBOUND ? &s->inbound : &s->outbound;
    if (added) atomic_fetch_add(n, 1); else atomic_fetch_sub(n, 1);
}
