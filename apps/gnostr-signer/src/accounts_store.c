#include "accounts_store.h"
#include <string.h>

struct _AccountsStore {
  GHashTable *map; /* id -> label */
  gchar *active;
  gchar *path;
};

static const char *config_path(void) {
  static gchar *p = NULL;
  if (!p) {
    const char *conf = g_get_user_config_dir();
    gchar *dir = g_build_filename(conf, "gnostr-signer", NULL);
    g_mkdir_with_parents(dir, 0700);
    p = g_build_filename(dir, "accounts.ini", NULL);
    g_free(dir);
  }
  return p;
}

AccountsStore *accounts_store_new(void) {
  AccountsStore *as = g_new0(AccountsStore, 1);
  as->map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  as->path = g_strdup(config_path());
  return as;
}

void accounts_store_free(AccountsStore *as) {
  if (!as) return;
  if (as->map) g_hash_table_destroy(as->map);
  g_free(as->active);
  g_free(as->path);
  g_free(as);
}

void accounts_store_load(AccountsStore *as) {
  if (!as) return;
  GKeyFile *kf = g_key_file_new();
  GError *err = NULL;
  if (!g_key_file_load_from_file(kf, as->path, G_KEY_FILE_NONE, &err)) {
    if (err) g_clear_error(&err);
    g_key_file_unref(kf);
    return;
  }
  /* accounts group */
  gsize nkeys = 0; gchar **keys = g_key_file_get_keys(kf, "accounts", &nkeys, NULL);
  for (gsize i = 0; i < nkeys; i++) {
    const gchar *id = keys[i];
    gchar *label = g_key_file_get_string(kf, "accounts", id, NULL);
    g_hash_table_replace(as->map, g_strdup(id), label ? label : g_strdup(""));
  }
  g_strfreev(keys);
  gchar *active = g_key_file_get_string(kf, "state", "active", NULL);
  if (active) { g_free(as->active); as->active = active; }
  g_key_file_unref(kf);
}

void accounts_store_save(AccountsStore *as) {
  if (!as) return;
  GKeyFile *kf = g_key_file_new();
  /* write accounts */
  GHashTableIter it; gpointer key, val;
  g_hash_table_iter_init(&it, as->map);
  while (g_hash_table_iter_next(&it, &key, &val)) {
    g_key_file_set_string(kf, "accounts", (const gchar*)key, (const gchar*)val);
  }
  if (as->active) g_key_file_set_string(kf, "state", "active", as->active);
  gsize len = 0; gchar *data = g_key_file_to_data(kf, &len, NULL);
  if (data) {
    GError *err = NULL;
    if (!g_file_set_contents(as->path, data, len, &err)) {
      if (err) { g_warning("accounts_store: save failed: %s", err->message); g_clear_error(&err);}    
    }
    g_free(data);
  }
  g_key_file_unref(kf);
}

static gboolean exists(AccountsStore *as, const gchar *id) {
  return g_hash_table_contains(as->map, id);
}

gboolean accounts_store_add(AccountsStore *as, const gchar *id, const gchar *label) {
  if (!as || !id || !*id) return FALSE;
  if (exists(as, id)) return FALSE;
  g_hash_table_insert(as->map, g_strdup(id), g_strdup(label ? label : ""));
  if (!as->active) as->active = g_strdup(id);
  return TRUE;
}

gboolean accounts_store_remove(AccountsStore *as, const gchar *id) {
  if (!as || !id) return FALSE;
  gboolean removed = g_hash_table_remove(as->map, id);
  if (removed && as->active && g_strcmp0(as->active, id) == 0) {
    g_clear_pointer(&as->active, g_free);
  }
  return removed;
}

GPtrArray *accounts_store_list(AccountsStore *as) {
  if (!as) return NULL;
  GPtrArray *arr = g_ptr_array_new();
  GHashTableIter it; gpointer key, val;
  g_hash_table_iter_init(&it, as->map);
  while (g_hash_table_iter_next(&it, &key, &val)) {
    AccountEntry *e = g_new0(AccountEntry, 1);
    e->id = g_strdup((const gchar*)key);
    e->label = g_strdup((const gchar*)val);
    g_ptr_array_add(arr, e);
  }
  return arr;
}

void accounts_store_set_active(AccountsStore *as, const gchar *id) {
  if (!as) return;
  g_free(as->active);
  as->active = id ? g_strdup(id) : NULL;
}

gboolean accounts_store_get_active(AccountsStore *as, gchar **out_id) {
  if (!as || !as->active) return FALSE;
  if (out_id) *out_id = g_strdup(as->active);
  return TRUE;
}

gboolean accounts_store_set_label(AccountsStore *as, const gchar *id, const gchar *label) {
  if (!as || !id || !*id) return FALSE;
  gpointer old_val = NULL;
  if (!g_hash_table_lookup_extended(as->map, id, NULL, &old_val)) return FALSE;
  /* Replace value with new label */
  g_hash_table_replace(as->map, g_strdup(id), g_strdup(label ? label : ""));
  return TRUE;
}
