#include <gio/gio.h>

#define SIGNER_NAME  "org.nostr.Signer"
#define SIGNER_PATH  "/org/nostr/signer"

/* from signer_service_g.c */
extern guint signer_export(GDBusConnection *conn, const char *object_path);
extern void  signer_unexport(GDBusConnection *conn, guint reg_id);

static GMainLoop *loop = NULL;
static guint obj_reg_id = 0;

static void on_bus_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data) {
  (void)name; (void)user_data;
  obj_reg_id = signer_export(connection, SIGNER_PATH);
  if (obj_reg_id == 0) {
    g_printerr("Failed to export %s on %s\n", SIGNER_PATH, SIGNER_NAME);
    g_main_loop_quit(loop);
  } else {
    g_print("nostr-signer: exported at %s on %s\n", SIGNER_PATH, SIGNER_NAME);
  }
}

static void on_name_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data) {
  (void)connection; (void)name; (void)user_data;
  g_print("nostr-signer: name acquired %s\n", SIGNER_NAME);
}

static void on_name_lost(GDBusConnection *connection, const gchar *name, gpointer user_data) {
  (void)name; (void)user_data;
  if (connection && obj_reg_id) {
    signer_unexport(connection, obj_reg_id);
    obj_reg_id = 0;
  }
  g_printerr("nostr-signer: lost name or could not acquire bus, exiting\n");
  if (loop) g_main_loop_quit(loop);
}

int main(int argc, char **argv){
  (void)argc;(void)argv;
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
