#include <fips/proto/mmp/core.h>
#include <string.h>
static bool room(size_t used, size_t capacity) { return used<capacity; }
fips_status fips_mmp_plan_heartbeats(const fips_mmp_peer_liveness_snapshot *p, size_t n, fips_mmp_action_list *out)
{
    size_t need=out->len;
    for (size_t i=0; i<n; ++i) if ((p[i].time_dead && !p[i].rekey_active) || p[i].heartbeat_due) {
        if (!room(need, out->capacity)) return FIPS_ERR_RANGE;
        ++need;
    }
    for (unsigned phase=0; phase<2; ++phase) for (size_t i=0; i<n; ++i) {
        bool reap=p[i].time_dead && !p[i].rekey_active;
        if ((phase==0 && reap) || (phase==1 && !reap && p[i].heartbeat_due))
            out->items[out->len++]=(fips_mmp_action){.kind=phase==0 ? FIPS_MMP_ReapPeer : FIPS_MMP_Heartbeat, .peer=p[i].peer};
    }
    return FIPS_OK;
}
static bool link_gate(const fips_mmp_link_report_snapshot *p, unsigned phase)
{
    if (!phase) return p->log_due;
    if (phase==1) return p->mode==FIPS_MMP_Full && p->send_sr && p->sr_due;
    return p->mode!=FIPS_MMP_Minimal && p->send_rr && p->rr_due;
}
fips_status fips_mmp_plan_link_reports(const fips_mmp_link_report_snapshot *p, size_t n, fips_mmp_action_list *out)
{
    size_t need=out->len;
    for (unsigned phase=0; phase<3; ++phase) for (size_t i=0; i<n; ++i) if (link_gate(&p[i], phase)) {
        if (!room(need, out->capacity)) return FIPS_ERR_RANGE;
        ++need;
    }
    for (unsigned phase=0; phase<3; ++phase) for (size_t i=0; i<n; ++i) if (link_gate(&p[i], phase)) {
        fips_mmp_action a={.kind=phase==0 ? FIPS_MMP_LogLink : FIPS_MMP_SendLinkReport, .peer=p[i].peer};
        if (phase) a.report.link=phase==1 ? FIPS_MMP_LinkSender : FIPS_MMP_LinkReceiver;
        out->items[out->len++]=a;
    }
    return FIPS_OK;
}
static bool session_gate(const fips_mmp_session_report_snapshot *s, unsigned phase)
{ return phase==0 ? s->mode==FIPS_MMP_Full && s->sr_due : phase==1 ? s->mode!=FIPS_MMP_Minimal && s->rr_due : s->mtu_due; }
fips_status fips_mmp_plan_session_reports(const fips_mmp_session_report_snapshot *s, size_t n, fips_mmp_action_list *out)
{
    size_t need=out->len;
    for (size_t i=0; i<n; ++i) {
        for (unsigned phase=0; phase<4; ++phase) if (phase==3 ? s[i].log_due : session_gate(&s[i], phase)) {
            if (!room(need, out->capacity)) return FIPS_ERR_RANGE;
            ++need;
        }
    }
    for (size_t i=0; i<n; ++i) if (s[i].log_due)
        out->items[out->len++]=(fips_mmp_action){.kind=FIPS_MMP_LogSession, .peer=s[i].dest};
    for (size_t i=0; i<n; ++i) for (unsigned phase=0; phase<3; ++phase) if (session_gate(&s[i], phase)) {
        fips_mmp_action a={.kind=FIPS_MMP_SendSessionReport, .peer=s[i].dest};
        a.report.session=(fips_mmp_session_report_kind)phase; out->items[out->len++]=a;
    }
    return FIPS_OK;
}
fips_status fips_mmp_plan_backoff(const fips_mmp_send_result *r, size_t n, fips_mmp_backoff_list *out)
{
    size_t need=out->len;
    for (size_t i=0; i<n; ++i) {
        bool first=true;
        for (size_t j=0; j<i; ++j) if (!memcmp(&r[i].dest, &r[j].dest, sizeof(r[i].dest))) { first=false; break; }
        if (first) { if (!room(need, out->capacity)) return FIPS_ERR_RANGE; ++need; }
    }
    size_t start=out->len;
    for (size_t i=0; i<n; ++i) {
        size_t at=start;
        while (at<out->len && memcmp(&out->items[at].dest, &r[i].dest, sizeof(r[i].dest))<0) ++at;
        if (at<out->len && !memcmp(&out->items[at].dest, &r[i].dest, sizeof(r[i].dest))) {
            if (r[i].ok) out->items[at].kind=FIPS_MMP_Success;
        } else {
            memmove(out->items+at+1, out->items+at, (out->len-at)*sizeof(*out->items));
            out->items[at]=(fips_mmp_backoff_update){r[i].ok ? FIPS_MMP_Success : FIPS_MMP_Failure, r[i].dest}; ++out->len;
        }
    }
    return FIPS_OK;
}
