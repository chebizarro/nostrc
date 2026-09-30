#include "gh-relay-gnostr-write.h"

#include <nostr-event.h>
#include <nostr-relay.h>
#include <stdlib.h>

/* libgo headers, quoted so glibc's <error.h> cannot shadow libgo's. */
#include "channel.h"
#include "error.h"
#include "select.h"

#define GH_RELAY_WRITE_CONFIRM_MS 5000

gchar *
gh_relay_gnostr_event_frame(const gchar *type, const gchar *event_json)
{
  g_return_val_if_fail(type != NULL, NULL);
  NostrEvent *event = event_json ? nostr_event_new() : NULL;
  if (!event)
    return NULL;
  gchar *frame = NULL;
  if (nostr_event_deserialize_signed(event, event_json, NULL) == NOSTR_EVENT_VALIDATION_OK) {
    char *json = nostr_event_serialize_compact(event);
    if (json)
      frame = g_strdup_printf("[\"%s\",%s]", type, json);
    free(json);
  }
  nostr_event_free(event);
  return frame;
}

static void
write_thread(GTask *task, gpointer source, gpointer task_data,
             GCancellable *cancellable)
{
  const gchar *frame = task_data;
  NostrRelay *core = gnostr_relay_get_core_relay(GNOSTR_RELAY(source));
  if (g_task_return_error_if_cancelled(task))
    return;
  (void)cancellable;
  if (!core || !nostr_relay_is_established(core)) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
                            "relay connection not established");
    return;
  }
  /* nostr_relay_write() copies the frame; its answer channel carries NULL
   * on success or an Error* once the socket write has been attempted. */
  GoChannel *answer = nostr_relay_write(core, (char *)frame);
  if (!answer) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "relay write could not be queued");
    return;
  }
  Error *write_error = NULL;
  GoSelectCase cases[] = {
    { .op = GO_SELECT_RECEIVE, .chan = answer, .recv_buf = (void **)&write_error },
  };
  GoSelectResult result = go_select_timeout(cases, 1, GH_RELAY_WRITE_CONFIRM_MS);
  /* Closing signals disinterest; the writer holds the other reference. An
   * answer it sent after the timeout, before the close, stays in the channel,
   * whose last unref frees no items: free it here (nostrc-xbso). */
  go_channel_close(answer);
  void *late = NULL;
  while (go_channel_try_receive(answer, &late) == 0) {
    if (late)
      free_error(late);
    late = NULL;
  }
  go_channel_unref(answer);
  if (result.selected_case < 0) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                            "relay write not confirmed within %u ms",
                            GH_RELAY_WRITE_CONFIRM_MS);
  } else if (write_error) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE,
                            "relay write failed: %s",
                            write_error->message ? write_error->message : "unknown");
    free_error(write_error);
  } else if (!result.ok) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE,
                            "relay write abandoned: connection closed");
  } else {
    g_task_return_boolean(task, TRUE);
  }
}

void
gh_relay_gnostr_write_async(GNostrRelay *relay, const gchar *frame,
                            GCancellable *cancellable,
                            GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(GNOSTR_IS_RELAY(relay) && frame != NULL);
  GTask *task = g_task_new(relay, cancellable, callback, user_data);
  g_task_set_source_tag(task, gh_relay_gnostr_write_async);
  g_task_set_task_data(task, g_strdup(frame), g_free);
  g_task_run_in_thread(task, write_thread);
  g_object_unref(task);
}

gboolean
gh_relay_gnostr_write_finish(GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(G_IS_TASK(result), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}
