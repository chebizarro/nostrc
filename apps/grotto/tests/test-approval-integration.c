#include "approval-controller.h"
#include "nostr-event.h"
#include "nostr-json.h"
#include "nostr-kinds.h"
#include "nostrc-test-bus.h"
#include "nostrc-test-gdk-frame.h"
#include <glib/gstdio.h>
#include <string.h>
#include <sys/resource.h>

typedef struct {
  guint done;
  GVariant *reply;
  GError *error;
} Reply;
static NostrcTestBus *active_bus;
static gboolean bound(gpointer data) {
  (void)data;
  if (active_bus) {
    nostrc_test_bus_dump_log(active_bus, "g07-daemon.log");
    nostrc_test_bus_dump_log(active_bus, "g07-gui.log");
  }
  g_error("Real-daemon approval did not complete");
  return G_SOURCE_REMOVE;
}
static guint trust_changes;
static void controller_message(GrottoApprovalController *s, const char *text, gpointer data) {
  (void)s;
  (void)data;
  if (g_str_has_prefix(text, "Trust level changed."))
    trust_changes++;
  g_printerr("Controller: %s\n", text);
}
#define WAIT(condition)                                                                            \
  do {                                                                                             \
    guint id = g_timeout_add_seconds(20, bound, NULL);                                             \
    while (!(condition))                                                                           \
      g_main_context_iteration(NULL, TRUE);                                                        \
    g_source_remove(id);                                                                           \
  } while (0)
static void bus_done(GObject *source, GAsyncResult *result, gpointer data) {
  Reply *r = data;
  r->reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &r->error);
  r->done++;
}
static void decision_done(GObject *source, GAsyncResult *result, gpointer data) {
  Reply *r = data;
  if (grotto_approval_controller_decide_finish(GROTTO_APPROVAL_CONTROLLER(source), result,
                                               &r->error))
    r->reply = g_variant_ref_sink(g_variant_new("(b)", TRUE));
  r->done++;
}
static void clear_reply(Reply *r) {
  g_clear_pointer(&r->reply, g_variant_unref);
  g_clear_error(&r->error);
  memset(r, 0, sizeof *r);
}
static void sign(GDBusConnection *bus, guint32 kind, const char *content, Reply *r) {
  g_autofree gchar *json = g_strdup_printf(
      "{\"kind\":%u,\"created_at\":1,\"tags\":[],\"content\":\"%s\"}", kind, content);
  g_dbus_connection_call(bus, "org.nostr.Signer", "/org/nostr/signer", "org.nostr.Signer",
                         "SignEvent", g_variant_new("(sss)", json, "", "spoofed-name"),
                         G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 30000, NULL, bus_done, r);
}
static gboolean ready(GrottoApprovalController *controller) {
  GListModel *pending = grotto_approval_controller_get_pending(controller);
  if (!g_list_model_get_n_items(pending))
    return FALSE;
  g_autoptr(GrottoApprovalRequest) r = g_list_model_get_item(pending, 0);
  return grotto_approval_request_get_state(r) == GROTTO_APPROVAL_READY;
}
static void owner_seen(GDBusConnection *bus, const char *name, const char *owner, gpointer data) {
  (void)bus;
  (void)name;
  (void)owner;
  *(gboolean *)data = TRUE;
}
static void verify(Reply *r, guint32 kind) {
  g_assert_no_error(r->error);
  g_assert_nonnull(r->reply);
  const char *json;
  g_variant_get(r->reply, "(&s)", &json);
  NostrEvent *event = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize(event, json), ==, 0);
  g_assert_true(nostr_event_check_signature(event));
  g_assert_cmpint(nostr_event_get_kind(event), ==, (int)kind);
  nostr_event_free(event);
}
#ifndef __APPLE__
static gboolean count_frame(GtkWidget *widget, GdkFrameClock *clock, gpointer data) {
  (void)widget;
  (void)clock;
  return ++*(guint *)data < 2 ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}
static GtkWidget *find_button(GtkWidget *root, const char *label) {
  if (GTK_IS_BUTTON(root) && g_strcmp0(gtk_button_get_label(GTK_BUTTON(root)), label) == 0)
    return root;
  for (GtkWidget *child = gtk_widget_get_first_child(root); child;
       child = gtk_widget_get_next_sibling(child)) {
    GtkWidget *button = find_button(child, label);
    if (button)
      return button;
  }
  return NULL;
}
static void click_review(GrottoApprovalController *controller, GApplication *app, const char *id,
                         gboolean allow, gboolean remember) {
  grotto_approval_controller_review(controller, id);
  GList *windows = gtk_application_get_windows(GTK_APPLICATION(app));
  g_assert_nonnull(windows);
  AdwDialog *dialog = adw_window_get_visible_dialog(ADW_WINDOW(windows->data));
  g_assert_nonnull(dialog);
  WAIT(gtk_widget_get_mapped(GTK_WIDGET(dialog)));
  guint frames = 0;
  gtk_widget_add_tick_callback(GTK_WIDGET(dialog), count_frame, &frames, NULL);
  WAIT(frames >= 2);
  GtkCheckButton *check = GTK_CHECK_BUTTON(gtk_widget_get_template_child(
      GTK_WIDGET(dialog), GNOSTR_TYPE_APPROVAL_DIALOG, "chk_remember"));
  gtk_check_button_set_active(check, remember);
  if (!allow) {
    GtkExpander *details = GTK_EXPANDER(gtk_widget_get_template_child(
        GTK_WIDGET(dialog), GNOSTR_TYPE_APPROVAL_DIALOG, "details_expander"));
    gtk_expander_set_expanded(details, TRUE);
    GtkWidget *change = GTK_WIDGET(gtk_widget_get_template_child(
        GTK_WIDGET(dialog), GNOSTR_TYPE_APPROVAL_DIALOG, "btn_change_trust"));
    WAIT(gtk_widget_get_sensitive(change) && gtk_widget_get_mapped(change));
    GtkDropDown *picker = GTK_DROP_DOWN(gtk_widget_get_template_child(
        GTK_WIDGET(dialog), GNOSTR_TYPE_APPROVAL_DIALOG, "trust_dropdown"));
    gtk_drop_down_set_selected(picker, 1);
    guint before = trust_changes;
    g_signal_emit_by_name(change, "clicked");
    AdwDialog *alert = adw_window_get_visible_dialog(ADW_WINDOW(windows->data));
    g_assert_true(ADW_IS_ALERT_DIALOG(alert));
    WAIT(gtk_widget_get_mapped(GTK_WIDGET(alert)));
    guint alert_frames = 0;
    gtk_widget_add_tick_callback(GTK_WIDGET(alert), count_frame, &alert_frames, NULL);
    WAIT(alert_frames >= 2);
    GtkWidget *confirm = find_button(GTK_WIDGET(alert), "Change trust level");
    g_assert_nonnull(confirm);
    g_signal_emit_by_name(confirm, "clicked");
    WAIT(trust_changes > before);
    /* Tier mutation travels Control1; the required-confirmation contact-list
     * request remains pending, and Remember still travels ApproveRequest. */
    Reply clients = {0};
    g_dbus_connection_call(g_application_get_dbus_connection(app), "org.nostr.Signer",
                           "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1", "ListClients",
                           NULL, G_VARIANT_TYPE("(aa{sv})"), 0, 5000, NULL, bus_done, &clients);
    WAIT(clients.done);
    g_assert_no_error(clients.error);
    g_assert_nonnull(clients.reply);
    g_autoptr(GVariant) rows = g_variant_get_child_value(clients.reply, 0);
    gboolean medium = FALSE;
    for (gsize i = 0; i < g_variant_n_children(rows); i++) {
      g_autoptr(GVariant) row = g_variant_get_child_value(rows, i);
      const char *tier = NULL;
      if (g_variant_lookup(row, "trust-tier", "&s", &tier) && !strcmp(tier, "medium"))
        medium = TRUE;
    }
    g_assert_true(medium);
    g_assert_true(ready(controller));
    clear_reply(&clients);
  }
  GtkWidget *button = GTK_WIDGET(gtk_widget_get_template_child(
      GTK_WIDGET(dialog), GNOSTR_TYPE_APPROVAL_DIALOG, allow ? "btn_approve" : "btn_deny"));
  g_assert_true(gtk_widget_get_sensitive(button));
  g_signal_emit_by_name(button, "clicked");
}
#endif
static void run_flow(GDBusConnection *connection, GDBusConnection *consumer, GApplication *app,
                     gboolean gui) {
  g_autoptr(GrottoApprovalController) controller = grotto_approval_controller_new(app, connection);
  g_signal_connect(controller, "message", G_CALLBACK(controller_message), NULL);
  g_print("daemon name acquired; waiting for first request\n");
  Reply result = {0}, decision = {0};
  sign(consumer, NOSTR_KIND_TEXT_NOTE, "first", &result);
  WAIT(ready(controller) || result.done);
  g_assert_no_error(result.error);
  g_assert_true(ready(controller));
  GListModel *pending = grotto_approval_controller_get_pending(controller);
  g_autoptr(GrottoApprovalRequest) request = g_list_model_get_item(pending, 0);
  GVariant *info = grotto_approval_request_get_info(request);
  const char *identity = NULL, *principal = NULL;
  g_assert_true(g_variant_lookup(info, "identity", "&s", &identity));
  g_assert_true(g_str_has_prefix(identity, "npub1"));
  g_assert_true(g_variant_lookup(info, "principal", "&s", &principal));
  g_assert_cmpstr(principal, !=, "spoofed-name");
#ifndef __APPLE__
  if (gui) {
    click_review(controller, app, grotto_approval_request_get_id(request), TRUE, TRUE);
    decision.done = 1;
  } else
#endif
    grotto_approval_controller_decide_async(controller, grotto_approval_request_get_id(request),
                                            TRUE, TRUE, 600, NULL, decision_done, &decision);
  WAIT(result.done && decision.done);
  verify(&result, NOSTR_KIND_TEXT_NOTE);
  g_assert_no_error(decision.error);
  WAIT(g_list_model_get_n_items(pending) == 0 &&
       grotto_approval_controller_get_holds(controller) == 0);
  clear_reply(&result);
  clear_reply(&decision);
  sign(consumer, NOSTR_KIND_TEXT_NOTE, "remembered", &result);
  WAIT(result.done);
  verify(&result, NOSTR_KIND_TEXT_NOTE);
  g_assert_cmpuint(g_list_model_get_n_items(pending), ==, 0);
  clear_reply(&result);
  /* A new connection avoids the deliberate per-connection prompt-rate limit;
   * its verified principal is unchanged (no sleeps or policy bypass). */
  g_autoptr(GDBusConnection) extra = NULL;
  if (active_bus)
    consumer = nostrc_test_bus_connect(active_bus);
  else {
    extra =
        g_dbus_connection_new_for_address_sync(g_getenv("DBUS_SESSION_BUS_ADDRESS"),
                                               G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                                   G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
                                               NULL, NULL, NULL);
    g_assert_nonnull(extra);
    consumer = extra;
  }
  sign(consumer, NOSTR_KIND_FOLLOW_LIST, "different scope", &result);
  WAIT(ready(controller) || result.done);
  g_assert_no_error(result.error);
  g_assert_true(ready(controller));
  g_clear_object(&request);
  request = g_list_model_get_item(pending, 0);
#ifndef __APPLE__
  if (gui) {
    click_review(controller, app, grotto_approval_request_get_id(request), FALSE, FALSE);
    decision.done = 1;
  } else
#endif
    grotto_approval_controller_decide_async(controller, grotto_approval_request_get_id(request),
                                            FALSE, FALSE, 0, NULL, decision_done, &decision);
  WAIT(result.done && decision.done);
  g_assert_nonnull(result.error);
  g_assert_no_error(decision.error);
  WAIT(g_list_model_get_n_items(pending) == 0);
  clear_reply(&result);
  clear_reply(&decision);
  grotto_approval_controller_stop(controller);
  WAIT(grotto_approval_controller_get_holds(controller) == 0);

  while (g_main_context_iteration(NULL, FALSE)) {
  }
  g_print("approval integration: %s review path exercised\n",
          gui ? "real GTK dialog" : "headless controller (platform UI skip)");
}
#ifndef __APPLE__
typedef struct {
  guint done;
  gboolean ok;
  gchar *output;
  GError *error;
} WorkerReply;
static void worker_done(GObject *source, GAsyncResult *result, gpointer data) {
  WorkerReply *r = data;
  r->ok = g_subprocess_wait_check_finish(G_SUBPROCESS(source), result, &r->error);
  r->done++;
}
#endif
int main(int argc, char **argv) {
  gboolean worker = argc == 3 && !strcmp(argv[2], "--gui-worker");
  g_assert_true(argc == 2 || worker);
  argc = 2;
  g_assert_cmpstr(g_getenv("NOSTRC_TEST_ISOLATION"), ==, "1");
  if (!nostrc_test_bus_available())
    return 77;
  struct rlimit limit = {0, 0};
  setrlimit(RLIMIT_CORE, &limit);
  g_setenv("NOSTR_SIGNER_TEST_APPROVERS", argv[0], TRUE);
  g_setenv("NOSTR_SIGNER_SECKEY_HEX",
           "0000000000000000000000000000000000000000000000000000000000000001", TRUE);
  g_test_init(&argc, &argv, NULL);
  nostr_json_init();
  nostrc_test_bus_tolerate_ebadf();
#ifndef __APPLE__
  if (worker) {
    /* GTK/Adwaita have process-global bus proxies. Run the GUI in a short-lived
     * peer on the coordinator's private bus, not the developer's session bus;
     * the coordinator still checks that its own bus connections are released. */
    g_assert_nonnull(g_getenv("DBUS_SESSION_BUS_ADDRESS"));
    nostrc_test_tolerate_gdk_frame_warning();
    gtk_init();
    adw_init();
    g_object_set(gtk_settings_get_default(), "gtk-enable-animations", FALSE, NULL);
    g_autoptr(GApplication) app =
        G_APPLICATION(adw_application_new("org.test.GrottoApproval", G_APPLICATION_NON_UNIQUE));
    g_assert_true(g_application_register(app, NULL, NULL));
    g_autoptr(GDBusConnection) connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    g_autoptr(GDBusConnection) consumer =
        g_dbus_connection_new_for_address_sync(g_getenv("DBUS_SESSION_BUS_ADDRESS"),
                                               G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                                   G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
                                               NULL, NULL, NULL);
    g_assert_nonnull(consumer);
    run_flow(connection, consumer, app, TRUE);
    g_clear_object(&app);
    nostr_json_cleanup();
    return 0;
  }
#endif
  NostrcTestBus *bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NONE);
  active_bus = bus;
  nostrc_test_bus_up(bus);
  /* Per-invocation profile, removed by the bus lifeline even after an abort. */
  g_autofree gchar *config = g_build_filename(nostrc_test_bus_get_dir(bus), "config", NULL);
  g_assert_cmpint(g_mkdir(config, 0700), ==, 0);
  g_setenv("XDG_CONFIG_HOME", config, TRUE);
  GDBusConnection *connection = nostrc_test_bus_connect(bus);
  g_autoptr(GVariant) owned = g_dbus_connection_call_sync(
      connection, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
      "RequestName", g_variant_new("(su)", "org.nostr.Grotto", 0), NULL, 0, 5000, NULL, NULL);
  g_assert_nonnull(owned);
  gboolean started = FALSE;
  guint watch = g_bus_watch_name_on_connection(connection, "org.nostr.Signer", 0, owner_seen, NULL,
                                               &started, NULL);
  const char *command[] = {argv[1], NULL};
  nostrc_test_bus_spawn_supervised(bus, "g07-daemon.log", NULL, command);
  WAIT(started);
#ifndef __APPLE__
  if (g_getenv("DISPLAY")) {
    WorkerReply r = {0};
    g_autoptr(GError) error = NULL;
    g_autoptr(GSubprocessLauncher) launcher =
        g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDERR_MERGE);
    g_autofree gchar *log = g_build_filename(nostrc_test_bus_get_dir(bus), "g07-gui.log", NULL);
    g_subprocess_launcher_set_stdout_file_path(launcher, log);
    g_autoptr(GSubprocess) peer =
        g_subprocess_launcher_spawn(launcher, &error, argv[0], argv[1], "--gui-worker", NULL);
    g_assert_no_error(error);
    g_subprocess_wait_check_async(peer, NULL, worker_done, &r);
    WAIT(r.done);
    nostrc_test_bus_dump_log(bus, "g07-gui.log");
    g_assert_no_error(r.error);
    g_assert_true(r.ok);
    g_free(r.output);
  } else
#endif
  {
    g_autoptr(GApplication) app =
        g_application_new("org.test.GrottoApproval", G_APPLICATION_NON_UNIQUE);
    run_flow(connection, nostrc_test_bus_connect(bus), app, FALSE);
  }
  g_bus_unwatch_name(watch);
  nostrc_test_bus_dump_log(bus, "g07-daemon.log");
  nostrc_test_bus_down(bus);
  nostr_json_cleanup();
  return 0;
}
