/**
 * NIP-38: User Statuses Implementation
 *
 * Handles user status events (kind 30315) for sharing ephemeral status updates.
 */

#include "user_status.h"
#include "relays.h"
#include "utils.h"
#include "../storage_ndb.h"
#include "../ipc/gnostr-signer-service.h"
#include "nostr-filter.h"
#include "nostr-event.h"
#include "nostr_relay.h"
#include "nostr_pool.h"
#include "nostr_json.h"
#include "json.h"
#include <string.h>
#include <time.h>

/* Kind for user status events (NIP-38) */
#define KIND_USER_STATUS 30315

/* Cache expiration check interval (5 minutes) */
#define CACHE_CLEANUP_INTERVAL_SEC 300

/* Maximum statuses to fetch per user */
#define STATUS_FETCH_LIMIT 10

/* ============== Status Structure Management ============== */

void gnostr_user_status_free(GnostrUserStatus *status) {
  if (!status) return;
  g_free(status->pubkey_hex);
  g_free(status->content);
  g_free(status->link_url);
  g_free(status->event_id);
  g_free(status);
}

GnostrUserStatus *gnostr_user_status_copy(const GnostrUserStatus *status) {
  if (!status) return NULL;

  GnostrUserStatus *copy = g_new0(GnostrUserStatus, 1);
  copy->pubkey_hex = g_strdup(status->pubkey_hex);
  copy->type = status->type;
  copy->content = g_strdup(status->content);
  copy->link_url = g_strdup(status->link_url);
  copy->created_at = status->created_at;
  copy->expiration = status->expiration;
  copy->event_id = g_strdup(status->event_id);
  return copy;
}

gboolean gnostr_user_status_is_expired(const GnostrUserStatus *status) {
  if (!status) return TRUE;
  if (status->expiration == 0) return FALSE;  /* No expiration set */

  gint64 now = (gint64)time(NULL);
  return now >= status->expiration;
}

const gchar *gnostr_user_status_type_to_string(GnostrUserStatusType type) {
  switch (type) {
    case GNOSTR_STATUS_MUSIC:
      return "music";
    case GNOSTR_STATUS_GENERAL:
    default:
      return "general";
  }
}

GnostrUserStatusType gnostr_user_status_type_from_string(const gchar *str) {
  if (!str) return GNOSTR_STATUS_GENERAL;
  if (g_ascii_strcasecmp(str, "music") == 0) return GNOSTR_STATUS_MUSIC;
  return GNOSTR_STATUS_GENERAL;
}

/* ============== Parsing ============== */

/* Callback context for parsing status tags */
typedef struct {
  GnostrUserStatus *status;
} StatusParseCtx;

static gboolean
status_tag_callback(gsize index, const gchar *element_json, gpointer user_data)
{
  (void)index;
  StatusParseCtx *ctx = user_data;

  char *tag_name = NULL;
  char *tag_value = NULL;

  tag_name = gnostr_json_get_array_string(element_json, NULL, 0, NULL);
  if (!tag_name) {
    return TRUE;
  }

  tag_value = gnostr_json_get_array_string(element_json, NULL, 1, NULL);
  if (!tag_value) {
    free(tag_name);
    return TRUE;
  }

  if (strcmp(tag_name, "d") == 0) {
    ctx->status->type = gnostr_user_status_type_from_string(tag_value);
  } else if (strcmp(tag_name, "r") == 0) {
    g_free(ctx->status->link_url);
    ctx->status->link_url = g_strdup(tag_value);
  } else if (strcmp(tag_name, "expiration") == 0) {
    ctx->status->expiration = g_ascii_strtoll(tag_value, NULL, 10);
  }

  free(tag_name);
  free(tag_value);
  return TRUE;
}

GnostrUserStatus *gnostr_user_status_parse_event(const gchar *event_json) {
  if (!event_json || !*event_json) return NULL;

  if (!gnostr_json_is_valid(event_json)) {
    g_debug("[NIP-38] Failed to parse event JSON");
    return NULL;
  }

  /* Verify kind */
  int kind = gnostr_json_get_int(event_json, "kind", NULL);
  if (kind != KIND_USER_STATUS) {
    return NULL;
  }

  GnostrUserStatus *status = g_new0(GnostrUserStatus, 1);

  /* Get pubkey */
  char *pubkey = NULL;
  pubkey = gnostr_json_get_string(event_json, "pubkey", NULL);
  if (pubkey) {
    status->pubkey_hex = g_strdup(pubkey);
    free(pubkey);
  }

  /* Get content */
  char *content = NULL;
  content = gnostr_json_get_string(event_json, "content", NULL);
  if (content) {
    status->content = g_strdup(content);
    free(content);
  } else {
    status->content = g_strdup("");
  }

  /* Get created_at */
  int64_t created_at = 0;
  if ((created_at = gnostr_json_get_int64(event_json, "created_at", NULL), TRUE)) {
    status->created_at = created_at;
  }

  /* Get id */
  char *event_id = NULL;
  event_id = gnostr_json_get_string(event_json, "id", NULL);
  if (event_id) {
    status->event_id = g_strdup(event_id);
    free(event_id);
  }

  /* Parse tags */
  StatusParseCtx ctx = { .status = status };
  gnostr_json_array_foreach(event_json, "tags", status_tag_callback, &ctx);

  /* Validate: must have pubkey */
  if (!status->pubkey_hex || !*status->pubkey_hex) {
    gnostr_user_status_free(status);
    return NULL;
  }

  return status;
}

/* ============== Cache Implementation ============== */

#define USER_STATUS_CACHE_MAX 1000

/* Cache structure: pubkey -> (type -> status) */
static GHashTable *g_status_cache = NULL;
static GMutex g_cache_mutex;
static gboolean g_cache_initialized = FALSE;

/* Cache key combining pubkey and type */
static gchar *make_cache_key(const gchar *pubkey_hex, GnostrUserStatusType type) {
  return g_strdup_printf("%s:%s", pubkey_hex, gnostr_user_status_type_to_string(type));
}

void gnostr_user_status_cache_init(void) {
  if (g_cache_initialized) return;

  g_mutex_init(&g_cache_mutex);
  g_mutex_lock(&g_cache_mutex);

  g_status_cache = g_hash_table_new_full(
    g_str_hash, g_str_equal,
    g_free,  /* key free */
    (GDestroyNotify)gnostr_user_status_free  /* value free */
  );

  g_cache_initialized = TRUE;
  g_mutex_unlock(&g_cache_mutex);

  g_debug("[NIP-38] User status cache initialized");
}

void gnostr_user_status_cache_shutdown(void) {
  if (!g_cache_initialized) return;

  g_mutex_lock(&g_cache_mutex);
  g_clear_pointer(&g_status_cache, g_hash_table_destroy);
  g_cache_initialized = FALSE;
  g_mutex_unlock(&g_cache_mutex);

  g_mutex_clear(&g_cache_mutex);
  g_debug("[NIP-38] User status cache shutdown");
}

GnostrUserStatus *gnostr_user_status_cache_get(const gchar *pubkey_hex,
                                                GnostrUserStatusType type) {
  if (!g_cache_initialized || !pubkey_hex) return NULL;

  g_mutex_lock(&g_cache_mutex);

  gchar *key = make_cache_key(pubkey_hex, type);
  GnostrUserStatus *cached = g_hash_table_lookup(g_status_cache, key);
  g_free(key);

  GnostrUserStatus *result = NULL;
  if (cached && !gnostr_user_status_is_expired(cached)) {
    result = gnostr_user_status_copy(cached);
  }

  g_mutex_unlock(&g_cache_mutex);
  return result;
}

void gnostr_user_status_cache_set(const GnostrUserStatus *status) {
  if (!g_cache_initialized || !status || !status->pubkey_hex) return;

  /* Don't cache expired statuses */
  if (gnostr_user_status_is_expired(status)) return;

  g_mutex_lock(&g_cache_mutex);

  gchar *key = make_cache_key(status->pubkey_hex, status->type);

  /* Check if we should replace existing */
  GnostrUserStatus *existing = g_hash_table_lookup(g_status_cache, key);
  if (existing && existing->created_at >= status->created_at) {
    /* Existing is newer or same, don't replace */
    g_free(key);
    g_mutex_unlock(&g_cache_mutex);
    return;
  }

  /* Cap cache to prevent unbounded growth */
  if (g_hash_table_size(g_status_cache) >= USER_STATUS_CACHE_MAX)
    g_hash_table_remove_all(g_status_cache);

  /* Insert (replaces existing with same key) */
  g_hash_table_insert(g_status_cache, key, gnostr_user_status_copy(status));

  g_mutex_unlock(&g_cache_mutex);
}

void gnostr_user_status_cache_remove(const gchar *pubkey_hex,
                                      GnostrUserStatusType type) {
  if (!g_cache_initialized || !pubkey_hex) return;

  g_mutex_lock(&g_cache_mutex);

  gchar *key = make_cache_key(pubkey_hex, type);
  g_hash_table_remove(g_status_cache, key);
  g_free(key);

  g_mutex_unlock(&g_cache_mutex);
}

/* ============== Fetch Implementation ============== */

typedef struct {
  gchar *pubkey_hex;
  GCancellable *cancellable;
  GnostrUserStatusCallback callback;
  gpointer user_data;
  GNostrPool *pool;
} StatusFetchContext;

static void status_fetch_context_free(StatusFetchContext *ctx) {
  if (!ctx) return;
  g_free(ctx->pubkey_hex);
  g_clear_object(&ctx->cancellable);
  g_clear_object(&ctx->pool);
  g_free(ctx);
}

static void on_status_fetch_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  StatusFetchContext *ctx = user_data;
  GError *error = NULL;

  GPtrArray *results = gnostr_pool_query_finish(
    GNOSTR_POOL(source), res, &error);

  if (g_cancellable_is_cancelled(ctx->cancellable)) {
    g_clear_error(&error);
    if (results) g_ptr_array_unref(results);
    status_fetch_context_free(ctx);
    return;
  }

  GPtrArray *statuses = g_ptr_array_new_with_free_func(
    (GDestroyNotify)gnostr_user_status_free);

  if (error) {
    g_debug("[NIP-38] Fetch error for %s: %s", ctx->pubkey_hex, error->message);
    g_clear_error(&error);
  } else if (results && results->len > 0) {
    g_debug("[NIP-38] Received %u status events for %s", results->len, ctx->pubkey_hex);

    /* Track newest status per type */
    GnostrUserStatus *newest_general = NULL;
    GnostrUserStatus *newest_music = NULL;

    for (guint i = 0; i < results->len; i++) {
      const gchar *event_json = g_ptr_array_index(results, i);
      GnostrUserStatus *status = gnostr_user_status_parse_event(event_json);

      if (!status) continue;

      /* Skip expired statuses */
      if (gnostr_user_status_is_expired(status)) {
        gnostr_user_status_free(status);
        continue;
      }

      /* Keep only the newest per type */
      if (status->type == GNOSTR_STATUS_GENERAL) {
        if (!newest_general || status->created_at > newest_general->created_at) {
          gnostr_user_status_free(newest_general);
          newest_general = status;
        } else {
          gnostr_user_status_free(status);
        }
      } else if (status->type == GNOSTR_STATUS_MUSIC) {
        if (!newest_music || status->created_at > newest_music->created_at) {
          gnostr_user_status_free(newest_music);
          newest_music = status;
        } else {
          gnostr_user_status_free(status);
        }
      } else {
        gnostr_user_status_free(status);
      }
    }

    /* Add to results and cache */
    if (newest_general) {
      gnostr_user_status_cache_set(newest_general);
      g_ptr_array_add(statuses, newest_general);
    }
    if (newest_music) {
      gnostr_user_status_cache_set(newest_music);
      g_ptr_array_add(statuses, newest_music);
    }
  }

  if (results) g_ptr_array_unref(results);

  /* Call user callback */
  if (ctx->callback) {
    ctx->callback(statuses, ctx->user_data);
  } else {
    g_ptr_array_unref(statuses);
  }

  status_fetch_context_free(ctx);
}

void gnostr_user_status_fetch_async(const gchar *pubkey_hex,
                                     GCancellable *cancellable,
                                     GnostrUserStatusCallback callback,
                                     gpointer user_data) {
  if (!pubkey_hex || strlen(pubkey_hex) != 64) {
    if (callback) callback(NULL, user_data);
    return;
  }

  /* Initialize cache if needed */
  if (!g_cache_initialized) {
    gnostr_user_status_cache_init();
  }

  /* Check cache first */
  GPtrArray *cached = g_ptr_array_new_with_free_func(
    (GDestroyNotify)gnostr_user_status_free);

  GnostrUserStatus *general = gnostr_user_status_cache_get(pubkey_hex, GNOSTR_STATUS_GENERAL);
  GnostrUserStatus *music = gnostr_user_status_cache_get(pubkey_hex, GNOSTR_STATUS_MUSIC);

  if (general) g_ptr_array_add(cached, general);
  if (music) g_ptr_array_add(cached, music);

  /* If we have cached data, return it immediately (still fetch in background) */
  gboolean have_cache = cached->len > 0;

  /* Create context for async fetch */
  StatusFetchContext *ctx = g_new0(StatusFetchContext, 1);
  ctx->pubkey_hex = g_strdup(pubkey_hex);
  ctx->cancellable = cancellable ? g_object_ref(cancellable) : NULL;
  ctx->callback = callback;
  ctx->user_data = user_data;
  ctx->pool = g_object_ref(gnostr_get_shared_query_pool());

  /* Get relay URLs */
  GPtrArray *relay_urls = g_ptr_array_new_with_free_func(g_free);
  gnostr_get_read_relay_urls_into(relay_urls);

  if (relay_urls->len == 0) {
    g_debug("[NIP-38] No relays configured, returning cached data only");
    if (callback) {
      callback(cached, user_data);
    } else {
      g_ptr_array_unref(cached);
    }
    g_ptr_array_unref(relay_urls);
    status_fetch_context_free(ctx);
    return;
  }

  /* If we have cached data, return it first */
  if (have_cache && callback) {
    /* Return cached data immediately, but also fetch fresh data */
    /* Clone the callback data since we'll call again with fresh data */
    callback(cached, user_data);
    /* Clear callback to avoid double-call */
    ctx->callback = NULL;
  } else {
    g_ptr_array_unref(cached);
  }

  /* Build filter for kind 30315 with author */
  NostrFilter *filter = nostr_filter_new();

  int kinds[1] = { KIND_USER_STATUS };
  nostr_filter_set_kinds(filter, kinds, 1);

  const char *authors[1] = { pubkey_hex };
  nostr_filter_set_authors(filter, authors, 1);

  nostr_filter_set_limit(filter, STATUS_FETCH_LIMIT);

  /* Build URL array */
  const char **urls = g_new0(const char *, relay_urls->len);
  for (guint i = 0; i < relay_urls->len; i++) {
    urls[i] = g_ptr_array_index(relay_urls, i);
  }

  /* Start async query */
    gnostr_pool_sync_relays(ctx->pool, (const gchar **)urls, relay_urls->len);
  {
    NostrFilters *_qf = nostr_filters_new();
    nostr_filters_add(_qf, filter);
    gnostr_pool_query_async(ctx->pool, _qf, ctx->cancellable, on_status_fetch_done, ctx);
  }

  g_free(urls);
  nostr_filter_free(filter);
  g_ptr_array_unref(relay_urls);
}

/* ============== Publish Implementation ============== */

gchar *gnostr_user_status_build_event_json(GnostrUserStatusType type,
                                            const gchar *content,
                                            const gchar *link_url,
                                            gint64 expiration_seconds) {
  GNostrJsonBuilder *builder = gnostr_json_builder_new();
  gnostr_json_builder_begin_object(builder);

  gnostr_json_builder_set_key(builder, "kind");
  gnostr_json_builder_add_int(builder, KIND_USER_STATUS);

  gnostr_json_builder_set_key(builder, "created_at");
  gnostr_json_builder_add_int64(builder, (int64_t)time(NULL));

  gnostr_json_builder_set_key(builder, "content");
  gnostr_json_builder_add_string(builder, content ? content : "");

  /* Build tags */
  gnostr_json_builder_set_key(builder, "tags");
  gnostr_json_builder_begin_array(builder);

  /* "d" tag for status type (required for parameterized replaceable) */
  gnostr_json_builder_begin_array(builder);
  gnostr_json_builder_add_string(builder, "d");
  gnostr_json_builder_add_string(builder, gnostr_user_status_type_to_string(type));
  gnostr_json_builder_end_array(builder);

  /* "r" tag for link URL (optional) */
  if (link_url && *link_url) {
    gnostr_json_builder_begin_array(builder);
    gnostr_json_builder_add_string(builder, "r");
    gnostr_json_builder_add_string(builder, link_url);
    gnostr_json_builder_end_array(builder);
  }

  /* "expiration" tag (NIP-40, optional) */
  if (expiration_seconds > 0) {
    gint64 exp_time = (gint64)time(NULL) + expiration_seconds;
    gchar exp_str[32];
    g_snprintf(exp_str, sizeof(exp_str), "%" G_GINT64_FORMAT, exp_time);

    gnostr_json_builder_begin_array(builder);
    gnostr_json_builder_add_string(builder, "expiration");
    gnostr_json_builder_add_string(builder, exp_str);
    gnostr_json_builder_end_array(builder);
  }

  gnostr_json_builder_end_array(builder);  /* tags */
  gnostr_json_builder_end_object(builder);

  char *result = gnostr_json_builder_finish(builder);
  g_object_unref(builder);

  return result;
}

typedef struct {
  GnostrUserStatusType type;
  gchar *content;
  gchar *signed_event_json;  /* stashed for cache update in publish callback */
  GnostrUserStatusPublishCallback callback;
  gpointer user_data;
} StatusPublishContext;

static void status_publish_context_free(StatusPublishContext *ctx) {
  if (!ctx) return;
  g_free(ctx->content);
  g_free(ctx->signed_event_json);
  g_free(ctx);
}

static void status_publish_done(guint success_count, guint fail_count, gpointer user_data);

static void on_status_sign_complete(GObject *source, GAsyncResult *res, gpointer user_data) {
  StatusPublishContext *ctx = user_data;
  (void)source;

  GError *error = NULL;
  gchar *signed_event_json = NULL;
  gboolean ok = gnostr_sign_event_finish(res, &signed_event_json, &error);

  if (!ok || !signed_event_json) {
    g_warning("[NIP-38] Failed to sign status event: %s",
              error ? error->message : "unknown error");
    if (ctx->callback) {
      ctx->callback(FALSE, error ? error->message : "Failed to sign event", ctx->user_data);
    }
    g_clear_error(&error);
    status_publish_context_free(ctx);
    return;
  }

  g_debug("[NIP-38] Signed status event: %.100s...", signed_event_json);

  /* Parse signed event */
  NostrEvent *event = nostr_event_new();
  int parse_rc = nostr_event_deserialize_compact(event, signed_event_json, NULL);
  if (!parse_rc) {
    g_warning("[NIP-38] Failed to parse signed status event");
    if (ctx->callback) {
      ctx->callback(FALSE, "Failed to parse signed event", ctx->user_data);
    }
    nostr_event_free(event);
    g_free(signed_event_json);
    status_publish_context_free(ctx);
    return;
  }

  /* Get write relays and publish asynchronously (hq-gflmf) */
  GPtrArray *relay_urls = gnostr_get_write_relay_urls();
  if (relay_urls->len == 0) {
    g_warning("[NIP-38] No write relays configured");
    if (ctx->callback) {
      ctx->callback(FALSE, "No write relays configured", ctx->user_data);
    }
    nostr_event_free(event);
    g_free(signed_event_json);
    g_ptr_array_unref(relay_urls);
    status_publish_context_free(ctx);
    return;
  }

  /* Stash signed JSON for cache update in callback */
  ctx->signed_event_json = signed_event_json;

  gnostr_publish_to_relays_async(event, relay_urls,
      status_publish_done, ctx);
  /* event + relay_urls ownership transferred; ctx freed in callback */
}

static void
status_publish_done(guint success_count, guint fail_count, gpointer user_data)
{
  StatusPublishContext *ctx = (StatusPublishContext *)user_data;

  /* Update cache with our own status on successful publish */
  if (success_count > 0 && ctx->signed_event_json) {
    GnostrUserStatus *status = gnostr_user_status_parse_event(ctx->signed_event_json);
    if (status) {
      gnostr_user_status_cache_set(status);
      gnostr_user_status_free(status);
    }
  }

  g_debug("[NIP-38] Published status to %u/%u relays",
          success_count, success_count + fail_count);

  if (ctx->callback) {
    if (success_count > 0) {
      ctx->callback(TRUE, NULL, ctx->user_data);
    } else {
      ctx->callback(FALSE, "Failed to publish to any relay", ctx->user_data);
    }
  }

  status_publish_context_free(ctx);
}

void gnostr_user_status_publish_async(GnostrUserStatusType type,
                                       const gchar *content,
                                       const gchar *link_url,
                                       gint64 expiration_seconds,
                                       GnostrUserStatusPublishCallback callback,
                                       gpointer user_data) {
  /* Build unsigned event */
  gchar *event_json = gnostr_user_status_build_event_json(
    type, content, link_url, expiration_seconds);

  if (!event_json) {
    if (callback) {
      callback(FALSE, "Failed to build event JSON", user_data);
    }
    return;
  }

  g_debug("[NIP-38] Unsigned status event: %s", event_json);

  /* Create context for async operation */
  StatusPublishContext *ctx = g_new0(StatusPublishContext, 1);
  ctx->type = type;
  ctx->content = g_strdup(content ? content : "");
  ctx->callback = callback;
  ctx->user_data = user_data;

  /* Sign event */
  gnostr_sign_event_async(
    event_json,
    "",        /* current_user: ignored */
    "gnostr",  /* app_id: ignored */
    NULL,      /* cancellable */
    on_status_sign_complete,
    ctx
  );

  g_free(event_json);
}

void gnostr_user_status_clear_async(GnostrUserStatusType type,
                                     GnostrUserStatusPublishCallback callback,
                                     gpointer user_data) {
  /* Clear status by publishing empty content */
  gnostr_user_status_publish_async(type, "", NULL, 0, callback, user_data);
}
