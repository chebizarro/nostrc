#include <fips/proto/stp/limits.h>
#include "internal.h"
void fips_flap_dampener_new(fips_flap_dampener *f, fips_instant origin) {
    *f = (fips_flap_dampener){.origin = origin,
                              .flap_threshold = 4,
                              .flap_window = 60000,
                              .flap_dampening_duration = 120000};
}
void fips_flap_dampener_set_hold_down(fips_flap_dampener *f, uint64_t s) {
    f->hold_down = secs_ms(s);
}
void fips_flap_dampener_set_flap_dampening(fips_flap_dampener *f, uint32_t t, uint64_t w,
                                           uint64_t d) {
    f->flap_threshold = t;
    f->flap_window = secs_ms(w);
    f->flap_dampening_duration = secs_ms(d);
    if (f->flap_dampening_duration > FIPS_MAX_FLAP_DAMPENING_MS)
        f->flap_dampening_duration = FIPS_MAX_FLAP_DAMPENING_MS;
}
uint64_t fips_flap_dampener_dampening_secs(const fips_flap_dampener *f) {
    return f->flap_dampening_duration / 1000;
}
void fips_flap_dampener_mark_switch(fips_flap_dampener *f, fips_instant now) {
    f->has_switch = true;
    f->last_parent_switch = stp_ms(f->origin, now);
}
bool fips_flap_dampener_is_flap_dampened(const fips_flap_dampener *f, fips_instant now) {
    return f->has_episode && stp_ms(f->origin, now) < f->flap_dampening_until;
}
bool fips_flap_dampener_is_hold_down_active(const fips_flap_dampener *f, fips_instant now) {
    return f->hold_down && f->has_switch &&
           sat_sub(stp_ms(f->origin, now), f->last_parent_switch) < f->hold_down;
}
bool fips_flap_dampener_record_parent_switch(fips_flap_dampener *f, fips_instant now) {
    uint64_t ms = stp_ms(f->origin, now);
    if (f->has_episode && !fips_flap_dampener_is_flap_dampened(f, now)) {
        f->has_episode = false;
        f->flap_count = 0;
        f->has_window = false;
    }
    if (f->has_window && sat_sub(ms, f->flap_window_start) < f->flap_window) {
        if (f->flap_count < UINT32_MAX)
            f->flap_count++;
    } else {
        f->has_window = true;
        f->flap_window_start = ms;
        f->flap_count = 1;
    }
    if (f->flap_count >= f->flap_threshold && !fips_flap_dampener_is_flap_dampened(f, now)) {
        f->has_episode = true;
        f->flap_dampening_until = sat_add(ms, f->flap_dampening_duration);
        return true;
    }
    return false;
}
