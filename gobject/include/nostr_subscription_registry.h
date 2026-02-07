/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 gnostr contributors
 *
 * nostr_subscription_registry.h - Central subscription tracking and management
 *
 * The SubscriptionRegistry tracks all active subscriptions, manages their
 * lifecycle, and provides batch operations for subscription groups.
 */

#ifndef NOSTR_SUBSCRIPTION_REGISTRY_H
#define NOSTR_SUBSCRIPTION_REGISTRY_H

#include <glib-object.h>

G_BEGIN_DECLS

/* --- Forward Declarations --- */

typedef struct _NostrSubscriptionRegistry NostrSubscriptionRegistry;
typedef struct _NostrSubscriptionRegistryClass NostrSubscriptionRegistryClass;
typedef struct _NostrSubscriptionGroup NostrSubscriptionGroup;

/* Forward declare GNostrSubscription to avoid circular includes */
typedef struct _GNostrSubscription GNostrSubscription;

/* --- Subscription State Enum (duplicated to avoid include conflicts) --- */

/**
 * NostrSubscriptionState:
 * @NOSTR_SUBSCRIPTION_STATE_PENDING: Subscription created but not yet sent to relay
 * @NOSTR_SUBSCRIPTION_STATE_ACTIVE: Subscription is active and receiving events
 * @NOSTR_SUBSCRIPTION_STATE_EOSE_RECEIVED: End of stored events received from relay
 * @NOSTR_SUBSCRIPTION_STATE_CLOSED: Subscription has been closed
 * @NOSTR_SUBSCRIPTION_STATE_ERROR: Subscription encountered an error
 *
 * Represents the lifecycle state of a Nostr subscription.
 * Note: This is duplicated here to avoid header conflicts.
 */
#ifndef NOSTR_SUBSCRIPTION_STATE_DEFINED
#define NOSTR_SUBSCRIPTION_STATE_DEFINED
typedef enum {
    NOSTR_SUBSCRIPTION_STATE_PENDING,
    NOSTR_SUBSCRIPTION_STATE_ACTIVE,
    NOSTR_SUBSCRIPTION_STATE_EOSE_RECEIVED,
    NOSTR_SUBSCRIPTION_STATE_CLOSED,
    NOSTR_SUBSCRIPTION_STATE_ERROR
} NostrSubscriptionState;
#endif

/* --- Subscription Group --- */

/**
 * NostrSubscriptionGroup:
 *
 * Opaque handle representing a group of related subscriptions.
 * Groups enable batch operations like closing all subscriptions
 * for a specific view or component.
 */
struct _NostrSubscriptionGroup {
    /*< private >*/
    gchar *name;
    GHashTable *subscriptions;  /* sub_id -> GNostrSubscription */
    gpointer _reserved[2];
};

/* --- State Change Callback --- */

/**
 * NostrSubscriptionStateCallback:
 * @registry: The registry that emitted the notification
 * @sub_id: The subscription ID
 * @old_state: The previous subscription state
 * @new_state: The new subscription state
 * @user_data: User data passed to the callback
 *
 * Callback signature for subscription state change notifications.
 */
typedef void (*NostrSubscriptionStateCallback)(NostrSubscriptionRegistry *registry,
                                                const gchar *sub_id,
                                                NostrSubscriptionState old_state,
                                                NostrSubscriptionState new_state,
                                                gpointer user_data);

/* --- GObject Type Definition --- */

#define NOSTR_TYPE_SUBSCRIPTION_REGISTRY (nostr_subscription_registry_get_type())
G_DECLARE_DERIVABLE_TYPE(NostrSubscriptionRegistry, nostr_subscription_registry,
                         NOSTR, SUBSCRIPTION_REGISTRY, GObject)

/**
 * NostrSubscriptionRegistryClass:
 * @parent_class: The parent class
 *
 * Class structure for #NostrSubscriptionRegistry. Can be subclassed
 * for custom subscription management implementations.
 */
struct _NostrSubscriptionRegistryClass {
    GObjectClass parent_class;

    /* Virtual methods for subclassing */
    gchar *(*register_subscription)(NostrSubscriptionRegistry *registry,
                                    GNostrSubscription *subscription,
                                    const gchar *group_name);
    gboolean (*unregister)(NostrSubscriptionRegistry *registry,
                           const gchar *sub_id);
    GNostrSubscription *(*get_by_id)(NostrSubscriptionRegistry *registry,
                                     const gchar *sub_id);
    void (*notify_eose)(NostrSubscriptionRegistry *registry,
                        const gchar *sub_id);

    /*< private >*/
    gpointer padding[8];
};

/* --- Singleton Accessor --- */

/**
 * nostr_subscription_registry_get_default:
 *
 * Gets the default (singleton) registry instance. The default registry
 * is created on first access and persists for the lifetime of the
 * application. Thread-safe.
 *
 * Returns: (transfer none): The default #NostrSubscriptionRegistry instance.
 *          Do not unref this instance.
 */
NostrSubscriptionRegistry *nostr_subscription_registry_get_default(void);

/* --- Registration API --- */

/**
 * nostr_subscription_registry_register:
 * @registry: A #NostrSubscriptionRegistry
 * @subscription: A #GNostrSubscription to register
 *
 * Registers a subscription with the registry and generates a unique
 * subscription ID. The registry takes a reference to the subscription.
 *
 * Returns: (transfer full): A newly allocated subscription ID string.
 *          Free with g_free(). Returns %NULL on error.
 */
gchar *nostr_subscription_registry_register(NostrSubscriptionRegistry *registry,
                                            GNostrSubscription *subscription);

/**
 * nostr_subscription_registry_register_with_group:
 * @registry: A #NostrSubscriptionRegistry
 * @subscription: A #GNostrSubscription to register
 * @group_name: (nullable): Name of the group to add the subscription to
 *
 * Registers a subscription and optionally adds it to a named group.
 * Groups are created automatically if they don't exist.
 *
 * Returns: (transfer full): A newly allocated subscription ID string.
 *          Free with g_free(). Returns %NULL on error.
 */
gchar *nostr_subscription_registry_register_with_group(NostrSubscriptionRegistry *registry,
                                                       GNostrSubscription *subscription,
                                                       const gchar *group_name);

/**
 * nostr_subscription_registry_unregister:
 * @registry: A #NostrSubscriptionRegistry
 * @sub_id: The subscription ID to unregister
 *
 * Unregisters a subscription from the registry. The registry releases
 * its reference to the subscription.
 *
 * Returns: %TRUE if the subscription was found and unregistered,
 *          %FALSE if not found
 */
gboolean nostr_subscription_registry_unregister(NostrSubscriptionRegistry *registry,
                                                 const gchar *sub_id);

/* --- Lookup API --- */

/**
 * nostr_subscription_registry_get_by_id:
 * @registry: A #NostrSubscriptionRegistry
 * @sub_id: The subscription ID to look up
 *
 * Retrieves a subscription by its ID.
 *
 * Returns: (transfer none) (nullable): The #GNostrSubscription, or %NULL
 *          if not found. Do not unref.
 */
GNostrSubscription *nostr_subscription_registry_get_by_id(NostrSubscriptionRegistry *registry,
                                                          const gchar *sub_id);

/**
 * nostr_subscription_registry_get_active_count:
 * @registry: A #NostrSubscriptionRegistry
 *
 * Gets the number of currently active (non-closed) subscriptions.
 *
 * Returns: The count of active subscriptions
 */
guint nostr_subscription_registry_get_active_count(NostrSubscriptionRegistry *registry);

/**
 * nostr_subscription_registry_get_total_count:
 * @registry: A #NostrSubscriptionRegistry
 *
 * Gets the total number of registered subscriptions (including closed).
 *
 * Returns: The total subscription count
 */
guint nostr_subscription_registry_get_total_count(NostrSubscriptionRegistry *registry);

/* --- EOSE Handling --- */

/**
 * nostr_subscription_registry_notify_eose:
 * @registry: A #NostrSubscriptionRegistry
 * @sub_id: The subscription ID that received EOSE
 *
 * Notifies the registry that a subscription has received EOSE
 * (End of Stored Events). For ephemeral subscriptions, this will
 * trigger automatic cleanup after the notification is processed.
 */
void nostr_subscription_registry_notify_eose(NostrSubscriptionRegistry *registry,
                                              const gchar *sub_id);

/* --- Relay Limits --- */

/**
 * nostr_subscription_registry_set_max_per_relay:
 * @registry: A #NostrSubscriptionRegistry
 * @max_subscriptions: Maximum concurrent subscriptions per relay (0 for unlimited)
 *
 * Sets the maximum number of concurrent subscriptions allowed per relay.
 * When the limit is reached, new subscription attempts will be queued.
 */
void nostr_subscription_registry_set_max_per_relay(NostrSubscriptionRegistry *registry,
                                                    guint max_subscriptions);

/**
 * nostr_subscription_registry_get_max_per_relay:
 * @registry: A #NostrSubscriptionRegistry
 *
 * Gets the maximum subscriptions per relay setting.
 *
 * Returns: The maximum subscriptions per relay (0 means unlimited)
 */
guint nostr_subscription_registry_get_max_per_relay(NostrSubscriptionRegistry *registry);

/**
 * nostr_subscription_registry_get_relay_subscription_count:
 * @registry: A #NostrSubscriptionRegistry
 * @relay_url: The relay URL to check
 *
 * Gets the number of active subscriptions for a specific relay.
 *
 * Returns: The subscription count for the relay
 */
guint nostr_subscription_registry_get_relay_subscription_count(NostrSubscriptionRegistry *registry,
                                                                const gchar *relay_url);

/* --- State Change Notifications --- */

/**
 * nostr_subscription_registry_add_state_callback:
 * @registry: A #NostrSubscriptionRegistry
 * @callback: The callback function
 * @user_data: (nullable): User data passed to the callback
 * @destroy_notify: (nullable): Function to free user_data when removed
 *
 * Adds a callback to be notified when any subscription changes state.
 *
 * Returns: A callback ID that can be used to remove the callback
 */
guint nostr_subscription_registry_add_state_callback(NostrSubscriptionRegistry *registry,
                                                      NostrSubscriptionStateCallback callback,
                                                      gpointer user_data,
                                                      GDestroyNotify destroy_notify);

/**
 * nostr_subscription_registry_remove_state_callback:
 * @registry: A #NostrSubscriptionRegistry
 * @callback_id: The callback ID returned by add_state_callback
 *
 * Removes a previously registered state change callback.
 */
void nostr_subscription_registry_remove_state_callback(NostrSubscriptionRegistry *registry,
                                                        guint callback_id);

/* --- Group Operations --- */

/**
 * nostr_subscription_registry_create_group:
 * @registry: A #NostrSubscriptionRegistry
 * @group_name: The name for the new group
 *
 * Creates a new subscription group. Groups are used to batch
 * operations on related subscriptions.
 *
 * Returns: (transfer none) (nullable): The created group, or %NULL if
 *          a group with that name already exists
 */
NostrSubscriptionGroup *nostr_subscription_registry_create_group(NostrSubscriptionRegistry *registry,
                                                                  const gchar *group_name);

/**
 * nostr_subscription_registry_get_group:
 * @registry: A #NostrSubscriptionRegistry
 * @group_name: The group name to look up
 *
 * Retrieves a subscription group by name.
 *
 * Returns: (transfer none) (nullable): The #NostrSubscriptionGroup,
 *          or %NULL if not found
 */
NostrSubscriptionGroup *nostr_subscription_registry_get_group(NostrSubscriptionRegistry *registry,
                                                               const gchar *group_name);

/**
 * nostr_subscription_registry_close_group:
 * @registry: A #NostrSubscriptionRegistry
 * @group_name: The group name
 *
 * Closes all subscriptions in a group and removes the group.
 * Each subscription in the group will be unsubscribed and unregistered.
 *
 * Returns: The number of subscriptions that were closed
 */
guint nostr_subscription_registry_close_group(NostrSubscriptionRegistry *registry,
                                               const gchar *group_name);

/**
 * nostr_subscription_registry_add_to_group:
 * @registry: A #NostrSubscriptionRegistry
 * @sub_id: The subscription ID to add
 * @group_name: The group name
 *
 * Adds an existing subscription to a group.
 *
 * Returns: %TRUE if successful, %FALSE if subscription or group not found
 */
gboolean nostr_subscription_registry_add_to_group(NostrSubscriptionRegistry *registry,
                                                   const gchar *sub_id,
                                                   const gchar *group_name);

/**
 * nostr_subscription_registry_remove_from_group:
 * @registry: A #NostrSubscriptionRegistry
 * @sub_id: The subscription ID to remove
 * @group_name: The group name
 *
 * Removes a subscription from a group without closing it.
 *
 * Returns: %TRUE if successful, %FALSE if not found in group
 */
gboolean nostr_subscription_registry_remove_from_group(NostrSubscriptionRegistry *registry,
                                                        const gchar *sub_id,
                                                        const gchar *group_name);

/* --- Iteration --- */

/**
 * NostrSubscriptionRegistryForeachFunc:
 * @sub_id: The subscription ID
 * @subscription: The #GNostrSubscription
 * @user_data: User data passed to the foreach function
 *
 * Callback for iterating over registered subscriptions.
 */
typedef void (*NostrSubscriptionRegistryForeachFunc)(const gchar *sub_id,
                                                      GNostrSubscription *subscription,
                                                      gpointer user_data);

/**
 * nostr_subscription_registry_foreach:
 * @registry: A #NostrSubscriptionRegistry
 * @func: The function to call for each subscription
 * @user_data: (nullable): User data passed to @func
 *
 * Iterates over all registered subscriptions.
 */
void nostr_subscription_registry_foreach(NostrSubscriptionRegistry *registry,
                                          NostrSubscriptionRegistryForeachFunc func,
                                          gpointer user_data);

/**
 * nostr_subscription_registry_foreach_active:
 * @registry: A #NostrSubscriptionRegistry
 * @func: The function to call for each active subscription
 * @user_data: (nullable): User data passed to @func
 *
 * Iterates over only active (non-closed) subscriptions.
 */
void nostr_subscription_registry_foreach_active(NostrSubscriptionRegistry *registry,
                                                 NostrSubscriptionRegistryForeachFunc func,
                                                 gpointer user_data);

/* --- Statistics --- */

/**
 * NostrSubscriptionRegistryStats:
 * @total_registered: Total subscriptions registered since creation
 * @current_active: Currently active subscriptions
 * @ephemeral_closed: Ephemeral subscriptions auto-closed after EOSE
 * @groups_count: Number of active groups
 * @avg_time_to_first_event_us: Average time to first event (microseconds, 0 if none measured)
 * @avg_eose_latency_us: Average EOSE latency (microseconds, 0 if none measured)
 * @stuck_pending_count: Subscriptions currently stuck in PENDING state
 * @auto_reconnects: Total auto-reconnect attempts for persistent subscriptions
 *
 * Statistics for monitoring subscription registry usage.
 */
typedef struct {
    guint64 total_registered;
    guint current_active;
    guint64 ephemeral_closed;
    guint groups_count;

    /* Health metrics */
    guint64 avg_time_to_first_event_us;
    guint64 avg_eose_latency_us;
    guint stuck_pending_count;
    guint64 auto_reconnects;
} NostrSubscriptionRegistryStats;

/**
 * nostr_subscription_registry_get_stats:
 * @registry: A #NostrSubscriptionRegistry
 * @stats: (out): Output structure for statistics
 *
 * Retrieves current statistics for the subscription registry.
 */
void nostr_subscription_registry_get_stats(NostrSubscriptionRegistry *registry,
                                            NostrSubscriptionRegistryStats *stats);

/* --- Health Monitoring --- */

/**
 * nostr_subscription_registry_notify_event:
 * @registry: A #NostrSubscriptionRegistry
 * @sub_id: The subscription ID that received an event
 *
 * Notifies the registry that a subscription has received an event.
 * Only the first call per subscription updates the time-to-first-event
 * metric; subsequent calls are no-ops.
 */
void nostr_subscription_registry_notify_event(NostrSubscriptionRegistry *registry,
                                               const gchar *sub_id);

/**
 * nostr_subscription_registry_start_health_monitor:
 * @registry: A #NostrSubscriptionRegistry
 * @check_interval_ms: How often to run health checks (milliseconds)
 * @stuck_timeout_ms: Time after which a PENDING subscription is "stuck" (milliseconds)
 *
 * Starts a periodic health monitor that:
 * - Detects subscriptions stuck in PENDING state
 * - Logs warnings for stuck subscriptions
 * - Auto-reconnects persistent subscriptions in ERROR state
 *
 * Only one monitor can be active at a time. Calling this while a monitor
 * is already running replaces it.
 */
void nostr_subscription_registry_start_health_monitor(NostrSubscriptionRegistry *registry,
                                                       guint check_interval_ms,
                                                       guint stuck_timeout_ms);

/**
 * nostr_subscription_registry_stop_health_monitor:
 * @registry: A #NostrSubscriptionRegistry
 *
 * Stops the periodic health monitor if one is running.
 */
void nostr_subscription_registry_stop_health_monitor(NostrSubscriptionRegistry *registry);

/* --- Cleanup --- */

/**
 * nostr_subscription_registry_close_all:
 * @registry: A #NostrSubscriptionRegistry
 *
 * Closes and unregisters all subscriptions. Used during shutdown.
 *
 * Returns: The number of subscriptions that were closed
 */
guint nostr_subscription_registry_close_all(NostrSubscriptionRegistry *registry);

G_END_DECLS

#endif /* NOSTR_SUBSCRIPTION_REGISTRY_H */
