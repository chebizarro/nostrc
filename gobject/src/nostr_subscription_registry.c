/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 gnostr contributors
 *
 * nostr_subscription_registry.c - Central subscription tracking and management
 */

#include "nostr_subscription_registry.h"
#include <string.h>

/* --- GNostrSubscription Type Stubs ---
 *
 * These are minimal definitions to allow the registry to compile independently.
 * The actual GNostrSubscription type is defined in nostr_subscription.h.
 * When used together, the full implementation will be linked.
 */

/* Define GNostrSubscription GObject type macros if not already defined */
#ifndef GNOSTR_TYPE_SUBSCRIPTION
#define GNOSTR_TYPE_SUBSCRIPTION (gnostr_subscription_get_type())
#endif

#ifndef GNOSTR_IS_SUBSCRIPTION
#define GNOSTR_IS_SUBSCRIPTION(obj) \
    (G_TYPE_CHECK_INSTANCE_TYPE((obj), GNOSTR_TYPE_SUBSCRIPTION))
#endif

#ifndef GNOSTR_SUBSCRIPTION
#define GNOSTR_SUBSCRIPTION(obj) \
    (G_TYPE_CHECK_INSTANCE_CAST((obj), GNOSTR_TYPE_SUBSCRIPTION, GNostrSubscription))
#endif

/* External function declarations - implemented in nostr_subscription.c */
GType gnostr_subscription_get_type(void);
NostrSubscriptionState gnostr_subscription_get_state(GNostrSubscription *self);
void gnostr_subscription_unsubscribe(GNostrSubscription *self);

/**
 * NostrSubscriptionType:
 * @NOSTR_SUBSCRIPTION_EPHEMERAL: Close subscription after EOSE
 * @NOSTR_SUBSCRIPTION_PERSISTENT: Keep open until explicit close
 */
typedef enum {
    NOSTR_SUBSCRIPTION_EPHEMERAL,
    NOSTR_SUBSCRIPTION_PERSISTENT
} NostrSubscriptionType;

/**
 * NostrSubscriptionConfig:
 * Configuration for a subscription.
 */
typedef struct {
    NostrSubscriptionType type;
    guint timeout_ms;
    gint retry_policy;
    guint max_events;
} NostrSubscriptionConfig;

/* External function to get subscription config */
const NostrSubscriptionConfig *gnostr_subscription_get_config(GNostrSubscription *self);

/* --- Private Data --- */

typedef struct {
    GHashTable *subscriptions;      /* sub_id (gchar*) -> GNostrSubscription* */
    GHashTable *groups;             /* group_name (gchar*) -> NostrSubscriptionGroup* */
    GHashTable *relay_counts;       /* relay_url (gchar*) -> GUINT_TO_POINTER(count) */
    GHashTable *sub_to_relay;       /* sub_id (gchar*) -> relay_url (gchar*) */

    guint max_per_relay;            /* Maximum subscriptions per relay (0 = unlimited) */
    guint64 next_sub_id;            /* Counter for generating unique IDs */

    /* State change callbacks */
    GArray *state_callbacks;        /* Array of StateCallbackEntry */
    guint next_callback_id;

    /* Statistics */
    guint64 total_registered;
    guint64 ephemeral_closed;

    GMutex mutex;                   /* Thread safety */
} NostrSubscriptionRegistryPrivate;

typedef struct {
    guint id;
    NostrSubscriptionStateCallback callback;
    gpointer user_data;
    GDestroyNotify destroy_notify;
} StateCallbackEntry;

G_DEFINE_TYPE_WITH_PRIVATE(NostrSubscriptionRegistry, nostr_subscription_registry, G_TYPE_OBJECT)

/* --- Singleton --- */

static NostrSubscriptionRegistry *default_registry = NULL;
G_LOCK_DEFINE_STATIC(default_registry_lock);

NostrSubscriptionRegistry *
nostr_subscription_registry_get_default(void)
{
    G_LOCK(default_registry_lock);
    if (default_registry == NULL) {
        default_registry = g_object_new(NOSTR_TYPE_SUBSCRIPTION_REGISTRY, NULL);
        /* prevent destruction */
        g_object_add_weak_pointer(G_OBJECT(default_registry),
                                  (gpointer *)&default_registry);
    }
    G_UNLOCK(default_registry_lock);
    return default_registry;
}

/* --- Group Management --- */

static NostrSubscriptionGroup *
subscription_group_new(const gchar *name)
{
    NostrSubscriptionGroup *group = g_new0(NostrSubscriptionGroup, 1);
    group->name = g_strdup(name);
    group->subscriptions = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                  g_free, NULL);
    return group;
}

static void
subscription_group_free(NostrSubscriptionGroup *group)
{
    if (group) {
        g_free(group->name);
        g_hash_table_unref(group->subscriptions);
        g_free(group);
    }
}

/* --- Internal Helpers --- */

static gchar *
generate_subscription_id(NostrSubscriptionRegistryPrivate *priv)
{
    guint64 id = priv->next_sub_id++;
    return g_strdup_printf("sub_%016" G_GUINT64_FORMAT "x", id);
}

static void
notify_state_change(NostrSubscriptionRegistry *registry,
                    const gchar *sub_id,
                    NostrSubscriptionState old_state,
                    NostrSubscriptionState new_state)
{
    NostrSubscriptionRegistryPrivate *priv =
        nostr_subscription_registry_get_instance_private(registry);

    for (guint i = 0; i < priv->state_callbacks->len; i++) {
        StateCallbackEntry *entry = &g_array_index(priv->state_callbacks,
                                                    StateCallbackEntry, i);
        if (entry->callback) {
            entry->callback(registry, sub_id, old_state, new_state, entry->user_data);
        }
    }
}

G_GNUC_UNUSED static void
increment_relay_count(NostrSubscriptionRegistryPrivate *priv, const gchar *relay_url)
{
    if (!relay_url)
        return;

    gpointer current = g_hash_table_lookup(priv->relay_counts, relay_url);
    guint count = GPOINTER_TO_UINT(current) + 1;
    g_hash_table_insert(priv->relay_counts,
                        g_strdup(relay_url),
                        GUINT_TO_POINTER(count));
}

static void
decrement_relay_count(NostrSubscriptionRegistryPrivate *priv, const gchar *relay_url)
{
    if (!relay_url)
        return;

    gpointer current = g_hash_table_lookup(priv->relay_counts, relay_url);
    guint count = GPOINTER_TO_UINT(current);
    if (count > 0) {
        count--;
        if (count == 0) {
            g_hash_table_remove(priv->relay_counts, relay_url);
        } else {
            g_hash_table_insert(priv->relay_counts,
                                g_strdup(relay_url),
                                GUINT_TO_POINTER(count));
        }
    }
}

/* --- GObject Implementation --- */

static void
nostr_subscription_registry_dispose(GObject *object)
{
    NostrSubscriptionRegistry *self = NOSTR_SUBSCRIPTION_REGISTRY(object);
    NostrSubscriptionRegistryPrivate *priv =
        nostr_subscription_registry_get_instance_private(self);

    g_mutex_lock(&priv->mutex);

    /* Clear all groups */
    if (priv->groups) {
        g_hash_table_remove_all(priv->groups);
    }

    /* Clear all subscriptions (releases references) */
    if (priv->subscriptions) {
        g_hash_table_remove_all(priv->subscriptions);
    }

    /* Clear state callbacks */
    if (priv->state_callbacks) {
        for (guint i = 0; i < priv->state_callbacks->len; i++) {
            StateCallbackEntry *entry = &g_array_index(priv->state_callbacks,
                                                        StateCallbackEntry, i);
            if (entry->destroy_notify && entry->user_data) {
                entry->destroy_notify(entry->user_data);
            }
        }
        g_array_set_size(priv->state_callbacks, 0);
    }

    g_mutex_unlock(&priv->mutex);

    G_OBJECT_CLASS(nostr_subscription_registry_parent_class)->dispose(object);
}

static void
nostr_subscription_registry_finalize(GObject *object)
{
    NostrSubscriptionRegistry *self = NOSTR_SUBSCRIPTION_REGISTRY(object);
    NostrSubscriptionRegistryPrivate *priv =
        nostr_subscription_registry_get_instance_private(self);

    g_hash_table_unref(priv->subscriptions);
    g_hash_table_unref(priv->groups);
    g_hash_table_unref(priv->relay_counts);
    g_hash_table_unref(priv->sub_to_relay);
    g_array_unref(priv->state_callbacks);
    g_mutex_clear(&priv->mutex);

    G_OBJECT_CLASS(nostr_subscription_registry_parent_class)->finalize(object);
}

static void
nostr_subscription_registry_class_init(NostrSubscriptionRegistryClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->dispose = nostr_subscription_registry_dispose;
    object_class->finalize = nostr_subscription_registry_finalize;
}

static void
nostr_subscription_registry_init(NostrSubscriptionRegistry *self)
{
    NostrSubscriptionRegistryPrivate *priv =
        nostr_subscription_registry_get_instance_private(self);

    g_mutex_init(&priv->mutex);

    priv->subscriptions = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                 g_free, g_object_unref);
    priv->groups = g_hash_table_new_full(g_str_hash, g_str_equal,
                                          g_free,
                                          (GDestroyNotify)subscription_group_free);
    priv->relay_counts = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                g_free, NULL);
    priv->sub_to_relay = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                g_free, g_free);
    priv->state_callbacks = g_array_new(FALSE, TRUE, sizeof(StateCallbackEntry));

    priv->max_per_relay = 0;  /* Unlimited by default */
    priv->next_sub_id = 1;
    priv->next_callback_id = 1;
    priv->total_registered = 0;
    priv->ephemeral_closed = 0;
}

/* --- Registration API --- */

gchar *
nostr_subscription_registry_register(NostrSubscriptionRegistry *registry,
                                      GNostrSubscription *subscription)
{
    return nostr_subscription_registry_register_with_group(registry, subscription, NULL);
}

gchar *
nostr_subscription_registry_register_with_group(NostrSubscriptionRegistry *registry,
                                                 GNostrSubscription *subscription,
                                                 const gchar *group_name)
{
    g_return_val_if_fail(NOSTR_IS_SUBSCRIPTION_REGISTRY(registry), NULL);
    g_return_val_if_fail(GNOSTR_IS_SUBSCRIPTION(subscription), NULL);

    NostrSubscriptionRegistryPrivate *priv =
        nostr_subscription_registry_get_instance_private(registry);

    g_mutex_lock(&priv->mutex);

    gchar *sub_id = generate_subscription_id(priv);

    /* Add to main registry */
    g_hash_table_insert(priv->subscriptions,
                        g_strdup(sub_id),
                        g_object_ref(subscription));

    priv->total_registered++;

    /* Add to group if specified */
    if (group_name) {
        NostrSubscriptionGroup *group = g_hash_table_lookup(priv->groups, group_name);
        if (!group) {
            group = subscription_group_new(group_name);
            g_hash_table_insert(priv->groups, g_strdup(group_name), group);
        }
        g_hash_table_insert(group->subscriptions,
                            g_strdup(sub_id),
                            subscription);
    }

    g_mutex_unlock(&priv->mutex);

    /* Notify state change (PENDING is the initial state) */
    notify_state_change(registry, sub_id,
                        NOSTR_SUBSCRIPTION_STATE_PENDING,
                        gnostr_subscription_get_state(subscription));

    return sub_id;
}

gboolean
nostr_subscription_registry_unregister(NostrSubscriptionRegistry *registry,
                                        const gchar *sub_id)
{
    g_return_val_if_fail(NOSTR_IS_SUBSCRIPTION_REGISTRY(registry), FALSE);
    g_return_val_if_fail(sub_id != NULL, FALSE);

    NostrSubscriptionRegistryPrivate *priv =
        nostr_subscription_registry_get_instance_private(registry);

    g_mutex_lock(&priv->mutex);

    GNostrSubscription *sub = g_hash_table_lookup(priv->subscriptions, sub_id);
    if (!sub) {
        g_mutex_unlock(&priv->mutex);
        return FALSE;
    }

    NostrSubscriptionState old_state = gnostr_subscription_get_state(sub);

    /* Remove from relay counts */
    gchar *relay_url = g_hash_table_lookup(priv->sub_to_relay, sub_id);
    if (relay_url) {
        decrement_relay_count(priv, relay_url);
        g_hash_table_remove(priv->sub_to_relay, sub_id);
    }

    /* Remove from all groups */
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, priv->groups);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        NostrSubscriptionGroup *group = value;
        g_hash_table_remove(group->subscriptions, sub_id);
    }

    /* Remove from main registry (releases reference) */
    g_hash_table_remove(priv->subscriptions, sub_id);

    g_mutex_unlock(&priv->mutex);

    /* Notify state change */
    notify_state_change(registry, sub_id, old_state, NOSTR_SUBSCRIPTION_STATE_CLOSED);

    return TRUE;
}

/* --- Lookup API --- */

GNostrSubscription *
nostr_subscription_registry_get_by_id(NostrSubscriptionRegistry *registry,
                                       const gchar *sub_id)
{
    g_return_val_if_fail(NOSTR_IS_SUBSCRIPTION_REGISTRY(registry), NULL);
    g_return_val_if_fail(sub_id != NULL, NULL);

    NostrSubscriptionRegistryPrivate *priv =
        nostr_subscription_registry_get_instance_private(registry);

    g_mutex_lock(&priv->mutex);
    GNostrSubscription *sub = g_hash_table_lookup(priv->subscriptions, sub_id);
    g_mutex_unlock(&priv->mutex);

    return sub;
}

guint
nostr_subscription_registry_get_active_count(NostrSubscriptionRegistry *registry)
{
    g_return_val_if_fail(NOSTR_IS_SUBSCRIPTION_REGISTRY(registry), 0);

    NostrSubscriptionRegistryPrivate *priv =
        nostr_subscription_registry_get_instance_private(registry);

    g_mutex_lock(&priv->mutex);

    guint count = 0;
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, priv->subscriptions);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        GNostrSubscription *sub = GNOSTR_SUBSCRIPTION(value);
        NostrSubscriptionState state = gnostr_subscription_get_state(sub);
        if (state != NOSTR_SUBSCRIPTION_STATE_CLOSED &&
            state != NOSTR_SUBSCRIPTION_STATE_ERROR) {
            count++;
        }
    }

    g_mutex_unlock(&priv->mutex);
    return count;
}

guint
nostr_subscription_registry_get_total_count(NostrSubscriptionRegistry *registry)
{
    g_return_val_if_fail(NOSTR_IS_SUBSCRIPTION_REGISTRY(registry), 0);

    NostrSubscriptionRegistryPrivate *priv =
        nostr_subscription_registry_get_instance_private(registry);

    g_mutex_lock(&priv->mutex);
    guint count = g_hash_table_size(priv->subscriptions);
    g_mutex_unlock(&priv->mutex);

    return count;
}

/* --- EOSE Handling --- */

void
nostr_subscription_registry_notify_eose(NostrSubscriptionRegistry *registry,
                                         const gchar *sub_id)
{
    g_return_if_fail(NOSTR_IS_SUBSCRIPTION_REGISTRY(registry));
    g_return_if_fail(sub_id != NULL);

    NostrSubscriptionRegistryPrivate *priv =
        nostr_subscription_registry_get_instance_private(registry);

    g_mutex_lock(&priv->mutex);

    GNostrSubscription *sub = g_hash_table_lookup(priv->subscriptions, sub_id);
    if (!sub) {
        g_mutex_unlock(&priv->mutex);
        return;
    }

    NostrSubscriptionState old_state = gnostr_subscription_get_state(sub);
    const NostrSubscriptionConfig *config = gnostr_subscription_get_config(sub);

    /* Check if ephemeral - should be auto-closed after EOSE */
    gboolean is_ephemeral = config && config->type == NOSTR_SUBSCRIPTION_EPHEMERAL;

    g_mutex_unlock(&priv->mutex);

    /* Notify EOSE state change */
    notify_state_change(registry, sub_id, old_state, NOSTR_SUBSCRIPTION_STATE_EOSE_RECEIVED);

    /* Auto-cleanup ephemeral subscriptions */
    if (is_ephemeral) {
        g_mutex_lock(&priv->mutex);
        priv->ephemeral_closed++;
        g_mutex_unlock(&priv->mutex);

        /* Unsubscribe and unregister */
        GNostrSubscription *sub_to_close = nostr_subscription_registry_get_by_id(registry, sub_id);
        if (sub_to_close) {
            gnostr_subscription_unsubscribe(sub_to_close);
        }
        nostr_subscription_registry_unregister(registry, sub_id);
    }
}

/* --- Relay Limits --- */

void
nostr_subscription_registry_set_max_per_relay(NostrSubscriptionRegistry *registry,
                                               guint max_subscriptions)
{
    g_return_if_fail(NOSTR_IS_SUBSCRIPTION_REGISTRY(registry));

    NostrSubscriptionRegistryPrivate *priv =
        nostr_subscription_registry_get_instance_private(registry);

    g_mutex_lock(&priv->mutex);
    priv->max_per_relay = max_subscriptions;
    g_mutex_unlock(&priv->mutex);
}

guint
nostr_subscription_registry_get_max_per_relay(NostrSubscriptionRegistry *registry)
{
    g_return_val_if_fail(NOSTR_IS_SUBSCRIPTION_REGISTRY(registry), 0);

    NostrSubscriptionRegistryPrivate *priv =
        nostr_subscription_registry_get_instance_private(registry);

    g_mutex_lock(&priv->mutex);
    guint max = priv->max_per_relay;
    g_mutex_unlock(&priv->mutex);

    return max;
}

guint
nostr_subscription_registry_get_relay_subscription_count(NostrSubscriptionRegistry *registry,
                                                          const gchar *relay_url)
{
    g_return_val_if_fail(NOSTR_IS_SUBSCRIPTION_REGISTRY(registry), 0);
    g_return_val_if_fail(relay_url != NULL, 0);

    NostrSubscriptionRegistryPrivate *priv =
        nostr_subscription_registry_get_instance_private(registry);

    g_mutex_lock(&priv->mutex);
    gpointer count_ptr = g_hash_table_lookup(priv->relay_counts, relay_url);
    guint count = GPOINTER_TO_UINT(count_ptr);
    g_mutex_unlock(&priv->mutex);

    return count;
}

/* --- State Change Notifications --- */

guint
nostr_subscription_registry_add_state_callback(NostrSubscriptionRegistry *registry,
                                                NostrSubscriptionStateCallback callback,
                                                gpointer user_data,
                                                GDestroyNotify destroy_notify)
{
    g_return_val_if_fail(NOSTR_IS_SUBSCRIPTION_REGISTRY(registry), 0);
    g_return_val_if_fail(callback != NULL, 0);

    NostrSubscriptionRegistryPrivate *priv =
        nostr_subscription_registry_get_instance_private(registry);

    g_mutex_lock(&priv->mutex);

    StateCallbackEntry entry = {
        .id = priv->next_callback_id++,
        .callback = callback,
        .user_data = user_data,
        .destroy_notify = destroy_notify
    };

    g_array_append_val(priv->state_callbacks, entry);

    guint id = entry.id;
    g_mutex_unlock(&priv->mutex);

    return id;
}

void
nostr_subscription_registry_remove_state_callback(NostrSubscriptionRegistry *registry,
                                                   guint callback_id)
{
    g_return_if_fail(NOSTR_IS_SUBSCRIPTION_REGISTRY(registry));

    NostrSubscriptionRegistryPrivate *priv =
        nostr_subscription_registry_get_instance_private(registry);

    g_mutex_lock(&priv->mutex);

    for (guint i = 0; i < priv->state_callbacks->len; i++) {
        StateCallbackEntry *entry = &g_array_index(priv->state_callbacks,
                                                    StateCallbackEntry, i);
        if (entry->id == callback_id) {
            if (entry->destroy_notify && entry->user_data) {
                entry->destroy_notify(entry->user_data);
            }
            g_array_remove_index(priv->state_callbacks, i);
            break;
        }
    }

    g_mutex_unlock(&priv->mutex);
}

/* --- Group Operations --- */

NostrSubscriptionGroup *
nostr_subscription_registry_create_group(NostrSubscriptionRegistry *registry,
                                          const gchar *group_name)
{
    g_return_val_if_fail(NOSTR_IS_SUBSCRIPTION_REGISTRY(registry), NULL);
    g_return_val_if_fail(group_name != NULL, NULL);

    NostrSubscriptionRegistryPrivate *priv =
        nostr_subscription_registry_get_instance_private(registry);

    g_mutex_lock(&priv->mutex);

    if (g_hash_table_contains(priv->groups, group_name)) {
        g_mutex_unlock(&priv->mutex);
        return NULL;
    }

    NostrSubscriptionGroup *group = subscription_group_new(group_name);
    g_hash_table_insert(priv->groups, g_strdup(group_name), group);

    g_mutex_unlock(&priv->mutex);
    return group;
}

NostrSubscriptionGroup *
nostr_subscription_registry_get_group(NostrSubscriptionRegistry *registry,
                                       const gchar *group_name)
{
    g_return_val_if_fail(NOSTR_IS_SUBSCRIPTION_REGISTRY(registry), NULL);
    g_return_val_if_fail(group_name != NULL, NULL);

    NostrSubscriptionRegistryPrivate *priv =
        nostr_subscription_registry_get_instance_private(registry);

    g_mutex_lock(&priv->mutex);
    NostrSubscriptionGroup *group = g_hash_table_lookup(priv->groups, group_name);
    g_mutex_unlock(&priv->mutex);

    return group;
}

guint
nostr_subscription_registry_close_group(NostrSubscriptionRegistry *registry,
                                         const gchar *group_name)
{
    g_return_val_if_fail(NOSTR_IS_SUBSCRIPTION_REGISTRY(registry), 0);
    g_return_val_if_fail(group_name != NULL, 0);

    NostrSubscriptionRegistryPrivate *priv =
        nostr_subscription_registry_get_instance_private(registry);

    g_mutex_lock(&priv->mutex);

    NostrSubscriptionGroup *group = g_hash_table_lookup(priv->groups, group_name);
    if (!group) {
        g_mutex_unlock(&priv->mutex);
        return 0;
    }

    /* Collect subscription IDs to close */
    GPtrArray *sub_ids = g_ptr_array_new_with_free_func(g_free);
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, group->subscriptions);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        g_ptr_array_add(sub_ids, g_strdup(key));
    }

    guint count = sub_ids->len;

    g_mutex_unlock(&priv->mutex);

    /* Close each subscription outside of lock */
    for (guint i = 0; i < sub_ids->len; i++) {
        const gchar *sub_id = g_ptr_array_index(sub_ids, i);
        GNostrSubscription *sub = nostr_subscription_registry_get_by_id(registry, sub_id);
        if (sub) {
            gnostr_subscription_unsubscribe(sub);
        }
        nostr_subscription_registry_unregister(registry, sub_id);
    }

    g_ptr_array_unref(sub_ids);

    /* Remove the group */
    g_mutex_lock(&priv->mutex);
    g_hash_table_remove(priv->groups, group_name);
    g_mutex_unlock(&priv->mutex);

    return count;
}

gboolean
nostr_subscription_registry_add_to_group(NostrSubscriptionRegistry *registry,
                                          const gchar *sub_id,
                                          const gchar *group_name)
{
    g_return_val_if_fail(NOSTR_IS_SUBSCRIPTION_REGISTRY(registry), FALSE);
    g_return_val_if_fail(sub_id != NULL, FALSE);
    g_return_val_if_fail(group_name != NULL, FALSE);

    NostrSubscriptionRegistryPrivate *priv =
        nostr_subscription_registry_get_instance_private(registry);

    g_mutex_lock(&priv->mutex);

    GNostrSubscription *sub = g_hash_table_lookup(priv->subscriptions, sub_id);
    if (!sub) {
        g_mutex_unlock(&priv->mutex);
        return FALSE;
    }

    NostrSubscriptionGroup *group = g_hash_table_lookup(priv->groups, group_name);
    if (!group) {
        group = subscription_group_new(group_name);
        g_hash_table_insert(priv->groups, g_strdup(group_name), group);
    }

    g_hash_table_insert(group->subscriptions, g_strdup(sub_id), sub);

    g_mutex_unlock(&priv->mutex);
    return TRUE;
}

gboolean
nostr_subscription_registry_remove_from_group(NostrSubscriptionRegistry *registry,
                                               const gchar *sub_id,
                                               const gchar *group_name)
{
    g_return_val_if_fail(NOSTR_IS_SUBSCRIPTION_REGISTRY(registry), FALSE);
    g_return_val_if_fail(sub_id != NULL, FALSE);
    g_return_val_if_fail(group_name != NULL, FALSE);

    NostrSubscriptionRegistryPrivate *priv =
        nostr_subscription_registry_get_instance_private(registry);

    g_mutex_lock(&priv->mutex);

    NostrSubscriptionGroup *group = g_hash_table_lookup(priv->groups, group_name);
    if (!group) {
        g_mutex_unlock(&priv->mutex);
        return FALSE;
    }

    gboolean removed = g_hash_table_remove(group->subscriptions, sub_id);

    g_mutex_unlock(&priv->mutex);
    return removed;
}

/* --- Iteration --- */

void
nostr_subscription_registry_foreach(NostrSubscriptionRegistry *registry,
                                     NostrSubscriptionRegistryForeachFunc func,
                                     gpointer user_data)
{
    g_return_if_fail(NOSTR_IS_SUBSCRIPTION_REGISTRY(registry));
    g_return_if_fail(func != NULL);

    NostrSubscriptionRegistryPrivate *priv =
        nostr_subscription_registry_get_instance_private(registry);

    g_mutex_lock(&priv->mutex);

    /* Create a copy of entries to iterate safely */
    GPtrArray *entries = g_ptr_array_new();
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, priv->subscriptions);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        g_ptr_array_add(entries, g_strdup(key));
        g_ptr_array_add(entries, g_object_ref(value));
    }

    g_mutex_unlock(&priv->mutex);

    /* Iterate over copied entries */
    for (guint i = 0; i < entries->len; i += 2) {
        const gchar *sub_id = g_ptr_array_index(entries, i);
        GNostrSubscription *sub = g_ptr_array_index(entries, i + 1);
        func(sub_id, sub, user_data);
        g_free((gchar *)sub_id);
        g_object_unref(sub);
    }

    g_ptr_array_unref(entries);
}

void
nostr_subscription_registry_foreach_active(NostrSubscriptionRegistry *registry,
                                            NostrSubscriptionRegistryForeachFunc func,
                                            gpointer user_data)
{
    g_return_if_fail(NOSTR_IS_SUBSCRIPTION_REGISTRY(registry));
    g_return_if_fail(func != NULL);

    NostrSubscriptionRegistryPrivate *priv =
        nostr_subscription_registry_get_instance_private(registry);

    g_mutex_lock(&priv->mutex);

    /* Create a copy of active entries */
    GPtrArray *entries = g_ptr_array_new();
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, priv->subscriptions);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        GNostrSubscription *sub = GNOSTR_SUBSCRIPTION(value);
        NostrSubscriptionState state = gnostr_subscription_get_state(sub);
        if (state != NOSTR_SUBSCRIPTION_STATE_CLOSED &&
            state != NOSTR_SUBSCRIPTION_STATE_ERROR) {
            g_ptr_array_add(entries, g_strdup(key));
            g_ptr_array_add(entries, g_object_ref(value));
        }
    }

    g_mutex_unlock(&priv->mutex);

    /* Iterate over copied entries */
    for (guint i = 0; i < entries->len; i += 2) {
        const gchar *sub_id = g_ptr_array_index(entries, i);
        GNostrSubscription *sub = g_ptr_array_index(entries, i + 1);
        func(sub_id, sub, user_data);
        g_free((gchar *)sub_id);
        g_object_unref(sub);
    }

    g_ptr_array_unref(entries);
}

/* --- Statistics --- */

void
nostr_subscription_registry_get_stats(NostrSubscriptionRegistry *registry,
                                       NostrSubscriptionRegistryStats *stats)
{
    g_return_if_fail(NOSTR_IS_SUBSCRIPTION_REGISTRY(registry));
    g_return_if_fail(stats != NULL);

    NostrSubscriptionRegistryPrivate *priv =
        nostr_subscription_registry_get_instance_private(registry);

    g_mutex_lock(&priv->mutex);

    stats->total_registered = priv->total_registered;
    stats->current_active = nostr_subscription_registry_get_active_count(registry);
    stats->ephemeral_closed = priv->ephemeral_closed;
    stats->groups_count = g_hash_table_size(priv->groups);

    g_mutex_unlock(&priv->mutex);
}

/* --- Cleanup --- */

guint
nostr_subscription_registry_close_all(NostrSubscriptionRegistry *registry)
{
    g_return_val_if_fail(NOSTR_IS_SUBSCRIPTION_REGISTRY(registry), 0);

    NostrSubscriptionRegistryPrivate *priv =
        nostr_subscription_registry_get_instance_private(registry);

    g_mutex_lock(&priv->mutex);

    /* Collect all subscription IDs */
    GPtrArray *sub_ids = g_ptr_array_new_with_free_func(g_free);
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, priv->subscriptions);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        g_ptr_array_add(sub_ids, g_strdup(key));
    }

    guint count = sub_ids->len;

    g_mutex_unlock(&priv->mutex);

    /* Close each subscription outside of lock */
    for (guint i = 0; i < sub_ids->len; i++) {
        const gchar *sub_id = g_ptr_array_index(sub_ids, i);
        GNostrSubscription *sub = nostr_subscription_registry_get_by_id(registry, sub_id);
        if (sub) {
            gnostr_subscription_unsubscribe(sub);
        }
        nostr_subscription_registry_unregister(registry, sub_id);
    }

    g_ptr_array_unref(sub_ids);

    /* Clear groups */
    g_mutex_lock(&priv->mutex);
    g_hash_table_remove_all(priv->groups);
    g_mutex_unlock(&priv->mutex);

    return count;
}
