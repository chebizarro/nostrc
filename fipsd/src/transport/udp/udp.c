#define _POSIX_C_SOURCE 200809L
#include "transport/udp/internal.h"
#include "libgo/fiber.h"
#include <stdlib.h>
#include <string.h>

static void gate_lock(udp_instance *u) { (void)go_channel_send(u->gate, u); }
static void gate_unlock(udp_instance *u) { void *v; (void)go_channel_receive(u->gate, &v); }
static fipsd_bind_result bind_instance(void *arg, fipsd_runtime *rt, GoChannel *stop,
    const fipsd_presence_facts *facts, uint64_t generation)
{
    (void)rt; (void)facts; udp_instance *u = arg;
    gate_lock(u);
    if (go_channel_is_closed(stop) || go_context_is_canceled(u->root)) { gate_unlock(u); return FIPSD_BIND_ABSENT; }
    udp_socket *s = fipsd_udp_socket_open(u, &u->bind_to, NULL, generation, FIPS_LINK_INBOUND);
    nsync_mu_lock(&u->mu); u->listener = s; nsync_mu_unlock(&u->mu);
    gate_unlock(u);
    if (!s) return FIPSD_BIND_RETRY;
    (void)go_channel_try_send(u->bound, u);
    for (size_t i = 0; i < u->bound_watcher_count; ++i) (void)go_channel_try_send(u->bound_watchers[i], &u->transport);
    return FIPSD_BIND_OK;
}
static void unbind_instance(void *arg, uint64_t generation)
{
    udp_instance *u = arg;
    gate_lock(u);
    udp_socket *peers[FIPSD_UDP_MAX_PEERS], *listener;
    nsync_mu_lock(&u->mu);
    if (!u->listener || u->listener->generation != generation) { nsync_mu_unlock(&u->mu); gate_unlock(u); return; }
    listener = u->listener; u->listener = NULL;
    memcpy(peers, u->peers, sizeof peers); memset(u->peers, 0, sizeof u->peers); memset(u->routes, 0, sizeof u->routes);
    nsync_mu_unlock(&u->mu);
    for (size_t i = 0; i < FIPSD_UDP_MAX_PEERS; ++i) if (peers[i]) {
        fipsd_transport_stats_pool(&u->shared.stats, peers[i]->direction, false);
        fipsd_udp_socket_close(peers[i]);
    }
    fipsd_udp_socket_close(listener); gate_unlock(u);
}
static int start_async(fipsd_transport *t, fipsd_runtime *rt, const fipsd_node_context *node, GoChannel *events)
{
    udp_instance *u = t->user;
    if (u->started) return -1;
    u->runtime = rt; u->root = fipsd_runtime_context(rt); u->events = events; u->started = true;
    return fipsd_watcher_start(u->watcher, rt, node, events);
}
static void stop(fipsd_transport *t) { fipsd_watcher_stop(((udp_instance *)t->user)->watcher); }
static void network_changed(fipsd_transport *t, const struct fipsd_netmon_event *e)
{ fipsd_watcher_network_changed(((udp_instance *)t->user)->watcher, e); }
static uint16_t mtu(const fipsd_transport *t, const fips_transport_addr *to)
{ (void)to; return ((const udp_instance *)t->user)->config.mtu; }
static int discover(fipsd_transport *t) { (void)t; return 0; } /* configured peers, not multicast */
static int send_frame(fipsd_transport *t, const fips_transport_addr *to, const uint8_t *bytes, size_t len)
{
    udp_instance *u = t->user;
    if (!u->started || !bytes || !len || go_context_is_canceled(u->root)) return -1;
    if (len > u->config.mtu) { atomic_fetch_add(&u->shared.stats.mtu_exceeded, 1); return -1; }
    fipsd_udp_endpoint target;
    if (!fipsd_udp_resolve(&u->dns, to, u->bind_to.address.ss_family, &target)) { fipsd_transport_stats_error(&u->shared.stats, true); return -1; }
    fips_transport_addr numeric;
    if (!fipsd_udp_endpoint_addr(&target, &numeric)) return -1;
    fipsd_udp_packet_info source = {0};
    nsync_mu_lock(&u->mu);
    udp_socket *s = u->listener;
    for (size_t i = 0; i < u->config.peer_capacity; ++i)
        if (u->peers[i] && !go_channel_is_closed(u->peers[i]->done) &&
            fips_transport_addr_equal(&u->peers[i]->peer, &numeric)) { s = u->peers[i]; break; }
    for (size_t i = 0; i < FIPSD_UDP_MAX_PEERS; ++i)
        if (u->routes[i].used && fips_transport_addr_equal(&u->routes[i].peer, &numeric)) { source = u->routes[i].info; break; }
    if (s) ++s->users;
    nsync_mu_unlock(&u->mu);
    if (!s) return -1;
    struct iovec iov = {.iov_base = (void *)bytes, .iov_len = len};
    union { struct cmsghdr align; uint8_t data[128]; } control;
    struct msghdr m = {.msg_iov = &iov, .msg_iovlen = 1};
    if (!s->connected) {
        m.msg_name = &target.address; m.msg_namelen = target.len;
        m.msg_controllen = fipsd_udp_source_control(control.data, sizeof control.data, &source);
        if (m.msg_controllen) m.msg_control = control.data;
    }
    /* Datagram backpressure is a send failure, not an unbounded dispatcher
     * stall. All bytes remain borrowed only through this single sendmsg. */
    ssize_t n = gof_sendmsg(s->fd, &m, 0, 0);
    fipsd_udp_socket_release(s);
    if (n != (ssize_t)len) { fipsd_transport_stats_error(&u->shared.stats, true); return -1; }
    fipsd_transport_stats_sent(&u->shared.stats, len); return 0;
}
static void authenticated(fipsd_transport *t, const fips_transport_addr *address, fips_link_direction direction)
{
    udp_instance *u = t->user;
    if (!u->started || go_context_is_canceled(u->root)) return;
    fipsd_udp_endpoint remote;
    if (!fipsd_udp_resolve(&u->dns, address, u->bind_to.address.ss_family, &remote)) return;
    fips_transport_addr numeric;
    if (!fipsd_udp_endpoint_addr(&remote, &numeric)) return;
    nsync_mu_lock(&u->mu);
    for (size_t i = 0; i < u->config.peer_capacity; ++i)
        if (u->peers[i] && fips_transport_addr_equal(&u->peers[i]->peer, &numeric)) {
            nsync_mu_unlock(&u->mu); return;
        }
    nsync_mu_unlock(&u->mu);
    gate_lock(u);
    nsync_mu_lock(&u->mu);
    udp_socket *listener = u->listener;
    size_t slot = u->config.peer_capacity;
    for (size_t i = 0; i < u->config.peer_capacity; ++i) {
        if (u->peers[i] && fips_transport_addr_equal(&u->peers[i]->peer, &numeric)) {
            nsync_mu_unlock(&u->mu); gate_unlock(u); return;
        }
        if (!u->peers[i]) slot = i;
    }
    fipsd_udp_endpoint local = listener ? listener->local : u->bind_to;
    /* Preserve ingress destination when the listener was wildcard-bound. */
    for (size_t i = 0; i < FIPSD_UDP_MAX_PEERS; ++i) if (u->routes[i].used &&
            fips_transport_addr_equal(&u->routes[i].peer, &numeric) && u->routes[i].info.has_destination) {
        if (local.address.ss_family == AF_INET)
            ((struct sockaddr_in *)&local.address)->sin_addr = u->routes[i].info.destination.v4;
        else ((struct sockaddr_in6 *)&local.address)->sin6_addr = u->routes[i].info.destination.v6;
        break;
    }
    nsync_mu_unlock(&u->mu);
    if (listener && slot < u->config.peer_capacity && !go_context_is_canceled(u->root)) {
        udp_socket *s = fipsd_udp_socket_open(u, &local, &remote, listener->generation, direction);
        if (s) {
            nsync_mu_lock(&u->mu); u->peers[slot] = s; nsync_mu_unlock(&u->mu);
            fipsd_transport_stats_pool(&u->shared.stats, direction, true);
        } else atomic_fetch_add(&u->shared.diagnostics.socket_errors, 1);
    } else if (listener && slot == u->config.peer_capacity) atomic_fetch_add(&u->shared.diagnostics.pool_full, 1);
    gate_unlock(u);
}
static void close_connection(fipsd_transport *t, const fips_transport_addr *address)
{
    udp_instance *u = t->user;
    fipsd_udp_endpoint remote;
    if (!fipsd_udp_resolve(&u->dns, address, u->bind_to.address.ss_family, &remote)) return;
    fips_transport_addr numeric;
    if (!fipsd_udp_endpoint_addr(&remote, &numeric)) return;
    gate_lock(u);
    udp_socket *s = NULL;
    nsync_mu_lock(&u->mu);
    for (size_t i = 0; i < u->config.peer_capacity; ++i)
        if (u->peers[i] && fips_transport_addr_equal(&u->peers[i]->peer, &numeric)) { s = u->peers[i]; u->peers[i] = NULL; break; }
    nsync_mu_unlock(&u->mu);
    if (s) { fipsd_transport_stats_pool(&u->shared.stats, s->direction, false); fipsd_udp_socket_close(s); }
    gate_unlock(u);
}
static bool local_addr(const fipsd_transport *t, fips_transport_addr *out)
{ return fipsd_udp_address((fipsd_transport *)t, out); }
static const fipsd_transport_ops ops = {.local_addr = local_addr, .start_async = start_async, .stop = stop,
    .state = fipsd_transport_shared_state, .mtu = mtu, .send = send_frame, .discover = discover,
    .close_connection = close_connection, .network_changed = network_changed, .authenticated = authenticated};
void fipsd_udp_config_default(fipsd_udp_config *c)
{
    *c = (fipsd_udp_config){.bind_addr = "0.0.0.0:2121", .mtu = 1357, .recv_buf_size = 2097152,
        .send_buf_size = 2097152, .peer_capacity = FIPSD_UDP_MAX_PEERS, .accept_connections = true};
}
fipsd_transport *fipsd_udp_create(fips_transport_id id, const char *name, const fipsd_udp_config *c)
{
    if (!name || !c || !c->bind_addr || strlen(name) >= 128 || strlen(c->bind_addr) > 256 ||
        !c->mtu || c->peer_capacity > FIPSD_UDP_MAX_PEERS ||
        (c->interface_name && (!*c->interface_name || strlen(c->interface_name) >= 64))) return NULL;
    udp_instance *u = calloc(1, sizeof *u);
    if (!u) return NULL;
    if (!fipsd_udp_endpoint_numeric(c->bind_addr, &u->bind_to)) { free(u); return NULL; }
    nsync_mu_init(&u->mu); fipsd_udp_dns_init(&u->dns);
    strcpy(u->name, name); strcpy(u->bind_addr, c->bind_addr); u->config = *c;
    u->config.bind_addr = u->bind_addr;
    if (c->interface_name) { strcpy(u->interface_name, c->interface_name); u->config.interface_name = u->interface_name; }
    u->transport = (fipsd_transport){.ops = &ops, .id = id, .type = "udp", .name = u->name,
        .auto_connect = true, .accept_connections = c->accept_connections, .user = u,
        .interface_name = c->interface_name ? u->interface_name : NULL};
    fipsd_transport_shared_init(&u->transport, &u->shared, c->optional);
    u->gate = go_channel_create(1); u->bound = go_channel_create(1);
    fipsd_binder_ops binder = {.bind = bind_instance, .unbind = unbind_instance, .user = u};
    u->watcher = fipsd_watcher_create(&u->transport, &binder, c->watcher_source);
    if (!u->gate || !u->bound || !u->watcher) { fipsd_udp_destroy(&u->transport); return NULL; }
    return &u->transport;
}
void fipsd_udp_destroy(fipsd_transport *t)
{
    if (!t) return;
    udp_instance *u = t->user;
    fipsd_watcher_destroy(u->watcher);
    if (u->gate) go_channel_unref(u->gate);
    if (u->bound) go_channel_unref(u->bound);
    free(u);
}
bool fipsd_udp_address(fipsd_transport *t, fips_transport_addr *out)
{
    udp_instance *u = t->user; nsync_mu_lock(&u->mu);
    bool ok = u->listener && fipsd_udp_endpoint_addr(&u->listener->local, out);
    nsync_mu_unlock(&u->mu); return ok;
}
GoChannel *fipsd_udp_bound(fipsd_transport *t) { return ((udp_instance *)t->user)->bound; }
int fipsd_udp_watch_bound(fipsd_transport *t, GoChannel *ch)
{
    udp_instance *u = t->user;
    if (!ch || u->bound_watcher_count >= FIPSD_UDP_BOUND_WATCHERS) return -1;
    u->bound_watchers[u->bound_watcher_count++] = ch;
    return 0;
}
size_t fipsd_udp_connected_count(fipsd_transport *t)
{
    udp_instance *u = t->user; size_t n = 0; nsync_mu_lock(&u->mu);
    for (size_t i = 0; i < u->config.peer_capacity; ++i) if (u->peers[i]) ++n;
    nsync_mu_unlock(&u->mu); return n;
}
bool fipsd_udp_register_classifier(fipsd_transport *t, const char *name, fipsd_udp_classifier fn, void *user)
{
    udp_instance *u = t->user;
    return !u->started && fipsd_udp_classifier_add(&u->classifiers, name, fn, user);
}
int fipsd_udp_register_config(fipsd_transport_registry *r, const fipsd_config *config,
    fipsd_transport **owned, size_t *count)
{
    const fipsd_config_value *udp = fipsd_config_get(config, "transports.udp");
    if (!udp) return 0;
    bool single = fipsd_config_member(udp, "bind_addr") != NULL;
    size_t n = single ? 1 : fipsd_config_count(udp);
    for (size_t i = 0; i < n; ++i) {
        if (*count == FIPSD_TRANSPORT_MAX) return -1;
        const char *name = single ? "udp" : fipsd_config_key(udp, i);
        const fipsd_config_value *v = single ? udp : fipsd_config_member(udp, name);
        fipsd_udp_config c; fipsd_udp_config_default(&c);
        c.bind_addr = fipsd_config_string(fipsd_config_member(v, "bind_addr"));
        c.interface_name = fipsd_config_string(fipsd_config_member(v, "interface"));
        c.mtu = (uint16_t)fipsd_config_uint(fipsd_config_member(v, "mtu"));
        c.recv_buf_size = (size_t)fipsd_config_uint(fipsd_config_member(v, "recv_buf_size"));
        c.send_buf_size = (size_t)fipsd_config_uint(fipsd_config_member(v, "send_buf_size"));
        c.accept_connections = fipsd_config_bool(fipsd_config_member(v, "accept_connections")) &&
            !fipsd_config_bool(fipsd_config_member(v, "outbound_only"));
        if (fipsd_config_bool(fipsd_config_member(v, "outbound_only"))) c.bind_addr = "0.0.0.0:0";
        fipsd_transport *t = fipsd_udp_create((fips_transport_id){(uint32_t)r->count + 1}, name, &c);
        if (!t) return -1;
        if (fipsd_transport_register(r, t)) { fipsd_udp_destroy(t); return -1; }
        owned[(*count)++] = t;
    }
    return 0;
}

bool fipsd_udp_register_datagram_classifier(fipsd_transport *t, const char *name, fipsd_udp_datagram_classifier fn, void *user)
{
    udp_instance *u = t->user;
    return !u->started && fipsd_udp_datagram_classifier_add(&u->classifiers, name, fn, user);
}
bool fipsd_udp_resolve_address(fipsd_transport *t, const fips_transport_addr *in, fips_transport_addr *out)
{
    udp_instance *u = t->user; fipsd_udp_endpoint endpoint;
    return fipsd_udp_resolve(&u->dns, in, u->bind_to.address.ss_family, &endpoint) &&
        fipsd_udp_endpoint_addr(&endpoint, out);
}

bool fipsd_udp_unregister_datagram_classifier(fipsd_transport *t, const char *name, void *user)
{
    udp_instance *u = t->user;
    if (u->started) return false; /* no registry mutation while drains exist */
    for (size_t i = 0; i < u->classifiers.count; ++i) {
        fipsd_udp_classifier_entry *e = &u->classifiers.entries[i];
        if (e->user == user && !strcmp(e->subsystem, name)) {
            memmove(e, e + 1, (--u->classifiers.count - i) * sizeof(*e)); return true;
        }
    }
    return false;
}
