#define _POSIX_C_SOURCE 200809L
#include "transport/ethernet/internal.h"
#include "node/context.h"
#include "blocking_executor.h"
#include "libgo/fiber.h"
#include "select.h"
#include "log.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static void join(GoChannel *ch) { void *v = NULL; (void)go_channel_receive(ch, &v); }
static bool stopped(eth_socket *s)
{ return go_channel_is_closed(s->stop) || go_context_is_canceled(s->owner->root); }
static eth_socket *acquire(eth_instance *u)
{ nsync_mu_lock(&u->mu); eth_socket *s = u->socket; if (s) ++s->users; nsync_mu_unlock(&u->mu); return s; }
static void release(eth_socket *s)
{
    nsync_mu_lock(&s->owner->mu);
    if (--s->users == 0 && s->detached) go_channel_close(s->unused);
    nsync_mu_unlock(&s->owner->mu);
}
static int transmit(eth_socket *s, const uint8_t mac[6], const uint8_t *p, size_t n)
{
    if (stopped(s)) return -1;
    ssize_t sent = fipsd_eth_io_send(s, mac, p, n);
    if (sent != (ssize_t)n) { fipsd_transport_stats_error(&s->owner->shared.stats, true); return -1; }
    fipsd_eth_stats_sent(s->owner, n); return 0;
}
static void beacon_loop(void *arg)
{
    eth_socket *s = arg; eth_instance *u = s->owner;
    static const uint8_t broadcast[6] = {255,255,255,255,255,255};
    uint8_t beacon[34]; fips_xonly_pubkey key = fips_identity_pubkey(fipsd_node_identity(u->node));
    fipsd_eth_beacon_encode(beacon, &key);
    unsigned failures = 0;
    while (!stopped(s)) {
        if (transmit(s, broadcast, beacon, sizeof beacon)) {
            if (++failures == 1) FIPSD_WARN("ethernet", "%s: beacon send failed", u->name);
            if (failures >= 3) { fipsd_watcher_binding_exited(u->watcher, s->generation); break; }
        } else { failures = 0; atomic_fetch_add(&u->shared.stats.beacons_sent, 1); }
        void *v = NULL; GoSelectCase c[] = {{GO_SELECT_RECEIVE, s->stop, NULL, &v},
            {GO_SELECT_RECEIVE, go_context_done(u->root), NULL, &v}};
        GoSelectResult result = go_select_timeout(c, 2, u->config.beacon_interval_secs * 1000);
        if (result.selected_case >= 0) break;
    }
    go_channel_close(s->beacon_done);
}
static int drain_neighbors(eth_instance *u, GoChannel *stop)
{
    fipsd_eth_neighbor peer;
    while (fipsd_eth_neighbors_take(&u->neighbors, &peer, 1)) {
        fips_transport_addr addr = fips_transport_addr_from_mac(peer.mac);
        if (fipsd_transport_emit(u->events, u->root, stop, FIPSD_TRANSPORT_DISCOVERED,
                u->transport.id, &addr, peer.key.bytes, 32)) return -1;
    }
    return 0;
}
int fipsd_eth_dispatch_frame(eth_instance *u, const fips_secp *secp, const uint8_t *frame, size_t n,
                             const uint8_t mac[6], const uint8_t **data, size_t *len)
{
    if (!n) return 0;
    fipsd_eth_stats_recv(u, n);
    if (fipsd_eth_group(mac)) { atomic_fetch_add(&u->shared.stats.group_drops, 1); return 0; }
    if (frame[0] == 1) {
        atomic_fetch_add(&u->shared.stats.beacons_recv, 1);
        fips_xonly_pubkey key;
        if (u->config.listen && fipsd_eth_beacon_decode(secp, frame, n, &key)) {
            if (!fipsd_eth_neighbors_add(&u->neighbors, mac, &key)) atomic_fetch_add(&u->shared.stats.beacons_dropped, 1);
            return 2;
        }
        return 0;
    }
    return fipsd_eth_data_decode(frame, n, data, len) ? 1 : 0;
}
static void receive_loop(void *arg)
{
    eth_socket *s = arg; eth_instance *u = s->owner;
    uint8_t *frame = malloc(65536);
    if (!frame) goto done;
    while (!stopped(s)) {
        uint8_t mac[6]; ssize_t n = fipsd_eth_io_recv(s, frame, 65536, mac);
        if (stopped(s)) break;
        atomic_fetch_add(&u->shared.stats.kernel_drops, fipsd_eth_io_drops(s));
        if (n < 0) {
            if (errno == EINTR || errno == EMSGSIZE) continue;
            fipsd_transport_stats_error(&u->shared.stats, false); break;
        }
        if (!n) continue;
        const uint8_t *data; size_t len;
        int kind = fipsd_eth_dispatch_frame(u, u->node ? fipsd_node_secp(u->node) : NULL,
                                           frame, (size_t)n, mac, &data, &len);
        if (kind == 2) { if (drain_neighbors(u, s->stop)) break; continue; }
        if (kind != 1) continue;
        fips_transport_addr from = fips_transport_addr_from_mac(mac);
        if (fipsd_transport_emit(u->events, u->root, s->stop, FIPSD_TRANSPORT_PACKET,
                u->transport.id, &from, data, len)) break;
    }
    free(frame);
done:
    if (!go_channel_is_closed(s->stop)) fipsd_watcher_binding_exited(u->watcher, s->generation);
    go_channel_close(s->recv_done);
}
static void *open_socket(void *arg)
{
    eth_socket *s = arg;
    if (fipsd_eth_io_open(s, &s->owner->config)) s->error = errno;
    return NULL;
}
static void *close_socket(void *arg) { eth_socket *s = arg; close(s->fd); return NULL; }
static void free_socket(eth_socket *s)
{
    if (s->stop) go_channel_unref(s->stop);
    if (s->recv_done) go_channel_unref(s->recv_done);
    if (s->beacon_done) go_channel_unref(s->beacon_done);
    if (s->unused) go_channel_unref(s->unused);
    free(s->batch); free(s);
}
static void unbind_instance(void *arg, uint64_t generation)
{
    eth_instance *u = arg;
    nsync_mu_lock(&u->mu); eth_socket *s = u->socket;
    if (!s || s->generation != generation) { nsync_mu_unlock(&u->mu); return; }
    u->socket = NULL; s->detached = true;
    if (!s->users) go_channel_close(s->unused);
    nsync_mu_unlock(&u->mu);
    go_channel_close(s->stop);
    (void)gof_io_cancel_fd(s->fd);
    join(s->unused);
    join(s->recv_done); join(s->beacon_done);
    (void)gof_io_reset_fd(s->fd);
    go_blocking_submit(close_socket, s); free_socket(s);
    fipsd_eth_neighbors_clear(&u->neighbors);
}
static fipsd_bind_result bind_instance(void *arg, fipsd_runtime *rt, GoChannel *stop,
                                      const fipsd_presence_facts *facts, uint64_t generation)
{
    (void)rt; eth_instance *u = arg;
    if (!facts->has_mac || !facts->index || go_channel_is_closed(stop)) return FIPSD_BIND_ABSENT;
    eth_socket *s = calloc(1, sizeof *s); if (!s) return FIPSD_BIND_RETRY;
    s->owner = u; s->fd = -1; s->generation = generation; s->ethertype = u->config.ethertype;
    s->stop = go_channel_create(1); s->recv_done = go_channel_create(1);
    s->beacon_done = go_channel_create(1); s->unused = go_channel_create(1);
    if (!s->stop || !s->recv_done || !s->beacon_done || !s->unused) { free_socket(s); return FIPSD_BIND_RETRY; }
    go_blocking_submit(open_socket, s);
    if (s->fd < 0) {
        int error = s->error; free_socket(s);
        if (error == ENODEV || error == ENXIO) return FIPSD_BIND_ABSENT;
        FIPSD_ERROR("ethernet", "%s: raw socket/BPF open failed: %s%s", u->name, strerror(error),
            error == EPERM || error == EACCES ? "; requires CAP_NET_RAW/root (Linux) or /dev/bpf access (macOS)" : "");
        return FIPSD_BIND_RETRY;
    }
    if (gof_io_reset_fd(s->fd)) { go_blocking_submit(close_socket, s); free_socket(s); return FIPSD_BIND_RETRY; }
    nsync_mu_lock(&u->mu); u->socket = s; nsync_mu_unlock(&u->mu);
    bool receiver = !fipsd_runtime_spawn(u->runtime, receive_loop, s);
    bool beacon = u->config.announce && u->node && !fipsd_runtime_spawn(u->runtime, beacon_loop, s);
    if (!receiver) go_channel_close(s->recv_done);
    if (!beacon) go_channel_close(s->beacon_done);
    if (!receiver || (u->config.announce && !beacon)) { unbind_instance(u, generation); return FIPSD_BIND_RETRY; }
    FIPSD_INFO("ethernet", "%s: bound %s index %d, MTU %u", u->name, u->interface_name, s->index, s->mtu);
    return FIPSD_BIND_OK;
}
static bool binding_healthy(void *arg)
{
    eth_instance *u = arg;
    nsync_mu_lock(&u->mu); eth_socket *s = u->socket;
    bool ok = !s || fipsd_eth_io_bound_index(s) == s->index;
    nsync_mu_unlock(&u->mu); return ok;
}
static int start_async(fipsd_transport *t, fipsd_runtime *rt, const fipsd_node_context *node, GoChannel *events)
{
    eth_instance *u = t->user;
    u->runtime = rt; u->root = fipsd_runtime_context(rt); u->node = node; u->events = events;
    return fipsd_watcher_start(u->watcher, rt, node, events);
}
static void stop(fipsd_transport *t) { fipsd_watcher_stop(((eth_instance *)t->user)->watcher); }
static void network_changed(fipsd_transport *t, const struct fipsd_netmon_event *e)
{ fipsd_watcher_network_changed(((eth_instance *)t->user)->watcher, e); }
static uint16_t mtu(const fipsd_transport *t, const fips_transport_addr *to)
{
    (void)to; eth_instance *u = t->user; nsync_mu_lock(&u->mu);
    uint16_t n = u->socket ? u->socket->mtu : u->config.mtu ? u->config.mtu : 1497;
    nsync_mu_unlock(&u->mu); return n;
}
static bool local_addr(const fipsd_transport *t, fips_transport_addr *out)
{
    eth_instance *u = t->user; nsync_mu_lock(&u->mu); bool ok = u->socket != NULL;
    if (ok) *out = fips_transport_addr_from_mac(u->socket->mac);
    nsync_mu_unlock(&u->mu); return ok;
}
static int discover(fipsd_transport *t) { (void)t; return 0; } /* receive owner drains on beacon arrival */
static int send_frame(fipsd_transport *t, const fips_transport_addr *to, const uint8_t *p, size_t n)
{
    eth_instance *u = t->user;
    static const uint8_t zero[6] = {0};
    if (!to || to->len != 6 || fipsd_eth_group(to->bytes) || !memcmp(to->bytes, zero, 6) || !p) return -1;
    eth_socket *s = acquire(u); if (!s) return -1;
    int rc = -1;
    if (n > s->mtu) { atomic_fetch_add(&u->shared.stats.frames_too_long, 1); atomic_fetch_add(&u->shared.stats.mtu_exceeded, 1); }
    else {
        uint8_t *wire = malloc(n + 3);
        if (wire) { size_t len = fipsd_eth_data_encode(wire, n + 3, p, n); rc = transmit(s, to->bytes, wire, len); free(wire); }
    }
    release(s); return rc;
}
static const fipsd_transport_ops ops = {.start_async = start_async, .stop = stop,
    .state = fipsd_transport_shared_state, .mtu = mtu, .send = send_frame, .discover = discover,
    .network_changed = network_changed, .local_addr = local_addr};
void fipsd_ethernet_config_default(fipsd_ethernet_config *c)
{ *c = (fipsd_ethernet_config){.ethertype = FIPSD_ETHERTYPE, .recv_buf_size = 2097152, .send_buf_size = 2097152,
    .listen = true, .beacon_interval_secs = 30}; }
fipsd_transport *fipsd_ethernet_create(fips_transport_id id, const char *name, const fipsd_ethernet_config *c)
{
    if (!c || !name || strlen(name) >= 128 || !c->interface_name || !*c->interface_name ||
        strlen(c->interface_name) >= 16 || strchr(c->interface_name, '/') || !c->ethertype ||
        c->beacon_interval_secs > UINT64_MAX / 1000) return NULL;
    eth_instance *u = calloc(1, sizeof *u); if (!u) return NULL;
    nsync_mu_init(&u->mu); fipsd_eth_neighbors_init(&u->neighbors);
    strcpy(u->name, name); strcpy(u->interface_name, c->interface_name); u->config = *c;
    u->config.interface_name = u->interface_name;
    if (u->config.beacon_interval_secs < 10) u->config.beacon_interval_secs = 10;
    u->transport = (fipsd_transport){.ops = &ops, .id = id, .type = "ethernet", .name = u->name,
        .auto_connect = c->auto_connect, .accept_connections = c->accept_connections,
        .interface_name = u->interface_name, .user = u};
    fipsd_transport_shared_init(&u->transport, &u->shared, c->optional);
    fipsd_binder_ops binder = {.bind = bind_instance, .unbind = unbind_instance, .user = u};
    u->watcher = fipsd_watcher_create(&u->transport, &binder, c->watcher_source);
    if (!u->watcher) { free(u); return NULL; }
    fipsd_watcher_event_driven(u->watcher, binding_healthy);
    return &u->transport;
}
void fipsd_ethernet_destroy(fipsd_transport *t)
{ if (t) { eth_instance *u = t->user; fipsd_watcher_destroy(u->watcher); free(u); } }
int fipsd_ethernet_register_config(fipsd_transport_registry *r, const fipsd_config *cfg,
                                    fipsd_transport **owned, size_t *count)
{
    const fipsd_config_value *eth = fipsd_config_get(cfg, "transports.ethernet");
    if (!eth) return 0;
    bool single = fipsd_config_member(eth, "interface") != NULL;
    size_t n = single ? 1 : fipsd_config_count(eth);
    for (size_t i = 0; i < n; ++i) {
        if (*count == FIPSD_TRANSPORT_MAX) return -1;
        const char *name = single ? "ethernet" : fipsd_config_key(eth, i);
        const fipsd_config_value *v = single ? eth : fipsd_config_member(eth, name);
        fipsd_ethernet_config c; fipsd_ethernet_config_default(&c);
        c.interface_name = fipsd_config_string(fipsd_config_member(v, "interface"));
        c.ethertype = (uint16_t)fipsd_config_uint(fipsd_config_member(v, "ethertype"));
        c.mtu = (uint16_t)fipsd_config_uint(fipsd_config_member(v, "mtu"));
        c.recv_buf_size = (size_t)fipsd_config_uint(fipsd_config_member(v, "recv_buf_size"));
        c.send_buf_size = (size_t)fipsd_config_uint(fipsd_config_member(v, "send_buf_size"));
        c.listen = fipsd_config_bool(fipsd_config_member(v, "listen"));
        c.announce = fipsd_config_bool(fipsd_config_member(v, "announce"));
        c.auto_connect = fipsd_config_bool(fipsd_config_member(v, "auto_connect"));
        c.accept_connections = fipsd_config_bool(fipsd_config_member(v, "accept_connections"));
        c.optional = fipsd_config_bool(fipsd_config_member(v, "optional"));
        c.beacon_interval_secs = fipsd_config_uint(fipsd_config_member(v, "beacon_interval_secs"));
        fipsd_transport *t = fipsd_ethernet_create((fips_transport_id){(uint32_t)r->count + 1}, name, &c);
        if (!t) return -1;
        if (fipsd_transport_register(r, t)) { fipsd_ethernet_destroy(t); return -1; }
        owned[(*count)++] = t;
    }
    return 0;
}
