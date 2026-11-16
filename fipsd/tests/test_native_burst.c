/* Regression for nostrc-pxwaj: a burst of application datagrams larger than
 * every bounded queue on the originating path (native flow buffer, dispatcher
 * mailbox, crypto packet pool, worker queues) crosses two in-process nodes
 * through the real native API without a single counted drop. The client
 * writes as fast as its socket accepts: the daemon's flow reader must park on
 * the dispatcher mailbox and the dispatcher must gate admission on packet-pool
 * headroom, so pressure reaches the client as EAGAIN/ENOBUFS rather than a
 * drop, and the far client must read every datagram exactly once. Readiness
 * is observed through notices and blocking reads; there are no sleeps and no
 * timing assertions. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "node_test.h"
#include "test_tmpdir.h"
#include "native/api.h"
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
typedef struct reader { fips_native_stream *in; uint8_t seen[BURST / 8]; unsigned received, duplicates, bad; } reader;
static void *drain(void *arg)
{
    reader *r = arg;
    uint8_t buf[PAYLOAD + 1];
    while (r->received < BURST) {
        ssize_t n = fips_native_recv(r->in, buf, sizeof buf);
        if (n == -EAGAIN) break; /* the bounded read deadline: the assertion below reports it */
        if (n != (ssize_t)PAYLOAD) { ++r->bad; break; }
        uint32_t seq; memcpy(&seq, buf, sizeof seq);
        if (seq >= BURST) { ++r->bad; break; }
        for (size_t i = sizeof seq; i < PAYLOAD; ++i) if (buf[i] != (uint8_t)(seq + i)) { ++r->bad; break; }
        uint8_t bit = (uint8_t)(1u << (seq % 8));
        if (r->seen[seq / 8] & bit) ++r->duplicates; else { r->seen[seq / 8] |= bit; ++r->received; }
    }
    return NULL;
}
/* Whole datagram or nothing; a full client socket (EAGAIN, or ENOBUFS on
 * Darwin) is the daemon's back-pressure: wait for writability, never drop. */
static void blast(fips_native_stream *out)
{
    uint8_t buf[PAYLOAD];
    int fd = fips_native_stream_fd(out);
    for (uint32_t seq = 0; seq < BURST; ++seq) {
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
static uint64_t metric(api_node *n, fipsd_metric id)
{ return fipsd_metrics_get((fipsd_metrics *)fipsd_node_handlers_metrics(n->node.h), id); }
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
    reader r = {.in = in};
    pthread_t thread; CHECK(pthread_create(&thread, NULL, drain, &r) == 0);
    blast(out);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(r.bad == 0 && r.duplicates == 0 && r.received == BURST);
    /* Every drop counter on both nodes is zero: the originating path applied
     * back-pressure, the far node delivered through its sink and flow. */
    CHECK(metric(&a, FIPSD_METRIC_native_sent_datagrams) == BURST + 1);
    CHECK(metric(&a, FIPSD_METRIC_forwarding_drop_send_error_packets) == 0);
    CHECK(metric(&a, FIPSD_METRIC_forwarding_drop_mtu_exceeded_packets) == 0);
    CHECK(metric(&a, FIPSD_METRIC_forwarding_drop_no_route_packets) == 0);
    CHECK(metric(&b, FIPSD_METRIC_native_received_datagrams) == BURST + 1);
    for (api_node *n = &a; n; n = n == &a ? &b : NULL) {
        CHECK(metric(n, FIPSD_METRIC_native_drop_flow_queue_full) == 0);
        CHECK(metric(n, FIPSD_METRIC_native_drop_arrival_queue_full) == 0);
        CHECK(metric(n, FIPSD_METRIC_native_drop_backlog_full) == 0);
        CHECK(metric(n, FIPSD_METRIC_native_drop_no_port) == 0);
        CHECK(metric(n, FIPSD_METRIC_native_drop_oversize) == 0);
        CHECK(metric(n, FIPSD_METRIC_native_drop_listener_gone) == 0);
    }
    fips_native_stream_close(in); fips_native_stream_close(out); fips_native_listener_close(listener);
    fipsd_runtime_shutdown(rt);
    CHECK(fipsd_runtime_run(rt) == FIPSD_RUN_OK);
    fipsd_runtime_destroy(rt);
    fipsd_native_api_destroy(a.api); fipsd_native_api_destroy(b.api);
    nt_drain(&a.node); nt_drain(&b.node);
    nt_destroy(&a.node); nt_destroy(&b.node);
    fipsd_loopback_network_destroy(net);
    printf("native burst: %u x %u-byte datagrams A -> B through the native API, zero drops: ok\n", BURST, PAYLOAD);
    return 0;
}
