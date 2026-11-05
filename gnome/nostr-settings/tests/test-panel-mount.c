/* Real Settings shell + real daemon, Linux executable-role fixtures.
 * No signer/service/widget mocks. Only desktop activation is captured.
 * SPDX-License-Identifier: MIT */
#include "nss-ui.h"
#include "tests/common/nostrc-test-bus.h"
#include <glib/gstdio.h>
#include <signal.h>
#include <string.h>

#define CONTROL "org.nostr.Grotto.Control1"
#define PATH "/org/nostr/signer/grotto"
#define APP "exe:/fixture/connected-app"
static GDBusConnection *bus;
static NostrcTestBus *private_bus;
static GSubprocess *fixture_daemon;
static const char *daemon_bin;
static guint handoffs;
static gboolean saw_backup, saw_review, saw_security;
static gboolean timeout(gpointer data) { (void)data; g_error("W6 event did not arrive"); return G_SOURCE_REMOVE; }
#define WAIT(condition) G_STMT_START { guint bound = g_timeout_add_seconds(20, timeout, NULL); \
  while (!(condition)) { g_main_context_iteration(NULL, TRUE); } g_source_remove(bound); } G_STMT_END

static GVariant *rpc(const char *method, GVariant *args, const char *expected_error)
{
  g_autoptr(GError) error = NULL;
  GVariant *reply = g_dbus_connection_call_sync(bus, "org.nostr.Signer", PATH, CONTROL,
    method, args, NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, 5000, NULL, &error);
  if (expected_error) {
    g_assert_null(reply); g_assert_nonnull(error);
    g_autofree char *name = g_dbus_error_get_remote_error(error);
    g_assert_cmpstr(name, ==, expected_error);
  } else {
    if (error) g_test_message("Control1 method failed: %s", method);
    g_assert_no_error(error); g_assert_nonnull(reply);
  }
  return reply;
}
static void role(const char *expected)
{
  g_autoptr(GVariant) reply = rpc("GetStatus", NULL, NULL);
  g_autoptr(GVariant) status = g_variant_get_child_value(reply, 0);
  const char *actual = NULL;
  g_assert_true(g_variant_lookup(status, "management-role", "&s", &actual));
  g_assert_cmpstr(actual, ==, expected);
  g_autoptr(GVariant) caps = g_variant_lookup_value(status, "management-capabilities", G_VARIANT_TYPE("a{sb}"));
  GVariantIter iter; const char *key; gboolean allowed;
  g_variant_iter_init(&iter, caps);
  while (g_variant_iter_next(&iter, "{&sb}", &key, &allowed))
    g_assert_cmpint(allowed, ==, !strcmp(expected, "approver") || (!strcmp(expected, "manager") && !strcmp(key, "set-client-label")));
}
static GVariant *empty_arg(const char *signature)
{
  if (!strcmp(signature, "s")) return g_variant_new_string("");
  if (!strcmp(signature, "b")) return g_variant_new_boolean(FALSE);
  if (!strcmp(signature, "t")) return g_variant_new_uint64(0);
  if (!strcmp(signature, "u")) return g_variant_new_uint32(0);
  if (*signature == 'a') return g_variant_new_array(G_VARIANT_TYPE(signature + 1), NULL, 0);
  g_error("Uncovered Control1 input signature: %s", signature);
  return NULL;
}
static gboolean manager_read(const char *method)
{
  return !strcmp(method, "GetStatus") || !strcmp(method, "ListIdentities") ||
    !strcmp(method, "ListClients") || !strcmp(method, "ListPermissions") ||
    !strcmp(method, "ListRequests") || !strcmp(method, "ListActivity");
}
static void rejected_contract(void)
{
  g_autofree char *xml = NULL; g_assert_true(g_file_get_contents(W6_CONTROL_XML, &xml, NULL, NULL));
  g_autoptr(GDBusNodeInfo) node = g_dbus_node_info_new_for_xml(xml, NULL);
  guint refused = 0;
  for (guint i = 0; node->interfaces[0]->methods[i]; i++) {
    GDBusMethodInfo *m = node->interfaces[0]->methods[i];
    if (manager_read(m->name) || !strcmp(m->name, "SetClientLabel")) continue;
    GVariantBuilder args; g_variant_builder_init(&args, G_VARIANT_TYPE_TUPLE);
    for (guint j = 0; m->in_args && m->in_args[j]; j++)
      g_variant_builder_add_value(&args, empty_arg(m->in_args[j]->signature));
    rpc(m->name, g_variant_builder_end(&args), CONTROL ".Error.NotAuthorized");
    g_print("manager refused %s\n", m->name); refused++;
  }
  rpc("SetPaused", g_variant_new("(b)", TRUE), CONTROL ".Error.NotAuthorized");
  g_assert_cmpuint(refused, >=, 20);
}
static gboolean text_in(GtkWidget *widget, const char *text)
{
  if (GTK_IS_LABEL(widget) && strstr(gtk_label_get_text(GTK_LABEL(widget)), text)) return TRUE;
  if (ADW_IS_ACTION_ROW(widget)) {
    const char *title = adw_preferences_row_get_title(ADW_PREFERENCES_ROW(widget));
    const char *sub = adw_action_row_get_subtitle(ADW_ACTION_ROW(widget));
    if ((title && strstr(title, text)) || (sub && strstr(sub, text))) {
      g_assert_false(adw_preferences_row_get_use_markup(ADW_PREFERENCES_ROW(widget))); return TRUE;
    }
  }
  for (GtkWidget *c = gtk_widget_get_first_child(widget); c; c = gtk_widget_get_next_sibling(c))
    if (text_in(c, text)) return TRUE;
  return FALSE;
}
static void assert_host_chrome(GtkWidget *widget)
{
  if (ADW_IS_HEADER_BAR(widget)) {
    g_assert_false(adw_header_bar_get_show_start_title_buttons(ADW_HEADER_BAR(widget)));
    g_assert_false(adw_header_bar_get_show_end_title_buttons(ADW_HEADER_BAR(widget)));
  }
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
    assert_host_chrome(child);
}
static gboolean model_label(GListModel *model, const char *label)
{
  for (guint i = 0; i < g_list_model_get_n_items(model); i++) {
    g_autoptr(GrottoPanelsItem) item = g_list_model_get_item(model, i);
    if (!strcmp(grotto_panels_item_get_metadata(item)->label, label)) return TRUE;
  }
  return FALSE;
}
static void invoked(GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GError) error = NULL;
  g_assert_true(grotto_panels_service_invoke_finish(GROTTO_PANELS_SERVICE(source), result, &error));
  g_assert_no_error(error); *(gboolean *)data = TRUE;
}
static void fixture_call(const char *method)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_sync(bus, g_getenv("W6_TEST_HOST"), "/org/nostr/Grotto",
    "org.nostr.TestHost", method, NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
  g_assert_no_error(error); g_assert_nonnull(reply);
}
static void copy_binary(const char *source, const char *destination)
{
  g_autoptr(GFile) from = g_file_new_for_path(source), to = g_file_new_for_path(destination);
  g_assert_true(g_file_copy(from, to, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, NULL));
  g_assert_cmpint(g_chmod(destination, 0700), ==, 0);
}
static int manager_child(const char *original, const char *self)
{
  adw_init(); bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL); g_assert_nonnull(bus);
  role("manager"); rejected_contract();
  g_autoptr(GVariant) permissions = rpc("ListPermissions", g_variant_new("(s)", APP), NULL);
  g_autoptr(GVariant) grants = g_variant_get_child_value(permissions, 0);
  g_assert_cmpuint(g_variant_n_children(grants), ==, 1);
  g_autoptr(GVariant) ids = rpc("ListIdentities", NULL, NULL);
  g_autoptr(GVariant) rows = g_variant_get_child_value(ids, 0);
  g_assert_cmpuint(g_variant_n_children(rows), ==, 1);
  g_autoptr(GVariant) identity = g_variant_get_child_value(rows, 0);
  const char *npub = NULL; g_assert_true(g_variant_lookup(identity, "identity", "&s", &npub));
  g_assert_true(g_str_has_prefix(npub, "npub1"));
  g_autoptr(GtkApplication) app = gtk_application_new("org.nostr.Settings", G_APPLICATION_NON_UNIQUE);
  g_assert_true(g_application_register(G_APPLICATION(app), NULL, NULL));
  GtkWindow *window = nss_window_new(app, "identity"); gtk_window_present(window);
  NssContext *ctx = g_object_get_data(G_OBJECT(window), "nss-context");
  g_assert_nonnull(ctx->grotto);
  GListModel *identities = grotto_panels_service_get_model(ctx->grotto, GROTTO_PANELS_IDENTITIES);
  GListModel *apps = grotto_panels_service_get_model(ctx->grotto, GROTTO_PANELS_APPS);
  WAIT(g_list_model_get_n_items(identities) == 1 && model_label(apps, "Connected app fixture"));
  GrottoPanel *id_panel = nss_window_get_panel(window, "identity");
  GrottoPanel *apps_panel = nss_window_get_panel(window, "apps");
  g_assert_true(GROTTO_IS_IDENTITY_PANEL(id_panel)); g_assert_true(GROTTO_IS_APPS_PANEL(apps_panel));
  g_assert_true(GROTTO_IS_ACTIVITY_PANEL(nss_window_get_panel(window, "activity")));
  g_assert_true(GROTTO_IS_REMOTE_PANEL(nss_window_get_panel(window, "remote-signing")));
  const char *panel_names[] = {"identity", "apps", "activity", "remote-signing"};
  for (guint i = 0; i < G_N_ELEMENTS(panel_names); i++)
    assert_host_chrome(GTK_WIDGET(nss_window_get_panel(window, panel_names[i])));
  WAIT(text_in(GTK_WIDGET(id_panel), npub));
  g_assert_true(nss_window_show_page(window, "apps"));
  WAIT(text_in(GTK_WIDGET(apps_panel), "Connected app fixture"));
  gboolean done = FALSE;
  GrottoPanelCommandArgs args = {.principal = APP, .label = "<b>Manager label</b>"};
  grotto_panels_service_invoke_async(ctx->grotto, GROTTO_PANEL_SET_CLIENT_LABEL, &args, NULL, invoked, &done);
  WAIT(done && model_label(apps, args.label));
  WAIT(text_in(GTK_WIDGET(apps_panel), args.label));
  g_assert_false(grotto_panel_can_command(apps_panel, GROTTO_PANEL_SET_CLIENT_TRUST));
  grotto_apps_panel_show_app(GROTTO_APPS_PANEL(apps_panel), APP);
  assert_host_chrome(GTK_WIDGET(apps_panel));
  WAIT(text_in(GTK_WIDGET(apps_panel), "Grotto"));
  g_assert_true(grotto_panel_host_invoke(apps_panel, GROTTO_PANEL_HOST_OPEN_SIGNER, "app:" APP, NULL));
  g_assert_true(grotto_panel_host_invoke(id_panel, GROTTO_PANEL_HOST_BACKUP, npub, NULL));
  g_assert_true(grotto_panel_host_invoke(apps_panel, GROTTO_PANEL_HOST_REVIEW, "fixture-request", NULL));
  /* Calls above use the production D-Bus activation wire, not a substituted callback. */
  fixture_call("CheckHandoffs");
  fixture_call("DropGrotto");
  g_assert_true(grotto_panel_host_invoke(id_panel, GROTTO_PANEL_HOST_BACKUP, npub, NULL));
  WAIT(text_in(GTK_WIDGET(window), "Grotto could not be opened"));
  fixture_call("StopDaemon");
  WAIT(!grotto_panels_service_is_available(ctx->grotto));
  WAIT(g_list_model_get_n_items(identities) == 0 && g_list_model_get_n_items(apps) == 0);
  g_assert_false(grotto_panels_service_can(ctx->grotto, GROTTO_PANEL_SET_CLIENT_LABEL));
  fixture_call("StartDaemon");
  WAIT(grotto_panels_service_is_available(ctx->grotto) && model_label(apps, args.label));
  g_assert_true(grotto_panels_service_can(ctx->grotto, GROTTO_PANEL_SET_CLIENT_LABEL));
  gtk_window_set_default_size(window, 360, 640);
  g_assert_true(nss_window_show_page(window, "remote-signing"));
  /* Same bytes, new inode. A still-running former manager must lose reads and labels. */
  g_autofree char *replacement = g_strconcat(self, ".new", NULL);
  copy_binary(original, replacement); g_assert_cmpint(g_rename(replacement, self), ==, 0);
  role("session");
  rpc("ListIdentities", NULL, CONTROL ".Error.NotAuthorized");
  rpc("ListClients", NULL, CONTROL ".Error.NotAuthorized");
  rpc("SetClientLabel", g_variant_new("(ss)", APP, "replaced"), CONTROL ".Error.NotAuthorized");
  g_assert_true(nss_window_show_page(window, "activity"));
  gtk_window_close(window);
  GtkWindow *reopened = nss_window_new(app, NULL);
  g_assert_cmpstr(nss_window_get_page(reopened), ==, "activity");
  g_assert_true(nss_window_show_page(reopened, "identity"));
  g_assert_false(nss_window_show_page(reopened, "unknown-page"));
  gtk_window_destroy(reopened);
  g_print("W6 real-host manager mount, label signal, handoffs, loss/restart and inode replacement passed\n");
  return 0;
}
static void receiver(GDBusConnection *connection, const char *sender, const char *path,
  const char *interface, const char *method, GVariant *args, GDBusMethodInvocation *inv, gpointer data)
{
  (void)connection; (void)sender; (void)path; (void)interface; (void)data;
  if (!strcmp(method, "ActivateAction")) {
    const char *action; g_autoptr(GVariant) values = NULL, platform = NULL;
    g_variant_get(args, "(&s@av@a{sv})", &action, &values, &platform);
    g_assert_cmpstr(action, ==, "open-management");
    g_assert_cmpuint(g_variant_n_children(values), ==, 1);
    g_autoptr(GVariant) value = g_variant_get_child_value(values, 0);
    g_autoptr(GVariant) tuple = g_variant_get_variant(value);
    const char *section, *target; g_variant_get(tuple, "(&s&s)", &section, &target);
    if (!strcmp(section, "backup")) { g_assert_true(g_str_has_prefix(target, "npub1")); saw_backup = TRUE; }
    else if (!strcmp(section, "review")) { g_assert_cmpstr(target, ==, "fixture-request"); saw_review = TRUE; }
    else { g_assert_cmpstr(section, ==, "target"); g_assert_cmpstr(target, ==, "app:" APP); saw_security = TRUE; }
    handoffs++;
  } else if (!strcmp(method, "CheckHandoffs")) {
    g_assert_cmpuint(handoffs, ==, 3); g_assert_true(saw_backup && saw_review && saw_security);
  } else if (!strcmp(method, "DropGrotto")) {
    g_autoptr(GVariant) released = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "ReleaseName", g_variant_new("(s)", "org.nostr.Grotto"), NULL, 0, 5000, NULL, NULL);
    g_assert_nonnull(released);
  } else if (!strcmp(method, "StopDaemon")) {
    g_subprocess_send_signal(fixture_daemon, SIGTERM); g_assert_true(g_subprocess_wait(fixture_daemon, NULL, NULL)); fixture_daemon = NULL;
  } else if (!strcmp(method, "StartDaemon")) {
    const char *argv[] = {daemon_bin, NULL};
    fixture_daemon = nostrc_test_bus_spawn_supervised(private_bus, "daemon-restart.log", NULL, argv);
  } else g_assert_not_reached();
  g_dbus_method_invocation_return_value(inv, NULL);
}
static const GDBusInterfaceVTable receiver_vtable = {.method_call = receiver};
static void acquired(GDBusConnection *c, const char *sender, const char *path, const char *interface,
  const char *name, GVariant *args, gpointer data)
{
  (void)c; (void)sender; (void)path; (void)interface; (void)name;
  const char *service, *old, *owner; g_variant_get(args, "(&s&s&s)", &service, &old, &owner);
  if (!strcmp(service, "org.nostr.Signer") && *owner) *(gboolean *)data = TRUE;
}
static void process_done(GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GError) error = NULL;
  g_assert_true(g_subprocess_wait_check_finish(G_SUBPROCESS(source), result, &error));
  g_assert_no_error(error); *(gboolean *)data = TRUE;
}
int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  if (argc == 4 && !strcmp(argv[1], "--manager")) return manager_child(argv[2], argv[3]);
  if (argc == 2 && !strcmp(argv[1], "--untrusted")) {
    bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL); g_assert_nonnull(bus);
    g_autoptr(GVariant) name = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "RequestName", g_variant_new("(su)", "org.nostr.Settings", 0u), NULL, 0, 5000, NULL, NULL);
    g_assert_nonnull(name); role("session");
    const char *methods[] = {"ListIdentities", "ListClients", "ListPermissions", "ListRequests", "ListActivity", "SetClientLabel"};
    for (guint i = 0; i < G_N_ELEMENTS(methods); i++)
      rpc(methods[i], i == 2 ? g_variant_new("(s)", APP) : i == 4 ? g_variant_new("(tu)", (guint64)0, 10u) :
        i == 5 ? g_variant_new("(ss)", APP, "untrusted") : NULL, CONTROL ".Error.NotAuthorized");
    return 0;
  }
  g_assert_cmpint(argc, ==, 2); daemon_bin = argv[1];
  g_autofree char *original = g_canonicalize_filename(argv[0], NULL);
  g_autofree char *home = g_dir_make_tmp("nostrc-w6-host-XXXXXX", NULL); g_assert_nonnull(home);
  g_setenv("XDG_RUNTIME_DIR", home, TRUE);
  g_setenv("XDG_CONFIG_HOME", home, TRUE); g_setenv("XDG_DATA_HOME", home, TRUE); g_setenv("XDG_CACHE_HOME", home, TRUE);
  g_setenv("GSETTINGS_BACKEND", "memory", TRUE);
  g_setenv("NOSTR_SIGNER_SECKEY_HEX", "0000000000000000000000000000000000000000000000000000000000000001", TRUE);
  g_setenv("NOSTR_SIGNER_TEST_APPROVERS", original, TRUE);
  g_autofree char *manager = g_build_filename(home, "nostr-settings", NULL);
  g_autofree char *untrusted = g_build_filename(home, "untrusted", NULL);
  copy_binary(original, manager); copy_binary(original, untrusted);
  g_setenv("NOSTR_SIGNER_TEST_MANAGERS", manager, TRUE);
  g_assert_true(nostrc_test_bus_available());
  private_bus = nostrc_test_bus_new(0); nostrc_test_bus_up(private_bus);
  bus = nostrc_test_bus_connect(private_bus);
  gboolean ready = FALSE;
  guint watch = g_dbus_connection_signal_subscribe(bus, "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged",
    "/org/freedesktop/DBus", "org.nostr.Signer", 0, acquired, &ready, NULL);
  const char *daemon_argv[] = {daemon_bin, NULL};
  fixture_daemon = nostrc_test_bus_spawn_supervised(private_bus, "daemon.log", NULL, daemon_argv); WAIT(ready);
  role("approver");
  g_autoptr(GVariant) status_reply = rpc("GetStatus", NULL, NULL);
  g_autoptr(GVariant) status = g_variant_get_child_value(status_reply, 0); guint64 revision = 0;
  g_assert_true(g_variant_lookup(status, "permissions_revision", "t", &revision));
  GVariantBuilder rows, row; g_variant_builder_init(&rows, G_VARIANT_TYPE("aa{sv}"));
  g_variant_builder_init(&row, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&row, "{sv}", "principal", g_variant_new_string(APP));
  g_variant_builder_add(&row, "{sv}", "identity", g_variant_new_string("*"));
  g_variant_builder_add(&row, "{sv}", "expires-at", g_variant_new_uint64(0));
  g_variant_builder_add(&row, "{sv}", "operation", g_variant_new_string("get_relays"));
  g_variant_builder_add(&row, "{sv}", "decision", g_variant_new_string("allow"));
  g_variant_builder_add(&row, "{sv}", "event-kind", g_variant_new_int32(-1));
  g_variant_builder_add_value(&rows, g_variant_builder_end(&row));
  g_autoptr(GVariant) seeded = rpc("SetPermissions", g_variant_new("(ss@aa{sv}t)", APP, "*", g_variant_builder_end(&rows), revision), NULL);
  g_autoptr(GVariant) label = rpc("SetClientLabel", g_variant_new("(ss)", APP, "Connected app fixture"), NULL);
  /* Fixture approver retains security authority (manager contract above refuses this). */
  g_autoptr(GVariant) trust = rpc("SetClientTrust", g_variant_new("(ss)", APP, "low"), NULL);
  g_autoptr(GVariant) paused = rpc("SetPaused", g_variant_new("(b)", TRUE), NULL);
  g_autoptr(GVariant) resumed = rpc("SetPaused", g_variant_new("(b)", FALSE), NULL);
  const char *xml = "<node><interface name='org.freedesktop.Application'><method name='ActivateAction'><arg type='s' direction='in'/><arg type='av' direction='in'/><arg type='a{sv}' direction='in'/></method></interface><interface name='org.nostr.TestHost'><method name='CheckHandoffs'/><method name='DropGrotto'/><method name='StopDaemon'/><method name='StartDaemon'/></interface></node>";
  g_autoptr(GDBusNodeInfo) node = g_dbus_node_info_new_for_xml(xml, NULL);
  guint registrations[2];
  for (guint i = 0; i < 2; i++) registrations[i] = g_dbus_connection_register_object(bus, "/org/nostr/Grotto", node->interfaces[i], &receiver_vtable, NULL, NULL, NULL);
  g_autoptr(GVariant) own = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
    "RequestName", g_variant_new("(su)", "org.nostr.Grotto", 0u), NULL, 0, 5000, NULL, NULL); g_assert_nonnull(own);
  g_setenv("W6_TEST_HOST", g_dbus_connection_get_unique_name(bus), TRUE);
  g_autoptr(GSubprocess) other = g_subprocess_new(0, NULL, untrusted, "--untrusted", NULL);
  gboolean finished = FALSE; g_subprocess_wait_check_async(other, NULL, process_done, &finished); WAIT(finished);
  g_autoptr(GSubprocess) child = g_subprocess_new(0, NULL, manager, "--manager", original, manager, NULL);
  finished = FALSE; g_subprocess_wait_check_async(child, NULL, process_done, &finished); WAIT(finished);
  g_dbus_connection_signal_unsubscribe(bus, watch);
  for (guint i = 0; i < 2; i++) g_dbus_connection_unregister_object(bus, registrations[i]);
  nostrc_test_bus_down(private_bus);
  g_autoptr(GSubprocess) cleanup = g_subprocess_new(0, NULL, "rm", "-rf", "--", home, NULL);
  g_assert_true(g_subprocess_wait_check(cleanup, NULL, NULL));
  return 0;
}
