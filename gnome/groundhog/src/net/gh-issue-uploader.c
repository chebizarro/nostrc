/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gh-issue-uploader.h"
#include "gh-metadata-strip.h"
#include <glib/gi18n.h>

struct _GhIssueUploader {
  GObject parent_instance;
  GhBlossomClient *client;
  GStrv servers;
  gchar *destination;
};

static void uploader_iface_init(GnIssueUploaderInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE(GhIssueUploader, gh_issue_uploader, G_TYPE_OBJECT,
  G_IMPLEMENT_INTERFACE(GN_TYPE_ISSUE_UPLOADER, uploader_iface_init))

static const gchar *
describe_destination(GnIssueUploader *uploader)
{
  return GH_ISSUE_UPLOADER(uploader)->destination;
}

static void
on_uploaded(GObject *source, GAsyncResult *result, gpointer data)
{
  GTask *task = data;
  GError *error = NULL;
  gchar *url = gh_blossom_client_upload_finish(GH_BLOSSOM_CLIENT(source), result, NULL, &error);
  if (url)
    g_task_return_pointer(task, url, g_free);
  else
    g_task_return_error(task, error);
  g_object_unref(task);
}

static void
on_loaded(GObject *source, GAsyncResult *result, gpointer data)
{
  GTask *task = data;
  GhIssueUploader *self = g_task_get_source_object(task);
  GError *error = NULL;
  g_autoptr(GBytes) bytes = g_file_load_bytes_finish(G_FILE(source), result, NULL, &error);
  g_autoptr(GBytes) stripped = NULL;
  gsize cap = gh_blossom_client_get_max_file_size(self->client);
  if (bytes && g_bytes_get_size(bytes) == 0)
    error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_INVALID_DATA, _("The file is empty."));
  else if (bytes && g_bytes_get_size(bytes) > cap)
    error = g_error_new(GH_BLOSSOM_ERROR, GH_BLOSSOM_ERROR_TOO_LARGE,
                        _("Files can be at most %" G_GSIZE_FORMAT " MB."), cap / (1024 * 1024));
  else if (bytes)
    stripped = gh_metadata_strip(bytes, NULL, NULL, &error);
  if (!stripped) {
    g_task_return_error(task, error);
    g_object_unref(task);
    return;
  }
  g_autofree gchar *sha256 = g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, stripped);
  gh_blossom_client_upload_public_async(self->client, (const gchar *const *)self->servers,
                                        stripped, sha256, g_task_get_cancellable(task),
                                        on_uploaded, task);
}

static void
on_info(GObject *source, GAsyncResult *result, gpointer data)
{
  GTask *task = data;
  GhIssueUploader *self = g_task_get_source_object(task);
  GError *error = NULL;
  g_autoptr(GFileInfo) info = g_file_query_info_finish(G_FILE(source), result, &error);
  if (info && g_file_info_get_file_type(info) != G_FILE_TYPE_REGULAR)
    error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_REGULAR_FILE,
                                _("Only regular files can be attached."));
  else if (info && (guint64)g_file_info_get_size(info) >
                       gh_blossom_client_get_max_file_size(self->client))
    error = g_error_new(GH_BLOSSOM_ERROR, GH_BLOSSOM_ERROR_TOO_LARGE,
                        _("Files can be at most %" G_GSIZE_FORMAT " MB."),
                        gh_blossom_client_get_max_file_size(self->client) / (1024 * 1024));
  if (error) {
    g_task_return_error(task, error);
    g_object_unref(task);
    return;
  }
  /* The size is checked again once read: the file may have grown. */
  g_file_load_bytes_async(G_FILE(source), g_task_get_cancellable(task), on_loaded, task);
}

static void
upload_async(GnIssueUploader *uploader, GFile *file, const gchar *content_type,
             GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data)
{
  (void)content_type; /* the format comes from the bytes (gh_media_sniff()) */
  GTask *task = g_task_new(uploader, cancellable, callback, data);
  g_task_set_source_tag(task, upload_async);
  g_file_query_info_async(file, G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_SIZE,
                          G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT, cancellable, on_info, task);
}

static gchar *
upload_finish(GnIssueUploader *uploader, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, uploader), NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

static void
uploader_iface_init(GnIssueUploaderInterface *iface)
{
  iface->describe_destination = describe_destination;
  iface->upload_async = upload_async;
  iface->upload_finish = upload_finish;
}

static void
gh_issue_uploader_dispose(GObject *object)
{
  g_clear_object(&GH_ISSUE_UPLOADER(object)->client);
  G_OBJECT_CLASS(gh_issue_uploader_parent_class)->dispose(object);
}

static void
gh_issue_uploader_finalize(GObject *object)
{
  GhIssueUploader *self = GH_ISSUE_UPLOADER(object);
  g_strfreev(self->servers);
  g_free(self->destination);
  G_OBJECT_CLASS(gh_issue_uploader_parent_class)->finalize(object);
}

static void
gh_issue_uploader_class_init(GhIssueUploaderClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = gh_issue_uploader_dispose;
  G_OBJECT_CLASS(klass)->finalize = gh_issue_uploader_finalize;
}

static void gh_issue_uploader_init(GhIssueUploader *self) { (void)self; }

GnIssueUploader *
gh_issue_uploader_new(GhBlossomClient *client)
{
  if (!client)
    return NULL;
  g_return_val_if_fail(GH_IS_BLOSSOM_CLIENT(client), NULL);
  g_auto(GStrv) configured = gh_blossom_client_dup_servers(client);
  g_auto(GStrv) usable = gh_blossom_client_dup_public_servers(client,
                                                              (const gchar *const *)configured);
  if (!usable || !usable[0])
    return NULL;
  /* Shown in the consent text in the form they are contacted in. */
  GPtrArray *normalized = g_ptr_array_new_null_terminated(0, NULL, TRUE);
  for (guint i = 0; usable[i]; i++) {
    gchar *server = gh_blossom_client_normalize_server(usable[i], NULL);
    gboolean seen = FALSE;
    for (guint j = 0; server && j < normalized->len; j++)
      seen = seen || g_str_equal(g_ptr_array_index(normalized, j), server);
    if (server && !seen)
      g_ptr_array_add(normalized, server);
    else
      g_free(server);
  }
  GhIssueUploader *self = g_object_new(GH_TYPE_ISSUE_UPLOADER, NULL);
  self->client = g_object_ref(client);
  self->servers = (GStrv)g_ptr_array_free(normalized, FALSE);
  if (!self->servers[1]) {
    self->destination = g_strdup(self->servers[0]);
  } else {
    g_autofree gchar *list = g_strjoinv(", ", self->servers);
    /* Translators: %s is a comma-separated list of server addresses. */
    self->destination = g_strdup_printf(_("the first of your attachment servers that accepts them (%s)"),
                                        list);
  }
  return GN_ISSUE_UPLOADER(self);
}

const gchar *const *
gh_issue_uploader_get_servers(GhIssueUploader *self)
{
  g_return_val_if_fail(GH_IS_ISSUE_UPLOADER(self), NULL);
  return (const gchar *const *)self->servers;
}
