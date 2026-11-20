#ifndef FIPSD_PEER_MACHINE_H
#define FIPSD_PEER_MACHINE_H
#include <fips/proto/fmp/state.h>

/* Dispatcher-only, sans IO. Instants share a monotonic domain. Actions borrow
 * the machine/event bytes until the next call. No session/index ownership here. */
typedef enum fipsd_peer_state {
    FIPSD_PEER_DISCOVERED, FIPSD_PEER_CONNECTING, FIPSD_PEER_HANDSHAKING,
    FIPSD_PEER_AUTHORIZING, FIPSD_PEER_ESTABLISHED, FIPSD_PEER_ACTIVE,
    FIPSD_PEER_MAINTAINING, FIPSD_PEER_CLOSING, FIPSD_PEER_FAILED,
    FIPSD_PEER_CLOSED
} fipsd_peer_state;
typedef struct fipsd_peer_timing {
    uint64_t retransmit_ms, handshake_timeout_ms, heartbeat_ms, cooldown_ms;
    uint32_t max_resends;
    double resend_backoff;
} fipsd_peer_timing;
typedef enum fipsd_peer_event_kind {
    FIPSD_PEER_DIAL, FIPSD_PEER_CONNECTED, FIPSD_PEER_CONNECT_FAILED,
    FIPSD_PEER_SEND_FAILED, FIPSD_PEER_RESEND_RESULT,
    FIPSD_PEER_INBOUND_AUTHENTICATED, FIPSD_PEER_MSG2_AUTHENTICATED,
    FIPSD_PEER_AUTHORIZED, FIPSD_PEER_REJECTED, FIPSD_PEER_PROMOTED,
    FIPSD_PEER_HEARD, FIPSD_PEER_LINK_DEAD, FIPSD_PEER_DISCONNECT,
    FIPSD_PEER_POLL
} fipsd_peer_event_kind;
typedef struct fipsd_peer_event {
    fipsd_peer_event_kind kind;
    bool connection_oriented, success;
    size_t authenticated_bytes; /* HEARD: trusted receive result, not raw ingress */
    uint64_t wall_ms;
    fips_fmp_wire_outcome wire;
    fips_fmp_establish_snapshot inbound;
    fips_fmp_outbound_snapshot outbound;
    fips_fmp_promotion_result promotion;
} fipsd_peer_event;
typedef enum fipsd_peer_action_kind {
    FIPSD_PEER_OPEN_TRANSPORT, FIPSD_PEER_START_HANDSHAKE,
    FIPSD_PEER_SEND_HANDSHAKE, FIPSD_PEER_PROMOTE,
    FIPSD_PEER_INVALIDATE, FIPSD_PEER_TEARDOWN,
    FIPSD_PEER_LOST_HANDSHAKE, FIPSD_PEER_LOST_LINK,
    FIPSD_PEER_CROSS_SWAP, FIPSD_PEER_CROSS_KEEP,
    FIPSD_PEER_REKEY_INPUT, FIPSD_PEER_SEND_HEARTBEAT,
    FIPSD_PEER_POLL_LINK, /* host calls link_step(Poll) with its injected Env */
    FIPSD_PEER_SEND_DISCONNECT
} fipsd_peer_action_kind;
typedef struct fipsd_peer_action {
    fipsd_peer_action_kind kind;
    fips_fmp_bytes bytes;
} fipsd_peer_action;
#define FIPSD_PEER_ACTIONS_MAX 8u
typedef struct fipsd_peer_actions {
    fipsd_peer_action items[FIPSD_PEER_ACTIONS_MAX];
    size_t count;
} fipsd_peer_actions;
typedef struct fipsd_peer_machine {
    fipsd_peer_state state;
    fipsd_peer_timing timing;
    fips_fmp_connection_state conn;
    fips_instant timeout_at, resend_at, liveness_at, closed_until;
    bool resend_pending, send_failed;
} fipsd_peer_machine;
fipsd_peer_timing fipsd_peer_timing_default(void);
bool fipsd_peer_timing_valid(fipsd_peer_timing timing);
void fipsd_peer_machine_init(fipsd_peer_machine *m, fips_link_id link,
    fips_link_direction direction, const fips_peer_identity *expected,
    fips_fmp_leg_stamp at, fipsd_peer_timing timing);
void fipsd_peer_machine_step(fipsd_peer_machine *m, fips_instant now,
    const fipsd_peer_event *event, fipsd_peer_actions *out);
fips_instant fipsd_peer_machine_next_wake(const fipsd_peer_machine *m);
bool fipsd_peer_machine_established(const fipsd_peer_machine *m);

/* Durable across per-connection machines; the future reconciler owns this.
 * count=0 link-loss retry is base, first handshake failure is 2*base. */
typedef struct fipsd_peer_retry_policy {
    uint64_t base_ms, cap_ms, handshake_timeout_ms;
    uint32_t max_retries;
} fipsd_peer_retry_policy;
typedef struct fipsd_peer_retry {
    uint32_t count;
    bool scheduled, reconnect;
    fips_instant after, expires;
} fipsd_peer_retry;
void fipsd_peer_retry_init(fipsd_peer_retry *r);
bool fipsd_peer_retry_failed(fipsd_peer_retry *r, fipsd_peer_retry_policy p,
    fips_instant now, bool link_dead, bool configured, bool auto_reconnect,
    bool connected, bool suspended);
bool fipsd_peer_retry_take(fipsd_peer_retry *r, fipsd_peer_retry_policy p,
    fips_instant now, bool connected, bool admission_ok, bool suspended);
#endif
