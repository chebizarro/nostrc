/**
 * gnostr-timeline-view-private.h — Internal struct definitions
 *
 * Exposes GnostrTimelineView and TimelineItem struct layouts for use
 * by app-level factory code that needs direct field access.
 *
 * NOT installed as a public header. Only include from within the
 * nostr-gtk library or tightly-coupled app factory code.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GNOSTR_TIMELINE_VIEW_PRIVATE_H
#define GNOSTR_TIMELINE_VIEW_PRIVATE_H

#include "gnostr-timeline-view.h"
#include <gtk/gtk.h>

G_BEGIN_DECLS

/* ============== TimelineItem ============== */

struct _TimelineItem {
  GObject parent_instance;
  gchar *display_name;
  gchar *handle;
  gchar *timestamp;
  gchar *content;
  guint depth;
  gchar *id;
  gchar *root_id;
  gchar *pubkey;
  gint64 created_at;
  gchar *avatar_url;
  gboolean visible;
  GListStore *children;
  /* NIP-18 repost info */
  gboolean is_repost;
  gchar *reposter_pubkey;
  gchar *reposter_display_name;
  gint64 repost_created_at;
  /* NIP-18 quote repost info */
  gboolean has_quote;
  gchar *quoted_event_id;
  gchar *quoted_content;
  gchar *quoted_author;
};

typedef struct _TimelineItemClass {
  GObjectClass parent_class;
} TimelineItemClass;

/* ============== GnostrTimelineView ============== */

struct _GnostrTimelineView {
  GtkWidget parent_instance;
  GtkWidget *root_box;
  GtkWidget *tabs;
  GtkWidget *root_scroller;
  GtkWidget *list_view;
  GtkSelectionModel *selection_model;
  GListStore *list_model;
  GtkTreeListModel *tree_model;
  GListStore *flattened_model;

  /* Scroll position tracking */
  guint visible_range_start;
  guint visible_range_end;
  gdouble last_scroll_value;
  gint64 last_scroll_time;
  gdouble scroll_velocity;
  gboolean is_fast_scrolling;
  guint scroll_idle_id;

  /* App-level slots: debounced batch metadata loading.
   * The library doesn't use these; factory code may. */
  GPtrArray *pending_metadata_items;
  guint metadata_batch_idle_id;
};

G_END_DECLS

#endif /* GNOSTR_TIMELINE_VIEW_PRIVATE_H */
