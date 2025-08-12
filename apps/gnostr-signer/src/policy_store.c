#include "policy_store.h"
#include <string.h>
#include <time.h>

typedef struct {
  gboolean decision;
  guint64 expires_at; /* 0 = forever; epoch seconds */
} PolicyVal;

struct _PolicyStore {
  GHashTable *map; /* key: composite "identity|app_id", value: PolicyVal* */
  gchar *path;     /* ~/.config/gnostr-signer/policy.ini */
};

static gchar *make_key(const gchar *app_id, const gchar *identity) {
  /* Keyed by identity then app for intuitive grouping */
  return g_strdup_printf("%s|%s", identity ? identity : "", app_id ? app_id : "");
}

static const char *config_path(void) {
  static gchar *p = NULL;
  if (!p) {
    const char *conf = g_get_user_config_dir();
    gchar *dir = g_build_filename(conf, "gnostr-signer", NULL);
    g_mkdir_with_parents(dir, 0700);
    p = g_build_filename(dir, "policy.ini", NULL);
    g_free(dir);
  }
  return p;
}

PolicyStore *policy_store_new(void) {
  PolicyStore *ps = g_new0(PolicyStore, 1);
  ps->map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  ps->path = g_strdup(config_path());
  return ps;
}

void policy_store_free(PolicyStore *ps) {
  if (!ps) return;
  if (ps->map) g_hash_table_destroy(ps->map);
  g_free(ps->path);
  g_free(ps);
}

void policy_store_load(PolicyStore *ps) {
  if (!ps) return;
  GKeyFile *kf = g_key_file_new();
  GError *err = NULL;
  if (!g_key_file_load_from_file(kf, ps->path, G_KEY_FILE_NONE, &err)) {
    if (err) g_clear_error(&err);
    g_key_file_unref(kf);
    return;
  }
  gsize ngroups = 0;
  gchar **groups = g_key_file_get_groups(kf, &ngroups);
  for (gsize i = 0; i < ngroups; i++) {
    const gchar *group = groups[i]; /* group format: identity */
    gsize nkeys = 0;
    gchar **keys = g_key_file_get_keys(kf, group, &nkeys, NULL);
    for (gsize j = 0; j < nkeys; j++) {
      const gchar *app_key = keys[j];
      /* Skip metadata keys of the form '<app>.expires' */
      const gchar *dot = strrchr(app_key, '.');
      if (dot && g_strcmp0(dot, ".expires") == 0) continue;
      gboolean val = g_key_file_get_boolean(kf, group, app_key, NULL);
      /* Try to read optional expires */
      gchar *expkey = g_strdup_printf("%s.expires", app_key);
      guint64 expires_at = 0;
      if (g_key_file_has_key(kf, group, expkey, NULL)) {
        gchar *s = g_key_file_get_string(kf, group, expkey, NULL);
        if (s) { expires_at = g_ascii_strtoull(s, NULL, 10); g_free(s); }
      }
      g_free(expkey);
      gchar *ckey = g_strdup_printf("%s|%s", group, app_key);
      PolicyVal *pv = g_new0(PolicyVal, 1);
      pv->decision = val ? TRUE : FALSE;
      pv->expires_at = expires_at;
      g_hash_table_replace(ps->map, ckey, pv);
    }
    g_strfreev(keys);
  }
  g_strfreev(groups);
  g_key_file_unref(kf);
}

void policy_store_save(PolicyStore *ps) {
  if (!ps) return;
  GKeyFile *kf = g_key_file_new();
  /* Reconstruct grouped by identity */
  GHashTableIter it; gpointer key, vptr;
  g_hash_table_iter_init(&it, ps->map);
  while (g_hash_table_iter_next(&it, &key, &vptr)) {
    const gchar *ckey = key; /* identity|app */
    const gchar *bar = strchr(ckey, '|');
    if (!bar) continue;
    gchar *identity = g_strndup(ckey, bar - ckey);
    /* Skip invalid/empty group names to satisfy GLib assertions */
    if (!identity || identity[0] == '\0') { g_free(identity); continue; }
    const gchar *app = bar + 1;
    PolicyVal *pv = vptr;
    g_key_file_set_boolean(kf, identity, app, pv ? pv->decision : FALSE);
    if (pv && pv->expires_at != 0) {
      gchar *expkey = g_strdup_printf("%s.expires", app);
      gchar buf[32]; g_snprintf(buf, sizeof(buf), "%" G_GUINT64_FORMAT, pv->expires_at);
      g_key_file_set_string(kf, identity, expkey, buf);
      g_free(expkey);
    }
    g_free(identity);
  }
  gsize len = 0;
  gchar *data = g_key_file_to_data(kf, &len, NULL);
  if (data) {
    GError *err = NULL;
    if (!g_file_set_contents(ps->path, data, len, &err)) {
      if (err) { g_warning("policy_store: save failed: %s", err->message); g_clear_error(&err);}    
    }
    g_free(data);
  }
  g_key_file_unref(kf);
}

gboolean policy_store_get(PolicyStore *ps, const gchar *app_id, const gchar *identity, gboolean *out_decision) {
  if (!ps) return FALSE;
  gchar *ckey = make_key(app_id, identity);
  PolicyVal *pv = g_hash_table_lookup(ps->map, ckey);
  g_free(ckey);
  if (pv == NULL) return FALSE;
  /* Enforce expiry */
  guint64 now = (guint64)time(NULL);
  if (pv->expires_at != 0 && now >= pv->expires_at) {
    /* prune expired entry */
    gchar *rmkey = make_key(app_id, identity);
    g_hash_table_remove(ps->map, rmkey);
    g_free(rmkey);
    return FALSE;
  }
  if (out_decision) *out_decision = pv->decision ? TRUE : FALSE;
  return TRUE;
}

void policy_store_set(PolicyStore *ps, const gchar *app_id, const gchar *identity, gboolean decision) {
  if (!ps) return;
  gchar *ckey = make_key(app_id, identity);
  PolicyVal *pv = g_new0(PolicyVal, 1);
  pv->decision = decision ? TRUE : FALSE;
  pv->expires_at = 0; /* forever */
  g_hash_table_replace(ps->map, ckey, pv);
}

void policy_store_set_with_ttl(PolicyStore *ps, const gchar *app_id, const gchar *identity, gboolean decision, guint64 ttl_seconds) {
  if (!ps) return;
  gchar *ckey = make_key(app_id, identity);
  PolicyVal *pv = g_new0(PolicyVal, 1);
  pv->decision = decision ? TRUE : FALSE;
  if (ttl_seconds == 0) pv->expires_at = 0; else pv->expires_at = (guint64)time(NULL) + ttl_seconds;
  g_hash_table_replace(ps->map, ckey, pv);
}

gboolean policy_store_unset(PolicyStore *ps, const gchar *app_id, const gchar *identity) {
  if (!ps) return FALSE;
  gchar *ckey = make_key(app_id, identity);
  gboolean removed = g_hash_table_remove(ps->map, ckey);
  g_free(ckey);
  return removed;
}

static void list_accum(gpointer key, gpointer value, gpointer user_data) {
  GPtrArray *arr = user_data;
  const gchar *ckey = key; /* identity|app */
  const gchar *bar = strchr(ckey, '|');
  if (!bar) return;
  PolicyEntry *e = g_new0(PolicyEntry, 1);
  e->identity = g_strdup(ckey);
  e->identity[bar - ckey] = '\0';
  e->app_id = g_strdup(bar + 1);
  PolicyVal *pv = value;
  e->decision = pv ? pv->decision : FALSE;
  e->expires_at = pv ? pv->expires_at : 0;
  g_ptr_array_add(arr, e);
}

GPtrArray *policy_store_list(PolicyStore *ps) {
  if (!ps) return NULL;
  GPtrArray *arr = g_ptr_array_new();
  g_hash_table_foreach(ps->map, list_accum, arr);
  return arr;
}
