#include "time.h"
#include <fips/proto/rekey/ladder.h>
static bool push(fips_rekey_schedule *s, uint64_t at, bool keep) {
    if (s->len == FIPS_REKEY_MAX_RUNGS)
        return false;
    s->at[s->len] = at;
    if (keep)
        s->keepalive |= (uint16_t)(1u << s->len);
    ++s->len;
    return true;
}
fips_rekey_schedule fips_rekey_schedule_doubling(fips_duration first, uint8_t count,
                                                 const fips_duration *end, uint8_t *inert) {
    fips_rekey_schedule s = {0};
    uint8_t dropped = 0;
    uint64_t f = rk_millis(first);
    if (!f)
        f = 1;
    for (unsigned i = 0; i < count; ++i) {
        uint64_t factor = i < 63 ? (UINT64_C(1) << (i + 1)) - 1 : UINT64_MAX;
        uint64_t at = rk_umul(f, factor);
        if ((end && at >= rk_millis(*end)) || !push(&s, at, false))
            dropped = rk_u8add(dropped, 1);
    }
    *inert = dropped;
    return s;
}
fips_rekey_schedule fips_rekey_schedule_probes(fips_duration first, uint8_t count,
                                               fips_duration end, fips_duration gap,
                                               uint8_t *inert) {
    uint8_t dropped = 0;
    fips_rekey_schedule sends = fips_rekey_schedule_doubling(first, count, &end, &dropped), s = {0};
    uint64_t g = rk_millis(gap);
    if (!g)
        g = 1;
    uint64_t last = 0;
    for (unsigned n = 0; n <= sends.len; ++n) {
        uint64_t next = n < sends.len ? sends.at[n] : rk_millis(end);
        uint64_t fill = next > last ? (next - last - 1) / g : 0;
        for (uint64_t i = 1; i <= fill; ++i) {
            if (!push(&s, rk_uadd(last, rk_umul(i, g)), true)) {
                dropped = rk_u8add(dropped, fill - i + 1);
                break;
            }
        }
        last = rk_uadd(last, rk_umul(fill, g));
        if (n < sends.len) {
            if (!push(&s, next, false))
                dropped = rk_u8add(dropped, 1);
            last = next;
        }
    }
    *inert = dropped;
    return s;
}
bool fips_rekey_schedule_last_send(const fips_rekey_schedule *s, uint64_t *ms) {
    for (unsigned i = s->len; i > 0; --i)
        if (!(s->keepalive & (1u << (i - 1)))) {
            *ms = s->at[i - 1];
            return true;
        }
    return false;
}
fips_rekey_ladder fips_rekey_ladder_new(fips_instant origin, fips_rekey_schedule s) {
    return (fips_rekey_ladder){origin, s, 0};
}
fips_instant fips_rekey_ladder_next_at(const fips_rekey_ladder *l) {
    return l->next < l->sched.len
               ? rk_later(l->origin, fips_duration_from_millis(l->sched.at[l->next]))
               : (fips_instant){0};
}
bool fips_rekey_ladder_take(fips_rekey_ladder *l, fips_instant now, fips_rekey_taken *out) {
    fips_rekey_taken t = {0};
    bool any = false;
    while (rk_due(fips_rekey_ladder_next_at(l), now)) {
        t.send |= !(l->sched.keepalive & (1u << l->next));
        t.latest = l->sched.at[l->next++];
        any = true;
    }
    if (any) {
        t.last = l->next >= l->sched.len;
        *out = t;
    }
    return any;
}
