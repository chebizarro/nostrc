#include "gnostr-timeline-snapshot-model.h"

struct _GnostrTimelineSnapshotModel {
  GObject parent_instance;

  GnostrTimelineSnapshot *snapshot;
};

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

  return self->snapshot ? gnostr_timeline_snapshot_get_n_rows(self->snapshot) : 0;
}

static gpointer
gnostr_timeline_snapshot_model_get_item(GListModel *model,
                                        guint position)
{
  GnostrTimelineSnapshotModel *self = GNOSTR_TIMELINE_SNAPSHOT_MODEL(model);

  if (!self->snapshot)
    return NULL;

  return gnostr_timeline_snapshot_dup_row(self->snapshot, position);
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
  (void)self;
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

void
gnostr_timeline_snapshot_model_replace_snapshot(GnostrTimelineSnapshotModel *self,
                                                GnostrTimelineSnapshot *snapshot)
{
  g_return_if_fail(GNOSTR_IS_TIMELINE_SNAPSHOT_MODEL(self));
  g_return_if_fail(snapshot == NULL || GNOSTR_IS_TIMELINE_SNAPSHOT(snapshot));

  guint old_len = self->snapshot ? gnostr_timeline_snapshot_get_n_rows(self->snapshot) : 0;
  guint new_len = snapshot ? gnostr_timeline_snapshot_get_n_rows(snapshot) : 0;

  if (snapshot)
    g_object_ref(snapshot);
  g_clear_object(&self->snapshot);
  self->snapshot = snapshot;

  if (old_len || new_len)
    g_list_model_items_changed(G_LIST_MODEL(self), 0, old_len, new_len);
}
