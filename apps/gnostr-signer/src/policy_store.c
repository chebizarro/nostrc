#include "policy_store.h"
#include <string.h>
#include <time.h>

typedef struct {
  gboolean decision;
  guint64 expires_at; /* 0 = forever; epoch seconds */
} PolicyVal;

struct _PolicyStore {
  GHashTable *map; /* key: "identity|kind|app_id", value: PolicyVal* */
  gchar *path;     /* ~/.config/gnostr-signer/policy.ini */
};

/* On disk: group = identity, key = "<kind>|<app_id>" (legacy: "<app_id>").
 * Kinds contain no '|' and app ids (signer principals) never do. */
static gboolean is_kind(const gchar *s, gsize n) {
  if (n == 0 || n > 64) return FALSE;
  for (gsize i = 0; i < n; i++)
    if (!(g_ascii_islower(s[i]) || g_ascii_isdigit(s[i]) || s[i] == '_')) return FALSE;
  return TRUE;
}

static gchar *make_key(const gchar *kind, const gchar *app_id, const gchar *identity) {
  return g_strdup_printf("%s|%s|%s", identity ? identity : "",
                         (kind && *kind) ? kind : POLICY_KIND_EVENT, app_id ? app_id : "");
}

/* Split "identity|kind|app" (identity never contains '|'). */
static gboolean split_key(const gchar *ckey, gchar **identity, gchar **kind, const gchar **app) {
  const gchar *b1 = strchr(ckey, '|');
  if (!b1) return FALSE;
  const gchar *b2 = strchr(b1 + 1, '|');
  if (!b2) return FALSE;
  *identity = g_strndup(ckey, (gsize)(b1 - ckey));
  *kind = g_strndup(b1 + 1, (gsize)(b2 - b1 - 1));
  *app = b2 + 1;
  return TRUE;
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

void policy_entry_free(PolicyEntry *e) {
  if (!e) return;
  g_free(e->app_id);
  g_free(e->identity);
  g_free(e->kind);
  g_free(e);
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
    const gchar *group = groups[i]; /* identity */
    gsize nkeys = 0;
    gchar **keys = g_key_file_get_keys(kf, group, &nkeys, NULL);
    for (gsize j = 0; j < nkeys; j++) {
      const gchar *app_key = keys[j];
      /* Skip metadata keys of the form '<key>.expires' */
      if (g_str_has_suffix(app_key, ".expires")) continue;
      gboolean val = g_key_file_get_boolean(kf, group, app_key, NULL);
      g_autofree gchar *expkey = g_strdup_printf("%s.expires", app_key);
      guint64 expires_at = 0;
      if (g_key_file_has_key(kf, group, expkey, NULL)) {
        gchar *s = g_key_file_get_string(kf, group, expkey, NULL);
        if (s) { expires_at = g_ascii_strtoull(s, NULL, 10); g_free(s); }
      }
      const gchar *bar = strchr(app_key, '|');
      gchar *ckey = (bar && is_kind(app_key, (gsize)(bar - app_key)))
                      ? g_strdup_printf("%s|%s", group, app_key)
                      : make_key(POLICY_KIND_EVENT, app_key, group);
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
  GHashTableIter it; gpointer key, vptr;
  g_hash_table_iter_init(&it, ps->map);
  while (g_hash_table_iter_next(&it, &key, &vptr)) {
    gchar *identity = NULL, *kind = NULL;
    const gchar *app = NULL;
    if (!split_key(key, &identity, &kind, &app)) continue;
    /* Skip invalid/empty group names to satisfy GLib assertions */
    if (identity[0] != '\0') {
      PolicyVal *pv = vptr;
      g_autofree gchar *k = g_strdup_printf("%s|%s", kind, app);
      g_key_file_set_boolean(kf, identity, k, pv ? pv->decision : FALSE);
      if (pv && pv->expires_at != 0) {
        g_autofree gchar *expkey = g_strdup_printf("%s.expires", k);
        gchar buf[32]; g_snprintf(buf, sizeof(buf), "%" G_GUINT64_FORMAT, pv->expires_at);
        g_key_file_set_string(kf, identity, expkey, buf);
      }
    }
    g_free(identity);
    g_free(kind);
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

gboolean policy_store_get_for_kind(PolicyStore *ps, const gchar *kind, const gchar *app_id,
                                   const gchar *identity, gboolean *out_decision) {
  if (!ps) return FALSE;
  gchar *ckey = make_key(kind, app_id, identity);
  PolicyVal *pv = g_hash_table_lookup(ps->map, ckey);
  gboolean found = FALSE;
  if (pv) {
    guint64 now = (guint64)time(NULL);
    if (pv->expires_at != 0 && now >= pv->expires_at) {
      g_hash_table_remove(ps->map, ckey); /* prune expired entry */
    } else {
      if (out_decision) *out_decision = pv->decision ? TRUE : FALSE;
      found = TRUE;
    }
  }
  g_free(ckey);
  return found;
}

void policy_store_set_for_kind(PolicyStore *ps, const gchar *kind, const gchar *app_id,
                               const gchar *identity, gboolean decision, guint64 ttl_seconds) {
  if (!ps) return;
  PolicyVal *pv = g_new0(PolicyVal, 1);
  pv->decision = decision ? TRUE : FALSE;
  pv->expires_at = ttl_seconds == 0 ? 0 : (guint64)time(NULL) + ttl_seconds;
  g_hash_table_replace(ps->map, make_key(kind, app_id, identity), pv);
}

gboolean policy_store_unset_for_kind(PolicyStore *ps, const gchar *kind, const gchar *app_id,
                                     const gchar *identity) {
  if (!ps) return FALSE;
  gchar *ckey = make_key(kind, app_id, identity);
  gboolean removed = g_hash_table_remove(ps->map, ckey);
  g_free(ckey);
  return removed;
}

gboolean policy_store_get(PolicyStore *ps, const gchar *app_id, const gchar *identity, gboolean *out_decision) {
  return policy_store_get_for_kind(ps, POLICY_KIND_EVENT, app_id, identity, out_decision);
}

void policy_store_set(PolicyStore *ps, const gchar *app_id, const gchar *identity, gboolean decision) {
  policy_store_set_for_kind(ps, POLICY_KIND_EVENT, app_id, identity, decision, 0);
}

void policy_store_set_with_ttl(PolicyStore *ps, const gchar *app_id, const gchar *identity, gboolean decision, guint64 ttl_seconds) {
  policy_store_set_for_kind(ps, POLICY_KIND_EVENT, app_id, identity, decision, ttl_seconds);
}

gboolean policy_store_unset(PolicyStore *ps, const gchar *app_id, const gchar *identity) {
  return policy_store_unset_for_kind(ps, POLICY_KIND_EVENT, app_id, identity);
}

static void list_accum(gpointer key, gpointer value, gpointer user_data) {
  GPtrArray *arr = user_data;
  gchar *identity = NULL, *kind = NULL;
  const gchar *app = NULL;
  if (!split_key(key, &identity, &kind, &app)) return;
  PolicyEntry *e = g_new0(PolicyEntry, 1);
  e->identity = identity;
  e->kind = kind;
  e->app_id = g_strdup(app);
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
