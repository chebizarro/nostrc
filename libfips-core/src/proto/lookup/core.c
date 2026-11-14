#include "internal.h"
static size_t select_peers(const fips_lookup_request *r, const fips_node_addr *from,
    const fips_lookup_routing_view *rv, bool initiate, fips_node_addr peers[FIPS_LOOKUP_MAX_FORWARD_PEERS], bool *fallback) {
    bool tree = false;
    for (size_t i = 0; i < rv->peer_count; i++) {
        fips_node_addr p = rv->peer_at(rv->ctx, i);
        if (rv->may_reach(rv->ctx, &p, &r->target) && rv->is_tree_peer(rv->ctx, &p)) { tree = true; break; }
    }
    *fallback = !tree;
    if (initiate && !tree) return 0;
    size_t n = 0;
    for (size_t i = 0; i < rv->peer_count; i++) {
        fips_node_addr p = rv->peer_at(rv->ctx, i);
        if (rv->is_tree_peer(rv->ctx, &p) != tree || !rv->may_reach(rv->ctx, &p, &r->target) ||
            (from && !lookup_addr_cmp(p, *from))) continue;
        if (n == FIPS_LOOKUP_MAX_FORWARD_PEERS) return SIZE_MAX;
        peers[n++] = p;
    }
    return n;
}
static fips_status send_capacity(const fips_lookup_action_list *a, size_t n, size_t bytes) {
    return n > a->capacity || (n && bytes > a->bytes_capacity) ? FIPS_ERR_RANGE : FIPS_OK;
}
static void send_actions(fips_lookup_action_list *a, const fips_node_addr *peers, size_t n, size_t bytes) {
    for (size_t i = 0; i < n; i++) a->items[i] = (fips_lookup_action){.kind = FIPS_LOOKUP_SendLink,
        .peer = peers[i], .bytes = a->bytes, .bytes_len = bytes};
    a->len = n;
}
static fips_status forward(fips_lookup *l, fips_lookup_request *r, const fips_node_addr *from,
    const fips_lookup_routing_view *rv, uint64_t now, fips_lookup_action_list *a, fips_forward_result *out) {
    if (!r->ttl) { a->len = 0; *out = (fips_forward_result){.outcome = FIPS_FORWARD_TtlExhausted}; return FIPS_OK; }
    fips_node_addr peers[FIPS_LOOKUP_MAX_FORWARD_PEERS];
    bool fallback;
    size_t n = select_peers(r, from, rv, false, peers, &fallback);
    if (n == SIZE_MAX || send_capacity(a, n, fips_lookup_request_encoded_len(r)) != FIPS_OK) return FIPS_ERR_RANGE;
    if (n && !r->origin_coords.len) return FIPS_ERR_RANGE;
    r->ttl--;
    if (!n) { a->len = 0; *out = (fips_forward_result){.outcome = FIPS_FORWARD_NoPeers}; return FIPS_OK; }
    if (l && !fips_lookup_forward_should_forward(&l->forward_limiter, from, &r->target, now)) {
        a->len = 0; *out = (fips_forward_result){.outcome = FIPS_FORWARD_RateLimited}; return FIPS_OK;
    }
    fips_writer w;
    fips_writer_init(&w, a->bytes, a->bytes_capacity);
    fips_lookup_request_encode(r, &w, NULL);
    send_actions(a, peers, n, w.pos);
    if (l) {
        fips_recent_request *recent = fips_lookup_recent(l, r->request_id);
        if (recent) { memcpy(recent->forwarded_to, peers, n * sizeof(*peers)); recent->forwarded_len = n; }
    }
    *out = (fips_forward_result){.outcome = FIPS_FORWARD_Forward, .used_fallback = fallback};
    return FIPS_OK;
}
fips_status fips_lookup_plan_forward(fips_lookup_request *r, const fips_node_addr *from,
    const fips_lookup_routing_view *rv, fips_lookup_action_list *a, fips_forward_result *out) { return forward(NULL, r, from, rv, 0, a, out); }
fips_status fips_lookup_plan_transit_forward(fips_lookup *l, fips_lookup_request *r, const fips_node_addr *from,
    const fips_lookup_routing_view *rv, uint64_t now, fips_lookup_action_list *a, fips_forward_result *out) { return forward(l, r, from, rv, now, a, out); }
fips_status fips_lookup_plan_initiate(const fips_lookup_request *r, const fips_lookup_routing_view *rv, fips_lookup_action_list *a) {
    fips_node_addr peers[FIPS_LOOKUP_MAX_FORWARD_PEERS];
    bool fallback;
    size_t n = select_peers(r, NULL, rv, true, peers, &fallback);
    if (n == SIZE_MAX || send_capacity(a, n, fips_lookup_request_encoded_len(r)) != FIPS_OK) return FIPS_ERR_RANGE;
    if (!n) { a->len = 0; return FIPS_OK; }
    fips_writer w;
    fips_writer_init(&w, a->bytes, a->bytes_capacity);
    if (fips_lookup_request_encode(r, &w, NULL) != FIPS_OK) return FIPS_ERR_RANGE;
    send_actions(a, peers, n, w.pos);
    return FIPS_OK;
}
fips_status fips_lookup_classify_request(fips_lookup *l, const fips_lookup_request *r,
    const fips_node_addr *from, const fips_node_addr *my_addr, uint64_t now, uint64_t expiry,
    size_t max_recent, size_t peer_count, fips_lookup_classification *out) {
    if (!max_recent) return FIPS_ERR_RANGE;
    if (max_recent > l->recent_capacity) max_recent = l->recent_capacity;
    fips_lookup_purge_recent(l, now, expiry);
    fips_lookup_classification result = {0};
    if (fips_lookup_recent(l, r->request_id)) { result.outcome = FIPS_REQUEST_Duplicate; *out = result; return FIPS_OK; }
    fips_pending_lookup *p = fips_lookup_pending(l, &r->target);
    if (p && fips_pending_lookup_matches(p, r->request_id)) { result.outcome = FIPS_REQUEST_OwnRequestLooped; *out = result; return FIPS_OK; }
    size_t share = fips_lookup_peer_share(max_recent, peer_count);
    fips_node_addr victim;
    bool evict = false;
    if (fips_lookup_peer_entries(l, from) >= share) { victim = *from; evict = true; }
    else if (l->recent_len >= max_recent) evict = fips_lookup_heaviest_peer(l, &victim);
    if (evict) {
        fips_recent_request old;
        if (fips_lookup_evict_oldest_from(l, &victim, &old)) {
            result.has_eviction = true;
            result.evicted = (fips_lookup_eviction){.request_id = old.request_id, .peer = victim, .share = share,
                .age_ms = lookup_elapsed(now, old.timestamp_ms), .answered = old.relayed_len > 0};
        }
    }
    fips_lookup_record_recent(l, r->request_id, *from, r->target, now);
    result.outcome = !lookup_addr_cmp(r->target, *my_addr) ? FIPS_REQUEST_RespondAsTarget :
        r->ttl ? FIPS_REQUEST_Forward : FIPS_REQUEST_TtlExhausted;
    *out = result;
    return FIPS_OK;
}
const char *fips_ineligibility_token(fips_ineligibility reason) { return reason == FIPS_INELIGIBLE_OtherTarget ? "other-target" : "not-sent-to"; }
/* Separate nonmutating classification lets handle_response preflight storage
 * and crypto before committing the transit digest. */
static fips_response_route response_route(fips_lookup *l, uint64_t id, const fips_node_addr *target,
    const fips_node_addr *from, const uint8_t digest[8]) {
    fips_response_route route = {.kind = FIPS_RESPONSE_Unsolicited};
    fips_pending_lookup *p = fips_lookup_pending(l, target);
    if (p && fips_pending_lookup_matches(p, id)) { route.kind = FIPS_RESPONSE_Originator; return route; }
    fips_recent_request *r = fips_lookup_recent(l, id);
    if (!r) return route;
    route.kind = FIPS_RESPONSE_Ineligible;
    if (lookup_addr_cmp(r->target, *target)) { route.reason = FIPS_INELIGIBLE_OtherTarget; return route; }
    bool sent = false;
    for (size_t i = 0; i < r->forwarded_len; i++) if (!lookup_addr_cmp(r->forwarded_to[i], *from)) { sent = true; break; }
    if (!sent) { route.reason = FIPS_INELIGIBLE_NotSentTo; return route; }
    for (size_t i = 0; i < r->relayed_len; i++) if (!memcmp(r->relayed[i], digest, 8)) { route.kind = FIPS_RESPONSE_AlreadyForwarded; return route; }
    route.kind = FIPS_RESPONSE_Transit; route.from_peer = r->from_peer;
    return route;
}
fips_response_route fips_lookup_classify_response(fips_lookup *l, uint64_t id, const fips_node_addr *target,
    const fips_node_addr *from, const uint8_t digest[8]) {
    fips_response_route route = response_route(l, id, target, from, digest);
    if (route.kind == FIPS_RESPONSE_Transit) fips_recent_request_note_relayed(fips_lookup_recent(l, id), digest);
    return route;
}
static void remove_pending(fips_lookup *l, const fips_node_addr *target) {
    fips_pending_lookup *p = fips_lookup_pending(l, target);
    if (p) { size_t i = (size_t)(p - l->pending_lookups); memmove(p, p + 1, (--l->pending_len - i) * sizeof(*p)); }
}
fips_status fips_lookup_on_response_accepted(fips_lookup *l, uint64_t id, const fips_node_addr *target,
    const fips_tree_coordinate *coords, uint64_t now, uint16_t mtu, fips_lookup_action_list *a) {
    if (a->capacity < 4) return FIPS_ERR_RANGE;
    fips_node_addr destination = *target;
    fips_lookup_backoff_record_success(&l->backoff, &destination);
    remove_pending(l, &destination);
    const fips_lookup_action_kind kinds[4] = {FIPS_LOOKUP_CacheCoords, FIPS_LOOKUP_WritePathMtu,
        FIPS_LOOKUP_ResetWarmupIfEstablished, FIPS_LOOKUP_RetryQueuedPackets};
    for (size_t i = 0; i < 4; i++) a->items[i] = (fips_lookup_action){.kind = kinds[i], .target = destination,
        .request_id = id, .now_ms = now, .path_mtu = mtu, .coords = i == 0 ? coords : NULL};
    a->len = 4;
    return FIPS_OK;
}
fips_status fips_lookup_handle_response(fips_lookup *l, const fips_lookup_response *r,
    const fips_node_addr *from, uint16_t outgoing_mtu, uint64_t now, const fips_secp *secp,
    const fips_peer_identity *peer, uint8_t *scratch, size_t scratch_capacity,
    fips_lookup_action_list *a, fips_response_route *out, fips_proto_error *e) {
    uint8_t digest[8];
    fips_lookup_response_proof_digest(r, digest);
    fips_response_route route = response_route(l, r->request_id, &r->target, from, digest);
    if (route.kind == FIPS_RESPONSE_Originator) {
        if (a->capacity < 4) return FIPS_ERR_RANGE;
        if (fips_lookup_response_verify(r, secp, peer, scratch, scratch_capacity, e) != FIPS_OK) return FIPS_ERR_RANGE;
        fips_lookup_on_response_accepted(l, r->request_id, &r->target, &r->target_coords, now, r->path_mtu, a);
    } else if (route.kind == FIPS_RESPONSE_Transit) {
        if (send_capacity(a, 1, fips_lookup_response_encoded_len(r)) != FIPS_OK) return FIPS_ERR_RANGE;
        fips_writer w;
        fips_writer_init(&w, a->bytes, a->bytes_capacity);
        if (fips_lookup_response_encode(r, &w, e) != FIPS_OK) return FIPS_ERR_RANGE;
        uint16_t mtu = r->path_mtu < outgoing_mtu ? r->path_mtu : outgoing_mtu;
        a->bytes[25] = (uint8_t)mtu; a->bytes[26] = (uint8_t)(mtu >> 8);
        send_actions(a, &route.from_peer, 1, w.pos);
        fips_recent_request_note_relayed(fips_lookup_recent(l, r->request_id), digest);
    } else a->len = 0;
    *out = route;
    return FIPS_OK;
}
fips_status fips_lookup_poll_pending(fips_lookup *l, uint64_t now, const uint64_t *ladder,
    size_t count, fips_lookup_poll_outcome *out) {
    size_t retries = 0, timeouts = 0;
    size_t max_attempts = count > UINT8_MAX ? UINT8_MAX : count;
    for (size_t i = 0; i < l->pending_len; i++) {
        const fips_pending_lookup *p = &l->pending_lookups[i];
        size_t idx = p->attempt ? (size_t)p->attempt - 1 : 0;
        uint64_t wait = idx < count ? lookup_millis(ladder[idx]) : 0;
        if (lookup_elapsed(now, p->last_sent_ms) >= wait) {
            if (p->attempt >= max_attempts) timeouts++; else retries++;
        }
    }
    if (retries > out->retries_capacity || timeouts > out->timeouts_capacity) return FIPS_ERR_RANGE;
    fips_lookup_backoff_prune(&l->backoff, now);
    out->retries_len = out->timeouts_len = 0;
    for (size_t i = 0; i < l->pending_len;) {
        fips_pending_lookup *p = &l->pending_lookups[i];
        size_t idx = p->attempt ? (size_t)p->attempt - 1 : 0;
        uint64_t wait = idx < count ? lookup_millis(ladder[idx]) : 0;
        if (lookup_elapsed(now, p->last_sent_ms) < wait) { i++; continue; }
        if (p->attempt < max_attempts) {
            p->attempt++; p->last_sent_ms = now;
            out->retries[out->retries_len++] = (fips_lookup_poll_entry){.target = p->target, .attempt = p->attempt};
            i++;
        } else {
            fips_node_addr target = p->target;
            remove_pending(l, &target);
            fips_lookup_backoff_record_failure(&l->backoff, &target, now);
            out->timeouts[out->timeouts_len++] = (fips_lookup_poll_entry){.target = target,
                .has_failures = fips_lookup_backoff_is_enabled(&l->backoff),
                .failures = fips_lookup_backoff_failure_count(&l->backoff, &target)};
        }
    }
    return FIPS_OK;
}
fips_status fips_lookup_initiate_gate(fips_lookup *l, const fips_node_addr *dest, uint64_t now,
    bool reachable, fips_initiate_decision *out, uint32_t *failures) {
    fips_initiate_decision result;
    uint32_t n = 0;
    if (fips_lookup_pending(l, dest)) result = FIPS_INITIATE_Deduplicated;
    else if (fips_lookup_backoff_is_suppressed(&l->backoff, dest, now)) {
        result = FIPS_INITIATE_Suppressed; n = fips_lookup_backoff_failure_count(&l->backoff, dest);
    } else if (!reachable) { fips_lookup_backoff_record_failure(&l->backoff, dest, now); result = FIPS_INITIATE_BloomMiss; }
    else {
        if (l->pending_len == l->pending_capacity) return FIPS_ERR_RANGE;
        size_t i = 0;
        while (i < l->pending_len && lookup_addr_cmp(l->pending_lookups[i].target, *dest) < 0) i++;
        memmove(l->pending_lookups + i + 1, l->pending_lookups + i, (l->pending_len - i) * sizeof(*l->pending_lookups));
        fips_pending_lookup_init(&l->pending_lookups[i], *dest, now);
        l->pending_len++; result = FIPS_INITIATE_Proceed;
    }
    *out = result; *failures = n;
    return FIPS_OK;
}
void fips_lookup_initiate_failed(fips_lookup *l, const fips_node_addr *dest, uint64_t now) {
    fips_node_addr destination = *dest;
    remove_pending(l, &destination);
    fips_lookup_backoff_record_failure(&l->backoff, &destination, now);
}
