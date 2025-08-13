#ifndef GNOSTR_TIMELINE_VIEW_H
#define GNOSTR_TIMELINE_VIEW_H
#pragma once
#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_TIMELINE_VIEW (gnostr_timeline_view_get_type())
G_DECLARE_FINAL_TYPE(GnostrTimelineView, gnostr_timeline_view, GNOSTR, TIMELINE_VIEW, GtkWidget)

/* Internal model item type (opaque to external code but usable via GType). */
typedef struct _TimelineItem TimelineItem;
GType timeline_item_get_type(void);

GtkWidget *gnostr_timeline_view_new(void);

/* Assign a model (selection model wrapping a list model) to the internal GtkListView. */
void gnostr_timeline_view_set_model(GnostrTimelineView *self, GtkSelectionModel *model);

/* Convenience: ensure a GtkStringList exists and prepend a text row quickly. */
void gnostr_timeline_view_prepend_text(GnostrTimelineView *self, const char *text);

/* New: prepend a structured item with identity/time/depth. */
void gnostr_timeline_view_prepend(GnostrTimelineView *self,
                                  const char *display,
                                  const char *handle,
                                  const char *ts,
                                  const char *content,
                                  guint depth);

/* Set a tree of TimelineItem roots (GListModel of internal items); view flattens via GtkTreeListModel. */
void gnostr_timeline_view_set_tree_roots(GnostrTimelineView *self, GListModel *roots);

/* Helpers for building thread trees from outside the view implementation. */
void gnostr_timeline_item_add_child(TimelineItem *parent, TimelineItem *child);
GListModel *gnostr_timeline_item_get_children(TimelineItem *item);

G_END_DECLS

#endif /* GNOSTR_TIMELINE_VIEW_H */
