#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* struct mmsghdr, recvmmsg */
#endif
#define _POSIX_C_SOURCE 200809L
#include "fiber_errno.h"
#include "transport/udp/internal.h"
#include "transport/framing.h"
#include "blocking_executor.h"
#include "libgo/fiber.h"
#include "log.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct setup { udp_socket *socket; const fipsd_udp_endpoint *bind_to; } setup;
static void *open_socket(void *arg)
{
    setup *q = arg; udp_socket *s = q->socket; udp_instance *u = s->owner;
    s->fd = fipsd_udp_open(q->bind_to, u->transport.interface_name, s->connected,
        u->config.recv_buf_size, u->config.send_buf_size, &s->recv_grant, &s->send_grant, &s->local);
    if (s->fd != GOF_INVALID_SOCKET && (fipsd_sockbuf_clamped(&s->recv_grant) || fipsd_sockbuf_clamped(&s->send_grant)))
        FIPSD_WARN("udp", "%s: socket buffers clamped (recv %zu/%zu, send %zu/%zu); raise host limits or grant CAP_NET_ADMIN on Linux",
            u->name, s->recv_grant.actual, s->recv_grant.requested, s->send_grant.actual, s->send_grant.requested);
#if defined(__linux__)
    /* A kernel without UDP GSO would ignore the UDP_SEGMENT cmsg and send one
     * oversize datagram, so probe the option (as quinn-udp does). */
    const char *gso = getenv("FIPSD_UDP_GSO");
    if (s->fd != GOF_INVALID_SOCKET && !(gso && !strcmp(gso, "0")))
        atomic_store(&s->gso, fipsd_udp_gso_supported(s->fd));
#endif
    return NULL;
}
static void *close_fd(void *arg)
{ udp_socket *s = arg; fipsd_socket_close(s->fd); return NULL; }
static void free_socket(udp_socket *s)
{
    if (s->stop) go_channel_unref(s->stop);
    if (s->done) go_channel_unref(s->done);
    if (s->unused) go_channel_unref(s->unused);
    free(s);
}
udp_socket *fipsd_udp_socket_open(udp_instance *u, const fipsd_udp_endpoint *local,
    const fipsd_udp_endpoint *remote, uint64_t generation, fips_link_direction direction)
{
    udp_socket *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->owner = u; s->fd = GOF_INVALID_SOCKET; s->generation = generation; s->direction = direction;
    s->connected = remote != NULL;
    if (remote) { s->remote = *remote; (void)fipsd_udp_endpoint_addr(remote, &s->peer); }
    s->stop = go_channel_create(1); s->done = go_channel_create(1); s->unused = go_channel_create(1);
    if (!s->stop || !s->done || !s->unused) { free_socket(s); return NULL; }
    setup q = {s, local}; go_blocking_submit(open_socket, &q);
    if (s->fd == GOF_INVALID_SOCKET) { free_socket(s); return NULL; }
    if (gof_io_reset_fd(s->fd) ||
        (remote && gof_connect(s->fd, (const struct sockaddr *)&remote->address, remote->len, 0)) ||
        fipsd_runtime_spawn(u->runtime, fipsd_udp_receive, s)) {
        go_blocking_submit(close_fd, s); free_socket(s); return NULL;
    }
    return s;
}
void fipsd_udp_socket_release(udp_socket *s)
{
    udp_instance *u = s->owner;
    udp_mu_lock(&u->mu);
    if (--s->users == 0 && s->detached) go_channel_close(s->unused);
    udp_mu_unlock(&u->mu);
}
void fipsd_udp_socket_close(udp_socket *s)
{
    if (!s) return;
    udp_instance *u = s->owner;
    udp_mu_lock(&u->mu);
    s->detached = true;
    if (!s->users) go_channel_close(s->unused);
    udp_mu_unlock(&u->mu);
    /* Detached from every lookup before this call; await send leases before
     * changing socket association. Receivers get their own cancellation for
     * channel backpressure as well as a latched readiness cancellation. */
    go_channel_close(s->stop);
    void *value = NULL; (void)go_channel_receive(s->unused, &value);
    /* No route/interface dependency: shutdown of unconnected UDP is not a
     * portable wake, and close is never cancellation of a parked fiber. */
    (void)gof_io_cancel_fd(s->fd); /* reset at open preallocated the registry entry */
    (void)go_channel_receive(s->done, &value);
    (void)gof_io_reset_fd(s->fd); /* all leases and the drain have joined */
    go_blocking_submit(close_fd, s); free_socket(s);
}
/* One received datagram, as either receive loop delivers it: counters,
 * ancillary decode, classifiers, framing and the source-route hint. True when
 * it is a frame for the node; the caller then emits it. *route caches the
 * hint slot last written by this drain, so a burst from one source takes the
 * lock without the table scan; the table content is the same either way. */
static bool take(udp_socket *s, const struct msghdr *message, const uint8_t *bytes, size_t n,
    const fipsd_udp_endpoint *source, fips_transport_addr *from, size_t *route, uint32_t *drops, bool *has_drops)
{
    udp_instance *u = s->owner;
    fipsd_transport_stats_recv(&u->shared.stats, n);
    fipsd_udp_packet_info info;
    if ((message->msg_flags & MSG_TRUNC) || !fipsd_udp_decode_control(message, &info) ||
        !fipsd_udp_endpoint_addr(source, from)) {
        atomic_fetch_add(&u->shared.diagnostics.malformed, 1);
        atomic_fetch_add(&u->shared.diagnostics.dropped, 1); return false;
    }
    if (info.has_ecn) {
        atomic_fetch_add(&u->shared.diagnostics.ecn_recv, 1);
        if (info.ecn == 3) atomic_fetch_add(&u->shared.diagnostics.ce_recv, 1);
    }
    if (info.has_destination) atomic_fetch_add(&u->shared.diagnostics.pktinfo_recv, 1);
    /* SO_RXQ_OVFL is cumulative: the caller records the last value it saw. */
    if (info.has_drops) { *drops = info.kernel_drops; *has_drops = true; }
    if (fipsd_udp_classify_from(&u->classifiers, bytes, n, from) == FIPSD_UDP_DISCARD) {
        atomic_fetch_add(&u->shared.diagnostics.classifier_drops, 1); return false;
    }
    if (fipsd_datagram_frame(bytes, n, UINT16_MAX) != FIPS_OK) {
        atomic_fetch_add(&u->shared.diagnostics.malformed, 1); atomic_fetch_add(&u->shared.diagnostics.dropped, 1); return false;
    }
    /* Bounded source-selection hints only, never an authentication or
     * connected-pool admission decision. Auth comes from the node. */
    udp_mu_lock(&u->mu);
    size_t slot = *route;
    if (slot >= FIPSD_UDP_MAX_PEERS || !u->routes[slot].used || !fips_transport_addr_equal(&u->routes[slot].peer, from)) {
        slot = FIPSD_UDP_MAX_PEERS;
        for (size_t i = 0; i < FIPSD_UDP_MAX_PEERS; ++i)
            if (u->routes[i].used && fips_transport_addr_equal(&u->routes[i].peer, from)) { slot = i; break; }
        if (slot == FIPSD_UDP_MAX_PEERS) slot = u->next_route++ % FIPSD_UDP_MAX_PEERS;
    }
    u->routes[slot] = (udp_route){.used = true, .peer = *from, .info = info};
    udp_mu_unlock(&u->mu);
    *route = slot;
    return true;
}
/* Errors that end neither drain: recvmsg consumes these asynchronous ICMP
 * errors and the next receive parks again; a dead drain must not retain a
 * matching connected tuple and black-hole traffic indefinitely. */
static bool transient(int error)
{
    if (error == ECONNREFUSED || error == EHOSTUNREACH || error == ENETUNREACH) return true;
#if defined(_WIN32)
    /* Port/net unreachable reports, should SIO_UDP_*RESET be refused. */
    if (error == ECONNRESET || error == ENETRESET) return true;
#endif
    return false;
}
#if defined(__linux__)
static fipsd_transport_event *rx_event(size_t cap) { return malloc(sizeof(fipsd_transport_event) + cap); }
void fipsd_udp_rx_free(udp_rx *rx)
{
    for (size_t i = 0; i < FIPSD_UDP_RECV_BATCH; ++i) free(rx->slot[i]);
    free(rx->spill); free(rx);
}
udp_rx *fipsd_udp_rx_create(size_t cap)
{
    udp_rx *rx = calloc(1, sizeof *rx);
    if (!rx) return NULL;
    rx->cap = cap;
    /* Only oversize datagrams write it: its untouched pages cost no memory. */
    rx->spill = malloc(FIPSD_UDP_RECV_BATCH * (FIPSD_UDP_RECV_MAX - cap));
    bool ok = rx->spill != NULL;
    for (size_t i = 0; ok && i < FIPSD_UDP_RECV_BATCH; ++i) ok = (rx->slot[i] = rx_event(cap)) != NULL;
    if (!ok) { fipsd_udp_rx_free(rx); return NULL; }
    rx->filled = FIPSD_UDP_RECV_BATCH;
    return rx;
}
/* The kernel writes only the messages it fills, and only those slots can
 * have a new event: re-arm the previous call's. */
static void rx_arm(udp_rx *rx)
{
    for (size_t i = 0; i < rx->filled; ++i) {
        rx->iov[i][0] = (struct iovec){.iov_base = rx->slot[i]->bytes, .iov_len = rx->cap};
        rx->iov[i][1] = (struct iovec){.iov_base = rx->spill + i * (FIPSD_UDP_RECV_MAX - rx->cap),
            .iov_len = FIPSD_UDP_RECV_MAX - rx->cap};
        rx->m[i].msg_len = 0;
        rx->m[i].msg_hdr = (struct msghdr){.msg_name = &rx->source[i].address,
            .msg_namelen = sizeof rx->source[i].address, .msg_iov = rx->iov[i], .msg_iovlen = 2,
            .msg_control = rx->control[i].data, .msg_controllen = sizeof rx->control[i].data};
    }
}
int fipsd_udp_recv_batch(udp_rx *rx, gof_socket_t fd)
{
    rx_arm(rx);
    int n = gof_recvmmsg(fd, rx->m, FIPSD_UDP_RECV_BATCH, 0, -1);
    rx->filled = n > 0 ? (size_t)n : 0;
    return n;
}
/* Hand datagram i to the node; false when the drain must stop. */
static bool rx_deliver(udp_socket *s, udp_rx *rx, size_t i, size_t *route, uint32_t *drops, bool *has_drops)
{
    udp_instance *u = s->owner;
    size_t n = rx->m[i].msg_len;
    fipsd_udp_endpoint *source = &rx->source[i];
    source->len = rx->m[i].msg_hdr.msg_namelen;
    fipsd_transport_event *e = rx->slot[i];
    bool spilled = n > rx->cap;
    if (spilled) {
        /* Contiguous for the classifiers and framing, and the slot keeps its event. */
        if (!(e = rx_event(n))) return false;
        memcpy(e->bytes, rx->slot[i]->bytes, rx->cap);
        memcpy(e->bytes + rx->cap, rx->iov[i][1].iov_base, n - rx->cap);
    }
    fips_transport_addr from;
    if (!take(s, &rx->m[i].msg_hdr, e->bytes, n, source, &from, route, drops, has_drops)) {
        if (spilled) free(e);
        return true;
    }
    fipsd_transport_event *fresh = spilled ? NULL : rx_event(rx->cap);
    if (!spilled && !fresh) return false; /* as an allocation failure in emit */
    e->kind = FIPSD_TRANSPORT_PACKET; e->id = u->transport.id; e->address = from; e->size = n;
    if (fipsd_transport_emit_event(u->events, u->root, s->stop, e)) {
        if (spilled) free(e); else free(fresh);
        return false;
    }
    if (!spilled) rx->slot[i] = fresh;
    return true;
}
void fipsd_udp_receive(void *arg)
{
    udp_socket *s = arg; udp_instance *u = s->owner;
    udp_rx *rx = fipsd_udp_rx_create(u->config.mtu); /* a uint16_t, below FIPSD_UDP_RECV_MAX */
    if (!rx) goto done;
    size_t route = FIPSD_UDP_MAX_PEERS;
    while (!go_channel_is_closed(s->stop) && !go_context_is_canceled(u->root)) {
        int n = fipsd_udp_recv_batch(rx, s->fd);
        if (go_channel_is_closed(s->stop) || go_context_is_canceled(u->root)) break;
        if (n < 0) {
            int error = fipsd_io_errno();
            if (error == EINTR) continue;
            fipsd_transport_stats_error(&u->shared.stats, false);
            if (transient(error)) continue;
            break;
        }
        uint32_t drops = 0; bool has_drops = false, live = true;
        for (int i = 0; i < n && live; ++i) live = rx_deliver(s, rx, (size_t)i, &route, &drops, &has_drops);
        if (has_drops) fipsd_transport_stats_kernel_drops(&u->shared.stats, s->connected, drops);
        if (!live) break;
    }
    fipsd_udp_rx_free(rx);
done:
    if (!go_channel_is_closed(s->stop)) fipsd_watcher_binding_exited(u->watcher, s->generation);
    go_channel_close(s->done);
}
#else
void fipsd_udp_receive(void *arg)
{
    udp_socket *s = arg; udp_instance *u = s->owner;
    uint8_t *bytes = malloc(FIPSD_UDP_RECV_MAX);
    if (!bytes) goto done;
    size_t route = FIPSD_UDP_MAX_PEERS;
    while (!go_channel_is_closed(s->stop) && !go_context_is_canceled(u->root)) {
        union { struct cmsghdr align; uint8_t data[512]; } control;
        fipsd_udp_endpoint source = {0};
        struct iovec iov = {.iov_base = bytes, .iov_len = FIPSD_UDP_RECV_MAX};
        struct msghdr message = {.msg_name = &source.address, .msg_namelen = sizeof source.address,
            .msg_iov = &iov, .msg_iovlen = 1, .msg_control = control.data, .msg_controllen = sizeof control.data};
        ssize_t n = gof_recvmsg(s->fd, &message, 0, -1);
        if (go_channel_is_closed(s->stop) || go_context_is_canceled(u->root)) break;
        if (n < 0) {
            int error = fipsd_io_errno();
            if (error == EINTR) continue;
            fipsd_transport_stats_error(&u->shared.stats, false);
            if (transient(error)) continue;
            break;
        }
        source.len = message.msg_namelen;
        fips_transport_addr from; uint32_t drops = 0; bool has_drops = false;
        bool frame = take(s, &message, bytes, (size_t)n, &source, &from, &route, &drops, &has_drops);
        if (has_drops) fipsd_transport_stats_kernel_drops(&u->shared.stats, s->connected, drops);
        if (frame && fipsd_transport_emit(u->events, u->root, s->stop, FIPSD_TRANSPORT_PACKET,
                u->transport.id, &from, bytes, (size_t)n)) break;
    }
    free(bytes);
done:
    if (!go_channel_is_closed(s->stop)) fipsd_watcher_binding_exited(u->watcher, s->generation);
    go_channel_close(s->done);
}
#endif
