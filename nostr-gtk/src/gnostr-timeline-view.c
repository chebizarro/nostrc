/**
 * NostrGtkTimelineView - Core scrollable timeline widget
 *
 * Provides a scrollable GtkListView with optional tab filtering,
 * scroll position tracking, and tree model support.
 *
 * This is the library version: no factory is created internally.
 * Consumers must call nostr_gtk_timeline_view_set_factory() to provide
 * a GtkListItemFactory for rendering items.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "gnostr-timeline-view"

#include "gnostr-timeline-view.h"
#include "gnostr-timeline-view-private.h"
#include "gn-timeline-tabs.h"
#include <nostr-gobject-1.0/nostr_utils.h>
#include <string.h>

#define UI_RESOURCE "/org/nostr/gtk/ui/widgets/gnostr-timeline-view.ui"

/* ============== TimelineItem GObject ============== */

G_DEFINE_TYPE(TimelineItem, timeline_item, G_TYPE_OBJECT)

enum {
  PROP_TI_0,
  PROP_DISPLAY_NAME,
  PROP_HANDLE,
  PROP_TIMESTAMP,
  PROP_CONTENT,
  PROP_DEPTH,
  PROP_ID,
  PROP_ROOT_ID,
  PROP_PUBKEY,
  PROP_CREATED_AT,
  PROP_AVATAR_URL,
  PROP_VISIBLE,
  N_TI_PROPS
};

static GParamSpec *ti_props[N_TI_PROPS];

static void timeline_item_set_property(GObject *obj, guint prop_id, const GValue *value, GParamSpec *pspec) {
  TimelineItem *self = (TimelineItem*)obj;
  switch (prop_id) {
    case PROP_DISPLAY_NAME: g_free(self->display_name); self->display_name = g_value_dup_string(value); break;
    case PROP_HANDLE:       g_free(self->handle);       self->handle       = g_value_dup_string(value); break;
    case PROP_TIMESTAMP:    g_free(self->timestamp);    self->timestamp    = g_value_dup_string(value); break;
    case PROP_CONTENT:      g_free(self->content);      self->content      = g_value_dup_string(value); break;
    case PROP_DEPTH:        self->depth = g_value_get_uint(value); break;
    case PROP_ID:           g_free(self->id);           self->id           = g_value_dup_string(value); break;
    case PROP_ROOT_ID:      g_free(self->root_id);      self->root_id      = g_value_dup_string(value); break;
    case PROP_PUBKEY:       g_free(self->pubkey);       self->pubkey       = g_value_dup_string(value); break;
    case PROP_CREATED_AT:   self->created_at            = g_value_get_int64(value); break;
    case PROP_AVATAR_URL:   g_free(self->avatar_url);   self->avatar_url   = g_value_dup_string(value); break;
    case PROP_VISIBLE:     self->visible = g_value_get_boolean(value); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
  }
}

static void timeline_item_get_property(GObject *obj, guint prop_id, GValue *value, GParamSpec *pspec) {
  TimelineItem *self = (TimelineItem*)obj;
  switch (prop_id) {
    case PROP_DISPLAY_NAME: g_value_set_string(value, self->display_name); break;
    case PROP_HANDLE:       g_value_set_string(value, self->handle); break;
    case PROP_TIMESTAMP:    g_value_set_string(value, self->timestamp); break;
    case PROP_CONTENT:      g_value_set_string(value, self->content); break;
    case PROP_DEPTH:        g_value_set_uint(value, self->depth); break;
    case PROP_ID:           g_value_set_string(value, self->id); break;
    case PROP_ROOT_ID:      g_value_set_string(value, self->root_id); break;
    case PROP_PUBKEY:       g_value_set_string(value, self->pubkey); break;
    case PROP_CREATED_AT:   g_value_set_int64 (value, self->created_at); break;
    case PROP_AVATAR_URL:   g_value_set_string(value, self->avatar_url); break;
    case PROP_VISIBLE:     g_value_set_boolean(value, self->visible); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
  }
}

static void timeline_item_dispose(GObject *obj) {
  TimelineItem *self = (TimelineItem*)obj;
  g_clear_pointer(&self->display_name, g_free);
  g_clear_pointer(&self->handle, g_free);
  g_clear_pointer(&self->timestamp, g_free);
  g_clear_pointer(&self->content, g_free);
  g_clear_pointer(&self->id, g_free);
  g_clear_pointer(&self->root_id, g_free);
  g_clear_pointer(&self->pubkey, g_free);
  g_clear_pointer(&self->avatar_url, g_free);
  if (self->children) g_clear_object(&self->children);
  g_clear_pointer(&self->reposter_pubkey, g_free);
  g_clear_pointer(&self->reposter_display_name, g_free);
  g_clear_pointer(&self->quoted_event_id, g_free);
  g_clear_pointer(&self->quoted_content, g_free);
  g_clear_pointer(&self->quoted_author, g_free);
  G_OBJECT_CLASS(timeline_item_parent_class)->dispose(obj);
}

static void timeline_item_class_init(TimelineItemClass *klass) {
  GObjectClass *oc = G_OBJECT_CLASS(klass);
  oc->set_property = timeline_item_set_property;
  oc->get_property = timeline_item_get_property;
  oc->dispose = timeline_item_dispose;
  ti_props[PROP_DISPLAY_NAME] = g_param_spec_string("display-name", "display-name", "Display Name", NULL, G_PARAM_READWRITE);
  ti_props[PROP_HANDLE]       = g_param_spec_string("handle",       "handle",       "Handle",       NULL, G_PARAM_READWRITE);
  ti_props[PROP_TIMESTAMP]    = g_param_spec_string("timestamp",    "timestamp",    "Timestamp",    NULL, G_PARAM_READWRITE);
  ti_props[PROP_CONTENT]      = g_param_spec_string("content",      "content",      "Content",      NULL, G_PARAM_READWRITE);
  ti_props[PROP_DEPTH]        = g_param_spec_uint   ("depth",        "depth",        "Depth",        0, 32, 0, G_PARAM_READWRITE);
  ti_props[PROP_ID]           = g_param_spec_string ("id",           "id",           "Event Id",     NULL, G_PARAM_READWRITE);
  ti_props[PROP_ROOT_ID]      = g_param_spec_string ("root-id",      "root-id",      "Root Event Id",NULL, G_PARAM_READWRITE);
  ti_props[PROP_PUBKEY]       = g_param_spec_string ("pubkey",       "pubkey",       "Pubkey",       NULL, G_PARAM_READWRITE);
  ti_props[PROP_CREATED_AT]   = g_param_spec_int64  ("created-at",   "created-at",   "Created At",   0, G_MAXINT64, 0, G_PARAM_READWRITE);
  ti_props[PROP_AVATAR_URL]   = g_param_spec_string("avatar-url",   "avatar-url",   "Avatar URL",    NULL, G_PARAM_READWRITE);
  ti_props[PROP_VISIBLE]     = g_param_spec_boolean("visible",     "visible",     "Visible",      TRUE, G_PARAM_READWRITE);
  g_object_class_install_properties(oc, N_TI_PROPS, ti_props);
}

static void timeline_item_init(TimelineItem *self) { (void)self; }

static TimelineItem *timeline_item_new(const char *display, const char *handle, const char *ts, const char *content, guint depth) {
  TimelineItem *it = g_object_new(timeline_item_get_type(),
                                  "display-name", display ? display : "Anonymous",
                                  "handle",       handle  ? handle  : "@anon",
                                  "timestamp",    ts      ? ts      : "now",
                                  "content",      content ? content : "",
                                  "depth",        depth,
                                  NULL);
  it->children = g_list_store_new(timeline_item_get_type());
  return it;
}

static GListModel *timeline_item_get_children_model(TimelineItem *it) {
  return it ? G_LIST_MODEL(it->children) : NULL;
}

static void timeline_item_add_child_internal(TimelineItem *parent, TimelineItem *child) {
  if (!parent || !child) return;
  g_list_store_append(parent->children, child);
}

/* Public wrappers */
void nostr_gtk_timeline_item_add_child(TimelineItem *parent, TimelineItem *child) {
  timeline_item_add_child_internal(parent, child);
}

GListModel *nostr_gtk_timeline_item_get_children(TimelineItem *item) {
  return timeline_item_get_children_model(item);
}

/* ============== NostrGtkTimelineView ============== */
/* struct _NostrGtkTimelineView defined in gnostr-timeline-view-private.h */

G_DEFINE_TYPE(NostrGtkTimelineView, nostr_gtk_timeline_view, GTK_TYPE_WIDGET)

enum {
  SIGNAL_TAB_FILTER_CHANGED,
  N_SIGNALS
};

static guint timeline_view_signals[N_SIGNALS];

/* Handler for tab-selected signal from NostrGtkTimelineTabs */
static void on_tabs_tab_selected(NostrGtkTimelineTabs *tabs_widget, guint index, gpointer user_data) {
  NostrGtkTimelineView *self = NOSTR_GTK_TIMELINE_VIEW(user_data);
  if (!self || !tabs_widget) return;

  GnTimelineTabType type = nostr_gtk_timeline_tabs_get_tab_type(tabs_widget, index);
  const char *filter_value = nostr_gtk_timeline_tabs_get_tab_filter_value(tabs_widget, index);

  g_debug("timeline_view: tab selected index=%u type=%d filter='%s'",
          index, type, filter_value ? filter_value : "(null)");

  g_signal_emit(self, timeline_view_signals[SIGNAL_TAB_FILTER_CHANGED], 0,
                (guint)type, filter_value);
}

static void nostr_gtk_timeline_view_dispose(GObject *obj) {
  NostrGtkTimelineView *self = NOSTR_GTK_TIMELINE_VIEW(obj);

  if (self->scroll_idle_id > 0) {
    g_source_remove(self->scroll_idle_id);
    self->scroll_idle_id = 0;
  }

  if (self->list_view && GTK_IS_LIST_VIEW(self->list_view)) {
    gtk_list_view_set_model(GTK_LIST_VIEW(self->list_view), NULL);
  }

  g_clear_object(&self->selection_model);
  g_clear_object(&self->tree_model);
  g_clear_object(&self->flattened_model);
  g_clear_object(&self->list_model);

  /* App-level batch metadata cleanup */
  if (self->metadata_batch_idle_id > 0) {
    g_source_remove(self->metadata_batch_idle_id);
    self->metadata_batch_idle_id = 0;
  }
  g_clear_pointer(&self->pending_metadata_items, g_ptr_array_unref);

  gtk_widget_dispose_template(GTK_WIDGET(obj), NOSTR_GTK_TYPE_TIMELINE_VIEW);
  self->root_scroller = NULL;
  self->list_view = NULL;
  G_OBJECT_CLASS(nostr_gtk_timeline_view_parent_class)->dispose(obj);
}

static void nostr_gtk_timeline_view_finalize(GObject *obj) {
  G_OBJECT_CLASS(nostr_gtk_timeline_view_parent_class)->finalize(obj);
}

/* Scroll tracking */
#define FAST_SCROLL_THRESHOLD 2.0  /* pixels/ms */
#define SCROLL_IDLE_TIMEOUT_MS 150

static gboolean on_scroll_idle_timeout(gpointer user_data) {
  NostrGtkTimelineView *self = NOSTR_GTK_TIMELINE_VIEW(user_data);
  self->scroll_idle_id = 0;
  self->is_fast_scrolling = FALSE;
  self->scroll_velocity = 0.0;
  return G_SOURCE_REMOVE;
}

static void on_scroll_value_changed(GtkAdjustment *adj, gpointer user_data) {
  NostrGtkTimelineView *self = NOSTR_GTK_TIMELINE_VIEW(user_data);
  gdouble value = gtk_adjustment_get_value(adj);
  gint64 now = g_get_monotonic_time() / 1000;

  if (self->last_scroll_time > 0) {
    gint64 dt = now - self->last_scroll_time;
    if (dt > 0) {
      gdouble dx = value - self->last_scroll_value;
      self->scroll_velocity = (dx < 0 ? -dx : dx) / (gdouble)dt;
      self->is_fast_scrolling = (self->scroll_velocity > FAST_SCROLL_THRESHOLD);
    }
  }

  self->last_scroll_value = value;
  self->last_scroll_time = now;

  /* Update visible range estimate */
  gdouble page = gtk_adjustment_get_page_size(adj);
  gdouble upper = gtk_adjustment_get_upper(adj);
  if (upper > 0 && self->list_view) {
    GtkSelectionModel *model = gtk_list_view_get_model(GTK_LIST_VIEW(self->list_view));
    if (model) {
      guint n = g_list_model_get_n_items(G_LIST_MODEL(model));
      if (n > 0 && upper > 0) {
        gdouble item_height_est = upper / (gdouble)n;
        if (item_height_est > 0) {
          self->visible_range_start = (guint)(value / item_height_est);
          self->visible_range_end = (guint)((value + page) / item_height_est) + 1;
          if (self->visible_range_end > n) self->visible_range_end = n;
        }
      }
    }
  }

  /* Reset idle timeout */
  if (self->scroll_idle_id > 0) {
    g_source_remove(self->scroll_idle_id);
  }
  self->scroll_idle_id = g_timeout_add(SCROLL_IDLE_TIMEOUT_MS, on_scroll_idle_timeout, self);
}

static void ensure_list_model(NostrGtkTimelineView *self) {
  if (self->list_model) return;
  self->list_model = g_list_store_new(timeline_item_get_type());
  self->selection_model = GTK_SELECTION_MODEL(gtk_single_selection_new(G_LIST_MODEL(self->list_model)));
  gtk_list_view_set_model(GTK_LIST_VIEW(self->list_view), self->selection_model);
}

/* ============== GObject Class ============== */

/* Fix horizontal expansion: clamp minimum width to 0 so AdwClamp can constrain */
static void nostr_gtk_timeline_view_measure(GtkWidget      *widget,
                                            GtkOrientation  orientation,
                                            int             for_size,
                                            int            *minimum,
                                            int            *natural,
                                            int            *minimum_baseline,
                                            int            *natural_baseline) {
  /* Chain up to parent to get actual measurements */
  GTK_WIDGET_CLASS(nostr_gtk_timeline_view_parent_class)->measure(
      widget, orientation, for_size, minimum, natural, minimum_baseline, natural_baseline);
  
  /* Clamp horizontal minimum to 0 so parent containers can constrain width */
  if (orientation == GTK_ORIENTATION_HORIZONTAL && minimum) {
    *minimum = 0;
  }
}

static void nostr_gtk_timeline_view_class_init(NostrGtkTimelineViewClass *klass) {
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  GObjectClass *gobj_class = G_OBJECT_CLASS(klass);
  gobj_class->dispose = nostr_gtk_timeline_view_dispose;
  gobj_class->finalize = nostr_gtk_timeline_view_finalize;
  widget_class->measure = nostr_gtk_timeline_view_measure;
  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BOX_LAYOUT);
  g_type_ensure(NOSTR_GTK_TYPE_TIMELINE_TABS);
  gtk_widget_class_set_template_from_resource(widget_class, UI_RESOURCE);
  gtk_widget_class_bind_template_child(widget_class, NostrGtkTimelineView, root_box);
  gtk_widget_class_bind_template_child(widget_class, NostrGtkTimelineView, tabs);
  gtk_widget_class_bind_template_child(widget_class, NostrGtkTimelineView, root_scroller);
  gtk_widget_class_bind_template_child(widget_class, NostrGtkTimelineView, list_view);

  timeline_view_signals[SIGNAL_TAB_FILTER_CHANGED] =
    g_signal_new("tab-filter-changed",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 2, G_TYPE_UINT, G_TYPE_STRING);
}

static void nostr_gtk_timeline_view_init(NostrGtkTimelineView *self) {
  gtk_widget_init_template(GTK_WIDGET(self));
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->list_view),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "Timeline List", -1);
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->root_scroller),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "Timeline Scroll", -1);

  /* No factory created here - consumer must call set_factory() */

  /* Connect to tabs signals */
  if (self->tabs && NOSTR_GTK_IS_TIMELINE_TABS(self->tabs)) {
    g_signal_connect(self->tabs, "tab-selected", G_CALLBACK(on_tabs_tab_selected), self);
  }

  /* Connect scroll position tracking */
  if (self->root_scroller && GTK_IS_SCROLLED_WINDOW(self->root_scroller)) {
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self->root_scroller));
    if (vadj) {
      g_signal_connect(vadj, "value-changed", G_CALLBACK(on_scroll_value_changed), self);
    }
  }

  /* Install minimal CSS for thread indicator and avatar */
  static const char *css =
    ".avatar { border-radius: 18px; background: @theme_bg_color; padding: 2px; }\n"
    ".dim-label { opacity: 0.7; }\n"
    ".thread-reply { background: alpha(@theme_bg_color, 0.5); border-left: 3px solid @theme_selected_bg_color; }\n"
    ".thread-root { }\n"
    ".thread-indicator { min-width: 4px; min-height: 4px; background: @theme_selected_bg_color; }\n"
    "note-card { border-radius: 8px; margin: 2px; }\n"
    "note-card.thread-depth-1 { margin-left: 20px; background: alpha(@theme_bg_color, 0.3); }\n"
    "note-card.thread-depth-2 { margin-left: 40px; background: alpha(@theme_bg_color, 0.4); }\n"
    "note-card.thread-depth-3 { margin-left: 60px; background: alpha(@theme_bg_color, 0.5); }\n"
    "note-card.thread-depth-4 { margin-left: 80px; background: alpha(@theme_bg_color, 0.6); }\n"
    ".root-0 { background: #6b7280; } .root-1 { background: #ef4444; } .root-2 { background: #f59e0b; } .root-3 { background: #10b981; }\n"
    ".root-4 { background: #3b82f6; } .root-5 { background: #8b5cf6; } .root-6 { background: #ec4899; } .root-7 { background: #22c55e; }\n"
    ".root-8 { background: #06b6d4; } .root-9 { background: #f97316; } .root-a { background: #0ea5e9; } .root-b { background: #84cc16; }\n"
    ".root-c { background: #a855f7; } .root-d { background: #eab308; } .root-e { background: #f43f5e; } .root-f { background: #14b8a6; }\n";
  GtkCssProvider *prov = gtk_css_provider_new();
  gtk_css_provider_load_from_string(prov, css);
  gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(prov), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(prov);
}

/* ============== Public API ============== */

GtkWidget *nostr_gtk_timeline_view_new(void) {
  return g_object_new(NOSTR_GTK_TYPE_TIMELINE_VIEW, NULL);
}

void nostr_gtk_timeline_view_set_factory(NostrGtkTimelineView *self, GtkListItemFactory *factory) {
  g_return_if_fail(NOSTR_GTK_IS_TIMELINE_VIEW(self));
  g_return_if_fail(factory == NULL || GTK_IS_LIST_ITEM_FACTORY(factory));
  gtk_list_view_set_factory(GTK_LIST_VIEW(self->list_view), factory);
}

void nostr_gtk_timeline_view_set_model(NostrGtkTimelineView *self, GtkSelectionModel *model) {
  g_return_if_fail(NOSTR_GTK_IS_TIMELINE_VIEW(self));
  if (self->selection_model == model) return;
  if (self->selection_model) g_clear_object(&self->selection_model);
  if (self->list_model) g_clear_object(&self->list_model);
  if (self->tree_model) g_clear_object(&self->tree_model);
  self->selection_model = model ? g_object_ref(model) : NULL;
  gtk_list_view_set_model(GTK_LIST_VIEW(self->list_view), self->selection_model);
}

/* Forward declaration */
static void populate_flattened_model(NostrGtkTimelineView *self, GListModel *roots);

static void on_root_items_changed(GListModel *list, guint position, guint removed, guint added, gpointer user_data) {
  NostrGtkTimelineView *self = (NostrGtkTimelineView *)user_data;
  (void)position; (void)removed; (void)added;
  g_debug("[TREE] Root items changed: position=%u removed=%u added=%u total=%u",
           position, removed, added, g_list_model_get_n_items(list));

  if (self && self->flattened_model && self->tree_model) {
    populate_flattened_model(self, G_LIST_MODEL(self->tree_model));
  }
}

static void populate_flattened_model(NostrGtkTimelineView *self, GListModel *roots) {
  if (!self || !self->flattened_model || !roots) return;

  g_list_store_remove_all(self->flattened_model);

  for (guint i = 0; i < g_list_model_get_n_items(roots); i++) {
    TimelineItem *root = (TimelineItem*)g_list_model_get_item(roots, i);
    if (!root) continue;

    g_list_store_append(self->flattened_model, root);

    GListModel *children = timeline_item_get_children_model(root);
    if (children) {
      guint child_count = g_list_model_get_n_items(children);
      for (guint j = 0; j < child_count; j++) {
        TimelineItem *child = (TimelineItem*)g_list_model_get_item(children, j);
        if (child) {
          g_list_store_append(self->flattened_model, child);
        }
      }
    }

    g_object_unref(root);
  }
}

void nostr_gtk_timeline_view_set_tree_roots(NostrGtkTimelineView *self, GListModel *roots) {
  g_return_if_fail(NOSTR_GTK_IS_TIMELINE_VIEW(self));

  if (roots) {
    g_signal_connect(roots, "items-changed", G_CALLBACK(on_root_items_changed), self);
  }

  if (self->list_view) {
    gtk_list_view_set_model(GTK_LIST_VIEW(self->list_view), NULL);
  }

  g_clear_object(&self->selection_model);
  g_clear_object(&self->tree_model);
  g_clear_object(&self->flattened_model);

  if (roots) {
    self->flattened_model = g_list_store_new(timeline_item_get_type());
    self->tree_model = (GtkTreeListModel*)roots;

    self->selection_model = GTK_SELECTION_MODEL(gtk_single_selection_new(G_LIST_MODEL(self->flattened_model)));
    g_object_ref_sink(self->selection_model);

    populate_flattened_model(self, roots);
  } else {
    self->tree_model = NULL;
    self->flattened_model = NULL;
    self->selection_model = NULL;
  }

  gtk_list_view_set_model(GTK_LIST_VIEW(self->list_view), self->selection_model);
}

void nostr_gtk_timeline_view_prepend_text(NostrGtkTimelineView *self, const char *text) {
  g_return_if_fail(NOSTR_GTK_IS_TIMELINE_VIEW(self));
  ensure_list_model(self);
  TimelineItem *item = timeline_item_new(NULL, NULL, NULL, text, 0);
  g_list_store_insert(self->list_model, 0, item);
  g_object_unref(item);
  if (self->root_scroller && GTK_IS_SCROLLED_WINDOW(self->root_scroller)) {
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self->root_scroller));
    if (vadj) {
      gtk_adjustment_set_value(vadj, gtk_adjustment_get_lower(vadj));
    }
  }
}

void nostr_gtk_timeline_view_prepend(NostrGtkTimelineView *self,
                                   const char *display,
                                   const char *handle,
                                   const char *ts,
                                   const char *content,
                                   guint depth) {
  g_return_if_fail(NOSTR_GTK_IS_TIMELINE_VIEW(self));
  ensure_list_model(self);
  TimelineItem *item = timeline_item_new(display, handle, ts, content, depth);
  g_list_store_insert(self->list_model, 0, item);
  g_object_unref(item);
  if (self->root_scroller && GTK_IS_SCROLLED_WINDOW(self->root_scroller)) {
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self->root_scroller));
    if (vadj) gtk_adjustment_set_value(vadj, gtk_adjustment_get_lower(vadj));
  }
}

GtkWidget *nostr_gtk_timeline_view_get_scrolled_window(NostrGtkTimelineView *self) {
  g_return_val_if_fail(NOSTR_GTK_IS_TIMELINE_VIEW(self), NULL);
  return self->root_scroller;
}

GtkWidget *nostr_gtk_timeline_view_get_list_view(NostrGtkTimelineView *self) {
  g_return_val_if_fail(NOSTR_GTK_IS_TIMELINE_VIEW(self), NULL);
  return self->list_view;
}

/* ============== Timeline Tabs Support ============== */

NostrGtkTimelineTabs *nostr_gtk_timeline_view_get_tabs(NostrGtkTimelineView *self) {
  g_return_val_if_fail(NOSTR_GTK_IS_TIMELINE_VIEW(self), NULL);
  return NOSTR_GTK_TIMELINE_TABS(self->tabs);
}

void nostr_gtk_timeline_view_set_tabs_visible(NostrGtkTimelineView *self, gboolean visible) {
  g_return_if_fail(NOSTR_GTK_IS_TIMELINE_VIEW(self));
  if (self->tabs) {
    gtk_widget_set_visible(self->tabs, visible);
  }
}

void nostr_gtk_timeline_view_add_hashtag_tab(NostrGtkTimelineView *self, const char *hashtag) {
  g_return_if_fail(NOSTR_GTK_IS_TIMELINE_VIEW(self));
  g_return_if_fail(hashtag != NULL);

  if (!self->tabs) return;

  gtk_widget_set_visible(self->tabs, TRUE);

  g_autofree char *label = g_strdup_printf("#%s", hashtag);
  guint index = nostr_gtk_timeline_tabs_add_tab(NOSTR_GTK_TIMELINE_TABS(self->tabs),
                                          GN_TIMELINE_TAB_HASHTAG,
                                          label,
                                          hashtag);

  nostr_gtk_timeline_tabs_set_selected(NOSTR_GTK_TIMELINE_TABS(self->tabs), index);
}

void nostr_gtk_timeline_view_add_author_tab(NostrGtkTimelineView *self, const char *pubkey_hex, const char *display_name) {
  g_return_if_fail(NOSTR_GTK_IS_TIMELINE_VIEW(self));
  g_return_if_fail(pubkey_hex != NULL);

  g_autofree gchar *hex = gnostr_ensure_hex_pubkey(pubkey_hex);
  if (!hex) return;

  if (!self->tabs) return;

  gtk_widget_set_visible(self->tabs, TRUE);

  char *label;
  if (display_name && *display_name) {
    label = g_strdup(display_name);
  } else {
    label = g_strndup(hex, 8);
  }

  guint index = nostr_gtk_timeline_tabs_add_tab(NOSTR_GTK_TIMELINE_TABS(self->tabs),
                                          GN_TIMELINE_TAB_AUTHOR,
                                          label,
                                          hex);
  g_free(label);

  nostr_gtk_timeline_tabs_set_selected(NOSTR_GTK_TIMELINE_TABS(self->tabs), index);
}

/* ============== Scroll Position Tracking API ============== */

gboolean nostr_gtk_timeline_view_get_visible_range(NostrGtkTimelineView *self,
                                                  guint *start,
                                                  guint *end) {
  g_return_val_if_fail(NOSTR_GTK_IS_TIMELINE_VIEW(self), FALSE);

  if (start) *start = self->visible_range_start;
  if (end) *end = self->visible_range_end;

  return (self->visible_range_end > self->visible_range_start);
}

gboolean nostr_gtk_timeline_view_is_item_visible(NostrGtkTimelineView *self, guint index) {
  g_return_val_if_fail(NOSTR_GTK_IS_TIMELINE_VIEW(self), FALSE);

  return (index >= self->visible_range_start && index < self->visible_range_end);
}

gboolean nostr_gtk_timeline_view_is_fast_scrolling(NostrGtkTimelineView *self) {
  g_return_val_if_fail(NOSTR_GTK_IS_TIMELINE_VIEW(self), FALSE);

  return self->is_fast_scrolling;
}

gdouble nostr_gtk_timeline_view_get_scroll_velocity(NostrGtkTimelineView *self) {
  g_return_val_if_fail(NOSTR_GTK_IS_TIMELINE_VIEW(self), 0.0);

  return self->scroll_velocity;
}
