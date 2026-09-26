/* nd-registry.c — see nd-registry.h for the precedence contract. */
#include "nd-registry.h"
#include "nd-error.h"
#include "nd-kinds.h"
#include "nd-nip89.h"

#include <errno.h>
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#include <glib/gstdio.h>
#include <string.h>

#define GROUP_DEFAULTS "Default Handlers"
#define GROUP_REMOVED "Removed Handlers"
#define GROUP_DISPATCHER "Dispatcher"
#define KEY_FETCH_HINTS "fetch-relay-hints"

struct NdRegistry {
  GPtrArray *files;     /* GKeyFile*, precedence order */
  GPtrArray *apps;      /* NdAppDecl* */
  GHashTable *installed;/* desktop id -> NdAppDecl* (borrowed) */
};

const char *nd_source_name(NdSource s) {
  switch (s) {
  case ND_SOURCE_HANDLERS_LIST: return "handlers.list";
  case ND_SOURCE_DECLARED: return "X-Nostr-Kinds";
  case ND_SOURCE_NIP89: return "nip89";
  case ND_SOURCE_FALLBACK_LIST: return "handlers.list (*)";
  case ND_SOURCE_FALLBACK_DECLARED: return "X-Nostr-Kinds (*)";
  default: return "none";
  }
}

gboolean nd_desktop_id_valid(const char *id) {
  if (!id || !g_str_has_suffix(id, ".desktop") || strlen(id) <= 8 ||
      strlen(id) > 255)
    return FALSE;
  for (const char *p = id; *p; p++)
    if (!g_ascii_isalnum(*p) && *p != '_' && *p != '.' && *p != '-')
      return FALSE;
  return strcmp(id, ND_SELF_DESKTOP_ID) != 0;
}

NdAppDecl *nd_app_decl_new(const char *desktop_id, const char *kinds_spec) {
  NdAppDecl *d = g_new0(NdAppDecl, 1);
  d->desktop_id = g_strdup(desktop_id);
  d->kinds = kinds_spec ? nd_kind_spec_parse(kinds_spec) : NULL;
  return d;
}

void nd_app_decl_free(gpointer p) {
  NdAppDecl *d = p;
  if (!d) return;
  g_free(d->desktop_id);
  if (d->kinds) g_array_unref(d->kinds);
  g_free(d);
}

NdRegistry *nd_registry_new(const char *const *config_files, GPtrArray *apps) {
  NdRegistry *reg = g_new0(NdRegistry, 1);
  reg->files = g_ptr_array_new_with_free_func((GDestroyNotify)g_key_file_unref);
  reg->apps = apps ? apps : g_ptr_array_new_with_free_func(nd_app_decl_free);
  reg->installed = g_hash_table_new(g_str_hash, g_str_equal);

  for (guint i = 0; i < reg->apps->len; i++) {
    NdAppDecl *d = g_ptr_array_index(reg->apps, i);
    if (d && d->desktop_id && strcmp(d->desktop_id, ND_SELF_DESKTOP_ID) != 0)
      g_hash_table_insert(reg->installed, d->desktop_id, d);
  }

  for (guint i = 0; config_files && config_files[i]; i++) {
    GKeyFile *kf = g_key_file_new();
    g_autoptr(GError) err = NULL;
    if (g_key_file_load_from_file(kf, config_files[i], G_KEY_FILE_NONE, &err)) {
      g_ptr_array_add(reg->files, kf);
    } else {
      if (!g_error_matches(err, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        g_warning("nostr-dispatcher: ignoring %s: %s", config_files[i], err->message);
      g_key_file_unref(kf);
    }
  }
  return reg;
}

char *nd_registry_user_config_file(void) {
  return g_build_filename(g_get_user_config_dir(), "nostr", "handlers.list", NULL);
}

char **nd_registry_config_files(void) {
  GPtrArray *out = g_ptr_array_new();
  g_ptr_array_add(out, nd_registry_user_config_file());
  for (const char *const *d = g_get_system_config_dirs(); d && *d; d++)
    g_ptr_array_add(out, g_build_filename(*d, "nostr", "handlers.list", NULL));
  for (const char *const *d = g_get_system_data_dirs(); d && *d; d++)
    g_ptr_array_add(out, g_build_filename(*d, "nostr", "handlers.list", NULL));
  g_ptr_array_add(out, NULL);
  return (char **)g_ptr_array_free(out, FALSE);
}

NdRegistry *nd_registry_new_default(void) {
  GPtrArray *apps = g_ptr_array_new_with_free_func(nd_app_decl_free);
  GList *all = g_app_info_get_all();
  for (GList *l = all; l; l = l->next) {
    if (!G_IS_DESKTOP_APP_INFO(l->data)) continue;
    GDesktopAppInfo *info = l->data;
    const char *id = g_app_info_get_id(G_APP_INFO(info));
    if (!id || g_desktop_app_info_get_is_hidden(info)) continue;
    g_autofree char *spec = g_desktop_app_info_get_string(info, "X-Nostr-Kinds");
    g_ptr_array_add(apps, nd_app_decl_new(id, spec));
  }
  g_list_free_full(all, g_object_unref);

  g_auto(GStrv) files = nd_registry_config_files();
  return nd_registry_new((const char *const *)files, apps);
}

void nd_registry_free(NdRegistry *reg) {
  if (!reg) return;
  g_ptr_array_unref(reg->files);
  g_ptr_array_unref(reg->apps);
  g_hash_table_unref(reg->installed);
  g_free(reg);
}

gboolean nd_registry_fetch_relay_hints(NdRegistry *reg) {
  for (guint i = 0; reg && i < reg->files->len; i++) {
    GKeyFile *kf = g_ptr_array_index(reg->files, i);
    g_autoptr(GError) err = NULL;
    gboolean v = g_key_file_get_boolean(kf, GROUP_DISPATCHER, KEY_FETCH_HINTS, &err);
    if (!err) return v;
  }
  return TRUE;
}

/* First installed, valid desktop id from a ';' list value. */
static char *first_installed(NdRegistry *reg, GKeyFile *kf, const char *group,
                             const char *key) {
  g_auto(GStrv) ids = g_key_file_get_string_list(kf, group, key, NULL, NULL);
  for (guint i = 0; ids && ids[i]; i++) {
    const char *id = g_strstrip(ids[i]);
    if (!nd_desktop_id_valid(id)) {
      if (*id) g_debug("nostr-dispatcher: ignoring invalid desktop id in handlers.list");
      continue;
    }
    if (g_hash_table_contains(reg->installed, id)) return g_strdup(id);
  }
  return NULL;
}

typedef struct {
  char *key;
  guint32 width;
  guint order;
} KeyMatch;

static gint key_match_cmp(gconstpointer a, gconstpointer b) {
  const KeyMatch *x = a, *y = b;
  if (x->width != y->width) return x->width < y->width ? -1 : 1;
  return x->order < y->order ? -1 : (x->order > y->order ? 1 : 0);
}

/* Step 1 for one file: exact beats narrowest range, ties in file order;
 * fall through to the next-best key when no listed id is installed. */
static char *lookup_file_defaults(NdRegistry *reg, GKeyFile *kf, guint32 kind) {
  gsize n = 0;
  g_auto(GStrv) keys = g_key_file_get_keys(kf, GROUP_DEFAULTS, &n, NULL);
  g_autoptr(GArray) matches = g_array_new(FALSE, FALSE, sizeof(KeyMatch));
  for (gsize i = 0; keys && i < n; i++) {
    NdKindRange r;
    if (!nd_kind_token_parse(keys[i], &r)) {
      g_debug("nostr-dispatcher: ignoring handlers.list key '%s'", keys[i]);
      continue;
    }
    if (r.any || !nd_kind_range_contains(&r, kind)) continue;
    KeyMatch m = {keys[i], nd_kind_range_width(&r), (guint)i};
    g_array_append_val(matches, m);
  }
  g_array_sort(matches, key_match_cmp);
  for (guint i = 0; i < matches->len; i++) {
    char *id = first_installed(reg, kf, GROUP_DEFAULTS,
                               g_array_index(matches, KeyMatch, i).key);
    if (id) return id;
  }
  return NULL;
}

/* [Removed Handlers] across all files. kind < 0 only honours `*` keys. */
static gboolean is_removed(NdRegistry *reg, const char *desktop_id, gint kind) {
  for (guint f = 0; f < reg->files->len; f++) {
    GKeyFile *kf = g_ptr_array_index(reg->files, f);
    gsize n = 0;
    g_auto(GStrv) keys = g_key_file_get_keys(kf, GROUP_REMOVED, &n, NULL);
    for (gsize i = 0; keys && i < n; i++) {
      NdKindRange r;
      if (!nd_kind_token_parse(keys[i], &r)) continue;
      if (!r.any && (kind < 0 || !nd_kind_range_contains(&r, (guint32)kind)))
        continue;
      g_auto(GStrv) ids = g_key_file_get_string_list(kf, GROUP_REMOVED, keys[i], NULL, NULL);
      for (guint j = 0; ids && ids[j]; j++)
        if (strcmp(g_strstrip(ids[j]), desktop_id) == 0) return TRUE;
    }
  }
  return FALSE;
}

/* Step 2 (want_any = FALSE) or step 4b (want_any = TRUE). */
static char *lookup_declared(NdRegistry *reg, gint kind, gboolean want_any) {
  const NdAppDecl *best = NULL;
  guint32 best_w = G_MAXUINT32;
  for (guint i = 0; i < reg->apps->len; i++) {
    const NdAppDecl *d = g_ptr_array_index(reg->apps, i);
    if (!d->kinds || !g_hash_table_contains(reg->installed, d->desktop_id))
      continue;
    guint32 w = 0;
    if (want_any) {
      if (!nd_kind_spec_has_any(d->kinds)) continue;
    } else if (kind < 0 || !nd_kind_spec_best(d->kinds, (guint32)kind, &w)) {
      continue;
    }
    if (is_removed(reg, d->desktop_id, kind)) continue;
    if (!best || w < best_w ||
        (w == best_w && strcmp(d->desktop_id, best->desktop_id) < 0)) {
      best = d;
      best_w = w;
    }
  }
  return best ? g_strdup(best->desktop_id) : NULL;
}

char *nd_registry_choose(NdRegistry *reg, gint kind, NdSource *out_source) {
  NdSource src = ND_SOURCE_NONE;
  char *id = NULL;
  if (!reg) goto out;

  if (kind >= 0) {
    for (guint f = 0; f < reg->files->len && !id; f++)
      id = lookup_file_defaults(reg, g_ptr_array_index(reg->files, f), (guint32)kind);
    if (id) { src = ND_SOURCE_HANDLERS_LIST; goto out; }

    id = lookup_declared(reg, kind, FALSE);
    if (id) { src = ND_SOURCE_DECLARED; goto out; }

    id = nd_nip89_discover((guint32)kind);
    if (id && !(nd_desktop_id_valid(id) && g_hash_table_contains(reg->installed, id)))
      g_clear_pointer(&id, g_free);
    if (id) { src = ND_SOURCE_NIP89; goto out; }
  }

  for (guint f = 0; f < reg->files->len && !id; f++) {
    GKeyFile *kf = g_ptr_array_index(reg->files, f);
    if (g_key_file_has_key(kf, GROUP_DEFAULTS, "*", NULL))
      id = first_installed(reg, kf, GROUP_DEFAULTS, "*");
  }
  if (id) { src = ND_SOURCE_FALLBACK_LIST; goto out; }

  id = lookup_declared(reg, kind, TRUE);
  if (id) src = ND_SOURCE_FALLBACK_DECLARED;

out:
  if (out_source) *out_source = src;
  return id;
}

gboolean nd_registry_set_default(const char *path, const char *key,
                                 const char *desktop_id, GError **error) {
  NdKindRange r;
  if (!key || !nd_kind_token_parse(key, &r)) {
    g_set_error(error, ND_ERROR, ND_ERROR_INVALID_URI,
                "invalid kind '%s' (expected N, A-B or *)", key ? key : "");
    return FALSE;
  }
  if (!nd_desktop_id_valid(desktop_id)) {
    g_set_error(error, ND_ERROR, ND_ERROR_NO_HANDLER,
                "invalid desktop id '%s' (expected e.g. org.example.App.desktop)",
                desktop_id ? desktop_id : "");
    return FALSE;
  }

  g_autoptr(GKeyFile) kf = g_key_file_new();
  g_autoptr(GError) lerr = NULL;
  if (!g_key_file_load_from_file(kf, path,
                                 G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS,
                                 &lerr) &&
      !g_error_matches(lerr, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
    g_propagate_error(error, g_steal_pointer(&lerr));
    return FALSE;
  }

  g_autofree char *norm_key = g_strstrip(g_strdup(key));
  const char *ids[] = {desktop_id, NULL};
  g_key_file_set_string_list(kf, GROUP_DEFAULTS, norm_key, ids, 1);

  g_autofree char *dir = g_path_get_dirname(path);
  if (g_mkdir_with_parents(dir, 0700) != 0) {
    int e = errno;
    g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(e),
                "cannot create %s: %s", dir, g_strerror(e));
    return FALSE;
  }
  /* g_key_file_save_to_file writes via g_file_set_contents (tmp + rename). */
  return g_key_file_save_to_file(kf, path, error);
}
