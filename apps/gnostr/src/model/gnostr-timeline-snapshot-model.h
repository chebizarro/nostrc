#ifndef GNOSTR_TIMELINE_SNAPSHOT_MODEL_H
#define GNOSTR_TIMELINE_SNAPSHOT_MODEL_H

#include <gio/gio.h>
#include "gnostr-timeline-snapshot.h"

G_BEGIN_DECLS

#define GNOSTR_TYPE_TIMELINE_SNAPSHOT_MODEL (gnostr_timeline_snapshot_model_get_type())
G_DECLARE_FINAL_TYPE(GnostrTimelineSnapshotModel, gnostr_timeline_snapshot_model, GNOSTR, TIMELINE_SNAPSHOT_MODEL, GObject)

GnostrTimelineSnapshotModel *gnostr_timeline_snapshot_model_new(void);

GnostrTimelineSnapshot *gnostr_timeline_snapshot_model_get_snapshot(GnostrTimelineSnapshotModel *self);
GnostrTimelineSnapshot *gnostr_timeline_snapshot_model_dup_snapshot(GnostrTimelineSnapshotModel *self);
void gnostr_timeline_snapshot_model_replace_snapshot(GnostrTimelineSnapshotModel *self,
                                                     GnostrTimelineSnapshot *snapshot);

G_END_DECLS

#endif /* GNOSTR_TIMELINE_SNAPSHOT_MODEL_H */
