/* The mdns subsystem: upstream's LanRendezvous runtime and the node's LAN
 * candidate handling, over this daemon's runtime, netmon and dial path. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "mdns/mdns.h"
#include "mdns/browser.h"
#include "mdns/lan.h"
#include "mdns/responder.h"
#include "mdns/socket.h"
#include "config/config.h"
#include "control/snapshot.h"
#include "log.h"
#include "transport/udp/udp.h"
#include <fips/identity/encoding.h>
#include <fips/identity/node_addr.h>
#include <libgo/fiber.h>
#include <select.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MODULE "mdns"
#define RX_QUEUE 256u
#define RX_BATCH 32u
#define SENT_RING 64u
#define SENT_WINDOW_MS 5000u
/* Upstream re-offers held LAN candidates on its one-second node tick; here
 * the same retry is a deadline armed only while candidates are held. */
#define LAN_RETRY_MS 1000u
/* A dial the dispatcher has not published within this long after it was
 * queued is over (refused, or failed before its leg was published). */
#define DIAL_SETTLE_MS 1000u
#define OUT_MAX 16u
#define NODES_MAX 256u

typedef struct sent_hash { uint64_t hash, at_ms; } sent_hash;
struct fipsd_mdns_service {
    fipsd_subsystem subsystem;
    GoSwap *configs;
    fipsd_node *node;
    fipsd_node_handlers *handlers;
    const fipsd_node_context *ctx;
    const fipsd_transport_registry *transports;
    fipsd_mdns_sockets sockets;
    fipsd_mdns_io io;
    bool has_netmon_source;
    fipsd_netmon_source netmon_source;
    fipsd_netmon *netmon;
    GoChannel *rx, *bound, *stop, *events;
    fipsd_runtime *rt;
    bool running;
    /* Engine-owned from here. */
    char npub[128], label[FIPSD_MDNS_TEXT_MAX], service_type[FIPSD_MDNS_TEXT_MAX], scope[FIPSD_MDNS_TEXT_MAX];
    bool has_scope;
    uint64_t handshake_timeout_ms;
    fipsd_mdns_responder *responder;
    fipsd_mdns_browser *browser;
    fipsd_lan_pending lan;
    uint64_t lan_next_ms;
    uint16_t port;
    size_t iface_count;
    fipsd_mdns_iface_addrs ifaces[FIPSD_MDNS_MAX_INTERFACES];
    bool joined[FIPSD_MDNS_MAX_INTERFACES][2];
    bool have_net;
    uint64_t net_generation;
    GoRefPtr net;
    sent_hash sent[SENT_RING];
    size_t sent_at;
    fipsd_mdns_message *msg;
    fipsd_mdns_out *outs;
    fipsd_mdns_resolved *resolved;
};

static uint32_t rng_uniform(void *user, uint32_t bound) { (void)user; return bound ? randombytes_uniform(bound) : 0; }
static const fipsd_mdns_rng rng = {rng_uniform, NULL};
static uint64_t clock_ms(clockid_t id)
{
    struct timespec ts; clock_gettime(id, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
static uint64_t mono_ms(void) { return clock_ms(CLOCK_MONOTONIC); }
static uint64_t fnv(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

static void post(fipsd_mdns_service *s, const fipsd_mdns_event *e)
{
    if (!s->events) return;
    fipsd_mdns_event *owned = malloc(sizeof *owned);
    if (!owned) return;
    *owned = *e;
    if (go_channel_try_send(s->events, owned) != 0) free(owned);
}
static void post_peer(fipsd_mdns_service *s, fipsd_mdns_event_kind kind, const char *npub, const fipsd_mdns_advert_fields *f,
                      const fipsd_mdns_sockaddr *addr, const uint32_t *ifaces, size_t n, const char *reason, const char *fullname)
{
    if (!s->events) return;
    fipsd_mdns_event e = {.kind = kind};
    if (npub) snprintf(e.npub, sizeof e.npub, "%s", npub);
    if (f && f->has_scope) { e.has_scope = true; snprintf(e.scope, sizeof e.scope, "%s", f->scope); }
    if (addr) e.addr = *addr;
    for (size_t i = 0; i < n && i < FIPSD_MDNS_MAX_IFACES; ++i) e.ifaces[e.iface_count++] = ifaces[i];
    if (reason) snprintf(e.reason, sizeof e.reason, "%s", reason);
    if (fullname) snprintf(e.fullname, sizeof e.fullname, "%s", fullname);
    post(s, &e);
}

/* ---- transports ---------------------------------------------------- */
static bool udp_bound(fipsd_transport *t, int *family, uint16_t *port)
{
    fips_transport_addr addr; const char *text; size_t len; char buf[300]; fipsd_udp_endpoint e;
    if (!fipsd_udp_address(t, &addr) || fips_transport_addr_as_str(&addr, &text, &len) != FIPS_OK || len >= sizeof buf) return false;
    memcpy(buf, text, len); buf[len] = 0;
    if (!fipsd_udp_endpoint_numeric(buf, &e)) return false;
    *family = e.address.ss_family;
    *port = ntohs(e.address.ss_family == AF_INET ? ((struct sockaddr_in *)&e.address)->sin_port
                                                 : ((struct sockaddr_in6 *)&e.address)->sin6_port);
    return true;
}
/* The lowest-id bound UDP transport, of `family` unless it is AF_UNSPEC. */
static fipsd_transport *lowest_udp(const fipsd_mdns_service *s, int family, uint16_t *port)
{
    fipsd_transport *best = NULL;
    for (size_t i = 0; i < s->transports->count; ++i) {
        fipsd_transport *t = s->transports->items[i]; int f; uint16_t p;
        if (strcmp(t->type, "udp") || !udp_bound(t, &f, &p) || (family != AF_UNSPEC && f != family)) continue;
        if (!best || t->id.value < best->id.value) { best = t; if (port) *port = p; }
    }
    return best;
}

/* ---- sending ------------------------------------------------------- */
static size_t iface_slot(const fipsd_mdns_service *s, uint32_t index)
{
    for (size_t i = 0; i < s->iface_count; ++i) if (s->ifaces[i].index == index) return i;
    return SIZE_MAX;
}
static void remember_sent(fipsd_mdns_service *s, const uint8_t *buf, size_t len, uint64_t now)
{
    s->sent[s->sent_at] = (sent_hash){fnv(buf, len), now};
    s->sent_at = (s->sent_at + 1) % SENT_RING;
}
static bool is_own(const fipsd_mdns_service *s, const uint8_t *buf, size_t len, uint64_t now)
{
    uint64_t h = fnv(buf, len);
    for (size_t i = 0; i < SENT_RING; ++i)
        if (s->sent[i].at_ms && s->sent[i].hash == h && now - s->sent[i].at_ms < SENT_WINDOW_MS) return true;
    return false;
}
static void send_multicast(fipsd_mdns_service *s, size_t slot, const uint8_t *buf, size_t len, uint64_t now)
{
    const fipsd_mdns_iface_addrs *a = &s->ifaces[slot];
    static const uint8_t none[4] = {0};
    remember_sent(s, buf, len, now);
    if (s->joined[slot][0]) (void)s->io.send(s->io.user, a->index, AF_INET, a->v4_count ? a->v4[0] : none, NULL, buf, len);
    if (s->joined[slot][1]) (void)s->io.send(s->io.user, a->index, AF_INET6, none, NULL, buf, len);
}
static void send_outs(fipsd_mdns_service *s, size_t n, uint64_t now)
{
    static const uint8_t none[4] = {0};
    for (size_t i = 0; i < n; ++i) {
        const fipsd_mdns_out *o = &s->outs[i];
        size_t slot = iface_slot(s, o->ifindex);
        if (slot == SIZE_MAX) continue;
        if (!o->unicast) { send_multicast(s, slot, o->bytes, o->len, now); continue; }
        int family = o->dst.ip.width == 4 ? AF_INET : AF_INET6;
        const fipsd_mdns_iface_addrs *a = &s->ifaces[slot];
        remember_sent(s, o->bytes, o->len, now);
        (void)s->io.send(s->io.user, a->index, family, a->v4_count ? a->v4[0] : none, &o->dst, o->bytes, o->len);
    }
}

/* ---- the LAN candidate path (upstream lifecycle handle_lan_events) -- */
typedef struct peer_view {
    fips_node_addr configured[NODES_MAX], connected[NODES_MAX], connecting[NODES_MAX];
    size_t nconfigured, nconnected, nconnecting;
    uint64_t snapshot_wall_ms;
    fipsd_lan_known_labels known;
} peer_view;
static bool hex_node(const char *hex, fips_node_addr *out)
{
    if (!hex || strlen(hex) != 32) return false;
    return sodium_hex2bin(out->bytes, sizeof out->bytes, hex, 32, NULL, NULL, NULL) == 0;
}
/* Configured peers from the configuration; connected and connecting peers
 * from the dispatcher's published snapshot, read afresh at each use so a
 * reload or a dropped link applies at once. */
static peer_view *peer_view_load(fipsd_mdns_service *s)
{
    peer_view *v = calloc(1, sizeof *v);
    if (!v) return NULL;
    const fips_secp *secp = fipsd_node_secp(s->ctx);
    GoRefPtr cfg = fipsd_node_config(s->ctx);
    const fipsd_config_value *peers = cfg.ptr ? fipsd_config_get(cfg.ptr, "peers") : NULL;
    for (size_t i = 0; peers && i < fipsd_config_count(peers) && v->nconfigured < NODES_MAX; ++i) {
        const char *npub = fipsd_config_string(fipsd_config_member(fipsd_config_at(peers, i), "npub"));
        fips_peer_identity id; char canon[128];
        if (!npub || fips_peer_identity_from_npub(secp, npub, strlen(npub), &id) != FIPS_IDENTITY_OK) continue;
        v->configured[v->nconfigured++] = id.node_addr;
        if (fips_peer_identity_npub(&id, canon, sizeof canon) == FIPS_IDENTITY_OK)
            fipsd_lan_known_labels_add(&v->known, canon, &id.node_addr);
    }
    if (cfg.ptr) go_refptr_release(&cfg);
    fipsd_control_provider p = fipsd_node_handlers_provider(s->handlers);
    GoRefPtr ref = p.acquire ? p.acquire(p.user) : (GoRefPtr){0};
    const fipsd_control_snapshot *snap = ref.ptr;
    if (snap) {
        v->snapshot_wall_ms = snap->now_ms;
        for (size_t i = 0; i < snap->peer_meta_count && v->nconnected < NODES_MAX; ++i) {
            if (!snap->peer_meta[i].is_active) continue;
            fips_node_addr a; memcpy(a.bytes, snap->peer_meta[i].node_addr, sizeof a.bytes);
            v->connected[v->nconnected++] = a;
            if (snap->peer_meta[i].npub) fipsd_lan_known_labels_add(&v->known, snap->peer_meta[i].npub, &a);
        }
        for (size_t i = 0; i < snap->connections_count && v->nconnecting < NODES_MAX; ++i)
            if (hex_node(snap->connections[i].expected_peer, &v->connecting[v->nconnecting])) ++v->nconnecting;
        go_refptr_release(&ref);
    }
    return v;
}
static fipsd_lan_nodes nodes(const fips_node_addr *items, size_t n) { return (fipsd_lan_nodes){items, n}; }
static const fips_onlink_prefixes *on_link_of(const fipsd_mdns_service *s)
{
    static const fips_onlink_prefixes empty_failed = {0};
    const fipsd_network_snapshot *snap = s->net.ptr;
    return snap ? &snap->onlink : &empty_failed;
}
static bool onlink_failed(const fipsd_mdns_service *s)
{
    const fipsd_network_snapshot *snap = s->net.ptr;
    return !snap || snap->failed || fips_onlink_read_failed(&snap->onlink);
}
typedef struct dial_ctx { const peer_view *v; uint64_t now; uint64_t timeout_ms; } dial_ctx;
static bool dial_over(void *user, const fipsd_lan_dialed *d)
{
    const dial_ctx *c = user;
    if (fipsd_lan_nodes_contain(nodes(c->v->connected, c->v->nconnected), &d->node)) return true;
    if (c->now - d->at_ms >= c->timeout_ms) return true;
    return c->v->snapshot_wall_ms >= d->at_wall_ms + DIAL_SETTLE_MS &&
        !fipsd_lan_nodes_contain(nodes(c->v->connecting, c->v->nconnecting), &d->node);
}
/* Upstream dial_pending_lan: drop held pairs of connected nodes and pairs
 * off this node's links, then dial what the in-flight bounds allow. */
static void dial_pending(fipsd_mdns_service *s, peer_view *v, uint64_t now)
{
    s->lan_next_ms = UINT64_MAX;
    fipsd_lan_nodes configured = nodes(v->configured, v->nconfigured);
    fipsd_lan_prune_connected(&s->lan, nodes(v->connected, v->nconnected));
    if (onlink_failed(s)) {
        FIPSD_DEBUG(MODULE, "lan: interface read failed; held candidates kept for the next tick held=%zu", s->lan.count);
        if (s->lan.count) s->lan_next_ms = now + LAN_RETRY_MS;
        return;
    }
    size_t dropped = fipsd_lan_retain_on_link(&s->lan, on_link_of(s));
    if (dropped) FIPSD_DEBUG(MODULE, "lan: dropped pending candidates no longer on this node's links dropped=%zu reason=off-link", dropped);
    dial_ctx dc = {v, now, s->handshake_timeout_ms};
    fipsd_lan_prune_dialed(&s->lan, dial_over, &dc);
    if (!s->lan.count) return;
    size_t order[FIPSD_LAN_MAX_PENDING], n = fipsd_lan_ordered(&s->lan, configured, rng, order);
    fipsd_lan_pair dial[FIPSD_LAN_MAX_PENDING]; size_t nd = 0;
    fips_node_addr picked[FIPSD_LAN_MAX_PENDING]; size_t np = 0;
    size_t in_flight = fipsd_lan_in_flight(&s->lan);
    for (size_t i = 0; i < n; ++i) {
        const fipsd_lan_pair *p = &s->lan.pairs[order[i]];
        bool conf = fipsd_lan_nodes_contain(configured, &p->identity.node_addr);
        if (fipsd_lan_nodes_contain(nodes(picked, np), &p->identity.node_addr) ||
            fipsd_lan_is_dialed(&s->lan, &p->identity.node_addr) ||
            fipsd_lan_nodes_contain(nodes(v->connecting, v->nconnecting), &p->identity.node_addr)) continue;
        if (!conf && in_flight >= FIPSD_LAN_MAX_DIALS_IN_FLIGHT) continue;
        picked[np++] = p->identity.node_addr;
        dial[nd++] = *p;
        if (!conf) ++in_flight;
    }
    uint64_t wall = clock_ms(CLOCK_REALTIME);
    for (size_t i = 0; i < nd; ++i) {
        const fipsd_lan_pair *p = &dial[i];
        fips_transport_addr to; char text[80], short_npub[24];
        (void)fipsd_mdns_sockaddr_text(&p->addr, text, sizeof text);
        if (fips_peer_identity_short_npub(&p->identity, short_npub, sizeof short_npub) != FIPS_IDENTITY_OK) short_npub[0] = 0;
        if (fips_transport_addr_from_socket_addr(p->addr.ip.bytes, p->addr.ip.width, p->addr.port, p->addr.scope_id, &to) != FIPS_OK)
            continue;
        if (fipsd_node_handlers_learn_identity(s->handlers, &p->identity) != 0 ||
            fipsd_node_handlers_connect(s->handlers, &p->identity, p->transport, &to) != 0) {
            FIPSD_DEBUG(MODULE, "lan: dial not queued; the dispatcher's command queue is full npub=%s addr=%s", short_npub, text);
            continue;
        }
        FIPSD_INFO(MODULE, "lan: dialling mDNS-discovered peer npub=%s addr=%s", short_npub, text);
        char npub[128];
        if (fips_peer_identity_npub(&p->identity, npub, sizeof npub) != FIPS_IDENTITY_OK) npub[0] = 0;
        post_peer(s, FIPSD_MDNS_EVENT_DIALLED, npub, NULL, &p->addr, p->ifaces, p->iface_count, NULL, NULL);
        fipsd_lan_take(&s->lan, &p->identity.node_addr, &p->addr);
        if (!fipsd_lan_nodes_contain(configured, &p->identity.node_addr))
            fipsd_lan_note_dialed(&s->lan, &p->identity.node_addr, &p->addr, now, wall);
    }
    if (s->lan.count) s->lan_next_ms = now + LAN_RETRY_MS;
}
static void log_set_full(bool first)
{
    if (first) FIPSD_WARN(MODULE, "LAN discovery candidate set full (%u); new adverts displace held unconfigured candidates",
                          FIPSD_LAN_MAX_PENDING);
}
/* One discovered (npub, address): upstream handle_lan_events's per-event body. */
static void offer_discovered(fipsd_mdns_service *s, peer_view *v, const fipsd_mdns_advert_fields *f,
                             const fipsd_mdns_sockaddr *addr, const uint32_t *ifaces, size_t nif, uint64_t now)
{
    char text[80]; (void)fipsd_mdns_sockaddr_text(addr, text, sizeof text);
    fipsd_transport *t = lowest_udp(s, addr->ip.width == 4 ? AF_INET : AF_INET6, NULL);
    if (!t) {
        FIPSD_DEBUG(MODULE, "lan: skip discovered peer with no compatible UDP transport addr=%s", text);
        post_peer(s, FIPSD_MDNS_EVENT_SKIPPED, f->npub, f, addr, ifaces, nif, "no-udp-transport", NULL);
        return;
    }
    if (!fipsd_lan_target_on_link(addr, ifaces, nif, on_link_of(s))) {
        if (onlink_failed(s))
            FIPSD_DEBUG(MODULE, "lan: ignoring advert; this node's interfaces could not be read npub=%s addr=%s reason=interface-read-failed", f->npub, text);
        else
            FIPSD_DEBUG(MODULE, "lan: ignoring advert for an address off the link it arrived on npub=%s addr=%s reason=off-link", f->npub, text);
        post_peer(s, FIPSD_MDNS_EVENT_SKIPPED, f->npub, f, addr, ifaces, nif, "off-link", NULL);
        return;
    }
    fipsd_lan_pair pair = {.transport = t->id, .addr = *addr};
    if (fips_peer_identity_from_npub(fipsd_node_secp(s->ctx), f->npub, f->npub_len, &pair.identity) != FIPS_IDENTITY_OK) {
        FIPSD_DEBUG(MODULE, "lan: skip bad npub npub=%s", f->npub);
        post_peer(s, FIPSD_MDNS_EVENT_SKIPPED, f->npub, f, addr, ifaces, nif, "bad-npub", NULL);
        return;
    }
    char canon[128], short_npub[24];
    if (fips_peer_identity_npub(&pair.identity, canon, sizeof canon) != FIPS_IDENTITY_OK) canon[0] = 0;
    if (fips_peer_identity_short_npub(&pair.identity, short_npub, sizeof short_npub) != FIPS_IDENTITY_OK) short_npub[0] = 0;
    if (fipsd_lan_known_labels_is_impostor(&v->known, canon, &pair.identity.node_addr)) {
        FIPSD_DEBUG(MODULE, "lan: advert refused; its host label is a known peer's under another npub npub=%s addr=%s reason=known-label", short_npub, text);
        post_peer(s, FIPSD_MDNS_EVENT_SKIPPED, f->npub, f, addr, ifaces, nif, "known-label", NULL);
        return;
    }
    for (size_t i = 0; i < nif && i < FIPSD_MDNS_MAX_IFACES; ++i) pair.ifaces[pair.iface_count++] = ifaces[i];
    fipsd_lan_pair displaced; bool first = false;
    switch (fipsd_lan_offer_pair(&s->lan, &pair, nodes(v->configured, v->nconfigured), now, rng, &displaced, &first)) {
    case FIPSD_LAN_HELD: case FIPSD_LAN_DUPLICATE: break;
    case FIPSD_LAN_REFUSED_PER_NODE:
        FIPSD_DEBUG(MODULE, "lan: advert refused; node already holds its pending addresses npub=%s addr=%s reason=per-node-limit", short_npub, text);
        post_peer(s, FIPSD_MDNS_EVENT_SKIPPED, f->npub, f, addr, ifaces, nif, "per-node-limit", NULL);
        break;
    case FIPSD_LAN_REPLACED: {
        log_set_full(first);
        char d[80]; (void)fipsd_mdns_sockaddr_text(&displaced.addr, d, sizeof d);
        FIPSD_DEBUG(MODULE, "lan: pending candidate displaced by a newer advert addr=%s reason=set-full", d);
        break;
    }
    case FIPSD_LAN_REFUSED:
        log_set_full(first);
        FIPSD_DEBUG(MODULE, "lan: advert refused; candidate set full of configured peers npub=%s addr=%s reason=set-full", short_npub, text);
        post_peer(s, FIPSD_MDNS_EVENT_SKIPPED, f->npub, f, addr, ifaces, nif, "set-full", NULL);
        break;
    }
}
/* One resolved advert: upstream's event pump (check_advert, one discovered
 * peer per usable address), then the node's handling of the discoveries. */
static void on_resolved(fipsd_mdns_service *s, const fipsd_mdns_resolved *r, uint64_t now)
{
    fipsd_mdns_advert_fields f;
    fipsd_mdns_skip skip = fipsd_mdns_check_advert(r, s->npub, s->has_scope ? s->scope : NULL, &f);
    if (skip != FIPSD_MDNS_ADVERT_OK) {
        static const char *const reasons[] = {"", "no-npub", "own", "cross-scope", "foreign-host", "zero-port"};
        char line[1024];
        if (*fipsd_mdns_skip_line(r, skip, &f, s->has_scope ? s->scope : NULL, line, sizeof line))
            FIPSD_DEBUG(MODULE, "%s", line);
        post_peer(s, FIPSD_MDNS_EVENT_SKIPPED, f.npub_len ? f.npub : NULL, &f, NULL, NULL, 0, reasons[skip], r->fullname);
        return;
    }
    peer_view *v = peer_view_load(s);
    if (!v) return;
    fipsd_lan_prune_connected(&s->lan, nodes(v->connected, v->nconnected));
    for (size_t i = 0; i < r->addr_count; ++i) {
        fipsd_mdns_sockaddr addr; uint32_t ifaces[FIPSD_MDNS_MAX_IFACES];
        size_t nif = fipsd_mdns_arrival_interfaces(&r->addrs[i], ifaces);
        if (!fipsd_mdns_socket_addr(&r->addrs[i], f.port, &addr)) {
            FIPSD_DEBUG(MODULE, "lan: skip scope-less IPv6 link-local advert npub=%.*s",
                        (int)fipsd_mdns_short_len(f.npub, f.npub_len), f.npub);
            post_peer(s, FIPSD_MDNS_EVENT_SKIPPED, f.npub, &f, &addr, NULL, 0, "scope-less-link-local", r->fullname);
            continue;
        }
        post_peer(s, FIPSD_MDNS_EVENT_DISCOVERED, f.npub, &f, &addr, ifaces, nif, NULL, r->fullname);
        offer_discovered(s, v, &f, &addr, ifaces, nif, now);
    }
    dial_pending(s, v, now);
    free(v);
}
static void drain_browser(fipsd_mdns_service *s, uint64_t now)
{
    char name[FIPSD_MDNS_TEXT_MAX];
    while (fipsd_mdns_browser_next_removed(s->browser, name, sizeof name)) {
        FIPSD_DEBUG(MODULE, "lan: service removed fullname=%s", name);
        post_peer(s, FIPSD_MDNS_EVENT_REMOVED, NULL, NULL, NULL, NULL, 0, NULL, name);
    }
    while (fipsd_mdns_browser_next_resolved(s->browser, s->resolved)) on_resolved(s, s->resolved, now);
}
static void lan_tick(fipsd_mdns_service *s, uint64_t now)
{
    uint64_t refused = 0;
    if (fipsd_lan_expire(&s->lan, now, &refused))
        FIPSD_INFO(MODULE, "LAN discovery candidate set below its bound again refused=%llu", (unsigned long long)refused);
    peer_view *v = peer_view_load(s);
    if (!v) { s->lan_next_ms = now + LAN_RETRY_MS; return; }
    dial_pending(s, v, now);
    free(v);
}

/* ---- network and port --------------------------------------------- */
static bool same_addrs(const fipsd_mdns_iface_addrs *a, const fipsd_mdns_iface_addrs *b)
{
    return a->v4_count == b->v4_count && a->v6_count == b->v6_count &&
        !memcmp(a->v4, b->v4, sizeof a->v4[0] * a->v4_count) && !memcmp(a->v6, b->v6, sizeof a->v6[0] * a->v6_count);
}
static void apply_network(fipsd_mdns_service *s, uint64_t now)
{
    GoRefPtr ref = fipsd_netmon_snapshot(s->netmon);
    const fipsd_network_snapshot *snap = ref.ptr;
    if (!snap || (s->have_net && snap->generation == s->net_generation)) { if (ref.ptr) go_refptr_release(&ref); return; }
    fipsd_mdns_iface_addrs next[FIPSD_MDNS_MAX_INTERFACES]; size_t n = 0;
    for (size_t i = 0; i < snap->interface_count && n < FIPSD_MDNS_MAX_INTERFACES; ++i) {
        const fipsd_interface *itf = &snap->interfaces[i];
        if (!itf->up || itf->loopback) continue;
        fipsd_mdns_iface_addrs a = {.index = itf->index};
        for (size_t k = 0; k < snap->address_count; ++k) {
            const fipsd_interface_address *ad = &snap->addresses[k];
            if (ad->index != itf->index) continue;
            if (ad->ip.width == 4 && a.v4_count < FIPSD_MDNS_IFACE_ADDRS) memcpy(a.v4[a.v4_count++], ad->ip.bytes, 4);
            if (ad->ip.width == 16 && a.v6_count < FIPSD_MDNS_IFACE_ADDRS) memcpy(a.v6[a.v6_count++], ad->ip.bytes, 16);
        }
        if (a.v4_count || a.v6_count) next[n++] = a;
    }
    bool next_joined[FIPSD_MDNS_MAX_INTERFACES][2] = {{false}};
    bool added = false;
    for (size_t i = 0; i < s->iface_count; ++i) {
        const fipsd_mdns_iface_addrs *old = &s->ifaces[i];
        size_t k = 0;
        while (k < n && next[k].index != old->index) ++k;
        bool v4_same = k < n && next[k].v4_count && old->v4_count && !memcmp(next[k].v4[0], old->v4[0], 4);
        if (s->joined[i][0] && !v4_same) s->io.leave(s->io.user, old->index, AF_INET, old->v4[0]);
        else if (s->joined[i][0]) next_joined[k][0] = true;
        if (s->joined[i][1] && (k == n || !next[k].v6_count)) s->io.leave(s->io.user, old->index, AF_INET6, old->v6[0]);
        else if (s->joined[i][1]) next_joined[k][1] = true;
        if (k == n) { FIPSD_DEBUG(MODULE, "lan: interface %u left", old->index); fipsd_mdns_browser_forget_interface(s->browser, old->index); }
        else if (!same_addrs(old, &next[k])) FIPSD_DEBUG(MODULE, "lan: interface %u addresses changed", old->index);
    }
    fipsd_mdns_iface_addrs usable[FIPSD_MDNS_MAX_INTERFACES]; size_t nu = 0;
    bool usable_joined[FIPSD_MDNS_MAX_INTERFACES][2];
    for (size_t k = 0; k < n; ++k) {
        bool known = iface_slot(s, next[k].index) != SIZE_MAX;
        if (next[k].v4_count && !next_joined[k][0]) next_joined[k][0] = s->io.join(s->io.user, next[k].index, AF_INET, next[k].v4[0]);
        if (next[k].v6_count && !next_joined[k][1]) next_joined[k][1] = s->io.join(s->io.user, next[k].index, AF_INET6, next[k].v6[0]);
        if (!next_joined[k][0] && !next_joined[k][1]) {
            if (!known) FIPSD_DEBUG(MODULE, "lan: interface %u has no multicast membership; not used", next[k].index);
            continue;
        }
        if (!known) { added = true; FIPSD_DEBUG(MODULE, "lan: interface %u joined (ipv4=%d ipv6=%d)", next[k].index, next_joined[k][0], next_joined[k][1]); }
        usable[nu] = next[k];
        usable_joined[nu][0] = next_joined[k][0]; usable_joined[nu][1] = next_joined[k][1];
        ++nu;
    }
    memcpy(s->ifaces, usable, sizeof usable[0] * nu);
    memcpy(s->joined, usable_joined, sizeof usable_joined[0] * nu);
    s->iface_count = nu;
    if (s->have_net) go_refptr_release(&s->net);
    s->net = ref; s->have_net = true; s->net_generation = snap->generation;
    fipsd_mdns_responder_set_interfaces(s->responder, s->ifaces, s->iface_count, now);
    if (added && s->port) fipsd_mdns_browser_restart(s->browser, now);
    if (s->lan.count) s->lan_next_ms = now; /* re-judge held candidates against the new links */
}
static void update_port(fipsd_mdns_service *s, uint64_t now)
{
    uint16_t port = 0;
    if (!lowest_udp(s, AF_UNSPEC, &port) || !port || port == s->port) return;
    bool first = !s->port;
    s->port = port;
    fipsd_mdns_responder_set_port(s->responder, port, now);
    if (first) {
        fipsd_mdns_browser_restart(s->browser, now);
        FIPSD_INFO(MODULE, "lan: mDNS discovery started instance=%s.%s port=%u scope=%s", fipsd_mdns_responder_label(s->responder),
                   s->service_type, port, s->has_scope ? s->scope : "(none)");
        fipsd_mdns_event e = {.kind = FIPSD_MDNS_EVENT_STARTED, .addr = {.port = port}};
        snprintf(e.npub, sizeof e.npub, "%s", s->npub);
        post(s, &e);
    } else FIPSD_INFO(MODULE, "lan: advertised UDP port changed port=%u", port);
}

/* ---- the engine ---------------------------------------------------- */
static void handle_datagram(fipsd_mdns_service *s, fipsd_mdns_datagram *d, uint64_t now)
{
    if (!s->port || !d->ifindex || iface_slot(s, d->ifindex) == SIZE_MAX) return;
    if (!fipsd_mdns_decode(d->bytes, d->len, s->msg)) return;
    /* RFC 6762 Section 6: a response not from port 5353 is ignored. */
    if (fipsd_mdns_is_response(s->msg) && d->src.port != FIPSD_MDNS_PORT) return;
    /* Only responses and probes are judged as our own echo: they carry our
     * names, so another node never sends the same bytes. A plain browse query
     * from another node on this host can be byte-identical to ours, and must
     * still be answered. */
    bool own = (fipsd_mdns_is_response(s->msg) || s->msg->nscount) && is_own(s, d->bytes, d->len, now);
    fipsd_mdns_responder_on_message(s->responder, s->msg, d->ifindex, &d->src, own, now);
    if (fipsd_mdns_is_response(s->msg)) {
        fipsd_mdns_browser_ingest(s->browser, s->msg, d->ifindex, now);
        drain_browser(s, now);
    }
}
static void run_due(fipsd_mdns_service *s, uint64_t now)
{
    size_t n = fipsd_mdns_responder_poll(s->responder, now, s->outs, OUT_MAX);
    send_outs(s, n, now);
    if (!s->port) return;
    fipsd_mdns_browser_expire(s->browser, now);
    drain_browser(s, now);
    uint8_t query[FIPSD_MDNS_MAX_PACKET];
    size_t len = fipsd_mdns_browser_query(s->browser, now, query, sizeof query);
    if (len) for (size_t i = 0; i < s->iface_count; ++i) send_multicast(s, i, query, len, now);
    if (s->lan_next_ms <= now) lan_tick(s, now);
}
static uint64_t next_deadline(const fipsd_mdns_service *s)
{
    uint64_t at = fipsd_mdns_responder_deadline(s->responder);
    if (s->port) {
        uint64_t b = fipsd_mdns_browser_deadline(s->browser);
        if (b < at) at = b;
        if (s->lan_next_ms < at) at = s->lan_next_ms;
        uint64_t e = fipsd_lan_next_expiry(&s->lan);
        if (e < at) at = e;
    }
    return at;
}
static void engine(void *arg)
{
    fipsd_mdns_service *s = arg;
    gof_set_name("mdns");
    GoChannel *network = fipsd_netmon_events(s->netmon);
    GoChannel *done = go_context_done(fipsd_runtime_context(s->rt));
    uint64_t now = mono_ms();
    apply_network(s, now);
    update_port(s, now);
    run_due(s, now);
    for (;;) {
        void *value = NULL, *cancel = NULL;
        GoSelectCase cases[] = {
            {GO_SELECT_RECEIVE, s->stop, NULL, &cancel},
            {GO_SELECT_RECEIVE, done, NULL, &cancel},
            {GO_SELECT_RECEIVE, s->rx, NULL, &value},
            {GO_SELECT_RECEIVE, network, NULL, &value},
            {GO_SELECT_RECEIVE, s->bound, NULL, &value}};
        now = mono_ms();
        uint64_t at = next_deadline(s);
        int selected;
        if (at == UINT64_MAX) selected = go_select(cases, 5);
        else selected = go_select_timeout(cases, 5, at > now ? at - now : 0).selected_case;
        if (selected == 0 || selected == 1) break;
        now = mono_ms();
        if (selected == 2 && value) {
            handle_datagram(s, value, now); free(value);
            for (unsigned k = 1; k < RX_BATCH && go_channel_try_receive(s->rx, &value) == 0; ++k) { handle_datagram(s, value, now); free(value); }
        } else if (selected == 3) {
            if (!value) network = NULL; /* the monitor stopped */
            else {
                const fipsd_netmon_event *e = value;
                if (e->kind == FIPSD_INTERFACE_UP || e->kind == FIPSD_INTERFACE_DOWN || e->kind == FIPSD_INTERFACE_ADDR_CHANGE ||
                    (e->kind == FIPSD_NETWORK_REFRESH && !s->have_net)) apply_network(s, now);
                free(value);
            }
        } else if (selected == 4) update_port(s, now);
        run_due(s, now);
    }
    /* Goodbyes for every record (RFC 6762 Section 10.1), then the sockets. */
    size_t n = fipsd_mdns_responder_goodbye(s->responder, s->outs, OUT_MAX);
    send_outs(s, n, mono_ms());
    for (size_t i = 0; i < s->iface_count; ++i) {
        if (s->joined[i][0]) s->io.leave(s->io.user, s->ifaces[i].index, AF_INET, s->ifaces[i].v4[0]);
        if (s->joined[i][1]) s->io.leave(s->io.user, s->ifaces[i].index, AF_INET6, s->ifaces[i].v6[0]);
    }
    s->io.close(s->io.user);
    FIPSD_INFO(MODULE, "lan: mDNS discovery stopped");
}

static int start(fipsd_subsystem *subsystem, fipsd_runtime *rt)
{
    fipsd_mdns_service *s = subsystem->user;
    s->rt = rt;
    GoRefPtr cfg = go_swap_load(s->configs);
    const fipsd_config *config = cfg.ptr;
    const char *type = fipsd_config_string(fipsd_config_get(config, "node.rendezvous.lan.service_type"));
    snprintf(s->service_type, sizeof s->service_type, "%s", type ? type : FIPSD_MDNS_SERVICE_TYPE);
    s->has_scope = fipsd_mdns_resolve_scope(fipsd_config_string(fipsd_config_get(config, "node.rendezvous.lan.scope")),
                                            fipsd_config_string(fipsd_config_get(config, "node.rendezvous.nostr.app")),
                                            s->scope, sizeof s->scope);
    uint64_t hs = fipsd_config_uint(fipsd_config_get(config, "node.rate_limit.handshake_timeout_secs"));
    s->handshake_timeout_ms = (hs ? hs : 30) * 1000u;
    go_refptr_release(&cfg);
    size_t tl = strlen(s->service_type);
    if (tl < 8 || strcmp(s->service_type + tl - 7, ".local.")) {
        FIPSD_WARN(MODULE, "LAN mDNS discovery not started: service type %s must end with .local.", s->service_type);
        return 0;
    }
    bool any_udp = false;
    for (size_t i = 0; i < s->transports->count; ++i) any_udp |= !strcmp(s->transports->items[i]->type, "udp");
    if (!any_udp) {
        FIPSD_WARN(MODULE, "LAN mDNS discovery not started: no advertised port, configure a UDP transport");
        return 0;
    }
    fips_xonly_pubkey key = fips_identity_pubkey(fipsd_node_identity(s->ctx));
    if (fips_encode_npub(&key, s->npub, sizeof s->npub) != FIPS_IDENTITY_OK) { FIPSD_ERROR(MODULE, "cannot encode this node's npub"); return -1; }
    fipsd_mdns_advert_label(s->npub, s->label, sizeof s->label);
    fipsd_mdns_txt_entry txt[3]; size_t ntxt = 0;
    txt[ntxt++] = (fipsd_mdns_txt_entry){FIPSD_MDNS_TXT_NPUB, s->npub};
    if (s->has_scope && *s->scope) txt[ntxt++] = (fipsd_mdns_txt_entry){FIPSD_MDNS_TXT_SCOPE, s->scope};
    txt[ntxt++] = (fipsd_mdns_txt_entry){FIPSD_MDNS_TXT_VERSION, FIPSD_MDNS_TXT_PROTOCOL_VERSION};
    fipsd_mdns_responder_config rc = {.service_type = s->service_type, .label = s->label, .txt = txt, .txt_count = ntxt, .rng = rng};
    uint64_t now = mono_ms();
    s->responder = fipsd_mdns_responder_create(&rc);
    s->browser = fipsd_mdns_browser_create(s->service_type, rng, now);
    s->msg = malloc(sizeof *s->msg);
    s->outs = malloc(sizeof *s->outs * OUT_MAX);
    s->resolved = malloc(sizeof *s->resolved);
    if (!s->responder || !s->browser || !s->msg || !s->outs || !s->resolved) {
        FIPSD_WARN(MODULE, "LAN mDNS discovery not started: invalid service type %s or out of memory", s->service_type);
        return 0;
    }
    s->netmon = fipsd_netmon_create(s->has_netmon_source ? &s->netmon_source : NULL);
    if (!s->netmon) { FIPSD_ERROR(MODULE, "cannot create the network monitor"); return -1; }
    if (s->io.open(s->io.user, rt, s->rx) != 0) {
        FIPSD_WARN(MODULE, "LAN mDNS discovery not started: cannot open the mDNS sockets on port %u", FIPSD_MDNS_PORT);
        return 0;
    }
    if (fipsd_netmon_start(s->netmon, rt) != 0 || fipsd_runtime_spawn(rt, engine, s) != 0) {
        FIPSD_ERROR(MODULE, "cannot start the mDNS engine");
        s->io.close(s->io.user);
        return -1;
    }
    s->running = true;
    return 0;
}
static void stop(fipsd_subsystem *subsystem, fipsd_runtime *rt)
{
    (void)rt;
    fipsd_mdns_service *s = subsystem->user;
    if (!s->running) return;
    go_channel_close(s->stop);
    fipsd_netmon_stop(s->netmon);
}
fipsd_mdns_service *fipsd_mdns_service_create(GoSwap *configs, fipsd_node *node, fipsd_node_handlers *handlers)
{
    if (sodium_init() < 0) return NULL;
    fipsd_mdns_service *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->configs = configs; s->node = node; s->handlers = handlers;
    s->ctx = fipsd_node_get_context(node);
    s->transports = fipsd_node_transports(node);
    s->lan_next_ms = UINT64_MAX;
    fipsd_mdns_sockets_init(&s->sockets);
    s->io = fipsd_mdns_sockets_io(&s->sockets);
    s->rx = go_channel_create(RX_QUEUE);
    s->bound = go_channel_create(8);
    s->stop = go_channel_create(1);
    if (!s->rx || !s->bound || !s->stop) { fipsd_mdns_service_destroy(s); return NULL; }
    for (size_t i = 0; i < s->transports->count; ++i)
        if (!strcmp(s->transports->items[i]->type, "udp")) (void)fipsd_udp_watch_bound(s->transports->items[i], s->bound);
    s->subsystem = (fipsd_subsystem){.name = "mdns", .start = start, .stop = stop, .user = s};
    return s;
}
fipsd_subsystem *fipsd_mdns_service_subsystem(fipsd_mdns_service *s) { return &s->subsystem; }
static void drain_free(GoChannel *c)
{
    void *v;
    if (!c) return;
    while (go_channel_try_receive(c, &v) == 0) free(v);
}
void fipsd_mdns_service_destroy(fipsd_mdns_service *s)
{
    if (!s) return;
    if (s->have_net) go_refptr_release(&s->net);
    fipsd_netmon_destroy(s->netmon);
    fipsd_mdns_sockets_release(&s->sockets);
    drain_free(s->rx);
    if (s->rx) go_channel_unref(s->rx);
    if (s->bound) { void *v; while (go_channel_try_receive(s->bound, &v) == 0) {} go_channel_unref(s->bound); }
    if (s->stop) go_channel_unref(s->stop);
    fipsd_mdns_responder_destroy(s->responder);
    fipsd_mdns_browser_destroy(s->browser);
    free(s->msg); free(s->outs); free(s->resolved);
    free(s);
}
void fipsd_mdns_service_set_io(fipsd_mdns_service *s, const fipsd_mdns_io *io) { s->io = *io; }
void fipsd_mdns_service_set_netmon(fipsd_mdns_service *s, const fipsd_netmon_source *source)
{ s->has_netmon_source = source != NULL; if (source) s->netmon_source = *source; }
void fipsd_mdns_service_observe(fipsd_mdns_service *s, GoChannel *events) { s->events = events; }
