/**
 * NostrGtkTimelineView - Scrollable timeline widget for nostr events
 *
 * A universal nostr UI component that provides a scrollable list view
 * with optional tabs, scroll tracking, and tree model support.
 *
 * The widget does NOT create a factory internally. Consumers must provide
 * a GtkListItemFactory via nostr_gtk_timeline_view_set_factory() to control
 * how items are rendered.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NOSTR_GTK_TIMELINE_VIEW_H
#define NOSTR_GTK_TIMELINE_VIEW_H

#include <gtk/gtk.h>
#include "gn-timeline-tabs.h"

G_BEGIN_DECLS

#define NOSTR_GTK_TYPE_TIMELINE_VIEW (nostr_gtk_timeline_view_get_type())
G_DECLARE_FINAL_TYPE(NostrGtkTimelineView, nostr_gtk_timeline_view, NOSTR_GTK, TIMELINE_VIEW, GtkWidget)

/* Internal model item type (opaque to external code but usable via GType). */
typedef struct _TimelineItem TimelineItem;
GType timeline_item_get_type(void);

GtkWidget *nostr_gtk_timeline_view_new(void);

/**
 * nostr_gtk_timeline_view_set_factory:
 * @self: The timeline view
 * @factory: (transfer none): A GtkListItemFactory to use for rendering items
 *
 * Set the factory used to create and bind list item widgets.
 * This must be called before any items will be rendered.
 */
void nostr_gtk_timeline_view_set_factory(NostrGtkTimelineView *self, GtkListItemFactory *factory);

/* Assign a model (selection model wrapping a list model) to the internal GtkListView. */
void nostr_gtk_timeline_view_set_model(NostrGtkTimelineView *self, GtkSelectionModel *model);

/* Convenience: ensure a GtkStringList exists and prepend a text row quickly. */
void nostr_gtk_timeline_view_prepend_text(NostrGtkTimelineView *self, const char *text);

/* Prepend a structured item with identity/time/depth. */
void nostr_gtk_timeline_view_prepend(NostrGtkTimelineView *self,
                                   const char *display,
                                   const char *handle,
                                   const char *ts,
                                   const char *content,
                                   guint depth);

/* Set a tree of TimelineItem roots (GListModel of internal items); view flattens via GtkTreeListModel. */
void nostr_gtk_timeline_view_set_tree_roots(NostrGtkTimelineView *self, GListModel *roots);

/* Helpers for building thread trees from outside the view implementation. */
void nostr_gtk_timeline_item_add_child(TimelineItem *parent, TimelineItem *child);
GListModel *nostr_gtk_timeline_item_get_children(TimelineItem *item);

/* Get the internal scrolled window for scroll position monitoring */
GtkWidget *nostr_gtk_timeline_view_get_scrolled_window(NostrGtkTimelineView *self);

/* Get the internal GtkListView for direct scroll operations */
GtkWidget *nostr_gtk_timeline_view_get_list_view(NostrGtkTimelineView *self);

/* Get the timeline tabs widget */
NostrGtkTimelineTabs *nostr_gtk_timeline_view_get_tabs(NostrGtkTimelineView *self);

/* Show/hide the timeline tabs bar */
void nostr_gtk_timeline_view_set_tabs_visible(NostrGtkTimelineView *self, gboolean visible);

/* Add a hashtag tab and switch to it */
void nostr_gtk_timeline_view_add_hashtag_tab(NostrGtkTimelineView *self, const char *hashtag);

/* Add an author tab and switch to it */
void nostr_gtk_timeline_view_add_author_tab(NostrGtkTimelineView *self, const char *pubkey_hex, const char *display_name);

/**
 * Signals:
 * - "tab-filter-changed": Emitted when the active tab changes.
 *   Handler signature: void handler(NostrGtkTimelineView *view, GnTimelineTabType type, const char *filter_value, gpointer user_data)
 */

/* Scroll position tracking */

gboolean nostr_gtk_timeline_view_get_visible_range(NostrGtkTimelineView *self,
                                                  guint *start,
                                                  guint *end);

gboolean nostr_gtk_timeline_view_is_item_visible(NostrGtkTimelineView *self, guint index);

gboolean nostr_gtk_timeline_view_is_fast_scrolling(NostrGtkTimelineView *self);

gdouble nostr_gtk_timeline_view_get_scroll_velocity(NostrGtkTimelineView *self);

G_END_DECLS

#endif /* NOSTR_GTK_TIMELINE_VIEW_H */
