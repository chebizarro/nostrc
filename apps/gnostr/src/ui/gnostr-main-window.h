#ifndef GNOSTR_MAIN_WINDOW_H
#define GNOSTR_MAIN_WINDOW_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_MAIN_WINDOW (gnostr_main_window_get_type())

G_DECLARE_FINAL_TYPE(GnostrMainWindow, gnostr_main_window, GNOSTR, MAIN_WINDOW, GtkApplicationWindow)

GnostrMainWindow *gnostr_main_window_new(GtkApplication *app);

G_END_DECLS

#endif /* GNOSTR_MAIN_WINDOW_H */
