#ifndef GNOSTR_TIMELINE_VIEW_H
#define GNOSTR_TIMELINE_VIEW_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_TIMELINE_VIEW (gnostr_timeline_view_get_type())

G_DECLARE_FINAL_TYPE(GnostrTimelineView, gnostr_timeline_view, GNOSTR, TIMELINE_VIEW, GtkWidget)

GtkWidget *gnostr_timeline_view_new(void);

/* Assign a model (selection model wrapping a list model) to the internal GtkListView. */
void gnostr_timeline_view_set_model(GnostrTimelineView *self, GtkSelectionModel *model);

/* Convenience: ensure a GtkStringList exists and prepend a text row quickly. */
void gnostr_timeline_view_prepend_text(GnostrTimelineView *self, const char *text);

G_END_DECLS

#endif /* GNOSTR_TIMELINE_VIEW_H */
