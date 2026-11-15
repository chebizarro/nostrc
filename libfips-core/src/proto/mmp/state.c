#include <fips/proto/mmp/state.h>
#include "internal.h"
#include <string.h>
const char *fips_mmp_mode_name(fips_mmp_mode mode)
{
    switch (mode) { case FIPS_MMP_Full: return "full"; case FIPS_MMP_Lightweight: return "lightweight";
        case FIPS_MMP_Minimal: return "minimal"; default: return "unknown"; }
}
bool fips_mmp_mode_parse(const char *name, fips_mmp_mode *out)
{
    for (int i=FIPS_MMP_Full; i<=FIPS_MMP_Minimal; ++i)
        if (!strcmp(name, fips_mmp_mode_name((fips_mmp_mode)i))) { *out=(fips_mmp_mode)i; return true; }
    return false;
}
fips_status fips_mmp_peer_init(fips_mmp_peer_state *s, fips_mmp_mode mode, uint64_t secs, size_t window, bool initiator, fips_mmp_owd_sample *storage, size_t capacity)
{
    fips_mmp_peer_state v={.mode=mode, .log_interval_ms=secs>UINT64_MAX/1000 ? UINT64_MAX : secs*1000};
    fips_status status=fips_mmp_receiver_init(&v.receiver, window, storage, capacity);
    if (status!=FIPS_OK) return status;
    fips_mmp_sender_init(&v.sender); fips_mmp_metrics_init(&v.metrics); fips_mmp_spin_init(&v.spin_bit, initiator);
    *s=v; return FIPS_OK;
}
fips_status fips_mmp_session_init(fips_mmp_session_state *s, fips_mmp_mode mode, uint64_t secs, size_t window, bool initiator, fips_mmp_owd_sample *storage, size_t capacity)
{
    fips_mmp_session_state v={0};
    fips_status status=fips_mmp_peer_init(&v.roles, mode, secs, window, initiator, storage, capacity);
    if (status!=FIPS_OK) return status;
    v.roles.sender.report_interval_ms=FIPS_MMP_SESSION_COLD_START_INTERVAL_MS;
    v.roles.receiver.report_interval_ms=FIPS_MMP_SESSION_COLD_START_INTERVAL_MS;
    fips_mmp_path_mtu_init(&v.path_mtu); *s=v; return FIPS_OK;
}
void fips_mmp_peer_reset_for_rekey(fips_mmp_peer_state *s, uint64_t now)
{ fips_mmp_receiver_reset_for_rekey(&s->receiver, now); fips_mmp_metrics_reset_for_rekey(&s->metrics); fips_mmp_spin_restart(&s->spin_bit); }
void fips_mmp_session_reset_for_rekey(fips_mmp_session_state *s, uint64_t now) { fips_mmp_peer_reset_for_rekey(&s->roles, now); }
bool fips_mmp_peer_should_log(const fips_mmp_peer_state *s, uint64_t now)
{ return !s->has_last_log || mmp_sub(now, s->last_log_ms)>=s->log_interval_ms; }
void fips_mmp_peer_mark_logged(fips_mmp_peer_state *s, uint64_t now) { s->has_last_log=true; s->last_log_ms=now; }
bool fips_mmp_session_should_log(const fips_mmp_session_state *s, uint64_t now) { return fips_mmp_peer_should_log(&s->roles, now); }
void fips_mmp_session_mark_logged(fips_mmp_session_state *s, uint64_t now) { fips_mmp_peer_mark_logged(&s->roles, now); }
