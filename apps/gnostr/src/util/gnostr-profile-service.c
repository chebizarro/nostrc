/**
 * GnostrProfileService - Centralized profile fetching service with automatic batching
 *
 * Implementation of the profile service singleton with debounced batching,
 * nostrdb cache integration, and callback management.
 */

#include "gnostr-profile-service.h"
#include "../storage_ndb.h"
#include "../ui/gnostr-profile-provider.h"
#include "nostr_pool.h"
#include "nostr_event.h"
#include "nostr-filter.h"
#include "relays.h"
#include <string.h>
#include <stdlib.h>

/* ============== Internal Structures ============== */

/* A pending callback for a pubkey */
typedef struct {
  GnostrProfileServiceCallback callback;
  gpointer user_data;
} PendingCallback;

/* Entry in the pending requests hash table */
typedef struct {
  char *pubkey_hex;        /* owned */
  GPtrArray *callbacks;    /* array of PendingCallback* */
  gboolean in_flight;      /* TRUE if currently being fetched */
} PendingRequest;

/* The service singleton structure */
typedef struct {
  /* Thread safety */
  GMutex mutex;

  /* State */
  gboolean initialized;
  gboolean shutdown;

  /* Request queue: pubkey_hex -> PendingRequest* */
  GHashTable *pending_requests;

  /* Debounce state */
  guint debounce_source_id;
  guint debounce_ms;

  /* Relay configuration */
  char **relay_urls;
  size_t relay_url_count;

  /* Network fetch */
  GNostrPool *pool;
  gboolean owns_pool;
  GCancellable *cancellable;

  /* Batch management */
  GPtrArray *fetch_batches;      /* array of GPtrArray* of pubkey strings */
  guint fetch_batch_pos;
  gboolean fetch_in_progress;

  /* Statistics */
  GnostrProfileServiceStats stats;
} GnostrProfileService;

/* Singleton instance */
static GnostrProfileService *s_service = NULL;
G_LOCK_DEFINE_STATIC(service_singleton);

/* ============== Internal Helpers ============== */

static void pending_callback_free(PendingCallback *cb) {
  if (!cb) return;
  g_free(cb);
}

static void pending_request_free(PendingRequest *req) {
  if (!req) return;
  g_free(req->pubkey_hex);
  if (req->callbacks) {
    g_ptr_array_free(req->callbacks, TRUE);
  }
  g_free(req);
}

static PendingRequest *pending_request_new(const char *pubkey_hex) {
  PendingRequest *req = g_new0(PendingRequest, 1);
  req->pubkey_hex = g_strdup(pubkey_hex);
  req->callbacks = g_ptr_array_new_with_free_func((GDestroyNotify)pending_callback_free);
  req->in_flight = FALSE;
  return req;
}

/* Convert hex string to 32-byte binary */
static gboolean hex_to_bytes32(const char *hex, unsigned char *out) {
  if (!hex || strlen(hex) != 64) return FALSE;
  for (int i = 0; i < 32; i++) {
    unsigned int b;
    if (sscanf(hex + i*2, "%2x", &b) != 1) return FALSE;
    out[i] = (unsigned char)b;
  }
  return TRUE;
}

/* Hex string to 32-byte binary */
static gboolean hex_to_pk32(const char *hex, unsigned char out[32]) {
  if (!hex || strlen(hex) != 64) return FALSE;
  for (int i = 0; i < 32; i++) {
    unsigned int byte;
    if (sscanf(hex + 2 * i, "%02x", &byte) != 1) return FALSE;
    out[i] = (unsigned char)byte;
  }
  return TRUE;
}

/* Check nostrdb cache for a profile */
static GnostrProfileMeta *check_ndb_cache(const char *pubkey_hex) {
  /* First try the in-memory LRU cache via profile provider */
  GnostrProfileMeta *meta = gnostr_profile_provider_get(pubkey_hex);
  if (meta) {
    return meta;
  }

  /* Fall back to nostrdb — profiles may be persisted there from prior
   * sessions or negentropy sync but not yet loaded into the LRU cache. */
  unsigned char pk32[32];
  if (!hex_to_pk32(pubkey_hex, pk32)) return NULL;

  void *txn = NULL;
  if (storage_ndb_begin_query(&txn) != 0 || !txn) return NULL;

  char *json = NULL;
  int json_len = 0;
  int rc = storage_ndb_get_profile_by_pubkey(txn, pk32, &json, &json_len);
  storage_ndb_end_query(txn);

  if (rc != 0 || !json || json_len <= 0) return NULL;

  /* Populate the in-memory provider cache so subsequent lookups are fast */
  gnostr_profile_provider_update(pubkey_hex, json);
  meta = gnostr_profile_provider_get(pubkey_hex);

  g_debug("[PROFILE_SERVICE] NDB cache hit for %.8s (json_len=%d)", pubkey_hex, json_len);
  return meta;
}

/* Fire callbacks for a pubkey with the given profile (may be NULL) */
static void fire_callbacks(GnostrProfileService *svc, const char *pubkey_hex, const GnostrProfileMeta *meta) {
  g_mutex_lock(&svc->mutex);

  PendingRequest *req = g_hash_table_lookup(svc->pending_requests, pubkey_hex);
  if (!req || !req->callbacks) {
    g_mutex_unlock(&svc->mutex);
    return;
  }

  /* Copy callbacks to fire outside the lock */
  GPtrArray *to_fire = g_ptr_array_new();
  for (guint i = 0; i < req->callbacks->len; i++) {
    PendingCallback *cb = g_ptr_array_index(req->callbacks, i);
    if (cb && cb->callback) {
      PendingCallback *copy = g_new0(PendingCallback, 1);
      copy->callback = cb->callback;
      copy->user_data = cb->user_data;
      g_ptr_array_add(to_fire, copy);
    }
  }

  /* Remove the request now that we're handling it */
  g_hash_table_remove(svc->pending_requests, pubkey_hex);

  g_mutex_unlock(&svc->mutex);

  /* Fire callbacks outside the lock */
  for (guint i = 0; i < to_fire->len; i++) {
    PendingCallback *cb = g_ptr_array_index(to_fire, i);
    if (cb->callback) {
      cb->callback(pubkey_hex, meta, cb->user_data);

      g_mutex_lock(&svc->mutex);
      svc->stats.callbacks_fired++;
      g_mutex_unlock(&svc->mutex);
    }
  }

  g_ptr_array_free(to_fire, TRUE);
}

/* Forward declarations */
static void dispatch_next_batch(GnostrProfileService *svc);
static gboolean debounce_timeout_cb(gpointer user_data);

/* ============== Batch Fetch Implementation ============== */

typedef struct {
  GnostrProfileService *svc;
  GPtrArray *batch;      /* owned; char* pubkeys */
  NostrFilters *filters; /* NOT owned — GTask owns via destroy notify */
} BatchFetchCtx;

static void on_profiles_fetched(GObject *source, GAsyncResult *res, gpointer user_data) {
  BatchFetchCtx *ctx = (BatchFetchCtx*)user_data;
  if (!ctx) return;

  GnostrProfileService *svc = ctx->svc;
  GPtrArray *batch = ctx->batch;

  GError *error = NULL;
  GPtrArray *jsons = gnostr_pool_query_finish(
      GNOSTR_POOL(source), res, &error);

  if (error) {
    g_warning("[PROFILE_SERVICE] Fetch error: %s", error->message);
    g_clear_error(&error);
  }

  if (jsons) {
    g_mutex_lock(&svc->mutex);
    svc->stats.profiles_fetched += jsons->len;
    g_mutex_unlock(&svc->mutex);

    /* nostrc-mzab: Collect JSONs for background NDB ingestion.
     * Provider cache updates + callbacks stay on main thread (fast). */
    GPtrArray *to_ingest = g_ptr_array_new_with_free_func(g_free);

    /* Process each returned profile */
    for (guint i = 0; i < jsons->len; i++) {
      const char *evt_json = g_ptr_array_index(jsons, i);
      if (!evt_json) continue;

      /* Queue for background NDB ingestion (not on main thread) */
      g_ptr_array_add(to_ingest, g_strdup(evt_json));

      /* Extract pubkey from event JSON using GNostrEvent and update provider cache */
      GNostrEvent *evt = gnostr_event_new_from_json(evt_json, NULL);
      if (evt) {
        const char *pubkey_hex = gnostr_event_get_pubkey(evt);
        if (pubkey_hex && strlen(pubkey_hex) == 64) {
          /* Update the profile provider cache */
          gnostr_profile_provider_update(pubkey_hex, evt_json);

          /* Get the updated profile and fire callbacks */
          GnostrProfileMeta *meta = gnostr_profile_provider_get(pubkey_hex);
          fire_callbacks(svc, pubkey_hex, meta);
          if (meta) gnostr_profile_meta_free(meta);
        }
        g_object_unref(evt);
      }
    }
    g_ptr_array_unref(jsons);

    /* Spawn background thread for NDB ingestion */
    storage_ndb_ingest_events_async(to_ingest); /* takes ownership */
  }

  /* For any pubkeys in the batch that didn't get a profile, fire callbacks with NULL */
  for (guint i = 0; i < batch->len; i++) {
    const char *pubkey = g_ptr_array_index(batch, i);
    if (pubkey) {
      /* Check if there are still pending callbacks for this pubkey */
      g_mutex_lock(&svc->mutex);
      PendingRequest *req = g_hash_table_lookup(svc->pending_requests, pubkey);
      g_mutex_unlock(&svc->mutex);

      if (req) {
        /* Profile not found - fire callbacks with NULL */
        fire_callbacks(svc, pubkey, NULL);
      }
    }
  }

  /* Cleanup — filters are owned by the GTask (via g_object_set_data_full
   * with nostr_filters_free destroy notify in gnostr_pool_query_async),
   * so do NOT free them here. */
  if (batch) g_ptr_array_free(batch, TRUE);
  g_free(ctx);

  /* Mark fetch no longer in progress and dispatch next batch */
  g_mutex_lock(&svc->mutex);
  svc->fetch_in_progress = FALSE;
  g_mutex_unlock(&svc->mutex);

  dispatch_next_batch(svc);
}

static void dispatch_next_batch(GnostrProfileService *svc) {
  g_mutex_lock(&svc->mutex);

  if (svc->shutdown) {
    g_mutex_unlock(&svc->mutex);
    return;
  }

  /* Check if already fetching */
  if (svc->fetch_in_progress) {
    g_mutex_unlock(&svc->mutex);
    return;
  }

  /* Check if we have batches to process */
  if (!svc->fetch_batches || svc->fetch_batch_pos >= svc->fetch_batches->len) {
    /* No more batches - cleanup */
    if (svc->fetch_batches) {
      g_ptr_array_free(svc->fetch_batches, TRUE);
      svc->fetch_batches = NULL;
    }
    svc->fetch_batch_pos = 0;

    /* Check if there are new pending requests that came in during fetch */
    guint pending = g_hash_table_size(svc->pending_requests);
    g_mutex_unlock(&svc->mutex);

    if (pending > 0 && !svc->debounce_source_id) {
      /* Schedule a new debounce to pick up new requests */
      svc->debounce_source_id = g_timeout_add(svc->debounce_ms, debounce_timeout_cb, svc);
    }
    return;
  }

  /* Auto-configure relays from user settings if not set */
  if (!svc->relay_urls || svc->relay_url_count == 0) {
    GPtrArray *configured = g_ptr_array_new_with_free_func(g_free);
    gnostr_load_relays_into(configured);
    if (configured->len > 0) {
      svc->relay_urls = g_new0(char*, configured->len);
      svc->relay_url_count = configured->len;
      for (guint i = 0; i < configured->len; i++) {
        svc->relay_urls[i] = g_strdup(g_ptr_array_index(configured, i));
      }
      g_debug("[PROFILE_SERVICE] Auto-configured %zu relays from settings",
              svc->relay_url_count);
    }
    g_ptr_array_unref(configured);
  }
  if (!svc->relay_urls || svc->relay_url_count == 0) {
    g_warning("[PROFILE_SERVICE] No relays configured, cannot fetch");
    g_mutex_unlock(&svc->mutex);
    return;
  }

  if (!svc->pool) {
    svc->pool = gnostr_pool_new();
    svc->owns_pool = TRUE;
  }

  if (!svc->cancellable) {
    svc->cancellable = g_cancellable_new();
  }

  /* Get the next batch */
  GPtrArray *batch = g_ptr_array_index(svc->fetch_batches, svc->fetch_batch_pos);
  g_ptr_array_index(svc->fetch_batches, svc->fetch_batch_pos) = NULL; /* transfer ownership */
  svc->fetch_batch_pos++;

  if (!batch || batch->len == 0) {
    if (batch) g_ptr_array_free(batch, TRUE);
    g_mutex_unlock(&svc->mutex);
    dispatch_next_batch(svc);
    return;
  }

  /* Mark in-flight */
  svc->fetch_in_progress = TRUE;
  svc->stats.network_fetches++;

  /* Build authors array */
  size_t n = batch->len;
  const char **authors = g_new0(const char*, n);
  for (guint i = 0; i < n; i++) {
    authors[i] = g_ptr_array_index(batch, i);
  }

  /* Copy relay URLs for async use */
  const char **urls = g_new0(const char*, svc->relay_url_count);
  for (size_t i = 0; i < svc->relay_url_count; i++) {
    urls[i] = svc->relay_urls[i];
  }
  size_t url_count = svc->relay_url_count;

  g_debug("[PROFILE_SERVICE] Dispatching batch of %zu profiles to %zu relays",
          n, url_count);

  g_mutex_unlock(&svc->mutex);

  /* Sync relays on the pool */
  gnostr_pool_sync_relays(svc->pool, (const gchar **)urls, url_count);

  /* Build kind-0 filter for the batch of authors */
  NostrFilter *f = nostr_filter_new();
  int kind0 = 0;
  nostr_filter_set_kinds(f, &kind0, 1);
  nostr_filter_set_authors(f, (const char *const *)authors, n);
  NostrFilters *filters = nostr_filters_new();
  nostr_filters_add(filters, f);
  nostr_filter_free(f);

  /* Create context for callback */
  BatchFetchCtx *ctx = g_new0(BatchFetchCtx, 1);
  ctx->svc = svc;
  ctx->batch = batch; /* transfer ownership */
  ctx->filters = filters; /* transfer ownership */

  /* Start async fetch */
  gnostr_pool_query_async(svc->pool, filters, svc->cancellable,
                          on_profiles_fetched, ctx);

  g_free((gpointer)authors);
  g_free((gpointer)urls);
}

/* ============== Debounce Timer Callback ============== */

static gboolean debounce_timeout_cb(gpointer user_data) {
  GnostrProfileService *svc = (GnostrProfileService*)user_data;
  if (!svc) return G_SOURCE_REMOVE;

  g_mutex_lock(&svc->mutex);
  svc->debounce_source_id = 0;

  if (svc->shutdown) {
    g_mutex_unlock(&svc->mutex);
    return G_SOURCE_REMOVE;
  }

  /* Collect all pending pubkeys that aren't already in-flight */
  GPtrArray *to_fetch = g_ptr_array_new_with_free_func(g_free);
  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init(&iter, svc->pending_requests);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    PendingRequest *req = (PendingRequest*)value;
    if (!req->in_flight) {
      g_ptr_array_add(to_fetch, g_strdup(req->pubkey_hex));
      req->in_flight = TRUE;
    }
  }

  if (to_fetch->len == 0) {
    g_ptr_array_free(to_fetch, TRUE);
    g_mutex_unlock(&svc->mutex);
    return G_SOURCE_REMOVE;
  }

  g_debug("[PROFILE_SERVICE] Debounce fired: %u profiles to fetch", to_fetch->len);

  /* First pass: check cache and fire immediate callbacks */
  GPtrArray *need_fetch = g_ptr_array_new_with_free_func(g_free);

  g_mutex_unlock(&svc->mutex);

  for (guint i = 0; i < to_fetch->len; i++) {
    const char *pubkey = g_ptr_array_index(to_fetch, i);

    /* Check nostrdb cache first */
    GnostrProfileMeta *meta = check_ndb_cache(pubkey);
    if (meta) {
      /* Cache hit - fire callbacks immediately */
      g_mutex_lock(&svc->mutex);
      svc->stats.cache_hits++;
      g_mutex_unlock(&svc->mutex);

      fire_callbacks(svc, pubkey, meta);
      gnostr_profile_meta_free(meta);
    } else {
      /* Need to fetch from network */
      g_ptr_array_add(need_fetch, g_strdup(pubkey));
    }
  }

  g_ptr_array_free(to_fetch, TRUE);

  if (need_fetch->len == 0) {
    g_ptr_array_free(need_fetch, TRUE);
    return G_SOURCE_REMOVE;
  }

  /* Partition into batches of 100 */
  g_mutex_lock(&svc->mutex);

  const guint batch_size = 100;
  guint total = need_fetch->len;

  if (svc->fetch_batches) {
    /* Shouldn't happen, but clean up stale state */
    g_ptr_array_free(svc->fetch_batches, TRUE);
  }
  svc->fetch_batches = g_ptr_array_new();
  svc->fetch_batch_pos = 0;

  for (guint off = 0; off < total; off += batch_size) {
    guint n = (off + batch_size <= total) ? batch_size : (total - off);
    GPtrArray *batch = g_ptr_array_new_with_free_func(g_free);
    for (guint j = 0; j < n; j++) {
      char *pk = g_ptr_array_index(need_fetch, off + j);
      g_ptr_array_index(need_fetch, off + j) = NULL; /* transfer */
      g_ptr_array_add(batch, pk);
    }
    g_ptr_array_add(svc->fetch_batches, batch);
  }

  g_ptr_array_free(need_fetch, TRUE);
  g_mutex_unlock(&svc->mutex);

  /* Start fetching */
  dispatch_next_batch(svc);

  return G_SOURCE_REMOVE;
}

/* ============== Public API ============== */

gpointer gnostr_profile_service_get_default(void) {
  G_LOCK(service_singleton);

  if (s_service && !s_service->shutdown) {
    G_UNLOCK(service_singleton);
    return s_service;
  }

  /* Create new service */
  GnostrProfileService *svc = g_new0(GnostrProfileService, 1);
  g_mutex_init(&svc->mutex);
  svc->initialized = TRUE;
  svc->shutdown = FALSE;
  svc->pending_requests = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                  g_free, (GDestroyNotify)pending_request_free);
  svc->debounce_ms = 150;
  svc->debounce_source_id = 0;
  svc->relay_urls = NULL;
  svc->relay_url_count = 0;
  svc->pool = NULL;
  svc->owns_pool = FALSE;
  svc->cancellable = NULL;
  svc->fetch_batches = NULL;
  svc->fetch_batch_pos = 0;
  svc->fetch_in_progress = FALSE;
  memset(&svc->stats, 0, sizeof(svc->stats));

  s_service = svc;
  g_message("[PROFILE_SERVICE] Initialized with debounce=%ums", svc->debounce_ms);

  G_UNLOCK(service_singleton);
  return svc;
}

void gnostr_profile_service_request(gpointer service,
                                     const char *pubkey_hex,
                                     GnostrProfileServiceCallback callback,
                                     gpointer user_data) {
  GnostrProfileService *svc = (GnostrProfileService*)service;
  if (!svc || !pubkey_hex || strlen(pubkey_hex) != 64) return;

  g_mutex_lock(&svc->mutex);

  if (svc->shutdown) {
    g_mutex_unlock(&svc->mutex);
    return;
  }

  svc->stats.requests++;

  /* Check if we already have a pending request for this pubkey */
  PendingRequest *req = g_hash_table_lookup(svc->pending_requests, pubkey_hex);
  if (!req) {
    req = pending_request_new(pubkey_hex);
    g_hash_table_insert(svc->pending_requests, g_strdup(pubkey_hex), req);
  }

  /* Add callback if provided */
  if (callback) {
    PendingCallback *cb = g_new0(PendingCallback, 1);
    cb->callback = callback;
    cb->user_data = user_data;
    g_ptr_array_add(req->callbacks, cb);
  }

  svc->stats.pending_requests = g_hash_table_size(svc->pending_requests);
  svc->stats.pending_callbacks++;

  /* LEGITIMATE TIMEOUT - Debounce profile fetching to batch requests.
   * nostrc-b0h: Audited - batching network requests is appropriate. */
  if (!svc->debounce_source_id && !req->in_flight) {
    svc->debounce_source_id = g_timeout_add(svc->debounce_ms, debounce_timeout_cb, svc);
  }

  g_mutex_unlock(&svc->mutex);
}

guint gnostr_profile_service_cancel_for_user_data(gpointer service, gpointer user_data) {
  GnostrProfileService *svc = (GnostrProfileService*)service;
  if (!svc) return 0;

  guint cancelled = 0;

  g_mutex_lock(&svc->mutex);

  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, svc->pending_requests);

  while (g_hash_table_iter_next(&iter, &key, &value)) {
    PendingRequest *req = (PendingRequest*)value;
    if (!req || !req->callbacks) continue;

    for (guint i = 0; i < req->callbacks->len; ) {
      PendingCallback *cb = g_ptr_array_index(req->callbacks, i);
      if (cb && cb->user_data == user_data) {
        g_ptr_array_remove_index(req->callbacks, i);
        cancelled++;
      } else {
        i++;
      }
    }
  }

  g_mutex_unlock(&svc->mutex);

  if (cancelled > 0) {
    g_debug("[PROFILE_SERVICE] Cancelled %u callbacks for user_data %p", cancelled, user_data);
  }

  return cancelled;
}

void gnostr_profile_service_set_relays(gpointer service,
                                        const char **urls,
                                        size_t url_count) {
  GnostrProfileService *svc = (GnostrProfileService*)service;
  if (!svc) return;

  g_mutex_lock(&svc->mutex);

  /* Free old URLs */
  if (svc->relay_urls) {
    for (size_t i = 0; i < svc->relay_url_count; i++) {
      g_free(svc->relay_urls[i]);
    }
    g_free(svc->relay_urls);
  }

  /* Copy new URLs */
  if (urls && url_count > 0) {
    svc->relay_urls = g_new0(char*, url_count);
    svc->relay_url_count = url_count;
    for (size_t i = 0; i < url_count; i++) {
      svc->relay_urls[i] = g_strdup(urls[i]);
    }
    g_debug("[PROFILE_SERVICE] Set %zu relays", url_count);
  } else {
    svc->relay_urls = NULL;
    svc->relay_url_count = 0;
  }

  g_mutex_unlock(&svc->mutex);
}

void gnostr_profile_service_set_debounce(gpointer service, guint debounce_ms) {
  GnostrProfileService *svc = (GnostrProfileService*)service;
  if (!svc) return;

  g_mutex_lock(&svc->mutex);
  svc->debounce_ms = debounce_ms > 0 ? debounce_ms : 150;
  g_debug("[PROFILE_SERVICE] Set debounce=%ums", svc->debounce_ms);
  g_mutex_unlock(&svc->mutex);
}

gpointer gnostr_profile_service_get_pool(gpointer service) {
  GnostrProfileService *svc = (GnostrProfileService*)service;
  if (!svc) return NULL;

  g_mutex_lock(&svc->mutex);
  gpointer pool = svc->pool;
  g_mutex_unlock(&svc->mutex);

  return pool;
}

void gnostr_profile_service_set_pool(gpointer service, gpointer pool) {
  GnostrProfileService *svc = (GnostrProfileService*)service;
  if (!svc) return;

  g_mutex_lock(&svc->mutex);

  /* Unref old pool if we own it */
  if (svc->pool && svc->owns_pool) {
    g_object_unref(svc->pool);
  }

  /* Take reference to new pool */
  if (pool) {
    svc->pool = g_object_ref(pool);
    svc->owns_pool = FALSE;
  } else {
    svc->pool = NULL;
    svc->owns_pool = FALSE;
  }

  g_mutex_unlock(&svc->mutex);
}

void gnostr_profile_service_get_stats(gpointer service, GnostrProfileServiceStats *stats) {
  GnostrProfileService *svc = (GnostrProfileService*)service;
  if (!svc || !stats) return;

  g_mutex_lock(&svc->mutex);
  *stats = svc->stats;
  stats->pending_requests = g_hash_table_size(svc->pending_requests);
  g_mutex_unlock(&svc->mutex);
}

void gnostr_profile_service_shutdown(void) {
  G_LOCK(service_singleton);

  if (!s_service) {
    G_UNLOCK(service_singleton);
    return;
  }

  GnostrProfileService *svc = s_service;
  s_service = NULL;

  G_UNLOCK(service_singleton);

  g_mutex_lock(&svc->mutex);
  svc->shutdown = TRUE;

  /* Cancel pending debounce */
  if (svc->debounce_source_id) {
    g_source_remove(svc->debounce_source_id);
    svc->debounce_source_id = 0;
  }

  /* Cancel ongoing fetches */
  if (svc->cancellable) {
    g_cancellable_cancel(svc->cancellable);
    g_object_unref(svc->cancellable);
    svc->cancellable = NULL;
  }

  /* Free pending requests */
  if (svc->pending_requests) {
    g_hash_table_destroy(svc->pending_requests);
    svc->pending_requests = NULL;
  }

  /* Free fetch batches */
  if (svc->fetch_batches) {
    for (guint i = 0; i < svc->fetch_batches->len; i++) {
      GPtrArray *b = g_ptr_array_index(svc->fetch_batches, i);
      if (b) g_ptr_array_free(b, TRUE);
    }
    g_ptr_array_free(svc->fetch_batches, TRUE);
    svc->fetch_batches = NULL;
  }

  /* Free relay URLs */
  if (svc->relay_urls) {
    for (size_t i = 0; i < svc->relay_url_count; i++) {
      g_free(svc->relay_urls[i]);
    }
    g_free(svc->relay_urls);
    svc->relay_urls = NULL;
    svc->relay_url_count = 0;
  }

  /* Unref pool if we own it */
  if (svc->pool && svc->owns_pool) {
    g_object_unref(svc->pool);
    svc->pool = NULL;
  }

  g_mutex_unlock(&svc->mutex);
  g_mutex_clear(&svc->mutex);

  g_free(svc);

  g_message("[PROFILE_SERVICE] Shutdown complete");
}
