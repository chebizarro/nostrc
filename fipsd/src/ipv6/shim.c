#define _DEFAULT_SOURCE 1
#define _DARWIN_C_SOURCE 1
#include "fiber_errno.h"
#include "ipv6/shim.h"
#include "ipv6/firewall_state.h"
#include "ipv6/icmp.h"
#include "ipv6/inbound.h"
#include "ipv6/outbound.h"
#include "ipv6/queue.h"
#include "host_os.h"
#include "log.h"
#include "node/handlers/internal.h"
#include "select.h"
#include <libgo/fiber.h>
#include <fips/proto/framing.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#define MOD "ipv6/shim"
#define READ_MAX 65535u
#define RX_MAX 2048u

int fipsd_ipv6_compress(const uint8_t *packet, size_t len, uint8_t *out, size_t cap)
{
    if (!packet || len < FIPSD_IPV6_HEADER_SIZE || packet[0] >> 4 != 6) return -1;
    size_t n = len - FIPSD_IPV6_HEADER_SAVING;
    if (!out || cap < n) return -1;
    out[0] = FIPSD_IPV6_SHIM_FORMAT_COMPRESSED;
    memcpy(out + 1, packet, 4);          /* version + traffic class + flow label */
    out[5] = packet[6]; out[6] = packet[7]; /* next header, hop limit */
    memcpy(out + 7, packet + FIPSD_IPV6_HEADER_SIZE, len - FIPSD_IPV6_HEADER_SIZE);
    return (int)n;
}
int fipsd_ipv6_decompress(const uint8_t *payload, size_t len, const uint8_t src[16], const uint8_t dst[16],
                          uint8_t *out, size_t cap)
{
    if (!payload || len < 1 + FIPSD_IPV6_SHIM_RESIDUAL_SIZE || payload[0] != FIPSD_IPV6_SHIM_FORMAT_COMPRESSED) return -1;
    size_t upper = len - 1 - FIPSD_IPV6_SHIM_RESIDUAL_SIZE, n = FIPSD_IPV6_HEADER_SIZE + upper;
    if (!out || cap < n || upper > UINT16_MAX) return -1;
    out[0] = (uint8_t)((payload[1] & 0x0f) | 0x60); /* the version nibble forced to 6 */
    memcpy(out + 1, payload + 2, 3);
    out[4] = (uint8_t)(upper >> 8); out[5] = (uint8_t)upper;
    out[6] = payload[5]; out[7] = payload[6];
    memcpy(out + 8, src, 16); memcpy(out + 24, dst, 16);
    memcpy(out + FIPSD_IPV6_HEADER_SIZE, payload + 7, upper);
    return (int)n;
}

struct fipsd_ipv6_shim {
    fipsd_ipv6_shim_config cfg;
    fipsd_runtime *rt;
    fipsd_ipv6_mesh_state state;
    fipsd_ipv6_queue *queue;
    fipsd_ipv6_icmp_limiter *limiter;  /* reader-only */
    fips_token_bucket unreachable;     /* reader-only */
    fips_clock clock;
    uint8_t our[16];
    int fd;
    bool running;
    GoChannel *notices, *stop, *done;
    unsigned fibers;
    _Atomic uint32_t ident;
    uint8_t *read_buf, *scratch, *rx_scratch;
    fipsd_ipv6_fragments *frags;
    _Atomic uint64_t read, hairpinned, off_mesh, unreachable_sent, too_big, forwarded, fragments, send_refused;
    _Atomic uint64_t delivered, undeliverable, written, write_errors, clamped_out, clamped_in, passed_through, path_updates;
};
#define COUNT(s, field) atomic_fetch_add_explicit(&(s)->field, 1, memory_order_relaxed)
static fips_instant now_of(fipsd_ipv6_shim *s) { return s->clock.now(s->clock.ctx); }

fipsd_ipv6_shim *fipsd_ipv6_shim_create(const fipsd_ipv6_shim_config *cfg)
{
    if (!cfg || !cfg->tun || !cfg->handlers) { errno = EINVAL; return NULL; }
    fipsd_ipv6_shim *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->cfg = *cfg; s->fd = -1;
    s->clock = cfg->clock.now ? cfg->clock : fipsd_host_os_clock();
    s->notices = cfg->notices ? cfg->notices : fipsd_node_handlers_notices(cfg->handlers);
    s->queue = fipsd_ipv6_queue_create(cfg->host_queue);
    s->limiter = calloc(1, sizeof *s->limiter);
    s->read_buf = malloc(READ_MAX); s->scratch = malloc(READ_MAX); s->rx_scratch = malloc(RX_MAX);
    s->frags = calloc(1, sizeof *s->frags);
    s->stop = go_channel_create(1); s->done = go_channel_create(3);
    if (!s->queue || !s->limiter || !s->read_buf || !s->scratch || !s->rx_scratch || !s->frags || !s->stop || !s->done ||
        !s->notices || fipsd_ipv6_mesh_state_init(&s->state, cfg->key_capacity, cfg->path_capacity) < 0) {
        fipsd_ipv6_shim_destroy(s); errno = ENOMEM; return NULL;
    }
    uint16_t limit = cfg->payload_limit ? cfg->payload_limit : fips_framing_payload_limit(FIPSD_IPV6_MIN_MTU + FIPSD_IPV6_OVERHEAD);
    fipsd_ipv6_from_mesh seed = {.kind = FIPSD_IPV6_MESH_NODE_MTU, .payload_limit = limit};
    (void)fipsd_ipv6_mesh_state_apply(&s->state, &seed, NULL);
    fipsd_ipv6_icmp_limiter_init(s->limiter);
    fipsd_ipv6_unreachable_bucket_init(&s->unreachable, now_of(s));
    fipsd_tun_set_host_drops(cfg->tun, fipsd_ipv6_queue_counter(s->queue));
    return s;
}
void fipsd_ipv6_shim_destroy(fipsd_ipv6_shim *s)
{
    if (!s) return;
    (void)fipsd_ipv6_shim_stop(s);
    if (s->cfg.tun) fipsd_tun_set_host_drops(s->cfg.tun, NULL);
    fipsd_ipv6_queue_destroy(s->queue);
    fipsd_ipv6_mesh_state_destroy(&s->state);
    if (s->stop) go_channel_unref(s->stop);
    if (s->done) go_channel_unref(s->done);
    free(s->limiter); free(s->read_buf); free(s->scratch); free(s->rx_scratch); free(s->frags);
    free(s);
}
fipsd_ipv6_mesh_state *fipsd_ipv6_shim_state(fipsd_ipv6_shim *s) { return &s->state; }
fipsd_ipv6_host_link fipsd_ipv6_shim_host_link(fipsd_ipv6_shim *s) { return (fipsd_ipv6_host_link){&s->state}; }
fipsd_ipv6_learned fipsd_ipv6_shim_learn(fipsd_ipv6_shim *s, const fips_node_addr *peer)
{ return fipsd_ipv6_mesh_state_learn(&s->state, peer); }
const _Atomic uint64_t *fipsd_ipv6_shim_drop_counter(const fipsd_ipv6_shim *s) { return fipsd_ipv6_queue_counter(s->queue); }
fipsd_ipv6_shim_counters fipsd_ipv6_shim_counters_read(const fipsd_ipv6_shim *s)
{
#define LOAD(f) atomic_load_explicit(&s->f, memory_order_relaxed)
    return (fipsd_ipv6_shim_counters){.read = LOAD(read), .hairpinned = LOAD(hairpinned), .off_mesh = LOAD(off_mesh),
        .unreachable = LOAD(unreachable_sent), .too_big = LOAD(too_big), .forwarded = LOAD(forwarded),
        .fragments = LOAD(fragments), .send_refused = LOAD(send_refused), .delivered = LOAD(delivered),
        .undeliverable = LOAD(undeliverable), .written = LOAD(written), .write_errors = LOAD(write_errors),
        .clamped_out = LOAD(clamped_out), .clamped_in = LOAD(clamped_in), .passed_through = LOAD(passed_through),
        .path_updates = LOAD(path_updates), .host_drops = fipsd_ipv6_queue_dropped(s->queue)};
#undef LOAD
}

/* ---- reader: the host half, carried out ---- */
static bool answer_unreachable(fipsd_ipv6_shim *s, const uint8_t *response, size_t len)
{
    bool ok = fipsd_ipv6_answer_unreachable(response, len, &s->unreachable, now_of(s), s->queue);
    if (ok) COUNT(s, unreachable_sent);
    return ok;
}
static void answer_oversize(fipsd_ipv6_shim *s, const uint8_t *packet, size_t len, uint32_t mtu)
{
    if (fipsd_ipv6_answer_oversize(packet, len, mtu, s->limiter, now_of(s), s->queue)) COUNT(s, too_big);
}
static void send_datagram(fipsd_ipv6_shim *s, const fips_node_addr *peer, const uint8_t *payload, size_t len)
{
    if (fipsd_node_handlers_send(s->cfg.handlers, peer, FIPSD_IPV6_PORT, FIPSD_IPV6_PORT, payload, len) == 0) {
        COUNT(s, forwarded); return;
    }
    /* The dispatcher's mailbox is full (upstream's reader blocks on its
     * channel instead); dropped and counted, logged at each doubling. */
    uint64_t n = COUNT(s, send_refused) + 1;
    if ((n & (n - 1)) == 0) FIPSD_WARN(MOD, "node not taking datagrams; dropping packets toward it (dropped=%llu)", (unsigned long long)n);
}
/* The node-side judgement upstream takes on a datagram it holds, with the
 * shim's replica of the node's limits: a payload over the smaller of the
 * node-wide limit and the stored path to its destination goes as IPv6
 * fragments when it can, else the host is owed Packet Too Big. */
static void forward(fipsd_ipv6_shim *s, const uint8_t *packet, size_t len, const fipsd_ipv6_datagram *dg)
{
    uint16_t limit = fipsd_ipv6_mesh_state_node_limit(&s->state), path;
    fips_ipv6_address dst = fipsd_ipv6_peer_address(&dg->peer);
    if (fipsd_ipv6_paths_get(&s->state.paths, dst.bytes, &path) && path < limit) limit = path;
    if (limit > FIPSD_HANDLERS_DATAGRAM_MAX) limit = FIPSD_HANDLERS_DATAGRAM_MAX;
    if (dg->len <= limit) { send_datagram(s, &dg->peer, dg->payload, dg->len); return; }
    uint32_t ident = atomic_fetch_add_explicit(&s->ident, 1, memory_order_relaxed);
    if (fipsd_ipv6_split(dg->payload, dg->len, &dg->peer, s->our, limit, ident, s->frags)) {
        for (size_t i = 0; i < s->frags->count; ++i) {
            send_datagram(s, &dg->peer, s->frags->bytes[i], s->frags->len[i]);
            COUNT(s, fragments);
        }
        return;
    }
    answer_oversize(s, packet, len, fipsd_ipv6_packet_too_big_mtu(len, fipsd_ipv6_mtu_of_limit(limit)));
}
/* False when the reader should exit: a closed queue toward the host on a
 * hairpin or off-mesh answer (the answers to a mesh destination ignore it). */
static bool handle_tun_packet(fipsd_ipv6_shim *s, uint8_t *packet, size_t len)
{
    COUNT(s, read);
    fipsd_ipv6_from_tun v;
    fipsd_ipv6_from_tun_decide(packet, len, fipsd_ipv6_mesh_state_ceiling(&s->state), &s->state.paths, &s->state, s->our,
                        s->scratch, READ_MAX, &v);
    switch (v.kind) {
    case FIPSD_IPV6_FROM_TUN_NOTHING: return true;
    case FIPSD_IPV6_FROM_TUN_HAIRPIN:
        COUNT(s, hairpinned);
        FIPSD_TRACE(MOD, "Hairpinning self-addressed packet back to TUN (loopback)");
        return fipsd_ipv6_queue_send(s->queue, packet, len) == 0;
    case FIPSD_IPV6_FROM_TUN_OFF_MESH:
        COUNT(s, off_mesh);
        FIPSD_TRACE(MOD, "Sending ICMPv6 Destination Unreachable (non-FIPS destination), %zu bytes", v.response_len);
        return answer_unreachable(s, v.response, v.response_len);
    case FIPSD_IPV6_FROM_TUN_HOST:
        if (v.clamped) { COUNT(s, clamped_out); FIPSD_TRACE(MOD, "Clamped TCP MSS in SYN packet to %u", v.clamped_mss); }
        switch (v.host.kind) {
        case FIPSD_IPV6_HOST_NOTHING: break;
        case FIPSD_IPV6_HOST_MESH: forward(s, packet, len, &v.host.datagram); break;
        case FIPSD_IPV6_HOST_UNREACHABLE: (void)answer_unreachable(s, v.host.response, v.host.response_len); break;
        case FIPSD_IPV6_HOST_TOO_BIG: answer_oversize(s, packet, len, v.host.mtu); break;
        }
        return true;
    }
    return true;
}
static void reader(void *arg)
{
    fipsd_ipv6_shim *s = arg;
    /* The stop is observed before every read as well: cancellation ends a
     * parked wait, but a descriptor that is always ready keeps reading. */
    while (!go_channel_is_closed(s->stop)) {
        ssize_t n = fipsd_tun_read(s->cfg.tun, s->read_buf, READ_MAX);
        if (n < 0) {
            int e = fipsd_io_errno();
            if (e == EINTR || e == EAGAIN) continue;
            if (e != ECANCELED && !go_channel_is_closed(s->stop)) FIPSD_ERROR(MOD, "TUN read error: %s", strerror(e));
            break;
        }
        if (n == 0) continue;
        if (!handle_tun_packet(s, s->read_buf, (size_t)n)) break;
    }
    (void)go_channel_send(s->done, s);
}

/* ---- writer: the bounded queue toward the host, SYNs clamped ---- */
static void write_packet(fipsd_ipv6_shim *s, fipsd_ipv6_packet *p)
{
    uint16_t mss;
    /* Read per packet, not once: the node-wide limit moves at runtime. */
    if (fipsd_ipv6_clamp(p->bytes, p->len, fipsd_ipv6_mesh_state_ceiling(&s->state), &s->state.paths,
                         FIPSD_IPV6_REMOTE_SOURCE, &mss)) {
        COUNT(s, clamped_in); FIPSD_TRACE(MOD, "Clamped TCP MSS in inbound SYN packet to %u", mss);
    }
    if (fipsd_tun_write(s->cfg.tun, p->bytes, p->len) < 0) {
        int e = fipsd_io_errno();
        COUNT(s, write_errors);
        if (e != ECANCELED && e != EFAULT && e != ENODEV) FIPSD_DEBUG(MOD, "TUN write error (%zu bytes): %s", p->len, strerror(e));
    } else COUNT(s, written);
    free(p);
}
static void writer(void *arg)
{
    fipsd_ipv6_shim *s = arg;
    GoChannel *chan = fipsd_ipv6_queue_channel(s->queue);
    while (!go_channel_is_closed(s->stop)) {
        void *v = NULL, *stop = NULL;
        GoSelectCase cases[] = {{GO_SELECT_RECEIVE, chan, NULL, &v}, {GO_SELECT_RECEIVE, s->stop, NULL, &stop}};
        int i = go_select(cases, 2);
        if (i == 0 && v) { write_packet(s, v); continue; }
        if (go_channel_is_closed(s->stop)) break;
    }
    /* What was queued before the stop still goes to the host, as upstream's
     * writer drains its channel once every sender has gone. */
    fipsd_ipv6_packet *p;
    while ((p = fipsd_ipv6_queue_try_receive(s->queue))) write_packet(s, p);
    (void)go_channel_send(s->done, s);
}

/* ---- mesh receiver: the node's notices ---- */
static void pass_through(fipsd_ipv6_shim *s, fipsd_node_notice *n)
{
    if (s->cfg.passthrough && go_channel_try_send(s->cfg.passthrough, n) == 0) { COUNT(s, passed_through); return; }
    fipsd_node_notice_free(n);
}
static void deliver(fipsd_ipv6_shim *s, const fipsd_node_notice *n)
{
    FIPSD_PERF_BEGIN(tun_start);
    fips_ipv6_address src = fipsd_ipv6_peer_address(&n->peer);
    /* The C node's delivery carries no CE mark; upstream's Deliver does. */
    fipsd_ipv6_deliver d = {.peer = n->peer, .payload = n->payload, .len = n->len, .ce = false};
    int len = fipsd_ipv6_unpack(&d, src.bytes, s->our, s->rx_scratch, RX_MAX);
    if (len < 0) { COUNT(s, undeliverable); FIPSD_PERF_END(tun_start, TUN_WRITE); return; }
    COUNT(s, delivered);
    (void)fipsd_ipv6_queue_send(s->queue, s->rx_scratch, (size_t)len);
    FIPSD_PERF_END(tun_start, TUN_WRITE);
}
static void on_notice(fipsd_ipv6_shim *s, fipsd_node_notice *n)
{
    fipsd_ipv6_from_mesh m;
    switch (n->kind) {
    case FIPSD_NOTICE_DATAGRAM:
        if (n->dst_port == FIPSD_IPV6_PORT) { deliver(s, n); fipsd_node_notice_free(n); return; }
        /* A datagram on another port still proves the peer routable. */
        /* fall through */
    case FIPSD_NOTICE_LINK_UP: case FIPSD_NOTICE_SESSION_UP:
        m = (fipsd_ipv6_from_mesh){.kind = FIPSD_IPV6_MESH_KNOWN_KEY, .peer = n->peer};
        (void)fipsd_ipv6_mesh_state_apply(&s->state, &m, NULL);
        break;
    case FIPSD_NOTICE_PATH_MTU:
        m = (fipsd_ipv6_from_mesh){.kind = FIPSD_IPV6_MESH_PATH_MTU, .peer = n->peer, .has_limit = n->value != 0,
            .payload_limit = fips_framing_payload_limit(n->value > UINT16_MAX ? UINT16_MAX : (uint16_t)n->value)};
        (void)fipsd_ipv6_mesh_state_apply(&s->state, &m, NULL);
        COUNT(s, path_updates);
        break;
    default: break;
    }
    pass_through(s, n);
}
static void mesh_rx(void *arg)
{
    fipsd_ipv6_shim *s = arg;
    while (!go_channel_is_closed(s->stop)) {
        void *v = NULL, *stop = NULL;
        GoSelectCase cases[] = {{GO_SELECT_RECEIVE, s->notices, NULL, &v}, {GO_SELECT_RECEIVE, s->stop, NULL, &stop}};
        int i = go_select(cases, 2);
        if (i == 0 && v) { on_notice(s, v); continue; }
        if (go_channel_is_closed(s->stop)) break;
    }
    (void)go_channel_send(s->done, s);
}

/* ---- lifecycle ---- */
static void join(fipsd_ipv6_shim *s)
{
    void *v;
    while (s->fibers) { if (go_channel_receive(s->done, &v) != 0) break; --s->fibers; }
}
int fipsd_ipv6_shim_start(fipsd_ipv6_shim *s, fipsd_runtime *rt)
{
    if (!s || !rt) { errno = EINVAL; return -1; }
    if (s->running) return 0;
    int fd = fipsd_tun_fd(s->cfg.tun);
    if (fd < 0) { errno = ENODEV; return -1; }
    if (gof_io_reset_fd(fd) != 0) return -1;
    fips_ipv6_address our = fipsd_tun_address(s->cfg.tun);
    memcpy(s->our, our.bytes, 16);
    s->fd = fd; s->rt = rt;
    /* A fresh stop/done pair per run: the channels are consumed by a stop. */
    go_channel_unref(s->stop); go_channel_unref(s->done);
    s->stop = go_channel_create(1); s->done = go_channel_create(3);
    if (!s->stop || !s->done) return -1;
    fipsd_ipv6_queue_open(s->queue); /* a stop leaves it refusing sends */
    s->running = true;
    void (*fns[])(void *) = {reader, writer, mesh_rx};
    for (unsigned i = 0; i < 3; ++i) {
        if (fipsd_runtime_spawn(rt, fns[i], s) != 0) { (void)fipsd_ipv6_shim_stop(s); fipsd_io_set_errno(EAGAIN); return -1; }
        ++s->fibers;
    }
    unsigned ipv6_mtu = fipsd_ipv6_mesh_state_ipv6_mtu(&s->state);
    unsigned mss_ceiling = fipsd_ipv6_mesh_state_ceiling(&s->state);
    FIPSD_INFO(MOD, "IPv6 shim up on %s (ipv6 mtu %u, mss ceiling %u)", fipsd_tun_name(s->cfg.tun),
        ipv6_mtu, mss_ceiling);
    if (s->cfg.firewall_scope_check) {
        fipsd_ipv6_filter_classifier *c = fipsd_ipv6_filter_query();
        fipsd_ipv6_filter_warn_scope(c, fipsd_tun_name(s->cfg.tun));
        fipsd_ipv6_filter_destroy(c);
    }
    return 0;
}
int fipsd_ipv6_shim_stop(fipsd_ipv6_shim *s)
{
    if (!s || !s->running) return 0;
    s->running = false;
    go_channel_close(s->stop);
    (void)gof_io_cancel_fd(s->fd);
    fipsd_ipv6_queue_close(s->queue);
    join(s);
    int rc = gof_io_reset_fd(s->fd);
    s->fd = -1;
    return rc;
}
