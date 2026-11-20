/* The CoreBluetooth backend over the fips-ble-agent socket (upstream
 * io_macos/agent_server.rs and the io_radio bridge it drives); see
 * agent_client.h. Fibers: one accepting, one reading each connection (hello,
 * then frames) and one writing each live connection's queued frames. Stream
 * ops park their own fiber on channels; the reader hands them RECV bytes,
 * CREDIT and CLOSED, and dial results through each dial's reply channel.
 *
 * Lifetimes are counted references: a connection (session) holds the
 * backend, a channel (stream) holds its session, and the reader, the writer
 * and every in-flight dial hold their session, because the core may close a
 * stream after destroy() (a queued inbound stream is discarded then). */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "transport/ble/agent_client.h"
#include "transport/ble/internal.h"
#include "privilege.h"
#include "runtime.h"
#include "log.h"
#include <blocking_executor.h>
#include <libgo/fiber.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>
#define MOD "ble"

typedef struct agent agent;
typedef struct session session;
typedef struct achan achan;
typedef struct outframe { struct outframe *next; size_t len; uint8_t bytes[]; } outframe;
typedef struct rxchunk { size_t len, off; uint8_t bytes[]; } rxchunk;
/* A dial parked in connect(), on its caller's stack; the reader answers it
 * only while it is listed, under the session lock. */
typedef struct dial {
    struct dial *next;
    int64_t id;
    GoChannel *reply;
    bool answered;
    int rc;
    achan *ch;
} dial;
struct agent {
    _Atomic int refs;
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    uint32_t hello_timeout_ms;
    gid_t gid;
    fipsd_runtime *rt;
    fipsd_ble_sink *sink;
    GoChannel *stop_ch, *changed;
    _Atomic uint64_t next_stream;
    nsync_mu mu; /* guards below; never held across a park */
    bool started, stopped, bound;
    int listener;
    dev_t dev;
    ino_t ino;
    session *sessions, *live;
    size_t pending;
    int64_t next_dial;
    fipsd_ble_agent_stats stats;
};
struct session {
    _Atomic int refs;
    agent *a;
    session *next; /* a->sessions, under a->mu */
    int fd;
    bool hello, writer; /* hello: a->mu */
    uint16_t psm;
    _Atomic bool dead;
    GoChannel *dead_ch, *wake, *writer_done;
    uint8_t *buf;
    nsync_mu mu; /* guards below and every channel's registered/eof/credit */
    outframe *head, **tail;
    achan *chans;
    dial *dials;
};
struct achan {
    fipsd_ble_stream base; /* first */
    _Atomic int refs;
    session *s;
    uint32_t ch;
    uint16_t mtu;
    GoChannel *rx, *credit_wake, *cancel;
    _Atomic bool cancelled;
    rxchunk *partial; /* the reading fiber's */
    achan *next;
    bool registered, eof;
    int64_t credit;
};

static void wake(GoChannel *c) { (void)go_channel_try_send(c, c); }
/* Reacquire TLS errno after a migrating fiber resumes (tcp_io_errno). */
__attribute__((noinline)) static int io_errno(void) { return errno; }
/* gof timeout for an absolute deadline: -1 none, -2 already passed. */
static int remaining(uint64_t deadline)
{
    if (deadline == FIPSD_BLE_NO_DEADLINE) return -1;
    uint64_t now = ble_now();
    if (now >= deadline) return -2;
    return deadline - now > INT_MAX ? INT_MAX : (int)(deadline - now);
}
static int park(GoSelectCase *cases, size_t n, uint64_t deadline)
{
    if (deadline == FIPSD_BLE_NO_DEADLINE) return go_select(cases, n);
    uint64_t now = ble_now();
    return go_select_timeout(cases, n, deadline > now ? deadline - now : 0).selected_case;
}

/* ---- references ------------------------------------------------------------- */
static void agent_unref(agent *a)
{
    if (atomic_fetch_sub(&a->refs, 1) != 1) return;
    go_channel_unref(a->stop_ch); go_channel_unref(a->changed);
    free(a);
}
static void session_unref(session *s)
{
    if (atomic_fetch_sub(&s->refs, 1) != 1) return;
    while (s->head) { outframe *o = s->head; s->head = o->next; free(o); }
    go_channel_unref(s->dead_ch); go_channel_unref(s->wake); go_channel_unref(s->writer_done);
    agent *a = s->a;
    free(s->buf); free(s);
    agent_unref(a);
}
static session *session_new(agent *a, int fd)
{
    session *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->dead_ch = go_channel_create(1); s->wake = go_channel_create(1); s->writer_done = go_channel_create(1);
    if (!s->dead_ch || !s->wake || !s->writer_done) {
        if (s->dead_ch) go_channel_unref(s->dead_ch);
        if (s->wake) go_channel_unref(s->wake);
        if (s->writer_done) go_channel_unref(s->writer_done);
        free(s); return NULL;
    }
    atomic_init(&s->refs, 1); atomic_init(&s->dead, false);
    s->a = a; s->fd = fd; s->tail = &s->head;
    nsync_mu_init(&s->mu);
    atomic_fetch_add(&a->refs, 1);
    return s;
}
static void chan_unref(achan *c)
{
    if (atomic_fetch_sub(&c->refs, 1) != 1) return;
    void *v;
    free(c->partial);
    if (c->rx) { while (go_channel_try_receive(c->rx, &v) == 0) free(v); go_channel_unref(c->rx); }
    if (c->credit_wake) go_channel_unref(c->credit_wake);
    if (c->cancel) go_channel_unref(c->cancel);
    session *s = c->s;
    free(c);
    if (s) session_unref(s);
}
/* The session holding the radio, referenced; NULL with the reason. */
static session *live_ref(agent *a, int *rc)
{
    nsync_mu_lock(&a->mu);
    session *s = a->stopped ? NULL : a->live;
    if (s) atomic_fetch_add(&s->refs, 1);
    *rc = a->stopped ? -ECANCELED : -ENODEV;
    nsync_mu_unlock(&a->mu);
    return s;
}

/* ---- the outbound queue: frames to the agent, written by the writer fiber --- */
static bool post_locked(session *s, const fipsd_ble_agent_frame *f)
{
    if (atomic_load(&s->dead)) return false;
    size_t n = fipsd_ble_agent_encoded_len(f);
    outframe *o = n ? malloc(sizeof *o + n) : NULL;
    if (!o) return false;
    o->len = fipsd_ble_agent_encode(f, o->bytes, n); o->next = NULL;
    *s->tail = o; s->tail = &o->next;
    wake(s->wake);
    return true;
}
static bool post(session *s, const fipsd_ble_agent_frame *f)
{
    nsync_mu_lock(&s->mu); bool ok = post_locked(s, f); nsync_mu_unlock(&s->mu);
    return ok;
}
static outframe *pop(session *s)
{
    nsync_mu_lock(&s->mu);
    outframe *o = s->head;
    if (o && !(s->head = o->next)) s->tail = &s->head;
    nsync_mu_unlock(&s->mu);
    return o;
}

/* ---- socket I/O on the calling fiber ------------------------------------------ */
static int send_all(int fd, const uint8_t *p, size_t n, uint64_t deadline)
{
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL; /* Apple: SO_NOSIGPIPE on the descriptor */
#endif
    while (n) {
        int t = remaining(deadline);
        if (t == -2) return -ETIMEDOUT;
        struct iovec v = {.iov_base = (void *)p, .iov_len = n};
        struct msghdr m = {.msg_iov = &v, .msg_iovlen = 1};
        ssize_t k = gof_sendmsg(fd, &m, flags, t);
        if (k < 0) { int e = io_errno(); if (e == EINTR) continue; return -e; }
        if (!k) return -EPIPE;
        p += k; n -= (size_t)k;
    }
    return 0;
}
/* Exactly n bytes: 0, 1 for a clean end before the first byte, or -errno
 * (-EPIPE: the stream ended part way). */
static int recv_all(int fd, uint8_t *p, size_t n, uint64_t deadline)
{
    size_t got = 0;
    while (got < n) {
        int t = remaining(deadline);
        if (t == -2) return -ETIMEDOUT;
        struct iovec v = {.iov_base = p + got, .iov_len = n - got};
        struct msghdr m = {.msg_iov = &v, .msg_iovlen = 1};
        ssize_t k = gof_recvmsg(fd, &m, 0, t);
        if (k < 0) { int e = io_errno(); if (e == EINTR) continue; return -e; }
        if (!k) return got ? -EPIPE : 1;
        got += (size_t)k;
    }
    return 0;
}
/* One frame body into `body`: 0, 1 for a clean end between frames, or -errno
 * (-EBADMSG: an empty or oversized length, refused before reading it). */
static int read_frame(int fd, uint8_t *body, size_t max, uint64_t deadline, size_t *len)
{
    uint8_t h[FIPSD_BLE_AGENT_HEADER];
    int rc = recv_all(fd, h, sizeof h, deadline);
    if (rc) return rc;
    if (!fipsd_ble_agent_frame_len(h, max, len)) return -EBADMSG;
    rc = recv_all(fd, body, *len, deadline);
    return rc == 1 ? -EPIPE : rc;
}

/* ---- channels: one L2CAP channel the agent relays --------------------------- */
static bool chan_stopped(achan *c) { return atomic_load(&c->cancelled) || go_channel_is_closed(c->s->a->stop_ch); }
static ssize_t chan_read(fipsd_ble_stream *bs, uint8_t *buf, size_t cap, uint64_t deadline)
{
    achan *c = (achan *)bs; session *s = c->s;
    if (!cap) return -EINVAL;
    size_t got = 0;
    for (;;) {
        if (chan_stopped(c)) return -ECANCELED;
        rxchunk *k = c->partial; c->partial = NULL;
        void *v = NULL;
        if (!k && go_channel_try_receive(c->rx, &v) == 0) k = v;
        if (!k && got) return (ssize_t)got; /* a byte stream returns what it has */
        if (!k) {
            if (go_channel_is_closed(c->rx)) return 0; /* CLOSED, and everything before it was read */
            if (atomic_load(&s->dead)) return -ECONNRESET;
            GoSelectCase cases[] = {{GO_SELECT_RECEIVE, c->rx, NULL, &v}, {GO_SELECT_RECEIVE, c->cancel, NULL, &v},
                {GO_SELECT_RECEIVE, s->a->stop_ch, NULL, &v}, {GO_SELECT_RECEIVE, s->dead_ch, NULL, &v}};
            int selected = park(cases, 4, deadline);
            if (selected < 0) return -ETIMEDOUT;
            if (selected != 0 || !v) continue; /* re-evaluated above: bytes already queued come first */
            k = v;
        }
        size_t take = k->len - k->off < cap - got ? k->len - k->off : cap - got;
        memcpy(buf + got, k->bytes + k->off, take); got += take; k->off += take;
        if (k->off < k->len) { c->partial = k; return (ssize_t)got; }
        free(k);
        if (got == cap) return (ssize_t)got;
    }
}
/* Spends the channel's credit, a SEND at a time; parks for CREDIT. */
static int chan_write(fipsd_ble_stream *bs, const uint8_t *p, size_t len, uint64_t deadline)
{
    achan *c = (achan *)bs; session *s = c->s;
    for (size_t at = 0; at < len;) {
        if (chan_stopped(c)) return -ECANCELED;
        bool sent = false;
        nsync_mu_lock(&s->mu);
        int rc = atomic_load(&s->dead) ? -ECONNRESET : (!c->registered || c->eof) ? -EPIPE : 0;
        if (!rc && c->credit > 0) {
            size_t n = len - at < FIPSD_BLE_AGENT_MAX_DATA ? len - at : FIPSD_BLE_AGENT_MAX_DATA;
            fipsd_ble_agent_frame f = {.tag = FIPSD_BLE_AGENT_SEND, .ch = c->ch, .data = p + at, .len = n};
            if (!post_locked(s, &f)) rc = atomic_load(&s->dead) ? -ECONNRESET : -ENOMEM;
            else { c->credit -= (int64_t)n; at += n; sent = true; if (c->credit > 0) wake(c->credit_wake); }
        }
        nsync_mu_unlock(&s->mu);
        if (rc) return rc;
        if (sent) continue;
        void *v = NULL;
        GoSelectCase cases[] = {{GO_SELECT_RECEIVE, c->credit_wake, NULL, &v}, {GO_SELECT_RECEIVE, c->cancel, NULL, &v},
            {GO_SELECT_RECEIVE, s->a->stop_ch, NULL, &v}, {GO_SELECT_RECEIVE, s->dead_ch, NULL, &v}};
        if (park(cases, 4, deadline) < 0) return -ETIMEDOUT;
    }
    return 0;
}
static void chan_cancel(fipsd_ble_stream *bs)
{
    achan *c = (achan *)bs;
    if (!atomic_exchange(&c->cancelled, true)) go_channel_close(c->cancel);
}
static void unlink_locked(session *s, achan *c)
{
    for (achan **p = &s->chans; *p; p = &(*p)->next) if (*p == c) { *p = c->next; break; }
    c->registered = false;
}
/* The core is done with it: tell the agent unless the agent ended it first. */
static void chan_close(fipsd_ble_stream *bs)
{
    achan *c = (achan *)bs; session *s = c->s;
    nsync_mu_lock(&s->mu);
    if (c->registered) {
        unlink_locked(s, c);
        fipsd_ble_agent_frame f = {.tag = FIPSD_BLE_AGENT_CLOSE, .ch = c->ch};
        (void)post_locked(s, &f); /* dropped with a dead session, which took the channel with it */
    }
    nsync_mu_unlock(&s->mu);
    wake(s->a->changed);
    chan_unref(c);
}
static void chan_mtu(const fipsd_ble_stream *bs, uint16_t *send, uint16_t *recv)
{ const achan *c = (const achan *)bs; *send = *recv = c->mtu; }
static int chan_fd(const fipsd_ble_stream *bs) { (void)bs; return -1; }
static const fipsd_ble_stream_ops chan_ops = {.read = chan_read, .write = chan_write, .cancel = chan_cancel,
    .close = chan_close, .mtu = chan_mtu, .fd = chan_fd};
static achan *find_locked(session *s, uint32_t ch)
{
    for (achan *c = s->chans; c; c = c->next) if (c->ch == ch) return c;
    return NULL;
}
/* Register agent channel `ch`; NULL on a duplicate (*dup), allocation failure
 * or a dead session. Credit starts at 0: the agent grants it. */
static achan *chan_open(session *s, uint32_t ch, const uint8_t addr[6], uint16_t mtu, bool *dup)
{
    *dup = false;
    achan *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    atomic_init(&c->refs, 1); atomic_init(&c->cancelled, false);
    c->rx = go_channel_create(FIPSD_BLE_AGENT_RX_DEPTH); c->credit_wake = go_channel_create(1); c->cancel = go_channel_create(1);
    if (!c->rx || !c->credit_wake || !c->cancel) { chan_unref(c); return NULL; }
    c->base.ops = &chan_ops;
    c->base.peer = (fipsd_ble_peer){FIPSD_BLE_RADIO_ADAPTER, {0}};
    memcpy(c->base.peer.device, addr, 6);
    c->base.id = atomic_fetch_add(&s->a->next_stream, 1) + 1;
    c->ch = ch; c->mtu = mtu;
    atomic_fetch_add(&s->refs, 1); c->s = s;
    nsync_mu_lock(&s->mu);
    *dup = find_locked(s, ch) != NULL;
    bool ok = !*dup && !atomic_load(&s->dead);
    if (ok) { c->registered = true; c->next = s->chans; s->chans = c; }
    nsync_mu_unlock(&s->mu);
    if (!ok) { chan_unref(c); return NULL; }
    wake(s->a->changed);
    return c;
}

/* ---- the reader: frames from the agent ---------------------------------------- */
static void deliver(session *s, const fipsd_ble_agent_frame *f)
{
    nsync_mu_lock(&s->mu);
    achan *c = find_locked(s, f->ch);
    if (c && !c->eof) atomic_fetch_add(&c->refs, 1); else c = NULL;
    nsync_mu_unlock(&s->mu);
    if (!c) return; /* a channel the core already closed: its late bytes go nowhere */
    rxchunk *k = malloc(sizeof *k + f->len);
    if (k) {
        k->len = f->len; k->off = 0; memcpy(k->bytes, f->data, f->len);
        /* Waits for room rather than drop (upstream deliver_recv_blocking): a
         * hole in a byte stream breaks its framing. A cancelled stream or a
         * stopped backend takes nothing more. */
        int selected = go_channel_try_send(c->rx, k) == 0 ? 0 : chan_stopped(c) ? 1 : -1;
        if (selected < 0) {
            void *v = NULL;
            GoSelectCase cases[] = {{GO_SELECT_SEND, c->rx, k, NULL}, {GO_SELECT_RECEIVE, c->cancel, NULL, &v},
                {GO_SELECT_RECEIVE, s->a->stop_ch, NULL, &v}};
            selected = go_select(cases, 3);
        }
        if (selected != 0) free(k);
    }
    chan_unref(c);
}
static void dispatch(session *s, const fipsd_ble_agent_frame *f)
{
    agent *a = s->a;
    bool dup;
    switch (f->tag) {
    case FIPSD_BLE_AGENT_INBOUND: {
        achan *c = chan_open(s, f->ch, f->addr, f->mtu, &dup);
        if (!c) {
            if (dup) FIPSD_WARN(MOD, "BLE agent reused channel %u; ignoring the second", f->ch);
            else { fipsd_ble_agent_frame no = {.tag = FIPSD_BLE_AGENT_CLOSE, .ch = f->ch}; (void)post(s, &no); }
            break;
        }
        (void)fipsd_ble_sink_inbound(a->sink, &c->base); /* ownership passes, delivered or not */
        break;
    }
    case FIPSD_BLE_AGENT_CONNECT_RESULT: {
        achan *c = f->ok ? chan_open(s, f->ch, f->addr, f->mtu, &dup) : NULL;
        if (f->ok && !c && dup) FIPSD_WARN(MOD, "BLE agent reused channel %u for dial %lld", f->ch, (long long)f->id);
        bool lost = f->ok && !c && !dup; /* opened by the agent but not registered here: still its to drop */
        nsync_mu_lock(&s->mu);
        dial *d = NULL;
        for (dial **p = &s->dials; *p; p = &(*p)->next)
            if ((*p)->id == f->id) { d = *p; *p = d->next; break; }
        if (d) {
            d->answered = true; d->ch = c; c = NULL;
            d->rc = d->ch ? 0 : f->ok ? -ENOMEM : -EIO; /* the agent gives no reason for a failed dial */
            (void)go_channel_try_send(d->reply, d);
        }
        if (lost) { fipsd_ble_agent_frame no = {.tag = FIPSD_BLE_AGENT_CLOSE, .ch = f->ch}; (void)post_locked(s, &no); }
        nsync_mu_unlock(&s->mu);
        if (c) chan_close(&c->base); /* answered after its dial gave up: nobody wants it */
        break;
    }
    case FIPSD_BLE_AGENT_SCAN: {
        fipsd_ble_peer p = {FIPSD_BLE_RADIO_ADAPTER, {0}};
        memcpy(p.device, f->addr, 6);
        int16_t rssi = f->rssi;
        uint8_t sd[2];
        bool psm = f->psm != FIPSD_BLE_AGENT_NO_PSM;
        if (psm) fipsd_ble_encode_psm(f->psm, sd); /* the core decodes service data, as from BlueZ */
        (void)fipsd_ble_sink_discovered(a->sink, &p, f->rssi == FIPSD_BLE_AGENT_NO_RSSI ? NULL : &rssi,
            psm ? sd : NULL, psm ? sizeof sd : 0);
        break;
    }
    case FIPSD_BLE_AGENT_RECV:
        if (f->len) deliver(s, f);
        break;
    case FIPSD_BLE_AGENT_CLOSED: {
        nsync_mu_lock(&s->mu);
        achan *c = find_locked(s, f->ch);
        if (c) { unlink_locked(s, c); c->eof = true; wake(c->credit_wake); go_channel_close(c->rx); }
        nsync_mu_unlock(&s->mu);
        if (c) wake(a->changed);
        break;
    }
    case FIPSD_BLE_AGENT_CREDIT: {
        nsync_mu_lock(&s->mu);
        achan *c = find_locked(s, f->ch);
        if (c) { c->credit += f->bytes; wake(c->credit_wake); }
        nsync_mu_unlock(&s->mu);
        break;
    }
    case FIPSD_BLE_AGENT_HELLO: break;
    default: FIPSD_DEBUG(MOD, "BLE agent: unexpected frame (tag %d)", (int)f->tag); break;
    }
}
static void writer_fiber(void *arg)
{
    session *s = arg;
    for (;;) {
        outframe *o = pop(s);
        if (!o) {
            if (atomic_load(&s->dead)) break;
            void *v = NULL;
            GoSelectCase cases[] = {{GO_SELECT_RECEIVE, s->wake, NULL, &v}, {GO_SELECT_RECEIVE, s->dead_ch, NULL, &v}};
            (void)go_select(cases, 2);
            continue;
        }
        int rc = atomic_load(&s->dead) ? -ECANCELED : send_all(s->fd, o->bytes, o->len, FIPSD_BLE_NO_DEADLINE);
        free(o);
        if (rc) {
            if (!atomic_load(&s->dead)) FIPSD_DEBUG(MOD, "BLE agent: write failed (%d)", rc);
            (void)gof_io_cancel_fd(s->fd); /* the reader ends the session */
            break;
        }
    }
    go_channel_close(s->writer_done);
    session_unref(s);
}
/* The session ends: the radio goes with it (upstream "agent disconnected;
 * radio down"), every channel resets, parked dials fail. */
static void teardown(session *s)
{
    agent *a = s->a;
    atomic_store(&s->dead, true);
    (void)gof_io_cancel_fd(s->fd);
    go_channel_close(s->dead_ch);
    nsync_mu_lock(&a->mu);
    for (session **p = &a->sessions; *p; p = &(*p)->next) if (*p == s) { *p = s->next; break; }
    if (!s->hello) a->pending--;
    bool live = a->live == s, report = live && !a->stopped;
    if (live) a->live = NULL;
    nsync_mu_unlock(&a->mu);
    nsync_mu_lock(&s->mu);
    achan *gone = s->chans; s->chans = NULL;
    for (achan *c = gone; c; c = c->next) { c->registered = false; wake(c->credit_wake); atomic_fetch_add(&c->refs, 1); }
    nsync_mu_unlock(&s->mu);
    if (report) fipsd_ble_sink_adapter(a->sink, false, false);
    for (achan *c = gone, *n; c; c = n) {
        n = c->next;
        if (report) (void)fipsd_ble_sink_closed(a->sink, c->base.id);
        chan_unref(c);
    }
    wake(a->changed);
    if (s->writer) { void *v; (void)go_channel_receive(s->writer_done, &v); }
    (void)gof_io_reset_fd(s->fd); /* every user of the descriptor has returned */
    close(s->fd); s->fd = -1;
    session_unref(s);
}
static void session_fiber(void *arg)
{
    session *s = arg; agent *a = s->a;
    uint8_t first[FIPSD_BLE_AGENT_MAX_HELLO_FRAME];
    size_t len = 0;
    fipsd_ble_agent_frame f = {0};
    /* A whole budget, however many reads it takes: a peer trickling a byte at
     * a time cannot hold the connection (upstream DeadlineReader). */
    int rc = read_frame(s->fd, first, sizeof first, ble_now() + a->hello_timeout_ms, &len);
    bool hello = !rc && fipsd_ble_agent_decode(first, len, &f) && f.tag == FIPSD_BLE_AGENT_HELLO;
    if (!hello || f.version != FIPSD_BLE_AGENT_VERSION) {
        if (hello) FIPSD_WARN(MOD, "BLE agent speaks protocol version %u, expected %u", f.version, FIPSD_BLE_AGENT_VERSION);
        else if (!go_channel_is_closed(a->stop_ch)) FIPSD_WARN(MOD, "BLE agent connection did not say hello (%d)", rc);
        nsync_mu_lock(&a->mu); a->stats.refused_hello++; nsync_mu_unlock(&a->mu);
        wake(a->changed);
        goto end;
    }
    nsync_mu_lock(&a->mu);
    s->hello = true; a->pending--;
    bool busy = !a->stopped && a->live, take = !a->stopped && !a->live;
    if (busy) a->stats.refused_busy++;
    if (take) { a->live = s; s->psm = f.psm; a->stats.sessions++; }
    nsync_mu_unlock(&a->mu);
    if (busy) {
        /* One agent at a time: a second user's agent must not take the radio
         * from the first on every reconnect. */
        FIPSD_INFO(MOD, "another fips BLE agent connected while one is live; refusing it");
        uint8_t out[8];
        fipsd_ble_agent_frame b = {.tag = FIPSD_BLE_AGENT_BUSY};
        (void)send_all(s->fd, out, fipsd_ble_agent_encode(&b, out, sizeof out), ble_now() + FIPSD_BLE_AGENT_BUSY_WRITE_MS);
        wake(a->changed);
        goto end;
    }
    if (!take) goto end;
    s->buf = malloc(FIPSD_BLE_AGENT_MAX_FRAME);
    atomic_fetch_add(&s->refs, 1);
    s->writer = s->buf && !fipsd_runtime_spawn(a->rt, writer_fiber, s);
    if (!s->writer) { atomic_fetch_sub(&s->refs, 1); FIPSD_ERROR(MOD, "BLE agent: cannot serve the connection"); goto end; }
    FIPSD_INFO(MOD, "fips BLE agent connected; radio up (psm 0x%04x)", f.psm);
    wake(a->changed);
    fipsd_ble_sink_adapter(a->sink, true, true);
    for (;;) {
        rc = read_frame(s->fd, s->buf, FIPSD_BLE_AGENT_MAX_FRAME, FIPSD_BLE_NO_DEADLINE, &len);
        if (!rc && !fipsd_ble_agent_decode(s->buf, len, &f)) rc = -EBADMSG;
        if (rc) break;
        dispatch(s, &f);
    }
    if (!go_channel_is_closed(a->stop_ch))
        FIPSD_INFO(MOD, "fips BLE agent disconnected; radio down (%s)", rc == 1 ? "closed" : strerror(-rc));
end:
    teardown(s);
}
static int configure_fd(int fd)
{
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) || fcntl(fd, F_SETFL, O_NONBLOCK)) return -1;
#ifdef SO_NOSIGPIPE
    int on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on)) return -1;
#endif
    return 0;
}
static void accept_fiber(void *arg)
{
    agent *a = arg;
    for (;;) {
        int fd = gof_accept(a->listener, NULL, NULL, -1);
        if (fd < 0) {
            int e = io_errno();
            if (e == EINTR || e == ECONNABORTED) continue;
            if (!go_channel_is_closed(a->stop_ch))
                FIPSD_ERROR(MOD, "BLE agent socket %s: accept failed: %s; no agent can connect", a->path, strerror(e));
            break;
        }
        if (configure_fd(fd) || gof_io_reset_fd(fd)) { close(fd); continue; }
        session *s = session_new(a, fd);
        nsync_mu_lock(&a->mu);
        bool ok = s && !a->stopped && a->pending < FIPSD_BLE_AGENT_PENDING_MAX;
        if (ok) { s->next = a->sessions; a->sessions = s; a->pending++; }
        nsync_mu_unlock(&a->mu);
        if (!ok) { if (s) session_unref(s); (void)gof_io_reset_fd(fd); close(fd); continue; }
        if (fipsd_runtime_spawn(a->rt, session_fiber, s)) teardown(s);
    }
}

/* ---- binding (on the blocking executor) ----------------------------------------- */
static int unix_socket(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd >= 0 && configure_fd(fd)) { int e = errno; close(fd); errno = e; return -1; }
    return fd;
}
/* The control socket's directory policy: a parent this creates, or a
 * canonical runtime directory, is group `gid`, mode 0750; a shared or custom
 * existing one is left alone. */
static int secure_parent(const char *path, gid_t gid, char *err, size_t cap)
{
    char parent[PATH_MAX];
    snprintf(parent, sizeof parent, "%s", path);
    char *slash = strrchr(parent, '/');
    if (!slash || slash == parent) return 0;
    *slash = 0;
    struct stat st;
    bool created = stat(parent, &st) != 0 || !S_ISDIR(st.st_mode);
    for (char *p = parent + (parent[0] == '/'); created; ++p) {
        if (*p != '/' && *p) continue;
        char saved = *p; *p = 0;
        if (mkdir(parent, FIPSD_SOCKET_DIR_MODE) != 0 && errno != EEXIST) return -1;
        /* Ancestors may be symlinks (macOS /var -> private/var), as with upstream's
         * create_dir_all; the socket directory itself must be a real directory. */
        if ((saved ? stat(parent, &st) : lstat(parent, &st)) != 0 || !S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
        *p = saved;
        if (!saved) break;
    }
    bool canonical = !strcmp(parent, "/run/fips") || !strcmp(parent, "/var/run/fips");
    return created || canonical ? fipsd_privilege_secure_private_dir(parent, gid, err, cap) : 0;
}
typedef struct bind_job { agent *a; int fd, err; char msg[256]; struct stat st; } bind_job;
static void *bind_listener(void *arg)
{
    bind_job *j = arg; agent *a = j->a;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr); addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, a->path, strlen(a->path) + 1);
    if (secure_parent(a->path, a->gid, j->msg, sizeof j->msg)) { j->err = errno; return NULL; }
    struct stat st;
    if (lstat(a->path, &st) == 0) {
        /* Never steal a live daemon's socket, nor remove what is not a stale one of ours. */
        if (!S_ISSOCK(st.st_mode) || (st.st_uid != geteuid() && geteuid() != 0)) { j->err = EEXIST; return NULL; }
        int probe = unix_socket();
        if (probe < 0) { j->err = errno; return NULL; }
        int rc = connect(probe, (struct sockaddr *)&addr, sizeof addr), e = errno;
        close(probe);
        if (rc == 0 || e != ECONNREFUSED) { j->err = EADDRINUSE; return NULL; }
        if (unlink(a->path)) { j->err = errno; return NULL; }
    } else if (errno != ENOENT) { j->err = errno; return NULL; }
    int fd = unix_socket();
    if (fd < 0) { j->err = errno; return NULL; }
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr)) { j->err = errno; close(fd); return NULL; }
    if (lstat(a->path, &j->st) || fipsd_privilege_secure_socket(a->path, a->gid, j->msg, sizeof j->msg) || listen(fd, 16)) {
        j->err = errno; close(fd); (void)unlink(a->path); return NULL;
    }
    j->fd = fd;
    return NULL;
}

/* ---- the backend vtable --------------------------------------------------------- */
static int b_start(void *self, struct fipsd_runtime *rt, fipsd_ble_sink *sink)
{
    agent *a = self;
    if (a->started || !rt || !sink) return -1;
    a->rt = rt; a->sink = sink;
    bind_job j = {.a = a, .fd = -1};
    (void)go_blocking_submit(bind_listener, &j);
    if (j.fd < 0 || gof_io_reset_fd(j.fd)) {
        if (j.fd >= 0) { close(j.fd); (void)unlink(a->path); }
        FIPSD_ERROR(MOD, "cannot listen for the fips BLE agent at %s: %s%s%s (set %s to another path)", a->path,
            strerror(j.err ? j.err : ENOMEM), *j.msg ? ": " : "", j.msg, FIPSD_BLE_AGENT_SOCKET_ENV);
        return -1;
    }
    nsync_mu_lock(&a->mu);
    a->listener = j.fd; a->bound = true; a->dev = j.st.st_dev; a->ino = j.st.st_ino; a->started = true;
    nsync_mu_unlock(&a->mu);
    fipsd_ble_sink_adapter(sink, false, false); /* no agent yet */
    if (fipsd_runtime_spawn(rt, accept_fiber, a)) {
        FIPSD_ERROR(MOD, "BLE agent socket %s: cannot accept", a->path);
        return -1; /* destroy closes and unlinks */
    }
    FIPSD_INFO(MOD, "waiting for the fips BLE agent at %s", a->path);
    return 0;
}
static void b_stop(void *self)
{
    agent *a = self;
    nsync_mu_lock(&a->mu);
    bool first = !a->stopped;
    a->stopped = true;
    nsync_mu_unlock(&a->mu);
    if (!first) return;
    /* Closed before any session is cut, so whatever a parked dial or stream
     * op wakes to, it finds the stop first and returns -ECANCELED. */
    go_channel_close(a->stop_ch);
    nsync_mu_lock(&a->mu);
    if (a->listener >= 0) (void)gof_io_cancel_fd(a->listener);
    for (session *s = a->sessions; s; s = s->next) (void)gof_io_cancel_fd(s->fd);
    nsync_mu_unlock(&a->mu);
}
static void b_destroy(void *self)
{
    agent *a = self;
    b_stop(a);
    nsync_mu_lock(&a->mu);
    int fd = a->listener; bool bound = a->bound;
    a->listener = -1; a->bound = false; a->sink = NULL;
    nsync_mu_unlock(&a->mu);
    if (fd >= 0) { (void)gof_io_reset_fd(fd); close(fd); }
    struct stat st;
    if (bound && !lstat(a->path, &st) && S_ISSOCK(st.st_mode) && st.st_dev == a->dev && st.st_ino == a->ino) (void)unlink(a->path);
    agent_unref(a); /* streams the core still holds keep their session, and so this, alive */
}
static fipsd_ble_adapter b_adapter(void *self)
{
    agent *a = self;
    nsync_mu_lock(&a->mu); bool up = a->live && !a->stopped; nsync_mu_unlock(&a->mu);
    return (fipsd_ble_adapter){up, up};
}
static int b_listen(void *self, uint16_t requested, uint64_t deadline, uint16_t *bound)
{
    (void)deadline; agent *a = self;
    nsync_mu_lock(&a->mu);
    int rc = a->stopped ? -ECANCELED : a->live ? 0 : -ENODEV;
    uint16_t psm = a->live ? a->live->psm : 0;
    nsync_mu_unlock(&a->mu);
    if (rc) return rc;
    if (!psm) FIPSD_WARN(MOD, "the fips BLE agent has no L2CAP listener yet; reporting psm 0x%04x until it publishes one", requested);
    *bound = psm ? psm : requested;
    return 0;
}
static int command(agent *a, const fipsd_ble_agent_frame *f, bool optional)
{
    int rc;
    session *s = live_ref(a, &rc);
    if (!s) return optional && rc == -ENODEV ? 0 : rc;
    rc = post(s, f) ? 0 : optional ? 0 : -ENODEV;
    session_unref(s);
    return rc;
}
static int b_advertise(void *self, const fipsd_ble_advert *ad, uint64_t deadline)
{ (void)deadline; return command(self, &(fipsd_ble_agent_frame){.tag = FIPSD_BLE_AGENT_START_ADVERTISING, .psm = ad->psm}, false); }
static int b_advertise_stop(void *self) { return command(self, &(fipsd_ble_agent_frame){.tag = FIPSD_BLE_AGENT_STOP_ADVERTISING}, true); }
static int b_scan_start(void *self, uint64_t deadline)
{ (void)deadline; return command(self, &(fipsd_ble_agent_frame){.tag = FIPSD_BLE_AGENT_START_SCANNING}, false); }
static int b_scan_stop(void *self) { return command(self, &(fipsd_ble_agent_frame){.tag = FIPSD_BLE_AGENT_STOP_SCANNING}, true); }
static int b_connect(void *self, const fipsd_ble_peer *p, uint16_t psm, uint64_t deadline, fipsd_ble_stream **out)
{
    agent *a = self;
    int rc;
    session *s = live_ref(a, &rc);
    if (!s) return rc;
    dial d = {.reply = go_channel_create(1)};
    if (!d.reply) { session_unref(s); return -ENOMEM; }
    nsync_mu_lock(&a->mu); d.id = ++a->next_dial; nsync_mu_unlock(&a->mu);
    fipsd_ble_agent_frame f = {.tag = FIPSD_BLE_AGENT_CONNECT, .id = d.id, .psm = psm};
    memcpy(f.addr, p->device, 6); /* the agent knows peers by device bytes under "ble0" */
    nsync_mu_lock(&s->mu);
    bool posted = post_locked(s, &f);
    if (posted) { d.next = s->dials; s->dials = &d; }
    rc = posted ? 0 : atomic_load(&s->dead) ? -ENODEV : -ENOMEM;
    nsync_mu_unlock(&s->mu);
    int selected = 0;
    if (posted) {
        void *v = NULL;
        GoSelectCase cases[] = {{GO_SELECT_RECEIVE, d.reply, NULL, &v}, {GO_SELECT_RECEIVE, a->stop_ch, NULL, &v},
            {GO_SELECT_RECEIVE, s->dead_ch, NULL, &v}};
        selected = park(cases, 3, deadline);
        nsync_mu_lock(&s->mu);
        for (dial **q = &s->dials; *q; q = &(*q)->next) if (*q == &d) { *q = d.next; break; } /* withdrawn if unanswered */
        nsync_mu_unlock(&s->mu);
        /* An answer that raced the deadline still counts; stop does not. */
        if (selected == 1 || go_channel_is_closed(a->stop_ch)) rc = -ECANCELED;
        else if (d.answered) rc = d.rc;
        else rc = selected == 2 ? -ENODEV : -ETIMEDOUT; /* unanswered: out of reach, never a refusal */
        if (!rc) { *out = &d.ch->base; d.ch = NULL; }
        if (d.ch) chan_close(&d.ch->base);
    }
    go_channel_unref(d.reply);
    session_unref(s);
    return rc;
}
static bool b_local_addr(void *self, fipsd_ble_peer *out) { (void)self; (void)out; return false; } /* CoreBluetooth hides it */
static const char *b_adapter_name(void *self) { (void)self; return FIPSD_BLE_RADIO_ADAPTER; }
static const fipsd_ble_backend_ops agent_ops = {.start = b_start, .stop = b_stop, .destroy = b_destroy,
    .adapter = b_adapter, .listen = b_listen, .advertise = b_advertise, .advertise_stop = b_advertise_stop,
    .scan_start = b_scan_start, .scan_stop = b_scan_stop, .read_psm = NULL, .connect = b_connect,
    .local_addr = b_local_addr, .adapter_name = b_adapter_name};

bool fipsd_ble_agent_backend(const fipsd_ble_agent_config *cfg, fipsd_ble_backend *out)
{
    fipsd_ble_agent_config c = cfg ? *cfg : (fipsd_ble_agent_config){0};
    const char *path = c.socket_path && *c.socket_path ? c.socket_path : getenv(FIPSD_BLE_AGENT_SOCKET_ENV);
    if (!path || !*path) path = FIPSD_BLE_AGENT_SOCKET_PATH;
    agent *a = calloc(1, sizeof *a);
    if (!a) return false;
    if (strlen(path) >= sizeof a->path) { FIPSD_ERROR(MOD, "BLE agent socket path is too long: %s", path); free(a); return false; }
    strcpy(a->path, path);
    a->hello_timeout_ms = c.hello_timeout_ms ? c.hello_timeout_ms : FIPSD_BLE_AGENT_HELLO_TIMEOUT_MS;
    a->gid = getegid();
    if (!c.group || *c.group) {
        char err[512] = "";
        if (fipsd_privilege_resolve_group(c.group ? c.group : FIPSD_SOCKET_GROUP, &a->gid, err, sizeof err) < 0) {
            FIPSD_ERROR(MOD, "BLE agent socket group: %s", err); free(a); return false;
        }
    }
    a->stop_ch = go_channel_create(1); a->changed = go_channel_create(1);
    if (!a->stop_ch || !a->changed) {
        if (a->stop_ch) go_channel_unref(a->stop_ch);
        if (a->changed) go_channel_unref(a->changed);
        free(a); return false;
    }
    atomic_init(&a->refs, 1); atomic_init(&a->next_stream, 0);
    nsync_mu_init(&a->mu);
    a->listener = -1;
    *out = (fipsd_ble_backend){&agent_ops, a};
    return true;
}
fipsd_ble_agent_stats fipsd_ble_agent_stats_read(void *self)
{
    agent *a = self;
    nsync_mu_lock(&a->mu);
    fipsd_ble_agent_stats st = a->stats;
    st.live = a->live != NULL; st.psm = a->live ? a->live->psm : 0; st.channels = 0;
    if (a->live) {
        nsync_mu_lock(&a->live->mu);
        for (achan *c = a->live->chans; c; c = c->next) st.channels++;
        nsync_mu_unlock(&a->live->mu);
    }
    nsync_mu_unlock(&a->mu);
    return st;
}
GoChannel *fipsd_ble_agent_changed(void *self) { return ((agent *)self)->changed; }
const char *fipsd_ble_agent_socket(void *self) { return ((agent *)self)->path; }
