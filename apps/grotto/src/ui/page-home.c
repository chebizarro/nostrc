#include "page-home.h"
#include "app-resources.h"
#include "page-identity-detail.h"
#include "onboarding-assistant.h"
#include "sheets/sheet-qr-display.h"
#include "../accounts_store.h"

struct _GrottoPageHome {
  AdwPreferencesPage parent_instance;
  GtkBox *pending_slot, *avatars;
  AdwActionRow *identity_card;
  GtkButton *copy_key, *show_qr;
  GtkLabel *signed_today, *clients_count, *pending_count, *status_message;
  GtkListBox *clients_list;
  AdwStatusPage *empty_clients;
  GrottoHomeModel *model;
  gchar *identity;
  guint account_handler;
};
G_DEFINE_TYPE(GrottoPageHome, grotto_page_home, ADW_TYPE_PREFERENCES_PAGE)
static void identities(GrottoPageHome *self);
static void accounts_changed(AccountsStore *store, AccountsChangeType change, const gchar *id, gpointer data) {
  (void)store; (void)change; (void)id; identities(data);
}
static void identity_clicked(GtkButton *button, gpointer data) {
  GrottoPageHome *self = data;
  g_autofree gchar *id = g_strdup(g_object_get_data(G_OBJECT(button), "identity"));
  accounts_store_set_selected(accounts_store_get_default(), id);
  identities(self); page_identity_detail_present_identity(GTK_WIDGET(self), id);
}
static void add_identity(GtkButton *button, gpointer data) {
  (void)button; GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(data));
  if (GTK_IS_WINDOW(root)) onboarding_assistant_present(GTK_WINDOW(root));
}
static void identities(GrottoPageHome *self) {
  AccountsStore *store = accounts_store_get_default();
  g_autoptr(GPtrArray) rows = accounts_store_list(store);
  const gchar *selected = accounts_store_get_selected(store);
  if (!selected && rows && rows->len) {
    /* Management-only selection; never writes the daemon default. */
    AccountEntry *first = rows->pdata[0]; accounts_store_set_selected(store, first->id);
    selected = accounts_store_get_selected(store);
  }
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->avatars)))) gtk_box_remove(self->avatars, child);
  g_clear_pointer(&self->identity, g_free);
  for (guint i = 0; rows && i < rows->len; i++) {
    AccountEntry *entry = rows->pdata[i];
    const gchar *name = entry->label && *entry->label ? entry->label : "Identity";
    GtkWidget *button = gtk_button_new(); gtk_widget_add_css_class(button, "flat");
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *avatar = adw_avatar_new(40, name, TRUE); gtk_box_append(GTK_BOX(box), avatar);
    GtkWidget *label = gtk_label_new(entry->watch_only ? "Watch only" : name);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 10); gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(box), label); gtk_button_set_child(GTK_BUTTON(button), box);
    gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
    g_object_set_data_full(G_OBJECT(button), "identity", g_strdup(entry->id), g_free);
    g_signal_connect(button, "clicked", G_CALLBACK(identity_clicked), self); gtk_box_append(self->avatars, button);
    if (!g_strcmp0(selected, entry->id)) {
      self->identity = g_strdup(entry->id);
      gtk_widget_remove_css_class(button, "flat"); gtk_widget_add_css_class(button, "suggested-action");
      adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->identity_card), name);
      g_autofree gchar *text = g_strdup_printf("%.16s…%.8s%s", entry->id, entry->id + MAX(0, (gint)strlen(entry->id) - 8),
          entry->watch_only ? " · Watch only" : "");
      adw_action_row_set_subtitle(self->identity_card, text);
    }
  }
  GtkWidget *add = gtk_button_new_with_label("+ Add"); gtk_widget_set_valign(add, GTK_ALIGN_CENTER);
  g_signal_connect(add, "clicked", G_CALLBACK(add_identity), self); gtk_box_append(self->avatars, add);
  if (!self->identity) {
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->identity_card), "Choose an identity");
    adw_action_row_set_subtitle(self->identity_card, "Management selection only — apps keep their own identity.");
  }
  gtk_widget_set_sensitive(GTK_WIDGET(self->copy_key), self->identity != NULL);
  gtk_widget_set_sensitive(GTK_WIDGET(self->show_qr), self->identity != NULL);
}
static void copy_key(GtkButton *button, gpointer data) {
  (void)button; GrottoPageHome *self = data;
  if (self->identity) gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(self)), self->identity);
}
static void show_qr(GtkButton *button, gpointer data) {
  (void)button; GrottoPageHome *self = data; if (!self->identity) return;
  SheetQrDisplay *sheet = sheet_qr_display_new(); sheet_qr_display_set_npub(sheet, self->identity);
  adw_dialog_present(ADW_DIALOG(sheet), GTK_WIDGET(self));
}
static void client_activated(AdwActionRow *row, gpointer data) {
  g_signal_emit_by_name(data, "client-selected", g_object_get_data(G_OBJECT(row), "principal"));
}
static void number(GtkLabel *label, GVariant *status, const gchar *key) {
  guint n; g_autofree gchar *text = status && g_variant_lookup(status, key, "u", &n) ? g_strdup_printf("%u", n) : g_strdup("—");
  gtk_label_set_text(label, text);
}
static void render(GrottoHomeModel *model, gpointer data) {
  GrottoPageHome *self = data; GVariant *status = grotto_home_model_status(model);
  number(self->signed_today, status, "signed_today"); number(self->clients_count, status, "client_count");
  number(self->pending_count, status, "pending_count");
  gtk_label_set_text(self->status_message, grotto_home_model_error(model) ? grotto_home_model_error(model) :
      grotto_home_model_writable(model) ? "" : "Read-only: this signer version is not supported.");
  GtkWidget *child; while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->clients_list)))) gtk_list_box_remove(self->clients_list, child);
  GVariant *clients = grotto_home_model_clients(model), *requests = grotto_home_model_requests(model);
  for (gsize i = 0; clients && i < g_variant_n_children(clients); i++) {
    g_autoptr(GVariant) client = g_variant_get_child_value(clients, i);
    const gchar *principal = "", *label = "", *transport = "", *tier = "low";
    guint64 seen = 0; gboolean pairing = FALSE, review = FALSE; guint pending = 0;
    g_variant_lookup(client, "principal", "&s", &principal); g_variant_lookup(client, "label", "&s", &label);
    g_variant_lookup(client, "transport", "&s", &transport); g_variant_lookup(client, "trust-tier", "&s", &tier);
    g_variant_lookup(client, "last-used-at", "t", &seen); g_variant_lookup(client, "pairing-pending-ack", "b", &pairing);
    g_variant_lookup(client, "needs-review", "b", &review);
    for (gsize j = 0; requests && j < g_variant_n_children(requests); j++) {
      g_autoptr(GVariant) request = g_variant_get_child_value(requests, j); const gchar *who = NULL;
      if (g_variant_lookup(request, "principal", "&s", &who) && !g_strcmp0(who, principal)) pending++;
    }
    AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), *label ? label : principal);
    g_autoptr(GDateTime) date = seen ? g_date_time_new_from_unix_local(seen) : NULL;
    g_autofree gchar *when = date ? g_date_time_format(date, "%b %e, %H:%M") : g_strdup("Not yet used");
    const gchar *badge = !strcmp(transport, "NIP46") ? "Remote" :
        (g_str_has_prefix(principal, "https://") || g_str_has_prefix(principal, "http://")) ? "Browser bridge" :
        (!strcmp(transport, "DBUS") || !strcmp(transport, "NIP5F")) ? "Local app" : "Transport not yet observed";
    g_autofree gchar *subtitle = g_strdup_printf("Last seen: %s · %u pending%s%s", when, pending,
        pairing ? " · Waiting for relay acknowledgement" : "", review ? " · Permissions need review" : "");
    adw_action_row_set_subtitle(row, subtitle); adw_action_row_set_subtitle_lines(row, 3);
    adw_action_row_set_title_lines(row, 1);
    adw_action_row_add_prefix(row, adw_avatar_new(32, *label ? label : principal, TRUE));
    GtkWidget *badges = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4); gtk_widget_set_valign(badges, GTK_ALIGN_CENTER);
    GtkWidget *transport_badge = gtk_label_new(badge), *trust_badge = gtk_label_new(!strcmp(tier, "full") ? "Full" : !strcmp(tier, "medium") ? "Medium" : "Low");
    gtk_widget_add_css_class(transport_badge, "grotto-badge"); gtk_widget_add_css_class(trust_badge, "grotto-badge");
    gtk_widget_add_css_class(trust_badge, !strcmp(tier, "full") ? "accent" : "dim-label");
    gtk_box_append(GTK_BOX(badges), transport_badge); gtk_box_append(GTK_BOX(badges), trust_badge);
    adw_action_row_add_suffix(row, badges);
    adw_action_row_add_suffix(row, gtk_image_new_from_icon_name("go-next-symbolic"));
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
    g_object_set_data_full(G_OBJECT(row), "principal", g_strdup(principal), g_free);
    g_signal_connect(row, "activated", G_CALLBACK(client_activated), self); gtk_list_box_append(self->clients_list, GTK_WIDGET(row));
  }
  gtk_widget_set_visible(GTK_WIDGET(self->empty_clients), clients && !g_variant_n_children(clients));
}
static void mapped(GtkWidget *widget, gpointer data) {
  (void)data; GrottoPageHome *self = GROTTO_PAGE_HOME(widget); identities(self);
  if (self->model) grotto_home_model_refresh(self->model);
}
void grotto_page_home_set_model(GrottoPageHome *self, GrottoHomeModel *model) {
  if (self->model) g_signal_handlers_disconnect_by_data(self->model, self);
  g_set_object(&self->model, model); g_signal_connect(model, "changed", G_CALLBACK(render), self); render(model, self); identities(self);
}
void grotto_page_home_set_pending_group(GrottoPageHome *self, GtkWidget *group) { gtk_box_prepend(self->pending_slot, group); }
static void dispose(GObject *object) {
  GrottoPageHome *self = GROTTO_PAGE_HOME(object);
  if (self->model) g_signal_handlers_disconnect_by_data(self->model, self);
  if (self->account_handler) accounts_store_disconnect_changed(accounts_store_get_default(), self->account_handler);
  self->account_handler = 0;
  g_clear_object(&self->model); gtk_widget_dispose_template(GTK_WIDGET(self), GROTTO_TYPE_PAGE_HOME);
  G_OBJECT_CLASS(grotto_page_home_parent_class)->dispose(object);
}
static void finalize(GObject *object) { g_free(GROTTO_PAGE_HOME(object)->identity); G_OBJECT_CLASS(grotto_page_home_parent_class)->finalize(object); }
static void grotto_page_home_class_init(GrottoPageHomeClass *klass) {
  GObjectClass *oc = G_OBJECT_CLASS(klass); oc->dispose = dispose; oc->finalize = finalize;
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass); gtk_widget_class_set_template_from_resource(wc, APP_RESOURCE_PATH "/ui/page-home.ui");
#define BIND(x) gtk_widget_class_bind_template_child(wc, GrottoPageHome, x)
  BIND(pending_slot); BIND(avatars); BIND(identity_card); BIND(copy_key); BIND(show_qr);
  BIND(signed_today); BIND(clients_count); BIND(pending_count); BIND(status_message); BIND(clients_list); BIND(empty_clients);
#undef BIND
  g_signal_new("client-selected", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
}
static void grotto_page_home_init(GrottoPageHome *self) {
  gtk_widget_init_template(GTK_WIDGET(self));
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(self->identity_card), FALSE);
  g_signal_connect(self->copy_key, "clicked", G_CALLBACK(copy_key), self);
  g_signal_connect(self->show_qr, "clicked", G_CALLBACK(show_qr), self);
  g_signal_connect(self, "map", G_CALLBACK(mapped), NULL);
  self->account_handler = accounts_store_connect_changed(accounts_store_get_default(), accounts_changed, self);
}
