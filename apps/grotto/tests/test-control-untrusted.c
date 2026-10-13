/* A distinct executable: bus-name ownership must not confer approver trust.
 * Linux-only acceptance (UID-only buses inherit the documented fallback). */
#include <gio/gio.h>
#include <string.h>
#define IFACE "org.nostr.Grotto.Control1"
static void denied(GDBusConnection *bus, const gchar *destination, const gchar *method, GVariant *args) {
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) value = g_dbus_connection_call_sync(bus, destination, "/org/nostr/signer/grotto", IFACE, method, args, NULL, 0, 10000, NULL, &error);
  g_assert_null(value); g_assert_nonnull(error);
  g_autofree gchar *name = g_dbus_error_get_remote_error(error);
  g_assert_cmpstr(name, ==, IFACE ".Error.NotAuthorized");
}
int main(int argc, char **argv) {
  g_assert_cmpint(argc, ==, 2); g_autoptr(GError) error = NULL;
  g_autoptr(GDBusConnection) bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error); g_assert_no_error(error);
  g_autoptr(GVariant) owner = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
      g_variant_new("(su)", "org.nostr.Grotto", 0u), G_VARIANT_TYPE("(u)"), 0, 10000, NULL, &error); g_assert_no_error(error);
  g_autoptr(GVariant) status = g_dbus_connection_call_sync(bus, argv[1], "/org/nostr/signer/grotto", IFACE, "GetStatus", NULL,
      G_VARIANT_TYPE("(a{sv})"), 0, 10000, NULL, &error); g_assert_no_error(error); g_assert_nonnull(status);
  g_autoptr(GVariant) coarse = g_variant_get_child_value(status, 0);
  const gchar *sensitive[] = {"identity", "default_identity", "requests", "clients", "activity", "principal", "preview"};
  for (guint i = 0; i < G_N_ELEMENTS(sensitive); i++) g_assert_null(g_variant_lookup_value(coarse, sensitive[i], NULL));
  denied(bus, argv[1], "ListRequests", NULL); denied(bus, argv[1], "ListClients", NULL);
  denied(bus, argv[1], "ListPermissions", g_variant_new("(s)", "exe:/client"));
  denied(bus, argv[1], "SetPermissions", g_variant_new("(ss@aa{sv}t)", "exe:/client", "*", g_variant_new_array(G_VARIANT_TYPE("a{sv}"), NULL, 0), (guint64)0));
  denied(bus, argv[1], "RevokeClient", g_variant_new("(s)", "exe:/client"));
  denied(bus, argv[1], "SetClientLabel", g_variant_new("(ss)", "exe:/client", "forged"));
  denied(bus, argv[1], "SetClientTrust", g_variant_new("(ss)", "exe:/client", "full"));
  denied(bus, argv[1], "SetDefaultIdentityForLegacy", g_variant_new("(s)", ""));
  denied(bus, argv[1], "ExportIdentity", g_variant_new("(sss)", "", "ncryptsec", "passphrase"));
  denied(bus, argv[1], "ExportRawIdentity", g_variant_new("(s)", "")); denied(bus, argv[1], "RotateIdentity", g_variant_new("(s)", ""));
  denied(bus, argv[1], "ConfigureBunker", g_variant_new("(@a{sv})", g_variant_new_array(G_VARIANT_TYPE("{sv}"), NULL, 0)));
  denied(bus, argv[1], "CreatePairing", g_variant_new("(ss)", "", ""));
  denied(bus, argv[1], "AcceptConnectURI", g_variant_new("(ss)", "invalid", ""));
  denied(bus, argv[1], "CancelPairing", g_variant_new("(s)", "id")); denied(bus, argv[1], "ResetBunkerEndpoint", NULL);
  denied(bus, argv[1], "ListActivity", g_variant_new("(tu)", (guint64)0, (guint)100)); denied(bus, argv[1], "ClearActivity", NULL);
  g_autoptr(GVariant) pause = g_dbus_connection_call_sync(bus, argv[1], "/org/nostr/signer/grotto", IFACE, "SetPaused", g_variant_new("(b)", TRUE), NULL, 0, 10000, NULL, &error);
  g_assert_no_error(error); g_assert_nonnull(pause);
  denied(bus, argv[1], "SetPaused", g_variant_new("(b)", FALSE));
  return 0;
}
