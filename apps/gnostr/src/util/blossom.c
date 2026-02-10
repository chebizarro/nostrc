#define G_LOG_DOMAIN "gnostr-blossom"

/**
 * gnostr Blossom Service Implementation
 *
 * BUD-01 Blossom media server client using libsoup for HTTP.
 */

#include "blossom.h"
#include "blossom_settings.h"
#include "../ipc/gnostr-signer-service.h"
#include <string.h>
#include <glib.h>
#include <json-glib/json-glib.h>

#ifdef HAVE_SOUP3
#include <libsoup/soup.h>
#endif

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

  GFile *file = g_file_new_for_path(file_path);
  GError *local_error = NULL;
  GFileInputStream *stream = g_file_read(file, NULL, &local_error);
  g_object_unref(file);

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
  JsonBuilder *builder = json_builder_new();

  json_builder_begin_object(builder);

  /* Kind 24242 for Blossom auth */
  json_builder_set_member_name(builder, "kind");
  json_builder_add_int_value(builder, 24242);

  /* Created at (current time) */
  json_builder_set_member_name(builder, "created_at");
  json_builder_add_int_value(builder, (gint64)g_get_real_time() / G_USEC_PER_SEC);

  /* Content is empty per BUD-01 */
  json_builder_set_member_name(builder, "content");
  json_builder_add_string_value(builder, "");

  /* Tags array */
  json_builder_set_member_name(builder, "tags");
  json_builder_begin_array(builder);

  /* t tag: action type */
  json_builder_begin_array(builder);
  json_builder_add_string_value(builder, "t");
  json_builder_add_string_value(builder, action);
  json_builder_end_array(builder);

  /* x tag: file hash (for upload/delete) */
  if (sha256 && *sha256) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "x");
    json_builder_add_string_value(builder, sha256);
    json_builder_end_array(builder);
  }

  /* server tag: server URL */
  if (server_url && *server_url) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "server");
    json_builder_add_string_value(builder, server_url);
    json_builder_end_array(builder);
  }

  /* size tag: file size (upload only) */
  if (file_size > 0) {
    char size_str[32];
    g_snprintf(size_str, sizeof(size_str), "%" G_GINT64_FORMAT, file_size);
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "size");
    json_builder_add_string_value(builder, size_str);
    json_builder_end_array(builder);
  }

  /* type tag: MIME type (upload only) */
  if (mime_type && *mime_type) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "type");
    json_builder_add_string_value(builder, mime_type);
    json_builder_end_array(builder);
  }

  /* expiration tag: valid for 5 minutes */
  gint64 expiration = (g_get_real_time() / G_USEC_PER_SEC) + 300;
  char exp_str[32];
  g_snprintf(exp_str, sizeof(exp_str), "%" G_GINT64_FORMAT, expiration);
  json_builder_begin_array(builder);
  json_builder_add_string_value(builder, "expiration");
  json_builder_add_string_value(builder, exp_str);
  json_builder_end_array(builder);

  json_builder_end_array(builder); /* End tags */

  json_builder_end_object(builder);

  JsonGenerator *gen = json_generator_new();
  json_generator_set_root(gen, json_builder_get_root(builder));
  char *json_str = json_generator_to_data(gen, NULL);

  g_object_unref(gen);
  g_object_unref(builder);

  return json_str;
}

#ifdef HAVE_SOUP3

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
  SoupSession *session;
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
  if (ctx->session) g_object_unref(ctx->session);
  g_free(ctx);
}

static void on_upload_response(GObject *source, GAsyncResult *res, gpointer user_data) {
  BlossomUploadContext *ctx = (BlossomUploadContext *)user_data;
  GError *error = NULL;

  GBytes *bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), res, &error);

  if (error) {
    if (ctx->callback) {
      ctx->callback(NULL, error, ctx->user_data);
    }
    g_clear_error(&error);
    upload_ctx_free(ctx);
    return;
  }

  /* Check response status from the message */
  SoupMessage *msg = soup_session_get_async_result_message(SOUP_SESSION(source), res);
  guint status = soup_message_get_status(msg);

  if (status < 200 || status >= 300) {
    GError *err = g_error_new(GNOSTR_BLOSSOM_ERROR,
                               GNOSTR_BLOSSOM_ERROR_UPLOAD_FAILED,
                               "Upload failed with status %u", status);
    if (ctx->callback) {
      ctx->callback(NULL, err, ctx->user_data);
    }
    g_error_free(err);
    if (bytes) g_bytes_unref(bytes);
    upload_ctx_free(ctx);
    return;
  }

  /* Parse response JSON to get blob info */
  gsize len = 0;
  const char *data = g_bytes_get_data(bytes, &len);

  GnostrBlossomBlob *blob = g_new0(GnostrBlossomBlob, 1);
  blob->sha256 = g_strdup(ctx->sha256);
  blob->size = ctx->file_size;
  blob->mime_type = g_strdup(ctx->mime_type);

  /* Try to parse server response for URL */
  if (data && len > 0) {
    JsonParser *parser = json_parser_new();
    if (json_parser_load_from_data(parser, data, len, NULL)) {
      JsonNode *root = json_parser_get_root(parser);
      if (root && JSON_NODE_HOLDS_OBJECT(root)) {
        JsonObject *obj = json_node_get_object(root);
        if (json_object_has_member(obj, "url")) {
          blob->url = g_strdup(json_object_get_string_member(obj, "url"));
        }
        if (json_object_has_member(obj, "sha256")) {
          g_free(blob->sha256);
          blob->sha256 = g_strdup(json_object_get_string_member(obj, "sha256"));
        }
        if (json_object_has_member(obj, "type")) {
          g_free(blob->mime_type);
          blob->mime_type = g_strdup(json_object_get_string_member(obj, "type"));
        }
        if (json_object_has_member(obj, "size")) {
          blob->size = json_object_get_int_member(obj, "size");
        }
      }
    }
    g_object_unref(parser);
  }

  /* Construct URL if not provided by server */
  if (!blob->url || !*blob->url) {
    g_free(blob->url);
    /* Use the ORIGINAL mime type (ctx->mime_type) for the extension, not
     * blob->mime_type which may have been overwritten by the server response
     * (e.g. server converts jpg→webp and reports type:"image/webp").
     * If we used the server type, ALL uploads get .webp regardless of the
     * original file type. */
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

  g_bytes_unref(bytes);

  if (ctx->callback) {
    ctx->callback(blob, NULL, ctx->user_data);
  } else {
    gnostr_blossom_blob_free(blob);
  }

  upload_ctx_free(ctx);
}

static void upload_with_auth(BlossomUploadContext *ctx, const char *auth_header) {
  /* Build upload URL */
  char *url = g_strdup_printf("%s/upload", ctx->server_url);

  SoupMessage *msg = soup_message_new("PUT", url);
  g_free(url);

  if (!msg) {
    GError *err = g_error_new(GNOSTR_BLOSSOM_ERROR,
                               GNOSTR_BLOSSOM_ERROR_UPLOAD_FAILED,
                               "Failed to create upload request");
    if (ctx->callback) {
      ctx->callback(NULL, err, ctx->user_data);
    }
    g_error_free(err);
    upload_ctx_free(ctx);
    return;
  }

  /* Set headers */
  SoupMessageHeaders *headers = soup_message_get_request_headers(msg);
  soup_message_headers_append(headers, "Authorization", auth_header);
  soup_message_headers_append(headers, "Content-Type", ctx->mime_type);

  /* Set body */
  soup_message_set_request_body_from_bytes(msg, ctx->mime_type, ctx->file_data);

  /* Send async */
  soup_session_send_and_read_async(ctx->session, msg, G_PRIORITY_DEFAULT,
                                    ctx->cancellable, on_upload_response, ctx);
  g_object_unref(msg);
}

/**
 * Callback when unified signer returns signed event for upload.
 * The signed event contains id, pubkey, and sig - we just base64-encode it.
 */
static void on_upload_sign_complete(GObject *source, GAsyncResult *res, gpointer user_data) {
  BlossomUploadContext *ctx = (BlossomUploadContext *)user_data;
  (void)source;
  if (!ctx) return;

  GError *error = NULL;
  char *signed_event_json = NULL;
  gboolean ok = gnostr_sign_event_finish(res, &signed_event_json, &error);

  if (!ok || !signed_event_json) {
    GError *err = g_error_new(GNOSTR_BLOSSOM_ERROR,
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

  /* Base64 encode the signed event for Authorization header */
  gchar *base64 = g_base64_encode((const guchar *)signed_event_json, strlen(signed_event_json));
  g_free(signed_event_json);

  /* Build auth header: "Nostr <base64-encoded-signed-event>" per BUD-01 */
  char *auth_header = g_strdup_printf("Nostr %s", base64);
  g_free(base64);

  /* Proceed with upload */
  upload_with_auth(ctx, auth_header);
  g_free(auth_header);
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
  GFile *file = g_file_new_for_path(file_path);
  GError *error = NULL;
  char *contents = NULL;
  gsize length = 0;

  if (!g_file_load_contents(file, NULL, &contents, &length, NULL, &error)) {
    g_object_unref(file);
    GError *err = g_error_new(GNOSTR_BLOSSOM_ERROR,
                               GNOSTR_BLOSSOM_ERROR_FILE_READ,
                               "Failed to read file: %s", error->message);
    g_clear_error(&error);
    if (callback) callback(NULL, err, user_data);
    g_error_free(err);
    return;
  }
  g_object_unref(file);

  /* Compute hash */
  char sha256[65];
  GError *hash_error = NULL;
  if (!gnostr_blossom_sha256_file(file_path, sha256, &hash_error)) {
    g_free(contents);
    GError *err = g_error_new(GNOSTR_BLOSSOM_ERROR,
                               GNOSTR_BLOSSOM_ERROR_FILE_READ,
                               "Failed to compute file hash: %s",
                               hash_error ? hash_error->message : "unknown error");
    g_clear_error(&hash_error);
    if (callback) callback(NULL, err, user_data);
    g_error_free(err);
    return;
  }

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
  ctx->session = soup_session_new();
  soup_session_set_timeout(ctx->session, 60); /* 60 second timeout for uploads */

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
  SoupSession *session;
} BlossomListContext;

static void list_ctx_free(BlossomListContext *ctx) {
  if (!ctx) return;
  g_free(ctx->server_url);
  g_free(ctx->pubkey_hex);
  if (ctx->cancellable) g_object_unref(ctx->cancellable);
  if (ctx->session) g_object_unref(ctx->session);
  g_free(ctx);
}

static void on_list_response(GObject *source, GAsyncResult *res, gpointer user_data) {
  BlossomListContext *ctx = (BlossomListContext *)user_data;
  GError *error = NULL;

  GBytes *bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), res, &error);

  if (error) {
    if (ctx->callback) {
      ctx->callback(NULL, error, ctx->user_data);
    }
    g_clear_error(&error);
    list_ctx_free(ctx);
    return;
  }

  SoupMessage *msg = soup_session_get_async_result_message(SOUP_SESSION(source), res);
  guint status = soup_message_get_status(msg);

  if (status < 200 || status >= 300) {
    GError *err = g_error_new(GNOSTR_BLOSSOM_ERROR,
                               GNOSTR_BLOSSOM_ERROR_SERVER_ERROR,
                               "List failed with status %u", status);
    if (ctx->callback) {
      ctx->callback(NULL, err, ctx->user_data);
    }
    g_error_free(err);
    if (bytes) g_bytes_unref(bytes);
    list_ctx_free(ctx);
    return;
  }

  /* Parse JSON array response */
  gsize len = 0;
  const char *data = g_bytes_get_data(bytes, &len);

  GPtrArray *blobs_arr = g_ptr_array_new();

  if (data && len > 0) {
    JsonParser *parser = json_parser_new();
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
    g_object_unref(parser);
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
  ctx->session = soup_session_new();
  soup_session_set_timeout(ctx->session, 30);

  /* Build list URL */
  char *url = g_strdup_printf("%s/list/%s", server_url, pubkey_hex);

  SoupMessage *msg = soup_message_new("GET", url);
  g_free(url);

  if (!msg) {
    GError *err = g_error_new(GNOSTR_BLOSSOM_ERROR,
                               GNOSTR_BLOSSOM_ERROR_PARSE_ERROR,
                               "Failed to create list request");
    if (callback) callback(NULL, err, user_data);
    g_error_free(err);
    list_ctx_free(ctx);
    return;
  }

  soup_session_send_and_read_async(ctx->session, msg, G_PRIORITY_DEFAULT,
                                    ctx->cancellable, on_list_response, ctx);
  g_object_unref(msg);
}

/* Delete context */
typedef struct {
  char *server_url;
  char *sha256;
  char *auth_event_json;
  GnostrBlossomDeleteCallback callback;
  gpointer user_data;
  GCancellable *cancellable;
  SoupSession *session;
} BlossomDeleteContext;

static void delete_ctx_free(BlossomDeleteContext *ctx) {
  if (!ctx) return;
  g_free(ctx->server_url);
  g_free(ctx->sha256);
  g_free(ctx->auth_event_json);
  if (ctx->cancellable) g_object_unref(ctx->cancellable);
  if (ctx->session) g_object_unref(ctx->session);
  g_free(ctx);
}

static void on_delete_response(GObject *source, GAsyncResult *res, gpointer user_data) {
  BlossomDeleteContext *ctx = (BlossomDeleteContext *)user_data;
  GError *error = NULL;

  GBytes *bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), res, &error);

  if (error) {
    if (ctx->callback) {
      ctx->callback(FALSE, error, ctx->user_data);
    }
    g_clear_error(&error);
    delete_ctx_free(ctx);
    return;
  }

  SoupMessage *msg = soup_session_get_async_result_message(SOUP_SESSION(source), res);
  guint status = soup_message_get_status(msg);

  if (bytes) g_bytes_unref(bytes);

  gboolean success = (status >= 200 && status < 300);

  if (!success) {
    GError *err = g_error_new(GNOSTR_BLOSSOM_ERROR,
                               GNOSTR_BLOSSOM_ERROR_SERVER_ERROR,
                               "Delete failed with status %u", status);
    if (ctx->callback) {
      ctx->callback(FALSE, err, ctx->user_data);
    }
    g_error_free(err);
  } else {
    if (ctx->callback) {
      ctx->callback(TRUE, NULL, ctx->user_data);
    }
  }

  delete_ctx_free(ctx);
}

static void delete_with_auth(BlossomDeleteContext *ctx, const char *auth_header) {
  char *url = g_strdup_printf("%s/%s", ctx->server_url, ctx->sha256);

  SoupMessage *msg = soup_message_new("DELETE", url);
  g_free(url);

  if (!msg) {
    GError *err = g_error_new(GNOSTR_BLOSSOM_ERROR,
                               GNOSTR_BLOSSOM_ERROR_PARSE_ERROR,
                               "Failed to create delete request");
    if (ctx->callback) {
      ctx->callback(FALSE, err, ctx->user_data);
    }
    g_error_free(err);
    delete_ctx_free(ctx);
    return;
  }

  SoupMessageHeaders *headers = soup_message_get_request_headers(msg);
  soup_message_headers_append(headers, "Authorization", auth_header);

  soup_session_send_and_read_async(ctx->session, msg, G_PRIORITY_DEFAULT,
                                    ctx->cancellable, on_delete_response, ctx);
  g_object_unref(msg);
}

/**
 * Callback when unified signer returns signed event for delete.
 * The signed event contains id, pubkey, and sig - we just base64-encode it.
 */
static void on_delete_sign_complete(GObject *source, GAsyncResult *res, gpointer user_data) {
  BlossomDeleteContext *ctx = (BlossomDeleteContext *)user_data;
  (void)source;
  if (!ctx) return;

  GError *error = NULL;
  char *signed_event_json = NULL;
  gboolean ok = gnostr_sign_event_finish(res, &signed_event_json, &error);

  if (!ok || !signed_event_json) {
    GError *err = g_error_new(GNOSTR_BLOSSOM_ERROR,
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

  /* Base64 encode the signed event for Authorization header */
  gchar *base64 = g_base64_encode((const guchar *)signed_event_json, strlen(signed_event_json));
  g_free(signed_event_json);

  /* Build auth header: "Nostr <base64-encoded-signed-event>" per BUD-01 */
  char *auth_header = g_strdup_printf("Nostr %s", base64);
  g_free(base64);

  /* Proceed with delete */
  delete_with_auth(ctx, auth_header);
  g_free(auth_header);
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
  ctx->session = soup_session_new();
  soup_session_set_timeout(ctx->session, 30);

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

#else /* !HAVE_SOUP3 */

/* Stub implementations when libsoup is not available */

void gnostr_blossom_upload_async(const char *server_url,
                                  const char *file_path,
                                  const char *mime_type,
                                  GnostrBlossomUploadCallback callback,
                                  gpointer user_data,
                                  GCancellable *cancellable) {
  (void)server_url; (void)file_path; (void)mime_type; (void)cancellable;
  GError *err = g_error_new(GNOSTR_BLOSSOM_ERROR,
                             GNOSTR_BLOSSOM_ERROR_UPLOAD_FAILED,
                             "Blossom upload requires libsoup3");
  if (callback) callback(NULL, err, user_data);
  g_error_free(err);
}

void gnostr_blossom_list_async(const char *server_url,
                                const char *pubkey_hex,
                                GnostrBlossomListCallback callback,
                                gpointer user_data,
                                GCancellable *cancellable) {
  (void)server_url; (void)pubkey_hex; (void)cancellable;
  GError *err = g_error_new(GNOSTR_BLOSSOM_ERROR,
                             GNOSTR_BLOSSOM_ERROR_SERVER_ERROR,
                             "Blossom list requires libsoup3");
  if (callback) callback(NULL, err, user_data);
  g_error_free(err);
}

void gnostr_blossom_delete_async(const char *server_url,
                                  const char *sha256,
                                  GnostrBlossomDeleteCallback callback,
                                  gpointer user_data,
                                  GCancellable *cancellable) {
  (void)server_url; (void)sha256; (void)cancellable;
  GError *err = g_error_new(GNOSTR_BLOSSOM_ERROR,
                             GNOSTR_BLOSSOM_ERROR_SERVER_ERROR,
                             "Blossom delete requires libsoup3");
  if (callback) callback(FALSE, err, user_data);
  g_error_free(err);
}

void gnostr_blossom_upload_with_fallback_async(const char *file_path,
                                                 const char *mime_type,
                                                 GnostrBlossomUploadCallback callback,
                                                 gpointer user_data,
                                                 GCancellable *cancellable) {
  (void)file_path; (void)mime_type; (void)cancellable;
  GError *err = g_error_new(GNOSTR_BLOSSOM_ERROR,
                             GNOSTR_BLOSSOM_ERROR_UPLOAD_FAILED,
                             "Blossom upload requires libsoup3");
  if (callback) callback(NULL, err, user_data);
  g_error_free(err);
}

#endif /* HAVE_SOUP3 */
