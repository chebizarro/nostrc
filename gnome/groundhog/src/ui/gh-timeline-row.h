#ifndef GH_TIMELINE_ROW_H
#define GH_TIMELINE_ROW_H

#include "gh-conversation-view.h"
#include "gh-message-row.h"

G_BEGIN_DECLS

/*
 * GhTimelineRow (data/ui/gh-timeline-row.blp): the widget of one entry of a
 * conversation's timeline (GhTimelineItem, gh-conversation-view.h), the
 * child of each item of the view's list (gh-message-list-item.blp). A
 * message shows as its GhMessageRow; a local event (the disappearing timer's
 * change, charter §3.7, nostrc-qp24.83) as a centred caption. Properties:
 * "item" (the GhTimelineItem; a GObject so a list item's item binds to it
 * directly) and "summary" (read-only): the entry's accessible text, the
 * message row's summary or the event's text and time.
 */
#define GH_TYPE_TIMELINE_ROW (gh_timeline_row_get_type())
G_DECLARE_FINAL_TYPE(GhTimelineRow, gh_timeline_row, GH, TIMELINE_ROW, GtkWidget)

GtkWidget *gh_timeline_row_new(void);
GhTimelineItem *gh_timeline_row_get_item(GhTimelineRow *self);
void gh_timeline_row_set_item(GhTimelineRow *self, GhTimelineItem *item);
const gchar *gh_timeline_row_get_summary(GhTimelineRow *self);
/* The message's row (shown only for a message entry). */
GhMessageRow *gh_timeline_row_get_message_row(GhTimelineRow *self);

G_END_DECLS
#endif
