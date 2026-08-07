#define G_LOG_DOMAIN "gnostr-timeline-source"

#include "gnostr-timeline-source.h"

#include <nostr-gobject-1.0/gn-ndb-sub-dispatcher.h>
#include <nostr-gobject-1.0/gnostr-mute-list.h>
#include <nostr-gobject-1.0/storage_ndb.h>
#include <nostr.h>
#include <string.h>

#include "../util/mute_filter.h"

#define FILTER_PROFILES     "{\"kinds\":[0]}"
#define FILTER_DELETES      "{\"kinds\":[5]}"
#define FILTER_INTERACTIONS "{\"kinds\":[1,6,7,9735]}"
#define DEFAULT_QUERY_LIMIT 50
#define SOURCE_KEY_QUEUE_CAP 512u
#define SOURCE_KEY_COALESCE_MS 10u

typedef enum {
  SOURCE_STREAM_TIMELINE = 0,
  SOURCE_STREAM_PROFILE,
  SOURCE_STREAM_DELETE,
  SOURCE_STREAM_METADATA,
  SOURCE_STREAM_COUNT
} SourceStream;

typedef struct {
  GArray *pending;       /* uint64_t, owner-main-context only */
  GHashTable *seen;      /* uint64_t set for pending keys */
  GHashTable *in_flight; /* uint64_t set for the active request */
  guint flush_source_id;
  guint dropped;
  guint64 generation;
  guint64 request_serial;
  gboolean active;
} SourceKeyQueue;

struct _GnostrTimelineSource {
  GObject parent_instance;

  GNostrTimelineQuery *query;
  guint64 generation;

  uint64_t sub_timeline;
  uint64_t sub_profiles;
  uint64_t sub_deletes;
  uint64_t sub_metadata;
  char *timeline_filter_json;

  SourceKeyQueue queues[SOURCE_STREAM_COUNT];
  GCancellable *generation_cancellable;
  GHashTable *completed_requests; /* guint64 sequence -> SourceCompletion */
  guint64 next_request_sequence;
  guint64 next_emit_sequence;
  gboolean disposed;
};

G_DEFINE_TYPE(GnostrTimelineSource, gnostr_timeline_source, G_TYPE_OBJECT)

enum {
  SIGNAL_BATCH,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

typedef struct {
  GnostrTimelineBatchKind kind;
  guint64 generation;
  GNostrTimelineQuery *query;
  uint64_t *note_keys;
  guint n_keys;
  guint requested_count;
  SourceStream stream;
  guint64 request_serial;
  guint64 request_sequence;
  gboolean subscription_request;
} SourceBatchRequest;

typedef struct {
  GnostrTimelineBatch *batch;
  GError *error;
  gboolean subscription_request;
} SourceCompletion;

typedef struct {
  StorageNdbProfileMeta meta;
} SourceProfileCacheEntry;

static void
source_profile_cache_entry_free(gpointer data)
{
  SourceProfileCacheEntry *entry = data;
  if (!entry)
    return;
  storage_ndb_profile_meta_clear(&entry->meta);
  g_free(entry);
}

static SourceProfileCacheEntry *
source_profile_cache_lookup(GHashTable *cache,
                            void *txn,
                            const unsigned char pk32[32],
                            const char *pubkey_hex)
{
  SourceProfileCacheEntry *entry = g_hash_table_lookup(cache, pubkey_hex);
  if (entry)
    return entry;

  entry = g_new0(SourceProfileCacheEntry, 1);
  /* Cache misses too: event_exists remains FALSE, while last_fetch still
   * records nostrdb's staleness marker without a second profile read. */
  storage_ndb_get_profile_meta_direct(txn, pk32, &entry->meta, NULL);
  g_hash_table_insert(cache, g_strdup(pubkey_hex), entry);
  return entry;
}

static gboolean hex_to_bytes32(const char *hex, uint8_t out[32]);
static void source_populate_delete_batch(GnostrTimelineBatch *batch,
                                         const uint64_t *note_keys,
                                         guint n_keys,
                                         GCancellable *cancellable);
static void source_populate_patch_batch(GnostrTimelineBatch *batch,
                                        const uint64_t *note_keys,
                                        guint n_keys,
                                        GCancellable *cancellable);
static void on_sub_timeline_batch(uint64_t subid, const uint64_t *note_keys, guint n_keys, gpointer user_data);
static void on_sub_profiles_batch(uint64_t subid, const uint64_t *note_keys, guint n_keys, gpointer user_data);
static void on_sub_deletes_batch(uint64_t subid, const uint64_t *note_keys, guint n_keys, gpointer user_data);
static void on_sub_metadata_batch(uint64_t subid, const uint64_t *note_keys, guint n_keys, gpointer user_data);
static void source_queue_schedule(GnostrTimelineSource *self, SourceStream stream);

static guint
source_u64_hash(gconstpointer key)
{
  guint64 value = *(const guint64 *)key;
  return (guint)(value ^ (value >> 32));
}

static gboolean
source_u64_equal(gconstpointer a,
                 gconstpointer b)
{
  return *(const guint64 *)a == *(const guint64 *)b;
}

static void
source_key_queue_init(SourceKeyQueue *queue,
                      guint64 generation)
{
  queue->pending = g_array_new(FALSE, FALSE, sizeof(guint64));
  queue->seen = g_hash_table_new_full(source_u64_hash, source_u64_equal, g_free, NULL);
  queue->in_flight = g_hash_table_new_full(source_u64_hash, source_u64_equal, g_free, NULL);
  queue->generation = generation;
}

static void
source_key_queue_reset(SourceKeyQueue *queue,
                       guint64 generation)
{
  if (queue->flush_source_id != 0) {
    g_source_remove(queue->flush_source_id);
    queue->flush_source_id = 0;
  }
  g_array_set_size(queue->pending, 0);
  g_hash_table_remove_all(queue->seen);
  g_hash_table_remove_all(queue->in_flight);
  queue->generation = generation;
  queue->dropped = 0;
  queue->active = FALSE;
  queue->request_serial++;
}

static void
source_key_queue_clear(SourceKeyQueue *queue)
{
  if (!queue)
    return;
  source_key_queue_reset(queue, queue->generation);
  g_clear_pointer(&queue->pending, g_array_unref);
  g_clear_pointer(&queue->seen, g_hash_table_unref);
  g_clear_pointer(&queue->in_flight, g_hash_table_unref);
}

static void
source_key_queue_add_set_key(GHashTable *set,
                             guint64 value)
{
  guint64 *owned = g_new(guint64, 1);
  *owned = value;
  g_hash_table_add(set, owned);
}

static void
source_key_queue_enqueue(SourceKeyQueue *queue,
                         guint64 generation,
                         const uint64_t *note_keys,
                         guint n_keys)
{
  if (!queue || generation != queue->generation || !note_keys)
    return;

  for (guint i = 0; i < n_keys; i++) {
    guint64 key = note_keys[i];
    if (key == 0 ||
        g_hash_table_contains(queue->seen, &key) ||
        g_hash_table_contains(queue->in_flight, &key))
      continue;

    if (queue->pending->len >= SOURCE_KEY_QUEUE_CAP) {
      guint64 oldest = g_array_index(queue->pending, guint64, 0);
      g_hash_table_remove(queue->seen, &oldest);
      g_array_remove_index(queue->pending, 0);
      queue->dropped++;
    }

    g_array_append_val(queue->pending, key);
    source_key_queue_add_set_key(queue->seen, key);
  }
}

static uint64_t *
source_key_queue_take(SourceKeyQueue *queue,
                      guint *out_n_keys,
                      guint64 *out_serial)
{
  *out_n_keys = 0;
  if (!queue || queue->active || queue->pending->len == 0)
    return NULL;

  guint n_keys = queue->pending->len;
  uint64_t *keys = g_memdup2(queue->pending->data, n_keys * sizeof(uint64_t));
  g_hash_table_remove_all(queue->in_flight);
  for (guint i = 0; i < n_keys; i++)
    source_key_queue_add_set_key(queue->in_flight, keys[i]);
  g_array_set_size(queue->pending, 0);
  g_hash_table_remove_all(queue->seen);
  queue->active = TRUE;
  queue->request_serial++;

  *out_n_keys = n_keys;
  *out_serial = queue->request_serial;
  return keys;
}

static void
source_batch_request_free(SourceBatchRequest *req)
{
  if (!req)
    return;
  g_clear_pointer(&req->query, gnostr_timeline_query_free);
  g_free(req->note_keys);
  g_free(req);
}

static void
source_completion_free(SourceCompletion *completion)
{
  if (!completion)
    return;
  g_clear_object(&completion->batch);
  g_clear_error(&completion->error);
  g_free(completion);
}

static GNostrTimelineQuery *
source_query_copy_or_default(GnostrTimelineSource *self)
{
  if (self->query)
    return gnostr_timeline_query_copy(self->query);

  GNostrTimelineQuery *query = gnostr_timeline_query_new_global();
  if (query && query->limit == 0)
    query->limit = DEFAULT_QUERY_LIMIT;
  return query;
}

static gboolean
hex_to_bytes32(const char *hex, uint8_t out[32])
{
  if (!hex || !out || strlen(hex) != 64)
    return FALSE;

  for (int i = 0; i < 32; i++) {
    char c1 = hex[i * 2];
    char c2 = hex[i * 2 + 1];
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

static gboolean
query_matches_note(GNostrTimelineQuery *query,
                   int kind,
                   const char *pubkey_hex,
                   gint64 created_at)
{
  if (!query)
    return TRUE;

  if (query->n_kinds > 0) {
    gboolean kind_ok = FALSE;
    for (gsize i = 0; i < query->n_kinds; i++) {
      if (query->kinds[i] == kind) {
        kind_ok = TRUE;
        break;
      }
    }
    if (!kind_ok)
      return FALSE;
  }

  if (query->n_authors > 0) {
    gboolean author_ok = FALSE;
    for (gsize i = 0; i < query->n_authors; i++) {
      if (query->authors[i] && pubkey_hex && g_strcmp0(query->authors[i], pubkey_hex) == 0) {
        author_ok = TRUE;
        break;
      }
    }
    if (!author_ok)
      return FALSE;
  }

  if (query->since > 0 && created_at > 0 && created_at < query->since)
    return FALSE;
  if (query->until > 0 && created_at > 0 && created_at > query->until)
    return FALSE;

  return TRUE;
}

static gboolean
note_is_muted_by_fields(storage_ndb_note *note,
                        const char *pubkey_hex)
{
  GNostrMuteList *mute_list = gnostr_mute_list_get_default();
  if (!mute_list)
    return FALSE;

  const char *content = storage_ndb_note_content(note);
  g_auto(GStrv) hashtags = storage_ndb_note_get_hashtags(note);

  return gnostr_mute_filter_should_hide_fields(mute_list,
                                               pubkey_hex,
                                               content,
                                               hashtags);
}

static gboolean
add_note_key_to_batch_from_txn(GnostrTimelineBatch *batch,
                               GNostrTimelineQuery *query,
                               void *txn,
                               GHashTable *profile_cache,
                               uint64_t note_key,
                               gboolean apply_query_filter)
{
  storage_ndb_note *note = storage_ndb_get_note_ptr(txn, note_key);
  if (!note)
    return FALSE;

  int kind = (int)storage_ndb_note_kind(note);
  if (kind != 1 && kind != 6 && kind != 1111 && kind != 9735)
    return FALSE;
  if (storage_ndb_note_is_expired(note))
    return FALSE;

  const unsigned char *pk32 = storage_ndb_note_pubkey(note);
  if (!pk32)
    return FALSE;

  char pubkey_hex[65];
  storage_ndb_hex_encode(pk32, pubkey_hex);

  if (note_is_muted_by_fields(note, pubkey_hex))
    return FALSE;

  gint64 created_at = (gint64)storage_ndb_note_created_at(note);
  if (apply_query_filter && !query_matches_note(query, kind, pubkey_hex, created_at))
    return FALSE;

  char *root_id = NULL;
  char *reply_id = NULL;
  storage_ndb_note_get_nip10_thread(note, &root_id, &reply_id);
  g_autofree char *quoted_event_id = storage_ndb_note_get_qtag(note);
  g_autofree char *reposted_event_id = kind == 6 ? storage_ndb_note_get_last_etag(note) : NULL;
  g_auto(GStrv) hashtags = storage_ndb_note_get_hashtags(note);
  storage_ndb_blocks *content_blocks = storage_ndb_get_blocks(txn, note_key);

  const char *content_ptr = storage_ndb_note_content(note);
  uint32_t content_len = content_ptr ? storage_ndb_note_content_length(note) : 0;
  g_autofree char *content = content_ptr ? g_strndup(content_ptr, content_len) : NULL;

  SourceProfileCacheEntry *profile =
    source_profile_cache_lookup(profile_cache, txn, pk32, pubkey_hex);
  gboolean has_profile = profile->meta.event_exists;
  if (!has_profile)
    gnostr_timeline_batch_add_profile_request(batch, pubkey_hex);

  GnostrTimelineBatchEntry entry = {
    .note_key = note_key,
    .content_blocks = content_blocks,
    .created_at = created_at,
    .pubkey_hex = pubkey_hex,
    .content = content,
    .display_name = profile->meta.display_name,
    .handle = profile->meta.name,
    .avatar_url = profile->meta.picture,
    .nip05 = profile->meta.nip05,
    .root_id = root_id,
    .reply_id = reply_id,
    .quoted_event_id = quoted_event_id,
    .reposted_event_id = reposted_event_id,
    .hashtags = hashtags,
    .kind = kind,
    .has_profile = has_profile,
  };
  memcpy(entry.event_id, storage_ndb_note_id(note), sizeof(entry.event_id));
  gnostr_timeline_batch_add_entry(batch, &entry);
  storage_ndb_blocks_free(content_blocks);

  g_free(root_id);
  g_free(reply_id);
  return TRUE;
}

static void
query_results_into_batch(GnostrTimelineBatch *batch,
                         GNostrTimelineQuery *query,
                         guint requested_count,
                         GCancellable *cancellable)
{
  if (cancellable && g_cancellable_is_cancelled(cancellable))
    return;

  const char *query_json = gnostr_timeline_query_to_json(query);
  if (!query_json)
    return;

  void *txn = NULL;
  if (storage_ndb_begin_query(&txn, NULL) != 0 || !txn) {
    g_warning("[SOURCE] query begin failed");
    return;
  }

  StorageNdbNoteKeyResult *key_results = NULL;
  int result_count = 0;
  int rc = storage_ndb_query_note_keys(txn, query_json,
                                       &key_results, &result_count, NULL);
  GHashTable *profile_cache = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                    g_free,
                                                    source_profile_cache_entry_free);

  guint added = 0;
  if (rc == 0 && key_results && result_count > 0) {
    for (int i = 0; i < result_count; i++) {
      if (cancellable && g_cancellable_is_cancelled(cancellable))
        break;
      if (key_results[i].note_key != 0 &&
          add_note_key_to_batch_from_txn(batch, query, txn, profile_cache,
                                         key_results[i].note_key, TRUE)) {
        added++;
        if (requested_count > 0 && added >= requested_count)
          break;
      }
    }
  }

  g_free(key_results);
  g_hash_table_unref(profile_cache);
  storage_ndb_end_query(txn);
}

static void
source_batch_thread_func(GTask *task,
                         gpointer source_object G_GNUC_UNUSED,
                         gpointer task_data,
                         GCancellable *cancellable)
{
  SourceBatchRequest *req = task_data;
  if (g_task_return_error_if_cancelled(task))
    return;

  /* Notification projection runs on a reusable GTask worker. Drop that
   * worker's retained read snapshot before opening a transaction so newly
   * committed subscription keys and note-meta are visible immediately. */
  storage_ndb_invalidate_txn_cache();

  GnostrTimelineBatch *batch = gnostr_timeline_batch_new(req->kind, req->generation);

  if (req->kind == GNOSTR_TIMELINE_BATCH_DELETE) {
    source_populate_delete_batch(batch, req->note_keys, req->n_keys, cancellable);
  } else if (req->kind == GNOSTR_TIMELINE_BATCH_PROFILE_PATCH ||
             req->kind == GNOSTR_TIMELINE_BATCH_METADATA_PATCH) {
    source_populate_patch_batch(batch, req->note_keys, req->n_keys, cancellable);
  } else if (req->note_keys && req->n_keys > 0) {
    void *txn = NULL;
    if (storage_ndb_begin_query(&txn, NULL) == 0 && txn) {
      GHashTable *profile_cache =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                              source_profile_cache_entry_free);
      for (guint i = 0; i < req->n_keys; i++) {
        if (g_cancellable_is_cancelled(cancellable))
          break;
        add_note_key_to_batch_from_txn(batch, req->query, txn, profile_cache,
                                       req->note_keys[i], TRUE);
      }
      g_hash_table_unref(profile_cache);
      storage_ndb_end_query(txn);
    }
  } else {
    query_results_into_batch(batch, req->query, req->requested_count,
                             cancellable);
  }

  if (g_task_return_error_if_cancelled(task)) {
    g_object_unref(batch);
    return;
  }
  g_task_return_pointer(task, batch, g_object_unref);
}

static gboolean
source_batch_has_payload(GnostrTimelineBatch *batch)
{
  switch (gnostr_timeline_batch_get_kind(batch)) {
    case GNOSTR_TIMELINE_BATCH_PROFILE_PATCH:
      return gnostr_timeline_batch_get_n_profile_patches(batch) > 0;
    case GNOSTR_TIMELINE_BATCH_METADATA_PATCH:
      return gnostr_timeline_batch_get_n_metadata_patches(batch) > 0;
    case GNOSTR_TIMELINE_BATCH_DELETE:
      return gnostr_timeline_batch_get_n_entries(batch) > 0 ||
             gnostr_timeline_batch_get_n_delete_targets(batch) > 0;
    default:
      return gnostr_timeline_batch_get_n_entries(batch) > 0;
  }
}

static void
source_queue_complete(GnostrTimelineSource *self,
                      SourceBatchRequest *req)
{
  if (!req->subscription_request || req->stream >= SOURCE_STREAM_COUNT)
    return;

  SourceKeyQueue *queue = &self->queues[req->stream];
  if (queue->generation != req->generation ||
      queue->request_serial != req->request_serial)
    return;

  queue->active = FALSE;
  g_hash_table_remove_all(queue->in_flight);
  if (!self->disposed && queue->pending->len > 0)
    source_queue_schedule(self, req->stream);
}

static void
source_publish_completion(GnostrTimelineSource *self,
                          SourceCompletion *completion)
{
  if (!completion->batch) {
    if (completion->error &&
        !g_error_matches(completion->error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_warning("[SOURCE] Batch projection failed: %s", completion->error->message);
    return;
  }

  /* Request batches are completion-bearing: empty refresh/page results must
   * reach consumers so they can clear state and pagination loading flags.
   * Subscription batches remain payload-only to avoid no-op global metadata
   * notifications (notably top-level kind-1 events). */
  if (!completion->subscription_request ||
      source_batch_has_payload(completion->batch))
    g_signal_emit(self, signals[SIGNAL_BATCH], 0, completion->batch);
}

static void
source_drain_completed_requests(GnostrTimelineSource *self)
{
  gpointer key = NULL;
  gpointer value = NULL;

  while (g_hash_table_steal_extended(self->completed_requests,
                                     &self->next_emit_sequence,
                                     &key, &value)) {
    SourceCompletion *completion = value;
    /* Advance before signal emission: a consumer may synchronously set a new
     * query, which resets the ordering state for the next generation. */
    self->next_emit_sequence++;
    source_publish_completion(self, completion);
    g_free(key);
    source_completion_free(completion);
  }
}

static void
source_batch_done_cb(GObject *source_object,
                     GAsyncResult *res,
                     gpointer user_data G_GNUC_UNUSED)
{
  GnostrTimelineSource *self = GNOSTR_TIMELINE_SOURCE(source_object);
  if (!GNOSTR_IS_TIMELINE_SOURCE(self))
    return;

  SourceBatchRequest *req = g_task_get_task_data(G_TASK(res));
  g_autoptr(GError) error = NULL;
  GnostrTimelineBatch *batch = g_task_propagate_pointer(G_TASK(res), &error);
  source_queue_complete(self, req);

  if (self->disposed || req->generation != self->generation) {
    if (batch)
      g_object_unref(batch);
    return;
  }

  SourceCompletion *completion = g_new0(SourceCompletion, 1);
  completion->batch = batch;
  completion->error = g_steal_pointer(&error);
  completion->subscription_request = req->subscription_request;

  guint64 *sequence = g_new(guint64, 1);
  *sequence = req->request_sequence;
  g_hash_table_insert(self->completed_requests, sequence, completion);
  source_drain_completed_requests(self);
}

static void
source_run_request(GnostrTimelineSource *self,
                   SourceBatchRequest *req)
{
  req->request_sequence = self->next_request_sequence++;
  GTask *task = g_task_new(self, self->generation_cancellable,
                           source_batch_done_cb, NULL);
  g_task_set_task_data(task, req, (GDestroyNotify)source_batch_request_free);
  g_task_run_in_thread(task, source_batch_thread_func);
  g_object_unref(task);
}

static void
source_start_key_batch(GnostrTimelineSource *self,
                       GnostrTimelineBatchKind kind,
                       SourceStream stream,
                       guint64 request_serial,
                       const uint64_t *note_keys,
                       guint n_keys)
{
  if (!note_keys || n_keys == 0)
    return;

  SourceBatchRequest *req = g_new0(SourceBatchRequest, 1);
  req->kind = kind;
  req->generation = self->generation;
  req->query = source_query_copy_or_default(self);
  req->note_keys = g_memdup2(note_keys, n_keys * sizeof(uint64_t));
  req->n_keys = n_keys;
  req->stream = stream;
  req->request_serial = request_serial;
  req->subscription_request = TRUE;

  source_run_request(self, req);
}

typedef struct {
  GnostrTimelineSource *source;
  SourceStream stream;
  guint64 generation;
} SourceFlushData;

static void
source_flush_data_free(SourceFlushData *data)
{
  if (!data)
    return;
  g_clear_object(&data->source);
  g_free(data);
}

static GnostrTimelineBatchKind
source_stream_batch_kind(SourceStream stream)
{
  switch (stream) {
    case SOURCE_STREAM_TIMELINE: return GNOSTR_TIMELINE_BATCH_LIVE_HEAD;
    case SOURCE_STREAM_PROFILE:  return GNOSTR_TIMELINE_BATCH_PROFILE_PATCH;
    case SOURCE_STREAM_DELETE:   return GNOSTR_TIMELINE_BATCH_DELETE;
    case SOURCE_STREAM_METADATA: return GNOSTR_TIMELINE_BATCH_METADATA_PATCH;
    default: g_assert_not_reached();
  }
}

static gboolean
source_queue_flush_cb(gpointer user_data)
{
  SourceFlushData *data = user_data;
  GnostrTimelineSource *self = data->source;
  SourceKeyQueue *queue = &self->queues[data->stream];
  queue->flush_source_id = 0;

  if (self->disposed || data->generation != self->generation ||
      queue->generation != data->generation || queue->active)
    return G_SOURCE_REMOVE;

  guint n_keys = 0;
  guint64 serial = 0;
  g_autofree uint64_t *keys = source_key_queue_take(queue, &n_keys, &serial);
  if (keys && n_keys > 0)
    source_start_key_batch(self, source_stream_batch_kind(data->stream),
                           data->stream, serial, keys, n_keys);
  return G_SOURCE_REMOVE;
}

static void
source_queue_schedule(GnostrTimelineSource *self,
                      SourceStream stream)
{
  SourceKeyQueue *queue = &self->queues[stream];
  if (self->disposed || queue->active || queue->pending->len == 0 ||
      queue->flush_source_id != 0)
    return;

  SourceFlushData *data = g_new0(SourceFlushData, 1);
  data->source = g_object_ref(self);
  data->stream = stream;
  data->generation = self->generation;
  queue->flush_source_id =
    g_timeout_add_full(G_PRIORITY_DEFAULT_IDLE, SOURCE_KEY_COALESCE_MS,
                       source_queue_flush_cb, data,
                       (GDestroyNotify)source_flush_data_free);
}

static void
source_queue_keys(GnostrTimelineSource *self,
                  SourceStream stream,
                  const uint64_t *note_keys,
                  guint n_keys)
{
  if (!GNOSTR_IS_TIMELINE_SOURCE(self) || self->disposed)
    return;
  source_key_queue_enqueue(&self->queues[stream], self->generation,
                           note_keys, n_keys);
  source_queue_schedule(self, stream);
}

static void
source_add_authorized_delete_targets_from_json(GnostrTimelineBatch *batch,
                                               void *txn,
                                               const char *delete_event_json)
{
  if (!batch || !txn || !delete_event_json)
    return;

  NostrEvent *evt = nostr_event_new();
  if (!evt)
    return;

  if (nostr_event_deserialize(evt, delete_event_json) != 0) {
    nostr_event_free(evt);
    return;
  }

  char *delete_event_id_tmp = nostr_event_get_id(evt);
  g_autofree char *delete_event_id = delete_event_id_tmp ? g_strdup(delete_event_id_tmp) : NULL;
  free(delete_event_id_tmp);

  const char *deletion_pubkey = nostr_event_get_pubkey(evt);
  uint8_t deletion_pk32[32];
  gboolean have_deletion_pk = hex_to_bytes32(deletion_pubkey, deletion_pk32);
  if (!have_deletion_pk) {
    nostr_event_free(evt);
    return;
  }

  NostrTags *tags = nostr_event_get_tags(evt);
  if (!tags) {
    nostr_event_free(evt);
    return;
  }

  for (size_t i = 0; i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (!tag || nostr_tag_size(tag) < 2)
      continue;

    const char *tag_name = nostr_tag_get(tag, 0);
    const char *target_id = nostr_tag_get(tag, 1);
    if (g_strcmp0(tag_name, "e") != 0 || !target_id)
      continue;

    uint8_t target_id32[32];
    if (!hex_to_bytes32(target_id, target_id32))
      continue;

    storage_ndb_note *target_note = NULL;
    uint64_t target_key = storage_ndb_get_note_key_by_id(txn, target_id32, &target_note);
    if (target_key == 0 || !target_note)
      continue;

    const unsigned char *target_pk32 = storage_ndb_note_pubkey(target_note);
    if (!target_pk32 || memcmp(deletion_pk32, target_pk32, 32) != 0) {
      g_debug("[SOURCE] Ignoring unauthorized delete target %s from delete %s",
              target_id,
              delete_event_id ? delete_event_id : "unknown");
      continue;
    }

    GnostrTimelineDeleteTarget target = {
      .target_event_id = (char *)target_id,
      .delete_event_id = delete_event_id,
    };
    gnostr_timeline_batch_add_delete_target(batch, &target);
  }

  nostr_event_free(evt);
}

static void
source_populate_delete_batch(GnostrTimelineBatch *batch,
                             const uint64_t *note_keys,
                             guint n_keys,
                             GCancellable *cancellable)
{
  if (!batch || !note_keys || n_keys == 0)
    return;

  void *txn = NULL;
  gboolean have_txn = (storage_ndb_begin_query(&txn, NULL) == 0 && txn != NULL);
  if (have_txn) {
    for (guint i = 0; i < n_keys; i++) {
      if (cancellable && g_cancellable_is_cancelled(cancellable))
        break;
      storage_ndb_note *note = storage_ndb_get_note_ptr(txn, note_keys[i]);
      if (!note || storage_ndb_note_kind(note) != 5)
        continue;

      const unsigned char *delete_id32 = storage_ndb_note_id(note);
      if (!delete_id32)
        continue;

      const unsigned char *delete_pk32 = storage_ndb_note_pubkey(note);
      char delete_pubkey_hex[65] = {0};
      if (delete_pk32)
        storage_ndb_hex_encode(delete_pk32, delete_pubkey_hex);

      /* Preserve the original delete-event entry payload for legacy source
       * consumers while also projecting resolved NIP-09 target ids for the
       * compositor path. */
      gnostr_timeline_batch_add_note(batch,
                                     note_keys[i],
                                     (gint64)storage_ndb_note_created_at(note),
                                     delete_id32,
                                     delete_pk32 ? delete_pubkey_hex : NULL,
                                     NULL,
                                     NULL,
                                     NULL,
                                     NULL,
                                     NULL,
                                     NULL,
                                     NULL,
                                     5,
                                     TRUE);

      char delete_id[65];
      storage_ndb_hex_encode(delete_id32, delete_id);
      g_autofree char *filter = g_strdup_printf("{\"ids\":[\"%s\"]}", delete_id);

      char **json_results = NULL;
      int result_count = 0;
      int rc = storage_ndb_query(txn, filter, &json_results, &result_count, NULL);
      if (rc == 0 && json_results && result_count > 0) {
        for (int j = 0; j < result_count; j++)
          source_add_authorized_delete_targets_from_json(batch, txn, json_results[j]);
        storage_ndb_free_results(json_results, result_count);
      }
    }
    storage_ndb_end_query(txn);
  }

}

enum {
  SOURCE_METRIC_LIKE   = 1u << 0,
  SOURCE_METRIC_REPOST = 1u << 1,
  SOURCE_METRIC_REPLY  = 1u << 2,
  SOURCE_METRIC_ZAP    = 1u << 3,
};

static void
source_add_target_metric(GHashTable *targets,
                         const char *event_id,
                         guint metric)
{
  if (!event_id || strlen(event_id) != 64)
    return;
  guint mask = GPOINTER_TO_UINT(g_hash_table_lookup(targets, event_id));
  g_hash_table_replace(targets, g_strdup(event_id), GUINT_TO_POINTER(mask | metric));
}

static void
source_populate_patch_batch(GnostrTimelineBatch *batch,
                            const uint64_t *note_keys,
                            guint n_keys,
                            GCancellable *cancellable)
{
  if (!batch || !note_keys || n_keys == 0)
    return;

  GnostrTimelineBatchKind batch_kind = gnostr_timeline_batch_get_kind(batch);
  g_autoptr(GHashTable) targets =
    g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  g_autoptr(GHashTable) profile_pubkeys =
    g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  void *txn = NULL;
  if (storage_ndb_begin_query(&txn, NULL) != 0 || !txn)
    return;

  for (guint i = 0; i < n_keys; i++) {
    if (cancellable && g_cancellable_is_cancelled(cancellable))
      break;

    storage_ndb_note *note = storage_ndb_get_note_ptr(txn, note_keys[i]);
    if (!note)
      continue;

    gint note_kind = (gint)storage_ndb_note_kind(note);
    const unsigned char *pk32 = storage_ndb_note_pubkey(note);
    char pubkey_hex[65] = {0};
    if (pk32)
      storage_ndb_hex_encode(pk32, pubkey_hex);

    if (batch_kind == GNOSTR_TIMELINE_BATCH_PROFILE_PATCH && pk32 &&
        !g_hash_table_contains(profile_pubkeys, pubkey_hex)) {
      g_hash_table_add(profile_pubkeys, g_strdup(pubkey_hex));
      StorageNdbProfileMeta profile = {0};
      storage_ndb_get_profile_meta_direct(txn, pk32, &profile, NULL);
      GnostrTimelineProfilePatch patch = {
        .pubkey_hex = pubkey_hex,
        .display_name = profile.display_name,
        .handle = profile.name,
        .avatar_url = profile.picture,
        .nip05 = profile.nip05,
      };
      gnostr_timeline_batch_add_profile_patch(batch, &patch);
      storage_ndb_profile_meta_clear(&profile);
    } else if (batch_kind == GNOSTR_TIMELINE_BATCH_METADATA_PATCH) {
      if (note_kind == 1) {
        char *root_id = NULL;
        char *reply_id = NULL;
        storage_ndb_note_get_nip10_thread(note, &root_id, &reply_id);
        source_add_target_metric(targets,
                                 reply_id ? reply_id : root_id,
                                 SOURCE_METRIC_REPLY);
        g_free(root_id);
        g_free(reply_id);
      } else if (note_kind == 6) {
        g_autofree char *target_id = storage_ndb_note_get_first_etag(note);
        source_add_target_metric(targets, target_id, SOURCE_METRIC_REPOST);
      } else {
        g_autofree char *target_id = storage_ndb_note_get_last_etag(note);
        if (note_kind == 7)
          source_add_target_metric(targets, target_id, SOURCE_METRIC_LIKE);
        else if (note_kind == 9735)
          source_add_target_metric(targets, target_id, SOURCE_METRIC_ZAP);
      }
    }

    /* Profile notifications are still consumed by the legacy adapter via
     * their note keys. Metadata consumers use the structured patches below;
     * retaining interaction entries would duplicate the global kind-1 path. */
    if (batch_kind == GNOSTR_TIMELINE_BATCH_PROFILE_PATCH) {
      gnostr_timeline_batch_add_note(batch,
                                     note_keys[i],
                                     (gint64)storage_ndb_note_created_at(note),
                                     storage_ndb_note_id(note),
                                     pk32 ? pubkey_hex : NULL,
                                     NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                                     note_kind,
                                     TRUE);
    }
  }

  if (batch_kind == GNOSTR_TIMELINE_BATCH_METADATA_PATCH &&
      g_hash_table_size(targets) > 0 &&
      !(cancellable && g_cancellable_is_cancelled(cancellable))) {
    g_autoptr(GPtrArray) zap_ids = g_ptr_array_new_with_free_func(g_free);
    GHashTableIter collect_iter;
    gpointer target_key = NULL;
    gpointer target_value = NULL;
    g_hash_table_iter_init(&collect_iter, targets);
    while (g_hash_table_iter_next(&collect_iter, &target_key, &target_value)) {
      if (GPOINTER_TO_UINT(target_value) & SOURCE_METRIC_ZAP)
        g_ptr_array_add(zap_ids, g_strdup(target_key));
    }
    g_ptr_array_add(zap_ids, NULL);

    g_autoptr(GHashTable) zap_stats = NULL;
    if (zap_ids->len > 1) {
      zap_stats = storage_ndb_get_zap_stats_batch(
        (const char * const *)zap_ids->pdata, zap_ids->len - 1);
    }

    GHashTableIter iter;
    g_hash_table_iter_init(&iter, targets);
    while (g_hash_table_iter_next(&iter, &target_key, &target_value)) {
      const char *target_id = target_key;
      guint mask = GPOINTER_TO_UINT(target_value);
      StorageNdbNoteCounts counts = {0};
      if (mask & (SOURCE_METRIC_LIKE | SOURCE_METRIC_REPOST | SOURCE_METRIC_REPLY))
        storage_ndb_read_note_counts_hex(txn, target_id, &counts);

      GnostrTimelineMetadataPatch patch = {
        .event_id = (char *)target_id,
        .has_like_count = (mask & SOURCE_METRIC_LIKE) != 0,
        .like_count = counts.total_reactions,
        .has_repost_count = (mask & SOURCE_METRIC_REPOST) != 0,
        .repost_count = counts.reposts,
        .has_reply_count = (mask & SOURCE_METRIC_REPLY) != 0,
        .reply_count = counts.direct_replies,
        .has_zap_count = (mask & SOURCE_METRIC_ZAP) != 0,
        .has_zap_total_msat = (mask & SOURCE_METRIC_ZAP) != 0,
      };
      StorageNdbZapStats *zs = zap_stats ?
        g_hash_table_lookup(zap_stats, target_id) : NULL;
      if (zs) {
        patch.zap_count = zs->zap_count;
        patch.zap_total_msat = zs->total_msat;
      }
      gnostr_timeline_batch_add_metadata_patch(batch, &patch);
    }
  }

  storage_ndb_end_query(txn);
}

static void
on_sub_timeline_batch(uint64_t subid,
                      const uint64_t *note_keys,
                      guint n_keys,
                      gpointer user_data)
{
  GnostrTimelineSource *self = GNOSTR_TIMELINE_SOURCE(user_data);
  if (subid != self->sub_timeline)
    return;
  source_queue_keys(self,
                    SOURCE_STREAM_TIMELINE, note_keys, n_keys);
}

static void
on_sub_profiles_batch(uint64_t subid,
                      const uint64_t *note_keys,
                      guint n_keys,
                      gpointer user_data)
{
  GnostrTimelineSource *self = GNOSTR_TIMELINE_SOURCE(user_data);
  if (subid != self->sub_profiles)
    return;
  source_queue_keys(self,
                    SOURCE_STREAM_PROFILE, note_keys, n_keys);
}

static void
on_sub_deletes_batch(uint64_t subid,
                     const uint64_t *note_keys,
                     guint n_keys,
                     gpointer user_data)
{
  GnostrTimelineSource *self = GNOSTR_TIMELINE_SOURCE(user_data);
  if (subid != self->sub_deletes)
    return;
  source_queue_keys(self,
                    SOURCE_STREAM_DELETE, note_keys, n_keys);
}

static void
on_sub_metadata_batch(uint64_t subid,
                      const uint64_t *note_keys,
                      guint n_keys,
                      gpointer user_data)
{
  GnostrTimelineSource *self = GNOSTR_TIMELINE_SOURCE(user_data);
  if (subid != self->sub_metadata)
    return;
  source_queue_keys(self,
                    SOURCE_STREAM_METADATA, note_keys, n_keys);
}

static char *
source_dup_timeline_filter(GnostrTimelineSource *self)
{
  GNostrTimelineQuery *effective = source_query_copy_or_default(self);
  const char *json = effective ? gnostr_timeline_query_to_json(effective) : NULL;
  char *result = g_strdup(json ? json : "{\"kinds\":[1,6]}");
  gnostr_timeline_query_free(effective);
  return result;
}

static void
source_rebuild_timeline_subscription(GnostrTimelineSource *self)
{
  if (self->sub_timeline > 0) {
    gn_ndb_unsubscribe(self->sub_timeline);
    self->sub_timeline = 0;
  }

  g_free(self->timeline_filter_json);
  self->timeline_filter_json = source_dup_timeline_filter(self);
  self->sub_timeline = gn_ndb_subscribe(self->timeline_filter_json,
                                         on_sub_timeline_batch,
                                         self,
                                         NULL);
  if (self->sub_timeline == 0)
    g_warning("[SOURCE] Unable to subscribe active timeline filter: %s",
              self->timeline_filter_json);
}

static void
gnostr_timeline_source_dispose(GObject *object)
{
  GnostrTimelineSource *self = GNOSTR_TIMELINE_SOURCE(object);

  if (!self->disposed) {
    self->disposed = TRUE;
    if (self->generation_cancellable)
      g_cancellable_cancel(self->generation_cancellable);
    for (guint i = 0; i < SOURCE_STREAM_COUNT; i++)
      source_key_queue_clear(&self->queues[i]);
    if (self->sub_timeline > 0) { gn_ndb_unsubscribe(self->sub_timeline); self->sub_timeline = 0; }
    if (self->sub_profiles > 0) { gn_ndb_unsubscribe(self->sub_profiles); self->sub_profiles = 0; }
    if (self->sub_deletes > 0)  { gn_ndb_unsubscribe(self->sub_deletes); self->sub_deletes = 0; }
    if (self->sub_metadata > 0) { gn_ndb_unsubscribe(self->sub_metadata); self->sub_metadata = 0; }
    g_clear_object(&self->generation_cancellable);
    g_clear_pointer(&self->completed_requests, g_hash_table_unref);
  }

  G_OBJECT_CLASS(gnostr_timeline_source_parent_class)->dispose(object);
}

static void
gnostr_timeline_source_finalize(GObject *object)
{
  GnostrTimelineSource *self = GNOSTR_TIMELINE_SOURCE(object);

  g_clear_pointer(&self->query, gnostr_timeline_query_free);
  g_clear_pointer(&self->timeline_filter_json, g_free);

  G_OBJECT_CLASS(gnostr_timeline_source_parent_class)->finalize(object);
}

static void
gnostr_timeline_source_class_init(GnostrTimelineSourceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = gnostr_timeline_source_dispose;
  object_class->finalize = gnostr_timeline_source_finalize;

  signals[SIGNAL_BATCH] =
    g_signal_new("batch",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0,
                 NULL, NULL,
                 g_cclosure_marshal_VOID__OBJECT,
                 G_TYPE_NONE,
                 1,
                 GNOSTR_TYPE_TIMELINE_BATCH);
}

static void
gnostr_timeline_source_init(GnostrTimelineSource *self)
{
  self->generation = 1;
  self->generation_cancellable = g_cancellable_new();
  self->completed_requests =
    g_hash_table_new_full(source_u64_hash, source_u64_equal, g_free,
                          (GDestroyNotify)source_completion_free);
  self->next_request_sequence = 1;
  self->next_emit_sequence = 1;
  for (guint i = 0; i < SOURCE_STREAM_COUNT; i++)
    source_key_queue_init(&self->queues[i], self->generation);

  self->sub_profiles = gn_ndb_subscribe(FILTER_PROFILES, on_sub_profiles_batch, self, NULL);
  self->sub_deletes = gn_ndb_subscribe(FILTER_DELETES, on_sub_deletes_batch, self, NULL);
  self->sub_metadata = gn_ndb_subscribe(FILTER_INTERACTIONS, on_sub_metadata_batch, self, NULL);
  source_rebuild_timeline_subscription(self);
}

GnostrTimelineSource *
gnostr_timeline_source_new(void)
{
  return g_object_new(GNOSTR_TYPE_TIMELINE_SOURCE, NULL);
}

GnostrTimelineSource *
gnostr_timeline_source_new_with_query(GNostrTimelineQuery *query)
{
  GnostrTimelineSource *self = gnostr_timeline_source_new();
  if (query)
    gnostr_timeline_source_set_query(self, query);
  return self;
}

void
gnostr_timeline_source_set_query(GnostrTimelineSource *self,
                                 GNostrTimelineQuery *query)
{
  g_return_if_fail(GNOSTR_IS_TIMELINE_SOURCE(self));

  if (self->generation_cancellable)
    g_cancellable_cancel(self->generation_cancellable);

  self->generation++;
  if (self->generation == 0)
    self->generation = 1;

  for (guint i = 0; i < SOURCE_STREAM_COUNT; i++)
    source_key_queue_reset(&self->queues[i], self->generation);
  g_hash_table_remove_all(self->completed_requests);
  self->next_request_sequence = 1;
  self->next_emit_sequence = 1;

  g_clear_pointer(&self->query, gnostr_timeline_query_free);
  if (query)
    self->query = gnostr_timeline_query_copy(query);

  g_clear_object(&self->generation_cancellable);
  self->generation_cancellable = g_cancellable_new();
  source_rebuild_timeline_subscription(self);

  g_debug("[SOURCE] Query set; generation=%" G_GUINT64_FORMAT " filter=%s",
          self->generation,
          self->timeline_filter_json);
}

GNostrTimelineQuery *
gnostr_timeline_source_get_query(GnostrTimelineSource *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SOURCE(self), NULL);
  return self->query;
}

guint64
gnostr_timeline_source_get_generation(GnostrTimelineSource *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SOURCE(self), 0);
  return self->generation;
}

void
gnostr_timeline_source_refresh_async(GnostrTimelineSource *self)
{
  g_return_if_fail(GNOSTR_IS_TIMELINE_SOURCE(self));

  SourceBatchRequest *req = g_new0(SourceBatchRequest, 1);
  req->kind = GNOSTR_TIMELINE_BATCH_REFRESH;
  req->generation = self->generation;
  req->query = source_query_copy_or_default(self);
  if (req->query && req->query->limit == 0)
    req->query->limit = DEFAULT_QUERY_LIMIT;
  req->requested_count = req->query ? req->query->limit : DEFAULT_QUERY_LIMIT;

  source_run_request(self, req);
}

void
gnostr_timeline_source_load_older_async(GnostrTimelineSource *self,
                                        guint count,
                                        gint64 before_timestamp)
{
  g_return_if_fail(GNOSTR_IS_TIMELINE_SOURCE(self));
  if (count == 0)
    return;

  SourceBatchRequest *req = g_new0(SourceBatchRequest, 1);
  req->kind = GNOSTR_TIMELINE_BATCH_PAGE_OLDER;
  req->generation = self->generation;
  req->query = source_query_copy_or_default(self);
  req->query->since = 0;
  req->query->until = before_timestamp > 0 ? before_timestamp - 1 : 0;
  req->query->limit = count;
  req->requested_count = count;

  source_run_request(self, req);
}

void
gnostr_timeline_source_load_newer_async(GnostrTimelineSource *self,
                                        guint count,
                                        gint64 after_timestamp)
{
  g_return_if_fail(GNOSTR_IS_TIMELINE_SOURCE(self));
  if (count == 0)
    return;

  SourceBatchRequest *req = g_new0(SourceBatchRequest, 1);
  req->kind = GNOSTR_TIMELINE_BATCH_PAGE_NEWER;
  req->generation = self->generation;
  req->query = source_query_copy_or_default(self);
  req->query->since = after_timestamp > 0 ? after_timestamp + 1 : 0;
  req->query->until = 0;
  /* The controller already includes its retained-window buffers in count;
   * do not inflate a 30-row publication into a 100-note DB query. */
  req->query->limit = count;
  req->requested_count = count;

  source_run_request(self, req);
}

#ifdef GNOSTR_TIMELINE_SOURCE_TESTING
char *
gnostr_timeline_source_testing_dup_filter(GnostrTimelineSource *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SOURCE(self), NULL);
  return g_strdup(self->timeline_filter_json);
}

void
gnostr_timeline_source_testing_enqueue_live_keys(GnostrTimelineSource *self,
                                                  const uint64_t *note_keys,
                                                  guint n_keys)
{
  g_return_if_fail(GNOSTR_IS_TIMELINE_SOURCE(self));
  source_key_queue_enqueue(&self->queues[SOURCE_STREAM_TIMELINE],
                           self->generation, note_keys, n_keys);
}

guint
gnostr_timeline_source_testing_get_live_pending_count(GnostrTimelineSource *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SOURCE(self), 0);
  return self->queues[SOURCE_STREAM_TIMELINE].pending->len;
}

guint
gnostr_timeline_source_testing_get_live_dropped_count(GnostrTimelineSource *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SOURCE(self), 0);
  return self->queues[SOURCE_STREAM_TIMELINE].dropped;
}

guint
gnostr_timeline_source_testing_get_queue_capacity(void)
{
  return SOURCE_KEY_QUEUE_CAP;
}
#endif
