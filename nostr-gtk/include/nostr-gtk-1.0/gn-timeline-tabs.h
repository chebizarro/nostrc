/**
 * GnTimelineTabs - Tab bar for switching between timeline views
 *
 * A horizontal tab bar that allows switching between different filtered
 * timeline views (Global, Following, Hashtags, etc.).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
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

GnTimelineTabs *gn_timeline_tabs_new(void);

guint gn_timeline_tabs_add_tab(GnTimelineTabs *self,
                                GnTimelineTabType type,
                                const char *label,
                                const char *filter_value);

void gn_timeline_tabs_remove_tab(GnTimelineTabs *self, guint index);

guint gn_timeline_tabs_get_selected(GnTimelineTabs *self);

void gn_timeline_tabs_set_selected(GnTimelineTabs *self, guint index);

GnTimelineTabType gn_timeline_tabs_get_tab_type(GnTimelineTabs *self, guint index);

const char *gn_timeline_tabs_get_tab_filter_value(GnTimelineTabs *self, guint index);

guint gn_timeline_tabs_get_n_tabs(GnTimelineTabs *self);

void gn_timeline_tabs_set_closable(GnTimelineTabs *self, guint index, gboolean closable);

G_END_DECLS

#endif /* GN_TIMELINE_TABS_H */
