/* BLE agent socket (nostrc-gfi8h.6.3). Three layers:
 *   A. the agent protocol against upstream's own encoder: the vectors below
 *      were printed by the pinned upstream agent_proto.rs, compiled verbatim
 *      (Frame::encode of upstream's every_frame() test list);
 *   B. fipsd's backend (agent_client.c) against a scripted agent on its
 *      socket: hello, BUSY, silent and foreign connections, commands,
 *      adverts, dials, streams with credit, CLOSED, cancellation, the agent
 *      leaving and stop;
 *   C. end to end: two BLE transports over their backends, each lent a radio
 *      by the real agent session (src/ble-agent/session.c) over a mock radio,
 *      linking, carrying frames and surviving a radio power cycle on a new PSM.
 * Every wait is a bounded select on an event or a socket read, never a sleep. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "transport/ble/internal.h"
#include "transport/ble/agent_client.h"
#include "transport/ble/agent_proto.h"
#include "session.h"
#include "host_os.h"
#include "test_tmpdir.h"
#include <fips/identity/local.h>
#include <fips/identity/node_addr.h>
#include <fips/identity/peer.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define WAIT_MS 15000u
static const char *tmp;
static fipsd_runtime *rt;
static fips_secp *secp;

static void *take(GoChannel *c, uint64_t ms)
{
    void *v = NULL; GoSelectCase k = {GO_SELECT_RECEIVE, c, NULL, &v};
    return go_select_timeout(&k, 1, ms).selected_case == 0 ? v : NULL;
}

/* ======================================================================
 * A. the protocol
 * ====================================================================== */
typedef struct vector { fipsd_ble_agent_frame f; const uint8_t *wire; size_t len; } vector;
#define W(...) (const uint8_t[]){__VA_ARGS__}, sizeof((const uint8_t[]){__VA_ARGS__})
static const uint8_t OUT[] = {'o', 'u', 't'}, A5[] = {0xA5, 0xA5, 0xA5, 0xA5};
static const vector vectors[] = {
    {{.tag = FIPSD_BLE_AGENT_CONNECT, .id = -7, .addr = {2, 1, 2, 3, 4, 1}, .psm = 0x00C1},
        W(0x00, 0x00, 0x00, 0x11, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xF9, 0x02, 0x01, 0x02, 0x03, 0x04, 0x01, 0x00, 0xC1)},
    {{.tag = FIPSD_BLE_AGENT_START_ADVERTISING, .psm = 0x0085}, W(0x00, 0x00, 0x00, 0x03, 0x02, 0x00, 0x85)},
    {{.tag = FIPSD_BLE_AGENT_STOP_ADVERTISING}, W(0x00, 0x00, 0x00, 0x01, 0x03)},
    {{.tag = FIPSD_BLE_AGENT_START_SCANNING}, W(0x00, 0x00, 0x00, 0x01, 0x04)},
    {{.tag = FIPSD_BLE_AGENT_STOP_SCANNING}, W(0x00, 0x00, 0x00, 0x01, 0x05)},
    {{.tag = FIPSD_BLE_AGENT_CLOSE, .ch = 9}, W(0x00, 0x00, 0x00, 0x05, 0x06, 0x00, 0x00, 0x00, 0x09)},
    {{.tag = FIPSD_BLE_AGENT_SEND, .ch = 9, .data = OUT, .len = 3},
        W(0x00, 0x00, 0x00, 0x08, 0x07, 0x00, 0x00, 0x00, 0x09, 0x6F, 0x75, 0x74)},
    {{.tag = FIPSD_BLE_AGENT_SEND, .ch = 9}, W(0x00, 0x00, 0x00, 0x05, 0x07, 0x00, 0x00, 0x00, 0x09)},
    {{.tag = FIPSD_BLE_AGENT_BUSY}, W(0x00, 0x00, 0x00, 0x01, 0x08)},
    {{.tag = FIPSD_BLE_AGENT_HELLO, .version = 2, .psm = 0x00C0}, W(0x00, 0x00, 0x00, 0x04, 0x10, 0x02, 0x00, 0xC0)},
    {{.tag = FIPSD_BLE_AGENT_INBOUND, .ch = 3, .addr = {2, 1, 2, 3, 4, 2}, .mtu = 2048},
        W(0x00, 0x00, 0x00, 0x0D, 0x11, 0x00, 0x00, 0x00, 0x03, 0x02, 0x01, 0x02, 0x03, 0x04, 0x02, 0x08, 0x00)},
    {{.tag = FIPSD_BLE_AGENT_CONNECT_RESULT, .id = 42, .ok = true, .ch = 4, .addr = {2, 1, 2, 3, 4, 3}, .mtu = 512},
        W(0x00, 0x00, 0x00, 0x16, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2A, 0x01, 0x00, 0x00, 0x00, 0x04,
          0x02, 0x01, 0x02, 0x03, 0x04, 0x03, 0x02, 0x00)},
    {{.tag = FIPSD_BLE_AGENT_CONNECT_RESULT, .id = 43, .addr = {2, 1, 2, 3, 4, 3}},
        W(0x00, 0x00, 0x00, 0x16, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2B, 0x00, 0x00, 0x00, 0x00, 0x00,
          0x02, 0x01, 0x02, 0x03, 0x04, 0x03, 0x00, 0x00)},
    {{.tag = FIPSD_BLE_AGENT_SCAN, .addr = {2, 1, 2, 3, 4, 4}, .psm = 0x0080, .rssi = -71},
        W(0x00, 0x00, 0x00, 0x0B, 0x13, 0x02, 0x01, 0x02, 0x03, 0x04, 0x04, 0x00, 0x80, 0xFF, 0xB9)},
    {{.tag = FIPSD_BLE_AGENT_SCAN, .addr = {2, 1, 2, 3, 4, 4}, .psm = FIPSD_BLE_AGENT_NO_PSM, .rssi = FIPSD_BLE_AGENT_NO_RSSI},
        W(0x00, 0x00, 0x00, 0x0B, 0x13, 0x02, 0x01, 0x02, 0x03, 0x04, 0x04, 0x00, 0x00, 0x80, 0x00)},
    {{.tag = FIPSD_BLE_AGENT_RECV, .ch = 3, .data = A5, .len = 4},
        W(0x00, 0x00, 0x00, 0x09, 0x14, 0x00, 0x00, 0x00, 0x03, 0xA5, 0xA5, 0xA5, 0xA5)},
    {{.tag = FIPSD_BLE_AGENT_CLOSED, .ch = 3}, W(0x00, 0x00, 0x00, 0x05, 0x15, 0x00, 0x00, 0x00, 0x03)},
    {{.tag = FIPSD_BLE_AGENT_CREDIT, .ch = 3, .bytes = 32 * 1024},
        W(0x00, 0x00, 0x00, 0x09, 0x16, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x80, 0x00)},
};
#define NVECTORS (sizeof vectors / sizeof vectors[0])
static bool same(const fipsd_ble_agent_frame *a, const fipsd_ble_agent_frame *b)
{
    return a->tag == b->tag && a->id == b->id && a->ch == b->ch && a->bytes == b->bytes && a->psm == b->psm &&
        a->mtu == b->mtu && a->rssi == b->rssi && a->version == b->version && a->ok == b->ok &&
        !memcmp(a->addr, b->addr, 6) && a->len == b->len && (!a->len || !memcmp(a->data, b->data, a->len));
}
static void protocol_vectors(void)
{
    assert(FIPSD_BLE_AGENT_VERSION == 2 && FIPSD_BLE_AGENT_MAX_FRAME == 131072 && FIPSD_BLE_AGENT_MAX_HELLO_FRAME == 16);
    uint8_t buf[256], stream[1024];
    size_t total = 0;
    for (size_t i = 0; i < NVECTORS; ++i) {
        const vector *v = &vectors[i];
        assert(fipsd_ble_agent_encoded_len(&v->f) == v->len);
        assert(fipsd_ble_agent_encode(&v->f, buf, sizeof buf) == v->len && !memcmp(buf, v->wire, v->len));
        size_t len = 0; fipsd_ble_agent_frame f;
        assert(fipsd_ble_agent_frame_len(v->wire, FIPSD_BLE_AGENT_MAX_FRAME, &len) && len == v->len - 4);
        assert(fipsd_ble_agent_decode(v->wire + 4, len, &f) && same(&f, &v->f));
        assert(fipsd_ble_agent_encode(&v->f, buf, v->len - 1) == 0); /* short buffer */
        memcpy(stream + total, v->wire, v->len); total += v->len;
    }
    /* a stream of frames reads back in order, to a clean end */
    size_t at = 0;
    for (size_t i = 0; i < NVECTORS; ++i) {
        size_t len = 0; fipsd_ble_agent_frame f;
        assert(fipsd_ble_agent_frame_len(stream + at, FIPSD_BLE_AGENT_MAX_FRAME, &len));
        assert(fipsd_ble_agent_decode(stream + at + 4, len, &f) && same(&f, &vectors[i].f));
        at += 4 + len;
    }
    assert(at == total);
    /* truncated, empty and unknown frames are rejected; trailing bytes are not (upstream Reader) */
    fipsd_ble_agent_frame f;
    assert(!fipsd_ble_agent_decode((const uint8_t[]){0}, 0, &f));
    assert(!fipsd_ble_agent_decode((const uint8_t[]){FIPSD_BLE_AGENT_CLOSE, 0, 0}, 3, &f));
    assert(!fipsd_ble_agent_decode((const uint8_t[]){0xEE}, 1, &f));
    assert(!fipsd_ble_agent_decode((const uint8_t[]){FIPSD_BLE_AGENT_CREDIT, 0, 0, 0, 3, 0, 0}, 7, &f));
    assert(fipsd_ble_agent_decode((const uint8_t[]){FIPSD_BLE_AGENT_CLOSE, 0, 0, 0, 7, 0xEE}, 6, &f) && f.ch == 7);
    /* lengths: oversized refused before allocating, empty refused, a first frame longer than a hello refused */
    size_t len = 0;
    assert(!fipsd_ble_agent_frame_len((const uint8_t[]){0xFF, 0xFF, 0xFF, 0xFF}, FIPSD_BLE_AGENT_MAX_FRAME, &len));
    assert(!fipsd_ble_agent_frame_len((const uint8_t[]){0, 0, 0, 0}, FIPSD_BLE_AGENT_MAX_FRAME, &len));
    assert(!fipsd_ble_agent_frame_len((const uint8_t[]){0, 0, 0, 0x45}, FIPSD_BLE_AGENT_MAX_HELLO_FRAME, &len));
    assert(vectors[9].len - 4 <= FIPSD_BLE_AGENT_MAX_HELLO_FRAME);
    assert(fipsd_ble_agent_frame_len((const uint8_t[]){0, 2, 0, 0}, FIPSD_BLE_AGENT_MAX_FRAME, &len) && len == 131072);
    /* the encoder never builds a frame the reader refuses */
    static uint8_t big[FIPSD_BLE_AGENT_MAX_FRAME];
    fipsd_ble_agent_frame huge = {.tag = FIPSD_BLE_AGENT_SEND, .ch = 1, .data = big, .len = FIPSD_BLE_AGENT_MAX_FRAME - 5};
    assert(fipsd_ble_agent_encoded_len(&huge) == 4 + FIPSD_BLE_AGENT_MAX_FRAME);
    huge.len++; assert(!fipsd_ble_agent_encoded_len(&huge));
    assert(!fipsd_ble_agent_encoded_len(&(fipsd_ble_agent_frame){.tag = (fipsd_ble_agent_tag)99}));
    assert(!fipsd_ble_agent_encoded_len(&(fipsd_ble_agent_frame){.tag = FIPSD_BLE_AGENT_RECV, .len = 3}));
    /* the identifier mapping is upstream peer_addr, pinned, and the core's */
    uint8_t id[16], d[6];
    for (int i = 0; i < 16; ++i) id[i] = (uint8_t)i;
    fipsd_ble_agent_device_from_identifier(id, d);
    assert(!memcmp(d, (const uint8_t[]){0x7E, 0x84, 0xDC, 0x94, 0x77, 0x85}, 6));
    for (int n = 0; n < 64; ++n) {
        for (int i = 0; i < 16; ++i) id[i] = (uint8_t)(n * 37 + i * 11);
        fipsd_ble_agent_device_from_identifier(id, d);
        fipsd_ble_peer p = fipsd_ble_peer_from_identifier(id);
        assert(!memcmp(d, p.device, 6) && (d[0] & 2) && !(d[0] & 1));
    }
    puts("protocol: 18 upstream encoder vectors byte for byte, frame stream, truncated/unknown/oversized/hello-limit refused, identifier mapping: ok");
}

/* ======================================================================
 * Shared harness: a runtime, scenarios on a fiber
 * ====================================================================== */
typedef struct rig {
    fipsd_ble_backend b;
    fipsd_ble_sink sink;
    char path[128];
    bool started;
} rig;
typedef struct tnode {
    fipsd_transport *t;
    void *backend; /* the agent backend's self */
    GoChannel *events;
    fips_identity *id;
    fips_xonly_pubkey key;
    fips_node_addr node;
    fips_transport_addr addr;
    char path[128];
} tnode;
static rig *rigs[8]; static size_t nrigs;
static tnode *nodes[4]; static size_t nnodes;
static fipsd_ble_stream *leftovers[8]; static size_t nleft; /* closed only after destroy */
static void (*after_join)(void);
static int no_start(fipsd_subsystem *s, fipsd_runtime *r) { (void)s; (void)r; return 0; }
static void stop_all(fipsd_subsystem *s, fipsd_runtime *r)
{
    (void)s; (void)r;
    for (size_t i = 0; i < nrigs; ++i) rigs[i]->b.ops->stop(rigs[i]->b.self);
    for (size_t i = 0; i < nnodes; ++i) nodes[i]->t->ops->stop(nodes[i]->t);
}
typedef struct body { void (*fn)(void); GoChannel *done; } body;
static void trampoline(void *arg) { body *b = arg; b->fn(); assert(go_channel_send(b->done, b) == 0); }
static void discard(fipsd_ble_event *e)
{
    if (e && e->stream) { e->stream->ops->cancel(e->stream); e->stream->ops->close(e->stream); }
    free(e);
}
static void scenario(void (*fn)(void))
{
    fipsd_runtime_config rc; fipsd_runtime_config_default(&rc); rc.workers = 2; rc.handle_signals = false; rc.grace_ms = 10000;
    rt = fipsd_runtime_create(&rc); assert(rt);
    fipsd_subsystem sub = {.name = "ble-agent-test", .start = no_start, .stop = stop_all};
    assert(!fipsd_runtime_register(rt, &sub) && !fipsd_runtime_start(rt));
    body b = {fn, go_channel_create(1)};
    assert(fipsd_runtime_spawn(rt, trampoline, &b) == 0);
    assert(take(b.done, 120000) == &b);
    fipsd_runtime_shutdown(rt); assert(fipsd_runtime_run(rt) == FIPSD_RUN_OK);
    for (size_t i = 0; i < nrigs; ++i) {
        rig *r = rigs[i]; void *v;
        r->b.ops->destroy(r->b.self);
        while (go_channel_try_receive(r->sink.events, &v) == 0) discard(v);
        go_channel_unref(r->sink.events); go_channel_unref(r->sink.adapter_wake); free(r);
    }
    /* streams the core may close after its backend was destroyed */
    for (size_t i = 0; i < nleft; ++i) { leftovers[i]->ops->cancel(leftovers[i]); leftovers[i]->ops->close(leftovers[i]); }
    for (size_t i = 0; i < nnodes; ++i) {
        tnode *n = nodes[i]; void *v;
        fipsd_ble_destroy(n->t);
        while (go_channel_try_receive(n->events, &v) == 0) free(v);
        go_channel_unref(n->events); fips_identity_destroy(n->id); free(n);
    }
    if (after_join) after_join();
    nrigs = nnodes = nleft = 0; after_join = NULL;
    fipsd_runtime_destroy(rt); rt = NULL;
    go_channel_unref(b.done);
}

/* ======================================================================
 * B. the backend against a scripted agent
 * ====================================================================== */
static rig *rig_new(const char *name, uint32_t hello_ms)
{
    rig *r = calloc(1, sizeof *r); assert(r);
    snprintf(r->path, sizeof r->path, "%s/%s.sock", tmp, name);
    fipsd_ble_agent_config c = {.socket_path = r->path, .group = "", .hello_timeout_ms = hello_ms};
    assert(fipsd_ble_agent_backend(&c, &r->b) && fipsd_ble_backend_valid(&r->b));
    assert(!strcmp(fipsd_ble_agent_socket(r->b.self), r->path));
    r->sink.events = go_channel_create(512); r->sink.adapter_wake = go_channel_create(1); atomic_store(&r->sink.open, true);
    assert(r->b.ops->start(r->b.self, rt, &r->sink) == 0);
    rigs[nrigs++] = r;
    return r;
}
static bool adapter_is(rig *r, bool up)
{ unsigned a = atomic_load(&r->sink.adapter); return (a & 4) && ((a & 3) == 3) == up && !(a & 3) == !up; }
static void wait_adapter(rig *r, bool up)
{
    const uint64_t deadline = ble_now() + WAIT_MS;
    while (!adapter_is(r, up)) {
        uint64_t now = ble_now(); assert(now < deadline);
        (void)take(r->sink.adapter_wake, deadline - now);
    }
    fipsd_ble_adapter a = r->b.ops->adapter(r->b.self);
    assert(a.present == up && a.powered == up);
}
static fipsd_ble_event *next_event(rig *r, fipsd_ble_event_kind kind)
{
    fipsd_ble_event *e = take(r->sink.events, WAIT_MS);
    if (!e || e->kind != kind) fprintf(stderr, "expected event %d, got %d\n", (int)kind, e ? (int)e->kind : -1);
    assert(e && e->kind == kind);
    return e;
}
/* Until the backend's stats satisfy `ok`, woken by its change channel. */
static fipsd_ble_agent_stats await_stats(rig *r, bool (*ok)(const fipsd_ble_agent_stats *, uint64_t), uint64_t arg)
{
    const uint64_t deadline = ble_now() + WAIT_MS;
    for (;;) {
        fipsd_ble_agent_stats s = fipsd_ble_agent_stats_read(r->b.self);
        if (ok(&s, arg)) return s;
        uint64_t now = ble_now(); assert(now < deadline);
        (void)take(fipsd_ble_agent_changed(r->b.self), deadline - now);
    }
}
static bool busy_is(const fipsd_ble_agent_stats *s, uint64_t n) { return s->refused_busy == n; }
static bool hello_refused_is(const fipsd_ble_agent_stats *s, uint64_t n) { return s->refused_hello == n; }
static bool channels_is(const fipsd_ble_agent_stats *s, uint64_t n) { return s->channels == n; }

/* The scripted agent: blocking socket I/O on its own threads, frames handed
 * to the test fiber through channels. */
typedef struct rx { fipsd_ble_agent_frame f; uint8_t data[]; } rx;
typedef struct wbuf { size_t len; uint8_t bytes[]; } wbuf;
typedef struct fake { int fd; pthread_t reader, writer; GoChannel *in, *out, *eof; } fake;
static int full(int fd, uint8_t *p, size_t n, bool reading)
{
    while (n) {
        ssize_t k = reading ? recv(fd, p, n, 0) : send(fd, p, n, 0);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -1;
        p += k; n -= (size_t)k;
    }
    return 0;
}
static void *fake_reader(void *arg)
{
    fake *k = arg;
    uint8_t *body = malloc(FIPSD_BLE_AGENT_MAX_FRAME); assert(body);
    for (;;) {
        uint8_t h[4]; size_t len = 0; fipsd_ble_agent_frame f;
        if (full(k->fd, h, 4, true) || !fipsd_ble_agent_frame_len(h, FIPSD_BLE_AGENT_MAX_FRAME, &len) ||
            full(k->fd, body, len, true) || !fipsd_ble_agent_decode(body, len, &f)) break;
        rx *m = malloc(sizeof *m + f.len); assert(m);
        m->f = f; if (f.len) memcpy(m->data, f.data, f.len); m->f.data = m->data;
        if (go_channel_send(k->in, m)) { free(m); break; }
    }
    free(body);
    go_channel_close(k->eof);
    return NULL;
}
static void *fake_writer(void *arg)
{
    fake *k = arg; void *v = NULL;
    while (go_channel_receive(k->out, &v) == 0 && v) {
        wbuf *w = v; int rc = full(k->fd, w->bytes, w->len, false); free(w);
        if (rc) break;
    }
    return NULL;
}
static fake *fake_connect(const char *path)
{
    fake *k = calloc(1, sizeof *k); assert(k);
    struct sockaddr_un a = {.sun_family = AF_UNIX}; strcpy(a.sun_path, path);
    k->fd = socket(AF_UNIX, SOCK_STREAM, 0); assert(k->fd >= 0);
#ifdef SO_NOSIGPIPE
    int on = 1; (void)setsockopt(k->fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
    assert(connect(k->fd, (struct sockaddr *)&a, sizeof a) == 0);
    k->in = go_channel_create(4096); k->out = go_channel_create(4096); k->eof = go_channel_create(1);
    assert(!pthread_create(&k->reader, NULL, fake_reader, k) && !pthread_create(&k->writer, NULL, fake_writer, k));
    return k;
}
static void fake_send(fake *k, fipsd_ble_agent_frame f)
{
    size_t n = fipsd_ble_agent_encoded_len(&f); assert(n);
    wbuf *w = malloc(sizeof *w + n); assert(w);
    w->len = fipsd_ble_agent_encode(&f, w->bytes, n);
    assert(go_channel_send(k->out, w) == 0);
}
static void fake_raw(fake *k, const uint8_t *p, size_t n)
{ wbuf *w = malloc(sizeof *w + n); assert(w); w->len = n; memcpy(w->bytes, p, n); assert(go_channel_send(k->out, w) == 0); }
static void hello(fake *k, uint16_t psm) { fake_send(k, (fipsd_ble_agent_frame){.tag = FIPSD_BLE_AGENT_HELLO, .version = 2, .psm = psm}); }
static rx *expect(fake *k, fipsd_ble_agent_tag tag)
{
    rx *m = take(k->in, WAIT_MS);
    if (!m || m->f.tag != tag) fprintf(stderr, "expected frame %d, got %d\n", (int)tag, m ? (int)m->f.tag : -1);
    assert(m && m->f.tag == tag);
    return m;
}
static void expect_free(fake *k, fipsd_ble_agent_tag tag) { free(expect(k, tag)); }
/* The daemon hangs up: anything it sent first is discarded, then the end. */
static void expect_eof(fake *k)
{
    const uint64_t deadline = ble_now() + WAIT_MS;
    for (;;) {
        void *v = NULL;
        if (go_channel_try_receive(k->in, &v) == 0) { free(v); continue; }
        if (go_channel_is_closed(k->eof)) return;
        GoSelectCase cases[] = {{GO_SELECT_RECEIVE, k->in, NULL, &v}, {GO_SELECT_RECEIVE, k->eof, NULL, &v}};
        uint64_t now = ble_now(); assert(now < deadline);
        GoSelectResult r = go_select_timeout(cases, 2, deadline - now);
        assert(r.selected_case >= 0);
        if (r.selected_case == 0) free(v);
    }
}
static void fake_close(fake *k)
{
    (void)shutdown(k->fd, SHUT_RDWR);
    go_channel_close(k->out);
    pthread_join(k->writer, NULL); pthread_join(k->reader, NULL);
    void *v;
    while (go_channel_try_receive(k->in, &v) == 0) free(v);
    while (go_channel_try_receive(k->out, &v) == 0) free(v);
    close(k->fd);
    go_channel_unref(k->in); go_channel_unref(k->out); go_channel_unref(k->eof);
    free(k);
}
static fipsd_ble_peer peer_n(uint8_t n) { return (fipsd_ble_peer){FIPSD_BLE_RADIO_ADAPTER, {0x02, 0, 0, 0, 0x0A, n}}; }

/* Parked operations, each on a fiber of its own. */
typedef struct op {
    rig *r;
    fipsd_ble_peer peer;
    uint16_t psm;
    uint64_t deadline;
    fipsd_ble_stream *s;
    uint8_t buf[64];
    ssize_t rc;
    GoChannel *done;
} op;
static void dial_fiber(void *arg)
{
    op *o = arg;
    o->rc = o->r->b.ops->connect(o->r->b.self, &o->peer, o->psm, o->deadline, &o->s);
    assert(go_channel_send(o->done, o) == 0);
}
static void read_fiber(void *arg)
{
    op *o = arg;
    o->rc = o->s->ops->read(o->s, o->buf, sizeof o->buf, o->deadline);
    assert(go_channel_send(o->done, o) == 0);
}
static op *spawn_op(void (*fn)(void *), rig *r, uint8_t n, uint16_t psm, fipsd_ble_stream *s, uint64_t ms)
{
    op *o = calloc(1, sizeof *o); assert(o);
    o->r = r; o->peer = peer_n(n); o->psm = psm; o->s = s; o->done = go_channel_create(1);
    o->deadline = ms == UINT64_MAX ? FIPSD_BLE_NO_DEADLINE : ble_now() + ms;
    assert(fipsd_runtime_spawn(rt, fn, o) == 0);
    return o;
}
static ssize_t op_wait(op *o)
{
    assert(take(o->done, WAIT_MS) == o);
    ssize_t rc = o->rc; go_channel_unref(o->done);
    return rc;
}
/* A dial the scripted agent answers: `ch` 0 refuses. */
static fipsd_ble_stream *dial(rig *r, fake *k, uint8_t n, uint16_t psm, uint32_t ch, uint16_t mtu, uint32_t credit, int *rc)
{
    op *o = spawn_op(dial_fiber, r, n, psm, NULL, WAIT_MS);
    rx *m = expect(k, FIPSD_BLE_AGENT_CONNECT);
    assert(m->f.psm == psm && !memcmp(m->f.addr, o->peer.device, 6));
    fake_send(k, (fipsd_ble_agent_frame){.tag = FIPSD_BLE_AGENT_CONNECT_RESULT, .id = m->f.id, .ok = ch != 0, .ch = ch,
        .addr = {0x02, 0, 0, 0, 0x0A, n}, .mtu = mtu});
    if (ch && credit) fake_send(k, (fipsd_ble_agent_frame){.tag = FIPSD_BLE_AGENT_CREDIT, .ch = ch, .bytes = credit});
    free(m);
    *rc = (int)op_wait(o);
    fipsd_ble_stream *s = o->s; free(o);
    return s;
}
static void recv_frame(fake *k, uint32_t ch, const void *p, size_t n)
{ fake_send(k, (fipsd_ble_agent_frame){.tag = FIPSD_BLE_AGENT_RECV, .ch = ch, .data = p, .len = n}); }
static void read_exact(fipsd_ble_stream *s, const void *want, size_t n)
{
    uint8_t got[4096]; size_t have = 0;
    while (have < n) {
        ssize_t k = s->ops->read(s, got + have, n - have, ble_now() + WAIT_MS);
        assert(k > 0); have += (size_t)k;
    }
    assert(!memcmp(got, want, n));
}

static void lifecycle_body(void)
{
    rig *r = rig_new("life", 300);
    wait_adapter(r, false); /* reported at once: no agent yet */
    uint16_t bound = 0; fipsd_ble_stream *s = NULL; fipsd_ble_advert ad = {.psm = 0x00C3};
    assert(r->b.ops->listen(r->b.self, 0x0085, ble_now() + WAIT_MS, &bound) == -ENODEV);
    assert(r->b.ops->advertise(r->b.self, &ad, ble_now() + WAIT_MS) == -ENODEV && r->b.ops->scan_start(r->b.self, ble_now() + WAIT_MS) == -ENODEV);
    assert(r->b.ops->advertise_stop(r->b.self) == 0 && r->b.ops->scan_stop(r->b.self) == 0);
    fipsd_ble_peer p = peer_n(1);
    assert(r->b.ops->connect(r->b.self, &p, 0x85, ble_now() + WAIT_MS, &s) == -ENODEV);
    assert(!r->b.ops->read_psm && !strcmp(r->b.ops->adapter_name(r->b.self), "ble0") && !r->b.ops->local_addr(r->b.self, &p));
    /* the agent says hello: the radio is up on its listener's PSM */
    fake *k = fake_connect(r->path); hello(k, 0x00C3);
    wait_adapter(r, true);
    fipsd_ble_agent_stats st = fipsd_ble_agent_stats_read(r->b.self);
    assert(st.live && st.psm == 0x00C3 && st.sessions == 1);
    assert(r->b.ops->listen(r->b.self, 0x0085, ble_now() + WAIT_MS, &bound) == 0 && bound == 0x00C3);
    /* commands */
    assert(r->b.ops->advertise(r->b.self, &ad, ble_now() + WAIT_MS) == 0);
    rx *m = expect(k, FIPSD_BLE_AGENT_START_ADVERTISING); assert(m->f.psm == 0x00C3); free(m);
    assert(r->b.ops->advertise_stop(r->b.self) == 0); expect_free(k, FIPSD_BLE_AGENT_STOP_ADVERTISING);
    assert(r->b.ops->scan_start(r->b.self, ble_now() + WAIT_MS) == 0); expect_free(k, FIPSD_BLE_AGENT_START_SCANNING);
    assert(r->b.ops->scan_stop(r->b.self) == 0); expect_free(k, FIPSD_BLE_AGENT_STOP_SCANNING);
    /* adverts: a PSM becomes 0x9C90 service data, as from BlueZ; none and no RSSI stay absent */
    fake_send(k, (fipsd_ble_agent_frame){.tag = FIPSD_BLE_AGENT_SCAN, .addr = {2, 0, 0, 0, 0x0A, 7}, .psm = 0x0080, .rssi = -71});
    fipsd_ble_event *e = next_event(r, FIPSD_BLE_PEER_DISCOVERED);
    p = peer_n(7); uint16_t psm = 0;
    assert(fipsd_ble_peer_equal(&e->peer, &p) && e->has_rssi && e->rssi == -71);
    assert(fipsd_ble_decode_psm(e->service_data, e->service_data_len, &psm) && psm == 0x0080 && e->service_data_len == 2); free(e);
    fake_send(k, (fipsd_ble_agent_frame){.tag = FIPSD_BLE_AGENT_SCAN, .addr = {2, 0, 0, 0, 0x0A, 8}, .rssi = FIPSD_BLE_AGENT_NO_RSSI});
    e = next_event(r, FIPSD_BLE_PEER_DISCOVERED); assert(!e->has_rssi && !e->service_data_len && e->peer.device[5] == 8); free(e);
    /* a second agent is told BUSY and dropped; the first keeps the radio */
    fake *second = fake_connect(r->path); hello(second, 0x00C9);
    expect_free(second, FIPSD_BLE_AGENT_BUSY); expect_eof(second); fake_close(second);
    st = await_stats(r, busy_is, 1); assert(st.live && st.psm == 0x00C3 && st.sessions == 1 && adapter_is(r, true));
    /* silent, foreign-version, oversized-first-frame connections never take it */
    fake *silent = fake_connect(r->path); expect_eof(silent); fake_close(silent);
    fake *old = fake_connect(r->path); fake_send(old, (fipsd_ble_agent_frame){.tag = FIPSD_BLE_AGENT_HELLO, .version = 1, .psm = 0x00C1});
    expect_eof(old); fake_close(old);
    fake *big = fake_connect(r->path); fake_raw(big, (const uint8_t[]){0, 0, 0, 64}, 4); expect_eof(big); fake_close(big);
    st = await_stats(r, hello_refused_is, 3); assert(st.live && st.sessions == 1 && adapter_is(r, true));
    /* a malformed frame from the live agent ends its session: the radio goes */
    fake_raw(k, (const uint8_t[]){0, 0, 0, 1, 0xEE}, 5);
    wait_adapter(r, false); expect_eof(k); fake_close(k);
    assert(r->b.ops->listen(r->b.self, 0x0085, ble_now() + WAIT_MS, &bound) == -ENODEV);
    /* a new agent is a replaced radio, on its own PSM */
    k = fake_connect(r->path); hello(k, 0);
    wait_adapter(r, true);
    /* no listener (PSM 0): the request stands in, as upstream RadioIo::listen */
    assert(r->b.ops->listen(r->b.self, 0x0085, ble_now() + WAIT_MS, &bound) == 0 && bound == 0x0085);
    fake_close(k); wait_adapter(r, false);
    assert(fipsd_ble_agent_stats_read(r->b.self).sessions == 2);
    puts("lifecycle: absent until hello, listen = hello PSM (fallback for 0), commands, adverts, BUSY, silent/old/oversized refused, malformed ends, replaced radio: ok");
}

static void streams_body(void)
{
    rig *r = rig_new("streams", 0);
    fake *k = fake_connect(r->path); hello(k, 0x00C1); wait_adapter(r, true);
    /* a dial: CONNECT, then the result and the window */
    int rc = 0;
    fipsd_ble_stream *s = dial(r, k, 3, 0x00C5, 4, 512, 10, &rc);
    assert(rc == 0 && s && s->ops->fd(s) == -1);
    fipsd_ble_peer p3 = peer_n(3); assert(fipsd_ble_peer_equal(&s->peer, &p3));
    uint16_t smtu = 0, rmtu = 0; s->ops->mtu(s, &smtu, &rmtu); assert(smtu == 512 && rmtu == 512);
    await_stats(r, channels_is, 1);
    /* writes spend the credit, a SEND each; with none left they park until CREDIT */
    assert(s->ops->write(s, (const uint8_t *)"0123456789", 10, ble_now() + WAIT_MS) == 0);
    rx *m = expect(k, FIPSD_BLE_AGENT_SEND); assert(m->f.ch == 4 && m->f.len == 10 && !memcmp(m->data, "0123456789", 10)); free(m);
    assert(s->ops->write(s, (const uint8_t *)"abc", 3, ble_now() + 50) == -ETIMEDOUT);
    fake_send(k, (fipsd_ble_agent_frame){.tag = FIPSD_BLE_AGENT_CREDIT, .ch = 4, .bytes = 1});
    /* a packet goes whole once any credit is left (upstream spend_credit) */
    assert(s->ops->write(s, (const uint8_t *)"whole", 5, ble_now() + WAIT_MS) == 0);
    m = expect(k, FIPSD_BLE_AGENT_SEND); assert(m->f.len == 5 && !memcmp(m->data, "whole", 5)); free(m);
    fake_send(k, (fipsd_ble_agent_frame){.tag = FIPSD_BLE_AGENT_CREDIT, .ch = 4, .bytes = 1 << 20});
    static uint8_t bulk[150000];
    for (size_t i = 0; i < sizeof bulk; ++i) bulk[i] = (uint8_t)(i * 7);
    assert(s->ops->write(s, bulk, sizeof bulk, ble_now() + WAIT_MS) == 0); /* split under the frame limit */
    size_t got = 0;
    while (got < sizeof bulk) {
        m = expect(k, FIPSD_BLE_AGENT_SEND);
        assert(m->f.len <= FIPSD_BLE_AGENT_MAX_DATA && !memcmp(m->data, bulk + got, m->f.len)); got += m->f.len; free(m);
    }
    assert(got == sizeof bulk);
    /* RECV bytes read back as a byte stream, coalesced or split */
    recv_frame(k, 4, "hello", 5); read_exact(s, "hello", 5);
    recv_frame(k, 4, "ab", 2); recv_frame(k, 4, "cd", 2); read_exact(s, "abcd", 4);
    uint8_t one[2];
    recv_frame(k, 4, "xyz", 3);
    assert(s->ops->read(s, one, 2, ble_now() + WAIT_MS) == 2 && !memcmp(one, "xy", 2));
    assert(s->ops->read(s, one, 2, ble_now() + WAIT_MS) == 1 && one[0] == 'z');
    assert(s->ops->read(s, one, 2, ble_now() + 50) == -ETIMEDOUT);
    /* the agent ends it: queued bytes first, then EOF; writes fail; close sends nothing */
    recv_frame(k, 4, "last", 4);
    fake_send(k, (fipsd_ble_agent_frame){.tag = FIPSD_BLE_AGENT_CLOSED, .ch = 4});
    read_exact(s, "last", 4);
    assert(s->ops->read(s, one, 2, ble_now() + WAIT_MS) == 0 && s->ops->read(s, one, 2, ble_now() + WAIT_MS) == 0);
    assert(s->ops->write(s, one, 1, ble_now() + WAIT_MS) == -EPIPE);
    await_stats(r, channels_is, 0);
    uint64_t dialled_id = s->id;
    s->ops->cancel(s); s->ops->close(s);
    assert(r->b.ops->scan_stop(r->b.self) == 0); expect_free(k, FIPSD_BLE_AGENT_STOP_SCANNING); /* and not a CLOSE first */
    /* an accepted channel arrives as INBOUND_STREAM; the core closing it sends CLOSE */
    fake_send(k, (fipsd_ble_agent_frame){.tag = FIPSD_BLE_AGENT_INBOUND, .ch = 7, .addr = {2, 0, 0, 0, 0x0A, 9}, .mtu = 2048});
    fake_send(k, (fipsd_ble_agent_frame){.tag = FIPSD_BLE_AGENT_CREDIT, .ch = 7, .bytes = FIPSD_BLE_AGENT_CHANNEL_WINDOW});
    fipsd_ble_event *e = next_event(r, FIPSD_BLE_INBOUND_STREAM);
    fipsd_ble_stream *in = e->stream; e->stream = NULL; free(e);
    fipsd_ble_peer p9 = peer_n(9); assert(fipsd_ble_peer_equal(&in->peer, &p9) && in->id != dialled_id);
    in->ops->mtu(in, &smtu, &rmtu); assert(smtu == 2048 && rmtu == 2048);
    recv_frame(k, 7, "in", 2); read_exact(in, "in", 2);
    assert(in->ops->write(in, (const uint8_t *)"out", 3, ble_now() + WAIT_MS) == 0);
    m = expect(k, FIPSD_BLE_AGENT_SEND); assert(m->f.ch == 7 && m->f.len == 3); free(m);
    /* cancel: a parked read returns -ECANCELED, and so does every later one */
    op *o = spawn_op(read_fiber, r, 0, 0, in, UINT64_MAX);
    in->ops->cancel(in); in->ops->cancel(in);
    assert(op_wait(o) == -ECANCELED); free(o);
    assert(in->ops->write(in, one, 1, ble_now() + WAIT_MS) == -ECANCELED);
    in->ops->close(in);
    m = expect(k, FIPSD_BLE_AGENT_CLOSE); assert(m->f.ch == 7); free(m);
    /* a refused dial carries no reason: -EIO */
    assert(!dial(r, k, 5, 0x00C1, 0, 0, 0, &rc) && rc == -EIO);
    /* an unanswered dial times out (never a refusal); its late answer is closed */
    op *late = spawn_op(dial_fiber, r, 6, 0x00C1, NULL, 100);
    m = expect(k, FIPSD_BLE_AGENT_CONNECT);
    assert(op_wait(late) == -ETIMEDOUT && !late->s); free(late);
    fake_send(k, (fipsd_ble_agent_frame){.tag = FIPSD_BLE_AGENT_CONNECT_RESULT, .id = m->f.id, .ok = true, .ch = 11,
        .addr = {2, 0, 0, 0, 0x0A, 6}, .mtu = 512});
    free(m);
    m = expect(k, FIPSD_BLE_AGENT_CLOSE); assert(m->f.ch == 11); free(m);
    /* frames for channels nobody has are ignored */
    recv_frame(k, 99, "x", 1);
    fake_send(k, (fipsd_ble_agent_frame){.tag = FIPSD_BLE_AGENT_CREDIT, .ch = 99, .bytes = 5});
    fake_send(k, (fipsd_ble_agent_frame){.tag = FIPSD_BLE_AGENT_CLOSED, .ch = 99});
    assert(r->b.ops->scan_stop(r->b.self) == 0); expect_free(k, FIPSD_BLE_AGENT_STOP_SCANNING);
    /* the reader waits for room rather than drop: a byte stream cannot have holes */
    fipsd_ble_stream *flood = dial(r, k, 8, 0x00C1, 12, 512, FIPSD_BLE_AGENT_CHANNEL_WINDOW, &rc);
    assert(!rc && flood);
    uint8_t chunk[100];
    for (unsigned i = 0; i < FIPSD_BLE_AGENT_RX_DEPTH + 64; ++i) { memset(chunk, (int)(i & 0xFF), sizeof chunk); recv_frame(k, 12, chunk, sizeof chunk); }
    for (unsigned i = 0; i < FIPSD_BLE_AGENT_RX_DEPTH + 64; ++i) { memset(chunk, (int)(i & 0xFF), sizeof chunk); read_exact(flood, chunk, sizeof chunk); }
    /* the agent leaving: the radio goes, its channels reset (STREAM_CLOSED), parked dials and reads fail */
    op *parked = spawn_op(dial_fiber, r, 10, 0x00C1, NULL, UINT64_MAX);
    free(expect(k, FIPSD_BLE_AGENT_CONNECT));
    op *reading = spawn_op(read_fiber, r, 0, 0, flood, UINT64_MAX);
    fake_close(k);
    wait_adapter(r, false);
    assert(op_wait(parked) == -ENODEV); free(parked);
    assert(op_wait(reading) == -ECONNRESET); free(reading);
    e = next_event(r, FIPSD_BLE_STREAM_CLOSED); assert(e->stream_id == flood->id); free(e);
    assert(flood->ops->write(flood, one, 1, ble_now() + WAIT_MS) == -ECONNRESET);
    flood->ops->cancel(flood); flood->ops->close(flood);
    puts("streams: dial/accept, peer+MTU, credit parks and spends whole, split SENDs, byte-stream reads, CLOSED is EOF after data, cancel latches, refusal -EIO, timeout then late answer closed, no RECV dropped, agent loss resets: ok");
}

static void stop_body(void)
{
    rig *r = rig_new("stop", 0);
    fake *k = fake_connect(r->path); hello(k, 0x00C1); wait_adapter(r, true);
    fake_send(k, (fipsd_ble_agent_frame){.tag = FIPSD_BLE_AGENT_INBOUND, .ch = 1, .addr = {2, 0, 0, 0, 0x0A, 1}, .mtu = 512});
    fipsd_ble_event *e = next_event(r, FIPSD_BLE_INBOUND_STREAM);
    fipsd_ble_stream *in = e->stream; e->stream = NULL; free(e);
    fake_send(k, (fipsd_ble_agent_frame){.tag = FIPSD_BLE_AGENT_INBOUND, .ch = 2, .addr = {2, 0, 0, 0, 0x0A, 2}, .mtu = 512});
    e = next_event(r, FIPSD_BLE_INBOUND_STREAM);
    leftovers[nleft++] = e->stream; e->stream = NULL; free(e); /* held past destroy */
    op *dialling = spawn_op(dial_fiber, r, 3, 0x00C1, NULL, UINT64_MAX);
    free(expect(k, FIPSD_BLE_AGENT_CONNECT));
    op *reading = spawn_op(read_fiber, r, 0, 0, in, UINT64_MAX);
    /* owner-directed cancellation: everything parked returns -ECANCELED at once */
    r->b.ops->stop(r->b.self); r->b.ops->stop(r->b.self);
    assert(op_wait(dialling) == -ECANCELED); free(dialling);
    assert(op_wait(reading) == -ECANCELED); free(reading);
    uint16_t bound; fipsd_ble_peer p = peer_n(4); fipsd_ble_stream *s = NULL;
    assert(r->b.ops->listen(r->b.self, 0x85, ble_now() + WAIT_MS, &bound) == -ECANCELED);
    assert(r->b.ops->connect(r->b.self, &p, 0x85, ble_now() + WAIT_MS, &s) == -ECANCELED);
    assert(!r->b.ops->adapter(r->b.self).present);
    expect_eof(k); fake_close(k); /* the agent is let go */
    in->ops->cancel(in); in->ops->close(in);
    puts("stop: parked dial/read -ECANCELED, later ops -ECANCELED, agent let go, a stream closed after destroy: ok");
}

/* ======================================================================
 * C. end to end: transports, backends, real agent sessions, mock radios
 * ====================================================================== */
struct fba_channel { int fd; atomic_bool closed; };
typedef struct mradio {
    fba_agent *agent;
    pthread_t thread;
    uint8_t device[6];
    bool psm_in_advert; /* BlueZ-style service data; else a Mac's UUID-only advert, PSM over GATT */
    bool up, advertising, scanning; /* air_mu */
    uint16_t psm;
    unsigned dials;
} mradio;
static pthread_mutex_t air_mu = PTHREAD_MUTEX_INITIALIZER;
static mradio *air[4]; static size_t air_n;
static fba_channel *m_channel(int fd)
{
    fba_channel *c = calloc(1, sizeof *c); assert(c);
    c->fd = fd; atomic_init(&c->closed, false);
#ifdef SO_NOSIGPIPE
    int on = 1; (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
    return c;
}
static void m_advert(mradio *to, const mradio *from) { fba_agent_advert(to->agent, from->device, from->psm_in_advert ? from->psm : 0, -50); }
/* The radio's dial plan (upstream DialPlan): an advertised PSM is dialled as
 * requested; a UUID-only advertiser's is read over GATT first. */
static void m_connect(void *radio, int64_t token, const uint8_t device[6], uint16_t psm)
{
    mradio *me = radio, *to = NULL;
    pthread_mutex_lock(&air_mu);
    for (size_t i = 0; i < air_n; ++i) if (!memcmp(air[i]->device, device, 6)) to = air[i];
    bool ok = me->up && to && to->up && to->psm && (!to->psm_in_advert || psm == to->psm);
    me->dials++;
    fba_agent *theirs = to ? to->agent : NULL;
    pthread_mutex_unlock(&air_mu);
    int fds[2];
    if (!ok || socketpair(AF_UNIX, SOCK_STREAM, 0, fds)) { fba_agent_dialled(me->agent, token, device, NULL, 0); return; }
    fba_agent_inbound(theirs, m_channel(fds[1]), me->device, 512);
    fba_agent_dialled(me->agent, token, device, m_channel(fds[0]), 512);
}
static void m_start_advertising(void *radio, uint16_t psm)
{
    (void)psm; mradio *me = radio, *to[4]; size_t n = 0;
    pthread_mutex_lock(&air_mu);
    me->advertising = me->up;
    for (size_t i = 0; me->advertising && i < air_n; ++i) if (air[i] != me && air[i]->up && air[i]->scanning) to[n++] = air[i];
    pthread_mutex_unlock(&air_mu);
    for (size_t i = 0; i < n; ++i) m_advert(to[i], me);
}
static void m_start_scanning(void *radio)
{
    mradio *me = radio, *from[4]; size_t n = 0;
    pthread_mutex_lock(&air_mu);
    me->scanning = me->up;
    for (size_t i = 0; me->scanning && i < air_n; ++i) if (air[i] != me && air[i]->up && air[i]->advertising) from[n++] = air[i];
    pthread_mutex_unlock(&air_mu);
    for (size_t i = 0; i < n; ++i) m_advert(me, from[i]);
}
static void m_stop_advertising(void *radio) { pthread_mutex_lock(&air_mu); ((mradio *)radio)->advertising = false; pthread_mutex_unlock(&air_mu); }
static void m_stop_scanning(void *radio) { pthread_mutex_lock(&air_mu); ((mradio *)radio)->scanning = false; pthread_mutex_unlock(&air_mu); }
static ssize_t m_read(fba_channel *c, uint8_t *buf, size_t cap)
{ for (;;) { ssize_t n = recv(c->fd, buf, cap, 0); if (n < 0 && errno == EINTR) continue; return n; } }
static ssize_t m_write(fba_channel *c, const uint8_t *buf, size_t len)
{
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL;
#endif
    for (;;) { ssize_t n = send(c->fd, buf, len, flags); if (n < 0 && errno == EINTR) continue; return n; }
}
static void m_close(fba_channel *c) { if (!atomic_exchange(&c->closed, true)) (void)shutdown(c->fd, SHUT_RDWR); }
static void m_release(fba_channel *c) { close(c->fd); free(c); }
static const fba_radio_ops mock_ops = {.connect = m_connect, .start_advertising = m_start_advertising,
    .stop_advertising = m_stop_advertising, .start_scanning = m_start_scanning, .stop_scanning = m_stop_scanning,
    .read = m_read, .write = m_write, .close = m_close, .release = m_release};
static void m_set(mradio *m, bool up, uint16_t psm)
{
    pthread_mutex_lock(&air_mu);
    m->up = up; m->psm = psm;
    if (!up) m->advertising = m->scanning = false;
    pthread_mutex_unlock(&air_mu);
    fba_agent_radio(m->agent, up, psm);
}
static void quiet(void *ctx, int level, const char *msg) { (void)ctx; if (level >= FBA_LOG_WARN) fprintf(stderr, "agent: %s\n", msg); }
static void *agent_main(void *arg) { fba_agent_run(arg); return NULL; }
static mradio *agent_new(const char *path, uint8_t dev, bool psm_in_advert)
{
    mradio *m = calloc(1, sizeof *m); assert(m);
    memcpy(m->device, (uint8_t[]){0x02, 0, 0, 0, 0x0B, dev}, 6); m->psm_in_advert = psm_in_advert;
    fba_agent_config c = {.socket_path = path, .check_uid = true, .daemon_uid = getuid(),
        .reconnect_ms = 20, .max_reconnect_ms = 200, .stable_ms = 200, .log = quiet};
    m->agent = fba_agent_create(&c, &mock_ops, m); assert(m->agent);
    pthread_mutex_lock(&air_mu); air[air_n++] = m; pthread_mutex_unlock(&air_mu);
    assert(!pthread_create(&m->thread, NULL, agent_main, m->agent));
    return m;
}
static void agents_free(void)
{
    for (size_t i = 0; i < air_n; ++i) { fba_agent_stop(air[i]->agent); pthread_join(air[i]->thread, NULL); }
    for (size_t i = 0; i < air_n; ++i) { fba_agent_destroy(air[i]->agent); free(air[i]); }
    air_n = 0;
}
static void identity_of(uint8_t seed, fips_identity **out, fips_xonly_pubkey *key, fips_node_addr *node)
{
    uint8_t secret[32]; memset(secret, seed, 32);
    assert(fips_identity_from_secret_bytes(secp, fipsd_host_os_entropy(), secret, out) == FIPS_IDENTITY_OK);
    *key = fips_identity_pubkey(*out); *node = fips_node_addr_from_pubkey(key);
}
static tnode *tnode_new(uint8_t dev, bool scan)
{
    tnode *n = calloc(1, sizeof *n); assert(n);
    snprintf(n->path, sizeof n->path, "%s/node%u.sock", tmp, dev);
    fipsd_ble_config c; fipsd_ble_config_default(&c);
    c.adapter = "ble0"; c.connect_timeout_ms = 3000; c.probe_cooldown_ms = 150; c.pubkey_exchange_ms = 2000; c.scan = scan;
    identity_of(dev, &n->id, &n->key, &n->node); c.identity = &n->key; c.secp = secp;
    fipsd_ble_backend b;
    fipsd_ble_agent_config ac = {.socket_path = n->path, .group = ""};
    assert(fipsd_ble_agent_backend(&ac, &b));
    n->backend = b.self;
    char name[16]; snprintf(name, sizeof name, "ble%u", dev);
    n->t = fipsd_ble_create((fips_transport_id){dev}, name, &c, b); assert(n->t);
    fipsd_ble_peer p = {FIPSD_BLE_RADIO_ADAPTER, {0x02, 0, 0, 0, 0x0B, dev}};
    assert(fipsd_ble_peer_to_addr(&p, &n->addr));
    n->events = go_channel_create(1024); assert(n->events);
    nodes[nnodes++] = n;
    assert(n->t->ops->start_async(n->t, rt, NULL, n->events) == 0);
    return n;
}
static fipsd_transport_event *await(tnode *n, fipsd_transport_event_kind kind, const fips_transport_addr *at)
{
    const uint64_t deadline = ble_now() + WAIT_MS;
    for (;;) {
        uint64_t now = ble_now();
        fipsd_transport_event *e = now < deadline ? take(n->events, deadline - now) : NULL;
        if (!e) fprintf(stderr, "%s: no event %d\n", n->t->name, (int)kind);
        assert(e);
        if (e->kind == kind && (!at || fips_transport_addr_equal(&e->address, at))) return e;
        free(e);
    }
}
static void await_free(tnode *n, fipsd_transport_event_kind kind, const fips_transport_addr *at) { free(await(n, kind, at)); }
/* a dials b (b only advertises): settled once both pools hold the one link.
 * Woken by transport events and by either backend's channel changes. */
static void link_pair(tnode *a, tnode *b)
{
    const uint64_t deadline = ble_now() + WAIT_MS;
    for (;;) {
        if (fipsd_ble_connected_count(a->t) == 1 && fipsd_ble_connected_count(b->t) == 1 &&
            fipsd_ble_agent_stats_read(a->backend).channels == 1 && fipsd_ble_agent_stats_read(b->backend).channels == 1) return;
        void *v = NULL;
        GoSelectCase cases[] = {{GO_SELECT_RECEIVE, a->events, NULL, &v}, {GO_SELECT_RECEIVE, b->events, NULL, &v},
            {GO_SELECT_RECEIVE, fipsd_ble_agent_changed(a->backend), NULL, &v},
            {GO_SELECT_RECEIVE, fipsd_ble_agent_changed(b->backend), NULL, &v},
            {GO_SELECT_RECEIVE, ((ble_instance *)a->t->user)->changed, NULL, &v},
            {GO_SELECT_RECEIVE, ((ble_instance *)b->t->user)->changed, NULL, &v}};
        uint64_t now = ble_now(); assert(now < deadline);
        GoSelectResult r = go_select_timeout(cases, 6, deadline - now);
        assert(r.selected_case >= 0);
        if (r.selected_case < 2 && v) {
            fipsd_transport_event *e = v;
            const tnode *other = r.selected_case ? a : b;
            if (e->kind == FIPSD_TRANSPORT_DISCOVERED)
                assert(fips_transport_addr_equal(&e->address, &other->addr) && e->size == 32 && !memcmp(e->bytes, other->key.bytes, 32));
            free(e);
        }
    }
}
static void frame(uint8_t *p, size_t n, uint8_t fill)
{ memset(p, fill, n); size_t payload = n - FIPS_FRAMING_FMP_ENCRYPTED_MIN_SIZE; p[0] = 0; p[1] = 0; p[2] = (uint8_t)payload; p[3] = (uint8_t)(payload >> 8); }
static void exchange(tnode *from, tnode *to, size_t n, uint8_t fill)
{
    uint8_t buf[600]; frame(buf, n, fill);
    assert(from->t->ops->send(from->t, &to->addr, buf, n) == 0);
    fipsd_transport_event *e = await(to, FIPSD_TRANSPORT_PACKET, &from->addr);
    assert(e->size == n && !memcmp(e->bytes, buf, n)); free(e);
}
static void e2e_body(void)
{
    after_join = agents_free;
    tnode *a = tnode_new(1, true), *b = tnode_new(2, false);
    /* a's radio advertises like a Mac (UUID only), b's like BlueZ (PSM in service data) */
    mradio *ra = agent_new(a->path, 1, false), *rb = agent_new(b->path, 2, true);
    m_set(ra, true, 0x00C1); m_set(rb, true, 0x00C2);
    await_free(a, FIPSD_TRANSPORT_BECAME_PRESENT, NULL); await_free(b, FIPSD_TRANSPORT_BECAME_PRESENT, NULL);
    assert(fipsd_ble_listener_psm(a->t) == 0x00C1 && fipsd_ble_listener_psm(b->t) == 0x00C2);
    link_pair(a, b);
    for (unsigned i = 0; i < 12; ++i) { exchange(a, b, 40 + i * 37, (uint8_t)i); exchange(b, a, 60 + i * 29, (uint8_t)(i + 50)); }
    for (unsigned i = 0; i < 6; ++i) { uint8_t buf[200]; frame(buf, 200, (uint8_t)(200 + i)); assert(a->t->ops->send(a->t, &b->addr, buf, 200) == 0); }
    for (unsigned i = 0; i < 6; ++i) {
        uint8_t want[200]; frame(want, 200, (uint8_t)(200 + i));
        fipsd_transport_event *e = await(b, FIPSD_TRANSPORT_PACKET, &a->addr); assert(e->size == 200 && !memcmp(e->bytes, want, 200)); free(e);
    }
    assert(a->t->ops->mtu(a->t, &b->addr) == 512);
    fba_agent_stats sa = fba_agent_stats_read(ra->agent), sb = fba_agent_stats_read(rb->agent);
    assert(sa.connected && sb.connected && sa.channels == 1 && sb.channels == 1 && !sa.overruns && !sb.overruns);
    /* b's radio power-cycles onto a new PSM: its agent hangs up, b goes absent and a loses the link;
     * the agent comes back with the new PSM, b listens and advertises it, a rediscovers and relinks */
    m_set(rb, false, 0);
    await_free(b, FIPSD_TRANSPORT_BECAME_ABSENT, NULL); await_free(a, FIPSD_TRANSPORT_DISCONNECTED, &b->addr);
    m_set(rb, true, 0x00C7);
    await_free(b, FIPSD_TRANSPORT_BECAME_PRESENT, NULL);
    assert(fipsd_ble_listener_psm(b->t) == 0x00C7);
    link_pair(a, b);
    exchange(a, b, 90, 1); exchange(b, a, 90, 2);
    assert(fba_agent_stats_read(rb->agent).sessions == 2 && fipsd_ble_agent_stats_read(b->backend).sessions == 2);
    puts("end to end: two transports over agent sockets, real agent sessions, mock radios (UUID-only and PSM adverts): discovery, GATT/learned PSM dials, exchange, link, frames both ways, radio cycle onto a new PSM and relink: ok");
}

int main(void)
{
    tmp = fipsd_test_tmpdir("/streams.sock");
    assert(fips_identity_init_secp(fipsd_host_os_entropy(), &secp) == FIPS_IDENTITY_OK);
    protocol_vectors();
    scenario(lifecycle_body);
    scenario(streams_body);
    scenario(stop_body);
    scenario(e2e_body);
    fips_identity_destroy_secp(secp);
    puts("ble agent: ok");
    return 0;
}
