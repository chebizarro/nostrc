#ifndef GNOSTR_TIMELINE_HYDRATOR_H
#define GNOSTR_TIMELINE_HYDRATOR_H

#include "gnostr-timeline-batch.h"
#include "gnostr-timeline-item-view-model.h"
#include <gio/gio.h>
#include <glib-object.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_TIMELINE_HYDRATOR (gnostr_timeline_hydrator_get_type())
G_DECLARE_FINAL_TYPE(GnostrTimelineHydrator,
                     gnostr_timeline_hydrator,
                     GNOSTR, TIMELINE_HYDRATOR,
                     GObject)

GnostrTimelineHydrator *gnostr_timeline_hydrator_new(guint64 generation);
void     gnostr_timeline_hydrator_set_generation(GnostrTimelineHydrator *self,
                                                  guint64 generation);
guint64  gnostr_timeline_hydrator_get_generation(GnostrTimelineHydrator *self);

GnostrTimelineItemViewModel *
gnostr_timeline_hydrator_hydrate_entry(GnostrTimelineHydrator *self,
                                       const GnostrTimelineBatchEntry *entry);

GPtrArray *gnostr_timeline_hydrator_hydrate_batch(GnostrTimelineHydrator *self,
                                                  GnostrTimelineBatch *batch);

void gnostr_timeline_hydrator_hydrate_batch_async(GnostrTimelineHydrator *self,
                                                  GnostrTimelineBatch *batch,
                                                  GCancellable *cancellable,
                                                  GAsyncReadyCallback callback,
                                                  gpointer user_data);

GPtrArray *gnostr_timeline_hydrator_hydrate_batch_finish(GnostrTimelineHydrator *self,
                                                         GAsyncResult *result,
                                                         GError **error);

G_END_DECLS

#endif /* GNOSTR_TIMELINE_HYDRATOR_H */
