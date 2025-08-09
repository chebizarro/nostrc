#include "nostr_async.h"
#include <gio/gio.h>

typedef struct {
    GTask *task;
    NostrRelay *relay;
    NostrEvent *event;
    NostrFilter *filter;
} AsyncData;

static void nostr_relay_connect_async_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    NostrRelay *self = NOSTR_RELAY(source_object);
    AsyncData *data = (AsyncData *)task_data;
    GError *error = NULL;

    if (gnostr_relay_connect(self, &error)) {
        g_task_return_boolean(task, TRUE);
    } else {
        g_task_return_error(task, error);
    }

    g_free(data);
}

void nostr_relay_connect_async(NostrRelay *self, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data) {
    GTask *task = g_task_new(self, cancellable, callback, user_data);
    AsyncData *data = g_new0(AsyncData, 1);
    data->task = task;
    g_task_set_task_data(task, data, g_free);
    g_task_run_in_thread(task, nostr_relay_connect_async_thread);
}

gboolean nostr_relay_connect_finish(NostrRelay *self, GAsyncResult *result, GError **error) {
    return g_task_propagate_boolean(G_TASK(result), error);
}

static void nostr_relay_publish_async_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    NostrRelay *self = NOSTR_RELAY(source_object);
    AsyncData *data = (AsyncData *)task_data;
    GError *error = NULL;

    if (gnostr_relay_publish(self, data->event, &error)) {
        g_task_return_boolean(task, TRUE);
    } else {
        g_task_return_error(task, error);
    }

    g_free(data);
}

void nostr_relay_publish_async(NostrRelay *self, NostrEvent *event, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data) {
    GTask *task = g_task_new(self, cancellable, callback, user_data);
    AsyncData *data = g_new0(AsyncData, 1);
    data->task = task;
    data->event = event;
    g_task_set_task_data(task, data, g_free);
    g_task_run_in_thread(task, nostr_relay_publish_async_thread);
}

gboolean nostr_relay_publish_finish(NostrRelay *self, GAsyncResult *result, GError **error) {
    return g_task_propagate_boolean(G_TASK(result), error);
}

static void nostr_relay_query_sync_async_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    NostrRelay *self = NOSTR_RELAY(source_object);
    AsyncData *data = (AsyncData *)task_data;
    GError *error = NULL;

    GPtrArray *events = gnostr_relay_query_sync(self, data->filter, &error);
    if (events) {
        g_task_return_pointer(task, events, (GDestroyNotify)g_ptr_array_unref);
    } else {
        g_task_return_error(task, error);
    }

    g_free(data);
}

void nostr_relay_query_sync_async(NostrRelay *self, NostrFilter *filter, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data) {
    GTask *task = g_task_new(self, cancellable, callback, user_data);
    AsyncData *data = g_new0(AsyncData, 1);
    data->task = task;
    data->filter = filter;
    g_task_set_task_data(task, data, g_free);
    g_task_run_in_thread(task, nostr_relay_query_sync_async_thread);
}

GPtrArray *nostr_relay_query_sync_finish(NostrRelay *self, GAsyncResult *result, GError **error) {
    return g_task_propagate_pointer(G_TASK(result), error);
}