/* Regression for nostrc-pxwaj: a burst of application datagrams larger than
 * every bounded queue on the originating path (native flow buffer, dispatcher
 * mailbox, crypto packet pool, worker queues) crosses two in-process nodes
 * through the real native API without a single drop on that path. The client
 * writes as fast as its socket accepts: the daemon's flow reader must park on
 * the dispatcher mailbox and the dispatcher must gate admission on packet-pool
 * headroom, so pressure reaches the client as EAGAIN rather than a drop.
 *
 * The far node has no such back-pressure by design: a full flow queue costs
 * the slow client a datagram, never the node its receive path (native/link.c).
 * So the far client reads every datagram exactly once or the far node counts
 * it exactly once as drop_flow_queue_full. Which of the two happens depends on
 * scheduling alone: a far reader descheduled for about a millisecond at line
 * rate fills its queue (nostrc-aqyad), so the count is not asserted. A second
 * burst makes that path deterministic: its reader takes nothing until the far
 * node has counted a drop.
 *
 * Beyond the flow queue nothing is lost on any platform (nostrc-6ljk8): a full
 * client buffer parks the far writer until the client drains, so
 * drop_client_buffer_full stays zero. Linux gets that from SOCK_SEQPACKET;
 * on macOS and FreeBSD libfipsnative asks for a length-framed SOCK_STREAM flow,
 * whose writer the kernel wakes when the reader drains (a datagram pair there
 * refuses a full client with ENOBUFS and no writability event). A third burst
 * proves it with a deliberately slow reader: a receive buffer shrunk to a few
 * datagrams and nothing read until the far node has dispatched the whole
 * burst, which is exactly the flow queue's capacity, so the node accepts every
 * datagram and the client must then read every one of them.
 *
 * After each burst one more datagram must arrive, so the far flow survived the
 * pressure. Readiness is observed through notices, native API events and
 * blocking reads; there are no sleeps and no timing assertions. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "node_test.h"
#include "test_tmpdir.h"
#include "native/api.h"
#include "native/seqpacket.h"
#include <fips-native-client.h>
#include "node/metrics.h"
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s (errno %d %s)\n", __FILE__, __LINE__, #x, errno, strerror(errno)); abort(); } } while (0)
#define BURST 1024u
#define PAYLOAD 1000u
#define PORT 4700

static const char *yaml_fmt =
    "node: {identity: {nsec: '%s', persistent: true}, control: {enabled: false}, native_api: {enabled: true, socket_path: '%s'}}";
typedef struct api_node { test_node node; fipsd_native_api *api; char path[PATH_MAX]; fips_native_addr addr; } api_node;
static void api_init(api_node *n, fipsd_loopback_network *net, const char *name, const char *address, const char *nsec, const char *path, const fipsd_node_handlers_config *hcfg)
{
    memset(n, 0, sizeof *n);
    char yaml[1024]; snprintf(yaml, sizeof yaml, yaml_fmt, nsec, path);
    CHECK(nt_init(&n->node, net, name, address, yaml, hcfg, 1));
    n->api = fipsd_native_api_create(n->node.configs, n->node.node, n->node.h); CHECK(n->api);
    snprintf(n->path, sizeof n->path, "%s", path);
}
static uint64_t metric(api_node *n, fipsd_metric id)
{ return fipsd_metrics_get((fipsd_metrics *)fipsd_node_handlers_metrics(n->node.h), id); }
/* What the far node counted instead of handing a datagram to its client. */
static uint64_t far_drops(api_node *n)
{ return metric(n, FIPSD_METRIC_native_drop_flow_queue_full) + metric(n, FIPSD_METRIC_native_drop_client_buffer_full); }
typedef struct reader {
    fips_native_stream *in;
    api_node *far;
    GoChannel *stall; /* the far API's events: read nothing until the far node counts a drop */
    uint64_t dropped_before;
    uint8_t seen[BURST / 8];
    unsigned received, duplicates, bad;
} reader;
static unsigned dropped(const reader *r) { return (unsigned)(far_drops(r->far) - r->dropped_before); }
static void *drain(void *arg)
{
    reader *r = arg;
    uint8_t buf[PAYLOAD + 1];
    /* The far API emits one event per inbound datagram after counting it, so
     * this wait is event-driven; its bound only ends a failed run, which the
     * accounting then reports. */
    while (r->stall && !dropped(r)) {
        void *value = NULL;
        GoSelectCase cases[] = {{.op = GO_SELECT_RECEIVE, .chan = r->stall, .recv_buf = &value}};
        if (go_select_timeout(cases, 1, NT_WAIT_MS).selected_case != 0 || !value) return NULL;
        free(value);
    }
    /* A drop is counted while the client still has queued datagrams to read,
     * so every datagram is read or counted by the time this loop ends. */
    while (r->received + dropped(r) < BURST) {
        ssize_t n = fips_native_recv(r->in, buf, sizeof buf);
        if (n == -EAGAIN) break; /* the bounded read deadline: the accounting reports it */
        if (n != (ssize_t)PAYLOAD) { ++r->bad; break; }
        uint32_t seq; memcpy(&seq, buf, sizeof seq);
        if (seq >= BURST) { ++r->bad; break; }
        for (size_t i = sizeof seq; i < PAYLOAD; ++i) if (buf[i] != (uint8_t)(seq + i)) { ++r->bad; break; }
        uint8_t bit = (uint8_t)(1u << (seq % 8));
        if (r->seen[seq / 8] & bit) ++r->duplicates; else { r->seen[seq / 8] |= bit; ++r->received; }
    }
    return NULL;
}
/* Whole datagram or nothing; a full client socket (EAGAIN, or ENOBUFS on a
 * Darwin datagram pair) is the daemon's back-pressure: wait for writability,
 * never drop. */
static void blast(fips_native_stream *out, uint32_t count)
{
    uint8_t buf[PAYLOAD];
    int fd = fips_native_stream_fd(out);
    for (uint32_t seq = 0; seq < count; ++seq) {
        memcpy(buf, &seq, sizeof seq);
        for (size_t i = sizeof seq; i < PAYLOAD; ++i) buf[i] = (uint8_t)(seq + i);
        for (;;) {
            int rc = fips_native_send(out, buf, sizeof buf);
            if (rc == 0) break;
            CHECK(rc == -EAGAIN || rc == -ENOBUFS);
            struct pollfd p = {.fd = fd, .events = POLLOUT};
            int ready;
            do { ready = poll(&p, 1, (int)NT_WAIT_MS); } while (ready < 0 && errno == EINTR);
            CHECK(ready == 1);
        }
    }
}
/* One burst, its far reader stalled first when `stall` is set; returns how
 * many the far node counted instead of delivering. Once every datagram is
 * accounted for nothing is queued on the far flow, so the next must arrive. */
static unsigned burst(fips_native_stream *out, fips_native_stream *in, api_node *far, GoChannel *stall, reader *r)
{
    memset(r, 0, sizeof *r);
    r->in = in; r->far = far; r->stall = stall; r->dropped_before = far_drops(far);
    pthread_t thread; CHECK(pthread_create(&thread, NULL, drain, r) == 0);
    blast(out, BURST);
    CHECK(pthread_join(thread, NULL) == 0);
    unsigned lost = dropped(r);
    CHECK(r->bad == 0 && r->duplicates == 0 && r->received + lost == BURST);
    uint8_t buf[16];
    CHECK(fips_native_send(out, "fin", 3) == 0);
    CHECK(fips_native_recv(in, buf, sizeof buf) == 3 && !memcmp(buf, "fin", 3));
    return lost;
}
/* The deliberately slow reader: a receive buffer of a few datagrams, and
 * nothing read until the far node has dispatched all `count` of them (one
 * DATAGRAM_COUNTED event each). `count` is the flow queue's capacity, so the
 * node delivers every one; the far writer then waits on the client's
 * buffer instead of dropping, and every datagram must be read exactly once. */
static void slow_reader_burst(fips_native_stream *out, fips_native_stream *in, api_node *far, GoChannel *events, uint32_t count)
{
    void *stale;
    while (go_channel_try_receive(events, &stale) == 0) free(stale);
    const uint64_t received_before = metric(far, FIPSD_METRIC_native_received_datagrams);
    const uint64_t queue_before = metric(far, FIPSD_METRIC_native_drop_flow_queue_full);
    const uint64_t client_before = metric(far, FIPSD_METRIC_native_drop_client_buffer_full);
    int small = 4096;
    CHECK(setsockopt(fips_native_stream_fd(in), SOL_SOCKET, SO_RCVBUF, &small, sizeof small) == 0);
    blast(out, count);
    for (uint32_t counted = 0; counted < count;) {
        void *value = NULL;
        GoSelectCase cases[] = {{.op = GO_SELECT_RECEIVE, .chan = events, .recv_buf = &value}};
        CHECK(go_select_timeout(cases, 1, NT_WAIT_MS).selected_case == 0 && value);
        const fipsd_native_event *e = value;
        if (e->kind == FIPSD_NATIVE_EVENT_DATAGRAM_COUNTED && e->port == PORT) ++counted;
        free(value);
    }
    CHECK(metric(far, FIPSD_METRIC_native_received_datagrams) - received_before == count);
    uint8_t buf[PAYLOAD + 1], seen[BURST / 8] = {0};
    for (uint32_t got = 0; got < count; ++got) {
        CHECK(fips_native_recv(in, buf, sizeof buf) == (ssize_t)PAYLOAD);
        uint32_t seq; memcpy(&seq, buf, sizeof seq);
        CHECK(seq < count && !(seen[seq / 8] & (1u << (seq % 8)))); /* each once; the mesh may reorder */
        seen[seq / 8] |= (uint8_t)(1u << (seq % 8));
        for (size_t i = sizeof seq; i < PAYLOAD; ++i) CHECK(buf[i] == (uint8_t)(seq + i));
    }
    CHECK(metric(far, FIPSD_METRIC_native_drop_client_buffer_full) == client_before);
    CHECK(metric(far, FIPSD_METRIC_native_drop_flow_queue_full) == queue_before);
    CHECK(fips_native_send(out, "fin", 3) == 0);
    CHECK(fips_native_recv(in, buf, sizeof buf) == 3 && !memcmp(buf, "fin", 3));
}
/* node_test.h helpers this proof does not need. */
static void (*const unused_helpers[])(void) = {(void (*)(void))nt_query, (void (*)(void))nt_print_trace};
int main(void)
{
    (void)unused_helpers;
    const char *tmp = fipsd_test_tmpdir("/b.sock");
    char path_a[PATH_MAX], path_b[PATH_MAX];
    snprintf(path_a, sizeof path_a, "%s/a.sock", tmp); snprintf(path_b, sizeof path_b, "%s/b.sock", tmp);
    fipsd_runtime_config rcfg; fipsd_runtime_config_default(&rcfg); rcfg.workers = 2; rcfg.handle_signals = false; rcfg.grace_ms = 20000;
    fipsd_runtime *rt = fipsd_runtime_create(&rcfg); CHECK(rt);
    fipsd_loopback_network *net = fipsd_loopback_network_create(); CHECK(net);
    fipsd_node_handlers_config hcfg; fipsd_node_handlers_config_default(&hcfg);
    hcfg.peer_capacity = 4; hcfg.tree_peer_capacity = 4; hcfg.session_capacity = 4; hcfg.cache_entries = 64;
    hcfg.notice_capacity = 256; hcfg.tick_ms = 200; hcfg.tree_announce_min_interval_ms = 100; hcfg.bloom_debounce_ms = 100;
    api_node a, b;
    api_init(&a, net, "A", "loop:a", "1111111111111111111111111111111111111111111111111111111111111111", path_a, &hcfg);
    api_init(&b, net, "B", "loop:b", "2222222222222222222222222222222222222222222222222222222222222222", path_b, &hcfg);
    GoChannel *events = go_channel_create(256); CHECK(events);
    fipsd_native_api_observe(b.api, events);
    a.node.transport->interface_name = "loop-test"; b.node.transport->interface_name = "loop-test";
    CHECK(fipsd_runtime_register(rt, fipsd_node_subsystem(a.node.node)) == 0);
    CHECK(fipsd_runtime_register(rt, fipsd_node_subsystem(b.node.node)) == 0);
    CHECK(fipsd_runtime_register(rt, fipsd_native_api_subsystem(a.api)) == 0);
    CHECK(fipsd_runtime_register(rt, fipsd_native_api_subsystem(b.api)) == 0);
    CHECK(fipsd_runtime_start(rt) == 0);
    nt_identity(&a.node); nt_identity(&b.node);
    memcpy(a.addr.key, a.node.identity.pubkey.bytes, 32); memcpy(b.addr.key, b.node.identity.pubkey.bytes, 32);
    CHECK(fipsd_node_handlers_connect(a.node.h, &b.node.identity, (fips_transport_id){1}, b.node.addr) == 0);
    fipsd_node_notice *e = nt_await(&a.node, FIPSD_NOTICE_LINK_UP, &b.node.identity.node_addr, NT_WAIT_MS); CHECK(e); free(e);
    e = nt_await(&b.node, FIPSD_NOTICE_LINK_UP, &a.node.identity.node_addr, NT_WAIT_MS); CHECK(e); free(e);
    bool a_smaller = memcmp(a.node.identity.node_addr.bytes, b.node.identity.node_addr.bytes, 16) < 0;
    e = nt_await(&(a_smaller ? &b : &a)->node, FIPSD_NOTICE_TREE_CHANGED, NULL, NT_WAIT_MS); CHECK(e); free(e);
    e = nt_await(&a.node, FIPSD_NOTICE_FILTER_ACCEPTED, &b.node.identity.node_addr, NT_WAIT_MS); CHECK(e); free(e);
    e = nt_await(&b.node, FIPSD_NOTICE_FILTER_ACCEPTED, &a.node.identity.node_addr, NT_WAIT_MS); CHECK(e); free(e);
    /* One flow each way; its first datagram opens the XK session and proves
     * the path before the burst, so the burst measures the data path alone. */
    fips_native_listener *listener = NULL;
    CHECK(fips_native_listen(b.path, PORT, &listener) == 0);
    fips_native_addr target = b.addr; target.port = PORT;
    fips_native_stream *out = NULL;
    CHECK(fips_native_connect(a.path, 0, &target, &out) == 0);
    CHECK(fips_native_stream_set_write_timeout(out, NT_WAIT_MS) == 0);
    CHECK(fips_native_send(out, "open", 4) == 0);
    fips_native_stream *in = NULL; fips_native_addr peer;
    CHECK(fips_native_accept(listener, &in, &peer) == 0);
    CHECK(fips_native_stream_set_read_timeout(in, NT_WAIT_MS) == 0);
    uint8_t buf[16];
    CHECK(fips_native_recv(in, buf, sizeof buf) == 4 && !memcmp(buf, "open", 4));
    CHECK(fips_native_stream_max_payload(out) >= PAYLOAD);
    e = nt_await(&a.node, FIPSD_NOTICE_SESSION_UP, &b.node.identity.node_addr, NT_WAIT_MS); CHECK(e); free(e);
    e = nt_await(&b.node, FIPSD_NOTICE_SESSION_UP, &a.node.identity.node_addr, NT_WAIT_MS); CHECK(e); free(e);
    reader free_running, stalled;
    unsigned lost_free = burst(out, in, &b, NULL, &free_running);
    void *stale;
    while (go_channel_try_receive(events, &stale) == 0) free(stale);
    unsigned lost_stalled = burst(out, in, &b, events, &stalled);
    CHECK(lost_stalled > 0); /* the stalled reader made the far node give up on it */
    const uint32_t queue = (uint32_t)fipsd_native_api_limits(b.api)->per_flow;
    CHECK(queue > 0 && queue <= BURST);
    slow_reader_burst(out, in, &b, events, queue);
    fips_native_stream_close(in); fips_native_stream_close(out); fips_native_listener_close(listener);
    fipsd_runtime_shutdown(rt);
    CHECK(fipsd_runtime_run(rt) == FIPSD_RUN_OK);
    /* Counted after every fiber joined: a client may read a datagram before
     * the far node counts it. The originating node applied back-pressure and
     * dropped nothing: it took every datagram the client wrote (the opener,
     * three bursts and their three liveness datagrams). */
    const uint64_t total = 2u * BURST + queue + 4u;
    CHECK(metric(&a, FIPSD_METRIC_native_sent_datagrams) == total);
    CHECK(metric(&a, FIPSD_METRIC_forwarding_drop_send_error_packets) == 0);
    CHECK(metric(&a, FIPSD_METRIC_forwarding_drop_mtu_exceeded_packets) == 0);
    CHECK(metric(&a, FIPSD_METRIC_forwarding_drop_no_route_packets) == 0);
    CHECK(metric(&a, FIPSD_METRIC_native_drop_flow_queue_full) == 0);
    CHECK(metric(&a, FIPSD_METRIC_native_drop_client_buffer_full) == 0);
    /* The far node counted each arriving datagram exactly once: handed to the
     * flow (received), or dropped at its full queue; the reader saw every
     * drop it was not handed. */
    uint64_t queue_drops = metric(&b, FIPSD_METRIC_native_drop_flow_queue_full);
    uint64_t client_drops = metric(&b, FIPSD_METRIC_native_drop_client_buffer_full);
    CHECK(metric(&b, FIPSD_METRIC_native_received_datagrams) + queue_drops == total);
    CHECK(queue_drops + client_drops == (uint64_t)lost_free + lost_stalled);
    CHECK(client_drops == 0); /* a full client buffer parks the far writer instead */
    for (api_node *n = &a; n; n = n == &a ? &b : NULL) {
        CHECK(metric(n, FIPSD_METRIC_native_drop_arrival_queue_full) == 0);
        CHECK(metric(n, FIPSD_METRIC_native_drop_backlog_full) == 0);
        CHECK(metric(n, FIPSD_METRIC_native_drop_no_port) == 0);
        CHECK(metric(n, FIPSD_METRIC_native_drop_oversize) == 0);
        CHECK(metric(n, FIPSD_METRIC_native_drop_listener_gone) == 0);
    }
    fipsd_runtime_destroy(rt);
    fipsd_native_api_destroy(a.api); fipsd_native_api_destroy(b.api);
    while (go_channel_try_receive(events, &stale) == 0) free(stale);
    go_channel_free(events);
    nt_drain(&a.node); nt_drain(&b.node);
    nt_destroy(&a.node); nt_destroy(&b.node);
    fipsd_loopback_network_destroy(net);
    printf("native burst: 2 x %u x %u-byte datagrams A -> B through the native API with no originating drop; "
           "draining reader read %u (%u counted at the far node), stalled reader read %u (%u counted), "
           "slow reader read all %u with no client-buffer drop: ok\n",
           BURST, PAYLOAD, free_running.received, lost_free, stalled.received, lost_stalled, (unsigned)queue);
    return 0;
}
