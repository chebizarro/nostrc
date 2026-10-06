#pragma once
#include <gtk/gtk.h>

G_BEGIN_DECLS

/* An app indicator (StatusNotifierItem, as KDE and Ubuntu's AppIndicator
 * extension show it; W32, owner decision). Nothing is registered unless an
 * org.kde.StatusNotifierWatcher owns its name on the session bus - stock
 * GNOME has none, and then this does nothing at all. The item shows the
 * app's icon with a menu (com.canonical.dbusmenu): Open <title> and Quit;
 * a left click opens the app. No library beyond GIO is needed. */
#define GN_TYPE_STATUS_NOTIFIER (gn_status_notifier_get_type())
G_DECLARE_FINAL_TYPE(GnStatusNotifier, gn_status_notifier, GN, STATUS_NOTIFIER, GObject)

/* icon_name: an installed themed icon (the app id). title: the app's name.
 * The notifier follows the watcher: registers when one appears, drops out
 * when it leaves. "activate" (no args) is emitted for Open and a click;
 * "quit" for Quit. */
GnStatusNotifier *gn_status_notifier_new(GApplication *app, const gchar *icon_name,
                                         const gchar *title);
/* Whether a watcher currently shows the item. */
gboolean gn_status_notifier_is_registered(GnStatusNotifier *self);

G_END_DECLS
