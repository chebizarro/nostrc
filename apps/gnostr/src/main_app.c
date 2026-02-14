#include <adwaita.h>
#include <glib/gstdio.h>
#include <errno.h>
#include "ui/gnostr-main-window.h"
#include "ui/gnostr-tray-icon.h"
#include "model/gn-ndb-sub-dispatcher.h"
#include <nostr-gobject-1.0/storage_ndb.h>
#include "util/gnostr_paths.h"
#include "util/gnostr_e2e.h"
#include "util/cache_prune.h"
#include "util/utils.h"
#include "util/gnostr-plugin-manager.h"
#include "sync/gnostr-sync-service.h"
#include "sync/gnostr-sync-bridge.h"

/* Global tray icon instance (Linux only) */
static GnostrTrayIcon *g_tray_icon = NULL;

/* Update tray icon with relay connection status.
 * Called by main window when relay status changes. */
void gnostr_app_update_relay_status(int connected_count, int total_count) {
  if (!g_tray_icon) return;

  GnostrTrayRelayState state;
  if (total_count == 0) {
    state = GNOSTR_TRAY_RELAY_DISCONNECTED;
  } else if (connected_count == 0) {
    state = GNOSTR_TRAY_RELAY_DISCONNECTED;
  } else if (connected_count < total_count) {
    state = GNOSTR_TRAY_RELAY_CONNECTING;
  } else {
    state = GNOSTR_TRAY_RELAY_CONNECTED;
  }

  gnostr_tray_icon_set_relay_status(g_tray_icon, connected_count, total_count, state);
}

/* nostrc-75o3.1: Deferred plugin discovery context.
 * Plugin discovery and loading run after the first frame so the window
 * appears immediately instead of blocking for several seconds. */
typedef struct {
  GtkApplication *app;
  GtkWindow      *win;
} DeferredPluginCtx;

static gboolean
deferred_plugin_init_cb(gpointer data)
{
  DeferredPluginCtx *ctx = data;
  GnostrPluginManager *plugin_manager = gnostr_plugin_manager_get_default();
  gnostr_plugin_manager_init_with_app(plugin_manager, ctx->app);
  gnostr_plugin_manager_discover_plugins(plugin_manager);
  gnostr_plugin_manager_load_enabled_plugins(plugin_manager);
  gnostr_plugin_manager_set_main_window(plugin_manager, ctx->win);
  g_free(ctx);
  return G_SOURCE_REMOVE;
}

static void on_activate(GApplication *app, gpointer user_data) {
  (void)user_data;

  GnostrMainWindow *win = gnostr_main_window_new(ADW_APPLICATION(app));
  gtk_window_present(GTK_WINDOW(win));

  /* nostrc-75o3.1: Defer heavy plugin discovery until after the first frame.
   * The window is already visible in LOADING state at this point. */
  DeferredPluginCtx *ctx = g_new0(DeferredPluginCtx, 1);
  ctx->app = GTK_APPLICATION(app);
  ctx->win = GTK_WINDOW(win);
  g_idle_add_full(G_PRIORITY_LOW, deferred_plugin_init_cb, ctx, NULL);

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

  g_message("gnostr: shutdown initiated");

  /* Shutdown sync bridge (unsubscribes from EventBus) */
  gnostr_sync_bridge_shutdown();

  /* Shutdown sync service (cancels pending sync, stops timer) */
  gnostr_sync_service_shutdown();

  /* Shutdown plugin manager */
  GnostrPluginManager *plugin_manager = gnostr_plugin_manager_get_default();
  gnostr_plugin_manager_shutdown(plugin_manager);

  /* Clean up tray icon */
  g_clear_object(&g_tray_icon);

  /*
   * nostrc-b1vg: Fix TLS cleanup crash by ensuring proper shutdown order.
   * The crash was caused by cleaning up SoupSession while relay pool connections
   * still had references to TLS certificates. Order matters:
   * 1. Clean up relay pool first (closes WebSocket/network connections)
   * 2. Drain main loop to let pending async callbacks complete
   * 3. Clean up SoupSession (now safe since no pending TLS operations)
   */

  /* Step 1: Clean up shared relay query pool - closes connections gracefully */
  gnostr_cleanup_shared_query_pool();

  /* Step 2: Drain pending main loop events to allow async cleanup callbacks
   * to complete. This prevents use-after-free when callbacks reference
   * TLS certificates that would be freed by soup session cleanup. */
  GMainContext *ctx = g_main_context_default();
  int drain_iterations = 0;
  while (g_main_context_pending(ctx) && drain_iterations < 100) {
    g_main_context_iteration(ctx, FALSE);
    drain_iterations++;
  }
  if (drain_iterations > 0) {
    g_debug("gnostr: drained %d pending main loop events", drain_iterations);
  }

  /* Step 2.5 (nostrc-i26h): Invalidate TLS transaction cache after draining.
   * Subscription callbacks during the drain may have opened transactions.
   * Invalidate them now before storage shutdown to prevent page pinning. */
  storage_ndb_invalidate_txn_cache();

  /* Step 3: Clean up shared SoupSession - now safe to destroy TLS state */
  gnostr_cleanup_shared_soup_session();

  /* Step 4: Clean up storage (nostrc-i26h: force-closes any remaining TLS txn) */
  storage_ndb_shutdown();

  g_message("gnostr: shutdown complete");
}

/* nostrc-9bn: Auto-discover GSettings schemas so the app can run without
 * GSETTINGS_SCHEMA_DIR being set externally. CMake compiles gschemas.compiled
 * into the same directory as the binary; installed layout puts them under
 * PREFIX/share/glib-2.0/schemas. */
static void
gnostr_ensure_gsettings_schemas(const char *argv0)
{
  if (g_getenv("GSETTINGS_SCHEMA_DIR"))
    return;  /* Already set by run-gnostr.sh or user */

  /* Resolve argv[0] to get the directory containing the binary */
  g_autofree gchar *bin_dir = NULL;
  if (g_path_is_absolute(argv0)) {
    bin_dir = g_path_get_dirname(argv0);
  } else {
    g_autofree gchar *cwd = g_get_current_dir();
    g_autofree gchar *abs_path = g_build_filename(cwd, argv0, NULL);
    bin_dir = g_path_get_dirname(abs_path);
  }

  if (!bin_dir)
    return;

  /* Try 1: Development build — gschemas.compiled alongside binary */
  g_autofree gchar *dev_schema = g_build_filename(bin_dir, "gschemas.compiled", NULL);
  if (g_file_test(dev_schema, G_FILE_TEST_EXISTS)) {
    g_setenv("GSETTINGS_SCHEMA_DIR", bin_dir, FALSE);
    return;
  }

  /* Try 2: Installed layout — PREFIX/bin/../share/glib-2.0/schemas */
  g_autofree gchar *inst_dir = g_build_filename(bin_dir, "..", "share",
                                                 "glib-2.0", "schemas", NULL);
  g_autofree gchar *inst_schema = g_build_filename(inst_dir, "gschemas.compiled", NULL);
  if (g_file_test(inst_schema, G_FILE_TEST_EXISTS)) {
    g_setenv("GSETTINGS_SCHEMA_DIR", inst_dir, FALSE);
    return;
  }

  /* nostrc-fm2g: Warn before GLib's fatal abort so the user knows what to do */
  g_warning("GSettings schemas not found (tried %s and %s). "
            "Set GSETTINGS_SCHEMA_DIR to the directory containing "
            "gschemas.compiled, or run via run-gnostr.sh.",
            bin_dir, inst_dir);
}

int main(int argc, char **argv) {
  gnostr_ensure_gsettings_schemas(argv[0]);
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
  g_autofree gchar *dbdir = gnostr_get_db_dir();
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

    /* Initialize sync bridge (subscribes to EventBus for data refresh).
     * User pubkey is set later on login via gnostr_sync_bridge_set_user_pubkey(). */
    gnostr_sync_bridge_init(NULL);
  }

  g_signal_connect(app, "shutdown", G_CALLBACK(on_shutdown), NULL);
  int status = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);
  return status;
}
