#include "background.h"
#include "nostrc-test-bus.h"
#include <glib/gstdio.h>

static NostrcTestBus *bus;
static GDBusConnection *service, *client;
static guint registration, request_registration, acquired, released, requests, statuses, closes;
static guint version = 2;
static gboolean grant, defer_response, portal_launch, early_response;
static gchar *response_path, *response_peer;
static const char xml[] =
  "<node><interface name='org.freedesktop.portal.Background'>"
  "<property name='version' type='u' access='read'/>"
  "<method name='RequestBackground'><arg type='s' direction='in'/><arg type='a{sv}' direction='in'/><arg type='o' direction='out'/></method>"
  "<method name='SetStatus'><arg type='a{sv}' direction='in'/></method>"
  "</interface></node>";
static const char request_xml[] =
  "<node><interface name='org.freedesktop.portal.Request'><method name='Close'/></interface></node>";
void g08_test_hold(GApplication *app) { acquired++; g_application_hold(app); }
void g08_test_release(GApplication *app) {
  released++; g_assert_cmpuint(released, <=, acquired); g_application_release(app);
}
static gboolean deadline(gpointer data) { *(gboolean *)data = TRUE; return G_SOURCE_REMOVE; }
static void await_state(GrottoBackground *s, GrottoBackgroundState expected) {
  gboolean expired = FALSE;
  guint timeout = g_timeout_add_seconds(5, deadline, &expired);
  while (!expired && grotto_background_get_state(s) != expected) g_main_context_iteration(NULL, TRUE);
  if (!expired) g_source_remove(timeout);
  g_assert_cmpint(grotto_background_get_state(s), ==, expected);
}
static void await_counter(guint *counter, guint expected) {
  gboolean expired = FALSE;
  guint timeout = g_timeout_add_seconds(5, deadline, &expired);
  while (!expired && *counter < expected) g_main_context_iteration(NULL, TRUE);
  if (!expired) g_source_remove(timeout);
  g_assert_cmpuint(*counter, >=, expected);
}
static void send_response(void) {
  GVariantBuilder values; g_variant_builder_init(&values, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&values, "{sv}", "background", g_variant_new_boolean(grant));
  g_variant_builder_add(&values, "{sv}", "autostart", g_variant_new_boolean(portal_launch && grant));
  g_assert_true(g_dbus_connection_emit_signal(service, response_peer, response_path,
      "org.freedesktop.portal.Request", "Response", g_variant_new("(ua{sv})", 0, &values), NULL));
}
static GVariant *property(GDBusConnection *connection, const char *sender, const char *path,
    const char *interface, const char *name, GError **error, gpointer data) {
  (void)connection; (void)sender; (void)path; (void)interface; (void)name; (void)error; (void)data;
  return g_variant_new_uint32(version);
}
static void method(GDBusConnection *connection, const char *sender, const char *path,
    const char *interface, const char *name, GVariant *args, GDBusMethodInvocation *invocation, gpointer data);
static const GDBusInterfaceVTable vtable = { method, property, NULL, { 0 } };
static void method(GDBusConnection *connection, const char *sender, const char *path,
    const char *interface, const char *name, GVariant *args, GDBusMethodInvocation *invocation, gpointer data) {
  (void)connection; (void)path; (void)interface; (void)data;
  if (!strcmp(name, "RequestBackground")) {
    g_autoptr(GVariant) options = NULL; const char *parent, *token;
    g_variant_get(args, "(&s@a{sv})", &parent, &options);
    g_assert_cmpstr(parent, ==, "");
    g_assert_true(g_variant_lookup(options, "handle_token", "&s", &token));
    g_assert_true(g_variant_lookup(options, "autostart", "b", &portal_launch));
    g_autoptr(GVariant) command = g_variant_lookup_value(options, "commandline", G_VARIANT_TYPE_STRING_ARRAY);
    const char *flag; g_variant_get_child(command, 1, "&s", &flag);
    g_assert_cmpstr(flag, ==, "--gapplication-service");
    g_autofree gchar *peer = g_strdup(sender + 1);
    for (gchar *p = peer; *p; p++) if (*p == '.') *p = '_';
    g_free(response_path); g_free(response_peer);
    response_path = g_strdup_printf("/org/freedesktop/portal/desktop/request/%s/%s", peer, token);
    response_peer = g_strdup(sender);
    if (request_registration) g_dbus_connection_unregister_object(service, request_registration);
    g_autoptr(GDBusNodeInfo) info = g_dbus_node_info_new_for_xml(request_xml, NULL);
    request_registration = g_dbus_connection_register_object(service, response_path, info->interfaces[0], &vtable, NULL, NULL, NULL);
    g_assert_cmpuint(request_registration, >, 0);
    requests++;
    if (!defer_response && early_response) send_response();
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(o)", response_path));
    if (!defer_response && !early_response) send_response();
  } else if (!strcmp(name, "SetStatus")) {
    g_assert_cmpuint(version, >=, 2);
    g_autoptr(GVariant) options = NULL; const char *message;
    g_variant_get(args, "(@a{sv})", &options);
    g_assert_true(g_variant_lookup(options, "message", "&s", &message));
    g_assert_true(!strcmp(message, "Ready to review requests") || !strcmp(message, "Requests waiting") || !strcmp(message, "Signing paused"));
    g_assert_cmpuint(strlen(message), <=, 96);
    statuses++; g_dbus_method_invocation_return_value(invocation, NULL);
  } else if (!strcmp(name, "Close")) {
    closes++; g_dbus_method_invocation_return_value(invocation, NULL);
  } else g_assert_not_reached();
}
static void own(gboolean enabled) {
  g_autoptr(GVariant) reply = g_dbus_connection_call_sync(service, "org.freedesktop.DBus",
      "/org/freedesktop/DBus", "org.freedesktop.DBus", enabled ? "RequestName" : "ReleaseName",
      enabled ? g_variant_new("(su)", "org.freedesktop.portal.Desktop", 0) :
                g_variant_new("(s)", "org.freedesktop.portal.Desktop"),
      NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
  g_assert_nonnull(reply);
}
static GSettings *settings(void) {
  GSettings *s = g_settings_new("org.nostr.Grotto");
  const char *keys[] = { "autostart", "start-minimized", "run-in-background", "launch-on-login",
    "pause-on-screen-lock", "settings-migration-version", "background-disclosure-accepted" };
  for (guint i = 0; i < G_N_ELEMENTS(keys); i++) g_settings_reset(s, keys[i]);
  return s;
}
static GApplication *application(void) {
  acquired = released = 0;
  return g_application_new("org.nostr.Grotto.BackgroundTest", G_APPLICATION_NON_UNIQUE);
}
static void test_absent(void) {
  own(FALSE);
  g_autoptr(GSettings) config = settings();
  g_settings_set_boolean(config, "background-disclosure-accepted", TRUE);
  g_autoptr(GApplication) app = application();
  g_autoptr(GrottoBackground) bg = grotto_background_new(app, client, config, TRUE);
  await_state(bg, GROTTO_BACKGROUND_UNAVAILABLE);
  g_assert_cmpuint(grotto_background_get_holds(bg), ==, 1);
  /* G07's independent per-request hold survives background release. */
  g_application_hold(app);
  g_settings_set_boolean(config, "run-in-background", FALSE);
  await_state(bg, GROTTO_BACKGROUND_DISABLED);
  g_assert_cmpuint(grotto_background_get_holds(bg), ==, 0);
  g_application_release(app);
  grotto_background_stop(bg); g_assert_cmpuint(acquired, ==, released);
}
static void changed(GrottoBackground *bg, gpointer data) {
  (void)bg; (*(guint *)data)++;
}
static void test_disclosure(void) {
  own(TRUE); grant = TRUE; defer_response = FALSE; version = 2;
  guint before = requests, changes = 0;
  g_autoptr(GSettings) config = settings();
  g_autoptr(GApplication) app = application();
  g_autoptr(GrottoBackground) bg = grotto_background_new(app, client, config, TRUE);
  g_signal_connect(bg, "state-changed", G_CALLBACK(changed), &changes);
  await_counter(&changes, 1);
  g_assert_cmpuint(requests, ==, before);
  g_assert_cmpuint(grotto_background_get_holds(bg), ==, 0);
  g_assert_cmpint(grotto_background_get_state(bg), ==, GROTTO_BACKGROUND_DISABLED);
  g_settings_set_boolean(config, "background-disclosure-accepted", TRUE);
  await_state(bg, GROTTO_BACKGROUND_REGISTERED);
  g_assert_cmpuint(requests, ==, before + 1);
  g_settings_set_boolean(config, "launch-on-login", TRUE);
  g_settings_set_boolean(config, "run-in-background", FALSE);
  g_assert_false(g_settings_get_boolean(config, "launch-on-login"));
  grotto_background_stop(bg); g_assert_cmpuint(acquired, ==, released);
}
static void test_absent_sandbox(void) {
  own(FALSE);
  g_autoptr(GSettings) config = settings();
  g_settings_set_boolean(config, "background-disclosure-accepted", TRUE);
  g_autoptr(GApplication) app = application();
  g_autoptr(GrottoBackground) bg = grotto_background_new(app, client, config, FALSE);
  await_state(bg, GROTTO_BACKGROUND_UNAVAILABLE);
  g_assert_cmpuint(grotto_background_get_holds(bg), ==, 0);
  grotto_background_stop(bg); g_assert_cmpuint(acquired, ==, released);
}
static void test_denied(void) {
  own(TRUE); grant = FALSE; defer_response = FALSE; version = 2;
  g_autoptr(GSettings) config = settings();
  g_settings_set_boolean(config, "background-disclosure-accepted", TRUE);
  g_autoptr(GApplication) app = application();
  g_autoptr(GrottoBackground) bg = grotto_background_new(app, client, config, TRUE);
  await_state(bg, GROTTO_BACKGROUND_DENIED);
  g_assert_cmpuint(grotto_background_get_holds(bg), ==, 0);
  grotto_background_stop(bg); g_assert_cmpuint(acquired, ==, released);
}
static void test_granted(void) {
  own(TRUE); grant = TRUE; defer_response = FALSE; version = 2;
  early_response = TRUE; /* Wire-order Response before the method reply. */
  guint before = statuses;
  g_autoptr(GSettings) config = settings();
  g_settings_set_boolean(config, "background-disclosure-accepted", TRUE);
  g_settings_set_boolean(config, "launch-on-login", TRUE);
  g_autoptr(GApplication) app = application();
  g_autoptr(GrottoBackground) bg = grotto_background_new(app, client, config, TRUE);
  await_state(bg, GROTTO_BACKGROUND_REGISTERED);
  g_assert_false(portal_launch);
  await_counter(&statuses, before + 1);
  guint request_count = requests;
  g_settings_set_boolean(config, "launch-on-login", FALSE);
  g_assert_cmpuint(requests, ==, request_count);
  g_assert_cmpint(grotto_background_get_state(bg), ==, GROTTO_BACKGROUND_REGISTERED);
  grotto_background_set_status(bg, TRUE, TRUE);
  await_counter(&statuses, before + 2);
  own(FALSE); await_state(bg, GROTTO_BACKGROUND_UNAVAILABLE);
  own(TRUE); await_state(bg, GROTTO_BACKGROUND_REGISTERED);
  grotto_background_stop(bg); grotto_background_stop(bg);
  g_assert_cmpuint(acquired, ==, released);
  early_response = FALSE;
}
static void test_cancel(void) {
  own(TRUE); grant = TRUE; defer_response = TRUE; version = 2;
  guint before = requests, closed_before = closes;
  g_autoptr(GSettings) config = settings();
  g_settings_set_boolean(config, "background-disclosure-accepted", TRUE);
  g_autoptr(GApplication) app = application();
  g_autoptr(GrottoBackground) bg = grotto_background_new(app, client, config, TRUE);
  await_counter(&requests, before + 1);
  g_assert_cmpuint(grotto_background_get_holds(bg), ==, 1);
  grotto_background_stop(bg);
  await_counter(&closes, closed_before + 1);
  send_response(); /* The cancelled generation cannot regain a hold. */
  g_dbus_connection_flush_sync(service, NULL, NULL);
  while (g_main_context_iteration(NULL, FALSE)) {}
  g_assert_cmpint(grotto_background_get_state(bg), ==, GROTTO_BACKGROUND_DISABLED);
  g_assert_cmpuint(acquired, ==, released);
  defer_response = FALSE;
}
static void test_v1(void) {
  own(TRUE); grant = TRUE; defer_response = FALSE; version = 1;
  guint before = statuses;
  g_autoptr(GSettings) config = settings();
  g_settings_set_boolean(config, "background-disclosure-accepted", TRUE);
  g_settings_set_boolean(config, "launch-on-login", TRUE);
  g_autoptr(GApplication) app = application();
  g_autoptr(GrottoBackground) bg = grotto_background_new(app, client, config, FALSE);
  await_state(bg, GROTTO_BACKGROUND_REGISTERED);
  g_assert_true(portal_launch);
  grotto_background_set_status(bg, TRUE, TRUE);
  g_assert_cmpuint(statuses, ==, before);
  g_autofree gchar *file = g_build_filename(g_get_user_config_dir(), "autostart", "org.nostr.Grotto.desktop", NULL);
  g_assert_false(g_file_test(file, G_FILE_TEST_EXISTS));
  guint request_count = requests;
  g_settings_set_boolean(config, "run-in-background", FALSE);
  await_state(bg, GROTTO_BACKGROUND_DISABLED);
  g_assert_cmpuint(requests, ==, request_count + 1);
  g_assert_false(portal_launch);
  g_assert_false(g_settings_get_boolean(config, "launch-on-login"));
  g_assert_cmpuint(grotto_background_get_holds(bg), ==, 0);
  grotto_background_stop(bg); g_assert_cmpuint(acquired, ==, released);
}
static void test_migration(void) {
  g_autoptr(GSettings) config = settings();
  g_settings_set_boolean(config, "autostart", TRUE);
  g_settings_set_boolean(config, "start-minimized", TRUE);
  grotto_background_migrate_settings(config);
  g_assert_true(g_settings_get_boolean(config, "launch-on-login"));
  g_assert_true(g_settings_get_boolean(config, "run-in-background"));
  g_assert_false(g_settings_get_boolean(config, "background-disclosure-accepted"));
  g_assert_cmpuint(g_settings_get_uint(config, "settings-migration-version"), ==, 1);
  g_assert_true(grotto_background_sync_autostart(config, TRUE, NULL));
  g_autofree gchar *file = g_build_filename(g_get_user_config_dir(), "autostart", "org.nostr.Grotto.desktop", NULL);
  g_autoptr(GKeyFile) entry = g_key_file_new();
  g_assert_true(g_key_file_load_from_file(entry, file, 0, NULL));
  g_autofree gchar *exec = g_key_file_get_string(entry, "Desktop Entry", "Exec", NULL);
  g_assert_nonnull(strstr(exec, "--gapplication-service"));
  g_settings_set_boolean(config, "launch-on-login", FALSE);
  grotto_background_migrate_settings(config);
  g_assert_false(g_settings_get_boolean(config, "launch-on-login"));
  g_assert_true(grotto_background_sync_autostart(config, TRUE, NULL));
  g_assert_false(g_file_test(file, G_FILE_TEST_EXISTS));
  /* Explicit new policy wins over legacy values. */
  g_settings_set_uint(config, "settings-migration-version", 0);
  g_settings_set_boolean(config, "run-in-background", FALSE);
  g_settings_set_boolean(config, "launch-on-login", TRUE);
  grotto_background_migrate_settings(config);
  g_assert_false(g_settings_get_boolean(config, "run-in-background"));
  g_assert_false(g_settings_get_boolean(config, "launch-on-login"));
}
int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  if (!nostrc_test_bus_available()) return 77;
  bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NONE); nostrc_test_bus_up(bus);
  service = nostrc_test_bus_connect(bus); client = nostrc_test_bus_connect(bus);
  g_autoptr(GDBusNodeInfo) info = g_dbus_node_info_new_for_xml(xml, NULL);
  registration = g_dbus_connection_register_object(service, "/org/freedesktop/portal/desktop", info->interfaces[0], &vtable, NULL, NULL, NULL);
  g_assert_cmpuint(registration, >, 0);
  nostrc_test_bus_add_func("/background/absent-native", test_absent);
  nostrc_test_bus_add_func("/background/absent-sandbox", test_absent_sandbox);
  nostrc_test_bus_add_func("/background/disclosure-and-disable", test_disclosure);
  nostrc_test_bus_add_func("/background/denied", test_denied);
  nostrc_test_bus_add_func("/background/granted-owner-loss", test_granted);
  nostrc_test_bus_add_func("/background/cancel-late-response", test_cancel);
  nostrc_test_bus_add_func("/background/portal-v1-autostart", test_v1);
  nostrc_test_bus_add_func("/background/migration-v1", test_migration);
  int status = g_test_run();
  g_dbus_connection_unregister_object(service, registration);
  if (request_registration) g_dbus_connection_unregister_object(service, request_registration);
  g_free(response_path); g_free(response_peer);
  /* Drain owned asynchronous callback refs before taking the private bus down. */
  while (g_main_context_iteration(NULL, FALSE)) {}
  nostrc_test_bus_down(bus);
  return status;
}
