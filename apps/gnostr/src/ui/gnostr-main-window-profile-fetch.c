#define G_LOG_DOMAIN "gnostr-main-window-profile-fetch"

#include "gnostr-main-window-private.h"

 #include <gio/gio.h>

#include <nostr-gobject-1.0/nostr_event.h>
#include <nostr-gobject-1.0/nostr_json.h>
#include <nostr-gobject-1.0/nostr_pool.h>
#include <nostr-gobject-1.0/nostr_profile_provider.h>
#include <nostr-gobject-1.0/storage_ndb.h>

#include <stdlib.h>
#include <string.h>

typedef struct ProfileBatchCtx {
  GnostrMainWindow *self;
  GPtrArray *batch;
} ProfileBatchCtx;

 typedef struct ProfileNdbResult {
   GPtrArray *cached_items;  /* ProfileApplyCtx*; owns items */
   GPtrArray *stale_authors; /* char*; owns strings */
 } ProfileNdbResult;

 static void profile_ndb_result_free(ProfileNdbResult *r);
 static void profile_ndb_check_task(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable);
 static void on_profile_ndb_check_done(GObject *source_object, GAsyncResult *res, gpointer user_data);
 static void profile_fetch_start_network(GnostrMainWindow *self, GPtrArray *authors);

static gboolean profile_fetch_fire_idle(gpointer data);
static void on_profiles_batch_done(GObject *source, GAsyncResult *res, gpointer user_data);
static gboolean profile_dispatch_next(gpointer data);
static gboolean hex_to_bytes32_local(const char *hex, uint8_t out[32]);
static void profile_apply_item_free_local(gpointer p);
static gboolean profile_startup_throttle_active(GnostrMainWindow *self);
static guint profile_effective_max_concurrent(GnostrMainWindow *self);
static void profile_fetch_requested_add(GnostrMainWindow *self, const char *pubkey_hex);
static void profile_fetch_requested_remove(GnostrMainWindow *self, const char *pubkey_hex);
static gboolean profile_fetch_requested_contains(GnostrMainWindow *self, const char *pubkey_hex);
static void profile_fetch_requested_remove_many(GnostrMainWindow *self, GPtrArray *authors);

static gboolean
profile_startup_throttle_active(GnostrMainWindow *self)
{
  return GNOSTR_IS_MAIN_WINDOW(self) &&
         self->startup_profile_throttle_until_us > 0 &&
         g_get_monotonic_time() < (gint64)self->startup_profile_throttle_until_us;
}

static guint
profile_effective_max_concurrent(GnostrMainWindow *self)
{
  if (profile_startup_throttle_active(self))
    return MAX(1u, self->startup_profile_max_concurrent);
  return MAX(1u, self->profile_fetch_max_concurrent);
}

static void
profile_fetch_requested_add(GnostrMainWindow *self, const char *pubkey_hex)
{
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !pubkey_hex || strlen(pubkey_hex) != 64)
    return;
  if (!self->profile_fetch_requested)
    self->profile_fetch_requested = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  g_hash_table_add(self->profile_fetch_requested, g_strdup(pubkey_hex));
}

static void
profile_fetch_requested_remove(GnostrMainWindow *self, const char *pubkey_hex)
{
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !self->profile_fetch_requested || !pubkey_hex)
    return;
  g_hash_table_remove(self->profile_fetch_requested, pubkey_hex);
}

static gboolean
profile_fetch_requested_contains(GnostrMainWindow *self, const char *pubkey_hex)
{
  return GNOSTR_IS_MAIN_WINDOW(self) &&
         self->profile_fetch_requested &&
         pubkey_hex &&
         g_hash_table_contains(self->profile_fetch_requested, pubkey_hex);
}

static void
profile_fetch_requested_remove_many(GnostrMainWindow *self, GPtrArray *authors)
{
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !self->profile_fetch_requested || !authors)
    return;

  for (guint i = 0; i < authors->len; i++) {
    const char *pk = (const char *)g_ptr_array_index(authors, i);
    if (pk)
      g_hash_table_remove(self->profile_fetch_requested, pk);
  }
}

void
gnostr_main_window_enqueue_profile_author_internal(GnostrMainWindow *self, const char *pubkey_hex)
{
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !pubkey_hex || strlen(pubkey_hex) != 64)
    return;

  GnostrProfileMeta *meta = gnostr_profile_provider_get(pubkey_hex);
  if (meta) {
    gnostr_profile_meta_free(meta);
    return;
  }

  if (profile_fetch_requested_contains(self, pubkey_hex))
    goto schedule_only;

  if (!self->profile_fetch_queue)
    self->profile_fetch_queue = g_ptr_array_new_with_free_func(g_free);

  g_ptr_array_add(self->profile_fetch_queue, g_strdup(pubkey_hex));
  profile_fetch_requested_add(self, pubkey_hex);

schedule_only:
  if (!self->profile_fetch_source_id) {
    guint delay = self->profile_fetch_debounce_ms ? self->profile_fetch_debounce_ms : 150;
    GnostrMainWindow *ref = g_object_ref(self);
    self->profile_fetch_source_id = g_timeout_add_full(G_PRIORITY_DEFAULT,
                                                       delay,
                                                       profile_fetch_fire_idle,
                                                       ref,
                                                       g_object_unref);
  }
}

static gboolean
profile_fetch_fire_idle(gpointer data)
{
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(data);
  if (!GNOSTR_IS_MAIN_WINDOW(self))
    return G_SOURCE_REMOVE;

  self->profile_fetch_source_id = 0;

  if (!self->pool) {
    g_debug("[PROFILE] Pool not initialized, skipping fetch");
    if (self->profile_fetch_queue) {
      profile_fetch_requested_remove_many(self, self->profile_fetch_queue);
      g_ptr_array_free(self->profile_fetch_queue, TRUE);
      self->profile_fetch_queue = g_ptr_array_new_with_free_func(g_free);
    }
    return G_SOURCE_REMOVE;
  }

  if (!self->profile_fetch_queue || self->profile_fetch_queue->len == 0)
    return G_SOURCE_REMOVE;

  GPtrArray *authors = self->profile_fetch_queue;
  self->profile_fetch_queue = g_ptr_array_new_with_free_func(g_free);

  /* Offload NostrDB cache/staleness checks to a worker thread to avoid
   * blocking the GTK main loop while scrolling or paginating. */
  GTask *task = g_task_new(self, NULL, on_profile_ndb_check_done, NULL);
  g_task_set_task_data(task, authors, (GDestroyNotify)g_ptr_array_unref);
  g_task_run_in_thread(task, profile_ndb_check_task);
  g_object_unref(task);
  return G_SOURCE_REMOVE;
}

static void
profile_ndb_result_free(ProfileNdbResult *r)
{
  if (!r)
    return;
  if (r->cached_items)
    g_ptr_array_free(r->cached_items, TRUE);
  if (r->stale_authors)
    g_ptr_array_free(r->stale_authors, TRUE);
  g_free(r);
}

static void
profile_ndb_check_task(GTask *task,
                       gpointer source_object,
                       gpointer task_data,
                       GCancellable *cancellable)
{
  (void)source_object;
  (void)cancellable;

  GPtrArray *authors = (GPtrArray *)task_data;

  ProfileNdbResult *out = g_new0(ProfileNdbResult, 1);
  out->cached_items = g_ptr_array_new_with_free_func(profile_apply_item_free_local);
  out->stale_authors = g_ptr_array_new_with_free_func(g_free);

  void *txn = NULL;
  if (storage_ndb_begin_query(&txn, NULL) == 0) {
    for (guint i = 0; i < authors->len; i++) {
      const char *pkhex = (const char *)g_ptr_array_index(authors, i);
      if (!pkhex || strlen(pkhex) != 64)
        continue;

      uint8_t pk32[32];
      if (!hex_to_bytes32_local(pkhex, pk32))
        continue;

      char *pjson = NULL;
      int plen = 0;
      if (storage_ndb_get_profile_by_pubkey(txn, pk32, &pjson, &plen, NULL) == 0 && pjson && plen > 0) {
        gchar *content_str = gnostr_json_get_string(pjson, "content", NULL);
        if (content_str) {
          ProfileApplyCtx *item = g_new0(ProfileApplyCtx, 1);
          item->pubkey_hex = g_strdup(pkhex);
          item->content_json = content_str;
          g_ptr_array_add(out->cached_items, item);
        }
        free(pjson);
      }
    }
    storage_ndb_end_query(txn);
  }

  for (guint i = 0; i < authors->len; i++) {
    const char *pkhex = (const char *)g_ptr_array_index(authors, i);
    if (!pkhex)
      continue;
    if (storage_ndb_is_profile_stale(pkhex, 0))
      g_ptr_array_add(out->stale_authors, g_strdup(pkhex));
  }

  g_task_return_pointer(task, out, (GDestroyNotify)profile_ndb_result_free);
}

static void
on_profile_ndb_check_done(GObject *source_object,
                          GAsyncResult *res,
                          gpointer user_data)
{
  (void)user_data;

  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(source_object);
  if (!GNOSTR_IS_MAIN_WINDOW(self))
    return;

  GError *error = NULL;
  ProfileNdbResult *r = g_task_propagate_pointer(G_TASK(res), &error);
  if (error) {
    g_warning("[PROFILE] NDB worker failed: %s", error->message);
    GPtrArray *authors = g_task_get_task_data(G_TASK(res));
    if (authors)
      profile_fetch_requested_remove_many(self, authors);
    g_clear_error(&error);
    return;
  }
  if (!r)
    return;

  /* Any author that is neither cached (apply scheduled) nor stale (network fetch)
   * will not result in a profile update. Clear its requested flag so it can be
   * retried later if needed. */
  {
    GPtrArray *authors = g_task_get_task_data(G_TASK(res));
    if (authors && self->profile_fetch_requested) {
      GHashTable *handled = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

      if (r->cached_items) {
        for (guint i = 0; i < r->cached_items->len; i++) {
          ProfileApplyCtx *it = (ProfileApplyCtx *)g_ptr_array_index(r->cached_items, i);
          if (it && it->pubkey_hex)
            g_hash_table_add(handled, g_strdup(it->pubkey_hex));
        }
      }
      if (r->stale_authors) {
        for (guint i = 0; i < r->stale_authors->len; i++) {
          const char *pk = (const char *)g_ptr_array_index(r->stale_authors, i);
          if (pk)
            g_hash_table_add(handled, g_strdup(pk));
        }
      }

      for (guint i = 0; i < authors->len; i++) {
        const char *pk = (const char *)g_ptr_array_index(authors, i);
        if (!pk)
          continue;
        if (!g_hash_table_contains(handled, pk))
          g_hash_table_remove(self->profile_fetch_requested, pk);
      }

      g_hash_table_unref(handled);
    }
  }

  if (r->cached_items && r->cached_items->len > 0) {
    gnostr_main_window_schedule_apply_profiles_internal(self, r->cached_items);
    r->cached_items = NULL;
  }

  if (!r->stale_authors || r->stale_authors->len == 0) {
    profile_ndb_result_free(r);
    return;
  }

  profile_fetch_start_network(self, r->stale_authors);
  r->stale_authors = NULL;

  profile_ndb_result_free(r);
}

static void
profile_fetch_start_network(GnostrMainWindow *self, GPtrArray *authors)
{
  const char **urls = NULL;
  size_t url_count = 0;
  NostrFilters *dummy = NULL;
  int kind1 = 1;
  gnostr_main_window_build_urls_and_filters_for_kinds_internal(self,
                                                              &kind1,
                                                              1,
                                                              &urls,
                                                              &url_count,
                                                              &dummy,
                                                              0);
  if (!urls || url_count == 0) {
    g_warning("[PROFILE] No relays configured for profile fetch");
    profile_fetch_requested_remove_many(self, authors);
    g_ptr_array_free(authors, TRUE);
    if (dummy)
      nostr_filters_free(dummy);
    if (urls)
      gnostr_main_window_free_urls_owned_internal(urls, url_count);
    return;
  }

  const gboolean startup_throttled = profile_startup_throttle_active(self);
  const guint batch_sz = startup_throttled && self->startup_profile_batch_size > 0
                           ? self->startup_profile_batch_size
                           : 100;

  if (startup_throttled && authors->len > batch_sz) {
    if (!self->profile_fetch_queue)
      self->profile_fetch_queue = g_ptr_array_new_with_free_func(g_free);

    for (guint i = batch_sz; i < authors->len; i++) {
      char *s = (char *)g_ptr_array_index(authors, i);
      if (s)
        g_ptr_array_add(self->profile_fetch_queue, s);
      g_ptr_array_index(authors, i) = NULL;
    }
    g_ptr_array_set_size(authors, batch_sz);

    g_debug("[PROFILE] Startup throttle active: fetching %u profiles now, deferring %u",
            authors->len, self->profile_fetch_queue->len);
  }

  const guint total = authors->len;

  if (self->profile_batches) {
    if (self->profile_fetch_active > 0) {
      g_debug("[PROFILE] Fetch in progress (active=%u), appending %u authors to batch sequence",
              self->profile_fetch_active, authors->len);
      for (guint off = 0; off < authors->len; off += batch_sz) {
        guint n = (off + batch_sz <= authors->len) ? batch_sz : (authors->len - off);
        GPtrArray *b = g_ptr_array_new_with_free_func(g_free);
        for (guint j = 0; j < n; j++) {
          char *s = (char *)g_ptr_array_index(authors, off + j);
          g_ptr_array_index(authors, off + j) = NULL;
          g_ptr_array_add(b, s);
        }
        g_ptr_array_add(self->profile_batches, b);
      }
      g_debug("[PROFILE] Batch sequence now has %u batches total", self->profile_batches->len);

      g_ptr_array_free(authors, TRUE);
      if (dummy)
        nostr_filters_free(dummy);
      if (urls)
        gnostr_main_window_free_urls_owned_internal(urls, url_count);
      return;
    }

    g_warning("[PROFILE] ⚠️ STALE BATCH DETECTED - profile_batches is non-NULL but no fetch running!");
    g_warning("[PROFILE] This indicates a previous fetch never completed. Clearing stale state.");
    for (guint i = 0; i < self->profile_batches->len; i++) {
      GPtrArray *b = g_ptr_array_index(self->profile_batches, i);
      if (b)
        g_ptr_array_free(b, TRUE);
    }
    g_ptr_array_free(self->profile_batches, TRUE);
    self->profile_batches = NULL;

    if (self->profile_batch_urls) {
      gnostr_main_window_free_urls_owned_internal(self->profile_batch_urls, self->profile_batch_url_count);
      self->profile_batch_urls = NULL;
      self->profile_batch_url_count = 0;
    }
    self->profile_batch_pos = 0;
  }

  self->profile_batches = g_ptr_array_new();
  self->profile_batch_pos = 0;
  self->profile_batch_urls = urls;
  self->profile_batch_url_count = url_count;

  for (guint off = 0; off < total; off += batch_sz) {
    guint n = (off + batch_sz <= total) ? batch_sz : (total - off);
    GPtrArray *b = g_ptr_array_new_with_free_func(g_free);
    for (guint j = 0; j < n; j++) {
      char *s = (char *)g_ptr_array_index(authors, off + j);
      g_ptr_array_index(authors, off + j) = NULL;
      g_ptr_array_add(b, s);
    }
    g_ptr_array_add(self->profile_batches, b);
  }

  g_ptr_array_free(authors, TRUE);
  if (dummy)
    nostr_filters_free(dummy);

  profile_dispatch_next(g_object_ref(self));
}

static void
on_profiles_batch_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
  (void)source;
  ProfileBatchCtx *ctx = (ProfileBatchCtx *)user_data;
  if (!ctx) {
    g_critical("profile_fetch: callback ctx is NULL!");
    return;
  }

  GError *error = NULL;
  GPtrArray *jsons = gnostr_pool_query_finish(GNOSTR_POOL(source), res, &error);
  if (error) {
    g_warning("profile_fetch: error - %s", error->message);
    g_clear_error(&error);
  }

  if (jsons) {
    guint dispatched = 0;
    GPtrArray *items = g_ptr_array_new_with_free_func(profile_apply_item_free_local);

    GHashTable *unique_pks = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (guint i = 0; i < jsons->len; i++) {
      const char *evt_json = (const char *)g_ptr_array_index(jsons, i);
      if (!evt_json)
        continue;
      g_autoptr(GNostrEvent) evt = gnostr_event_new_from_json(evt_json, NULL);
      if (evt) {
        const char *pk = gnostr_event_get_pubkey(evt);
        if (pk)
          g_hash_table_add(unique_pks, g_strdup(pk));
      }
    }
    guint unique_count = g_hash_table_size(unique_pks);
    g_debug("[PROFILE] Batch received %u events (%u unique authors)", jsons->len, unique_count);

    guint queued = 0;
    for (guint i = 0; i < jsons->len; i++) {
      const char *evt_json = (const char *)g_ptr_array_index(jsons, i);
      if (!evt_json)
        continue;

      if (!strstr(evt_json, "\"tags\"")) {
        const char *kind_pos = strstr(evt_json, "\"kind\"");
        if (kind_pos) {
          const char *comma_after_kind = strchr(kind_pos, ',');
          if (comma_after_kind) {
            GString *fixed = g_string_new("");
            g_string_append_len(fixed, evt_json, comma_after_kind - evt_json + 1);
            g_string_append(fixed, "\"tags\":[],");
            g_string_append(fixed, comma_after_kind + 1);
            gchar *fixed_str = g_string_free(fixed, FALSE);
            if (gnostr_main_window_ingest_queue_push_internal(ctx->self, fixed_str))
              queued++;
            else
              g_free(fixed_str);
            continue;
          }
        }
      }

      gchar *dup = g_strdup(evt_json);
      if (gnostr_main_window_ingest_queue_push_internal(ctx->self, dup))
        queued++;
      else
        g_free(dup);
    }
    if (queued > 0)
      g_debug("[PROFILE] Queued %u events for background ingestion", queued);

    for (guint i = 0; i < jsons->len; i++) {
      const char *evt_json = (const char *)g_ptr_array_index(jsons, i);
      if (!evt_json)
        continue;

      g_autoptr(GNostrEvent) evt = gnostr_event_new_from_json(evt_json, NULL);
      if (!evt) {
        size_t len = strlen(evt_json);
        char snippet[121];
        size_t copy = len < 120 ? len : 120;
        memcpy(snippet, evt_json, copy);
        snippet[copy] = '\0';
        g_warning("profile_fetch: deserialize failed at index %u len=%zu json='%s'%s",
                  i, len, snippet, len > 120 ? "…" : "");
        continue;
      }

      const char *pk_hex = gnostr_event_get_pubkey(evt);
      const char *content = gnostr_event_get_content(evt);
      if (!pk_hex || !content)
        continue;

      ProfileApplyCtx *pctx = g_new0(ProfileApplyCtx, 1);
      pctx->pubkey_hex = g_strdup(pk_hex);
      pctx->content_json = g_strdup(content);
      g_ptr_array_add(items, pctx);
      dispatched++;

      uint8_t pk32[32];
      if (strlen(pk_hex) == 64 && hex_to_bytes32_local(pk_hex, pk32)) {
        uint64_t now = (uint64_t)(g_get_real_time() / G_USEC_PER_SEC);
        storage_ndb_write_last_profile_fetch(pk32, now);
      }
    }

    if (GNOSTR_IS_MAIN_WINDOW(ctx->self) && ctx->batch) {
      for (guint i = 0; i < ctx->batch->len; i++) {
        const char *requested_pk = g_ptr_array_index(ctx->batch, i);
        if (!requested_pk)
          continue;
        if (!g_hash_table_contains(unique_pks, requested_pk)) {
          profile_fetch_requested_remove(ctx->self, requested_pk);
        }
      }
    }
    g_hash_table_unref(unique_pks);

    g_debug("[PROFILE] ✓ Batch complete: %u profiles applied", dispatched);
    if (items->len > 0 && GNOSTR_IS_MAIN_WINDOW(ctx->self)) {
      gnostr_main_window_schedule_apply_profiles_internal(ctx->self, items);
      items = NULL;
    }
    if (items)
      g_ptr_array_free(items, TRUE);
    g_ptr_array_free(jsons, TRUE);
  } else {
    g_debug("[PROFILE] Batch returned no results");
    if (GNOSTR_IS_MAIN_WINDOW(ctx->self) && ctx->batch) {
      for (guint i = 0; i < ctx->batch->len; i++) {
        const char *requested_pk = g_ptr_array_index(ctx->batch, i);
        if (requested_pk)
          profile_fetch_requested_remove(ctx->self, requested_pk);
      }
    }
  }

  if (ctx->batch)
    g_ptr_array_free(ctx->batch, TRUE);

  if (GNOSTR_IS_MAIN_WINDOW(ctx->self)) {
    GnostrMainWindow *self = ctx->self;
    if (self->profile_fetch_active > 0)
      self->profile_fetch_active--;

    g_debug("[PROFILE] Batch %u/%u complete (active=%u/%u), dispatching next",
            self->profile_batch_pos,
            self->profile_batches ? self->profile_batches->len : 0,
            self->profile_fetch_active,
            profile_effective_max_concurrent(self));
    if (profile_startup_throttle_active(self)) {
      guint delay = self->startup_profile_inter_batch_delay_ms ?
                    self->startup_profile_inter_batch_delay_ms : 250;
      g_timeout_add_full(G_PRIORITY_DEFAULT,
                         delay,
                         profile_dispatch_next,
                         g_object_ref(self),
                         g_object_unref);
    } else {
      g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
                      profile_dispatch_next,
                      g_object_ref(self),
                      g_object_unref);
    }
  } else {
    g_warning("profile_fetch: cannot dispatch next batch - invalid context");
  }

  if (ctx->self)
    g_object_unref(ctx->self);
  g_free(ctx);
}

static gboolean
profile_dispatch_next(gpointer data)
{
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) {
    g_warning("profile_fetch: dispatch_next called with invalid self");
    return G_SOURCE_REMOVE;
  }

  guint max_concurrent = profile_effective_max_concurrent(self);
  if (self->profile_fetch_active >= max_concurrent) {
    g_debug("profile_fetch: at max concurrent (%u/%u), deferring batch",
            self->profile_fetch_active, max_concurrent);
    g_timeout_add_full(G_PRIORITY_DEFAULT, 500, profile_dispatch_next,
                       g_object_ref(self), g_object_unref);
    return G_SOURCE_REMOVE;
  }

  if (!self->profile_batches || self->profile_batch_pos >= self->profile_batches->len) {
    if (self->profile_batches) {
      g_debug("profile_fetch: sequence complete (batches=%u)", self->profile_batches->len);
      for (guint i = self->profile_batch_pos; i < self->profile_batches->len; i++) {
        GPtrArray *b = g_ptr_array_index(self->profile_batches, i);
        if (b)
          g_ptr_array_free(b, TRUE);
      }
      g_ptr_array_free(self->profile_batches, TRUE);
      self->profile_batches = NULL;
    } else {
      g_debug("profile_fetch: sequence complete (no batches)");
    }

    if (self->profile_batch_urls) {
      gnostr_main_window_free_urls_owned_internal(self->profile_batch_urls, self->profile_batch_url_count);
      self->profile_batch_urls = NULL;
      self->profile_batch_url_count = 0;
    }
    self->profile_batch_pos = 0;

    if (self->profile_fetch_queue && self->profile_fetch_queue->len > 0) {
      g_debug("profile_fetch: SEQUENCE COMPLETE - %u authors queued, scheduling new fetch",
              self->profile_fetch_queue->len);
      if (!self->profile_fetch_source_id) {
        guint delay = self->profile_fetch_debounce_ms ? self->profile_fetch_debounce_ms : 150;
        self->profile_fetch_source_id = g_timeout_add_full(G_PRIORITY_DEFAULT,
                                                           delay,
                                                           profile_fetch_fire_idle,
                                                           g_object_ref(self),
                                                           g_object_unref);
      } else {
        g_warning("profile_fetch: fetch already scheduled (source_id=%u)", self->profile_fetch_source_id);
      }
    } else {
      g_debug("profile_fetch: SEQUENCE COMPLETE - no authors queued");
    }
    return G_SOURCE_REMOVE;
  }

  if (!self->pool)
    self->pool = gnostr_pool_new();
  if (!self->profile_pool)
    self->profile_pool = gnostr_pool_new();
  if (!self->profile_fetch_cancellable)
    self->profile_fetch_cancellable = g_cancellable_new();
  if (g_cancellable_is_cancelled(self->profile_fetch_cancellable)) {
    if (self->profile_batches) {
      for (guint i = self->profile_batch_pos; i < self->profile_batches->len; i++) {
        GPtrArray *b = g_ptr_array_index(self->profile_batches, i);
        if (b) {
          profile_fetch_requested_remove_many(self, b);
          g_ptr_array_free(b, TRUE);
        }
      }
      g_ptr_array_free(self->profile_batches, TRUE);
      self->profile_batches = NULL;
    }
    if (self->profile_batch_urls) {
      gnostr_main_window_free_urls_owned_internal(self->profile_batch_urls, self->profile_batch_url_count);
      self->profile_batch_urls = NULL;
      self->profile_batch_url_count = 0;
    }
    self->profile_batch_pos = 0;
    return G_SOURCE_REMOVE;
  }

  guint batch_idx = self->profile_batch_pos;
  GPtrArray *batch = g_ptr_array_index(self->profile_batches, batch_idx);
  g_ptr_array_index(self->profile_batches, batch_idx) = NULL;
  self->profile_batch_pos++;
  if (!batch || batch->len == 0) {
    if (batch)
      g_ptr_array_free(batch, TRUE);
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
                    profile_dispatch_next,
                    g_object_ref(self),
                    (GDestroyNotify)g_object_unref);
    return G_SOURCE_REMOVE;
  }

  size_t n = batch->len;
  const char **authors = g_new0(const char *, n);
  for (guint i = 0; i < n; i++)
    authors[i] = (const char *)g_ptr_array_index(batch, i);

  ProfileBatchCtx *ctx = g_new0(ProfileBatchCtx, 1);
  ctx->self = g_object_ref(self);
  ctx->batch = batch;

  g_debug("[PROFILE] Dispatching batch %u/%u (%zu authors, active=%u/%u)",
          self->profile_batch_pos,
          self->profile_batches ? self->profile_batches->len : 0,
          n,
          self->profile_fetch_active,
          max_concurrent);

  self->profile_fetch_active++;

  gnostr_pool_sync_relays(self->profile_pool,
                          self->profile_batch_urls,
                          self->profile_batch_url_count);

  NostrFilter *f = nostr_filter_new();
  int kind0 = 0;
  nostr_filter_set_kinds(f, &kind0, 1);
  nostr_filter_set_authors(f, (const char *const *)authors, n);
  if (self->profile_batch_filters)
    nostr_filters_free(self->profile_batch_filters);
  self->profile_batch_filters = nostr_filters_new();
  nostr_filters_add(self->profile_batch_filters, f);
  nostr_filter_free(f);

  gnostr_pool_query_async(self->profile_pool,
                          self->profile_batch_filters,
                          self->profile_fetch_cancellable,
                          on_profiles_batch_done,
                          ctx);
  self->profile_batch_filters = NULL;

  g_free((gpointer)authors);
  return G_SOURCE_REMOVE;
}

static gboolean
hex_to_bytes32_local(const char *hex, uint8_t out[32])
{
  if (!hex || !out)
    return FALSE;
  if (strlen(hex) != 64)
    return FALSE;

  for (int i = 0; i < 32; i++) {
    char c1 = hex[i * 2];
    char c2 = hex[i * 2 + 1];
    int v1, v2;
    if (c1 >= '0' && c1 <= '9') v1 = c1 - '0';
    else if (c1 >= 'a' && c1 <= 'f') v1 = 10 + (c1 - 'a');
    else if (c1 >= 'A' && c1 <= 'F') v1 = 10 + (c1 - 'A');
    else return FALSE;
    if (c2 >= '0' && c2 <= '9') v2 = c2 - '0';
    else if (c2 >= 'a' && c2 <= 'f') v2 = 10 + (c2 - 'a');
    else if (c2 >= 'A' && c2 <= 'F') v2 = 10 + (c2 - 'A');
    else return FALSE;
    out[i] = (uint8_t)((v1 << 4) | v2);
  }
  return TRUE;
}

static void
profile_apply_item_free_local(gpointer p)
{
  ProfileApplyCtx *it = (ProfileApplyCtx *)p;
  if (!it)
    return;
  g_free(it->pubkey_hex);
  g_free(it->content_json);
  g_free(it);
}
