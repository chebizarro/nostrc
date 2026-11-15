#ifndef FIPSD_PEER_DATA_H
#define FIPSD_PEER_DATA_H
#include <fips/proto/bloom/core.h>
#include <fips/proto/stp/declaration.h>
#include <fips/proto/coord.h>
#include <fips/transport/types.h>

/* Explicit host bound, not a wire-format restriction. Oversized tree updates
 * fail unchanged; never truncate an authenticated coordinate. */
#define FIPSD_PEER_COORD_CAPACITY 64u
typedef struct fipsd_peer_data {
    bool has_tree;
    fips_parent_declaration declaration;
    fips_coord_entry ancestry[FIPSD_PEER_COORD_CAPACITY];
    size_t ancestry_count;
    uint64_t tree_announce_min_interval_ms, last_tree_announce_sent_ms;
    bool has_tree_announce_sent, pending_tree_announce;
    bool has_filter, pending_filter_update;
    fips_bloom_filter inbound_filter;
    uint64_t filter_sequence, filter_received_at;
    fips_link_stats link_stats;
    uint64_t authenticated_at, last_seen, replay_total, replay_base, prev_frames;
    fips_instant promoted_at, heard_at, heartbeat_sent, heartbeat_attempt;
    bool heard, frame_heard;
} fipsd_peer_data;
void fipsd_peer_data_init(fipsd_peer_data *d, uint64_t wall_ms, fips_instant now, bool heard);
bool fipsd_peer_data_tree(fipsd_peer_data *d, const fips_parent_declaration *declaration,
    const fips_coord_entry *ancestry, size_t count);
bool fipsd_peer_data_tree_due(const fipsd_peer_data *d, uint64_t mono_ms);
void fipsd_peer_data_tree_sent(fipsd_peer_data *d, uint64_t mono_ms);
void fipsd_peer_data_filter(fipsd_peer_data *d, const fips_bloom_filter *filter,
    uint64_t sequence, uint64_t wall_ms);
bool fipsd_peer_data_filter_stale(const fipsd_peer_data *d, uint64_t wall_ms, uint64_t threshold_ms);
#endif
