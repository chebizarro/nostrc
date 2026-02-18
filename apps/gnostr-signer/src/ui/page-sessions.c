/* page-sessions.c - UI page for managing active client sessions
 *
 * SPDX-License-Identifier: MIT
 */
#include "page-sessions.h"
#include "../client_session.h"
#include "app-resources.h"

#include <time.h>

struct _GnPageSessions {
  AdwPreferencesPage parent_instance;

  /* Template widgets */
  GtkListBox *session_list;
  GtkButton *btn_revoke_all;
  GtkLabel *lbl_active_count;
  AdwSpinRow *spin_timeout;
  GtkStack *empty_stack;

  /* State */
  gulong session_created_handler;
  gulong session_expired_handler;
  gulong session_revoked_handler;
  gulong session_activity_handler;
  guint refresh_timer_id;
};

G_DEFINE_FINAL_TYPE(GnPageSessions, gn_page_sessions, ADW_TYPE_PREFERENCES_PAGE)

/* Forward declarations */
static void populate_session_list(GnPageSessions *self);
static GtkWidget *create_session_row(GnClientSession *session);
static void update_active_count(GnPageSessions *self);
static gboolean on_refresh_timer(gpointer user_data);

/* Signal handlers for session manager events */
static void
on_session_created(GnClientSessionManager *manager,
                   GnClientSession *session,
                   gpointer user_data)
{
  (void)manager;
  (void)session;
  GnPageSessions *self = GN_PAGE_SESSIONS(user_data);
  populate_session_list(self);
}

static void
on_session_expired(GnClientSessionManager *manager,
                   GnClientSession *session,
                   gpointer user_data)
{
  (void)manager;
  (void)session;
  GnPageSessions *self = GN_PAGE_SESSIONS(user_data);
  populate_session_list(self);
}

static void
on_session_revoked(GnClientSessionManager *manager,
                   GnClientSession *session,
                   gpointer user_data)
{
  (void)manager;
  (void)session;
  GnPageSessions *self = GN_PAGE_SESSIONS(user_data);
  populate_session_list(self);
}

static void
on_session_activity(GnClientSessionManager *manager,
                    GnClientSession *session,
                    gpointer user_data)
{
  (void)manager;
  (void)session;
  GnPageSessions *self = GN_PAGE_SESSIONS(user_data);
  /* Just update the count and times - full refresh on timer */
  update_active_count(self);
}

/* Button click handlers */
static void
on_revoke_all_clicked(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  GnPageSessions *self = GN_PAGE_SESSIONS(user_data);

  GnClientSessionManager *mgr = gn_client_session_manager_get_default();
  guint count = gn_client_session_manager_revoke_all(mgr);

  g_debug("page-sessions: Revoked %u sessions", count);
  populate_session_list(self);
}

static void
on_revoke_session_clicked(GtkButton *btn, gpointer user_data)
{
  GnClientSession *session = GN_CLIENT_SESSION(user_data);
  (void)btn;

  const gchar *pubkey = gn_client_session_get_client_pubkey(session);
  const gchar *identity = gn_client_session_get_identity(session);

  GnClientSessionManager *mgr = gn_client_session_manager_get_default();
  gn_client_session_manager_revoke_session(mgr, pubkey, identity);
}

static void
on_timeout_changed(AdwSpinRow *spin, GParamSpec *pspec, gpointer user_data)
{
  (void)pspec;
  (void)user_data;

  guint value = (guint)adw_spin_row_get_value(spin);
  GnClientSessionManager *mgr = gn_client_session_manager_get_default();
  gn_client_session_manager_set_timeout(mgr, value * 60);  /* Convert minutes to seconds */
}

/* Format time duration as human-readable string */
static gchar *
format_duration(guint seconds)
{
  if (seconds == 0) {
    return g_strdup("Expired");
  }

  if (seconds == G_MAXUINT) {
    return g_strdup("No timeout");
  }

  if (seconds < 60) {
    return g_strdup_printf("%u sec", seconds);
  }

  if (seconds < 3600) {
    guint mins = seconds / 60;
    return g_strdup_printf("%u min", mins);
  }

  guint hours = seconds / 3600;
  guint mins = (seconds % 3600) / 60;

  if (mins > 0) {
    return g_strdup_printf("%uh %um", hours, mins);
  }

  return g_strdup_printf("%u hr", hours);
}

/* Format timestamp as relative time */
static gchar *
format_relative_time(gint64 timestamp)
{
  gint64 now = (gint64)time(NULL);
  gint64 diff = now - timestamp;

  if (diff < 0) {
    return g_strdup("In the future");
  }

  if (diff < 60) {
    return g_strdup("Just now");
  }

  if (diff < 3600) {
    guint mins = (guint)(diff / 60);
    return g_strdup_printf("%u min ago", mins);
  }

  if (diff < 86400) {
    guint hours = (guint)(diff / 3600);
    return g_strdup_printf("%u hr ago", hours);
  }

  guint days = (guint)(diff / 86400);
  return g_strdup_printf("%u day%s ago", days, days == 1 ? "" : "s");
}

/* Create a row widget for a session */
static GtkWidget *
create_session_row(GnClientSession *session)
{
  const gchar *app_name = gn_client_session_get_app_name(session);
  const gchar *client_pubkey = gn_client_session_get_client_pubkey(session);
  const gchar *identity = gn_client_session_get_identity(session);
  GnClientSessionState state = gn_client_session_get_state(session);
  guint remaining = gn_client_session_get_remaining_time(session);
  gint64 last_activity = gn_client_session_get_last_activity(session);
  guint request_count = gn_client_session_get_request_count(session);
  gboolean persistent = gn_client_session_is_persistent(session);

  /* Create row */
  AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());

  /* Title: app name or truncated pubkey */
  if (app_name && *app_name) {
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), app_name);
  } else {
    /* Truncate pubkey for display */
    gchar *short_pk = g_strndup(client_pubkey, 16);
    g_autofree gchar *title = g_strdup_printf("%s...", short_pk);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    g_free(short_pk);
  }

  /* Subtitle: identity and status */
  gchar *remaining_str = format_duration(remaining);
  gchar *activity_str = format_relative_time(last_activity);

  const gchar *state_str = "";
  switch (state) {
    case GN_CLIENT_SESSION_ACTIVE:
      state_str = persistent ? "Active (remembered)" : "Active";
      break;
    case GN_CLIENT_SESSION_EXPIRED:
      state_str = "Expired";
      break;
    case GN_CLIENT_SESSION_REVOKED:
      state_str = "Revoked";
      break;
    case GN_CLIENT_SESSION_PENDING:
      state_str = "Pending";
      break;
  }

  /* Truncate identity for display */
  g_autofree gchar *short_identity = NULL;
  if (identity && strlen(identity) > 20) {
    short_identity = g_strdup_printf("%.12s...%.4s",
                                     identity,
                                     identity + strlen(identity) - 4);
  } else {
    short_identity = g_strdup(identity ? identity : "Unknown");
  }

  g_autofree gchar *subtitle = g_strdup_printf("%s | %s | %s | %u requests | Last: %s",
                                    short_identity,
                                    state_str,
                                    remaining_str,
                                    request_count,
                                    activity_str);

  adw_action_row_set_subtitle(row, subtitle);

  g_free(remaining_str);
  g_free(activity_str);

  /* Add status icon */
  GtkWidget *status_icon = gtk_image_new();
  switch (state) {
    case GN_CLIENT_SESSION_ACTIVE:
      gtk_image_set_from_icon_name(GTK_IMAGE(status_icon),
                                   persistent ? "starred-symbolic" : "media-playback-start-symbolic");
      gtk_widget_add_css_class(status_icon, "success");
      break;
    case GN_CLIENT_SESSION_EXPIRED:
      gtk_image_set_from_icon_name(GTK_IMAGE(status_icon), "appointment-soon-symbolic");
      gtk_widget_add_css_class(status_icon, "warning");
      break;
    case GN_CLIENT_SESSION_REVOKED:
      gtk_image_set_from_icon_name(GTK_IMAGE(status_icon), "action-unavailable-symbolic");
      gtk_widget_add_css_class(status_icon, "error");
      break;
    case GN_CLIENT_SESSION_PENDING:
      gtk_image_set_from_icon_name(GTK_IMAGE(status_icon), "content-loading-symbolic");
      break;
  }
  adw_action_row_add_prefix(row, status_icon);

  /* Add revoke button for active sessions */
  if (state == GN_CLIENT_SESSION_ACTIVE) {
    GtkWidget *revoke_btn = gtk_button_new_from_icon_name("user-trash-symbolic");
    gtk_widget_set_valign(revoke_btn, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(revoke_btn, "flat");
    gtk_widget_add_css_class(revoke_btn, "circular");
    gtk_widget_set_tooltip_text(revoke_btn, "Revoke this session");

    g_signal_connect(revoke_btn, "clicked",
                     G_CALLBACK(on_revoke_session_clicked),
                     session);

    adw_action_row_add_suffix(row, revoke_btn);
  }

  /* Add chevron for navigation hint */
  GtkWidget *chevron = gtk_image_new_from_icon_name("go-next-symbolic");
  gtk_widget_add_css_class(chevron, "dim-label");
  adw_action_row_add_suffix(row, chevron);

  return GTK_WIDGET(row);
}

/* Populate the session list */
static void
populate_session_list(GnPageSessions *self)
{
  /* Clear existing rows */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->session_list))) != NULL) {
    gtk_list_box_remove(self->session_list, child);
  }

  /* Get all sessions */
  GnClientSessionManager *mgr = gn_client_session_manager_get_default();
  GPtrArray *sessions = gn_client_session_manager_list_sessions(mgr);

  if (!sessions || sessions->len == 0) {
    gtk_stack_set_visible_child_name(self->empty_stack, "empty");
    if (sessions)
      g_ptr_array_unref(sessions);
    update_active_count(self);
    return;
  }

  gtk_stack_set_visible_child_name(self->empty_stack, "list");

  /* Add rows for each session */
  for (guint i = 0; i < sessions->len; i++) {
    GnClientSession *session = g_ptr_array_index(sessions, i);
    GtkWidget *row = create_session_row(session);
    gtk_list_box_append(self->session_list, row);
  }

  g_ptr_array_unref(sessions);
  update_active_count(self);
}

/* Update active session count label */
static void
update_active_count(GnPageSessions *self)
{
  GnClientSessionManager *mgr = gn_client_session_manager_get_default();
  guint active = gn_client_session_manager_get_active_count(mgr);
  guint total = gn_client_session_manager_get_session_count(mgr);

  g_autofree gchar *text = g_strdup_printf("%u active / %u total", active, total);
  gtk_label_set_text(self->lbl_active_count, text);

  /* Enable/disable revoke all button */
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_revoke_all), active > 0);
}

/* Timer callback for refreshing remaining times */
static gboolean
on_refresh_timer(gpointer user_data)
{
  GnPageSessions *self = GN_PAGE_SESSIONS(user_data);

  /* Refresh the list to update remaining times */
  populate_session_list(self);

  return G_SOURCE_CONTINUE;
}

static void
gn_page_sessions_dispose(GObject *object)
{
  GnPageSessions *self = GN_PAGE_SESSIONS(object);

  /* Disconnect signal handlers */
  GnClientSessionManager *mgr = gn_client_session_manager_get_default();

  if (self->session_created_handler > 0) {
    g_signal_handler_disconnect(mgr, self->session_created_handler);
    self->session_created_handler = 0;
  }
  if (self->session_expired_handler > 0) {
    g_signal_handler_disconnect(mgr, self->session_expired_handler);
    self->session_expired_handler = 0;
  }
  if (self->session_revoked_handler > 0) {
    g_signal_handler_disconnect(mgr, self->session_revoked_handler);
    self->session_revoked_handler = 0;
  }
  if (self->session_activity_handler > 0) {
    g_signal_handler_disconnect(mgr, self->session_activity_handler);
    self->session_activity_handler = 0;
  }

  /* Stop refresh timer */
  if (self->refresh_timer_id > 0) {
    g_source_remove(self->refresh_timer_id);
    self->refresh_timer_id = 0;
  }

  G_OBJECT_CLASS(gn_page_sessions_parent_class)->dispose(object);
}

static void
gn_page_sessions_class_init(GnPageSessionsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = gn_page_sessions_dispose;

  gtk_widget_class_set_template_from_resource(widget_class,
                                              APP_RESOURCE_PATH "/ui/page-sessions.ui");

  gtk_widget_class_bind_template_child(widget_class, GnPageSessions, session_list);
  gtk_widget_class_bind_template_child(widget_class, GnPageSessions, btn_revoke_all);
  gtk_widget_class_bind_template_child(widget_class, GnPageSessions, lbl_active_count);
  gtk_widget_class_bind_template_child(widget_class, GnPageSessions, spin_timeout);
  gtk_widget_class_bind_template_child(widget_class, GnPageSessions, empty_stack);
}

static void
gn_page_sessions_init(GnPageSessions *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));

  /* Connect button signals */
  g_signal_connect(self->btn_revoke_all, "clicked",
                   G_CALLBACK(on_revoke_all_clicked), self);

  /* Connect timeout spinner */
  g_signal_connect(self->spin_timeout, "notify::value",
                   G_CALLBACK(on_timeout_changed), self);

  /* Set initial timeout value from settings */
  GnClientSessionManager *mgr = gn_client_session_manager_get_default();
  guint timeout = gn_client_session_manager_get_timeout(mgr);
  adw_spin_row_set_value(self->spin_timeout, (double)(timeout / 60));  /* Convert to minutes */

  /* Connect to session manager signals */
  self->session_created_handler = g_signal_connect(mgr, "session-created",
                                                   G_CALLBACK(on_session_created), self);
  self->session_expired_handler = g_signal_connect(mgr, "session-expired",
                                                   G_CALLBACK(on_session_expired), self);
  self->session_revoked_handler = g_signal_connect(mgr, "session-revoked",
                                                   G_CALLBACK(on_session_revoked), self);
  self->session_activity_handler = g_signal_connect(mgr, "session-activity",
                                                    G_CALLBACK(on_session_activity), self);

  /* Start refresh timer (every 10 seconds to update remaining times) */
  self->refresh_timer_id = g_timeout_add_seconds(10, on_refresh_timer, self);

  /* Initial population */
  populate_session_list(self);
}

GnPageSessions *
gn_page_sessions_new(void)
{
  return g_object_new(GN_TYPE_PAGE_SESSIONS, NULL);
}

void
gn_page_sessions_refresh(GnPageSessions *self)
{
  g_return_if_fail(GN_IS_PAGE_SESSIONS(self));
  populate_session_list(self);
}
