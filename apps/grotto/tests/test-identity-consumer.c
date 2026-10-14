/* A real, unprivileged consumer: receives only an explicit npub and event. */
#include <gio/gio.h>
#include <nostr-event.h>
#include <nostr-json.h>
#include <nostr/nip19/nip19.h>
#include <string.h>
int main(int argc, char **argv) {
  if (argc != 2 || g_getenv("NOSTR_SIGNER_SECKEY_HEX") || g_getenv("NOSTR_SIGNER_NSEC")) return 2;
  GError *error = NULL;
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
  if (!bus) return 3;
  GVariant *reply = g_dbus_connection_call_sync(bus, "org.nostr.Signer", "/org/nostr/signer", "org.nostr.Signer",
      "SignEvent", g_variant_new("(sss)", "{\"kind\":1,\"created_at\":1,\"tags\":[],\"content\":\"unpublished independent consumer test\"}", argv[1], ""),
      G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 10000, NULL, &error);
  if (!reply) { g_printerr("explicit consumer sign failed\n"); return 4; }
  const gchar *json; g_variant_get(reply, "(&s)", &json);
  NostrEvent *event = nostr_event_new();
  gboolean ok = !nostr_event_deserialize(event, json) && nostr_event_check_signature(event);
  unsigned char public_key[32];
  char hex[65];
  if (nostr_nip19_decode_npub(argv[1], public_key)) ok = FALSE;
  else {
    for (guint i = 0; i < 32; i++) g_snprintf(hex + 2*i, 3, "%02x", public_key[i]);
    ok = ok && !strcmp(event->pubkey, hex);
  }
  nostr_event_free(event); g_variant_unref(reply); g_object_unref(bus); g_clear_error(&error);
  return ok ? 0 : 5;
}
