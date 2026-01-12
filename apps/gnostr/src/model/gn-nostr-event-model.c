#include "gn-nostr-event-model.h"
#include "../storage_ndb.h"
#include <nostr.h>
#include <string.h>

struct _GnNostrEventModel {
  GObject parent_instance;
  
  /* Query parameters */
  gint *kinds;
  gsize n_kinds;
  char **authors;
  gsize n_authors;
  gint64 since;
  gint64 until;
  guint limit;
  
  /* Thread view */
  gboolean is_thread_view;
  char *root_event_id;
  
  /* Event storage - ordered list of event IDs */
  GPtrArray *event_ids;  /* element-type: char* (owned) */
  
  /* Item cache - event_id -> GnNostrEventItem */
  GHashTable *item_cache;  /* key: event_id (string), value: GnNostrEventItem* (owned) */
  
  /* Profile cache - pubkey -> GnNostrProfile */
  GHashTable *profile_cache;  /* key: pubkey (string), value: GnNostrProfile* (owned) */
  
  /* Threading data - event_id -> thread info */
  GHashTable *thread_info;  /* key: event_id, value: ThreadInfo* */
};

typedef struct {
  char *root_id;
  char *parent_id;
  guint depth;
} ThreadInfo;

static void thread_info_free(ThreadInfo *info) {
  if (!info) return;
  g_free(info->root_id);
  g_free(info->parent_id);
  g_free(info);
}

static void gn_nostr_event_model_list_model_iface_init(GListModelInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GnNostrEventModel, gn_nostr_event_model, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(G_TYPE_LIST_MODEL, gn_nostr_event_model_list_model_iface_init))

enum {
  PROP_0,
  PROP_IS_THREAD_VIEW,
  PROP_ROOT_EVENT_ID,
  N_PROPS
};

static GParamSpec *properties[N_PROPS];

static void gn_nostr_event_model_finalize(GObject *object) {
  GnNostrEventModel *self = GN_NOSTR_EVENT_MODEL(object);
  
  g_free(self->kinds);
  g_strfreev(self->authors);
  g_free(self->root_event_id);
  
  g_ptr_array_unref(self->event_ids);
  g_hash_table_unref(self->item_cache);
  g_hash_table_unref(self->profile_cache);
  g_hash_table_unref(self->thread_info);
  
  G_OBJECT_CLASS(gn_nostr_event_model_parent_class)->finalize(object);
}

static void gn_nostr_event_model_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec) {
  GnNostrEventModel *self = GN_NOSTR_EVENT_MODEL(object);
  
  switch (prop_id) {
    case PROP_IS_THREAD_VIEW:
      g_value_set_boolean(value, self->is_thread_view);
      break;
    case PROP_ROOT_EVENT_ID:
      g_value_set_string(value, self->root_event_id);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void gn_nostr_event_model_class_init(GnNostrEventModelClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  
  object_class->finalize = gn_nostr_event_model_finalize;
  object_class->get_property = gn_nostr_event_model_get_property;
  
  properties[PROP_IS_THREAD_VIEW] = g_param_spec_boolean("is-thread-view", "Is Thread View", 
                                                          "Whether this is a thread view", FALSE,
                                                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  properties[PROP_ROOT_EVENT_ID] = g_param_spec_string("root-event-id", "Root Event ID",
                                                        "Thread root event ID", NULL,
                                                        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  
  g_object_class_install_properties(object_class, N_PROPS, properties);
}

static void gn_nostr_event_model_init(GnNostrEventModel *self) {
  self->event_ids = g_ptr_array_new_with_free_func(g_free);
  self->item_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
  self->profile_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
  self->thread_info = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)thread_info_free);
  self->limit = 100;
}

/* GListModel interface implementation */

static GType gn_nostr_event_model_get_item_type(GListModel *list) {
  return GN_TYPE_NOSTR_EVENT_ITEM;
}

static guint gn_nostr_event_model_get_n_items(GListModel *list) {
  GnNostrEventModel *self = GN_NOSTR_EVENT_MODEL(list);
  return self->event_ids->len;
}

static gpointer gn_nostr_event_model_get_item(GListModel *list, guint position) {
  GnNostrEventModel *self = GN_NOSTR_EVENT_MODEL(list);
  
  if (position >= self->event_ids->len) {
    g_warning("[MODEL] get_item: position %u >= len %u", position, self->event_ids->len);
    return NULL;
  }
  
  const char *event_id = g_ptr_array_index(self->event_ids, position);
  GnNostrEventItem *item = g_hash_table_lookup(self->item_cache, event_id);
  
  if (item) {
    return g_object_ref(item);
  }
  
  return NULL;
}

static void gn_nostr_event_model_list_model_iface_init(GListModelInterface *iface) {
  iface->get_item_type = gn_nostr_event_model_get_item_type;
  iface->get_n_items = gn_nostr_event_model_get_n_items;
  iface->get_item = gn_nostr_event_model_get_item;
}

/* Helper functions */

static GnNostrProfile *gn_nostr_event_model_get_or_create_profile(GnNostrEventModel *self, const char *pubkey) {
  GnNostrProfile *profile = g_hash_table_lookup(self->profile_cache, pubkey);
  if (!profile) {
    profile = gn_nostr_profile_new(pubkey);
    g_hash_table_insert(self->profile_cache, g_strdup(pubkey), profile);
    
    /* Try to load profile from nostrdb */
    void *txn = NULL;
    if (storage_ndb_begin_query(&txn) == 0) {
      char filter[256];
      snprintf(filter, sizeof(filter), "[{\"kinds\":[0],\"authors\":[\"%s\"],\"limit\":1}]", pubkey);
      
      char **results = NULL;
      int count = 0;
      if (storage_ndb_query(txn, filter, &results, &count) == 0 && count > 0) {
        /* storage_ndb_query returns full JSON event objects */
        const char *event_json = results[0];
        if (event_json) {
          NostrEvent *evt = nostr_event_new();
          if (evt && nostr_event_deserialize(evt, event_json) == 0) {
            const char *content = nostr_event_get_content(evt);
            if (content) {
              gn_nostr_profile_update_from_json(profile, content);
            }
          }
          if (evt) nostr_event_free(evt);
        }
        storage_ndb_free_results(results, count);
      }
      storage_ndb_end_query(txn);
    }
  }
  return profile;
}

static void parse_nip10_tags(NostrEvent *evt, char **root_id, char **reply_id) {
  *root_id = NULL;
  *reply_id = NULL;
  
  NostrTags *tags = (NostrTags*)nostr_event_get_tags(evt);
  if (!tags) return;
  
  for (size_t i = 0; i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (!tag || nostr_tag_size(tag) < 2) continue;
    
    const char *key = nostr_tag_get(tag, 0);
    if (strcmp(key, "e") != 0) continue;
    
    const char *event_id = nostr_tag_get(tag, 1);
    if (!event_id || strlen(event_id) != 64) continue;
    
    const char *marker = (nostr_tag_size(tag) >= 4) ? nostr_tag_get(tag, 3) : NULL;
    
    if (marker && strcmp(marker, "root") == 0) {
      *root_id = g_strdup(event_id);
    } else if (marker && strcmp(marker, "reply") == 0) {
      *reply_id = g_strdup(event_id);
    } else if (!*root_id && i == 0) {
      *root_id = g_strdup(event_id);
    }
  }
}

static guint calculate_thread_depth(GnNostrEventModel *self, const char *event_id, const char *parent_id) {
  if (!parent_id) return 0;
  
  /* Only show depth if parent is actually in the timeline */
  if (!g_hash_table_contains(self->item_cache, parent_id)) {
    g_debug("[THREAD] Parent %s not in timeline for event %.8s - depth=0", parent_id, event_id);
    return 0;
  }
  
  guint depth = 1;
  char *current = g_strdup(parent_id);
  
  while (current && depth < 100) {
    ThreadInfo *info = g_hash_table_lookup(self->thread_info, current);
    if (!info || !info->parent_id) {
      break;
    }
    
    char *next = g_strdup(info->parent_id);
    g_free(current);
    current = next;
    depth++;
  }
  
  g_free(current);
  return depth;
}

static void gn_nostr_event_model_add_event(GnNostrEventModel *self, NostrEvent *evt) {
  const char *event_id = nostr_event_get_id(evt);
  if (!event_id) return;
  
  /* Check if already exists */
  if (g_hash_table_contains(self->item_cache, event_id)) {
    g_debug("[MODEL] Skipping duplicate event %.8s", event_id);
    return;
  }
  
  g_debug("[MODEL] Adding new event %.8s", event_id);
  
  /* Parse event data */
  const char *pubkey = nostr_event_get_pubkey(evt);
  gint64 created_at = nostr_event_get_created_at(evt);
  const char *content = nostr_event_get_content(evt);
  gint kind = nostr_event_get_kind(evt);
  
  /* Parse NIP-10 threading */
  char *root_id = NULL;
  char *reply_id = NULL;
  parse_nip10_tags(evt, &root_id, &reply_id);
  
  g_debug("[THREAD] Event %.8s: root=%s reply=%s", 
          event_id, 
          root_id ? root_id : "(none)",
          reply_id ? reply_id : "(none)");
  
  /* Calculate depth */
  guint depth = calculate_thread_depth(self, event_id, reply_id);
  
  /* Store thread info */
  ThreadInfo *tinfo = g_new0(ThreadInfo, 1);
  tinfo->root_id = root_id;
  tinfo->parent_id = reply_id;
  tinfo->depth = depth;
  g_hash_table_insert(self->thread_info, g_strdup(event_id), tinfo);
  
  /* Create item */
  GnNostrEventItem *item = gn_nostr_event_item_new(event_id);
  gn_nostr_event_item_update_from_event(item, pubkey, created_at, content, kind);
  gn_nostr_event_item_set_thread_info(item, root_id, reply_id, depth);
  
  /* Get or create profile */
  if (pubkey) {
    GnNostrProfile *profile = gn_nostr_event_model_get_or_create_profile(self, pubkey);
    gn_nostr_event_item_set_profile(item, profile);
  }
  
  /* Add to cache */
  g_hash_table_insert(self->item_cache, g_strdup(event_id), item);
  
  /* Determine insertion position */
  guint position = 0;
  
  if (self->is_thread_view && reply_id) {
    /* Thread view: insert after parent */
    for (guint i = 0; i < self->event_ids->len; i++) {
      const char *id = g_ptr_array_index(self->event_ids, i);
      if (strcmp(id, reply_id) == 0) {
        position = i + 1;
        break;
      }
    }
  } else {
    /* Timeline view: insert by timestamp (newest first) */
    for (guint i = 0; i < self->event_ids->len; i++) {
      const char *id = g_ptr_array_index(self->event_ids, i);
      GnNostrEventItem *existing = g_hash_table_lookup(self->item_cache, id);
      if (existing && gn_nostr_event_item_get_created_at(existing) < created_at) {
        position = i;
        break;
      }
    }
    if (position == 0 && self->event_ids->len > 0) {
      position = self->event_ids->len;
    }
  }
  
  /* Insert into list */
  g_ptr_array_insert(self->event_ids, position, g_strdup(event_id));
  
  /* Emit items-changed signal */
  g_list_model_items_changed(G_LIST_MODEL(self), position, 0, 1);
  
  g_debug("[MODEL] Added event %s at position %u (depth=%u, total=%u)", 
            event_id, position, depth, self->event_ids->len);
}

/* Public API */

GnNostrEventModel *gn_nostr_event_model_new(void) {
  return g_object_new(GN_TYPE_NOSTR_EVENT_MODEL, NULL);
}

void gn_nostr_event_model_set_query(GnNostrEventModel *self, const GnNostrQueryParams *params) {
  g_return_if_fail(GN_IS_NOSTR_EVENT_MODEL(self));
  g_return_if_fail(params != NULL);
  
  /* Update query parameters */
  g_free(self->kinds);
  self->kinds = NULL;
  self->n_kinds = 0;
  
  if (params->kinds && params->n_kinds > 0) {
    self->kinds = g_new(gint, params->n_kinds);
    memcpy(self->kinds, params->kinds, params->n_kinds * sizeof(gint));
    self->n_kinds = params->n_kinds;
  }
  
  g_strfreev(self->authors);
  self->authors = NULL;
  self->n_authors = 0;
  
  if (params->authors && params->n_authors > 0) {
    self->authors = g_new(char*, params->n_authors + 1);
    for (gsize i = 0; i < params->n_authors; i++) {
      self->authors[i] = g_strdup(params->authors[i]);
    }
    self->authors[params->n_authors] = NULL;
    self->n_authors = params->n_authors;
  }
  
  self->since = params->since;
  self->until = params->until;
  self->limit = params->limit > 0 ? params->limit : 100;
  
  g_debug("[MODEL] Query updated: kinds=%zu authors=%zu limit=%u", 
          self->n_kinds, self->n_authors, self->limit);
}

void gn_nostr_event_model_set_thread_root(GnNostrEventModel *self, const char *root_event_id) {
  g_return_if_fail(GN_IS_NOSTR_EVENT_MODEL(self));
  
  if (g_strcmp0(self->root_event_id, root_event_id) == 0) {
    return;
  }
  
  g_free(self->root_event_id);
  self->root_event_id = g_strdup(root_event_id);
  self->is_thread_view = (root_event_id != NULL);
  
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_IS_THREAD_VIEW]);
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_ROOT_EVENT_ID]);
  
  g_debug("[MODEL] Thread root set to: %s", root_event_id ? root_event_id : "(none)");
}

void gn_nostr_event_model_refresh(GnNostrEventModel *self) {
  g_return_if_fail(GN_IS_NOSTR_EVENT_MODEL(self));
  
  g_debug("[MODEL] Refreshing model (current items: %u)", self->event_ids->len);
  
  /* Build filter JSON */
  GString *filter = g_string_new("[{");
  
  if (self->n_kinds > 0) {
    g_string_append(filter, "\"kinds\":[");
    for (gsize i = 0; i < self->n_kinds; i++) {
      if (i > 0) g_string_append_c(filter, ',');
      g_string_append_printf(filter, "%d", self->kinds[i]);
    }
    g_string_append(filter, "],");
  }
  
  if (self->n_authors > 0) {
    g_string_append(filter, "\"authors\":[");
    for (gsize i = 0; i < self->n_authors; i++) {
      if (i > 0) g_string_append_c(filter, ',');
      g_string_append_printf(filter, "\"%s\"", self->authors[i]);
    }
    g_string_append(filter, "],");
  }
  
  if (self->since > 0) {
    g_string_append_printf(filter, "\"since\":%ld,", self->since);
  }
  
  if (self->until > 0) {
    g_string_append_printf(filter, "\"until\":%ld,", self->until);
  }
  
  g_string_append_printf(filter, "\"limit\":%u}]", self->limit);
  
  /* Query nostrdb */
  g_debug("[MODEL] Executing query: %s", filter->str);
  
  void *txn = NULL;
  if (storage_ndb_begin_query(&txn) != 0) {
    g_warning("[MODEL] Failed to begin query");
    g_string_free(filter, TRUE);
    return;
  }
  
  char **ids = NULL;
  int count = 0;
  
  int query_rc = storage_ndb_query(txn, filter->str, &ids, &count);
  g_debug("[MODEL] Query result: rc=%d count=%d", query_rc, count);
  
  guint added = 0, skipped = 0;
  if (query_rc == 0) {
    /* storage_ndb_query returns full JSON event objects, not just IDs */
    g_debug("[MODEL] Processing %d events from query", count);
    guint before_count = self->event_ids->len;
    
    for (int i = 0; i < count; i++) {
      const char *event_json = ids[i];
      if (!event_json) {
        g_warning("[MODEL] Event %d is NULL", i);
        continue;
      }
      
      NostrEvent *evt = nostr_event_new();
      if (!evt) {
        g_warning("[MODEL] Failed to allocate event %d", i);
        continue;
      }
      
      int deser_rc = nostr_event_deserialize(evt, event_json);
      if (deser_rc == 0) {
        guint before = self->event_ids->len;
        gn_nostr_event_model_add_event(self, evt);
        if (self->event_ids->len > before) {
          added++;
        } else {
          skipped++;
        }
      } else {
        g_warning("[MODEL] Failed to deserialize event %d (rc=%d)", i, deser_rc);
      }
      nostr_event_free(evt);
    }
    storage_ndb_free_results(ids, count);
    
    g_message("[MODEL] Refresh complete: %u total items (%u added, %u skipped as duplicates)", 
              self->event_ids->len, added, skipped);
  }
  
  storage_ndb_end_query(txn);
  g_string_free(filter, TRUE);
}

void gn_nostr_event_model_update_profile(GObject *model, const char *pubkey_hex, const char *content_json) {
  g_return_if_fail(GN_IS_NOSTR_EVENT_MODEL(model));
  g_return_if_fail(pubkey_hex != NULL);
  g_return_if_fail(content_json != NULL);
  
  GnNostrEventModel *self = GN_NOSTR_EVENT_MODEL(model);
  
  /* Update the profile in the profile cache */
  GnNostrProfile *profile = g_hash_table_lookup(self->profile_cache, pubkey_hex);
  if (profile) {
    gn_nostr_profile_update_from_json(profile, content_json);
    g_debug("[MODEL] Updated profile for %.8s in cache", pubkey_hex);
  }
  
  /* Update all event items with this pubkey */
  guint updated = 0;
  for (guint i = 0; i < self->event_ids->len; i++) {
    const char *event_id = g_ptr_array_index(self->event_ids, i);
    GnNostrEventItem *item = g_hash_table_lookup(self->item_cache, event_id);
    if (!item) continue;
    
    gchar *item_pubkey = NULL;
    g_object_get(item, "pubkey", &item_pubkey, NULL);
    
    if (item_pubkey && g_strcmp0(item_pubkey, pubkey_hex) == 0) {
      /* This item belongs to the updated profile - update its profile reference */
      if (profile) {
        gn_nostr_event_item_set_profile(item, profile);
        updated++;
        g_debug("[MODEL] Updated profile for event %.8s", event_id);
      }
    }
    
    g_free(item_pubkey);
  }
  
  if (updated > 0) {
    g_message("[MODEL] ✓ Profile update for %.8s applied to %u events", pubkey_hex, updated);
  }
}

void gn_nostr_event_model_clear(GnNostrEventModel *self) {
  g_return_if_fail(GN_IS_NOSTR_EVENT_MODEL(self));
  
  guint old_size = self->event_ids->len;
  if (old_size == 0) return;
  
  g_ptr_array_remove_range(self->event_ids, 0, old_size);
  g_hash_table_remove_all(self->item_cache);
  g_hash_table_remove_all(self->thread_info);
  
  g_list_model_items_changed(G_LIST_MODEL(self), 0, old_size, 0);
  
  g_debug("[MODEL] Cleared %u items", old_size);
}

gboolean gn_nostr_event_model_get_is_thread_view(GnNostrEventModel *self) {
  g_return_val_if_fail(GN_IS_NOSTR_EVENT_MODEL(self), FALSE);
  return self->is_thread_view;
}

const char *gn_nostr_event_model_get_root_event_id(GnNostrEventModel *self) {
  g_return_val_if_fail(GN_IS_NOSTR_EVENT_MODEL(self), NULL);
  return self->root_event_id;
}
