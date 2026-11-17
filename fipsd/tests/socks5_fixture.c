#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "socks5_fixture.h"
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
typedef struct s5_conn { socks5_fixture *f; int client; } s5_conn;
/* poll() both the descriptor and the stop pipe: a stop wakes every thread. */
static bool wait_readable(socks5_fixture *f, int fd)
{
    for (;;) {
        struct pollfd p[] = {{fd, POLLIN, 0}, {f->stop[0], POLLIN, 0}};
        int n = poll(p, 2, -1);
        if (n < 0 && errno == EINTR) continue;
        assert(n > 0);
        return !p[1].revents;
    }
}
static bool read_full(socks5_fixture *f, int fd, void *buf, size_t n)
{
    uint8_t *p = buf;
    for (size_t got = 0; got < n;) {
        if (!wait_readable(f, fd)) return false;
        ssize_t k = read(fd, p + got, n - got);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return false;
        got += (size_t)k;
    }
    return true;
}
static bool write_full(int fd, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL; /* a client that left is EPIPE, not a signal (Apple: SO_NOSIGPIPE at accept) */
#endif
    for (size_t sent = 0; sent < n;) {
        ssize_t k = send(fd, p + sent, n - sent, flags);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return false;
        sent += (size_t)k;
    }
    return true;
}
static bool numeric(const fips_transport_addr *a, struct sockaddr_storage *ss, socklen_t *len)
{
    const char *text; size_t n; char host[300];
    if (fips_transport_addr_as_str(a, &text, &n) != FIPS_OK || n >= sizeof host) return false;
    memcpy(host, text, n); host[n] = 0;
    char *colon = strrchr(host, ':'); if (!colon) return false;
    *colon = 0; unsigned port = (unsigned)atoi(colon + 1);
    char *h = host;
    if (*h == '[') { ++h; h[strlen(h) - 1] = 0; }
    memset(ss, 0, sizeof *ss);
    struct sockaddr_in *v4 = (struct sockaddr_in *)ss; struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)ss;
    if (inet_pton(AF_INET, h, &v4->sin_addr) == 1) {
        v4->sin_family = AF_INET; v4->sin_port = htons((uint16_t)port); *len = sizeof *v4; return true;
    }
    if (inet_pton(AF_INET6, h, &v6->sin6_addr) == 1) {
        v6->sin6_family = AF_INET6; v6->sin6_port = htons((uint16_t)port); *len = sizeof *v6; return true;
    }
    return false;
}
static void relay(socks5_fixture *f, int a, int b)
{
    uint8_t buf[16384];
    for (;;) {
        struct pollfd p[] = {{a, POLLIN, 0}, {b, POLLIN, 0}, {f->stop[0], POLLIN, 0}};
        int n = poll(p, 3, -1);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0 || p[2].revents) return;
        for (int i = 0; i < 2; ++i) {
            if (!p[i].revents) continue;
            ssize_t k = read(p[i].fd, buf, sizeof buf);
            if (k <= 0 || !write_full(p[1 - i].fd, buf, (size_t)k)) return;
        }
    }
}
static void *serve_conn(void *arg)
{
    s5_conn *c = arg; socks5_fixture *f = c->f; int fd = c->client, target = -1;
    free(c);
    s5_request *r = calloc(1, sizeof *r); assert(r);
    uint8_t vn[2], methods[255], buf[512];
    if (!read_full(f, fd, vn, 2)) goto out; /* the readiness probe, or any early close */
    assert(vn[0] == 0x05 && vn[1] > 0);
    if (!read_full(f, fd, methods, vn[1])) goto out;
    r->greeting[0] = vn[0]; r->greeting[1] = vn[1];
    memcpy(r->greeting + 2, methods, vn[1] < 6 ? vn[1] : 6); r->greeting_len = 2u + (vn[1] < 6 ? vn[1] : 6);
    pthread_mutex_lock(&f->mu); s5_behaviour behaviour = f->behaviour; uint8_t rep = f->rep; pthread_mutex_unlock(&f->mu);
    if (behaviour == S5_STALL) {
        (void)go_channel_try_send(f->stalled, f);
        while (wait_readable(f, fd) && read(fd, buf, sizeof buf) > 0) {}
        goto out;
    }
    bool offered_pass = memchr(methods, 0x02, vn[1]) != NULL, offered_none = memchr(methods, 0x00, vn[1]) != NULL;
    uint8_t selected = behaviour == S5_NO_METHOD ? 0xff : offered_pass ? 0x02 : offered_none ? 0x00 : 0xff;
    uint8_t reply[2] = {0x05, selected};
    if (!write_full(fd, reply, 2) || selected == 0xff) goto out;
    if (selected == 0x02) {
        uint8_t h[2], plen;
        if (!read_full(f, fd, h, 2) || h[0] != 0x01 || !read_full(f, fd, r->username, h[1]) ||
            !read_full(f, fd, &plen, 1) || !read_full(f, fd, r->password, plen)) goto out;
        r->userpass = true;
        uint8_t status[2] = {0x01, behaviour == S5_AUTH_FAIL ? 0x01 : 0x00};
        if (!write_full(fd, status, 2) || behaviour == S5_AUTH_FAIL) goto out;
    }
    uint8_t head[4];
    if (!read_full(f, fd, head, 4)) goto out;
    assert(head[0] == 0x05 && head[1] == 0x01 && head[2] == 0x00);
    r->atyp = head[3];
    struct sockaddr_storage to; socklen_t to_len = 0; uint8_t port[2];
    if (head[3] == 0x01 || head[3] == 0x04) {
        size_t n = head[3] == 0x01 ? 4 : 16;
        if (!read_full(f, fd, buf, n) || !read_full(f, fd, port, 2)) goto out;
        (void)inet_ntop(n == 4 ? AF_INET : AF_INET6, buf, r->host, sizeof r->host);
        r->port = (uint16_t)(port[0] << 8 | port[1]);
        memset(&to, 0, sizeof to);
        if (n == 4) {
            struct sockaddr_in *v4 = (struct sockaddr_in *)&to; v4->sin_family = AF_INET;
            memcpy(&v4->sin_addr, buf, 4); v4->sin_port = htons(r->port); to_len = sizeof *v4;
        } else {
            struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)&to; v6->sin6_family = AF_INET6;
            memcpy(&v6->sin6_addr, buf, 16); v6->sin6_port = htons(r->port); to_len = sizeof *v6;
        }
    } else {
        assert(head[3] == 0x03);
        uint8_t len;
        if (!read_full(f, fd, &len, 1) || !read_full(f, fd, r->host, len) || !read_full(f, fd, port, 2)) goto out;
        r->host[len] = 0; r->port = (uint16_t)(port[0] << 8 | port[1]);
        pthread_mutex_lock(&f->mu);
        for (size_t i = 0; i < f->route_count; ++i)
            if (!strcmp(f->routes[i].host, r->host)) { to = f->routes[i].to; to_len = f->routes[i].len; }
        pthread_mutex_unlock(&f->mu);
    }
    s5_request *copy = malloc(sizeof *copy); assert(copy); *copy = *r;
    if (go_channel_try_send(f->requests, copy)) free(copy);
    uint8_t answer[10] = {0x05, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0};
    if (behaviour == S5_REPLY) { answer[1] = rep; (void)write_full(fd, answer, sizeof answer); goto out; }
    if (!to_len) { answer[1] = 0x04; (void)write_full(fd, answer, sizeof answer); goto out; } /* host unreachable */
    target = socket(to.ss_family, SOCK_STREAM, 0); assert(target >= 0);
    if (connect(target, (struct sockaddr *)&to, to_len)) { answer[1] = 0x05; (void)write_full(fd, answer, sizeof answer); goto out; }
    if (!write_full(fd, answer, sizeof answer)) goto out;
    relay(f, fd, target);
out:
    if (target >= 0) close(target);
    close(fd); free(r);
    return NULL;
}
static void *serve(void *arg)
{
    socks5_fixture *f = arg;
    while (wait_readable(f, f->fd)) {
        int fd = accept(f->fd, NULL, NULL);
        if (fd < 0) { assert(errno == EINTR || errno == ECONNABORTED || errno == EAGAIN); continue; }
#ifdef SO_NOSIGPIPE
        int one = 1; (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        s5_conn *c = malloc(sizeof *c); assert(c); c->f = f; c->client = fd;
        pthread_mutex_lock(&f->mu);
        assert(f->conn_count < S5_CONNS);
        assert(!pthread_create(&f->conns[f->conn_count++], NULL, serve_conn, c));
        pthread_mutex_unlock(&f->mu);
    }
    return NULL;
}
void socks5_fixture_start(socks5_fixture *f)
{
    memset(f, 0, sizeof *f);
    assert(!pthread_mutex_init(&f->mu, NULL));
    f->requests = go_channel_create(64); f->stalled = go_channel_create(16); assert(f->requests && f->stalled);
    f->fd = socket(AF_INET, SOCK_STREAM, 0); assert(f->fd >= 0 && !pipe(f->stop));
    int one = 1; assert(!setsockopt(f->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one));
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(!bind(f->fd, (struct sockaddr *)&a, sizeof a) && !listen(f->fd, 64));
    socklen_t len = sizeof a; assert(!getsockname(f->fd, (struct sockaddr *)&a, &len));
    snprintf(f->addr, sizeof f->addr, "127.0.0.1:%u", ntohs(a.sin_port));
    assert(fips_transport_addr_from_string(f->addr, strlen(f->addr), &f->address) == FIPS_OK);
    assert(!pthread_create(&f->thread, NULL, serve, f));
}
void socks5_fixture_stop(socks5_fixture *f)
{
    assert(write(f->stop[1], "x", 1) == 1); assert(!pthread_join(f->thread, NULL));
    for (size_t i = 0; i < f->conn_count; ++i) assert(!pthread_join(f->conns[i], NULL));
    close(f->fd); close(f->stop[0]); close(f->stop[1]);
    void *p;
    while (!go_channel_try_receive(f->requests, &p)) free(p);
    while (!go_channel_try_receive(f->stalled, &p)) {}
    go_channel_unref(f->requests); go_channel_unref(f->stalled);
    pthread_mutex_destroy(&f->mu);
}
void socks5_fixture_behave(socks5_fixture *f, s5_behaviour b, uint8_t rep)
{ pthread_mutex_lock(&f->mu); f->behaviour = b; f->rep = rep; pthread_mutex_unlock(&f->mu); }
void socks5_fixture_route(socks5_fixture *f, const char *host, const fips_transport_addr *to)
{
    pthread_mutex_lock(&f->mu);
    s5_route *r = NULL;
    for (size_t i = 0; i < f->route_count; ++i) if (!strcmp(f->routes[i].host, host)) r = &f->routes[i];
    if (!r) { assert(f->route_count < S5_ROUTES); r = &f->routes[f->route_count++]; }
    snprintf(r->host, sizeof r->host, "%s", host);
    assert(numeric(to, &r->to, &r->len));
    pthread_mutex_unlock(&f->mu);
}
