/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 gnostr contributors
 *
 * nostr_event_bus.c - Central event routing for reactive architecture
 *
 * Implementation of the NostrEventBus, providing a thread-safe
 * publish-subscribe mechanism for routing Nostr events throughout
 * the application.
 */

#include "nostr_event_bus.h"
#include <string.h>

/* --- Private Structures --- */

/**
 * Subscription:
 *
 * Internal structure representing a single subscription.
 * Uses reference counting for safe lifetime management.
 */
typedef struct _Subscription {
    guint64 id;                          /* Unique subscription ID */
    gchar *pattern;                      /* Topic pattern (owned) */
    NostrEventBusCallback callback;      /* User callback */
    NostrEventBusFilterFunc filter_func; /* Optional filter predicate */
    gpointer user_data;                  /* User data for callback */
    GDestroyNotify destroy_notify;       /* Cleanup function for user_data */
    volatile gint ref_count;             /* Reference count */
    volatile gint cancelled;             /* Set to 1 when unsubscribed */
} Subscription;

/**
 * NostrEventBusPrivate:
 *
 * Private data for the NostrEventBus instance.
 */
typedef struct _NostrEventBusPrivate {
    GMutex mutex;                    /* Protects all mutable state */
    GHashTable *subscriptions;       /* id -> Subscription* */
    GHashTable *pattern_cache;       /* "pattern::topic" -> gboolean */
    guint64 next_subscription_id;    /* Counter for subscription IDs */

    /* Statistics */
    guint subscription_count;
    guint64 events_emitted;
    guint64 callbacks_invoked;
    guint64 pattern_cache_hits;
    guint64 pattern_cache_misses;
} NostrEventBusPrivate;

G_DEFINE_TYPE_WITH_PRIVATE(NostrEventBus, nostr_event_bus, G_TYPE_OBJECT)

/* --- Subscription Lifecycle --- */

static Subscription *subscription_new(guint64 id,
                                       const gchar *pattern,
                                       NostrEventBusCallback callback,
                                       NostrEventBusFilterFunc filter_func,
                                       gpointer user_data,
                                       GDestroyNotify destroy_notify) {
    Subscription *sub = g_new0(Subscription, 1);
    sub->id = id;
    sub->pattern = g_strdup(pattern);
    sub->callback = callback;
    sub->filter_func = filter_func;
    sub->user_data = user_data;
    sub->destroy_notify = destroy_notify;
    sub->ref_count = 1;
    sub->cancelled = 0;
    return sub;
}

static Subscription *subscription_ref(Subscription *sub) {
    if (sub) {
        g_atomic_int_inc(&sub->ref_count);
    }
    return sub;
}

static void subscription_unref(Subscription *sub) {
    if (!sub) return;

    if (g_atomic_int_dec_and_test(&sub->ref_count)) {
        if (sub->destroy_notify && sub->user_data) {
            sub->destroy_notify(sub->user_data);
        }
        g_free(sub->pattern);
        g_free(sub);
    }
}

/* --- Topic Pattern Matching --- */

/**
 * split_topic:
 * @topic: Topic string with "::" separators
 *
 * Splits a topic into segments.
 *
 * Returns: (transfer full): A NULL-terminated array of segments
 */
static gchar **split_topic(const gchar *topic) {
    if (!topic || *topic == '\0') {
        gchar **empty = g_new0(gchar *, 1);
        return empty;
    }
    return g_strsplit(topic, "::", -1);
}

/**
 * topic_matches_recursive:
 * @pattern_segs: Pattern segments array
 * @pattern_idx: Current index in pattern
 * @pattern_len: Length of pattern segments
 * @topic_segs: Topic segments array
 * @topic_idx: Current index in topic
 * @topic_len: Length of topic segments
 *
 * Recursive helper for pattern matching.
 *
 * Returns: %TRUE if match succeeds
 */
static gboolean topic_matches_recursive(gchar **pattern_segs,
                                         gsize pattern_idx,
                                         gsize pattern_len,
                                         gchar **topic_segs,
                                         gsize topic_idx,
                                         gsize topic_len) {
    /* Base cases */
    if (pattern_idx == pattern_len && topic_idx == topic_len) {
        return TRUE;  /* Both exhausted - match */
    }

    if (pattern_idx == pattern_len) {
        return FALSE;  /* Pattern exhausted but topic has more - no match */
    }

    const gchar *pat = pattern_segs[pattern_idx];

    /* Handle "**" - matches zero or more segments */
    if (g_strcmp0(pat, "**") == 0) {
        /* Try matching zero segments (skip **) */
        if (topic_matches_recursive(pattern_segs, pattern_idx + 1, pattern_len,
                                     topic_segs, topic_idx, topic_len)) {
            return TRUE;
        }

        /* Try matching one or more segments */
        for (gsize i = topic_idx; i < topic_len; i++) {
            if (topic_matches_recursive(pattern_segs, pattern_idx + 1, pattern_len,
                                         topic_segs, i + 1, topic_len)) {
                return TRUE;
            }
        }

        /* ** at end matches rest of topic */
        if (pattern_idx + 1 == pattern_len) {
            return TRUE;
        }

        return FALSE;
    }

    /* Need a topic segment to match */
    if (topic_idx == topic_len) {
        return FALSE;
    }

    const gchar *top = topic_segs[topic_idx];

    /* Handle "*" - matches exactly one segment */
    if (g_strcmp0(pat, "*") == 0) {
        return topic_matches_recursive(pattern_segs, pattern_idx + 1, pattern_len,
                                        topic_segs, topic_idx + 1, topic_len);
    }

    /* Literal match */
    if (g_strcmp0(pat, top) == 0) {
        return topic_matches_recursive(pattern_segs, pattern_idx + 1, pattern_len,
                                        topic_segs, topic_idx + 1, topic_len);
    }

    return FALSE;
}

/**
 * nostr_event_bus_topic_matches:
 *
 * Public function to test if a topic matches a pattern.
 */
gboolean nostr_event_bus_topic_matches(const gchar *pattern,
                                        const gchar *topic) {
    if (!pattern || !topic) {
        return FALSE;
    }

    /* Fast path: exact match */
    if (g_strcmp0(pattern, topic) == 0) {
        return TRUE;
    }

    /* Fast path: no wildcards means must be exact */
    if (!strchr(pattern, '*')) {
        return FALSE;
    }

    gchar **pattern_segs = split_topic(pattern);
    gchar **topic_segs = split_topic(topic);

    gsize pattern_len = g_strv_length(pattern_segs);
    gsize topic_len = g_strv_length(topic_segs);

    gboolean result = topic_matches_recursive(pattern_segs, 0, pattern_len,
                                               topic_segs, 0, topic_len);

    g_strfreev(pattern_segs);
    g_strfreev(topic_segs);

    return result;
}

/**
 * check_pattern_cached:
 * @priv: Private data
 * @pattern: Pattern to check
 * @topic: Topic to match
 *
 * Checks pattern match with caching.
 *
 * Returns: %TRUE if pattern matches topic
 */
static gboolean check_pattern_cached(NostrEventBusPrivate *priv,
                                      const gchar *pattern,
                                      const gchar *topic) {
    /* Build cache key */
    gchar *cache_key = g_strdup_printf("%s\x1f%s", pattern, topic);

    /* Check cache (must be called with mutex held) */
    gpointer cached = g_hash_table_lookup(priv->pattern_cache, cache_key);
    if (cached != NULL) {
        priv->pattern_cache_hits++;
        gboolean result = GPOINTER_TO_INT(cached) != 0;
        g_free(cache_key);
        return result;
    }

    priv->pattern_cache_misses++;

    /* Compute match */
    gboolean result = nostr_event_bus_topic_matches(pattern, topic);

    /* Cache result (limit cache size to prevent unbounded growth) */
    if (g_hash_table_size(priv->pattern_cache) < 10000) {
        g_hash_table_insert(priv->pattern_cache,
                            cache_key,
                            GINT_TO_POINTER(result ? 1 : -1));
    } else {
        g_free(cache_key);
    }

    return result;
}

/* --- Virtual Method Implementations --- */

static NostrEventBusHandle *nostr_event_bus_real_subscribe(NostrEventBus *bus,
                                                            const gchar *topic_pattern,
                                                            NostrEventBusCallback callback,
                                                            gpointer user_data,
                                                            GDestroyNotify destroy_notify) {
    g_return_val_if_fail(NOSTR_IS_EVENT_BUS(bus), NULL);
    g_return_val_if_fail(topic_pattern != NULL, NULL);
    g_return_val_if_fail(callback != NULL, NULL);

    NostrEventBusPrivate *priv = nostr_event_bus_get_instance_private(bus);

    g_mutex_lock(&priv->mutex);

    guint64 id = ++priv->next_subscription_id;

    Subscription *sub = subscription_new(id, topic_pattern, callback,
                                          NULL, user_data, destroy_notify);

    g_hash_table_insert(priv->subscriptions, GUINT_TO_POINTER(id), sub);
    priv->subscription_count++;

    /* Allocate handle - it stores the subscription ID */
    NostrEventBusHandle *handle = g_new0(NostrEventBusHandle, 1);
    handle->id = id;

    g_mutex_unlock(&priv->mutex);

    return handle;
}

static void nostr_event_bus_real_unsubscribe(NostrEventBus *bus,
                                              NostrEventBusHandle *handle) {
    if (!handle) return;
    g_return_if_fail(NOSTR_IS_EVENT_BUS(bus));

    NostrEventBusPrivate *priv = nostr_event_bus_get_instance_private(bus);

    g_mutex_lock(&priv->mutex);

    Subscription *sub = g_hash_table_lookup(priv->subscriptions,
                                             GUINT_TO_POINTER(handle->id));
    if (sub) {
        /* Mark as cancelled to prevent further callbacks */
        g_atomic_int_set(&sub->cancelled, 1);

        /* Remove from table (will unref subscription) */
        g_hash_table_remove(priv->subscriptions, GUINT_TO_POINTER(handle->id));
        priv->subscription_count--;
    }

    g_mutex_unlock(&priv->mutex);

    g_free(handle);
}

static void nostr_event_bus_real_emit(NostrEventBus *bus,
                                       const gchar *topic,
                                       const gchar *event_json) {
    g_return_if_fail(NOSTR_IS_EVENT_BUS(bus));
    g_return_if_fail(topic != NULL);

    NostrEventBusPrivate *priv = nostr_event_bus_get_instance_private(bus);

    /* Build list of matching subscriptions with refs */
    GPtrArray *matching = g_ptr_array_new_with_free_func(
        (GDestroyNotify)subscription_unref);

    g_mutex_lock(&priv->mutex);

    priv->events_emitted++;

    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, priv->subscriptions);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        Subscription *sub = value;

        /* Skip cancelled subscriptions */
        if (g_atomic_int_get(&sub->cancelled)) {
            continue;
        }

        /* Check pattern match */
        if (check_pattern_cached(priv, sub->pattern, topic)) {
            g_ptr_array_add(matching, subscription_ref(sub));
        }
    }

    g_mutex_unlock(&priv->mutex);

    /* Invoke callbacks outside of lock */
    for (guint i = 0; i < matching->len; i++) {
        Subscription *sub = g_ptr_array_index(matching, i);

        /* Double-check cancelled flag */
        if (g_atomic_int_get(&sub->cancelled)) {
            continue;
        }

        /* Apply filter if present */
        if (sub->filter_func) {
            if (!sub->filter_func(topic, event_json, sub->user_data)) {
                continue;
            }
        }

        /* Invoke callback */
        sub->callback(topic, event_json, sub->user_data);

        g_mutex_lock(&priv->mutex);
        priv->callbacks_invoked++;
        g_mutex_unlock(&priv->mutex);
    }

    g_ptr_array_unref(matching);
}

static void nostr_event_bus_real_emit_batch(NostrEventBus *bus,
                                             const gchar *topic,
                                             const gchar *const *events_array,
                                             gsize count) {
    g_return_if_fail(NOSTR_IS_EVENT_BUS(bus));
    g_return_if_fail(topic != NULL);

    if (!events_array || count == 0) {
        return;
    }

    NostrEventBusPrivate *priv = nostr_event_bus_get_instance_private(bus);

    /* Build list of matching subscriptions with refs */
    GPtrArray *matching = g_ptr_array_new_with_free_func(
        (GDestroyNotify)subscription_unref);

    g_mutex_lock(&priv->mutex);

    priv->events_emitted += count;

    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, priv->subscriptions);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        Subscription *sub = value;

        if (g_atomic_int_get(&sub->cancelled)) {
            continue;
        }

        if (check_pattern_cached(priv, sub->pattern, topic)) {
            g_ptr_array_add(matching, subscription_ref(sub));
        }
    }

    g_mutex_unlock(&priv->mutex);

    /* Invoke callbacks for each event */
    for (gsize e = 0; e < count; e++) {
        const gchar *event_json = events_array[e];

        for (guint i = 0; i < matching->len; i++) {
            Subscription *sub = g_ptr_array_index(matching, i);

            if (g_atomic_int_get(&sub->cancelled)) {
                continue;
            }

            if (sub->filter_func) {
                if (!sub->filter_func(topic, event_json, sub->user_data)) {
                    continue;
                }
            }

            sub->callback(topic, event_json, sub->user_data);

            g_mutex_lock(&priv->mutex);
            priv->callbacks_invoked++;
            g_mutex_unlock(&priv->mutex);
        }
    }

    g_ptr_array_unref(matching);
}

/* --- GObject Lifecycle --- */

static void nostr_event_bus_finalize(GObject *object) {
    NostrEventBus *bus = NOSTR_EVENT_BUS(object);
    NostrEventBusPrivate *priv = nostr_event_bus_get_instance_private(bus);

    g_hash_table_destroy(priv->subscriptions);
    g_hash_table_destroy(priv->pattern_cache);
    g_mutex_clear(&priv->mutex);

    G_OBJECT_CLASS(nostr_event_bus_parent_class)->finalize(object);
}

static void nostr_event_bus_class_init(NostrEventBusClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->finalize = nostr_event_bus_finalize;

    /* Set up virtual methods */
    klass->subscribe = nostr_event_bus_real_subscribe;
    klass->unsubscribe = nostr_event_bus_real_unsubscribe;
    klass->emit = nostr_event_bus_real_emit;
    klass->emit_batch = nostr_event_bus_real_emit_batch;
}

static void nostr_event_bus_init(NostrEventBus *bus) {
    NostrEventBusPrivate *priv = nostr_event_bus_get_instance_private(bus);

    g_mutex_init(&priv->mutex);

    priv->subscriptions = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                                 NULL,
                                                 (GDestroyNotify)subscription_unref);
    priv->pattern_cache = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                 g_free, NULL);
    priv->next_subscription_id = 0;
    priv->subscription_count = 0;
    priv->events_emitted = 0;
    priv->callbacks_invoked = 0;
    priv->pattern_cache_hits = 0;
    priv->pattern_cache_misses = 0;
}

/* --- Singleton --- */

static NostrEventBus *default_bus = NULL;
static GMutex default_bus_mutex;

/**
 * nostr_event_bus_get_default:
 *
 * Gets the default singleton event bus instance.
 *
 * Returns: (transfer none): The default event bus
 */
NostrEventBus *nostr_event_bus_get_default(void) {
    g_mutex_lock(&default_bus_mutex);

    if (default_bus == NULL) {
        default_bus = g_object_new(NOSTR_TYPE_EVENT_BUS, NULL);
        /* Keep a reference so it lives for the application lifetime */
    }

    g_mutex_unlock(&default_bus_mutex);

    return default_bus;
}

/* --- Public API Implementation --- */

NostrEventBusHandle *nostr_event_bus_subscribe(NostrEventBus *bus,
                                                const gchar *topic_pattern,
                                                NostrEventBusCallback callback,
                                                gpointer user_data) {
    return nostr_event_bus_subscribe_full(bus, topic_pattern, callback,
                                           user_data, NULL);
}

NostrEventBusHandle *nostr_event_bus_subscribe_full(NostrEventBus *bus,
                                                     const gchar *topic_pattern,
                                                     NostrEventBusCallback callback,
                                                     gpointer user_data,
                                                     GDestroyNotify destroy_notify) {
    g_return_val_if_fail(NOSTR_IS_EVENT_BUS(bus), NULL);

    NostrEventBusClass *klass = NOSTR_EVENT_BUS_GET_CLASS(bus);
    return klass->subscribe(bus, topic_pattern, callback, user_data, destroy_notify);
}

NostrEventBusHandle *nostr_event_bus_subscribe_filtered(NostrEventBus *bus,
                                                         const gchar *topic_pattern,
                                                         NostrEventBusFilterFunc filter_func,
                                                         NostrEventBusCallback callback,
                                                         gpointer user_data,
                                                         GDestroyNotify destroy_notify) {
    g_return_val_if_fail(NOSTR_IS_EVENT_BUS(bus), NULL);
    g_return_val_if_fail(topic_pattern != NULL, NULL);
    g_return_val_if_fail(callback != NULL, NULL);

    NostrEventBusPrivate *priv = nostr_event_bus_get_instance_private(bus);

    g_mutex_lock(&priv->mutex);

    guint64 id = ++priv->next_subscription_id;

    Subscription *sub = subscription_new(id, topic_pattern, callback,
                                          filter_func, user_data, destroy_notify);

    g_hash_table_insert(priv->subscriptions, GUINT_TO_POINTER(id), sub);
    priv->subscription_count++;

    NostrEventBusHandle *handle = g_new0(NostrEventBusHandle, 1);
    handle->id = id;

    g_mutex_unlock(&priv->mutex);

    return handle;
}

void nostr_event_bus_unsubscribe(NostrEventBus *bus,
                                  NostrEventBusHandle *handle) {
    if (!handle) return;
    g_return_if_fail(NOSTR_IS_EVENT_BUS(bus));

    NostrEventBusClass *klass = NOSTR_EVENT_BUS_GET_CLASS(bus);
    klass->unsubscribe(bus, handle);
}

void nostr_event_bus_emit(NostrEventBus *bus,
                           const gchar *topic,
                           const gchar *event_json) {
    g_return_if_fail(NOSTR_IS_EVENT_BUS(bus));

    NostrEventBusClass *klass = NOSTR_EVENT_BUS_GET_CLASS(bus);
    klass->emit(bus, topic, event_json);
}

void nostr_event_bus_emit_batch(NostrEventBus *bus,
                                 const gchar *topic,
                                 const gchar *const *events_array,
                                 gsize count) {
    g_return_if_fail(NOSTR_IS_EVENT_BUS(bus));

    NostrEventBusClass *klass = NOSTR_EVENT_BUS_GET_CLASS(bus);
    klass->emit_batch(bus, topic, events_array, count);
}

/* --- Utility Functions --- */

gchar *nostr_event_bus_format_event_topic(gint kind) {
    return g_strdup_printf("event::kind::%d", kind);
}

gchar *nostr_event_bus_format_eose_topic(const gchar *subscription_id) {
    g_return_val_if_fail(subscription_id != NULL, NULL);
    return g_strdup_printf("eose::%s", subscription_id);
}

gchar *nostr_event_bus_format_ok_topic(const gchar *event_id) {
    g_return_val_if_fail(event_id != NULL, NULL);
    return g_strdup_printf("ok::%s", event_id);
}

/* --- Statistics --- */

void nostr_event_bus_get_stats(NostrEventBus *bus,
                                NostrEventBusStats *stats) {
    g_return_if_fail(NOSTR_IS_EVENT_BUS(bus));
    g_return_if_fail(stats != NULL);

    NostrEventBusPrivate *priv = nostr_event_bus_get_instance_private(bus);

    g_mutex_lock(&priv->mutex);

    stats->subscription_count = priv->subscription_count;
    stats->events_emitted = priv->events_emitted;
    stats->callbacks_invoked = priv->callbacks_invoked;
    stats->pattern_cache_hits = priv->pattern_cache_hits;
    stats->pattern_cache_misses = priv->pattern_cache_misses;

    g_mutex_unlock(&priv->mutex);
}
