/* The agent's daemon session (upstream io_macos/agent.rs); see session.h.
 * Threads: the run thread reads the daemon's frames and acts on them; one
 * writer thread per session sends the queued frames, control first (channel
 * openings and closings, dial results, credit: never refused) and then data
 * (channel bytes, which wait for room, and adverts, which are dropped when the
 * queue is full: they repeat); and two pumps per channel move its bytes
 * between the radio and the session. Lock order: agent, then session. */
#if defined(__linux__)
#define _GNU_SOURCE /* struct ucred */
#else
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#endif
#include "session.h"
#include "transport/ble/agent_proto.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define DATA_QUEUE 256u /* upstream DATA_QUEUE: channel bytes and adverts towards the daemon */
/* Bytes queued on one channel beyond which the daemon overran its credit:
 * it may spend its last credit on a packet larger than what remained. */
#define CHANNEL_OVERRUN ((size_t)FIPSD_BLE_AGENT_CHANNEL_WINDOW + FIPSD_BLE_AGENT_MAX_FRAME)
#define PENDING_DIALS 64u
enum { OUT_ENDED, OUT_BUSY, OUT_RADIO };

typedef struct qframe { struct qframe *next; size_t len; uint8_t bytes[]; } qframe;
typedef struct obuf { struct obuf *next; size_t len; uint8_t bytes[]; } obuf;
typedef struct session session;
typedef struct chan {
    _Atomic int refs; /* the session's map, the reader pump, the writer pump */
    session *s;
    uint32_t ch;
    fba_channel *rc;
    uint16_t mtu;
    pthread_cond_t cv; /* the writer pump, with s->mu */
    /* s->mu */
    struct chan *next;
    bool listed, closing; /* closing: no CLOSED from its reader (the daemon or the session ended it) */
    obuf *head, **tail;
    size_t queued;
} chan;
struct session {
    _Atomic int refs; /* the serving thread and each channel */
    fba_agent *a;
    int fd;
    uint16_t psm;
    pthread_t writer;
    bool writer_ok;
    pthread_mutex_t mu;
    pthread_cond_t cv; /* the writer waiting for frames, producers waiting for room */
    qframe *ctl, **ctl_tail, *data, **data_tail;
    size_t data_count;
    bool ended;
    int outcome;
    chan *chans;
    uint32_t next_ch;
    struct { uint64_t token; int64_t id; } dials[PENDING_DIALS]; /* the radio knows a dial by its token */
    size_t ndials;
};
struct fba_agent {
    fba_agent_config cfg; /* cfg.socket_path is path below, the agent's own copy */
    char *path;
    const fba_radio_ops *ops;
    void *radio;
    pthread_mutex_t mu;
    pthread_cond_t cv; /* radio state, stop, and the last session/channel going away */
    bool stopping, up;
    uint16_t psm;
    session *s;
    size_t sessions, channels; /* alive, for destroy */
    uint64_t next_token;
    fba_agent_stats stats;
};

static void logf_(fba_agent *a, int level, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void logf_(fba_agent *a, int level, const char *fmt, ...)
{
    if (!a->cfg.log) return;
    char msg[512];
    va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    a->cfg.log(a->cfg.log_ctx, level, msg);
}

/* ---- frames ------------------------------------------------------------------- */
static qframe *qnew(const fipsd_ble_agent_frame *f)
{
    size_t n = fipsd_ble_agent_encoded_len(f);
    qframe *q = n ? malloc(sizeof *q + n) : NULL;
    if (q) { q->next = NULL; q->len = fipsd_ble_agent_encode(f, q->bytes, n); }
    return q;
}
static void post_ctl_locked(session *s, const fipsd_ble_agent_frame *f)
{
    qframe *q = s->ended ? NULL : qnew(f);
    if (!q) return;
    *s->ctl_tail = q; s->ctl_tail = &q->next;
    pthread_cond_broadcast(&s->cv);
}
static void post_ctl(session *s, const fipsd_ble_agent_frame *f)
{ pthread_mutex_lock(&s->mu); post_ctl_locked(s, f); pthread_mutex_unlock(&s->mu); }
static void append_data_locked(session *s, qframe *q)
{
    *s->data_tail = q; s->data_tail = &q->next; s->data_count++;
    pthread_cond_broadcast(&s->cv);
}
static bool send_all(int fd, const uint8_t *p, size_t n)
{
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL;
#endif
    while (n) {
        ssize_t k = send(fd, p, n, flags);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return false;
        p += k; n -= (size_t)k;
    }
    return true;
}
/* 0, 1 for a clean end before the first byte, -1 otherwise. */
static int recv_all(int fd, uint8_t *p, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t k = recv(fd, p + got, n - got, 0);
        if (k < 0 && errno == EINTR) continue;
        if (k < 0) return -1;
        if (!k) return got ? -1 : 1;
        got += (size_t)k;
    }
    return 0;
}

/* ---- references ----------------------------------------------------------------- */
static void session_unref(session *s)
{
    if (atomic_fetch_sub(&s->refs, 1) != 1) return;
    fba_agent *a = s->a;
    for (qframe *q = s->ctl, *n; q; q = n) { n = q->next; free(q); }
    for (qframe *q = s->data, *n; q; q = n) { n = q->next; free(q); }
    pthread_cond_destroy(&s->cv); pthread_mutex_destroy(&s->mu);
    free(s);
    pthread_mutex_lock(&a->mu); a->sessions--; pthread_cond_broadcast(&a->cv); pthread_mutex_unlock(&a->mu);
}
static void chan_unref(chan *c)
{
    if (atomic_fetch_sub(&c->refs, 1) != 1) return;
    session *s = c->s; fba_agent *a = s->a;
    a->ops->release(c->rc);
    for (obuf *b = c->head, *n; b; b = n) { n = b->next; free(b); }
    pthread_cond_destroy(&c->cv);
    free(c);
    pthread_mutex_lock(&a->mu); a->channels--; pthread_cond_broadcast(&a->cv); pthread_mutex_unlock(&a->mu);
    session_unref(s);
}
/* The live session, referenced, or NULL. */
static session *live(fba_agent *a)
{
    pthread_mutex_lock(&a->mu);
    session *s = a->s;
    if (s) atomic_fetch_add(&s->refs, 1);
    pthread_mutex_unlock(&a->mu);
    return s;
}
static void unlist_locked(session *s, chan *c)
{
    for (chan **p = &s->chans; *p; p = &(*p)->next) if (*p == c) { *p = c->next; break; }
    c->listed = false;
}
static chan *find_locked(session *s, uint32_t ch)
{
    for (chan *c = s->chans; c; c = c->next) if (c->ch == ch) return c;
    return NULL;
}
/* End a listed channel this side (the daemon's CLOSE, an overrun, the
 * session ending): no CLOSED from its reader. Drops the map's reference. */
static void end_listed(session *s, chan *c, bool report_closed)
{
    pthread_mutex_lock(&s->mu);
    bool mine = c->listed;
    if (mine) {
        unlist_locked(s, c); c->closing = true;
        if (report_closed) { fipsd_ble_agent_frame f = {.tag = FIPSD_BLE_AGENT_CLOSED, .ch = c->ch}; post_ctl_locked(s, &f); }
        pthread_cond_broadcast(&c->cv); pthread_cond_broadcast(&s->cv); /* its pumps, whichever wait */
    }
    pthread_mutex_unlock(&s->mu);
    if (!mine) return;
    s->a->ops->close(c->rc);
    chan_unref(c);
}

/* ---- channel pumps ----------------------------------------------------------------- */
static void *reader_pump(void *arg)
{
    chan *c = arg; session *s = c->s; fba_agent *a = s->a;
    size_t cap = c->mtu > 64 ? c->mtu : 64;
    uint8_t *buf = malloc(cap);
    for (ssize_t n; buf && (n = a->ops->read(c->rc, buf, cap)) > 0;) {
        fipsd_ble_agent_frame f = {.tag = FIPSD_BLE_AGENT_RECV, .ch = c->ch, .data = buf, .len = (size_t)n};
        qframe *q = qnew(&f);
        if (!q) break;
        pthread_mutex_lock(&s->mu);
        while (s->data_count >= DATA_QUEUE && !s->ended && !c->closing) pthread_cond_wait(&s->cv, &s->mu);
        bool gone = s->ended || c->closing;
        if (!gone) append_data_locked(s, q);
        pthread_mutex_unlock(&s->mu);
        if (gone) { free(q); break; }
    }
    free(buf);
    /* The channel ended on the radio side: CLOSED behind the bytes read before
     * it, on the data queue (on the control queue it would overtake them). */
    pthread_mutex_lock(&s->mu);
    bool report = c->listed && !c->closing && !s->ended;
    if (report) {
        fipsd_ble_agent_frame f = {.tag = FIPSD_BLE_AGENT_CLOSED, .ch = c->ch};
        qframe *q = qnew(&f);
        if (q) append_data_locked(s, q);
        unlist_locked(s, c); c->closing = true;
    }
    pthread_cond_broadcast(&c->cv);
    pthread_mutex_unlock(&s->mu);
    a->ops->close(c->rc);
    if (report) chan_unref(c); /* the map's */
    chan_unref(c);
    return NULL;
}
static bool write_all(fba_agent *a, fba_channel *rc, const uint8_t *p, size_t n)
{
    while (n) {
        ssize_t k = a->ops->write(rc, p, n);
        if (k <= 0) return false;
        p += k; n -= (size_t)k;
    }
    return true;
}
static void *writer_pump(void *arg)
{
    chan *c = arg; session *s = c->s; fba_agent *a = s->a;
    for (;;) {
        pthread_mutex_lock(&s->mu);
        while (!c->head && !c->closing) pthread_cond_wait(&c->cv, &s->mu);
        obuf *b = c->closing ? NULL : c->head; /* a closed channel's queued bytes are dropped, as upstream */
        if (b && !(c->head = b->next)) c->tail = &c->head;
        pthread_mutex_unlock(&s->mu);
        if (!b) break;
        bool ok = write_all(a, c->rc, b->bytes, b->len);
        pthread_mutex_lock(&s->mu);
        c->queued -= b->len;
        /* The radio has the bytes: the daemon may send as many more. */
        if (ok && b->len) { fipsd_ble_agent_frame f = {.tag = FIPSD_BLE_AGENT_CREDIT, .ch = c->ch, .bytes = (uint32_t)b->len}; post_ctl_locked(s, &f); }
        pthread_mutex_unlock(&s->mu);
        free(b);
        if (!ok) { a->ops->close(c->rc); break; } /* the reader sees the end and reports it */
    }
    chan_unref(c);
    return NULL;
}
static bool spawn(void *(*fn)(void *), void *arg)
{
    pthread_t t;
    if (pthread_create(&t, NULL, fn, arg)) return false;
    (void)pthread_detach(t);
    return true;
}
/* A channel opened on the radio: announce it (and grant its window) before
 * its reader starts, so the daemon knows it by the time its bytes arrive. */
static void open_channel(session *s, fba_channel *rc, const uint8_t device[6], uint16_t mtu, bool dialled, int64_t id)
{
    fba_agent *a = s->a;
    chan *c = calloc(1, sizeof *c);
    if (c && pthread_cond_init(&c->cv, NULL)) { free(c); c = NULL; }
    pthread_mutex_lock(&s->mu);
    if (!c || s->ended) {
        if (dialled && !s->ended) {
            fipsd_ble_agent_frame f = {.tag = FIPSD_BLE_AGENT_CONNECT_RESULT, .id = id};
            memcpy(f.addr, device, 6); post_ctl_locked(s, &f);
        }
        pthread_mutex_unlock(&s->mu);
        if (c) { pthread_cond_destroy(&c->cv); free(c); }
        a->ops->close(rc); a->ops->release(rc);
        return;
    }
    do { s->next_ch = s->next_ch + 1 ? s->next_ch + 1 : 1; } while (find_locked(s, s->next_ch));
    c->s = s; c->ch = s->next_ch; c->rc = rc; c->mtu = mtu; c->tail = &c->head;
    atomic_init(&c->refs, 3);
    c->listed = true; c->next = s->chans; s->chans = c;
    atomic_fetch_add(&s->refs, 1);
    fipsd_ble_agent_frame f = {.tag = dialled ? FIPSD_BLE_AGENT_CONNECT_RESULT : FIPSD_BLE_AGENT_INBOUND,
        .id = id, .ok = true, .ch = c->ch, .mtu = mtu};
    memcpy(f.addr, device, 6);
    post_ctl_locked(s, &f);
    fipsd_ble_agent_frame credit = {.tag = FIPSD_BLE_AGENT_CREDIT, .ch = c->ch, .bytes = FIPSD_BLE_AGENT_CHANNEL_WINDOW};
    post_ctl_locked(s, &credit);
    pthread_mutex_unlock(&s->mu);
    pthread_mutex_lock(&a->mu); a->channels++; a->stats.channels_opened++; pthread_mutex_unlock(&a->mu);
    bool reader = spawn(reader_pump, c), writer = spawn(writer_pump, c);
    if (reader && writer) return;
    logf_(a, FBA_LOG_WARN, "BLE agent: could not start the pumps of channel %u", c->ch);
    end_listed(s, c, true);
    if (!reader) chan_unref(c);
    if (!writer) chan_unref(c);
}

/* ---- radio reports ------------------------------------------------------------- */
void fba_agent_radio(fba_agent *a, bool up, uint16_t psm)
{
    pthread_mutex_lock(&a->mu);
    a->up = up; a->psm = psm;
    session *s = a->s;
    /* A session lasts only while the radio is up on the PSM its hello
     * reported: the daemon must never hold a radio that is not there. */
    if (s && (!up || psm != s->psm)) {
        pthread_mutex_lock(&s->mu); s->outcome = OUT_RADIO; pthread_mutex_unlock(&s->mu);
        (void)shutdown(s->fd, SHUT_RDWR);
    }
    pthread_cond_broadcast(&a->cv);
    pthread_mutex_unlock(&a->mu);
}
void fba_agent_inbound(fba_agent *a, fba_channel *rc, const uint8_t device[6], uint16_t mtu)
{
    session *s = live(a);
    if (!s) { a->ops->close(rc); a->ops->release(rc); return; } /* stale: no daemon to take it */
    open_channel(s, rc, device, mtu, false, 0);
    session_unref(s);
}
void fba_agent_dialled(fba_agent *a, int64_t token, const uint8_t device[6], fba_channel *rc, uint16_t mtu)
{
    session *s = live(a);
    bool pending = false;
    int64_t id = 0;
    if (s) {
        pthread_mutex_lock(&s->mu);
        for (size_t i = 0; i < s->ndials; ++i)
            if (s->dials[i].token == (uint64_t)token) { id = s->dials[i].id; s->dials[i] = s->dials[--s->ndials]; pending = true; break; }
        pthread_mutex_unlock(&s->mu);
    }
    /* A dial of an earlier session has nobody left to answer. */
    if (!pending) { if (rc) { a->ops->close(rc); a->ops->release(rc); } }
    else if (!rc) {
        fipsd_ble_agent_frame f = {.tag = FIPSD_BLE_AGENT_CONNECT_RESULT, .id = id};
        memcpy(f.addr, device, 6); post_ctl(s, &f);
    } else open_channel(s, rc, device, mtu, true, id);
    if (s) session_unref(s);
}
void fba_agent_advert(fba_agent *a, const uint8_t device[6], uint16_t psm, int16_t rssi)
{
    session *s = live(a);
    if (!s) return;
    fipsd_ble_agent_frame f = {.tag = FIPSD_BLE_AGENT_SCAN, .psm = psm, .rssi = rssi};
    memcpy(f.addr, device, 6);
    qframe *q = qnew(&f);
    pthread_mutex_lock(&s->mu);
    /* Adverts repeat; one dropped while the daemon is behind is heard again. */
    if (q && !s->ended && s->data_count < DATA_QUEUE) { append_data_locked(s, q); q = NULL; }
    pthread_mutex_unlock(&s->mu);
    free(q);
    session_unref(s);
}

/* ---- the session ----------------------------------------------------------------- */
static void *writer_thread(void *arg)
{
    session *s = arg;
    for (;;) {
        pthread_mutex_lock(&s->mu);
        while (!s->ctl && !s->data && !s->ended) pthread_cond_wait(&s->cv, &s->mu);
        qframe *q = NULL;
        if (!s->ended) {
            if ((q = s->ctl)) { if (!(s->ctl = q->next)) s->ctl_tail = &s->ctl; }
            else if ((q = s->data)) {
                if (!(s->data = q->next)) s->data_tail = &s->data;
                s->data_count--; pthread_cond_broadcast(&s->cv);
            }
        }
        pthread_mutex_unlock(&s->mu);
        if (!q) break;
        bool ok = send_all(s->fd, q->bytes, q->len);
        free(q);
        if (!ok) { (void)shutdown(s->fd, SHUT_RDWR); break; } /* the reading thread ends the session */
    }
    return NULL;
}
static void command(session *s, const fipsd_ble_agent_frame *f)
{
    fba_agent *a = s->a;
    switch (f->tag) {
    case FIPSD_BLE_AGENT_CONNECT: {
        pthread_mutex_lock(&a->mu); uint64_t token = ++a->next_token; pthread_mutex_unlock(&a->mu);
        pthread_mutex_lock(&s->mu);
        bool room = s->ndials < PENDING_DIALS;
        if (room) { s->dials[s->ndials].token = token; s->dials[s->ndials++].id = f->id; }
        else { fipsd_ble_agent_frame no = {.tag = FIPSD_BLE_AGENT_CONNECT_RESULT, .id = f->id}; memcpy(no.addr, f->addr, 6); post_ctl_locked(s, &no); }
        pthread_mutex_unlock(&s->mu);
        if (room) a->ops->connect(a->radio, (int64_t)token, f->addr, f->psm);
        break;
    }
    case FIPSD_BLE_AGENT_START_ADVERTISING: a->ops->start_advertising(a->radio, f->psm); break;
    case FIPSD_BLE_AGENT_STOP_ADVERTISING: a->ops->stop_advertising(a->radio); break;
    case FIPSD_BLE_AGENT_START_SCANNING: a->ops->start_scanning(a->radio); break;
    case FIPSD_BLE_AGENT_STOP_SCANNING: a->ops->stop_scanning(a->radio); break;
    case FIPSD_BLE_AGENT_SEND: {
        obuf *b = malloc(sizeof *b + f->len);
        pthread_mutex_lock(&s->mu);
        chan *c = find_locked(s, f->ch);
        bool overrun = false;
        if (c && !c->closing) {
            c->queued += f->len;
            overrun = !b || c->queued > CHANNEL_OVERRUN; /* no memory: closed rather than a hole in its bytes */
            if (!overrun) {
                b->next = NULL; b->len = f->len; memcpy(b->bytes, f->data, f->len);
                *c->tail = b; c->tail = &b->next; b = NULL;
                pthread_cond_broadcast(&c->cv);
            }
        }
        if (overrun) atomic_fetch_add(&c->refs, 1);
        pthread_mutex_unlock(&s->mu);
        free(b);
        if (overrun) {
            logf_(a, FBA_LOG_WARN, "BLE agent: daemon sent past its credit on channel %u; closing it", f->ch);
            pthread_mutex_lock(&a->mu); a->stats.overruns++; pthread_mutex_unlock(&a->mu);
            end_listed(s, c, true);
            chan_unref(c);
        }
        break;
    }
    case FIPSD_BLE_AGENT_CLOSE: {
        pthread_mutex_lock(&s->mu);
        chan *c = find_locked(s, f->ch);
        if (c) atomic_fetch_add(&c->refs, 1);
        pthread_mutex_unlock(&s->mu);
        if (c) { end_listed(s, c, false); chan_unref(c); }
        break;
    }
    default: logf_(a, FBA_LOG_DEBUG, "BLE agent: unexpected frame (tag %d)", (int)f->tag); break;
    }
}
static int serve(fba_agent *a, int fd, uint16_t psm)
{
    session *s = calloc(1, sizeof *s);
    uint8_t *buf = malloc(FIPSD_BLE_AGENT_MAX_FRAME);
    if (!s || !buf || pthread_mutex_init(&s->mu, NULL)) { free(s); free(buf); return OUT_ENDED; }
    if (pthread_cond_init(&s->cv, NULL)) { pthread_mutex_destroy(&s->mu); free(s); free(buf); return OUT_ENDED; }
    atomic_init(&s->refs, 1);
    s->a = a; s->fd = fd; s->psm = psm; s->ctl_tail = &s->ctl; s->data_tail = &s->data; s->outcome = OUT_ENDED;
    pthread_mutex_lock(&a->mu);
    a->s = s; a->sessions++; a->stats.sessions++;
    if (!a->up || a->psm != psm || a->stopping) s->outcome = OUT_RADIO; /* changed while connecting */
    pthread_mutex_unlock(&a->mu);
    fipsd_ble_agent_frame hello = {.tag = FIPSD_BLE_AGENT_HELLO, .version = FIPSD_BLE_AGENT_VERSION, .psm = psm};
    post_ctl(s, &hello);
    s->writer_ok = s->outcome == OUT_ENDED && !pthread_create(&s->writer, NULL, writer_thread, s);
    while (s->writer_ok) {
        uint8_t h[FIPSD_BLE_AGENT_HEADER];
        size_t len;
        fipsd_ble_agent_frame f;
        if (recv_all(fd, h, sizeof h) || !fipsd_ble_agent_frame_len(h, FIPSD_BLE_AGENT_MAX_FRAME, &len) ||
            recv_all(fd, buf, len) || !fipsd_ble_agent_decode(buf, len, &f)) break;
        if (f.tag == FIPSD_BLE_AGENT_BUSY) { pthread_mutex_lock(&s->mu); s->outcome = OUT_BUSY; pthread_mutex_unlock(&s->mu); break; }
        command(s, &f);
    }
    free(buf);
    /* The session ends: its channels with it; the radio stays the agent's. */
    pthread_mutex_lock(&a->mu); a->s = NULL; pthread_mutex_unlock(&a->mu);
    pthread_mutex_lock(&s->mu);
    s->ended = true;
    chan *gone = s->chans; s->chans = NULL;
    for (chan *c = gone; c; c = c->next) { c->listed = false; c->closing = true; pthread_cond_broadcast(&c->cv); }
    pthread_cond_broadcast(&s->cv);
    int outcome = s->outcome;
    pthread_mutex_unlock(&s->mu);
    for (chan *c = gone, *n; c; c = n) { n = c->next; a->ops->close(c->rc); chan_unref(c); }
    (void)shutdown(fd, SHUT_RDWR);
    if (s->writer_ok) (void)pthread_join(s->writer, NULL);
    a->ops->stop_advertising(a->radio);
    a->ops->stop_scanning(a->radio);
    session_unref(s);
    return outcome;
}

/* ---- the run loop ------------------------------------------------------------------ */
static int connect_to(const char *path)
{
    struct sockaddr_un addr;
    if (strlen(path) >= sizeof addr.sun_path) { errno = ENAMETOOLONG; return -1; }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    int on = 1; (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
    memset(&addr, 0, sizeof addr); addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, strlen(path) + 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr)) { int e = errno; close(fd); errno = e; return -1; }
    return fd;
}
static bool peer_uid(int fd, uid_t *uid)
{
#if defined(__linux__)
    struct ucred cred; socklen_t len = sizeof cred;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len)) return false;
    *uid = cred.uid;
    return true;
#else
    gid_t gid;
    return getpeereid(fd, uid, &gid) == 0;
#endif
}
static uint64_t now_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}
/* The reconnect delay: a timed wait on the agent's condition, ended early by stop. */
static void pause_ms(fba_agent *a, uint32_t ms)
{
    struct timespec until; clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += ms / 1000u; until.tv_nsec += (long)(ms % 1000u) * 1000000L;
    if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&a->mu);
    while (!a->stopping && pthread_cond_timedwait(&a->cv, &a->mu, &until) != ETIMEDOUT) {}
    pthread_mutex_unlock(&a->mu);
}
void fba_agent_run(fba_agent *a)
{
    const fba_agent_config *c = &a->cfg;
    uint32_t backoff = c->reconnect_ms;
    for (;;) {
        pthread_mutex_lock(&a->mu);
        /* No hello before the radio is up: it would hand the daemon a PSM nothing listens on. */
        while (!a->stopping && !a->up) pthread_cond_wait(&a->cv, &a->mu);
        bool stopping = a->stopping; uint16_t psm = a->psm;
        pthread_mutex_unlock(&a->mu);
        if (stopping) break;
        int fd = connect_to(c->socket_path);
        if (fd < 0) {
            logf_(a, FBA_LOG_DEBUG, "BLE agent: daemon not reachable at %s: %s", c->socket_path, strerror(errno));
            pause_ms(a, c->reconnect_ms);
            continue;
        }
        uid_t uid = 0;
        if (c->check_uid && (!peer_uid(fd, &uid) || uid != c->daemon_uid)) {
            /* The socket's directory is the daemon's, but anyone who got there
             * first would otherwise be handed the radio and every peer's bytes. */
            logf_(a, FBA_LOG_WARN, "BLE agent: %s is answered by uid %u, not the daemon (uid %u); not serving it",
                c->socket_path, (unsigned)uid, (unsigned)c->daemon_uid);
            close(fd);
            backoff = backoff * 2 < c->max_reconnect_ms ? backoff * 2 : c->max_reconnect_ms;
            pause_ms(a, backoff);
            continue;
        }
        logf_(a, FBA_LOG_INFO, "BLE agent: connected to the daemon at %s (psm 0x%04x)", c->socket_path, psm);
        uint64_t started = now_ms();
        int outcome = serve(a, fd, psm);
        close(fd);
        if (outcome == OUT_BUSY) {
            pthread_mutex_lock(&a->mu); a->stats.busy++; pthread_mutex_unlock(&a->mu);
            backoff = backoff * 2 < c->max_reconnect_ms ? backoff * 2 : c->max_reconnect_ms;
            logf_(a, FBA_LOG_INFO, "BLE agent: another agent lends the daemon its radio; standing by (retry in %u ms)", backoff);
        } else if (outcome == OUT_RADIO) {
            backoff = c->reconnect_ms;
            logf_(a, FBA_LOG_INFO, "BLE agent: radio went down or changed PSM; reconnecting");
        } else {
            /* A session the daemon cut short at once (another version, say)
             * backs off; one that ran a while is a daemon restart. */
            backoff = now_ms() - started >= c->stable_ms ? c->reconnect_ms
                : backoff * 2 < c->max_reconnect_ms ? backoff * 2 : c->max_reconnect_ms;
            logf_(a, FBA_LOG_INFO, "BLE agent: daemon gone");
        }
        pause_ms(a, backoff);
    }
}

fba_agent *fba_agent_create(const fba_agent_config *cfg, const fba_radio_ops *ops, void *radio)
{
    if (!cfg || !cfg->socket_path || !ops) return NULL;
    fba_agent *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    /* The run thread dials the socket until it returns, which may be after the caller's stop
     * and after whatever owned the caller's string is gone: the path is the agent's own. */
    a->path = strdup(cfg->socket_path);
    if (!a->path) { free(a); return NULL; }
    a->cfg = *cfg; a->cfg.socket_path = a->path; a->ops = ops; a->radio = radio;
    if (!a->cfg.reconnect_ms) a->cfg.reconnect_ms = 2000;
    if (!a->cfg.max_reconnect_ms) a->cfg.max_reconnect_ms = 60000;
    if (!a->cfg.stable_ms) a->cfg.stable_ms = 30000;
    if (pthread_mutex_init(&a->mu, NULL)) { free(a->path); free(a); return NULL; }
    if (pthread_cond_init(&a->cv, NULL)) { pthread_mutex_destroy(&a->mu); free(a->path); free(a); return NULL; }
    return a;
}
void fba_agent_stop(fba_agent *a)
{
    pthread_mutex_lock(&a->mu);
    a->stopping = true;
    if (a->s) (void)shutdown(a->s->fd, SHUT_RDWR);
    pthread_cond_broadcast(&a->cv);
    pthread_mutex_unlock(&a->mu);
}
void fba_agent_destroy(fba_agent *a)
{
    if (!a) return;
    pthread_mutex_lock(&a->mu);
    while (a->sessions || a->channels) pthread_cond_wait(&a->cv, &a->mu); /* pumps finishing */
    pthread_mutex_unlock(&a->mu);
    pthread_cond_destroy(&a->cv); pthread_mutex_destroy(&a->mu);
    free(a->path);
    free(a);
}
fba_agent_stats fba_agent_stats_read(fba_agent *a)
{
    pthread_mutex_lock(&a->mu);
    fba_agent_stats st = a->stats;
    st.connected = a->s != NULL; st.channels = 0;
    if (a->s) { pthread_mutex_lock(&a->s->mu); for (chan *c = a->s->chans; c; c = c->next) st.channels++; pthread_mutex_unlock(&a->s->mu); }
    pthread_mutex_unlock(&a->mu);
    return st;
}
