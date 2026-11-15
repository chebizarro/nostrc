#ifndef FIPS_REKEY_CYCLE_H
#define FIPS_REKEY_CYCLE_H
#include <fips/proto/rekey/epoch.h>
#include <fips/proto/rekey/record.h>
typedef enum fips_rekey_wait {
    FIPS_REKEY_WAIT_Held,
    FIPS_REKEY_WAIT_Evidence,
    FIPS_REKEY_WAIT_Delay
} fips_rekey_wait;
typedef enum fips_rekey_cycle_view {
    FIPS_REKEY_Idle,
    FIPS_REKEY_Init,
    FIPS_REKEY_PendI,
    FIPS_REKEY_PendR
} fips_rekey_cycle_view;
typedef struct fips_rekey_attempt {
    fips_rekey_hs hs;
    fips_rekey_wire out;
    fips_rekey_index_lease lease;
    fips_rekey_transport transport;
    fips_instant first, deadline;
    fips_rekey_ladder ladder;
    bool resent;
    uint8_t resend_count;
} fips_rekey_attempt;
/* Both variant slots are cleared on every transition; kind is the sole owner. */
typedef struct fips_rekey_cycle {
    fips_rekey_cycle_view kind;
    fips_rekey_attempt attempt;
    fips_rekey_epoch ep;
    fips_rekey_wait wait;
    fips_instant deadline;
    bool restart;
    fips_rekey_final fin;
    fips_rekey_ladder probes;
    bool has_probes;
    fips_instant probed;
    bool stayed, reverted;
    fips_rekey_held_answer answer;
} fips_rekey_cycle;

#endif /* FIPS_REKEY_CYCLE_H */
