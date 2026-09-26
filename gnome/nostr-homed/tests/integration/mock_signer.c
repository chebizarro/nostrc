/**
 * mock_signer.c - Test mock for org.nostr.Signer D-Bus interface.
 *
 * Implements the subset of the real signer interface used by nostr-homed:
 *   GetPublicKey()        → (npub)
 *   SignEvent(sss)        → (signed_event)  complete signed event JSON
 *   NIP44Encrypt(sss)     → (ciphertext)    marker transform, not NIP-44
 *   NIP44Decrypt(sss)     → (plaintext)     marker transform, not NIP-44
 *
 * SignEvent follows the nip55l 0.2.0 contract: it signs the template for
 * real (BIP-340 Schnorr, canonical NIP-01 id) with a fixed test key and
 * returns the whole event, so consumers exercise the same verification
 * they apply to the real signer. GetPublicKey returns that key's npub.
 * NOT for production use: the key below is public.
 */
#include <gio/gio.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "nostr-event.h"
#include "json.h"
#include "keys.h"
#include "nostr-utils.h"
#include "nostr/nip19/nip19.h"

static const char *BUS_NAME = "org.nostr.Signer";
static const char *OBJ_PATH = "/org/nostr/signer";

/* Fixed, publicly known test identity. DO NOT use real keys in tests. */
#define MOCK_SECKEY_HEX "7f4c11a9742721d66e40e321ca50b682c27f7422190c14a187525e69e604836a"

static char *mock_npub = NULL;

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
  "      <arg type='s' name='signed_event' direction='out'/>"
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

static char *derive_npub(void){
  char *pk_hex = nostr_key_get_public(MOCK_SECKEY_HEX);
  if (!pk_hex) return NULL;
  uint8_t pk[32];
  char *npub = NULL;
  if (nostr_hex2bin(pk, pk_hex, sizeof pk)) (void)nostr_nip19_encode_npub(pk, &npub);
  free(pk_hex);
  return npub;
}

/* Same semantics as nostr_nip55l_sign_event_json: pubkey is the signing
 * key's, a zero created_at becomes now. NULL if the template is not an
 * event. */
static char *sign_template(const char *event_json){
  NostrEvent *ev = nostr_event_new();
  if (!ev) return NULL;
  char *out = NULL;
  if (event_json && nostr_event_deserialize(ev, event_json) == 0) {
    if (nostr_event_get_created_at(ev) == 0) nostr_event_set_created_at(ev, (int64_t)time(NULL));
    if (nostr_event_sign(ev, MOCK_SECKEY_HEX) == 0) out = nostr_event_serialize(ev);
  }
  nostr_event_free(ev);
  return out;
}

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
      g_variant_new("(s)", mock_npub));

  } else if (g_strcmp0(method_name, "SignEvent") == 0){
    const char *event_json = NULL, *current_user = NULL, *app_id = NULL;
    g_variant_get(parameters, "(&s&s&s)", &event_json, &current_user, &app_id);
    fprintf(stderr, "mock_signer: SignEvent called by app_id=%s\n",
            app_id ? app_id : "(null)");
    char *signed_json = sign_template(event_json);
    if (!signed_json) {
      g_dbus_method_invocation_return_dbus_error(invocation,
        "org.nostr.Signer.Error.InvalidInput", "event JSON is not a valid Nostr event");
      return;
    }
    g_dbus_method_invocation_return_value(invocation,
      g_variant_new("(s)", signed_json));
    free(signed_json);

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
  mock_npub = derive_npub();
  if (!mock_npub){
    fprintf(stderr, "mock_signer: failed to derive test npub\n");
    return 1;
  }
  GMainLoop *loop = g_main_loop_new(NULL, FALSE);
  GError *err = NULL;
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (!bus){
    fprintf(stderr, "mock_signer: failed to get bus: %s\n",
            err ? err->message : "error");
    return 1;
  }
  introspection = g_dbus_node_info_new_for_xml(introspection_xml, &err);
  if (!introspection){
    fprintf(stderr, "mock_signer: introspection error: %s\n",
            err ? err->message : "error");
    return 1;
  }
  /* Register before owning the name so a client that sees the name appear
   * can call immediately. */
  guint reg = g_dbus_connection_register_object(bus, OBJ_PATH,
    introspection->interfaces[0], &vtable, NULL, NULL, &err);
  if (reg == 0){
    fprintf(stderr, "mock_signer: register object failed: %s\n",
            err ? err->message : "error");
    return 1;
  }
  guint owner = g_bus_own_name_on_connection(bus, BUS_NAME,
    G_BUS_NAME_OWNER_FLAGS_NONE, NULL, NULL, NULL, NULL);
  fprintf(stderr, "mock_signer: running on %s %s as %s\n", BUS_NAME, OBJ_PATH, mock_npub);
  g_main_loop_run(loop);
  g_dbus_connection_unregister_object(bus, reg);
  g_bus_unown_name(owner);
  g_object_unref(bus);
  g_main_loop_unref(loop);
  g_dbus_node_info_unref(introspection);
  free(mock_npub);
  return 0;
}
