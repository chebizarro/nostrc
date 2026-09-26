/* test_relay_stats — org.nostr.SessionRelay1 client against a mock that
 * exports the interface from the shipped XML on a private bus; the
 * "never auto-start" rule; presentation strings. The real daemon is
 * covered by apps/relayd/tests/test_session_relay_dbus.c.
 * SPDX-License-Identifier: MIT */
#include "nss-relay-stats.h"

#include <glib/gstdio.h>
#include <string.h>

static GVariant *mock_stats;

/* g_format_size() uses U+00A0 between number and unit on newer GLib. */
static gchar *
nbsp(gchar *s)
{
  GString *g = g_string_new_take(s);
  g_string_replace(g, "\u00a0", " ", 0);
  return g_string_free(g, FALSE);
}

static void
mock_call(GDBusConnection *c, const gchar *sender, const gchar *path, const gchar *iface,
          const gchar *method, GVariant *params, GDBusMethodInvocation *inv, gpointer d)
{
  (void)c; (void)sender; (void)path; (void)iface; (void)params; (void)d;
  g_assert_cmpstr(method, ==, "GetStats");
  g_dbus_method_invocation_return_value(inv, g_variant_new("(@a{sv})", mock_stats));
}

static const GDBusInterfaceVTable vt = { mock_call, NULL, NULL, { 0 } };

static GVariant *
build_stats(const gchar *backend, gint64 events)
{
  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&b, "{sv}", "event_count", g_variant_new_int64(events));
  g_variant_builder_add(&b, "{sv}", "storage_bytes", g_variant_new_uint64(12u * 1000 * 1000));
  g_variant_builder_add(&b, "{sv}", "connected_clients", g_variant_new_uint32(3));
  g_variant_builder_add(&b, "{sv}", "uptime", g_variant_new_uint64(3 * 3600 + 12 * 60 + 5));
  g_variant_builder_add(&b, "{sv}", "storage_backend", g_variant_new_string(backend));
  g_variant_builder_add(&b, "{sv}", "requested_storage_backend", g_variant_new_string("nostrdb"));
  g_variant_builder_add(&b, "{sv}", "storage_path", g_variant_new_string("/tmp/x"));
  const guint32 nips[] = { 1, 11, 42 };
  g_variant_builder_add(&b, "{sv}", "supported_nips",
    g_variant_new_fixed_array(G_VARIANT_TYPE_UINT32, nips, 3, sizeof(guint32)));
  g_variant_builder_add(&b, "{sv}", "retention_supported", g_variant_new_boolean(FALSE));
  g_variant_builder_add(&b, "{sv}", "connections_total", g_variant_new_uint64(17));
  g_variant_builder_add(&b, "{sv}", "future_key", g_variant_new_string("ignored"));
  return g_variant_ref_sink(g_variant_builder_end(&b));
}

static void
on_done(GObject *o, GAsyncResult *rr, gpointer d)
{
  (void)o;
  *(GAsyncResult **)d = g_object_ref(rr);
}

static void
test_client(void)
{
  g_autofree gchar *svcdir = g_dir_make_tmp("nss-svc-XXXXXX", NULL);
  g_autofree gchar *marker = g_build_filename(svcdir, "activated", NULL);
  g_autofree gchar *svc = g_build_filename(svcdir, "org.nostr.SessionRelay1.service", NULL);
  g_autofree gchar *svc_body = g_strdup_printf(
    "[D-BUS Service]\nName=org.nostr.SessionRelay1\nExec=/bin/sh -c 'touch %s'\n", marker);
  g_assert_true(g_file_set_contents(svc, svc_body, -1, NULL));

  g_autoptr(GTestDBus) bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_test_dbus_add_service_dir(bus, svcdir);
  g_test_dbus_up(bus);
  g_autoptr(GDBusConnection) client = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
  g_assert_nonnull(client);

  /* Not running: error, and the activatable name was NOT started. */
  NssRelayStats s = { 0 };
  GError *e = NULL;
  g_assert_false(nss_relay_stats_fetch(client, &s, &e));
  g_assert_true(g_error_matches(e, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN) ||
                g_error_matches(e, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER));
  g_clear_error(&e);
  g_usleep(300 * 1000);
  g_assert_false(g_file_test(marker, G_FILE_TEST_EXISTS));

  /* Mock server exporting the shipped interface. */
  g_autofree gchar *xml = NULL;
  g_assert_true(g_file_get_contents(NSS_RELAY_XML, &xml, NULL, NULL));
  g_autoptr(GDBusNodeInfo) node = g_dbus_node_info_new_for_xml(xml, &e);
  g_assert_no_error(e);
  g_autoptr(GDBusConnection) server = g_dbus_connection_new_for_address_sync(
    g_test_dbus_get_bus_address(bus),
    G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
    NULL, NULL, &e);
  g_assert_no_error(e);
  guint reg = g_dbus_connection_register_object(server, NSS_RELAY_OBJ_PATH,
    g_dbus_node_info_lookup_interface(node, NSS_RELAY_IFACE), &vt, NULL, NULL, &e);
  g_assert_no_error(e);
  g_autoptr(GVariant) r = g_dbus_connection_call_sync(server, "org.freedesktop.DBus",
    "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
    g_variant_new("(su)", NSS_RELAY_BUS_NAME, 4u), G_VARIANT_TYPE("(u)"),
    G_DBUS_CALL_FLAGS_NONE, -1, NULL, &e);
  g_assert_no_error(e);

  mock_stats = build_stats("nostrdb", 3456);
  /* The server connection is serviced by GDBus' worker thread; replies are
   * dispatched from this (default) context, so the sync client call must
   * not block it: use the async variant and iterate. */
  GAsyncResult *res = NULL;
  nss_relay_stats_fetch_async(client, NULL, on_done, &res);
  while (res == NULL)
    g_main_context_iteration(NULL, TRUE);
  g_assert_true(nss_relay_stats_fetch_finish(res, &s, &e));
  g_object_unref(res);
  g_assert_no_error(e);
  g_assert_cmpint(s.event_count, ==, 3456);
  g_assert_cmpuint(s.connected_clients, ==, 3);
  g_assert_cmpuint(s.connections_total, ==, 17);
  g_assert_cmpuint(s.supported_nips->len, ==, 3);
  g_assert_true(nss_relay_stats_has_storage(&s));
  g_autofree gchar *title = nss_format_storage_title(&s);
  g_assert_cmpstr(title, ==, "nostrdb");
  g_autofree gchar *detail = nbsp(nss_format_storage_detail(&s));
  g_assert_cmpstr(detail, ==, "12.0 MB on disk · 3,456 events");
  g_autofree gchar *up = nss_format_uptime(s.uptime);
  g_assert_cmpstr(up, ==, "3 h 12 min");
  g_autofree gchar *nips = nss_format_nips(s.supported_nips);
  g_assert_cmpstr(nips, ==, "1, 11, 42");
  nss_relay_stats_clear(&s);
  g_variant_unref(mock_stats);

  g_dbus_connection_unregister_object(server, reg);
  g_dbus_connection_close_sync(server, NULL, NULL);
  g_clear_object(&client);  /* g_test_dbus_down() waits for the shared bus */
  g_test_dbus_down(bus);
  g_autofree gchar *cmd = g_strdup_printf("rm -rf '%s'", svcdir);
  g_assert_cmpint(system(cmd), ==, 0);
}

static void
test_presentation(void)
{
  NssRelayStats s = { 0 };
  g_autoptr(GVariant) none = build_stats("none", 0);
  nss_relay_stats_from_variant(&s, none);
  g_assert_false(nss_relay_stats_has_storage(&s));
  g_autofree gchar *t = nss_format_storage_title(&s);
  g_assert_cmpstr(t, ==, "No storage — this relay keeps nothing (nostrdb is not available)");
  g_autofree gchar *d = nbsp(nss_format_storage_detail(&s));
  g_assert_cmpstr(d, ==, "12.0 MB on disk");         /* no event count at all */
  nss_relay_stats_clear(&s);

  g_autoptr(GVariant) unk = build_stats("nostrdb", -1);
  nss_relay_stats_from_variant(&s, unk);
  g_autofree gchar *d2 = nbsp(nss_format_storage_detail(&s));
  g_assert_cmpstr(d2, ==, "12.0 MB on disk · event count unavailable");
  nss_relay_stats_clear(&s);

  /* Missing keys → neutral defaults (never a fake count). */
  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
  g_autoptr(GVariant) empty = g_variant_ref_sink(g_variant_builder_end(&b));
  nss_relay_stats_from_variant(&s, empty);
  g_assert_cmpint(s.event_count, ==, -1);
  g_assert_cmpstr(s.storage_backend, ==, "none");
  nss_relay_stats_clear(&s);

  g_autofree gchar *u1 = nss_format_uptime(45);
  g_assert_cmpstr(u1, ==, "45 s");
  g_autofree gchar *u2 = nss_format_uptime(2 * 86400 + 4 * 3600 + 5);
  g_assert_cmpstr(u2, ==, "2 d 4 h");
  g_autofree gchar *u3 = nss_format_uptime(125);
  g_assert_cmpstr(u3, ==, "2 min");

  g_autofree gchar *dir = g_dir_make_tmp("nss-du-XXXXXX", NULL);
  g_autofree gchar *f = g_build_filename(dir, "blob", NULL);
  gchar buf[20000];
  memset(buf, 1, sizeof buf);
  g_assert_true(g_file_set_contents(f, buf, sizeof buf, NULL));
  g_assert_cmpuint(nss_disk_usage(dir), >=, sizeof buf);
  g_assert_cmpuint(nss_disk_usage("/nonexistent/nss"), ==, 0);
  g_autofree gchar *cmd = g_strdup_printf("rm -rf '%s'", dir);
  g_assert_cmpint(system(cmd), ==, 0);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/nostr-settings/relay-stats/client", test_client);
  g_test_add_func("/nostr-settings/relay-stats/presentation", test_presentation);
  return g_test_run();
}
