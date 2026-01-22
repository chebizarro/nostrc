/* relay_store.c - Relay configuration implementation */
#include "relay_store.h"
#include <json.h>
#include <string.h>
#include <time.h>

struct _RelayStore {
  GPtrArray *relays;    /* Array of RelayEntry* */
  gchar *config_path;
};

static const gchar *config_path(void) {
  static gchar *p = NULL;
  if (!p) {
    const gchar *conf = g_get_user_config_dir();
    gchar *dir = g_build_filename(conf, "gnostr-signer", NULL);
    g_mkdir_with_parents(dir, 0700);
    p = g_build_filename(dir, "relays.json", NULL);
    g_free(dir);
  }
  return p;
}

void relay_entry_free(RelayEntry *entry) {
  if (!entry) return;
  g_free(entry->url);
  g_free(entry);
}

RelayStore *relay_store_new(void) {
  RelayStore *rs = g_new0(RelayStore, 1);
  rs->relays = g_ptr_array_new_with_free_func((GDestroyNotify)relay_entry_free);
  rs->config_path = g_strdup(config_path());
  return rs;
}

void relay_store_free(RelayStore *rs) {
  if (!rs) return;
  g_ptr_array_unref(rs->relays);
  g_free(rs->config_path);
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

  json_object *root = json_tokener_parse(contents);
  g_free(contents);

  if (!root || !json_object_is_type(root, json_type_array)) {
    if (root) json_object_put(root);
    return;
  }

  g_ptr_array_set_size(rs->relays, 0);

  int n = json_object_array_length(root);
  for (int i = 0; i < n; i++) {
    json_object *item = json_object_array_get_idx(root, i);
    if (!item) continue;

    json_object *url_obj, *read_obj, *write_obj;

    if (!json_object_object_get_ex(item, "url", &url_obj)) continue;

    RelayEntry *entry = g_new0(RelayEntry, 1);
    entry->url = g_strdup(json_object_get_string(url_obj));

    if (json_object_object_get_ex(item, "read", &read_obj)) {
      entry->read = json_object_get_boolean(read_obj);
    } else {
      entry->read = TRUE;
    }

    if (json_object_object_get_ex(item, "write", &write_obj)) {
      entry->write = json_object_get_boolean(write_obj);
    } else {
      entry->write = TRUE;
    }

    g_ptr_array_add(rs->relays, entry);
  }

  json_object_put(root);
}

void relay_store_save(RelayStore *rs) {
  if (!rs) return;

  json_object *root = json_object_new_array();

  for (guint i = 0; i < rs->relays->len; i++) {
    RelayEntry *entry = g_ptr_array_index(rs->relays, i);

    json_object *item = json_object_new_object();
    json_object_object_add(item, "url", json_object_new_string(entry->url));
    json_object_object_add(item, "read", json_object_new_boolean(entry->read));
    json_object_object_add(item, "write", json_object_new_boolean(entry->write));

    json_object_array_add(root, item);
  }

  const gchar *json_str = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PRETTY);

  GError *err = NULL;
  if (!g_file_set_contents(rs->config_path, json_str, -1, &err)) {
    if (err) {
      g_warning("relay_store_save: %s", err->message);
      g_clear_error(&err);
    }
  }

  json_object_put(root);
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

  /* Build tags array per NIP-65 */
  json_object *tags = json_object_new_array();

  for (guint i = 0; i < rs->relays->len; i++) {
    RelayEntry *entry = g_ptr_array_index(rs->relays, i);

    json_object *tag = json_object_new_array();
    json_object_array_add(tag, json_object_new_string("r"));
    json_object_array_add(tag, json_object_new_string(entry->url));

    /* Add marker for read-only or write-only */
    if (entry->read && !entry->write) {
      json_object_array_add(tag, json_object_new_string("read"));
    } else if (!entry->read && entry->write) {
      json_object_array_add(tag, json_object_new_string("write"));
    }
    /* If both read and write, no marker needed */

    json_object_array_add(tags, tag);
  }

  /* Build event */
  json_object *event = json_object_new_object();
  json_object_object_add(event, "kind", json_object_new_int(10002));
  json_object_object_add(event, "created_at", json_object_new_int64((int64_t)time(NULL)));
  json_object_object_add(event, "tags", tags);
  json_object_object_add(event, "content", json_object_new_string(""));

  gchar *result = g_strdup(json_object_to_json_string(event));
  json_object_put(event);

  return result;
}

gboolean relay_store_parse_event(RelayStore *rs, const gchar *event_json) {
  if (!rs || !event_json) return FALSE;

  json_object *root = json_tokener_parse(event_json);
  if (!root) return FALSE;

  json_object *kind_obj;
  if (!json_object_object_get_ex(root, "kind", &kind_obj) ||
      json_object_get_int(kind_obj) != 10002) {
    json_object_put(root);
    return FALSE;
  }

  json_object *tags_obj;
  if (!json_object_object_get_ex(root, "tags", &tags_obj) ||
      !json_object_is_type(tags_obj, json_type_array)) {
    json_object_put(root);
    return FALSE;
  }

  /* Clear existing and parse tags */
  g_ptr_array_set_size(rs->relays, 0);

  int n = json_object_array_length(tags_obj);
  for (int i = 0; i < n; i++) {
    json_object *tag = json_object_array_get_idx(tags_obj, i);
    if (!tag || !json_object_is_type(tag, json_type_array)) continue;

    int tag_len = json_object_array_length(tag);
    if (tag_len < 2) continue;

    json_object *tag_type = json_object_array_get_idx(tag, 0);
    if (!tag_type || g_strcmp0(json_object_get_string(tag_type), "r") != 0) continue;

    json_object *url_obj = json_object_array_get_idx(tag, 1);
    if (!url_obj) continue;

    const gchar *url = json_object_get_string(url_obj);
    if (!url || !*url) continue;

    gboolean read = TRUE, write = TRUE;

    /* Check for marker */
    if (tag_len >= 3) {
      json_object *marker_obj = json_object_array_get_idx(tag, 2);
      if (marker_obj) {
        const gchar *marker = json_object_get_string(marker_obj);
        if (g_strcmp0(marker, "read") == 0) {
          write = FALSE;
        } else if (g_strcmp0(marker, "write") == 0) {
          read = FALSE;
        }
      }
    }

    relay_store_add(rs, url, read, write);
  }

  json_object_put(root);
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
