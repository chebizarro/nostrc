#include <fips/proto/mmp/metrics.h>
#include "internal.h"
void fips_mmp_metrics_init(fips_mmp_metrics *s)
{ *s = (fips_mmp_metrics){.delivery_ratio_forward=1.0, .delivery_ratio_reverse=1.0, .etx=1.0}; }
void fips_mmp_metrics_reset_for_rekey(fips_mmp_metrics *s)
{
    s->prev_rr_cum_packets=0; s->prev_rr_cum_bytes=0; s->prev_rr_highest_counter=0;
    s->prev_rr_ecn_ce=0; s->prev_rr_reorder=0; s->prev_rr_ms=0; s->has_prev_rr=false;
    s->delivery_ratio_forward=1.0; s->prev_reverse_packets=0; s->prev_reverse_highest=0; s->has_prev_reverse=false;
}
bool fips_mmp_metrics_process_receiver_report(fips_mmp_metrics *s, const fips_mmp_receiver_report *rr, uint32_t timestamp, uint64_t now, fips_mmp_rr_log *log)
{
    bool had_srtt=s->srtt.initialized;
    ++s->reports_seen;
    *log=(fips_mmp_rr_log){0};
    if (s->has_prev_rr) {
        bool regressed=rr->highest_counter<s->prev_rr_highest_counter || rr->cumulative_packets_recv<s->prev_rr_cum_packets ||
            rr->cumulative_bytes_recv<s->prev_rr_cum_bytes || rr->ecn_ce_count<s->prev_rr_ecn_ce || rr->cumulative_reorder_count<s->prev_rr_reorder;
        bool duplicate=rr->highest_counter==s->prev_rr_highest_counter && rr->cumulative_packets_recv==s->prev_rr_cum_packets &&
            rr->cumulative_bytes_recv==s->prev_rr_cum_bytes && rr->ecn_ce_count==s->prev_rr_ecn_ce && rr->cumulative_reorder_count==s->prev_rr_reorder;
        if (regressed || duplicate) {
            *log=(fips_mmp_rr_log){.kind=FIPS_MMP_RrStale, .prev_highest=s->prev_rr_highest_counter,
                .prev_packets=s->prev_rr_cum_packets, .prev_bytes=s->prev_rr_cum_bytes}; return false;
        }
    }
    if (rr->timestamp_echo) {
        uint64_t done=(uint64_t)rr->timestamp_echo+rr->dwell_time;
        if (done>UINT32_MAX || done>timestamp) { ++s->rtt_arith_fail; log->kind=FIPS_MMP_RrInvalidRtt; }
        else {
            uint32_t rtt=timestamp-(uint32_t)done;
            if (!rtt) { ++s->rtt_zero; log->kind=FIPS_MMP_RrInvalidRtt; }
            else {
                int64_t us=(int64_t)rtt*1000;
                *log=(fips_mmp_rr_log){.kind=FIPS_MMP_RrRttSample, .rtt_ms=rtt, .srtt_ms=(double)s->srtt.srtt_us/1000.0};
                ++s->rtt_samples; s->last_rtt_ms=rtt; s->has_last_rtt=true;
                fips_mmp_srtt_update(&s->srtt, us); fips_mmp_dual_ewma_update(&s->rtt_trend, (double)us);
            }
        }
    }
    if (s->has_prev_rr) {
        uint64_t span=mmp_sub(rr->highest_counter, s->prev_rr_highest_counter);
        uint64_t packets=mmp_sub(rr->cumulative_packets_recv, s->prev_rr_cum_packets);
        if (span) {
            s->delivery_ratio_forward=mmp_clampd((double)packets/(double)span, 0.0, 1.0);
            fips_mmp_dual_ewma_update(&s->loss_trend, 1.0-s->delivery_ratio_forward);
            s->etx=fips_mmp_compute_etx(s->delivery_ratio_forward, s->delivery_ratio_reverse);
            fips_mmp_dual_ewma_update(&s->etx_trend, s->etx);
        }
        uint64_t bytes=mmp_sub(rr->cumulative_bytes_recv, s->prev_rr_cum_bytes);
        fips_mmp_dual_ewma_update(&s->goodput_trend, (double)bytes);
        double secs=(double)mmp_sub(now, s->prev_rr_ms)/1000.0;
        if (secs>0.0) {
            double bps=(double)bytes/secs;
            if (s->goodput_bps==0.0) s->goodput_bps=bps;
            else s->goodput_bps+=(bps-s->goodput_bps)*0.25;
        }
    }
    fips_mmp_dual_ewma_update(&s->jitter_trend, (double)rr->jitter);
    s->prev_rr_cum_packets=rr->cumulative_packets_recv; s->prev_rr_cum_bytes=rr->cumulative_bytes_recv;
    s->prev_rr_highest_counter=rr->highest_counter; s->prev_rr_ecn_ce=rr->ecn_ce_count;
    s->prev_rr_reorder=rr->cumulative_reorder_count; s->prev_rr_ms=now; s->has_prev_rr=true;
    return !had_srtt && s->srtt.initialized;
}
void fips_mmp_metrics_update_reverse_delivery(fips_mmp_metrics *s, uint64_t packets, uint64_t highest)
{
    if (s->has_prev_reverse) {
        uint64_t span=mmp_sub(highest, s->prev_reverse_highest), delta=mmp_sub(packets, s->prev_reverse_packets);
        if (span) {
            s->delivery_ratio_reverse=mmp_clampd((double)delta/(double)span, 0.0, 1.0);
            s->etx=fips_mmp_compute_etx(s->delivery_ratio_forward, s->delivery_ratio_reverse);
        }
    }
    s->prev_reverse_packets=packets; s->prev_reverse_highest=highest; s->has_prev_reverse=true;
}
bool fips_mmp_metrics_srtt_ms(const fips_mmp_metrics *s, double *out)
{ if (!s->srtt.initialized) return false; *out=(double)s->srtt.srtt_us/1000.0; return true; }
double fips_mmp_metrics_loss_rate(const fips_mmp_metrics *s) { return 1.0-s->delivery_ratio_forward; }
bool fips_mmp_metrics_smoothed_loss(const fips_mmp_metrics *s, double *out)
{ if (!s->loss_trend.initialized) return false; *out=s->loss_trend.long_avg; return true; }
bool fips_mmp_metrics_smoothed_etx(const fips_mmp_metrics *s, double *out)
{ if (!s->etx_trend.initialized) return false; *out=s->etx_trend.long_avg; return true; }
bool fips_mmp_metrics_quality_index(const fips_mmp_metrics *s, double *out)
{ double rtt, etx; if (!fips_mmp_metrics_srtt_ms(s, &rtt) || !fips_mmp_metrics_smoothed_etx(s, &etx)) return false;
    *out=fips_mmp_quality_index(etx, rtt); return true; }
bool fips_mmp_metrics_rr_counters(const fips_mmp_metrics *s, uint64_t *highest, uint64_t *packets, uint32_t *reordered)
{ if (!s->has_prev_rr) return false; *highest=s->prev_rr_highest_counter; *packets=s->prev_rr_cum_packets; *reordered=s->prev_rr_reorder; return true; }
