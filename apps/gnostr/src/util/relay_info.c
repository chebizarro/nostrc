#include "relay_info.h"
#include <json-glib/json-glib.h>
#include <string.h>

#ifdef HAVE_SOUP3
#include <libsoup/soup.h>
#endif

/* Cache TTL in seconds (1 hour) */
#define RELAY_INFO_CACHE_TTL_SEC 3600

/* Global cache: normalized URL -> GnostrRelayInfo* */
static GHashTable *relay_info_cache = NULL;
static GMutex cache_mutex;
static gboolean cache_initialized = FALSE;

static void ensure_cache_init(void) {
  if (G_UNLIKELY(!cache_initialized)) {
    g_mutex_init(&cache_mutex);
    relay_info_cache = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              g_free, (GDestroyNotify)gnostr_relay_info_free);
    cache_initialized = TRUE;
  }
}

/* Normalize relay URL for cache key (lowercase, no trailing slash) */
static gchar *normalize_url_for_cache(const gchar *url) {
  if (!url) return NULL;
  gchar *lower = g_ascii_strdown(url, -1);
  /* Remove trailing slash */
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
  /* Already HTTP(S) or unknown, return copy */
  return g_strdup(ws_url);
}

GnostrRelayInfo *gnostr_relay_info_new(void) {
  GnostrRelayInfo *info = g_new0(GnostrRelayInfo, 1);
  info->fetched_at = g_get_real_time() / G_USEC_PER_SEC;
  return info;
}

void gnostr_relay_info_free(GnostrRelayInfo *info) {
  if (!info) return;
  g_free(info->url);
  g_free(info->name);
  g_free(info->description);
  g_free(info->pubkey);
  g_free(info->contact);
  g_free(info->software);
  g_free(info->version);
  g_free(info->icon);
  g_free(info->posting_policy);
  g_free(info->payments_url);
  g_free(info->supported_nips);
  g_free(info->fetch_error);
  if (info->relay_countries) {
    for (gsize i = 0; i < info->relay_countries_count; i++)
      g_free(info->relay_countries[i]);
    g_free(info->relay_countries);
  }
  if (info->language_tags) {
    for (gsize i = 0; i < info->language_tags_count; i++)
      g_free(info->language_tags[i]);
    g_free(info->language_tags);
  }
  if (info->tags) {
    for (gsize i = 0; i < info->tags_count; i++)
      g_free(info->tags[i]);
    g_free(info->tags);
  }
  g_free(info);
}

/* Deep copy of relay info for cache storage */
static GnostrRelayInfo *gnostr_relay_info_copy(const GnostrRelayInfo *src) {
  if (!src) return NULL;
  GnostrRelayInfo *dst = gnostr_relay_info_new();
  dst->url = g_strdup(src->url);
  dst->name = g_strdup(src->name);
  dst->description = g_strdup(src->description);
  dst->pubkey = g_strdup(src->pubkey);
  dst->contact = g_strdup(src->contact);
  dst->software = g_strdup(src->software);
  dst->version = g_strdup(src->version);
  dst->icon = g_strdup(src->icon);
  dst->posting_policy = g_strdup(src->posting_policy);
  dst->payments_url = g_strdup(src->payments_url);
  dst->fetch_error = g_strdup(src->fetch_error);

  if (src->supported_nips && src->supported_nips_count > 0) {
    dst->supported_nips = g_memdup2(src->supported_nips, src->supported_nips_count * sizeof(gint));
    dst->supported_nips_count = src->supported_nips_count;
  }

  dst->max_message_length = src->max_message_length;
  dst->max_subscriptions = src->max_subscriptions;
  dst->max_filters = src->max_filters;
  dst->max_limit = src->max_limit;
  dst->max_subid_length = src->max_subid_length;
  dst->max_event_tags = src->max_event_tags;
  dst->max_content_length = src->max_content_length;
  dst->min_pow_difficulty = src->min_pow_difficulty;
  dst->created_at_lower_limit = src->created_at_lower_limit;
  dst->created_at_upper_limit = src->created_at_upper_limit;
  dst->auth_required = src->auth_required;
  dst->payment_required = src->payment_required;
  dst->restricted_writes = src->restricted_writes;

  if (src->relay_countries && src->relay_countries_count > 0) {
    dst->relay_countries = g_new0(gchar*, src->relay_countries_count);
    for (gsize i = 0; i < src->relay_countries_count; i++)
      dst->relay_countries[i] = g_strdup(src->relay_countries[i]);
    dst->relay_countries_count = src->relay_countries_count;
  }
  if (src->language_tags && src->language_tags_count > 0) {
    dst->language_tags = g_new0(gchar*, src->language_tags_count);
    for (gsize i = 0; i < src->language_tags_count; i++)
      dst->language_tags[i] = g_strdup(src->language_tags[i]);
    dst->language_tags_count = src->language_tags_count;
  }
  if (src->tags && src->tags_count > 0) {
    dst->tags = g_new0(gchar*, src->tags_count);
    for (gsize i = 0; i < src->tags_count; i++)
      dst->tags[i] = g_strdup(src->tags[i]);
    dst->tags_count = src->tags_count;
  }

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

/* Helper to get optional int from JSON object */
static gint json_object_get_int_or_zero(JsonObject *obj, const gchar *member) {
  if (!json_object_has_member(obj, member)) return 0;
  JsonNode *node = json_object_get_member(obj, member);
  if (!node || JSON_NODE_TYPE(node) != JSON_NODE_VALUE) return 0;
  return (gint)json_node_get_int(node);
}

/* Helper to get optional int64 from JSON object */
static gint64 json_object_get_int64_or_zero(JsonObject *obj, const gchar *member) {
  if (!json_object_has_member(obj, member)) return 0;
  JsonNode *node = json_object_get_member(obj, member);
  if (!node || JSON_NODE_TYPE(node) != JSON_NODE_VALUE) return 0;
  return json_node_get_int(node);
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

/* Parse string array from JSON */
static gchar **json_array_to_string_array(JsonArray *arr, gsize *out_count) {
  if (!arr || !out_count) return NULL;
  gsize len = json_array_get_length(arr);
  if (len == 0) { *out_count = 0; return NULL; }
  gchar **result = g_new0(gchar*, len + 1);
  gsize actual = 0;
  for (gsize i = 0; i < len; i++) {
    const gchar *str = json_array_get_string_element(arr, i);
    if (str) result[actual++] = g_strdup(str);
  }
  *out_count = actual;
  return result;
}

GnostrRelayInfo *gnostr_relay_info_parse_json(const gchar *json, const gchar *url) {
  if (!json) return NULL;

  JsonParser *parser = json_parser_new();
  GError *err = NULL;
  if (!json_parser_load_from_data(parser, json, -1, &err)) {
    g_warning("relay_info: JSON parse error: %s", err ? err->message : "unknown");
    g_clear_error(&err);
    g_object_unref(parser);
    return NULL;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!root || JSON_NODE_TYPE(root) != JSON_NODE_OBJECT) {
    g_object_unref(parser);
    return NULL;
  }

  JsonObject *obj = json_node_get_object(root);
  GnostrRelayInfo *info = gnostr_relay_info_new();

  info->url = url ? g_strdup(url) : NULL;
  info->name = json_object_get_string_or_null(obj, "name");
  info->description = json_object_get_string_or_null(obj, "description");
  info->pubkey = json_object_get_string_or_null(obj, "pubkey");
  info->contact = json_object_get_string_or_null(obj, "contact");
  info->software = json_object_get_string_or_null(obj, "software");
  info->version = json_object_get_string_or_null(obj, "version");
  info->icon = json_object_get_string_or_null(obj, "icon");
  info->posting_policy = json_object_get_string_or_null(obj, "posting_policy");
  info->payments_url = json_object_get_string_or_null(obj, "payments_url");

  /* Parse supported_nips array */
  if (json_object_has_member(obj, "supported_nips")) {
    JsonArray *nips_arr = json_object_get_array_member(obj, "supported_nips");
    if (nips_arr) {
      info->supported_nips = json_array_to_int_array(nips_arr, &info->supported_nips_count);
    }
  }

  /* Parse limitation object */
  if (json_object_has_member(obj, "limitation")) {
    JsonObject *lim = json_object_get_object_member(obj, "limitation");
    if (lim) {
      info->max_message_length = json_object_get_int_or_zero(lim, "max_message_length");
      info->max_subscriptions = json_object_get_int_or_zero(lim, "max_subscriptions");
      info->max_filters = json_object_get_int_or_zero(lim, "max_filters");
      info->max_limit = json_object_get_int_or_zero(lim, "max_limit");
      info->max_subid_length = json_object_get_int_or_zero(lim, "max_subid_length");
      info->max_event_tags = json_object_get_int_or_zero(lim, "max_event_tags");
      info->max_content_length = json_object_get_int_or_zero(lim, "max_content_length");
      info->min_pow_difficulty = json_object_get_int_or_zero(lim, "min_pow_difficulty");
      info->created_at_lower_limit = json_object_get_int64_or_zero(lim, "created_at_lower_limit");
      info->created_at_upper_limit = json_object_get_int64_or_zero(lim, "created_at_upper_limit");
      info->auth_required = json_object_get_bool_or_false(lim, "auth_required");
      info->payment_required = json_object_get_bool_or_false(lim, "payment_required");
      info->restricted_writes = json_object_get_bool_or_false(lim, "restricted_writes");
    }
  }

  /* Parse optional string arrays */
  if (json_object_has_member(obj, "relay_countries")) {
    JsonArray *arr = json_object_get_array_member(obj, "relay_countries");
    info->relay_countries = json_array_to_string_array(arr, &info->relay_countries_count);
  }
  if (json_object_has_member(obj, "language_tags")) {
    JsonArray *arr = json_object_get_array_member(obj, "language_tags");
    info->language_tags = json_array_to_string_array(arr, &info->language_tags_count);
  }
  if (json_object_has_member(obj, "tags")) {
    JsonArray *arr = json_object_get_array_member(obj, "tags");
    info->tags = json_array_to_string_array(arr, &info->tags_count);
  }

  g_object_unref(parser);
  return info;
}

/* ---- Cache operations ---- */

GnostrRelayInfo *gnostr_relay_info_cache_get(const gchar *relay_url) {
  ensure_cache_init();
  if (!relay_url) return NULL;

  gchar *key = normalize_url_for_cache(relay_url);
  GnostrRelayInfo *cached = NULL;

  g_mutex_lock(&cache_mutex);
  GnostrRelayInfo *entry = g_hash_table_lookup(relay_info_cache, key);
  if (entry) {
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    if (now - entry->fetched_at < RELAY_INFO_CACHE_TTL_SEC) {
      cached = gnostr_relay_info_copy(entry);
    } else {
      /* Expired, remove from cache */
      g_hash_table_remove(relay_info_cache, key);
    }
  }
  g_mutex_unlock(&cache_mutex);

  g_free(key);
  return cached;
}

void gnostr_relay_info_cache_put(GnostrRelayInfo *info) {
  ensure_cache_init();
  if (!info || !info->url) return;

  gchar *key = normalize_url_for_cache(info->url);
  GnostrRelayInfo *copy = gnostr_relay_info_copy(info);

  g_mutex_lock(&cache_mutex);
  g_hash_table_replace(relay_info_cache, key, copy); /* key ownership transferred */
  g_mutex_unlock(&cache_mutex);
}

void gnostr_relay_info_cache_clear(void) {
  ensure_cache_init();
  g_mutex_lock(&cache_mutex);
  g_hash_table_remove_all(relay_info_cache);
  g_mutex_unlock(&cache_mutex);
}

/* ---- Async fetch implementation ---- */

#ifdef HAVE_SOUP3

typedef struct {
  gchar *relay_url;
  GTask *task;
} FetchContext;

static void fetch_context_free(FetchContext *ctx) {
  if (!ctx) return;
  g_free(ctx->relay_url);
  g_free(ctx);
}

static void on_soup_message_complete(GObject *source, GAsyncResult *result, gpointer user_data) {
  SoupSession *session = SOUP_SESSION(source);
  FetchContext *ctx = (FetchContext*)user_data;
  GError *err = NULL;

  GBytes *body = soup_session_send_and_read_finish(session, result, &err);

  if (err) {
    g_task_return_error(ctx->task, err);
    g_object_unref(ctx->task);
    fetch_context_free(ctx);
    return;
  }

  gsize len;
  const gchar *data = g_bytes_get_data(body, &len);

  GnostrRelayInfo *info = gnostr_relay_info_parse_json(data, ctx->relay_url);
  g_bytes_unref(body);

  if (!info) {
    g_task_return_new_error(ctx->task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Failed to parse NIP-11 response from %s", ctx->relay_url);
  } else {
    /* Store in cache */
    gnostr_relay_info_cache_put(info);
    g_task_return_pointer(ctx->task, info, (GDestroyNotify)gnostr_relay_info_free);
  }

  g_object_unref(ctx->task);
  fetch_context_free(ctx);
}

void gnostr_relay_info_fetch_async(const gchar *relay_url,
                                   GCancellable *cancellable,
                                   GAsyncReadyCallback callback,
                                   gpointer user_data) {
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);

  if (!relay_url) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "relay_url is NULL");
    g_object_unref(task);
    return;
  }

  /* Check cache first */
  GnostrRelayInfo *cached = gnostr_relay_info_cache_get(relay_url);
  if (cached) {
    g_task_return_pointer(task, cached, (GDestroyNotify)gnostr_relay_info_free);
    g_object_unref(task);
    return;
  }

  gchar *http_url = ws_url_to_http(relay_url);

  SoupSession *session = soup_session_new();
  soup_session_set_user_agent(session, "gnostr/1.0");

  SoupMessage *msg = soup_message_new("GET", http_url);
  if (!msg) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Invalid relay URL: %s", relay_url);
    g_object_unref(task);
    g_object_unref(session);
    g_free(http_url);
    return;
  }

  /* Set NIP-11 required Accept header */
  SoupMessageHeaders *headers = soup_message_get_request_headers(msg);
  soup_message_headers_replace(headers, "Accept", "application/nostr+json");

  FetchContext *ctx = g_new0(FetchContext, 1);
  ctx->relay_url = g_strdup(relay_url);
  ctx->task = task;

  soup_session_send_and_read_async(session, msg, G_PRIORITY_DEFAULT,
                                    cancellable, on_soup_message_complete, ctx);

  g_object_unref(msg);
  g_object_unref(session);
  g_free(http_url);
}

GnostrRelayInfo *gnostr_relay_info_fetch_finish(GAsyncResult *result, GError **error) {
  return g_task_propagate_pointer(G_TASK(result), error);
}

#else /* !HAVE_SOUP3 */

/* Fallback: Return error indicating soup3 not available */
void gnostr_relay_info_fetch_async(const gchar *relay_url,
                                   GCancellable *cancellable,
                                   GAsyncReadyCallback callback,
                                   gpointer user_data) {
  (void)relay_url;
  (void)cancellable;
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                          "NIP-11 fetch requires libsoup3");
  g_object_unref(task);
}

GnostrRelayInfo *gnostr_relay_info_fetch_finish(GAsyncResult *result, GError **error) {
  return g_task_propagate_pointer(G_TASK(result), error);
}

#endif /* HAVE_SOUP3 */

/* ---- Formatting helpers ---- */

gchar *gnostr_relay_info_format_nips(const GnostrRelayInfo *info) {
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

gchar *gnostr_relay_info_format_limitations(const GnostrRelayInfo *info) {
  if (!info) return g_strdup("(none specified)");

  GString *str = g_string_new(NULL);
  gboolean has_any = FALSE;

  if (info->max_message_length > 0) {
    g_string_append_printf(str, "Max message: %d bytes\n", info->max_message_length);
    has_any = TRUE;
  }
  if (info->max_subscriptions > 0) {
    g_string_append_printf(str, "Max subscriptions: %d\n", info->max_subscriptions);
    has_any = TRUE;
  }
  if (info->max_filters > 0) {
    g_string_append_printf(str, "Max filters: %d\n", info->max_filters);
    has_any = TRUE;
  }
  if (info->max_limit > 0) {
    g_string_append_printf(str, "Max limit: %d\n", info->max_limit);
    has_any = TRUE;
  }
  if (info->max_subid_length > 0) {
    g_string_append_printf(str, "Max sub ID length: %d\n", info->max_subid_length);
    has_any = TRUE;
  }
  if (info->max_event_tags > 0) {
    g_string_append_printf(str, "Max event tags: %d\n", info->max_event_tags);
    has_any = TRUE;
  }
  if (info->max_content_length > 0) {
    g_string_append_printf(str, "Max content length: %d\n", info->max_content_length);
    has_any = TRUE;
  }
  if (info->min_pow_difficulty > 0) {
    g_string_append_printf(str, "Min PoW difficulty: %d\n", info->min_pow_difficulty);
    has_any = TRUE;
  }
  if (info->created_at_lower_limit > 0) {
    g_string_append_printf(str, "Max event age: %" G_GINT64_FORMAT " seconds\n", info->created_at_lower_limit);
    has_any = TRUE;
  }
  if (info->created_at_upper_limit > 0) {
    g_string_append_printf(str, "Max future timestamp: %" G_GINT64_FORMAT " seconds\n", info->created_at_upper_limit);
    has_any = TRUE;
  }
  if (info->auth_required) {
    g_string_append(str, "Auth required: Yes\n");
    has_any = TRUE;
  }
  if (info->payment_required) {
    g_string_append(str, "Payment required: Yes\n");
    has_any = TRUE;
  }
  if (info->restricted_writes) {
    g_string_append(str, "Restricted writes: Yes\n");
    has_any = TRUE;
  }

  if (!has_any) {
    g_string_free(str, TRUE);
    return g_strdup("(none specified)");
  }

  /* Remove trailing newline */
  if (str->len > 0 && str->str[str->len - 1] == '\n') {
    g_string_truncate(str, str->len - 1);
  }

  return g_string_free(str, FALSE);
}

/* ---- Event Validation Against Relay Limits (NIP-11) ---- */

GnostrRelayValidationResult *gnostr_relay_validation_result_new(void) {
  GnostrRelayValidationResult *result = g_new0(GnostrRelayValidationResult, 1);
  result->violations = GNOSTR_LIMIT_NONE;
  return result;
}

void gnostr_relay_validation_result_free(GnostrRelayValidationResult *result) {
  if (!result) return;
  g_free(result->relay_url);
  g_free(result->relay_name);
  g_free(result);
}

gboolean gnostr_relay_validation_result_is_valid(const GnostrRelayValidationResult *result) {
  if (!result) return TRUE;
  return result->violations == GNOSTR_LIMIT_NONE;
}

gchar *gnostr_relay_validation_result_format_errors(const GnostrRelayValidationResult *result) {
  if (!result || result->violations == GNOSTR_LIMIT_NONE) return NULL;

  GString *str = g_string_new(NULL);
  const gchar *relay_desc = result->relay_name ? result->relay_name :
                            (result->relay_url ? result->relay_url : "relay");

  if (result->violations & GNOSTR_LIMIT_CONTENT_LENGTH) {
    g_string_append_printf(str, "%s: Content too long (%d bytes, max %d)\n",
                           relay_desc, result->content_length, result->max_content_length);
  }

  if (result->violations & GNOSTR_LIMIT_EVENT_TAGS) {
    g_string_append_printf(str, "%s: Too many tags (%d, max %d)\n",
                           relay_desc, result->tag_count, result->max_tags);
  }

  if (result->violations & GNOSTR_LIMIT_MESSAGE_LENGTH) {
    g_string_append_printf(str, "%s: Message too large (%d bytes, max %d)\n",
                           relay_desc, result->message_length, result->max_message_length);
  }

  if (result->violations & GNOSTR_LIMIT_TIMESTAMP_TOO_OLD) {
    g_string_append_printf(str, "%s: Event timestamp too old\n", relay_desc);
  }

  if (result->violations & GNOSTR_LIMIT_TIMESTAMP_TOO_NEW) {
    g_string_append_printf(str, "%s: Event timestamp too far in the future\n", relay_desc);
  }

  if (result->violations & GNOSTR_LIMIT_POW_REQUIRED) {
    g_string_append_printf(str, "%s: Proof-of-work required\n", relay_desc);
  }

  if (result->violations & GNOSTR_LIMIT_AUTH_REQUIRED) {
    g_string_append_printf(str, "%s: Authentication required\n", relay_desc);
  }

  if (result->violations & GNOSTR_LIMIT_PAYMENT_REQUIRED) {
    g_string_append_printf(str, "%s: Payment required\n", relay_desc);
  }

  if (result->violations & GNOSTR_LIMIT_RESTRICTED_WRITES) {
    g_string_append_printf(str, "%s: Writes are restricted\n", relay_desc);
  }

  /* Remove trailing newline */
  if (str->len > 0 && str->str[str->len - 1] == '\n') {
    g_string_truncate(str, str->len - 1);
  }

  return g_string_free(str, FALSE);
}

GnostrRelayValidationResult *gnostr_relay_info_validate_event(
    const GnostrRelayInfo *info,
    const gchar *content,
    gssize content_length,
    gint tag_count,
    gint64 created_at,
    gssize serialized_length) {

  GnostrRelayValidationResult *result = gnostr_relay_validation_result_new();

  /* If no relay info, assume no limits (graceful degradation) */
  if (!info) {
    return result;
  }

  result->relay_url = g_strdup(info->url);
  result->relay_name = g_strdup(info->name);

  /* Calculate content length if not provided */
  gint actual_content_length = (content_length >= 0) ? (gint)content_length :
                               (content ? (gint)strlen(content) : 0);
  result->content_length = actual_content_length;
  result->tag_count = tag_count;
  result->event_created_at = created_at;

  /* Check content length */
  if (info->max_content_length > 0 && actual_content_length > info->max_content_length) {
    result->violations |= GNOSTR_LIMIT_CONTENT_LENGTH;
    result->max_content_length = info->max_content_length;
  }

  /* Check tag count */
  if (info->max_event_tags > 0 && tag_count > info->max_event_tags) {
    result->violations |= GNOSTR_LIMIT_EVENT_TAGS;
    result->max_tags = info->max_event_tags;
  }

  /* Check message length (serialized event) */
  if (serialized_length >= 0 && info->max_message_length > 0 &&
      serialized_length > info->max_message_length) {
    result->violations |= GNOSTR_LIMIT_MESSAGE_LENGTH;
    result->message_length = (gint)serialized_length;
    result->max_message_length = info->max_message_length;
  }

  /* Check timestamp bounds */
  if (created_at > 0) {
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;

    /* created_at_lower_limit: how many seconds before now is allowed */
    if (info->created_at_lower_limit > 0) {
      gint64 min_allowed = now - info->created_at_lower_limit;
      result->min_allowed_timestamp = min_allowed;
      if (created_at < min_allowed) {
        result->violations |= GNOSTR_LIMIT_TIMESTAMP_TOO_OLD;
      }
    }

    /* created_at_upper_limit: how many seconds after now is allowed */
    if (info->created_at_upper_limit > 0) {
      gint64 max_allowed = now + info->created_at_upper_limit;
      result->max_allowed_timestamp = max_allowed;
      if (created_at > max_allowed) {
        result->violations |= GNOSTR_LIMIT_TIMESTAMP_TOO_NEW;
      }
    }
  }

  /* Check PoW requirement (just flag it, we don't calculate PoW here) */
  if (info->min_pow_difficulty > 0) {
    result->violations |= GNOSTR_LIMIT_POW_REQUIRED;
  }

  return result;
}

GnostrRelayValidationResult *gnostr_relay_info_validate_for_publishing(
    const GnostrRelayInfo *info) {

  GnostrRelayValidationResult *result = gnostr_relay_validation_result_new();

  if (!info) {
    return result;
  }

  result->relay_url = g_strdup(info->url);
  result->relay_name = g_strdup(info->name);

  if (info->auth_required) {
    result->violations |= GNOSTR_LIMIT_AUTH_REQUIRED;
  }

  if (info->payment_required) {
    result->violations |= GNOSTR_LIMIT_PAYMENT_REQUIRED;
  }

  if (info->restricted_writes) {
    result->violations |= GNOSTR_LIMIT_RESTRICTED_WRITES;
  }

  return result;
}
