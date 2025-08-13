#include "relays.h"

#include <gio/gio.h>
#include <string.h>

/* -- Settings persistence (GKeyFile) -- */
gchar *gnostr_config_path(void) {
  const char *override = g_getenv("GNOSTR_CONFIG_PATH");
  if (override && *override) {
    /* Ensure parent dir exists */
    gchar *parent = g_path_get_dirname(override);
    if (parent && *parent && g_strcmp0(parent, ".") != 0)
      g_mkdir_with_parents(parent, 0700);
    g_free(parent);
    return g_strdup(override);
  }
  const char *xdg = g_get_user_config_dir();
  gchar *dir = g_build_filename(xdg ? xdg : g_get_home_dir(), "gnostr", NULL);
  g_mkdir_with_parents(dir, 0700);
  gchar *path = g_build_filename(dir, "config.ini", NULL);
  g_free(dir);
  return path; /* caller frees */
}

gchar *config_dir(void) {
  const gchar *env = g_getenv("GNOSTR_CONFIG_DIR");
  if (env && *env) return g_strdup(env);
  return g_build_filename(g_get_user_config_dir(), "gnostr", NULL);
}

/* --- Relay Manager --- */
gboolean gnostr_is_valid_relay_url(const char *url) {
  if (!url || !*url) return FALSE;
  /* Basic check: must be ws:// or wss:// and a valid URI */
  if (!g_str_has_prefix(url, "ws://") && !g_str_has_prefix(url, "wss://")) return FALSE;
  GError *err = NULL;
  GUri *uri = g_uri_parse(url, G_URI_FLAGS_NONE, &err);
  if (!uri) { g_clear_error(&err); return FALSE; }
  const char *host = g_uri_get_host(uri);
  gboolean ok = (host && *host);
  g_uri_unref(uri);
  return ok;
}

gchar *gnostr_normalize_relay_url(const char *url) {
  if (!url) return NULL;
  /* Trim whitespace */
  gchar *trimmed = g_strstrip(g_strdup(url));
  if (!gnostr_is_valid_relay_url(trimmed)) { g_free(trimmed); return NULL; }
  GError *err = NULL;
  GUri *uri = g_uri_parse(trimmed, G_URI_FLAGS_NONE, &err);
  g_free(trimmed);
  if (!uri) { g_clear_error(&err); return NULL; }
  const char *scheme = g_uri_get_scheme(uri);
  const char *host = g_uri_get_host(uri);
  int port = g_uri_get_port(uri);
  const char *path = g_uri_get_path(uri);
  /* Lowercase scheme and host */
  gchar *lscheme = g_ascii_strdown(scheme ? scheme : "wss", -1);
  gchar *lhost = g_ascii_strdown(host ? host : "", -1);
  gboolean drop_trailing = (path && g_strcmp0(path, "/") == 0);
  gchar *norm = NULL;
  if (port > 0) {
    norm = g_strdup_printf("%s://%s:%d", lscheme, lhost, port);
  } else {
    norm = g_strdup_printf("%s://%s", lscheme, lhost);
  }
  /* If there is a non-root path, append it */
  if (path && *path && !drop_trailing) {
    gchar *tmp = g_strconcat(norm, path, NULL);
    g_free(norm);
    norm = tmp;
  }
  g_free(lscheme);
  g_free(lhost);
  g_uri_unref(uri);
  return norm;
}

void gnostr_load_relays_into(GPtrArray *out) {
  if (!out) return;
  /* Try GSettings first if schema exists */
  GSettings *settings = NULL;
  GSettingsSchemaSource *src = g_settings_schema_source_get_default();
  if (src) {
    GSettingsSchema *schema = g_settings_schema_source_lookup(src, "org.gnostr.gnostr", TRUE);
    if (schema) {
      settings = g_settings_new("org.gnostr.gnostr");
      g_settings_schema_unref(schema);
    }
  }
  gboolean loaded = FALSE;
  if (settings) {
    gsize n = 0;
    gchar **arr = g_settings_get_strv(settings, "relays");
    if (arr) {
      guint added = 0;
      for (gsize i = 0; arr[i] != NULL; i++) {
        if (arr[i] && *arr[i]) {
          g_ptr_array_add(out, g_strdup(arr[i]));
          added++;
        }
      }
      g_debug("relays: loaded %u from GSettings", added);
      loaded = (added > 0);
      g_strfreev(arr);
    }
  }
  if (!loaded) {
    /* Fallback: keyfile */
    gchar *cfg = gnostr_config_path();
    GKeyFile *kf = g_key_file_new();
    GError *err = NULL;
    if (g_key_file_load_from_file(kf, cfg, G_KEY_FILE_NONE, &err)) {
      gsize n = 0;
      gchar **urls = g_key_file_get_string_list(kf, "relays", "urls", &n, NULL);
      if (urls) {
        for (gsize i = 0; i < n; i++) {
          if (!urls[i] || !*urls[i]) continue;
          g_ptr_array_add(out, g_strdup(urls[i]));
        }
        g_strfreev(urls);
        loaded = (out->len > 0);
        g_debug("relays: loaded %u from keyfile %s", loaded ? (guint)out->len : 0, cfg);
      }
    } else {
      g_clear_error(&err);
    }
    g_key_file_free(kf);
    g_free(cfg);
    /* If we have settings and we loaded from keyfile, migrate to GSettings */
    if (settings && loaded) {
      /* Build strv */
      g_auto(GStrv) strv = NULL;
      strv = g_new0(gchar*, out->len + 1);
      for (guint i = 0; i < out->len; i++) strv[i] = g_strdup(out->pdata[i]);
      g_settings_set_strv(settings, "relays", (const gchar * const*)strv);
      g_debug("relays: migrated %u entries to GSettings", out->len);
    }
  }
  if (settings) g_object_unref(settings);
}

void gnostr_save_relays_from(GPtrArray *list) {
  if (!list) return;
  /* Write to GSettings if available */
  gboolean wrote_settings = FALSE;
  GSettings *settings = NULL;
  GSettingsSchemaSource *src = g_settings_schema_source_get_default();
  if (src) {
    GSettingsSchema *schema = g_settings_schema_source_lookup(src, "org.gnostr.gnostr", TRUE);
    if (schema) {
      settings = g_settings_new("org.gnostr.gnostr");
      g_settings_schema_unref(schema);
    }
  }
  if (settings) {
    g_auto(GStrv) strv = g_new0(gchar*, list->len + 1);
    for (guint i = 0; i < list->len; i++) strv[i] = g_strdup((const gchar*)list->pdata[i]);
    wrote_settings = g_settings_set_strv(settings, "relays", (const gchar * const*)strv);
    g_debug("relays: saved %u to GSettings (ok=%d)", list->len, wrote_settings ? 1 : 0);
    g_object_unref(settings);
  }
  /* Always maintain keyfile as fallback */
  gchar *cfg = gnostr_config_path();
  gchar *dir = g_path_get_dirname(cfg);
  g_mkdir_with_parents(dir, 0700);
  g_free(dir);
  GKeyFile *kf = g_key_file_new();
  /* Build a dedicated GStrv of DUPLICATED strings.
   * We must not borrow pointers from 'list' here because the caller
   * typically owns and frees those strings (e.g. via g_ptr_array_free
   * with a free func). Since we free this GStrv via g_strfreev() when
   * leaving the scope, duplicate each element to avoid double free. */
  g_auto(GStrv) arr = g_new0(gchar*, list->len + 1);
  for (guint i = 0; i < list->len; i++) arr[i] = g_strdup((const gchar*)list->pdata[i]);
  g_key_file_set_string_list(kf, "relays", "urls", (const gchar* const*)arr, list->len);
  GError *err = NULL;
  gchar *data = g_key_file_to_data(kf, NULL, &err);
  if (data) {
    if (!g_file_set_contents(cfg, data, -1, &err)) {
      g_warning("failed to write %s: %s", cfg, err ? err->message : "(unknown)");
      g_clear_error(&err);
    }
    g_debug("relays: wrote %u to keyfile %s", list->len, cfg);
    g_free(data);
  } else {
    g_clear_error(&err);
  }
  g_key_file_free(kf);
  g_free(cfg);
}
