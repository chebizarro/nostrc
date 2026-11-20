#include <fips/proto/mmp/algorithms.h>
#include "internal.h"
#include <float.h>
void fips_mmp_jitter_update(fips_mmp_jitter_estimator *s, int32_t delta)
{
    int64_t d = delta;
    if (d < 0) d = -d;
    s->jitter_q4 += d - (s->jitter_q4 >> 4);
}
uint32_t fips_mmp_jitter_us(const fips_mmp_jitter_estimator *s) { return (uint32_t)(s->jitter_q4 >> 4); }
void fips_mmp_srtt_update(fips_mmp_srtt_estimator *s, int64_t rtt)
{
    if (!s->initialized) { s->srtt_us = rtt; s->rttvar_us = rtt/2; s->initialized = true; }
    else {
        int64_t err = s->srtt_us > rtt ? s->srtt_us-rtt : rtt-s->srtt_us;
        s->rttvar_us = s->rttvar_us - (s->rttvar_us >> 2) + (err >> 2);
        s->srtt_us = s->srtt_us - (s->srtt_us >> 3) + (rtt >> 3);
    }
}
void fips_mmp_dual_ewma_update(fips_mmp_dual_ewma *s, double sample)
{
    if (!s->initialized) { s->short_avg = sample; s->long_avg = sample; s->initialized = true; }
    else {
        s->short_avg += FIPS_MMP_EWMA_SHORT_ALPHA * (sample - s->short_avg);
        s->long_avg += FIPS_MMP_EWMA_LONG_ALPHA * (sample - s->long_avg);
    }
}
fips_status fips_mmp_owd_init(fips_mmp_owd_trend_detector *out, size_t window, fips_mmp_owd_sample *storage, size_t capacity)
{
    size_t n = window < FIPS_MMP_MIN_OWD_WINDOW_SIZE ? FIPS_MMP_MIN_OWD_WINDOW_SIZE :
        window > FIPS_MMP_MAX_OWD_WINDOW_SIZE ? FIPS_MMP_MAX_OWD_WINDOW_SIZE : window;
    if (!storage || capacity < n) return FIPS_ERR_RANGE;
    *out = (fips_mmp_owd_trend_detector){storage, n, 0, 0}; return FIPS_OK;
}
void fips_mmp_owd_clear(fips_mmp_owd_trend_detector *s) { s->len = 0; s->head = 0; }
void fips_mmp_owd_push(fips_mmp_owd_trend_detector *s, uint32_t at_ms, int64_t owd_us)
{
    if (s->len == s->capacity) { s->head = (s->head+1)%s->capacity; --s->len; }
    s->samples[(s->head+s->len)%s->capacity] = (fips_mmp_owd_sample){at_ms, owd_us}; ++s->len;
}
int32_t fips_mmp_owd_trend_us_per_sec(const fips_mmp_owd_trend_detector *s)
{
    if (s->len < 2) return 0;
    double sum_x = 0.0, sum_y = 0.0;
    for (size_t i=0; i<s->len; ++i) {
        const fips_mmp_owd_sample *v = &s->samples[(s->head+i)%s->capacity];
        sum_x += (double)v->at_ms; sum_y += (double)v->owd_us;
    }
    double mean_x = sum_x/(double)s->len, mean_y = sum_y/(double)s->len;
    double num = 0.0, den = 0.0;
    for (size_t i=0; i<s->len; ++i) {
        const fips_mmp_owd_sample *v = &s->samples[(s->head+i)%s->capacity];
        double dx = (double)v->at_ms-mean_x, dy = (double)v->owd_us-mean_y;
        num += dx*dy; den += dx*dx;
    }
    if (den < DBL_EPSILON) return 0;
    double slope = (num/den)*1000.0;
    return (int32_t)mmp_clampd(slope, (double)INT32_MIN, (double)INT32_MAX);
}
double fips_mmp_compute_etx(double forward, double reverse)
{
    double product = forward*reverse;
    if (product <= 0.0) return 100.0;
    return mmp_clampd(1.0/product, 1.0, 100.0);
}
double fips_mmp_quality_index(double etx, double rtt_ms) { return etx*(1.0+rtt_ms/100.0); }
void fips_mmp_spin_init(fips_mmp_spin_bit_state *s, bool initiator) { *s = (fips_mmp_spin_bit_state){.is_initiator=initiator}; }
void fips_mmp_spin_restart(fips_mmp_spin_bit_state *s) { fips_mmp_spin_init(s, s->is_initiator); }
bool fips_mmp_spin_rx_observe(fips_mmp_spin_bit_state *s, bool bit, uint64_t counter, uint64_t now, uint64_t *rtt)
{
    if (s->is_initiator) {
        if (bit == s->current_value) {
            bool had = s->has_last_edge;
            if (had) *rtt = mmp_sub(now, s->last_edge_ms);
            s->last_edge_ms = now; s->has_last_edge = true; s->current_value = !s->current_value;
            return had;
        }
    } else if (counter > s->highest_counter_for_spin) {
        s->highest_counter_for_spin = counter; s->current_value = bit;
    }
    return false;
}
