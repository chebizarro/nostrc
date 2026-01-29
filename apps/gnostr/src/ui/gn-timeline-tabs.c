/**
 * GnTimelineTabs - Tab bar for switching between timeline views
 *
 * Part of the Timeline Architecture Refactor (nostrc-e03f)
 */

#define G_LOG_DOMAIN "gn-timeline-tabs"

#include "gn-timeline-tabs.h"
#include <adwaita.h>

typedef struct {
  GnTimelineTabType type;
  char *label;
  char *filter_value;
  GtkWidget *button;
  GtkWidget *close_button;
  gboolean closable;
} TabInfo;

struct _GnTimelineTabs {
  GtkWidget parent_instance;

  GtkWidget *scrolled;
  GtkWidget *box;

  GPtrArray *tabs;  /* TabInfo* */
  guint selected;
};

G_DEFINE_TYPE(GnTimelineTabs, gn_timeline_tabs, GTK_TYPE_WIDGET)

enum {
  PROP_0,
  PROP_SELECTED,
  N_PROPS
};

static GParamSpec *properties[N_PROPS];

enum {
  SIGNAL_TAB_SELECTED,
  SIGNAL_TAB_CLOSED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

static void tab_info_free(gpointer data) {
  TabInfo *info = data;
  if (!info) return;
  g_free(info->label);
  g_free(info->filter_value);
  g_free(info);
}

static void on_tab_clicked(GtkButton *button, gpointer user_data);
static void on_close_clicked(GtkButton *button, gpointer user_data);
static void update_tab_styles(GnTimelineTabs *self);

/* ============== Public API ============== */

GnTimelineTabs *gn_timeline_tabs_new(void) {
  return g_object_new(GN_TYPE_TIMELINE_TABS, NULL);
}

guint gn_timeline_tabs_add_tab(GnTimelineTabs *self,
                                GnTimelineTabType type,
                                const char *label,
                                const char *filter_value) {
  g_return_val_if_fail(GN_IS_TIMELINE_TABS(self), 0);
  g_return_val_if_fail(label != NULL, 0);

  TabInfo *info = g_new0(TabInfo, 1);
  info->type = type;
  info->label = g_strdup(label);
  info->filter_value = g_strdup(filter_value);
  info->closable = (type != GN_TIMELINE_TAB_GLOBAL);  /* Global tab not closable */

  /* Create tab button */
  info->button = gtk_button_new();
  gtk_widget_add_css_class(info->button, "flat");
  gtk_widget_add_css_class(info->button, "timeline-tab");

  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_button_set_child(GTK_BUTTON(info->button), hbox);

  /* Icon based on type */
  const char *icon_name = "view-list-symbolic";
  switch (type) {
    case GN_TIMELINE_TAB_GLOBAL:
      icon_name = "network-workgroup-symbolic";
      break;
    case GN_TIMELINE_TAB_FOLLOWING:
      icon_name = "system-users-symbolic";
      break;
    case GN_TIMELINE_TAB_HASHTAG:
      icon_name = "tag-symbolic";
      break;
    case GN_TIMELINE_TAB_AUTHOR:
      icon_name = "avatar-default-symbolic";
      break;
    case GN_TIMELINE_TAB_CUSTOM:
      icon_name = "view-list-symbolic";
      break;
  }

  GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
  gtk_box_append(GTK_BOX(hbox), icon);

  GtkWidget *lbl = gtk_label_new(label);
  gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars(GTK_LABEL(lbl), 15);
  gtk_box_append(GTK_BOX(hbox), lbl);

  /* Close button (if closable) */
  if (info->closable) {
    info->close_button = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_add_css_class(info->close_button, "flat");
    gtk_widget_add_css_class(info->close_button, "circular");
    gtk_widget_add_css_class(info->close_button, "tab-close");
    gtk_widget_set_valign(info->close_button, GTK_ALIGN_CENTER);
    g_signal_connect(info->close_button, "clicked", G_CALLBACK(on_close_clicked), self);
    gtk_box_append(GTK_BOX(hbox), info->close_button);
  }

  g_signal_connect(info->button, "clicked", G_CALLBACK(on_tab_clicked), self);

  guint index = self->tabs->len;
  g_ptr_array_add(self->tabs, info);
  gtk_box_append(GTK_BOX(self->box), info->button);

  /* Select first tab by default */
  if (index == 0) {
    self->selected = 0;
    update_tab_styles(self);
  }

  return index;
}

void gn_timeline_tabs_remove_tab(GnTimelineTabs *self, guint index) {
  g_return_if_fail(GN_IS_TIMELINE_TABS(self));
  g_return_if_fail(index < self->tabs->len);

  TabInfo *info = g_ptr_array_index(self->tabs, index);
  if (!info->closable) return;  /* Can't close non-closable tabs */

  gtk_box_remove(GTK_BOX(self->box), info->button);
  g_ptr_array_remove_index(self->tabs, index);

  /* Adjust selection if needed */
  if (self->selected >= self->tabs->len && self->tabs->len > 0) {
    self->selected = self->tabs->len - 1;
  } else if (self->selected > index) {
    self->selected--;
  }

  update_tab_styles(self);
  g_signal_emit(self, signals[SIGNAL_TAB_CLOSED], 0, index);
}

guint gn_timeline_tabs_get_selected(GnTimelineTabs *self) {
  g_return_val_if_fail(GN_IS_TIMELINE_TABS(self), 0);
  return self->selected;
}

void gn_timeline_tabs_set_selected(GnTimelineTabs *self, guint index) {
  g_return_if_fail(GN_IS_TIMELINE_TABS(self));
  g_return_if_fail(index < self->tabs->len);

  if (self->selected == index) return;

  self->selected = index;
  update_tab_styles(self);
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_SELECTED]);
  g_signal_emit(self, signals[SIGNAL_TAB_SELECTED], 0, index);
}

GnTimelineTabType gn_timeline_tabs_get_tab_type(GnTimelineTabs *self, guint index) {
  g_return_val_if_fail(GN_IS_TIMELINE_TABS(self), GN_TIMELINE_TAB_GLOBAL);
  g_return_val_if_fail(index < self->tabs->len, GN_TIMELINE_TAB_GLOBAL);

  TabInfo *info = g_ptr_array_index(self->tabs, index);
  return info->type;
}

const char *gn_timeline_tabs_get_tab_filter_value(GnTimelineTabs *self, guint index) {
  g_return_val_if_fail(GN_IS_TIMELINE_TABS(self), NULL);
  g_return_val_if_fail(index < self->tabs->len, NULL);

  TabInfo *info = g_ptr_array_index(self->tabs, index);
  return info->filter_value;
}

guint gn_timeline_tabs_get_n_tabs(GnTimelineTabs *self) {
  g_return_val_if_fail(GN_IS_TIMELINE_TABS(self), 0);
  return self->tabs->len;
}

void gn_timeline_tabs_set_closable(GnTimelineTabs *self, guint index, gboolean closable) {
  g_return_if_fail(GN_IS_TIMELINE_TABS(self));
  g_return_if_fail(index < self->tabs->len);

  TabInfo *info = g_ptr_array_index(self->tabs, index);
  info->closable = closable;

  if (info->close_button) {
    gtk_widget_set_visible(info->close_button, closable);
  }
}

/* ============== Signal Handlers ============== */

static void on_tab_clicked(GtkButton *button, gpointer user_data) {
  GnTimelineTabs *self = GN_TIMELINE_TABS(user_data);

  /* Find which tab was clicked */
  for (guint i = 0; i < self->tabs->len; i++) {
    TabInfo *info = g_ptr_array_index(self->tabs, i);
    if (info->button == GTK_WIDGET(button)) {
      gn_timeline_tabs_set_selected(self, i);
      return;
    }
  }
}

static void on_close_clicked(GtkButton *button, gpointer user_data) {
  GnTimelineTabs *self = GN_TIMELINE_TABS(user_data);

  /* Find which tab's close button was clicked */
  for (guint i = 0; i < self->tabs->len; i++) {
    TabInfo *info = g_ptr_array_index(self->tabs, i);
    if (info->close_button == GTK_WIDGET(button)) {
      gn_timeline_tabs_remove_tab(self, i);
      return;
    }
  }
}

static void update_tab_styles(GnTimelineTabs *self) {
  for (guint i = 0; i < self->tabs->len; i++) {
    TabInfo *info = g_ptr_array_index(self->tabs, i);
    if (i == self->selected) {
      gtk_widget_add_css_class(info->button, "suggested-action");
      gtk_widget_remove_css_class(info->button, "flat");
    } else {
      gtk_widget_remove_css_class(info->button, "suggested-action");
      gtk_widget_add_css_class(info->button, "flat");
    }
  }
}

/* ============== GObject Lifecycle ============== */

static void gn_timeline_tabs_dispose(GObject *object) {
  GnTimelineTabs *self = GN_TIMELINE_TABS(object);

  g_clear_pointer(&self->scrolled, gtk_widget_unparent);

  G_OBJECT_CLASS(gn_timeline_tabs_parent_class)->dispose(object);
}

static void gn_timeline_tabs_finalize(GObject *object) {
  GnTimelineTabs *self = GN_TIMELINE_TABS(object);

  g_ptr_array_unref(self->tabs);

  G_OBJECT_CLASS(gn_timeline_tabs_parent_class)->finalize(object);
}

static void gn_timeline_tabs_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec) {
  GnTimelineTabs *self = GN_TIMELINE_TABS(object);

  switch (prop_id) {
    case PROP_SELECTED:
      g_value_set_uint(value, self->selected);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void gn_timeline_tabs_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec) {
  GnTimelineTabs *self = GN_TIMELINE_TABS(object);

  switch (prop_id) {
    case PROP_SELECTED:
      gn_timeline_tabs_set_selected(self, g_value_get_uint(value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void gn_timeline_tabs_class_init(GnTimelineTabsClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = gn_timeline_tabs_dispose;
  object_class->finalize = gn_timeline_tabs_finalize;
  object_class->get_property = gn_timeline_tabs_get_property;
  object_class->set_property = gn_timeline_tabs_set_property;

  properties[PROP_SELECTED] =
    g_param_spec_uint("selected", "Selected", "Selected tab index",
                      0, G_MAXUINT, 0,
                      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties(object_class, N_PROPS, properties);

  signals[SIGNAL_TAB_SELECTED] =
    g_signal_new("tab-selected",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 1, G_TYPE_UINT);

  signals[SIGNAL_TAB_CLOSED] =
    g_signal_new("tab-closed",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 1, G_TYPE_UINT);

  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name(widget_class, "timeline-tabs");
}

static void gn_timeline_tabs_init(GnTimelineTabs *self) {
  self->tabs = g_ptr_array_new_with_free_func(tab_info_free);
  self->selected = 0;

  /* Create scrolled window for horizontal scrolling */
  self->scrolled = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(self->scrolled),
                                  GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
  gtk_widget_set_parent(self->scrolled, GTK_WIDGET(self));

  /* Create horizontal box for tabs */
  self->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_add_css_class(self->box, "linked");
  gtk_widget_set_margin_start(self->box, 6);
  gtk_widget_set_margin_end(self->box, 6);
  gtk_widget_set_margin_top(self->box, 4);
  gtk_widget_set_margin_bottom(self->box, 4);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(self->scrolled), self->box);

  /* Add default Global tab */
  gn_timeline_tabs_add_tab(self, GN_TIMELINE_TAB_GLOBAL, "Global", NULL);
}
