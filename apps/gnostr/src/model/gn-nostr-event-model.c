#define G_LOG_DOMAIN "gnostr-event-model"

#include "gn-nostr-event-model.h"
#include <nostr-gobject-1.0/gn-timeline-query.h>
#include <nostr-gobject-1.0/gn-ndb-sub-dispatcher.h>
#include <nostr-gobject-1.0/storage_ndb.h>
#include <nostr-gobject-1.0/gnostr-mute-list.h>
#include <nostr-gobject-1.0/nostr_profile_provider.h>
#include <nostr.h>
#include <string.h>

/* Window sizing and cache sizes */
#define MODEL_MAX_ITEMS 100
#define ITEM_CACHE_SIZE 100
#define PROFILE_CACHE_MAX 500
#define AUTHORS_READY_MAX 1000

/* Frame-aware batching — adaptive drain rate (backported from GnTimelineModel)
 *
 * The pipeline uses a ~16ms GLib timeout (g_timeout_add) for frame-rate
 * insertion buffer drain, replacing the former GTK tick callback.
 * Batch size adapts dynamically based on insertion buffer depth:
 *   Deep buffer (startup flood) → drain aggressively (up to 50/frame).
 *   Shallow buffer (steady state) → conservative (3/frame) for smooth scroll.
 * An inline frame-time guard yields early if the budget is exceeded. */
#define ITEMS_PER_FRAME_FLOOR 3      /* Steady-state conservative drain */
#define ITEMS_PER_FRAME_MAX 50       /* Ceiling during aggressive drain */
#define FRAME_BUDGET_US 12000        /* 12ms target, leaving 4ms margin for 16.6ms frame */
#define INSERTION_BUFFER_MAX 100     /* Max items in insertion buffer before backpressure */
#define EVICT_DEFER_FRAMES 30        /* Only enforce window size every 30 frames (~500ms) */
#define PENDING_SIGNAL_INTERVAL_US 250000  /* 250ms between new-items-pending emissions */
#define REACTION_CACHE_MAX 500   /* Cap reaction count cache (keyed by target event_id) */
#define ZAP_CACHE_MAX 500        /* Cap zap stats cache (keyed by target event_id) */

/* Subscription filters - storage_ndb_subscribe expects a single filter object, not an array */
#define FILTER_TIMELINE   "{\"kinds\":[1,6,9735]}"
#define FILTER_PROFILES   "{\"kinds\":[0]}"
#define FILTER_DELETES    "{\"kinds\":[5]}"
#define FILTER_REACTIONS  "{\"kinds\":[7]}"
#define FILTER_ZAPS       "{\"kinds\":[9735]}"

/* Note entry for sorted storage */
typedef struct {
  uint64_t note_key;
  gint64 created_at;
} NoteEntry;

/* Pending entry for frame-aware insertion buffer */
typedef struct {
  uint64_t note_key;
  gint64   created_at;
  gint64   arrival_time_us;  /* Monotonic time when queued, for backpressure */
} PendingEntry;

/* Validated entry produced by worker thread for timeline batch processing */
typedef struct {
  uint64_t note_key;
  gint64   created_at;
  char     pubkey_hex[65];
  char    *root_id;       /* owned, nullable — NIP-10 thread root */
  char    *reply_id;      /* owned, nullable — NIP-10 thread reply */
  int      kind;
} TimelineBatchEntry;

static void timeline_batch_entry_clear(gpointer data) {
  TimelineBatchEntry *e = data;
  g_free(e->root_id);
  g_free(e->reply_id);
}

/* Data passed to the timeline batch worker thread */
typedef struct {
  uint64_t *note_keys;    /* input: owned copy of note keys to process */
  guint     n_keys;
  gint     *kinds;        /* snapshot of filter kinds (owned) */
  gsize     n_kinds;
  char    **authors;      /* snapshot of filter authors (owned, NULL-terminated) */
  gsize     n_authors;
  gint64    since, until;
  GArray   *validated;    /* output: TimelineBatchEntry array */
  GPtrArray *prefetch_pubkeys;  /* output: unique pubkey hexes for profile prefetch */
} TimelineBatchProcessData;

static void timeline_batch_data_free(gpointer data) {
  TimelineBatchProcessData *bp = data;
  if (!bp) return;
  g_free(bp->note_keys);
  g_free(bp->kinds);
  if (bp->authors) {
    for (gsize i = 0; i < bp->n_authors; i++)
      g_free(bp->authors[i]);
    g_free(bp->authors);
  }
  if (bp->validated)
    g_array_unref(bp->validated);
  if (bp->prefetch_pubkeys)
    g_ptr_array_unref(bp->prefetch_pubkeys);
  g_free(bp);
}

struct _GnNostrEventModel {
  GObject parent_instance;

  /* Query parameters (new API) */
  GNostrTimelineQuery *timeline_query;

  /* Query parameters (legacy - kept for compatibility) */
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
  GHashTable *note_key_set;  /* O(1) dedup: key = uint64_t*, value = GINT_TO_POINTER(1) */

  /* Lifetime nostrdb subscriptions (via dispatcher) */
  uint64_t sub_timeline;   /* kinds 1/6 */
  uint64_t sub_profiles;   /* kind 0 */
  uint64_t sub_deletes;    /* kind 5 */
  uint64_t sub_reactions;  /* kind 7 (NIP-25) */
  uint64_t sub_zaps;       /* kind 9735 (NIP-57) */

  /* Reaction/zap stats caches - event_id_hex -> stats */
  GHashTable *reaction_cache;  /* key: event_id (string), value: guint count via GUINT_TO_POINTER */
  GHashTable *zap_stats_cache; /* key: event_id (string), value: ZapStats* */

  /* Windowing */
  guint window_size;

  /* Small LRU cache for visible items */
  GHashTable *item_cache;  /* key: uint64_t*, value: GnNostrEventItem* */
  GQueue *cache_lru;       /* uint64_t* keys in LRU order */

  /* Profile cache - pubkey -> GNostrProfile (with LRU eviction) */
  GHashTable *profile_cache;      /* key: pubkey (string), value: GNostrProfile* */
  GQueue *profile_cache_lru;      /* char* pubkey in LRU order (head=oldest) */

  /* Author readiness (kind 0 exists in DB / loaded) - with LRU eviction */
  GHashTable *authors_ready;      /* key: pubkey hex (string), value: GINT_TO_POINTER(1) */
  GQueue *authors_ready_lru;      /* char* pubkey in LRU order (head=oldest) */
  /* Thread info cache - note_key -> ThreadInfo */
  GHashTable *thread_info;

  /* nostrc-7o7: Animation control - track which items should skip animation */
  guint visible_start;  /* First visible position in the list */
  guint visible_end;    /* Last visible position in the list */
  GHashTable *skip_animation_keys;  /* key: uint64_t*, value: GINT_TO_POINTER(1) */

  /* Scroll position awareness */
  gboolean user_at_top;           /* TRUE if user is at scroll top (auto-scroll allowed) */
  guint unseen_count;             /* Items added while user is scrolled down */

  /* Pipeline: worker thread → insertion_buffer → tick callback → notes array
   * Backported from GnTimelineModel for frame-synced insertion with adaptive drain. */
  GArray *insertion_buffer;       /* PendingEntry items awaiting frame-synced insertion */
  GHashTable *insertion_key_set;  /* note_key → TRUE for O(1) dedup in insertion buffer */
  guint tick_source_id;           /* g_timeout_add source ID, 0 if inactive */
  gboolean drain_enabled;         /* TRUE when drain timer may run */
  guint peak_insertion_depth;     /* High-water mark for monitoring */
  gboolean backpressure_active;   /* TRUE when backpressure is being applied */
  guint evict_defer_counter;      /* Defer window eviction to avoid replace-all every frame */
  gint64 last_pending_signal_us;  /* Throttle SIGNAL_NEW_ITEMS_PENDING emission */

  /* Async pagination state */
  gboolean async_loading;
};

typedef struct {
  char *root_id;
  char *parent_id;
  guint depth;
} ThreadInfo;

/* NIP-57: Zap stats for caching */
typedef struct {
  guint count;
  gint64 total_msat;
} ZapStats;

static void thread_info_free(ThreadInfo *info) {
  if (!info) return;
  g_free(info->root_id);
  g_free(info->parent_id);
  g_free(info);
}

static void zap_stats_free(ZapStats *stats) {
  g_free(stats);
}

static guint uint64_hash(gconstpointer key) {
  uint64_t k = *(const uint64_t *)key;
  return (guint)(k ^ (k >> 32));
}

static gboolean uint64_equal(gconstpointer a, gconstpointer b) {
  return *(const uint64_t *)a == *(const uint64_t *)b;
}

static gint uint64_compare_for_queue(gconstpointer a, gconstpointer b) {
  const uint64_t av = *(const uint64_t *)a;
  const uint64_t bv = *(const uint64_t *)b;
  return (av == bv) ? 0 : 1;
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

enum {
  SIGNAL_NEED_PROFILE,
  SIGNAL_NEW_ITEMS_PENDING,  /* nostrc-yi2: emitted when new items are waiting */
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* Forward declarations */
static GNostrProfile *profile_cache_get(GnNostrEventModel *self, const char *pubkey_hex);
static GNostrProfile *profile_cache_ensure_from_db(GnNostrEventModel *self, void *txn,
                                                    const unsigned char pk32[32],
                                                    const char *pubkey_hex);
static void profile_cache_update_from_content(GnNostrEventModel *self, const char *pubkey_hex,
                                              const char *content, gsize content_len);
static gboolean note_matches_query(GnNostrEventModel *self, int kind, const char *pubkey_hex, gint64 created_at);
static gboolean remove_note_by_key(GnNostrEventModel *self, uint64_t note_key);

/* Worker thread pipeline */
static void timeline_batch_thread_func(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable);
static void timeline_batch_complete_cb(GObject *source_object, GAsyncResult *res, gpointer user_data);

/* Subscription callbacks */
static void on_sub_timeline_batch(uint64_t subid, const uint64_t *note_keys, guint n_keys, gpointer user_data);
static void on_sub_profiles_batch(uint64_t subid, const uint64_t *note_keys, guint n_keys, gpointer user_data);
static void on_sub_deletes_batch(uint64_t subid, const uint64_t *note_keys, guint n_keys, gpointer user_data);
static void on_sub_reactions_batch(uint64_t subid, const uint64_t *note_keys, guint n_keys, gpointer user_data);
static void on_sub_zaps_batch(uint64_t subid, const uint64_t *note_keys, guint n_keys, gpointer user_data);

/* LRU cache management */
static void cache_touch(GnNostrEventModel *self, uint64_t key) {
  /* Move to front of LRU queue */
  GList *link = g_queue_find_custom(self->cache_lru, &key, (GCompareFunc)uint64_compare_for_queue);
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
      /* Remove from thread_info before removing from item_cache to prevent dangling pointers */
      g_hash_table_remove(self->thread_info, old_key);
      g_hash_table_remove(self->item_cache, old_key);
      /* Note: key is freed by hash table; queue just dropped its pointer */
    }
  }
}

/* nostrc-slot: Pre-create and cache an item during batch processing.
 * This populates the item with data from the note pointer while the transaction
 * is still open, avoiding a new transaction later when get_item is called.
 * This is the key optimization to prevent LMDB reader slot exhaustion. */
static void precache_item_from_note(GnNostrEventModel *self, uint64_t note_key,
                                     gint64 created_at, storage_ndb_note *note) {
  /* Already in cache? Nothing to do */
  if (g_hash_table_contains(self->item_cache, &note_key)) {
    return;
  }

  /* Create item and populate from note (no new transaction needed) */
  GnNostrEventItem *item = gn_nostr_event_item_new_from_key(note_key, created_at);
  gn_nostr_event_item_populate_from_note(item, (struct ndb_note *)note);

  /* Add to cache */
  cache_add(self, note_key, item);

  /* Release our reference - cache now owns it */
  g_object_unref(item);
}

/* Helper: remove a key from cache_lru (must be called before removing from item_cache) */
static void cache_lru_remove_key(GnNostrEventModel *self, uint64_t note_key) {
  if (!self || !self->cache_lru) return;
  GList *link = g_queue_find_custom(self->cache_lru, &note_key, (GCompareFunc)uint64_compare_for_queue);
  if (!link) return;
  g_queue_unlink(self->cache_lru, link);
  g_list_free_1(link);
}

/* Batch mode removed - direct signal emission per note works reliably */

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

static gboolean author_is_ready(GnNostrEventModel *self, const char *pubkey_hex) {
  if (!self || !self->authors_ready || !pubkey_hex) return FALSE;
  return g_hash_table_contains(self->authors_ready, pubkey_hex);
}

static void authors_ready_evict(GnNostrEventModel *self);  /* forward decl */
static void profile_cache_evict(GnNostrEventModel *self);  /* forward decl */

static void mark_author_ready(GnNostrEventModel *self, const char *pubkey_hex) {
  if (!self || !self->authors_ready || !pubkey_hex) return;
  if (!g_hash_table_contains(self->authors_ready, pubkey_hex)) {
    g_hash_table_insert(self->authors_ready, g_strdup(pubkey_hex), GINT_TO_POINTER(1));
    /* Track in LRU queue */
    if (self->authors_ready_lru) {
      g_queue_push_tail(self->authors_ready_lru, g_strdup(pubkey_hex));
    }
    /* Evict if over limit */
    authors_ready_evict(self);
  }
}

/* NOTE: This is a strict gating check based on DB availability, not in-memory cache. */
static gboolean db_has_profile_event_for_pubkey(void *txn, const unsigned char pk32[32]) {
  if (!txn || !pk32) return FALSE;
  char *evt_json = NULL;
  int evt_len = 0;
  int rc = storage_ndb_get_profile_by_pubkey(txn, pk32, &evt_json, &evt_len, NULL);
  if (rc != 0 || !evt_json || evt_len <= 0) {
    if (evt_json) free(evt_json);
    return FALSE;
  }
  free(evt_json);
  return TRUE;
}

static GNostrProfile *profile_cache_get(GnNostrEventModel *self, const char *pubkey_hex) {
  if (!self || !self->profile_cache || !pubkey_hex) return NULL;
  return g_hash_table_lookup(self->profile_cache, pubkey_hex);
}

/* Load kind-0 profile from DB (storage_ndb_get_profile_by_pubkey returns *event* JSON), then cache it.
 * Returns a cached GNostrProfile* on success, NULL if not found.
 */
static GNostrProfile *profile_cache_ensure_from_db(GnNostrEventModel *self, void *txn,
                                                    const unsigned char pk32[32],
                                                    const char *pubkey_hex) {
  if (!self || !txn || !pk32 || !pubkey_hex) return NULL;

  GNostrProfile *existing = profile_cache_get(self, pubkey_hex);
  if (existing) return existing;

  char *evt_json = NULL;
  int evt_len = 0;
  int rc = storage_ndb_get_profile_by_pubkey(txn, pk32, &evt_json, &evt_len, NULL);
  if (rc != 0 || !evt_json || evt_len <= 0) {
    if (evt_json) free(evt_json);
    return NULL;
  }

  NostrEvent *evt = nostr_event_new();
  if (!evt) {
    free(evt_json);
    return NULL;
  }

  GNostrProfile *profile = NULL;

  if (nostr_event_deserialize(evt, evt_json) == 0 && nostr_event_get_kind(evt) == 0) {
    const char *content = nostr_event_get_content(evt);
    if (content && *content) {
      profile = gnostr_profile_new(pubkey_hex);
      gnostr_profile_update_from_json(profile, content);
      g_hash_table_replace(self->profile_cache, g_strdup(pubkey_hex), profile);
      /* Add to LRU queue (new entry) */
      if (self->profile_cache_lru) {
        g_queue_push_tail(self->profile_cache_lru, g_strdup(pubkey_hex));
        profile_cache_evict(self);
      }
      mark_author_ready(self, pubkey_hex);
    }
  }

  nostr_event_free(evt);
  free(evt_json);

  return profile;
}

/* Evict oldest entries from profile_cache if over limit */
static void profile_cache_evict(GnNostrEventModel *self) {
  if (!self || !self->profile_cache || !self->profile_cache_lru) return;
  
  guint before = g_hash_table_size(self->profile_cache);
  guint evicted = 0;
  
  while (g_hash_table_size(self->profile_cache) > PROFILE_CACHE_MAX &&
         !g_queue_is_empty(self->profile_cache_lru)) {
    char *oldest = g_queue_pop_head(self->profile_cache_lru);
    if (oldest) {
      /* Use g_hash_table_remove which frees both key and value via destroy funcs.
       * We only free our LRU queue copy (oldest), not the hash table's key. */
      g_hash_table_remove(self->profile_cache, oldest);
      g_free(oldest);  /* Free LRU queue's copy of the key */
      evicted++;
    }
  }
  
  if (evicted > 0) {
    g_debug("[MODEL] profile_cache evicted %u entries (%u -> %u)", 
            evicted, before, g_hash_table_size(self->profile_cache));
  }
}

/* Evict oldest entries from authors_ready if over limit */
static void authors_ready_evict(GnNostrEventModel *self) {
  if (!self || !self->authors_ready || !self->authors_ready_lru) return;
  
  guint before = g_hash_table_size(self->authors_ready);
  guint evicted = 0;
  
  while (g_hash_table_size(self->authors_ready) > AUTHORS_READY_MAX &&
         !g_queue_is_empty(self->authors_ready_lru)) {
    char *oldest = g_queue_pop_head(self->authors_ready_lru);
    if (oldest) {
      g_hash_table_remove(self->authors_ready, oldest);
      g_free(oldest);
      evicted++;
    }
  }
  
  if (evicted > 0) {
    g_debug("[MODEL] authors_ready evicted %u entries (%u -> %u)", 
            evicted, before, g_hash_table_size(self->authors_ready));
  }
}

static void profile_cache_update_from_content(GnNostrEventModel *self, const char *pubkey_hex,
                                              const char *content, gsize content_len) {
  if (!self || !pubkey_hex || !content || content_len == 0) return;

  /* content is kind-0 event content JSON, not necessarily NUL terminated */
  char *tmp = g_strndup(content, content_len);

  GNostrProfile *profile = profile_cache_get(self, pubkey_hex);
  if (!profile) {
    profile = gnostr_profile_new(pubkey_hex);
    g_hash_table_replace(self->profile_cache, g_strdup(pubkey_hex), profile);
    /* Add to LRU queue (new entry) */
    g_queue_push_tail(self->profile_cache_lru, g_strdup(pubkey_hex));
    /* Evict if over limit */
    profile_cache_evict(self);
  }

  gnostr_profile_update_from_json(profile, tmp);
  mark_author_ready(self, pubkey_hex);

  g_free(tmp);
}

/* Notify cached items that their "profile" property should be re-read by views.
 * nostrc-80i1: Actually SET the profile on the item, not just notify. */
static void notify_cached_items_for_pubkey(GnNostrEventModel *self, const char *pubkey_hex) {
  if (!self || !pubkey_hex || !self->item_cache) return;

  /* Get the profile from the model's cache */
  GNostrProfile *profile = profile_cache_get(self, pubkey_hex);
  if (!profile) return; /* No profile to set */

  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, self->item_cache);

  while (g_hash_table_iter_next(&iter, &key, &value)) {
    GnNostrEventItem *item = GN_NOSTR_EVENT_ITEM(value);
    const char *item_pubkey = gn_nostr_event_item_get_pubkey(item);
    if (item_pubkey && g_strcmp0(item_pubkey, pubkey_hex) == 0) {
      /* Actually set the profile on the item - this will also notify */
      gn_nostr_event_item_set_profile(item, profile);
    }
  }
}

static gboolean note_matches_query(GnNostrEventModel *self, int kind, const char *pubkey_hex, gint64 created_at) {
  if (!self) return FALSE;

  /* NIP-51 Mute list filter: check if author is muted */
  GNostrMuteList *mute_list = gnostr_mute_list_get_default();
  if (mute_list && pubkey_hex && gnostr_mute_list_is_pubkey_muted(mute_list, pubkey_hex)) {
    return FALSE;
  }

  /* Kind filter */
  if (self->n_kinds > 0) {
    gboolean kind_ok = FALSE;
    for (gsize i = 0; i < self->n_kinds; i++) {
      if (self->kinds[i] == kind) { kind_ok = TRUE; break; }
    }
    if (!kind_ok) return FALSE;
  }

  /* Author filter */
  if (self->n_authors > 0) {
    gboolean author_ok = FALSE;
    for (gsize i = 0; i < self->n_authors; i++) {
      if (self->authors[i] && pubkey_hex && g_strcmp0(self->authors[i], pubkey_hex) == 0) {
        author_ok = TRUE;
        break;
      }
    }
    if (!author_ok) return FALSE;
  }

  /* since/until filters */
  if (self->since > 0 && created_at > 0 && created_at < self->since) return FALSE;
  if (self->until > 0 && created_at > 0 && created_at > self->until) return FALSE;

  return TRUE;
}

/* Find insertion position for sorted insert (newest first) — O(log N) binary search */
static guint find_sorted_position(GnNostrEventModel *self, gint64 created_at) {
  guint lo = 0, hi = self->notes->len;
  while (lo < hi) {
    guint mid = lo + (hi - lo) / 2;
    NoteEntry *entry = &g_array_index(self->notes, NoteEntry, mid);
    if (entry->created_at >= created_at)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo;
}

/* Check if note_key is already in the model — O(1) via hash set */
static gboolean has_note_key(GnNostrEventModel *self, uint64_t key) {
  return g_hash_table_contains(self->note_key_set, &key);
}

/* ============== Insertion Buffer Pipeline (backported from GnTimelineModel) ============== */

/* Forward declaration for pipeline drain timer */
static gboolean on_drain_timer(gpointer user_data);

/**
 * has_note_key_pending:
 * Check if a note key is already in the insertion buffer. O(1) lookup.
 */
static gboolean has_note_key_pending(GnNostrEventModel *self, uint64_t key) {
  if (!self->insertion_key_set) return FALSE;
  return g_hash_table_contains(self->insertion_key_set, &key);
}

/**
 * add_note_key_to_insertion_set:
 * Add a note key to the insertion buffer dedup set.
 */
static void add_note_key_to_insertion_set(GnNostrEventModel *self, uint64_t key) {
  if (!self->insertion_key_set) return;
  uint64_t *key_copy = g_new(uint64_t, 1);
  *key_copy = key;
  g_hash_table_add(self->insertion_key_set, key_copy);
}

/**
 * remove_note_key_from_insertion_set:
 * Remove a note key from the insertion buffer dedup set.
 */
static void remove_note_key_from_insertion_set(GnNostrEventModel *self, uint64_t key) {
  if (!self->insertion_key_set) return;
  g_hash_table_remove(self->insertion_key_set, &key);
}

/**
 * ensure_drain_timer:
 * Start a ~16ms GLib timeout for frame-rate insertion buffer drain.
 * No-op if timer is already running or drain is disabled.
 */
static void ensure_drain_timer(GnNostrEventModel *self) {
  if (self->tick_source_id != 0)
    return;
  if (!self->drain_enabled)
    return;

  self->tick_source_id = g_timeout_add(16, on_drain_timer, g_object_ref(self));

  if (self->tick_source_id != 0) {
    g_debug("[FRAME] Drain timer started (id=%u)", self->tick_source_id);
  }
}

/**
 * remove_drain_timer:
 * Stop the drain timer if active.
 */
static void remove_drain_timer(GnNostrEventModel *self) {
  if (self->tick_source_id != 0) {
    g_source_remove(self->tick_source_id);
    g_object_unref(self);  /* balance the ref from ensure_drain_timer */
  }
  self->tick_source_id = 0;
}

/**
 * apply_insertion_backpressure:
 * Drop oldest items (tail of newest-first buffer) when exceeding INSERTION_BUFFER_MAX.
 */
static void apply_insertion_backpressure(GnNostrEventModel *self) {
  if (!self->insertion_buffer || self->insertion_buffer->len <= INSERTION_BUFFER_MAX)
    return;

  guint to_drop = self->insertion_buffer->len - INSERTION_BUFFER_MAX;

  g_debug("[BACKPRESSURE] Dropping %u oldest items from insertion buffer (%u -> %u)",
          to_drop, self->insertion_buffer->len, INSERTION_BUFFER_MAX);

  for (guint i = self->insertion_buffer->len - to_drop; i < self->insertion_buffer->len; i++) {
    PendingEntry *entry = &g_array_index(self->insertion_buffer, PendingEntry, i);
    remove_note_key_from_insertion_set(self, entry->note_key);
  }
  g_array_remove_range(self->insertion_buffer, self->insertion_buffer->len - to_drop, to_drop);

  self->backpressure_active = TRUE;
}

/**
 * insertion_buffer_sorted_insert:
 * Binary search insert into insertion buffer, maintaining newest-first order.
 * O(log N) search + one memmove.
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

/**
 * reset_internal_state_silent:
 * Reset all internal data structures WITHOUT emitting any GListModel signal.
 * Used by refresh paths that will emit a single atomic items_changed(0, old, new)
 * to avoid the pathological mass-disposal cascade that causes heap corruption
 * when GTK tears down hundreds of complex widget trees in one stack frame.
 *
 * nostrc-atomic-replace: This is the key fix for the persistent timeline crash.
 */
static void reset_internal_state_silent(GnNostrEventModel *self) {
  /* Stop drain timer to avoid concurrent mutations */
  remove_drain_timer(self);

  /* Clear insertion buffer pipeline state */
  if (self->insertion_buffer)
    g_array_set_size(self->insertion_buffer, 0);
  if (self->insertion_key_set)
    g_hash_table_remove_all(self->insertion_key_set);
  self->backpressure_active = FALSE;
  self->unseen_count = 0;
  self->evict_defer_counter = 0;

  /* Clear notes array and all caches - no signal emitted */
  g_array_set_size(self->notes, 0);
  g_hash_table_remove_all(self->note_key_set);
  g_hash_table_remove_all(self->item_cache);
  g_queue_clear(self->cache_lru);
  g_hash_table_remove_all(self->thread_info);
  if (self->reaction_cache) g_hash_table_remove_all(self->reaction_cache);
  if (self->zap_stats_cache) g_hash_table_remove_all(self->zap_stats_cache);
  if (self->skip_animation_keys) g_hash_table_remove_all(self->skip_animation_keys);
}

/**
 * enforce_window_inline:
 * Evict oldest items from the tail of the newest-first notes array when
 * exceeding MODEL_MAX_ITEMS. Returns the number of items evicted.
 *
 * CRITICAL: This function collects keys to evict and resizes the array,
 * but does NOT clean up caches. The caller MUST call cleanup_evicted_keys()
 * AFTER emitting items_changed to avoid use-after-free during GTK widget
 * finalization.
 */
static guint enforce_window_inline(GnNostrEventModel *self, GArray **evicted_keys_out) {
  if (self->is_thread_view) return 0;
  guint cap = self->window_size ? self->window_size : MODEL_MAX_ITEMS;
  if (self->notes->len <= cap) return 0;

  guint to_remove = self->notes->len - cap;

  /* Collect keys to evict (oldest are at tail in newest-first array) */
  GArray *evicted = g_array_sized_new(FALSE, FALSE, sizeof(uint64_t), to_remove);
  for (guint i = 0; i < to_remove; i++) {
    guint idx = self->notes->len - 1 - i;
    NoteEntry *old = &g_array_index(self->notes, NoteEntry, idx);
    g_array_append_val(evicted, old->note_key);
  }

  g_array_set_size(self->notes, cap);

  if (evicted_keys_out) {
    *evicted_keys_out = evicted;
  } else {
    g_array_unref(evicted);
  }
  return to_remove;
}

/**
 * cleanup_evicted_keys:
 * Clean up caches for evicted keys. MUST be called AFTER items_changed
 * signal so GTK widgets are disposed before cache entries are freed.
 */
static void cleanup_evicted_keys(GnNostrEventModel *self, GArray *evicted_keys) {
  if (!evicted_keys) return;
  for (guint i = 0; i < evicted_keys->len; i++) {
    uint64_t k = g_array_index(evicted_keys, uint64_t, i);
    g_hash_table_remove(self->note_key_set, &k);
    cache_lru_remove_key(self, k);
    g_hash_table_remove(self->thread_info, &k);
    g_hash_table_remove(self->item_cache, &k);
    g_hash_table_remove(self->skip_animation_keys, &k);
  }
  g_array_unref(evicted_keys);
}

/**
 * process_pending_items:
 * Move items from insertion buffer to main notes array.
 * Each item is inserted at its correct position via find_sorted_position()
 * to maintain newest-first order.
 */
static void process_pending_items(GnNostrEventModel *self, guint count) {
  if (!self->insertion_buffer || self->insertion_buffer->len == 0)
    return;

  guint to_process = MIN(count, self->insertion_buffer->len);
  guint actually_processed = 0;

  /* Insert items at position 0 (prepend) to avoid the "replace all" signal
   * pattern (items_changed(0, old, new)) which causes GTK to dispose many
   * widgets simultaneously, triggering Pango layout corruption crashes.
   *
   * Insertion buffer is sorted newest-first. We process in reverse order
   * (oldest first) so items end up newest-first at the front of notes.
   * This matches the old flush_deferred_notes_cb pattern. */
  for (guint i = to_process; i > 0; i--) {
    PendingEntry *pending = &g_array_index(self->insertion_buffer, PendingEntry, i - 1);

    NoteEntry entry = {
      .note_key = pending->note_key,
      .created_at = pending->created_at
    };

    /* Prepend at position 0 — newest items end up at front */
    g_array_insert_val(self->notes, 0, entry);

    /* Move from insertion set to main set */
    remove_note_key_from_insertion_set(self, pending->note_key);
    uint64_t *set_key = g_new(uint64_t, 1);
    *set_key = pending->note_key;
    g_hash_table_add(self->note_key_set, set_key);

    /* Items prepended at 0 push visible range down — mark for skip animation */
    {
      uint64_t *anim_key = g_new(uint64_t, 1);
      *anim_key = pending->note_key;
      g_hash_table_insert(self->skip_animation_keys, anim_key, GINT_TO_POINTER(1));
    }

    actually_processed++;
  }

  if (actually_processed > 0) {
    g_array_remove_range(self->insertion_buffer, 0, actually_processed);
    g_debug("[FRAME] Processed %u pending items, %u remaining",
            actually_processed, self->insertion_buffer->len);
  }
}

/**
 * on_drain_timer:
 * Called ~60 times/sec by GLib timeout. Processes pending items from insertion
 * buffer using adaptive batch sizing, emits a single batched items_changed signal.
 *
 * Adaptive drain rate: buffer depth controls batch size each tick.
 * Deep buffer (startup flood) → drain aggressively (up to ITEMS_PER_FRAME_MAX).
 * Shallow buffer (steady state) → conservative (ITEMS_PER_FRAME_FLOOR).
 * Sub-batches of 10 with inline frame-time guard.
 */
static gboolean on_drain_timer(gpointer user_data) {
  GnNostrEventModel *self = GN_NOSTR_EVENT_MODEL(user_data);
  if (!GN_IS_NOSTR_EVENT_MODEL(self)) {
    return G_SOURCE_REMOVE;
  }

  gint64 start_us = g_get_monotonic_time();
  guint buffer_depth = self->insertion_buffer->len;
  guint total_processed = 0;

  if (buffer_depth > 0) {
    /* Adaptive batch sizing based on buffer depth */
    guint batch_limit;
    if (buffer_depth > 50) {
      batch_limit = ITEMS_PER_FRAME_MAX;   /* 50 — aggressive startup drain */
    } else if (buffer_depth > 20) {
      batch_limit = 20;
    } else if (buffer_depth > 10) {
      batch_limit = 10;
    } else {
      batch_limit = ITEMS_PER_FRAME_FLOOR; /* 3 — smooth steady state */
    }

    guint old_count = self->notes->len;

    /* Process in sub-batches of 10 with frame-time guard */
    while (total_processed < batch_limit && self->insertion_buffer->len > 0) {
      guint chunk = MIN(10, MIN(batch_limit - total_processed,
                                self->insertion_buffer->len));
      process_pending_items(self, chunk);
      total_processed += chunk;

      if (total_processed >= 10) {
        gint64 elapsed = g_get_monotonic_time() - start_us;
        if (elapsed > FRAME_BUDGET_US) {
          g_debug("[FRAME] Budget hit at %u items (%ldus), yielding",
                  total_processed, (long)elapsed);
          break;
        }
      }
    }

    /* Emit safe addition signal: items were prepended at position 0.
     * This avoids the "replace all" pattern (items_changed(0, old, new)) which
     * causes mass widget disposal and Pango layout corruption crashes. */
    if (total_processed > 0) {
      g_list_model_items_changed(G_LIST_MODEL(self), 0, 0, total_processed);
      g_debug("[FRAME] Inserted %u items at front, model now %u",
              total_processed, self->notes->len);
    }

    /* Track unseen items when user is scrolled down.
     * Throttle signal emission to avoid per-frame toast label updates. */
    if (!self->user_at_top && total_processed > 0) {
      self->unseen_count += total_processed;
      gboolean is_last_batch = (self->insertion_buffer->len == 0);
      if (is_last_batch ||
          (start_us - self->last_pending_signal_us >= PENDING_SIGNAL_INTERVAL_US)) {
        self->last_pending_signal_us = start_us;
        g_signal_emit(self, signals[SIGNAL_NEW_ITEMS_PENDING], 0, self->unseen_count);
      }
    }
  }

  /* Phase 2: Window eviction — ONLY when no items were inserted this frame.
   * CRITICAL: Two items_changed signals in one frame (insert at 0 + evict at
   * tail) causes a GTK widget recycling storm. During rapid recycle, GtkPicture's
   * internal GtkImageDefinition can get corrupted, triggering:
   *   Gtk:ERROR:gtkimagedefinition.c:156:gtk_image_definition_unref:
   *     code should not be reached
   * By deferring eviction to a frame with no insertions, we guarantee at most
   * ONE items_changed signal per frame. */
  if (total_processed == 0) {
    guint cap = self->window_size ? self->window_size : MODEL_MAX_ITEMS;
    if (self->notes->len > cap) {
      guint pre_evict = self->notes->len;
      GArray *evicted_keys = NULL;
      guint evicted = enforce_window_inline(self, &evicted_keys);
      if (evicted > 0) {
        g_list_model_items_changed(G_LIST_MODEL(self), self->notes->len, evicted, 0);
        cleanup_evicted_keys(self, evicted_keys);
        g_debug("[FRAME] Evicted %u items from tail, model %u -> %u",
                evicted, pre_evict, self->notes->len);
      }
    }
  }

  /* Continue while there is work remaining OR eviction is needed */
  {
    guint cap = self->window_size ? self->window_size : MODEL_MAX_ITEMS;
    if (self->insertion_buffer->len > 0 || self->notes->len > cap) {
      return G_SOURCE_CONTINUE;
    }
  }

  g_debug("[FRAME] All work complete, removing drain timer");
  self->tick_source_id = 0;
  g_object_unref(self);  /* balance the ref from ensure_drain_timer */
  return G_SOURCE_REMOVE;
}

/**
 * gn_nostr_event_model_set_drain_enabled:
 * @self: The model
 * @enabled: %TRUE to enable frame-rate drain, %FALSE to disable
 *
 * Enable or disable the insertion buffer drain timer.
 * Call with %TRUE after the model is attached to a visible view.
 * If there are already pending items, the timer starts immediately.
 */
void gn_nostr_event_model_set_drain_enabled(GnNostrEventModel *self, gboolean enabled) {
  g_return_if_fail(GN_IS_NOSTR_EVENT_MODEL(self));

  if (self->drain_enabled == enabled) return;
  self->drain_enabled = enabled;

  if (enabled) {
    g_debug("[FRAME] Drain enabled");
    if (self->insertion_buffer && self->insertion_buffer->len > 0) {
      ensure_drain_timer(self);
    }
  } else {
    g_debug("[FRAME] Drain disabled");
    remove_drain_timer(self);
  }
}

/* ============== End Insertion Buffer Pipeline ============== */

/* Parse NIP-10 tags for threading (best-effort; used on refresh paths that have full event JSON).
 * NIP-10 specifies two modes:
 *   1. Preferred: explicit markers - ["e", id, relay, "root"|"reply"|"mention"]
 *   2. Fallback: positional - first e-tag = root, last e-tag = reply (if different)
 */
static void parse_nip10_tags(NostrEvent *evt, char **root_id, char **reply_id) {
  *root_id = NULL;
  *reply_id = NULL;

  NostrTags *tags = (NostrTags*)nostr_event_get_tags(evt);
  if (!tags) return;

  const char *first_e_id = NULL;
  const char *last_e_id = NULL;

  for (size_t i = 0; i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (!tag || nostr_tag_size(tag) < 2) continue;

    const char *key = nostr_tag_get(tag, 0);
    if (strcmp(key, "e") != 0) continue;

    const char *event_id = nostr_tag_get(tag, 1);
    if (!event_id || strlen(event_id) != 64) continue;

    const char *marker = (nostr_tag_size(tag) >= 4) ? nostr_tag_get(tag, 3) : NULL;

    if (marker && strcmp(marker, "root") == 0) {
      g_free(*root_id);
      *root_id = g_strdup(event_id);
    } else if (marker && strcmp(marker, "reply") == 0) {
      g_free(*reply_id);
      *reply_id = g_strdup(event_id);
    } else if (marker && strcmp(marker, "mention") == 0) {
      /* Mentions are not part of the reply chain, skip */
      continue;
    } else {
      /* No marker - track for positional fallback */
      if (!first_e_id) first_e_id = event_id;
      last_e_id = event_id;
    }
  }

  /* NIP-10 positional fallback: if no explicit markers found.
   * When there's only one e-tag (first == last), the event is a direct reply
   * to that event, so both root and reply should point to it.
   * nostrc-5b8: Fix single e-tag case where reply_id was incorrectly left NULL */
  if (!*root_id && first_e_id) {
    *root_id = g_strdup(first_e_id);
  }
  if (!*reply_id && last_e_id) {
    /* Any e-tag (even if same as root) indicates this is a reply */
    *reply_id = g_strdup(last_e_id);
  }
  /* nostrc-mef: NIP-10 "root-only" marker case.
   * When an event has a "root" marker but NO "reply" marker, it means
   * the event is a direct reply to the root. Set reply_id = root_id. */
  if (!*reply_id && *root_id) {
    *reply_id = g_strdup(*root_id);
  }

  /* Debug logging to trace threading issues */
  g_debug("[NIP10-MODEL] Final result - root: %s, reply: %s",
          *root_id ? *root_id : "(null)",
          *reply_id ? *reply_id : "(null)");
}

/* Compare function for sorting NoteEntry by created_at (newest first) */
static gint note_entry_compare_newest_first(gconstpointer a, gconstpointer b) {
  const NoteEntry *ea = (const NoteEntry *)a;
  const NoteEntry *eb = (const NoteEntry *)b;
  /* Newest first: higher created_at comes first */
  if (ea->created_at > eb->created_at) return -1;
  if (ea->created_at < eb->created_at) return 1;
  return 0;
}

/* Add a note to the model (assumes gating has already been satisfied) */
static void add_note_internal(GnNostrEventModel *self, uint64_t note_key, gint64 created_at,
                               const char *root_id, const char *parent_id, guint depth) {
  if (has_note_key(self, note_key)) {
    return;  /* Already in model */
  }

  /* Store thread info FIRST (before potential deferral) so it's available
   * when the note is eventually flushed into the model. Without this,
   * deferred notes would lose their threading context. */
  if (root_id || parent_id) {
    /* Check if we already have thread info for this key */
    if (!g_hash_table_contains(self->thread_info, &note_key)) {
      ThreadInfo *tinfo = g_new0(ThreadInfo, 1);
      tinfo->root_id = g_strdup(root_id);
      tinfo->parent_id = g_strdup(parent_id);
      tinfo->depth = depth;

      uint64_t *key_copy = g_new(uint64_t, 1);
      *key_copy = note_key;
      g_hash_table_insert(self->thread_info, key_copy, tinfo);
      g_debug("[NIP10-MODEL] Stored thread info for key %lu: root=%.16s... parent=%.16s...",
              (unsigned long)note_key,
              root_id ? root_id : "(null)",
              parent_id ? parent_id : "(null)");
    }
  }

  /* Find insertion position */
  guint pos = find_sorted_position(self, created_at);

  /* nostrc-7o7: Mark items added outside visible viewport to skip animation */
  if (pos < self->visible_start || pos > self->visible_end) {
    uint64_t *key_copy = g_new(uint64_t, 1);
    *key_copy = note_key;
    g_hash_table_insert(self->skip_animation_keys, key_copy, GINT_TO_POINTER(1));
  }

  /* Insert note entry */
  NoteEntry entry = { .note_key = note_key, .created_at = created_at };
  g_array_insert_val(self->notes, pos, entry);
  uint64_t *set_key = g_new(uint64_t, 1);
  *set_key = note_key;
  g_hash_table_add(self->note_key_set, set_key);

  /* Emit items-changed signal immediately for each insertion */
  g_list_model_items_changed(G_LIST_MODEL(self), pos, 0, 1);
}

/* nostrc-5r8b: Helper to insert note without emitting signal (for batching) */
static gboolean insert_note_silent(GnNostrEventModel *self, uint64_t note_key, gint64 created_at,
                                    const char *root_id, const char *parent_id, guint depth) {
  if (has_note_key(self, note_key)) return FALSE;

  /* Store thread info */
  if (root_id || parent_id) {
    if (!g_hash_table_contains(self->thread_info, &note_key)) {
      ThreadInfo *tinfo = g_new0(ThreadInfo, 1);
      tinfo->root_id = g_strdup(root_id);
      tinfo->parent_id = g_strdup(parent_id);
      tinfo->depth = depth;
      uint64_t *key_copy = g_new(uint64_t, 1);
      *key_copy = note_key;
      g_hash_table_insert(self->thread_info, key_copy, tinfo);
    }
  }

  /* Find sorted position and insert */
  guint pos = find_sorted_position(self, created_at);
  if (pos < self->visible_start || pos > self->visible_end) {
    uint64_t *key_copy = g_new(uint64_t, 1);
    *key_copy = note_key;
    g_hash_table_insert(self->skip_animation_keys, key_copy, GINT_TO_POINTER(1));
  }

  NoteEntry entry = { .note_key = note_key, .created_at = created_at };
  g_array_insert_val(self->notes, pos, entry);
  uint64_t *set_key = g_new(uint64_t, 1);
  *set_key = note_key;
  g_hash_table_add(self->note_key_set, set_key);
  return TRUE;  /* Actually inserted */
}

/* Remove a note from the visible list by note_key (incremental). */
static gboolean remove_note_by_key(GnNostrEventModel *self, uint64_t note_key) {
  if (!self || !self->notes) return FALSE;

  for (guint i = 0; i < self->notes->len; i++) {
    NoteEntry *entry = &g_array_index(self->notes, NoteEntry, i);
    if (entry->note_key != note_key) continue;

    /* Remove visible entry and emit change FIRST so GTK can tear down widgets
     * while cached items are still valid */
    g_array_remove_index(self->notes, i);
    g_hash_table_remove(self->note_key_set, &note_key);
    g_list_model_items_changed(G_LIST_MODEL(self), i, 1, 0);

    /* NOW cleanup caches after GTK has finished with widgets */
    cache_lru_remove_key(self, note_key);
    g_hash_table_remove(self->thread_info, &note_key);
    g_hash_table_remove(self->item_cache, &note_key);
    g_hash_table_remove(self->skip_animation_keys, &note_key);

    return TRUE;
  }

  return FALSE;
}

/* Handle NIP-09 deletes (kind 5) by parsing tags and removing referenced notes incrementally.
 * SECURITY: Per NIP-09, we MUST validate that deletion_event.pubkey === target_event.pubkey
 * before hiding/deleting any event. This prevents unauthorized deletion of others' events.
 */
static void handle_delete_event_json(GnNostrEventModel *self, void *txn, const char *event_json) {
  if (!self || !txn || !event_json) return;

  NostrEvent *evt = nostr_event_new();
  if (!evt) return;

  if (nostr_event_deserialize(evt, event_json) != 0 || nostr_event_get_kind(evt) != 5) {
    nostr_event_free(evt);
    return;
  }

  /* Get the deletion event's pubkey for authorization check */
  const char *deletion_pubkey = nostr_event_get_pubkey(evt);
  if (!deletion_pubkey || strlen(deletion_pubkey) != 64) {
    nostr_event_free(evt);
    return;
  }

  uint8_t deletion_pk32[32];
  if (!hex_to_bytes32(deletion_pubkey, deletion_pk32)) {
    nostr_event_free(evt);
    return;
  }

  NostrTags *tags = (NostrTags *)nostr_event_get_tags(evt);
  if (!tags) {
    nostr_event_free(evt);
    return;
  }

  for (size_t i = 0; i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (!tag || nostr_tag_size(tag) < 2) continue;

    const char *k = nostr_tag_get(tag, 0);
    if (!k) continue;

    if (strcmp(k, "e") == 0) {
      const char *id_hex = nostr_tag_get(tag, 1);
      if (!id_hex || strlen(id_hex) != 64) continue;

      uint8_t id32[32];
      if (!hex_to_bytes32(id_hex, id32)) continue;

      storage_ndb_note *target_note = NULL;
      uint64_t target_key = storage_ndb_get_note_key_by_id(txn, id32, &target_note);
      if (target_key == 0 || !target_note) continue;

      /* NIP-09 SECURITY: Verify deletion event pubkey matches target event pubkey.
       * Only the author of an event can request its deletion. */
      const unsigned char *target_pk32 = storage_ndb_note_pubkey(target_note);
      if (!target_pk32) continue;

      if (memcmp(deletion_pk32, target_pk32, 32) != 0) {
        /* Pubkey mismatch - unauthorized deletion attempt, skip this target */
        g_debug("[NIP-09] Rejected deletion: pubkey mismatch for event %s", id_hex);
        continue;
      }

      /* Authorized: pubkeys match, proceed with deletion */
      (void)remove_note_by_key(self, target_key);
    }
  }

  nostr_event_free(evt);
}

/* -------------------- GListModel interface implementation -------------------- */

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
    /* nostrc-7o7: Check if this item should skip animation */
    gboolean skip_anim = g_hash_table_contains(self->skip_animation_keys, &key);
    gn_nostr_event_item_set_skip_animation(item, skip_anim);
    /* nostrc-5b8: Apply thread info even for cached items, in case it was added
     * after the item was first cached (e.g., during refresh or later processing) */
    ThreadInfo *tinfo = g_hash_table_lookup(self->thread_info, &key);
    if (tinfo) {
      gn_nostr_event_item_set_thread_info(item, tinfo->root_id, tinfo->parent_id, tinfo->depth);
    }
    /* Check if profile needs to be applied or fetched */
    if (!gn_nostr_event_item_get_profile(item)) {
      const char *pubkey = gn_nostr_event_item_get_pubkey(item);
      if (pubkey) {
        GNostrProfile *profile = profile_cache_get(self, pubkey);
        if (profile) {
          gn_nostr_event_item_set_profile(item, profile);
        } else {
          g_signal_emit(self, signals[SIGNAL_NEED_PROFILE], 0, pubkey);
        }
      }
    }
    return g_object_ref(item);
  }

  /* Create new item from nostrdb */
  item = gn_nostr_event_item_new_from_key(key, entry->created_at);

  /* nostrc-7o7: Set skip_animation flag based on whether item was added outside viewport */
  gboolean skip_anim = g_hash_table_contains(self->skip_animation_keys, &key);
  gn_nostr_event_item_set_skip_animation(item, skip_anim);

  /* Apply thread info if available */
  ThreadInfo *tinfo = g_hash_table_lookup(self->thread_info, &key);
  if (tinfo) {
    gn_nostr_event_item_set_thread_info(item, tinfo->root_id, tinfo->parent_id, tinfo->depth);
    g_debug("[NIP10-MODEL] Applied thread info to item key %lu: root=%.16s... parent=%.16s...",
            (unsigned long)key,
            tinfo->root_id ? tinfo->root_id : "(null)",
            tinfo->parent_id ? tinfo->parent_id : "(null)");
  } else {
    g_debug("[NIP10-MODEL] No thread info found for item key %lu", (unsigned long)key);
  }

  /* Apply profile if available, otherwise request fetch.
   * This is critical for items that were evicted from cache and recreated -
   * their profiles might still not be loaded. */
  const char *pubkey = gn_nostr_event_item_get_pubkey(item);
  if (pubkey) {
    GNostrProfile *profile = profile_cache_get(self, pubkey);
    if (profile) {
      gn_nostr_event_item_set_profile(item, profile);
    } else {
      /* Profile not cached - emit need-profile to trigger fetch */
      g_signal_emit(self, signals[SIGNAL_NEED_PROFILE], 0, pubkey);
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

/* -------------------- Subscriptions -------------------- */

static void on_sub_profiles_batch(uint64_t subid, const uint64_t *note_keys, guint n_keys, gpointer user_data) {
  (void)subid;
  GnNostrEventModel *self = GN_NOSTR_EVENT_MODEL(user_data);
  if (!GN_IS_NOSTR_EVENT_MODEL(self) || !note_keys || n_keys == 0) return;

  void *txn = NULL;
  if (storage_ndb_begin_query(&txn, NULL) != 0 || !txn) return;

  for (guint i = 0; i < n_keys; i++) {
    uint64_t note_key = note_keys[i];
    storage_ndb_note *note = storage_ndb_get_note_ptr(txn, note_key);
    if (!note) continue;

    uint32_t kind = storage_ndb_note_kind(note);
    if (kind != 0) continue;

    const unsigned char *pk32 = storage_ndb_note_pubkey(note);
    if (!pk32) continue;

    char pubkey_hex[65];
    storage_ndb_hex_encode(pk32, pubkey_hex);

    const char *content = storage_ndb_note_content(note);
    uint32_t content_len = storage_ndb_note_content_length(note);
    if (!content || content_len == 0) continue;

    profile_cache_update_from_content(self, pubkey_hex, content, (gsize)content_len);
    notify_cached_items_for_pubkey(self, pubkey_hex);
  }

  storage_ndb_end_query(txn);
}

/**
 * timeline_batch_thread_func:
 *
 * Worker thread: opens NDB read txn, validates each note key, extracts NIP-10
 * thread info, checks kind/author/time filters and mute list. Produces a
 * GArray<TimelineBatchEntry> and collects unique pubkeys for profile prefetch.
 *
 * Thread-safe operations only: NDB reads, mute list (has internal GMutex).
 * GHashTable dedup is deferred to the main-thread completion callback.
 */
static void timeline_batch_thread_func(GTask        *task,
                                        gpointer      source_object,
                                        gpointer      task_data,
                                        GCancellable *cancellable) {
  (void)source_object;
  (void)cancellable;

  TimelineBatchProcessData *bp = task_data;
  bp->validated = g_array_sized_new(FALSE, FALSE, sizeof(TimelineBatchEntry), bp->n_keys);
  g_array_set_clear_func(bp->validated, timeline_batch_entry_clear);

  GHashTable *pk_set = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  void *txn = NULL;
  if (storage_ndb_begin_query(&txn, NULL) != 0 || !txn) {
    g_hash_table_destroy(pk_set);
    g_task_return_pointer(task, bp, NULL);
    return;
  }

  for (guint i = 0; i < bp->n_keys; i++) {
    uint64_t note_key = bp->note_keys[i];

    storage_ndb_note *note = storage_ndb_get_note_ptr(txn, note_key);
    if (!note) continue;

    int kind = (int)storage_ndb_note_kind(note);
    if (kind != 1 && kind != 6 && kind != 1111 && kind != 9735) continue;
    if (storage_ndb_note_is_expired(note)) continue;

    gint64 created_at = (gint64)storage_ndb_note_created_at(note);

    const unsigned char *pk32 = storage_ndb_note_pubkey(note);
    if (!pk32) continue;

    char pubkey_hex[65];
    storage_ndb_hex_encode(pk32, pubkey_hex);

    /* Mute check — gnostr_mute_list uses internal GMutex, thread-safe */
    GNostrMuteList *mute_list = gnostr_mute_list_get_default();
    if (mute_list && gnostr_mute_list_is_pubkey_muted(mute_list, pubkey_hex))
      continue;

    /* Kind filter from snapshot */
    if (bp->n_kinds > 0) {
      gboolean kind_ok = FALSE;
      for (gsize k = 0; k < bp->n_kinds; k++) {
        if (bp->kinds[k] == kind) { kind_ok = TRUE; break; }
      }
      if (!kind_ok) continue;
    }

    /* Author filter from snapshot */
    if (bp->n_authors > 0) {
      gboolean auth_ok = FALSE;
      for (gsize a = 0; a < bp->n_authors; a++) {
        if (bp->authors[a] && g_strcmp0(bp->authors[a], pubkey_hex) == 0) {
          auth_ok = TRUE; break;
        }
      }
      if (!auth_ok) continue;
    }

    /* Time range from snapshot */
    if (bp->since > 0 && created_at > 0 && created_at < bp->since) continue;
    if (bp->until > 0 && created_at > 0 && created_at > bp->until) continue;

    /* NIP-10 thread info extraction (thread-safe NDB read) */
    char *root_id = NULL;
    char *reply_id = NULL;
    storage_ndb_note_get_nip10_thread(note, &root_id, &reply_id);

    TimelineBatchEntry entry = {
      .note_key = note_key,
      .created_at = created_at,
      .root_id = root_id,     /* ownership transferred to GArray */
      .reply_id = reply_id,   /* ownership transferred to GArray */
      .kind = kind
    };
    memcpy(entry.pubkey_hex, pubkey_hex, 65);
    g_array_append_val(bp->validated, entry);

    /* Collect unique pubkeys for profile prefetch */
    if (!g_hash_table_contains(pk_set, pubkey_hex)) {
      g_hash_table_add(pk_set, g_strdup(pubkey_hex));
    }
  }

  storage_ndb_end_query(txn);

  /* Convert pubkey set to GPtrArray for main-thread callback */
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
 * timeline_batch_complete_cb:
 *
 * Main-thread callback. Performs dedup (GHashTable), profile caching,
 * meta count increments, thread info storage, item precaching, and
 * inserts validated entries into the insertion buffer for frame-synced drain.
 */
static void timeline_batch_complete_cb(GObject      *source_object,
                                        GAsyncResult *res,
                                        gpointer      user_data) {
  (void)user_data;
  GnNostrEventModel *self = GN_NOSTR_EVENT_MODEL(source_object);
  if (!GN_IS_NOSTR_EVENT_MODEL(self)) return;

  GTask *task = G_TASK(res);
  TimelineBatchProcessData *bp = g_task_propagate_pointer(task, NULL);
  if (!bp || !bp->validated || bp->validated->len == 0) {
    if (bp) timeline_batch_data_free(bp);
    return;
  }

  /* Open short NDB txn for profile caching and item precaching */
  void *txn = NULL;
  gboolean have_txn = (storage_ndb_begin_query(&txn, NULL) == 0 && txn != NULL);

  guint inserted_count = 0;
  guint direct_inserted = 0;
  guint old_len = self->notes->len;
  gint64 arrival_time_us = g_get_monotonic_time();

  for (guint i = 0; i < bp->validated->len; i++) {
    TimelineBatchEntry *ve = &g_array_index(bp->validated, TimelineBatchEntry, i);

    /* Dedup: skip if already in main array or insertion buffer */
    if (has_note_key(self, ve->note_key)) continue;
    if (has_note_key_pending(self, ve->note_key)) continue;

    /* Profile caching (main-thread only — uses GHashTable) */
    if (have_txn && !author_is_ready(self, ve->pubkey_hex)) {
      uint8_t pk32[32];
      if (hex_to_bytes32(ve->pubkey_hex, pk32)) {
        GNostrProfile *p = profile_cache_ensure_from_db(self, txn, pk32, ve->pubkey_hex);
        if (!p)
          g_signal_emit(self, signals[SIGNAL_NEED_PROFILE], 0, ve->pubkey_hex);
      }
    }

    /* hq-vvmzu: Persist reply/repost counts to ndb_note_meta */
    if (ve->reply_id && strlen(ve->reply_id) == 64) {
      uint8_t parent_id32[32];
      if (hex_to_bytes32(ve->reply_id, parent_id32)) {
        if (ve->kind == 1 || ve->kind == 1111)
          storage_ndb_increment_note_meta(parent_id32, "direct_replies");
        else if (ve->kind == 6)
          storage_ndb_increment_note_meta(parent_id32, "reposts");
      }
    }

    /* Store NIP-10 thread info */
    if (ve->root_id || ve->reply_id) {
      if (!g_hash_table_contains(self->thread_info, &ve->note_key)) {
        ThreadInfo *tinfo = g_new0(ThreadInfo, 1);
        tinfo->root_id = g_strdup(ve->root_id);
        tinfo->parent_id = g_strdup(ve->reply_id);
        tinfo->depth = 0;
        uint64_t *key_copy = g_new(uint64_t, 1);
        *key_copy = ve->note_key;
        g_hash_table_insert(self->thread_info, key_copy, tinfo);
      }
    }

    /* Precache item data while txn is open */
    if (have_txn) {
      storage_ndb_note *note = storage_ndb_get_note_ptr(txn, ve->note_key);
      if (note)
        precache_item_from_note(self, ve->note_key, ve->created_at, note);
    }

    inserted_count++;

    /* Decide: buffer for tick drain vs direct insert.
     * If no tick widget or widget not realized (startup: Loading page visible,
     * Session page hidden in GtkStack), insert directly like the old sync path.
     * Otherwise use the frame-synced insertion buffer. */
    gboolean can_tick = self->drain_enabled;
    if (can_tick) {
      PendingEntry pentry = {
        .note_key = ve->note_key,
        .created_at = ve->created_at,
        .arrival_time_us = arrival_time_us
      };
      insertion_buffer_sorted_insert(self->insertion_buffer, &pentry);
      add_note_key_to_insertion_set(self, ve->note_key);
    } else {
      /* Direct insert (startup fallback): sorted insert + track in note_key_set */
      if (insert_note_silent(self, ve->note_key, ve->created_at,
                             ve->root_id, ve->reply_id, 0)) {
        direct_inserted++;
      }
    }
  }

  if (have_txn)
    storage_ndb_end_query(txn);

  /* Emit batched signal for direct inserts (startup path).
   * CRITICAL: evict BEFORE signal to keep model consistent — same pattern
   * as refresh and on_refresh_async_done. */
  if (direct_inserted > 0) {
    GArray *evicted_keys = NULL;
    enforce_window_inline(self, &evicted_keys);
    g_list_model_items_changed(G_LIST_MODEL(self), 0, old_len, self->notes->len);
    cleanup_evicted_keys(self, evicted_keys);
    g_debug("[INSERT] Direct insert: %u items (startup fallback), model now %u",
            direct_inserted, self->notes->len);
  }

  /* Queue pipeline drain for buffered inserts (live events) */
  if (inserted_count > direct_inserted) {
    guint buffered = inserted_count - direct_inserted;
    if (self->insertion_buffer->len > self->peak_insertion_depth) {
      self->peak_insertion_depth = self->insertion_buffer->len;
    }

    g_debug("[INSERT] Buffered %u items for tick drain (pending: %u)",
            buffered, self->insertion_buffer->len);

    apply_insertion_backpressure(self);

    if (self->insertion_buffer->len < INSERTION_BUFFER_MAX) {
      self->backpressure_active = FALSE;
    }

    ensure_drain_timer(self);
  }

  /* Background profile prefetch for unique pubkeys */
  if (bp->prefetch_pubkeys && bp->prefetch_pubkeys->len > 0) {
    const gchar **pk_array = g_new0(const gchar *, bp->prefetch_pubkeys->len + 1);
    for (guint i = 0; i < bp->prefetch_pubkeys->len; i++) {
      pk_array[i] = g_ptr_array_index(bp->prefetch_pubkeys, i);
    }
    pk_array[bp->prefetch_pubkeys->len] = NULL;
    gnostr_profile_provider_prefetch_batch_async(pk_array);
    g_free(pk_array);
  }

  timeline_batch_data_free(bp);
}

/**
 * on_sub_timeline_batch:
 *
 * NDB subscription callback for timeline events (kinds 1/6/1111/9735).
 * Lightweight dispatcher: copies note keys, snapshots filter params,
 * dispatches GTask to worker thread for NDB reads.
 */
static void on_sub_timeline_batch(uint64_t subid, const uint64_t *note_keys, guint n_keys, gpointer user_data) {
  (void)subid;
  GnNostrEventModel *self = GN_NOSTR_EVENT_MODEL(user_data);
  if (!GN_IS_NOSTR_EVENT_MODEL(self) || !note_keys || n_keys == 0) return;

  /* Allocate and populate batch data with snapshots of filter params */
  TimelineBatchProcessData *bp = g_new0(TimelineBatchProcessData, 1);
  bp->note_keys = g_memdup2(note_keys, n_keys * sizeof(uint64_t));
  bp->n_keys = n_keys;

  /* Snapshot filter params — worker thread must not touch self->kinds etc. */
  if (self->n_kinds > 0 && self->kinds) {
    bp->kinds = g_memdup2(self->kinds, self->n_kinds * sizeof(gint));
    bp->n_kinds = self->n_kinds;
  }
  if (self->n_authors > 0 && self->authors) {
    bp->authors = g_new0(char *, self->n_authors);
    for (gsize i = 0; i < self->n_authors; i++)
      bp->authors[i] = g_strdup(self->authors[i]);
    bp->n_authors = self->n_authors;
  }
  bp->since = self->since;
  bp->until = self->until;

  GTask *task = g_task_new(self, NULL, timeline_batch_complete_cb, NULL);
  g_task_set_task_data(task, bp, NULL);  /* bp freed in complete_cb */
  g_task_run_in_thread(task, timeline_batch_thread_func);
  g_object_unref(task);
}

static void on_sub_deletes_batch(uint64_t subid, const uint64_t *note_keys, guint n_keys, gpointer user_data) {
  (void)subid;
  GnNostrEventModel *self = GN_NOSTR_EVENT_MODEL(user_data);
  if (!GN_IS_NOSTR_EVENT_MODEL(self) || !note_keys || n_keys == 0) return;

  void *txn = NULL;
  if (storage_ndb_begin_query(&txn, NULL) != 0 || !txn) return;

  for (guint i = 0; i < n_keys; i++) {
    uint64_t del_key = note_keys[i];
    storage_ndb_note *note = storage_ndb_get_note_ptr(txn, del_key);
    if (!note) continue;

    uint32_t kind = storage_ndb_note_kind(note);
    if (kind != 5) continue;

    /* We don't have tag APIs from note ptr, so query full JSON by id to parse tags. */
    const unsigned char *id32 = storage_ndb_note_id(note);
    if (!id32) continue;

    char id_hex[65];
    storage_ndb_hex_encode(id32, id_hex);

    char **arr = NULL;
    int n = 0;
    char *filter = g_strdup_printf("[{\"ids\":[\"%s\"]}]", id_hex);
    int qrc = storage_ndb_query(txn, filter, &arr, &n, NULL);
    g_free(filter);

    if (qrc == 0 && arr && n > 0 && arr[0]) {
      handle_delete_event_json(self, txn, arr[0]);
    }

    storage_ndb_free_results(arr, n);
  }

  storage_ndb_end_query(txn);
}

/* Helper: Update cached item's reaction count */
static void update_item_reaction_count(GnNostrEventModel *self, const char *event_id_hex) {
  if (!self || !self->item_cache || !event_id_hex) return;

  /* Find the item in cache by iterating (items are keyed by note_key, not event_id) */
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, self->item_cache);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    GnNostrEventItem *item = GN_NOSTR_EVENT_ITEM(value);
    const char *item_id = gn_nostr_event_item_get_event_id(item);
    if (item_id && g_strcmp0(item_id, event_id_hex) == 0) {
      /* Found matching item - update its reaction count from cache */
      gpointer cached = g_hash_table_lookup(self->reaction_cache, event_id_hex);
      guint count = cached ? GPOINTER_TO_UINT(cached) : 0;
      gn_nostr_event_item_set_like_count(item, count);
      break;
    }
  }
}

/* Helper: Update cached item's zap stats */
static void update_item_zap_stats(GnNostrEventModel *self, const char *event_id_hex) {
  if (!self || !self->item_cache || !event_id_hex) return;

  /* Find the item in cache by iterating */
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, self->item_cache);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    GnNostrEventItem *item = GN_NOSTR_EVENT_ITEM(value);
    const char *item_id = gn_nostr_event_item_get_event_id(item);
    if (item_id && g_strcmp0(item_id, event_id_hex) == 0) {
      /* Found matching item - update its zap stats from cache */
      ZapStats *stats = g_hash_table_lookup(self->zap_stats_cache, event_id_hex);
      if (stats) {
        gn_nostr_event_item_set_zap_count(item, stats->count);
        gn_nostr_event_item_set_zap_total_msat(item, stats->total_msat);
      }
      break;
    }
  }
}

/* NIP-25: Process incoming reaction events (kind 7) */
static void on_sub_reactions_batch(uint64_t subid, const uint64_t *note_keys, guint n_keys, gpointer user_data) {
  (void)subid;
  GnNostrEventModel *self = GN_NOSTR_EVENT_MODEL(user_data);
  if (!GN_IS_NOSTR_EVENT_MODEL(self) || !note_keys || n_keys == 0) return;
  if (!self->reaction_cache) return;

  void *txn = NULL;
  if (storage_ndb_begin_query(&txn, NULL) != 0 || !txn) return;

  /* Track which event IDs we need to update */
  GHashTable *events_to_update = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  for (guint i = 0; i < n_keys; i++) {
    uint64_t note_key = note_keys[i];
    storage_ndb_note *note = storage_ndb_get_note_ptr(txn, note_key);
    if (!note) continue;

    uint32_t kind = storage_ndb_note_kind(note);
    if (kind != 7) continue;

    /* Extract target event ID from tags.
     * Try NIP-10 thread parsing first, then direct tag iteration as fallback.
     * Both use the note pointer directly — no NDB query needed. */
    char *target_event_id = NULL;
    storage_ndb_note_get_nip10_thread(note, NULL, &target_event_id);
    if (!target_event_id)
      target_event_id = storage_ndb_note_get_last_etag(note);

    if (target_event_id) {
      /* Increment reaction count in cache */
      gpointer existing = g_hash_table_lookup(self->reaction_cache, target_event_id);
      guint new_count = (existing ? GPOINTER_TO_UINT(existing) : 0) + 1;
      g_hash_table_insert(self->reaction_cache, g_strdup(target_event_id), GUINT_TO_POINTER(new_count));

      /* hq-vvmzu: Also persist reaction count to ndb_note_meta for O(1) reads */
      uint8_t target_id32[32];
      if (hex_to_bytes32(target_event_id, target_id32))
        storage_ndb_increment_note_meta(target_id32, "reactions");

      /* Mark for UI update */
      g_hash_table_add(events_to_update, g_strdup(target_event_id));
      g_free(target_event_id);
    }
  }

  storage_ndb_end_query(txn);

  /* Cap reaction cache to prevent unbounded growth */
  if (g_hash_table_size(self->reaction_cache) > REACTION_CACHE_MAX) {
    g_debug("[REACTION] Cache overflow (%u > %u), clearing",
            g_hash_table_size(self->reaction_cache), REACTION_CACHE_MAX);
    g_hash_table_remove_all(self->reaction_cache);
  }

  /* Update cached items */
  GHashTableIter iter;
  gpointer key;
  g_hash_table_iter_init(&iter, events_to_update);
  while (g_hash_table_iter_next(&iter, &key, NULL)) {
    update_item_reaction_count(self, (const char *)key);
  }

  g_hash_table_unref(events_to_update);
}

/* NIP-57: Process incoming zap receipt events (kind 9735) */
static void on_sub_zaps_batch(uint64_t subid, const uint64_t *note_keys, guint n_keys, gpointer user_data) {
  (void)subid;
  GnNostrEventModel *self = GN_NOSTR_EVENT_MODEL(user_data);
  if (!GN_IS_NOSTR_EVENT_MODEL(self) || !note_keys || n_keys == 0) return;
  if (!self->zap_stats_cache) return;

  void *txn = NULL;
  if (storage_ndb_begin_query(&txn, NULL) != 0 || !txn) return;

  /* Track which event IDs we need to update */
  GHashTable *events_to_update = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  for (guint i = 0; i < n_keys; i++) {
    uint64_t note_key = note_keys[i];
    storage_ndb_note *note = storage_ndb_get_note_ptr(txn, note_key);
    if (!note) continue;

    uint32_t kind = storage_ndb_note_kind(note);
    if (kind != 9735) continue;

    /* Extract target event ID via direct tag iteration (no NDB query) */
    char *target_event_id = storage_ndb_note_get_last_etag(note);

    if (target_event_id) {
      /* Get fresh stats from storage (includes this new zap) */
      guint zap_count = 0;
      gint64 total_msat = 0;
      storage_ndb_get_zap_stats(target_event_id, &zap_count, &total_msat);

      /* Update cache */
      ZapStats *stats = g_hash_table_lookup(self->zap_stats_cache, target_event_id);
      if (!stats) {
        stats = g_new0(ZapStats, 1);
        g_hash_table_insert(self->zap_stats_cache, g_strdup(target_event_id), stats);
      }
      stats->count = zap_count;
      stats->total_msat = total_msat;

      /* Mark for UI update */
      g_hash_table_add(events_to_update, g_strdup(target_event_id));
      g_free(target_event_id);
    }
  }

  storage_ndb_end_query(txn);

  /* Cap zap cache to prevent unbounded growth */
  if (g_hash_table_size(self->zap_stats_cache) > ZAP_CACHE_MAX) {
    g_debug("[ZAP] Cache overflow (%u > %u), clearing",
            g_hash_table_size(self->zap_stats_cache), ZAP_CACHE_MAX);
    g_hash_table_remove_all(self->zap_stats_cache);
  }

  /* Update cached items */
  GHashTableIter iter;
  gpointer key;
  g_hash_table_iter_init(&iter, events_to_update);
  while (g_hash_table_iter_next(&iter, &key, NULL)) {
    update_item_zap_stats(self, (const char *)key);
  }

  g_hash_table_unref(events_to_update);
}

/* -------------------- GObject boilerplate -------------------- */

static void gn_nostr_event_model_finalize(GObject *object) {
  GnNostrEventModel *self = GN_NOSTR_EVENT_MODEL(object);

  /* Unsubscribe from nostrdb via dispatcher */
  if (self->sub_timeline > 0)   { gn_ndb_unsubscribe(self->sub_timeline);   self->sub_timeline = 0;   }
  if (self->sub_profiles > 0)   { gn_ndb_unsubscribe(self->sub_profiles);   self->sub_profiles = 0;   }
  if (self->sub_deletes > 0)    { gn_ndb_unsubscribe(self->sub_deletes);    self->sub_deletes = 0;    }
  if (self->sub_reactions > 0)  { gn_ndb_unsubscribe(self->sub_reactions);  self->sub_reactions = 0;  }
  if (self->sub_zaps > 0)       { gn_ndb_unsubscribe(self->sub_zaps);       self->sub_zaps = 0;       }

  /* NIP-25/57: Clean up reaction and zap caches */
  if (self->reaction_cache) g_hash_table_unref(self->reaction_cache);
  if (self->zap_stats_cache) g_hash_table_unref(self->zap_stats_cache);

  /* Free timeline query */
  if (self->timeline_query) {
    gnostr_timeline_query_free(self->timeline_query);
    self->timeline_query = NULL;
  }

  g_free(self->kinds);
  g_strfreev(self->authors);
  g_free(self->root_event_id);

  if (self->notes) g_array_unref(self->notes);
  if (self->note_key_set) g_hash_table_unref(self->note_key_set);
  if (self->item_cache) g_hash_table_unref(self->item_cache);
  if (self->cache_lru) g_queue_free(self->cache_lru);
  if (self->profile_cache) g_hash_table_unref(self->profile_cache);
  if (self->profile_cache_lru) {
    g_queue_free_full(self->profile_cache_lru, g_free);
  }
  if (self->authors_ready) g_hash_table_unref(self->authors_ready);
  if (self->authors_ready_lru) {
    g_queue_free_full(self->authors_ready_lru, g_free);
  }
  if (self->thread_info) g_hash_table_unref(self->thread_info);

  /* nostrc-7o7: Clean up animation skip tracking */
  if (self->skip_animation_keys) g_hash_table_unref(self->skip_animation_keys);

  /* Clean up pipeline drain timer and insertion buffer */
  remove_drain_timer(self);
  if (self->insertion_buffer) g_array_unref(self->insertion_buffer);
  if (self->insertion_key_set) g_hash_table_unref(self->insertion_key_set);

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

  properties[PROP_IS_THREAD_VIEW] =
    g_param_spec_boolean("is-thread-view", "Is Thread View",
                         "Whether this is a thread view", FALSE,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  properties[PROP_ROOT_EVENT_ID] =
    g_param_spec_string("root-event-id", "Root Event ID",
                        "Thread root event ID", NULL,
                        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties(object_class, N_PROPS, properties);

  /* need-profile(pubkey_hex): emitted when kind {1,6} arrives but author has no kind 0 in DB. */
  signals[SIGNAL_NEED_PROFILE] =
    g_signal_new("need-profile",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0,
                 NULL, NULL,
                 g_cclosure_marshal_VOID__STRING,
                 G_TYPE_NONE,
                 1,
                 G_TYPE_STRING);

  /* nostrc-yi2: new-items-pending(count): emitted when new items are waiting due to scroll position */
  signals[SIGNAL_NEW_ITEMS_PENDING] =
    g_signal_new("new-items-pending",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0,
                 NULL, NULL,
                 g_cclosure_marshal_VOID__UINT,
                 G_TYPE_NONE,
                 1,
                 G_TYPE_UINT);
}

static void gn_nostr_event_model_init(GnNostrEventModel *self) {
  self->notes = g_array_new(FALSE, FALSE, sizeof(NoteEntry));
  self->note_key_set = g_hash_table_new_full(uint64_hash, uint64_equal, g_free, NULL);

  self->item_cache = g_hash_table_new_full(uint64_hash, uint64_equal, g_free, g_object_unref);
  self->cache_lru = g_queue_new();

  self->profile_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
  self->profile_cache_lru = g_queue_new();
  self->thread_info = g_hash_table_new_full(uint64_hash, uint64_equal, g_free, (GDestroyNotify)thread_info_free);

  self->authors_ready = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->authors_ready_lru = g_queue_new();

  self->limit = MODEL_MAX_ITEMS;
  self->window_size = MODEL_MAX_ITEMS;

  /* nostrc-7o7: Initialize animation skip tracking */
  self->visible_start = 0;
  self->visible_end = 10;  /* Default to showing first 10 items as "visible" */
  self->skip_animation_keys = g_hash_table_new_full(uint64_hash, uint64_equal, g_free, NULL);

  /* Scroll position awareness and pipeline */
  self->user_at_top = TRUE;  /* Assume user starts at top */
  self->unseen_count = 0;
  self->insertion_buffer = g_array_new(FALSE, FALSE, sizeof(PendingEntry));
  self->insertion_key_set = g_hash_table_new_full(uint64_hash, uint64_equal, g_free, NULL);

  /* NIP-25/57: Initialize reaction and zap caches */
  self->reaction_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->zap_stats_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)zap_stats_free);

  /* Install lifetime subscriptions via dispatcher (marshals to main loop) */
  self->sub_profiles  = gn_ndb_subscribe(FILTER_PROFILES,  on_sub_profiles_batch,  self, NULL);
  self->sub_timeline  = gn_ndb_subscribe(FILTER_TIMELINE,  on_sub_timeline_batch,  self, NULL);
  self->sub_deletes   = gn_ndb_subscribe(FILTER_DELETES,   on_sub_deletes_batch,   self, NULL);
  self->sub_reactions = gn_ndb_subscribe(FILTER_REACTIONS, on_sub_reactions_batch, self, NULL);
  self->sub_zaps      = gn_ndb_subscribe(FILTER_ZAPS,      on_sub_zaps_batch,      self, NULL);
}

/* -------------------- Public API -------------------- */

GnNostrEventModel *gn_nostr_event_model_new(void) {
  return g_object_new(GN_TYPE_NOSTR_EVENT_MODEL, NULL);
}

GnNostrEventModel *gn_nostr_event_model_new_with_query(GNostrTimelineQuery *query) {
  GnNostrEventModel *self = gn_nostr_event_model_new();
  if (query) {
    gn_nostr_event_model_set_timeline_query(self, query);
  }
  return self;
}

void gn_nostr_event_model_set_timeline_query(GnNostrEventModel *self, GNostrTimelineQuery *query) {
  g_return_if_fail(GN_IS_NOSTR_EVENT_MODEL(self));

  /* Free old query */
  if (self->timeline_query) {
    gnostr_timeline_query_free(self->timeline_query);
    self->timeline_query = NULL;
  }

  if (!query) return;

  /* Store a copy of the query */
  self->timeline_query = gnostr_timeline_query_copy(query);

  /* Sync to legacy fields for compatibility */
  g_free(self->kinds);
  self->kinds = NULL;
  self->n_kinds = 0;

  if (query->n_kinds > 0) {
    self->kinds = g_new(gint, query->n_kinds);
    memcpy(self->kinds, query->kinds, query->n_kinds * sizeof(gint));
    self->n_kinds = query->n_kinds;
  }

  g_strfreev(self->authors);
  self->authors = NULL;
  self->n_authors = 0;

  if (query->n_authors > 0) {
    self->authors = g_new(char*, query->n_authors + 1);
    for (gsize i = 0; i < query->n_authors; i++) {
      self->authors[i] = g_strdup(query->authors[i]);
    }
    self->authors[query->n_authors] = NULL;
    self->n_authors = query->n_authors;
  }

  self->since = query->since;
  self->until = query->until;
  self->limit = query->limit > 0 ? query->limit : MODEL_MAX_ITEMS;
  self->window_size = MIN((guint)MODEL_MAX_ITEMS, self->limit);

  g_debug("[MODEL] Timeline query set: kinds=%zu authors=%zu window=%u",
          self->n_kinds, self->n_authors, self->window_size);
}

GNostrTimelineQuery *gn_nostr_event_model_get_timeline_query(GnNostrEventModel *self) {
  g_return_val_if_fail(GN_IS_NOSTR_EVENT_MODEL(self), NULL);
  return self->timeline_query;
}

/* Legacy API - deprecated, use gn_nostr_event_model_set_timeline_query instead */
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

  /* Treat limit as desired window size cap, but never exceed MODEL_MAX_ITEMS */
  self->limit = params->limit > 0 ? params->limit : MODEL_MAX_ITEMS;
  self->window_size = MIN((guint)MODEL_MAX_ITEMS, (guint)(self->limit > 0 ? self->limit : MODEL_MAX_ITEMS));

  g_debug("[MODEL] Query updated: kinds=%zu authors=%zu window=%u",
          self->n_kinds, self->n_authors, self->window_size);

  /* nostrc-init: Load existing events from nostrdb cache immediately.
   * nostrdb subscriptions (gn_ndb_subscribe) only deliver NEW events ingested
   * after subscription creation. Without this initial load, the timeline sits
   * empty until relay connections deliver events — making the app appear stalled.
   *
   * IMPORTANT: This MUST be synchronous. The cursor results are inserted
   * directly into the model so the GtkListView has items before the first
   * frame. The async GTask pipeline is for live events from relays only.
   * Full data (profiles, NIP-10 thread info, meta counts) is populated by
   * refresh_async 150ms later. */
  if (self->n_kinds > 0 && self->notes->len == 0) {
    GString *filter = g_string_new("{\"kinds\":[");
    for (gsize i = 0; i < self->n_kinds; i++) {
      if (i > 0) g_string_append_c(filter, ',');
      g_string_append_printf(filter, "%d", self->kinds[i]);
    }
    g_string_append(filter, "]}");

    StorageNdbCursor *cursor = storage_ndb_cursor_new(filter->str, 50);
    g_string_free(filter, TRUE);

    if (cursor) {
      const StorageNdbCursorEntry *entries = NULL;
      guint count = 0;
      guint total = 0;

      while (storage_ndb_cursor_next(cursor, &entries, &count, NULL) == 0 && count > 0) {
        for (guint i = 0; i < count; i++) {
          insert_note_silent(self, entries[i].note_key,
                             (gint64)entries[i].created_at, NULL, NULL, 0);
        }
        total += count;
        if (total >= self->window_size)
          break;
      }

      storage_ndb_cursor_free(cursor);

      if (self->notes->len > 0) {
        GArray *evicted_keys = NULL;
        enforce_window_inline(self, &evicted_keys);
        g_list_model_items_changed(G_LIST_MODEL(self), 0, 0, self->notes->len);
        cleanup_evicted_keys(self, evicted_keys);
        g_debug("[MODEL] Initial load: %u events from nostrdb cache", self->notes->len);
      }
    }
  }
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

/* Initial/explicit refresh: query nostrdb and populate the visible window with only profile-ready notes.
 * Live changes are handled incrementally by subscriptions.
 */
void gn_nostr_event_model_refresh(GnNostrEventModel *self) {
  g_return_if_fail(GN_IS_NOSTR_EVENT_MODEL(self));

  /* nostrc-atomic-replace: Record old size, then reset silently.
   * We emit a single items_changed(0, old, new) at the end. */
  guint old_size = self->notes->len;
  reset_internal_state_silent(self);

  /* Build filter JSON for kinds (default to 1/6) */
  GString *filter = g_string_new("[{");

  if (self->n_kinds > 0) {
    g_string_append(filter, "\"kinds\":[");
    for (gsize i = 0; i < self->n_kinds; i++) {
      if (i > 0) g_string_append_c(filter, ',');
      g_string_append_printf(filter, "%d", self->kinds[i]);
    }
    g_string_append(filter, "],");
  } else {
    g_string_append(filter, "\"kinds\":[1,6],");
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

  guint qlimit = self->window_size ? self->window_size : MODEL_MAX_ITEMS;
  g_string_append_printf(filter, "\"limit\":%u}]", qlimit);

  void *txn = NULL;
  if (storage_ndb_begin_query(&txn, NULL) != 0 || !txn) {
    g_warning("[MODEL] Failed to begin query");
    g_string_free(filter, TRUE);
    return;
  }

  char **json_results = NULL;
  int count = 0;
  int query_rc = storage_ndb_query(txn, filter->str, &json_results, &count, NULL);

  guint added = 0;
  if (query_rc == 0 && json_results && count > 0) {
    for (int i = 0; i < count; i++) {
      const char *event_json = json_results[i];
      if (!event_json) continue;

      NostrEvent *evt = nostr_event_new();
      if (!evt) continue;

      if (nostr_event_deserialize(evt, event_json) == 0) {
        int kind = nostr_event_get_kind(evt);
        if (kind != 1 && kind != 6 && kind != 1111) {
          nostr_event_free(evt);
          continue;
        }

        const char *event_id = nostr_event_get_id(evt);
        const char *pubkey_hex = nostr_event_get_pubkey(evt);
        gint64 created_at = nostr_event_get_created_at(evt);

        if (!event_id || !pubkey_hex) {
          nostr_event_free(evt);
          continue;
        }

        if (!note_matches_query(self, kind, pubkey_hex, created_at)) {
          nostr_event_free(evt);
          continue;
        }

        uint8_t id32[32];
        if (!hex_to_bytes32(event_id, id32)) {
          nostr_event_free(evt);
          continue;
        }

        storage_ndb_note *note_ptr = NULL;
        uint64_t note_key = storage_ndb_get_note_key_by_id(txn, id32, &note_ptr);
        if (note_key == 0) {
          nostr_event_free(evt);
          continue;
        }

        /* NIP-40: Filter out expired events */
        if (note_ptr && storage_ndb_note_is_expired(note_ptr)) {
          nostr_event_free(evt);
          continue;
        }

        /* nostrc-gate: Opportunistically cache profile, never gate display */
        uint8_t pk32[32];
        if (!hex_to_bytes32(pubkey_hex, pk32)) {
          nostr_event_free(evt);
          continue;
        }

        if (!profile_cache_ensure_from_db(self, txn, pk32, pubkey_hex)) {
          g_signal_emit(self, signals[SIGNAL_NEED_PROFILE], 0, pubkey_hex);
        }

        char *root_id = NULL;
        char *reply_id = NULL;
        parse_nip10_tags(evt, &root_id, &reply_id);

        if (insert_note_silent(self, note_key, created_at, root_id, reply_id, 0))
          added++;

        g_free(root_id);
        g_free(reply_id);
      }

      nostr_event_free(evt);

      if (added >= qlimit) break;
    }

    storage_ndb_free_results(json_results, count);
  }

  storage_ndb_end_query(txn);
  g_string_free(filter, TRUE);

  /* Evict before signal to avoid nested items_changed */
  GArray *evicted_keys_refresh = NULL;
  enforce_window_inline(self, &evicted_keys_refresh);

  /* nostrc-atomic-replace: ONE atomic replace signal instead of clear + add.
   * GTK rebinds existing widget slots instead of mass teardown + recreation. */
  guint new_size = self->notes->len;
  if (old_size > 0 || new_size > 0)
    g_list_model_items_changed(G_LIST_MODEL(self), 0, old_size, new_size);
  cleanup_evicted_keys(self, evicted_keys_refresh);

  g_debug("[MODEL] Refresh complete: %u total items (%u added, %u replaced)",
          self->notes->len, added, old_size);
}

/* --- Async refresh: moves NDB I/O + JSON deserialization off main thread --- */

typedef struct {
  uint64_t note_key;
  gint64   created_at;
  char    *pubkey_hex;   /* owned */
  char    *root_id;      /* owned, may be NULL */
  char    *reply_id;     /* owned, may be NULL */
  gboolean has_profile;
} RefreshEntry;

static void refresh_entry_free(gpointer p) {
  RefreshEntry *e = p;
  if (!e) return;
  g_free(e->pubkey_hex);
  g_free(e->root_id);
  g_free(e->reply_id);
  g_free(e);
}

/* Snapshot of query params for the worker (immutable after creation) */
typedef struct {
  gint  *kinds;
  gsize  n_kinds;
  char **authors;
  gsize  n_authors;
  gint64 since;
  gint64 until;
  guint  qlimit;
} RefreshQuerySnap;

static void refresh_snap_free(RefreshQuerySnap *s) {
  if (!s) return;
  g_free(s->kinds);
  if (s->authors) {
    for (gsize i = 0; i < s->n_authors; i++) g_free(s->authors[i]);
    g_free(s->authors);
  }
  g_free(s);
}

static gboolean snap_matches_query(RefreshQuerySnap *snap, int kind,
                                   const char *pubkey_hex, gint64 created_at) {
  if (snap->n_kinds > 0) {
    gboolean ok = FALSE;
    for (gsize i = 0; i < snap->n_kinds; i++)
      if (snap->kinds[i] == kind) { ok = TRUE; break; }
    if (!ok) return FALSE;
  }
  if (snap->n_authors > 0) {
    gboolean ok = FALSE;
    for (gsize i = 0; i < snap->n_authors; i++)
      if (snap->authors[i] && pubkey_hex && g_strcmp0(snap->authors[i], pubkey_hex) == 0)
        { ok = TRUE; break; }
    if (!ok) return FALSE;
  }
  if (snap->since > 0 && created_at > 0 && created_at < snap->since) return FALSE;
  if (snap->until > 0 && created_at > 0 && created_at > snap->until) return FALSE;
  return TRUE;
}

static RefreshQuerySnap *refresh_snap_new(GnNostrEventModel *self) {
  RefreshQuerySnap *s = g_new0(RefreshQuerySnap, 1);
  if (self->n_kinds > 0) {
    s->kinds = g_memdup2(self->kinds, self->n_kinds * sizeof(gint));
    s->n_kinds = self->n_kinds;
  }
  if (self->n_authors > 0) {
    s->authors = g_new0(char*, self->n_authors);
    for (gsize i = 0; i < self->n_authors; i++)
      s->authors[i] = g_strdup(self->authors[i]);
    s->n_authors = self->n_authors;
  }
  s->since = self->since;
  s->until = self->until;
  s->qlimit = self->window_size ? self->window_size : MODEL_MAX_ITEMS;
  return s;
}

static char *refresh_build_filter(RefreshQuerySnap *snap) {
  GString *f = g_string_new("[{");
  if (snap->n_kinds > 0) {
    g_string_append(f, "\"kinds\":[");
    for (gsize i = 0; i < snap->n_kinds; i++) {
      if (i > 0) g_string_append_c(f, ',');
      g_string_append_printf(f, "%d", snap->kinds[i]);
    }
    g_string_append(f, "],");
  } else {
    g_string_append(f, "\"kinds\":[1,6],");
  }
  if (snap->n_authors > 0) {
    g_string_append(f, "\"authors\":[");
    for (gsize i = 0; i < snap->n_authors; i++) {
      if (i > 0) g_string_append_c(f, ',');
      g_string_append_printf(f, "\"%s\"", snap->authors[i]);
    }
    g_string_append(f, "],");
  }
  if (snap->since > 0) g_string_append_printf(f, "\"since\":%" G_GINT64_FORMAT ",", snap->since);
  if (snap->until > 0) g_string_append_printf(f, "\"until\":%" G_GINT64_FORMAT ",", snap->until);
  g_string_append_printf(f, "\"limit\":%u}]", snap->qlimit);
  return g_string_free(f, FALSE);
}

/* Worker thread: query NDB + deserialize events (no GObject access) */
static void
refresh_thread_func(GTask *task, gpointer source_object G_GNUC_UNUSED,
                    gpointer task_data, GCancellable *cancellable G_GNUC_UNUSED)
{
  RefreshQuerySnap *snap = task_data;
  GPtrArray *entries = g_ptr_array_new_with_free_func(refresh_entry_free);

  char *filter_str = refresh_build_filter(snap);

  void *txn = NULL;
  if (storage_ndb_begin_query(&txn, NULL) != 0 || !txn) {
    g_warning("[MODEL] refresh_thread: begin_query failed");
    g_free(filter_str);
    g_task_return_pointer(task, entries, (GDestroyNotify)g_ptr_array_unref);
    return;
  }

  char **json_results = NULL;
  int count = 0;
  int rc = storage_ndb_query(txn, filter_str, &json_results, &count, NULL);
  g_free(filter_str);

  guint ready = 0;
  if (rc == 0 && json_results && count > 0) {
    for (int i = 0; i < count; i++) {
      const char *ej = json_results[i];
      if (!ej) continue;

      NostrEvent *evt = nostr_event_new();
      if (!evt) continue;
      if (nostr_event_deserialize(evt, ej) != 0) { nostr_event_free(evt); continue; }

      int kind = nostr_event_get_kind(evt);
      if (kind != 1 && kind != 6 && kind != 1111) { nostr_event_free(evt); continue; }

      const char *eid = nostr_event_get_id(evt);
      const char *pk  = nostr_event_get_pubkey(evt);
      gint64 cat = nostr_event_get_created_at(evt);
      if (!eid || !pk) { nostr_event_free(evt); continue; }

      if (!snap_matches_query(snap, kind, pk, cat)) { nostr_event_free(evt); continue; }

      uint8_t id32[32];
      if (!hex_to_bytes32(eid, id32)) { nostr_event_free(evt); continue; }

      storage_ndb_note *note_ptr = NULL;
      uint64_t nk = storage_ndb_get_note_key_by_id(txn, id32, &note_ptr);
      if (nk == 0) { nostr_event_free(evt); continue; }
      if (note_ptr && storage_ndb_note_is_expired(note_ptr)) { nostr_event_free(evt); continue; }

      /* nostrc-gate: Always include entry; profile check is advisory only */
      uint8_t pk32[32];
      gboolean has_prof = FALSE;
      if (hex_to_bytes32(pk, pk32))
        has_prof = db_has_profile_event_for_pubkey(txn, pk32);

      char *root_id = NULL, *reply_id = NULL;
      parse_nip10_tags(evt, &root_id, &reply_id);

      RefreshEntry *e = g_new0(RefreshEntry, 1);
      e->note_key = nk;
      e->created_at = cat;
      e->pubkey_hex = g_strdup(pk);
      e->root_id = root_id;
      e->reply_id = reply_id;
      e->has_profile = has_prof;
      g_ptr_array_add(entries, e);

      ready++;
      if (ready >= snap->qlimit) { nostr_event_free(evt); break; }

      nostr_event_free(evt);
    }
    storage_ndb_free_results(json_results, count);
  }

  storage_ndb_end_query(txn);
  g_task_return_pointer(task, entries, (GDestroyNotify)g_ptr_array_unref);
}

/* Main-thread callback: apply pre-processed results to model */
static void
on_refresh_async_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
  (void)user_data;
  GnNostrEventModel *self = GN_NOSTR_EVENT_MODEL(source);
  GPtrArray *entries = g_task_propagate_pointer(G_TASK(result), NULL);
  if (!entries) return;
  if (!GN_IS_NOSTR_EVENT_MODEL(self)) { g_ptr_array_unref(entries); return; }

  /* nostrc-atomic-replace: Record old size BEFORE clearing internal state.
   * We will emit a single items_changed(0, old_size, new_size) instead of
   * separate clear + add signals. This avoids the pathological GTK disposal
   * cascade where hundreds of complex widget trees are torn down in one
   * stack frame, causing heap corruption in CSS node finalization. */
  guint old_size = self->notes->len;

  /* Reset all internal state WITHOUT emitting any signal */
  reset_internal_state_silent(self);

  guint added = 0;
  for (guint i = 0; i < entries->len; i++) {
    RefreshEntry *e = g_ptr_array_index(entries, i);
    if (!e) continue;

    /* Mute list check (must be on main thread) */
    GNostrMuteList *ml = gnostr_mute_list_get_default();
    if (ml && e->pubkey_hex && gnostr_mute_list_is_pubkey_muted(ml, e->pubkey_hex))
      continue;

    /* Request profile fetch via signal - no main-thread NDB queries during refresh */
    if (!e->has_profile) {
      g_signal_emit(self, signals[SIGNAL_NEED_PROFILE], 0, e->pubkey_hex);
    }

    /* Insert silently (no per-note items_changed) */
    if (insert_note_silent(self, e->note_key, e->created_at, e->root_id, e->reply_id, 0))
      added++;
  }

  /* Evict before signal to avoid nested items_changed */
  GArray *evicted_keys_async = NULL;
  enforce_window_inline(self, &evicted_keys_async);

  /* ONE atomic replace signal: GTK rebinds existing widget slots instead of
   * tearing them all down and recreating them. This is the key fix. */
  guint new_size = self->notes->len;
  if (old_size > 0 || new_size > 0)
    g_list_model_items_changed(G_LIST_MODEL(self), 0, old_size, new_size);
  cleanup_evicted_keys(self, evicted_keys_async);

  g_debug("[MODEL] Async refresh complete: %u total items (%u added, %u replaced)",
          self->notes->len, added, old_size);

  g_ptr_array_unref(entries);
}

void gn_nostr_event_model_refresh_async(GnNostrEventModel *self) {
  g_return_if_fail(GN_IS_NOSTR_EVENT_MODEL(self));

  RefreshQuerySnap *snap = refresh_snap_new(self);
  GTask *task = g_task_new(self, NULL, on_refresh_async_done, NULL);
  g_task_set_task_data(task, snap, (GDestroyNotify)refresh_snap_free);
  g_task_run_in_thread(task, refresh_thread_func);
  g_object_unref(task);
}

/* --- Async pagination: load_older / load_newer off main thread --- */

/* Main-thread callback: apply pre-processed entries WITHOUT clearing model */
static void
on_paginate_async_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
  (void)user_data;
  GnNostrEventModel *self = GN_NOSTR_EVENT_MODEL(source);
  if (!GN_IS_NOSTR_EVENT_MODEL(self)) return;

  self->async_loading = FALSE;

  GPtrArray *entries = g_task_propagate_pointer(G_TASK(result), NULL);
  if (!entries) return;

  /* Recover trim parameters from GTask qdata */
  guint trim_max = GPOINTER_TO_UINT(
      g_object_get_data(G_OBJECT(result), "paginate-trim-max"));
  gboolean trim_newer = GPOINTER_TO_INT(
      g_object_get_data(G_OBJECT(result), "paginate-trim-newer"));

  guint old_len = self->notes->len;
  guint added = 0;

  /* Pagination inserts should avoid replace-all items_changed to prevent mass disposal.
   * Use trim_newer flag: TRUE = load older (append), FALSE = load newer (prepend). */
  if (trim_newer) {
    /* Load older: append in the order we received (newest-first among older). */
    for (guint i = 0; i < entries->len; i++) {
      RefreshEntry *e = g_ptr_array_index(entries, i);
      if (!e) continue;

      if (has_note_key(self, e->note_key)) continue;

      GNostrMuteList *ml = gnostr_mute_list_get_default();
      if (ml && e->pubkey_hex && gnostr_mute_list_is_pubkey_muted(ml, e->pubkey_hex))
        continue;

      if (!e->has_profile) {
        g_signal_emit(self, signals[SIGNAL_NEED_PROFILE], 0, e->pubkey_hex);
      }

      if (e->root_id || e->reply_id) {
        if (!g_hash_table_contains(self->thread_info, &e->note_key)) {
          ThreadInfo *tinfo = g_new0(ThreadInfo, 1);
          tinfo->root_id = g_strdup(e->root_id);
          tinfo->parent_id = g_strdup(e->reply_id);
          tinfo->depth = 0;
          uint64_t *key_copy = g_new(uint64_t, 1);
          *key_copy = e->note_key;
          g_hash_table_insert(self->thread_info, key_copy, tinfo);
        }
      }

      NoteEntry entry = { .note_key = e->note_key, .created_at = e->created_at };
      g_array_insert_val(self->notes, self->notes->len, entry);
      uint64_t *set_key = g_new(uint64_t, 1);
      *set_key = e->note_key;
      g_hash_table_add(self->note_key_set, set_key);
      added++;
    }
  } else {
    /* Load newer: prepend in reverse order to preserve newest-first. */
    for (gint i = (gint)entries->len - 1; i >= 0; i--) {
      RefreshEntry *e = g_ptr_array_index(entries, (guint)i);
      if (!e) continue;

      if (has_note_key(self, e->note_key)) continue;

      GNostrMuteList *ml = gnostr_mute_list_get_default();
      if (ml && e->pubkey_hex && gnostr_mute_list_is_pubkey_muted(ml, e->pubkey_hex))
        continue;

      if (!e->has_profile) {
        g_signal_emit(self, signals[SIGNAL_NEED_PROFILE], 0, e->pubkey_hex);
      }

      if (e->root_id || e->reply_id) {
        if (!g_hash_table_contains(self->thread_info, &e->note_key)) {
          ThreadInfo *tinfo = g_new0(ThreadInfo, 1);
          tinfo->root_id = g_strdup(e->root_id);
          tinfo->parent_id = g_strdup(e->reply_id);
          tinfo->depth = 0;
          uint64_t *key_copy = g_new(uint64_t, 1);
          *key_copy = e->note_key;
          g_hash_table_insert(self->thread_info, key_copy, tinfo);
        }
      }

      NoteEntry entry = { .note_key = e->note_key, .created_at = e->created_at };
      g_array_insert_val(self->notes, 0, entry);
      uint64_t *set_key = g_new(uint64_t, 1);
      *set_key = e->note_key;
      g_hash_table_add(self->note_key_set, set_key);
      added++;
    }
  }

  /* Emit ONE localized items_changed for all insertions */
  if (added > 0) {
    guint start = trim_newer ? old_len : 0;
    g_list_model_items_changed(G_LIST_MODEL(self), start, 0, added);
  }

  /* Trim model to bounded size if requested */
  if (trim_max > 0 && self->notes->len > trim_max) {
    if (trim_newer)
      gn_nostr_event_model_trim_newer(self, trim_max);
    else
      gn_nostr_event_model_trim_older(self, trim_max);
  }

  g_debug("[MODEL] Async paginate: %u added, %u total", added, self->notes->len);
  g_ptr_array_unref(entries);
}

void
gn_nostr_event_model_load_older_async(GnNostrEventModel *self, guint count,
                                       guint max_items)
{
  g_return_if_fail(GN_IS_NOSTR_EVENT_MODEL(self));
  if (count == 0 || self->async_loading) return;

  gint64 oldest_ts = gn_nostr_event_model_get_oldest_timestamp(self);
  if (oldest_ts == 0) {
    /* No events yet — fall back to async refresh */
    gn_nostr_event_model_refresh_async(self);
    return;
  }

  self->async_loading = TRUE;

  RefreshQuerySnap *snap = refresh_snap_new(self);
  snap->until = oldest_ts - 1;
  snap->since = 0;
  snap->qlimit = count;

  GTask *task = g_task_new(self, NULL, on_paginate_async_done, NULL);
  g_task_set_task_data(task, snap, (GDestroyNotify)refresh_snap_free);

  /* Store trim info as GTask qdata (read by callback) */
  g_object_set_data(G_OBJECT(task), "paginate-trim-max",
                    GUINT_TO_POINTER(max_items));
  g_object_set_data(G_OBJECT(task), "paginate-trim-newer",
                    GINT_TO_POINTER(TRUE)); /* loaded older → trim newer */

  g_task_run_in_thread(task, refresh_thread_func);
  g_object_unref(task);
}

void
gn_nostr_event_model_load_newer_async(GnNostrEventModel *self, guint count,
                                       guint max_items)
{
  g_return_if_fail(GN_IS_NOSTR_EVENT_MODEL(self));
  if (count == 0 || self->async_loading) return;

  gint64 newest_ts = gn_nostr_event_model_get_newest_timestamp(self);
  if (newest_ts == 0) {
    gn_nostr_event_model_refresh_async(self);
    return;
  }

  self->async_loading = TRUE;

  RefreshQuerySnap *snap = refresh_snap_new(self);
  snap->since = newest_ts + 1;
  snap->until = 0;
  /* Query more to ensure contiguous events (NDB returns newest-first) */
  guint query_limit = count * 4;
  if (query_limit < 100) query_limit = 100;
  snap->qlimit = query_limit;

  GTask *task = g_task_new(self, NULL, on_paginate_async_done, NULL);
  g_task_set_task_data(task, snap, (GDestroyNotify)refresh_snap_free);

  g_object_set_data(G_OBJECT(task), "paginate-trim-max",
                    GUINT_TO_POINTER(max_items));
  g_object_set_data(G_OBJECT(task), "paginate-trim-newer",
                    GINT_TO_POINTER(FALSE)); /* loaded newer → trim older */

  g_task_run_in_thread(task, refresh_thread_func);
  g_object_unref(task);
}

gboolean
gn_nostr_event_model_is_async_loading(GnNostrEventModel *self)
{
  g_return_val_if_fail(GN_IS_NOSTR_EVENT_MODEL(self), FALSE);
  return self->async_loading;
}

void gn_nostr_event_model_update_profile(GObject *model, const char *pubkey_hex, const char *content_json) {
  g_return_if_fail(GN_IS_NOSTR_EVENT_MODEL(model));
  g_return_if_fail(pubkey_hex != NULL);
  g_return_if_fail(content_json != NULL);

  GnNostrEventModel *self = GN_NOSTR_EVENT_MODEL(model);

  profile_cache_update_from_content(self, pubkey_hex, content_json, strlen(content_json));
  notify_cached_items_for_pubkey(self, pubkey_hex);
}

void gn_nostr_event_model_check_pending_for_profile(GnNostrEventModel *self, const char *pubkey) {
  /* Subscription-driven gating handles this automatically now. */
  (void)self;
  (void)pubkey;
}

void gn_nostr_event_model_clear(GnNostrEventModel *self) {
  g_return_if_fail(GN_IS_NOSTR_EVENT_MODEL(self));

  /* Stop drain timer to avoid concurrent mutations during clear */
  remove_drain_timer(self);

  /* Clear insertion buffer pipeline state */
  if (self->insertion_buffer)
    g_array_set_size(self->insertion_buffer, 0);
  if (self->insertion_key_set)
    g_hash_table_remove_all(self->insertion_key_set);
  self->backpressure_active = FALSE;
  self->unseen_count = 0;
  self->evict_defer_counter = 0;

  guint old_size = self->notes->len;
  if (old_size == 0) {
    /* Still clear caches to be safe */
    g_hash_table_remove_all(self->item_cache);
    g_queue_clear(self->cache_lru);
    g_hash_table_remove_all(self->thread_info);
    if (self->reaction_cache) g_hash_table_remove_all(self->reaction_cache);
    if (self->zap_stats_cache) g_hash_table_remove_all(self->zap_stats_cache);
    if (self->skip_animation_keys) g_hash_table_remove_all(self->skip_animation_keys);
    return;
  }

  /* nostrc-atomic-replace: Emit items_changed FIRST while data is still valid,
   * then clean up caches AFTER GTK has finished unbinding widgets.
   *
   * Why this order matters:
   * - GTK processes items_changed synchronously: unbind callbacks fire during
   *   this call, and they may call get_item() or access cached GnNostrEventItem
   *   objects. The notes array and caches MUST be valid during the signal.
   * - After the signal returns, GTK has unbound all widgets from the old items.
   *   It is now safe to free the cached data.
   *
   * Why a single signal (not batched):
   * - One items_changed(0, N, 0) → one pass through GtkListItemManager →
   *   widgets are unbound in order, no interleaved cache mutation.
   * - Batched removal interleaved cache cleanup with signals, causing
   *   use-after-free when GTK accessed items from a previous batch.
   */
  g_array_set_size(self->notes, 0);
  g_hash_table_remove_all(self->note_key_set);
  g_list_model_items_changed(G_LIST_MODEL(self), 0, old_size, 0);

  /* NOW safe to clean caches — GTK has finished with all widgets */
  g_hash_table_remove_all(self->item_cache);
  g_queue_clear(self->cache_lru);
  g_hash_table_remove_all(self->thread_info);
  if (self->reaction_cache) g_hash_table_remove_all(self->reaction_cache);
  if (self->zap_stats_cache) g_hash_table_remove_all(self->zap_stats_cache);
  if (self->skip_animation_keys) g_hash_table_remove_all(self->skip_animation_keys);

  g_debug("[MODEL] Cleared %u items (single signal, deferred cache cleanup)", old_size);
}

gboolean gn_nostr_event_model_get_is_thread_view(GnNostrEventModel *self) {
  g_return_val_if_fail(GN_IS_NOSTR_EVENT_MODEL(self), FALSE);
  return self->is_thread_view;
}

const char *gn_nostr_event_model_get_root_event_id(GnNostrEventModel *self) {
  g_return_val_if_fail(GN_IS_NOSTR_EVENT_MODEL(self), NULL);
  return self->root_event_id;
}

/* Compatibility: attempt to add an event by JSON, but still enforce persistence-first gating.
 * If the event isn't yet in nostrdb, it will not be added here; subscriptions will pick it up after ingest.
 */
void gn_nostr_event_model_add_event_json(GnNostrEventModel *self, const char *event_json) {
  g_return_if_fail(GN_IS_NOSTR_EVENT_MODEL(self));
  g_return_if_fail(event_json != NULL);

  NostrEvent *evt = nostr_event_new();
  if (!evt) return;

  if (nostr_event_deserialize(evt, event_json) != 0) {
    nostr_event_free(evt);
    return;
  }

  int kind = nostr_event_get_kind(evt);
  if (kind != 1 && kind != 6 && kind != 1111) {
    nostr_event_free(evt);
    return;
  }

  const char *event_id = nostr_event_get_id(evt);
  const char *pubkey_hex = nostr_event_get_pubkey(evt);
  gint64 created_at = nostr_event_get_created_at(evt);

  if (!event_id || !pubkey_hex) {
    nostr_event_free(evt);
    return;
  }

  uint8_t id32[32];
  if (!hex_to_bytes32(event_id, id32)) {
    nostr_event_free(evt);
    return;
  }

  void *txn = NULL;
  if (storage_ndb_begin_query(&txn, NULL) != 0 || !txn) {
    nostr_event_free(evt);
    return;
  }

  uint64_t note_key = storage_ndb_get_note_key_by_id(txn, id32, NULL);
  if (note_key == 0) {
    storage_ndb_end_query(txn);
    nostr_event_free(evt);
    return;
  }

  /* nostrc-gate: Opportunistically cache profile, never gate display */
  uint8_t pk32[32];
  if (!hex_to_bytes32(pubkey_hex, pk32)) {
    storage_ndb_end_query(txn);
    nostr_event_free(evt);
    return;
  }

  if (!profile_cache_ensure_from_db(self, txn, pk32, pubkey_hex)) {
    /* Profile not in DB yet -- request background fetch */
    g_signal_emit(self, signals[SIGNAL_NEED_PROFILE], 0, pubkey_hex);
  }

  /* Always show note regardless of profile availability */
  char *root_id = NULL;
  char *reply_id = NULL;
  parse_nip10_tags(evt, &root_id, &reply_id);

  add_note_internal(self, note_key, created_at, root_id, reply_id, 0);
  GArray *evicted_keys_add = NULL;
  guint evicted = enforce_window_inline(self, &evicted_keys_add);
  if (evicted > 0) {
    guint cap = self->window_size ? self->window_size : MODEL_MAX_ITEMS;
    g_list_model_items_changed(G_LIST_MODEL(self), cap, evicted, 0);
    cleanup_evicted_keys(self, evicted_keys_add);
  } else {
    cleanup_evicted_keys(self, evicted_keys_add);
  }

  g_free(root_id);
  g_free(reply_id);

  storage_ndb_end_query(txn);
  nostr_event_free(evt);
}

/* Compatibility: deprecated. Subscriptions are the authoritative mechanism for updates.
 * This function intentionally does nothing to avoid bypassing persistence-first ordering.
 */
void gn_nostr_event_model_add_live_event(GnNostrEventModel *self, void *nostr_event) {
  (void)self;
  (void)nostr_event;
}

/* ============== Sliding Window Pagination ============== */

gint64 gn_nostr_event_model_get_oldest_timestamp(GnNostrEventModel *self) {
  g_return_val_if_fail(GN_IS_NOSTR_EVENT_MODEL(self), 0);
  if (self->notes->len == 0) return 0;
  NoteEntry *oldest = &g_array_index(self->notes, NoteEntry, self->notes->len - 1);
  return oldest->created_at;
}

void gn_nostr_event_model_trim_newer(GnNostrEventModel *self, guint keep_count) {
  g_return_if_fail(GN_IS_NOSTR_EVENT_MODEL(self));
  if (self->notes->len <= keep_count) return;

  guint to_remove = self->notes->len - keep_count;

  /* Collect keys to remove BEFORE modifying data structures */
  uint64_t *keys_to_remove = g_new(uint64_t, to_remove);
  for (guint i = 0; i < to_remove; i++) {
    NoteEntry *entry = &g_array_index(self->notes, NoteEntry, i);
    keys_to_remove[i] = entry->note_key;
  }

  /* Remove from array and emit items_changed FIRST so GTK can tear down widgets
   * while cached items are still valid */
  g_array_remove_range(self->notes, 0, to_remove);
  g_list_model_items_changed(G_LIST_MODEL(self), 0, to_remove, 0);

  /* NOW cleanup caches after GTK has finished with widgets */
  for (guint i = 0; i < to_remove; i++) {
    uint64_t k = keys_to_remove[i];
    g_hash_table_remove(self->note_key_set, &k);
    cache_lru_remove_key(self, k);
    g_hash_table_remove(self->thread_info, &k);
    g_hash_table_remove(self->item_cache, &k);
    g_hash_table_remove(self->skip_animation_keys, &k);
  }

  g_free(keys_to_remove);
  g_debug("[MODEL] Trimmed %u newer items, %u remaining", to_remove, self->notes->len);
}

guint gn_nostr_event_model_load_older(GnNostrEventModel *self, guint count) {
  g_return_val_if_fail(GN_IS_NOSTR_EVENT_MODEL(self), 0);
  if (count == 0) return 0;

  gint64 oldest_ts = gn_nostr_event_model_get_oldest_timestamp(self);
  if (oldest_ts == 0) {
    /* No events yet, do a normal refresh */
    gn_nostr_event_model_refresh(self);
    return self->notes->len;
  }

  /* Build filter JSON for kinds with until = oldest_ts - 1 */
  GString *filter = g_string_new("[{");

  if (self->n_kinds > 0) {
    g_string_append(filter, "\"kinds\":[");
    for (gsize i = 0; i < self->n_kinds; i++) {
      if (i > 0) g_string_append_c(filter, ',');
      g_string_append_printf(filter, "%d", self->kinds[i]);
    }
    g_string_append(filter, "],");
  } else {
    g_string_append(filter, "\"kinds\":[1,6],");
  }

  if (self->n_authors > 0) {
    g_string_append(filter, "\"authors\":[");
    for (gsize i = 0; i < self->n_authors; i++) {
      if (i > 0) g_string_append_c(filter, ',');
      g_string_append_printf(filter, "\"%s\"", self->authors[i]);
    }
    g_string_append(filter, "],");
  }

  /* Query for events older than our oldest */
  g_string_append_printf(filter, "\"until\":%" G_GINT64_FORMAT ",", oldest_ts - 1);
  g_string_append_printf(filter, "\"limit\":%u}]", count);

  void *txn = NULL;
  if (storage_ndb_begin_query(&txn, NULL) != 0 || !txn) {
    g_warning("[MODEL] load_older: Failed to begin query");
    g_string_free(filter, TRUE);
    return 0;
  }

  char **json_results = NULL;
  int result_count = 0;
  int query_rc = storage_ndb_query(txn, filter->str, &json_results, &result_count, NULL);

  guint old_len = self->notes->len;
  guint added = 0;
  if (query_rc == 0 && json_results && result_count > 0) {
    for (int i = 0; i < result_count; i++) {
      const char *event_json = json_results[i];
      if (!event_json) continue;

      NostrEvent *evt = nostr_event_new();
      if (!evt) continue;

      if (nostr_event_deserialize(evt, event_json) == 0) {
        int kind = nostr_event_get_kind(evt);
        if (kind != 1 && kind != 6 && kind != 1111) {
          nostr_event_free(evt);
          continue;
        }

        const char *event_id = nostr_event_get_id(evt);
        const char *pubkey_hex = nostr_event_get_pubkey(evt);
        gint64 created_at = nostr_event_get_created_at(evt);

        if (!event_id || !pubkey_hex) {
          nostr_event_free(evt);
          continue;
        }

        if (!note_matches_query(self, kind, pubkey_hex, created_at)) {
          nostr_event_free(evt);
          continue;
        }

        uint8_t id32[32];
        if (!hex_to_bytes32(event_id, id32)) {
          nostr_event_free(evt);
          continue;
        }

        storage_ndb_note *note_ptr = NULL;
        uint64_t note_key = storage_ndb_get_note_key_by_id(txn, id32, &note_ptr);
        if (note_key == 0) {
          nostr_event_free(evt);
          continue;
        }

        if (note_ptr && storage_ndb_note_is_expired(note_ptr)) {
          nostr_event_free(evt);
          continue;
        }

        if (has_note_key(self, note_key)) {
          nostr_event_free(evt);
          continue;
        }

        uint8_t pk32[32];
        if (!hex_to_bytes32(pubkey_hex, pk32)) {
          nostr_event_free(evt);
          continue;
        }

        if (!profile_cache_ensure_from_db(self, txn, pk32, pubkey_hex))
          g_signal_emit(self, signals[SIGNAL_NEED_PROFILE], 0, pubkey_hex);

        char *root_id = NULL;
        char *reply_id = NULL;
        parse_nip10_tags(evt, &root_id, &reply_id);

        /* Direct sorted insert — bypass deferral (user-initiated pagination) */
        if (root_id || reply_id) {
          if (!g_hash_table_contains(self->thread_info, &note_key)) {
            ThreadInfo *tinfo = g_new0(ThreadInfo, 1);
            tinfo->root_id = g_strdup(root_id);
            tinfo->parent_id = g_strdup(reply_id);
            tinfo->depth = 0;
            uint64_t *key_copy = g_new(uint64_t, 1);
            *key_copy = note_key;
            g_hash_table_insert(self->thread_info, key_copy, tinfo);
          }
        }
        guint pos = find_sorted_position(self, created_at);
        NoteEntry entry = { .note_key = note_key, .created_at = created_at };
        g_array_insert_val(self->notes, pos, entry);
        uint64_t *set_key = g_new(uint64_t, 1);
        *set_key = note_key;
        g_hash_table_add(self->note_key_set, set_key);
        added++;

        g_free(root_id);
        g_free(reply_id);
      }

      nostr_event_free(evt);

      if (added >= count) break;
    }

    storage_ndb_free_results(json_results, result_count);
  }

  storage_ndb_end_query(txn);
  g_string_free(filter, TRUE);

  /* ONE batched signal for all insertions */
  if (added > 0) {
    guint new_len = self->notes->len;
    g_list_model_items_changed(G_LIST_MODEL(self), 0, old_len, new_len);
  }

  g_debug("[MODEL] load_older: added %u events, total now %u", added, self->notes->len);

  return added;
}

gint64 gn_nostr_event_model_get_newest_timestamp(GnNostrEventModel *self) {
  g_return_val_if_fail(GN_IS_NOSTR_EVENT_MODEL(self), 0);
  if (self->notes->len == 0) return 0;
  NoteEntry *newest = &g_array_index(self->notes, NoteEntry, 0);
  return newest->created_at;
}

void gn_nostr_event_model_trim_older(GnNostrEventModel *self, guint keep_count) {
  g_return_if_fail(GN_IS_NOSTR_EVENT_MODEL(self));
  if (self->notes->len <= keep_count) return;

  guint to_remove = self->notes->len - keep_count;
  guint start_idx = keep_count; /* Remove from end */

  /* Collect keys to remove BEFORE modifying data structures */
  uint64_t *keys_to_remove = g_new(uint64_t, to_remove);
  for (guint i = 0; i < to_remove; i++) {
    NoteEntry *entry = &g_array_index(self->notes, NoteEntry, start_idx + i);
    keys_to_remove[i] = entry->note_key;
  }

  /* Remove from array and emit items_changed FIRST so GTK can tear down widgets
   * while cached items are still valid */
  g_array_remove_range(self->notes, start_idx, to_remove);
  g_list_model_items_changed(G_LIST_MODEL(self), start_idx, to_remove, 0);

  /* NOW cleanup caches after GTK has finished with widgets */
  for (guint i = 0; i < to_remove; i++) {
    uint64_t k = keys_to_remove[i];
    g_hash_table_remove(self->note_key_set, &k);
    cache_lru_remove_key(self, k);
    g_hash_table_remove(self->thread_info, &k);
    g_hash_table_remove(self->item_cache, &k);
    g_hash_table_remove(self->skip_animation_keys, &k);
  }

  g_free(keys_to_remove);
  g_debug("[MODEL] Trimmed %u older items, %u remaining", to_remove, self->notes->len);
}

guint gn_nostr_event_model_load_newer(GnNostrEventModel *self, guint count) {
  g_return_val_if_fail(GN_IS_NOSTR_EVENT_MODEL(self), 0);
  if (count == 0) return 0;

  gint64 newest_ts = gn_nostr_event_model_get_newest_timestamp(self);
  if (newest_ts == 0) {
    /* No events yet, do a normal refresh */
    gn_nostr_event_model_refresh(self);
    return self->notes->len;
  }

  /* Build filter JSON for kinds with since = newest_ts + 1
   * 
   * IMPORTANT: nostrdb returns results ordered by timestamp DESCENDING (newest first).
   * With since+limit, we'd get the N most recent events, not the N events immediately
   * after our current newest. To get contiguous events, we query a larger batch and
   * process from the end (oldest in results = closest to our current window).
   */
  guint query_limit = count * 4; /* Query more to ensure we get contiguous events */
  if (query_limit < 100) query_limit = 100;

  GString *filter = g_string_new("[{");

  if (self->n_kinds > 0) {
    g_string_append(filter, "\"kinds\":[");
    for (gsize i = 0; i < self->n_kinds; i++) {
      if (i > 0) g_string_append_c(filter, ',');
      g_string_append_printf(filter, "%d", self->kinds[i]);
    }
    g_string_append(filter, "],");
  } else {
    g_string_append(filter, "\"kinds\":[1,6],");
  }

  if (self->n_authors > 0) {
    g_string_append(filter, "\"authors\":[");
    for (gsize i = 0; i < self->n_authors; i++) {
      if (i > 0) g_string_append_c(filter, ',');
      g_string_append_printf(filter, "\"%s\"", self->authors[i]);
    }
    g_string_append(filter, "],");
  }

  /* Query for events newer than our newest */
  g_string_append_printf(filter, "\"since\":%" G_GINT64_FORMAT ",", newest_ts + 1);
  g_string_append_printf(filter, "\"limit\":%u}]", query_limit);


  void *txn = NULL;
  if (storage_ndb_begin_query(&txn, NULL) != 0 || !txn) {
    g_warning("[MODEL] load_newer: Failed to begin query");
    g_string_free(filter, TRUE);
    return 0;
  }

  char **json_results = NULL;
  int result_count = 0;
  int query_rc = storage_ndb_query(txn, filter->str, &json_results, &result_count, NULL);

  guint old_len = self->notes->len;
  guint added = 0;
  if (query_rc == 0 && json_results && result_count > 0) {
    /* Iterate from end (oldest in results) to get events closest to our current window.
     * Results are ordered newest-first, so index result_count-1 is the oldest. */
    for (int i = result_count - 1; i >= 0; i--) {
      const char *event_json = json_results[i];
      if (!event_json) continue;

      NostrEvent *evt = nostr_event_new();
      if (!evt) continue;

      if (nostr_event_deserialize(evt, event_json) == 0) {
        int kind = nostr_event_get_kind(evt);
        if (kind != 1 && kind != 6 && kind != 1111) {
          nostr_event_free(evt);
          continue;
        }

        const char *event_id = nostr_event_get_id(evt);
        const char *pubkey_hex = nostr_event_get_pubkey(evt);
        gint64 created_at = nostr_event_get_created_at(evt);

        if (!event_id || !pubkey_hex) {
          nostr_event_free(evt);
          continue;
        }

        if (!note_matches_query(self, kind, pubkey_hex, created_at)) {
          nostr_event_free(evt);
          continue;
        }

        uint8_t id32[32];
        if (!hex_to_bytes32(event_id, id32)) {
          nostr_event_free(evt);
          continue;
        }

        storage_ndb_note *note_ptr = NULL;
        uint64_t note_key = storage_ndb_get_note_key_by_id(txn, id32, &note_ptr);
        if (note_key == 0) {
          nostr_event_free(evt);
          continue;
        }

        if (note_ptr && storage_ndb_note_is_expired(note_ptr)) {
          nostr_event_free(evt);
          continue;
        }

        if (has_note_key(self, note_key)) {
          nostr_event_free(evt);
          continue;
        }

        uint8_t pk32[32];
        if (!hex_to_bytes32(pubkey_hex, pk32)) {
          nostr_event_free(evt);
          continue;
        }

        if (!profile_cache_ensure_from_db(self, txn, pk32, pubkey_hex))
          g_signal_emit(self, signals[SIGNAL_NEED_PROFILE], 0, pubkey_hex);

        char *root_id = NULL;
        char *reply_id = NULL;
        parse_nip10_tags(evt, &root_id, &reply_id);

        /* Direct sorted insert — bypass deferral (user-initiated pagination) */
        if (root_id || reply_id) {
          if (!g_hash_table_contains(self->thread_info, &note_key)) {
            ThreadInfo *tinfo = g_new0(ThreadInfo, 1);
            tinfo->root_id = g_strdup(root_id);
            tinfo->parent_id = g_strdup(reply_id);
            tinfo->depth = 0;
            uint64_t *key_copy = g_new(uint64_t, 1);
            *key_copy = note_key;
            g_hash_table_insert(self->thread_info, key_copy, tinfo);
          }
        }
        guint pos = find_sorted_position(self, created_at);
        NoteEntry entry = { .note_key = note_key, .created_at = created_at };
        g_array_insert_val(self->notes, pos, entry);
        uint64_t *set_key = g_new(uint64_t, 1);
        *set_key = note_key;
        g_hash_table_add(self->note_key_set, set_key);
        added++;

        g_free(root_id);
        g_free(reply_id);
      }

      nostr_event_free(evt);

      if (added >= count) break;
    }

    storage_ndb_free_results(json_results, result_count);
  }

  storage_ndb_end_query(txn);
  g_string_free(filter, TRUE);

  /* ONE batched signal for all insertions */
  if (added > 0) {
    guint new_len = self->notes->len;
    g_list_model_items_changed(G_LIST_MODEL(self), 0, old_len, new_len);
  }

  return added;
}

/* nostrc-7o7: Update visible range for animation skip tracking */
void gn_nostr_event_model_set_visible_range(GnNostrEventModel *self, guint start, guint end) {
  g_return_if_fail(GN_IS_NOSTR_EVENT_MODEL(self));
  self->visible_start = start;
  self->visible_end = end;
}

/* Set whether user is at top of scroll.
 * When user scrolls to top, reset unseen_count (items are already inserted
 * by the tick callback; the count just tracks how many arrived while scrolled down). */
void gn_nostr_event_model_set_user_at_top(GnNostrEventModel *self, gboolean at_top) {
  g_return_if_fail(GN_IS_NOSTR_EVENT_MODEL(self));

  gboolean was_at_top = self->user_at_top;
  self->user_at_top = at_top;

  if (at_top && !was_at_top && self->unseen_count > 0) {
    g_debug("[CALM] User scrolled to top, clearing %u unseen count", self->unseen_count);
    self->unseen_count = 0;
    g_signal_emit(self, signals[SIGNAL_NEW_ITEMS_PENDING], 0, (guint)0);
  }
}

/* Get the count of items added while user was scrolled down */
guint gn_nostr_event_model_get_pending_count(GnNostrEventModel *self) {
  g_return_val_if_fail(GN_IS_NOSTR_EVENT_MODEL(self), 0);
  return self->unseen_count;
}

/* Reset unseen count (e.g., when user clicks "N new notes" indicator).
 * Items are already in the model — this just clears the notification. */
void gn_nostr_event_model_flush_pending(GnNostrEventModel *self) {
  g_return_if_fail(GN_IS_NOSTR_EVENT_MODEL(self));

  if (self->unseen_count > 0) {
    g_debug("[CALM] Flushing unseen count: %u", self->unseen_count);
    self->unseen_count = 0;
    g_signal_emit(self, signals[SIGNAL_NEW_ITEMS_PENDING], 0, (guint)0);
  }
}
