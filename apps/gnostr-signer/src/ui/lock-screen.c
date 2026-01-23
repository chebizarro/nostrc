/* lock-screen.c - Lock screen widget implementation
 *
 * SPDX-License-Identifier: MIT
 *
 * Includes rate limiting display for brute force protection (nostrc-1g1).
 * Includes keyboard navigation support (nostrc-tz8w).
 */
#include "lock-screen.h"
#include "app-resources.h"
#include "../session-manager.h"
#include "../rate-limiter.h"
#include "../keyboard-nav.h"

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

  /* Rate limiting UI (nostrc-1g1) */
  GtkBox *box_rate_limit;
  GtkLabel *lbl_rate_limit_message;
  GtkLabel *lbl_rate_limit_countdown;
  guint lockout_timer_id;

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
static void update_rate_limit_ui(GnLockScreen *self);
static gboolean on_lockout_timer_tick(gpointer user_data);
static void on_rate_limit_exceeded(GnRateLimiter *limiter, guint lockout_seconds, gpointer user_data);
static void on_lockout_expired(GnRateLimiter *limiter, gpointer user_data);

static void
gn_lock_screen_dispose(GObject *object)
{
  GnLockScreen *self = GN_LOCK_SCREEN(object);

  /* Stop lockout timer (nostrc-1g1) */
  if (self->lockout_timer_id > 0) {
    g_source_remove(self->lockout_timer_id);
    self->lockout_timer_id = 0;
  }

  /* Disconnect rate limiter signals */
  GnRateLimiter *limiter = gn_rate_limiter_get_default();
  g_signal_handlers_disconnect_by_data(limiter, self);

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
  self->lockout_timer_id = 0;

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

  /* Create rate limit UI box (nostrc-1g1) */
  self->box_rate_limit = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 6));
  gtk_widget_add_css_class(GTK_WIDGET(self->box_rate_limit), "rate-limit-box");
  gtk_widget_set_visible(GTK_WIDGET(self->box_rate_limit), FALSE);

  self->lbl_rate_limit_message = GTK_LABEL(gtk_label_new(NULL));
  gtk_widget_add_css_class(GTK_WIDGET(self->lbl_rate_limit_message), "warning");
  gtk_label_set_wrap(self->lbl_rate_limit_message, TRUE);
  gtk_label_set_justify(self->lbl_rate_limit_message, GTK_JUSTIFY_CENTER);
  gtk_box_append(self->box_rate_limit, GTK_WIDGET(self->lbl_rate_limit_message));

  /* Set accessibility for rate limit message (nostrc-qfdg) */
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->lbl_rate_limit_message),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "Rate limit warning",
                                 GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, "Shows warning when too many failed authentication attempts have occurred",
                                 -1);

  self->lbl_rate_limit_countdown = GTK_LABEL(gtk_label_new(NULL));
  gtk_widget_add_css_class(GTK_WIDGET(self->lbl_rate_limit_countdown), "title-1");
  gtk_box_append(self->box_rate_limit, GTK_WIDGET(self->lbl_rate_limit_countdown));

  /* Set accessibility for countdown (nostrc-qfdg) */
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->lbl_rate_limit_countdown),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "Lockout countdown timer",
                                 GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, "Time remaining until you can try again",
                                 -1);

  /* Insert rate limit box before error label */
  gtk_box_insert_child_after(GTK_BOX(self), GTK_WIDGET(self->box_rate_limit),
                              GTK_WIDGET(self->entry_password));

  /* Connect to rate limiter signals (nostrc-1g1) */
  GnRateLimiter *limiter = gn_rate_limiter_get_default();
  g_signal_connect(limiter, "rate-limit-exceeded",
                   G_CALLBACK(on_rate_limit_exceeded), self);
  g_signal_connect(limiter, "lockout-expired",
                   G_CALLBACK(on_lockout_expired), self);

  /* Check initial rate limit state */
  update_rate_limit_ui(self);

  /* Setup keyboard navigation (nostrc-tz8w):
   * Connect Enter key in password entry to trigger unlock button */
  if (self->entry_password && self->btn_unlock) {
    gn_keyboard_nav_connect_enter_activate(GTK_WIDGET(self->entry_password),
                                            GTK_WIDGET(self->btn_unlock));
  }
}

/* Rate limiting UI functions (nostrc-1g1) */

static gchar *
format_countdown_time(guint seconds)
{
  if (seconds < 60) {
    return g_strdup_printf("%u second%s", seconds, seconds == 1 ? "" : "s");
  } else if (seconds < 3600) {
    guint mins = seconds / 60;
    guint secs = seconds % 60;
    if (secs > 0) {
      return g_strdup_printf("%u:%02u", mins, secs);
    } else {
      return g_strdup_printf("%u minute%s", mins, mins == 1 ? "" : "s");
    }
  } else {
    guint hours = seconds / 3600;
    guint mins = (seconds % 3600) / 60;
    return g_strdup_printf("%u:%02u:%02u", hours, mins, seconds % 60);
  }
}

static gboolean
on_lockout_timer_tick(gpointer user_data)
{
  GnLockScreen *self = GN_LOCK_SCREEN(user_data);

  GnRateLimiter *limiter = gn_rate_limiter_get_default();
  guint remaining = gn_rate_limiter_get_remaining_lockout(limiter);

  if (remaining == 0) {
    /* Lockout expired */
    self->lockout_timer_id = 0;
    update_rate_limit_ui(self);

    /* Announce unlock to screen readers (nostrc-qfdg) */
    gtk_accessible_update_property(GTK_ACCESSIBLE(self->lbl_rate_limit_countdown),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, "Lockout expired. You may now try again.",
                                   -1);
    return G_SOURCE_REMOVE;
  }

  /* Update countdown display */
  gchar *time_str = format_countdown_time(remaining);
  gtk_label_set_text(self->lbl_rate_limit_countdown, time_str);

  /* Update accessibility value for screen readers (nostrc-qfdg)
   * Only announce at key intervals to avoid spam */
  if (remaining == 60 || remaining == 30 || remaining == 10 || remaining <= 5) {
    gchar *accessible_value = g_strdup_printf("%s remaining", time_str);
    gtk_accessible_update_property(GTK_ACCESSIBLE(self->lbl_rate_limit_countdown),
                                   GTK_ACCESSIBLE_PROPERTY_VALUE_TEXT, accessible_value,
                                   -1);
    g_free(accessible_value);
  }

  g_free(time_str);

  return G_SOURCE_CONTINUE;
}

static void
update_rate_limit_ui(GnLockScreen *self)
{
  GnRateLimiter *limiter = gn_rate_limiter_get_default();

  if (gn_rate_limiter_is_locked_out(limiter)) {
    guint remaining = gn_rate_limiter_get_remaining_lockout(limiter);

    /* Show lockout UI */
    gtk_label_set_text(self->lbl_rate_limit_message,
                       "Too many failed authentication attempts.\nPlease wait before trying again:");

    gchar *time_str = format_countdown_time(remaining);
    gtk_label_set_text(self->lbl_rate_limit_countdown, time_str);
    g_free(time_str);

    gtk_widget_set_visible(GTK_WIDGET(self->box_rate_limit), TRUE);

    /* Disable input */
    gtk_widget_set_sensitive(GTK_WIDGET(self->entry_password), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(self->btn_unlock), FALSE);

    /* Start countdown timer if not already running */
    if (self->lockout_timer_id == 0) {
      self->lockout_timer_id = g_timeout_add_seconds(1, on_lockout_timer_tick, self);
    }
  } else {
    /* Hide lockout UI */
    gtk_widget_set_visible(GTK_WIDGET(self->box_rate_limit), FALSE);

    /* Enable input */
    gtk_widget_set_sensitive(GTK_WIDGET(self->entry_password), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(self->btn_unlock), TRUE);

    /* Stop countdown timer */
    if (self->lockout_timer_id > 0) {
      g_source_remove(self->lockout_timer_id);
      self->lockout_timer_id = 0;
    }

    /* Show remaining attempts if there have been failures */
    guint attempts_remaining = gn_rate_limiter_get_attempts_remaining(limiter);
    guint max_attempts = GN_RATE_LIMITER_DEFAULT_MAX_ATTEMPTS;
    if (attempts_remaining < max_attempts && attempts_remaining > 0) {
      gchar *msg = g_strdup_printf("%u attempt%s remaining",
                                   attempts_remaining, attempts_remaining == 1 ? "" : "s");
      gtk_label_set_text(self->lbl_rate_limit_message, msg);
      gtk_label_set_text(self->lbl_rate_limit_countdown, "");
      gtk_widget_set_visible(GTK_WIDGET(self->box_rate_limit), TRUE);
      g_free(msg);
    }
  }
}

static void
on_rate_limit_exceeded(GnRateLimiter *limiter, guint lockout_seconds, gpointer user_data)
{
  (void)limiter;
  (void)lockout_seconds;
  GnLockScreen *self = GN_LOCK_SCREEN(user_data);
  update_rate_limit_ui(self);
}

static void
on_lockout_expired(GnRateLimiter *limiter, gpointer user_data)
{
  (void)limiter;
  GnLockScreen *self = GN_LOCK_SCREEN(user_data);
  update_rate_limit_ui(self);

  /* Focus password entry when lockout expires */
  gn_lock_screen_focus_password(self);
}

static void
attempt_unlock(GnLockScreen *self)
{
  if (self->busy)
    return;

  /* Check rate limit before attempting (nostrc-1g1) */
  GnRateLimiter *limiter = gn_rate_limiter_get_default();
  if (gn_rate_limiter_is_locked_out(limiter)) {
    update_rate_limit_ui(self);
    return;
  }

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

    /* Update rate limit UI to show countdown if locked out (nostrc-1g1) */
    update_rate_limit_ui(self);

    /* Only focus if not locked out */
    if (!gn_rate_limiter_is_locked_out(limiter)) {
      gn_lock_screen_focus_password(self);
    }

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
  /* Announce error to screen readers via live region */
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->lbl_error),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, message,
                                 -1);
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

gboolean
gn_lock_screen_is_rate_limited(GnLockScreen *self)
{
  g_return_val_if_fail(GN_IS_LOCK_SCREEN(self), FALSE);

  GnRateLimiter *limiter = gn_rate_limiter_get_default();
  return gn_rate_limiter_is_locked_out(limiter);
}

guint
gn_lock_screen_get_rate_limit_remaining(GnLockScreen *self)
{
  g_return_val_if_fail(GN_IS_LOCK_SCREEN(self), 0);

  GnRateLimiter *limiter = gn_rate_limiter_get_default();
  return gn_rate_limiter_get_remaining_lockout(limiter);
}
