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
 * Lossless mode still has a hard ceiling, far above any legitimate backfill
 * (see gnostr_subscription_set_backlog_limit()): a relay that outpaces the
 * main loop past it ends the subscription with an explicit
 * #GNOSTR_SUBSCRIPTION_OVERFLOW_REASON "closed", never a silent drop.
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

/**
 * GNOSTR_SUBSCRIPTION_DEFAULT_MAX_BACKLOG_EVENTS:
 *
 * The default ceiling on events waiting for the main loop in lossless mode.
 *
 * Since: 2.2
 */
#define GNOSTR_SUBSCRIPTION_DEFAULT_MAX_BACKLOG_EVENTS 100000u

/**
 * GNOSTR_SUBSCRIPTION_DEFAULT_MAX_BACKLOG_BYTES:
 *
 * The default ceiling on the serialized size of the events waiting for the
 * main loop in lossless mode (64 MiB).
 *
 * Since: 2.2
 */
#define GNOSTR_SUBSCRIPTION_DEFAULT_MAX_BACKLOG_BYTES ((guint64)64 * 1024 * 1024)

/**
 * GNOSTR_SUBSCRIPTION_OVERFLOW_PREFIX:
 *
 * The machine-readable prefix (NIP-01 CLOSED style) of the reason a
 * subscription reports when its backlog ceiling ended it. A relay never
 * sends it; see #GNOSTR_SUBSCRIPTION_OVERFLOW_REASON.
 *
 * Since: 2.2
 */
#define GNOSTR_SUBSCRIPTION_OVERFLOW_PREFIX "overflow:"

/**
 * GNOSTR_SUBSCRIPTION_OVERFLOW_REASON:
 *
 * The "closed" reason of a lossless subscription whose backlog ceiling was
 * reached (nostrc-5rfp).
 *
 * Since: 2.2
 */
#define GNOSTR_SUBSCRIPTION_OVERFLOW_REASON \
    GNOSTR_SUBSCRIPTION_OVERFLOW_PREFIX " event backlog exceeded the subscription's limit"

/**
 * gnostr_subscription_set_backlog_limit:
 * @self: a #GNostrSubscription
 * @max_events: most events that may wait for the main loop, or 0 for
 *   #GNOSTR_SUBSCRIPTION_DEFAULT_MAX_BACKLOG_EVENTS
 * @max_bytes: most bytes of serialized event JSON that may wait, or 0 for
 *   #GNOSTR_SUBSCRIPTION_DEFAULT_MAX_BACKLOG_BYTES
 *
 * The hard ceiling of lossless mode (nostrc-5rfp). An event that would take
 * the backlog past either limit is not queued. Instead the subscription
 * stops reading, sends CLOSE to the relay, logs a warning, and queues a
 * "closed" with the reason #GNOSTR_SUBSCRIPTION_OVERFLOW_REASON. That
 * "closed" is emitted in order, after every event queued before it, and
 * nothing follows it: the handler knows exactly what it received and that
 * the rest of the answer is missing, and can subscribe again (for instance
 * paging older events with `until`). Bounded mode is not affected (it keeps
 * at most 200 events). May be called at any time; it applies to events
 * queued afterwards.
 *
 * Since: 2.2
 */
void gnostr_subscription_set_backlog_limit(GNostrSubscription *self,
                                           guint max_events, guint64 max_bytes);

/**
 * gnostr_subscription_get_backlog_limit:
 * @self: a #GNostrSubscription
 * @max_events: (out) (optional): the event ceiling
 * @max_bytes: (out) (optional): the byte ceiling
 *
 * See gnostr_subscription_set_backlog_limit().
 *
 * Since: 2.2
 */
void gnostr_subscription_get_backlog_limit(GNostrSubscription *self,
                                           guint *max_events, guint64 *max_bytes);

/**
 * gnostr_subscription_get_overflowed:
 * @self: a #GNostrSubscription
 *
 * Returns: whether the backlog ceiling ended @self (its "closed" carries
 *   #GNOSTR_SUBSCRIPTION_OVERFLOW_REASON, or is still queued)
 *
 * Since: 2.2
 */
gboolean gnostr_subscription_get_overflowed(GNostrSubscription *self);

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
