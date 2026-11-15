#ifndef FIPSD_CONTROL_SNAPSHOT_H
#define FIPSD_CONTROL_SNAPSHOT_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <jansson.h>
#include <refptr.h>
#include <context.h>
#include <fips/node/stats_history.h>

/* Immutable read-side projections, mirroring upstream control/snapshot.rs.
 * A provider publishes these through GoSwap (or an equivalent retained load).
 * Strings, arrays and nested projections are borrowed for the snapshot's life.
 * Never point these at mutable dispatcher state. No locks cross serialization.
 * JSON-valued projections below contain data only, never response envelopes:
 * they are the extensible counter/row projections produced by later owners.
 * NULL tables mean empty; NULL optional scalars mean unavailable, not zero.
 */
typedef struct fipsd_control_counter { const char *name; uint64_t value; } fipsd_control_counter;
typedef struct fipsd_control_counters {
    const fipsd_control_counter *items;
    size_t count;
} fipsd_control_counters;
typedef struct fipsd_control_interface {
    const char *name, *presence, *policy;
    bool carrier;
    uint64_t since_secs, binds;
    uint32_t failed_attempts;
} fipsd_control_interface;
typedef struct fipsd_control_transport {
    uint32_t transport_id;
    const char *type, *state, *name, *local_addr, *tor_mode, *onion_address;
    uint16_t mtu;
    const json_t *tor_monitoring;
    fipsd_control_counters stats;
    const fipsd_control_interface *interface;
} fipsd_control_transport;
typedef struct fipsd_control_cache_entry {
    const char *node_addr, *display_name;
    const json_t *coord_path;
    uint64_t depth, created_at, last_used_ms;
    const uint16_t *path_mtu;
} fipsd_control_cache_entry;
typedef struct fipsd_control_identity {
    const char *node_addr, *npub, *display_name, *ipv6_addr;
    uint64_t last_seen_ms;
} fipsd_control_identity;
typedef struct fipsd_control_lookup {
    const char *target, *display_name;
    uint64_t initiated_ms, last_sent_ms;
    uint8_t attempt;
} fipsd_control_lookup;
typedef struct fipsd_control_connection {
    uint64_t link_id, started_at_ms, last_activity_ms;
    const char *direction, *handshake_state, *expected_peer;
    uint32_t resend_count;
} fipsd_control_connection;
typedef struct fipsd_control_native_flow {
    uint64_t flow_id, queued, since_ms;
    const char *peer, *peer_addr;
    uint16_t local_port, remote_port;
    bool established;
} fipsd_control_native_flow;
typedef struct fipsd_control_native_listener { uint16_t local_port; size_t backlog; } fipsd_control_native_listener;
typedef struct fipsd_control_peer_meta {
    uint8_t node_addr[16];
    const char *npub, *display_name;
    bool is_active;
} fipsd_control_peer_meta;

typedef struct fipsd_control_snapshot {
    /* NULL npub: no identity has been constructed. show_status must refuse. */
    const char *npub, *node_addr, *ipv6_addr, *state;
    const char *version, *exe_path, *control_socket;
    uint64_t pid, uptime_secs, now_ms;
    fips_instant now;
    bool is_leaf_only, persistent, is_root;
    const uint64_t *estimated_mesh_size;
    uint64_t peer_count, session_count, link_count, transport_count, connection_count;
    const json_t *transport_peer_counts;
    const fips_stats_history *history;
    const fipsd_control_peer_meta *peer_meta;
    size_t peer_meta_count;

    struct {
        const char *my_node_addr, *root, *root_npub, *parent, *parent_display_name;
        bool is_root, declaration_signed;
        uint64_t depth, declaration_sequence, peer_tree_count;
        const json_t *my_coords, *peers;
    } tree;
    struct {
        const char *own_node_addr;
        bool is_leaf_only;
        uint64_t sequence;
        const json_t *leaf_dependents, *peer_filters;
        const double *uptree_fill_ratio, *uptree_estimated_count;
    } bloom;
    struct {
        size_t count, max_entries, expired;
        double fill_ratio;
        uint64_t default_ttl_ms, avg_age_ms;
        const fipsd_control_cache_entry *entries;
        size_t entry_count;
    } cache;
    const fipsd_control_identity *identities;
    size_t identity_count, identity_max_entries;
    const fipsd_control_lookup *lookups;
    size_t lookup_count;
    uint64_t pending_native_destinations, pending_native_datagrams;
    uint64_t pending_tun_destinations, pending_tun_packets, recent_requests;
    const json_t *retries;

    /* Entity owners project conditional fields (noise/MMP/rekey/path/identity)
     * into immutable rows, using upstream snapshot.rs field names. The control
     * layer takes a deep copy, never steals or changes a provider's objects.
     * Time-dependent connection/cache/native rows remain typed above/below. */
    const json_t *peers, *sessions, *links, *mmp_peers, *mmp_sessions;
    const fipsd_control_connection *connections;
    size_t connections_count;
    const fipsd_control_transport *transports;
    size_t transports_count;
    const fipsd_control_native_flow *flows;
    size_t flow_count;
    const fipsd_control_native_listener *listeners;
    size_t listener_count;
    struct {
        const char *allow_file, *deny_file, *effective_mode, *default_decision;
        bool enforcement_active, allow_all, deny_all;
        const json_t *allow_file_entries, *deny_file_entries, *allow_entries, *deny_entries;
    } acl;
    fipsd_control_counters forwarding, lookup, tree_stats, bloom_stats, congestion;
    fipsd_control_counters errors, native, dataplane, handshake, session;
} fipsd_control_snapshot;

typedef struct fipsd_control_request_context {
    GoContext *shutdown; /* borrowed root; select alongside the reply */
    uint64_t deadline_ns; /* CLOCK_MONOTONIC, absolute execution deadline */
} fipsd_control_request_context;

typedef struct fipsd_control_provider {
    void *user;
    /* Return a retained fipsd_control_snapshot; empty ref = empty seed views.
     * Release after render. Publication must synchronize load AND retain. */
    GoRefPtr (*acquire)(void *user);
    /* Live commands run in connection fibers. Implementations must enqueue to
     * the single-writer owner and select its reply against cancellation, never
     * mutate node state here. Return an owned response, NULL if unavailable.
     * Any blocking work must use go_blocking_submit. */
    json_t *(*command)(void *user, const char *name, const json_t *params,
                       const fipsd_control_request_context *context);
} fipsd_control_provider;
#endif
