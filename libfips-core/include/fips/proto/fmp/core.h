#ifndef FIPS_PROTO_FMP_CORE_H
#define FIPS_PROTO_FMP_CORE_H
#include <fips/proto/fmp/silent.h>
#include <fips/proto/fmp/wire.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Fmp is stateless upstream. Arrays of snapshots replace LifecycleView's owned
 * Vec iterator. No live registry, Noise object or allocator is borrowed. */
typedef struct fips_fmp_conn_snapshot {
    fips_link_id link;
    bool is_outbound, has_retry_addr;
    fips_node_addr retry_addr;
    uint32_t resend_count;
    fips_fmp_bytes msg1;
} fips_fmp_conn_snapshot;
typedef struct fips_fmp_wire_outcome {
    fips_peer_identity peer_identity;
    fips_fmp_epoch remote_epoch;
    fips_session_index their_index;
    fips_fmp_bytes msg2_payload;
    fips_fmp_msg1_digest msg1_digest;
} fips_fmp_wire_outcome;
typedef struct fips_fmp_establish_snapshot {
    bool has_existing_peer;
    fips_fmp_epoch existing_peer_epoch;
    bool replace, at_max_peers, has_silent_backoff;
    fips_fmp_active_backoff silent_backoff;
} fips_fmp_establish_snapshot;
typedef struct fips_fmp_outbound_snapshot {
    bool has_existing_peer;
    fips_fmp_epoch existing_peer_epoch;
    bool our_outbound_wins, own_dial;
} fips_fmp_outbound_snapshot;
typedef enum fips_fmp_inbound_reject {
    FIPS_FMP_AtMaxPeers, FIPS_FMP_SilentBackoff
} fips_fmp_inbound_reject;
typedef enum fips_fmp_inbound_kind {
    FIPS_FMP_IN_Promote, FIPS_FMP_IN_ReplaceThenPromote,
    FIPS_FMP_IN_RestartThenPromote, FIPS_FMP_IN_Rekey, FIPS_FMP_IN_Reject
} fips_fmp_inbound_kind;
typedef struct fips_fmp_inbound_decision {
    fips_fmp_inbound_kind kind;
    fips_node_addr peer;
    fips_fmp_inbound_reject reason;
} fips_fmp_inbound_decision;
typedef enum fips_fmp_outbound_decision {
    FIPS_FMP_OUT_Promote, FIPS_FMP_OUT_CrossConnectionSwap,
    FIPS_FMP_OUT_CrossConnectionKeep, FIPS_FMP_OUT_RestartThenPromote
} fips_fmp_outbound_decision;
typedef enum fips_fmp_promotion_kind {
    FIPS_FMP_Promoted, FIPS_FMP_CrossConnectionLost, FIPS_FMP_CrossConnectionWon
} fips_fmp_promotion_kind;
typedef struct fips_fmp_promotion_result {
    fips_fmp_promotion_kind kind;
    fips_node_addr node_addr;
    fips_link_id winner_link_id, loser_link_id;
} fips_fmp_promotion_result;
FIPS_CORE_API bool fips_fmp_promotion_node_addr(const fips_fmp_promotion_result *r, fips_node_addr *out);
FIPS_CORE_API bool fips_fmp_promotion_should_close_this_connection(const fips_fmp_promotion_result *r);
FIPS_CORE_API bool fips_fmp_promotion_link_to_close(const fips_fmp_promotion_result *r, fips_link_id *out);
FIPS_CORE_API bool fips_fmp_cross_connection_winner(const fips_node_addr *ours, const fips_node_addr *theirs, bool outbound);
FIPS_CORE_API fips_fmp_inbound_decision fips_fmp_establish_inbound(const fips_fmp_establish_snapshot *snap, const fips_fmp_wire_outcome *wire);
FIPS_CORE_API fips_fmp_outbound_decision fips_fmp_establish_outbound(const fips_fmp_outbound_snapshot *snap, fips_fmp_epoch epoch);

/* Rekey attachment slot: IN_Rekey hands the verified WireOutcome and existing
 * link to the future fmp/rekey profile. If it asks for replacement, call
 * establish_inbound again with replace=true. No rekey policy/crypto here. */
typedef enum fips_fmp_conn_action_kind {
    FIPS_FMP_Teardown, FIPS_FMP_ScheduleRetry, FIPS_FMP_ResendMsg1,
    /* C ingress adapter: still unauthenticated, never a promotion or heard(). */
    FIPS_FMP_ReceiveMsg1, FIPS_FMP_ReceiveMsg2, FIPS_FMP_ReceiveEncrypted,
    FIPS_FMP_DropFrame, FIPS_FMP_DeliverLinkMessage
} fips_fmp_conn_action_kind;
typedef struct fips_fmp_conn_action {
    fips_fmp_conn_action_kind kind;
    fips_link_id link;
    fips_node_addr peer;
    fips_fmp_bytes bytes;
    fips_instant next_resend_at;
    uint32_t timestamp_ms; /* DeliverLinkMessage only */
    fips_proto_error error; /* DropFrame only */
} fips_fmp_conn_action;
/* Caller-owned action list (len <= capacity, items holds capacity entries;
 * NULL items only at capacity zero); functions APPEND atomically, returning RANGE when
 * capacity cannot hold the complete batch. Bytes borrow the input/snapshot and
 * must outlive execution. Failed sends MUST NOT call record_resend(). No I/O.
 * poll_timeouts takes only stale/failed connections, poll_resends only due ones;
 * the host schedules returned deadlines, never sleeps/polls inside the core. */
typedef struct fips_fmp_action_list {
    fips_fmp_conn_action *items;
    size_t len, capacity;
} fips_fmp_action_list;
FIPS_CORE_API fips_status fips_fmp_poll_timeouts(const fips_fmp_conn_snapshot *stale, size_t count, fips_fmp_action_list *actions);
/* Delay follows Rust powi/count+1 and float-to-u64 saturation; exponent and
 * clock overflow are bounded in C. fips_deadline applies the host ceiling. */
FIPS_CORE_API fips_status fips_fmp_poll_resends(const fips_fmp_conn_snapshot *due, size_t count, fips_instant now, uint64_t interval_ms, double backoff, fips_fmp_action_list *actions);
/* Both established and pending-session ingress MUST use this gate. Invalid
 * frames yield DropFrame without any decrypt/dispatch action; capacity errors
 * are local errors, not dropped peer input. Time is consumed on lifecycle
 * events, not on unauthenticated receipt. */
FIPS_CORE_API fips_status fips_fmp_receive(fips_link_id link, const uint8_t *frame, size_t len, fips_fmp_action_list *actions);
/* Post-AEAD event for current, previous AND pending sessions. The caller must
 * have authenticated the plaintext. This gate implements the short-plaintext
 * drop: fewer than INNER_HEADER_SIZE bytes emit only DropFrame, not delivery.
 * On success DeliverLinkMessage borrows the message starting at its type byte.
 * No promotion, rekey cutover, silent-record update or replay commit occurs
 * here; the future rekey profile owns its authenticated-frame effects. It must
 * preserve this gate on its pending-session delivery path as upstream does. */
FIPS_CORE_API fips_status fips_fmp_receive_plaintext(fips_link_id link, const uint8_t *plaintext, size_t len, fips_fmp_action_list *actions);
#ifdef __cplusplus
}
#endif
#endif
