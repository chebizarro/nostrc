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
    nsync_mu_lock(&u->mu);
    if (--s->users == 0 && s->detached) go_channel_close(s->unused);
    nsync_mu_unlock(&u->mu);
}
void fipsd_udp_socket_close(udp_socket *s)
{
    if (!s) return;
    udp_instance *u = s->owner;
    nsync_mu_lock(&u->mu);
    s->detached = true;
    if (!s->users) go_channel_close(s->unused);
    nsync_mu_unlock(&u->mu);
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
void fipsd_udp_receive(void *arg)
{
    udp_socket *s = arg; udp_instance *u = s->owner;
    uint8_t *bytes = malloc(65536);
    if (!bytes) goto done;
    while (!go_channel_is_closed(s->stop) && !go_context_is_canceled(u->root)) {
        union { struct cmsghdr align; uint8_t data[512]; } control;
        fipsd_udp_endpoint source = {0};
        struct iovec iov = {.iov_base = bytes, .iov_len = 65536};
        struct msghdr message = {.msg_name = &source.address, .msg_namelen = sizeof source.address,
            .msg_iov = &iov, .msg_iovlen = 1, .msg_control = control.data, .msg_controllen = sizeof control.data};
        ssize_t n = gof_recvmsg(s->fd, &message, 0, -1);
        if (go_channel_is_closed(s->stop) || go_context_is_canceled(u->root)) break;
        if (n < 0) {
            if (fipsd_io_errno() == EINTR) continue;
            int error = fipsd_io_errno();
            fipsd_transport_stats_error(&u->shared.stats, false);
            /* recvmsg consumes these asynchronous ICMP errors. The next
             * receive parks again; a dead drain must not retain a matching
             * connected tuple and black-hole traffic indefinitely. */
            if (error == ECONNREFUSED || error == EHOSTUNREACH || error == ENETUNREACH) continue;
#if defined(_WIN32)
            /* Port/net unreachable reports, should SIO_UDP_*RESET be refused. */
            if (error == ECONNRESET || error == ENETRESET) continue;
#endif
            break;
        }
        fipsd_transport_stats_recv(&u->shared.stats, (size_t)n);
        fipsd_udp_packet_info info;
        source.len = message.msg_namelen;
        fips_transport_addr from;
        if ((message.msg_flags & MSG_TRUNC) || !fipsd_udp_decode_control(&message, &info) ||
            !fipsd_udp_endpoint_addr(&source, &from)) {
            atomic_fetch_add(&u->shared.diagnostics.malformed, 1);
            atomic_fetch_add(&u->shared.diagnostics.dropped, 1); continue;
        }
        if (info.has_ecn) {
            atomic_fetch_add(&u->shared.diagnostics.ecn_recv, 1);
            if (info.ecn == 3) atomic_fetch_add(&u->shared.diagnostics.ce_recv, 1);
        }
        if (info.has_destination) atomic_fetch_add(&u->shared.diagnostics.pktinfo_recv, 1);
        if (info.has_drops) fipsd_transport_stats_kernel_drops(&u->shared.stats, s->connected, info.kernel_drops);
        if (fipsd_udp_classify_from(&u->classifiers, bytes, (size_t)n, &from) == FIPSD_UDP_DISCARD) {
            atomic_fetch_add(&u->shared.diagnostics.classifier_drops, 1); continue;
        }
        if (fipsd_datagram_frame(bytes, (size_t)n, UINT16_MAX) != FIPS_OK) {
            atomic_fetch_add(&u->shared.diagnostics.malformed, 1); atomic_fetch_add(&u->shared.diagnostics.dropped, 1); continue;
        }
        /* Bounded source-selection hints only, never an authentication or
         * connected-pool admission decision. Auth comes from the node. */
        nsync_mu_lock(&u->mu);
        size_t slot = FIPSD_UDP_MAX_PEERS;
        for (size_t i = 0; i < FIPSD_UDP_MAX_PEERS; ++i)
            if (u->routes[i].used && fips_transport_addr_equal(&u->routes[i].peer, &from)) { slot = i; break; }
        if (slot == FIPSD_UDP_MAX_PEERS) slot = u->next_route++ % FIPSD_UDP_MAX_PEERS;
        u->routes[slot] = (udp_route){.used = true, .peer = from, .info = info};
        nsync_mu_unlock(&u->mu);
        if (fipsd_transport_emit(u->events, u->root, s->stop, FIPSD_TRANSPORT_PACKET,
                u->transport.id, &from, bytes, (size_t)n)) break;
    }
    free(bytes);
done:
    if (!go_channel_is_closed(s->stop)) fipsd_watcher_binding_exited(u->watcher, s->generation);
    go_channel_close(s->done);
}
