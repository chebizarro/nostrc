#define G_LOG_DOMAIN "gnostr-blossom"

/**
 * gnostr Blossom Service Implementation
 *
 * Blossom app policy and signer adapter over GnBlossomClient.
 */

#include "blossom.h"
#include "blossom_settings.h"
#include "../ipc/gnostr-signer-service.h"
#include <string.h>
#include <glib.h>
#include <json-glib/json-glib.h>

#include <nostr-gobject-1.0/gn-blossom-client.h>
#include <nostr/bud02/auth.h>
#include <nostr-event.h>
#include <nostr-tag.h>

/* For file hashing */
#include <gio/gio.h>

G_DEFINE_QUARK(gnostr-blossom-error-quark, gnostr_blossom_error)

void gnostr_blossom_blob_free(GnostrBlossomBlob *blob) {
  if (!blob) return;
  g_free(blob->sha256);
  g_free(blob->url);
  g_free(blob->mime_type);
  g_free(blob);
}

static void blob_array_free(GnostrBlossomBlob **blobs) {
  if (!blobs) return;
  for (int i = 0; blobs[i] != NULL; i++) {
    gnostr_blossom_blob_free(blobs[i]);
  }
  g_free(blobs);
}

gboolean gnostr_blossom_sha256_file(const char *file_path, char out_hash[65], GError **error) {
  if (!file_path || !out_hash) {
    g_set_error(error, GNOSTR_BLOSSOM_ERROR, GNOSTR_BLOSSOM_ERROR_FILE_READ,
                "Invalid arguments: file_path or out_hash is NULL");
    return FALSE;
  }

  g_autoptr(GFile) file = g_file_new_for_path(file_path);
  GError *local_error = NULL;
  GFileInputStream *stream = g_file_read(file, NULL, &local_error);

  if (!stream) {
    g_propagate_error(error, local_error);
    return FALSE;
  }

  GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
  guchar buffer[8192];
  gssize bytes_read;

  while ((bytes_read = g_input_stream_read(G_INPUT_STREAM(stream), buffer, sizeof(buffer), NULL, &local_error)) > 0) {
    g_checksum_update(checksum, buffer, bytes_read);
  }

  g_object_unref(stream);

  if (bytes_read < 0) {
    g_checksum_free(checksum);
    g_propagate_error(error, local_error);
    return FALSE;
  }

  const char *hash = g_checksum_get_string(checksum);
  strncpy(out_hash, hash, 64);
  out_hash[64] = '\0';

  g_checksum_free(checksum);
  return TRUE;
}

const char *gnostr_blossom_detect_mime_type(const char *file_path) {
  if (!file_path) return "application/octet-stream";

  const char *ext = strrchr(file_path, '.');
  if (!ext) return "application/octet-stream";

  ext++; /* Skip the dot */

  /* Common image types */
  if (g_ascii_strcasecmp(ext, "png") == 0) return "image/png";
  if (g_ascii_strcasecmp(ext, "jpg") == 0 || g_ascii_strcasecmp(ext, "jpeg") == 0) return "image/jpeg";
  if (g_ascii_strcasecmp(ext, "gif") == 0) return "image/gif";
  if (g_ascii_strcasecmp(ext, "webp") == 0) return "image/webp";
  if (g_ascii_strcasecmp(ext, "svg") == 0) return "image/svg+xml";
  if (g_ascii_strcasecmp(ext, "avif") == 0) return "image/avif";
  if (g_ascii_strcasecmp(ext, "ico") == 0) return "image/x-icon";
  if (g_ascii_strcasecmp(ext, "bmp") == 0) return "image/bmp";
  if (g_ascii_strcasecmp(ext, "tiff") == 0 || g_ascii_strcasecmp(ext, "tif") == 0) return "image/tiff";

  /* Video types */
  if (g_ascii_strcasecmp(ext, "mp4") == 0) return "video/mp4";
  if (g_ascii_strcasecmp(ext, "webm") == 0) return "video/webm";
  if (g_ascii_strcasecmp(ext, "mov") == 0) return "video/quicktime";
  if (g_ascii_strcasecmp(ext, "avi") == 0) return "video/x-msvideo";
  if (g_ascii_strcasecmp(ext, "mkv") == 0) return "video/x-matroska";

  /* Audio types */
  if (g_ascii_strcasecmp(ext, "mp3") == 0) return "audio/mpeg";
  if (g_ascii_strcasecmp(ext, "ogg") == 0) return "audio/ogg";
  if (g_ascii_strcasecmp(ext, "wav") == 0) return "audio/wav";
  if (g_ascii_strcasecmp(ext, "flac") == 0) return "audio/flac";

  return "application/octet-stream";
}

char *gnostr_blossom_build_auth_event(const char *action,
                                       const char *sha256,
                                       const char *server_url,
                                       gint64 file_size,
                                       const char *mime_type) {
  nostr_bud02_action_t bud_action;
  if (nostr_bud02_action_from_str(action, &bud_action) != NOSTR_BUD02_OK)
    return NULL;
  NostrEvent *event = nostr_bud02_create_auth_event(bud_action,
      sha256 && *sha256 ? sha256 : NULL, 0,
      server_url && *server_url ? server_url : NULL);
  if (!event) return NULL;
  /* Keep the app's optional metadata and signer-facing content. */
  nostr_event_set_content(event, "");
  if (file_size > 0) {
    g_autofree char *size = g_strdup_printf("%" G_GINT64_FORMAT, file_size);
    nostr_tags_append(event->tags, nostr_tag_new("size", size, NULL));
  }
  if (mime_type && *mime_type)
    nostr_tags_append(event->tags, nostr_tag_new("type", mime_type, NULL));
  char *json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  return json;
}

static GError *map_transfer_error(GError *error, GnostrBlossomError code) {
  if (error && (error->domain == GN_BLOSSOM_ERROR ||
                g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))) {
    GError *mapped = g_error_new_literal(GNOSTR_BLOSSOM_ERROR, code, error->message);
    g_error_free(error);
    return mapped;
  }
  return error; /* Preserve GIO transport errors, especially cancellation. */
}

static GnBlossomClient *new_client(const char *server, guint timeout) {
  GnBlossomClient *client = gn_blossom_client_new(server);
#ifdef GNOSTR_BLOSSOM_LOCAL_TEST
  /* Compiled only into the in-process transport fixture, never the app. */
  gn_blossom_client_set_network(client, NULL, GN_BLOSSOM_NETWORK_LOCAL_TEST);
#else
  gn_blossom_client_set_network(client, g_proxy_resolver_get_default(), GN_BLOSSOM_NETWORK_DEFAULT);
#endif
  gn_blossom_client_set_limits(client, 0, timeout, NULL);
  return client;
}

/* Upload context */
typedef struct {
  char *server_url;
  char *file_path;
  char *mime_type;
  char *sha256;
  gint64 file_size;
  GBytes *file_data;
  GnostrBlossomUploadCallback callback;
  gpointer user_data;
  GCancellable *cancellable;
  GnBlossomClient *client;
  char *auth_event_json;  /* Unsigned auth event for signing */
} BlossomUploadContext;

static void upload_ctx_free(BlossomUploadContext *ctx) {
  if (!ctx) return;
  g_free(ctx->server_url);
  g_free(ctx->file_path);
  g_free(ctx->mime_type);
  g_free(ctx->sha256);
  g_free(ctx->auth_event_json);
  if (ctx->file_data) g_bytes_unref(ctx->file_data);
  if (ctx->cancellable) g_object_unref(ctx->cancellable);
  g_clear_object(&ctx->client);
  g_free(ctx);
}

static void on_upload_response(GObject *source, GAsyncResult *res, gpointer user_data) {
  BlossomUploadContext *ctx = (BlossomUploadContext *)user_data;
  GError *error = NULL;

  char *url = gn_blossom_client_upload_finish(GN_BLOSSOM_CLIENT(source), res, &error);
  error = map_transfer_error(error, GNOSTR_BLOSSOM_ERROR_UPLOAD_FAILED);
  if (!url) {
    if (ctx->callback) ctx->callback(NULL, error, ctx->user_data);
    g_clear_error(&error);
    upload_ctx_free(ctx);
    return;
  }

  GnostrBlossomBlob *blob = g_new0(GnostrBlossomBlob, 1);
  blob->sha256 = g_strdup(ctx->sha256);
  blob->size = ctx->file_size;
  blob->mime_type = g_strdup(ctx->mime_type);

  blob->url = url;

  /* Construct URL if not provided by server */
  if (!blob->url || !*blob->url) {
    g_free(blob->url);
    /* Extensions follow the original media type; the wire hash is verified. */
    const char *ext = "";
    const char *orig_mime = ctx->mime_type;
    if (orig_mime) {
      if (g_str_has_suffix(orig_mime, "png")) ext = ".png";
      else if (g_str_has_suffix(orig_mime, "jpeg")) ext = ".jpg";
      else if (g_str_has_suffix(orig_mime, "gif")) ext = ".gif";
      else if (g_str_has_suffix(orig_mime, "webp")) ext = ".webp";
      else if (g_str_has_suffix(orig_mime, "svg+xml")) ext = ".svg";
      else if (g_str_has_suffix(orig_mime, "avif")) ext = ".avif";
      else if (g_str_has_suffix(orig_mime, "mp4")) ext = ".mp4";
      else if (g_str_has_suffix(orig_mime, "webm")) ext = ".webm";
      else if (g_str_has_suffix(orig_mime, "quicktime")) ext = ".mov";
    }
    blob->url = g_strdup_printf("%s/%s%s", ctx->server_url, blob->sha256, ext);
  }

  if (ctx->callback) {
    ctx->callback(blob, NULL, ctx->user_data);
  } else {
    gnostr_blossom_blob_free(blob);
  }

  upload_ctx_free(ctx);
}

static void on_upload_sign_complete(GObject *source, GAsyncResult *res, gpointer user_data) {
  BlossomUploadContext *ctx = (BlossomUploadContext *)user_data;
  (void)source;
  if (!ctx) return;

  GError *error = NULL;
  char *signed_event_json = NULL;
  gboolean ok = gnostr_sign_event_finish(res, &signed_event_json, &error);

  if (!ok || !signed_event_json) {
    g_free(signed_event_json);
    GError *err = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)
      ? g_error_copy(error) : g_error_new(GNOSTR_BLOSSOM_ERROR,
                               GNOSTR_BLOSSOM_ERROR_SIGNING_FAILED,
                               "Failed to sign auth event: %s",
                               error ? error->message : "unknown");
    g_clear_error(&error);
    if (ctx->callback) {
      ctx->callback(NULL, err, ctx->user_data);
    }
    g_error_free(err);
    upload_ctx_free(ctx);
    return;
  }

  gn_blossom_client_upload_async(ctx->client, ctx->file_data, ctx->sha256,
      ctx->mime_type, signed_event_json, ctx->cancellable, on_upload_response, ctx);
  g_free(signed_event_json);
}

void gnostr_blossom_upload_async(const char *server_url,
                                  const char *file_path,
                                  const char *mime_type,
                                  GnostrBlossomUploadCallback callback,
                                  gpointer user_data,
                                  GCancellable *cancellable) {
  if (!server_url || !file_path) {
    GError *err = g_error_new(GNOSTR_BLOSSOM_ERROR,
                               GNOSTR_BLOSSOM_ERROR_FILE_NOT_FOUND,
                               "Invalid server URL or file path");
    if (callback) callback(NULL, err, user_data);
    g_error_free(err);
    return;
  }

  /* Check if signer service is available */
  GnostrSignerService *signer = gnostr_signer_service_get_default();
  if (!gnostr_signer_service_is_available(signer)) {
    GError *err = g_error_new(GNOSTR_BLOSSOM_ERROR,
                               GNOSTR_BLOSSOM_ERROR_SIGNING_FAILED,
                               "Signer not available");
    if (callback) callback(NULL, err, user_data);
    g_error_free(err);
    return;
  }

  /* Read file */
  g_autoptr(GFile) file = g_file_new_for_path(file_path);
  GError *error = NULL;
  char *contents = NULL;
  gsize length = 0;

  if (!g_file_load_contents(file, cancellable, &contents, &length, NULL, &error)) {
    GError *err = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)
      ? g_error_copy(error) : g_error_new(GNOSTR_BLOSSOM_ERROR,
                               GNOSTR_BLOSSOM_ERROR_FILE_READ,
                               "Failed to read file: %s", error->message);
    g_clear_error(&error);
    if (callback) callback(NULL, err, user_data);
    g_error_free(err);
    return;
  }

  /* Address the exact bytes read, not a second potentially changed file. */
  g_autofree char *sha256 = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
      (const guchar *)contents, length);

  /* Detect MIME type if not provided */
  const char *detected_mime = mime_type ? mime_type : gnostr_blossom_detect_mime_type(file_path);

  /* Create context */
  BlossomUploadContext *ctx = g_new0(BlossomUploadContext, 1);
  ctx->server_url = g_strdup(server_url);
  ctx->file_path = g_strdup(file_path);
  ctx->mime_type = g_strdup(detected_mime);
  ctx->sha256 = g_strdup(sha256);
  ctx->file_size = (gint64)length;
  ctx->file_data = g_bytes_new_take(contents, length);
  ctx->callback = callback;
  ctx->user_data = user_data;
  ctx->cancellable = cancellable ? g_object_ref(cancellable) : NULL;
  ctx->client = new_client(server_url, 60);
  /* The app already holds these bytes; do not impose the download default on uploads. */
  gn_blossom_client_set_limits(ctx->client, MAX(length, 1), 60, NULL);

  /* Build auth event (unsigned) */
  ctx->auth_event_json = gnostr_blossom_build_auth_event("upload", sha256, server_url,
                                                          ctx->file_size, ctx->mime_type);

  /* Sign event asynchronously using unified signer service */
  gnostr_sign_event_async(
    ctx->auth_event_json,
    "",  /* current_user: ignored */
    "",  /* app_id: ignored */
    ctx->cancellable,
    on_upload_sign_complete,
    ctx
  );
}

/* List context */
typedef struct {
  char *server_url;
  char *pubkey_hex;
  GnostrBlossomListCallback callback;
  gpointer user_data;
  GCancellable *cancellable;
  GnBlossomClient *client;
} BlossomListContext;

static void list_ctx_free(BlossomListContext *ctx) {
  if (!ctx) return;
  g_free(ctx->server_url);
  g_free(ctx->pubkey_hex);
  if (ctx->cancellable) g_object_unref(ctx->cancellable);
  g_clear_object(&ctx->client);
  g_free(ctx);
}

static void on_list_response(GObject *source, GAsyncResult *res, gpointer user_data) {
  BlossomListContext *ctx = (BlossomListContext *)user_data;
  GError *error = NULL;

  GBytes *bytes = gn_blossom_client_list_finish(GN_BLOSSOM_CLIENT(source), res, &error);
  error = map_transfer_error(error, GNOSTR_BLOSSOM_ERROR_SERVER_ERROR);
  if (!bytes) {
    if (ctx->callback) ctx->callback(NULL, error, ctx->user_data);
    g_clear_error(&error);
    list_ctx_free(ctx);
    return;
  }

  /* Parse JSON array response */
  gsize len = 0;
  const char *data = g_bytes_get_data(bytes, &len);

  GPtrArray *blobs_arr = g_ptr_array_new();

  if (data && len > 0) {
    g_autoptr(JsonParser) parser = json_parser_new();
    if (json_parser_load_from_data(parser, data, len, NULL)) {
      JsonNode *root = json_parser_get_root(parser);
      if (root && JSON_NODE_HOLDS_ARRAY(root)) {
        JsonArray *arr = json_node_get_array(root);
        guint arr_len = json_array_get_length(arr);

        for (guint i = 0; i < arr_len; i++) {
          JsonObject *obj = json_array_get_object_element(arr, i);
          if (!obj) continue;

          GnostrBlossomBlob *blob = g_new0(GnostrBlossomBlob, 1);

          if (json_object_has_member(obj, "sha256")) {
            blob->sha256 = g_strdup(json_object_get_string_member(obj, "sha256"));
          }
          if (json_object_has_member(obj, "url")) {
            blob->url = g_strdup(json_object_get_string_member(obj, "url"));
          }
          if (json_object_has_member(obj, "type")) {
            blob->mime_type = g_strdup(json_object_get_string_member(obj, "type"));
          }
          if (json_object_has_member(obj, "size")) {
            blob->size = json_object_get_int_member(obj, "size");
          }

          g_ptr_array_add(blobs_arr, blob);
        }
      }
    }
  }

  g_bytes_unref(bytes);

  /* Convert to NULL-terminated array */
  g_ptr_array_add(blobs_arr, NULL);
  GnostrBlossomBlob **blobs = (GnostrBlossomBlob **)g_ptr_array_free(blobs_arr, FALSE);

  if (ctx->callback) {
    ctx->callback(blobs, NULL, ctx->user_data);
  } else {
    blob_array_free(blobs);
  }

  list_ctx_free(ctx);
}

void gnostr_blossom_list_async(const char *server_url,
                                const char *pubkey_hex,
                                GnostrBlossomListCallback callback,
                                gpointer user_data,
                                GCancellable *cancellable) {
  if (!server_url || !pubkey_hex) {
    GError *err = g_error_new(GNOSTR_BLOSSOM_ERROR,
                               GNOSTR_BLOSSOM_ERROR_PARSE_ERROR,
                               "Invalid server URL or pubkey");
    if (callback) callback(NULL, err, user_data);
    g_error_free(err);
    return;
  }

  BlossomListContext *ctx = g_new0(BlossomListContext, 1);
  ctx->server_url = g_strdup(server_url);
  ctx->pubkey_hex = g_strdup(pubkey_hex);
  ctx->callback = callback;
  ctx->user_data = user_data;
  ctx->cancellable = cancellable ? g_object_ref(cancellable) : NULL;
  ctx->client = new_client(server_url, 30);
  gn_blossom_client_list_async(ctx->client, pubkey_hex, ctx->cancellable,
      on_list_response, ctx);
}

/* Delete context */
typedef struct {
  char *server_url;
  char *sha256;
  char *auth_event_json;
  GnostrBlossomDeleteCallback callback;
  gpointer user_data;
  GCancellable *cancellable;
  GnBlossomClient *client;
} BlossomDeleteContext;

static void delete_ctx_free(BlossomDeleteContext *ctx) {
  if (!ctx) return;
  g_free(ctx->server_url);
  g_free(ctx->sha256);
  g_free(ctx->auth_event_json);
  if (ctx->cancellable) g_object_unref(ctx->cancellable);
  g_clear_object(&ctx->client);
  g_free(ctx);
}

static void on_delete_response(GObject *source, GAsyncResult *res, gpointer user_data) {
  BlossomDeleteContext *ctx = (BlossomDeleteContext *)user_data;
  GError *error = NULL;

  gboolean success = gn_blossom_client_delete_finish(GN_BLOSSOM_CLIENT(source), res, &error);
  error = map_transfer_error(error, GNOSTR_BLOSSOM_ERROR_SERVER_ERROR);
  if (ctx->callback) ctx->callback(success, error, ctx->user_data);
  g_clear_error(&error);
  delete_ctx_free(ctx);
}

static void on_delete_sign_complete(GObject *source, GAsyncResult *res, gpointer user_data) {
  BlossomDeleteContext *ctx = (BlossomDeleteContext *)user_data;
  (void)source;
  if (!ctx) return;

  GError *error = NULL;
  char *signed_event_json = NULL;
  gboolean ok = gnostr_sign_event_finish(res, &signed_event_json, &error);

  if (!ok || !signed_event_json) {
    g_free(signed_event_json);
    GError *err = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)
      ? g_error_copy(error) : g_error_new(GNOSTR_BLOSSOM_ERROR,
                               GNOSTR_BLOSSOM_ERROR_SIGNING_FAILED,
                               "Failed to sign auth event: %s",
                               error ? error->message : "unknown");
    g_clear_error(&error);
    if (ctx->callback) {
      ctx->callback(FALSE, err, ctx->user_data);
    }
    g_error_free(err);
    delete_ctx_free(ctx);
    return;
  }

  gn_blossom_client_delete_async(ctx->client, ctx->sha256, signed_event_json,
      ctx->cancellable, on_delete_response, ctx);
  g_free(signed_event_json);
}

void gnostr_blossom_delete_async(const char *server_url,
                                  const char *sha256,
                                  GnostrBlossomDeleteCallback callback,
                                  gpointer user_data,
                                  GCancellable *cancellable) {
  if (!server_url || !sha256) {
    GError *err = g_error_new(GNOSTR_BLOSSOM_ERROR,
                               GNOSTR_BLOSSOM_ERROR_NOT_FOUND,
                               "Invalid server URL or hash");
    if (callback) callback(FALSE, err, user_data);
    g_error_free(err);
    return;
  }

  /* Check if signer service is available */
  GnostrSignerService *signer = gnostr_signer_service_get_default();
  if (!gnostr_signer_service_is_available(signer)) {
    GError *err = g_error_new(GNOSTR_BLOSSOM_ERROR,
                               GNOSTR_BLOSSOM_ERROR_SIGNING_FAILED,
                               "Signer not available");
    if (callback) callback(FALSE, err, user_data);
    g_error_free(err);
    return;
  }

  BlossomDeleteContext *ctx = g_new0(BlossomDeleteContext, 1);
  ctx->server_url = g_strdup(server_url);
  ctx->sha256 = g_strdup(sha256);
  ctx->callback = callback;
  ctx->user_data = user_data;
  ctx->cancellable = cancellable ? g_object_ref(cancellable) : NULL;
  ctx->client = new_client(server_url, 30);

  /* Build auth event for delete (unsigned) */
  ctx->auth_event_json = gnostr_blossom_build_auth_event("delete", sha256, server_url, 0, NULL);

  /* Sign event asynchronously using unified signer service */
  gnostr_sign_event_async(
    ctx->auth_event_json,
    "",  /* current_user: ignored */
    "",  /* app_id: ignored */
    ctx->cancellable,
    on_delete_sign_complete,
    ctx
  );
}

/* ---- Upload with fallback ---- */

typedef struct {
  char *file_path;
  char *mime_type;
  const char **server_urls;  /* owned array of borrowed strings */
  gsize n_servers;
  gsize current_index;
  GnostrBlossomUploadCallback callback;
  gpointer user_data;
  GCancellable *cancellable;
  GPtrArray *errors;         /* Accumulated error messages */
} FallbackUploadContext;

static void fallback_ctx_free(FallbackUploadContext *ctx) {
  if (!ctx) return;
  g_free(ctx->file_path);
  g_free(ctx->mime_type);
  g_free(ctx->server_urls);
  if (ctx->errors) {
    g_ptr_array_free(ctx->errors, TRUE);
  }
  if (ctx->cancellable) {
    g_object_unref(ctx->cancellable);
  }
  g_free(ctx);
}

static void try_next_server(FallbackUploadContext *ctx);

static void on_fallback_upload_complete(GnostrBlossomBlob *blob, GError *error, gpointer user_data) {
  FallbackUploadContext *ctx = (FallbackUploadContext *)user_data;

  if (blob && !error) {
    /* Success! Return the blob */
    if (ctx->callback) {
      ctx->callback(blob, NULL, ctx->user_data);
    } else {
      gnostr_blossom_blob_free(blob);
    }
    fallback_ctx_free(ctx);
    return;
  }

  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    if (ctx->callback) ctx->callback(NULL, error, ctx->user_data);
    fallback_ctx_free(ctx);
    return;
  }

  /* Upload failed - record error and try next server */
  if (error) {
    char *err_msg = g_strdup_printf("Server %s: %s",
                                     ctx->server_urls[ctx->current_index],
                                     error->message);
    g_ptr_array_add(ctx->errors, err_msg);
    g_debug("Blossom upload to %s failed: %s",
            ctx->server_urls[ctx->current_index], error->message);
  }

  ctx->current_index++;

  if (ctx->current_index < ctx->n_servers) {
    /* Try next server */
    try_next_server(ctx);
  } else {
    /* All servers failed */
    GString *combined = g_string_new("All Blossom servers failed:\n");
    for (guint i = 0; i < ctx->errors->len; i++) {
      g_string_append_printf(combined, "  - %s\n", (char *)g_ptr_array_index(ctx->errors, i));
    }

    GError *final_err = g_error_new(GNOSTR_BLOSSOM_ERROR,
                                     GNOSTR_BLOSSOM_ERROR_ALL_SERVERS_FAILED,
                                     "%s", combined->str);
    g_string_free(combined, TRUE);

    if (ctx->callback) {
      ctx->callback(NULL, final_err, ctx->user_data);
    }
    g_error_free(final_err);
    fallback_ctx_free(ctx);
  }
}

static void try_next_server(FallbackUploadContext *ctx) {
  if (ctx->current_index >= ctx->n_servers) {
    /* Should not happen, but handle gracefully */
    GError *err = g_error_new(GNOSTR_BLOSSOM_ERROR,
                               GNOSTR_BLOSSOM_ERROR_NO_SERVERS,
                               "No more servers to try");
    if (ctx->callback) {
      ctx->callback(NULL, err, ctx->user_data);
    }
    g_error_free(err);
    fallback_ctx_free(ctx);
    return;
  }

  const char *server_url = ctx->server_urls[ctx->current_index];
  g_debug("Blossom: trying upload to server %zu/%zu: %s",
          ctx->current_index + 1, ctx->n_servers, server_url);

  gnostr_blossom_upload_async(server_url, ctx->file_path, ctx->mime_type,
                               on_fallback_upload_complete, ctx,
                               ctx->cancellable);
}

void gnostr_blossom_upload_with_fallback_async(const char *file_path,
                                                 const char *mime_type,
                                                 GnostrBlossomUploadCallback callback,
                                                 gpointer user_data,
                                                 GCancellable *cancellable) {
  if (!file_path) {
    GError *err = g_error_new(GNOSTR_BLOSSOM_ERROR,
                               GNOSTR_BLOSSOM_ERROR_FILE_NOT_FOUND,
                               "No file path provided");
    if (callback) callback(NULL, err, user_data);
    g_error_free(err);
    return;
  }

  /* Get server list from settings */
  gsize n_servers = 0;
  const char **server_urls = gnostr_blossom_settings_get_enabled_urls(&n_servers);

  if (!server_urls || n_servers == 0) {
    g_free(server_urls);
    GError *err = g_error_new(GNOSTR_BLOSSOM_ERROR,
                               GNOSTR_BLOSSOM_ERROR_NO_SERVERS,
                               "No Blossom servers configured");
    if (callback) callback(NULL, err, user_data);
    g_error_free(err);
    return;
  }

  /* Create fallback context */
  FallbackUploadContext *ctx = g_new0(FallbackUploadContext, 1);
  ctx->file_path = g_strdup(file_path);
  ctx->mime_type = mime_type ? g_strdup(mime_type) : NULL;
  ctx->server_urls = server_urls;
  ctx->n_servers = n_servers;
  ctx->current_index = 0;
  ctx->callback = callback;
  ctx->user_data = user_data;
  ctx->cancellable = cancellable ? g_object_ref(cancellable) : NULL;
  ctx->errors = g_ptr_array_new_with_free_func(g_free);

  /* Start with first server */
  try_next_server(ctx);
}
