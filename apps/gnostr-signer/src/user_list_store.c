/* user_list_store.c - User list management implementation */
#include "user_list_store.h"
#include <json-glib/json-glib.h>
#include <string.h>
#include <time.h>

struct _UserListStore {
  UserListType type;
  GPtrArray *entries;   /* Array of UserListEntry* */
  gchar *config_path;
  gint64 last_sync;     /* Timestamp of last sync with relays */
  gchar *owner_pubkey;  /* Owner's pubkey for signing events */
};

static gchar *get_config_path(UserListType type) {
  const gchar *conf = g_get_user_config_dir();
  gchar *dir = g_build_filename(conf, "gnostr-signer", NULL);
  g_mkdir_with_parents(dir, 0700);

  const gchar *filename = (type == USER_LIST_FOLLOWS) ? "follows.json" : "mutes.json";
  gchar *path = g_build_filename(dir, filename, NULL);
  g_free(dir);
  return path;
}

void user_list_entry_free(UserListEntry *entry) {
  if (!entry) return;
  g_free(entry->pubkey);
  g_free(entry->relay_hint);
  g_free(entry->petname);
  g_free(entry->display_name);
  g_free(entry->avatar_url);
  g_free(entry->nip05);
  g_free(entry);
}

static UserListEntry *user_list_entry_copy(const UserListEntry *entry) {
  if (!entry) return NULL;
  UserListEntry *copy = g_new0(UserListEntry, 1);
  copy->pubkey = g_strdup(entry->pubkey);
  copy->relay_hint = g_strdup(entry->relay_hint);
  copy->petname = g_strdup(entry->petname);
  copy->display_name = g_strdup(entry->display_name);
  copy->avatar_url = g_strdup(entry->avatar_url);
  copy->nip05 = g_strdup(entry->nip05);
  return copy;
}

UserListStore *user_list_store_new(UserListType type) {
  UserListStore *store = g_new0(UserListStore, 1);
  store->type = type;
  store->entries = g_ptr_array_new_with_free_func((GDestroyNotify)user_list_entry_free);
  store->config_path = get_config_path(type);
  return store;
}

void user_list_store_free(UserListStore *store) {
  if (!store) return;
  g_ptr_array_unref(store->entries);
  g_free(store->config_path);
  g_free(store->owner_pubkey);
  g_free(store);
}

void user_list_store_load(UserListStore *store) {
  if (!store) return;

  gchar *contents = NULL;
  gsize len = 0;
  GError *err = NULL;

  if (!g_file_get_contents(store->config_path, &contents, &len, &err)) {
    if (err) g_clear_error(&err);
    return;
  }

  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, contents, -1, NULL)) {
    g_free(contents);
    return;
  }
  g_free(contents);

  JsonNode *root_node = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_ARRAY(root_node)) {
    return;
  }

  JsonArray *root = json_node_get_array(root_node);
  g_ptr_array_set_size(store->entries, 0);

  guint n = json_array_get_length(root);
  for (guint i = 0; i < n; i++) {
    JsonNode *item_node = json_array_get_element(root, i);
    if (!JSON_NODE_HOLDS_OBJECT(item_node)) continue;

    JsonObject *item = json_node_get_object(item_node);
    if (!json_object_has_member(item, "pubkey")) continue;

    UserListEntry *entry = g_new0(UserListEntry, 1);
    entry->pubkey = g_strdup(json_object_get_string_member(item, "pubkey"));

    if (json_object_has_member(item, "relay")) {
      entry->relay_hint = g_strdup(json_object_get_string_member(item, "relay"));
    }
    if (json_object_has_member(item, "petname")) {
      entry->petname = g_strdup(json_object_get_string_member(item, "petname"));
    }

    g_ptr_array_add(store->entries, entry);
  }

}

void user_list_store_save(UserListStore *store) {
  if (!store) return;

  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_array(builder);

  for (guint i = 0; i < store->entries->len; i++) {
    UserListEntry *entry = g_ptr_array_index(store->entries, i);

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "pubkey");
    json_builder_add_string_value(builder, entry->pubkey);

    if (entry->relay_hint && *entry->relay_hint) {
      json_builder_set_member_name(builder, "relay");
      json_builder_add_string_value(builder, entry->relay_hint);
    }
    if (entry->petname && *entry->petname) {
      json_builder_set_member_name(builder, "petname");
      json_builder_add_string_value(builder, entry->petname);
    }

    json_builder_end_object(builder);
  }

  json_builder_end_array(builder);

  JsonNode *root = json_builder_get_root(builder);
  g_autoptr(JsonGenerator) gen = json_generator_new();
  json_generator_set_pretty(gen, TRUE);
  json_generator_set_root(gen, root);
  gchar *json_str = json_generator_to_data(gen, NULL);

  GError *err = NULL;
  if (!g_file_set_contents(store->config_path, json_str, -1, &err)) {
    if (err) {
      g_warning("user_list_store_save: %s", err->message);
      g_clear_error(&err);
    }
  }

  g_free(json_str);
  json_node_unref(root);
}

static gint find_entry_by_pubkey(UserListStore *store, const gchar *pubkey) {
  if (!store || !pubkey) return -1;

  for (guint i = 0; i < store->entries->len; i++) {
    UserListEntry *entry = g_ptr_array_index(store->entries, i);
    if (g_strcmp0(entry->pubkey, pubkey) == 0) {
      return (gint)i;
    }
  }
  return -1;
}

gboolean user_list_store_add(UserListStore *store, const gchar *pubkey,
                             const gchar *relay_hint, const gchar *petname) {
  if (!store || !pubkey || !*pubkey) return FALSE;

  /* Check for duplicate */
  if (find_entry_by_pubkey(store, pubkey) >= 0) return FALSE;

  UserListEntry *entry = g_new0(UserListEntry, 1);
  entry->pubkey = g_strdup(pubkey);
  entry->relay_hint = g_strdup(relay_hint);
  entry->petname = g_strdup(petname);

  g_ptr_array_add(store->entries, entry);
  return TRUE;
}

gboolean user_list_store_remove(UserListStore *store, const gchar *pubkey) {
  if (!store || !pubkey) return FALSE;

  gint idx = find_entry_by_pubkey(store, pubkey);
  if (idx < 0) return FALSE;

  g_ptr_array_remove_index(store->entries, (guint)idx);
  return TRUE;
}

gboolean user_list_store_contains(UserListStore *store, const gchar *pubkey) {
  return find_entry_by_pubkey(store, pubkey) >= 0;
}

gboolean user_list_store_set_petname(UserListStore *store, const gchar *pubkey,
                                     const gchar *petname) {
  if (!store || !pubkey) return FALSE;

  gint idx = find_entry_by_pubkey(store, pubkey);
  if (idx < 0) return FALSE;

  UserListEntry *entry = g_ptr_array_index(store->entries, (guint)idx);
  g_free(entry->petname);
  entry->petname = g_strdup(petname);
  return TRUE;
}

GPtrArray *user_list_store_list(UserListStore *store) {
  if (!store) return NULL;

  GPtrArray *arr = g_ptr_array_new_with_free_func((GDestroyNotify)user_list_entry_free);

  for (guint i = 0; i < store->entries->len; i++) {
    UserListEntry *entry = g_ptr_array_index(store->entries, i);
    g_ptr_array_add(arr, user_list_entry_copy(entry));
  }

  return arr;
}

guint user_list_store_count(UserListStore *store) {
  return store ? store->entries->len : 0;
}

gint user_list_store_get_kind(UserListStore *store) {
  if (!store) return 0;
  return (store->type == USER_LIST_FOLLOWS) ? 3 : 10000;
}

gchar *user_list_store_build_event_json(UserListStore *store) {
  if (!store) return NULL;

  gint kind = user_list_store_get_kind(store);

  /* Build tags array */
  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_object(builder);

  json_builder_set_member_name(builder, "kind");
  json_builder_add_int_value(builder, kind);

  json_builder_set_member_name(builder, "created_at");
  json_builder_add_int_value(builder, (gint64)time(NULL));

  json_builder_set_member_name(builder, "tags");
  json_builder_begin_array(builder);

  for (guint i = 0; i < store->entries->len; i++) {
    UserListEntry *entry = g_ptr_array_index(store->entries, i);

    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "p");
    json_builder_add_string_value(builder, entry->pubkey);

    /* Add relay hint if present */
    if (entry->relay_hint && *entry->relay_hint) {
      json_builder_add_string_value(builder, entry->relay_hint);
    } else {
      json_builder_add_string_value(builder, "");
    }

    /* Add petname if present (for follows) */
    if (store->type == USER_LIST_FOLLOWS && entry->petname && *entry->petname) {
      json_builder_add_string_value(builder, entry->petname);
    }

    json_builder_end_array(builder);
  }

  json_builder_end_array(builder);

  json_builder_set_member_name(builder, "content");
  json_builder_add_string_value(builder, "");

  json_builder_end_object(builder);

  JsonNode *root = json_builder_get_root(builder);
  g_autoptr(JsonGenerator) gen = json_generator_new();
  json_generator_set_root(gen, root);
  gchar *result = json_generator_to_data(gen, NULL);

  json_node_unref(root);

  return result;
}

gboolean user_list_store_parse_event(UserListStore *store, const gchar *event_json) {
  if (!store || !event_json) return FALSE;

  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, event_json, -1, NULL)) {
    return FALSE;
  }

  JsonNode *root_node = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_OBJECT(root_node)) {
    return FALSE;
  }

  JsonObject *root = json_node_get_object(root_node);

  /* Verify kind */
  if (!json_object_has_member(root, "kind")) {
    return FALSE;
  }

  gint kind = (gint)json_object_get_int_member(root, "kind");
  gint expected_kind = user_list_store_get_kind(store);
  if (kind != expected_kind) {
    return FALSE;
  }

  if (!json_object_has_member(root, "tags")) {
    return FALSE;
  }

  JsonNode *tags_node = json_object_get_member(root, "tags");
  if (!JSON_NODE_HOLDS_ARRAY(tags_node)) {
    return FALSE;
  }

  JsonArray *tags = json_node_get_array(tags_node);

  /* Clear and parse tags */
  g_ptr_array_set_size(store->entries, 0);

  guint n = json_array_get_length(tags);
  for (guint i = 0; i < n; i++) {
    JsonNode *tag_node = json_array_get_element(tags, i);
    if (!JSON_NODE_HOLDS_ARRAY(tag_node)) continue;

    JsonArray *tag = json_node_get_array(tag_node);
    guint tag_len = json_array_get_length(tag);
    if (tag_len < 2) continue;

    const gchar *tag_type = json_array_get_string_element(tag, 0);
    if (g_strcmp0(tag_type, "p") != 0) continue;

    const gchar *pubkey = json_array_get_string_element(tag, 1);
    if (!pubkey || !*pubkey) continue;

    gchar *relay_hint = NULL;
    gchar *petname = NULL;

    if (tag_len >= 3) {
      const gchar *r = json_array_get_string_element(tag, 2);
      if (r && *r) relay_hint = g_strdup(r);
    }

    if (tag_len >= 4 && store->type == USER_LIST_FOLLOWS) {
      const gchar *p = json_array_get_string_element(tag, 3);
      if (p && *p) petname = g_strdup(p);
    }

    UserListEntry *entry = g_new0(UserListEntry, 1);
    entry->pubkey = g_strdup(pubkey);
    entry->relay_hint = relay_hint;
    entry->petname = petname;

    g_ptr_array_add(store->entries, entry);
  }

  return TRUE;
}

void user_list_store_clear(UserListStore *store) {
  if (!store) return;
  g_ptr_array_set_size(store->entries, 0);
}

GPtrArray *user_list_store_search(UserListStore *store, const gchar *query) {
  if (!store) return NULL;

  GPtrArray *arr = g_ptr_array_new_with_free_func((GDestroyNotify)user_list_entry_free);

  if (!query || !*query) {
    /* Return all */
    for (guint i = 0; i < store->entries->len; i++) {
      UserListEntry *entry = g_ptr_array_index(store->entries, i);
      g_ptr_array_add(arr, user_list_entry_copy(entry));
    }
    return arr;
  }

  /* Search by pubkey prefix or petname */
  gchar *query_lower = g_utf8_strdown(query, -1);

  for (guint i = 0; i < store->entries->len; i++) {
    UserListEntry *entry = g_ptr_array_index(store->entries, i);

    gboolean match = FALSE;

    /* Match pubkey prefix */
    if (entry->pubkey && g_str_has_prefix(entry->pubkey, query)) {
      match = TRUE;
    }

    /* Match petname (case-insensitive) */
    if (!match && entry->petname) {
      gchar *petname_lower = g_utf8_strdown(entry->petname, -1);
      if (g_strstr_len(petname_lower, -1, query_lower)) {
        match = TRUE;
      }
      g_free(petname_lower);
    }

    if (match) {
      g_ptr_array_add(arr, user_list_entry_copy(entry));
    }
  }

  g_free(query_lower);
  return arr;
}

UserListType user_list_store_get_type(UserListStore *store) {
  return store ? store->type : USER_LIST_FOLLOWS;
}

gint64 user_list_store_get_last_sync(UserListStore *store) {
  return store ? store->last_sync : 0;
}

void user_list_store_set_last_sync(UserListStore *store, gint64 timestamp) {
  if (store) store->last_sync = timestamp;
}

guint user_list_store_merge_event(UserListStore *store, const gchar *event_json) {
  if (!store || !event_json) return 0;

  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, event_json, -1, NULL)) {
    return 0;
  }

  JsonNode *root_node = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_OBJECT(root_node)) {
    return 0;
  }

  JsonObject *root = json_node_get_object(root_node);

  /* Verify kind */
  if (!json_object_has_member(root, "kind")) {
    return 0;
  }

  gint kind = (gint)json_object_get_int_member(root, "kind");
  gint expected_kind = user_list_store_get_kind(store);
  if (kind != expected_kind) {
    return 0;
  }

  if (!json_object_has_member(root, "tags")) {
    return 0;
  }

  JsonNode *tags_node = json_object_get_member(root, "tags");
  if (!JSON_NODE_HOLDS_ARRAY(tags_node)) {
    return 0;
  }

  JsonArray *tags = json_node_get_array(tags_node);
  guint added = 0;

  guint n = json_array_get_length(tags);
  for (guint i = 0; i < n; i++) {
    JsonNode *tag_node = json_array_get_element(tags, i);
    if (!JSON_NODE_HOLDS_ARRAY(tag_node)) continue;

    JsonArray *tag = json_node_get_array(tag_node);
    guint tag_len = json_array_get_length(tag);
    if (tag_len < 2) continue;

    const gchar *tag_type = json_array_get_string_element(tag, 0);
    if (g_strcmp0(tag_type, "p") != 0) continue;

    const gchar *pubkey = json_array_get_string_element(tag, 1);
    if (!pubkey || !*pubkey) continue;

    /* Skip if already exists */
    if (user_list_store_contains(store, pubkey)) continue;

    gchar *relay_hint = NULL;
    gchar *petname = NULL;

    if (tag_len >= 3) {
      const gchar *r = json_array_get_string_element(tag, 2);
      if (r && *r) relay_hint = g_strdup(r);
    }

    if (tag_len >= 4 && store->type == USER_LIST_FOLLOWS) {
      const gchar *p = json_array_get_string_element(tag, 3);
      if (p && *p) petname = g_strdup(p);
    }

    UserListEntry *entry = g_new0(UserListEntry, 1);
    entry->pubkey = g_strdup(pubkey);
    entry->relay_hint = relay_hint;
    entry->petname = petname;

    g_ptr_array_add(store->entries, entry);
    added++;
  }

  return added;
}

gboolean user_list_store_update_profile(UserListStore *store, const gchar *pubkey,
                                        const gchar *display_name,
                                        const gchar *avatar_url,
                                        const gchar *nip05) {
  if (!store || !pubkey) return FALSE;

  gint idx = find_entry_by_pubkey(store, pubkey);
  if (idx < 0) return FALSE;

  UserListEntry *entry = g_ptr_array_index(store->entries, (guint)idx);

  g_free(entry->display_name);
  entry->display_name = g_strdup(display_name);

  g_free(entry->avatar_url);
  entry->avatar_url = g_strdup(avatar_url);

  g_free(entry->nip05);
  entry->nip05 = g_strdup(nip05);

  return TRUE;
}

const UserListEntry *user_list_store_get_entry(UserListStore *store, const gchar *pubkey) {
  if (!store || !pubkey) return NULL;

  gint idx = find_entry_by_pubkey(store, pubkey);
  if (idx < 0) return NULL;

  return g_ptr_array_index(store->entries, (guint)idx);
}

gchar *user_list_store_get_display_name(UserListStore *store, const gchar *pubkey) {
  if (!store || !pubkey) return NULL;

  const UserListEntry *entry = user_list_store_get_entry(store, pubkey);
  if (!entry) return NULL;

  /* Priority: petname > display_name > truncated pubkey */
  if (entry->petname && *entry->petname) {
    return g_strdup(entry->petname);
  }
  if (entry->display_name && *entry->display_name) {
    return g_strdup(entry->display_name);
  }
  /* Return truncated pubkey */
  return g_strdup_printf("%.12s...", pubkey);
}

void user_list_store_request_profiles(UserListStore *store,
                                      UserListProfileFetchCb callback,
                                      gpointer user_data) {
  if (!store || !callback) return;

  for (guint i = 0; i < store->entries->len; i++) {
    UserListEntry *entry = g_ptr_array_index(store->entries, i);
    /* Call the callback with current cached info (may be NULL) */
    callback(entry->pubkey, entry->display_name, entry->avatar_url,
             entry->nip05, user_data);
  }
}

void user_list_store_set_owner(UserListStore *store, const gchar *owner_pubkey) {
  if (!store) return;
  g_free(store->owner_pubkey);
  store->owner_pubkey = g_strdup(owner_pubkey);
}

const gchar *user_list_store_get_owner(UserListStore *store) {
  return store ? store->owner_pubkey : NULL;
}

gchar *user_list_store_build_fetch_filter(UserListStore *store, const gchar *pubkey) {
  if (!store || !pubkey) return NULL;

  gint kind = user_list_store_get_kind(store);

  /* Build REQ filter for fetching user's list from relay:
   * {"kinds":[<kind>],"authors":["<pubkey>"],"limit":1}
   */
  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_object(builder);

  json_builder_set_member_name(builder, "kinds");
  json_builder_begin_array(builder);
  json_builder_add_int_value(builder, kind);
  json_builder_end_array(builder);

  json_builder_set_member_name(builder, "authors");
  json_builder_begin_array(builder);
  json_builder_add_string_value(builder, pubkey);
  json_builder_end_array(builder);

  json_builder_set_member_name(builder, "limit");
  json_builder_add_int_value(builder, 1);

  json_builder_end_object(builder);

  JsonNode *root = json_builder_get_root(builder);
  g_autoptr(JsonGenerator) gen = json_generator_new();
  json_generator_set_root(gen, root);
  gchar *result = json_generator_to_data(gen, NULL);

  json_node_unref(root);

  return result;
}

void user_list_store_mark_synced(UserListStore *store) {
  if (store) {
    store->last_sync = (gint64)time(NULL);
  }
}

gboolean user_list_store_needs_sync(UserListStore *store, gint64 threshold_seconds) {
  if (!store) return FALSE;
  if (store->last_sync == 0) return TRUE;

  gint64 now = (gint64)time(NULL);
  return (now - store->last_sync) > threshold_seconds;
}
