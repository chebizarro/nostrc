/* Upstream transport/ble/io_radio.rs, as a simulated radio: radios share an
 * in-process "air" and each runs its control plane on its own foreign thread,
 * exactly like a CoreBluetooth dispatch queue or a D-Bus loop. So it is also
 * the reference bridge for such backends:
 *   - a fiber-side operation hands a request to the control thread and parks
 *     on the reply with its deadline and the backend's stop channel; an
 *     abandoned request is reclaimed (InFlightDial) and a stream it would have
 *     produced is closed, never leaked;
 *   - the control thread reports through the sink only, and never runs
 *     transport code; an inbound channel is delivered outside the air lock;
 *   - bytes never cross the control thread (io_radio "bytes never cross the
 *     command surface"): they move through bounded per-direction channels, or
 *     a socketpair in fd mode, and a full channel backpressures the writer.
 * Adapter loss (power off, removal) fails parked operations with -ENODEV,
 * withdraws the radio's adverts (PEER_LOST at the scanners) and kills its
 * channels (STREAM_CLOSED at both ends). */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "transport/ble/internal.h"
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#define SIM_SEEN 64u
#define SIM_COMMANDS 256u
enum { CMD_QUIT, CMD_LISTEN, CMD_ADVERTISE, CMD_ADVERTISE_STOP, CMD_SCAN_START, CMD_SCAN_STOP, CMD_READ_PSM,
    CMD_CONNECT, CMD_ADAPTER, CMD_STALL, CMD_REFUSE, CMD_ASSIGN, CMD_DROP, CMD_INJECT };
typedef struct sim_req {
    _Atomic int refs;
    int kind;
    fipsd_ble_peer peer;
    uint16_t psm;
    bool a, b;
    uint8_t data[FIPSD_BLE_SERVICE_DATA_MAX];
    size_t len;
    GoChannel *reply; /* NULL: fire and forget */
    _Atomic bool abandoned;
    /* results, published to the caller by the reply channel */
    int rc;
    uint16_t bound;
    uint8_t value[FIPSD_BLE_GATT_VALUE_MAX];
    size_t value_len, count;
    fipsd_ble_stream *stream;
    struct sim_req *next; /* parked on the control thread (out of reach, stalled) */
} sim_req;
typedef struct sim_chunk { size_t len, off; uint8_t bytes[]; } sim_chunk;
typedef struct sim_channel sim_channel;
typedef struct sim_end {
    fipsd_ble_stream base; /* first */
    sim_channel *ch;
    int side; /* 0 dialler, 1 acceptor */
    GoChannel *cancel;
    sim_chunk *partial;
    int fd;
    uint16_t send_mtu, recv_mtu;
} sim_end;
struct sim_channel {
    sim_channel *next;
    fipsd_ble_air *air;
    fipsd_ble_radio *radio[2];
    sim_end *end[2];
    GoChannel *q[2], *gone[2], *dead; /* q[s] carries side s's writes; gone[s]: side s closed */
    int fd[2], open_ends;
    bool broken;
    fipsd_ble_radio_mode mode;
    size_t chunk;
};
struct fipsd_ble_air {
    nsync_mu mu;
    nsync_cv idle;
    GoChannel *changed; /* one-slot wake on every channel opened or freed */
    fipsd_ble_radio *radios;
    sim_channel *channels;
    uint64_t next_id;
};
struct fipsd_ble_radio {
    fipsd_ble_air *air;
    fipsd_ble_radio *next;
    fipsd_ble_radio_config cfg;
    char adapter[FIPSD_BLE_ADAPTER_MAX];
    fipsd_ble_peer self;
    pthread_t thread;
    bool thread_ok;
    GoChannel *commands, *stop_ch;
    /* air.mu */
    fipsd_ble_sink *sink;
    unsigned sink_users;
    bool stopped, present, powered, stall, refuse, listening, advertising, scanning;
    uint16_t assigned, bound_psm, advertised_psm;
    fipsd_ble_peer seen[SIM_SEEN];
    int32_t seen_psm[SIM_SEEN]; /* -1: legacy advert */
    size_t seen_count;
    sim_req *parked;
    fipsd_ble_radio_stats stats;
};
static uint64_t now_ms(void) { return ble_now(); }
static bool up(const fipsd_ble_radio *r) { return r->present && r->powered && !r->stopped; }

/* ---- requests ------------------------------------------------------------- */
static void stream_close(fipsd_ble_stream *s);
static void req_unref(sim_req *q)
{
    if (atomic_fetch_sub(&q->refs, 1) != 1) return;
    if (q->stream) stream_close(q->stream); /* answered after the caller gave up */
    if (q->reply) go_channel_unref(q->reply);
    free(q);
}
static sim_req *req_new(int kind, bool reply)
{
    sim_req *q = calloc(1, sizeof *q);
    if (!q) return NULL;
    q->kind = kind; atomic_init(&q->refs, reply ? 2 : 1);
    if (reply && !(q->reply = go_channel_create(1))) { free(q); return NULL; }
    return q;
}
/* Control thread, air.mu held: publish the result. */
static void publish(sim_req *q, int rc)
{
    q->rc = rc;
    if (q->reply) (void)go_channel_try_send(q->reply, q);
}
/* Publish and drop the control thread's reference, under air.mu: only for a
 * request that carries no stream. A caller that gave up leaves the last
 * reference here, and dropping it closes an answered stream, which takes
 * air.mu (process() drops a connect's reference after unlocking). */
static void answer(sim_req *q, int rc)
{
    publish(q, rc);
    req_unref(q);
}
static void answer_parked(fipsd_ble_radio *r, int rc)
{
    while (r->parked) { sim_req *q = r->parked; r->parked = q->next; answer(q, rc); }
}
static void reap_abandoned(fipsd_ble_radio *r)
{
    for (sim_req **p = &r->parked; *p;) {
        if (atomic_load(&(*p)->abandoned)) { sim_req *q = *p; *p = q->next; req_unref(q); }
        else p = &(*p)->next;
    }
}
/* Fiber (or any thread) side: hand over and park on the reply. */
static int call(fipsd_ble_radio *r, sim_req *q, uint64_t deadline)
{
    if (!q) return -ENOMEM;
    if (go_channel_is_closed(r->stop_ch) || go_channel_try_send(r->commands, q)) {
        int rc = go_channel_is_closed(r->stop_ch) ? -ECANCELED : -EAGAIN;
        req_unref(q); /* the control thread's reference; the caller drops its own */
        return rc;
    }
    void *v = NULL;
    GoSelectCase cases[] = {{GO_SELECT_RECEIVE, q->reply, NULL, &v}, {GO_SELECT_RECEIVE, r->stop_ch, NULL, &v}};
    int selected;
    if (deadline == FIPSD_BLE_NO_DEADLINE) selected = go_select(cases, 2);
    else {
        uint64_t now = now_ms();
        selected = go_select_timeout(cases, 2, deadline > now ? deadline - now : 0).selected_case;
    }
    int rc = selected == 0 ? q->rc : selected == 1 ? -ECANCELED : -ETIMEDOUT;
    if (selected != 0) atomic_store(&q->abandoned, true);
    return rc; /* the caller reads results, then req_unref */
}

/* ---- adverts ---------------------------------------------------------------- */
static size_t seen_at(const fipsd_ble_radio *s, const fipsd_ble_peer *p)
{
    for (size_t i = 0; i < s->seen_count; ++i) if (fipsd_ble_peer_equal(&s->seen[i], p)) return i;
    return SIZE_MAX;
}
/* BlueZ reports a device once per discovery session (CoreBluetooth: once per
 * interval), and a changed PSM at once. */
static void offer_advert(fipsd_ble_radio *s, fipsd_ble_radio *a)
{
    if (s == a || !s->scanning || !up(s) || !s->sink || !a->advertising || !up(a)) return;
    int32_t psm = a->cfg.legacy_advert ? -1 : a->advertised_psm;
    size_t i = seen_at(s, &a->self);
    if (i != SIZE_MAX && s->seen_psm[i] == psm) return;
    if (i == SIZE_MAX) {
        if (s->seen_count == SIM_SEEN) { memmove(s->seen, s->seen + 1, (SIM_SEEN - 1) * sizeof s->seen[0]);
            memmove(s->seen_psm, s->seen_psm + 1, (SIM_SEEN - 1) * sizeof s->seen_psm[0]); --s->seen_count; }
        i = s->seen_count++; s->seen[i] = a->self;
    }
    s->seen_psm[i] = psm;
    uint8_t sd[2]; int16_t rssi = -50;
    fipsd_ble_encode_psm(a->advertised_psm, sd);
    (void)fipsd_ble_sink_discovered(s->sink, &a->self, &rssi, psm < 0 ? NULL : sd, psm < 0 ? 0 : 2);
}
static void withdraw_advert(fipsd_ble_radio *s, fipsd_ble_radio *a)
{
    size_t i = seen_at(s, &a->self);
    if (i == SIZE_MAX) return;
    memmove(s->seen + i, s->seen + i + 1, (s->seen_count - i - 1) * sizeof s->seen[0]);
    memmove(s->seen_psm + i, s->seen_psm + i + 1, (s->seen_count - i - 1) * sizeof s->seen_psm[0]);
    --s->seen_count;
    if (s->sink && up(s)) (void)fipsd_ble_sink_lost(s->sink, &a->self);
}
static void advert_changed(fipsd_ble_air *air, fipsd_ble_radio *a)
{
    for (fipsd_ble_radio *s = air->radios; s; s = s->next)
        if (a->advertising && up(a)) offer_advert(s, a); else withdraw_advert(s, a);
}
static void scan_session(fipsd_ble_air *air, fipsd_ble_radio *s)
{ s->seen_count = 0; for (fipsd_ble_radio *a = air->radios; a; a = a->next) offer_advert(s, a); }
static fipsd_ble_radio *find(fipsd_ble_air *air, const fipsd_ble_peer *p)
{
    for (fipsd_ble_radio *r = air->radios; r; r = r->next) if (fipsd_ble_peer_equal(&r->self, p)) return r;
    return NULL;
}

/* ---- channels ----------------------------------------------------------------- */
static size_t break_channels(fipsd_ble_air *air, fipsd_ble_radio *r)
{
    size_t n = 0;
    for (sim_channel *c = air->channels; c; c = c->next) {
        if (c->broken || (c->radio[0] != r && c->radio[1] != r)) continue;
        c->broken = true; go_channel_close(c->dead); ++n;
        for (int s = 0; s < 2; ++s) {
            if (c->fd[s] >= 0) (void)shutdown(c->fd[s], SHUT_RDWR);
            if (c->end[s] && c->radio[s] && c->radio[s]->sink) (void)fipsd_ble_sink_closed(c->radio[s]->sink, c->end[s]->base.id);
        }
    }
    return n;
}
static ssize_t sim_read(fipsd_ble_stream *s, uint8_t *buf, size_t cap, uint64_t deadline)
{
    sim_end *e = (sim_end *)s; sim_channel *c = e->ch;
    GoChannel *in = c->q[!e->side];
    size_t got = 0;
    for (;;) {
        if (go_channel_is_closed(e->cancel)) return -ECANCELED;
        sim_chunk *k = e->partial; e->partial = NULL;
        void *v = NULL;
        if (!k && go_channel_try_receive(in, &v) == 0) k = v;
        if (!k && got) return (ssize_t)got; /* a byte stream returns what it has */
        if (!k) {
            if (go_channel_is_closed(c->dead)) return -ECONNRESET;
            GoSelectCase cases[] = {{GO_SELECT_RECEIVE, in, NULL, &v}, {GO_SELECT_RECEIVE, e->cancel, NULL, &v},
                {GO_SELECT_RECEIVE, c->dead, NULL, &v}};
            int selected;
            if (deadline == FIPSD_BLE_NO_DEADLINE) selected = go_select(cases, 3);
            else { uint64_t now = now_ms(); selected = go_select_timeout(cases, 3, deadline > now ? deadline - now : 0).selected_case; }
            if (selected < 0) return -ETIMEDOUT;
            if (selected == 1) return -ECANCELED;
            if (selected == 2) return -ECONNRESET;
            if (!v) return 0; /* the peer closed and everything it wrote was read */
            k = v;
        }
        size_t take = k->len - k->off < cap - got ? k->len - k->off : cap - got;
        memcpy(buf + got, k->bytes + k->off, take); got += take; k->off += take;
        if (c->mode == FIPSD_BLE_RADIO_SDU) { free(k); return (ssize_t)got; } /* one SDU per read; excess truncated */
        if (k->off < k->len) { e->partial = k; return (ssize_t)got; }
        free(k);
        if (got == cap) return (ssize_t)got;
    }
}
static int sim_write(fipsd_ble_stream *s, const uint8_t *p, size_t len, uint64_t deadline)
{
    sim_end *e = (sim_end *)s; sim_channel *c = e->ch;
    size_t piece = c->mode == FIPSD_BLE_RADIO_BYTES ? c->chunk : len;
    for (size_t at = 0; at < len;) {
        size_t n = len - at < piece ? len - at : piece;
        sim_chunk *k = malloc(sizeof *k + n);
        if (!k) return -ENOMEM;
        k->len = n; k->off = 0; memcpy(k->bytes, p + at, n);
        void *v = NULL;
        GoSelectCase cases[] = {{GO_SELECT_SEND, c->q[e->side], k, NULL}, {GO_SELECT_RECEIVE, e->cancel, NULL, &v},
            {GO_SELECT_RECEIVE, c->dead, NULL, &v}, {GO_SELECT_RECEIVE, c->gone[!e->side], NULL, &v}};
        int selected;
        /* select picks among ready cases at random: a dead or closed channel
         * must win over the free slot that is still there */
        if (go_channel_is_closed(e->cancel)) selected = 1;
        else if (go_channel_is_closed(c->dead) || go_channel_is_closed(c->gone[!e->side])) selected = 2;
        else if (deadline == FIPSD_BLE_NO_DEADLINE) selected = go_select(cases, 4);
        else { uint64_t now = now_ms(); selected = go_select_timeout(cases, 4, deadline > now ? deadline - now : 0).selected_case; }
        if (selected != 0) { free(k); return selected < 0 ? -ETIMEDOUT : selected == 1 ? -ECANCELED : -EPIPE; }
        at += n;
    }
    return 0;
}
static void sim_cancel(fipsd_ble_stream *s) { go_channel_close(((sim_end *)s)->cancel); }
static void sim_mtu(const fipsd_ble_stream *s, uint16_t *send, uint16_t *recv)
{ const sim_end *e = (const sim_end *)s; *send = e->send_mtu; *recv = e->recv_mtu; }
static int sim_fd(const fipsd_ble_stream *s) { return ((const sim_end *)s)->fd; }
static void channel_free(sim_channel *c)
{
    void *v;
    for (int s = 0; s < 2; ++s) {
        while (c->q[s] && go_channel_try_receive(c->q[s], &v) == 0) free(v);
        if (c->q[s]) go_channel_unref(c->q[s]);
        if (c->gone[s]) go_channel_unref(c->gone[s]);
    }
    if (c->dead) go_channel_unref(c->dead);
    free(c);
}
static void stream_close(fipsd_ble_stream *s)
{
    sim_end *e = (sim_end *)s; sim_channel *c = e->ch; fipsd_ble_air *air = c->air;
    bool last;
    nsync_mu_lock(&air->mu);
    go_channel_close(c->q[e->side]); go_channel_close(c->gone[e->side]);
    if (c->fd[e->side] >= 0) { close(c->fd[e->side]); c->fd[e->side] = -1; }
    c->end[e->side] = NULL;
    last = --c->open_ends == 0;
    if (last) for (sim_channel **p = &air->channels; *p; p = &(*p)->next) if (*p == c) { *p = c->next; break; }
    nsync_mu_unlock(&air->mu);
    if (last) (void)go_channel_try_send(air->changed, air);
    free(e->partial);
    go_channel_unref(e->cancel);
    free(e);
    if (last) channel_free(c);
}
static const fipsd_ble_stream_ops stream_ops = {.read = sim_read, .write = sim_write, .cancel = sim_cancel,
    .close = stream_close, .mtu = sim_mtu, .fd = sim_fd};
static int fd_pair(int fds[2])
{
#ifdef __linux__
    int type = SOCK_SEQPACKET; /* SDU-preserving, as BlueZ's L2CAP socket */
#else
    int type = SOCK_STREAM;    /* Apple has no AF_UNIX SEQPACKET: a byte stream, framed by the core */
#endif
    if (socketpair(AF_UNIX, type, 0, fds)) return -1;
    for (int i = 0; i < 2; ++i) {
        int on = 1; (void)on;
        if (fcntl(fds[i], F_SETFD, FD_CLOEXEC) || fcntl(fds[i], F_SETFL, O_NONBLOCK)) goto fail;
#ifdef SO_NOSIGPIPE
        if (setsockopt(fds[i], SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on)) goto fail;
#endif
    }
    return 0;
fail:
    close(fds[0]); close(fds[1]); return -1;
}
/* air.mu held. Returns the dialler's end; *inbound is the acceptor's. */
static fipsd_ble_stream *channel_new(fipsd_ble_air *air, fipsd_ble_radio *from, fipsd_ble_radio *to, fipsd_ble_stream **inbound)
{
    sim_channel *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->air = air; c->radio[0] = from; c->radio[1] = to; c->mode = from->cfg.mode; c->fd[0] = c->fd[1] = -1;
    c->chunk = from->cfg.chunk ? from->cfg.chunk : 7;
    size_t depth = from->cfg.queue ? from->cfg.queue : 32;
    for (int s = 0; s < 2; ++s) { c->q[s] = go_channel_create(depth); c->gone[s] = go_channel_create(1); }
    c->dead = go_channel_create(1);
    bool ok = c->q[0] && c->q[1] && c->gone[0] && c->gone[1] && c->dead;
    if (ok && c->mode == FIPSD_BLE_RADIO_FD) ok = fd_pair(c->fd) == 0;
    for (int s = 0; ok && s < 2; ++s) {
        sim_end *e = calloc(1, sizeof *e);
        if (!e || !(e->cancel = go_channel_create(1))) { free(e); ok = false; break; }
        fipsd_ble_radio *mine = s ? to : from, *theirs = s ? from : to;
        e->base = (fipsd_ble_stream){.ops = &stream_ops, .peer = theirs->self, .id = ++air->next_id};
        e->ch = c; e->side = s; e->fd = c->fd[s];
        e->send_mtu = mine->cfg.send_mtu; e->recv_mtu = mine->cfg.recv_mtu;
        c->end[s] = e; ++c->open_ends;
    }
    if (!ok) {
        for (int s = 0; s < 2; ++s) if (c->end[s]) { go_channel_unref(c->end[s]->cancel); free(c->end[s]); }
        for (int s = 0; s < 2; ++s) if (c->fd[s] >= 0) close(c->fd[s]);
        channel_free(c); return NULL;
    }
    c->next = air->channels; air->channels = c;
    from->stats.channels++; to->stats.channels++;
    (void)go_channel_try_send(air->changed, air);
    *inbound = &c->end[1]->base;
    return &c->end[0]->base;
}

/* ---- the control thread ------------------------------------------------------------ */
static void process(fipsd_ble_radio *r, sim_req *q)
{
    fipsd_ble_air *air = r->air;
    fipsd_ble_stream *inbound = NULL;
    fipsd_ble_radio *target = NULL;
    fipsd_ble_sink *target_sink = NULL;
    bool parked = false;
    int rc = 0;
    nsync_mu_lock(&air->mu);
    reap_abandoned(r);
    switch (q->kind) {
    case CMD_LISTEN:
        if (!up(r)) { rc = -ENODEV; break; }
        q->bound = r->assigned ? r->assigned : q->psm;
        r->listening = true; r->bound_psm = q->bound; r->stats.listens++; r->stats.bound_psm = q->bound;
        /* A listener opened under a live advert moves it onto the bound PSM. */
        if (r->advertising && r->advertised_psm != q->bound) {
            r->advertised_psm = q->bound; r->stats.advertise_restarts++; advert_changed(air, r);
        }
        break;
    case CMD_ADVERTISE:
        if (!up(r)) { rc = -ENODEV; break; }
        r->stats.advertise_calls++;
        if (r->advertising && r->advertised_psm == q->psm) break; /* the same PSM does not restart it */
        if (r->advertising) r->stats.advertise_restarts++;
        r->advertising = true; r->advertised_psm = q->psm;
        advert_changed(air, r);
        break;
    case CMD_ADVERTISE_STOP:
        if (r->advertising) { r->advertising = false; advert_changed(air, r); }
        break;
    case CMD_SCAN_START:
        if (!up(r)) { rc = -ENODEV; break; }
        r->scanning = true; r->stats.scan_starts++; scan_session(air, r);
        break;
    case CMD_SCAN_STOP:
        if (r->scanning) { r->scanning = false; r->seen_count = 0; }
        r->stats.scan_stops++;
        break;
    case CMD_READ_PSM:
        if (!r->cfg.read_gatt) { rc = -ENOTSUP; break; }
        if (!up(r)) { rc = -ENODEV; break; }
        target = find(air, &q->peer);
        if (r->stall || !target || !up(target)) { parked = true; break; } /* out of reach: the caller times out */
        if (!target->cfg.serve_gatt_psm) { rc = -ENOENT; break; }
        r->stats.gatt_reads++;
        fipsd_ble_encode_psm(target->listening ? target->bound_psm : 0, q->value); q->value_len = 2;
        break;
    case CMD_CONNECT:
        if (!up(r)) { rc = -ENODEV; break; }
        target = find(air, &q->peer);
        if (r->stall || !target || !up(target)) { parked = true; break; }
        if (target->refuse || !target->listening || target->bound_psm != q->psm || !target->sink) {
            r->stats.refused++; rc = -ECONNREFUSED; break;
        }
        q->stream = channel_new(air, r, target, &inbound);
        if (!q->stream) { rc = -ENOMEM; break; }
        r->stats.connects++; ++target->sink_users; target_sink = target->sink;
        break;
    case CMD_ADAPTER: {
        bool was = up(r);
        r->present = q->a; r->powered = q->b;
        if (was && !up(r)) {
            r->listening = false; r->bound_psm = 0; r->scanning = false; r->seen_count = 0;
            if (r->advertising) { r->advertising = false; advert_changed(air, r); }
            (void)break_channels(air, r);
            answer_parked(r, -ENODEV);
        }
        if (r->sink && !r->stopped) fipsd_ble_sink_adapter(r->sink, r->present, r->powered);
        break;
    }
    case CMD_STALL: r->stall = q->a; break;
    case CMD_REFUSE: r->refuse = q->a; break;
    case CMD_ASSIGN: r->assigned = q->psm; break;
    case CMD_DROP: q->count = break_channels(air, r); break;
    case CMD_INJECT:
        if (r->scanning && up(r) && r->sink) (void)fipsd_ble_sink_discovered(r->sink, &q->peer, NULL, q->data, q->len);
        break;
    default: break;
    }
    if (parked) { q->next = r->parked; r->parked = q; } /* parked: never holds a stream */
    else publish(q, rc);
    nsync_mu_unlock(&air->mu);
    if (!parked) req_unref(q);
    if (inbound) {
        /* Outside the lock: a sink that refuses closes the stream itself. */
        (void)fipsd_ble_sink_inbound(target_sink, inbound);
        nsync_mu_lock(&air->mu);
        if (!--target->sink_users) nsync_cv_broadcast(&air->idle);
        nsync_mu_unlock(&air->mu);
    }
}
static void *control(void *arg)
{
    fipsd_ble_radio *r = arg;
    for (;;) {
        void *v = NULL;
        if (go_channel_receive(r->commands, &v) != 0 || !v) break;
        sim_req *q = v;
        if (q->kind == CMD_QUIT) { req_unref(q); break; }
        process(r, q);
    }
    return NULL;
}

/* ---- the backend vtable --------------------------------------------------------------- */
static int b_start(void *self, struct fipsd_runtime *rt, fipsd_ble_sink *sink)
{
    (void)rt; fipsd_ble_radio *r = self;
    nsync_mu_lock(&r->air->mu); r->sink = sink; bool present = r->present, powered = r->powered; nsync_mu_unlock(&r->air->mu);
    /* The initial adapter state comes from the control thread, as on a real
     * platform where it arrives from the stack's own queue. */
    sim_req *q = req_new(CMD_ADAPTER, false);
    if (!q) return -1;
    q->a = present; q->b = powered;
    if (go_channel_try_send(r->commands, q)) { req_unref(q); return -1; }
    return 0;
}
static void b_stop(void *self)
{
    fipsd_ble_radio *r = self; fipsd_ble_air *air = r->air;
    nsync_mu_lock(&air->mu);
    if (!r->stopped) {
        r->stopped = true; r->listening = false;
        if (r->scanning) { r->scanning = false; r->seen_count = 0; }
        r->stats.scan_stops++;
        if (r->advertising) { r->advertising = false; advert_changed(air, r); }
        answer_parked(r, -ECANCELED);
    }
    nsync_mu_unlock(&air->mu);
    go_channel_close(r->stop_ch);
}
static fipsd_ble_adapter b_adapter(void *self)
{
    fipsd_ble_radio *r = self;
    nsync_mu_lock(&r->air->mu); fipsd_ble_adapter a = {r->present, r->powered}; nsync_mu_unlock(&r->air->mu);
    return a;
}
static int b_listen(void *self, uint16_t requested, uint64_t deadline, uint16_t *bound)
{
    sim_req *q = req_new(CMD_LISTEN, true);
    if (q) q->psm = requested;
    int rc = call(self, q, deadline);
    if (!rc) *bound = q->bound;
    if (q) req_unref(q);
    return rc;
}
static int simple(fipsd_ble_radio *r, int kind, uint16_t psm, uint64_t deadline)
{
    sim_req *q = req_new(kind, true);
    if (q) q->psm = psm;
    int rc = call(r, q, deadline);
    if (q) req_unref(q);
    return rc;
}
static int b_advertise(void *self, const fipsd_ble_advert *ad, uint64_t deadline) { return simple(self, CMD_ADVERTISE, ad->psm, deadline); }
static int b_advertise_stop(void *self) { return simple(self, CMD_ADVERTISE_STOP, 0, FIPSD_BLE_NO_DEADLINE); }
static int b_scan_start(void *self, uint64_t deadline) { return simple(self, CMD_SCAN_START, 0, deadline); }
static int b_scan_stop(void *self) { return simple(self, CMD_SCAN_STOP, 0, FIPSD_BLE_NO_DEADLINE); }
static int b_read_psm(void *self, const fipsd_ble_peer *p, uint64_t deadline, uint8_t *value, size_t cap, size_t *len)
{
    sim_req *q = req_new(CMD_READ_PSM, true);
    if (q) q->peer = *p;
    int rc = call(self, q, deadline);
    if (!rc) { *len = q->value_len < cap ? q->value_len : cap; memcpy(value, q->value, *len); }
    if (q) req_unref(q);
    return rc;
}
static int b_connect(void *self, const fipsd_ble_peer *p, uint16_t psm, uint64_t deadline, fipsd_ble_stream **out)
{
    sim_req *q = req_new(CMD_CONNECT, true);
    if (q) { q->peer = *p; q->psm = psm; }
    int rc = call(self, q, deadline);
    if (!rc) { *out = q->stream; q->stream = NULL; }
    if (q) req_unref(q);
    return rc;
}
static bool b_local_addr(void *self, fipsd_ble_peer *out) { *out = ((fipsd_ble_radio *)self)->self; return true; }
static const char *b_adapter_name(void *self) { return ((fipsd_ble_radio *)self)->adapter; }
static void b_destroy(void *self);
static const fipsd_ble_backend_ops radio_ops = {.start = b_start, .stop = b_stop, .destroy = b_destroy,
    .adapter = b_adapter, .listen = b_listen, .advertise = b_advertise, .advertise_stop = b_advertise_stop,
    .scan_start = b_scan_start, .scan_stop = b_scan_stop, .read_psm = b_read_psm, .connect = b_connect,
    .local_addr = b_local_addr, .adapter_name = b_adapter_name};

/* ---- construction and controls ---------------------------------------------------------- */
fipsd_ble_air *fipsd_ble_air_create(void)
{
    fipsd_ble_air *a = calloc(1, sizeof *a);
    if (a) { nsync_mu_init(&a->mu); nsync_cv_init(&a->idle); a->changed = go_channel_create(1); }
    if (a && !a->changed) { free(a); a = NULL; }
    return a;
}
void fipsd_ble_air_destroy(fipsd_ble_air *a)
{
    if (!a) return;
    go_channel_unref(a->changed);
    free(a); /* every channel was closed by its ends first */
}
fipsd_ble_radio *fipsd_ble_radio_create(fipsd_ble_air *air, const fipsd_ble_radio_config *cfg)
{
    const char *adapter = cfg->adapter ? cfg->adapter : FIPSD_BLE_RADIO_ADAPTER;
    if (!air || strlen(adapter) >= FIPSD_BLE_ADAPTER_MAX || !*adapter) return NULL;
    fipsd_ble_radio *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->air = air; r->cfg = *cfg; strcpy(r->adapter, adapter); r->cfg.adapter = r->adapter;
    strcpy(r->self.adapter, adapter); memcpy(r->self.device, cfg->device, 6);
    r->present = !cfg->absent; r->powered = !cfg->unpowered; r->assigned = cfg->assigned_psm;
    r->commands = go_channel_create(SIM_COMMANDS); r->stop_ch = go_channel_create(1);
    if (!r->commands || !r->stop_ch || pthread_create(&r->thread, NULL, control, r)) {
        if (r->commands) go_channel_unref(r->commands);
        if (r->stop_ch) go_channel_unref(r->stop_ch);
        free(r); return NULL;
    }
    r->thread_ok = true;
    nsync_mu_lock(&air->mu); r->next = air->radios; air->radios = r; nsync_mu_unlock(&air->mu);
    return r;
}
fipsd_ble_backend fipsd_ble_radio_backend(fipsd_ble_radio *r) { return (fipsd_ble_backend){&radio_ops, r}; }
static void b_destroy(void *self)
{
    fipsd_ble_radio *r = self; fipsd_ble_air *air = r->air;
    b_stop(r);
    nsync_mu_lock(&air->mu);
    for (fipsd_ble_radio **p = &air->radios; *p; p = &(*p)->next) if (*p == r) { *p = r->next; break; }
    for (sim_channel *c = air->channels; c; c = c->next) for (int s = 0; s < 2; ++s) if (c->radio[s] == r) c->radio[s] = NULL;
    while (r->sink_users) nsync_cv_wait(&air->idle, &air->mu); /* another radio delivering to our sink */
    r->sink = NULL;
    nsync_mu_unlock(&air->mu);
    sim_req *quit = req_new(CMD_QUIT, false);
    if (quit && go_channel_send(r->commands, quit)) req_unref(quit);
    if (!quit) go_channel_close(r->commands);
    (void)pthread_join(r->thread, NULL);
    void *v;
    while (go_channel_try_receive(r->commands, &v) == 0) if (v) req_unref(v);
    nsync_mu_lock(&air->mu); answer_parked(r, -ECANCELED); nsync_mu_unlock(&air->mu);
    go_channel_unref(r->commands); go_channel_unref(r->stop_ch);
    free(r);
}
static void post(fipsd_ble_radio *r, sim_req *q) { if (q && go_channel_try_send(r->commands, q)) req_unref(q); }
void fipsd_ble_radio_set_adapter(fipsd_ble_radio *r, bool present, bool powered)
{ sim_req *q = req_new(CMD_ADAPTER, false); if (q) { q->a = present; q->b = powered; } post(r, q); }
void fipsd_ble_radio_set_stall(fipsd_ble_radio *r, bool stall)
{ sim_req *q = req_new(CMD_STALL, false); if (q) q->a = stall; post(r, q); }
void fipsd_ble_radio_set_refuse(fipsd_ble_radio *r, bool refuse)
{ sim_req *q = req_new(CMD_REFUSE, false); if (q) q->a = refuse; post(r, q); }
void fipsd_ble_radio_set_assigned_psm(fipsd_ble_radio *r, uint16_t psm)
{ sim_req *q = req_new(CMD_ASSIGN, false); if (q) q->psm = psm; post(r, q); }
size_t fipsd_ble_radio_drop_links(fipsd_ble_radio *r)
{
    sim_req *q = req_new(CMD_DROP, true);
    size_t n = call(r, q, FIPSD_BLE_NO_DEADLINE) ? 0 : q->count;
    if (q) req_unref(q);
    return n;
}
void fipsd_ble_radio_inject_advert(fipsd_ble_radio *r, const fipsd_ble_peer *p, const uint8_t *data, size_t len)
{
    sim_req *q = req_new(CMD_INJECT, false);
    if (q) { q->peer = *p; q->len = len < sizeof q->data ? len : sizeof q->data; if (data) memcpy(q->data, data, q->len); }
    post(r, q);
}
GoChannel *fipsd_ble_air_changed(fipsd_ble_air *a) { return a->changed; }
size_t fipsd_ble_radio_channels(fipsd_ble_radio *r)
{
    size_t n = 0;
    nsync_mu_lock(&r->air->mu);
    for (sim_channel *c = r->air->channels; c; c = c->next) n += c->radio[0] == r || c->radio[1] == r;
    nsync_mu_unlock(&r->air->mu);
    return n;
}
fipsd_ble_radio_stats fipsd_ble_radio_stats_read(fipsd_ble_radio *r)
{
    nsync_mu_lock(&r->air->mu);
    fipsd_ble_radio_stats s = r->stats;
    s.advertised_psm = r->advertising ? r->advertised_psm : 0; s.bound_psm = r->listening ? r->bound_psm : 0;
    s.advertising = r->advertising; s.scanning = r->scanning;
    nsync_mu_unlock(&r->air->mu);
    return s;
}
