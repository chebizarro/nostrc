/**
 * mock_signer.c - Test mock for org.nostr.Signer D-Bus interface.
 *
 * Implements the subset of the real signer interface used by nostr-homed:
 *   GetPublicKey()        → (npub)
 *   SignEvent(sss)        → (signature)
 *   NIP44Decrypt(sss)     → (plaintext)
 *
 * Uses a deterministic test identity. NOT for production use.
 * The introspection XML matches the real signer element-for-element
 * so that gdbus-introspection comparisons pass.
 */
#include <gio/gio.h>
#include <string.h>
#include <stdio.h>

static const char *BUS_NAME = "org.nostr.Signer";
static const char *OBJ_PATH = "/org/nostr/signer";

/* Deterministic test identity — DO NOT use real keys in tests. */
#define TEST_NPUB "npub1testmocksigneraaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"

static const gchar introspection_xml[] =
  "<node>"
  "  <interface name='org.nostr.Signer'>"
  "    <method name='GetPublicKey'>"
  "      <arg type='s' name='npub' direction='out'/>"
  "    </method>"
  "    <method name='SignEvent'>"
  "      <arg type='s' name='event_json' direction='in'/>"
  "      <arg type='s' name='current_user' direction='in'/>"
  "      <arg type='s' name='app_id' direction='in'/>"
  "      <arg type='s' name='signature' direction='out'/>"
  "    </method>"
  "    <method name='NIP44Encrypt'>"
  "      <arg type='s' name='plaintext' direction='in'/>"
  "      <arg type='s' name='peer_pubkey' direction='in'/>"
  "      <arg type='s' name='current_user' direction='in'/>"
  "      <arg type='s' name='ciphertext' direction='out'/>"
  "    </method>"
  "    <method name='NIP44Decrypt'>"
  "      <arg type='s' name='ciphertext' direction='in'/>"
  "      <arg type='s' name='peer_pubkey' direction='in'/>"
  "      <arg type='s' name='current_user' direction='in'/>"
  "      <arg type='s' name='plaintext' direction='out'/>"
  "    </method>"
  "  </interface>"
  "</node>";

static GDBusNodeInfo *introspection;

/* Deterministic fake signature — 128 hex chars (64 bytes).
 * Real signer produces a Schnorr signature; mock returns a constant
 * that passes the length check in pam_nip46_challenge. */
static const char FAKE_SIG[] =
  "deadbeef" "deadbeef" "deadbeef" "deadbeef"
  "deadbeef" "deadbeef" "deadbeef" "deadbeef"
  "deadbeef" "deadbeef" "deadbeef" "deadbeef"
  "deadbeef" "deadbeef" "deadbeef" "deadbeef";

static void handle_method_call(GDBusConnection *connection,
                               const gchar *sender,
                               const gchar *object_path,
                               const gchar *interface_name,
                               const gchar *method_name,
                               GVariant *parameters,
                               GDBusMethodInvocation *invocation,
                               gpointer user_data){
  (void)connection; (void)sender; (void)object_path;
  (void)interface_name; (void)user_data;

  if (g_strcmp0(method_name, "GetPublicKey") == 0){
    g_dbus_method_invocation_return_value(invocation,
      g_variant_new("(s)", TEST_NPUB));

  } else if (g_strcmp0(method_name, "SignEvent") == 0){
    const char *event_json = NULL, *current_user = NULL, *app_id = NULL;
    g_variant_get(parameters, "(&s&s&s)", &event_json, &current_user, &app_id);
    fprintf(stderr, "mock_signer: SignEvent called by app_id=%s\n",
            app_id ? app_id : "(null)");
    /* Accept all signing requests — return deterministic fake sig */
    g_dbus_method_invocation_return_value(invocation,
      g_variant_new("(s)", FAKE_SIG));

  } else if (g_strcmp0(method_name, "NIP44Decrypt") == 0){
    const char *ct = NULL, *peer = NULL, *user = NULL;
    g_variant_get(parameters, "(&s&s&s)", &ct, &peer, &user);
    /* Simple test decryption: prefix with "decrypted:" */
    gchar *pt = g_strdup_printf("decrypted:%s", ct ? ct : "");
    g_dbus_method_invocation_return_value(invocation,
      g_variant_new("(s)", pt));
    g_free(pt);

  } else if (g_strcmp0(method_name, "NIP44Encrypt") == 0){
    const char *pt = NULL, *peer = NULL, *user = NULL;
    g_variant_get(parameters, "(&s&s&s)", &pt, &peer, &user);
    /* Simple test encryption: prefix with "encrypted:" */
    gchar *ct = g_strdup_printf("encrypted:%s", pt ? pt : "");
    g_dbus_method_invocation_return_value(invocation,
      g_variant_new("(s)", ct));
    g_free(ct);

  } else {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.freedesktop.DBus.Error.UnknownMethod", "Unknown method");
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
  if (!bus){
    fprintf(stderr, "mock_signer: failed to get bus: %s\n",
            err ? err->message : "error");
    return 1;
  }
  guint owner = g_bus_own_name_on_connection(bus, BUS_NAME,
    G_BUS_NAME_OWNER_FLAGS_NONE, NULL, NULL, NULL, NULL);
  introspection = g_dbus_node_info_new_for_xml(introspection_xml, &err);
  if (!introspection){
    fprintf(stderr, "mock_signer: introspection error: %s\n",
            err ? err->message : "error");
    return 1;
  }
  guint reg = g_dbus_connection_register_object(bus, OBJ_PATH,
    introspection->interfaces[0], &vtable, NULL, NULL, &err);
  if (reg == 0){
    fprintf(stderr, "mock_signer: register object failed: %s\n",
            err ? err->message : "error");
    return 1;
  }
  fprintf(stderr, "mock_signer: running on %s %s\n", BUS_NAME, OBJ_PATH);
  g_main_loop_run(loop);
  g_dbus_connection_unregister_object(bus, reg);
  g_bus_unown_name(owner);
  g_object_unref(bus);
  g_main_loop_unref(loop);
  g_dbus_node_info_unref(introspection);
  return 0;
}
