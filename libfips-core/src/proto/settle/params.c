#include <fips/proto/settle/settle.h>
#include <fips/proto/settle/faults.h>
#include <fips/proto/math.h>
#include <fips/proto/fmp/silent.h>
#include <math.h>
#include <string.h>

_Static_assert(FIPS_DEADLINE_CEILING_SECS == UINT64_C(100)*365*24*60*60,
               "settle deadline uses upstream's 100-year ceiling");
_Static_assert(FIPS_SETTLE_DEFAULT_SILENT == FIPS_FMP_SILENT_SESSION_LIMIT, "shared silent limit");
_Static_assert(FIPS_SETTLE_DEFAULT_COUNTED == FIPS_FMP_COUNTED_SETUP_RECORD, "shared replay bound");
_Static_assert(sizeof(double) == sizeof(uint64_t), "backoff uses binary64 bits");

fips_settle_params fips_settle_params_default(void)
{
    fips_settle_params p = {
        .unheard={1,0}, .interval={SETTLE_FAULT(IntervalTen) ? 10u : 15u,0}, .silentbase={30,0},
        .silentcap={600,0}, .horizon={600,0}, .handshake={30,0},
        .first={1,0}, .resend={1,0}, .retry={1,0},
        .replays=FIPS_SETTLE_DEFAULT_REPLAYS, .counted=FIPS_SETTLE_DEFAULT_COUNTED,
        .pending=1000, .peers=128, .silent=FIPS_SETTLE_DEFAULT_SILENT,
        .resends=5, .backoff=UINT64_C(0x4000000000000000)
    };
    return p;
}
fips_duration fips_settle_wait(const fips_settle_params *p, uint32_t sent)
{
    uint64_t base;
    if (p->resend.secs > UINT64_MAX / 1000 ||
        (p->resend.secs == UINT64_MAX / 1000 && p->resend.nanos/1000000 > UINT64_MAX%1000))
        base = UINT64_MAX;
    else base = p->resend.secs*1000 + p->resend.nanos/1000000;
    double multiplier;
    memcpy(&multiplier, &p->backoff, sizeof(multiplier));
    if (SETTLE_FAULT(BackoffDoubles)) return fips_backoff(sent,p->resend,(fips_duration){UINT64_MAX,999999999});
    double ms = (double)base * fips_powi(multiplier, sent);
    /* Rust float-to-u64 cast saturates, including NaN -> 0. */
    uint64_t wait = !(ms > 0) ? 0 : ms >= 0x1p64 ? UINT64_MAX : (uint64_t)ms;
    return fips_duration_from_millis(wait);
}
