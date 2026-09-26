/* test_identity — GetRelays JSON handling and signer calls against a mock
 * org.nostr.Signer on a private bus. SPDX-License-Identifier: MIT */
#include "nss-identity.h"

#include <string.h>

static void
test_parse_relays(void)
{
  GError *e = NULL;
  g_auto(GStrv) a = nss_signer_parse_relays_json(
    "[\"wss://Relay.One/\",{\"url\":\"wss://two.example\",\"read\":true},"
    "\"https://not-relay\",\"wss://relay.one/\",42]", &e);
  g_assert_no_error(e);
  const gchar *want[] = { "wss://relay.one/", "wss://two.example", NULL };
  g_assert_cmpstrv(a, want);
  g_assert_null(nss_signer_parse_relays_json("{}", &e));
  g_assert_nonnull(e);
  g_clear_error(&e);
}

static gboolean locked;

static void
signer_call(GDBusConnection *c, const gchar *sender, const gchar *path, const gchar *iface,
            const gchar *method, GVariant *params, GDBusMethodInvocation *inv, gpointer d)
{
  (void)c; (void)sender; (void)path; (void)iface; (void)params; (void)d;
  if (g_str_equal(method, "GetPublicKey")) {
    if (locked)
      g_dbus_method_invocation_return_dbus_error(inv, "org.nostr.Signer.Error.Locked", "locked");
    else
      g_dbus_method_invocation_return_value(inv, g_variant_new("(s)",
        "npub10elfcs4fr0l0r8af98jlmgdh9c8tcxjvz9qkw038js35mp4dma8qzvjptg"));
  } else if (g_str_equal(method, "GetRelays")) {
    if (locked)
      g_dbus_method_invocation_return_dbus_error(inv, "org.nostr.Signer.Error.NotFound", "none");
    else
      g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", "[\"wss://s.example\"]"));
  }
}

static const GDBusInterfaceVTable vt = { signer_call, NULL, NULL, { 0 } };

typedef struct {
  GDBusConnection *bus;
  gchar *npub;
  gchar **relays;
  GError *e1, *e2;
  gint done;
} Job;

static gpointer
job(gpointer d)
{
  Job *j = d;
  j->npub = nss_signer_get_npub(j->bus, &j->e1);
  j->relays = nss_signer_get_relays(j->bus, &j->e2);
  g_atomic_int_set(&j->done, 1);
  g_main_context_wakeup(NULL);
  return NULL;
}

static void
run(Job *j)
{
  GThread *t = g_thread_new("j", job, j);
  while (!g_atomic_int_get(&j->done))
    g_main_context_iteration(NULL, TRUE);
  g_thread_join(t);
}

static void
test_signer(void)
{
  g_autoptr(GTestDBus) tb = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_test_dbus_up(tb);
  GError *e = NULL;
  g_autoptr(GDBusNodeInfo) node = g_dbus_node_info_new_for_xml(
    "<node><interface name='org.nostr.Signer'>"
    "<method name='GetPublicKey'><arg type='s' direction='out'/></method>"
    "<method name='GetRelays'><arg type='s' direction='out'/></method>"
    "</interface></node>", &e);
  g_assert_no_error(e);
  g_autoptr(GDBusConnection) srv = g_dbus_connection_new_for_address_sync(
    g_test_dbus_get_bus_address(tb),
    G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
    NULL, NULL, &e);
  g_assert_no_error(e);
  g_dbus_connection_register_object(srv, "/org/nostr/signer", node->interfaces[0], &vt, NULL,
                                    NULL, &e);
  g_assert_no_error(e);
  g_autoptr(GVariant) r = g_dbus_connection_call_sync(srv, "org.freedesktop.DBus",
    "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
    g_variant_new("(su)", "org.nostr.Signer", 4u), G_VARIANT_TYPE("(u)"),
    G_DBUS_CALL_FLAGS_NONE, -1, NULL, &e);
  g_assert_no_error(e);
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &e);
  g_assert_no_error(e);

  Job ok = { .bus = bus };
  run(&ok);
  g_assert_no_error(ok.e1);
  g_assert_no_error(ok.e2);
  g_assert_true(g_str_has_prefix(ok.npub, "npub1"));
  g_assert_cmpuint(g_strv_length(ok.relays), ==, 1);
  g_free(ok.npub);
  g_strfreev(ok.relays);

  locked = TRUE;
  Job lk = { .bus = bus };
  run(&lk);
  g_assert_null(lk.npub);
  g_assert_nonnull(lk.e1);
  g_assert_null(strstr(lk.e1->message, "GDBus.Error"));  /* stripped for display */
  g_clear_error(&lk.e1);
  g_assert_no_error(lk.e2);                              /* NotFound → empty */
  g_assert_cmpuint(g_strv_length(lk.relays), ==, 0);
  g_strfreev(lk.relays);

  /* Keyring listing never fails hard (no Secret Service on the test bus). */
  g_autoptr(GPtrArray) ids = nss_keyring_identities(NULL, NULL);
  g_assert_nonnull(ids);

  g_object_unref(bus);
  g_dbus_connection_close_sync(srv, NULL, NULL);
  g_test_dbus_down(tb);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/nostr-settings/identity/parse-relays", test_parse_relays);
  g_test_add_func("/nostr-settings/identity/signer", test_signer);
  return g_test_run();
}
