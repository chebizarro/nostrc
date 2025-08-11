#include <gtk/gtk.h>
#include "ui/gnostr-main-window.h"

static void on_activate(GtkApplication *app, gpointer user_data) {
  GnostrMainWindow *win = gnostr_main_window_new(app);
  gtk_window_present(GTK_WINDOW(win));
}

int main(int argc, char **argv) {
  g_set_prgname("gnostr");
  GtkApplication *app = gtk_application_new("org.gnostr.Client", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
  int status = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);
  return status;
}
