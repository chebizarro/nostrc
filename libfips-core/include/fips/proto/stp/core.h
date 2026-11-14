#ifndef FIPS_STP_CORE_H
#define FIPS_STP_CORE_H
#include <fips/proto/stp/state.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum fips_tree_decision_kind {
    FIPS_TREE_Switch,
    FIPS_TREE_SelfRoot,
    FIPS_TREE_LoopDrop,
    FIPS_TREE_AncestryUpdate,
    FIPS_TREE_PeriodicRebroadcast,
    FIPS_TREE_ParentLost,
    FIPS_TREE_NoChange
} fips_tree_decision_kind;
typedef struct fips_tree_decision {
    fips_tree_decision_kind kind;
    fips_node_addr parent;
    uint64_t new_seq;
} fips_tree_decision;
/* Pure classifiers require my_declaration.sequence < UINT64_MAX if a new
 * declaration is selected. Action-driving APIs guard exhaustion before mutation. */
FIPS_CORE_API fips_tree_decision fips_stp_classify_announce(const fips_tree_state *s,
                                                            fips_node_addr from,
                                                            const fips_stp_ranking *r,
                                                            bool switch_suppressed);
FIPS_CORE_API fips_tree_decision fips_stp_classify_periodic(const fips_tree_state *s,
                                                            const fips_stp_ranking *r,
                                                            bool switch_suppressed);
FIPS_CORE_API bool fips_stp_should_echo(fips_node_addr announce_root, fips_node_addr our_root);

typedef enum fips_stp_action_kind {
    FIPS_STP_SendAnnounce,
    FIPS_STP_ChangeParent,
    FIPS_STP_BecameRoot,
    FIPS_STP_AncestryChanged,
    FIPS_STP_SignDeclaration,
    FIPS_STP_RefreshFilters,
    FIPS_STP_Drop
} fips_stp_action_kind;
typedef enum fips_stp_drop_reason {
    FIPS_STP_UnknownPeer,
    FIPS_STP_SenderMismatch,
    FIPS_STP_BadSignature,
    FIPS_STP_BadAncestry,
    FIPS_STP_Stale,
    FIPS_STP_Loop
} fips_stp_drop_reason;
typedef struct fips_stp_action {
    fips_stp_action_kind kind;
    fips_node_addr peer;
    bool all_peers;    /* SendAnnounce/RefreshFilters: broadcast vs peer target */
    bool changed_only; /* RefreshFilters: compare actual content, not force */
    bool invalidate_via_node, invalidate_other_roots, reset_backoff;
    bool dampened;
    fips_stp_drop_reason reason;
    fips_tree_error error;
} fips_stp_action;
#define FIPS_STP_MAX_ACTIONS 10u
typedef struct fips_stp_actions {
    fips_stp_action items[FIPS_STP_MAX_ACTIONS];
    size_t len;
} fips_stp_actions;
/* The driver mutates core state, never calls host callbacks. Execute actions in
 * order before the next input. SignDeclaration must succeed and set the state's
 * signature before SendAnnounce; retain this batch and retry starting at SignDeclaration if signing
 * fails, or terminate/discard this state. Never feed another input after a failed sign until the
 * batch completes. State mutation is NOT rolled back. SendAnnounce requests the host's normal
 * coalescing/rate-limited send path, not an unchecked websocket write. The host records actual
 * sends separately. ChangeParent/AncestryChanged carry cache invalidation + backoff obligations;
 * RefreshFilters explicitly distinguishes forced vs content-delta refreshes.
 * Invalid/unknown/stale announces return ONLY Drop: no echo/cache/filter effects.
 * known_peer/signature_verified are host authentication facts, never wire claims.
 * RANGE means local storage/depth/sequence-exhaustion refusal, preserving state and actions. */
FIPS_CORE_API fips_status fips_stp_receive_announce(fips_tree_state *s, fips_node_addr from,
                                                    const fips_tree_announce *a, bool known_peer,
                                                    bool signature_verified,
                                                    const fips_stp_ranking *r, uint64_t now_secs,
                                                    fips_instant now, fips_stp_actions *out);
FIPS_CORE_API fips_status fips_stp_periodic(fips_tree_state *s, const fips_stp_ranking *r,
                                            uint64_t now_secs, fips_instant now,
                                            fips_stp_actions *out);
FIPS_CORE_API fips_status fips_stp_remove_peer(fips_tree_state *s, fips_node_addr peer,
                                               const fips_stp_ranking *r, uint64_t now_secs,
                                               fips_instant now, fips_stp_actions *out);
#ifdef __cplusplus
}
#endif
#endif
