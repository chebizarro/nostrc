#include "background.h"
#include <glib/gstdio.h>
#include <errno.h>

#ifndef GROTTO_EXECUTABLE
#define GROTTO_EXECUTABLE "grotto"
#endif
#ifndef GROTTO_AUTOSTART_TEMPLATE
#define GROTTO_AUTOSTART_TEMPLATE "/usr/local/share/grotto/org.nostr.Grotto-autostart.desktop"
#endif
#define PORTAL "org.freedesktop.portal.Desktop"
#define PATH "/org/freedesktop/portal/desktop"
#define BACKGROUND "org.freedesktop.portal.Background"
#define REQUEST "org.freedesktop.portal.Request"

struct _GrottoBackground {
  GObject parent;
  GApplication *app; /* weak: application stops us before disposing */
  GDBusConnection *bus;
  GSettings *settings;
  GCancellable *cancel;
  gchar *owner, *request_path, *last_status;
  guint watch, response, generation, version;
  gulong changed;
  gboolean native, stopped, continuous, temporary, waiting, paused, portal_autostart;
  GrottoBackgroundState state;
};
G_DEFINE_TYPE(GrottoBackground, grotto_background, G_TYPE_OBJECT)
static guint state_changed;
static void refresh(GrottoBackground *s);

typedef struct { GrottoBackground *self; guint generation; } Call;
static Call *call_new(GrottoBackground *s) {
  Call *c = g_new0(Call, 1); c->self = g_object_ref(s); c->generation = s->generation; return c;
}
static void call_free(Call *c) { g_object_unref(c->self); g_free(c); }
static gboolean current(Call *c) { return !c->self->stopped && c->generation == c->self->generation; }
static void held(GrottoBackground *s, gboolean *slot, gboolean value) {
  if (*slot == value) return;
  *slot = value;
  if (value) g_application_hold(s->app); else g_application_release(s->app);
}
static void state(GrottoBackground *s, GrottoBackgroundState value) {
  s->state = value;
  gboolean enabled = g_settings_get_boolean(s->settings, "run-in-background") &&
                     g_settings_get_boolean(s->settings, "background-disclosure-accepted");
  held(s, &s->continuous, enabled && (value == GROTTO_BACKGROUND_REGISTERED ||
       (value == GROTTO_BACKGROUND_UNAVAILABLE && s->native)));
  g_signal_emit(s, state_changed, 0);
}
static void cancel_request(GrottoBackground *s) {
  s->generation++;
  if (s->cancel) g_cancellable_cancel(s->cancel);
  g_clear_object(&s->cancel);
  if (s->response) { g_dbus_connection_signal_unsubscribe(s->bus, s->response); s->response = 0; }
  if (s->request_path && s->owner)
    g_dbus_connection_call(s->bus, s->owner, s->request_path, REQUEST, "Close", NULL,
                          NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000, NULL, NULL, NULL);
  g_clear_pointer(&s->request_path, g_free);
  g_clear_pointer(&s->last_status, g_free);
  held(s, &s->temporary, FALSE);
}
void grotto_background_migrate_settings(GSettings *settings) {
  if (g_settings_get_uint(settings, "settings-migration-version") >= 1) return;
  g_autoptr(GVariant) old_launch = g_settings_get_user_value(settings, "autostart");
  g_autoptr(GVariant) launch = g_settings_get_user_value(settings, "launch-on-login");
  g_autoptr(GVariant) background = g_settings_get_user_value(settings, "run-in-background");
  g_autoptr(GVariant) minimized = g_settings_get_user_value(settings, "start-minimized");
  if (!launch && old_launch)
    g_settings_set_boolean(settings, "launch-on-login", g_variant_get_boolean(old_launch));
  /* A minimized launch implied residence, never a hidden explicit activation.
   * False/default start-minimized says nothing about background permission. */
  if (!background && minimized && g_variant_get_boolean(minimized))
    g_settings_set_boolean(settings, "run-in-background", TRUE);
  if (!g_settings_get_boolean(settings, "run-in-background"))
    g_settings_set_boolean(settings, "launch-on-login", FALSE);
  g_settings_set_uint(settings, "settings-migration-version", 1);
}
gboolean grotto_background_sync_autostart(GSettings *settings, gboolean native, GError **error) {
  g_autofree gchar *dir = g_build_filename(g_get_user_config_dir(), "autostart", NULL);
  /* Distinct from the portal-owned <app-id>.desktop entry. */
  g_autofree gchar *file = g_build_filename(dir, "org.nostr.Grotto-login.desktop", NULL);
  gboolean enabled = native && !g_getenv("APPIMAGE") &&
                     g_settings_get_boolean(settings, "background-disclosure-accepted") &&
                     !g_settings_get_boolean(settings, "portal-autostart-created") &&
                     g_settings_get_boolean(settings, "run-in-background") &&
                     g_settings_get_boolean(settings, "launch-on-login");
  if (!enabled) {
    if (g_unlink(file) == 0 || errno == ENOENT) return TRUE;
    g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno), "Cannot remove Grotto login entry: %s", g_strerror(errno));
    return FALSE;
  }
  if (g_mkdir_with_parents(dir, 0700) != 0) {
    g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno), "Cannot create autostart directory: %s", g_strerror(errno));
    return FALSE;
  }
  g_autoptr(GKeyFile) entry = g_key_file_new();
  if (!g_key_file_load_from_file(entry, GROTTO_AUTOSTART_TEMPLATE, G_KEY_FILE_NONE, error)) return FALSE;
  return g_key_file_save_to_file(entry, file, error);
}
static void status_done(GObject *source, GAsyncResult *result, gpointer data) {
  Call *c = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
  if (current(c) && !r && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_debug("Background status unavailable: %s", error->message);
  call_free(c);
}
void grotto_background_set_status(GrottoBackground *s, gboolean waiting, gboolean paused) {
  s->waiting = waiting; s->paused = paused;
  if (s->stopped || s->state != GROTTO_BACKGROUND_REGISTERED || s->version < 2) return;
  const char *message = paused ? "Signing paused" : waiting ? "Requests waiting" : "Ready to review requests";
  if (!g_strcmp0(message, s->last_status)) return;
  g_free(s->last_status); s->last_status = g_strdup(message);
  GVariantBuilder options; g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&options, "{sv}", "message", g_variant_new_string(message));
  g_dbus_connection_call(s->bus, s->owner, PATH, BACKGROUND, "SetStatus",
      g_variant_new("(a{sv})", &options), G_VARIANT_TYPE_UNIT, G_DBUS_CALL_FLAGS_NO_AUTO_START,
      2000, s->cancel, status_done, call_new(s));
}
static void response(GDBusConnection *bus, const gchar *sender, const gchar *path,
                     const gchar *interface, const gchar *signal, GVariant *args, gpointer data) {
  (void)bus; (void)interface; (void)signal;
  GrottoBackground *s = data;
  if (s->stopped || g_strcmp0(sender, s->owner) || g_strcmp0(path, s->request_path)) return;
  guint code; gboolean allowed = FALSE, autostart = FALSE;
  g_autoptr(GVariant) results = NULL;
  g_variant_get(args, "(u@a{sv})", &code, &results);
  gboolean has_background = g_variant_lookup(results, "background", "b", &allowed);
  gboolean has_autostart = g_variant_lookup(results, "autostart", "b", &autostart);
  if (s->response) { g_dbus_connection_signal_unsubscribe(s->bus, s->response); s->response = 0; }
  g_clear_pointer(&s->request_path, g_free);
  if (code == 0 && has_autostart) {
    s->portal_autostart = autostart;
    g_settings_set_boolean(s->settings, "portal-autostart-created", autostart);
    g_autoptr(GError) error = NULL;
    if (!grotto_background_sync_autostart(s->settings, s->native, &error))
      g_warning("Grotto launch on login unavailable: %s", error->message);
  }
  gboolean enabled = g_settings_get_boolean(s->settings, "run-in-background") &&
                     g_settings_get_boolean(s->settings, "background-disclosure-accepted");
  state(s, !enabled ? GROTTO_BACKGROUND_DISABLED :
        code == 0 && has_background ? (allowed ? GROTTO_BACKGROUND_REGISTERED : GROTTO_BACKGROUND_DENIED) :
        GROTTO_BACKGROUND_UNAVAILABLE);
  held(s, &s->temporary, FALSE);
  if (!s->native && code == 0 && has_autostart && !autostart && g_settings_get_boolean(s->settings, "launch-on-login")) {
    /* Do not recurse/re-prompt when autostart alone was denied. */
    g_signal_handler_block(s->settings, s->changed);
    g_settings_set_boolean(s->settings, "launch-on-login", FALSE);
    g_signal_handler_unblock(s->settings, s->changed);
  }
  grotto_background_set_status(s, s->waiting, s->paused);
}
static void request_done(GObject *source, GAsyncResult *result, gpointer data) {
  Call *c = data; GrottoBackground *s = c->self;
  g_autoptr(GVariant) r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, NULL);
  if (current(c) && s->state == GROTTO_BACKGROUND_REQUESTING) {
    if (!r) { cancel_request(s); state(s, GROTTO_BACKGROUND_UNAVAILABLE); }
    else {
      const char *path; g_variant_get(r, "(&o)", &path);
      if (g_strcmp0(path, s->request_path)) {
        if (s->response) g_dbus_connection_signal_unsubscribe(s->bus, s->response);
        g_free(s->request_path); s->request_path = g_strdup(path);
        s->response = g_dbus_connection_signal_subscribe(s->bus, s->owner, REQUEST, "Response",
            path, NULL, G_DBUS_SIGNAL_FLAGS_NONE, response, s, NULL);
      }
    }
  }
  call_free(c);
}
static void version_done(GObject *source, GAsyncResult *result, gpointer data) {
  Call *c = data; GrottoBackground *s = c->self;
  g_autoptr(GVariant) r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, NULL);
  if (!current(c)) { call_free(c); return; }
  if (!r) { cancel_request(s); state(s, GROTTO_BACKGROUND_UNAVAILABLE); call_free(c); return; }
  g_autoptr(GVariant) version = NULL; g_variant_get(r, "(v)", &version);
  if (!g_variant_is_of_type(version, G_VARIANT_TYPE_UINT32) || !(s->version = g_variant_get_uint32(version))) {
    cancel_request(s); state(s, GROTTO_BACKGROUND_UNAVAILABLE); call_free(c); return;
  }
  g_autofree gchar *token = g_uuid_string_random();
  for (gchar *p = token; *p; p++) if (*p == '-') *p = '_';
  g_autofree gchar *sender = g_strdup(g_dbus_connection_get_unique_name(s->bus) + 1);
  for (gchar *p = sender; *p; p++) if (*p == '.') *p = '_';
  s->request_path = g_strdup_printf(PATH "/request/%s/%s", sender, token);
  s->response = g_dbus_connection_signal_subscribe(s->bus, s->owner, REQUEST, "Response",
      s->request_path, NULL, G_DBUS_SIGNAL_FLAGS_NONE, response, s, NULL);
  GVariantBuilder options; g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(token));
  g_variant_builder_add(&options, "{sv}", "reason", g_variant_new_string("Review signing requests while Grotto is closed"));
  gboolean launch = !s->native && g_settings_get_boolean(s->settings, "background-disclosure-accepted") &&
                    g_settings_get_boolean(s->settings, "run-in-background") &&
                    g_settings_get_boolean(s->settings, "launch-on-login");
  g_variant_builder_add(&options, "{sv}", "autostart", g_variant_new_boolean(launch));
  const char *command[] = { GROTTO_EXECUTABLE, "--gapplication-service", NULL };
  g_variant_builder_add(&options, "{sv}", "commandline", g_variant_new_strv(command, -1));
  g_variant_builder_add(&options, "{sv}", "dbus-activatable", g_variant_new_boolean(FALSE));
  /* Record intent before sending: even an interrupted reply may have created
   * an entry. Keep this marker until a confirmed removal, including restarts. */
  if (launch) {
    s->portal_autostart = TRUE;
    g_settings_set_boolean(s->settings, "portal-autostart-created", TRUE);
    g_settings_sync();
  }
  g_dbus_connection_call(s->bus, s->owner, PATH, BACKGROUND, "RequestBackground",
      g_variant_new("(sa{sv})", "", &options), G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NO_AUTO_START,
      5000, s->cancel, request_done, call_new(s));
  call_free(c);
}
static void refresh(GrottoBackground *s) {
  if (s->stopped) return;
  cancel_request(s);
  gboolean enabled = g_settings_get_boolean(s->settings, "run-in-background");
  if (!enabled && g_settings_get_boolean(s->settings, "launch-on-login")) {
    /* Updating the dependent setting belongs to this same reconciliation.
     * Re-entering refresh here would start two portal calls in one generation. */
    g_signal_handler_block(s->settings, s->changed);
    g_settings_set_boolean(s->settings, "launch-on-login", FALSE);
    g_signal_handler_unblock(s->settings, s->changed);
  }
  g_autoptr(GError) error = NULL;
  if (!grotto_background_sync_autostart(s->settings, s->native, &error))
    g_warning("Grotto launch on login unavailable: %s", error->message);
  gboolean disclosed = g_settings_get_boolean(s->settings, "background-disclosure-accepted");
  /* RequestBackground(autostart=false) also removes an earlier portal login
   * entry. A disabled UI must not leave the previously granted entry behind. */
  gboolean clear_portal_login = s->portal_autostart &&
      (s->native || !enabled || !g_settings_get_boolean(s->settings, "launch-on-login"));
  if ((!enabled || !disclosed) && !clear_portal_login) {
    state(s, GROTTO_BACKGROUND_DISABLED); return;
  }
  if (!s->owner) { state(s, enabled && disclosed ? GROTTO_BACKGROUND_UNAVAILABLE : GROTTO_BACKGROUND_DISABLED); return; }
  s->cancel = g_cancellable_new(); held(s, &s->temporary, TRUE);
  state(s, GROTTO_BACKGROUND_REQUESTING);
  g_dbus_connection_call(s->bus, s->owner, PATH, "org.freedesktop.DBus.Properties", "Get",
      g_variant_new("(ss)", BACKGROUND, "version"), G_VARIANT_TYPE("(v)"),
      G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000, s->cancel, version_done, call_new(s));
}
static void settings_changed(GSettings *settings, const char *key, gpointer data) {
  (void)settings;
  GrottoBackground *s = data;
  if (s->native && !s->portal_autostart && !g_strcmp0(key, "launch-on-login")) {
    if (!g_settings_get_boolean(settings, "run-in-background") &&
        g_settings_get_boolean(settings, "launch-on-login"))
      g_settings_set_boolean(settings, "launch-on-login", FALSE);
    g_autoptr(GError) error = NULL;
    if (!grotto_background_sync_autostart(settings, TRUE, &error))
      g_warning("Grotto launch on login unavailable: %s", error->message);
    return;
  }
  if (!g_strcmp0(key, "run-in-background") || !g_strcmp0(key, "launch-on-login") ||
      !g_strcmp0(key, "background-disclosure-accepted")) refresh(data);
}
static void appeared(GDBusConnection *bus, const char *name, const char *owner, gpointer data) {
  (void)bus; (void)name; GrottoBackground *s = data;
  cancel_request(s);
  g_free(s->owner); s->owner = g_strdup(owner); refresh(s);
}
static void vanished(GDBusConnection *bus, const char *name, gpointer data) {
  (void)bus; (void)name; GrottoBackground *s = data;
  cancel_request(s);
  g_clear_pointer(&s->owner, g_free); refresh(s);
}
GrottoBackground *grotto_background_new(GApplication *app, GDBusConnection *bus,
                                        GSettings *settings, gboolean native) {
  GrottoBackground *s = g_object_new(GROTTO_TYPE_BACKGROUND, NULL);
  s->app = app; s->bus = g_object_ref(bus); s->settings = g_object_ref(settings); s->native = native;
  grotto_background_migrate_settings(settings);
  s->portal_autostart = g_settings_get_boolean(settings, "portal-autostart-created");
  /* The name watch is asynchronous. A service launch must not fall through
   * use-count zero before its first owner/permission callback. */
  if (s->portal_autostart || (g_settings_get_boolean(settings, "run-in-background") &&
      g_settings_get_boolean(settings, "background-disclosure-accepted")))
    held(s, &s->temporary, TRUE);
  s->changed = g_signal_connect(settings, "changed", G_CALLBACK(settings_changed), s);
  s->watch = g_bus_watch_name_on_connection(bus, PORTAL, G_BUS_NAME_WATCHER_FLAGS_AUTO_START,
                                           appeared, vanished, s, NULL);
  return s;
}
void grotto_background_stop(GrottoBackground *s) {
  if (s->stopped) return;
  s->stopped = TRUE;
  if (s->watch) { g_bus_unwatch_name(s->watch); s->watch = 0; }
  if (s->changed) { g_signal_handler_disconnect(s->settings, s->changed); s->changed = 0; }
  cancel_request(s); held(s, &s->continuous, FALSE);
  s->state = GROTTO_BACKGROUND_DISABLED;
}
GrottoBackgroundState grotto_background_get_state(GrottoBackground *s) { return s->state; }
guint grotto_background_get_holds(GrottoBackground *s) { return s->continuous + s->temporary; }
const char *grotto_background_get_description(GrottoBackground *s) {
  if (s->portal_autostart && (s->native ||
      !g_settings_get_boolean(s->settings, "launch-on-login")))
    return "Waiting for the desktop portal to confirm removal of its previous login entry";
  switch (s->state) {
    case GROTTO_BACKGROUND_REQUESTING: return "Waiting for desktop background permission";
    case GROTTO_BACKGROUND_REGISTERED: return "Background permission granted";
    case GROTTO_BACKGROUND_UNAVAILABLE: return s->native ? "Desktop background portal unavailable; native background mode remains available" : "Desktop background portal unavailable";
    case GROTTO_BACKGROUND_DENIED: return "Desktop denied background permission; requests can still start Grotto";
    default: return g_settings_get_boolean(s->settings, "run-in-background") &&
                    !g_settings_get_boolean(s->settings, "background-disclosure-accepted") ?
                    "Background mode awaits setup disclosure" :
                    "Background mode is off; requests can still start Grotto";
  }
}
static void dispose(GObject *object) {
  GrottoBackground *s = GROTTO_BACKGROUND(object); grotto_background_stop(s);
  g_clear_object(&s->bus); g_clear_object(&s->settings); g_clear_pointer(&s->owner, g_free);
  g_clear_pointer(&s->last_status, g_free);
  G_OBJECT_CLASS(grotto_background_parent_class)->dispose(object);
}
static void grotto_background_class_init(GrottoBackgroundClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = dispose;
  state_changed = g_signal_new("state-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
                              0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}
static void grotto_background_init(GrottoBackground *s) { s->state = GROTTO_BACKGROUND_DISABLED; }
