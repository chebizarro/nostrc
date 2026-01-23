/* lock-screen.c - Lock screen widget implementation
 *
 * SPDX-License-Identifier: MIT
 */
#include "lock-screen.h"
#include "app-resources.h"
#include "../session-manager.h"

#include <time.h>

struct _GnLockScreen {
  GtkBox parent_instance;

  /* Template widgets */
  GtkImage *icon_lock;
  GtkLabel *lbl_title;
  GtkLabel *lbl_subtitle;
  AdwPasswordEntryRow *entry_password;
  GtkLabel *lbl_error;
  GtkButton *btn_unlock;
  GtkBox *box_session_info;
  GtkLabel *lbl_lock_reason;
  GtkLabel *lbl_locked_at;

  /* State */
  GnLockReason lock_reason;
  gint64 locked_at;
  gboolean busy;
};

enum {
  SIGNAL_UNLOCK_REQUESTED,
  N_SIGNALS
};

static guint signals[N_SIGNALS] = { 0 };

G_DEFINE_FINAL_TYPE(GnLockScreen, gn_lock_screen, GTK_TYPE_BOX)

/* Forward declarations */
static void on_unlock_clicked(GtkButton *btn, gpointer user_data);
static void on_password_activate(AdwPasswordEntryRow *entry, gpointer user_data);
static void attempt_unlock(GnLockScreen *self);

static void
gn_lock_screen_dispose(GObject *object)
{
  /* GnLockScreen *self = GN_LOCK_SCREEN(object); */
  G_OBJECT_CLASS(gn_lock_screen_parent_class)->dispose(object);
}

static void
gn_lock_screen_class_init(GnLockScreenClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = gn_lock_screen_dispose;

  /* Setup template */
  gtk_widget_class_set_template_from_resource(widget_class,
                                              APP_RESOURCE_PATH "/ui/lock-screen.ui");

  gtk_widget_class_bind_template_child(widget_class, GnLockScreen, icon_lock);
  gtk_widget_class_bind_template_child(widget_class, GnLockScreen, lbl_title);
  gtk_widget_class_bind_template_child(widget_class, GnLockScreen, lbl_subtitle);
  gtk_widget_class_bind_template_child(widget_class, GnLockScreen, entry_password);
  gtk_widget_class_bind_template_child(widget_class, GnLockScreen, lbl_error);
  gtk_widget_class_bind_template_child(widget_class, GnLockScreen, btn_unlock);
  gtk_widget_class_bind_template_child(widget_class, GnLockScreen, box_session_info);
  gtk_widget_class_bind_template_child(widget_class, GnLockScreen, lbl_lock_reason);
  gtk_widget_class_bind_template_child(widget_class, GnLockScreen, lbl_locked_at);

  /**
   * GnLockScreen::unlock-requested:
   * @self: The lock screen
   *
   * Emitted when the user requests to unlock the session.
   * The handler should call gn_session_manager_authenticate() and
   * then call gn_lock_screen_show_error() if authentication fails.
   */
  signals[SIGNAL_UNLOCK_REQUESTED] =
    g_signal_new("unlock-requested",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 0);
}

static void
gn_lock_screen_init(GnLockScreen *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));

  /* Initialize state */
  self->lock_reason = GN_LOCK_REASON_STARTUP;
  self->locked_at = (gint64)time(NULL);
  self->busy = FALSE;

  /* Connect signals */
  g_signal_connect(self->btn_unlock, "clicked",
                   G_CALLBACK(on_unlock_clicked), self);
  g_signal_connect(self->entry_password, "apply",
                   G_CALLBACK(on_password_activate), self);

  /* Check if password is configured */
  GnSessionManager *sm = gn_session_manager_get_default();
  if (!gn_session_manager_has_password(sm)) {
    /* No password required - show different UI */
    gtk_label_set_text(self->lbl_subtitle, "Click to unlock");
    gtk_widget_set_visible(GTK_WIDGET(self->entry_password), FALSE);
    gtk_button_set_label(self->btn_unlock, "Unlock");
  }
}

static void
attempt_unlock(GnLockScreen *self)
{
  if (self->busy)
    return;

  GnSessionManager *sm = gn_session_manager_get_default();
  GError *error = NULL;
  gboolean success;

  /* Clear any previous error */
  gn_lock_screen_clear_error(self);

  /* Set busy state */
  gn_lock_screen_set_busy(self, TRUE);

  if (gn_session_manager_has_password(sm)) {
    /* Get password from entry */
    const gchar *password = gtk_editable_get_text(GTK_EDITABLE(self->entry_password));

    if (!password || !*password) {
      gn_lock_screen_show_error(self, "Please enter your password");
      gn_lock_screen_set_busy(self, FALSE);
      return;
    }

    success = gn_session_manager_authenticate(sm, password, &error);
  } else {
    /* No password configured - just authenticate */
    success = gn_session_manager_authenticate(sm, NULL, &error);
  }

  gn_lock_screen_set_busy(self, FALSE);

  if (!success) {
    gn_lock_screen_show_error(self, error ? error->message : "Authentication failed");
    gn_lock_screen_clear_password(self);
    gn_lock_screen_focus_password(self);
    g_clear_error(&error);
    return;
  }

  /* Success - clear password and emit signal */
  gn_lock_screen_clear_password(self);
  g_signal_emit(self, signals[SIGNAL_UNLOCK_REQUESTED], 0);
}

static void
on_unlock_clicked(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  GnLockScreen *self = GN_LOCK_SCREEN(user_data);
  attempt_unlock(self);
}

static void
on_password_activate(AdwPasswordEntryRow *entry, gpointer user_data)
{
  (void)entry;
  GnLockScreen *self = GN_LOCK_SCREEN(user_data);
  attempt_unlock(self);
}

/* Public API */

GnLockScreen *
gn_lock_screen_new(void)
{
  return g_object_new(GN_TYPE_LOCK_SCREEN, NULL);
}

void
gn_lock_screen_set_lock_reason(GnLockScreen *self, GnLockReason reason)
{
  g_return_if_fail(GN_IS_LOCK_SCREEN(self));

  self->lock_reason = reason;

  const gchar *reason_text;
  switch (reason) {
    case GN_LOCK_REASON_MANUAL:
      reason_text = "Manually locked";
      break;
    case GN_LOCK_REASON_TIMEOUT:
      reason_text = "Locked due to inactivity";
      break;
    case GN_LOCK_REASON_STARTUP:
      reason_text = "Session started locked";
      break;
    case GN_LOCK_REASON_SYSTEM_IDLE:
      reason_text = "Locked due to system idle";
      break;
    default:
      reason_text = "Session locked";
      break;
  }

  gtk_label_set_text(self->lbl_lock_reason, reason_text);
  gtk_widget_set_visible(GTK_WIDGET(self->box_session_info), TRUE);
}

void
gn_lock_screen_set_locked_at(GnLockScreen *self, gint64 timestamp)
{
  g_return_if_fail(GN_IS_LOCK_SCREEN(self));

  self->locked_at = timestamp;

  time_t t = (time_t)timestamp;
  struct tm *tm_info = localtime(&t);
  char buffer[32];
  strftime(buffer, sizeof(buffer), "Locked at %H:%M", tm_info);

  gtk_label_set_text(self->lbl_locked_at, buffer);
  gtk_widget_set_visible(GTK_WIDGET(self->box_session_info), TRUE);
}

void
gn_lock_screen_clear_error(GnLockScreen *self)
{
  g_return_if_fail(GN_IS_LOCK_SCREEN(self));

  gtk_label_set_text(self->lbl_error, "");
  gtk_widget_set_visible(GTK_WIDGET(self->lbl_error), FALSE);
}

void
gn_lock_screen_show_error(GnLockScreen *self, const gchar *message)
{
  g_return_if_fail(GN_IS_LOCK_SCREEN(self));

  if (!message || !*message) {
    gn_lock_screen_clear_error(self);
    return;
  }

  gtk_label_set_text(self->lbl_error, message);
  gtk_widget_set_visible(GTK_WIDGET(self->lbl_error), TRUE);
}

void
gn_lock_screen_focus_password(GnLockScreen *self)
{
  g_return_if_fail(GN_IS_LOCK_SCREEN(self));

  if (gtk_widget_get_visible(GTK_WIDGET(self->entry_password))) {
    gtk_widget_grab_focus(GTK_WIDGET(self->entry_password));
  } else {
    gtk_widget_grab_focus(GTK_WIDGET(self->btn_unlock));
  }
}

void
gn_lock_screen_clear_password(GnLockScreen *self)
{
  g_return_if_fail(GN_IS_LOCK_SCREEN(self));

  gtk_editable_set_text(GTK_EDITABLE(self->entry_password), "");
}

void
gn_lock_screen_set_busy(GnLockScreen *self, gboolean busy)
{
  g_return_if_fail(GN_IS_LOCK_SCREEN(self));

  self->busy = busy;

  gtk_widget_set_sensitive(GTK_WIDGET(self->entry_password), !busy);
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_unlock), !busy);

  if (busy) {
    gtk_button_set_label(self->btn_unlock, "Unlocking...");
  } else {
    gtk_button_set_label(self->btn_unlock, "_Unlock");
  }
}

gboolean
gn_lock_screen_get_password_configured(GnLockScreen *self)
{
  g_return_val_if_fail(GN_IS_LOCK_SCREEN(self), FALSE);

  GnSessionManager *sm = gn_session_manager_get_default();
  return gn_session_manager_has_password(sm);
}
