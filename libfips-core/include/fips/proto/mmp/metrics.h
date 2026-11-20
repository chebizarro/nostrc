#ifndef FIPS_PROTO_MMP_METRICS_H
#define FIPS_PROTO_MMP_METRICS_H
#include <fips/proto/codec.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
#include <fips/proto/mmp/algorithms.h>
#include <fips/proto/mmp/wire.h>
typedef enum fips_mmp_rr_log_kind { FIPS_MMP_RrNone, FIPS_MMP_RrStale, FIPS_MMP_RrRttSample, FIPS_MMP_RrInvalidRtt } fips_mmp_rr_log_kind;
typedef struct fips_mmp_rr_log {
    fips_mmp_rr_log_kind kind;
    uint64_t prev_highest, prev_packets, prev_bytes;
    uint32_t rtt_ms;
    double srtt_ms; /* before this sample */
} fips_mmp_rr_log;
typedef struct fips_mmp_metrics {
    fips_mmp_srtt_estimator srtt;
    fips_mmp_dual_ewma rtt_trend, loss_trend, goodput_trend, jitter_trend, etx_trend;
    double delivery_ratio_forward, delivery_ratio_reverse, etx, goodput_bps;
    uint64_t prev_rr_cum_packets, prev_rr_cum_bytes, prev_rr_highest_counter;
    uint32_t prev_rr_ecn_ce, prev_rr_reorder;
    uint64_t prev_rr_ms;
    bool has_prev_rr;
    uint64_t prev_reverse_packets, prev_reverse_highest;
    bool has_prev_reverse;
    uint64_t reports_seen, rtt_samples, rtt_zero, rtt_arith_fail;
    uint32_t last_rtt_ms;
    bool has_last_rtt;
} fips_mmp_metrics;
FIPS_CORE_API void fips_mmp_metrics_init(fips_mmp_metrics *s);
FIPS_CORE_API void fips_mmp_metrics_reset_for_rekey(fips_mmp_metrics *s);
/* Returns first SRTT transition. log is always filled. Stale reports count as
 * seen but change neither baselines nor trends. No logging occurs in core. */
FIPS_CORE_API bool fips_mmp_metrics_process_receiver_report(fips_mmp_metrics *s, const fips_mmp_receiver_report *rr, uint32_t our_timestamp_ms, uint64_t now_ms, fips_mmp_rr_log *log);
FIPS_CORE_API void fips_mmp_metrics_update_reverse_delivery(fips_mmp_metrics *s, uint64_t packets, uint64_t highest);
FIPS_CORE_API bool fips_mmp_metrics_srtt_ms(const fips_mmp_metrics *s, double *out);
FIPS_CORE_API double fips_mmp_metrics_loss_rate(const fips_mmp_metrics *s);
FIPS_CORE_API bool fips_mmp_metrics_smoothed_loss(const fips_mmp_metrics *s, double *out);
FIPS_CORE_API bool fips_mmp_metrics_smoothed_etx(const fips_mmp_metrics *s, double *out);
FIPS_CORE_API bool fips_mmp_metrics_quality_index(const fips_mmp_metrics *s, double *out);
FIPS_CORE_API bool fips_mmp_metrics_rr_counters(const fips_mmp_metrics *s, uint64_t *highest, uint64_t *packets, uint32_t *reordered);

#ifdef __cplusplus
}
#endif
#endif
