/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-mls-media-manager.c - MLS Encrypted Media Manager (MIP-04)
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "gn-mls-media-manager.h"
#include <gnostr-plugin-api.h>
#include <json-glib/json-glib.h>
#include <marmot-gobject-1.0/marmot-gobject.h>
#include <marmot/marmot.h>
#include <nostr-gobject-1.0/gn-blossom-client.h>
#include <nostr/bud02/auth.h>
#include <nostr-event.h>

#define DEFAULT_BLOSSOM_SERVER "https://blossom.primal.net"

/* MIP-04 imeta tag field names */
#define IMETA_URL    "url"
#define IMETA_NONCE  "nonce"
#define IMETA_EPOCH  "epoch"
#define IMETA_HASH   "x"
#define IMETA_ENC    "encoding"
#define IMETA_ENC_VAL "mls"

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

gboolean
gn_mls_media_manager_parse_imeta(const gchar *imeta_tag_json,
                                   gchar      **out_url,
                                   gchar      **out_nonce_b64,
                                   guint64     *out_epoch,
                                   gchar      **out_hash)
{
  g_return_val_if_fail(imeta_tag_json != NULL, FALSE);

  g_autoptr(JsonParser) parser = json_parser_new();
  g_autoptr(GError) error = NULL;

  if (!json_parser_load_from_data(parser, imeta_tag_json, -1, &error))
    {
      g_warning("MlsMediaManager: failed to parse imeta tag: %s",
                error->message);
      return FALSE;
    }

  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_ARRAY(root))
    return FALSE;

  JsonArray *arr = json_node_get_array(root);
  guint len = json_array_get_length(arr);

  if (out_url)       *out_url       = NULL;
  if (out_nonce_b64) *out_nonce_b64 = NULL;
  if (out_epoch)     *out_epoch     = 0;
  if (out_hash)      *out_hash      = NULL;

  /*
   * imeta tag format (NIP-92 / MIP-04 extension):
   * ["imeta", "url <url>", "nonce <b64>", "epoch <n>", "x <hash>", "encoding mls"]
   *
   * Each element after "imeta" is a space-separated key-value pair.
   */
  for (guint i = 1; i < len; i++)
    {
      const gchar *entry = json_array_get_string_element(arr, i);
      if (entry == NULL) continue;

      g_auto(GStrv) parts = g_strsplit(entry, " ", 2);
      if (parts == NULL || parts[0] == NULL || parts[1] == NULL)
        continue;

      if (g_strcmp0(parts[0], IMETA_URL) == 0 && out_url)
        *out_url = g_strdup(parts[1]);
      else if (g_strcmp0(parts[0], IMETA_NONCE) == 0 && out_nonce_b64)
        *out_nonce_b64 = g_strdup(parts[1]);
      else if (g_strcmp0(parts[0], IMETA_EPOCH) == 0 && out_epoch)
        *out_epoch = g_ascii_strtoull(parts[1], NULL, 10);
      else if (g_strcmp0(parts[0], IMETA_HASH) == 0 && out_hash)
        *out_hash = g_strdup(parts[1]);
    }

  return (out_url == NULL || *out_url != NULL);
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
  gchar              *nonce_b64;
  gchar              *hash_hex;
  gchar              *wire_hash;
  guint64             epoch;
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
  g_free(data->nonce_b64);
  g_free(data->hash_hex);
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

  /* Build imeta tags */
  GPtrArray *tags = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_add(tags, g_strdup("imeta"));
  g_ptr_array_add(tags, g_strdup_printf("%s %s", IMETA_URL, blob_url));
  g_ptr_array_add(tags, g_strdup_printf("%s %s", IMETA_NONCE, data->nonce_b64));
  g_ptr_array_add(tags, g_strdup_printf("%s %" G_GUINT64_FORMAT,
                                         IMETA_EPOCH, data->epoch));
  g_ptr_array_add(tags, g_strdup_printf("%s %s", IMETA_HASH, data->hash_hex));
  g_ptr_array_add(tags, g_strdup_printf("%s %s", IMETA_ENC, IMETA_ENC_VAL));
  g_ptr_array_add(tags, NULL);   /* NULL terminator */

  GnMlsMediaUploadResult *upload_result = g_new0(GnMlsMediaUploadResult, 1);
  upload_result->blossom_url = g_steal_pointer(&blob_url);
  upload_result->imeta_tags  = (gchar **)g_ptr_array_steal(tags, NULL);

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
  data->wire_hash = g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, data->ciphertext);
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

  /* Parse group ID hex to MarmotGroupId */
  size_t gid_len = strlen(data->group_id_hex) / 2;
  uint8_t *gid_bytes = g_malloc(gid_len);
  for (size_t i = 0; i < gid_len; i++)
    {
      unsigned int byte_val;
      sscanf(data->group_id_hex + (i * 2), "%02x", &byte_val);
      gid_bytes[i] = (uint8_t)byte_val;
    }
  MarmotGroupId mls_group_id = marmot_group_id_new(gid_bytes, gid_len);
  g_free(gid_bytes);

  /* Call libmarmot encryption */
  MarmotEncryptedMedia enc_result = {0};
  /* A Marmot is not thread-safe: hold the client's lock (W17b C1). */
  marmot_gobject_client_lock(client);
  MarmotError err = marmot_encrypt_media(
    marmot,
    &mls_group_id,
    plain_bytes, plain_len,
    data->content_type,
    data->filename,
    &enc_result);
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

  g_info("MlsMediaManager: MIP-04 encryption successful (epoch %" G_GUINT64_FORMAT ")",
         enc_result.imeta.epoch);

  /* Store encryption results */
  data->ciphertext = g_bytes_new_take(enc_result.encrypted_data, enc_result.encrypted_len);
  enc_result.encrypted_data = NULL;  /* ownership transferred */

  /* Convert nonce to base64 */
  data->nonce_b64 = g_base64_encode(enc_result.nonce, sizeof(enc_result.nonce));
  data->epoch = enc_result.imeta.epoch;

  /* Convert file hash to hex */
  GString *hash_str = g_string_sized_new(64);
  for (size_t i = 0; i < sizeof(enc_result.file_hash); i++)
    g_string_append_printf(hash_str, "%02x", enc_result.file_hash[i]);
  data->hash_hex = g_string_free(hash_str, FALSE);

  do_blossom_upload(data);
}

/* ── Download flow ───────────────────────────────────────────────── */

typedef struct
{
  GnMlsMediaManager *manager;       /* strong ref */
  GTask              *task;
  gchar              *group_id_hex;
  gchar              *nonce_b64;
  guint64             epoch;
} DownloadData;

static void
download_data_free(DownloadData *data)
{
  g_clear_object(&data->manager);
  g_free(data->group_id_hex);
  g_free(data->nonce_b64);
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

  /* Parse group ID hex to MarmotGroupId */
  size_t gid_len = strlen(data->group_id_hex) / 2;
  uint8_t *gid_bytes = g_malloc(gid_len);
  for (size_t i = 0; i < gid_len; i++)
    {
      unsigned int byte_val;
      sscanf(data->group_id_hex + (i * 2), "%02x", &byte_val);
      gid_bytes[i] = (uint8_t)byte_val;
    }
  MarmotGroupId mls_group_id = marmot_group_id_new(gid_bytes, gid_len);
  g_free(gid_bytes);

  /* Build imeta info for decryption */
  MarmotImetaInfo imeta = {0};
  imeta.epoch = data->epoch;

  /* Decode nonce from base64 */
  gsize nonce_len = 0;
  guchar *nonce_decoded = g_base64_decode(data->nonce_b64, &nonce_len);
  if (nonce_decoded != NULL && nonce_len == sizeof(imeta.nonce))
    memcpy(imeta.nonce, nonce_decoded, sizeof(imeta.nonce));
  g_free(nonce_decoded);

  /* Call libmarmot decryption */
  uint8_t *plaintext_out = NULL;
  size_t plaintext_len = 0;

  marmot_gobject_client_lock(client);   /* W17b C1 */
  MarmotError err = marmot_decrypt_media(
    marmot,
    &mls_group_id,
    cipher_data, cipher_len,
    &imeta,
    &plaintext_out, &plaintext_len);
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
                                      const gchar          *blossom_url,
                                      const gchar          *nonce_b64,
                                      guint64               epoch,
                                      GCancellable         *cancellable,
                                      GAsyncReadyCallback   callback,
                                      gpointer              user_data)
{
  g_return_if_fail(GN_IS_MLS_MEDIA_MANAGER(self));
  g_return_if_fail(group_id_hex != NULL);
  g_return_if_fail(blossom_url != NULL);

  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, gn_mls_media_manager_download_async);

  DownloadData *data = g_new0(DownloadData, 1);
  data->manager      = g_object_ref(self);
  data->task         = task;
  data->group_id_hex = g_strdup(group_id_hex);
  data->nonce_b64    = g_strdup(nonce_b64);
  data->epoch        = epoch;

  /* Blossom addresses are the wire SHA-256, optionally followed by an extension.
   * This legacy API carries no separate ciphertext hash. Refuse non-addressed
   * URLs rather than silently dropping the shared client's integrity check. */
  g_autoptr(GUri) uri = g_uri_parse(blossom_url, G_URI_FLAGS_ENCODED, NULL);
  const gchar *path = uri ? g_uri_get_path(uri) : NULL;
  const gchar *name = path ? strrchr(path, '/') : NULL;
  name = name ? name + 1 : NULL;
  gboolean valid = name && strlen(name) >= 64 && (name[64] == '.' || name[64] == '\0');
  for (guint i = 0; valid && i < 64; i++)
    valid = g_ascii_isxdigit(name[i]);
  if (!valid)
    {
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                             "Blossom URL has no ciphertext SHA-256 address");
      g_object_unref(task);
      download_data_free(data);
      return;
    }
  g_autofree gchar *wire_hash = g_ascii_strdown(name, 64);
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

  if (out_content_type != NULL)
    *out_content_type = NULL;

  return g_task_propagate_pointer(G_TASK(result), error);
}
