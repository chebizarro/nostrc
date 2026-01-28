#include <adwaita.h>
#include <glib/gstdio.h>
#include <errno.h>
#include "ui/gnostr-main-window.h"
#include "ui/gnostr-tray-icon.h"
#include "model/gn-ndb-sub-dispatcher.h"
#include "storage_ndb.h"
#include "util/gnostr_paths.h"
#include "util/gnostr_e2e.h"
#include "util/cache_prune.h"

/* Global tray icon instance (Linux only) */
static GnostrTrayIcon *g_tray_icon = NULL;

static void on_activate(GApplication *app, gpointer user_data) {
  (void)user_data;
  GnostrMainWindow *win = gnostr_main_window_new(ADW_APPLICATION(app));
  gtk_window_present(GTK_WINDOW(win));

  /* Create system tray icon now that GTK is fully initialized.
   * Must be done here (not before g_application_run) to avoid
   * macOS Core Graphics assertion failures. */
  if (!g_tray_icon && gnostr_tray_icon_is_available()) {
    g_tray_icon = gnostr_tray_icon_new(GTK_APPLICATION(app));
    if (g_tray_icon) {
      g_debug("System tray icon enabled");
    }
  }

  /* Associate window with tray icon for show/hide functionality */
  if (g_tray_icon) {
    gnostr_tray_icon_set_window(g_tray_icon, GTK_WINDOW(win));
  }

  if (gnostr_e2e_enabled()) {
    gnostr_e2e_mark_ready();
  }
}

static void on_app_quit(GSimpleAction *action, GVariant *param, gpointer user_data) {
  (void)action; (void)param;
  GApplication *app = G_APPLICATION(user_data);
  g_application_quit(app);
}

static void on_shutdown(GApplication *app, gpointer user_data) {
  (void)app; (void)user_data;

  /* Clean up tray icon */
  g_clear_object(&g_tray_icon);

  storage_ndb_shutdown();
}

int main(int argc, char **argv) {
  g_set_prgname("gnostr");

  /* Initialize libadwaita - required for adaptive/responsive features */
  AdwApplication *app = adw_application_new("org.gnostr.Client", G_APPLICATION_DEFAULT_FLAGS);

  /* Install app actions */
  static const GActionEntry app_entries[] = {
    { "quit", on_app_quit, NULL, NULL, NULL },
  };
  g_action_map_add_action_entries(G_ACTION_MAP(app), app_entries, G_N_ELEMENTS(app_entries), app);
  const char *quit_accels[] = { "<Primary>q", NULL };
  gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.quit", quit_accels);
  g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);

  /* Initialize subscription dispatcher BEFORE storage to register callback */
  gn_ndb_dispatcher_init();

  /* Initialize cache pruning system (runs before storage init to free space) */
  gnostr_cache_prune_init();

  /* Initialize NostrdB-backed storage in user cache directory */
  gchar *dbdir = gnostr_get_db_dir();
  g_message("Attempting to initialize storage at %s", dbdir);
  int mkdir_rc = g_mkdir_with_parents(dbdir, 0700);
  if (mkdir_rc != 0) {
    g_warning("g_mkdir_with_parents failed: %d (%s)", mkdir_rc, g_strerror(errno));
  }
  /* Enable signature validation to prevent storing malformed events that cause heap corruption.
   * ingester_threads=1 to minimize reader slot contention (LMDB default is ~126 slots). */
  const char *opts = "{\"mapsize\":1073741824,\"ingester_threads\":1}";
  fprintf(stderr, "[main] About to call storage_ndb_init(dbdir=%s, opts=%s)\n", dbdir, opts);
  fflush(stderr);
  if (!storage_ndb_init(dbdir, opts)) {
    fprintf(stderr, "[main] storage_ndb_init FAILED for %s\n", dbdir);
    fflush(stderr);
    g_warning("Failed to initialize storage at %s", dbdir);
  } else {
    fprintf(stderr, "[main] storage_ndb_init SUCCESS for %s\n", dbdir);
    fflush(stderr);

    if (gnostr_e2e_enabled()) {
      GError *seed_err = NULL;
      if (!gnostr_e2e_seed_storage(&seed_err)) {
        g_warning("e2e: seed failed: %s", seed_err ? seed_err->message : "(unknown)");
        g_clear_error(&seed_err);
      }
    }
  }
  g_free(dbdir);

  g_signal_connect(app, "shutdown", G_CALLBACK(on_shutdown), NULL);
  int status = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);
  return status;
}
