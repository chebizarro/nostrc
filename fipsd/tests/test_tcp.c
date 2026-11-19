#define _POSIX_C_SOURCE 200809L
#include "transport/tcp/internal.h"
#include "transport/medium.h"
#include "test_tmpdir.h"
#include "blocking_executor.h"
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static void *receive(GoChannel *channel)
{
    void *v = NULL; GoSelectCase c = {GO_SELECT_RECEIVE, channel, NULL, &v};
    GoSelectResult r = go_select_timeout(&c, 1, 15000); assert(r.selected_case == 0); return v;
}
static void frame(uint8_t *p, size_t n)
{
    memset(p, 0, n); assert(n >= FIPS_FRAMING_FMP_ENCRYPTED_MIN_SIZE);
    size_t payload = n - FIPS_FRAMING_FMP_ENCRYPTED_MIN_SIZE;
    p[2] = (uint8_t)payload; p[3] = (uint8_t)(payload >> 8);
}
static void framing(void)
{
    uint8_t msg[114] = {1, 0, 110, 0}, buf[256], pair[228];
    memcpy(pair, msg, 114); memcpy(pair + 114, msg, 114);
    for (size_t split = 0; split < sizeof msg; ++split) {
        fipsd_stream s; fipsd_stream_init(&s, buf, sizeof buf, UINT16_MAX); size_t n;
        assert(fipsd_stream_feed(&s, msg, split, &n) == 0 && n == split);
        assert(fipsd_stream_feed(&s, msg + split, sizeof msg - split, &n) == 1);
        assert(!memcmp(buf, msg, sizeof msg));
    }
    fipsd_stream s; size_t n;
    fipsd_stream_init(&s, buf, sizeof buf, UINT16_MAX);
    assert(fipsd_stream_feed(&s, pair, sizeof pair, &n) == 1 && n == 114);
    fipsd_stream_reset(&s);
    assert(fipsd_stream_feed(&s, pair + n, sizeof pair - n, &n) == 1 && n == 114);
    fipsd_stream_reset(&s);
    uint8_t oversized[4] = {0, 0, 255, 255};
    assert(fipsd_stream_feed(&s, oversized, 4, &n) == -1 && !fipsd_stream_eof(&s));
    assert(fipsd_stream_feed(&s, msg, sizeof msg, &n) == -1);
    fipsd_stream_reset(&s); assert(fipsd_stream_eof(&s));
    assert(fipsd_stream_feed(&s, msg, 1, &n) == 0 && !fipsd_stream_eof(&s));
    puts("framing: every partial offset, coalesced frames, oversize, truncated EOF: ok");
}
static unsigned plain_sends;
static int plain_send(fipsd_transport *t, const fips_transport_addr *to, const uint8_t *bytes, size_t n)
{ (void)t; (void)to; (void)bytes; (void)n; ++plain_sends; return 0; }
static void config(void)
{
    /* A transport without send_lane (UDP, Ethernet, BLE, Tor, Nym, loopback)
     * takes every lane through its plain send. */
    const fipsd_transport_ops plain = {.send = plain_send};
    fipsd_transport single = {.ops = &plain};
    assert(fipsd_transport_send_lane(&single, FIPSD_TRANSPORT_LANE_CONTROL, NULL, NULL, 0) == 0);
    assert(fipsd_transport_send_lane(&single, FIPSD_TRANSPORT_LANE_DATA, NULL, NULL, 0) == 0 && plain_sends == 2);
    const char *yaml = "node: {limits: {max_connections: 17}}\ntransports:\n  tcp:\n    dial: {}\n    listen: {bind_addr: '[::1]:0', max_inbound_connections: 3, keepalive_secs: 0}\n";
    fipsd_config *cfg = fipsd_config_parse(yaml, strlen(yaml), NULL); assert(cfg);
    fipsd_transport_registry r = {0}; fipsd_transport *owned[32]; size_t n = 0;
    assert(fipsd_tcp_register_config(&r, cfg, owned, &n) == 0 && n == 2);
    bool dial = false, listener = false;
    for (size_t i = 0; i < n; ++i) {
        tcp_instance *u = owned[i]->user;
        if (!strcmp(owned[i]->name, "dial")) { assert(!owned[i]->accept_connections && u->config.max_inbound_connections == 17); dial = true; }
        else { assert(owned[i]->accept_connections && u->config.max_inbound_connections == 3 && !u->config.keepalive_secs); listener = true; }
        assert(owned[i]->ops->mtu(owned[i], NULL) == 1400);
        assert(fipsd_transport_medium(owned[i]) == FIPS_PATH_Unknown);
        fipsd_tcp_destroy(owned[i]);
    }
    assert(dial && listener); fipsd_config_free(cfg);
    fipsd_tcp_config c; fipsd_tcp_config_default(&c); c.bind_addr = "not-a-socket";
    assert(!fipsd_tcp_create((fips_transport_id){1}, "invalid", &c));
    tcp_endpoint x = {0}, y = {0};
    struct sockaddr_in6 *x6 = (void *)&x.address, *y6 = (void *)&y.address;
    x6->sin6_family = y6->sin6_family = AF_INET6;
    assert(inet_pton(AF_INET6, "2001:db8:1:2::1", &x6->sin6_addr) == 1);
    assert(inet_pton(AF_INET6, "2001:db8:1:2::ffff", &y6->sin6_addr) == 1 && tcp_same_source(&x, &y));
    assert(inet_pton(AF_INET6, "2001:db8:1:3::1", &y6->sin6_addr) == 1 && !tcp_same_source(&x, &y));
    assert(inet_pton(AF_INET6, "::ffff:127.0.0.1", &x6->sin6_addr) == 1);
    struct sockaddr_in *y4 = (void *)&y.address; y4->sin_family = AF_INET;
    assert(inet_pton(AF_INET, "127.0.0.1", &y4->sin_addr) == 1 && tcp_same_source(&x, &y));
    puts("config: named/outbound-only, cap override and node fallback, MTU and shared medium: ok");
}
typedef struct fixture {
    fipsd_transport *a, *b, *dial;
    fipsd_runtime *rt;
    GoChannel *ea, *eb, *ed, *done;
    fips_transport_addr ba;
} fixture;
static fipsd_transport_event *event(GoChannel *ch, fipsd_transport_event_kind kind)
{
    for (;;) {
        fipsd_transport_event *e = receive(ch); assert(e);
        if (e->kind == kind) return e;
        assert(e->kind == FIPSD_TRANSPORT_BECAME_PRESENT || e->kind == FIPSD_TRANSPORT_CONNECTED);
        free(e);
    }
}
static int start(fipsd_subsystem *sub, fipsd_runtime *rt)
{
    fixture *f = sub->user; f->rt = rt;
    if (f->a->ops->start_async(f->a, rt, NULL, f->ea) || f->b->ops->start_async(f->b, rt, NULL, f->eb)) return -1;
    return f->dial->ops->start_async(f->dial, rt, NULL, f->ed);
}
static void stop(fipsd_subsystem *sub, fipsd_runtime *rt)
{
    (void)rt; fixture *f = sub->user;
    f->a->ops->stop(f->a); f->b->ops->stop(f->b); f->dial->ops->stop(f->dial);
}
static int raw_connect(fixture *f)
{
    tcp_endpoint *items = NULL; size_t n = 0;
    assert(tcp_resolve(&f->ba, true, &items, &n));
    int fd = socket(items[0].address.ss_family, SOCK_STREAM, IPPROTO_TCP); assert(fd >= 0);
    assert(tcp_socket_configure(f->a->user, fd) == 0 && gof_io_reset_fd(fd) == 0);
    assert(gof_connect(fd, (struct sockaddr *)&items[0].address, items[0].len, 5000) == 0);
    free(items); return fd;
}
static void pool_drained(tcp_instance *u)
{
    /* A pool-removal notification, not a timing assumption, releases the next
     * admission. This channel may contain earlier notifications. */
    for (;;) {
        nsync_mu_lock(&u->mu); bool empty = !u->connections; nsync_mu_unlock(&u->mu);
        if (empty) break;
        (void)receive(u->changed);
    }
}
static void peer_removed(fixture *f)
{
    free(event(f->eb, FIPSD_TRANSPORT_DISCONNECTED));
    pool_drained(f->b->user);
}
/* libgo's trace hook reports committed I/O parking; a native notifier cancels
 * only after a real gof_connect or read has reached that boundary. */
static pthread_mutex_t park_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t park_cv = PTHREAD_COND_INITIALIZER;
static int watch_fd = -1, watch_events;
static bool parked, intercept_arm, release_arm;
void gof_trace_on_io_armed(int fd, int events)
{
    (void)fd;
    pthread_mutex_lock(&park_mu);
    if (intercept_arm && watch_fd == -2 && (events & watch_events)) {
        parked = true; pthread_cond_signal(&park_cv);
        while (!release_arm) pthread_cond_wait(&park_cv, &park_mu);
    }
    pthread_mutex_unlock(&park_mu);
}
void gof_trace_on_block(int fd, int events)
{
    pthread_mutex_lock(&park_mu);
    if ((watch_fd == -2 || watch_fd == fd) && (events & watch_events)) {
        parked = true; pthread_cond_signal(&park_cv);
    }
    pthread_mutex_unlock(&park_mu);
}
static void *cancel_when_parked(void *arg)
{
    fipsd_transport *t = arg;
    pthread_mutex_lock(&park_mu);
    while (!parked) pthread_cond_wait(&park_cv, &park_mu);
    watch_fd = -1;
    pthread_mutex_unlock(&park_mu);
    t->ops->stop(t);
    pthread_mutex_lock(&park_mu); release_arm = true; pthread_cond_signal(&park_cv); pthread_mutex_unlock(&park_mu);
    return NULL;
}
static void *join_thread(void *arg) { assert(pthread_join(*(pthread_t *)arg, NULL) == 0); return NULL; }
static void *wait_parked(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&park_mu);
    while (!parked) pthread_cond_wait(&park_cv, &park_mu);
    watch_fd = -1;
    pthread_mutex_unlock(&park_mu);
    return NULL;
}
static void cancel_connect(fixture *f)
{
    /* Intercept the real connect between readiness registration and park.
     * Loopback may complete immediately after arm, so a committed-park-only
     * test is inherently racy. The trace barrier forces cancellation to race
     * readiness in exactly the fd latch's pre-park window. */
    tcp_endpoint *items = NULL; size_t count = 0;
    assert(tcp_resolve(&f->ba, true, &items, &count));
    tcp_endpoint local = items[0]; free(items);
    if (local.address.ss_family == AF_INET) ((struct sockaddr_in *)&local.address)->sin_port = 0;
    else ((struct sockaddr_in6 *)&local.address)->sin6_port = 0;
    int listener = socket(local.address.ss_family, SOCK_STREAM, IPPROTO_TCP); assert(listener >= 0);
    assert(bind(listener, (struct sockaddr *)&local.address, local.len) == 0 && listen(listener, 1) == 0);
    assert(getsockname(listener, (struct sockaddr *)&local.address, &local.len) == 0);
    fips_transport_addr target; assert(tcp_endpoint_addr(&local, &target));
    pthread_mutex_lock(&park_mu); watch_fd = -2; watch_events = 2; parked = false; intercept_arm = true; release_arm = false; pthread_mutex_unlock(&park_mu);
    pthread_t notifier; assert(pthread_create(&notifier, NULL, cancel_when_parked, f->dial) == 0);
    assert(f->dial->ops->connect_async(f->dial, &target) == 0);
    go_blocking_submit(join_thread, &notifier);
    pthread_mutex_lock(&park_mu); intercept_arm = false; pthread_mutex_unlock(&park_mu);
    assert(fipsd_tcp_connected_count(f->dial) == 0);
    tcp_fd_close(listener);
    puts("cancellation: real connect arm/readiness race, stop before park and join: ok");
}
static void scenario(void *arg)
{
    fixture *f = arg; uint8_t bytes[1280]; frame(bytes, sizeof bytes);
    assert(f->a->ops->connect_async(f->a, &f->ba) == 0);
    free(event(f->ea, FIPSD_TRANSPORT_CONNECTED));
    assert(f->a->ops->send(f->a, &f->ba, bytes, sizeof bytes) == 0);
    memset(bytes, 0xff, sizeof bytes); /* caller storage is no longer borrowed */
    fipsd_transport_event *e = event(f->eb, FIPSD_TRANSPORT_PACKET);
    assert(e->size == 1280 && e->bytes[0] == 0 && e->bytes[1279] == 0);
    fips_transport_addr remote = e->address; free(e);
    uint8_t msg[114] = {1, 0, 110, 0};
    assert(f->b->ops->send(f->b, &remote, msg, sizeof msg) == 0);
    e = event(f->ea, FIPSD_TRANSPORT_PACKET); assert(e->size == sizeof msg); free(e);
    frame(bytes, sizeof bytes);
    assert(f->b->ops->send(f->b, &remote, bytes, sizeof bytes) == -1); /* B has 128-byte send MTU */
    assert(f->a->ops->send(f->a, &f->ba, bytes, sizeof bytes + 1) == -1);
    fipsd_transport_stats_view av = fipsd_tcp_stats_snapshot(f->a), bv = fipsd_tcp_stats_snapshot(f->b);
    assert(av.packets_recv == 1 && av.bytes_recv == 114 && av.mtu_exceeded == 1);
    assert(bv.packets_recv == 1 && bv.bytes_recv == 1280 && bv.mtu_exceeded == 1);
    assert(av.pool_outbound == 1 && bv.pool_inbound == 1 && !av.send_errors && !bv.recv_errors);
    assert(fipsd_tcp_connected_count(f->a) == 1 && fipsd_tcp_connected_count(f->b) == 1);
    tcp_instance *a = f->a->user;
    nsync_mu_lock(&a->mu); tcp_conn *active = tcp_pool_find(a, &f->ba); assert(active);
    int setting = 0; socklen_t setting_len = sizeof setting;
    assert(!getsockopt(active->fd, SOL_SOCKET, SO_KEEPALIVE, &setting, &setting_len) && setting != 0);
    assert(!getsockopt(active->fd, IPPROTO_TCP, TCP_NODELAY, &setting, &setting_len) && setting != 0);
    assert(active->mss_mtu > 0 && active->mss_mtu <= a->config.mtu);
    nsync_mu_unlock(&a->mu);
    /* Pending/established outbound share one capacity. */
    fips_transport_addr other; assert(fips_transport_addr_from_string("127.0.0.1:1", 11, &other) == FIPS_OK);
    assert(f->a->ops->connect_async(f->a, &other) == -1);
    int rejected = raw_connect(f); uint8_t byte;
    assert(gof_read(rejected, &byte, 1) <= 0); tcp_fd_close(rejected);
    assert(atomic_load(&((tcp_instance *)f->b->user)->shared.stats.source_rejected) == 1);
    f->a->ops->close_connection(f->a, &f->ba); peer_removed(f);
    bv = fipsd_tcp_stats_snapshot(f->b);
    assert(bv.packets_sent == 1 && bv.bytes_sent == 114 && bv.connections_accepted == 1);
    assert(bv.connections_rejected == 1 && bv.source_rejected == 1 && !bv.pool_inbound);
    assert(!atomic_load(&f->a->shared->stats.connections_rejected));
    puts("lifecycle: connect/read/reply, owned send, asymmetric MTU, inbound/source/outbound caps and counters: ok");
    /* Reconnect after close on a fresh four-tuple; partial/coalesced socket I/O. */
    int raw = raw_connect(f);
    assert(gof_write(raw, msg, 1) == 1 && gof_write(raw, msg + 1, 2) == 2);
    uint8_t both[225]; memcpy(both, msg + 3, 111); memcpy(both + 111, msg, 114);
    assert(gof_write(raw, both, sizeof both) == sizeof both);
    e = event(f->eb, FIPSD_TRANSPORT_PACKET); assert(e->size == 114); remote = e->address; free(e);
    e = event(f->eb, FIPSD_TRANSPORT_PACKET); assert(e->size == 114); free(e);
    uint8_t oversize[4] = {0, 0, 255, 255}; assert(gof_write(raw, oversize, sizeof oversize) == 4);
    peer_removed(f); tcp_fd_close(raw);
    assert(atomic_load(&f->b->shared->diagnostics.malformed) == 1);
    raw = raw_connect(f); assert(gof_write(raw, msg, 3) == 3); tcp_fd_close(raw); peer_removed(f);
    assert(atomic_load(&f->b->shared->diagnostics.malformed) == 2);
    bv = fipsd_tcp_stats_snapshot(f->b);
    assert(bv.recv_errors == 2 && bv.packets_recv == 3 && bv.bytes_recv == 1508);
    puts("real stream: partial/coalesced frames, oversized prefix and partial EOF rejected: ok");
    /* A peer that never reads eventually backpressures the sole writer; send
     * remains bounded/nonblocking and stop must cancel the parked writer. */
    raw = raw_connect(f);
    assert(gof_write(raw, msg, sizeof msg) == sizeof msg);
    e = event(f->eb, FIPSD_TRANSPORT_PACKET); remote = e->address; free(e);
    size_t queued = 0;
    while (queued < 100000 && f->b->ops->send(f->b, &remote, msg, sizeof msg) == 0) ++queued;
    assert(queued > 0 && queued < 100000 && atomic_load(&f->b->shared->diagnostics.dropped) > 0);
    assert(atomic_load(&f->b->shared->stats.send_errors) > 0);
    /* Committed mid-read cancellation: watch this accepted socket, rather than
     * using a warm-up delay. Its reader is idle after the complete msg1. */
    tcp_instance *b = f->b->user;
    nsync_mu_lock(&b->mu); tcp_conn *conn = tcp_pool_find(b, &remote); assert(conn); int fd = conn->fd; nsync_mu_unlock(&b->mu);
    pthread_mutex_lock(&park_mu); watch_fd = fd; watch_events = 1; parked = false; pthread_mutex_unlock(&park_mu);
    pthread_t notifier; assert(pthread_create(&notifier, NULL, cancel_when_parked, f->b) == 0);
    assert(gof_write(raw, msg, 1) == 1); /* force a new park with a partial prefix */
    go_blocking_submit(join_thread, &notifier);
    tcp_fd_close(raw);
    puts("backpressure: bounded owned queue refuses; mid-read partial-frame cancellation: ok");
    cancel_connect(f);
    assert(go_channel_send(f->done, f) == 0);
}
/* Lane frames carry their tag in the last byte: stalled 3, control 2, data 1. */
enum { LANE_BIG = 60000, LANE_SMALL = 114 };
static void lane_frame(uint8_t *p, size_t n, uint8_t tag) { frame(p, n); p[n - 1] = tag; }
/* Hold the writer inside one frame larger than both 4 KiB socket buffers can
 * take, so nothing more leaves either lane until the far end reads. The park
 * trace on the connection's fd, not a delay, says the writer is there. */
static void stall_writer(fixture *f, const fips_transport_addr *remote, int fd, const uint8_t *big)
{
    pthread_mutex_lock(&park_mu); watch_fd = fd; watch_events = 2; parked = false; pthread_mutex_unlock(&park_mu);
    assert(f->b->ops->send(f->b, remote, big, LANE_BIG) == 0);
    go_blocking_submit(wait_parked, NULL);
}
static size_t fill(fixture *f, const fips_transport_addr *remote, fipsd_transport_lane lane, uint8_t tag)
{
    uint8_t bytes[LANE_SMALL]; lane_frame(bytes, sizeof bytes, tag);
    size_t n = 0;
    while (n < 1024 && fipsd_transport_send_lane(f->b, lane, remote, bytes, sizeof bytes) == 0) ++n;
    return n;
}
/* The stalled frame, then every control frame, then the data queued before
 * them: what the far end must read once it reads again. */
static void expect_order(int raw, uint8_t *buf, size_t controls, size_t data)
{
    size_t n = 0;
    assert(fipsd_stream_read(raw, buf, UINT16_MAX, &n) == 1 && n == LANE_BIG && buf[n - 1] == 3);
    for (size_t i = 0; i < controls + data; ++i) {
        assert(fipsd_stream_read(raw, buf, UINT16_MAX, &n) == 1 && n == LANE_SMALL);
        assert(buf[n - 1] == (i < controls ? 2 : 1));
    }
}
static void lanes(void *arg)
{
    fixture *f = arg; tcp_instance *b = f->b->user;
    fipsd_transport_diagnostics *d = &b->shared.diagnostics;
    const size_t depth = b->config.send_queue_depth;
    uint8_t msg[114] = {1, 0, 110, 0}, *big = malloc(LANE_BIG), *buf = malloc(UINT16_MAX);
    assert(big && buf); lane_frame(big, LANE_BIG, 3);
    int raw = raw_connect(f);
    assert(gof_write(raw, msg, sizeof msg) == sizeof msg);
    fipsd_transport_event *e = event(f->eb, FIPSD_TRANSPORT_PACKET);
    fips_transport_addr remote = e->address; free(e);
    nsync_mu_lock(&b->mu); tcp_conn *conn = tcp_pool_find(b, &remote); assert(conn); int fd = conn->fd; nsync_mu_unlock(&b->mu);
    /* A full data lane refuses data but still takes control frames, up to
     * their own bound, and the writer sends every one before that data. */
    stall_writer(f, &remote, fd, big);
    assert(fill(f, &remote, FIPSD_TRANSPORT_LANE_DATA, 1) == depth);
    assert(fill(f, &remote, FIPSD_TRANSPORT_LANE_CONTROL, 2) == FIPSD_TCP_CONTROL_DEPTH);
    assert(atomic_load(&d->data_dropped) == 1 && atomic_load(&d->control_dropped) == 1);
    assert(atomic_load(&d->dropped) == 2 && atomic_load(&b->shared.stats.send_errors) == 2);
    expect_order(raw, buf, FIPSD_TCP_CONTROL_DEPTH, depth);
    assert(atomic_load(&d->control_sent) == FIPSD_TCP_CONTROL_DEPTH);
    puts("lanes: full data lane still queues control, written before the queued data; per-lane refusals: ok");
    /* Deliberate close drains both lanes in the same order, then closes. */
    stall_writer(f, &remote, fd, big);
    assert(fill(f, &remote, FIPSD_TRANSPORT_LANE_DATA, 1) == depth);
    assert(fill(f, &remote, FIPSD_TRANSPORT_LANE_CONTROL, 2) == FIPSD_TCP_CONTROL_DEPTH);
    f->b->ops->close_connection(f->b, &remote);
    expect_order(raw, buf, FIPSD_TCP_CONTROL_DEPTH, depth);
    size_t n = 0; assert(fipsd_stream_read(raw, buf, UINT16_MAX, &n) == 0);
    pool_drained(b); tcp_fd_close(raw);
    assert(atomic_load(&d->control_sent) == 2 * FIPSD_TCP_CONTROL_DEPTH);
    assert(atomic_load(&b->shared.stats.packets_sent) == 2 * (1 + FIPSD_TCP_CONTROL_DEPTH + depth));
    free(big); free(buf);
    puts("lanes: deliberate close drains control, then data, then EOF: ok");
    assert(go_channel_send(f->done, f) == 0);
}
static void admission_limits(void *arg)
{
    fixture *f = arg; uint8_t msg[114] = {1, 0, 110, 0};
    assert(f->dial->ops->send(f->dial, &f->ba, msg, sizeof msg) == -1);
    assert(!atomic_load(&f->dial->shared->stats.send_errors)); /* NotConnected, not enqueue failure */
    assert(f->a->ops->connect_async(f->a, &f->ba) == 0);
    free(event(f->ea, FIPSD_TRANSPORT_CONNECTED));
    assert(f->a->ops->send(f->a, &f->ba, msg, sizeof msg) == 0);
    free(event(f->eb, FIPSD_TRANSPORT_PACKET));
    int rejected = raw_connect(f); uint8_t byte;
    assert(gof_read(rejected, &byte, 1) <= 0); tcp_fd_close(rejected);
    fipsd_transport_stats_view b = fipsd_tcp_stats_snapshot(f->b);
    assert(b.connections_rejected == 1 && b.source_rejected == 0 && b.connections_accepted == 1);
    /* Both caps are at one: upstream's global gate wins, not the source gate. */
    tcp_endpoint *items = NULL; size_t n = 0; assert(tcp_resolve(&f->ba, true, &items, &n));
    tcp_endpoint reserved = items[0]; free(items);
    if (reserved.address.ss_family == AF_INET) ((struct sockaddr_in *)&reserved.address)->sin_port = 0;
    else ((struct sockaddr_in6 *)&reserved.address)->sin6_port = 0;
    int fd = socket(reserved.address.ss_family, SOCK_STREAM, IPPROTO_TCP); assert(fd >= 0);
    assert(bind(fd, (struct sockaddr *)&reserved.address, reserved.len) == 0);
    assert(getsockname(fd, (struct sockaddr *)&reserved.address, &reserved.len) == 0);
    fips_transport_addr refused; assert(tcp_endpoint_addr(&reserved, &refused));
    tcp_fd_close(fd); /* a closed loopback port replies with RST on both OSes */
    assert(f->dial->ops->connect_async(f->dial, &refused) == 0);
    free(event(f->ed, FIPSD_TRANSPORT_DISCONNECTED)); /* real ECONNREFUSED */
    fipsd_transport_stats_view d = fipsd_tcp_stats_snapshot(f->dial);
    assert(d.connect_refused == 1 && !d.connect_timeouts && !d.send_errors && !d.pool_outbound);
    puts("accounting: global-before-source gate, NotConnected, real refused connect: ok");
    assert(go_channel_send(f->done, f) == 0);
}
static void deadlines(void *arg)
{
    fixture *f = arg;
    int raw = raw_connect(f);
    peer_removed(f); tcp_fd_close(raw); /* no first frame */
    raw = raw_connect(f);
    uint8_t msg[114] = {1, 0, 110, 0};
    assert(gof_write(raw, msg, sizeof msg) == sizeof msg);
    free(event(f->eb, FIPSD_TRANSPORT_PACKET));
    peer_removed(f); tcp_fd_close(raw); /* no subsequent frame */
    assert(atomic_load(&((tcp_instance *)f->b->user)->shared.diagnostics.deadlines) == 2);
    assert(atomic_load(&((tcp_instance *)f->b->user)->shared.stats.recv_errors) == 0);
    puts("inbound deadlines: silent first frame and post-frame idle reclaim slots without sleeps: ok");
    assert(go_channel_send(f->done, f) == 0);
}
static void notification_pressure(void *arg)
{
    fixture *f = arg;
    free(event(f->ea, FIPSD_TRANSPORT_BECAME_PRESENT));
    for (unsigned i = 0; i < 64; ++i) {
        fipsd_transport_event *e = calloc(1, sizeof *e); assert(e);
        e->kind = FIPSD_TRANSPORT_BECAME_PRESENT; e->id.value = 77;
        assert(go_channel_try_send(f->ea, e) == 0);
    }
    assert(f->a->ops->connect_async(f->a, &f->ba) == 0);
    peer_removed(f); /* the unpublished outbound connection has closed its fd */
    f->a->ops->close_connection(f->a, &f->ba); /* cancel its backpressured exit event */
    tcp_instance *a = f->a->user;
    for (;;) {
        nsync_mu_lock(&a->mu); bool empty = !a->connections; nsync_mu_unlock(&a->mu);
        if (empty) break;
        (void)receive(a->changed);
    }
    for (unsigned i = 0; i < 64; ++i) {
        fipsd_transport_event *e = receive(f->ea); assert(e && e->id.value == 77); free(e);
    }
    void *v = NULL; assert(go_channel_try_receive(f->ea, &v) != 0);
    assert(!fipsd_tcp_connected_count(f->a));
    puts("control backpressure: unpublished connect tears down; explicit close cancels exit event; no stale CONNECTED: ok");
    assert(go_channel_send(f->done, f) == 0);
}
static void lifecycle(const char *bind_addr, unsigned mode)
{
    fipsd_runtime_config rc; fipsd_runtime_config_default(&rc); rc.workers = 2; rc.handle_signals = false;
    fipsd_runtime *rt = fipsd_runtime_create(&rc); assert(rt);
    fixture f = {.ea = go_channel_create(64), .eb = go_channel_create(64), .ed = go_channel_create(64), .done = go_channel_create(1)};
    fipsd_tcp_config c; fipsd_tcp_config_default(&c); c.bind_addr = bind_addr; c.max_outbound_connections = 1;
    c.max_inbound_connections = c.max_inbound_per_source = 0;
    c.mtu = 1280; c.recv_buf_size = c.send_buf_size = 4096;
    f.a = fipsd_tcp_create((fips_transport_id){1}, "a", &c);
    if (mode == 1) c.first_frame_ms = c.idle_ms = 100;
    c.mtu = mode == 4 ? 60000 : 128; c.max_outbound_connections = 0; c.max_inbound_connections = mode == 3 ? 1 : 2; c.max_inbound_per_source = 1;
    f.b = fipsd_tcp_create((fips_transport_id){2}, "b", &c);
    c.bind_addr = NULL; c.max_outbound_connections = 1; f.dial = fipsd_tcp_create((fips_transport_id){3}, "dial", &c);
    assert(f.a && f.b && f.dial);
    fipsd_subsystem sub = {.name = "tcp-test", .start = start, .stop = stop, .user = &f};
    assert(!fipsd_runtime_register(rt, &sub) && !fipsd_runtime_start(rt));
    assert(receive(fipsd_tcp_bound(f.a)) && receive(fipsd_tcp_bound(f.b)) && receive(fipsd_tcp_bound(f.dial)));
    assert(fipsd_tcp_address(f.b, &f.ba));
    assert(fipsd_runtime_spawn(rt, mode == 4 ? lanes : mode == 3 ? admission_limits : mode == 2 ? notification_pressure : mode == 1 ? deadlines : scenario, &f) == 0 && receive(f.done));
    fipsd_runtime_shutdown(rt); assert(fipsd_runtime_run(rt) == FIPSD_RUN_OK);
    assert(!atomic_load(&f.a->shared->stats.pool_outbound) && !atomic_load(&f.b->shared->stats.pool_inbound));
    fipsd_runtime_destroy(rt); fipsd_tcp_destroy(f.a); fipsd_tcp_destroy(f.b); fipsd_tcp_destroy(f.dial);
    GoChannel *channels[] = {f.ea, f.eb, f.ed, f.done};
    for (size_t i = 0; i < 4; ++i) { void *v; while (go_channel_try_receive(channels[i], &v) == 0) free(v); go_channel_unref(channels[i]); }
}
int main(void)
{
    (void)fipsd_test_tmpdir(NULL); framing(); config(); lifecycle("127.0.0.1:0", 0); lifecycle("[::1]:0", 0);
    lifecycle("127.0.0.1:0", 1); lifecycle("127.0.0.1:0", 2); lifecycle("127.0.0.1:0", 3);
    lifecycle("127.0.0.1:0", 4); return 0;
}
