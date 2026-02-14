#define G_LOG_DOMAIN "gnostr-blossom-settings"

/**
 * gnostr Blossom Settings Implementation
 *
 * Manages user Blossom server preferences using GSettings
 * and synchronizes with kind 10063 events.
 */

#include "blossom_settings.h"
#include "../ipc/gnostr-signer-service.h"
#include "relays.h"
#include <string.h>
#include <time.h>
#include <json-glib/json-glib.h>
#include <nostr-kinds.h>
#include "nostr-event.h"
/* GObject relay wrapper for publishing (includes nostr-relay.h internally) */
#include <nostr-gobject-1.0/nostr_relay.h>
#include <nostr-gobject-1.0/nostr_pool.h>
/* GObject simple pool wrapper for fetching */
#include "nostr-filter.h"

/* GSettings schema IDs */
#define BLOSSOM_SCHEMA_ID "org.gnostr.Client"

/* Singleton instance */
static GSettings *blossom_gsettings = NULL;

/* Flag to track if we already tried and failed to init GSettings */
static gboolean gsettings_init_attempted = FALSE;

/* Cached server list from GSettings */
static GPtrArray *cached_servers = NULL;

/* Ensure GSettings is initialized. Returns TRUE if GSettings is available. */
static gboolean ensure_gsettings(void) {
  if (blossom_gsettings) {
    return TRUE;
  }

  if (gsettings_init_attempted) {
    return FALSE;  /* Already tried and failed */
  }

  gsettings_init_attempted = TRUE;

  /* Check if schema is installed before trying to create GSettings */
  GSettingsSchemaSource *source = g_settings_schema_source_get_default();
  if (!source) {
    g_debug("GSettings schema source not available - using defaults");
    return FALSE;
  }

  GSettingsSchema *schema = g_settings_schema_source_lookup(source, BLOSSOM_SCHEMA_ID, TRUE);
  if (!schema) {
    g_debug("GSettings schema '%s' not installed - using defaults", BLOSSOM_SCHEMA_ID);
    return FALSE;
  }
  g_settings_schema_unref(schema);

  blossom_gsettings = g_settings_new(BLOSSOM_SCHEMA_ID);
  return TRUE;
}

void gnostr_blossom_server_free(GnostrBlossomServer *server) {
  if (!server) return;
  g_free(server->url);
  g_free(server);
}

GObject *gnostr_blossom_settings_get_default(void) {
  if (!ensure_gsettings()) {
    return NULL;
  }
  return G_OBJECT(blossom_gsettings);
}

const char *gnostr_blossom_settings_get_default_server(void) {
  /* Check GSettings first if available */
  if (ensure_gsettings()) {
    g_autofree char *url = g_settings_get_string(blossom_gsettings, "blossom-server");
    if (url && *url) {
      /* Return from gsettings - we need to keep a static copy */
      static char *cached_default = NULL;
      g_free(cached_default);
      cached_default = g_strdup(url);
      return cached_default;
    }
  }

  /* Fall back to default */
  return GNOSTR_BLOSSOM_DEFAULT_SERVER;
}

void gnostr_blossom_settings_set_default_server(const char *url) {
  if (!ensure_gsettings()) {
    return;  /* GSettings not available, silently ignore */
  }
  g_settings_set_string(blossom_gsettings, "blossom-server", url ? url : "");
}

static void load_servers_from_gsettings(void) {
  if (cached_servers) {
    g_ptr_array_unref(cached_servers);
  }
  cached_servers = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_blossom_server_free);

  if (!ensure_gsettings()) {
    return;  /* GSettings not available, use empty list */
  }

  g_auto(GStrv) servers = g_settings_get_strv(blossom_gsettings, "blossom-servers");
  if (servers) {
    for (int i = 0; servers[i] != NULL; i++) {
      if (servers[i][0] == '\0') continue;

      GnostrBlossomServer *server = g_new0(GnostrBlossomServer, 1);
      server->url = g_strdup(servers[i]);
      server->enabled = TRUE;
      g_ptr_array_add(cached_servers, server);
    }
  }

  /* If no servers configured, add the default */
  if (cached_servers->len == 0) {
    GnostrBlossomServer *server = g_new0(GnostrBlossomServer, 1);
    server->url = g_strdup(GNOSTR_BLOSSOM_DEFAULT_SERVER);
    server->enabled = TRUE;
    g_ptr_array_add(cached_servers, server);
  }
}

static void save_servers_to_gsettings(void) {
  if (!ensure_gsettings()) {
    return;  /* GSettings not available, silently ignore */
  }

  if (!cached_servers) {
    load_servers_from_gsettings();
    return;
  }

  GPtrArray *urls = g_ptr_array_new();
  for (guint i = 0; i < cached_servers->len; i++) {
    GnostrBlossomServer *server = g_ptr_array_index(cached_servers, i);
    if (server->enabled) {
      g_ptr_array_add(urls, server->url);
    }
  }
  g_ptr_array_add(urls, NULL);

  g_settings_set_strv(blossom_gsettings, "blossom-servers", (const char * const *)urls->pdata);
  g_ptr_array_free(urls, TRUE);
}

GnostrBlossomServer **gnostr_blossom_settings_get_servers(gsize *out_count) {
  if (!cached_servers) {
    load_servers_from_gsettings();
  }

  if (out_count) {
    *out_count = cached_servers->len;
  }

  /* Create a copy of the array for the caller */
  GnostrBlossomServer **result = g_new0(GnostrBlossomServer *, cached_servers->len + 1);
  for (guint i = 0; i < cached_servers->len; i++) {
    GnostrBlossomServer *src = g_ptr_array_index(cached_servers, i);
    GnostrBlossomServer *dst = g_new0(GnostrBlossomServer, 1);
    dst->url = g_strdup(src->url);
    dst->enabled = src->enabled;
    result[i] = dst;
  }
  result[cached_servers->len] = NULL;

  return result;
}

void gnostr_blossom_servers_free(GnostrBlossomServer **servers, gsize count) {
  if (!servers) return;
  for (gsize i = 0; i < count; i++) {
    gnostr_blossom_server_free(servers[i]);
  }
  g_free(servers);
}

gboolean gnostr_blossom_settings_add_server(const char *url) {
  if (!url || !*url) return FALSE;

  if (!cached_servers) {
    load_servers_from_gsettings();
  }

  /* Check if already exists */
  for (guint i = 0; i < cached_servers->len; i++) {
    GnostrBlossomServer *server = g_ptr_array_index(cached_servers, i);
    if (g_ascii_strcasecmp(server->url, url) == 0) {
      return FALSE; /* Already exists */
    }
  }

  /* Add new server */
  GnostrBlossomServer *server = g_new0(GnostrBlossomServer, 1);
  server->url = g_strdup(url);
  server->enabled = TRUE;
  g_ptr_array_add(cached_servers, server);

  save_servers_to_gsettings();
  return TRUE;
}

gboolean gnostr_blossom_settings_remove_server(const char *url) {
  if (!url || !*url) return FALSE;

  if (!cached_servers) {
    load_servers_from_gsettings();
  }

  for (guint i = 0; i < cached_servers->len; i++) {
    GnostrBlossomServer *server = g_ptr_array_index(cached_servers, i);
    if (g_ascii_strcasecmp(server->url, url) == 0) {
      g_ptr_array_remove_index(cached_servers, i);
      save_servers_to_gsettings();
      return TRUE;
    }
  }

  return FALSE;
}

gboolean gnostr_blossom_settings_from_event(const char *event_json) {
  if (!event_json) return FALSE;

  JsonParser *parser = json_parser_new();
  GError *error = NULL;

  if (!json_parser_load_from_data(parser, event_json, -1, &error)) {
    g_warning("Failed to parse kind 10063 event: %s", error->message);
    g_error_free(error);
    g_object_unref(parser);
    return FALSE;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
    g_object_unref(parser);
    return FALSE;
  }

  JsonObject *obj = json_node_get_object(root);

  /* Verify kind */
  if (!json_object_has_member(obj, "kind")) {
    g_object_unref(parser);
    return FALSE;
  }

  gint64 kind = json_object_get_int_member(obj, "kind");
  if (kind != NOSTR_KIND_USER_SERVER_LIST) {
    g_warning("Event is not kind 10063, got kind %" G_GINT64_FORMAT, kind);
    g_object_unref(parser);
    return FALSE;
  }

  /* Parse tags for server entries */
  if (!json_object_has_member(obj, "tags")) {
    g_object_unref(parser);
    return FALSE;
  }

  JsonArray *tags = json_object_get_array_member(obj, "tags");
  if (!tags) {
    g_object_unref(parser);
    return FALSE;
  }

  /* Clear existing cache and rebuild from event */
  if (cached_servers) {
    g_ptr_array_unref(cached_servers);
  }
  cached_servers = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_blossom_server_free);

  guint tags_len = json_array_get_length(tags);
  for (guint i = 0; i < tags_len; i++) {
    JsonArray *tag = json_array_get_array_element(tags, i);
    if (!tag || json_array_get_length(tag) < 2) continue;

    const char *tag_name = json_array_get_string_element(tag, 0);
    if (!tag_name || g_ascii_strcasecmp(tag_name, "server") != 0) continue;

    const char *server_url = json_array_get_string_element(tag, 1);
    if (!server_url || !*server_url) continue;

    GnostrBlossomServer *server = g_new0(GnostrBlossomServer, 1);
    server->url = g_strdup(server_url);
    server->enabled = TRUE;
    g_ptr_array_add(cached_servers, server);
  }

  g_object_unref(parser);

  /* Update default server to first one if list changed */
  if (cached_servers->len > 0) {
    GnostrBlossomServer *first = g_ptr_array_index(cached_servers, 0);
    gnostr_blossom_settings_set_default_server(first->url);
  }

  save_servers_to_gsettings();
  return TRUE;
}

char *gnostr_blossom_settings_to_event(void) {
  if (!cached_servers) {
    load_servers_from_gsettings();
  }

  JsonBuilder *builder = json_builder_new();

  json_builder_begin_object(builder);

  /* Kind 10063 */
  json_builder_set_member_name(builder, "kind");
  json_builder_add_int_value(builder, NOSTR_KIND_USER_SERVER_LIST);

  /* Created at */
  json_builder_set_member_name(builder, "created_at");
  json_builder_add_int_value(builder, (gint64)g_get_real_time() / G_USEC_PER_SEC);

  /* Content is empty */
  json_builder_set_member_name(builder, "content");
  json_builder_add_string_value(builder, "");

  /* Tags array with server entries */
  json_builder_set_member_name(builder, "tags");
  json_builder_begin_array(builder);

  for (guint i = 0; i < cached_servers->len; i++) {
    GnostrBlossomServer *server = g_ptr_array_index(cached_servers, i);
    if (!server->enabled) continue;

    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "server");
    json_builder_add_string_value(builder, server->url);
    json_builder_end_array(builder);
  }

  json_builder_end_array(builder);

  json_builder_end_object(builder);

  JsonGenerator *gen = json_generator_new();
  json_generator_set_root(gen, json_builder_get_root(builder));
  char *json_str = json_generator_to_data(gen, NULL);

  g_object_unref(gen);
  g_object_unref(builder);

  return json_str;
}

/* Async load/publish implementation */

/* Context for async publish operation */
typedef struct {
  GnostrBlossomSettingsPublishCallback callback;
  gpointer user_data;
  gchar *event_json;
} BlossomPublishCtx;

static void blossom_publish_ctx_free(BlossomPublishCtx *ctx) {
  if (!ctx) return;
  g_free(ctx->event_json);
  g_free(ctx);
}

/* hq-0df86: Worker thread data for async relay publishing */
typedef struct {
  NostrEvent *event;
  GPtrArray  *relay_urls;
  guint       success_count;
  guint       fail_count;
} BlossomRelayPublishData;

static void blossom_relay_publish_data_free(BlossomRelayPublishData *d) {
  if (!d) return;
  if (d->event) nostr_event_free(d->event);
  if (d->relay_urls) g_ptr_array_free(d->relay_urls, TRUE);
  g_free(d);
}

/* hq-0df86: Worker thread — connect+publish loop runs off main thread */
static void
blossom_publish_thread(GTask *task, gpointer source_object,
                       gpointer task_data, GCancellable *cancellable)
{
  (void)source_object; (void)cancellable;
  BlossomRelayPublishData *d = (BlossomRelayPublishData *)task_data;

  for (guint i = 0; i < d->relay_urls->len; i++) {
    const gchar *url = (const gchar *)g_ptr_array_index(d->relay_urls, i);
    GNostrRelay *relay = gnostr_relay_new(url);
    if (!relay) { d->fail_count++; continue; }

    GError *conn_err = NULL;
    if (!gnostr_relay_connect(relay, &conn_err)) {
      g_debug("blossom: failed to connect to %s: %s", url,
              conn_err ? conn_err->message : "unknown");
      g_clear_error(&conn_err);
      g_object_unref(relay);
      d->fail_count++;
      continue;
    }

    GError *pub_err = NULL;
    if (gnostr_relay_publish(relay, d->event, &pub_err)) {
      g_debug("blossom: published kind 10063 to %s", url);
      d->success_count++;
    } else {
      g_debug("blossom: publish failed to %s: %s", url,
              pub_err ? pub_err->message : "unknown");
      g_clear_error(&pub_err);
      d->fail_count++;
    }
    g_object_unref(relay);
  }

  g_task_return_boolean(task, d->success_count > 0);
}

/* hq-0df86: Completion callback — runs on main thread */
static void
blossom_publish_task_done(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  (void)source_object;
  BlossomPublishCtx *ctx = (BlossomPublishCtx *)user_data;

  GTask *task = G_TASK(res);
  BlossomRelayPublishData *d = g_task_get_task_data(task);
  GError *error = NULL;
  g_task_propagate_boolean(task, &error);

  if (ctx->callback) {
    if (d->success_count > 0) {
      ctx->callback(TRUE, NULL, ctx->user_data);
    } else {
      GError *err = g_error_new_literal(
          g_quark_from_static_string("blossom-settings"), 1,
          "Failed to publish to any relay");
      ctx->callback(FALSE, err, ctx->user_data);
      g_error_free(err);
    }
  }

  g_debug("blossom: published to %u relays, failed %u",
          d->success_count, d->fail_count);
  g_clear_error(&error);
  blossom_publish_ctx_free(ctx);
}

static void on_blossom_sign_complete(GObject *source, GAsyncResult *res, gpointer user_data) {
  BlossomPublishCtx *ctx = (BlossomPublishCtx*)user_data;
  (void)source;
  if (!ctx) return;

  GError *error = NULL;
  gchar *signed_event_json = NULL;

  gboolean ok = gnostr_sign_event_finish(res, &signed_event_json, &error);

  if (!ok || !signed_event_json) {
    g_warning("blossom: signing failed: %s", error ? error->message : "unknown error");
    if (ctx->callback) {
      GError *err = g_error_new(g_quark_from_static_string("blossom-settings"), 1,
                                 "Signing failed: %s", error ? error->message : "unknown");
      ctx->callback(FALSE, err, ctx->user_data);
      g_error_free(err);
    }
    g_clear_error(&error);
    blossom_publish_ctx_free(ctx);
    return;
  }

  g_debug("blossom: signed event successfully");

  /* Parse the signed event JSON into a NostrEvent */
  NostrEvent *event = nostr_event_new();
  int parse_rc = nostr_event_deserialize_compact(event, signed_event_json, NULL);
  if (parse_rc != 1) {
    g_warning("blossom: failed to parse signed event");
    if (ctx->callback) {
      GError *err = g_error_new_literal(g_quark_from_static_string("blossom-settings"), 1,
                                         "Failed to parse signed event");
      ctx->callback(FALSE, err, ctx->user_data);
      g_error_free(err);
    }
    nostr_event_free(event);
    g_free(signed_event_json);
    blossom_publish_ctx_free(ctx);
    return;
  }

  /* Get relay URLs from config */
  GPtrArray *relay_urls = g_ptr_array_new_with_free_func(g_free);
  gnostr_load_relays_into(relay_urls);

  g_free(signed_event_json);

  /* hq-0df86: Move connect+publish loop to worker thread to avoid blocking UI */
  BlossomRelayPublishData *wd = g_new0(BlossomRelayPublishData, 1);
  wd->event = event;          /* transfer ownership */
  wd->relay_urls = relay_urls; /* transfer ownership */

  GTask *task = g_task_new(NULL, NULL, blossom_publish_task_done, ctx);
  g_task_set_task_data(task, wd, (GDestroyNotify)blossom_relay_publish_data_free);
  g_task_run_in_thread(task, blossom_publish_thread);
  g_object_unref(task);
}

/* Context for async load operation */
typedef struct {
  gchar *pubkey_hex;
  GCancellable *cancellable;
  GnostrBlossomSettingsLoadCallback callback;
  gpointer user_data;
} BlossomLoadCtx;

static void blossom_load_ctx_free(BlossomLoadCtx *ctx) {
  if (!ctx) return;
  g_free(ctx->pubkey_hex);
  if (ctx->cancellable) g_object_unref(ctx->cancellable);
  g_free(ctx);
}

#ifndef GNOSTR_RELAY_TEST_ONLY

static void on_blossom_fetch_complete(GObject *source, GAsyncResult *res, gpointer user_data) {
  BlossomLoadCtx *ctx = (BlossomLoadCtx*)user_data;
  if (!ctx) return;

  GError *err = NULL;
  GPtrArray *results = gnostr_pool_query_finish(GNOSTR_POOL(source), res, &err);

  if (err) {
    g_warning("blossom: fetch failed: %s", err->message);
    if (ctx->callback) {
      ctx->callback(FALSE, err, ctx->user_data);
    }
    g_error_free(err);
    blossom_load_ctx_free(ctx);
    return;
  }

  gboolean found = FALSE;
  gint64 newest_created_at = 0;
  const gchar *newest_event_json = NULL;

  /* Find the newest kind 10063 event */
  if (results && results->len > 0) {
    for (guint i = 0; i < results->len; i++) {
      const gchar *json = g_ptr_array_index(results, i);
      if (!json) continue;

      /* Parse to check kind and created_at */
      JsonParser *parser = json_parser_new();
      if (json_parser_load_from_data(parser, json, -1, NULL)) {
        JsonNode *root = json_parser_get_root(parser);
        if (root && JSON_NODE_HOLDS_OBJECT(root)) {
          JsonObject *obj = json_node_get_object(root);
          if (json_object_has_member(obj, "kind")) {
            gint64 kind = json_object_get_int_member(obj, "kind");
            if (kind == NOSTR_KIND_USER_SERVER_LIST) {
              gint64 created_at = 0;
              if (json_object_has_member(obj, "created_at")) {
                created_at = json_object_get_int_member(obj, "created_at");
              }
              if (created_at > newest_created_at) {
                newest_created_at = created_at;
                newest_event_json = json;
              }
            }
          }
        }
      }
      g_object_unref(parser);
    }
  }

  if (newest_event_json) {
    if (gnostr_blossom_settings_from_event(newest_event_json)) {
      g_debug("blossom: loaded server list from relay (created_at: %" G_GINT64_FORMAT ")",
              newest_created_at);
      found = TRUE;
    }
  }

  if (results) g_ptr_array_unref(results);

  if (!found) {
    g_debug("blossom: no server list found on network for user %.*s, using local config",
            8, ctx->pubkey_hex ? ctx->pubkey_hex : "");
    load_servers_from_gsettings();
  }

  if (ctx->callback) {
    ctx->callback(TRUE, NULL, ctx->user_data);
  }

  blossom_load_ctx_free(ctx);
}

void gnostr_blossom_settings_load_from_relays_async(const char *pubkey_hex,
                                                      GnostrBlossomSettingsLoadCallback callback,
                                                      gpointer user_data) {
  if (!pubkey_hex || !*pubkey_hex) {
    load_servers_from_gsettings();
    if (callback) callback(TRUE, NULL, user_data);
    return;
  }

  BlossomLoadCtx *ctx = g_new0(BlossomLoadCtx, 1);
  ctx->pubkey_hex = g_strdup(pubkey_hex);
  ctx->callback = callback;
  ctx->user_data = user_data;

  /* Build filter for kind 10063 */
  NostrFilter *filter = nostr_filter_new();
  int kinds[1] = { NOSTR_KIND_USER_SERVER_LIST };
  nostr_filter_set_kinds(filter, kinds, 1);
  const char *authors[1] = { pubkey_hex };
  nostr_filter_set_authors(filter, authors, 1);

  /* Get configured relays */
  GPtrArray *relay_arr = g_ptr_array_new_with_free_func(g_free);
  gnostr_load_relays_into(relay_arr);

  /* Build URL array for SimplePool */
  const char **urls = g_new0(const char*, relay_arr->len + 1);
  for (guint i = 0; i < relay_arr->len; i++) {
    urls[i] = g_ptr_array_index(relay_arr, i);
  }

  /* Create pool and query */
  GNostrPool *pool = gnostr_pool_new();

    gnostr_pool_sync_relays(pool, (const gchar **)urls, relay_arr->len);
  {
    NostrFilters *_qf = nostr_filters_new();
    nostr_filters_add(_qf, filter);
    /* nostrc-uaf3: task takes ownership of _qf — do NOT stash on pool */
    gnostr_pool_query_async(pool, _qf, NULL, on_blossom_fetch_complete, ctx);
  }

  g_ptr_array_unref(relay_arr);
  g_free(urls);
  g_object_unref(pool);
  nostr_filter_free(filter);
}

#else

void gnostr_blossom_settings_load_from_relays_async(const char *pubkey_hex,
                                                      GnostrBlossomSettingsLoadCallback callback,
                                                      gpointer user_data) {
  /* Stub when SimplePool is not available */
  (void)pubkey_hex;
  load_servers_from_gsettings();
  if (callback) callback(TRUE, NULL, user_data);
}

#endif /* GNOSTR_RELAY_TEST_ONLY */

void gnostr_blossom_settings_publish_async(GnostrBlossomSettingsPublishCallback callback,
                                             gpointer user_data) {
  /* Check if signer service is available */
  GnostrSignerService *signer = gnostr_signer_service_get_default();
  if (!gnostr_signer_service_is_available(signer)) {
    if (callback) {
      GError *err = g_error_new_literal(g_quark_from_static_string("blossom-settings"), 1,
                                         "Signer not available");
      callback(FALSE, err, user_data);
      g_error_free(err);
    }
    return;
  }

  /* Build unsigned event JSON from current settings */
  gchar *event_json = gnostr_blossom_settings_to_event();
  if (!event_json) {
    if (callback) {
      GError *err = g_error_new_literal(g_quark_from_static_string("blossom-settings"), 1,
                                         "Failed to build event JSON");
      callback(FALSE, err, user_data);
      g_error_free(err);
    }
    return;
  }

  g_debug("blossom: requesting signature for server list event (kind 10063)");

  /* Create publish context */
  BlossomPublishCtx *ctx = g_new0(BlossomPublishCtx, 1);
  ctx->callback = callback;
  ctx->user_data = user_data;
  ctx->event_json = event_json;

  /* Call unified signer service (uses NIP-46 or NIP-55L based on login method) */
  gnostr_sign_event_async(
    event_json,
    "",        /* current_user: ignored */
    "gnostr",  /* app_id: ignored */
    NULL,      /* cancellable */
    on_blossom_sign_complete,
    ctx
  );
}

gboolean gnostr_blossom_settings_reorder_server(gsize from_index, gsize to_index) {
  if (!cached_servers) {
    load_servers_from_gsettings();
  }

  if (from_index >= cached_servers->len || to_index >= cached_servers->len) {
    return FALSE;
  }

  if (from_index == to_index) {
    return TRUE; /* No-op */
  }

  /* Remove from old position and insert at new position */
  GnostrBlossomServer *server = g_ptr_array_index(cached_servers, from_index);
  g_ptr_array_remove_index(cached_servers, from_index);

  /* Adjust target index if we removed before it */
  if (from_index < to_index) {
    to_index--;
  }

  g_ptr_array_insert(cached_servers, to_index, server);

  /* Update default server to first enabled one */
  for (guint i = 0; i < cached_servers->len; i++) {
    GnostrBlossomServer *s = g_ptr_array_index(cached_servers, i);
    if (s->enabled) {
      gnostr_blossom_settings_set_default_server(s->url);
      break;
    }
  }

  save_servers_to_gsettings();
  return TRUE;
}

gboolean gnostr_blossom_settings_set_server_enabled(gsize index, gboolean enabled) {
  if (!cached_servers) {
    load_servers_from_gsettings();
  }

  if (index >= cached_servers->len) {
    return FALSE;
  }

  GnostrBlossomServer *server = g_ptr_array_index(cached_servers, index);
  server->enabled = enabled;

  /* Update default server if this was the primary and is now disabled */
  if (!enabled && index == 0) {
    for (guint i = 1; i < cached_servers->len; i++) {
      GnostrBlossomServer *s = g_ptr_array_index(cached_servers, i);
      if (s->enabled) {
        gnostr_blossom_settings_set_default_server(s->url);
        break;
      }
    }
  }

  save_servers_to_gsettings();
  return TRUE;
}

gsize gnostr_blossom_settings_get_server_count(void) {
  if (!cached_servers) {
    load_servers_from_gsettings();
  }
  return cached_servers->len;
}

const char *gnostr_blossom_settings_get_server_url(gsize index) {
  if (!cached_servers) {
    load_servers_from_gsettings();
  }

  if (index >= cached_servers->len) {
    return NULL;
  }

  GnostrBlossomServer *server = g_ptr_array_index(cached_servers, index);
  return server->url;
}

const char **gnostr_blossom_settings_get_enabled_urls(gsize *out_count) {
  if (!cached_servers) {
    load_servers_from_gsettings();
  }

  /* Count enabled servers */
  gsize count = 0;
  for (guint i = 0; i < cached_servers->len; i++) {
    GnostrBlossomServer *server = g_ptr_array_index(cached_servers, i);
    if (server->enabled) {
      count++;
    }
  }

  if (out_count) {
    *out_count = count;
  }

  if (count == 0) {
    return NULL;
  }

  /* Build array of URLs */
  const char **urls = g_new0(const char *, count + 1);
  gsize idx = 0;
  for (guint i = 0; i < cached_servers->len && idx < count; i++) {
    GnostrBlossomServer *server = g_ptr_array_index(cached_servers, i);
    if (server->enabled) {
      urls[idx++] = server->url;
    }
  }
  urls[count] = NULL;

  return urls;
}

void gnostr_blossom_settings_clear_servers(void) {
  if (!cached_servers) {
    cached_servers = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_blossom_server_free);
  } else {
    g_ptr_array_set_size(cached_servers, 0);
  }
  save_servers_to_gsettings();
}
