/* relay_store.c - Relay configuration implementation */
#include "relay_store.h"
#include <json-glib/json-glib.h>
#include <string.h>
#include <time.h>

struct _RelayStore {
  GPtrArray *relays;    /* Array of RelayEntry* */
  gchar *config_path;
  gchar *identity;      /* npub for per-identity store, NULL for global */
  GHashTable *status_map; /* url -> RelayConnectionStatus */
};

/* Get config directory for gnostr-signer */
static gchar *get_config_dir(void) {
  const gchar *conf = g_get_user_config_dir();
  gchar *dir = g_build_filename(conf, "gnostr-signer", NULL);
  g_mkdir_with_parents(dir, 0700);
  return dir;
}

/* Build config path for a specific identity (or global if identity is NULL) */
static gchar *build_config_path(const gchar *identity) {
  gchar *dir = get_config_dir();
  gchar *path;

  if (identity && *identity) {
    /* Per-identity relay config: relays/<npub>.json */
    gchar *relays_dir = g_build_filename(dir, "relays", NULL);
    g_mkdir_with_parents(relays_dir, 0700);
    gchar *filename = g_strdup_printf("%s.json", identity);
    path = g_build_filename(relays_dir, filename, NULL);
    g_free(filename);
    g_free(relays_dir);
  } else {
    /* Global relay config: relays.json */
    path = g_build_filename(dir, "relays.json", NULL);
  }

  g_free(dir);
  return path;
}

static const gchar *config_path(void) {
  static gchar *p = NULL;
  if (!p) {
    p = build_config_path(NULL);
  }
  return p;
}

void relay_entry_free(RelayEntry *entry) {
  if (!entry) return;
  g_free(entry->url);
  g_free(entry);
}

RelayStore *relay_store_new(void) {
  return relay_store_new_for_identity(NULL);
}

RelayStore *relay_store_new_for_identity(const gchar *identity) {
  RelayStore *rs = g_new0(RelayStore, 1);
  rs->relays = g_ptr_array_new_with_free_func((GDestroyNotify)relay_entry_free);
  rs->identity = identity ? g_strdup(identity) : NULL;
  rs->config_path = build_config_path(identity);
  rs->status_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  return rs;
}

void relay_store_free(RelayStore *rs) {
  if (!rs) return;
  g_ptr_array_unref(rs->relays);
  g_free(rs->config_path);
  g_free(rs->identity);
  if (rs->status_map) g_hash_table_destroy(rs->status_map);
  g_free(rs);
}

void relay_store_load(RelayStore *rs) {
  if (!rs) return;

  gchar *contents = NULL;
  gsize len = 0;
  GError *err = NULL;

  if (!g_file_get_contents(rs->config_path, &contents, &len, &err)) {
    if (err) g_clear_error(&err);
    return;
  }

  JsonParser *parser = json_parser_new();
  if (!json_parser_load_from_data(parser, contents, -1, NULL)) {
    g_free(contents);
    g_object_unref(parser);
    return;
  }
  g_free(contents);

  JsonNode *root_node = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_ARRAY(root_node)) {
    g_object_unref(parser);
    return;
  }

  JsonArray *root = json_node_get_array(root_node);
  g_ptr_array_set_size(rs->relays, 0);

  guint n = json_array_get_length(root);
  for (guint i = 0; i < n; i++) {
    JsonNode *item_node = json_array_get_element(root, i);
    if (!JSON_NODE_HOLDS_OBJECT(item_node)) continue;

    JsonObject *item = json_node_get_object(item_node);
    if (!json_object_has_member(item, "url")) continue;

    RelayEntry *entry = g_new0(RelayEntry, 1);
    entry->url = g_strdup(json_object_get_string_member(item, "url"));

    if (json_object_has_member(item, "read")) {
      entry->read = json_object_get_boolean_member(item, "read");
    } else {
      entry->read = TRUE;
    }

    if (json_object_has_member(item, "write")) {
      entry->write = json_object_get_boolean_member(item, "write");
    } else {
      entry->write = TRUE;
    }

    g_ptr_array_add(rs->relays, entry);
  }

  g_object_unref(parser);
}

void relay_store_save(RelayStore *rs) {
  if (!rs) return;

  JsonBuilder *builder = json_builder_new();
  json_builder_begin_array(builder);

  for (guint i = 0; i < rs->relays->len; i++) {
    RelayEntry *entry = g_ptr_array_index(rs->relays, i);

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "url");
    json_builder_add_string_value(builder, entry->url);
    json_builder_set_member_name(builder, "read");
    json_builder_add_boolean_value(builder, entry->read);
    json_builder_set_member_name(builder, "write");
    json_builder_add_boolean_value(builder, entry->write);
    json_builder_end_object(builder);
  }

  json_builder_end_array(builder);

  JsonNode *root = json_builder_get_root(builder);
  JsonGenerator *gen = json_generator_new();
  json_generator_set_pretty(gen, TRUE);
  json_generator_set_root(gen, root);
  gchar *json_str = json_generator_to_data(gen, NULL);

  g_object_unref(gen);
  json_node_unref(root);
  g_object_unref(builder);

  GError *err = NULL;
  if (!g_file_set_contents(rs->config_path, json_str, -1, &err)) {
    if (err) {
      g_warning("relay_store_save: %s", err->message);
      g_clear_error(&err);
    }
  }

  g_free(json_str);
}

static gint find_relay_by_url(RelayStore *rs, const gchar *url) {
  if (!rs || !url) return -1;

  for (guint i = 0; i < rs->relays->len; i++) {
    RelayEntry *entry = g_ptr_array_index(rs->relays, i);
    if (g_strcmp0(entry->url, url) == 0) {
      return (gint)i;
    }
  }
  return -1;
}

gboolean relay_store_add(RelayStore *rs, const gchar *url,
                         gboolean read, gboolean write) {
  if (!rs || !url || !*url) return FALSE;

  /* Check for duplicate */
  if (find_relay_by_url(rs, url) >= 0) return FALSE;

  RelayEntry *entry = g_new0(RelayEntry, 1);
  entry->url = g_strdup(url);
  entry->read = read;
  entry->write = write;

  g_ptr_array_add(rs->relays, entry);
  return TRUE;
}

gboolean relay_store_remove(RelayStore *rs, const gchar *url) {
  if (!rs || !url) return FALSE;

  gint idx = find_relay_by_url(rs, url);
  if (idx < 0) return FALSE;

  g_ptr_array_remove_index(rs->relays, (guint)idx);
  return TRUE;
}

gboolean relay_store_update(RelayStore *rs, const gchar *url,
                            gboolean read, gboolean write) {
  if (!rs || !url) return FALSE;

  gint idx = find_relay_by_url(rs, url);
  if (idx < 0) return FALSE;

  RelayEntry *entry = g_ptr_array_index(rs->relays, (guint)idx);
  entry->read = read;
  entry->write = write;
  return TRUE;
}

GPtrArray *relay_store_list(RelayStore *rs) {
  if (!rs) return NULL;

  GPtrArray *arr = g_ptr_array_new_with_free_func((GDestroyNotify)relay_entry_free);

  for (guint i = 0; i < rs->relays->len; i++) {
    RelayEntry *src = g_ptr_array_index(rs->relays, i);
    RelayEntry *copy = g_new0(RelayEntry, 1);
    copy->url = g_strdup(src->url);
    copy->read = src->read;
    copy->write = src->write;
    g_ptr_array_add(arr, copy);
  }

  return arr;
}

guint relay_store_count(RelayStore *rs) {
  return rs ? rs->relays->len : 0;
}

gchar *relay_store_build_event_json(RelayStore *rs) {
  if (!rs) return NULL;

  JsonBuilder *builder = json_builder_new();
  json_builder_begin_object(builder);

  json_builder_set_member_name(builder, "kind");
  json_builder_add_int_value(builder, 10002);

  json_builder_set_member_name(builder, "created_at");
  json_builder_add_int_value(builder, (gint64)time(NULL));

  /* Build tags array per NIP-65 */
  json_builder_set_member_name(builder, "tags");
  json_builder_begin_array(builder);

  for (guint i = 0; i < rs->relays->len; i++) {
    RelayEntry *entry = g_ptr_array_index(rs->relays, i);

    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "r");
    json_builder_add_string_value(builder, entry->url);

    /* Add marker for read-only or write-only */
    if (entry->read && !entry->write) {
      json_builder_add_string_value(builder, "read");
    } else if (!entry->read && entry->write) {
      json_builder_add_string_value(builder, "write");
    }
    /* If both read and write, no marker needed */

    json_builder_end_array(builder);
  }

  json_builder_end_array(builder);

  json_builder_set_member_name(builder, "content");
  json_builder_add_string_value(builder, "");

  json_builder_end_object(builder);

  JsonNode *root = json_builder_get_root(builder);
  JsonGenerator *gen = json_generator_new();
  json_generator_set_root(gen, root);
  gchar *result = json_generator_to_data(gen, NULL);

  g_object_unref(gen);
  json_node_unref(root);
  g_object_unref(builder);

  return result;
}

gboolean relay_store_parse_event(RelayStore *rs, const gchar *event_json) {
  if (!rs || !event_json) return FALSE;

  JsonParser *parser = json_parser_new();
  if (!json_parser_load_from_data(parser, event_json, -1, NULL)) {
    g_object_unref(parser);
    return FALSE;
  }

  JsonNode *root_node = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_OBJECT(root_node)) {
    g_object_unref(parser);
    return FALSE;
  }

  JsonObject *root = json_node_get_object(root_node);

  if (!json_object_has_member(root, "kind") ||
      json_object_get_int_member(root, "kind") != 10002) {
    g_object_unref(parser);
    return FALSE;
  }

  if (!json_object_has_member(root, "tags")) {
    g_object_unref(parser);
    return FALSE;
  }

  JsonNode *tags_node = json_object_get_member(root, "tags");
  if (!JSON_NODE_HOLDS_ARRAY(tags_node)) {
    g_object_unref(parser);
    return FALSE;
  }

  JsonArray *tags_arr = json_node_get_array(tags_node);

  /* Clear existing and parse tags */
  g_ptr_array_set_size(rs->relays, 0);

  guint n = json_array_get_length(tags_arr);
  for (guint i = 0; i < n; i++) {
    JsonNode *tag_node = json_array_get_element(tags_arr, i);
    if (!JSON_NODE_HOLDS_ARRAY(tag_node)) continue;

    JsonArray *tag = json_node_get_array(tag_node);
    guint tag_len = json_array_get_length(tag);
    if (tag_len < 2) continue;

    const gchar *tag_type = json_array_get_string_element(tag, 0);
    if (g_strcmp0(tag_type, "r") != 0) continue;

    const gchar *url = json_array_get_string_element(tag, 1);
    if (!url || !*url) continue;

    gboolean read = TRUE, write = TRUE;

    /* Check for marker */
    if (tag_len >= 3) {
      const gchar *marker = json_array_get_string_element(tag, 2);
      if (marker) {
        if (g_strcmp0(marker, "read") == 0) {
          write = FALSE;
        } else if (g_strcmp0(marker, "write") == 0) {
          read = FALSE;
        }
      }
    }

    relay_store_add(rs, url, read, write);
  }

  g_object_unref(parser);
  return TRUE;
}

GPtrArray *relay_store_get_defaults(void) {
  GPtrArray *arr = g_ptr_array_new_with_free_func((GDestroyNotify)relay_entry_free);

  /* Default bootstrap relays */
  const gchar *defaults[] = {
    "wss://relay.damus.io",
    "wss://relay.nostr.band",
    "wss://nos.lol",
    "wss://relay.snort.social",
    "wss://nostr.wine",
    NULL
  };

  for (int i = 0; defaults[i]; i++) {
    RelayEntry *entry = g_new0(RelayEntry, 1);
    entry->url = g_strdup(defaults[i]);
    entry->read = TRUE;
    entry->write = TRUE;
    g_ptr_array_add(arr, entry);
  }

  return arr;
}

gboolean relay_store_validate_url(const gchar *url) {
  if (!url || !*url) return FALSE;

  /* Must start with wss:// or ws:// */
  if (!g_str_has_prefix(url, "wss://") && !g_str_has_prefix(url, "ws://")) {
    return FALSE;
  }

  /* Must have a host */
  const gchar *host = url + (g_str_has_prefix(url, "wss://") ? 6 : 5);
  if (!host || !*host || *host == '/') {
    return FALSE;
  }

  return TRUE;
}

GPtrArray *relay_store_get_read_relays(RelayStore *rs) {
  if (!rs) return NULL;

  GPtrArray *arr = g_ptr_array_new_with_free_func(g_free);

  for (guint i = 0; i < rs->relays->len; i++) {
    RelayEntry *entry = g_ptr_array_index(rs->relays, i);
    if (entry->read) {
      g_ptr_array_add(arr, g_strdup(entry->url));
    }
  }

  return arr;
}

GPtrArray *relay_store_get_write_relays(RelayStore *rs) {
  if (!rs) return NULL;

  GPtrArray *arr = g_ptr_array_new_with_free_func(g_free);

  for (guint i = 0; i < rs->relays->len; i++) {
    RelayEntry *entry = g_ptr_array_index(rs->relays, i);
    if (entry->write) {
      g_ptr_array_add(arr, g_strdup(entry->url));
    }
  }

  return arr;
}

RelayConnectionStatus relay_store_get_status(RelayStore *rs, const gchar *url) {
  if (!rs || !url || !rs->status_map) return RELAY_STATUS_UNKNOWN;
  gpointer val = g_hash_table_lookup(rs->status_map, url);
  if (!val) return RELAY_STATUS_UNKNOWN;
  return GPOINTER_TO_INT(val);
}

void relay_store_set_status(RelayStore *rs, const gchar *url, RelayConnectionStatus status) {
  if (!rs || !url || !rs->status_map) return;
  g_hash_table_replace(rs->status_map, g_strdup(url), GINT_TO_POINTER(status));
}

/* Connection test data */
typedef struct {
  gchar *url;
  RelayTestCallback cb;
  gpointer user_data;
} RelayTestData;

static void relay_test_data_free(RelayTestData *data) {
  if (!data) return;
  g_free(data->url);
  g_free(data);
}

static gboolean relay_test_timeout(gpointer user_data) {
  RelayTestData *data = user_data;
  if (data && data->cb) {
    data->cb(data->url, RELAY_STATUS_ERROR, data->user_data);
  }
  relay_test_data_free(data);
  return G_SOURCE_REMOVE;
}

void relay_store_test_connection(const gchar *url, RelayTestCallback cb, gpointer user_data) {
  if (!url || !cb) return;

  /* For now, simulate connection test with a timeout
   * A real implementation would use GSocketClient to test WebSocket connection */
  RelayTestData *data = g_new0(RelayTestData, 1);
  data->url = g_strdup(url);
  data->cb = cb;
  data->user_data = user_data;

  /* If URL looks valid, report connected after brief delay; otherwise error */
  if (relay_store_validate_url(url)) {
    /* Simulate successful connection after 500ms */
    g_timeout_add(500, relay_test_timeout, data);
    /* Update to connecting status immediately via callback */
    cb(url, RELAY_STATUS_CONNECTING, user_data);
  } else {
    /* Invalid URL - report error immediately */
    cb(url, RELAY_STATUS_ERROR, user_data);
    relay_test_data_free(data);
  }
}

const gchar *relay_store_get_identity(RelayStore *rs) {
  if (!rs) return NULL;
  return rs->identity;
}

gboolean relay_store_identity_has_config(const gchar *identity) {
  if (!identity || !*identity) return FALSE;

  gchar *path = build_config_path(identity);
  gboolean exists = g_file_test(path, G_FILE_TEST_EXISTS);
  g_free(path);
  return exists;
}

void relay_store_copy_from(RelayStore *dest, RelayStore *src) {
  if (!dest || !src) return;

  /* Clear destination */
  g_ptr_array_set_size(dest->relays, 0);

  /* Copy all relays from source */
  for (guint i = 0; i < src->relays->len; i++) {
    RelayEntry *entry = g_ptr_array_index(src->relays, i);
    relay_store_add(dest, entry->url, entry->read, entry->write);
  }
}

void relay_store_reset_to_defaults(RelayStore *rs) {
  if (!rs) return;

  /* Clear current relays */
  g_ptr_array_set_size(rs->relays, 0);

  /* Add defaults */
  GPtrArray *defaults = relay_store_get_defaults();
  for (guint i = 0; i < defaults->len; i++) {
    RelayEntry *entry = g_ptr_array_index(defaults, i);
    relay_store_add(rs, entry->url, entry->read, entry->write);
  }
  g_ptr_array_unref(defaults);
}
