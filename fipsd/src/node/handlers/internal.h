#ifndef FIPSD_NODE_HANDLERS_INTERNAL_H
#define FIPSD_NODE_HANDLERS_INTERNAL_H
#include "node/handlers/handlers.h"
#include "peer/link.h"
#include "peer/table.h"
#include "peer/identities.h"
#include "host_os.h"
#include "node/dataplane/dataplane.h"
#include "log.h"
#include "node/acl.h"
#include "node/metrics.h"
#include "node/stats.h"
#include "node/diag.h"
#include <fips/proto/fmp/mod.h>
#include <fips/proto/fmp/rekey.h>
#include <fips/proto/fsp/mod.h>
#include <fips/proto/fsp/rekey.h>
#include <fips/proto/stp/mod.h>
#include <fips/proto/bloom/mod.h>
#include <fips/proto/lookup/mod.h>
#include <fips/proto/routing/mod.h>
#include <fips/proto/mmp/mod.h>
#include <fips/proto/path/mod.h>
#include <fips/proto/probe/mod.h>
#include <fips/node/session.h>
#include <fips/node/rate_limit.h>
#include <fips/node/stats_history.h>
#include <fips/cache/coord_cache.h>
#include <fips/identity/key.h>
#include <fips/identity/node_addr.h>
#include <fips/identity/address.h>
#include <fips/identity/encoding.h>
#include "control/protocol.h"
#include "select.h"
#include <jansson.h>
#include <sodium.h>
#include <string.h>
#include <stdlib.h>

#define HMOD "node"
#define FIPSD_HANDLERS_WIRE 2048u
#define FIPSD_HANDLERS_PENDING_MAX 16u
#define FIPSD_HANDLERS_COORD_ADDRS 80u
#define FIPSD_HANDLERS_PROBE_JOBS FIPS_PROBE_MAX_CONCURRENT_PROBES
#define FIPSD_HANDLERS_DATAGRAM_MAX 1400u

typedef struct fipsd_node_handlers handlers;
/* Pending outbound handshake leg (upstream PeerMachine + ConnectionState +
 * pending_outbound index). The Noise initiator state lives in the IK profile's
 * handshake owner until msg2 completes it into the link machine. */
typedef struct pending_leg {
    bool used, hs_live, has_ticket, configured;
    fipsd_peer_machine machine;
    fips_rekey_hs hs;
    fips_rekey_index_lease lease;
    fips_peer_identity expected;
    fips_transport_id transport;
    fips_transport_addr addr;
    fips_link_id link;
    fipsd_peer_ticket ticket;
    fips_instant started;
} pending_leg;
/* H1: a known peer's authenticated msg1 in another startup epoch is held here
 * instead of evicting the live link. Host-only: never a peer-table member, a
 * published dataplane epoch or an index-map binding. One candidate per live
 * peer, hs_limiter.max_pending node-wide, released at the hold deadline. */
#define FIPSD_RESTART_HISTORY 64u
typedef struct restart_candidate {
    bool used, replace;
    fips_rekey_completed done;   /* keys derived from our fresh msg2 ephemeral */
    fips_rekey_index_lease lease; /* candidate receive index, unbound until confirmed */
    fips_rekey_wire msg1, msg2;  /* exact-retransmit match, resend and setup record */
    uint8_t ephemeral[32];
    fips_transport_id transport;
    fips_transport_addr addr;
    fips_link_id link;
    fips_instant deadline;
} restart_candidate;
/* Host record beside each table member (dispatcher-only). */
typedef struct peer_rec {
    bool used, path_live, tree_pending, filter_pending, heard, disconnecting;
    fips_node_addr addr;
    fipsd_active_peer *peer;
    restart_candidate restart;
    fips_path_bundle *path;
    fips_instant last_heard, last_tree_announce, established_at;
    uint64_t tree_sent, tree_received, frames_in, frames_out, bytes_in, bytes_out;
    uint64_t announce_sequence;
    bool has_announce_counter;
    uint64_t announce_counter;
    fips_mmp_link_evidence evidence;
    bool has_rr;
    uint32_t rtt_ms;
    bool has_rtt;
} peer_rec;
typedef struct index_entry { bool used; uint32_t index; fips_rekey_transport transport; fips_node_addr peer; } index_entry;
/* L4 (nostrc-f66sr.10) plaintext ownership. A user datagram is copied, never
 * shared, so each copy has one owner that wipes it (hs_wipe) at the end of its
 * own tenure and never earlier: the HCMD_SEND flexible payload until
 * command_free; a session_ext.pending or held[].items slot until it is
 * consumed, dropped as oldest, expired, closed with its session or freed at
 * teardown; the dispatcher scratch (send_data's body, h->session_plain) until
 * fipsd_dp_seal has copied it into a pooled packet; an inbound DataPacket is
 * copied from the packet into a fipsd_node_notice its consumer releases with
 * fipsd_node_notice_free. Borrowed, not ours: the pooled fipsd_dp_packet slab a
 * worker holds from submit until the dispatcher destroys the completed job
 * (only its recycle wipes it), which is also the plaintext handed to
 * hs_session_message. Slots at or beyond pending_len/len are always zero. */
typedef struct pending_datagram { uint16_t src_port, dst_port; size_t len; uint8_t bytes[FIPSD_HANDLERS_DATAGRAM_MAX]; } pending_datagram;
/* Which owner finished with its plaintext copy (test observation only). */
typedef enum fipsd_wipe_event {
    FIPSD_WIPE_PENDING_CONSUMED, FIPSD_WIPE_PENDING_DROPPED, FIPSD_WIPE_PENDING_CLOSED,
    FIPSD_WIPE_HELD_CONSUMED, FIPSD_WIPE_HELD_DROPPED, FIPSD_WIPE_HELD_EXPIRED,
    FIPSD_WIPE_SCRATCH, FIPSD_WIPE_COMMAND, FIPSD_WIPE_NOTICE, FIPSD_WIPE_TEARDOWN,
    FIPSD_WIPE_COUNT
} fipsd_wipe_event;
#ifdef FIPSD_TEST_HOOKS
/* Test seam, compiled only into a test's own copy of handlers.c/session.c:
 * observes each wipe after it happened and whether the bytes were non-zero.
 * `h` is NULL for owners released off the dispatcher (commands, notices). */
typedef void (*fipsd_wipe_hook)(void *user, handlers *h, fipsd_wipe_event ev, const uint8_t *bytes, size_t size, bool was_dirty);
extern fipsd_wipe_hook fipsd_test_wipe_hook;
extern void *fipsd_test_wipe_user;
#endif
static inline void hs_wipe(handlers *h, fipsd_wipe_event ev, void *bytes, size_t size)
{
#ifdef FIPSD_TEST_HOOKS
    bool dirty = size && !sodium_is_zero(bytes, size);
#endif
    sodium_memzero(bytes, size);
#ifdef FIPSD_TEST_HOOKS
    if (fipsd_test_wipe_hook) fipsd_test_wipe_hook(fipsd_test_wipe_user, h, ev, bytes, size, dirty);
#else
    (void)h; (void)ev;
#endif
}
/* Host state beside each session table entry. */
typedef struct session_ext {
    bool used, established, mmp_live, has_dest_coords, is_initiator;
    fips_node_addr peer;
    fips_mmp_session_state mmp;
    fips_mmp_owd_sample owd[64];
    pending_datagram *pending;
    size_t pending_len, pending_capacity;
    fips_node_addr dest_coords[FIPSD_HANDLERS_COORD_ADDRS];
    size_t dest_count;
    fips_node_addr carrier; /* last authenticated arrival link peer */
    fips_instant created, last_data;
    uint64_t sent, received, rekeys;
} session_ext;
typedef struct configured_peer {
    bool used;
    fips_peer_identity identity;
    fips_transport_id transport;
    fips_transport_addr addr;
    fipsd_peer_retry retry;
    bool dialing;
} configured_peer;
typedef struct probe_job {
    bool used;
    uint64_t id;
    fips_node_addr target;
    fips_probe *probe; /* heap: ~4 MiB with path facts */
    fips_probe_snapshot *report;
    uint64_t finished_ms;
    /* probe.rs ProbeJob: the traffic baseline taken when the session this job
     * opened was established, and the host's refusal of the core's teardown
     * (reported as `session_left_intact`, never as a teardown that did not
     * happen). */
    bool has_baseline, teardown_refused;
    uint64_t sent_at_establish, received_at_establish;
    fips_instant last_data_at_establish;
    fips_probe_left_intact refused_reason;
} probe_job;

typedef enum handler_command_kind {
    HCMD_CONNECT, HCMD_DISCONNECT, HCMD_SEND, HCMD_LEARN_IDENTITY, HCMD_DIAL_CONFIGURED,
    HCMD_CONTROL /* live control command with a shared execution ticket */
} handler_command_kind;
/* Shared by a queued HCMD_CONTROL and its waiting connection fiber (handlers.c).
 * The dispatcher claims it before any state mutation; the waiter abandons it at
 * its execution deadline or shutdown. The ticket owns the JSON reply. */
typedef struct hs_control_ticket hs_control_ticket;
typedef struct handler_command {
    handler_command_kind kind;
    fips_peer_identity identity;
    fips_node_addr node;
    fips_transport_id transport;
    fips_transport_addr addr;
    uint16_t src_port, dst_port;
    char name[32];
    json_t *params;
    hs_control_ticket *ticket; /* HCMD_CONTROL: one of the ticket's two references */
    size_t len;
    uint8_t payload[];
} handler_command;

struct fipsd_node_handlers {
    fipsd_node_handlers_config cfg;
    /* Attached (dispatcher fiber) */
    bool attached, running;
    const fipsd_node_context *ctx;
    fipsd_peer_dispatcher *peers;
    const fipsd_transport_registry *registry;
    fipsd_runtime *runtime;
    GoContext *cancel;
    fips_clock clock;
    fips_entropy entropy;
    const fips_identity *identity;
    fips_secp *secp;
    fips_node_addr me;
    fips_peer_key my_key;
    uint8_t epoch[8];
    char npub[FIPS_NIP19_CAPACITY];
    fips_rekey_node_keys local;
    fips_rekey_tag_source tags;
    fips_index_allocator indices;
    uint32_t *index_storage;
    fips_rekey_params link_params, session_params;
    fips_instant origin, started_at, next_tick;
    uint64_t wall_offset_ms; /* wall clock at origin */
    uint64_t next_link_id;
    /* Distinct buffers per nesting level: the inbound frame (decrypted in place),
     * the executor's wire assembly for links and sessions, datagram wrapping,
     * link sealing, router output and the rekey Env scratch. */
    uint8_t in_wire[FIPSD_HANDLERS_WIRE], link_exec_wire[FIPSD_HANDLERS_WIRE], session_exec_wire[FIPSD_HANDLERS_WIRE];
    uint8_t dg_buf[FIPSD_HANDLERS_WIRE], link_plain[FIPSD_HANDLERS_WIRE], link_wire[FIPSD_HANDLERS_WIRE];
    uint8_t route_buf[FIPSD_HANDLERS_WIRE], scratch[FIPSD_HANDLERS_WIRE], msg_buf[FIPSD_HANDLERS_WIRE];
    uint8_t tx_msg[FIPSD_HANDLERS_WIRE], session_frame[FIPSD_HANDLERS_WIRE], session_plain[FIPSD_HANDLERS_WIRE];
    uint8_t session_wire[FIPSD_HANDLERS_WIRE], lookup_bytes[FIPSD_HANDLERS_WIRE];
    fips_bloom_action *bloom_actions;    /* peer_capacity + 1 */
    fips_path_output *path_outputs;      /* FIPS_PATH_OUTPUTS_MAX */
    fips_probe_path_facts *probe_facts;
    /* Output lists: a machine's outputs are executed to completion before any
     * nested machine step; deliveries are deferred past the execute call. */
    fips_rekey_outputs link_out, session_out, step_out;
    /* Peers and pending legs */
    peer_rec *recs;
    pending_leg *legs;
    index_entry *index_map;
    size_t index_map_capacity;
    configured_peer *configured;
    fips_handshake_rate_limiter hs_limiter;
    fips_fmp_silent_sessions silent;
    fips_fmp_silent_record *silent_storage;
    /* Supplementary restart memory: epochs a peer has proven it moved past and
     * msg1 ephemerals whose hold expired unconfirmed (refused for one further
     * hold window, then challengeable again). Bounded, overwritten in order,
     * and never a freshness proof by itself. */
    size_t restart_pending, retired_next;
    struct {
        bool used, has_epoch;
        fips_node_addr peer;
        uint8_t value[32]; /* opaque epoch (8 bytes) or msg1 ephemeral public key */
        fips_instant until; /* ephemeral entries only; no ops means no expiry */
    } retired[FIPSD_RESTART_HISTORY];
    /* Tree */
    fips_tree_state *tree;
    fips_stp_peer *tree_peers;
    fips_instant last_reeval;
    fips_node_addr my_coord_addrs[FIPSD_HANDLERS_COORD_ADDRS];
    size_t my_coord_count;
    fips_tree_announce *announce_scratch;
    fips_tree_coordinate *coord_scratch;
    /* Bloom */
    fips_bloom_state bloom;
    fips_bloom_peer *bloom_peers;
    fips_node_addr *leaves;
    uint64_t bloom_floor;
    fips_bloom_filter filter_scratch;
    /* Lookup, coordinate and identity caches */
    fips_lookup lookup;
    fips_recent_request *recent;
    fips_pending_lookup *pending_lookups;
    fips_lookup_backoff_entry *backoff_entries;
    fips_lookup_forward_entry *forward_entries;
    fips_lookup_sign_limiter sign_limiter;
    fips_sign_entry *sign_entries;
    fips_lookup_request *request_scratch;
    fips_lookup_response *response_scratch;
    fips_coord_cache cache;
    fips_coord_slot *cache_slots;
    uint32_t *cache_index;
    fipsd_identity_cache identities;
    fipsd_identity_entry *identity_entries;
    fips_router router;
    /* Sessions */
    fips_session_table sessions;
    fips_session_entry *session_entries;
    session_ext *session_ext;
    fips_session_setup_limiter setup_limiter;
    fips_setup_entry *setup_entries;
    struct { bool used; fips_node_addr dest; pending_datagram *items; size_t len; fips_instant since; } *held; /* awaiting lookup */
    /* Probes */
    probe_job probes[FIPSD_HANDLERS_PROBE_JOBS];
    uint64_t next_probe_id;
    /* Diagnostics */
    handler_counters counters;
    fipsd_metrics metrics;
    fipsd_diag_budget diag;
    fipsd_dataplane *dataplane; /* .7: bounded jobs + immutable routing/epoch tables */
    fips_stats_history *history;
    fips_peer_stats *history_peers;
    fips_peer_snapshot *history_samples;
    /* Control */
    GoSwap *snapshot;
    bool dirty;
    GoChannel *commands, *notices;
    uint64_t snapshot_generation;
    /* Native datagram API sink (handlers.h); loaded on every delivery. */
    _Atomic(GoChannel *) native_sink;
};

/* handlers.c */
peer_rec *hs_rec_find(handlers *h, const fips_node_addr *addr);
peer_rec *hs_rec_by_peer(handlers *h, const fipsd_active_peer *peer);
peer_rec *hs_rec_alloc(handlers *h, fipsd_active_peer *peer);
void hs_rec_free(handlers *h, peer_rec *rec);
fips_instant hs_now(handlers *h);
uint64_t hs_now_ms(handlers *h);     /* monotonic millis since origin */
uint64_t hs_wall_ms(handlers *h);    /* wall clock millis */
uint64_t hs_wall_secs(handlers *h);
uint64_t hs_elapsed_ms(fips_instant now, fips_instant earlier);
uint64_t hs_until_ms(fips_instant at, fips_instant now); /* UINT64_MAX absent */
uint64_t hs_min_wait(uint64_t a, fips_instant at, fips_instant now);
void hs_notice(handlers *h, fipsd_notice_kind kind, const fips_node_addr *peer, const uint8_t *payload, size_t len);
void hs_notice_full(handlers *h, const fipsd_node_notice *notice, const uint8_t *payload, size_t len);
fipsd_transport *hs_transport(handlers *h, fips_transport_id id);
bool hs_peer_identity(handlers *h, const fips_peer_key *key, fips_peer_identity *out);
void hs_mark_dirty(handlers *h);
bool hs_acl_authorize(handlers *, const fips_peer_identity *, const char *context);
/* handshake.c */
bool hs_handshake_frame(handlers *h, fipsd_transport *t, const fips_transport_addr *from, uint8_t *frame, size_t len, int phase);
bool hs_dial(handlers *h, const fips_peer_identity *peer, fips_transport_id transport, const fips_transport_addr *addr, bool configured);
bool hs_disconnect(handlers *h, const fips_node_addr *peer, fips_fmp_disconnect_reason reason, bool notify_peer);
bool hs_leg_effects(handlers *h, pending_leg *leg, const fipsd_peer_actions *actions);
bool hs_peer_effects(handlers *h, fipsd_active_peer *peer, const fipsd_peer_actions *actions);
uint64_t hs_legs_wait_ms(handlers *h, fips_instant now);
bool hs_legs_poll(handlers *h, fips_instant now);
void hs_remove_peer(handlers *h, peer_rec *rec, fips_rekey_close why, bool notify_peer, fips_fmp_disconnect_reason reason);
void hs_close_all(handlers *h);
bool hs_dial_configured(handlers *h);
void hs_configured_poll(handlers *h, fips_instant now);
/* H1 eviction gate: candidate frames use the ordinary worker pool under a
 * clone of the candidate's keys, never the live link's; the result on the
 * dispatcher is the only place the incumbent is closed for a restart. */
bool hs_restart_frame(handlers *, fipsd_transport *, const fips_transport_addr *, const uint8_t *, size_t, const fips_fmp_encrypted_header *);
bool hs_restart_result(handlers *, const fipsd_crypto_job *);
/* rekey.c */
typedef struct link_exec_result { bool teardown, replace, restart, delivered; size_t begin, end; uint64_t counter; } link_exec_result;
void hs_env(handlers *h, fips_rekey_env *env, fips_rekey_transport transport);
void hs_release_outputs(handlers *h, const fips_node_addr *peer, fips_rekey_outputs *out);
bool hs_link_execute(handlers *h, peer_rec *rec, const fips_transport_addr *arrival, uint8_t *input, size_t len, link_exec_result *result);
bool hs_link_step(handlers *h, peer_rec *rec, fips_rekey_input input, const fips_transport_addr *arrival, uint8_t *frame, size_t len, link_exec_result *result);
bool hs_link_send(handlers *h, peer_rec *rec, const uint8_t *message, size_t len);
bool hs_link_send_to(handlers *h, peer_rec *rec, fips_transport_id transport, const fips_transport_addr *to, const uint8_t *message, size_t len);
bool hs_link_frame(handlers *h, peer_rec *rec, fipsd_transport *t, const fips_transport_addr *from, uint8_t *frame, size_t len);
bool hs_session_execute(handlers *h, fips_session_entry *entry, const uint8_t *input, size_t len);
void hs_index_bind(handlers *h, const fips_node_addr *peer, fips_rekey_transport transport, uint32_t index);
void hs_index_unbind(handlers *h, fips_rekey_transport transport, uint32_t index);
peer_rec *hs_index_lookup(handlers *h, fips_rekey_transport transport, uint32_t index);
/* A multipath-enabled Bundle resolves receive indices from any ingress transport. */
static inline bool hs_rec_multipath(const peer_rec *rec)
{ return rec && rec->path_live && rec->path->params.cfg.enabled; }
void hs_index_forget_peer(handlers *h, const fips_node_addr *peer);
void hs_link_message(handlers *h, peer_rec *rec, fipsd_transport *t, const fips_transport_addr *from, uint32_t timestamp_ms, const uint8_t *message, size_t len);
/* tree.c */
bool hs_tree_init(handlers *h);
void hs_tree_peer_added(handlers *h, peer_rec *rec);
void hs_tree_peer_removed(handlers *h, peer_rec *rec);
void hs_tree_announce_received(handlers *h, peer_rec *rec, const uint8_t *body, size_t len);
bool hs_tree_send_announce(handlers *h, peer_rec *rec, bool force);
void hs_tree_tick(handlers *h, fips_instant now);
bool hs_tree_is_parent(handlers *h, const fips_node_addr *peer);
bool hs_tree_is_child(handlers *h, const fips_node_addr *peer);
const fips_tree_coordinate *hs_tree_peer_coords(handlers *h, const fips_node_addr *peer);
void hs_tree_refresh_my_addrs(handlers *h);
void hs_tree_ranking(handlers *h, fips_stp_peer_cost *costs, size_t capacity, fips_stp_ranking *out);
/* bloom.c */
bool hs_bloom_init(handlers *h);
void hs_bloom_peer_added(handlers *h, peer_rec *rec);
void hs_bloom_peer_removed(handlers *h, peer_rec *rec);
void hs_bloom_filter_received(handlers *h, peer_rec *rec, const uint8_t *body, size_t len);
void hs_bloom_roles(handlers *h);
void hs_bloom_refresh(handlers *h, fips_instant now, bool force_all);
void hs_bloom_rr(handlers *h, peer_rec *rec);
const fips_bloom_filter *hs_bloom_peer_filter(handlers *h, const fips_node_addr *peer);
bool hs_bloom_may_reach(handlers *h, const fips_node_addr *peer, const fips_node_addr *target);
/* lookup.c */
bool hs_lookup_init(handlers *h);
void hs_lookup_request_received(handlers *h, peer_rec *rec, const uint8_t *body, size_t len);
void hs_lookup_response_received(handlers *h, peer_rec *rec, const uint8_t *body, size_t len);
bool hs_lookup_initiate(handlers *h, const fips_node_addr *target);
void hs_lookup_tick(handlers *h, fips_instant now);
bool hs_cache_lookup(handlers *h, const fips_node_addr *dest, fips_node_addr *addrs, size_t capacity, size_t *count, uint16_t *path_mtu);
const fips_tree_coordinate *hs_cache_coords(void *ctx, const fips_node_addr *dest, fips_instant now);
/* session.c */
bool hs_session_init(handlers *h);
void hs_session_poll(handlers *h, fips_instant now);
bool hs_session_open(handlers *h, const fips_node_addr *dest);
bool hs_session_send_message(handlers *h, fips_session_entry *entry, session_ext *ext, uint8_t msg_type, const uint8_t *body, size_t len);
void hs_session_datagram_received(handlers *h, peer_rec *rec, const uint8_t *body, size_t len);
bool hs_session_send(handlers *h, const fips_node_addr *dest, uint16_t src_port, uint16_t dst_port, const uint8_t *payload, size_t len);
void hs_session_tick(handlers *h, fips_instant now);
uint64_t hs_session_wait_ms(handlers *h, fips_instant now);
void hs_session_close_all(handlers *h);
void hs_session_peer_removed(handlers *h, const fips_node_addr *peer);
void hs_session_coords_resolved(handlers *h, const fips_node_addr *dest);
session_ext *hs_session_ext(handlers *h, const fips_session_entry *entry);
bool hs_route_datagram(handlers *h, const fips_session_datagram *dg, const fips_node_addr *exclude, bool incoming_ce);
void hs_session_message(handlers *h, fips_session_entry *entry, const uint8_t *plaintext, size_t len);
void hs_routing_signal(handlers *h, peer_rec *rec, const fips_node_addr *src, const uint8_t *frame, size_t len);
/* mmp.c */
void hs_mmp_peer_added(handlers *h, peer_rec *rec);
void hs_mmp_frame_received(handlers *h, peer_rec *rec, uint64_t counter, uint32_t sender_ts, size_t bytes, bool ce);
void hs_mmp_frame_sent(handlers *h, peer_rec *rec, uint64_t counter, uint32_t timestamp, size_t bytes);
void hs_mmp_report_received(handlers *h, peer_rec *rec, fips_link_message_type type, const uint8_t *body, size_t len);
void hs_mmp_tick(handlers *h, fips_instant now);
bool hs_mmp_srtt(handlers *h, const peer_rec *rec, double *out);
double hs_mmp_link_cost(handlers *h, const peer_rec *rec);
void hs_mmp_session_message(handlers *h, fips_session_entry *entry, session_ext *ext, uint8_t msg_type, const uint8_t *body, size_t len);
void hs_mmp_session_tick(handlers *h, fips_instant now);
/* path.c */
void hs_path_peer_added(handlers *h, peer_rec *rec);
void hs_path_peer_removed(handlers *h, peer_rec *rec);
void hs_path_message(handlers *h, peer_rec *rec, fipsd_transport *t, const fips_transport_addr *from, fips_link_message_type type, const uint8_t *body, size_t len);
void hs_path_account(handlers *h, peer_rec *rec, fipsd_transport *t, const fips_transport_addr *from, size_t len, uint8_t msg_type);
uint64_t hs_path_wait_ms(handlers *h, fips_instant now);
void hs_path_poll(handlers *h, fips_instant now);
void hs_path_network_event(handlers *h, const fipsd_netmon_event *e);
json_t *hs_path_show(handlers *h, const fips_node_addr *peer);
json_t *hs_path_pin(handlers *h, const fips_node_addr *peer, fips_transport_id transport, const fips_transport_addr *addr, bool has_addr, bool pin);
/* probe.c */
json_t *hs_probe_start(handlers *h, const fips_peer_identity *target);
json_t *hs_probe_poll(handlers *h, uint64_t id);
json_t *hs_probe_cancel(handlers *h, uint64_t id);
void hs_probe_tick(handlers *h, fips_instant now);
void hs_probe_close_all(handlers *h);
/* timeout.c */
uint64_t hs_wait_ms(handlers *h, fips_instant now);
bool hs_poll(handlers *h, fips_instant now);
void hs_tick(handlers *h, fips_instant now);
/* native.c / netmon.c (Tier B4 seams) */
void hs_native_deliver(handlers *h, const fips_node_addr *src, uint16_t src_port, uint16_t dst_port, const uint8_t *payload, size_t len);
void hs_host_link_mtu_changed(handlers *h, uint16_t mtu);
void hs_netmon_event(handlers *h, const fipsd_netmon_event *e);
/* provider.c */
bool hs_provider_init(handlers *h);
void hs_publish(handlers *h, fips_instant now);
json_t *hs_control_command(handlers *h, const char *name, const json_t *params);
#endif
