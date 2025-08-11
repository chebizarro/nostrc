#include "policy_store.h"
#include <string.h>

struct _PolicyStore {
  GHashTable *map; /* key: composite "account|app_id", value: GINT_TO_POINTER(0/1) */
  gchar *path;     /* ~/.config/gnostr-signer/policy.ini */
};

static gchar *make_key(const gchar *app_id, const gchar *account) {
  /* Keyed by account then app for intuitive grouping */
  return g_strdup_printf("%s|%s", account ? account : "", app_id ? app_id : "");
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
  ps->map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
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
    const gchar *group = groups[i]; /* group format: account */
    gsize nkeys = 0;
    gchar **keys = g_key_file_get_keys(kf, group, &nkeys, NULL);
    for (gsize j = 0; j < nkeys; j++) {
      const gchar *app_id = keys[j];
      gboolean val = g_key_file_get_boolean(kf, group, app_id, NULL);
      gchar *ckey = g_strdup_printf("%s|%s", group, app_id);
      g_hash_table_replace(ps->map, ckey, GINT_TO_POINTER(val ? 1 : 0));
    }
    g_strfreev(keys);
  }
  g_strfreev(groups);
  g_key_file_unref(kf);
}

void policy_store_save(PolicyStore *ps) {
  if (!ps) return;
  GKeyFile *kf = g_key_file_new();
  /* Reconstruct grouped by account */
  GHashTableIter it; gpointer key, vptr;
  g_hash_table_iter_init(&it, ps->map);
  while (g_hash_table_iter_next(&it, &key, &vptr)) {
    const gchar *ckey = key; /* account|app */
    const gchar *bar = strchr(ckey, '|');
    if (!bar) continue;
    gchar *account = g_strndup(ckey, bar - ckey);
    const gchar *app = bar + 1;
    gboolean val = (GPOINTER_TO_INT(vptr) != 0);
    g_key_file_set_boolean(kf, account, app, val);
    g_free(account);
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

gboolean policy_store_get(PolicyStore *ps, const gchar *app_id, const gchar *account, gboolean *out_decision) {
  if (!ps) return FALSE;
  gchar *ckey = make_key(app_id, account);
  gpointer v = g_hash_table_lookup(ps->map, ckey);
  g_free(ckey);
  if (v == NULL) return FALSE;
  if (out_decision) *out_decision = (GPOINTER_TO_INT(v) != 0);
  return TRUE;
}

void policy_store_set(PolicyStore *ps, const gchar *app_id, const gchar *account, gboolean decision) {
  if (!ps) return;
  gchar *ckey = make_key(app_id, account);
  g_hash_table_replace(ps->map, ckey, GINT_TO_POINTER(decision ? 1 : 0));
}

gboolean policy_store_unset(PolicyStore *ps, const gchar *app_id, const gchar *account) {
  if (!ps) return FALSE;
  gchar *ckey = make_key(app_id, account);
  gboolean removed = g_hash_table_remove(ps->map, ckey);
  g_free(ckey);
  return removed;
}

static void list_accum(gpointer key, gpointer value, gpointer user_data) {
  GPtrArray *arr = user_data;
  const gchar *ckey = key; /* account|app */
  const gchar *bar = strchr(ckey, '|');
  if (!bar) return;
  PolicyEntry *e = g_new0(PolicyEntry, 1);
  e->account = g_strndup(ckey, bar - ckey);
  e->app_id = g_strdup(bar + 1);
  e->decision = (GPOINTER_TO_INT(value) != 0);
  g_ptr_array_add(arr, e);
}

GPtrArray *policy_store_list(PolicyStore *ps) {
  if (!ps) return NULL;
  GPtrArray *arr = g_ptr_array_new();
  g_hash_table_foreach(ps->map, list_accum, arr);
  return arr;
}
