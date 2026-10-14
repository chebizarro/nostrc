#include "session-lock.h"
#include <unistd.h>

#define LOGIN "org.freedesktop.login1"
#define MANAGER "/org/freedesktop/login1"
#define SESSION LOGIN ".Session"
#define USER LOGIN ".User"
#define PROPERTIES "org.freedesktop.DBus.Properties"
struct _GrottoSessionLock {
  GObject parent;
  SignerApprovalService *approval;
  GSettings *settings;
  GDBusConnection *bus;
  GCancellable *cancel;
  gchar *owner, *session, *user;
  guint watch, subscription, epoch, revision;
  gulong setting_handler;
  gboolean stopped, available, locked, lock_requested;
};
G_DEFINE_TYPE(GrottoSessionLock, grotto_session_lock, G_TYPE_OBJECT)
static guint changed;
typedef struct { GrottoSessionLock *self; guint epoch, revision; } Query;
static Query *query_new(GrottoSessionLock *s) {
  Query *q = g_new0(Query, 1); q->self = g_object_ref(s); q->epoch = s->epoch; q->revision = s->revision; return q;
}
static void query_free(Query *q) { g_object_unref(q->self); g_free(q); }
static gboolean current(Query *q) { return !q->self->stopped && q->epoch == q->self->epoch && q->revision == q->self->revision; }
gboolean grotto_session_lock_enabled(GrottoSessionLock *s) {
  return s->settings && g_settings_get_boolean(s->settings, "pause-on-screen-lock");
}
gboolean grotto_session_lock_available(GrottoSessionLock *s) { return s->available; }
gboolean grotto_session_lock_locked(GrottoSessionLock *s) { return s->locked; }
gboolean grotto_session_lock_can_resume(GrottoSessionLock *s) {
  return !s->locked && (s->available || !grotto_session_lock_enabled(s));
}
static void publish(GrottoSessionLock *s) {
  if (grotto_session_lock_enabled(s) && (!s->available || s->locked))
    signer_approval_service_set_paused(s->approval, TRUE);
  g_signal_emit(s, changed, 0);
}
static void policy_changed(GSettings *settings, const char *key, gpointer data) {
  (void)settings; (void)key; publish(data);
}
static void get_hint(GrottoSessionLock *s);
static void select_session(GrottoSessionLock *s);
static void hint_done(GObject *source, GAsyncResult *result, gpointer data) {
  Query *q = data; GrottoSessionLock *s = q->self;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, NULL);
  if (current(q)) {
    gboolean hint = FALSE; guint uid = G_MAXUINT; const char *user_path = NULL;
    g_autoptr(GVariant) values = NULL;
    if (reply) g_variant_get(reply, "(@a{sv})", &values);
    s->available = values && g_variant_lookup(values, "User", "(u&o)", &uid, &user_path) &&
        uid == (guint)getuid() && g_variant_lookup(values, "LockedHint", "b", &hint);
    if (s->available) {
      if (hint) s->lock_requested = FALSE;
      s->locked = hint || s->lock_requested;
    }
    publish(s);
  } else if (!s->stopped && q->epoch == s->epoch && !s->available && s->session) {
    /* A Lock arriving during the initial snapshot invalidates that snapshot,
     * but must not strand initial User/LockedHint validation. */
    get_hint(s);
  }
  query_free(q);
}
static void get_hint(GrottoSessionLock *s) {
  if (!s->session || !s->owner || s->stopped) return;
  g_dbus_connection_call(s->bus, s->owner, s->session, PROPERTIES, "GetAll",
      g_variant_new("(s)", SESSION), G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NO_AUTO_START,
      3000, s->cancel, hint_done, query_new(s));
}
static void session_selected(GrottoSessionLock *s, const char *path) {
  if (!strcmp(path, "/")) { s->available = FALSE; publish(s); return; }
  g_free(s->session); s->session = g_strdup(path); s->revision++;
  s->available = FALSE; s->lock_requested = FALSE; publish(s); get_hint(s);
}
static void display_done(GObject *source, GAsyncResult *result, gpointer data) {
  Query *q = data; GrottoSessionLock *s = q->self;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, NULL);
  if (current(q) && reply) {
    g_autoptr(GVariant) value = NULL; g_variant_get(reply, "(v)", &value);
    if (g_variant_is_of_type(value, G_VARIANT_TYPE("(so)"))) {
      const char *id, *path; g_variant_get(value, "(&s&o)", &id, &path); session_selected(s, path);
    }
  }
  query_free(q);
}
static void user_done(GObject *source, GAsyncResult *result, gpointer data) {
  Query *q = data; GrottoSessionLock *s = q->self;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, NULL);
  if (current(q) && reply) {
    const char *path; g_variant_get(reply, "(&o)", &path);
    g_free(s->user); s->user = g_strdup(path);
    g_dbus_connection_call(s->bus, s->owner, path, PROPERTIES, "Get",
        g_variant_new("(ss)", USER, "Display"), G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NO_AUTO_START,
        3000, s->cancel, display_done, query_new(s));
  }
  query_free(q);
}
static void pid_done(GObject *source, GAsyncResult *result, gpointer data) {
  Query *q = data; GrottoSessionLock *s = q->self;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, NULL);
  if (current(q)) {
    if (reply) { const char *path; g_variant_get(reply, "(&o)", &path); session_selected(s, path); }
    /* systemd user services need not belong to a logind session themselves.
     * User.Display selects this UID's primary graphical session, not an
     * arbitrary entry from ListSessions. GetAll validates the selected UID. */
    else g_dbus_connection_call(s->bus, s->owner, MANAGER, LOGIN ".Manager", "GetUser",
        g_variant_new("(u)", (guint)getuid()), G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NO_AUTO_START,
        3000, s->cancel, user_done, query_new(s));
  }
  query_free(q);
}
static void select_session(GrottoSessionLock *s) {
  s->revision++; s->available = FALSE; g_clear_pointer(&s->session, g_free); publish(s);
  g_dbus_connection_call(s->bus, s->owner, MANAGER, LOGIN ".Manager", "GetSessionByPID",
      g_variant_new("(u)", (guint)getpid()), G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NO_AUTO_START,
      3000, s->cancel, pid_done, query_new(s));
}
static void signal_received(GDBusConnection *bus, const char *sender, const char *path,
    const char *interface, const char *signal, GVariant *args, gpointer data) {
  (void)bus; GrottoSessionLock *s = data;
  if (s->stopped || g_strcmp0(sender, s->owner)) return;
  if (!strcmp(interface, SESSION) && !g_strcmp0(path, s->session)) {
    s->revision++;
    if (!strcmp(signal, "Lock")) {
      s->lock_requested = TRUE; s->locked = TRUE; publish(s);
      if (s->available) get_hint(s);
    }
    else if (!strcmp(signal, "Unlock")) {
      /* Unlock is a request; only a fresh LockedHint may lift the UI guard.
       * It never resumes the shared crypto service. */
      s->lock_requested = FALSE; get_hint(s);
    }
  } else if (!strcmp(interface, PROPERTIES) && !strcmp(signal, "PropertiesChanged")) {
    const char *property_interface; g_autoptr(GVariant) values = NULL; g_auto(GStrv) invalid = NULL;
    g_variant_get(args, "(&s@a{sv}^as)", &property_interface, &values, &invalid);
    if (!g_strcmp0(path, s->session) && !strcmp(property_interface, SESSION)) {
      g_autoptr(GVariant) user = g_variant_lookup_value(values, "User", NULL);
      g_autoptr(GVariant) locked = g_variant_lookup_value(values, "LockedHint", NULL);
      gboolean invalidated = g_strv_contains((const char *const *)invalid, "LockedHint") ||
                             g_strv_contains((const char *const *)invalid, "User");
      if (!user && !locked && !invalidated) return;
      s->revision++;
      if (s->available && !user && !invalidated && locked && g_variant_is_of_type(locked, G_VARIANT_TYPE_BOOLEAN)) {
        s->lock_requested = FALSE; s->locked = g_variant_get_boolean(locked); publish(s);
      } else {
        s->available = FALSE; publish(s); get_hint(s);
      }
    } else if (!g_strcmp0(path, s->user) && !strcmp(property_interface, USER)) {
      g_autoptr(GVariant) display = g_variant_lookup_value(values, "Display", NULL);
      if (display || g_strv_contains((const char *const *)invalid, "Display")) select_session(s);
    }
  } else if (!strcmp(interface, LOGIN ".Manager") &&
             (!strcmp(signal, "SessionNew") || !strcmp(signal, "SessionRemoved"))) {
    const char *id, *session; g_variant_get(args, "(&s&o)", &id, &session);
    if (!strcmp(signal, "SessionRemoved") && !g_strcmp0(session, s->session)) select_session(s);
    else if (!strcmp(signal, "SessionNew") && !s->available) select_session(s);
  }
}
static void appeared(GDBusConnection *bus, const char *name, const char *owner, gpointer data) {
  (void)bus; (void)name; GrottoSessionLock *s = data;
  s->epoch++; g_free(s->owner); s->owner = g_strdup(owner);
  if (s->subscription) g_dbus_connection_signal_unsubscribe(s->bus, s->subscription);
  s->subscription = g_dbus_connection_signal_subscribe(s->bus, owner, NULL, NULL, NULL, NULL,
      G_DBUS_SIGNAL_FLAGS_NONE, signal_received, s, NULL);
  select_session(s);
}
static void vanished(GDBusConnection *bus, const char *name, gpointer data) {
  (void)bus; (void)name; GrottoSessionLock *s = data;
  s->epoch++; s->revision++; s->available = FALSE;
  g_clear_pointer(&s->owner, g_free); g_clear_pointer(&s->session, g_free); publish(s);
}
static void watch(GrottoSessionLock *s) {
  s->watch = g_bus_watch_name_on_connection(s->bus, LOGIN, G_BUS_NAME_WATCHER_FLAGS_NONE, appeared, vanished, s, NULL);
}
static void bus_ready(GObject *source, GAsyncResult *result, gpointer data) {
  (void)source; GrottoSessionLock *s = data;
  g_autoptr(GDBusConnection) bus = g_bus_get_finish(result, NULL);
  if (!s->stopped && bus) { s->bus = g_object_ref(bus); watch(s); }
  g_object_unref(s);
}
GrottoSessionLock *grotto_session_lock_new(SignerApprovalService *approval, GSettings *settings, GDBusConnection *bus) {
  GrottoSessionLock *s = g_object_new(GROTTO_TYPE_SESSION_LOCK, NULL);
  s->approval = signer_approval_service_ref(approval); s->cancel = g_cancellable_new();
  if (settings) {
    s->settings = g_object_ref(settings);
    s->setting_handler = g_signal_connect(settings, "changed::pause-on-screen-lock", G_CALLBACK(policy_changed), s);
  }
  publish(s);
  if (bus) { s->bus = g_object_ref(bus); watch(s); }
  else g_bus_get(G_BUS_TYPE_SYSTEM, s->cancel, bus_ready, g_object_ref(s));
  return s;
}
void grotto_session_lock_stop(GrottoSessionLock *s) {
  if (s->stopped) return;
  s->stopped = TRUE; s->epoch++; g_cancellable_cancel(s->cancel);
  if (s->watch) { g_bus_unwatch_name(s->watch); s->watch = 0; }
  if (s->subscription) { g_dbus_connection_signal_unsubscribe(s->bus, s->subscription); s->subscription = 0; }
  if (s->setting_handler) { g_signal_handler_disconnect(s->settings, s->setting_handler); s->setting_handler = 0; }
}
static void dispose(GObject *object) {
  GrottoSessionLock *s = GROTTO_SESSION_LOCK(object); grotto_session_lock_stop(s);
  g_clear_object(&s->bus); g_clear_object(&s->cancel); g_clear_object(&s->settings);
  if (s->approval) { signer_approval_service_unref(s->approval); s->approval = NULL; }
  g_clear_pointer(&s->owner, g_free); g_clear_pointer(&s->session, g_free); g_clear_pointer(&s->user, g_free);
  G_OBJECT_CLASS(grotto_session_lock_parent_class)->dispose(object);
}
static void grotto_session_lock_class_init(GrottoSessionLockClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = dispose;
  changed = g_signal_new("changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}
static void grotto_session_lock_init(GrottoSessionLock *s) { (void)s; }
