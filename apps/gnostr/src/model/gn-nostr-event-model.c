#include "gn-nostr-event-model.h"
#include "../storage_ndb.h"
#include <nostr.h>
#include <string.h>

/* Maximum visible items and LRU cache size */
#define MODEL_MAX_ITEMS 500
#define ITEM_CACHE_SIZE 100

/* Note entry for sorted storage */
typedef struct {
  uint64_t note_key;
  gint64 created_at;
} NoteEntry;

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

  /* Core data: note keys sorted by created_at (newest first) */
  GArray *notes;  /* element-type: NoteEntry */

  /* nostrdb subscription */
  uint64_t subscription_id;

  /* Small LRU cache for visible items */
  GHashTable *item_cache;  /* key: uint64_t*, value: GnNostrEventItem* */
  GQueue *cache_lru;       /* uint64_t* keys in LRU order */

  /* Profile cache - pubkey -> GnNostrProfile */
  GHashTable *profile_cache;  /* key: pubkey (string), value: GnNostrProfile* */

  /* Thread info cache - note_key -> ThreadInfo */
  GHashTable *thread_info;
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

static guint uint64_hash(gconstpointer key) {
  uint64_t k = *(const uint64_t *)key;
  return (guint)(k ^ (k >> 32));
}

static gboolean uint64_equal(gconstpointer a, gconstpointer b) {
  return *(const uint64_t *)a == *(const uint64_t *)b;
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

  /* Unsubscribe from nostrdb */
  if (self->subscription_id > 0) {
    storage_ndb_unsubscribe(self->subscription_id);
    self->subscription_id = 0;
  }

  g_free(self->kinds);
  g_strfreev(self->authors);
  g_free(self->root_event_id);

  g_array_unref(self->notes);
  g_hash_table_unref(self->item_cache);
  g_queue_free(self->cache_lru);
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
  self->notes = g_array_new(FALSE, FALSE, sizeof(NoteEntry));
  self->item_cache = g_hash_table_new_full(uint64_hash, uint64_equal, g_free, g_object_unref);
  self->cache_lru = g_queue_new();
  self->profile_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
  self->thread_info = g_hash_table_new_full(uint64_hash, uint64_equal, g_free, (GDestroyNotify)thread_info_free);
  self->limit = 100;
}

/* LRU cache management */
static void cache_touch(GnNostrEventModel *self, uint64_t key) {
  /* Move to front of LRU queue */
  GList *link = g_queue_find_custom(self->cache_lru, &key, (GCompareFunc)uint64_equal);
  if (link) {
    g_queue_unlink(self->cache_lru, link);
    g_queue_push_head_link(self->cache_lru, link);
  }
}

static void cache_add(GnNostrEventModel *self, uint64_t key, GnNostrEventItem *item) {
  uint64_t *key_copy = g_new(uint64_t, 1);
  *key_copy = key;

  g_hash_table_insert(self->item_cache, key_copy, g_object_ref(item));
  g_queue_push_head(self->cache_lru, key_copy);

  /* Evict oldest if over capacity */
  while (g_queue_get_length(self->cache_lru) > ITEM_CACHE_SIZE) {
    uint64_t *old_key = g_queue_pop_tail(self->cache_lru);
    if (old_key) {
      g_hash_table_remove(self->item_cache, old_key);
      /* Note: key is freed by hash table */
    }
  }
}

/* GListModel interface implementation */

static GType gn_nostr_event_model_get_item_type(GListModel *list) {
  return GN_TYPE_NOSTR_EVENT_ITEM;
}

static guint gn_nostr_event_model_get_n_items(GListModel *list) {
  GnNostrEventModel *self = GN_NOSTR_EVENT_MODEL(list);
  return self->notes->len;
}

static gpointer gn_nostr_event_model_get_item(GListModel *list, guint position) {
  GnNostrEventModel *self = GN_NOSTR_EVENT_MODEL(list);

  if (position >= self->notes->len) {
    return NULL;
  }

  NoteEntry *entry = &g_array_index(self->notes, NoteEntry, position);
  uint64_t key = entry->note_key;

  /* Check LRU cache */
  GnNostrEventItem *item = g_hash_table_lookup(self->item_cache, &key);
  if (item) {
    cache_touch(self, key);
    return g_object_ref(item);
  }

  /* Create new item from nostrdb */
  item = gn_nostr_event_item_new_from_key(key, entry->created_at);

  /* Apply thread info if available */
  ThreadInfo *tinfo = g_hash_table_lookup(self->thread_info, &key);
  if (tinfo) {
    gn_nostr_event_item_set_thread_info(item, tinfo->root_id, tinfo->parent_id, tinfo->depth);
  }

  /* Apply profile if available */
  const char *pubkey = gn_nostr_event_item_get_pubkey(item);
  if (pubkey) {
    GnNostrProfile *profile = g_hash_table_lookup(self->profile_cache, pubkey);
    if (profile) {
      gn_nostr_event_item_set_profile(item, profile);
    }
  }

  /* Add to cache */
  cache_add(self, key, item);

  return item;
}

static void gn_nostr_event_model_list_model_iface_init(GListModelInterface *iface) {
  iface->get_item_type = gn_nostr_event_model_get_item_type;
  iface->get_n_items = gn_nostr_event_model_get_n_items;
  iface->get_item = gn_nostr_event_model_get_item;
}

/* Helper functions */

static gboolean hex_to_bytes32(const char *hex, uint8_t out[32]) {
  if (!hex || !out) return FALSE;
  size_t L = strlen(hex);
  if (L != 64) return FALSE;
  for (int i = 0; i < 32; i++) {
    char c1 = hex[i*2];
    char c2 = hex[i*2+1];
    int v1, v2;
    if      (c1 >= '0' && c1 <= '9') v1 = c1 - '0';
    else if (c1 >= 'a' && c1 <= 'f') v1 = 10 + (c1 - 'a');
    else if (c1 >= 'A' && c1 <= 'F') v1 = 10 + (c1 - 'A');
    else return FALSE;
    if      (c2 >= '0' && c2 <= '9') v2 = c2 - '0';
    else if (c2 >= 'a' && c2 <= 'f') v2 = 10 + (c2 - 'a');
    else if (c2 >= 'A' && c2 <= 'F') v2 = 10 + (c2 - 'A');
    else return FALSE;
    out[i] = (uint8_t)((v1 << 4) | v2);
  }
  return TRUE;
}

static GnNostrProfile *get_or_create_profile(GnNostrEventModel *self, const char *pubkey) {
  GnNostrProfile *profile = g_hash_table_lookup(self->profile_cache, pubkey);
  if (!profile) {
    profile = gn_nostr_profile_new(pubkey);
    g_hash_table_insert(self->profile_cache, g_strdup(pubkey), profile);

    /* Try to load profile from nostrdb */
    void *txn = NULL;
    if (storage_ndb_begin_query(&txn) == 0) {
      uint8_t pk32[32];
      if (hex_to_bytes32(pubkey, pk32)) {
        char *json = NULL;
        int len = 0;
        if (storage_ndb_get_profile_by_pubkey(txn, pk32, &json, &len) == 0 && json) {
          /* Parse profile JSON from kind:0 content */
          NostrEvent *evt = nostr_event_new();
          if (evt && nostr_event_deserialize(evt, json) == 0) {
            const char *content = nostr_event_get_content(evt);
            if (content) {
              gn_nostr_profile_update_from_json(profile, content);
            }
          }
          if (evt) nostr_event_free(evt);
        }
      }
      storage_ndb_end_query(txn);
    }
  }
  return profile;
}

/* Find insertion position for sorted insert (newest first) */
static guint find_sorted_position(GnNostrEventModel *self, gint64 created_at) {
  for (guint i = 0; i < self->notes->len; i++) {
    NoteEntry *entry = &g_array_index(self->notes, NoteEntry, i);
    if (entry->created_at < created_at) {
      return i;
    }
  }
  return self->notes->len;
}

/* Check if note_key is already in the model */
static gboolean has_note_key(GnNostrEventModel *self, uint64_t key) {
  for (guint i = 0; i < self->notes->len; i++) {
    NoteEntry *entry = &g_array_index(self->notes, NoteEntry, i);
    if (entry->note_key == key) return TRUE;
  }
  return FALSE;
}

/* Parse NIP-10 tags for threading */
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

/* Add a note to the model */
static void add_note_internal(GnNostrEventModel *self, uint64_t note_key, gint64 created_at,
                               const char *root_id, const char *parent_id, guint depth) {
  if (has_note_key(self, note_key)) {
    return;  /* Already in model */
  }

  /* Store thread info */
  if (root_id || parent_id) {
    ThreadInfo *tinfo = g_new0(ThreadInfo, 1);
    tinfo->root_id = g_strdup(root_id);
    tinfo->parent_id = g_strdup(parent_id);
    tinfo->depth = depth;

    uint64_t *key_copy = g_new(uint64_t, 1);
    *key_copy = note_key;
    g_hash_table_insert(self->thread_info, key_copy, tinfo);
  }

  /* Find insertion position */
  guint pos = find_sorted_position(self, created_at);

  /* Insert note entry */
  NoteEntry entry = { .note_key = note_key, .created_at = created_at };
  g_array_insert_val(self->notes, pos, entry);

  /* Emit items-changed signal */
  g_list_model_items_changed(G_LIST_MODEL(self), pos, 0, 1);

  /* Enforce limit */
  if (self->notes->len > MODEL_MAX_ITEMS && !self->is_thread_view) {
    guint to_remove = self->notes->len - MODEL_MAX_ITEMS;
    guint old_len = self->notes->len;

    /* Remove oldest entries and their thread info */
    for (guint i = 0; i < to_remove; i++) {
      guint idx = old_len - 1 - i;
      NoteEntry *old = &g_array_index(self->notes, NoteEntry, idx);
      g_hash_table_remove(self->thread_info, &old->note_key);
      g_hash_table_remove(self->item_cache, &old->note_key);
    }

    g_array_set_size(self->notes, MODEL_MAX_ITEMS);
    g_list_model_items_changed(G_LIST_MODEL(self), MODEL_MAX_ITEMS, to_remove, 0);
  }
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

  g_debug("[MODEL] Refreshing model (current items: %u)", self->notes->len);

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
    g_string_append_printf(filter, "\"since\":%" G_GINT64_FORMAT ",", self->since);
  }

  if (self->until > 0) {
    g_string_append_printf(filter, "\"until\":%" G_GINT64_FORMAT ",", self->until);
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

  char **json_results = NULL;
  int count = 0;

  int query_rc = storage_ndb_query(txn, filter->str, &json_results, &count);
  g_debug("[MODEL] Query result: rc=%d count=%d", query_rc, count);

  guint added = 0;
  if (query_rc == 0) {
    for (int i = 0; i < count; i++) {
      const char *event_json = json_results[i];
      if (!event_json) continue;

      NostrEvent *evt = nostr_event_new();
      if (!evt) continue;

      if (nostr_event_deserialize(evt, event_json) == 0) {
        const char *event_id = nostr_event_get_id(evt);
        const char *pubkey = nostr_event_get_pubkey(evt);
        gint64 created_at = nostr_event_get_created_at(evt);

        /* Get note key from nostrdb */
        uint8_t id32[32];
        if (event_id && hex_to_bytes32(event_id, id32)) {
          uint64_t note_key = storage_ndb_get_note_key_by_id(txn, id32, NULL);
          if (note_key > 0) {
            /* Parse threading */
            char *root_id = NULL;
            char *reply_id = NULL;
            parse_nip10_tags(evt, &root_id, &reply_id);

            guint old_len = self->notes->len;
            add_note_internal(self, note_key, created_at, root_id, reply_id, 0);
            if (self->notes->len > old_len) added++;

            /* Ensure profile is in cache */
            if (pubkey) {
              get_or_create_profile(self, pubkey);
            }

            g_free(root_id);
            g_free(reply_id);
          }
        }
      }
      nostr_event_free(evt);
    }
    storage_ndb_free_results(json_results, count);
  }

  storage_ndb_end_query(txn);
  g_string_free(filter, TRUE);

  g_message("[MODEL] Refresh complete: %u total items (%u added)", self->notes->len, added);
}

void gn_nostr_event_model_update_profile(GObject *model, const char *pubkey_hex, const char *content_json) {
  g_return_if_fail(GN_IS_NOSTR_EVENT_MODEL(model));
  g_return_if_fail(pubkey_hex != NULL);
  g_return_if_fail(content_json != NULL);

  GnNostrEventModel *self = GN_NOSTR_EVENT_MODEL(model);

  GnNostrProfile *profile = g_hash_table_lookup(self->profile_cache, pubkey_hex);
  if (!profile) {
    g_debug("[MODEL] Profile for %.8s not in cache, skipping update", pubkey_hex);
    return;
  }

  gn_nostr_profile_update_from_json(profile, content_json);
  g_debug("[MODEL] Updated profile for %.8s in cache", pubkey_hex);

  /* Notify items in cache that have this pubkey */
  GHashTableIter iter;
  gpointer key, value;
  guint updated = 0;

  g_hash_table_iter_init(&iter, self->item_cache);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    GnNostrEventItem *item = GN_NOSTR_EVENT_ITEM(value);
    const char *item_pubkey = gn_nostr_event_item_get_pubkey(item);
    if (item_pubkey && g_strcmp0(item_pubkey, pubkey_hex) == 0) {
      g_object_notify(G_OBJECT(item), "profile");
      updated++;
    }
  }

  if (updated > 0) {
    g_message("[MODEL] Profile update for %.8s notified on %u cached items", pubkey_hex, updated);
  }
}

void gn_nostr_event_model_check_pending_for_profile(GnNostrEventModel *self, const char *pubkey) {
  /* No longer needed - events shown immediately */
  (void)self;
  (void)pubkey;
}

void gn_nostr_event_model_clear(GnNostrEventModel *self) {
  g_return_if_fail(GN_IS_NOSTR_EVENT_MODEL(self));

  guint old_size = self->notes->len;
  if (old_size == 0) return;

  g_array_set_size(self->notes, 0);
  g_hash_table_remove_all(self->item_cache);
  g_queue_clear(self->cache_lru);
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

void gn_nostr_event_model_add_event_json(GnNostrEventModel *self, const char *event_json) {
  g_return_if_fail(GN_IS_NOSTR_EVENT_MODEL(self));
  g_return_if_fail(event_json != NULL);

  NostrEvent *evt = nostr_event_new();
  if (!evt) return;

  if (nostr_event_deserialize(evt, event_json) != 0) {
    nostr_event_free(evt);
    return;
  }

  /* Check kind filter */
  int kind = nostr_event_get_kind(evt);
  gboolean kind_matches = (self->n_kinds == 0);
  for (gsize i = 0; i < self->n_kinds && !kind_matches; i++) {
    if (self->kinds[i] == kind) kind_matches = TRUE;
  }

  if (!kind_matches) {
    nostr_event_free(evt);
    return;
  }

  const char *event_id = nostr_event_get_id(evt);
  const char *pubkey = nostr_event_get_pubkey(evt);
  gint64 created_at = nostr_event_get_created_at(evt);

  /* Get note key */
  void *txn = NULL;
  if (storage_ndb_begin_query(&txn) != 0) {
    nostr_event_free(evt);
    return;
  }

  uint8_t id32[32];
  if (event_id && hex_to_bytes32(event_id, id32)) {
    uint64_t note_key = storage_ndb_get_note_key_by_id(txn, id32, NULL);
    if (note_key > 0) {
      char *root_id = NULL;
      char *reply_id = NULL;
      parse_nip10_tags(evt, &root_id, &reply_id);

      add_note_internal(self, note_key, created_at, root_id, reply_id, 0);

      if (pubkey) {
        get_or_create_profile(self, pubkey);
      }

      g_free(root_id);
      g_free(reply_id);

      g_message("[MODEL] Added live event %.8s", event_id);
    }
  }

  storage_ndb_end_query(txn);
  nostr_event_free(evt);
}

/* Delayed event add data */
typedef struct {
  GnNostrEventModel *model;
  char *event_id;
  char *pubkey;
  gint64 created_at;
  char *root_id;
  char *reply_id;
  int retries;
} DelayedEventAdd;

static void delayed_event_add_free(DelayedEventAdd *data) {
  if (!data) return;
  g_free(data->event_id);
  g_free(data->pubkey);
  g_free(data->root_id);
  g_free(data->reply_id);
  g_free(data);
}

static gboolean try_add_delayed_event(gpointer user_data) {
  DelayedEventAdd *data = (DelayedEventAdd *)user_data;

  if (!GN_IS_NOSTR_EVENT_MODEL(data->model)) {
    delayed_event_add_free(data);
    return G_SOURCE_REMOVE;
  }

  void *txn = NULL;
  if (storage_ndb_begin_query(&txn) != 0) {
    /* Retry if we haven't exceeded max retries (15 retries @ 100ms = 1.5s) */
    if (data->retries < 15) {
      data->retries++;
      return G_SOURCE_CONTINUE;
    }
    g_warning("[MODEL] Failed to get txn for delayed event %.8s after retries", data->event_id);
    delayed_event_add_free(data);
    return G_SOURCE_REMOVE;
  }

  uint8_t id32[32];
  if (hex_to_bytes32(data->event_id, id32)) {
    uint64_t note_key = storage_ndb_get_note_key_by_id(txn, id32, NULL);
    if (note_key > 0) {
      add_note_internal(data->model, note_key, data->created_at, data->root_id, data->reply_id, 0);

      if (data->pubkey) {
        get_or_create_profile(data->model, data->pubkey);
      }

      g_debug("[MODEL] Added delayed event %.8s (key=%lu, retries=%d)", data->event_id, (unsigned long)note_key, data->retries);
      storage_ndb_end_query(txn);
      delayed_event_add_free(data);
      return G_SOURCE_REMOVE;
    }
  }

  storage_ndb_end_query(txn);

  /* Not found yet - retry if we haven't exceeded max retries (15 retries @ 100ms = 1.5s) */
  if (data->retries < 15) {
    data->retries++;
    return G_SOURCE_CONTINUE;  /* Try again after another interval */
  }

  g_warning("[MODEL] Event %.8s not found in nostrdb after retries", data->event_id);
  delayed_event_add_free(data);
  return G_SOURCE_REMOVE;
}

void gn_nostr_event_model_add_live_event(GnNostrEventModel *self, void *nostr_event) {
  g_return_if_fail(GN_IS_NOSTR_EVENT_MODEL(self));
  g_return_if_fail(nostr_event != NULL);

  NostrEvent *evt = (NostrEvent *)nostr_event;

  /* Check kind filter */
  int kind = nostr_event_get_kind(evt);
  gboolean kind_matches = (self->n_kinds == 0);
  for (gsize i = 0; i < self->n_kinds && !kind_matches; i++) {
    if (self->kinds[i] == kind) kind_matches = TRUE;
  }

  if (!kind_matches) {
    return;
  }

  const char *event_id = nostr_event_get_id(evt);
  const char *pubkey = nostr_event_get_pubkey(evt);
  gint64 created_at = nostr_event_get_created_at(evt);

  if (!event_id) return;

  /* Parse threading now since we have the event */
  char *root_id = NULL;
  char *reply_id = NULL;
  parse_nip10_tags(evt, &root_id, &reply_id);

  /* Try immediate lookup first */
  void *txn = NULL;
  if (storage_ndb_begin_query(&txn) == 0) {
    uint8_t id32[32];
    if (hex_to_bytes32(event_id, id32)) {
      uint64_t note_key = storage_ndb_get_note_key_by_id(txn, id32, NULL);
      if (note_key > 0) {
        /* Found immediately - add now */
        add_note_internal(self, note_key, created_at, root_id, reply_id, 0);
        if (pubkey) {
          get_or_create_profile(self, pubkey);
        }
        g_debug("[MODEL] Added event %.8s immediately (key=%lu)", event_id, (unsigned long)note_key);
        storage_ndb_end_query(txn);
        g_free(root_id);
        g_free(reply_id);
        return;
      }
    }
    storage_ndb_end_query(txn);
  }

  /* Not in nostrdb yet (async ingestion) - schedule delayed add */
  DelayedEventAdd *data = g_new0(DelayedEventAdd, 1);
  data->model = self;
  data->event_id = g_strdup(event_id);
  data->pubkey = g_strdup(pubkey);
  data->created_at = created_at;
  data->root_id = root_id;  /* Take ownership */
  data->reply_id = reply_id;  /* Take ownership */
  data->retries = 0;

  /* Try again after 100ms (nostrdb async ingestion may take a moment) */
  g_timeout_add(100, try_add_delayed_event, data);
}
