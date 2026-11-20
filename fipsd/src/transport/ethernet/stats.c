#include "transport/ethernet/internal.h"
void fipsd_eth_stats_recv(eth_instance *u, size_t n)
{ fipsd_transport_stats_recv(&u->shared.stats, n); atomic_fetch_add(&u->shared.stats.frames_recv, 1); }
void fipsd_eth_stats_sent(eth_instance *u, size_t n)
{ fipsd_transport_stats_sent(&u->shared.stats, n); atomic_fetch_add(&u->shared.stats.frames_sent, 1); }
