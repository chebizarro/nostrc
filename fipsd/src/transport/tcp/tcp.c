#include "transport/tcp/internal.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
static int start_async(fipsd_transport *t, fipsd_runtime *rt, const fipsd_node_context *node, GoChannel *events)
{
    tcp_instance *u = t->user;
    if (u->started) return -1;
    u->runtime = rt; u->root = fipsd_runtime_context(rt); u->events = events; u->started = true;
    return fipsd_watcher_start(u->watcher, rt, node, events);
}
static void stop(fipsd_transport *t)
{
    tcp_instance *u = t->user;
    go_channel_close(u->stop); fipsd_watcher_stop(u->watcher);
}
static void network_changed(fipsd_transport *t, const struct fipsd_netmon_event *e)
{ fipsd_watcher_network_changed(((tcp_instance *)t->user)->watcher, e); }
static uint16_t mtu(const fipsd_transport *t, const fips_transport_addr *to)
{ (void)to; return ((const tcp_instance *)t->user)->config.mtu; }
static int discover(fipsd_transport *t) { (void)t; return 0; }
int tcp_connected_locked(tcp_conn *c)
{
    tcp_instance *u = c->owner;
    if (c->closing || go_channel_is_closed(c->stop) || go_channel_is_closed(u->stop) ||
        go_context_is_canceled(u->root)) return -1;
    fipsd_transport_event *e = calloc(1, sizeof *e);
    if (!e) return -1;
    e->kind = FIPSD_TRANSPORT_CONNECTED; e->id = u->transport.id; e->address = c->peer;
    if (go_channel_try_send(u->events, e)) { free(e); return -1; }
    return 0;
}
static int connect_async(fipsd_transport *t, const fips_transport_addr *to)
{
    tcp_instance *u = t->user;
    if (!u->started || !to || !to->len || to->len > 256 || memchr(to->bytes, 0, to->len)) return -1;
    nsync_mu_lock(&u->mu);
    tcp_conn *c = tcp_pool_find(u, to);
    if (c && c->established) {
        /* Publication and close share the lock: DISCONNECTED cannot overtake
         * an established fast-path CONNECTED notification. */
        int rc = tcp_connected_locked(c); nsync_mu_unlock(&u->mu); return rc;
    }
    nsync_mu_unlock(&u->mu);
    return tcp_pool_launch(u, -1, to, NULL, NULL, FIPS_LINK_OUTBOUND);
}
static int send_frame(fipsd_transport *t, const fips_transport_addr *to, const uint8_t *bytes, size_t n)
{
    tcp_instance *u = t->user;
    if (!u->started || !to || !bytes || !to->len || to->len > 256 || go_channel_is_closed(u->stop) ||
        go_context_is_canceled(u->root)) return -1;
    if (n > u->config.mtu) { atomic_fetch_add(&u->shared.stats.mtu_exceeded, 1); return -1; }
    if (fipsd_datagram_frame(bytes, n, UINT16_MAX) != FIPS_OK) return -1;
    /* The node vtable is Rust's send_existing seam: the peer retry machine
     * explicitly calls connect_async before IK. Late encryption completions
     * must not resurrect a deliberately closed connection. */
    tcp_frame *frame = malloc(sizeof *frame + n);
    if (!frame) return -1;
    frame->len = n; memcpy(frame->bytes, bytes, n);
    nsync_mu_lock(&u->mu); tcp_conn *c = tcp_pool_find(u, to);
    bool connected = c != NULL;
    int rc = c ? go_channel_try_send(c->queue, frame) : -1;
    nsync_mu_unlock(&u->mu);
    if (rc) {
        free(frame);
        if (connected) tcp_stats_error(u, true);
        atomic_fetch_add(&u->shared.diagnostics.dropped, 1); return -1;
    }
    return 0;
}
static void close_connection(fipsd_transport *t, const fips_transport_addr *to)
{
    tcp_instance *u = t->user;
    nsync_mu_lock(&u->mu); tcp_conn *c = tcp_pool_find(u, to);
    /* A retiring owner may be backpressured on its exit event. Explicit
     * close must cancel that publication too, even after send lookup detaches. */
    if (!c) for (tcp_conn *p = u->connections; p; p = p->next)
        if (fips_transport_addr_equal(&p->peer, to) && (!c || p->id > c->id)) c = p;
    if (c) { c->closing = true; go_channel_close(c->notify_stop); go_channel_close(c->close); }
    nsync_mu_unlock(&u->mu);
}
static bool local_addr(const fipsd_transport *t, fips_transport_addr *out)
{ return fipsd_tcp_address((fipsd_transport *)t, out); }
static const fipsd_transport_ops ops = {.local_addr = local_addr, .start_async = start_async, .stop = stop,
    .state = fipsd_transport_shared_state, .mtu = mtu, .send = send_frame, .discover = discover,
    .connect_async = connect_async, .close_connection = close_connection, .network_changed = network_changed};
void fipsd_tcp_config_default(fipsd_tcp_config *c)
{
    *c = (fipsd_tcp_config){.mtu = 1400, .connect_timeout_ms = 5000, .keepalive_secs = 30,
        .recv_buf_size = 2097152, .send_buf_size = 2097152, .max_inbound_connections = 256,
        .max_inbound_per_source = 8, .max_outbound_connections = 64, .send_queue_depth = FIPSD_TCP_SEND_DEPTH,
        .nodelay = true, .first_frame_ms = 30000, .idle_ms = 64000};
}
fipsd_transport *fipsd_tcp_create(fips_transport_id id, const char *name, const fipsd_tcp_config *c)
{
    if (!name || !*name || strlen(name) >= 128 || !c || !c->mtu || !c->connect_timeout_ms ||
        c->connect_timeout_ms > INT_MAX || c->keepalive_secs > INT_MAX || !c->send_queue_depth ||
        c->send_queue_depth > FIPSD_TCP_SEND_DEPTH || !c->first_frame_ms || !c->idle_ms ||
        (c->bind_addr && (!*c->bind_addr || strlen(c->bind_addr) > 256))) return NULL;
    if (c->bind_addr) {
        fips_transport_addr bind_to; tcp_endpoint *items = NULL; size_t count = 0;
        if (fips_transport_addr_from_string(c->bind_addr, strlen(c->bind_addr), &bind_to) != FIPS_OK ||
            !tcp_resolve(&bind_to, true, &items, &count)) return NULL;
        free(items);
    }
    tcp_instance *u = calloc(1, sizeof *u); if (!u) return NULL;
    nsync_mu_init(&u->mu); u->listener_fd = -1; u->config = *c;
    strcpy(u->name, name);
    if (c->bind_addr) { strcpy(u->bind_addr, c->bind_addr); u->config.bind_addr = u->bind_addr; }
    u->transport = (fipsd_transport){.ops = &ops, .id = id, .type = "tcp", .name = u->name,
        .auto_connect = true, .accept_connections = c->bind_addr != NULL, .user = u};
    fipsd_transport_shared_init(&u->transport, &u->shared, false);
    u->stop = go_channel_create(1); u->bound = go_channel_create(1);
    u->accept_done = go_channel_create(1); u->changed = go_channel_create(1);
    fipsd_binder_ops binder = {.bind = tcp_bind, .unbind = tcp_unbind, .user = u};
    u->watcher = fipsd_watcher_create(&u->transport, &binder, c->watcher_source);
    if (!u->stop || !u->bound || !u->accept_done || !u->changed || !u->watcher) {
        fipsd_tcp_destroy(&u->transport); return NULL;
    }
    return &u->transport;
}
void fipsd_tcp_destroy(fipsd_transport *t)
{
    if (!t) return;
    tcp_instance *u = t->user;
    fipsd_watcher_destroy(u->watcher);
    GoChannel *channels[] = {u->stop, u->bound, u->accept_done, u->changed};
    for (size_t i = 0; i < sizeof channels / sizeof channels[0]; ++i) if (channels[i]) go_channel_unref(channels[i]);
    free(u);
}
bool fipsd_tcp_is(const fipsd_transport *t) { return t && t->ops == &ops; }
GoChannel *fipsd_tcp_bound(fipsd_transport *t) { return ((tcp_instance *)t->user)->bound; }
bool fipsd_tcp_address(fipsd_transport *t, fips_transport_addr *out)
{
    tcp_instance *u = t->user; nsync_mu_lock(&u->mu);
    bool ok = u->listener_fd >= 0 && tcp_endpoint_addr(&u->local, out);
    nsync_mu_unlock(&u->mu); return ok;
}
size_t fipsd_tcp_connected_count(fipsd_transport *t)
{
    tcp_instance *u = t->user; size_t n = 0; nsync_mu_lock(&u->mu);
    for (tcp_conn *c = u->connections; c; c = c->next) if (c->established && !c->closing) ++n;
    nsync_mu_unlock(&u->mu); return n;
}
int fipsd_tcp_register_config(fipsd_transport_registry *r, const fipsd_config *config,
    fipsd_transport **owned, size_t *count)
{
    const fipsd_config_value *tcp = fipsd_config_get(config, "transports.tcp");
    if (!tcp) return 0;
    bool single = fipsd_config_member(tcp, "mtu") != NULL;
    size_t n = single ? 1 : fipsd_config_count(tcp);
    for (size_t i = 0; i < n; ++i) {
        if (*count == FIPSD_TRANSPORT_MAX) return -1;
        const char *name = single ? "tcp" : fipsd_config_key(tcp, i);
        const fipsd_config_value *v = single ? tcp : fipsd_config_member(tcp, name);
        fipsd_tcp_config c; fipsd_tcp_config_default(&c);
        c.bind_addr = fipsd_config_string(fipsd_config_member(v, "bind_addr"));
        c.mtu = (uint16_t)fipsd_config_uint(fipsd_config_member(v, "mtu"));
        uint64_t timeout = fipsd_config_uint(fipsd_config_member(v, "connect_timeout_ms"));
        uint64_t keepalive = fipsd_config_uint(fipsd_config_member(v, "keepalive_secs"));
        if (timeout > INT_MAX || keepalive > INT_MAX) return -1;
        c.connect_timeout_ms = (uint32_t)timeout; c.keepalive_secs = (uint32_t)keepalive;
        c.nodelay = fipsd_config_bool(fipsd_config_member(v, "nodelay"));
        c.recv_buf_size = (size_t)fipsd_config_uint(fipsd_config_member(v, "recv_buf_size"));
        c.send_buf_size = (size_t)fipsd_config_uint(fipsd_config_member(v, "send_buf_size"));
        c.max_inbound_connections = (size_t)fipsd_config_uint(fipsd_config_member(v, "max_inbound_connections"));
        c.max_inbound_per_source = (size_t)fipsd_config_uint(fipsd_config_member(v, "max_inbound_per_source"));
        fipsd_config_core core; fipsd_config_core_params(config, &core);
        fips_duration hold = core.link_rekey.hold;
        c.idle_ms = hold.secs > (UINT64_MAX - hold.nanos / 1000000) / 1000 ? UINT64_MAX :
            hold.secs * 1000 + hold.nanos / 1000000;
        c.max_outbound_connections = (size_t)fipsd_config_uint(fipsd_config_get(config, "node.limits.max_connections"));
        fipsd_transport *t = fipsd_tcp_create((fips_transport_id){(uint32_t)r->count + 1}, name, &c);
        if (!t) return -1;
        if (fipsd_transport_register(r, t)) { fipsd_tcp_destroy(t); return -1; }
        owned[(*count)++] = t;
    }
    return 0;
}
