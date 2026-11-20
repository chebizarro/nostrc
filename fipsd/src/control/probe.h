#ifndef FIPSD_CONTROL_PROBE_H
#define FIPSD_CONTROL_PROBE_H
#include "control/protocol.h"
#include <fips/proto/probe/state.h>
/* Host-resolved display/address strings supplement the core stage records.
 * These are snapshots, not a probe runner or a job registry. */
typedef struct fipsd_control_probe_path {
    const json_t *our_coords, *their_coords;
    uint64_t our_depth, their_depth, lca_depth, hops_up, hops_down, distance;
    bool same_root;
    const char *lca;
    const char *next_hop_addr, *next_hop_display_name, *next_hop_class;
    bool direct_peer, leaves_tree_walk;
} fipsd_control_probe_path;
typedef struct fipsd_control_probe_report {
    const fips_probe_snapshot *snapshot;
    uint64_t probe_id, elapsed_ms;
    const char *npub, *node_addr, *display_name, *ipv6_addr;
    const fipsd_control_probe_path *path;
    const uint64_t *attempt_timeouts_secs;
    size_t timeout_count;
    const uint8_t *attempts;
} fipsd_control_probe_report;
json_t *fipsd_control_probe_render(const fipsd_control_probe_report *report);
#endif
