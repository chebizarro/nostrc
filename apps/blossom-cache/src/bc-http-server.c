/*
 * bc-http-server.c - BcHttpServer: local Blossom-compatible HTTP server
 *
 * SPDX-License-Identifier: MIT
 *
 * Implements the Blossom HTTP API per the local-blossom-cache spec and BUD-01/02:
 *
 *   HEAD /                – Health check (2xx, no body)
 *   GET  /<sha256>[.ext]  – Retrieve a blob (BUD-01, with range support)
 *   HEAD /<sha256>[.ext]  – Check blob existence + return headers (BUD-01)
 *   PUT  /upload          – Store a blob, returns Blob Descriptor (BUD-02)
 *   DELETE /<sha256>      – Delete a blob (BUD-02)
 *   GET  /list/<pubkey>   – List cached blobs with cursor/limit pagination (BUD-02)
 *   GET  /status          – JSON cache statistics (extension)
 *
 * Proxy hint query params (BUD-10 / local-blossom-cache spec):
 *   ?xs=<server>          – Server hints for upstream fetch on cache miss
 *   ?as=<pubkey>          – Author hints (TODO: kind:10063 relay lookup)
 *
 * CORS: Access-Control-Allow-Origin: * on ALL responses (BUD-01 mandate)
 */

#include "bc-http-server.h"
#include "bc-upstream-client.h"

#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <string.h>

/* ---- Error quark ---- */

G_DEFINE_QUARK(bc-http-server-error-quark, bc_http_server_error)

/* ---- Private structure ---- */

struct _BcHttpServer {
  GObject         parent_instance;

  BcBlobStore    *store;      /* not owned */
  BcCacheManager *cache_mgr;  /* not owned */
  SoupServer     *soup;       /* owned */
  gboolean        running;
  gchar          *listen_addr; /* owned, for building URLs */
  guint           listen_port;
};

G_DEFINE_TYPE(BcHttpServer, bc_http_server, G_TYPE_OBJECT)

/* ---- Helpers ---- */

static gboolean
is_valid_sha256(const gchar *s)
{
  if (s == NULL || strlen(s) != 64)
    return FALSE;
  for (gsize i = 0; i < 64; i++) {
    if (!g_ascii_isxdigit(s[i]))
      return FALSE;
  }
  return TRUE;
}

/**
 * extract_sha256_from_path:
 * @path: URL path starting with '/'
 *
 * Extracts the SHA-256 hash from paths like:
 *   /<sha256>
 *   /<sha256>.pdf
 *   /<sha256>.png
 *
 * Returns: (transfer full) (nullable): the 64-char hex hash, or NULL
 */
static gchar *
extract_sha256_from_path(const gchar *path)
{
  if (path == NULL || path[0] != '/' || strlen(path) < 65)
    return NULL;

  /* Skip leading '/' */
  const gchar *start = path + 1;

  /* The hash is always exactly 64 hex chars */
  gchar hash[65];
  memcpy(hash, start, 64);
  hash[64] = '\0';

  if (!is_valid_sha256(hash))
    return NULL;

  /* After the 64 chars, must be end-of-string or a '.' (file extension) */
  char after = start[64];
  if (after != '\0' && after != '.')
    return NULL;

  return g_strdup(hash);
}

/**
 * Collect all values for a given query parameter key from a GHashTable.
 * libsoup 3.0's query parsing only gives us the last value for duplicate keys,
 * so we also manually parse the raw query string to collect all values.
 *
 * Returns: (transfer full): NULL-terminated array of values, or NULL if none
 */
static gchar **
collect_query_values(SoupServerMessage *msg, const gchar *key)
{
  GUri *uri = soup_server_message_get_uri(msg);
  const gchar *query_str = g_uri_get_query(uri);
  if (query_str == NULL)
    return NULL;

  g_autoptr(GPtrArray) values = g_ptr_array_new_with_free_func(g_free);
  g_autofree gchar *key_eq = g_strdup_printf("%s=", key);
  gsize key_eq_len = strlen(key_eq);

  /* Parse manually to handle repeated params */
  g_auto(GStrv) pairs = g_strsplit(query_str, "&", -1);
  for (gsize i = 0; pairs[i] != NULL; i++) {
    if (g_str_has_prefix(pairs[i], key_eq)) {
      g_autofree gchar *decoded = g_uri_unescape_string(pairs[i] + key_eq_len, NULL);
      if (decoded != NULL && decoded[0] != '\0')
        g_ptr_array_add(values, g_steal_pointer(&decoded));
    }
  }

  if (values->len == 0)
    return NULL;

  g_ptr_array_add(values, NULL);
  return (gchar **)g_ptr_array_steal(values, NULL);
}

static const gchar *
get_query_param(GHashTable *query, const gchar *key)
{
  if (query == NULL)
    return NULL;
  return g_hash_table_lookup(query, key);
}

static void
set_cors_headers(SoupMessageHeaders *hdrs)
{
  soup_message_headers_replace(hdrs, "Access-Control-Allow-Origin", "*");
}

static void
respond_json(SoupServerMessage *msg, guint status, JsonBuilder *builder)
{
  g_autoptr(JsonGenerator) gen = json_generator_new();
  json_generator_set_root(gen, json_builder_get_root(builder));
  json_generator_set_pretty(gen, TRUE);

  gsize length = 0;
  g_autofree gchar *body = json_generator_to_data(gen, &length);

  soup_server_message_set_status(msg, status, NULL);
  SoupMessageHeaders *hdrs = soup_server_message_get_response_headers(msg);
  soup_message_headers_replace(hdrs, "Content-Type", "application/json");
  set_cors_headers(hdrs);
  soup_server_message_set_response(msg, "application/json",
                                   SOUP_MEMORY_COPY,
                                   body, length);
}

static void
respond_error(SoupServerMessage *msg, guint status, const gchar *message)
{
  /* BUD-01: X-Reason header on error responses */
  SoupMessageHeaders *hdrs = soup_server_message_get_response_headers(msg);
  soup_message_headers_replace(hdrs, "X-Reason", message);

  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "error");
  json_builder_add_string_value(builder, message);
  json_builder_end_object(builder);

  respond_json(msg, status, builder);
}

/**
 * Build a BUD-02 compliant Blob Descriptor JSON object.
 */
static void
build_blob_descriptor(JsonBuilder *builder,
                      const gchar *sha256,
                      gint64       size,
                      const gchar *mime_type,
                      gint64       uploaded,
                      const gchar *base_url)
{
  /* Guess file extension from MIME type */
  const gchar *ext = "";
  if (mime_type != NULL) {
    if (g_str_equal(mime_type, "application/pdf")) ext = ".pdf";
    else if (g_str_equal(mime_type, "image/png")) ext = ".png";
    else if (g_str_equal(mime_type, "image/jpeg")) ext = ".jpg";
    else if (g_str_equal(mime_type, "image/gif")) ext = ".gif";
    else if (g_str_equal(mime_type, "image/webp")) ext = ".webp";
    else if (g_str_equal(mime_type, "image/svg+xml")) ext = ".svg";
    else if (g_str_equal(mime_type, "video/mp4")) ext = ".mp4";
    else if (g_str_equal(mime_type, "video/webm")) ext = ".webm";
    else if (g_str_equal(mime_type, "audio/mpeg")) ext = ".mp3";
    else if (g_str_equal(mime_type, "audio/ogg")) ext = ".ogg";
    else if (g_str_equal(mime_type, "text/plain")) ext = ".txt";
    else if (g_str_equal(mime_type, "text/html")) ext = ".html";
    else if (g_str_equal(mime_type, "application/json")) ext = ".json";
    else if (g_str_equal(mime_type, "application/zip")) ext = ".zip";
  }

  json_builder_begin_object(builder);

  json_builder_set_member_name(builder, "url");
  g_autofree gchar *url = g_strdup_printf("%s/%s%s", base_url, sha256, ext);
  json_builder_add_string_value(builder, url);

  json_builder_set_member_name(builder, "sha256");
  json_builder_add_string_value(builder, sha256);

  json_builder_set_member_name(builder, "size");
  json_builder_add_int_value(builder, size);

  json_builder_set_member_name(builder, "type");
  json_builder_add_string_value(builder,
    mime_type ? mime_type : "application/octet-stream");

  json_builder_set_member_name(builder, "uploaded");
  json_builder_add_int_value(builder, uploaded);

  json_builder_end_object(builder);
}

/* ---- Request handlers ---- */

static void
handle_health_check(SoupServerMessage *msg)
{
  /* local-blossom-cache spec: HEAD / MUST return 2xx, no body */
  soup_server_message_set_status(msg, 200, NULL);
  SoupMessageHeaders *hdrs = soup_server_message_get_response_headers(msg);
  set_cors_headers(hdrs);
}

static void
handle_get_blob(BcHttpServer *self, SoupServerMessage *msg, const gchar *sha256)
{
  /* Collect xs= server hints from query string (BUD-10 proxy hints) */
  g_auto(GStrv) xs_hints = collect_query_values(msg, "xs");
  /* TODO: as= author hints would require kind:10063 relay lookup */

  GError *error = NULL;
  g_autoptr(BcBlobInfo) info = NULL;
  g_autoptr(GBytes) data = bc_cache_manager_get_blob_with_hints(
    self->cache_mgr, sha256, (const gchar * const *)xs_hints, &info, &error);

  if (data == NULL) {
    if (error != NULL &&
        (g_error_matches(error, BC_BLOB_STORE_ERROR, BC_BLOB_STORE_ERROR_NOT_FOUND) ||
         g_error_matches(error, BC_UPSTREAM_CLIENT_ERROR, BC_UPSTREAM_CLIENT_ERROR_NOT_FOUND))) {
      respond_error(msg, 404, "Blob not found");
    } else {
      respond_error(msg, 502, error ? error->message : "Upstream fetch failed");
    }
    g_clear_error(&error);
    return;
  }

  gsize total_length = 0;
  gconstpointer raw = g_bytes_get_data(data, &total_length);

  const gchar *mime = (info && info->mime_type) ? info->mime_type
                                                : "application/octet-stream";

  /* Check for Range request (RFC 7233) */
  SoupMessageHeaders *req_hdrs = soup_server_message_get_request_headers(msg);
  SoupRange *ranges = NULL;
  int n_ranges = 0;
  gboolean has_range = FALSE;

  if (soup_message_headers_get_ranges(req_hdrs, (goffset)total_length,
                                       &ranges, &n_ranges)) {
    has_range = (n_ranges > 0);
  }

  SoupMessageHeaders *hdrs = soup_server_message_get_response_headers(msg);
  set_cors_headers(hdrs);

  if (has_range && n_ranges == 1) {
    /* Serve partial content (single range only for simplicity) */
    goffset start = ranges[0].start;
    goffset end   = ranges[0].end;

    if (start < 0) start = 0;
    if (end >= (goffset)total_length) end = (goffset)total_length - 1;

    /* RFC 7233: If start > end after clamping, range is unsatisfiable */
    if (start > end || start >= (goffset)total_length) {
      soup_message_headers_free_ranges(req_hdrs, ranges);
      soup_server_message_set_status(msg, 416, NULL);
      g_autofree gchar *cr_str = g_strdup_printf("bytes */%" G_GSIZE_FORMAT, total_length);
      soup_message_headers_replace(hdrs, "Content-Range", cr_str);
      return;
    }

    goffset range_len = end - start + 1;

    soup_server_message_set_status(msg, 206, NULL);
    soup_message_headers_replace(hdrs, "Content-Type", mime);

    g_autofree gchar *range_str = g_strdup_printf(
      "bytes %" G_GOFFSET_FORMAT "-%" G_GOFFSET_FORMAT "/%" G_GSIZE_FORMAT,
      start, end, total_length);
    soup_message_headers_replace(hdrs, "Content-Range", range_str);
    soup_message_headers_replace(hdrs, "Accept-Ranges", "bytes");

    g_autofree gchar *cl_str = g_strdup_printf("%" G_GOFFSET_FORMAT, range_len);
    soup_message_headers_replace(hdrs, "Content-Length", cl_str);

    soup_server_message_set_response(msg, mime, SOUP_MEMORY_COPY,
                                     (const gchar *)raw + start,
                                     (gsize)range_len);

    soup_message_headers_free_ranges(req_hdrs, ranges);
    return;
  }

  if (ranges != NULL)
    soup_message_headers_free_ranges(req_hdrs, ranges);

  /* Full response */
  soup_server_message_set_status(msg, 200, NULL);
  soup_message_headers_replace(hdrs, "Content-Type", mime);
  soup_message_headers_replace(hdrs, "Accept-Ranges", "bytes");

  g_autofree gchar *cl_str = g_strdup_printf("%" G_GSIZE_FORMAT, total_length);
  soup_message_headers_replace(hdrs, "Content-Length", cl_str);

  if (info != NULL) {
    soup_message_headers_replace(hdrs, "X-Blob-SHA256", sha256);
  }

  soup_server_message_set_response(msg, mime, SOUP_MEMORY_COPY,
                                   (const gchar *)raw, total_length);
}

static void
handle_head_blob(BcHttpServer *self, SoupServerMessage *msg, const gchar *sha256)
{
  GError *error = NULL;
  g_autoptr(BcBlobInfo) info = bc_blob_store_get_info(self->store, sha256, &error);

  if (info == NULL) {
    soup_server_message_set_status(msg, 404, NULL);
    SoupMessageHeaders *hdrs = soup_server_message_get_response_headers(msg);
    set_cors_headers(hdrs);
    soup_message_headers_replace(hdrs, "X-Reason", "Blob not found");
    g_clear_error(&error);
    return;
  }

  soup_server_message_set_status(msg, 200, NULL);
  SoupMessageHeaders *hdrs = soup_server_message_get_response_headers(msg);
  set_cors_headers(hdrs);

  const gchar *mime = info->mime_type ? info->mime_type : "application/octet-stream";
  soup_message_headers_replace(hdrs, "Content-Type", mime);

  g_autofree gchar *size_str = g_strdup_printf("%" G_GINT64_FORMAT, info->size);
  soup_message_headers_replace(hdrs, "Content-Length", size_str);
  soup_message_headers_replace(hdrs, "Accept-Ranges", "bytes");
  soup_message_headers_replace(hdrs, "X-Blob-SHA256", sha256);
}

static void
handle_put_upload(BcHttpServer *self, SoupServerMessage *msg)
{
  SoupMessageHeaders *req_hdrs = soup_server_message_get_request_headers(msg);

  /* In libsoup 3, get the request body as SoupMessageBody then flatten to GBytes */
  SoupMessageBody *msg_body = soup_server_message_get_request_body(msg);
  g_autoptr(GBytes) body = NULL;

  if (msg_body != NULL) {
    body = soup_message_body_flatten(msg_body);
  }

  if (body == NULL || g_bytes_get_size(body) == 0) {
    respond_error(msg, 400, "Empty request body");
    return;
  }

  /* Compute SHA-256 of the uploaded data */
  gsize data_len = 0;
  const guchar *raw = g_bytes_get_data(body, &data_len);

  g_autoptr(GChecksum) checksum = g_checksum_new(G_CHECKSUM_SHA256);
  g_checksum_update(checksum, raw, data_len);
  const gchar *sha256 = g_checksum_get_string(checksum);

  /* Get Content-Type from request */
  const gchar *content_type =
    soup_message_headers_get_content_type(req_hdrs, NULL);

  /* Store via cache manager */
  GError *error = NULL;
  if (!bc_cache_manager_put_blob(self->cache_mgr, sha256, body,
                                 content_type, &error)) {
    if (g_error_matches(error, BC_BLOB_STORE_ERROR, BC_BLOB_STORE_ERROR_TOO_LARGE)) {
      respond_error(msg, 413, error->message);
    } else {
      respond_error(msg, 500, error->message);
    }
    g_clear_error(&error);
    return;
  }

  /* Build BUD-02 compliant Blob Descriptor response */
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  g_autofree gchar *base_url = g_strdup_printf("http://%s:%u",
    self->listen_addr, self->listen_port);

  g_autoptr(JsonBuilder) builder = json_builder_new();
  build_blob_descriptor(builder, sha256, (gint64)data_len,
                        content_type ? content_type : "application/octet-stream",
                        now, base_url);

  respond_json(msg, 200, builder);
}

static void
handle_delete_blob(BcHttpServer *self, SoupServerMessage *msg, const gchar *sha256)
{
  /* BUD-02: DELETE /<sha256> — delete a cached blob.
   * local-blossom-cache spec: no auth required for local access. */
  GError *error = NULL;

  if (!bc_blob_store_contains(self->store, sha256)) {
    respond_error(msg, 404, "Blob not found");
    return;
  }

  if (!bc_blob_store_delete(self->store, sha256, &error)) {
    respond_error(msg, 500, error ? error->message : "Delete failed");
    g_clear_error(&error);
    return;
  }

  soup_server_message_set_status(msg, 200, NULL);
  SoupMessageHeaders *hdrs = soup_server_message_get_response_headers(msg);
  set_cors_headers(hdrs);

  /* Return a simple success JSON */
  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "message");
  json_builder_add_string_value(builder, "Blob deleted");
  json_builder_set_member_name(builder, "sha256");
  json_builder_add_string_value(builder, sha256);
  json_builder_end_object(builder);

  respond_json(msg, 200, builder);
}

static void
handle_list(BcHttpServer *self, SoupServerMessage *msg, GHashTable *query)
{
  /* BUD-02: GET /list/<pubkey> with cursor/limit pagination.
   * For the local cache we ignore the pubkey and return all blobs. */
  const gchar *cursor = get_query_param(query, "cursor");
  const gchar *limit_str = get_query_param(query, "limit");

  guint limit = 100;
  if (limit_str != NULL) {
    gint64 parsed = g_ascii_strtoll(limit_str, NULL, 10);
    if (parsed > 0 && parsed <= 1000)
      limit = (guint)parsed;
  }

  GError *error = NULL;
  g_autoptr(GPtrArray) blobs = bc_blob_store_list_blobs(self->store, cursor,
                                                         limit, &error);
  if (blobs == NULL) {
    respond_error(msg, 500, error ? error->message : "List failed");
    g_clear_error(&error);
    return;
  }

  g_autofree gchar *base_url = g_strdup_printf("http://%s:%u",
    self->listen_addr, self->listen_port);

  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_array(builder);

  for (guint i = 0; i < blobs->len; i++) {
    BcBlobInfo *info = g_ptr_array_index(blobs, i);
    build_blob_descriptor(builder, info->sha256, info->size,
                          info->mime_type, info->created_at, base_url);
  }

  json_builder_end_array(builder);

  g_autoptr(JsonGenerator) gen = json_generator_new();
  json_generator_set_root(gen, json_builder_get_root(builder));
  json_generator_set_pretty(gen, TRUE);

  gsize length = 0;
  g_autofree gchar *body = json_generator_to_data(gen, &length);

  soup_server_message_set_status(msg, 200, NULL);
  SoupMessageHeaders *hdrs = soup_server_message_get_response_headers(msg);
  soup_message_headers_replace(hdrs, "Content-Type", "application/json");
  set_cors_headers(hdrs);
  soup_server_message_set_response(msg, "application/json",
                                   SOUP_MEMORY_COPY, body, length);
}

static void
handle_status(BcHttpServer *self, SoupServerMessage *msg)
{
  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_object(builder);

  json_builder_set_member_name(builder, "blob_count");
  json_builder_add_int_value(builder, bc_blob_store_get_blob_count(self->store));

  json_builder_set_member_name(builder, "total_size_bytes");
  json_builder_add_int_value(builder, bc_blob_store_get_total_size(self->store));

  gint64 total_mb = bc_blob_store_get_total_size(self->store) / (1024 * 1024);
  json_builder_set_member_name(builder, "total_size_mb");
  json_builder_add_int_value(builder, total_mb);

  json_builder_set_member_name(builder, "status");
  json_builder_add_string_value(builder, "ok");

  json_builder_end_object(builder);

  respond_json(msg, 200, builder);
}

/* ---- Main request dispatcher ---- */

static void
on_request(SoupServer        *soup_server,
           SoupServerMessage *msg,
           const gchar       *path,
           GHashTable        *query,
           gpointer           user_data)
{
  (void)soup_server;

  BcHttpServer *self = BC_HTTP_SERVER(user_data);
  const gchar *method = soup_server_message_get_method(msg);

  g_debug("HTTP %s %s", method, path);

  /* === CORS preflight (BUD-01) === */
  if (g_str_equal(method, "OPTIONS")) {
    soup_server_message_set_status(msg, 204, NULL);
    SoupMessageHeaders *hdrs = soup_server_message_get_response_headers(msg);
    set_cors_headers(hdrs);
    /* BUD-01: "Access-Control-Allow-Headers: Authorization, *" */
    soup_message_headers_replace(hdrs, "Access-Control-Allow-Methods",
                                 "GET, HEAD, PUT, DELETE, OPTIONS");
    soup_message_headers_replace(hdrs, "Access-Control-Allow-Headers",
                                 "Authorization, *");
    soup_message_headers_replace(hdrs, "Access-Control-Max-Age", "86400");
    return;
  }

  /* === Health check: HEAD / (local-blossom-cache spec) === */
  if (g_str_equal(path, "/") && g_str_equal(method, "HEAD")) {
    handle_health_check(msg);
    return;
  }

  /* === GET / — also support GET for convenience === */
  if (g_str_equal(path, "/") && g_str_equal(method, "GET")) {
    handle_health_check(msg);
    return;
  }

  /* === GET /status === */
  if (g_str_equal(method, "GET") && g_str_equal(path, "/status")) {
    handle_status(self, msg);
    return;
  }

  /* === PUT /upload === */
  if (g_str_equal(method, "PUT") && g_str_equal(path, "/upload")) {
    handle_put_upload(self, msg);
    return;
  }

  /* === GET /list/<pubkey> === */
  if (g_str_equal(method, "GET") && g_str_has_prefix(path, "/list")) {
    handle_list(self, msg, query);
    return;
  }

  /* === Routes that need a SHA-256 hash from the URL === */
  g_autofree gchar *sha256 = extract_sha256_from_path(path);

  if (sha256 != NULL) {
    if (g_str_equal(method, "GET")) {
      handle_get_blob(self, msg, sha256);
      return;
    }

    if (g_str_equal(method, "HEAD")) {
      handle_head_blob(self, msg, sha256);
      return;
    }

    if (g_str_equal(method, "DELETE")) {
      handle_delete_blob(self, msg, sha256);
      return;
    }
  }

  /* Fallback — ensure CORS on 404 too */
  respond_error(msg, 404, "Not found");
}

/* ---- GObject lifecycle ---- */

static void
bc_http_server_dispose(GObject *obj)
{
  BcHttpServer *self = BC_HTTP_SERVER(obj);

  bc_http_server_stop(self);
  self->store     = NULL;
  self->cache_mgr = NULL;

  G_OBJECT_CLASS(bc_http_server_parent_class)->dispose(obj);
}

static void
bc_http_server_finalize(GObject *obj)
{
  BcHttpServer *self = BC_HTTP_SERVER(obj);
  g_clear_object(&self->soup);
  g_clear_pointer(&self->listen_addr, g_free);
  G_OBJECT_CLASS(bc_http_server_parent_class)->finalize(obj);
}

static void
bc_http_server_class_init(BcHttpServerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose  = bc_http_server_dispose;
  object_class->finalize = bc_http_server_finalize;
}

static void
bc_http_server_init(BcHttpServer *self)
{
  self->running = FALSE;
  self->listen_addr = NULL;
  self->listen_port = 0;
}

/* ---- Public API ---- */

BcHttpServer *
bc_http_server_new(BcBlobStore    *store,
                   BcCacheManager *cache_mgr)
{
  g_return_val_if_fail(BC_IS_BLOB_STORE(store), NULL);
  g_return_val_if_fail(BC_IS_CACHE_MANAGER(cache_mgr), NULL);

  BcHttpServer *self = g_object_new(BC_TYPE_HTTP_SERVER, NULL);
  self->store     = store;
  self->cache_mgr = cache_mgr;

  return self;
}

gboolean
bc_http_server_start(BcHttpServer *self,
                     const gchar  *address,
                     guint         port,
                     GError      **error)
{
  g_return_val_if_fail(BC_IS_HTTP_SERVER(self), FALSE);

  if (self->running) {
    g_set_error_literal(error, BC_HTTP_SERVER_ERROR,
                        BC_HTTP_SERVER_ERROR_ALREADY_RUNNING,
                        "HTTP server is already running");
    return FALSE;
  }

  self->soup = soup_server_new("server-header", "blossom-cache/1.0", NULL);

  /* Register the catch-all handler */
  soup_server_add_handler(self->soup, "/", on_request, self, NULL);

  /* Resolve listen address */
  g_autoptr(GInetAddress) inet_addr = g_inet_address_new_from_string(address);

  if (inet_addr == NULL) {
    g_set_error(error, BC_HTTP_SERVER_ERROR, BC_HTTP_SERVER_ERROR_BIND,
                "Invalid listen address: %s", address);
    g_clear_object(&self->soup);
    return FALSE;
  }

  g_autoptr(GSocketAddress) sock_addr =
    G_SOCKET_ADDRESS(g_inet_socket_address_new(inet_addr, port));

  GError *listen_err = NULL;
  if (!soup_server_listen(self->soup, sock_addr, 0, &listen_err)) {
    g_propagate_prefixed_error(error, listen_err,
                               "Failed to listen on %s:%u: ", address, port);
    g_clear_object(&self->soup);
    return FALSE;
  }

  self->running     = TRUE;
  self->listen_addr = g_strdup(address);
  self->listen_port = port;

  return TRUE;
}

void
bc_http_server_stop(BcHttpServer *self)
{
  g_return_if_fail(BC_IS_HTTP_SERVER(self));

  if (!self->running)
    return;

  if (self->soup != NULL) {
    soup_server_disconnect(self->soup);
  }

  self->running = FALSE;
  g_debug("HTTP server stopped");
}

gboolean
bc_http_server_is_running(BcHttpServer *self)
{
  g_return_val_if_fail(BC_IS_HTTP_SERVER(self), FALSE);
  return self->running;
}
