#include "nostr_relay_store.h"
#include "nostr-event.h"
#include "nostr-filter.h"
#include <glib.h>
#include <gio/gio.h>

/* GNostrRelayStore interface implementation */
G_DEFINE_INTERFACE(GNostrRelayStore, gnostr_relay_store, G_TYPE_OBJECT)

static void gnostr_relay_store_default_init(GNostrRelayStoreInterface *iface) {
    (void)iface;
}

gboolean
gnostr_relay_store_publish(GNostrRelayStore *self, NostrEvent *event, GError **error)
{
    g_return_val_if_fail(GNOSTR_IS_RELAY_STORE(self), FALSE);
    GNostrRelayStoreInterface *iface = GNOSTR_RELAY_STORE_GET_IFACE(self);
    if (!iface->publish) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                            "relay store does not implement publish");
        return FALSE;
    }
    return iface->publish(self, event, error);
}

gboolean
gnostr_relay_store_query_sync(GNostrRelayStore *self, NostrFilter *filter,
                              GPtrArray **events, GError **error)
{
    g_return_val_if_fail(GNOSTR_IS_RELAY_STORE(self), FALSE);
    GNostrRelayStoreInterface *iface = GNOSTR_RELAY_STORE_GET_IFACE(self);
    if (!iface->query_sync) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                            "relay store does not implement query_sync");
        return FALSE;
    }
    return iface->query_sync(self, filter, events, error);
}

typedef struct {
    NostrFilter *filter;
} QueryTaskData;

static void
query_task_data_free(QueryTaskData *data)
{
    if (!data) return;
    if (data->filter) nostr_filter_free(data->filter);
    g_free(data);
}

static void
query_task_thread(GTask *task, gpointer source_object, gpointer task_data,
                  GCancellable *cancellable)
{
    (void)cancellable;
    GNostrRelayStore *self = GNOSTR_RELAY_STORE(source_object);
    QueryTaskData *data = task_data;
    GPtrArray *events = NULL;
    g_autoptr(GError) error = NULL;

    if (!gnostr_relay_store_query_sync(self, data->filter, &events, &error)) {
        g_task_return_error(task, g_steal_pointer(&error));
        return;
    }

    g_task_return_pointer(task, events, events ? (GDestroyNotify)g_ptr_array_unref : NULL);
}

static void
default_query_async(GNostrRelayStore *self, NostrFilter *filter,
                    GCancellable *cancellable,
                    GAsyncReadyCallback callback,
                    gpointer user_data)
{
    GTask *task = g_task_new(self, cancellable, callback, user_data);
    QueryTaskData *data = g_new0(QueryTaskData, 1);
    data->filter = filter ? nostr_filter_copy(filter) : NULL;
    g_task_set_task_data(task, data, (GDestroyNotify)query_task_data_free);
    g_task_run_in_thread(task, query_task_thread);
    g_object_unref(task);
}

void
gnostr_relay_store_query_async(GNostrRelayStore *self, NostrFilter *filter,
                               GCancellable *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer user_data)
{
    g_return_if_fail(GNOSTR_IS_RELAY_STORE(self));
    GNostrRelayStoreInterface *iface = GNOSTR_RELAY_STORE_GET_IFACE(self);
    if (iface->query_async)
        iface->query_async(self, filter, cancellable, callback, user_data);
    else
        default_query_async(self, filter, cancellable, callback, user_data);
}

GPtrArray *
gnostr_relay_store_query_finish(GNostrRelayStore *self, GAsyncResult *result, GError **error)
{
    g_return_val_if_fail(GNOSTR_IS_RELAY_STORE(self), NULL);
    GNostrRelayStoreInterface *iface = GNOSTR_RELAY_STORE_GET_IFACE(self);
    if (iface->query_finish)
        return iface->query_finish(self, result, error);
    g_return_val_if_fail(g_task_is_valid(result, self), NULL);
    return g_task_propagate_pointer(G_TASK(result), error);
}

/* GNostrMultiStore GObject implementation */
static void gnostr_multi_store_relay_store_iface_init(GNostrRelayStoreInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GNostrMultiStore, gnostr_multi_store, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_RELAY_STORE,
                                              gnostr_multi_store_relay_store_iface_init))

static void gnostr_multi_store_finalize(GObject *object) {
    GNostrMultiStore *self = GNOSTR_MULTI_STORE(object);
    g_clear_pointer(&self->stores, g_ptr_array_unref);
    G_OBJECT_CLASS(gnostr_multi_store_parent_class)->finalize(object);
}

static void gnostr_multi_store_class_init(GNostrMultiStoreClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = gnostr_multi_store_finalize;
}

static void gnostr_multi_store_init(GNostrMultiStore *self) {
    self->stores = g_ptr_array_new_with_free_func(g_object_unref);
}

GNostrMultiStore *gnostr_multi_store_new(void) {
    return g_object_new(GNOSTR_TYPE_MULTI_STORE, NULL);
}

void gnostr_multi_store_add_store(GNostrMultiStore *self, GNostrRelayStore *store) {
    g_return_if_fail(GNOSTR_IS_MULTI_STORE(self));
    g_return_if_fail(GNOSTR_IS_RELAY_STORE(store));
    g_ptr_array_add(self->stores, g_object_ref(store));
}

static gboolean
gnostr_multi_store_publish_iface(GNostrRelayStore *store, NostrEvent *event, GError **error)
{
    GNostrMultiStore *self = GNOSTR_MULTI_STORE(store);
    gboolean any_success = FALSE;
    g_autoptr(GError) last_error = NULL;

    for (guint i = 0; i < self->stores->len; i++) {
        GNostrRelayStore *child = g_ptr_array_index(self->stores, i);
        g_autoptr(GError) child_error = NULL;
        if (gnostr_relay_store_publish(child, event, &child_error)) {
            any_success = TRUE;
        } else if (child_error) {
            g_clear_error(&last_error);
            last_error = g_steal_pointer(&child_error);
        }
    }

    if (!any_success && last_error) {
        g_propagate_error(error, g_steal_pointer(&last_error));
        return FALSE;
    }
    if (!any_success && self->stores->len == 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                            "multi-store has no child stores");
        return FALSE;
    }
    return any_success;
}

static gboolean
gnostr_multi_store_query_sync_iface(GNostrRelayStore *store, NostrFilter *filter,
                                    GPtrArray **events, GError **error)
{
    GNostrMultiStore *self = GNOSTR_MULTI_STORE(store);
    GPtrArray *combined = g_ptr_array_new();
    gboolean any_success = FALSE;
    g_autoptr(GError) last_error = NULL;

    for (guint i = 0; i < self->stores->len; i++) {
        GNostrRelayStore *child = g_ptr_array_index(self->stores, i);
        GPtrArray *child_events = NULL;
        g_autoptr(GError) child_error = NULL;
        if (!gnostr_relay_store_query_sync(child, filter, &child_events, &child_error)) {
            if (child_error) {
                g_clear_error(&last_error);
                last_error = g_steal_pointer(&child_error);
            }
            continue;
        }
        any_success = TRUE;
        if (child_events) {
            gsize child_len = 0;
            gpointer *child_items = g_ptr_array_steal(child_events, &child_len);
            for (gsize j = 0; j < child_len; j++)
                g_ptr_array_add(combined, child_items[j]);
            g_free(child_items);
            g_ptr_array_unref(child_events);
        }
    }

    if (!any_success && last_error) {
        g_ptr_array_unref(combined);
        g_propagate_error(error, g_steal_pointer(&last_error));
        return FALSE;
    }

    if (events)
        *events = combined;
    else
        g_ptr_array_unref(combined);
    return TRUE;
}

static void
gnostr_multi_store_relay_store_iface_init(GNostrRelayStoreInterface *iface)
{
    iface->publish = gnostr_multi_store_publish_iface;
    iface->query_sync = gnostr_multi_store_query_sync_iface;
}
