#ifndef FIPS_REKEY_LADDER_H
#define FIPS_REKEY_LADDER_H
#include <fips/host.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
#define FIPS_REKEY_MAX_RUNGS 16u
typedef struct fips_rekey_schedule {
    uint64_t at[FIPS_REKEY_MAX_RUNGS];
    uint16_t keepalive;
    uint8_t len;
} fips_rekey_schedule;
typedef struct fips_rekey_ladder {
    fips_instant origin;
    fips_rekey_schedule sched;
    uint8_t next;
} fips_rekey_ladder;
typedef struct fips_rekey_taken {
    bool send, last;
    uint64_t latest;
} fips_rekey_taken;
/* A NULL end means no deadline. Counts include rungs beyond the fixed room. */
FIPS_CORE_API fips_rekey_schedule fips_rekey_schedule_doubling(fips_duration first, uint8_t count,
                                                               const fips_duration *end,
                                                               uint8_t *inert);
FIPS_CORE_API fips_rekey_schedule fips_rekey_schedule_probes(fips_duration first, uint8_t count,
                                                             fips_duration end, fips_duration gap,
                                                             uint8_t *inert);
FIPS_CORE_API bool fips_rekey_schedule_last_send(const fips_rekey_schedule *s, uint64_t *ms);
FIPS_CORE_API fips_rekey_ladder fips_rekey_ladder_new(fips_instant origin, fips_rekey_schedule s);
/* Absent instants have ops == NULL; no clock operation accepts an absent instant. */
FIPS_CORE_API fips_instant fips_rekey_ladder_next_at(const fips_rekey_ladder *l);
FIPS_CORE_API bool fips_rekey_ladder_take(fips_rekey_ladder *l, fips_instant now,
                                          fips_rekey_taken *out);
#ifdef __cplusplus
}
#endif

#endif /* FIPS_REKEY_LADDER_H */
