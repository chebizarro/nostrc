#include "gh-mls-media.h"

#include "gh-attachment.h"

#include <nostr-event.h>
#include <nostr-tag.h>
#include <string.h>

G_DEFINE_QUARK(gh-mls-media-error-quark, gh_mls_media_error)

struct _GhMlsAttachment {
  MarmotMediaReference ref;
  guint64 source_epoch;
};

/* ---- the attachment --------------------------------------------------------------- */

static GhMlsAttachment *
attachment_take(MarmotMediaReference *ref, guint64 source_epoch)
{
  GhMlsAttachment *a = g_new0(GhMlsAttachment, 1);
  a->ref = *ref;
  memset(ref, 0, sizeof *ref);
  a->source_epoch = source_epoch;
  return a;
}

void
gh_mls_attachment_free(GhMlsAttachment *attachment)
{
  if (!attachment)
    return;
  marmot_media_reference_clear(&attachment->ref);
  g_free(attachment);
}

GhMlsAttachment *
gh_mls_attachment_copy(const GhMlsAttachment *src)
{
  g_return_val_if_fail(src != NULL, NULL);
  MarmotMediaReference ref = { 0 };
  memcpy(ref.ciphertext_sha256, src->ref.ciphertext_sha256, 32);
  memcpy(ref.plaintext_sha256, src->ref.plaintext_sha256, 32);
  memcpy(ref.nonce, src->ref.nonce, 12);
  ref.media_type = g_strdup(src->ref.media_type);
  ref.filename = g_strdup(src->ref.filename);
  gboolean ok = marmot_media_reference_set_hints(&ref, src->ref.dim, src->ref.thumbhash) ==
                MARMOT_OK;
  for (size_t i = 0; ok && i < src->ref.locator_count; i++)
    ok = marmot_media_reference_add_locator(&ref, src->ref.locators[i].kind,
                                            src->ref.locators[i].value) == MARMOT_OK;
  if (!ok)
    g_error("out of memory copying an attachment");
  return attachment_take(&ref, src->source_epoch);
}

G_DEFINE_BOXED_TYPE(GhMlsAttachment, gh_mls_attachment, gh_mls_attachment_copy,
                    gh_mls_attachment_free)

guint64
gh_mls_attachment_get_source_epoch(const GhMlsAttachment *a)
{
  g_return_val_if_fail(a != NULL, 0);
  return a->source_epoch;
}

const gchar *
gh_mls_attachment_get_media_type(const GhMlsAttachment *a)
{
  g_return_val_if_fail(a != NULL, NULL);
  return a->ref.media_type;
}

const gchar *
gh_mls_attachment_get_filename(const GhMlsAttachment *a)
{
  g_return_val_if_fail(a != NULL, NULL);
  return a->ref.filename;
}

const gchar *
gh_mls_attachment_get_dim(const GhMlsAttachment *a)
{
  g_return_val_if_fail(a != NULL, NULL);
  return a->ref.dim;
}

static gchar *
hex_of(const guint8 *bytes, gsize n)
{
  gchar *out = g_malloc(2 * n + 1);
  for (gsize i = 0; i < n; i++)
    g_snprintf(out + 2 * i, 3, "%02x", bytes[i]);
  return out;
}

gchar *
gh_mls_attachment_dup_ciphertext_sha256(const GhMlsAttachment *a)
{
  g_return_val_if_fail(a != NULL, NULL);
  return hex_of(a->ref.ciphertext_sha256, 32);
}

GStrv
gh_mls_attachment_dup_blossom_urls(const GhMlsAttachment *a)
{
  g_return_val_if_fail(a != NULL, NULL);
  g_autoptr(GStrvBuilder) urls = g_strv_builder_new();
  for (size_t i = 0; i < a->ref.locator_count; i++)
    if (g_strcmp0(a->ref.locators[i].kind, MARMOT_MEDIA_LOCATOR_BLOSSOM_V1) == 0)
      g_strv_builder_add(urls, a->ref.locators[i].value);
  return g_strv_builder_end(urls);
}

/* libmarmot's verdict as a GError. */
static gboolean
media_fail(MarmotError err, const gchar *what, GError **error)
{
  GhMlsMediaError code;
  switch (err) {
  case MARMOT_ERR_MEDIA_INVALID_REFERENCE:
  case MARMOT_ERR_INVALID_INPUT:
    code = GH_MLS_MEDIA_ERROR_INVALID;
    break;
  case MARMOT_ERR_MEDIA_UNSUPPORTED_VERSION:
    code = GH_MLS_MEDIA_ERROR_UNSUPPORTED;
    break;
  case MARMOT_ERR_MEDIA_CIPHERTEXT_HASH:
  case MARMOT_ERR_MEDIA_DECRYPT:
  case MARMOT_ERR_MEDIA_HASH_MISMATCH:
    code = GH_MLS_MEDIA_ERROR_DAMAGED;
    break;
  case MARMOT_ERR_STORAGE_NOT_FOUND:
    code = GH_MLS_MEDIA_ERROR_NO_KEY;
    break;
  default:
    code = GH_MLS_MEDIA_ERROR_FAILED;
  }
  g_set_error(error, GH_MLS_MEDIA_ERROR, code, "%s: %s", what, marmot_error_string(err));
  return FALSE;
}

GStrv
gh_mls_attachment_dup_imeta(const GhMlsAttachment *a, GError **error)
{
  g_return_val_if_fail(a != NULL, NULL);
  char **fields = NULL;
  size_t n = 0;
  MarmotError err = marmot_media_imeta_build(&a->ref, NULL, &fields, &n);
  if (err != MARMOT_OK) {
    media_fail(err, "The attachment cannot be sent", error);
    return NULL;
  }
  GStrv tag = g_new0(gchar *, n + 1);
  for (size_t i = 0; i < n; i++)
    tag[i] = g_strdup(fields[i]);
  marmot_media_imeta_fields_free(fields, n);
  return tag;
}

GhMlsAttachment *
gh_mls_attachment_new_from_imeta(const gchar *const *tag, guint64 source_epoch, GError **error)
{
  g_return_val_if_fail(tag != NULL, NULL);
  MarmotMediaReference ref;
  MarmotError err = marmot_media_imeta_parse((const char *const *)tag, NULL,
                                             g_strv_length((GStrv)tag), &ref);
  if (err != MARMOT_OK) {
    media_fail(err, "The attachment is not readable", error);
    return NULL;
  }
  return attachment_take(&ref, source_epoch);
}

GPtrArray *
gh_mls_attachments_from_inner_event(const gchar *inner_json, guint64 source_epoch,
                                    guint *out_rejected)
{
  GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)gh_mls_attachment_free);
  if (out_rejected)
    *out_rejected = 0;
  NostrEvent *event = inner_json ? nostr_event_new() : NULL;
  if (!event)
    return out;
  if (nostr_event_deserialize_unsigned(event, inner_json, NULL) == NOSTR_EVENT_VALIDATION_OK) {
    NostrTags *tags = nostr_event_get_tags(event);
    for (size_t i = 0; tags && i < nostr_tags_size(tags); i++) {
      NostrTag *t = nostr_tags_get(tags, i);
      if (!t || nostr_tag_size(t) < 1 || g_strcmp0(nostr_tag_get(t, 0), "imeta") != 0)
        continue;
      g_autoptr(GStrvBuilder) b = g_strv_builder_new();
      for (size_t k = 0; k < nostr_tag_size(t); k++)
        g_strv_builder_add(b, nostr_tag_get(t, k) ? nostr_tag_get(t, k) : "");
      g_auto(GStrv) fields = g_strv_builder_end(b);
      GhMlsAttachment *a =
        gh_mls_attachment_new_from_imeta((const gchar *const *)fields, source_epoch, NULL);
      if (a)
        g_ptr_array_add(out, a);
      else if (out_rejected)
        (*out_rejected)++;
    }
  }
  nostr_event_free(event);
  return out;
}

/* ---- step 1: a file on this device ------------------------------------------------ */

typedef struct {
  gchar *name, *type;
} FileInfo;

static void
file_info_free(gpointer data)
{
  FileInfo *info = data;
  g_free(info->name);
  g_free(info->type);
  g_free(info);
}

static void
on_file_loaded(GObject *source, GAsyncResult *result, gpointer data)
{
  GTask *task = data;
  GError *error = NULL;
  GBytes *bytes = g_file_load_bytes_finish(G_FILE(source), result, NULL, &error);
  if (bytes)
    g_task_return_pointer(task, bytes, (GDestroyNotify)g_bytes_unref);
  else
    g_task_return_error(task, error);
  g_object_unref(task);
}

static void
on_file_info(GObject *source, GAsyncResult *result, gpointer data)
{
  GTask *task = data;
  GError *error = NULL;
  g_autoptr(GFileInfo) info = g_file_query_info_finish(G_FILE(source), result, &error);
  if (!info) {
    g_task_return_error(task, error);
    g_object_unref(task);
    return;
  }
  if (g_file_info_get_file_type(info) != G_FILE_TYPE_REGULAR) {
    g_task_return_new_error(task, GH_MLS_MEDIA_ERROR, GH_MLS_MEDIA_ERROR_NOT_LOCAL,
                            "Only files on this device can be sent");
    g_object_unref(task);
    return;
  }
  FileInfo *fi = g_task_get_task_data(task);
  fi->name = g_strdup(g_file_info_get_display_name(info));
  const gchar *type = g_file_info_get_content_type(info);
  fi->type = type ? g_content_type_get_mime_type(type) : NULL;
  g_file_load_bytes_async(G_FILE(source), g_task_get_cancellable(task), on_file_loaded, task);
}

void
gh_mls_media_read_file_async(GFile *file, GCancellable *cancellable,
                             GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(G_IS_FILE(file));
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  g_task_set_source_tag(task, gh_mls_media_read_file_async);
  g_task_set_task_data(task, g_new0(FileInfo, 1), file_info_free);
  /* Charter §4.2: nothing of a remote GVfs location (https://, smb://) is
   * queried or read; GVfs would fetch it outside GhNetHttp and Tor. */
  if (!g_file_is_native(file)) {
    g_task_return_new_error(task, GH_MLS_MEDIA_ERROR, GH_MLS_MEDIA_ERROR_NOT_LOCAL,
                            "Only files on this device can be sent");
    g_object_unref(task);
    return;
  }
  g_file_query_info_async(file,
                          G_FILE_ATTRIBUTE_STANDARD_TYPE ","
                          G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME ","
                          G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
                          G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT, cancellable, on_file_info,
                          task);
}

GBytes *
gh_mls_media_read_file_finish(GAsyncResult *result, gchar **out_name, gchar **out_type,
                              GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, NULL), NULL);
  FileInfo *fi = g_task_get_task_data(G_TASK(result));
  GBytes *bytes = g_task_propagate_pointer(G_TASK(result), error);
  if (out_name)
    *out_name = bytes && fi ? g_strdup(fi->name) : NULL;
  if (out_type)
    *out_type = bytes && fi ? g_strdup(fi->type) : NULL;
  return bytes;
}

/* ---- step 2: seal ----------------------------------------------------------------- */

static gboolean
gid_from_hex(const gchar *hex, MarmotGroupId *out, GError **error)
{
  gsize len = hex ? strlen(hex) : 0;
  guint8 bytes[256];
  gboolean ok = len >= 2 && len % 2 == 0 && len / 2 <= sizeof bytes;
  for (gsize i = 0; ok && i < len / 2; i++) {
    gint hi = g_ascii_xdigit_value(hex[2 * i]), lo = g_ascii_xdigit_value(hex[2 * i + 1]);
    ok = hi >= 0 && lo >= 0;
    bytes[i] = (guint8)(hi << 4 | lo);
  }
  if (ok)
    *out = marmot_group_id_new(bytes, len / 2);
  if (!ok || !out->data) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Not a group id");
    return FALSE;
  }
  return TRUE;
}

void
gh_mls_media_sealed_free(GhMlsMediaSealed *sealed)
{
  if (!sealed)
    return;
  g_clear_pointer(&sealed->ciphertext, g_bytes_unref);
  g_clear_pointer(&sealed->attachment, gh_mls_attachment_free);
  g_free(sealed);
}

GhMlsMediaSealed *
gh_mls_media_seal(Marmot *marmot, const gchar *group_id_hex, GBytes *file,
                  const gchar *mime_hint, const gchar *filename, gsize max_size, GError **error)
{
  g_return_val_if_fail(marmot != NULL && file != NULL, NULL);
  if (!filename || !*filename || strlen(filename) > MARMOT_MEDIA_FILENAME_MAX ||
      !g_utf8_validate(filename, -1, NULL)) {
    g_set_error_literal(error, GH_MLS_MEDIA_ERROR, GH_MLS_MEDIA_ERROR_INVALID,
                        "The file name must be 1 to 255 bytes of text");
    return NULL;
  }
  MarmotGroupId gid;
  if (!gid_from_hex(group_id_hex, &gid, error))
    return NULL;
  /* Metadata out first (§6 step 2): what is encrypted is what is sent. */
  g_autoptr(GhAttachmentPrepared) prepared = gh_attachment_prepare(file, mime_hint, max_size,
                                                                   error);
  if (!prepared) {
    marmot_group_id_free(&gid);
    return NULL;
  }
  gsize len = 0;
  const guint8 *data = g_bytes_get_data(prepared->plaintext, &len);
  /* A hint libmarmot cannot canonicalize is sent as opaque bytes. */
  g_autofree gchar *mime = NULL;
  char *canonical = NULL;
  if (prepared->mime && marmot_media_type_canonicalize(prepared->mime, &canonical) == MARMOT_OK)
    mime = g_strdup(canonical);
  else
    mime = g_strdup("application/octet-stream");
  free(canonical);
  MarmotMediaUpload up;
  MarmotError err = marmot_media_encrypt(marmot, &gid, data, len, mime, filename, &up);
  marmot_group_id_free(&gid);
  if (err != MARMOT_OK) {
    media_fail(err, "The attachment could not be encrypted", error);
    return NULL;
  }
  GhMlsMediaSealed *sealed = g_new0(GhMlsMediaSealed, 1);
  sealed->ciphertext = g_bytes_new_take(up.ciphertext, up.ciphertext_len);
  up.ciphertext = NULL;
  if (prepared->width && prepared->height) {
    g_autofree gchar *dim = g_strdup_printf("%ux%u", prepared->width, prepared->height);
    if (marmot_media_reference_set_hints(&up.reference, dim, NULL) != MARMOT_OK)
      g_error("out of memory sealing an attachment");
  }
  sealed->width = prepared->width;
  sealed->height = prepared->height;
  sealed->attachment = attachment_take(&up.reference, up.source_epoch);
  marmot_media_upload_clear(&up);
  return sealed;
}

/* ---- step 3: upload --------------------------------------------------------------- */

static void
on_uploaded(GObject *source, GAsyncResult *result, gpointer data)
{
  GTask *task = data;
  GError *error = NULL;
  g_autofree gchar *url = gh_blossom_client_upload_finish(GH_BLOSSOM_CLIENT(source), result,
                                                          NULL, &error);
  if (!url) {
    g_task_return_error(task, error);
    g_object_unref(task);
    return;
  }
  GhMlsAttachment *a = gh_mls_attachment_copy(g_task_get_task_data(task));
  if (marmot_media_reference_add_locator(&a->ref, MARMOT_MEDIA_LOCATOR_BLOSSOM_V1, url) !=
      MARMOT_OK)
    g_error("out of memory adding a locator");
  /* The server's answer is the locator only if it is a valid one. */
  g_auto(GStrv) check = gh_mls_attachment_dup_imeta(a, &error);
  if (!check) {
    gh_mls_attachment_free(a);
    g_task_return_error(task, error);
  } else {
    g_task_return_pointer(task, a, (GDestroyNotify)gh_mls_attachment_free);
  }
  g_object_unref(task);
}

void
gh_mls_media_upload_async(GhBlossomClient *client, const GhMlsMediaSealed *sealed,
                          GCancellable *cancellable, GAsyncReadyCallback callback,
                          gpointer user_data)
{
  g_return_if_fail(GH_IS_BLOSSOM_CLIENT(client));
  g_return_if_fail(sealed != NULL && sealed->ciphertext && sealed->attachment);
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  g_task_set_source_tag(task, gh_mls_media_upload_async);
  g_task_set_task_data(task, gh_mls_attachment_copy(sealed->attachment),
                       (GDestroyNotify)gh_mls_attachment_free);
  g_autofree gchar *sha = gh_mls_attachment_dup_ciphertext_sha256(sealed->attachment);
  gh_blossom_client_upload_async(client, sealed->ciphertext, sha, cancellable, on_uploaded,
                                 task);
}

GhMlsAttachment *
gh_mls_media_upload_finish(GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, NULL), NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

/* ---- step 4: the epoch ------------------------------------------------------------ */

gboolean
gh_mls_media_check_epoch(Marmot *marmot, const gchar *group_id_hex,
                         const GhMlsAttachment *attachment, GError **error)
{
  g_return_val_if_fail(marmot != NULL && attachment != NULL, FALSE);
  MarmotGroupId gid;
  if (!gid_from_hex(group_id_hex, &gid, error))
    return FALSE;
  MarmotGroup *group = NULL;
  MarmotError err = marmot_get_group(marmot, &gid, &group);
  marmot_group_id_free(&gid);
  if (err != MARMOT_OK || !group)
    return media_fail(err != MARMOT_OK ? err : MARMOT_ERR_GROUP_NOT_FOUND,
                      "The group is not available", error);
  gboolean same = group->epoch == attachment->source_epoch;
  marmot_group_free(group);
  if (!same)
    g_set_error_literal(error, GH_MLS_MEDIA_ERROR, GH_MLS_MEDIA_ERROR_EPOCH_CHANGED,
                        "The group changed while the file was uploading; it is sent again");
  return same;
}

/* ---- download --------------------------------------------------------------------- */

typedef struct {
  GhBlossomClient *client;
  GStrv urls;
  guint next;
  gchar *sha;
  GError *last;
} Fetch;

static void
fetch_free(gpointer data)
{
  Fetch *f = data;
  g_clear_object(&f->client);
  g_strfreev(f->urls);
  g_free(f->sha);
  g_clear_error(&f->last);
  g_free(f);
}

static void fetch_next(GTask *task);

static gboolean
bytes_have_sha256(GBytes *bytes, const gchar *sha)
{
  g_autofree gchar *got = g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, bytes);
  return got && g_ascii_strcasecmp(got, sha) == 0;
}

static void
on_fetched(GObject *source, GAsyncResult *result, gpointer data)
{
  GTask *task = data;
  Fetch *f = g_task_get_task_data(task);
  GError *error = NULL;
  GBytes *body = gh_blossom_client_download_finish(GH_BLOSSOM_CLIENT(source), result, &error);
  if (body && bytes_have_sha256(body, f->sha)) {
    g_task_return_pointer(task, body, (GDestroyNotify)g_bytes_unref);
    g_object_unref(task);
    return;
  }
  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    g_task_return_error(task, error);
    g_object_unref(task);
    return;
  }
  g_clear_pointer(&body, g_bytes_unref);
  g_clear_error(&f->last);
  f->last = error ? error
                  : g_error_new_literal(GH_MLS_MEDIA_ERROR, GH_MLS_MEDIA_ERROR_DAMAGED,
                                        "The server sent other bytes than the file");
  fetch_next(task);
}

static void
fetch_next(GTask *task)
{
  Fetch *f = g_task_get_task_data(task);
  if (!f->urls[f->next]) {
    if (f->last && f->last->domain == GH_MLS_MEDIA_ERROR) {
      g_task_return_error(task, g_steal_pointer(&f->last));
    } else {
      g_task_return_new_error(task, GH_MLS_MEDIA_ERROR, GH_MLS_MEDIA_ERROR_UNAVAILABLE,
                              "The file is not available%s%s", f->last ? ": " : "",
                              f->last ? f->last->message : "");
    }
    g_object_unref(task);
    return;
  }
  const gchar *url = f->urls[f->next++];
  gh_blossom_client_download_async(f->client, url, f->sha, 0, g_task_get_cancellable(task),
                                   on_fetched, task);
}

void
gh_mls_media_fetch_async(GhBlossomClient *client, const GhMlsAttachment *attachment,
                         GCancellable *cancellable, GAsyncReadyCallback callback,
                         gpointer user_data)
{
  g_return_if_fail(GH_IS_BLOSSOM_CLIENT(client));
  g_return_if_fail(attachment != NULL);
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  g_task_set_source_tag(task, gh_mls_media_fetch_async);
  Fetch *f = g_new0(Fetch, 1);
  f->client = g_object_ref(client);
  f->urls = gh_mls_attachment_dup_blossom_urls(attachment);
  f->sha = gh_mls_attachment_dup_ciphertext_sha256(attachment);
  g_task_set_task_data(task, f, fetch_free);
  fetch_next(task);
}

GBytes *
gh_mls_media_fetch_finish(GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, NULL), NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

GBytes *
gh_mls_media_open(Marmot *marmot, const gchar *group_id_hex, const GhMlsAttachment *attachment,
                  GBytes *ciphertext, GError **error)
{
  g_return_val_if_fail(marmot != NULL && attachment != NULL && ciphertext != NULL, NULL);
  MarmotGroupId gid;
  if (!gid_from_hex(group_id_hex, &gid, error))
    return NULL;
  gsize len = 0;
  const guint8 *data = g_bytes_get_data(ciphertext, &len);
  uint8_t *pt = NULL;
  size_t pt_len = 0;
  MarmotError err = marmot_media_decrypt(marmot, &gid, attachment->source_epoch,
                                         &attachment->ref, data ? data : (const guint8 *)"",
                                         len, &pt, &pt_len);
  marmot_group_id_free(&gid);
  if (err != MARMOT_OK) {
    media_fail(err, "The file could not be opened", error);
    return NULL;
  }
  return g_bytes_new_take(pt, pt_len);
}
