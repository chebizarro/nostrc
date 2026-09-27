/*
 * test_nwc_wallet_agent.c — GnostrNwcService as a client of
 * org.nostr.Wallet1 (nostrc-prqu.13).
 *
 * A fake wallet agent on a private GTestDBus bus. Checks that the service
 * migrates a legacy plaintext nwc-connection-uri into the agent with Pair()
 * and resets the key (success, already paired, denied), keeps it when the
 * agent is missing (retry next start), never writes it, and forwards
 * balance / payments / invoices to the agent.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "util/nwc.h"

#include <gio/gio.h>
#include <string.h>

#define PK_W "b889ff5b1513b641e2a139f661a661364979c5beee91842f8f0ef42ab558e9d4"
#define SECRET "71a8c14c1407c113601079c4302dab36460f0ccd0ad506f1f2dc73b5100e4f3c"
#define URI_1 "nostr+walletconnect://" PK_W "?relay=wss%3A%2F%2Frelay.example&secret=" SECRET "&lud16=me%40example.com"
#define URI_2 "nostr+walletconnect://" PK_W "?relay=wss%3A%2F%2Fother.example&secret=" SECRET

/* ---- fake org.nostr.Wallet1 ---------------------------------------------- */

typedef enum { PAIR_ACCEPT, PAIR_DENY } PairMode;

static struct {
  GDBusConnection *conn;
  GDBusNodeInfo *node;
  guint reg, own;
  gboolean paired;
  PairMode mode;
  guint pair_calls;
  char *last_pair_uri;
  char *last_bolt11;
  guint32 last_amount;
} fake;

static const char wallet_xml[] =
  "<node><interface name='org.nostr.Wallet1'>"
  " <method name='GetBalance'><arg name='b' type='t' direction='out'/></method>"
  " <method name='MakeInvoice'><arg type='u' direction='in'/><arg type='s' direction='in'/>"
  "  <arg type='u' direction='in'/><arg type='s' direction='out'/><arg type='s' direction='out'/></method>"
  " <method name='PayInvoice'><arg type='s' direction='in'/><arg type='u' direction='in'/>"
  "  <arg type='s' direction='out'/><arg type='t' direction='out'/></method>"
  " <method name='Pair'><arg type='s' direction='in'/></method>"
  " <method name='Unpair'/>"
  " <property name='Paired' type='b' access='read'/>"
  " <property name='WalletPubkey' type='s' access='read'/>"
  " <property name='Lud16' type='s' access='read'/>"
  " <property name='Relays' type='as' access='read'/>"
  "</interface></node>";

static GVariant *
fake_prop(const char *name)
{
  if (!strcmp(name, "Paired")) return g_variant_new_boolean(fake.paired);
  if (!strcmp(name, "WalletPubkey")) return g_variant_new_string(fake.paired ? PK_W : "");
  if (!strcmp(name, "Lud16")) return g_variant_new_string(fake.paired ? "me@example.com" : "");
  const char *relays[] = { "wss://relay.example", NULL };
  return g_variant_new_strv(fake.paired ? relays : NULL, fake.paired ? 1 : 0);
}

static void
fake_set_paired(gboolean paired)
{
  fake.paired = paired;
  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
  const char *names[] = { "Paired", "WalletPubkey", "Lud16", "Relays" };
  for (gsize i = 0; i < G_N_ELEMENTS(names); i++)
    g_variant_builder_add(&b, "{sv}", names[i], fake_prop(names[i]));
  g_dbus_connection_emit_signal(fake.conn, NULL, "/org/nostr/Wallet1",
                                "org.freedesktop.DBus.Properties", "PropertiesChanged",
                                g_variant_new("(sa{sv}as)", "org.nostr.Wallet1", &b, NULL), NULL);
}

static GVariant *
fake_get_property(GDBusConnection *c, const char *s, const char *p, const char *i,
                  const char *name, GError **e, gpointer ud)
{
  (void)c; (void)s; (void)p; (void)i; (void)e; (void)ud;
  return fake_prop(name);
}

static void
fake_method(GDBusConnection *c, const char *sender, const char *path, const char *iface,
            const char *method, GVariant *params, GDBusMethodInvocation *inv, gpointer ud)
{
  (void)c; (void)sender; (void)path; (void)iface; (void)ud;
  if (!strcmp(method, "Pair")) {
    fake.pair_calls++;
    g_free(fake.last_pair_uri);
    g_variant_get(params, "(s)", &fake.last_pair_uri);
    if (fake.mode == PAIR_DENY) {
      g_dbus_method_invocation_return_dbus_error(inv, "org.nostr.Wallet1.Error.Denied",
                                                 "The user declined");
      return;
    }
    fake_set_paired(TRUE);
    g_dbus_method_invocation_return_value(inv, NULL);
  } else if (!strcmp(method, "Unpair")) {
    fake_set_paired(FALSE);
    g_dbus_method_invocation_return_value(inv, NULL);
  } else if (!strcmp(method, "GetBalance")) {
    g_dbus_method_invocation_return_value(inv, g_variant_new("(t)", (guint64)21000000));
  } else if (!strcmp(method, "PayInvoice")) {
    g_free(fake.last_bolt11);
    g_variant_get(params, "(su)", &fake.last_bolt11, &fake.last_amount);
    g_dbus_method_invocation_return_value(inv, g_variant_new("(st)", "ab12", (guint64)3000));
  } else if (!strcmp(method, "MakeInvoice")) {
    const char *desc;
    guint32 expiry;
    g_variant_get(params, "(u&su)", &fake.last_amount, &desc, &expiry);
    g_dbus_method_invocation_return_value(inv, g_variant_new("(ss)", "lnbc210n1fake", "cafe"));
  }
}

static const GDBusInterfaceVTable fake_vtable = { fake_method, fake_get_property, NULL, { 0 } };

static void
fake_up(GTestDBus *bus)
{
  g_autoptr(GError) error = NULL;
  fake.conn = g_dbus_connection_new_for_address_sync(g_test_dbus_get_bus_address(bus),
      G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
      NULL, NULL, &error);
  g_assert_no_error(error);
  fake.node = g_dbus_node_info_new_for_xml(wallet_xml, &error);
  g_assert_no_error(error);
  fake.reg = g_dbus_connection_register_object(fake.conn, "/org/nostr/Wallet1",
      fake.node->interfaces[0], &fake_vtable, NULL, NULL, &error);
  g_assert_no_error(error);
  gboolean owned = FALSE;
  fake.own = g_bus_own_name_on_connection(fake.conn, "org.nostr.Wallet1",
                                          G_BUS_NAME_OWNER_FLAGS_NONE, NULL, NULL, NULL, NULL);
  /* Wait until the bus reports the owner. */
  for (int i = 0; i < 500 && !owned; i++) {
    g_main_context_iteration(NULL, FALSE);
    g_autoptr(GVariant) r = g_dbus_connection_call_sync(fake.conn, "org.freedesktop.DBus",
        "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner",
        g_variant_new("(s)", "org.nostr.Wallet1"), G_VARIANT_TYPE("(b)"),
        G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
    if (r) g_variant_get(r, "(b)", &owned);
  }
  g_assert_true(owned);
}

/* ---- helpers ---------------------------------------------------------------- */

static GSettings *
client_settings(void)
{
  return g_settings_new("org.gnostr.Client");
}

static char *
legacy_uri(void)
{
  g_autoptr(GSettings) s = client_settings();
  return g_settings_get_string(s, "nwc-connection-uri");
}

static void
set_legacy_uri(const char *uri)
{
  g_autoptr(GSettings) s = client_settings();
  g_settings_set_string(s, "nwc-connection-uri", uri);
}

static gboolean
wait_state(GnostrNwcService *svc, GnostrNwcState want)
{
  for (int i = 0; i < 2000; i++) {
    if (gnostr_nwc_service_get_state(svc) == want)
      return TRUE;
    g_main_context_iteration(NULL, FALSE);
    g_usleep(1000);
  }
  return FALSE;
}

typedef struct { gboolean done; GAsyncResult *res; } Wait;

static void
on_done(GObject *src, GAsyncResult *res, gpointer ud)
{
  (void)src;
  Wait *w = ud;
  w->res = g_object_ref(res);
  w->done = TRUE;
}

static void
wait_done(Wait *w)
{
  while (!w->done)
    g_main_context_iteration(NULL, TRUE);
}

/* ---- tests ---------------------------------------------------------------------- */

static void
test_wallet_agent_client(void)
{
  g_autoptr(GTestDBus) bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_test_dbus_up(bus);
  g_autoptr(GError) error = NULL;

  /* 1. No agent on the bus: the stored URI is kept for the next start. */
  set_legacy_uri(URI_1);
  GnostrNwcService *svc = g_object_new(GNOSTR_TYPE_NWC_SERVICE, NULL);
  gnostr_nwc_service_start(svc);
  g_assert_true(wait_state(svc, GNOSTR_NWC_STATE_ERROR));
  g_autofree char *kept = legacy_uri();
  g_assert_cmpstr(kept, ==, URI_1);
  g_object_unref(svc);

  fake_up(bus);

  /* 2. Migration: Pair(stored URI), then the key is reset. */
  fake.mode = PAIR_ACCEPT;
  svc = g_object_new(GNOSTR_TYPE_NWC_SERVICE, NULL);
  gnostr_nwc_service_start(svc);
  g_assert_true(wait_state(svc, GNOSTR_NWC_STATE_CONNECTED));
  g_assert_cmpuint(fake.pair_calls, ==, 1);
  g_assert_cmpstr(fake.last_pair_uri, ==, URI_1);
  g_autofree char *after = legacy_uri();
  g_assert_cmpstr(after, ==, "");
  g_assert_true(gnostr_nwc_service_is_connected(svc));
  g_assert_cmpstr(gnostr_nwc_service_get_wallet_pubkey(svc), ==, PK_W);
  g_assert_cmpstr(gnostr_nwc_service_get_relay(svc), ==, "wss://relay.example");
  g_assert_cmpstr(gnostr_nwc_service_get_lud16(svc), ==, "me@example.com");

  /* 3. Requests go to the agent. */
  Wait w = { 0 };
  gint64 balance = 0;
  gnostr_nwc_service_get_balance_async(svc, NULL, on_done, &w);
  wait_done(&w);
  g_assert_true(gnostr_nwc_service_get_balance_finish(svc, w.res, &balance, &error));
  g_assert_no_error(error);
  g_assert_cmpint(balance, ==, 21000000);
  g_clear_object(&w.res);
  w.done = FALSE;

  g_autofree char *preimage = NULL;
  gnostr_nwc_service_pay_invoice_async(svc, "lnbc1invoice", 21000, NULL, on_done, &w);
  wait_done(&w);
  g_assert_true(gnostr_nwc_service_pay_invoice_finish(svc, w.res, &preimage, &error));
  g_assert_no_error(error);
  g_assert_cmpstr(preimage, ==, "ab12");
  g_assert_cmpstr(fake.last_bolt11, ==, "lnbc1invoice");
  g_assert_cmpuint(fake.last_amount, ==, 21000);
  g_clear_object(&w.res);
  w.done = FALSE;

  /* Amounts beyond the interface's u (msat) are refused locally. */
  gnostr_nwc_service_pay_invoice_async(svc, "lnbc1invoice", (gint64)G_MAXUINT32 + 1, NULL, on_done, &w);
  wait_done(&w);
  g_assert_false(gnostr_nwc_service_pay_invoice_finish(svc, w.res, NULL, &error));
  g_assert_error(error, GNOSTR_NWC_ERROR, GNOSTR_NWC_ERROR_REQUEST_FAILED);
  g_clear_error(&error);
  g_clear_object(&w.res);
  w.done = FALSE;

  g_autofree char *bolt11 = NULL, *hash = NULL;
  gnostr_nwc_service_make_invoice_async(svc, 21000, "coffee", 600, NULL, on_done, &w);
  wait_done(&w);
  g_assert_true(gnostr_nwc_service_make_invoice_finish(svc, w.res, &bolt11, &hash, &error));
  g_assert_cmpstr(bolt11, ==, "lnbc210n1fake");
  g_assert_cmpstr(hash, ==, "cafe");
  g_clear_object(&w.res);

  /* 4. connect() never stores the URI; an invalid one is refused up front. */
  guint pairs = fake.pair_calls;
  g_assert_false(gnostr_nwc_service_connect(svc, "nostr+walletconnect://nope", &error));
  g_assert_error(error, GNOSTR_NWC_ERROR, GNOSTR_NWC_ERROR_INVALID_URI);
  g_clear_error(&error);
  g_assert_cmpuint(fake.pair_calls, ==, pairs);
  g_assert_true(gnostr_nwc_service_connect(svc, URI_2, &error));
  g_assert_true(wait_state(svc, GNOSTR_NWC_STATE_CONNECTED));
  g_assert_cmpstr(fake.last_pair_uri, ==, URI_2);
  g_autofree char *still_empty = legacy_uri();
  g_assert_cmpstr(still_empty, ==, "");

  /* Unpair follows the agent's Paired property. */
  gnostr_nwc_service_disconnect(svc);
  g_assert_true(wait_state(svc, GNOSTR_NWC_STATE_DISCONNECTED));
  g_object_unref(svc);

  /* 5. Already paired in the agent: the stale plaintext copy is dropped. */
  fake_set_paired(TRUE);
  set_legacy_uri(URI_1);
  pairs = fake.pair_calls;
  svc = g_object_new(GNOSTR_TYPE_NWC_SERVICE, NULL);
  gnostr_nwc_service_start(svc);
  g_assert_true(wait_state(svc, GNOSTR_NWC_STATE_CONNECTED));
  g_assert_cmpuint(fake.pair_calls, ==, pairs);
  g_autofree char *dropped = legacy_uri();
  g_assert_cmpstr(dropped, ==, "");
  g_object_unref(svc);

  /* 6. The user declines the migration: the secret must not linger. */
  fake.paired = FALSE;
  fake.mode = PAIR_DENY;
  set_legacy_uri(URI_1);
  svc = g_object_new(GNOSTR_TYPE_NWC_SERVICE, NULL);
  g_test_expect_message(NULL, G_LOG_LEVEL_MESSAGE, "*Moving the stored wallet connection*");
  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*did not pair*");
  g_test_expect_message(NULL, G_LOG_LEVEL_MESSAGE, "*Removed the stored wallet connection*");
  gnostr_nwc_service_start(svc);
  g_assert_true(wait_state(svc, GNOSTR_NWC_STATE_ERROR));
  g_test_assert_expected_messages();
  g_autofree char *declined = legacy_uri();
  g_assert_cmpstr(declined, ==, "");
  g_assert_false(gnostr_nwc_service_is_connected(svc));
  g_object_unref(svc);

  g_bus_unown_name(fake.own);
  g_dbus_connection_unregister_object(fake.conn, fake.reg);
  g_clear_object(&fake.conn);
  g_dbus_node_info_unref(fake.node);
  g_free(fake.last_pair_uri);
  g_free(fake.last_bolt11);
  g_test_dbus_down(bus);
}

int
main(int argc, char **argv)
{
  g_setenv("GSETTINGS_BACKEND", "memory", TRUE);
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/nwc/wallet-agent-client", test_wallet_agent_client);
  return g_test_run();
}
