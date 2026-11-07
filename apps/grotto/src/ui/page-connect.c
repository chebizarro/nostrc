#include "../i18n.h"
#include "page-connect.h"
#include "app-resources.h"
#include "pairing-flow.h"
#include "sheets/sheet-qr-scanner.h"
#include "sheets/sheet-qr-display.h"
#include <secure_buf.h>
#include <string.h>

struct _GrottoPageConnect {
  AdwPreferencesPage parent_instance;
  AdwActionRow *paste, *scan, *show_link;
  GrottoHomeModel *model;
};
G_DEFINE_TYPE(GrottoPageConnect, grotto_page_connect, ADW_TYPE_PREFERENCES_PAGE)
static void weak_free(gpointer data) { GWeakRef *ref = data; g_weak_ref_clear(ref); g_free(ref); }
static void paste_ready(GObject *source, GAsyncResult *result, gpointer data) {
  GWeakRef *ref = data; g_autoptr(GtkWidget) page = g_weak_ref_get(ref);
  gchar *text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(source), result, NULL);
  if (page && gtk_widget_get_mapped(page)) {
    if (text && g_str_has_prefix(text, "nostrconnect://")) grotto_pairing_present(page, text);
    else adw_action_row_set_subtitle(GROTTO_PAGE_CONNECT(page)->paste, _("Copy a nostrconnect:// connection link from the remote app first"));
  }
  if (text) { secure_wipe(text, strlen(text)); g_free(text); }
  weak_free(ref);
}
static void paste(AdwActionRow *row, gpointer data) {
  (void)row; GWeakRef *ref = g_new0(GWeakRef, 1); g_weak_ref_init(ref, data);
  gdk_clipboard_read_text_async(gtk_widget_get_clipboard(GTK_WIDGET(data)), NULL, paste_ready, ref);
}
static void scanned(const gchar *text, GnQrContentType type, gpointer data) {
  (void)type; SheetQrScanner *scanner = data; GWeakRef *ref = g_object_get_data(G_OBJECT(scanner), "connect-page");
  g_autoptr(GtkWidget) page = g_weak_ref_get(ref);
  if (page && gtk_widget_get_mapped(page)) grotto_pairing_present(page, text);
}
static void scan(AdwActionRow *row, gpointer data) {
  (void)row; SheetQrScanner *sheet = sheet_qr_scanner_new();
  const GnQrContentType types[] = {GN_QR_TYPE_BUNKER_URI, GN_QR_TYPE_UNKNOWN};
  sheet_qr_scanner_set_accepted_types(sheet, types);
  GWeakRef *ref = g_new0(GWeakRef, 1); g_weak_ref_init(ref, data);
  g_object_set_data_full(G_OBJECT(sheet), "connect-page", ref, weak_free);
  sheet_qr_scanner_set_on_success(sheet, scanned, sheet); adw_dialog_present(ADW_DIALOG(sheet), data);
}
static void show_link(AdwActionRow *row, gpointer data) {
  (void)row;
  if (!gtk_widget_activate_action(GTK_WIDGET(data), "win.show-connection-link", NULL))
    adw_dialog_present(ADW_DIALOG(sheet_qr_display_new_connection()), data);
}
static void model_changed(GrottoHomeModel *model, gpointer data) {
  GrottoPageConnect *self = data; GVariant *status = grotto_home_model_status(model); gboolean enabled = FALSE;
  if (status) g_variant_lookup(status, "bunker_enabled", "b", &enabled);
  adw_action_row_set_subtitle(self->show_link, enabled ? _("Let a remote app paste or scan this signer’s link") :
    _("Remote signing is disabled. Open to enable it and show a connection link."));
}
void grotto_page_connect_set_model(GrottoPageConnect *self, GrottoHomeModel *model) {
  if (self->model) g_signal_handlers_disconnect_by_data(self->model, self);
  g_set_object(&self->model, model);
  if (model) { g_signal_connect(model, "changed", G_CALLBACK(model_changed), self); model_changed(model, self); }
}
static void dispose(GObject *object) {
  GrottoPageConnect *self = GROTTO_PAGE_CONNECT(object);
  if (self->model) g_signal_handlers_disconnect_by_data(self->model, self);
  g_clear_object(&self->model);
  gtk_widget_dispose_template(GTK_WIDGET(object), GROTTO_TYPE_PAGE_CONNECT);
  G_OBJECT_CLASS(grotto_page_connect_parent_class)->dispose(object);
}
static void grotto_page_connect_class_init(GrottoPageConnectClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = dispose; GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  gtk_widget_class_set_template_from_resource(wc, APP_RESOURCE_PATH "/ui/page-connect.ui");
  gtk_widget_class_bind_template_child(wc, GrottoPageConnect, paste);
  gtk_widget_class_bind_template_child(wc, GrottoPageConnect, scan);
  gtk_widget_class_bind_template_child(wc, GrottoPageConnect, show_link);
}
static void grotto_page_connect_init(GrottoPageConnect *self) {
  gtk_widget_init_template(GTK_WIDGET(self));
  g_signal_connect(self->paste, "activated", G_CALLBACK(paste), self);
  g_signal_connect(self->scan, "activated", G_CALLBACK(scan), self);
  g_signal_connect(self->show_link, "activated", G_CALLBACK(show_link), self);
}
