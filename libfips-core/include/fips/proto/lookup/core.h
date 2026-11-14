#ifndef FIPS_PROTO_LOOKUP_CORE_H
#define FIPS_PROTO_LOOKUP_CORE_H
#include <fips/proto/lookup/state.h>
#include <fips/proto/lookup/wire.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Read-only, stable snapshot for the duration of a call. Enumerate each peer
 * once; callbacks must be pure. Equivalent to Rust's RoutingView, without a
 * temporary allocated Vec. Filter capability is queried for this exact target. */
typedef struct fips_lookup_routing_view {
    void *ctx;
    size_t peer_count;
    fips_node_addr (*peer_at)(void *ctx, size_t i);
    bool (*is_tree_peer)(void *ctx, const fips_node_addr *peer);
    bool (*may_reach)(void *ctx, const fips_node_addr *peer, const fips_node_addr *target);
} fips_lookup_routing_view;
typedef enum fips_lookup_action_kind {
    FIPS_LOOKUP_SendLink, FIPS_LOOKUP_CacheCoords, FIPS_LOOKUP_WritePathMtu,
    FIPS_LOOKUP_ResetWarmupIfEstablished, FIPS_LOOKUP_RetryQueuedPackets
} fips_lookup_action_kind;
typedef struct fips_lookup_action {
    fips_lookup_action_kind kind;
    fips_node_addr peer, target;
    const uint8_t *bytes;
    size_t bytes_len;
    uint64_t request_id, now_ms;
    uint16_t path_mtu;
    const fips_tree_coordinate *coords;
} fips_lookup_action;
/* Caller supplies actions and a shared encoded-PDU buffer. SendLink borrows
 * that buffer; CacheCoords borrows the accepted response's coordinates. Drive
 * actions before reusing either. No pointers into temporary core storage.
 * Capacity failures are atomic (state, request, actions unchanged). */
typedef struct fips_lookup_action_list {
    fips_lookup_action *items;
    size_t len, capacity;
    uint8_t *bytes;
    size_t bytes_capacity;
} fips_lookup_action_list;
typedef enum fips_forward_outcome {
    FIPS_FORWARD_TtlExhausted, FIPS_FORWARD_NoPeers, FIPS_FORWARD_Forward, FIPS_FORWARD_RateLimited
} fips_forward_outcome;
typedef struct fips_forward_result { fips_forward_outcome outcome; bool used_fallback; } fips_forward_result;
typedef enum fips_request_outcome {
    FIPS_REQUEST_Duplicate, FIPS_REQUEST_OwnRequestLooped, FIPS_REQUEST_RespondAsTarget,
    FIPS_REQUEST_Forward, FIPS_REQUEST_TtlExhausted
} fips_request_outcome;
typedef struct fips_lookup_eviction {
    uint64_t request_id, age_ms;
    fips_node_addr peer;
    size_t share;
    bool answered;
} fips_lookup_eviction;
typedef struct fips_lookup_classification {
    fips_request_outcome outcome;
    bool has_eviction;
    fips_lookup_eviction evicted;
} fips_lookup_classification;
typedef enum fips_response_route_kind {
    FIPS_RESPONSE_AlreadyForwarded, FIPS_RESPONSE_Ineligible, FIPS_RESPONSE_Transit,
    FIPS_RESPONSE_Originator, FIPS_RESPONSE_Unsolicited
} fips_response_route_kind;
typedef enum fips_ineligibility { FIPS_INELIGIBLE_OtherTarget, FIPS_INELIGIBLE_NotSentTo } fips_ineligibility;
typedef struct fips_response_route {
    fips_response_route_kind kind;
    fips_ineligibility reason;
    fips_node_addr from_peer;
} fips_response_route;
typedef enum fips_initiate_decision {
    FIPS_INITIATE_Deduplicated, FIPS_INITIATE_Suppressed, FIPS_INITIATE_BloomMiss, FIPS_INITIATE_Proceed
} fips_initiate_decision;
typedef struct fips_lookup_poll_entry { fips_node_addr target; uint8_t attempt; uint32_t failures; bool has_failures; } fips_lookup_poll_entry;
typedef struct fips_lookup_poll_outcome {
    fips_lookup_poll_entry *retries, *timeouts;
    size_t retries_len, retries_capacity, timeouts_len, timeouts_capacity;
} fips_lookup_poll_outcome;
FIPS_CORE_API const char *fips_ineligibility_token(fips_ineligibility reason);
FIPS_CORE_API fips_status fips_lookup_plan_forward(fips_lookup_request *r, const fips_node_addr *from,
    const fips_lookup_routing_view *rv, fips_lookup_action_list *actions, fips_forward_result *out);
FIPS_CORE_API fips_status fips_lookup_plan_transit_forward(fips_lookup *l, fips_lookup_request *r,
    const fips_node_addr *from, const fips_lookup_routing_view *rv, uint64_t now_ms,
    fips_lookup_action_list *actions, fips_forward_result *out);
FIPS_CORE_API fips_status fips_lookup_plan_initiate(const fips_lookup_request *r,
    const fips_lookup_routing_view *rv, fips_lookup_action_list *actions);
FIPS_CORE_API fips_status fips_lookup_classify_request(fips_lookup *l, const fips_lookup_request *r,
    const fips_node_addr *from, const fips_node_addr *my_addr, uint64_t now_ms, uint64_t recent_expiry_ms,
    size_t max_recent, size_t peer_count, fips_lookup_classification *out);
FIPS_CORE_API fips_response_route fips_lookup_classify_response(fips_lookup *l, uint64_t request_id,
    const fips_node_addr *target, const fips_node_addr *from, const uint8_t digest[8]);
/* Trusted accept seam, matching upstream: caller has classified Originator and
 * verified the proof. Prefer handle_response below at an untrusted boundary. */
FIPS_CORE_API fips_status fips_lookup_on_response_accepted(fips_lookup *l, uint64_t request_id,
    const fips_node_addr *target, const fips_tree_coordinate *coords, uint64_t now_ms, uint16_t path_mtu,
    fips_lookup_action_list *actions);
/* Classify before expensive verify. Originator verifies then emits four cache/
 * retry actions; transit clamps MTU and emits one SendLink on the reverse path.
 * Stale/unsolicited/ineligible responses emit no actions and do not verify.
 * peer may be NULL except on Originator. RANGE never consumes relay memory. */
FIPS_CORE_API fips_status fips_lookup_handle_response(fips_lookup *l, const fips_lookup_response *r,
    const fips_node_addr *from, uint16_t outgoing_link_mtu, uint64_t now_ms,
    const fips_secp *secp, const fips_peer_identity *peer, uint8_t *scratch, size_t scratch_capacity,
    fips_lookup_action_list *actions, fips_response_route *route, fips_proto_error *e);
/* Exactly one rung per due call; deadlines are relative to last actual send.
 * Default [1,2,4,8] seconds: retries at 1,3,7s; final timeout at 15s.
 * Length saturates to 255, seconds->ms saturates, empty ladder times out.
 * Host resends with fresh entropy-derived ID and calls pending_record. */
FIPS_CORE_API fips_status fips_lookup_poll_pending(fips_lookup *l, uint64_t now_ms,
    const uint64_t *attempt_timeouts_secs, size_t count, fips_lookup_poll_outcome *out);
FIPS_CORE_API fips_status fips_lookup_initiate_gate(fips_lookup *l, const fips_node_addr *dest,
    uint64_t now_ms, bool reachable, fips_initiate_decision *out, uint32_t *failures);
FIPS_CORE_API void fips_lookup_initiate_failed(fips_lookup *l, const fips_node_addr *dest, uint64_t now_ms);
#ifdef __cplusplus
}
#endif
#endif
