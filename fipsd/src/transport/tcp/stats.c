#include "transport/tcp/internal.h"
void tcp_stats_init(tcp_stats *s)
{
    atomic_init(&s->accepted, 0); atomic_init(&s->connected, 0);
    atomic_init(&s->connect_timeouts, 0); atomic_init(&s->connect_refused, 0); atomic_init(&s->send_errors, 0);
    atomic_init(&s->recv_errors, 0); atomic_init(&s->rejected, 0);
    atomic_init(&s->source_rejected, 0); atomic_init(&s->mtu_exceeded, 0);
    atomic_init(&s->deadlines, 0);
}
void tcp_stats_error(tcp_instance *u, bool send)
{
    atomic_fetch_add(&u->shared.stats.errors, 1);
    if (send) atomic_fetch_add(&u->stats.send_errors, 1);
    else atomic_fetch_add(&u->stats.recv_errors, 1);
}

fipsd_tcp_stats_view fipsd_tcp_stats_snapshot(const fipsd_transport *t)
{
    const tcp_instance *u = t->user;
    return (fipsd_tcp_stats_view){
        .send_errors = atomic_load(&u->stats.send_errors), .recv_errors = atomic_load(&u->stats.recv_errors),
        .mtu_exceeded = atomic_load(&u->stats.mtu_exceeded), .connections_established = atomic_load(&u->stats.connected),
        .connections_accepted = atomic_load(&u->stats.accepted), .connections_rejected = atomic_load(&u->stats.rejected),
        .source_rejected = atomic_load(&u->stats.source_rejected), .connect_timeouts = atomic_load(&u->stats.connect_timeouts),
        .connect_refused = atomic_load(&u->stats.connect_refused), .pool_inbound = atomic_load(&u->shared.stats.inbound),
        .pool_outbound = atomic_load(&u->shared.stats.outbound)};
}
