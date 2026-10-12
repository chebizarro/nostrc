/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gh-issue-uploader.h"
#include "gh-metadata-strip.h"
#include <glib/gi18n.h>
#include <string.h>

#define SNIFF_TEXT_BYTES (64 * 1024)

struct _GhIssueUploader {
  GObject parent_instance;
  GhBlossomClient *client;
  GStrv servers;
  gchar *destination;
  GStrv option_servers; /* the servers of the consent options last offered */
  GhIssueConsentFunc record_consent;
  gpointer record_data;
};

/* ---- Content type: from the bytes, never the name ---- */

static gboolean
starts_with(const guint8 *data, gsize size, gsize offset, const gchar *magic, gsize length)
{
  return size >= offset + length && memcmp(data + offset, magic, length) == 0;
}

/* UTF-8 text without NUL or other control characters but tab, line feed,
 * form feed and carriage return, in the first SNIFF_TEXT_BYTES. A character
 * cut by that bound is fine; one cut by the end of the file is not. */
static gboolean
looks_like_text(const guint8 *data, gsize size)
{
  gsize length = MIN(size, SNIFF_TEXT_BYTES);
  const gchar *end = NULL;
  if (!g_utf8_validate_len((const gchar *)data, length, &end)) {
    gsize valid = (gsize)((const guint8 *)end - data);
    if (length == size || length - valid >= 4 ||
        g_utf8_get_char_validated(end, (gssize)(length - valid)) != (gunichar)-2)
      return FALSE;
    length = valid;
  }
  for (gsize i = 0; i < length; i++) {
    guint8 c = data[i];
    if ((c < 0x20 && c != '\t' && c != '\n' && c != '\r' && c != '\f') || c == 0x7f)
      return FALSE;
  }
  return TRUE;
}

const gchar *
gh_issue_uploader_sniff(GBytes *bytes, const gchar **out_extension)
{
  gsize size = 0;
  const guint8 *data = bytes ? g_bytes_get_data(bytes, &size) : NULL;
  const gchar *type = "application/octet-stream";
  const gchar *extension = NULL;
  static const gchar *const mp4_brands[] = { "isom", "iso2", "iso4", "iso5", "iso6",
                                             "mp41", "mp42", "avc1", "dash", NULL };
  if (!data || size == 0) {
    /* nothing */
  } else if (starts_with(data, size, 0, "\xff\xd8\xff", 3)) {
    type = "image/jpeg", extension = "jpg";
  } else if (starts_with(data, size, 0, "\x89PNG\r\n\x1a\n", 8)) {
    type = "image/png", extension = "png";
  } else if (starts_with(data, size, 0, "GIF87a", 6) || starts_with(data, size, 0, "GIF89a", 6)) {
    type = "image/gif", extension = "gif";
  } else if (starts_with(data, size, 0, "RIFF", 4) && starts_with(data, size, 8, "WEBP", 4)) {
    type = "image/webp", extension = "webp";
  } else if (starts_with(data, size, 0, "%PDF-", 5)) {
    type = "application/pdf", extension = "pdf";
  } else if (starts_with(data, size, 4, "ftyp", 4) && size >= 12) {
    /* ISO base media: MP4 only by its major brand (QuickTime and others
     * are not on the list). */
    gchar brand[5] = { 0 };
    memcpy(brand, data + 8, 4);
    if (g_strv_contains(mp4_brands, brand))
      type = "video/mp4", extension = "mp4";
  } else if (starts_with(data, size, 0, "\x1a\x45\xdf\xa3", 4)) {
    /* EBML: WebM names its DocType "webm" in the header. */
    for (gsize i = 4; i + 4 <= MIN(size, (gsize)64); i++)
      if (memcmp(data + i, "webm", 4) == 0) {
        type = "video/webm", extension = "webm";
        break;
      }
  } else if (looks_like_text(data, size)) {
    type = "text/plain", extension = "txt";
  }
  if (out_extension)
    *out_extension = extension;
  return type;
}

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
  const gchar *extension = NULL;
  const gchar *type = gh_issue_uploader_sniff(stripped, &extension);
  gh_blossom_client_upload_public_async(self->client, (const gchar *const *)self->servers,
                                        stripped, sha256, type, extension,
                                        g_task_get_cancellable(task), on_uploaded, task);
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
  (void)content_type; /* the type comes from the bytes (gh_issue_uploader_sniff()) */
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

/* "Upload as your account" for each server the account has not consented
 * to, while the client has an account signer (nostrc-u42c7). */
static GStrv
dup_consent_options(GnIssueUploader *uploader)
{
  GhIssueUploader *self = GH_ISSUE_UPLOADER(uploader);
  g_clear_pointer(&self->option_servers, g_strfreev);
  if (!gh_blossom_client_has_account_signer(self->client))
    return NULL;
  g_autoptr(GStrvBuilder) servers = g_strv_builder_new();
  g_autoptr(GStrvBuilder) labels = g_strv_builder_new();
  for (guint i = 0; self->servers[i]; i++) {
    if (gh_blossom_client_get_account_consent(self->client, self->servers[i]))
      continue;
    g_autofree gchar *host = NULL;
    g_autofree gchar *normalized = gh_blossom_client_normalize_server(self->servers[i], &host);
    g_strv_builder_add(servers, self->servers[i]);
    /* Translators: %s is a server's host name. */
    g_strv_builder_take(labels, g_strdup_printf(
      _("Upload to %s as my account, if it only accepts accounts it knows. It then learns that these files are yours. Kept for this server, as for message attachments."),
      host ? host : self->servers[i]));
  }
  self->option_servers = g_strv_builder_end(servers);
  return g_strv_builder_end(labels);
}

static gboolean
apply_consent_option(GnIssueUploader *uploader, guint index, GError **error)
{
  GhIssueUploader *self = GH_ISSUE_UPLOADER(uploader);
  if (!self->option_servers || index >= g_strv_length(self->option_servers) ||
      !gh_blossom_client_has_account_signer(self->client)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        _("The account changed. Close this report and open a new one."));
    return FALSE;
  }
  const gchar *server = self->option_servers[index];
  if (self->record_consent)
    return self->record_consent(self->record_data, server, error);
  gh_blossom_client_set_account_consent(self->client, server, TRUE);
  return TRUE;
}

static void
uploader_iface_init(GnIssueUploaderInterface *iface)
{
  iface->describe_destination = describe_destination;
  iface->upload_async = upload_async;
  iface->upload_finish = upload_finish;
  iface->dup_consent_options = dup_consent_options;
  iface->apply_consent_option = apply_consent_option;
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
  g_strfreev(self->option_servers);
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

void
gh_issue_uploader_set_consent_recorder(GhIssueUploader *self, GhIssueConsentFunc func,
                                       gpointer user_data)
{
  g_return_if_fail(GH_IS_ISSUE_UPLOADER(self));
  self->record_consent = func;
  self->record_data = func ? user_data : NULL;
}
