/* Coordinate lookup (upstream handlers/lookup.rs over core lookup): requests
 * are deduplicated, answered as target with a signed proof, or forwarded to
 * bloom-matching tree peers; responses are relayed along the recorded reverse
 * path or verified at the origin and written to the coordinate cache. */
#include "node/handlers/internal.h"

bool hs_lookup_init(handlers *h)
{
    fips_lookup_backoff backoff; fips_lookup_forward_rate_limiter limiter;
    if (fips_lookup_backoff_init(&backoff, h->backoff_entries, h->cfg.session_capacity, 5, 300) != FIPS_OK) return false;
    if (fips_lookup_forward_rate_limiter_init(&limiter, h->forward_entries, FIPS_MIN_RECENT_PER_PEER, FIPS_LOOKUP_FORWARD_MIN_INTERVAL_MS) != FIPS_OK) return false;
    return fips_lookup_init(&h->lookup, h->recent, FIPS_MIN_RECENT_PER_PEER * 2, h->pending_lookups, h->cfg.session_capacity, backoff, limiter) == FIPS_OK;
}
static fips_node_addr view_peer_at(void *ctx, size_t index)
{
    handlers *h = ctx; size_t n = 0;
    for (size_t i = 0; i < h->cfg.peer_capacity; ++i) if (h->recs[i].used && n++ == index) return h->recs[i].addr;
    return (fips_node_addr){{0}};
}
static bool view_is_tree_peer(void *ctx, const fips_node_addr *peer)
{ handlers *h = ctx; return hs_tree_is_parent(h, peer) || hs_tree_is_child(h, peer); }
static bool view_may_reach(void *ctx, const fips_node_addr *peer, const fips_node_addr *target)
{ return hs_bloom_may_reach(ctx, peer, target); }
static fips_lookup_routing_view view(handlers *h)
{
    size_t n = 0;
    for (size_t i = 0; i < h->cfg.peer_capacity; ++i) if (h->recs[i].used) ++n;
    return (fips_lookup_routing_view){h, n, view_peer_at, view_is_tree_peer, view_may_reach};
}
static void send_actions(handlers *h, const fips_lookup_action_list *actions, const fips_node_addr *target, size_t *sent)
{
    for (size_t i = 0; i < actions->len; ++i) {
        const fips_lookup_action *a = &actions->items[i];
        switch (a->kind) {
        case FIPS_LOOKUP_SendLink: {
            peer_rec *rec = hs_rec_find(h, &a->peer);
            if (rec && hs_link_send(h, rec, a->bytes, a->bytes_len) && sent) ++*sent;
            break;
        }
        case FIPS_LOOKUP_CacheCoords: {
            fips_cached_coord cc;
            uint8_t addrs[FIPS_CACHED_COORD_MAX_ENTRIES][16];
            size_t n = a->coords->len < FIPS_CACHED_COORD_MAX_ENTRIES ? a->coords->len : FIPS_CACHED_COORD_MAX_ENTRIES;
            for (size_t j = 0; j < n; ++j) memcpy(addrs[j], a->coords->entries[j].node_addr.bytes, 16);
            if (fips_cached_coord_from_addrs(&cc, (const uint8_t (*)[16])addrs, n) == FIPS_OK) {
                if (a->path_mtu && a->path_mtu != UINT16_MAX)
                    (void)fips_coord_cache_insert_verified_with_path_mtu(&h->cache, a->target.bytes, &cc, hs_now(h), a->path_mtu);
                else (void)fips_coord_cache_insert_verified(&h->cache, a->target.bytes, &cc, hs_now(h));
            }
            break;
        }
        case FIPS_LOOKUP_WritePathMtu: hs_host_link_mtu_changed(h, a->path_mtu); break;
        case FIPS_LOOKUP_ResetWarmupIfEstablished: break;
        case FIPS_LOOKUP_RetryQueuedPackets:
            hs_session_coords_resolved(h, target ? target : &a->target);
            break;
        }
    }
}
void hs_lookup_request_received(handlers *h, peer_rec *rec, const uint8_t *body, size_t len)
{
    ++h->counters.lookup_req_received;
    fips_reader r; fips_reader_init(&r, body, len);
    fips_lookup_request *req = h->request_scratch;
    if (fips_lookup_request_decode(&r, req, NULL) != FIPS_OK) { ++h->counters.lookup_decode_error; METRIC(h, lookup, req_decode_error); return; }
    size_t peers = view(h).peer_count;
    fips_lookup_classification cls;
    if (fips_lookup_classify_request(&h->lookup, req, &rec->addr, &h->me, hs_now_ms(h), h->cfg.lookup_recent_expiry_ms,
            FIPS_MIN_RECENT_PER_PEER * 2, peers, &cls) != FIPS_OK) return;
    if (cls.has_eviction) METRIC(h, lookup, req_dedup_evicted);
    fips_lookup_action items[FIPS_LOOKUP_MAX_FORWARD_PEERS > 64 ? 64 : FIPS_LOOKUP_MAX_FORWARD_PEERS];
    fips_lookup_action_list actions = {items, 0, sizeof items / sizeof items[0], h->lookup_bytes, sizeof h->lookup_bytes};
    switch (cls.outcome) {
    case FIPS_REQUEST_Duplicate: ++h->counters.lookup_req_duplicate; METRIC(h, lookup, req_deduplicated); return;
    case FIPS_REQUEST_OwnRequestLooped: METRIC(h, lookup, req_own_loopback); return;
    case FIPS_REQUEST_TtlExhausted: ++h->counters.lookup_req_ttl_exhausted; return;
    case FIPS_REQUEST_RespondAsTarget: {
        ++h->counters.lookup_req_target_is_us;
        fips_lookup_response *resp = h->response_scratch;
        memset(resp, 0, sizeof *resp);
        resp->request_id = req->request_id; resp->target = h->me; resp->path_mtu = UINT16_MAX;
        resp->target_coords = *fips_tree_state_my_coords(h->tree);
        bool signed_out = false;
        if (fips_lookup_response_sign(resp, h->identity, &h->sign_limiter, &rec->addr, hs_now(h), h->scratch, sizeof h->scratch, &signed_out, NULL) != FIPS_OK || !signed_out) {
            ++h->counters.lookup_req_rate_limited; return;
        }
        fips_writer w; fips_writer_init(&w, h->tx_msg, sizeof h->tx_msg);
        if (fips_lookup_response_encode(resp, &w, NULL) == FIPS_OK) (void)hs_link_send(h, rec, h->tx_msg, fips_writer_len(&w));
        return;
    }
    case FIPS_REQUEST_Forward: {
        fips_lookup_routing_view rv = view(h);
        fips_forward_result result;
        if (fips_lookup_plan_transit_forward(&h->lookup, req, &rec->addr, &rv, hs_now_ms(h), &actions, &result) != FIPS_OK) return;
        if (result.used_fallback) METRIC(h, lookup, req_fallback_forwarded);
        switch (result.outcome) {
        case FIPS_FORWARD_Forward: ++h->counters.lookup_req_forwarded; send_actions(h, &actions, NULL, NULL); break;
        case FIPS_FORWARD_NoPeers: ++h->counters.lookup_req_no_tree_peer; break;
        case FIPS_FORWARD_RateLimited: METRIC(h, lookup, req_forward_rate_limited); break;
        case FIPS_FORWARD_TtlExhausted: ++h->counters.lookup_req_ttl_exhausted; break;
        }
        return;
    }
    }
}
void hs_lookup_response_received(handlers *h, peer_rec *rec, const uint8_t *body, size_t len)
{
    ++h->counters.lookup_resp_received;
    fips_reader r; fips_reader_init(&r, body, len);
    fips_lookup_response *resp = h->response_scratch;
    if (fips_lookup_response_decode(&r, resp, NULL) != FIPS_OK) { ++h->counters.lookup_decode_error; METRIC(h, lookup, resp_decode_error); return; }
    const fips_peer_identity *target = NULL;
    fipsd_active_peer *direct = fipsd_peer_table_find(h->peers->table, &resp->target);
    if (direct) target = &fipsd_active_peer_info(direct)->identity;
    else target = fipsd_identity_cache_touch(&h->identities, fips_node_addr_prefix(&resp->target), hs_wall_ms(h));
    const fipsd_peer_info *info = fipsd_active_peer_info(rec->peer);
    fipsd_transport *t = hs_transport(h, info->transport);
    uint16_t mtu = t ? t->ops->mtu(t, &info->addr) : 1280;
    fips_lookup_action items[8];
    fips_lookup_action_list actions = {items, 0, 8, h->lookup_bytes, sizeof h->lookup_bytes};
    fips_response_route route; fips_proto_error e;
    fips_status s = fips_lookup_handle_response(&h->lookup, resp, &rec->addr, mtu, hs_now_ms(h), h->secp, target,
        h->scratch, sizeof h->scratch, &actions, &route, &e);
    if (s != FIPS_OK) {
        if (route.kind == FIPS_RESPONSE_Originator && !target) METRIC(h, lookup, resp_identity_miss);
        else ++h->counters.lookup_resp_proof_failed;
        return;
    }
    switch (route.kind) {
    case FIPS_RESPONSE_Originator:
        ++h->counters.lookup_resp_accepted;
        send_actions(h, &actions, &resp->target, NULL);
        hs_notice(h, FIPSD_NOTICE_LOOKUP_RESOLVED, &resp->target, NULL, 0);
        hs_mark_dirty(h);
        break;
    case FIPS_RESPONSE_Transit: ++h->counters.lookup_resp_forwarded; send_actions(h, &actions, NULL, NULL); break;
    case FIPS_RESPONSE_Unsolicited: case FIPS_RESPONSE_Ineligible: ++h->counters.lookup_resp_unsolicited; break;
    case FIPS_RESPONSE_AlreadyForwarded: break;
    }
}
static bool send_request(handlers *h, const fips_node_addr *target, fips_pending_lookup *pending, size_t *fanout)
{
    if (fanout) *fanout = 0;
    fips_lookup_request *req = h->request_scratch;
    memset(req, 0, sizeof *req);
    h->entropy.fill(h->entropy.ctx, (uint8_t *)&req->request_id, sizeof req->request_id);
    req->target = *target; req->origin = h->me; req->origin_coords = *fips_tree_state_my_coords(h->tree);
    req->ttl = 16; req->min_mtu = UINT16_MAX;
    fips_lookup_action items[64];
    fips_lookup_action_list actions = {items, 0, 64, h->lookup_bytes, sizeof h->lookup_bytes};
    fips_lookup_routing_view rv = view(h);
    if (fips_lookup_plan_initiate(req, &rv, &actions) != FIPS_OK) return false;
    size_t sent = 0;
    send_actions(h, &actions, NULL, &sent);
    if (fanout) *fanout = sent;
    if (sent && pending) fips_pending_lookup_record(pending, req->request_id);
    return sent > 0;
}
/* The initiation outcome in the probe's vocabulary (lookup.rs LookupOutcome):
 * the diagnostic probe's bloom stage needs it, since a request answered within
 * one tick never shows in the pending table. `fanout` is the number of tree
 * peers the request went to (meaningful for Sent and ZeroFanout). */
fips_probe_lookup_outcome_kind hs_lookup_initiate_outcome(handlers *h, const fips_node_addr *target, size_t *fanout)
{
    if (fanout) *fanout = 0;
    bool reachable = false;
    for (size_t i = 0; i < h->cfg.peer_capacity && !reachable; ++i)
        if (h->recs[i].used && hs_bloom_may_reach(h, &h->recs[i].addr, target)) reachable = true;
    fips_initiate_decision d; uint32_t failures = 0;
    /* The C gate alone can refuse an otherwise admissible target when the
     * pending table is full (upstream's gate is infallible): the host declined
     * to initiate, which is reported as suppressed, not as a lack of tree peers. */
    if (fips_lookup_initiate_gate(&h->lookup, target, hs_now_ms(h), reachable, &d, &failures) != FIPS_OK) return FIPS_PROBE_LOOKUP_Suppressed;
    switch (d) {
    case FIPS_INITIATE_Deduplicated: return FIPS_PROBE_LOOKUP_Deduplicated;
    case FIPS_INITIATE_Suppressed: ++h->counters.lookup_req_backoff_suppressed; return FIPS_PROBE_LOOKUP_Suppressed;
    case FIPS_INITIATE_BloomMiss: ++h->counters.lookup_req_bloom_miss; return FIPS_PROBE_LOOKUP_BloomMiss;
    case FIPS_INITIATE_Proceed: break;
    }
    ++h->counters.lookup_req_initiated;
    fips_pending_lookup *p = fips_lookup_pending(&h->lookup, target);
    if (!send_request(h, target, p, fanout)) { fips_lookup_initiate_failed(&h->lookup, target, hs_now_ms(h)); return FIPS_PROBE_LOOKUP_ZeroFanout; }
    hs_mark_dirty(h);
    return FIPS_PROBE_LOOKUP_Sent;
}
bool hs_lookup_initiate(handlers *h, const fips_node_addr *target)
{
    fips_probe_lookup_outcome_kind kind = hs_lookup_initiate_outcome(h, target, NULL);
    return kind == FIPS_PROBE_LOOKUP_Sent || kind == FIPS_PROBE_LOOKUP_Deduplicated;
}
void hs_lookup_tick(handlers *h, fips_instant now)
{
    (void)now;
    fips_lookup_poll_entry retries[16], timeouts[16];
    fips_lookup_poll_outcome out = {retries, timeouts, 0, 16, 0, 16};
    if (fips_lookup_poll_pending(&h->lookup, hs_now_ms(h), h->cfg.lookup_attempt_timeouts_secs, h->cfg.lookup_attempt_count, &out) == FIPS_OK) {
        for (size_t i = 0; i < out.retries_len; ++i) {
            fips_pending_lookup *p = fips_lookup_pending(&h->lookup, &retries[i].target);
            (void)send_request(h, &retries[i].target, p, NULL);
        }
        for (size_t i = 0; i < out.timeouts_len; ++i) {
            ++h->counters.lookup_resp_timed_out;
            fips_lookup_initiate_failed(&h->lookup, &timeouts[i].target, hs_now_ms(h));
            hs_notice(h, FIPSD_NOTICE_LOOKUP_FAILED, &timeouts[i].target, NULL, 0);
        }
    }
    fips_lookup_purge_recent(&h->lookup, hs_now_ms(h), h->cfg.lookup_recent_expiry_ms);
    fips_lookup_forward_prune(&h->lookup.forward_limiter, hs_now_ms(h));
    fips_lookup_backoff_prune(&h->lookup.backoff, hs_now_ms(h));
}
bool hs_cache_lookup(handlers *h, const fips_node_addr *dest, fips_node_addr *addrs, size_t capacity, size_t *count, uint16_t *path_mtu)
{
    const fips_cached_coord *c = fips_coord_cache_get_and_touch(&h->cache, dest->bytes, hs_now(h));
    if (!c || !c->len) return false;
    size_t n = c->len < capacity ? c->len : capacity;
    for (size_t i = 0; i < n; ++i) memcpy(addrs[i].bytes, c->entries[i].node_addr, 16);
    *count = n;
    const fips_cache_entry *e = fips_coord_cache_get_entry(&h->cache, dest->bytes);
    *path_mtu = e && e->has_path_mtu ? e->path_mtu : 0;
    return true;
}
const fips_tree_coordinate *hs_cache_coords(void *ctx, const fips_node_addr *dest, fips_instant now)
{
    handlers *h = ctx;
    const fips_cached_coord *c = fips_coord_cache_get(&h->cache, dest->bytes, now);
    if (!c || !c->len) return NULL;
    fips_node_addr addrs[FIPS_CACHED_COORD_MAX_ENTRIES];
    for (size_t i = 0; i < c->len; ++i) memcpy(addrs[i].bytes, c->entries[i].node_addr, 16);
    if (fips_tree_coordinate_from_addrs(h->coord_scratch, addrs, c->len, NULL) != FIPS_OK) return NULL;
    return h->coord_scratch;
}
