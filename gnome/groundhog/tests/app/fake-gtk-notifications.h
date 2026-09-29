/* H4 (privacy charter §9.1): a fake org.gtk.Notifications server on a private
 * test bus (tests/common/nostrc-test-bus.h). With GNOTIFICATION_BACKEND=gtk
 * set before an application sends its first notification, GLib's GTK backend
 * delivers every g_application_send_notification() and
 * g_application_withdraw_notification() here, serialized exactly as a GNOME
 * desktop receives them. Each call is recorded with the time of a GhClock
 * (nullable), in arrival order. GLib picks its backend once per application,
 * so the server must own its name before the first send.
 *
 * GLib's GTK backend does not carry a notification's category; its
 * freedesktop backend does (the "category" and "urgency" hints). The fake
 * also owns org.freedesktop.Notifications and records every Notify's
 * parameters, for a test that selects GNOTIFICATION_BACKEND=freedesktop. */
#ifndef FAKE_GTK_NOTIFICATIONS_H
#define FAKE_GTK_NOTIFICATIONS_H

#include <gio/gio.h>

#include "gh-clock.h"

G_BEGIN_DECLS

typedef struct {
  gboolean added;          /* AddNotification; FALSE: RemoveNotification */
  gchar *app_id;
  gchar *id;
  GVariant *notification;  /* a{sv} of an AddNotification, else NULL */
  gint64 at;               /* the clock's monotonic time at arrival; 0 without one */
} FakeNotificationCall;

typedef struct _FakeGtkNotifications FakeGtkNotifications;

/* Registers /org/gtk/Notifications on owner and owns org.gtk.Notifications. */
FakeGtkNotifications *fake_gtk_notifications_new(GDBusConnection *owner);
void fake_gtk_notifications_free(FakeGtkNotifications *fake);
void fake_gtk_notifications_set_clock(FakeGtkNotifications *fake, GhClock *clock);

/* Returns once every call client sent before this one was recorded (a
 * round trip through the same connection and main context). */
void fake_gtk_notifications_sync(FakeGtkNotifications *fake, GDBusConnection *client);

/* FakeNotificationCall, in arrival order. */
GPtrArray *fake_gtk_notifications_get_calls(FakeGtkNotifications *fake);
void fake_gtk_notifications_clear(FakeGtkNotifications *fake);
/* What id currently shows (its last AddNotification with no later
 * RemoveNotification), or NULL. Borrowed. */
GVariant *fake_gtk_notifications_lookup(FakeGtkNotifications *fake, const gchar *id);
/* Calls of that kind for id (NULL: any id). */
guint fake_gtk_notifications_count(FakeGtkNotifications *fake, gboolean added, const gchar *id);
/* The parameters (susssasa{sv}i) of every org.freedesktop.Notifications
 * Notify, in arrival order. */
GPtrArray *fake_gtk_notifications_get_fdo_notifies(FakeGtkNotifications *fake);
/* Every recorded payload, printed (for canary scans). */
gchar *fake_gtk_notifications_dump(FakeGtkNotifications *fake);

G_END_DECLS
#endif
