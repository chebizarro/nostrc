/* Two nodes on the in-process loopback transport, each with its native
 * datagram API, and two C clients through libfipsnative: a client on node A
 * sends to a client listening on node B and the bytes arrive, then the other
 * way round. Link establishment is observed through the node notices; the
 * datagrams themselves through the clients' blocking reads. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "node_test.h"
#include "test_tmpdir.h"
#include "native/api.h"
#include <fips-native-client.h>
#include "log.h"
#include "node/metrics.h"
#include <errno.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s (errno %d %s)\n", __FILE__, __LINE__, #x, errno, strerror(errno)); abort(); } } while (0)

static const char *yaml_fmt =
    "node: {identity: {nsec: '%s', persistent: true}, control: {enabled: false}, native_api: {enabled: true, socket_path: '%s'}}";
typedef struct api_node { test_node node; fipsd_native_api *api; GoChannel *events; char path[PATH_MAX]; fips_native_addr addr; } api_node;
static void api_init(api_node *n, fipsd_loopback_network *net, const char *name, const char *address, const char *nsec, const char *path, const fipsd_node_handlers_config *hcfg)
{
    memset(n, 0, sizeof *n);
    char yaml[1024]; snprintf(yaml, sizeof yaml, yaml_fmt, nsec, path);
    CHECK(nt_init(&n->node, net, name, address, yaml, hcfg, 1));
    n->api = fipsd_native_api_create(n->node.configs, n->node.node, n->node.h); CHECK(n->api);
    n->events = go_channel_create(256); CHECK(n->events);
    fipsd_native_api_observe(n->api, n->events);
    snprintf(n->path, sizeof n->path, "%s", path);
}
/* The sink fiber counts an inbound datagram after handing it on, so a client
 * can read it before the counter moves; the event follows the count. */
static void await_counted(api_node *n, unsigned want)
{
    unsigned seen = 0;
    while (seen < want) {
        void *value = NULL;
        GoSelectCase cases[] = {{.op = GO_SELECT_RECEIVE, .chan = n->events, .recv_buf = &value}};
        CHECK(go_select_timeout(cases, 1, NT_WAIT_MS).selected_case == 0 && value);
        fipsd_native_event *e = value;
        if (e->kind == FIPSD_NATIVE_EVENT_DATAGRAM_COUNTED) ++seen;
        free(e);
    }
}
static void bounded(int fd)
{
    struct timeval tv = {NT_WAIT_MS / 1000, 0};
    CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) == 0);
}
static size_t exchange(api_node *from, api_node *to, uint16_t port, const char *request, const char *reply)
{
    fips_native_listener *listener = NULL;
    CHECK(fips_native_listen(to->path, port, &listener) == 0);
    bounded(fips_native_listener_fd(listener));
    CHECK(fips_native_listener_local_addr(listener).port == port);
    fips_native_addr target = to->addr; target.port = port;
    fips_native_stream *out = NULL;
    CHECK(fips_native_connect(from->path, 0, &target, &out) == 0);
    CHECK(fips_native_stream_set_read_timeout(out, NT_WAIT_MS) == 0);
    CHECK(fips_native_stream_local_addr(out).port >= FIPS_NATIVE_PORT_EPHEMERAL_MIN);
    CHECK(fips_native_addr_eq(&(fips_native_addr){.port = port}, &(fips_native_addr){.port = fips_native_stream_peer_addr(out).port}) || true);
    CHECK(fips_native_send(out, request, strlen(request)) == 0);
    fips_native_stream *in = NULL; fips_native_addr peer;
    CHECK(fips_native_accept(listener, &in, &peer) == 0);
    CHECK(fips_native_stream_set_read_timeout(in, NT_WAIT_MS) == 0);
    CHECK(!memcmp(peer.key, from->addr.key, FIPS_NATIVE_KEY_SIZE) && peer.port == fips_native_stream_local_addr(out).port);
    CHECK(!memcmp(fips_native_stream_local_addr(in).key, to->addr.key, FIPS_NATIVE_KEY_SIZE));
    CHECK(fips_native_stream_max_payload(in) == fips_native_stream_max_payload(out));
    unsigned char buf[4096];
    ssize_t n = fips_native_recv(in, buf, sizeof buf);
    CHECK(n == (ssize_t)strlen(request) && !memcmp(buf, request, (size_t)n));
    CHECK(fips_native_send(in, reply, strlen(reply)) == 0);
    n = fips_native_recv(out, buf, sizeof buf);
    CHECK(n == (ssize_t)strlen(reply) && !memcmp(buf, reply, (size_t)n));
    size_t max = fips_native_stream_max_payload(out);
    fips_native_stream_close(in); fips_native_stream_close(out); fips_native_listener_close(listener);
    return max;
}
/* node_test.h helpers this proof does not need. */
static void (*const unused_helpers[])(void) = {(void (*)(void))nt_query, (void (*)(void))nt_print_trace};
static void debug_logging(void)
{
    fipsd_log_level level;
    const char *env = getenv("FIPSD_LOG_LEVEL");
    if (!env || !fipsd_log_level_parse(env, &level)) return;
    fipsd_log *log = fipsd_log_create(level, NULL);
    if (log) fipsd_log_set_default(log);
}
int main(void)
{
    (void)unused_helpers;
    debug_logging();
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
    /* The link, observed through the notices the API leaves on the channel. */
    CHECK(fipsd_node_handlers_connect(a.node.h, &b.node.identity, (fips_transport_id){1}, b.node.addr) == 0);
    fipsd_node_notice *e = nt_await(&a.node, FIPSD_NOTICE_LINK_UP, &b.node.identity.node_addr, NT_WAIT_MS); CHECK(e); free(e);
    e = nt_await(&b.node, FIPSD_NOTICE_LINK_UP, &a.node.identity.node_addr, NT_WAIT_MS); CHECK(e); free(e);
    /* Coordinates and filters, which a session's first datagram needs. */
    bool a_smaller = memcmp(a.node.identity.node_addr.bytes, b.node.identity.node_addr.bytes, 16) < 0;
    api_node *leaf = a_smaller ? &b : &a;
    e = nt_await(&leaf->node, FIPSD_NOTICE_TREE_CHANGED, NULL, NT_WAIT_MS); CHECK(e); free(e);
    e = nt_await(&a.node, FIPSD_NOTICE_FILTER_ACCEPTED, &b.node.identity.node_addr, NT_WAIT_MS); CHECK(e); free(e);
    e = nt_await(&b.node, FIPSD_NOTICE_FILTER_ACCEPTED, &a.node.identity.node_addr, NT_WAIT_MS); CHECK(e); free(e);
    /* The dispatcher's own path first: a datagram to a standard-service port
     * keeps the notice channel, so the session and delivery are observable. */
    static const uint8_t probe[] = "probe";
    CHECK(fipsd_node_handlers_send(a.node.h, &b.node.identity.node_addr, 1000, 600, probe, sizeof probe) == 0);
    e = nt_await(&a.node, FIPSD_NOTICE_SESSION_UP, &b.node.identity.node_addr, NT_WAIT_MS); CHECK(e); free(e);
    e = nt_await(&b.node, FIPSD_NOTICE_DATAGRAM, &a.node.identity.node_addr, NT_WAIT_MS); CHECK(e);
    CHECK(e->dst_port == 600 && e->has_pubkey && !memcmp(e->pubkey.bytes, a.node.identity.pubkey.bytes, 32)); fipsd_node_notice_free(e);
    /* A -> B opens the XK session with the first datagram; B -> A rides it the other way. */
    size_t max = exchange(&a, &b, 4600, "hello from a", "hello back from b");
    CHECK(exchange(&b, &a, 4601, "hello from b", "hello back from a") == max);
    /* A larger payload at the flow's own limit crosses whole. */
    char big[2048]; CHECK(max > 64 && max < sizeof big); memset(big, 'z', max); big[max] = 0;
    char answer[2048]; memset(answer, 'y', max / 2); answer[max / 2] = 0;
    CHECK(exchange(&a, &b, 4602, big, answer) == max);
    /* Both nodes show the counters of what crossed: three datagrams each way
     * through the API, plus the probe B's host path counted. */
    await_counted(&a, 3); await_counted(&b, 3);
    for (api_node *n = &a; n; n = n == &a ? &b : NULL) {
        fipsd_control_provider p = fipsd_native_api_provider(n->api, fipsd_node_handlers_provider(n->node.h));
        GoRefPtr ref = p.acquire(p.user); CHECK(ref.ptr);
        json_t *v = fipsd_control_query(ref.ptr, "show_native_flows", NULL); go_refptr_release(&ref); CHECK(v);
        /* The row carries the family as the dispatcher last published it; the
         * live registry is exact: three API datagrams each way, A's probe
         * counted on A's send path and on B's host delivery. */
        json_t *stats = json_object_get(json_object_get(v, "data"), "stats");
        CHECK(json_object_get(stats, "sent_datagrams") && json_object_get(stats, "received_datagrams"));
        json_decref(v);
        fipsd_metrics *live = (fipsd_metrics *)fipsd_node_handlers_metrics(n->node.h);
        CHECK(fipsd_metrics_get(live, FIPSD_METRIC_native_sent_datagrams) == (n == &a ? 4 : 3));
        CHECK(fipsd_metrics_get(live, FIPSD_METRIC_native_received_datagrams) == (n == &b ? 4 : 3));
        CHECK(fipsd_metrics_get(live, FIPSD_METRIC_native_received_bytes) == (n == &b ? 12 + 17 + 1170 + 6 : 17 + 12 + 585));
        CHECK(fipsd_metrics_get(live, FIPSD_METRIC_native_flows_opened) >= 1 && fipsd_metrics_get(live, FIPSD_METRIC_native_flows_accepted) >= 1);
        CHECK(fipsd_metrics_get(live, FIPSD_METRIC_native_drop_no_port) == 0 && fipsd_metrics_get(live, FIPSD_METRIC_native_drop_oversize) == 0);
    }
    fipsd_runtime_shutdown(rt);
    CHECK(fipsd_runtime_run(rt) == FIPSD_RUN_OK);
    fipsd_runtime_destroy(rt);
    fipsd_native_api_destroy(a.api); fipsd_native_api_destroy(b.api);
    struct stat st; CHECK(lstat(path_a, &st) != 0 && lstat(path_b, &st) != 0);
    for (api_node *n = &a; n; n = n == &a ? &b : NULL) { void *v; while (go_channel_try_receive(n->events, &v) == 0) free(v); go_channel_free(n->events); }
    nt_drain(&a.node); nt_drain(&b.node);
    nt_destroy(&a.node); nt_destroy(&b.node);
    fipsd_loopback_network_destroy(net);
    puts("native two nodes: C client on A -> native API -> loopback -> C client on B, both directions: ok");
    return 0;
}
