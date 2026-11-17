/* Upstream transport/ble/pool.rs (ConnectionPool), mod.rs arbitrate /
 * newcomer_wins / PendingProbes, and recall.rs. Pure state: the caller holds
 * the transport mutex and supplies time, so every rule is unit-tested here
 * without a radio. */
#include "transport/ble/internal.h"
#include <stdlib.h>
#include <string.h>
static uint64_t elapsed(uint64_t now, uint64_t since) { return now > since ? now - since : 0; } /* saturating */
static bool same_node(const ble_link *l, const fips_node_addr *n)
{ return l->has_node && !memcmp(l->node.bytes, n->bytes, 16); }
uint16_t ble_link_mtu(const ble_link *l) { return l->send_mtu < l->recv_mtu ? l->send_mtu : l->recv_mtu; }
int ble_pool_init(ble_pool *p, size_t max, uint64_t grace)
{
    *p = (ble_pool){.max = max, .verify_grace_ms = grace};
    p->items = calloc(max ? max : 1, sizeof *p->items);
    return p->items ? 0 : -1;
}
void ble_pool_free(ble_pool *p) { free(p->items); p->items = NULL; p->count = 0; }
static size_t index_of(const ble_pool *p, const fips_transport_addr *a)
{
    for (size_t i = 0; i < p->count; ++i) if (fips_transport_addr_equal(&p->items[i]->addr, a)) return i;
    return SIZE_MAX;
}
ble_link *ble_pool_get(const ble_pool *p, const fips_transport_addr *a)
{ size_t i = index_of(p, a); return i == SIZE_MAX ? NULL : p->items[i]; }
static void take(ble_pool *p, size_t i) { p->items[i] = p->items[--p->count]; p->items[p->count] = NULL; }
bool ble_pool_remove(ble_pool *p, const ble_link *l)
{
    for (size_t i = 0; i < p->count; ++i) if (p->items[i] == l) { take(p, i); return true; }
    return false;
}
ble_link *ble_pool_remove_own(ble_pool *p, const fips_transport_addr *a, uint64_t id)
{
    size_t i = index_of(p, a);
    if (i == SIZE_MAX || p->items[i]->id != id) return NULL;
    ble_link *l = p->items[i]; take(p, i); return l;
}
ble_link *ble_pool_find_verified(const ble_pool *p, const fips_node_addr *n)
{
    for (size_t i = 0; i < p->count; ++i) if (p->items[i]->verified && same_node(p->items[i], n)) return p->items[i];
    return NULL;
}
bool ble_pool_claims(const ble_pool *p, const fips_node_addr *n)
{
    for (size_t i = 0; i < p->count; ++i) if (same_node(p->items[i], n)) return true;
    return false;
}
/* A verified link is never evicted; an unverified one only once it had its
 * grace. A node's duplicate links go first, then the oldest. */
ble_link *ble_pool_victim(const ble_pool *p, uint64_t now)
{
    for (int dup_only = 1; dup_only >= 0; --dup_only) {
        ble_link *best = NULL;
        for (size_t i = 0; i < p->count; ++i) {
            ble_link *c = p->items[i];
            if (c->verified || elapsed(now, c->grace_from_ms) < p->verify_grace_ms) continue;
            if (dup_only) {
                size_t same = 0;
                if (c->has_node) for (size_t j = 0; j < p->count; ++j) if (same_node(p->items[j], &c->node)) ++same;
                if (same < 2) continue;
            }
            if (!best || c->established_ms < best->established_ms) best = c;
        }
        if (best) return best;
    }
    return NULL;
}
static ble_insert refusal(ble_pool *p, uint64_t now)
{
    ble_insert r = {.kind = BLE_INSERT_REFUSED};
    r.first = !p->has_refusal || elapsed(now, p->last_refusal_ms) >= BLE_BOUND_LOG_HOLD_MS;
    if (r.first && p->refusals) { r.has_ended = true; r.ended = p->refusals; p->refusals = 0; }
    ++p->refusals; p->last_refusal_ms = now; p->has_refusal = true;
    return r;
}
static void admission(ble_pool *p, uint64_t now, ble_insert *r)
{
    if (!p->has_refusal || elapsed(now, p->last_refusal_ms) < BLE_BOUND_LOG_HOLD_MS) return;
    p->has_refusal = false; r->has_ended = true; r->ended = p->refusals; p->refusals = 0;
}
ble_insert ble_pool_insert(ble_pool *p, ble_link *l, uint64_t now)
{
    ble_insert r = {.kind = BLE_INSERT_ADMITTED};
    if (ble_pool_get(p, &l->addr)) return (ble_insert){.kind = BLE_INSERT_OCCUPIED};
    if (p->count >= p->max) {
        r.evicted = ble_pool_victim(p, now);
        if (!r.evicted) return refusal(p, now);
        (void)ble_pool_remove(p, r.evicted);
    }
    p->items[p->count++] = l;
    admission(p, now, &r);
    return r;
}
ble_verify ble_pool_mark_verified(ble_pool *p, const fips_transport_addr *a, const fips_node_addr *n, size_t *demoted)
{
    if (demoted) *demoted = 0;
    ble_link *l = ble_pool_get(p, a);
    if (!l) return BLE_VERIFY_ABSENT;
    if (l->has_node && !same_node(l, n)) return BLE_VERIFY_MISMATCH;
    l->node = *n; l->has_node = l->verified = l->proven = true;
    for (size_t i = 0; i < p->count; ++i) {
        ble_link *o = p->items[i];
        if (o != l && o->verified && same_node(o, n)) { o->verified = false; if (demoted) ++*demoted; }
    }
    return BLE_VERIFY_MARKED;
}
bool ble_pool_clear_verified(ble_pool *p, const fips_transport_addr *a, const fips_node_addr *n, bool regrace, uint64_t now)
{
    ble_link *l = ble_pool_get(p, a);
    if (!l || !l->verified || !same_node(l, n)) return false;
    l->verified = false;
    if (regrace) l->grace_from_ms = now;
    return true;
}
void ble_pool_hold(ble_pool *p, const fips_transport_addr *a, uint64_t now)
{ ble_link *l = ble_pool_get(p, a); if (l) l->held_from_ms = now; }
size_t ble_pool_unproven(const ble_pool *p, uint64_t now, uint64_t timeout, ble_link **out, size_t cap)
{
    size_t n = 0;
    for (size_t i = 0; i < p->count && n < cap; ++i) {
        ble_link *c = p->items[i];
        if (!c->verified && !c->proven && elapsed(now, c->held_from_ms) >= timeout) out[n++] = c;
    }
    return n;
}

/* The link the smaller node dialled is outbound on the smaller side and
 * inbound on the larger, so each side keeps its outbound link exactly when it
 * is the smaller. Two links in the same direction are no race. */
bool ble_newcomer_wins(bool local_is_smaller, bool newcomer_outbound, bool incumbent_outbound)
{ bool keep = local_is_smaller; return newcomer_outbound == keep && incumbent_outbound != keep; }
/* Deliberate deviation from upstream, which declines every newcomer beside a
 * verified link: inside the tie window a verified link racing a newcomer at
 * its own address, in the opposite direction (the two ends' crossing dials),
 * is settled by the tie-break like an unverified one. Verification is local
 * knowledge: with discovery pushed to the node at once (upstream polls it on
 * a tick), an end can verify the race's loser before the winner arrives while
 * the other end has already replaced it, and the two ends then keep different
 * channels and lose both. Only the same address qualifies: the newcomer's key
 * claim is unauthenticated until Noise, and a claim from any other address
 * must not displace a link that proved the key (upstream's rule; a crossing
 * seen at two addresses falls back to upstream's redial). Outside the window
 * a verified link declines as upstream. */
ble_admission ble_arbitrate(const ble_pool *p, const fips_node_addr *local, const fips_node_addr *peer,
    const fips_transport_addr *ta, bool outbound, uint64_t tie_window, uint64_t now)
{
    bool smaller = memcmp(local->bytes, peer->bytes, 16) < 0;
    ble_link *v = ble_pool_find_verified(p, peer);
    if (v) {
        bool racing = elapsed(now, v->established_ms) < tie_window && fips_transport_addr_equal(&v->addr, ta);
        return (ble_admission){racing && ble_newcomer_wins(smaller, outbound, v->outbound) ? BLE_REPLACE : BLE_DECLINE, v};
    }
    ble_link *at = ble_pool_get(p, ta);
    if (at && !same_node(at, peer)) return (ble_admission){BLE_OCCUPIED, at};
    if (at) return (ble_admission){elapsed(now, at->established_ms) < tie_window &&
        ble_newcomer_wins(smaller, outbound, at->outbound) ? BLE_REPLACE : BLE_DECLINE, at};
    ble_link *rival = NULL;
    for (size_t i = 0; i < p->count; ++i) {
        ble_link *c = p->items[i];
        if (c->verified || !same_node(c, peer) || elapsed(now, c->established_ms) >= tie_window || c->outbound == outbound) continue;
        if (!rival || c->established_ms < rival->established_ms) rival = c;
    }
    if (!rival) return (ble_admission){BLE_ADMIT, NULL};
    return (ble_admission){ble_newcomer_wins(smaller, outbound, rival->outbound) ? BLE_REPLACE : BLE_DECLINE, rival};
}

/* PendingProbes: the retry book. An entry leaves on a conclusive outcome or
 * when other addresses compete for its slot, never because it failed. */
void ble_probes_init(ble_probes *b, uint64_t cooldown) { memset(b, 0, sizeof *b); b->cooldown_ms = cooldown; }
static size_t probe_at(const ble_probes *b, const fipsd_ble_peer *a)
{
    for (size_t i = 0; i < b->count; ++i) if (fipsd_ble_peer_equal(&b->entries[i].peer, a)) return i;
    return SIZE_MAX;
}
static void probe_take(ble_probes *b, size_t i)
{ memmove(&b->entries[i], &b->entries[i + 1], (b->count - i - 1) * sizeof b->entries[0]); --b->count; }
bool ble_probes_contains(const ble_probes *b, const fipsd_ble_peer *a) { return probe_at(b, a) != SIZE_MAX; }
void ble_probes_observe(ble_probes *b, const fipsd_ble_peer *a, uint64_t now)
{
    if (probe_at(b, a) != SIZE_MAX) return;
    if (b->count >= BLE_MAX_PENDING_PROBES) {
        size_t worst = 0; /* the most-failed, latest-due; the last of equals, as max_by_key */
        for (size_t i = 1; i < b->count; ++i) {
            const ble_probe *e = &b->entries[i], *w = &b->entries[worst];
            if (e->failures > w->failures || (e->failures == w->failures && e->next_attempt_ms >= w->next_attempt_ms)) worst = i;
        }
        probe_take(b, worst);
    }
    b->entries[b->count++] = (ble_probe){.peer = *a, .next_attempt_ms = now};
}
bool ble_probes_is_due(const ble_probes *b, const fipsd_ble_peer *a, uint64_t now)
{ size_t i = probe_at(b, a); return i == SIZE_MAX || b->entries[i].next_attempt_ms <= now; }
void ble_probes_mark_attempt(ble_probes *b, const fipsd_ble_peer *a, uint64_t now)
{ size_t i = probe_at(b, a); if (i != SIZE_MAX) b->entries[i].next_attempt_ms = now + b->cooldown_ms; }
uint32_t ble_probes_record_failure(ble_probes *b, const fipsd_ble_peer *a, uint64_t now)
{
    size_t i = probe_at(b, a);
    if (i == SIZE_MAX) return 0;
    ble_probe *e = &b->entries[i];
    if (e->failures < UINT32_MAX) ++e->failures;
    uint32_t shift = e->failures - 1 < BLE_MAX_PROBE_BACKOFF_SHIFT ? e->failures - 1 : BLE_MAX_PROBE_BACKOFF_SHIFT;
    e->next_attempt_ms = now + (b->cooldown_ms << shift);
    return e->failures;
}
void ble_probes_resolve(ble_probes *b, const fipsd_ble_peer *a)
{ size_t i = probe_at(b, a); if (i != SIZE_MAX) probe_take(b, i); }
void ble_probes_drop(ble_probes *b, ble_peer_pred pred, void *ctx)
{
    for (size_t i = 0; i < b->count;) if (pred(ctx, &b->entries[i].peer)) probe_take(b, i); else ++i;
}
bool ble_probes_next_due(ble_probes *b, uint64_t now, fipsd_ble_peer *out)
{
    for (size_t i = 0; i < b->count; ++i) if (b->entries[i].next_attempt_ms <= now) {
        ble_probe e = b->entries[i]; probe_take(b, i);
        b->entries[b->count++] = e; *out = e.peer; return true;
    }
    return false;
}

/* recall.rs: bounded memory fed by untrusted adverts. Live addresses (pooled
 * or in the retry book) are never evicted or expired and do not count. */
int ble_recall_init(ble_recall *r, size_t capacity)
{
    *r = (ble_recall){.capacity = capacity};
    r->entries = calloc(capacity ? capacity : 1, sizeof *r->entries);
    return r->entries ? 0 : -1;
}
void ble_recall_free(ble_recall *r) { free(r->entries); r->entries = NULL; r->capacity = 0; }
static ble_recall_entry *recall_find(const ble_recall *r, const fipsd_ble_peer *a)
{
    for (size_t i = 0; i < r->capacity; ++i)
        if (r->entries[i].used && fipsd_ble_peer_equal(&r->entries[i].peer, a)) return &r->entries[i];
    return NULL;
}
const ble_recall_entry *ble_recall_get(const ble_recall *r, const fipsd_ble_peer *a) { return recall_find(r, a); }
static ble_recall_entry *oldest(ble_recall *r, ble_peer_pred live, void *ctx)
{
    ble_recall_entry *o = NULL;
    for (size_t i = 0; i < r->capacity; ++i) {
        ble_recall_entry *e = &r->entries[i];
        if (e->used && (!live || !live(ctx, &e->peer)) && (!o || e->seq < o->seq)) o = e;
    }
    return o;
}
size_t ble_recall_note(ble_recall *r, const fipsd_ble_peer *a, uint16_t psm, const fips_node_addr *node,
    uint64_t now, ble_peer_pred live, void *ctx)
{
    ble_recall_entry *e = recall_find(r, a);
    if (!e) for (size_t i = 0; i < r->capacity && !e; ++i) if (!r->entries[i].used) e = &r->entries[i];
    if (!e) { e = oldest(r, live, ctx); if (!e) e = oldest(r, NULL, NULL); }
    if (!e) return 0;
    *e = (ble_recall_entry){.used = true, .peer = *a, .seen_ms = now, .seq = r->next_seq++, .psm = psm};
    if (node) e->node = *node;
    size_t non_live = 0, evicted = 0;
    for (size_t i = 0; i < r->capacity; ++i)
        if (r->entries[i].used && !(live && live(ctx, &r->entries[i].peer))) ++non_live;
    while (non_live > BLE_MAX_RECALLED) {
        ble_recall_entry *gone = oldest(r, live, ctx);
        if (!gone) break;
        gone->used = false; --non_live; ++evicted;
    }
    return evicted;
}
bool ble_recall_remove(ble_recall *r, const fipsd_ble_peer *a)
{ ble_recall_entry *e = recall_find(r, a); if (e) e->used = false; return e != NULL; }
void ble_recall_prune(ble_recall *r, uint64_t now, ble_peer_pred live, void *ctx)
{
    for (size_t i = 0; i < r->capacity; ++i) {
        ble_recall_entry *e = &r->entries[i];
        if (!e->used) continue;
        if (live && live(ctx, &e->peer)) e->seen_ms = now;
        else if (elapsed(now, e->seen_ms) >= BLE_RECALL_MAX_AGE_MS) e->used = false;
    }
}
size_t ble_recall_len(const ble_recall *r)
{ size_t n = 0; for (size_t i = 0; i < r->capacity; ++i) n += r->entries[i].used; return n; }
