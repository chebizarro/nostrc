#include "gnostr-timeline-snapshot-model.h"

struct _GnostrTimelineSnapshotModel {
  GObject parent_instance;

  GnostrTimelineSnapshot *snapshot;
  GPtrArray *published_rows; /* element-type: GnostrTimelineSnapshotRow* */
};

typedef struct {
  guint position;
  guint removed;
  guint added;
} ItemsChangedSpan;

static void gnostr_timeline_snapshot_model_list_model_iface_init(GListModelInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GnostrTimelineSnapshotModel, gnostr_timeline_snapshot_model, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(G_TYPE_LIST_MODEL, gnostr_timeline_snapshot_model_list_model_iface_init))

static GType
gnostr_timeline_snapshot_model_get_item_type(GListModel *model)
{
  (void)model;
  return GNOSTR_TYPE_TIMELINE_SNAPSHOT_ROW;
}

static guint
gnostr_timeline_snapshot_model_get_n_items(GListModel *model)
{
  GnostrTimelineSnapshotModel *self = GNOSTR_TIMELINE_SNAPSHOT_MODEL(model);

  return self->published_rows ? self->published_rows->len : 0;
}

static gpointer
gnostr_timeline_snapshot_model_get_item(GListModel *model,
                                        guint position)
{
  GnostrTimelineSnapshotModel *self = GNOSTR_TIMELINE_SNAPSHOT_MODEL(model);

  if (!self->published_rows || position >= self->published_rows->len)
    return NULL;

  return g_object_ref(g_ptr_array_index(self->published_rows, position));
}

static void
gnostr_timeline_snapshot_model_list_model_iface_init(GListModelInterface *iface)
{
  iface->get_item_type = gnostr_timeline_snapshot_model_get_item_type;
  iface->get_n_items = gnostr_timeline_snapshot_model_get_n_items;
  iface->get_item = gnostr_timeline_snapshot_model_get_item;
}

static void
gnostr_timeline_snapshot_model_finalize(GObject *object)
{
  GnostrTimelineSnapshotModel *self = GNOSTR_TIMELINE_SNAPSHOT_MODEL(object);

  g_clear_object(&self->snapshot);
  g_clear_pointer(&self->published_rows, g_ptr_array_unref);

  G_OBJECT_CLASS(gnostr_timeline_snapshot_model_parent_class)->finalize(object);
}

static void
gnostr_timeline_snapshot_model_class_init(GnostrTimelineSnapshotModelClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->finalize = gnostr_timeline_snapshot_model_finalize;
}

static void
gnostr_timeline_snapshot_model_init(GnostrTimelineSnapshotModel *self)
{
  self->published_rows = g_ptr_array_new_with_free_func(g_object_unref);
}

GnostrTimelineSnapshotModel *
gnostr_timeline_snapshot_model_new(void)
{
  return g_object_new(GNOSTR_TYPE_TIMELINE_SNAPSHOT_MODEL, NULL);
}

GnostrTimelineSnapshot *
gnostr_timeline_snapshot_model_get_snapshot(GnostrTimelineSnapshotModel *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT_MODEL(self), NULL);
  return self->snapshot;
}

GnostrTimelineSnapshot *
gnostr_timeline_snapshot_model_dup_snapshot(GnostrTimelineSnapshotModel *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT_MODEL(self), NULL);
  return self->snapshot ? g_object_ref(self->snapshot) : NULL;
}

static GPtrArray *
snapshot_model_dup_rows(GnostrTimelineSnapshot *snapshot)
{
  GPtrArray *rows = g_ptr_array_new_with_free_func(g_object_unref);
  guint n_rows = snapshot ? gnostr_timeline_snapshot_get_n_rows(snapshot) : 0;

  for (guint i = 0; i < n_rows; i++) {
    GnostrTimelineSnapshotRow *row = gnostr_timeline_snapshot_get_row(snapshot, i);
    if (row)
      g_ptr_array_add(rows, g_object_ref(row));
  }

  return rows;
}

static gboolean
snapshot_model_rows_identical(GPtrArray *old_rows,
                              GPtrArray *new_rows)
{
  if (old_rows->len != new_rows->len)
    return FALSE;

  for (guint i = 0; i < old_rows->len; i++) {
    if (g_ptr_array_index(old_rows, i) != g_ptr_array_index(new_rows, i))
      return FALSE;
  }

  return TRUE;
}

static gboolean
snapshot_model_validate_event_ids(GPtrArray *rows)
{
  GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);

  for (guint i = 0; i < rows->len; i++) {
    GnostrTimelineSnapshotRow *row = g_ptr_array_index(rows, i);
    const char *event_id = row ? gnostr_timeline_snapshot_row_get_event_id(row) : NULL;

    if (!event_id || !*event_id || g_hash_table_contains(seen, event_id)) {
      g_hash_table_destroy(seen);
      return FALSE;
    }

    g_hash_table_add(seen, (gpointer)event_id);
  }

  g_hash_table_destroy(seen);
  return TRUE;
}

static guint
lcs_at(guint *lcs,
       guint n_cols,
       guint row,
       guint col)
{
  return lcs[(row * n_cols) + col];
}

static void
lcs_set(guint *lcs,
        guint n_cols,
        guint row,
        guint col,
        guint value)
{
  lcs[(row * n_cols) + col] = value;
}

static gboolean
snapshot_model_same_event_id(GPtrArray *old_rows,
                             guint old_index,
                             GPtrArray *new_rows,
                             guint new_index)
{
  GnostrTimelineSnapshotRow *old_row = g_ptr_array_index(old_rows, old_index);
  GnostrTimelineSnapshotRow *new_row = g_ptr_array_index(new_rows, new_index);
  return g_strcmp0(gnostr_timeline_snapshot_row_get_event_id(old_row),
                   gnostr_timeline_snapshot_row_get_event_id(new_row)) == 0;
}

static void
snapshot_model_add_span(GArray *spans,
                        guint position,
                        guint removed,
                        guint added)
{
  if (removed == 0 && added == 0)
    return;

  ItemsChangedSpan span = {
    .position = position,
    .removed = removed,
    .added = added,
  };
  g_array_append_val(spans, span);
}

static GArray *
snapshot_model_build_diff_spans(GPtrArray *old_rows,
                                GPtrArray *new_rows)
{
  if (!snapshot_model_validate_event_ids(old_rows) ||
      !snapshot_model_validate_event_ids(new_rows))
    return NULL;

  guint old_len = old_rows->len;
  guint new_len = new_rows->len;
  guint n_cols = new_len + 1;
  guint *lcs = g_new0(guint, (old_len + 1) * (new_len + 1));

  for (gint i = (gint)old_len - 1; i >= 0; i--) {
    for (gint j = (gint)new_len - 1; j >= 0; j--) {
      if (snapshot_model_same_event_id(old_rows, (guint)i, new_rows, (guint)j)) {
        lcs_set(lcs, n_cols, (guint)i, (guint)j,
                lcs_at(lcs, n_cols, (guint)i + 1, (guint)j + 1) + 1);
      } else {
        guint skip_old = lcs_at(lcs, n_cols, (guint)i + 1, (guint)j);
        guint skip_new = lcs_at(lcs, n_cols, (guint)i, (guint)j + 1);
        lcs_set(lcs, n_cols, (guint)i, (guint)j, MAX(skip_old, skip_new));
      }
    }
  }

  GArray *spans = g_array_new(FALSE, FALSE, sizeof(ItemsChangedSpan));
  guint i = 0;
  guint j = 0;
  guint old_start = 0;
  guint new_start = 0;
  guint position = 0;

  while (i < old_len && j < new_len) {
    if (snapshot_model_same_event_id(old_rows, i, new_rows, j)) {
      snapshot_model_add_span(spans, position, i - old_start, j - new_start);
      position += j - new_start;

      GnostrTimelineSnapshotRow *old_row = g_ptr_array_index(old_rows, i);
      GnostrTimelineSnapshotRow *new_row = g_ptr_array_index(new_rows, j);
      if (old_row != new_row)
        snapshot_model_add_span(spans, position, 1, 1);

      i++;
      j++;
      old_start = i;
      new_start = j;
      position++;
    } else if (lcs_at(lcs, n_cols, i + 1, j) >= lcs_at(lcs, n_cols, i, j + 1)) {
      i++;
    } else {
      j++;
    }
  }

  snapshot_model_add_span(spans,
                          position,
                          old_len - old_start,
                          new_len - new_start);

  g_free(lcs);
  return spans;
}

void
gnostr_timeline_snapshot_model_replace_snapshot(GnostrTimelineSnapshotModel *self,
                                                GnostrTimelineSnapshot *snapshot)
{
  g_return_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT_MODEL(self));
  g_return_if_fail(snapshot == NULL || GNOSTR_IS_TIMELINE_SNAPSHOT(snapshot));

  guint old_len = self->published_rows ? self->published_rows->len : 0;
  GPtrArray *new_rows = snapshot_model_dup_rows(snapshot);

  if (snapshot_model_rows_identical(self->published_rows, new_rows)) {
    if (snapshot)
      g_object_ref(snapshot);
    g_clear_object(&self->snapshot);
    self->snapshot = snapshot;
    g_ptr_array_unref(new_rows);
    return;
  }

  g_autoptr(GArray) spans = snapshot_model_build_diff_spans(self->published_rows, new_rows);
  gboolean fallback = spans == NULL;

  if (snapshot)
    g_object_ref(snapshot);
  g_clear_object(&self->snapshot);
  self->snapshot = snapshot;

  g_ptr_array_unref(self->published_rows);
  self->published_rows = new_rows;

  if (fallback) {
    guint new_len = self->published_rows->len;
    if (old_len || new_len)
      g_list_model_items_changed(G_LIST_MODEL(self), 0, old_len, new_len);
    return;
  }

  for (guint i = 0; i < spans->len; i++) {
    ItemsChangedSpan *span = &g_array_index(spans, ItemsChangedSpan, i);
    g_list_model_items_changed(G_LIST_MODEL(self),
                               span->position,
                               span->removed,
                               span->added);
  }
}
