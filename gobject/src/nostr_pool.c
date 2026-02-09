/**
 * SPDX-License-Identifier: MIT
 *
 * GNostrPool: GObject wrapper for managing multiple Nostr relay connections.
 *
 * Replaces GnostrSimplePool with a proper GObject implementation:
 * - Properties with notify signals (relays, default-timeout)
 * - Signals for relay lifecycle (relay-added, relay-removed, relay-state-changed)
 * - Async query methods with GCancellable support
 * - GListStore-backed relay list for GtkListView integration
 * - Proper GIR annotations for language binding support
 */

#define G_LOG_DOMAIN "gnostr-pool"

/* Core libnostr headers first */
#include "nostr-relay.h"
#include "nostr-event.h"
#include "nostr-filter.h"
#include "nostr-subscription.h"
#include "json.h"
#include "context.h"
#include "error.h"

/* GObject wrapper headers - note: we do NOT include nostr_subscription.h here
 * because it transitively includes nostr_filter.h (GObject) which conflicts
 * with the core nostr-filter.h included above. GNostrSubscription is forward-
 * declared in nostr_pool.h. */
#include "nostr_pool.h"
#include "nostr_relay.h"

/* Forward-declare subscription API to avoid include conflicts (nostrc-wjlt) */
GNostrSubscription *gnostr_subscription_new(GNostrRelay *relay, NostrFilters *filters);
gboolean gnostr_subscription_fire(GNostrSubscription *self, GError **error);
void gnostr_subscription_close(GNostrSubscription *self);
void gnostr_subscription_detach_filters(GNostrSubscription *self); /* nostrc-aaf0 */


#include <glib.h>
#include <gio/gio.h>
#include <string.h>

/* Property IDs */
enum {
    PROP_0,
    POOL_PROP_RELAYS,
    POOL_PROP_DEFAULT_TIMEOUT,
    POOL_N_PROPERTIES
};

static GParamSpec *pool_properties[POOL_N_PROPERTIES] = { NULL, };

/* Signal IDs */
enum {
    POOL_SIGNAL_RELAY_ADDED,
    POOL_SIGNAL_RELAY_REMOVED,
    POOL_SIGNAL_RELAY_STATE_CHANGED,
    POOL_N_SIGNALS
};

static guint pool_signals[POOL_N_SIGNALS] = { 0 };

struct _GNostrPool {
    GObject parent_instance;

    GListStore *relays;          /* GListStore of GNostrRelay* */
    guint default_timeout;       /* Default query timeout in ms */

    /* Internal: track state-changed signal handlers per relay */
    GHashTable *relay_handler_ids; /* url -> GUINT_TO_POINTER(handler_id) */

    /* NIP-42 AUTH: pool-wide auth handler applied to all relays (nostrc-kn38) */
    GNostrRelayAuthSignFunc auth_sign_func;
    gpointer auth_sign_data;
    GDestroyNotify auth_sign_destroy;
};

G_DEFINE_TYPE(GNostrPool, gnostr_pool, G_TYPE_OBJECT)

/* --- Internal helpers --- */

/**
 * Find a relay in the list store by URL. Returns the index or -1 if not found.
 */
static gint
find_relay_index(GNostrPool *self, const gchar *url)
{
    guint n = g_list_model_get_n_items(G_LIST_MODEL(self->relays));
    for (guint i = 0; i < n; i++) {
        g_autoptr(GNostrRelay) relay = g_list_model_get_item(G_LIST_MODEL(self->relays), i);
        const gchar *relay_url = gnostr_relay_get_url(relay);
        if (relay_url && g_strcmp0(relay_url, url) == 0)
            return (gint)i;
    }
    return -1;
}

/**
 * Callback for relay state-changed signals. Re-emits as pool::relay-state-changed.
 */
static void
on_relay_state_changed(GNostrRelay      *relay,
                       GNostrRelayState  old_state,
                       GNostrRelayState  new_state,
                       gpointer          user_data)
{
    GNostrPool *self = GNOSTR_POOL(user_data);

    g_signal_emit(self, pool_signals[POOL_SIGNAL_RELAY_STATE_CHANGED], 0,
                  relay, new_state);
}

/**
 * Connect relay state-changed signal and track the handler.
 */
static void
watch_relay(GNostrPool *self, GNostrRelay *relay)
{
    const gchar *url = gnostr_relay_get_url(relay);
    if (!url) return;

    gulong handler_id = g_signal_connect(relay, "state-changed",
                                         G_CALLBACK(on_relay_state_changed), self);
    g_hash_table_insert(self->relay_handler_ids,
                        g_strdup(url),
                        GUINT_TO_POINTER((guint)handler_id));
}

/**
 * Disconnect relay state-changed signal handler.
 */
static void
unwatch_relay(GNostrPool *self, GNostrRelay *relay)
{
    const gchar *url = gnostr_relay_get_url(relay);
    if (!url) return;

    gpointer val = NULL;
    if (g_hash_table_lookup_extended(self->relay_handler_ids, url, NULL, &val)) {
        gulong handler_id = (gulong)GPOINTER_TO_UINT(val);
        if (handler_id > 0 && g_signal_handler_is_connected(relay, handler_id)) {
            g_signal_handler_disconnect(relay, handler_id);
        }
        g_hash_table_remove(self->relay_handler_ids, url);
    }
}

/* --- GObject boilerplate --- */

static void
gnostr_pool_get_property(GObject    *object,
                         guint       property_id,
                         GValue     *value,
                         GParamSpec *pspec)
{
    GNostrPool *self = GNOSTR_POOL(object);

    switch (property_id) {
    case POOL_PROP_RELAYS:
        g_value_set_object(value, self->relays);
        break;
    case POOL_PROP_DEFAULT_TIMEOUT:
        g_value_set_uint(value, self->default_timeout);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
}

static void
gnostr_pool_set_property(GObject      *object,
                         guint         property_id,
                         const GValue *value,
                         GParamSpec   *pspec)
{
    GNostrPool *self = GNOSTR_POOL(object);

    switch (property_id) {
    case POOL_PROP_DEFAULT_TIMEOUT:
        gnostr_pool_set_default_timeout(self, g_value_get_uint(value));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
}

static void
gnostr_pool_finalize(GObject *object)
{
    GNostrPool *self = GNOSTR_POOL(object);

    /* Disconnect all relay watchers */
    guint n = g_list_model_get_n_items(G_LIST_MODEL(self->relays));
    for (guint i = 0; i < n; i++) {
        g_autoptr(GNostrRelay) relay = g_list_model_get_item(G_LIST_MODEL(self->relays), i);
        unwatch_relay(self, relay);
    }

    /* Clean up NIP-42 auth handler (nostrc-kn38) */
    if (self->auth_sign_destroy && self->auth_sign_data) {
        self->auth_sign_destroy(self->auth_sign_data);
    }
    self->auth_sign_func = NULL;
    self->auth_sign_data = NULL;
    self->auth_sign_destroy = NULL;

    g_clear_pointer(&self->relay_handler_ids, g_hash_table_destroy);
    g_clear_object(&self->relays);

    G_OBJECT_CLASS(gnostr_pool_parent_class)->finalize(object);
}

static void
gnostr_pool_class_init(GNostrPoolClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->get_property = gnostr_pool_get_property;
    object_class->set_property = gnostr_pool_set_property;
    object_class->finalize = gnostr_pool_finalize;

    /**
     * GNostrPool:relays:
     *
     * The list of relays in the pool as a GListStore.
     * Items are #GNostrRelay objects.
     */
    pool_properties[POOL_PROP_RELAYS] =
        g_param_spec_object("relays",
                            "Relays",
                            "GListStore of GNostrRelay objects",
                            G_TYPE_LIST_STORE,
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    /**
     * GNostrPool:default-timeout:
     *
     * Default timeout in milliseconds for query operations.
     * 0 means no timeout.
     */
    pool_properties[POOL_PROP_DEFAULT_TIMEOUT] =
        g_param_spec_uint("default-timeout",
                          "Default Timeout",
                          "Default query timeout in milliseconds (0 = no timeout)",
                          0, G_MAXUINT, 30000,
                          G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, POOL_N_PROPERTIES, pool_properties);

    /* Signals */

    /**
     * GNostrPool::relay-added:
     * @self: the pool
     * @relay: the #GNostrRelay that was added
     *
     * Emitted when a relay is added to the pool.
     */
    pool_signals[POOL_SIGNAL_RELAY_ADDED] =
        g_signal_new("relay-added",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_FIRST,
                     0, NULL, NULL, NULL,
                     G_TYPE_NONE, 1,
                     GNOSTR_TYPE_RELAY);

    /**
     * GNostrPool::relay-removed:
     * @self: the pool
     * @relay: the #GNostrRelay that was removed
     *
     * Emitted when a relay is removed from the pool.
     */
    pool_signals[POOL_SIGNAL_RELAY_REMOVED] =
        g_signal_new("relay-removed",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_FIRST,
                     0, NULL, NULL, NULL,
                     G_TYPE_NONE, 1,
                     GNOSTR_TYPE_RELAY);

    /**
     * GNostrPool::relay-state-changed:
     * @self: the pool
     * @relay: the #GNostrRelay whose state changed
     * @state: the new #GNostrRelayState
     *
     * Emitted when any relay in the pool changes connection state.
     */
    pool_signals[POOL_SIGNAL_RELAY_STATE_CHANGED] =
        g_signal_new("relay-state-changed",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_FIRST,
                     0, NULL, NULL, NULL,
                     G_TYPE_NONE, 2,
                     GNOSTR_TYPE_RELAY,
                     GNOSTR_TYPE_RELAY_STATE);
}

static void
gnostr_pool_init(GNostrPool *self)
{
    self->relays = g_list_store_new(GNOSTR_TYPE_RELAY);
    self->default_timeout = 30000; /* 30 seconds */
    self->relay_handler_ids = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                     g_free, NULL);
}

/* --- Public API --- */

GNostrPool *
gnostr_pool_new(void)
{
    return g_object_new(GNOSTR_TYPE_POOL, NULL);
}

GNostrRelay *
gnostr_pool_add_relay(GNostrPool *self, const gchar *url)
{
    g_return_val_if_fail(GNOSTR_IS_POOL(self), NULL);
    g_return_val_if_fail(url != NULL, NULL);

    /* Check if already present */
    gint idx = find_relay_index(self, url);
    if (idx >= 0) {
        g_autoptr(GNostrRelay) existing = g_list_model_get_item(G_LIST_MODEL(self->relays), idx);
        return existing;
    }

    /* Create new relay and add */
    GNostrRelay *relay = gnostr_relay_new(url);
    if (!relay)
        return NULL;

    /* NIP-42: Apply pool-wide auth handler to new relay (nostrc-kn38) */
    if (self->auth_sign_func) {
        gnostr_relay_set_auth_handler(relay, self->auth_sign_func,
                                      self->auth_sign_data, NULL);
    }

    g_list_store_append(self->relays, relay);
    watch_relay(self, relay);

    g_signal_emit(self, pool_signals[POOL_SIGNAL_RELAY_ADDED], 0, relay);
    g_debug("Added relay: %s (total: %u)", url,
            g_list_model_get_n_items(G_LIST_MODEL(self->relays)));

    /* Return borrowed ref - the list store owns it */
    g_object_unref(relay);

    /* Look up again to return the store-owned ref */
    idx = find_relay_index(self, url);
    if (idx >= 0) {
        g_autoptr(GNostrRelay) r = g_list_model_get_item(G_LIST_MODEL(self->relays), idx);
        return r;
    }
    return NULL;
}

gboolean
gnostr_pool_add_relay_object(GNostrPool *self, GNostrRelay *relay)
{
    g_return_val_if_fail(GNOSTR_IS_POOL(self), FALSE);
    g_return_val_if_fail(GNOSTR_IS_RELAY(relay), FALSE);

    const gchar *url = gnostr_relay_get_url(relay);
    if (!url)
        return FALSE;

    /* Check if already present */
    if (find_relay_index(self, url) >= 0)
        return FALSE;

    /* Apply pool-wide NIP-42 AUTH handler if set (nostrc-kn38) */
    if (self->auth_sign_func) {
        gnostr_relay_set_auth_handler(relay, self->auth_sign_func,
                                      self->auth_sign_data, NULL);
    }

    g_list_store_append(self->relays, relay);
    watch_relay(self, relay);

    g_signal_emit(self, pool_signals[POOL_SIGNAL_RELAY_ADDED], 0, relay);
    g_debug("Added relay object: %s (total: %u)", url,
            g_list_model_get_n_items(G_LIST_MODEL(self->relays)));

    return TRUE;
}

gboolean
gnostr_pool_remove_relay(GNostrPool *self, const gchar *url)
{
    g_return_val_if_fail(GNOSTR_IS_POOL(self), FALSE);
    g_return_val_if_fail(url != NULL, FALSE);

    gint idx = find_relay_index(self, url);
    if (idx < 0)
        return FALSE;

    g_autoptr(GNostrRelay) relay = g_list_model_get_item(G_LIST_MODEL(self->relays), idx);

    /* Unwatch before removal; relay stays alive if other pools hold refs */
    unwatch_relay(self, relay);

    /* Remove from store (this unrefs the relay) */
    g_list_store_remove(self->relays, idx);

    g_signal_emit(self, pool_signals[POOL_SIGNAL_RELAY_REMOVED], 0, relay);
    g_debug("Removed relay: %s (remaining: %u)", url,
            g_list_model_get_n_items(G_LIST_MODEL(self->relays)));

    return TRUE;
}

GNostrRelay *
gnostr_pool_get_relay(GNostrPool *self, const gchar *url)
{
    g_return_val_if_fail(GNOSTR_IS_POOL(self), NULL);
    g_return_val_if_fail(url != NULL, NULL);

    gint idx = find_relay_index(self, url);
    if (idx < 0)
        return NULL;

    g_autoptr(GNostrRelay) relay = g_list_model_get_item(G_LIST_MODEL(self->relays), idx);
    return relay;
}

GListStore *
gnostr_pool_get_relays(GNostrPool *self)
{
    g_return_val_if_fail(GNOSTR_IS_POOL(self), NULL);
    return self->relays;
}

guint
gnostr_pool_get_relay_count(GNostrPool *self)
{
    g_return_val_if_fail(GNOSTR_IS_POOL(self), 0);
    return g_list_model_get_n_items(G_LIST_MODEL(self->relays));
}

void
gnostr_pool_sync_relays(GNostrPool *self, const gchar **urls, gsize url_count)
{
    g_return_if_fail(GNOSTR_IS_POOL(self));

    /* Build a set of desired URLs for O(1) lookup */
    GHashTable *desired = g_hash_table_new(g_str_hash, g_str_equal);
    for (gsize i = 0; i < url_count; i++) {
        if (urls[i])
            g_hash_table_add(desired, (gpointer)urls[i]);
    }

    /* Remove relays not in the desired set (iterate in reverse for safe removal) */
    guint n = g_list_model_get_n_items(G_LIST_MODEL(self->relays));
    for (gint i = (gint)n - 1; i >= 0; i--) {
        g_autoptr(GNostrRelay) relay = g_list_model_get_item(G_LIST_MODEL(self->relays), i);
        const gchar *url = gnostr_relay_get_url(relay);
        if (url && !g_hash_table_contains(desired, url)) {
            gnostr_pool_remove_relay(self, url);
        }
    }

    /* Add relays that aren't already present */
    for (gsize i = 0; i < url_count; i++) {
        if (urls[i] && find_relay_index(self, urls[i]) < 0) {
            gnostr_pool_add_relay(self, urls[i]);
        }
    }

    g_hash_table_destroy(desired);
}

guint
gnostr_pool_get_default_timeout(GNostrPool *self)
{
    g_return_val_if_fail(GNOSTR_IS_POOL(self), 0);
    return self->default_timeout;
}

void
gnostr_pool_set_default_timeout(GNostrPool *self, guint timeout_ms)
{
    g_return_if_fail(GNOSTR_IS_POOL(self));

    if (self->default_timeout == timeout_ms)
        return;

    self->default_timeout = timeout_ms;
    g_object_notify_by_pspec(G_OBJECT(self), pool_properties[POOL_PROP_DEFAULT_TIMEOUT]);
}

/* --- Async query implementation --- */

typedef struct {
    GNostrPool *pool;           /* weak ref */
    GPtrArray *results;         /* collected event JSON strings */
    GHashTable *seen_ids;       /* dedup set */
    guint pending_relays;       /* count of relays not yet EOSE */
    guint timeout_source_id;    /* GSource ID for timeout */
    gboolean completed;         /* whether we've already returned results */
} QueryAsyncData;

static void
query_async_data_free(QueryAsyncData *data)
{
    if (!data) return;
    if (data->timeout_source_id > 0)
        g_source_remove(data->timeout_source_id);
    g_clear_pointer(&data->seen_ids, g_hash_table_destroy);
    /* Don't free results - ownership transferred to GTask */
    g_free(data);
}

static void
query_complete(GTask *task, QueryAsyncData *data)
{
    if (data->completed)
        return;
    data->completed = TRUE;

    if (data->timeout_source_id > 0) {
        g_source_remove(data->timeout_source_id);
        data->timeout_source_id = 0;
    }

    g_task_return_pointer(task, data->results,
                          (GDestroyNotify)g_ptr_array_unref);
}

static gboolean
query_timeout_cb(gpointer user_data)
{
    GTask *task = G_TASK(user_data);
    QueryAsyncData *data = g_task_get_task_data(task);

    data->timeout_source_id = 0;
    g_debug("Query timed out with %u results", data->results->len);
    query_complete(task, data);

    return G_SOURCE_REMOVE;
}

/* Worker thread for async query */
static void
query_thread_func(GTask         *task,
                  gpointer       source_object,
                  gpointer       task_data,
                  GCancellable  *cancellable)
{
    GNostrPool *self = GNOSTR_POOL(source_object);
    QueryAsyncData *data = (QueryAsyncData *)task_data;
    NostrFilters *filters = g_object_get_data(G_OBJECT(task), "filters");

    guint n_relays = g_list_model_get_n_items(G_LIST_MODEL(self->relays));
    g_debug("pool_query_thread: %u relays in pool, filters=%p (count=%zu)",
              n_relays, (void *)filters, filters ? filters->count : 0);
    if (n_relays == 0) {
        g_debug("pool_query_thread: 0 relays - returning empty");
        g_task_return_pointer(task,
                              g_ptr_array_new_with_free_func(g_free),
                              (GDestroyNotify)g_ptr_array_unref);
        return;
    }

    /* Create subscriptions per relay */
    typedef struct {
        NostrRelay *core_relay;
        struct NostrSubscription *sub;
        gboolean eosed;
        gboolean owns_relay; /* whether we created this relay for the query */
    } RelaySubItem;

    GPtrArray *items = g_ptr_array_new();
    GoContext *bg = go_context_background();

    for (guint i = 0; i < n_relays; i++) {
        if (cancellable && g_cancellable_is_cancelled(cancellable))
            break;

        g_autoptr(GNostrRelay) grelay = g_list_model_get_item(G_LIST_MODEL(self->relays), i);
        NostrRelay *core_relay = gnostr_relay_get_core_relay(grelay);
        gboolean relay_connected = gnostr_relay_get_connected(grelay);

        if (!core_relay || !relay_connected) {
            /* Relay not connected - create a temporary one for this query.
             * GNostrRelay always creates a core relay in its constructor,
             * but it may not be connected. We need a connected relay to
             * subscribe, so create a fresh temporary one. */
            const gchar *url = gnostr_relay_get_url(grelay);
            if (!url) continue;

            g_debug("pool_query_thread: relay[%u] %s not connected, creating temp", i, url);
            Error *err = NULL;
            NostrRelay *temp_relay = nostr_relay_new(bg, url, &err);
            if (!temp_relay) {
                if (err) {
                    g_debug("pool_query_thread: relay[%u] %s create FAILED: %s", i, url,
                            err->message ? err->message : "unknown");
                    free_error(err);
                }
                continue;
            }
            temp_relay->assume_valid = true;
            if (!nostr_relay_connect(temp_relay, &err)) {
                if (err) {
                    g_debug("pool_query_thread: relay[%u] %s connect FAILED: %s", i, url,
                            err->message ? err->message : "unknown");
                    free_error(err);
                }
                nostr_relay_free(temp_relay);
                continue;
            }
            g_debug("pool_query_thread: relay[%u] %s connected OK", i, url);

            RelaySubItem *item = g_new0(RelaySubItem, 1);
            item->core_relay = temp_relay;
            item->owns_relay = TRUE;

            struct NostrSubscription *sub = nostr_relay_prepare_subscription(temp_relay, bg, filters);
            if (!sub) {
                g_debug("pool_query_thread: relay[%u] %s prepare_subscription FAILED", i, url);
                nostr_relay_disconnect(temp_relay);
                nostr_relay_free(temp_relay);
                g_free(item);
                continue;
            }

            Error *fire_err = NULL;
            if (!nostr_subscription_fire(sub, &fire_err)) {
                g_debug("pool_query_thread: relay[%u] %s fire FAILED: %s", i, url,
                          fire_err ? fire_err->message : "unknown");
                if (fire_err) free_error(fire_err);
                nostr_subscription_free(sub);
                nostr_relay_disconnect(temp_relay);
                nostr_relay_free(temp_relay);
                g_free(item);
                continue;
            }
            g_debug("pool_query_thread: relay[%u] %s subscribed OK", i, url);

            item->sub = sub;
            g_ptr_array_add(items, item);
            continue;
        }

        /* Use existing connected relay */
        RelaySubItem *item = g_new0(RelaySubItem, 1);
        item->core_relay = core_relay;
        item->owns_relay = FALSE;

        struct NostrSubscription *sub = nostr_relay_prepare_subscription(core_relay, bg, filters);
        if (!sub) {
            g_free(item);
            continue;
        }

        Error *fire_err = NULL;
        if (!nostr_subscription_fire(sub, &fire_err)) {
            if (fire_err) free_error(fire_err);
            nostr_subscription_free(sub);
            g_free(item);
            continue;
        }

        item->sub = sub;
        g_ptr_array_add(items, item);
    }

    g_debug("pool_query_thread: %u active subscriptions across relays", items->len);
    if (items->len == 0) {
        g_debug("pool_query_thread: 0 subscriptions - returning %u results", data->results->len);
        g_ptr_array_unref(items);
        g_task_return_pointer(task,
                              data->results,
                              (GDestroyNotify)g_ptr_array_unref);
        return;
    }

    /* Drain events from all subscriptions until all EOSE or cancelled/timeout */
    guint64 deadline_us = 0;
    if (self->default_timeout > 0) {
        deadline_us = g_get_monotonic_time() + (guint64)self->default_timeout * 1000;
    }
    g_debug("pool_query_thread: polling for events (timeout=%ums)", self->default_timeout);

    for (;;) {
        if (cancellable && g_cancellable_is_cancelled(cancellable))
            break;
        if (deadline_us > 0 && (guint64)g_get_monotonic_time() > deadline_us)
            break;

        gboolean any_activity = FALSE;
        gboolean all_eosed = TRUE;

        for (guint i = 0; i < items->len; i++) {
            RelaySubItem *item = g_ptr_array_index(items, i);
            if (!item || !item->sub || item->eosed) continue;
            all_eosed = FALSE;

            /* Check for events */
            void *msg = NULL;
            GoChannel *ch_events = nostr_subscription_get_events_channel(item->sub);
            while (ch_events && go_channel_try_receive(ch_events, &msg) == 0) {
                any_activity = TRUE;
                if (msg) {
                    NostrEvent *ev = (NostrEvent *)msg;
                    char *eid = nostr_event_get_id(ev);
                    if (eid && *eid && !g_hash_table_contains(data->seen_ids, eid)) {
                        g_hash_table_add(data->seen_ids, g_strdup(eid));
                        char *json = nostr_event_serialize(ev);
                        if (json)
                            g_ptr_array_add(data->results, json);
                    }
                    free(eid);
                    nostr_event_free(ev);
                }
                msg = NULL;
            }

            /* Check for EOSE */
            GoChannel *ch_eose = nostr_subscription_get_eose_channel(item->sub);
            if (ch_eose && go_channel_try_receive(ch_eose, NULL) == 0) {
                item->eosed = TRUE;
                any_activity = TRUE;
            }
        }

        if (all_eosed) {
            g_debug("pool_query_thread: all relays sent EOSE, %u results collected", data->results->len);
            break;
        }
        if (!any_activity)
            g_usleep(1000); /* 1ms backoff */
    }
    g_debug("pool_query_thread: poll loop done, %u results total", data->results->len);

    /* Cleanup subscriptions and temp relays */
    for (guint i = 0; i < items->len; i++) {
        RelaySubItem *item = g_ptr_array_index(items, i);
        if (!item) continue;
        if (item->sub) {
            nostr_subscription_close(item->sub, NULL);
            nostr_subscription_free(item->sub);
        }
        if (item->owns_relay && item->core_relay) {
            nostr_relay_disconnect(item->core_relay);
            nostr_relay_free(item->core_relay);
        }
        g_free(item);
    }
    g_ptr_array_unref(items);

    g_debug("Query completed with %u results", data->results->len);
    g_task_return_pointer(task, data->results,
                          (GDestroyNotify)g_ptr_array_unref);
}

void
gnostr_pool_query_async(GNostrPool          *self,
                        NostrFilters         *filters,
                        GCancellable         *cancellable,
                        GAsyncReadyCallback   callback,
                        gpointer              user_data)
{
    g_return_if_fail(GNOSTR_IS_POOL(self));
    g_return_if_fail(filters != NULL);

    GTask *task = g_task_new(self, cancellable, callback, user_data);
    g_task_set_source_tag(task, gnostr_pool_query_async);
    /* nostrc-ns2k: Never discard results due to cancellation. The thread
     * checks cancellation itself and returns partial results. Without this,
     * g_task_propagate_pointer returns NULL + G_IO_ERROR_CANCELLED if the
     * GCancellable is cancelled, silently eating all collected events. */
    g_task_set_check_cancellable(task, FALSE);

    QueryAsyncData *data = g_new0(QueryAsyncData, 1);
    data->pool = self;
    data->results = g_ptr_array_new_with_free_func(g_free);
    data->seen_ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    data->completed = FALSE;

    g_task_set_task_data(task, data, (GDestroyNotify)query_async_data_free);

    /* Stash filters on the task so the thread can access them.
     * Caller retains ownership but filters must outlive the task thread's
     * initial subscription setup. We store without destroy notify since
     * the caller manages lifetime (typically via context struct or pool stash). */
    g_object_set_data(G_OBJECT(task), "filters", filters);

    g_task_run_in_thread(task, query_thread_func);
    g_object_unref(task);
}

GPtrArray *
gnostr_pool_query_finish(GNostrPool   *self,
                         GAsyncResult *result,
                         GError      **error)
{
    g_return_val_if_fail(GNOSTR_IS_POOL(self), NULL);
    g_return_val_if_fail(g_task_is_valid(result, self), NULL);

    return g_task_propagate_pointer(G_TASK(result), error);
}

/* --- Connect All --- */

typedef struct {
    guint total;
    guint completed;
    guint succeeded;
    gboolean returned;  /* TRUE after g_task_return called (exactly once) */
} ConnectAllData;

static void
connect_one_relay_cb(GObject      *source,
                     GAsyncResult *result,
                     gpointer      user_data)
{
    GNostrRelay *relay = GNOSTR_RELAY(source);
    GTask *task = G_TASK(user_data);
    ConnectAllData *data = g_task_get_task_data(task);

    GError *error = NULL;
    if (gnostr_relay_connect_finish(relay, result, &error)) {
        data->succeeded++;
    } else {
        g_debug("Relay connect failed: %s", error ? error->message : "unknown");
        g_clear_error(&error);
    }

    data->completed++;

    /* Return as soon as we have an answer:
     * - First successful connection → return TRUE immediately (fast startup).
     *   Remaining relays keep connecting in the background.
     * - All relays failed → return FALSE. */
    if (!data->returned) {
        if (data->succeeded > 0) {
            data->returned = TRUE;
            g_task_return_boolean(task, TRUE);
        } else if (data->completed >= data->total) {
            data->returned = TRUE;
            g_task_return_boolean(task, FALSE);
        }
    }

    /* Only unref when ALL callbacks have fired, regardless of when
     * we returned the result.  This keeps data alive for late arrivals. */
    if (data->completed >= data->total) {
        g_object_unref(task);
    }
}

void
gnostr_pool_connect_all_async(GNostrPool         *self,
                              GCancellable       *cancellable,
                              GAsyncReadyCallback callback,
                              gpointer            user_data)
{
    g_return_if_fail(GNOSTR_IS_POOL(self));

    GTask *task = g_task_new(self, cancellable, callback, user_data);
    g_task_set_source_tag(task, gnostr_pool_connect_all_async);

    guint n = g_list_model_get_n_items(G_LIST_MODEL(self->relays));
    if (n == 0) {
        g_task_return_boolean(task, TRUE);
        g_object_unref(task);
        return;
    }

    ConnectAllData *data = g_new0(ConnectAllData, 1);
    data->total = n;
    g_task_set_task_data(task, data, g_free);

    for (guint i = 0; i < n; i++) {
        g_autoptr(GNostrRelay) relay = g_list_model_get_item(G_LIST_MODEL(self->relays), i);
        gnostr_relay_connect_async(relay, cancellable, connect_one_relay_cb, task);
    }
}

gboolean
gnostr_pool_connect_all_finish(GNostrPool   *self,
                               GAsyncResult *result,
                               GError      **error)
{
    g_return_val_if_fail(GNOSTR_IS_POOL(self), FALSE);
    g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

    return g_task_propagate_boolean(G_TASK(result), error);
}

void
gnostr_pool_disconnect_all(GNostrPool *self)
{
    g_return_if_fail(GNOSTR_IS_POOL(self));

    /* Release relays — shared relays survive if other pools hold refs,
     * unshared relays finalize and disconnect immediately. */
    guint n = g_list_model_get_n_items(G_LIST_MODEL(self->relays));
    for (gint i = (gint)n - 1; i >= 0; i--) {
        g_autoptr(GNostrRelay) relay = g_list_model_get_item(G_LIST_MODEL(self->relays), i);
        unwatch_relay(self, relay);
        g_list_store_remove(self->relays, i);
    }
}

/* --- Subscription API (nostrc-wjlt) --- */

GNostrSubscription *
gnostr_pool_subscribe(GNostrPool   *self,
                      NostrFilters *filters,
                      GError      **error)
{
    g_return_val_if_fail(GNOSTR_IS_POOL(self), NULL);
    g_return_val_if_fail(filters != NULL, NULL);

    /* Find first connected relay */
    GNostrRelay *connected_relay = NULL;
    guint n = g_list_model_get_n_items(G_LIST_MODEL(self->relays));
    for (guint i = 0; i < n; i++) {
        g_autoptr(GNostrRelay) relay = g_list_model_get_item(G_LIST_MODEL(self->relays), i);
        if (gnostr_relay_get_connected(relay)) {
            connected_relay = relay;
            break;
        }
    }

    if (!connected_relay) {
        g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_CONNECTION_FAILED,
                            "no connected relay in pool");
        return NULL;
    }

    GNostrSubscription *sub = gnostr_subscription_new(connected_relay, filters);
    if (!sub) {
        g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_CONNECTION_FAILED,
                            "failed to create subscription");
        return NULL;
    }

    if (!gnostr_subscription_fire(sub, error)) {
        /* Detach filters before unref so caller retains ownership on failure (nostrc-aaf0) */
        gnostr_subscription_detach_filters(sub);
        g_object_unref(sub);
        return NULL;
    }

    g_debug("Pool subscribe: created subscription on %s",
            gnostr_relay_get_url(connected_relay));

    return sub;
}

/* --- NIP-42 AUTH handler API (nostrc-kn38) --- */

void
gnostr_pool_set_auth_handler(GNostrPool              *self,
                              GNostrRelayAuthSignFunc   sign_func,
                              gpointer                 user_data,
                              GDestroyNotify            destroy)
{
    g_return_if_fail(GNOSTR_IS_POOL(self));

    /* Clean up old handler */
    if (self->auth_sign_destroy && self->auth_sign_data) {
        self->auth_sign_destroy(self->auth_sign_data);
    }

    self->auth_sign_func = sign_func;
    self->auth_sign_data = user_data;
    self->auth_sign_destroy = destroy;

    /* Apply to all existing relays */
    guint n = g_list_model_get_n_items(G_LIST_MODEL(self->relays));
    for (guint i = 0; i < n; i++) {
        g_autoptr(GNostrRelay) relay = g_list_model_get_item(G_LIST_MODEL(self->relays), i);
        gnostr_relay_set_auth_handler(relay, sign_func, user_data, NULL);
    }

    g_debug("NIP-42: auth handler %s for pool (%u relays)",
            sign_func ? "set" : "cleared", n);
}
