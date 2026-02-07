#ifndef __NOSTR_RELAY_H__
#define __NOSTR_RELAY_H__

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
GType nostr_relay_get_type(void);
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
NostrRelay *nostr_relay_new(GoContext *context, const char *url, Error **err);

/**
 * nostr_relay_free:
 * @relay: (nullable): relay
 *
 * Convenience alias for unref.
 */
void        nostr_relay_free(NostrRelay *relay);

/**
 * nostr_relay_ref:
 * @relay: (nullable): relay
 *
 * Increments the reference count.
 *
 * Returns: (transfer none) (nullable): same relay
 */
NostrRelay *nostr_relay_ref(NostrRelay *relay);

/**
 * nostr_relay_unref:
 * @relay: (nullable): relay
 *
 * Decrements the reference count and frees when it reaches zero. Safe on NULL.
 */
void        nostr_relay_unref(NostrRelay *relay);

/**
 * nostr_relay_connect:
 * @relay: (nullable): relay
 * @err: (out) (optional) (nullable): error out param
 *
 * Returns: success
 */
bool        nostr_relay_connect(NostrRelay *relay, Error **err);

/**
 * nostr_relay_disconnect:
 * @relay: (nullable): relay
 */
void        nostr_relay_disconnect(NostrRelay *relay);

/**
 * nostr_relay_close:
 * @relay: (nullable): relay
 * @err: (out) (optional) (nullable): error out param
 *
 * Returns: success
 */
bool        nostr_relay_close(NostrRelay *relay, Error **err);

/**
 * nostr_relay_subscribe:
 * @relay: (nullable): relay
 * @ctx: (nullable): context
 * @filters: (nullable): filters
 * @err: (out) (optional) (nullable): error out param
 *
 * Returns: success
 */
bool        nostr_relay_subscribe(NostrRelay *relay, GoContext *ctx, NostrFilters *filters, Error **err);

/**
 * nostr_relay_prepare_subscription:
 * @relay: (nullable): relay
 * @ctx: (nullable): context
 * @filters: (nullable): filters
 *
 * Returns: (transfer none) (nullable): internal subscription pointer
 */
struct NostrSubscription *nostr_relay_prepare_subscription(NostrRelay *relay, GoContext *ctx, NostrFilters *filters);

/**
 * nostr_relay_publish:
 * @relay: (nullable): relay
 * @event: (nullable): event
 */
void        nostr_relay_publish(NostrRelay *relay, NostrEvent *event);

/**
 * nostr_relay_auth:
 * @relay: (nullable): relay
 * @sign: (nullable): sign callback
 * @err: (out) (optional) (nullable): error out param
 */
void        nostr_relay_auth(NostrRelay *relay, void (*sign)(NostrEvent *, Error **), Error **err);

/**
 * nostr_relay_count:
 * @relay: (nullable): relay
 * @ctx: (nullable): context
 * @filter: (nullable): filter
 * @err: (out) (optional) (nullable): error out param
 *
 * Returns: count
 */
int64_t     nostr_relay_count(NostrRelay *relay, GoContext *ctx, NostrFilter *filter, Error **err);

/**
 * nostr_relay_is_connected:
 * @relay: (nullable): relay
 *
 * Returns: whether the relay has an active connection (may be in handshake)
 */
bool        nostr_relay_is_connected(NostrRelay *relay);

/**
 * nostr_relay_is_established:
 * @relay: (nullable): relay
 *
 * Returns: whether the WebSocket handshake has completed and the connection
 *          is ready for message exchange. Use this when you need to ensure
 *          the connection is fully established before sending messages.
 */
bool        nostr_relay_is_established(NostrRelay *relay);

/**
 * nostr_relay_enable_debug_raw:
 * @relay: (nullable): relay
 * @enable: enable flag
 */
void        nostr_relay_enable_debug_raw(NostrRelay *relay, int enable);

/**
 * nostr_relay_get_debug_raw_channel:
 * @relay: (nullable): relay
 *
 * Returns: (transfer none) (nullable): internal channel owned by relay
 */
GoChannel  *nostr_relay_get_debug_raw_channel(NostrRelay *relay);

/* Accessors (GLib-friendly) */
/**
 * nostr_relay_get_url_const:
 * @relay: (nullable): relay
 *
 * Returns: (transfer none) (nullable): internal URL string
 */
const char *nostr_relay_get_url_const(const NostrRelay *relay);

/**
 * nostr_relay_get_context:
 * @relay: (nullable): relay
 *
 * Returns: (transfer none) (nullable): connection GoContext
 */
GoContext *nostr_relay_get_context(const NostrRelay *relay);

/**
 * nostr_relay_get_write_channel:
 * @relay: (nullable): relay
 *
 * Returns: (transfer none) (nullable): the write channel used internally
 */
GoChannel *nostr_relay_get_write_channel(const NostrRelay *relay);

/**
 * nostr_relay_write:
 * @relay: (nullable): relay
 * @msg: (transfer full): JSON string to send; will be freed by callee
 *
 * Enqueue a JSON message for sending. Returns a channel that yields an Error* (or NULL) once written.
 *
 * Returns: (transfer full) (nullable): channel of Error* result
 */
GoChannel *nostr_relay_write(NostrRelay *relay, char *msg);

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
void nostr_relay_set_auto_reconnect(NostrRelay *relay, bool enable);

/**
 * nostr_relay_get_auto_reconnect:
 * @relay: (nullable): relay
 *
 * Returns: Whether auto-reconnection is enabled
 */
bool nostr_relay_get_auto_reconnect(NostrRelay *relay);

/**
 * nostr_relay_get_connection_state:
 * @relay: (nullable): relay
 *
 * Returns: Current connection state
 */
NostrRelayConnectionState nostr_relay_get_connection_state(NostrRelay *relay);

/**
 * nostr_relay_get_connection_state_name:
 * @state: Connection state
 *
 * Returns: Human-readable state name (static string)
 */
const char *nostr_relay_get_connection_state_name(NostrRelayConnectionState state);

/**
 * nostr_relay_set_state_callback:
 * @relay: (nullable): relay
 * @callback: (nullable): Callback function, or NULL to remove
 * @user_data: (nullable): User data passed to callback
 *
 * Set a callback to be notified of connection state changes.
 * The callback is invoked from the relay worker thread.
 */
void nostr_relay_set_state_callback(NostrRelay *relay,
                                    NostrRelayStateCallback callback,
                                    void *user_data);

/**
 * nostr_relay_get_reconnect_attempt:
 * @relay: (nullable): relay
 *
 * Returns: Number of consecutive failed reconnection attempts (0 if connected)
 */
int nostr_relay_get_reconnect_attempt(NostrRelay *relay);

/**
 * nostr_relay_get_next_reconnect_ms:
 * @relay: (nullable): relay
 *
 * Returns: Milliseconds until next reconnection attempt (0 if not in backoff)
 */
uint64_t nostr_relay_get_next_reconnect_ms(NostrRelay *relay);

/**
 * nostr_relay_reconnect_now:
 * @relay: (nullable): relay
 *
 * Request immediate reconnection, bypassing backoff delay.
 * Has no effect if already connected or connection attempt in progress.
 */
void nostr_relay_reconnect_now(NostrRelay *relay);

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
void nostr_relay_set_custom_handler(NostrRelay *relay, bool (*handler)(const char *));

#ifdef __cplusplus
}
#endif

#endif /* __NOSTR_RELAY_H__ */
