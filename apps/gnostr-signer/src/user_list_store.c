/* user_list_store.c - User list management implementation */
#include "user_list_store.h"
#include <json.h>
#include <string.h>
#include <time.h>

struct _UserListStore {
  UserListType type;
  GPtrArray *entries;   /* Array of UserListEntry* */
  gchar *config_path;
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
  g_free(entry);
}

static UserListEntry *user_list_entry_copy(const UserListEntry *entry) {
  if (!entry) return NULL;
  UserListEntry *copy = g_new0(UserListEntry, 1);
  copy->pubkey = g_strdup(entry->pubkey);
  copy->relay_hint = g_strdup(entry->relay_hint);
  copy->petname = g_strdup(entry->petname);
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

  json_object *root = json_tokener_parse(contents);
  g_free(contents);

  if (!root || !json_object_is_type(root, json_type_array)) {
    if (root) json_object_put(root);
    return;
  }

  g_ptr_array_set_size(store->entries, 0);

  int n = json_object_array_length(root);
  for (int i = 0; i < n; i++) {
    json_object *item = json_object_array_get_idx(root, i);
    if (!item) continue;

    json_object *pubkey_obj;
    if (!json_object_object_get_ex(item, "pubkey", &pubkey_obj)) continue;

    UserListEntry *entry = g_new0(UserListEntry, 1);
    entry->pubkey = g_strdup(json_object_get_string(pubkey_obj));

    json_object *relay_obj, *petname_obj;
    if (json_object_object_get_ex(item, "relay", &relay_obj)) {
      entry->relay_hint = g_strdup(json_object_get_string(relay_obj));
    }
    if (json_object_object_get_ex(item, "petname", &petname_obj)) {
      entry->petname = g_strdup(json_object_get_string(petname_obj));
    }

    g_ptr_array_add(store->entries, entry);
  }

  json_object_put(root);
}

void user_list_store_save(UserListStore *store) {
  if (!store) return;

  json_object *root = json_object_new_array();

  for (guint i = 0; i < store->entries->len; i++) {
    UserListEntry *entry = g_ptr_array_index(store->entries, i);

    json_object *item = json_object_new_object();
    json_object_object_add(item, "pubkey", json_object_new_string(entry->pubkey));

    if (entry->relay_hint && *entry->relay_hint) {
      json_object_object_add(item, "relay", json_object_new_string(entry->relay_hint));
    }
    if (entry->petname && *entry->petname) {
      json_object_object_add(item, "petname", json_object_new_string(entry->petname));
    }

    json_object_array_add(root, item);
  }

  const gchar *json_str = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PRETTY);

  GError *err = NULL;
  if (!g_file_set_contents(store->config_path, json_str, -1, &err)) {
    if (err) {
      g_warning("user_list_store_save: %s", err->message);
      g_clear_error(&err);
    }
  }

  json_object_put(root);
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
  json_object *tags = json_object_new_array();

  for (guint i = 0; i < store->entries->len; i++) {
    UserListEntry *entry = g_ptr_array_index(store->entries, i);

    json_object *tag = json_object_new_array();
    json_object_array_add(tag, json_object_new_string("p"));
    json_object_array_add(tag, json_object_new_string(entry->pubkey));

    /* Add relay hint if present */
    if (entry->relay_hint && *entry->relay_hint) {
      json_object_array_add(tag, json_object_new_string(entry->relay_hint));
    } else {
      json_object_array_add(tag, json_object_new_string(""));
    }

    /* Add petname if present (for follows) */
    if (store->type == USER_LIST_FOLLOWS && entry->petname && *entry->petname) {
      json_object_array_add(tag, json_object_new_string(entry->petname));
    }

    json_object_array_add(tags, tag);
  }

  /* Build event */
  json_object *event = json_object_new_object();
  json_object_object_add(event, "kind", json_object_new_int(kind));
  json_object_object_add(event, "created_at", json_object_new_int64((int64_t)time(NULL)));
  json_object_object_add(event, "tags", tags);
  json_object_object_add(event, "content", json_object_new_string(""));

  gchar *result = g_strdup(json_object_to_json_string(event));
  json_object_put(event);

  return result;
}

gboolean user_list_store_parse_event(UserListStore *store, const gchar *event_json) {
  if (!store || !event_json) return FALSE;

  json_object *root = json_tokener_parse(event_json);
  if (!root) return FALSE;

  /* Verify kind */
  json_object *kind_obj;
  if (!json_object_object_get_ex(root, "kind", &kind_obj)) {
    json_object_put(root);
    return FALSE;
  }

  gint kind = json_object_get_int(kind_obj);
  gint expected_kind = user_list_store_get_kind(store);
  if (kind != expected_kind) {
    json_object_put(root);
    return FALSE;
  }

  json_object *tags_obj;
  if (!json_object_object_get_ex(root, "tags", &tags_obj) ||
      !json_object_is_type(tags_obj, json_type_array)) {
    json_object_put(root);
    return FALSE;
  }

  /* Clear and parse tags */
  g_ptr_array_set_size(store->entries, 0);

  int n = json_object_array_length(tags_obj);
  for (int i = 0; i < n; i++) {
    json_object *tag = json_object_array_get_idx(tags_obj, i);
    if (!tag || !json_object_is_type(tag, json_type_array)) continue;

    int tag_len = json_object_array_length(tag);
    if (tag_len < 2) continue;

    json_object *tag_type = json_object_array_get_idx(tag, 0);
    if (!tag_type || g_strcmp0(json_object_get_string(tag_type), "p") != 0) continue;

    json_object *pubkey_obj = json_object_array_get_idx(tag, 1);
    if (!pubkey_obj) continue;

    const gchar *pubkey = json_object_get_string(pubkey_obj);
    if (!pubkey || !*pubkey) continue;

    gchar *relay_hint = NULL;
    gchar *petname = NULL;

    if (tag_len >= 3) {
      json_object *relay_obj = json_object_array_get_idx(tag, 2);
      if (relay_obj) {
        const gchar *r = json_object_get_string(relay_obj);
        if (r && *r) relay_hint = g_strdup(r);
      }
    }

    if (tag_len >= 4 && store->type == USER_LIST_FOLLOWS) {
      json_object *petname_obj = json_object_array_get_idx(tag, 3);
      if (petname_obj) {
        const gchar *p = json_object_get_string(petname_obj);
        if (p && *p) petname = g_strdup(p);
      }
    }

    UserListEntry *entry = g_new0(UserListEntry, 1);
    entry->pubkey = g_strdup(pubkey);
    entry->relay_hint = relay_hint;
    entry->petname = petname;

    g_ptr_array_add(store->entries, entry);
  }

  json_object_put(root);
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
