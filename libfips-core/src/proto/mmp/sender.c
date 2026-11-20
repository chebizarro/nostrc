#include <fips/proto/mmp/sender.h>
#include "internal.h"
void fips_mmp_sender_init_with_cold_start(fips_mmp_sender_state *s, uint64_t ms)
{ *s = (fips_mmp_sender_state){.report_interval_ms = ms}; }
void fips_mmp_sender_init(fips_mmp_sender_state *s)
{ fips_mmp_sender_init_with_cold_start(s, FIPS_MMP_DEFAULT_COLD_START_INTERVAL_MS); }
void fips_mmp_sender_record_sent(fips_mmp_sender_state *s, uint64_t counter, uint32_t timestamp, size_t bytes)
{
    if (!s->interval_has_data) {
        s->interval_start_counter = counter; s->interval_start_timestamp = timestamp; s->interval_has_data = true;
    }
    s->last_counter = counter; s->last_timestamp = timestamp;
    s->interval_bytes_sent = mmp_add32(s->interval_bytes_sent, (uint32_t)bytes);
    ++s->cumulative_packets_sent; s->cumulative_bytes_sent += (uint64_t)bytes;
}
bool fips_mmp_sender_build_report(fips_mmp_sender_state *s, uint64_t now, fips_mmp_sender_report *out)
{
    if (!s->interval_has_data) return false;
    *out = (fips_mmp_sender_report){s->interval_start_counter, s->last_counter,
        s->interval_start_timestamp, s->last_timestamp, s->interval_bytes_sent,
        s->cumulative_packets_sent, s->cumulative_bytes_sent};
    s->interval_has_data = false; s->interval_bytes_sent = 0; s->last_report_ms = now; s->has_last_report = true;
    return true;
}
double fips_mmp_sender_send_failure_backoff_multiplier(const fips_mmp_sender_state *s)
{ return (double)(UINT64_C(1) << (s->consecutive_send_failures < 5 ? s->consecutive_send_failures : 5)); }
bool fips_mmp_sender_should_send_report(const fips_mmp_sender_state *s, uint64_t now)
{
    if (!s->interval_has_data) return false;
    if (!s->has_last_report) return true;
    uint64_t effective = mmp_u64((double)s->report_interval_ms * fips_mmp_sender_send_failure_backoff_multiplier(s));
    return mmp_sub(now, s->last_report_ms) >= effective;
}
uint32_t fips_mmp_sender_record_send_failure(fips_mmp_sender_state *s) { return ++s->consecutive_send_failures; }
uint32_t fips_mmp_sender_record_send_success(fips_mmp_sender_state *s)
{ uint32_t prev = s->consecutive_send_failures; s->consecutive_send_failures = 0; return prev; }
void fips_mmp_sender_update_report_interval_with_bounds(fips_mmp_sender_state *s, int64_t srtt, uint64_t min, uint64_t max)
{ if (srtt > 0) s->report_interval_ms = mmp_clamp(((uint64_t)srtt * 2)/1000, min, max); }
void fips_mmp_sender_update_report_interval_from_srtt(fips_mmp_sender_state *s, int64_t srtt)
{
    s->srtt_sample_count = mmp_add32(s->srtt_sample_count, 1);
    uint64_t floor = s->srtt_sample_count <= FIPS_MMP_COLD_START_SAMPLES ? FIPS_MMP_DEFAULT_COLD_START_INTERVAL_MS : FIPS_MMP_MIN_REPORT_INTERVAL_MS;
    fips_mmp_sender_update_report_interval_with_bounds(s, srtt, floor, FIPS_MMP_MAX_REPORT_INTERVAL_MS);
}
