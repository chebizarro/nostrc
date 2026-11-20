/* The socket pair behind a flow and a listener, the descriptor hand-off and
 * the C client library, over real socket pairs and a fake daemon (upstream
 * src/native/{seqpacket,fdpass,dgram_probe}.rs and client/mod.rs tests).
 * Every wait is bounded by a socket deadline; the one timed wait is the
 * kernel descriptor collector's. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "native/seqpacket.h"
#include "native/fdpass.h"
#include "native/dgram_probe.h"
#include "native/client_internal.h"
#include "runtime.h"
#include "host_os.h"
#include <fips/identity/encoding.h>
#include <go.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s (errno %d %s)\n", __FILE__, __LINE__, #x, errno, strerror(errno)); abort(); } } while (0)

static const char PEER[] = "npub1sjlh2c3x9w7kjsqg2ay080n2lff2uvt325vpan33ke34rn8l5jcqawh57m";
static const char NODE[] = "npub1n9lpnv0592cc2ps6nm0ca3qls642vx7yjsv35rkxqzj2vgds52sqgpverl";
static const char CONNECT_REPLY[] = "{\"status\":\"ok\",\"data\":{\"flow_id\":3,\"local_port\":49152,\"remote_port\":4242,"
    "\"peer\":\"npub1sjlh2c3x9w7kjsqg2ay080n2lff2uvt325vpan33ke34rn8l5jcqawh57m\","
    "\"node\":\"npub1n9lpnv0592cc2ps6nm0ca3qls642vx7yjsv35rkxqzj2vgds52sqgpverl\",\"max_payload\":1362}}\n";
static const char LISTEN_REPLY[] = "{\"status\":\"ok\",\"data\":{\"local_port\":4242,"
    "\"node\":\"npub1n9lpnv0592cc2ps6nm0ca3qls642vx7yjsv35rkxqzj2vgds52sqgpverl\",\"backlog\":16}}\n";
static const char ARRIVAL[] = "{\"flow_id\":9,\"peer\":\"npub1sjlh2c3x9w7kjsqg2ay080n2lff2uvt325vpan33ke34rn8l5jcqawh57m\","
    "\"node\":\"npub1n9lpnv0592cc2ps6nm0ca3qls642vx7yjsv35rkxqzj2vgds52sqgpverl\","
    "\"local_port\":4242,\"remote_port\":5001,\"max_payload\":1362,\"held\":1}";
static const char REFUSAL[] = "{\"status\":\"error\",\"message\":\"port 4242 is already in use on this node\",\"data\":{\"errno\":\"EADDRINUSE\"}}\n";
#define DEADLINE_MS 5000

static void bounded(int fd)
{
    struct timeval tv = {DEADLINE_MS / 1000, 0};
    CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) == 0);
    CHECK(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) == 0);
}
static void stream_pair(int fds[2]) { CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0); bounded(fds[0]); bounded(fds[1]); }
/* One sendmsg with a descriptor, from a plain thread: the daemon's send as a
 * test's fake daemon performs it. */
static ssize_t send_fd(int sock, const char *line, size_t len, int fd)
{
    struct iovec iov = {.iov_base = (void *)line, .iov_len = len};
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1};
    union { struct cmsghdr header; unsigned char bytes[CMSG_SPACE(sizeof(int))]; } control;
    if (fd >= 0) {
        memset(&control, 0, sizeof control);
        msg.msg_control = control.bytes; msg.msg_controllen = CMSG_SPACE(sizeof(int));
        struct cmsghdr *h = CMSG_FIRSTHDR(&msg);
        h->cmsg_level = SOL_SOCKET; h->cmsg_type = SCM_RIGHTS; h->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(h), &fd, sizeof fd);
    }
    return sendmsg(sock, &msg, FIPSD_NATIVE_MSG_NOSIGNAL);
}
static size_t read_line(int fd, char *out, size_t cap)
{
    size_t n = 0;
    for (;;) {
        char c; ssize_t r = read(fd, &c, 1);
        CHECK(r == 1);
        if (c == '\n') { out[n] = 0; return n; }
        CHECK(n + 1 < cap); out[n++] = c;
    }
}
static fips_native_addr addr_of(const char *npub, uint16_t port)
{ fips_native_addr a; CHECK(fips_native_addr_from_npub(npub, port, &a) == 0); return a; }

/* ---- seqpacket.rs: the pair and the end-of-file rule ---------------------- */
static void test_message_boundaries_survive_in_both_directions(void)
{
    int fds[2]; CHECK(fipsd_native_pair(fds) == 0);
    CHECK(fipsd_native_set_nonblocking(fds[0], true) == 0);
    CHECK(send(fds[1], "one", 3, 0) == 3 && send(fds[1], "two", 3, 0) == 3 && send(fds[1], "three", 5, 0) == 5);
    char buf[64]; size_t len;
    CHECK(fipsd_native_recv_once(fds[0], buf, sizeof buf, &len) == FIPSD_NATIVE_RECEIVED_DATAGRAM && len == 3 && !memcmp(buf, "one", 3));
    CHECK(fipsd_native_recv_once(fds[0], buf, sizeof buf, &len) == FIPSD_NATIVE_RECEIVED_DATAGRAM && len == 3 && !memcmp(buf, "two", 3));
    CHECK(fipsd_native_recv_once(fds[0], buf, sizeof buf, &len) == FIPSD_NATIVE_RECEIVED_DATAGRAM && len == 5 && !memcmp(buf, "three", 5));
    CHECK(fipsd_native_recv_once(fds[0], buf, sizeof buf, &len) == FIPSD_NATIVE_RECEIVED_AGAIN);
    CHECK(fipsd_native_send_once(fds[0], "alpha", 5) == 5 && fipsd_native_send_once(fds[0], "beta", 4) == 4);
    CHECK(recv(fds[1], buf, sizeof buf, 0) == 5 && !memcmp(buf, "alpha", 5));
    CHECK(recv(fds[1], buf, sizeof buf, 0) == 4 && !memcmp(buf, "beta", 4));
    /* Both halves close-on-exec; the client's half is left blocking. */
    CHECK(fcntl(fds[0], F_GETFD) & FD_CLOEXEC); CHECK(fcntl(fds[1], F_GETFD) & FD_CLOEXEC);
    CHECK(!(fcntl(fds[1], F_GETFL) & O_NONBLOCK));
    close(fds[0]); close(fds[1]);
}
static void test_the_end_of_file_rule(void)
{
    char buf[64]; size_t len; int fds[2];
    /* Closing the client half reports end of file. */
    CHECK(fipsd_native_pair(fds) == 0); CHECK(fipsd_native_set_nonblocking(fds[0], true) == 0);
    close(fds[1]);
    CHECK(fipsd_native_recv_once(fds[0], buf, sizeof buf, &len) == FIPSD_NATIVE_RECEIVED_EOF);
    close(fds[0]);
    /* An empty datagram is not end of file. */
    CHECK(fipsd_native_pair(fds) == 0); CHECK(fipsd_native_set_nonblocking(fds[0], true) == 0);
    CHECK(send(fds[1], NULL, 0, 0) == 0); CHECK(send(fds[1], "after", 5, 0) == 5);
    CHECK(fipsd_native_recv_once(fds[0], buf, sizeof buf, &len) == FIPSD_NATIVE_RECEIVED_DATAGRAM && len == 0);
    CHECK(fipsd_native_recv_once(fds[0], buf, sizeof buf, &len) == FIPSD_NATIVE_RECEIVED_DATAGRAM && len == 5);
    close(fds[0]); close(fds[1]);
    /* An empty datagram before a close does not swallow the message behind it. */
    CHECK(fipsd_native_pair(fds) == 0); CHECK(fipsd_native_set_nonblocking(fds[0], true) == 0);
    CHECK(send(fds[1], NULL, 0, 0) == 0); CHECK(send(fds[1], "after", 5, 0) == 5); close(fds[1]);
    CHECK(fipsd_native_recv_once(fds[0], buf, sizeof buf, &len) == FIPSD_NATIVE_RECEIVED_DATAGRAM && len == 0);
    CHECK(fipsd_native_recv_once(fds[0], buf, sizeof buf, &len) == FIPSD_NATIVE_RECEIVED_DATAGRAM && len == 5 && !memcmp(buf, "after", 5));
    CHECK(fipsd_native_recv_once(fds[0], buf, sizeof buf, &len) == FIPSD_NATIVE_RECEIVED_EOF);
    close(fds[0]);
    /* A trailing empty datagram before a close reads as the close only on SOCK_SEQPACKET. */
    CHECK(fipsd_native_pair(fds) == 0); CHECK(fipsd_native_set_nonblocking(fds[0], true) == 0);
    CHECK(send(fds[1], NULL, 0, 0) == 0); close(fds[1]);
    if (FIPSD_NATIVE_SOCK_TYPE == SOCK_SEQPACKET) {
        CHECK(fipsd_native_recv_once(fds[0], buf, sizeof buf, &len) == FIPSD_NATIVE_RECEIVED_EOF);
    } else {
        CHECK(fipsd_native_recv_once(fds[0], buf, sizeof buf, &len) == FIPSD_NATIVE_RECEIVED_DATAGRAM && len == 0);
        CHECK(fipsd_native_recv_once(fds[0], buf, sizeof buf, &len) == FIPSD_NATIVE_RECEIVED_EOF);
    }
    close(fds[0]);
    /* Timeouts: zero refused, read back, cleared, directions separate. */
    CHECK(fipsd_native_pair(fds) == 0);
    int64_t ms = 0;
    CHECK(fipsd_native_set_timeout(fds[0], SO_RCVTIMEO, 0) == -1 && errno == EINVAL);
    CHECK(fipsd_native_timeout(fds[0], SO_RCVTIMEO, &ms) == 0 && ms == -1);
    CHECK(fipsd_native_set_timeout(fds[0], SO_RCVTIMEO, 3000) == 0);
    CHECK(fipsd_native_timeout(fds[0], SO_RCVTIMEO, &ms) == 0 && ms == 3000);
    CHECK(fipsd_native_timeout(fds[0], SO_SNDTIMEO, &ms) == 0 && ms == -1);
    CHECK(fipsd_native_set_timeout(fds[0], SO_RCVTIMEO, -1) == 0);
    CHECK(fipsd_native_timeout(fds[0], SO_RCVTIMEO, &ms) == 0 && ms == -1);
    CHECK(fipsd_native_peer_gone_as_epipe(ECONNRESET) == EPIPE && fipsd_native_peer_gone_as_epipe(EAGAIN) == EAGAIN);
    close(fds[0]); close(fds[1]);
}

/* ---- dgram_probe.rs: what this kernel does ------------------------------- */
static void test_dgram_probes(void)
{
    int fds[2]; unsigned char buf[64];
    CHECK(fipsd_native_probe_dgram_pair(fds) == 0);
    CHECK(fipsd_native_probe_send(fds[0], "\1\2\3", 3) == 3 && fipsd_native_probe_send(fds[0], "\4\5", 2) == 2);
    CHECK(fipsd_native_probe_recv(fds[1], buf, sizeof buf) == 3 && fipsd_native_probe_recv(fds[1], buf, sizeof buf) == 2);
    CHECK(fipsd_native_probe_send(fds[0], "", 0) == 0);
    CHECK(fipsd_native_probe_recv(fds[1], buf, sizeof buf) == 0 && !fipsd_native_probe_hung_up(fds[1]));
    /* The largest payload the API advertises crosses a SOCK_DGRAM pair. */
    unsigned char big[1362 * 2]; memset(big, 0xA5, sizeof big);
    size_t largest = 0;
    static const size_t sizes[] = {64, 256, 1024, 1362, 1362 * 2};
    for (size_t i = 0; i < 5; ++i) {
        if (fipsd_native_probe_send(fds[0], big, sizes[i]) < 0) break;
        ssize_t n = fipsd_native_probe_recv(fds[1], big, sizeof big);
        if (n != (ssize_t)sizes[i]) break;
        largest = sizes[i];
    }
    CHECK(largest >= 1362);
    close(fds[0]); close(fds[1]);
    /* The close signal, per kernel. */
    CHECK(fipsd_native_probe_dgram_pair(fds) == 0); close(fds[0]);
    ssize_t n = fipsd_native_probe_recv(fds[1], buf, sizeof buf); int e = errno;
#if defined(__linux__)
    CHECK(n < 0 && e == EAGAIN && !fipsd_native_probe_hung_up(fds[1]));
#elif defined(__APPLE__)
    CHECK(n < 0 && e == ECONNRESET && !fipsd_native_probe_hung_up(fds[1]));
#else
    CHECK(fipsd_native_probe_hung_up(fds[1]) || n == 0 || (n < 0 && e != EAGAIN && e != EWOULDBLOCK));
#endif
    close(fds[1]);
    /* A datagram queued before the close is still readable; an empty one is not read as the close. */
    CHECK(fipsd_native_probe_dgram_pair(fds) == 0);
    CHECK(fipsd_native_probe_send(fds[0], "\7\7\7", 3) == 3); close(fds[0]);
    CHECK(fipsd_native_probe_recv(fds[1], buf, sizeof buf) == 3 && buf[0] == 7); close(fds[1]);
    CHECK(fipsd_native_probe_dgram_pair(fds) == 0);
    CHECK(fipsd_native_probe_send(fds[0], "", 0) == 0); close(fds[0]);
    CHECK(fipsd_native_probe_recv(fds[1], buf, sizeof buf) == 0 && !fipsd_native_probe_hung_up(fds[1])); close(fds[1]);
    /* What the collector does to a socket in flight: the measurement the
     * daemon's descriptor pin rests on. */
    ssize_t after = fipsd_native_probe_read_after_collection_in_flight();
#if defined(__APPLE__)
    CHECK(after == 0);
#elif defined(__linux__)
    CHECK(after == 4);
#else
    CHECK(after == 0 || after == 4);
#endif
}

/* ---- fdpass.rs and the fiber receive, inside a runtime -------------------- */
typedef struct fiber_case { GoChannel *done; int outcome; } fiber_case;
static void fdpass_fiber(void *arg)
{
    fiber_case *c = arg;
    int conn[2], pair[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, conn) == 0);
    CHECK(fipsd_native_pair(pair) == 0);
    CHECK(fipsd_native_set_nonblocking(pair[0], true) == 0);
    /* A reply without a descriptor arrives whole. */
    CHECK(fipsd_native_sendmsg_fd(conn[0], "{\"status\":\"ok\"}\n", 16, -1, -1) == 16);
    unsigned char buf[4096]; fipsd_native_chunk chunk;
    CHECK(fipsd_native_recvmsg_fd(conn[1], buf, sizeof buf, &chunk, -1) == 0 && chunk.len == 16 && chunk.fd < 0 && !chunk.truncated);
    /* A descriptor arrives with its reply and still works. */
    CHECK(fipsd_native_sendmsg_fd(conn[0], "{\"status\":\"ok\"}\n", 16, pair[1], -1) == 16);
    close(pair[1]);
    CHECK(fipsd_native_recvmsg_fd(conn[1], buf, sizeof buf, &chunk, -1) == 0 && chunk.len == 16 && chunk.fd >= 0);
    CHECK(fcntl(chunk.fd, F_GETFD) & FD_CLOEXEC);
    CHECK(send(chunk.fd, "through the passed fd", 21, 0) == 21);
    size_t len;
    CHECK(fipsd_native_recv_fiber(pair[0], buf, sizeof buf, &len) == FIPSD_NATIVE_RECEIVED_DATAGRAM && len == 21);
    CHECK(!memcmp(buf, "through the passed fd", 21));
    /* The fiber send parks until writable; an empty datagram crosses too. */
    CHECK(fipsd_native_send_fiber(pair[0], "back", 4, -1) == 0);
    CHECK(recv(chunk.fd, buf, sizeof buf, 0) == 4 && !memcmp(buf, "back", 4));
    CHECK(fipsd_native_send_fiber(pair[0], "", 0, -1) == 0);
    CHECK(recv(chunk.fd, buf, sizeof buf, 0) == 0);
    /* Closing the received half reaches the daemon's reader as end of file,
     * on Darwin through the bounded retry. */
    close(chunk.fd);
    CHECK(fipsd_native_recv_fiber(pair[0], buf, sizeof buf, &len) == FIPSD_NATIVE_RECEIVED_EOF);
    /* End of file on the connection itself. */
    close(conn[0]);
    CHECK(fipsd_native_recvmsg_fd(conn[1], buf, sizeof buf, &chunk, -1) == 0 && chunk.len == 0 && chunk.fd < 0);
    close(conn[1]); close(pair[0]);
    c->outcome = 1;
    CHECK(go_channel_send(c->done, c) == 0);
}
static void test_fdpass_in_a_runtime(void)
{
    fipsd_runtime_config cfg; fipsd_runtime_config_default(&cfg); cfg.handle_signals = false; cfg.workers = 2;
    fipsd_runtime *rt = fipsd_runtime_create(&cfg); CHECK(rt);
    CHECK(fipsd_runtime_start(rt) == 0);
    fiber_case c = {.done = go_channel_create(1)};
    CHECK(c.done && fipsd_runtime_spawn(rt, fdpass_fiber, &c) == 0);
    void *token = NULL;
    GoSelectCase cases[] = {{.op = GO_SELECT_RECEIVE, .chan = c.done, .recv_buf = &token}};
    CHECK(go_select_timeout(cases, 1, 30000).selected_case == 0 && token == &c && c.outcome == 1);
    fipsd_runtime_shutdown(rt); CHECK(fipsd_runtime_run(rt) == FIPSD_RUN_OK); fipsd_runtime_destroy(rt);
    go_channel_free(c.done);
}

/* ---- client/mod.rs and codec.rs: the C client against a fake daemon ------- */
typedef struct fake { int fd, pass; const char *reply; char command[512]; ssize_t after; bool close_after; } fake;
static void *fake_daemon(void *arg)
{
    fake *f = arg;
    read_line(f->fd, f->command, sizeof f->command);
    if (f->reply) CHECK(send_fd(f->fd, f->reply, strlen(f->reply), f->pass) == (ssize_t)strlen(f->reply));
    if (f->close_after) { close(f->fd); return NULL; }
    /* The connection must be gone once setup has returned. */
    char rest[16];
    f->after = read(f->fd, rest, sizeof rest);
    if (f->pass >= 0) close(f->pass);
    close(f->fd);
    return NULL;
}
static void test_npub_codec_agrees_with_libfips_core(void)
{
    fips_secp *secp; CHECK(fips_identity_init_secp(fipsd_host_os_entropy(), &secp) == FIPS_IDENTITY_OK);
    const char *samples[] = {PEER, NODE};
    for (size_t i = 0; i < 2; ++i) {
        uint8_t key[32]; fips_xonly_pubkey reference;
        CHECK(fips_native_npub_decode(samples[i], key) == 0);
        CHECK(fips_decode_npub(secp, samples[i], strlen(samples[i]), &reference) == FIPS_IDENTITY_OK);
        CHECK(!memcmp(key, reference.bytes, 32));
        char npub[FIPS_NATIVE_NPUB_CAPACITY], expected[FIPS_NIP19_CAPACITY];
        CHECK(fips_native_npub_encode(key, npub, sizeof npub) == 0);
        CHECK(fips_encode_npub(&reference, expected, sizeof expected) == FIPS_IDENTITY_OK);
        CHECK(!strcmp(npub, expected) && !strcmp(npub, samples[i]));
    }
    char upper[64]; for (size_t i = 0; i <= strlen(PEER); ++i) upper[i] = (char)((PEER[i] >= 'a' && PEER[i] <= 'z') ? PEER[i] - 32 : PEER[i]);
    uint8_t key[32]; CHECK(fips_native_npub_decode(upper, key) == 0);
    char mixed[64]; strcpy(mixed, PEER); mixed[10] = (char)(mixed[10] - 32);
    CHECK(fips_native_npub_decode(mixed, key) == -EINVAL);
    CHECK(fips_native_npub_decode("not-an-npub", key) == -EINVAL);
    CHECK(fips_native_npub_decode("nsec1vl029mgpspedva04g90vltkh6fvh240zqtv9k0t9af8935ke9laqsnlfe5", key) == -EINVAL);
    char corrupt[64]; strcpy(corrupt, PEER); corrupt[20] = corrupt[20] == 'q' ? 'p' : 'q';
    CHECK(fips_native_npub_decode(corrupt, key) == -EINVAL);
    fips_identity_destroy_secp(secp);
}
static void test_addresses(void)
{
    fips_native_addr want = addr_of(PEER, 4242), got;
    char text[FIPS_NATIVE_ADDR_CAPACITY];
    snprintf(text, sizeof text, "%s:4242", PEER);
    CHECK(fips_native_addr_parse(text, &got) == 0 && fips_native_addr_eq(&got, &want));
    char back[FIPS_NATIVE_ADDR_CAPACITY];
    CHECK(fips_native_addr_format(&want, back, sizeof back) == 0 && !strcmp(back, text));
    CHECK(fips_native_addr_format(&want, back, 10) == -ENAMETOOLONG);
    const char *bad[] = {"not-an-npub", "npub1abc:4242", PEER, "npub1sjlh2c3x9w7kjsqg2ay080n2lff2uvt325vpan33ke34rn8l5jcqawh57m:70000",
        "npub1sjlh2c3x9w7kjsqg2ay080n2lff2uvt325vpan33ke34rn8l5jcqawh57m:", "npub1sjlh2c3x9w7kjsqg2ay080n2lff2uvt325vpan33ke34rn8l5jcqawh57m:-1"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) CHECK(fips_native_addr_parse(bad[i], &got) == -EINVAL);
    char path[256]; CHECK(fips_native_socket_path(path, sizeof path) == 0 && strstr(path, "api.sock"));
    CHECK(fips_native_socket_path(path, 4) == -ENAMETOOLONG);
    CHECK(strstr(fips_native_socket, "api.sock"));
    CHECK(!strcmp(fips_native_strerror(0), "success") && strstr(fips_native_strerror(-EPROTO), "protocol"));
    CHECK(fips_native__errno_of("EADDRINUSE", 10) == -EADDRINUSE && fips_native__errno_of("EADDRNOTAVAIL", 13) == -EADDRNOTAVAIL);
    CHECK(fips_native__errno_of("EMFILE", 6) == -EMFILE && fips_native__errno_of("EINVAL", 6) == -EINVAL);
    CHECK(fips_native__errno_of("EMSGSIZE", 8) == -EMSGSIZE && fips_native__errno_of("EPIPE", 5) == -EPIPE);
    CHECK(fips_native__errno_of("ETIMEDOUT", 9) == -ETIMEDOUT && fips_native__errno_of("EWHATEVER", 9) == -ECONNREFUSED);
    const unsigned char *data; size_t n;
    const char *ok = "{\"status\":\"ok\",\"data\":{\"local_port\":4242}}";
    CHECK(fips_native__reply((const unsigned char *)ok, strlen(ok), &data, &n) == 0 && n == 19);
    CHECK(fips_native__reply((const unsigned char *)REFUSAL, strlen(REFUSAL) - 1, &data, &n) == -EADDRINUSE);
    const char *bare = "{\"status\":\"error\",\"message\":\"no such flow: 9\"}";
    CHECK(fips_native__reply((const unsigned char *)bare, strlen(bare), &data, &n) == -ECONNREFUSED);
    const char *unknown = "{\"status\":\"error\",\"data\":{\"errno\":\"EWHATEVER\"}}";
    CHECK(fips_native__reply((const unsigned char *)unknown, strlen(unknown), &data, &n) == -ECONNREFUSED);
    CHECK(fips_native__reply((const unsigned char *)"not json at all", 15, &data, &n) == -EPROTO);
    CHECK(fips_native__reply((const unsigned char *)"{\"data\":{}}", 11, &data, &n) == -EPROTO);
    CHECK(fips_native__reply((const unsigned char *)"{\"status\":\"maybe\"}", 18, &data, &n) == -EPROTO);
}
static void test_connect_returns_a_stream_and_closes_the_connection(void)
{
    int conn[2], pair[2]; stream_pair(conn); CHECK(fipsd_native_pair(pair) == 0);
    fake f = {.fd = conn[0], .pass = pair[1], .reply = CONNECT_REPLY, .after = -2};
    pthread_t thread; CHECK(pthread_create(&thread, NULL, fake_daemon, &f) == 0);
    fips_native_addr peer = addr_of(PEER, 4242);
    fips_native_stream *flow = NULL;
    CHECK(fips_native__connect_on(conn[1], 0, &peer, &flow) == 0 && flow);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(strstr(f.command, "\"command\":\"connect\"") && strstr(f.command, PEER) && strstr(f.command, "\"local_port\":0"));
    /* Framing is asked for where the datagram pair would drop a slow reader;
     * this daemon answered with its datagram pair, so datagrams are what the
     * stream carries (the upstream daemon's answer, which ignores the field). */
    CHECK(!strstr(f.command, "\"framing\":\"stream\"") == !FIPSD_NATIVE_STREAM_FRAMING);
    CHECK(f.after == 0); /* the RPC connection was closed */
    char text[FIPS_NATIVE_ADDR_CAPACITY], want[FIPS_NATIVE_ADDR_CAPACITY];
    fips_native_addr a = fips_native_stream_peer_addr(flow);
    CHECK(fips_native_addr_format(&a, text, sizeof text) == 0); snprintf(want, sizeof want, "%s:4242", PEER); CHECK(!strcmp(text, want));
    a = fips_native_stream_local_addr(flow);
    CHECK(fips_native_addr_format(&a, text, sizeof text) == 0); snprintf(want, sizeof want, "%s:49152", NODE); CHECK(!strcmp(text, want));
    CHECK(fips_native_stream_max_payload(flow) == 1362);
    /* The descriptor is a live half of the flow: the daemon's side sees it. */
    CHECK(fips_native_send(flow, "alive", 5) == 0);
    char buf[64]; CHECK(recv(pair[0], buf, sizeof buf, 0) == 5 && !memcmp(buf, "alive", 5));
    CHECK(fcntl(fips_native_stream_fd(flow), F_GETFD) & FD_CLOEXEC);
    fips_native_stream_close(flow); close(pair[0]);
}
static void test_bind_keeps_the_listener_and_reports_the_port(void)
{
    int conn[2], pair[2]; stream_pair(conn); CHECK(fipsd_native_pair(pair) == 0);
    fake f = {.fd = conn[0], .pass = pair[1], .reply = LISTEN_REPLY, .after = -2};
    pthread_t thread; CHECK(pthread_create(&thread, NULL, fake_daemon, &f) == 0);
    fips_native_listener *listener = NULL;
    CHECK(fips_native__listen_on(conn[1], 0, &listener) == 0 && listener);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(strstr(f.command, "\"command\":\"listen\"") && strstr(f.command, "\"local_port\":0") && f.after == 0);
    CHECK(!strstr(f.command, "\"framing\":\"stream\"") == !FIPSD_NATIVE_STREAM_FRAMING);
    fips_native_addr local = fips_native_listener_local_addr(listener), want = addr_of(NODE, 4242);
    CHECK(fips_native_addr_eq(&local, &want));
    CHECK(send(pair[0], "alive", 5, 0) == 5);
    char buf[8]; CHECK(recv(fips_native_listener_fd(listener), buf, sizeof buf, 0) == 5);
    fips_native_listener_close(listener); close(pair[0]);
}
static void test_setup_refusals_and_deadlines(void)
{
    /* A refusal becomes the error a bind would have returned. */
    int conn[2]; stream_pair(conn);
    fake f = {.fd = conn[0], .pass = -1, .reply = REFUSAL, .close_after = true};
    pthread_t thread; CHECK(pthread_create(&thread, NULL, fake_daemon, &f) == 0);
    fips_native_listener *listener = NULL;
    CHECK(fips_native__listen_on(conn[1], 4242, &listener) == -EADDRINUSE && !listener);
    CHECK(pthread_join(thread, NULL) == 0);
    /* A daemon that never answers is the one producer of ETIMEDOUT. */
    stream_pair(conn);
    struct timeval tv = {0, 50 * 1000};
    CHECK(setsockopt(conn[1], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) == 0);
    CHECK(fips_native__listen_on(conn[1], 4242, &listener) == -ETIMEDOUT);
    close(conn[0]);
    /* Gone before the command: EPIPE; gone after it: ECONNREFUSED. */
    stream_pair(conn); close(conn[0]);
    CHECK(fips_native__listen_on(conn[1], 4242, &listener) == -EPIPE);
    stream_pair(conn);
    f = (fake){.fd = conn[0], .pass = -1, .reply = NULL, .close_after = true};
    CHECK(pthread_create(&thread, NULL, fake_daemon, &f) == 0);
    CHECK(fips_native__listen_on(conn[1], 4242, &listener) == -ECONNREFUSED);
    CHECK(pthread_join(thread, NULL) == 0);
    /* A reply with no descriptor, and a descriptor on a read that completed no line. */
    stream_pair(conn);
    f = (fake){.fd = conn[0], .pass = -1, .reply = LISTEN_REPLY, .close_after = true};
    CHECK(pthread_create(&thread, NULL, fake_daemon, &f) == 0);
    CHECK(fips_native__listen_on(conn[1], 4242, &listener) == -EPROTO);
    CHECK(pthread_join(thread, NULL) == 0);
    int pair[2]; stream_pair(conn); CHECK(fipsd_native_pair(pair) == 0);
    f = (fake){.fd = conn[0], .pass = pair[1], .reply = "{\"status\":", .close_after = true};
    CHECK(pthread_create(&thread, NULL, fake_daemon, &f) == 0);
    fips_native_stream *flow = NULL; fips_native_addr peer = addr_of(PEER, 4242);
    CHECK(fips_native__connect_on(conn[1], 0, &peer, &flow) == -EPROTO);
    CHECK(pthread_join(thread, NULL) == 0);
    close(pair[1]);
    /* The refused descriptor was closed rather than held: its far half reads end of file. */
    char buf[8]; size_t len; CHECK(fipsd_native_set_nonblocking(pair[0], true) == 0);
    CHECK(fipsd_native_recv_once(pair[0], buf, sizeof buf, &len) == FIPSD_NATIVE_RECEIVED_EOF);
    close(pair[0]);
}
/* The association rule: a reply split across two reads attaches its
 * descriptor to the line the second read completes. */
static void *split_daemon(void *arg)
{
    fake *f = arg;
    read_line(f->fd, f->command, sizeof f->command);
    size_t half = strlen(CONNECT_REPLY) / 2;
    CHECK(send(f->fd, CONNECT_REPLY, half, 0) == (ssize_t)half);
    CHECK(send_fd(f->fd, CONNECT_REPLY + half, strlen(CONNECT_REPLY) - half, f->pass) == (ssize_t)(strlen(CONNECT_REPLY) - half));
    char rest[16]; f->after = read(f->fd, rest, sizeof rest);
    close(f->pass); close(f->fd);
    return NULL;
}
static void test_a_descriptor_lands_on_the_line_the_read_completed(void)
{
    int conn[2], pair[2]; stream_pair(conn); CHECK(fipsd_native_pair(pair) == 0);
    fake f = {.fd = conn[0], .pass = pair[1]};
    pthread_t thread; CHECK(pthread_create(&thread, NULL, split_daemon, &f) == 0);
    fips_native_stream *flow = NULL; fips_native_addr peer = addr_of(PEER, 4242);
    CHECK(fips_native__connect_on(conn[1], 0, &peer, &flow) == 0 && flow);
    CHECK(pthread_join(thread, NULL) == 0 && f.after == 0);
    CHECK(fips_native_send(flow, "alive", 5) == 0);
    char buf[8]; CHECK(recv(pair[0], buf, sizeof buf, 0) == 5);
    fips_native_stream_close(flow); close(pair[0]);
}
static fips_native_stream *stream_over(int fds[2], size_t max)
{
    CHECK(fipsd_native_pair(fds) == 0); bounded(fds[1]);
    fips_native_addr peer = addr_of(PEER, 5001), local = addr_of(NODE, 4242);
    fips_native_stream *s = NULL;
    CHECK(fips_native__stream_wrap(fds[0], &peer, &local, max, &s) == 0);
    return s;
}
static ssize_t recv_bounded(fips_native_stream *s, void *buf, size_t cap)
{
    int64_t previous; CHECK(fips_native_stream_read_timeout(s, &previous) == 0);
    CHECK(fips_native_stream_set_read_timeout(s, DEADLINE_MS) == 0);
    ssize_t n = fips_native_recv(s, buf, cap);
    CHECK(fips_native_stream_set_read_timeout(s, previous) == 0);
    CHECK(n != -EAGAIN);
    return n;
}
static void test_flows(void)
{
    int fds[2]; char buf[64];
    /* Datagrams cross whole in both directions. */
    fips_native_stream *s = stream_over(fds, 1362);
    CHECK(fips_native_send(s, "out", 3) == 0 && fips_native_send(s, "again", 5) == 0);
    CHECK(recv(fds[1], buf, sizeof buf, 0) == 3 && !memcmp(buf, "out", 3));
    CHECK(recv(fds[1], buf, sizeof buf, 0) == 5 && !memcmp(buf, "again", 5));
    CHECK(send(fds[1], "back", 4, 0) == 4);
    CHECK(recv_bounded(s, buf, sizeof buf) == 4 && !memcmp(buf, "back", 4));
    /* An empty datagram is not a closed flow; the close itself is EPIPE. */
    CHECK(send(fds[1], NULL, 0, 0) == 0);
    CHECK(recv_bounded(s, buf, sizeof buf) == 0);
    close(fds[1]);
    CHECK(recv_bounded(s, buf, sizeof buf) == -EPIPE);
    CHECK(fips_native_send(s, "x", 1) == -EPIPE);
    fips_native_stream_close(s);
    /* A datagram above the limit is refused before it is sent. */
    s = stream_over(fds, 8);
    unsigned char nine[9] = {0};
    CHECK(fips_native_send(s, nine, 9) == -EMSGSIZE);
    struct timeval tv = {0, 50 * 1000}; CHECK(setsockopt(fds[1], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) == 0);
    CHECK(recv(fds[1], buf, sizeof buf, 0) < 0);
    CHECK(fips_native_send(s, nine, 8) == 0 && recv(fds[1], buf, sizeof buf, 0) == 8);
    close(fds[1]); fips_native_stream_close(s);
    /* An empty datagram before the daemon closes does not swallow what follows. */
    s = stream_over(fds, 1024);
    CHECK(send(fds[1], NULL, 0, 0) == 0 && send(fds[1], "after", 5, 0) == 5); close(fds[1]);
    CHECK(recv_bounded(s, buf, sizeof buf) == 0);
    CHECK(recv_bounded(s, buf, sizeof buf) == 5 && !memcmp(buf, "after", 5));
    CHECK(recv_bounded(s, buf, sizeof buf) == -EPIPE);
    fips_native_stream_close(s);
    /* Deadlines: zero refused, read back, None clears, directions separate. */
    s = stream_over(fds, 1362);
    int64_t ms;
    CHECK(fips_native_stream_set_read_timeout(s, 0) == -EINVAL);
    CHECK(fips_native_stream_read_timeout(s, &ms) == 0 && ms == -1 && fips_native_stream_write_timeout(s, &ms) == 0 && ms == -1);
    CHECK(fips_native_stream_set_read_timeout(s, 3000) == 0 && fips_native_stream_read_timeout(s, &ms) == 0 && ms == 3000);
    CHECK(fips_native_stream_write_timeout(s, &ms) == 0 && ms == -1);
    CHECK(fips_native_stream_set_write_timeout(s, 2000) == 0 && fips_native_stream_write_timeout(s, &ms) == 0 && ms == 2000);
    CHECK(fips_native_stream_read_timeout(s, &ms) == 0 && ms == 3000);
    CHECK(fips_native_stream_set_read_timeout(s, -1) == 0 && fips_native_stream_read_timeout(s, &ms) == 0 && ms == -1);
    CHECK(fips_native_stream_write_timeout(s, &ms) == 0 && ms == 2000);
    /* A read deadline expires rather than waiting for a datagram that never comes. */
    CHECK(fips_native_stream_set_read_timeout(s, 150) == 0);
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    CHECK(fips_native_recv(s, buf, sizeof buf) == -EAGAIN);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    CHECK((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000 >= 100);
    /* Non-blocking reports would-block; clearing it leaves the other flags alone. */
    int raw = fips_native_stream_fd(s);
    CHECK(!(fcntl(raw, F_GETFL) & O_NONBLOCK));
    CHECK(fips_native_stream_set_nonblocking(s, true) == 0 && fips_native_recv(s, buf, sizeof buf) == -EAGAIN);
    int start = fcntl(raw, F_GETFL);
    CHECK(fcntl(raw, F_SETFL, start | O_ASYNC) == 0);
    int original = fcntl(raw, F_GETFL); CHECK(original & O_ASYNC);
    CHECK(fips_native_stream_set_nonblocking(s, false) == 0);
    int after = fcntl(raw, F_GETFL);
    CHECK(!(after & O_NONBLOCK) && (after & O_ASYNC) && after == (original & ~O_NONBLOCK));
    close(fds[1]); fips_native_stream_close(s);
}
/* ---- nostrc-6ljk8: framed stream flows (native/frame.h) -------------------- */
static void test_frame_parser(void)
{
    uint8_t buf[16]; uint32_t len = 99;
    CHECK(fipsd_native_frame_take(buf, 0, &len) == FIPSD_NATIVE_FRAME_PARTIAL && len == 99);
    fipsd_native_frame_header(buf, 3); memcpy(buf + 4, "abc", 3);
    CHECK(buf[0] == 0 && buf[1] == 0 && buf[2] == 0 && buf[3] == 3 && fipsd_native_frame_length(buf) == 3);
    CHECK(fipsd_native_frame_take(buf, 3, &len) == FIPSD_NATIVE_FRAME_PARTIAL);
    CHECK(fipsd_native_frame_take(buf, 6, &len) == FIPSD_NATIVE_FRAME_PARTIAL && len == 3);
    CHECK(fipsd_native_frame_take(buf, 7, &len) == FIPSD_NATIVE_FRAME_WHOLE && len == 3);
    CHECK(fipsd_native_frame_take(buf, 16, &len) == FIPSD_NATIVE_FRAME_WHOLE && len == 3);
    fipsd_native_frame_header(buf, 0); CHECK(fipsd_native_frame_take(buf, 4, &len) == FIPSD_NATIVE_FRAME_WHOLE && len == 0);
    fipsd_native_frame_header(buf, FIPSD_NATIVE_FRAME_MAX); CHECK(fipsd_native_frame_take(buf, 4, &len) == FIPSD_NATIVE_FRAME_PARTIAL && len == FIPSD_NATIVE_FRAME_MAX);
    fipsd_native_frame_header(buf, FIPSD_NATIVE_FRAME_MAX + 1); CHECK(fipsd_native_frame_take(buf, 4, &len) == FIPSD_NATIVE_FRAME_INVALID);
    fipsd_native_frame_header(buf, 0x01020304u); CHECK(buf[0] == 1 && buf[1] == 2 && buf[2] == 3 && buf[3] == 4 && fipsd_native_frame_length(buf) == 0x01020304u);
}
/* The daemon's half of a framed flow, written and read by hand. */
static fips_native_stream *framed_over(int fds[2], size_t max)
{
    CHECK(fipsd_native_pair_framed(fds) == 0); bounded(fds[1]);
    fips_native_addr peer = addr_of(PEER, 5001), local = addr_of(NODE, 4242);
    fips_native_stream *s = NULL;
    CHECK(fips_native__stream_wrap(fds[0], &peer, &local, max, &s) == 0);
    return s;
}
static void put_frame(int fd, const void *payload, uint32_t len)
{
    uint8_t header[FIPSD_NATIVE_FRAME_HEADER]; fipsd_native_frame_header(header, len);
    CHECK(send(fd, header, sizeof header, FIPSD_NATIVE_MSG_NOSIGNAL) == (ssize_t)sizeof header);
    if (len) CHECK(send(fd, payload, len, FIPSD_NATIVE_MSG_NOSIGNAL) == (ssize_t)len);
}
static void take_exact(int fd, void *buf, size_t len)
{
    for (size_t at = 0; at < len;) {
        ssize_t n = recv(fd, (uint8_t *)buf + at, len - at, 0);
        if (n < 0 && errno == EINTR) continue;
        CHECK(n > 0); at += (size_t)n;
    }
}
static uint32_t take_frame(int fd, void *buf, size_t cap)
{
    uint8_t header[FIPSD_NATIVE_FRAME_HEADER]; take_exact(fd, header, sizeof header);
    uint32_t len = fipsd_native_frame_length(header); CHECK(len <= cap);
    take_exact(fd, buf, len);
    return len;
}
typedef struct sender { fips_native_stream *s; uint8_t tag; unsigned count; size_t len; } sender;
static void *send_many(void *arg)
{
    sender *w = arg;
    uint8_t *buf = malloc(w->len); CHECK(buf);
    int fd = fips_native_stream_fd(w->s);
    for (unsigned i = 0; i < w->count; ++i) {
        memset(buf, w->tag, w->len); memcpy(buf, &i, sizeof i);
        for (;;) {
            int rc = fips_native_send(w->s, buf, w->len);
            if (rc == 0) break;
            CHECK(rc == -EAGAIN); /* non-blocking with nothing of this frame written */
            struct pollfd p = {.fd = fd, .events = POLLOUT};
            CHECK(poll(&p, 1, DEADLINE_MS) == 1);
        }
    }
    free(buf);
    return NULL;
}
static void test_framed_flows(void)
{
    int fds[2]; uint8_t buf[64];
    /* Datagrams cross whole in both directions as frames. */
    fips_native_stream *s = framed_over(fds, 1362);
    CHECK(fips_native_send(s, "out", 3) == 0 && fips_native_send(s, "again", 5) == 0 && fips_native_send(s, NULL, 0) == 0);
    CHECK(take_frame(fds[1], buf, sizeof buf) == 3 && !memcmp(buf, "out", 3));
    CHECK(take_frame(fds[1], buf, sizeof buf) == 5 && !memcmp(buf, "again", 5));
    CHECK(take_frame(fds[1], buf, sizeof buf) == 0);
    put_frame(fds[1], "back", 4);
    CHECK(recv_bounded(s, buf, sizeof buf) == 4 && !memcmp(buf, "back", 4));
    /* Two frames in one write: each recv takes one and leaves the next in
     * the kernel, so the descriptor stays readable for it. */
    uint8_t two[] = {0, 0, 0, 2, 'h', 'i', 0, 0, 0, 3, 'y', 'o', 'u'};
    CHECK(send(fds[1], two, sizeof two, 0) == (ssize_t)sizeof two);
    CHECK(recv_bounded(s, buf, sizeof buf) == 2 && !memcmp(buf, "hi", 2));
    struct pollfd p = {.fd = fips_native_stream_fd(s), .events = POLLIN};
    CHECK(poll(&p, 1, 0) == 1);
    CHECK(recv_bounded(s, buf, sizeof buf) == 3 && !memcmp(buf, "you", 3));
    /* A frame in pieces: non-blocking reads report would-block and keep what
     * they took; so does an expired deadline. */
    CHECK(fips_native_stream_set_nonblocking(s, true) == 0);
    uint8_t header[FIPSD_NATIVE_FRAME_HEADER]; fipsd_native_frame_header(header, 6);
    CHECK(send(fds[1], header, 2, 0) == 2);
    CHECK(fips_native_recv(s, buf, sizeof buf) == -EAGAIN);
    CHECK(send(fds[1], header + 2, 2, 0) == 2 && send(fds[1], "pie", 3, 0) == 3);
    CHECK(fips_native_recv(s, buf, sizeof buf) == -EAGAIN);
    CHECK(fips_native_stream_set_nonblocking(s, false) == 0 && fips_native_stream_set_read_timeout(s, 100) == 0);
    CHECK(fips_native_recv(s, buf, sizeof buf) == -EAGAIN);
    CHECK(send(fds[1], "ces", 3, 0) == 3);
    CHECK(fips_native_stream_set_read_timeout(s, -1) == 0);
    CHECK(recv_bounded(s, buf, sizeof buf) == 6 && !memcmp(buf, "pieces", 6));
    /* A datagram longer than the buffer is truncated, the rest discarded. */
    put_frame(fds[1], "truncated", 9); put_frame(fds[1], "next", 4);
    CHECK(recv_bounded(s, buf, 5) == 5 && !memcmp(buf, "trunc", 5));
    CHECK(recv_bounded(s, buf, sizeof buf) == 4 && !memcmp(buf, "next", 4));
    /* An empty datagram is not a closed flow, even last; the close is EPIPE.
     * Everything is queued before the close, so the reads cannot wait (and
     * Darwin refuses a deadline on a stream whose peer has closed). */
    put_frame(fds[1], NULL, 0); put_frame(fds[1], "after", 5); put_frame(fds[1], NULL, 0);
    close(fds[1]);
    CHECK(fips_native_recv(s, buf, sizeof buf) == 0);
    CHECK(fips_native_recv(s, buf, sizeof buf) == 5 && !memcmp(buf, "after", 5));
    CHECK(fips_native_recv(s, buf, sizeof buf) == 0);
    CHECK(fips_native_recv(s, buf, sizeof buf) == -EPIPE);
    CHECK(fips_native_send(s, "x", 1) == -EPIPE);
    fips_native_stream_close(s);
    /* A close mid-frame is EPIPE; a length past the maximum is not this protocol. */
    s = framed_over(fds, 1362);
    fipsd_native_frame_header(header, 10); CHECK(send(fds[1], header, sizeof header, 0) == 4 && send(fds[1], "abc", 3, 0) == 3);
    close(fds[1]);
    CHECK(fips_native_recv(s, buf, sizeof buf) == -EPIPE);
    fips_native_stream_close(s);
    s = framed_over(fds, 1362);
    fipsd_native_frame_header(header, FIPSD_NATIVE_FRAME_MAX + 1); CHECK(send(fds[1], header, sizeof header, 0) == 4);
    CHECK(recv_bounded(s, buf, sizeof buf) == -EPROTO && recv_bounded(s, buf, sizeof buf) == -EPROTO);
    close(fds[1]); fips_native_stream_close(s);
    /* Above the limit is refused before any byte is written. */
    s = framed_over(fds, 8);
    unsigned char nine[9] = {0};
    CHECK(fips_native_send(s, nine, 9) == -EMSGSIZE);
    CHECK(fips_native_send(s, nine, 8) == 0 && take_frame(fds[1], buf, sizeof buf) == 8);
    close(fds[1]); fips_native_stream_close(s);
    /* Two threads sending at once never interleave frames, and a
     * non-blocking sender whose frame only partly fits finishes it: the far
     * side reads every frame whole. */
    enum { COUNT = 200, BIG = 30000 };
    s = framed_over(fds, FIPSD_NATIVE_FRAME_MAX);
    CHECK(fips_native_stream_set_nonblocking(s, true) == 0);
    sender w[2] = {{.s = s, .tag = 0xa1, .count = COUNT, .len = BIG}, {.s = s, .tag = 0xb2, .count = COUNT, .len = 700}};
    pthread_t threads[2];
    for (int i = 0; i < 2; ++i) CHECK(pthread_create(&threads[i], NULL, send_many, &w[i]) == 0);
    uint8_t *big = malloc(BIG); CHECK(big);
    unsigned next[2] = {0, 0};
    for (unsigned got = 0; got < 2 * COUNT; ++got) {
        uint32_t len = take_frame(fds[1], big, BIG);
        int which = len == BIG ? 0 : 1;
        CHECK(len == w[which].len);
        unsigned seq; memcpy(&seq, big, sizeof seq);
        CHECK(seq == next[which]++); /* one sender's frames stay in its order */
        for (size_t i = sizeof seq; i < len; ++i) CHECK(big[i] == w[which].tag);
    }
    for (int i = 0; i < 2; ++i) CHECK(pthread_join(threads[i], NULL) == 0);
    free(big);
    close(fds[1]); fips_native_stream_close(s);
}
static void test_listeners(void)
{
    int ours[2], flow[2]; char buf[64];
    CHECK(fipsd_native_pair(ours) == 0); CHECK(fipsd_native_pair(flow) == 0);
    bounded(ours[0]); bounded(flow[1]);
    fips_native_addr local = addr_of(NODE, 4242);
    fips_native_listener *listener = NULL;
    CHECK(fips_native__listener_wrap(ours[0], &local, &listener) == 0);
    /* A non-blocking listener reports would-block rather than waiting. */
    CHECK(fips_native_listener_set_nonblocking(listener, true) == 0);
    fips_native_stream *s = NULL; fips_native_addr peer;
    CHECK(fips_native_accept(listener, &s, &peer) == -EAGAIN);
    CHECK(fips_native_listener_set_nonblocking(listener, false) == 0);
    /* The listener is pollable: readable exactly when accept would not block. */
    struct pollfd p = {.fd = fips_native_listener_fd(listener), .events = POLLIN};
    CHECK(poll(&p, 1, 0) == 0);
    /* The daemon writes the opening datagram first and the arrival second. */
    CHECK(send(flow[1], "opening", 7, 0) == 7);
    CHECK(send_fd(ours[1], ARRIVAL, strlen(ARRIVAL), flow[0]) == (ssize_t)strlen(ARRIVAL));
    fipsd_native_probe_provoke_collection();
    p.revents = 0; CHECK(poll(&p, 1, 0) == 1 && (p.revents & POLLIN));
    CHECK(fips_native_accept(listener, &s, &peer) == 0 && s);
    close(flow[0]);
    fips_native_addr want = addr_of(PEER, 5001); CHECK(fips_native_addr_eq(&peer, &want));
    fips_native_addr got = fips_native_stream_peer_addr(s); CHECK(fips_native_addr_eq(&got, &want));
    got = fips_native_stream_local_addr(s); CHECK(fips_native_addr_eq(&got, &local));
    CHECK(fips_native_stream_max_payload(s) == 1362);
    CHECK(recv_bounded(s, buf, sizeof buf) == 7 && !memcmp(buf, "opening", 7));
    CHECK(fips_native_send(s, "back", 4) == 0 && recv(flow[1], buf, sizeof buf, 0) == 4);
    /* A flow from accept is blocking however its listener was set. */
    CHECK(!(fcntl(fips_native_stream_fd(s), F_GETFL) & O_NONBLOCK));
    fips_native_stream_close(s); close(flow[1]);
    /* An arrival with no descriptor is EPIPE; an unparseable one refuses the flow. */
    CHECK(send_fd(ours[1], ARRIVAL, strlen(ARRIVAL), -1) == (ssize_t)strlen(ARRIVAL));
    CHECK(fips_native_accept(listener, &s, NULL) == -EPIPE);
    CHECK(fipsd_native_pair(flow) == 0);
    CHECK(send_fd(ours[1], "{\"flow_id\":9}", 13, flow[0]) == 13);
    CHECK(fips_native_accept(listener, &s, NULL) == -EPROTO);
    close(flow[0]);
    size_t len; CHECK(fipsd_native_set_nonblocking(flow[1], true) == 0);
    CHECK(fipsd_native_recv_once(flow[1], buf, sizeof buf, &len) == FIPSD_NATIVE_RECEIVED_EOF);
    close(flow[1]);
    /* The daemon closing its half ends accept with EPIPE. */
    close(ours[1]);
    CHECK(fips_native_accept(listener, &s, NULL) == -EPIPE);
    fips_native_listener_close(listener);
}
int main(void)
{
    test_message_boundaries_survive_in_both_directions();
    test_the_end_of_file_rule();
    test_dgram_probes();
    test_fdpass_in_a_runtime();
    test_npub_codec_agrees_with_libfips_core();
    test_addresses();
    test_connect_returns_a_stream_and_closes_the_connection();
    test_bind_keeps_the_listener_and_reports_the_port();
    test_setup_refusals_and_deadlines();
    test_a_descriptor_lands_on_the_line_the_read_completed();
    test_flows();
    test_frame_parser();
    test_framed_flows();
    test_listeners();
    puts("native seqpacket: pair rule, probes, fd passing, client library against a fake daemon, framed streams: ok");
    return 0;
}
