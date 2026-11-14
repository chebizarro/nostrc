#ifndef FIPS_PROTO_BLOOM_STATE_H
#define FIPS_PROTO_BLOOM_STATE_H
#include <fips/proto/bloom/wire.h>
#ifdef __cplusplus
extern "C" {
#endif
#define FIPS_BLOOM_FALLBACK_MS UINT64_C(30000)
#define FIPS_BLOOM_QUIET_MS UINT64_C(120000)
#define FIPS_BLOOM_LOSS_BUDGET 3u
#define FIPS_BLOOM_UNVERIFIED_BUDGET 1u
/* MMP value snapshot, not an MMP implementation. The shell supplies counters
 * after sending and the latest authenticated receiver report. */
typedef struct fips_bloom_rr { uint64_t highest, received; uint32_t reordered; } fips_bloom_rr;
typedef struct fips_bloom_link_evidence {
    uint64_t epoch, next_counter;
    bool has_rr;
    fips_bloom_rr rr;
} fips_bloom_link_evidence;
typedef enum fips_bloom_resend {
    FIPS_BLOOM_RESEND_NONE, FIPS_BLOOM_RESEND_LOSS, FIPS_BLOOM_RESEND_UNVERIFIED,
    FIPS_BLOOM_RESEND_SESSION_CHANGED, FIPS_BLOOM_RESEND_TIMEOUT
} fips_bloom_resend;
/* Private bookkeeping fields are exposed only to permit caller-owned storage. */
typedef struct fips_bloom_ack {
    uint64_t first_epoch, budget_epoch, counter, epoch, at_ms, resent_ms;
    fips_bloom_rr base_rr;
    uint8_t base, level, unverified_left, loss_left;
    bool initialized, outstanding, orphan, base_complete, has_resent;
} fips_bloom_ack;
typedef struct fips_bloom_peer {
    fips_node_addr addr;
    fips_bloom_filter inbound, sent[FIPS_BLOOM_SENT_HISTORY]; /* newest first */
    size_t sent_len;
    uint64_t inbound_sequence, sequence, last_sent_ms, echo_warned_ms;
    bool has_inbound, has_sequence, has_last_sent, pending, has_echo_warned, mtu_warned;
    bool tree, child, authenticated;
    fips_bloom_resend pending_resend;
    fips_bloom_ack ack;
} fips_bloom_peer;
typedef struct fips_bloom_state {
    fips_node_addr own_addr;
    fips_bloom_peer *peers;
    size_t peer_count, peer_capacity;
    fips_node_addr *leaves;
    size_t leaf_count, leaf_capacity;
    uint64_t sequence, update_debounce_ms, fallback_ms, inbound_recomputes;
    double max_inbound_fpr, child_echo_threshold;
    bool is_leaf_only, inbound_dirty;
} fips_bloom_state;
typedef enum fips_bloom_refusal {
    FIPS_BLOOM_REFUSE_NONE, FIPS_BLOOM_REFUSE_PARAMETERS, FIPS_BLOOM_REFUSE_FPR,
    FIPS_BLOOM_REFUSE_CHILD_ECHO, FIPS_BLOOM_REFUSE_MTU, FIPS_BLOOM_REFUSE_TRANSPORT
} fips_bloom_refusal;
typedef enum fips_bloom_action_kind { FIPS_BLOOM_SEND, FIPS_BLOOM_REFUSED } fips_bloom_action_kind;
typedef struct fips_bloom_action {
    fips_bloom_action_kind kind;
    fips_node_addr peer;
    fips_filter_announce announce;
    fips_bloom_refusal refusal;
    fips_bloom_resend resend;
    bool warn; /* every refusal reported; false means DEBUG, never suppressed */
    double fpr, overlap, bound;
    size_t packet_size;
    uint16_t mtu; /* send-refusal evidence supplied by the transport */
    uint8_t received_digest[32], sent_digest[32]; /* child-echo diagnostics */
} fips_bloom_action;
typedef struct fips_bloom_actions {
    fips_bloom_action *items;
    size_t len, capacity;
} fips_bloom_actions;
/* No IO/allocations. Caller synchronizes and owns distinct storage (use heap
 * or static storage: a peer holds five bounded filters). Millisecond readings
 * share one monotonic domain. Initialize before all use; do not mutate fields
 * except documented tunables (debounce/fallback/cap/threshold). All capacity
 * failures return RANGE without mutation. No eviction of live peer state.
 * add_peer is idempotent; topology/auth changes use peer_role. */
FIPS_CORE_API void fips_bloom_state_init(fips_bloom_state *s, fips_node_addr own, bool leaf_only,
    fips_bloom_peer *peers, size_t peer_capacity, fips_node_addr *leaves, size_t leaf_capacity);
FIPS_CORE_API fips_bloom_peer *fips_bloom_state_peer(fips_bloom_state *s, const fips_node_addr *peer);
FIPS_CORE_API fips_status fips_bloom_state_add_peer(fips_bloom_state *s, fips_node_addr addr);
FIPS_CORE_API bool fips_bloom_state_remove_peer(fips_bloom_state *s, const fips_node_addr *addr);
FIPS_CORE_API fips_status fips_bloom_state_add_leaf(fips_bloom_state *s, fips_node_addr addr);
FIPS_CORE_API bool fips_bloom_state_remove_leaf(fips_bloom_state *s, const fips_node_addr *addr);
FIPS_CORE_API void fips_bloom_state_peer_role(fips_bloom_state *s, fips_bloom_peer *peer, bool tree, bool child, bool authenticated);
FIPS_CORE_API void fips_bloom_state_mark_all(fips_bloom_state *s);
FIPS_CORE_API void fips_bloom_state_clear_pending(fips_bloom_state *s);
FIPS_CORE_API void fips_bloom_state_note_inbound_change(fips_bloom_state *s);
FIPS_CORE_API bool fips_bloom_state_take_inbound_change(fips_bloom_state *s);
FIPS_CORE_API void fips_bloom_state_base_filter(const fips_bloom_state *s, fips_bloom_filter *out);
FIPS_CORE_API void fips_bloom_state_outgoing(const fips_bloom_state *s, const fips_node_addr *exclude, fips_bloom_filter *out);
/* One pass, O(peers * filter bytes), not quadratic in peer count. */
FIPS_CORE_API void fips_bloom_state_mark_changed(fips_bloom_state *s, const fips_node_addr *exclude_source);
FIPS_CORE_API bool fips_bloom_peer_should_send(const fips_bloom_state *s, const fips_bloom_peer *p, uint64_t now_ms);
FIPS_CORE_API uint64_t fips_bloom_peer_next_sequence(fips_bloom_state *s, fips_bloom_peer *p, uint64_t floor);
FIPS_CORE_API void fips_bloom_peer_record_update(fips_bloom_peer *p, uint64_t now_ms);
FIPS_CORE_API void fips_bloom_peer_record_filter(fips_bloom_peer *p, const fips_bloom_filter *f);
FIPS_CORE_API void fips_bloom_peer_forget_sent(fips_bloom_peer *p);
FIPS_CORE_API bool fips_bloom_peer_warn_echo(fips_bloom_peer *p, uint64_t now_ms);
FIPS_CORE_API bool fips_bloom_peer_warn_mtu(fips_bloom_peer *p);
/* Record delivery BEFORE record_filter, exactly as upstream. */
FIPS_CORE_API void fips_bloom_peer_record_announce(fips_bloom_peer *p, const fips_bloom_filter *f, uint64_t counter, const fips_bloom_link_evidence *link, uint64_t now_ms);
FIPS_CORE_API fips_bloom_resend fips_bloom_peer_check_announce(fips_bloom_state *s, fips_bloom_peer *p, const fips_bloom_link_evidence *link, uint64_t now_ms);
/* Accepted inbound replaces the previous filter, marks a deferred recompute.
 * Duplicate/old sequences change nothing. Policy refusals append a REFUSED
 * action but return OK; capacity errors return RANGE. p must belong to s.
 * Refusals leave inbound filter AND sequence unchanged (no wire NACK). */
FIPS_CORE_API fips_status fips_bloom_state_receive(fips_bloom_state *s, fips_bloom_peer *p, const fips_filter_announce *a, uint64_t now_ms, fips_bloom_actions *actions);
/* Refresh triggers: connect/remove/role/leaf changes, accepted inbound and
 * responder rekey/restart. No periodic refresh or independent expiry timer.
 * forget_sent preserves delivery/debounce/MTU warning, marks pending. */
FIPS_CORE_API void fips_bloom_state_peer_restarted(fips_bloom_state *s, fips_bloom_peer *p);
/* Emit due SEND actions on a host-driven send pass. floor is the shell's
 * increasing sequence floor (must outrun each peer's sequence across removal).
 * Does NOT record a transport send. Host must feed each result back below;
 * do not retain peer pointers across add/remove. Unauthenticated peers wait.
 * Capacity preflight counts exactly the sends due, including deferred
 * recomputation; failure preserves all state. Consume send results before
 * processing further state changes (the adapter is synchronous). */
FIPS_CORE_API fips_status fips_bloom_state_refresh(fips_bloom_state *s, uint64_t now_ms, uint64_t floor, fips_bloom_actions *actions);
typedef enum fips_bloom_send_result { FIPS_BLOOM_SEND_ACCEPTED, FIPS_BLOOM_SEND_TOO_LARGE, FIPS_BLOOM_SEND_FAILED } fips_bloom_send_result;
typedef struct fips_bloom_send_outcome {
    fips_bloom_send_result result;
    size_t packet_size;
    uint16_t mtu;
} fips_bloom_send_outcome;
/* Only ACCEPTED records filter, timestamp and outstanding delivery. Failure
 * keeps pending, reports a refusal plainly; MTU WARN is once per peering. */
FIPS_CORE_API fips_status fips_bloom_state_sent(fips_bloom_state *s, fips_bloom_peer *p, const fips_bloom_action *send, fips_bloom_send_outcome outcome, uint64_t counter, const fips_bloom_link_evidence *link, uint64_t now_ms, fips_bloom_actions *actions);
#ifdef __cplusplus
}
#endif
#endif
