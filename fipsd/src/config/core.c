#include "internal.h"
#include <math.h>
#include <string.h>
#define V(p) fipsd_config_get(c,p)
#define CU(p) fipsd_config_uint(V(p))
#define N(p) fipsd_config_number(V(p))
static uint64_t add(uint64_t a,uint64_t b) { return a>UINT64_MAX-b ? UINT64_MAX : a+b; }
static uint64_t millis(uint64_t s) { return s>UINT64_MAX/1000 ? UINT64_MAX : s*1000; }
static bool present(const fipsd_config_value *v) { return v && v->kind!=CFG_NULL; }
bool cfg_core(fipsd_config *c, fipsd_config_log *log)
{
    fipsd_config_core *out=&c->core;
    uint64_t timeout=CU("node.rate_limit.handshake_timeout_secs"), resends=CU("node.rate_limit.handshake_max_resends");
    uint8_t count=(uint8_t)(resends>UINT8_MAX ? UINT8_MAX : resends);
    fips_rekey_config base=fips_rekey_config_fmp();
    base.arrival=fipsd_config_bool(V("node.path.enabled"));
    fips_rekey_config link=base;
    link.handshake=(fips_duration){timeout,0}; link.resends=count;
    fips_duration span=fips_rekey_msg1_span(count);
    uint64_t heartbeat=CU("node.heartbeat_interval_secs"), dead=CU("node.link_dead_timeout_secs");
    uint64_t silence=add(add(millis(span.secs),span.nanos/1000000),add(3000,millis(heartbeat>dead ? heartbeat : dead)));
    link=fips_rekey_config_with_hold(link,fips_duration_from_millis(silence));
    link.handshake.secs=timeout>30 ? 30 : timeout;
    fips_rekey_params params;
    if (fips_rekey_params_new(link,&params)!=FIPS_REKEY_PARAMS_OK) {
        cfg_log(log,FIPSD_LOG_WARN,"Link rekey timers use the defaults: the configured handshake timeout does not fit the 1 s tick");
        link=base;
    }
    uint64_t after=CU("node.rekey.after_secs");
    after=after>FIPS_REKEY_MAX_AFTER ? FIPS_REKEY_MAX_AFTER : after;
    uint64_t floor=add(link.jitter,link.link_age.secs);
    if (after<floor) cfg_log(log,FIPSD_LOG_WARN,"Link rekeys start no sooner than the jitter plus the 30 s link age at which a peer answers them after_secs=%llu used_secs=%llu",(unsigned long long)after,(unsigned long long)floor);
    link.after=(fips_duration){after<floor ? floor : after,0};
    uint64_t messages=CU("node.rekey.after_messages");
    link.messages=messages>link.soft ? link.soft : messages;
    out->link_rekey=link;
    fips_rekey_config session=fips_rekey_config_fsp();
    session.link_age=(fips_duration){0,0};
    base=session;
    session.handshake=(fips_duration){timeout,0}; session.resends=count;
    if (session.hold.secs<add(timeout,2)) session.hold.secs=add(timeout,2);
    if (fips_rekey_params_new(session,&params)!=FIPS_REKEY_PARAMS_OK) {
        cfg_log(log,FIPSD_LOG_WARN,"Session rekey timers use the defaults: the configured handshake timeout does not fit the 1 s tick");
        session=base;
    }
    session.after=(fips_duration){after,0}; session.messages=messages>session.soft ? session.soft : messages;
    out->session_rekey=session;
    out->handshake=(fips_bucket_params){(uint32_t)CU("node.rate_limit.handshake_burst"),N("node.rate_limit.handshake_rate")};
    if (!isfinite(out->handshake.rate) || out->handshake.rate<0)
        return cfg_error(log,"invalid configuration: `node.rate_limit.handshake_rate` must be finite and nonnegative");
    out->established=fips_derive_established_bucket((size_t)CU("node.limits.max_peers"),CU("node.rekey.after_secs"),(uint32_t)resends,out->handshake);
    if (present(V("node.rate_limit.established_handshake_burst"))) out->established.burst=(uint32_t)CU("node.rate_limit.established_handshake_burst");
    if (present(V("node.rate_limit.established_handshake_rate"))) out->established.rate=N("node.rate_limit.established_handshake_rate");
    out->session_setup=(fips_bucket_params){(uint32_t)CU("node.rate_limit.session_setup_burst"),N("node.rate_limit.session_setup_rate")};
    out->max_pending_inbound=(size_t)CU("node.limits.max_pending_inbound");
    out->coord_size=(size_t)CU("node.cache.coord_size");
    uint64_t ttl=CU("node.cache.coord_ttl_secs");
#ifdef FIPSD_TEST_OVERRIDES
    if (present(V("node.cache.test_coord_ttl_secs"))) ttl=CU("node.cache.test_coord_ttl_secs");
#endif
    out->coord_ttl_ms=millis(ttl);
    (void)fips_mmp_mode_parse(fipsd_config_string(V("node.mmp.mode")),&out->mmp_mode);
    (void)fips_mmp_mode_parse(fipsd_config_string(V("node.session_mmp.mode")),&out->session_mmp_mode);
    out->mmp_window=(size_t)CU("node.mmp.owd_window_size");
    out->session_mmp_window=(size_t)CU("node.session_mmp.owd_window_size");
    out->mmp_log_interval_secs=CU("node.mmp.log_interval_secs");
    out->session_mmp_log_interval_secs=CU("node.session_mmp.log_interval_secs");
    return true;
}
void fipsd_config_core_params(const fipsd_config *c, fipsd_config_core *out) { *out=c->core; }
