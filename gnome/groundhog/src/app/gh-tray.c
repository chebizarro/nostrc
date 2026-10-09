#include "gh-tray.h"
#include "gh-background.h"
#include "gh-update-handoff.h"
#include "gn-status-notifier.h"
#include <adwaita.h>
#include <glib/gi18n.h>

enum {
  ITEM_WINDOW = 10, ITEM_ACCOUNTS = 11, ITEM_LAUNCH = 12,
  ITEM_PREFERENCES = 13, ITEM_UPDATE = 14, ITEM_ISSUE = 15,
  ITEM_SEPARATOR = 16, ITEM_QUIT = 17,
};

struct _GhTray {
  GObject parent_instance;
  GtkApplication *app; /* app outlives tray */
  GSettings *settings;
  GhBackground *background; /* app service outlives tray */
  GnStatusNotifier *notifier;
  gboolean disposed;
};
G_DEFINE_FINAL_TYPE(GhTray, gh_tray, G_TYPE_OBJECT)

static GtkWindow *visible_primary(GhTray *self)
{
  for (GList *l = gtk_application_get_windows(self->app); l; l = l->next) {
    GtkWindow *window = l->data;
    if (!gtk_window_get_transient_for(window) && gtk_widget_get_visible(GTK_WIDGET(window)))
      return window;
  }
  return NULL;
}

static GtkWindow *present_primary(GhTray *self)
{
  GtkWindow *window = visible_primary(self);
  if (!window) {
    g_application_activate(G_APPLICATION(self->app));
    window = gtk_application_get_active_window(self->app);
  }
  if (window) gtk_window_present(window);
  return window;
}

static void rebuild(GhTray *self)
{
  if (self->disposed) return;
  g_autofree gchar *account = g_settings_get_string(self->settings, "current-npub");
  gboolean named = g_getenv("GROUNDHOG_INSTANCE") && *g_getenv("GROUNDHOG_INSTANCE");
  gboolean system_disabled = self->background &&
    gh_background_launch_disabled_in_system_settings(self->background);
  GnStatusNotifierItem items[] = {
    { ITEM_WINDOW, visible_primary(self) ? _("Close Window") : _("Open Groundhog"),
      TRUE, TRUE, FALSE, -1 },
    { ITEM_ACCOUNTS, _("User Accounts"), TRUE, TRUE, FALSE, -1 },
    { ITEM_LAUNCH, named ? _("Launch on Login (unavailable for named instances)") :
      system_disabled ? _("Launch on Login (disabled in system settings)") :
      _("Launch on Login"), !named && !system_disabled, TRUE, FALSE,
      self->background && gh_background_get_launch_on_login(self->background) ? 1 : 0 },
    { ITEM_PREFERENCES, _("Preferences"), TRUE, TRUE, FALSE, -1 },
    { ITEM_UPDATE, _("Update…"), TRUE, TRUE, FALSE, -1 },
    { ITEM_ISSUE, _("File a NIP-34 Issue…"), account && *account, TRUE, FALSE, -1 },
    { ITEM_SEPARATOR, NULL, FALSE, TRUE, TRUE, -1 },
    { ITEM_QUIT, _("Quit Groundhog"), TRUE, TRUE, FALSE, -1 },
  };
  gn_status_notifier_set_menu_items(self->notifier, items, G_N_ELEMENTS(items));
}

static void on_window_visible(GObject *window, GParamSpec *pspec, GhTray *self)
{
  (void)window; (void)pspec;
  rebuild(self);
}

static void on_window_added(GtkApplication *app, GtkWindow *window, GhTray *self)
{
  (void)app;
  g_signal_connect(window, "notify::visible", G_CALLBACK(on_window_visible), self);
  rebuild(self);
}

static void on_window_removed(GtkApplication *app, GtkWindow *window, GhTray *self)
{
  (void)app;
  g_signal_handlers_disconnect_by_data(window, self);
  rebuild(self);
}

static void on_state_changed(GObject *source, gpointer detail, GhTray *self)
{
  (void)source; (void)detail;
  rebuild(self);
}

static void show_launch_error(GhTray *self, const gchar *message)
{
  GtkWindow *window = present_primary(self);
  if (!window) return;
  AdwDialog *dialog = adw_alert_dialog_new(_("Launch on Login Was Not Changed"), message);
  adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "close", _("Close"));
  adw_dialog_present(dialog, GTK_WIDGET(window));
}

static void on_launch_done(GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GhTray) self = data;
  g_autoptr(GError) error = NULL;
  if (!gh_background_set_launch_on_login_finish(GH_BACKGROUND(source), result, &error) &&
      !self->disposed && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    show_launch_error(self, error->message);
  if (!self->disposed) rebuild(self);
}

static void on_launch_disclosure(AdwAlertDialog *dialog, const gchar *response, GhTray *self)
{
  (void)dialog;
  if (g_str_equal(response, "enable") && !self->disposed && self->background)
    gh_background_set_launch_on_login_async(self->background, TRUE, NULL,
                                            on_launch_done, g_object_ref(self));
}

static void on_item_activated(GnStatusNotifier *notifier, gint id, GhTray *self)
{
  (void)notifier;
  if (id == ITEM_WINDOW) {
    GtkWindow *window = visible_primary(self);
    if (window) gtk_window_close(window);
    else present_primary(self);
  } else if (id == ITEM_ACCOUNTS) {
    GtkWindow *window = present_primary(self);
    if (window) gtk_widget_activate_action(GTK_WIDGET(window), "account.open", NULL);
  } else if (id == ITEM_LAUNCH && self->background) {
    gboolean enabled = !gh_background_get_launch_on_login(self->background);
    if (enabled && !gh_background_get_explained(self->background)) {
      GtkWindow *window = present_primary(self);
      if (window) {
        AdwDialog *dialog = adw_alert_dialog_new(_("Receive Messages in the Background"),
          _("Groundhog will start when you log in and keep receiving messages after its window "
            "is closed. Your inbox relays can see your IP address and when this device is online. "
            "Quit Groundhog to stop it."));
        AdwAlertDialog *alert = ADW_ALERT_DIALOG(dialog);
        adw_alert_dialog_add_responses(alert, "cancel", _("Cancel"),
                                       "enable", _("Enable Launch on Login"), NULL);
        adw_alert_dialog_set_default_response(alert, "cancel");
        adw_alert_dialog_set_close_response(alert, "cancel");
        g_signal_connect_object(dialog, "response", G_CALLBACK(on_launch_disclosure), self, 0);
        adw_dialog_present(dialog, GTK_WIDGET(window));
      }
    } else {
      gh_background_set_launch_on_login_async(self->background, enabled, NULL,
                                              on_launch_done, g_object_ref(self));
    }
  } else if (id == ITEM_PREFERENCES) {
    if (present_primary(self))
      g_action_group_activate_action(G_ACTION_GROUP(self->app), "preferences", NULL);
  } else if (id == ITEM_UPDATE) {
    GtkWindow *window = present_primary(self);
    if (window) gh_update_handoff_present(window);
  } else if (id == ITEM_ISSUE) {
    if (present_primary(self))
      g_action_group_activate_action(G_ACTION_GROUP(self->app), "report-issue", NULL);
  } else if (id == ITEM_QUIT) {
    g_application_quit(G_APPLICATION(self->app));
  }
}

static void on_activate(GnStatusNotifier *notifier, GhTray *self)
{
  (void)notifier;
  present_primary(self);
}

static void gh_tray_dispose(GObject *object)
{
  GhTray *self = GH_TRAY(object);
  if (self->disposed) {
    G_OBJECT_CLASS(gh_tray_parent_class)->dispose(object);
    return;
  }
  self->disposed = TRUE;
  if (self->app) {
    g_signal_handlers_disconnect_by_data(self->app, self);
    for (GList *l = gtk_application_get_windows(self->app); l; l = l->next)
      g_signal_handlers_disconnect_by_data(l->data, self);
  }
  if (self->settings) g_signal_handlers_disconnect_by_data(self->settings, self);
  if (self->background) g_signal_handlers_disconnect_by_data(self->background, self);
  g_clear_object(&self->notifier);
  g_clear_object(&self->settings);
  G_OBJECT_CLASS(gh_tray_parent_class)->dispose(object);
}

static void gh_tray_class_init(GhTrayClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = gh_tray_dispose;
}

static void gh_tray_init(GhTray *self) { (void)self; }

GhTray *gh_tray_new(GtkApplication *app, GSettings *settings)
{
  g_return_val_if_fail(GTK_IS_APPLICATION(app) && G_IS_SETTINGS(settings), NULL);
  GhTray *self = g_object_new(GH_TYPE_TRAY, NULL);
  self->app = app;
  self->settings = g_object_ref(settings);
  self->background = gh_background_get_for_application(G_APPLICATION(app));
  const gchar *app_id = g_application_get_application_id(G_APPLICATION(app));
  self->notifier = gn_status_notifier_new(G_APPLICATION(app), app_id, "Groundhog");
  g_signal_connect(self->notifier, "activate", G_CALLBACK(on_activate), self);
  g_signal_connect(self->notifier, "item-activated", G_CALLBACK(on_item_activated), self);
  g_signal_connect(app, "window-added", G_CALLBACK(on_window_added), self);
  g_signal_connect(app, "window-removed", G_CALLBACK(on_window_removed), self);
  for (GList *l = gtk_application_get_windows(app); l; l = l->next)
    on_window_added(app, l->data, self);
  g_signal_connect(self->settings, "changed::current-npub", G_CALLBACK(on_state_changed), self);
  g_signal_connect(self->settings, "changed::launch-on-login", G_CALLBACK(on_state_changed), self);
  if (self->background)
    g_signal_connect(self->background, "notify::launch-on-login",
                     G_CALLBACK(on_state_changed), self);
  rebuild(self);
  return self;
}
