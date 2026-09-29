#include "gh-background.h"

/* GH_BACKGROUND_AUTOSTART_TEMPLATE: the configured
 * data/org.nostr.Groundhog-autostart.desktop.in (CMake generates it). */
#include "gh-background-autostart.h"

#include <adwaita.h>
#include <errno.h>
#include <glib/gstdio.h>

#if GH_BACKGROUND_WITH_STORE
#include "gh-account-store.h"
#endif

#define RUN_IN_BACKGROUND "run-in-background"
#define APP_DATA_KEY "gh-background"
#define OWN_ENTRY_KEY "X-Groundhog-Autostart"

#define PORTAL_BUS "org.freedesktop.portal.Desktop"
#define PORTAL_PATH "/org/freedesktop/portal/desktop"
#define PORTAL_BACKGROUND "org.freedesktop.portal.Background"
#define PORTAL_REQUEST "org.freedesktop.portal.Request"
#define PORTAL_REASON N_("Receive messages while the window is closed")

#define RESPONSE_QUIT "quit"
#define RESPONSE_BACKGROUND "background"

/* The key as the user left it: only a user value is a confirmed choice. */
typedef enum {
  KEY_DEFAULT, /* no user value: the default (on) is in effect, unconfirmed */
  KEY_ON,
  KEY_OFF,
} KeyState;

struct _GhBackground {
  GObject parent_instance;
  GApplication *app; /* borrowed: it owns the process's services, us included */
  GSettings *settings;
  GDBusConnection *connection; /* nullable */
  GhBackgroundMethod method;
  gchar *autostart_dir;
  gchar *state_dir;
  GObject *account_store; /* nullable */
  GCancellable *cancellable;
  gboolean enabled;
  gboolean holding;
  gboolean explained;
  gboolean disposed;
  gchar *status;
  gchar *sent_status;
  guint portal_version; /* 0 until known */

  /* Autostart reconciliation: one at a time, the latest key state wins. */
  KeyState target;  /* the state of the running or queued reconciliation */
  gboolean in_flight;
  gboolean again;
  gboolean requested_autostart;
  GPtrArray *waiters; /* GTask from gh_background_set_enabled_async() */
  guint response_subscription;
  gchar *request_path;
  guint token_serial;

  /* The one-time explanation. */
  AdwDialog *dialog;    /* weak */
  GtkWindow *closing;   /* weak */
};

enum { PROP_0, PROP_ENABLED, PROP_HOLDING, PROP_STATUS, PROP_EXPLAINED, N_PROPS };
static GParamSpec *props[N_PROPS];

G_DEFINE_FINAL_TYPE(GhBackground, gh_background, G_TYPE_OBJECT)

static void request_reconcile(GhBackground *self);

/* ---- key, hold, status ----------------------------------------------------------- */

static KeyState
key_state(GhBackground *self)
{
  g_autoptr(GVariant) user = g_settings_get_user_value(self->settings, RUN_IN_BACKGROUND);
  if (!user)
    return KEY_DEFAULT;
  return g_variant_get_boolean(user) ? KEY_ON : KEY_OFF;
}

/* The one hold of background mode: taken while the key is on. */
static void
sync_hold(GhBackground *self)
{
  gboolean want = self->enabled && !self->disposed;
  if (want == self->holding)
    return;
  self->holding = want;
  if (want)
    g_application_hold(self->app);
  else
    g_application_release(self->app);
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_HOLDING]);
}

static const gchar *
compute_status(GhBackground *self)
{
  if (!self->enabled)
    return NULL;
#if GH_BACKGROUND_WITH_STORE
  if (self->account_store) {
    switch (gh_account_store_get_state(GH_ACCOUNT_STORE(self->account_store))) {
    case GH_ACCOUNT_STORE_INACTIVE:
      return _(GH_BACKGROUND_STATUS_NO_ACCOUNT);
    case GH_ACCOUNT_STORE_OPENING:
      /* The key lookup has not answered: nothing new is known. "Receiving"
       * would be untrue if it finds the keyring locked (NO-11), and the
       * desktop would show it whenever the portal answers first (under load;
       * nostrc-yzlp). Keep what was said (nothing, at start). */
      return self->status;
    case GH_ACCOUNT_STORE_OPEN:
    case GH_ACCOUNT_STORE_EPHEMERAL:
      return _(GH_BACKGROUND_STATUS_RECEIVING);
    case GH_ACCOUNT_STORE_LOCKED:
      return _(GH_BACKGROUND_STATUS_LOCKED);
    default:
      return _(GH_BACKGROUND_STATUS_STOPPED);
    }
  }
#endif
  return _(GH_BACKGROUND_STATUS_RECEIVING);
}

static void
on_status_sent(GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result,
                                                            &error);
  (void)data;
  if (!reply && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_debug("Groundhog could not set its background status: %s", error->message);
}

/* SetStatus is Background version 2, and only for sandboxed callers. */
static void
send_status(GhBackground *self)
{
  if (self->disposed || self->method != GH_BACKGROUND_METHOD_PORTAL || !self->connection ||
      self->portal_version < 2 || !self->status ||
      g_strcmp0(self->status, self->sent_status) == 0)
    return;
  g_free(self->sent_status);
  self->sent_status = g_strdup(self->status);
  GVariantBuilder options;
  g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&options, "{sv}", "message", g_variant_new_string(self->status));
  g_dbus_connection_call(self->connection, PORTAL_BUS, PORTAL_PATH, PORTAL_BACKGROUND,
                         "SetStatus", g_variant_new("(a{sv})", &options), NULL,
                         G_DBUS_CALL_FLAGS_NONE, -1, self->cancellable, on_status_sent, NULL);
}

static void
update_status(GhBackground *self)
{
  const gchar *status = compute_status(self);
  if (g_strcmp0(status, self->status) != 0) {
    g_free(self->status);
    self->status = g_strdup(status);
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_STATUS]);
  }
  send_status(self);
}

static void
on_portal_version(GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GhBackground) self = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result,
                                                            &error);
  if (self->disposed)
    return;
  if (!reply) {
    g_debug("Groundhog found no background portal: %s", error->message);
    return;
  }
  g_autoptr(GVariant) value = NULL;
  g_variant_get(reply, "(v)", &value);
  if (g_variant_is_of_type(value, G_VARIANT_TYPE_UINT32))
    self->portal_version = g_variant_get_uint32(value);
  send_status(self);
}

static void
on_settings_changed(GSettings *settings, const gchar *key, GhBackground *self)
{
  (void)key;
  gboolean enabled = g_settings_get_boolean(settings, RUN_IN_BACKGROUND);
  if (enabled != self->enabled) {
    self->enabled = enabled;
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_ENABLED]);
  }
  sync_hold(self);
  update_status(self);
  if (key_state(self) != self->target)
    request_reconcile(self);
}

/* ---- explanation marker ---------------------------------------------------------- */

static gchar *
explained_path(GhBackground *self)
{
  return g_build_filename(self->state_dir, GH_BACKGROUND_EXPLAINED_FILE, NULL);
}

static void
mark_explained(GhBackground *self)
{
  if (self->explained)
    return;
  self->explained = TRUE;
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_EXPLAINED]);
  g_autoptr(GError) error = NULL;
  g_autofree gchar *path = explained_path(self);
  if (g_mkdir_with_parents(self->state_dir, 0700) != 0 ||
      !g_file_set_contents_full(path, "1\n", -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, &error))
    g_message("Groundhog could not remember that background mode was explained: %s",
              error ? error->message : g_strerror(errno));
}

/* ---- autostart: the host file ------------------------------------------------------ */

static gboolean
file_apply(GhBackground *self, gboolean want, GError **error)
{
  g_autofree gchar *path = g_build_filename(self->autostart_dir, GH_BACKGROUND_AUTOSTART_FILE,
                                            NULL);
  g_autofree gchar *current = NULL;
  gsize length = 0;
  gboolean exists = g_file_get_contents(path, &current, &length, NULL);
  gboolean ours = FALSE, switched_off = FALSE;
  if (exists) {
    g_autoptr(GKeyFile) entry = g_key_file_new();
    if (g_key_file_load_from_data(entry, current, length, G_KEY_FILE_NONE, NULL)) {
      const gchar *group = G_KEY_FILE_DESKTOP_GROUP;
      ours = g_key_file_get_boolean(entry, group, OWN_ENTRY_KEY, NULL);
      switched_off = g_key_file_get_boolean(entry, group, G_KEY_FILE_DESKTOP_KEY_HIDDEN, NULL) ||
                     (g_key_file_has_key(entry, group, "X-GNOME-Autostart-enabled", NULL) &&
                      !g_key_file_get_boolean(entry, group, "X-GNOME-Autostart-enabled", NULL));
    }
  }
  /* Someone else's entry under our name (e.g. one the user made): theirs. */
  if (exists && !ours)
    return TRUE;
  if (!want) {
    if (exists && g_unlink(path) != 0 && errno != ENOENT) {
      int saved = errno;
      g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved),
                  "Could not remove the autostart entry: %s", g_strerror(saved));
      return FALSE;
    }
    return TRUE;
  }
  /* Switched off in the desktop's startup settings: that wins. */
  if (exists && (switched_off || g_str_equal(current, GH_BACKGROUND_AUTOSTART_TEMPLATE)))
    return TRUE;
  if (g_mkdir_with_parents(self->autostart_dir, 0700) != 0) {
    int saved = errno;
    g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved),
                "Could not create the autostart folder: %s", g_strerror(saved));
    return FALSE;
  }
  return g_file_set_contents_full(path, GH_BACKGROUND_AUTOSTART_TEMPLATE, -1,
                                  G_FILE_SET_CONTENTS_CONSISTENT, 0600, error);
}

/* ---- autostart: the portal ----------------------------------------------------------- */

static void reconcile_done(GhBackground *self, GError *error);

static void
unsubscribe_response(GhBackground *self)
{
  if (self->response_subscription && self->connection)
    g_dbus_connection_signal_unsubscribe(self->connection, self->response_subscription);
  self->response_subscription = 0;
}

static void
on_portal_response(GDBusConnection *connection, const gchar *sender, const gchar *path,
                   const gchar *interface, const gchar *signal, GVariant *parameters,
                   gpointer data)
{
  GhBackground *self = data;
  (void)connection; (void)sender; (void)interface; (void)signal;
  if (self->disposed || g_strcmp0(path, self->request_path) != 0 ||
      !g_variant_is_of_type(parameters, G_VARIANT_TYPE("(ua{sv})")))
    return;
  guint32 response = 2;
  g_autoptr(GVariant) results = NULL;
  g_variant_get(parameters, "(u@a{sv})", &response, &results);
  unsubscribe_response(self);
  g_clear_pointer(&self->request_path, g_free);

  gboolean background = FALSE, autostart = FALSE;
  g_variant_lookup(results, "background", "b", &background);
  g_variant_lookup(results, "autostart", "b", &autostart);
  GError *error = NULL;
  if (response != 0 || !background) {
    error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                "The system did not allow Groundhog to run in the background");
    /* Honest state: the system will not let it run without a window. */
    if (g_settings_get_boolean(self->settings, RUN_IN_BACKGROUND)) {
      self->target = KEY_OFF; /* already what the portal was told */
      g_settings_set_boolean(self->settings, RUN_IN_BACKGROUND, FALSE);
    }
  } else if (self->requested_autostart && !autostart) {
    error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                "Groundhog could not be set to start when you log in");
  }
  reconcile_done(self, error);
}

static gboolean
is_unavailable(const GError *error)
{
  g_autofree gchar *name = g_dbus_error_get_remote_error(error);
  return g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN) ||
         g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER) ||
         g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD) ||
         g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_INTERFACE) ||
         g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_OBJECT) ||
         g_strcmp0(name, "org.freedesktop.DBus.Error.ServiceUnknown") == 0;
}

static void
subscribe_response(GhBackground *self, const gchar *path)
{
  unsubscribe_response(self);
  /* The match rule goes out on this connection before the request does. */
  self->response_subscription = g_dbus_connection_signal_subscribe(self->connection,
    PORTAL_BUS, PORTAL_REQUEST, "Response", path, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
    on_portal_response, self, NULL);
}

static void
on_request_called(GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GhBackground) self = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result,
                                                            &error);
  if (self->disposed)
    return;
  if (!reply) {
    unsubscribe_response(self);
    g_clear_pointer(&self->request_path, g_free);
    if (is_unavailable(error)) {
      g_clear_error(&error);
      error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                  "The background portal is not available");
    }
    reconcile_done(self, g_steal_pointer(&error));
    return;
  }
  const gchar *handle = NULL;
  g_variant_get(reply, "(&o)", &handle);
  /* Portals older than handle_token answer on a path of their own. */
  if (self->request_path && !g_str_equal(handle, self->request_path)) {
    g_free(self->request_path);
    self->request_path = g_strdup(handle);
    subscribe_response(self, handle);
  }
}

static void
portal_request(GhBackground *self)
{
  const gchar *unique = self->connection ? g_dbus_connection_get_unique_name(self->connection)
                                         : NULL;
  if (!unique) {
    reconcile_done(self, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                             "The background portal is not available"));
    return;
  }
  g_autofree gchar *sender = g_strdup(unique[0] == ':' ? unique + 1 : unique);
  g_strdelimit(sender, ".", '_');
  g_autofree gchar *token = g_strdup_printf("groundhog%u", ++self->token_serial);
  g_free(self->request_path);
  self->request_path = g_strdup_printf("%s/request/%s/%s", PORTAL_PATH, sender, token);
  /* Subscribed before the call, so a fast Response is never missed. */
  subscribe_response(self, self->request_path);

  static const gchar *const commandline[] = { "groundhog", "--gapplication-service", NULL };
  GVariantBuilder options;
  g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(token));
  g_variant_builder_add(&options, "{sv}", "reason", g_variant_new_string(_(PORTAL_REASON)));
  g_variant_builder_add(&options, "{sv}", "autostart",
                        g_variant_new_boolean(self->requested_autostart));
  g_variant_builder_add(&options, "{sv}", "commandline", g_variant_new_strv(commandline, -1));
  /* Started by its command line: D-Bus activation would open a window. */
  g_variant_builder_add(&options, "{sv}", "dbus-activatable", g_variant_new_boolean(FALSE));
  g_dbus_connection_call(self->connection, PORTAL_BUS, PORTAL_PATH, PORTAL_BACKGROUND,
                         "RequestBackground", g_variant_new("(sa{sv})", "", &options),
                         G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, -1, self->cancellable,
                         on_request_called, g_object_ref(self));
}

/* ---- reconciliation -------------------------------------------------------------------- */

static void
complete_waiters(GhBackground *self, const GError *error)
{
  g_autoptr(GPtrArray) waiters = g_steal_pointer(&self->waiters);
  self->waiters = g_ptr_array_new_with_free_func(g_object_unref);
  for (guint i = 0; i < waiters->len; i++) {
    GTask *task = g_ptr_array_index(waiters, i);
    if (error)
      g_task_return_error(task, g_error_copy(error));
    else
      g_task_return_boolean(task, TRUE);
  }
}

static void
run_reconcile(GhBackground *self)
{
  self->in_flight = TRUE;
  /* Keeps the process until the entry or the portal agrees, even if the
   * key was just turned off with no window open. */
  g_application_hold(self->app);
  self->requested_autostart = key_state(self) == KEY_ON;
  if (self->method == GH_BACKGROUND_METHOD_PORTAL) {
    portal_request(self);
    return;
  }
  GError *error = NULL;
  file_apply(self, self->requested_autostart, &error);
  reconcile_done(self, error);
}

/* error: owned. */
static void
reconcile_done(GhBackground *self, GError *error)
{
  self->in_flight = FALSE;
  if (self->again && !self->disposed) {
    self->again = FALSE;
    g_clear_error(&error);
    run_reconcile(self); /* holds again before the release below */
    g_application_release(self->app);
    return;
  }
  if (error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_message("Groundhog could not update its autostart entry: %s", error->message);
  complete_waiters(self, error);
  g_clear_error(&error);
  g_application_release(self->app);
}

static void
request_reconcile(GhBackground *self)
{
  if (self->disposed)
    return;
  self->target = key_state(self);
  if (self->in_flight) {
    self->again = TRUE;
    return;
  }
  run_reconcile(self);
}

/* ---- the one-time explanation --------------------------------------------------------- */

/* The window's close, out of the dialog's emission: while an AdwDialog is
 * still the window's visible dialog, a close request goes to the dialog. */
static gboolean
close_window_later(gpointer data)
{
  GWeakRef *ref = data;
  g_autoptr(GtkWindow) window = g_weak_ref_get(ref);
  if (window)
    gtk_window_close(window); /* explained now: this close goes through */
  return G_SOURCE_REMOVE;
}

static void
weak_ref_free(gpointer data)
{
  g_weak_ref_clear(data);
  g_free(data);
}

/* Once per dialog. A button closes the dialog first and then responds;
 * Escape (or closing it otherwise) responds with the close response. */
static void
on_dialog_response(AdwAlertDialog *dialog, const gchar *response, GhBackground *self)
{
  g_signal_handlers_disconnect_by_data(dialog, self);
  GtkWindow *window = self->closing;
  g_clear_weak_pointer(&self->closing);
  g_clear_weak_pointer(&self->dialog);
  if (self->disposed)
    return;
  mark_explained(self);
  if (g_str_equal(response, RESPONSE_QUIT)) {
    g_application_quit(self->app);
  } else if (window) {
    GWeakRef *ref = g_new0(GWeakRef, 1);
    g_weak_ref_init(ref, window);
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, close_window_later, ref, weak_ref_free);
  }
}

static gboolean
is_last_window(GhBackground *self, GtkWindow *window)
{
  for (GList *l = gtk_application_get_windows(GTK_APPLICATION(self->app)); l; l = l->next) {
    GtkWindow *other = l->data;
    if (other != window && !gtk_window_get_transient_for(other) &&
        gtk_widget_get_visible(GTK_WIDGET(other)))
      return FALSE;
  }
  return TRUE;
}

static gboolean
on_close_request(GtkWindow *window, GhBackground *self)
{
  if (self->disposed || !self->enabled || self->explained || !is_last_window(self, window))
    return FALSE;
  if (self->dialog)
    return TRUE; /* already asking */
  if (!ADW_IS_APPLICATION_WINDOW(window) && !ADW_IS_WINDOW(window))
    return FALSE;
  AdwDialog *dialog = adw_alert_dialog_new(_("Groundhog Keeps Running"),
    _("Closing the window does not stop Groundhog. It keeps receiving messages, so it stays "
      "connected to your inbox relays, which can see your IP address and when this device "
      "is online.\n\n"
      "To stop it, choose Quit in the main menu or press Ctrl+Q. To turn this off, switch off "
      "“Receive Messages When Closed” in Preferences."));
  AdwAlertDialog *alert = ADW_ALERT_DIALOG(dialog);
  adw_alert_dialog_add_responses(alert, RESPONSE_QUIT, _("_Quit Groundhog"),
                                 RESPONSE_BACKGROUND, _("_Keep Running"), NULL);
  adw_alert_dialog_set_response_appearance(alert, RESPONSE_BACKGROUND,
                                           ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response(alert, RESPONSE_BACKGROUND);
  adw_alert_dialog_set_close_response(alert, RESPONSE_BACKGROUND);
  g_signal_connect(dialog, "response", G_CALLBACK(on_dialog_response), self);
  g_set_weak_pointer(&self->dialog, dialog);
  g_set_weak_pointer(&self->closing, window);
  adw_dialog_present(dialog, GTK_WIDGET(window));
  return TRUE;
}

static void
on_window_added(GtkApplication *app, GtkWindow *window, GhBackground *self)
{
  (void)app;
  g_signal_connect(window, "close-request", G_CALLBACK(on_close_request), self);
}

/* ---- GObject ------------------------------------------------------------------------- */

static void
gh_background_get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
  GhBackground *self = GH_BACKGROUND(object);
  switch (id) {
  case PROP_ENABLED: g_value_set_boolean(value, self->enabled); break;
  case PROP_HOLDING: g_value_set_boolean(value, self->holding); break;
  case PROP_STATUS: g_value_set_string(value, self->status); break;
  case PROP_EXPLAINED: g_value_set_boolean(value, self->explained); break;
  default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
  }
}

static void
gh_background_dispose(GObject *object)
{
  GhBackground *self = GH_BACKGROUND(object);
  if (!self->disposed) {
    self->disposed = TRUE;
    if (g_object_get_data(G_OBJECT(self->app), APP_DATA_KEY) == self)
      g_object_set_data(G_OBJECT(self->app), APP_DATA_KEY, NULL);
    if (GTK_IS_APPLICATION(self->app)) {
      g_signal_handlers_disconnect_by_data(self->app, self);
      for (GList *l = gtk_application_get_windows(GTK_APPLICATION(self->app)); l; l = l->next)
        g_signal_handlers_disconnect_by_data(l->data, self);
    }
    if (self->dialog)
      g_signal_handlers_disconnect_by_data(self->dialog, self);
    g_clear_weak_pointer(&self->dialog);
    g_clear_weak_pointer(&self->closing);
    g_cancellable_cancel(self->cancellable);
    unsubscribe_response(self);
    if (self->request_path && self->connection)
      g_dbus_connection_call(self->connection, PORTAL_BUS, self->request_path, PORTAL_REQUEST,
                             "Close", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL,
                             NULL);
    g_clear_pointer(&self->request_path, g_free);
    if (self->account_store)
      g_signal_handlers_disconnect_by_data(self->account_store, self);
    g_signal_handlers_disconnect_by_data(self->settings, self);
    g_autoptr(GError) cancelled = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                                      "Background service stopped");
    complete_waiters(self, cancelled);
    if (self->in_flight) {
      self->in_flight = FALSE;
      g_application_release(self->app);
    }
    sync_hold(self);
  }
  g_clear_object(&self->account_store);
  G_OBJECT_CLASS(gh_background_parent_class)->dispose(object);
}

static void
gh_background_finalize(GObject *object)
{
  GhBackground *self = GH_BACKGROUND(object);
  g_clear_object(&self->settings);
  g_clear_object(&self->connection);
  g_clear_object(&self->cancellable);
  g_clear_pointer(&self->waiters, g_ptr_array_unref);
  g_free(self->autostart_dir);
  g_free(self->state_dir);
  g_free(self->status);
  g_free(self->sent_status);
  G_OBJECT_CLASS(gh_background_parent_class)->finalize(object);
}

static void
gh_background_class_init(GhBackgroundClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->get_property = gh_background_get_property;
  object_class->dispose = gh_background_dispose;
  object_class->finalize = gh_background_finalize;
  props[PROP_ENABLED] = g_param_spec_boolean("enabled", NULL, NULL, FALSE,
    G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
  props[PROP_HOLDING] = g_param_spec_boolean("holding", NULL, NULL, FALSE,
    G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
  props[PROP_STATUS] = g_param_spec_string("status", NULL, NULL, NULL,
    G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
  props[PROP_EXPLAINED] = g_param_spec_boolean("explained", NULL, NULL, FALSE,
    G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
  g_object_class_install_properties(object_class, N_PROPS, props);
}

static void
gh_background_init(GhBackground *self)
{
  self->cancellable = g_cancellable_new();
  self->waiters = g_ptr_array_new_with_free_func(g_object_unref);
}

/* ---- public --------------------------------------------------------------------------- */

GhBackground *
gh_background_new(GApplication *app, const GhBackgroundConfig *config)
{
  g_return_val_if_fail(G_IS_APPLICATION(app), NULL);
  g_return_val_if_fail(config != NULL && G_IS_SETTINGS(config->settings), NULL);
  g_return_val_if_fail(!config->connection || G_IS_DBUS_CONNECTION(config->connection), NULL);
#if GH_BACKGROUND_WITH_STORE
  g_return_val_if_fail(!config->account_store || GH_IS_ACCOUNT_STORE(config->account_store),
                       NULL);
#else
  g_return_val_if_fail(!config->account_store, NULL);
#endif
  GhBackground *self = g_object_new(GH_TYPE_BACKGROUND, NULL);
  self->app = app;
  self->settings = g_object_ref(config->settings);
  GDBusConnection *connection = config->connection;
  if (!connection && g_application_get_is_registered(app))
    connection = g_application_get_dbus_connection(app);
  self->connection = connection ? g_object_ref(connection) : NULL;
  self->method = config->method;
  if (self->method == GH_BACKGROUND_METHOD_AUTO)
    self->method = g_file_test("/.flatpak-info", G_FILE_TEST_EXISTS)
                     ? GH_BACKGROUND_METHOD_PORTAL : GH_BACKGROUND_METHOD_FILE;
  self->autostart_dir = g_build_filename(config->config_dir ? config->config_dir
                                                            : g_get_user_config_dir(),
                                         "autostart", NULL);
  self->state_dir = config->state_dir ? g_strdup(config->state_dir)
                                      : g_build_filename(g_get_user_state_dir(), "groundhog",
                                                         NULL);
  g_autofree gchar *marker = explained_path(self);
  self->explained = g_file_test(marker, G_FILE_TEST_EXISTS);
  self->enabled = g_settings_get_boolean(self->settings, RUN_IN_BACKGROUND);
  self->target = KEY_DEFAULT;

  g_object_set_data(G_OBJECT(app), APP_DATA_KEY, self);
  g_signal_connect(self->settings, "changed::" RUN_IN_BACKGROUND,
                   G_CALLBACK(on_settings_changed), self);
  if (config->account_store) {
    self->account_store = g_object_ref(config->account_store);
    g_signal_connect_swapped(self->account_store, "changed", G_CALLBACK(update_status), self);
  }
  if (GTK_IS_APPLICATION(app)) {
    g_signal_connect(app, "window-added", G_CALLBACK(on_window_added), self);
    for (GList *l = gtk_application_get_windows(GTK_APPLICATION(app)); l; l = l->next)
      on_window_added(GTK_APPLICATION(app), l->data, self);
  }
  if (self->method == GH_BACKGROUND_METHOD_PORTAL && self->connection)
    g_dbus_connection_call(self->connection, PORTAL_BUS, PORTAL_PATH,
                           "org.freedesktop.DBus.Properties", "Get",
                           g_variant_new("(ss)", PORTAL_BACKGROUND, "version"),
                           G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, -1, self->cancellable,
                           on_portal_version, g_object_ref(self));

  sync_hold(self);
  update_status(self);
  /* Only a confirmed choice touches autostart. */
  if (key_state(self) != KEY_DEFAULT)
    request_reconcile(self);
  return self;
}

GhBackground *
gh_background_get_for_application(GApplication *app)
{
  g_return_val_if_fail(G_IS_APPLICATION(app), NULL);
  return g_object_get_data(G_OBJECT(app), APP_DATA_KEY);
}

gboolean
gh_background_get_enabled(GhBackground *self)
{
  g_return_val_if_fail(GH_IS_BACKGROUND(self), FALSE);
  return self->enabled;
}

gboolean
gh_background_get_holding(GhBackground *self)
{
  g_return_val_if_fail(GH_IS_BACKGROUND(self), FALSE);
  return self->holding;
}

GhBackgroundMethod
gh_background_get_method(GhBackground *self)
{
  g_return_val_if_fail(GH_IS_BACKGROUND(self), GH_BACKGROUND_METHOD_FILE);
  return self->method;
}

const gchar *
gh_background_get_status(GhBackground *self)
{
  g_return_val_if_fail(GH_IS_BACKGROUND(self), NULL);
  return self->status;
}

gboolean
gh_background_get_explained(GhBackground *self)
{
  g_return_val_if_fail(GH_IS_BACKGROUND(self), FALSE);
  return self->explained;
}

void
gh_background_set_enabled_async(GhBackground *self, gboolean enabled,
                                GCancellable *cancellable, GAsyncReadyCallback callback,
                                gpointer user_data)
{
  g_return_if_fail(GH_IS_BACKGROUND(self));
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, gh_background_set_enabled_async);
  if (self->disposed) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Background service stopped");
    g_object_unref(task);
    return;
  }
  g_ptr_array_add(self->waiters, task);
  /* The caller (onboarding) explained background mode itself. */
  mark_explained(self);
  g_settings_set_boolean(self->settings, RUN_IN_BACKGROUND, enabled);
  /* The change handler may already have started this reconciliation (it
   * then completes the task); otherwise run it now, also to retry a failed
   * one for an unchanged choice. */
  if (!self->in_flight && self->waiters->len > 0)
    request_reconcile(self);
}

gboolean
gh_background_set_enabled_finish(GhBackground *self, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(GH_IS_BACKGROUND(self), FALSE);
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}
