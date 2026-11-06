/* SPDX-License-Identifier: MIT */
#include "grotto-panels.h"
#include "grotto-panel-labels.h"
struct _GrottoActivityPanel {
  GrottoPanel parent_instance;
  GtkSearchEntry *search;
  AdwEntryRow *kind;
  AdwComboRow *outcome, *catalog;
  AdwBanner *feedback;
  GtkListView *records;
  GtkButton *older, *clear;
  GListModel *live;
  GListStore *pages;
  GtkCustomFilter *filter;
  char *principal;
  guint64 generation;
  gboolean busy, end;
};
G_DEFINE_TYPE(GrottoActivityPanel, grotto_activity_panel, GROTTO_TYPE_PANEL)
static const char *outcomes[] = {"", "success", "denied", "expired", "expired-caller", "cancelled", "failed"};
static const char *outcome_labels[] = {N_("Unknown Outcome"), N_("Succeeded"), N_("Denied"), N_("Expired"), N_("Caller Disconnected"), N_("Cancelled"), N_("Failed")};
static gboolean contains(const char *text, const char *query) {
  g_autofree char *haystack = g_utf8_casefold(text ? text : "", -1);
  g_autofree char *needle = g_utf8_casefold(query ? query : "", -1);
  return strstr(haystack, needle) != NULL;
}
static gboolean matches(gpointer item, gpointer data) {
  GrottoActivityPanel *self = data; const GrottoPanelsMetadata *m = grotto_panels_item_get_metadata(item);
  const char *search = gtk_editable_get_text(GTK_EDITABLE(self->search));
  const char *kind = gtk_editable_get_text(GTK_EDITABLE(self->kind));
  if (self->principal && !strcmp(search, self->principal) && strcmp(m->principal, self->principal)) return FALSE;
  guint outcome = adw_combo_row_get_selected(self->outcome);
  if (outcome < G_N_ELEMENTS(outcomes) && outcome && strcmp(m->outcome, outcomes[outcome])) return FALSE;
  g_autofree char *app_name = grotto_panels_app_display_info(m->principal, m->label, NULL);
  if (*search && !contains(m->principal, search) && !contains(app_name, search) && !contains(operation_name(m->operation), search)) return FALSE;
  if (*kind) {
    char *end = NULL; gint64 number = g_ascii_strtoll(kind, &end, 10);
    if (!m->has_event_kind || (end != kind && !*end ? number != m->event_kind : !contains(kind_name(m->event_kind), kind))) return FALSE;
  }
  return TRUE;
}
static void filtered(GObject *object, gpointer data) { (void)object; gtk_filter_changed(GTK_FILTER(GROTTO_ACTIVITY_PANEL(data)->filter), GTK_FILTER_CHANGE_DIFFERENT); }
static void outcome_changed(GObject *object, GParamSpec *spec, gpointer data) { (void)spec; filtered(object, data); }
static void catalog_changed(GObject *object, GParamSpec *spec, gpointer data) {
  (void)object; (void)spec; GrottoActivityPanel *self = data; guint selected = adw_combo_row_get_selected(self->catalog);
  g_autofree char *number = selected && selected <= G_N_ELEMENTS(kinds) ? g_strdup_printf("%u", kinds[selected - 1].kind) : g_strdup("");
  gtk_editable_set_text(GTK_EDITABLE(self->kind), number);
}
static void setup(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data) {
  (void)factory; (void)data; GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_margin_start(box, 18); gtk_widget_set_margin_end(box, 18); gtk_widget_set_margin_top(box, 8); gtk_widget_set_margin_bottom(box, 8);
  for (guint i = 0; i < 2; i++) {
    GtkWidget *label = gtk_label_new(NULL); gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE); gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    if (i) gtk_widget_add_css_class(label, "dim-label");
    gtk_box_append(GTK_BOX(box), label);
  }
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  GtkWidget *icon = gtk_image_new();
  gtk_widget_set_margin_start(icon, 18);
  gtk_widget_set_hexpand(box, TRUE);
  gtk_box_append(GTK_BOX(row), icon);
  gtk_box_append(GTK_BOX(row), box);
  gtk_list_item_set_child(item, row);
}
static void bind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data) {
  (void)factory; (void)data;
  const GrottoPanelsMetadata *m = grotto_panels_item_get_metadata(gtk_list_item_get_item(item));
  GtkWidget *icon = gtk_widget_get_first_child(gtk_list_item_get_child(item));
  GtkWidget *title = gtk_widget_get_first_child(gtk_widget_get_next_sibling(icon)), *subtitle = gtk_widget_get_next_sibling(title);
  g_autoptr(GIcon) app_icon = NULL;
  g_autofree char *app_name = grotto_panels_app_display_info(m->principal, m->label, &app_icon);
  gtk_image_set_from_gicon(GTK_IMAGE(icon), app_icon);
  g_autofree char *name = m->has_event_kind ? g_strdup_printf("%s · %s (%d)", operation_name(m->operation), kind_name(m->event_kind), m->event_kind) : g_strdup(operation_name(m->operation));
  g_autofree char *heading = g_strdup_printf(_("%s · %s"), app_name, name);
  gtk_label_set_text(GTK_LABEL(title), heading);
  guint outcome = 0; for (guint i = 1; i < G_N_ELEMENTS(outcomes); i++) if (!strcmp(m->outcome, outcomes[i])) outcome = i;
  g_autoptr(GDateTime) date = m->timestamp ? g_date_time_new_from_unix_local(m->timestamp) : NULL;
  g_autofree char *when = date ? g_date_time_format(date, "%F %R") : g_strdup("");
  g_autofree char *detail = g_strdup_printf("%s · %s\n%s", _(outcome_labels[outcome]), when, m->principal);
  gtk_label_set_text(GTK_LABEL(subtitle), detail);
}
static void controls(GrottoActivityPanel *self) {
  GrottoPanel *panel = GROTTO_PANEL(self);
  gboolean available = grotto_panels_service_is_available(grotto_panel_get_service(panel));
  adw_banner_set_revealed(self->feedback, !available);
  gtk_widget_set_sensitive(GTK_WIDGET(self->older), available && !self->busy && !self->end && g_list_model_get_n_items(G_LIST_MODEL(self->pages)) > 0);
  gtk_widget_set_sensitive(GTK_WIDGET(self->clear), !self->busy && grotto_panel_can_command(panel, GROTTO_PANEL_CLEAR_ACTIVITY));
}
static void live_changed(GListModel *model, guint p, guint r, guint a, gpointer data) {
  (void)p; (void)r; (void)a; GrottoActivityPanel *self = data;
  self->generation++; self->end = FALSE;
  g_list_store_remove_all(self->pages);
  for (guint i = 0; model && i < g_list_model_get_n_items(model); i++) {
    g_autoptr(GrottoPanelsItem) item = g_list_model_get_item(model, i); g_list_store_append(self->pages, item);
  }
  controls(self);
}
static void attach(GrottoPanel *panel, gpointer data) {
  (void)data; GrottoActivityPanel *self = GROTTO_ACTIVITY_PANEL(panel);
  GrottoPanelsService *service = grotto_panel_get_service(panel);
  GListModel *model = service ? grotto_panels_service_get_model(service, GROTTO_PANELS_ACTIVITY) : NULL;
  if (self->live != model) {
    if (self->live) g_signal_handlers_disconnect_by_data(self->live, self);
    g_set_object(&self->live, model);
    if (model) g_signal_connect(model, "items-changed", G_CALLBACK(live_changed), self);
    live_changed(model, 0, 0, 0, self);
  }
  controls(self);
}
typedef struct { GrottoActivityPanel *panel; guint64 generation; } PageRead;
static void older_done(GObject *source, GAsyncResult *result, gpointer data) {
  PageRead *read = data; GrottoActivityPanel *self = read->panel; g_autoptr(GError) error = NULL;
  g_autoptr(GListModel) page = grotto_panels_service_query_finish(GROTTO_PANELS_SERVICE(source), result, &error);
  self->busy = FALSE;
  if (page && read->generation == self->generation) {
    self->end = g_list_model_get_n_items(page) == 0;
    for (guint i = 0; i < g_list_model_get_n_items(page); i++) {
      g_autoptr(GrottoPanelsItem) item = g_list_model_get_item(page, i); gboolean seen = FALSE;
      for (guint j = 0; j < g_list_model_get_n_items(G_LIST_MODEL(self->pages)); j++) {
        g_autoptr(GrottoPanelsItem) old = g_list_model_get_item(G_LIST_MODEL(self->pages), j);
        if (grotto_panels_item_get_metadata(old)->sequence == grotto_panels_item_get_metadata(item)->sequence) { seen = TRUE; break; }
      }
      if (!seen) g_list_store_append(self->pages, item);
    }
  }
  controls(self);
  if (error) { adw_banner_set_title(self->feedback, _("Older activity could not be loaded. Try again.")); adw_banner_set_revealed(self->feedback, TRUE); }
  g_object_unref(self); g_free(read);
}
static void older(GtkButton *button, gpointer data) {
  (void)button; GrottoActivityPanel *self = data; guint count = g_list_model_get_n_items(G_LIST_MODEL(self->pages));
  if (self->busy || !count) return;
  g_autoptr(GrottoPanelsItem) last = g_list_model_get_item(G_LIST_MODEL(self->pages), count - 1);
  PageRead *read = g_new0(PageRead, 1); *read = (PageRead){g_object_ref(self), self->generation}; self->busy = TRUE;
  grotto_panels_service_query_async(grotto_panel_get_service(GROTTO_PANEL(self)), GROTTO_PANELS_QUERY_ACTIVITY, NULL,
    grotto_panels_item_get_metadata(last)->sequence, grotto_panel_get_cancellable(GROTTO_PANEL(self)), older_done, read);
  controls(self);
}
static void clear_done(GObject *source, GAsyncResult *result, gpointer data) {
  GrottoActivityPanel *self = data; g_autoptr(GError) error = NULL;
  gboolean ok = grotto_panel_command_finish(GROTTO_PANEL(source), result, &error); self->busy = FALSE; controls(self);
  if (!ok) { adw_banner_set_title(self->feedback, _("Activity wasn't cleared. Try again.")); adw_banner_set_revealed(self->feedback, TRUE); }
  g_object_unref(self);
}
static void clear_confirmed(GObject *source, GAsyncResult *result, gpointer data) {
  GrottoActivityPanel *self = data;
  if (!strcmp(adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result), "clear")) {
    self->busy = TRUE; grotto_panel_command_async(GROTTO_PANEL(self), GROTTO_PANEL_CLEAR_ACTIVITY, NULL, clear_done, g_object_ref(self)); controls(self);
  }
  g_object_unref(self);
}
static void clear(GtkButton *button, gpointer data) {
  (void)button; GrottoActivityPanel *self = data;
  AdwDialog *dialog = adw_alert_dialog_new(_("Clear Activity?"), _("Delete retained operation metadata? Permissions are not changed."));
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", _("Cancel"), "clear", _("Clear Activity"), NULL);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel"); adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "clear", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), GTK_WIDGET(self), grotto_panel_get_cancellable(GROTTO_PANEL(self)), clear_confirmed, g_object_ref(self));
}
static void dispose(GObject *object) {
  GrottoActivityPanel *self = GROTTO_ACTIVITY_PANEL(object);
  if (self->live) g_signal_handlers_disconnect_by_data(self->live, self);
  g_clear_object(&self->live); g_clear_object(&self->pages); g_clear_object(&self->filter);
  g_clear_pointer(&self->principal, g_free);
  adw_navigation_page_set_child(ADW_NAVIGATION_PAGE(self), NULL); gtk_widget_dispose_template(GTK_WIDGET(self), GROTTO_TYPE_ACTIVITY_PANEL);
  G_OBJECT_CLASS(grotto_activity_panel_parent_class)->dispose(object);
}
static void grotto_activity_panel_class_init(GrottoActivityPanelClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = dispose; GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  gtk_widget_class_set_template_from_resource(wc, "/org/nostr/GrottoPanels/activity-panel.ui");
#define BIND(x) gtk_widget_class_bind_template_child(wc, GrottoActivityPanel, x)
  BIND(search); BIND(kind); BIND(outcome); BIND(catalog); BIND(feedback); BIND(records); BIND(older); BIND(clear);
#undef BIND
}
static void grotto_activity_panel_init(GrottoActivityPanel *self) {
  gtk_widget_init_template(GTK_WIDGET(self)); self->pages = g_list_store_new(GROTTO_TYPE_PANELS_ITEM);
  self->filter = gtk_custom_filter_new(matches, self, NULL);
  GtkFilterListModel *filtered_model = gtk_filter_list_model_new(G_LIST_MODEL(g_object_ref(self->pages)), GTK_FILTER(g_object_ref(self->filter)));
  GtkSelectionModel *selection = GTK_SELECTION_MODEL(gtk_no_selection_new(G_LIST_MODEL(filtered_model)));
  gtk_list_view_set_model(self->records, selection); g_object_unref(selection);
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_signal_connect(factory, "setup", G_CALLBACK(setup), NULL); g_signal_connect(factory, "bind", G_CALLBACK(bind), NULL);
  gtk_list_view_set_factory(self->records, factory); g_object_unref(factory);
  GtkStringList *catalog = gtk_string_list_new(NULL); gtk_string_list_append(catalog, _("All Event Types"));
  for (guint i = 0; i < G_N_ELEMENTS(kinds); i++) {
    g_autofree char *caption = g_strdup_printf("%s (%u)", _(kinds[i].caption), kinds[i].kind); gtk_string_list_append(catalog, caption);
  }
  adw_combo_row_set_model(self->catalog, G_LIST_MODEL(catalog)); g_object_unref(catalog);
  g_signal_connect(self, "service-changed", G_CALLBACK(attach), NULL);
  g_signal_connect(self->search, "search-changed", G_CALLBACK(filtered), self); g_signal_connect(self->kind, "changed", G_CALLBACK(filtered), self);
  g_signal_connect(self->outcome, "notify::selected", G_CALLBACK(outcome_changed), self); g_signal_connect(self->catalog, "notify::selected", G_CALLBACK(catalog_changed), self);
  g_signal_connect(self->older, "clicked", G_CALLBACK(older), self); g_signal_connect(self->clear, "clicked", G_CALLBACK(clear), self);
}
GtkWidget *grotto_activity_panel_new(GrottoPanelsService *service) { return g_object_new(GROTTO_TYPE_ACTIVITY_PANEL, "service", service, NULL); }
void grotto_activity_panel_filter_app(GrottoActivityPanel *self, const char *principal) {
  if (!grotto_panels_public_text_validate(principal, NULL)) return;
  g_free(self->principal); self->principal = g_strdup(principal);
  gtk_editable_set_text(GTK_EDITABLE(self->search), principal ? principal : "");
  filtered(G_OBJECT(self->search), self);
}
