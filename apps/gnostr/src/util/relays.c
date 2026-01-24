#define G_LOG_DOMAIN "gnostr-relays"

#include "relays.h"
#include "relay_info.h"

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

/* --- NIP-65 Relay List Metadata --- */

#include <json-glib/json-glib.h>

#ifndef GNOSTR_RELAY_TEST_ONLY
/* Full NIP-65 async support requires SimplePool */
#include "nostr_simple_pool.h"
#include "nostr-filter.h"
#include "nostr-event.h"
#endif

void gnostr_nip65_relay_free(GnostrNip65Relay *relay) {
  if (!relay) return;
  g_free(relay->url);
  g_free(relay);
}

GPtrArray *gnostr_nip65_parse_event(const gchar *event_json, gint64 *out_created_at) {
  if (!event_json) return NULL;

  JsonParser *parser = json_parser_new();
  if (!json_parser_load_from_data(parser, event_json, -1, NULL)) {
    g_object_unref(parser);
    return NULL;
  }

  JsonNode *root_node = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_OBJECT(root_node)) {
    g_object_unref(parser);
    return NULL;
  }

  JsonObject *root = json_node_get_object(root_node);

  /* Verify this is kind 10002 */
  if (json_object_has_member(root, "kind")) {
    gint64 kind = json_object_get_int_member(root, "kind");
    if (kind != 10002) {
      g_object_unref(parser);
      return NULL;
    }
  }

  /* Extract created_at if requested */
  if (out_created_at && json_object_has_member(root, "created_at")) {
    *out_created_at = json_object_get_int_member(root, "created_at");
  }

  /* Parse tags */
  GPtrArray *relays = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_nip65_relay_free);

  if (!json_object_has_member(root, "tags")) {
    g_object_unref(parser);
    return relays;
  }

  JsonNode *tags_node = json_object_get_member(root, "tags");
  if (!JSON_NODE_HOLDS_ARRAY(tags_node)) {
    g_object_unref(parser);
    return relays;
  }

  JsonArray *tags = json_node_get_array(tags_node);
  guint n = json_array_get_length(tags);

  for (guint i = 0; i < n; i++) {
    JsonNode *tag_node = json_array_get_element(tags, i);
    if (!JSON_NODE_HOLDS_ARRAY(tag_node)) continue;

    JsonArray *tag = json_node_get_array(tag_node);
    guint tag_len = json_array_get_length(tag);
    if (tag_len < 2) continue;

    /* Check for "r" tag */
    const gchar *tag_type = json_array_get_string_element(tag, 0);
    if (g_strcmp0(tag_type, "r") != 0) continue;

    const gchar *url = json_array_get_string_element(tag, 1);
    if (!url || !*url) continue;

    /* Validate URL */
    if (!gnostr_is_valid_relay_url(url)) continue;

    GnostrNip65Relay *relay = g_new0(GnostrNip65Relay, 1);
    relay->url = gnostr_normalize_relay_url(url);
    relay->type = GNOSTR_RELAY_READWRITE;

    /* Check for read/write marker */
    if (tag_len >= 3) {
      const gchar *marker = json_array_get_string_element(tag, 2);
      if (g_strcmp0(marker, "read") == 0) {
        relay->type = GNOSTR_RELAY_READ;
      } else if (g_strcmp0(marker, "write") == 0) {
        relay->type = GNOSTR_RELAY_WRITE;
      }
    }

    g_ptr_array_add(relays, relay);
  }

  g_object_unref(parser);
  return relays;
}

GPtrArray *gnostr_nip65_get_write_relays(GPtrArray *nip65_relays) {
  GPtrArray *result = g_ptr_array_new_with_free_func(g_free);
  if (!nip65_relays) return result;

  for (guint i = 0; i < nip65_relays->len; i++) {
    GnostrNip65Relay *relay = g_ptr_array_index(nip65_relays, i);
    /* Write relays are where the user publishes to - we read from these */
    if (relay->type == GNOSTR_RELAY_WRITE || relay->type == GNOSTR_RELAY_READWRITE) {
      g_ptr_array_add(result, g_strdup(relay->url));
    }
  }

  return result;
}

GPtrArray *gnostr_nip65_get_read_relays(GPtrArray *nip65_relays) {
  GPtrArray *result = g_ptr_array_new_with_free_func(g_free);
  if (!nip65_relays) return result;

  for (guint i = 0; i < nip65_relays->len; i++) {
    GnostrNip65Relay *relay = g_ptr_array_index(nip65_relays, i);
    /* Read relays are where the user reads from - we publish to these */
    if (relay->type == GNOSTR_RELAY_READ || relay->type == GNOSTR_RELAY_READWRITE) {
      g_ptr_array_add(result, g_strdup(relay->url));
    }
  }

  return result;
}

#ifndef GNOSTR_RELAY_TEST_ONLY
/* Async NIP-65 fetch context - requires SimplePool */
typedef struct {
  gchar *pubkey_hex;
  GCancellable *cancellable;
  GnostrNip65RelayCallback callback;
  gpointer user_data;
} Nip65FetchCtx;

static void nip65_fetch_ctx_free(Nip65FetchCtx *ctx) {
  if (!ctx) return;
  g_free(ctx->pubkey_hex);
  if (ctx->cancellable) g_object_unref(ctx->cancellable);
  g_free(ctx);
}

static void on_nip65_query_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  Nip65FetchCtx *ctx = (Nip65FetchCtx*)user_data;
  if (!ctx) return;

  GError *err = NULL;
  GPtrArray *results = gnostr_simple_pool_query_single_finish(GNOSTR_SIMPLE_POOL(source), res, &err);

  if (err) {
    if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_warning("nip65: query failed: %s", err->message);
    }
    g_error_free(err);
    if (ctx->callback) ctx->callback(NULL, ctx->user_data);
    nip65_fetch_ctx_free(ctx);
    return;
  }

  GPtrArray *relays = NULL;
  gint64 newest_created_at = 0;

  /* Find the newest kind 10002 event */
  if (results && results->len > 0) {
    for (guint i = 0; i < results->len; i++) {
      const gchar *json = g_ptr_array_index(results, i);
      gint64 created_at = 0;
      GPtrArray *parsed = gnostr_nip65_parse_event(json, &created_at);
      if (parsed) {
        if (created_at > newest_created_at) {
          if (relays) g_ptr_array_unref(relays);
          relays = parsed;
          newest_created_at = created_at;
        } else {
          g_ptr_array_unref(parsed);
        }
      }
    }
  }

  if (results) g_ptr_array_unref(results);

  if (ctx->callback) ctx->callback(relays, ctx->user_data);
  nip65_fetch_ctx_free(ctx);
}

void gnostr_nip65_fetch_relays_async(const gchar *pubkey_hex,
                                      GCancellable *cancellable,
                                      GnostrNip65RelayCallback callback,
                                      gpointer user_data) {
  if (!pubkey_hex || !*pubkey_hex) {
    if (callback) callback(NULL, user_data);
    return;
  }

  Nip65FetchCtx *ctx = g_new0(Nip65FetchCtx, 1);
  ctx->pubkey_hex = g_strdup(pubkey_hex);
  ctx->cancellable = cancellable ? g_object_ref(cancellable) : NULL;
  ctx->callback = callback;
  ctx->user_data = user_data;

  /* Build filter for kind 10002 */
  NostrFilter *filter = nostr_filter_new();
  int kinds[1] = { 10002 };
  nostr_filter_set_kinds(filter, kinds, 1);
  const char *authors[1] = { pubkey_hex };
  nostr_filter_set_authors(filter, authors, 1);
  nostr_filter_set_limit(filter, 1);

  /* Get configured relays */
  GPtrArray *relay_arr = g_ptr_array_new_with_free_func(g_free);
  gnostr_load_relays_into(relay_arr);

  /* Relays come from GSettings with defaults configured in schema */

  /* Build URL array */
  const char **urls = g_new0(const char*, relay_arr->len);
  for (guint i = 0; i < relay_arr->len; i++) {
    urls[i] = g_ptr_array_index(relay_arr, i);
  }

  /* Use static pool */
  static GnostrSimplePool *nip65_pool = NULL;
  if (!nip65_pool) nip65_pool = gnostr_simple_pool_new();

  gnostr_simple_pool_query_single_async(
    nip65_pool,
    urls,
    relay_arr->len,
    filter,
    ctx->cancellable,
    on_nip65_query_done,
    ctx
  );

  g_free(urls);
  g_ptr_array_unref(relay_arr);
  nostr_filter_free(filter);
}
#endif /* GNOSTR_RELAY_TEST_ONLY */

/* --- NIP-17 DM Relay List (kind 10050) --- */

void gnostr_load_dm_relays_into(GPtrArray *out) {
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
    gchar **arr = g_settings_get_strv(settings, "dm-relays");
    if (arr) {
      guint added = 0;
      for (gsize i = 0; arr[i] != NULL; i++) {
        if (arr[i] && *arr[i]) {
          g_ptr_array_add(out, g_strdup(arr[i]));
          added++;
        }
      }
      g_debug("dm-relays: loaded %u from GSettings", added);
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
      gchar **urls = g_key_file_get_string_list(kf, "dm-relays", "urls", &n, NULL);
      if (urls) {
        for (gsize i = 0; i < n; i++) {
          if (!urls[i] || !*urls[i]) continue;
          g_ptr_array_add(out, g_strdup(urls[i]));
        }
        g_strfreev(urls);
        loaded = (out->len > 0);
        g_debug("dm-relays: loaded %u from keyfile %s", loaded ? (guint)out->len : 0, cfg);
      }
    } else {
      g_clear_error(&err);
    }
    g_key_file_free(kf);
    g_free(cfg);
    /* If we have settings and we loaded from keyfile, migrate to GSettings */
    if (settings && loaded) {
      g_auto(GStrv) strv = g_new0(gchar*, out->len + 1);
      for (guint i = 0; i < out->len; i++) strv[i] = g_strdup(out->pdata[i]);
      g_settings_set_strv(settings, "dm-relays", (const gchar * const*)strv);
      g_debug("dm-relays: migrated %u entries to GSettings", out->len);
    }
  }
  if (settings) g_object_unref(settings);
}

void gnostr_save_dm_relays_from(GPtrArray *list) {
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
    wrote_settings = g_settings_set_strv(settings, "dm-relays", (const gchar * const*)strv);
    g_debug("dm-relays: saved %u to GSettings (ok=%d)", list->len, wrote_settings ? 1 : 0);
    g_object_unref(settings);
  }
  /* Always maintain keyfile as fallback */
  gchar *cfg = gnostr_config_path();
  GKeyFile *kf = g_key_file_new();
  /* Load existing config to preserve other sections */
  g_key_file_load_from_file(kf, cfg, G_KEY_FILE_NONE, NULL);
  g_auto(GStrv) arr = g_new0(gchar*, list->len + 1);
  for (guint i = 0; i < list->len; i++) arr[i] = g_strdup((const gchar*)list->pdata[i]);
  g_key_file_set_string_list(kf, "dm-relays", "urls", (const gchar* const*)arr, list->len);
  GError *err = NULL;
  gchar *data = g_key_file_to_data(kf, NULL, &err);
  if (data) {
    gchar *dir = g_path_get_dirname(cfg);
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    if (!g_file_set_contents(cfg, data, -1, &err)) {
      g_warning("failed to write dm-relays to %s: %s", cfg, err ? err->message : "(unknown)");
      g_clear_error(&err);
    }
    g_debug("dm-relays: wrote %u to keyfile %s", list->len, cfg);
    g_free(data);
  } else {
    g_clear_error(&err);
  }
  g_key_file_free(kf);
  g_free(cfg);
}

GPtrArray *gnostr_nip17_parse_dm_relays_event(const gchar *event_json, gint64 *out_created_at) {
  if (!event_json) return NULL;

  JsonParser *parser = json_parser_new();
  if (!json_parser_load_from_data(parser, event_json, -1, NULL)) {
    g_object_unref(parser);
    return NULL;
  }

  JsonNode *root_node = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_OBJECT(root_node)) {
    g_object_unref(parser);
    return NULL;
  }

  JsonObject *root = json_node_get_object(root_node);

  /* Verify this is kind 10050 (NIP-17 DM relay list) */
  if (json_object_has_member(root, "kind")) {
    gint64 kind = json_object_get_int_member(root, "kind");
    if (kind != 10050) {
      g_object_unref(parser);
      return NULL;
    }
  }

  /* Extract created_at if requested */
  if (out_created_at && json_object_has_member(root, "created_at")) {
    *out_created_at = json_object_get_int_member(root, "created_at");
  }

  /* Parse tags - looking for "relay" tags */
  GPtrArray *relays = g_ptr_array_new_with_free_func(g_free);

  if (!json_object_has_member(root, "tags")) {
    g_object_unref(parser);
    return relays;
  }

  JsonNode *tags_node = json_object_get_member(root, "tags");
  if (!JSON_NODE_HOLDS_ARRAY(tags_node)) {
    g_object_unref(parser);
    return relays;
  }

  JsonArray *tags = json_node_get_array(tags_node);
  guint n = json_array_get_length(tags);

  for (guint i = 0; i < n; i++) {
    JsonNode *tag_node = json_array_get_element(tags, i);
    if (!JSON_NODE_HOLDS_ARRAY(tag_node)) continue;

    JsonArray *tag = json_node_get_array(tag_node);
    guint tag_len = json_array_get_length(tag);
    if (tag_len < 2) continue;

    /* Check for "relay" tag (NIP-17 uses "relay" not "r") */
    const gchar *tag_type = json_array_get_string_element(tag, 0);
    if (g_strcmp0(tag_type, "relay") != 0) continue;

    const gchar *url = json_array_get_string_element(tag, 1);
    if (!url || !*url) continue;

    /* Validate and normalize URL */
    if (!gnostr_is_valid_relay_url(url)) continue;

    gchar *normalized = gnostr_normalize_relay_url(url);
    if (normalized) {
      g_ptr_array_add(relays, normalized);
    }
  }

  g_object_unref(parser);
  return relays;
}

GPtrArray *gnostr_get_dm_relays(void) {
  GPtrArray *dm_relays = g_ptr_array_new_with_free_func(g_free);
  gnostr_load_dm_relays_into(dm_relays);

  /* Fall back to general relays if no DM-specific relays configured */
  if (dm_relays->len == 0) {
    gnostr_load_relays_into(dm_relays);
  }

  /* DM relays come from GSettings with defaults configured in schema */

  return dm_relays;
}

#ifndef GNOSTR_RELAY_TEST_ONLY
/* Async NIP-17 DM relay fetch context */
typedef struct {
  gchar *pubkey_hex;
  GCancellable *cancellable;
  GnostrNip17DmRelayCallback callback;
  gpointer user_data;
} Nip17DmFetchCtx;

static void nip17_dm_fetch_ctx_free(Nip17DmFetchCtx *ctx) {
  if (!ctx) return;
  g_free(ctx->pubkey_hex);
  if (ctx->cancellable) g_object_unref(ctx->cancellable);
  g_free(ctx);
}

static void on_nip17_dm_query_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  Nip17DmFetchCtx *ctx = (Nip17DmFetchCtx*)user_data;
  if (!ctx) return;

  GError *err = NULL;
  GPtrArray *results = gnostr_simple_pool_query_single_finish(GNOSTR_SIMPLE_POOL(source), res, &err);

  if (err) {
    if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_warning("nip17-dm: query failed: %s", err->message);
    }
    g_error_free(err);
    if (ctx->callback) ctx->callback(NULL, ctx->user_data);
    nip17_dm_fetch_ctx_free(ctx);
    return;
  }

  GPtrArray *relays = NULL;
  gint64 newest_created_at = 0;

  /* Find the newest kind 10050 event */
  if (results && results->len > 0) {
    for (guint i = 0; i < results->len; i++) {
      const gchar *json = g_ptr_array_index(results, i);
      gint64 created_at = 0;
      GPtrArray *parsed = gnostr_nip17_parse_dm_relays_event(json, &created_at);
      if (parsed) {
        if (created_at > newest_created_at) {
          if (relays) g_ptr_array_unref(relays);
          relays = parsed;
          newest_created_at = created_at;
        } else {
          g_ptr_array_unref(parsed);
        }
      }
    }
  }

  if (results) g_ptr_array_unref(results);

  if (ctx->callback) ctx->callback(relays, ctx->user_data);
  nip17_dm_fetch_ctx_free(ctx);
}

void gnostr_nip17_fetch_dm_relays_async(const gchar *pubkey_hex,
                                         GCancellable *cancellable,
                                         GnostrNip17DmRelayCallback callback,
                                         gpointer user_data) {
  if (!pubkey_hex || !*pubkey_hex) {
    if (callback) callback(NULL, user_data);
    return;
  }

  Nip17DmFetchCtx *ctx = g_new0(Nip17DmFetchCtx, 1);
  ctx->pubkey_hex = g_strdup(pubkey_hex);
  ctx->cancellable = cancellable ? g_object_ref(cancellable) : NULL;
  ctx->callback = callback;
  ctx->user_data = user_data;

  /* Build filter for kind 10050 */
  NostrFilter *filter = nostr_filter_new();
  int kinds[1] = { 10050 };
  nostr_filter_set_kinds(filter, kinds, 1);
  const char *authors[1] = { pubkey_hex };
  nostr_filter_set_authors(filter, authors, 1);
  nostr_filter_set_limit(filter, 1);

  /* Get configured relays - prefer DM relays, fall back to general */
  GPtrArray *relay_arr = g_ptr_array_new_with_free_func(g_free);
  gnostr_load_dm_relays_into(relay_arr);
  if (relay_arr->len == 0) {
    gnostr_load_relays_into(relay_arr);
  }

  /* Relays come from GSettings with defaults configured in schema */

  /* Build URL array */
  const char **urls = g_new0(const char*, relay_arr->len);
  for (guint i = 0; i < relay_arr->len; i++) {
    urls[i] = g_ptr_array_index(relay_arr, i);
  }

  /* Use static pool for NIP-17 queries */
  static GnostrSimplePool *nip17_dm_pool = NULL;
  if (!nip17_dm_pool) nip17_dm_pool = gnostr_simple_pool_new();

  gnostr_simple_pool_query_single_async(
    nip17_dm_pool,
    urls,
    relay_arr->len,
    filter,
    ctx->cancellable,
    on_nip17_dm_query_done,
    ctx
  );

  g_free(urls);
  g_ptr_array_unref(relay_arr);
  nostr_filter_free(filter);
}
#endif /* GNOSTR_RELAY_TEST_ONLY */

/* --- NIP-65 Local Config with Types --- */

/* Helper to convert type enum to string for storage */
static const gchar *relay_type_to_string(GnostrRelayType type) {
  switch (type) {
    case GNOSTR_RELAY_READ: return "r";
    case GNOSTR_RELAY_WRITE: return "w";
    case GNOSTR_RELAY_READWRITE:
    default: return "rw";
  }
}

/* Helper to convert string to type enum */
static GnostrRelayType relay_type_from_string(const gchar *str) {
  if (!str || !*str) return GNOSTR_RELAY_READWRITE;
  if (g_strcmp0(str, "r") == 0) return GNOSTR_RELAY_READ;
  if (g_strcmp0(str, "w") == 0) return GNOSTR_RELAY_WRITE;
  return GNOSTR_RELAY_READWRITE;
}

GPtrArray *gnostr_load_nip65_relays(void) {
  GPtrArray *result = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_nip65_relay_free);

  /* Load URLs */
  GPtrArray *urls = g_ptr_array_new_with_free_func(g_free);
  gnostr_load_relays_into(urls);

  if (urls->len == 0) {
    g_ptr_array_unref(urls);
    return result;
  }

  /* Load types from GSettings */
  gchar **types = NULL;
  gsize types_len = 0;
  GSettingsSchemaSource *src = g_settings_schema_source_get_default();
  if (src) {
    GSettingsSchema *schema = g_settings_schema_source_lookup(src, "org.gnostr.gnostr", TRUE);
    if (schema) {
      GSettings *settings = g_settings_new("org.gnostr.gnostr");
      types = g_settings_get_strv(settings, "relay-types");
      /* Count types array length (NULL-terminated) */
      if (types) {
        while (types[types_len] != NULL) types_len++;
      }
      g_settings_schema_unref(schema);
      g_object_unref(settings);
    }
  }

  /* Build NIP-65 relay list */
  for (guint i = 0; i < urls->len; i++) {
    const gchar *url = g_ptr_array_index(urls, i);
    GnostrNip65Relay *relay = g_new0(GnostrNip65Relay, 1);
    relay->url = g_strdup(url);

    /* Get type if available (check bounds), default to read+write */
    if (types && i < types_len && types[i]) {
      relay->type = relay_type_from_string(types[i]);
    } else {
      relay->type = GNOSTR_RELAY_READWRITE;
    }

    g_ptr_array_add(result, relay);
  }

  if (types) g_strfreev(types);
  g_ptr_array_unref(urls);

  return result;
}

/* Forward declaration for relay change emit */
void gnostr_relay_change_emit(void);

void gnostr_save_nip65_relays(GPtrArray *relays) {
  if (!relays) return;

  /* Build URL and type arrays */
  GPtrArray *urls = g_ptr_array_new_with_free_func(g_free);
  g_auto(GStrv) types = g_new0(gchar*, relays->len + 1);

  for (guint i = 0; i < relays->len; i++) {
    GnostrNip65Relay *relay = g_ptr_array_index(relays, i);
    g_ptr_array_add(urls, g_strdup(relay->url));
    types[i] = g_strdup(relay_type_to_string(relay->type));
  }

  /* Save URLs using existing function */
  gnostr_save_relays_from(urls);
  g_ptr_array_unref(urls);

  /* Save types to GSettings */
  GSettingsSchemaSource *src = g_settings_schema_source_get_default();
  if (src) {
    GSettingsSchema *schema = g_settings_schema_source_lookup(src, "org.gnostr.gnostr", TRUE);
    if (schema) {
      GSettings *settings = g_settings_new("org.gnostr.gnostr");
      g_settings_set_strv(settings, "relay-types", (const gchar * const *)types);
      g_settings_schema_unref(schema);
      g_object_unref(settings);
      g_debug("relays: saved %u relay types to GSettings", relays->len);
    }
  }

  /* Emit relay change notification for live switching */
  gnostr_relay_change_emit();
}

GPtrArray *gnostr_get_read_relay_urls(void) {
  GPtrArray *result = g_ptr_array_new_with_free_func(g_free);
  GPtrArray *nip65 = gnostr_load_nip65_relays();

  for (guint i = 0; i < nip65->len; i++) {
    GnostrNip65Relay *relay = g_ptr_array_index(nip65, i);
    /* Read-capable: read-only or read+write */
    if (relay->type == GNOSTR_RELAY_READ || relay->type == GNOSTR_RELAY_READWRITE) {
      g_ptr_array_add(result, g_strdup(relay->url));
    }
  }

  g_ptr_array_unref(nip65);

  /* Fallback: if no read relays, use all relays */
  if (result->len == 0) {
    gnostr_load_relays_into(result);
  }

  return result;
}

GPtrArray *gnostr_get_write_relay_urls(void) {
  GPtrArray *result = g_ptr_array_new_with_free_func(g_free);
  GPtrArray *nip65 = gnostr_load_nip65_relays();

  for (guint i = 0; i < nip65->len; i++) {
    GnostrNip65Relay *relay = g_ptr_array_index(nip65, i);
    /* Write-capable: write-only or read+write */
    if (relay->type == GNOSTR_RELAY_WRITE || relay->type == GNOSTR_RELAY_READWRITE) {
      g_ptr_array_add(result, g_strdup(relay->url));
    }
  }

  g_ptr_array_unref(nip65);

  /* Fallback: if no write relays, use all relays */
  if (result->len == 0) {
    gnostr_load_relays_into(result);
  }

  return result;
}

void gnostr_get_read_relay_urls_into(GPtrArray *out) {
  if (!out) return;
  GPtrArray *temp = gnostr_get_read_relay_urls();
  for (guint i = 0; i < temp->len; i++) {
    g_ptr_array_add(out, g_strdup(g_ptr_array_index(temp, i)));
  }
  g_ptr_array_unref(temp);
}

void gnostr_get_write_relay_urls_into(GPtrArray *out) {
  if (!out) return;
  GPtrArray *temp = gnostr_get_write_relay_urls();
  for (guint i = 0; i < temp->len; i++) {
    g_ptr_array_add(out, g_strdup(g_ptr_array_index(temp, i)));
  }
  g_ptr_array_unref(temp);
}

/* --- NIP-65 Publishing Implementation --- */

#ifndef GNOSTR_RELAY_TEST_ONLY
#include "../ipc/gnostr-signer-service.h"
#include "nostr-relay.h"
#include <time.h>

gchar *gnostr_nip65_build_event_json(GPtrArray *nip65_relays) {
  JsonBuilder *builder = json_builder_new();

  json_builder_begin_object(builder);

  /* kind 10002 = NIP-65 Relay List Metadata */
  json_builder_set_member_name(builder, "kind");
  json_builder_add_int_value(builder, 10002);

  /* created_at */
  json_builder_set_member_name(builder, "created_at");
  json_builder_add_int_value(builder, (gint64)time(NULL));

  /* content is always empty for NIP-65 */
  json_builder_set_member_name(builder, "content");
  json_builder_add_string_value(builder, "");

  /* tags - array of "r" tags */
  json_builder_set_member_name(builder, "tags");
  json_builder_begin_array(builder);

  if (nip65_relays) {
    for (guint i = 0; i < nip65_relays->len; i++) {
      GnostrNip65Relay *relay = g_ptr_array_index(nip65_relays, i);
      if (!relay || !relay->url) continue;

      json_builder_begin_array(builder);
      json_builder_add_string_value(builder, "r");
      json_builder_add_string_value(builder, relay->url);

      /* Add marker for read-only or write-only relays */
      if (relay->type == GNOSTR_RELAY_READ) {
        json_builder_add_string_value(builder, "read");
      } else if (relay->type == GNOSTR_RELAY_WRITE) {
        json_builder_add_string_value(builder, "write");
      }
      /* No marker for READWRITE (default) */

      json_builder_end_array(builder);
    }
  }

  json_builder_end_array(builder);
  json_builder_end_object(builder);

  JsonNode *root = json_builder_get_root(builder);
  JsonGenerator *gen = json_generator_new();
  json_generator_set_root(gen, root);
  gchar *json_str = json_generator_to_data(gen, NULL);

  json_node_unref(root);
  g_object_unref(gen);
  g_object_unref(builder);

  return json_str;
}

/* Context for async NIP-65 publish operation */
typedef struct {
  GnostrNip65PublishCallback callback;
  gpointer user_data;
  gchar *event_json;
} Nip65PublishCtx;

static void nip65_publish_ctx_free(Nip65PublishCtx *ctx) {
  if (!ctx) return;
  g_free(ctx->event_json);
  g_free(ctx);
}

static void on_nip65_sign_complete(GObject *source, GAsyncResult *res, gpointer user_data) {
  Nip65PublishCtx *ctx = (Nip65PublishCtx*)user_data;
  (void)source;
  if (!ctx) return;

  GError *error = NULL;
  gchar *signed_event_json = NULL;

  gboolean ok = gnostr_sign_event_finish(res, &signed_event_json, &error);

  if (!ok || !signed_event_json) {
    g_warning("nip65: signing failed: %s", error ? error->message : "unknown error");
    if (ctx->callback) {
      ctx->callback(FALSE, error ? error->message : "Signing failed", ctx->user_data);
    }
    g_clear_error(&error);
    nip65_publish_ctx_free(ctx);
    return;
  }

  g_debug("nip65: signed event successfully");

  /* Parse the signed event JSON into a NostrEvent */
  NostrEvent *event = nostr_event_new();
  int parse_rc = nostr_event_deserialize_compact(event, signed_event_json);
  if (parse_rc != 1) {
    g_warning("nip65: failed to parse signed event");
    if (ctx->callback) {
      ctx->callback(FALSE, "Failed to parse signed event", ctx->user_data);
    }
    nostr_event_free(event);
    g_free(signed_event_json);
    nip65_publish_ctx_free(ctx);
    return;
  }

  /* Get relay URLs from config - defaults in GSettings schema */
  GPtrArray *relay_urls = g_ptr_array_new_with_free_func(g_free);
  gnostr_load_relays_into(relay_urls);

  /* Extract event properties for NIP-11 validation */
  const char *nip65_content = nostr_event_get_content(event);
  gint nip65_content_len = nip65_content ? (gint)strlen(nip65_content) : 0;
  NostrTags *nip65_tags = nostr_event_get_tags(event);
  gint nip65_tag_count = nip65_tags ? (gint)nostr_tags_size(nip65_tags) : 0;
  gint64 nip65_created_at = nostr_event_get_created_at(event);
  gssize nip65_serialized_len = signed_event_json ? (gssize)strlen(signed_event_json) : -1;

  /* Publish to each relay */
  guint success_count = 0;
  guint fail_count = 0;
  for (guint i = 0; i < relay_urls->len; i++) {
    const gchar *url = (const gchar*)g_ptr_array_index(relay_urls, i);

    /* NIP-11: Check relay limitations before publishing */
    GnostrRelayInfo *relay_info = gnostr_relay_info_cache_get(url);
    if (relay_info) {
      GnostrRelayValidationResult *validation = gnostr_relay_info_validate_event(
        relay_info, nip65_content, nip65_content_len, nip65_tag_count, nip65_created_at, nip65_serialized_len);

      if (!gnostr_relay_validation_result_is_valid(validation)) {
        gchar *errors = gnostr_relay_validation_result_format_errors(validation);
        g_debug("nip65: skipping %s due to limit violations: %s", url, errors ? errors : "unknown");
        g_free(errors);
        gnostr_relay_validation_result_free(validation);
        gnostr_relay_info_free(relay_info);
        fail_count++;
        continue;
      }
      gnostr_relay_validation_result_free(validation);
      gnostr_relay_info_free(relay_info);
    }

    GNostrRelay *relay = gnostr_relay_new(url);
    if (!relay) {
      fail_count++;
      continue;
    }

    GError *conn_err = NULL;
    if (!gnostr_relay_connect(relay, &conn_err)) {
      g_debug("nip65: failed to connect to %s: %s", url, conn_err ? conn_err->message : "unknown");
      g_clear_error(&conn_err);
      g_object_unref(relay);
      fail_count++;
      continue;
    }

    GError *pub_err = NULL;
    if (gnostr_relay_publish(relay, event, &pub_err)) {
      g_debug("nip65: published to %s", url);
      success_count++;
    } else {
      g_debug("nip65: publish failed to %s: %s", url, pub_err ? pub_err->message : "unknown");
      g_clear_error(&pub_err);
      fail_count++;
    }
    g_object_unref(relay);
  }

  /* Cleanup */
  nostr_event_free(event);
  g_free(signed_event_json);
  g_ptr_array_free(relay_urls, TRUE);

  /* Notify callback */
  if (ctx->callback) {
    if (success_count > 0) {
      ctx->callback(TRUE, NULL, ctx->user_data);
    } else {
      ctx->callback(FALSE, "Failed to publish to any relay", ctx->user_data);
    }
  }

  g_debug("nip65: published to %u relays, failed %u", success_count, fail_count);
  nip65_publish_ctx_free(ctx);
}

void gnostr_nip65_publish_async(GPtrArray *nip65_relays,
                                 GnostrNip65PublishCallback callback,
                                 gpointer user_data) {
  /* Check if signer service is available */
  GnostrSignerService *signer = gnostr_signer_service_get_default();
  if (!gnostr_signer_service_is_available(signer)) {
    if (callback) callback(FALSE, "Signer not available", user_data);
    return;
  }

  /* Build unsigned event JSON */
  gchar *event_json = gnostr_nip65_build_event_json(nip65_relays);
  if (!event_json) {
    if (callback) callback(FALSE, "Failed to build event JSON", user_data);
    return;
  }

  g_debug("nip65: requesting signature for relay list event");

  /* Create publish context */
  Nip65PublishCtx *ctx = g_new0(Nip65PublishCtx, 1);
  ctx->callback = callback;
  ctx->user_data = user_data;
  ctx->event_json = event_json;

  /* Call unified signer service (uses NIP-46 or NIP-55L based on login method) */
  gnostr_sign_event_async(
    event_json,
    "",        /* current_user: ignored */
    "gnostr",  /* app_id: ignored */
    NULL,      /* cancellable */
    on_nip65_sign_complete,
    ctx
  );
}

/* Context for async NIP-65 load on login */
typedef struct {
  gchar *pubkey_hex;
  GnostrNip65LoadCallback callback;
  gpointer user_data;
} Nip65LoadLoginCtx;

static void nip65_load_login_ctx_free(Nip65LoadLoginCtx *ctx) {
  if (!ctx) return;
  g_free(ctx->pubkey_hex);
  g_free(ctx);
}

static void on_nip65_load_login_done(GPtrArray *relays, gpointer user_data) {
  Nip65LoadLoginCtx *ctx = (Nip65LoadLoginCtx*)user_data;
  if (!ctx) {
    if (relays) g_ptr_array_unref(relays);
    return;
  }

  if (relays && relays->len > 0) {
    g_debug("nip65: loaded %u relays from network for user %.*s...",
            relays->len, 8, ctx->pubkey_hex ? ctx->pubkey_hex : "");

    /* Apply to local config */
    gnostr_nip65_apply_to_local_config(relays);
  } else {
    g_debug("nip65: no relay list found on network for user %.*s...",
            8, ctx->pubkey_hex ? ctx->pubkey_hex : "");
  }

  /* Call user callback with the relay list */
  if (ctx->callback) {
    ctx->callback(relays, ctx->user_data);
  } else if (relays) {
    g_ptr_array_unref(relays);
  }

  nip65_load_login_ctx_free(ctx);
}

void gnostr_nip65_load_on_login_async(const gchar *pubkey_hex,
                                       GnostrNip65LoadCallback callback,
                                       gpointer user_data) {
  if (!pubkey_hex || !*pubkey_hex) {
    if (callback) callback(NULL, user_data);
    return;
  }

  Nip65LoadLoginCtx *ctx = g_new0(Nip65LoadLoginCtx, 1);
  ctx->pubkey_hex = g_strdup(pubkey_hex);
  ctx->callback = callback;
  ctx->user_data = user_data;

  /* Use the existing fetch function */
  gnostr_nip65_fetch_relays_async(pubkey_hex, NULL, on_nip65_load_login_done, ctx);
}

GPtrArray *gnostr_nip65_from_local_config(void) {
  /* Use the existing function that loads NIP-65 relays with types */
  return gnostr_load_nip65_relays();
}

void gnostr_nip65_apply_to_local_config(GPtrArray *nip65_relays) {
  if (!nip65_relays || nip65_relays->len == 0) return;

  /* Save using the existing function that preserves types */
  gnostr_save_nip65_relays(nip65_relays);

  g_debug("nip65: applied %u relays to local config", nip65_relays->len);

  /* Emit relay change notification for live switching */
  gnostr_relay_change_emit();
}

#endif /* GNOSTR_RELAY_TEST_ONLY */

/* --- Live Relay Switching Implementation (nostrc-36y.4) --- */

/* Singleton GSettings instance for relay configuration */
static GSettings *s_relay_settings = NULL;

/* Array of registered callbacks for relay changes */
typedef struct {
  gulong id;
  GnostrRelayChangeCallback callback;
  gpointer user_data;
} RelayChangeHandler;

static GPtrArray *s_relay_change_handlers = NULL;
static gulong s_next_handler_id = 1;
static gulong s_gsettings_handler_id = 0;

/* Internal callback from GSettings "changed" signal */
static void on_gsettings_relays_changed(GSettings *settings, const gchar *key, gpointer user_data) {
  (void)settings; (void)user_data;
  /* Only respond to relay-related keys */
  if (g_strcmp0(key, "relays") != 0 &&
      g_strcmp0(key, "relay-types") != 0 &&
      g_strcmp0(key, "dm-relays") != 0) {
    return;
  }
  g_debug("[RELAYS] GSettings '%s' changed, notifying %u handlers",
          key, s_relay_change_handlers ? s_relay_change_handlers->len : 0);
  gnostr_relay_change_emit();
}

GSettings *gnostr_relay_get_settings(void) {
  if (s_relay_settings) return s_relay_settings;

  GSettingsSchemaSource *src = g_settings_schema_source_get_default();
  if (!src) return NULL;

  GSettingsSchema *schema = g_settings_schema_source_lookup(src, "org.gnostr.gnostr", TRUE);
  if (!schema) return NULL;

  s_relay_settings = g_settings_new("org.gnostr.gnostr");
  g_settings_schema_unref(schema);

  /* Connect to changes once */
  if (s_relay_settings && s_gsettings_handler_id == 0) {
    s_gsettings_handler_id = g_signal_connect(s_relay_settings, "changed",
                                               G_CALLBACK(on_gsettings_relays_changed), NULL);
  }

  return s_relay_settings;
}

gulong gnostr_relay_change_connect(GnostrRelayChangeCallback callback, gpointer user_data) {
  if (!callback) return 0;

  /* Ensure settings singleton is initialized (connects to GSettings changes) */
  gnostr_relay_get_settings();

  if (!s_relay_change_handlers) {
    s_relay_change_handlers = g_ptr_array_new_with_free_func(g_free);
  }

  RelayChangeHandler *h = g_new0(RelayChangeHandler, 1);
  h->id = s_next_handler_id++;
  h->callback = callback;
  h->user_data = user_data;
  g_ptr_array_add(s_relay_change_handlers, h);

  g_debug("[RELAYS] Registered relay change handler id=%lu (total=%u)",
          h->id, s_relay_change_handlers->len);
  return h->id;
}

void gnostr_relay_change_disconnect(gulong handler_id) {
  if (!s_relay_change_handlers || handler_id == 0) return;

  for (guint i = 0; i < s_relay_change_handlers->len; i++) {
    RelayChangeHandler *h = g_ptr_array_index(s_relay_change_handlers, i);
    if (h && h->id == handler_id) {
      g_debug("[RELAYS] Disconnected relay change handler id=%lu", handler_id);
      g_ptr_array_remove_index(s_relay_change_handlers, i);
      return;
    }
  }
}

void gnostr_relay_change_emit(void) {
  if (!s_relay_change_handlers) return;

  g_debug("[RELAYS] Emitting relay change notification to %u handlers",
          s_relay_change_handlers->len);

  /* Copy handlers to avoid issues if callbacks modify the list */
  GPtrArray *handlers_copy = g_ptr_array_new();
  for (guint i = 0; i < s_relay_change_handlers->len; i++) {
    RelayChangeHandler *h = g_ptr_array_index(s_relay_change_handlers, i);
    if (h) g_ptr_array_add(handlers_copy, h);
  }

  /* Invoke all callbacks */
  for (guint i = 0; i < handlers_copy->len; i++) {
    RelayChangeHandler *h = g_ptr_array_index(handlers_copy, i);
    if (h && h->callback) {
      h->callback(h->user_data);
    }
  }

  g_ptr_array_free(handlers_copy, TRUE);
}
