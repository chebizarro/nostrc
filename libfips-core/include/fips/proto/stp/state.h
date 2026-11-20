#ifndef FIPS_STP_STATE_H
#define FIPS_STP_STATE_H
#include <fips/proto/stp/wire.h>
#include <fips/proto/stp/limits.h>
#ifdef __cplusplus
extern "C" {
#endif
/* STP's receiver-report delivery boundary, mirroring mmp::delivery value types.
 * No MMP wire implementation or link ownership is implied. */
#define FIPS_STP_FALLBACK_MS UINT64_C(30000)
typedef struct fips_stp_rr_counters {
    uint64_t highest, received;
    uint32_t reordered;
} fips_stp_rr_counters;
typedef struct fips_stp_link_evidence {
    uint64_t epoch, next_counter;
    bool has_rr;
    fips_stp_rr_counters rr;
} fips_stp_link_evidence;
typedef enum fips_stp_resend_reason {
    FIPS_STP_Loss,
    FIPS_STP_Unverified,
    FIPS_STP_SessionChanged,
    FIPS_STP_Timeout
} fips_stp_resend_reason;
/* Delivery bookkeeping is private-by-contract. Zeroed on insertion/removal. */
typedef struct fips_stp_ack_state {
    uint64_t first_epoch, budget_epoch, counter, epoch, at_ms, resent_ms, sequence;
    fips_stp_rr_counters base;
    uint8_t base_kind, level, unverified_left, loss_left;
    bool initialized, sent, orphan, complete, resent;
} fips_stp_ack_state;
typedef struct fips_stp_peer {
    fips_node_addr id;
    bool has_declaration;
    fips_parent_declaration declaration;
    fips_tree_coordinate ancestry;
    fips_stp_ack_state ack;
} fips_stp_peer;
typedef struct fips_tree_state {
    fips_node_addr my_node_addr, root;
    fips_parent_declaration my_declaration;
    fips_tree_coordinate my_coords;
    fips_stp_peer *peers;
    size_t peer_count, peer_capacity;
    double parent_hysteresis;
    fips_flap_dampener flap;
    uint64_t fallback_ms;
} fips_tree_state;
typedef struct fips_stp_peer_cost {
    fips_node_addr peer;
    double cost;
} fips_stp_peer_cost;
typedef struct fips_stp_ranking {
    const fips_stp_peer_cost *peer_costs;
    size_t cost_count;
    const fips_node_addr *skip;
    size_t skip_count;
} fips_stp_ranking;
typedef enum fips_parent_eval_kind {
    FIPS_PARENT_None,
    FIPS_PARENT_Mandatory,
    FIPS_PARENT_Discretionary
} fips_parent_eval_kind;
typedef struct fips_parent_eval {
    fips_parent_eval_kind kind;
    fips_node_addr peer;
} fips_parent_eval;
typedef struct fips_parent_loss {
    bool changed, dampened;
} fips_parent_loss;
/* Caller-owned state/peer storage: ~2 MiB each. Use heap/static storage, never
 * small stacks. Sorted bounded arrays; zero capacity permits NULL. No allocation.
 * Fields are read-only except signatures/config through the methods below.
 * peer_count is the occupied storage length (including send-only slots); use
 * fips_tree_state_peer_count for the upstream declaration-peer count.
 * New-key exhaustion and max-depth extension return RANGE without mutation.
 * Instants share origin's domain. Wall timestamp is separately supplied.
 * Ranking costs match upstream MMP's finite nonnegative link-cost domain. */
FIPS_CORE_API void fips_tree_state_new(fips_tree_state *s, fips_node_addr me, uint64_t now_secs,
                                       fips_instant origin, fips_stp_peer *storage,
                                       size_t capacity);
FIPS_CORE_API bool fips_tree_state_is_root(const fips_tree_state *s);
FIPS_CORE_API const fips_stp_peer *fips_tree_state_peer(const fips_tree_state *s,
                                                        fips_node_addr peer);
FIPS_CORE_API fips_status fips_tree_state_update_peer(fips_tree_state *s,
                                                      const fips_parent_declaration *d,
                                                      const fips_tree_coordinate *c, bool *updated);
FIPS_CORE_API void fips_tree_state_remove_peer(fips_tree_state *s, fips_node_addr peer);
FIPS_CORE_API bool fips_tree_state_set_parent(fips_tree_state *s, fips_node_addr parent,
                                              uint64_t sequence, uint64_t timestamp,
                                              fips_instant now);
FIPS_CORE_API fips_status fips_tree_state_recompute_coords(fips_tree_state *s);
FIPS_CORE_API bool fips_tree_state_smallest_visible_root(const fips_tree_state *s,
                                                         fips_node_addr *out);
FIPS_CORE_API bool fips_tree_state_should_be_root(const fips_tree_state *s);
FIPS_CORE_API fips_status fips_tree_state_become_root(fips_tree_state *s, uint64_t now_secs);
FIPS_CORE_API bool fips_tree_state_distance_to_peer(const fips_tree_state *s, fips_node_addr peer,
                                                    size_t *distance);
FIPS_CORE_API bool fips_tree_state_find_next_hop(const fips_tree_state *s,
                                                 const fips_tree_coordinate *dest,
                                                 const fips_node_addr *skip, size_t skip_count,
                                                 fips_node_addr *out);
FIPS_CORE_API void fips_tree_state_set_parent_hysteresis(fips_tree_state *s, double hysteresis);
FIPS_CORE_API bool fips_tree_state_is_switch_suppressed(const fips_tree_state *s, fips_instant now);
FIPS_CORE_API fips_parent_eval fips_tree_state_evaluate_parent(const fips_tree_state *s,
                                                               const fips_stp_ranking *ranking);
FIPS_CORE_API fips_status fips_tree_state_recover(fips_tree_state *s,
                                                  const fips_stp_ranking *ranking,
                                                  uint64_t now_secs, fips_instant now,
                                                  fips_parent_loss *out);
FIPS_CORE_API fips_status fips_tree_state_handle_parent_lost(fips_tree_state *s,
                                                             const fips_stp_ranking *ranking,
                                                             uint64_t now_secs, fips_instant now,
                                                             bool *changed);
FIPS_CORE_API fips_status fips_tree_state_record_announce(fips_tree_state *s, fips_node_addr peer,
                                                          uint64_t seq, uint64_t counter,
                                                          const fips_stp_link_evidence *link,
                                                          fips_instant now);
FIPS_CORE_API bool fips_tree_state_check_announce(fips_tree_state *s, fips_node_addr peer,
                                                  const fips_stp_link_evidence *link,
                                                  fips_instant now, fips_stp_resend_reason *reason);
FIPS_CORE_API bool fips_tree_state_outstanding_counter(const fips_tree_state *s,
                                                       fips_node_addr peer, uint64_t *out);
FIPS_CORE_API bool fips_tree_state_outstanding_seq(const fips_tree_state *s, fips_node_addr peer,
                                                   uint64_t *out);
FIPS_CORE_API void fips_tree_state_set_fallback(fips_tree_state *s, uint64_t ms);
/* Upstream accessors: peer_count/peer_ids exclude send-only delivery slots.
 * peer_ids is a sorted indexed walk, returning false at the end. */
FIPS_CORE_API const fips_node_addr *fips_tree_state_my_node_addr(const fips_tree_state *s);
FIPS_CORE_API const fips_parent_declaration *
fips_tree_state_my_declaration(const fips_tree_state *s);
FIPS_CORE_API fips_parent_declaration *fips_tree_state_my_declaration_mut(fips_tree_state *s);
FIPS_CORE_API const fips_tree_coordinate *fips_tree_state_my_coords(const fips_tree_state *s);
FIPS_CORE_API const fips_node_addr *fips_tree_state_root(const fips_tree_state *s);
FIPS_CORE_API const fips_tree_coordinate *fips_tree_state_peer_coords(const fips_tree_state *s,
                                                                      fips_node_addr peer);
FIPS_CORE_API const fips_parent_declaration *
fips_tree_state_peer_declaration(const fips_tree_state *s, fips_node_addr peer);
FIPS_CORE_API size_t fips_tree_state_peer_count(const fips_tree_state *s);
FIPS_CORE_API bool fips_tree_state_peer_ids(const fips_tree_state *s, size_t index,
                                            fips_node_addr *out);
FIPS_CORE_API bool fips_tree_state_is_declaration_signed(const fips_tree_state *s);
FIPS_CORE_API void fips_tree_state_set_hold_down(fips_tree_state *s, uint64_t secs);
FIPS_CORE_API void fips_tree_state_set_flap_dampening(fips_tree_state *s, uint32_t threshold,
                                                      uint64_t window_secs,
                                                      uint64_t dampening_secs);
FIPS_CORE_API uint64_t fips_tree_state_dampening_secs(const fips_tree_state *s);
FIPS_CORE_API bool fips_tree_state_is_flap_dampened(const fips_tree_state *s, fips_instant now);
FIPS_CORE_API bool fips_tree_state_announce_outstanding(const fips_tree_state *s,
                                                        fips_node_addr peer);
#ifdef __cplusplus
}
#endif
#endif
