#ifndef FIPS_PROTO_LOOKUP_LIMITS_H
#define FIPS_PROTO_LOOKUP_LIMITS_H
#include <fips/proto/coord.h>
#ifdef __cplusplus
extern "C" {
#endif
#define FIPS_MAX_RECENT_LOOKUP_REQUESTS 4096u
#define FIPS_MAX_BACKOFF_ENTRIES 4096u
#define FIPS_MAX_FORWARD_ENTRIES 4096u
#define FIPS_LOOKUP_FORWARD_MIN_INTERVAL_MS UINT64_C(2000)
#define FIPS_LOOKUP_FORWARD_SWEEP_INTERVAL_MS UINT64_C(7500)
typedef struct fips_lookup_backoff_entry {
    fips_node_addr target;
    uint64_t suppress_until_ms;
    uint32_t failures;
} fips_lookup_backoff_entry;
typedef struct fips_lookup_backoff {
    fips_lookup_backoff_entry *entries;
    size_t len, capacity;
    uint64_t base_ms, max_ms, evicted;
} fips_lookup_backoff;
typedef struct fips_lookup_forward_entry {
    fips_node_addr from, target;
    uint64_t at_ms;
} fips_lookup_forward_entry;
typedef struct fips_lookup_forward_rate_limiter {
    fips_lookup_forward_entry *entries; /* insertion order, no stale queue */
    size_t len, capacity;
    uint64_t min_interval_ms, last_sweep_ms, evicted;
    bool has_sweep;
} fips_lookup_forward_rate_limiter;
/* Caller-owned storage retained for lifetime. Caps may be smaller than Rust's
 * defaults; nonzero capacity <= corresponding MAX is required. No allocation.
 * Scan-based ordered tables replace Rust maps/indexes, eliminating stale index
 * storage. Full tables evict rather than fail open; ties use address order. */
FIPS_CORE_API fips_status fips_lookup_backoff_init(fips_lookup_backoff *b, fips_lookup_backoff_entry *entries,
    size_t capacity, uint64_t base_secs, uint64_t max_secs);
FIPS_CORE_API bool fips_lookup_backoff_is_enabled(const fips_lookup_backoff *b);
FIPS_CORE_API bool fips_lookup_backoff_is_suppressed(const fips_lookup_backoff *b, const fips_node_addr *target, uint64_t now_ms);
FIPS_CORE_API uint32_t fips_lookup_backoff_failure_count(const fips_lookup_backoff *b, const fips_node_addr *target);
FIPS_CORE_API void fips_lookup_backoff_record_failure(fips_lookup_backoff *b, const fips_node_addr *target, uint64_t now_ms);
FIPS_CORE_API void fips_lookup_backoff_record_success(fips_lookup_backoff *b, const fips_node_addr *target);
FIPS_CORE_API void fips_lookup_backoff_prune(fips_lookup_backoff *b, uint64_t now_ms);
FIPS_CORE_API void fips_lookup_backoff_reset_all(fips_lookup_backoff *b);
FIPS_CORE_API uint64_t fips_lookup_backoff_take_evicted(fips_lookup_backoff *b);
FIPS_CORE_API fips_status fips_lookup_forward_rate_limiter_init(fips_lookup_forward_rate_limiter *l,
    fips_lookup_forward_entry *entries, size_t capacity, uint64_t min_interval_ms);
FIPS_CORE_API bool fips_lookup_forward_should_forward(fips_lookup_forward_rate_limiter *l,
    const fips_node_addr *from, const fips_node_addr *target, uint64_t now_ms);
FIPS_CORE_API void fips_lookup_forward_prune(fips_lookup_forward_rate_limiter *l, uint64_t now_ms);
FIPS_CORE_API uint64_t fips_lookup_forward_take_evicted(fips_lookup_forward_rate_limiter *l);
#ifdef __cplusplus
}
#endif
#endif
