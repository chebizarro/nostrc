#ifndef FIPS_PROTO_SETTLE_INPUT_H
#define FIPS_PROTO_SETTLE_INPUT_H
#include <fips/proto/settle/gates.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct fips_settle_bytes { const uint8_t *bytes; size_t length; } fips_settle_bytes;
typedef struct fips_settle_read {
    fips_node_addr peer;
    fips_settle_pair from;
    fips_settle_bytes bytes;
    uint8_t ephemeral[32], epoch[8];
} fips_settle_read;
typedef struct fips_settle_dial {
    fips_node_addr peer;
    fips_settle_pair to;
    fips_settle_bytes msg1;
    fips_rekey_index_lease *lease;
    bool has_cap, has_link;
    uint32_t cap;
    uint64_t link;
} fips_settle_dial;
typedef enum fips_settle_input_kind {
    FIPS_SETTLE_IN_DIAL, FIPS_SETTLE_IN_CONNECTING, FIPS_SETTLE_IN_PUNCHED,
    FIPS_SETTLE_IN_CONNECT_FAILED, FIPS_SETTLE_IN_ARRIVED, FIPS_SETTLE_IN_MSG1,
    FIPS_SETTLE_IN_REPLACE, FIPS_SETTLE_IN_REDIAL, FIPS_SETTLE_IN_LEG_HEARD,
    FIPS_SETTLE_IN_HELLO, FIPS_SETTLE_IN_ABANDON, FIPS_SETTLE_IN_MSG2,
    FIPS_SETTLE_IN_RESTARTED, FIPS_SETTLE_IN_PEER_EPOCH, FIPS_SETTLE_IN_ENDED,
    FIPS_SETTLE_IN_ORPHANED, FIPS_SETTLE_IN_HEARD, FIPS_SETTLE_IN_DISCONNECT,
    FIPS_SETTLE_IN_DEAD, FIPS_SETTLE_IN_ADMIN, FIPS_SETTLE_IN_SENT,
    FIPS_SETTLE_IN_SEND_FAILED, FIPS_SETTLE_IN_UNSENDABLE, FIPS_SETTLE_IN_MOVED,
    FIPS_SETTLE_IN_SWITCHED, FIPS_SETTLE_IN_RELEASED, FIPS_SETTLE_IN_CAPABLE,
    FIPS_SETTLE_IN_STATE
} fips_settle_input_kind;
/* Input spans are borrowed for one step. A Dial/Punched/Hello lease moves only
 * on success, including a protocol refusal returned as ordered outputs. */
typedef struct fips_settle_input {
    fips_settle_input_kind kind;
    union {
        fips_settle_dial dial;
        struct { fips_node_addr peer; fips_settle_pair to; bool has_cap; uint32_t cap; } connecting;
        struct { fips_settle_pair from; fips_settle_bytes bytes; } arrived;
        struct { fips_settle_read read; bool reachable; } msg1;
        fips_settle_read read; /* Replace, Redial */
        struct { uint64_t link; fips_settle_pair from; } leg_heard;
        struct { uint64_t link; fips_settle_bytes msg2; fips_rekey_index_lease *lease; } hello;
        struct { uint64_t link; bool authorized; } abandon;
        struct { uint64_t link; uint8_t epoch[8]; fips_settle_pair from; } msg2;
        struct { fips_node_addr peer; uint8_t epoch[8]; } epoch; /* Restarted, PeerEpoch */
        struct { fips_node_addr peer; fips_settle_pair from; uint8_t kind; } heard;
        struct { uint64_t link; fips_settle_pair from, to; bool roam; } moved;
        struct { uint64_t link; fips_settle_pair to; fips_settle_proven proven; } switched;
        struct { uint64_t link; fips_settle_pair pair; bool keep; } released;
        struct { uint64_t link; fips_settle_pair pair; fips_settle_proven proven; } state;
        uint64_t link; /* ConnectFailed, Sent, SendFailed, Unsendable, Capable */
        fips_node_addr peer; /* Ended, Orphaned, Disconnect, Dead, Admin */
    } data;
} fips_settle_input;
#ifdef __cplusplus
}
#endif
#endif
