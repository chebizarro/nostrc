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
#include <jansson.h>
#include <gtk/gtk.h>

/* Configuration */
#define MODEL_PAGE_SIZE 50          /* Items per query page */
#define MODEL_MAX_CACHED 200        /* Max cached items */
#define PROFILE_CACHE_MAX 500       /* Max cached profiles */
#define UPDATE_DEBOUNCE_MS 50       /* Debounce UI updates during rapid ingestion */
#define MODEL_MAX_WINDOW 1000       /* Max items in model - oldest evicted beyond this */

/* Phase 1: Frame-aware batching (nostrc-0hp) */
#define ITEMS_PER_FRAME_DEFAULT 3   /* Start conservative per design review */
#define FRAME_BUDGET_US 12000       /* 12ms target, leaving 4ms margin for 16.6ms frame */

/* Note entry for internal tracking */
typedef struct {
  uint64_t note_key;
  gint64 created_at;
} NoteEntry;

/* Staged entry for frame-aware batching (nostrc-0hp Phase 1) */
typedef struct {
  uint64_t note_key;
  gint64 created_at;
  gint64 arrival_time_us;  /* Monotonic time when staged, for backpressure */
} StagedEntry;

struct _GnTimelineModel {
  GObject parent_instance;

  /* Query filter */
  GnTimelineQuery *query;

  /* Note keys array - sorted by created_at descending */
  GArray *notes;
  GHashTable *note_key_set;  /* note_key -> TRUE for O(1) dedup lookups */

  /* Timestamps for pagination */
  gint64 newest_timestamp;
  gint64 oldest_timestamp;

  /* Item cache - note_key -> GnNostrEventItem */
  GHashTable *item_cache;
  GQueue *cache_lru;

  /* Profile cache - pubkey -> GnNostrProfile */
  GHashTable *profile_cache;
  GQueue *profile_cache_lru;

  /* New items tracking (for "N new notes" indicator) */
  guint unseen_count;        /* Items at top not yet seen by user */
  gboolean user_at_top;

  /* Batch insertion tracking for debounce */
  GArray *batch_buffer;      /* Temp buffer for sorting incoming items */
  guint batch_insert_count;  /* Items to insert at position 0 */

  /* Update debouncing for crash resistance */
  guint update_debounce_id;
  gboolean needs_refresh;
  guint pending_update_old_count;  /* Count before batch started */
  gboolean in_batch_mode;          /* Suppress signals during initial load */
  guint initial_load_timeout_id;   /* Timer to end initial batch mode */

  /* Visible range for prefetching */
  guint visible_start;
  guint visible_end;

  /* Subscription */
  uint64_t sub_timeline;

  /* Frame-aware batching (nostrc-0hp Phase 1) */
  GArray *staging_buffer;        /* StagedEntry items awaiting frame-synced insertion */
  GHashTable *staged_key_set;    /* note_key -> TRUE for O(1) dedup in staging */
  guint tick_callback_id;        /* gtk_widget_add_tick_callback ID, 0 if inactive */
  guint items_per_frame;         /* Max items to insert per frame (adaptive) */
  GtkWidget *tick_widget;        /* Widget providing frame clock (weak ref) */
};

/* Forward declarations */
static void gn_timeline_model_list_model_iface_init(GListModelInterface *iface);
static void on_sub_timeline_batch(uint64_t subid, const uint64_t *note_keys, guint n_keys, gpointer user_data);
static void gn_timeline_model_schedule_update(GnTimelineModel *self);
static gboolean on_update_debounce_timeout(gpointer user_data);
static gboolean on_initial_load_timeout(gpointer user_data);
static guint enforce_window_size(GnTimelineModel *self, gboolean emit_signal);

/* Frame-aware batching forward declarations (nostrc-0hp Phase 1) */
static gboolean on_tick_callback(GtkWidget *widget, GdkFrameClock *clock, gpointer user_data);
static void on_tick_widget_destroyed(gpointer data, GObject *where_the_object_was);
static void ensure_tick_callback(GnTimelineModel *self);
static void remove_tick_callback(GnTimelineModel *self);
static void process_staged_items(GnTimelineModel *self, guint count);
static gboolean has_note_key_staged(GnTimelineModel *self, uint64_t key);
static void add_note_key_to_staged_set(GnTimelineModel *self, uint64_t key);
static void remove_note_key_from_staged_set(GnTimelineModel *self, uint64_t key);

#define INITIAL_LOAD_TIMEOUT_MS 500  /* Time to wait for initial subscription data */

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

/* ============== Update Debouncing ============== */

static gboolean on_update_debounce_timeout(gpointer user_data) {
  GnTimelineModel *self = GN_TIMELINE_MODEL(user_data);
  if (!GN_IS_TIMELINE_MODEL(self)) return G_SOURCE_REMOVE;

  self->update_debounce_id = 0;

  if (!self->needs_refresh) return G_SOURCE_REMOVE;
  self->needs_refresh = FALSE;

  guint inserted = self->batch_insert_count;
  if (inserted > 0) {
    g_debug("[TIMELINE] Debounced insert: %u items at position 0", inserted);
    /* Emit incremental insert signal - GTK handles scroll position adjustment */
    g_list_model_items_changed(G_LIST_MODEL(self), 0, 0, inserted);
  }

  /* Enforce window size - evict oldest items if needed.
   * Do this AFTER the insert signal so eviction signal is separate. */
  enforce_window_size(self, TRUE);

  /* Reset batch counter for next debounce window */
  self->batch_insert_count = 0;
  self->pending_update_old_count = self->notes->len;

  return G_SOURCE_REMOVE;
}

static void gn_timeline_model_schedule_update(GnTimelineModel *self) {
  g_return_if_fail(GN_IS_TIMELINE_MODEL(self));

  /* Skip if in batch mode - we'll emit signal when batch ends */
  if (self->in_batch_mode) return;

  /* Cancel existing timer and start fresh */
  if (self->update_debounce_id > 0) {
    g_source_remove(self->update_debounce_id);
  }

  self->update_debounce_id = g_timeout_add(
    UPDATE_DEBOUNCE_MS,
    on_update_debounce_timeout,
    self
  );
}

static gboolean on_initial_load_timeout(gpointer user_data) {
  GnTimelineModel *self = GN_TIMELINE_MODEL(user_data);
  if (!GN_IS_TIMELINE_MODEL(self)) return G_SOURCE_REMOVE;

  self->initial_load_timeout_id = 0;

  if (self->in_batch_mode) {
    g_debug("[TIMELINE] Initial load complete, ending batch mode");
    gn_timeline_model_end_batch(self);
  }

  return G_SOURCE_REMOVE;
}

/* ============== Note Helpers ============== */

static gboolean has_note_key(GnTimelineModel *self, uint64_t key) {
  return g_hash_table_contains(self->note_key_set, &key);
}

static void add_note_key_to_set(GnTimelineModel *self, uint64_t key) {
  uint64_t *key_copy = g_new(uint64_t, 1);
  *key_copy = key;
  g_hash_table_add(self->note_key_set, key_copy);
}

static gint note_entry_compare_newest_first(gconstpointer a, gconstpointer b) {
  const NoteEntry *ea = (const NoteEntry *)a;
  const NoteEntry *eb = (const NoteEntry *)b;
  if (ea->created_at > eb->created_at) return -1;
  if (ea->created_at < eb->created_at) return 1;
  return 0;
}

/**
 * enforce_window_size:
 * @self: The model
 * @emit_signal: Whether to emit items_changed signal for evicted items
 *
 * Evicts oldest items if array exceeds MODEL_MAX_WINDOW.
 * Returns the number of items evicted.
 *
 * This ensures bounded memory regardless of scroll history.
 */
static guint enforce_window_size(GnTimelineModel *self, gboolean emit_signal) {
  if (self->notes->len <= MODEL_MAX_WINDOW) return 0;

  guint to_evict = self->notes->len - MODEL_MAX_WINDOW;
  guint evict_start = MODEL_MAX_WINDOW;  /* First item to evict */

  /* Remove evicted items from the note_key_set */
  for (guint i = evict_start; i < self->notes->len; i++) {
    NoteEntry *entry = &g_array_index(self->notes, NoteEntry, i);
    g_hash_table_remove(self->note_key_set, &entry->note_key);
  }

  /* Truncate the array */
  g_array_set_size(self->notes, MODEL_MAX_WINDOW);

  /* Update oldest_timestamp from new last item */
  if (self->notes->len > 0) {
    NoteEntry *last = &g_array_index(self->notes, NoteEntry, self->notes->len - 1);
    self->oldest_timestamp = last->created_at;
  }

  if (emit_signal && to_evict > 0) {
    g_debug("[TIMELINE] Evicted %u oldest items to enforce window size", to_evict);
    g_list_model_items_changed(G_LIST_MODEL(self), evict_start, to_evict, 0);
  }

  return to_evict;
}

/* ============== Frame-Aware Batching (nostrc-0hp Phase 1) ============== */

/**
 * has_note_key_staged:
 * @self: The model
 * @key: Note key to check
 *
 * Check if a note key is already in the staging buffer.
 * O(1) lookup via hash set.
 */
static gboolean has_note_key_staged(GnTimelineModel *self, uint64_t key) {
  if (!self->staged_key_set) return FALSE;
  return g_hash_table_contains(self->staged_key_set, &key);
}

/**
 * add_note_key_to_staged_set:
 * @self: The model
 * @key: Note key to add
 *
 * Add a note key to the staging dedup set.
 */
static void add_note_key_to_staged_set(GnTimelineModel *self, uint64_t key) {
  if (!self->staged_key_set) return;
  uint64_t *key_copy = g_new(uint64_t, 1);
  *key_copy = key;
  g_hash_table_add(self->staged_key_set, key_copy);
}

/**
 * remove_note_key_from_staged_set:
 * @self: The model
 * @key: Note key to remove
 *
 * Remove a note key from the staging dedup set.
 */
static void remove_note_key_from_staged_set(GnTimelineModel *self, uint64_t key) {
  if (!self->staged_key_set) return;
  g_hash_table_remove(self->staged_key_set, &key);
}

/**
 * ensure_tick_callback:
 * @self: The model
 *
 * Ensure a tick callback is registered with the associated view widget.
 * If no widget is associated or widget is not realized, this is a no-op
 * and items will be processed via the legacy debounce path.
 */
static void ensure_tick_callback(GnTimelineModel *self) {
  /* Already have an active tick callback */
  if (self->tick_callback_id != 0)
    return;

  /* No widget associated - fall back to debounce */
  if (!self->tick_widget)
    return;

  /* Widget must be realized to get frame clock */
  if (!gtk_widget_get_realized(self->tick_widget)) {
    g_debug("[FRAME] Widget not realized, deferring tick callback");
    return;
  }

  /*
   * gtk_widget_add_tick_callback handles all lifecycle concerns:
   * - Returns 0 if widget is not realized (we checked above)
   * - Automatically removed when widget is destroyed
   * - Paused when widget is unmapped
   */
  self->tick_callback_id = gtk_widget_add_tick_callback(
    self->tick_widget,
    on_tick_callback,
    g_object_ref(self),
    g_object_unref
  );

  if (self->tick_callback_id != 0) {
    g_debug("[FRAME] Tick callback registered (id=%u)", self->tick_callback_id);
  }
}

/**
 * remove_tick_callback:
 * @self: The model
 *
 * Remove the tick callback if active and clean up widget reference.
 */
static void remove_tick_callback(GnTimelineModel *self) {
  if (self->tick_callback_id != 0 && self->tick_widget) {
    gtk_widget_remove_tick_callback(self->tick_widget, self->tick_callback_id);
  }
  self->tick_callback_id = 0;

  if (self->tick_widget) {
    g_object_weak_unref(G_OBJECT(self->tick_widget),
                        on_tick_widget_destroyed, self);
    self->tick_widget = NULL;
  }
}

/**
 * process_staged_items:
 * @self: The model
 * @count: Maximum number of items to process
 *
 * Move items from staging buffer to main notes array.
 * Items are inserted at position 0 (newest first).
 */
static void process_staged_items(GnTimelineModel *self, guint count) {
  if (!self->staging_buffer || self->staging_buffer->len == 0)
    return;

  guint to_process = MIN(count, self->staging_buffer->len);
  guint actually_processed = 0;

  /*
   * Process items from the front of staging buffer (FIFO order).
   * Each item becomes a NoteEntry and is prepended to notes array.
   */
  for (guint i = 0; i < to_process; i++) {
    StagedEntry *staged = &g_array_index(self->staging_buffer, StagedEntry, i);

    /* Create note entry for main array */
    NoteEntry entry = {
      .note_key = staged->note_key,
      .created_at = staged->created_at
    };

    /* Insert at position 0 (newest first) */
    g_array_prepend_val(self->notes, entry);

    /* Move from staged set to main set */
    remove_note_key_from_staged_set(self, staged->note_key);
    add_note_key_to_set(self, staged->note_key);

    /* Update timestamps */
    if (staged->created_at > self->newest_timestamp || self->newest_timestamp == 0) {
      self->newest_timestamp = staged->created_at;
    }

    actually_processed++;
  }

  /* Remove processed items from staging buffer */
  if (actually_processed > 0) {
    g_array_remove_range(self->staging_buffer, 0, actually_processed);
    g_debug("[FRAME] Processed %u staged items, %u remaining",
            actually_processed, self->staging_buffer->len);
  }
}

/**
 * on_tick_callback:
 * @widget: The widget providing the frame clock
 * @clock: The frame clock (unused but available for timing)
 * @user_data: The GnTimelineModel
 *
 * Called once per frame by GTK. Processes a bounded number of staged items
 * and emits a single batched items_changed signal.
 *
 * Returns: G_SOURCE_CONTINUE if more items pending, G_SOURCE_REMOVE otherwise
 */
static gboolean on_tick_callback(GtkWidget     *widget,
                                  GdkFrameClock *clock,
                                  gpointer       user_data) {
  (void)widget;
  (void)clock;

  GnTimelineModel *self = GN_TIMELINE_MODEL(user_data);
  if (!GN_IS_TIMELINE_MODEL(self)) {
    return G_SOURCE_REMOVE;
  }

  /* Track frame budget */
  gint64 start_us = g_get_monotonic_time();

  guint to_process = MIN(self->staging_buffer->len, self->items_per_frame);

  if (to_process == 0) {
    /* Nothing to do, remove callback until more items arrive */
    g_debug("[FRAME] No staged items, removing tick callback");
    self->tick_callback_id = 0;
    return G_SOURCE_REMOVE;
  }

  /* Process the batch */
  process_staged_items(self, to_process);

  /* Emit single batched signal for all items processed this frame */
  g_list_model_items_changed(G_LIST_MODEL(self), 0, 0, to_process);

  /* Track unseen items if user is scrolled down */
  if (!self->user_at_top) {
    self->unseen_count += to_process;
    g_signal_emit(self, signals[SIGNAL_NEW_ITEMS_PENDING], 0, self->unseen_count);
  }

  /* Enforce window size after insert */
  enforce_window_size(self, TRUE);

  /* Check frame budget */
  gint64 elapsed_us = g_get_monotonic_time() - start_us;

  if (elapsed_us > FRAME_BUDGET_US) {
    g_warning("[FRAME] Budget exceeded: %ldus (budget: %dus, items: %u)",
              (long)elapsed_us, FRAME_BUDGET_US, to_process);

    /* Reduce items_per_frame adaptively */
    if (self->items_per_frame > 1) {
      self->items_per_frame--;
      g_debug("[FRAME] Reduced items_per_frame to %u", self->items_per_frame);
    }
  } else if (elapsed_us < FRAME_BUDGET_US / 2 && self->items_per_frame < 5) {
    /* Well under budget, can potentially increase */
    self->items_per_frame++;
    g_debug("[FRAME] Increased items_per_frame to %u (elapsed: %ldus)",
            self->items_per_frame, (long)elapsed_us);
  }

  /* Continue callback if more items pending */
  if (self->staging_buffer->len > 0) {
    return G_SOURCE_CONTINUE;
  }

  g_debug("[FRAME] All staged items processed, removing tick callback");
  self->tick_callback_id = 0;
  return G_SOURCE_REMOVE;
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

/**
 * staged_entry_compare_newest_first:
 *
 * Comparison function for sorting staged entries by created_at descending.
 */
static gint staged_entry_compare_newest_first(gconstpointer a, gconstpointer b) {
  const StagedEntry *ea = (const StagedEntry *)a;
  const StagedEntry *eb = (const StagedEntry *)b;
  if (ea->created_at > eb->created_at) return -1;
  if (ea->created_at < eb->created_at) return 1;
  return 0;
}

static void on_sub_timeline_batch(uint64_t subid, const uint64_t *note_keys, guint n_keys, gpointer user_data) {
  (void)subid;
  GnTimelineModel *self = GN_TIMELINE_MODEL(user_data);
  if (!GN_IS_TIMELINE_MODEL(self) || !note_keys || n_keys == 0) return;

  void *txn = NULL;
  if (storage_ndb_begin_query(&txn) != 0 || !txn) return;

  /*
   * nostrc-0hp Phase 1: Frame-aware batching
   *
   * If we have a tick widget registered, use the staging buffer and let
   * the tick callback process items frame-by-frame. Otherwise, fall back
   * to the legacy debounce path for backward compatibility.
   */
  gboolean use_frame_batching = (self->tick_widget != NULL &&
                                  self->staging_buffer != NULL);

  /* Legacy path: Capture count at start of first batch in debounce window */
  if (!use_frame_batching && !self->needs_refresh) {
    self->pending_update_old_count = self->notes->len;
  }

  /* Legacy path: Clear batch buffer for this batch */
  if (!use_frame_batching) {
    g_array_set_size(self->batch_buffer, 0);
  }

  guint staged_count = 0;
  gint64 arrival_time_us = g_get_monotonic_time();

  for (guint i = 0; i < n_keys; i++) {
    uint64_t note_key = note_keys[i];

    /* Skip if already have this note in main array */
    if (has_note_key(self, note_key)) continue;

    /* Skip if already staged (frame-aware path only) */
    if (use_frame_batching && has_note_key_staged(self, note_key)) continue;

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

    if (use_frame_batching) {
      /*
       * Frame-aware path: Stage the item for tick callback processing.
       * NO items_changed signal is emitted here - that happens in on_tick_callback().
       */
      StagedEntry staged = {
        .note_key = note_key,
        .created_at = created_at,
        .arrival_time_us = arrival_time_us
      };
      g_array_append_val(self->staging_buffer, staged);
      add_note_key_to_staged_set(self, note_key);
      staged_count++;
    } else {
      /* Legacy path: Add to batch buffer for debounced insert */
      NoteEntry entry = { .note_key = note_key, .created_at = created_at };
      g_array_append_val(self->batch_buffer, entry);
      add_note_key_to_set(self, note_key);

      /* Update timestamps immediately for legacy path */
      if (created_at > self->newest_timestamp || self->newest_timestamp == 0) {
        self->newest_timestamp = created_at;
      }
      if (created_at < self->oldest_timestamp || self->oldest_timestamp == 0) {
        self->oldest_timestamp = created_at;
      }
    }
  }

  storage_ndb_end_query(txn);

  if (use_frame_batching) {
    /*
     * Frame-aware path: Sort staging buffer and schedule tick callback.
     * The tick callback will emit items_changed signals, not us.
     */
    if (staged_count > 0) {
      /* Sort by created_at descending so newest are processed first */
      g_array_sort(self->staging_buffer, staged_entry_compare_newest_first);

      g_debug("[FRAME] Staged %u items (total pending: %u)",
              staged_count, self->staging_buffer->len);

      /* Ensure tick callback is running */
      ensure_tick_callback(self);
    }
  } else {
    /* Legacy debounce path */
    guint batch_count = self->batch_buffer->len;
    if (batch_count > 0) {
      /* Sort batch by created_at descending (newest first) */
      g_array_sort(self->batch_buffer, note_entry_compare_newest_first);

      /* Insert sorted batch at position 0 */
      g_array_prepend_vals(self->notes, self->batch_buffer->data, batch_count);

      /* Track batch for debounced signal emission */
      self->batch_insert_count += batch_count;

      /* If user is not at top, these are "unseen" items */
      if (!self->user_at_top) {
        self->unseen_count += batch_count;
      }

      /* Schedule debounced UI update */
      self->needs_refresh = TRUE;
      gn_timeline_model_schedule_update(self);

      /* Emit pending signal for toast if user is scrolled down */
      if (!self->user_at_top && self->unseen_count > 0) {
        g_signal_emit(self, signals[SIGNAL_NEW_ITEMS_PENDING], 0, self->unseen_count);
      }
    }
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
  g_array_set_size(self->batch_buffer, 0);
  g_array_set_size(self->staging_buffer, 0);
  g_hash_table_remove_all(self->note_key_set);
  g_hash_table_remove_all(self->staged_key_set);
  cache_clear(self);
  self->newest_timestamp = 0;
  self->oldest_timestamp = 0;
  self->unseen_count = 0;
  self->batch_insert_count = 0;

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
  g_array_set_size(self->batch_buffer, 0);
  g_array_set_size(self->staging_buffer, 0);
  g_hash_table_remove_all(self->note_key_set);
  g_hash_table_remove_all(self->staged_key_set);
  cache_clear(self);
  self->newest_timestamp = 0;
  self->oldest_timestamp = 0;
  self->unseen_count = 0;
  self->batch_insert_count = 0;

  if (old_count > 0) {
    g_list_model_items_changed(G_LIST_MODEL(self), 0, old_count, 0);
  }
}

guint gn_timeline_model_load_older(GnTimelineModel *self, guint count) {
  g_return_val_if_fail(GN_IS_TIMELINE_MODEL(self), 0);
  (void)count;  /* Currently loads whatever the query returns */

  if (!self->query || self->oldest_timestamp == 0) return 0;

  /* Query with until=oldest_timestamp-1 to get older items */
  char *filter_json = gn_timeline_query_to_json_with_until(self->query, self->oldest_timestamp - 1);
  if (!filter_json) return 0;

  void *txn = NULL;
  if (storage_ndb_begin_query(&txn) != 0 || !txn) {
    g_free(filter_json);
    return 0;
  }

  char **results = NULL;
  int n_results = 0;
  guint added = 0;

  if (storage_ndb_query(txn, filter_json, &results, &n_results) == 0 && n_results > 0) {
    g_debug("[TIMELINE] load_older: got %d results", n_results);

    for (int i = 0; i < n_results; i++) {
      if (!results[i]) continue;

      /* Parse JSON to extract event ID and created_at */
      json_error_t err;
      json_t *event = json_loads(results[i], 0, &err);
      if (!event) {
        g_debug("[TIMELINE] Failed to parse JSON result %d: %s", i, err.text);
        continue;
      }

      const char *id_hex = json_string_value(json_object_get(event, "id"));
      json_t *created_at_json = json_object_get(event, "created_at");
      gint64 created_at = created_at_json ? json_integer_value(created_at_json) : 0;

      if (!id_hex || strlen(id_hex) != 64) {
        json_decref(event);
        continue;
      }

      /* Convert hex ID to binary - must convert all 32 bytes */
      unsigned char id32[32];
      int bytes_converted = 0;
      for (int j = 0; j < 32; j++) {
        unsigned int byte;
        if (sscanf(id_hex + j*2, "%2x", &byte) != 1) {
          break;
        }
        id32[j] = (unsigned char)byte;
        bytes_converted++;
      }
      if (bytes_converted != 32) {
        g_debug("[TIMELINE] Incomplete hex conversion: only %d/32 bytes", bytes_converted);
        json_decref(event);
        continue;
      }

      /* Get note_key from ID */
      uint64_t note_key = storage_ndb_get_note_key_by_id(txn, id32, NULL);
      if (note_key == 0) {
        json_decref(event);
        continue;
      }

      /* Skip if already have this note */
      if (has_note_key(self, note_key)) {
        json_decref(event);
        continue;
      }

      /* Check mute list */
      const char *pubkey_hex = json_string_value(json_object_get(event, "pubkey"));
      if (pubkey_hex) {
        GnostrMuteList *mute_list = gnostr_mute_list_get_default();
        if (mute_list && gnostr_mute_list_is_pubkey_muted(mute_list, pubkey_hex)) {
          json_decref(event);
          continue;
        }
      }

      /* Add to notes array at the end (older items) */
      NoteEntry entry = { .note_key = note_key, .created_at = created_at };
      g_array_append_val(self->notes, entry);
      add_note_key_to_set(self, note_key);
      added++;

      /* Update oldest timestamp */
      if (created_at < self->oldest_timestamp || self->oldest_timestamp == 0) {
        self->oldest_timestamp = created_at;
      }

      json_decref(event);
    }

    storage_ndb_free_results(results, n_results);
  }

  storage_ndb_end_query(txn);
  g_free(filter_json);

  if (added > 0) {
    guint old_count = self->notes->len - added;
    /*
     * Sort only the newly added items (they're all at the end, all older than existing).
     * This is safer than sorting the entire array and won't disturb existing positions.
     */
    if (added > 1) {
      /* Sort just the appended portion */
      NoteEntry *new_start = &g_array_index(self->notes, NoteEntry, old_count);
      qsort(new_start, added, sizeof(NoteEntry),
            (int (*)(const void *, const void *))note_entry_compare_newest_first);
    }
    /*
     * Emit incremental append signal instead of replace-all.
     * This tells GTK to create widgets only for new items at the end,
     * without disturbing existing widgets - much safer during scroll.
     */
    g_list_model_items_changed(G_LIST_MODEL(self), old_count, 0, added);
    g_debug("[TIMELINE] load_older: appended %u items at position %u", added, old_count);

    /* Enforce window size - evict oldest items if we exceeded the max */
    enforce_window_size(self, TRUE);
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

  gboolean was_at_top = self->user_at_top;
  self->user_at_top = at_top;

  /* When user scrolls to top, mark all items as seen */
  if (at_top && !was_at_top && self->unseen_count > 0) {
    self->unseen_count = 0;
    g_signal_emit(self, signals[SIGNAL_NEW_ITEMS_PENDING], 0, (guint)0);
  }
}

guint gn_timeline_model_get_pending_count(GnTimelineModel *self) {
  g_return_val_if_fail(GN_IS_TIMELINE_MODEL(self), 0);
  return self->unseen_count;
}

void gn_timeline_model_flush_pending(GnTimelineModel *self) {
  g_return_if_fail(GN_IS_TIMELINE_MODEL(self));

  /*
   * New design: Items are already inserted at position 0 as they arrive.
   * "Flush" just clears the unseen count - no data manipulation needed.
   * This makes clicking "New Notes" instant (< 100ms latency).
   */
  if (self->unseen_count == 0) return;

  g_debug("[TIMELINE] Marking %u notes as seen (instant flush)", self->unseen_count);

  self->unseen_count = 0;

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

/* ============== Batch Mode ============== */

void gn_timeline_model_begin_batch(GnTimelineModel *self) {
  g_return_if_fail(GN_IS_TIMELINE_MODEL(self));

  if (self->in_batch_mode) return;  /* Already in batch mode */

  self->in_batch_mode = TRUE;
  self->pending_update_old_count = self->notes->len;

  /* Cancel any pending debounce since we're now in batch mode */
  if (self->update_debounce_id > 0) {
    g_source_remove(self->update_debounce_id);
    self->update_debounce_id = 0;
  }

  g_debug("[TIMELINE] Begin batch mode (current count: %u)", self->pending_update_old_count);
}

void gn_timeline_model_end_batch(GnTimelineModel *self) {
  g_return_if_fail(GN_IS_TIMELINE_MODEL(self));

  if (!self->in_batch_mode) return;  /* Not in batch mode */

  self->in_batch_mode = FALSE;

  guint old_count = self->pending_update_old_count;
  guint new_count = self->notes->len;

  g_debug("[TIMELINE] End batch mode: %u -> %u items", old_count, new_count);

  /* Emit a single "replace all" signal for all accumulated changes */
  if (old_count != new_count) {
    g_list_model_items_changed(G_LIST_MODEL(self), 0, old_count, new_count);
  }

  /* Reset for future batches */
  self->pending_update_old_count = new_count;
  self->needs_refresh = FALSE;
}

/* ============== Frame-Aware Batching Public API (nostrc-0hp Phase 1) ============== */

/**
 * Weak notify callback when the tick widget is destroyed.
 */
static void on_tick_widget_destroyed(gpointer data, GObject *where_the_object_was) {
  (void)where_the_object_was;
  GnTimelineModel *self = GN_TIMELINE_MODEL(data);
  if (!GN_IS_TIMELINE_MODEL(self)) return;

  g_debug("[FRAME] Tick widget destroyed, disabling frame-aware batching");
  self->tick_widget = NULL;
  self->tick_callback_id = 0;
}

void gn_timeline_model_set_view_widget(GnTimelineModel *self, GtkWidget *widget) {
  g_return_if_fail(GN_IS_TIMELINE_MODEL(self));

  /* Same widget, nothing to do */
  if (self->tick_widget == widget) return;

  /* Clean up old widget reference */
  if (self->tick_widget) {
    /* Remove tick callback if active */
    if (self->tick_callback_id != 0) {
      gtk_widget_remove_tick_callback(self->tick_widget, self->tick_callback_id);
      self->tick_callback_id = 0;
    }
    /* Remove weak reference */
    g_object_weak_unref(G_OBJECT(self->tick_widget),
                        on_tick_widget_destroyed, self);
    self->tick_widget = NULL;
  }

  /* Set up new widget reference */
  if (widget) {
    self->tick_widget = widget;
    g_object_weak_ref(G_OBJECT(widget), on_tick_widget_destroyed, self);

    g_debug("[FRAME] View widget set, enabling frame-aware batching");

    /* If there are already staged items, start the tick callback */
    if (self->staging_buffer && self->staging_buffer->len > 0) {
      ensure_tick_callback(self);
    }
  } else {
    g_debug("[FRAME] View widget cleared, disabling frame-aware batching");
  }
}

guint gn_timeline_model_get_staged_count(GnTimelineModel *self) {
  g_return_val_if_fail(GN_IS_TIMELINE_MODEL(self), 0);
  if (!self->staging_buffer) return 0;
  return self->staging_buffer->len;
}

/* ============== GObject Lifecycle ============== */

static void gn_timeline_model_dispose(GObject *object) {
  GnTimelineModel *self = GN_TIMELINE_MODEL(object);

  /* Cancel pending update debounce */
  if (self->update_debounce_id > 0) {
    g_source_remove(self->update_debounce_id);
    self->update_debounce_id = 0;
  }

  /* Cancel initial load timer */
  if (self->initial_load_timeout_id > 0) {
    g_source_remove(self->initial_load_timeout_id);
    self->initial_load_timeout_id = 0;
  }

  /* Frame-aware batching cleanup (nostrc-0hp Phase 1) */
  if (self->tick_widget) {
    if (self->tick_callback_id != 0) {
      gtk_widget_remove_tick_callback(self->tick_widget, self->tick_callback_id);
      self->tick_callback_id = 0;
    }
    g_object_weak_unref(G_OBJECT(self->tick_widget),
                        on_tick_widget_destroyed, self);
    self->tick_widget = NULL;
  }
  g_clear_pointer(&self->staging_buffer, g_array_unref);
  g_clear_pointer(&self->staged_key_set, g_hash_table_unref);

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

  /* Free arrays and hash set */
  g_clear_pointer(&self->notes, g_array_unref);
  g_clear_pointer(&self->batch_buffer, g_array_unref);
  g_clear_pointer(&self->note_key_set, g_hash_table_unref);

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
  self->batch_buffer = g_array_new(FALSE, FALSE, sizeof(NoteEntry));
  self->note_key_set = g_hash_table_new_full(uint64_hash, uint64_equal, g_free, NULL);

  self->item_cache = g_hash_table_new_full(uint64_hash, uint64_equal, g_free, g_object_unref);
  self->cache_lru = g_queue_new();

  self->profile_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
  self->profile_cache_lru = g_queue_new();

  self->user_at_top = TRUE;

  /* Frame-aware batching initialization (nostrc-0hp Phase 1) */
  self->staging_buffer = g_array_new(FALSE, FALSE, sizeof(StagedEntry));
  self->staged_key_set = g_hash_table_new_full(uint64_hash, uint64_equal, g_free, NULL);
  self->items_per_frame = ITEMS_PER_FRAME_DEFAULT;
  self->tick_callback_id = 0;
  self->tick_widget = NULL;

  /* Start in batch mode to prevent widget recycling storms during initial load */
  self->in_batch_mode = TRUE;
  self->pending_update_old_count = 0;

  /* Subscribe to timeline events */
  const char *filter = "{\"kinds\":[1,6]}";
  self->sub_timeline = gn_ndb_subscribe(filter, on_sub_timeline_batch, self, NULL);

  /* Schedule end of batch mode after initial load window */
  self->initial_load_timeout_id = g_timeout_add(
    INITIAL_LOAD_TIMEOUT_MS,
    on_initial_load_timeout,
    self
  );
}
