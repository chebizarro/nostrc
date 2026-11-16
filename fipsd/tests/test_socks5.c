/* SOCKS5 client (nostrc-gfi8h.4): byte vectors against the layouts
 * tokio-socks 0.5.3 writes and reads (upstream socks5/dialer.rs), the target
 * parse tor and nym apply before a dial, and live handshakes on libgo fibers
 * against the loopback fake server: success (no-auth and username/password,
 * IPv4, IPv6 and .onion targets, bytes relayed both ways), authentication
 * failure, no acceptable method, REP refused and general failure, the
 * handshake deadline against a server that never answers, and owner-directed
 * cancellation. No sleeps: every wait is a bounded channel select or the
 * handshake's own deadline. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#define _DARWIN_C_SOURCE 1
#include "transport/socks5/socks5.h"
#include "transport/presence.h"
#include "runtime.h"
#include "select.h"
#include "socks5_fixture.h"
#include "test_tmpdir.h"
#include <libgo/fiber.h>
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define ONION "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz2345.onion"
static void same(const uint8_t *got, size_t n, const uint8_t *want, size_t m)
{
    if (n != m || memcmp(got, want, n)) {
        fprintf(stderr, "vector mismatch (%zu vs %zu bytes):", n, m);
        for (size_t i = 0; i < n; ++i) fprintf(stderr, " %02x", got[i]);
        fputc('\n', stderr); assert(0);
    }
}
static void vectors(void)
{
    uint8_t b[FIPSD_SOCKS5_MAX_REQUEST];
    /* prepare_send_method_selection: Authentication::None / ::Password. */
    same(b, fipsd_socks5_greeting(b, sizeof b, false), (const uint8_t[]){5, 1, 0}, 3);
    same(b, fipsd_socks5_greeting(b, sizeof b, true), (const uint8_t[]){5, 2, 0, 2}, 4);
    assert(!fipsd_socks5_greeting(b, 3, true));
    /* prepare_send_password_auth: tor's isolation credentials. */
    const uint8_t up[] = {1, 4, 'f', 'i', 'p', 's', 13, '1', '0', '.', '0', '.', '0', '.', '1', ':', '2', '1', '2', '1'};
    same(b, fipsd_socks5_userpass(b, sizeof b, "fips", "10.0.0.1:2121"), up, sizeof up);
    char long_name[300]; memset(long_name, 'u', 256); long_name[256] = 0;
    assert(!fipsd_socks5_userpass(b, sizeof b, long_name, "p"));
    /* prepare_send_request: IPv4, IPv6 and domain ATYPs, big-endian port. */
    fipsd_socks5_target t;
    assert(fipsd_socks5_target_parse("10.0.0.1:2121", 13, true, &t) && t.kind == FIPSD_SOCKS5_TARGET_IPV4);
    same(b, fipsd_socks5_request(b, sizeof b, &t), (const uint8_t[]){5, 1, 0, 1, 10, 0, 0, 1, 0x08, 0x49}, 10);
    assert(fipsd_socks5_target_parse("[2001:db8::1]:443", 17, true, &t) && t.kind == FIPSD_SOCKS5_TARGET_IPV6);
    same(b, fipsd_socks5_request(b, sizeof b, &t), (const uint8_t[]){5, 1, 0, 4, 0x20, 0x01, 0x0d, 0xb8,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0x01, 0xbb}, 22);
    assert(fipsd_socks5_target_parse("example.com:2121", 16, true, &t) && t.kind == FIPSD_SOCKS5_TARGET_DOMAIN);
    same(b, fipsd_socks5_request(b, sizeof b, &t), (const uint8_t[]){5, 1, 0, 3, 11, 'e', 'x', 'a', 'm', 'p', 'l',
        'e', '.', 'c', 'o', 'm', 0x08, 0x49}, 18);
    const char *onion = ONION ":443";
    assert(fipsd_socks5_target_parse(onion, strlen(onion), true, &t) && t.kind == FIPSD_SOCKS5_TARGET_DOMAIN);
    size_t n = fipsd_socks5_request(b, sizeof b, &t);
    assert(n == 7 + 62 && b[3] == 3 && b[4] == 62 && !memcmp(b + 5, ONION, 62) && b[67] == 0x01 && b[68] == 0xbb);
    assert(!fipsd_socks5_request(b, 10, &t));
    /* receive_reply: version, reserved byte, ATYP decide the reply length. */
    assert(fipsd_socks5_reply_length((const uint8_t[]){5, 0, 0, 1}, 4) == 10);
    assert(fipsd_socks5_reply_length((const uint8_t[]){5, 0, 0, 4}, 4) == 22);
    assert(fipsd_socks5_reply_length((const uint8_t[]){5, 0, 0, 3}, 4) == 0);
    assert(fipsd_socks5_reply_length((const uint8_t[]){5, 0, 0, 3, 9}, 5) == 16);
    assert(fipsd_socks5_reply_length((const uint8_t[]){5, 0, 0}, 3) == 0);
    assert(fipsd_socks5_reply_length((const uint8_t[]){4, 0, 0, 1}, 4) == -1);
    assert(fipsd_socks5_reply_length((const uint8_t[]){5, 0, 1, 1}, 4) == -1);
    assert(fipsd_socks5_reply_length((const uint8_t[]){5, 0, 0, 2}, 4) == -1);
    /* parse_tor_addr / nym parse_target_addr. */
    assert(fipsd_socks5_target_parse("localhost:80", 12, false, &t) && t.kind == FIPSD_SOCKS5_TARGET_DOMAIN);
    assert(!fipsd_socks5_target_parse("localhost:80", 12, true, &t)); /* tor: must be fully qualified */
    const char *bad[] = {"example.com", "example.com:", ":80", "example.com:65536", "example.com:8a", "::1:80",
        "[::1:80", "[zz::1]:80", "exa mple.com:80"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; ++i) assert(!fipsd_socks5_target_parse(bad[i], strlen(bad[i]), false, &t));
    char huge[300]; memset(huge, 'a', 256); memcpy(huge + 256, ".b:1", 5);
    assert(!fipsd_socks5_target_parse(huge, strlen(huge), false, &t));
    assert(fipsd_socks5_errno(FIPSD_SOCKS5_REFUSED) == ECONNREFUSED && fipsd_socks5_errno(FIPSD_SOCKS5_TIMEOUT) == ETIMEDOUT);
    assert(fipsd_socks5_errno(FIPSD_SOCKS5_CANCELLED) == ECANCELED && fipsd_socks5_errno(FIPSD_SOCKS5_REJECTED) != ECONNREFUSED);
    assert(!strcmp(fipsd_socks5_result_name(FIPSD_SOCKS5_NO_METHOD), "no_method"));
    puts("vectors: tokio-socks greeting/userpass/request layouts, reply lengths, tor and nym target parse: ok");
}
typedef struct fixture {
    socks5_fixture proxy;
    fipsd_runtime *rt;
    GoChannel *done;
    int listen4, listen6;
    fips_transport_addr target4, target6;
} fixture;
static int start(fipsd_subsystem *s, fipsd_runtime *rt) { ((fixture *)s->user)->rt = rt; return 0; }
static void stop(fipsd_subsystem *s, fipsd_runtime *rt) { (void)s; (void)rt; }
static int listener(int family, fips_transport_addr *out)
{
    struct sockaddr_storage ss = {0}; socklen_t len;
    if (family == AF_INET) {
        struct sockaddr_in *a = (void *)&ss; a->sin_family = AF_INET; a->sin_addr.s_addr = htonl(INADDR_LOOPBACK); len = sizeof *a;
    } else {
        struct sockaddr_in6 *a = (void *)&ss; a->sin6_family = AF_INET6; a->sin6_addr = in6addr_loopback; len = sizeof *a;
    }
    int fd = socket(family, SOCK_STREAM, 0); assert(fd >= 0);
    assert(!bind(fd, (struct sockaddr *)&ss, len) && !listen(fd, 8) && !getsockname(fd, (struct sockaddr *)&ss, &len));
    char text[80]; char ip[64];
    if (family == AF_INET) {
        struct sockaddr_in *a = (void *)&ss; inet_ntop(AF_INET, &a->sin_addr, ip, sizeof ip);
        snprintf(text, sizeof text, "%s:%u", ip, ntohs(a->sin_port));
    } else {
        struct sockaddr_in6 *a = (void *)&ss; inet_ntop(AF_INET6, &a->sin6_addr, ip, sizeof ip);
        snprintf(text, sizeof text, "[%s]:%u", ip, ntohs(a->sin6_port));
    }
    assert(fips_transport_addr_from_string(text, strlen(text), out) == FIPS_OK);
    assert(!fcntl(fd, F_SETFL, O_NONBLOCK) && !gof_io_reset_fd(fd));
    return fd;
}
/* On a fiber: a connected, nonblocking socket to the fake proxy. */
static int dial(fixture *f)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0); assert(fd >= 0);
    assert(!fcntl(fd, F_SETFL, O_NONBLOCK) && !gof_io_reset_fd(fd));
#ifdef SO_NOSIGPIPE
    int one = 1; assert(!setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one));
#endif
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        .sin_port = htons((uint16_t)atoi(strrchr(f->proxy.addr, ':') + 1))};
    assert(gof_connect(fd, (struct sockaddr *)&a, sizeof a, 5000) == 0);
    return fd;
}
static void closefd(int fd) { (void)gof_io_reset_fd(fd); close(fd); }
static s5_request *request(fixture *f)
{
    void *v = NULL; GoSelectCase c = {GO_SELECT_RECEIVE, f->proxy.requests, NULL, &v};
    assert(go_select_timeout(&c, 1, 10000).selected_case == 0 && v); return v;
}
static fipsd_socks5_target target(const fips_transport_addr *a)
{
    const char *text; size_t n; fipsd_socks5_target t;
    assert(fips_transport_addr_as_str(a, &text, &n) == FIPS_OK && fipsd_socks5_target_parse(text, n, true, &t)); return t;
}
/* The relay works both ways: the CONNECT reached the target listener. */
static void relayed(int client, int listen_fd)
{
    int peer = gof_accept(listen_fd, NULL, NULL, 10000); assert(peer >= 0);
    assert(!fcntl(peer, F_SETFL, O_NONBLOCK) && !gof_io_reset_fd(peer));
    uint8_t buf[8];
    assert(gof_write(client, "ping", 4) == 4 && gof_read(peer, buf, 4) == 4 && !memcmp(buf, "ping", 4));
    assert(gof_write(peer, "pong", 4) == 4 && gof_read(client, buf, 4) == 4 && !memcmp(buf, "pong", 4));
    closefd(peer);
}
static fipsd_socks5_result attempt(fixture *f, const fipsd_socks5_auth *auth, const fipsd_socks5_target *t,
    uint64_t wait_ms, uint8_t *rep, int *fd_out)
{
    int fd = dial(f);
    fipsd_socks5_result r = fipsd_socks5_handshake(fd, auth, t, fipsd_transport_now_ms() + wait_ms, rep);
    if (fd_out) *fd_out = fd; else closefd(fd);
    return r;
}
typedef struct canceller { fixture *f; int fd; GoChannel *done; } canceller;
static void cancel_when_stalled(void *arg)
{
    canceller *c = arg; void *v = NULL;
    GoSelectCase s = {GO_SELECT_RECEIVE, c->f->proxy.stalled, NULL, &v};
    assert(go_select_timeout(&s, 1, 10000).selected_case == 0);
    assert(gof_io_cancel_fd(c->fd) == 0); /* owner-directed: the handshake's next or pending wait ends */
    go_channel_close(c->done);
}
static void scenario(void *arg)
{
    fixture *f = arg; uint8_t rep = 0xee; int fd;
    fipsd_socks5_auth none = {0}, pass = {.username = "fips", .password = ONION ":443"};
    /* No-auth CONNECT to a numeric IPv4 target (nym's dial). */
    fipsd_socks5_target t4 = target(&f->target4);
    assert(attempt(f, &none, &t4, 10000, &rep, &fd) == FIPSD_SOCKS5_OK && rep == 0);
    s5_request *r = request(f);
    same(r->greeting, r->greeting_len, (const uint8_t[]){5, 1, 0}, 3);
    assert(!r->userpass && r->atyp == 1 && !strcmp(r->host, "127.0.0.1") && r->port == t4.port); free(r);
    relayed(fd, f->listen4); closefd(fd);
    /* Username/password CONNECT to an onion (tor's isolated dial). */
    const char *onion = ONION ":443"; fipsd_socks5_target to;
    assert(fipsd_socks5_target_parse(onion, strlen(onion), true, &to));
    socks5_fixture_route(&f->proxy, ONION, &f->target4);
    assert(attempt(f, &pass, &to, 10000, &rep, &fd) == FIPSD_SOCKS5_OK);
    r = request(f);
    same(r->greeting, r->greeting_len, (const uint8_t[]){5, 2, 0, 2}, 4);
    assert(r->userpass && !strcmp(r->username, "fips") && !strcmp(r->password, onion));
    assert(r->atyp == 3 && !strcmp(r->host, ONION) && r->port == 443); free(r);
    relayed(fd, f->listen4); closefd(fd);
    /* IPv6 target. */
    if (f->listen6 >= 0) {
        fipsd_socks5_target t6 = target(&f->target6);
        assert(attempt(f, &pass, &t6, 10000, &rep, &fd) == FIPSD_SOCKS5_OK);
        r = request(f); assert(r->atyp == 4 && !strcmp(r->host, "::1") && r->port == t6.port); free(r);
        relayed(fd, f->listen6); closefd(fd);
    }
    /* Failure classes, each as tokio-socks reports it. */
    socks5_fixture_behave(&f->proxy, S5_AUTH_FAIL, 0);
    assert(attempt(f, &pass, &t4, 10000, &rep, NULL) == FIPSD_SOCKS5_AUTH_FAILED);
    socks5_fixture_behave(&f->proxy, S5_NO_METHOD, 0);
    assert(attempt(f, &none, &t4, 10000, &rep, NULL) == FIPSD_SOCKS5_NO_METHOD);
    socks5_fixture_behave(&f->proxy, S5_REPLY, 0x05);
    assert(attempt(f, &pass, &to, 10000, &rep, NULL) == FIPSD_SOCKS5_REFUSED && rep == 0x05);
    free(request(f));
    socks5_fixture_behave(&f->proxy, S5_REPLY, 0x01);
    assert(attempt(f, &none, &t4, 10000, &rep, NULL) == FIPSD_SOCKS5_REJECTED && rep == 0x01);
    free(request(f));
    /* Slow handshake: the server takes the greeting and never answers; the
     * absolute deadline ends the handshake, not the peer. */
    socks5_fixture_behave(&f->proxy, S5_STALL, 0);
    uint64_t began = fipsd_transport_now_ms();
    assert(attempt(f, &pass, &to, 300, &rep, NULL) == FIPSD_SOCKS5_TIMEOUT);
    uint64_t took = fipsd_transport_now_ms() - began;
    assert(took >= 290 && took < 5000);
    void *v; assert(go_channel_receive(f->proxy.stalled, &v) == 0);
    /* An already-expired deadline never touches the socket. */
    assert(attempt(f, &pass, &to, 0, &rep, NULL) == FIPSD_SOCKS5_TIMEOUT);
    /* Owner cancellation of a parked handshake (gof_io_cancel_fd latch). */
    fd = dial(f);
    canceller c = {.f = f, .fd = fd, .done = go_channel_create(1)}; assert(c.done);
    assert(fipsd_runtime_spawn(f->rt, cancel_when_stalled, &c) == 0);
    began = fipsd_transport_now_ms();
    assert(fipsd_socks5_handshake(fd, &pass, &to, fipsd_transport_now_ms() + 60000, &rep) == FIPSD_SOCKS5_CANCELLED);
    assert(fipsd_transport_now_ms() - began < 30000);
    assert(go_channel_receive(c.done, &v) == 0 || go_channel_is_closed(c.done));
    go_channel_unref(c.done); closefd(fd);
    socks5_fixture_behave(&f->proxy, S5_RELAY, 0);
    puts("handshake: success both auth modes and ATYPs, auth failure, no method, refused, rejected, deadline, cancel: ok");
    assert(go_channel_send(f->done, f) == 0);
}
int main(void)
{
    (void)fipsd_test_tmpdir(NULL);
    vectors();
    fixture f = {.done = go_channel_create(1)}; assert(f.done);
    socks5_fixture_start(&f.proxy);
    f.listen4 = listener(AF_INET, &f.target4);
    int probe = socket(AF_INET6, SOCK_STREAM, 0);
    f.listen6 = -1;
    if (probe >= 0) { close(probe); f.listen6 = listener(AF_INET6, &f.target6); }
    fipsd_runtime_config rc; fipsd_runtime_config_default(&rc); rc.workers = 2; rc.handle_signals = false;
    fipsd_runtime *rt = fipsd_runtime_create(&rc); assert(rt);
    fipsd_subsystem sub = {.name = "socks5-test", .start = start, .stop = stop, .user = &f};
    assert(!fipsd_runtime_register(rt, &sub) && !fipsd_runtime_start(rt));
    assert(fipsd_runtime_spawn(rt, scenario, &f) == 0);
    void *v = NULL; GoSelectCase c = {GO_SELECT_RECEIVE, f.done, NULL, &v};
    assert(go_select_timeout(&c, 1, 60000).selected_case == 0);
    fipsd_runtime_shutdown(rt); assert(fipsd_runtime_run(rt) == FIPSD_RUN_OK);
    fipsd_runtime_destroy(rt);
    socks5_fixture_stop(&f.proxy);
    closefd(f.listen4); if (f.listen6 >= 0) closefd(f.listen6);
    go_channel_unref(f.done);
    return 0;
}
