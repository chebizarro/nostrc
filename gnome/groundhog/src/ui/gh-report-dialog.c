#include "gh-report-dialog.h"
#include <glib/gi18n.h>
#include <json-glib/json-glib.h>
#include <string.h>

struct _GhReportDialog {
  AdwDialog parent_instance;
  GtkButton *cancel_button, *publish_button;
  AdwComboRow *category_row;
  AdwEntryRow *explanation_row;
  AdwSwitchRow *hide_row;
  GtkLabel *identity_label, *tags_label, *status_label;
  GtkBox *targets_box;
  GhReportService *service;
  GhSocialLists *lists;
  gchar *account, *target, *public_json, *room_id;
  GStrv targets;
  guint64 generation;
  gint64 id, created_at;
  GhReportPublicRoomFunc public_room;
  gpointer public_data;
};
G_DEFINE_TYPE(GhReportDialog, gh_report_dialog, ADW_TYPE_DIALOG)
static gboolean current(GhReportDialog *s)
{
  return s->generation == gh_social_lists_get_generation(s->lists)
      && !g_strcmp0(s->account, gh_social_lists_get_account(s->lists));
}
static gchar *payload(GhReportDialog *s, GError **error)
{
  const gchar *text = gtk_editable_get_text(GTK_EDITABLE(s->explanation_row));
  NostrNip56Category category = adw_combo_row_get_selected(s->category_row);
  if (s->public_json) {
    if (!s->public_room || !s->public_room(s->public_data, s->room_id)) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                          "The group is no longer known to be public"); return NULL;
    }
    g_autofree gchar *relay = NULL, *group = NULL;
    if (!gh_message_nip29_room_split(s->room_id, &relay, &group)) return NULL;
    g_autoptr(GhMessage) checked = gh_message_new_from_nip29_event(s->account, relay, s->public_json, error);
    if (!checked || !gh_message_is_signed(checked)
        || g_strcmp0(gh_message_get_room_id(checked), s->room_id)
        || g_strcmp0(gh_message_get_sender(checked), s->target)) {
      if (checked) g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                      "The public message does not match the reviewed account and group");
      return NULL;
    }
    return gh_report_build_public_message(s->account, s->public_json, category, text, s->created_at, error);
  }
  return gh_report_build_account(s->account, s->target, category, text, s->created_at, error);
}
static void preview(GObject *object, GParamSpec *pspec, GhReportDialog *s)
{
  (void)object; (void)pspec;
  if (!s->lists || s->id) return;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *json = payload(s, &error);
  g_autoptr(JsonParser) parser = json_parser_new();
  if (json && json_parser_load_from_data(parser, json, -1, NULL)) {
    JsonObject *o = json_node_get_object(json_parser_get_root(parser));
    g_autofree gchar *tags = json_to_string(json_object_get_member(o, "tags"), FALSE);
    gtk_label_set_text(s->tags_label, tags);
  }
  gtk_widget_set_sensitive(GTK_WIDGET(s->publish_button), json && s->targets && s->targets[0]
      && !gh_report_service_get_busy(s->service) && current(s));
  gtk_label_set_text(s->status_label, error ? error->message : s->targets && s->targets[0]
      ? "" : _("Needs a Relay List — configure an unblocked NIP-65 write relay in Preferences."));
}
static const gchar *outcome_label(gint outcome)
{
  switch (outcome) {
    case GH_RELAY_PUBLISH_ACCEPTED: return _("Accepted by this relay");
    case GH_RELAY_PUBLISH_REJECTED: return _("Rejected by this relay");
    case GH_RELAY_PUBLISH_AUTH_REQUIRED: return _("Relay authentication failed");
    case GH_RELAY_PUBLISH_CONNECTION_FAILED: return _("Connection failed; acceptance unknown");
    case GH_RELAY_PUBLISH_CANCELLED: return _("Cancelled; acceptance may be unknown");
    case GH_RELAY_PUBLISH_BLOCKED_BY_LOCAL_POLICY: return _("Destination no longer allowed");
    default: return _("Not yet accepted");
  }
}
static void refresh(GhReportService *service, GhReportDialog *s);
static void retry(GtkButton *button, GhReportDialog *s)
{
  g_autoptr(GError) error = NULL;
  const gchar *url = g_object_get_data(G_OBJECT(button), "report-target");
  if (current(s) && !gh_report_service_retry(s->service, s->id, url, &error))
    gtk_label_set_text(s->status_label, error ? error->message : _("Report could not be retried"));
  else refresh(s->service, s);
}
static void refresh(GhReportService *service, GhReportDialog *s)
{
  if (!current(s)) { adw_dialog_force_close(ADW_DIALOG(s)); return; }
  if (!s->id) { preview(NULL, NULL, s); return; }
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) reports = gh_report_service_list(service, &error);
  GhStoreReport *r = NULL;
  for (guint i = 0; reports && i < reports->len; i++) {
    GhStoreReport *candidate = g_ptr_array_index(reports, i);
    if (candidate->id == s->id) { r = candidate; break; }
  }
  if (!r) return;
  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, r->outcomes_json, -1, NULL)) return;
  JsonObject *outcomes = json_node_get_object(json_parser_get_root(parser));
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(s->targets_box)))) gtk_box_remove(s->targets_box, child);
  for (guint i = 0; s->targets && s->targets[i]; i++) {
    const gchar *url = s->targets[i];
    gint outcome = json_object_has_member(outcomes, url) ? json_object_get_int_member(outcomes, url) : 0;
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *label = gtk_label_new(url);
    gtk_label_set_selectable(GTK_LABEL(label), TRUE); gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    gtk_box_append(GTK_BOX(row), label);
    gtk_box_append(GTK_BOX(row), gtk_label_new(outcome_label(outcome)));
    if (outcome != GH_RELAY_PUBLISH_ACCEPTED) {
      GtkWidget *button = gtk_button_new_with_label(_("Retry This Relay"));
      g_object_set_data_full(G_OBJECT(button), "report-target", g_strdup(url), g_free);
      g_signal_connect(button, "clicked", G_CALLBACK(retry), s);
      gtk_widget_set_sensitive(button, !gh_report_service_get_busy(service));
      gtk_box_append(GTK_BOX(row), button);
    }
    gtk_box_append(s->targets_box, row);
  }
  const gchar *failure = gh_report_service_get_error(service);
  gtk_label_set_text(s->status_label, failure ? failure : gh_report_service_get_busy(service)
      ? _("Publishing Report…") : !g_strcmp0(r->state, "accepted")
      ? _("Report Accepted by a Relay. Any selected local hide remains in effect.")
      : _("No relay has confirmed acceptance of this report. Publication may already have occurred. Any selected local hide remains in effect. Retry a destination below."));
}
static void lists_changed(GhSocialLists *lists, GhReportDialog *s)
{
  (void)lists;
  if (!current(s)) adw_dialog_force_close(ADW_DIALOG(s));
  else if (!s->id) preview(NULL, NULL, s);
}
static void publish_clicked(GtkButton *button, GhReportDialog *s)
{
  (void)button;
  if (!current(s) || s->id) return;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *json = payload(s, &error);
  if (json) s->id = gh_report_service_submit(s->service, json, (const gchar *const *)s->targets,
                                            adw_switch_row_get_active(s->hide_row), &error);
  if (error) gtk_label_set_text(s->status_label, error->message);
  if (!s->id) return;
  gtk_widget_set_sensitive(GTK_WIDGET(s->category_row), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(s->explanation_row), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(s->hide_row), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(s->publish_button), FALSE);
  gtk_button_set_label(s->cancel_button, _("_Close"));
  refresh(s->service, s);
}
static void cancel_clicked(GtkButton *button, GhReportDialog *s)
{
  (void)button; adw_dialog_close(ADW_DIALOG(s));
}
static void closed(AdwDialog *dialog)
{
  GhReportDialog *s = GH_REPORT_DIALOG(dialog);
  if (s->id && gh_report_service_get_busy(s->service)) gh_report_service_cancel(s->service);
}
static void dispose(GObject *object)
{
  GhReportDialog *s = GH_REPORT_DIALOG(object);
  if (s->service) g_signal_handlers_disconnect_by_data(s->service, s);
  if (s->lists) g_signal_handlers_disconnect_by_data(s->lists, s);
  g_clear_object(&s->service); g_clear_object(&s->lists);
  gtk_widget_dispose_template(GTK_WIDGET(s), GH_TYPE_REPORT_DIALOG);
  G_OBJECT_CLASS(gh_report_dialog_parent_class)->dispose(object);
}
static void finalize(GObject *object)
{
  GhReportDialog *s = GH_REPORT_DIALOG(object);
  g_free(s->account); g_free(s->target); g_free(s->public_json); g_free(s->room_id); g_strfreev(s->targets);
  G_OBJECT_CLASS(gh_report_dialog_parent_class)->finalize(object);
}
static void gh_report_dialog_class_init(GhReportDialogClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = dispose; G_OBJECT_CLASS(klass)->finalize = finalize;
  ADW_DIALOG_CLASS(klass)->closed = closed;
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  gtk_widget_class_set_template_from_resource(wc, "/org/nostr/Groundhog/ui/gh-report-dialog.ui");
#define BIND(n) gtk_widget_class_bind_template_child(wc, GhReportDialog, n)
  BIND(cancel_button); BIND(publish_button); BIND(category_row); BIND(explanation_row); BIND(hide_row);
  BIND(identity_label); BIND(tags_label); BIND(status_label); BIND(targets_box);
#undef BIND
}
static void gh_report_dialog_init(GhReportDialog *s)
{
  gtk_widget_init_template(GTK_WIDGET(s));
  const gchar *categories[] = {_("Nudity"), _("Malware"), _("Profanity"), _("Illegal Content"),
                              _("Spam"), _("Impersonation"), _("Other"), NULL};
  GtkStringList *model = gtk_string_list_new(categories);
  adw_combo_row_set_model(s->category_row, G_LIST_MODEL(model)); g_object_unref(model);
  adw_combo_row_set_selected(s->category_row, NOSTR_NIP56_OTHER);
  g_signal_connect(s->category_row, "notify::selected", G_CALLBACK(preview), s);
  g_signal_connect(s->explanation_row, "notify::text", G_CALLBACK(preview), s);
  g_signal_connect(s->publish_button, "clicked", G_CALLBACK(publish_clicked), s);
  g_signal_connect(s->cancel_button, "clicked", G_CALLBACK(cancel_clicked), s);
  /* This public action is irreversible: Return must not publish by default. */
  gtk_widget_set_focusable(GTK_WIDGET(s->cancel_button), TRUE);
  gtk_widget_grab_focus(GTK_WIDGET(s->cancel_button));
  adw_dialog_set_default_widget(ADW_DIALOG(s), GTK_WIDGET(s->cancel_button));
}
GhReportDialog *gh_report_dialog_new(GhReportService *service, GhSocialLists *lists,
    const gchar *target, const gchar *public_json, const gchar *room_id,
    GhReportPublicRoomFunc public_room, gpointer data)
{
  GhReportDialog *s = g_object_new(GH_TYPE_REPORT_DIALOG, NULL);
  s->service = g_object_ref(service); s->lists = g_object_ref(lists);
  s->account = g_strdup(gh_social_lists_get_account(lists)); s->target = g_strdup(target);
  s->public_json = g_strdup(public_json); s->room_id = g_strdup(room_id);
  s->public_room = public_room; s->public_data = data;
  s->generation = gh_social_lists_get_generation(lists); s->created_at = g_get_real_time() / G_USEC_PER_SEC;
  s->targets = gh_report_service_dup_targets(service);
  adw_dialog_set_title(ADW_DIALOG(s), public_json ? _("Report Message") : _("Report Account"));
  g_autofree gchar *identity = g_strdup_printf(_("Reporting identity: %s\nReported account: %s"), s->account, target);
  gtk_label_set_text(s->identity_label, identity);
  for (guint i = 0; s->targets && s->targets[i]; i++) {
    GtkWidget *label = gtk_label_new(s->targets[i]);
    gtk_label_set_selectable(GTK_LABEL(label), TRUE); gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    gtk_box_append(s->targets_box, label);
  }
  g_signal_connect(service, "changed", G_CALLBACK(refresh), s);
  g_signal_connect(lists, "changed", G_CALLBACK(lists_changed), s);
  preview(NULL, NULL, s); return s;
}
#define REPORT_CONTEXT "groundhog-report-ui"
typedef struct {
  GtkApplication *app;
  GhReportService *service;
  GhSocialLists *lists;
  GhReportPublicRoomFunc public_room;
  gpointer data;
  GhReportDialog *dialog;
  AdwDialog *history;
  guint64 generation;
} ReportUI;
static void clear_dialog(ReportUI *ui)
{
  if (ui->dialog) adw_dialog_force_close(ADW_DIALOG(ui->dialog));
  g_clear_object(&ui->dialog);
}
static void activate(GSimpleAction *action, GVariant *parameter, gpointer data)
{
  ReportUI *ui = data;
  const gchar *account, *target, *room = NULL, *json = NULL;
  if (!strcmp(g_action_get_name(G_ACTION(action)), "report-account"))
    g_variant_get(parameter, "(&s&s)", &account, &target);
  else g_variant_get(parameter, "(&s&s&s&s)", &account, &target, &room, &json);
  if (g_strcmp0(account, gh_social_lists_get_account(ui->lists)) || !g_strcmp0(account, target)
      || strlen(target) != 64 || (json && (!ui->public_room || !ui->public_room(ui->data, room)))) return;
  GtkWindow *window = gtk_application_get_active_window(ui->app);
  if (!window) return;
  clear_dialog(ui);
  ui->dialog = g_object_ref_sink(gh_report_dialog_new(ui->service, ui->lists, target, json, room, ui->public_room, ui->data));
  adw_dialog_present(ADW_DIALOG(ui->dialog), GTK_WIDGET(window));
}
gboolean gh_report_dialog_message_is_public(GtkWidget *widget, GhMessage *message)
{
  GtkRoot *root = gtk_widget_get_root(widget);
  GtkApplication *app = GTK_IS_WINDOW(root) ? gtk_window_get_application(GTK_WINDOW(root)) : NULL;
  ReportUI *ui = app ? g_object_get_data(G_OBJECT(app), REPORT_CONTEXT) : NULL;
  return ui && gh_message_is_nip29(message) && gh_message_is_signed(message)
      && ui->public_room
      && !g_strcmp0(gh_message_get_account(message), gh_social_lists_get_account(ui->lists))
      && ui->public_room(ui->data, gh_message_get_room_id(message));
}
static void review(GSimpleAction *action, GVariant *parameter, gpointer data)
{
  (void)action;
  ReportUI *ui = data;
  const gchar *account; gint64 id;
  g_variant_get(parameter, "(&sx)", &account, &id);
  if (g_strcmp0(account, gh_social_lists_get_account(ui->lists))) return;
  g_autoptr(GPtrArray) rows = gh_report_service_list(ui->service, NULL);
  GhStoreReport *r = NULL;
  for (guint i = 0; rows && i < rows->len; i++) {
    GhStoreReport *candidate = g_ptr_array_index(rows, i);
    if (candidate->id == id) { r = candidate; break; }
  }
  if (!r) return;
  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, r->unsigned_json, -1, NULL)) return;
  JsonObject *o = json_node_get_object(json_parser_get_root(parser));
  JsonArray *tags = json_object_get_array_member(o, "tags");
  const gchar *target = json_array_get_string_element(json_array_get_array_element(tags, 0), 1);
  GtkWindow *window = gtk_application_get_active_window(ui->app);
  if (!window) return;
  clear_dialog(ui);
  ui->dialog = g_object_ref_sink(gh_report_dialog_new(ui->service, ui->lists, target, NULL, NULL, NULL, NULL));
  GhReportDialog *s = ui->dialog;
  s->id = id; /* Stored disclosure is immutable, including its e tag. */
  g_autofree gchar *tag_text = json_to_string(json_object_get_member(o, "tags"), FALSE);
  gtk_label_set_text(s->tags_label, tag_text);
  gtk_editable_set_text(GTK_EDITABLE(s->explanation_row), json_object_get_string_member(o, "content"));
  NostrNip56Category category;
  if (!nostr_nip56_category_parse(json_array_get_string_element(json_array_get_array_element(tags, 0), 2), &category))
    adw_combo_row_set_selected(s->category_row, category);
  g_autoptr(JsonParser) destinations = json_parser_new();
  if (!json_parser_load_from_data(destinations, r->targets_json, -1, NULL)) return;
  JsonArray *urls = json_node_get_array(json_parser_get_root(destinations));
  g_strfreev(s->targets); s->targets = g_new0(gchar *, json_array_get_length(urls) + 1);
  for (guint i = 0; i < json_array_get_length(urls); i++) s->targets[i] = g_strdup(json_array_get_string_element(urls, i));
  gtk_widget_set_sensitive(GTK_WIDGET(s->category_row), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(s->explanation_row), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(s->hide_row), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(s->publish_button), FALSE);
  gtk_button_set_label(s->cancel_button, _("_Close"));
  refresh(ui->service, s);
  adw_dialog_present(ADW_DIALOG(s), GTK_WIDGET(window));
}
static void history(GSimpleAction *action, GVariant *parameter, gpointer data)
{
  (void)action; (void)parameter;
  ReportUI *ui = data;
  GtkWindow *window = gtk_application_get_active_window(ui->app);
  if (!window) return;
  if (ui->history) adw_dialog_force_close(ui->history);
  g_clear_object(&ui->history);
  ui->history = g_object_ref_sink(adw_dialog_new());
  ui->generation = gh_social_lists_get_generation(ui->lists);
  adw_dialog_set_title(ui->history, _("Reports"));
  adw_dialog_set_content_width(ui->history, 480);
  adw_dialog_set_content_height(ui->history, 500);
  GtkWidget *toolbar = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), adw_header_bar_new());
  GtkWidget *scroll = gtk_scrolled_window_new();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(box, 18); gtk_widget_set_margin_end(box, 18);
  gtk_widget_set_margin_top(box, 18); gtk_widget_set_margin_bottom(box, 18);
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) rows = gh_report_service_list(ui->service, &error);
  if (!rows || !rows->len) gtk_box_append(GTK_BOX(box), gtk_label_new(error ? error->message : _("No reports on this device")));
  for (guint i = 0; rows && i < rows->len; i++) {
    GhStoreReport *r = g_ptr_array_index(rows, i);
    g_autofree gchar *title = g_strdup_printf(_("Review Report %lld"), (long long)r->id);
    GtkWidget *button = gtk_button_new_with_label(title);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(button), "app.review-report");
    gtk_actionable_set_action_target_value(GTK_ACTIONABLE(button), g_variant_new("(sx)", gh_social_lists_get_account(ui->lists), r->id));
    gtk_box_append(GTK_BOX(box), button);
  }
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), scroll);
  adw_dialog_set_child(ui->history, toolbar);
  adw_dialog_present(ui->history, GTK_WIDGET(window));
}
static void ui_changed(GhSocialLists *lists, ReportUI *ui)
{
  if (ui->history && ui->generation != gh_social_lists_get_generation(lists)) {
    adw_dialog_force_close(ui->history); g_clear_object(&ui->history);
  }
}
static void ui_free(gpointer data)
{
  ReportUI *ui = data; clear_dialog(ui);
  if (ui->history) adw_dialog_force_close(ui->history);
  g_clear_object(&ui->history);
  g_signal_handlers_disconnect_by_data(ui->lists, ui);
  gh_report_service_close(ui->service);
  g_action_map_remove_action(G_ACTION_MAP(ui->app), "reports");
  g_action_map_remove_action(G_ACTION_MAP(ui->app), "review-report");
  g_action_map_remove_action(G_ACTION_MAP(ui->app), "report-account");
  g_action_map_remove_action(G_ACTION_MAP(ui->app), "report-public-message");
  g_object_unref(ui->service); g_object_unref(ui->lists); g_free(ui);
}
void gh_report_dialog_uninstall(GtkApplication *app) { g_object_set_data(G_OBJECT(app), REPORT_CONTEXT, NULL); }
void gh_report_dialog_install(GtkApplication *app, GhReportService *service, GhSocialLists *lists,
                              GhReportPublicRoomFunc public_room, gpointer data)
{
  gh_report_dialog_uninstall(app);
  ReportUI *ui = g_new0(ReportUI, 1);
  ui->app = app; ui->service = g_object_ref(service); ui->lists = g_object_ref(lists);
  ui->public_room = public_room; ui->data = data;
  const gchar *names[] = {"report-account", "report-public-message"};
  for (guint i = 0; i < G_N_ELEMENTS(names); i++) {
    GSimpleAction *a = g_simple_action_new(names[i], G_VARIANT_TYPE(i ? "(ssss)" : "(ss)"));
    g_signal_connect(a, "activate", G_CALLBACK(activate), ui);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(a)); g_object_unref(a);
  }
  GSimpleAction *reports = g_simple_action_new("reports", NULL);
  g_signal_connect(reports, "activate", G_CALLBACK(history), ui);
  g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(reports)); g_object_unref(reports);
  GSimpleAction *review_action = g_simple_action_new("review-report", G_VARIANT_TYPE("(sx)"));
  g_signal_connect(review_action, "activate", G_CALLBACK(review), ui);
  g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(review_action)); g_object_unref(review_action);
  g_signal_connect(lists, "changed", G_CALLBACK(ui_changed), ui);
  g_object_set_data_full(G_OBJECT(app), REPORT_CONTEXT, ui, ui_free);
}
