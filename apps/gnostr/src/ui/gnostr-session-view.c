#define G_LOG_DOMAIN "gnostr-session-view"

#include "gnostr-session-view.h"

#include "gnostr-classifieds-view.h"
#include "gnostr-dm-inbox-view.h"
#include "gnostr-notifications-view.h"
#include "gnostr-profile-pane.h"
#include "gnostr-thread-view.h"
#include "gnostr-timeline-view.h"
#include "page-discover.h"

#include <gdk/gdkkeysyms.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>

/* This must match the compiled resource path for the blueprint template */
#define UI_RESOURCE "/org/gnostr/ui/ui/widgets/gnostr-session-view.ui"

typedef enum {
  PROP_0,
  PROP_COMPACT,
  PROP_AUTHENTICATED,
  N_PROPS
} GnostrSessionViewProperty;

static GParamSpec *props[N_PROPS];

typedef enum {
  SIGNAL_PAGE_SELECTED,
  SIGNAL_SETTINGS_REQUESTED,
  SIGNAL_RELAYS_REQUESTED,
  SIGNAL_LOGIN_REQUESTED,
  SIGNAL_LOGOUT_REQUESTED,
  SIGNAL_NEW_NOTES_CLICKED,
  N_SIGNALS
} GnostrSessionViewSignal;

static guint signals[N_SIGNALS];

struct _GnostrSessionView {
  AdwBin parent_instance;

  /* Template root/container */
  GtkOverlay *session_overlay;
  AdwNavigationSplitView *split_view;

  /* Sidebar */
  AdwNavigationPage *sidebar_page;
  AdwHeaderBar *sidebar_header_bar;
  GtkButton *btn_settings;
  GtkMenuButton *btn_menu;
  GtkScrolledWindow *sidebar_scroller;
  GtkListBox *sidebar_list;

  GtkListBoxRow *row_timeline;
  GtkListBoxRow *row_notifications;
  GtkListBoxRow *row_messages;
  GtkListBoxRow *row_discover;
  GtkListBoxRow *row_classifieds;

  /* Content */
  AdwNavigationPage *content_page;
  AdwToolbarView *toolbar_view;
  AdwHeaderBar *header_bar;
  GtkButton *btn_relays;
  GtkMenuButton *btn_avatar;

  GtkPopover *avatar_popover;
  GtkLabel *lbl_signin_status;
  GtkLabel *lbl_profile_name;
  GtkButton *btn_login;
  GtkButton *btn_logout;

  GtkBox *content_root;

  GtkRevealer *new_notes_revealer;
  GtkButton *btn_new_notes;
  GtkLabel *lbl_new_notes_count;

  AdwOverlaySplitView *panel_split;
  GtkBox *panel_container;
  GtkWidget *profile_pane;
  GtkWidget *thread_view;

  AdwViewStack *stack;
  GtkScrolledWindow *timeline_scroller;
  GtkWidget *timeline;
  GtkWidget *notifications_view;
  GtkWidget *dm_inbox;
  GtkWidget *discover_page;
  GtkWidget *classifieds_view;

  AdwViewSwitcherBar *bottom_bar;

  /* State */
  gboolean compact;
  gboolean authenticated;
  gboolean showing_profile;

  /* Optional toast forwarding (weak) */
  GWeakRef toast_overlay_ref;
};

G_DEFINE_FINAL_TYPE(GnostrSessionView, gnostr_session_view, ADW_TYPE_BIN)

static const char *page_name_for_row(GnostrSessionView *self, GtkListBoxRow *row) {
  if (!self || !row) return NULL;
  if (row == self->row_timeline) return "timeline";
  if (row == self->row_notifications) return "notifications";
  if (row == self->row_messages) return "messages";
  if (row == self->row_discover) return "discover";
  if (row == self->row_classifieds) return "classifieds";
  return NULL;
}

static GtkListBoxRow *row_for_page_name(GnostrSessionView *self, const char *page_name) {
  if (!self || !page_name) return NULL;
  if (g_strcmp0(page_name, "timeline") == 0) return self->row_timeline;
  if (g_strcmp0(page_name, "notifications") == 0) return self->row_notifications;
  if (g_strcmp0(page_name, "messages") == 0) return self->row_messages;
  if (g_strcmp0(page_name, "discover") == 0) return self->row_discover;
  if (g_strcmp0(page_name, "classifieds") == 0) return self->row_classifieds;
  return NULL;
}

static const char *title_for_page_name(const char *page_name) {
  if (g_strcmp0(page_name, "timeline") == 0) return _("Timeline");
  if (g_strcmp0(page_name, "notifications") == 0) return _("Notifications");
  if (g_strcmp0(page_name, "messages") == 0) return _("Messages");
  if (g_strcmp0(page_name, "discover") == 0) return _("Discover");
  if (g_strcmp0(page_name, "classifieds") == 0) return _("Marketplace");
  return NULL;
}

/* Forward declarations for signal handlers used in ensure_avatar_popover */
static void on_btn_login_clicked(GtkButton *btn, gpointer user_data);
static void on_btn_logout_clicked(GtkButton *btn, gpointer user_data);

/* Create avatar popover lazily to avoid GTK4 crash on Linux */
static void ensure_avatar_popover(GnostrSessionView *self) {
  if (!self || !self->btn_avatar || self->avatar_popover) return;

  self->avatar_popover = GTK_POPOVER(gtk_popover_new());
  
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_top(box, 12);
  gtk_widget_set_margin_bottom(box, 12);
  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);
  
  self->lbl_signin_status = GTK_LABEL(gtk_label_new(_("Not signed in")));
  gtk_widget_add_css_class(GTK_WIDGET(self->lbl_signin_status), "dim-label");
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(self->lbl_signin_status));
  
  self->lbl_profile_name = GTK_LABEL(gtk_label_new(""));
  gtk_widget_add_css_class(GTK_WIDGET(self->lbl_profile_name), "heading");
  gtk_widget_set_visible(GTK_WIDGET(self->lbl_profile_name), FALSE);
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(self->lbl_profile_name));
  
  self->btn_login = GTK_BUTTON(gtk_button_new_with_label(_("Sign In")));
  gtk_widget_add_css_class(GTK_WIDGET(self->btn_login), "suggested-action");
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(self->btn_login));
  
  self->btn_logout = GTK_BUTTON(gtk_button_new_with_label(_("Sign Out")));
  gtk_widget_add_css_class(GTK_WIDGET(self->btn_logout), "destructive-action");
  gtk_widget_set_visible(GTK_WIDGET(self->btn_logout), FALSE);
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(self->btn_logout));
  
  gtk_popover_set_child(self->avatar_popover, box);
  gtk_menu_button_set_popover(self->btn_avatar, GTK_WIDGET(self->avatar_popover));

  /* Connect signals for the newly created buttons */
  g_signal_connect(self->btn_login, "clicked", G_CALLBACK(on_btn_login_clicked), self);
  g_signal_connect(self->btn_logout, "clicked", G_CALLBACK(on_btn_logout_clicked), self);
}

static void update_auth_gating(GnostrSessionView *self) {
  if (!self) return;

  /* NOTE: Do NOT call ensure_avatar_popover here - it will be created lazily
   * when the user clicks the avatar button. Creating it during init causes
   * GTK4 crash on Linux. */

  if (self->row_notifications)
    gtk_widget_set_sensitive(GTK_WIDGET(self->row_notifications), self->authenticated);
  if (self->row_messages)
    gtk_widget_set_sensitive(GTK_WIDGET(self->row_messages), self->authenticated);

  /* If we became unauthenticated while on a gated page, go back to timeline */
  if (!self->authenticated && self->stack) {
    const char *visible = adw_view_stack_get_visible_child_name(self->stack);
    if (g_strcmp0(visible, "notifications") == 0 || g_strcmp0(visible, "messages") == 0) {
      gnostr_session_view_show_page(self, "timeline");
    }
  }
}

static gboolean on_key_pressed(GtkEventControllerKey *controller,
                               guint keyval,
                               guint keycode,
                               GdkModifierType state,
                               gpointer user_data) {
  (void)controller;
  (void)keycode;
  (void)state;

  GnostrSessionView *self = GNOSTR_SESSION_VIEW(user_data);
  if (!GNOSTR_IS_SESSION_VIEW(self)) return GDK_EVENT_PROPAGATE;

  if (keyval == GDK_KEY_Escape) {
    if (gnostr_session_view_is_side_panel_visible(self)) {
      gnostr_session_view_hide_side_panel(self);
      return GDK_EVENT_STOP;
    }
  }

  return GDK_EVENT_PROPAGATE;
}

static void on_sidebar_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
  (void)box;

  GnostrSessionView *self = GNOSTR_SESSION_VIEW(user_data);
  if (!GNOSTR_IS_SESSION_VIEW(self) || !row) return;

  const char *page_name = page_name_for_row(self, row);
  if (!page_name) return;

  if (!self->authenticated &&
      (g_strcmp0(page_name, "notifications") == 0 || g_strcmp0(page_name, "messages") == 0)) {
    g_signal_emit(self, signals[SIGNAL_LOGIN_REQUESTED], 0);
    gnostr_session_view_show_toast(self, _("Sign in to view this page."));
    gnostr_session_view_show_page(self, "timeline");
    return;
  }

  gnostr_session_view_show_page(self, page_name);
  g_signal_emit(self, signals[SIGNAL_PAGE_SELECTED], 0, page_name);
}

static void on_btn_settings_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrSessionView *self = GNOSTR_SESSION_VIEW(user_data);
  if (!GNOSTR_IS_SESSION_VIEW(self)) return;
  g_signal_emit(self, signals[SIGNAL_SETTINGS_REQUESTED], 0);
}

static void on_btn_relays_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrSessionView *self = GNOSTR_SESSION_VIEW(user_data);
  if (!GNOSTR_IS_SESSION_VIEW(self)) return;
  g_signal_emit(self, signals[SIGNAL_RELAYS_REQUESTED], 0);
}

static void on_btn_login_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrSessionView *self = GNOSTR_SESSION_VIEW(user_data);
  if (!GNOSTR_IS_SESSION_VIEW(self)) return;
  g_signal_emit(self, signals[SIGNAL_LOGIN_REQUESTED], 0);
}

static void on_btn_logout_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrSessionView *self = GNOSTR_SESSION_VIEW(user_data);
  if (!GNOSTR_IS_SESSION_VIEW(self)) return;
  g_signal_emit(self, signals[SIGNAL_LOGOUT_REQUESTED], 0);
}

static void on_btn_new_notes_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrSessionView *self = GNOSTR_SESSION_VIEW(user_data);
  if (!GNOSTR_IS_SESSION_VIEW(self)) return;
  g_signal_emit(self, signals[SIGNAL_NEW_NOTES_CLICKED], 0);
}

static void gnostr_session_view_dispose(GObject *object) {
  GnostrSessionView *self = GNOSTR_SESSION_VIEW(object);

  g_weak_ref_clear(&self->toast_overlay_ref);

  G_OBJECT_CLASS(gnostr_session_view_parent_class)->dispose(object);
}

static void gnostr_session_view_get_property(GObject *object,
                                             guint prop_id,
                                             GValue *value,
                                             GParamSpec *pspec) {
  GnostrSessionView *self = GNOSTR_SESSION_VIEW(object);

  switch ((GnostrSessionViewProperty)prop_id) {
    case PROP_COMPACT:
      g_value_set_boolean(value, self->compact);
      break;
    case PROP_AUTHENTICATED:
      g_value_set_boolean(value, self->authenticated);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static void gnostr_session_view_set_property(GObject *object,
                                             guint prop_id,
                                             const GValue *value,
                                             GParamSpec *pspec) {
  GnostrSessionView *self = GNOSTR_SESSION_VIEW(object);

  switch ((GnostrSessionViewProperty)prop_id) {
    case PROP_COMPACT:
      gnostr_session_view_set_compact(self, g_value_get_boolean(value));
      break;
    case PROP_AUTHENTICATED:
      gnostr_session_view_set_authenticated(self, g_value_get_boolean(value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static void gnostr_session_view_class_init(GnostrSessionViewClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = gnostr_session_view_dispose;
  object_class->get_property = gnostr_session_view_get_property;
  object_class->set_property = gnostr_session_view_set_property;

  props[PROP_COMPACT] = g_param_spec_boolean(
      "compact",
      "Compact",
      "Whether the session view is in compact mode (responsive layout)",
      FALSE,
      G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  props[PROP_AUTHENTICATED] = g_param_spec_boolean(
      "authenticated",
      "Authenticated",
      "Whether the user is authenticated (enables Notifications and Messages)",
      FALSE,
      G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties(object_class, N_PROPS, props);

  signals[SIGNAL_PAGE_SELECTED] = g_signal_new(
      "page-selected",
      G_TYPE_FROM_CLASS(klass),
      G_SIGNAL_RUN_LAST,
      0,
      NULL,
      NULL,
      g_cclosure_marshal_VOID__STRING,
      G_TYPE_NONE,
      1,
      G_TYPE_STRING);

  signals[SIGNAL_SETTINGS_REQUESTED] = g_signal_new(
      "settings-requested",
      G_TYPE_FROM_CLASS(klass),
      G_SIGNAL_RUN_LAST,
      0,
      NULL,
      NULL,
      g_cclosure_marshal_VOID__VOID,
      G_TYPE_NONE,
      0);

  signals[SIGNAL_RELAYS_REQUESTED] = g_signal_new(
      "relays-requested",
      G_TYPE_FROM_CLASS(klass),
      G_SIGNAL_RUN_LAST,
      0,
      NULL,
      NULL,
      g_cclosure_marshal_VOID__VOID,
      G_TYPE_NONE,
      0);

  signals[SIGNAL_LOGIN_REQUESTED] = g_signal_new(
      "login-requested",
      G_TYPE_FROM_CLASS(klass),
      G_SIGNAL_RUN_LAST,
      0,
      NULL,
      NULL,
      g_cclosure_marshal_VOID__VOID,
      G_TYPE_NONE,
      0);

  signals[SIGNAL_LOGOUT_REQUESTED] = g_signal_new(
      "logout-requested",
      G_TYPE_FROM_CLASS(klass),
      G_SIGNAL_RUN_LAST,
      0,
      NULL,
      NULL,
      g_cclosure_marshal_VOID__VOID,
      G_TYPE_NONE,
      0);

  signals[SIGNAL_NEW_NOTES_CLICKED] = g_signal_new(
      "new-notes-clicked",
      G_TYPE_FROM_CLASS(klass),
      G_SIGNAL_RUN_LAST,
      0,
      NULL,
      NULL,
      g_cclosure_marshal_VOID__VOID,
      G_TYPE_NONE,
      0);

  /* Ensure custom widget types used in the template are registered */
  g_type_ensure(GNOSTR_TYPE_TIMELINE_VIEW);
  g_type_ensure(GNOSTR_TYPE_NOTIFICATIONS_VIEW);
  g_type_ensure(GNOSTR_TYPE_DM_INBOX_VIEW);
  g_type_ensure(GNOSTR_TYPE_PAGE_DISCOVER);
  g_type_ensure(GNOSTR_TYPE_CLASSIFIEDS_VIEW);
  g_type_ensure(GNOSTR_TYPE_PROFILE_PANE);
  g_type_ensure(GNOSTR_TYPE_THREAD_VIEW);

  gtk_widget_class_set_template_from_resource(widget_class, UI_RESOURCE);

  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, session_overlay);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, split_view);

  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, sidebar_page);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, sidebar_header_bar);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, btn_settings);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, btn_menu);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, sidebar_scroller);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, sidebar_list);

  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, row_timeline);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, row_notifications);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, row_messages);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, row_discover);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, row_classifieds);

  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, content_page);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, toolbar_view);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, header_bar);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, btn_relays);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, btn_avatar);

  /* avatar_popover and its children are now created programmatically to avoid
   * GTK4 crash on Linux where GtkPopover in template causes gtk_widget_root assertion */

  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, content_root);

  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, new_notes_revealer);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, btn_new_notes);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, lbl_new_notes_count);

  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, panel_split);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, panel_container);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, profile_pane);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, thread_view);

  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, stack);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, timeline_scroller);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, timeline);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, notifications_view);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, dm_inbox);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, discover_page);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, classifieds_view);

  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, bottom_bar);
}

static void gnostr_session_view_init(GnostrSessionView *self) {
  g_weak_ref_init(&self->toast_overlay_ref, NULL);

  /* Defaults */
  self->compact = FALSE;
  self->authenticated = FALSE;
  self->showing_profile = TRUE;

  gtk_widget_init_template(GTK_WIDGET(self));

  /* Avatar popover will be created lazily in ensure_avatar_popover() to avoid
   * GTK4 crash on Linux where GtkPopover creation during template init causes
   * gtk_widget_root assertion failure */
  self->avatar_popover = NULL;
  self->lbl_signin_status = NULL;
  self->lbl_profile_name = NULL;
  self->btn_login = NULL;
  self->btn_logout = NULL;

  /* ESC closes profile/thread side panel when visible */
  GtkEventController *keys = gtk_event_controller_key_new();
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), self);
  gtk_widget_add_controller(GTK_WIDGET(self), keys);

  /* Wire up interactions */
  if (self->sidebar_list) {
    g_signal_connect(self->sidebar_list, "row-activated", G_CALLBACK(on_sidebar_row_activated), self);
  }

  if (self->btn_settings) {
    g_signal_connect(self->btn_settings, "clicked", G_CALLBACK(on_btn_settings_clicked), self);
  }

  if (self->btn_relays) {
    g_signal_connect(self->btn_relays, "clicked", G_CALLBACK(on_btn_relays_clicked), self);
  }

  if (self->btn_login) {
    g_signal_connect(self->btn_login, "clicked", G_CALLBACK(on_btn_login_clicked), self);
  }

  if (self->btn_logout) {
    g_signal_connect(self->btn_logout, "clicked", G_CALLBACK(on_btn_logout_clicked), self);
  }

  if (self->btn_new_notes) {
    g_signal_connect(self->btn_new_notes, "clicked", G_CALLBACK(on_btn_new_notes_clicked), self);
  }

  /* Start on Timeline by default */
  if (self->sidebar_list && self->row_timeline) {
    gtk_list_box_select_row(self->sidebar_list, self->row_timeline);
  }
  if (self->stack) {
    adw_view_stack_set_visible_child_name(self->stack, "timeline");
  }

  update_auth_gating(self);
}

/* ---- Public API ---- */

GnostrSessionView *gnostr_session_view_new(void) {
  return g_object_new(GNOSTR_TYPE_SESSION_VIEW, NULL);
}

gboolean gnostr_session_view_get_compact(GnostrSessionView *self) {
  g_return_val_if_fail(GNOSTR_IS_SESSION_VIEW(self), FALSE);
  return self->compact;
}

void gnostr_session_view_set_compact(GnostrSessionView *self, gboolean compact) {
  g_return_if_fail(GNOSTR_IS_SESSION_VIEW(self));

  compact = !!compact;
  if (self->compact == compact) return;

  self->compact = compact;
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_COMPACT]);
}

gboolean gnostr_session_view_get_authenticated(GnostrSessionView *self) {
  g_return_val_if_fail(GNOSTR_IS_SESSION_VIEW(self), FALSE);
  return self->authenticated;
}

void gnostr_session_view_set_authenticated(GnostrSessionView *self, gboolean authenticated) {
  g_return_if_fail(GNOSTR_IS_SESSION_VIEW(self));

  authenticated = !!authenticated;
  if (self->authenticated == authenticated) return;

  self->authenticated = authenticated;
  update_auth_gating(self);
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_AUTHENTICATED]);
}

void gnostr_session_view_show_page(GnostrSessionView *self, const char *page_name) {
  g_return_if_fail(GNOSTR_IS_SESSION_VIEW(self));
  g_return_if_fail(page_name != NULL);

  if (!self->stack) return;

  /* If caller requests a gated page in guest mode, bounce to timeline */
  if (!self->authenticated &&
      (g_strcmp0(page_name, "notifications") == 0 || g_strcmp0(page_name, "messages") == 0)) {
    g_signal_emit(self, signals[SIGNAL_LOGIN_REQUESTED], 0);
    gnostr_session_view_show_toast(self, _("Sign in to view this page."));
    page_name = "timeline";
  }

  adw_view_stack_set_visible_child_name(self->stack, page_name);

  if (self->content_page) {
    const char *title = title_for_page_name(page_name);
    if (title) adw_navigation_page_set_title(self->content_page, title);
  }

  if (self->sidebar_list) {
    GtkListBoxRow *row = row_for_page_name(self, page_name);
    if (row) gtk_list_box_select_row(self->sidebar_list, row);
  }
}

void gnostr_session_view_show_profile_panel(GnostrSessionView *self) {
  g_return_if_fail(GNOSTR_IS_SESSION_VIEW(self));
  if (!self->panel_split) return;

  if (self->thread_view) gtk_widget_set_visible(self->thread_view, FALSE);
  if (self->profile_pane) gtk_widget_set_visible(self->profile_pane, TRUE);

  self->showing_profile = TRUE;
  adw_overlay_split_view_set_show_sidebar(self->panel_split, TRUE);
}

void gnostr_session_view_show_thread_panel(GnostrSessionView *self) {
  g_return_if_fail(GNOSTR_IS_SESSION_VIEW(self));
  if (!self->panel_split) return;

  if (self->profile_pane) gtk_widget_set_visible(self->profile_pane, FALSE);
  if (self->thread_view) gtk_widget_set_visible(self->thread_view, TRUE);

  self->showing_profile = FALSE;
  adw_overlay_split_view_set_show_sidebar(self->panel_split, TRUE);
}

void gnostr_session_view_hide_side_panel(GnostrSessionView *self) {
  g_return_if_fail(GNOSTR_IS_SESSION_VIEW(self));
  if (!self->panel_split) return;

  adw_overlay_split_view_set_show_sidebar(self->panel_split, FALSE);
}

gboolean gnostr_session_view_is_side_panel_visible(GnostrSessionView *self) {
  g_return_val_if_fail(GNOSTR_IS_SESSION_VIEW(self), FALSE);
  if (!self->panel_split) return FALSE;

  return adw_overlay_split_view_get_show_sidebar(self->panel_split);
}

void gnostr_session_view_set_toast_overlay(GnostrSessionView *self, AdwToastOverlay *overlay) {
  g_return_if_fail(GNOSTR_IS_SESSION_VIEW(self));
  g_weak_ref_set(&self->toast_overlay_ref, overlay);
}

void gnostr_session_view_show_toast(GnostrSessionView *self, const char *message) {
  g_return_if_fail(GNOSTR_IS_SESSION_VIEW(self));
  if (!message || *message == '\0') return;

  AdwToastOverlay *overlay = g_weak_ref_get(&self->toast_overlay_ref);
  if (!overlay) return;

  AdwToast *toast = adw_toast_new(message);
  adw_toast_set_timeout(toast, 2);
  adw_toast_overlay_add_toast(overlay, toast);

  g_object_unref(overlay);
}

GtkWidget *gnostr_session_view_get_timeline(GnostrSessionView *self) {
  g_return_val_if_fail(GNOSTR_IS_SESSION_VIEW(self), NULL);
  return self->timeline;
}

GtkWidget *gnostr_session_view_get_notifications_view(GnostrSessionView *self) {
  g_return_val_if_fail(GNOSTR_IS_SESSION_VIEW(self), NULL);
  return self->notifications_view;
}

GtkWidget *gnostr_session_view_get_dm_inbox(GnostrSessionView *self) {
  g_return_val_if_fail(GNOSTR_IS_SESSION_VIEW(self), NULL);
  return self->dm_inbox;
}

GtkWidget *gnostr_session_view_get_discover_page(GnostrSessionView *self) {
  g_return_val_if_fail(GNOSTR_IS_SESSION_VIEW(self), NULL);
  return self->discover_page;
}

GtkWidget *gnostr_session_view_get_classifieds_view(GnostrSessionView *self) {
  g_return_val_if_fail(GNOSTR_IS_SESSION_VIEW(self), NULL);
  return self->classifieds_view;
}

GtkWidget *gnostr_session_view_get_profile_pane(GnostrSessionView *self) {
  g_return_val_if_fail(GNOSTR_IS_SESSION_VIEW(self), NULL);
  return self->profile_pane;
}

GtkWidget *gnostr_session_view_get_thread_view(GnostrSessionView *self) {
  g_return_val_if_fail(GNOSTR_IS_SESSION_VIEW(self), NULL);
  return self->thread_view;
}

gboolean gnostr_session_view_is_showing_profile(GnostrSessionView *self) {
  g_return_val_if_fail(GNOSTR_IS_SESSION_VIEW(self), FALSE);
  return self->showing_profile;
}

void gnostr_session_view_set_new_notes_count(GnostrSessionView *self, guint count) {
  g_return_if_fail(GNOSTR_IS_SESSION_VIEW(self));

  if (count > 0) {
    char *label_text = g_strdup_printf(ngettext("%u New Note", "%u New Notes", count), count);
    if (self->lbl_new_notes_count) {
      gtk_label_set_text(self->lbl_new_notes_count, label_text);
    }
    g_free(label_text);
    if (self->new_notes_revealer) {
      gtk_revealer_set_reveal_child(self->new_notes_revealer, TRUE);
    }
  } else {
    if (self->new_notes_revealer) {
      gtk_revealer_set_reveal_child(self->new_notes_revealer, FALSE);
    }
  }
}