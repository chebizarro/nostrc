/* Upstream transport/ble/mod.rs: the platform-independent BLE transport.
 *
 * Fibers, all tracked by the runtime and ended by owner-directed
 * cancellation (u->stop, backend stop, per-stream cancel):
 *   actor   - backend events and adapter presence; spawns inbound admissions
 *   prober  - upstream scan_probe_loop: one probe at a time, retry book tick
 *   dial    - one per connect_async (the node's own dials)
 *   admit   - one per inbound channel, at most BLE_INBOUND_INFLIGHT
 *   owner, reader, writer - one set per pooled link, as transport/tcp
 * A stream belongs to exactly one fiber until it is pooled; the pool and every
 * list below are guarded by u->mu, which is never held across a park. */
#define _POSIX_C_SOURCE 200809L
#include "transport/ble/internal.h"
#if defined(FIPSD_ENABLE_BLE_BLUEZ)
#include "transport/ble/bluez.h"
#endif
#ifdef FIPSD_BLE_COREBLUETOOTH
#include "transport/ble/agent_client.h"
#endif
#include "node/context.h"
#include "log.h"
#include <fips/identity/local.h>
#include <fips/identity/node_addr.h>
#include <fips/identity/peer.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MOD "ble"
static const fipsd_transport_ops ops;

/* ---- link addresses -------------------------------------------------------- */
bool fipsd_ble_peer_equal(const fipsd_ble_peer *a, const fipsd_ble_peer *b)
{ return !strcmp(a->adapter, b->adapter) && !memcmp(a->device, b->device, 6); }
static int hex(char c)
{ return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
bool fipsd_ble_peer_parse(const char *s, size_t n, fipsd_ble_peer *out)
{
    const char *slash = s ? memchr(s, '/', n) : NULL;
    if (!slash) return false;
    size_t alen = (size_t)(slash - s);
    if (!alen || alen >= FIPSD_BLE_ADAPTER_MAX || memchr(s, 0, alen) || n - alen - 1 != 17) return false;
    fipsd_ble_peer p = {0};
    for (size_t i = 0; i < 6; ++i) {
        const char *x = slash + 1 + i * 3;
        int hi = hex(x[0]), lo = hex(x[1]);
        if (hi < 0 || lo < 0 || (i < 5 && x[2] != ':')) return false;
        p.device[i] = (uint8_t)(hi << 4 | lo);
    }
    memcpy(p.adapter, s, alen); *out = p;
    return true;
}
size_t fipsd_ble_peer_format(const fipsd_ble_peer *p, char *out, size_t cap)
{
    int n = snprintf(out, cap, "%s/%02X:%02X:%02X:%02X:%02X:%02X", p->adapter, p->device[0], p->device[1],
        p->device[2], p->device[3], p->device[4], p->device[5]);
    return n < 0 || (size_t)n >= cap ? 0 : (size_t)n;
}
bool fipsd_ble_peer_to_addr(const fipsd_ble_peer *p, fips_transport_addr *out)
{
    char text[FIPSD_BLE_ADAPTER_MAX + 20];
    size_t n = fipsd_ble_peer_format(p, text, sizeof text);
    return n && fips_transport_addr_from_string(text, n, out) == FIPS_OK;
}
bool fipsd_ble_peer_from_addr(const fips_transport_addr *a, fipsd_ble_peer *out)
{ return a && a->len <= FIPS_TRANSPORT_ADDR_CAPACITY && fipsd_ble_peer_parse((const char *)a->bytes, a->len, out); }
bool ble_challenger_addr(const fipsd_ble_peer *p, uint64_t id, fips_transport_addr *out)
{
    char text[FIPSD_BLE_ADAPTER_MAX + 48];
    size_t n = fipsd_ble_peer_format(p, text, sizeof text);
    int m = n ? snprintf(text + n, sizeof text - n, "#%llu", (unsigned long long)id) : -1;
    return m > 0 && (size_t)m < sizeof text - n && fips_transport_addr_from_string(text, n + (size_t)m, out) == FIPS_OK;
}
fipsd_ble_peer fipsd_ble_peer_from_identifier(const uint8_t id[16])
{
    uint64_t h = UINT64_C(0xcbf29ce484222325);
    for (size_t i = 0; i < 16; ++i) h = (h ^ id[i]) * UINT64_C(0x100000001b3);
    fipsd_ble_peer p = {FIPSD_BLE_RADIO_ADAPTER, {0}};
    for (size_t i = 0; i < 6; ++i) p.device[i] = (uint8_t)(h >> (56 - 8 * i));
    p.device[0] = (uint8_t)((p.device[0] | 0x02) & ~0x01);
    return p;
}

/* ---- the sink ------------------------------------------------------------- */
static bool adapter_up(unsigned v) { return (v & 7u) == 7u; }
void fipsd_ble_sink_adapter(fipsd_ble_sink *s, bool present, bool powered)
{
    /* Coalesced to the latest value, but every up/down transition is counted,
     * so an off-on blip the core did not see in between still reads as one. */
    unsigned old = atomic_load(&s->adapter), next;
    do {
        next = (present ? 1u : 0u) | (powered ? 2u : 0u) | 4u;
        unsigned count = old >> 3;
        if (adapter_up(next) != adapter_up(old) || !(old & 4u)) ++count;
        next |= count << 3;
    } while (!atomic_compare_exchange_weak(&s->adapter, &old, next));
    if (s->adapter_wake) (void)go_channel_try_send(s->adapter_wake, s);
}
static bool push(fipsd_ble_sink *s, fipsd_ble_event *e)
{
    if (e && atomic_load(&s->open) && s->events && !go_channel_try_send(s->events, e)) return true;
    atomic_fetch_add(&s->dropped, 1);
    return false;
}
static fipsd_ble_event *event(fipsd_ble_event_kind kind, const fipsd_ble_peer *peer)
{
    fipsd_ble_event *e = calloc(1, sizeof *e);
    if (e) { e->kind = kind; if (peer) e->peer = *peer; }
    return e;
}
bool fipsd_ble_sink_discovered(fipsd_ble_sink *s, const fipsd_ble_peer *peer, const int16_t *rssi,
    const uint8_t *data, size_t n)
{
    fipsd_ble_event *e = event(FIPSD_BLE_PEER_DISCOVERED, peer);
    if (e) {
        if (rssi) { e->has_rssi = true; e->rssi = *rssi; }
        if (n > FIPSD_BLE_SERVICE_DATA_MAX) n = FIPSD_BLE_SERVICE_DATA_MAX;
        if (data && n) { memcpy(e->service_data, data, n); e->service_data_len = (uint8_t)n; }
    }
    if (push(s, e)) return true;
    free(e); return false;
}
bool fipsd_ble_sink_lost(fipsd_ble_sink *s, const fipsd_ble_peer *peer)
{
    fipsd_ble_event *e = event(FIPSD_BLE_PEER_LOST, peer);
    if (push(s, e)) return true;
    free(e); return false;
}
bool fipsd_ble_sink_inbound(fipsd_ble_sink *s, fipsd_ble_stream *stream)
{
    fipsd_ble_event *e = stream ? event(FIPSD_BLE_INBOUND_STREAM, &stream->peer) : NULL;
    if (e) e->stream = stream;
    if (push(s, e)) return true;
    free(e);
    if (stream) { stream->ops->cancel(stream); stream->ops->close(stream); }
    return false;
}
bool fipsd_ble_sink_closed(fipsd_ble_sink *s, uint64_t id)
{
    fipsd_ble_event *e = event(FIPSD_BLE_STREAM_CLOSED, NULL);
    if (e) e->stream_id = id;
    if (push(s, e)) return true;
    free(e); return false;
}
static void event_discard(fipsd_ble_event *e)
{
    if (!e) return;
    if (e->kind == FIPSD_BLE_INBOUND_STREAM && e->stream) { e->stream->ops->cancel(e->stream); e->stream->ops->close(e->stream); }
    free(e);
}
bool fipsd_ble_backend_valid(const fipsd_ble_backend *b)
{
    const fipsd_ble_backend_ops *o = b ? b->ops : NULL;
    return o && o->start && o->stop && o->destroy && o->adapter && o->listen && o->advertise &&
        o->advertise_stop && o->scan_start && o->scan_stop && o->connect && o->adapter_name;
}

/* ---- instance helpers ------------------------------------------------------ */
static bool stopped(ble_instance *u) { return go_channel_is_closed(u->stop) || (u->root && go_context_is_canceled(u->root)); }
static bool conn_stopped(ble_conn *c) { return go_channel_is_closed(c->stop) || stopped(c->owner); }
static uint64_t tie_window(ble_instance *u) { return u->config.connect_timeout_ms; }
static uint64_t deadline_in(uint64_t ms) { uint64_t now = ble_now(); return ms > UINT64_MAX - now ? FIPSD_BLE_NO_DEADLINE : now + ms; }
static ble_conn *conn_of(ble_link *l) { return (ble_conn *)l; }
static bool live(void *ctx, const fipsd_ble_peer *p)
{ ble_instance *u = ctx; return ble_probes_contains(&u->probes, p) || ble_pool_peer_pooled(&u->pool, p); }
static bool pooled_peer(void *ctx, const fipsd_ble_peer *p)
{ ble_instance *u = ctx; return ble_pool_peer_pooled(&u->pool, p); }
static bool peer_node(ble_instance *u, const uint8_t key[32], fips_node_addr *out)
{
    fips_xonly_pubkey x; memcpy(x.bytes, key, 32);
    if (!u->secp) { *out = fips_node_addr_from_pubkey(&x); return true; }
    fips_peer_identity id;
    if (fips_peer_identity_from_pubkey(u->secp, &x, &id) != FIPS_IDENTITY_OK) return false;
    *out = id.node_addr; return true;
}
/* A node-directed notification that must not park (mu held), as
 * tcp_connected_locked: publication and close share the lock. */
static int publish_locked(ble_instance *u, fipsd_transport_event_kind kind, const fips_transport_addr *a)
{
    if (stopped(u) || !u->events) return -1;
    fipsd_transport_event *e = calloc(1, sizeof *e);
    if (!e) return -1;
    e->kind = kind; e->id = u->transport.id; e->address = *a;
    if (go_channel_try_send(u->events, e)) { free(e); return -1; }
    return 0;
}
/* Upstream NeighborBuffer::add_peer[_with_pubkey]: a DISCOVERED event whose
 * payload, when known, is the peer's 32-byte x-only key (the pubkey hint). */
static void announce(ble_instance *u, const fipsd_ble_peer *p, const uint8_t *key)
{
    fips_transport_addr a;
    if (u->events && fipsd_ble_peer_to_addr(p, &a))
        (void)fipsd_transport_emit(u->events, u->root, u->stop, FIPSD_TRANSPORT_DISCOVERED,
            u->transport.id, &a, key, key ? 32 : 0);
}

/* ---- streams before the pool ---------------------------------------------- */
static void abort_exchange_locked(ble_instance *u, ble_exchange *x)
{
    if (x->aborted) return;
    x->aborted = true;
    if (x->inbound) --u->inbound_exchanges;
    ble_io_cancel(x->stream, x->fd);
}
static ble_exchange *exchange_begin(ble_instance *u, fipsd_ble_stream *s, int fd, bool inbound)
{
    ble_exchange *x = calloc(1, sizeof *x);
    if (!x) return NULL;
    x->stream = s; x->fd = fd; x->inbound = inbound;
    nsync_mu_lock(&u->mu);
    if (stopped(u) || !atomic_load(&u->present)) { nsync_mu_unlock(&u->mu); free(x); return NULL; }
    if (inbound && u->inbound_exchanges >= BLE_INBOUND_INFLIGHT) {
        /* Nothing a connecting peer does can delay the next accept: the oldest
         * pending handshake is aborted to make room. */
        for (ble_exchange *o = u->exchanges; o; o = o->next) if (o->inbound && !o->aborted) {
            abort_exchange_locked(u, o); BLE_STAT(u, handshakes_aborted); break;
        }
    }
    ble_exchange **tail = &u->exchanges;
    while (*tail) tail = &(*tail)->next;
    *tail = x;
    if (inbound) ++u->inbound_exchanges;
    nsync_mu_unlock(&u->mu);
    return x;
}
static bool exchange_end(ble_instance *u, ble_exchange *x)
{
    nsync_mu_lock(&u->mu);
    for (ble_exchange **p = &u->exchanges; *p; p = &(*p)->next) if (*p == x) { *p = x->next; break; }
    bool aborted = x->aborted;
    if (x->inbound && !aborted) --u->inbound_exchanges;
    nsync_mu_unlock(&u->mu);
    free(x);
    return aborted;
}
/* Both sides send [0x00][x-only key] and read the peer's through the
 * connection's one reader. Each half has its own deadline: a peer that never
 * drains stalls the write, one that never answers stalls the read. */
static int pubkey_exchange(ble_instance *u, fipsd_ble_stream *s, int fd, ble_reader *r, uint8_t key[32], fips_node_addr *node)
{
    uint8_t msg[BLE_PUBKEY_SIZE] = {BLE_PUBKEY_PREFIX};
    memcpy(msg + 1, u->local_key.bytes, 32);
    int rc = ble_io_write(s, fd, msg, sizeof msg, deadline_in(u->config.pubkey_exchange_ms));
    if (rc) return rc;
    rc = ble_reader_read_exact(r, msg, sizeof msg, deadline_in(u->config.pubkey_exchange_ms));
    if (rc) return rc;
    if (msg[0] != BLE_PUBKEY_PREFIX) return -EPROTO;
    memcpy(key, msg + 1, 32);
    return peer_node(u, key, node) ? 0 : -EPROTO;
}

/* ---- pooled links ----------------------------------------------------------- */
static void conn_free(ble_conn *c)
{
    void *v;
    if (c->queue) while (go_channel_try_receive(c->queue, &v) == 0) free(v);
    GoChannel *ch[] = {c->stop, c->close, c->notify_stop, c->notified, c->reader_done, c->writer_done, c->queue};
    for (size_t i = 0; i < sizeof ch / sizeof ch[0]; ++i) if (ch[i]) go_channel_unref(ch[i]);
    ble_reader_free(&c->reader);
    free(c);
}
/* Out of the pool: a deliberate close lets the writer finish its queue, every
 * other teardown drops it. Caller holds mu. */
static void conn_drop_locked(ble_conn *c, bool deliberate)
{
    ble_instance *u = c->owner;
    if (c->pooled) {
        (void)ble_pool_remove(&u->pool, &c->link);
        c->pooled = false;
        fipsd_transport_stats_pool(&u->shared.stats, c->link.outbound ? FIPS_LINK_OUTBOUND : FIPS_LINK_INBOUND, false);
    }
    if (deliberate) { c->deliberate = true; go_channel_close(c->notify_stop); go_channel_close(c->close); }
    else go_channel_close(c->stop);
}
static void reader_fiber(void *arg)
{
    ble_conn *c = arg; ble_instance *u = c->owner;
    size_t cap = c->link.recv_mtu;
    if (cap < FIPS_FRAMING_FMP_MSG1_WIRE_SIZE) cap = FIPS_FRAMING_FMP_MSG1_WIRE_SIZE;
    uint8_t *buffer = malloc(cap);
    fipsd_stream f; fipsd_stream_init(&f, buffer, cap, c->link.recv_mtu);
    while (buffer && !conn_stopped(c)) {
        size_t n = 0;
        int rc = ble_reader_frame(&c->reader, &f, FIPSD_BLE_NO_DEADLINE, &n);
        if (rc != 1) {
            if (rc < 0 && rc != -ECANCELED && !conn_stopped(c)) {
                if (rc == -EPROTO) atomic_fetch_add(&u->shared.diagnostics.malformed, 1);
                fipsd_transport_stats_error(&u->shared.stats, false);
            }
            FIPSD_DEBUG(MOD, "%s: stream %llu receive ended (%d)", u->name, (unsigned long long)c->stream_id, rc);
            break;
        }
        fipsd_transport_stats_recv(&u->shared.stats, n);
        if (fipsd_transport_emit(u->events, u->root, c->stop, FIPSD_TRANSPORT_PACKET, u->transport.id,
                &c->link.addr, buffer, n)) break;
    }
    free(buffer);
    go_channel_close(c->reader_done);
}
/* The only place a BLE write is awaited: callers enqueue and return, and the
 * pool lock is never held across the link. */
static void writer_fiber(void *arg)
{
    ble_conn *c = arg; ble_instance *u = c->owner;
    while (!conn_stopped(c)) {
        void *value = NULL;
        GoSelectCase cases[] = {{GO_SELECT_RECEIVE, c->stop, NULL, &value},
            {GO_SELECT_RECEIVE, u->stop, NULL, &value}, {GO_SELECT_RECEIVE, c->queue, NULL, &value}};
        if (go_select(cases, 3) != 2 || !value) break;
        ble_frame *frame = value;
        int rc = ble_io_write(c->stream, c->fd, frame->bytes, frame->len, FIPSD_BLE_NO_DEADLINE);
        if (!rc) fipsd_transport_stats_sent(&u->shared.stats, frame->len);
        free(frame);
        if (rc) {
            if (!conn_stopped(c) && rc != -ECANCELED) fipsd_transport_stats_error(&u->shared.stats, true);
            FIPSD_DEBUG(MOD, "%s: stream %llu send failed (%d)", u->name, (unsigned long long)c->stream_id, rc);
            nsync_mu_lock(&u->mu); /* only this connection's own entry (remove_own) */
            if (c->pooled && ble_pool_get(&u->pool, &c->link.addr) == &c->link) conn_drop_locked(c, false);
            nsync_mu_unlock(&u->mu);
            break;
        }
    }
    go_channel_close(c->writer_done);
}
static void join(GoChannel *done) { void *v; (void)go_channel_receive(done, &v); }
static void conn_owner(void *arg)
{
    ble_conn *c = arg; ble_instance *u = c->owner;
    bool has_reader = fipsd_runtime_spawn(u->runtime, reader_fiber, c) == 0;
    bool has_writer = has_reader && fipsd_runtime_spawn(u->runtime, writer_fiber, c) == 0;
    void *v;
    while (has_writer && !conn_stopped(c)) {
        GoSelectCase cases[] = {{GO_SELECT_RECEIVE, c->stop, NULL, &v}, {GO_SELECT_RECEIVE, u->stop, NULL, &v},
            {GO_SELECT_RECEIVE, go_context_done(u->root), NULL, &v}, {GO_SELECT_RECEIVE, c->reader_done, NULL, &v},
            {GO_SELECT_RECEIVE, c->writer_done, NULL, &v}, {GO_SELECT_RECEIVE, c->close, NULL, &v}};
        int selected = go_select(cases, 6);
        if (selected == 5) {
            /* Deliberate close: the queued frames (a Disconnect, msg2) still go
             * out, within the writer drain bound. */
            go_channel_close(c->queue);
            GoSelectCase drain[] = {{GO_SELECT_RECEIVE, c->writer_done, NULL, &v},
                {GO_SELECT_RECEIVE, c->stop, NULL, &v}, {GO_SELECT_RECEIVE, u->stop, NULL, &v}};
            (void)go_select_timeout(drain, 3, BLE_WRITER_DRAIN_MS);
        }
        break;
    }
    nsync_mu_lock(&u->mu);
    if (c->pooled) conn_drop_locked(c, false);
    c->closing = true;
    /* Notify the node unless this was a deliberate close or a replacement
     * already holds the address; a newcomer at the address waits for it. */
    c->notifying = !c->deliberate && !ble_pool_get(&u->pool, &c->link.addr);
    /* A link that ended on its own takes its challengers with it: the pair is
     * lost, and both ends redial from scratch, where upstream's decline would
     * have left them. A deliberate close is the node moving onto a challenger
     * (its path switch closes the old address first), which must survive. */
    if (!c->deliberate && !c->link.challenger) {
        ble_link *d;
        for (size_t left = u->pool.count; left && (d = ble_pool_challenger_at(&u->pool, &c->link.peer)); --left)
            conn_drop_locked(conn_of(d), false);
    }
    go_channel_close(c->stop);
    ble_io_cancel(c->stream, c->fd);
    nsync_mu_unlock(&u->mu);
    if (has_reader) join(c->reader_done);
    if (has_writer) join(c->writer_done);
    ble_io_close(c->stream, c->fd);
    nsync_mu_lock(&u->mu); c->stream = NULL; nsync_mu_unlock(&u->mu);
    {
        char text[40]; (void)fipsd_ble_peer_format(&c->link.peer, text, sizeof text);
        FIPSD_DEBUG(MOD, "%s: link %s (stream %llu) closed%s", u->name, text, (unsigned long long)c->stream_id,
            c->notifying ? ", disconnected" : c->deliberate ? " deliberately" : ", replaced");
    }
    if (c->notifying)
        (void)fipsd_transport_emit(u->events, u->root, c->notify_stop, FIPSD_TRANSPORT_DISCONNECTED,
            u->transport.id, &c->link.addr, NULL, 0);
    nsync_mu_lock(&u->mu);
    c->notifying = false;
    go_channel_close(c->notified);
    for (ble_conn **p = &u->conns; *p; p = &(*p)->next) if (*p == c) { *p = c->next; break; }
    nsync_mu_unlock(&u->mu);
    (void)go_channel_try_send(u->changed, u);
    conn_free(c);
}

/* ---- offering a stream to the pool ------------------------------------------ */
typedef enum ble_role { ROLE_DIAL, ROLE_PROBE, ROLE_INBOUND } ble_role;
/* OFFER_CHALLENGER: pooled beside a verified link that stays authoritative, and
 * not announced: the node meets it only through its own address. */
typedef enum ble_outcome { OFFER_POOLED, OFFER_DECLINED, OFFER_OCCUPIED, OFFER_REFUSED, OFFER_FAILED, OFFER_CHALLENGER } ble_outcome;
typedef struct ble_offer {
    fipsd_ble_stream *stream;
    int fd;
    ble_reader reader;
    fipsd_ble_peer peer;
    fips_transport_addr addr;
    bool has_node;
    fips_node_addr node;
    uint8_t key[32];
} ble_offer;
static void offer_close(ble_offer *o)
{
    if (!o->stream) return;
    ble_io_cancel(o->stream, o->fd); ble_io_close(o->stream, o->fd);
    ble_reader_free(&o->reader); o->stream = NULL;
}
/* DISCONNECTED for an address must reach the node before anything that
 * replaces it there is announced: wait for an exiting link's notification.
 * Caller holds mu; it is released only while waiting and held again on
 * return. True once no link at the address is notifying, false on stop or
 * at the drain bound. The check and the insertion after it share one hold of
 * mu, so a link cannot start notifying between them (conn_owner decides
 * under mu, by whether the pool already holds a replacement). */
static bool await_notified_locked(ble_instance *u, const fips_transport_addr *a)
{
    for (;;) {
        if (stopped(u)) return false;
        GoChannel *wait = NULL;
        for (ble_conn *c = u->conns; c && !wait; c = c->next)
            if (c->notifying && fips_transport_addr_equal(&c->link.addr, a)) wait = go_channel_ref(c->notified);
        if (!wait) return true;
        nsync_mu_unlock(&u->mu);
        void *v;
        GoSelectCase cases[] = {{GO_SELECT_RECEIVE, wait, NULL, &v}, {GO_SELECT_RECEIVE, u->stop, NULL, &v}};
        GoSelectResult r = go_select_timeout(cases, 2, BLE_WRITER_DRAIN_MS);
        go_channel_unref(wait);
        nsync_mu_lock(&u->mu);
        if (r.selected_case != 0) return false;
    }
}
static void log_insert(ble_instance *u, const ble_insert *r, const fips_transport_addr *a, const char *role)
{
    if (r->has_ended) FIPSD_INFO(MOD, "%s: pool refusal run ended (%llu refused)", u->name, (unsigned long long)r->ended);
    if (r->kind == BLE_INSERT_REFUSED && r->first)
        FIPSD_WARN(MOD, "%s: pool full of verified links or links still inside their grace (max_connections %zu); refusing new links",
            u->name, u->pool.max);
    (void)a; (void)role;
}
/* Settle a stream that finished its exchange against the pool and insert it,
 * under one guard (upstream arbitrate + pool_link): a link the pool refuses
 * never starts its receive loop. On success the stream belongs to the link. */
static ble_outcome offer(ble_instance *u, ble_offer *o, ble_role role, fipsd_ble_peer *kept)
{
    bool outbound = role != ROLE_INBOUND;
    /* Copied: once pooled, the owner fiber may free c before the log below. */
    const uint64_t stream_id = o->stream->id;
    ble_conn *c = calloc(1, sizeof *c);
    if (c) {
        c->stop = go_channel_create(1); c->close = go_channel_create(1); c->notify_stop = go_channel_create(1);
        c->notified = go_channel_create(1); c->reader_done = go_channel_create(1); c->writer_done = go_channel_create(1);
        c->queue = go_channel_create(u->config.send_queue_depth);
        if (!c->stop || !c->close || !c->notify_stop || !c->notified || !c->reader_done || !c->writer_done || !c->queue) {
            conn_free(c); c = NULL;
        }
    }
    if (!c) return OFFER_FAILED;
    c->owner = u; c->stream = o->stream; c->stream_id = stream_id; c->fd = o->fd; c->reader = o->reader;
    c->link = (ble_link){.addr = o->addr, .peer = o->peer, .id = fipsd_stream_next_id(), .outbound = outbound,
        .has_node = o->has_node, .node = o->node};
    ble_stream_mtu(o->stream, u->config.mtu, &c->link.send_mtu, &c->link.recv_mtu);
    ble_outcome result = OFFER_FAILED;
    const char *why = "stopped";
    bool challenger = false;
    nsync_mu_lock(&u->mu);
    if (!await_notified_locked(u, &o->addr) || !atomic_load(&u->present)) goto out;
    uint64_t now = ble_now();
    c->link.established_ms = c->link.grace_from_ms = c->link.held_from_ms = now;
    if (u->has_identity && o->has_node) {
        ble_admission a = ble_arbitrate(&u->pool, &u->local_node, &o->node, &o->addr, outbound, tie_window(u), now);
        if (a.kind == BLE_DECLINE) {
            BLE_STAT(u, duplicate_node_declines);
            if (kept) *kept = a.existing->peer;
            why = "duplicate-node-decline"; result = OFFER_DECLINED; goto out;
        }
        if (a.kind == BLE_OCCUPIED) { why = "address-occupied"; result = OFFER_OCCUPIED; goto out; }
        if (a.kind == BLE_CHALLENGE) {
            /* The verified link keeps its address and its session. The newcomer
             * waits under an address of its own until the node authenticates it
             * (fipsd_ble_mark_verified): its claim proves nothing yet. */
            if (!ble_challenger_addr(&o->peer, c->link.id, &c->link.addr)) { why = "challenger-address"; goto out; }
            c->link.challenger = challenger = true;
            FIPSD_DEBUG(MOD, "%s: stream %llu waits beside the verified link %llu", u->name,
                (unsigned long long)c->stream_id, (unsigned long long)conn_of(a.existing)->stream_id);
        }
        if (a.kind == BLE_REPLACE) {
            /* Raced a link to the same peer and won the tie-break. */
            FIPSD_DEBUG(MOD, "%s: duplicate-link-replaced (stream %llu replaces %llu)", u->name,
                (unsigned long long)c->stream_id, (unsigned long long)conn_of(a.existing)->stream_id);
            conn_drop_locked(conn_of(a.existing), false);
            BLE_STAT(u, duplicate_link_replacements);
        }
    }
    ble_insert r = ble_pool_insert(&u->pool, &c->link, now);
    log_insert(u, &r, &o->addr, role == ROLE_INBOUND ? "peripheral" : "central");
    if (r.kind == BLE_INSERT_OCCUPIED) { why = "address-occupied"; result = OFFER_OCCUPIED; goto out; }
    if (r.kind == BLE_INSERT_REFUSED) { BLE_STAT(u, connections_rejected); why = "pool-rejected"; result = OFFER_REFUSED; goto out; }
    if (r.evicted) { conn_drop_locked(conn_of(r.evicted), false); BLE_STAT(u, pool_evictions); }
    c->pooled = true;
    c->next = u->conns; u->conns = c;
    fipsd_transport_stats_pool(&u->shared.stats, outbound ? FIPS_LINK_OUTBOUND : FIPS_LINK_INBOUND, true);
    if (fipsd_runtime_spawn(u->runtime, conn_owner, c)) {
        u->conns = c->next; (void)ble_pool_remove(&u->pool, &c->link); c->pooled = false;
        fipsd_transport_stats_pool(&u->shared.stats, outbound ? FIPS_LINK_OUTBOUND : FIPS_LINK_INBOUND, false);
        goto out;
    }
    result = challenger ? OFFER_CHALLENGER : OFFER_POOLED; why = challenger ? "challenger" : "pooled";
    o->stream = NULL; /* now the link's */
out:
    nsync_mu_unlock(&u->mu);
    {
        char text[40]; (void)fipsd_ble_peer_format(&o->peer, text, sizeof text);
        FIPSD_DEBUG(MOD, "%s: %s link %s: %s (stream %llu)", u->name, role == ROLE_INBOUND ? "inbound" :
            role == ROLE_PROBE ? "probe" : "dialled", text, why, (unsigned long long)stream_id);
    }
    if (result != OFFER_POOLED && result != OFFER_CHALLENGER) { c->stream = NULL; c->reader = (ble_reader){0}; conn_free(c); }
    return result;
}

/* ---- dialling -------------------------------------------------------------- */
/* The PSM to dial: the one the peer advertised; else, for a UUID-only
 * advertiser, the GATT PSM characteristic (never cached: a restarted listener
 * is assigned a new PSM); else the configured one. *learned reports whether
 * the advertised PSM was used, which is the only one a failed dial forgets. */
static uint16_t dial_psm(ble_instance *u, const fipsd_ble_peer *p, uint64_t deadline, bool *learned)
{
    nsync_mu_lock(&u->mu);
    const ble_recall_entry *e = ble_recall_get(&u->learned_psm, p);
    uint16_t psm = e ? e->psm : 0;
    nsync_mu_unlock(&u->mu);
    *learned = e != NULL;
    if (e) return psm;
    if (u->backend.ops->read_psm) {
        uint8_t value[FIPSD_BLE_GATT_VALUE_MAX]; size_t n = 0;
        if (!u->backend.ops->read_psm(u->backend.self, p, deadline, value, sizeof value, &n) &&
            n <= sizeof value && fipsd_ble_decode_gatt_psm(value, n, &psm)) return psm;
    }
    return u->config.psm;
}
/* A learned PSM that failed a dial (other than by timing out) is stale. Only
 * the PSM actually dialled is forgotten: a dial at the fallback, or at a value
 * since replaced, says nothing about the entry held now. */
static void forget_psm(ble_instance *u, const fipsd_ble_peer *p, uint16_t dialled)
{
    nsync_mu_lock(&u->mu);
    const ble_recall_entry *e = ble_recall_get(&u->learned_psm, p);
    if (e && e->psm == dialled) (void)ble_recall_remove(&u->learned_psm, p);
    nsync_mu_unlock(&u->mu);
}
/* Dial, adopt and exchange: 0 with the offer filled, or -errno (counted). */
static int dial_and_exchange(ble_instance *u, const fipsd_ble_peer *p, ble_offer *o, bool *timed_out)
{
    *timed_out = false;
    *o = (ble_offer){.peer = *p, .fd = -1};
    if (!fipsd_ble_peer_to_addr(p, &o->addr)) return -EINVAL;
    uint64_t deadline = deadline_in(u->config.connect_timeout_ms);
    bool learned = false;
    uint16_t psm = dial_psm(u, p, deadline, &learned);
    fipsd_ble_stream *s = NULL;
    int rc = u->backend.ops->connect(u->backend.self, p, psm, deadline, &s);
    if (!rc && !s) rc = -EIO;
    if (rc) {
        if (rc == -ECANCELED || rc == -ENODEV || rc == -EISCONN || stopped(u)) return rc;
        if (rc == -ETIMEDOUT) { BLE_STAT(u, connect_timeouts); *timed_out = true; }
        else { BLE_STAT(u, connect_errors); if (learned) forget_psm(u, p, psm); }
        FIPSD_DEBUG(MOD, "%s: connect to psm 0x%04x failed (%d)", u->name, psm, rc);
        return rc;
    }
    o->stream = s; s->peer = *p;
    uint16_t send_mtu, recv_mtu;
    ble_stream_mtu(s, u->config.mtu, &send_mtu, &recv_mtu);
    if (ble_io_adopt(s, &o->fd) || ble_reader_init(&o->reader, s, o->fd, recv_mtu)) {
        ble_io_cancel(s, o->fd); ble_io_close(s, o->fd); o->stream = NULL; return -ENOMEM;
    }
    if (!u->has_identity) return 0;
    ble_exchange *x = exchange_begin(u, s, o->fd, false);
    if (!x) { offer_close(o); return -ECANCELED; }
    rc = pubkey_exchange(u, s, o->fd, &o->reader, o->key, &o->node);
    bool aborted = exchange_end(u, x);
    if (rc) {
        if (!aborted && !stopped(u) && rc != -ECANCELED) BLE_STAT(u, pubkey_exchange_failures);
        offer_close(o);
        return rc;
    }
    o->has_node = true;
    return 0;
}
typedef struct dial_job { ble_instance *u; ble_dial *entry; fipsd_ble_peer peer; } dial_job;
/* connect_async's background task: connect, exchange, settle, and tell the
 * node whether the address is now connected. */
static void dial_fiber(void *arg)
{
    dial_job *j = arg; ble_instance *u = j->u;
    ble_offer o; bool timed_out;
    int rc = dial_and_exchange(u, &j->peer, &o, &timed_out);
    ble_outcome outcome = OFFER_FAILED;
    fipsd_ble_peer kept = j->peer;
    if (!rc) outcome = offer(u, &o, ROLE_DIAL, &kept);
    if (outcome == OFFER_POOLED || outcome == OFFER_CHALLENGER) BLE_STAT(u, connections_established);
    offer_close(&o);
    nsync_mu_lock(&u->mu);
    for (ble_dial **p = &u->dials; *p; p = &(*p)->next) if (*p == j->entry) { *p = j->entry->next; break; }
    bool abandoned = j->entry->abandoned;
    /* Connected exactly when a link holds the address now, whichever link;
     * published under the lock, so that link's DISCONNECTED comes after. */
    bool connected = !abandoned && ble_pool_get(&u->pool, &j->entry->addr);
    if (connected) (void)publish_locked(u, FIPSD_TRANSPORT_CONNECTED, &j->entry->addr);
    nsync_mu_unlock(&u->mu);
    if (!abandoned && !connected && !stopped(u))
        (void)fipsd_transport_emit(u->events, u->root, u->stop, FIPSD_TRANSPORT_DISCONNECTED, u->transport.id,
            &j->entry->addr, NULL, 0);
    if (rc == 0 && (outcome == OFFER_POOLED || outcome == OFFER_DECLINED) && o.has_node)
        announce(u, outcome == OFFER_POOLED ? &j->peer : &kept, o.key);
    free(j->entry); free(j);
}

/* ---- the scan + probe loop ---------------------------------------------------- */
static void probe(ble_instance *u, const fipsd_ble_peer *p)
{
    fips_transport_addr a;
    if (!atomic_load(&u->present) || !fipsd_ble_peer_to_addr(p, &a)) return;
    BLE_STAT(u, scan_results);
    uint64_t now = ble_now();
    nsync_mu_lock(&u->mu);
    if (ble_pool_peer_pooled(&u->pool, p)) { ble_probes_resolve(&u->probes, p); nsync_mu_unlock(&u->mu); return; }
    /* Tracked for retry: a scanner is not a reliable repeat source. */
    ble_probes_observe(&u->probes, p, now);
    if (!ble_probes_is_due(&u->probes, p, now)) { nsync_mu_unlock(&u->mu); return; }
    /* An address already resolved to a peer we hold a verified link to is
     * skipped; an unverified claim must not hide the node's own address. */
    const ble_recall_entry *known = ble_recall_get(&u->known_node, p);
    if (known) {
        fips_node_addr node = known->node;
        if (ble_pool_find_verified(&u->pool, &node)) {
            ble_probes_resolve(&u->probes, p);
            (void)ble_recall_note(&u->known_node, p, 0, &node, now, live, u);
            nsync_mu_unlock(&u->mu); return;
        }
        (void)ble_recall_remove(&u->known_node, p);
    }
    ble_probes_mark_attempt(&u->probes, p, now);
    nsync_mu_unlock(&u->mu);
    if (!u->has_identity) { announce(u, p, NULL); return; }
    ble_offer o; bool timed_out;
    int rc = dial_and_exchange(u, p, &o, &timed_out);
    if (rc) {
        if (rc != -ECANCELED && rc != -ENODEV && rc != -EISCONN && !stopped(u)) {
            nsync_mu_lock(&u->mu); (void)ble_probes_record_failure(&u->probes, p, ble_now()); nsync_mu_unlock(&u->mu);
        }
        return;
    }
    fipsd_ble_peer kept = *p;
    ble_outcome outcome = offer(u, &o, ROLE_PROBE, &kept);
    offer_close(&o);
    nsync_mu_lock(&u->mu);
    if (outcome == OFFER_DECLINED) {
        /* Remember what this address resolved to, so the next cooldown skips
         * it rather than paying another connect and exchange. */
        (void)ble_recall_note(&u->known_node, p, 0, &o.node, ble_now(), live, u);
        ble_link *v = ble_pool_find_verified(&u->pool, &o.node);
        kept = v ? v->peer : *p;
    }
    if (outcome != OFFER_REFUSED && outcome != OFFER_FAILED) ble_probes_resolve(&u->probes, p);
    nsync_mu_unlock(&u->mu);
    if (outcome == OFFER_POOLED) { BLE_STAT(u, connections_established); announce(u, p, o.key); }
    else if (outcome == OFFER_CHALLENGER) BLE_STAT(u, connections_established);
    else if (outcome == OFFER_DECLINED) announce(u, &kept, o.key);
}
static void prober_fiber(void *arg)
{
    ble_instance *u = arg;
    uint64_t next_tick = deadline_in(u->config.probe_cooldown_ms);
    while (!stopped(u)) {
        void *v = NULL;
        uint64_t now = ble_now();
        GoSelectCase cases[] = {{GO_SELECT_RECEIVE, u->stop, NULL, &v},
            {GO_SELECT_RECEIVE, go_context_done(u->root), NULL, &v}, {GO_SELECT_RECEIVE, u->adverts, NULL, &v}};
        GoSelectResult r = go_select_timeout(cases, 3, next_tick > now ? next_tick - now : 0);
        fipsd_ble_peer p;
        if (r.selected_case == 2 && v) {
            ble_advert *m = v; p = m->peer;
            nsync_mu_lock(&u->mu);
            if (m->lost) ble_probes_resolve(&u->probes, &p);
            else if (m->has_psm) (void)ble_recall_note(&u->learned_psm, &p, m->psm, NULL, ble_now(), live, u);
            nsync_mu_unlock(&u->mu);
            bool lost = m->lost;
            free(m);
            if (lost) continue;
        } else if (r.selected_case < 0) {
            /* The retry tick: re-probe what the scanner will not offer again,
             * rotating so one dead address cannot starve the rest. */
            now = ble_now();
            do next_tick += u->config.probe_cooldown_ms; while (next_tick <= now);
            nsync_mu_lock(&u->mu);
            ble_probes_drop(&u->probes, pooled_peer, u);
            ble_recall_prune(&u->known_node, now, live, u);
            ble_recall_prune(&u->learned_psm, now, live, u);
            bool due = ble_probes_next_due(&u->probes, now, &p);
            nsync_mu_unlock(&u->mu);
            if (!due) continue;
        } else break;
        probe(u, &p);
    }
}

/* ---- inbound ---------------------------------------------------------------- */
typedef struct admit_job { ble_instance *u; fipsd_ble_stream *stream; int fd; ble_exchange *x; } admit_job;
static void admit_fiber(void *arg)
{
    admit_job *j = arg; ble_instance *u = j->u;
    ble_offer o = {.stream = j->stream, .peer = j->stream->peer, .fd = j->fd};
    ble_exchange *x = j->x;
    free(j);
    uint16_t send_mtu, recv_mtu;
    ble_stream_mtu(o.stream, u->config.mtu, &send_mtu, &recv_mtu);
    if (!fipsd_ble_peer_to_addr(&o.peer, &o.addr) || ble_reader_init(&o.reader, o.stream, o.fd, recv_mtu)) {
        if (x) (void)exchange_end(u, x);
        offer_close(&o); return;
    }
    if (x) {
        int rc = pubkey_exchange(u, o.stream, o.fd, &o.reader, o.key, &o.node);
        bool aborted = exchange_end(u, x);
        if (rc) {
            if (!aborted && !stopped(u) && rc != -ECANCELED) BLE_STAT(u, pubkey_exchange_failures);
            offer_close(&o); return;
        }
        o.has_node = true;
    }
    fipsd_ble_peer kept = o.peer;
    ble_outcome outcome = offer(u, &o, ROLE_INBOUND, &kept);
    offer_close(&o);
    if (outcome == OFFER_POOLED) {
        BLE_STAT(u, connections_accepted);
        if (o.has_node) announce(u, &o.peer, o.key);
    } else if (outcome == OFFER_CHALLENGER) BLE_STAT(u, connections_accepted);
    else if (outcome == OFFER_DECLINED && o.has_node) announce(u, &kept, o.key);
}
/* Upstream accept_loop: one iteration is bounded by the accept alone. The
 * exchange is registered here, in arrival order, so the in-flight budget
 * aborts the oldest pending handshake; the exchange itself runs off the loop. */
static void inbound(ble_instance *u, fipsd_ble_stream *s)
{
    fips_transport_addr a;
    bool ok = atomic_load(&u->present) && u->config.accept_connections && !stopped(u) && fipsd_ble_peer_to_addr(&s->peer, &a);
    if (ok) {
        /* A settled link at this address makes the channel redundant; a young
         * one may be racing it, and only arbitration settles that alike. */
        nsync_mu_lock(&u->mu);
        ble_link *l = ble_pool_get(&u->pool, &a);
        ok = !l || ble_now() - l->established_ms < tie_window(u);
        nsync_mu_unlock(&u->mu);
    }
    int fd = -1;
    admit_job *j = ok && !ble_io_adopt(s, &fd) ? malloc(sizeof *j) : NULL;
    if (j) { j->u = u; j->stream = s; j->fd = fd; j->x = u->has_identity ? exchange_begin(u, s, fd, true) : NULL; }
    if (j && u->has_identity && !j->x) { free(j); j = NULL; }
    if (!j || fipsd_runtime_spawn(u->runtime, admit_fiber, j)) {
        if (j && j->x) (void)exchange_end(u, j->x);
        free(j); ble_io_cancel(s, fd); ble_io_close(s, fd);
    }
}

/* ---- adapter presence --------------------------------------------------------- */
static void become_absent(ble_instance *u)
{
    nsync_mu_lock(&u->mu);
    bool was = atomic_load(&u->present);
    atomic_store(&u->present, false); u->listener_psm = 0;
    /* The channels die with the radio; the node hears DISCONNECTED for each
     * pooled link, and every exchange in flight is cancelled. */
    for (ble_conn *c = u->conns; c; c = c->next) if (!c->closing) conn_drop_locked(c, false);
    for (ble_exchange *x = u->exchanges; x; x = x->next) abort_exchange_locked(u, x);
    nsync_mu_unlock(&u->mu);
    if (!was) return;
    (void)u->backend.ops->advertise_stop(u->backend.self);
    (void)u->backend.ops->scan_stop(u->backend.self);
    atomic_store(&u->shared.state, FIPSD_TRANSPORT_ABSENT);
    fipsd_presence_transition(&u->shared.presence, FIPSD_ABSENT, ble_now());
    FIPSD_INFO(MOD, "%s: adapter %s unavailable", u->name, u->adapter);
    (void)fipsd_transport_emit(u->events, u->root, u->stop, FIPSD_TRANSPORT_BECAME_ABSENT, u->transport.id, NULL, NULL, 0);
}
/* Listen, advertise the PSM actually bound, scan. A listener that cannot be
 * opened is retried on the bind backoff; advertising and scanning failures
 * are reported and leave the transport up, as upstream start_async. */
static bool become_present(ble_instance *u)
{
    fipsd_presence_transition(&u->shared.presence, FIPSD_BINDING, ble_now());
    uint16_t psm = u->config.psm;
    if (u->config.accept_connections) {
        int rc = u->backend.ops->listen(u->backend.self, u->config.psm, deadline_in(u->config.connect_timeout_ms), &psm);
        if (rc || !psm) {
            fipsd_presence_transition(&u->shared.presence, FIPSD_ABSENT, ble_now());
            uint32_t n = fipsd_presence_attempt(&u->shared.presence);
            if (n == 1) FIPSD_ERROR(MOD, "%s: failed to start the BLE listener (%d); retrying", u->name, rc);
            return false;
        }
    }
    nsync_mu_lock(&u->mu);
    atomic_store(&u->present, true); u->listener_psm = psm;
    nsync_mu_unlock(&u->mu);
    if (u->config.advertise) {
        fipsd_ble_advert ad = {.psm = psm};
        fipsd_ble_encode_psm(psm, ad.service_data);
        int rc = u->backend.ops->advertise(u->backend.self, &ad, deadline_in(u->config.connect_timeout_ms));
        if (rc) FIPSD_WARN(MOD, "%s: failed to start BLE advertising (%d)", u->name, rc);
        else BLE_STAT(u, advertisements_sent);
    }
    if (u->config.scan) {
        int rc = u->backend.ops->scan_start(u->backend.self, deadline_in(u->config.connect_timeout_ms));
        if (rc) FIPSD_WARN(MOD, "%s: failed to start BLE scanning (%d)", u->name, rc);
    }
    fipsd_presence_facts facts = {.valid = true, .exists = true, .up = true, .carrier = true, .medium = FIPS_PATH_Ble};
    fipsd_presence_bound(&u->shared.presence, facts, ble_now());
    atomic_store(&u->shared.state, FIPSD_TRANSPORT_BOUND);
    FIPSD_INFO(MOD, "%s: BLE transport up on %s, psm 0x%04x (requested 0x%04x)", u->name, u->adapter, psm, u->config.psm);
    (void)fipsd_transport_emit(u->events, u->root, u->stop, FIPSD_TRANSPORT_BECAME_PRESENT, u->transport.id, NULL, NULL, 0);
    (void)go_channel_try_send(u->bound, u);
    return true;
}
static void stream_closed(ble_instance *u, uint64_t id)
{
    nsync_mu_lock(&u->mu);
    for (ble_conn *c = u->conns; c; c = c->next) if (!c->closing && c->stream_id == id) conn_drop_locked(c, false);
    for (ble_exchange *x = u->exchanges; x; x = x->next) if (x->stream->id == id) abort_exchange_locked(u, x);
    nsync_mu_unlock(&u->mu);
}
static void handle(ble_instance *u, fipsd_ble_event *e)
{
    if (e->kind == FIPSD_BLE_INBOUND_STREAM) { fipsd_ble_stream *s = e->stream; e->stream = NULL; inbound(u, s); }
    else if (e->kind == FIPSD_BLE_STREAM_CLOSED) stream_closed(u, e->stream_id);
    else {
        ble_advert *m = calloc(1, sizeof *m);
        if (m) {
            m->peer = e->peer; m->lost = e->kind == FIPSD_BLE_PEER_LOST;
            m->has_psm = !m->lost && fipsd_ble_decode_psm(e->service_data, e->service_data_len, &m->psm);
        }
        /* The scanner fan-in is bounded, like the upstream scan channel. */
        if (!m || !u->config.scan || go_channel_try_send(u->adverts, m)) { free(m); atomic_fetch_add(&u->sink.dropped, 1); }
    }
    event_discard(e);
}
static void actor_fiber(void *arg)
{
    ble_instance *u = arg;
    uint64_t retry_at = 0;
    bool retry = false;
    unsigned seen = 0;
    while (!stopped(u)) {
        void *v = NULL;
        GoSelectCase cases[] = {{GO_SELECT_RECEIVE, u->stop, NULL, &v},
            {GO_SELECT_RECEIVE, go_context_done(u->root), NULL, &v},
            {GO_SELECT_RECEIVE, u->sink.adapter_wake, NULL, &v}, {GO_SELECT_RECEIVE, u->sink.events, NULL, &v}};
        int selected;
        if (retry) {
            uint64_t now = ble_now();
            selected = go_select_timeout(cases, 4, retry_at > now ? retry_at - now : 0).selected_case;
        } else selected = go_select(cases, 4);
        if (selected == 0 || selected == 1) break;
        if (selected == 3) { if (v) handle(u, v); continue; }
        unsigned a = atomic_load(&u->sink.adapter);
        bool up = adapter_up(a), cycled = (a >> 3) - seen >= 2;
        seen = a >> 3;
        if (!up) { retry = false; become_absent(u); continue; }
        if (atomic_load(&u->present) && cycled) become_absent(u); /* went down and up unseen */
        if (atomic_load(&u->present)) continue;
        retry = !become_present(u);
        if (retry) retry_at = ble_now() + fipsd_bind_backoff_ms(fipsd_presence_snapshot(&u->shared.presence).attempts);
    }
}

/* ---- the transport vtable ------------------------------------------------------ */
static int start_async(fipsd_transport *t, fipsd_runtime *rt, const fipsd_node_context *node, GoChannel *events)
{
    ble_instance *u = t->user;
    if (u->started || !rt || !events) return -1;
    u->runtime = rt; u->root = fipsd_runtime_context(rt); u->events = events;
    if (node) {
        u->local_key = fips_identity_pubkey(fipsd_node_identity(node)); u->secp = fipsd_node_secp(node);
        u->has_identity = true;
    } else if (u->config.identity) { u->local_key = *u->config.identity; u->secp = u->config.secp; u->has_identity = true; }
    if (u->has_identity) u->local_node = fips_node_addr_from_pubkey(&u->local_key);
    atomic_store(&u->shared.started, true);
    atomic_store(&u->shared.state, FIPSD_TRANSPORT_ABSENT);
    atomic_store(&u->sink.open, true);
    if (u->backend.ops->start(u->backend.self, rt, &u->sink)) {
        atomic_store(&u->sink.open, false);
        atomic_store(&u->shared.state, FIPSD_TRANSPORT_FAILED);
        FIPSD_ERROR(MOD, "%s: the BLE backend for %s did not start", u->name, u->adapter);
        return -1;
    }
    u->started = true;
    if (fipsd_runtime_spawn(rt, actor_fiber, u) || fipsd_runtime_spawn(rt, prober_fiber, u)) {
        go_channel_close(u->stop); u->backend.ops->stop(u->backend.self);
        atomic_store(&u->shared.state, FIPSD_TRANSPORT_FAILED);
        return -1;
    }
    return 0;
}
static void stop(fipsd_transport *t)
{
    ble_instance *u = t->user;
    go_channel_close(u->stop);
    atomic_store(&u->sink.open, false);
    if (u->started) u->backend.ops->stop(u->backend.self);
    nsync_mu_lock(&u->mu);
    for (ble_conn *c = u->conns; c; c = c->next) { go_channel_close(c->notify_stop); go_channel_close(c->stop); }
    for (ble_exchange *x = u->exchanges; x; x = x->next) abort_exchange_locked(u, x);
    for (ble_dial *d = u->dials; d; d = d->next) d->abandoned = true;
    nsync_mu_unlock(&u->mu);
}
static uint16_t mtu(const fipsd_transport *t, const fips_transport_addr *to)
{
    ble_instance *u = t->user;
    if (!to || !to->len) return u->config.mtu;
    nsync_mu_lock(&u->mu);
    ble_link *l = ble_pool_get(&u->pool, to);
    uint16_t m = l ? ble_link_mtu(l) : u->config.mtu;
    nsync_mu_unlock(&u->mu);
    return m;
}
/* Enqueue for the link's writer and return; fail fast when not connected
 * (the node's retry machine dials through connect_async first). */
static int send_frame(fipsd_transport *t, const fips_transport_addr *to, const uint8_t *bytes, size_t n)
{
    ble_instance *u = t->user;
    if (!u->started || !to || !bytes || stopped(u) || fipsd_datagram_frame(bytes, n, UINT16_MAX) != FIPS_OK) return -1;
    ble_frame *frame = malloc(sizeof *frame + n);
    if (!frame) return -1;
    frame->len = n; memcpy(frame->bytes, bytes, n);
    nsync_mu_lock(&u->mu);
    ble_link *l = ble_pool_get(&u->pool, to);
    int rc = -1;
    if (l && n > ble_link_mtu(l)) BLE_STAT(u, mtu_exceeded);
    else if (l && !(rc = go_channel_try_send(conn_of(l)->queue, frame) ? -1 : 0)) frame = NULL;
    else if (l) { fipsd_transport_stats_error(&u->shared.stats, true); atomic_fetch_add(&u->shared.diagnostics.dropped, 1); }
    nsync_mu_unlock(&u->mu);
    free(frame);
    return rc;
}
static int discover(fipsd_transport *t) { (void)t; return 0; } /* discovery is pushed as DISCOVERED */
static int connect_async(fipsd_transport *t, const fips_transport_addr *to)
{
    ble_instance *u = t->user;
    fipsd_ble_peer p;
    if (!u->started || stopped(u) || !fipsd_ble_peer_from_addr(to, &p)) return -1;
    fips_transport_addr canonical;
    if (!fipsd_ble_peer_to_addr(&p, &canonical)) return -1;
    nsync_mu_lock(&u->mu);
    int rc = 0;
    if (ble_pool_get(&u->pool, &canonical)) rc = publish_locked(u, FIPSD_TRANSPORT_CONNECTED, &canonical);
    else {
        bool pending = false;
        for (ble_dial *d = u->dials; d && !pending; d = d->next) pending = !d->abandoned && fips_transport_addr_equal(&d->addr, &canonical);
        dial_job *j = pending ? NULL : calloc(1, sizeof *j);
        ble_dial *d = j ? calloc(1, sizeof *d) : NULL;
        if (!pending && (!d || (j->u = u, j->entry = d, j->peer = p, d->addr = canonical, d->next = u->dials, u->dials = d,
                fipsd_runtime_spawn(u->runtime, dial_fiber, j)))) {
            if (d && u->dials == d) u->dials = d->next;
            free(d); free(j); rc = -1;
        }
    }
    nsync_mu_unlock(&u->mu);
    return rc;
}
static void close_connection(fipsd_transport *t, const fips_transport_addr *to)
{
    ble_instance *u = t->user;
    nsync_mu_lock(&u->mu);
    ble_link *l = ble_pool_get(&u->pool, to);
    if (l) conn_drop_locked(conn_of(l), true);
    /* A retiring link may be backpressured on its exit event: an explicit
     * close cancels that publication too. A dial in flight is abandoned. */
    for (ble_conn *c = u->conns; c; c = c->next)
        if (!c->pooled && fips_transport_addr_equal(&c->link.addr, to)) { c->deliberate = true; go_channel_close(c->notify_stop); }
    for (ble_dial *d = u->dials; d; d = d->next) if (fips_transport_addr_equal(&d->addr, to)) d->abandoned = true;
    nsync_mu_unlock(&u->mu);
}
static void network_changed(fipsd_transport *t, const struct fipsd_netmon_event *e) { (void)t; (void)e; }
/* The node authenticated the peer at `a` as `n`: the only evidence that a
 * challenger is who it claimed, since the node reports it for the address the
 * authenticated frame arrived on and a challenger has an address of its own.
 * The verified link's own traffic (its address) can never speak for it. A
 * challenger the node authenticates as the claimed node replaces every link at
 * its device address, the verified one included; as a different node it is
 * dropped and the verified link stays. Dropped incumbents notify the node, as
 * any lost link does: the peer is already bound to the challenger's address,
 * so the notice finds nothing there to end. */
void fipsd_ble_mark_verified(fipsd_transport *t, const fips_transport_addr *a, const fips_node_addr *n)
{
    ble_instance *u = t->user;
    nsync_mu_lock(&u->mu);
    ble_link *l = ble_pool_get(&u->pool, a);
    ble_proof proof = l ? ble_pool_prove(&u->pool, l, n) : BLE_PROOF_NOT_CHALLENGER;
    if (proof == BLE_PROOF_PROMOTED) {
        /* The node may already have closed the verified link itself (its path
         * switch does), so the replacement is counted either way. */
        ble_link *d;
        for (size_t left = u->pool.count; left && (d = ble_pool_displaced(&u->pool, l)); --left)
            conn_drop_locked(conn_of(d), false);
        BLE_STAT(u, duplicate_link_replacements);
    } else if (proof == BLE_PROOF_MISMATCH) {
        BLE_STAT(u, duplicate_node_declines);
        conn_drop_locked(conn_of(l), false);
    } else (void)ble_pool_mark_verified(&u->pool, a, n, NULL);
    nsync_mu_unlock(&u->mu);
}
void fipsd_ble_clear_verified(fipsd_transport *t, const fips_transport_addr *a, const fips_node_addr *n, bool removed)
{
    ble_instance *u = t->user;
    nsync_mu_lock(&u->mu); (void)ble_pool_clear_verified(&u->pool, a, n, removed, ble_now()); nsync_mu_unlock(&u->mu);
}
static void peer_link(fipsd_transport *t, const fips_transport_addr *a, const struct fips_node_addr *n, bool uses, bool removed)
{ if (uses) fipsd_ble_mark_verified(t, a, n); else fipsd_ble_clear_verified(t, a, n, removed); }
/* FRC-1301 6.5.4: a channel that is unverified, never carried a handshake,
 * and has gone `timeout` since its admission or since the node last held it.
 * The node's tick calls this; the transport sets no timer of its own. */
size_t fipsd_ble_close_unproven(fipsd_transport *t, uint64_t timeout, bool (*held)(void *, const fips_transport_addr *), void *ctx)
{
    ble_instance *u = t->user;
    uint64_t now = ble_now();
    nsync_mu_lock(&u->mu);
    for (size_t i = 0; held && i < u->pool.count; ++i)
        if (held(ctx, &u->pool.items[i]->addr)) ble_pool_hold(&u->pool, &u->pool.items[i]->addr, now);
    size_t cap = u->pool.count ? u->pool.count : 1, n = 0;
    ble_link **out = calloc(cap, sizeof *out);
    if (out) n = ble_pool_unproven(&u->pool, now, timeout, out, cap);
    for (size_t i = 0; i < n; ++i) conn_drop_locked(conn_of(out[i]), true);
    nsync_mu_unlock(&u->mu);
    free(out);
    if (n) FIPSD_DEBUG(MOD, "%s: closed %zu channel(s) with no handshake within %llu ms", u->name, n, (unsigned long long)timeout);
    return n;
}
static const fipsd_transport_ops ops = {.start_async = start_async, .stop = stop,
    .state = fipsd_transport_shared_state, .mtu = mtu, .send = send_frame, .discover = discover,
    .connect_async = connect_async, .close_connection = close_connection, .network_changed = network_changed,
    .peer_link = peer_link, .close_unproven = fipsd_ble_close_unproven};

/* ---- construction ------------------------------------------------------------- */
void fipsd_ble_config_default(fipsd_ble_config *c)
{
    *c = (fipsd_ble_config){.adapter = "hci0", .psm = FIPSD_BLE_DEFAULT_PSM, .mtu = 2048, .max_connections = 7,
        .connect_timeout_ms = 10000, .advertise = true, .scan = true, .auto_connect = false,
        .accept_connections = true, .probe_cooldown_ms = 30000, .pubkey_exchange_ms = 5000,
        .send_queue_depth = FIPSD_BLE_SEND_DEPTH};
}
uint64_t fipsd_ble_verify_grace_ms(const fipsd_ble_config *c)
{ return c->connect_timeout_ms > BLE_MIN_VERIFY_GRACE_MS ? c->connect_timeout_ms : BLE_MIN_VERIFY_GRACE_MS; }
fipsd_transport *fipsd_ble_create(fips_transport_id id, const char *name, const fipsd_ble_config *c, fipsd_ble_backend b)
{
    bool valid = name && *name && strlen(name) < 128 && c && c->adapter && *c->adapter &&
        strlen(c->adapter) < FIPSD_BLE_ADAPTER_MAX && !strchr(c->adapter, '/') && c->mtu && c->connect_timeout_ms &&
        c->probe_cooldown_ms && c->pubkey_exchange_ms && c->send_queue_depth && c->send_queue_depth <= 64 &&
        fipsd_ble_backend_valid(&b);
    ble_instance *u = valid ? calloc(1, sizeof *u) : NULL;
    if (!u) { if (fipsd_ble_backend_valid(&b)) b.ops->destroy(b.self); return NULL; }
    nsync_mu_init(&u->mu); u->config = *c; u->backend = b;
    strcpy(u->name, name); strcpy(u->adapter, c->adapter); u->config.adapter = u->adapter;
    u->transport = (fipsd_transport){.ops = &ops, .id = id, .type = "ble", .name = u->name,
        .auto_connect = c->auto_connect, .accept_connections = c->accept_connections, .user = u};
    fipsd_transport_shared_init(&u->transport, &u->shared, true);
    u->stop = go_channel_create(1); u->bound = go_channel_create(1); u->changed = go_channel_create(1);
    u->adverts = go_channel_create(BLE_PROBE_QUEUE);
    u->sink.events = go_channel_create(BLE_EVENT_QUEUE); u->sink.adapter_wake = go_channel_create(1);
    size_t memory = BLE_MAX_RECALLED + BLE_MAX_PENDING_PROBES + c->max_connections + 1;
    ble_probes_init(&u->probes, c->probe_cooldown_ms);
    if (!u->stop || !u->bound || !u->changed || !u->adverts || !u->sink.events || !u->sink.adapter_wake ||
        ble_pool_init(&u->pool, c->max_connections, fipsd_ble_verify_grace_ms(c)) ||
        ble_recall_init(&u->learned_psm, memory) || ble_recall_init(&u->known_node, memory)) {
        fipsd_ble_destroy(&u->transport); return NULL;
    }
    return &u->transport;
}
void fipsd_ble_destroy(fipsd_transport *t)
{
    if (!t) return;
    ble_instance *u = t->user;
    atomic_store(&u->sink.open, false);
    u->backend.ops->destroy(u->backend.self); /* joins its threads: no more sink calls */
    void *v;
    if (u->sink.events) while (go_channel_try_receive(u->sink.events, &v) == 0) event_discard(v);
    if (u->adverts) while (go_channel_try_receive(u->adverts, &v) == 0) free(v);
    while (u->conns) { ble_conn *c = u->conns; u->conns = c->next; if (c->stream) ble_io_close(c->stream, c->fd); conn_free(c); }
    while (u->exchanges) { ble_exchange *x = u->exchanges; u->exchanges = x->next; free(x); }
    while (u->dials) { ble_dial *d = u->dials; u->dials = d->next; free(d); }
    ble_pool_free(&u->pool); ble_recall_free(&u->learned_psm); ble_recall_free(&u->known_node);
    GoChannel *ch[] = {u->stop, u->bound, u->changed, u->adverts, u->sink.events, u->sink.adapter_wake};
    for (size_t i = 0; i < sizeof ch / sizeof ch[0]; ++i) if (ch[i]) go_channel_unref(ch[i]);
    free(u);
}
bool fipsd_ble_is(const fipsd_transport *t) { return t && t->ops == &ops; }
GoChannel *fipsd_ble_bound(fipsd_transport *t) { return ((ble_instance *)t->user)->bound; }
size_t fipsd_ble_connected_count(fipsd_transport *t)
{ ble_instance *u = t->user; nsync_mu_lock(&u->mu); size_t n = u->pool.count; nsync_mu_unlock(&u->mu); return n; }
uint16_t fipsd_ble_listener_psm(fipsd_transport *t)
{ ble_instance *u = t->user; nsync_mu_lock(&u->mu); uint16_t p = u->listener_psm; nsync_mu_unlock(&u->mu); return p; }
uint64_t fipsd_ble_event_drops(fipsd_transport *t) { return atomic_load(&((ble_instance *)t->user)->sink.dropped); }
int fipsd_ble_is_verified(fipsd_transport *t, const fips_transport_addr *a)
{
    ble_instance *u = t->user;
    nsync_mu_lock(&u->mu); ble_link *l = ble_pool_get(&u->pool, a); int v = l ? l->verified : -1; nsync_mu_unlock(&u->mu);
    return v;
}
/* This build's platform backend: BlueZ on Linux, the per-user fips-ble-agent
 * on macOS. The simulated radio is never a production fallback, since a
 * transport over it would start, report itself up and never peer (upstream
 * DefaultBleTransport tripwire). */
bool fipsd_ble_platform_backend(const fipsd_ble_config *c, fipsd_ble_backend *out, const char **reason)
{
#if defined(FIPSD_ENABLE_BLE_BLUEZ)
    return fipsd_ble_bluez_backend(c, out, reason);
#elif defined(FIPSD_BLE_COREBLUETOOTH)
    (void)c;
    /* macOS: the radio is the per-user fips-ble-agent's, lent over its socket. */
    if (fipsd_ble_agent_backend(NULL, out)) return true;
    *reason = "the fips-ble-agent socket backend could not be set up";
    return false;
#else
    (void)c; (void)out;
    *reason = "this build has no Bluetooth backend (BlueZ on Linux: nostrc-gfi8h.6.2, CoreBluetooth on macOS: nostrc-gfi8h.6.3)";
    return false;
#endif
}
int fipsd_ble_register_config(fipsd_transport_registry *r, const fipsd_config *config, fipsd_transport **owned, size_t *count)
{
    const fipsd_config_value *ble = fipsd_config_get(config, "transports.ble");
    if (!ble) return 0;
    bool single = fipsd_config_member(ble, "adapter") != NULL;
    size_t n = single ? 1 : fipsd_config_count(ble);
    for (size_t i = 0; i < n; ++i) {
        if (*count == FIPSD_TRANSPORT_MAX) return -1;
        const char *name = single ? "ble" : fipsd_config_key(ble, i);
        const fipsd_config_value *v = single ? ble : fipsd_config_member(ble, name);
        fipsd_ble_config c; fipsd_ble_config_default(&c);
        c.adapter = fipsd_config_string(fipsd_config_member(v, "adapter"));
        c.psm = (uint16_t)fipsd_config_uint(fipsd_config_member(v, "psm"));
        c.mtu = (uint16_t)fipsd_config_uint(fipsd_config_member(v, "mtu"));
        c.max_connections = (size_t)fipsd_config_uint(fipsd_config_member(v, "max_connections"));
        uint64_t timeout = fipsd_config_uint(fipsd_config_member(v, "connect_timeout_ms"));
        uint64_t cooldown = fipsd_config_uint(fipsd_config_member(v, "probe_cooldown_secs"));
        if (timeout > UINT32_MAX || cooldown > UINT64_MAX / 1000) return -1;
        c.connect_timeout_ms = (uint32_t)timeout; c.probe_cooldown_ms = cooldown * 1000;
        c.advertise = fipsd_config_bool(fipsd_config_member(v, "advertise"));
        c.scan = fipsd_config_bool(fipsd_config_member(v, "scan"));
        c.auto_connect = fipsd_config_bool(fipsd_config_member(v, "auto_connect"));
        c.accept_connections = fipsd_config_bool(fipsd_config_member(v, "accept_connections"));
        fipsd_ble_backend b; const char *reason = NULL;
        if (!fipsd_ble_platform_backend(&c, &b, &reason)) {
            FIPSD_WARN(MOD, "instance %s: BLE transport unavailable (%s); ignoring configured instance", name, reason);
            continue;
        }
        fipsd_transport *t = fipsd_ble_create((fips_transport_id){(uint32_t)r->count + 1}, name, &c, b);
        if (!t) return -1;
        if (fipsd_transport_register(r, t)) { fipsd_ble_destroy(t); return -1; }
        owned[(*count)++] = t;
    }
    return 0;
}
