#ifndef FIPS_PROTO_MMP_SENDER_H
#define FIPS_PROTO_MMP_SENDER_H
#include <fips/proto/codec.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
#include <fips/proto/mmp/wire.h>
#include <fips/proto/mmp/limits.h>
typedef struct fips_mmp_sender_state {
    uint64_t cumulative_packets_sent, cumulative_bytes_sent;
    uint64_t interval_start_counter;
    uint32_t interval_start_timestamp, interval_bytes_sent;
    uint64_t last_counter;
    uint32_t last_timestamp;
    bool interval_has_data, has_last_report;
    uint64_t last_report_ms, report_interval_ms;
    uint32_t consecutive_send_failures, srtt_sample_count;
} fips_mmp_sender_state;
FIPS_CORE_API void fips_mmp_sender_init(fips_mmp_sender_state *s);
FIPS_CORE_API void fips_mmp_sender_init_with_cold_start(fips_mmp_sender_state *s, uint64_t cold_start_ms);
FIPS_CORE_API void fips_mmp_sender_record_sent(fips_mmp_sender_state *s, uint64_t counter, uint32_t timestamp, size_t bytes);
/* false = None, preserves out. Build advances interval; drive only when gated. */
FIPS_CORE_API bool fips_mmp_sender_build_report(fips_mmp_sender_state *s, uint64_t now_ms, fips_mmp_sender_report *out);
FIPS_CORE_API bool fips_mmp_sender_should_send_report(const fips_mmp_sender_state *s, uint64_t now_ms);
FIPS_CORE_API uint32_t fips_mmp_sender_record_send_failure(fips_mmp_sender_state *s);
FIPS_CORE_API uint32_t fips_mmp_sender_record_send_success(fips_mmp_sender_state *s);
FIPS_CORE_API double fips_mmp_sender_send_failure_backoff_multiplier(const fips_mmp_sender_state *s);
FIPS_CORE_API void fips_mmp_sender_update_report_interval_from_srtt(fips_mmp_sender_state *s, int64_t srtt_us);
/* bounds require min <= max; positive SRTT. Integer multiply uses u64 to avoid
 * signed UB at out-of-wire-domain extremes. */
FIPS_CORE_API void fips_mmp_sender_update_report_interval_with_bounds(fips_mmp_sender_state *s, int64_t srtt_us, uint64_t min_ms, uint64_t max_ms);

#ifdef __cplusplus
}
#endif
#endif
