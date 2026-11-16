/* Bloom filter exchange (upstream node/bloom.rs over core bloom state): inbound
 * FilterAnnounce frames are admitted by fips_bloom_state_receive, outgoing
 * filters are planned by fips_bloom_state_refresh on the maintenance tick and
 * after topology changes, and every send result is fed back. */
#include "node/handlers/internal.h"

bool hs_bloom_init(handlers *h)
{
    fips_bloom_state_init(&h->bloom, h->me, h->cfg.leaf_only, h->bloom_peers, h->cfg.peer_capacity, h->leaves, h->cfg.peer_capacity);
    h->bloom.update_debounce_ms = h->cfg.bloom_debounce_ms;
    return true;
}
void hs_bloom_roles(handlers *h)
{
    for (size_t i = 0; i < h->cfg.peer_capacity; ++i) {
        peer_rec *rec = &h->recs[i];
        if (!rec->used) continue;
        fips_bloom_peer *p = fips_bloom_state_peer(&h->bloom, &rec->addr);
        if (!p) continue;
        bool child = hs_tree_is_child(h, &rec->addr), tree = child || hs_tree_is_parent(h, &rec->addr);
        fips_bloom_state_peer_role(&h->bloom, p, tree, child, rec->heard);
    }
}
void hs_bloom_peer_added(handlers *h, peer_rec *rec)
{
    if (fips_bloom_state_add_peer(&h->bloom, rec->addr) != FIPS_OK) FIPSD_WARN(HMOD, "bloom peer storage full");
    hs_bloom_roles(h);
}
void hs_bloom_peer_removed(handlers *h, peer_rec *rec)
{
    (void)fips_bloom_state_remove_peer(&h->bloom, &rec->addr);
    fips_bloom_state_mark_changed(&h->bloom, NULL);
}
void hs_bloom_filter_received(handlers *h, peer_rec *rec, const uint8_t *body, size_t len)
{
    ++h->counters.bloom_received;
    fips_filter_announce *a = malloc(sizeof *a); /* 4 KiB filter: off the fiber stack */
    if (!a) return;
    fips_reader r; fips_reader_init(&r, body, len);
    if (fips_filter_announce_decode(&r, a, NULL) != FIPS_OK) { free(a); return; }
    fips_bloom_peer *p = fips_bloom_state_peer(&h->bloom, &rec->addr);
    if (!p) { free(a); return; }
    uint64_t before = p->inbound_sequence; bool had = p->has_inbound;
    fips_bloom_actions actions = {h->bloom_actions, 0, h->cfg.peer_capacity + 1};
    if (fips_bloom_state_receive(&h->bloom, p, a, hs_now_ms(h), &actions) != FIPS_OK) { free(a); return; }
    for (size_t i = 0; i < actions.len; ++i) if (actions.items[i].kind == FIPS_BLOOM_REFUSED) ++h->counters.bloom_refused;
    if (p->has_inbound && (!had || p->inbound_sequence != before)) {
        ++h->counters.bloom_accepted;
        fipsd_peer_data_filter(fipsd_active_peer_data(rec->peer), &a->filter, a->sequence, hs_wall_ms(h));
        (void)fipsd_active_peer_publish(rec->peer);
        fipsd_node_notice n = {.kind = FIPSD_NOTICE_FILTER_ACCEPTED, .peer = rec->addr, .value = a->sequence};
        hs_notice_full(h, &n, NULL, 0);
        hs_mark_dirty(h);
    }
    free(a);
}
void hs_bloom_refresh(handlers *h, fips_instant now, bool force_all)
{
    (void)now;
    if (force_all) fips_bloom_state_mark_all(&h->bloom);
    if (fips_bloom_state_take_inbound_change(&h->bloom)) fips_bloom_state_mark_changed(&h->bloom, NULL);
    fips_bloom_actions actions = {h->bloom_actions, 0, h->cfg.peer_capacity + 1};
    uint64_t now_ms = hs_now_ms(h);
    if (fips_bloom_state_refresh(&h->bloom, now_ms, ++h->bloom_floor, &actions) != FIPS_OK) return;
    for (size_t i = 0; i < actions.len; ++i) {
        fips_bloom_action *a = &actions.items[i];
        if (a->kind != FIPS_BLOOM_SEND) continue;
        peer_rec *rec = hs_rec_find(h, &a->peer);
        fips_bloom_peer *p = fips_bloom_state_peer(&h->bloom, &a->peer);
        if (!rec || !p) continue;
        fips_writer w; fips_writer_init(&w, h->tx_msg, sizeof h->tx_msg);
        fips_bloom_send_outcome outcome = {.result = FIPS_BLOOM_SEND_FAILED};
        const fipsd_peer_info *info = fipsd_active_peer_info(rec->peer);
        fipsd_transport *t = hs_transport(h, info->transport);
        if (fips_filter_announce_encode(&a->announce, &w, NULL) == FIPS_OK) {
            outcome.packet_size = fips_writer_len(&w) + FIPS_FRAMING_FMP_FRAME_OVERHEAD;
            outcome.mtu = t ? t->ops->mtu(t, &info->addr) : 0;
            if (t && outcome.packet_size > outcome.mtu) outcome.result = FIPS_BLOOM_SEND_TOO_LARGE;
            else if (hs_link_send(h, rec, h->tx_msg, fips_writer_len(&w))) outcome.result = FIPS_BLOOM_SEND_ACCEPTED;
        }
        fips_bloom_actions more = {NULL, 0, 0};
        fips_bloom_link_evidence evidence = {.epoch = rec->evidence.epoch, .next_counter = rec->evidence.next_counter,
            .has_rr = rec->evidence.has_rr, .rr = {rec->evidence.rr.highest, rec->evidence.rr.received, rec->evidence.rr.reordered}};
        (void)fips_bloom_state_sent(&h->bloom, p, a, outcome, 0, &evidence, now_ms, &more);
        if (outcome.result == FIPS_BLOOM_SEND_ACCEPTED) {
            ++h->counters.bloom_sent;
            fipsd_node_notice n = {.kind = FIPSD_NOTICE_FILTER_SENT, .peer = rec->addr, .value = a->announce.sequence};
            hs_notice_full(h, &n, NULL, 0);
        } else ++h->counters.bloom_send_failed;
    }
    hs_mark_dirty(h);
}
void hs_bloom_rr(handlers *h, peer_rec *rec)
{
    fips_bloom_peer *p = fips_bloom_state_peer(&h->bloom, &rec->addr);
    if (!p) return;
    fips_bloom_link_evidence evidence = {.epoch = rec->evidence.epoch, .next_counter = rec->evidence.next_counter,
        .has_rr = rec->evidence.has_rr, .rr = {rec->evidence.rr.highest, rec->evidence.rr.received, rec->evidence.rr.reordered}};
    (void)fips_bloom_peer_check_announce(&h->bloom, p, &evidence, hs_now_ms(h));
}
const fips_bloom_filter *hs_bloom_peer_filter(handlers *h, const fips_node_addr *peer)
{
    fips_bloom_peer *p = fips_bloom_state_peer(&h->bloom, peer);
    return p && p->has_inbound ? &p->inbound : NULL;
}
bool hs_bloom_may_reach(handlers *h, const fips_node_addr *peer, const fips_node_addr *target)
{
    const fips_bloom_filter *f = hs_bloom_peer_filter(h, peer);
    return f && fips_bloom_contains(f, target);
}
