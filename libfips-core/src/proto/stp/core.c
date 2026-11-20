#include <fips/proto/stp/core.h>
#include "internal.h"
static fips_tree_decision classify(const fips_tree_state *s, const fips_stp_ranking *r,
                                   bool suppressed) {
    fips_parent_eval e = fips_tree_state_evaluate_parent(s, r);
    if (e.kind == FIPS_PARENT_Mandatory || (e.kind == FIPS_PARENT_Discretionary && !suppressed))
        return (fips_tree_decision){FIPS_TREE_Switch, e.peer, s->my_declaration.sequence + 1};
    return (fips_tree_decision){.kind =
                                    !fips_tree_state_is_root(s) && fips_tree_state_should_be_root(s)
                                        ? FIPS_TREE_SelfRoot
                                        : FIPS_TREE_NoChange};
}
fips_tree_decision fips_stp_classify_announce(const fips_tree_state *s, fips_node_addr from,
                                              const fips_stp_ranking *r, bool suppressed) {
    fips_tree_decision d = classify(s, r, suppressed);
    if (d.kind != FIPS_TREE_NoChange)
        return d;
    if (!fips_tree_state_is_root(s) && !addr_cmp(s->my_declaration.parent_id, from)) {
        const fips_stp_peer *p = fips_tree_state_peer(s, from);
        if (p && p->has_declaration &&
            fips_tree_coordinate_contains(&p->ancestry, &s->my_node_addr))
            d.kind = FIPS_TREE_LoopDrop;
        else {
            d.kind = FIPS_TREE_AncestryUpdate;
            d.parent = from;
            d.new_seq = s->my_declaration.sequence + 1;
        }
    }
    return d;
}
fips_tree_decision fips_stp_classify_periodic(const fips_tree_state *s, const fips_stp_ranking *r,
                                              bool suppressed) {
    fips_tree_decision d = classify(s, r, suppressed);
    if (d.kind == FIPS_TREE_NoChange)
        d.kind = FIPS_TREE_PeriodicRebroadcast;
    return d;
}
bool fips_stp_should_echo(fips_node_addr a, fips_node_addr b) { return addr_cmp(a, b) > 0; }
static void add(fips_stp_actions *a, fips_stp_action action) { a->items[a->len++] = action; }
static fips_status preflight(const fips_tree_state *s) {
    for (size_t i = 0; i < s->peer_count; i++)
        if (s->peers[i].has_declaration && s->peers[i].ancestry.len == FIPS_COORD_MAX_ENTRIES)
            return FIPS_ERR_RANGE;
    return FIPS_OK;
}
static bool path_will_change(const fips_tree_state *s, fips_tree_decision d) {
    const fips_stp_peer *p = fips_tree_state_peer(s, d.parent);
    if (!p || !p->has_declaration)
        return false;
    if (addr_cmp(s->my_node_addr, *fips_tree_coordinate_root_id(&p->ancestry)) <= 0)
        return s->my_coords.len != 1 ||
               addr_cmp(s->my_coords.entries[0].node_addr, s->my_node_addr);
    if (s->my_coords.len != (size_t)p->ancestry.len + 1)
        return true;
    for (size_t i = 0; i < p->ancestry.len; i++)
        if (addr_cmp(s->my_coords.entries[i + 1].node_addr, p->ancestry.entries[i].node_addr))
            return true;
    return false;
}
static void drive(fips_tree_state *s, fips_tree_decision d, const fips_stp_ranking *r, uint64_t ts,
                  fips_instant now, fips_stp_actions *a) {
    bool dampened = false, changed = true;
    if (d.kind == FIPS_TREE_NoChange)
        return;
    if (d.kind == FIPS_TREE_PeriodicRebroadcast) {
        add(a, (fips_stp_action){.kind = FIPS_STP_SendAnnounce, .all_peers = true});
        return;
    }
    if (d.kind == FIPS_TREE_AncestryUpdate)
        changed = path_will_change(s, d);
    if (d.kind == FIPS_TREE_Switch || d.kind == FIPS_TREE_AncestryUpdate) {
        dampened = fips_tree_state_set_parent(s, d.parent, d.new_seq, ts, now);
        fips_tree_state_recompute_coords(s);
    } else if (d.kind == FIPS_TREE_SelfRoot)
        fips_tree_state_become_root(s, ts);
    else if (d.kind == FIPS_TREE_LoopDrop) {
        fips_parent_loss loss;
        fips_tree_state_recover(s, r, ts, now, &loss);
        dampened = loss.dampened;
        add(a, (fips_stp_action){.kind = FIPS_STP_Drop, .peer = d.parent, .reason = FIPS_STP_Loop});
    }
    add(a, (fips_stp_action){.kind = FIPS_STP_SignDeclaration});
    if (!changed)
        return;
    bool root = fips_tree_state_is_root(s);
    add(a, (fips_stp_action){.kind = root                                 ? FIPS_STP_BecameRoot
                                     : d.kind == FIPS_TREE_AncestryUpdate ? FIPS_STP_AncestryChanged
                                                                          : FIPS_STP_ChangeParent,
                             .peer = s->my_declaration.parent_id,
                             .invalidate_via_node = d.kind != FIPS_TREE_SelfRoot,
                             .invalidate_other_roots = d.kind == FIPS_TREE_SelfRoot ||
                                                       d.kind == FIPS_TREE_LoopDrop ||
                                                       d.kind == FIPS_TREE_ParentLost,
                             .reset_backoff = true,
                             .dampened = dampened});
    add(a, (fips_stp_action){.kind = FIPS_STP_SendAnnounce, .all_peers = true});
    add(a, (fips_stp_action){.kind = FIPS_STP_RefreshFilters,
                             .all_peers = true,
                             .changed_only = d.kind == FIPS_TREE_AncestryUpdate});
}
fips_status fips_stp_receive_announce(fips_tree_state *s, fips_node_addr from,
                                      const fips_tree_announce *a, bool known, bool verified,
                                      const fips_stp_ranking *r, uint64_t ts, fips_instant now,
                                      fips_stp_actions *out) {
    fips_stp_actions actions = {0};
    fips_stp_action drop = {.kind = FIPS_STP_Drop, .peer = from};
    if (!known)
        drop.reason = FIPS_STP_UnknownPeer;
    else if (addr_cmp(a->declaration.node_addr, from))
        drop.reason = FIPS_STP_SenderMismatch;
    else if (!verified || !a->declaration.is_signed)
        drop.reason = FIPS_STP_BadSignature;
    else if (fips_tree_announce_validate_semantics(a, &drop.error) != FIPS_OK)
        drop.reason = FIPS_STP_BadAncestry;
    else {
        const fips_stp_peer *p = fips_tree_state_peer(s, from);
        if (p && p->has_declaration &&
            !fips_parent_declaration_is_fresher_than(&a->declaration, &p->declaration))
            drop.reason = FIPS_STP_Stale;
        else {
            if (s->my_declaration.sequence == UINT64_MAX ||
                a->ancestry.len == FIPS_COORD_MAX_ENTRIES || preflight(s) != FIPS_OK ||
                (!p && s->peer_count == s->peer_capacity))
                return FIPS_ERR_RANGE;
            bool was_tree =
                !fips_tree_state_is_root(s) && !addr_cmp(s->my_declaration.parent_id, from);
            was_tree = was_tree || (p && p->has_declaration &&
                                    !addr_cmp(p->declaration.parent_id, s->my_node_addr));
            bool updated;
            fips_tree_state_update_peer(s, &a->declaration, &a->ancestry, &updated);
            bool is_tree =
                (!fips_tree_state_is_root(s) && !addr_cmp(s->my_declaration.parent_id, from)) ||
                !addr_cmp(a->declaration.parent_id, s->my_node_addr);
            if (was_tree != is_tree)
                add(&actions, (fips_stp_action){.kind = FIPS_STP_RefreshFilters,
                                                .all_peers = true,
                                                .changed_only = true});
            bool echo = fips_stp_should_echo(*fips_tree_coordinate_root_id(&a->ancestry), s->root);
            fips_tree_decision d = fips_stp_classify_announce(
                s, from, r, fips_tree_state_is_switch_suppressed(s, now));
            if (d.kind == FIPS_TREE_LoopDrop)
                d.parent = from;
            drive(s, d, r, ts, now, &actions);
            /* All actions read the post-transition state. Never queue an echo
             * before its SignDeclaration; a fan-out already includes this peer. */
            bool broadcast = false;
            for (size_t i = 0; i < actions.len; i++)
                if (actions.items[i].kind == FIPS_STP_SendAnnounce && actions.items[i].all_peers)
                    broadcast = true;
            if (echo && !broadcast)
                add(&actions, (fips_stp_action){.kind = FIPS_STP_SendAnnounce, .peer = from});
            *out = actions;
            return FIPS_OK;
        }
    }
    add(&actions, drop);
    *out = actions;
    return FIPS_OK;
}
fips_status fips_stp_periodic(fips_tree_state *s, const fips_stp_ranking *r, uint64_t ts,
                              fips_instant now, fips_stp_actions *out) {
    if (preflight(s) != FIPS_OK)
        return FIPS_ERR_RANGE;
    fips_tree_decision d =
        fips_stp_classify_periodic(s, r, fips_tree_state_is_switch_suppressed(s, now));
    if (d.kind != FIPS_TREE_PeriodicRebroadcast && s->my_declaration.sequence == UINT64_MAX)
        return FIPS_ERR_RANGE;
    fips_stp_actions a = {0};
    drive(s, d, r, ts, now, &a);
    *out = a;
    return FIPS_OK;
}
fips_status fips_stp_remove_peer(fips_tree_state *s, fips_node_addr peer, const fips_stp_ranking *r,
                                 uint64_t ts, fips_instant now, fips_stp_actions *out) {
    if (preflight(s) != FIPS_OK)
        return FIPS_ERR_RANGE;
    bool parent = !fips_tree_state_is_root(s) && !addr_cmp(s->my_declaration.parent_id, peer);
    if (parent && s->my_declaration.sequence == UINT64_MAX)
        return FIPS_ERR_RANGE;
    fips_tree_state_remove_peer(s, peer);
    fips_stp_actions a = {0};
    if (parent) {
        fips_parent_loss loss;
        fips_tree_state_recover(s, r, ts, now, &loss);
        add(&a, (fips_stp_action){.kind = FIPS_STP_SignDeclaration});
        add(&a, (fips_stp_action){.kind = fips_tree_state_is_root(s) ? FIPS_STP_BecameRoot
                                                                     : FIPS_STP_ChangeParent,
                                  .peer = s->my_declaration.parent_id,
                                  .invalidate_via_node = true,
                                  .invalidate_other_roots = true,
                                  .reset_backoff = true,
                                  .dampened = loss.dampened});
        add(&a, (fips_stp_action){.kind = FIPS_STP_SendAnnounce, .all_peers = true});
    }
    add(&a, (fips_stp_action){
                .kind = FIPS_STP_RefreshFilters, .all_peers = true, .changed_only = true});
    *out = a;
    return FIPS_OK;
}
