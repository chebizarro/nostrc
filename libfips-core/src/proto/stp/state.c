#include <fips/proto/stp/state.h>
#include "internal.h"
#include <math.h>
static size_t peer_index(const fips_tree_state *s, fips_node_addr id) {
    size_t lo = 0, hi = s->peer_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (addr_cmp(s->peers[mid].id, id) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}
/* Shared with the STP-local delivery adapter. */
fips_stp_peer *fips_stp_ensure_peer(fips_tree_state *s, fips_node_addr id);
fips_stp_peer *fips_stp_ensure_peer(fips_tree_state *s, fips_node_addr id) {
    size_t i = peer_index(s, id);
    if (i < s->peer_count && !addr_cmp(s->peers[i].id, id))
        return &s->peers[i];
    if (s->peer_count == s->peer_capacity)
        return NULL;
    memmove(s->peers + i + 1, s->peers + i, (s->peer_count - i) * sizeof(*s->peers));
    memset(&s->peers[i], 0, sizeof(*s->peers));
    s->peers[i].id = id;
    s->peer_count++;
    return &s->peers[i];
}
void fips_tree_state_new(fips_tree_state *s, fips_node_addr me, uint64_t ts, fips_instant origin,
                         fips_stp_peer *p, size_t cap) {
    s->my_node_addr = me;
    s->root = me;
    s->my_declaration = fips_parent_declaration_self_root(me, 1, ts);
    fips_tree_coordinate_root_with_meta(&s->my_coords, me, 1, ts);
    s->peers = p;
    s->peer_count = 0;
    s->peer_capacity = cap;
    s->parent_hysteresis = 0;
    s->fallback_ms = FIPS_STP_FALLBACK_MS;
    fips_flap_dampener_new(&s->flap, origin);
}
bool fips_tree_state_is_root(const fips_tree_state *s) {
    return !addr_cmp(s->root, s->my_node_addr);
}
const fips_stp_peer *fips_tree_state_peer(const fips_tree_state *s, fips_node_addr id) {
    size_t i = peer_index(s, id);
    return i < s->peer_count && !addr_cmp(s->peers[i].id, id) ? &s->peers[i] : NULL;
}
fips_status fips_tree_state_update_peer(fips_tree_state *s, const fips_parent_declaration *d,
                                        const fips_tree_coordinate *c, bool *updated) {
    const fips_stp_peer *old = fips_tree_state_peer(s, d->node_addr);
    if (old && old->has_declaration &&
        !fips_parent_declaration_is_fresher_than(d, &old->declaration)) {
        *updated = false;
        return FIPS_OK;
    }
    fips_stp_peer *p = fips_stp_ensure_peer(s, d->node_addr);
    if (!p)
        return FIPS_ERR_RANGE;
    p->declaration = *d;
    memcpy(p->ancestry.entries, c->entries, (size_t)c->len * sizeof(c->entries[0]));
    p->ancestry.len = c->len;
    p->has_declaration = true;
    *updated = true;
    return FIPS_OK;
}
void fips_tree_state_remove_peer(fips_tree_state *s, fips_node_addr id) {
    size_t i = peer_index(s, id);
    if (i == s->peer_count || addr_cmp(s->peers[i].id, id))
        return;
    memmove(s->peers + i, s->peers + i + 1, (s->peer_count - i - 1) * sizeof(*s->peers));
    s->peer_count--;
}
bool fips_tree_state_set_parent(fips_tree_state *s, fips_node_addr parent, uint64_t seq,
                                uint64_t ts, fips_instant now) {
    bool changed = fips_tree_state_is_root(s) || addr_cmp(s->my_declaration.parent_id, parent);
    s->my_declaration = fips_parent_declaration_new(s->my_node_addr, parent, seq, ts);
    if (!changed)
        return false;
    fips_flap_dampener_mark_switch(&s->flap, now);
    return fips_flap_dampener_record_parent_switch(&s->flap, now);
}
fips_status fips_tree_state_recompute_coords(fips_tree_state *s) {
    if (fips_parent_declaration_is_root(&s->my_declaration)) {
        fips_tree_coordinate_root_with_meta(&s->my_coords, s->my_node_addr,
                                            s->my_declaration.sequence,
                                            s->my_declaration.timestamp);
        s->root = s->my_node_addr;
        return FIPS_OK;
    }
    const fips_stp_peer *p = fips_tree_state_peer(s, s->my_declaration.parent_id);
    if (!p || !p->has_declaration)
        return FIPS_OK;
    fips_node_addr root = *fips_tree_coordinate_root_id(&p->ancestry);
    if (addr_cmp(s->my_node_addr, root) <= 0) {
        s->my_declaration = fips_parent_declaration_self_root(
            s->my_node_addr, s->my_declaration.sequence, s->my_declaration.timestamp);
        return fips_tree_state_recompute_coords(s);
    }
    if (p->ancestry.len == FIPS_COORD_MAX_ENTRIES)
        return FIPS_ERR_RANGE;
    s->my_coords.entries[0] = fips_coord_entry_new(s->my_node_addr, s->my_declaration.sequence,
                                                   s->my_declaration.timestamp);
    memcpy(s->my_coords.entries + 1, p->ancestry.entries,
           (size_t)p->ancestry.len * sizeof(p->ancestry.entries[0]));
    s->my_coords.len = (uint16_t)(p->ancestry.len + 1);
    s->root = root;
    return FIPS_OK;
}
bool fips_tree_state_smallest_visible_root(const fips_tree_state *s, fips_node_addr *out) {
    bool found = false;
    fips_node_addr root = {{0}};
    for (size_t i = 0; i < s->peer_count; i++)
        if (s->peers[i].has_declaration) {
            fips_node_addr r = *fips_tree_coordinate_root_id(&s->peers[i].ancestry);
            if (!found || addr_cmp(r, root) < 0) {
                root = r;
                found = true;
            }
        }
    if (found)
        *out = root;
    return found;
}
bool fips_tree_state_should_be_root(const fips_tree_state *s) {
    fips_node_addr root;
    return !fips_tree_state_smallest_visible_root(s, &root) || addr_cmp(s->my_node_addr, root) <= 0;
}
fips_status fips_tree_state_become_root(fips_tree_state *s, uint64_t ts) {
    if (s->my_declaration.sequence == UINT64_MAX)
        return FIPS_ERR_RANGE;
    s->my_declaration =
        fips_parent_declaration_self_root(s->my_node_addr, s->my_declaration.sequence + 1, ts);
    return fips_tree_state_recompute_coords(s);
}
bool fips_tree_state_distance_to_peer(const fips_tree_state *s, fips_node_addr peer, size_t *out) {
    const fips_stp_peer *p = fips_tree_state_peer(s, peer);
    if (!p || !p->has_declaration)
        return false;
    *out = fips_tree_coordinate_distance_to(&s->my_coords, &p->ancestry);
    return true;
}
static bool skipped(fips_node_addr a, const fips_node_addr *skip, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!addr_cmp(a, skip[i]))
            return true;
    return false;
}
bool fips_tree_state_find_next_hop(const fips_tree_state *s, const fips_tree_coordinate *dest,
                                   const fips_node_addr *skip, size_t n, fips_node_addr *out) {
    if (addr_cmp(*fips_tree_coordinate_root_id(&s->my_coords), *fips_tree_coordinate_root_id(dest)))
        return false;
    size_t best = SIZE_MAX;
    const fips_stp_peer *chosen = NULL;
    for (size_t i = 0; i < s->peer_count; i++) {
        const fips_stp_peer *p = &s->peers[i];
        if (!p->has_declaration || skipped(p->id, skip, n))
            continue;
        size_t d = fips_tree_coordinate_distance_to(&p->ancestry, dest);
        if (!chosen || d < best) {
            chosen = p;
            best = d;
        }
    }
    if (!chosen || best >= fips_tree_coordinate_distance_to(&s->my_coords, dest))
        return false;
    *out = chosen->id;
    return true;
}
void fips_tree_state_set_parent_hysteresis(fips_tree_state *s, double h) {
    s->parent_hysteresis = h < 0 ? 0 : h > 1 ? 1 : h;
}
bool fips_tree_state_is_switch_suppressed(const fips_tree_state *s, fips_instant now) {
    return fips_flap_dampener_is_hold_down_active(&s->flap, now) ||
           fips_flap_dampener_is_flap_dampened(&s->flap, now);
}
static bool cost_for(const fips_stp_ranking *r, fips_node_addr id, double *cost) {
    for (size_t i = 0; i < r->cost_count; i++)
        if (!addr_cmp(r->peer_costs[i].peer, id)) {
            *cost = r->peer_costs[i].cost;
            return true;
        }
    return false;
}
static const fips_stp_peer *best_candidate(const fips_tree_state *s, const fips_stp_ranking *r,
                                           fips_node_addr root, bool measured, double *eff) {
    const fips_stp_peer *best = NULL;
    for (size_t i = 0; i < s->peer_count; i++) {
        const fips_stp_peer *p = &s->peers[i];
        if (!p->has_declaration || addr_cmp(*fips_tree_coordinate_root_id(&p->ancestry), root) ||
            skipped(p->id, r->skip, r->skip_count) ||
            fips_tree_coordinate_contains(&p->ancestry, &s->my_node_addr))
            continue;
        double cost = 1;
        if (!cost_for(r, p->id, &cost) && measured)
            continue;
        double e = (double)fips_tree_coordinate_depth(&p->ancestry) + cost;
        if (!best || e < *eff) {
            best = p;
            *eff = e;
        }
    }
    return best;
}
fips_parent_eval fips_tree_state_evaluate_parent(const fips_tree_state *s,
                                                 const fips_stp_ranking *r) {
    fips_parent_eval none = {.kind = FIPS_PARENT_None};
    fips_node_addr root;
    if (!fips_tree_state_smallest_visible_root(s, &root) || addr_cmp(s->my_node_addr, root) <= 0)
        return none;
    double best_eff = 0;
    const fips_stp_peer *best = best_candidate(s, r, root, r->cost_count != 0, &best_eff);
    bool fallback = !best && r->cost_count != 0;
    if (fallback)
        best = best_candidate(s, r, root, false, &best_eff);
    if (!best)
        return none;
    bool is_root = fips_tree_state_is_root(s);
    if (!is_root && !addr_cmp(s->my_declaration.parent_id, best->id))
        return none;
    const fips_stp_peer *parent = fips_tree_state_peer(s, s->my_declaration.parent_id);
    fips_parent_eval mandatory = {FIPS_PARENT_Mandatory, best->id},
                     discretionary = {FIPS_PARENT_Discretionary, best->id};
    if (!is_root && (!parent || !parent->has_declaration ||
                     fips_tree_coordinate_contains(&parent->ancestry, &s->my_node_addr)))
        return mandatory;
    bool via_parent = fips_tree_coordinate_contains(&best->ancestry, &s->my_declaration.parent_id);
    if (addr_cmp(root, s->root) < 0 ||
        (!is_root && !via_parent && parent && parent->has_declaration &&
         addr_cmp(root, *fips_tree_coordinate_root_id(&parent->ancestry)) < 0) ||
        is_root)
        return mandatory;
    if (fallback)
        return none;
    double cost = r->cost_count ? INFINITY : 1;
    cost_for(r, s->my_declaration.parent_id, &cost);
    if (!parent || !parent->has_declaration)
        return discretionary;
    double current = (double)fips_tree_coordinate_depth(&parent->ancestry) + cost;
    return best_eff < current * (1 - s->parent_hysteresis) ? discretionary : none;
}
fips_status fips_tree_state_recover(fips_tree_state *s, const fips_stp_ranking *r, uint64_t ts,
                                    fips_instant now, fips_parent_loss *out) {
    if (s->my_declaration.sequence == UINT64_MAX)
        return FIPS_ERR_RANGE;
    /* Upstream recovery ignores skip_peers, but keeps measured link costs. */
    fips_stp_ranking recovery = *r;
    recovery.skip = NULL;
    recovery.skip_count = 0;
    fips_parent_eval e = fips_tree_state_evaluate_parent(s, &recovery);
    bool use = e.kind == FIPS_PARENT_Mandatory || (e.kind == FIPS_PARENT_Discretionary &&
                                                   !fips_tree_state_is_switch_suppressed(s, now));
    bool dampened = false;
    if (use) {
        const fips_stp_peer *p = fips_tree_state_peer(s, e.peer);
        if (p->ancestry.len == FIPS_COORD_MAX_ENTRIES)
            return FIPS_ERR_RANGE;
        dampened = fips_tree_state_set_parent(s, e.peer, s->my_declaration.sequence + 1, ts, now);
        fips_tree_state_recompute_coords(s);
    } else
        fips_tree_state_become_root(s, ts);
    *out = (fips_parent_loss){true, dampened};
    return FIPS_OK;
}
fips_status fips_tree_state_handle_parent_lost(fips_tree_state *s, const fips_stp_ranking *r,
                                               uint64_t ts, fips_instant now, bool *changed) {
    fips_parent_loss loss;
    if (fips_tree_state_recover(s, r, ts, now, &loss) != FIPS_OK)
        return FIPS_ERR_RANGE;
    *changed = loss.changed;
    return FIPS_OK;
}

const fips_node_addr *fips_tree_state_my_node_addr(const fips_tree_state *s) {
    return &s->my_node_addr;
}
const fips_parent_declaration *fips_tree_state_my_declaration(const fips_tree_state *s) {
    return &s->my_declaration;
}
fips_parent_declaration *fips_tree_state_my_declaration_mut(fips_tree_state *s) {
    return &s->my_declaration;
}
const fips_tree_coordinate *fips_tree_state_my_coords(const fips_tree_state *s) {
    return &s->my_coords;
}
const fips_node_addr *fips_tree_state_root(const fips_tree_state *s) { return &s->root; }
const fips_tree_coordinate *fips_tree_state_peer_coords(const fips_tree_state *s,
                                                        fips_node_addr id) {
    const fips_stp_peer *p = fips_tree_state_peer(s, id);
    return p && p->has_declaration ? &p->ancestry : NULL;
}
const fips_parent_declaration *fips_tree_state_peer_declaration(const fips_tree_state *s,
                                                                fips_node_addr id) {
    const fips_stp_peer *p = fips_tree_state_peer(s, id);
    return p && p->has_declaration ? &p->declaration : NULL;
}
size_t fips_tree_state_peer_count(const fips_tree_state *s) {
    size_t n = 0;
    for (size_t i = 0; i < s->peer_count; i++)
        n += s->peers[i].has_declaration;
    return n;
}
bool fips_tree_state_peer_ids(const fips_tree_state *s, size_t index, fips_node_addr *out) {
    for (size_t i = 0; i < s->peer_count; i++)
        if (s->peers[i].has_declaration) {
            if (index == 0) {
                *out = s->peers[i].id;
                return true;
            }
            index--;
        }
    return false;
}
bool fips_tree_state_is_declaration_signed(const fips_tree_state *s) {
    return s->my_declaration.is_signed;
}
void fips_tree_state_set_hold_down(fips_tree_state *s, uint64_t secs) {
    fips_flap_dampener_set_hold_down(&s->flap, secs);
}
void fips_tree_state_set_flap_dampening(fips_tree_state *s, uint32_t threshold, uint64_t window,
                                        uint64_t duration) {
    fips_flap_dampener_set_flap_dampening(&s->flap, threshold, window, duration);
}
uint64_t fips_tree_state_dampening_secs(const fips_tree_state *s) {
    return fips_flap_dampener_dampening_secs(&s->flap);
}
bool fips_tree_state_is_flap_dampened(const fips_tree_state *s, fips_instant now) {
    return fips_flap_dampener_is_flap_dampened(&s->flap, now);
}
bool fips_tree_state_announce_outstanding(const fips_tree_state *s, fips_node_addr peer) {
    uint64_t ignored;
    return fips_tree_state_outstanding_counter(s, peer, &ignored);
}
