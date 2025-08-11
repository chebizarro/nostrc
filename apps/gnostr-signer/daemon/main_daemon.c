#include <gio/gio.h>
#include <signal.h>

// Use nip55l GLib implementation. This exports the full signer protocol.
// Prototypes provided by nips/nip55l/src/glib/signer_service_g.c
extern guint signer_export(GDBusConnection *conn, const char *object_path);
extern void  signer_unexport(GDBusConnection *conn, guint reg_id);

#define SIGNER_NAME  "org.nostr.Signer"
#define SIGNER_PATH  "/org/nostr/signer"

static GMainLoop *loop = NULL;
static guint obj_reg_id = 0;

static void on_bus_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data) {
  (void)name; (void)user_data;
  obj_reg_id = signer_export(connection, SIGNER_PATH);
  if (!obj_reg_id) {
    g_critical("Failed to export %s on %s", SIGNER_PATH, SIGNER_NAME);
    if (loop) g_main_loop_quit(loop);
  } else {
    g_message("gnostr-signer: exported at %s on %s", SIGNER_PATH, SIGNER_NAME);
  }
}

static void on_name_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data) {
  (void)connection; (void)user_data; (void)name;
  g_message("gnostr-signer: name acquired %s", SIGNER_NAME);
}

static void on_name_lost(GDBusConnection *connection, const gchar *name, gpointer user_data) {
  (void)name; (void)user_data;
  if (connection && obj_reg_id) {
    signer_unexport(connection, obj_reg_id);
    obj_reg_id = 0;
  }
  g_warning("gnostr-signer: lost name or could not acquire bus, exiting");
  if (loop) g_main_loop_quit(loop);
}

static void handle_sig(int sig) {
  (void)sig;
  if (loop) g_main_loop_quit(loop);
}

int main(int argc, char **argv) {
  (void)argc; (void)argv;
  signal(SIGINT, handle_sig);
  signal(SIGTERM, handle_sig);
  loop = g_main_loop_new(NULL, FALSE);
  guint owner_id = g_bus_own_name(G_BUS_TYPE_SESSION,
                                  SIGNER_NAME,
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
