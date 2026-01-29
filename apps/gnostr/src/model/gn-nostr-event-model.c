#define G_LOG_DOMAIN "gnostr-event-model"

#include "gn-nostr-event-model.h"
#include "gn-timeline-query.h"
#include "gn-ndb-sub-dispatcher.h"
#include "../storage_ndb.h"
#include "../util/mute_list.h"
#include <nostr.h>
#include <string.h>

/* Window sizing and cache sizes */
#define MODEL_MAX_ITEMS 100
#define ITEM_CACHE_SIZE 100
#define PROFILE_CACHE_MAX 500
#define AUTHORS_READY_MAX 1000
#define DEFERRED_NOTES_MAX 200       /* Max deferred notes before force flush */
#define PENDING_BY_AUTHOR_MAX 500    /* Max pending authors before cleanup */

/* nostrc-yi2: Calm timeline - debounce and rate limiting */
#define DEBOUNCE_INTERVAL_MS 500     /* Batch rapid updates */
#define MAX_UPDATES_PER_SEC 3        /* Rate limit UI refreshes */
#define MIN_UPDATE_INTERVAL_MS (1000 / MAX_UPDATES_PER_SEC)

/* Subscription filters - storage_ndb_subscribe expects a single filter object, not an array */
#define FILTER_TIMELINE "{\"kinds\":[1,6]}"
#define FILTER_PROFILES "{\"kinds\":[0]}"
#define FILTER_DELETES  "{\"kinds\":[5]}"

/* Note entry for sorted storage */
typedef struct {
  uint64_t note_key;
  gint64 created_at;
} NoteEntry;

typedef struct {
  uint64_t note_key;
  gint64 created_at;
} PendingEntry;

struct _GnNostrEventModel {
  GObject parent_instance;

  /* Query parameters (new API) */
  GnTimelineQuery *timeline_query;

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

  /* Lifetime nostrdb subscriptions (via dispatcher) */
  uint64_t sub_timeline; /* kinds 1/6 */
  uint64_t sub_profiles; /* kind 0 */
  uint64_t sub_deletes;  /* kind 5 */

  /* Windowing */
  guint window_size;

  /* Small LRU cache for visible items */
  GHashTable *item_cache;  /* key: uint64_t*, value: GnNostrEventItem* */
  GQueue *cache_lru;       /* uint64_t* keys in LRU order */

  /* Profile cache - pubkey -> GnNostrProfile (with LRU eviction) */
  GHashTable *profile_cache;      /* key: pubkey (string), value: GnNostrProfile* */
  GQueue *profile_cache_lru;      /* char* pubkey in LRU order (head=oldest) */

  /* Author readiness (kind 0 exists in DB / loaded) - with LRU eviction */
  GHashTable *authors_ready;      /* key: pubkey hex (string), value: GINT_TO_POINTER(1) */
  GQueue *authors_ready_lru;      /* char* pubkey in LRU order (head=oldest) */
  GHashTable *pending_by_author;  /* key: pubkey hex (string), value: GArray* (PendingEntry) */

  /* Thread info cache - note_key -> ThreadInfo */
  GHashTable *thread_info;

  /* nostrc-7o7: Animation control - track which items should skip animation */
  guint visible_start;  /* First visible position in the list */
  guint visible_end;    /* Last visible position in the list */
  GHashTable *skip_animation_keys;  /* key: uint64_t*, value: GINT_TO_POINTER(1) */

  /* nostrc-yi2: Calm timeline - debounce and batching */
  gboolean user_at_top;           /* TRUE if user is at scroll top (auto-scroll allowed) */
  GArray *deferred_notes;         /* NoteEntry items waiting to be inserted */
  guint debounce_source_id;       /* Pending debounce timeout */
  gint64 last_update_time_ms;     /* Timestamp of last items-changed emission */
  guint pending_new_count;        /* Count of new items waiting (for indicator) */

  /* Deferred enforce_window to avoid nested items_changed signals */
  guint enforce_window_idle_id;
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
static GnNostrProfile *profile_cache_get(GnNostrEventModel *self, const char *pubkey_hex);
static GnNostrProfile *profile_cache_ensure_from_db(GnNostrEventModel *self, void *txn,
                                                    const unsigned char pk32[32],
                                                    const char *pubkey_hex);
static void profile_cache_update_from_content(GnNostrEventModel *self, const char *pubkey_hex,
                                              const char *content, gsize content_len);
static gboolean note_matches_query(GnNostrEventModel *self, int kind, const char *pubkey_hex, gint64 created_at);
static void enforce_window(GnNostrEventModel *self);
static gboolean remove_note_by_key(GnNostrEventModel *self, uint64_t note_key);
static void pending_remove_note_key(GnNostrEventModel *self, uint64_t note_key);
static gboolean add_pending(GnNostrEventModel *self, const char *pubkey_hex, uint64_t note_key, gint64 created_at);
static void flush_pending_notes(GnNostrEventModel *self, const char *pubkey_hex);

/* nostrc-yi2: Calm timeline helpers */
static void defer_note_insertion(GnNostrEventModel *self, uint64_t note_key, gint64 created_at);
static gboolean flush_deferred_notes_cb(gpointer user_data);
static void schedule_deferred_flush(GnNostrEventModel *self);
static gint64 get_current_time_ms(void);

/* Subscription callbacks */
static void on_sub_timeline_batch(uint64_t subid, const uint64_t *note_keys, guint n_keys, gpointer user_data);
static void on_sub_profiles_batch(uint64_t subid, const uint64_t *note_keys, guint n_keys, gpointer user_data);
static void on_sub_deletes_batch(uint64_t subid, const uint64_t *note_keys, guint n_keys, gpointer user_data);

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
  int rc = storage_ndb_get_profile_by_pubkey(txn, pk32, &evt_json, &evt_len);
  if (rc != 0 || !evt_json || evt_len <= 0) {
    if (evt_json) free(evt_json);
    return FALSE;
  }
  free(evt_json);
  return TRUE;
}

static GnNostrProfile *profile_cache_get(GnNostrEventModel *self, const char *pubkey_hex) {
  if (!self || !self->profile_cache || !pubkey_hex) return NULL;
  return g_hash_table_lookup(self->profile_cache, pubkey_hex);
}

/* Load kind-0 profile from DB (storage_ndb_get_profile_by_pubkey returns *event* JSON), then cache it.
 * Returns a cached GnNostrProfile* on success, NULL if not found.
 */
static GnNostrProfile *profile_cache_ensure_from_db(GnNostrEventModel *self, void *txn,
                                                    const unsigned char pk32[32],
                                                    const char *pubkey_hex) {
  if (!self || !txn || !pk32 || !pubkey_hex) return NULL;

  GnNostrProfile *existing = profile_cache_get(self, pubkey_hex);
  if (existing) return existing;

  char *evt_json = NULL;
  int evt_len = 0;
  int rc = storage_ndb_get_profile_by_pubkey(txn, pk32, &evt_json, &evt_len);
  if (rc != 0 || !evt_json || evt_len <= 0) {
    if (evt_json) free(evt_json);
    return NULL;
  }

  NostrEvent *evt = nostr_event_new();
  if (!evt) {
    free(evt_json);
    return NULL;
  }

  GnNostrProfile *profile = NULL;

  if (nostr_event_deserialize(evt, evt_json) == 0 && nostr_event_get_kind(evt) == 0) {
    const char *content = nostr_event_get_content(evt);
    if (content && *content) {
      profile = gn_nostr_profile_new(pubkey_hex);
      gn_nostr_profile_update_from_json(profile, content);
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

  GnNostrProfile *profile = profile_cache_get(self, pubkey_hex);
  if (!profile) {
    profile = gn_nostr_profile_new(pubkey_hex);
    g_hash_table_replace(self->profile_cache, g_strdup(pubkey_hex), profile);
    /* Add to LRU queue (new entry) */
    g_queue_push_tail(self->profile_cache_lru, g_strdup(pubkey_hex));
    /* Evict if over limit */
    profile_cache_evict(self);
  }

  gn_nostr_profile_update_from_json(profile, tmp);
  mark_author_ready(self, pubkey_hex);

  g_free(tmp);
}

/* Notify cached items that their "profile" property should be re-read by views. */
static void notify_cached_items_for_pubkey(GnNostrEventModel *self, const char *pubkey_hex) {
  if (!self || !pubkey_hex || !self->item_cache) return;

  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, self->item_cache);

  while (g_hash_table_iter_next(&iter, &key, &value)) {
    GnNostrEventItem *item = GN_NOSTR_EVENT_ITEM(value);
    const char *item_pubkey = gn_nostr_event_item_get_pubkey(item);
    if (item_pubkey && g_strcmp0(item_pubkey, pubkey_hex) == 0) {
      g_object_notify(G_OBJECT(item), "profile");
    }
  }
}

static gboolean note_matches_query(GnNostrEventModel *self, int kind, const char *pubkey_hex, gint64 created_at) {
  if (!self) return FALSE;

  /* NIP-51 Mute list filter: check if author is muted */
  GnostrMuteList *mute_list = gnostr_mute_list_get_default();
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

  /* NIP-10 positional fallback: if no explicit markers found */
  if (!*root_id && first_e_id) {
    *root_id = g_strdup(first_e_id);
  }
  if (!*reply_id && last_e_id && g_strcmp0(last_e_id, first_e_id) != 0) {
    *reply_id = g_strdup(last_e_id);
  }
}

/* nostrc-yi2: Get current time in milliseconds */
static gint64 get_current_time_ms(void) {
  return g_get_monotonic_time() / 1000;
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

/* nostrc-yi2: Actually insert deferred notes into the model
 * 
 * OPTIMIZATION: Instead of emitting items_changed for each insertion (which
 * causes GTK ListView to recalculate layout N times), we:
 * 1. Filter and sort all deferred notes
 * 2. Insert them all at position 0 (they're all newer than existing items)
 * 3. Emit a SINGLE items_changed signal covering all insertions
 * 
 * This reduces O(N) layout recalculations to O(1), dramatically improving
 * performance when flushing many deferred notes (e.g., clicking "N new notes").
 */
static gboolean flush_deferred_notes_cb(gpointer user_data) {
  GnNostrEventModel *self = GN_NOSTR_EVENT_MODEL(user_data);
  if (!GN_IS_NOSTR_EVENT_MODEL(self)) return G_SOURCE_REMOVE;

  self->debounce_source_id = 0;  /* Mark as not pending */

  if (!self->deferred_notes || self->deferred_notes->len == 0) {
    return G_SOURCE_REMOVE;
  }

  /* Rate limit check */
  gint64 now_ms = get_current_time_ms();
  gint64 elapsed = now_ms - self->last_update_time_ms;
  if (elapsed < MIN_UPDATE_INTERVAL_MS && self->last_update_time_ms > 0) {
    /* Too soon, reschedule */
    gint64 delay = MIN_UPDATE_INTERVAL_MS - elapsed;
    self->debounce_source_id = g_timeout_add((guint)delay, flush_deferred_notes_cb, self);
    return G_SOURCE_REMOVE;
  }

  guint total_deferred = self->deferred_notes->len;
  g_debug("[CALM] Flushing %u deferred notes (batched)", total_deferred);

  /* Step 1: Filter out duplicates and collect valid entries */
  GArray *to_insert = g_array_new(FALSE, FALSE, sizeof(NoteEntry));
  for (guint i = 0; i < self->deferred_notes->len; i++) {
    NoteEntry *entry = &g_array_index(self->deferred_notes, NoteEntry, i);
    if (!has_note_key(self, entry->note_key)) {
      g_array_append_val(to_insert, *entry);
    }
  }

  guint n_to_insert = to_insert->len;
  if (n_to_insert == 0) {
    g_array_free(to_insert, TRUE);
    g_array_set_size(self->deferred_notes, 0);
    self->pending_new_count = 0;
    g_signal_emit(self, signals[SIGNAL_NEW_ITEMS_PENDING], 0, (guint)0);
    return G_SOURCE_REMOVE;
  }

  /* Step 2: Sort by created_at (newest first) */
  g_array_sort(to_insert, note_entry_compare_newest_first);

  /* Step 3: Merge deferred notes with existing notes using "replace all" strategy.
   * Instead of inserting at position 0 (which triggers aggressive widget recycling),
   * we rebuild the entire notes array and emit a single "replace all" signal.
   * This tells GTK "the model changed completely" which is handled more gracefully
   * than "N items inserted at front" which causes widget recycling storms. */
  
  guint old_count = self->notes->len;
  
  /* Create new array with deferred notes first, then existing notes */
  GArray *new_notes = g_array_new(FALSE, FALSE, sizeof(NoteEntry));
  g_array_set_size(new_notes, n_to_insert + old_count);
  
  /* Copy deferred notes (already sorted newest first) */
  for (guint i = 0; i < n_to_insert; i++) {
    NoteEntry *entry = &g_array_index(to_insert, NoteEntry, i);
    g_array_index(new_notes, NoteEntry, i) = *entry;
  }
  
  /* Copy existing notes after deferred notes */
  for (guint i = 0; i < old_count; i++) {
    NoteEntry *entry = &g_array_index(self->notes, NoteEntry, i);
    g_array_index(new_notes, NoteEntry, n_to_insert + i) = *entry;
  }
  
  /* Swap arrays */
  g_array_free(self->notes, TRUE);
  self->notes = new_notes;
  g_array_free(to_insert, TRUE);
  
  /* DON'T clear item cache - existing items are still valid, just at different positions.
   * The cache is keyed by note_key, not position, so items remain valid.
   * This avoids expensive DB transactions when GTK calls get_item after the signal. */
  
  /* Step 4: Emit "replace all" signal - this is gentler on GTK's widget recycling
   * than inserting at position 0. We tell GTK "old_count items removed, new total added"
   * which causes it to treat this as a model reset rather than a mass insertion. */
  g_list_model_items_changed(G_LIST_MODEL(self), 0, old_count, self->notes->len);

  g_debug("[CALM] Batch inserted %u notes with single signal", n_to_insert);

  /* Clear deferred queue */
  g_array_set_size(self->deferred_notes, 0);

  /* Update timestamp */
  self->last_update_time_ms = now_ms;

  /* Clear pending count and notify */
  self->pending_new_count = 0;
  g_signal_emit(self, signals[SIGNAL_NEW_ITEMS_PENDING], 0, (guint)0);

  /* Enforce window size to prevent unbounded growth */
  enforce_window(self);

  return G_SOURCE_REMOVE;
}

/* nostrc-yi2: Schedule a flush of deferred notes */
static void schedule_deferred_flush(GnNostrEventModel *self) {
  if (self->debounce_source_id > 0) {
    return;  /* Already scheduled */
  }
  self->debounce_source_id = g_timeout_add(DEBOUNCE_INTERVAL_MS, flush_deferred_notes_cb, self);
}

/* nostrc-yi2: Defer note insertion (when user is not at top) */
static void defer_note_insertion(GnNostrEventModel *self, uint64_t note_key, gint64 created_at) {
  /* Check if already in main notes array - prevents duplicates */
  if (has_note_key(self, note_key)) return;

  /* Check if already in deferred queue */
  for (guint i = 0; i < self->deferred_notes->len; i++) {
    NoteEntry *entry = &g_array_index(self->deferred_notes, NoteEntry, i);
    if (entry->note_key == note_key) return;  /* Already queued */
  }

  NoteEntry entry = { .note_key = note_key, .created_at = created_at };
  g_array_append_val(self->deferred_notes, entry);

  /* Enforce limit to prevent unbounded memory growth */
  if (self->deferred_notes->len > DEFERRED_NOTES_MAX) {
    g_debug("[CALM] Deferred notes exceeded limit (%u > %u), force flushing",
            self->deferred_notes->len, DEFERRED_NOTES_MAX);
    /* Remove oldest entries to stay within limit */
    guint to_remove = self->deferred_notes->len - DEFERRED_NOTES_MAX;
    g_array_remove_range(self->deferred_notes, 0, to_remove);
  }

  /* Update pending count and notify UI */
  self->pending_new_count = self->deferred_notes->len;
  g_signal_emit(self, signals[SIGNAL_NEW_ITEMS_PENDING], 0, self->pending_new_count);

  g_debug("[CALM] Deferred note insertion, %u pending", self->pending_new_count);
}

/* Add a note to the model (assumes gating has already been satisfied) */
static void add_note_internal(GnNostrEventModel *self, uint64_t note_key, gint64 created_at,
                               const char *root_id, const char *parent_id, guint depth) {
  if (has_note_key(self, note_key)) {
    return;  /* Already in model */
  }

  /* nostrc-yi2: Calm timeline - defer insertion if user is scrolled down reading
   * This prevents jarring auto-scroll and visual churn while the user is reading.
   * Notes are queued and a "N new notes" indicator is shown instead.
   * Skip deferral for thread views (they need immediate updates). */
  if (!self->is_thread_view && !self->user_at_top) {
    defer_note_insertion(self, note_key, created_at);
    return;
  }

  /* Store thread info (optional) */
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

  /* nostrc-7o7: Mark items added outside visible viewport to skip animation */
  if (pos < self->visible_start || pos > self->visible_end) {
    uint64_t *key_copy = g_new(uint64_t, 1);
    *key_copy = note_key;
    g_hash_table_insert(self->skip_animation_keys, key_copy, GINT_TO_POINTER(1));
  }

  /* Insert note entry */
  NoteEntry entry = { .note_key = note_key, .created_at = created_at };
  g_array_insert_val(self->notes, pos, entry);

  /* Emit items-changed signal immediately for each insertion */
  g_list_model_items_changed(G_LIST_MODEL(self), pos, 0, 1);
}

/* Actual enforce_window implementation - called from idle to avoid nested signals */
static gboolean enforce_window_idle_cb(gpointer user_data) {
  GnNostrEventModel *self = GN_NOSTR_EVENT_MODEL(user_data);
  if (!GN_IS_NOSTR_EVENT_MODEL(self)) return G_SOURCE_REMOVE;

  self->enforce_window_idle_id = 0;

  if (self->is_thread_view) return G_SOURCE_REMOVE;
  guint cap = self->window_size ? self->window_size : MODEL_MAX_ITEMS;

  if (self->notes->len <= cap) return G_SOURCE_REMOVE;

  guint to_remove = self->notes->len - cap;
  guint old_len = self->notes->len;

  /* Collect keys to remove BEFORE modifying any data structures */
  uint64_t *keys_to_remove = g_new(uint64_t, to_remove);
  for (guint i = 0; i < to_remove; i++) {
    guint idx = old_len - 1 - i;
    NoteEntry *old = &g_array_index(self->notes, NoteEntry, idx);
    keys_to_remove[i] = old->note_key;
  }

  /* Resize array and emit items_changed FIRST so GTK can tear down widgets
   * while the cached items are still valid */
  g_array_set_size(self->notes, cap);
  g_list_model_items_changed(G_LIST_MODEL(self), cap, to_remove, 0);

  /* NOW clean up caches after GTK has finished with the widgets */
  for (guint i = 0; i < to_remove; i++) {
    uint64_t k = keys_to_remove[i];
    cache_lru_remove_key(self, k);
    g_hash_table_remove(self->thread_info, &k);
    g_hash_table_remove(self->item_cache, &k);
  }

  g_free(keys_to_remove);
  return G_SOURCE_REMOVE;
}

/* Schedule enforce_window to run in idle - prevents nested items_changed signals
 * which cause GTK ListView widget teardown corruption */
static void enforce_window(GnNostrEventModel *self) {
  if (!self) return;
  if (self->enforce_window_idle_id > 0) return;  /* Already scheduled */
  self->enforce_window_idle_id = g_idle_add(enforce_window_idle_cb, self);
}

/* Pending queue: pubkey -> array of PendingEntry. Returns TRUE if this pubkey had no pending queue before. */
static gboolean add_pending(GnNostrEventModel *self, const char *pubkey_hex, uint64_t note_key, gint64 created_at) {
  if (!self || !self->pending_by_author || !pubkey_hex) return FALSE;

  /* Enforce limit on pending_by_author to prevent unbounded memory growth */
  guint pending_size = g_hash_table_size(self->pending_by_author);
  if (pending_size > PENDING_BY_AUTHOR_MAX) {
    g_debug("[MODEL] pending_by_author exceeded limit (%u > %u), clearing oldest entries",
            pending_size, PENDING_BY_AUTHOR_MAX);
    /* Clear half the entries to avoid frequent cleanup */
    GHashTableIter iter;
    gpointer k, v;
    guint to_remove = pending_size / 2;
    guint removed = 0;
    g_hash_table_iter_init(&iter, self->pending_by_author);
    while (g_hash_table_iter_next(&iter, &k, &v) && removed < to_remove) {
      g_hash_table_iter_remove(&iter);
      removed++;
    }
  }

  GArray *arr = g_hash_table_lookup(self->pending_by_author, pubkey_hex);
  gboolean first = (arr == NULL);

  if (!arr) {
    arr = g_array_new(FALSE, FALSE, sizeof(PendingEntry));
    g_hash_table_insert(self->pending_by_author, g_strdup(pubkey_hex), arr);
  }

  PendingEntry pe = { .note_key = note_key, .created_at = created_at };
  g_array_append_val(arr, pe);

  return first;
}

static void flush_pending_notes(GnNostrEventModel *self, const char *pubkey_hex) {
  if (!self || !pubkey_hex || !self->pending_by_author) return;

  gpointer orig_key = NULL;
  gpointer value = NULL;

  if (!g_hash_table_lookup_extended(self->pending_by_author, pubkey_hex, &orig_key, &value)) {
    return;
  }

  /* Steal so we can process without destroy notify running */
  g_hash_table_steal(self->pending_by_author, orig_key);

  GArray *arr = (GArray *)value;
  char *key_str = (char *)orig_key;

  if (!arr) {
    g_free(key_str);
    return;
  }

  /* Open transaction to access note tags for NIP-10 parsing */
  void *txn = NULL;
  gboolean have_txn = (storage_ndb_begin_query(&txn) == 0 && txn != NULL);

  for (guint i = 0; i < arr->len; i++) {
    PendingEntry *pe = &g_array_index(arr, PendingEntry, i);

    /* Extract NIP-10 thread info if we have a transaction */
    char *root_id = NULL;
    char *reply_id = NULL;
    if (have_txn) {
      storage_ndb_note *note = storage_ndb_get_note_ptr(txn, pe->note_key);
      if (note) {
        storage_ndb_note_get_nip10_thread(note, &root_id, &reply_id);
      }
    }

    add_note_internal(self, pe->note_key, pe->created_at, root_id, reply_id, 0);
    g_free(root_id);
    g_free(reply_id);
  }

  if (have_txn) {
    storage_ndb_end_query(txn);
  }

  g_array_unref(arr);
  g_free(key_str);

  enforce_window(self);
}

/* Remove a note_key from any pending queues. */
static void pending_remove_note_key(GnNostrEventModel *self, uint64_t note_key) {
  if (!self || !self->pending_by_author) return;

  GHashTableIter iter;
  gpointer k, v;
  g_hash_table_iter_init(&iter, self->pending_by_author);

  while (g_hash_table_iter_next(&iter, &k, &v)) {
    GArray *arr = (GArray *)v;
    if (!arr) continue;

    /* Remove in reverse to preserve indices */
    for (gint i = (gint)arr->len - 1; i >= 0; i--) {
      PendingEntry *pe = &g_array_index(arr, PendingEntry, (guint)i);
      if (pe->note_key == note_key) {
        g_array_remove_index(arr, (guint)i);
      }
    }

    if (arr->len == 0) {
      g_hash_table_iter_remove(&iter);
    }
  }
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
    g_list_model_items_changed(G_LIST_MODEL(self), i, 1, 0);

    /* NOW cleanup caches after GTK has finished with widgets */
    cache_lru_remove_key(self, note_key);
    g_hash_table_remove(self->thread_info, &note_key);
    g_hash_table_remove(self->item_cache, &note_key);

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
      pending_remove_note_key(self, target_key);
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
  }

  /* Apply profile if available (gated notes should always have profiles, but be defensive) */
  const char *pubkey = gn_nostr_event_item_get_pubkey(item);
  if (pubkey) {
    GnNostrProfile *profile = profile_cache_get(self, pubkey);
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

/* -------------------- Subscriptions -------------------- */

static void on_sub_profiles_batch(uint64_t subid, const uint64_t *note_keys, guint n_keys, gpointer user_data) {
  (void)subid;
  GnNostrEventModel *self = GN_NOSTR_EVENT_MODEL(user_data);
  if (!GN_IS_NOSTR_EVENT_MODEL(self) || !note_keys || n_keys == 0) return;

  void *txn = NULL;
  if (storage_ndb_begin_query(&txn) != 0 || !txn) return;

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

    /* Any pending notes for this author can now be inserted */
    flush_pending_notes(self, pubkey_hex);
  }

  storage_ndb_end_query(txn);
}

static void on_sub_timeline_batch(uint64_t subid, const uint64_t *note_keys, guint n_keys, gpointer user_data) {
  (void)subid;
  GnNostrEventModel *self = GN_NOSTR_EVENT_MODEL(user_data);
  if (!GN_IS_NOSTR_EVENT_MODEL(self) || !note_keys || n_keys == 0) return;

  void *txn = NULL;
  if (storage_ndb_begin_query(&txn) != 0 || !txn) {
    g_warning("[TIMELINE] failed to begin query");
    return;
  }

  guint added = 0, filtered = 0, pending = 0, no_note = 0;

  for (guint i = 0; i < n_keys; i++) {
    uint64_t note_key = note_keys[i];
    storage_ndb_note *note = storage_ndb_get_note_ptr(txn, note_key);

    if (!note) continue;

    uint32_t kind_u32 = storage_ndb_note_kind(note);
    int kind = (int)kind_u32;
    /* Allow kind 1 (text notes), kind 6 (reposts), and kind 1111 (NIP-22 comments) */
    if (kind != 1 && kind != 6 && kind != 1111) { filtered++; continue; }

    /* NIP-40: Filter out expired events */
    if (storage_ndb_note_is_expired(note)) { filtered++; continue; }

    gint64 created_at = (gint64)storage_ndb_note_created_at(note);

    const unsigned char *pk32 = storage_ndb_note_pubkey(note);
    if (!pk32) { no_note++; continue; }

    char pubkey_hex[65];
    storage_ndb_hex_encode(pk32, pubkey_hex);

    if (!note_matches_query(self, kind, pubkey_hex, created_at)) { filtered++; continue; }

    /* Extract NIP-10 thread info from note tags */
    char *root_id = NULL;
    char *reply_id = NULL;
    storage_ndb_note_get_nip10_thread(note, &root_id, &reply_id);

    if (author_is_ready(self, pubkey_hex)) {
      add_note_internal(self, note_key, created_at, root_id, reply_id, 0);
      g_free(root_id);
      g_free(reply_id);
      added++;
      continue;
    }

    /* Check DB for an existing profile and cache/mark ready if present */
    GnNostrProfile *p = profile_cache_ensure_from_db(self, txn, pk32, pubkey_hex);
    if (p) {
      (void)p;
      add_note_internal(self, note_key, created_at, root_id, reply_id, 0);
      g_free(root_id);
      g_free(reply_id);
      added++;
      continue;
    }

    g_free(root_id);
    g_free(reply_id);

    /* Still not ready: queue note and request profile fetch */
    gboolean first_pending = add_pending(self, pubkey_hex, note_key, created_at);
    if (first_pending) {
      g_signal_emit(self, signals[SIGNAL_NEED_PROFILE], 0, pubkey_hex);
    }
    pending++;
  }

  storage_ndb_end_query(txn);

  /* Enforce window size */
  enforce_window(self);
}

static void on_sub_deletes_batch(uint64_t subid, const uint64_t *note_keys, guint n_keys, gpointer user_data) {
  (void)subid;
  GnNostrEventModel *self = GN_NOSTR_EVENT_MODEL(user_data);
  if (!GN_IS_NOSTR_EVENT_MODEL(self) || !note_keys || n_keys == 0) return;

  void *txn = NULL;
  if (storage_ndb_begin_query(&txn) != 0 || !txn) return;

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
    int qrc = storage_ndb_query(txn, filter, &arr, &n);
    g_free(filter);

    if (qrc == 0 && arr && n > 0 && arr[0]) {
      handle_delete_event_json(self, txn, arr[0]);
    }

    storage_ndb_free_results(arr, n);
  }

  storage_ndb_end_query(txn);
}

/* -------------------- GObject boilerplate -------------------- */

static void gn_nostr_event_model_finalize(GObject *object) {
  GnNostrEventModel *self = GN_NOSTR_EVENT_MODEL(object);

  /* Unsubscribe from nostrdb via dispatcher */
  if (self->sub_timeline > 0) { gn_ndb_unsubscribe(self->sub_timeline); self->sub_timeline = 0; }
  if (self->sub_profiles > 0) { gn_ndb_unsubscribe(self->sub_profiles); self->sub_profiles = 0; }
  if (self->sub_deletes > 0)  { gn_ndb_unsubscribe(self->sub_deletes);  self->sub_deletes = 0;  }

  /* Free timeline query */
  if (self->timeline_query) {
    gn_timeline_query_free(self->timeline_query);
    self->timeline_query = NULL;
  }

  g_free(self->kinds);
  g_strfreev(self->authors);
  g_free(self->root_event_id);

  if (self->notes) g_array_unref(self->notes);
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
  if (self->pending_by_author) g_hash_table_unref(self->pending_by_author);
  if (self->thread_info) g_hash_table_unref(self->thread_info);

  /* nostrc-7o7: Clean up animation skip tracking */
  if (self->skip_animation_keys) g_hash_table_unref(self->skip_animation_keys);

  /* nostrc-yi2: Clean up calm timeline fields */
  if (self->debounce_source_id > 0) {
    g_source_remove(self->debounce_source_id);
    self->debounce_source_id = 0;
  }
  if (self->enforce_window_idle_id > 0) {
    g_source_remove(self->enforce_window_idle_id);
    self->enforce_window_idle_id = 0;
  }
  if (self->deferred_notes) g_array_unref(self->deferred_notes);

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

  self->item_cache = g_hash_table_new_full(uint64_hash, uint64_equal, g_free, g_object_unref);
  self->cache_lru = g_queue_new();

  self->profile_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
  self->profile_cache_lru = g_queue_new();
  self->thread_info = g_hash_table_new_full(uint64_hash, uint64_equal, g_free, (GDestroyNotify)thread_info_free);

  self->authors_ready = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->authors_ready_lru = g_queue_new();
  self->pending_by_author = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_array_unref);

  self->limit = MODEL_MAX_ITEMS;
  self->window_size = MODEL_MAX_ITEMS;

  /* nostrc-7o7: Initialize animation skip tracking */
  self->visible_start = 0;
  self->visible_end = 10;  /* Default to showing first 10 items as "visible" */
  self->skip_animation_keys = g_hash_table_new_full(uint64_hash, uint64_equal, g_free, NULL);

  /* nostrc-yi2: Initialize calm timeline fields */
  self->user_at_top = TRUE;  /* Assume user starts at top */
  self->deferred_notes = g_array_new(FALSE, FALSE, sizeof(NoteEntry));
  self->debounce_source_id = 0;
  self->last_update_time_ms = 0;
  self->pending_new_count = 0;

  /* Install lifetime subscriptions via dispatcher (marshals to main loop) */
  self->sub_profiles = gn_ndb_subscribe(FILTER_PROFILES, on_sub_profiles_batch, self, NULL);
  self->sub_timeline = gn_ndb_subscribe(FILTER_TIMELINE, on_sub_timeline_batch, self, NULL);
  self->sub_deletes  = gn_ndb_subscribe(FILTER_DELETES,  on_sub_deletes_batch,  self, NULL);
}

/* -------------------- Public API -------------------- */

GnNostrEventModel *gn_nostr_event_model_new(void) {
  return g_object_new(GN_TYPE_NOSTR_EVENT_MODEL, NULL);
}

GnNostrEventModel *gn_nostr_event_model_new_with_query(GnTimelineQuery *query) {
  GnNostrEventModel *self = gn_nostr_event_model_new();
  if (query) {
    gn_nostr_event_model_set_timeline_query(self, query);
  }
  return self;
}

void gn_nostr_event_model_set_timeline_query(GnNostrEventModel *self, GnTimelineQuery *query) {
  g_return_if_fail(GN_IS_NOSTR_EVENT_MODEL(self));

  /* Free old query */
  if (self->timeline_query) {
    gn_timeline_query_free(self->timeline_query);
    self->timeline_query = NULL;
  }

  if (!query) return;

  /* Store a copy of the query */
  self->timeline_query = gn_timeline_query_copy(query);

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

GnTimelineQuery *gn_nostr_event_model_get_timeline_query(GnNostrEventModel *self) {
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

  /* Clear current window */
  gn_nostr_event_model_clear(self);

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
  if (storage_ndb_begin_query(&txn) != 0 || !txn) {
    g_warning("[MODEL] Failed to begin query");
    g_string_free(filter, TRUE);
    return;
  }

  char **json_results = NULL;
  int count = 0;
  int query_rc = storage_ndb_query(txn, filter->str, &json_results, &count);

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

        /* Gate by presence of kind-0 profile */
        uint8_t pk32[32];
        if (!hex_to_bytes32(pubkey_hex, pk32)) {
          nostr_event_free(evt);
          continue;
        }

        if (!db_has_profile_event_for_pubkey(txn, pk32)) {
          /* Queue and request profile */
          gboolean first_pending = add_pending(self, pubkey_hex, note_key, created_at);
          if (first_pending) {
            g_signal_emit(self, signals[SIGNAL_NEED_PROFILE], 0, pubkey_hex);
          }
          nostr_event_free(evt);
          continue;
        }

        /* Cache profile + mark ready */
        (void)profile_cache_ensure_from_db(self, txn, pk32, pubkey_hex);

        /* Parse thread info best-effort */
        char *root_id = NULL;
        char *reply_id = NULL;
        parse_nip10_tags(evt, &root_id, &reply_id);

        add_note_internal(self, note_key, created_at, root_id, reply_id, 0);
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

  enforce_window(self);

  g_debug("[MODEL] Refresh complete: %u total items (%u added)", self->notes->len, added);
}

void gn_nostr_event_model_update_profile(GObject *model, const char *pubkey_hex, const char *content_json) {
  g_return_if_fail(GN_IS_NOSTR_EVENT_MODEL(model));
  g_return_if_fail(pubkey_hex != NULL);
  g_return_if_fail(content_json != NULL);

  GnNostrEventModel *self = GN_NOSTR_EVENT_MODEL(model);

  profile_cache_update_from_content(self, pubkey_hex, content_json, strlen(content_json));
  notify_cached_items_for_pubkey(self, pubkey_hex);

  /* New profile may unblock pending notes */
  flush_pending_notes(self, pubkey_hex);
}

void gn_nostr_event_model_check_pending_for_profile(GnNostrEventModel *self, const char *pubkey) {
  /* Subscription-driven gating handles this automatically now. */
  (void)self;
  (void)pubkey;
}

void gn_nostr_event_model_clear(GnNostrEventModel *self) {
  g_return_if_fail(GN_IS_NOSTR_EVENT_MODEL(self));

  guint old_size = self->notes->len;
  if (old_size == 0) {
    /* Still clear caches/pending to be safe */
    g_hash_table_remove_all(self->item_cache);
    g_queue_clear(self->cache_lru);
    g_hash_table_remove_all(self->thread_info);
    g_hash_table_remove_all(self->pending_by_author);
    return;
  }

  /* Resize array and emit items_changed FIRST so GTK can tear down widgets
   * while cached items are still valid */
  g_array_set_size(self->notes, 0);
  g_list_model_items_changed(G_LIST_MODEL(self), 0, old_size, 0);

  /* NOW clear caches after GTK has finished with widgets */
  g_hash_table_remove_all(self->item_cache);
  g_queue_clear(self->cache_lru);
  g_hash_table_remove_all(self->thread_info);
  g_hash_table_remove_all(self->pending_by_author);

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
  if (storage_ndb_begin_query(&txn) != 0 || !txn) {
    nostr_event_free(evt);
    return;
  }

  uint64_t note_key = storage_ndb_get_note_key_by_id(txn, id32, NULL);
  if (note_key == 0) {
    storage_ndb_end_query(txn);
    nostr_event_free(evt);
    return;
  }

  uint8_t pk32[32];
  if (!hex_to_bytes32(pubkey_hex, pk32)) {
    storage_ndb_end_query(txn);
    nostr_event_free(evt);
    return;
  }

  if (!db_has_profile_event_for_pubkey(txn, pk32)) {
    gboolean first_pending = add_pending(self, pubkey_hex, note_key, created_at);
    if (first_pending) g_signal_emit(self, signals[SIGNAL_NEED_PROFILE], 0, pubkey_hex);
    storage_ndb_end_query(txn);
    nostr_event_free(evt);
    return;
  }

  (void)profile_cache_ensure_from_db(self, txn, pk32, pubkey_hex);

  /* Best-effort thread parse */
  char *root_id = NULL;
  char *reply_id = NULL;
  parse_nip10_tags(evt, &root_id, &reply_id);

  add_note_internal(self, note_key, created_at, root_id, reply_id, 0);
  enforce_window(self);

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
    cache_lru_remove_key(self, k);
    g_hash_table_remove(self->thread_info, &k);
    g_hash_table_remove(self->item_cache, &k);
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
  if (storage_ndb_begin_query(&txn) != 0 || !txn) {
    g_warning("[MODEL] load_older: Failed to begin query");
    g_string_free(filter, TRUE);
    return 0;
  }

  char **json_results = NULL;
  int result_count = 0;
  int query_rc = storage_ndb_query(txn, filter->str, &json_results, &result_count);

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

        /* NIP-40: Filter out expired events */
        if (note_ptr && storage_ndb_note_is_expired(note_ptr)) {
          nostr_event_free(evt);
          continue;
        }

        /* Skip if already in model */
        if (has_note_key(self, note_key)) {
          nostr_event_free(evt);
          continue;
        }

        uint8_t pk32[32];
        if (!hex_to_bytes32(pubkey_hex, pk32)) {
          nostr_event_free(evt);
          continue;
        }

        /* Gate by presence of kind-0 profile */
        if (!db_has_profile_event_for_pubkey(txn, pk32)) {
          gboolean first_pending = add_pending(self, pubkey_hex, note_key, created_at);
          if (first_pending) {
            g_signal_emit(self, signals[SIGNAL_NEED_PROFILE], 0, pubkey_hex);
          }
          nostr_event_free(evt);
          continue;
        }

        (void)profile_cache_ensure_from_db(self, txn, pk32, pubkey_hex);

        char *root_id = NULL;
        char *reply_id = NULL;
        parse_nip10_tags(evt, &root_id, &reply_id);

        add_note_internal(self, note_key, created_at, root_id, reply_id, 0);
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
    cache_lru_remove_key(self, k);
    g_hash_table_remove(self->thread_info, &k);
    g_hash_table_remove(self->item_cache, &k);
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
  if (storage_ndb_begin_query(&txn) != 0 || !txn) {
    g_warning("[MODEL] load_newer: Failed to begin query");
    g_string_free(filter, TRUE);
    return 0;
  }

  char **json_results = NULL;
  int result_count = 0;
  int query_rc = storage_ndb_query(txn, filter->str, &json_results, &result_count);

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

        /* NIP-40: Filter out expired events */
        if (note_ptr && storage_ndb_note_is_expired(note_ptr)) {
          nostr_event_free(evt);
          continue;
        }

        /* Skip if already in model */
        if (has_note_key(self, note_key)) {
          nostr_event_free(evt);
          continue;
        }

        uint8_t pk32[32];
        if (!hex_to_bytes32(pubkey_hex, pk32)) {
          nostr_event_free(evt);
          continue;
        }

        /* Gate by presence of kind-0 profile */
        if (!db_has_profile_event_for_pubkey(txn, pk32)) {
          gboolean first_pending = add_pending(self, pubkey_hex, note_key, created_at);
          if (first_pending) {
            g_signal_emit(self, signals[SIGNAL_NEED_PROFILE], 0, pubkey_hex);
          }
          nostr_event_free(evt);
          continue;
        }

        (void)profile_cache_ensure_from_db(self, txn, pk32, pubkey_hex);

        char *root_id = NULL;
        char *reply_id = NULL;
        parse_nip10_tags(evt, &root_id, &reply_id);

        add_note_internal(self, note_key, created_at, root_id, reply_id, 0);
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


  return added;
}

/* nostrc-7o7: Update visible range for animation skip tracking */
void gn_nostr_event_model_set_visible_range(GnNostrEventModel *self, guint start, guint end) {
  g_return_if_fail(GN_IS_NOSTR_EVENT_MODEL(self));
  self->visible_start = start;
  self->visible_end = end;
}

/* nostrc-yi2: Set whether user is at top of scroll (enables auto-insert) */
void gn_nostr_event_model_set_user_at_top(GnNostrEventModel *self, gboolean at_top) {
  g_return_if_fail(GN_IS_NOSTR_EVENT_MODEL(self));

  gboolean was_at_top = self->user_at_top;
  self->user_at_top = at_top;

  /* If user just scrolled to top, flush any deferred notes */
  if (at_top && !was_at_top && self->deferred_notes && self->deferred_notes->len > 0) {
    g_debug("[CALM] User scrolled to top, flushing %u deferred notes", self->deferred_notes->len);
    flush_deferred_notes_cb(self);
  }
}

/* nostrc-yi2: Get the count of pending new items */
guint gn_nostr_event_model_get_pending_count(GnNostrEventModel *self) {
  g_return_val_if_fail(GN_IS_NOSTR_EVENT_MODEL(self), 0);
  return self->pending_new_count;
}

/* nostrc-yi2: Force flush of deferred notes (e.g., when user clicks indicator) */
void gn_nostr_event_model_flush_pending(GnNostrEventModel *self) {
  g_return_if_fail(GN_IS_NOSTR_EVENT_MODEL(self));

  if (self->deferred_notes && self->deferred_notes->len > 0) {
    g_debug("[CALM] Manually flushing %u deferred notes", self->deferred_notes->len);
    /* Cancel any pending debounce */
    if (self->debounce_source_id > 0) {
      g_source_remove(self->debounce_source_id);
      self->debounce_source_id = 0;
    }
    flush_deferred_notes_cb(self);
  }
}
