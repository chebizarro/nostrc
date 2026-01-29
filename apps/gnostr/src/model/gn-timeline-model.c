/**
 * GnTimelineModel - Lazy view on NostrDB for timeline display
 *
 * This is the core of the new timeline architecture. Instead of maintaining
 * a manual list of items, we query NostrDB on-demand when GTK requests items.
 *
 * Key design decisions:
 * 1. No array of note keys - we query NostrDB directly in get_item()
 * 2. Cursor pagination using 'until' timestamp for older items
 * 3. New notes just increment count, no manual insertion
 * 4. Single items_changed signal on flush (replace-all strategy)
 * 5. Position-based cache cleared on invalidation
 *
 * Part of the Timeline Architecture Refactor (nostrc-e03f)
 */

#define G_LOG_DOMAIN "gn-timeline-model"

#include "gn-timeline-model.h"
#include "gn-nostr-event-item.h"
#include "gn-nostr-profile.h"
#include "gn-ndb-sub-dispatcher.h"
#include "../storage_ndb.h"
#include "../util/mute_list.h"
#include <string.h>
#include <stdio.h>

/* Configuration */
#define MODEL_PAGE_SIZE 50          /* Items per query page */
#define MODEL_MAX_CACHED 200        /* Max cached items */
#define PROFILE_CACHE_MAX 500       /* Max cached profiles */
#define INVALIDATE_DEBOUNCE_MS 100  /* Debounce invalidation signals */

/* Note entry for internal tracking */
typedef struct {
  uint64_t note_key;
  gint64 created_at;
} NoteEntry;

struct _GnTimelineModel {
  GObject parent_instance;

  /* Query filter */
  GnTimelineQuery *query;

  /* Note keys array - sorted by created_at descending */
  GArray *notes;

  /* Timestamps for pagination */
  gint64 newest_timestamp;
  gint64 oldest_timestamp;

  /* Item cache - note_key -> GnNostrEventItem */
  GHashTable *item_cache;
  GQueue *cache_lru;

  /* Profile cache - pubkey -> GnNostrProfile */
  GHashTable *profile_cache;
  GQueue *profile_cache_lru;

  /* Pending new items */
  GArray *pending_notes;
  guint pending_count;
  gboolean user_at_top;

  /* Invalidation debounce */
  guint invalidate_source_id;
  gboolean needs_refresh;

  /* Visible range for prefetching */
  guint visible_start;
  guint visible_end;

  /* Subscription */
  uint64_t sub_timeline;
};

/* Forward declarations */
static void gn_timeline_model_list_model_iface_init(GListModelInterface *iface);
static void on_sub_timeline_batch(uint64_t subid, const uint64_t *note_keys, guint n_keys, gpointer user_data);

G_DEFINE_TYPE_WITH_CODE(GnTimelineModel, gn_timeline_model, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(G_TYPE_LIST_MODEL, gn_timeline_model_list_model_iface_init))

enum {
  SIGNAL_NEW_ITEMS_PENDING,
  SIGNAL_NEED_PROFILE,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* ============== Hash Table Helpers ============== */

static guint uint64_hash(gconstpointer key) {
  uint64_t k = *(const uint64_t *)key;
  return (guint)(k ^ (k >> 32));
}

static gboolean uint64_equal(gconstpointer a, gconstpointer b) {
  return *(const uint64_t *)a == *(const uint64_t *)b;
}

/* ============== Cache Management ============== */

static void cache_clear(GnTimelineModel *self) {
  if (self->item_cache) {
    g_hash_table_remove_all(self->item_cache);
  }
  if (self->cache_lru) {
    g_queue_clear(self->cache_lru);
  }
}

static void cache_add(GnTimelineModel *self, uint64_t key, GnNostrEventItem *item) {
  uint64_t *key_copy = g_new(uint64_t, 1);
  *key_copy = key;

  /* Remove old entry if exists */
  GList *link = g_queue_find_custom(self->cache_lru, key_copy, (GCompareFunc)uint64_equal);
  if (link) {
    g_queue_delete_link(self->cache_lru, link);
    g_hash_table_remove(self->item_cache, key_copy);
  }

  g_hash_table_insert(self->item_cache, key_copy, g_object_ref(item));
  g_queue_push_head(self->cache_lru, key_copy);

  /* Evict oldest if over capacity */
  while (g_queue_get_length(self->cache_lru) > MODEL_MAX_CACHED) {
    uint64_t *old_key = g_queue_pop_tail(self->cache_lru);
    if (old_key) {
      g_hash_table_remove(self->item_cache, old_key);
    }
  }
}

static GnNostrEventItem *cache_get(GnTimelineModel *self, uint64_t key) {
  GnNostrEventItem *item = g_hash_table_lookup(self->item_cache, &key);
  if (item) {
    /* Move to front of LRU */
    GList *link = g_queue_find_custom(self->cache_lru, &key, (GCompareFunc)uint64_equal);
    if (link) {
      g_queue_unlink(self->cache_lru, link);
      g_queue_push_head_link(self->cache_lru, link);
    }
    return g_object_ref(item);
  }
  return NULL;
}

/* ============== Profile Cache ============== */

static void profile_cache_add(GnTimelineModel *self, const char *pubkey_hex, GnNostrProfile *profile) {
  if (!pubkey_hex || !profile) return;

  char *key = g_strdup(pubkey_hex);

  /* Remove old entry if exists */
  if (g_hash_table_contains(self->profile_cache, key)) {
    GList *link = g_queue_find_custom(self->profile_cache_lru, key, (GCompareFunc)g_strcmp0);
    if (link) {
      g_free(link->data);
      g_queue_delete_link(self->profile_cache_lru, link);
    }
  }

  g_hash_table_insert(self->profile_cache, key, g_object_ref(profile));
  g_queue_push_head(self->profile_cache_lru, g_strdup(pubkey_hex));

  /* Evict oldest if over capacity */
  while (g_queue_get_length(self->profile_cache_lru) > PROFILE_CACHE_MAX) {
    char *old_key = g_queue_pop_tail(self->profile_cache_lru);
    if (old_key) {
      g_hash_table_remove(self->profile_cache, old_key);
      g_free(old_key);
    }
  }
}

static GnNostrProfile *profile_cache_get(GnTimelineModel *self, const char *pubkey_hex) {
  if (!pubkey_hex) return NULL;
  return g_hash_table_lookup(self->profile_cache, pubkey_hex);
}

/* ============== Note Helpers ============== */

static gboolean has_note_key(GnTimelineModel *self, uint64_t key) {
  for (guint i = 0; i < self->notes->len; i++) {
    NoteEntry *entry = &g_array_index(self->notes, NoteEntry, i);
    if (entry->note_key == key) return TRUE;
  }
  return FALSE;
}

static gint note_entry_compare_newest_first(gconstpointer a, gconstpointer b) {
  const NoteEntry *ea = (const NoteEntry *)a;
  const NoteEntry *eb = (const NoteEntry *)b;
  if (ea->created_at > eb->created_at) return -1;
  if (ea->created_at < eb->created_at) return 1;
  return 0;
}

/* ============== GListModel Interface ============== */

static GType gn_timeline_model_get_item_type(GListModel *list) {
  return GN_TYPE_NOSTR_EVENT_ITEM;
}

static guint gn_timeline_model_get_n_items(GListModel *list) {
  GnTimelineModel *self = GN_TIMELINE_MODEL(list);
  return self->notes->len;
}

static gpointer gn_timeline_model_get_item(GListModel *list, guint position) {
  GnTimelineModel *self = GN_TIMELINE_MODEL(list);

  if (position >= self->notes->len) {
    return NULL;
  }

  NoteEntry *entry = &g_array_index(self->notes, NoteEntry, position);
  uint64_t key = entry->note_key;

  /* Check cache first */
  GnNostrEventItem *item = cache_get(self, key);
  if (item) {
    return item;  /* Already ref'd by cache_get */
  }

  /* Create new item from NostrDB */
  item = gn_nostr_event_item_new_from_key(key, entry->created_at);
  if (!item) {
    g_warning("Failed to create item for note_key %lu", (unsigned long)key);
    return NULL;
  }

  /* Apply profile if available */
  const char *pubkey = gn_nostr_event_item_get_pubkey(item);
  if (pubkey) {
    GnNostrProfile *profile = profile_cache_get(self, pubkey);
    if (profile) {
      gn_nostr_event_item_set_profile(item, profile);
    } else {
      /* Request profile fetch */
      g_signal_emit(self, signals[SIGNAL_NEED_PROFILE], 0, pubkey);
    }
  }

  /* Add to cache */
  cache_add(self, key, item);

  return item;
}

static void gn_timeline_model_list_model_iface_init(GListModelInterface *iface) {
  iface->get_item_type = gn_timeline_model_get_item_type;
  iface->get_n_items = gn_timeline_model_get_n_items;
  iface->get_item = gn_timeline_model_get_item;
}

/* ============== Subscription Callback ============== */

static void on_sub_timeline_batch(uint64_t subid, const uint64_t *note_keys, guint n_keys, gpointer user_data) {
  (void)subid;
  GnTimelineModel *self = GN_TIMELINE_MODEL(user_data);
  if (!GN_IS_TIMELINE_MODEL(self) || !note_keys || n_keys == 0) return;

  void *txn = NULL;
  if (storage_ndb_begin_query(&txn) != 0 || !txn) return;

  guint added = 0;
  for (guint i = 0; i < n_keys; i++) {
    uint64_t note_key = note_keys[i];

    /* Skip if already have this note */
    if (has_note_key(self, note_key)) continue;

    /* Check pending array too */
    gboolean in_pending = FALSE;
    for (guint j = 0; j < self->pending_notes->len; j++) {
      NoteEntry *pe = &g_array_index(self->pending_notes, NoteEntry, j);
      if (pe->note_key == note_key) {
        in_pending = TRUE;
        break;
      }
    }
    if (in_pending) continue;

    /* Get note from NostrDB */
    storage_ndb_note *note = storage_ndb_get_note_ptr(txn, note_key);
    if (!note) continue;

    int kind = storage_ndb_note_kind(note);

    /* Check if kind matches query */
    gboolean kind_ok = FALSE;
    if (self->query && self->query->n_kinds > 0) {
      for (gsize k = 0; k < self->query->n_kinds; k++) {
        if (self->query->kinds[k] == kind) {
          kind_ok = TRUE;
          break;
        }
      }
    } else {
      kind_ok = TRUE;  /* No kind filter */
    }
    if (!kind_ok) continue;

    /* Check mute list */
    const unsigned char *pk = storage_ndb_note_pubkey(note);
    if (pk) {
      char pubkey_hex[65];
      storage_ndb_hex_encode(pk, pubkey_hex);
      GnostrMuteList *mute_list = gnostr_mute_list_get_default();
      if (mute_list && gnostr_mute_list_is_pubkey_muted(mute_list, pubkey_hex)) {
        continue;
      }
    }

    gint64 created_at = storage_ndb_note_created_at(note);
    NoteEntry entry = { .note_key = note_key, .created_at = created_at };

    if (self->user_at_top) {
      /* Add directly to notes array */
      g_array_append_val(self->notes, entry);
      added++;

      /* Update timestamps */
      if (created_at > self->newest_timestamp || self->newest_timestamp == 0) {
        self->newest_timestamp = created_at;
      }
      if (created_at < self->oldest_timestamp || self->oldest_timestamp == 0) {
        self->oldest_timestamp = created_at;
      }
    } else {
      /* Add to pending */
      g_array_append_val(self->pending_notes, entry);
      self->pending_count++;
    }
  }

  storage_ndb_end_query(txn);

  if (self->user_at_top && added > 0) {
    /* Sort and emit signal */
    g_array_sort(self->notes, note_entry_compare_newest_first);
    cache_clear(self);
    g_list_model_items_changed(G_LIST_MODEL(self), 0, self->notes->len - added, self->notes->len);
  } else if (self->pending_count > 0) {
    g_signal_emit(self, signals[SIGNAL_NEW_ITEMS_PENDING], 0, self->pending_count);
  }
}

/* ============== Public API ============== */

GnTimelineModel *gn_timeline_model_new(GnTimelineQuery *query) {
  GnTimelineModel *self = g_object_new(GN_TYPE_TIMELINE_MODEL, NULL);
  if (query) {
    self->query = gn_timeline_query_copy(query);
  }
  return self;
}

GnTimelineModel *gn_timeline_model_new_global(void) {
  GnTimelineQuery *query = gn_timeline_query_new_global();
  GnTimelineModel *self = gn_timeline_model_new(query);
  gn_timeline_query_free(query);
  return self;
}

void gn_timeline_model_set_query(GnTimelineModel *self, GnTimelineQuery *query) {
  g_return_if_fail(GN_IS_TIMELINE_MODEL(self));

  gn_timeline_query_free(self->query);
  self->query = query ? gn_timeline_query_copy(query) : NULL;

  gn_timeline_model_refresh(self);
}

GnTimelineQuery *gn_timeline_model_get_query(GnTimelineModel *self) {
  g_return_val_if_fail(GN_IS_TIMELINE_MODEL(self), NULL);
  return self->query;
}

void gn_timeline_model_refresh(GnTimelineModel *self) {
  g_return_if_fail(GN_IS_TIMELINE_MODEL(self));

  guint old_count = self->notes->len;

  /* Clear everything */
  g_array_set_size(self->notes, 0);
  g_array_set_size(self->pending_notes, 0);
  cache_clear(self);
  self->newest_timestamp = 0;
  self->oldest_timestamp = 0;
  self->pending_count = 0;

  /* Query initial items from NostrDB */
  if (self->query) {
    void *txn = NULL;
    if (storage_ndb_begin_query(&txn) == 0 && txn) {
      const char *filter_json = gn_timeline_query_to_json(self->query);
      char **results = NULL;
      int n_results = 0;

      if (storage_ndb_query(txn, filter_json, &results, &n_results) == 0 && n_results > 0) {
        /* Parse results - they're JSON strings, we need note keys */
        /* For now, use subscription to populate */
        storage_ndb_free_results(results, n_results);
      }

      storage_ndb_end_query(txn);
    }
  }

  if (old_count > 0 || self->notes->len > 0) {
    g_list_model_items_changed(G_LIST_MODEL(self), 0, old_count, self->notes->len);
  }
}

void gn_timeline_model_clear(GnTimelineModel *self) {
  g_return_if_fail(GN_IS_TIMELINE_MODEL(self));

  guint old_count = self->notes->len;
  g_array_set_size(self->notes, 0);
  g_array_set_size(self->pending_notes, 0);
  cache_clear(self);
  self->newest_timestamp = 0;
  self->oldest_timestamp = 0;
  self->pending_count = 0;

  if (old_count > 0) {
    g_list_model_items_changed(G_LIST_MODEL(self), 0, old_count, 0);
  }
}

guint gn_timeline_model_load_older(GnTimelineModel *self, guint count) {
  g_return_val_if_fail(GN_IS_TIMELINE_MODEL(self), 0);

  if (!self->query || self->oldest_timestamp == 0) return 0;

  /* Query with until=oldest_timestamp */
  char *filter_json = gn_timeline_query_to_json_with_until(self->query, self->oldest_timestamp - 1);

  void *txn = NULL;
  if (storage_ndb_begin_query(&txn) != 0 || !txn) {
    g_free(filter_json);
    return 0;
  }

  char **results = NULL;
  int n_results = 0;
  guint added = 0;

  if (storage_ndb_query(txn, filter_json, &results, &n_results) == 0 && n_results > 0) {
    /* TODO: Parse JSON results and add to notes array */
    storage_ndb_free_results(results, n_results);
  }

  storage_ndb_end_query(txn);
  g_free(filter_json);

  if (added > 0) {
    g_list_model_items_changed(G_LIST_MODEL(self), self->notes->len - added, 0, added);
  }

  return added;
}

gint64 gn_timeline_model_get_oldest_timestamp(GnTimelineModel *self) {
  g_return_val_if_fail(GN_IS_TIMELINE_MODEL(self), 0);
  return self->oldest_timestamp;
}

gint64 gn_timeline_model_get_newest_timestamp(GnTimelineModel *self) {
  g_return_val_if_fail(GN_IS_TIMELINE_MODEL(self), 0);
  return self->newest_timestamp;
}

void gn_timeline_model_set_user_at_top(GnTimelineModel *self, gboolean at_top) {
  g_return_if_fail(GN_IS_TIMELINE_MODEL(self));
  self->user_at_top = at_top;
}

guint gn_timeline_model_get_pending_count(GnTimelineModel *self) {
  g_return_val_if_fail(GN_IS_TIMELINE_MODEL(self), 0);
  return self->pending_count;
}

void gn_timeline_model_flush_pending(GnTimelineModel *self) {
  g_return_if_fail(GN_IS_TIMELINE_MODEL(self));

  if (self->pending_notes->len == 0) return;

  g_debug("[TIMELINE] Flushing %u pending notes", self->pending_notes->len);

  guint old_count = self->notes->len;
  guint n_pending = self->pending_notes->len;

  /* Sort pending by created_at */
  g_array_sort(self->pending_notes, note_entry_compare_newest_first);

  /* Create new array with pending first, then existing */
  GArray *new_notes = g_array_new(FALSE, FALSE, sizeof(NoteEntry));
  g_array_set_size(new_notes, n_pending + old_count);

  /* Copy pending notes */
  for (guint i = 0; i < n_pending; i++) {
    NoteEntry *entry = &g_array_index(self->pending_notes, NoteEntry, i);
    g_array_index(new_notes, NoteEntry, i) = *entry;

    /* Update timestamps */
    if (entry->created_at > self->newest_timestamp || self->newest_timestamp == 0) {
      self->newest_timestamp = entry->created_at;
    }
  }

  /* Copy existing notes */
  for (guint i = 0; i < old_count; i++) {
    NoteEntry *entry = &g_array_index(self->notes, NoteEntry, i);
    g_array_index(new_notes, NoteEntry, n_pending + i) = *entry;
  }

  /* Swap arrays */
  g_array_free(self->notes, TRUE);
  self->notes = new_notes;

  /* Clear pending */
  g_array_set_size(self->pending_notes, 0);
  self->pending_count = 0;

  /* Clear cache - items will be re-fetched with correct positions */
  cache_clear(self);

  /* Emit single "replace all" signal */
  g_list_model_items_changed(G_LIST_MODEL(self), 0, old_count, self->notes->len);

  /* Clear pending indicator */
  g_signal_emit(self, signals[SIGNAL_NEW_ITEMS_PENDING], 0, (guint)0);
}

void gn_timeline_model_set_visible_range(GnTimelineModel *self, guint start, guint end) {
  g_return_if_fail(GN_IS_TIMELINE_MODEL(self));
  self->visible_start = start;
  self->visible_end = end;
}

void gn_timeline_model_update_profile(GnTimelineModel *self, const char *pubkey_hex) {
  g_return_if_fail(GN_IS_TIMELINE_MODEL(self));
  g_return_if_fail(pubkey_hex != NULL);

  /* Update profile in cache */
  void *txn = NULL;
  if (storage_ndb_begin_query(&txn) != 0 || !txn) return;

  uint8_t pk32[32];
  for (int i = 0; i < 32; i++) {
    unsigned int byte;
    if (sscanf(pubkey_hex + i*2, "%2x", &byte) != 1) {
      storage_ndb_end_query(txn);
      return;
    }
    pk32[i] = (uint8_t)byte;
  }

  char *json = NULL;
  int json_len = 0;
  if (storage_ndb_get_profile_by_pubkey(txn, pk32, &json, &json_len) == 0 && json) {
    GnNostrProfile *profile = gn_nostr_profile_new(pubkey_hex);
    if (profile) {
      gn_nostr_profile_update_from_json(profile, json);
      profile_cache_add(self, pubkey_hex, profile);

      /* Notify items with this pubkey */
      GHashTableIter iter;
      gpointer key, value;
      g_hash_table_iter_init(&iter, self->item_cache);
      while (g_hash_table_iter_next(&iter, &key, &value)) {
        GnNostrEventItem *item = GN_NOSTR_EVENT_ITEM(value);
        const char *item_pubkey = gn_nostr_event_item_get_pubkey(item);
        if (item_pubkey && g_strcmp0(item_pubkey, pubkey_hex) == 0) {
          gn_nostr_event_item_set_profile(item, profile);
          g_object_notify(G_OBJECT(item), "profile");
        }
      }

      g_object_unref(profile);
    }
  }

  storage_ndb_end_query(txn);
}

/* ============== GObject Lifecycle ============== */

static void gn_timeline_model_dispose(GObject *object) {
  GnTimelineModel *self = GN_TIMELINE_MODEL(object);

  /* Cancel pending invalidation */
  if (self->invalidate_source_id > 0) {
    g_source_remove(self->invalidate_source_id);
    self->invalidate_source_id = 0;
  }

  /* Unsubscribe */
  if (self->sub_timeline > 0) {
    gn_ndb_unsubscribe(self->sub_timeline);
    self->sub_timeline = 0;
  }

  /* Clear caches */
  g_clear_pointer(&self->item_cache, g_hash_table_unref);
  g_clear_pointer(&self->cache_lru, g_queue_free);
  g_clear_pointer(&self->profile_cache, g_hash_table_unref);
  if (self->profile_cache_lru) {
    g_queue_free_full(self->profile_cache_lru, g_free);
    self->profile_cache_lru = NULL;
  }

  /* Free arrays */
  g_clear_pointer(&self->notes, g_array_unref);
  g_clear_pointer(&self->pending_notes, g_array_unref);

  /* Free query */
  gn_timeline_query_free(self->query);
  self->query = NULL;

  G_OBJECT_CLASS(gn_timeline_model_parent_class)->dispose(object);
}

static void gn_timeline_model_class_init(GnTimelineModelClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->dispose = gn_timeline_model_dispose;

  signals[SIGNAL_NEW_ITEMS_PENDING] =
    g_signal_new("new-items-pending",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 1, G_TYPE_UINT);

  signals[SIGNAL_NEED_PROFILE] =
    g_signal_new("need-profile",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void gn_timeline_model_init(GnTimelineModel *self) {
  self->notes = g_array_new(FALSE, FALSE, sizeof(NoteEntry));
  self->pending_notes = g_array_new(FALSE, FALSE, sizeof(NoteEntry));

  self->item_cache = g_hash_table_new_full(uint64_hash, uint64_equal, g_free, g_object_unref);
  self->cache_lru = g_queue_new();

  self->profile_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
  self->profile_cache_lru = g_queue_new();

  self->user_at_top = TRUE;

  /* Subscribe to timeline events */
  const char *filter = "{\"kinds\":[1,6]}";
  self->sub_timeline = gn_ndb_subscribe(filter, on_sub_timeline_batch, self, NULL);
}
