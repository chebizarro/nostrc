#ifndef NOSTR_RELAY_PRIVATE_H
#define NOSTR_RELAY_PRIVATE_H

#include "go.h"
#include "nostr-envelope.h"
#include <stdbool.h>
#include <stdint.h>

/* Connection state enum - also defined in nostr-relay.h with same guard */
#ifndef NOSTR_RELAY_CONNECTION_STATE_DEFINED
#define NOSTR_RELAY_CONNECTION_STATE_DEFINED
typedef enum NostrRelayConnectionState_ {
    NOSTR_RELAY_STATE_DISCONNECTED = 0,
    NOSTR_RELAY_STATE_CONNECTING,
    NOSTR_RELAY_STATE_CONNECTED,
    NOSTR_RELAY_STATE_BACKOFF
} NostrRelayConnectionState;
#endif

/* Forward declaration */
struct NostrRelay;

/* Callback for connection state changes */
typedef void (*NostrRelayStateCallback)(struct NostrRelay *relay,
                                        NostrRelayConnectionState old_state,
                                        NostrRelayConnectionState new_state,
                                        void *user_data);

/* Callback for NIP-42 AUTH challenge received (nostrc-7og) */
typedef void (*NostrRelayAuthCallback)(struct NostrRelay *relay,
                                       const char *challenge,
                                       void *user_data);

struct _NostrRelayPrivate {
    nsync_mu mutex;

    GoContext *connection_context;
    CancelFunc connection_context_cancel;
    // cancel_func

    char *challenge;
    void (*notice_handler)(const char *);
    bool (*custom_handler)(const char *);
    GoHashMap *ok_callbacks; /* event_id -> NostrRelayOkWaiter* */
    GoChannel *write_queue;
    GoChannel *subscription_channel_close_queue;
    GoChannel *debug_raw; // optional: emits summary/raw strings for debugging
    GoChannel *reconnect_now; // buffered wake channel for immediate reconnect
    GoWaitGroup workers;
    /* Security: invalid signature tracker (pubkey->counters/bans). Impl in relay.c */
    void *invalid_sig_head;
    int invalid_sig_count;    /* current number of nodes in list */

    /* Reconnection with exponential backoff (nostrc-4du) */
    NostrRelayConnectionState connection_state;
    int reconnect_attempt;           /* Number of failed reconnection attempts */
    uint64_t backoff_ms;             /* Current backoff delay in milliseconds */
    uint64_t next_reconnect_time_ms; /* Absolute time (CLOCK_MONOTONIC ms) for next reconnect */
    bool auto_reconnect;             /* Enable/disable auto-reconnection (default: true) */
    bool reconnect_requested;        /* Signal to message_loop to trigger reconnect */

    /* fp-ieg8: who owns reconnection for this relay.
     *
     * The backoff/reconnect loop lives in message_loop, which nostr_relay_connect
     * only spawns AFTER a connection succeeds. A relay whose first dial failed
     * therefore has no loop and, before fp-ieg8, was never re-dialled by anyone.
     * The pool's redial worker fills that gap, and these two fields keep the two
     * mechanisms from fighting over the same relay:
     *
     *   message_loop_active - true while a message_loop is alive and owns
     *                         reconnection. The pool leaves such relays alone.
     *   dial_in_progress    - a claim held by whichever thread is inside a
     *                         connect. Two concurrent nostr_connection_new calls
     *                         on one relay would overwrite relay->connection and
     *                         leak the loser. */
    bool message_loop_active;
    bool dial_in_progress;

    /* State change callback */
    NostrRelayStateCallback state_callback;
    void *state_callback_user_data;

    /* NIP-42 AUTH challenge callback (nostrc-7og) */
    NostrRelayAuthCallback auth_callback;
    void *auth_callback_user_data;

    /* OK response callback — fires for every ["OK","id",ok,"reason"] message */
    void (*ok_response_callback)(const char *event_id, bool ok, const char *reason, void *user_data);
    void *ok_response_callback_user_data;
};

typedef struct _NostrRelayWriteRequest {
    char *msg;
    GoChannel *answer;
} NostrRelayWriteRequest;

typedef void (*NostrRelayOkCallback)(bool, char *);

typedef struct _NostrRelayOkResult {
    bool ok;
    char *reason;
} NostrRelayOkResult;

typedef struct _NostrRelayOkWaiter {
    GoChannel *done; /* carries NostrRelayOkResult* */
} NostrRelayOkWaiter;

/* Shared relay control-frame dispatch used by relay.c and relay_optimized.c.
 * Handles NOTICE, EOSE, AUTH, CLOSED, OK, and COUNT. EVENT remains on each
 * receive path so optimized event verification/batching can stay local. */
void nostr_relay_dispatch_control_envelope(struct NostrRelay *relay,
                                           NostrEnvelope *envelope);

int nostr_invalidsig_is_banned(struct NostrRelay *relay, const char *pk);
void nostr_invalidsig_record_fail(struct NostrRelay *relay, const char *pk);

/* Worker thread argument struct (nostrc-o56)
 * Used to pass a pre-ref'd context to worker threads to eliminate the race
 * between thread startup and context freeing. The context is ref'd BEFORE
 * spawning the thread, so the worker owns a valid reference from the start. */
typedef struct _NostrRelayWorkerArg {
    struct NostrRelay *relay;
    GoContext *ctx;  /* Pre-ref'd context - worker MUST unref when done */
} NostrRelayWorkerArg;

/* ========================================================================
 * fp-ieg8: dial coordination between a relay and a pool that re-dials it.
 *
 * Internal on purpose. These are the primitives NostrSimplePool's redial
 * worker needs to retry a relay whose initial connect failed without
 * stepping on the relay's own reconnect loop. Not part of the installed
 * API surface -- callers outside libnostr have nostr_relay_reconnect_now().
 * ======================================================================== */

/**
 * nostr_relay_reconnect_is_self_managed:
 *
 * True while this relay has a live message_loop, i.e. it connected at least
 * once and its own backoff loop is responsible for reconnecting it. False for
 * a relay whose first dial failed (no loop was ever spawned) or whose loop has
 * exited -- those are the relays nobody retries unless the pool does.
 */
bool nostr_relay_reconnect_is_self_managed(struct NostrRelay *relay);

/**
 * nostr_relay_dial_try_claim:
 *
 * Claims the exclusive right to call nostr_relay_connect() on @relay.
 * Returns false when another thread is already inside a dial, when the relay's
 * own loop owns reconnection, when auto-reconnect is disabled, or when the
 * connection context has been cancelled (the relay is being torn down).
 *
 * Every claim that succeeds must be released with nostr_relay_dial_release().
 */
bool nostr_relay_dial_try_claim(struct NostrRelay *relay);

/**
 * nostr_relay_dial_release:
 * @connected: whether the dial established a connection
 *
 * Releases the claim and records the outcome in the relay's existing backoff
 * state, so a pool-driven dial advances reconnect_attempt / next_reconnect_ms
 * exactly the way message_loop's own reconnect does. On failure the relay is
 * left in NOSTR_RELAY_STATE_BACKOFF with next_reconnect_time_ms set, which is
 * what nostr_relay_get_next_reconnect_ms() reports to the redial worker.
 */
void nostr_relay_dial_release(struct NostrRelay *relay, bool connected);

#endif // NOSTR_RELAY_PRIVATE_H
