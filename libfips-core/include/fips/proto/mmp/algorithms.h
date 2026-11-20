#ifndef FIPS_PROTO_MMP_ALGORITHMS_H
#define FIPS_PROTO_MMP_ALGORITHMS_H
#include <fips/proto/codec.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
#include <fips/proto/mmp/limits.h>
/* Value fields are read-only to callers after init. No hidden allocation or IO.
 * RTT samples are nonnegative microseconds (wire-derived RTT fits 4294967295ms).
 * IEEE binary64, round-to-nearest, gradual underflow; no fast-math or FMA. */
typedef struct fips_mmp_jitter_estimator { int64_t jitter_q4; } fips_mmp_jitter_estimator;
FIPS_CORE_API void fips_mmp_jitter_update(fips_mmp_jitter_estimator *s, int32_t transit_delta);
FIPS_CORE_API uint32_t fips_mmp_jitter_us(const fips_mmp_jitter_estimator *s);
typedef struct fips_mmp_srtt_estimator { int64_t srtt_us, rttvar_us; bool initialized; } fips_mmp_srtt_estimator;
FIPS_CORE_API void fips_mmp_srtt_update(fips_mmp_srtt_estimator *s, int64_t rtt_us);
typedef struct fips_mmp_dual_ewma { double short_avg, long_avg; bool initialized; } fips_mmp_dual_ewma;
FIPS_CORE_API void fips_mmp_dual_ewma_update(fips_mmp_dual_ewma *s, double sample);
typedef struct fips_mmp_owd_sample { uint32_t at_ms; int64_t owd_us; } fips_mmp_owd_sample;
typedef struct fips_mmp_owd_trend_detector {
    fips_mmp_owd_sample *samples;
    size_t capacity, len, head;
} fips_mmp_owd_trend_detector;
/* Clamp requested window to [2,4096]. Storage must have at least the clamped
 * capacity and outlive the state. RANGE leaves out/storage unchanged. */
FIPS_CORE_API fips_status fips_mmp_owd_init(fips_mmp_owd_trend_detector *out, size_t window, fips_mmp_owd_sample *storage, size_t capacity);
FIPS_CORE_API void fips_mmp_owd_clear(fips_mmp_owd_trend_detector *s);
FIPS_CORE_API void fips_mmp_owd_push(fips_mmp_owd_trend_detector *s, uint32_t at_ms, int64_t owd_us);
FIPS_CORE_API int32_t fips_mmp_owd_trend_us_per_sec(const fips_mmp_owd_trend_detector *s);
FIPS_CORE_API double fips_mmp_compute_etx(double forward, double reverse);
FIPS_CORE_API double fips_mmp_quality_index(double etx, double rtt_ms);
typedef struct fips_mmp_spin_bit_state {
    bool is_initiator, current_value;
    uint64_t highest_counter_for_spin, last_edge_ms;
    bool has_last_edge;
} fips_mmp_spin_bit_state;
FIPS_CORE_API void fips_mmp_spin_init(fips_mmp_spin_bit_state *s, bool is_initiator);
FIPS_CORE_API void fips_mmp_spin_restart(fips_mmp_spin_bit_state *s);
/* false = None; rtt_ms unchanged. Counter guard applies only to responder. */
FIPS_CORE_API bool fips_mmp_spin_rx_observe(fips_mmp_spin_bit_state *s, bool received_bit, uint64_t counter, uint64_t now_ms, uint64_t *rtt_ms);

#ifdef __cplusplus
}
#endif
#endif
