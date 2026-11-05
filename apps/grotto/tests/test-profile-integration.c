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
typedef struct { GDBusConnection *bus; guint reviews, decisions, completed; char *request, *reviewed; const char *relay, *discovery; GrottoApprovalController *controller; gboolean cancel_next, last_allowed, fallback; } Approval;
static void decision_done(GObject *source, GAsyncResult *result, gpointer data) {
  Approval *a = data; g_autoptr(GError) error = NULL;
  gboolean ok = grotto_approval_controller_decide_finish(GROTTO_APPROVAL_CONTROLLER(source), result, &error);
  g_assert_no_error(error); g_assert_true(ok); a->decisions++;
}
static void reusable_refused(GObject *source, GAsyncResult *result, gpointer data) {
  Approval *a = data; g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
  g_assert_null(reply); g_assert_nonnull(error);
  g_autofree char *name = g_dbus_error_get_remote_error(error); g_assert_cmpstr(name, ==, "org.nostr.Signer.Error.InvalidInput");
  g_assert_nonnull(strstr(error->message, "Management requires one-time consent"));
  grotto_approval_controller_decide_async(a->controller, a->request, TRUE, FALSE, 0, NULL, decision_done, a);
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
  g_assert_true(g_variant_lookup(summary, "management_event_json", "&s", &raw)); g_free(a->reviewed); a->reviewed = g_strdup(raw);
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
  g_assert_cmpstr(adw_action_row_get_subtitle(relays), ==, "destinations: see Details");
  g_assert_nonnull(strstr(gtk_label_get_text(details), a->relay));
  g_assert_nonnull(strstr(gtk_label_get_text(details), json_object_get_string_member(json_node_get_object(exact), "content")));
  g_assert_true(gnostr_approval_info_can_approve(info)); g_object_unref(dialog);
  grotto_approval_controller_review(a->controller, a->request);
  AdwDialog *shown = NULL;
  WAIT((shown = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(diagnostic_window))) && GNOSTR_IS_APPROVAL_DIALOG(shown));
  guint review_frames = 0; gtk_widget_add_tick_callback(GTK_WIDGET(shown), presented, &review_frames, NULL); WAIT(review_frames == 3);
  AdwActionRow *shown_relays = ADW_ACTION_ROW(gtk_widget_get_template_child(GTK_WIDGET(shown), GNOSTR_TYPE_APPROVAL_DIALOG, "row_relay"));
  GtkLabel *shown_details = GTK_LABEL(gtk_widget_get_template_child(GTK_WIDGET(shown), GNOSTR_TYPE_APPROVAL_DIALOG, "label_details"));
  if (a->fallback) {
    g_assert_cmpstr(adw_action_row_get_subtitle(shown_relays), ==, "destinations: see Details");
    g_assert_nonnull(strstr(gtk_label_get_text(shown_details), a->relay));
    GtkWidget *approve = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(shown), GNOSTR_TYPE_APPROVAL_DIALOG, "btn_approve"));
    g_assert_true(gtk_widget_get_sensitive(approve));
  } else {
    WAIT(strstr(adw_action_row_get_subtitle(shown_relays), a->discovery));
    g_assert_nonnull(strstr(adw_action_row_get_subtitle(shown_relays), a->relay));
    g_assert_nonnull(strstr(adw_action_row_get_subtitle(shown_relays), "lookup only, not publication destinations or authorization"));
    g_assert_nonnull(strstr(gtk_label_get_text(shown_details), a->discovery));
  }
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
  g_assert_cmpuint(a->reviews, ==, a->fallback ? 3 : 1);
  g_autoptr(GVariant) id = g_variant_get_child_value(args, 4); g_free(a->request); a->request = g_variant_dup_string(id, NULL);
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
typedef struct { const char *hex; guint requests; gboolean challenge; BunkerTestRelay *relay; } BootstrapProbe;
static void forbidden_bootstrap_publish(const char *event, gpointer data) { (void)event; (void)data; g_error("Publication must never use bootstrap fallback"); }
static void bootstrap_request(JsonArray *request, gpointer data) {
  BootstrapProbe *p = data; p->requests++;
  g_assert_cmpuint(json_array_get_length(request), ==, 5);
  const guint kinds[] = {0, 10002, 10050};
  for (guint i = 0; i < 3; i++) {
    JsonObject *filter = json_array_get_object_element(request, i + 2);
    g_assert_cmpint(json_array_get_int_element(json_object_get_array_member(filter, "kinds"), 0), ==, kinds[i]);
    g_assert_cmpstr(json_array_get_string_element(json_object_get_array_member(filter, "authors"), 0), ==, p->hex);
    g_assert_cmpint(json_object_get_int_member(filter, "limit"), ==, 1);
  }
  if (p->challenge) bunker_test_relay_challenge(p->relay);
}
static void unexpected_signing(GDBusConnection *bus, const char *sender, const char *path, const char *iface, const char *method, GVariant *args, GDBusMethodInvocation *inv, gpointer data) {
  (void)bus; (void)sender; (void)path; (void)iface; (void)method; (void)args; (*(guint *)data)++;
  g_dbus_method_invocation_return_dbus_error(inv, "org.nostr.Signer.Error.Denied", "bootstrap must never ask for a signature");
}
static void bootstrap_auth_refused(GDBusConnection *bus, const char *npub, BunkerTestRelay *relay, BootstrapProbe *probe) {
  guint sign_requests = 0;
  const char *xml = "<node><interface name='org.nostr.Grotto.Control1'>"
    "<method name='SignProfileEvent'><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='as' direction='in'/><arg type='s' direction='out'/></method>"
    "</interface></node>";
  g_autoptr(GDBusNodeInfo) node = g_dbus_node_info_new_for_xml(xml, NULL);
  const GDBusInterfaceVTable methods = {.method_call = unexpected_signing};
  guint object = g_dbus_connection_register_object(bus, "/org/nostr/signer/grotto", node->interfaces[0], &methods, &sign_requests, NULL, NULL); g_assert_cmpuint(object, >, 0);
  g_autoptr(GVariant) owned = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName", g_variant_new("(su)", "org.nostr.Signer", 4u), G_VARIANT_TYPE("(u)"), 0, 10000, NULL, NULL);
  g_assert_nonnull(owned); guint owner; g_variant_get(owned, "(u)", &owner); g_assert_cmpuint(owner, ==, 1);
  g_autoptr(GrottoProfileInfo) draft = grotto_profile_info_copy(NULL);
  for (guint closed = 0; closed < 2; closed++) {
    probe->challenge = !closed; bunker_test_relay_reject_subscriptions(relay, closed);
    for (guint save = 0; save < 2; save++) {
      GAsyncResult *result = NULL; g_autoptr(GError) error = NULL;
      grotto_profile_service_async(NULL, NULL, save ? GROTTO_PROFILE_SAVE : GROTTO_PROFILE_FETCH, npub, save ? draft : NULL, NULL, host_result, &result); WAIT(result);
      g_assert_null(g_task_propagate_pointer(G_TASK(result), &error)); g_object_unref(result);
      g_assert_error(error, G_IO_ERROR, G_IO_ERROR_HOST_UNREACHABLE);
      g_assert_nonnull(strstr(error->message, "relay requires authentication"));
      g_assert_cmpuint(sign_requests, ==, 0); g_assert_cmpuint(bunker_test_relay_auth_count(relay), ==, 0);
    }
  }
  probe->challenge = FALSE; bunker_test_relay_reject_subscriptions(relay, FALSE);
  g_autoptr(GVariant) released = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "ReleaseName", g_variant_new("(s)", "org.nostr.Signer"), G_VARIANT_TYPE("(u)"), 0, 10000, NULL, NULL);
  g_assert_nonnull(released); g_dbus_connection_unregister_object(bus, object);
}
static void seed_metadata(BunkerTestRelay *relay, const char *hex, const char *key, guint kind, const char *tags) {
  g_autofree char *raw = g_strdup_printf("{\"pubkey\":\"%s\",\"kind\":%u,\"created_at\":1,\"content\":\"\",\"tags\":%s}", hex, kind, tags);
  NostrEvent *event = nostr_event_new(); g_assert_cmpint(nostr_event_deserialize(event, raw), ==, 0);
  g_assert_cmpint(nostr_event_sign(event, key), ==, 0);
  g_autofree char *signed_json = nostr_event_serialize_compact(event); nostr_event_free(event);
  json_array_add_element(bunker_test_relay_events(relay), json_from_string(signed_json, NULL));
}
#ifdef __linux__
static void crop_picker_cancel(SignerWindow *window, AdwDialog *editor) {
  g_autoptr(GdkPixbuf) pixels = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 64, 32); gdk_pixbuf_fill(pixels, 0x7faaffff);
  g_autofree char *path = g_build_filename(g_get_user_cache_dir(), "crop-fixture.png", NULL);
  g_assert_true(gdk_pixbuf_save(pixels, path, "png", NULL, "tEXt::Comment", "private metadata fixture", NULL));
  g_signal_emit_by_name(find(GTK_WIDGET(editor), "Choose Avatar…"), "clicked");
  GtkFileChooser *chooser = NULL; GListModel *windows = gtk_window_get_toplevels();
  guint timer = g_timeout_add_seconds(30, timeout, "real crop file chooser");
  while (!chooser) {
    for (guint i = 0; i < g_list_model_get_n_items(windows); i++) {
      GtkWidget *candidate = g_list_model_get_item(windows, i);
      if (GTK_IS_FILE_CHOOSER(candidate) && gtk_widget_get_mapped(candidate) && gtk_window_get_transient_for(GTK_WINDOW(candidate)) == GTK_WINDOW(window)) chooser = GTK_FILE_CHOOSER(candidate);
      else g_object_unref(candidate);
      if (chooser) break;
    }
    if (!chooser) g_main_context_iteration(NULL, TRUE);
  }
  g_source_remove(timer);
  g_autoptr(GFile) file = g_file_new_for_path(path); g_assert_true(gtk_file_chooser_set_file(chooser, file, NULL));
  guint frames = 0; gtk_widget_add_tick_callback(GTK_WIDGET(chooser), presented, &frames, NULL); WAIT(frames == 3);
  GtkWidget *accept = gtk_dialog_get_widget_for_response(GTK_DIALOG(chooser), GTK_RESPONSE_ACCEPT); WAIT(gtk_widget_is_sensitive(accept));
  g_signal_emit_by_name(accept, "clicked"); g_object_unref(chooser);
  AdwDialog *visible = NULL;
  WAIT((visible = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window))) && ADW_IS_ALERT_DIALOG(visible));
  g_assert_cmpstr(adw_alert_dialog_get_heading(ADW_ALERT_DIALOG(visible)), ==, "Crop and Upload Profile Image?");
  GtkWidget *box = adw_alert_dialog_get_extra_child(ADW_ALERT_DIALOG(visible));
  guint ranges = 0;
  for (GtkWidget *child = gtk_widget_get_first_child(box); child; child = gtk_widget_get_next_sibling(child))
    if (GTK_IS_RANGE(child)) { gtk_range_set_value(GTK_RANGE(child), ranges++ ? 1 : 2); }
  g_assert_cmpuint(ranges, ==, 3);
  GtkWidget *picture = gtk_widget_get_first_child(box); g_assert_true(GTK_IS_PICTURE(picture));
  GdkPaintable *crop = gtk_picture_get_paintable(GTK_PICTURE(picture)); g_assert_true(GDK_IS_TEXTURE(crop));
  g_assert_cmpint(gdk_texture_get_width(GDK_TEXTURE(crop)), ==, 16); g_assert_cmpint(gdk_texture_get_height(GDK_TEXTURE(crop)), ==, 16);
  frames = 0; gtk_widget_add_tick_callback(GTK_WIDGET(visible), presented, &frames, NULL); WAIT(frames == 3);
  g_signal_emit_by_name(find(GTK_WIDGET(visible), "Cancel"), "clicked");
  WAIT(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)) == editor && gtk_widget_is_sensitive(find(GTK_WIDGET(editor), "Save")));
  g_assert_cmpint(g_remove(path), ==, 0);
}
#endif
static void profile_activate(GApplication *application, gpointer data) {
  (void)data; AdwApplication *app = ADW_APPLICATION(application);
  nostrc_test_bus_tolerate_ebadf(); NostrcTestBus *bus = private_bus;
  GDBusConnection *connection = nostrc_test_bus_connect(bus);
  const char *sk = "0000000000000000000000000000000000000000000000000000000000000001";
  g_autofree char *hex = nostr_key_get_public(sk); guint8 pk[32];
  for (guint i = 0; i < 32; i++) { char byte[3] = {hex[2*i], hex[2*i+1], 0}; pk[i] = g_ascii_strtoull(byte, NULL, 16); }
  char *npub = NULL; g_assert_cmpint(nostr_nip19_encode_npub(pk, &npub), ==, 0);
  GSettings *preferences = settings_manager_get_gsettings(settings_manager_get_default());
  g_settings_set_boolean(preferences, "profile-bootstrap-enabled", FALSE);
  /* Opt-out: empty configuration makes no network connection and remains editable. */
  GAsyncResult *empty = NULL;
  grotto_profile_service_async(NULL, NULL, GROTTO_PROFILE_FETCH, npub, NULL, NULL, host_result, &empty); WAIT(empty);
  g_autoptr(GError) empty_error = NULL;
  GrottoProfileInfo *missing = g_task_propagate_pointer(G_TASK(empty), &empty_error); g_object_unref(empty);
  g_assert_no_error(empty_error); g_assert_nonnull(missing); g_assert_true(missing->no_relays); g_assert_false(missing->published); grotto_profile_info_free(missing);
  g_autoptr(GrottoProfileInfo) invalid_address = grotto_profile_info_copy(NULL);
  g_free(invalid_address->fields[GROTTO_PROFILE_NIP05]); invalid_address->fields[GROTTO_PROFILE_NIP05] = g_strdup("Invalid@example.com");
  empty = NULL; grotto_profile_service_async(NULL, NULL, GROTTO_PROFILE_VERIFY_NIP05, npub, invalid_address, NULL, host_result, &empty); WAIT(empty);
  GrottoProfileInfo *checked = g_task_propagate_pointer(G_TASK(empty), &empty_error); g_object_unref(empty);
  g_assert_no_error(empty_error); g_assert_cmpint(checked->nip05_status, ==, GROTTO_NIP05_INVALID); grotto_profile_info_free(checked);
  g_settings_set_boolean(preferences, "use-tor-for-relays", TRUE);
  empty = NULL; grotto_profile_service_async(NULL, NULL, GROTTO_PROFILE_VERIFY_NIP05, npub, invalid_address, NULL, host_result, &empty); WAIT(empty);
  checked = g_task_propagate_pointer(G_TASK(empty), &empty_error); g_object_unref(empty);
  g_assert_no_error(empty_error); g_assert_cmpint(checked->nip05_status, ==, GROTTO_NIP05_UNAVAILABLE); grotto_profile_info_free(checked);
  g_settings_set_boolean(preferences, "use-tor-for-relays", FALSE);
  BunkerTestRelay *bootstrap = bunker_test_relay_new(), *read_only = bunker_test_relay_new();
  BootstrapProbe probe = {.hex = hex, .relay = bootstrap}; bunker_test_relay_set_request_callback(bootstrap, bootstrap_request, &probe);
  bunker_test_relay_set_event_callback(bootstrap, forbidden_bootstrap_publish, NULL);
  const char *bootstrap_urls[] = {bunker_test_relay_url(bootstrap), NULL};
  g_settings_set_strv(preferences, "profile-bootstrap-relays", bootstrap_urls); g_settings_set_boolean(preferences, "profile-bootstrap-enabled", TRUE);
  empty = NULL; grotto_profile_service_async(NULL, NULL, GROTTO_PROFILE_FETCH, npub, NULL, NULL, host_result, &empty); WAIT(empty);
  missing = g_task_propagate_pointer(G_TASK(empty), &empty_error); g_object_unref(empty);
  g_assert_no_error(empty_error); g_assert_nonnull(missing); g_assert_true(missing->bootstrap_used); g_assert_false(missing->published);
  g_assert_null(missing->publish_relays[0]);
  empty = NULL; grotto_profile_service_async(NULL, NULL, GROTTO_PROFILE_SAVE, npub, missing, NULL, host_result, &empty); WAIT(empty);
  g_assert_null(g_task_propagate_pointer(G_TASK(empty), &empty_error)); g_object_unref(empty);
  g_assert_error(empty_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND); g_clear_error(&empty_error); grotto_profile_info_free(missing);
  guint bc, br, bp; bunker_test_relay_stats(bootstrap, &bc, &br, &bp); g_assert_cmpuint(bp, ==, 0);
  bootstrap_auth_refused(connection, npub, bootstrap, &probe);
  guint image_contacts = 0;
  g_autoptr(GSocketService) image_server = g_socket_service_new();
  g_autoptr(GSocketAddress) address = g_inet_socket_address_new_from_string("127.0.0.1", 0); GSocketAddress *effective = NULL;
  g_assert_true(g_socket_listener_add_address(G_SOCKET_LISTENER(image_server), address, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP, NULL, &effective, NULL));
  g_autofree char *image = g_strdup_printf("https://127.0.0.1:%u/tracker.png", g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(effective))); g_object_unref(effective);
  g_signal_connect(image_server, "incoming", G_CALLBACK(image_contact), &image_contacts); g_socket_service_start(image_server);
  BunkerTestRelay *relay = bunker_test_relay_new(); seed_profiles(relay, hex, sk, image);
#ifdef __linux__
  seed_metadata(relay, hex, sk, 10063, "[[\"server\",\"https://images.example/\"]]");
#endif
  guint initial_events = json_array_get_length(bunker_test_relay_events(relay));
  g_autofree char *outbox_tags = g_strdup_printf("[[\"r\",\"%s\",\"write\"],[\"r\",\"%s\",\"read\"]]", bunker_test_relay_url(relay), bunker_test_relay_url(read_only));
  seed_metadata(bootstrap, hex, sk, 10002, outbox_tags);
  g_autofree char *dm_tags = g_strdup_printf("[[\"relay\",\"%s\"]]", bunker_test_relay_url(read_only));
  seed_metadata(bootstrap, hex, sk, 10050, dm_tags);
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
  g_autoptr(GrottoApprovalController) controller = grotto_approval_controller_new(G_APPLICATION(app), connection);
  approval.controller = controller;
  signer_window_set_pending_group(window, grotto_approval_pending_group_new(controller));
  g_object_weak_ref(G_OBJECT(window), finalised, &window_gone);
  diagnostic_window = GTK_WIDGET(window);
  gtk_window_set_default_size(GTK_WINDOW(window), 420, 720); gtk_window_present(GTK_WINDOW(window));
  signer_window_show_page(window, "identity");
  GtkStack *stack = GTK_STACK(gtk_widget_get_template_child(GTK_WIDGET(window), TYPE_SIGNER_WINDOW, "panel_stack"));
  GtkWidget *panel = gtk_stack_get_child_by_name(stack, "identity"); g_assert_true(GROTTO_IS_IDENTITY_PANEL(panel));
  g_assert_true(grotto_panel_has_profile_host(GROTTO_PANEL(panel)));
  WAIT(find(panel, "Edit Profile…") && gtk_widget_get_visible(find(panel, "Edit Profile…")) && gtk_widget_get_sensitive(find(panel, "Edit Profile…")));
  g_assert_nonnull(find(panel, "Newest")); g_assert_null(find(panel, "Older"));
  guint bootstrap_requests = probe.requests;
  g_assert_cmpuint(bootstrap_requests, >=, 3);
  g_autofree char *dm_cache = g_strdup_printf("%s/grotto/profiles/%s-10050.json", g_get_user_cache_dir(), hex);
  g_assert_true(g_file_test(dm_cache, G_FILE_TEST_IS_REGULAR));
  g_signal_emit_by_name(find(panel, "Refresh Profile"), "clicked");
  WAIT(gtk_widget_is_sensitive(find(panel, "Edit Profile…")));
  g_assert_cmpuint(probe.requests, ==, bootstrap_requests);
  guint rc, rr, rp; bunker_test_relay_stats(read_only, &rc, &rr, &rp); g_assert_cmpuint(rc, ==, 0);
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
#ifdef __linux__
  crop_picker_cancel(window, dialog); /* Real chooser -> crop controls -> Cancel; no upload authorization. */
#endif
  gtk_editable_set_text(GTK_EDITABLE(find(GTK_WIDGET(dialog), "Name")), "alice");
  gtk_editable_set_text(GTK_EDITABLE(find(GTK_WIDGET(dialog), "Display Name")), "Alice");
  adw_switch_row_set_active(ADW_SWITCH_ROW(find(GTK_WIDGET(dialog), "Automated Account (Bot)")), TRUE);
  /* Save learns a relay list from a new bootstrap contact, not merely the
   * context previously rendered in the editor. The real review must show it. */
  BunkerTestRelay *save_bootstrap = bunker_test_relay_new();
  seed_metadata(save_bootstrap, hex, sk, 10002, outbox_tags);
  g_autofree char *relay_cache = g_strdup_printf("%s/grotto/profiles/%s-10002.json", g_get_user_cache_dir(), hex);
  g_assert_cmpint(g_remove(relay_cache), ==, 0);
  const char *save_lookup[] = {bunker_test_relay_url(save_bootstrap), NULL};
  g_settings_set_strv(preferences, "profile-bootstrap-relays", save_lookup); approval.discovery = save_lookup[0];
  GtkWidget *save = find(GTK_WIDGET(dialog), "Save"); g_assert_true(gtk_widget_is_sensitive(save));
  GrottoPanelsService *service = grotto_panel_get_service(GROTTO_PANEL(panel));
  /* Opening the real form can overlap a daemon snapshot refresh. Wait for
   * current authority, not only the last rendered button sensitivity. */
  WAIT(grotto_panels_service_can(service, GROTTO_PANEL_SET_IDENTITY_LABEL));
  g_assert_false(gn_session_manager_is_locked(gn_session_manager_get_default()));
  g_signal_emit_by_name(save, "clicked");
  WAIT(approval.decisions == 1 && !adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
  g_assert_cmpuint(approval.reviews, ==, 1);
  JsonArray *events = bunker_test_relay_events(relay); g_assert_cmpuint(json_array_get_length(events), ==, initial_events + 1);
  g_autofree char *json = json_to_string(json_array_get_element(events, initial_events), FALSE);
  NostrEvent *event = nostr_event_new(); g_assert_cmpint(nostr_event_deserialize_signed(event, json, NULL), ==, NOSTR_EVENT_VALIDATION_OK);
  g_assert_cmpint(nostr_event_validate(event, NULL), ==, NOSTR_EVENT_VALIDATION_OK); g_assert_cmpint(nostr_event_get_kind(event), ==, 0); g_assert_cmpstr(nostr_event_get_pubkey(event), ==, hex);
  g_assert_nonnull(strstr(nostr_event_get_content(event), "alice"));
  NostrEvent *reviewed = nostr_event_new(); char reviewed_id[65]; g_assert_cmpint(nostr_event_deserialize(reviewed, approval.reviewed), ==, 0);
  g_assert_cmpstr(nostr_event_get_content(event), ==, nostr_event_get_content(reviewed));
  g_assert_cmpint(nostr_event_compute_id(reviewed, reviewed_id), ==, NOSTR_EVENT_VALIDATION_OK);
  g_assert_cmpstr(nostr_event_get_id(event), ==, reviewed_id); g_assert_cmpuint(nostr_tags_size(nostr_event_get_tags(event)), ==, 0); nostr_event_free(reviewed);
  g_autoptr(JsonParser) content = json_parser_new(); g_assert_true(json_parser_load_from_data(content, nostr_event_get_content(event), -1, NULL));
  g_assert_true(json_object_get_boolean_member(json_object_get_object_member(json_node_get_object(json_parser_get_root(content)), "extension"), "keep"));
  g_assert_true(json_object_get_boolean_member(json_node_get_object(json_parser_get_root(content)), "bot"));
  g_assert_cmpuint(probe.requests, ==, bootstrap_requests);
  bunker_test_relay_stats(bootstrap, &bc, &br, &bp); g_assert_cmpuint(bp, ==, 2);
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
  g_assert_false(approval.last_allowed); g_assert_cmpuint(json_array_get_length(events), ==, initial_events + 1);
  g_signal_emit_by_name(find(GTK_WIDGET(dialog), "Cancel"), "clicked");
  WAIT(!adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
  /* Missing host correlation is display-only: an ordinary Save still gets
   * exactly one review and completes through the unchanged real signer gate. */
  approval.cancel_next = FALSE; approval.fallback = TRUE;
  g_autoptr(GrottoProfileInfo) fallback_draft = grotto_profile_info_copy(NULL);
  g_free(fallback_draft->fields[GROTTO_PROFILE_NAME]); fallback_draft->fields[GROTTO_PROFILE_NAME] = g_strdup("alice");
  g_free(fallback_draft->fields[GROTTO_PROFILE_DISPLAY_NAME]); fallback_draft->fields[GROTTO_PROFILE_DISPLAY_NAME] = g_strdup("Alice");
  NostrEvent *baseline = nostr_event_new(); nostr_event_deserialize(baseline, json);
  fallback_draft->created_at = nostr_event_get_created_at(baseline); nostr_event_free(baseline);
  GAsyncResult *fallback_saved = NULL;
  grotto_profile_service_async(GTK_WINDOW(window), NULL, GROTTO_PROFILE_SAVE, npub, fallback_draft, NULL, host_result, &fallback_saved); WAIT(fallback_saved);
  g_autoptr(GError) fallback_error = NULL;
  g_autoptr(GrottoProfileInfo) fallback_profile = g_task_propagate_pointer(G_TASK(fallback_saved), &fallback_error); g_object_unref(fallback_saved);
  g_assert_no_error(fallback_error); g_assert_nonnull(fallback_profile); g_assert_true(fallback_profile->published);
  g_assert_cmpuint(approval.reviews, ==, 3); g_assert_cmpuint(approval.decisions, ==, 2);
  WAIT(approval.completed == 3 && !adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
  /* Offline cache is the same host path, with no synthetic cached result. */
  bunker_test_relay_free(relay); relay = NULL;
  g_signal_emit_by_name(find(panel, "Refresh Profile"), "clicked"); WAIT(find(panel, "Offline — showing the saved profile"));
  g_assert_nonnull(find(panel, "Alice")); g_assert_cmpuint(approval.reviews, ==, 3);
  bunker_test_relay_free(save_bootstrap); bunker_test_relay_free(bootstrap); bunker_test_relay_free(read_only);
  g_socket_service_stop(image_server);
  diagnostic_window = NULL;
  grotto_approval_controller_stop(controller);
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
    const char *keys[] = {"operation", "transport", "purpose", "identity", "reason", "request_id"};
    const char *values[] = {"sign_event", "MANAGEMENT", "profile-management", npub, i ? "wss://private.example/" : "https://images.example/\nhttps://fallback.example/", "authorization-request"};
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
    AdwActionRow *relay_row = ADW_ACTION_ROW(gtk_widget_get_template_child(GTK_WIDGET(dialog), GNOSTR_TYPE_APPROVAL_DIALOG, "row_relay"));
    GtkLabel *detail_row = GTK_LABEL(gtk_widget_get_template_child(GTK_WIDGET(dialog), GNOSTR_TYPE_APPROVAL_DIALOG, "label_details"));
    g_assert_cmpstr(adw_action_row_get_subtitle(relay_row), ==, "destinations: see Details");
    g_assert_true(gnostr_approval_info_can_approve(snapshot));
    const char *destinations[] = {i ? "wss://private.example/" : "https://images.example/", NULL};
    const char *bootstrap[] = {"wss://bootstrap.example/", NULL};
    gnostr_approval_dialog_set_profile_provenance(dialog, "another-request", destinations, bootstrap);
    g_assert_cmpstr(adw_action_row_get_subtitle(relay_row), ==, "destinations: see Details");
    gnostr_approval_dialog_set_profile_provenance(dialog, "authorization-request", destinations, bootstrap);
    g_assert_nonnull(strstr(adw_action_row_get_subtitle(relay_row), bootstrap[0]));
    g_assert_nonnull(strstr(gtk_label_get_text(detail_row), destinations[0]));
    g_assert_nonnull(strstr(gtk_label_get_text(detail_row), raw));
    g_assert_true(gnostr_approval_info_can_approve(snapshot));
    gnostr_approval_dialog_set_info(dialog, snapshot, "Grotto");
    g_assert_cmpstr(adw_action_row_get_subtitle(relay_row), ==, "destinations: see Details");
    if (!i) {
      const char *effect = adw_action_row_get_subtitle(impact);
      g_assert_nonnull(strstr(effect, "reviewed, cropped and re-encoded PNG"));
      g_assert_nonnull(strstr(effect, "without the original file's embedded metadata"));
      g_assert_nonnull(strstr(effect, "public immediately, independently of profile Save"));
      g_assert_nonnull(strstr(effect, "Stopping does not delete"));
      g_assert_null(strstr(effect, "uploads the original image"));
    }
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
  g_assert_nonnull(strstr(body, "EXIF")); g_assert_nonnull(strstr(body, "re-encoded as PNG")); g_assert_nonnull(strstr(body, "public immediately"));
  g_assert_nonnull(strstr(body, "will not delete")); g_assert_nonnull(strstr(body, servers[0])); g_assert_nonnull(strstr(body, servers[1]));
  g_assert_cmpstr(adw_alert_dialog_get_default_response(upload), ==, "cancel"); g_object_unref(upload);
  g_autoptr(GBytes) clean = grotto_profile_crop_image(preview, FALSE, 1, .5, .5);
  g_autoptr(GInputStream) clean_stream = g_memory_input_stream_new_from_bytes(clean);
  g_autoptr(GdkPixbuf) clean_pixels = gdk_pixbuf_new_from_stream(clean_stream, NULL, NULL);
  g_assert_null(gdk_pixbuf_get_option(clean_pixels, "tEXt::Comment"));
  g_autoptr(GBytes) svg = g_bytes_new_static("<svg/>", 6); g_autoptr(GError) error = NULL;
  g_assert_null(grotto_profile_decode_image(svg, NULL, &error)); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA); g_clear_error(&error);
  g_autoptr(GBytes) huge = g_bytes_new_take(g_malloc0(5*1024*1024+1), 5*1024*1024+1);
  g_assert_null(grotto_profile_decode_image(huge, NULL, &error)); g_assert_nonnull(error); g_clear_error(&error);
  g_autoptr(GdkPixbuf) wide = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 8193, 1); gdk_pixbuf_fill(wide, 0);
  data = NULL; g_assert_true(gdk_pixbuf_save_to_buffer(wide, &data, &length, "png", NULL, NULL));
  g_autoptr(GBytes) wide_bytes = g_bytes_new_take(data, length);
  g_assert_null(grotto_profile_decode_image(wide_bytes, NULL, &error)); g_assert_nonnull(error);
}
static void crop_and_nip05(void) {
  g_autoptr(GdkPixbuf) pixels = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 1200, 600);
  gdk_pixbuf_fill(pixels, 0xff0000ff);
  g_autoptr(GdkPixbuf) right = gdk_pixbuf_new_subpixbuf(pixels, 600, 0, 600, 600); gdk_pixbuf_fill(right, 0x0000ffff);
  g_autoptr(GdkTexture) texture = gdk_texture_new_for_pixbuf(pixels);
  g_autoptr(GBytes) left_bytes = grotto_profile_crop_image(texture, FALSE, 1, 0, .5);
  g_autoptr(GBytes) right_bytes = grotto_profile_crop_image(texture, FALSE, 1, 1, .5);
  g_autoptr(GdkTexture) left_image = gdk_texture_new_from_bytes(left_bytes, NULL);
  g_autoptr(GdkTexture) right_image = gdk_texture_new_from_bytes(right_bytes, NULL);
  g_assert_cmpint(gdk_texture_get_width(left_image), ==, 512); g_assert_cmpint(gdk_texture_get_height(left_image), ==, 512);
  g_autoptr(GdkPixbuf) left_pixels = gdk_pixbuf_get_from_texture(left_image);
  g_autoptr(GdkPixbuf) right_pixels = gdk_pixbuf_get_from_texture(right_image);
  g_assert_cmpuint(gdk_pixbuf_get_pixels(left_pixels)[0], ==, 255); g_assert_cmpuint(gdk_pixbuf_get_pixels(right_pixels)[2], ==, 255);
  g_autoptr(GBytes) banner = grotto_profile_crop_image(texture, TRUE, 1, .5, 0);
  g_autoptr(GdkTexture) banner_image = gdk_texture_new_from_bytes(banner, NULL);
  g_assert_cmpint(gdk_texture_get_width(banner_image), ==, 3 * gdk_texture_get_height(banner_image));
  g_autoptr(GBytes) zoomed = grotto_profile_crop_image(texture, FALSE, 2, .5, 1);
  g_autoptr(GdkTexture) zoomed_image = gdk_texture_new_from_bytes(zoomed, NULL);
  g_assert_cmpint(gdk_texture_get_width(zoomed_image), ==, 300); g_assert_cmpint(gdk_texture_get_height(zoomed_image), ==, 300);
  const char *hex = "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798";
  g_autofree char *valid = g_strdup_printf("{\"names\":{\"alice\":\"%s\",\"_\":\"%s\"}}", hex, hex);
  g_assert_cmpint(grotto_profile_nip05_response(valid, strlen(valid), "alice", hex), ==, GROTTO_NIP05_VERIFIED);
  g_assert_cmpint(grotto_profile_nip05_response(valid, strlen(valid), "_", hex), ==, GROTTO_NIP05_VERIFIED);
  g_assert_cmpint(grotto_profile_nip05_response(valid, strlen(valid), "other", hex), ==, GROTTO_NIP05_MISMATCH);
  g_assert_cmpint(grotto_profile_nip05_response("{}", 2, "alice", hex), ==, GROTTO_NIP05_UNAVAILABLE);
  g_assert_cmpint(grotto_profile_nip05_response("[", 1, "alice", hex), ==, GROTTO_NIP05_UNAVAILABLE);
  g_autofree char *upper = g_ascii_strup(valid, -1);
  g_assert_cmpint(grotto_profile_nip05_response(upper, strlen(upper), "alice", hex), !=, GROTTO_NIP05_VERIFIED);
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
  g_test_add_func("/signer/profile/crop-and-nip05", crop_and_nip05);
  g_test_add_func("/signer/profile/authorization-preview", authorization_preview);
  g_test_add_func("/signer/profile/image-privacy-and-limits", privacy_helpers);
  g_test_add_func("/signer/profile/save-one-review-real-daemon", profile_integration);
  int result = g_test_run(); nostrc_test_bus_down(private_bus);
  g_autoptr(GFile) owned = g_file_new_for_path(home); remove_owned_tree(owned); return result;
}
