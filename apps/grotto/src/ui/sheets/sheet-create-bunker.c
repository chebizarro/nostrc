#include "sheet-create-bunker.h"
#include "sheet-qr-display.h"
#include "../app-resources.h"
#include "../../pairing_uri.h"
#include <string.h>

struct _SheetCreateBunker {
  AdwDialog parent_instance;
  GtkWidget *btn_cancel, *btn_create, *btn_apply, *btn_reset, *entry_name, *entry_relay, *identities, *enabled, *status;
  GDBusConnection *bus;
  GCancellable *cancel;
  gchar **methods;
  guint subscription;
  gboolean closed, loaded, listening;
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
    if (!reply) gtk_label_set_text(GTK_LABEL(s->status), "Update or start the Grotto daemon to configure remote signing.");
    else {
      g_autoptr(GVariant) status = g_variant_get_child_value(reply, 0);
      const gchar *state = "Unavailable"; g_variant_lookup(status, "bunker_state", "&s", &state);
      s->listening = !strcmp(state, "LISTENING");
      g_autofree gchar *message = g_strdup_printf("Bunker: %s. A link is available only after a relay subscription reaches EOSE.", state);
      gtk_label_set_text(GTK_LABEL(s->status), message);
      if (!s->loaded) {
        g_auto(GStrv) relays = NULL; gboolean enabled = FALSE;
        g_variant_lookup(status, "bunker_relays", "^as", &relays);
        g_variant_lookup(status, "bunker_allowed_methods", "^as", &s->methods);
        g_variant_lookup(status, "bunker_enabled", "b", &enabled);
        g_autofree gchar *text = relays ? g_strjoinv(" ", relays) : g_strdup("");
        gtk_editable_set_text(GTK_EDITABLE(s->entry_relay), text); gtk_check_button_set_active(GTK_CHECK_BUTTON(s->enabled), enabled);
        s->loaded = TRUE;
      }
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
static void configured(GObject *source, GAsyncResult *result, gpointer data) {
  SheetCreateBunker *s = data; g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
  if (!s->closed) {
    gtk_widget_set_sensitive(s->btn_apply, TRUE); gtk_widget_set_sensitive(s->btn_reset, TRUE);
    if (!reply) gtk_label_set_text(GTK_LABEL(s->status), "The remote signing change failed. Check daemon status and key availability, then retry.");
    else refresh(s);
  }
  g_object_unref(s);
}
static void apply(GtkButton *button, gpointer data) {
  (void)button; SheetCreateBunker *s = data;
  if (!s->bus || !s->methods) return;
  if (gtk_drop_down_get_selected(GTK_DROP_DOWN(s->identities)) == GTK_INVALID_LIST_POSITION) {
    gtk_label_set_text(GTK_LABEL(s->status), "Choose an identity before enabling relay contact."); return;
  }
  g_auto(GStrv) split = g_strsplit_set(gtk_editable_get_text(GTK_EDITABLE(s->entry_relay)), " \t\n", -1);
  g_autoptr(GPtrArray) urls = g_ptr_array_new();
  for (guint i = 0; split[i]; i++) if (*split[i]) g_ptr_array_add(urls, split[i]);
  gboolean enabled = gtk_check_button_get_active(GTK_CHECK_BUTTON(s->enabled));
  if ((enabled || urls->len) && !grotto_relays_validate((const gchar *const *)urls->pdata, urls->len, NULL)) {
    gtk_label_set_text(GTK_LABEL(s->status), "Use explicit wss:// relays; ws:// is allowed only on loopback. Tor is not supported."); return;
  }
  GVariantBuilder b; g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&b, "{sv}", "enabled", g_variant_new_boolean(enabled));
  g_variant_builder_add(&b, "{sv}", "relays", g_variant_new_strv((const gchar *const *)urls->pdata, urls->len));
  g_variant_builder_add(&b, "{sv}", "allowed-methods", g_variant_new_strv((const gchar *const *)s->methods, -1));
  gtk_widget_set_sensitive(s->btn_apply, FALSE);
  g_dbus_connection_call(s->bus, "org.nostr.Signer", "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1", "ConfigureBunker",
      g_variant_new("(a{sv})", &b), G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 10000, NULL, configured, g_object_ref(s));
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
  } else if (!s->closed) { gtk_label_set_text(GTK_LABEL(s->status), "The daemon could not issue a link. Check listening state, key availability, and the four-offer limit."); update_create(s); }
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
static void reset_chosen(GObject *source, GAsyncResult *result, gpointer data) {
  SheetCreateBunker *s = data;
  const gchar *choice = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
  if (!s->closed && s->bus && !g_strcmp0(choice, "reset")) {
    gtk_widget_set_sensitive(s->btn_reset, FALSE);
    g_dbus_connection_call(s->bus, "org.nostr.Signer", "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1", "ResetBunkerEndpoint",
        NULL, G_VARIANT_TYPE("()"), G_DBUS_CALL_FLAGS_NONE, 30000, NULL, configured, g_object_ref(s));
  }
  g_object_unref(s);
}
static void reset_clicked(GtkButton *button, gpointer data) {
  (void)button; SheetCreateBunker *s = data;
  AdwAlertDialog *dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new("Reset Remote Signing Endpoint?",
      "This explicitly creates a new transport credential in the daemon's key store. All existing remote clients and their permissions are revoked and must be paired again. Identity keys do not change."));
  adw_alert_dialog_add_responses(dialog, "cancel", "Cancel", "reset", "Reset and Revoke Clients", NULL);
  adw_alert_dialog_set_default_response(dialog, "cancel"); adw_alert_dialog_set_close_response(dialog, "cancel");
  adw_alert_dialog_set_response_appearance(dialog, "reset", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_choose(dialog, GTK_WIDGET(s), s->cancel, reset_chosen, g_object_ref(s));
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
  g_clear_object(&s->cancel); g_clear_object(&s->bus); g_clear_pointer(&s->methods, g_strfreev);
  G_OBJECT_CLASS(sheet_create_bunker_parent_class)->dispose(object);
}
static void sheet_create_bunker_class_init(SheetCreateBunkerClass *klass) {
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass); G_OBJECT_CLASS(klass)->dispose = dispose;
  gtk_widget_class_set_template_from_resource(wc, APP_RESOURCE_PATH "/ui/sheets/sheet-create-bunker.ui");
#define CHILD(name) gtk_widget_class_bind_template_child(wc, SheetCreateBunker, name)
  CHILD(btn_cancel); CHILD(btn_create); CHILD(btn_apply); CHILD(btn_reset); CHILD(entry_name); CHILD(entry_relay); CHILD(identities); CHILD(enabled); CHILD(status);
#undef CHILD
}
static void sheet_create_bunker_init(SheetCreateBunker *s) {
  gtk_widget_init_template(GTK_WIDGET(s)); s->cancel = g_cancellable_new();
  g_signal_connect(s->btn_cancel, "clicked", G_CALLBACK(close_clicked), s);
  g_signal_connect(s->btn_reset, "clicked", G_CALLBACK(reset_clicked), s);
  g_signal_connect(s->btn_create, "clicked", G_CALLBACK(create), s); g_signal_connect(s->btn_apply, "clicked", G_CALLBACK(apply), s);
  g_signal_connect(s->identities, "notify::selected", G_CALLBACK(selected), s); g_signal_connect(s, "closed", G_CALLBACK(closed), NULL);
  GApplication *app = g_application_get_default(); GDBusConnection *bus = app ? g_application_get_dbus_connection(app) : NULL;
  if (bus) {
    s->bus = g_object_ref(bus);
    s->subscription = g_dbus_connection_signal_subscribe(bus, "org.nostr.Signer", "org.nostr.Grotto.Control1", "StatusChanged", "/org/nostr/signer/grotto", NULL, G_DBUS_SIGNAL_FLAGS_NONE, status_changed, s, NULL);
    refresh(s);
    g_dbus_connection_call(bus, "org.nostr.Signer", "/org/nostr/signer", "org.nostr.Signer", "ListIdentities", NULL,
        G_VARIANT_TYPE("(as)"), G_DBUS_CALL_FLAGS_NONE, 10000, s->cancel, identities_ready, g_object_ref(s));
  } else gtk_label_set_text(GTK_LABEL(s->status), "The signer session bus is unavailable.");
}
SheetCreateBunker *sheet_create_bunker_new(void) { return g_object_new(TYPE_SHEET_CREATE_BUNKER, NULL); }
