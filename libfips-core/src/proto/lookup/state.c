#include "internal.h"
fips_status fips_lookup_init(fips_lookup *l, fips_recent_request *recent, size_t recent_capacity,
    fips_pending_lookup *pending, size_t pending_capacity, fips_lookup_backoff backoff,
    fips_lookup_forward_rate_limiter forward_limiter) {
    if (!recent_capacity || recent_capacity > FIPS_MAX_RECENT_LOOKUP_REQUESTS ||
        !pending_capacity || pending_capacity > FIPS_LOOKUP_MAX_PENDING) return FIPS_ERR_RANGE;
    *l = (fips_lookup){.recent_requests = recent, .recent_capacity = recent_capacity,
        .pending_lookups = pending, .pending_capacity = pending_capacity, .backoff = backoff,
        .forward_limiter = forward_limiter};
    return FIPS_OK;
}
void fips_pending_lookup_init(fips_pending_lookup *p, fips_node_addr target, uint64_t now) {
    *p = (fips_pending_lookup){.target = target, .initiated_ms = now, .last_sent_ms = now, .attempt = 1};
}
bool fips_pending_lookup_matches(const fips_pending_lookup *p, uint64_t id) {
    for (size_t i = 0; i < p->ids_len; i++) if (p->ids[i] == id) return true;
    return false;
}
void fips_pending_lookup_record(fips_pending_lookup *p, uint64_t id) {
    if (fips_pending_lookup_matches(p, id)) return;
    if (p->ids_len == FIPS_MAX_RECORDED_IDS) {
        memmove(p->ids, p->ids + 1, (FIPS_MAX_RECORDED_IDS - 1) * sizeof(*p->ids));
        p->ids_len--;
    }
    p->ids[p->ids_len++] = id;
}
fips_pending_lookup *fips_lookup_pending(fips_lookup *l, const fips_node_addr *target) {
    for (size_t i = 0; i < l->pending_len; i++) if (!lookup_addr_cmp(l->pending_lookups[i].target, *target)) return &l->pending_lookups[i];
    return NULL;
}
fips_recent_request *fips_lookup_recent(fips_lookup *l, uint64_t id) {
    for (size_t i = 0; i < l->recent_len; i++) if (l->recent_requests[i].request_id == id) return &l->recent_requests[i];
    return NULL;
}
bool fips_recent_request_is_expired(const fips_recent_request *r, uint64_t now, uint64_t expiry) { return lookup_elapsed(now, r->timestamp_ms) > expiry; }
bool fips_recent_request_note_relayed(fips_recent_request *r, const uint8_t digest[8]) {
    for (size_t i = 0; i < r->relayed_len; i++) if (!memcmp(r->relayed[i], digest, 8)) return false;
    if (r->relayed_len == FIPS_MAX_RELAYED_DIGESTS) {
        memmove(r->relayed, r->relayed + 1, (FIPS_MAX_RELAYED_DIGESTS - 1) * 8);
        r->relayed_len--;
    }
    memcpy(r->relayed[r->relayed_len++], digest, 8);
    return true;
}
size_t fips_lookup_peer_share(size_t max_recent, size_t peer_count) {
    size_t share = max_recent / (peer_count ? peer_count : 1);
    return share > FIPS_MIN_RECENT_PER_PEER ? share : FIPS_MIN_RECENT_PER_PEER;
}
size_t fips_lookup_peer_entries(const fips_lookup *l, const fips_node_addr *peer) {
    size_t n = 0;
    for (size_t i = 0; i < l->recent_len; i++) if (!lookup_addr_cmp(l->recent_requests[i].from_peer, *peer)) n++;
    return n;
}
bool fips_lookup_heaviest_peer(const fips_lookup *l, fips_node_addr *out) {
    size_t best = 0;
    fips_node_addr peer = {{0}};
    for (size_t i = 0; i < l->recent_len; i++) {
        fips_node_addr candidate = l->recent_requests[i].from_peer;
        bool seen = false;
        for (size_t j = 0; j < i; j++) if (!lookup_addr_cmp(candidate, l->recent_requests[j].from_peer)) { seen = true; break; }
        if (seen) continue;
        size_t n = fips_lookup_peer_entries(l, &candidate);
        if (n > best || (n == best && lookup_addr_cmp(candidate, peer) > 0)) { best = n; peer = candidate; }
    }
    if (!best) return false;
    *out = peer;
    return true;
}
fips_status fips_lookup_record_recent(fips_lookup *l, uint64_t id, fips_node_addr from, fips_node_addr target, uint64_t now) {
    if (l->recent_len == l->recent_capacity || fips_lookup_recent(l, id)) return FIPS_ERR_RANGE;
    l->recent_requests[l->recent_len++] = (fips_recent_request){.request_id = id, .from_peer = from, .target = target, .timestamp_ms = now};
    return FIPS_OK;
}
bool fips_lookup_evict_oldest_from(fips_lookup *l, const fips_node_addr *peer, fips_recent_request *out) {
    for (size_t i = 0; i < l->recent_len; i++) if (!lookup_addr_cmp(l->recent_requests[i].from_peer, *peer)) {
        *out = l->recent_requests[i];
        l->evicted[l->evicted_next] = out->request_id;
        l->evicted_next = (l->evicted_next + 1) % FIPS_EVICTED_KEPT;
        if (l->evicted_len < FIPS_EVICTED_KEPT) l->evicted_len++;
        memmove(l->recent_requests + i, l->recent_requests + i + 1, (--l->recent_len - i) * sizeof(*l->recent_requests));
        return true;
    }
    return false;
}
bool fips_lookup_was_evicted(const fips_lookup *l, uint64_t id) {
    for (size_t i = 0; i < l->evicted_len; i++) if (l->evicted[i] == id) return true;
    return false;
}
void fips_lookup_purge_recent(fips_lookup *l, uint64_t now, uint64_t expiry) {
    size_t n = 0;
    for (size_t i = 0; i < l->recent_len; i++)
        if (!fips_recent_request_is_expired(&l->recent_requests[i], now, expiry)) l->recent_requests[n++] = l->recent_requests[i];
    l->recent_len = n;
}
size_t fips_lookup_reset_backoff(fips_lookup *l) { size_t n = l->backoff.len; fips_lookup_backoff_reset_all(&l->backoff); return n; }
