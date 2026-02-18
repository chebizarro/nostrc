#include "nostr_profile_provider.h"
#include "nostr_utils.h"
#include "storage_ndb.h"
#include <gio/gio.h>
#include <json.h>          /* nostr_json_is_object_str (no GObject wrapper yet) */
#include "nostr_json.h"    /* GObject JSON utilities */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Thread safety: all cache operations are protected by this mutex.
 * GTK apps commonly access profiles from multiple threads (main thread,
 * async callbacks, etc.) so we must protect shared state. */
G_LOCK_DEFINE_STATIC(profile_provider);

static GHashTable *s_cache = NULL;
static GQueue *s_lru = NULL;
static GHashTable *s_lru_nodes = NULL;
static guint s_cap = 0;
static gboolean s_init = FALSE;
static GnostrProfileProviderStats s_stats = {0};

/* Follow-list provider callback (set by app layer) */
static GnostrFollowListProvider s_follow_list_provider = NULL;

/* Profile update watchers */
typedef struct {
  guint id;
  char *pubkey_hex;
  GnostrProfileWatchCallback callback;
  gpointer user_data;
} ProfileWatch;

static GSList *s_watches = NULL;
static guint s_next_watch_id = 1;

/* Data for dispatching watcher callbacks on the main thread */
typedef struct {
  GnostrProfileWatchCallback callback;
  GnostrProfileMeta *meta;  /* owned copy */
  gpointer user_data;
} WatchDispatch;

void gnostr_profile_provider_init(guint cap) {
  G_LOCK(profile_provider);
  if (s_init) { G_UNLOCK(profile_provider); return; }
  s_init = TRUE;

  if (cap == 0) {
    const char *env = g_getenv("GNOSTR_PROFILE_CAP");
    if (env && *env) {
      long v = strtol(env, NULL, 10);
      if (v > 0 && v < 1000000) s_cap = (guint)v;
    }
  } else {
    s_cap = cap;
  }
  if (s_cap == 0) s_cap = 3000;

  s_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)gnostr_profile_meta_free);
  s_lru = g_queue_new();
  s_lru_nodes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  G_UNLOCK(profile_provider);

  g_message("[PROFILE_PROVIDER] Init cap=%u", s_cap);
}

void gnostr_profile_provider_shutdown(void) {
  G_LOCK(profile_provider);
  if (!s_init) { G_UNLOCK(profile_provider); return; }
  if (s_cache) { g_hash_table_destroy(s_cache); s_cache = NULL; }
  if (s_lru) {
    while (!g_queue_is_empty(s_lru)) g_free(g_queue_pop_head(s_lru));
    g_queue_free(s_lru);
    s_lru = NULL;
  }
  if (s_lru_nodes) { g_hash_table_destroy(s_lru_nodes); s_lru_nodes = NULL; }
  /* Clean up watchers */
  for (GSList *l = s_watches; l; l = l->next) {
    ProfileWatch *w = l->data;
    g_free(w->pubkey_hex);
    g_free(w);
  }
  g_slist_free(s_watches);
  s_watches = NULL;
  s_init = FALSE;
  G_UNLOCK(profile_provider);
}

/* LRU helpers */
static void lru_touch(const char *pk) {
  if (!pk || !s_lru || !s_lru_nodes) return;
  GList *n = g_hash_table_lookup(s_lru_nodes, pk);
  if (!n) return;
  g_queue_unlink(s_lru, n);
  g_queue_push_tail_link(s_lru, n);
}

static void lru_insert(const char *pk) {
  if (!pk || !s_lru || !s_lru_nodes) return;
  if (g_hash_table_contains(s_lru_nodes, pk)) { lru_touch(pk); return; }
  char *k = g_strdup(pk);
  GList *n = g_list_alloc(); n->data = k;
  g_queue_push_tail_link(s_lru, n);
  g_hash_table_insert(s_lru_nodes, g_strdup(pk), n);
}

static void lru_evict(void) {
  if (!s_lru || !s_lru_nodes || !s_cache) return;
  while (g_hash_table_size(s_lru_nodes) > s_cap) {
    GList *old = s_lru->head;
    if (!old) break;
    char *k = (char*)old->data;
    g_queue_unlink(s_lru, old);
    g_list_free_1(old);
    g_hash_table_remove(s_lru_nodes, k);
    g_hash_table_remove(s_cache, k);
    g_free(k);
  }
}

/* Parse profile from JSON.
 * The json_str may be either:
 * 1. A kind-0 event (full nostr event with content field containing profile JSON)
 * 2. A raw profile object (display_name, name, picture, etc. at top level)
 * We detect which format and extract accordingly. */
static GnostrProfileMeta *meta_from_json(const char *pk, const char *json_str) {
  if (!pk || !json_str) return NULL;
  if (!gnostr_json_is_valid(json_str)) return NULL;
  if (!gnostr_json_is_object_str(json_str)) return NULL;

  GnostrProfileMeta *m = g_new0(GnostrProfileMeta, 1);
  m->pubkey_hex = g_strdup(pk);

  /* Check if this is a kind-0 event by looking for "content" field.
   * If found, the profile metadata is inside the content field as nested JSON. */
  const char *profile_json = json_str;
  gchar *content_str = gnostr_json_get_string(json_str, "content", NULL);
  if (content_str && *content_str) {
    /* This is a kind-0 event - parse the content field */
    if (gnostr_json_is_valid(content_str) && gnostr_json_is_object_str(content_str)) {
      profile_json = content_str;
    }
  }

  gchar *tmp = gnostr_json_get_string(profile_json, "display_name", NULL);
  if (tmp && *tmp) {
    m->display_name = tmp;
  } else {
    g_free(tmp);
  }

  tmp = gnostr_json_get_string(profile_json, "name", NULL);
  if (tmp && *tmp) {
    m->name = tmp;
  } else {
    g_free(tmp);
  }

  tmp = gnostr_json_get_string(profile_json, "picture", NULL);
  if (tmp && *tmp) {
    m->picture = tmp;
  } else {
    g_free(tmp);
  }

  tmp = gnostr_json_get_string(profile_json, "banner", NULL);
  if (tmp && *tmp) {
    m->banner = tmp;
  } else {
    g_free(tmp);
  }

  tmp = gnostr_json_get_string(profile_json, "nip05", NULL);
  if (tmp && *tmp) {
    m->nip05 = tmp;
  } else {
    g_free(tmp);
  }

  tmp = gnostr_json_get_string(profile_json, "lud16", NULL);
  if (tmp && *tmp) {
    m->lud16 = tmp;
  } else {
    g_free(tmp);
  }

  /* Extract created_at from kind-0 event if available */
  GError *err = NULL;
  gint64 created_at = gnostr_json_get_int64(json_str, "created_at", &err);
  if (!err) {
    m->created_at = created_at;
  } else {
    g_clear_error(&err);
  }

  g_free(content_str);
  return m;
}

/* Query DB -- read profile fields directly from NdbProfile FlatBuffer,
 * skipping the wasteful FlatBuffer -> JSON -> struct round-trip. */
static GnostrProfileMeta *meta_from_db(const char *pk) {
  if (!pk || strlen(pk) != 64) return NULL;
  unsigned char pk32[32];
  for (int i = 0; i < 32; i++) {
    unsigned int b;
    if (sscanf(pk + i*2, "%2x", &b) != 1) return NULL;
    pk32[i] = (unsigned char)b;
  }
  void *txn = NULL;
  if (storage_ndb_begin_query(&txn, NULL) != 0 || !txn) return NULL;

  StorageNdbProfileMeta fb = {0};
  int rc = storage_ndb_get_profile_meta_direct(txn, pk32, &fb, NULL);
  storage_ndb_end_query(txn);

  if (rc != 0) {
    G_LOCK(profile_provider);
    s_stats.db_misses++;
    G_UNLOCK(profile_provider);
    return NULL;
  }

  /* Build GnostrProfileMeta from the FlatBuffer-extracted fields.
   * The strings are already g_strdup'd copies, so we transfer ownership. */
  GnostrProfileMeta *m = g_new0(GnostrProfileMeta, 1);
  m->pubkey_hex   = g_strdup(pk);
  m->display_name = fb.display_name;  /* transfer ownership */
  m->name         = fb.name;
  m->picture      = fb.picture;
  m->banner       = fb.banner;
  m->nip05        = fb.nip05;
  m->lud16        = fb.lud16;
  m->created_at   = (gint64)fb.created_at;

  /* about, website, lud06 are not in GnostrProfileMeta -- free them */
  g_free(fb.about);
  g_free(fb.website);
  g_free(fb.lud06);

  G_LOCK(profile_provider);
  s_stats.db_hits++;
  G_UNLOCK(profile_provider);
  return m;
}

/* Copy profile */
static GnostrProfileMeta *meta_copy(const GnostrProfileMeta *src) {
  if (!src) return NULL;
  GnostrProfileMeta *c = g_new0(GnostrProfileMeta, 1);
  c->pubkey_hex = g_strdup(src->pubkey_hex);
  c->display_name = src->display_name ? g_strdup(src->display_name) : NULL;
  c->name = src->name ? g_strdup(src->name) : NULL;
  c->picture = src->picture ? g_strdup(src->picture) : NULL;
  c->banner = src->banner ? g_strdup(src->banner) : NULL;
  c->nip05 = src->nip05 ? g_strdup(src->nip05) : NULL;
  c->lud16 = src->lud16 ? g_strdup(src->lud16) : NULL;
  c->created_at = src->created_at;
  return c;
}

GnostrProfileMeta *gnostr_profile_provider_get(const char *pk) {
  /* Defensively normalize npub/nprofile to hex */
  g_autofree gchar *hex_pk = NULL;
  if (pk && strlen(pk) != 64) {
    hex_pk = gnostr_ensure_hex_pubkey(pk);
    pk = hex_pk;
  }
  if (!pk || strlen(pk) != 64) return NULL;

  G_LOCK(profile_provider);
  if (!s_init) { G_UNLOCK(profile_provider); return NULL; }
  GnostrProfileMeta *cached = g_hash_table_lookup(s_cache, pk);
  if (cached) {
    s_stats.hits++;
    lru_touch(pk);
    GnostrProfileMeta *result = meta_copy(cached);
    G_UNLOCK(profile_provider);
    return result;
  }
  s_stats.misses++;
  G_UNLOCK(profile_provider);

  /* Query DB without holding lock (I/O can be slow) */
  GnostrProfileMeta *m = meta_from_db(pk);
  if (m) {
    G_LOCK(profile_provider);
    if (s_init) { /* re-check in case of shutdown race */
      GnostrProfileMeta *tc = meta_copy(m);
      g_hash_table_replace(s_cache, g_strdup(pk), tc);
      lru_insert(pk);
      lru_evict();
      s_stats.cache_size = g_hash_table_size(s_cache);
    }
    G_UNLOCK(profile_provider);
  }
  return m;
}

int gnostr_profile_provider_get_batch(const char **pks, guint cnt, GnostrProfileMeta ***out, guint *ocnt) {
  if (!s_init || !pks || !out || !ocnt) return -1;
  GnostrProfileMeta **ms = g_new0(GnostrProfileMeta*, cnt);
  guint f = 0;
  for (guint i = 0; i < cnt; i++) {
    GnostrProfileMeta *m = gnostr_profile_provider_get(pks[i]);
    if (m) ms[f++] = m;
  }
  *out = ms;
  *ocnt = f;
  return 0;
}

static gboolean watch_dispatch_idle(gpointer data) {
  WatchDispatch *wd = data;
  wd->callback(wd->meta->pubkey_hex, wd->meta, wd->user_data);
  gnostr_profile_meta_free(wd->meta);
  g_free(wd);
  return G_SOURCE_REMOVE;
}

int gnostr_profile_provider_update(const char *pk, const char *json) {
  if (!pk || !json) return -1;
  /* Defensively normalize npub/nprofile to hex */
  g_autofree gchar *hex_pk = NULL;
  if (strlen(pk) != 64) {
    hex_pk = gnostr_ensure_hex_pubkey(pk);
    if (!hex_pk) return -1;
    pk = hex_pk;
  }
  /* Parse JSON without holding lock (can be slow) */
  GnostrProfileMeta *m = meta_from_json(pk, json);
  if (!m) return -1;

  G_LOCK(profile_provider);
  if (!s_init) {
    G_UNLOCK(profile_provider);
    gnostr_profile_meta_free(m);
    return -1;
  }
  g_hash_table_replace(s_cache, g_strdup(pk), m);
  lru_insert(pk);
  lru_evict();
  s_stats.cache_size = g_hash_table_size(s_cache);

  /* Collect matching watchers while holding the lock */
  GSList *dispatches = NULL;
  for (GSList *l = s_watches; l; l = l->next) {
    ProfileWatch *w = l->data;
    if (g_strcmp0(w->pubkey_hex, pk) == 0) {
      WatchDispatch *wd = g_new0(WatchDispatch, 1);
      wd->callback = w->callback;
      wd->meta = meta_copy(m);
      wd->user_data = w->user_data;
      dispatches = g_slist_prepend(dispatches, wd);
    }
  }
  G_UNLOCK(profile_provider);

  /* Dispatch to main thread outside the lock */
  for (GSList *l = dispatches; l; l = l->next) {
    g_idle_add(watch_dispatch_idle, l->data);
  }
  g_slist_free(dispatches);

  return 0;
}

void gnostr_profile_meta_free(GnostrProfileMeta *m) {
  if (!m) return;
  g_free(m->pubkey_hex);
  g_free(m->display_name);
  g_free(m->name);
  g_free(m->picture);
  g_free(m->banner);
  g_free(m->nip05);
  g_free(m->lud16);
  g_free(m);
}

guint gnostr_profile_provider_watch(const char *pubkey_hex,
                                    GnostrProfileWatchCallback callback,
                                    gpointer user_data) {
  if (!pubkey_hex || !callback) return 0;
  /* Defensively normalize npub/nprofile to hex */
  g_autofree gchar *hex = gnostr_ensure_hex_pubkey(pubkey_hex);
  if (!hex) return 0;

  G_LOCK(profile_provider);
  ProfileWatch *w = g_new0(ProfileWatch, 1);
  w->id = s_next_watch_id++;
  w->pubkey_hex = g_strdup(hex);
  w->callback = callback;
  w->user_data = user_data;
  s_watches = g_slist_prepend(s_watches, w);
  G_UNLOCK(profile_provider);

  return w->id;
}

void gnostr_profile_provider_unwatch(guint watch_id) {
  if (watch_id == 0) return;

  G_LOCK(profile_provider);
  for (GSList *l = s_watches; l; l = l->next) {
    ProfileWatch *w = l->data;
    if (w->id == watch_id) {
      s_watches = g_slist_delete_link(s_watches, l);
      g_free(w->pubkey_hex);
      g_free(w);
      break;
    }
  }
  G_UNLOCK(profile_provider);
}

void gnostr_profile_provider_get_stats(GnostrProfileProviderStats *st) {
  if (!st) return;
  G_LOCK(profile_provider);
  st->cache_size = s_stats.cache_size;
  st->cache_cap = s_cap;
  st->hits = s_stats.hits;
  st->misses = s_stats.misses;
  st->db_hits = s_stats.db_hits;
  st->db_misses = s_stats.db_misses;
  G_UNLOCK(profile_provider);
}

void gnostr_profile_provider_log_stats(void) {
  G_LOCK(profile_provider);
  guint cache_size = s_stats.cache_size;
  guint cap = s_cap;
  guint64 hits = s_stats.hits;
  guint64 misses = s_stats.misses;
  guint64 db_hits = s_stats.db_hits;
  guint64 db_misses = s_stats.db_misses;
  G_UNLOCK(profile_provider);
  g_message("[PROFILE_PROVIDER] cache=%u/%u hits=%" G_GUINT64_FORMAT " misses=%" G_GUINT64_FORMAT
            " db_hits=%" G_GUINT64_FORMAT " db_misses=%" G_GUINT64_FORMAT,
            cache_size, cap, hits, misses, db_hits, db_misses);
}

void gnostr_profile_provider_set_follow_list_provider(GnostrFollowListProvider provider) {
  G_LOCK(profile_provider);
  s_follow_list_provider = provider;
  G_UNLOCK(profile_provider);
}

/* Check if a pubkey is already in the LRU cache (lock must NOT be held). */
static gboolean
is_pubkey_cached(const char *pk)
{
  if (!pk || strlen(pk) != 64) return FALSE;
  G_LOCK(profile_provider);
  gboolean cached = s_init && s_cache && g_hash_table_contains(s_cache, pk);
  G_UNLOCK(profile_provider);
  return cached;
}

/* Background task to prefetch a batch of profiles into cache.
 * This runs gnostr_profile_provider_get() for each pubkey, which populates
 * the LRU cache from NDB on a worker thread. */
static void
prefetch_batch_task_func(GTask *task, gpointer source_object G_GNUC_UNUSED,
                         gpointer task_data, GCancellable *cancellable G_GNUC_UNUSED)
{
  gchar **pubkeys = (gchar **)task_data;
  guint warmed = 0;

  for (guint i = 0; pubkeys[i] != NULL; i++) {
    /* Skip if already cached -- avoids unnecessary NDB transaction */
    if (is_pubkey_cached(pubkeys[i])) continue;

    GnostrProfileMeta *m = gnostr_profile_provider_get(pubkeys[i]);
    if (m) {
      warmed++;
      gnostr_profile_meta_free(m);
    }
  }

  g_task_return_int(task, (gssize)warmed);
}

static void
prefetch_batch_task_done(GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                         gpointer user_data G_GNUC_UNUSED)
{
  GTask *task = G_TASK(result);
  gssize warmed = g_task_propagate_int(task, NULL);
  if (warmed > 0) {
    g_debug("[PROFILE_PROVIDER] Prefetch batch complete: %d profiles loaded from NDB",
            (int)warmed);
  }
}

void
gnostr_profile_provider_prefetch_batch_async(const char **pubkeys_hex)
{
  if (!pubkeys_hex || !s_init) return;

  /* Count and copy the pubkey array -- caller retains ownership of original */
  guint count = 0;
  for (const char **p = pubkeys_hex; *p; p++) count++;
  if (count == 0) return;

  gchar **copy = g_new0(gchar *, count + 1);
  for (guint i = 0; i < count; i++) {
    copy[i] = g_strdup(pubkeys_hex[i]);
  }
  copy[count] = NULL;

  GTask *task = g_task_new(NULL, NULL, prefetch_batch_task_done, NULL);
  g_task_set_task_data(task, copy, (GDestroyNotify)g_strfreev);
  g_task_run_in_thread(task, prefetch_batch_task_func);
  g_object_unref(task);

  g_debug("[PROFILE_PROVIDER] Prefetch batch started for %u pubkeys", count);
}

/* Pre-warm LRU cache from NDB for user + follow list profiles.
 * Runs in a GTask worker thread to avoid blocking startup or UI. */
static void
prewarm_task_func(GTask *task, gpointer source_object G_GNUC_UNUSED,
                  gpointer task_data, GCancellable *cancellable G_GNUC_UNUSED)
{
  const char *user_pk = (const char *)task_data;
  guint warmed = 0;

  /* 1. Pre-warm the user's own profile */
  GnostrProfileMeta *m = gnostr_profile_provider_get(user_pk);
  if (m) {
    warmed++;
    gnostr_profile_meta_free(m);
  }

  /* 2. Get follow list pubkeys via registered provider and pre-warm each */
  G_LOCK(profile_provider);
  GnostrFollowListProvider provider = s_follow_list_provider;
  G_UNLOCK(profile_provider);

  if (provider) {
    gchar **follow_pks = provider(user_pk);
    if (follow_pks) {
      for (guint i = 0; follow_pks[i]; i++) {
        m = gnostr_profile_provider_get(follow_pks[i]);
        if (m) {
          warmed++;
          gnostr_profile_meta_free(m);
        }
      }
      g_strfreev(follow_pks);
    }
  }

  g_task_return_int(task, (gssize)warmed);
}

static void
prewarm_task_done(GObject *source G_GNUC_UNUSED, GAsyncResult *result,
                  gpointer user_data G_GNUC_UNUSED)
{
  GTask *task = G_TASK(result);
  gssize warmed = g_task_propagate_int(task, NULL);
  g_debug("[PROFILE_PROVIDER] Pre-warm complete: %d profiles loaded from NDB",
          (int)warmed);
  gnostr_profile_provider_log_stats();
}

void
gnostr_profile_provider_prewarm_async(const char *user_pubkey_hex)
{
  if (!user_pubkey_hex || !s_init) return;

  GTask *task = g_task_new(NULL, NULL, prewarm_task_done, NULL);
  g_task_set_task_data(task, g_strdup(user_pubkey_hex), g_free);
  g_task_run_in_thread(task, prewarm_task_func);
  g_object_unref(task);

  g_debug("[PROFILE_PROVIDER] Pre-warm started for user %.16s...", user_pubkey_hex);
}
