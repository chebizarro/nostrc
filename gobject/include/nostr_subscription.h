#ifndef NOSTR_GSUBSCRIPTION_H
#define NOSTR_GSUBSCRIPTION_H

#include <glib-object.h>
#include "nostr-subscription.h"   /* core NostrSubscription APIs */
#include "nostr_relay.h"          /* GNostrRelay wrapper */
#include "nostr_filter.h"         /* GLib filter wrapper (temporary name) */

G_BEGIN_DECLS

/**
 * NostrSubscriptionType:
 * @NOSTR_SUBSCRIPTION_EPHEMERAL: Close subscription after EOSE (End of Stored Events)
 * @NOSTR_SUBSCRIPTION_PERSISTENT: Keep subscription open until explicit close
 *
 * Defines the lifetime behavior of a Nostr subscription.
 *
 * Ephemeral subscriptions are useful for one-time queries where you only need
 * the currently stored events. Persistent subscriptions remain open to receive
 * new events as they are published.
 *
 * Since: 0.1
 */
typedef enum {
    NOSTR_SUBSCRIPTION_EPHEMERAL,   /* Close after EOSE */
    NOSTR_SUBSCRIPTION_PERSISTENT   /* Keep open until explicit close */
} NostrSubscriptionType;

/**
 * NostrSubscriptionState:
 * @NOSTR_SUBSCRIPTION_STATE_PENDING: Subscription created but not yet sent to relay
 * @NOSTR_SUBSCRIPTION_STATE_ACTIVE: Subscription is active and receiving events
 * @NOSTR_SUBSCRIPTION_STATE_EOSE_RECEIVED: End of stored events received from relay
 * @NOSTR_SUBSCRIPTION_STATE_CLOSED: Subscription has been closed
 * @NOSTR_SUBSCRIPTION_STATE_ERROR: Subscription encountered an error
 *
 * Represents the lifecycle state of a Nostr subscription.
 *
 * The typical lifecycle is:
 * PENDING -> ACTIVE -> EOSE_RECEIVED -> CLOSED
 *
 * For ephemeral subscriptions, the transition to CLOSED happens automatically
 * after EOSE_RECEIVED. For persistent subscriptions, an explicit close is required.
 *
 * Since: 0.1
 */
typedef enum {
    NOSTR_SUBSCRIPTION_STATE_PENDING,
    NOSTR_SUBSCRIPTION_STATE_ACTIVE,
    NOSTR_SUBSCRIPTION_STATE_EOSE_RECEIVED,
    NOSTR_SUBSCRIPTION_STATE_CLOSED,
    NOSTR_SUBSCRIPTION_STATE_ERROR
} NostrSubscriptionState;

/**
 * NostrRetryPolicy:
 * @NOSTR_RETRY_POLICY_NONE: Do not retry on failure
 * @NOSTR_RETRY_POLICY_IMMEDIATE: Retry immediately on failure
 * @NOSTR_RETRY_POLICY_EXPONENTIAL_BACKOFF: Retry with exponential backoff
 *
 * Defines the retry behavior when a subscription fails.
 *
 * Since: 0.1
 */
typedef enum {
    NOSTR_RETRY_POLICY_NONE,
    NOSTR_RETRY_POLICY_IMMEDIATE,
    NOSTR_RETRY_POLICY_EXPONENTIAL_BACKOFF
} NostrRetryPolicy;

/**
 * NostrSubscriptionConfig:
 * @type: The subscription type (ephemeral or persistent)
 * @timeout_ms: Timeout in milliseconds (0 for no timeout)
 * @retry_policy: The retry policy to use on failure
 * @max_events: Maximum number of events to receive (0 for unlimited)
 *
 * Configuration options for creating a Nostr subscription.
 *
 * This struct allows fine-grained control over subscription behavior,
 * including timeout handling, retry logic, and event limits.
 *
 * Example:
 * |[<!-- language="C" -->
 * NostrSubscriptionConfig config = {
 *     .type = NOSTR_SUBSCRIPTION_EPHEMERAL,
 *     .timeout_ms = 5000,
 *     .retry_policy = NOSTR_RETRY_POLICY_EXPONENTIAL_BACKOFF,
 *     .max_events = 100
 * };
 * ]|
 *
 * Since: 0.1
 */
typedef struct {
    NostrSubscriptionType type;
    guint timeout_ms;
    NostrRetryPolicy retry_policy;
    guint max_events;
} NostrSubscriptionConfig;

/**
 * NOSTR_SUBSCRIPTION_CONFIG_DEFAULT:
 *
 * Default subscription configuration.
 *
 * Creates a persistent subscription with no timeout, no retry, and unlimited events.
 *
 * Since: 0.1
 */
#define NOSTR_SUBSCRIPTION_CONFIG_DEFAULT \
    (NostrSubscriptionConfig) { \
        .type = NOSTR_SUBSCRIPTION_PERSISTENT, \
        .timeout_ms = 0, \
        .retry_policy = NOSTR_RETRY_POLICY_NONE, \
        .max_events = 0 \
    }

/* Define GNostrSubscription GObject (G-prefixed to avoid clashing with core) */
#define GNOSTR_TYPE_SUBSCRIPTION (gnostr_subscription_get_type())
G_DECLARE_FINAL_TYPE(GNostrSubscription, gnostr_subscription, GNOSTR, SUBSCRIPTION, GObject)

/**
 * GNostrSubscription:
 *
 * A GObject wrapper for Nostr subscriptions with reactive state management.
 *
 * GNostrSubscription provides a GObject-based interface for managing Nostr
 * subscriptions. It emits signals when state changes occur, enabling reactive
 * UI updates and event-driven programming.
 *
 * ## Signals
 *
 * - #GNostrSubscription::state-changed - Emitted when the subscription state changes
 * - #GNostrSubscription::event-received - Emitted when an event is received
 *
 * ## Properties
 *
 * - #GNostrSubscription:state - The current subscription state
 * - #GNostrSubscription:config - The subscription configuration
 *
 * Since: 0.1
 */
struct _GNostrSubscription {
    GObject parent_instance;

    /*< private >*/
    NostrSubscription *subscription;      /* core subscription pointer */
    NostrSubscriptionState state;         /* current lifecycle state */
    NostrSubscriptionConfig config;       /* subscription configuration */
    gchar *error_message;                 /* error message if state is ERROR */
    guint event_count;                    /* number of events received */
};

/* GObject convenience API */

/**
 * gnostr_subscription_new:
 * @relay: A #GNostrRelay to subscribe on
 * @filter: A #NostrFilter specifying the subscription criteria
 *
 * Creates a new subscription with default configuration.
 *
 * Returns: (transfer full): A new #GNostrSubscription instance
 *
 * Since: 0.1
 */
GNostrSubscription *gnostr_subscription_new(GNostrRelay *relay, NostrFilter *filter);

/**
 * gnostr_subscription_new_with_config:
 * @relay: A #GNostrRelay to subscribe on
 * @filter: A #NostrFilter specifying the subscription criteria
 * @config: A #NostrSubscriptionConfig with subscription options
 *
 * Creates a new subscription with the specified configuration.
 *
 * Returns: (transfer full): A new #GNostrSubscription instance
 *
 * Since: 0.1
 */
GNostrSubscription *gnostr_subscription_new_with_config(GNostrRelay *relay,
                                                         NostrFilter *filter,
                                                         NostrSubscriptionConfig *config);

/**
 * gnostr_subscription_unsubscribe:
 * @self: A #GNostrSubscription
 *
 * Closes the subscription and releases resources.
 *
 * This will transition the subscription to %NOSTR_SUBSCRIPTION_STATE_CLOSED
 * and emit the #GNostrSubscription::state-changed signal.
 *
 * Since: 0.1
 */
void gnostr_subscription_unsubscribe(GNostrSubscription *self);

/**
 * gnostr_subscription_get_state:
 * @self: A #GNostrSubscription
 *
 * Gets the current subscription state.
 *
 * Returns: The current #NostrSubscriptionState
 *
 * Since: 0.1
 */
NostrSubscriptionState gnostr_subscription_get_state(GNostrSubscription *self);

/**
 * gnostr_subscription_get_config:
 * @self: A #GNostrSubscription
 *
 * Gets the subscription configuration.
 *
 * Returns: (transfer none): The #NostrSubscriptionConfig for this subscription
 *
 * Since: 0.1
 */
const NostrSubscriptionConfig *gnostr_subscription_get_config(GNostrSubscription *self);

/**
 * gnostr_subscription_get_error_message:
 * @self: A #GNostrSubscription
 *
 * Gets the error message if the subscription is in error state.
 *
 * Returns: (nullable) (transfer none): The error message, or %NULL if no error
 *
 * Since: 0.1
 */
const gchar *gnostr_subscription_get_error_message(GNostrSubscription *self);

/**
 * gnostr_subscription_get_event_count:
 * @self: A #GNostrSubscription
 *
 * Gets the number of events received by this subscription.
 *
 * Returns: The event count
 *
 * Since: 0.1
 */
guint gnostr_subscription_get_event_count(GNostrSubscription *self);

G_END_DECLS

#endif /* NOSTR_GSUBSCRIPTION_H */