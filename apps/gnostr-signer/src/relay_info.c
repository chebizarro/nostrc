/* relay_info.c - NIP-11 Relay Information Document implementation */
#include "relay_info.h"
#include <json-glib/json-glib.h>
#include <string.h>

/* Cache TTL in seconds (1 hour) */
#define RELAY_INFO_CACHE_TTL_SEC 3600

/* Global cache: normalized URL -> RelayInfo* */
static GHashTable *relay_info_cache = NULL;
static GMutex cache_mutex;
static gboolean cache_initialized = FALSE;

static void ensure_cache_init(void) {
  if (G_UNLIKELY(!cache_initialized)) {
    g_mutex_init(&cache_mutex);
    relay_info_cache = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              g_free, (GDestroyNotify)relay_info_free);
    cache_initialized = TRUE;
  }
}

/* Normalize relay URL for cache key (lowercase, no trailing slash) */
static gchar *normalize_url_for_cache(const gchar *url) {
  if (!url) return NULL;
  gchar *lower = g_ascii_strdown(url, -1);
  gsize len = strlen(lower);
  while (len > 0 && lower[len - 1] == '/') {
    lower[--len] = '\0';
  }
  return lower;
}

/* Convert ws:// or wss:// to http:// or https:// */
static gchar *ws_url_to_http(const gchar *ws_url) {
  if (!ws_url) return NULL;
  if (g_str_has_prefix(ws_url, "wss://")) {
    return g_strconcat("https://", ws_url + 6, NULL);
  } else if (g_str_has_prefix(ws_url, "ws://")) {
    return g_strconcat("http://", ws_url + 5, NULL);
  }
  return g_strdup(ws_url);
}

RelayInfo *relay_info_new(void) {
  RelayInfo *info = g_new0(RelayInfo, 1);
  info->fetched_at = g_get_real_time() / G_USEC_PER_SEC;
  return info;
}

void relay_info_free(RelayInfo *info) {
  if (!info) return;
  g_free(info->url);
  g_free(info->name);
  g_free(info->description);
  g_free(info->software);
  g_free(info->version);
  g_free(info->contact);
  g_free(info->supported_nips);
  g_free(info->fetch_error);
  g_free(info);
}

/* Deep copy of relay info for cache storage */
static RelayInfo *relay_info_copy(const RelayInfo *src) {
  if (!src) return NULL;
  RelayInfo *dst = relay_info_new();
  dst->url = g_strdup(src->url);
  dst->name = g_strdup(src->name);
  dst->description = g_strdup(src->description);
  dst->software = g_strdup(src->software);
  dst->version = g_strdup(src->version);
  dst->contact = g_strdup(src->contact);
  dst->fetch_error = g_strdup(src->fetch_error);

  if (src->supported_nips && src->supported_nips_count > 0) {
    dst->supported_nips = g_memdup2(src->supported_nips, src->supported_nips_count * sizeof(gint));
    dst->supported_nips_count = src->supported_nips_count;
  }

  dst->auth_required = src->auth_required;
  dst->payment_required = src->payment_required;
  dst->fetched_at = src->fetched_at;
  dst->fetch_failed = src->fetch_failed;
  return dst;
}

/* Helper to get optional string from JSON object */
static gchar *json_object_get_string_or_null(JsonObject *obj, const gchar *member) {
  if (!json_object_has_member(obj, member)) return NULL;
  JsonNode *node = json_object_get_member(obj, member);
  if (!node || JSON_NODE_TYPE(node) != JSON_NODE_VALUE) return NULL;
  const gchar *val = json_node_get_string(node);
  return val ? g_strdup(val) : NULL;
}

/* Helper to get optional bool from JSON object */
static gboolean json_object_get_bool_or_false(JsonObject *obj, const gchar *member) {
  if (!json_object_has_member(obj, member)) return FALSE;
  JsonNode *node = json_object_get_member(obj, member);
  if (!node || JSON_NODE_TYPE(node) != JSON_NODE_VALUE) return FALSE;
  return json_node_get_boolean(node);
}

/* Parse int array from JSON */
static gint *json_array_to_int_array(JsonArray *arr, gsize *out_count) {
  if (!arr || !out_count) return NULL;
  gsize len = json_array_get_length(arr);
  if (len == 0) { *out_count = 0; return NULL; }
  gint *result = g_new(gint, len);
  gsize actual = 0;
  for (gsize i = 0; i < len; i++) {
    JsonNode *node = json_array_get_element(arr, i);
    if (node && JSON_NODE_TYPE(node) == JSON_NODE_VALUE) {
      result[actual++] = (gint)json_node_get_int(node);
    }
  }
  *out_count = actual;
  return result;
}

RelayInfo *relay_info_parse_json(const gchar *json, const gchar *url) {
  if (!json) return NULL;

  g_autoptr(JsonParser) parser = json_parser_new();
  GError *err = NULL;
  if (!json_parser_load_from_data(parser, json, -1, &err)) {
    g_warning("relay_info: JSON parse error: %s", err ? err->message : "unknown");
    g_clear_error(&err);
    return NULL;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!root || JSON_NODE_TYPE(root) != JSON_NODE_OBJECT) {
    return NULL;
  }

  JsonObject *obj = json_node_get_object(root);
  RelayInfo *info = relay_info_new();

  info->url = url ? g_strdup(url) : NULL;
  info->name = json_object_get_string_or_null(obj, "name");
  info->description = json_object_get_string_or_null(obj, "description");
  info->software = json_object_get_string_or_null(obj, "software");
  info->version = json_object_get_string_or_null(obj, "version");
  info->contact = json_object_get_string_or_null(obj, "contact");

  /* Parse supported_nips array */
  if (json_object_has_member(obj, "supported_nips")) {
    JsonArray *nips_arr = json_object_get_array_member(obj, "supported_nips");
    if (nips_arr) {
      info->supported_nips = json_array_to_int_array(nips_arr, &info->supported_nips_count);
    }
  }

  /* Parse limitation object for key flags */
  if (json_object_has_member(obj, "limitation")) {
    JsonObject *lim = json_object_get_object_member(obj, "limitation");
    if (lim) {
      info->auth_required = json_object_get_bool_or_false(lim, "auth_required");
      info->payment_required = json_object_get_bool_or_false(lim, "payment_required");
    }
  }

  return info;
}

/* ---- Cache operations ---- */

RelayInfo *relay_info_cache_get(const gchar *relay_url) {
  ensure_cache_init();
  if (!relay_url) return NULL;

  gchar *key = normalize_url_for_cache(relay_url);
  RelayInfo *cached = NULL;

  g_mutex_lock(&cache_mutex);
  RelayInfo *entry = g_hash_table_lookup(relay_info_cache, key);
  if (entry) {
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    if (now - entry->fetched_at < RELAY_INFO_CACHE_TTL_SEC) {
      cached = relay_info_copy(entry);
    } else {
      /* Expired, remove from cache */
      g_hash_table_remove(relay_info_cache, key);
    }
  }
  g_mutex_unlock(&cache_mutex);

  g_free(key);
  return cached;
}

void relay_info_cache_put(RelayInfo *info) {
  ensure_cache_init();
  if (!info || !info->url) return;

  gchar *key = normalize_url_for_cache(info->url);
  RelayInfo *copy = relay_info_copy(info);

  g_mutex_lock(&cache_mutex);
  g_hash_table_replace(relay_info_cache, key, copy); /* key ownership transferred */
  g_mutex_unlock(&cache_mutex);
}

/* ---- Async fetch implementation using GIO ---- */

typedef struct {
  gchar *relay_url;
  RelayInfoCallback callback;
  gpointer user_data;
  GInputStream *input_stream;
  GByteArray *buffer;
} FetchContext;

static void fetch_context_free(FetchContext *ctx) {
  if (!ctx) return;
  g_free(ctx->relay_url);
  if (ctx->input_stream) g_object_unref(ctx->input_stream);
  if (ctx->buffer) g_byte_array_unref(ctx->buffer);
  g_free(ctx);
}

static void on_read_complete(GObject *source, GAsyncResult *result, gpointer user_data);

static void on_connection_complete(GObject *source, GAsyncResult *result, gpointer user_data) {
  FetchContext *ctx = (FetchContext*)user_data;
  GError *err = NULL;

  GSocketConnection *conn = g_socket_client_connect_to_uri_finish(
    G_SOCKET_CLIENT(source), result, &err);

  if (err || !conn) {
    if (ctx->callback) {
      ctx->callback(NULL, err ? err->message : "Connection failed", ctx->user_data);
    }
    g_clear_error(&err);
    fetch_context_free(ctx);
    return;
  }

  /* Build HTTP request with NIP-11 Accept header */
  gchar *http_url = ws_url_to_http(ctx->relay_url);
  GUri *uri = g_uri_parse(http_url, G_URI_FLAGS_NONE, NULL);
  g_free(http_url);

  if (!uri) {
    if (ctx->callback) {
      ctx->callback(NULL, "Invalid URL", ctx->user_data);
    }
    g_object_unref(conn);
    fetch_context_free(ctx);
    return;
  }

  const gchar *host = g_uri_get_host(uri);
  const gchar *path = g_uri_get_path(uri);
  if (!path || !*path) path = "/";

  g_autofree gchar *request = g_strdup_printf(
    "GET %s HTTP/1.1\r\n"
    "Host: %s\r\n"
    "Accept: application/nostr+json\r\n"
    "User-Agent: gnostr-signer/1.0\r\n"
    "Connection: close\r\n"
    "\r\n",
    path, host);

  g_uri_unref(uri);

  GOutputStream *output = g_io_stream_get_output_stream(G_IO_STREAM(conn));
  gsize bytes_written;
  if (!g_output_stream_write_all(output, request, strlen(request), &bytes_written, NULL, &err)) {
    if (ctx->callback) {
      ctx->callback(NULL, err ? err->message : "Write failed", ctx->user_data);
    }
    g_clear_error(&err);
    g_object_unref(conn);
    fetch_context_free(ctx);
    return;
  }

  /* Read response */
  ctx->input_stream = g_object_ref(g_io_stream_get_input_stream(G_IO_STREAM(conn)));
  ctx->buffer = g_byte_array_new();

  guint8 *read_buf = g_new(guint8, 4096);
  g_input_stream_read_async(ctx->input_stream, read_buf, 4096,
                            G_PRIORITY_DEFAULT, NULL, on_read_complete, ctx);

  g_object_unref(conn);
}

static void on_read_complete(GObject *source, GAsyncResult *result, gpointer user_data) {
  FetchContext *ctx = (FetchContext*)user_data;
  GError *err = NULL;

  gssize bytes_read = g_input_stream_read_finish(G_INPUT_STREAM(source), result, &err);

  if (err) {
    if (ctx->callback) {
      ctx->callback(NULL, err->message, ctx->user_data);
    }
    g_clear_error(&err);
    fetch_context_free(ctx);
    return;
  }

  /* Get the buffer that was passed to read_async (stored in GTask) */
  guint8 *read_buf = g_task_get_task_data(G_TASK(result));
  if (!read_buf) read_buf = g_new(guint8, 4096);

  if (bytes_read > 0) {
    g_byte_array_append(ctx->buffer, read_buf, bytes_read);
    /* Continue reading */
    g_input_stream_read_async(ctx->input_stream, read_buf, 4096,
                              G_PRIORITY_DEFAULT, NULL, on_read_complete, ctx);
    return;
  }

  g_free(read_buf);

  /* Done reading - parse response */
  g_byte_array_append(ctx->buffer, (guint8*)"", 1); /* Null terminate */
  gchar *response = (gchar*)ctx->buffer->data;

  /* Find HTTP body (after \r\n\r\n) */
  gchar *body = strstr(response, "\r\n\r\n");
  if (!body) {
    if (ctx->callback) {
      ctx->callback(NULL, "Invalid HTTP response", ctx->user_data);
    }
    fetch_context_free(ctx);
    return;
  }
  body += 4; /* Skip \r\n\r\n */

  /* Parse JSON */
  RelayInfo *info = relay_info_parse_json(body, ctx->relay_url);

  if (info) {
    relay_info_cache_put(info);
    if (ctx->callback) {
      ctx->callback(info, NULL, ctx->user_data);
    }
  } else {
    if (ctx->callback) {
      ctx->callback(NULL, "Failed to parse NIP-11 response", ctx->user_data);
    }
  }

  fetch_context_free(ctx);
}

void relay_info_fetch_async(const gchar *relay_url,
                            RelayInfoCallback callback,
                            gpointer user_data) {
  if (!relay_url) {
    if (callback) callback(NULL, "relay_url is NULL", user_data);
    return;
  }

  /* Check cache first */
  RelayInfo *cached = relay_info_cache_get(relay_url);
  if (cached) {
    if (callback) callback(cached, NULL, user_data);
    return;
  }

  /* Convert URL and connect */
  gchar *http_url = ws_url_to_http(relay_url);

  FetchContext *ctx = g_new0(FetchContext, 1);
  ctx->relay_url = g_strdup(relay_url);
  ctx->callback = callback;
  ctx->user_data = user_data;

  g_autoptr(GSocketClient) client = g_socket_client_new();
  g_socket_client_set_tls(client, g_str_has_prefix(http_url, "https://"));

  g_socket_client_connect_to_uri_async(client, http_url, 443,
                                        NULL, on_connection_complete, ctx);

  g_free(http_url);
}

gchar *relay_info_format_nips(const RelayInfo *info) {
  if (!info || !info->supported_nips || info->supported_nips_count == 0) {
    return g_strdup("(none)");
  }

  GString *str = g_string_new(NULL);
  for (gsize i = 0; i < info->supported_nips_count; i++) {
    if (i > 0) g_string_append(str, ", ");
    g_string_append_printf(str, "%d", info->supported_nips[i]);
  }
  return g_string_free(str, FALSE);
}
