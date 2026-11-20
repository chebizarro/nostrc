#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#define __APPLE_USE_RFC_3542 1
#include "transport/udp/internal.h"
#include "transport/framing.h"
#include "libgo/fiber.h"
#include "test_tmpdir.h"
#include "select.h"
#include "blocking_executor.h"
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <net/if.h>
#include <pthread.h>
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <net/if_dl.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static pthread_mutex_t cancel_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cancel_cv = PTHREAD_COND_INITIALIZER;
static int watched_fd = -1;
static bool committed_park;
static bool intercept_armed, armed, release_armed;
void gof_trace_on_io_armed(int fd, int events)
{
    (void)events;
    pthread_mutex_lock(&cancel_mu);
    if (fd == watched_fd && intercept_armed) {
        armed = true; pthread_cond_signal(&cancel_cv);
        /* Test barrier only: no scheduler or I/O registry mutex is held. */
        while (!release_armed) pthread_cond_wait(&cancel_cv, &cancel_mu);
    }
    pthread_mutex_unlock(&cancel_mu);
}
void gof_trace_on_block(int fd, int events)
{
    (void)events;
    pthread_mutex_lock(&cancel_mu);
    if (fd == watched_fd) { committed_park = true; pthread_cond_signal(&cancel_cv); }
    pthread_mutex_unlock(&cancel_mu);
}
typedef struct cancel_test { int fd[2]; pthread_t thread; bool before_park; } cancel_test;
static void *cancel_parked(void *arg)
{
    cancel_test *q = arg;
    pthread_mutex_lock(&cancel_mu);
    while (q->before_park ? !armed : !committed_park) pthread_cond_wait(&cancel_cv, &cancel_mu);
    pthread_mutex_unlock(&cancel_mu);
    if (q->before_park) {
        assert(gof_io_reset_fd(q->fd[0]) == -1 && errno == EBUSY);
        uint8_t byte = 42;
        /* Native notifier thread, not a scheduler fiber. Data readiness and
         * repeated cancellation both target the registered waiter. */
        assert(write(q->fd[1], &byte, 1) == 1);
    }
    assert(gof_io_cancel_fd(q->fd[0]) == 0);
    assert(gof_io_cancel_fd(q->fd[0]) == 0);
    pthread_mutex_lock(&cancel_mu); release_armed = true; pthread_cond_signal(&cancel_cv); pthread_mutex_unlock(&cancel_mu);
    return NULL;
}
static void *start_cancel_test(void *arg)
{
    cancel_test *q = arg;
    assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, q->fd) == 0);
    pthread_mutex_lock(&cancel_mu);
    watched_fd = q->fd[0]; committed_park = false; intercept_armed = q->before_park; armed = release_armed = false;
    pthread_mutex_unlock(&cancel_mu);
    assert(pthread_create(&q->thread, NULL, cancel_parked, q) == 0); return NULL;
}
static void *finish_cancel_test(void *arg)
{
    cancel_test *q = arg; assert(pthread_join(q->thread, NULL) == 0);
    pthread_mutex_lock(&cancel_mu); watched_fd = -1; pthread_mutex_unlock(&cancel_mu);
    close(q->fd[0]); close(q->fd[1]); return NULL;
}
static void cancellation(void)
{
    cancel_test q = {0}; go_blocking_submit(start_cancel_test, &q);
    assert(gof_io_reset_fd(q.fd[0]) == 0);
    assert(gof_io_cancel_fd(q.fd[0]) == 0); /* latch before any registration */
    uint8_t byte;
    struct iovec v = {.iov_base = &byte, .iov_len = 1};
    struct msghdr m = {.msg_iov = &v, .msg_iovlen = 1};
    assert(gof_recvmsg(q.fd[0], &m, 0, -1) == -1 && errno == ECANCELED);
    assert(gof_io_reset_fd(q.fd[0]) == 0);
    /* The canceller waits for the committed park, not an elapsed delay. */
    assert(gof_recvmsg(q.fd[0], &m, 0, -1) == -1 && errno == ECANCELED);
    assert(gof_io_reset_fd(q.fd[0]) == 0);
    byte = 42; assert(gof_sendmsg(q.fd[1], &m, 0, -1) == 1);
    byte = 0; assert(gof_recvmsg(q.fd[0], &m, 0, -1) == 1 && byte == 42);
    go_blocking_submit(finish_cancel_test, &q);
    q.before_park = true; go_blocking_submit(start_cancel_test, &q);
    assert(gof_io_reset_fd(q.fd[0]) == 0);
    assert(gof_recvmsg(q.fd[0], &m, 0, -1) == -1 && errno == ECANCELED);
    assert(gof_io_reset_fd(q.fd[0]) == 0);
    byte = 0; assert(gof_recvmsg(q.fd[0], &m, 0, -1) == 1 && byte == 42);
    go_blocking_submit(finish_cancel_test, &q);
    puts("latched I/O cancellation: pre-registration, armed/pre-park + readiness/repeated cancel, committed park, EBUSY reset, join/reset/reuse: ok");
}

static void *receive(GoChannel *c)
{
    void *value = NULL;
    GoSelectCase wait = {GO_SELECT_RECEIVE, c, NULL, &value};
    GoSelectResult r = go_select_timeout(&wait, 1, 10000);
    assert(r.selected_case == 0);
    return value;
}
static size_t append(void *buffer, size_t at, int level, int kind, const void *data, size_t len)
{
    struct cmsghdr *h = (void *)((uint8_t *)buffer + at);
    h->cmsg_level = level; h->cmsg_type = kind; h->cmsg_len = CMSG_LEN(len);
    memcpy(CMSG_DATA(h), data, len); return at + CMSG_SPACE(len);
}
static void ancillary(void)
{
    union { struct cmsghdr align; uint8_t data[512]; } c = {0};
    struct msghdr m = {.msg_control = c.data};
    size_t n = 0;
    struct in_addr dst; assert(inet_pton(AF_INET, "127.0.0.2", &dst) == 1);
#if defined(__linux__)
    struct in_pktinfo pi = {.ipi_addr = dst, .ipi_ifindex = 23};
    n = append(c.data, n, IPPROTO_IP, IP_PKTINFO, &pi, sizeof pi);
#else
    struct sockaddr_dl dl = {.sdl_index = 23};
    n = append(c.data, n, IPPROTO_IP, IP_RECVDSTADDR, &dst, sizeof dst);
    n = append(c.data, n, IPPROTO_IP, IP_RECVIF, &dl, sizeof dl);
#endif
    uint8_t tos = 0xab; /* DSCP bits must not become ECN. */
    n = append(c.data, n, IPPROTO_IP, IP_TOS, &tos, sizeof tos);
    m.msg_controllen = (socklen_t)n;
    fipsd_udp_packet_info info;
    assert(fipsd_udp_decode_control(&m, &info));
    assert(info.family == AF_INET && info.has_destination && info.has_ecn && info.ecn == 3);
    assert(info.destination.v4.s_addr == dst.s_addr && info.interface_index == 23);
    union { struct cmsghdr align; uint8_t data[128]; } out;
    assert(fipsd_udp_source_control(out.data, sizeof out.data, &info) > 0);
#if defined(__linux__) || defined(__APPLE__)
    struct in_pktinfo selected;
    memcpy(&selected, CMSG_DATA((struct cmsghdr *)out.data), sizeof selected);
    assert(selected.ipi_spec_dst.s_addr == dst.s_addr && selected.ipi_ifindex == 23);
#endif
    assert(fipsd_udp_source_control(out.data, 1, &info) == 0);
    m.msg_flags = MSG_CTRUNC; assert(!fipsd_udp_decode_control(&m, &info)); m.msg_flags = 0;
    struct cmsghdr saved; memcpy(&saved, c.data, sizeof saved);
    struct cmsghdr *h = (void *)c.data;
    for (size_t cut = 0; cut < CMSG_LEN(0); ++cut) {
        h->cmsg_len = cut; assert(!fipsd_udp_decode_control(&m, &info));
    }
    h->cmsg_len = n + 1; assert(!fipsd_udp_decode_control(&m, &info));
    memcpy(c.data, &saved, sizeof saved);
    for (size_t cut = 1; cut < saved.cmsg_len; ++cut) {
        m.msg_controllen = (socklen_t)cut; assert(!fipsd_udp_decode_control(&m, &info));
    }
    memset(&c, 0, sizeof c);
    struct in6_pktinfo pi6 = {.ipi6_addr = IN6ADDR_LOOPBACK_INIT, .ipi6_ifindex = 7};
    n = append(c.data, 0, IPPROTO_IPV6, IPV6_PKTINFO, &pi6, sizeof pi6);
    int tc = 0xfe; n = append(c.data, n, IPPROTO_IPV6, IPV6_TCLASS, &tc, sizeof tc);
    m.msg_controllen = (socklen_t)n;
    assert(fipsd_udp_decode_control(&m, &info));
    assert(info.family == AF_INET6 && info.ecn == 2 && info.interface_index == 7);
    assert(IN6_IS_ADDR_LOOPBACK(&info.destination.v6));
    size_t encoded = fipsd_udp_source_control(out.data, sizeof out.data, &info);
    struct msghdr roundtrip = {.msg_control = out.data, .msg_controllen = (socklen_t)encoded};
    assert(fipsd_udp_decode_control(&roundtrip, &info) && info.interface_index == 7);
    puts("ancillary: v4 dst/ifindex, v6 pktinfo, ECN DSCP masking, CE, every truncated header offset: ok");
}
static fipsd_udp_class consume(void *arg, const uint8_t *p, size_t n)
{
    unsigned *calls = arg; ++*calls;
    return n >= 4 && !memcmp(p, "TEST", 4) ? FIPSD_UDP_DISCARD : FIPSD_UDP_FMP;
}
static void classifier(void)
{
    unsigned calls = 0; fipsd_udp_classifiers r = {0};
    assert(fipsd_udp_classifier_add(&r, "test", consume, &calls));
    assert(!fipsd_udp_classifier_add(&r, "test", consume, &calls));
    assert(fipsd_udp_classify(&r, (const uint8_t *)"TEST", 4) == FIPSD_UDP_DISCARD && calls == 1);
    assert(fipsd_udp_classify(&r, (const uint8_t *)"FMP", 3) == FIPSD_UDP_FMP);
    assert(fipsd_udp_classify(NULL, (const uint8_t *)"NPTC", 4) == FIPSD_UDP_DISCARD);
    assert(fipsd_udp_classify(NULL, (const uint8_t *)"NPTA", 4) == FIPSD_UDP_DISCARD);
    assert(fipsd_udp_classify(NULL, NULL, 0) == FIPSD_UDP_FMP);
}
static bool socket_options(void)
{
    fipsd_udp_endpoint bind_to, local;
    assert(fipsd_udp_endpoint_numeric("127.0.0.1:0", &bind_to));
    fipsd_buffer_granted recv_grant, send_grant;
    int fd = fipsd_udp_open(&bind_to, NULL, false, 65536, 65536, &recv_grant, &send_grant, &local);
    assert(fd >= 0 && recv_grant.actual > 0 && send_grant.actual > 0);
    assert(recv_grant.requested == 65536 && send_grant.requested == 65536);
    int duplicate = fipsd_udp_open(&local, NULL, false, 65536, 65536, &recv_grant, &send_grant, &bind_to);
    assert(duplicate == -1); /* listener must not silently join another daemon */
    close(fd);
#ifdef __APPLE__
    const char *name = "lo0";
#else
    const char *name = "lo";
#endif
    assert(fipsd_udp_endpoint_numeric("127.0.0.1:0", &bind_to));
    if (!if_nametoindex(name)) { puts("SKIP interface binding: no loopback interface"); return false; }
    fd = fipsd_udp_open(&bind_to, name, false, 65536, 65536, &recv_grant, &send_grant, &local);
    if (fd < 0 && (errno == EPERM || errno == EACCES)) { puts("SKIP interface binding: not permitted"); return false; }
    assert(fd >= 0);
    close(fd); printf("socket buffers and %s interface binding: ok\n", name); return true;
}
typedef struct dynamic_source { _Atomic bool exists; _Atomic uint64_t now; const char *interface_name; } dynamic_source;
static fipsd_presence_facts dynamic_read(void *arg)
{
    dynamic_source *d = arg;
    if (!atomic_load(&d->exists)) return (fipsd_presence_facts){.valid = true};
    return fipsd_presence_read(d->interface_name);
}
static uint64_t dynamic_now(void *arg) { return atomic_load(&((dynamic_source *)arg)->now); }
typedef struct fixture {
    fipsd_transport *a, *b, *absent;
    fipsd_runtime *runtime;
    GoChannel *events, *done;
    fips_transport_addr aa, ba;
    unsigned calls;
    dynamic_source source;
    GoChannel *ticks;
    bool can_bind;
} fixture;
static void *mark_ce(void *arg)
{
    udp_socket *s = arg; int tos = 3;
    assert(setsockopt(s->fd, IPPROTO_IP, IP_TOS, &tos, sizeof tos) == 0);
    return NULL;
}
static int start(fipsd_subsystem *sub, fipsd_runtime *rt)
{
    fixture *f = sub->user; f->runtime = rt;
    if (f->a->ops->start_async(f->a, rt, NULL, f->events) || f->b->ops->start_async(f->b, rt, NULL, f->events)) return -1;
    return f->absent->ops->start_async(f->absent, rt, NULL, f->events);
}
static void stop(fipsd_subsystem *sub, fipsd_runtime *rt)
{
    (void)rt; fixture *f = sub->user;
    f->a->ops->stop(f->a); f->b->ops->stop(f->b); f->absent->ops->stop(f->absent);
}
static void drain_presence(fixture *f)
{
    for (unsigned i = 0; i < 2; ++i) {
        fipsd_transport_event *e = receive(f->events); assert(e && e->kind == FIPSD_TRANSPORT_BECAME_PRESENT); free(e);
    }
}
static void expect_packet(fixture *f, uint32_t id, size_t n)
{
    fipsd_transport_event *e = receive(f->events);
    assert(e && e->kind == FIPSD_TRANSPORT_PACKET && e->id.value == id && e->size == n); free(e);
}
#if defined(__linux__)
/* nostrc-u19cz.4: one recvmmsg takes every queued datagram, each with its own
 * source, pktinfo destination and ECN; one longer than the slot spills into
 * its tail. Called on a fiber; everything is queued first (loopback), so the
 * call returns without parking. */
static void recv_batch(void)
{
    fipsd_udp_endpoint bind_to, local; fipsd_buffer_granted rg, sg;
    assert(fipsd_udp_endpoint_numeric("0.0.0.0:0", &bind_to));
    gof_socket_t fd = fipsd_udp_open(&bind_to, NULL, false, 1 << 20, 1 << 20, &rg, &sg, &local);
    assert(fd >= 0);
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = ((struct sockaddr_in *)&local.address)->sin_port};
    assert(inet_pton(AF_INET, "127.0.0.1", &to.sin_addr) == 1);
    int senders[2]; struct sockaddr_in from[2];
    for (int i = 0; i < 2; ++i) {
        senders[i] = socket(AF_INET, SOCK_DGRAM, 0); assert(senders[i] >= 0);
        from[i] = (struct sockaddr_in){.sin_family = AF_INET}; from[i].sin_addr = to.sin_addr;
        assert(bind(senders[i], (struct sockaddr *)&from[i], sizeof from[i]) == 0);
        socklen_t len = sizeof from[i]; assert(getsockname(senders[i], (struct sockaddr *)&from[i], &len) == 0);
    }
    int ce = 3; assert(setsockopt(senders[1], IPPROTO_IP, IP_TOS, &ce, sizeof ce) == 0);
    static uint8_t data[3000];
    for (size_t i = 0; i < sizeof data; ++i) data[i] = (uint8_t)(i * 7);
    enum { SMALL = 7 };
    for (int i = 0; i < SMALL; ++i)
        assert(sendto(senders[i & 1], data + i, 10 + (size_t)i, 0, (struct sockaddr *)&to, sizeof to) == 10 + i);
    assert(sendto(senders[0], data, sizeof data, 0, (struct sockaddr *)&to, sizeof to) == (ssize_t)sizeof data);
    udp_rx *rx = fipsd_udp_rx_create(1357); assert(rx);
    assert(fipsd_udp_recv_batch(rx, fd) == SMALL + 1);
    for (int i = 0; i <= SMALL; ++i) {
        const struct sockaddr_in *source = (const struct sockaddr_in *)&rx->source[i].address;
        int who = i == SMALL ? 0 : i & 1;
        assert(rx->m[i].msg_hdr.msg_namelen == sizeof *source && source->sin_port == from[who].sin_port);
        fipsd_udp_packet_info info;
        assert(fipsd_udp_decode_control(&rx->m[i].msg_hdr, &info) && info.has_destination && info.has_ecn);
        assert(info.family == AF_INET && info.destination.v4.s_addr == to.sin_addr.s_addr);
        assert(info.ecn == (who ? 3 : 0));
        if (i < SMALL) assert(rx->m[i].msg_len == 10u + (unsigned)i && !memcmp(rx->slot[i]->bytes, data + i, 10 + (size_t)i));
    }
    assert(rx->m[SMALL].msg_len == sizeof data && !(rx->m[SMALL].msg_hdr.msg_flags & MSG_TRUNC));
    assert(!memcmp(rx->slot[SMALL]->bytes, data, 1357) && !memcmp(rx->iov[SMALL][1].iov_base, data + 1357, sizeof data - 1357));
    /* The next call re-arms what this one filled; nothing is queued now. */
    assert(gof_recvmmsg(fd, rx->m, FIPSD_UDP_RECV_BATCH, 0, 0) == -1 && fipsd_io_errno() == ETIMEDOUT);
    fipsd_udp_rx_free(rx);
    close(senders[0]); close(senders[1]); close(fd);
    puts("recvmmsg: one call per queued burst, per-datagram source/pktinfo/ECN, oversize spill: ok");
}
/* Batched sends (udp.c send_batch) through the syscall seam. */
enum { HOOK_PASS, HOOK_PARTIAL, HOOK_FAIL_FIRST, HOOK_EAGAIN_GSO, HOOK_REFUSE_GSO };
static int hook_mode; static unsigned hook_calls, hook_gso, hook_once;
static int hooked_sendmmsg(gof_socket_t fd, struct mmsghdr *m, unsigned n, int flags)
{
    ++hook_calls;
    for (unsigned i = 0; i < n; ++i) hook_gso += m[i].msg_hdr.msg_iovlen > 1;
    bool gso = m[0].msg_hdr.msg_iovlen > 1;
    if (hook_once && hook_mode == HOOK_PARTIAL && n > 1) { --hook_once; return gof_sendmmsg(fd, m, 1, flags, 0); }
    if (hook_once && hook_mode == HOOK_FAIL_FIRST) { --hook_once; errno = ENOBUFS; return -1; }
    if (hook_once && hook_mode == HOOK_EAGAIN_GSO && gso) { --hook_once; errno = EAGAIN; return -1; }
    if (hook_mode == HOOK_REFUSE_GSO && gso) { errno = EIO; return -1; }
    return gof_sendmmsg(fd, m, n, flags, 0);
}
/* An established-phase frame of n bytes, marked in its counter field. */
static void established(uint8_t *p, size_t n, uint8_t mark)
{
    memset(p, 0, n); p[2] = (uint8_t)(n - FIPS_FRAMING_FMP_ENCRYPTED_MIN_SIZE);
    p[3] = (uint8_t)((n - FIPS_FRAMING_FMP_ENCRYPTED_MIN_SIZE) >> 8); p[8] = mark;
}
static unsigned send_marked(fipsd_transport *t, const fips_transport_addr *const *to, const size_t *sizes,
    size_t n, int mode, unsigned once, int *rc)
{
    static uint8_t bytes[16][1400]; fipsd_transport_frame frames[16];
    assert(n <= 16);
    for (size_t i = 0; i < n; ++i) {
        established(bytes[i], sizes[i], (uint8_t)i);
        frames[i] = (fipsd_transport_frame){.to = to[i], .bytes = bytes[i], .len = sizes[i]};
    }
    hook_mode = mode; hook_once = once; hook_calls = hook_gso = 0;
    size_t sent = t->ops->send_batch(t, frames, n, rc);
    hook_mode = HOOK_PASS;
    size_t ok = 0; for (size_t i = 0; i < n; ++i) ok += rc[i] == 0;
    assert(sent == ok);
    return hook_calls;
}
static void expect_marked(fixture *f, uint32_t id, size_t n, uint8_t mark)
{
    fipsd_transport_event *e = receive(f->events);
    assert(e && e->kind == FIPSD_TRANSPORT_PACKET && e->id.value == id && e->size == n && e->bytes[8] == mark); free(e);
}
static udp_socket *sender_socket(fipsd_transport *t, const fips_transport_addr *to)
{
    udp_instance *u = t->user; fips_transport_addr numeric; fipsd_udp_endpoint e;
    assert(fipsd_udp_resolve(&u->dns, to, AF_INET, &e) && fipsd_udp_endpoint_addr(&e, &numeric));
    nsync_mu_lock(&u->mu);
    udp_socket *s = u->listener;
    for (size_t i = 0; i < u->config.peer_capacity; ++i)
        if (u->peers[i] && fips_transport_addr_equal(&u->peers[i]->peer, &numeric)) s = u->peers[i];
    nsync_mu_unlock(&u->mu);
    return s;
}
/* From t to its peer at `to` (whose events carry id). */
static void batched(fixture *f, fipsd_transport *t, const fips_transport_addr *to, uint32_t id)
{
    udp_socket *s = sender_socket(t, to);
    bool gso = atomic_load(&s->gso);
    const fips_transport_addr *dest[16]; for (size_t i = 0; i < 16; ++i) dest[i] = to;
    int rc[16];
    fipsd_transport_stats_view before = fipsd_transport_stats_read(&t->shared->stats);
    /* An equal-size run with a shorter tail: one message, one call. */
    size_t run[11]; for (size_t i = 0; i < 11; ++i) run[i] = i < 10 ? 200 : 120;
    assert(send_marked(t, dest, run, 11, HOOK_PASS, 0, rc) == 1 && hook_gso == (gso ? 1u : 0u));
    for (uint8_t i = 0; i < 11; ++i) { assert(!rc[i]); expect_marked(f, id, run[i], i); }
    /* Runs end at a longer frame and after a shorter one. */
    size_t mixed[7] = {200, 200, 120, 200, 200, 64, 64};
    assert(send_marked(t, dest, mixed, 7, HOOK_PASS, 0, rc) == 1 && hook_gso == (gso ? 2u : 0u));
    for (uint8_t i = 0; i < 7; ++i) expect_marked(f, id, mixed[i], i);
    /* The kernel takes only the first message: the rest follow, in order. */
    size_t rising[4] = {100, 150, 200, 250};
    assert(send_marked(t, dest, rising, 4, HOOK_PARTIAL, 1, rc) == 2);
    for (uint8_t i = 0; i < 4; ++i) { assert(!rc[i]); expect_marked(f, id, rising[i], i); }
    /* The first message fails: only its frame does, and counts once. */
    assert(send_marked(t, dest, rising, 3, HOOK_FAIL_FIRST, 1, rc) == 2 && rc[0] == -1 && !rc[1] && !rc[2]);
    for (uint8_t i = 1; i < 3; ++i) expect_marked(f, id, rising[i], i);
    /* An oversize frame fails its own MTU check; the frames around it go. */
    size_t over[3] = {100, 1358, 100};
    assert(send_marked(t, dest, over, 3, HOOK_PASS, 0, rc) == 1 && !rc[0] && rc[1] == -1 && !rc[2]);
    expect_marked(f, id, 100, 0); expect_marked(f, id, 100, 2);
    fipsd_transport_stats_view after = fipsd_transport_stats_read(&t->shared->stats);
    assert(after.packets_sent - before.packets_sent == 11 + 7 + 4 + 2 + 2);
    assert(after.send_errors - before.send_errors == 1 && after.mtu_exceeded - before.mtu_exceeded == 1);
    size_t equal[4] = {200, 200, 200, 200};
    if (gso) {
        /* Backpressure on a GSO message: resent as plain messages; GSO stays. */
        assert(send_marked(t, dest, equal, 4, HOOK_EAGAIN_GSO, 1, rc) == 2 && atomic_load(&s->gso));
        for (uint8_t i = 0; i < 4; ++i) { assert(!rc[i]); expect_marked(f, id, 200, i); }
        /* Refused: resent plain, and the socket stops using GSO. */
        assert(send_marked(t, dest, equal, 4, HOOK_REFUSE_GSO, 0, rc) == 2 && !atomic_load(&s->gso));
        for (uint8_t i = 0; i < 4; ++i) { assert(!rc[i]); expect_marked(f, id, 200, i); }
    }
    assert(send_marked(t, dest, equal, 4, HOOK_PASS, 0, rc) == 1 && hook_gso == 0);
    for (uint8_t i = 0; i < 4; ++i) expect_marked(f, id, 200, i);
    atomic_store(&s->gso, gso);
    printf("batched send (%s, GSO %s): runs, partial sendmmsg, failed message, MTU, GSO backpressure/refusal fallback: ok\n",
        s->connected ? "connected" : "listener", gso ? "probed" : "unavailable");
}
#endif
static void scenario(void *arg)
{
    fixture *f = arg;
    cancellation();
    uint8_t frame[114] = {1, 0, 110, 0};
    assert(f->a->ops->send(f->a, &f->ba, frame, sizeof frame) == 0);
    expect_packet(f, 2, sizeof frame);
    assert(f->b->ops->send(f->b, &f->aa, frame, sizeof frame) == 0);
    fipsd_transport_event *reply = receive(f->events);
    assert(reply && reply->kind == FIPSD_TRANSPORT_PACKET && reply->id.value == 1);
    assert(fips_transport_addr_equal(&reply->address, &f->ba)); free(reply);
    fipsd_transport_stats_view av = fipsd_transport_stats_read(&f->a->shared->stats);
    fipsd_transport_stats_view bv = fipsd_transport_stats_read(&f->b->shared->stats);
    assert(av.packets_sent == 1 && av.bytes_sent == sizeof frame && av.packets_recv == 1 && av.bytes_recv == sizeof frame);
    assert(bv.packets_sent == 1 && bv.bytes_sent == sizeof frame && bv.packets_recv == 1 && bv.bytes_recv == sizeof frame);
    assert(!av.send_errors && !av.recv_errors && !av.mtu_exceeded && !av.kernel_drops);
    uint8_t oversized[1358] = {0};
    assert(f->a->ops->send(f->a, &f->ba, oversized, sizeof oversized) == -1);
    fips_transport_addr bad; assert(fips_transport_addr_from_string("255.255.255.255:9", 17, &bad) == FIPS_OK);
    assert(f->a->ops->send(f->a, &bad, frame, sizeof frame) == -1); /* SO_BROADCAST is intentionally disabled: real sendmsg failure */
    av = fipsd_transport_stats_read(&f->a->shared->stats);
    assert(av.mtu_exceeded == 1 && av.send_errors == 1 && av.packets_sent == 1 && av.bytes_sent == sizeof frame);
    puts("wildcard listener: pktinfo-selected loopback reply source, send/recv/MTU/error accounting: ok");
    assert(fipsd_udp_connected_count(f->a) == 0 && fipsd_udp_connected_count(f->b) == 0);
    /* Merely parsing a valid envelope must never allocate a connected fd. */
    f->a->ops->authenticated(f->a, &f->ba, FIPS_LINK_OUTBOUND);
    f->b->ops->authenticated(f->b, &f->aa, FIPS_LINK_INBOUND);
    f->a->ops->authenticated(f->a, &f->ba, FIPS_LINK_OUTBOUND);
    assert(fipsd_udp_connected_count(f->a) == 1 && fipsd_udp_connected_count(f->b) == 1);
    udp_instance *a = f->a->user;
    udp_socket *connected = NULL;
    nsync_mu_lock(&a->mu);
    for (size_t i = 0; i < a->config.peer_capacity; ++i) if (a->peers[i]) connected = a->peers[i];
    nsync_mu_unlock(&a->mu); assert(connected);
    go_blocking_submit(mark_ce, connected);
    struct iovec empty_iov = {.iov_base = frame, .iov_len = 0};
    struct msghdr empty = {.msg_iov = &empty_iov, .msg_iovlen = 1};
    assert(gof_sendmsg(connected->fd, &empty, 0, 0) == 0);
    fips_transport_addr other;
    assert(fips_transport_addr_from_string("127.0.0.1:1", 11, &other) == FIPS_OK);
    f->a->ops->authenticated(f->a, &other, FIPS_LINK_OUTBOUND);
    assert(fipsd_udp_connected_count(f->a) == 1 && atomic_load(&f->a->shared->diagnostics.pool_full) == 1);
    for (size_t i = 1; i < sizeof frame; ++i) assert(f->a->ops->send(f->a, &f->ba, frame, i) == 0);
    assert(f->a->ops->send(f->a, &f->ba, (const uint8_t *)"TEST", 4) == 0);
    assert(f->a->ops->send(f->a, &f->ba, (const uint8_t *)"NPTC", 4) == 0);
    assert(f->a->ops->send(f->a, &f->ba, frame, sizeof frame) == 0);
    expect_packet(f, 2, sizeof frame); /* ordered receive barrier, never a sleep */
    bv = fipsd_transport_stats_read(&f->b->shared->stats);
    assert(bv.packets_recv == 118 && bv.bytes_recv == 6677); /* 2 full + zero + 1..113 + two classifiers */
    assert(!bv.recv_errors && !bv.kernel_drops);
    assert(atomic_load(&f->b->shared->diagnostics.malformed) == sizeof frame);
    assert(atomic_load(&f->b->shared->diagnostics.classifier_drops) == 2);
    assert(atomic_load(&f->b->shared->diagnostics.pktinfo_recv) >= sizeof frame);
    assert(atomic_load(&f->b->shared->diagnostics.ecn_recv) >= sizeof frame);
    assert(atomic_load(&f->b->shared->diagnostics.ce_recv) >= sizeof frame);
    /* Close must join a drain parked on gof_recvmsg before freeing the fd.
     * A replacement then receives through its new identity, not the old one. */
    f->a->ops->close_connection(f->a, &f->ba); f->b->ops->close_connection(f->b, &f->aa);
    assert(fipsd_udp_connected_count(f->a) == 0 && fipsd_udp_connected_count(f->b) == 0);
    f->a->ops->authenticated(f->a, &f->ba, FIPS_LINK_OUTBOUND);
    f->b->ops->authenticated(f->b, &f->aa, FIPS_LINK_INBOUND);
    assert(f->a->ops->send(f->a, &f->ba, frame, sizeof frame) == 0); expect_packet(f, 2, sizeof frame);
    assert(!fipsd_udp_register_classifier(f->a, "late", consume, &f->calls));
#if defined(__linux__)
    recv_batch();
    int (*plain_sendmmsg)(gof_socket_t, struct mmsghdr *, unsigned, int) = fipsd_udp_sendmmsg;
    fipsd_udp_sendmmsg = hooked_sendmmsg;
    batched(f, f->a, &f->ba, 2); /* a's connected socket */
    /* Two destinations in one batch: a run each, each in order. */
    const fips_transport_addr *two[4] = {&f->ba, &f->ba, &f->aa, &f->ba};
    size_t sizes[4] = {100, 100, 100, 100}; int rc[4];
    assert(send_marked(f->a, two, sizes, 4, HOOK_PASS, 0, rc) == 3);
    uint8_t next_b = 0; bool got_a = false;
    for (int i = 0; i < 4; ++i) {
        fipsd_transport_event *e = receive(f->events); assert(e && e->kind == FIPSD_TRANSPORT_PACKET);
        if (e->id.value == 1) { assert(!got_a && e->bytes[8] == 2); got_a = true; }
        else { assert(e->id.value == 2 && e->bytes[8] == next_b); next_b = next_b == 1 ? 3 : next_b + 1; }
        free(e);
    }
    assert(got_a && next_b == 4);
#endif
    if (f->can_bind) {
        for (unsigned generation = 1; generation <= 2; ++generation) {
            atomic_store(&f->source.exists, true); atomic_store(&f->source.now, generation * 20000);
            assert(go_channel_send(f->ticks, f) == 0);
            assert(receive(fipsd_udp_bound(f->absent)));
            fipsd_transport_event *e = receive(f->events);
            assert(e && e->id.value == 3 && e->kind == FIPSD_TRANSPORT_BECAME_PRESENT); free(e);
            fips_transport_addr endpoint; assert(fipsd_udp_address(f->absent, &endpoint));
            assert(f->a->ops->send(f->a, &endpoint, frame, sizeof frame) == 0); expect_packet(f, 3, sizeof frame);
            atomic_store(&f->source.exists, false); atomic_store(&f->source.now, generation * 20000 + 10000);
            assert(go_channel_send(f->ticks, f) == 0);
            e = receive(f->events);
            assert(e && e->id.value == 3 && e->kind == FIPSD_TRANSPORT_BECAME_ABSENT); free(e);
            assert(!fipsd_udp_address(f->absent, &endpoint));
        }
        fipsd_presence_view v = fipsd_presence_snapshot(&f->absent->shared->presence);
        assert(v.binds == 2);
        puts("interface watcher: absent start -> appears/binds/receives -> disappears/joins -> reappears/rebinds/receives: ok");
    }
#if defined(__linux__)
    /* b's wildcard listener: pktinfo source control beside UDP_SEGMENT. */
    f->b->ops->close_connection(f->b, &f->aa);
    batched(f, f->b, &f->aa, 1);
    fipsd_udp_sendmmsg = plain_sendmmsg;
#endif
    nsync_mu_lock(&a->mu);
    assert(a->listener); int receive_fd = a->listener->fd;
    GoChannel *receive_done = go_channel_ref(a->listener->done);
    nsync_mu_unlock(&a->mu);
    assert(gof_io_cancel_fd(receive_fd) == 0); (void)receive(receive_done); go_channel_unref(receive_done);
    assert(atomic_load(&f->a->shared->stats.recv_errors) == 1);
    puts("real UDP: unauthenticated pool empty; authenticated connect, idempotence, cap, all malformed offsets counted, classifiers on connected drain, close/join/reopen: ok");
    assert(go_channel_send(f->done, f) == 0);
}
static void lifecycle(bool can_bind)
{
    fipsd_runtime_config rc; fipsd_runtime_config_default(&rc); rc.workers = 2; rc.handle_signals = false;
    fipsd_runtime *rt = fipsd_runtime_create(&rc); assert(rt);
    fipsd_udp_config c; fipsd_udp_config_default(&c); c.bind_addr = "127.0.0.1:0"; c.peer_capacity = 1;
    fixture f = {.events = go_channel_create(256), .done = go_channel_create(1), .ticks = go_channel_create(1), .can_bind = can_bind};
    f.a = fipsd_udp_create((fips_transport_id){1}, "a", &c);
    c.bind_addr = "0.0.0.0:0"; f.b = fipsd_udp_create((fips_transport_id){2}, "b", &c);
    c.bind_addr = "127.0.0.1:0";
#ifdef __APPLE__
    f.source.interface_name = "lo0";
#else
    f.source.interface_name = "lo";
#endif
    atomic_init(&f.source.exists, false); atomic_init(&f.source.now, fipsd_transport_now_ms());
    fipsd_watcher_source source = {.read = dynamic_read, .user = &f.source, .ticks = f.ticks, .now_ms = dynamic_now};
    c.interface_name = f.source.interface_name; c.optional = true; c.watcher_source = &source;
    f.absent = fipsd_udp_create((fips_transport_id){3}, "absent", &c);
    assert(f.a && f.b && f.absent);
    assert(fipsd_udp_register_classifier(f.b, "test", consume, &f.calls));
    fipsd_subsystem sub = {.name = "udp-test", .start = start, .stop = stop, .user = &f};
    assert(fipsd_runtime_register(rt, &sub) == 0 && fipsd_runtime_start(rt) == 0);
    assert(receive(fipsd_udp_bound(f.a)) && receive(fipsd_udp_bound(f.b)));
    assert(fipsd_udp_address(f.a, &f.aa) && fipsd_udp_address(f.b, &f.ba));
    fipsd_udp_endpoint wildcard;
    assert(fipsd_udp_resolve(&((udp_instance *)f.b->user)->dns, &f.ba, AF_INET, &wildcard));
    /* Only 127.0.0.1 is guaranteed assigned on Darwin; another 127/8 address
     * needs an alias there. Distinct source selection is also checked above
     * against crafted pktinfo without mutating host interfaces. */
    assert(inet_pton(AF_INET, "127.0.0.1", &((struct sockaddr_in *)&wildcard.address)->sin_addr) == 1);
    assert(fipsd_udp_endpoint_addr(&wildcard, &f.ba));
    assert(f.absent->ops->state(f.absent) == FIPSD_TRANSPORT_ABSENT);
    drain_presence(&f);
    assert(fipsd_runtime_spawn(rt, scenario, &f) == 0); assert(receive(f.done));
    fipsd_runtime_shutdown(rt); assert(fipsd_runtime_run(rt) == FIPSD_RUN_OK);
    assert(f.a->ops->state(f.a) == FIPSD_TRANSPORT_STOPPED);
    fipsd_runtime_destroy(rt); fipsd_udp_destroy(f.a); fipsd_udp_destroy(f.b); fipsd_udp_destroy(f.absent);
    void *value; while (go_channel_try_receive(f.events, &value) == 0) free(value);
    go_channel_unref(f.events); go_channel_unref(f.done); go_channel_unref(f.ticks);
}
int main(void)
{
    (void)fipsd_test_tmpdir(NULL);
    fipsd_transport_stats stats; fipsd_transport_stats_init(&stats);
    fipsd_transport_stats_kernel_drops(&stats, false, 7);
    fipsd_transport_stats_kernel_drops(&stats, true, 100);
    assert(atomic_load(&stats.kernel_drops) == 7);
    fipsd_transport_stats_kernel_drops(&stats, false, 9);
    assert(atomic_load(&stats.kernel_drops) == 9);
    ancillary(); classifier(); bool can_bind = socket_options(); lifecycle(can_bind); return 0;
}
