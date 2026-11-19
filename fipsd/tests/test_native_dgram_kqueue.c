/* Characterisation of kqueue EVFILT_WRITE on AF_UNIX SOCK_DGRAM (nostrc-e3ps5).
 *
 * A macOS datagram flow refuses a full client with ENOBUFS. fipsd parks its
 * flow writer instead of dropping only if the kernel reliably raises write
 * readiness on the daemon's half once the client reads. XNU does: unix
 * SOCK_DGRAM carries PR_WANTRCVD, so every soreceive runs uipc_rcvd, which
 * wakes the *connected peer's* so_snd knotes; and filt_sowrite_common reports
 * data = MIN(sbspace(so_snd), sbspace(peer so_rcv)) for a connected PF_LOCAL
 * socket, honouring NOTE_LOWAT. A datagram of L bytes from an unbound sender
 * needs L + sizeof(struct sockaddr) receiver bytes (sbconcat_mbufs counts the
 * 16-byte sun_noname), so a level registration with NOTE_LOWAT = L + 16 fires
 * exactly when that datagram fits.
 *
 * This probe measures that on the running kernel, without libgo or fipsd:
 *   1. snapshots: what kevent reports on a full receiver and after one recv,
 *      level vs EV_CLEAR, with and without NOTE_LOWAT, per socket topology
 *      (socketpair; sender connected to a bound receiver that is not
 *      connected back; both bound and connected to each other);
 *   2. the per-datagram space cost on an empty receiver;
 *   3. reliability: thousands of ENOBUFS -> park -> wake -> resend cycles
 *      against a slow reader, every datagram sequence-checked;
 *   4. edge cases the daemon depends on: a drain before arming fires at
 *      attach; a need above the receiver's capacity never fires (the
 *      daemon's overflow fallback); NOTE_LOWAT is clamped to the sender's
 *      SO_SNDBUF; a peer close while parked raises nothing.
 *
 * Assertions cover only what fipsd's Darwin datagram path relies on, on the
 * topology it uses (socketpair): NOTE_LOWAT level wakes never time out, never
 * wake early, lose nothing; a drain before arming fires at attach. A peer
 * close does not wake a parked writer (printed, not asserted): the daemon
 * cancels its writer on teardown instead.
 * Everything else is printed as data. The waits' 5 s bound is a failure
 * detector for a missing wake, not a pacing interval. Exits 77 elsewhere:
 * FreeBSD's unix SOCK_DGRAM uses the generic filt_sowrite over the sender's
 * own so_snd, which a connected send never fills, and its
 * uipc_soreceive_dgram wakes no sender, so there is no wake to characterise. */
#define _DARWIN_C_SOURCE 1
#include <stdio.h>
#ifndef __APPLE__
int main(void)
{
    puts("skip: kqueue AF_UNIX SOCK_DGRAM write-wake probe is macOS-only");
    return 77;
}
#else
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s (errno %d %s)\n", __FILE__, __LINE__, #x, errno, strerror(errno)); abort(); } } while (0)
#define BUDGET 65536      /* fipsd's per-flow budget: pending_per_flow 16 x 4096 */
#define OVERHEAD ((int)sizeof(struct sockaddr))
#define WAIT_MS 5000      /* failure detector for a wake that never comes */
#define MAX_LEN 1400
#define CYCLES 20000u     /* datagrams per reliability run */

typedef enum topology { PAIR, BOUND_ONEWAY, BOUND_BOTH } topology;
static const char *topology_name[] = {"socketpair", "bound-oneway", "bound-both"};
static char dir[64];
static uint8_t scratch[MAX_LEN];

static void unlink_paths(void)
{
    char p[96];
    snprintf(p, sizeof p, "%s/r", dir);
    (void)unlink(p);
    snprintf(p, sizeof p, "%s/s", dir);
    (void)unlink(p);
}
static void open_topology(topology t, int *tx, int *rx)
{
    int fds[2];
    if (t == PAIR) {
        CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, fds) == 0);
        *tx = fds[0];
        *rx = fds[1];
    } else {
        struct sockaddr_un r = {.sun_family = AF_UNIX}, s = {.sun_family = AF_UNIX};
        snprintf(r.sun_path, sizeof r.sun_path, "%s/r", dir);
        snprintf(s.sun_path, sizeof s.sun_path, "%s/s", dir);
        unlink_paths();
        *rx = socket(AF_UNIX, SOCK_DGRAM, 0);
        *tx = socket(AF_UNIX, SOCK_DGRAM, 0);
        CHECK(*rx >= 0 && *tx >= 0);
        CHECK(bind(*rx, (struct sockaddr *)&r, sizeof r) == 0);
        if (t == BOUND_BOTH) {
            CHECK(bind(*tx, (struct sockaddr *)&s, sizeof s) == 0);
        }
        CHECK(connect(*tx, (struct sockaddr *)&r, sizeof r) == 0);
        if (t == BOUND_BOTH) {
            CHECK(connect(*rx, (struct sockaddr *)&s, sizeof s) == 0);
        }
    }
    int size = BUDGET, one = 1;
    CHECK(setsockopt(*tx, SOL_SOCKET, SO_SNDBUF, &size, sizeof size) == 0);
    CHECK(setsockopt(*rx, SOL_SOCKET, SO_RCVBUF, &size, sizeof size) == 0);
    CHECK(setsockopt(*tx, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one) == 0);
    CHECK(fcntl(*tx, F_SETFL, fcntl(*tx, F_GETFL) | O_NONBLOCK) == 0);
}
static void close_topology(int tx, int rx)
{
    close(tx);
    close(rx);
    unlink_paths();
}
/* What a sender's address costs each datagram: a bound sender stamps its
 * sockaddr_un (sa_len = SUN_LEN rounded by the kernel), an unbound one the
 * 16-byte sun_noname. Measured below in section 2. */
static int overhead_of(topology t, int measured_bound) { return t == BOUND_BOTH ? measured_bound : OVERHEAD; }
static int bound_overhead = (int)sizeof(struct sockaddr_un);

/* Send `len`-byte datagrams until the receiver refuses; returns how many fit. */
static unsigned fill(int tx, size_t len, int *error)
{
    unsigned n = 0;
    for (;;) {
        if (send(tx, scratch, len, 0) < 0) {
            *error = errno;
            return n;
        }
        ++n;
    }
}
static void drain_all(int rx)
{
    while (recv(rx, scratch, sizeof scratch, MSG_DONTWAIT) >= 0) {}
}
/* One registration plus one harvest. Returns events harvested (0 on timeout). */
static int kq_write(int kq, int fd, unsigned short flags, int64_t lowat, int timeout_ms, struct kevent *out)
{
    struct kevent ch;
    EV_SET(&ch, fd, EVFILT_WRITE, EV_ADD | EV_ENABLE | flags, lowat > 0 ? NOTE_LOWAT : 0, lowat > 0 ? lowat : 0, NULL);
    struct timespec ts = {timeout_ms / 1000, (long)(timeout_ms % 1000) * 1000000L};
    int n;
    do n = kevent(kq, &ch, 1, out, 1, &ts); while (n < 0 && errno == EINTR);
    CHECK(n >= 0);
    return n;
}
static int kq_harvest(int kq, int timeout_ms, struct kevent *out)
{
    struct timespec ts = {timeout_ms / 1000, (long)(timeout_ms % 1000) * 1000000L};
    int n;
    do n = kevent(kq, NULL, 0, out, 1, &ts); while (n < 0 && errno == EINTR);
    CHECK(n >= 0);
    return n;
}
static void kq_delete(int kq, int fd)
{
    struct kevent ch;
    EV_SET(&ch, fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    (void)kevent(kq, &ch, 1, NULL, 0, NULL);
}
static void show(const char *what, int n, const struct kevent *e)
{
    if (n) {
        printf("    %-46s fired  data=%-6lld%s\n", what, (long long)e->data, e->flags & EV_EOF ? " EV_EOF" : "");
    } else {
        printf("    %-46s silent\n", what);
    }
}
static void drain_one(int rx)
{
    CHECK(recv(rx, scratch, sizeof scratch, 0) >= 0);
}

/* ---- 1, 2: snapshots ------------------------------------------------------ */
static int64_t level_data(int kq, int tx)
{
    struct kevent e;
    int64_t d = kq_write(kq, tx, EV_ONESHOT, 0, 0, &e) ? e.data : -1;
    kq_delete(kq, tx);
    return d;
}
static void snapshot(topology t)
{
    const size_t len = 1200;
    int tx, rx, error = 0;
    struct kevent e;
    open_topology(t, &tx, &rx);
    int kq = kqueue();
    CHECK(kq >= 0);
    /* 2 first: what one datagram costs the receiver, read off kn_data. */
    int64_t before = level_data(kq, tx);
    CHECK(send(tx, scratch, 100, 0) == 100);
    int64_t after = level_data(kq, tx);
    drain_all(rx);
    if (t == BOUND_BOTH && before > after) bound_overhead = (int)(before - after - 100);
    int64_t need = (int64_t)len + overhead_of(t, bound_overhead);
    printf("  [%s] empty data=%lld; one 100-byte datagram costs %lld bytes; 1200-byte need %lld\n", topology_name[t],
        (long long)before, (long long)(before - after), (long long)need);
    unsigned fit = fill(tx, len, &error);
    printf("    filled with %u datagrams, then errno %d (%s)\n", fit, error, strerror(error));
    show("full, level (default lowat)", kq_write(kq, tx, EV_ONESHOT, 0, 0, &e), &e);
    show("full, level NOTE_LOWAT=need", kq_write(kq, tx, EV_ONESHOT, need, 0, &e), &e);
    kq_delete(kq, tx);
    show("full, EV_CLEAR at attach", kq_write(kq, tx, EV_CLEAR, 0, 0, &e), &e);
    show("full, EV_CLEAR second harvest", kq_harvest(kq, 0, &e), &e);
    drain_one(rx);
    show("after 1 recv, EV_CLEAR harvest", kq_harvest(kq, 0, &e), &e);
    show("then EV_CLEAR again (no recv)", kq_harvest(kq, 0, &e), &e);
    drain_one(rx);
    drain_one(rx);
    drain_one(rx);
    show("after 3 more recvs, EV_CLEAR harvest", kq_harvest(kq, 0, &e), &e);
    show("then EV_CLEAR again (no recv)", kq_harvest(kq, 0, &e), &e);
    kq_delete(kq, tx);
    (void)fill(tx, len, &error);
    show("refilled, level NOTE_LOWAT=need (attach)", kq_write(kq, tx, EV_ONESHOT, need, 0, &e), &e);
    drain_one(rx);
    show("after 1 recv, harvest of that registration", kq_harvest(kq, 0, &e), &e);
    kq_delete(kq, tx);
    close(kq);
    close_topology(tx, rx);
}

/* ---- 3: reliability -------------------------------------------------------- */
typedef enum mode { LOWAT_ONESHOT, CLEAR_PERSISTENT, LEVEL_ONESHOT } mode;
static const char *mode_name[] = {"level+NOTE_LOWAT=need oneshot", "EV_CLEAR persistent", "level oneshot, default lowat"};
typedef struct reader_arg { int rx; unsigned expect, got, bad; unsigned pace_every; long pace_ns; _Atomic bool stop; } reader_arg;
static void *reader(void *p)
{
    reader_arg *r = p;
    uint8_t buf[MAX_LEN];
    /* An aborted run (no wake, as on bound-oneway) leaves this reader with
     * nothing to read and nothing that ends a bound receiver's recv: the
     * receive timeout only lets it notice the abort. */
    struct timeval tv = {0, 200000};
    CHECK(setsockopt(r->rx, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) == 0);
    while (r->got < r->expect) {
        ssize_t n = recv(r->rx, buf, sizeof buf, 0);
        if (n < 0 && errno == EAGAIN && !atomic_load(&r->stop)) continue;
        if (n < 0) break;
        uint32_t seq = 0;
        memcpy(&seq, buf, sizeof seq);
        if (n < 4 || seq != r->got) ++r->bad;
        ++r->got;
        if (r->pace_every && r->got % r->pace_every == 0) {
            struct timespec ts = {0, r->pace_ns};
            nanosleep(&ts, NULL); /* the slow reader under test, not synchronisation */
        }
    }
    return NULL;
}
typedef struct result { unsigned enobufs, wakes, early, timeouts; bool aborted; } result;
static result reliability(topology t, mode m, unsigned count, unsigned pace_every, long pace_ns, int wait_ms)
{
    int tx, rx;
    open_topology(t, &tx, &rx);
    int kq = kqueue();
    CHECK(kq >= 0);
    struct kevent e;
    if (m == CLEAR_PERSISTENT) (void)kq_write(kq, tx, EV_CLEAR, 0, 0, &e);
    reader_arg ra = {.rx = rx, .expect = count, .pace_every = pace_every, .pace_ns = pace_ns};
    pthread_t th;
    CHECK(pthread_create(&th, NULL, reader, &ra) == 0);
    result res = {0};
    uint8_t buf[MAX_LEN] = {0};
    uint32_t rng = 0x9e3779b9u;
    unsigned spins = 0;
    for (uint32_t seq = 0; seq < count && !res.aborted;) {
        rng = rng * 1664525u + 1013904223u;
        size_t len = 4 + (rng >> 8) % (MAX_LEN - 4);
        memcpy(buf, &seq, sizeof seq);
        bool parked = false;
        for (;;) {
            if (send(tx, buf, len, 0) == (ssize_t)len) {
                ++seq;
                break;
            }
            CHECK(errno == ENOBUFS || errno == EAGAIN);
            if (parked) {
                ++res.early;
            } else {
                ++res.enobufs;
            }
            parked = true;
            int n = m == LOWAT_ONESHOT ? kq_write(kq, tx, EV_ONESHOT, (int64_t)len + overhead_of(t, bound_overhead), wait_ms, &e)
                  : m == LEVEL_ONESHOT ? kq_write(kq, tx, EV_ONESHOT, 0, wait_ms, &e)
                                       : kq_harvest(kq, wait_ms, &e);
            if (!n) {
                ++res.timeouts;
                res.aborted = true;
                break;
            }
            ++res.wakes;
            /* An unconditional level registration fires at once on any residual
             * space: bound the spin so the run ends with its count. */
            if (m == LEVEL_ONESHOT && ++spins > 2000000u) {
                res.aborted = true;
                break;
            }
        }
    }
    atomic_store(&ra.stop, true);
    close(tx);
    pthread_join(th, NULL);
    close(kq);
    close(rx);
    unlink_paths();
    printf("  [%s] %-30s pace %u/%ldns: %s, ENOBUFS %u, wakes %u, early %u, timeouts %u, reader got %u/%u bad %u\n",
        topology_name[t], mode_name[m], pace_every, pace_ns, res.aborted ? "ABORTED" : "complete", res.enobufs, res.wakes,
        res.early, res.timeouts, ra.got, count, ra.bad);
    if (!res.aborted) CHECK(ra.got == count && ra.bad == 0);
    return res;
}

/* ---- 4: edge cases ---------------------------------------------------------- */
static void *close_later(void *p)
{
    int *fd = p;
    /* Gives the writer time to park; the result holds either way (a close
     * before the arm is seen at attach). */
    struct timespec ts = {0, 20 * 1000000L};
    nanosleep(&ts, NULL);
    close(*fd);
    return NULL;
}
static void edges(void)
{
    int tx, rx, error;
    struct kevent e;
    int kq = kqueue();
    CHECK(kq >= 0);
    /* Drain between the refused send and the arm: the attach evaluation fires. */
    open_topology(PAIR, &tx, &rx);
    (void)fill(tx, 1200, &error);
    drain_one(rx);
    int n = kq_write(kq, tx, EV_ONESHOT, 1200 + OVERHEAD, 0, &e);
    show("drained before arming, NOTE_LOWAT (attach)", n, &e);
    CHECK(n == 1);
    kq_delete(kq, tx);
    /* A need above the receiver's whole capacity never fires: it can never fit. */
    int small = 1024;
    CHECK(setsockopt(rx, SOL_SOCKET, SO_RCVBUF, &small, sizeof small) == 0);
    drain_all(rx);
    errno = 0;
    ssize_t s = send(tx, scratch, 1200, 0);
    printf("    client rcvbuf shrunk to 1024, empty: 1200-byte send -> %zd errno %d (%s)\n", s, s < 0 ? errno : 0, s < 0 ? strerror(errno) : "-");
    n = kq_write(kq, tx, EV_ONESHOT, 1200 + OVERHEAD, 200, &e);
    show("need above capacity, NOTE_LOWAT, 200 ms", n, &e);
    kq_delete(kq, tx);
    close_topology(tx, rx);
    /* NOTE_LOWAT is clamped to the sender's so_snd hiwat. */
    open_topology(PAIR, &tx, &rx);
    int sb = 2048;
    CHECK(setsockopt(tx, SOL_SOCKET, SO_SNDBUF, &sb, sizeof sb) == 0);
    (void)fill(tx, 1200, &error);
    drain_one(rx);
    printf("    sndbuf 2048: receiver data after fill+1 recv = %lld\n", (long long)level_data(kq, tx));
    n = kq_write(kq, tx, EV_ONESHOT, 4096, 0, &e);
    show("sndbuf 2048, NOTE_LOWAT 4096", n, &e);
    kq_delete(kq, tx);
    close_topology(tx, rx);
    /* A peer that closes while the writer is parked does NOT wake it: a
     * datagram disconnect (unp_drop -> unp_disconnect) sets so_error and
     * clears SS_ISCONNECTED without sowwakeup, so no knote is re-evaluated.
     * fipsd must therefore cancel a parked writer itself when the flow ends.
     * A registration made after the close sees so_error at attach. */
    open_topology(PAIR, &tx, &rx);
    (void)fill(tx, 1200, &error);
    pthread_t th;
    CHECK(pthread_create(&th, NULL, close_later, &rx) == 0);
    n = kq_write(kq, tx, EV_ONESHOT, 1200 + OVERHEAD, 500, &e);
    pthread_join(th, NULL);
    show("peer closed while parked, NOTE_LOWAT, 500 ms", n, &e);
    kq_delete(kq, tx);
    n = kq_write(kq, tx, EV_ONESHOT, 1200 + OVERHEAD, 0, &e);
    show("registered after the peer closed (attach)", n, &e);
    kq_delete(kq, tx);
    errno = 0;
    s = send(tx, scratch, 1200, 0);
    printf("    send after the peer closed -> %zd errno %d (%s)\n", s, s < 0 ? errno : 0, s < 0 ? strerror(errno) : "-");
    close(tx);
    close(kq);
}

int main(int argc, char **argv)
{
    unsigned cycles = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : CYCLES;
    setvbuf(stdout, NULL, _IOLBF, 0);
    snprintf(dir, sizeof dir, "/tmp/fkqXXXXXX");
    CHECK(mkdtemp(dir));
    puts("1-2. snapshots (rcvbuf/sndbuf 65536)");
    for (topology t = PAIR; t <= BOUND_BOTH; ++t) snapshot(t);
    puts("3. reliability (slow reader, random 4..1399-byte datagrams)");
    for (topology t = PAIR; t <= BOUND_BOTH; ++t) {
        /* bound-oneway: the receiver has no unp_conn, so uipc_rcvd wakes
         * nobody; its expected silence is waited out briefly. */
        result r = reliability(t, LOWAT_ONESHOT, t == BOUND_ONEWAY ? 2000 : cycles, 8, 20000, t == BOUND_ONEWAY ? 300 : WAIT_MS);
        if (t != BOUND_ONEWAY) {
            CHECK(!r.aborted && r.timeouts == 0 && r.early == 0 && r.enobufs > 1000);
        }
    }
    (void)reliability(PAIR, LOWAT_ONESHOT, cycles, 1, 1000, WAIT_MS);
    (void)reliability(PAIR, LOWAT_ONESHOT, cycles, 0, 0, WAIT_MS);
    (void)reliability(PAIR, CLEAR_PERSISTENT, cycles, 8, 20000, WAIT_MS);
    (void)reliability(BOUND_BOTH, CLEAR_PERSISTENT, cycles, 8, 20000, WAIT_MS);
    (void)reliability(PAIR, LEVEL_ONESHOT, cycles / 10, 8, 20000, WAIT_MS);
    puts("4. edge cases (socketpair)");
    edges();
    (void)rmdir(dir);
    puts("ok");
    return 0;
}
#endif
