/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 gnostr contributors
 *
 * nostr_event_bus.h - Central event routing for reactive architecture
 *
 * The EventBus provides a publish-subscribe mechanism for routing Nostr
 * events throughout the application. Components subscribe to topic patterns
 * and receive events matching those patterns.
 */

#ifndef NOSTR_EVENT_BUS_H
#define NOSTR_EVENT_BUS_H

#include <glib-object.h>

G_BEGIN_DECLS

/* --- Topic Pattern Format ---
 *
 * Topics use a hierarchical namespace with :: separators:
 *
 *   event::kind::<kind>    - Events by kind number (e.g., "event::kind::1")
 *   event::author::<pubkey> - Events by author pubkey
 *   event::id::<id>        - Specific event by ID
 *   eose::<sub-id>         - End of stored events for subscription
 *   ok::<event-id>         - Relay acknowledgment for published event
 *   notice::<relay-url>    - Relay notice messages
 *   error::<context>       - Error notifications
 *
 * Wildcards:
 *   *   - Matches any single segment (e.g., "event::kind::*")
 *   **  - Matches zero or more segments (e.g., "event::**")
 */

/* --- Forward Declarations --- */

typedef struct _NostrEventBus NostrEventBus;
typedef struct _NostrEventBusClass NostrEventBusClass;
typedef struct _NostrEventBusHandle NostrEventBusHandle;

/**
 * NostrEventBusCallback:
 * @topic: The topic that matched the subscription pattern
 * @event_json: The event payload as a JSON string (may be NULL for some topics)
 * @user_data: User data passed to nostr_event_bus_subscribe()
 *
 * Callback signature for event bus subscriptions.
 */
typedef void (*NostrEventBusCallback)(const gchar *topic,
                                       const gchar *event_json,
                                       gpointer user_data);

/* --- GObject Type Definition --- */

#define NOSTR_TYPE_EVENT_BUS (nostr_event_bus_get_type())
G_DECLARE_DERIVABLE_TYPE(NostrEventBus, nostr_event_bus, NOSTR, EVENT_BUS, GObject)

/**
 * NostrEventBusClass:
 * @parent_class: The parent class
 *
 * Class structure for #NostrEventBus. Can be subclassed for custom
 * event routing implementations.
 */
struct _NostrEventBusClass {
    GObjectClass parent_class;

    /* Virtual methods for subclassing */
    NostrEventBusHandle *(*subscribe)(NostrEventBus *bus,
                                      const gchar *topic_pattern,
                                      NostrEventBusCallback callback,
                                      gpointer user_data,
                                      GDestroyNotify destroy_notify);
    void (*unsubscribe)(NostrEventBus *bus, NostrEventBusHandle *handle);
    void (*emit)(NostrEventBus *bus, const gchar *topic, const gchar *event_json);
    void (*emit_batch)(NostrEventBus *bus, const gchar *topic,
                       const gchar *const *events_array, gsize count);

    /*< private >*/
    gpointer padding[8];
};

/* --- Filter Predicate --- */

/**
 * NostrEventBusFilterFunc:
 * @topic: The topic being tested
 * @event_json: The event payload (may be NULL)
 * @user_data: User data passed to nostr_event_bus_subscribe_filtered()
 *
 * Predicate function for fine-grained filtering of events.
 * Called after topic pattern matching succeeds but before
 * the callback is invoked.
 *
 * Returns: %TRUE if the event should be delivered, %FALSE to skip
 */
typedef gboolean (*NostrEventBusFilterFunc)(const gchar *topic,
                                            const gchar *event_json,
                                            gpointer user_data);

/* --- Handle Type --- */

/**
 * NostrEventBusHandle:
 *
 * Opaque handle representing an active subscription. Used to
 * unsubscribe from the event bus. The handle is valid from the
 * time it is returned by subscribe until unsubscribe is called.
 *
 * Do not free this handle directly; always use
 * nostr_event_bus_unsubscribe().
 */
struct _NostrEventBusHandle {
    /*< private >*/
    guint64 id;
    gpointer _reserved[3];
};

/* --- Singleton Accessor --- */

/**
 * nostr_event_bus_get_default:
 *
 * Gets the default (singleton) event bus instance. The default bus
 * is created on first access and persists for the lifetime of the
 * application. Thread-safe.
 *
 * Returns: (transfer none): The default #NostrEventBus instance.
 *          Do not unref this instance.
 */
NostrEventBus *nostr_event_bus_get_default(void);

/* --- Subscription API --- */

/**
 * nostr_event_bus_subscribe:
 * @bus: A #NostrEventBus
 * @topic_pattern: A topic pattern to subscribe to (supports wildcards)
 * @callback: (scope notified): Function to call when matching events arrive
 * @user_data: (nullable): User data passed to @callback
 *
 * Subscribes to events matching the given topic pattern. The callback
 * will be invoked for each event emitted on a matching topic.
 *
 * Common topic patterns:
 * - "event::kind::1" - All kind-1 (short text notes)
 * - "event::kind::*" - All events by kind
 * - "eose::*" - All EOSE notifications
 * - "ok::*" - All OK acknowledgments
 *
 * Returns: (transfer none): A handle to the subscription. Use this
 *          with nostr_event_bus_unsubscribe() to cancel the subscription.
 *          Returns %NULL on error.
 */
NostrEventBusHandle *nostr_event_bus_subscribe(NostrEventBus *bus,
                                               const gchar *topic_pattern,
                                               NostrEventBusCallback callback,
                                               gpointer user_data);

/**
 * nostr_event_bus_subscribe_full:
 * @bus: A #NostrEventBus
 * @topic_pattern: A topic pattern to subscribe to
 * @callback: (scope notified): Function to call when matching events arrive
 * @user_data: (nullable): User data passed to @callback
 * @destroy_notify: (nullable): Function to free @user_data when unsubscribing
 *
 * Like nostr_event_bus_subscribe() but with a destroy notify function
 * for automatic cleanup of user_data.
 *
 * Returns: (transfer none): A subscription handle, or %NULL on error
 */
NostrEventBusHandle *nostr_event_bus_subscribe_full(NostrEventBus *bus,
                                                    const gchar *topic_pattern,
                                                    NostrEventBusCallback callback,
                                                    gpointer user_data,
                                                    GDestroyNotify destroy_notify);

/**
 * nostr_event_bus_subscribe_filtered:
 * @bus: A #NostrEventBus
 * @topic_pattern: A topic pattern to subscribe to
 * @filter_func: (scope notified): Predicate to filter events
 * @callback: (scope notified): Function to call for matching events
 * @user_data: (nullable): User data passed to both functions
 * @destroy_notify: (nullable): Function to free @user_data
 *
 * Subscribes with an additional filter predicate for fine-grained routing.
 * The filter is called after topic pattern matching; if it returns %FALSE,
 * the callback is not invoked for that event.
 *
 * Use cases:
 * - Filter events by author pubkey
 * - Filter by tag content
 * - Rate limiting per subscriber
 *
 * Returns: (transfer none): A subscription handle, or %NULL on error
 */
NostrEventBusHandle *nostr_event_bus_subscribe_filtered(NostrEventBus *bus,
                                                        const gchar *topic_pattern,
                                                        NostrEventBusFilterFunc filter_func,
                                                        NostrEventBusCallback callback,
                                                        gpointer user_data,
                                                        GDestroyNotify destroy_notify);

/**
 * nostr_event_bus_unsubscribe:
 * @bus: A #NostrEventBus
 * @handle: A subscription handle from nostr_event_bus_subscribe()
 *
 * Cancels a subscription. After this call, the callback will no longer
 * be invoked for any events. If a destroy_notify was provided during
 * subscription, it will be called to free user_data.
 *
 * It is safe to call this from within a callback, but the current
 * callback invocation will complete.
 *
 * Passing %NULL for @handle is a no-op.
 */
void nostr_event_bus_unsubscribe(NostrEventBus *bus,
                                 NostrEventBusHandle *handle);

/* --- Emit API --- */

/**
 * nostr_event_bus_emit:
 * @bus: A #NostrEventBus
 * @topic: The topic to emit on
 * @event_json: (nullable): The event payload as JSON
 *
 * Emits an event on the specified topic. All subscribers with matching
 * topic patterns will have their callbacks invoked synchronously in
 * registration order.
 *
 * The topic should follow the naming conventions:
 * - "event::kind::<N>" for Nostr events by kind
 * - "eose::<sub-id>" for EOSE notifications
 * - "ok::<event-id>" for relay acknowledgments
 *
 * Thread-safe. Callbacks are invoked on the calling thread.
 */
void nostr_event_bus_emit(NostrEventBus *bus,
                          const gchar *topic,
                          const gchar *event_json);

/**
 * nostr_event_bus_emit_batch:
 * @bus: A #NostrEventBus
 * @topic: The topic to emit on
 * @events_array: (array length=count) (element-type utf8): Array of event JSON strings
 * @count: Number of events in the array
 *
 * Emits multiple events on the same topic in a single batch. This is
 * more efficient than calling nostr_event_bus_emit() repeatedly when
 * processing a batch of events from a relay.
 *
 * Each subscriber callback is invoked once per event in the batch.
 * Events are delivered in array order.
 *
 * Thread-safe. Callbacks are invoked on the calling thread.
 */
void nostr_event_bus_emit_batch(NostrEventBus *bus,
                                const gchar *topic,
                                const gchar *const *events_array,
                                gsize count);

/* --- Utility Functions --- */

/**
 * nostr_event_bus_topic_matches:
 * @pattern: A topic pattern (may include wildcards)
 * @topic: A concrete topic to test
 *
 * Tests whether a topic matches a pattern. Useful for debugging
 * or implementing custom routing logic.
 *
 * Wildcard rules:
 * - "*" matches exactly one segment
 * - "**" matches zero or more segments
 * - Segments are separated by "::"
 *
 * Examples:
 * - "event::kind::1" matches "event::kind::1"
 * - "event::kind::*" matches "event::kind::1", "event::kind::7"
 * - "event::**" matches "event::kind::1", "event::author::abc"
 *
 * Returns: %TRUE if the topic matches the pattern
 */
gboolean nostr_event_bus_topic_matches(const gchar *pattern,
                                       const gchar *topic);

/**
 * nostr_event_bus_format_event_topic:
 * @kind: The Nostr event kind number
 *
 * Formats a topic string for an event kind. Convenience function
 * to avoid manual string formatting.
 *
 * Returns: (transfer full): A newly allocated topic string like
 *          "event::kind::1". Free with g_free().
 */
gchar *nostr_event_bus_format_event_topic(gint kind);

/**
 * nostr_event_bus_format_eose_topic:
 * @subscription_id: The subscription ID
 *
 * Formats an EOSE topic string for a subscription.
 *
 * Returns: (transfer full): A newly allocated topic string like
 *          "eose::sub123". Free with g_free().
 */
gchar *nostr_event_bus_format_eose_topic(const gchar *subscription_id);

/**
 * nostr_event_bus_format_ok_topic:
 * @event_id: The event ID
 *
 * Formats an OK topic string for an event acknowledgment.
 *
 * Returns: (transfer full): A newly allocated topic string like
 *          "ok::abc123...". Free with g_free().
 */
gchar *nostr_event_bus_format_ok_topic(const gchar *event_id);

/* --- Statistics --- */

/**
 * NostrEventBusStats:
 * @subscription_count: Number of active subscriptions
 * @events_emitted: Total events emitted since creation
 * @callbacks_invoked: Total callback invocations
 * @pattern_cache_hits: Topic pattern cache hits
 * @pattern_cache_misses: Topic pattern cache misses
 * @dispatch_latency_p50_ns: 50th percentile dispatch latency (nanoseconds)
 * @dispatch_latency_p95_ns: 95th percentile dispatch latency (nanoseconds)
 * @dispatch_latency_p99_ns: 99th percentile dispatch latency (nanoseconds)
 * @dispatch_latency_min_ns: Minimum dispatch latency observed (nanoseconds)
 * @dispatch_latency_max_ns: Maximum dispatch latency observed (nanoseconds)
 * @dispatch_count: Total number of emit() calls measured
 * @events_dropped: Events not delivered (filtered or cancelled during dispatch)
 *
 * Statistics for monitoring event bus performance.
 */
typedef struct {
    guint subscription_count;
    guint64 events_emitted;
    guint64 callbacks_invoked;
    guint64 pattern_cache_hits;
    guint64 pattern_cache_misses;

    /* Dispatch latency histogram percentiles (nanoseconds) */
    guint64 dispatch_latency_p50_ns;
    guint64 dispatch_latency_p95_ns;
    guint64 dispatch_latency_p99_ns;
    guint64 dispatch_latency_min_ns;
    guint64 dispatch_latency_max_ns;
    guint64 dispatch_count;

    /* Dropped events */
    guint64 events_dropped;
} NostrEventBusStats;

/**
 * nostr_event_bus_get_stats:
 * @bus: A #NostrEventBus
 * @stats: (out): Output structure for statistics
 *
 * Retrieves current statistics for the event bus. Latency
 * percentiles are computed from an internal histogram with
 * exponential bin boundaries (1 us base, 1.5x growth factor).
 */
void nostr_event_bus_get_stats(NostrEventBus *bus,
                               NostrEventBusStats *stats);

/**
 * nostr_event_bus_reset_stats:
 * @bus: A #NostrEventBus
 *
 * Resets all counters and the latency histogram to zero.
 * Active subscriptions are not affected.
 */
void nostr_event_bus_reset_stats(NostrEventBus *bus);

G_END_DECLS

#endif /* NOSTR_EVENT_BUS_H */
