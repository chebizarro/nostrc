#include "native/registry.h"
#include "native/protocol.h"
#include <fips/identity/node_addr.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct reg_flow {
    bool used, inbound;
    uint64_t id, at;
    fipsd_native_flow_key key;
    fips_xonly_pubkey pubkey;
    void *sink;
} reg_flow;
typedef struct reg_pending {
    bool used;
    uint64_t id, at;
    fipsd_native_flow_key key;
    fips_xonly_pubkey pubkey;
    uint16_t listener;
    fipsd_native_datagram **held;
    size_t held_len;
} reg_pending;
typedef struct reg_listener {
    uint16_t port;
    void *arrivals;
    uint64_t *pending;
    size_t pending_len, pending_cap;
} reg_listener;
#define OWNER_FREE 0u
#define OWNER_LISTENER UINT32_MAX
struct fipsd_native_registry {
    fipsd_native_limits limits;
    uint32_t (*random)(uint32_t, void *);
    void *user;
    reg_flow *flows;
    reg_pending *pending;
    size_t cap, flow_count, pending_count;
    reg_listener *listeners;
    size_t listener_count, listener_cap;
    uint32_t *owner; /* 65536: OWNER_FREE, OWNER_LISTENER or flow index + 1 */
    size_t ports_held;
    uint64_t next_flow;
};

bool fipsd_native_flow_key_eq(const fipsd_native_flow_key *a, const fipsd_native_flow_key *b)
{
    return a->remote == b->remote && a->local == b->local && !memcmp(a->peer.bytes, b->peer.bytes, 16);
}
size_t fipsd_native_limits_per_peer(const fipsd_native_limits *limits)
{
    size_t share = limits->max_flows / FIPSD_NATIVE_PEER_SHARE;
    return share ? share : 1;
}
const char *fipsd_native_registry_errno(fipsd_native_registry_error error)
{
    switch (error) {
    case FIPSD_NATIVE_REG_PORT_TAKEN: case FIPSD_NATIVE_REG_FLOW_TAKEN: return "EADDRINUSE";
    case FIPSD_NATIVE_REG_NO_PORT: case FIPSD_NATIVE_REG_PORT_PROTOCOL: case FIPSD_NATIVE_REG_PORT_STANDARD: return "EADDRNOTAVAIL";
    case FIPSD_NATIVE_REG_TOO_MANY_FLOWS: return "EMFILE";
    /* Neither reaches a setup reply today; if one does, the daemon refused
     * for a reason of its own, which is what the catch-all names. */
    case FIPSD_NATIVE_REG_BACKLOG_FULL: case FIPSD_NATIVE_REG_NO_PENDING: default: return "ECONNREFUSED";
    }
}
void fipsd_native_refusal_message(const fipsd_native_refusal *r, char *out, size_t cap)
{
    switch (r->error) {
    case FIPSD_NATIVE_REG_PORT_TAKEN: snprintf(out, cap, "port %u is already in use on this node", (unsigned)r->port); break;
    case FIPSD_NATIVE_REG_FLOW_TAKEN:
        snprintf(out, cap, "a flow to that peer between ports %u and %u already exists", (unsigned)r->local, (unsigned)r->remote); break;
    case FIPSD_NATIVE_REG_NO_PORT: snprintf(out, cap, "no ephemeral port is free"); break;
    case FIPSD_NATIVE_REG_TOO_MANY_FLOWS: snprintf(out, cap, "this node holds its maximum of %zu flows", r->max); break;
    case FIPSD_NATIVE_REG_BACKLOG_FULL: snprintf(out, cap, "listener on port %u has a full backlog", (unsigned)r->port); break;
    case FIPSD_NATIVE_REG_NO_PENDING: snprintf(out, cap, "no pending flow %llu", (unsigned long long)r->flow); break;
    case FIPSD_NATIVE_REG_PORT_PROTOCOL: case FIPSD_NATIVE_REG_PORT_STANDARD:
        (void)fipsd_native_check_bindable(r->port, out, cap); break;
    default: snprintf(out, cap, "ok"); break;
    }
}
fipsd_native_datagram *fipsd_native_datagram_new(const uint8_t *data, size_t len)
{
    fipsd_native_datagram *d = malloc(sizeof *d + len);
    if (!d) return NULL;
    d->len = len;
    if (len) memcpy(d->bytes, data, len);
    return d;
}
void fipsd_native_datagram_free(fipsd_native_datagram *d)
{
    if (!d) return;
    sodium_memzero(d, sizeof *d + d->len);
    free(d);
}
static uint32_t sodium_random(uint32_t span, void *user)
{
    (void)user;
    return randombytes_uniform(span);
}
fipsd_native_registry *fipsd_native_registry_create(const fipsd_native_limits *limits,
                                                    uint32_t (*random)(uint32_t, void *), void *user)
{
    if (!limits->max_flows) return NULL;
    if (!random && sodium_init() < 0) return NULL;
    fipsd_native_registry *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->limits = *limits; r->random = random ? random : sodium_random; r->user = user;
    r->cap = limits->max_flows; r->next_flow = 1;
    r->flows = calloc(r->cap, sizeof *r->flows);
    r->pending = calloc(r->cap, sizeof *r->pending);
    r->owner = calloc(65536, sizeof *r->owner);
    if (!r->flows || !r->pending || !r->owner) { fipsd_native_registry_destroy(r); return NULL; }
    return r;
}
static void pending_clear(reg_pending *p)
{
    for (size_t i = 0; i < p->held_len; ++i) fipsd_native_datagram_free(p->held[i]);
    free(p->held);
    memset(p, 0, sizeof *p);
}
void fipsd_native_registry_destroy(fipsd_native_registry *r)
{
    if (!r) return;
    for (size_t i = 0; r->pending && i < r->cap; ++i) if (r->pending[i].used) pending_clear(&r->pending[i]);
    for (size_t i = 0; i < r->listener_count; ++i) free(r->listeners[i].pending);
    free(r->listeners); free(r->flows); free(r->pending); free(r->owner);
    free(r);
}
size_t fipsd_native_registry_flow_count(const fipsd_native_registry *r) { return r->flow_count + r->pending_count; }
size_t fipsd_native_registry_port_count(const fipsd_native_registry *r) { return r->ports_held; }
static int by_flow(const void *a, const void *b)
{
    uint64_t x = ((const fipsd_native_flow_view *)a)->flow, y = ((const fipsd_native_flow_view *)b)->flow;
    return x < y ? -1 : x > y;
}
static int by_port(const void *a, const void *b)
{
    return (int)((const fipsd_native_listener_view *)a)->port - (int)((const fipsd_native_listener_view *)b)->port;
}
size_t fipsd_native_registry_flows(const fipsd_native_registry *r, fipsd_native_flow_view *out, size_t cap,
                                   size_t (*queued)(void *, void *), void *user)
{
    size_t n = 0;
    for (size_t i = 0; i < r->cap; ++i) {
        const reg_flow *f = &r->flows[i];
        if (!f->used) continue;
        if (n < cap) out[n] = (fipsd_native_flow_view){.flow = f->id, .key = f->key, .pubkey = f->pubkey,
            .established = true, .queued = queued ? queued(f->sink, user) : 0, .at = f->at};
        ++n;
    }
    for (size_t i = 0; i < r->cap; ++i) {
        const reg_pending *p = &r->pending[i];
        if (!p->used) continue;
        if (n < cap) out[n] = (fipsd_native_flow_view){.flow = p->id, .key = p->key, .pubkey = p->pubkey,
            .established = false, .queued = p->held_len, .at = p->at};
        ++n;
    }
    /* Counting callers pass out == NULL, cap == 0: qsort needs a valid base even for zero elements. */
    size_t filled = n < cap ? n : cap;
    if (filled > 1) qsort(out, filled, sizeof *out, by_flow);
    return n;
}
size_t fipsd_native_registry_listeners(const fipsd_native_registry *r, fipsd_native_listener_view *out, size_t cap)
{
    for (size_t i = 0; i < r->listener_count && i < cap; ++i)
        out[i] = (fipsd_native_listener_view){.port = r->listeners[i].port, .backlog = r->listeners[i].pending_len};
    size_t filled = r->listener_count < cap ? r->listener_count : cap;
    if (filled > 1) qsort(out, filled, sizeof *out, by_port);
    return r->listener_count;
}
static reg_listener *listener_find(fipsd_native_registry *r, uint16_t port)
{
    for (size_t i = 0; i < r->listener_count; ++i) if (r->listeners[i].port == port) return &r->listeners[i];
    return NULL;
}
static reg_flow *flow_find(fipsd_native_registry *r, const fipsd_native_flow_key *key)
{
    for (size_t i = 0; i < r->cap; ++i)
        if (r->flows[i].used && fipsd_native_flow_key_eq(&r->flows[i].key, key)) return &r->flows[i];
    return NULL;
}
static reg_pending *pending_find(fipsd_native_registry *r, uint64_t id)
{
    for (size_t i = 0; i < r->cap; ++i) if (r->pending[i].used && r->pending[i].id == id) return &r->pending[i];
    return NULL;
}
static reg_pending *pending_for(fipsd_native_registry *r, const fipsd_native_flow_key *key)
{
    for (size_t i = 0; i < r->cap; ++i)
        if (r->pending[i].used && fipsd_native_flow_key_eq(&r->pending[i].key, key)) return &r->pending[i];
    return NULL;
}
static bool port_held(const fipsd_native_registry *r, uint16_t port) { return r->owner[port] != OWNER_FREE; }
static void port_claim(fipsd_native_registry *r, uint16_t port, uint32_t owner) { r->owner[port] = owner; ++r->ports_held; }
static void port_free(fipsd_native_registry *r, uint16_t port) { r->owner[port] = OWNER_FREE; --r->ports_held; }
/* RFC 6056 algorithm 1: a random start, probing forward and wrapping once. */
static bool take_ephemeral(fipsd_native_registry *r, uint16_t *out)
{
    const uint32_t span = 65536u - FIPSD_NATIVE_PORT_EPHEMERAL_MIN;
    uint32_t start = r->random(span, r->user) % span;
    for (uint32_t step = 0; step < span; ++step) {
        uint16_t port = (uint16_t)((start + step) % span + FIPSD_NATIVE_PORT_EPHEMERAL_MIN);
        if (!port_held(r, port)) { *out = port; return true; }
    }
    return false;
}
static fipsd_native_registry_error tier(uint16_t port, fipsd_native_refusal *why)
{
    fipsd_native_command_error e = fipsd_native_check_bindable(port, NULL, 0);
    if (e == FIPSD_NATIVE_CMD_OK) return FIPSD_NATIVE_REG_OK;
    why->error = e == FIPSD_NATIVE_CMD_PORT_PROTOCOL ? FIPSD_NATIVE_REG_PORT_PROTOCOL : FIPSD_NATIVE_REG_PORT_STANDARD;
    why->port = port;
    return why->error;
}
fipsd_native_registry_error fipsd_native_registry_listen(fipsd_native_registry *r, bool has_port, uint16_t port,
                                                         void *arrivals, uint16_t *held, fipsd_native_refusal *why)
{
    fipsd_native_refusal scratch; if (!why) why = &scratch;
    memset(why, 0, sizeof *why);
    if (has_port) {
        if (tier(port, why) != FIPSD_NATIVE_REG_OK) return why->error;
        if (port_held(r, port)) { why->error = FIPSD_NATIVE_REG_PORT_TAKEN; why->port = port; return why->error; }
    } else if (!take_ephemeral(r, &port)) { why->error = FIPSD_NATIVE_REG_NO_PORT; return why->error; }
    if (r->listener_count == r->listener_cap) {
        size_t cap = r->listener_cap ? r->listener_cap * 2 : 8;
        reg_listener *grown = realloc(r->listeners, cap * sizeof *grown);
        if (!grown) { why->error = FIPSD_NATIVE_REG_NO_PORT; return why->error; }
        r->listeners = grown; r->listener_cap = cap;
    }
    r->listeners[r->listener_count++] = (reg_listener){.port = port, .arrivals = arrivals};
    port_claim(r, port, OWNER_LISTENER);
    *held = port;
    return FIPSD_NATIVE_REG_OK;
}
fipsd_native_registry_error fipsd_native_registry_connect(fipsd_native_registry *r, const fips_xonly_pubkey *peer,
                                                          uint16_t remote, bool has_local, uint16_t local, void *sink,
                                                          uint64_t now, uint16_t *held, uint64_t *flow,
                                                          fipsd_native_refusal *why)
{
    fipsd_native_refusal scratch; if (!why) why = &scratch;
    memset(why, 0, sizeof *why);
    if (tier(remote, why) != FIPSD_NATIVE_REG_OK) return why->error;
    if (fipsd_native_registry_flow_count(r) >= r->limits.max_flows) {
        why->error = FIPSD_NATIVE_REG_TOO_MANY_FLOWS; why->max = r->limits.max_flows; return why->error;
    }
    if (has_local) {
        if (tier(local, why) != FIPSD_NATIVE_REG_OK) return why->error;
        if (port_held(r, local)) { why->error = FIPSD_NATIVE_REG_PORT_TAKEN; why->port = local; return why->error; }
    } else if (!take_ephemeral(r, &local)) { why->error = FIPSD_NATIVE_REG_NO_PORT; return why->error; }
    fipsd_native_flow_key key = {.peer = fips_node_addr_from_pubkey(peer), .remote = remote, .local = local};
    /* Checked before the port is claimed, so a refusal leaves nothing to undo. */
    if (flow_find(r, &key)) {
        why->error = FIPSD_NATIVE_REG_FLOW_TAKEN; why->local = local; why->remote = remote; return why->error;
    }
    size_t slot = r->cap;
    for (size_t i = 0; i < r->cap; ++i) if (!r->flows[i].used) { slot = i; break; }
    if (slot == r->cap) { why->error = FIPSD_NATIVE_REG_TOO_MANY_FLOWS; why->max = r->limits.max_flows; return why->error; }
    uint64_t id = r->next_flow++;
    r->flows[slot] = (reg_flow){.used = true, .inbound = false, .id = id, .at = now, .key = key, .pubkey = *peer, .sink = sink};
    ++r->flow_count;
    port_claim(r, local, (uint32_t)slot + 1);
    *held = local; *flow = id;
    return FIPSD_NATIVE_REG_OK;
}
bool fipsd_native_registry_key_of(const fipsd_native_registry *r, uint64_t flow, fipsd_native_flow_key *out)
{
    for (size_t i = 0; i < r->cap; ++i)
        if (r->flows[i].used && r->flows[i].id == flow) { *out = r->flows[i].key; return true; }
    return false;
}
static size_t inbound_from(const fipsd_native_registry *r, const fips_node_addr *peer)
{
    size_t n = 0;
    for (size_t i = 0; i < r->cap; ++i) {
        if (r->flows[i].used && r->flows[i].inbound && !memcmp(r->flows[i].key.peer.bytes, peer->bytes, 16)) ++n;
        if (r->pending[i].used && !memcmp(r->pending[i].key.peer.bytes, peer->bytes, 16)) ++n;
    }
    return n;
}
fipsd_native_delivery fipsd_native_registry_deliver(fipsd_native_registry *r, const fips_node_addr *peer,
                                                    const fips_xonly_pubkey *pubkey, uint16_t src, uint16_t dst, uint64_t now)
{
    fipsd_native_delivery d = {.kind = FIPSD_NATIVE_DELIVER_DROP, .cause = FIPSD_NATIVE_DROP_NO_PORT};
    fipsd_native_flow_key key = {.peer = *peer, .remote = src, .local = dst};
    reg_pending *p = pending_for(r, &key);
    if (p) { d.kind = FIPSD_NATIVE_DELIVER_PENDING; d.flow = p->id; return d; }
    reg_flow *f = flow_find(r, &key);
    if (f) { d.kind = FIPSD_NATIVE_DELIVER_FLOW; d.sink = f->sink; d.flow = f->id; return d; }
    reg_listener *l = listener_find(r, dst);
    if (!l) return d;
    if (fipsd_native_registry_flow_count(r) >= r->limits.max_flows) { d.cause = FIPSD_NATIVE_DROP_TOO_MANY_FLOWS; return d; }
    if (inbound_from(r, peer) >= fipsd_native_limits_per_peer(&r->limits)) { d.cause = FIPSD_NATIVE_DROP_PEER_SHARE; return d; }
    if (l->pending_len >= r->limits.backlog) { d.cause = FIPSD_NATIVE_DROP_BACKLOG_FULL; return d; }
    size_t slot = r->cap;
    for (size_t i = 0; i < r->cap; ++i) if (!r->pending[i].used) { slot = i; break; }
    if (slot == r->cap) { d.cause = FIPSD_NATIVE_DROP_TOO_MANY_FLOWS; return d; }
    if (l->pending_len == l->pending_cap) {
        size_t cap = l->pending_cap ? l->pending_cap * 2 : 8;
        uint64_t *grown = realloc(l->pending, cap * sizeof *grown);
        if (!grown) { d.cause = FIPSD_NATIVE_DROP_BACKLOG_FULL; return d; }
        l->pending = grown; l->pending_cap = cap;
    }
    uint64_t id = r->next_flow++;
    r->pending[slot] = (reg_pending){.used = true, .id = id, .at = now, .key = key, .pubkey = *pubkey, .listener = dst};
    ++r->pending_count;
    l->pending[l->pending_len++] = id;
    d.kind = FIPSD_NATIVE_DELIVER_ARRIVED; d.arrivals = l->arrivals; d.flow = id;
    d.arrival = (fipsd_native_arrival){.flow = id, .key = key, .pubkey = *pubkey};
    return d;
}
bool fipsd_native_registry_hold(fipsd_native_registry *r, uint64_t flow, fipsd_native_datagram *dg)
{
    reg_pending *p = pending_find(r, flow);
    if (!p || p->held_len >= r->limits.per_flow) return false;
    if (!p->held) {
        p->held = calloc(r->limits.per_flow, sizeof *p->held);
        if (!p->held) return false;
    }
    p->held[p->held_len++] = dg;
    return true;
}
static void listener_forget(fipsd_native_registry *r, uint16_t port, uint64_t flow)
{
    reg_listener *l = listener_find(r, port);
    if (!l) return;
    for (size_t i = 0; i < l->pending_len; ++i)
        if (l->pending[i] == flow) { memmove(l->pending + i, l->pending + i + 1, (l->pending_len - i - 1) * sizeof *l->pending); --l->pending_len; return; }
}
fipsd_native_registry_error fipsd_native_registry_accept(fipsd_native_registry *r, uint64_t flow, void *sink, uint64_t now,
                                                         fipsd_native_flow_key *key, fips_xonly_pubkey *pubkey,
                                                         fipsd_native_datagram ***held, size_t *held_count)
{
    reg_pending *p = pending_find(r, flow);
    if (!p) return FIPSD_NATIVE_REG_NO_PENDING;
    size_t slot = r->cap;
    for (size_t i = 0; i < r->cap; ++i) if (!r->flows[i].used) { slot = i; break; }
    if (slot == r->cap) return FIPSD_NATIVE_REG_TOO_MANY_FLOWS;
    listener_forget(r, p->listener, flow);
    *key = p->key; *pubkey = p->pubkey; *held = p->held; *held_count = p->held_len;
    p->held = NULL; p->held_len = 0;
    r->flows[slot] = (reg_flow){.used = true, .inbound = true, .id = flow, .at = now, .key = p->key, .pubkey = p->pubkey, .sink = sink};
    ++r->flow_count;
    memset(p, 0, sizeof *p); --r->pending_count;
    return FIPSD_NATIVE_REG_OK;
}
fipsd_native_registry_error fipsd_native_registry_reject(fipsd_native_registry *r, uint64_t flow, fipsd_native_flow_key *key)
{
    reg_pending *p = pending_find(r, flow);
    if (!p) return FIPSD_NATIVE_REG_NO_PENDING;
    listener_forget(r, p->listener, flow);
    if (key) *key = p->key;
    pending_clear(p); --r->pending_count;
    return FIPSD_NATIVE_REG_OK;
}
bool fipsd_native_registry_release(fipsd_native_registry *r, const fipsd_native_flow_key *key)
{
    reg_flow *f = flow_find(r, key);
    if (!f) return false;
    size_t slot = (size_t)(f - r->flows);
    if (r->owner[key->local] == (uint32_t)slot + 1) port_free(r, key->local);
    memset(f, 0, sizeof *f); --r->flow_count;
    return true;
}
void fipsd_native_registry_release_listener(fipsd_native_registry *r, uint16_t port)
{
    if (r->owner[port] == OWNER_LISTENER) port_free(r, port);
    reg_listener *l = listener_find(r, port);
    if (!l) return;
    for (size_t i = 0; i < l->pending_len; ++i) {
        reg_pending *p = pending_find(r, l->pending[i]);
        if (p) { pending_clear(p); --r->pending_count; }
    }
    free(l->pending);
    size_t at = (size_t)(l - r->listeners);
    memmove(r->listeners + at, r->listeners + at + 1, (r->listener_count - at - 1) * sizeof *r->listeners);
    --r->listener_count;
}
size_t fipsd_native_registry_expire(fipsd_native_registry *r, uint64_t now, uint64_t *expired, size_t cap)
{
    size_t n = 0;
    for (size_t i = 0; i < r->cap; ++i) {
        reg_pending *p = &r->pending[i];
        if (!p->used || now - p->at < FIPSD_NATIVE_PENDING_DEADLINE_MS || now < p->at) continue;
        if (n < cap) expired[n] = p->id;
        ++n;
        listener_forget(r, p->listener, p->id);
        pending_clear(p); --r->pending_count;
    }
    return n;
}
