#define _POSIX_C_SOURCE 200809L
#include "transport/tcp/internal.h"
#include "blocking_executor.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static bool stopped(tcp_conn *c)
{
    return go_channel_is_closed(c->stop) || go_channel_is_closed(c->owner->stop) ||
        go_context_is_canceled(c->owner->root);
}
typedef struct socket_setup { tcp_instance *u; int fd, family, result, error; } socket_setup;
static void *configure(void *arg)
{
    socket_setup *s = arg;
    if (s->fd < 0) s->fd = socket(s->family, SOCK_STREAM, IPPROTO_TCP);
    s->result = s->fd < 0 ? -1 : tcp_socket_configure(s->u, s->fd);
    s->error = s->result ? errno : 0;
    return NULL;
}
static bool establish(tcp_conn *c, int *error)
{
    tcp_instance *u = c->owner;
    if (c->direction == FIPS_LINK_INBOUND) {
        socket_setup s = {.u = u, .fd = c->fd}; go_blocking_submit(configure, &s);
        nsync_mu_lock(&u->mu);
        bool ok = !stopped(c) && s.result == 0 && gof_io_reset_fd(c->fd) == 0;
        *error = s.error; nsync_mu_unlock(&u->mu); return ok;
    }
    const fipsd_stream_dialer *dialer = u->config.dialer;
    tcp_endpoint *items = NULL; size_t n = 0;
    if (!tcp_resolve(dialer ? dialer->via : &c->peer, false, &items, &n)) { *error = dialer ? EHOSTUNREACH : 0; return false; }
    bool ok = false;
    uint64_t deadline = fipsd_transport_now_ms() + u->config.connect_timeout_ms;
    for (size_t i = 0; i < n && !stopped(c); ++i) {
        socket_setup s = {.u = u, .fd = -1, .family = items[i].address.ss_family};
        go_blocking_submit(configure, &s);
        if (s.result || stopped(c)) { *error = s.fd < 0 ? s.error : 0; tcp_fd_close(s.fd); continue; }
        nsync_mu_lock(&u->mu);
        if (stopped(c) || gof_io_reset_fd(s.fd)) { nsync_mu_unlock(&u->mu); tcp_fd_close(s.fd); break; }
        c->fd = s.fd;
        nsync_mu_unlock(&u->mu);
        uint64_t now = fipsd_transport_now_ms();
        int rc = now < deadline ? gof_connect(s.fd, (const struct sockaddr *)&items[i].address,
                items[i].len, (int)(deadline - now)) : -1;
        *error = now < deadline ? (rc ? tcp_io_errno() : 0) : ETIMEDOUT;
        /* An unreachable proxy is a SOCKS5 failure, not the peer refusing. */
        if (rc && dialer && *error != ETIMEDOUT) *error = EHOSTUNREACH;
        /* Once a proxy address accepted the TCP connect its handshake answer
         * is final, as tokio-socks: no other proxy address is tried. */
        bool answered = !rc && dialer && !stopped(c);
        if (answered && dialer->upgrade(dialer->user, s.fd, &c->peer, deadline, error)) rc = -1;
        else answered = false;
        if (!rc && !stopped(c)) {
            c->remote = items[i]; c->local.len = sizeof c->local.address;
            ok = getsockname(s.fd, (struct sockaddr *)&c->local.address, &c->local.len) == 0;
            if (ok) break;
            *error = tcp_io_errno();
        }
        nsync_mu_lock(&u->mu); c->fd = -1; nsync_mu_unlock(&u->mu);
        tcp_fd_close(s.fd);
        if (answered) break;
    }
    free(items); return ok;
}
static void reader(void *arg)
{
    tcp_conn *c = arg; tcp_instance *u = c->owner;
    int error = EHOSTUNREACH;
    if (!stopped(c) && establish(c, &error)) {
        c->mss_mtu = tcp_socket_mss(c->fd, u->config.mtu);
        go_channel_close(c->ready);
        void *v; GoSelectCase wait[] = {{GO_SELECT_RECEIVE, c->run, NULL, &v},
            {GO_SELECT_RECEIVE, c->stop, NULL, &v}, {GO_SELECT_RECEIVE, go_context_done(u->root), NULL, &v}};
        if (go_select(wait, 3) == 0 && !stopped(c)) {
            uint8_t *buffer = malloc(UINT16_MAX);
            if (buffer) while (!stopped(c)) {
                size_t n = 0; int rc = fipsd_stream_read(c->fd, buffer, UINT16_MAX, &n);
                if (rc != 1) {
                    if (rc < 0 && !stopped(c)) {
                        if (tcp_io_errno() == EPROTO) atomic_fetch_add(&u->shared.diagnostics.malformed, 1);
                        tcp_stats_error(u, false);
                    }
                    break;
                }
                atomic_store(&c->last_frame_ms, fipsd_transport_now_ms());
                (void)go_channel_try_send(c->progress, c);
                fipsd_transport_stats_recv(&u->shared.stats, n);
                if (fipsd_transport_emit(u->events, u->root, c->stop, FIPSD_TRANSPORT_PACKET,
                        u->transport.id, &c->peer, buffer, n)) break;
            }
            free(buffer);
        }
    } else if (!stopped(c) && c->direction == FIPS_LINK_OUTBOUND && error) {
        if (error == ETIMEDOUT) atomic_fetch_add(&u->shared.stats.connect_timeouts, 1);
        else if (!u->config.dialer || error == ECONNREFUSED) atomic_fetch_add(&u->shared.stats.connect_refused, 1);
        else atomic_fetch_add(&u->shared.stats.socks5_errors, 1);
    }
    go_channel_close(c->reader_done);
}
static void writer(void *arg)
{
    tcp_conn *c = arg; tcp_instance *u = c->owner;
    while (!stopped(c)) {
        void *value = NULL;
        GoSelectCase cases[] = {{GO_SELECT_RECEIVE, c->stop, NULL, &value},
            {GO_SELECT_RECEIVE, go_context_done(u->root), NULL, &value},
            {GO_SELECT_RECEIVE, c->queue, NULL, &value}};
        int which = go_select(cases, 3);
        if (which != 2 || !value) break;
        tcp_frame *frame = value;
        int rc = fipsd_stream_write(c->fd, frame->bytes, frame->len);
        if (!rc) fipsd_transport_stats_sent(&u->shared.stats, frame->len);
        free(frame);
        if (rc) { if (!stopped(c)) tcp_stats_error(u, true); break; }
    }
    go_channel_close(c->writer_done);
}
static void join(GoChannel *done) { void *v; (void)go_channel_receive(done, &v); }
void tcp_conn_owner(void *arg)
{
    tcp_conn *c = arg; tcp_instance *u = c->owner;
    bool has_reader = fipsd_runtime_spawn(u->runtime, reader, c) == 0, has_writer = false;
    if (has_reader) {
        void *v;
        GoSelectCase init[] = {{GO_SELECT_RECEIVE, c->stop, NULL, &v},
            {GO_SELECT_RECEIVE, go_context_done(u->root), NULL, &v},
            {GO_SELECT_RECEIVE, c->reader_done, NULL, &v}, {GO_SELECT_RECEIVE, c->ready, NULL, &v},
            {GO_SELECT_RECEIVE, c->close, NULL, &v}};
        if (go_select(init, 5) == 3 && !stopped(c)) {
            has_writer = fipsd_runtime_spawn(u->runtime, writer, c) == 0;
            if (has_writer) {
                nsync_mu_lock(&u->mu);
                bool admitted = !c->closing && !stopped(c);
                if (admitted && c->direction == FIPS_LINK_OUTBOUND) admitted = tcp_connected_locked(c) == 0;
                if (admitted) {
                    c->established = true;
                    if (c->direction == FIPS_LINK_OUTBOUND) fipsd_transport_stats_pool(&u->shared.stats, c->direction, true);
                }
                nsync_mu_unlock(&u->mu);
                if (admitted) {
                    if (c->direction == FIPS_LINK_INBOUND) atomic_fetch_add(&u->shared.stats.connections_accepted, 1);
                    else atomic_fetch_add(&u->shared.stats.connections_established, 1);
                    go_channel_close(c->run);
                    while (!stopped(c)) {
                        GoSelectCase cases[] = {{GO_SELECT_RECEIVE, c->stop, NULL, &v},
                            {GO_SELECT_RECEIVE, go_context_done(u->root), NULL, &v},
                            {GO_SELECT_RECEIVE, c->reader_done, NULL, &v},
                            {GO_SELECT_RECEIVE, c->writer_done, NULL, &v},
                            {GO_SELECT_RECEIVE, c->close, NULL, &v}, {GO_SELECT_RECEIVE, c->progress, NULL, &v}};
                        int selected;
                        if (c->direction == FIPS_LINK_INBOUND) {
                            uint64_t last = atomic_load(&c->last_frame_ms), now = fipsd_transport_now_ms();
                            uint64_t limit = last ? u->config.idle_ms : u->config.first_frame_ms;
                            uint64_t elapsed = now - (last ? last : c->accepted_ms);
                            if (elapsed >= limit) { atomic_fetch_add(&u->shared.diagnostics.deadlines, 1); break; }
                            GoSelectResult r = go_select_timeout(cases, 6, limit - elapsed);
                            selected = r.selected_case;
                            if (selected < 0) continue; /* recompute after timer or progress */
                        } else selected = go_select(cases, 6);
                        if (selected == 5) continue;
                        if (selected == 4) {
                            /* Deliberate close preserves queued msg2/reject/close frames.
                             * Whole-fd cancellation is deferred until writer drain or
                             * the same bounded 5 s grace as upstream stream.rs. */
                            go_channel_close(c->queue);
                            GoSelectCase drain[] = {{GO_SELECT_RECEIVE, c->writer_done, NULL, &v},
                                {GO_SELECT_RECEIVE, c->stop, NULL, &v},
                                {GO_SELECT_RECEIVE, go_context_done(u->root), NULL, &v}};
                            (void)go_select_timeout(drain, 3, 5000);
                        }
                        break;
                    }
                }
            }
        }
    }
    nsync_mu_lock(&u->mu);
    c->closing = true;
    go_channel_close(c->stop);
    if (c->fd >= 0) (void)gof_io_cancel_fd(c->fd);
    nsync_mu_unlock(&u->mu);
    if (has_reader) join(c->reader_done);
    if (has_writer) join(c->writer_done);
    tcp_fd_close(c->fd);
    /* Never let an old owner's exit take a newer connection down. Retain the
     * pool entry through this event so ordinary reconnect cannot overtake it. */
    nsync_mu_lock(&u->mu); bool replacement = tcp_pool_find(u, &c->peer) != NULL; nsync_mu_unlock(&u->mu);
    if (!replacement && !go_channel_is_closed(c->close))
        (void)fipsd_transport_emit(u->events, u->root, c->notify_stop, FIPSD_TRANSPORT_DISCONNECTED,
            u->transport.id, &c->peer, NULL, 0);
    tcp_pool_remove(c); tcp_conn_free(c);
}
void tcp_conn_free(tcp_conn *c)
{
    if (!c) return;
    void *v;
    if (c->queue) while (go_channel_try_receive(c->queue, &v) == 0) free(v);
    GoChannel *channels[] = {c->stop, c->notify_stop, c->close, c->ready, c->run, c->reader_done,
        c->writer_done, c->progress, c->queue};
    for (size_t i = 0; i < sizeof channels / sizeof channels[0]; ++i) if (channels[i]) go_channel_unref(channels[i]);
    free(c);
}
