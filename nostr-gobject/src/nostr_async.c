/**
 * SPDX-License-Identifier: MIT
 *
 * Deprecated async wrapper functions for relay operations.
 * These delegate to the new gnostr_relay_*_async() functions.
 *
 * New code should use gnostr_relay_connect_async() and related
 * functions directly from nostr_relay.h.
 */

#include "nostr_async.h"
#include <gio/gio.h>

/* Core libnostr headers for NostrEvent/NostrFilter types */
#include "nostr-event.h"
#include "nostr-filter.h"

/* Async data for legacy API wrappers */
typedef struct {
    GTask *task;
    GNostrRelay *relay;
    NostrEvent *event;
    NostrFilter *filter;
} AsyncData;

static void
async_data_free(AsyncData *data)
{
    if (data) {
        /* Don't unref relay - it's the source_object owned by GTask */
        g_free(data);
    }
}

/* Connect async thread function */
static void
nostr_relay_connect_async_thread(GTask        *task,
                                  gpointer      source_object,
                                  gpointer      task_data,
                                  GCancellable *cancellable)
{
    GNostrRelay *self = GNOSTR_RELAY(source_object);
    GError *error = NULL;

    if (gnostr_relay_connect(self, &error)) {
        g_task_return_boolean(task, TRUE);
    } else {
        g_task_return_error(task, error);
    }
}

void
nostr_relay_connect_async(GNostrRelay         *self,
                          GCancellable        *cancellable,
                          GAsyncReadyCallback  callback,
                          gpointer             user_data)
{
    g_return_if_fail(GNOSTR_IS_RELAY(self));

    GTask *task = g_task_new(self, cancellable, callback, user_data);

    AsyncData *data = g_new0(AsyncData, 1);
    data->task = task;
    g_task_set_task_data(task, data, (GDestroyNotify)async_data_free);

    g_task_run_in_thread(task, nostr_relay_connect_async_thread);
    g_object_unref(task);
}

gboolean
nostr_relay_connect_finish(GNostrRelay   *self,
                           GAsyncResult  *result,
                           GError       **error)
{
    g_return_val_if_fail(GNOSTR_IS_RELAY(self), FALSE);
    g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

    return g_task_propagate_boolean(G_TASK(result), error);
}

/* Publish async thread function */
static void
nostr_relay_publish_async_thread(GTask        *task,
                                  gpointer      source_object,
                                  gpointer      task_data,
                                  GCancellable *cancellable)
{
    GNostrRelay *self = GNOSTR_RELAY(source_object);
    AsyncData *data = (AsyncData *)task_data;
    GError *error = NULL;

    if (gnostr_relay_publish(self, data->event, &error)) {
        g_task_return_boolean(task, TRUE);
    } else {
        g_task_return_error(task, error);
    }
}

void
nostr_relay_publish_async(GNostrRelay         *self,
                          NostrEvent          *event,
                          GCancellable        *cancellable,
                          GAsyncReadyCallback  callback,
                          gpointer             user_data)
{
    g_return_if_fail(GNOSTR_IS_RELAY(self));

    GTask *task = g_task_new(self, cancellable, callback, user_data);

    AsyncData *data = g_new0(AsyncData, 1);
    data->task = task;
    data->event = event;
    g_task_set_task_data(task, data, (GDestroyNotify)async_data_free);

    g_task_run_in_thread(task, nostr_relay_publish_async_thread);
    g_object_unref(task);
}

gboolean
nostr_relay_publish_finish(GNostrRelay   *self,
                           GAsyncResult  *result,
                           GError       **error)
{
    g_return_val_if_fail(GNOSTR_IS_RELAY(self), FALSE);
    g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

    return g_task_propagate_boolean(G_TASK(result), error);
}

/* Query sync async thread function */
static void
nostr_relay_query_sync_async_thread(GTask        *task,
                                     gpointer      source_object,
                                     gpointer      task_data,
                                     GCancellable *cancellable)
{
    GNostrRelay *self = GNOSTR_RELAY(source_object);
    AsyncData *data = (AsyncData *)task_data;
    GError *error = NULL;

    GPtrArray *events = gnostr_relay_query_sync(self, data->filter, &error);
    if (events) {
        g_task_return_pointer(task, events, (GDestroyNotify)g_ptr_array_unref);
    } else {
        g_task_return_error(task, error);
    }
}

void
nostr_relay_query_sync_async(GNostrRelay         *self,
                             NostrFilter         *filter,
                             GCancellable        *cancellable,
                             GAsyncReadyCallback  callback,
                             gpointer             user_data)
{
    g_return_if_fail(GNOSTR_IS_RELAY(self));

    GTask *task = g_task_new(self, cancellable, callback, user_data);

    AsyncData *data = g_new0(AsyncData, 1);
    data->task = task;
    data->filter = filter;
    g_task_set_task_data(task, data, (GDestroyNotify)async_data_free);

    g_task_run_in_thread(task, nostr_relay_query_sync_async_thread);
    g_object_unref(task);
}

GPtrArray *
nostr_relay_query_sync_finish(GNostrRelay   *self,
                              GAsyncResult  *result,
                              GError       **error)
{
    g_return_val_if_fail(GNOSTR_IS_RELAY(self), NULL);
    g_return_val_if_fail(g_task_is_valid(result, self), NULL);

    return g_task_propagate_pointer(G_TASK(result), error);
}
