#include "../i18n.h"
#include "page-connect.h"
#include "page-history.h"
#include "app-resources.h"
#include "pairing-flow.h"
#include "sheets/sheet-qr-scanner.h"
#include "sheets/sheet-create-bunker.h"
#include <secure_buf.h>
#include <string.h>
struct _GrottoPageConnect { AdwPreferencesPage parent_instance; AdwActionRow *paste, *scan, *open_link, *create_link, *runtime; GrottoHomeModel *model; GCancellable *file_cancel; };
G_DEFINE_TYPE(GrottoPageConnect, grotto_page_connect, ADW_TYPE_PREFERENCES_PAGE)
static void paste_ready(GObject *source, GAsyncResult *result, gpointer data) {
  GWeakRef *ref = data; g_autoptr(GtkWidget) page = g_weak_ref_get(ref);
  g_autoptr(GError) error = NULL; gchar *text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(source), result, &error);
  if (page && gtk_widget_get_mapped(page)) grotto_pairing_present(page, text && *text ? text : NULL);
  if (text) { secure_wipe(text, strlen(text)); g_free(text); }
  g_weak_ref_clear(ref); g_free(ref);
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
static void weak_free(gpointer data) { GWeakRef *ref = data; g_weak_ref_clear(ref); g_free(ref); }
static void scan(AdwActionRow *row, gpointer data) {
  (void)row; SheetQrScanner *sheet = sheet_qr_scanner_new();
  const GnQrContentType types[] = {GN_QR_TYPE_BUNKER_URI, GN_QR_TYPE_UNKNOWN};
  sheet_qr_scanner_set_accepted_types(sheet, types);
  GWeakRef *ref = g_new0(GWeakRef, 1); g_weak_ref_init(ref, data);
  g_object_set_data_full(G_OBJECT(sheet), "connect-page", ref, weak_free);
  sheet_qr_scanner_set_on_success(sheet, scanned, sheet); adw_dialog_present(ADW_DIALOG(sheet), data);
}
typedef struct { GWeakRef page; GFileInputStream *stream; GCancellable *cancel; gchar buffer[65538]; } LinkFile;
static void link_file_free(LinkFile *file) {
  secure_wipe(file->buffer, sizeof(file->buffer)); g_clear_object(&file->stream); g_clear_object(&file->cancel); g_weak_ref_clear(&file->page); g_free(file);
}
static void link_read(GObject *source, GAsyncResult *result, gpointer data) {
  LinkFile *file = data; g_autoptr(GtkWidget) page = g_weak_ref_get(&file->page);
  g_autoptr(GError) error = NULL; gsize size = 0;
  gboolean ok = g_input_stream_read_all_finish(G_INPUT_STREAM(source), result, &size, &error);
  if (page && gtk_widget_get_mapped(page) && !g_cancellable_is_cancelled(file->cancel)) {
    if (ok && size && size <= 65536 && !memchr(file->buffer, '\0', size) && g_utf8_validate(file->buffer, size, NULL)) {
      file->buffer[size] = '\0'; grotto_pairing_present(page, g_strstrip(file->buffer));
    } else adw_action_row_set_subtitle(GROTTO_PAGE_CONNECT(page)->runtime, _("Could not read a valid link file (maximum 64 KiB). Paste or scan the link instead."));
  }
  link_file_free(file);
}
static void link_stream(GObject *source, GAsyncResult *result, gpointer data) {
  LinkFile *file = data; g_autoptr(GError) error = NULL;
  file->stream = g_file_read_finish(G_FILE(source), result, &error);
  if (!file->stream) {
    g_autoptr(GtkWidget) page = g_weak_ref_get(&file->page);
    if (page && gtk_widget_get_mapped(page) && !g_cancellable_is_cancelled(file->cancel))
      adw_action_row_set_subtitle(GROTTO_PAGE_CONNECT(page)->runtime, _("The selected link file could not be read. Paste or scan the link instead."));
    link_file_free(file); return;
  }
  g_input_stream_read_all_async(G_INPUT_STREAM(file->stream), file->buffer, 65537, G_PRIORITY_DEFAULT, file->cancel, link_read, file);
}
static void link_selected(GObject *source, GAsyncResult *result, gpointer data) {
  LinkFile *file = data; g_autoptr(GError) error = NULL;
  g_autoptr(GFile) chosen = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, &error);
  g_autoptr(GtkWidget) page = g_weak_ref_get(&file->page);
  if (!chosen || !page || !gtk_widget_get_mapped(page)) { link_file_free(file); return; }
  g_file_read_async(chosen, G_PRIORITY_DEFAULT, file->cancel, link_stream, file);
}
static void open_link(AdwActionRow *row, gpointer data) {
  (void)row; GtkRoot *root = gtk_widget_get_root(data); if (!GTK_IS_WINDOW(root)) return;
  GrottoPageConnect *self = data;
  if (self->file_cancel) g_cancellable_cancel(self->file_cancel);
  g_clear_object(&self->file_cancel); self->file_cancel = g_cancellable_new();
  LinkFile *file = g_new0(LinkFile, 1); g_weak_ref_init(&file->page, data); file->cancel = g_object_ref(self->file_cancel);
  g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new(); gtk_file_dialog_set_title(dialog, _("Open a Nostr Connect link file"));
  gtk_file_dialog_open(dialog, GTK_WINDOW(root), file->cancel, link_selected, file);
}
static void create_link(AdwActionRow *row, gpointer data) {
  (void)row; adw_dialog_present(ADW_DIALOG(sheet_create_bunker_new()), data);
}
static void model_changed(GrottoHomeModel *model, gpointer data) {
  GrottoPageConnect *self = data; GVariant *status = grotto_home_model_status(model); const gchar *state = NULL;
  guint pending = 0, acknowledgements = 0;
  if (status) {
    g_variant_lookup(status, "bunker_state", "&s", &state); g_variant_lookup(status, "bunker_pending_pairing_requests", "u", &pending);
    g_variant_lookup(status, "bunker_pending_pairing_acks", "u", &acknowledgements);
  }
  g_autofree gchar *text = g_strdup_printf(_("%s · %u awaiting review · %u awaiting relay acknowledgement"), gn_page_history_remote_state_label(state), pending, acknowledgements);
  adw_action_row_set_subtitle(self->runtime, text);
}
void grotto_page_connect_set_model(GrottoPageConnect *self, GrottoHomeModel *model) {
  g_set_object(&self->model, model); g_signal_connect(model, "changed", G_CALLBACK(model_changed), self); model_changed(model, self);
}
static void dispose(GObject *object) {
  GrottoPageConnect *self = GROTTO_PAGE_CONNECT(object);
  if (self->model) g_signal_handlers_disconnect_by_data(self->model, self);
  g_clear_object(&self->model);
  if (self->file_cancel) g_cancellable_cancel(self->file_cancel);
  g_clear_object(&self->file_cancel);
  gtk_widget_dispose_template(GTK_WIDGET(object), GROTTO_TYPE_PAGE_CONNECT);
  G_OBJECT_CLASS(grotto_page_connect_parent_class)->dispose(object);
}
static void grotto_page_connect_class_init(GrottoPageConnectClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = dispose; GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  gtk_widget_class_set_template_from_resource(wc, APP_RESOURCE_PATH "/ui/page-connect.ui");
  gtk_widget_class_bind_template_child(wc, GrottoPageConnect, runtime);
  gtk_widget_class_bind_template_child(wc, GrottoPageConnect, paste);
  gtk_widget_class_bind_template_child(wc, GrottoPageConnect, scan);
  gtk_widget_class_bind_template_child(wc, GrottoPageConnect, open_link);
  gtk_widget_class_bind_template_child(wc, GrottoPageConnect, create_link);
}
static void unmapped(GtkWidget *widget, gpointer data) {
  (void)data; GrottoPageConnect *self = GROTTO_PAGE_CONNECT(widget);
  if (self->file_cancel) g_cancellable_cancel(self->file_cancel);
}
static void grotto_page_connect_init(GrottoPageConnect *self) {
  gtk_widget_init_template(GTK_WIDGET(self));
  g_signal_connect(self, "unmap", G_CALLBACK(unmapped), NULL);
  g_signal_connect(self->paste, "activated", G_CALLBACK(paste), self);
  g_signal_connect(self->scan, "activated", G_CALLBACK(scan), self);
  g_signal_connect(self->open_link, "activated", G_CALLBACK(open_link), self);
  g_signal_connect(self->create_link, "activated", G_CALLBACK(create_link), self);
}
