#define _POSIX_C_SOURCE 200809L
#include "transport/tor/internal.h"
#include "log.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#define MODULE "tor"
/* The advert latch: set once, by the monitor's view of the descriptor or, with
 * no monitor, by the hostname read alone (upstream advertises at start). */
static void advertisable(tor_instance *x, const char *how)
{
    if (atomic_exchange(&x->advertisable, true)) return;
    FIPSD_INFO(MODULE, "%s: onion %s advertisable (%s)", x->name, x->onion, how);
    (void)go_channel_try_send(x->advert, x);
}
static void descriptor(void *user, tor_descriptor_signal s)
{
    tor_instance *x = user;
    if (s == TOR_DESCRIPTOR_UNMONITORED && !atomic_load(&x->advertisable))
        FIPSD_WARN(MODULE, "%s: Tor control port %s gave no session to confirm the onion descriptor; advertising from the hostname file",
            x->name, x->control_addr);
    advertisable(x, s == TOR_DESCRIPTOR_UPLOADED ? "HS_DESC UPLOADED" : s == TOR_DESCRIPTOR_CURRENT ?
        "Tor holds a current descriptor" : "unconfirmed: control port unavailable");
}
static int start_async(fipsd_transport *t, fipsd_runtime *rt, const fipsd_node_context *node, GoChannel *events)
{
    tor_instance *x = t->user;
    if (x->started) return -1;
    x->started = true; x->runtime = rt; x->root = fipsd_runtime_context(rt);
    if (x->mode == TOR_MODE_DIRECTORY && tor_onion_read(x) != 0) return -1;
    size_t service = x->mode == TOR_MODE_DIRECTORY ? strlen(x->onion) - 6 : 0; /* without ".onion" */
    if (service && x->has_control && service < sizeof x->control.service) {
        memcpy(x->control.service, x->onion, service); x->control.service[service] = 0;
        x->control.descriptor = descriptor; x->control.descriptor_user = x;
    }
    /* control_port mode fails to start without its monitor, as upstream; a
     * directory-mode monitor is optional and keeps retrying in the background. */
    if (x->mode == TOR_MODE_CONTROL_PORT && tor_control_connect_now(&x->control, rt) != 0) {
        FIPSD_ERROR(MODULE, "%s: Tor control port %s unavailable", x->name, x->control_addr);
        return -1;
    }
    if (x->has_control && tor_control_start(&x->control, rt) != 0) { tor_control_close(&x->control); return -1; }
    int rc = x->tcp->ops->start_async(x->tcp, rt, node, events);
    if (rc) { tor_control_stop(&x->control); return rc; }
    if (service && !x->control.descriptor) advertisable(x, "hostname file; no control monitor");
    FIPSD_INFO(MODULE, "%s: Tor transport started (mode=%s socks5=%s onion=%s mtu=%u)", x->name,
        tor_mode_name(x->mode), x->socks5_addr, atomic_load(&x->has_onion) ? x->onion : "none", (unsigned)x->config.mtu);
    return 0;
}
static void stop(fipsd_transport *t)
{
    tor_instance *x = t->user;
    if (x->has_control) tor_control_stop(&x->control);
    x->tcp->ops->stop(x->tcp);
}
static uint16_t mtu(const fipsd_transport *t, const fips_transport_addr *to)
{ (void)to; return ((const tor_instance *)t->user)->config.mtu; }
static int discover(fipsd_transport *t) { (void)t; return 0; }
static int connect_async(fipsd_transport *t, const fips_transport_addr *to)
{
    tor_instance *x = t->user; fipsd_socks5_target target;
    if (!tor_target(to, &target)) return -1; /* upstream InvalidAddress, before any dial */
    return x->tcp->ops->connect_async(x->tcp, to);
}
static int send_frame(fipsd_transport *t, const fips_transport_addr *to, const uint8_t *bytes, size_t n)
{ tor_instance *x = t->user; return x->tcp->ops->send(x->tcp, to, bytes, n); }
/* Upstream TorTransport::send_lane: the inner stream owner's control and data
 * lanes (socks5/pool.rs ProxiedConnection::queue, PATH_QUEUE_DEPTH 8). */
static int send_lane(fipsd_transport *t, fipsd_transport_lane lane, const fips_transport_addr *to,
    const uint8_t *bytes, size_t n)
{ tor_instance *x = t->user; return fipsd_transport_send_lane(x->tcp, lane, to, bytes, n); }
static int lane_admit(fipsd_transport *t, fipsd_transport_lane lane, const fips_transport_addr *to)
{ tor_instance *x = t->user; return fipsd_transport_lane_admit(x->tcp, lane, to); }
static void close_connection(fipsd_transport *t, const fips_transport_addr *to)
{ tor_instance *x = t->user; x->tcp->ops->close_connection(x->tcp, to); }
static void network_changed(fipsd_transport *t, const struct fipsd_netmon_event *e)
{ tor_instance *x = t->user; x->tcp->ops->network_changed(x->tcp, e); }
/* Upstream reports no local address for Tor; the onion is a separate row field. */
static bool local_addr(const fipsd_transport *t, fips_transport_addr *out) { (void)t; (void)out; return false; }
static const fipsd_transport_ops ops = {.local_addr = local_addr, .start_async = start_async, .stop = stop,
    .state = fipsd_transport_shared_state, .mtu = mtu, .send = send_frame, .discover = discover,
    .connect_async = connect_async, .close_connection = close_connection, .network_changed = network_changed,
    .send_lane = send_lane, .lane_admit = lane_admit};
void fipsd_tor_config_default(fipsd_tor_config *c)
{
    *c = (fipsd_tor_config){.mode = "socks5", .socks5_addr = "127.0.0.1:9050", .connect_timeout_ms = 120000,
        .mtu = 1400, .control_auth = "cookie", .cookie_path = "/var/run/tor/control.authcookie",
        .max_inbound_connections = 64, .hostname_file = "/var/lib/tor/fips_onion_service/hostname",
        .bind_addr = "127.0.0.1:8443", .advertised_port = 443, .max_outbound_connections = 64,
        .send_queue_depth = FIPSD_TCP_SEND_DEPTH, .first_frame_ms = 30000, .idle_ms = 64000,
        .control_backoff_ms = 1000, .control_stall_ms = 60000};
}
static bool copy_text(char *dst, size_t cap, const char *src)
{ if (!src || !*src || strlen(src) >= cap) return false; strcpy(dst, src); return true; }
fipsd_transport *fipsd_tor_create(fips_transport_id id, const char *name, const fipsd_tor_config *c)
{
    tor_mode mode;
    if (!name || !*name || strlen(name) >= 128 || !c || !tor_mode_parse(c->mode, &mode) || !c->mtu ||
        !c->connect_timeout_ms || c->connect_timeout_ms > INT_MAX || !tor_host_port(c->socks5_addr) ||
        strlen(c->socks5_addr) > 256) return NULL;
    if (mode == TOR_MODE_CONTROL_PORT && !c->control_addr) return NULL;
    if (c->control_addr && (!*c->control_addr || strlen(c->control_addr) > 256 ||
        (c->control_addr[0] != '/' && strncmp(c->control_addr, "./", 2) && !tor_host_port(c->control_addr)))) return NULL;
    tor_instance *x = calloc(1, sizeof *x);
    if (!x) return NULL;
    x->mode = mode; x->config = *c; atomic_init(&x->has_onion, false);
    strcpy(x->name, name); strcpy(x->socks5_addr, c->socks5_addr);
    x->config.mode = tor_mode_name(mode); x->config.socks5_addr = x->socks5_addr;
    if (mode == TOR_MODE_DIRECTORY) {
        if (!copy_text(x->hostname_file, sizeof x->hostname_file, c->hostname_file) ||
            !copy_text(x->bind_addr, sizeof x->bind_addr, c->bind_addr)) { free(x); return NULL; }
        x->config.hostname_file = x->hostname_file; x->config.bind_addr = x->bind_addr;
    } else x->config.hostname_file = x->config.bind_addr = NULL;
    if (fips_transport_addr_from_string(x->socks5_addr, strlen(x->socks5_addr), &x->proxy) != FIPS_OK) { free(x); return NULL; }
    x->dialer = (fipsd_stream_dialer){.via = &x->proxy, .upgrade = tor_upgrade, .user = x};
    fipsd_tcp_config tc; fipsd_tcp_config_default(&tc);
    tc.bind_addr = mode == TOR_MODE_DIRECTORY ? x->bind_addr : NULL;
    tc.mtu = c->mtu; tc.connect_timeout_ms = c->connect_timeout_ms;
    /* Every onion forward arrives from the local Tor daemon: one source. */
    tc.max_inbound_connections = tc.max_inbound_per_source = c->max_inbound_connections;
    tc.max_outbound_connections = c->max_outbound_connections; tc.send_queue_depth = c->send_queue_depth;
    tc.first_frame_ms = c->first_frame_ms; tc.idle_ms = c->idle_ms; tc.watcher_source = c->watcher_source;
    tc.dialer = &x->dialer;
    x->tcp = fipsd_tcp_create(id, name, &tc);
    if (!x->tcp) { free(x); return NULL; }
    x->config.control_addr = x->config.control_auth = x->config.cookie_path = NULL;
    /* socks5 mode never opens the control port upstream, whatever is set. */
    if (c->control_addr && mode != TOR_MODE_SOCKS5) {
        strcpy(x->control_addr, c->control_addr);
        if (c->control_auth) snprintf(x->control_auth, sizeof x->control_auth, "%s", c->control_auth);
        if (c->cookie_path) snprintf(x->cookie_path, sizeof x->cookie_path, "%s", c->cookie_path);
        if (tor_control_init(&x->control, x->name, &x->tcp->shared->stats, x->control_addr,
                c->control_auth, c->cookie_path, c->control_backoff_ms, c->control_stall_ms) == 0) {
            x->has_control = true; x->config.control_addr = x->control_addr;
            x->config.control_auth = x->control_auth; x->config.cookie_path = x->cookie_path;
        } else if (mode == TOR_MODE_CONTROL_PORT) { fipsd_tcp_destroy(x->tcp); free(x); return NULL; }
        else FIPSD_WARN(MODULE, "%s: Tor control auth config error ('%s'); monitoring disabled", name,
            c->control_auth ? c->control_auth : "");
    }
    atomic_init(&x->advertisable, false);
    if (!(x->advert = go_channel_create(1))) {
        if (x->has_control) tor_control_destroy(&x->control);
        fipsd_tcp_destroy(x->tcp); free(x); return NULL;
    }
    x->transport = (fipsd_transport){.ops = &ops, .id = id, .type = "tor", .name = x->name,
        .auto_connect = true, .accept_connections = mode == TOR_MODE_DIRECTORY, .user = x, .shared = x->tcp->shared};
    return &x->transport;
}
void fipsd_tor_destroy(fipsd_transport *t)
{
    if (!t) return;
    tor_instance *x = t->user;
    if (x->has_control) tor_control_destroy(&x->control);
    fipsd_tcp_destroy(x->tcp);
    go_channel_unref(x->advert);
    free(x);
}
bool fipsd_tor_is(const fipsd_transport *t) { return t && t->ops == &ops; }
const char *fipsd_tor_mode(const fipsd_transport *t)
{ return fipsd_tor_is(t) ? tor_mode_name(((const tor_instance *)t->user)->mode) : NULL; }
const char *fipsd_tor_onion_address(const fipsd_transport *t)
{
    if (!fipsd_tor_is(t)) return NULL;
    const tor_instance *x = t->user;
    return atomic_load_explicit(&x->has_onion, memory_order_acquire) ? x->onion : NULL;
}
const char *fipsd_tor_advertised_onion(const fipsd_transport *t)
{
    if (!fipsd_tor_is(t)) return NULL;
    tor_instance *x = t->user;
    return atomic_load(&x->advertisable) && atomic_load_explicit(&x->has_onion, memory_order_acquire) ? x->onion : NULL;
}
uint16_t fipsd_tor_advertised_port(const fipsd_transport *t)
{ return fipsd_tor_is(t) ? ((const tor_instance *)t->user)->config.advertised_port : 0; }
GoChannel *fipsd_tor_advert_updates(fipsd_transport *t)
{ return fipsd_tor_is(t) ? ((tor_instance *)t->user)->advert : NULL; }
bool fipsd_tor_monitoring_snapshot(const fipsd_transport *t, fipsd_tor_monitoring *out)
{
    if (!fipsd_tor_is(t) || !out) return false;
    tor_instance *x = t->user;
    return x->has_control && tor_control_snapshot(&x->control, out);
}
json_t *fipsd_tor_monitoring_json(const fipsd_tor_monitoring *m)
{
    if (!m) return NULL;
    return json_pack("{s:i,s:b,s:I,s:I,s:s,s:s,s:b}", "bootstrap", (int)m->bootstrap,
        "circuit_established", (int)m->circuit_established, "traffic_read", (json_int_t)m->traffic_read,
        "traffic_written", (json_int_t)m->traffic_written, "network_liveness", m->network_liveness,
        "version", m->version, "dormant", (int)m->dormant);
}
GoChannel *fipsd_tor_bound(fipsd_transport *t) { return fipsd_tcp_bound(((tor_instance *)t->user)->tcp); }
GoChannel *fipsd_tor_monitoring_updates(fipsd_transport *t)
{ tor_instance *x = t->user; return x->has_control ? x->control.updates : NULL; }
bool fipsd_tor_listener_address(fipsd_transport *t, fips_transport_addr *out)
{ return fipsd_tcp_address(((tor_instance *)t->user)->tcp, out); }
size_t fipsd_tor_connected_count(fipsd_transport *t)
{ return fipsd_tcp_connected_count(((tor_instance *)t->user)->tcp); }
fipsd_transport *fipsd_tor_stream(fipsd_transport *t) { return ((tor_instance *)t->user)->tcp; }
int fipsd_tor_register_config(fipsd_transport_registry *r, const fipsd_config *config,
    fipsd_transport **owned, size_t *count)
{
    const fipsd_config_value *tor = fipsd_config_get(config, "transports.tor");
    if (!tor) return 0;
    bool single = fipsd_config_member(tor, "mode") != NULL;
    size_t n = single ? 1 : fipsd_config_count(tor);
    for (size_t i = 0; i < n; ++i) {
        if (*count == FIPSD_TRANSPORT_MAX) return -1;
        const char *name = single ? "tor" : fipsd_config_key(tor, i);
        const fipsd_config_value *v = single ? tor : fipsd_config_member(tor, name);
        fipsd_tor_config c; fipsd_tor_config_default(&c);
        c.mode = fipsd_config_string(fipsd_config_member(v, "mode"));
        c.socks5_addr = fipsd_config_string(fipsd_config_member(v, "socks5_addr"));
        uint64_t timeout = fipsd_config_uint(fipsd_config_member(v, "connect_timeout_ms"));
        if (timeout > INT_MAX) return -1;
        c.connect_timeout_ms = (uint32_t)timeout;
        c.mtu = (uint16_t)fipsd_config_uint(fipsd_config_member(v, "mtu"));
        c.control_addr = fipsd_config_string(fipsd_config_member(v, "control_addr"));
        if (!c.control_addr && c.mode && !strcmp(c.mode, "control_port")) c.control_addr = "/run/tor/control";
        c.control_auth = fipsd_config_string(fipsd_config_member(v, "control_auth"));
        c.cookie_path = fipsd_config_string(fipsd_config_member(v, "cookie_path"));
        c.max_inbound_connections = (size_t)fipsd_config_uint(fipsd_config_member(v, "max_inbound_connections"));
        c.advertised_port = (uint16_t)fipsd_config_uint(fipsd_config_member(v, "advertised_port"));
        const fipsd_config_value *directory = fipsd_config_member(v, "directory_service");
        if (directory) {
            c.hostname_file = fipsd_config_string(fipsd_config_member(directory, "hostname_file"));
            c.bind_addr = fipsd_config_string(fipsd_config_member(directory, "bind_addr"));
            if (c.mode && !strcmp(c.mode, "socks5")) {
                FIPSD_ERROR(MODULE, "%s: directory_service config requires mode 'directory', not 'socks5'", name);
                return -1;
            }
        }
        fipsd_config_core core; fipsd_config_core_params(config, &core);
        fips_duration hold = core.link_rekey.hold;
        c.idle_ms = hold.secs > (UINT64_MAX - hold.nanos / 1000000) / 1000 ? UINT64_MAX :
            hold.secs * 1000 + hold.nanos / 1000000;
        c.max_outbound_connections = (size_t)fipsd_config_uint(fipsd_config_get(config, "node.limits.max_connections"));
        fipsd_transport *t = fipsd_tor_create((fips_transport_id){(uint32_t)r->count + 1}, name, &c);
        if (!t) {
            FIPSD_ERROR(MODULE, "%s: invalid Tor transport configuration (mode=%s socks5_addr=%s)", name,
                c.mode ? c.mode : "?", c.socks5_addr ? c.socks5_addr : "?");
            return -1;
        }
        if (fipsd_transport_register(r, t)) { fipsd_tor_destroy(t); return -1; }
        owned[(*count)++] = t;
    }
    return 0;
}
