/*
 * bc-author-hints.c - Resolve ?as=<pubkey> author hints via kind:10063
 *
 * SPDX-License-Identifier: MIT
 *
 * Queries a Nostr relay (via libsoup WebSocket) for kind:10063 events
 * to find which Blossom servers a given pubkey uses.
 *
 * Results are cached in a GHashTable with expiry timestamps.
 */

#include "bc-author-hints.h"

#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <string.h>

/* Cache entry: list of server URLs + expiry */
typedef struct {
  gchar **server_urls;   /* NULL-terminated, or NULL if no servers found */
  gint64  expires_at;    /* g_get_monotonic_time() microseconds */
} CacheEntry;

static void
cache_entry_free(CacheEntry *e)
{
  if (!e) return;
  g_strfreev(e->server_urls);
  g_free(e);
}

struct _BcAuthorHintCache {
  gchar      *relay_url;
  guint       ttl_seconds;
  GHashTable *entries;    /* pubkey_hex → CacheEntry* */
  GMutex      lock;
};

BcAuthorHintCache *
bc_author_hint_cache_new(const gchar *relay_url, guint ttl_seconds)
{
  BcAuthorHintCache *cache = g_new0(BcAuthorHintCache, 1);
  cache->relay_url = (relay_url && relay_url[0]) ? g_strdup(relay_url) : NULL;
  cache->ttl_seconds = ttl_seconds;
  cache->entries = g_hash_table_new_full(g_str_hash, g_str_equal,
                                          g_free, (GDestroyNotify)cache_entry_free);
  g_mutex_init(&cache->lock);
  return cache;
}

void
bc_author_hint_cache_free(BcAuthorHintCache *cache)
{
  if (!cache) return;
  g_free(cache->relay_url);
  g_hash_table_destroy(cache->entries);
  g_mutex_clear(&cache->lock);
  g_free(cache);
}

/* ---- WebSocket relay query context ---- */

typedef struct {
  GMainLoop  *loop;
  gchar     **result_urls;
  gboolean    got_eose;
} WsQueryCtx;

/*
 * Parse a kind:10063 event's tags to extract "r" tag values (server URLs).
 * Event format: {"tags":[["r","https://server1.com"],["r","https://server2.com"]]}
 */
static gchar **
parse_server_urls_from_event(JsonObject *event_obj)
{
  JsonArray *tags = json_object_get_array_member(event_obj, "tags");
  if (tags == NULL) return NULL;

  g_autoptr(GPtrArray) urls = g_ptr_array_new_with_free_func(g_free);
  guint n = json_array_get_length(tags);

  for (guint i = 0; i < n; i++) {
    JsonArray *tag = json_array_get_array_element(tags, i);
    if (tag == NULL || json_array_get_length(tag) < 2) continue;

    const gchar *tag_name = json_array_get_string_element(tag, 0);
    if (tag_name == NULL || strcmp(tag_name, "r") != 0) continue;

    const gchar *url = json_array_get_string_element(tag, 1);
    if (url != NULL && url[0] != '\0')
      g_ptr_array_add(urls, g_strdup(url));
  }

  if (urls->len == 0) return NULL;

  g_ptr_array_add(urls, NULL);
  return (gchar **)g_ptr_array_steal(urls, NULL);
}

static void
on_ws_message(SoupWebsocketConnection *ws,
              SoupWebsocketDataType    type,
              GBytes                  *message,
              gpointer                 user_data)
{
  WsQueryCtx *ctx = user_data;
  if (type != SOUP_WEBSOCKET_DATA_TEXT) return;

  gsize len = 0;
  const gchar *text = g_bytes_get_data(message, &len);
  if (text == NULL || len == 0) return;

  /* Parse the Nostr envelope: ["EVENT","sub_id",{...}] or ["EOSE","sub_id"] */
  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, text, (gssize)len, NULL)) return;

  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_ARRAY(root)) return;

  JsonArray *envelope = json_node_get_array(root);
  guint elen = json_array_get_length(envelope);
  if (elen < 2) return;

  const gchar *verb = json_array_get_string_element(envelope, 0);
  if (verb == NULL) return;

  if (strcmp(verb, "EVENT") == 0 && elen >= 3) {
    JsonObject *event_obj = json_array_get_object_element(envelope, 2);
    if (event_obj != NULL && ctx->result_urls == NULL) {
      ctx->result_urls = parse_server_urls_from_event(event_obj);
    }
  } else if (strcmp(verb, "EOSE") == 0) {
    ctx->got_eose = TRUE;
    g_main_loop_quit(ctx->loop);
  }
}

static void
on_ws_closed(SoupWebsocketConnection *ws, gpointer user_data)
{
  WsQueryCtx *ctx = user_data;
  g_main_loop_quit(ctx->loop);
}

static gboolean
on_ws_timeout(gpointer user_data)
{
  WsQueryCtx *ctx = user_data;
  g_main_loop_quit(ctx->loop);
  return G_SOURCE_REMOVE;
}

/* Async WebSocket connect callback */
typedef struct {
  SoupWebsocketConnection *ws;
  GMainLoop               *loop;
  GError                  *error;
} WsConnectCtx;

static void
on_ws_connected(GObject *source, GAsyncResult *result, gpointer user_data)
{
  WsConnectCtx *cctx = user_data;
  cctx->ws = soup_session_websocket_connect_finish(
    SOUP_SESSION(source), result, &cctx->error);
  g_main_loop_quit(cctx->loop);
}

/*
 * Query a Nostr relay for kind:10063 events from the given author.
 * Uses a dedicated GMainContext to run synchronously with a timeout.
 * Returns NULL-terminated array of server URLs, or NULL.
 */
static gchar **
query_relay_for_blossom_servers(const gchar *relay_url, const gchar *pubkey_hex)
{
  g_autoptr(GMainContext) ctx = g_main_context_new();
  g_main_context_push_thread_default(ctx);

  g_autoptr(SoupSession) session = soup_session_new();
  soup_session_set_timeout(session, 5);

  /* Build WebSocket URI */
  g_autoptr(GUri) uri = g_uri_parse(relay_url, G_URI_FLAGS_NONE, NULL);
  if (uri == NULL) {
    g_main_context_pop_thread_default(ctx);
    return NULL;
  }

  /* Open WebSocket connection asynchronously, then block until done */
  g_autoptr(SoupMessage) ws_msg = soup_message_new_from_uri("GET", uri);

  WsConnectCtx conn_ctx = { .ws = NULL, .loop = NULL, .error = NULL };
  conn_ctx.loop = g_main_loop_new(ctx, FALSE);

  soup_session_websocket_connect_async(
    session, ws_msg, NULL, NULL, G_PRIORITY_DEFAULT, NULL,
    on_ws_connected, &conn_ctx);

  /* Add connection timeout (5 seconds) */
  GSource *conn_timeout = g_timeout_source_new(5000);
  g_source_set_callback(conn_timeout, on_ws_timeout,
                        &(WsQueryCtx){ .loop = conn_ctx.loop }, NULL);
  g_source_attach(conn_timeout, ctx);

  g_main_loop_run(conn_ctx.loop);

  g_source_destroy(conn_timeout);
  g_source_unref(conn_timeout);
  g_main_loop_unref(conn_ctx.loop);

  if (conn_ctx.ws == NULL) {
    g_debug("author-hints: relay connect failed: %s",
            conn_ctx.error ? conn_ctx.error->message : "timeout");
    g_clear_error(&conn_ctx.error);
    g_main_context_pop_thread_default(ctx);
    return NULL;
  }

  /* Now we have an open WebSocket — query for kind:10063 */
  WsQueryCtx query_ctx = { .loop = NULL, .result_urls = NULL, .got_eose = FALSE };
  query_ctx.loop = g_main_loop_new(ctx, FALSE);

  g_signal_connect(conn_ctx.ws, "message", G_CALLBACK(on_ws_message), &query_ctx);
  g_signal_connect(conn_ctx.ws, "closed",  G_CALLBACK(on_ws_closed),  &query_ctx);

  /* Send REQ for kind:10063 */
  g_autofree gchar *req = g_strdup_printf(
    "[\"REQ\",\"bh\",{\"kinds\":[10063],\"authors\":[\"%s\"],\"limit\":1}]",
    pubkey_hex);
  soup_websocket_connection_send_text(conn_ctx.ws, req);

  /* Add query timeout (3 seconds) */
  GSource *query_timeout = g_timeout_source_new(3000);
  g_source_set_callback(query_timeout, on_ws_timeout, &query_ctx, NULL);
  g_source_attach(query_timeout, ctx);

  g_main_loop_run(query_ctx.loop);

  /* Clean up WebSocket */
  if (soup_websocket_connection_get_state(conn_ctx.ws) == SOUP_WEBSOCKET_STATE_OPEN) {
    soup_websocket_connection_send_text(conn_ctx.ws, "[\"CLOSE\",\"bh\"]");
    soup_websocket_connection_close(conn_ctx.ws, SOUP_WEBSOCKET_CLOSE_NORMAL, NULL);
  }

  g_source_destroy(query_timeout);
  g_source_unref(query_timeout);
  g_main_loop_unref(query_ctx.loop);
  g_object_unref(conn_ctx.ws);
  g_main_context_pop_thread_default(ctx);

  return query_ctx.result_urls;
}

gchar **
bc_author_hint_cache_lookup(BcAuthorHintCache *cache,
                             const gchar *pubkey_hex)
{
  if (cache == NULL || cache->relay_url == NULL) return NULL;
  if (pubkey_hex == NULL || strlen(pubkey_hex) != 64) return NULL;

  /* Check cache first */
  g_mutex_lock(&cache->lock);

  CacheEntry *entry = g_hash_table_lookup(cache->entries, pubkey_hex);
  if (entry != NULL) {
    gint64 now = g_get_monotonic_time();
    if (now < entry->expires_at) {
      gchar **result = g_strdupv(entry->server_urls);
      g_mutex_unlock(&cache->lock);
      return result;
    }
    /* Expired — remove and re-query */
    g_hash_table_remove(cache->entries, pubkey_hex);
  }

  g_mutex_unlock(&cache->lock);

  /* Query relay */
  g_message("author-hints: querying %s for kind:10063 from %s",
            cache->relay_url, pubkey_hex);

  gchar **server_urls = query_relay_for_blossom_servers(cache->relay_url, pubkey_hex);

  if (server_urls != NULL)
    g_message("author-hints: found %u server(s) for %s",
              g_strv_length(server_urls), pubkey_hex);
  else
    g_debug("author-hints: no servers found for %s", pubkey_hex);

  /* Store in cache */
  g_mutex_lock(&cache->lock);

  CacheEntry *new_entry = g_new0(CacheEntry, 1);
  new_entry->server_urls = g_strdupv(server_urls);
  new_entry->expires_at = g_get_monotonic_time() +
                           (gint64)cache->ttl_seconds * G_USEC_PER_SEC;

  g_hash_table_replace(cache->entries, g_strdup(pubkey_hex), new_entry);

  g_mutex_unlock(&cache->lock);

  return server_urls;
}
