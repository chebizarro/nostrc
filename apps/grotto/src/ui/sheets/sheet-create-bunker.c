#include "sheet-create-bunker.h"
#include "sheet-qr-display.h"
#include "../app-resources.h"
#include <string.h>
#include <glib/gi18n.h>

struct _SheetCreateBunker {
  AdwDialog parent_instance;
  GtkWidget *btn_cancel, *btn_create, *entry_name, *identities, *status;
  GDBusConnection *bus;
  GCancellable *cancel;
  guint subscription;
  gboolean closed, listening;
};
G_DEFINE_TYPE(SheetCreateBunker, sheet_create_bunker, ADW_TYPE_DIALOG)
static void refresh(SheetCreateBunker *self);
static void update_create(SheetCreateBunker *self) {
  gtk_widget_set_sensitive(self->btn_create, !self->closed && self->listening &&
      gtk_drop_down_get_selected(GTK_DROP_DOWN(self->identities)) != GTK_INVALID_LIST_POSITION);
}
static void status_ready(GObject *source, GAsyncResult *result, gpointer data) {
  SheetCreateBunker *s = data; g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
  if (!s->closed) {
    if (!reply) gtk_label_set_text(GTK_LABEL(s->status), _("Open Remote Signing to set up a connection, then try again."));
    else {
      g_autoptr(GVariant) status = g_variant_get_child_value(reply, 0);
      const gchar *state = "Unavailable"; g_variant_lookup(status, "bunker_state", "&s", &state);
      s->listening = !strcmp(state, "LISTENING");
      gtk_label_set_text(GTK_LABEL(s->status), s->listening ? _("Ready to create a single-use link") : _("Remote signing is not ready. Check Remote Signing, then try again."));

    }
    update_create(s);
  }
  g_object_unref(s);
}
static void refresh(SheetCreateBunker *s) {
  if (!s->bus || s->closed) return;
  g_dbus_connection_call(s->bus, "org.nostr.Signer", "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1", "GetStatus", NULL,
      G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 10000, s->cancel, status_ready, g_object_ref(s));
}
static void status_changed(GDBusConnection *bus, const gchar *sender, const gchar *path, const gchar *interface,
    const gchar *signal, GVariant *args, gpointer data) {
  (void)bus; (void)sender; (void)path; (void)interface; (void)signal; (void)args; refresh(data);
}
static void identities_ready(GObject *source, GAsyncResult *result, gpointer data) {
  SheetCreateBunker *s = data; g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
  if (!s->closed && reply) {
    g_auto(GStrv) ids = NULL; g_variant_get(reply, "(^as)", &ids);
    GtkStringList *model = gtk_string_list_new((const gchar *const *)ids);
    gtk_drop_down_set_model(GTK_DROP_DOWN(s->identities), G_LIST_MODEL(model)); g_object_unref(model);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(s->identities), GTK_INVALID_LIST_POSITION);
  }
  g_object_unref(s);
}
static void created(GObject *source, GAsyncResult *result, gpointer data) {
  SheetCreateBunker *s = data; g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
  if (reply) {
    g_autoptr(GVariant) offer = g_variant_get_child_value(reply, 0); const gchar *id = NULL, *uri = NULL; guint64 expires = 0;
    g_variant_lookup(offer, "pairing_id", "&s", &id); g_variant_lookup(offer, "uri", "&s", &uri); g_variant_lookup(offer, "expires_at", "t", &expires);
    if (s->closed || !id || !uri) {
      if (id) g_dbus_connection_call(s->bus, "org.nostr.Signer", "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1", "CancelPairing", g_variant_new("(s)", id), NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL, NULL);
    } else {
      GtkRoot *parent = gtk_widget_get_root(GTK_WIDGET(s));
      SheetQrDisplay *qr = sheet_qr_display_new(); sheet_qr_display_set_bunker_uri(qr, uri);
      sheet_qr_display_set_pairing(qr, s->bus, id, expires);
      adw_dialog_close(ADW_DIALOG(s)); adw_dialog_present(ADW_DIALOG(qr), GTK_WIDGET(parent));
    }
  } else if (!s->closed) { gtk_label_set_text(GTK_LABEL(s->status), _("A link could not be created. Check Remote Signing and close any unused pairing links, then try again.")); update_create(s); }
  g_object_unref(s);
}
static void create(GtkButton *button, gpointer data) {
  (void)button; SheetCreateBunker *s = data;
  GtkStringObject *identity = GTK_STRING_OBJECT(gtk_drop_down_get_selected_item(GTK_DROP_DOWN(s->identities)));
  if (!s->bus || !s->listening || !identity) return;
  gtk_widget_set_sensitive(s->btn_create, FALSE);
  g_dbus_connection_call(s->bus, "org.nostr.Signer", "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1", "CreatePairing",
      g_variant_new("(ss)", gtk_string_object_get_string(identity), gtk_editable_get_text(GTK_EDITABLE(s->entry_name))),
      G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 10000, NULL, created, g_object_ref(s));
}
static void close_clicked(GtkButton *button, gpointer data) { (void)button; adw_dialog_close(ADW_DIALOG(data)); }
static void selected(GObject *object, GParamSpec *pspec, gpointer data) { (void)object; (void)pspec; update_create(data); }
static void closed(AdwDialog *dialog, gpointer data) {
  (void)data; SheetCreateBunker *s = SHEET_CREATE_BUNKER(dialog); if (s->closed) return;
  s->closed = TRUE; if (s->cancel) g_cancellable_cancel(s->cancel);
  if (s->subscription) { g_dbus_connection_signal_unsubscribe(s->bus, s->subscription); s->subscription = 0; }
}
static void dispose(GObject *object) {
  SheetCreateBunker *s = SHEET_CREATE_BUNKER(object); closed(ADW_DIALOG(s), NULL);
  g_clear_object(&s->cancel); g_clear_object(&s->bus);
  G_OBJECT_CLASS(sheet_create_bunker_parent_class)->dispose(object);
}
static void sheet_create_bunker_class_init(SheetCreateBunkerClass *klass) {
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass); G_OBJECT_CLASS(klass)->dispose = dispose;
  gtk_widget_class_set_template_from_resource(wc, APP_RESOURCE_PATH "/ui/sheets/sheet-create-bunker.ui");
#define CHILD(name) gtk_widget_class_bind_template_child(wc, SheetCreateBunker, name)
  CHILD(btn_cancel); CHILD(btn_create); CHILD(entry_name); CHILD(identities); CHILD(status);
#undef CHILD
}
static void sheet_create_bunker_init(SheetCreateBunker *s) {
  gtk_widget_init_template(GTK_WIDGET(s)); s->cancel = g_cancellable_new();
  g_signal_connect(s->btn_cancel, "clicked", G_CALLBACK(close_clicked), s);
  g_signal_connect(s->btn_create, "clicked", G_CALLBACK(create), s);
  g_signal_connect(s->identities, "notify::selected", G_CALLBACK(selected), s); g_signal_connect(s, "closed", G_CALLBACK(closed), NULL);
  GApplication *app = g_application_get_default(); GDBusConnection *bus = app ? g_application_get_dbus_connection(app) : NULL;
  if (bus) {
    s->bus = g_object_ref(bus);
    s->subscription = g_dbus_connection_signal_subscribe(bus, "org.nostr.Signer", "org.nostr.Grotto.Control1", "StatusChanged", "/org/nostr/signer/grotto", NULL, G_DBUS_SIGNAL_FLAGS_NONE, status_changed, s, NULL);
    refresh(s);
    g_dbus_connection_call(bus, "org.nostr.Signer", "/org/nostr/signer", "org.nostr.Signer", "ListIdentities", NULL,
        G_VARIANT_TYPE("(as)"), G_DBUS_CALL_FLAGS_NONE, 10000, s->cancel, identities_ready, g_object_ref(s));
  } else gtk_label_set_text(GTK_LABEL(s->status), _("Grotto could not reach the signer."));
}
SheetCreateBunker *sheet_create_bunker_new(void) { return g_object_new(TYPE_SHEET_CREATE_BUNKER, NULL); }
