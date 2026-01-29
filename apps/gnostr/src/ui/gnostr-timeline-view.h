#ifndef GNOSTR_TIMELINE_VIEW_H
#define GNOSTR_TIMELINE_VIEW_H

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

/* Centralized avatar cache API */
#include "gnostr-avatar-cache.h"

/* Get the internal scrolled window for scroll position monitoring */
GtkWidget *gnostr_timeline_view_get_scrolled_window(GnostrTimelineView *self);

/* Timeline tabs support (Phase 3) */
#include "gn-timeline-tabs.h"

/* Get the timeline tabs widget */
GnTimelineTabs *gnostr_timeline_view_get_tabs(GnostrTimelineView *self);

/* Show/hide the timeline tabs bar */
void gnostr_timeline_view_set_tabs_visible(GnostrTimelineView *self, gboolean visible);

/* Add a hashtag tab and switch to it */
void gnostr_timeline_view_add_hashtag_tab(GnostrTimelineView *self, const char *hashtag);

/* Add an author tab and switch to it */
void gnostr_timeline_view_add_author_tab(GnostrTimelineView *self, const char *pubkey_hex, const char *display_name);

G_END_DECLS

#endif /* GNOSTR_TIMELINE_VIEW_H */
