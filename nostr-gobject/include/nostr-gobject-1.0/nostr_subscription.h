#ifndef NOSTR_GSUBSCRIPTION_H
#define NOSTR_GSUBSCRIPTION_H

#include <glib-object.h>
#include "nostr-enums.h"
#include "nostr_relay.h"

G_BEGIN_DECLS

/* Forward declarations for core types */
#ifndef NOSTR_SUBSCRIPTION_FORWARD_DECLARED
#define NOSTR_SUBSCRIPTION_FORWARD_DECLARED
struct NostrSubscription;
typedef struct NostrSubscription NostrSubscription;
#endif

#ifndef NOSTR_FILTERS_FORWARD_DECLARED
#define NOSTR_FILTERS_FORWARD_DECLARED
typedef struct NostrFilters NostrFilters;
#endif

/* Define GNostrSubscription GObject (G-prefixed to avoid clashing with core) */
#define GNOSTR_TYPE_SUBSCRIPTION (gnostr_subscription_get_type())
G_DECLARE_FINAL_TYPE(GNostrSubscription, gnostr_subscription, GNOSTR, SUBSCRIPTION, GObject)

/**
 * GNostrSubscription:
 *
 * A GObject wrapper for Nostr subscriptions with reactive lifecycle management.
 *
 * GNostrSubscription provides a signal-driven interface for managing Nostr
 * subscriptions. A monitor thread drains core GoChannels and emits GObject
 * signals on the main thread, enabling reactive UI updates.
 *
 * ## Lifecycle
 *
 * 1. Create: gnostr_subscription_new() → state = PENDING
 * 2. Fire: gnostr_subscription_fire() → state = ACTIVE, monitor starts
 * 3. Receive: "event" signals emitted as events arrive
 * 4. EOSE: "eose" signal emitted, state = EOSE_RECEIVED
 * 5. Close: gnostr_subscription_close() → state = CLOSED, monitor stops
 *
 * ## Signals
 *
 * - #GNostrSubscription::event - Emitted when an event is received
 * - #GNostrSubscription::eose - Emitted when End of Stored Events is received
 * - #GNostrSubscription::closed - Emitted when the subscription is closed
 * - #GNostrSubscription::state-changed - Emitted on state transitions
 *
 * ## Properties
 *
 * - #GNostrSubscription:id - The subscription ID (read-only)
 * - #GNostrSubscription:active - Whether the subscription is live (read-only)
 * - #GNostrSubscription:state - The lifecycle state (read-only)
 *
 * Since: 1.0
 */

/* Signal indices */
enum {
    GNOSTR_SUBSCRIPTION_SIGNAL_EVENT,
    GNOSTR_SUBSCRIPTION_SIGNAL_EOSE,
    GNOSTR_SUBSCRIPTION_SIGNAL_CLOSED,
    GNOSTR_SUBSCRIPTION_SIGNAL_STATE_CHANGED,
    GNOSTR_SUBSCRIPTION_SIGNALS_COUNT
};

/* --- Constructors --- */

/**
 * gnostr_subscription_new:
 * @relay: a #GNostrRelay to subscribe on
 * @filters: (transfer none): core NostrFilters for the subscription
 *
 * Creates a new subscription in PENDING state. Call gnostr_subscription_fire()
 * to activate it and start receiving events.
 *
 * Returns: (transfer full): a new #GNostrSubscription
 *
 * Since: 1.0
 */
GNostrSubscription *gnostr_subscription_new(GNostrRelay *relay, NostrFilters *filters);

/**
 * gnostr_subscription_fire:
 * @self: a #GNostrSubscription
 * @error: (nullable): return location for a #GError
 *
 * Sends the REQ message to the relay and starts the monitor thread.
 * Transitions from PENDING to ACTIVE state.
 *
 * Returns: %TRUE on success, %FALSE on error
 *
 * Since: 1.0
 */
gboolean gnostr_subscription_fire(GNostrSubscription *self, GError **error);

/**
 * gnostr_subscription_close:
 * @self: a #GNostrSubscription
 *
 * Closes the subscription, sends CLOSE to the relay, and stops the
 * monitor thread. Transitions to CLOSED state and emits the "closed" signal.
 *
 * Safe to call multiple times; subsequent calls are no-ops.
 *
 * Since: 1.0
 */
void gnostr_subscription_close(GNostrSubscription *self);

/**
 * gnostr_subscription_set_lossless:
 * @self: a #GNostrSubscription
 * @lossless: %TRUE to deliver every event, %FALSE to bound the queue
 *
 * Chooses how events wait between the relay and the main loop, where the
 * "event" signal is emitted in small time-sliced batches.
 *
 * Lossless (the default): no event is ever dropped. The monitor thread keeps
 * libnostr's subscription channel drained (libnostr itself drops events when
 * that channel is full), so a burst the main loop has not caught up with
 * waits in memory. Use this whenever the "event" handler is where events are
 * kept, e.g. a DM inbox or group history backfill.
 *
 * Bounded (%FALSE): at most 200 events wait; when full, the oldest waiting
 * event is dropped. Only for UIs whose events are also persisted by another
 * path (Gnostr ingests relay events into nostrdb), where a stale backlog is
 * worth less than a responsive main loop.
 *
 * In both modes "eose" and "closed" are never dropped, and "event", "eose"
 * and "closed" are emitted in the order the relay sent them. May be called at
 * any time; it applies to events queued afterwards. (nostrc-dha5)
 *
 * Since: 2.1
 */
void gnostr_subscription_set_lossless(GNostrSubscription *self, gboolean lossless);

/**
 * gnostr_subscription_get_lossless:
 * @self: a #GNostrSubscription
 *
 * Returns: whether @self delivers every event; see
 *   gnostr_subscription_set_lossless()
 *
 * Since: 2.1
 */
gboolean gnostr_subscription_get_lossless(GNostrSubscription *self);

/* --- Property Accessors --- */

/**
 * gnostr_subscription_get_id:
 * @self: a #GNostrSubscription
 *
 * Gets the subscription ID assigned by the core library.
 *
 * Returns: (transfer none) (nullable): the subscription ID string
 *
 * Since: 1.0
 */
const gchar *gnostr_subscription_get_id(GNostrSubscription *self);

/**
 * gnostr_subscription_get_active:
 * @self: a #GNostrSubscription
 *
 * Gets whether the subscription is currently active (live).
 *
 * Returns: %TRUE if the subscription is active
 *
 * Since: 1.0
 */
gboolean gnostr_subscription_get_active(GNostrSubscription *self);

/**
 * gnostr_subscription_get_state:
 * @self: a #GNostrSubscription
 *
 * Gets the current lifecycle state.
 *
 * Returns: the current #GNostrSubscriptionState
 *
 * Since: 1.0
 */
GNostrSubscriptionState gnostr_subscription_get_state(GNostrSubscription *self);

/**
 * gnostr_subscription_get_relay:
 * @self: a #GNostrSubscription
 *
 * Gets the relay this subscription is associated with.
 *
 * Returns: (transfer none) (nullable): the #GNostrRelay
 *
 * Since: 1.0
 */
GNostrRelay *gnostr_subscription_get_relay(GNostrSubscription *self);

/**
 * gnostr_subscription_get_event_count:
 * @self: a #GNostrSubscription
 *
 * Gets the number of events received by this subscription.
 *
 * Returns: the event count
 *
 * Since: 1.0
 */
guint gnostr_subscription_get_event_count(GNostrSubscription *self);

/**
 * gnostr_subscription_get_core_subscription:
 * @self: a #GNostrSubscription
 *
 * Gets the underlying core NostrSubscription pointer.
 * For advanced use cases requiring direct libnostr API access.
 *
 * Returns: (transfer none) (nullable): the core NostrSubscription pointer
 *
 * Since: 1.0
 */
NostrSubscription *gnostr_subscription_get_core_subscription(GNostrSubscription *self);

G_END_DECLS

#endif /* NOSTR_GSUBSCRIPTION_H */
