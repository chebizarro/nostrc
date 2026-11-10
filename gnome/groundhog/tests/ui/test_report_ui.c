#include "gh-report-dialog.h"
#include "nostrc-test-gdk-frame.h"
#include "gh-message-row.h"
#include "gh-nip29-group.h"
#include <nostr-event.h>
#include <json-glib/json-glib.h>
#include <json.h>
#include <string.h>
void groundhog_register_resource(void);
#define ACCOUNT "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define TARGET "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
static GtkWidget *find(GtkWidget *w, GType type, const gchar *label)
{
  if (g_type_is_a(G_OBJECT_TYPE(w), type)) {
    const gchar *text = GTK_IS_BUTTON(w) ? gtk_button_get_label(GTK_BUTTON(w))
        : ADW_IS_PREFERENCES_ROW(w) ? adw_preferences_row_get_title(ADW_PREFERENCES_ROW(w)) : NULL;
    if (!label || !g_strcmp0(text, label)) return w;
  }
  for (GtkWidget *c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c)) {
    GtkWidget *found = find(c, type, label); if (found) return found;
  }
  return NULL;
}
static void test_dialog(void)
{
  GhStore *store = gh_store_open_ephemeral(ACCOUNT, NULL, NULL);
  g_autoptr(GhSocialLists) lists = gh_social_lists_new(NULL);
  g_assert_true(gh_social_lists_bind_store(lists, store, 1, NULL));
  g_autoptr(GhReportService) reports = gh_report_service_new(lists, NULL, NULL, NULL);
  gh_report_service_bind(reports, store, 1);
  GhReportDialog *dialog = g_object_ref_sink(gh_report_dialog_new(reports, lists, TARGET, NULL, NULL, NULL, NULL));
  GtkWidget *cancel = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(dialog), GH_TYPE_REPORT_DIALOG, "cancel_button"));
  GtkWidget *publish = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(dialog), GH_TYPE_REPORT_DIALOG, "publish_button"));
  GtkWidget *hide = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(dialog), GH_TYPE_REPORT_DIALOG, "hide_row"));
  g_assert_nonnull(cancel); g_assert_nonnull(publish); g_assert_nonnull(hide);
  g_assert_true(adw_dialog_get_default_widget(ADW_DIALOG(dialog)) == cancel);
  g_assert_false(adw_switch_row_get_active(ADW_SWITCH_ROW(hide)));
  g_assert_false(gtk_widget_is_sensitive(publish)); /* no NIP-65 list, no fallback */
  g_assert_cmpstr(adw_dialog_get_title(ADW_DIALOG(dialog)), ==, "Report Account");
  g_object_unref(dialog);
  gh_report_service_close(reports); gh_social_lists_close(lists); gh_store_close(store);
}
#define SENDER "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798"
#define SECRET "0000000000000000000000000000000000000000000000000000000000000001"
static gchar *signed_event(const gchar *json)
{
  NostrEvent *e = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_unsigned(e, json, NULL), ==, NOSTR_EVENT_VALIDATION_OK);
  g_assert_cmpint(nostr_event_sign(e, SECRET), ==, 0);
  gchar *result = nostr_event_serialize_compact(e); nostr_event_free(e); return result;
}
static GVariant *report_target(GMenuModel *model, const gchar *expected)
{
  for (gint i = 0; i < g_menu_model_get_n_items(model); i++) {
    g_autofree gchar *action = NULL;
    g_menu_model_get_item_attribute(model, i, G_MENU_ATTRIBUTE_ACTION, "s", &action);
    if (!g_strcmp0(action, expected))
      return g_menu_model_get_item_attribute_value(model, i, G_MENU_ATTRIBUTE_TARGET, NULL);
    const gchar *links[] = {G_MENU_LINK_SECTION, G_MENU_LINK_SUBMENU};
    for (guint j = 0; j < G_N_ELEMENTS(links); j++) {
      g_autoptr(GMenuModel) child = g_menu_model_get_item_link(model, i, links[j]);
      GVariant *target = child ? report_target(child, expected) : NULL;
      if (target) return target;
    }
  }
  return NULL;
}
static void no_private_text(GtkWidget *w)
{
  if (GTK_IS_LABEL(w)) {
    const gchar *text = gtk_label_get_text(GTK_LABEL(w));
    g_assert_null(strstr(text, "PRIVATE_TEXT_SENTINEL"));
    g_assert_null(strstr(text, "privategroup"));
    g_assert_null(strstr(text, "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"));
  }
  for (GtkWidget *c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c)) no_private_text(c);
}
static gboolean public_room(gpointer data, const gchar *room)
{
  return !g_strcmp0(room, "wss://nos.lol\x1fprivategroup")
      && gh_nip29_group_is_known_public(data);
}
static void test_message_entrypoints(void)
{
  GhStore *store = gh_store_open_ephemeral(ACCOUNT, NULL, NULL);
  g_autoptr(GhSocialLists) lists = gh_social_lists_new(NULL);
  g_assert_true(gh_social_lists_bind_store(lists, store, 1, NULL));
  g_autoptr(GhReportService) service = gh_report_service_new(lists, NULL, NULL, NULL);
  gh_report_service_bind(service, store, 1);
  g_autoptr(GtkApplication) app = gtk_application_new("org.nostr.Groundhog.ReportTest", G_APPLICATION_NON_UNIQUE);
  g_assert_true(g_application_register(G_APPLICATION(app), NULL, NULL));
  GtkWidget *window = adw_application_window_new(app);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  adw_application_window_set_content(ADW_APPLICATION_WINDOW(window), box);
  gtk_window_present(GTK_WINDOW(window));
  while (g_main_context_iteration(NULL, FALSE));
  GhNip29GroupKey *key = gh_nip29_group_key_new("wss://nos.lol", "privategroup", NULL);
  GhNip29Group *group = gh_nip29_group_new(key, SENDER, NULL);
  gh_report_dialog_install(app, service, lists, public_room, group);
  const gchar *rumor = "{\"pubkey\":\"" SENDER "\",\"kind\":14,\"created_at\":100,\"tags\":[[\"p\",\"" ACCOUNT "\"]],\"content\":\"PRIVATE_TEXT_SENTINEL\"}";
  const gchar *legacy = "{\"pubkey\":\"" SENDER "\",\"kind\":14,\"created_at\":100,\"tags\":[[\"p\",\"" ACCOUNT "\"],[\"groundhog-legacy\",\"nip04\",\"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\"]],\"content\":\"PRIVATE_TEXT_SENTINEL\"}";
  const gchar *inner = "{\"pubkey\":\"" SENDER "\",\"kind\":9,\"created_at\":100,\"tags\":[],\"content\":\"PRIVATE_TEXT_SENTINEL\"}";
  g_autofree gchar *public_json = signed_event("{\"pubkey\":\"" SENDER "\",\"kind\":9,\"created_at\":100,\"tags\":[[\"h\",\"privategroup\"]],\"content\":\"PRIVATE_TEXT_SENTINEL\"}");
  GhMessage *messages[] = {
    gh_message_new_from_rumor(ACCOUNT, rumor, NULL),
    gh_message_new_from_rumor(ACCOUNT, legacy, NULL),
    gh_message_new_from_mls(ACCOUNT, "aabb", inner, NULL),
    gh_message_new_from_nip29_event(ACCOUNT, "wss://nos.lol", public_json, NULL),
  };
  GhMessageRow *group_row = NULL;
  for (guint i = 0; i < G_N_ELEMENTS(messages); i++) {
    g_assert_nonnull(messages[i]);
    GtkWidget *row = gh_message_row_new();
    gtk_box_append(GTK_BOX(box), row); gh_message_row_set_message(GH_MESSAGE_ROW(row), messages[i]);
    g_assert_false(gh_report_dialog_message_is_public(row, messages[i]));
    g_assert_true(gh_message_row_popup_menu(GH_MESSAGE_ROW(row), -1, -1));
    GtkPopover *menu = gh_message_row_get_menu(GH_MESSAGE_ROW(row));
    GMenuModel *model = gtk_popover_menu_get_menu_model(GTK_POPOVER_MENU(menu));
    g_autoptr(GVariant) account_target = report_target(model, "app.report-account");
    g_assert_nonnull(account_target); g_assert_true(g_variant_is_of_type(account_target, G_VARIANT_TYPE("(ss)")));
    const gchar *a, *p; g_variant_get(account_target, "(&s&s)", &a, &p);
    g_assert_cmpstr(a, ==, ACCOUNT); g_assert_cmpstr(p, ==, SENDER);
    g_assert_null(report_target(model, "app.report-public-message"));
    gtk_popover_popdown(menu);
    g_action_group_activate_action(G_ACTION_GROUP(app), "report-account", account_target);
    AdwDialog *report = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
    g_assert_true(GH_IS_REPORT_DIALOG(report));
    g_assert_cmpstr(adw_dialog_get_title(report), ==, "Report Account");
    no_private_text(GTK_WIDGET(report));
    GtkWidget *explanation = find(GTK_WIDGET(report), ADW_TYPE_ENTRY_ROW, "Explanation (optional)");
    g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(explanation)), ==, "");
    g_signal_emit_by_name(find(GTK_WIDGET(report), GTK_TYPE_BUTTON, "_Cancel"), "clicked");
    if (i == 3) group_row = GH_MESSAGE_ROW(row);
  }
  /* The very same signed event is private until received kind-39000 metadata. */
  g_autofree gchar *metadata = signed_event("{\"pubkey\":\"" SENDER "\",\"kind\":39000,\"created_at\":100,\"tags\":[[\"d\",\"privategroup\"]],\"content\":\"\"}");
  NostrEvent *e = nostr_event_new(); nostr_event_deserialize_signed(e, metadata, NULL);
  g_assert_cmpint(gh_nip29_group_admit_at(group, e, 100), ==, GH_NIP29_ADMISSION_ACCEPTED);
  nostr_event_free(e);
  g_assert_true(gh_report_dialog_message_is_public(GTK_WIDGET(group_row), messages[3]));
  g_assert_true(gh_message_row_popup_menu(group_row, -1, -1));
  GMenuModel *model = gtk_popover_menu_get_menu_model(GTK_POPOVER_MENU(gh_message_row_get_menu(group_row)));
  g_autoptr(GVariant) public_target = report_target(model, "app.report-public-message");
  g_assert_nonnull(public_target);
  g_assert_null(report_target(model, "app.report-account"));
  gtk_popover_popdown(gh_message_row_get_menu(group_row));
  g_autofree gchar *approval = gh_report_build_public_message(ACCOUNT, public_json,
      NOSTR_NIP56_SPAM, "User explanation from a saved report", 100, NULL);
  g_assert_nonnull(approval);
  gint64 report_id = gh_store_report_approve(store, approval, "[\"wss://write.test.invalid\"]", 100, NULL);
  g_action_group_activate_action(G_ACTION_GROUP(app), "review-report", g_variant_new("(sx)", ACCOUNT, report_id));
  AdwDialog *reviewed = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  g_assert_true(GH_IS_REPORT_DIALOG(reviewed));
  g_autoptr(JsonParser) approved = json_parser_new();
  g_assert_true(json_parser_load_from_data(approved, approval, -1, NULL));
  g_autofree gchar *expected_tags = json_to_string(json_object_get_member(
      json_node_get_object(json_parser_get_root(approved)), "tags"), FALSE);
  GtkLabel *shown_tags = GTK_LABEL(gtk_widget_get_template_child(GTK_WIDGET(reviewed), GH_TYPE_REPORT_DIALOG, "tags_label"));
  g_assert_cmpstr(gtk_label_get_text(shown_tags), ==, expected_tags);
  g_assert_nonnull(strstr(expected_tags, "\"e\""));
  adw_dialog_force_close(reviewed);
  g_autofree gchar *private_metadata = signed_event("{\"pubkey\":\"" SENDER "\",\"kind\":39000,\"created_at\":101,\"tags\":[[\"d\",\"privategroup\"],[\"private\"]],\"content\":\"\"}");
  e = nostr_event_new(); nostr_event_deserialize_signed(e, private_metadata, NULL);
  g_assert_cmpint(gh_nip29_group_admit_at(group, e, 101), ==, GH_NIP29_ADMISSION_ACCEPTED);
  nostr_event_free(e);
  g_assert_false(gh_report_dialog_message_is_public(GTK_WIDGET(group_row), messages[3]));
  /* A public menu captured before a metadata change cannot open a report. */
  AdwDialog *before = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  g_action_group_activate_action(G_ACTION_GROUP(app), "report-public-message", public_target);
  g_assert_true(before == adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
  g_assert_true(gh_message_row_popup_menu(group_row, -1, -1));
  model = gtk_popover_menu_get_menu_model(GTK_POPOVER_MENU(gh_message_row_get_menu(group_row)));
  g_autoptr(GVariant) private_target = report_target(model, "app.report-account");
  g_assert_nonnull(private_target);
  g_assert_null(report_target(model, "app.report-public-message"));
  gh_report_dialog_uninstall(app);
  gtk_window_destroy(GTK_WINDOW(window));
  for (guint i = 0; i < G_N_ELEMENTS(messages); i++) g_object_unref(messages[i]);
  gh_nip29_group_free(group); gh_nip29_group_key_free(key);
  gh_report_service_close(service); gh_social_lists_close(lists); gh_store_close(store);
  while (g_main_context_iteration(NULL, FALSE));
}
int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  nostr_json_init(); groundhog_register_resource();
  if (!gtk_init_check()) return 77;
  adw_init();
  nostrc_test_tolerate_gdk_frame_warning();
  g_test_add_func("/reports/ui/disclosure-default-cancel", test_dialog);
  g_test_add_func("/reports/ui/message-origin-entrypoints", test_message_entrypoints);
  return g_test_run();
}
