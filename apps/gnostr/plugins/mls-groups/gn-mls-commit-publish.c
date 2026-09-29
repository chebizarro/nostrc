/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-mls-commit-publish.c - Publish a Commit until a relay accepts it
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "gn-mls-commit-publish.h"

typedef struct {
  GnMlsAckPublishFunc publish;
  GnMlsAckFinishFunc  finish;
  gpointer            target;
  gchar              *event_json;
  GStrv               relays;
  guint               next;
  GError             *last_error;
} PublishState;

static void
publish_state_free(gpointer data)
{
  PublishState *st = data;
  g_free(st->event_json);
  g_strfreev(st->relays);
  g_clear_error(&st->last_error);
  g_free(st);
}

static void try_next_relay(GTask *task);

static void
on_relay_answer(GObject *source, GAsyncResult *result, gpointer user_data)
{
  (void)source;
  GTask *task = user_data;
  PublishState *st = g_task_get_task_data(task);
  GError *error = NULL;
  if (st->finish(st->target, result, &error))
    {
      g_task_return_pointer(task, g_strdup(st->relays[st->next - 1]), g_free);
      g_object_unref(task);
      return;
    }
  g_clear_error(&st->last_error);
  st->last_error = error;
  try_next_relay(task);
}

static void
try_next_relay(GTask *task)
{
  PublishState *st = g_task_get_task_data(task);
  if (st->relays == NULL || st->relays[st->next] == NULL)
    {
      if (st->last_error != NULL)
        g_task_return_error(task, g_steal_pointer(&st->last_error));
      else
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                "The group has no relays to publish to");
      g_object_unref(task);
      return;
    }
  const char *relay = st->relays[st->next++];
  st->publish(st->target, st->event_json, relay, g_task_get_cancellable(task),
              on_relay_answer, task);
}

void
gn_mls_publish_until_ack_async(GnMlsAckPublishFunc  publish,
                               GnMlsAckFinishFunc   finish,
                               gpointer             target,
                               const char          *event_json,
                               const char * const  *relay_urls,
                               GCancellable        *cancellable,
                               GAsyncReadyCallback  callback,
                               gpointer             user_data)
{
  g_return_if_fail(publish != NULL && finish != NULL && event_json != NULL);
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  PublishState *st = g_new0(PublishState, 1);
  st->publish = publish;
  st->finish = finish;
  st->target = target;
  st->event_json = g_strdup(event_json);
  st->relays = g_strdupv((gchar **)relay_urls);
  g_task_set_task_data(task, st, publish_state_free);
  try_next_relay(task);   /* the task holds its own reference until done */
}

gchar *
gn_mls_publish_until_ack_finish(GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, NULL), NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}
