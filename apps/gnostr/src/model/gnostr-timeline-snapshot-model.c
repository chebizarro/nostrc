#include "gnostr-timeline-snapshot-model.h"

struct _GnostrTimelineSnapshotModel {
  GObject parent_instance;

  GnostrTimelineSnapshot *snapshot;
  GPtrArray *published_rows; /* element-type: GnostrTimelineSnapshotRow* */
  guint64 last_diff_work;
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
                              GPtrArray *new_rows,
                              guint64 *work)
{
  if (old_rows->len != new_rows->len)
    return FALSE;

  for (guint i = 0; i < old_rows->len; i++) {
    (*work)++;
    if (g_ptr_array_index(old_rows, i) != g_ptr_array_index(new_rows, i))
      return FALSE;
  }

  return TRUE;
}

typedef enum {
  SNAPSHOT_ROWS_VALID,
  SNAPSHOT_ROWS_INVALID_KEY,
  SNAPSHOT_ROWS_UNSORTED,
} SnapshotRowsValidation;

static SnapshotRowsValidation
snapshot_model_validate_rows(GPtrArray *rows,
                             guint64 *work)
{
  GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);

  for (guint i = 0; i < rows->len; i++) {
    GnostrTimelineSnapshotRow *row = g_ptr_array_index(rows, i);
    const char *event_id = row ? gnostr_timeline_snapshot_row_get_event_id(row) : NULL;
    (*work)++;

    if (!event_id || !*event_id || g_hash_table_contains(seen, event_id)) {
      g_hash_table_destroy(seen);
      return SNAPSHOT_ROWS_INVALID_KEY;
    }

    if (i > 0) {
      GnostrTimelineSnapshotRow *previous = g_ptr_array_index(rows, i - 1);
      (*work)++;
      if (gnostr_timeline_snapshot_compare_rows(previous, row) > 0) {
        g_hash_table_destroy(seen);
        return SNAPSHOT_ROWS_UNSORTED;
      }
    }

    g_hash_table_add(seen, (gpointer)event_id);
  }

  g_hash_table_destroy(seen);
  return SNAPSHOT_ROWS_VALID;
}

static gboolean
snapshot_model_same_event_id(GnostrTimelineSnapshotRow *old_row,
                             GnostrTimelineSnapshotRow *new_row,
                             guint64 *work)
{
  (*work)++;
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
                                GPtrArray *new_rows,
                                guint64 *work)
{
  SnapshotRowsValidation old_validation = snapshot_model_validate_rows(old_rows, work);
  SnapshotRowsValidation new_validation = snapshot_model_validate_rows(new_rows, work);
  if (old_validation != SNAPSHOT_ROWS_VALID ||
      new_validation != SNAPSHOT_ROWS_VALID)
    return NULL;

  const guint old_len = old_rows->len;
  const guint new_len = new_rows->len;
  const guint common_len = MIN(old_len, new_len);
  guint prefix = 0;

  while (prefix < common_len) {
    (*work)++;
    if (g_ptr_array_index(old_rows, prefix) != g_ptr_array_index(new_rows, prefix))
      break;
    prefix++;
  }

  guint old_end = old_len;
  guint new_end = new_len;
  while (old_end > prefix && new_end > prefix) {
    (*work)++;
    if (g_ptr_array_index(old_rows, old_end - 1) !=
        g_ptr_array_index(new_rows, new_end - 1))
      break;
    old_end--;
    new_end--;
  }

  GArray *spans = g_array_new(FALSE, FALSE, sizeof(ItemsChangedSpan));
  guint old_index = prefix;
  guint new_index = prefix;
  guint position = prefix;
  guint span_position = prefix;
  guint removed = 0;
  guint added = 0;

  while (old_index < old_end && new_index < new_end) {
    GnostrTimelineSnapshotRow *old_row = g_ptr_array_index(old_rows, old_index);
    GnostrTimelineSnapshotRow *new_row = g_ptr_array_index(new_rows, new_index);

    if (snapshot_model_same_event_id(old_row, new_row, work)) {
      if (old_row == new_row) {
        snapshot_model_add_span(spans, span_position, removed, added);
        removed = 0;
        added = 0;
        old_index++;
        new_index++;
        position++;
        continue;
      }

      if (removed == 0 && added == 0)
        span_position = position;
      removed++;
      added++;
      old_index++;
      new_index++;
      position++;
      continue;
    }

    (*work)++;
    gint order = gnostr_timeline_snapshot_compare_rows(old_row, new_row);
    if (order < 0) {
      if (removed == 0 && added == 0)
        span_position = position;
      removed++;
      old_index++;
    } else if (order > 0) {
      if (removed == 0 && added == 0)
        span_position = position;
      added++;
      new_index++;
      position++;
    } else {
      g_array_unref(spans);
      return NULL;
    }
  }

  if (old_index < old_end) {
    if (removed == 0 && added == 0)
      span_position = position;
    removed += old_end - old_index;
  }
  if (new_index < new_end) {
    if (removed == 0 && added == 0)
      span_position = position;
    added += new_end - new_index;
  }
  snapshot_model_add_span(spans, span_position, removed, added);

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

  self->last_diff_work = 0;
  if (snapshot_model_rows_identical(self->published_rows,
                                    new_rows,
                                    &self->last_diff_work)) {
    if (snapshot)
      g_object_ref(snapshot);
    g_clear_object(&self->snapshot);
    self->snapshot = snapshot;
    g_ptr_array_unref(new_rows);
    return;
  }

  g_autoptr(GArray) spans =
    snapshot_model_build_diff_spans(self->published_rows,
                                    new_rows,
                                    &self->last_diff_work);

  if (snapshot)
    g_object_ref(snapshot);
  g_clear_object(&self->snapshot);
  self->snapshot = snapshot;

  if (spans == NULL) {
    /* Fallback: full replacement in one consistent emission. */
    g_ptr_array_unref(self->published_rows);
    self->published_rows = new_rows;
    guint new_len = self->published_rows->len;
    if (old_len || new_len)
      g_list_model_items_changed(G_LIST_MODEL(self), 0, old_len, new_len);
    return;
  }

  /* Apply each diff span to published_rows BEFORE emitting it. The
   * GListModel contract requires the model to reflect exactly the
   * announced change at every items_changed emission; swapping to the
   * final array up-front and then emitting all spans leaves the model
   * ahead of consumers mid-sequence, which corrupts GTK's list item
   * manager bookkeeping (crash in gtk_list_item_manager_ensure_items).
   * Span positions are in new-array coordinates and ascend, so the
   * prefix of published_rows is already in sync when each is applied. */
  for (guint i = 0; i < spans->len; i++) {
    ItemsChangedSpan *span = &g_array_index(spans, ItemsChangedSpan, i);
    if (span->position > self->published_rows->len ||
        span->removed > self->published_rows->len - span->position ||
        (span->added > 0 && span->position + span->added > new_rows->len))
      break; /* malformed span — defensive resync below repairs the model */
    if (span->removed > 0)
      g_ptr_array_remove_range(self->published_rows, span->position, span->removed);
    for (guint k = 0; k < span->added; k++) {
      GnostrTimelineSnapshotRow *row = g_ptr_array_index(new_rows, span->position + k);
      g_ptr_array_insert(self->published_rows, (gint)(span->position + k), g_object_ref(row));
    }
    g_list_model_items_changed(G_LIST_MODEL(self),
                               span->position,
                               span->removed,
                               span->added);
  }

  /* Defensive resync: if the diff ever diverges from the target rows,
   * publish a full replacement rather than leaving the model wrong. */
  if (!snapshot_model_rows_identical(self->published_rows,
                                      new_rows,
                                      &self->last_diff_work)) {
    guint cur_len = self->published_rows->len;
    g_ptr_array_unref(self->published_rows);
    self->published_rows = new_rows;
    g_list_model_items_changed(G_LIST_MODEL(self), 0, cur_len, new_rows->len);
    return;
  }

  g_ptr_array_unref(new_rows);
}

#ifdef GNOSTR_TIMELINE_SNAPSHOT_MODEL_TESTING
guint64
gnostr_timeline_snapshot_model_get_last_diff_work(GnostrTimelineSnapshotModel *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT_MODEL(self), 0);
  return self->last_diff_work;
}
#endif
