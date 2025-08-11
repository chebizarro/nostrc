#include <gio/gio.h>
#include <signal.h>
#include "nip55l_dbus_names.h"

// Use nip55l GLib implementation. This exports the full signer protocol.
// Prototypes provided by nips/nip55l/src/glib/signer_service_g.c
extern guint signer_export(GDBusConnection *conn, const char *object_path);
extern void  signer_unexport(GDBusConnection *conn, guint reg_id);

// NIP-5F Unix domain socket fallback server
int gnostr_uds_sockd_start(const char *socket_path);
void gnostr_uds_sockd_stop(void);


static GMainLoop *loop = NULL;
static guint obj_reg_id = 0;

static void on_bus_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data) {
  (void)name; (void)user_data;
  obj_reg_id = signer_export(connection, ORG_NOSTR_SIGNER_OBJECT_PATH);
  if (!obj_reg_id) {
    g_critical("DBUS_EXPORT_FAILED: path=%s bus=%s", ORG_NOSTR_SIGNER_OBJECT_PATH, ORG_NOSTR_SIGNER_BUS);
    if (loop) g_main_loop_quit(loop);
  } else {
    g_message("gnostr-signer: exported at %s on %s", ORG_NOSTR_SIGNER_OBJECT_PATH, ORG_NOSTR_SIGNER_BUS);
    // Start UDS fallback listener (NIP-5F)
    const char *sock = g_getenv("NOSTR_SIGNER_SOCK");
    if (gnostr_uds_sockd_start(sock) != 0) {
      g_warning("gnostr-signer: failed to start UDS signer (NIP-5F)");
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
  // Stop UDS fallback
  gnostr_uds_sockd_stop();
  if (loop) g_main_loop_quit(loop);
}

static void handle_sig(int sig) {
  (void)sig;
  // Stop UDS fallback early to unblock accept loop
  gnostr_uds_sockd_stop();
  if (loop) g_main_loop_quit(loop);
}

int main(int argc, char **argv) {
  (void)argc; (void)argv;
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
