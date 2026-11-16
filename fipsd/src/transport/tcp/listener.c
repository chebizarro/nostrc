#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#define _DARWIN_C_SOURCE 1
#include "transport/tcp/internal.h"
#include "blocking_executor.h"
#include "log.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

__attribute__((noinline)) int tcp_io_errno(void) { return errno; }

typedef struct lookup { char text[257]; bool numeric; tcp_endpoint *items; size_t count; } lookup;
static void *resolve(void *arg)
{
    lookup *q = arg;
    char *host = q->text, *port;
    if (*host == '[') {
        char *end = strchr(host, ']'); if (!end || end[1] != ':') return NULL;
        *end = 0; ++host; port = end + 2;
    } else { port = strrchr(host, ':'); if (!port) return NULL; *port++ = 0; }
    if (!*host || !*port) return NULL;
    unsigned n = 0;
    for (const char *p = port; *p; ++p) {
        if (*p < '0' || *p > '9' || n > (65535u - (unsigned)(*p - '0')) / 10) return NULL;
        n = n * 10 + (unsigned)(*p - '0');
    }
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM,
        .ai_protocol = IPPROTO_TCP, .ai_flags = AI_NUMERICSERV | (q->numeric ? AI_NUMERICHOST : 0)};
    struct addrinfo *answer = NULL;
    if (getaddrinfo(host, port, &hints, &answer)) return NULL;
    for (struct addrinfo *a = answer; a; a = a->ai_next)
        if ((a->ai_family == AF_INET || a->ai_family == AF_INET6) && a->ai_addrlen <= sizeof(struct sockaddr_storage)) ++q->count;
    q->items = calloc(q->count, sizeof *q->items);
    if (q->items) {
        size_t i = 0;
        for (struct addrinfo *a = answer; a; a = a->ai_next) {
            if ((a->ai_family != AF_INET && a->ai_family != AF_INET6) || a->ai_addrlen > sizeof(struct sockaddr_storage)) continue;
            memcpy(&q->items[i].address, a->ai_addr, a->ai_addrlen); q->items[i++].len = (socklen_t)a->ai_addrlen;
        }
    } else q->count = 0;
    freeaddrinfo(answer); return NULL;
}
bool tcp_resolve(const fips_transport_addr *peer, bool numeric, tcp_endpoint **items, size_t *count)
{
    if (!peer || !peer->len || peer->len > 256 || memchr(peer->bytes, 0, peer->len)) return false;
    lookup q = {.numeric = numeric}; memcpy(q.text, peer->bytes, peer->len);
    if (numeric) resolve(&q); else go_blocking_submit(resolve, &q);
    *items = q.items; *count = q.count; return q.count > 0;
}
bool tcp_endpoint_addr(const tcp_endpoint *e, fips_transport_addr *out)
{
    if (e->address.ss_family == AF_INET) {
        const struct sockaddr_in *a = (const void *)&e->address;
        return fips_transport_addr_from_socket_addr((const uint8_t *)&a->sin_addr, 4, ntohs(a->sin_port), 0, out) == FIPS_OK;
    }
    if (e->address.ss_family == AF_INET6) {
        const struct sockaddr_in6 *a = (const void *)&e->address;
        return fips_transport_addr_from_socket_addr((const uint8_t *)&a->sin6_addr, 16, ntohs(a->sin6_port), a->sin6_scope_id, out) == FIPS_OK;
    }
    return false;
}
static int option(int fd, int level, int name, int v)
{ return setsockopt(fd, level, name, &v, sizeof v); }
int tcp_socket_configure(tcp_instance *u, int fd)
{
    if (fcntl(fd, F_SETFL, O_NONBLOCK) || fcntl(fd, F_SETFD, FD_CLOEXEC) ||
        option(fd, IPPROTO_TCP, TCP_NODELAY, u->config.nodelay ? 1 : 0) ||
        option(fd, SOL_SOCKET, SO_KEEPALIVE, u->config.keepalive_secs ? 1 : 0)) return -1;
#ifdef SO_NOSIGPIPE
    if (option(fd, SOL_SOCKET, SO_NOSIGPIPE, 1)) return -1;
#endif
    if (u->config.keepalive_secs) {
#ifdef TCP_KEEPIDLE
        if (option(fd, IPPROTO_TCP, TCP_KEEPIDLE, (int)u->config.keepalive_secs)) return -1;
#elif defined(TCP_KEEPALIVE)
        if (option(fd, IPPROTO_TCP, TCP_KEEPALIVE, (int)u->config.keepalive_secs)) return -1;
#else
        errno = ENOTSUP; return -1;
#endif
    }
    fipsd_buffer_granted recv, send;
    if (fipsd_sockbuf_set(fd, FIPSD_SOCKET_RECV, u->config.recv_buf_size, &recv) ||
        fipsd_sockbuf_set(fd, FIPSD_SOCKET_SEND, u->config.send_buf_size, &send)) return -1;
    if (fipsd_sockbuf_clamped(&recv) || fipsd_sockbuf_clamped(&send))
        FIPSD_WARN("tcp", "%s: buffers clamped (recv %zu/%zu, send %zu/%zu)", u->name,
            recv.actual, recv.requested, send.actual, send.requested);
    return 0;
}
uint16_t tcp_socket_mss(int fd, uint16_t mtu)
{
#ifdef __linux__
    int mss = 0; socklen_t len = sizeof mss;
    if (!getsockopt(fd, IPPROTO_TCP, TCP_MAXSEG, &mss, &len) && mss > 0 && mss < mtu) return (uint16_t)mss;
#else
    (void)fd;
#endif
    return mtu;
}
static void *close_fd(void *arg) { close(*(int *)arg); return NULL; }
void tcp_fd_close(int fd)
{
    if (fd < 0) return;
    (void)gof_io_reset_fd(fd); /* owner has joined every user */
    go_blocking_submit(close_fd, &fd);
}
typedef struct listen_setup { tcp_endpoint endpoint; int fd; } listen_setup;
static void *open_listener(void *arg)
{
    listen_setup *s = arg;
    int fd = socket(s->endpoint.address.ss_family, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return NULL;
    if (fcntl(fd, F_SETFL, O_NONBLOCK) || fcntl(fd, F_SETFD, FD_CLOEXEC) || option(fd, SOL_SOCKET, SO_REUSEADDR, 1)) goto fail;
    if (s->endpoint.address.ss_family == AF_INET6 && option(fd, IPPROTO_IPV6, IPV6_V6ONLY, 1)) goto fail;
    if (bind(fd, (const struct sockaddr *)&s->endpoint.address, s->endpoint.len) || listen(fd, 128)) goto fail;
    s->endpoint.len = sizeof s->endpoint.address;
    if (getsockname(fd, (struct sockaddr *)&s->endpoint.address, &s->endpoint.len)) goto fail;
    s->fd = fd; return NULL;
fail:
    close(fd); return NULL;
}
static void accept_loop(void *arg)
{
    tcp_instance *u = arg;
    for (;;) {
        if (go_channel_is_closed(u->stop) || go_context_is_canceled(u->root)) break;
        tcp_endpoint remote = {.len = sizeof remote.address};
        int fd = gof_accept(u->listener_fd, (struct sockaddr *)&remote.address, &remote.len, -1);
        if (fd < 0) {
            if (tcp_io_errno() == EINTR) continue;
            break;
        }
        tcp_endpoint local = {.len = sizeof local.address}; fips_transport_addr peer;
        if (getsockname(fd, (struct sockaddr *)&local.address, &local.len) || !tcp_endpoint_addr(&remote, &peer) ||
            tcp_pool_launch(u, fd, &peer, &remote, &local, FIPS_LINK_INBOUND)) tcp_fd_close(fd);
    }
    if (!go_channel_is_closed(u->stop) && !go_context_is_canceled(u->root))
        fipsd_watcher_binding_exited(u->watcher, u->generation);
    go_channel_close(u->accept_done);
}
fipsd_bind_result tcp_bind(void *arg, fipsd_runtime *rt, GoChannel *stop,
    const fipsd_presence_facts *facts, uint64_t generation)
{
    (void)rt; (void)facts; tcp_instance *u = arg;
    if (go_channel_is_closed(stop) || go_channel_is_closed(u->stop) || go_context_is_canceled(u->root)) return FIPSD_BIND_ABSENT;
    listen_setup setup = {.fd = -1};
    if (u->config.bind_addr) {
        fips_transport_addr addr; tcp_endpoint *items = NULL; size_t n = 0;
        (void)fips_transport_addr_from_string(u->bind_addr, strlen(u->bind_addr), &addr);
        if (!tcp_resolve(&addr, true, &items, &n)) return FIPSD_BIND_RETRY;
        setup.endpoint = items[0]; free(items);
        go_blocking_submit(open_listener, &setup);
        if (setup.fd < 0) return FIPSD_BIND_RETRY;
        if (gof_io_reset_fd(setup.fd)) { tcp_fd_close(setup.fd); return FIPSD_BIND_RETRY; }
    }
    nsync_mu_lock(&u->mu);
    u->local = setup.endpoint; u->listener_fd = setup.fd; u->generation = generation; u->binding = true;
    nsync_mu_unlock(&u->mu);
    if (setup.fd >= 0 && fipsd_runtime_spawn(u->runtime, accept_loop, u)) {
        nsync_mu_lock(&u->mu); u->binding = false; u->listener_fd = -1; nsync_mu_unlock(&u->mu);
        tcp_fd_close(setup.fd); return FIPSD_BIND_RETRY;
    }
    (void)go_channel_try_send(u->bound, u); return FIPSD_BIND_OK;
}
void tcp_unbind(void *arg, uint64_t generation)
{
    tcp_instance *u = arg; (void)generation;
    nsync_mu_lock(&u->mu); u->binding = false; int fd = u->listener_fd; nsync_mu_unlock(&u->mu);
    if (fd >= 0) {
        (void)gof_io_cancel_fd(fd);
        void *v; (void)go_channel_receive(u->accept_done, &v);
        nsync_mu_lock(&u->mu); u->listener_fd = -1; nsync_mu_unlock(&u->mu);
        tcp_fd_close(fd);
        go_channel_unref(u->accept_done); u->accept_done = go_channel_create(1);
    }
    tcp_pool_stop(u);
}
