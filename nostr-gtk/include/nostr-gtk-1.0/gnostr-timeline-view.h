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

#include "nostr-gtk-export.h"
#include <gtk/gtk.h>
#include "gn-timeline-tabs.h"

G_BEGIN_DECLS

#define NOSTR_GTK_TYPE_TIMELINE_VIEW (nostr_gtk_timeline_view_get_type())
NOSTR_GTK_API G_DECLARE_FINAL_TYPE(NostrGtkTimelineView, nostr_gtk_timeline_view, NOSTR_GTK, TIMELINE_VIEW, GtkWidget)

/* Internal model item type (opaque to external code but usable via GType). */
typedef struct _TimelineItem TimelineItem;
NOSTR_GTK_API GType timeline_item_get_type(void);

NOSTR_GTK_API GtkWidget *nostr_gtk_timeline_view_new(void);

/**
 * nostr_gtk_timeline_view_set_factory:
 * @self: The timeline view
 * @factory: (transfer none): A GtkListItemFactory to use for rendering items
 *
 * Set the factory used to create and bind list item widgets.
 * This must be called before any items will be rendered.
 */
NOSTR_GTK_API void nostr_gtk_timeline_view_set_factory(NostrGtkTimelineView *self, GtkListItemFactory *factory);

/* Assign a model (selection model wrapping a list model) to the internal GtkListView. */
NOSTR_GTK_API void nostr_gtk_timeline_view_set_model(NostrGtkTimelineView *self, GtkSelectionModel *model);

/* Convenience: ensure a GtkStringList exists and prepend a text row quickly. */
NOSTR_GTK_API void nostr_gtk_timeline_view_prepend_text(NostrGtkTimelineView *self, const char *text);

/* Prepend a structured item with identity/time/depth. */
NOSTR_GTK_API void nostr_gtk_timeline_view_prepend(NostrGtkTimelineView *self,
                                   const char *display,
                                   const char *handle,
                                   const char *ts,
                                   const char *content,
                                   guint depth);

/* Set a tree of TimelineItem roots (GListModel of internal items); the view flattens
 * roots and their children into the internal list store. */
NOSTR_GTK_API void nostr_gtk_timeline_view_set_tree_roots(NostrGtkTimelineView *self, GListModel *roots);

/* Helpers for building thread trees from outside the view implementation. */
NOSTR_GTK_API void nostr_gtk_timeline_item_add_child(TimelineItem *parent, TimelineItem *child);
NOSTR_GTK_API GListModel *nostr_gtk_timeline_item_get_children(TimelineItem *item);

/* Get the internal scrolled window for scroll position monitoring */
NOSTR_GTK_API GtkWidget *nostr_gtk_timeline_view_get_scrolled_window(NostrGtkTimelineView *self);

/* Get the internal GtkListView for direct scroll operations */
NOSTR_GTK_API GtkWidget *nostr_gtk_timeline_view_get_list_view(NostrGtkTimelineView *self);

/* Get the timeline tabs widget */
NOSTR_GTK_API NostrGtkTimelineTabs *nostr_gtk_timeline_view_get_tabs(NostrGtkTimelineView *self);

/* Show/hide the timeline tabs bar */
NOSTR_GTK_API void nostr_gtk_timeline_view_set_tabs_visible(NostrGtkTimelineView *self, gboolean visible);

/* Add a hashtag tab and switch to it */
NOSTR_GTK_API void nostr_gtk_timeline_view_add_hashtag_tab(NostrGtkTimelineView *self, const char *hashtag);

/* Add an author tab and switch to it */
NOSTR_GTK_API void nostr_gtk_timeline_view_add_author_tab(NostrGtkTimelineView *self, const char *pubkey_hex, const char *display_name);

/**
 * Signals:
 * - "tab-filter-changed": Emitted when the active tab changes.
 *   Handler signature: void handler(NostrGtkTimelineView *view, GnTimelineTabType type, const char *filter_value, gpointer user_data)
 */

/* Scroll position tracking */

NOSTR_GTK_API gboolean nostr_gtk_timeline_view_get_visible_range(NostrGtkTimelineView *self,
                                                  guint *start,
                                                  guint *end);

NOSTR_GTK_API gboolean nostr_gtk_timeline_view_is_item_visible(NostrGtkTimelineView *self, guint index);

NOSTR_GTK_API gboolean nostr_gtk_timeline_view_is_fast_scrolling(NostrGtkTimelineView *self);

NOSTR_GTK_API gdouble nostr_gtk_timeline_view_get_scroll_velocity(NostrGtkTimelineView *self);

/**
 * nostr_gtk_timeline_view_begin_programmatic_scroll:
 * @self: The timeline view
 *
 * Suppress scroll-velocity tracking while the caller changes the scroll
 * position programmatically (e.g. controller-driven scroll restores), so
 * the jump is not mistaken for fast user scrolling. Must be paired with
 * nostr_gtk_timeline_view_end_programmatic_scroll(); pairs must not nest.
 */
NOSTR_GTK_API void nostr_gtk_timeline_view_begin_programmatic_scroll(NostrGtkTimelineView *self);

/**
 * nostr_gtk_timeline_view_end_programmatic_scroll:
 * @self: The timeline view
 *
 * Re-enable scroll-velocity tracking after a programmatic scroll and
 * re-sync the tracker to the current position.
 */
NOSTR_GTK_API void nostr_gtk_timeline_view_end_programmatic_scroll(NostrGtkTimelineView *self);

G_END_DECLS

#endif /* NOSTR_GTK_TIMELINE_VIEW_H */
