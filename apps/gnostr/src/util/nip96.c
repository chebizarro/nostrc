/**
 * @file nip96.c
 * @brief NIP-96 HTTP File Storage Integration
 *
 * Implements NIP-96 file upload protocol: multipart form POST with NIP-98
 * (kind 27235) authentication. Provides discovery, upload, and delete.
 *
 * nostrc-fs5g: NIP-96 file storage upload support.
 */

#include "nip96.h"
#include "blossom.h"  /* gnostr_blossom_sha256_file, gnostr_blossom_detect_mime_type */
#include "../ipc/gnostr-signer-service.h"
#include <json-glib/json-glib.h>
#include <string.h>

#ifdef HAVE_SOUP3
#include <libsoup/soup.h>
#endif

G_DEFINE_QUARK(gnostr-nip96-error-quark, gnostr_nip96_error)

/* ---- Server Info ---- */

void gnostr_nip96_server_info_free(GnostrNip96ServerInfo *info)
{
  if (!info) return;
  g_free(info->api_url);
  g_free(info->download_url);
  g_free(info->tos_url);
  g_strfreev(info->content_types);
  g_free(info);
}

/* ---- Discovery Cache ---- */

#define NIP96_DISCOVERY_CACHE_MAX 50
static GHashTable *discovery_cache = NULL;  /* server_url -> GnostrNip96ServerInfo* */

static GnostrNip96ServerInfo *cache_lookup(const char *server_url)
{
  if (!discovery_cache) return NULL;
  return g_hash_table_lookup(discovery_cache, server_url);
}

static void cache_store(const char *server_url, GnostrNip96ServerInfo *info)
{
  if (!discovery_cache) {
    discovery_cache = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, (GDestroyNotify)gnostr_nip96_server_info_free);
  }
  /* Deep copy for the cache */
  GnostrNip96ServerInfo *copy = g_new0(GnostrNip96ServerInfo, 1);
  copy->api_url = g_strdup(info->api_url);
  copy->download_url = g_strdup(info->download_url);
  copy->tos_url = g_strdup(info->tos_url);
  copy->content_types = g_strdupv(info->content_types);
  copy->max_byte_size = info->max_byte_size;
  copy->nip98_required = info->nip98_required;
  if (g_hash_table_size(discovery_cache) >= NIP96_DISCOVERY_CACHE_MAX)
    g_hash_table_remove_all(discovery_cache);
  g_hash_table_replace(discovery_cache, g_strdup(server_url), copy);
}

/* ---- JSON Parsing Helpers ---- */

static GnostrNip96ServerInfo *parse_server_info(const char *data, gsize len)
{
  if (!data || len == 0) return NULL;

  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, data, len, NULL)) {
    return NULL;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
    return NULL;
  }

  JsonObject *obj = json_node_get_object(root);
  if (!json_object_has_member(obj, "api_url")) {
    return NULL;
  }

  GnostrNip96ServerInfo *info = g_new0(GnostrNip96ServerInfo, 1);
  info->api_url = g_strdup(json_object_get_string_member(obj, "api_url"));

  if (json_object_has_member(obj, "download_url"))
    info->download_url = g_strdup(json_object_get_string_member(obj, "download_url"));
  if (json_object_has_member(obj, "tos_url"))
    info->tos_url = g_strdup(json_object_get_string_member(obj, "tos_url"));

  /* Parse content_types array */
  if (json_object_has_member(obj, "content_types")) {
    JsonArray *ct_array = json_object_get_array_member(obj, "content_types");
    if (ct_array) {
      guint ct_len = json_array_get_length(ct_array);
      info->content_types = g_new0(char*, ct_len + 1);
      for (guint i = 0; i < ct_len; i++) {
        info->content_types[i] = g_strdup(json_array_get_string_element(ct_array, i));
      }
    }
  }

  /* Parse plans.free for limits */
  if (json_object_has_member(obj, "plans")) {
    JsonObject *plans = json_object_get_object_member(obj, "plans");
    if (plans && json_object_has_member(plans, "free")) {
      JsonObject *free_plan = json_object_get_object_member(plans, "free");
      if (free_plan) {
        if (json_object_has_member(free_plan, "max_byte_size"))
          info->max_byte_size = json_object_get_int_member(free_plan, "max_byte_size");
        if (json_object_has_member(free_plan, "is_nip98_required"))
          info->nip98_required = json_object_get_boolean_member(free_plan, "is_nip98_required");
      }
    }
  }

  return info;
}

/**
 * Parse NIP-96 upload response to extract blob info from nip94_event tags.
 *
 * Response format:
 * {
 *   "status": "success",
 *   "nip94_event": {
 *     "tags": [["url","..."], ["ox","..."], ["x","..."], ["m","..."], ["dim","WxH"]]
 *   }
 * }
 */
static GnostrBlossomBlob *parse_upload_response(const char *data, gsize len,
                                                  const char *fallback_sha256,
                                                  const char *fallback_mime,
                                                  gint64 fallback_size)
{
  if (!data || len == 0) return NULL;

  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, data, len, NULL)) {
    return NULL;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
    return NULL;
  }

  JsonObject *obj = json_node_get_object(root);

  /* Check status field */
  if (json_object_has_member(obj, "status")) {
    const char *status = json_object_get_string_member(obj, "status");
    if (status && g_strcmp0(status, "success") != 0 && g_strcmp0(status, "processing") != 0) {
      return NULL;
    }
  }

  GnostrBlossomBlob *blob = g_new0(GnostrBlossomBlob, 1);
  blob->sha256 = g_strdup(fallback_sha256);
  blob->mime_type = g_strdup(fallback_mime);
  blob->size = fallback_size;

  /* Parse nip94_event.tags */
  if (json_object_has_member(obj, "nip94_event")) {
    JsonObject *nip94 = json_object_get_object_member(obj, "nip94_event");
    if (nip94 && json_object_has_member(nip94, "tags")) {
      JsonArray *tags = json_object_get_array_member(nip94, "tags");
      if (tags) {
        guint n_tags = json_array_get_length(tags);
        for (guint i = 0; i < n_tags; i++) {
          JsonArray *tag = json_array_get_array_element(tags, i);
          if (!tag || json_array_get_length(tag) < 2) continue;

          const char *tag_name = json_array_get_string_element(tag, 0);
          const char *tag_value = json_array_get_string_element(tag, 1);
          if (!tag_name || !tag_value) continue;

          if (g_strcmp0(tag_name, "url") == 0) {
            g_free(blob->url);
            blob->url = g_strdup(tag_value);
          } else if (g_strcmp0(tag_name, "ox") == 0) {
            /* Original file hash takes precedence */
            g_free(blob->sha256);
            blob->sha256 = g_strdup(tag_value);
          } else if (g_strcmp0(tag_name, "m") == 0) {
            g_free(blob->mime_type);
            blob->mime_type = g_strdup(tag_value);
          } else if (g_strcmp0(tag_name, "size") == 0) {
            blob->size = g_ascii_strtoll(tag_value, NULL, 10);
          }
        }
      }
    }
  }


  /* Must have a URL to be useful */
  if (!blob->url || !*blob->url) {
    gnostr_blossom_blob_free(blob);
    return NULL;
  }

  return blob;
}

/* ---- NIP-98 Auth Event Builder ---- */

/**
 * Build a kind 27235 NIP-98 HTTP auth event JSON for signing.
 *
 * Tags: ["u", url], ["method", method], optionally ["payload", sha256]
 *
 * @return Newly allocated JSON string (caller frees)
 */
static char *nip96_build_auth_event(const char *url,
                                     const char *method,
                                     const char *payload_sha256)
{
  g_autoptr(JsonBuilder) builder = json_builder_new();

  json_builder_begin_object(builder);

  /* Kind 27235 per NIP-98 */
  json_builder_set_member_name(builder, "kind");
  json_builder_add_int_value(builder, 27235);

  json_builder_set_member_name(builder, "created_at");
  json_builder_add_int_value(builder, (gint64)g_get_real_time() / G_USEC_PER_SEC);

  /* Content is empty per NIP-98 */
  json_builder_set_member_name(builder, "content");
  json_builder_add_string_value(builder, "");

  /* Tags */
  json_builder_set_member_name(builder, "tags");
  json_builder_begin_array(builder);

  /* u tag: request URL */
  json_builder_begin_array(builder);
  json_builder_add_string_value(builder, "u");
  json_builder_add_string_value(builder, url);
  json_builder_end_array(builder);

  /* method tag */
  json_builder_begin_array(builder);
  json_builder_add_string_value(builder, "method");
  json_builder_add_string_value(builder, method);
  json_builder_end_array(builder);

  /* payload tag: SHA-256 of request body (optional) */
  if (payload_sha256 && *payload_sha256) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "payload");
    json_builder_add_string_value(builder, payload_sha256);
    json_builder_end_array(builder);
  }

  json_builder_end_array(builder); /* End tags */
  json_builder_end_object(builder);

  g_autoptr(JsonGenerator) gen = json_generator_new();
  json_generator_set_root(gen, json_builder_get_root(builder));
  char *json_str = json_generator_to_data(gen, NULL);


  return json_str;
}

#ifdef HAVE_SOUP3

/* ---- Discovery Implementation ---- */

static void on_discover_response(GObject *source, GAsyncResult *res, gpointer user_data)
{
  GTask *task = G_TASK(user_data);
  char *server_url = g_task_get_task_data(task);
  GError *error = NULL;

  GBytes *bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), res, &error);

  if (error) {
    g_object_unref(source); /* session */
    g_task_return_new_error(task, GNOSTR_NIP96_ERROR,
                             GNOSTR_NIP96_ERROR_DISCOVERY_FAILED,
                             "Discovery request failed: %s", error->message);
    g_clear_error(&error);
    g_object_unref(task);
    return;
  }

  /* Check HTTP status */
  SoupMessage *msg = soup_session_get_async_result_message(SOUP_SESSION(source), res);
  guint status = msg ? soup_message_get_status(msg) : 0;
  g_object_unref(source); /* session */

  if (status < 200 || status >= 300) {
    g_bytes_unref(bytes);
    g_task_return_new_error(task, GNOSTR_NIP96_ERROR,
                             GNOSTR_NIP96_ERROR_DISCOVERY_FAILED,
                             "Discovery failed with HTTP status %u", status);
    g_object_unref(task);
    return;
  }

  gsize len = 0;
  const char *data = g_bytes_get_data(bytes, &len);

  GnostrNip96ServerInfo *info = parse_server_info(data, len);
  g_bytes_unref(bytes);

  if (!info) {
    g_task_return_new_error(task, GNOSTR_NIP96_ERROR,
                             GNOSTR_NIP96_ERROR_DISCOVERY_FAILED,
                             "Failed to parse NIP-96 server info");
    g_object_unref(task);
    return;
  }

  /* Cache the result */
  cache_store(server_url, info);

  g_task_return_pointer(task, info, (GDestroyNotify)gnostr_nip96_server_info_free);
  g_object_unref(task);
}

void gnostr_nip96_discover_async(const char *server_url,
                                  GCancellable *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer user_data)
{
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  g_task_set_task_data(task, g_strdup(server_url), g_free);

  /* Check cache first */
  GnostrNip96ServerInfo *cached = cache_lookup(server_url);
  if (cached) {
    /* Return a copy from cache */
    GnostrNip96ServerInfo *copy = g_new0(GnostrNip96ServerInfo, 1);
    copy->api_url = g_strdup(cached->api_url);
    copy->download_url = g_strdup(cached->download_url);
    copy->tos_url = g_strdup(cached->tos_url);
    copy->content_types = g_strdupv(cached->content_types);
    copy->max_byte_size = cached->max_byte_size;
    copy->nip98_required = cached->nip98_required;
    g_task_return_pointer(task, copy, (GDestroyNotify)gnostr_nip96_server_info_free);
    g_object_unref(task);
    return;
  }

  /* Fetch .well-known/nostr/nip96.json */
  char *url = g_strdup_printf("%s/.well-known/nostr/nip96.json", server_url);
  SoupSession *session = soup_session_new();
  SoupMessage *msg = soup_message_new("GET", url);
  g_free(url);

  if (!msg) {
    g_task_return_new_error(task, GNOSTR_NIP96_ERROR,
                             GNOSTR_NIP96_ERROR_DISCOVERY_FAILED,
                             "Invalid server URL: %s", server_url);
    g_object_unref(session);
    g_object_unref(task);
    return;
  }

  soup_session_send_and_read_async(session, msg, G_PRIORITY_DEFAULT,
                                    cancellable, on_discover_response, task);
  g_object_unref(msg);
  /* session is unreffed in on_discover_response */
}

GnostrNip96ServerInfo *gnostr_nip96_discover_finish(GAsyncResult *res,
                                                     GError **error)
{
  return g_task_propagate_pointer(G_TASK(res), error);
}

/* ---- Upload Implementation ---- */

typedef struct {
  char *server_url;
  char *api_url;
  char *file_path;
  char *mime_type;
  char *sha256;
  gint64 file_size;
  GBytes *file_data;
  GnostrBlossomUploadCallback callback;
  gpointer user_data;
  GCancellable *cancellable;
  SoupSession *session;
  char *auth_event_json;
} Nip96UploadContext;

static void nip96_upload_ctx_free(Nip96UploadContext *ctx)
{
  if (!ctx) return;
  g_free(ctx->server_url);
  g_free(ctx->api_url);
  g_free(ctx->file_path);
  g_free(ctx->mime_type);
  g_free(ctx->sha256);
  g_free(ctx->auth_event_json);
  if (ctx->file_data) g_bytes_unref(ctx->file_data);
  if (ctx->cancellable) g_object_unref(ctx->cancellable);
  if (ctx->session) g_object_unref(ctx->session);
  g_free(ctx);
}

static void on_nip96_upload_response(GObject *source, GAsyncResult *res, gpointer user_data)
{
  Nip96UploadContext *ctx = (Nip96UploadContext *)user_data;
  GError *error = NULL;

  GBytes *bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), res, &error);

  if (error) {
    if (ctx->callback) ctx->callback(NULL, error, ctx->user_data);
    g_clear_error(&error);
    nip96_upload_ctx_free(ctx);
    return;
  }

  /* Check HTTP status */
  SoupMessage *msg = soup_session_get_async_result_message(SOUP_SESSION(source), res);
  guint status = soup_message_get_status(msg);

  if (status < 200 || status >= 300) {
    gsize body_len = 0;
    const char *body = g_bytes_get_data(bytes, &body_len);
    GError *err = g_error_new(GNOSTR_NIP96_ERROR,
                               GNOSTR_NIP96_ERROR_UPLOAD_FAILED,
                               "NIP-96 upload failed with status %u: %.*s",
                               status, (int)MIN(body_len, 200), body ? body : "");
    if (ctx->callback) ctx->callback(NULL, err, ctx->user_data);
    g_error_free(err);
    g_bytes_unref(bytes);
    nip96_upload_ctx_free(ctx);
    return;
  }

  /* Parse response */
  gsize len = 0;
  const char *data = g_bytes_get_data(bytes, &len);
  GnostrBlossomBlob *blob = parse_upload_response(data, len, ctx->sha256,
                                                    ctx->mime_type, ctx->file_size);
  g_bytes_unref(bytes);

  if (!blob) {
    GError *err = g_error_new(GNOSTR_NIP96_ERROR,
                               GNOSTR_NIP96_ERROR_PARSE_ERROR,
                               "Failed to parse NIP-96 upload response");
    if (ctx->callback) ctx->callback(NULL, err, ctx->user_data);
    g_error_free(err);
    nip96_upload_ctx_free(ctx);
    return;
  }

  if (ctx->callback) {
    ctx->callback(blob, NULL, ctx->user_data);
  } else {
    gnostr_blossom_blob_free(blob);
  }
  nip96_upload_ctx_free(ctx);
}

static void nip96_send_multipart(Nip96UploadContext *ctx, const char *auth_header)
{
  /* Build multipart form */
  SoupMultipart *mp = soup_multipart_new("multipart/form-data");

  /* Append file */
  g_autofree char *basename = g_path_get_basename(ctx->file_path);
  soup_multipart_append_form_file(mp, "file", basename, ctx->mime_type, ctx->file_data);

  /* Build POST message and serialize multipart body (libsoup3 API) */
  SoupMessage *msg = soup_message_new("POST", ctx->api_url);
  if (!msg) {
    soup_multipart_free(mp);
    GError *err = g_error_new(GNOSTR_NIP96_ERROR,
                               GNOSTR_NIP96_ERROR_UPLOAD_FAILED,
                               "Failed to create NIP-96 upload request");
    if (ctx->callback) ctx->callback(NULL, err, ctx->user_data);
    g_error_free(err);
    nip96_upload_ctx_free(ctx);
    return;
  }

  /* Serialize multipart into message headers and body */
  SoupMessageHeaders *req_headers = soup_message_get_request_headers(msg);
  GBytes *body = NULL;
  soup_multipart_to_message(mp, req_headers, &body);
  soup_multipart_free(mp);
  soup_message_set_request_body_from_bytes(msg, NULL, body);
  g_bytes_unref(body);

  /* Add NIP-98 Authorization header */
  soup_message_headers_append(req_headers, "Authorization", auth_header);

  /* Send async */
  soup_session_send_and_read_async(ctx->session, msg, G_PRIORITY_DEFAULT,
                                    ctx->cancellable, on_nip96_upload_response, ctx);
  g_object_unref(msg);
}

static void on_nip96_sign_complete(GObject *source, GAsyncResult *res, gpointer user_data)
{
  Nip96UploadContext *ctx = (Nip96UploadContext *)user_data;
  (void)source;
  if (!ctx) return;

  GError *error = NULL;
  char *signed_event_json = NULL;
  gboolean ok = gnostr_sign_event_finish(res, &signed_event_json, &error);

  if (!ok || !signed_event_json) {
    GError *err = g_error_new(GNOSTR_NIP96_ERROR,
                               GNOSTR_NIP96_ERROR_AUTH_FAILED,
                               "Failed to sign NIP-98 auth event: %s",
                               error ? error->message : "unknown");
    g_clear_error(&error);
    if (ctx->callback) ctx->callback(NULL, err, ctx->user_data);
    g_error_free(err);
    nip96_upload_ctx_free(ctx);
    return;
  }

  /* Base64 encode the signed event for Authorization header */
  gchar *base64 = g_base64_encode((const guchar *)signed_event_json, strlen(signed_event_json));
  g_free(signed_event_json);

  char *auth_header = g_strdup_printf("Nostr %s", base64);
  g_free(base64);

  nip96_send_multipart(ctx, auth_header);
  g_free(auth_header);
}

static void on_discover_for_upload(GObject *source, GAsyncResult *res, gpointer user_data)
{
  Nip96UploadContext *ctx = (Nip96UploadContext *)user_data;
  (void)source;

  GError *error = NULL;
  GnostrNip96ServerInfo *info = gnostr_nip96_discover_finish(res, &error);

  if (!info) {
    GError *err = g_error_new(GNOSTR_NIP96_ERROR,
                               GNOSTR_NIP96_ERROR_DISCOVERY_FAILED,
                               "NIP-96 server discovery failed: %s",
                               error ? error->message : "unknown");
    g_clear_error(&error);
    if (ctx->callback) ctx->callback(NULL, err, ctx->user_data);
    g_error_free(err);
    nip96_upload_ctx_free(ctx);
    return;
  }

  /* Check file size limits */
  if (info->max_byte_size > 0 && ctx->file_size > info->max_byte_size) {
    GError *err = g_error_new(GNOSTR_NIP96_ERROR,
                               GNOSTR_NIP96_ERROR_FILE_TOO_LARGE,
                               "File too large: %" G_GINT64_FORMAT " bytes (max %" G_GINT64_FORMAT ")",
                               ctx->file_size, info->max_byte_size);
    gnostr_nip96_server_info_free(info);
    if (ctx->callback) ctx->callback(NULL, err, ctx->user_data);
    g_error_free(err);
    nip96_upload_ctx_free(ctx);
    return;
  }

  /* Resolve api_url - may be relative or absolute */
  if (g_str_has_prefix(info->api_url, "http://") || g_str_has_prefix(info->api_url, "https://")) {
    ctx->api_url = g_strdup(info->api_url);
  } else {
    /* Relative URL - prepend server_url */
    ctx->api_url = g_strdup_printf("%s%s%s",
                                    ctx->server_url,
                                    info->api_url[0] == '/' ? "" : "/",
                                    info->api_url);
  }

  gnostr_nip96_server_info_free(info);

  /* Build NIP-98 auth event (kind 27235) */
  ctx->auth_event_json = nip96_build_auth_event(ctx->api_url, "POST", ctx->sha256);

  /* Sign event */
  gnostr_sign_event_async(
    ctx->auth_event_json,
    "",  /* current_user: ignored */
    "",  /* app_id: ignored */
    ctx->cancellable,
    on_nip96_sign_complete,
    ctx);
}

void gnostr_nip96_upload_async(const char *server_url,
                                const char *file_path,
                                const char *mime_type,
                                GnostrBlossomUploadCallback callback,
                                gpointer user_data,
                                GCancellable *cancellable)
{
  if (!server_url || !file_path) {
    GError *err = g_error_new(GNOSTR_NIP96_ERROR,
                               GNOSTR_NIP96_ERROR_FILE_NOT_FOUND,
                               "Invalid server URL or file path");
    if (callback) callback(NULL, err, user_data);
    g_error_free(err);
    return;
  }

  /* Check signer availability */
  GnostrSignerService *signer = gnostr_signer_service_get_default();
  if (!gnostr_signer_service_is_available(signer)) {
    GError *err = g_error_new(GNOSTR_NIP96_ERROR,
                               GNOSTR_NIP96_ERROR_AUTH_FAILED,
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

  if (!g_file_load_contents(file, NULL, &contents, &length, NULL, &error)) {
    GError *err = g_error_new(GNOSTR_NIP96_ERROR,
                               GNOSTR_NIP96_ERROR_FILE_READ,
                               "Failed to read file: %s", error->message);
    g_clear_error(&error);
    if (callback) callback(NULL, err, user_data);
    g_error_free(err);
    return;
  }

  /* Compute SHA-256 */
  char sha256[65];
  GError *hash_error = NULL;
  if (!gnostr_blossom_sha256_file(file_path, sha256, &hash_error)) {
    g_free(contents);
    GError *err = g_error_new(GNOSTR_NIP96_ERROR,
                               GNOSTR_NIP96_ERROR_FILE_READ,
                               "Failed to compute file hash: %s",
                               hash_error ? hash_error->message : "unknown");
    g_clear_error(&hash_error);
    if (callback) callback(NULL, err, user_data);
    g_error_free(err);
    return;
  }

  /* Detect MIME type */
  const char *detected_mime = mime_type ? mime_type : gnostr_blossom_detect_mime_type(file_path);

  /* Create context */
  Nip96UploadContext *ctx = g_new0(Nip96UploadContext, 1);
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

  g_message("nip96: starting upload of %s to %s (%" G_GINT64_FORMAT " bytes)",
            file_path, server_url, ctx->file_size);

  /* Discover server capabilities, then upload */
  gnostr_nip96_discover_async(server_url, cancellable, on_discover_for_upload, ctx);
}

/* ---- Delete Implementation ---- */

typedef struct {
  char *server_url;
  char *api_url;
  char *sha256;
  GnostrBlossomDeleteCallback callback;
  gpointer user_data;
  GCancellable *cancellable;
  SoupSession *session;
  char *auth_event_json;
} Nip96DeleteContext;

static void nip96_delete_ctx_free(Nip96DeleteContext *ctx)
{
  if (!ctx) return;
  g_free(ctx->server_url);
  g_free(ctx->api_url);
  g_free(ctx->sha256);
  g_free(ctx->auth_event_json);
  if (ctx->cancellable) g_object_unref(ctx->cancellable);
  if (ctx->session) g_object_unref(ctx->session);
  g_free(ctx);
}

static void on_nip96_delete_response(GObject *source, GAsyncResult *res, gpointer user_data)
{
  Nip96DeleteContext *ctx = (Nip96DeleteContext *)user_data;
  GError *error = NULL;

  GBytes *bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), res, &error);

  if (error) {
    if (ctx->callback) ctx->callback(FALSE, error, ctx->user_data);
    g_clear_error(&error);
    nip96_delete_ctx_free(ctx);
    return;
  }

  SoupMessage *msg = soup_session_get_async_result_message(SOUP_SESSION(source), res);
  guint status = soup_message_get_status(msg);
  g_bytes_unref(bytes);

  if (status < 200 || status >= 300) {
    GError *err = g_error_new(GNOSTR_NIP96_ERROR,
                               GNOSTR_NIP96_ERROR_SERVER_ERROR,
                               "NIP-96 delete failed with status %u", status);
    if (ctx->callback) ctx->callback(FALSE, err, ctx->user_data);
    g_error_free(err);
    nip96_delete_ctx_free(ctx);
    return;
  }

  if (ctx->callback) ctx->callback(TRUE, NULL, ctx->user_data);
  nip96_delete_ctx_free(ctx);
}

static void on_nip96_delete_sign_complete(GObject *source, GAsyncResult *res, gpointer user_data)
{
  Nip96DeleteContext *ctx = (Nip96DeleteContext *)user_data;
  (void)source;

  GError *error = NULL;
  char *signed_event_json = NULL;
  gboolean ok = gnostr_sign_event_finish(res, &signed_event_json, &error);

  if (!ok || !signed_event_json) {
    GError *err = g_error_new(GNOSTR_NIP96_ERROR,
                               GNOSTR_NIP96_ERROR_AUTH_FAILED,
                               "Failed to sign NIP-98 delete auth: %s",
                               error ? error->message : "unknown");
    g_clear_error(&error);
    if (ctx->callback) ctx->callback(FALSE, err, ctx->user_data);
    g_error_free(err);
    nip96_delete_ctx_free(ctx);
    return;
  }

  gchar *base64 = g_base64_encode((const guchar *)signed_event_json, strlen(signed_event_json));
  g_free(signed_event_json);
  char *auth_header = g_strdup_printf("Nostr %s", base64);
  g_free(base64);

  /* Build DELETE request */
  char *url = g_strdup_printf("%s/%s", ctx->api_url, ctx->sha256);
  SoupMessage *msg = soup_message_new("DELETE", url);
  g_free(url);

  if (!msg) {
    GError *err = g_error_new(GNOSTR_NIP96_ERROR,
                               GNOSTR_NIP96_ERROR_SERVER_ERROR,
                               "Failed to create delete request");
    g_free(auth_header);
    if (ctx->callback) ctx->callback(FALSE, err, ctx->user_data);
    g_error_free(err);
    nip96_delete_ctx_free(ctx);
    return;
  }

  SoupMessageHeaders *headers = soup_message_get_request_headers(msg);
  soup_message_headers_append(headers, "Authorization", auth_header);
  g_free(auth_header);

  soup_session_send_and_read_async(ctx->session, msg, G_PRIORITY_DEFAULT,
                                    ctx->cancellable, on_nip96_delete_response, ctx);
  g_object_unref(msg);
}

static void on_discover_for_delete(GObject *source, GAsyncResult *res, gpointer user_data)
{
  Nip96DeleteContext *ctx = (Nip96DeleteContext *)user_data;
  (void)source;

  GError *error = NULL;
  GnostrNip96ServerInfo *info = gnostr_nip96_discover_finish(res, &error);

  if (!info) {
    GError *err = g_error_new(GNOSTR_NIP96_ERROR,
                               GNOSTR_NIP96_ERROR_DISCOVERY_FAILED,
                               "NIP-96 server discovery failed: %s",
                               error ? error->message : "unknown");
    g_clear_error(&error);
    if (ctx->callback) ctx->callback(FALSE, err, ctx->user_data);
    g_error_free(err);
    nip96_delete_ctx_free(ctx);
    return;
  }

  /* Resolve api_url */
  if (g_str_has_prefix(info->api_url, "http://") || g_str_has_prefix(info->api_url, "https://")) {
    ctx->api_url = g_strdup(info->api_url);
  } else {
    ctx->api_url = g_strdup_printf("%s%s%s",
                                    ctx->server_url,
                                    info->api_url[0] == '/' ? "" : "/",
                                    info->api_url);
  }
  gnostr_nip96_server_info_free(info);

  /* Build NIP-98 auth for DELETE */
  char *delete_url = g_strdup_printf("%s/%s", ctx->api_url, ctx->sha256);
  ctx->auth_event_json = nip96_build_auth_event(delete_url, "DELETE", NULL);
  g_free(delete_url);

  gnostr_sign_event_async(ctx->auth_event_json, "", "", ctx->cancellable,
                           on_nip96_delete_sign_complete, ctx);
}

void gnostr_nip96_delete_async(const char *server_url,
                                const char *sha256,
                                GnostrBlossomDeleteCallback callback,
                                gpointer user_data,
                                GCancellable *cancellable)
{
  if (!server_url || !sha256) {
    GError *err = g_error_new(GNOSTR_NIP96_ERROR,
                               GNOSTR_NIP96_ERROR_SERVER_ERROR,
                               "Invalid server URL or hash");
    if (callback) callback(FALSE, err, user_data);
    g_error_free(err);
    return;
  }

  GnostrSignerService *signer = gnostr_signer_service_get_default();
  if (!gnostr_signer_service_is_available(signer)) {
    GError *err = g_error_new(GNOSTR_NIP96_ERROR,
                               GNOSTR_NIP96_ERROR_AUTH_FAILED,
                               "Signer not available");
    if (callback) callback(FALSE, err, user_data);
    g_error_free(err);
    return;
  }

  Nip96DeleteContext *ctx = g_new0(Nip96DeleteContext, 1);
  ctx->server_url = g_strdup(server_url);
  ctx->sha256 = g_strdup(sha256);
  ctx->callback = callback;
  ctx->user_data = user_data;
  ctx->cancellable = cancellable ? g_object_ref(cancellable) : NULL;
  ctx->session = soup_session_new();

  gnostr_nip96_discover_async(server_url, cancellable, on_discover_for_delete, ctx);
}

#else /* !HAVE_SOUP3 */

/* Stub implementations when libsoup3 is not available */

void gnostr_nip96_discover_async(const char *server_url,
                                  GCancellable *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer user_data)
{
  (void)server_url; (void)cancellable;
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  g_task_return_new_error(task, GNOSTR_NIP96_ERROR,
                           GNOSTR_NIP96_ERROR_DISCOVERY_FAILED,
                           "NIP-96 requires libsoup3");
  g_object_unref(task);
}

GnostrNip96ServerInfo *gnostr_nip96_discover_finish(GAsyncResult *res,
                                                     GError **error)
{
  return g_task_propagate_pointer(G_TASK(res), error);
}

void gnostr_nip96_upload_async(const char *server_url,
                                const char *file_path,
                                const char *mime_type,
                                GnostrBlossomUploadCallback callback,
                                gpointer user_data,
                                GCancellable *cancellable)
{
  (void)server_url; (void)file_path; (void)mime_type; (void)cancellable;
  GError *err = g_error_new(GNOSTR_NIP96_ERROR,
                             GNOSTR_NIP96_ERROR_UPLOAD_FAILED,
                             "NIP-96 upload requires libsoup3");
  if (callback) callback(NULL, err, user_data);
  g_error_free(err);
}

void gnostr_nip96_delete_async(const char *server_url,
                                const char *sha256,
                                GnostrBlossomDeleteCallback callback,
                                gpointer user_data,
                                GCancellable *cancellable)
{
  (void)server_url; (void)sha256; (void)cancellable;
  GError *err = g_error_new(GNOSTR_NIP96_ERROR,
                             GNOSTR_NIP96_ERROR_SERVER_ERROR,
                             "NIP-96 delete requires libsoup3");
  if (callback) callback(FALSE, err, user_data);
  g_error_free(err);
}

#endif /* HAVE_SOUP3 */
