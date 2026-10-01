#include "gh-mls-attachment-ui.h"

static GhMlsGroup *
group_of(GhMlsAttachments *files, GhConversation *conversation)
{
  GhMlsService *service = gh_mls_attachments_get_service(files);
  return service && conversation
           ? gh_mls_service_lookup(service, gh_conversation_get_room_id(conversation)) : NULL;
}

static gboolean
can_send(GhConversation *conversation, gpointer data)
{
  GhMlsGroup *group = group_of(data, conversation);
  /* Leaving (nostrc-2um6): nothing but the leave is sent any more. */
  return group && gh_mls_group_get_active(group) && !gh_mls_group_get_leaving(group);
}

static void
send_async(GhConversation *conversation, GBytes *file, const gchar *name, const gchar *mime,
           GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data,
           gpointer data)
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
  gh_mls_attachments_send_async(files, group, file, name, mime, NULL, cancellable, callback,
                                user_data);
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

static const GhAttachmentUiGroups groups = {
  .can_send = can_send,
  .send_async = send_async,
  .send_finish = send_finish,
  .describe = describe,
  .lookup = lookup,
  .download = download,
  .cancel = cancel,
  .download_note = download_note,
};

void
gh_mls_attachment_ui_attach(GhWindow *window, GhMlsAttachments *files)
{
  g_return_if_fail(GH_IS_WINDOW(window));
  g_return_if_fail(GH_IS_MLS_ATTACHMENTS(files));
  gh_attachment_ui_set_groups(window, &groups, g_object_ref(files), g_object_unref);
}
