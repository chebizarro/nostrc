#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include "ui/gnostr-main-window.h"
#include "storage_ndb.h"

static void on_activate(GtkApplication *app, gpointer user_data) {
  GnostrMainWindow *win = gnostr_main_window_new(app);
  gtk_window_present(GTK_WINDOW(win));
}

static void on_app_quit(GSimpleAction *action, GVariant *param, gpointer user_data) {
  (void)action; (void)param;
  GtkApplication *app = GTK_APPLICATION(user_data);
  g_application_quit(G_APPLICATION(app));
}

static void on_shutdown(GApplication *app, gpointer user_data) {
  (void)app; (void)user_data;
  storage_ndb_shutdown();
}

int main(int argc, char **argv) {
  g_set_prgname("gnostr");
  GtkApplication *app = gtk_application_new("org.gnostr.Client", G_APPLICATION_DEFAULT_FLAGS);
  /* Install app actions */
  static const GActionEntry app_entries[] = {
    { "quit", on_app_quit, NULL, NULL, NULL },
  };
  g_action_map_add_action_entries(G_ACTION_MAP(app), app_entries, G_N_ELEMENTS(app_entries), app);
  const char *quit_accels[] = { "<Primary>q", NULL };
  gtk_application_set_accels_for_action(app, "app.quit", quit_accels);
  g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);

  /* Initialize NostrdB-backed storage in user cache directory */
  gchar *dbdir = g_build_filename(g_get_user_cache_dir(), "gnostr", "ndb", NULL);
  g_mkdir_with_parents(dbdir, 0700);
  const char *opts = "{\"mapsize\":1073741824,\"ingester_threads\":1}";
  if (!storage_ndb_init(dbdir, opts)) {
    g_warning("Failed to initialize storage at %s", dbdir);
  }
  g_free(dbdir);

  g_signal_connect(app, "shutdown", G_CALLBACK(on_shutdown), NULL);
  int status = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);
  return status;
}
