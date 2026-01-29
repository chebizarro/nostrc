/**
 * GnTimelineTabs - Tab bar for switching between timeline views
 *
 * A horizontal tab bar that allows switching between different filtered
 * timeline views (Global, Following, Hashtags, etc.).
 *
 * Part of the Timeline Architecture Refactor (nostrc-e03f)
 */

#ifndef GN_TIMELINE_TABS_H
#define GN_TIMELINE_TABS_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GN_TYPE_TIMELINE_TABS (gn_timeline_tabs_get_type())
G_DECLARE_FINAL_TYPE(GnTimelineTabs, gn_timeline_tabs, GN, TIMELINE_TABS, GtkWidget)

/**
 * GnTimelineTabType:
 * @GN_TIMELINE_TAB_GLOBAL: Global timeline (all notes)
 * @GN_TIMELINE_TAB_FOLLOWING: Notes from followed users
 * @GN_TIMELINE_TAB_HASHTAG: Notes with a specific hashtag
 * @GN_TIMELINE_TAB_AUTHOR: Notes from a specific author
 * @GN_TIMELINE_TAB_CUSTOM: Custom filter
 */
typedef enum {
  GN_TIMELINE_TAB_GLOBAL,
  GN_TIMELINE_TAB_FOLLOWING,
  GN_TIMELINE_TAB_HASHTAG,
  GN_TIMELINE_TAB_AUTHOR,
  GN_TIMELINE_TAB_CUSTOM,
} GnTimelineTabType;

/**
 * gn_timeline_tabs_new:
 *
 * Create a new timeline tabs widget.
 *
 * Returns: (transfer full): A new timeline tabs widget
 */
GnTimelineTabs *gn_timeline_tabs_new(void);

/**
 * gn_timeline_tabs_add_tab:
 * @self: The tabs widget
 * @type: The type of tab
 * @label: Display label for the tab
 * @filter_value: (nullable): Filter value (hashtag, pubkey, etc.)
 *
 * Add a new tab to the tab bar.
 *
 * Returns: The index of the new tab
 */
guint gn_timeline_tabs_add_tab(GnTimelineTabs *self,
                                GnTimelineTabType type,
                                const char *label,
                                const char *filter_value);

/**
 * gn_timeline_tabs_remove_tab:
 * @self: The tabs widget
 * @index: Index of the tab to remove
 *
 * Remove a tab from the tab bar.
 */
void gn_timeline_tabs_remove_tab(GnTimelineTabs *self, guint index);

/**
 * gn_timeline_tabs_get_selected:
 * @self: The tabs widget
 *
 * Get the index of the currently selected tab.
 *
 * Returns: The selected tab index
 */
guint gn_timeline_tabs_get_selected(GnTimelineTabs *self);

/**
 * gn_timeline_tabs_set_selected:
 * @self: The tabs widget
 * @index: Index of the tab to select
 *
 * Select a tab by index.
 */
void gn_timeline_tabs_set_selected(GnTimelineTabs *self, guint index);

/**
 * gn_timeline_tabs_get_tab_type:
 * @self: The tabs widget
 * @index: Tab index
 *
 * Get the type of a tab.
 *
 * Returns: The tab type
 */
GnTimelineTabType gn_timeline_tabs_get_tab_type(GnTimelineTabs *self, guint index);

/**
 * gn_timeline_tabs_get_tab_filter_value:
 * @self: The tabs widget
 * @index: Tab index
 *
 * Get the filter value of a tab (hashtag, pubkey, etc.).
 *
 * Returns: (transfer none) (nullable): The filter value
 */
const char *gn_timeline_tabs_get_tab_filter_value(GnTimelineTabs *self, guint index);

/**
 * gn_timeline_tabs_get_n_tabs:
 * @self: The tabs widget
 *
 * Get the number of tabs.
 *
 * Returns: The number of tabs
 */
guint gn_timeline_tabs_get_n_tabs(GnTimelineTabs *self);

/**
 * gn_timeline_tabs_set_closable:
 * @self: The tabs widget
 * @index: Tab index
 * @closable: Whether the tab can be closed
 *
 * Set whether a tab can be closed by the user.
 */
void gn_timeline_tabs_set_closable(GnTimelineTabs *self, guint index, gboolean closable);

G_END_DECLS

#endif /* GN_TIMELINE_TABS_H */
