/* nostrc-e3ps5: a flow whose client never asks for framing (upstream's Rust
 * client, here a raw line-protocol client) is lossless behind a deliberately
 * slow reader. On Linux that is SOCK_SEQPACKET's waiting writer; on macOS the
 * datagram pair's feeder parks on a NOTE_LOWAT write wait sized to the
 * datagram instead of dropping at ENOBUFS (native/api.c feed_datagram).
 *
 * One node with debug commands; inbound datagrams enter through `arrive`,
 * the same registry delivery and flow queue the node's receive path uses, so
 * every stage is deterministic. Stages:
 *   1. a reader stalled until a burst of the flow queue's capacity is queued,
 *      its buffer a few datagrams: every datagram is read, in order;
 *   2. lockstep rounds of the same (thousands of datagrams, the feeder parked
 *      and woken by the reader every round);
 *   3. a free-running reader with a two-datagram buffer trailing the sender
 *      by half the queue, the sender bounded by the queue's capacity so the
 *      queue never overflows: lossless while every read races a parked
 *      feeder;
 *   macOS only:
 *   4. the overflow fallback: a datagram larger than the client's whole
 *      buffer, which no kernel wake ever reports, parks the feeder; the queue
 *      overflowing sheds it and the queue behind it (counted), the flow stays
 *      usable and parking resumes;
 *   5. teardown releases a parked feeder and its queue (client close), and
 *      shutdown releases another;
 *   6. FIPSD_NATIVE_DGRAM_PARK=0 restores the counted drop.
 * Waits are observer events and bounded blocking reads; nothing sleeps. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "node_test.h"
#include "test_tmpdir.h"
#include "native/api.h"
#include "native/protocol.h"
#include "native/seqpacket.h"
#include "node/metrics.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s (errno %d %s)\n", __FILE__, __LINE__, #x, errno, strerror(errno)); abort(); } } while (0)
#define WAIT_MS 30000
#define PAYLOAD 1000u
#define ROUNDS 200u
#define FREE_RUN 3000u
static const char PEER[] = "npub1sjlh2c3x9w7kjsqg2ay080n2lff2uvt325vpan33ke34rn8l5jcqawh57m";
#define REMOTE 4242u

#if defined(__FreeBSD__)
int main(void) { puts("skip: a FreeBSD datagram flow still drops at a full client (nostrc-e3ps5)"); return 77; }
#else
/* ---- a raw line-protocol client ---------------------------------------------- */
static int raw_open(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    CHECK(strlen(path) < sizeof addr.sun_path);
    memcpy(addr.sun_path, path, strlen(path) + 1);
    CHECK(connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0);
    struct timeval tv = {WAIT_MS / 1000, 0};
    CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) == 0 && setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) == 0);
#ifdef SO_NOSIGPIPE
    int one = 1;
    CHECK(setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one) == 0);
#endif
    return fd;
}
static void raw_send(int fd, const char *line)
{
    size_t len = strlen(line), at = 0;
    while (at < len) {
        ssize_t n = send(fd, line + at, len - at, FIPSD_NATIVE_MSG_NOSIGNAL);
        CHECK(n > 0);
        at += (size_t)n;
    }
    CHECK(send(fd, "\n", 1, FIPSD_NATIVE_MSG_NOSIGNAL) == 1);
}
/* One reply line; the descriptor its message carried, if any, in *fd_out. */
static json_t *raw_reply(int fd, int *fd_out)
{
    static unsigned char buf[FIPSD_NATIVE_MAX_COMMAND * 2];
    size_t len = 0;
    if (fd_out) *fd_out = -1;
    for (;;) {
        unsigned char *nl = memchr(buf, '\n', len);
        if (nl) {
            json_error_t e;
            json_t *v = json_loadb((const char *)buf, (size_t)(nl - buf), 0, &e);
            CHECK(v && (size_t)(nl - buf) + 1 == len);
            return v;
        }
        struct iovec iov = {.iov_base = buf + len, .iov_len = sizeof buf - len};
        union { struct cmsghdr header; unsigned char bytes[CMSG_SPACE(sizeof(int))]; } control;
        memset(&control, 0, sizeof control);
        struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control.bytes, .msg_controllen = sizeof control.bytes};
        ssize_t n;
        do n = recvmsg(fd, &msg, 0); while (n < 0 && errno == EINTR);
        CHECK(n > 0 && !(msg.msg_flags & MSG_CTRUNC));
        for (struct cmsghdr *h = CMSG_FIRSTHDR(&msg); h; h = CMSG_NXTHDR(&msg, h)) {
            if (h->cmsg_level != SOL_SOCKET || h->cmsg_type != SCM_RIGHTS) continue;
            int got;
            memcpy(&got, CMSG_DATA(h), sizeof got);
            if (fd_out && *fd_out < 0) {
                *fd_out = got;
            } else {
                close(got);
            }
        }
        len += (size_t)n;
    }
}
static json_t *field(json_t *v, const char *key) { return json_object_get(json_object_get(v, "data"), key); }
static bool is_ok(json_t *v) { return !strcmp(json_string_value(json_object_get(v, "status")), "ok"); }

/* ---- the node ------------------------------------------------------------------ */
typedef struct api_node { test_node node; fipsd_native_api *api; GoChannel *events; char path[PATH_MAX]; int rpc; } api_node;
static const char *yaml_fmt =
    "node: {identity: {nsec: '%s', persistent: true}, control: {enabled: false},"
    " native_api: {enabled: true, socket_path: '%s', max_flows: 8, debug_commands: true}}";
static void api_init(api_node *n, fipsd_loopback_network *net, const char *name, const char *address, const char *nsec,
                     const char *path, const fipsd_node_handlers_config *hcfg)
{
    memset(n, 0, sizeof *n);
    char yaml[1024];
    snprintf(yaml, sizeof yaml, yaml_fmt, nsec, path);
    CHECK(nt_init(&n->node, net, name, address, yaml, hcfg, 1));
    n->api = fipsd_native_api_create(n->node.configs, n->node.node, n->node.h);
    CHECK(n->api);
    n->events = go_channel_create(16384);
    CHECK(n->events);
    fipsd_native_api_observe(n->api, n->events);
    snprintf(n->path, sizeof n->path, "%s", path);
    n->rpc = -1;
}
static uint64_t metric(api_node *n, fipsd_metric id)
{ return fipsd_metrics_get((fipsd_metrics *)fipsd_node_handlers_metrics(n->node.h), id); }
static void drain_events(api_node *n)
{
    void *v;
    while (go_channel_try_receive(n->events, &v) == 0) free(v);
}
/* Waits for `count` events of `kind` on `flow`, discarding the rest. */
static void await_events(api_node *n, fipsd_native_event_kind kind, uint64_t flow, unsigned count)
{
    while (count) {
        void *v = NULL;
        GoSelectCase cases[] = {{.op = GO_SELECT_RECEIVE, .chan = n->events, .recv_buf = &v}};
        if (go_select_timeout(cases, 1, WAIT_MS).selected_case != 0 || !v)
            fprintf(stderr, "no event %d for flow %llu (%u still awaited)\n", (int)kind, (unsigned long long)flow, count);
        CHECK(v);
        const fipsd_native_event *e = v;
        if (e->kind == kind && e->flow == flow) --count;
        free(v);
    }
}
static unsigned count_events(api_node *n, fipsd_native_event_kind kind, uint64_t flow)
{
    unsigned seen = 0;
    void *v;
    while (go_channel_try_receive(n->events, &v) == 0) {
        const fipsd_native_event *e = v;
        if (e->kind == kind && e->flow == flow) ++seen;
        free(v);
    }
    return seen;
}
/* A flow from a client that never asks for framing; its receive buffer
 * shrunk to `rcvbuf`. Returns the client's descriptor. */
static int open_flow(api_node *n, unsigned local, int rcvbuf, uint64_t *id)
{
    char line[256];
    snprintf(line, sizeof line, "{\"command\":\"connect\",\"params\":{\"peer\":\"%s\",\"remote_port\":%u,\"local_port\":%u}}", PEER, REMOTE, local);
    int c = raw_open(n->path), fd;
    raw_send(c, line);
    json_t *v = raw_reply(c, &fd);
    close(c);
    CHECK(is_ok(v) && fd >= 0);
    *id = (uint64_t)json_integer_value(field(v, "flow_id"));
    CHECK(json_integer_value(field(v, "max_payload")) >= (json_int_t)PAYLOAD);
    json_decref(v);
    int type = 0;
    socklen_t tl = sizeof type;
    CHECK(getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &tl) == 0 && type == FIPSD_NATIVE_SOCK_TYPE);
    CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf) == 0);
    struct timeval tv = {WAIT_MS / 1000, 0};
    CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) == 0);
    return fd;
}
static void fill(uint8_t *buf, uint32_t seq, size_t len)
{
    memcpy(buf, &seq, sizeof seq);
    for (size_t i = sizeof seq; i < len; ++i) buf[i] = (uint8_t)(seq * 7u + i);
}
static size_t length_of(uint32_t seq) { return 64 + (seq * 389u) % (PAYLOAD - 63); }
/* One inbound datagram for the flow on `local`; returns the outcome text. */
static const char *arrive(api_node *n, unsigned local, const uint8_t *data, size_t len)
{
    static char line[FIPSD_NATIVE_MAX_COMMAND];
    static char outcome[96];
    static const char hex[] = "0123456789abcdef";
    int at = snprintf(line, sizeof line, "{\"command\":\"arrive\",\"params\":{\"peer\":\"%s\",\"src_port\":%u,\"dst_port\":%u,\"data\":\"", PEER, REMOTE, local);
    CHECK(at > 0 && (size_t)at + 2 * len + 8 < sizeof line);
    for (size_t i = 0; i < len; ++i) {
        line[at++] = hex[data[i] >> 4];
        line[at++] = hex[data[i] & 15];
    }
    snprintf(line + at, sizeof line - (size_t)at, "\"}}");
    if (n->rpc < 0) n->rpc = raw_open(n->path);
    raw_send(n->rpc, line);
    json_t *v = raw_reply(n->rpc, NULL);
    CHECK(is_ok(v));
    snprintf(outcome, sizeof outcome, "%s", json_string_value(field(v, "outcome")));
    json_decref(v);
    return outcome;
}
static void expect_read(int fd, uint32_t seq, size_t len)
{
    static uint8_t got[PAYLOAD + 1], want[PAYLOAD];
    ssize_t n;
    do n = recv(fd, got, sizeof got, 0); while (n < 0 && errno == EINTR);
    fill(want, seq, len);
    if (n != (ssize_t)len || memcmp(got, want, len)) {
        uint32_t s = 0;
        if (n >= 4) memcpy(&s, got, sizeof s);
        fprintf(stderr, "expected datagram %u (%zu bytes), read %zd bytes (seq %u)\n", seq, len, n, s);
        abort();
    }
}

/* ---- 1, 2: a stalled reader, once and in lockstep rounds ----------------------- */
static void stalled_rounds(api_node *n, unsigned local, unsigned rounds, const char *what)
{
    uint64_t flow;
    int fd = open_flow(n, local, 4096, &flow);
    const uint32_t queue = (uint32_t)fipsd_native_api_limits(n->api)->per_flow;
    const uint64_t client_before = metric(n, FIPSD_METRIC_native_drop_client_buffer_full);
    const uint64_t queue_before = metric(n, FIPSD_METRIC_native_drop_flow_queue_full);
    static uint8_t buf[PAYLOAD];
    uint32_t seq = 0;
    unsigned parked = 0;
    drain_events(n);
    for (unsigned r = 0; r < rounds; ++r) {
        uint32_t first = seq;
#if FIPSD_NATIVE_DGRAM_PARK
        /* The last round's later parks; the await below must see this round's. */
        parked += count_events(n, FIPSD_NATIVE_EVENT_FLOW_PARKED, flow);
#endif
        for (uint32_t i = 0; i < queue; ++i, ++seq) {
            fill(buf, seq, PAYLOAD);
            CHECK(!strcmp(arrive(n, local, buf, PAYLOAD), "delivered"));
        }
#if FIPSD_NATIVE_DGRAM_PARK
        /* Four 1000-byte datagrams fill the client: the feeder waits. */
        await_events(n, FIPSD_NATIVE_EVENT_FLOW_PARKED, flow, 1);
        ++parked;
#endif
        for (uint32_t s = first; s < seq; ++s) expect_read(fd, s, PAYLOAD);
    }
#if FIPSD_NATIVE_DGRAM_PARK
    parked += count_events(n, FIPSD_NATIVE_EVENT_FLOW_PARKED, flow);
    CHECK(count_events(n, FIPSD_NATIVE_EVENT_FLOW_DROPPED, flow) == 0);
#endif
    CHECK(metric(n, FIPSD_METRIC_native_drop_client_buffer_full) == client_before);
    CHECK(metric(n, FIPSD_METRIC_native_drop_flow_queue_full) == queue_before);
    close(fd);
    await_events(n, FIPSD_NATIVE_EVENT_FLOW_RELEASED, flow, 1);
    printf("  %s: %u x %u datagrams behind a 4 KiB stalled reader, all read in order (feeder parks seen: %u)\n", what, rounds, queue, parked);
}

/* ---- 3: a free-running slow reader ------------------------------------------- */
/* The reader trails the sender by LAG datagrams, so the client's buffer is
 * full whenever it reads and every read races a parked feeder. */
#define LAG 8u
typedef struct reader { int fd; uint32_t count; pthread_mutex_t mu; pthread_cond_t cv; uint32_t read, sent; } reader;
static void *read_all(void *arg)
{
    reader *r = arg;
    for (uint32_t s = 0; s < r->count; ++s) {
        pthread_mutex_lock(&r->mu);
        while (r->sent < r->count && r->sent < s + LAG) pthread_cond_wait(&r->cv, &r->mu);
        pthread_mutex_unlock(&r->mu);
        expect_read(r->fd, s, length_of(s));
        pthread_mutex_lock(&r->mu);
        r->read = s + 1;
        pthread_cond_broadcast(&r->cv);
        pthread_mutex_unlock(&r->mu);
    }
    return NULL;
}
static void free_running(api_node *n, unsigned local)
{
    uint64_t flow;
    reader r = {.fd = open_flow(n, local, 2048, &flow), .count = FREE_RUN};
    pthread_mutex_init(&r.mu, NULL);
    pthread_cond_init(&r.cv, NULL);
    const uint32_t queue = (uint32_t)fipsd_native_api_limits(n->api)->per_flow;
    const uint64_t client_before = metric(n, FIPSD_METRIC_native_drop_client_buffer_full);
    const uint64_t queue_before = metric(n, FIPSD_METRIC_native_drop_flow_queue_full);
    drain_events(n);
    pthread_t th;
    CHECK(pthread_create(&th, NULL, read_all, &r) == 0);
    static uint8_t buf[PAYLOAD];
    unsigned parked = 0;
    for (uint32_t seq = 0; seq < FREE_RUN; ++seq) {
        /* At most the queue's capacity in flight (queued, held by the feeder
         * or in the client's buffer), so the queue itself never overflows. */
        pthread_mutex_lock(&r.mu);
        while (seq - r.read >= queue) pthread_cond_wait(&r.cv, &r.mu);
        pthread_mutex_unlock(&r.mu);
        fill(buf, seq, length_of(seq));
        CHECK(!strcmp(arrive(n, local, buf, length_of(seq)), "delivered"));
        pthread_mutex_lock(&r.mu);
        r.sent = seq + 1;
        pthread_cond_broadcast(&r.cv);
        pthread_mutex_unlock(&r.mu);
        if (seq % 256 == 255) parked += count_events(n, FIPSD_NATIVE_EVENT_FLOW_PARKED, flow);
    }
    CHECK(pthread_join(th, NULL) == 0);
    parked += count_events(n, FIPSD_NATIVE_EVENT_FLOW_PARKED, flow);
    CHECK(metric(n, FIPSD_METRIC_native_drop_client_buffer_full) == client_before);
    CHECK(metric(n, FIPSD_METRIC_native_drop_flow_queue_full) == queue_before);
    close(r.fd);
    await_events(n, FIPSD_NATIVE_EVENT_FLOW_RELEASED, flow, 1);
    pthread_mutex_destroy(&r.mu);
    pthread_cond_destroy(&r.cv);
#if FIPSD_NATIVE_DGRAM_PARK
    CHECK(parked > 0);
#endif
    printf("  free-running: %u datagrams of 64..%u bytes through a 2 KiB reader %u behind, all read in order (feeder parks seen: %u)\n",
           FREE_RUN, PAYLOAD, LAG, parked);
}

#if FIPSD_NATIVE_DGRAM_PARK
/* ---- 4, 5: the overflow fallback, teardown ------------------------------------- */
static GoChannel *reaped;
static void before_reap(void *user, uint64_t flow)
{
    (void)user;
    uint64_t *v = malloc(sizeof *v);
    CHECK(v);
    *v = flow;
    CHECK(go_channel_try_send(reaped, v) == 0);
}
static void await_reap(uint64_t flow)
{
    for (;;) {
        void *v = NULL;
        GoSelectCase cases[] = {{.op = GO_SELECT_RECEIVE, .chan = reaped, .recv_buf = &v}};
        CHECK(go_select_timeout(cases, 1, WAIT_MS).selected_case == 0 && v);
        bool match = *(uint64_t *)v == flow;
        free(v);
        if (match) return;
    }
}
static void overflow_fallback(api_node *n, unsigned local)
{
    uint64_t flow;
    /* 600 bytes: a 1000-byte datagram never fits, so no recv ever wakes it. */
    int fd = open_flow(n, local, 600, &flow);
    const uint32_t queue = (uint32_t)fipsd_native_api_limits(n->api)->per_flow;
    const uint64_t client_before = metric(n, FIPSD_METRIC_native_drop_client_buffer_full);
    const uint64_t queue_before = metric(n, FIPSD_METRIC_native_drop_flow_queue_full);
    static uint8_t buf[PAYLOAD];
    drain_events(n);
    fill(buf, 0, PAYLOAD);
    CHECK(!strcmp(arrive(n, local, buf, PAYLOAD), "delivered"));
    await_events(n, FIPSD_NATIVE_EVENT_FLOW_PARKED, flow, 1);
    /* The feeder holds the first; the queue takes exactly its capacity. */
    for (uint32_t s = 1; s <= queue; ++s) {
        fill(buf, s, PAYLOAD);
        CHECK(!strcmp(arrive(n, local, buf, PAYLOAD), "delivered"));
    }
    fill(buf, queue + 1, PAYLOAD);
    CHECK(!strcmp(arrive(n, local, buf, PAYLOAD), "dropped: queue full"));
    /* The overflow sheds the held datagram and the queue behind it. */
    await_events(n, FIPSD_NATIVE_EVENT_FLOW_DROPPED, flow, queue + 1);
    CHECK(metric(n, FIPSD_METRIC_native_drop_client_buffer_full) - client_before == queue + 1);
    CHECK(metric(n, FIPSD_METRIC_native_drop_flow_queue_full) - queue_before == 1);
    /* The flow is not hung: a datagram that fits is delivered... */
    fill(buf, 1000, 100);
    CHECK(!strcmp(arrive(n, local, buf, 100), "delivered"));
    expect_read(fd, 1000, 100);
    /* ...and parking resumed: the next one that cannot fit waits again. */
    fill(buf, 1001, PAYLOAD);
    CHECK(!strcmp(arrive(n, local, buf, PAYLOAD), "delivered"));
    await_events(n, FIPSD_NATIVE_EVENT_FLOW_PARKED, flow, 1);
    CHECK(count_events(n, FIPSD_NATIVE_EVENT_FLOW_DROPPED, flow) == 0);
    /* 5: the client closing releases the parked feeder and what it holds,
     * with more queued behind it. */
    for (uint32_t s = 0; s < 3; ++s) CHECK(!strcmp(arrive(n, local, buf, PAYLOAD), "delivered"));
    close(fd);
    await_events(n, FIPSD_NATIVE_EVENT_FLOW_RELEASED, flow, 1);
    await_reap(flow);
    printf("  overflow fallback: a datagram larger than the client's buffer parked, the overflow shed %u, "
           "the flow stayed usable, teardown released the parked feeder\n", queue + 1);
}
/* ---- 6: the switch ---------------------------------------------------------------- */
static void switched_off(api_node *n, unsigned local)
{
    uint64_t flow;
    int fd = open_flow(n, local, 4096, &flow);
    const uint32_t queue = (uint32_t)fipsd_native_api_limits(n->api)->per_flow;
    const uint64_t client_before = metric(n, FIPSD_METRIC_native_drop_client_buffer_full);
    static uint8_t buf[PAYLOAD];
    drain_events(n);
    for (uint32_t s = 0; s < queue; ++s) {
        fill(buf, s, PAYLOAD);
        CHECK(!strcmp(arrive(n, local, buf, PAYLOAD), "delivered"));
    }
    /* Nothing is read: the client holds four, so the old path must refuse
     * and count the rest at once (stage 1 shows the parking path keeps them).
     * The first drop is enough; parking would have produced none. */
    for (bool dropped_one = false; !dropped_one;) {
        void *v = NULL;
        GoSelectCase cases[] = {{.op = GO_SELECT_RECEIVE, .chan = n->events, .recv_buf = &v}};
        CHECK(go_select_timeout(cases, 1, WAIT_MS).selected_case == 0 && v);
        const fipsd_native_event *e = v;
        CHECK(!(e->kind == FIPSD_NATIVE_EVENT_FLOW_PARKED && e->flow == flow));
        dropped_one = e->kind == FIPSD_NATIVE_EVENT_FLOW_DROPPED && e->flow == flow;
        free(v);
    }
    uint64_t dropped = metric(n, FIPSD_METRIC_native_drop_client_buffer_full) - client_before;
    CHECK(dropped > 0);
    CHECK(count_events(n, FIPSD_NATIVE_EVENT_FLOW_PARKED, flow) == 0);
    close(fd);
    await_events(n, FIPSD_NATIVE_EVENT_FLOW_RELEASED, flow, 1);
    printf("  FIPSD_NATIVE_DGRAM_PARK=0: a stalled 4 KiB client lost datagrams to counted drops (>= %llu), no feeder parked\n", (unsigned long long)dropped);
}
#endif

/* node_test.h helpers this test does not need. */
static void (*const unused_helpers[])(void) = {(void (*)(void))nt_query, (void (*)(void))nt_print_trace,
                                               (void (*)(void))nt_await, (void (*)(void))nt_identity};
int main(void)
{
    (void)unused_helpers;
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *tmp = fipsd_test_tmpdir("/b.sock");
    char path_a[PATH_MAX], path_b[PATH_MAX];
    snprintf(path_a, sizeof path_a, "%s/a.sock", tmp);
    snprintf(path_b, sizeof path_b, "%s/b.sock", tmp);
    fipsd_runtime_config rcfg;
    fipsd_runtime_config_default(&rcfg);
    rcfg.workers = 2; rcfg.handle_signals = false; rcfg.grace_ms = 20000;
    fipsd_runtime *rt = fipsd_runtime_create(&rcfg);
    CHECK(rt);
    fipsd_loopback_network *net = fipsd_loopback_network_create();
    CHECK(net);
    fipsd_node_handlers_config hcfg;
    fipsd_node_handlers_config_default(&hcfg);
    hcfg.peer_capacity = 4; hcfg.tree_peer_capacity = 4; hcfg.session_capacity = 4; hcfg.cache_entries = 64;
    hcfg.notice_capacity = 256; hcfg.tick_ms = 200;
    api_node a, b;
    api_init(&a, net, "A", "loop:a", "1111111111111111111111111111111111111111111111111111111111111111", path_a, &hcfg);
#if FIPSD_NATIVE_DGRAM_PARK
    reaped = go_channel_create(64);
    CHECK(reaped);
    fipsd_native_api_before_flow_reap(a.api, before_reap, NULL);
    /* Read once, when the API is created. */
    CHECK(setenv("FIPSD_NATIVE_DGRAM_PARK", "0", 1) == 0);
#endif
    api_init(&b, net, "B", "loop:b", "2222222222222222222222222222222222222222222222222222222222222222", path_b, &hcfg);
#if FIPSD_NATIVE_DGRAM_PARK
    CHECK(unsetenv("FIPSD_NATIVE_DGRAM_PARK") == 0);
#endif
    CHECK(fipsd_runtime_register(rt, fipsd_node_subsystem(a.node.node)) == 0);
    CHECK(fipsd_runtime_register(rt, fipsd_node_subsystem(b.node.node)) == 0);
    CHECK(fipsd_runtime_register(rt, fipsd_native_api_subsystem(a.api)) == 0);
    CHECK(fipsd_runtime_register(rt, fipsd_native_api_subsystem(b.api)) == 0);
    CHECK(fipsd_runtime_start(rt) == 0);
    puts("native datagram flows behind a slow reader:");
    stalled_rounds(&a, 5000, 1, "stalled burst");
    stalled_rounds(&a, 5001, ROUNDS, "lockstep rounds");
    free_running(&a, 5002);
#if FIPSD_NATIVE_DGRAM_PARK
    overflow_fallback(&a, 5003);
    switched_off(&b, 5004);
    /* Shutdown with a feeder parked on a client that is not reading. */
    uint64_t flow;
    int parked = open_flow(&a, 5005, 600, &flow);
    static uint8_t buf[PAYLOAD];
    drain_events(&a);
    fill(buf, 0, PAYLOAD);
    CHECK(!strcmp(arrive(&a, 5005, buf, PAYLOAD), "delivered"));
    await_events(&a, FIPSD_NATIVE_EVENT_FLOW_PARKED, flow, 1);
#endif
    if (a.rpc >= 0) close(a.rpc);
    if (b.rpc >= 0) close(b.rpc);
    fipsd_runtime_shutdown(rt);
    CHECK(fipsd_runtime_run(rt) == FIPSD_RUN_OK);
#if FIPSD_NATIVE_DGRAM_PARK
    close(parked);
#endif
    fipsd_runtime_destroy(rt);
    fipsd_native_api_destroy(a.api);
    fipsd_native_api_destroy(b.api);
    drain_events(&a);
    drain_events(&b);
    go_channel_free(a.events);
    go_channel_free(b.events);
#if FIPSD_NATIVE_DGRAM_PARK
    void *v;
    while (go_channel_try_receive(reaped, &v) == 0) free(v);
    go_channel_free(reaped);
#endif
    nt_drain(&a.node);
    nt_drain(&b.node);
    nt_destroy(&a.node);
    nt_destroy(&b.node);
    fipsd_loopback_network_destroy(net);
    puts("native datagram park: ok");
    return 0;
}
#endif
