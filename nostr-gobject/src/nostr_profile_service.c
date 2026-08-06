/**
 * GnostrProfileService - Centralized profile fetching service with automatic batching
 *
 * Implementation of the profile service singleton with debounced batching,
 * nostrdb cache integration, and callback management.
 */

#include "nostr_profile_service.h"
#include "storage_ndb.h"
#include "nostr_profile_provider.h"
#include "nostr_pool.h"
#include "nostr_event.h"
#include "nostr-filter.h"
#include <string.h>
#include <stdlib.h>

/* ============== Internal Structures ============== */

/* A pending callback for a pubkey */
typedef struct {
  GnostrProfileServiceCallback callback;
  gpointer user_data;
} PendingCallback;

/* Entry in the pending requests hash table */
#define MAX_HINT_RELAYS_PER_REQUEST 8
#define MAX_HINT_RELAYS_PER_BATCH 64

typedef struct {
  char *pubkey_hex;        /* owned */
  GPtrArray *callbacks;    /* array of PendingCallback* */
  GPtrArray *hint_relays;  /* owned unique relay URL strings */
  gboolean in_flight;      /* TRUE if currently being fetched */
  gboolean retry_with_new_hints; /* hints arrived after relay snapshot */
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

  /* Relay URL provider callback (set by app layer) */
  GnostrRelayUrlProvider relay_provider;

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

/* Global relay provider */
static GnostrRelayUrlProvider s_relay_provider = NULL;

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
  g_clear_pointer(&req->hint_relays, g_ptr_array_unref);
  g_free(req);
}

static PendingRequest *pending_request_new(const char *pubkey_hex) {
  PendingRequest *req = g_new0(PendingRequest, 1);
  req->pubkey_hex = g_strdup(pubkey_hex);
  req->callbacks = g_ptr_array_new_with_free_func((GDestroyNotify)pending_callback_free);
  req->hint_relays = g_ptr_array_new_with_free_func(g_free);
  req->in_flight = FALSE;
  return req;
}

/* Private batch-queue primitives.  They are intentionally not declared in the
 * installed API; the batching unit test declares them locally so it can cover
 * append-while-active deterministically without a relay or mock pool. */
#define PROFILE_FETCH_BATCH_SIZE 100

static void fetch_batch_free(GPtrArray *batch) {
  if (batch)
    g_ptr_array_unref(batch);
}

void gnostr_profile_service_private_fetch_batches_append(
    GPtrArray **fetch_batches,
    guint *fetch_batch_pos,
    GPtrArray *pubkeys) {
  g_return_if_fail(fetch_batches != NULL);
  g_return_if_fail(fetch_batch_pos != NULL);
  g_return_if_fail(pubkeys != NULL);

  /* Consumed slots no longer own their batches.  If there is no queued tail,
   * compact back to an empty queue before appending the next generation. */
  if (*fetch_batches && *fetch_batch_pos >= (*fetch_batches)->len) {
    g_clear_pointer(fetch_batches, g_ptr_array_unref);
    *fetch_batch_pos = 0;
  }

  if (!*fetch_batches) {
    *fetch_batches = g_ptr_array_new_with_free_func(
        (GDestroyNotify)fetch_batch_free);
  }

  for (guint off = 0; off < pubkeys->len; off += PROFILE_FETCH_BATCH_SIZE) {
    guint n = MIN(PROFILE_FETCH_BATCH_SIZE, pubkeys->len - off);
    GPtrArray *batch = g_ptr_array_new_with_free_func(g_free);

    for (guint i = 0; i < n; i++) {
      char *pubkey = g_ptr_array_index(pubkeys, off + i);
      g_ptr_array_index(pubkeys, off + i) = NULL;
      g_ptr_array_add(batch, pubkey);
    }

    g_ptr_array_add(*fetch_batches, batch);
  }

  g_ptr_array_unref(pubkeys);
}

GPtrArray *gnostr_profile_service_private_fetch_batches_pop(
    GPtrArray *fetch_batches,
    guint *fetch_batch_pos) {
  g_return_val_if_fail(fetch_batch_pos != NULL, NULL);

  while (fetch_batches && *fetch_batch_pos < fetch_batches->len) {
    guint pos = (*fetch_batch_pos)++;
    GPtrArray *batch = g_ptr_array_index(fetch_batches, pos);
    g_ptr_array_index(fetch_batches, pos) = NULL;
    if (batch)
      return batch;
  }

  return NULL;
}

void gnostr_profile_service_private_fetch_batches_clear(
    GPtrArray **fetch_batches,
    guint *fetch_batch_pos) {
  g_return_if_fail(fetch_batches != NULL);
  g_return_if_fail(fetch_batch_pos != NULL);

  g_clear_pointer(fetch_batches, g_ptr_array_unref);
  *fetch_batch_pos = 0;
}

static gboolean relay_array_contains(GPtrArray *relays, const char *url) {
  if (!relays || !url || !*url) return FALSE;
  for (guint i = 0; i < relays->len; i++) {
    if (g_strcmp0(g_ptr_array_index(relays, i), url) == 0)
      return TRUE;
  }
  return FALSE;
}

static gboolean pending_request_merge_hints(
                                        PendingRequest *req,
                                        const char *const *hint_relays,
                                        size_t hint_relay_count) {
  if (!req || !hint_relays) return FALSE;

  gboolean added = FALSE;
  size_t count = MIN(hint_relay_count, (size_t)MAX_HINT_RELAYS_PER_REQUEST);
  for (size_t i = 0; i < count; i++) {
    const char *url = hint_relays[i];
    if (url && *url && !relay_array_contains(req->hint_relays, url)) {
      g_ptr_array_add(req->hint_relays, g_strdup(url));
      added = TRUE;
    }
  }
  return added;
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

  /* Fall back to nostrdb -- profiles may be persisted there from prior
   * sessions or negentropy sync but not yet loaded into the LRU cache. */
  unsigned char pk32[32];
  if (!hex_to_pk32(pubkey_hex, pk32)) return NULL;

  void *txn = NULL;
  if (storage_ndb_begin_query(&txn, NULL) != 0 || !txn) return NULL;

  g_autofree char *json = NULL;
  int json_len = 0;
  int rc = storage_ndb_get_profile_by_pubkey(txn, pk32, &json, &json_len, NULL);
  storage_ndb_end_query(txn);

  if (rc != 0 || !json || json_len <= 0) return NULL;

  /* Populate the in-memory provider cache so subsequent lookups are fast */
  gnostr_profile_provider_update(pubkey_hex, json);
  meta = gnostr_profile_provider_get(pubkey_hex);

  g_debug("[PROFILE_SERVICE] NDB cache hit for %.8s (json_len=%d)", pubkey_hex, json_len);
  return meta;
}

/* Forward declarations */
static void dispatch_next_batch(GnostrProfileService *svc);
static gboolean debounce_timeout_cb(gpointer user_data);

/* Complete a request, or retain it for one retry when hints arrived after the
 * active batch took its relay snapshot. */
static void complete_request(GnostrProfileService *svc,
                             const char *pubkey_hex,
                             const GnostrProfileMeta *meta,
                             gboolean retry_for_new_hints) {
  g_mutex_lock(&svc->mutex);

  PendingRequest *req = g_hash_table_lookup(svc->pending_requests, pubkey_hex);
  if (!req || !req->callbacks) {
    g_mutex_unlock(&svc->mutex);
    return;
  }

  if (retry_for_new_hints && req->retry_with_new_hints) {
    req->retry_with_new_hints = FALSE;
    req->in_flight = FALSE;
    if (!svc->debounce_source_id) {
      svc->debounce_source_id =
          g_timeout_add(svc->debounce_ms, debounce_timeout_cb, svc);
    }
    g_mutex_unlock(&svc->mutex);
    return;
  }

  /* Steal the owned callback array so delivery can happen outside the lock.
   * Its existing element free function releases every PendingCallback after
   * delivery; copying the elements here used to leak one allocation each. */
  GPtrArray *to_fire = g_steal_pointer(&req->callbacks);
  guint callback_count = to_fire->len;

  g_warn_if_fail(svc->stats.pending_callbacks >= callback_count);
  if (svc->stats.pending_callbacks >= callback_count)
    svc->stats.pending_callbacks -= callback_count;
  else
    svc->stats.pending_callbacks = 0;

  /* Terminal completion removes the request, so its in-flight state cannot
   * strand it.  pending_request_free() sees the stolen callbacks as NULL. */
  g_hash_table_remove(svc->pending_requests, pubkey_hex);
  svc->stats.pending_requests = g_hash_table_size(svc->pending_requests);

  g_mutex_unlock(&svc->mutex);

  /* Fire callbacks outside the lock. */
  for (guint i = 0; i < to_fire->len; i++) {
    PendingCallback *cb = g_ptr_array_index(to_fire, i);
    if (cb->callback) {
      cb->callback(pubkey_hex, meta, cb->user_data);

      g_mutex_lock(&svc->mutex);
      svc->stats.callbacks_fired++;
      g_mutex_unlock(&svc->mutex);
    }
  }

  g_ptr_array_unref(to_fire);
}

/* Fire callbacks for a terminal cache or network result (which may be NULL). */
static void fire_callbacks(GnostrProfileService *svc,
                           const char *pubkey_hex,
                           const GnostrProfileMeta *meta) {
  complete_request(svc, pubkey_hex, meta, FALSE);
}

static void complete_missing_request(GnostrProfileService *svc,
                                     const char *pubkey_hex) {
  complete_request(svc, pubkey_hex, NULL, TRUE);
}

/* ============== Batch Fetch Implementation ============== */

typedef struct {
  GnostrProfileService *svc;
  GPtrArray *batch;      /* owned; char* pubkeys */
  NostrFilters *filters; /* NOT owned -- GTask owns via destroy notify */
} BatchFetchCtx;

static void on_profiles_fetched(GObject *source, GAsyncResult *res, gpointer user_data) {
  BatchFetchCtx *ctx = (BatchFetchCtx*)user_data;
  if (!ctx) return;

  GnostrProfileService *svc = ctx->svc;
  GPtrArray *batch = ctx->batch;

  g_autoptr(GError) error = NULL;
  GPtrArray *jsons = gnostr_pool_query_finish(
      GNOSTR_POOL(source), res, &error);

  if (error) {
    g_warning("[PROFILE_SERVICE] Fetch error: %s", error->message);
  }

  /* Record the requested authors separately from returned events so each batch
   * member completes exactly once, after all provider updates are applied. */
  GHashTable *batch_pubkeys = g_hash_table_new(g_str_hash, g_str_equal);
  GHashTable *resolved_pubkeys =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  for (guint i = 0; i < batch->len; i++) {
    const char *pubkey = g_ptr_array_index(batch, i);
    if (pubkey)
      g_hash_table_add(batch_pubkeys, (gpointer)pubkey);
  }

  if (jsons) {
    g_mutex_lock(&svc->mutex);
    svc->stats.profiles_fetched += jsons->len;
    g_mutex_unlock(&svc->mutex);

    /* Collect JSONs for background NDB ingestion.
     * Provider cache updates stay on the main thread (fast). */
    GPtrArray *to_ingest = g_ptr_array_new_with_free_func(g_free);

    for (guint i = 0; i < jsons->len; i++) {
      const char *evt_json = g_ptr_array_index(jsons, i);
      if (!evt_json) continue;

      g_ptr_array_add(to_ingest, g_strdup(evt_json));

      GNostrEvent *evt = gnostr_event_new_from_json(evt_json, NULL);
      if (evt) {
        const char *pubkey_hex = gnostr_event_get_pubkey(evt);
        if (pubkey_hex && strlen(pubkey_hex) == 64) {
          gnostr_profile_provider_update(pubkey_hex, evt_json);

          unsigned char pk32[32];
          if (hex_to_bytes32(pubkey_hex, pk32)) {
            uint64_t now = (uint64_t)(g_get_real_time() / G_USEC_PER_SEC);
            storage_ndb_write_last_profile_fetch(pk32, now);
          }

          if (g_hash_table_contains(batch_pubkeys, pubkey_hex))
            g_hash_table_add(resolved_pubkeys, g_strdup(pubkey_hex));
        }
        g_object_unref(evt);
      }
    }
    g_ptr_array_unref(jsons);

    storage_ndb_ingest_events_async(to_ingest); /* takes ownership */
  }

  for (guint i = 0; i < batch->len; i++) {
    const char *pubkey = g_ptr_array_index(batch, i);
    if (!pubkey) continue;

    if (g_hash_table_contains(resolved_pubkeys, pubkey)) {
      GnostrProfileMeta *meta = gnostr_profile_provider_get(pubkey);
      fire_callbacks(svc, pubkey, meta);
      if (meta) gnostr_profile_meta_free(meta);
    } else {
      complete_missing_request(svc, pubkey);
    }
  }

  g_hash_table_unref(resolved_pubkeys);
  g_hash_table_unref(batch_pubkeys);

  /* Cleanup -- filters are owned by the GTask (via g_object_set_data_full
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
    gnostr_profile_service_private_fetch_batches_clear(
        &svc->fetch_batches, &svc->fetch_batch_pos);

    /* Check if there are new pending requests that came in during fetch.
     * Keep source ID inspection and assignment under the service mutex. */
    guint pending = g_hash_table_size(svc->pending_requests);
    if (pending > 0 && !svc->debounce_source_id) {
      svc->debounce_source_id =
          g_timeout_add(svc->debounce_ms, debounce_timeout_cb, svc);
    }
    g_mutex_unlock(&svc->mutex);
    return;
  }

  /* Auto-configure relays via registered provider if not set */
  if ((!svc->relay_urls || svc->relay_url_count == 0) && svc->relay_provider) {
    GPtrArray *configured = g_ptr_array_new_with_free_func(g_free);
    svc->relay_provider(configured);
    if (configured->len > 0) {
      svc->relay_urls = g_new0(char*, configured->len);
      svc->relay_url_count = configured->len;
      for (guint i = 0; i < configured->len; i++) {
        svc->relay_urls[i] = g_strdup(g_ptr_array_index(configured, i));
      }
      g_debug("[PROFILE_SERVICE] Auto-configured %zu relays from provider",
              svc->relay_url_count);
    }
    g_ptr_array_unref(configured);
  }
  /* Get the next batch (transfer full). */
  GPtrArray *batch = gnostr_profile_service_private_fetch_batches_pop(
      svc->fetch_batches, &svc->fetch_batch_pos);

  if (!batch || batch->len == 0) {
    if (batch) g_ptr_array_free(batch, TRUE);
    g_mutex_unlock(&svc->mutex);
    dispatch_next_batch(svc);
    return;
  }

  /* Build the per-batch relay union. Configured relays remain the baseline;
   * hint relays are temporary and bounded by their pending request lifetime. */
  GPtrArray *urls = g_ptr_array_new_with_free_func(g_free);
  for (size_t i = 0; i < svc->relay_url_count; i++) {
    const char *url = svc->relay_urls[i];
    if (url && *url && !relay_array_contains(urls, url))
      g_ptr_array_add(urls, g_strdup(url));
  }

  guint hints_added = 0;
  for (guint i = 0;
       i < batch->len && hints_added < MAX_HINT_RELAYS_PER_BATCH;
       i++) {
    const char *pubkey = g_ptr_array_index(batch, i);
    PendingRequest *req = g_hash_table_lookup(svc->pending_requests, pubkey);
    if (!req || !req->hint_relays) continue;
    for (guint j = 0;
         j < req->hint_relays->len && hints_added < MAX_HINT_RELAYS_PER_BATCH;
         j++) {
      const char *url = g_ptr_array_index(req->hint_relays, j);
      if (!relay_array_contains(urls, url)) {
        g_ptr_array_add(urls, g_strdup(url));
        hints_added++;
      }
    }
  }

  /* Hints merged before this point are represented by the URL snapshot above.
   * Only hints merged after the mutex is released should trigger a retry. */
  for (guint i = 0; i < batch->len; i++) {
    const char *pubkey = g_ptr_array_index(batch, i);
    PendingRequest *req = g_hash_table_lookup(svc->pending_requests, pubkey);
    if (req)
      req->retry_with_new_hints = FALSE;
  }

  if (urls->len == 0) {
    g_message("[PROFILE_SERVICE] No configured or hint relays, skipping fetch");
    g_mutex_unlock(&svc->mutex);
    for (guint i = 0; i < batch->len; i++)
      complete_missing_request(svc, g_ptr_array_index(batch, i));
    g_ptr_array_free(batch, TRUE);
    g_ptr_array_unref(urls);
    dispatch_next_batch(svc);
    return;
  }

  if (!svc->pool) {
    svc->pool = gnostr_pool_new();
    svc->owns_pool = TRUE;
  }

  if (!svc->cancellable) {
    svc->cancellable = g_cancellable_new();
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

  g_debug("[PROFILE_SERVICE] Dispatching batch of %zu profiles to %u relays (%u hints)",
          n, urls->len, hints_added);

  g_mutex_unlock(&svc->mutex);

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

  /* Query the explicit union so per-request hints do not accumulate in the
   * service pool. gnostr_pool_query_urls_async snapshots the URL strings. */
  gnostr_pool_query_urls_async(svc->pool,
                               (const gchar **)urls->pdata, urls->len,
                               filters, svc->cancellable,
                               on_profiles_fetched, ctx);

  g_free((gpointer)authors);
  g_ptr_array_unref(urls);
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
      /* Profile not in cache - need to fetch from network.
       * Note: We previously checked staleness here to avoid redundant fetches,
       * but that caused profiles to never load when the staleness timestamp
       * existed but the actual profile data didn't (e.g., after cache eviction
       * or failed prior fetch). Always try network fetch when cache misses. */
      g_ptr_array_add(need_fetch, g_strdup(pubkey));
    }
  }

  g_ptr_array_free(to_fetch, TRUE);

  if (need_fetch->len == 0) {
    g_ptr_array_free(need_fetch, TRUE);
    return G_SOURCE_REMOVE;
  }

  /* Append this debounce generation behind any batches already queued by an
   * active fetch sequence.  Replacing the queue here strands the old tail's
   * PendingRequest objects with in_flight=TRUE forever. */
  g_mutex_lock(&svc->mutex);
  gnostr_profile_service_private_fetch_batches_append(
      &svc->fetch_batches, &svc->fetch_batch_pos, need_fetch);
  g_mutex_unlock(&svc->mutex);

  /* Start fetching */
  dispatch_next_batch(svc);

  return G_SOURCE_REMOVE;
}

/* ============== Public API ============== */

void gnostr_profile_service_set_relay_provider(GnostrRelayUrlProvider provider) {
  G_LOCK(service_singleton);
  s_relay_provider = provider;
  if (s_service) {
    g_mutex_lock(&s_service->mutex);
    s_service->relay_provider = provider;
    g_mutex_unlock(&s_service->mutex);
  }
  G_UNLOCK(service_singleton);
}

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
  svc->relay_provider = s_relay_provider;
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

void gnostr_profile_service_request_with_hints(
                                     gpointer service,
                                     const char *pubkey_hex,
                                     const char *const *hint_relays,
                                     size_t hint_relay_count,
                                     GnostrProfileServiceCallback callback,
                                     gpointer user_data) {
  GnostrProfileService *svc = (GnostrProfileService*)service;
  if (!svc || !pubkey_hex || strlen(pubkey_hex) != 64) return;

  /* Validate hex characters */
  for (size_t i = 0; i < 64; i++) {
    char c = pubkey_hex[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
      return; /* Invalid hex character */
    }
  }

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
  gboolean added_hints =
      pending_request_merge_hints(req, hint_relays, hint_relay_count);
  if (req->in_flight && added_hints)
    req->retry_with_new_hints = TRUE;

  /* Add callback if provided */
  if (callback) {
    PendingCallback *cb = g_new0(PendingCallback, 1);
    cb->callback = callback;
    cb->user_data = user_data;
    g_ptr_array_add(req->callbacks, cb);
  }

  svc->stats.pending_requests = g_hash_table_size(svc->pending_requests);
  if (callback)
    svc->stats.pending_callbacks++;

  /* LEGITIMATE TIMEOUT - Debounce profile fetching to batch requests. */
  if (!svc->debounce_source_id && !req->in_flight) {
    svc->debounce_source_id = g_timeout_add(svc->debounce_ms, debounce_timeout_cb, svc);
  }

  g_mutex_unlock(&svc->mutex);
}

void gnostr_profile_service_request(gpointer service,
                                     const char *pubkey_hex,
                                     GnostrProfileServiceCallback callback,
                                     gpointer user_data) {
  gnostr_profile_service_request_with_hints(service, pubkey_hex, NULL, 0,
                                            callback, user_data);
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

    gboolean removed_from_request = FALSE;
    for (guint i = 0; i < req->callbacks->len; ) {
      PendingCallback *cb = g_ptr_array_index(req->callbacks, i);
      if (cb && cb->user_data == user_data) {
        g_ptr_array_remove_index(req->callbacks, i);
        removed_from_request = TRUE;
        cancelled++;
        if (svc->stats.pending_callbacks > 0)
          svc->stats.pending_callbacks--;
      } else {
        i++;
      }
    }

    /* An unclaimed request with no subscribers has no work left to perform.
     * Once claimed by debounce, retain it until its batch completes so queued
     * ownership and in-flight state remain consistent. */
    if (removed_from_request && req->callbacks->len == 0 && !req->in_flight)
      g_hash_table_iter_remove(&iter);
  }

  svc->stats.pending_requests = g_hash_table_size(svc->pending_requests);
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

  /* Free queued (but not currently active) fetch batches. */
  gnostr_profile_service_private_fetch_batches_clear(
      &svc->fetch_batches, &svc->fetch_batch_pos);

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

/* ============== GTask-based Async API (R3: GIR-friendly) ============== */

/* Bridge: old callback → GTask completion */
static void profile_request_gtask_bridge_cb(const char *pubkey_hex,
                                             const GnostrProfileMeta *meta,
                                             gpointer user_data) {
    (void)pubkey_hex;
    GTask *task = G_TASK(user_data);
    /* Return the meta pointer (owned by service cache, not by us) */
    g_task_return_pointer(task, (gpointer)meta, NULL);
    g_object_unref(task);
}

void gnostr_profile_service_request_gtask_async(gpointer service,
                                                 const char *pubkey_hex,
                                                 GCancellable *cancellable,
                                                 GAsyncReadyCallback callback,
                                                 gpointer user_data) {
    GTask *task = g_task_new(NULL, cancellable, callback, user_data);
    gnostr_profile_service_request(service, pubkey_hex,
                                    profile_request_gtask_bridge_cb, task);
}

const GnostrProfileMeta *gnostr_profile_service_request_gtask_finish(
    gpointer service,
    GAsyncResult *result,
    GError **error) {
    (void)service;
    return g_task_propagate_pointer(G_TASK(result), error);
}
