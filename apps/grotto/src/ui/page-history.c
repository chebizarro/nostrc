#include "../i18n.h"
#include "page-history.h"
#include "app-resources.h"
#include "approval_dialog.h"
struct _GnPageHistory {
  AdwPreferencesPage parent_instance;
  GtkSearchEntry *client_filter; AdwEntryRow *kind_filter; AdwComboRow *outcome;
  GtkListBox *records; GtkLabel *status; GtkButton *older, *clear;
  GrottoHomeModel *model; GVariant *rows; gboolean busy;
};
G_DEFINE_TYPE(GnPageHistory, gn_page_history, ADW_TYPE_PREFERENCES_PAGE)
static const gchar *field(GVariant *v, const gchar *key) { const gchar *s = ""; g_variant_lookup(v, key, "&s", &s); return s; }
const gchar *gn_page_history_operation_label(const gchar *operation) {
  static const struct { const gchar *operation, *label; } names[] = {
    {"event", N_("Sign event")}, {"sign_event", N_("Sign event")}, {"get_public_key", N_("Share public key")},
    {"get_relays", N_("Read relay list")}, {"ping", N_("Check connection")}, {"connect", N_("Connect app")},
    {"nip04_encrypt", N_("Encrypt message (legacy)")}, {"nip04_decrypt", N_("Decrypt message (legacy)")},
    {"nip44_encrypt", N_("Encrypt message")}, {"nip44_decrypt", N_("Decrypt message")},
    {"nip44_conversation_key", N_("Export conversation key")}, {"zap_decrypt", N_("Decrypt payment")},
    {"self_test", N_("Verify identity")}
  };
  for (guint i = 0; i < G_N_ELEMENTS(names); i++) if (!g_strcmp0(operation, names[i].operation)) return _(names[i].label);
  return operation && *operation ? _("Unknown operation") : _("Operation");
}
const gchar *gn_page_history_outcome_label(const gchar *outcome) {
  if (!g_strcmp0(outcome, "success")) return _("Succeeded");
  if (!g_strcmp0(outcome, "denied")) return _("Denied");
  if (!g_strcmp0(outcome, "expired")) return _("Expired");
  if (!g_strcmp0(outcome, "expired-caller")) return _("Caller disconnected");
  if (!g_strcmp0(outcome, "cancelled")) return _("Cancelled");
  if (!g_strcmp0(outcome, "failed")) return _("Failed");
  return _("Unknown outcome");
}
const gchar *gn_page_history_remote_state_label(const gchar *state) {
  if (!g_strcmp0(state, "DISABLED")) return _("Disabled");
  if (!g_strcmp0(state, "STARTING")) return _("Starting");
  if (!g_strcmp0(state, "LISTENING")) return _("Ready");
  if (!g_strcmp0(state, "DEGRADED")) return _("Degraded");
  if (!g_strcmp0(state, "OFFLINE")) return _("Offline");
  if (!g_strcmp0(state, "STOPPING")) return _("Stopping");
  if (!g_strcmp0(state, "ERROR")) return _("Error");
  return _("Unavailable");
}
static void render(GnPageHistory *self) {
  GtkWidget *child; while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->records)))) gtk_list_box_remove(self->records, child);
  const gchar *client = gtk_editable_get_text(GTK_EDITABLE(self->client_filter)), *kind_text = gtk_editable_get_text(GTK_EDITABLE(self->kind_filter));
  gchar *end = NULL; gint64 filter = *kind_text ? g_ascii_strtoll(kind_text, &end, 10) : -1;
  if (*kind_text && (end == kind_text || *end || filter < 0 || filter > G_MAXUINT16)) { gtk_label_set_text(self->status, _("Enter a whole event-type number from 0 to 65535.")); return; }
  const gchar *outcomes[] = {"", "success", "denied", "expired", "expired-caller", "cancelled", "failed"};
  guint selected = adw_combo_row_get_selected(self->outcome), shown = 0;
  for (gsize i = 0; self->rows && i < g_variant_n_children(self->rows); i++) {
    g_autoptr(GVariant) item = g_variant_get_child_value(self->rows, i); gint kind = -1; guint64 timestamp = 0;
    guint32 event_kind; if (g_variant_lookup(item, "event_kind", "u", &event_kind)) kind = event_kind;
    g_variant_lookup(item, "timestamp", "t", &timestamp);
    if ((filter >= 0 && kind != filter) || (*client && !strstr(field(item, "principal"), client)) ||
        (selected && selected < G_N_ELEMENTS(outcomes) && strcmp(field(item, "outcome"), outcomes[selected]))) continue;
    AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new()); adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    g_autofree gchar *title = kind >= 0 ? g_strdup_printf(_("%s · %s (%d)"), gn_page_history_operation_label(field(item, "operation")), gnostr_approval_kind_name(kind), kind) : g_strdup(gn_page_history_operation_label(field(item, "operation")));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    g_autoptr(GDateTime) date = timestamp ? g_date_time_new_from_unix_local(timestamp) : NULL;
    g_autofree gchar *when = date ? g_date_time_format(date, _("%F %R")) : g_strdup("");
    g_autofree gchar *subtitle = g_strdup_printf(_("%s · %s\n%s"), gn_page_history_outcome_label(field(item, "outcome")), when, field(item, "principal"));
    adw_action_row_set_subtitle(row, subtitle); adw_action_row_set_subtitle_lines(row, 3); gtk_list_box_append(self->records, GTK_WIDGET(row)); shown++;
  }
  gtk_label_set_text(self->status, self->model && grotto_home_model_error(self->model) ? grotto_home_model_error(self->model) : shown ? "" : _("No matching activity"));
  gtk_widget_set_sensitive(GTK_WIDGET(self->older), self->rows && g_variant_n_children(self->rows) && !self->busy && grotto_home_model_writable(self->model));
  gtk_widget_set_sensitive(GTK_WIDGET(self->clear), self->model && grotto_home_model_writable(self->model) && !self->busy);
}
static void filtered(GtkWidget *widget, gpointer data) { (void)widget; render(data); }
static void outcome_changed(GObject *object, GParamSpec *pspec, gpointer data) { (void)object; (void)pspec; render(data); }
static void model_changed(GrottoHomeModel *model, gpointer data) {
  GnPageHistory *self = data; g_clear_pointer(&self->rows, g_variant_unref);
  GVariant *rows = grotto_home_model_activity(model); if (rows) self->rows = g_variant_ref(rows); render(self);
}
static void older_done(GObject *source, GAsyncResult *result, gpointer data) {
  GnPageHistory *self = data; g_autoptr(GError) error = NULL; g_autoptr(GVariant) reply = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), result, &error);
  self->busy = FALSE;
  if (reply && self->rows) {
    g_autoptr(GVariant) incoming = g_variant_get_child_value(reply, 0); GVariantBuilder b; g_variant_builder_init(&b, G_VARIANT_TYPE("aa{sv}"));
    for (gsize i = 0; i < g_variant_n_children(self->rows); i++) { g_autoptr(GVariant) row = g_variant_get_child_value(self->rows, i); g_variant_builder_add_value(&b, row); }
    for (gsize i = 0; i < g_variant_n_children(incoming); i++) { g_autoptr(GVariant) row = g_variant_get_child_value(incoming, i); g_variant_builder_add_value(&b, row); }
    g_clear_pointer(&self->rows, g_variant_unref); self->rows = g_variant_ref_sink(g_variant_builder_end(&b)); render(self);
    if (!g_variant_n_children(incoming)) { gtk_label_set_text(self->status, _("End of retained activity")); gtk_widget_set_sensitive(GTK_WIDGET(self->older), FALSE); }
  } else { render(self); gtk_label_set_text(self->status, _("Older activity could not be loaded.")); }
  g_object_unref(self);
}
static void older(GtkButton *button, gpointer data) {
  (void)button; GnPageHistory *self = data; if (!self->rows || !g_variant_n_children(self->rows) || self->busy) return;
  g_autoptr(GVariant) last = g_variant_get_child_value(self->rows, g_variant_n_children(self->rows) - 1); guint64 before = 0; g_variant_lookup(last, "sequence", "t", &before);
  self->busy = TRUE; render(self); g_dbus_proxy_call(grotto_home_model_proxy(self->model), "ListActivity", g_variant_new("(tu)", before, 100u), G_DBUS_CALL_FLAGS_NONE, 10000, NULL, older_done, g_object_ref(self));
}
static void clear_done(GObject *source, GAsyncResult *result, gpointer data) {
  GnPageHistory *self = data; g_autoptr(GError) error = NULL; g_autoptr(GVariant) reply = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), result, &error);
  if (!reply) gtk_label_set_text(self->status, _("Activity could not be cleared."));
  grotto_home_model_refresh(self->model); g_object_unref(self);
}
static void clear_confirmed(GObject *source, GAsyncResult *result, gpointer data) {
  GnPageHistory *self = data;
  if (!strcmp(adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result), "clear") && grotto_home_model_writable(self->model))
    g_dbus_proxy_call(grotto_home_model_proxy(self->model), "ClearActivity", NULL, G_DBUS_CALL_FLAGS_NONE, 10000, NULL, clear_done, g_object_ref(self));
  g_object_unref(self);
}
static void clear(GtkButton *button, gpointer data) {
  (void)button; AdwDialog *d = adw_alert_dialog_new(_("Clear activity?"), _("Delete the signer's retained operation metadata? Permissions are not changed."));
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(d), "cancel", _("Cancel"), "clear", _("Clear activity"), NULL);
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(d), "cancel"); adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(d), "clear", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_choose(ADW_ALERT_DIALOG(d), data, NULL, clear_confirmed, g_object_ref(data));
}
void gn_page_history_set_model(GnPageHistory *self, GrottoHomeModel *model) { g_set_object(&self->model, model); g_signal_connect(model, "changed", G_CALLBACK(model_changed), self); model_changed(model, self); }
void gn_page_history_refresh(GnPageHistory *self) { if (self->model) grotto_home_model_refresh(self->model); }
void gn_page_history_clear_filters(GnPageHistory *self) { gtk_editable_set_text(GTK_EDITABLE(self->client_filter), ""); gtk_editable_set_text(GTK_EDITABLE(self->kind_filter), ""); adw_combo_row_set_selected(self->outcome, 0); }
static void dispose(GObject *object) {
  GnPageHistory *self = GN_PAGE_HISTORY(object); if (self->model) g_signal_handlers_disconnect_by_data(self->model, self);
  g_clear_object(&self->model); g_clear_pointer(&self->rows, g_variant_unref); gtk_widget_dispose_template(GTK_WIDGET(self), GN_TYPE_PAGE_HISTORY);
  G_OBJECT_CLASS(gn_page_history_parent_class)->dispose(object);
}
static void gn_page_history_class_init(GnPageHistoryClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = dispose; GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass); gtk_widget_class_set_template_from_resource(wc, APP_RESOURCE_PATH "/ui/page-history.ui");
#define BIND(x) gtk_widget_class_bind_template_child(wc, GnPageHistory, x)
  BIND(client_filter); BIND(kind_filter); BIND(outcome); BIND(records); BIND(status); BIND(older); BIND(clear);
#undef BIND
}
static void gn_page_history_init(GnPageHistory *self) {
  gtk_widget_init_template(GTK_WIDGET(self)); g_signal_connect(self->client_filter, "search-changed", G_CALLBACK(filtered), self);
  g_signal_connect(self->kind_filter, "changed", G_CALLBACK(filtered), self); g_signal_connect(self->outcome, "notify::selected", G_CALLBACK(outcome_changed), self);
  g_signal_connect(self->older, "clicked", G_CALLBACK(older), self); g_signal_connect(self->clear, "clicked", G_CALLBACK(clear), self);
}
GnPageHistory *gn_page_history_new(void) { return g_object_new(GN_TYPE_PAGE_HISTORY, NULL); }
