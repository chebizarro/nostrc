#include <gio/gio.h>
#include <string.h>
#include <stdio.h>

static const char *BUS_NAME = "org.nostr.Signer";
static const char *OBJ_PATH = "/org/nostr/Signer";

static const gchar introspection_xml[] =
  "<node>"
  "  <interface name='org.nostr.Signer'>"
  "    <method name='Decrypt'>"
  "      <arg type='s' name='ciphertext' direction='in'/>"
  "      <arg type='s' name='plaintext' direction='out'/>"
  "    </method>"
  "    <method name='Authenticate'>"
  "      <arg type='s' name='user' direction='in'/>"
  "      <arg type='b' name='ok' direction='out'/>"
  "    </method>"
  "  </interface>"
  "</node>";

static GDBusNodeInfo *introspection;

static void handle_method_call(GDBusConnection *connection,
                               const gchar *sender,
                               const gchar *object_path,
                               const gchar *interface_name,
                               const gchar *method_name,
                               GVariant *parameters,
                               GDBusMethodInvocation *invocation,
                               gpointer user_data){
  (void)connection; (void)sender; (void)object_path; (void)interface_name; (void)user_data;
  if (g_strcmp0(method_name, "Decrypt") == 0){
    const char *ct = NULL; g_variant_get(parameters, "(s)", &ct);
    if (!ct) ct = "";
    gchar *pt = g_strdup_printf("decrypted:%s", ct);
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", pt));
    g_free(pt);
  } else if (g_strcmp0(method_name, "Authenticate") == 0){
    const char *user = NULL; g_variant_get(parameters, "(s)", &user);
    gboolean ok = TRUE; /* accept all */
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", ok));
  } else {
    g_dbus_method_invocation_return_dbus_error(invocation, "org.freedesktop.DBus.Error.UnknownMethod", "Unknown method");
  }
}

static const GDBusInterfaceVTable vtable = {
  .method_call = handle_method_call,
};

int main(int argc, char **argv){
  (void)argc; (void)argv;
  GMainLoop *loop = g_main_loop_new(NULL, FALSE);
  GError *err = NULL;
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (!bus){ fprintf(stderr, "mock_signer: failed to get bus: %s\n", err?err->message:"error"); return 1; }
  guint owner = g_bus_own_name_on_connection(bus, BUS_NAME, G_BUS_NAME_OWNER_FLAGS_NONE, NULL, NULL, NULL, NULL);
  introspection = g_dbus_node_info_new_for_xml(introspection_xml, &err);
  if (!introspection){ fprintf(stderr, "mock_signer: introspection error: %s\n", err?err->message:"error"); return 1; }
  guint reg = g_dbus_connection_register_object(bus, OBJ_PATH, introspection->interfaces[0], &vtable, NULL, NULL, &err);
  if (reg == 0){ fprintf(stderr, "mock_signer: register object failed: %s\n", err?err->message:"error"); return 1; }
  g_main_loop_run(loop);
  g_dbus_connection_unregister_object(bus, reg);
  g_bus_unown_name(owner);
  g_object_unref(bus);
  g_main_loop_unref(loop);
  g_dbus_node_info_unref(introspection);
  return 0;
}
