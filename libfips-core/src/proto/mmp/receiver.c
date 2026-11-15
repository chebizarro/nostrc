#include <fips/proto/mmp/receiver.h>
#include "internal.h"
static void finish_burst(fips_mmp_gap_tracker *g)
{
    if (!g->in_burst) return;
    if (g->current_burst_len > g->max_burst_len) g->max_burst_len = g->current_burst_len;
    g->total_burst_len += g->current_burst_len; g->in_burst = false; g->current_burst_len = 0;
}
static void observe(fips_mmp_gap_tracker *g, uint64_t counter)
{
    if (!g->has_expected) { g->has_expected = true; g->expected_next = mmp_add(counter, 1); return; }
    if (counter > g->expected_next) {
        uint16_t gap = (uint16_t)(counter - g->expected_next); /* upstream narrows before saturation */
        if (g->in_burst) {
            uint32_t sum = (uint32_t)g->current_burst_len + gap;
            g->current_burst_len = sum > UINT16_MAX ? UINT16_MAX : (uint16_t)sum;
        } else { g->in_burst = true; g->current_burst_len = gap; ++g->burst_count; }
    } else finish_burst(g);
    if (counter >= g->expected_next) g->expected_next = mmp_add(counter, 1);
}
fips_status fips_mmp_receiver_init_with_cold_start(fips_mmp_receiver_state *s, size_t window, fips_mmp_owd_sample *storage, size_t capacity, uint64_t ms)
{
    fips_mmp_receiver_state v = {.report_interval_ms=ms};
    fips_status status = fips_mmp_owd_init(&v.owd_trend, window, storage, capacity);
    if (status != FIPS_OK) return status;
    *s = v; return FIPS_OK;
}
fips_status fips_mmp_receiver_init(fips_mmp_receiver_state *s, size_t window, fips_mmp_owd_sample *storage, size_t capacity)
{ return fips_mmp_receiver_init_with_cold_start(s, window, storage, capacity, FIPS_MMP_DEFAULT_COLD_START_INTERVAL_MS); }
void fips_mmp_receiver_reset_for_rekey(fips_mmp_receiver_state *s, uint64_t now)
{
    s->highest_counter = 0; s->cumulative_reorder_count = 0;
    s->gap_tracker = (fips_mmp_gap_tracker){0}; s->interval_packets_recv = 0; s->interval_bytes_recv = 0;
    s->jitter = (fips_mmp_jitter_estimator){0}; fips_mmp_owd_clear(&s->owd_trend); s->has_owd_ref = false;
    s->owd_ref_recv_ms = 0; s->owd_ref_timestamp = 0;
    s->last_sender_timestamp = 0; s->has_last_recv = false; s->last_recv_ms = 0;
    s->has_rekey_grace = true; s->rekey_jitter_grace_until_ms = mmp_add(now, 15000);
    s->ecn_ce_count = 0; s->interval_has_data = false;
}
static void sample(fips_mmp_receiver_state *s, uint32_t ts, uint64_t now)
{
    bool grace = s->has_rekey_grace && now < s->rekey_jitter_grace_until_ms;
    if (!grace) {
        s->has_rekey_grace = false;
        if (s->has_last_recv) {
            /* Low 32 bits implement Rust's final as-i32 truncation without
             * signed intermediate overflow, even for hostile time inputs. */
            uint32_t recv_delta = (uint32_t)mmp_sub(now, s->last_recv_ms) * UINT32_C(1000);
            uint32_t send_delta = (ts - s->last_sender_timestamp) * UINT32_C(1000);
            fips_mmp_jitter_update(&s->jitter, mmp_i32(recv_delta-send_delta));
        }
        if (!s->has_owd_ref || mmp_sub(now, s->owd_ref_recv_ms) >= (UINT64_C(1)<<30)) {
            if (s->has_owd_ref) fips_mmp_owd_clear(&s->owd_trend);
            s->has_owd_ref = true; s->owd_ref_recv_ms = now; s->owd_ref_timestamp = ts;
        }
        uint64_t recv_ms = mmp_sub(now, s->owd_ref_recv_ms);
        int64_t sent_ms = mmp_i32(ts-s->owd_ref_timestamp);
        int64_t owd_us = ((int64_t)recv_ms-sent_ms)*1000;
        fips_mmp_owd_push(&s->owd_trend, (uint32_t)recv_ms, owd_us);
    }
    s->last_sender_timestamp = ts; s->last_recv_ms = now; s->has_last_recv = true;
}
void fips_mmp_receiver_record_frame(fips_mmp_receiver_state *s, uint64_t counter, uint32_t ts, size_t bytes, bool ce, uint64_t now, bool sampled)
{
    s->interval_has_data = true; ++s->cumulative_packets_recv; s->cumulative_bytes_recv += (uint64_t)bytes;
    s->interval_packets_recv = mmp_add32(s->interval_packets_recv, 1);
    s->interval_bytes_recv = mmp_add32(s->interval_bytes_recv, (uint32_t)bytes);
    if (counter < s->highest_counter) ++s->cumulative_reorder_count;
    else s->highest_counter = counter;
    observe(&s->gap_tracker, counter);
    if (ce) s->ecn_ce_count = mmp_add32(s->ecn_ce_count, 1);
    if (sampled) sample(s, ts, now);
}
void fips_mmp_receiver_record_recv(fips_mmp_receiver_state *s, uint64_t counter, uint32_t ts, size_t bytes, bool ce, uint64_t now)
{ fips_mmp_receiver_record_frame(s, counter, ts, bytes, ce, now, true); }
bool fips_mmp_receiver_build_report(fips_mmp_receiver_state *s, uint64_t now, fips_mmp_receiver_report *out)
{
    if (!s->interval_has_data) return false;
    uint64_t dwell = s->has_last_recv ? mmp_sub(now, s->last_recv_ms) : 0;
    finish_burst(&s->gap_tracker);
    fips_mmp_gap_tracker *g = &s->gap_tracker;
    double mean = g->burst_count ? (double)g->total_burst_len/(double)g->burst_count : 0.0;
    *out = (fips_mmp_receiver_report){
        .highest_counter=s->highest_counter, .cumulative_packets_recv=s->cumulative_packets_recv,
        .cumulative_bytes_recv=s->cumulative_bytes_recv,
        .timestamp_echo=dwell > UINT16_MAX ? 0 : s->last_sender_timestamp,
        .dwell_time=dwell > UINT16_MAX ? UINT16_MAX : (uint16_t)dwell,
        .max_burst_loss=g->max_burst_len, .mean_burst_loss=mmp_u16(mean*256.0),
        .jitter=fips_mmp_jitter_us(&s->jitter), .ecn_ce_count=s->ecn_ce_count,
        .owd_trend=fips_mmp_owd_trend_us_per_sec(&s->owd_trend), .burst_loss_count=g->burst_count,
        .cumulative_reorder_count=s->cumulative_reorder_count > UINT32_MAX ? UINT32_MAX : (uint32_t)s->cumulative_reorder_count,
        .interval_packets_recv=s->interval_packets_recv, .interval_bytes_recv=s->interval_bytes_recv};
    g->burst_count = 0; g->max_burst_len = 0; g->total_burst_len = 0;
    s->interval_packets_recv = 0; s->interval_bytes_recv = 0; s->interval_has_data = false;
    s->last_report_ms = now; s->has_last_report = true; return true;
}
bool fips_mmp_receiver_should_send_report(const fips_mmp_receiver_state *s, uint64_t now)
{ return s->interval_has_data && (!s->has_last_report || mmp_sub(now, s->last_report_ms) >= s->report_interval_ms); }
void fips_mmp_receiver_update_report_interval_with_bounds(fips_mmp_receiver_state *s, int64_t srtt, uint64_t min, uint64_t max)
{ if (srtt > 0) s->report_interval_ms = mmp_clamp((uint64_t)srtt/1000, min, max); }
void fips_mmp_receiver_update_report_interval_from_srtt(fips_mmp_receiver_state *s, int64_t srtt)
{
    s->srtt_sample_count = mmp_add32(s->srtt_sample_count, 1);
    uint64_t floor = s->srtt_sample_count <= FIPS_MMP_COLD_START_SAMPLES ? FIPS_MMP_DEFAULT_COLD_START_INTERVAL_MS : FIPS_MMP_MIN_REPORT_INTERVAL_MS;
    fips_mmp_receiver_update_report_interval_with_bounds(s, srtt, floor, FIPS_MMP_MAX_REPORT_INTERVAL_MS);
}
