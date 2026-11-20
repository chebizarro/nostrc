#ifndef __NOSTR_RELAY_H__
#define __NOSTR_RELAY_H__

#include "nostr-export.h"

/* Canonical NostrRelay public header */

#include <stdbool.h>
#include "nostr-filter.h"
#include "channel.h"      /* GoChannel */
#include "context.h"      /* GoContext */
#include "nostr-connection.h"   /* NostrConnection */
#include "hash_map.h"     /* GoHashMap */
#include "error.h"        /* Error */
#ifdef NOSTR_WITH_GLIB
#include <glib-object.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations to avoid heavy includes */
struct NostrSubscription;
typedef struct _NostrRelayPrivate NostrRelayPrivate;

/* Canonical NostrRelay type */
typedef struct NostrRelay {
    NostrRelayPrivate *priv;
    char *url;
    /* request_header; */
    NostrConnection *connection;
    Error **connection_error;
    GoHashMap *subscriptions;
    bool assume_valid;
    int refcount;
} NostrRelay;

#ifdef NOSTR_WITH_GLIB
/**
 * nostr_relay_get_type:
 *
 * Boxed type for `NostrRelay`. The boxed copy increases the reference count
 * (no deep-copy); boxed free decreases it.
 *
 * Returns: (type GType): the GType of NostrRelay
 */
NOSTR_API GType nostr_relay_get_type(void);
#endif

/* GI-facing API (stable symbol names) */
/**
 * nostr_relay_new:
 * @context: (nullable): optional context
 * @url: (not nullable): relay URL
 * @err: (out) (optional) (nullable): error out param
 *
 * Returns: (transfer full) (nullable): new relay
 */
NOSTR_API NostrRelay *nostr_relay_new(GoContext *context, const char *url, Error **err);

/**
 * nostr_relay_free:
 * @relay: (nullable): relay
 *
 * Convenience alias for unref.
 */
NOSTR_API void        nostr_relay_free(NostrRelay *relay);

/**
 * nostr_relay_ref:
 * @relay: (nullable): relay
 *
 * Increments the reference count.
 *
 * Returns: (transfer none) (nullable): same relay
 */
NOSTR_API NostrRelay *nostr_relay_ref(NostrRelay *relay);

/**
 * nostr_relay_unref:
 * @relay: (nullable): relay
 *
 * Decrements the reference count and frees when it reaches zero. Safe on NULL.
 */
NOSTR_API void        nostr_relay_unref(NostrRelay *relay);

/**
 * nostr_relay_connect:
 * @relay: (nullable): relay
 * @err: (out) (optional) (nullable): error out param
 *
 * Returns: success
 */
NOSTR_API bool        nostr_relay_connect(NostrRelay *relay, Error **err);
NOSTR_API bool        nostr_relay_connect_with_context(NostrRelay *relay, GoContext *cancel, Error **err);
/* Terminal nonblocking stop request. Drain/free off the main thread. Dials
 * admitted earlier cannot publish after this call. */
NOSTR_API void        nostr_relay_request_close(NostrRelay *relay);

/**
 * nostr_relay_disconnect:
 * @relay: (nullable): relay
 */
NOSTR_API void        nostr_relay_disconnect(NostrRelay *relay);

/**
 * nostr_relay_close:
 * @relay: (nullable): relay
 * @err: (out) (optional) (nullable): error out param
 *
 * Ends this relay's lifetime: its context and write queues are cancelled/closed.
 * Construct a new relay to dial again. Automatic reconnect on a live relay is
 * unaffected. An already admitted dial cannot publish after close.
 *
 * Returns: success
 */
NOSTR_API bool        nostr_relay_close(NostrRelay *relay, Error **err);

/**
 * nostr_relay_subscribe:
 * @relay: (nullable): relay
 * @ctx: (nullable): context
 * @filters: (nullable): filters
 * @err: (out) (optional) (nullable): error out param
 *
 * Returns: success
 */
NOSTR_API bool        nostr_relay_subscribe(NostrRelay *relay, GoContext *ctx, NostrFilters *filters, Error **err);

/**
 * nostr_relay_prepare_subscription:
 * @relay: (nullable): relay
 * @ctx: (nullable): ignored; may be NULL. The subscription's lifetime follows
 *   the relay's connection context (see nostr_subscription_new()).
 * @filters: (nullable): filters
 *
 * Creates a subscription and registers it for dispatch on @relay. Call
 * nostr_subscription_fire() to send the REQ.
 *
 * Returns: (transfer none) (nullable): internal subscription pointer, or NULL
 *   if @relay or @filters is NULL
 */
NOSTR_API struct NostrSubscription *nostr_relay_prepare_subscription(NostrRelay *relay, GoContext *ctx, NostrFilters *filters);

/**
 * nostr_relay_publish:
 * @relay: (nullable): relay
 * @event: (nullable): event
 */
NOSTR_API void        nostr_relay_publish(NostrRelay *relay, NostrEvent *event);

/**
 * nostr_relay_publish_and_wait:
 * @relay: (nullable): relay
 * @event: (nullable): event
 * @timeout_ms: timeout in milliseconds; 0 uses the default 30s deadline
 * @err: (out) (optional) (nullable): error out param
 *
 * Publishes @event and waits for the relay's NIP-01 OK acknowledgement for
 * that event id. Existing nostr_relay_publish() remains transport-level and
 * backward-compatible; callers that require relay acceptance should use this
 * additive helper.
 *
 * Returns: true only when the relay sends OK with ok=true.
 */
NOSTR_API bool        nostr_relay_publish_and_wait(NostrRelay *relay, NostrEvent *event,
                                         uint64_t timeout_ms, Error **err);

/**
 * nostr_relay_auth:
 * @relay: (nullable): relay
 * @sign: (nullable): sign callback
 *
 * Signs the current challenge synchronously and sends ["AUTH",event] without
 * waiting for the relay's OK; the OK reaches the OK callback and, correlated
 * by event id, the NIP-42 session (nostr_relay_get_auth_status(), the AUTH
 * result callback, pool recovery). A missing challenge or a signing failure
 * sets @err.
 *
 * Deprecated: 2.4: @sign has no user data or cancellation and runs on the
 * caller's thread. Use nostr_relay_set_auth_signer_full() or
 * nostr_relay_send_auth().
 *
 * @err: (out) (optional) (nullable): error out param
 */
NOSTR_API void        nostr_relay_auth(NostrRelay *relay, void (*sign)(NostrEvent *, Error **), Error **err);

/**
 * nostr_relay_auth_wait:
 * @relay: (nullable): relay
 * @sign: (nullable): sign callback
 * @timeout_ms: OK deadline in milliseconds (0 selects 5000)
 * @err: (out) (optional) (nullable): error out param
 *
 * Like nostr_relay_auth() but blocks until the relay answers the AUTH event
 * with OK. Call off the relay reader thread and off any UI main loop. A
 * missing challenge, signing failure, negative OK or deadline sets @err.
 *
 * Deprecated: 2.4: see nostr_relay_auth(); observe the result with
 * nostr_relay_set_auth_result_callback_full() instead of blocking.
 *
 * Returns: true only when the relay sends OK with ok=true.
 */
NOSTR_API bool        nostr_relay_auth_wait(NostrRelay *relay, void (*sign)(NostrEvent *, Error **),
                                            uint64_t timeout_ms, Error **err);

/**
 * nostr_relay_get_auth_challenge:
 * @relay: (nullable): relay
 * @generation: (out) (optional): changes on reconnect or a different challenge
 *
 * Snapshot the current connection's retained NIP-42 challenge. This also
 * covers AUTH received before the caller installed its callback. No signing
 * or I/O occurs; the caller can authenticate outside the relay reader thread.
 *
 * Returns: (transfer full) (nullable): challenge copy; free with free().
 */
NOSTR_API char *nostr_relay_get_auth_challenge(NostrRelay *relay, uint64_t *generation);

/**
 * nostr_relay_count:
 * @relay: (nullable): relay
 * @ctx: (nullable): context
 * @filter: (nullable): filter
 * @err: (out) (optional) (nullable): error out param
 *
 * Returns: count
 */
NOSTR_API int64_t     nostr_relay_count(NostrRelay *relay, GoContext *ctx, NostrFilter *filter, Error **err);

/**
 * nostr_relay_is_connected:
 * @relay: (nullable): relay
 *
 * Returns: whether the relay has an active connection (may be in handshake)
 */
NOSTR_API bool        nostr_relay_is_connected(NostrRelay *relay);

/**
 * nostr_relay_is_established:
 * @relay: (nullable): relay
 *
 * Returns: whether the WebSocket handshake has completed and the connection
 *          is ready for message exchange. Use this when you need to ensure
 *          the connection is fully established before sending messages.
 */
NOSTR_API bool        nostr_relay_is_established(NostrRelay *relay);
/* nostrc-oz77: nostr_relay_connect() returns once the dial has started;
 * lws connects asynchronously. Block (off the main thread) until the current
 * connection's WebSocket handshake succeeds -> true, or fails (refused port,
 * TLS or upgrade error, closed) or @timeout_ms passes -> false with @err set.
 * The relay's own reconnect loop is left alone either way. */
NOSTR_API bool        nostr_relay_wait_established(NostrRelay *relay, uint32_t timeout_ms, Error **err);
NOSTR_API bool        nostr_relay_wait_established_with_context(NostrRelay *relay,
                                                            uint32_t timeout_ms,
                                                            GoContext *cancel, Error **err);

/**
 * nostr_relay_enable_debug_raw:
 * @relay: (nullable): relay
 * @enable: enable flag
 */
NOSTR_API void        nostr_relay_enable_debug_raw(NostrRelay *relay, int enable);

/**
 * nostr_relay_get_debug_raw_channel:
 * @relay: (nullable): relay
 *
 * Returns: (transfer none) (nullable): internal channel owned by relay
 */
NOSTR_API GoChannel  *nostr_relay_get_debug_raw_channel(NostrRelay *relay);

/* Accessors (GLib-friendly) */
/**
 * nostr_relay_get_url_const:
 * @relay: (nullable): relay
 *
 * Returns: (transfer none) (nullable): internal URL string
 */
NOSTR_API const char *nostr_relay_get_url_const(const NostrRelay *relay);

/**
 * nostr_relay_get_context:
 * @relay: (nullable): relay
 *
 * Returns: (transfer none) (nullable): connection GoContext
 */
NOSTR_API GoContext *nostr_relay_get_context(const NostrRelay *relay);

/**
 * nostr_relay_get_write_channel:
 * @relay: (nullable): relay
 *
 * Returns: (transfer none) (nullable): the write channel used internally
 */
NOSTR_API GoChannel *nostr_relay_get_write_channel(const NostrRelay *relay);

/**
 * nostr_relay_write:
 * @relay: (nullable): relay
 * @msg: (transfer none): JSON string to send; copied, the caller keeps it
 *
 * Enqueue a JSON message for sending. Returns a channel that yields an Error* (or NULL) once written.
 *
 * The channel holds at most one answer, and an Error* may already be in it on
 * return (a relay that is closing answers at once). A caller that stops
 * waiting lets go of it in this order: go_channel_close(), then receive with
 * go_channel_try_receive() until it fails and free_error() each non-NULL
 * Error*, then go_channel_unref(). Closing first means no answer can arrive
 * after the drain; closing and unreffing alone leaks an answer already there.
 *
 * Returns: (transfer full) (nullable): channel of Error* result
 */
NOSTR_API GoChannel *nostr_relay_write(NostrRelay *relay, char *msg);

/* ========================================================================
 * Auto-reconnection with exponential backoff (nostrc-4du)
 * ======================================================================== */

/**
 * NostrRelayConnectionState:
 * @NOSTR_RELAY_STATE_DISCONNECTED: Not connected
 * @NOSTR_RELAY_STATE_CONNECTING: Connection attempt in progress
 * @NOSTR_RELAY_STATE_CONNECTED: Successfully connected
 * @NOSTR_RELAY_STATE_BACKOFF: Waiting before next reconnection attempt
 *
 * Connection state for a relay.
 */
#ifndef NOSTR_RELAY_CONNECTION_STATE_DEFINED
#define NOSTR_RELAY_CONNECTION_STATE_DEFINED
typedef enum NostrRelayConnectionState_ {
    NOSTR_RELAY_STATE_DISCONNECTED = 0,
    NOSTR_RELAY_STATE_CONNECTING,
    NOSTR_RELAY_STATE_CONNECTED,
    NOSTR_RELAY_STATE_BACKOFF
} NostrRelayConnectionState;
#endif

/**
 * NostrRelayStateCallback:
 * @relay: The relay whose state changed
 * @old_state: Previous connection state
 * @new_state: New connection state
 * @user_data: User data passed to nostr_relay_set_state_callback()
 *
 * Callback invoked when relay connection state changes.
 * Called from relay worker thread - use thread-safe operations.
 */
typedef void (*NostrRelayStateCallback)(NostrRelay *relay,
                                        NostrRelayConnectionState old_state,
                                        NostrRelayConnectionState new_state,
                                        void *user_data);

/**
 * nostr_relay_set_auto_reconnect:
 * @relay: (nullable): relay
 * @enable: Whether to enable auto-reconnection
 *
 * Enable or disable automatic reconnection with exponential backoff.
 * When enabled, the relay will automatically attempt to reconnect
 * when the connection is lost.
 *
 * Default: enabled (true)
 */
NOSTR_API void nostr_relay_set_auto_reconnect(NostrRelay *relay, bool enable);

/**
 * nostr_relay_get_auto_reconnect:
 * @relay: (nullable): relay
 *
 * Returns: Whether auto-reconnection is enabled
 */
NOSTR_API bool nostr_relay_get_auto_reconnect(NostrRelay *relay);

/**
 * nostr_relay_get_connection_state:
 * @relay: (nullable): relay
 *
 * Returns: Current connection state
 */
NOSTR_API NostrRelayConnectionState nostr_relay_get_connection_state(NostrRelay *relay);

/**
 * nostr_relay_get_connection_state_name:
 * @state: Connection state
 *
 * Returns: Human-readable state name (static string)
 */
NOSTR_API const char *nostr_relay_get_connection_state_name(NostrRelayConnectionState state);

/**
 * NostrRelayDestroyNotify:
 * @user_data: the user data registered with a callback
 *
 * Releases the user data of a callback registered with one of the
 * nostr_relay_set_*_callback_full() functions.
 */
typedef void (*NostrRelayDestroyNotify)(void *user_data);

/**
 * nostr_relay_set_state_callback:
 * @relay: (nullable): relay
 * @callback: (nullable): Callback function, or NULL to remove
 * @user_data: (nullable): User data passed to callback
 *
 * Set a callback to be notified of connection state changes.
 * The callback is invoked from the relay worker thread.
 *
 * The relay does not own @user_data. A call that replaces or removes the
 * callback does not wait for an invocation that has already started on
 * another thread, and that invocation still uses @user_data. An owner that
 * frees @user_data after removing the callback should register it with
 * nostr_relay_set_state_callback_full() instead.
 */
NOSTR_API void nostr_relay_set_state_callback(NostrRelay *relay,
                                    NostrRelayStateCallback callback,
                                    void *user_data);

/**
 * nostr_relay_set_state_callback_full:
 * @relay: (nullable): relay
 * @callback: (nullable): Callback function, or NULL to remove
 * @user_data: (nullable): User data passed to callback; owned by the relay
 * @destroy: (nullable): Releases @user_data
 *
 * Like nostr_relay_set_state_callback(), but the relay owns @user_data and
 * calls @destroy on it exactly once, when both of these hold: this callback
 * has been replaced or removed (or the relay freed), and every invocation of
 * it that had already started has returned. So an owner can remove the
 * callback and drop its own references right away, even while a worker thread
 * is inside the callback. Nothing waits for that worker, and it never sees
 * freed @user_data. (nostrc-flp7)
 *
 * @destroy runs on whichever thread lets go last: the caller that replaced or
 * removed the callback, the thread whose invocation returned last, or the
 * thread freeing the relay. It must not use @relay, which may be being freed.
 * If @callback or @relay is NULL, @destroy is called on @user_data before
 * this function returns.
 *
 * Since: 1.1
 */
NOSTR_API void nostr_relay_set_state_callback_full(NostrRelay *relay,
                                         NostrRelayStateCallback callback,
                                         void *user_data,
                                         NostrRelayDestroyNotify destroy);

/**
 * NostrRelayAuthCallback:
 * @relay: The relay that received the AUTH challenge
 * @challenge: The challenge string from the relay
 * @user_data: User data passed to nostr_relay_set_auth_callback()
 *
 * Callback invoked when a NIP-42 AUTH challenge is received.
 * Called from relay worker thread - use thread-safe operations.
 */
typedef void (*NostrRelayAuthCallback)(NostrRelay *relay,
                                       const char *challenge,
                                       void *user_data);

/**
 * nostr_relay_set_auth_callback:
 * @relay: (nullable): relay
 * @callback: (nullable): Callback function, or NULL to remove
 * @user_data: (nullable): User data passed to callback
 *
 * Set a callback to be notified of NIP-42 AUTH challenges.
 * The callback is invoked from the relay worker thread.
 *
 * The relay does not own @user_data; see nostr_relay_set_state_callback()
 * and use nostr_relay_set_auth_callback_full() when the data is freed after
 * the callback is removed.
 */
NOSTR_API void nostr_relay_set_auth_callback(NostrRelay *relay,
                                   NostrRelayAuthCallback callback,
                                   void *user_data);

/**
 * nostr_relay_set_auth_callback_full:
 * @relay: (nullable): relay
 * @callback: (nullable): Callback function, or NULL to remove
 * @user_data: (nullable): User data passed to callback; owned by the relay
 * @destroy: (nullable): Releases @user_data
 *
 * Like nostr_relay_set_auth_callback(), with the ownership rules of
 * nostr_relay_set_state_callback_full().
 *
 * Since: 1.1
 */
NOSTR_API void nostr_relay_set_auth_callback_full(NostrRelay *relay,
                                        NostrRelayAuthCallback callback,
                                        void *user_data,
                                        NostrRelayDestroyNotify destroy);

/**
 * nostr_relay_get_reconnect_attempt:
 * @relay: (nullable): relay
 *
 * Returns: Number of consecutive failed reconnection attempts (0 if connected)
 */
NOSTR_API int nostr_relay_get_reconnect_attempt(NostrRelay *relay);

/**
 * nostr_relay_get_next_reconnect_ms:
 * @relay: (nullable): relay
 *
 * Returns: Milliseconds until next reconnection attempt (0 if not in backoff)
 */
NOSTR_API uint64_t nostr_relay_get_next_reconnect_ms(NostrRelay *relay);

/**
 * nostr_relay_reconnect_now:
 * @relay: (nullable): relay
 *
 * Request immediate reconnection, bypassing backoff delay.
 * Has no effect if already connected or connection attempt in progress.
 */
NOSTR_API void nostr_relay_reconnect_now(NostrRelay *relay);

/* ========================================================================
 * Extension message handler (NIP-77 negentropy, etc.)
 * ======================================================================== */

/**
 * nostr_relay_set_custom_handler:
 * @relay: (nullable): relay
 * @handler: (nullable): callback for unknown/extension messages, or NULL to clear
 *
 * Install a handler for incoming messages that don't match standard Nostr
 * envelope types (EVENT, EOSE, OK, etc.). The handler receives the raw JSON
 * string and returns true if it handled the message.
 *
 * Used for NIP-77 negentropy (NEG-MSG, NEG-ERR) and other extensions.
 * Called from relay worker thread - use thread-safe operations.
 */
NOSTR_API void nostr_relay_set_custom_handler(NostrRelay *relay, bool (*handler)(const char *));

/**
 * NostrRelayOkResponseCallback:
 * @event_id: id of the event the relay answered
 * @ok: whether the relay accepted the event
 * @reason: the relay's message, "" if it gave none
 * @user_data: user data passed to nostr_relay_set_ok_callback()
 *
 * Callback invoked for every ["OK","id",ok,"reason"] received from a relay.
 * Called from relay worker thread - use thread-safe operations.
 */
typedef void (*NostrRelayOkResponseCallback)(const char *event_id,
                                             bool ok,
                                             const char *reason,
                                             void *user_data);

/**
 * nostr_relay_set_ok_callback:
 * @relay: relay instance
 * @callback: called for every ["OK","id",ok,"reason"] received from the relay
 * @user_data: user data passed to callback
 *
 * Register a callback that fires whenever an OK response is received.
 * Used by NIP-42 to subscribe only after auth is confirmed.
 * Called from relay worker thread - use thread-safe operations (e.g. g_idle_add).
 *
 * The relay does not own @user_data; see nostr_relay_set_state_callback()
 * and use nostr_relay_set_ok_callback_full() when the data is freed after
 * the callback is removed.
 */
NOSTR_API void nostr_relay_set_ok_callback(NostrRelay *relay,
                                  NostrRelayOkResponseCallback callback,
                                  void *user_data);

/**
 * nostr_relay_set_ok_callback_full:
 * @relay: (nullable): relay
 * @callback: (nullable): Callback function, or NULL to remove
 * @user_data: (nullable): User data passed to callback; owned by the relay
 * @destroy: (nullable): Releases @user_data
 *
 * Like nostr_relay_set_ok_callback(), with the ownership rules of
 * nostr_relay_set_state_callback_full().
 *
 * Since: 1.1
 */
NOSTR_API void nostr_relay_set_ok_callback_full(NostrRelay *relay,
                                      NostrRelayOkResponseCallback callback,
                                      void *user_data,
                                      NostrRelayDestroyNotify destroy);

/* ========================================================================
 * NIP-42 authentication session (nostrc-7i6w)
 *
 * Each connection has at most one NIP-42 session. Its state, the challenge
 * it answers and the AUTH event in flight belong to that connection: a
 * reconnect (a new connection generation) or a different challenge (a new
 * auth generation) cancels a signature in progress and forgets an AUTH whose
 * OK has not arrived. Nothing authenticates unless the caller registers a
 * signer for this relay (nostr_relay_set_auth_signer_full()): there is no
 * global or automatic identity, so the caller chooses per connection which
 * identity, if any, a relay learns.
 *
 * An AUTH is sent as ["AUTH",<signed kind-22242 event>] and its result is
 * the relay's OK for that event id only. OK true -> ACCEPTED; OK false ->
 * REJECTED, which is held: nothing is signed again on this connection until
 * the relay sends a new challenge or the caller calls nostr_relay_auth_start().
 * A local failure (signer refused, event failed verification, write could
 * not be queued) is FAILED and held the same way.
 * ======================================================================== */

/**
 * NostrRelayAuthState:
 * @NOSTR_RELAY_AUTH_STATE_NONE: no challenge on this connection
 * @NOSTR_RELAY_AUTH_STATE_CHALLENGED: a challenge, no AUTH attempted for it
 * @NOSTR_RELAY_AUTH_STATE_SIGNING: the registered signer is signing it
 * @NOSTR_RELAY_AUTH_STATE_SENT: ["AUTH",...] queued, the relay's OK pending
 * @NOSTR_RELAY_AUTH_STATE_ACCEPTED: the relay answered this challenge's AUTH with OK true
 * @NOSTR_RELAY_AUTH_STATE_REJECTED: OK false; held until a new challenge or nostr_relay_auth_start()
 * @NOSTR_RELAY_AUTH_STATE_FAILED: local failure; held like REJECTED
 */
typedef enum NostrRelayAuthState {
    NOSTR_RELAY_AUTH_STATE_NONE = 0,
    NOSTR_RELAY_AUTH_STATE_CHALLENGED,
    NOSTR_RELAY_AUTH_STATE_SIGNING,
    NOSTR_RELAY_AUTH_STATE_SENT,
    NOSTR_RELAY_AUTH_STATE_ACCEPTED,
    NOSTR_RELAY_AUTH_STATE_REJECTED,
    NOSTR_RELAY_AUTH_STATE_FAILED
} NostrRelayAuthState;

NOSTR_API const char *nostr_relay_auth_state_name(NostrRelayAuthState state);

/**
 * NostrRelayFrameInfo:
 * @sequence: receive order of this relay's control frames (AUTH, OK, CLOSED,
 *   EOSE, NOTICE, COUNT), counted on the reader thread from 1 and never
 *   reused, across reconnects. Of two frames from one relay, the one with
 *   the larger sequence arrived later on the wire.
 * @connection_generation: the connection the frame arrived on; see
 *   nostr_relay_get_connection_generation()
 * @auth_generation: the challenge generation in force when it arrived (the
 *   generation nostr_relay_get_auth_challenge() reports)
 * @auth_state: this connection's NIP-42 state when it arrived, before the
 *   frame's own effect (an OK answering our AUTH sees SENT)
 *
 * Where a control frame sits in wire order relative to NIP-42. A consumer
 * that sees results on different threads (CLOSED on a pool worker, OK on the
 * reader) compares these instead of arrival order at its own callbacks: e.g.
 * a CLOSED "auth-required:" with @auth_state ACCEPTED was the relay's answer
 * to a REQ it received after accepting our AUTH, and a frame whose
 * @connection_generation is older than the relay's current one belongs to a
 * lost connection. Fields are only ever appended. Since: 2.4
 */
typedef struct NostrRelayFrameInfo {
    uint64_t sequence;
    uint64_t connection_generation;
    uint64_t auth_generation;
    NostrRelayAuthState auth_state;
} NostrRelayFrameInfo;

/**
 * nostr_relay_get_connection_generation:
 * @relay: (nullable): relay
 *
 * Returns: a counter that changes each time the relay starts a new
 *   connection (initial dial or reconnect); 0 before the first one.
 */
NOSTR_API uint64_t nostr_relay_get_connection_generation(NostrRelay *relay);

/**
 * nostr_relay_get_callback_frame:
 * @relay: (nullable): relay
 * @out: (out): the frame being reported
 *
 * Inside this relay's OK callback or AUTH challenge callback, the
 * #NostrRelayFrameInfo of the frame that callback reports. Thread-local;
 * false (and @out zeroed) outside those callbacks.
 */
NOSTR_API bool nostr_relay_get_callback_frame(NostrRelay *relay, NostrRelayFrameInfo *out);

/**
 * NostrRelayAuthStatus:
 * @state: current NIP-42 state of the current connection
 * @connection_generation: current connection generation
 * @auth_generation: current challenge generation
 * @sequence: frame sequence of the last relay frame that changed @state
 *   (the challenge or the OK); 0 if none on this connection
 * @event_id: the AUTH event in flight or last answered, "" if none
 */
typedef struct NostrRelayAuthStatus {
    NostrRelayAuthState state;
    uint64_t connection_generation;
    uint64_t auth_generation;
    uint64_t sequence;
    char event_id[65];
} NostrRelayAuthStatus;

/**
 * nostr_relay_get_auth_status:
 * @relay: (nullable): relay
 * @out: (out): snapshot, taken under the relay's mutex
 *
 * Returns: false if @relay or @out is NULL
 */
NOSTR_API bool nostr_relay_get_auth_status(NostrRelay *relay, NostrRelayAuthStatus *out);

/**
 * NostrRelayAuthPolicy:
 * @NOSTR_RELAY_AUTH_ON_CHALLENGE: answer each new challenge of a connection
 * @NOSTR_RELAY_AUTH_ON_DEMAND: answer a connection's challenge only once the
 *   relay has refused something with "auth-required:" (a CLOSED, or OK false
 *   for an EVENT) on it; a challenge that arrives after the refusal still
 *   triggers it. At most once per challenge. A refusal received while the
 *   connection is already authenticated does not count (the identity is not
 *   enough; see NOSTR_POOL_RECOVERY_AUTH_REFUSED): use nostr_relay_auth_start().
 */
typedef enum NostrRelayAuthPolicy {
    NOSTR_RELAY_AUTH_ON_CHALLENGE = 0,
    NOSTR_RELAY_AUTH_ON_DEMAND
} NostrRelayAuthPolicy;

/**
 * NostrRelayAuthRequest:
 *
 * One attempt to answer one challenge of one connection, handed to the
 * signer. Refcounted; the signer refs it to finish asynchronously. The
 * relay cancels it when the attempt can no longer be sent: a new challenge,
 * a reconnect, the signer replaced or cleared, nostr_relay_close() or the
 * relay freed. After cancellation nostr_relay_auth_request_complete() does
 * nothing, so a stale signature is never sent.
 */
typedef struct NostrRelayAuthRequest NostrRelayAuthRequest;

/**
 * NostrRelayAuthSignFunc:
 * @request: (transfer none): the attempt; ref it to keep it past the call
 * @user_data: the signer's user data
 *
 * Signs nostr_relay_auth_request_get_event() and reports the outcome with
 * nostr_relay_auth_request_complete() (or _complete_json()), now or later,
 * from any thread. Called on a thread the library starts for this attempt,
 * never on the relay's reader thread or with a relay lock held, so it may
 * block (a remote signer, a user prompt).
 */
typedef void (*NostrRelayAuthSignFunc)(NostrRelayAuthRequest *request, void *user_data);

/**
 * NostrRelayAuthCancelFunc:
 * @request: (transfer none): the cancelled attempt
 * @user_data: data given to nostr_relay_auth_request_set_cancel_handler()
 *
 * Called at most once, on the cancelling thread, without relay locks held.
 */
typedef void (*NostrRelayAuthCancelFunc)(NostrRelayAuthRequest *request, void *user_data);

/**
 * nostr_relay_set_auth_signer_full:
 * @relay: (nullable): relay
 * @policy: when to answer a challenge
 * @identity_generation: the caller's identity/account generation; reported
 *   back on requests and results so the caller can refuse a stale one
 * @expected_pubkey: (nullable): 64-hex pubkey put in the template and
 *   required of the signed event
 * @sign: (nullable): signer, or NULL to authenticate no more
 * @user_data: (nullable): owned by the relay
 * @destroy: (nullable): releases @user_data once no attempt can call @sign
 *
 * The identity this connection authenticates with, chosen by the caller for
 * this relay only. Replacing or clearing it cancels an attempt in flight and
 * forgets an AUTH whose OK is pending. Registering a signer never re-answers
 * the current challenge by itself with ON_DEMAND; with ON_CHALLENGE a
 * challenge already received and not yet attempted is answered now.
 *
 * Returns: false if @relay is NULL or allocation failed (@destroy then ran)
 */
NOSTR_API bool nostr_relay_set_auth_signer_full(NostrRelay *relay,
                                               NostrRelayAuthPolicy policy,
                                               uint64_t identity_generation,
                                               const char *expected_pubkey,
                                               NostrRelayAuthSignFunc sign,
                                               void *user_data,
                                               NostrRelayDestroyNotify destroy);

/**
 * nostr_relay_auth_start:
 * @relay: (nullable): relay
 * @err: (out) (optional) (nullable): why nothing started
 *
 * The explicit action that answers the current challenge with the
 * registered signer now, whatever the policy, including after a REJECTED
 * or FAILED attempt for the same challenge, or an AUTH the relay has not
 * answered yet (its late OK is then ignored). Refused with no challenge, no
 * signer, an attempt being signed, or the challenge already accepted for
 * this signer's identity generation.
 */
NOSTR_API bool nostr_relay_auth_start(NostrRelay *relay, Error **err);

/**
 * nostr_relay_send_auth:
 * @relay: (nullable): relay
 * @signed_event: (transfer none): a signed kind-22242 event
 * @auth_generation: the challenge generation it answers (0: the current one)
 * @err: (out) (optional) (nullable): why it was not sent
 *
 * For callers that sign themselves. Verifies @signed_event locally (valid id
 * and signature, kind 22242, "relay" tag equal to this relay's URL,
 * "challenge" tag equal to the current challenge, created_at within 10
 * minutes), refuses a stale @auth_generation, then queues
 * ["AUTH",event] and tracks its OK exactly like a signer's attempt (state,
 * result callback, pool recovery). Caller-signed retries must use distinct
 * event ids if late OKs from superseded attempts need to be distinguished.
 * Cancels a registered signer's attempt in
 * flight. Never waits for the socket write, the OK or queue space: a full
 * write queue fails the attempt. Queue admission and the writer's connection
 * lease both reject superseded attempts. Writes already admitted to a
 * connection cannot be recalled.
 */
NOSTR_API bool nostr_relay_send_auth(NostrRelay *relay, const NostrEvent *signed_event,
                                    uint64_t auth_generation, Error **err);

/* Request accessors. Strings and the event are owned by the request. */
NOSTR_API NostrRelayAuthRequest *nostr_relay_auth_request_ref(NostrRelayAuthRequest *request);
NOSTR_API void nostr_relay_auth_request_unref(NostrRelayAuthRequest *request);
/* The unsigned template: kind 22242, created_at now, empty content, tags
 * ["relay",url], ["challenge",challenge] and a unique per-attempt ["nonce",...],
 * pubkey only if expected. Preserve the nonce when signing: it prevents a
 * same-second retry from reusing the superseded attempt's event id. */
NOSTR_API const NostrEvent *nostr_relay_auth_request_get_event(const NostrRelayAuthRequest *request);
NOSTR_API const char *nostr_relay_auth_request_get_relay_url(const NostrRelayAuthRequest *request);
NOSTR_API const char *nostr_relay_auth_request_get_challenge(const NostrRelayAuthRequest *request);
NOSTR_API uint64_t nostr_relay_auth_request_get_auth_generation(const NostrRelayAuthRequest *request);
NOSTR_API uint64_t nostr_relay_auth_request_get_connection_generation(const NostrRelayAuthRequest *request);
NOSTR_API uint64_t nostr_relay_auth_request_get_identity_generation(const NostrRelayAuthRequest *request);
NOSTR_API bool nostr_relay_auth_request_is_cancelled(NostrRelayAuthRequest *request);
/* @cancel runs once if the request is cancelled before it completes, at
 * once if it already was; never after completion. */
NOSTR_API void nostr_relay_auth_request_set_cancel_handler(NostrRelayAuthRequest *request,
                                                         NostrRelayAuthCancelFunc cancel,
                                                         void *user_data);
/**
 * nostr_relay_auth_request_complete:
 * @request: the attempt
 * @signed_event: (nullable) (transfer none): the signed template, or NULL
 * @error_message: (nullable): why signing failed, with @signed_event NULL
 *
 * Finishes the attempt once; later calls do nothing. A cancelled or stale
 * attempt is dropped silently. Otherwise the event is verified as by
 * nostr_relay_send_auth() (and against the expected pubkey) and sent, or
 * the attempt ends FAILED with a result callback.
 *
 * Returns: true if ["AUTH",...] was queued
 */
NOSTR_API bool nostr_relay_auth_request_complete(NostrRelayAuthRequest *request,
                                                const NostrEvent *signed_event,
                                                const char *error_message);
NOSTR_API bool nostr_relay_auth_request_complete_json(NostrRelayAuthRequest *request,
                                                     const char *signed_event_json,
                                                     const char *error_message);

/**
 * NostrRelayAuthResult:
 * @state: ACCEPTED, REJECTED or FAILED
 * @event_id: the AUTH event, "" when it never existed (signing failed)
 * @reason: the relay's OK message, or the local failure, "" if none
 * @challenge: the challenge answered
 * @identity_generation: of the signer that signed (0 for nostr_relay_send_auth())
 * @frame: the OK frame for ACCEPTED/REJECTED; for FAILED the generations in
 *   force when failure was committed (not callback delivery) and sequence 0
 */
typedef struct NostrRelayAuthResult {
    NostrRelayAuthState state;
    const char *event_id;
    const char *reason;
    const char *challenge;
    uint64_t identity_generation;
    NostrRelayFrameInfo frame;
} NostrRelayAuthResult;

/**
 * NostrRelayAuthResultCallback:
 *
 * The end of an AUTH attempt that was not cancelled. Runs on the reader
 * thread (relay OK) or on the thread that completed the request (local
 * failure), without relay locks held. Strings are valid during the call.
 */
typedef void (*NostrRelayAuthResultCallback)(NostrRelay *relay,
                                             const NostrRelayAuthResult *result,
                                             void *user_data);

/* Ownership rules of nostr_relay_set_state_callback_full(). Since: 2.4 */
NOSTR_API void nostr_relay_set_auth_result_callback_full(NostrRelay *relay,
                                                        NostrRelayAuthResultCallback callback,
                                                        void *user_data,
                                                        NostrRelayDestroyNotify destroy);

#ifdef __cplusplus
}
#endif

#endif /* __NOSTR_RELAY_H__ */
