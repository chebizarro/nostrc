/**
 * GnTimelineView - Timeline display widget
 *
 * A GTK widget that wraps GtkListView with efficient factory and scroll
 * handling for displaying timeline content. Supports the "new notes"
 * indicator and smooth scrolling.
 *
 * Part of the Timeline Architecture Refactor (nostrc-e03f)
 */

#ifndef GN_TIMELINE_VIEW_H
#define GN_TIMELINE_VIEW_H

#include <gtk/gtk.h>
#include "../model/gn-timeline-model.h"

G_BEGIN_DECLS

#define GN_TYPE_TIMELINE_VIEW (gn_timeline_view_get_type())
G_DECLARE_FINAL_TYPE(GnTimelineView, gn_timeline_view, GN, TIMELINE_VIEW, GtkWidget)

/**
 * gn_timeline_view_new:
 *
 * Create a new timeline view widget.
 *
 * Returns: (transfer full): A new timeline view
 */
GnTimelineView *gn_timeline_view_new(void);

/**
 * gn_timeline_view_new_with_model:
 * @model: (transfer none): The timeline model to display
 *
 * Create a new timeline view with a model.
 *
 * Returns: (transfer full): A new timeline view
 */
GnTimelineView *gn_timeline_view_new_with_model(GnTimelineModel *model);

/* ============== Model Management ============== */

/**
 * gn_timeline_view_set_model:
 * @self: The view
 * @model: (transfer none) (nullable): The model to display
 *
 * Set the timeline model to display.
 */
void gn_timeline_view_set_model(GnTimelineView *self, GnTimelineModel *model);

/**
 * gn_timeline_view_get_model:
 * @self: The view
 *
 * Get the current timeline model.
 *
 * Returns: (transfer none) (nullable): The model
 */
GnTimelineModel *gn_timeline_view_get_model(GnTimelineView *self);

/* ============== Scroll Control ============== */

/**
 * gn_timeline_view_scroll_to_top:
 * @self: The view
 *
 * Scroll to the top of the timeline.
 */
void gn_timeline_view_scroll_to_top(GnTimelineView *self);

/**
 * gn_timeline_view_scroll_to_position:
 * @self: The view
 * @position: Position to scroll to
 *
 * Scroll to a specific position in the timeline.
 */
void gn_timeline_view_scroll_to_position(GnTimelineView *self, guint position);

/**
 * gn_timeline_view_is_at_top:
 * @self: The view
 *
 * Check if the view is scrolled to the top.
 *
 * Returns: TRUE if at top
 */
gboolean gn_timeline_view_is_at_top(GnTimelineView *self);

/* ============== New Notes Indicator ============== */

/**
 * gn_timeline_view_show_new_notes_indicator:
 * @self: The view
 * @count: Number of new notes
 *
 * Show the "N new notes" indicator.
 */
void gn_timeline_view_show_new_notes_indicator(GnTimelineView *self, guint count);

/**
 * gn_timeline_view_hide_new_notes_indicator:
 * @self: The view
 *
 * Hide the new notes indicator.
 */
void gn_timeline_view_hide_new_notes_indicator(GnTimelineView *self);

/* ============== Loading State ============== */

/**
 * gn_timeline_view_set_loading:
 * @self: The view
 * @loading: Whether loading is in progress
 *
 * Set the loading state (shows spinner at bottom).
 */
void gn_timeline_view_set_loading(GnTimelineView *self, gboolean loading);

/**
 * gn_timeline_view_set_empty_message:
 * @self: The view
 * @message: (nullable): Message to show when empty
 *
 * Set the message to display when the timeline is empty.
 */
void gn_timeline_view_set_empty_message(GnTimelineView *self, const char *message);

G_END_DECLS

#endif /* GN_TIMELINE_VIEW_H */
