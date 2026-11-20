#ifndef FIPS_NODE_RATE_LIMIT_H
#define FIPS_NODE_RATE_LIMIT_H
#include <fips/host.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
#define FIPS_DEFAULT_BURST_CAPACITY 100
#define FIPS_DEFAULT_REFILL_RATE 10.0
#define FIPS_ESTABLISHED_RATE_FLOOR 1.0
#define FIPS_SETUP_BUCKET_IDLE_SECS 300
#define FIPS_SIGN_MAX_AGE_SECS 300
#define FIPS_DEFAULT_SIGN_BURST 256.0
#define FIPS_DEFAULT_SIGN_RATE 32.0

typedef struct fips_bucket_params { uint32_t burst; double rate; } fips_bucket_params;
typedef struct fips_token_bucket {
    uint32_t capacity;
    double tokens, refill_rate;
    fips_instant last_refill;
} fips_token_bucket;
/** Rates must be finite and nonnegative. All time inputs share one domain.
 * All storage is caller-owned; no API reads a clock or allocates. */
FIPS_CORE_API void fips_token_bucket_init(fips_token_bucket *b, fips_bucket_params params,
                                         fips_instant now);
FIPS_CORE_API bool fips_token_bucket_acquire(fips_token_bucket *b, uint32_t n,
                                            fips_instant now);
FIPS_CORE_API double fips_token_bucket_tokens(fips_token_bucket *b, fips_instant now);
FIPS_CORE_API void fips_token_bucket_reset(fips_token_bucket *b, fips_instant now);
/** FIPS_ERR_RANGE for an infinite/unrepresentable wait, out unchanged. */
FIPS_CORE_API fips_status fips_token_bucket_wait(fips_token_bucket *b, fips_instant now,
                                               fips_duration *out);
FIPS_CORE_API fips_bucket_params fips_derive_established_bucket(size_t max_peers,
    uint64_t rekey_after_secs, uint32_t max_resends, fips_bucket_params stranger);
typedef enum fips_msg1_class { FIPS_MSG1_STRANGER, FIPS_MSG1_ESTABLISHED } fips_msg1_class;
typedef enum fips_msg1_result {
    FIPS_MSG1_ADMIT, FIPS_MSG1_PENDING_LIMIT, FIPS_MSG1_RATE_LIMIT
} fips_msg1_result;
FIPS_CORE_API const char *fips_msg1_result_name(fips_msg1_result result);
typedef struct fips_handshake_rate_limiter {
    fips_token_bucket stranger, established;
    size_t pending, max_pending;
} fips_handshake_rate_limiter;
/** C equivalent of Rust's pending guard. Initialize to {0}, do not copy or
 * overwrite a live slot. The limiter must outlive its slots and stay at a
 * stable address. Release on EVERY handler exit; release is idempotent for
 * this object. Caller supplies synchronization (no atomics in the core). */
typedef struct fips_pending_handshake { fips_handshake_rate_limiter *owner; } fips_pending_handshake;
FIPS_CORE_API void fips_handshake_init(fips_handshake_rate_limiter *l,
    fips_token_bucket stranger, fips_token_bucket established, size_t max_pending);
FIPS_CORE_API bool fips_handshake_can_start(fips_handshake_rate_limiter *l,
    fips_msg1_class class_, fips_instant now);
/** Refusal leaves slot unchanged; pending refusal never spends a token. */
FIPS_CORE_API fips_msg1_result fips_handshake_start(fips_handshake_rate_limiter *l,
    fips_msg1_class class_, fips_instant now, fips_pending_handshake *slot);
FIPS_CORE_API void fips_pending_handshake_release(fips_pending_handshake *slot);
/** Test/reset semantics mirror upstream: zero pending; live slots subsequently
 * release with saturation, even if new slots have been acquired meanwhile. */
FIPS_CORE_API void fips_handshake_reset(fips_handshake_rate_limiter *l, fips_instant now);

typedef struct fips_setup_entry {
    uint8_t addr[16];
    fips_token_bucket stranger, established;
    fips_instant seen;
} fips_setup_entry;
typedef struct fips_session_setup_limiter {
    fips_setup_entry *entries;
    size_t len, capacity;
    fips_bucket_params stranger, established;
} fips_session_setup_limiter;
typedef struct fips_sign_entry {
    uint8_t addr[16];
    double tokens;
    fips_instant updated;
} fips_sign_entry;
typedef struct fips_lookup_sign_limiter {
    fips_sign_entry *entries;
    size_t len, capacity;
    double burst, rate;
} fips_lookup_sign_limiter;
/** Sorted per-authenticated-link maps borrow storage for their whole lifetime.
 * Rust bounds these maps by the host peer table, not a fixed entry constant.
 * Supply that bound here; no growth or unrelated-peer eviction. New keys at
 * capacity return FIPS_ERR_RANGE without mutation (including out). Normal
 * cleanup occurs only after admission, exactly at age >= 300 seconds.
 * Explicit cleanup can reclaim idle entries before retrying a capacity refusal.
 * entries may be NULL iff capacity == 0. Sign burst must be finite;
 * zero/negative sign burst is unlimited. */
FIPS_CORE_API void fips_session_setup_init(fips_session_setup_limiter *l,
    fips_setup_entry *entries, size_t capacity, fips_bucket_params stranger,
    fips_bucket_params established);
FIPS_CORE_API fips_status fips_session_setup_admit(fips_session_setup_limiter *l,
    const uint8_t link_peer[16], fips_msg1_class class_, fips_instant now, bool *out);
FIPS_CORE_API void fips_session_setup_cleanup(fips_session_setup_limiter *l, fips_instant now);
FIPS_CORE_API void fips_lookup_sign_init(fips_lookup_sign_limiter *l,
    fips_sign_entry *entries, size_t capacity, double burst, double rate);
FIPS_CORE_API fips_status fips_lookup_should_sign(fips_lookup_sign_limiter *l,
    const uint8_t link_peer[16], fips_instant now, bool *out);
FIPS_CORE_API void fips_lookup_sign_cleanup(fips_lookup_sign_limiter *l, fips_instant now);
#ifdef __cplusplus
}
#endif
#endif
