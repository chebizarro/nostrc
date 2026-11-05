/* SPDX-License-Identifier: MIT */
#include "grotto-panels-control1-service.h"
#include "nostrc-test-bus.h"
#include <string.h>

static NostrcTestBus *bus;
static GDBusConnection *server;
static const char *label = "First";
static gboolean authorized, have_request = TRUE, hold_clients, inventory_error;
static guint clients_calls, request_calls, mutations;
static const char *remote_error, *credential_field, *management_role;
static gboolean hold_command;
static GDBusMethodInvocation *pending_command;
static GDBusMethodInvocation *held;
static void capabilities_changed(GrottoPanelsService *service, gpointer data) {
  (void)service; (*(guint *)data)++;
}
static GVariant *rows(gboolean request) {
  GVariantBuilder array, row;
  g_variant_builder_init(&array, G_VARIANT_TYPE("aa{sv}"));
  if (!request || have_request) {
    g_variant_builder_init(&row, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&row, "{sv}", request ? "request_id" : "principal", g_variant_new_string(request ? "request-1" : "app-1"));
    g_variant_builder_add(&row, "{sv}", "label", g_variant_new_string(label));
    g_variant_builder_add(&row, "{sv}", "state", g_variant_new_string("daemon-state"));
    g_variant_builder_add(&row, "{sv}", "reason", g_variant_new_string("daemon-reason"));
    g_variant_builder_add(&row, "{sv}", "access-policy", g_variant_new_string("unknown-preset"));
    g_variant_builder_add(&row, "{sv}", "blocked", g_variant_new_boolean(TRUE));
    g_variant_builder_add(&row, "{sv}", "remaining_seconds", g_variant_new_uint32(100));
    /* Deliberately hostile unrecognized payload. The adapter must discard it. */
    g_variant_builder_add(&row, "{sv}", "private_payload", g_variant_new_string("must-not-cross"));
    g_variant_builder_add(&row, "{sv}", "sequence", g_variant_new_uint64(9));
    if (credential_field) g_variant_builder_add(&row, "{sv}", credential_field, g_variant_new_string("must-not-cross"));
    g_variant_builder_add_value(&array, g_variant_builder_end(&row));
  }
  return g_variant_new("(aa{sv})", &array);
}
static void method(GDBusConnection *connection, const char *sender, const char *path,
  const char *interface, const char *name, GVariant *args, GDBusMethodInvocation *invocation, gpointer data) {
  (void)connection; (void)sender; (void)path; (void)interface; (void)args; (void)data;
  if (!strcmp(name, "GetStatus")) {
    GVariantBuilder status, capabilities, coarse;
    g_variant_builder_init(&status, G_VARIANT_TYPE_VARDICT);
    if (management_role) g_variant_builder_add(&status, "{sv}", "management-role", g_variant_new_string(management_role));
    g_variant_builder_init(&capabilities, G_VARIANT_TYPE("a{sb}"));
    g_variant_builder_add(&capabilities, "{sb}", "set-client-label", authorized);
    g_variant_builder_add(&capabilities, "{sb}", "resume", authorized);
    if (authorized) g_variant_builder_add(&status, "{sv}", "management-capabilities", g_variant_builder_end(&capabilities));
    else g_variant_builder_clear(&capabilities);
    g_variant_builder_init(&coarse, G_VARIANT_TYPE("a{sb}"));
    g_variant_builder_add(&coarse, "{sb}", "clients", TRUE);
    g_variant_builder_add(&coarse, "{sb}", "pause", TRUE);
    g_variant_builder_add(&status, "{sv}", "capabilities", g_variant_builder_end(&coarse));
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(a{sv})", &status));
  } else if (!strcmp(name, "ListIdentities")) {
    if (inventory_error) g_dbus_method_invocation_return_dbus_error(invocation,
      "org.nostr.Grotto.Control1.Error.BackendUnavailable", "Inventory unavailable");
    else g_dbus_method_invocation_return_value(invocation, g_variant_new("(@aa{sv})",
      g_variant_new_array(G_VARIANT_TYPE_VARDICT, NULL, 0)));
  } else if (!strcmp(name, "ListClients")) {
    clients_calls++;
    if (hold_clients) { hold_clients = FALSE; held = g_object_ref(invocation); }
    else g_dbus_method_invocation_return_value(invocation, rows(FALSE));
  } else if (!strcmp(name, "ListRequests")) {
    request_calls++;
    g_dbus_method_invocation_return_value(invocation, rows(TRUE));
  } else if (!strcmp(name, "ListActivity")) {
    g_dbus_method_invocation_return_value(invocation, rows(FALSE));
  } else if (!strcmp(name, "SetClientLabel") || !strcmp(name, "SetPaused")) {
    mutations++;
    if (hold_command) pending_command = g_object_ref(invocation);
    else if (remote_error) g_dbus_method_invocation_return_dbus_error(invocation, remote_error, "token=must-not-appear-in-public-error");
    else g_dbus_method_invocation_return_value(invocation, g_variant_new("()"));
  } else g_dbus_method_invocation_return_dbus_error(invocation, "org.nostr.Test.Unexpected", name);
}
static const GDBusInterfaceVTable vtable = {.method_call = method};
static gboolean expired(gpointer data) { *(gboolean *)data = TRUE; return G_SOURCE_REMOVE; }
#define WAIT(condition) do { \
  gboolean timeout = FALSE; \
  guint timer = g_timeout_add_seconds(5, expired, &timeout); \
  while (!(condition) && !timeout) g_main_context_iteration(NULL, TRUE); \
  g_assert_false(timeout); g_source_remove(timer); \
} while (0)
static void emit(const char *signal) {
  g_autoptr(GError) error = NULL;
  g_assert_true(g_dbus_connection_emit_signal(server, NULL, "/org/nostr/signer/grotto",
    "org.nostr.Grotto.Control1", signal, g_variant_new("(t)", (guint64)1), &error));
  g_assert_no_error(error);
}
static void name_call(const char *method_name) {
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) result = g_dbus_connection_call_sync(server, "org.freedesktop.DBus", "/org/freedesktop/DBus",
    "org.freedesktop.DBus", method_name,
    !strcmp(method_name, "RequestName") ? g_variant_new("(su)", "org.nostr.Signer", 0u) : g_variant_new("(s)", "org.nostr.Signer"),
    G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
  g_assert_no_error(error); g_assert_nonnull(result);
}
static const char *model_label(GListModel *model) {
  if (!g_list_model_get_n_items(model)) return "";
  g_autoptr(GrottoPanelsItem) item = g_list_model_get_item(model, 0);
  return grotto_panels_item_get_metadata(item)->label;
}
static void done(GObject *source, GAsyncResult *result, gpointer data) {
  g_autoptr(GError) error = NULL;
  g_assert_true(grotto_panels_service_invoke_finish(GROTTO_PANELS_SERVICE(source), result, &error));
  g_assert_no_error(error);
  *(gboolean *)data = TRUE;
}
typedef struct { gboolean done, ok; GError *error; } CommandResult;
static void command_done(GObject *source, GAsyncResult *result, gpointer data) {
  CommandResult *r = data;
  r->ok = grotto_panels_service_invoke_finish(GROTTO_PANELS_SERVICE(source), result, &r->error);
  r->done = TRUE;
}
static void exercise(void) {
  server = nostrc_test_bus_connect(bus);
  g_autofree char *xml = NULL;
  g_autoptr(GError) error = NULL;
  g_assert_true(g_file_get_contents(GROTTO_CONTROL_XML_PATH, &xml, NULL, &error));
  g_assert_no_error(error);
  g_autoptr(GDBusNodeInfo) info = g_dbus_node_info_new_for_xml(xml, &error);
  g_assert_no_error(error);
  guint registration = g_dbus_connection_register_object(server, "/org/nostr/signer/grotto",
    info->interfaces[0], &vtable, NULL, NULL, &error);
  g_assert_no_error(error); g_assert_cmpuint(registration, >, 0);
  name_call("RequestName");
  GDBusConnection *client = nostrc_test_bus_connect(bus);
  g_autoptr(GDBusProxy) proxy = g_dbus_proxy_new_sync(client,
    G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES | G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START, NULL,
    "org.nostr.Signer", "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1", NULL, &error);
  g_assert_no_error(error);
  g_autoptr(GrottoPanelsService) service = grotto_panels_control1_service_new(proxy);
  GListModel *apps = grotto_panels_service_get_model(service, GROTTO_PANELS_APPS);
  GListModel *requests = grotto_panels_service_get_model(service, GROTTO_PANELS_REQUESTS);
  WAIT(grotto_panels_service_is_available(service) && g_list_model_get_n_items(apps) == 1 && g_list_model_get_n_items(requests) == 1);
  /* Today's coarse features are not role authorization. */
  for (guint i = 0; i < GROTTO_PANEL_N_COMMANDS; i++) g_assert_false(grotto_panels_service_can(service, i));
  g_autoptr(GrottoPanelsItem) app = g_list_model_get_item(apps, 0);
  const GrottoPanelsMetadata *m = grotto_panels_item_get_metadata(app);
  g_assert_cmpstr(m->state, ==, "daemon-state");
  g_assert_cmpstr(m->reason, ==, "daemon-reason");
  g_assert_cmpstr(m->access_policy, ==, "unknown-preset"); g_assert_true(m->blocked);
  g_autoptr(GrottoPanelsItem) request = g_list_model_get_item(requests, 0);
  gint64 deadline = grotto_panels_item_get_metadata(request)->deadline_usec;
  guint before = request_calls;
  emit("RequestsChanged"); WAIT(request_calls > before);
  /* An ordered status signal provides a callback barrier after the snapshot. */
  authorized = TRUE; emit("StatusChanged");
  WAIT(grotto_panels_service_can(service, GROTTO_PANEL_SET_CLIENT_LABEL));
  g_clear_object(&request); request = g_list_model_get_item(requests, 0);
  g_assert_cmpint(grotto_panels_item_get_metadata(request)->deadline_usec, <=, deadline);
  gboolean completed = FALSE;
  const GrottoPanelCommandArgs args = {.principal = "app-1", .label = "Renamed"};
  grotto_panels_service_invoke_async(service, GROTTO_PANEL_SET_CLIENT_LABEL, &args, NULL, done, &completed);
  WAIT(completed); g_assert_cmpuint(mutations, ==, 1);
  const char *names[] = {"org.nostr.Grotto.Error.Conflict", "org.nostr.Grotto.Error.NotAuthorized",
    "org.nostr.Grotto.Error.NotSupported", "org.nostr.Grotto.Error.InvalidInput", "org.nostr.Grotto.Error.Internal"};
  const GIOErrorEnum codes[] = {G_IO_ERROR_BUSY, G_IO_ERROR_PERMISSION_DENIED,
    G_IO_ERROR_NOT_SUPPORTED, G_IO_ERROR_INVALID_ARGUMENT, G_IO_ERROR_FAILED};
  for (guint i = 0; i < G_N_ELEMENTS(names); i++) {
    remote_error = names[i];
    CommandResult r = {0};
    grotto_panels_service_invoke_async(service, GROTTO_PANEL_SET_CLIENT_LABEL, &args, NULL, command_done, &r);
    WAIT(r.done); g_assert_false(r.ok); g_assert_error(r.error, G_IO_ERROR, (gint)codes[i]);
    g_assert_null(strstr(r.error->message, "token=")); g_clear_error(&r.error);
  }
  remote_error = NULL; hold_command = TRUE;
  CommandResult cancelled_command = {0};
  g_autoptr(GCancellable) cancel = g_cancellable_new();
  grotto_panels_service_invoke_async(service, GROTTO_PANEL_SET_CLIENT_LABEL, &args, cancel, command_done, &cancelled_command);
  WAIT(pending_command != NULL); g_cancellable_cancel(cancel); WAIT(cancelled_command.done);
  g_assert_error(cancelled_command.error, G_IO_ERROR, G_IO_ERROR_CANCELLED); g_clear_error(&cancelled_command.error);
  g_dbus_method_invocation_return_value(pending_command, g_variant_new("()")); g_clear_object(&pending_command);
  hold_command = FALSE;
  hold_clients = TRUE; emit("ClientsChanged"); WAIT(held != NULL);
  label = "New snapshot"; emit("ClientsChanged"); WAIT(!strcmp(model_label(apps), label));
  /* Returning the earlier reply cannot overwrite the newer signal snapshot. */
  label = "Stale"; g_dbus_method_invocation_return_value(held, rows(FALSE)); g_clear_object(&held);
  authorized = FALSE; emit("StatusChanged"); WAIT(!grotto_panels_service_can(service, GROTTO_PANEL_SET_CLIENT_LABEL));
  g_assert_cmpstr(model_label(apps), ==, "New snapshot");
  have_request = FALSE; emit("RequestsChanged"); WAIT(g_list_model_get_n_items(requests) == 0);
  have_request = TRUE; before = request_calls; emit("RequestsChanged"); WAIT(request_calls > before);
  authorized = TRUE; emit("StatusChanged"); WAIT(grotto_panels_service_can(service, GROTTO_PANEL_SET_CLIENT_LABEL));
  g_assert_cmpuint(g_list_model_get_n_items(requests), ==, 0); /* tombstoned */
  hold_clients = TRUE; emit("ClientsChanged"); WAIT(held != NULL);
  hold_command = TRUE; CommandResult old_owner = {0};
  grotto_panels_service_invoke_async(service, GROTTO_PANEL_SET_CLIENT_LABEL, &args, NULL, command_done, &old_owner);
  WAIT(pending_command != NULL);
  name_call("ReleaseName"); WAIT(!grotto_panels_service_is_available(service));
  g_assert_cmpuint(g_list_model_get_n_items(apps), ==, 0);
  g_dbus_method_invocation_return_value(pending_command, g_variant_new("()")); g_clear_object(&pending_command);
  WAIT(old_owner.done); g_assert_error(old_owner.error, G_IO_ERROR, G_IO_ERROR_CANCELLED); g_clear_error(&old_owner.error);
  hold_command = FALSE;
  label = "Restarted"; name_call("RequestName");
  WAIT(grotto_panels_service_is_available(service) && !strcmp(model_label(apps), label));
  label = "Old owner"; g_dbus_method_invocation_return_value(held, rows(FALSE)); g_clear_object(&held);
  authorized = FALSE; emit("StatusChanged"); WAIT(!grotto_panels_service_can(service, GROTTO_PANEL_SET_CLIENT_LABEL));
  g_assert_cmpstr(model_label(apps), ==, "Restarted");
  g_assert_cmpuint(g_list_model_get_n_items(grotto_panels_service_get_model(service, GROTTO_PANELS_IDENTITIES)), ==, 0);
  GListModel *activity = grotto_panels_service_get_model(service, GROTTO_PANELS_ACTIVITY);
  WAIT(g_list_model_get_n_items(activity) == 1);
  g_autoptr(GrottoPanelsItem) record = g_list_model_get_item(activity, 0);
  g_assert_cmpuint(grotto_panels_item_get_metadata(record)->sequence, ==, 9);
  const char *credential_fields[] = {"nsec", "mnemonic", "token", "pairing_token"};
  for (guint i = 0; i < G_N_ELEMENTS(credential_fields); i++) {
    credential_field = credential_fields[i];
    emit("ClientsChanged"); emit("RequestsChanged"); emit("ActivityChanged");
    WAIT(g_list_model_get_n_items(apps) == 0 && g_list_model_get_n_items(activity) == 0 && g_list_model_get_n_items(requests) == 0);
    credential_field = NULL;
    emit("ClientsChanged"); emit("ActivityChanged");
    WAIT(g_list_model_get_n_items(apps) == 1 && g_list_model_get_n_items(activity) == 1);
  }
  /* Settings' manager adapter fails closed on legacy/misbound role metadata;
   * even an over-advertised resume capability cannot reach the host. */
  g_autoptr(GrottoPanelsService) manager = grotto_panels_control1_service_new_manager(proxy);
  guint changes = 0;
  g_signal_connect(manager, "capabilities-changed", G_CALLBACK(capabilities_changed), &changes);
  WAIT(changes > 0);
  g_assert_false(grotto_panels_service_is_available(manager));
  authorized = TRUE; management_role = "approver"; changes = 0; emit("StatusChanged"); WAIT(changes > 0);
  g_assert_false(grotto_panels_service_is_available(manager));
  g_assert_false(grotto_panels_service_can(manager, GROTTO_PANEL_SET_CLIENT_LABEL));
  management_role = "manager"; emit("StatusChanged");
  WAIT(grotto_panels_service_is_available(manager));
  g_assert_true(grotto_panels_service_can(manager, GROTTO_PANEL_SET_CLIENT_LABEL));
  g_assert_false(grotto_panels_service_can(manager, GROTTO_PANEL_RESUME));
  inventory_error = TRUE; emit("StatusChanged"); WAIT(!grotto_panels_service_is_available(manager));
  changes = 0; emit("StatusChanged"); WAIT(changes > 0);
  g_assert_false(grotto_panels_service_is_available(manager));
  g_assert_false(grotto_panels_service_can(manager, GROTTO_PANEL_SET_CLIENT_LABEL));
  inventory_error = FALSE; emit("StatusChanged"); WAIT(grotto_panels_service_is_available(manager));
  management_role = NULL; emit("StatusChanged"); WAIT(!grotto_panels_service_is_available(manager));
  gpointer weak_manager = manager;
  g_object_add_weak_pointer(G_OBJECT(manager), &weak_manager);
  g_clear_object(&manager); WAIT(weak_manager == NULL);
  gpointer weak_service = service, weak_proxy = proxy;
  g_object_add_weak_pointer(G_OBJECT(service), &weak_service);
  g_object_add_weak_pointer(G_OBJECT(proxy), &weak_proxy);
  g_clear_object(&service); WAIT(weak_service == NULL);
  g_clear_object(&proxy); WAIT(weak_proxy == NULL);
  g_dbus_connection_unregister_object(server, registration);
}
int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NOT_SESSION);
  nostrc_test_bus_up(bus);
  nostrc_test_bus_add_func("/grotto-panels/control1", exercise);
  int result = g_test_run();
  nostrc_test_bus_down(bus);
  return result;
}
