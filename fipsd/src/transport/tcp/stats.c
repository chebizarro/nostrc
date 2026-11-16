#include "transport/tcp/internal.h"
void tcp_stats_error(tcp_instance *u, bool send)
{ fipsd_transport_stats_error(&u->shared.stats, send); }
fipsd_tcp_stats_view fipsd_tcp_stats_snapshot(const fipsd_transport *t)
{ return fipsd_transport_stats_read(&t->shared->stats); }
