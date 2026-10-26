/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-mls-media-manager.c - MLS Encrypted Media Manager (MIP-04)
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "gn-mls-media-manager.h"
#include <gnostr-plugin-api.h>
#include <jansson.h>
#include <marmot-gobject-1.0/marmot-gobject.h>
#include <marmot/marmot.h>
#include <marmot/marmot-media.h>
#include <stdlib.h>
#include <string.h>
#include <nostr-gobject-1.0/gn-blossom-client.h>
#include <nostr/bud02/auth.h>
#include <nostr-event.h>

#define DEFAULT_BLOSSOM_SERVER "https://blossom.primal.net"

struct _GnMlsMediaManager
{
  GObject parent_instance;

  GnMarmotService     *service;             /* strong ref */
  GnostrPluginContext *plugin_context;      /* borrowed */
  gchar               *blossom_server_url;  /* owned */
  GnBlossomClient     *blossom;        /* owned */
};

G_DEFINE_TYPE(GnMlsMediaManager, gn_mls_media_manager, G_TYPE_OBJECT)

/* ── GObject lifecycle ───────────────────────────────────────────── */

static void
gn_mls_media_manager_dispose(GObject *object)
{
  GnMlsMediaManager *self = GN_MLS_MEDIA_MANAGER(object);

  g_clear_object(&self->service);
  g_clear_object(&self->blossom);
  g_clear_pointer(&self->blossom_server_url, g_free);
  self->plugin_context = NULL;

  G_OBJECT_CLASS(gn_mls_media_manager_parent_class)->dispose(object);
}

static void
gn_mls_media_manager_class_init(GnMlsMediaManagerClass *klass)
{
  GObjectClass *oc = G_OBJECT_CLASS(klass);
  oc->dispose = gn_mls_media_manager_dispose;
}

static void
gn_mls_media_manager_init(GnMlsMediaManager *self)
{
  self->service             = NULL;
  self->plugin_context      = NULL;
  self->blossom_server_url  = NULL;
  self->blossom             = NULL;
}

/* ── Upload result ───────────────────────────────────────────────── */

void
gn_mls_media_upload_result_free(GnMlsMediaUploadResult *result)
{
  if (result == NULL) return;
  g_free(result->blossom_url);
  g_strfreev(result->imeta_tags);
  g_free(result);
}

/* ── imeta tag parsing ───────────────────────────────────────────── */

static gboolean
media_error(MarmotError err, const gchar *operation, GError **error)
{
  g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s: %s",
              operation, marmot_error_string(err));
  return FALSE;
}

static gboolean
group_id_from_hex(const gchar *hex, MarmotGroupId *out, GError **error)
{
  g_autofree guint8 *bytes = NULL;
  gsize n = strlen(hex);
  if (n == 0 || n % 2 != 0)
    goto invalid;
  for (gsize i = 0; i < n; i++)
    if (!g_ascii_isxdigit(hex[i]))
      goto invalid;
  bytes = g_malloc(n / 2);
  for (gsize i = 0; i < n / 2; i++)
    bytes[i] = (g_ascii_xdigit_value(hex[2 * i]) << 4) |
               g_ascii_xdigit_value(hex[2 * i + 1]);
  *out = marmot_group_id_new(bytes, n / 2);
  return TRUE;
invalid:
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                      "Invalid MLS group ID hex");
  return FALSE;
}

static gchar *
hash_hex(const guint8 hash[32])
{
  gchar *hex = g_malloc(65);
  for (gsize i = 0; i < 32; i++)
    g_snprintf(hex + 2 * i, 3, "%02x", hash[i]);
  return hex;
}

gboolean
gn_mls_media_manager_parse_imeta(const gchar *imeta_tag_json,
                                  MarmotMediaReference *out,
                                  GError **error)
{
  g_return_val_if_fail(imeta_tag_json != NULL && out != NULL, FALSE);
  memset(out, 0, sizeof *out);
  g_autofree const gchar **fields = NULL;
  g_autofree size_t *lengths = NULL;
  json_error_t parse_error;
  json_t *arr = json_loads(imeta_tag_json, JSON_ALLOW_NUL, &parse_error);
  MarmotError err = MARMOT_ERR_MEDIA_INVALID_REFERENCE;
  if (!json_is_array(arr))
    goto done;
  size_t n = json_array_size(arr);
  fields = g_new0(const gchar *, n + 1);
  lengths = g_new0(size_t, n);
  for (size_t i = 0; i < n; i++)
    {
      json_t *field = json_array_get(arr, i);
      if (!json_is_string(field))
        goto done;
      fields[i] = json_string_value(field);
      lengths[i] = json_string_length(field);
    }
  /* Preserve lengths so libmarmot rejects embedded NULs instead of validating
   * a truncated filename, locator, nonce or hash. */
  err = marmot_media_imeta_parse(fields, lengths, n, out);
done:
  json_decref(arr);
  return err == MARMOT_OK || media_error(err, "Invalid media reference", error);
}

gboolean
gn_mls_media_manager_check_epoch(GnMlsMediaManager *self,
                                  const gchar *group_id_hex,
                                  guint64 source_epoch,
                                  GError **error)
{
  g_return_val_if_fail(GN_IS_MLS_MEDIA_MANAGER(self), FALSE);
  g_return_val_if_fail(group_id_hex != NULL, FALSE);
  MarmotGroupId gid = {0};
  if (!group_id_from_hex(group_id_hex, &gid, error))
    return FALSE;
  MarmotGobjectClient *client = gn_marmot_service_get_client(self->service);
  Marmot *marmot = client ? marmot_gobject_client_get_marmot(client) : NULL;
  MarmotError err = MARMOT_ERR_INVALID_INPUT;
  if (marmot)
    {
      marmot_gobject_client_lock(client);
      err = marmot_media_check_epoch(marmot, &gid, source_epoch);
      marmot_gobject_client_unlock(client);
    }
  marmot_group_id_free(&gid);
  return err == MARMOT_OK || media_error(err, "Media epoch unavailable or changed; upload again", error);
}

/* ── Upload flow ─────────────────────────────────────────────────── */

typedef struct
{
  GnMlsMediaManager *manager;       /* strong ref */
  GTask              *task;
  gchar              *group_id_hex;
  GBytes             *plaintext;    /* original file bytes */
  gchar              *filename;
  gchar              *content_type;
  /* Encryption output */
  GBytes             *ciphertext;
  GnBlossomClient    *blossom;      /* per-upload limits, not download policy */
  MarmotMediaUpload   upload;
  gchar              *wire_hash;
} UploadData;

static void
upload_data_free(UploadData *data)
{
  g_clear_object(&data->manager);
  g_free(data->group_id_hex);
  g_clear_pointer(&data->plaintext, g_bytes_unref);
  g_free(data->filename);
  g_free(data->content_type);
  g_clear_pointer(&data->ciphertext, g_bytes_unref);
  g_clear_object(&data->blossom);
  marmot_media_upload_clear(&data->upload);
  g_free(data->wire_hash);
  g_free(data);
}

static void
on_blossom_upload_done(GObject      *source,
                       GAsyncResult *result,
                       gpointer      user_data)
{
  UploadData *data = user_data;
  g_autoptr(GError) error = NULL;

  g_autofree gchar *blob_url = gn_blossom_client_upload_finish(
    GN_BLOSSOM_CLIENT(source), result, &error);
  if (blob_url == NULL)
    {
      g_task_return_error(data->task, g_steal_pointer(&error));
      g_object_unref(data->task);
      upload_data_free(data);
      return;
    }
  if (!*blob_url)
    {
      g_free(blob_url);
      blob_url = g_strdup_printf("%s/%s", data->manager->blossom_server_url,
                                data->wire_hash);
    }

  g_info("MlsMediaManager: uploaded encrypted blob to %s", blob_url);

  char **fields = NULL;
  size_t count = 0;
  MarmotError err = marmot_media_reference_add_locator(&data->upload.reference,
    MARMOT_MEDIA_LOCATOR_BLOSSOM_V1, blob_url);
  if (err == MARMOT_OK)
    err = marmot_media_imeta_build(&data->upload.reference, NULL, &fields, &count);
  if (err != MARMOT_OK || !gn_mls_media_manager_check_epoch(data->manager,
        data->group_id_hex, data->upload.source_epoch, &error))
    {
      if (err != MARMOT_OK)
        media_error(err, "Cannot build media reference", &error);
      marmot_media_imeta_fields_free(fields, count);
      g_task_return_error(data->task, g_steal_pointer(&error));
      g_object_unref(data->task);
      upload_data_free(data);
      return;
    }

  GnMlsMediaUploadResult *upload_result = g_new0(GnMlsMediaUploadResult, 1);
  upload_result->blossom_url = g_steal_pointer(&blob_url);
  upload_result->source_epoch = data->upload.source_epoch;
  upload_result->imeta_tags = g_new0(gchar *, count + 1);
  for (size_t i = 0; i < count; i++)
    upload_result->imeta_tags[i] = g_strdup(fields[i]);
  marmot_media_imeta_fields_free(fields, count);

  g_task_return_pointer(data->task, upload_result,
                         (GDestroyNotify)gn_mls_media_upload_result_free);
  g_object_unref(data->task);
  upload_data_free(data);
}

static void
on_upload_signed(GObject *source, GAsyncResult *result, gpointer user_data)
{
  (void)source;
  UploadData *data = user_data;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *signed_event = gnostr_plugin_context_request_sign_event_finish(
    data->manager->plugin_context, result, &error);
  if (!signed_event)
    {
      if (!error)
        error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, "Media authorization signing failed");
      g_task_return_error(data->task, g_steal_pointer(&error));
      g_object_unref(data->task);
      upload_data_free(data);
      return;
    }
  gn_blossom_client_upload_async(data->blossom, data->ciphertext,
    data->wire_hash, "application/octet-stream", signed_event,
    g_task_get_cancellable(data->task), on_blossom_upload_done, data);
}

static void
do_blossom_upload(UploadData *data)
{
  /* BUD-02 binds the ciphertext on the wire, not the plaintext imeta hash. */
  data->wire_hash = hash_hex(data->upload.reference.ciphertext_sha256);
  data->blossom = gn_blossom_client_new(data->manager->blossom_server_url);
  gn_blossom_client_set_network(data->blossom, g_proxy_resolver_get_default(),
                               GN_BLOSSOM_NETWORK_DEFAULT);
  gn_blossom_client_set_limits(data->blossom, MAX(g_bytes_get_size(data->ciphertext), 1), 60, NULL);
  NostrEvent *event = nostr_bud02_create_auth_event(NOSTR_BUD02_ACTION_UPLOAD,
    data->wire_hash, 0, data->manager->blossom_server_url);
  g_autofree gchar *json = event ? nostr_event_serialize_compact(event) : NULL;
  nostr_event_free(event);
  if (!json)
    {
      g_task_return_new_error(data->task, G_IO_ERROR, G_IO_ERROR_FAILED,
                             "Cannot build media authorization");
      g_object_unref(data->task);
      upload_data_free(data);
      return;
    }
  gnostr_plugin_context_request_sign_event(data->manager->plugin_context, json,
    g_task_get_cancellable(data->task), on_upload_signed, data);
}

static void
on_file_read(GObject      *source,
             GAsyncResult *result,
             gpointer      user_data)
{
  UploadData *data = user_data;
  g_autoptr(GError) error = NULL;

  GFile *file = G_FILE(source);
  g_autoptr(GFileInputStream) stream =
    g_file_read_finish(file, result, &error);

  if (stream == NULL)
    {
      g_task_return_error(data->task, g_steal_pointer(&error));
      g_object_unref(data->task);
      upload_data_free(data);
      return;
    }

  /* Read entire file into memory */
  g_autoptr(GOutputStream) mem_out = g_memory_output_stream_new_resizable();
  g_autoptr(GError) splice_error = NULL;

  gssize spliced = g_output_stream_splice(
    mem_out,
    G_INPUT_STREAM(stream),
    G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE | G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
    g_task_get_cancellable(data->task),
    &splice_error);

  if (spliced < 0)
    {
      g_task_return_error(data->task, g_steal_pointer(&splice_error));
      g_object_unref(data->task);
      upload_data_free(data);
      return;
    }

  data->plaintext = g_memory_output_stream_steal_as_bytes(
    G_MEMORY_OUTPUT_STREAM(mem_out));

  /*
   * MIP-04: Encrypt the media using the group's MLS exporter secret.
   * We use the libmarmot C API directly via marmot_gobject_client_get_marmot().
   */
  MarmotGobjectClient *client = gn_marmot_service_get_client(data->manager->service);
  if (client == NULL)
    {
      g_task_return_new_error(data->task, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                              "Marmot client not available");
      g_object_unref(data->task);
      upload_data_free(data);
      return;
    }

  Marmot *marmot = marmot_gobject_client_get_marmot(client);
  if (marmot == NULL)
    {
      g_task_return_new_error(data->task, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                              "Marmot instance not available");
      g_object_unref(data->task);
      upload_data_free(data);
      return;
    }

  gsize plain_len = 0;
  const guint8 *plain_bytes = g_bytes_get_data(data->plaintext, &plain_len);

  MarmotGroupId mls_group_id = {0};
  if (!group_id_from_hex(data->group_id_hex, &mls_group_id, &error))
    {
      g_task_return_error(data->task, g_steal_pointer(&error));
      g_object_unref(data->task);
      upload_data_free(data);
      return;
    }

  g_autofree gchar *guessed = g_content_type_guess(data->filename, plain_bytes, plain_len, NULL);
  data->content_type = guessed ? g_content_type_get_mime_type(guessed) : NULL;
  if (!data->content_type)
    data->content_type = g_strdup("application/octet-stream");

  /* libmarmot derives encrypted-media-v2 keys from the source epoch's
   * MLS-Exporter("marmot", "encrypted-media", 32). */
  /* A Marmot is not thread-safe: hold the client's lock (W17b C1). */
  marmot_gobject_client_lock(client);
  MarmotError err = marmot_media_encrypt(
    marmot,
    &mls_group_id,
    plain_bytes, plain_len,
    data->content_type,
    data->filename,
    &data->upload);
  marmot_gobject_client_unlock(client);

  marmot_group_id_free(&mls_group_id);

  if (err != MARMOT_OK)
    {
      g_task_return_new_error(data->task, G_IO_ERROR, G_IO_ERROR_FAILED,
                              "MIP-04 encryption failed: %s",
                              marmot_error_string(err));
      g_object_unref(data->task);
      upload_data_free(data);
      return;
    }

  data->ciphertext = g_bytes_new_take(data->upload.ciphertext, data->upload.ciphertext_len);
  data->upload.ciphertext = NULL; /* transferred to GBytes */

  do_blossom_upload(data);
}

/* ── Download flow ───────────────────────────────────────────────── */

typedef struct
{
  GnMlsMediaManager *manager;       /* strong ref */
  GTask              *task;
  gchar              *group_id_hex;
  MarmotMediaReference reference;
  guint64             source_epoch;
} DownloadData;

static void
download_data_free(DownloadData *data)
{
  g_clear_object(&data->manager);
  g_free(data->group_id_hex);
  marmot_media_reference_clear(&data->reference);
  g_free(data);
}

static void
on_blossom_download_done(GObject      *source,
                         GAsyncResult *result,
                         gpointer      user_data)
{
  DownloadData *data = user_data;
  g_autoptr(GError) error = NULL;

  g_autoptr(GBytes) cipher_bytes = gn_blossom_client_download_finish(
    GN_BLOSSOM_CLIENT(source), result, &error);

  if (cipher_bytes == NULL)
    {
      g_task_return_error(data->task, g_steal_pointer(&error));
      g_object_unref(data->task);
      download_data_free(data);
      return;
    }

  /*
   * MIP-04: Decrypt the downloaded blob using libmarmot.
   */
  MarmotGobjectClient *client = gn_marmot_service_get_client(data->manager->service);
  if (client == NULL)
    {
      g_task_return_new_error(data->task, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                              "Marmot client not available");
      g_object_unref(data->task);
      download_data_free(data);
      return;
    }

  Marmot *marmot = marmot_gobject_client_get_marmot(client);
  if (marmot == NULL)
    {
      g_task_return_new_error(data->task, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                              "Marmot instance not available");
      g_object_unref(data->task);
      download_data_free(data);
      return;
    }

  gsize cipher_len = 0;
  const guint8 *cipher_data = g_bytes_get_data(cipher_bytes, &cipher_len);

  MarmotGroupId mls_group_id = {0};
  if (!group_id_from_hex(data->group_id_hex, &mls_group_id, &error))
    {
      g_task_return_error(data->task, g_steal_pointer(&error));
      g_object_unref(data->task);
      download_data_free(data);
      return;
    }

  /* Call libmarmot decryption */
  uint8_t *plaintext_out = NULL;
  size_t plaintext_len = 0;

  marmot_gobject_client_lock(client);   /* W17b C1 */
  /* libmarmot verifies ciphertext hash, AEAD and plaintext hash, using
   * the carrying message's epoch, including any retained earlier secret. */
  MarmotError err = marmot_media_decrypt(
    marmot, &mls_group_id, data->source_epoch, &data->reference,
    cipher_data, cipher_len, &plaintext_out, &plaintext_len);
  marmot_gobject_client_unlock(client);

  marmot_group_id_free(&mls_group_id);

  if (err != MARMOT_OK)
    {
      g_task_return_new_error(data->task, G_IO_ERROR, G_IO_ERROR_FAILED,
                              "MIP-04 decryption failed: %s",
                              marmot_error_string(err));
      g_object_unref(data->task);
      download_data_free(data);
      return;
    }

  g_info("MlsMediaManager: MIP-04 decryption successful (%zu bytes)", plaintext_len);

  g_task_set_task_data(data->task, g_strdup(data->reference.media_type), g_free);
  GBytes *result_bytes = g_bytes_new_take(plaintext_out, plaintext_len);
  g_task_return_pointer(data->task, result_bytes, (GDestroyNotify)g_bytes_unref);
  g_object_unref(data->task);
  download_data_free(data);
}

/* ── Public API ──────────────────────────────────────────────────── */

GnMlsMediaManager *
gn_mls_media_manager_new(GnMarmotService     *service,
                           GnostrPluginContext *plugin_context,
                           const gchar         *blossom_server_url)
{
  g_return_val_if_fail(GN_IS_MARMOT_SERVICE(service), NULL);
  g_return_val_if_fail(plugin_context != NULL, NULL);

  GnMlsMediaManager *self = g_object_new(GN_TYPE_MLS_MEDIA_MANAGER, NULL);
  self->service             = g_object_ref(service);
  self->plugin_context      = plugin_context;
  self->blossom_server_url  = g_strdup(
    (blossom_server_url != NULL) ? blossom_server_url : DEFAULT_BLOSSOM_SERVER);

  self->blossom = gn_blossom_client_new(self->blossom_server_url);
  gn_blossom_client_set_network(self->blossom, g_proxy_resolver_get_default(),
                               GN_BLOSSOM_NETWORK_DEFAULT);
  gn_blossom_client_set_limits(self->blossom, 0, 60, NULL);
  return self;
}

void
gn_mls_media_manager_upload_async(GnMlsMediaManager   *self,
                                    const gchar          *group_id_hex,
                                    GFile                *file,
                                    GCancellable         *cancellable,
                                    GAsyncReadyCallback   callback,
                                    gpointer              user_data)
{
  g_return_if_fail(GN_IS_MLS_MEDIA_MANAGER(self));
  g_return_if_fail(group_id_hex != NULL);
  g_return_if_fail(G_IS_FILE(file));

  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, gn_mls_media_manager_upload_async);

  UploadData *data = g_new0(UploadData, 1);
  data->manager      = g_object_ref(self);
  data->task         = task;
  data->group_id_hex = g_strdup(group_id_hex);

  /* Get filename for metadata */
  data->filename = g_file_get_basename(file);

  g_file_read_async(file,
                    G_PRIORITY_DEFAULT,
                    cancellable,
                    on_file_read,
                    data);
}

GnMlsMediaUploadResult *
gn_mls_media_manager_upload_finish(GnMlsMediaManager *self,
                                     GAsyncResult       *result,
                                     GError            **error)
{
  g_return_val_if_fail(GN_IS_MLS_MEDIA_MANAGER(self), NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

void
gn_mls_media_manager_download_async(GnMlsMediaManager   *self,
                                      const gchar          *group_id_hex,
                                      const gchar          *imeta_tag_json,
                                      guint64               source_epoch,
                                      GCancellable         *cancellable,
                                      GAsyncReadyCallback   callback,
                                      gpointer              user_data)
{
  g_return_if_fail(GN_IS_MLS_MEDIA_MANAGER(self));
  g_return_if_fail(group_id_hex != NULL);
  g_return_if_fail(imeta_tag_json != NULL);

  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, gn_mls_media_manager_download_async);

  DownloadData *data = g_new0(DownloadData, 1);
  data->manager      = g_object_ref(self);
  data->task         = task;
  data->group_id_hex = g_strdup(group_id_hex);
  data->source_epoch = source_epoch;

  g_autoptr(GError) error = NULL;
  if (!gn_mls_media_manager_parse_imeta(imeta_tag_json, &data->reference, &error))
    {
      g_task_return_error(task, g_steal_pointer(&error));
      g_object_unref(task);
      download_data_free(data);
      return;
    }
  const gchar *blossom_url = NULL;
  for (size_t i = 0; i < data->reference.locator_count; i++)
    if (g_strcmp0(data->reference.locators[i].kind, MARMOT_MEDIA_LOCATOR_BLOSSOM_V1) == 0)
      {
        blossom_url = data->reference.locators[i].value;
        break;
      }
  if (!blossom_url)
    {
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                             "Media reference has no supported Blossom locator");
      g_object_unref(task);
      download_data_free(data);
      return;
    }
  g_autofree gchar *wire_hash = hash_hex(data->reference.ciphertext_sha256);
  gn_blossom_client_download_async(self->blossom, blossom_url, wire_hash,
    cancellable, on_blossom_download_done, data);
}

GBytes *
gn_mls_media_manager_download_finish(GnMlsMediaManager *self,
                                       GAsyncResult       *result,
                                       gchar             **out_content_type,
                                       GError            **error)
{
  g_return_val_if_fail(GN_IS_MLS_MEDIA_MANAGER(self), NULL);

  GBytes *bytes = g_task_propagate_pointer(G_TASK(result), error);
  if (out_content_type != NULL)
    *out_content_type = bytes ? g_strdup(g_task_get_task_data(G_TASK(result))) : NULL;
  return bytes;
}
