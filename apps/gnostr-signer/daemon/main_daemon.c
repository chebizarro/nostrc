#include <gio/gio.h>
#include <signal.h>
#include <sys/resource.h>
#include <stdlib.h>
#include <string.h>
#include "nip55l_dbus_names.h"
#include "ipc.h"

// Use nip55l GLib implementation. This exports the full signer protocol.
// Prototypes provided by nips/nip55l/src/glib/signer_service_g.c
extern guint signer_export(GDBusConnection *conn, const char *object_path);
extern void  signer_unexport(GDBusConnection *conn, guint reg_id);

// IPC abstraction (POSIX UDS / TCP / Windows named pipe) from ipc.h

// Global state for signal handling and graceful shutdown
static GMainLoop *loop = NULL;
static guint obj_reg_id = 0;
static GnostrIpcServer *ipc_srv = NULL;
static GDBusConnection *g_dbus_conn = NULL;
static gboolean g_shutdown_requested = FALSE;
static GMutex g_shutdown_mutex;

// Daemon version and build info
#define DAEMON_VERSION "0.1.0"
#define DAEMON_NAME "gnostr-signer-daemon"

static void on_bus_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data) {
  (void)name; (void)user_data;
  
  g_dbus_conn = connection;
  g_object_ref(g_dbus_conn);
  
  g_message("%s v%s: D-Bus connection acquired", DAEMON_NAME, DAEMON_VERSION);
  
  obj_reg_id = signer_export(connection, ORG_NOSTR_SIGNER_OBJECT_PATH);
  if (!obj_reg_id) {
    g_critical("DBUS_EXPORT_FAILED: path=%s bus=%s", ORG_NOSTR_SIGNER_OBJECT_PATH, ORG_NOSTR_SIGNER_BUS);
    if (loop) g_main_loop_quit(loop);
    return;
  }
  
  g_message("gnostr-signer: D-Bus interface exported at %s on %s", 
            ORG_NOSTR_SIGNER_OBJECT_PATH, ORG_NOSTR_SIGNER_BUS);
  
  // Start IPC listener. Endpoint selection via env:
  //  NOSTR_SIGNER_ENDPOINT examples:
  //    unix:/run/user/1000/gnostr/signer.sock
  //    tcp:127.0.0.1:5897
  //    npipe:\\.\pipe\gnostr-signer (Windows)
  const char *endpoint = g_getenv("NOSTR_SIGNER_ENDPOINT");
  if (!endpoint || !*endpoint) {
    endpoint = g_getenv("NOSTR_SIGNER_SOCK"); // legacy
  }
  
  ipc_srv = gnostr_ipc_server_start(endpoint);
  if (!ipc_srv) {
    const char *ep = (endpoint && *endpoint) ? endpoint : "(default)";
    g_warning("gnostr-signer: failed to start IPC server for endpoint '%s'", ep);
    g_warning("gnostr-signer: continuing with D-Bus interface only");
  } else {
    g_message("gnostr-signer: daemon fully initialized and ready");
  }
}

static void on_name_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data) {
  (void)connection; (void)user_data; (void)name;
  g_message("gnostr-signer: name acquired %s", ORG_NOSTR_SIGNER_BUS);
}

static void on_name_lost(GDBusConnection *connection, const gchar *name, gpointer user_data) {
  (void)name; (void)user_data;
  
  g_mutex_lock(&g_shutdown_mutex);
  if (g_shutdown_requested) {
    g_mutex_unlock(&g_shutdown_mutex);
    return;  // Already shutting down
  }
  g_shutdown_requested = TRUE;
  g_mutex_unlock(&g_shutdown_mutex);
  
  g_warning("gnostr-signer: lost D-Bus name or could not acquire bus");
  
  if (connection && obj_reg_id) {
    g_message("gnostr-signer: unexporting D-Bus interface");
    signer_unexport(connection, obj_reg_id);
    obj_reg_id = 0;
  }
  
  // Stop IPC server
  if (ipc_srv) {
    g_message("gnostr-signer: stopping IPC server");
    gnostr_ipc_server_stop(ipc_srv);
    ipc_srv = NULL;
  }
  
  if (loop) {
    g_message("gnostr-signer: initiating shutdown");
    g_main_loop_quit(loop);
  }
}

static void handle_sig(int sig) {
  const char *sig_name = (sig == SIGINT) ? "SIGINT" : (sig == SIGTERM) ? "SIGTERM" : "SIGNAL";
  
  g_mutex_lock(&g_shutdown_mutex);
  if (g_shutdown_requested) {
    g_mutex_unlock(&g_shutdown_mutex);
    g_message("gnostr-signer: shutdown already in progress, ignoring %s", sig_name);
    return;
  }
  g_shutdown_requested = TRUE;
  g_mutex_unlock(&g_shutdown_mutex);
  
  g_message("gnostr-signer: received %s, initiating graceful shutdown", sig_name);
  
  // Stop IPC early to unblock accept loop
  if (ipc_srv) {
    g_message("gnostr-signer: stopping IPC server");
    gnostr_ipc_server_stop(ipc_srv);
    ipc_srv = NULL;
  }
  
  if (loop) {
    g_main_loop_quit(loop);
  }
}

static void print_usage(const char *prog_name) {
  g_print("Usage: %s [OPTIONS]\n", prog_name);
  g_print("\n");
  g_print("Nostr Signer Daemon - Secure key management and signing service\n");
  g_print("\n");
  g_print("Options:\n");
  g_print("  -h, --help              Show this help message\n");
  g_print("  -v, --version           Show version information\n");
  g_print("  --system                Use system bus instead of session bus\n");
  g_print("\n");
  g_print("Environment Variables:\n");
  g_print("  NOSTR_SIGNER_ENDPOINT   IPC endpoint (unix:/path, tcp:host:port)\n");
  g_print("  NOSTR_SIGNER_MAX_CONNECTIONS  Maximum concurrent TCP connections (default: 100)\n");
  g_print("  NOSTR_DEBUG             Enable debug logging\n");
  g_print("\n");
}

int main(int argc, char **argv) {
  GBusType bus_type = G_BUS_TYPE_SESSION;
  gboolean show_version = FALSE;
  
  // Parse command line arguments
  for (int i = 1; i < argc; i++) {
    if (g_strcmp0(argv[i], "-h") == 0 || g_strcmp0(argv[i], "--help") == 0) {
      print_usage(argv[0]);
      return 0;
    } else if (g_strcmp0(argv[i], "-v") == 0 || g_strcmp0(argv[i], "--version") == 0) {
      show_version = TRUE;
    } else if (g_strcmp0(argv[i], "--system") == 0) {
      bus_type = G_BUS_TYPE_SYSTEM;
    } else {
      g_printerr("Unknown option: %s\n", argv[i]);
      print_usage(argv[0]);
      return 1;
    }
  }
  
  if (show_version) {
    g_print("%s version %s\n", DAEMON_NAME, DAEMON_VERSION);
    g_print("Built with GLib %d.%d.%d\n", GLIB_MAJOR_VERSION, GLIB_MINOR_VERSION, GLIB_MICRO_VERSION);
    return 0;
  }
  
  g_message("%s v%s starting...", DAEMON_NAME, DAEMON_VERSION);
  
  // Initialize mutex for shutdown coordination
  g_mutex_init(&g_shutdown_mutex);
  
  // Disable core dumps when handling secrets
  struct rlimit rl;
  rl.rlim_cur = 0;
  rl.rlim_max = 0;
  if (setrlimit(RLIMIT_CORE, &rl) != 0) {
    g_warning("failed to disable core dumps: %s", g_strerror(errno));
  } else {
    g_message("core dumps disabled for security");
  }
  
  // Set up signal handlers for graceful shutdown
  signal(SIGINT, handle_sig);
  signal(SIGTERM, handle_sig);
  signal(SIGPIPE, SIG_IGN);  // Ignore broken pipe
  
  g_message("registering D-Bus name on %s bus", 
            bus_type == G_BUS_TYPE_SYSTEM ? "system" : "session");
  
  loop = g_main_loop_new(NULL, FALSE);
  
  guint owner_id = g_bus_own_name(bus_type,
                                  ORG_NOSTR_SIGNER_BUS,
                                  G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT | G_BUS_NAME_OWNER_FLAGS_REPLACE,
                                  on_bus_acquired,
                                  on_name_acquired,
                                  on_name_lost,
                                  NULL,
                                  NULL);
  
  g_message("entering main loop");
  g_main_loop_run(loop);
  
  g_message("main loop exited, cleaning up");
  
  // Cleanup
  g_bus_unown_name(owner_id);
  
  if (g_dbus_conn) {
    g_object_unref(g_dbus_conn);
    g_dbus_conn = NULL;
  }
  
  g_main_loop_unref(loop);
  loop = NULL;
  
  g_mutex_clear(&g_shutdown_mutex);
  
  g_message("%s shutdown complete", DAEMON_NAME);
  
  return 0;
}
