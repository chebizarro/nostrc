/**
 * gnostr Blossom Settings Implementation
 *
 * Manages user Blossom server preferences using GSettings
 * and synchronizes with kind 10063 events.
 */

#include "blossom_settings.h"
#include <string.h>
#include <json-glib/json-glib.h>
#include <nostr-kinds.h>

/* GSettings schema IDs */
#define BLOSSOM_SCHEMA_ID "org.gnostr.Client"

/* Singleton instance */
static GSettings *blossom_gsettings = NULL;

/* Cached server list from GSettings */
static GPtrArray *cached_servers = NULL;

/* Ensure GSettings is initialized */
static void ensure_gsettings(void) {
  if (!blossom_gsettings) {
    blossom_gsettings = g_settings_new(BLOSSOM_SCHEMA_ID);
  }
}

void gnostr_blossom_server_free(GnostrBlossomServer *server) {
  if (!server) return;
  g_free(server->url);
  g_free(server);
}

GObject *gnostr_blossom_settings_get_default(void) {
  ensure_gsettings();
  return G_OBJECT(blossom_gsettings);
}

const char *gnostr_blossom_settings_get_default_server(void) {
  ensure_gsettings();

  /* Check GSettings first */
  g_autofree char *url = g_settings_get_string(blossom_gsettings, "blossom-server");
  if (url && *url) {
    /* Return from gsettings - we need to keep a static copy */
    static char *cached_default = NULL;
    g_free(cached_default);
    cached_default = g_strdup(url);
    return cached_default;
  }

  /* Fall back to default */
  return GNOSTR_BLOSSOM_DEFAULT_SERVER;
}

void gnostr_blossom_settings_set_default_server(const char *url) {
  ensure_gsettings();
  g_settings_set_string(blossom_gsettings, "blossom-server", url ? url : "");
}

static void load_servers_from_gsettings(void) {
  ensure_gsettings();

  if (cached_servers) {
    g_ptr_array_unref(cached_servers);
  }
  cached_servers = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_blossom_server_free);

  g_auto(GStrv) servers = g_settings_get_strv(blossom_gsettings, "blossom-servers");
  if (servers) {
    for (int i = 0; servers[i] != NULL; i++) {
      if (servers[i][0] == '\0') continue;

      GnostrBlossomServer *server = g_new0(GnostrBlossomServer, 1);
      server->url = g_strdup(servers[i]);
      server->enabled = TRUE;
      g_ptr_array_add(cached_servers, server);
    }
  }

  /* If no servers configured, add the default */
  if (cached_servers->len == 0) {
    GnostrBlossomServer *server = g_new0(GnostrBlossomServer, 1);
    server->url = g_strdup(GNOSTR_BLOSSOM_DEFAULT_SERVER);
    server->enabled = TRUE;
    g_ptr_array_add(cached_servers, server);
  }
}

static void save_servers_to_gsettings(void) {
  ensure_gsettings();

  if (!cached_servers) {
    load_servers_from_gsettings();
    return;
  }

  GPtrArray *urls = g_ptr_array_new();
  for (guint i = 0; i < cached_servers->len; i++) {
    GnostrBlossomServer *server = g_ptr_array_index(cached_servers, i);
    if (server->enabled) {
      g_ptr_array_add(urls, server->url);
    }
  }
  g_ptr_array_add(urls, NULL);

  g_settings_set_strv(blossom_gsettings, "blossom-servers", (const char * const *)urls->pdata);
  g_ptr_array_free(urls, TRUE);
}

GnostrBlossomServer **gnostr_blossom_settings_get_servers(gsize *out_count) {
  if (!cached_servers) {
    load_servers_from_gsettings();
  }

  if (out_count) {
    *out_count = cached_servers->len;
  }

  /* Create a copy of the array for the caller */
  GnostrBlossomServer **result = g_new0(GnostrBlossomServer *, cached_servers->len + 1);
  for (guint i = 0; i < cached_servers->len; i++) {
    GnostrBlossomServer *src = g_ptr_array_index(cached_servers, i);
    GnostrBlossomServer *dst = g_new0(GnostrBlossomServer, 1);
    dst->url = g_strdup(src->url);
    dst->enabled = src->enabled;
    result[i] = dst;
  }
  result[cached_servers->len] = NULL;

  return result;
}

void gnostr_blossom_servers_free(GnostrBlossomServer **servers, gsize count) {
  if (!servers) return;
  for (gsize i = 0; i < count; i++) {
    gnostr_blossom_server_free(servers[i]);
  }
  g_free(servers);
}

gboolean gnostr_blossom_settings_add_server(const char *url) {
  if (!url || !*url) return FALSE;

  if (!cached_servers) {
    load_servers_from_gsettings();
  }

  /* Check if already exists */
  for (guint i = 0; i < cached_servers->len; i++) {
    GnostrBlossomServer *server = g_ptr_array_index(cached_servers, i);
    if (g_ascii_strcasecmp(server->url, url) == 0) {
      return FALSE; /* Already exists */
    }
  }

  /* Add new server */
  GnostrBlossomServer *server = g_new0(GnostrBlossomServer, 1);
  server->url = g_strdup(url);
  server->enabled = TRUE;
  g_ptr_array_add(cached_servers, server);

  save_servers_to_gsettings();
  return TRUE;
}

gboolean gnostr_blossom_settings_remove_server(const char *url) {
  if (!url || !*url) return FALSE;

  if (!cached_servers) {
    load_servers_from_gsettings();
  }

  for (guint i = 0; i < cached_servers->len; i++) {
    GnostrBlossomServer *server = g_ptr_array_index(cached_servers, i);
    if (g_ascii_strcasecmp(server->url, url) == 0) {
      g_ptr_array_remove_index(cached_servers, i);
      save_servers_to_gsettings();
      return TRUE;
    }
  }

  return FALSE;
}

gboolean gnostr_blossom_settings_from_event(const char *event_json) {
  if (!event_json) return FALSE;

  JsonParser *parser = json_parser_new();
  GError *error = NULL;

  if (!json_parser_load_from_data(parser, event_json, -1, &error)) {
    g_warning("Failed to parse kind 10063 event: %s", error->message);
    g_error_free(error);
    g_object_unref(parser);
    return FALSE;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
    g_object_unref(parser);
    return FALSE;
  }

  JsonObject *obj = json_node_get_object(root);

  /* Verify kind */
  if (!json_object_has_member(obj, "kind")) {
    g_object_unref(parser);
    return FALSE;
  }

  gint64 kind = json_object_get_int_member(obj, "kind");
  if (kind != NOSTR_KIND_USER_SERVER_LIST) {
    g_warning("Event is not kind 10063, got kind %" G_GINT64_FORMAT, kind);
    g_object_unref(parser);
    return FALSE;
  }

  /* Parse tags for server entries */
  if (!json_object_has_member(obj, "tags")) {
    g_object_unref(parser);
    return FALSE;
  }

  JsonArray *tags = json_object_get_array_member(obj, "tags");
  if (!tags) {
    g_object_unref(parser);
    return FALSE;
  }

  /* Clear existing cache and rebuild from event */
  if (cached_servers) {
    g_ptr_array_unref(cached_servers);
  }
  cached_servers = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_blossom_server_free);

  guint tags_len = json_array_get_length(tags);
  for (guint i = 0; i < tags_len; i++) {
    JsonArray *tag = json_array_get_array_element(tags, i);
    if (!tag || json_array_get_length(tag) < 2) continue;

    const char *tag_name = json_array_get_string_element(tag, 0);
    if (!tag_name || g_ascii_strcasecmp(tag_name, "server") != 0) continue;

    const char *server_url = json_array_get_string_element(tag, 1);
    if (!server_url || !*server_url) continue;

    GnostrBlossomServer *server = g_new0(GnostrBlossomServer, 1);
    server->url = g_strdup(server_url);
    server->enabled = TRUE;
    g_ptr_array_add(cached_servers, server);
  }

  g_object_unref(parser);

  /* Update default server to first one if list changed */
  if (cached_servers->len > 0) {
    GnostrBlossomServer *first = g_ptr_array_index(cached_servers, 0);
    gnostr_blossom_settings_set_default_server(first->url);
  }

  save_servers_to_gsettings();
  return TRUE;
}

char *gnostr_blossom_settings_to_event(void) {
  if (!cached_servers) {
    load_servers_from_gsettings();
  }

  JsonBuilder *builder = json_builder_new();

  json_builder_begin_object(builder);

  /* Kind 10063 */
  json_builder_set_member_name(builder, "kind");
  json_builder_add_int_value(builder, NOSTR_KIND_USER_SERVER_LIST);

  /* Created at */
  json_builder_set_member_name(builder, "created_at");
  json_builder_add_int_value(builder, (gint64)g_get_real_time() / G_USEC_PER_SEC);

  /* Content is empty */
  json_builder_set_member_name(builder, "content");
  json_builder_add_string_value(builder, "");

  /* Tags array with server entries */
  json_builder_set_member_name(builder, "tags");
  json_builder_begin_array(builder);

  for (guint i = 0; i < cached_servers->len; i++) {
    GnostrBlossomServer *server = g_ptr_array_index(cached_servers, i);
    if (!server->enabled) continue;

    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "server");
    json_builder_add_string_value(builder, server->url);
    json_builder_end_array(builder);
  }

  json_builder_end_array(builder);

  json_builder_end_object(builder);

  JsonGenerator *gen = json_generator_new();
  json_generator_set_root(gen, json_builder_get_root(builder));
  char *json_str = json_generator_to_data(gen, NULL);

  g_object_unref(gen);
  g_object_unref(builder);

  return json_str;
}

/* Async load/publish stubs - these would integrate with SimplePool for relay queries */
void gnostr_blossom_settings_load_from_relays_async(const char *pubkey_hex,
                                                      GnostrBlossomSettingsLoadCallback callback,
                                                      gpointer user_data) {
  /* TODO: Implement relay query for kind 10063 */
  /* For now, just load from local GSettings */
  (void)pubkey_hex;

  load_servers_from_gsettings();

  if (callback) {
    callback(TRUE, NULL, user_data);
  }
}

void gnostr_blossom_settings_publish_async(GnostrBlossomSettingsPublishCallback callback,
                                             gpointer user_data) {
  /* TODO: Implement publishing to relays */
  /* This requires signer IPC to sign the event first */

  if (callback) {
    GError *err = g_error_new_literal(g_quark_from_static_string("blossom-settings"),
                                       1, "Publishing not yet implemented");
    callback(FALSE, err, user_data);
    g_error_free(err);
  }
}
