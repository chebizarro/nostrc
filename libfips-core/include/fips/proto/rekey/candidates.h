#ifndef FIPS_REKEY_CANDIDATES_H
#define FIPS_REKEY_CANDIDATES_H
#include <fips/proto/rekey/profile.h>
typedef struct fips_rekey_candidate {
    fips_rekey_hs hs;
    fips_rekey_wire msg1, ack;
    fips_node_addr carrier;
    fips_rekey_route_handle route;
    fips_instant born, deadline;
    uint8_t bound[8];
    bool has_bound;
    uint64_t seq;
} fips_rekey_candidate;
typedef struct fips_rekey_candidates {
    fips_rekey_candidate list[FIPS_REKEY_CANDIDATES_MAX];
    size_t count;
    uint64_t next;
} fips_rekey_candidates;

#endif /* FIPS_REKEY_CANDIDATES_H */
