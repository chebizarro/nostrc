#define _POSIX_C_SOURCE 200809L
#include "transport/nym/nym.h"
#include "transport/tcp/internal.h"
#include "transport/socks5/socks5.h"
#include "select.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define MODULE "nym"
#define PROBE_WAIT_MS 5000
typedef struct nym_instance {
    fipsd_transport transport;
    fipsd_transport *tcp;
    fipsd_nym_config config;
    char name[128], socks5_addr[257];
    fips_transport_addr proxy;
    fipsd_stream_dialer dialer;
    fipsd_runtime *runtime;
    GoContext *root;
    const fipsd_node_context *node;
    GoChannel *events, *stop, *ready;
    /* stream_up publishes the inner owner's deferred start: every op reads it
     * (acquire) before touching the owner, which the readiness fiber started. */
    _Atomic bool proxy_ready, stream_up;
    bool started;
} nym_instance;
#ifndef FIPSD_ENABLE_NYM
#error "nym.c is compiled only with FIPSD_ENABLE_NYM"
#endif
static bool stopped(nym_instance *x) { return go_channel_is_closed(x->stop) || go_context_is_canceled(x->root); }
static int upgrade(void *user, int fd, const fips_transport_addr *peer, uint64_t deadline, int *error)
{
    nym_instance *x = user; fipsd_socks5_target target; const char *text; size_t len;
    if (fips_transport_addr_as_str(peer, &text, &len) != FIPS_OK || !fipsd_socks5_target_parse(text, len, false, &target)) {
        *error = EINVAL; return -1;
    }
    fipsd_socks5_auth none = {0}; uint8_t rep = 0;
    fipsd_socks5_result r = fipsd_socks5_handshake(fd, &none, &target, deadline, &rep);
    if (r == FIPSD_SOCKS5_OK) return 0;
    /* Nym keeps no connect_refused counter: every SOCKS5 failure, a REP 0x05
     * refusal included, is a socks5_error upstream. */
    *error = r == FIPSD_SOCKS5_TIMEOUT || r == FIPSD_SOCKS5_CANCELLED ? fipsd_socks5_errno(r) : EPROTO;
    if (r != FIPSD_SOCKS5_CANCELLED)
        FIPSD_DEBUG(MODULE, "%s: SOCKS5 connect to %.*s failed: %s (rep=%u)", x->name, (int)len, text,
            fipsd_socks5_result_name(r), (unsigned)rep);
    return -1;
}
/* One TCP connect to the client's port; the socket is dropped either way. */
static bool probe(nym_instance *x, uint64_t wait_ms)
{
    tcp_endpoint *items = NULL; size_t n = 0;
    if (!tcp_resolve(&x->proxy, false, &items, &n)) return false;
    int fd = socket(items[0].address.ss_family, SOCK_STREAM, IPPROTO_TCP);
    bool ok = false;
    if (fd >= 0 && !fcntl(fd, F_SETFL, O_NONBLOCK) && !fcntl(fd, F_SETFD, FD_CLOEXEC) && !gof_io_reset_fd(fd))
        ok = gof_connect(fd, (const struct sockaddr *)&items[0].address, items[0].len,
            (int)(wait_ms > INT_MAX ? INT_MAX : wait_ms)) == 0;
    if (fd >= 0) tcp_fd_close(fd);
    free(items); return ok;
}
/* upstream wait_for_socks5_ready: probe, back off 1 s doubling to 10 s, give
 * up after startup_timeout_secs; the stream owner starts either way. */
static void readiness(void *arg)
{
    nym_instance *x = arg;
    uint64_t began = fipsd_transport_now_ms(), limit = x->config.startup_timeout_secs * 1000, delay = 1000;
    FIPSD_INFO(MODULE, "%s: waiting for Nym SOCKS5 client at %s to become ready (timeout %llu s)", x->name,
        x->socks5_addr, (unsigned long long)x->config.startup_timeout_secs);
    while (!stopped(x)) {
        uint64_t elapsed = fipsd_transport_now_ms() - began;
        if (probe(x, limit > elapsed ? (limit - elapsed < PROBE_WAIT_MS ? limit - elapsed + 1 : PROBE_WAIT_MS) : PROBE_WAIT_MS)) {
            atomic_store(&x->proxy_ready, true);
            FIPSD_INFO(MODULE, "%s: Nym SOCKS5 client is ready (%llu s)", x->name, (unsigned long long)(elapsed / 1000));
            break;
        }
        elapsed = fipsd_transport_now_ms() - began;
        if (elapsed >= limit) {
            FIPSD_WARN(MODULE, "%s: Nym SOCKS5 client at %s not reachable after %llu s; starting anyway (connections fail until it is)",
                x->name, x->socks5_addr, (unsigned long long)(elapsed / 1000));
            break;
        }
        uint64_t wait = delay < limit - elapsed ? delay : limit - elapsed;
        void *v = NULL;
        GoSelectCase cases[] = {{GO_SELECT_RECEIVE, x->stop, NULL, &v}, {GO_SELECT_RECEIVE, go_context_done(x->root), NULL, &v}};
        if (go_select_timeout(cases, 2, wait).selected_case >= 0) break;
        delay = delay * 2 > 10000 ? 10000 : delay * 2;
    }
    if (!stopped(x) && x->tcp->ops->start_async(x->tcp, x->runtime, x->node, x->events) == 0) {
        atomic_store_explicit(&x->stream_up, true, memory_order_release);
        FIPSD_INFO(MODULE, "%s: Nym mixnet transport started (socks5=%s mtu=%u)", x->name, x->socks5_addr, (unsigned)x->config.mtu);
    } else if (!stopped(x)) FIPSD_ERROR(MODULE, "%s: Nym stream owner failed to start", x->name);
    go_channel_close(x->ready);
}
static int start_async(fipsd_transport *t, fipsd_runtime *rt, const fipsd_node_context *node, GoChannel *events)
{
    nym_instance *x = t->user;
    if (x->started) return -1;
    x->started = true; x->runtime = rt; x->root = fipsd_runtime_context(rt); x->node = node; x->events = events;
    /* Nonblocking start: the readiness wait runs on its own fiber. */
    return fipsd_runtime_spawn(rt, readiness, x);
}
static void stop(fipsd_transport *t)
{ nym_instance *x = t->user; go_channel_close(x->stop); x->tcp->ops->stop(x->tcp); }
static uint16_t mtu(const fipsd_transport *t, const fips_transport_addr *to)
{ (void)to; return ((const nym_instance *)t->user)->config.mtu; }
static int discover(fipsd_transport *t) { (void)t; return 0; }
static bool up(nym_instance *x) { return atomic_load_explicit(&x->stream_up, memory_order_acquire); }
/* Until the readiness wait has started the stream owner, Nym is NotStarted
 * upstream (state Starting): connects and sends fail without dialing. */
static int connect_async(fipsd_transport *t, const fips_transport_addr *to)
{
    nym_instance *x = t->user; fipsd_socks5_target target; const char *text; size_t len;
    if (!to || fips_transport_addr_as_str(to, &text, &len) != FIPS_OK || !fipsd_socks5_target_parse(text, len, false, &target)) return -1;
    return up(x) ? x->tcp->ops->connect_async(x->tcp, to) : -1;
}
static int send_frame(fipsd_transport *t, const fips_transport_addr *to, const uint8_t *bytes, size_t n)
{ nym_instance *x = t->user; return up(x) ? x->tcp->ops->send(x->tcp, to, bytes, n) : -1; }
static void close_connection(fipsd_transport *t, const fips_transport_addr *to)
{ nym_instance *x = t->user; if (up(x)) x->tcp->ops->close_connection(x->tcp, to); }
static void network_changed(fipsd_transport *t, const struct fipsd_netmon_event *e)
{ nym_instance *x = t->user; if (up(x)) x->tcp->ops->network_changed(x->tcp, e); }
static bool local_addr(const fipsd_transport *t, fips_transport_addr *out) { (void)t; (void)out; return false; }
static const fipsd_transport_ops ops = {.local_addr = local_addr, .start_async = start_async, .stop = stop,
    .state = fipsd_transport_shared_state, .mtu = mtu, .send = send_frame, .discover = discover,
    .connect_async = connect_async, .close_connection = close_connection, .network_changed = network_changed};
void fipsd_nym_config_default(fipsd_nym_config *c)
{
    *c = (fipsd_nym_config){.socks5_addr = "127.0.0.1:1080", .connect_timeout_ms = 300000, .mtu = 1400,
        .startup_timeout_secs = 120, .max_outbound_connections = 64, .send_queue_depth = FIPSD_TCP_SEND_DEPTH};
}
fipsd_transport *fipsd_nym_create(fips_transport_id id, const char *name, const fipsd_nym_config *c)
{
    fipsd_socks5_target t;
    if (!name || !*name || strlen(name) >= 128 || !c || !c->socks5_addr || strlen(c->socks5_addr) > 256 ||
        !fipsd_socks5_target_parse(c->socks5_addr, strlen(c->socks5_addr), false, &t) || !c->mtu ||
        !c->connect_timeout_ms || c->connect_timeout_ms > INT_MAX ||
        c->startup_timeout_secs > UINT64_MAX / 1000) return NULL;
    nym_instance *x = calloc(1, sizeof *x);
    if (!x) return NULL;
    x->transport.user = x;
    x->config = *c; strcpy(x->name, name); strcpy(x->socks5_addr, c->socks5_addr); x->config.socks5_addr = x->socks5_addr;
    atomic_init(&x->proxy_ready, false); atomic_init(&x->stream_up, false);
    if (fips_transport_addr_from_string(x->socks5_addr, strlen(x->socks5_addr), &x->proxy) != FIPS_OK) { fipsd_nym_destroy(&x->transport); return NULL; }
    x->dialer = (fipsd_stream_dialer){.via = &x->proxy, .upgrade = upgrade, .user = x};
    fipsd_tcp_config tc; fipsd_tcp_config_default(&tc);
    tc.mtu = c->mtu; tc.connect_timeout_ms = c->connect_timeout_ms;
    tc.max_outbound_connections = c->max_outbound_connections; tc.send_queue_depth = c->send_queue_depth;
    tc.watcher_source = c->watcher_source; tc.dialer = &x->dialer;
    x->tcp = fipsd_tcp_create(id, name, &tc);
    x->stop = go_channel_create(1); x->ready = go_channel_create(1);
    if (!x->tcp || !x->stop || !x->ready) { fipsd_nym_destroy(&x->transport); return NULL; }
    x->transport = (fipsd_transport){.ops = &ops, .id = id, .type = "nym", .name = x->name,
        .auto_connect = true, .accept_connections = false, .user = x, .shared = x->tcp->shared};
    return &x->transport;
}
void fipsd_nym_destroy(fipsd_transport *t)
{
    if (!t) return;
    nym_instance *x = t->user;
    if (x->tcp) fipsd_tcp_destroy(x->tcp);
    if (x->stop) go_channel_unref(x->stop);
    if (x->ready) go_channel_unref(x->ready);
    free(x);
}
bool fipsd_nym_is(const fipsd_transport *t) { return t && t->ops == &ops; }
GoChannel *fipsd_nym_bound(fipsd_transport *t) { return fipsd_tcp_bound(((nym_instance *)t->user)->tcp); }
GoChannel *fipsd_nym_ready(fipsd_transport *t) { return ((nym_instance *)t->user)->ready; }
bool fipsd_nym_proxy_ready(const fipsd_transport *t) { return atomic_load(&((const nym_instance *)t->user)->proxy_ready); }
size_t fipsd_nym_connected_count(fipsd_transport *t) { return fipsd_tcp_connected_count(((nym_instance *)t->user)->tcp); }
fipsd_transport *fipsd_nym_stream(fipsd_transport *t) { return ((nym_instance *)t->user)->tcp; }
int fipsd_nym_register_config(fipsd_transport_registry *r, const fipsd_config *config,
    fipsd_transport **owned, size_t *count)
{
    const fipsd_config_value *nym = fipsd_config_get(config, "transports.nym");
    if (!nym) return 0;
    bool single = fipsd_config_member(nym, "mtu") != NULL;
    size_t n = single ? 1 : fipsd_config_count(nym);
    for (size_t i = 0; i < n; ++i) {
        if (*count == FIPSD_TRANSPORT_MAX) return -1;
        const char *name = single ? "nym" : fipsd_config_key(nym, i);
        const fipsd_config_value *v = single ? nym : fipsd_config_member(nym, name);
        fipsd_nym_config c; fipsd_nym_config_default(&c);
        c.socks5_addr = fipsd_config_string(fipsd_config_member(v, "socks5_addr"));
        uint64_t timeout = fipsd_config_uint(fipsd_config_member(v, "connect_timeout_ms"));
        if (timeout > INT_MAX) return -1;
        c.connect_timeout_ms = (uint32_t)timeout;
        c.mtu = (uint16_t)fipsd_config_uint(fipsd_config_member(v, "mtu"));
        c.startup_timeout_secs = fipsd_config_uint(fipsd_config_member(v, "startup_timeout_secs"));
        c.max_outbound_connections = (size_t)fipsd_config_uint(fipsd_config_get(config, "node.limits.max_connections"));
        fipsd_transport *t = fipsd_nym_create((fips_transport_id){(uint32_t)r->count + 1}, name, &c);
        if (!t) { FIPSD_ERROR(MODULE, "%s: invalid Nym transport configuration", name); return -1; }
        if (fipsd_transport_register(r, t)) { fipsd_nym_destroy(t); return -1; }
        owned[(*count)++] = t;
    }
    return 0;
}
