/* SPDX-License-Identifier: MIT */
/* External SDK consumer and minimal Control1 fixture, on a private session bus. */
#include <grotto-panels-control1-service.h>
#include <string.h>

static guint status_calls, identity_calls, mutations;
static gboolean timed_out, invoked;

static void method(GDBusConnection *bus, const char *sender, const char *path,
  const char *interface, const char *name, GVariant *args,
  GDBusMethodInvocation *call, gpointer data) {
  (void)bus; (void)sender; (void)path; (void)interface; (void)args; (void)data;
  if (!strcmp(name, "GetStatus")) {
    status_calls++;
    g_dbus_method_invocation_return_value(call, g_variant_new_parsed(
      "({'management-role': <'manager'>, 'management-capabilities': <{'set-client-label': true}>},)"));
  } else if (!strcmp(name, "ListIdentities")) {
    identity_calls++;
    g_dbus_method_invocation_return_value(call, g_variant_new_parsed(
      "([{'identity': <'public-test-identity'>, 'can-sign': <true>, 'state': <'unknown'>}],)"));
  } else if (!strcmp(name, "ListClients") || !strcmp(name, "ListActivity")) {
    g_dbus_method_invocation_return_value(call, g_variant_new("(@aa{sv})",
      g_variant_new_array(G_VARIANT_TYPE_VARDICT, NULL, 0)));
  } else if (!strcmp(name, "SetClientLabel")) {
    mutations++;
    /* Advertised client capability is never daemon authorization. */
    g_dbus_method_invocation_return_dbus_error(call,
      "org.nostr.Grotto.Control1.Error.NotAuthorized", "Fixture denies mutation");
  } else {
    g_error("Unexpected fixture method: %s", name);
  }
}

static gboolean deadline(gpointer data) {
  (void)data;
  timed_out = TRUE;
  return G_SOURCE_REMOVE;
}

static void command_done(GObject *source, GAsyncResult *result, gpointer data) {
  (void)data;
  g_autoptr(GError) error = NULL;
  g_assert_false(grotto_panels_service_invoke_finish(GROTTO_PANELS_SERVICE(source), result, &error));
  g_assert_nonnull(error);
  invoked = TRUE;
}

int main(int argc, char **argv) {
  /* Keep unsuppressed transport validation separate from native GTK cache lifetime. */
  g_assert_true(argc == 2 || (argc == 3 && !strcmp(argv[2], "--host-identity")));
  gboolean host_identity = argc == 3;
  if (host_identity) adw_init();
  g_autoptr(GError) error = NULL;
  g_autofree char *xml = NULL;
  g_assert_true(g_file_get_contents(argv[1], &xml, NULL, &error));
  g_assert_no_error(error);
  g_autoptr(GDBusNodeInfo) node = g_dbus_node_info_new_for_xml(xml, &error);
  g_assert_no_error(error);
  g_autoptr(GDBusConnection) server = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
  g_assert_no_error(error);
  const GDBusInterfaceVTable vtable = {.method_call = method};
  guint registration = g_dbus_connection_register_object(server, "/org/nostr/signer/grotto",
    node->interfaces[0], &vtable, NULL, NULL, &error);
  g_assert_no_error(error);
  g_assert_cmpuint(registration, >, 0);
  g_autoptr(GVariant) name = g_dbus_connection_call_sync(server, "org.freedesktop.DBus",
    "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
    g_variant_new("(su)", "org.nostr.Signer", 4u), G_VARIANT_TYPE("(u)"),
    G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
  g_assert_no_error(error);
  guint result;
  g_variant_get(name, "(u)", &result);
  g_assert_cmpuint(result, ==, 1); /* Name acquisition is the readiness barrier. */
  g_autoptr(GDBusConnection) client = g_dbus_connection_new_for_address_sync(
    g_getenv("DBUS_SESSION_BUS_ADDRESS"), G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
    G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION, NULL, NULL, &error);
  g_assert_no_error(error);
  g_autoptr(GDBusProxy) proxy = g_dbus_proxy_new_sync(client,
    G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES | G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
    NULL, "org.nostr.Signer", "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1", NULL, &error);
  g_assert_no_error(error);
  GrottoPanelsService *service = grotto_panels_control1_service_new_manager(proxy);
  guint watchdog = g_timeout_add_seconds(10, deadline, NULL);
  GListModel *identities = grotto_panels_service_get_model(service, GROTTO_PANELS_IDENTITIES);
  while ((!grotto_panels_service_is_available(service) || g_list_model_get_n_items(identities) != 1) && !timed_out)
    g_main_context_iteration(NULL, TRUE);
  g_assert_false(timed_out);
  g_assert_cmpuint(status_calls, >, 0);
  g_assert_cmpuint(identity_calls, >, 0);
  g_autoptr(GrottoPanelsItem) item = g_list_model_get_item(identities, 0);
  g_assert_cmpstr(grotto_panels_item_get_metadata(item)->public_key, ==, "public-test-identity");
  g_assert_false(grotto_panels_service_can(service, GROTTO_PANEL_SET_PERMISSIONS));
  g_assert_false(grotto_panels_service_can(service, GROTTO_PANEL_RESUME));
  GtkWidget *window = NULL;
  if (host_identity) {
    window = adw_window_new();
    g_object_ref_sink(window);
    GtkWidget *panel = grotto_identity_panel_new(service, NULL);
    adw_window_set_content(ADW_WINDOW(window), panel);
    gtk_window_present(GTK_WINDOW(window));
    while (!gtk_widget_get_mapped(panel) && !timed_out) g_main_context_iteration(NULL, TRUE);
    g_assert_false(timed_out);
  }
  g_assert_true(grotto_panels_service_can(service, GROTTO_PANEL_SET_CLIENT_LABEL));
  GrottoPanelCommandArgs args = {.principal = "app:test", .label = "Test label"};
  grotto_panels_service_invoke_async(service, GROTTO_PANEL_SET_CLIENT_LABEL, &args, NULL, command_done, NULL);
  while (!invoked && !timed_out) g_main_context_iteration(NULL, TRUE);
  g_assert_false(timed_out);
  g_assert_cmpuint(mutations, ==, 1);
  g_source_remove(watchdog);
  if (window) {
    gtk_window_destroy(GTK_WINDOW(window));
    g_object_unref(window);
  }
  /* Drain the adapter's async snapshot references before closing its connection. */
  gpointer weak = service;
  g_object_add_weak_pointer(G_OBJECT(service), &weak);
  g_object_unref(service);
  while (weak) g_main_context_iteration(NULL, TRUE);
  g_clear_object(&proxy);
  g_dbus_connection_unregister_object(server, registration);
  g_assert_true(g_dbus_connection_close_sync(client, NULL, &error));
  g_assert_no_error(error);
  g_assert_true(g_dbus_connection_close_sync(server, NULL, &error));
  g_assert_no_error(error);
  g_print("PASS: installed pkg-config client, Control1 snapshots, daemon denial%s\n",
    host_identity ? ", Identity panel" : "");
  return 0;
}
