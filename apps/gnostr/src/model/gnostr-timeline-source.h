#ifndef GNOSTR_TIMELINE_SOURCE_H
#define GNOSTR_TIMELINE_SOURCE_H

#include "gnostr-timeline-batch.h"
#include <nostr-gobject-1.0/gn-timeline-query.h>
#include <glib-object.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_TIMELINE_SOURCE (gnostr_timeline_source_get_type())
G_DECLARE_FINAL_TYPE(GnostrTimelineSource, gnostr_timeline_source, GNOSTR, TIMELINE_SOURCE, GObject)

GnostrTimelineSource *gnostr_timeline_source_new(void);
GnostrTimelineSource *gnostr_timeline_source_new_with_query(GNostrTimelineQuery *query);

void gnostr_timeline_source_set_query(GnostrTimelineSource *self,
                                      GNostrTimelineQuery *query);
GNostrTimelineQuery *gnostr_timeline_source_get_query(GnostrTimelineSource *self);
guint64 gnostr_timeline_source_get_generation(GnostrTimelineSource *self);

void gnostr_timeline_source_refresh_async(GnostrTimelineSource *self);
void gnostr_timeline_source_load_older_async(GnostrTimelineSource *self,
                                             guint count,
                                             gint64 before_timestamp);
void gnostr_timeline_source_load_newer_async(GnostrTimelineSource *self,
                                             guint count,
                                             gint64 after_timestamp);

G_END_DECLS

#endif /* GNOSTR_TIMELINE_SOURCE_H */
