#ifndef FIPS_STP_LIMITS_H
#define FIPS_STP_LIMITS_H
#include <fips/proto/rate_limit.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
#define FIPS_MAX_FLAP_DAMPENING_MS UINT64_C(31536000000)
typedef struct fips_flap_dampener {
    fips_instant origin;
    uint64_t hold_down, last_parent_switch, flap_window_start, flap_dampening_until;
    uint64_t flap_window, flap_dampening_duration;
    uint32_t flap_count, flap_threshold;
    bool has_switch, has_window, has_episode;
} fips_flap_dampener;
FIPS_CORE_API void fips_flap_dampener_new(fips_flap_dampener *f, fips_instant origin);
FIPS_CORE_API void fips_flap_dampener_set_hold_down(fips_flap_dampener *f, uint64_t secs);
FIPS_CORE_API void fips_flap_dampener_set_flap_dampening(fips_flap_dampener *f, uint32_t threshold,
                                                         uint64_t window_secs,
                                                         uint64_t dampening_secs);
FIPS_CORE_API uint64_t fips_flap_dampener_dampening_secs(const fips_flap_dampener *f);
FIPS_CORE_API void fips_flap_dampener_mark_switch(fips_flap_dampener *f, fips_instant now);
FIPS_CORE_API bool fips_flap_dampener_record_parent_switch(fips_flap_dampener *f, fips_instant now);
FIPS_CORE_API bool fips_flap_dampener_is_flap_dampened(const fips_flap_dampener *f,
                                                       fips_instant now);
FIPS_CORE_API bool fips_flap_dampener_is_hold_down_active(const fips_flap_dampener *f,
                                                          fips_instant now);
#ifdef __cplusplus
}
#endif
#endif
