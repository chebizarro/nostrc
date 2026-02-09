/**
 * @file follow_list.c
 * @brief NIP-02 follow list fetching and parsing utilities
 *
 * Fetches kind 3 contact list events from relays, parses p tags to extract
 * followed pubkeys with optional relay hints and petnames, and caches in nostrdb.
 */

#include "follow_list.h"
#include "relays.h"
#include "utils.h"
#include "../storage_ndb.h"
#include <nostr-filter.h>
#include <nostr-json.h>
#include "nostr_json.h"
#include <string.h>

/* Free a follow entry */
void gnostr_follow_entry_free(GnostrFollowEntry *entry)
{
  if (!entry) return;
  g_free(entry->pubkey_hex);
  g_free(entry->relay_hint);
  g_free(entry->petname);
  g_free(entry);
}

/* Parse "p" tags from tags JSON array into GnostrFollowEntry entries.
 * Tag format: ["p", "<pubkey>", "<relay>", "<petname>"]
 * Only pubkey is required; relay and petname are optional. */
static GPtrArray *parse_p_tags_to_entries(const gchar *tags_json)
{
  if (!tags_json) return NULL;

  GPtrArray *entries = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_follow_entry_free);

  /* Iterate through tags array */
  const char *p = tags_json;
  while (*p && (p = strchr(p, '['))) {
    /* Skip outer array bracket if at start */
    if (p == tags_json && *p == '[') {
      p++;
      continue;
    }

    /* Find the end of this tag array */
    const char *tag_start = p;
    int depth = 0;
    gboolean in_string = FALSE;
    gboolean escape = FALSE;
    const char *tag_end = NULL;

    while (*p) {
      char c = *p;
      if (escape) {
        escape = FALSE;
        p++;
        continue;
      }
      if (c == '\\' && in_string) {
        escape = TRUE;
        p++;
        continue;
      }
      if (c == '"') {
        in_string = !in_string;
      } else if (!in_string) {
        if (c == '[') depth++;
        else if (c == ']') {
          depth--;
          if (depth == 0) {
            tag_end = p;
            break;
          }
        }
      }
      p++;
    }

    if (!tag_end) break;

    /* Extract this single tag: ["p", "pubkey", "relay", "petname"] */
    gsize tag_len = (gsize)(tag_end - tag_start + 1);
    gchar *tag_str = g_strndup(tag_start, tag_len);

    /* Check if this is a "p" tag.
     * tag_str is the array itself e.g. ["p", "pubkey", "relay", "petname"]
     * We need to parse it as a root array, not an array at a key. */
    gint tag_len_check = 0;
    if ((tag_len_check = gnostr_json_get_array_length(tag_str, NULL, NULL)) <= 0) {
      g_free(tag_str);
      p = tag_end + 1;
      continue;
    }

    gchar *tag_type = NULL;
    if ((tag_type = gnostr_json_get_array_string(tag_str, NULL, 0, NULL)) != NULL) {
      if (g_strcmp0(tag_type, "p") == 0) {
        gchar *pubkey = NULL;
        gchar *relay = NULL;
        gchar *petname = NULL;

        pubkey = gnostr_json_get_array_string(tag_str, NULL, 1, NULL);
        relay = gnostr_json_get_array_string(tag_str, NULL, 2, NULL);
        petname = gnostr_json_get_array_string(tag_str, NULL, 3, NULL);

        /* Validate pubkey is 64 hex chars */
        if (pubkey && strlen(pubkey) == 64) {
          GnostrFollowEntry *entry = g_new0(GnostrFollowEntry, 1);
          entry->pubkey_hex = pubkey;
          entry->relay_hint = (relay && *relay) ? relay : NULL;
          entry->petname = (petname && *petname) ? petname : NULL;

          /* NULL out moved pointers */
          if (entry->relay_hint) relay = NULL;
          if (entry->petname) petname = NULL;

          g_ptr_array_add(entries, entry);
        } else {
          g_free(pubkey);
        }
        g_free(relay);
        g_free(petname);
      }
      free(tag_type);
    }

    g_free(tag_str);
    p = tag_end + 1;
  }

  return entries;
}

/* Get follow list from local nostrdb cache */
GPtrArray *gnostr_follow_list_get_cached(const gchar *pubkey_hex)
{
  /* nostrc-akyz: defensively normalize npub/nprofile to hex */
  g_autofree gchar *hex = NULL;
  if (pubkey_hex && strlen(pubkey_hex) != 64) {
    hex = gnostr_ensure_hex_pubkey(pubkey_hex);
    pubkey_hex = hex;
  }
  if (!pubkey_hex || strlen(pubkey_hex) != 64) return NULL;

  /* Query for kind 3 from this author, limit 1 (most recent) */
  g_autofree gchar *filter = g_strdup_printf(
    "[{\"kinds\":[3],\"authors\":[\"%s\"],\"limit\":1}]",
    pubkey_hex);

  void *txn = NULL;
  int rc = storage_ndb_begin_query(&txn);
  if (rc != 0 || !txn) {
    return NULL;
  }

  char **results = NULL;
  int count = 0;
  rc = storage_ndb_query(txn, filter, &results, &count);

  if (rc != 0 || count == 0 || !results) {
    storage_ndb_end_query(txn);
    if (results) storage_ndb_free_results(results, count);
    return NULL;
  }

  /* Parse p-tags from the contact list JSON */
  GPtrArray *entries = NULL;
  g_autofree char *tags_json = NULL;
  if ((tags_json = gnostr_json_get_raw(results[0], "tags", NULL)) != NULL) {
    entries = parse_p_tags_to_entries(tags_json);
  }

  storage_ndb_free_results(results, count);
  storage_ndb_end_query(txn);

  if (!entries || entries->len == 0) {
    if (entries) g_ptr_array_unref(entries);
    return NULL;
  }

  g_debug("[FOLLOW_LIST] Found %u cached entries for %.16s...",
          entries->len, pubkey_hex);
  return entries;
}

/* Convenience wrapper to get just pubkeys */
gchar **gnostr_follow_list_get_pubkeys_cached(const gchar *pubkey_hex)
{
  GPtrArray *entries = gnostr_follow_list_get_cached(pubkey_hex);
  if (!entries) return NULL;

  gchar **pubkeys = g_new0(gchar*, entries->len + 1);
  for (guint i = 0; i < entries->len; i++) {
    GnostrFollowEntry *e = g_ptr_array_index(entries, i);
    pubkeys[i] = g_strdup(e->pubkey_hex);
  }

  g_ptr_array_unref(entries);
  return pubkeys;
}

/* Context for async follow list fetch */
typedef struct {
  gchar *pubkey_hex;
  GCancellable *cancellable;
  GnostrFollowListCallback callback;
  gpointer user_data;
  GPtrArray *nip65_relays;
  gboolean tried_configured;  /* TRUE after configured-relay attempt */
} FollowListFetchCtx;

static void follow_list_fetch_ctx_free(FollowListFetchCtx *ctx)
{
  if (!ctx) return;
  g_free(ctx->pubkey_hex);
  g_clear_object(&ctx->cancellable);
  if (ctx->nip65_relays) g_ptr_array_unref(ctx->nip65_relays);
  g_free(ctx);
}

/* Forward declarations */
static void on_nip65_relays_fetched(GPtrArray *nip65_relays, gpointer user_data);

/* Helper: extract follow entries from a set of kind:3 events.
 * Returns entries parsed from the most recent event, or NULL. */
static GPtrArray *extract_follow_entries_from_events(GPtrArray *events)
{
  if (!events || events->len == 0) return NULL;

  const gchar *best_event = NULL;
  gint64 best_created_at = 0;

  for (guint i = 0; i < events->len; i++) {
    const gchar *ev = g_ptr_array_index(events, i);
    gint64 created_at = 0;

    g_autofree gchar *created_at_str = NULL;
    if ((created_at_str = gnostr_json_get_raw(ev, "created_at", NULL)) != NULL) {
      created_at = g_ascii_strtoll(created_at_str, NULL, 10);
    }

    if (created_at > best_created_at) {
      best_created_at = created_at;
      best_event = ev;
    }
  }

  GPtrArray *entries = NULL;
  if (best_event) {
    g_autofree char *tags_json = NULL;
    if ((tags_json = gnostr_json_get_raw(best_event, "tags", NULL)) != NULL) {
      entries = parse_p_tags_to_entries(tags_json);
    }

    /* Cache in nostrdb via ingest */
    storage_ndb_ingest_event_json(best_event, NULL);
  }

  return entries;
}

/* Final callback: kind:3 query from NIP-65 relays (fallback) */
static void on_nip65_follow_list_query_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
  FollowListFetchCtx *ctx = user_data;
  GNostrPool *pool = GNOSTR_POOL(source);
  GError *error = NULL;

  GPtrArray *events = gnostr_pool_query_finish(pool, res, &error);
  GPtrArray *entries = extract_follow_entries_from_events(events);

  if (events) g_ptr_array_unref(events);
  if (error) g_error_free(error);

  g_debug("[FOLLOW_LIST] NIP-65 fallback returned %u entries for %.8s",
          entries ? entries->len : 0, ctx->pubkey_hex);

  if (ctx->callback) {
    ctx->callback(entries, ctx->user_data);
  } else if (entries) {
    g_ptr_array_unref(entries);
  }

  follow_list_fetch_ctx_free(ctx);
}

/* Query a specific set of relays for kind:3, using the given callback */
static void query_relays_for_follow_list_cb(FollowListFetchCtx *ctx,
                                             GPtrArray *relay_urls,
                                             GAsyncReadyCallback cb)
{
  if (!relay_urls || relay_urls->len == 0) {
    /* No relays to query, return cached or empty */
    GPtrArray *cached = gnostr_follow_list_get_cached(ctx->pubkey_hex);
    if (ctx->callback) ctx->callback(cached, ctx->user_data);
    follow_list_fetch_ctx_free(ctx);
    return;
  }

  /* Build filter for kind 3 */
  NostrFilter *filter = nostr_filter_new();
  int kinds[1] = { 3 };
  nostr_filter_set_kinds(filter, kinds, 1);
  const char *authors[1] = { ctx->pubkey_hex };
  nostr_filter_set_authors(filter, authors, 1);

  /* Build URL array */
  const char **urls = g_new0(const char*, relay_urls->len);
  for (guint i = 0; i < relay_urls->len; i++) {
    urls[i] = g_ptr_array_index(relay_urls, i);
  }

  /* Use shared pool */
  GNostrPool *pool = gnostr_get_shared_query_pool();

  gnostr_pool_sync_relays(pool, (const gchar **)urls, relay_urls->len);
  {
    /* nostrc-9pj1: Use unique key per query to avoid freeing filters still in use
     * by a concurrent query thread (use-after-free on overlapping fetches). */
    static gint _qf_counter_fl = 0;
    int _qfid = g_atomic_int_add(&_qf_counter_fl, 1);
    char _qfk[32]; g_snprintf(_qfk, sizeof(_qfk), "qf-fl-%d", _qfid);
    NostrFilters *_qf = nostr_filters_new();
    nostr_filters_add(_qf, filter);
    g_object_set_data_full(G_OBJECT(pool), _qfk, _qf, (GDestroyNotify)nostr_filters_free);
    gnostr_pool_query_async(pool, _qf, ctx->cancellable, cb, ctx);
  }

  g_free(urls);
  nostr_filter_free(filter);
}

/* Callback when NIP-65 relay list is fetched (for fallback after configured relays miss) */
static void on_nip65_relays_fetched(GPtrArray *nip65_relays, gpointer user_data)
{
  FollowListFetchCtx *ctx = user_data;

  /* Get write relays from NIP-65 list */
  GPtrArray *write_relays = NULL;
  if (nip65_relays && nip65_relays->len > 0) {
    write_relays = gnostr_nip65_get_write_relays(nip65_relays);
  }

  ctx->nip65_relays = nip65_relays; /* Keep ref for cleanup */

  if (!write_relays || write_relays->len == 0) {
    /* No NIP-65 relays either — return empty */
    g_debug("[FOLLOW_LIST] NIP-65 returned 0 relays for %.8s, giving up",
            ctx->pubkey_hex);
    if (write_relays) g_ptr_array_unref(write_relays);
    if (ctx->callback) ctx->callback(NULL, ctx->user_data);
    follow_list_fetch_ctx_free(ctx);
    return;
  }

  g_debug("[FOLLOW_LIST] Falling back to %u NIP-65 relays for %.8s",
          write_relays->len, ctx->pubkey_hex);
  query_relays_for_follow_list_cb(ctx, write_relays, on_nip65_follow_list_query_done);

  g_ptr_array_unref(write_relays);
}

/* Callback when kind:3 query on configured relays completes (first attempt) */
static void on_configured_relay_query_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
  FollowListFetchCtx *ctx = user_data;
  GNostrPool *pool = GNOSTR_POOL(source);
  GError *error = NULL;

  GPtrArray *events = gnostr_pool_query_finish(pool, res, &error);
  GPtrArray *entries = extract_follow_entries_from_events(events);

  if (events) g_ptr_array_unref(events);
  if (error) g_error_free(error);

  if (entries && entries->len > 0) {
    /* Got follow list from configured relays — done */
    g_debug("[FOLLOW_LIST] Configured relays returned %u entries for %.8s",
            entries->len, ctx->pubkey_hex);
    if (ctx->callback) {
      ctx->callback(entries, ctx->user_data);
    } else {
      g_ptr_array_unref(entries);
    }
    follow_list_fetch_ctx_free(ctx);
    return;
  }

  if (entries) g_ptr_array_unref(entries);

  /* Configured relays returned nothing — fall back to NIP-65 relay discovery */
  g_debug("[FOLLOW_LIST] Configured relays returned 0 entries for %.8s, trying NIP-65",
          ctx->pubkey_hex);
  ctx->tried_configured = TRUE;
  gnostr_nip65_fetch_relays_async(ctx->pubkey_hex, ctx->cancellable,
                                   on_nip65_relays_fetched, ctx);
}

/* Public async fetch function */
void gnostr_follow_list_fetch_async(const gchar *pubkey_hex,
                                     GCancellable *cancellable,
                                     GnostrFollowListCallback callback,
                                     gpointer user_data)
{
  /* nostrc-akyz: defensively normalize npub/nprofile to hex */
  g_autofree gchar *hex = NULL;
  if (pubkey_hex && strlen(pubkey_hex) != 64) {
    hex = gnostr_ensure_hex_pubkey(pubkey_hex);
    pubkey_hex = hex;
  }
  if (!pubkey_hex || strlen(pubkey_hex) != 64) {
    if (callback) callback(NULL, user_data);
    return;
  }

  /* First check cache */
  GPtrArray *cached = gnostr_follow_list_get_cached(pubkey_hex);
  if (cached && cached->len > 0) {
    /* Return cached immediately for fast UI */
    if (callback) callback(cached, user_data);

    /* Background refresh: re-fetch from configured relays to keep cache fresh.
     * The callback is NULL so results are silently ingested into NDB
     * via storage_ndb_ingest_event_json() in extract_follow_entries_from_events. */
    FollowListFetchCtx *bg_ctx = g_new0(FollowListFetchCtx, 1);
    bg_ctx->pubkey_hex = g_strdup(pubkey_hex);
    bg_ctx->cancellable = NULL;
    bg_ctx->callback = NULL;  /* silent background refresh */
    bg_ctx->user_data = NULL;

    GPtrArray *bg_relays = g_ptr_array_new_with_free_func(g_free);
    gnostr_load_relays_into(bg_relays);
    if (bg_relays->len > 0) {
      query_relays_for_follow_list_cb(bg_ctx, bg_relays, on_configured_relay_query_done);
    } else {
      gnostr_nip65_fetch_relays_async(pubkey_hex, NULL,
                                       on_nip65_relays_fetched, bg_ctx);
    }
    g_ptr_array_unref(bg_relays);
    return;
  }
  if (cached) g_ptr_array_unref(cached);

  /* Create fetch context */
  FollowListFetchCtx *ctx = g_new0(FollowListFetchCtx, 1);
  ctx->pubkey_hex = g_strdup(pubkey_hex);
  ctx->cancellable = cancellable ? g_object_ref(cancellable) : NULL;
  ctx->callback = callback;
  ctx->user_data = user_data;

  /* Try configured relays first — they're fast and usually have the follow list.
   * Only fall back to NIP-65 relay discovery if configured relays return nothing. */
  GPtrArray *configured = g_ptr_array_new_with_free_func(g_free);
  gnostr_load_relays_into(configured);

  if (configured->len > 0) {
    g_debug("[FOLLOW_LIST] Trying %u configured relays first for %.8s",
            configured->len, pubkey_hex);
    query_relays_for_follow_list_cb(ctx, configured, on_configured_relay_query_done);
    g_ptr_array_unref(configured);
  } else {
    g_ptr_array_unref(configured);
    /* No configured relays — go straight to NIP-65 */
    gnostr_nip65_fetch_relays_async(pubkey_hex, cancellable,
                                     on_nip65_relays_fetched, ctx);
  }
}
