#include <gio/gio.h>
#include <signal.h>
#ifdef G_OS_UNIX
#include <glib-unix.h>
#endif
#include <sys/resource.h>
#include <stdlib.h>
#include <string.h>
#include "legacy_dirs.h"
#include "nip55l_dbus_names.h"
#include "ipc.h"
#include "bunker_service.h"
#include <glib/gstdio.h>
#include "settings_manager.h"
#include "accounts_store.h"
#include "grotto_control.h"

// Use nip55l GLib implementation. This exports the full signer protocol.
// Prototypes provided by nips/nip55l/src/glib/signer_service_g.c
extern guint signer_export(GDBusConnection *conn, const char *object_path);
extern void  signer_unexport(GDBusConnection *conn, guint reg_id);
extern guint signer_pending_count(void);

// IPC abstraction (POSIX UDS / TCP / Windows named pipe) from ipc.h

// Global state for signal handling and graceful shutdown
static GMainLoop *loop = NULL;
static guint obj_reg_id = 0;
static GnostrIpcServer *ipc_srv = NULL;
static GDBusConnection *g_dbus_conn = NULL;
static gboolean g_shutdown_requested = FALSE;
static GMutex g_shutdown_mutex;
static int g_exit_status = 0;

// NIP-46 bunker mode (--bunker)
static gboolean g_bunker_requested = FALSE;
static gboolean g_no_approval_ui = FALSE;
static gboolean g_bunker_started = FALSE;
static BunkerService *g_bunker = NULL;
static GrottoControl *g_control = NULL;

// Daemon version and build info
#define DAEMON_VERSION GROTTO_VERSION
#define DAEMON_NAME "grotto-daemon"

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
  
  signer_approval_service_set_no_approval_ui(signer_get_approval_service(), g_no_approval_ui);
  g_autoptr(GError) control_error = NULL;
  g_control = grotto_control_new(connection, signer_get_approval_service(), NULL, &control_error);
  if (!g_control) {
    g_critical("CONTROL_EXPORT_FAILED: %s", control_error ? control_error->message : "unknown error");
    g_exit_status = 1;
    if (loop) g_main_loop_quit(loop);
    return;
  }
  g_bunker = bunker_service_new();
  bunker_service_set_no_approval_ui(g_bunker, g_no_approval_ui);
  grotto_control_set_bunker(g_control, g_bunker, g_bunker_requested);

  g_message("grotto: D-Bus interface exported at %s on %s", 
            ORG_NOSTR_SIGNER_OBJECT_PATH, ORG_NOSTR_SIGNER_BUS);
  
  /* Start IPC listener. Endpoint selection via env:
   *  NOSTR_SIGNER_ENDPOINT examples:
   *    unix:/run/user/1000/gnostr/signer.sock
   *    tcp:127.0.0.1:5897
   *    npipe:\\.\pipe\grotto (Windows)
   */
  const char *endpoint = g_getenv("NOSTR_SIGNER_ENDPOINT");
  if (!endpoint || !*endpoint) {
    endpoint = g_getenv("NOSTR_SIGNER_SOCK"); /* legacy */
  }
  /* The NIP-5F listener goes through the same approval gate as the D-Bus
   * methods (principal from the socket peer's kernel credentials, shared
   * signer-grants.ini, ApprovalRequested on the bus; nostrc-q23h). It stays
   * opt-in: run it only when an endpoint is configured explicitly. */
  if (!endpoint || !*endpoint) {
    g_message("grotto: no NOSTR_SIGNER_ENDPOINT; NIP-5F socket disabled (D-Bus only)");
    g_message("grotto: daemon fully initialized and ready");
    return;
  }

  g_autoptr(GError) ipc_error = NULL;
  ipc_srv = gnostr_ipc_server_start(endpoint, &ipc_error);
  if (!ipc_srv) {
    const char *ep = (endpoint && *endpoint) ? endpoint : "(default)";
    g_warning("grotto: failed to start IPC server for endpoint '%s': %s",
              ep, ipc_error ? ipc_error->message : "unknown error");
    g_warning("grotto: continuing with D-Bus interface only");
  } else {
    g_message("grotto: daemon fully initialized and ready");
  }
}

static void on_name_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data) {
  (void)connection; (void)user_data; (void)name;
  g_message("grotto: name acquired %s", ORG_NOSTR_SIGNER_BUS);
  /* Only the process that owns the signer name runs the bunker, so two
   * daemons never answer the same NIP-46 relays. */
  /* Remote startup failure must not stop the local signer. Keep a failed
   * service in ERROR until shutdown, rather than resetting it to STOPPED. */
  if (!g_bunker_started) {
    g_bunker_started = TRUE;
    grotto_control_start_bunker(g_control);
  }
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
  
  /* Exit nonzero: a daemon that cannot own the name must not linger
   * nameless, and the unit's StartLimitBurst stops a restart flap. */
  g_exit_status = 1;
  if (!connection) {
    g_critical("NO_BUS: could not connect to the D-Bus bus to own %s", ORG_NOSTR_SIGNER_BUS);
  } else {
    g_critical("NAME_TAKEN: %s is owned by another process (another signer daemon is running); exiting",
               ORG_NOSTR_SIGNER_BUS);
  }
  
  if (connection && obj_reg_id) {
    g_message("grotto: unexporting D-Bus interface");
    signer_unexport(connection, obj_reg_id);
    obj_reg_id = 0;
  }
  g_clear_pointer(&g_control, grotto_control_free);
  
  // Stop IPC server
  if (ipc_srv) {
    g_message("grotto: stopping IPC server");
    gnostr_ipc_server_stop(ipc_srv);
    ipc_srv = NULL;
  }
  
  if (loop) {
    g_message("grotto: initiating shutdown");
    g_main_loop_quit(loop);
  }
}

/* nostrc-4n1v: shutdown work must not run inside a POSIX signal handler —
 * g_mutex_lock() deadlocks if the signal interrupts on_name_lost() (which
 * holds g_shutdown_mutex), and g_message()/gnostr_ipc_server_stop() allocate
 * memory and join threads, none of which is async-signal-safe. The body is
 * shared; only the delivery mechanism differs per platform. */
static void do_shutdown(int sig) {
  const char *sig_name = (sig == SIGINT) ? "SIGINT" : (sig == SIGTERM) ? "SIGTERM" : "SIGNAL";

  g_mutex_lock(&g_shutdown_mutex);
  if (g_shutdown_requested) {
    g_mutex_unlock(&g_shutdown_mutex);
    g_message("grotto: shutdown already in progress, ignoring %s", sig_name);
    return;
  }
  g_shutdown_requested = TRUE;
  g_mutex_unlock(&g_shutdown_mutex);

  g_message("grotto: received %s, initiating graceful shutdown", sig_name);

  // Stop IPC early to unblock accept loop
  if (ipc_srv) {
    g_message("grotto: stopping IPC server");
    gnostr_ipc_server_stop(ipc_srv);
    ipc_srv = NULL;
  }

  if (loop) {
    g_main_loop_quit(loop);
  }
}

#ifdef G_OS_UNIX
/* g_unix_signal_add() defers the handler to the main context, where the
 * whole shutdown body is legal. */
static gboolean on_shutdown_signal(gpointer user_data) {
  do_shutdown(GPOINTER_TO_INT(user_data));
  return G_SOURCE_REMOVE;
}
#else
/* Windows: the CRT invokes signal handlers on their own thread where the
 * calls above are permitted; keep the legacy direct path there. */
static void handle_sig(int sig) {
  do_shutdown(sig);
}
#endif

static void print_usage(const char *prog_name) {
  g_print("Usage: %s [OPTIONS]\n", prog_name);
  g_print("\n");
  g_print("Nostr Signer Daemon - Secure key management and signing service\n");
  g_print("\n");
  g_print("Options:\n");
  g_print("  -h, --help              Show this help message\n");
  g_print("  -v, --version           Show version information\n");
  g_print("  --system                Use system bus instead of session bus (requires\n");
  g_print("                          NOSTR_SIGNER_ALLOW_SYSTEM_BUS=1; see source notes)\n");
  g_print("  --bunker                Enable legacy-configured NIP-46 if bunker.ini is absent.\n");
  g_print("                          Persisted grotto/bunker.ini desired state is authoritative;\n");
  g_print("                          explicit relays and transport custody are required.\n");
  g_print("  --no-approval-ui        Existing scoped grants only; review-required requests\n");
  g_print("                          fail closed without summoning the approval UI\n");
  g_print("\n");
  g_print("Environment Variables:\n");
  g_print("  NOSTR_SIGNER_ENDPOINT   IPC endpoint (unix:/path, tcp:host:port)\n");
  g_print("  NOSTR_SIGNER_MAX_CONNECTIONS  Maximum concurrent TCP connections (default: 100)\n");
  g_print("  NOSTR_DEBUG             Enable debug logging\n");
  g_print("\n");
}

#ifdef __linux__
static dev_t self_exe_dev;
static ino_t self_exe_ino;
static gchar *self_exe_path;

static gboolean
retire_if_replaced(gpointer data)
{
  (void)data;
  GStatBuf st;
  gboolean replaced = g_stat(self_exe_path, &st) != 0 ||
                      st.st_dev != self_exe_dev || st.st_ino != self_exe_ino;
  if (!replaced) return G_SOURCE_CONTINUE;
  if (signer_pending_count() > 0) return G_SOURCE_CONTINUE;
  g_message("grotto-daemon: %s was replaced on disk; exiting so the new one serves the next request",
            self_exe_path);
  if (loop) g_main_loop_quit(loop);
  return G_SOURCE_REMOVE;
}

static void
retire_watch_start(void)
{
  g_autofree gchar *exe = g_file_read_link("/proc/self/exe", NULL);
  GStatBuf st;
  if (!exe || g_str_has_suffix(exe, " (deleted)") || g_stat(exe, &st) != 0) return;
  self_exe_path = g_steal_pointer(&exe);
  self_exe_dev = st.st_dev;
  self_exe_ino = st.st_ino;
  g_timeout_add_seconds(30, retire_if_replaced, NULL);
}
#else
static void retire_watch_start(void) {}
#endif

int main(int argc, char **argv) {
  /* W31: carry the pre-rename directories over before anything reads them. */
  grotto_legacy_dirs_migrate();
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
      /* nostrc-nfhf: owning org.nostr.Signer on the system bus exposes
       * signing to every local user, and no D-Bus policy ships with the
       * daemon — require an explicit opt-in. */
      const gchar *allow = g_getenv("NOSTR_SIGNER_ALLOW_SYSTEM_BUS");
      if (!allow || g_strcmp0(allow, "1") != 0) {
        g_printerr("%s: refusing --system: the signer protocol is not policy-\n"
                   "controlled on the system bus (any local user could request\n"
                   "signatures). Set NOSTR_SIGNER_ALLOW_SYSTEM_BUS=1 only if you\n"
                   "installed your own D-Bus policy for org.nostr.Signer.\n",
                   argv[0]);
        return 1;
      }
      bus_type = G_BUS_TYPE_SYSTEM;
    } else if (g_strcmp0(argv[i], "--bunker") == 0) {
      g_bunker_requested = TRUE;
    } else if (g_strcmp0(argv[i], "--no-approval-ui") == 0) {
      g_no_approval_ui = TRUE;
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
#ifdef G_OS_UNIX
  g_unix_signal_add(SIGINT, on_shutdown_signal, GINT_TO_POINTER(SIGINT));
  g_unix_signal_add(SIGTERM, on_shutdown_signal, GINT_TO_POINTER(SIGTERM));
#else
  signal(SIGINT, handle_sig);
  signal(SIGTERM, handle_sig);
#endif
  signal(SIGPIPE, SIG_IGN);  // Ignore broken pipe

  g_message("registering D-Bus name on %s bus",
            bus_type == G_BUS_TYPE_SYSTEM ? "system" : "session");

  loop = g_main_loop_new(NULL, FALSE);

  /* First owner wins: a second daemon fails to acquire the name and exits
   * (NAME_TAKEN) instead of stealing it and triggering a restart war. */
  guint owner_id = g_bus_own_name(bus_type,
                                  ORG_NOSTR_SIGNER_BUS,
                                  G_BUS_NAME_OWNER_FLAGS_NONE,
                                  on_bus_acquired,
                                  on_name_acquired,
                                  on_name_lost,
                                  NULL,
                                  NULL);

  /* A package upgrade replaces this executable but cannot restart a user's
   * bus-activated service: the old daemon would keep answering (an owner
   * saw a fixed bug persist until logout). Retire once the file on disk is
   * no longer the one running and nothing awaits approval; the next call
   * activates the new binary. */
  retire_watch_start();

  g_message("entering main loop");
  g_main_loop_run(loop);

  g_message("main loop exited, cleaning up");

  /* Finish outstanding work while Control1 still records terminal outcomes. */
  if (obj_reg_id && g_dbus_conn) {
    signer_unexport(g_dbus_conn, obj_reg_id);
    obj_reg_id = 0;
  }
  g_clear_pointer(&g_control, grotto_control_free);
  g_clear_pointer(&g_bunker, bunker_service_free);
  g_bus_unown_name(owner_id);

  g_clear_object(&g_dbus_conn);

  g_clear_pointer(&loop, g_main_loop_unref);

  g_mutex_clear(&g_shutdown_mutex);

  g_message("%s shutdown complete (status %d)", DAEMON_NAME, g_exit_status);

  return g_exit_status;
}
