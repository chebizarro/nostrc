/**
 * SPDX-License-Identifier: MIT
 *
 * GNostrPool: GObject wrapper for managing multiple Nostr relay connections.
 *
 * Replaces GNostrSimplePool with a proper GObject implementation:
 * - Properties with notify signals (relays)
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

    /* Internal: track state-changed signal handlers per relay */
    GHashTable *relay_handler_ids; /* url -> GUINT_TO_POINTER(handler_id) */

    /* NIP-42 AUTH: pool-wide auth handler applied to all relays (nostrc-kn38) */
    GNostrRelayAuthSignFunc auth_sign_func;
    gpointer auth_sign_data;
    GDestroyNotify auth_sign_destroy;

    /* Event sink: callback for persisting fetched events (e.g. nostrdb) */
    GNostrPoolEventSinkFunc event_sink_func;
    gpointer event_sink_data;
    GDestroyNotify event_sink_destroy;

    /* Cache query: check local store before hitting the network */
    GNostrPoolCacheQueryFunc cache_query_func;
    gpointer cache_query_data;
    GDestroyNotify cache_query_destroy;
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

    /* Clean up event sink */
    if (self->event_sink_destroy && self->event_sink_data) {
        self->event_sink_destroy(self->event_sink_data);
    }
    self->event_sink_func = NULL;
    self->event_sink_data = NULL;
    self->event_sink_destroy = NULL;

    /* Clean up cache query */
    if (self->cache_query_destroy && self->cache_query_data) {
        self->cache_query_destroy(self->cache_query_data);
    }
    self->cache_query_func = NULL;
    self->cache_query_data = NULL;
    self->cache_query_destroy = NULL;

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

/* --- Async query implementation --- */

/* nostrc-snap: Relay snapshot entry — captured on main thread, read-only on
 * worker thread.  Prevents races with sync_relays() / add_relay() mutating
 * the GListStore concurrently (GListStore is NOT thread-safe). */
typedef struct {
    gchar *url;
    NostrRelay *core_relay;     /* borrowed from GNostrRelay (ref held below) */
    GNostrRelay *grelay_ref;    /* ref held to keep core_relay alive */
    gboolean connected;
} RelaySnapshotEntry;

static void
relay_snapshot_entry_free(gpointer p)
{
    RelaySnapshotEntry *e = p;
    if (!e) return;
    g_free(e->url);
    g_clear_object(&e->grelay_ref);
    g_free(e);
}

typedef struct {
    GNostrPool *pool;           /* weak ref */
    GPtrArray *results;         /* collected event JSON strings */
    GHashTable *seen_ids;       /* dedup set */
    /* nostrc-snap: Immutable relay list snapshot for worker thread */
    GPtrArray *relay_snapshots; /* RelaySnapshotEntry* (owned) */
    /* Event sink: snapshot of pool's sink callback for worker thread */
    GNostrPoolEventSinkFunc event_sink_func;
    gpointer event_sink_data;
    /* Cache query: snapshot of pool's cache callback for worker thread */
    GNostrPoolCacheQueryFunc cache_query_func;
    gpointer cache_query_data;
} QueryAsyncData;

static void
query_async_data_free(QueryAsyncData *data)
{
    if (!data) return;
    g_clear_pointer(&data->seen_ids, g_hash_table_destroy);
    g_clear_pointer(&data->relay_snapshots, g_ptr_array_unref);
    /* Don't free results - ownership transferred to GTask */
    g_free(data);
}

/* nostrc-snap: Worker thread for async query.
 * Uses relay_snapshots (captured on main thread) instead of self->relays
 * to avoid racing with sync_relays() / add_relay() / disconnect_all()
 * that other features call on the shared pool from the main thread.
 * GListStore is NOT thread-safe — direct access caused 100+ commits of
 * broken NIP-66, follows, threads, and profile loading. */
static void
query_thread_func(GTask         *task,
                  gpointer       source_object,
                  gpointer       task_data,
                  GCancellable  *cancellable)
{
    QueryAsyncData *data = (QueryAsyncData *)task_data;
    NostrFilters *filters = g_object_get_data(G_OBJECT(task), "filters");

    /* Check local cache first — avoid network round-trip if data exists */
    if (data->cache_query_func && filters) {
        GPtrArray *cached = data->cache_query_func(filters, data->cache_query_data);
        if (cached && cached->len > 0) {
            fprintf(stderr, "[POOL_QUERY] cache HIT — %u results, skipping network\n", cached->len);
            g_task_return_pointer(task, cached, (GDestroyNotify)g_ptr_array_unref);
            return;
        }
        fprintf(stderr, "[POOL_QUERY] cache MISS (cached=%p len=%u)\n",
                (void *)cached, cached ? cached->len : 0);
        if (cached) g_ptr_array_unref(cached);
    } else {
        fprintf(stderr, "[POOL_QUERY] no cache func or no filters (cache=%p filters=%p)\n",
                (void *)data->cache_query_func, (void *)filters);
    }

    /* nostrc-snap: Use snapshot captured on main thread, NOT self->relays */
    guint n_relays = data->relay_snapshots ? data->relay_snapshots->len : 0;
    fprintf(stderr, "[POOL_QUERY] START: %u relays (snapshot), filters=%p (count=%zu)\n",
              n_relays, (void *)filters, filters ? filters->count : 0);
    if (n_relays == 0) {
        fprintf(stderr, "[POOL_QUERY] 0 relays - returning empty\n");
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
    } RelaySubItem;

    GPtrArray *items = g_ptr_array_new();
    GoContext *bg = go_context_background();

    for (guint i = 0; i < n_relays; i++) {
        if (cancellable && g_cancellable_is_cancelled(cancellable))
            break;

        /* nostrc-snap: Read from immutable snapshot, not GListStore */
        RelaySnapshotEntry *snap = g_ptr_array_index(data->relay_snapshots, i);
        NostrRelay *core_relay = snap->core_relay;
        const gchar *url = snap->url;
        if (!url || !core_relay) continue;

        /* Connect the relay if not already connected.
         * nostr_relay_connect() returns immediately when relay->connection
         * is already set (e.g. connected by connect_all_async in another
         * thread, or shared via the relay registry).  Otherwise it blocks
         * on DNS + TLS + WS handshake — fine for a worker thread.
         * The connection persists on the real relay for future queries,
         * unlike the old temp-relay approach which created throwaway
         * connections every single query. */
        Error *conn_err = NULL;
        gint64 t0 = g_get_monotonic_time();
        fprintf(stderr, "[POOL_QUERY] relay[%u] %s: connecting (connection=%p)...\n",
                i, url, (void *)core_relay->connection);
        if (!nostr_relay_connect(core_relay, &conn_err)) {
            gint64 dt = g_get_monotonic_time() - t0;
            fprintf(stderr, "[POOL_QUERY] relay[%u] %s: CONNECT FAILED after %lldms: %s\n",
                    i, url, (long long)(dt / 1000),
                    conn_err ? conn_err->message : "unknown");
            if (conn_err) free_error(conn_err);
            continue;
        }
        gint64 dt = g_get_monotonic_time() - t0;
        fprintf(stderr, "[POOL_QUERY] relay[%u] %s: connected OK in %lldms\n",
                i, url, (long long)(dt / 1000));

        struct NostrSubscription *sub = nostr_relay_prepare_subscription(core_relay, bg, filters);
        if (!sub) {
            fprintf(stderr, "[POOL_QUERY] relay[%u] %s: prepare_subscription FAILED (filters=%p count=%zu)\n",
                    i, url, (void *)filters, filters ? filters->count : 0);
            continue;
        }

        Error *fire_err = NULL;
        if (!nostr_subscription_fire(sub, &fire_err)) {
            fprintf(stderr, "[POOL_QUERY] relay[%u] %s: FIRE FAILED: %s\n",
                    i, url, fire_err ? fire_err->message : "unknown");
            if (fire_err) free_error(fire_err);
            nostr_subscription_free(sub);
            continue;
        }

        fprintf(stderr, "[POOL_QUERY] relay[%u] %s: subscription FIRED OK\n", i, url);
        RelaySubItem *item = g_new0(RelaySubItem, 1);
        item->core_relay = core_relay;
        item->sub = sub;
        g_ptr_array_add(items, item);
    }

    fprintf(stderr, "[POOL_QUERY] %u active subscriptions across relays\n", items->len);
    if (items->len == 0) {
        fprintf(stderr, "[POOL_QUERY] 0 subscriptions - returning %u results\n", data->results->len);
        g_ptr_array_unref(items);
        g_task_return_pointer(task,
                              data->results,
                              (GDestroyNotify)g_ptr_array_unref);
        return;
    }

    /* nostrc-blk1: Drain events using protocol signals only — no arbitrary
     * timeouts.  Relays signal completion via EOSE, or the subscription
     * gets closed/disconnected.  This is WebSocket + Nostr, not REST.
     *
     * EOSE quorum: once a majority of relays have EOSE'd, start a short
     * grace window (2s) for stragglers, then break.  One misbehaving relay
     * must not block the entire query for minutes. */
    gint64 poll_start = g_get_monotonic_time();
    gint64 first_event_time = 0;
    gint64 quorum_time = 0;        /* when quorum was first reached (0 = not yet) */
    const gint64 QUORUM_GRACE_US = 2 * G_USEC_PER_SEC; /* 2s grace after quorum */
    guint poll_iterations = 0;
    g_debug("pool_query_thread: polling for events (protocol-driven + EOSE quorum)");

    for (;;) {
        poll_iterations++;
        if (cancellable && g_cancellable_is_cancelled(cancellable)) {
            fprintf(stderr, "[POOL_QUERY] poll: CANCELLED after %lldms (%u iters)\n",
                    (long long)((g_get_monotonic_time() - poll_start) / 1000), poll_iterations);
            break;
        }

        gboolean any_activity = FALSE;
        gboolean all_done = TRUE;

        for (guint i = 0; i < items->len; i++) {
            RelaySubItem *item = g_ptr_array_index(items, i);
            if (!item || !item->sub || item->eosed) continue;

            /* Check if subscription was closed or relay disconnected */
            if (nostr_subscription_is_closed(item->sub) ||
                !nostr_relay_is_connected(item->core_relay)) {
                item->eosed = TRUE; /* treat as done */
                fprintf(stderr, "[POOL_QUERY] poll: relay[%u] %s after %lldms, %u results so far\n",
                        i,
                        nostr_subscription_is_closed(item->sub) ? "CLOSED" : "DISCONNECTED",
                        (long long)((g_get_monotonic_time() - poll_start) / 1000),
                        data->results->len);
                continue;
            }

            all_done = FALSE;

            /* Check for events */
            void *msg = NULL;
            GoChannel *ch_events = nostr_subscription_get_events_channel(item->sub);
            while (ch_events && go_channel_try_receive(ch_events, &msg) == 0) {
                any_activity = TRUE;
                if (msg) {
                    NostrEvent *ev = (NostrEvent *)msg;
                    if (!first_event_time) {
                        first_event_time = g_get_monotonic_time();
                        fprintf(stderr, "[POOL_QUERY] poll: FIRST EVENT after %lldms (kind=%d)\n",
                                (long long)((first_event_time - poll_start) / 1000),
                                ev->kind);
                    }
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
                fprintf(stderr, "[POOL_QUERY] poll: relay[%u] EOSE after %lldms, %u results so far\n",
                        i, (long long)((g_get_monotonic_time() - poll_start) / 1000),
                        data->results->len);
            }
        }

        if (all_done) {
            fprintf(stderr, "[POOL_QUERY] all relays done after %lldms, %u results collected\n",
                    (long long)((g_get_monotonic_time() - poll_start) / 1000), data->results->len);
            break;
        }

        /* EOSE quorum: count how many relays have finished */
        if (!all_done && items->len >= 2) {
            guint n_eosed = 0;
            for (guint i = 0; i < items->len; i++) {
                RelaySubItem *item = g_ptr_array_index(items, i);
                if (item && item->eosed) n_eosed++;
            }
            /* Quorum = strict majority (more than half) */
            if (n_eosed > items->len / 2) {
                if (quorum_time == 0) {
                    quorum_time = g_get_monotonic_time();
                    fprintf(stderr, "[POOL_QUERY] EOSE quorum reached (%u/%u relays) after %lldms — "
                            "grace window %lldms for stragglers\n",
                            n_eosed, items->len,
                            (long long)((quorum_time - poll_start) / 1000),
                            (long long)(QUORUM_GRACE_US / 1000));
                } else if (g_get_monotonic_time() - quorum_time > QUORUM_GRACE_US) {
                    fprintf(stderr, "[POOL_QUERY] quorum grace expired — %u/%u relays done, "
                            "%u results, breaking\n",
                            n_eosed, items->len, data->results->len);
                    break;
                }
            }
        }

        if (!any_activity)
            g_usleep(1000); /* 1ms backoff */
    }
    fprintf(stderr, "[POOL_QUERY] poll loop done, %u results total\n", data->results->len);

    /* nostrc-blk1: Deliver results BEFORE subscription cleanup.
     * nostr_subscription_close/free can block waiting on lifecycle worker
     * wait groups, which would prevent g_task_return_pointer from ever
     * being called — causing 0 results delivered to the UI despite
     * events being received from relays. */

    /* 1. Persist fetched events via the event sink (e.g. nostrdb).
     * Build a copy of the JSON strings since the sink takes ownership. */
    if (data->event_sink_func && data->results->len > 0) {
        GPtrArray *copy = g_ptr_array_new_with_free_func(g_free);
        for (guint i = 0; i < data->results->len; i++) {
            const char *json = g_ptr_array_index(data->results, i);
            if (json)
                g_ptr_array_add(copy, g_strdup(json));
        }
        data->event_sink_func(copy, data->event_sink_data);
    }

    /* 2. Return results to the caller immediately */
    g_debug("Query completed with %u results — returning before cleanup",
            data->results->len);
    fprintf(stderr, "[POOL_QUERY] returning %u results to caller\n", data->results->len);
    g_task_return_pointer(task, data->results,
                          (GDestroyNotify)g_ptr_array_unref);
    data->results = NULL; /* ownership transferred to GTask */

    /* 3. Now do subscription cleanup — may block, but results already delivered */
    for (guint i = 0; i < items->len; i++) {
        RelaySubItem *item = g_ptr_array_index(items, i);
        if (!item) continue;
        if (item->sub) {
            nostr_subscription_close(item->sub, NULL);
            /* nostrc-ws2: Cancel subscription context + wait for lifecycle
             * worker BEFORE free.  Without this, subscription_destroy blocks
             * forever in go_wait_group_wait because the lifecycle thread is
             * stuck in go_channel_receive(done) waiting for context
             * cancellation that never comes — the relay is still alive.
             * This leaks GTask worker threads; after enough thread-view
             * opens/closes the GLib thread pool is exhausted and the entire
             * app appears frozen. */
            nostr_subscription_wait(item->sub);
            nostr_subscription_free(item->sub);
        }
        /* Relay is owned by the pool snapshot (grelay_ref keeps it alive).
         * Do NOT disconnect — connection persists for future queries. */
        g_free(item);
    }
    g_ptr_array_unref(items);
    fprintf(stderr, "[POOL_QUERY] subscription cleanup done\n");
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
    /* nostrc-snap: Snapshot relay list on main thread.
     *
     * CRITICAL FIX: The shared query pool's relay list is mutated by
     * sync_relays() from 29+ call sites across the codebase (NIP-66,
     * follows, threads, profiles, etc.).  Each feature overwrites the
     * list with its own relays before calling query_async().  Without
     * this snapshot, the worker thread reads self->relays (GListStore)
     * AFTER another feature has already replaced the relay list, causing
     * queries to run against the WRONG relays and return empty results.
     *
     * GListStore is also NOT thread-safe — concurrent access from the
     * worker thread and main thread is undefined behavior.
     *
     * By snapshotting here, each query gets its own immutable relay list
     * that cannot be trampled by subsequent sync_relays() calls. */
    data->relay_snapshots = g_ptr_array_new_with_free_func(relay_snapshot_entry_free);
    {
        guint n = g_list_model_get_n_items(G_LIST_MODEL(self->relays));
        for (guint i = 0; i < n; i++) {
            g_autoptr(GNostrRelay) grelay =
                g_list_model_get_item(G_LIST_MODEL(self->relays), i);
            RelaySnapshotEntry *entry = g_new0(RelaySnapshotEntry, 1);
            entry->url = g_strdup(gnostr_relay_get_url(grelay));
            entry->core_relay = gnostr_relay_get_core_relay(grelay);
            entry->grelay_ref = g_object_ref(grelay);
            entry->connected = gnostr_relay_get_connected(grelay);
            g_ptr_array_add(data->relay_snapshots, entry);
        }
        g_debug("pool_query_async: snapshot %u relays for worker thread", n);
    }

    /* Snapshot event sink and cache query for worker thread */
    data->event_sink_func = self->event_sink_func;
    data->event_sink_data = self->event_sink_data;
    data->cache_query_func = self->cache_query_func;
    data->cache_query_data = self->cache_query_data;

    g_task_set_task_data(task, data, (GDestroyNotify)query_async_data_free);

    /* nostrc-uaf3: Task takes ownership of filters so they survive for the
     * entire worker thread lifetime. Previously callers stashed filters on the
     * pool with g_object_set_data_full(pool, "qf", ...), but re-entrant calls
     * on the same pool replaced "qf" and freed the old filters while the worker
     * thread was still using them → heap-use-after-free in nostr_subscription_fire.
     * Now the GTask itself owns the filters via destroy notify. */
    g_object_set_data_full(G_OBJECT(task), "filters", filters,
                           (GDestroyNotify)nostr_filters_free);

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

/* --- Multi-Relay Subscription Implementation --- */

/**
 * Per-relay subscription tracker for multi-relay subscriptions.
 */
typedef struct {
    GNostrRelay        *relay;         /* weak ref */
    GNostrSubscription *subscription;  /* owned */
    gchar              *relay_url;     /* owned */
    gulong              event_handler_id;
    gulong              eose_handler_id;
    gulong              closed_handler_id;
} RelaySubTracker;

struct _GNostrPoolMultiSub {
    GNostrPool *pool;  /* weak ref */
    NostrFilters *filters;  /* owned */
    
    /* User callbacks */
    GNostrPoolMultiSubEventFunc event_func;
    GNostrPoolMultiSubEoseFunc eose_func;
    gpointer user_data;
    GDestroyNotify destroy;
    
    /* Per-relay subscription trackers */
    GPtrArray *relay_subs;  /* array of RelaySubTracker* */
    
    /* Track relay additions/removals */
    gulong relay_added_handler_id;
    gulong relay_state_changed_handler_id;
    
    gboolean closed;
};

static void
relay_sub_tracker_free(RelaySubTracker *tracker)
{
    if (!tracker) return;
    
    if (tracker->subscription) {
        /* Disconnect signal handlers */
        if (tracker->event_handler_id > 0)
            g_signal_handler_disconnect(tracker->subscription, tracker->event_handler_id);
        if (tracker->eose_handler_id > 0)
            g_signal_handler_disconnect(tracker->subscription, tracker->eose_handler_id);
        if (tracker->closed_handler_id > 0)
            g_signal_handler_disconnect(tracker->subscription, tracker->closed_handler_id);
        
        gnostr_subscription_close(tracker->subscription);
        g_object_unref(tracker->subscription);
    }
    
    g_free(tracker->relay_url);
    g_free(tracker);
}

static void
on_multi_sub_event(GNostrSubscription *sub,
                   const gchar        *event_json,
                   gpointer            user_data)
{
    RelaySubTracker *tracker = user_data;
    GNostrPoolMultiSub *multi_sub = g_object_get_data(G_OBJECT(sub), "multi-sub");
    
    if (!multi_sub || multi_sub->closed || !multi_sub->event_func)
        return;
    
    multi_sub->event_func(multi_sub, tracker->relay_url, event_json, multi_sub->user_data);
}

static void
on_multi_sub_eose(GNostrSubscription *sub,
                  gpointer            user_data)
{
    RelaySubTracker *tracker = user_data;
    GNostrPoolMultiSub *multi_sub = g_object_get_data(G_OBJECT(sub), "multi-sub");
    
    if (!multi_sub || multi_sub->closed || !multi_sub->eose_func)
        return;
    
    multi_sub->eose_func(multi_sub, tracker->relay_url, multi_sub->user_data);
}

static void
on_multi_sub_closed(GNostrSubscription *sub,
                    gpointer            user_data)
{
    RelaySubTracker *tracker = user_data;
    g_debug("Multi-sub: relay subscription closed for %s", tracker->relay_url);
}

static gboolean
multi_sub_subscribe_to_relay(GNostrPoolMultiSub *multi_sub, GNostrRelay *relay)
{
    if (multi_sub->closed)
        return FALSE;
    
    if (!gnostr_relay_get_connected(relay))
        return FALSE;
    
    const gchar *url = gnostr_relay_get_url(relay);
    if (!url)
        return FALSE;
    
    /* Check if already subscribed to this relay */
    for (guint i = 0; i < multi_sub->relay_subs->len; i++) {
        RelaySubTracker *existing = g_ptr_array_index(multi_sub->relay_subs, i);
        if (g_strcmp0(existing->relay_url, url) == 0)
            return TRUE;  /* already subscribed */
    }
    
    /* Create subscription for this relay */
    GError *error = NULL;
    GNostrSubscription *sub = gnostr_subscription_new(relay, multi_sub->filters);
    if (!sub) {
        g_warning("Multi-sub: failed to create subscription for %s", url);
        return FALSE;
    }
    
    /* Store multi-sub reference in subscription for callbacks */
    g_object_set_data(G_OBJECT(sub), "multi-sub", multi_sub);
    
    RelaySubTracker *tracker = g_new0(RelaySubTracker, 1);
    tracker->relay = relay;
    tracker->subscription = sub;
    tracker->relay_url = g_strdup(url);
    
    /* Connect signal handlers */
    tracker->event_handler_id = g_signal_connect(sub, "event",
                                                  G_CALLBACK(on_multi_sub_event), tracker);
    tracker->eose_handler_id = g_signal_connect(sub, "eose",
                                                 G_CALLBACK(on_multi_sub_eose), tracker);
    tracker->closed_handler_id = g_signal_connect(sub, "closed",
                                                   G_CALLBACK(on_multi_sub_closed), tracker);
    
    /* Fire the subscription */
    if (!gnostr_subscription_fire(sub, &error)) {
        g_warning("Multi-sub: failed to fire subscription on %s: %s",
                  url, error ? error->message : "unknown error");
        g_clear_error(&error);
        relay_sub_tracker_free(tracker);
        return FALSE;
    }
    
    g_ptr_array_add(multi_sub->relay_subs, tracker);
    g_warning("Multi-sub: subscribed to relay %s (total: %u)", url, multi_sub->relay_subs->len);
    
    return TRUE;
}

static void
on_multi_sub_relay_added(GNostrPool  *pool,
                         GNostrRelay *relay,
                         gpointer     user_data)
{
    GNostrPoolMultiSub *multi_sub = user_data;
    
    if (gnostr_relay_get_connected(relay)) {
        multi_sub_subscribe_to_relay(multi_sub, relay);
    }
}

static void
on_multi_sub_relay_state_changed(GNostrPool       *pool,
                                 GNostrRelay      *relay,
                                 GNostrRelayState  new_state,
                                 gpointer          user_data)
{
    GNostrPoolMultiSub *multi_sub = user_data;
    
    if (new_state == GNOSTR_RELAY_STATE_CONNECTED) {
        multi_sub_subscribe_to_relay(multi_sub, relay);
    }
}

GNostrPoolMultiSub *
gnostr_pool_subscribe_multi(GNostrPool                   *pool,
                            NostrFilters                 *filters,
                            GNostrPoolMultiSubEventFunc   event_func,
                            GNostrPoolMultiSubEoseFunc    eose_func,
                            gpointer                      user_data,
                            GDestroyNotify                destroy,
                            GError                      **error)
{
    g_return_val_if_fail(GNOSTR_IS_POOL(pool), NULL);
    g_return_val_if_fail(filters != NULL, NULL);
    g_return_val_if_fail(event_func != NULL, NULL);
    
    GNostrPoolMultiSub *multi_sub = g_new0(GNostrPoolMultiSub, 1);
    multi_sub->pool = pool;
    
    /* Deep copy filters for multi-sub lifetime */
    multi_sub->filters = nostr_filters_new();
    if (!multi_sub->filters) {
        g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_CONNECTION_FAILED,
                            "failed to allocate filters");
        g_free(multi_sub);
        return NULL;
    }
    
    for (size_t i = 0; i < filters->count; i++) {
        NostrFilter *filter_copy = nostr_filter_copy(&filters->filters[i]);
        if (!filter_copy) {
            g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_CONNECTION_FAILED,
                                "failed to copy filter");
            nostr_filters_free(multi_sub->filters);
            g_free(multi_sub);
            return NULL;
        }
        if (!nostr_filters_add(multi_sub->filters, filter_copy)) {
            nostr_filter_free(filter_copy);
            g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_CONNECTION_FAILED,
                                "failed to add filter");
            nostr_filters_free(multi_sub->filters);
            g_free(multi_sub);
            return NULL;
        }
    }
    
    multi_sub->event_func = event_func;
    multi_sub->eose_func = eose_func;
    multi_sub->user_data = user_data;
    multi_sub->destroy = destroy;
    multi_sub->relay_subs = g_ptr_array_new_with_free_func((GDestroyNotify)relay_sub_tracker_free);
    multi_sub->closed = FALSE;
    
    /* Subscribe to all currently connected relays */
    guint n = g_list_model_get_n_items(G_LIST_MODEL(pool->relays));
    guint subscribed_count = 0;
    
    for (guint i = 0; i < n; i++) {
        g_autoptr(GNostrRelay) relay = g_list_model_get_item(G_LIST_MODEL(pool->relays), i);
        if (multi_sub_subscribe_to_relay(multi_sub, relay)) {
            subscribed_count++;
        }
    }
    
    if (subscribed_count == 0) {
        g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_CONNECTION_FAILED,
                            "no connected relays available for multi-subscription");
        gnostr_pool_multi_sub_close(multi_sub);
        return NULL;
    }
    
    /* Watch for new relay connections */
    multi_sub->relay_added_handler_id =
        g_signal_connect(pool, "relay-added",
                        G_CALLBACK(on_multi_sub_relay_added), multi_sub);
    multi_sub->relay_state_changed_handler_id =
        g_signal_connect(pool, "relay-state-changed",
                        G_CALLBACK(on_multi_sub_relay_state_changed), multi_sub);
    
    g_debug("Multi-sub: created with %u/%u relay subscriptions", subscribed_count, n);
    
    return multi_sub;
}

void
gnostr_pool_multi_sub_close(GNostrPoolMultiSub *multi_sub)
{
    if (!multi_sub)
        return;
    
    if (multi_sub->closed)
        return;
    
    multi_sub->closed = TRUE;
    
    /* Disconnect pool signal handlers */
    if (multi_sub->pool) {
        if (multi_sub->relay_added_handler_id > 0)
            g_signal_handler_disconnect(multi_sub->pool, multi_sub->relay_added_handler_id);
        if (multi_sub->relay_state_changed_handler_id > 0)
            g_signal_handler_disconnect(multi_sub->pool, multi_sub->relay_state_changed_handler_id);
    }
    
    /* Close all relay subscriptions */
    if (multi_sub->relay_subs) {
        g_debug("Multi-sub: closing %u relay subscriptions", multi_sub->relay_subs->len);
        g_ptr_array_unref(multi_sub->relay_subs);
        multi_sub->relay_subs = NULL;
    }
    
    /* Clean up user data */
    if (multi_sub->destroy && multi_sub->user_data) {
        multi_sub->destroy(multi_sub->user_data);
        multi_sub->user_data = NULL;
    }
    
    /* Free filters */
    if (multi_sub->filters) {
        nostr_filters_free(multi_sub->filters);
        multi_sub->filters = NULL;
    }
    
    g_free(multi_sub);
}

guint
gnostr_pool_multi_sub_get_relay_count(GNostrPoolMultiSub *multi_sub)
{
    g_return_val_if_fail(multi_sub != NULL, 0);
    g_return_val_if_fail(!multi_sub->closed, 0);
    
    return multi_sub->relay_subs ? multi_sub->relay_subs->len : 0;
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

/* --- Event Sink API --- */

void
gnostr_pool_set_event_sink(GNostrPool              *self,
                            GNostrPoolEventSinkFunc  sink_func,
                            gpointer                 user_data,
                            GDestroyNotify           destroy)
{
    g_return_if_fail(GNOSTR_IS_POOL(self));

    if (self->event_sink_destroy && self->event_sink_data) {
        self->event_sink_destroy(self->event_sink_data);
    }

    self->event_sink_func = sink_func;
    self->event_sink_data = user_data;
    self->event_sink_destroy = destroy;
}

/* --- Cache Query API --- */

void
gnostr_pool_set_cache_query(GNostrPool                *self,
                             GNostrPoolCacheQueryFunc   query_func,
                             gpointer                  user_data,
                             GDestroyNotify             destroy)
{
    g_return_if_fail(GNOSTR_IS_POOL(self));

    if (self->cache_query_destroy && self->cache_query_data) {
        self->cache_query_destroy(self->cache_query_data);
    }

    self->cache_query_func = query_func;
    self->cache_query_data = user_data;
    self->cache_query_destroy = destroy;
}
