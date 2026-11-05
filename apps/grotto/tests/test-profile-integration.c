/* SPDX-License-Identifier: MIT */
#include <grotto-panels.h>
#include "ui/approval_dialog.h"
#include "relay_store.h"
#include "bunker_test_relay.h"
#include "ui/signer-window.h"
#include "grotto-identity-service.h"
#include "session-manager.h"
#include "accounts_store.h"
#include "settings_manager.h"
#include "tests/common/nostrc-test-bus.h"
#include "nostrc-test-gdk-frame.h"
#include <nostr/nip19/nip19.h>
#include <nostr-event.h>
#include <json.h>
#include <keys.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <string.h>
static NostrcTestBus *private_bus;
static GtkWidget *diagnostic_window;
static void dump_labels(GtkWidget *widget) {
  if (!gtk_widget_get_mapped(widget)) return;
  if (GTK_IS_LABEL(widget)) g_test_message("Profile UI: %s", gtk_label_get_text(GTK_LABEL(widget)));
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) dump_labels(child);
}
static gboolean timeout(gpointer data) {
  if (diagnostic_window) dump_labels(diagnostic_window);
  nostrc_test_bus_dump_log(private_bus, "profile-daemon.log");
  g_error("Profile integration barrier: %s", (char *)data); return G_SOURCE_REMOVE;
}
#define WAIT(c) do { guint timer = g_timeout_add_seconds(30, timeout, #c); while (!(c)) g_main_context_iteration(NULL, TRUE); g_source_remove(timer); } while (0)
static GtkWidget *find(GtkWidget *widget, const char *text) {
  if ((GTK_IS_BUTTON(widget) && !g_strcmp0(gtk_button_get_label(GTK_BUTTON(widget)), text)) ||
      (GTK_IS_LABEL(widget) && !g_strcmp0(gtk_label_get_text(GTK_LABEL(widget)), text)) ||
      (ADW_IS_PREFERENCES_ROW(widget) && !g_strcmp0(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(widget)), text)) ||
      !g_strcmp0(gtk_widget_get_tooltip_text(widget), text)) return widget;
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) { GtkWidget *w = find(child, text); if (w) return w; }
  return NULL;
}
static gboolean presented(GtkWidget *widget, GdkFrameClock *clock, gpointer data) {
  (void)widget; (void)clock; return ++*(guint *)data < 3 ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}
static void remove_owned_tree(GFile *file) {
  if (g_file_query_file_type(file, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL) == G_FILE_TYPE_DIRECTORY) {
    g_autoptr(GFileEnumerator) children = g_file_enumerate_children(file, G_FILE_ATTRIBUTE_STANDARD_NAME, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
    GFileInfo *info;
    while (children && (info = g_file_enumerator_next_file(children, NULL, NULL))) {
      g_autoptr(GFile) child = g_file_get_child(file, g_file_info_get_name(info)); remove_owned_tree(child); g_object_unref(info);
    }
  }
  g_assert_true(g_file_delete(file, NULL, NULL));
}
static void finalised(gpointer data, GObject *object) { (void)object; *(gboolean *)data = TRUE; }
typedef struct { GDBusConnection *bus; guint reviews, decisions; char *request; } Approval;
static void decision_done(GObject *source, GAsyncResult *result, gpointer data) {
  Approval *a = data; g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
  g_assert_no_error(error); g_assert_nonnull(reply); a->decisions++;
}
static void reusable_refused(GObject *source, GAsyncResult *result, gpointer data) {
  Approval *a = data; g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
  g_assert_null(reply); g_assert_nonnull(error);
  g_autofree char *name = g_dbus_error_get_remote_error(error); g_assert_cmpstr(name, ==, "org.nostr.Signer.Error.InvalidInput");
  g_assert_nonnull(strstr(error->message, "Management requires one-time consent"));
  g_dbus_connection_call(a->bus, "org.nostr.Signer", "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1", "DecideRequest", g_variant_new("(ss)", a->request, "allow-once"), NULL, 0, 10000, NULL, decision_done, a);
}
static void info_done(GObject *source, GAsyncResult *result, gpointer data) {
  Approval *a = data; g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error); g_assert_no_error(error);
  g_autoptr(GVariant) rows = g_variant_get_child_value(reply, 0);
  g_assert_cmpuint(g_variant_n_children(rows), ==, 1);
  g_autoptr(GVariant) info = g_variant_get_child_value(rows, 0);
  const char *transport = NULL, *purpose = NULL; guint32 kind = 99; gboolean remember = TRUE;
  g_assert_true(g_variant_lookup(info, "transport", "&s", &transport)); g_assert_cmpstr(transport, ==, "MANAGEMENT");
  g_assert_true(g_variant_lookup(info, "purpose", "&s", &purpose)); g_assert_cmpstr(purpose, ==, "profile-management");
  g_assert_true(g_variant_lookup(info, "event_kind", "u", &kind)); g_assert_cmpuint(kind, ==, 0);
  g_assert_true(g_variant_lookup(info, "rememberable", "b", &remember)); g_assert_false(remember);
  GnostrApprovalDialog *dialog = g_object_ref_sink(gnostr_approval_dialog_new());
  gnostr_approval_dialog_set_info(dialog, info, "Grotto");
  const char *label = "", *identity = "";
  g_variant_lookup(info, "identity_label", "&s", &label); g_variant_lookup(info, "identity", "&s", &identity);
  g_autofree char *headline = g_strdup_printf("Publish profile for %s", *label ? label : identity);
  GtkLabel *title = GTK_LABEL(gtk_widget_get_template_child(GTK_WIDGET(dialog), GNOSTR_TYPE_APPROVAL_DIALOG, "header_title"));
  g_assert_cmpstr(gtk_label_get_text(title), ==, headline); g_object_unref(dialog);
  g_dbus_connection_call(a->bus, "org.nostr.Signer", "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1", "DecideRequest", g_variant_new("(ss)", a->request, "allow-24h"), NULL, 0, 10000, NULL, reusable_refused, a);
}
static void review(GDBusConnection *bus, const char *sender, const char *path, const char *interface, const char *signal, GVariant *args, gpointer data) {
  (void)bus; (void)sender; (void)path; (void)interface; (void)signal;
  Approval *a = data; a->reviews++; g_assert_cmpuint(a->reviews, ==, 1);
  g_autoptr(GVariant) id = g_variant_get_child_value(args, 4); a->request = g_variant_dup_string(id, NULL);
  g_dbus_connection_call(a->bus, "org.nostr.Signer", "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1", "ListRequests", NULL, NULL, 0, 10000, NULL, info_done, a);
}
static void daemon_ready(GDBusConnection *bus, const char *name, const char *owner, gpointer data) { (void)bus; (void)name; (void)owner; *(gboolean *)data = TRUE; }
static void seed_profiles(BunkerTestRelay *relay, const char *hex, const char *key) {
  for (guint i = 0; i < 2; i++) {
    NostrEvent *event = nostr_event_new(); nostr_event_set_kind(event, 0); nostr_event_set_pubkey(event, hex);
    nostr_event_set_created_at(event, g_get_real_time() / G_USEC_PER_SEC - (i ? 10 : 20));
    nostr_event_set_content(event, i ? "{\"display_name\":\"Newest\",\"extension\":{\"keep\":true}}" : "{\"display_name\":\"Older\"}");
    g_assert_cmpint(nostr_event_sign(event, key), ==, 0); g_assert_cmpint(nostr_event_validate(event, NULL), ==, NOSTR_EVENT_VALIDATION_OK);
    g_autofree char *json = nostr_event_serialize_compact(event); nostr_event_free(event);
    g_autoptr(JsonParser) parser = json_parser_new(); g_assert_true(json_parser_load_from_data(parser, json, -1, NULL));
    json_array_add_element(bunker_test_relay_events(relay), json_node_copy(json_parser_get_root(parser)));
    if (i) {
      JsonNode *forged = json_from_string(json, NULL);
      json_object_set_int_member(json_node_get_object(forged), "created_at", g_get_real_time() / G_USEC_PER_SEC + 1000);
      json_array_add_element(bunker_test_relay_events(relay), forged);
    }
  }
}
static void profile_activate(GApplication *application, gpointer data) {
  (void)data; AdwApplication *app = ADW_APPLICATION(application);
  nostrc_test_bus_tolerate_ebadf(); NostrcTestBus *bus = private_bus;
  GDBusConnection *connection = nostrc_test_bus_connect(bus);
  const char *sk = "0000000000000000000000000000000000000000000000000000000000000001";
  g_autofree char *hex = nostr_key_get_public(sk); guint8 pk[32];
  for (guint i = 0; i < 32; i++) { char byte[3] = {hex[2*i], hex[2*i+1], 0}; pk[i] = g_ascii_strtoull(byte, NULL, 16); }
  char *npub = NULL; g_assert_cmpint(nostr_nip19_encode_npub(pk, &npub), ==, 0);
  BunkerTestRelay *relay = bunker_test_relay_new(); seed_profiles(relay, hex, sk);
  RelayStore *store = relay_store_new_for_identity(npub); relay_store_add(store, bunker_test_relay_url(relay), TRUE, TRUE); relay_store_save(store); relay_store_free(store);
  g_setenv("NOSTR_SIGNER_SECKEY_HEX", sk, TRUE);
  AccountsStore *accounts = accounts_store_get_default(); g_assert_true(accounts_store_add(accounts, npub, "Everyday", NULL));
  g_settings_set_string(settings_manager_get_gsettings(settings_manager_get_default()), "default-identity", npub);
  gboolean ready = FALSE; guint watch = g_bus_watch_name_on_connection(connection, "org.nostr.Signer", 0, daemon_ready, NULL, &ready, NULL);
  guint gui_name = g_bus_own_name_on_connection(connection, "org.nostr.Grotto", 0, NULL, NULL, NULL, NULL);
  Approval approval = {.bus = connection};
  guint subscription = g_dbus_connection_signal_subscribe(connection, "org.nostr.Signer", "org.nostr.Signer", "ApprovalRequested", "/org/nostr/signer", NULL, 0, review, &approval, NULL);
  const char *command[] = {PROFILE_DAEMON_BIN, NULL};
  GSubprocess *daemon = nostrc_test_bus_spawn_supervised(bus, "profile-daemon.log", NULL, command); g_assert_nonnull(daemon); WAIT(ready); g_bus_unwatch_name(watch);
  gboolean window_gone = FALSE;
  SignerWindow *window = signer_window_new(app); g_object_ref_sink(window);
  g_object_weak_ref(G_OBJECT(window), finalised, &window_gone);
  diagnostic_window = GTK_WIDGET(window);
  gtk_window_set_default_size(GTK_WINDOW(window), 420, 720); gtk_window_present(GTK_WINDOW(window));
  signer_window_show_page(window, "identity");
  GtkStack *stack = GTK_STACK(gtk_widget_get_template_child(GTK_WIDGET(window), TYPE_SIGNER_WINDOW, "panel_stack"));
  GtkWidget *panel = gtk_stack_get_child_by_name(stack, "identity"); g_assert_true(GROTTO_IS_IDENTITY_PANEL(panel));
  g_assert_true(grotto_panel_has_profile_host(GROTTO_PANEL(panel)));
  WAIT(find(panel, "Edit Profile…") && gtk_widget_get_visible(find(panel, "Edit Profile…")) && gtk_widget_get_sensitive(find(panel, "Edit Profile…")));
  g_assert_nonnull(find(panel, "Newest")); g_assert_null(find(panel, "Older"));
  g_signal_emit_by_name(find(panel, "Edit Profile…"), "clicked");
  AdwDialog *dialog = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)); g_assert_nonnull(dialog);
  guint frames = 0; gtk_widget_add_tick_callback(GTK_WIDGET(dialog), presented, &frames, NULL); WAIT(frames == 3);
  gtk_editable_set_text(GTK_EDITABLE(find(GTK_WIDGET(dialog), "Name")), "alice");
  gtk_editable_set_text(GTK_EDITABLE(find(GTK_WIDGET(dialog), "Display Name")), "Alice");
  GtkWidget *save = find(GTK_WIDGET(dialog), "Save"); g_assert_true(gtk_widget_is_sensitive(save));
  GrottoHomeModel *model = grotto_identity_service_get_control(GROTTO_IDENTITY_SERVICE(grotto_panel_get_service(GROTTO_PANEL(panel))));
  /* Opening the real form can overlap a daemon snapshot refresh. Wait for
   * current authority, not only the last rendered button sensitivity. */
  WAIT(grotto_home_model_writable(model));
  g_assert_false(gn_session_manager_is_locked(gn_session_manager_get_default()));
  g_signal_emit_by_name(save, "clicked");
  WAIT(approval.decisions == 1 && !adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
  g_assert_cmpuint(approval.reviews, ==, 1);
  JsonArray *events = bunker_test_relay_events(relay); g_assert_cmpuint(json_array_get_length(events), ==, 4);
  g_autofree char *json = json_to_string(json_array_get_element(events, 3), FALSE);
  NostrEvent *event = nostr_event_new(); g_assert_cmpint(nostr_event_deserialize_signed(event, json, NULL), ==, NOSTR_EVENT_VALIDATION_OK);
  g_assert_cmpint(nostr_event_validate(event, NULL), ==, NOSTR_EVENT_VALIDATION_OK); g_assert_cmpint(nostr_event_get_kind(event), ==, 0); g_assert_cmpstr(nostr_event_get_pubkey(event), ==, hex);
  g_assert_nonnull(strstr(nostr_event_get_content(event), "alice"));
  g_autoptr(JsonParser) content = json_parser_new(); g_assert_true(json_parser_load_from_data(content, nostr_event_get_content(event), -1, NULL));
  g_assert_true(json_object_get_boolean_member(json_object_get_object_member(json_node_get_object(json_parser_get_root(content)), "extension"), "keep"));
  g_test_message("Published kind-0 event %s after exactly one review", nostr_event_get_id(event)); nostr_event_free(event);
  g_autofree char *cache = g_strdup_printf("%s/grotto/profiles/%s-0.json", g_get_user_cache_dir(), hex);
  g_assert_true(g_file_test(cache, G_FILE_TEST_IS_REGULAR));
  g_assert_nonnull(find(panel, "Alice"));
  guint connections, requests, published; bunker_test_relay_stats(relay, &connections, &requests, &published);
  g_assert_cmpuint(requests, >=, 3); /* initial fetch, Save discovery and post-publish EOSE re-fetch */
  /* Offline cache is the same host path, with no synthetic cached result. */
  bunker_test_relay_free(relay); relay = NULL;
  g_signal_emit_by_name(find(panel, "Refresh Profile"), "clicked"); WAIT(find(panel, "Offline — showing the saved profile"));
  g_assert_nonnull(find(panel, "Alice")); g_assert_cmpuint(approval.reviews, ==, 1);
  diagnostic_window = NULL;
  gtk_window_destroy(GTK_WINDOW(window)); g_object_unref(window); WAIT(window_gone);
  g_dbus_connection_signal_unsubscribe(connection, subscription); g_bus_unown_name(gui_name);
  g_subprocess_send_signal(daemon, SIGTERM); g_assert_true(g_subprocess_wait_check(daemon, NULL, NULL));
  accounts_store_free(accounts_store_get_default()); settings_manager_free(settings_manager_get_default());
  g_free(approval.request); g_free(npub); g_application_quit(application);
}
static void profile_integration(void) {
  gboolean app_gone = FALSE;
  g_autoptr(AdwApplication) app = adw_application_new("org.nostr.Grotto.ProfileTest", G_APPLICATION_NON_UNIQUE);
  g_object_weak_ref(G_OBJECT(app), finalised, &app_gone);
  g_signal_connect(app, "activate", G_CALLBACK(profile_activate), NULL);
  g_assert_cmpint(g_application_run(G_APPLICATION(app), 0, NULL), ==, 0);
  g_clear_object(&app); WAIT(app_gone);
}
int main(int argc, char **argv) {
  if (g_getenv("GNOSTR_SKIP_UI_TESTS")) return 77;
  g_test_init(&argc, &argv, NULL); nostrc_test_tolerate_gdk_frame_warning();
  g_autofree char *binary = g_canonicalize_filename(argv[0], NULL); g_setenv("NOSTR_SIGNER_TEST_APPROVERS", binary, TRUE);
  g_autofree char *defaults = g_strconcat("event@", binary, NULL); g_setenv("NOSTR_SIGNER_TEST_DEFAULT_GRANTS", defaults, TRUE);
  g_autofree char *home = g_dir_make_tmp("grotto-profile-integration-XXXXXX", NULL); g_assert_nonnull(home);
  g_autofree char *config = g_build_filename(home, "config", NULL), *cache = g_build_filename(home, "cache", NULL);
  g_setenv("XDG_CONFIG_HOME", config, TRUE); g_setenv("XDG_CACHE_HOME", cache, TRUE);
  g_setenv("GSETTINGS_BACKEND", "memory", TRUE);
  private_bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NONE); nostrc_test_bus_up(private_bus);
  adw_init(); g_object_set(gtk_settings_get_default(), "gtk-enable-animations", FALSE, NULL);
  g_test_add_func("/signer/profile/save-one-review-real-daemon", profile_integration);
  int result = g_test_run(); nostrc_test_bus_down(private_bus);
  g_autoptr(GFile) owned = g_file_new_for_path(home); remove_owned_tree(owned); return result;
}
