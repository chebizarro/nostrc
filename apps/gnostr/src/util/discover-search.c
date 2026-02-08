/**
 * discover-search.c - NIP-50 Index Relay Search Implementation
 *
 * Implements network search for the Discover tab using:
 * - NIP-50 search queries to index relays
 * - Local nostrdb text search
 * - Query parsing for npub, NIP-05, text
 * - Result merging and deduplication
 */

#define G_LOG_DOMAIN "gnostr-discover-search"

#include "discover-search.h"
#include "relays.h"
#include "nip05.h"
#include "nostr-filter.h"
#include "nostr-event.h"
#include "nostr_pool.h"
#include "../storage_ndb.h"

#include <string.h>
#include <ctype.h>
#include <json-glib/json-glib.h>
#include "nostr_nip19.h"

/* Default search limit */
#define DEFAULT_SEARCH_LIMIT 50

/* Error quark */
G_DEFINE_QUARK(gnostr-search-error-quark, gnostr_search_error)

/* --- Result Management --- */

void gnostr_search_result_free(GnostrSearchResult *result)
{
  if (!result) return;
  g_free(result->pubkey_hex);
  g_free(result->display_name);
  g_free(result->name);
  g_free(result->nip05);
  g_free(result->picture);
  g_free(result->about);
  g_free(result);
}

static GnostrSearchResult *search_result_copy(const GnostrSearchResult *src)
{
  if (!src) return NULL;
  GnostrSearchResult *dst = g_new0(GnostrSearchResult, 1);
  dst->type = src->type;
  dst->pubkey_hex = g_strdup(src->pubkey_hex);
  dst->display_name = g_strdup(src->display_name);
  dst->name = g_strdup(src->name);
  dst->nip05 = g_strdup(src->nip05);
  dst->picture = g_strdup(src->picture);
  dst->about = g_strdup(src->about);
  dst->from_network = src->from_network;
  dst->created_at = src->created_at;
  return dst;
}

/* --- Query Management --- */

void gnostr_search_query_free(GnostrSearchQuery *query)
{
  if (!query) return;
  g_free(query->original);
  g_free(query->normalized);
  g_free(query);
}

/**
 * Check if string is a valid 64-char hex string
 */
static gboolean is_valid_hex64(const char *s)
{
  if (!s || strlen(s) != 64) return FALSE;
  for (int i = 0; i < 64; i++) {
    char c = s[i];
    if (!g_ascii_isxdigit(c)) return FALSE;
  }
  return TRUE;
}

/**
 * Convert npub to hex. Returns newly allocated string or NULL.
 */
static char *npub_to_hex(const char *npub)
{
  if (!npub) return NULL;

  /* Already hex? */
  if (is_valid_hex64(npub)) {
    return g_strdup(npub);
  }

  /* Must start with npub1 */
  if (!g_str_has_prefix(npub, "npub1")) {
    return NULL;
  }

  /* Decode bech32 */
  g_autoptr(GNostrNip19) n19 = gnostr_nip19_decode(npub, NULL);
  if (!n19)
    return NULL;

  return g_strdup(gnostr_nip19_get_pubkey(n19));
}

GnostrSearchQuery *gnostr_search_parse_query(const char *text)
{
  if (!text || !*text) return NULL;

  /* Trim whitespace */
  char *trimmed = g_strstrip(g_strdup(text));
  if (!*trimmed) {
    g_free(trimmed);
    return NULL;
  }

  GnostrSearchQuery *query = g_new0(GnostrSearchQuery, 1);
  query->original = trimmed;

  /* Check for npub */
  if (g_str_has_prefix(trimmed, "npub1") && strlen(trimmed) >= 60) {
    char *hex = npub_to_hex(trimmed);
    if (hex) {
      query->type = GNOSTR_SEARCH_QUERY_NPUB;
      query->normalized = hex;
      g_debug("search: parsed npub query -> %s", hex);
      return query;
    }
  }

  /* Check for hex pubkey */
  if (is_valid_hex64(trimmed)) {
    query->type = GNOSTR_SEARCH_QUERY_HEX;
    query->normalized = g_ascii_strdown(trimmed, -1);
    g_debug("search: parsed hex query -> %s", query->normalized);
    return query;
  }

  /* Check for NIP-05 identifier (contains @) */
  if (strchr(trimmed, '@')) {
    char *local = NULL;
    char *domain = NULL;
    if (gnostr_nip05_parse(trimmed, &local, &domain)) {
      query->type = GNOSTR_SEARCH_QUERY_NIP05;
      query->normalized = g_strdup(trimmed);
      g_free(local);
      g_free(domain);
      g_debug("search: parsed NIP-05 query -> %s", trimmed);
      return query;
    }
  }

  /* Default to text search */
  query->type = GNOSTR_SEARCH_QUERY_TEXT;
  query->normalized = g_strdup(trimmed);
  g_debug("search: parsed text query -> %s", trimmed);
  return query;
}

/* --- Index Relay Loading --- */

void gnostr_load_index_relays_into(GPtrArray *out)
{
  if (!out) return;

  GSettingsSchemaSource *src = g_settings_schema_source_get_default();
  if (!src) return;

  GSettingsSchema *schema = g_settings_schema_source_lookup(src, "org.gnostr.gnostr", TRUE);
  if (!schema) return;

  GSettings *settings = g_settings_new("org.gnostr.gnostr");
  g_settings_schema_unref(schema);

  gchar **arr = g_settings_get_strv(settings, "index-relays");
  if (arr) {
    for (gsize i = 0; arr[i] != NULL; i++) {
      if (arr[i] && *arr[i]) {
        g_ptr_array_add(out, g_strdup(arr[i]));
      }
    }
    g_strfreev(arr);
  }

  g_object_unref(settings);
  g_debug("search: loaded %u index relays", out->len);
}

/* --- Profile Parsing --- */

/**
 * Parse a kind 0 profile event JSON into a search result.
 */
static GnostrSearchResult *parse_profile_event(const char *json, gboolean from_network)
{
  if (!json) return NULL;

  JsonParser *parser = json_parser_new();
  if (!json_parser_load_from_data(parser, json, -1, NULL)) {
    g_object_unref(parser);
    return NULL;
  }

  JsonNode *root_node = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_OBJECT(root_node)) {
    g_object_unref(parser);
    return NULL;
  }

  JsonObject *event = json_node_get_object(root_node);

  /* Verify kind 0 */
  if (!json_object_has_member(event, "kind") ||
      json_object_get_int_member(event, "kind") != 0) {
    g_object_unref(parser);
    return NULL;
  }

  /* Get pubkey */
  const char *pubkey = NULL;
  if (json_object_has_member(event, "pubkey")) {
    pubkey = json_object_get_string_member(event, "pubkey");
  }
  if (!pubkey || strlen(pubkey) != 64) {
    g_object_unref(parser);
    return NULL;
  }

  /* Get created_at */
  gint64 created_at = 0;
  if (json_object_has_member(event, "created_at")) {
    created_at = json_object_get_int_member(event, "created_at");
  }

  /* Parse content as JSON for profile metadata */
  const char *content = NULL;
  if (json_object_has_member(event, "content")) {
    content = json_object_get_string_member(event, "content");
  }

  GnostrSearchResult *result = g_new0(GnostrSearchResult, 1);
  result->type = GNOSTR_SEARCH_RESULT_PROFILE;
  result->pubkey_hex = g_strdup(pubkey);
  result->from_network = from_network;
  result->created_at = created_at;

  /* Parse profile content */
  if (content && *content) {
    JsonParser *content_parser = json_parser_new();
    if (json_parser_load_from_data(content_parser, content, -1, NULL)) {
      JsonNode *content_root = json_parser_get_root(content_parser);
      if (JSON_NODE_HOLDS_OBJECT(content_root)) {
        JsonObject *profile = json_node_get_object(content_root);

        if (json_object_has_member(profile, "display_name")) {
          result->display_name = g_strdup(json_object_get_string_member(profile, "display_name"));
        }
        if (json_object_has_member(profile, "name")) {
          result->name = g_strdup(json_object_get_string_member(profile, "name"));
        }
        if (json_object_has_member(profile, "nip05")) {
          result->nip05 = g_strdup(json_object_get_string_member(profile, "nip05"));
        }
        if (json_object_has_member(profile, "picture")) {
          result->picture = g_strdup(json_object_get_string_member(profile, "picture"));
        }
        if (json_object_has_member(profile, "about")) {
          result->about = g_strdup(json_object_get_string_member(profile, "about"));
        }
      }
    }
    g_object_unref(content_parser);
  }

  g_object_unref(parser);
  return result;
}

/* --- Search Context --- */

typedef struct {
  GnostrSearchQuery *query;
  gboolean search_network;
  gboolean search_local;
  int limit;
  GCancellable *cancellable;
  GnostrSearchCallback callback;
  gpointer user_data;

  /* Results storage */
  GHashTable *results_by_pubkey; /* pubkey -> GnostrSearchResult* */
  GMutex results_mutex;

  /* Completion tracking */
  gboolean network_done;
  gboolean local_done;
  GError *network_error;
} SearchContext;

static void search_context_free(SearchContext *ctx)
{
  if (!ctx) return;
  gnostr_search_query_free(ctx->query);
  if (ctx->cancellable) g_object_unref(ctx->cancellable);
  if (ctx->results_by_pubkey) g_hash_table_unref(ctx->results_by_pubkey);
  if (ctx->network_error) g_error_free(ctx->network_error);
  g_mutex_clear(&ctx->results_mutex);
  g_free(ctx);
}

/**
 * Check if search is complete and invoke callback if so.
 */
static void search_check_complete(SearchContext *ctx)
{
  gboolean complete = FALSE;

  g_mutex_lock(&ctx->results_mutex);

  /* Check if all requested searches are done */
  if (ctx->search_network && !ctx->network_done) {
    g_mutex_unlock(&ctx->results_mutex);
    return;
  }
  if (ctx->search_local && !ctx->local_done) {
    g_mutex_unlock(&ctx->results_mutex);
    return;
  }

  complete = TRUE;
  g_mutex_unlock(&ctx->results_mutex);

  if (!complete) return;

  /* Build results array */
  GPtrArray *results = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_search_result_free);

  g_mutex_lock(&ctx->results_mutex);
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, ctx->results_by_pubkey);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    GnostrSearchResult *result = value;
    g_ptr_array_add(results, search_result_copy(result));
  }
  g_mutex_unlock(&ctx->results_mutex);

  g_debug("search: complete with %u results", results->len);

  /* Invoke callback */
  if (ctx->callback) {
    ctx->callback(results, ctx->network_error, ctx->user_data);
  } else {
    g_ptr_array_unref(results);
  }

  search_context_free(ctx);
}

/**
 * Add a result to the context, merging if pubkey already exists.
 * Prefers newer events (higher created_at).
 */
static void search_add_result(SearchContext *ctx, GnostrSearchResult *result)
{
  if (!ctx || !result || !result->pubkey_hex) {
    gnostr_search_result_free(result);
    return;
  }

  g_mutex_lock(&ctx->results_mutex);

  /* Check if we already have this pubkey */
  GnostrSearchResult *existing = g_hash_table_lookup(ctx->results_by_pubkey, result->pubkey_hex);
  if (existing) {
    /* Keep the newer one */
    if (result->created_at > existing->created_at) {
      g_hash_table_replace(ctx->results_by_pubkey, g_strdup(result->pubkey_hex), result);
    } else {
      gnostr_search_result_free(result);
    }
  } else {
    /* Check limit */
    if ((int)g_hash_table_size(ctx->results_by_pubkey) < ctx->limit) {
      g_hash_table_insert(ctx->results_by_pubkey, g_strdup(result->pubkey_hex), result);
    } else {
      gnostr_search_result_free(result);
    }
  }

  g_mutex_unlock(&ctx->results_mutex);
}

/* --- Network Search --- */

static void on_network_search_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
  SearchContext *ctx = (SearchContext *)user_data;
  if (!ctx) return;

  GError *err = NULL;
  GPtrArray *events = gnostr_pool_query_finish(GNOSTR_POOL(source), res, &err);

  if (err) {
    if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_debug("search: network query failed: %s", err->message);
      ctx->network_error = g_error_new(GNOSTR_SEARCH_ERROR,
                                        GNOSTR_SEARCH_ERROR_NETWORK_FAILED,
                                        "Network search failed: %s", err->message);
    }
    g_error_free(err);
  } else if (events) {
    g_debug("search: network returned %u events", events->len);
    for (guint i = 0; i < events->len; i++) {
      const char *json = g_ptr_array_index(events, i);

      /* Save event to nostrdb for local caching (nostrc-yf3v) */
      if (json && *json) {
        int ingest_rc = storage_ndb_ingest_event_json(json, NULL);
        if (ingest_rc != 0) {
          g_debug("search: failed to ingest profile event to nostrdb (rc=%d)", ingest_rc);
        }
      }

      GnostrSearchResult *result = parse_profile_event(json, TRUE);
      if (result) {
        search_add_result(ctx, result);
      }
    }
    g_ptr_array_unref(events);
  }

  g_mutex_lock(&ctx->results_mutex);
  ctx->network_done = TRUE;
  g_mutex_unlock(&ctx->results_mutex);

  search_check_complete(ctx);
}

static void do_network_search(SearchContext *ctx)
{
  /* Get index relays */
  GPtrArray *relay_urls = g_ptr_array_new_with_free_func(g_free);
  gnostr_load_index_relays_into(relay_urls);

  if (relay_urls->len == 0) {
    g_debug("search: no index relays configured, skipping network search");
    g_ptr_array_unref(relay_urls);

    g_mutex_lock(&ctx->results_mutex);
    ctx->network_done = TRUE;
    g_mutex_unlock(&ctx->results_mutex);

    search_check_complete(ctx);
    return;
  }

  /* Build URL array */
  const char **urls = g_new0(const char *, relay_urls->len);
  for (guint i = 0; i < relay_urls->len; i++) {
    urls[i] = g_ptr_array_index(relay_urls, i);
  }

  /* Build filter for kind 0 profiles with NIP-50 search */
  NostrFilter *filter = nostr_filter_new();
  int kinds[1] = { 0 }; /* kind 0 = profile metadata */
  nostr_filter_set_kinds(filter, kinds, 1);
  nostr_filter_set_limit(filter, ctx->limit);

  /* Set search based on query type */
  switch (ctx->query->type) {
    case GNOSTR_SEARCH_QUERY_NPUB:
    case GNOSTR_SEARCH_QUERY_HEX: {
      /* Direct author lookup */
      const char *authors[1] = { ctx->query->normalized };
      nostr_filter_set_authors(filter, authors, 1);
      break;
    }
    case GNOSTR_SEARCH_QUERY_NIP05:
    case GNOSTR_SEARCH_QUERY_TEXT:
      /* NIP-50 search query */
      nostr_filter_set_search(filter, ctx->query->normalized);
      break;
  }

  /* Use shared pool */
  static GNostrPool *search_pool = NULL;
  if (!search_pool) {
    search_pool = gnostr_pool_new();
  }

  g_debug("search: querying %u index relays for '%s'", relay_urls->len, ctx->query->normalized);

    gnostr_pool_sync_relays(search_pool, (const gchar **)urls, relay_urls->len);
  {
    NostrFilters *_qf = nostr_filters_new();
    nostr_filters_add(_qf, filter);
    g_object_set_data_full(G_OBJECT(search_pool), "qf", _qf, (GDestroyNotify)nostr_filters_free);
    gnostr_pool_query_async(search_pool, _qf, ctx->cancellable, on_network_search_done, ctx);
  }

  g_free(urls);
  g_ptr_array_unref(relay_urls);
  nostr_filter_free(filter);
}

/* --- Local Search --- */

static void do_local_search(SearchContext *ctx)
{
  /* Build search query for nostrdb */
  const char *search_text = ctx->query->normalized;
  if (!search_text || !*search_text) {
    g_mutex_lock(&ctx->results_mutex);
    ctx->local_done = TRUE;
    g_mutex_unlock(&ctx->results_mutex);
    search_check_complete(ctx);
    return;
  }

  /* For pubkey queries, try direct lookup */
  if (ctx->query->type == GNOSTR_SEARCH_QUERY_NPUB ||
      ctx->query->type == GNOSTR_SEARCH_QUERY_HEX) {
    /* Convert hex to binary */
    unsigned char pk32[32];
    const char *hex = ctx->query->normalized;
    for (int i = 0; i < 32; i++) {
      unsigned int byte;
      if (sscanf(hex + i*2, "%2x", &byte) != 1) break;
      pk32[i] = (unsigned char)byte;
    }

    void *txn = NULL;
    if (storage_ndb_begin_query(&txn) == 0 && txn) {
      char *json = NULL;
      int json_len = 0;
      if (storage_ndb_get_profile_by_pubkey(txn, pk32, &json, &json_len) == 0 && json) {
        GnostrSearchResult *result = parse_profile_event(json, FALSE);
        if (result) {
          search_add_result(ctx, result);
          g_debug("search: found profile in local cache for %s", ctx->query->normalized);
        }
      }
      storage_ndb_end_query(txn);
    }
  } else {
    /* Text search in nostrdb */
    void *txn = NULL;
    if (storage_ndb_begin_query(&txn) == 0 && txn) {
      char **results_arr = NULL;
      int results_count = 0;

      /* Build config JSON for text search (kind 0 profiles only) */
      char *config_json = g_strdup_printf("{\"kinds\":[0],\"limit\":%d}", ctx->limit);

      if (storage_ndb_text_search(txn, search_text, config_json, &results_arr, &results_count) == 0) {
        g_debug("search: local text search found %d results", results_count);
        for (int i = 0; i < results_count; i++) {
          if (results_arr[i]) {
            GnostrSearchResult *result = parse_profile_event(results_arr[i], FALSE);
            if (result) {
              search_add_result(ctx, result);
            }
          }
        }
        storage_ndb_free_results(results_arr, results_count);
      }

      g_free(config_json);
      storage_ndb_end_query(txn);
    }
  }

  g_mutex_lock(&ctx->results_mutex);
  ctx->local_done = TRUE;
  g_mutex_unlock(&ctx->results_mutex);

  search_check_complete(ctx);
}

/* --- Public API --- */

void gnostr_discover_search_async(GnostrSearchQuery *query,
                                   gboolean search_network,
                                   gboolean search_local,
                                   int limit,
                                   GCancellable *cancellable,
                                   GnostrSearchCallback callback,
                                   gpointer user_data)
{
  /* Validate input */
  if (!query || !query->normalized) {
    if (callback) {
      GError *err = g_error_new(GNOSTR_SEARCH_ERROR,
                                 GNOSTR_SEARCH_ERROR_INVALID_QUERY,
                                 "Invalid search query");
      callback(NULL, err, user_data);
      g_error_free(err);
    }
    return;
  }

  /* Create context */
  SearchContext *ctx = g_new0(SearchContext, 1);
  ctx->query = g_new0(GnostrSearchQuery, 1);
  ctx->query->type = query->type;
  ctx->query->original = g_strdup(query->original);
  ctx->query->normalized = g_strdup(query->normalized);
  ctx->search_network = search_network;
  ctx->search_local = search_local;
  ctx->limit = limit > 0 ? limit : DEFAULT_SEARCH_LIMIT;
  ctx->cancellable = cancellable ? g_object_ref(cancellable) : NULL;
  ctx->callback = callback;
  ctx->user_data = user_data;

  g_mutex_init(&ctx->results_mutex);
  ctx->results_by_pubkey = g_hash_table_new_full(
    g_str_hash, g_str_equal,
    g_free, (GDestroyNotify)gnostr_search_result_free
  );

  /* Mark as done if not searching */
  if (!search_network) ctx->network_done = TRUE;
  if (!search_local) ctx->local_done = TRUE;

  g_debug("search: starting async search for '%s' (network=%d, local=%d, limit=%d)",
          query->normalized, search_network, search_local, ctx->limit);

  /* Start searches */
  if (search_local) {
    do_local_search(ctx);
  }
  if (search_network) {
    do_network_search(ctx);
  }

  /* If neither search was requested, complete immediately */
  if (!search_network && !search_local) {
    search_check_complete(ctx);
  }
}
