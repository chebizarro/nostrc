#define G_LOG_DOMAIN "gnostr-timeline-feed-controller"

#include "gnostr-timeline-feed-controller.h"

#include <string.h>

#define DEFAULT_ROW_ESTIMATE_PX 160.0
#define DEFAULT_WIDTH_BUCKET    480u
#define COMPOSE_DEBOUNCE_MS     50u
#define AT_TOP_EPSILON_PX       1.0
#define LAYOUT_SIGNATURE        "timeline-v1"

typedef struct {
  char    *event_id;
  char    *note_key;
  char    *pubkey;
  char    *tie_breaker;
  char    *content;
  char    *display_name;
  char    *handle;
  char    *avatar_url;
  char    *nip05;
  guint    like_count;
  gboolean is_liked;
  guint    repost_count;
  guint    reply_count;
  guint    zap_count;
  gint64   zap_total_msat;
  guint64  note_key_u64;
  gint64   created_at;
  gint     kind;
  gboolean has_profile;
} WorkingEntry;

typedef struct {
  double measured_height;
  guint  width_bucket;
  char  *layout_signature;
  guint64 snapshot_generation;
} GeometryEntry;

struct _GnostrTimelineFeedController {
  GObject parent_instance;

  GnostrTimelineSource *source;
  gulong source_batch_handler;
  guint64 query_generation;
  guint64 snapshot_generation;

  GnostrTimelineSnapshotModel *model;

  GPtrArray *working;       /* element-type: WorkingEntry* */
  GHashTable *by_event_id;  /* char* -> WorkingEntry* (borrowed value) */
  GHashTable *pending_head; /* char* set of event ids hidden from visible snapshots */
  GHashTable *geometry;     /* char* geometry-token -> GeometryEntry* */

  gboolean user_at_top;
  double scroll_y;
  double viewport_height;
  guint width_bucket;

  guint compose_source_id;
  gboolean scheduled_preserve_anchor;
  gboolean scheduled_scroll_to_top;
  gboolean loading_older;
  gboolean loading_newer;
};

G_DEFINE_TYPE(GnostrTimelineFeedController,
              gnostr_timeline_feed_controller,
              G_TYPE_OBJECT)

enum {
  SIGNAL_PENDING_COUNT_CHANGED,
  SIGNAL_RESTORE_SCROLL,
  SIGNAL_SNAPSHOT_PUBLISHED,
  SIGNAL_NEED_PROFILE,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

static void schedule_compose(GnostrTimelineFeedController *self,
                             gboolean preserve_anchor,
                             gboolean scroll_to_top);
static void compose_and_publish(GnostrTimelineFeedController *self,
                                gboolean preserve_anchor,
                                gboolean scroll_to_top);

static void
bytes_to_hex32(const guint8 bytes[32],
               char out[65])
{
  static const char hexdigits[] = "0123456789abcdef";
  for (guint i = 0; i < 32; i++) {
    out[i * 2] = hexdigits[(bytes[i] >> 4) & 0x0f];
    out[i * 2 + 1] = hexdigits[bytes[i] & 0x0f];
  }
  out[64] = '\0';
}

static gboolean
bytes32_all_zero(const guint8 bytes[32])
{
  for (guint i = 0; i < 32; i++) {
    if (bytes[i] != 0)
      return FALSE;
  }
  return TRUE;
}

static WorkingEntry *
working_entry_new_from_batch_entry(const GnostrTimelineBatchEntry *entry)
{
  g_return_val_if_fail(entry != NULL, NULL);

  WorkingEntry *we = g_new0(WorkingEntry, 1);
  we->note_key_u64 = entry->note_key;
  we->created_at = entry->created_at;
  we->kind = entry->kind;
  we->has_profile = entry->has_profile;

  char event_id[65];
  if (!bytes32_all_zero(entry->event_id)) {
    bytes_to_hex32(entry->event_id, event_id);
    we->event_id = g_strdup(event_id);
  } else {
    /* The source should normally provide the event id.  Keep a deterministic
     * fallback so the hidden set can still dedupe/order test and bootstrap rows
     * rather than collapsing every zero-id row into a single snapshot item. */
    we->event_id = g_strdup_printf("note-key-%" G_GUINT64_FORMAT, entry->note_key);
  }

  we->note_key = g_strdup_printf("%" G_GUINT64_FORMAT, entry->note_key);
  we->pubkey = g_strdup(entry->pubkey_hex);
  we->tie_breaker = g_strdup(we->event_id);
  we->content = g_strdup("");

  return we;
}

static void
working_entry_free(WorkingEntry *we)
{
  if (!we)
    return;
  g_free(we->event_id);
  g_free(we->note_key);
  g_free(we->pubkey);
  g_free(we->tie_breaker);
  g_free(we->content);
  g_free(we->display_name);
  g_free(we->handle);
  g_free(we->avatar_url);
  g_free(we->nip05);
  g_free(we);
}

static void
working_entry_update_from_batch_entry(WorkingEntry *we,
                                      const GnostrTimelineBatchEntry *entry)
{
  if (!we || !entry)
    return;

  we->note_key_u64 = entry->note_key;
  we->created_at = entry->created_at;
  we->kind = entry->kind;
  we->has_profile = entry->has_profile;

  g_free(we->note_key);
  we->note_key = g_strdup_printf("%" G_GUINT64_FORMAT, entry->note_key);

  if (entry->pubkey_hex) {
    g_free(we->pubkey);
    we->pubkey = g_strdup(entry->pubkey_hex);
  }
}

static GeometryEntry *
geometry_entry_new(guint width_bucket,
                   const char *layout_signature,
                   double measured_height,
                   guint64 snapshot_generation)
{
  GeometryEntry *entry = g_new0(GeometryEntry, 1);
  entry->width_bucket = width_bucket;
  entry->layout_signature = g_strdup(layout_signature ? layout_signature : LAYOUT_SIGNATURE);
  entry->measured_height = measured_height;
  entry->snapshot_generation = snapshot_generation;
  return entry;
}

static void
geometry_entry_free(GeometryEntry *entry)
{
  if (!entry)
    return;
  g_free(entry->layout_signature);
  g_free(entry);
}

static guint
width_to_bucket(guint width_px)
{
  if (width_px == 0)
    return DEFAULT_WIDTH_BUCKET;

  /* Bucket by 80px increments so small resize jitter does not invalidate every
   * geometry entry, while material layout width changes still get distinct keys. */
  guint bucket = ((width_px + 39u) / 80u) * 80u;
  return MAX(bucket, 80u);
}

static char *
geometry_token_new(const char *event_id,
                   guint width_bucket,
                   const char *layout_signature)
{
  return g_strdup_printf("%s|%u|%s",
                         event_id ? event_id : "",
                         width_bucket,
                         layout_signature ? layout_signature : LAYOUT_SIGNATURE);
}

static gboolean
geometry_token_parse(const char *token,
                     char **out_event_id,
                     guint *out_width_bucket,
                     char **out_layout_signature)
{
  if (!token || !*token)
    return FALSE;

  g_auto(GStrv) parts = g_strsplit(token, "|", 3);
  if (!parts[0] || !*parts[0] || !parts[1] || !parts[2])
    return FALSE;

  guint64 width = g_ascii_strtoull(parts[1], NULL, 10);
  if (width == 0 || width > G_MAXUINT)
    return FALSE;

  if (out_event_id)
    *out_event_id = g_strdup(parts[0]);
  if (out_width_bucket)
    *out_width_bucket = (guint)width;
  if (out_layout_signature)
    *out_layout_signature = g_strdup(parts[2]);

  return TRUE;
}

static gboolean
batch_generation_is_current(GnostrTimelineFeedController *self,
                            GnostrTimelineBatch *batch)
{
  guint64 batch_generation = gnostr_timeline_batch_get_generation(batch);
  guint64 current_generation = self->query_generation;
#ifndef GNOSTR_TIMELINE_FEED_CONTROLLER_NO_SOURCE
  if (self->source)
    current_generation = gnostr_timeline_source_get_generation(self->source);
#endif

  if (batch_generation != current_generation) {
    g_debug("[COMPOSITOR] Dropping stale %s batch gen=%" G_GUINT64_FORMAT
            " current=%" G_GUINT64_FORMAT,
            gnostr_timeline_batch_kind_to_string(gnostr_timeline_batch_get_kind(batch)),
            batch_generation,
            current_generation);
    return FALSE;
  }

  return TRUE;
}

static WorkingEntry *
lookup_working(GnostrTimelineFeedController *self,
               const char *event_id)
{
  if (!event_id)
    return NULL;
  return g_hash_table_lookup(self->by_event_id, event_id);
}

static WorkingEntry *
merge_batch_entry(GnostrTimelineFeedController *self,
                  const GnostrTimelineBatchEntry *entry,
                  gboolean *out_inserted)
{
  g_return_val_if_fail(entry != NULL, NULL);

  g_autofree char *event_id = NULL;
  if (!bytes32_all_zero(entry->event_id)) {
    char hex[65];
    bytes_to_hex32(entry->event_id, hex);
    event_id = g_strdup(hex);
  } else {
    event_id = g_strdup_printf("note-key-%" G_GUINT64_FORMAT, entry->note_key);
  }

  WorkingEntry *existing = lookup_working(self, event_id);
  if (existing) {
    working_entry_update_from_batch_entry(existing, entry);
    if (out_inserted)
      *out_inserted = FALSE;
    return existing;
  }

  WorkingEntry *created = working_entry_new_from_batch_entry(entry);
  g_ptr_array_add(self->working, created);
  g_hash_table_insert(self->by_event_id, g_strdup(created->event_id), created);

  if (out_inserted)
    *out_inserted = TRUE;
  return created;
}

static void
clear_working_set(GnostrTimelineFeedController *self)
{
  g_ptr_array_set_size(self->working, 0);
  g_hash_table_remove_all(self->by_event_id);
  g_hash_table_remove_all(self->pending_head);
}

static guint
pending_count(GnostrTimelineFeedController *self)
{
  return self->pending_head ? g_hash_table_size(self->pending_head) : 0;
}

static void
emit_pending_count(GnostrTimelineFeedController *self)
{
  g_signal_emit(self, signals[SIGNAL_PENDING_COUNT_CHANGED], 0, pending_count(self));
}

static void
emit_profile_requests(GnostrTimelineFeedController *self,
                      GnostrTimelineBatch *batch)
{
  guint n = gnostr_timeline_batch_get_n_profile_requests(batch);
  for (guint i = 0; i < n; i++) {
    const char *pubkey = gnostr_timeline_batch_get_profile_request(batch, i);
    if (pubkey && *pubkey)
      g_signal_emit(self, signals[SIGNAL_NEED_PROFILE], 0, pubkey);
  }
}

static GnostrTimelineSnapshot *
dup_current_snapshot(GnostrTimelineFeedController *self)
{
  return gnostr_timeline_snapshot_model_dup_snapshot(self->model);
}

static gboolean
capture_anchor(GnostrTimelineFeedController *self,
               GnostrTimelineSnapshot *snapshot,
               GnostrTimelineAnchor *out_anchor)
{
  if (!snapshot || !out_anchor)
    return FALSE;

  guint n_rows = gnostr_timeline_snapshot_get_n_rows(snapshot);
  if (n_rows == 0)
    return FALSE;

  double scroll_y = MAX(self->scroll_y, 0.0);
  guint lo = 0;
  guint hi = n_rows;
  while (lo < hi) {
    guint mid = lo + (hi - lo) / 2;
    double bottom = gnostr_timeline_snapshot_get_row_bottom(snapshot, mid);
    if (bottom <= scroll_y)
      lo = mid + 1;
    else
      hi = mid;
  }

  guint index = MIN(lo, n_rows - 1);
  GnostrTimelineSnapshotRow *row = gnostr_timeline_snapshot_get_row(snapshot, index);
  const char *event_id = row ? gnostr_timeline_snapshot_row_get_event_id(row) : NULL;
  if (!event_id || !*event_id)
    return FALSE;

  out_anchor->event_id = g_strdup(event_id);
  out_anchor->index_hint = index;
  out_anchor->offset_px_in_row = MAX(0.0, scroll_y - gnostr_timeline_snapshot_get_row_top(snapshot, index));
  out_anchor->snapshot_generation = gnostr_timeline_snapshot_get_generation(snapshot);
  return TRUE;
}

static gboolean
restore_anchor_scroll(GnostrTimelineSnapshot *old_snapshot,
                      GnostrTimelineSnapshot *new_snapshot,
                      const GnostrTimelineAnchor *anchor,
                      double *out_scroll_y)
{
  if (!old_snapshot || !new_snapshot || !anchor || !out_scroll_y)
    return FALSE;

  guint new_index = 0;
  if (anchor->event_id && gnostr_timeline_snapshot_lookup_event(new_snapshot, anchor->event_id, &new_index)) {
    *out_scroll_y = gnostr_timeline_snapshot_get_row_top(new_snapshot, new_index) + anchor->offset_px_in_row;
    return TRUE;
  }

  guint old_n = gnostr_timeline_snapshot_get_n_rows(old_snapshot);
  if (old_n == 0)
    return FALSE;

  guint hint = MIN(anchor->index_hint, old_n - 1);

  for (gint i = (gint)hint - 1; i >= 0; i--) {
    GnostrTimelineSnapshotRow *row = gnostr_timeline_snapshot_get_row(old_snapshot, (guint)i);
    const char *event_id = row ? gnostr_timeline_snapshot_row_get_event_id(row) : NULL;
    if (event_id && gnostr_timeline_snapshot_lookup_event(new_snapshot, event_id, &new_index)) {
      *out_scroll_y = gnostr_timeline_snapshot_get_row_top(new_snapshot, new_index);
      return TRUE;
    }
  }

  for (guint i = hint + 1; i < old_n; i++) {
    GnostrTimelineSnapshotRow *row = gnostr_timeline_snapshot_get_row(old_snapshot, i);
    const char *event_id = row ? gnostr_timeline_snapshot_row_get_event_id(row) : NULL;
    if (event_id && gnostr_timeline_snapshot_lookup_event(new_snapshot, event_id, &new_index)) {
      *out_scroll_y = gnostr_timeline_snapshot_get_row_top(new_snapshot, new_index);
      return TRUE;
    }
  }

  if (gnostr_timeline_snapshot_get_n_rows(new_snapshot) > 0) {
    *out_scroll_y = 0.0;
    return TRUE;
  }

  return FALSE;
}

static double
entry_effective_height(GnostrTimelineFeedController *self,
                       WorkingEntry *entry,
                       double *out_measured,
                       gboolean *out_geometry_measured)
{
  g_autofree char *token = geometry_token_new(entry->event_id, self->width_bucket, LAYOUT_SIGNATURE);
  GeometryEntry *geometry = g_hash_table_lookup(self->geometry, token);

  if (geometry && geometry->measured_height > 0.0) {
    if (out_measured)
      *out_measured = geometry->measured_height;
    if (out_geometry_measured)
      *out_geometry_measured = TRUE;
    return geometry->measured_height;
  }

  if (out_measured)
    *out_measured = 0.0;
  if (out_geometry_measured)
    *out_geometry_measured = FALSE;
  return DEFAULT_ROW_ESTIMATE_PX;
}

static GnostrTimelineSnapshotRow *
snapshot_row_from_entry(GnostrTimelineFeedController *self,
                        WorkingEntry *entry)
{
  double measured = 0.0;
  gboolean geometry_measured = FALSE;
  double effective = entry_effective_height(self, entry, &measured, &geometry_measured);

  return gnostr_timeline_snapshot_row_new_full(entry->event_id,
                                               entry->note_key,
                                               entry->pubkey,
                                               entry->created_at,
                                               entry->tie_breaker,
                                               entry->content,
                                               entry->display_name,
                                               entry->handle,
                                               entry->avatar_url,
                                               entry->nip05,
                                               entry->has_profile,
                                               entry->like_count,
                                               entry->is_liked,
                                               entry->repost_count,
                                               entry->reply_count,
                                               entry->zap_count,
                                               entry->zap_total_msat,
                                               DEFAULT_ROW_ESTIMATE_PX,
                                               measured,
                                               effective,
                                               self->width_bucket,
                                               LAYOUT_SIGNATURE,
                                               geometry_measured);
}

static GnostrTimelineSnapshot *
compose_snapshot(GnostrTimelineFeedController *self)
{
  GPtrArray *rows = g_ptr_array_new_with_free_func(g_object_unref);

  for (guint i = 0; i < self->working->len; i++) {
    WorkingEntry *entry = g_ptr_array_index(self->working, i);
    if (!entry || !entry->event_id)
      continue;
    if (g_hash_table_contains(self->pending_head, entry->event_id))
      continue;

    g_ptr_array_add(rows, snapshot_row_from_entry(self, entry));
  }

  self->snapshot_generation++;
  if (self->snapshot_generation == 0)
    self->snapshot_generation = 1;

  GnostrTimelineSnapshot *snapshot = gnostr_timeline_snapshot_new(self->snapshot_generation,
                                                                  self->query_generation,
                                                                  (GnostrTimelineSnapshotRow * const *)rows->pdata,
                                                                  rows->len,
                                                                  pending_count(self));
  g_ptr_array_unref(rows);
  return snapshot;
}

static gboolean
compose_timeout_cb(gpointer user_data)
{
  GnostrTimelineFeedController *self = GNOSTR_TIMELINE_FEED_CONTROLLER(user_data);
  self->compose_source_id = 0;

  gboolean preserve_anchor = self->scheduled_preserve_anchor;
  gboolean scroll_to_top = self->scheduled_scroll_to_top;
  self->scheduled_preserve_anchor = FALSE;
  self->scheduled_scroll_to_top = FALSE;

  compose_and_publish(self, preserve_anchor, scroll_to_top);
  return G_SOURCE_REMOVE;
}

static void
schedule_compose(GnostrTimelineFeedController *self,
                 gboolean preserve_anchor,
                 gboolean scroll_to_top)
{
  g_return_if_fail(GNOSTR_IS_TIMELINE_FEED_CONTROLLER(self));

  self->scheduled_preserve_anchor |= preserve_anchor;
  self->scheduled_scroll_to_top |= scroll_to_top;

  if (self->compose_source_id != 0)
    return;

  self->compose_source_id = g_timeout_add_full(G_PRIORITY_DEFAULT_IDLE,
                                               COMPOSE_DEBOUNCE_MS,
                                               compose_timeout_cb,
                                               g_object_ref(self),
                                               g_object_unref);
}

static void
compose_and_publish(GnostrTimelineFeedController *self,
                    gboolean preserve_anchor,
                    gboolean scroll_to_top)
{
  g_autoptr(GnostrTimelineSnapshot) old_snapshot = dup_current_snapshot(self);
  GnostrTimelineAnchor anchor = { 0 };
  gboolean have_anchor = FALSE;

  if (preserve_anchor && !scroll_to_top)
    have_anchor = capture_anchor(self, old_snapshot, &anchor);

  g_autoptr(GnostrTimelineSnapshot) snapshot = compose_snapshot(self);
  gnostr_timeline_snapshot_model_replace_snapshot(self->model, snapshot);

  if (scroll_to_top) {
    self->scroll_y = 0.0;
    g_signal_emit(self, signals[SIGNAL_RESTORE_SCROLL], 0, 0.0);
  } else if (have_anchor) {
    double restore_y = self->scroll_y;
    if (restore_anchor_scroll(old_snapshot, snapshot, &anchor, &restore_y)) {
      restore_y = MAX(0.0, restore_y);
      self->scroll_y = restore_y;
      g_signal_emit(self, signals[SIGNAL_RESTORE_SCROLL], 0, restore_y);
    }
  }

  g_signal_emit(self, signals[SIGNAL_SNAPSHOT_PUBLISHED], 0, snapshot);
  gnostr_timeline_anchor_clear(&anchor);
}

static gboolean
apply_metadata_patch(GnostrTimelineFeedController *self,
                     const GnostrTimelineMetadataPatch *patch)
{
  if (!patch || !patch->event_id)
    return FALSE;

  WorkingEntry *entry = lookup_working(self, patch->event_id);
  if (!entry)
    return FALSE;

  gboolean changed = FALSE;
  if (patch->has_like_count && entry->like_count != patch->like_count) {
    entry->like_count = patch->like_count;
    changed = TRUE;
  }
  if (patch->has_is_liked && entry->is_liked != patch->is_liked) {
    entry->is_liked = patch->is_liked;
    changed = TRUE;
  }
  if (patch->has_repost_count && entry->repost_count != patch->repost_count) {
    entry->repost_count = patch->repost_count;
    changed = TRUE;
  }
  if (patch->has_reply_count && entry->reply_count != patch->reply_count) {
    entry->reply_count = patch->reply_count;
    changed = TRUE;
  }
  if (patch->has_zap_count && entry->zap_count != patch->zap_count) {
    entry->zap_count = patch->zap_count;
    changed = TRUE;
  }
  if (patch->has_zap_total_msat && entry->zap_total_msat != patch->zap_total_msat) {
    entry->zap_total_msat = patch->zap_total_msat;
    changed = TRUE;
  }

  return changed;
}

static gboolean
apply_profile_patch(GnostrTimelineFeedController *self,
                    const GnostrTimelineProfilePatch *patch)
{
  if (!patch || !patch->pubkey_hex)
    return FALSE;

  gboolean changed = FALSE;
  for (guint i = 0; i < self->working->len; i++) {
    WorkingEntry *entry = g_ptr_array_index(self->working, i);
    if (!entry || g_strcmp0(entry->pubkey, patch->pubkey_hex) != 0)
      continue;

    if (g_strcmp0(entry->display_name, patch->display_name) != 0) {
      g_free(entry->display_name);
      entry->display_name = g_strdup(patch->display_name);
      changed = TRUE;
    }
    if (g_strcmp0(entry->handle, patch->handle) != 0) {
      g_free(entry->handle);
      entry->handle = g_strdup(patch->handle);
      changed = TRUE;
    }
    if (g_strcmp0(entry->avatar_url, patch->avatar_url) != 0) {
      g_free(entry->avatar_url);
      entry->avatar_url = g_strdup(patch->avatar_url);
      changed = TRUE;
    }
    if (g_strcmp0(entry->nip05, patch->nip05) != 0) {
      g_free(entry->nip05);
      entry->nip05 = g_strdup(patch->nip05);
      changed = TRUE;
    }
    entry->has_profile = TRUE;
  }

  return changed;
}

static void
on_source_batch(GnostrTimelineSource *source G_GNUC_UNUSED,
                GnostrTimelineBatch *batch,
                gpointer user_data)
{
  gnostr_timeline_feed_controller_ingest_batch(GNOSTR_TIMELINE_FEED_CONTROLLER(user_data), batch);
}

static void
gnostr_timeline_feed_controller_dispose(GObject *object)
{
  GnostrTimelineFeedController *self = GNOSTR_TIMELINE_FEED_CONTROLLER(object);

  if (self->compose_source_id != 0) {
    g_source_remove(self->compose_source_id);
    self->compose_source_id = 0;
  }

  if (self->source && self->source_batch_handler != 0) {
    g_signal_handler_disconnect(self->source, self->source_batch_handler);
    self->source_batch_handler = 0;
  }

  g_clear_object(&self->source);
  g_clear_object(&self->model);
  g_clear_pointer(&self->working, g_ptr_array_unref);
  g_clear_pointer(&self->by_event_id, g_hash_table_unref);
  g_clear_pointer(&self->pending_head, g_hash_table_unref);
  g_clear_pointer(&self->geometry, g_hash_table_unref);

  G_OBJECT_CLASS(gnostr_timeline_feed_controller_parent_class)->dispose(object);
}

static void
gnostr_timeline_feed_controller_class_init(GnostrTimelineFeedControllerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = gnostr_timeline_feed_controller_dispose;

  signals[SIGNAL_PENDING_COUNT_CHANGED] =
    g_signal_new("pending-count-changed",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0,
                 NULL, NULL,
                 g_cclosure_marshal_VOID__UINT,
                 G_TYPE_NONE,
                 1,
                 G_TYPE_UINT);

  signals[SIGNAL_RESTORE_SCROLL] =
    g_signal_new("restore-scroll",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0,
                 NULL, NULL,
                 NULL,
                 G_TYPE_NONE,
                 1,
                 G_TYPE_DOUBLE);

  signals[SIGNAL_SNAPSHOT_PUBLISHED] =
    g_signal_new("snapshot-published",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0,
                 NULL, NULL,
                 g_cclosure_marshal_VOID__OBJECT,
                 G_TYPE_NONE,
                 1,
                 GNOSTR_TYPE_TIMELINE_SNAPSHOT);

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
}

static void
gnostr_timeline_feed_controller_init(GnostrTimelineFeedController *self)
{
  self->query_generation = 1;
  self->snapshot_generation = 0;
  self->model = gnostr_timeline_snapshot_model_new();
  self->working = g_ptr_array_new_with_free_func((GDestroyNotify)working_entry_free);
  self->by_event_id = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->pending_head = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->geometry = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)geometry_entry_free);
  self->user_at_top = TRUE;
  self->scroll_y = 0.0;
  self->viewport_height = 0.0;
  self->width_bucket = DEFAULT_WIDTH_BUCKET;
}

GnostrTimelineFeedController *
gnostr_timeline_feed_controller_new(GnostrTimelineSource *source)
{
  GnostrTimelineFeedController *self =
    g_object_new(GNOSTR_TYPE_TIMELINE_FEED_CONTROLLER, NULL);
  if (source)
    gnostr_timeline_feed_controller_set_source(self, source);
  return self;
}

GnostrTimelineSnapshotModel *
gnostr_timeline_feed_controller_get_model(GnostrTimelineFeedController *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_FEED_CONTROLLER(self), NULL);
  return self->model;
}

GnostrTimelineSource *
gnostr_timeline_feed_controller_get_source(GnostrTimelineFeedController *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_FEED_CONTROLLER(self), NULL);
  return self->source;
}

guint64
gnostr_timeline_feed_controller_get_query_generation(GnostrTimelineFeedController *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_FEED_CONTROLLER(self), 0);
  return self->query_generation;
}

void
gnostr_timeline_feed_controller_set_source(GnostrTimelineFeedController *self,
                                           GnostrTimelineSource *source)
{
  g_return_if_fail(GNOSTR_IS_TIMELINE_FEED_CONTROLLER(self));
#ifndef GNOSTR_TIMELINE_FEED_CONTROLLER_NO_SOURCE
  g_return_if_fail(source == NULL || GNOSTR_IS_TIMELINE_SOURCE(source));
#endif

  if (self->source == source)
    return;

  if (self->source && self->source_batch_handler != 0) {
    g_signal_handler_disconnect(self->source, self->source_batch_handler);
    self->source_batch_handler = 0;
  }

  g_set_object(&self->source, source);
#ifndef GNOSTR_TIMELINE_FEED_CONTROLLER_NO_SOURCE
  if (self->source) {
    self->query_generation = gnostr_timeline_source_get_generation(self->source);
    self->source_batch_handler = g_signal_connect(self->source,
                                                   "batch",
                                                   G_CALLBACK(on_source_batch),
                                                   self);
  }
#endif
}

void
gnostr_timeline_feed_controller_set_query(GnostrTimelineFeedController *self,
                                          GNostrTimelineQuery *query)
{
  g_return_if_fail(GNOSTR_IS_TIMELINE_FEED_CONTROLLER(self));

#ifndef GNOSTR_TIMELINE_FEED_CONTROLLER_NO_SOURCE
  if (!self->source) {
    GnostrTimelineSource *source = gnostr_timeline_source_new();
    gnostr_timeline_feed_controller_set_source(self, source);
    g_object_unref(source);
  }

  gnostr_timeline_source_set_query(self->source, query);
  self->query_generation = gnostr_timeline_source_get_generation(self->source);
#else
  (void)query;
  self->query_generation++;
  if (self->query_generation == 0)
    self->query_generation = 1;
#endif
  self->loading_older = FALSE;
  self->loading_newer = FALSE;
  clear_working_set(self);
  emit_pending_count(self);
  compose_and_publish(self, FALSE, TRUE);
}

void
gnostr_timeline_feed_controller_refresh(GnostrTimelineFeedController *self)
{
  g_return_if_fail(GNOSTR_IS_TIMELINE_FEED_CONTROLLER(self));
#ifndef GNOSTR_TIMELINE_FEED_CONTROLLER_NO_SOURCE
  if (self->source)
    gnostr_timeline_source_refresh_async(self->source);
#endif
}

void
gnostr_timeline_feed_controller_load_older(GnostrTimelineFeedController *self,
                                           guint count)
{
  g_return_if_fail(GNOSTR_IS_TIMELINE_FEED_CONTROLLER(self));
#ifdef GNOSTR_TIMELINE_FEED_CONTROLLER_NO_SOURCE
  (void)count;
  return;
#else
  if (!self->source || count == 0 || self->loading_older)
    return;

  self->loading_older = TRUE;

  gint64 before = 0;
  g_autoptr(GnostrTimelineSnapshot) snapshot = dup_current_snapshot(self);
  if (snapshot && gnostr_timeline_snapshot_get_n_rows(snapshot) > 0) {
    GnostrTimelineSnapshotRow *last =
      gnostr_timeline_snapshot_get_row(snapshot,
                                       gnostr_timeline_snapshot_get_n_rows(snapshot) - 1);
    before = last ? gnostr_timeline_snapshot_row_get_created_at(last) : 0;
  }

  gnostr_timeline_source_load_older_async(self->source, count, before);
#endif
}

void
gnostr_timeline_feed_controller_load_newer(GnostrTimelineFeedController *self,
                                           guint count)
{
  g_return_if_fail(GNOSTR_IS_TIMELINE_FEED_CONTROLLER(self));
#ifdef GNOSTR_TIMELINE_FEED_CONTROLLER_NO_SOURCE
  (void)count;
  return;
#else
  if (!self->source || count == 0 || self->loading_newer)
    return;

  self->loading_newer = TRUE;

  gint64 after = 0;
  g_autoptr(GnostrTimelineSnapshot) snapshot = dup_current_snapshot(self);
  if (snapshot && gnostr_timeline_snapshot_get_n_rows(snapshot) > 0) {
    GnostrTimelineSnapshotRow *first = gnostr_timeline_snapshot_get_row(snapshot, 0);
    after = first ? gnostr_timeline_snapshot_row_get_created_at(first) : 0;
  }

  gnostr_timeline_source_load_newer_async(self->source, count, after);
#endif
}

void
gnostr_timeline_feed_controller_ingest_batch(GnostrTimelineFeedController *self,
                                             GnostrTimelineBatch *batch)
{
  g_return_if_fail(GNOSTR_IS_TIMELINE_FEED_CONTROLLER(self));
  g_return_if_fail(GNOSTR_IS_TIMELINE_BATCH(batch));

  if (!batch_generation_is_current(self, batch))
    return;

  GnostrTimelineBatchKind kind = gnostr_timeline_batch_get_kind(batch);
  guint n_entries = gnostr_timeline_batch_get_n_entries(batch);

  switch (kind) {
    case GNOSTR_TIMELINE_BATCH_REFRESH:
      clear_working_set(self);
      for (guint i = 0; i < n_entries; i++) {
        const GnostrTimelineBatchEntry *entry = gnostr_timeline_batch_get_entry(batch, i);
        if (entry)
          merge_batch_entry(self, entry, NULL);
      }
      emit_pending_count(self);
      schedule_compose(self, FALSE, TRUE);
      break;

    case GNOSTR_TIMELINE_BATCH_LIVE_HEAD: {
      gboolean pending_changed = FALSE;
      for (guint i = 0; i < n_entries; i++) {
        const GnostrTimelineBatchEntry *entry = gnostr_timeline_batch_get_entry(batch, i);
        if (!entry)
          continue;

        gboolean inserted = FALSE;
        WorkingEntry *we = merge_batch_entry(self, entry, &inserted);
        if (!self->user_at_top && we && inserted && !g_hash_table_contains(self->pending_head, we->event_id)) {
          g_hash_table_add(self->pending_head, g_strdup(we->event_id));
          pending_changed = TRUE;
        }
      }

      if (self->user_at_top) {
        schedule_compose(self, FALSE, TRUE);
      } else if (pending_changed) {
        emit_pending_count(self);
      }
      break;
    }

    case GNOSTR_TIMELINE_BATCH_PAGE_OLDER:
      self->loading_older = FALSE;
      for (guint i = 0; i < n_entries; i++) {
        const GnostrTimelineBatchEntry *entry = gnostr_timeline_batch_get_entry(batch, i);
        if (entry)
          merge_batch_entry(self, entry, NULL);
      }
      schedule_compose(self, TRUE, FALSE);
      break;

    case GNOSTR_TIMELINE_BATCH_PAGE_NEWER:
      self->loading_newer = FALSE;
      for (guint i = 0; i < n_entries; i++) {
        const GnostrTimelineBatchEntry *entry = gnostr_timeline_batch_get_entry(batch, i);
        if (entry)
          merge_batch_entry(self, entry, NULL);
      }
      schedule_compose(self, TRUE, FALSE);
      break;

    case GNOSTR_TIMELINE_BATCH_DELETE:
      /* The current source batch entry identifies the delete event itself, not
       * the NIP-09 target(s).  Do not remove or insert anything until the patch
       * conversion layer carries explicit target event ids. */
      g_debug("[COMPOSITOR] Ignoring delete batch without target ids (%u entries)", n_entries);
      break;

    case GNOSTR_TIMELINE_BATCH_PROFILE_PATCH: {
      gboolean changed = FALSE;
      guint n_patches = gnostr_timeline_batch_get_n_profile_patches(batch);
      for (guint i = 0; i < n_patches; i++) {
        const GnostrTimelineProfilePatch *patch =
          gnostr_timeline_batch_get_profile_patch(batch, i);
        changed |= apply_profile_patch(self, patch);
      }
      if (changed)
        schedule_compose(self, TRUE, FALSE);
      else if (n_patches == 0 && n_entries > 0)
        g_debug("[COMPOSITOR] Ignoring profile patch batch with no projected profile payload (%u entries)",
                n_entries);
      break;
    }

    case GNOSTR_TIMELINE_BATCH_METADATA_PATCH: {
      gboolean changed = FALSE;
      guint n_patches = gnostr_timeline_batch_get_n_metadata_patches(batch);
      for (guint i = 0; i < n_patches; i++) {
        const GnostrTimelineMetadataPatch *patch =
          gnostr_timeline_batch_get_metadata_patch(batch, i);
        changed |= apply_metadata_patch(self, patch);
      }
      if (changed)
        schedule_compose(self, TRUE, FALSE);
      else if (n_patches == 0 && n_entries > 0)
        g_debug("[COMPOSITOR] Ignoring metadata patch batch with no target-row payload (%u entries)",
                n_entries);
      break;
    }
  }

  emit_profile_requests(self, batch);
}

void
gnostr_timeline_feed_controller_set_viewport(GnostrTimelineFeedController *self,
                                             double scroll_y,
                                             double viewport_height,
                                             guint width_px)
{
  g_return_if_fail(GNOSTR_IS_TIMELINE_FEED_CONTROLLER(self));

  guint old_bucket = self->width_bucket;
  self->scroll_y = MAX(scroll_y, 0.0);
  self->viewport_height = MAX(viewport_height, 0.0);
  self->width_bucket = width_to_bucket(width_px);
  self->user_at_top = self->scroll_y <= AT_TOP_EPSILON_PX;

  if (old_bucket != self->width_bucket)
    schedule_compose(self, TRUE, FALSE);
}

void
gnostr_timeline_feed_controller_set_user_at_top(GnostrTimelineFeedController *self,
                                                gboolean at_top)
{
  g_return_if_fail(GNOSTR_IS_TIMELINE_FEED_CONTROLLER(self));
  self->user_at_top = at_top;
}

gboolean
gnostr_timeline_feed_controller_get_user_at_top(GnostrTimelineFeedController *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_FEED_CONTROLLER(self), FALSE);
  return self->user_at_top;
}

guint
gnostr_timeline_feed_controller_get_pending_count(GnostrTimelineFeedController *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_FEED_CONTROLLER(self), 0);
  return pending_count(self);
}

void
gnostr_timeline_feed_controller_admit_pending_head(GnostrTimelineFeedController *self,
                                                   gboolean scroll_to_top)
{
  g_return_if_fail(GNOSTR_IS_TIMELINE_FEED_CONTROLLER(self));

  if (pending_count(self) == 0)
    return;

  g_hash_table_remove_all(self->pending_head);
  emit_pending_count(self);
  schedule_compose(self, !scroll_to_top, scroll_to_top);
}

void
gnostr_timeline_feed_controller_compose_now(GnostrTimelineFeedController *self)
{
  g_return_if_fail(GNOSTR_IS_TIMELINE_FEED_CONTROLLER(self));

  gboolean preserve_anchor = self->scheduled_preserve_anchor;
  gboolean scroll_to_top = self->scheduled_scroll_to_top;
  self->scheduled_preserve_anchor = FALSE;
  self->scheduled_scroll_to_top = FALSE;

  if (self->compose_source_id != 0) {
    g_source_remove(self->compose_source_id);
    self->compose_source_id = 0;
  }

  compose_and_publish(self, preserve_anchor, scroll_to_top);
}

char *
gnostr_timeline_feed_controller_dup_geometry_token_for_row(GnostrTimelineSnapshotRow *row)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT_ROW(row), NULL);

  return geometry_token_new(gnostr_timeline_snapshot_row_get_event_id(row),
                            gnostr_timeline_snapshot_row_get_width_bucket(row),
                            gnostr_timeline_snapshot_row_get_layout_signature(row));
}

void
gnostr_timeline_feed_controller_record_geometry(GnostrTimelineFeedController *self,
                                                const char *geometry_token,
                                                guint64 snapshot_generation,
                                                gint width_px,
                                                gint height_px)
{
  g_return_if_fail(GNOSTR_IS_TIMELINE_FEED_CONTROLLER(self));

  if (!geometry_token || !*geometry_token || height_px <= 0)
    return;

  g_autofree char *event_id = NULL;
  g_autofree char *layout_signature = NULL;
  guint token_width_bucket = 0;
  if (!geometry_token_parse(geometry_token, &event_id, &token_width_bucket, &layout_signature))
    return;

  guint actual_width_bucket = width_to_bucket((guint)MAX(width_px, 0));
  if (actual_width_bucket != token_width_bucket) {
    g_debug("[COMPOSITOR] Ignoring geometry token width mismatch token=%u actual=%u",
            token_width_bucket,
            actual_width_bucket);
    return;
  }

  g_autoptr(GnostrTimelineSnapshot) current_snapshot = dup_current_snapshot(self);
  guint64 current_generation = current_snapshot ?
    gnostr_timeline_snapshot_get_generation(current_snapshot) : 0;
  if (snapshot_generation > 0 && current_generation > 0 && snapshot_generation < current_generation)
    return;

  GeometryEntry *existing = g_hash_table_lookup(self->geometry, geometry_token);
  if (existing) {
    if (existing->snapshot_generation > snapshot_generation)
      return;
    if (existing->width_bucket == token_width_bucket &&
        existing->measured_height == (double)height_px &&
        g_strcmp0(existing->layout_signature, layout_signature) == 0) {
      existing->snapshot_generation = MAX(existing->snapshot_generation, snapshot_generation);
      return;
    }
  }

  GeometryEntry *entry = geometry_entry_new(token_width_bucket,
                                            layout_signature,
                                            (double)height_px,
                                            snapshot_generation);
  g_hash_table_replace(self->geometry, g_strdup(geometry_token), entry);

  /* Measurements refine future editions.  Preserve the reader's current anchor
   * because footprint changes are compositor-driven, not user navigation. */
  schedule_compose(self, TRUE, FALSE);
}

void
gnostr_timeline_anchor_clear(GnostrTimelineAnchor *anchor)
{
  if (!anchor)
    return;
  g_clear_pointer(&anchor->event_id, g_free);
  anchor->index_hint = 0;
  anchor->offset_px_in_row = 0.0;
  anchor->snapshot_generation = 0;
}
