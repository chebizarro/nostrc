#ifndef FIPS_PROBE_STATE_H
#define FIPS_PROBE_STATE_H
#include <fips/proto/probe/limits.h>
#include <fips/proto/routing/core.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum fips_probe_stage {
    FIPS_PROBE_Bloom, FIPS_PROBE_Discovery, FIPS_PROBE_Path,
    FIPS_PROBE_Session, FIPS_PROBE_Rtt, FIPS_PROBE_Terminal
} fips_probe_stage;
typedef enum fips_probe_stage_verdict {
    FIPS_PROBE_VERDICT_Pending, FIPS_PROBE_VERDICT_Running,
    FIPS_PROBE_VERDICT_Ok, FIPS_PROBE_VERDICT_Skipped, FIPS_PROBE_VERDICT_Failed
} fips_probe_stage_verdict;
typedef enum fips_probe_fail_kind {
    FIPS_PROBE_Cached, FIPS_PROBE_DirectPeer, FIPS_PROBE_BloomMiss,
    FIPS_PROBE_BackoffSuppressed, FIPS_PROBE_AlreadyPending, FIPS_PROBE_NoTreePeers,
    FIPS_PROBE_NoResponse, FIPS_PROBE_BloomUnconfirmed, FIPS_PROBE_DisjointTrees,
    FIPS_PROBE_NoNextHop, FIPS_PROBE_Preexisting, FIPS_PROBE_SendError,
    FIPS_PROBE_HandshakeTimeout, FIPS_PROBE_NoReport, FIPS_PROBE_NoEcho,
    FIPS_PROBE_SubMillisecond, FIPS_PROBE_BadTimestampEcho, FIPS_PROBE_NotReached
} fips_probe_fail_kind;
typedef enum fips_probe_resolve_source {
    FIPS_PROBE_RESOLVE_Cache, FIPS_PROBE_RESOLVE_Lookup, FIPS_PROBE_RESOLVE_DirectPeer
} fips_probe_resolve_source;
typedef enum fips_probe_overall {
    FIPS_PROBE_OVERALL_Running, FIPS_PROBE_OVERALL_Ok, FIPS_PROBE_OVERALL_Partial,
    FIPS_PROBE_OVERALL_Failed, FIPS_PROBE_OVERALL_Cancelled
} fips_probe_overall;
typedef enum fips_probe_no_hop_reason {
    FIPS_PROBE_HOP_Local, FIPS_PROBE_HOP_NoCoords, FIPS_PROBE_HOP_DisjointTrees,
    FIPS_PROBE_HOP_NoCloserPeer, FIPS_PROBE_HOP_HopNotSendReady
} fips_probe_no_hop_reason;
typedef enum fips_probe_lookup_outcome_kind {
    FIPS_PROBE_LOOKUP_BloomMiss, FIPS_PROBE_LOOKUP_Suppressed,
    FIPS_PROBE_LOOKUP_Deduplicated, FIPS_PROBE_LOOKUP_ZeroFanout, FIPS_PROBE_LOOKUP_Sent
} fips_probe_lookup_outcome_kind;
typedef enum fips_probe_left_intact {
    FIPS_PROBE_LEFT_Preexisting, FIPS_PROBE_LEFT_AdoptedByTraffic, FIPS_PROBE_LEFT_Replaced
} fips_probe_left_intact;

typedef struct fips_probe_stage_record {
    fips_probe_stage_verdict verdict;
    bool has_reason;
    fips_probe_fail_kind reason;
    bool has_detail;
    char detail[FIPS_PROBE_DETAIL_CAPACITY];
    bool has_elapsed_ms;
    uint64_t elapsed_ms;
} fips_probe_stage_record;
/* Address-only copies, self -> root, retaining the full coordinate wire domain.
 * ~2 MiB: place PathFacts, Probe and ProbeSnapshot in caller heap/static storage,
 * not on a small stack. Fields with has_* == false have no semantic value. */
typedef struct fips_probe_path_facts {
    fips_node_addr our_coords[FIPS_COORD_MAX_ENTRIES];
    fips_node_addr their_coords[FIPS_COORD_MAX_ENTRIES];
    size_t our_coords_len, their_coords_len, our_depth, their_depth;
    bool same_root, has_lca, has_lca_depth;
    fips_node_addr lca;
    size_t lca_depth;
    bool has_tree_hops_up, has_tree_hops_down, has_tree_distance;
    size_t tree_hops_up, tree_hops_down, tree_distance;
} fips_probe_path_facts;
typedef struct fips_probe_next_hop_facts {
    fips_node_addr node_addr;
    fips_route_class route_class; /* Rust `class`; C++ keyword avoided. */
    bool direct_peer, leaves_tree_walk;
} fips_probe_next_hop_facts;
typedef struct fips_probe_preflight {
    bool session_present, coords_cached, identity_cached, target_claimed;
} fips_probe_preflight;
typedef struct fips_probe_rtt_counters {
    uint64_t reports_seen, samples, zero, arith_fail;
} fips_probe_rtt_counters;
typedef struct fips_probe_snapshot {
    fips_probe_overall overall;
    uint64_t tick_ms;
    fips_probe_stage_record bloom, discovery, path, session, rtt;
    bool has_resolve_source;
    fips_probe_resolve_source resolve_source;
    bool has_lookup_fanout;
    size_t lookup_fanout;
    bool has_path_facts;
    fips_probe_path_facts path_facts;
    bool has_next_hop;
    fips_probe_next_hop_facts next_hop;
    bool has_no_hop_reason;
    fips_probe_no_hop_reason no_hop_reason;
    bool session_preexisting, session_established;
    bool has_path_mtu, has_rtt_ms, has_srtt_ms;
    uint32_t path_mtu, rtt_ms;
    double srtt_ms;
    fips_probe_rtt_counters counters;
    bool torn_down, has_left_intact;
    fips_probe_left_intact left_intact;
    bool lookup_issued, coords_were_cached, identity_was_cached;
    uint8_t warmups_sent;
} fips_probe_snapshot;

FIPS_CORE_API const char *fips_probe_stage_verdict_name(fips_probe_stage_verdict v);
FIPS_CORE_API const char *fips_probe_fail_kind_name(fips_probe_fail_kind v);
FIPS_CORE_API const char *fips_probe_resolve_source_name(fips_probe_resolve_source v);
FIPS_CORE_API const char *fips_probe_overall_name(fips_probe_overall v);
FIPS_CORE_API const char *fips_probe_no_hop_reason_name(fips_probe_no_hop_reason v);
FIPS_CORE_API const char *fips_probe_left_intact_name(fips_probe_left_intact v);
FIPS_CORE_API fips_probe_stage_record fips_probe_stage_record_pending(void);
FIPS_CORE_API bool fips_probe_stage_record_is_failed(const fips_probe_stage_record *r);
FIPS_CORE_API fips_probe_rtt_counters fips_probe_rtt_counters_delta(
    fips_probe_rtt_counters value, fips_probe_rtt_counters base);
#ifdef __cplusplus
}
#endif
#endif
