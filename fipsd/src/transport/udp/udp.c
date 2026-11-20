#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* struct mmsghdr, sendmmsg */
#endif
#define _POSIX_C_SOURCE 200809L
#include "transport/udp/internal.h"
#include "libgo/fiber.h"
#include "log.h"
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
    udp_mu_lock(&u->mu); u->listener = s; udp_mu_unlock(&u->mu);
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
    udp_mu_lock(&u->mu);
    if (!u->listener || u->listener->generation != generation) { udp_mu_unlock(&u->mu); gate_unlock(u); return; }
    listener = u->listener; u->listener = NULL;
    memcpy(peers, u->peers, sizeof peers); memset(u->peers, 0, sizeof u->peers); memset(u->routes, 0, sizeof u->routes);
    udp_mu_unlock(&u->mu);
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
/* The socket that sends to `to` (its connected peer socket, else the
 * listener), leased, with the numeric target and the listener's source hint.
 * NULL when nothing can send there; *unresolved when `to` did not resolve,
 * which a send counts as a send error. */
static udp_socket *lease(udp_instance *u, const fips_transport_addr *to, fipsd_udp_endpoint *target,
    fipsd_udp_packet_info *source, bool *unresolved)
{
    *unresolved = !fipsd_udp_resolve(&u->dns, to, u->bind_to.address.ss_family, target);
    if (*unresolved) return NULL;
    fips_transport_addr numeric;
    if (!fipsd_udp_endpoint_addr(target, &numeric)) return NULL;
    *source = (fipsd_udp_packet_info){0};
    udp_mu_lock(&u->mu);
    udp_socket *s = u->listener;
    for (size_t i = 0; i < u->config.peer_capacity; ++i)
        if (u->peers[i] && !go_channel_is_closed(u->peers[i]->done) &&
            fips_transport_addr_equal(&u->peers[i]->peer, &numeric)) { s = u->peers[i]; break; }
    for (size_t i = 0; i < FIPSD_UDP_MAX_PEERS; ++i)
        if (u->routes[i].used && fips_transport_addr_equal(&u->routes[i].peer, &numeric)) { *source = u->routes[i].info; break; }
    if (s) ++s->users;
    udp_mu_unlock(&u->mu);
    return s;
}
static int send_frame(fipsd_transport *t, const fips_transport_addr *to, const uint8_t *bytes, size_t len)
{
    udp_instance *u = t->user;
    if (!u->started || !bytes || !len || go_context_is_canceled(u->root)) return -1;
    if (len > u->config.mtu) { atomic_fetch_add(&u->shared.stats.mtu_exceeded, 1); return -1; }
    fipsd_udp_endpoint target; fipsd_udp_packet_info source; bool unresolved;
    udp_socket *s = lease(u, to, &target, &source, &unresolved);
    if (!s) { if (unresolved) fipsd_transport_stats_error(&u->shared.stats, true); return -1; }
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
    FIPSD_PERF_BEGIN(send_start);
    ssize_t n = gof_sendmsg(s->fd, &m, 0, 0);
    FIPSD_PERF_END(send_start, UDP_SEND);
    FIPSD_PERF_EVENT(UDP_SEND_CONNECTED, n == (ssize_t)len && s->connected ? 1 : 0);
    FIPSD_PERF_EVENT(UDP_SEND_WILDCARD, n == (ssize_t)len && !s->connected ? 1 : 0);
    fipsd_udp_socket_release(s);
    if (n != (ssize_t)len) { fipsd_transport_stats_error(&u->shared.stats, true); return -1; }
    fipsd_transport_stats_sent(&u->shared.stats, len); return 0;
}
#if defined(__linux__)
/* Batched sends (nostrc-u19cz.4, upstream encrypt_worker.rs flush_batch_sync):
 * one sendmmsg per run of frames to one destination. A run of equal-size
 * datagrams, of which only the last may be shorter, rides one message with
 * UDP_SEGMENT, which the kernel splits back into those datagrams (GSO) in a
 * single stack walk. A message the kernel did not take is resubmitted alone
 * to learn its error; a plain one fails only its own frame, and the rest are
 * sent after it, in order. A GSO message that fails is resent, with the rest
 * of the run, as plain messages, so its frames meet what single sends would
 * (backpressure takes what fits, a pending socket error fails one send). If
 * the kernel refused GSO itself (EIO: the route has no checksum offload;
 * EINVAL, EOPNOTSUPP, ENOPROTOOPT, EMSGSIZE), the socket stops using it. */
static int sendmmsg_now(gof_socket_t fd, struct mmsghdr *m, unsigned n, int flags)
{ return gof_sendmmsg(fd, m, n, flags, 0); /* never parks: backpressure is a send failure */ }
int (*fipsd_udp_sendmmsg)(gof_socket_t, struct mmsghdr *, unsigned, int) = sendmmsg_now;
static bool gso_refused(int error)
{ return error == EIO || error == EINVAL || error == EOPNOTSUPP || error == ENOPROTOOPT || error == EMSGSIZE; }
typedef struct tx_run {
    struct mmsghdr m[FIPSD_UDP_SEND_BATCH];
    struct iovec iov[FIPSD_UDP_SEND_BATCH];
    struct { _Alignas(struct cmsghdr) uint8_t data[128]; } control[FIPSD_UDP_SEND_BATCH];
    size_t frame[FIPSD_UDP_SEND_BATCH]; /* iov[k] carries frame frame[k] of the run */
    size_t first[FIPSD_UDP_SEND_BATCH], count[FIPSD_UDP_SEND_BATCH]; /* message -> iov range */
    size_t frames, messages;
} tx_run;
/* Messages for iov entries from `at` on, replacing those from message `msg`. */
static void tx_build(tx_run *r, const udp_socket *s, const fipsd_udp_endpoint *target,
    const fipsd_udp_packet_info *source, size_t msg, size_t at, bool gso)
{
    r->messages = msg;
    for (size_t p = at; p < r->frames;) {
        size_t size = r->iov[p].iov_len, q = p + 1;
        if (gso) while (q < r->frames && q - p < FIPSD_UDP_GSO_SEGMENTS && (q - p + 1) * size <= FIPSD_UDP_GSO_BYTES) {
            size_t next = r->iov[q].iov_len;
            if (next > size) break;
            ++q;
            if (next < size) break; /* only the last segment may be shorter */
        }
        size_t k = r->messages++;
        r->first[k] = p; r->count[k] = q - p;
        struct msghdr *h = &r->m[k].msg_hdr;
        *h = (struct msghdr){.msg_iov = &r->iov[p], .msg_iovlen = q - p};
        r->m[k].msg_len = 0;
        size_t control = 0;
        if (!s->connected) {
            h->msg_name = (void *)&target->address; h->msg_namelen = target->len;
            control = fipsd_udp_source_control(r->control[k].data, sizeof r->control[k].data, source);
        }
        if (q - p > 1) {
            struct cmsghdr *c = (struct cmsghdr *)(r->control[k].data + control);
            uint16_t segment = (uint16_t)size;
            memset(c, 0, CMSG_SPACE(sizeof segment));
            c->cmsg_level = SOL_UDP; c->cmsg_type = UDP_SEGMENT; c->cmsg_len = CMSG_LEN(sizeof segment);
            memcpy(CMSG_DATA(c), &segment, sizeof segment);
            control += CMSG_SPACE(sizeof segment);
        }
        if (control) { h->msg_control = r->control[k].data; h->msg_controllen = control; }
        p = q;
    }
}
/* Frames f[0..n) to one destination through s, n <= FIPSD_UDP_SEND_BATCH. */
static size_t send_run(udp_instance *u, udp_socket *s, const fipsd_udp_endpoint *target,
    const fipsd_udp_packet_info *source, const fipsd_transport_frame *f, size_t n, int *rc)
{
    tx_run r; r.frames = 0;
    for (size_t i = 0; i < n; ++i) {
        if (rc[i]) continue; /* failed its own checks */
        r.iov[r.frames] = (struct iovec){.iov_base = (void *)f[i].bytes, .iov_len = f[i].len};
        r.frame[r.frames++] = i;
    }
    tx_build(&r, s, target, source, 0, 0, atomic_load_explicit(&s->gso, memory_order_relaxed));
    size_t sent = 0;
    FIPSD_PERF_BEGIN(send_start);
    for (size_t at = 0; at < r.messages;) {
        int k = fipsd_udp_sendmmsg(s->fd, &r.m[at], (unsigned)(r.messages - at), 0);
        if (k > 0) {
            size_t frames = 0;
            for (size_t j = at; j < at + (size_t)k; ++j)
                for (size_t e = r.first[j]; e < r.first[j] + r.count[j]; ++e, ++frames)
                    fipsd_transport_stats_sent(&u->shared.stats, f[r.frame[e]].len);
            sent += frames;
            FIPSD_PERF_EVENT(UDP_SEND_CONNECTED, s->connected ? frames : 0);
            FIPSD_PERF_EVENT(UDP_SEND_WILDCARD, s->connected ? 0 : frames);
            at += (size_t)k; continue;
        }
        int error = k < 0 ? fipsd_io_errno() : 0;
        if (r.count[at] > 1) {
            if (gso_refused(error) && atomic_exchange_explicit(&s->gso, false, memory_order_relaxed))
                FIPSD_WARN("udp", "%s: UDP GSO refused (errno %d); this socket sends without it", u->name, error);
            tx_build(&r, s, target, source, at, r.first[at], false); continue;
        }
        for (size_t e = r.first[at]; e < r.first[at] + r.count[at]; ++e) {
            rc[r.frame[e]] = -1; fipsd_transport_stats_error(&u->shared.stats, true);
        }
        ++at;
    }
    FIPSD_PERF_END(send_start, UDP_SEND);
    return sent;
}
static size_t send_batch(fipsd_transport *t, const fipsd_transport_frame *f, size_t n, int *rc)
{
    udp_instance *u = t->user;
    bool live = u->started && !go_context_is_canceled(u->root);
    size_t sent = 0;
    for (size_t i = 0; i < n;) {
        size_t k = i + 1;
        while (k < n && k - i < FIPSD_UDP_SEND_BATCH && (f[k].to == f[i].to || fips_transport_addr_equal(f[k].to, f[i].to))) ++k;
        size_t ready = 0;
        for (size_t j = i; j < k; ++j) {
            rc[j] = live && f[j].bytes && f[j].len ? 0 : -1;
            if (!rc[j] && f[j].len > u->config.mtu) { atomic_fetch_add(&u->shared.stats.mtu_exceeded, 1); rc[j] = -1; }
            ready += !rc[j];
        }
        if (ready) {
            fipsd_udp_endpoint target; fipsd_udp_packet_info source; bool unresolved;
            udp_socket *s = lease(u, f[i].to, &target, &source, &unresolved);
            if (s) { sent += send_run(u, s, &target, &source, f + i, k - i, rc + i); fipsd_udp_socket_release(s); }
            else for (size_t j = i; j < k; ++j) if (!rc[j]) {
                if (unresolved) fipsd_transport_stats_error(&u->shared.stats, true);
                rc[j] = -1;
            }
        }
        i = k;
    }
    return sent;
}
#endif
static void authenticated(fipsd_transport *t, const fips_transport_addr *address, fips_link_direction direction)
{
    udp_instance *u = t->user;
    if (!u->started || go_context_is_canceled(u->root)) return;
#if defined(_WIN32)
    /* Connected per-peer sockets share the listener's port through
     * SO_REUSEPORT, which Windows lacks; every peer uses the listener, as
     * upstream does on Windows. */
    (void)address; (void)direction; return;
#endif
    fipsd_udp_endpoint remote;
    if (!fipsd_udp_resolve(&u->dns, address, u->bind_to.address.ss_family, &remote)) return;
    fips_transport_addr numeric;
    if (!fipsd_udp_endpoint_addr(&remote, &numeric)) return;
    udp_mu_lock(&u->mu);
    for (size_t i = 0; i < u->config.peer_capacity; ++i)
        if (u->peers[i] && fips_transport_addr_equal(&u->peers[i]->peer, &numeric)) {
            udp_mu_unlock(&u->mu); return;
        }
    udp_mu_unlock(&u->mu);
    gate_lock(u);
    udp_mu_lock(&u->mu);
    udp_socket *listener = u->listener;
    size_t slot = u->config.peer_capacity;
    for (size_t i = 0; i < u->config.peer_capacity; ++i) {
        if (u->peers[i] && fips_transport_addr_equal(&u->peers[i]->peer, &numeric)) {
            udp_mu_unlock(&u->mu); gate_unlock(u); return;
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
    udp_mu_unlock(&u->mu);
    if (listener && slot < u->config.peer_capacity && !go_context_is_canceled(u->root)) {
        udp_socket *s = fipsd_udp_socket_open(u, &local, &remote, listener->generation, direction);
        if (s) {
            udp_mu_lock(&u->mu); u->peers[slot] = s; udp_mu_unlock(&u->mu);
            fipsd_transport_stats_pool(&u->shared.stats, direction, true);
            FIPSD_PERF_EVENT(CONNECTED_UDP_INSTALLED, 1);
        } else {
            FIPSD_PERF_EVENT(CONNECTED_UDP_ACTIVATION_FAILED, 1);
            atomic_fetch_add(&u->shared.diagnostics.socket_errors, 1);
        }
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
    udp_mu_lock(&u->mu);
    for (size_t i = 0; i < u->config.peer_capacity; ++i)
        if (u->peers[i] && fips_transport_addr_equal(&u->peers[i]->peer, &numeric)) { s = u->peers[i]; u->peers[i] = NULL; break; }
    udp_mu_unlock(&u->mu);
    if (s) { fipsd_transport_stats_pool(&u->shared.stats, s->direction, false); fipsd_udp_socket_close(s); }
    gate_unlock(u);
}
static bool local_addr(const fipsd_transport *t, fips_transport_addr *out)
{ return fipsd_udp_address((fipsd_transport *)t, out); }
/* send_frame is safe on any thread for a numeric peer: its state is under
 * u->mu, socket leases hold descriptors open, and the zero-timeout sendmsg
 * never parks. A hostname may need a blocking lookup: dispatcher only. On
 * Windows the fiber socket layer issues overlapped I/O: dispatcher only, as
 * upstream spawns no encrypt workers there. */
static bool send_concurrent(const fipsd_transport *t, const fips_transport_addr *to)
{
    (void)t;
#if defined(_WIN32)
    (void)to; return false;
#else
    char text[257]; fipsd_udp_endpoint numeric;
    if (!to || !to->len || to->len >= sizeof text || memchr(to->bytes, 0, to->len)) return false;
    memcpy(text, to->bytes, to->len); text[to->len] = 0;
    return fipsd_udp_endpoint_numeric(text, &numeric);
#endif
}
static const fipsd_transport_ops ops = {.local_addr = local_addr, .start_async = start_async, .stop = stop,
    .state = fipsd_transport_shared_state, .mtu = mtu, .send = send_frame, .discover = discover,
    .close_connection = close_connection, .network_changed = network_changed, .authenticated = authenticated,
    .send_concurrent = send_concurrent,
#if defined(__linux__)
    .send_batch = send_batch,
#endif
};
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
    udp_instance *u = t->user; udp_mu_lock(&u->mu);
    bool ok = u->listener && fipsd_udp_endpoint_addr(&u->listener->local, out);
    udp_mu_unlock(&u->mu); return ok;
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
    udp_instance *u = t->user; size_t n = 0; udp_mu_lock(&u->mu);
    for (size_t i = 0; i < u->config.peer_capacity; ++i) if (u->peers[i]) ++n;
    udp_mu_unlock(&u->mu); return n;
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
