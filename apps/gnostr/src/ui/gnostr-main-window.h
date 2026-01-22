#ifndef GNOSTR_MAIN_WINDOW_H
#define GNOSTR_MAIN_WINDOW_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_MAIN_WINDOW (gnostr_main_window_get_type())

G_DECLARE_FINAL_TYPE(GnostrMainWindow, gnostr_main_window, GNOSTR, MAIN_WINDOW, GtkApplicationWindow)

GnostrMainWindow *gnostr_main_window_new(GtkApplication *app);

/* Demand-driven profile prefetch: enqueue author(s) by 64-hex pubkey */
void gnostr_main_window_enqueue_profile_author(GnostrMainWindow *self, const char *pubkey_hex);
void gnostr_main_window_enqueue_profile_authors(GnostrMainWindow *self, const char **pubkey_hexes, size_t count);

/* Public: Show a toast message in the main window (auto-dismisses after 2s) */
void gnostr_main_window_show_toast(GtkWidget *window, const char *message);

G_END_DECLS

#endif /* GNOSTR_MAIN_WINDOW_H */
