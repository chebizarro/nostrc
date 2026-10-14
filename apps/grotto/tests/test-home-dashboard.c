#include "home-model.h"
#include "tests/common/nostrc-test-bus.h"
#include <string.h>
static GDBusConnection *server;
static guint signed_count, pending_count, client_count, version = 1, calls, changes;
static gboolean fail_clients, hold_clients;
static GDBusMethodInvocation *held;
static gboolean bound(gpointer data) { (void)data; g_error("Home model did not reach its event-driven barrier"); return G_SOURCE_REMOVE; }
#define WAIT(condition) do { guint timer = g_timeout_add_seconds(10, bound, NULL); while (!(condition)) g_main_context_iteration(NULL, TRUE); g_source_remove(timer); } while (0)
static GVariant *clients(void) {
  GVariantBuilder b; g_variant_builder_init(&b, G_VARIANT_TYPE("aa{sv}"));
  for (guint i = 0; i < client_count; i++) {
    GVariantBuilder row; g_variant_builder_init(&row, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&row, "{sv}", "principal", g_variant_new_string("browser:https://app.example"));
    g_variant_builder_add_value(&b, g_variant_builder_end(&row));
  }
  return g_variant_builder_end(&b);
}
static void call(GDBusConnection *bus, const gchar *sender, const gchar *path, const gchar *interface, const gchar *method, GVariant *args, GDBusMethodInvocation *inv, gpointer data) {
  (void)bus; (void)sender; (void)path; (void)interface; (void)args; (void)data; calls++;
  if (!strcmp(method, "GetStatus")) {
    GVariantBuilder b; g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
#define ADD(key,value) g_variant_builder_add(&b, "{sv}", key, g_variant_new_uint32(value))
    ADD("control_version", version); ADD("signed_today", signed_count); ADD("client_count", client_count); ADD("pending_count", pending_count);
#undef ADD
    g_dbus_method_invocation_return_value(inv, g_variant_new("(@a{sv})", g_variant_builder_end(&b)));
  } else if (!strcmp(method, "ListClients")) {
    if (hold_clients) { held = g_object_ref(inv); return; }
    if (fail_clients) { g_dbus_method_invocation_return_dbus_error(inv, "org.nostr.Grotto.Control1.Error.NotReady", "unavailable"); return; }
    g_dbus_method_invocation_return_value(inv, g_variant_new("(@aa{sv})", clients()));
  } else {
    GVariantBuilder b; g_variant_builder_init(&b, G_VARIANT_TYPE("aa{sv}"));
    guint count = !strcmp(method, "ListRequests") ? pending_count : signed_count;
    for (guint i = 0; i < count; i++) {
      GVariantBuilder row; g_variant_builder_init(&row, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add(&row, "{sv}", "principal", g_variant_new_string("browser:https://app.example"));
      g_variant_builder_add_value(&b, g_variant_builder_end(&row));
    }
    g_dbus_method_invocation_return_value(inv, g_variant_new("(@aa{sv})", g_variant_builder_end(&b)));
  }
}
static void changed(GrottoHomeModel *model, gpointer data) { (void)model; (void)data; changes++; }
static void emit_change(const gchar *name) { g_dbus_connection_emit_signal(server, NULL, "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1", name, g_variant_new("(t)", (guint64)calls), NULL); }
static void name(const gchar *method) {
  g_autoptr(GVariant) reply = g_dbus_connection_call_sync(server, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", method,
      !strcmp(method, "RequestName") ? g_variant_new("(su)", "org.nostr.Signer", 0u) : g_variant_new("(s)", "org.nostr.Signer"), NULL, 0, 5000, NULL, NULL); g_assert_nonnull(reply);
}
static void dashboard(void) {
  g_autoptr(GrottoHomeModel) model = grotto_home_model_new(); g_signal_connect(model, "changed", G_CALLBACK(changed), NULL);
  signed_count = 7; client_count = 2; pending_count = 1; grotto_home_model_start(model); WAIT(grotto_home_model_writable(model));
  guint n = 0; g_assert_true(g_variant_lookup(grotto_home_model_status(model), "signed_today", "u", &n)); g_assert_cmpuint(n, ==, 7);
  g_assert_cmpuint(g_variant_n_children(grotto_home_model_clients(model)), ==, 2);
  g_assert_cmpuint(g_variant_n_children(grotto_home_model_requests(model)), ==, 1);
  const gchar *signals[] = {"RequestsChanged", "ActivityChanged", "ClientsChanged", "StatusChanged"};
  for (guint i = 0; i < G_N_ELEMENTS(signals); i++) {
    guint before = changes; signed_count++; pending_count = i % 2; client_count = i + 1; emit_change(signals[i]); WAIT(changes > before && grotto_home_model_writable(model));
    g_assert_true(g_variant_lookup(grotto_home_model_status(model), "signed_today", "u", &n)); g_assert_cmpuint(n, ==, signed_count);
    g_assert_cmpuint(g_variant_n_children(grotto_home_model_requests(model)), ==, pending_count);
    g_assert_cmpuint(g_variant_n_children(grotto_home_model_clients(model)), ==, client_count);
  }
  hold_clients = TRUE; grotto_home_model_refresh(model); WAIT(held != NULL);
  name("ReleaseName"); WAIT(grotto_home_model_status(model) == NULL);
  g_assert_null(grotto_home_model_clients(model)); g_assert_false(grotto_home_model_writable(model));
  hold_clients = FALSE; name("RequestName"); signed_count = 42;
  g_dbus_method_invocation_return_value(held, g_variant_new("(@aa{sv})", clients())); g_clear_object(&held);
  WAIT(grotto_home_model_writable(model)); g_assert_true(g_variant_lookup(grotto_home_model_status(model), "signed_today", "u", &n)); g_assert_cmpuint(n, ==, 42);
  version = 99; guint before = changes; emit_change("StatusChanged"); WAIT(changes > before && grotto_home_model_status(model)); g_assert_false(grotto_home_model_writable(model));
  version = 1; fail_clients = TRUE; before = changes; emit_change("ClientsChanged"); WAIT(changes > before && grotto_home_model_error(model));
  g_assert_null(grotto_home_model_status(model)); g_assert_null(grotto_home_model_clients(model));
  fail_clients = FALSE; grotto_home_model_refresh(model); WAIT(grotto_home_model_writable(model));
  grotto_home_model_stop(model); g_assert_null(grotto_home_model_status(model)); g_assert_null(grotto_home_model_proxy(model));
}
int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL); nostrc_test_bus_tolerate_ebadf(); NostrcTestBus *bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NONE); nostrc_test_bus_up(bus); server = nostrc_test_bus_connect(bus);
  g_autoptr(GDBusNodeInfo) node = g_dbus_node_info_new_for_xml("<node><interface name='org.nostr.Grotto.Control1'><method name='GetStatus'><arg type='a{sv}' direction='out'/></method><method name='ListClients'><arg type='aa{sv}' direction='out'/></method><method name='ListRequests'><arg type='aa{sv}' direction='out'/></method><method name='ListActivity'><arg type='t' direction='in'/><arg type='u' direction='in'/><arg type='aa{sv}' direction='out'/></method><signal name='StatusChanged'><arg type='t'/></signal><signal name='RequestsChanged'><arg type='t'/></signal><signal name='ClientsChanged'><arg type='t'/></signal><signal name='ActivityChanged'><arg type='t'/></signal></interface></node>", NULL);
  const GDBusInterfaceVTable vtable = {call, NULL, NULL, {0}};
  guint registration = g_dbus_connection_register_object(server, "/org/nostr/signer/grotto", node->interfaces[0], &vtable, NULL, NULL, NULL); g_assert_cmpuint(registration, >, 0); name("RequestName");
  g_test_add_func("/home/dashboard/daemon-signals-owner-failure", dashboard); int result = g_test_run(); g_dbus_connection_unregister_object(server, registration); nostrc_test_bus_down(bus); return result;
}
