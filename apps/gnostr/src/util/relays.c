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

/* --- NIP-65 Relay List Metadata --- */

#include <json-glib/json-glib.h>
#include "nostr_simple_pool.h"
#include "nostr-filter.h"
#include "nostr-event.h"

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

/* Async NIP-65 fetch context */
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

  if (relay_arr->len == 0) {
    /* Add default relays */
    g_ptr_array_add(relay_arr, g_strdup("wss://relay.damus.io"));
    g_ptr_array_add(relay_arr, g_strdup("wss://nos.lol"));
    g_ptr_array_add(relay_arr, g_strdup("wss://purplepag.es"));
  }

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
