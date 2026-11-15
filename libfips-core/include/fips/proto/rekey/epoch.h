#ifndef FIPS_REKEY_EPOCH_H
#define FIPS_REKEY_EPOCH_H
#include <fips/proto/rekey/ladder.h>
#include <fips/proto/rekey/profile.h>
typedef enum fips_rekey_slot {
    FIPS_REKEY_Current,
    FIPS_REKEY_Pending,
    FIPS_REKEY_Previous
} fips_rekey_slot;
typedef enum fips_rekey_role { FIPS_REKEY_Initiator, FIPS_REKEY_Responder } fips_rekey_role;
typedef enum fips_rekey_recv_home { FIPS_REKEY_Here, FIPS_REKEY_Delegated } fips_rekey_recv_home;
typedef enum fips_rekey_purpose {
    FIPS_REKEY_PURPOSE_Attempt,
    FIPS_REKEY_PURPOSE_Pending,
    FIPS_REKEY_PURPOSE_Current,
    FIPS_REKEY_PURPOSE_Previous
} fips_rekey_purpose;
typedef struct fips_rekey_ids {
    fips_rekey_index_lease lease;
    fips_rekey_index theirs;
    fips_rekey_transport transport;
} fips_rekey_ids;
typedef struct fips_rekey_final {
    bool present;
    fips_rekey_wire msg;
    fips_rekey_ladder ladder;
} fips_rekey_final;
/* Storage definitions, not state setters. Hosts must not modify any field.
 * session points into the machine's fixed arena: slot moves copy no keys.
 * ext is caller-local per-epoch data, zeroed on epoch creation/retirement. */
typedef struct fips_rekey_epoch {
    bool present;
    fips_rekey_epoch_tag tag;
    fips_noise_session *session;
    fips_rekey_ids ids;
    bool k;
    fips_rekey_role role;
    fips_instant base, started;
    fips_rekey_route_handle route;
    bool has_route;
    uint8_t ext[128];
    fips_rekey_recv_home recv;
    uint64_t floor;
    bool has_floor;
    fips_instant heard, moved;
    bool assumed;
    fips_rekey_final fin;
    fips_rekey_ladder elicit;
    bool has_elicit;
    fips_instant prompted;
} fips_rekey_epoch;
typedef struct fips_rekey_drain {
    fips_rekey_epoch ep;
    fips_instant deadline;
    bool evidenced;
} fips_rekey_drain;

#endif /* FIPS_REKEY_EPOCH_H */
