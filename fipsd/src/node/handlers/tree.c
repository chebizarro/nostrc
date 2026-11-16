/* Spanning tree (upstream node/tree.rs over core STP): announces are verified
 * against the authenticated link peer's key, fed to fips_stp_receive_announce
 * with MMP-derived link costs, and the returned actions are executed in order. */
#include "node/handlers/internal.h"

static bool sign_declaration(handlers *h)
{
    fips_parent_declaration *d = fips_tree_state_my_declaration_mut(h->tree);
    uint8_t bytes[FIPS_PARENT_DECLARATION_SIGNING_SIZE], sig[64];
    fips_writer w; fips_writer_init(&w, bytes, sizeof bytes);
    if (fips_parent_declaration_signing_bytes(d, &w, NULL) != FIPS_OK ||
        fips_identity_sign(h->identity, bytes, fips_writer_len(&w), sig) != FIPS_IDENTITY_OK) {
        ++h->counters.tree_sign_failed; return false;
    }
    fips_parent_declaration_set_signature(d, sig);
    return true;
}
void hs_tree_refresh_my_addrs(handlers *h)
{
    const fips_tree_coordinate *c = fips_tree_state_my_coords(h->tree);
    size_t n = c->len < FIPSD_HANDLERS_COORD_ADDRS ? c->len : FIPSD_HANDLERS_COORD_ADDRS;
    for (size_t i = 0; i < n; ++i) h->my_coord_addrs[i] = c->entries[i].node_addr;
    h->my_coord_count = n;
}
bool hs_tree_init(handlers *h)
{
    fips_tree_state_new(h->tree, h->me, hs_wall_secs(h), h->origin, h->tree_peers, h->cfg.tree_peer_capacity);
    fips_tree_state_set_parent_hysteresis(h->tree, h->cfg.parent_hysteresis);
    if (!sign_declaration(h)) return false;
    hs_tree_refresh_my_addrs(h);
    return true;
}
void hs_tree_ranking(handlers *h, fips_stp_peer_cost *costs, size_t capacity, fips_stp_ranking *out)
{
    size_t n = 0;
    for (size_t i = 0; i < h->cfg.peer_capacity && n < capacity; ++i) {
        peer_rec *rec = &h->recs[i];
        double srtt;
        if (rec->used && hs_mmp_srtt(h, rec, &srtt)) costs[n++] = (fips_stp_peer_cost){rec->addr, hs_mmp_link_cost(h, rec)};
    }
    *out = (fips_stp_ranking){costs, n, NULL, 0};
}
bool hs_tree_is_parent(handlers *h, const fips_node_addr *peer)
{
    const fips_parent_declaration *d = fips_tree_state_my_declaration(h->tree);
    return !fips_parent_declaration_is_root(d) && !memcmp(d->parent_id.bytes, peer->bytes, 16);
}
bool hs_tree_is_child(handlers *h, const fips_node_addr *peer)
{
    const fips_parent_declaration *d = fips_tree_state_peer_declaration(h->tree, *peer);
    return d && !fips_parent_declaration_is_root(d) && !memcmp(d->parent_id.bytes, h->me.bytes, 16);
}
const fips_tree_coordinate *hs_tree_peer_coords(handlers *h, const fips_node_addr *peer)
{ return fips_tree_state_peer_coords(h->tree, *peer); }
static void tree_changed(handlers *h)
{
    hs_tree_refresh_my_addrs(h);
    hs_bloom_roles(h);
    fipsd_node_notice n = {.kind = FIPSD_NOTICE_TREE_CHANGED, .root = *fips_tree_state_root(h->tree),
        .is_root = fips_tree_state_is_root(h->tree), .depth = fips_tree_coordinate_depth(fips_tree_state_my_coords(h->tree))};
    n.peer = fips_tree_state_my_declaration(h->tree)->parent_id;
    hs_notice_full(h, &n, NULL, 0);
    hs_mark_dirty(h);
}
static void execute(handlers *h, const fips_stp_actions *a)
{
    for (size_t i = 0; i < a->len; ++i) {
        const fips_stp_action *x = &a->items[i];
        switch (x->kind) {
        case FIPS_STP_SignDeclaration:
            if (!sign_declaration(h)) FIPSD_ERROR(HMOD, "declaration signing failed");
            break;
        case FIPS_STP_SendAnnounce:
            if (x->all_peers) {
                for (size_t j = 0; j < h->cfg.peer_capacity; ++j) if (h->recs[j].used) { h->recs[j].tree_pending = true; (void)hs_tree_send_announce(h, &h->recs[j], false); }
            } else {
                peer_rec *rec = hs_rec_find(h, &x->peer);
                if (rec) { rec->tree_pending = true; (void)hs_tree_send_announce(h, rec, false); }
            }
            break;
        case FIPS_STP_ChangeParent:
            ++h->counters.tree_switches;
            if (x->invalidate_via_node) (void)fips_coord_cache_invalidate_via_node(&h->cache, x->peer.bytes);
            if (x->reset_backoff) (void)fips_lookup_reset_backoff(&h->lookup);
            tree_changed(h);
            break;
        case FIPS_STP_BecameRoot:
            ++h->counters.tree_losses;
            if (x->invalidate_other_roots) (void)fips_coord_cache_invalidate_other_roots(&h->cache, h->me.bytes);
            if (x->reset_backoff) (void)fips_lookup_reset_backoff(&h->lookup);
            tree_changed(h);
            break;
        case FIPS_STP_AncestryChanged:
            if (x->invalidate_via_node) (void)fips_coord_cache_invalidate_via_node(&h->cache, x->peer.bytes);
            tree_changed(h);
            break;
        case FIPS_STP_RefreshFilters:
            if (x->changed_only) fips_bloom_state_mark_changed(&h->bloom, NULL);
            else fips_bloom_state_mark_all(&h->bloom);
            break;
        case FIPS_STP_Drop:
            ++h->counters.tree_dropped;
            if (x->dampened) FIPSD_DEBUG(HMOD, "tree announce dampened");
            break;
        }
    }
}
void hs_tree_announce_received(handlers *h, peer_rec *rec, const uint8_t *body, size_t len)
{
    ++h->counters.tree_received;
    fips_reader r; fips_reader_init(&r, body, len);
    fips_tree_announce *a = h->announce_scratch;
    if (fips_tree_announce_decode(&r, a, NULL) != FIPS_OK) { ++h->counters.tree_dropped; return; }
    bool verified = false;
    if (a->declaration.is_signed && !memcmp(a->declaration.node_addr.bytes, rec->addr.bytes, 16)) {
        uint8_t bytes[FIPS_PARENT_DECLARATION_SIGNING_SIZE];
        fips_writer w; fips_writer_init(&w, bytes, sizeof bytes);
        verified = fips_parent_declaration_signing_bytes(&a->declaration, &w, NULL) == FIPS_OK &&
            fips_peer_identity_verify(h->secp, &fipsd_active_peer_info(rec->peer)->identity, bytes,
                                      fips_writer_len(&w), a->declaration.signature) == FIPS_IDENTITY_OK;
    }
    /* Root disagreement: a peer advertising a worse root hears ours again. */
    if (fips_stp_should_echo(*fips_tree_coordinate_root_id(&a->ancestry), *fips_tree_state_root(h->tree)))
        rec->tree_pending = true;
    fips_stp_peer_cost costs[64]; fips_stp_ranking ranking;
    hs_tree_ranking(h, costs, 64, &ranking);
    fips_stp_actions actions;
    fips_status s = fips_stp_receive_announce(h->tree, rec->addr, a, true, verified, &ranking, hs_wall_secs(h), hs_now(h), &actions);
    if (s != FIPS_OK) { ++h->counters.tree_dropped; FIPSD_WARN(HMOD, "tree announce refused by local storage bounds"); return; }
    bool accepted = true;
    for (size_t i = 0; i < actions.len; ++i) if (actions.items[i].kind == FIPS_STP_Drop) accepted = false;
    if (accepted) {
        ++h->counters.tree_accepted; ++rec->tree_received;
        size_t n = a->ancestry.len < FIPSD_PEER_COORD_CAPACITY ? a->ancestry.len : FIPSD_PEER_COORD_CAPACITY;
        (void)fipsd_peer_data_tree(fipsd_active_peer_data(rec->peer), &a->declaration, a->ancestry.entries, n);
        (void)fipsd_active_peer_publish(rec->peer);
    }
    execute(h, &actions);
    hs_bloom_roles(h);
    if (rec->used && rec->tree_pending) (void)hs_tree_send_announce(h, rec, false);
    hs_mark_dirty(h);
}
bool hs_tree_send_announce(handlers *h, peer_rec *rec, bool force)
{
    fips_instant now = hs_now(h);
    if (!force && rec->last_tree_announce.ops && hs_elapsed_ms(now, rec->last_tree_announce) < h->cfg.tree_announce_min_interval_ms) {
        rec->tree_pending = true; ++h->counters.tree_rate_limited; return false;
    }
    if (!fips_tree_state_is_declaration_signed(h->tree) && !sign_declaration(h)) return false;
    fips_tree_announce *a = h->announce_scratch;
    a->declaration = *fips_tree_state_my_declaration(h->tree);
    a->ancestry = *fips_tree_state_my_coords(h->tree);
    fips_writer w; fips_writer_init(&w, h->tx_msg, sizeof h->tx_msg);
    if (fips_tree_announce_encode(a, &w, NULL) != FIPS_OK) return false;
    if (!hs_link_send(h, rec, h->tx_msg, fips_writer_len(&w))) { rec->tree_pending = true; return false; }
    rec->last_tree_announce = now; rec->tree_pending = false; ++rec->tree_sent; ++h->counters.tree_sent;
    fipsd_peer_data_tree_sent(fipsd_active_peer_data(rec->peer), hs_now_ms(h));
    return true;
}
void hs_tree_peer_added(handlers *h, peer_rec *rec) { (void)hs_tree_send_announce(h, rec, true); }
void hs_tree_peer_removed(handlers *h, peer_rec *rec)
{
    fips_stp_peer_cost costs[64]; fips_stp_ranking ranking;
    hs_tree_ranking(h, costs, 64, &ranking);
    fips_stp_actions actions;
    if (fips_stp_remove_peer(h->tree, rec->addr, &ranking, hs_wall_secs(h), hs_now(h), &actions) == FIPS_OK) execute(h, &actions);
    hs_bloom_roles(h);
    hs_mark_dirty(h);
}
void hs_tree_tick(handlers *h, fips_instant now)
{
    for (size_t i = 0; i < h->cfg.peer_capacity; ++i)
        if (h->recs[i].used && h->recs[i].tree_pending) (void)hs_tree_send_announce(h, &h->recs[i], false);
    if (h->cfg.tree_reeval_ms && (!h->last_reeval.ops || hs_elapsed_ms(now, h->last_reeval) >= h->cfg.tree_reeval_ms)) {
        h->last_reeval = now;
        fips_stp_peer_cost costs[64]; fips_stp_ranking ranking;
        hs_tree_ranking(h, costs, 64, &ranking);
        fips_stp_actions actions;
        if (fips_stp_periodic(h->tree, &ranking, hs_wall_secs(h), now, &actions) == FIPS_OK) execute(h, &actions);
    }
}
