#include "gnostr-timeline-snapshot.h"

#include <string.h>

struct _GnostrTimelineSnapshotRow {
  GObject parent_instance;

  char *event_id;
  char *note_key;
  char *pubkey;
  gint64 created_at;
  char *tie_breaker;
  char *content;
  char *display_name;
  char *handle;
  char *avatar_url;
  char *nip05;
  char *root_id;
  char *reply_id;
  char *quoted_event_id;
  char *reposted_event_id;
  char **hashtags;
  gint kind;
  gboolean has_profile;
  guint like_count;
  gboolean is_liked;
  guint repost_count;
  guint reply_count;
  guint zap_count;
  gint64 zap_total_msat;
  GnostrTimelineItemViewModel *view_model;

  double estimated_height;
  double measured_height;
  double effective_height;
  double media_reserved_height;
  double link_preview_reserved_height;
  guint width_bucket;
  char *layout_signature;
  gboolean geometry_measured;
};

G_DEFINE_TYPE(GnostrTimelineSnapshotRow, gnostr_timeline_snapshot_row, G_TYPE_OBJECT)

static void
gnostr_timeline_snapshot_row_finalize(GObject *object)
{
  GnostrTimelineSnapshotRow *self = GNOSTR_TIMELINE_SNAPSHOT_ROW(object);

  g_free(self->event_id);
  g_free(self->note_key);
  g_free(self->pubkey);
  g_free(self->tie_breaker);
  g_free(self->content);
  g_free(self->display_name);
  g_free(self->handle);
  g_free(self->avatar_url);
  g_free(self->nip05);
  g_free(self->root_id);
  g_free(self->reply_id);
  g_free(self->quoted_event_id);
  g_free(self->reposted_event_id);
  g_strfreev(self->hashtags);
  g_clear_object(&self->view_model);
  g_free(self->layout_signature);

  G_OBJECT_CLASS(gnostr_timeline_snapshot_row_parent_class)->finalize(object);
}

static void
gnostr_timeline_snapshot_row_class_init(GnostrTimelineSnapshotRowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->finalize = gnostr_timeline_snapshot_row_finalize;
}

static void
gnostr_timeline_snapshot_row_init(GnostrTimelineSnapshotRow *self)
{
  (void)self;
}

GnostrTimelineSnapshotRow *
gnostr_timeline_snapshot_row_new(const char *event_id,
                                  const char *note_key,
                                  const char *pubkey,
                                  gint64 created_at,
                                  const char *tie_breaker,
                                  const char *content,
                                  double estimated_height,
                                  double measured_height,
                                  double effective_height,
                                  double media_reserved_height,
                                  double link_preview_reserved_height,
                                  guint width_bucket,
                                  const char *layout_signature,
                                  gboolean geometry_measured)
{
  return gnostr_timeline_snapshot_row_new_full(event_id,
                                               note_key,
                                               pubkey,
                                               created_at,
                                               tie_breaker,
                                               content,
                                               NULL,
                                               NULL,
                                               NULL,
                                               NULL,
                                               NULL,
                                               NULL,
                                               NULL,
                                               NULL,
                                               NULL,
                                               1,
                                               FALSE,
                                               0,
                                               FALSE,
                                               0,
                                               0,
                                               0,
                                               0,
                                               estimated_height,
                                               measured_height,
                                               effective_height,
                                               media_reserved_height,
                                               link_preview_reserved_height,
                                               width_bucket,
                                               layout_signature,
                                               geometry_measured);
}

GnostrTimelineSnapshotRow *
gnostr_timeline_snapshot_row_new_full(const char *event_id,
                                       const char *note_key,
                                       const char *pubkey,
                                       gint64 created_at,
                                       const char *tie_breaker,
                                       const char *content,
                                       const char *display_name,
                                       const char *handle,
                                       const char *avatar_url,
                                       const char *nip05,
                                       const char *root_id,
                                       const char *reply_id,
                                       const char *quoted_event_id,
                                       const char *reposted_event_id,
                                       const char * const *hashtags,
                                       gint kind,
                                       gboolean has_profile,
                                       guint like_count,
                                       gboolean is_liked,
                                       guint repost_count,
                                       guint reply_count,
                                       guint zap_count,
                                       gint64 zap_total_msat,
                                       double estimated_height,
                                       double measured_height,
                                       double effective_height,
                                       double media_reserved_height,
                                       double link_preview_reserved_height,
                                       guint width_bucket,
                                       const char *layout_signature,
                                       gboolean geometry_measured)
{
  GnostrTimelineSnapshotRow *self = g_object_new(GNOSTR_TYPE_TIMELINE_SNAPSHOT_ROW, NULL);

  self->event_id = g_strdup(event_id);
  self->note_key = g_strdup(note_key);
  self->pubkey = g_strdup(pubkey);
  self->created_at = created_at;
  self->tie_breaker = g_strdup(tie_breaker ? tie_breaker : event_id);
  self->content = g_strdup(content);
  self->display_name = g_strdup(display_name);
  self->handle = g_strdup(handle);
  self->avatar_url = g_strdup(avatar_url);
  self->nip05 = g_strdup(nip05);
  self->root_id = g_strdup(root_id);
  self->reply_id = g_strdup(reply_id);
  self->quoted_event_id = g_strdup(quoted_event_id);
  self->reposted_event_id = g_strdup(reposted_event_id);
  self->hashtags = g_strdupv((char **)hashtags);
  self->kind = kind;
  self->has_profile = has_profile;
  self->like_count = like_count;
  self->is_liked = is_liked;
  self->repost_count = repost_count;
  self->reply_count = reply_count;
  self->zap_count = zap_count;
  self->zap_total_msat = zap_total_msat;
  self->estimated_height = estimated_height;
  self->measured_height = measured_height;
  self->effective_height = effective_height > 0.0 ? effective_height : estimated_height;
  self->media_reserved_height = media_reserved_height;
  self->link_preview_reserved_height = link_preview_reserved_height;
  self->width_bucket = width_bucket;
  self->layout_signature = g_strdup(layout_signature);
  self->geometry_measured = geometry_measured;

  return self;
}

GnostrTimelineSnapshotRow *
gnostr_timeline_snapshot_row_new_from_view_model(GnostrTimelineItemViewModel *view_model,
                                                  double estimated_height,
                                                  double measured_height,
                                                  double effective_height,
                                                  double media_reserved_height,
                                                  double link_preview_reserved_height,
                                                  guint width_bucket,
                                                  const char *layout_signature,
                                                  gboolean geometry_measured)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(view_model), NULL);

  GnostrTimelineSnapshotRow *row =
    gnostr_timeline_snapshot_row_new_full(gnostr_timeline_item_view_model_get_event_id(view_model),
                                          gnostr_timeline_item_view_model_get_note_key(view_model),
                                          gnostr_timeline_item_view_model_get_pubkey(view_model),
                                          gnostr_timeline_item_view_model_get_created_at(view_model),
                                          gnostr_timeline_item_view_model_get_tie_breaker(view_model),
                                          gnostr_timeline_item_view_model_get_content(view_model),
                                          gnostr_timeline_item_view_model_get_display_name(view_model),
                                          gnostr_timeline_item_view_model_get_handle(view_model),
                                          gnostr_timeline_item_view_model_get_avatar_url(view_model),
                                          gnostr_timeline_item_view_model_get_nip05(view_model),
                                          gnostr_timeline_item_view_model_get_root_id(view_model),
                                          gnostr_timeline_item_view_model_get_reply_id(view_model),
                                          gnostr_timeline_item_view_model_get_quoted_event_id(view_model),
                                          gnostr_timeline_item_view_model_get_reposted_event_id(view_model),
                                          gnostr_timeline_item_view_model_get_hashtags(view_model),
                                          gnostr_timeline_item_view_model_get_kind(view_model),
                                          gnostr_timeline_item_view_model_get_has_profile(view_model),
                                          gnostr_timeline_item_view_model_get_like_count(view_model),
                                          gnostr_timeline_item_view_model_get_is_liked(view_model),
                                          gnostr_timeline_item_view_model_get_repost_count(view_model),
                                          gnostr_timeline_item_view_model_get_reply_count(view_model),
                                          gnostr_timeline_item_view_model_get_zap_count(view_model),
                                          gnostr_timeline_item_view_model_get_zap_total_msat(view_model),
                                          estimated_height,
                                          measured_height,
                                          effective_height,
                                          media_reserved_height,
                                          link_preview_reserved_height,
                                          width_bucket,
                                          layout_signature,
                                          geometry_measured);
  row->view_model = g_object_ref(view_model);
  return row;
}

const char *gnostr_timeline_snapshot_row_get_event_id(GnostrTimelineSnapshotRow *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT_ROW(self), NULL);
  return self->event_id;
}

const char *gnostr_timeline_snapshot_row_get_note_key(GnostrTimelineSnapshotRow *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT_ROW(self), NULL);
  return self->note_key;
}

const char *gnostr_timeline_snapshot_row_get_pubkey(GnostrTimelineSnapshotRow *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT_ROW(self), NULL);
  return self->pubkey;
}

gint64 gnostr_timeline_snapshot_row_get_created_at(GnostrTimelineSnapshotRow *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT_ROW(self), 0);
  return self->created_at;
}

const char *gnostr_timeline_snapshot_row_get_tie_breaker(GnostrTimelineSnapshotRow *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT_ROW(self), NULL);
  return self->tie_breaker;
}

const char *gnostr_timeline_snapshot_row_get_content(GnostrTimelineSnapshotRow *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT_ROW(self), NULL);
  return self->content;
}

const char *gnostr_timeline_snapshot_row_get_display_name(GnostrTimelineSnapshotRow *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT_ROW(self), NULL);
  return self->display_name;
}

const char *gnostr_timeline_snapshot_row_get_handle(GnostrTimelineSnapshotRow *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT_ROW(self), NULL);
  return self->handle;
}

const char *gnostr_timeline_snapshot_row_get_avatar_url(GnostrTimelineSnapshotRow *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT_ROW(self), NULL);
  return self->avatar_url;
}

const char *gnostr_timeline_snapshot_row_get_nip05(GnostrTimelineSnapshotRow *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT_ROW(self), NULL);
  return self->nip05;
}

const char *gnostr_timeline_snapshot_row_get_root_id(GnostrTimelineSnapshotRow *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT_ROW(self), NULL);
  return self->root_id;
}

const char *gnostr_timeline_snapshot_row_get_reply_id(GnostrTimelineSnapshotRow *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT_ROW(self), NULL);
  return self->reply_id;
}

const char *gnostr_timeline_snapshot_row_get_quoted_event_id(GnostrTimelineSnapshotRow *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT_ROW(self), NULL);
  return self->quoted_event_id;
}

const char *gnostr_timeline_snapshot_row_get_reposted_event_id(GnostrTimelineSnapshotRow *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT_ROW(self), NULL);
  return self->reposted_event_id;
}

const char * const *gnostr_timeline_snapshot_row_get_hashtags(GnostrTimelineSnapshotRow *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT_ROW(self), NULL);
  return (const char * const *)self->hashtags;
}

gint gnostr_timeline_snapshot_row_get_kind(GnostrTimelineSnapshotRow *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT_ROW(self), 1);
  return self->kind;
}

gboolean gnostr_timeline_snapshot_row_get_has_profile(GnostrTimelineSnapshotRow *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT_ROW(self), FALSE);
  return self->has_profile;
}

guint gnostr_timeline_snapshot_row_get_like_count(GnostrTimelineSnapshotRow *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT_ROW(self), 0);
  return self->like_count;
}

gboolean gnostr_timeline_snapshot_row_get_is_liked(GnostrTimelineSnapshotRow *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT_ROW(self), FALSE);
  return self->is_liked;
}

guint gnostr_timeline_snapshot_row_get_repost_count(GnostrTimelineSnapshotRow *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT_ROW(self), 0);
  return self->repost_count;
}

guint gnostr_timeline_snapshot_row_get_reply_count(GnostrTimelineSnapshotRow *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT_ROW(self), 0);
  return self->reply_count;
}

guint gnostr_timeline_snapshot_row_get_zap_count(GnostrTimelineSnapshotRow *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT_ROW(self), 0);
  return self->zap_count;
}

gint64 gnostr_timeline_snapshot_row_get_zap_total_msat(GnostrTimelineSnapshotRow *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT_ROW(self), 0);
  return self->zap_total_msat;
}

double gnostr_timeline_snapshot_row_get_estimated_height(GnostrTimelineSnapshotRow *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT_ROW(self), 0.0);
  return self->estimated_height;
}

double gnostr_timeline_snapshot_row_get_measured_height(GnostrTimelineSnapshotRow *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT_ROW(self), 0.0);
  return self->measured_height;
}

double gnostr_timeline_snapshot_row_get_effective_height(GnostrTimelineSnapshotRow *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT_ROW(self), 0.0);
  return self->effective_height;
}

double gnostr_timeline_snapshot_row_get_media_reserved_height(GnostrTimelineSnapshotRow *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT_ROW(self), 0.0);
  return self->media_reserved_height;
}

double gnostr_timeline_snapshot_row_get_link_preview_reserved_height(GnostrTimelineSnapshotRow *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT_ROW(self), 0.0);
  return self->link_preview_reserved_height;
}

guint gnostr_timeline_snapshot_row_get_width_bucket(GnostrTimelineSnapshotRow *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT_ROW(self), 0);
  return self->width_bucket;
}

const char *gnostr_timeline_snapshot_row_get_layout_signature(GnostrTimelineSnapshotRow *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT_ROW(self), NULL);
  return self->layout_signature;
}

gboolean gnostr_timeline_snapshot_row_get_geometry_measured(GnostrTimelineSnapshotRow *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT_ROW(self), FALSE);
  return self->geometry_measured;
}

GnostrTimelineItemViewModel *
gnostr_timeline_snapshot_row_dup_view_model(GnostrTimelineSnapshotRow *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT_ROW(self), NULL);
  return self->view_model ? g_object_ref(self->view_model) : NULL;
}

gint
gnostr_timeline_snapshot_compare_rows(GnostrTimelineSnapshotRow *a,
                                      GnostrTimelineSnapshotRow *b)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT_ROW(a), 0);
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT_ROW(b), 0);

  if (a->created_at > b->created_at)
    return -1;
  if (a->created_at < b->created_at)
    return 1;

  gint tie = g_strcmp0(a->tie_breaker, b->tie_breaker);
  if (tie != 0)
    return tie;

  return g_strcmp0(a->event_id, b->event_id);
}

struct _GnostrTimelineSnapshot {
  GObject parent_instance;

  guint64 generation;
  guint64 query_generation;
  GPtrArray *rows;
  GHashTable *event_index;
  double *prefix_tops;
  double total_height;
  guint pending_head_count;
};

G_DEFINE_TYPE(GnostrTimelineSnapshot, gnostr_timeline_snapshot, G_TYPE_OBJECT)

static gint
sort_rows_cb(gconstpointer a,
             gconstpointer b)
{
  GnostrTimelineSnapshotRow *row_a = *(GnostrTimelineSnapshotRow * const *)a;
  GnostrTimelineSnapshotRow *row_b = *(GnostrTimelineSnapshotRow * const *)b;
  return gnostr_timeline_snapshot_compare_rows(row_a, row_b);
}

static void
gnostr_timeline_snapshot_rebuild_indexes(GnostrTimelineSnapshot *self)
{
  self->prefix_tops = g_new0(double, self->rows->len + 1);

  for (guint i = 0; i < self->rows->len; i++) {
    GnostrTimelineSnapshotRow *row = g_ptr_array_index(self->rows, i);
    const char *event_id = gnostr_timeline_snapshot_row_get_event_id(row);

    self->prefix_tops[i + 1] = self->prefix_tops[i] +
      gnostr_timeline_snapshot_row_get_effective_height(row);

    if (event_id && *event_id)
      g_hash_table_insert(self->event_index, g_strdup(event_id), GUINT_TO_POINTER(i + 1));
  }

  self->total_height = self->prefix_tops[self->rows->len];
}

static void
gnostr_timeline_snapshot_finalize(GObject *object)
{
  GnostrTimelineSnapshot *self = GNOSTR_TIMELINE_SNAPSHOT(object);

  g_clear_pointer(&self->rows, g_ptr_array_unref);
  g_clear_pointer(&self->event_index, g_hash_table_destroy);
  g_clear_pointer(&self->prefix_tops, g_free);

  G_OBJECT_CLASS(gnostr_timeline_snapshot_parent_class)->finalize(object);
}

static void
gnostr_timeline_snapshot_class_init(GnostrTimelineSnapshotClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->finalize = gnostr_timeline_snapshot_finalize;
}

static void
gnostr_timeline_snapshot_init(GnostrTimelineSnapshot *self)
{
  self->rows = g_ptr_array_new_with_free_func(g_object_unref);
  self->event_index = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
}

GnostrTimelineSnapshot *
gnostr_timeline_snapshot_new(guint64 generation,
                             guint64 query_generation,
                             GnostrTimelineSnapshotRow * const *rows,
                             guint n_rows,
                             guint pending_head_count)
{
  GnostrTimelineSnapshot *self = g_object_new(GNOSTR_TYPE_TIMELINE_SNAPSHOT, NULL);

  self->generation = generation;
  self->query_generation = query_generation;
  self->pending_head_count = pending_head_count;

  for (guint i = 0; i < n_rows; i++) {
    g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT_ROW(rows[i]), self);
    g_ptr_array_add(self->rows, g_object_ref(rows[i]));
  }

  g_ptr_array_sort(self->rows, sort_rows_cb);

  GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
  for (guint i = 0; i < self->rows->len;) {
    GnostrTimelineSnapshotRow *row = g_ptr_array_index(self->rows, i);
    const char *event_id = gnostr_timeline_snapshot_row_get_event_id(row);

    if (event_id && *event_id && g_hash_table_contains(seen, event_id)) {
      g_ptr_array_remove_index(self->rows, i);
      continue;
    }

    if (event_id && *event_id)
      g_hash_table_add(seen, (gpointer)event_id);
    i++;
  }
  g_hash_table_destroy(seen);

  gnostr_timeline_snapshot_rebuild_indexes(self);
  return self;
}

GnostrTimelineSnapshot *
gnostr_timeline_snapshot_new_empty(guint64 generation,
                                   guint64 query_generation)
{
  return gnostr_timeline_snapshot_new(generation, query_generation, NULL, 0, 0);
}

guint64 gnostr_timeline_snapshot_get_generation(GnostrTimelineSnapshot *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT(self), 0);
  return self->generation;
}

guint64 gnostr_timeline_snapshot_get_query_generation(GnostrTimelineSnapshot *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT(self), 0);
  return self->query_generation;
}

guint gnostr_timeline_snapshot_get_n_rows(GnostrTimelineSnapshot *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT(self), 0);
  return self->rows->len;
}

guint gnostr_timeline_snapshot_get_pending_head_count(GnostrTimelineSnapshot *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT(self), 0);
  return self->pending_head_count;
}

double gnostr_timeline_snapshot_get_total_height(GnostrTimelineSnapshot *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT(self), 0.0);
  return self->total_height;
}

GnostrTimelineSnapshotRow *
gnostr_timeline_snapshot_get_row(GnostrTimelineSnapshot *self,
                                  guint index)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT(self), NULL);

  if (index >= self->rows->len)
    return NULL;

  return g_ptr_array_index(self->rows, index);
}

GnostrTimelineSnapshotRow *
gnostr_timeline_snapshot_dup_row(GnostrTimelineSnapshot *self,
                                  guint index)
{
  GnostrTimelineSnapshotRow *row = gnostr_timeline_snapshot_get_row(self, index);
  return row ? g_object_ref(row) : NULL;
}

gboolean
gnostr_timeline_snapshot_lookup_event(GnostrTimelineSnapshot *self,
                                      const char *event_id,
                                      guint *out_index)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT(self), FALSE);
  g_return_val_if_fail(event_id != NULL, FALSE);

  gpointer value = g_hash_table_lookup(self->event_index, event_id);
  if (!value)
    return FALSE;

  if (out_index)
    *out_index = GPOINTER_TO_UINT(value) - 1;
  return TRUE;
}

double
gnostr_timeline_snapshot_get_row_top(GnostrTimelineSnapshot *self,
                                     guint index)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT(self), 0.0);

  if (index >= self->rows->len)
    return self->total_height;

  return self->prefix_tops[index];
}

double
gnostr_timeline_snapshot_get_row_bottom(GnostrTimelineSnapshot *self,
                                        guint index)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT(self), 0.0);

  if (index >= self->rows->len)
    return self->total_height;

  return self->prefix_tops[index + 1];
}
