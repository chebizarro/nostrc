#ifndef FIPS_PROTO_LOOKUP_STATE_H
#define FIPS_PROTO_LOOKUP_STATE_H
#include <fips/proto/lookup/limits.h>
#ifdef __cplusplus
extern "C" {
#endif
#define FIPS_MAX_RELAYED_DIGESTS 8u
#define FIPS_MAX_RECORDED_IDS 8u
#define FIPS_MIN_RECENT_PER_PEER 64u
#define FIPS_EVICTED_KEPT 1024u
/* Additional local storage policies, not wire limits. Oversized routing fanout
 * fails with RANGE before mutation; it is never silently truncated. */
#define FIPS_LOOKUP_MAX_FORWARD_PEERS 256u
#define FIPS_LOOKUP_MAX_PENDING 4096u
typedef struct fips_recent_request {
    uint64_t request_id, timestamp_ms;
    fips_node_addr from_peer, target;
    fips_node_addr forwarded_to[FIPS_LOOKUP_MAX_FORWARD_PEERS];
    size_t forwarded_len;
    uint8_t relayed[FIPS_MAX_RELAYED_DIGESTS][8];
    size_t relayed_len;
} fips_recent_request;
typedef struct fips_pending_lookup {
    fips_node_addr target;
    uint64_t initiated_ms, last_sent_ms;
    uint8_t attempt;
    uint64_t ids[FIPS_MAX_RECORDED_IDS];
    size_t ids_len;
} fips_pending_lookup;
typedef struct fips_lookup {
    fips_recent_request *recent_requests; /* arrival order */
    size_t recent_len, recent_capacity;
    fips_pending_lookup *pending_lookups; /* target address order */
    size_t pending_len, pending_capacity;
    fips_lookup_backoff backoff;
    fips_lookup_forward_rate_limiter forward_limiter;
    uint64_t evicted[FIPS_EVICTED_KEPT];
    size_t evicted_len, evicted_next;
} fips_lookup;
FIPS_CORE_API fips_status fips_lookup_init(fips_lookup *l, fips_recent_request *recent, size_t recent_capacity,
    fips_pending_lookup *pending, size_t pending_capacity, fips_lookup_backoff backoff,
    fips_lookup_forward_rate_limiter forward_limiter);
FIPS_CORE_API void fips_pending_lookup_init(fips_pending_lookup *p, fips_node_addr target, uint64_t now_ms);
FIPS_CORE_API void fips_pending_lookup_record(fips_pending_lookup *p, uint64_t request_id);
FIPS_CORE_API bool fips_pending_lookup_matches(const fips_pending_lookup *p, uint64_t request_id);
FIPS_CORE_API fips_pending_lookup *fips_lookup_pending(fips_lookup *l, const fips_node_addr *target);
FIPS_CORE_API fips_recent_request *fips_lookup_recent(fips_lookup *l, uint64_t request_id);
FIPS_CORE_API bool fips_recent_request_is_expired(const fips_recent_request *r, uint64_t now_ms, uint64_t expiry_ms);
FIPS_CORE_API bool fips_recent_request_note_relayed(fips_recent_request *r, const uint8_t digest[8]);
FIPS_CORE_API size_t fips_lookup_peer_share(size_t max_recent, size_t peer_count);
FIPS_CORE_API size_t fips_lookup_peer_entries(const fips_lookup *l, const fips_node_addr *peer);
FIPS_CORE_API bool fips_lookup_heaviest_peer(const fips_lookup *l, fips_node_addr *out);
/* record_recent requires an absent ID; RANGE at storage capacity. */
FIPS_CORE_API fips_status fips_lookup_record_recent(fips_lookup *l, uint64_t id, fips_node_addr from,
    fips_node_addr target, uint64_t now_ms);
FIPS_CORE_API bool fips_lookup_evict_oldest_from(fips_lookup *l, const fips_node_addr *peer, fips_recent_request *out);
FIPS_CORE_API bool fips_lookup_was_evicted(const fips_lookup *l, uint64_t id);
FIPS_CORE_API void fips_lookup_purge_recent(fips_lookup *l, uint64_t now_ms, uint64_t expiry_ms);
FIPS_CORE_API size_t fips_lookup_reset_backoff(fips_lookup *l);
#ifdef __cplusplus
}
#endif
#endif
