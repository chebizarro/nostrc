#include <gio/gio.h>
#include <signal.h>
#include <sys/resource.h>
#include "nip55l_dbus_names.h"
#include "ipc.h"

// Use nip55l GLib implementation. This exports the full signer protocol.
// Prototypes provided by nips/nip55l/src/glib/signer_service_g.c
extern guint signer_export(GDBusConnection *conn, const char *object_path);
extern void  signer_unexport(GDBusConnection *conn, guint reg_id);

// IPC abstraction (POSIX UDS / TCP / Windows named pipe) from ipc.h


static GMainLoop *loop = NULL;
static guint obj_reg_id = 0;

static GnostrIpcServer *ipc_srv = NULL;

static void on_bus_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data) {
  (void)name; (void)user_data;
  obj_reg_id = signer_export(connection, ORG_NOSTR_SIGNER_OBJECT_PATH);
  if (!obj_reg_id) {
    g_critical("DBUS_EXPORT_FAILED: path=%s bus=%s", ORG_NOSTR_SIGNER_OBJECT_PATH, ORG_NOSTR_SIGNER_BUS);
    if (loop) g_main_loop_quit(loop);
  } else {
    g_message("gnostr-signer: exported at %s on %s", ORG_NOSTR_SIGNER_OBJECT_PATH, ORG_NOSTR_SIGNER_BUS);
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
    }
  }
}

static void on_name_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data) {
  (void)connection; (void)user_data; (void)name;
  g_message("gnostr-signer: name acquired %s", ORG_NOSTR_SIGNER_BUS);
}

static void on_name_lost(GDBusConnection *connection, const gchar *name, gpointer user_data) {
  (void)name; (void)user_data;
  if (connection && obj_reg_id) {
    signer_unexport(connection, obj_reg_id);
    obj_reg_id = 0;
  }
  g_warning("gnostr-signer: lost name or could not acquire bus, exiting");
  // Stop IPC server
  if (ipc_srv) { gnostr_ipc_server_stop(ipc_srv); ipc_srv = NULL; }
  if (loop) g_main_loop_quit(loop);
}

static void handle_sig(int sig) {
  (void)sig;
  // Stop IPC early to unblock accept loop
  if (ipc_srv) { gnostr_ipc_server_stop(ipc_srv); ipc_srv = NULL; }
  if (loop) g_main_loop_quit(loop);
}

int main(int argc, char **argv) {
  (void)argc; (void)argv;
  // Disable core dumps when handling secrets
  struct rlimit rl;
  rl.rlim_cur = 0; rl.rlim_max = 0;
  setrlimit(RLIMIT_CORE, &rl);
  signal(SIGINT, handle_sig);
  signal(SIGTERM, handle_sig);
  loop = g_main_loop_new(NULL, FALSE);
  guint owner_id = g_bus_own_name(G_BUS_TYPE_SESSION,
                                  ORG_NOSTR_SIGNER_BUS,
                                  G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT | G_BUS_NAME_OWNER_FLAGS_REPLACE,
                                  on_bus_acquired,
                                  on_name_acquired,
                                  on_name_lost,
                                  NULL,
                                  NULL);
  g_main_loop_run(loop);
  g_bus_unown_name(owner_id);
  g_main_loop_unref(loop);
  return 0;
}
