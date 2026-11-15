#ifndef FIPS_PROTO_MMP_RECEIVER_H
#define FIPS_PROTO_MMP_RECEIVER_H
#include <fips/proto/codec.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
#include <fips/proto/mmp/wire.h>
#include <fips/proto/mmp/algorithms.h>
typedef struct fips_mmp_gap_tracker {
    uint64_t expected_next;
    bool has_expected, in_burst;
    uint16_t current_burst_len;
    uint32_t burst_count;
    uint16_t max_burst_len;
    uint64_t total_burst_len;
} fips_mmp_gap_tracker;
typedef struct fips_mmp_receiver_state {
    uint64_t cumulative_packets_recv, cumulative_bytes_recv, cumulative_reorder_count, highest_counter;
    uint32_t interval_packets_recv, interval_bytes_recv;
    fips_mmp_jitter_estimator jitter;
    fips_mmp_owd_trend_detector owd_trend;
    bool has_owd_ref;
    uint64_t owd_ref_recv_ms;
    uint32_t owd_ref_timestamp;
    fips_mmp_gap_tracker gap_tracker;
    uint32_t ecn_ce_count, last_sender_timestamp;
    uint64_t last_recv_ms, rekey_jitter_grace_until_ms, last_report_ms, report_interval_ms;
    bool has_last_recv, has_rekey_grace, has_last_report, interval_has_data;
    uint32_t srtt_sample_count;
} fips_mmp_receiver_state;
/* Same window/storage contract as owd_init; failure preserves out and storage. */
FIPS_CORE_API fips_status fips_mmp_receiver_init(fips_mmp_receiver_state *s, size_t window, fips_mmp_owd_sample *storage, size_t capacity);
FIPS_CORE_API fips_status fips_mmp_receiver_init_with_cold_start(fips_mmp_receiver_state *s, size_t window, fips_mmp_owd_sample *storage, size_t capacity, uint64_t cold_start_ms);
FIPS_CORE_API void fips_mmp_receiver_reset_for_rekey(fips_mmp_receiver_state *s, uint64_t now_ms);
FIPS_CORE_API void fips_mmp_receiver_record_recv(fips_mmp_receiver_state *s, uint64_t counter, uint32_t sender_timestamp_ms, size_t bytes, bool ce, uint64_t now_ms);
/* Count every frame. Only sampled frames feed echo, jitter and OWD. */
FIPS_CORE_API void fips_mmp_receiver_record_frame(fips_mmp_receiver_state *s, uint64_t counter, uint32_t sender_timestamp_ms, size_t bytes, bool ce, uint64_t now_ms, bool sampled);
FIPS_CORE_API bool fips_mmp_receiver_build_report(fips_mmp_receiver_state *s, uint64_t now_ms, fips_mmp_receiver_report *out);
FIPS_CORE_API bool fips_mmp_receiver_should_send_report(const fips_mmp_receiver_state *s, uint64_t now_ms);
FIPS_CORE_API void fips_mmp_receiver_update_report_interval_from_srtt(fips_mmp_receiver_state *s, int64_t srtt_us);
FIPS_CORE_API void fips_mmp_receiver_update_report_interval_with_bounds(fips_mmp_receiver_state *s, int64_t srtt_us, uint64_t min_ms, uint64_t max_ms);

#ifdef __cplusplus
}
#endif
#endif
