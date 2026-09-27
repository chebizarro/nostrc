#include <gio/gio.h>

#include "nip55l_nip5f.h"

#define SIGNER_NAME  "org.nostr.Signer"
#define SIGNER_PATH  "/org/nostr/signer"

/* from signer_service_g.c */
extern guint signer_export(GDBusConnection *conn, const char *object_path);
extern void  signer_unexport(GDBusConnection *conn, guint reg_id);

static GMainLoop *loop = NULL;
static guint obj_reg_id = 0;
static int exit_status = 0;

static void on_bus_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data) {
  (void)name; (void)user_data;
  obj_reg_id = signer_export(connection, SIGNER_PATH);
  if (obj_reg_id == 0) {
    g_printerr("Failed to export %s on %s\n", SIGNER_PATH, SIGNER_NAME);
    exit_status = 1;
    g_main_loop_quit(loop);
  } else {
    g_print("nostr-signer: exported at %s on %s\n", SIGNER_PATH, SIGNER_NAME);
    /* Opt-in NIP-5F socket, gated like the D-Bus methods (nostrc-q23h). */
    const char *ep = g_getenv("NOSTR_SIGNER_ENDPOINT");
    if (ep && g_str_has_prefix(ep, "unix:") && ep[5]) {
      GError *err = NULL;
      if (!nip55l_nip5f_start(ep + 5, &err)) {
        g_printerr("nostr-signer: NIP-5F socket: %s\n", err ? err->message : "failed");
        g_clear_error(&err);
      }
    } else if (ep && *ep) {
      g_printerr("nostr-signer: NOSTR_SIGNER_ENDPOINT=%s ignored (only unix:<path>)\n", ep);
    }
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
  /* Nonzero: a signer that cannot own the name must not linger nameless. */
  if (connection)
    g_printerr("nostr-signer: NAME_TAKEN: %s is owned by another process, exiting\n", SIGNER_NAME);
  else
    g_printerr("nostr-signer: NO_BUS: could not connect to the session bus, exiting\n");
  exit_status = 1;
  if (loop) g_main_loop_quit(loop);
}

int main(int argc, char **argv){
  (void)argc;(void)argv;
  loop = g_main_loop_new(NULL, FALSE);
  guint owner_id = g_bus_own_name(G_BUS_TYPE_SESSION,
                                  SIGNER_NAME,
                                  G_BUS_NAME_OWNER_FLAGS_NONE, /* first owner wins */
                                  on_bus_acquired,
                                  on_name_acquired,
                                  on_name_lost,
                                  NULL,
                                  NULL);
  g_main_loop_run(loop);
  nip55l_nip5f_stop();
  g_bus_unown_name(owner_id);
  g_main_loop_unref(loop);
  return exit_status;
}
