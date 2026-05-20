#define G_LOG_DOMAIN "gnostr-timeline-source"

#include "gnostr-timeline-source.h"

#include <nostr-gobject-1.0/gn-ndb-sub-dispatcher.h>
#include <nostr-gobject-1.0/gnostr-mute-list.h>
#include <nostr-gobject-1.0/storage_ndb.h>
#include <nostr.h>
#include <string.h>

#include "../util/mute_filter.h"

#define FILTER_TIMELINE   "{\"kinds\":[1,6,9735]}"
#define FILTER_PROFILES   "{\"kinds\":[0]}"
#define FILTER_DELETES    "{\"kinds\":[5]}"
#define FILTER_REACTIONS  "{\"kinds\":[7]}"
#define FILTER_ZAPS       "{\"kinds\":[9735]}"
#define DEFAULT_QUERY_LIMIT 50

struct _GnostrTimelineSource {
  GObject parent_instance;

  GNostrTimelineQuery *query;
  guint64 generation;

  uint64_t sub_timeline;
  uint64_t sub_profiles;
  uint64_t sub_deletes;
  uint64_t sub_reactions;
  uint64_t sub_zaps;
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
} SourceBatchRequest;

static gboolean hex_to_bytes32(const char *hex, uint8_t out[32]);
static void on_sub_timeline_batch(uint64_t subid, const uint64_t *note_keys, guint n_keys, gpointer user_data);
static void on_sub_profiles_batch(uint64_t subid, const uint64_t *note_keys, guint n_keys, gpointer user_data);
static void on_sub_deletes_batch(uint64_t subid, const uint64_t *note_keys, guint n_keys, gpointer user_data);
static void on_sub_reactions_batch(uint64_t subid, const uint64_t *note_keys, guint n_keys, gpointer user_data);
static void on_sub_zaps_batch(uint64_t subid, const uint64_t *note_keys, guint n_keys, gpointer user_data);

static void
source_batch_request_free(SourceBatchRequest *req)
{
  if (!req)
    return;
  g_clear_pointer(&req->query, gnostr_timeline_query_free);
  g_free(req->note_keys);
  g_free(req);
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
db_has_profile_event_for_pubkey(void *txn, const unsigned char pk32[32])
{
  if (!txn || !pk32)
    return FALSE;

  char *evt_json = NULL;
  int evt_len = 0;
  int rc = storage_ndb_get_profile_by_pubkey(txn, pk32, &evt_json, &evt_len, NULL);
  if (rc != 0 || !evt_json || evt_len <= 0) {
    if (evt_json)
      free(evt_json);
    return FALSE;
  }

  free(evt_json);
  return TRUE;
}

static gboolean
add_note_key_to_batch_from_txn(GnostrTimelineBatch *batch,
                               GNostrTimelineQuery *query,
                               void *txn,
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

  gboolean has_profile = db_has_profile_event_for_pubkey(txn, pk32);
  if (!has_profile)
    gnostr_timeline_batch_add_profile_request(batch, pubkey_hex);

  gnostr_timeline_batch_add_note(batch,
                                 note_key,
                                 created_at,
                                 storage_ndb_note_id(note),
                                 pubkey_hex,
                                 root_id,
                                 reply_id,
                                 kind,
                                 has_profile);

  g_free(root_id);
  g_free(reply_id);
  return TRUE;
}

static void
query_results_into_batch(GnostrTimelineBatch *batch,
                         GNostrTimelineQuery *query,
                         guint requested_count)
{
  const char *query_json = gnostr_timeline_query_to_json(query);
  if (!query_json)
    return;

  void *txn = NULL;
  if (storage_ndb_begin_query(&txn, NULL) != 0 || !txn) {
    g_warning("[SOURCE] query begin failed");
    return;
  }

  char **json_results = NULL;
  int result_count = 0;
  int rc = storage_ndb_query(txn, query_json, &json_results, &result_count, NULL);

  guint added = 0;
  if (rc == 0 && json_results && result_count > 0) {
    for (int i = 0; i < result_count; i++) {
      const char *event_json = json_results[i];
      if (!event_json)
        continue;

      NostrEvent *evt = nostr_event_new();
      if (!evt)
        continue;

      if (nostr_event_deserialize(evt, event_json) == 0) {
        char *event_id_tmp = nostr_event_get_id(evt);
        g_autofree gchar *event_id = event_id_tmp ? g_strdup(event_id_tmp) : NULL;
        free(event_id_tmp);

        uint8_t id32[32];
        if (event_id && hex_to_bytes32(event_id, id32)) {
          uint64_t note_key = storage_ndb_get_note_key_by_id(txn, id32, NULL);
          if (note_key != 0 && add_note_key_to_batch_from_txn(batch, query, txn, note_key, TRUE)) {
            added++;
            if (requested_count > 0 && added >= requested_count) {
              nostr_event_free(evt);
              break;
            }
          }
        }
      }

      nostr_event_free(evt);
    }

    storage_ndb_free_results(json_results, result_count);
  }

  storage_ndb_end_query(txn);
}

static void
source_batch_thread_func(GTask *task,
                         gpointer source_object G_GNUC_UNUSED,
                         gpointer task_data,
                         GCancellable *cancellable G_GNUC_UNUSED)
{
  SourceBatchRequest *req = task_data;
  GnostrTimelineBatch *batch = gnostr_timeline_batch_new(req->kind, req->generation);

  if (req->note_keys && req->n_keys > 0) {
    void *txn = NULL;
    if (storage_ndb_begin_query(&txn, NULL) == 0 && txn) {
      for (guint i = 0; i < req->n_keys; i++)
        add_note_key_to_batch_from_txn(batch, req->query, txn, req->note_keys[i], TRUE);
      storage_ndb_end_query(txn);
    }
  } else {
    query_results_into_batch(batch, req->query, req->requested_count);
  }

  g_task_return_pointer(task, batch, g_object_unref);
}

static void
source_batch_done_cb(GObject *source_object,
                     GAsyncResult *res,
                     gpointer user_data G_GNUC_UNUSED)
{
  GnostrTimelineSource *self = GNOSTR_TIMELINE_SOURCE(source_object);
  if (!GNOSTR_IS_TIMELINE_SOURCE(self))
    return;

  GnostrTimelineBatch *batch = g_task_propagate_pointer(G_TASK(res), NULL);
  if (!batch)
    return;

  if (gnostr_timeline_batch_get_generation(batch) == self->generation) {
    g_signal_emit(self, signals[SIGNAL_BATCH], 0, batch);
  } else {
    g_debug("[SOURCE] Dropping stale %s batch gen=%" G_GUINT64_FORMAT " current=%" G_GUINT64_FORMAT,
            gnostr_timeline_batch_kind_to_string(gnostr_timeline_batch_get_kind(batch)),
            gnostr_timeline_batch_get_generation(batch),
            self->generation);
  }

  g_object_unref(batch);
}

static void
source_start_key_batch(GnostrTimelineSource *self,
                       GnostrTimelineBatchKind kind,
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

  GTask *task = g_task_new(self, NULL, source_batch_done_cb, NULL);
  g_task_set_task_data(task, req, (GDestroyNotify)source_batch_request_free);
  g_task_run_in_thread(task, source_batch_thread_func);
  g_object_unref(task);
}

static void
source_emit_note_key_patch_batch(GnostrTimelineSource *self,
                                 GnostrTimelineBatchKind kind,
                                 const uint64_t *note_keys,
                                 guint n_keys)
{
  if (!note_keys || n_keys == 0)
    return;

  GnostrTimelineBatch *batch = gnostr_timeline_batch_new(kind, self->generation);

  void *txn = NULL;
  gboolean have_txn = (storage_ndb_begin_query(&txn, NULL) == 0 && txn != NULL);
  if (have_txn) {
    for (guint i = 0; i < n_keys; i++) {
      storage_ndb_note *note = storage_ndb_get_note_ptr(txn, note_keys[i]);
      if (!note)
        continue;

      const unsigned char *pk32 = storage_ndb_note_pubkey(note);
      char pubkey_hex[65] = {0};
      if (pk32)
        storage_ndb_hex_encode(pk32, pubkey_hex);

      gnostr_timeline_batch_add_note(batch,
                                     note_keys[i],
                                     (gint64)storage_ndb_note_created_at(note),
                                     storage_ndb_note_id(note),
                                     pk32 ? pubkey_hex : NULL,
                                     NULL,
                                     NULL,
                                     (gint)storage_ndb_note_kind(note),
                                     TRUE);
    }
    storage_ndb_end_query(txn);
  }

  if (gnostr_timeline_batch_get_n_entries(batch) > 0)
    g_signal_emit(self, signals[SIGNAL_BATCH], 0, batch);

  g_object_unref(batch);
}

static void
on_sub_timeline_batch(uint64_t subid G_GNUC_UNUSED,
                      const uint64_t *note_keys,
                      guint n_keys,
                      gpointer user_data)
{
  GnostrTimelineSource *self = GNOSTR_TIMELINE_SOURCE(user_data);
  if (!GNOSTR_IS_TIMELINE_SOURCE(self))
    return;

  source_start_key_batch(self, GNOSTR_TIMELINE_BATCH_LIVE_HEAD, note_keys, n_keys);
}

static void
on_sub_profiles_batch(uint64_t subid G_GNUC_UNUSED,
                      const uint64_t *note_keys,
                      guint n_keys,
                      gpointer user_data)
{
  GnostrTimelineSource *self = GNOSTR_TIMELINE_SOURCE(user_data);
  if (!GNOSTR_IS_TIMELINE_SOURCE(self))
    return;

  source_emit_note_key_patch_batch(self, GNOSTR_TIMELINE_BATCH_PROFILE_PATCH, note_keys, n_keys);
}

static void
on_sub_deletes_batch(uint64_t subid G_GNUC_UNUSED,
                     const uint64_t *note_keys,
                     guint n_keys,
                     gpointer user_data)
{
  GnostrTimelineSource *self = GNOSTR_TIMELINE_SOURCE(user_data);
  if (!GNOSTR_IS_TIMELINE_SOURCE(self))
    return;

  source_emit_note_key_patch_batch(self, GNOSTR_TIMELINE_BATCH_DELETE, note_keys, n_keys);
}

static void
on_sub_reactions_batch(uint64_t subid G_GNUC_UNUSED,
                       const uint64_t *note_keys,
                       guint n_keys,
                       gpointer user_data)
{
  GnostrTimelineSource *self = GNOSTR_TIMELINE_SOURCE(user_data);
  if (!GNOSTR_IS_TIMELINE_SOURCE(self))
    return;

  source_emit_note_key_patch_batch(self, GNOSTR_TIMELINE_BATCH_METADATA_PATCH, note_keys, n_keys);
}

static void
on_sub_zaps_batch(uint64_t subid G_GNUC_UNUSED,
                  const uint64_t *note_keys,
                  guint n_keys,
                  gpointer user_data)
{
  GnostrTimelineSource *self = GNOSTR_TIMELINE_SOURCE(user_data);
  if (!GNOSTR_IS_TIMELINE_SOURCE(self))
    return;

  source_emit_note_key_patch_batch(self, GNOSTR_TIMELINE_BATCH_METADATA_PATCH, note_keys, n_keys);
}

static void
gnostr_timeline_source_dispose(GObject *object)
{
  GnostrTimelineSource *self = GNOSTR_TIMELINE_SOURCE(object);

  if (!self->disposed) {
    self->disposed = TRUE;
    if (self->sub_timeline > 0)  { gn_ndb_unsubscribe(self->sub_timeline);  self->sub_timeline = 0; }
    if (self->sub_profiles > 0)  { gn_ndb_unsubscribe(self->sub_profiles);  self->sub_profiles = 0; }
    if (self->sub_deletes > 0)   { gn_ndb_unsubscribe(self->sub_deletes);   self->sub_deletes = 0; }
    if (self->sub_reactions > 0) { gn_ndb_unsubscribe(self->sub_reactions); self->sub_reactions = 0; }
    if (self->sub_zaps > 0)      { gn_ndb_unsubscribe(self->sub_zaps);      self->sub_zaps = 0; }
  }

  G_OBJECT_CLASS(gnostr_timeline_source_parent_class)->dispose(object);
}

static void
gnostr_timeline_source_finalize(GObject *object)
{
  GnostrTimelineSource *self = GNOSTR_TIMELINE_SOURCE(object);

  g_clear_pointer(&self->query, gnostr_timeline_query_free);

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

  self->sub_profiles  = gn_ndb_subscribe(FILTER_PROFILES,  on_sub_profiles_batch,  self, NULL);
  self->sub_timeline  = gn_ndb_subscribe(FILTER_TIMELINE,  on_sub_timeline_batch,  self, NULL);
  self->sub_deletes   = gn_ndb_subscribe(FILTER_DELETES,   on_sub_deletes_batch,   self, NULL);
  self->sub_reactions = gn_ndb_subscribe(FILTER_REACTIONS, on_sub_reactions_batch, self, NULL);
  self->sub_zaps      = gn_ndb_subscribe(FILTER_ZAPS,      on_sub_zaps_batch,      self, NULL);
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

  g_clear_pointer(&self->query, gnostr_timeline_query_free);
  if (query)
    self->query = gnostr_timeline_query_copy(query);

  self->generation++;
  if (self->generation == 0)
    self->generation = 1;

  g_debug("[SOURCE] Query set; generation=%" G_GUINT64_FORMAT, self->generation);
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

  GTask *task = g_task_new(self, NULL, source_batch_done_cb, NULL);
  g_task_set_task_data(task, req, (GDestroyNotify)source_batch_request_free);
  g_task_run_in_thread(task, source_batch_thread_func);
  g_object_unref(task);
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

  GTask *task = g_task_new(self, NULL, source_batch_done_cb, NULL);
  g_task_set_task_data(task, req, (GDestroyNotify)source_batch_request_free);
  g_task_run_in_thread(task, source_batch_thread_func);
  g_object_unref(task);
}

void
gnostr_timeline_source_load_newer_async(GnostrTimelineSource *self,
                                        guint count,
                                        gint64 after_timestamp)
{
  g_return_if_fail(GNOSTR_IS_TIMELINE_SOURCE(self));
  if (count == 0)
    return;

  guint query_limit = count * 4;
  if (query_limit < 100)
    query_limit = 100;

  SourceBatchRequest *req = g_new0(SourceBatchRequest, 1);
  req->kind = GNOSTR_TIMELINE_BATCH_PAGE_NEWER;
  req->generation = self->generation;
  req->query = source_query_copy_or_default(self);
  req->query->since = after_timestamp > 0 ? after_timestamp + 1 : 0;
  req->query->until = 0;
  req->query->limit = query_limit;
  req->requested_count = count;

  GTask *task = g_task_new(self, NULL, source_batch_done_cb, NULL);
  g_task_set_task_data(task, req, (GDestroyNotify)source_batch_request_free);
  g_task_run_in_thread(task, source_batch_thread_func);
  g_object_unref(task);
}
