/**
 * NostrGtkTimelineTabs - Tab bar for switching between timeline views
 *
 * A horizontal tab bar that allows switching between different filtered
 * timeline views (Global, Following, Hashtags, etc.).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NOSTR_GTK_TIMELINE_TABS_H
#define NOSTR_GTK_TIMELINE_TABS_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define NOSTR_GTK_TYPE_TIMELINE_TABS (nostr_gtk_timeline_tabs_get_type())
G_DECLARE_FINAL_TYPE(NostrGtkTimelineTabs, nostr_gtk_timeline_tabs, NOSTR_GTK, TIMELINE_TABS, GtkWidget)

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

NostrGtkTimelineTabs *nostr_gtk_timeline_tabs_new(void);

guint nostr_gtk_timeline_tabs_add_tab(NostrGtkTimelineTabs *self,
                                GnTimelineTabType type,
                                const char *label,
                                const char *filter_value);

void nostr_gtk_timeline_tabs_remove_tab(NostrGtkTimelineTabs *self, guint index);

guint nostr_gtk_timeline_tabs_get_selected(NostrGtkTimelineTabs *self);

void nostr_gtk_timeline_tabs_set_selected(NostrGtkTimelineTabs *self, guint index);

GnTimelineTabType nostr_gtk_timeline_tabs_get_tab_type(NostrGtkTimelineTabs *self, guint index);

const char *nostr_gtk_timeline_tabs_get_tab_filter_value(NostrGtkTimelineTabs *self, guint index);

guint nostr_gtk_timeline_tabs_get_n_tabs(NostrGtkTimelineTabs *self);

void nostr_gtk_timeline_tabs_set_closable(NostrGtkTimelineTabs *self, guint index, gboolean closable);

G_END_DECLS

#endif /* NOSTR_GTK_TIMELINE_TABS_H */
