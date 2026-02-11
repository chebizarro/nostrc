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
#include "../ui/gnostr-profile-provider.h"
#include "../util/mute_list.h"
#include <string.h>
#include <stdio.h>
#include "nostr_json.h"
#include <json.h>
#include <gtk/gtk.h>

/* Configuration */
#define MODEL_PAGE_SIZE 50          /* Items per query page */
#define MODEL_MAX_CACHED 200        /* Max cached items */
#define PROFILE_CACHE_MAX 500       /* Max cached profiles */
#define UPDATE_DEBOUNCE_MS 50       /* Debounce UI updates during rapid ingestion */
#define MODEL_MAX_WINDOW 1000       /* Max items in model - oldest evicted beyond this */

/* Frame-aware batching */
#define ITEMS_PER_FRAME_DEFAULT 3   /* Start conservative per design review */
#define FRAME_BUDGET_US 12000       /* 12ms target, leaving 4ms margin for 16.6ms frame */

/* Insertion buffer pipeline */
#define INSERTION_BUFFER_MAX 100    /* Max items in insertion buffer before backpressure */

/* Smooth "New Notes" Reveal Animation */
#define REVEAL_ITEMS_PER_BATCH 3    /* Items revealed per tick callback frame */
#define REVEAL_ANIMATION_MS 200     /* CSS fade-in duration per item */

/* Signal throttling: avoid per-frame toast/label updates */
#define PENDING_SIGNAL_INTERVAL_US 250000  /* 250ms between new-items-pending emissions */
#define EVICT_DEFER_FRAMES 30              /* Only enforce window size every 30 frames (~500ms) */

/* Note entry for internal tracking */
typedef struct {
  uint64_t note_key;
  gint64 created_at;
} NoteEntry;

/* Pending entry for frame-aware batching */
typedef struct {
  uint64_t note_key;
  gint64 created_at;
  gint64 arrival_time_us;  /* Monotonic time when queued, for backpressure */
} PendingEntry;

struct _GnTimelineModel {
  GObject parent_instance;

  /* Query filter */
  GnTimelineQuery *query;

  /* Note keys array - sorted by created_at ascending (oldest at 0, newest at end).
   * Logical GListModel position is reversed in get_item(): position 0 = newest. */
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
  gint64 last_pending_signal_us; /* Throttle SIGNAL_NEW_ITEMS_PENDING emission */
  guint evict_defer_counter;     /* Defer window eviction to avoid replace-all every frame */

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

  /* Frame-aware batching: single insertion buffer pipeline (hq-fnuqs).
   * NDB worker thread -> insertion_buffer -> tick callback -> notes. */
  GArray *insertion_buffer;        /* PendingEntry items awaiting frame-synced insertion */
  GHashTable *insertion_key_set;   /* note_key -> TRUE for O(1) dedup in insertion buffer */
  guint tick_callback_id;          /* gtk_widget_add_tick_callback ID, 0 if inactive */
  guint items_per_frame;           /* Max items to insert per frame (adaptive) */
  GtkWidget *tick_widget;          /* Widget providing frame clock (weak ref) */

  /* Backpressure tracking */
  guint peak_insertion_depth;      /* High-water mark for monitoring */
  gboolean backpressure_active;    /* TRUE when backpressure is being applied */

  /* Smooth "New Notes" Reveal Animation */
  GArray *reveal_queue;          /* Items being revealed with animation */
  guint reveal_position;         /* Current position in reveal sequence */
  gboolean reveal_in_progress;   /* TRUE while reveal animation is active */
  GFunc reveal_complete_cb;      /* Callback when reveal finishes */
  gpointer reveal_complete_data; /* User data for completion callback */
  GHashTable *revealing_keys;    /* note_key -> gint64* (monotonic start time in usec) */
};

/* Forward declarations */
static void gn_timeline_model_list_model_iface_init(GListModelInterface *iface);
static void on_sub_timeline_batch(uint64_t subid, const uint64_t *note_keys, guint n_keys, gpointer user_data);
static void gn_timeline_model_schedule_update(GnTimelineModel *self);
static gboolean on_update_debounce_timeout(gpointer user_data);
static gboolean on_end_batch_mode_idle(gpointer user_data);
static guint enforce_window_size(GnTimelineModel *self, gboolean emit_signal);

/* Frame-aware insertion buffer forward declarations */
static gboolean on_tick_callback(GtkWidget *widget, GdkFrameClock *clock, gpointer user_data);
static void on_tick_widget_destroyed(gpointer data, GObject *where_the_object_was);
static void ensure_tick_callback(GnTimelineModel *self);
static void remove_tick_callback(GnTimelineModel *self);
static void process_pending_items(GnTimelineModel *self, guint count);
static gboolean has_note_key_pending(GnTimelineModel *self, uint64_t key);
static void add_note_key_to_insertion_set(GnTimelineModel *self, uint64_t key);
static void remove_note_key_from_insertion_set(GnTimelineModel *self, uint64_t key);
static void apply_insertion_backpressure(GnTimelineModel *self);
static gint pending_entry_compare_newest_first(gconstpointer a, gconstpointer b);

/* Smooth "New Notes" Reveal Animation forward declarations */
static void process_reveal_batch(GnTimelineModel *self);
static void cancel_reveal_animation(GnTimelineModel *self);
static void mark_key_revealing(GnTimelineModel *self, uint64_t key);
static gboolean is_key_revealing(GnTimelineModel *self, uint64_t key);
static guint sweep_revealing_keys(GnTimelineModel *self);
static gboolean has_active_reveals(GnTimelineModel *self);

/* GTask worker thread for NDB batch processing */
typedef struct {
  /* Input — set by on_sub_timeline_batch(), read by worker thread */
  uint64_t *note_keys;        /* Owned copy of incoming note_keys array */
  guint n_keys;
  gint *kinds;                /* Copied from query (NULL = no filter) */
  gsize n_kinds;

  /* Output — written by worker thread, read by main-thread callback */
  GArray *validated;           /* Array of NoteEntry (note_key, created_at) */
  GPtrArray *prefetch_pubkeys; /* Unique pubkey hex strings for profile prefetch */
} BatchProcessData;

static void batch_process_data_free(gpointer data);
static void batch_process_thread_func(GTask *task, gpointer source_object,
                                       gpointer task_data, GCancellable *cancellable);
static void batch_process_complete_cb(GObject *source_object, GAsyncResult *res,
                                       gpointer user_data);

/* REMOVED: INITIAL_LOAD_TIMEOUT_MS - Batch mode is now ended reactively via idle callback
 * when the first notes arrive, not via a fixed timeout. See on_sub_timeline_batch. */

G_DEFINE_TYPE_WITH_CODE(GnTimelineModel, gn_timeline_model, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(G_TYPE_LIST_MODEL, gn_timeline_model_list_model_iface_init))

enum {
  SIGNAL_NEW_ITEMS_PENDING,
  SIGNAL_NEED_PROFILE,
  SIGNAL_BACKPRESSURE_APPLIED,
  SIGNAL_REVEAL_PROGRESS,     /* Emitted during animated reveal */
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
    /* Enforce window size SILENTLY before emitting signal (nostrc-2n7).
     * Two sequential items_changed signals break GTK's widget cache. */
    guint evicted = enforce_window_size(self, FALSE);

    if (evicted > 0) {
      /* Prepend + tail eviction can't be a single positional signal.
       * Use replace-all: items_changed(0, old_total, new_total) (nostrc-2n7). */
      guint gtk_old = self->pending_update_old_count;
      g_list_model_items_changed(G_LIST_MODEL(self), 0, gtk_old, self->notes->len);
      g_debug("[TIMELINE] Debounced insert+evict: added %u, evicted %u (replace-all)",
              inserted, evicted);
    } else {
      g_list_model_items_changed(G_LIST_MODEL(self), 0, 0, inserted);
      g_debug("[TIMELINE] Debounced insert: %u items at position 0", inserted);
    }
  }

  /* Reset batch counter for next debounce window */
  self->batch_insert_count = 0;
  self->pending_update_old_count = self->notes->len;

  return G_SOURCE_REMOVE;
}

/**
 * on_end_batch_mode_idle:
 * Idle callback that ends batch mode after first notes arrive.
 * Using an idle callback ensures GTK has processed any pending events
 * before we emit the items_changed signal.
 */
static gboolean on_end_batch_mode_idle(gpointer user_data) {
  GnTimelineModel *self = GN_TIMELINE_MODEL(user_data);
  if (!GN_IS_TIMELINE_MODEL(self)) return G_SOURCE_REMOVE;

  if (self->in_batch_mode) {
    g_debug("[TIMELINE] Ending batch mode via idle callback (reactive, no timeout)");
    gn_timeline_model_end_batch(self);
  }

  /* Clear the timeout ID since this was a one-shot callback */
  self->initial_load_timeout_id = 0;
  return G_SOURCE_REMOVE;
}

static void gn_timeline_model_schedule_update(GnTimelineModel *self) {
  g_return_if_fail(GN_IS_TIMELINE_MODEL(self));

  /* Skip if in batch mode - we'll emit signal when batch ends */
  if (self->in_batch_mode) return;

  /* LEGITIMATE TIMEOUT - Debounce model updates for batching.
   * nostrc-b0h: Audited - batching UI updates is appropriate. */
  if (self->update_debounce_id > 0) {
    g_source_remove(self->update_debounce_id);
  }

  self->update_debounce_id = g_timeout_add(
    UPDATE_DEBOUNCE_MS,
    on_update_debounce_timeout,
    self
  );
}

/* REMOVED: on_initial_load_timeout - replaced by on_end_batch_mode_idle.
 * Batch mode is now ended reactively when first notes arrive via an idle
 * callback, not via a fixed timeout. This eliminates artificial delays. */

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

static gint note_entry_compare_oldest_first(gconstpointer a, gconstpointer b) {
  const NoteEntry *ea = (const NoteEntry *)a;
  const NoteEntry *eb = (const NoteEntry *)b;
  if (ea->created_at < eb->created_at) return -1;
  if (ea->created_at > eb->created_at) return 1;
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

  /* Physical array: oldest at index 0.  Evict from the front. */
  for (guint i = 0; i < to_evict; i++) {
    NoteEntry *entry = &g_array_index(self->notes, NoteEntry, i);
    g_hash_table_remove(self->note_key_set, &entry->note_key);
  }

  /* Remove the oldest items from the front */
  g_array_remove_range(self->notes, 0, to_evict);

  /* Update oldest_timestamp from new first item (oldest remaining) */
  if (self->notes->len > 0) {
    NoteEntry *first = &g_array_index(self->notes, NoteEntry, 0);
    self->oldest_timestamp = first->created_at;
  }

  if (emit_signal && to_evict > 0) {
    g_debug("[TIMELINE] Evicted %u oldest items to enforce window size", to_evict);
    /* Evicted items were at logical positions MODEL_MAX_WINDOW onward (bottom of list) */
    g_list_model_items_changed(G_LIST_MODEL(self), MODEL_MAX_WINDOW, to_evict, 0);
  }

  return to_evict;
}

/* ============== Insertion Buffer Pipeline ============== */

/**
 * has_note_key_pending:
 * @self: The model
 * @key: Note key to check
 *
 * Check if a note key is already in the insertion buffer.
 * O(1) lookup via hash set.
 */
static gboolean has_note_key_pending(GnTimelineModel *self, uint64_t key) {
  if (!self->insertion_key_set) return FALSE;
  return g_hash_table_contains(self->insertion_key_set, &key);
}

/**
 * add_note_key_to_insertion_set:
 * @self: The model
 * @key: Note key to add
 *
 * Add a note key to the insertion buffer dedup set.
 */
static void add_note_key_to_insertion_set(GnTimelineModel *self, uint64_t key) {
  if (!self->insertion_key_set) return;
  uint64_t *key_copy = g_new(uint64_t, 1);
  *key_copy = key;
  g_hash_table_add(self->insertion_key_set, key_copy);
}

/**
 * remove_note_key_from_insertion_set:
 * @self: The model
 * @key: Note key to remove
 *
 * Remove a note key from the insertion buffer dedup set.
 */
static void remove_note_key_from_insertion_set(GnTimelineModel *self, uint64_t key) {
  if (!self->insertion_key_set) return;
  g_hash_table_remove(self->insertion_key_set, &key);
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
 * process_pending_items:
 * @self: The model
 * @count: Maximum number of items to process
 *
 * Move items from insertion buffer to main notes array.
 * Items are inserted at position 0 (newest first).
 */
static void process_pending_items(GnTimelineModel *self, guint count) {
  if (!self->insertion_buffer || self->insertion_buffer->len == 0)
    return;

  guint to_process = MIN(count, self->insertion_buffer->len);
  guint actually_processed = 0;

  /*
   * Process items from the front of insertion buffer (FIFO order).
   * Each item becomes a NoteEntry and is appended to the notes array.
   */
  for (guint i = 0; i < to_process; i++) {
    PendingEntry *pending = &g_array_index(self->insertion_buffer, PendingEntry, i);

    /* Create note entry for main array */
    NoteEntry entry = {
      .note_key = pending->note_key,
      .created_at = pending->created_at
    };

    /* Append to end — O(1) amortized.  The physical array stores items
     * in chronological order (oldest at index 0, newest at end).  The
     * logical GListModel position is reversed in get_item(). */
    g_array_append_val(self->notes, entry);

    /* Move from insertion set to main set */
    remove_note_key_from_insertion_set(self, pending->note_key);
    add_note_key_to_set(self, pending->note_key);

    /* Update timestamps */
    if (pending->created_at > self->newest_timestamp || self->newest_timestamp == 0) {
      self->newest_timestamp = pending->created_at;
    }

    actually_processed++;
  }

  /* Remove processed items from insertion buffer */
  if (actually_processed > 0) {
    g_array_remove_range(self->insertion_buffer, 0, actually_processed);
    g_debug("[FRAME] Processed %u pending items, %u remaining",
            actually_processed, self->insertion_buffer->len);
  }
}

/**
 * on_tick_callback:
 * @widget: The widget providing the frame clock
 * @clock: The frame clock (unused but available for timing)
 * @user_data: The GnTimelineModel
 *
 * Called once per frame by GTK. Processes a bounded number of pending items
 * from the insertion buffer and emits a single batched items_changed signal.
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

  /* --- Phase 1: Process reveal queue (animated reveal batching) --- */
  if (self->reveal_in_progress && self->reveal_queue &&
      self->reveal_position < self->reveal_queue->len) {
    process_reveal_batch(self);

    /* Check if reveal is complete */
    if (self->reveal_position >= self->reveal_queue->len) {
      g_debug("[REVEAL] Animation complete, %u items revealed", self->reveal_queue->len);

      /* Clear reveal state */
      g_array_set_size(self->reveal_queue, 0);
      self->reveal_in_progress = FALSE;
      self->reveal_position = 0;

      /* Deferred eviction: trim window once after all items revealed. */
      guint evicted = enforce_window_size(self, TRUE);
      if (evicted > 0) {
        g_debug("[REVEAL] Post-reveal eviction: %u items", evicted);
      }

      /* Clear unseen count since all items are now revealed */
      self->unseen_count = 0;
      g_signal_emit(self, signals[SIGNAL_NEW_ITEMS_PENDING], 0, (guint)0);

      /* Invoke completion callback */
      if (self->reveal_complete_cb) {
        GFunc cb = self->reveal_complete_cb;
        gpointer data = self->reveal_complete_data;
        self->reveal_complete_cb = NULL;
        self->reveal_complete_data = NULL;
        cb(self, data);
      }
    }
  }

  /* --- Phase 2: Process pending items from insertion buffer --- */
  guint to_process = MIN(self->insertion_buffer->len, self->items_per_frame);

  if (to_process > 0) {
    /* Process the batch */
    guint gtk_old_count = self->notes->len;
    process_pending_items(self, to_process);

    /* Defer window eviction to avoid the expensive replace-all signal on every
     * frame.  When the model is at capacity, every prepend evicts from the tail,
     * which forces g_list_model_items_changed(0, old_count, new_count) — a full
     * model replacement that makes GTK rebind every visible widget.  By deferring
     * eviction to every EVICT_DEFER_FRAMES frames, we emit the cheap prepend
     * signal (0, 0, N) most of the time.  The model temporarily exceeds
     * MODEL_MAX_WINDOW by at most EVICT_DEFER_FRAMES * items_per_frame items. */
    self->evict_defer_counter++;
    gboolean do_evict = (self->evict_defer_counter >= EVICT_DEFER_FRAMES) ||
                         (self->insertion_buffer->len == 0); /* last batch: clean up */
    guint evicted = 0;
    if (do_evict) {
      evicted = enforce_window_size(self, FALSE);
      self->evict_defer_counter = 0;
    }

    /* Emit single atomic signal */
    if (evicted > 0) {
      /* Prepend + tail eviction: replace-all signal (nostrc-2n7) */
      g_list_model_items_changed(G_LIST_MODEL(self), 0, gtk_old_count, self->notes->len);
      g_debug("[FRAME] Processed %u items, evicted %u (replace-all)", to_process, evicted);
    } else {
      g_list_model_items_changed(G_LIST_MODEL(self), 0, 0, to_process);
    }

    /* Track unseen items if user is scrolled down.
     * Throttle the signal emission to avoid triggering the toast label/revealer
     * update (g_strdup_printf + gtk_label_set_text + gtk_revealer_set_reveal_child)
     * on every 16ms frame — the user can't perceive count changes at 60fps. */
    if (!self->user_at_top) {
      self->unseen_count += to_process;
      gboolean is_last_batch = (self->insertion_buffer->len == 0);
      if (is_last_batch ||
          (start_us - self->last_pending_signal_us >= PENDING_SIGNAL_INTERVAL_US)) {
        self->last_pending_signal_us = start_us;
        g_signal_emit(self, signals[SIGNAL_NEW_ITEMS_PENDING], 0, self->unseen_count);
      }
    }
  }

  /* --- Phase 3: Sweep revealing keys that have expired --- */
  sweep_revealing_keys(self);

  /* --- Phase 4: Adaptive frame budget (only when processing insertions) --- */
  if (to_process > 0) {
    gint64 elapsed_us = g_get_monotonic_time() - start_us;

    if (elapsed_us > FRAME_BUDGET_US) {
      g_debug("[FRAME] Budget exceeded: %ldus (budget: %dus, items: %u)",
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
  }

  /* --- Continue or remove: keep alive while there is any work remaining --- */
  gboolean has_pending = (self->insertion_buffer->len > 0);
  gboolean has_reveals = has_active_reveals(self);
  gboolean has_reveal_queue = (self->reveal_in_progress &&
                                self->reveal_queue &&
                                self->reveal_position < self->reveal_queue->len);

  if (has_pending || has_reveals || has_reveal_queue) {
    return G_SOURCE_CONTINUE;
  }

  g_debug("[FRAME] All work complete, removing tick callback");
  self->tick_callback_id = 0;
  return G_SOURCE_REMOVE;
}

/* ============== Insertion Buffer Backpressure ============== */

/**
 * apply_insertion_backpressure:
 * @self: The model
 *
 * Apply backpressure when insertion buffer exceeds capacity.
 * Strategy: Drop oldest items (at the end of the newest-first sorted buffer)
 * to stay within INSERTION_BUFFER_MAX.
 */
static void apply_insertion_backpressure(GnTimelineModel *self) {
  if (!self->insertion_buffer || self->insertion_buffer->len <= INSERTION_BUFFER_MAX)
    return;

  guint to_drop = self->insertion_buffer->len - INSERTION_BUFFER_MAX;

  g_debug("[BACKPRESSURE] Dropping %u oldest items from insertion buffer (%u -> %u)",
          to_drop, self->insertion_buffer->len, INSERTION_BUFFER_MAX);

  /* Insertion buffer is sorted newest-first, so oldest items are at the end.
   * Remove from the tail to drop the oldest. */
  for (guint i = self->insertion_buffer->len - to_drop; i < self->insertion_buffer->len; i++) {
    PendingEntry *entry = &g_array_index(self->insertion_buffer, PendingEntry, i);
    remove_note_key_from_insertion_set(self, entry->note_key);
  }
  g_array_remove_range(self->insertion_buffer, self->insertion_buffer->len - to_drop, to_drop);

  self->backpressure_active = TRUE;

  /* Emit signal for UI feedback */
  g_signal_emit(self, signals[SIGNAL_BACKPRESSURE_APPLIED], 0, to_drop);
}

/**
 * insertion_buffer_sorted_insert:
 *
 * Insert a PendingEntry into the insertion buffer at the correct position
 * to maintain newest-first sort order.  Binary search O(log N) + one
 * memmove.  Replaces the O(N log N) g_array_sort after every transfer.
 */
static void insertion_buffer_sorted_insert(GArray *buf, PendingEntry *entry) {
  guint lo = 0, hi = buf->len;
  while (lo < hi) {
    guint mid = lo + (hi - lo) / 2;
    PendingEntry *e = &g_array_index(buf, PendingEntry, mid);
    if (entry->created_at > e->created_at)
      hi = mid;
    else
      lo = mid + 1;
  }
  g_array_insert_val(buf, lo, *entry);
}

/* ============== Smooth "New Notes" Reveal Animation ============== */

/**
 * cancel_reveal_animation:
 * @self: The model
 *
 * Cancel any in-progress reveal animation. Called during dispose or
 * when starting a new reveal.
 */
static void cancel_reveal_animation(GnTimelineModel *self) {
  self->reveal_in_progress = FALSE;
  self->reveal_position = 0;
  self->reveal_complete_cb = NULL;
  self->reveal_complete_data = NULL;

  if (self->reveal_queue) {
    g_array_set_size(self->reveal_queue, 0);
  }

  /* Clear all revealing keys */
  if (self->revealing_keys) {
    g_hash_table_remove_all(self->revealing_keys);
  }
}

/**
 * mark_key_revealing:
 * @self: The model
 * @key: Note key to mark
 *
 * Mark a note key as currently being revealed. Stores the monotonic
 * start time so the tick callback can clear the flag once
 * REVEAL_ANIMATION_MS has elapsed. No per-item timer is created;
 * the tick callback sweeps all revealing keys each frame.
 */
static void mark_key_revealing(GnTimelineModel *self, uint64_t key) {
  if (!self->revealing_keys) return;

  uint64_t *key_copy = g_new(uint64_t, 1);
  *key_copy = key;
  gint64 *start_time = g_new(gint64, 1);
  *start_time = g_get_monotonic_time();
  g_hash_table_insert(self->revealing_keys, key_copy, start_time);
}

/**
 * sweep_revealing_keys:
 * @self: The model
 *
 * Iterate over revealing_keys and remove entries whose animation
 * duration (REVEAL_ANIMATION_MS) has elapsed.
 *
 * Returns: Number of keys cleared this sweep.
 */
static guint sweep_revealing_keys(GnTimelineModel *self) {
  if (!self->revealing_keys) return 0;

  gint64 now_us = g_get_monotonic_time();
  gint64 threshold_us = (gint64)REVEAL_ANIMATION_MS * 1000;
  guint cleared = 0;

  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, self->revealing_keys);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    gint64 *start_us = (gint64 *)value;
    if (now_us - *start_us >= threshold_us) {
      g_hash_table_iter_remove(&iter);
      cleared++;
    }
  }

  if (cleared > 0) {
    g_debug("[REVEAL] Swept %u expired revealing keys (%u remaining)",
            cleared, g_hash_table_size(self->revealing_keys));
  }

  return cleared;
}

/**
 * has_active_reveals:
 * @self: The model
 *
 * Check whether there are any keys still in the revealing state.
 *
 * Returns: TRUE if there are active revealing keys.
 */
static gboolean has_active_reveals(GnTimelineModel *self) {
  if (!self->revealing_keys) return FALSE;
  return g_hash_table_size(self->revealing_keys) > 0;
}

/**
 * is_key_revealing:
 * @self: The model
 * @key: Note key to check
 *
 * Check if a note key is currently being revealed with animation.
 *
 * Returns: TRUE if the key is in the revealing state
 */
static gboolean is_key_revealing(GnTimelineModel *self, uint64_t key) {
  if (!self->revealing_keys) return FALSE;
  return g_hash_table_contains(self->revealing_keys, &key);
}

/**
 * process_reveal_batch:
 * @self: The model
 *
 * Process one batch of items from the reveal queue, inserting them
 * into the main notes array with animation CSS class applied.
 */
static void process_reveal_batch(GnTimelineModel *self) {
  if (!self->reveal_queue || self->reveal_queue->len == 0)
    return;

  guint remaining = self->reveal_queue->len - self->reveal_position;
  if (remaining == 0) return;

  guint batch_size = MIN(remaining, REVEAL_ITEMS_PER_BATCH);
  guint batch_start = self->reveal_position;
  guint batch_end = batch_start + batch_size;

  g_debug("[REVEAL] Processing batch %u-%u of %u items",
          batch_start, batch_end - 1, self->reveal_queue->len);

  /* Move items from reveal queue to main notes array */
  for (guint i = batch_start; i < batch_end; i++) {
    PendingEntry *pending = &g_array_index(self->reveal_queue, PendingEntry, i);

    /* Create note entry for main array */
    NoteEntry entry = {
      .note_key = pending->note_key,
      .created_at = pending->created_at
    };

    /* Append to end — O(1).  Logical position 0 via reversed get_item(). */
    g_array_append_val(self->notes, entry);

    /* Add to main key set */
    add_note_key_to_set(self, pending->note_key);

    /* Mark this key as revealing for CSS animation */
    mark_key_revealing(self, pending->note_key);

    /* Update timestamps */
    if (pending->created_at > self->newest_timestamp || self->newest_timestamp == 0) {
      self->newest_timestamp = pending->created_at;
    }
  }

  /* Skip window eviction during reveal — evict once at reveal completion.
   * Calling enforce_window_size here triggers the expensive replace-all
   * signal (0, old_count, new_count) on every 50ms batch, when a cheap
   * prepend signal (0, 0, batch_size) suffices during the animation. */
  g_list_model_items_changed(G_LIST_MODEL(self), 0, 0, batch_size);

  /* Emit reveal progress signal (position, total) */
  guint revealed = batch_end;
  guint total = self->reveal_queue->len;
  g_signal_emit(self, signals[SIGNAL_REVEAL_PROGRESS], 0, revealed, total);

  /* Update position for next batch */
  self->reveal_position = batch_end;
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

  /* Physical array: oldest at 0, newest at end.
   * Logical GListModel: position 0 = newest.
   * Reverse the mapping. */
  guint physical = self->notes->len - 1 - position;
  NoteEntry *entry = &g_array_index(self->notes, NoteEntry, physical);
  uint64_t key = entry->note_key;

  /* Check cache first */
  GnNostrEventItem *item = cache_get(self, key);
  if (item) {
    /* Update revealing state in case it changed */
    gn_nostr_event_item_set_revealing(item, is_key_revealing(self, key));
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

  /* Mark item as revealing if it's part of the current reveal */
  if (is_key_revealing(self, key)) {
    gn_nostr_event_item_set_revealing(item, TRUE);
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
 * pending_entry_compare_newest_first:
 *
 * Comparison function for sorting pending entries by created_at descending.
 */
static gint pending_entry_compare_newest_first(gconstpointer a, gconstpointer b) {
  const PendingEntry *ea = (const PendingEntry *)a;
  const PendingEntry *eb = (const PendingEntry *)b;
  if (ea->created_at > eb->created_at) return -1;
  if (ea->created_at < eb->created_at) return 1;
  return 0;
}

/* ============== GTask Worker Thread ============== */

/**
 * batch_process_data_free:
 *
 * Free all resources owned by BatchProcessData.
 */
static void batch_process_data_free(gpointer data) {
  BatchProcessData *bp = data;
  if (!bp) return;
  g_free(bp->note_keys);
  g_free(bp->kinds);
  if (bp->validated)
    g_array_unref(bp->validated);
  if (bp->prefetch_pubkeys)
    g_ptr_array_unref(bp->prefetch_pubkeys);
  g_free(bp);
}

/**
 * batch_process_thread_func:
 *
 * Runs on a GLib worker thread. Opens an NDB read transaction, queries each
 * note key, checks kind filter and mute list (both are thread-safe), and
 * builds an array of validated NoteEntry structs. Dedup hash-set lookups are
 * intentionally NOT done here because GHashTable is not thread-safe — they
 * are deferred to the main-thread completion callback.
 */
static void batch_process_thread_func(GTask        *task,
                                       gpointer      source_object,
                                       gpointer      task_data,
                                       GCancellable *cancellable) {
  (void)source_object;
  (void)cancellable;

  BatchProcessData *bp = task_data;
  bp->validated = g_array_sized_new(FALSE, FALSE, sizeof(NoteEntry), bp->n_keys);

  /* nostrc-perf: Collect unique pubkeys for background profile prefetch.
   * Uses a local hash set for O(1) dedup within this batch. */
  GHashTable *pk_set = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  void *txn = NULL;
  if (storage_ndb_begin_query(&txn) != 0 || !txn) {
    /* Return empty array — not an error, just nothing to insert */
    g_hash_table_destroy(pk_set);
    g_task_return_pointer(task, bp, NULL);
    return;
  }

  for (guint i = 0; i < bp->n_keys; i++) {
    uint64_t note_key = bp->note_keys[i];

    /* Get note from NostrDB */
    storage_ndb_note *note = storage_ndb_get_note_ptr(txn, note_key);
    if (!note) continue;

    /* Check kind filter */
    if (bp->kinds && bp->n_kinds > 0) {
      int kind = storage_ndb_note_kind(note);
      gboolean kind_ok = FALSE;
      for (gsize k = 0; k < bp->n_kinds; k++) {
        if (bp->kinds[k] == kind) {
          kind_ok = TRUE;
          break;
        }
      }
      if (!kind_ok) continue;
    }

    /* Check mute list — thread-safe (mute_list uses internal GMutex) */
    const unsigned char *pk = storage_ndb_note_pubkey(note);
    if (pk) {
      char pubkey_hex[65];
      storage_ndb_hex_encode(pk, pubkey_hex);
      GnostrMuteList *mute_list = gnostr_mute_list_get_default();
      if (mute_list && gnostr_mute_list_is_pubkey_muted(mute_list, pubkey_hex)) {
        continue;
      }
      /* Collect unique pubkey for profile prefetch */
      if (!g_hash_table_contains(pk_set, pubkey_hex)) {
        g_hash_table_add(pk_set, g_strdup(pubkey_hex));
      }
    }

    gint64 created_at = storage_ndb_note_created_at(note);
    NoteEntry entry = { .note_key = note_key, .created_at = created_at };
    g_array_append_val(bp->validated, entry);
  }

  storage_ndb_end_query(txn);

  /* Convert hash set to GPtrArray for the main-thread callback */
  guint n_pks = g_hash_table_size(pk_set);
  if (n_pks > 0) {
    bp->prefetch_pubkeys = g_ptr_array_new_with_free_func(g_free);
    GHashTableIter iter;
    gpointer key;
    g_hash_table_iter_init(&iter, pk_set);
    while (g_hash_table_iter_next(&iter, &key, NULL)) {
      g_ptr_array_add(bp->prefetch_pubkeys, g_strdup(key));
    }
  }
  g_hash_table_destroy(pk_set);

  g_task_return_pointer(task, bp, NULL);
}

/**
 * batch_process_complete_cb:
 *
 * Main-thread callback invoked when the worker thread finishes. Performs
 * dedup checks (GHashTable lookups must happen on the main thread) and
 * inserts validated entries directly into the insertion buffer (frame-aware
 * pipeline) or legacy batch buffer.
 *
 * Pipeline: NDB worker -> insertion_buffer -> tick callback -> notes.
 * The tick callback is the sole rate limiter.
 *
 * IMPORTANT: No GObject signals are emitted from the worker thread.
 * All signal emission happens here, on the main thread.
 */
static void batch_process_complete_cb(GObject      *source_object,
                                       GAsyncResult *res,
                                       gpointer      user_data) {
  (void)user_data;
  GnTimelineModel *self = GN_TIMELINE_MODEL(source_object);
  if (!GN_IS_TIMELINE_MODEL(self)) return;

  GTask *task = G_TASK(res);
  BatchProcessData *bp = g_task_propagate_pointer(task, NULL);
  if (!bp || !bp->validated || bp->validated->len == 0) {
    if (bp) batch_process_data_free(bp);
    return;
  }

  gboolean use_insertion_pipeline = (self->tick_widget != NULL &&
                                    self->insertion_buffer != NULL);

  /* Legacy path: Capture count at start of first batch in debounce window */
  if (!use_insertion_pipeline && !self->needs_refresh) {
    self->pending_update_old_count = self->notes->len;
  }

  /* Legacy path: Clear batch buffer for this batch */
  if (!use_insertion_pipeline) {
    g_array_set_size(self->batch_buffer, 0);
  }

  guint inserted_count = 0;
  gint64 arrival_time_us = g_get_monotonic_time();

  for (guint i = 0; i < bp->validated->len; i++) {
    NoteEntry *ve = &g_array_index(bp->validated, NoteEntry, i);
    uint64_t note_key = ve->note_key;
    gint64 created_at = ve->created_at;

    /* Dedup: skip if already in main array (GHashTable, main-thread only) */
    if (has_note_key(self, note_key)) continue;

    /* Skip if already in insertion buffer */
    if (use_insertion_pipeline) {
      if (has_note_key_pending(self, note_key)) continue;
    }

    if (use_insertion_pipeline) {
      /* Insert directly into insertion buffer (sorted).
       * The tick callback drains at items_per_frame items/frame. */
      PendingEntry entry = {
        .note_key = note_key,
        .created_at = created_at,
        .arrival_time_us = arrival_time_us
      };
      insertion_buffer_sorted_insert(self->insertion_buffer, &entry);
      add_note_key_to_insertion_set(self, note_key);
      inserted_count++;
    } else {
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

  if (use_insertion_pipeline) {
    if (inserted_count > 0) {
      /* Track peak insertion buffer depth for monitoring */
      if (self->insertion_buffer->len > self->peak_insertion_depth) {
        self->peak_insertion_depth = self->insertion_buffer->len;
      }

      g_debug("[INSERT] Inserted %u items into insertion buffer (pending: %u)",
              inserted_count, self->insertion_buffer->len);

      /* Apply backpressure if insertion buffer exceeds capacity */
      apply_insertion_backpressure(self);

      /* Clear backpressure flag if insertion buffer is under control */
      if (self->insertion_buffer->len < INSERTION_BUFFER_MAX) {
        self->backpressure_active = FALSE;
      }

      /* Ensure tick callback is running to drain insertion buffer */
      ensure_tick_callback(self);
    }
  } else {
    guint batch_count = self->batch_buffer->len;
    if (batch_count > 0) {
      g_array_sort(self->batch_buffer, note_entry_compare_oldest_first);
      g_array_append_vals(self->notes, self->batch_buffer->data, batch_count);

      self->batch_insert_count += batch_count;

      if (!self->user_at_top) {
        self->unseen_count += batch_count;
      }

      self->needs_refresh = TRUE;
      gn_timeline_model_schedule_update(self);

      if (!self->user_at_top && self->unseen_count > 0) {
        g_signal_emit(self, signals[SIGNAL_NEW_ITEMS_PENDING], 0, self->unseen_count);
      }
    }
  }

  /* nostrc-perf: Trigger background profile prefetch for unique pubkeys collected
   * by the worker thread. By warming the LRU cache asynchronously, profiles will
   * already be cached when GtkListView's factory_bind_cb runs. */
  if (bp->prefetch_pubkeys && bp->prefetch_pubkeys->len > 0) {
    const gchar **pk_array = g_new0(const gchar *, bp->prefetch_pubkeys->len + 1);
    for (guint i = 0; i < bp->prefetch_pubkeys->len; i++) {
      pk_array[i] = g_ptr_array_index(bp->prefetch_pubkeys, i);
    }
    pk_array[bp->prefetch_pubkeys->len] = NULL;
    gnostr_profile_provider_prefetch_batch_async(pk_array);
    g_free(pk_array);
  }

  /* End batch mode reactively when first notes arrive */
  if (self->in_batch_mode && self->notes->len > 0 && self->initial_load_timeout_id == 0) {
    g_debug("[TIMELINE] First notes received, scheduling batch mode end via idle");
    self->initial_load_timeout_id = g_idle_add_full(
      G_PRIORITY_LOW,
      on_end_batch_mode_idle,
      self,
      NULL
    );
  }

  batch_process_data_free(bp);
}

/* ============== Subscription Callback ============== */

/**
 * on_sub_timeline_batch:
 *
 * Subscription callback from NDB dispatcher.
 *
 * Copies incoming note_keys and dispatches a GTask to a worker thread.
 * The worker thread owns the NDB read transaction, performs kind checks and
 * mute list filtering, and returns validated NoteEntry structs. The main-thread
 * completion callback (batch_process_complete_cb) does dedup and insertion
 * buffer insertion.
 */
static void on_sub_timeline_batch(uint64_t subid, const uint64_t *note_keys, guint n_keys, gpointer user_data) {
  (void)subid;
  GnTimelineModel *self = GN_TIMELINE_MODEL(user_data);
  if (!GN_IS_TIMELINE_MODEL(self) || !note_keys || n_keys == 0) return;

  /* Prepare task data with a copy of the note_keys array and kind filter */
  BatchProcessData *bp = g_new0(BatchProcessData, 1);
  bp->note_keys = g_memdup2(note_keys, sizeof(uint64_t) * n_keys);
  bp->n_keys = n_keys;

  /* Copy kind filter from query (immutable struct, but worker must not touch self) */
  if (self->query && self->query->n_kinds > 0) {
    bp->n_kinds = self->query->n_kinds;
    bp->kinds = g_memdup2(self->query->kinds, sizeof(gint) * bp->n_kinds);
  } else {
    bp->kinds = NULL;
    bp->n_kinds = 0;
  }

  GTask *task = g_task_new(self, NULL, batch_process_complete_cb, NULL);
  g_task_set_task_data(task, bp, NULL);  /* bp freed in complete_cb */
  g_task_run_in_thread(task, batch_process_thread_func);
  g_object_unref(task);
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

  /* Clear everything including insertion buffer pipeline */
  g_array_set_size(self->notes, 0);
  g_array_set_size(self->batch_buffer, 0);
  g_array_set_size(self->insertion_buffer, 0);
  g_hash_table_remove_all(self->note_key_set);
  g_hash_table_remove_all(self->insertion_key_set);
  cache_clear(self);
  self->newest_timestamp = 0;
  self->oldest_timestamp = 0;
  self->unseen_count = 0;
  self->batch_insert_count = 0;
  self->backpressure_active = FALSE;

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

  /* Clear all arrays and hash tables including insertion buffer pipeline */
  g_array_set_size(self->notes, 0);
  g_array_set_size(self->batch_buffer, 0);
  g_array_set_size(self->insertion_buffer, 0);
  g_hash_table_remove_all(self->note_key_set);
  g_hash_table_remove_all(self->insertion_key_set);
  cache_clear(self);
  self->newest_timestamp = 0;
  self->oldest_timestamp = 0;
  self->unseen_count = 0;
  self->batch_insert_count = 0;
  self->backpressure_active = FALSE;

  if (old_count > 0) {
    g_list_model_items_changed(G_LIST_MODEL(self), 0, old_count, 0);
  }
}

guint gn_timeline_model_load_older(GnTimelineModel *self, guint count) {
  g_return_val_if_fail(GN_IS_TIMELINE_MODEL(self), 0);

  if (!self->query || self->oldest_timestamp == 0) return 0;
  if (count == 0) count = 50;  /* Default page size */

  /* Build filter with until=oldest_timestamp-1 for pagination */
  char *filter_json = gn_timeline_query_to_json_with_until(self->query, self->oldest_timestamp - 1);
  if (!filter_json) return 0;

  /* Use streaming cursor for zero-copy note_key access (nostrc-tbv) */
  StorageNdbCursor *cursor = storage_ndb_cursor_new(filter_json, count);
  g_free(filter_json);
  if (!cursor) return 0;

  const StorageNdbCursorEntry *entries = NULL;
  guint n_entries = 0;
  guint added = 0;

  if (storage_ndb_cursor_next(cursor, &entries, &n_entries) == 0 && n_entries > 0) {
    g_debug("[TIMELINE] load_older: cursor returned %u entries", n_entries);

    /* Open txn for mute list checking (need note pubkey) */
    void *txn = NULL;
    /* Non-blocking: no retry+sleep on main thread */
    storage_ndb_begin_query(&txn);

    /* Collect older items into a temporary array first, then bulk-insert
     * at physical position 0 (oldest end) in one operation. */
    GArray *temp = g_array_sized_new(FALSE, FALSE, sizeof(NoteEntry), n_entries);

    for (guint i = 0; i < n_entries; i++) {
      uint64_t note_key = entries[i].note_key;
      uint32_t created_at = entries[i].created_at;

      /* Skip if already have this note */
      if (has_note_key(self, note_key)) continue;

      /* Check mute list via direct note access */
      if (txn) {
        storage_ndb_note *note = storage_ndb_get_note_ptr(txn, note_key);
        if (note) {
          const unsigned char *pk = storage_ndb_note_pubkey(note);
          if (pk) {
            char pk_hex[65];
            storage_ndb_hex_encode(pk, pk_hex);
            GnostrMuteList *mute_list = gnostr_mute_list_get_default();
            if (mute_list && gnostr_mute_list_is_pubkey_muted(mute_list, pk_hex))
              continue;
          }
        }
      }

      NoteEntry entry = { .note_key = note_key, .created_at = (gint64)created_at };
      g_array_append_val(temp, entry);
      add_note_key_to_set(self, note_key);
      added++;

      /* Update oldest timestamp */
      if ((gint64)created_at < self->oldest_timestamp || self->oldest_timestamp == 0) {
        self->oldest_timestamp = (gint64)created_at;
      }
    }

    if (txn) storage_ndb_end_query(txn);

    /* Sort temp oldest-first, then bulk-insert at physical front */
    if (added > 1) {
      g_array_sort(temp, note_entry_compare_oldest_first);
    }
    if (added > 0) {
      g_array_insert_vals(self->notes, 0, temp->data, added);
    }
    g_array_unref(temp);
  }

  storage_ndb_cursor_free(cursor);

  if (added > 0) {
    guint old_count = self->notes->len - added;

    /* Enforce window size SILENTLY before emitting signal (nostrc-2n7).
     * Emitting two sequential items_changed signals (append then evict)
     * causes GTK's widget cache to become inconsistent — stale rows or crashes. */
    enforce_window_size(self, FALSE);

    /* Emit single atomic signal with net items added after eviction */
    guint net_added = self->notes->len - old_count;
    if (net_added > 0) {
      g_list_model_items_changed(G_LIST_MODEL(self), old_count, 0, net_added);
      g_debug("[TIMELINE] load_older: inserted %u items at logical position %u (evicted %u)",
              net_added, old_count, added - net_added);
    }
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

/**
 * gn_timeline_model_flush_pending_animated:
 * @self: The model
 * @complete_cb: (nullable): Callback when reveal finishes (signature: void (*)(gpointer model, gpointer user_data))
 * @complete_data: (nullable): User data for completion callback
 *
 * Flush pending items with a smooth frame-synced reveal animation.
 *
 * Instead of inserting all pending items at once (which causes jarring UX),
 * this function moves pending items to a reveal queue and animates them in
 * batches via the tick callback (one batch per frame, ~16ms).
 *
 * The completion callback is invoked AFTER all items are revealed, allowing
 * the caller to perform scroll-to-top after the animation completes.
 *
 * If there are no pending items, the completion callback is invoked immediately.
 * If a reveal is already in progress, it is cancelled and restarted.
 */
void gn_timeline_model_flush_pending_animated(GnTimelineModel *self,
                                               GFunc            complete_cb,
                                               gpointer         complete_data) {
  g_return_if_fail(GN_IS_TIMELINE_MODEL(self));

  /* Cancel any existing reveal animation */
  cancel_reveal_animation(self);

  /*
   * Collect items to reveal from the insertion buffer.
   * These are items that have arrived but haven't been displayed yet.
   */
  guint total_to_reveal = 0;

  /* Transfer insertion buffer items to reveal queue */
  if (self->insertion_buffer && self->insertion_buffer->len > 0) {
    for (guint i = 0; i < self->insertion_buffer->len; i++) {
      PendingEntry *entry = &g_array_index(self->insertion_buffer, PendingEntry, i);

      /* Skip if already in main notes array */
      if (has_note_key(self, entry->note_key))
        continue;

      g_array_append_val(self->reveal_queue, *entry);
      total_to_reveal++;
    }

    /* Clear the insertion buffer and its key set */
    for (guint i = 0; i < self->insertion_buffer->len; i++) {
      PendingEntry *entry = &g_array_index(self->insertion_buffer, PendingEntry, i);
      remove_note_key_from_insertion_set(self, entry->note_key);
    }
    g_array_set_size(self->insertion_buffer, 0);
  }

  if (total_to_reveal == 0) {
    g_debug("[REVEAL] No items to reveal, calling completion immediately");

    /* No items to reveal - just clear unseen count and call completion */
    self->unseen_count = 0;
    g_signal_emit(self, signals[SIGNAL_NEW_ITEMS_PENDING], 0, (guint)0);

    if (complete_cb) {
      complete_cb(self, complete_data);
    }
    return;
  }

  g_debug("[REVEAL] Starting animated reveal of %u items", total_to_reveal);

  /* Sort reveal queue by created_at descending (newest first) */
  g_array_sort(self->reveal_queue, pending_entry_compare_newest_first);

  /* Set up reveal state */
  self->reveal_in_progress = TRUE;
  self->reveal_position = 0;
  self->reveal_complete_cb = complete_cb;
  self->reveal_complete_data = complete_data;

  /* Start the reveal animation via the tick callback. The tick callback
   * processes reveal_queue batches in Phase 1, pending items in Phase 2,
   * and sweeps expired revealing keys in Phase 3 — all frame-synced. */
  ensure_tick_callback(self);
}

/**
 * gn_timeline_model_is_reveal_in_progress:
 * @self: The model
 *
 * Check if an animated reveal is currently in progress.
 *
 * Returns: TRUE if reveal animation is active
 */
gboolean gn_timeline_model_is_reveal_in_progress(GnTimelineModel *self) {
  g_return_val_if_fail(GN_IS_TIMELINE_MODEL(self), FALSE);
  return self->reveal_in_progress;
}

/**
 * gn_timeline_model_cancel_reveal:
 * @self: The model
 *
 * Cancel any in-progress reveal animation.
 * Items already revealed will remain, but remaining items are discarded.
 */
void gn_timeline_model_cancel_reveal(GnTimelineModel *self) {
  g_return_if_fail(GN_IS_TIMELINE_MODEL(self));
  cancel_reveal_animation(self);
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

/* ============== Frame-Aware Batching Public API ============== */

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

    /* If there are already pending items, start the tick callback */
    if (self->insertion_buffer && self->insertion_buffer->len > 0) {
      ensure_tick_callback(self);
    }
  } else {
    g_debug("[FRAME] View widget cleared, disabling frame-aware batching");
  }
}

guint gn_timeline_model_get_staged_count(GnTimelineModel *self) {
  g_return_val_if_fail(GN_IS_TIMELINE_MODEL(self), 0);
  if (!self->insertion_buffer) return 0;
  return self->insertion_buffer->len;
}

/* ============== Insertion Pipeline Diagnostics ============== */

guint gn_timeline_model_get_total_queued_count(GnTimelineModel *self) {
  g_return_val_if_fail(GN_IS_TIMELINE_MODEL(self), 0);
  return gn_timeline_model_get_staged_count(self);
}

guint gn_timeline_model_get_peak_queue_depth(GnTimelineModel *self) {
  g_return_val_if_fail(GN_IS_TIMELINE_MODEL(self), 0);
  return self->peak_insertion_depth;
}

gboolean gn_timeline_model_is_backpressure_active(GnTimelineModel *self) {
  g_return_val_if_fail(GN_IS_TIMELINE_MODEL(self), FALSE);
  return self->backpressure_active;
}

void gn_timeline_model_reset_peak_queue_depth(GnTimelineModel *self) {
  g_return_if_fail(GN_IS_TIMELINE_MODEL(self));
  self->peak_insertion_depth = 0;
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

  /* Frame-aware batching cleanup */
  if (self->tick_widget) {
    if (self->tick_callback_id != 0) {
      gtk_widget_remove_tick_callback(self->tick_widget, self->tick_callback_id);
      self->tick_callback_id = 0;
    }
    g_object_weak_unref(G_OBJECT(self->tick_widget),
                        on_tick_widget_destroyed, self);
    self->tick_widget = NULL;
  }
  g_clear_pointer(&self->insertion_buffer, g_array_unref);
  g_clear_pointer(&self->insertion_key_set, g_hash_table_unref);

  /* Cancel reveal animation and clean up */
  cancel_reveal_animation(self);
  g_clear_pointer(&self->reveal_queue, g_array_unref);
  g_clear_pointer(&self->revealing_keys, g_hash_table_unref);

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

  signals[SIGNAL_BACKPRESSURE_APPLIED] =
    g_signal_new("backpressure-applied",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 1, G_TYPE_UINT);

  signals[SIGNAL_REVEAL_PROGRESS] =
    g_signal_new("reveal-progress",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 2, G_TYPE_UINT, G_TYPE_UINT);  /* (revealed, total) */
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

  /* Frame-aware batching: insertion buffer pipeline */
  self->insertion_buffer = g_array_new(FALSE, FALSE, sizeof(PendingEntry));
  self->insertion_key_set = g_hash_table_new_full(uint64_hash, uint64_equal, g_free, NULL);
  self->items_per_frame = ITEMS_PER_FRAME_DEFAULT;
  self->tick_callback_id = 0;
  self->tick_widget = NULL;

  /* Backpressure tracking */
  self->peak_insertion_depth = 0;
  self->backpressure_active = FALSE;

  /* Smooth reveal animation */
  self->reveal_queue = g_array_new(FALSE, FALSE, sizeof(PendingEntry));
  self->reveal_position = 0;
  self->reveal_in_progress = FALSE;
  self->reveal_complete_cb = NULL;
  self->reveal_complete_data = NULL;
  self->revealing_keys = g_hash_table_new_full(uint64_hash, uint64_equal, g_free, g_free);

  /* Start in batch mode to prevent widget recycling storms during initial load.
   * Batch mode will be ended after the first batch of notes is processed,
   * using an idle callback (not a fixed timeout). */
  self->in_batch_mode = TRUE;
  self->pending_update_old_count = 0;

  /* Subscribe to timeline events */
  const char *filter = "{\"kinds\":[1,6]}";
  self->sub_timeline = gn_ndb_subscribe(filter, on_sub_timeline_batch, self, NULL);

  /* NOTE: No timeout! Batch mode is ended signal-driven when first notes arrive.
   * See on_sub_timeline_batch for the idle callback that ends batch mode. */
}
