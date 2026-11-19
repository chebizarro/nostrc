/* nostrc-3vl7v: FipsdTcpTest's lane case for a proxied stream transport (Tor,
 * Nym), whose send_lane reaches its inner TCP owner's two lanes. The SOCKS5
 * fixture answers the CONNECT and holds the client socket unread (S5_HOLD),
 * so a few large data frames fill that one hop's socket buffers and the
 * writer parks inside one of them; libgo's park trace, not a delay, says it
 * is there. Then both lanes take frames up to their exact depths, and the far
 * end reads: the frames the writer had taken (the last one stalled), every
 * control frame, then the rest of the data lane in order. Include once per
 * test executable: it defines libgo's weak gof_trace_on_block hook. */
#ifndef FIPSD_TESTS_STREAM_LANES_H
#define FIPSD_TESTS_STREAM_LANES_H
#include "transport/tcp/internal.h"
#include "blocking_executor.h"
#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t lanes_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t lanes_cv = PTHREAD_COND_INITIALIZER;
static int lanes_fd = -1;
static bool lanes_parked;
void gof_trace_on_block(int fd, int events)
{
    pthread_mutex_lock(&lanes_mu);
    if (fd == lanes_fd && (events & 2)) { lanes_parked = true; pthread_cond_signal(&lanes_cv); }
    pthread_mutex_unlock(&lanes_mu);
}
static void *lanes_wait_parked(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&lanes_mu);
    while (!lanes_parked) pthread_cond_wait(&lanes_cv, &lanes_mu);
    lanes_fd = -1;
    pthread_mutex_unlock(&lanes_mu);
    return NULL;
}
/* Tags in the last byte: stalled/large data 3, control 2, small data 1.
 * Sixteen large frames (960 KB) are well past what one loopback hop buffers
 * for a reader that never reads (about 175 KiB on macOS, whose accepted
 * sockets ignore a smaller SO_RCVBUF), and well inside the data lane. The
 * transport's mtu must admit LANES_BIG. */
enum { LANES_BIG = 60000, LANES_SMALL = 114, LANES_BIGS = 16 };
static void lanes_frame(uint8_t *p, size_t n, uint8_t tag)
{
    memset(p, 0, n);
    size_t payload = n - FIPS_FRAMING_FMP_ENCRYPTED_MIN_SIZE;
    p[2] = (uint8_t)payload; p[3] = (uint8_t)(payload >> 8); p[n - 1] = tag;
}
static size_t lanes_fill(fipsd_transport *t, const fips_transport_addr *to, fipsd_transport_lane lane, uint8_t tag)
{
    uint8_t bytes[LANES_SMALL]; lanes_frame(bytes, sizeof bytes, tag);
    size_t n = 0;
    while (n < 1024 && fipsd_transport_send_lane(t, lane, to, bytes, sizeof bytes) == 0) ++n;
    return n;
}
static void lanes_read(int fd, uint8_t *buf, size_t want, uint8_t tag)
{
    size_t n = 0;
    assert(fipsd_stream_read(fd, buf, UINT16_MAX, &n) == 1 && n == want && buf[n - 1] == tag);
}
/* On a fiber. `t` is connected to `to` and `held` is the proxy's end of that
 * connection, taken off socks5_fixture.held; this closes it. */
static void stream_lanes(fipsd_transport *t, fipsd_transport *inner, const fips_transport_addr *to, int held)
{
    tcp_instance *u = inner->user;
    fipsd_transport_diagnostics *d = &u->shared.diagnostics;
    const size_t depth = u->config.send_queue_depth;
    uint64_t data0 = atomic_load(&d->data_dropped), control0 = atomic_load(&d->control_dropped);
    uint64_t sent0 = atomic_load(&d->control_sent);
    uint8_t *big = malloc(LANES_BIG), *buf = malloc(UINT16_MAX);
    assert(big && buf); lanes_frame(big, LANES_BIG, 3);
    nsync_mu_lock(&u->mu); tcp_conn *conn = tcp_pool_find(u, to); assert(conn); int fd = conn->fd; nsync_mu_unlock(&u->mu);
    pthread_mutex_lock(&lanes_mu); lanes_fd = fd; lanes_parked = false; pthread_mutex_unlock(&lanes_mu);
    for (size_t i = 0; i < LANES_BIGS; ++i)
        assert(fipsd_transport_send_lane(t, FIPSD_TRANSPORT_LANE_DATA, to, big, LANES_BIG) == 0);
    go_blocking_submit(lanes_wait_parked, NULL);
    /* The writer is parked: the data lane holds the large frames it has not
     * taken, the control lane is empty. Each lane refuses once full. */
    size_t data = lanes_fill(t, to, FIPSD_TRANSPORT_LANE_DATA, 1);
    size_t control = lanes_fill(t, to, FIPSD_TRANSPORT_LANE_CONTROL, 2);
    assert(control == FIPSD_TCP_CONTROL_DEPTH);
    assert(atomic_load(&d->data_dropped) == data0 + 1 && atomic_load(&d->control_dropped) == control0 + 1);
    int flags = fcntl(held, F_GETFL);
    assert(flags >= 0 && fcntl(held, F_SETFL, flags | O_NONBLOCK) == 0 && gof_io_reset_fd(held) == 0);
    size_t n = 0, taken = 0;
    for (;;) {
        assert(fipsd_stream_read(held, buf, UINT16_MAX, &n) == 1);
        if (n == LANES_SMALL) break;
        assert(n == LANES_BIG && buf[n - 1] == 3); ++taken;
    }
    assert(taken >= 1 && taken <= LANES_BIGS && buf[n - 1] == 2); /* the first control frame */
    for (size_t i = 1; i < control; ++i) lanes_read(held, buf, LANES_SMALL, 2);
    for (size_t i = taken; i < LANES_BIGS; ++i) lanes_read(held, buf, LANES_BIG, 3);
    for (size_t i = 0; i < data; ++i) lanes_read(held, buf, LANES_SMALL, 1);
    assert(LANES_BIGS - taken + data == depth); /* the data lane was exactly full */
    assert(atomic_load(&d->control_sent) == sent0 + control);
    t->ops->close_connection(t, to);
    assert(fipsd_stream_read(held, buf, UINT16_MAX, &n) == 0);
    tcp_fd_close(held);
    free(big); free(buf);
}
#endif
