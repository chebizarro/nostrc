/* SPDX-License-Identifier: MIT */
#include <grotto-panels.h>
#include "ui/approval_dialog.h"
#include "relay_store.h"
#include "bunker_test_relay.h"
#include "ui/signer-window.h"
#include "session-manager.h"
#include "grotto-profile-service.h"
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
typedef struct { GDBusConnection *bus; guint reviews, decisions, completed; char *request, *reviewed; const char *relay; gboolean cancel_next, last_allowed; } Approval;
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
  g_autoptr(GVariant) summary = g_variant_lookup_value(info, "event_summary", G_VARIANT_TYPE_VARDICT);
  const char *raw = NULL, *reason = NULL;
  g_assert_true(g_variant_lookup(summary, "management_event_json", "&s", &raw)); a->reviewed = g_strdup(raw);
  g_assert_true(g_variant_lookup(info, "reason", "&s", &reason)); g_assert_nonnull(strstr(reason, a->relay));
  GnostrApprovalDialog *dialog = g_object_ref_sink(gnostr_approval_dialog_new());
  gnostr_approval_dialog_set_info(dialog, info, "Grotto");
  const char *label = "", *identity = "";
  g_variant_lookup(info, "identity_label", "&s", &label); g_variant_lookup(info, "identity", "&s", &identity);
  g_autofree char *headline = g_strdup_printf("Publish profile for %s", *label ? label : identity);
  GtkLabel *title = GTK_LABEL(gtk_widget_get_template_child(GTK_WIDGET(dialog), GNOSTR_TYPE_APPROVAL_DIALOG, "header_title"));
  g_assert_cmpstr(gtk_label_get_text(title), ==, headline);
  GtkLabel *preview = GTK_LABEL(gtk_widget_get_template_child(GTK_WIDGET(dialog), GNOSTR_TYPE_APPROVAL_DIALOG, "content_preview"));
  g_assert_nonnull(strstr(gtk_label_get_text(preview), "alice")); g_assert_nonnull(strstr(gtk_label_get_text(preview), "extension"));
  GtkLabel *details = GTK_LABEL(gtk_widget_get_template_child(GTK_WIDGET(dialog), GNOSTR_TYPE_APPROVAL_DIALOG, "label_details"));
  g_autoptr(JsonNode) exact = json_from_string(raw, NULL);
  g_assert_false(gtk_label_get_use_markup(preview)); g_assert_false(gtk_label_get_use_markup(details));
  g_assert_nonnull(strstr(gtk_label_get_text(details), raw)); g_assert_nonnull(strstr(gtk_label_get_text(details), "Exact profile JSON:"));
  AdwActionRow *impact = ADW_ACTION_ROW(gtk_widget_get_template_child(GTK_WIDGET(dialog), GNOSTR_TYPE_APPROVAL_DIALOG, "row_impact"));
  g_assert_nonnull(strstr(adw_action_row_get_subtitle(impact), "Grotto to publish")); g_assert_null(strstr(adw_action_row_get_subtitle(impact), "does not publish"));
  AdwActionRow *relays = ADW_ACTION_ROW(gtk_widget_get_template_child(GTK_WIDGET(dialog), GNOSTR_TYPE_APPROVAL_DIALOG, "row_relay"));
  g_assert_nonnull(strstr(adw_action_row_get_subtitle(relays), a->relay));
  g_assert_nonnull(strstr(gtk_label_get_text(details), json_object_get_string_member(json_node_get_object(exact), "content")));
  g_assert_true(gnostr_approval_info_can_approve(info)); g_object_unref(dialog);
  g_dbus_connection_call(a->bus, "org.nostr.Signer", "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1", "DecideRequest", g_variant_new("(ss)", a->request, "allow-24h"), NULL, 0, 10000, NULL, reusable_refused, a);
}
static void review(GDBusConnection *bus, const char *sender, const char *path, const char *interface, const char *signal, GVariant *args, gpointer data) {
  (void)bus; (void)sender; (void)path; (void)interface; (void)signal;
  Approval *a = data; a->reviews++;
  if (a->cancel_next) {
    g_assert_cmpuint(a->reviews, ==, 2);
    AdwDialog *dialog = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(diagnostic_window));
    g_assert_nonnull(dialog); g_signal_emit_by_name(find(GTK_WIDGET(dialog), "Stop"), "clicked"); return;
  }
  g_assert_cmpuint(a->reviews, ==, 1);
  g_autoptr(GVariant) id = g_variant_get_child_value(args, 4); a->request = g_variant_dup_string(id, NULL);
  g_dbus_connection_call(a->bus, "org.nostr.Signer", "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1", "ListRequests", NULL, NULL, 0, 10000, NULL, info_done, a);
}
static void completed(GDBusConnection *bus, const char *sender, const char *path, const char *interface, const char *signal, GVariant *args, gpointer data) {
  (void)bus; (void)sender; (void)path; (void)interface; (void)signal;
  Approval *a = data; const char *id; g_variant_get(args, "(&sb)", &id, &a->last_allowed); a->completed++;
}
static void daemon_ready(GDBusConnection *bus, const char *name, const char *owner, gpointer data) { (void)bus; (void)name; (void)owner; *(gboolean *)data = TRUE; }
static void host_result(GObject *source, GAsyncResult *result, gpointer data) { (void)source; *(GAsyncResult **)data = g_object_ref(result); }
static void invalid_profile(GDBusConnection *bus, const char *npub, const char *relay, const char *raw) {
  const char *destinations[] = {relay, NULL}; g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_sync(bus, "org.nostr.Signer", "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1", "SignProfileEvent", g_variant_new("(ss^as)", npub, raw, destinations), NULL, 0, 10000, NULL, &error);
  g_assert_null(reply); g_assert_nonnull(error); g_autofree char *name = g_dbus_error_get_remote_error(error);
  g_assert_cmpstr(name, ==, "org.nostr.Grotto.Control1.Error.InvalidInput");
}
static gboolean image_contact(GSocketService *service, GSocketConnection *connection, GObject *source, gpointer data) {
  (void)service; (void)source; (*(guint *)data)++; g_io_stream_close(G_IO_STREAM(connection), NULL, NULL); return TRUE;
}
static void seed_profiles(BunkerTestRelay *relay, const char *hex, const char *key, const char *image) {
  for (guint i = 0; i < 2; i++) {
    NostrEvent *event = nostr_event_new(); nostr_event_set_kind(event, 0); nostr_event_set_pubkey(event, hex);
    nostr_event_set_created_at(event, g_get_real_time() / G_USEC_PER_SEC - (i ? 10 : 20));
    g_autofree char *text = i ? g_strdup_printf("{\"display_name\":\"Newest\",\"extension\":{\"keep\":true},\"picture\":\"%s\"}", image) : g_strdup("{\"display_name\":\"Older\"}");
    nostr_event_set_content(event, text);
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
  /* Missing configuration must not bootstrap via schema-default relays. */
  GAsyncResult *empty = NULL;
  grotto_profile_service_async(NULL, GROTTO_PROFILE_FETCH, npub, NULL, NULL, host_result, &empty); WAIT(empty);
  g_autoptr(GError) empty_error = NULL;
  GrottoProfileInfo *missing = g_task_propagate_pointer(G_TASK(empty), &empty_error); g_object_unref(empty);
  g_assert_null(missing); g_assert_error(empty_error, G_IO_ERROR, G_IO_ERROR_HOST_UNREACHABLE);
  guint image_contacts = 0;
  g_autoptr(GSocketService) image_server = g_socket_service_new();
  g_autoptr(GSocketAddress) address = g_inet_socket_address_new_from_string("127.0.0.1", 0); GSocketAddress *effective = NULL;
  g_assert_true(g_socket_listener_add_address(G_SOCKET_LISTENER(image_server), address, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP, NULL, &effective, NULL));
  g_autofree char *image = g_strdup_printf("https://127.0.0.1:%u/tracker.png", g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(effective))); g_object_unref(effective);
  g_signal_connect(image_server, "incoming", G_CALLBACK(image_contact), &image_contacts); g_socket_service_start(image_server);
  BunkerTestRelay *relay = bunker_test_relay_new(); seed_profiles(relay, hex, sk, image);
  RelayStore *store = relay_store_new_for_identity(npub); relay_store_add(store, bunker_test_relay_url(relay), TRUE, TRUE); relay_store_save(store); relay_store_free(store);
  g_setenv("NOSTR_SIGNER_SECKEY_HEX", sk, TRUE);
  AccountsStore *accounts = accounts_store_get_default(); g_assert_true(accounts_store_add(accounts, npub, "Everyday", NULL));
  g_settings_set_string(settings_manager_get_gsettings(settings_manager_get_default()), "default-identity", npub);
  gboolean ready = FALSE; guint watch = g_bus_watch_name_on_connection(connection, "org.nostr.Signer", 0, daemon_ready, NULL, &ready, NULL);
  guint gui_name = g_bus_own_name_on_connection(connection, "org.nostr.Grotto", 0, NULL, NULL, NULL, NULL);
  Approval approval = {.bus = connection, .relay = bunker_test_relay_url(relay)};
  guint completions = g_dbus_connection_signal_subscribe(connection, "org.nostr.Signer", "org.nostr.Signer", "ApprovalCompleted", "/org/nostr/signer", NULL, 0, completed, &approval, NULL);
  guint subscription = g_dbus_connection_signal_subscribe(connection, "org.nostr.Signer", "org.nostr.Signer", "ApprovalRequested", "/org/nostr/signer", NULL, 0, review, &approval, NULL);
  const char *command[] = {PROFILE_DAEMON_BIN, NULL};
  GSubprocess *daemon = nostrc_test_bus_spawn_supervised(bus, "profile-daemon.log", NULL, command); g_assert_nonnull(daemon); WAIT(ready); g_bus_unwatch_name(watch);
  g_autofree char *mismatch = g_strdup_printf("{\"kind\":0,\"pubkey\":\"%064d\",\"created_at\":1,\"tags\":[],\"content\":\"{}\"}", 0);
  invalid_profile(connection, npub, bunker_test_relay_url(relay), mismatch);
  g_autofree char *tagged = g_strdup_printf("{\"kind\":0,\"pubkey\":\"%s\",\"created_at\":1,\"tags\":[[\"t\",\"smuggled\"]],\"content\":\"{}\"}", hex);
  invalid_profile(connection, npub, bunker_test_relay_url(relay), tagged);
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
  g_assert_cmpuint(image_contacts, ==, 0);
  g_signal_emit_by_name(find(panel, "Load Profile Images…"), "clicked");
  AdwDialog *consent = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  g_assert_true(ADW_IS_ALERT_DIALOG(consent));
  g_assert_nonnull(strstr(adw_alert_dialog_get_body(ADW_ALERT_DIALOG(consent)), "watch-only"));
  g_assert_nonnull(strstr(adw_alert_dialog_get_body(ADW_ALERT_DIALOG(consent)), image));
  guint consent_frames = 0; gtk_widget_add_tick_callback(GTK_WIDGET(consent), presented, &consent_frames, NULL); WAIT(consent_frames == 3);
  g_signal_emit_by_name(find(GTK_WIDGET(consent), "Cancel"), "clicked");
  WAIT(!adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)) && gtk_widget_is_sensitive(find(panel, "Edit Profile…")));
  g_assert_cmpuint(image_contacts, ==, 0);
  g_signal_emit_by_name(find(panel, "Load Profile Images…"), "clicked");
  consent = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  consent_frames = 0; gtk_widget_add_tick_callback(GTK_WIDGET(consent), presented, &consent_frames, NULL); WAIT(consent_frames == 3);
  g_signal_emit_by_name(find(GTK_WIDGET(consent), "Load Images"), "clicked");
  WAIT(find(panel, "Some profile images could not be loaded. Check the URLs or try again"));
  g_assert_cmpuint(image_contacts, >, 0); guint consented_contacts = image_contacts;
  g_signal_emit_by_name(find(panel, "Edit Profile…"), "clicked");
  AdwDialog *dialog = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)); g_assert_nonnull(dialog);
  guint frames = 0; gtk_widget_add_tick_callback(GTK_WIDGET(dialog), presented, &frames, NULL); WAIT(frames == 3);
  gtk_editable_set_text(GTK_EDITABLE(find(GTK_WIDGET(dialog), "Name")), "alice");
  gtk_editable_set_text(GTK_EDITABLE(find(GTK_WIDGET(dialog), "Display Name")), "Alice");
  GtkWidget *save = find(GTK_WIDGET(dialog), "Save"); g_assert_true(gtk_widget_is_sensitive(save));
  GrottoPanelsService *service = grotto_panel_get_service(GROTTO_PANEL(panel));
  /* Opening the real form can overlap a daemon snapshot refresh. Wait for
   * current authority, not only the last rendered button sensitivity. */
  WAIT(grotto_panels_service_can(service, GROTTO_PANEL_SET_IDENTITY_LABEL));
  g_assert_false(gn_session_manager_is_locked(gn_session_manager_get_default()));
  g_signal_emit_by_name(save, "clicked");
  WAIT(approval.decisions == 1 && !adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
  g_assert_cmpuint(approval.reviews, ==, 1);
  JsonArray *events = bunker_test_relay_events(relay); g_assert_cmpuint(json_array_get_length(events), ==, 4);
  g_autofree char *json = json_to_string(json_array_get_element(events, 3), FALSE);
  NostrEvent *event = nostr_event_new(); g_assert_cmpint(nostr_event_deserialize_signed(event, json, NULL), ==, NOSTR_EVENT_VALIDATION_OK);
  g_assert_cmpint(nostr_event_validate(event, NULL), ==, NOSTR_EVENT_VALIDATION_OK); g_assert_cmpint(nostr_event_get_kind(event), ==, 0); g_assert_cmpstr(nostr_event_get_pubkey(event), ==, hex);
  g_assert_nonnull(strstr(nostr_event_get_content(event), "alice"));
  NostrEvent *reviewed = nostr_event_new(); char reviewed_id[65]; g_assert_cmpint(nostr_event_deserialize(reviewed, approval.reviewed), ==, 0);
  g_assert_cmpstr(nostr_event_get_content(event), ==, nostr_event_get_content(reviewed));
  g_assert_cmpint(nostr_event_compute_id(reviewed, reviewed_id), ==, NOSTR_EVENT_VALIDATION_OK);
  g_assert_cmpstr(nostr_event_get_id(event), ==, reviewed_id); g_assert_cmpuint(nostr_tags_size(nostr_event_get_tags(event)), ==, 0); nostr_event_free(reviewed);
  g_autoptr(JsonParser) content = json_parser_new(); g_assert_true(json_parser_load_from_data(content, nostr_event_get_content(event), -1, NULL));
  g_assert_true(json_object_get_boolean_member(json_object_get_object_member(json_node_get_object(json_parser_get_root(content)), "extension"), "keep"));
  g_test_message("Published kind-0 event %s after exactly one review", nostr_event_get_id(event)); nostr_event_free(event);
  g_autofree char *cache = g_strdup_printf("%s/grotto/profiles/%s-0.json", g_get_user_cache_dir(), hex);
  g_assert_true(g_file_test(cache, G_FILE_TEST_IS_REGULAR));
  g_assert_nonnull(find(panel, "Alice"));
  g_assert_cmpuint(image_contacts, ==, consented_contacts); /* Save/review do not resolve URLs. */
  guint connections, requests, published; bunker_test_relay_stats(relay, &connections, &requests, &published);
  g_assert_cmpuint(requests, >=, 3); /* initial fetch, Save discovery and post-publish EOSE re-fetch */
  /* Stop a second Save at the real daemon review, without signing/publishing. */
  WAIT(approval.completed == 1); approval.cancel_next = TRUE;
  g_signal_emit_by_name(find(panel, "Edit Profile…"), "clicked");
  dialog = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  frames = 0; gtk_widget_add_tick_callback(GTK_WIDGET(dialog), presented, &frames, NULL); WAIT(frames == 3);
  WAIT(grotto_panels_service_can(service, GROTTO_PANEL_SET_IDENTITY_LABEL));
  g_signal_emit_by_name(find(GTK_WIDGET(dialog), "Save"), "clicked");
  WAIT(approval.completed == 2 && find(GTK_WIDGET(dialog), "Stopped. Data may already have reached a server or relay. Refresh before saving again"));
  g_assert_false(approval.last_allowed); g_assert_cmpuint(json_array_get_length(events), ==, 4);
  g_signal_emit_by_name(find(GTK_WIDGET(dialog), "Cancel"), "clicked");
  WAIT(!adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
  /* Offline cache is the same host path, with no synthetic cached result. */
  bunker_test_relay_free(relay); relay = NULL;
  g_signal_emit_by_name(find(panel, "Refresh Profile"), "clicked"); WAIT(find(panel, "Offline — showing the saved profile"));
  g_assert_nonnull(find(panel, "Alice")); g_assert_cmpuint(approval.reviews, ==, 2);
  g_socket_service_stop(image_server);
  diagnostic_window = NULL;
  gtk_window_destroy(GTK_WINDOW(window)); g_object_unref(window); WAIT(window_gone);
  g_dbus_connection_signal_unsubscribe(connection, subscription); g_dbus_connection_signal_unsubscribe(connection, completions); g_bus_unown_name(gui_name);
  g_subprocess_send_signal(daemon, SIGTERM); g_assert_true(g_subprocess_wait_check(daemon, NULL, NULL));
  accounts_store_free(accounts_store_get_default()); settings_manager_free(settings_manager_get_default());
  g_free(approval.reviewed); g_free(approval.request); g_free(npub); g_application_quit(application);
}
static void authorization_preview(void) {
  const char *hex = "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798";
  guint8 pk[32]; for (guint i = 0; i < 32; i++) { char byte[3] = {hex[i*2], hex[i*2+1], 0}; pk[i] = g_ascii_strtoull(byte, NULL, 16); }
  char *npub = NULL; g_assert_cmpint(nostr_nip19_encode_npub(pk, &npub), ==, 0);
  for (guint i = 0; i < 2; i++) {
    guint kind = i ? 22242 : 24242;
    g_autofree char *tags = i ? g_strdup("[[\"relay\",\"wss://private.example/\"],[\"challenge\",\"challenge-fixture\"]]") :
      g_strdup_printf("[[\"t\",\"upload\"],[\"x\",\"%064d\"],[\"expiration\",\"%" G_GINT64_FORMAT "\"]]", 1, g_get_real_time()/G_USEC_PER_SEC+300);
    g_autofree char *raw = g_strdup_printf("{\"kind\":%u,\"pubkey\":\"%s\",\"created_at\":1,\"content\":\"\",\"tags\":%s}", kind, hex, tags);
    GVariantBuilder summary, info; g_variant_builder_init(&summary, G_VARIANT_TYPE_VARDICT); g_variant_builder_init(&info, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&summary, "{sv}", "management_event_json", g_variant_new_string(raw));
    if (i) g_variant_builder_add(&summary, "{sv}", "auth_relay", g_variant_new_string("wss://private.example/"));
    const char *keys[] = {"operation", "transport", "purpose", "identity", "reason"};
    const char *values[] = {"sign_event", "MANAGEMENT", "profile-management", npub, i ? "wss://private.example/" : "https://images.example/\nhttps://fallback.example/"};
    for (guint k = 0; k < G_N_ELEMENTS(keys); k++) g_variant_builder_add(&info, "{sv}", keys[k], g_variant_new_string(values[k]));
    g_variant_builder_add(&info, "{sv}", "event_kind", g_variant_new_uint32(kind)); g_variant_builder_add(&info, "{sv}", "event_summary", g_variant_builder_end(&summary));
    g_autoptr(GVariant) snapshot = g_variant_ref_sink(g_variant_builder_end(&info)); g_assert_true(gnostr_approval_info_can_approve(snapshot));
    GnostrApprovalDialog *dialog = g_object_ref_sink(gnostr_approval_dialog_new()); gnostr_approval_dialog_set_info(dialog, snapshot, "Grotto");
    GtkLabel *preview = GTK_LABEL(gtk_widget_get_template_child(GTK_WIDGET(dialog), GNOSTR_TYPE_APPROVAL_DIALOG, "content_preview"));
    const char *text = gtk_label_get_text(preview);
    if (i) { g_assert_nonnull(strstr(text, "challenge-fixture")); g_assert_nonnull(strstr(text, "wss://private.example/")); }
    else { g_assert_nonnull(strstr(text, "Action: upload")); g_assert_nonnull(strstr(text, "Blob SHA-256: ")); g_assert_nonnull(strstr(text, "Expires (UTC)")); g_assert_nonnull(strstr(text, "No server restriction")); }
    AdwActionRow *impact = ADW_ACTION_ROW(gtk_widget_get_template_child(GTK_WIDGET(dialog), GNOSTR_TYPE_APPROVAL_DIALOG, "row_impact"));
    g_assert_null(strstr(adw_action_row_get_subtitle(impact), "does not publish anything"));
    g_object_unref(dialog);
    GVariantDict invalid; g_variant_dict_init(&invalid, snapshot); g_variant_dict_insert(&invalid, "event_kind", "u", 1u);
    g_autoptr(GVariant) wrong = g_variant_ref_sink(g_variant_dict_end(&invalid)); g_assert_false(gnostr_approval_info_can_approve(wrong));
  }
  g_free(npub);
}
static void privacy_helpers(void) {
  g_autoptr(GdkPixbuf) source = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 2, 2); gdk_pixbuf_fill(source, 0x7faaffff);
  char *data = NULL; gsize length = 0;
  g_assert_true(gdk_pixbuf_save_to_buffer(source, &data, &length, "png", NULL, "tEXt::Comment", "GPS location metadata fixture", NULL));
  g_autoptr(GBytes) bytes = g_bytes_new_take(data, length);
  g_autoptr(GInputStream) stream = g_memory_input_stream_new_from_bytes(bytes);
  g_autoptr(GdkPixbuf) metadata = gdk_pixbuf_new_from_stream(stream, NULL, NULL);
  g_assert_cmpstr(gdk_pixbuf_get_option(metadata, "tEXt::Comment"), ==, "GPS location metadata fixture");
  g_autoptr(GdkTexture) preview = grotto_profile_decode_image(bytes, NULL, NULL); g_assert_nonnull(preview);
  const char *servers[] = {"https://images.example/", "https://fallback.example/", NULL};
  AdwAlertDialog *upload = g_object_ref_sink(grotto_profile_upload_confirmation(preview, FALSE, servers));
  const char *body = adw_alert_dialog_get_body(upload);
  g_assert_nonnull(strstr(body, "EXIF")); g_assert_nonnull(strstr(body, "unchanged")); g_assert_nonnull(strstr(body, "public immediately"));
  g_assert_nonnull(strstr(body, "will not delete")); g_assert_nonnull(strstr(body, servers[0])); g_assert_nonnull(strstr(body, servers[1]));
  g_assert_cmpstr(adw_alert_dialog_get_default_response(upload), ==, "cancel"); g_object_unref(upload);
  g_autoptr(GBytes) svg = g_bytes_new_static("<svg/>", 6); g_autoptr(GError) error = NULL;
  g_assert_null(grotto_profile_decode_image(svg, NULL, &error)); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA); g_clear_error(&error);
  g_autoptr(GBytes) huge = g_bytes_new_take(g_malloc0(5*1024*1024+1), 5*1024*1024+1);
  g_assert_null(grotto_profile_decode_image(huge, NULL, &error)); g_assert_nonnull(error); g_clear_error(&error);
  g_autoptr(GdkPixbuf) wide = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 8193, 1); gdk_pixbuf_fill(wide, 0);
  data = NULL; g_assert_true(gdk_pixbuf_save_to_buffer(wide, &data, &length, "png", NULL, NULL));
  g_autoptr(GBytes) wide_bytes = g_bytes_new_take(data, length);
  g_assert_null(grotto_profile_decode_image(wide_bytes, NULL, &error)); g_assert_nonnull(error);
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
  g_test_add_func("/signer/profile/authorization-preview", authorization_preview);
  g_test_add_func("/signer/profile/image-privacy-and-limits", privacy_helpers);
  g_test_add_func("/signer/profile/save-one-review-real-daemon", profile_integration);
  int result = g_test_run(); nostrc_test_bus_down(private_bus);
  g_autoptr(GFile) owned = g_file_new_for_path(home); remove_owned_tree(owned); return result;
}
