#include <fips/proto/mmp/path_mtu.h>
#include "internal.h"
void fips_mmp_path_mtu_init(fips_mmp_path_mtu_state *s)
{ *s=(fips_mmp_path_mtu_state){.current_mtu=UINT16_MAX, .last_observed_mtu=UINT16_MAX,
    .last_notified_mtu=UINT16_MAX, .notification_interval_ms=10000}; }
void fips_mmp_path_mtu_update_interval_from_srtt(fips_mmp_path_mtu_state *s, double srtt)
{ uint64_t ms=mmp_u64(srtt*5.0); s->notification_interval_ms=ms>10000 ? ms : 10000; }
void fips_mmp_path_mtu_seed_source_mtu(fips_mmp_path_mtu_state *s, uint16_t mtu)
{ if (mtu<s->current_mtu) s->current_mtu=mtu; }
void fips_mmp_path_mtu_observe_incoming_mtu(fips_mmp_path_mtu_state *s, uint16_t mtu)
{ if (mtu!=s->last_observed_mtu) { s->observed_changed=true; s->last_observed_mtu=mtu; } }
bool fips_mmp_path_mtu_should_send_notification(const fips_mmp_path_mtu_state *s, uint64_t now)
{
    if (s->last_observed_mtu==UINT16_MAX) return false;
    return !s->has_last_notification || (s->observed_changed && s->last_observed_mtu<s->last_notified_mtu) ||
        mmp_sub(now, s->last_notification_ms)>=s->notification_interval_ms;
}
bool fips_mmp_path_mtu_build_notification(fips_mmp_path_mtu_state *s, uint64_t now, uint16_t *out)
{
    if (s->last_observed_mtu==UINT16_MAX) return false;
    s->last_notification_ms=now; s->has_last_notification=true; s->last_notified_mtu=s->last_observed_mtu;
    s->observed_changed=false; *out=s->last_observed_mtu; return true;
}
static void clear_ladder(fips_mmp_path_mtu_state *s)
{ s->consecutive_increase_count=0; s->has_first_increase=false; s->first_increase_ms=0; s->pending_increase_mtu=0; }
static void decrease_to(fips_mmp_path_mtu_state *s, uint16_t mtu) { s->current_mtu=mtu; clear_ladder(s); }
bool fips_mmp_path_mtu_apply_notification(fips_mmp_path_mtu_state *s, uint16_t mtu, uint64_t now)
{
    if (mtu<FIPS_MMP_MIN_ACTIONABLE_PATH_MTU) return false;
    if (mtu<s->current_mtu) { decrease_to(s, mtu); return true; }
    if (mtu>s->current_mtu) {
        if (mtu==s->pending_increase_mtu) {
            if (s->consecutive_increase_count<UINT8_MAX) ++s->consecutive_increase_count;
        } else {
            s->pending_increase_mtu=mtu; s->consecutive_increase_count=1;
            s->first_increase_ms=now; s->has_first_increase=true;
        }
        /* Saturation outside Rust's non-overflowing interval domain avoids a
         * wrapped deadline falsely accepting an enormous requested interval. */
        uint64_t required=mmp_add(s->notification_interval_ms, s->notification_interval_ms);
        if (s->consecutive_increase_count>=3 && s->has_first_increase && mmp_sub(now, s->first_increase_ms)>=required) {
            s->current_mtu=mtu; clear_ladder(s); return true;
        }
    }
    return false;
}
bool fips_mmp_path_mtu_apply_reactive_decrease(fips_mmp_path_mtu_state *s, uint16_t mtu)
{
    if (mtu<FIPS_MMP_MIN_ACTIONABLE_PATH_MTU || mtu>=s->current_mtu) return false;
    decrease_to(s, mtu); return true;
}
void fips_mmp_path_mtu_reset_source_mtu(fips_mmp_path_mtu_state *s) { s->current_mtu=UINT16_MAX; clear_ladder(s); }
