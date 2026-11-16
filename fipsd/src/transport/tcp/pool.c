#include "transport/tcp/internal.h"
#include <stdlib.h>
#include <string.h>
static void source(const tcp_endpoint *e, uint8_t out[16])
{
    memset(out, 0, 16);
    if (e->address.ss_family == AF_INET) {
        out[10] = out[11] = 0xff;
        memcpy(out + 12, &((const struct sockaddr_in *)&e->address)->sin_addr, 4);
    } else {
        const struct in6_addr *ip = &((const struct sockaddr_in6 *)&e->address)->sin6_addr;
        memcpy(out, ip, IN6_IS_ADDR_V4MAPPED(ip) ? 16 : 8);
    }
}
bool tcp_same_source(const tcp_endpoint *a, const tcp_endpoint *b)
{ uint8_t x[16], y[16]; source(a, x); source(b, y); return !memcmp(x, y, 16); }
tcp_conn *tcp_pool_find(tcp_instance *u, const fips_transport_addr *peer)
{
    tcp_conn *best = NULL;
    for (tcp_conn *c = u->connections; c; c = c->next) {
        if (c->closing || !fips_transport_addr_equal(&c->peer, peer)) continue;
        if (c->direction == FIPS_LINK_OUTBOUND) return c;
        if (!best || c->id > best->id) best = c;
    }
    return best;
}
int tcp_pool_launch(tcp_instance *u, int fd, const fips_transport_addr *peer,
    const tcp_endpoint *remote, const tcp_endpoint *local, fips_link_direction direction)
{
    bool inbound = direction == FIPS_LINK_INBOUND;
    nsync_mu_lock(&u->mu);
    if (!u->binding || go_channel_is_closed(u->stop) || go_context_is_canceled(u->root)) goto fail;
    /* Keep disconnect notification ordered before any replacement at this
     * endpoint. The event seam names an endpoint, not a connection id. */
    for (tcp_conn *old = u->connections; old; old = old->next)
        if (old->closing && fips_transport_addr_equal(&old->peer, peer)) goto fail;
    if (!inbound && tcp_pool_find(u, peer)) { nsync_mu_unlock(&u->mu); return 0; }
    size_t from = 0;
    if (inbound) for (tcp_conn *c = u->connections; c; c = c->next)
        if (c->direction == FIPS_LINK_INBOUND && tcp_same_source(&c->remote, remote)) ++from;
    if ((inbound && (u->inbound >= u->config.max_inbound_connections || from >= u->config.max_inbound_per_source)) ||
        (!inbound && u->outbound >= u->config.max_outbound_connections)) {
        atomic_fetch_add(&u->shared.diagnostics.pool_full, 1);
        if (inbound) atomic_fetch_add(&u->shared.stats.connections_rejected, 1);
        if (inbound && u->inbound < u->config.max_inbound_connections &&
            from >= u->config.max_inbound_per_source) atomic_fetch_add(&u->shared.stats.source_rejected, 1);
        goto fail;
    }
    tcp_conn *c = calloc(1, sizeof *c);
    if (!c) goto fail;
    c->owner = u; c->fd = fd; c->peer = *peer; c->direction = direction; c->id = fipsd_stream_next_id();
    c->accepted_ms = fipsd_transport_now_ms();
    if (remote) c->remote = *remote;
    if (local) c->local = *local;
    c->stop = go_channel_create(1); c->notify_stop = go_channel_create(1); c->close = go_channel_create(1); c->ready = go_channel_create(1);
    c->run = go_channel_create(1); c->reader_done = go_channel_create(1); c->writer_done = go_channel_create(1);
    c->progress = go_channel_create(1); c->queue = go_channel_create(u->config.send_queue_depth);
    atomic_init(&c->last_frame_ms, 0);
    if (!c->stop || !c->notify_stop || !c->close || !c->ready || !c->run || !c->reader_done || !c->writer_done || !c->progress || !c->queue) {
        tcp_conn_free(c); goto fail;
    }
    c->next = u->connections; u->connections = c;
    if (inbound) ++u->inbound; else ++u->outbound;
    if (inbound) fipsd_transport_stats_pool(&u->shared.stats, direction, true);
    if (fipsd_runtime_spawn(u->runtime, tcp_conn_owner, c)) {
        u->connections = c->next;
        if (inbound) --u->inbound; else --u->outbound;
        if (inbound) fipsd_transport_stats_pool(&u->shared.stats, direction, false);
        tcp_conn_free(c); goto fail;
    }
    nsync_mu_unlock(&u->mu); return 0;
fail:
    nsync_mu_unlock(&u->mu); return -1;
}
void tcp_pool_remove(tcp_conn *c)
{
    tcp_instance *u = c->owner;
    nsync_mu_lock(&u->mu);
    tcp_conn **p = &u->connections;
    while (*p && *p != c) p = &(*p)->next;
    if (*p) {
        *p = c->next;
        if (c->direction == FIPS_LINK_INBOUND) --u->inbound; else --u->outbound;
        if (c->direction == FIPS_LINK_INBOUND || c->established)
            fipsd_transport_stats_pool(&u->shared.stats, c->direction, false);
    }
    nsync_mu_unlock(&u->mu);
    (void)go_channel_try_send(u->changed, u);
}
void tcp_pool_stop(tcp_instance *u)
{
    nsync_mu_lock(&u->mu);
    u->binding = false;
    for (tcp_conn *c = u->connections; c; c = c->next) { go_channel_close(c->notify_stop); go_channel_close(c->stop); }
    nsync_mu_unlock(&u->mu);
    for (;;) {
        nsync_mu_lock(&u->mu); bool empty = u->connections == NULL; nsync_mu_unlock(&u->mu);
        if (empty) break;
        void *v; (void)go_channel_receive(u->changed, &v);
    }
}
