#ifndef FIPS_ROUTING_CORE_H
#define FIPS_ROUTING_CORE_H
#include <fips/proto/routing/state.h>
#include <fips/proto/routing/wire.h>
#include <fips/proto/link.h>
#include <fips/proto/bloom/core.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Borrowed snapshot of currently active peers; no peer-count allocation or
 * coordinate copies. A NULL filter means may_reach=false, NULL coords means
 * unknown (distance SIZE_MAX). The host supplies aggregated inbound filters
 * from bloom state and coordinates from STP. No additional can_send predicate
 * exists in the pinned RoutingView. Enumeration order is retained (including
 * upstream IEEE comparisons for non-finite link costs). */
typedef struct fips_routing_peer {
    fips_node_addr addr;
    const fips_tree_coordinate *coords;
    const fips_bloom_filter *filter;
    double link_cost;
} fips_routing_peer;
typedef struct fips_routing_view {
    const fips_routing_peer *peers;
    size_t peer_count;
    void *ctx;
    /* Optional callbacks: NULL means false / uncached. Read-only; no IO. */
    bool (*is_congested)(void *ctx, const fips_node_addr *next_hop);
    const fips_tree_coordinate *(*cached_coords)(void *ctx, const fips_node_addr *dest, fips_instant now);
} fips_routing_view;
typedef struct fips_next_hop { fips_node_addr addr; uint16_t link_mtu; } fips_next_hop;
typedef enum fips_drop_reason { FIPS_DROP_TtlExhausted } fips_drop_reason;
typedef enum fips_route_outcome_kind { FIPS_ROUTE_Drop, FIPS_ROUTE_DeliverLocal, FIPS_ROUTE_Forward, FIPS_ROUTE_NoRoute } fips_route_outcome_kind;
typedef struct fips_route_outcome {
    fips_route_outcome_kind kind;
    fips_drop_reason reason; /* Drop only */
    fips_node_addr next_hop; /* Forward only */
    bool outgoing_ce;       /* Forward only */
} fips_route_outcome;
typedef enum fips_route_action_kind { FIPS_ROUTE_SendError } fips_route_action_kind;
typedef struct fips_route_action { fips_route_action_kind kind; fips_node_addr toward; } fips_route_action;
typedef struct fips_error_synth {
    fips_limit_verdict verdict;
    bool has_action;
    fips_route_action action;
} fips_error_synth;
typedef enum fips_route_class {
    FIPS_ROUTE_TreeUp, FIPS_ROUTE_TreeDown, FIPS_ROUTE_TreeDownCross,
    FIPS_ROUTE_CrosslinkDescend, FIPS_ROUTE_CrosslinkAscend, FIPS_ROUTE_DirectPeer
} fips_route_class;
/* Ordering: (cost, distance, address); strict self-distance progress. None
 * preserves out. No fallback is invented here: the shell may resolve a greedy
 * tree/direct hop on a bloom miss, then supply it to route, as in Rust. */
FIPS_CORE_API bool fips_select_best_candidate(const fips_routing_view *rv, const fips_node_addr *dest,
    const fips_tree_coordinate *dest_coords, const fips_tree_coordinate *my_coords, fips_node_addr *out);
FIPS_CORE_API fips_route_class fips_classify_forward(const fips_node_addr *dest, const fips_node_addr *chosen_peer,
    const fips_node_addr *my_addr, const fips_tree_coordinate *my_coords,
    const fips_tree_coordinate *dest_coords, const fips_tree_coordinate *peer_coords);
FIPS_CORE_API const char *fips_route_class_name(fips_route_class c);
/* bytes appends encoded datagrams only for Forward / SendError, replacing
 * Rust's Vec in the corresponding outcome/action. All results borrow that
 * writer's caller-owned storage. No append for local/drop/no-route/suppress.
 * Required pointers valid, input and output buffers disjoint. Capacity failure
 * preserves bytes, out AND router (including the limiter); callbacks are only
 * read after the corresponding gate. now/origin share one clock domain. */
FIPS_CORE_API fips_status fips_router_route(fips_router *router, const fips_session_datagram *dg,
    const fips_node_addr *my_addr, bool incoming_ce, const fips_next_hop *next_hop,
    const fips_routing_view *rv, fips_writer *bytes, fips_route_outcome *out, fips_proto_error *error);
FIPS_CORE_API fips_status fips_router_synth_routing_error(fips_router *router, const fips_node_addr *dest,
    const fips_node_addr *toward, const fips_node_addr *my_addr, const fips_routing_view *rv,
    fips_instant now, uint8_t default_ttl, fips_writer *bytes, fips_error_synth *out, fips_proto_error *error);
FIPS_CORE_API fips_status fips_router_synth_mtu_exceeded(fips_router *router, const fips_node_addr *dest,
    const fips_node_addr *toward, const fips_node_addr *my_addr, uint16_t bottleneck_mtu,
    fips_instant now, uint8_t default_ttl, fips_writer *bytes, fips_error_synth *out, fips_proto_error *error);
#ifdef __cplusplus
}
#endif
#endif
