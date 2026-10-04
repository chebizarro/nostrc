#include "gh-mls-attachment-ui.h"

#include "gh-mls-copy.h"

static GhMlsGroup *
group_of(GhMlsAttachments *files, GhConversation *conversation)
{
  GhMlsService *service = gh_mls_attachments_get_service(files);
  return service && conversation
           ? gh_mls_service_lookup(service, gh_conversation_get_room_id(conversation)) : NULL;
}

/* The composer's own rule (gh_mls_send_reason(): ended, leaving, or not
 * read, i.e. offline; W25 review N1): files go where text can. */
static gboolean
can_send(GhConversation *conversation, gpointer data)
{
  GhMlsGroup *group = group_of(data, conversation);
  g_autofree gchar *reason = group ? gh_mls_send_reason(gh_mls_attachments_get_service(data),
                                                        group, NULL)
                                   : NULL;
  return group && !reason;
}

static void
send_async(GhConversation *conversation, GBytes *file, const gchar *name, const gchar *mime,
           const gchar *const *servers, GCancellable *cancellable,
           GAsyncReadyCallback callback, gpointer user_data, gpointer data)
{
  GhMlsAttachments *files = data;
  GhMlsGroup *group = group_of(files, conversation);
  if (!group) {
    GTask *task = g_task_new(files, cancellable, callback, user_data);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                            "The group isn't available any more");
    g_object_unref(task);
    return;
  }
  gh_mls_attachments_send_on_servers_async(files, group, file, name, mime, NULL, servers,
                                           cancellable, callback, user_data);
}

static gboolean
send_finish(GAsyncResult *result, gchar **out_server, GError **error, gpointer data)
{
  g_autoptr(GhMessage) message = gh_mls_attachments_send_finish(data, result, out_server, error);
  return message != NULL;
}

static gchar *
describe(const GError *error, const gchar *host, gpointer data)
{
  return gh_mls_attachments_describe(data, error, GH_ATTACHMENTS_UPLOAD, host, NULL);
}

static GhAttachmentTransfer *
lookup(GhMessage *message, guint index, gpointer data)
{
  return gh_mls_attachments_lookup(data, message, index);
}

static void
download(GhAttachmentTransfer *transfer, gpointer data)
{
  gh_mls_attachments_download(data, transfer);
}

static void
cancel(GhAttachmentTransfer *transfer, gpointer data)
{
  gh_mls_attachments_cancel(data, transfer);
}

static gchar *
download_note(GhAttachmentTransfer *transfer, gpointer data)
{
  return gh_mls_attachments_download_note(data, transfer);
}

/* The shown group: the attach button follows it as it changes (left,
 * removed, leaving, offline), not only when another conversation is shown.
 * The attachment UI connects to it for the window (W25 re-review R2). */
static GObject *
watch(GhConversation *conversation, gpointer data)
{
  return G_OBJECT(group_of(data, conversation));
}

static const GhAttachmentUiGroups groups = {
  .can_send = can_send,
  .send_async = send_async,
  .send_finish = send_finish,
  .describe = describe,
  .lookup = lookup,
  .download = download,
  .cancel = cancel,
  .download_note = download_note,
  .watch = watch,
};

void
gh_mls_attachment_ui_attach(GhWindow *window, GhMlsAttachments *files)
{
  g_return_if_fail(GH_IS_WINDOW(window));
  g_return_if_fail(GH_IS_MLS_ATTACHMENTS(files));
  gh_attachment_ui_set_groups(window, &groups, g_object_ref(files), g_object_unref);
}
