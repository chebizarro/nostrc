#include "time.h"
#include <fips/proto/rekey/params.h>
fips_rekey_config fips_rekey_config_fmp(void) {
    return (fips_rekey_config){.after = {120, 0},
                               .jitter = 15,
                               .messages = UINT64_C(1) << 16,
                               .handshake = {30, 0},
                               .check = {1, 0},
                               .drain = {10, 0},
                               .retention = {120, 0},
                               .hold = {120, 0},
                               .revert = {10, 0},
                               .dampening = {30, 0},
                               .ttl = {40, 0},
                               .young = {5, 0},
                               .link_age = {30, 0},
                               .session_age = {10, 0},
                               .floor_link = {31, 0},
                               .floor_session = {31, 0},
                               .floor_cutover = {0, 0},
                               .silence = {30, 0},
                               .resends = 5,
                               .soft = UINT64_C(1) << 62,
                               .hard = UINT64_MAX - (UINT64_C(1) << 20) + 1,
                               .delay = {2, 0},
                               .arrival = false};
}
fips_rekey_config fips_rekey_config_fsp(void) {
    fips_rekey_config c = fips_rekey_config_fmp();
    c.floor_link = c.floor_session = rk_secs(0);
    c.floor_cutover = rk_secs(10);
    return c;
}
fips_duration fips_rekey_msg1_span(uint8_t n) {
    uint8_t inert;
    uint64_t last = 0;
    fips_rekey_schedule s = fips_rekey_schedule_doubling(rk_secs(1), n, NULL, &inert);
    (void)fips_rekey_schedule_last_send(&s, &last);
    return fips_duration_from_millis(last);
}
fips_rekey_config fips_rekey_config_with_hold(fips_rekey_config c, fips_duration silence) {
    fips_duration retry = rk_add(rk_add(fips_rekey_msg1_span(c.resends), c.handshake), c.dampening);
    c.hold = rk_dmax(c.retention, rk_dmax(retry, silence));
    return c;
}
fips_rekey_params_error fips_rekey_params_new(fips_rekey_config c, fips_rekey_params *out) {
    const fips_duration ds[] = {
        c.after,      c.handshake,     c.check,         c.drain,   c.retention, c.hold,
        c.revert,     c.dampening,     c.ttl,           c.young,   c.link_age,  c.session_age,
        c.floor_link, c.floor_session, c.floor_cutover, c.silence, c.delay};
    for (size_t i = 0; i < sizeof ds / sizeof ds[0]; ++i)
        if (ds[i].nanos >= 1000000000)
            return FIPS_REKEY_PARAMS_InvalidDuration;
    if (!c.messages)
        return FIPS_REKEY_PARAMS_ZeroMessages;
    if (c.messages > c.soft)
        return FIPS_REKEY_PARAMS_MessagesPastSoft;
    if (rk_dcmp(c.after, rk_secs(c.jitter)) <= 0)
        return FIPS_REKEY_PARAMS_AfterNotAboveJitter;
    if (rk_dcmp(c.after, rk_add(rk_secs(c.jitter), c.link_age)) < 0)
        return FIPS_REKEY_PARAMS_AfterBelowLinkFloor;
    if (rk_dcmp(c.after, rk_secs(FIPS_REKEY_MAX_AFTER)) > 0)
        return FIPS_REKEY_PARAMS_AfterTooLong;
    const fips_duration positive[] = {c.check, c.hold, c.revert, c.ttl, c.silence};
    for (size_t i = 0; i < sizeof positive / sizeof positive[0]; ++i)
        if (!positive[i].secs && !positive[i].nanos)
            return FIPS_REKEY_PARAMS_Zero;
    if (rk_dcmp(c.handshake, rk_add(c.check, rk_secs(3))) < 0)
        return FIPS_REKEY_PARAMS_HandshakeTooShort;
    if (rk_dcmp(c.drain, rk_secs(2)) <= 0)
        return FIPS_REKEY_PARAMS_DrainTooShort;
    if (rk_dcmp(c.retention, c.drain) < 0)
        return FIPS_REKEY_PARAMS_RetentionBelowDrain;
    if (rk_dcmp(c.hold, rk_add(c.handshake, c.check)) <= 0)
        return FIPS_REKEY_PARAMS_HoldTooShort;
    if (c.soft >= c.hard)
        return FIPS_REKEY_PARAMS_SoftNotBelowHard;
    fips_rekey_params p = {0};
    p.cfg = c;
    p.confirm = rk_add(c.handshake, c.check);
    p.span = rk_add(p.confirm, c.drain);
    p.rung = rk_secs(rk_sub(c.handshake, c.check).secs / 3);
    p.gap = rk_half(rk_sub(c.drain, rk_secs(2)));
    p.budget = rk_dmin(rk_add(fips_rekey_msg1_span(c.resends), c.check), c.handshake);
    uint8_t ignored;
    uint64_t last = 0;
    p.first = fips_rekey_schedule_doubling(rk_secs(1), c.resends, &p.budget, &p.inert.first);
    p.fin = fips_rekey_schedule_doubling(rk_secs(1), c.resends, NULL, &ignored);
    p.probes = fips_rekey_schedule_probes(rk_secs(1), c.resends, p.confirm, p.gap, &p.inert.probes);
    p.revert = fips_rekey_schedule_probes(rk_secs(1), 3, c.revert, p.gap, &ignored);
    p.elicit = fips_rekey_schedule_doubling(rk_secs(1), 2, NULL, &ignored);
    p.delayed = fips_rekey_schedule_doubling(rk_secs(1), 4, NULL, &ignored);
    (void)fips_rekey_schedule_last_send(&p.probes, &last);
    uint64_t confirm = rk_millis(p.confirm);
    p.grace = fips_duration_from_millis((confirm > last ? confirm - last : 0) / 2);
    *out = p;
    return FIPS_REKEY_PARAMS_OK;
}
fips_rekey_params fips_rekey_params_fmp(void) {
    fips_rekey_params p;
    (void)fips_rekey_params_new(fips_rekey_config_fmp(), &p);
    return p;
}
fips_rekey_params fips_rekey_params_fsp(void) {
    fips_rekey_params p;
    (void)fips_rekey_params_new(fips_rekey_config_fsp(), &p);
    return p;
}
fips_duration fips_rekey_params_backoff(const fips_rekey_params *p, uint8_t n) {
    return fips_backoff(n ? n - 1u : 0u, p->cfg.check, p->cfg.hold);
}
