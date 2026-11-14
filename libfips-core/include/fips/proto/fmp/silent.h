#ifndef FIPS_PROTO_FMP_SILENT_H
#define FIPS_PROTO_FMP_SILENT_H
#include <fips/proto/fmp/types.h>
#include <fips/proto/fmp/limits.h>
#ifdef __cplusplus
extern "C" {
#endif
#define FIPS_FMP_SILENT_SESSION_LIMIT 3
#define FIPS_FMP_SILENT_BACKOFF_BASE_MS UINT64_C(30000)
#define FIPS_FMP_SILENT_BACKOFF_CAP_MS UINT64_C(600000)
#define FIPS_FMP_COUNTED_SETUP_RECORD 8

typedef struct fips_fmp_active_backoff {
    fips_fmp_epoch epoch;
    uint32_t silent;
    uint64_t remaining_ms;
} fips_fmp_active_backoff;
typedef struct fips_fmp_refusal_start {
    fips_fmp_active_backoff backoff;
    uint32_t prior_refused;
} fips_fmp_refusal_start;
typedef struct fips_fmp_silent_record {
    fips_node_addr peer;
    fips_fmp_epoch epoch;
    uint32_t silent, refused;
    uint64_t last_end_ms, until_ms;
    bool has_until;
    fips_fmp_ephemeral_digest counted[FIPS_FMP_COUNTED_SETUP_RECORD];
    size_t counted_len;
} fips_fmp_silent_record;
/* Caller-owned sorted slots. No eviction of live records: full storage returns
 * RANGE, which the host must handle as a local admission/resource refusal, not
 * permission to promote. Expired records are reclaimed on ended(). Time is
 * milliseconds since origin (saturating u64), matching Rust's time arithmetic.
 * Required pointers, one clock domain; optional setup=NULL means own dial. */
typedef struct fips_fmp_silent_sessions {
    fips_fmp_silent_record *records;
    size_t len, capacity;
    fips_instant origin;
} fips_fmp_silent_sessions;
FIPS_CORE_API void fips_fmp_silent_sessions_init(fips_fmp_silent_sessions *s, fips_fmp_silent_record *storage, size_t capacity, fips_instant origin);
/* On success started says whether out was written; on capacity failure both
 * outputs are unchanged. Pruning expired records may still occur. */
FIPS_CORE_API fips_status fips_fmp_silent_sessions_ended(fips_fmp_silent_sessions *s, fips_node_addr peer, fips_fmp_epoch epoch, const fips_fmp_ephemeral_digest *setup, fips_instant now, bool *started, fips_fmp_refusal_start *out);
FIPS_CORE_API uint32_t fips_fmp_silent_sessions_heard(fips_fmp_silent_sessions *s, const fips_node_addr *peer);
FIPS_CORE_API bool fips_fmp_silent_sessions_note_refused(fips_fmp_silent_sessions *s, const fips_node_addr *peer, uint32_t *out);
FIPS_CORE_API bool fips_fmp_silent_sessions_refusing(const fips_fmp_silent_sessions *s, const fips_node_addr *peer, fips_instant now, fips_fmp_active_backoff *out);
FIPS_CORE_API bool fips_fmp_silent_sessions_count(const fips_fmp_silent_sessions *s, const fips_node_addr *peer, uint32_t *out);
#ifdef __cplusplus
}
#endif
#endif
