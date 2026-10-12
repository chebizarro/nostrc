#pragma once
#include <glib.h>

G_BEGIN_DECLS

/* Run before any GLib/GTK call that can reach the session bus (gtk_init,
 * g_application_register). A child process resolves and connects the bus
 * exactly as GLib does for G_BUS_TYPE_SESSION: $DBUS_SESSION_BUS_ADDRESS, or
 * $XDG_RUNTIME_DIR/bus, or `dbus-launch --autolaunch`, which on macOS returns
 * the launchd socket. If that does not finish the AUTH handshake within
 * timeout_ms, DBUS_SESSION_BUS_ADDRESS is pointed at an address that fails
 * at once, so every later session-bus connection in this process errors
 * instead of blocking, and TRUE is returned: the caller must then start
 * G_APPLICATION_NON_UNIQUE. A bus that answers, or fails by itself, returns
 * FALSE and changes nothing. */
gboolean gn_session_bus_fallback_if_unresponsive(guint timeout_ms);

G_END_DECLS
