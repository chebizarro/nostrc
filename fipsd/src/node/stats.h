#ifndef FIPSD_NODE_STATS_H
#define FIPSD_NODE_STATS_H
#include <fips/node/stats_history.h>
#include "node/stats_view.h"
typedef struct handler_counters {
    uint64_t hs_msg1, hs_msg2, hs_rejected, hs_rate_limited, hs_promoted, hs_restart, hs_timeouts, hs_failed, hs_cross_swap, hs_cross_keep;
    uint64_t frames_received, frames_delivered, frames_dropped, frames_sent, send_failed, decrypt_failed, replay;
    uint64_t tree_received, tree_accepted, tree_sent, tree_switches, tree_losses, tree_dropped, tree_sign_failed, tree_rate_limited;
    uint64_t bloom_received, bloom_accepted, bloom_sent, bloom_refused, bloom_send_failed;
    uint64_t lookup_req_received, lookup_req_initiated, lookup_req_forwarded, lookup_req_target_is_us, lookup_req_duplicate;
    uint64_t lookup_req_ttl_exhausted, lookup_req_no_tree_peer, lookup_req_rate_limited, lookup_req_backoff_suppressed, lookup_req_bloom_miss;
    uint64_t lookup_resp_received, lookup_resp_accepted, lookup_resp_forwarded, lookup_resp_unsolicited, lookup_resp_proof_failed, lookup_resp_timed_out, lookup_decode_error;
    uint64_t fwd_received_packets, fwd_received_bytes, fwd_delivered_packets, fwd_delivered_bytes, fwd_forwarded_packets, fwd_forwarded_bytes;
    uint64_t fwd_originated_packets, fwd_originated_bytes, fwd_drop_no_route_packets, fwd_drop_no_route_bytes, fwd_ttl_exhausted_packets, fwd_decode_error_packets;
    uint64_t fwd_route_direct_peer, fwd_route_tree_up, fwd_route_tree_down;
    uint64_t sess_setup_received, sess_setup_rate_limited, sess_established, sess_closed, sess_rekeys, sess_data_in, sess_data_out, sess_decode_error;
    uint64_t mmp_sr_sent, mmp_rr_sent, mmp_sr_received, mmp_rr_received, heartbeats_sent, links_dead;
    uint64_t rekey_installed, rekey_abandoned, rekey_rejected, rekey_restart, rekey_teardown;
    uint64_t path_probes_in, path_acks_in, path_closes_in, path_sent, path_switched;
    uint64_t notices_dropped, commands_refused, commands_cancelled, native_delivered;
} handler_counters;
struct fipsd_node_handlers;
bool hs_stats_mesh(struct fipsd_node_handlers *, uint64_t *);
void hs_stats_tick(struct fipsd_node_handlers *, fips_instant);
#endif
