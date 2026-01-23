#include "gnostr-profile-provider.h"
#include "../storage_ndb.h"
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static GHashTable *s_cache = NULL;
static GQueue *s_lru = NULL;
static GHashTable *s_lru_nodes = NULL;
static guint s_cap = 0;
static gboolean s_init = FALSE;
static GnostrProfileProviderStats s_stats = {0};

void gnostr_profile_provider_init(guint cap) {
  if (s_init) return;
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
  
  g_message("[PROFILE_PROVIDER] Init cap=%u", s_cap);
}

void gnostr_profile_provider_shutdown(void) {
  if (!s_init) return;
  if (s_cache) { g_hash_table_destroy(s_cache); s_cache = NULL; }
  if (s_lru) {
    while (!g_queue_is_empty(s_lru)) g_free(g_queue_pop_head(s_lru));
    g_queue_free(s_lru);
    s_lru = NULL;
  }
  if (s_lru_nodes) { g_hash_table_destroy(s_lru_nodes); s_lru_nodes = NULL; }
  s_init = FALSE;
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

/* Parse profile from JSON */
static GnostrProfileMeta *meta_from_json(const char *pk, const char *json) {
  if (!pk || !json) return NULL;
  GError *err = NULL;
  JsonParser *p = json_parser_new();
  if (!json_parser_load_from_data(p, json, -1, &err)) {
    g_clear_error(&err);
    g_object_unref(p);
    return NULL;
  }
  JsonNode *root = json_parser_get_root(p);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) { g_object_unref(p); return NULL; }
  JsonObject *obj = json_node_get_object(root);
  GnostrProfileMeta *m = g_new0(GnostrProfileMeta, 1);
  m->pubkey_hex = g_strdup(pk);
  if (json_object_has_member(obj, "display_name")) {
    const char *v = json_object_get_string_member(obj, "display_name");
    if (v && *v) m->display_name = g_strdup(v);
  }
  if (json_object_has_member(obj, "name")) {
    const char *v = json_object_get_string_member(obj, "name");
    if (v && *v) m->name = g_strdup(v);
  }
  if (json_object_has_member(obj, "picture")) {
    const char *v = json_object_get_string_member(obj, "picture");
    if (v && *v) m->picture = g_strdup(v);
  }
  if (json_object_has_member(obj, "nip05")) {
    const char *v = json_object_get_string_member(obj, "nip05");
    if (v && *v) m->nip05 = g_strdup(v);
  }
  if (json_object_has_member(obj, "lud16")) {
    const char *v = json_object_get_string_member(obj, "lud16");
    if (v && *v) m->lud16 = g_strdup(v);
  }
  m->created_at = 0;
  g_object_unref(p);
  return m;
}

/* Query DB */
static GnostrProfileMeta *meta_from_db(const char *pk) {
  if (!pk || strlen(pk) != 64) return NULL;
  unsigned char pk32[32];
  for (int i = 0; i < 32; i++) {
    unsigned int b;
    if (sscanf(pk + i*2, "%2x", &b) != 1) return NULL;
    pk32[i] = (unsigned char)b;
  }
  void *txn = NULL;
  if (storage_ndb_begin_query(&txn) != 0 || !txn) return NULL;
  char *json = NULL; int len = 0;
  int rc = storage_ndb_get_profile_by_pubkey(txn, pk32, &json, &len);
  storage_ndb_end_query(txn);
  if (rc != 0 || !json) { s_stats.db_misses++; return NULL; }
  s_stats.db_hits++;
  GnostrProfileMeta *m = meta_from_json(pk, json);
  free(json);
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
  c->nip05 = src->nip05 ? g_strdup(src->nip05) : NULL;
  c->lud16 = src->lud16 ? g_strdup(src->lud16) : NULL;
  c->created_at = src->created_at;
  return c;
}

GnostrProfileMeta *gnostr_profile_provider_get(const char *pk) {
  if (!s_init || !pk || strlen(pk) != 64) return NULL;
  GnostrProfileMeta *cached = g_hash_table_lookup(s_cache, pk);
  if (cached) {
    s_stats.hits++;
    lru_touch(pk);
    return meta_copy(cached);
  }
  s_stats.misses++;
  GnostrProfileMeta *m = meta_from_db(pk);
  if (m) {
    GnostrProfileMeta *tc = meta_copy(m);
    g_hash_table_replace(s_cache, g_strdup(pk), tc);
    lru_insert(pk);
    lru_evict();
    s_stats.cache_size = g_hash_table_size(s_cache);
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

int gnostr_profile_provider_update(const char *pk, const char *json) {
  if (!s_init || !pk || !json) return -1;
  GnostrProfileMeta *m = meta_from_json(pk, json);
  if (!m) return -1;
  g_hash_table_replace(s_cache, g_strdup(pk), m);
  lru_insert(pk);
  lru_evict();
  s_stats.cache_size = g_hash_table_size(s_cache);
  return 0;
}

void gnostr_profile_meta_free(GnostrProfileMeta *m) {
  if (!m) return;
  g_free(m->pubkey_hex);
  g_free(m->display_name);
  g_free(m->name);
  g_free(m->picture);
  g_free(m->nip05);
  g_free(m->lud16);
  g_free(m);
}

void gnostr_profile_provider_get_stats(GnostrProfileProviderStats *st) {
  if (!st) return;
  st->cache_size = s_stats.cache_size;
  st->cache_cap = s_cap;
  st->hits = s_stats.hits;
  st->misses = s_stats.misses;
  st->db_hits = s_stats.db_hits;
  st->db_misses = s_stats.db_misses;
}

void gnostr_profile_provider_log_stats(void) {
  g_message("[PROFILE_PROVIDER] cache=%u/%u hits=%" G_GUINT64_FORMAT " misses=%" G_GUINT64_FORMAT
            " db_hits=%" G_GUINT64_FORMAT " db_misses=%" G_GUINT64_FORMAT,
            s_stats.cache_size, s_cap, s_stats.hits, s_stats.misses, 
            s_stats.db_hits, s_stats.db_misses);
}
