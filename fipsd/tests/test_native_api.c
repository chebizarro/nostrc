/* The native datagram API subsystem on a live node: the line protocol over
 * the real Unix socket, descriptor hand-off, the debug commands, the
 * descriptor pins, the command cap and refusal sizes, the provider's
 * show_native_flows projection and a self-delivered datagram through the
 * dispatcher (upstream src/native/mod.rs tests, tests/flows.rs and
 * tests/replies.rs). Two nodes in one process: A answers the debug commands,
 * B has the gate closed. Every wait is an observer event or a bounded socket
 * read; nothing sleeps. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "node_test.h"
#include "test_tmpdir.h"
#include "native/api.h"
#include "native/protocol.h"
#include "native/seqpacket.h"
#include "log.h"
#include "node/metrics.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s (errno %d %s)\n", __FILE__, __LINE__, #x, errno, strerror(errno)); abort(); } } while (0)
#define WAIT_MS 30000
static const char PEER[] = "npub1sjlh2c3x9w7kjsqg2ay080n2lff2uvt325vpan33ke34rn8l5jcqawh57m";
static const char OTHER[] = "npub1n9lpnv0592cc2ps6nm0ca3qls642vx7yjsv35rkxqzj2vgds52sqgpverl";

/* ---- a raw line-protocol client, one connection ---------------------------- */
static int raw_open(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0); CHECK(fd >= 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX}; CHECK(strlen(path) < sizeof addr.sun_path);
    memcpy(addr.sun_path, path, strlen(path) + 1);
    CHECK(connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0);
    struct timeval tv = {WAIT_MS / 1000, 0};
    CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) == 0 && setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) == 0);
#ifdef SO_NOSIGPIPE
    int one = 1; (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    return fd;
}
/* One recvmsg: bytes appended to `buf`, descriptors collected (the first kept). */
static ssize_t raw_recv(int fd, unsigned char *buf, size_t cap, int *fd_out)
{
    struct iovec iov = {.iov_base = buf, .iov_len = cap};
    union { struct cmsghdr header; unsigned char bytes[CMSG_SPACE(sizeof(int) * 4)]; } control;
    memset(&control, 0, sizeof control);
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control.bytes, .msg_controllen = sizeof control.bytes};
    ssize_t n;
    do n = recvmsg(fd, &msg, 0); while (n < 0 && errno == EINTR);
    if (n < 0 && errno == ECONNRESET) n = 0;
    if (n < 0) return n;
    for (struct cmsghdr *h = CMSG_FIRSTHDR(&msg); h; h = CMSG_NXTHDR(&msg, h)) {
        if (h->cmsg_level != SOL_SOCKET || h->cmsg_type != SCM_RIGHTS) continue;
        size_t payload = h->cmsg_len - CMSG_LEN(0);
        for (size_t i = 0; i + sizeof(int) <= payload; i += sizeof(int)) {
            int got; memcpy(&got, CMSG_DATA(h) + i, sizeof got);
            if (fd_out && *fd_out < 0) *fd_out = got; else close(got);
        }
    }
    CHECK(!(msg.msg_flags & MSG_CTRUNC));
    return n;
}
static void raw_send(int fd, const char *line)
{
    size_t len = strlen(line), at = 0;
    while (at < len) { ssize_t n = send(fd, line + at, len - at, FIPSD_NATIVE_MSG_NOSIGNAL); CHECK(n > 0); at += (size_t)n; }
    CHECK(send(fd, "\n", 1, FIPSD_NATIVE_MSG_NOSIGNAL) == 1);
}
/* Read one whole reply line; NULL at end of file. The descriptor on the
 * read that completed the line is returned. */
static json_t *raw_reply(int fd, int *fd_out)
{
    static unsigned char buf[FIPSD_NATIVE_MAX_COMMAND * 2];
    size_t len = 0;
    if (fd_out) *fd_out = -1;
    for (;;) {
        unsigned char *nl = memchr(buf, '\n', len);
        if (nl) {
            json_error_t e; json_t *v = json_loadb((const char *)buf, (size_t)(nl - buf), 0, &e);
            CHECK(v);
            CHECK((size_t)(nl - buf) + 1 == len); /* one whole line per sendmsg */
            return v;
        }
        ssize_t n = raw_recv(fd, buf + len, sizeof buf - len, fd_out);
        CHECK(n >= 0);
        if (n == 0) return NULL;
        len += (size_t)n;
    }
}
static json_t *raw_call(int fd, const char *line, int *fd_out) { raw_send(fd, line); return raw_reply(fd, fd_out); }
/* One command on a fresh connection, closed before returning (the shipped client's shape). */
static json_t *ask(const char *path, const char *line, int *fd_out)
{
    int c = raw_open(path); json_t *v = raw_call(c, line, fd_out); close(c); CHECK(v); return v;
}
static const char *str(json_t *v, const char *key) { return json_string_value(json_object_get(v, key)); }
static json_t *data(json_t *v, const char *key) { return json_object_get(json_object_get(v, "data"), key); }
static bool is_ok(json_t *v) { return !strcmp(str(v, "status"), "ok"); }
static const char *err_name(json_t *v) { return json_string_value(data(v, "errno")); }
static json_t *expect_error(json_t *v, const char *errno_name, const char *message_part)
{
    if (is_ok(v) || strcmp(err_name(v), errno_name) || (message_part && !strstr(str(v, "message"), message_part))) {
        char *text = json_dumps(v, 0); fprintf(stderr, "unexpected reply: %s\n", text); free(text); abort();
    }
    return v;
}
/* Read one arrival on a listener descriptor: the message and the flow's descriptor. */
static json_t *raw_accept(int listener, int *flow)
{
    unsigned char buf[4096]; *flow = -1;
    ssize_t n = raw_recv(listener, buf, sizeof buf, flow);
    CHECK(n > 0 && *flow >= 0);
    json_error_t e; json_t *v = json_loadb((const char *)buf, (size_t)n, 0, &e); CHECK(v);
    CHECK(!memchr(buf, '\n', (size_t)n));
    return v;
}
static void bounded(int fd)
{
    struct timeval tv = {WAIT_MS / 1000, 0};
    CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) == 0 && setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) == 0);
#ifdef SO_NOSIGPIPE
    int one = 1; (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
}
static ssize_t read_dgram(int fd, void *buf, size_t cap) { ssize_t n; do n = recv(fd, buf, cap, 0); while (n < 0 && errno == EINTR); return n; }
static void write_dgram(int fd, const void *buf, size_t len) { CHECK(send(fd, buf, len, FIPSD_NATIVE_MSG_NOSIGNAL) == (ssize_t)len); }

/* ---- observer events ------------------------------------------------------ */
typedef struct observer { GoChannel *chan; fipsd_native_event seen[256]; size_t count; } observer;
static bool event_matches(const fipsd_native_event *e, fipsd_native_event_kind kind, uint64_t flow, uint16_t port)
{ return e->kind == kind && (!flow || e->flow == flow) && (!port || e->port == port); }
static void await_event(observer *o, fipsd_native_event_kind kind, uint64_t flow, uint16_t port)
{
    for (size_t i = 0; i < o->count; ++i)
        if (event_matches(&o->seen[i], kind, flow, port)) { memmove(o->seen + i, o->seen + i + 1, (o->count - i - 1) * sizeof *o->seen); --o->count; return; }
    for (;;) {
        void *value = NULL;
        GoSelectCase cases[] = {{.op = GO_SELECT_RECEIVE, .chan = o->chan, .recv_buf = &value}};
        GoSelectResult r = go_select_timeout(cases, 1, WAIT_MS);
        if (r.selected_case != 0 || !value) { fprintf(stderr, "no event kind %d flow %llu port %u within %d ms\n", (int)kind, (unsigned long long)flow, (unsigned)port, WAIT_MS); abort(); }
        fipsd_native_event *e = value;
        bool match = event_matches(e, kind, flow, port);
        if (!match && o->count < 256) o->seen[o->count++] = *e;
        free(e);
        if (match) return;
    }
}
static void drain_events(observer *o)
{
    void *v; while (go_channel_try_receive(o->chan, &v) == 0) free(v);
    o->count = 0;
}

/* ---- the nodes ------------------------------------------------------------ */
typedef struct api_node { test_node node; fipsd_native_api *api; observer events; char path[PATH_MAX]; const char *npub; } api_node;
static const char *yaml_fmt =
    "node: {identity: {nsec: '%s', persistent: true}, control: {enabled: false},"
    " native_api: {enabled: true, socket_path: '%s', pending_per_flow: 4, backlog: 2, max_flows: 8, debug_commands: %s}}";
static void api_init(api_node *n, fipsd_loopback_network *net, const char *name, const char *address, const char *nsec,
                     const char *path, bool debug, const fipsd_node_handlers_config *hcfg)
{
    memset(n, 0, sizeof *n);
    char yaml[1024]; snprintf(yaml, sizeof yaml, yaml_fmt, nsec, path, debug ? "true" : "false");
    CHECK(nt_init(&n->node, net, name, address, yaml, hcfg, 1));
    n->api = fipsd_native_api_create(n->node.configs, n->node.node, n->node.h); CHECK(n->api);
    n->events.chan = go_channel_create(256); CHECK(n->events.chan);
    fipsd_native_api_observe(n->api, n->events.chan);
    snprintf(n->path, sizeof n->path, "%s", path);
}
static json_t *show_native_flows(api_node *n)
{
    fipsd_control_provider p = fipsd_native_api_provider(n->api, fipsd_node_handlers_provider(n->node.h));
    GoRefPtr ref = p.acquire(p.user); CHECK(ref.ptr);
    json_t *out = fipsd_control_query(ref.ptr, "show_native_flows", NULL);
    go_refptr_release(&ref); CHECK(out);
    return out;
}
static char line[FIPSD_NATIVE_MAX_COMMAND + 64];
static const char *connect_line(const char *peer, unsigned remote, unsigned local)
{
    if (local) snprintf(line, sizeof line, "{\"command\":\"connect\",\"params\":{\"peer\":\"%s\",\"remote_port\":%u,\"local_port\":%u}}", peer, remote, local);
    else snprintf(line, sizeof line, "{\"command\":\"connect\",\"params\":{\"peer\":\"%s\",\"remote_port\":%u}}", peer, remote);
    return line;
}
static const char *listen_line(unsigned port) { snprintf(line, sizeof line, "{\"command\":\"listen\",\"params\":{\"local_port\":%u}}", port); return line; }
static const char *stats_line(uint64_t flow) { snprintf(line, sizeof line, "{\"command\":\"stats\",\"params\":{\"flow_id\":%llu}}", (unsigned long long)flow); return line; }
static const char *inject_line(uint64_t flow, const char *hex, unsigned repeat)
{
    if (repeat) snprintf(line, sizeof line, "{\"command\":\"inject\",\"params\":{\"flow_id\":%llu,\"data\":\"%s\",\"repeat\":%u}}", (unsigned long long)flow, hex, repeat);
    else snprintf(line, sizeof line, "{\"command\":\"inject\",\"params\":{\"flow_id\":%llu,\"data\":\"%s\"}}", (unsigned long long)flow, hex);
    return line;
}
static const char *arrive_line(const char *peer, unsigned src, unsigned dst, const char *hex)
{ snprintf(line, sizeof line, "{\"command\":\"arrive\",\"params\":{\"peer\":\"%s\",\"src_port\":%u,\"dst_port\":%u,\"data\":\"%s\"}}", peer, src, dst, hex); return line; }
static uint64_t flow_id_of(json_t *v) { return (uint64_t)json_integer_value(data(v, "flow_id")); }

/* ---- scenarios ------------------------------------------------------------ */
static void test_listeners_and_ports(api_node *a)
{
    int lfd, lfd2; json_t *v = ask(a->path, listen_line(4242), &lfd);
    CHECK(is_ok(v) && lfd >= 0 && json_integer_value(data(v, "local_port")) == 4242);
    CHECK(!strcmp(str(json_object_get(v, "data"), "node"), a->npub) && json_integer_value(data(v, "backlog")) == 2);
    json_decref(v);
    v = ask(a->path, listen_line(4242), NULL); json_decref(expect_error(v, "EADDRINUSE", "already in use"));
    v = ask(a->path, "{\"command\":\"listen\",\"params\":{\"local_port\":0}}", &lfd2);
    CHECK(is_ok(v) && lfd2 >= 0);
    unsigned port = (unsigned)json_integer_value(data(v, "local_port")); CHECK(port >= FIPSD_NATIVE_PORT_EPHEMERAL_MIN); json_decref(v);
    v = ask(a->path, listen_line(port), NULL); json_decref(expect_error(v, "EADDRINUSE", NULL));
    close(lfd); await_event(&a->events, FIPSD_NATIVE_EVENT_LISTENER_RELEASED, 0, 4242);
    close(lfd2); await_event(&a->events, FIPSD_NATIVE_EVENT_LISTENER_RELEASED, 0, (uint16_t)port);
    /* The unbind is what close(listen_fd) means: the port is bindable again. */
    v = ask(a->path, listen_line(4242), &lfd); CHECK(is_ok(v)); json_decref(v);
    close(lfd); await_event(&a->events, FIPSD_NATIVE_EVENT_LISTENER_RELEASED, 0, 4242);
}
static void test_refusals(api_node *a)
{
    json_t *v = ask(a->path, listen_line(256), NULL); json_decref(expect_error(v, "EADDRNOTAVAIL", "standard services"));
    v = ask(a->path, "{not json", NULL); json_decref(expect_error(v, "EINVAL", "invalid request"));
    v = ask(a->path, "{\"command\":\"teleport\"}", NULL); json_decref(expect_error(v, "EINVAL", "teleport"));
    v = ask(a->path, "{\"command\":\"accept\",\"params\":{\"flow_id\":1}}", NULL); json_decref(expect_error(v, "EINVAL", "accept"));
    v = ask(a->path, "{\"command\":\"reject\",\"params\":{\"flow_id\":1}}", NULL); json_decref(expect_error(v, "EINVAL", "reject"));
    v = ask(a->path, "{\"params\":{}}", NULL); json_decref(expect_error(v, "EINVAL", "invalid request"));
    v = ask(a->path, "[1,2]", NULL); json_decref(expect_error(v, "EINVAL", "invalid request"));
    /* A bad peer is refused without holding a port. */
    v = ask(a->path, connect_line("not-an-npub", 4242, 5000), NULL); json_decref(expect_error(v, "EINVAL", "invalid peer"));
    int lfd; v = ask(a->path, listen_line(5000), &lfd); CHECK(is_ok(v)); json_decref(v);
    close(lfd); await_event(&a->events, FIPSD_NATIVE_EVENT_LISTENER_RELEASED, 0, 5000);
    /* An unreasonable repeat and an unknown flow. */
    v = ask(a->path, inject_line(1, "00", 0), NULL); /* flow 1 may or may not exist; only the shape matters below */
    json_decref(v);
    v = ask(a->path, "{\"command\":\"inject\",\"params\":{\"flow_id\":1,\"data\":\"00\",\"repeat\":0}}", NULL); json_decref(expect_error(v, "EINVAL", "repeat"));
    v = ask(a->path, "{\"command\":\"inject\",\"params\":{\"flow_id\":1,\"data\":\"00\",\"repeat\":99999}}", NULL); json_decref(expect_error(v, "EINVAL", "repeat"));
    v = ask(a->path, stats_line(99), NULL); json_decref(expect_error(v, "ENOENT", "no such flow"));
    v = ask(a->path, inject_line(99, "00", 0), NULL); json_decref(expect_error(v, "ENOENT", "no such flow"));
    v = ask(a->path, arrive_line("not-an-npub", 4242, 5000, "00"), NULL); json_decref(expect_error(v, "EINVAL", "invalid peer"));
    /* A datagram for an unheld port is dropped, and says so. */
    v = ask(a->path, arrive_line(PEER, 5000, 9999, "aa"), NULL);
    CHECK(is_ok(v) && !strcmp(str(json_object_get(v, "data"), "outcome"), "dropped: no listener or flow on that port"));
    CHECK(json_is_null(data(v, "flow_id"))); json_decref(v);
}
static void test_connect(api_node *a)
{
    int ffd; json_t *v = ask(a->path, connect_line(PEER, 4242, 5000), &ffd);
    CHECK(is_ok(v) && ffd >= 0); bounded(ffd);
    CHECK(json_integer_value(data(v, "local_port")) == 5000 && json_integer_value(data(v, "remote_port")) == 4242);
    CHECK(!strcmp(str(json_object_get(v, "data"), "peer"), PEER) && !strcmp(str(json_object_get(v, "data"), "node"), a->npub));
    CHECK(json_integer_value(data(v, "max_payload")) == fipsd_native_api_max_payload(a->api));
    uint64_t flow = flow_id_of(v); json_decref(v);
    write_dgram(ffd, "hello", 5);
    await_event(&a->events, FIPSD_NATIVE_EVENT_FLOW_READ, flow, 0);
    v = ask(a->path, stats_line(flow), NULL);
    CHECK(is_ok(v) && json_integer_value(data(v, "rx_datagrams")) == 1 && json_integer_value(data(v, "rx_bytes")) == 5);
    CHECK(json_is_false(data(v, "closed")) && json_integer_value(data(v, "local_port")) == 5000); json_decref(v);
    v = ask(a->path, listen_line(5000), NULL); json_decref(expect_error(v, "EADDRINUSE", NULL));
    /* Closing the descriptor is seen by the daemon, and the port comes back. */
    close(ffd); await_event(&a->events, FIPSD_NATIVE_EVENT_FLOW_RELEASED, flow, 0);
    v = ask(a->path, stats_line(flow), NULL); json_decref(expect_error(v, "ENOENT", NULL));
    v = ask(a->path, connect_line(PEER, 4242, 5000), &ffd); CHECK(is_ok(v)); flow = flow_id_of(v); json_decref(v);
    close(ffd); await_event(&a->events, FIPSD_NATIVE_EVENT_FLOW_RELEASED, flow, 0);
    /* The node's flow ceiling: EMFILE, the one registry refusal other than a taken port. */
    int fds[8]; uint64_t ids[8];
    for (unsigned i = 0; i < 8; ++i) {
        v = ask(a->path, connect_line(PEER, 4242, 5000 + i), &fds[i]); CHECK(is_ok(v)); ids[i] = flow_id_of(v); json_decref(v);
    }
    v = ask(a->path, connect_line(PEER, 4242, 6000), NULL); json_decref(expect_error(v, "EMFILE", "maximum"));
    for (unsigned i = 0; i < 8; ++i) { close(fds[i]); await_event(&a->events, FIPSD_NATIVE_EVENT_FLOW_RELEASED, ids[i], 0); }
}
static void test_arrivals_and_pins(api_node *a)
{
    int lfd, ffd; json_t *v = ask(a->path, listen_line(4242), &lfd); CHECK(is_ok(v)); json_decref(v); bounded(lfd);
    v = ask(a->path, arrive_line(PEER, 5000, 4242, "00ff10"), NULL);
    CHECK(is_ok(v) && !strcmp(str(json_object_get(v, "data"), "outcome"), "announced"));
    uint64_t flow = flow_id_of(v); json_decref(v);
    json_t *m = raw_accept(lfd, &ffd); bounded(ffd);
    CHECK((uint64_t)json_integer_value(json_object_get(m, "flow_id")) == flow);
    CHECK(!strcmp(str(m, "peer"), PEER) && !json_object_get(m, "peer_addr") && !strcmp(str(m, "node"), a->npub));
    CHECK(json_integer_value(json_object_get(m, "local_port")) == 4242 && json_integer_value(json_object_get(m, "remote_port")) == 5000);
    CHECK(json_integer_value(json_object_get(m, "max_payload")) == fipsd_native_api_max_payload(a->api) && json_integer_value(json_object_get(m, "held")) == 1);
    json_decref(m);
    unsigned char buf[64]; CHECK(read_dgram(ffd, buf, sizeof buf) == 3 && buf[0] == 0 && buf[1] == 0xff && buf[2] == 0x10);
    /* A handed-over flow outlives its dropped descriptor until its listener
     * closes: the daemon's copy keeps the socket alive, so an inject still
     * lands and stats still finds the flow open. */
    close(ffd);
    v = ask(a->path, inject_line(flow, "ab", 0), NULL); CHECK(is_ok(v)); json_decref(v);
    v = ask(a->path, stats_line(flow), NULL); CHECK(is_ok(v) && json_is_false(data(v, "closed"))); json_decref(v);
    close(lfd); await_event(&a->events, FIPSD_NATIVE_EVENT_LISTENER_RELEASED, 0, 4242);
    await_event(&a->events, FIPSD_NATIVE_EVENT_FLOW_UNPINNED, flow, 0);
    await_event(&a->events, FIPSD_NATIVE_EVENT_FLOW_RELEASED, flow, 0);
    v = ask(a->path, stats_line(flow), NULL); json_decref(expect_error(v, "ENOENT", NULL));
    /* A handed-over flow closes with its descriptor once its client has written. */
    v = ask(a->path, listen_line(4242), &lfd); CHECK(is_ok(v)); json_decref(v); bounded(lfd);
    v = ask(a->path, arrive_line(PEER, 5000, 4242, "00ff10"), NULL); flow = flow_id_of(v); json_decref(v);
    m = raw_accept(lfd, &ffd); json_decref(m); bounded(ffd);
    write_dgram(ffd, "x", 1);
    await_event(&a->events, FIPSD_NATIVE_EVENT_FLOW_UNPINNED, flow, 0);
    close(ffd); await_event(&a->events, FIPSD_NATIVE_EVENT_FLOW_RELEASED, flow, 0);
    v = ask(a->path, stats_line(flow), NULL); json_decref(expect_error(v, "ENOENT", NULL));
    close(lfd); await_event(&a->events, FIPSD_NATIVE_EVENT_LISTENER_RELEASED, 0, 4242);
    /* A handed-over flow its client still holds outlives its listener and closes with its descriptor. */
    v = ask(a->path, listen_line(4242), &lfd); CHECK(is_ok(v)); json_decref(v); bounded(lfd);
    v = ask(a->path, arrive_line(PEER, 5000, 4242, "00ff10"), NULL); flow = flow_id_of(v); json_decref(v);
    m = raw_accept(lfd, &ffd); json_decref(m); bounded(ffd);
    close(lfd); await_event(&a->events, FIPSD_NATIVE_EVENT_LISTENER_RELEASED, 0, 4242);
    await_event(&a->events, FIPSD_NATIVE_EVENT_FLOW_UNPINNED, flow, 0);
    int lfd2; v = ask(a->path, listen_line(4242), &lfd2); CHECK(is_ok(v)); json_decref(v);
    CHECK(read_dgram(ffd, buf, sizeof buf) == 3);
    v = ask(a->path, inject_line(flow, "ab", 0), NULL); CHECK(is_ok(v)); json_decref(v);
    CHECK(read_dgram(ffd, buf, sizeof buf) == 1 && buf[0] == 0xab);
    close(ffd); await_event(&a->events, FIPSD_NATIVE_EVENT_FLOW_RELEASED, flow, 0);
    close(lfd2); await_event(&a->events, FIPSD_NATIVE_EVENT_LISTENER_RELEASED, 0, 4242);
    /* Every datagram from one peer reaches the one flow it belongs to, in order. */
    v = ask(a->path, listen_line(4242), &lfd); CHECK(is_ok(v)); json_decref(v); bounded(lfd);
    for (int i = 0; i < 3; ++i) {
        v = ask(a->path, arrive_line(PEER, 5000, 4242, "aa"), NULL);
        const char *outcome = str(json_object_get(v, "data"), "outcome");
        CHECK(!strcmp(outcome, "announced") || !strcmp(outcome, "held") || !strcmp(outcome, "delivered"));
        json_decref(v);
    }
    m = raw_accept(lfd, &ffd); CHECK(json_integer_value(json_object_get(m, "local_port")) == 4242); flow = (uint64_t)json_integer_value(json_object_get(m, "flow_id")); json_decref(m); bounded(ffd);
    for (int i = 0; i < 3; ++i) CHECK(read_dgram(ffd, buf, sizeof buf) == 1 && buf[0] == 0xaa);
    /* inject writes separate datagrams, and the arrival to its port is delivered to the flow. */
    v = ask(a->path, inject_line(flow, "00ff10", 3), NULL); CHECK(is_ok(v) && json_integer_value(data(v, "bytes")) == 9); json_decref(v);
    for (int i = 0; i < 3; ++i) CHECK(read_dgram(ffd, buf, sizeof buf) == 3 && buf[1] == 0xff);
    v = ask(a->path, arrive_line(PEER, 5000, 4242, "bb"), NULL);
    CHECK(is_ok(v) && !strcmp(str(json_object_get(v, "data"), "outcome"), "delivered") && json_is_null(data(v, "flow_id"))); json_decref(v);
    CHECK(read_dgram(ffd, buf, sizeof buf) == 1 && buf[0] == 0xbb);
    write_dgram(ffd, "x", 1); await_event(&a->events, FIPSD_NATIVE_EVENT_FLOW_UNPINNED, flow, 0);
    close(ffd); await_event(&a->events, FIPSD_NATIVE_EVENT_FLOW_RELEASED, flow, 0);
    close(lfd); await_event(&a->events, FIPSD_NATIVE_EVENT_LISTENER_RELEASED, 0, 4242);
}
static void test_kept_descriptors(api_node *a)
{
    /* The daemon keeps its copy of the descriptor in a reply until the
     * client's next command on that connection, or the connection's end. */
    int c = raw_open(a->path), ffd;
    json_t *v = raw_call(c, connect_line(PEER, 4242, 4243), &ffd); CHECK(is_ok(v) && ffd >= 0);
    uint64_t flow = flow_id_of(v); json_decref(v);
    close(ffd);
    v = ask(a->path, inject_line(flow, "ab", 0), NULL); CHECK(is_ok(v)); json_decref(v);
    v = ask(a->path, stats_line(flow), NULL); CHECK(is_ok(v) && json_is_false(data(v, "closed"))); json_decref(v);
    v = raw_call(c, stats_line(0), NULL); json_decref(v); /* any command: the hold is over */
    await_event(&a->events, FIPSD_NATIVE_EVENT_FLOW_RELEASED, flow, 0);
    v = ask(a->path, stats_line(flow), NULL); json_decref(expect_error(v, "ENOENT", NULL));
    /* Let go when the connection ends, with the client still holding its copy. */
    v = raw_call(c, connect_line(PEER, 4242, 4243), &ffd); CHECK(is_ok(v)); flow = flow_id_of(v); json_decref(v);
    close(c); await_event(&a->events, FIPSD_NATIVE_EVENT_CONNECTION_CLOSED, 0, 0);
    v = ask(a->path, stats_line(flow), NULL); CHECK(is_ok(v) && json_is_false(data(v, "closed"))); json_decref(v);
    close(ffd); await_event(&a->events, FIPSD_NATIVE_EVENT_FLOW_RELEASED, flow, 0);
    /* A flow closed after the next command ends at once, with the connection still open. */
    c = raw_open(a->path);
    v = raw_call(c, connect_line(PEER, 4242, 4243), &ffd); CHECK(is_ok(v)); flow = flow_id_of(v); json_decref(v);
    v = raw_call(c, stats_line(0), NULL); json_decref(v);
    close(ffd); await_event(&a->events, FIPSD_NATIVE_EVENT_FLOW_RELEASED, flow, 0);
    /* A flow whose reply is never followed by a command ends with the connection. */
    v = raw_call(c, connect_line(PEER, 4242, 4243), &ffd); CHECK(is_ok(v)); flow = flow_id_of(v); json_decref(v);
    close(ffd); close(c);
    await_event(&a->events, FIPSD_NATIVE_EVENT_CONNECTION_CLOSED, 0, 0);
    await_event(&a->events, FIPSD_NATIVE_EVENT_FLOW_RELEASED, flow, 0);
    /* Closing the RPC connection leaves its flows and listeners alone, and
     * the provider shows both. */
    c = raw_open(a->path); int lfd;
    v = raw_call(c, listen_line(4242), &lfd); CHECK(is_ok(v)); json_decref(v);
    v = raw_call(c, connect_line(PEER, 4242, 5000), &ffd); CHECK(is_ok(v)); flow = flow_id_of(v); json_decref(v);
    close(c); await_event(&a->events, FIPSD_NATIVE_EVENT_CONNECTION_CLOSED, 0, 0);
    v = show_native_flows(a);
    json_t *flows = data(v, "flows"), *listeners = data(v, "listeners");
    CHECK(json_array_size(flows) == 1 && json_array_size(listeners) == 1);
    json_t *row = json_array_get(flows, 0);
    CHECK((uint64_t)json_integer_value(json_object_get(row, "flow_id")) == flow && !strcmp(str(row, "peer"), PEER));
    CHECK(strlen(str(row, "peer_addr")) == 32 && json_integer_value(json_object_get(row, "local_port")) == 5000);
    CHECK(json_integer_value(json_object_get(row, "remote_port")) == 4242 && !strcmp(str(row, "state"), "established"));
    CHECK(json_integer_value(json_object_get(row, "queued")) == 0 && json_object_get(row, "age_ms"));
    CHECK(json_integer_value(json_object_get(json_array_get(listeners, 0), "local_port")) == 4242);
    CHECK(json_integer_value(json_object_get(json_array_get(listeners, 0), "backlog")) == 0);
    CHECK(json_integer_value(json_object_get(data(v, "stats"), "flows_opened")) >= 1);
    json_decref(v);
    close(ffd); await_event(&a->events, FIPSD_NATIVE_EVENT_FLOW_RELEASED, flow, 0);
    close(lfd); await_event(&a->events, FIPSD_NATIVE_EVENT_LISTENER_RELEASED, 0, 4242);
    /* Two commands pipelined on one connection are answered in order, the
     * descriptor attached to the line that earned it. */
    c = raw_open(a->path);
    snprintf(line, sizeof line, "{\"command\":\"listen\",\"params\":{\"local_port\":4300}}\n{\"command\":\"stats\",\"params\":{\"flow_id\":0}}");
    raw_send(c, line);
    unsigned char buf[4096]; size_t len = 0; int got = -1; unsigned lines = 0;
    while (lines < 2) {
        int one = -1; ssize_t n = raw_recv(c, buf + len, sizeof buf - len, &one); CHECK(n > 0);
        if (one >= 0) { CHECK(got < 0 && memchr(buf + len, '\n', (size_t)n)); got = one; }
        for (ssize_t i = 0; i < n; ++i) if (buf[len + (size_t)i] == '\n') ++lines;
        len += (size_t)n;
    }
    CHECK(got >= 0 && !strncmp((const char *)buf, "{\"status\":\"ok\"", 14));
    const char *second = strchr((const char *)buf, '\n') + 1;
    CHECK(!strncmp(second, "{\"status\":\"error\"", 17));
    close(got); close(c);
    await_event(&a->events, FIPSD_NATIVE_EVENT_LISTENER_RELEASED, 0, 4300);
}
static void test_debug_gate(api_node *a, api_node *b)
{
    int ffd; json_t *v = ask(b->path, connect_line(PEER, 4242, 0), &ffd); CHECK(is_ok(v)); bounded(ffd);
    uint64_t flow = flow_id_of(v); unsigned local = (unsigned)json_integer_value(data(v, "local_port")); json_decref(v);
    const char *lines[3];
    lines[0] = strdup(inject_line(flow, "00ff10", 0)); lines[1] = strdup(stats_line(flow)); lines[2] = strdup(arrive_line(PEER, 4242, local, "00"));
    for (int i = 0; i < 3; ++i) { v = ask(b->path, lines[i], NULL); json_decref(expect_error(v, "ECONNREFUSED", "node.native_api.debug_commands")); }
    /* The gate refuses the command, not the connection. */
    int lfd; v = ask(b->path, listen_line(4243), &lfd); CHECK(is_ok(v)); json_decref(v);
    close(lfd); await_event(&b->events, FIPSD_NATIVE_EVENT_LISTENER_RELEASED, 0, 4243);
    close(ffd); await_event(&b->events, FIPSD_NATIVE_EVENT_FLOW_RELEASED, flow, 0);
    for (int i = 0; i < 3; ++i) free((void *)lines[i]);
    /* And every debug command is answered where the gate is open. */
    v = ask(a->path, connect_line(PEER, 4242, 0), &ffd); CHECK(is_ok(v)); bounded(ffd);
    flow = flow_id_of(v); local = (unsigned)json_integer_value(data(v, "local_port")); json_decref(v);
    v = ask(a->path, inject_line(flow, "00ff10", 0), NULL); CHECK(is_ok(v)); json_decref(v);
    v = ask(a->path, stats_line(flow), NULL); CHECK(is_ok(v)); json_decref(v);
    v = ask(a->path, arrive_line(PEER, 4242, local, "00"), NULL);
    CHECK(is_ok(v) && !strcmp(str(json_object_get(v, "data"), "outcome"), "delivered")); json_decref(v);
    unsigned char buf[8]; CHECK(read_dgram(ffd, buf, sizeof buf) == 3 && read_dgram(ffd, buf, sizeof buf) == 1);
    close(ffd); await_event(&a->events, FIPSD_NATIVE_EVENT_FLOW_RELEASED, flow, 0);
}
static void test_command_cap_and_refusal_sizes(api_node *a)
{
    /* Over the cap, with and without the newline, ends the connection with nothing written back. */
    int c = raw_open(a->path);
    char *big = malloc(FIPSD_NATIVE_MAX_COMMAND + 2); CHECK(big);
    memset(big, 'x', FIPSD_NATIVE_MAX_COMMAND + 1); big[FIPSD_NATIVE_MAX_COMMAND + 1] = 0;
    raw_send(c, big);
    CHECK(raw_reply(c, NULL) == NULL); close(c);
    /* Exactly the cap is accepted and refused as an unknown command, in a
     * reply under 2048 bytes, and the connection goes on answering. */
    c = raw_open(a->path);
    big[FIPSD_NATIVE_MAX_COMMAND] = 0;
    memcpy(big, "{\"command\":\"", 12); big[FIPSD_NATIVE_MAX_COMMAND - 2] = '"'; big[FIPSD_NATIVE_MAX_COMMAND - 1] = '}';
    raw_send(c, big);
    unsigned char reply[4096]; size_t len = 0;
    for (;;) { ssize_t n = raw_recv(c, reply + len, sizeof reply - len, NULL); CHECK(n > 0); len += (size_t)n; if (memchr(reply, '\n', len)) break; }
    CHECK(len < 2048);
    json_error_t e; json_t *v = json_loadb((const char *)reply, len - 1, 0, &e); CHECK(v);
    json_decref(expect_error(v, "EINVAL", "unknown command"));
    int lfd; v = raw_call(c, listen_line(4242), &lfd); CHECK(is_ok(v) && lfd >= 0); json_decref(v);
    close(lfd); close(c);
    await_event(&a->events, FIPSD_NATIVE_EVENT_LISTENER_RELEASED, 0, 4242);
    /* A refusal made of control characters, and one quoting a long value. */
    const char *frame = "{\"command\":\"\"}";
    size_t room = FIPSD_NATIVE_MAX_COMMAND - strlen(frame);
    memcpy(big, "{\"command\":\"", 12);
    size_t at = 12; while (at + 6 <= 12 + room) { memcpy(big + at, "\\u0001", 6); at += 6; }
    while (at < 12 + room) big[at++] = 'x';
    memcpy(big + at, "\"}", 3);
    CHECK(strlen(big) == FIPSD_NATIVE_MAX_COMMAND);
    c = raw_open(a->path); raw_send(c, big);
    len = 0; for (;;) { ssize_t n = raw_recv(c, reply + len, sizeof reply - len, NULL); CHECK(n > 0); len += (size_t)n; if (memchr(reply, '\n', len)) break; }
    CHECK(len < 2048); v = json_loadb((const char *)reply, len - 1, 0, &e); CHECK(v); json_decref(expect_error(v, "EINVAL", NULL)); close(c);
    snprintf(big, FIPSD_NATIVE_MAX_COMMAND + 2, "{\"command\":\"connect\",\"params\":{\"peer\":\"%s\",\"remote_port\":\"", PEER);
    at = strlen(big); room = FIPSD_NATIVE_MAX_COMMAND - at - 3;
    size_t base = at;
    while (at + 6 <= base + room) { memcpy(big + at, "\\u0001", 6); at += 6; big[at] = 0; }
    while (at < FIPSD_NATIVE_MAX_COMMAND - 3) big[at++] = 'x';
    memcpy(big + at, "\"}}", 4);
    CHECK(strlen(big) == FIPSD_NATIVE_MAX_COMMAND);
    c = raw_open(a->path); raw_send(c, big);
    len = 0; for (;;) { ssize_t n = raw_recv(c, reply + len, sizeof reply - len, NULL); CHECK(n > 0); len += (size_t)n; if (memchr(reply, '\n', len)) break; }
    CHECK(len < 2048); v = json_loadb((const char *)reply, len - 1, 0, &e); CHECK(v);
    CHECK(!strncmp(str(v, "message"), "invalid params for 'connect'", 28)); json_decref(expect_error(v, "EINVAL", NULL)); close(c);
    free(big);
}
static void test_one_peer_flooding(api_node *a)
{
    int lfd; json_t *v = ask(a->path, listen_line(4242), &lfd); CHECK(is_ok(v)); json_decref(v); bounded(lfd);
    unsigned announced = 0; uint64_t refused[8];
    for (unsigned src = 5000; src < 5008; ++src) {
        v = ask(a->path, arrive_line(PEER, src, 4242, "00"), NULL); CHECK(is_ok(v));
        if (!strcmp(str(json_object_get(v, "data"), "outcome"), "announced")) {
            refused[announced++] = flow_id_of(v); int ffd; json_t *m = raw_accept(lfd, &ffd); json_decref(m); close(ffd);
        }
        json_decref(v);
    }
    CHECK(announced == 2); /* the quarter share of eight */
    int ffd; v = ask(a->path, connect_line(PEER, 4243, 6000), &ffd); CHECK(is_ok(v)); uint64_t flow = flow_id_of(v); json_decref(v);
    v = ask(a->path, arrive_line(OTHER, 5000, 4242, "00"), NULL);
    CHECK(is_ok(v) && !strcmp(str(json_object_get(v, "data"), "outcome"), "announced")); json_decref(v);
    close(ffd); await_event(&a->events, FIPSD_NATIVE_EVENT_FLOW_RELEASED, flow, 0);
    close(lfd); await_event(&a->events, FIPSD_NATIVE_EVENT_LISTENER_RELEASED, 0, 4242);
    /* The refused flows were pinned until the listener closed; they end now. */
    for (unsigned i = 0; i < announced; ++i) await_event(&a->events, FIPSD_NATIVE_EVENT_FLOW_RELEASED, refused[i], 0);
    drain_events(&a->events);
}
/* A datagram a client writes leaves through the dispatcher and comes back
 * to a listener on this node: the outbound and inbound paths end to end. */
static void test_self_delivery(api_node *a)
{
    int lfd, cfd; json_t *v = ask(a->path, listen_line(4600), &lfd); CHECK(is_ok(v)); json_decref(v); bounded(lfd);
    v = ask(a->path, connect_line(a->npub, 4600, 0), &cfd); CHECK(is_ok(v)); bounded(cfd);
    unsigned local = (unsigned)json_integer_value(data(v, "local_port")); uint64_t cflow = flow_id_of(v); json_decref(v);
    write_dgram(cfd, "over the wire", 13);
    int ffd; json_t *m = raw_accept(lfd, &ffd); bounded(ffd);
    CHECK(json_integer_value(json_object_get(m, "remote_port")) == (json_int_t)local && json_integer_value(json_object_get(m, "local_port")) == 4600);
    CHECK(!strcmp(str(m, "peer"), a->npub) && json_integer_value(json_object_get(m, "held")) == 1);
    uint64_t aflow = (uint64_t)json_integer_value(json_object_get(m, "flow_id")); json_decref(m);
    unsigned char buf[64]; CHECK(read_dgram(ffd, buf, sizeof buf) == 13 && !memcmp(buf, "over the wire", 13));
    write_dgram(ffd, "and back", 8);
    CHECK(read_dgram(cfd, buf, sizeof buf) == 8 && !memcmp(buf, "and back", 8));
    await_event(&a->events, FIPSD_NATIVE_EVENT_DATAGRAM_COUNTED, 0, 4600);
    await_event(&a->events, FIPSD_NATIVE_EVENT_DATAGRAM_COUNTED, 0, (uint16_t)local);
    v = show_native_flows(a);
    CHECK(json_object_get(data(v, "stats"), "sent_datagrams") && json_array_size(data(v, "flows")) == 2);
    json_decref(v);
    /* A datagram to the node's own address is delivered without crossing
     * the session send path, so only the receive side counts here; the wire
     * case, sent and received, is exact in the two-node proof. */
    fipsd_metrics *live = (fipsd_metrics *)fipsd_node_handlers_metrics(a->node.h);
    CHECK(fipsd_metrics_get(live, FIPSD_METRIC_native_received_datagrams) >= 2 && fipsd_metrics_get(live, FIPSD_METRIC_native_received_bytes) >= 21);
    CHECK(fipsd_metrics_get(live, FIPSD_METRIC_native_flows_accepted) >= 1 && fipsd_metrics_get(live, FIPSD_METRIC_native_flows_opened) >= 1);
    close(cfd); await_event(&a->events, FIPSD_NATIVE_EVENT_FLOW_RELEASED, cflow, 0);
    close(ffd); await_event(&a->events, FIPSD_NATIVE_EVENT_FLOW_RELEASED, aflow, 0);
    close(lfd); await_event(&a->events, FIPSD_NATIVE_EVENT_LISTENER_RELEASED, 0, 4600);
}
/* node_test.h helpers these scenarios do not need. */
static void (*const unused_helpers[])(void) = {(void (*)(void))nt_await, (void (*)(void))nt_identity, (void (*)(void))nt_query, (void (*)(void))nt_print_trace};
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
    hcfg.peer_capacity = 4; hcfg.tree_peer_capacity = 4; hcfg.session_capacity = 4; hcfg.cache_entries = 64; hcfg.notice_capacity = 256; hcfg.tick_ms = 200;
    api_node a, b;
    api_init(&a, net, "A", "loop:a", "1111111111111111111111111111111111111111111111111111111111111111", path_a, true, &hcfg);
    api_init(&b, net, "B", "loop:b", "2222222222222222222222222222222222222222222222222222222222222222", path_b, false, &hcfg);
    CHECK(fipsd_runtime_register(rt, fipsd_node_subsystem(a.node.node)) == 0);
    CHECK(fipsd_runtime_register(rt, fipsd_node_subsystem(b.node.node)) == 0);
    CHECK(fipsd_runtime_register(rt, fipsd_native_api_subsystem(a.api)) == 0);
    CHECK(fipsd_runtime_register(rt, fipsd_native_api_subsystem(b.api)) == 0);
    CHECK(fipsd_runtime_start(rt) == 0);
    static char npub_a[FIPS_NIP19_CAPACITY], npub_b[FIPS_NIP19_CAPACITY];
    CHECK(fips_identity_npub(fipsd_node_identity(fipsd_node_get_context(a.node.node)), npub_a, sizeof npub_a) == FIPS_IDENTITY_OK);
    CHECK(fips_identity_npub(fipsd_node_identity(fipsd_node_get_context(b.node.node)), npub_b, sizeof npub_b) == FIPS_IDENTITY_OK);
    a.npub = npub_a; b.npub = npub_b;
    struct stat st; CHECK(lstat(path_a, &st) == 0 && S_ISSOCK(st.st_mode) && (st.st_mode & 0777) == 0770);
    test_listeners_and_ports(&a);
    test_refusals(&a);
    test_connect(&a);
    test_arrivals_and_pins(&a);
    test_kept_descriptors(&a);
    test_debug_gate(&a, &b);
    test_command_cap_and_refusal_sizes(&a);
    test_one_peer_flooding(&a);
    test_self_delivery(&a);
    /* Shutdown with a flow, a listener and an idle connection still held by
     * clients: cancellation-safe, within the grace period, and the clients
     * then read end of file. */
    int lfd, ffd, c = raw_open(a.path);
    json_t *v = raw_call(c, listen_line(4700), &lfd); CHECK(is_ok(v)); json_decref(v);
    v = ask(a.path, connect_line(PEER, 4242, 4701), &ffd); CHECK(is_ok(v)); json_decref(v);
    bounded(lfd); bounded(ffd);
    fipsd_runtime_shutdown(rt);
    CHECK(fipsd_runtime_run(rt) == FIPSD_RUN_OK);
    unsigned char buf[16];
    CHECK(read_dgram(ffd, buf, sizeof buf) <= 0 && read_dgram(lfd, buf, sizeof buf) <= 0);
    CHECK(raw_recv(c, buf, sizeof buf, NULL) == 0);
    close(lfd); close(ffd); close(c);
    fipsd_runtime_destroy(rt);
    fipsd_native_api_destroy(a.api); fipsd_native_api_destroy(b.api);
    CHECK(lstat(path_a, &st) != 0 && lstat(path_b, &st) != 0);
    drain_events(&a.events); drain_events(&b.events);
    go_channel_free(a.events.chan); go_channel_free(b.events.chan);
    nt_drain(&a.node); nt_drain(&b.node);
    nt_destroy(&a.node); nt_destroy(&b.node);
    fipsd_loopback_network_destroy(net);
    puts("native api: line protocol, hand-off, pins, debug gate, caps, provider, self-delivery, shutdown: ok");
    return 0;
}
