#pragma once
#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GH_TYPE_TRAY (gh_tray_get_type())
G_DECLARE_FINAL_TYPE(GhTray, gh_tray, GH, TRAY, GObject)

/* Owns the Groundhog menu and routes all clicks on the main context. */
GhTray *gh_tray_new(GtkApplication *app, GSettings *settings);

G_END_DECLS
