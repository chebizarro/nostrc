#ifndef FIPS_PROTO_SETTLE_OUTPUT_H
#define FIPS_PROTO_SETTLE_OUTPUT_H
#include <fips/proto/settle/input.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum fips_settle_build { FIPS_SETTLE_BUILD_FRESH, FIPS_SETTLE_BUILD_FROM_CARRY } fips_settle_build;
typedef enum fips_settle_transition {
    FIPS_SETTLE_TRANSITION_PROMOTE, FIPS_SETTLE_TRANSITION_REPLACE,
    FIPS_SETTLE_TRANSITION_MOVE, FIPS_SETTLE_TRANSITION_KEEP,
    FIPS_SETTLE_TRANSITION_RESTARTED, FIPS_SETTLE_TRANSITION_ROAM
} fips_settle_transition;
typedef enum fips_settle_why {
    FIPS_SETTLE_WHY_RESTART, FIPS_SETTLE_WHY_REPLACEMENT, FIPS_SETTLE_WHY_REDIAL,
    FIPS_SETTLE_WHY_OWN_RESTART, FIPS_SETTLE_WHY_ENDED, FIPS_SETTLE_WHY_ORPHANED,
    FIPS_SETTLE_WHY_DISCONNECT, FIPS_SETTLE_WHY_DEAD, FIPS_SETTLE_WHY_ADMIN,
    FIPS_SETTLE_WHY_CLOSED
} fips_settle_why;
typedef enum fips_settle_denial {
    FIPS_SETTLE_DENIAL_GATE, FIPS_SETTLE_DENIAL_FULL,
    FIPS_SETTLE_DENIAL_HELD, FIPS_SETTLE_DENIAL_CAP
} fips_settle_denial;
typedef enum fips_settle_output_kind {
    FIPS_SETTLE_OUT_OPENED, FIPS_SETTLE_OUT_ASK, FIPS_SETTLE_OUT_ANSWER,
    FIPS_SETTLE_OUT_RESEND, FIPS_SETTLE_OUT_REKEY, FIPS_SETTLE_OUT_INSTALL,
    FIPS_SETTLE_OUT_RESET, FIPS_SETTLE_OUT_REMOVE, FIPS_SETTLE_OUT_RETIRE,
    FIPS_SETTLE_OUT_RELEASE, FIPS_SETTLE_OUT_CLOSE, FIPS_SETTLE_OUT_FREE,
    FIPS_SETTLE_OUT_SWAPPED, FIPS_SETTLE_OUT_RETRY, FIPS_SETTLE_OUT_REJECT
} fips_settle_output_kind;
/* Execute front to back. Install/Free leases are owning. Byte payloads are
 * copied into the output, never borrowed from a leg that a later action retires.
 * Connection lists use offsets in the same call's caller-owned closes arena.
 * Consume owners before reusing an output arena; do not copy live outputs. */
typedef struct fips_settle_output {
    fips_settle_output_kind kind;
    fips_node_addr peer;
    bool has_peer, has_source, stored, initiator, own, unpin, on_link, reachable, heard;
    uint64_t link;
    fips_settle_pair pair;
    fips_settle_build build;
    fips_settle_transition transition;
    fips_settle_why why;
    fips_settle_denial denial;
    fips_settle_source source;
    fips_settle_refusal refusal;
    fips_rekey_index_lease lease;
    uint8_t bytes[FIPS_SETTLE_MESSAGE_CAPACITY];
    size_t length, close_offset, close_count;
} fips_settle_output;
#ifdef __cplusplus
}
#endif
#endif
