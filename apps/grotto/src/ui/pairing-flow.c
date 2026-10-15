#include "pairing-flow.h"
#include "../pairing_uri.h"
#include "sheets/sheet-create-bunker.h"
#include "sheets/sheet-qr-scanner.h"
#include <secure_buf.h>

typedef struct {
  AdwDialog *dialog;
  GDBusConnection *bus;
  GWeakRef parent;
  GCancellable *cancel;
  GtkWidget *entry, *status, *identities, *submit;
  gchar *uri;
  gboolean closed;
} PairingFlow;
static void flow_free(gpointer data) {
  PairingFlow *f = data; if (f->uri) { secure_wipe(f->uri, strlen(f->uri)); g_free(f->uri); }
  g_clear_object(&f->bus); g_clear_object(&f->cancel); g_weak_ref_clear(&f->parent); g_free(f);
}
static void flow_closed(AdwDialog *dialog, gpointer data) {
  (void)dialog; PairingFlow *f = data; f->closed = TRUE; g_cancellable_cancel(f->cancel);
  if (GTK_IS_EDITABLE(f->entry)) gtk_editable_set_text(GTK_EDITABLE(f->entry), "");
  if (f->uri) { secure_wipe(f->uri, strlen(f->uri)); g_clear_pointer(&f->uri, g_free); }
}
static void identities_ready(GObject *source, GAsyncResult *result, gpointer data) {
  AdwDialog *d = data; PairingFlow *f = g_object_get_data(G_OBJECT(d), "pairing-flow");
  g_autoptr(GError) error = NULL; g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
  if (!f->closed) {
    if (!reply) gtk_label_set_text(GTK_LABEL(f->status), "The signer could not list its identities. Check Grotto is running and unlocked.");
    else {
      g_auto(GStrv) identities = NULL; g_variant_get(reply, "(^as)", &identities);
      GtkStringList *model = gtk_string_list_new((const gchar *const *)identities);
      gtk_drop_down_set_model(GTK_DROP_DOWN(f->identities), G_LIST_MODEL(model)); g_object_unref(model);
      gtk_drop_down_set_selected(GTK_DROP_DOWN(f->identities), GTK_INVALID_LIST_POSITION);
      gtk_label_set_text(GTK_LABEL(f->status), identities[0] ? "Choose an identity. The next review grants pairing only, not signing permissions." : "Create or import a signable identity first.");
      gtk_widget_set_sensitive(f->submit, identities[0] != NULL);
    }
  }
  g_object_unref(d);
}
static void accepted(GObject *source, GAsyncResult *result, gpointer data) {
  AdwDialog *d = data; PairingFlow *f = g_object_get_data(G_OBJECT(d), "pairing-flow");
  g_autoptr(GError) error = NULL; g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
  if (reply) {
    const gchar *id; g_variant_get(reply, "(&s)", &id);
    if (f->closed) g_dbus_connection_call(f->bus, "org.nostr.Signer", "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1", "CancelPairing", g_variant_new("(s)", id), NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL, NULL);
    else {
      GApplication *app = g_application_get_default();
      if (app && g_action_group_has_action(G_ACTION_GROUP(app), "review-request"))
        g_action_group_activate_action(G_ACTION_GROUP(app), "review-request", g_variant_new_string(id));
      adw_dialog_close(d);
    }
  } else if (!f->closed) {
    gtk_label_set_text(GTK_LABEL(f->status), "Pairing is unavailable. Configure remote signing and wait for a listening relay, then retry.");
    gtk_widget_set_sensitive(f->submit, TRUE);
  }
  g_object_unref(d);
}
static void request_review(GtkButton *button, gpointer data) {
  (void)button; PairingFlow *f = data;
  guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(f->identities));
  if (selected == GTK_INVALID_LIST_POSITION) { gtk_label_set_text(GTK_LABEL(f->status), "Choose the identity this application may request."); return; }
  GtkStringObject *item = GTK_STRING_OBJECT(gtk_drop_down_get_selected_item(GTK_DROP_DOWN(f->identities)));
  gtk_widget_set_sensitive(f->submit, FALSE);
  /* Do not cancel an in-flight mutating call without learning its request ID:
   * the callback cancels that exact pairing if the sheet was closed. */
  g_dbus_connection_call(f->bus, "org.nostr.Signer", "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1", "AcceptConnectURI",
      g_variant_new("(ss)", f->uri, gtk_string_object_get_string(item)), G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 10000, NULL, accepted, g_object_ref(f->dialog));
}
static void preview_uri(PairingFlow *f, const gchar *text) {
  NostrNip46ConnectURI uri; g_autoptr(GError) error = NULL;
  if (!grotto_connect_parse(text, &uri, &error)) {
    gtk_label_set_text(GTK_LABEL(f->status), g_str_has_prefix(text, "bunker://") ?
        "This is another signer's bunker link. Grotto pairs applications using nostrconnect://; it does not delegate custody to another signer." :
        "This is not a valid Nostr Connect link. Use nostrconnect:// with a client key, secret, and supported relays."); return;
  }
  f->uri = g_strdup(text);
  adw_dialog_set_title(f->dialog, "Connect with Which Identity?");
  g_autofree gchar *relays = g_strjoinv("\n", uri.relays);
  g_autofree gchar *preview = g_strdup_printf("Client transport key: %s\nName supplied by the client (unverified): %s\nRequested permissions (hints only): %s\nRelays:\n%s\n\nThese relays see your network address and connection timing. Nothing has been contacted for this link.",
      uri.client_pubkey_hex, uri.name ? uri.name : "Not supplied", uri.perms_csv ? uri.perms_csv : "None", relays);
  gtk_label_set_text(GTK_LABEL(f->entry), preview); grotto_connect_clear(&uri);
  if (!f->bus) { gtk_label_set_text(GTK_LABEL(f->status), "The signer session bus is unavailable."); return; }
  g_dbus_connection_call(f->bus, "org.nostr.Signer", "/org/nostr/signer", "org.nostr.Signer", "ListIdentities", NULL,
      G_VARIANT_TYPE("(as)"), G_DBUS_CALL_FLAGS_NONE, 10000, f->cancel, identities_ready, g_object_ref(f->dialog));
}
static void pasted(GtkButton *button, gpointer data) {
  (void)button; PairingFlow *f = data; g_autoptr(GtkWidget) parent = g_weak_ref_get(&f->parent);
  const gchar *text = gtk_editable_get_text(GTK_EDITABLE(f->entry));
  if (parent && text && *text) { grotto_pairing_present(parent, text); adw_dialog_close(f->dialog); }
}
static void scanned(const gchar *text, GnQrContentType type, gpointer data) {
  (void)type; SheetQrScanner *scanner = data;
  GtkWidget *parent = g_object_get_data(G_OBJECT(scanner), "pairing-parent");
  if (parent) grotto_pairing_present(parent, text);
}
static void scan(GtkButton *button, gpointer data) {
  (void)button; PairingFlow *f = data; g_autoptr(GtkWidget) parent = g_weak_ref_get(&f->parent);
  if (!parent) return;
  SheetQrScanner *scanner = sheet_qr_scanner_new();
  const GnQrContentType types[] = {GN_QR_TYPE_BUNKER_URI, GN_QR_TYPE_UNKNOWN};
  sheet_qr_scanner_set_accepted_types(scanner, types);
  sheet_qr_scanner_set_on_success(scanner, scanned, scanner);
  g_object_set_data_full(G_OBJECT(scanner), "pairing-parent", g_object_ref(parent), g_object_unref);
  adw_dialog_present(ADW_DIALOG(scanner), parent); adw_dialog_close(f->dialog);
}
static void create(GtkButton *button, gpointer data) {
  (void)button; PairingFlow *f = data; g_autoptr(GtkWidget) parent = g_weak_ref_get(&f->parent);
  if (parent) { adw_dialog_present(ADW_DIALOG(sheet_create_bunker_new()), parent); adw_dialog_close(f->dialog); }
}
void grotto_pairing_present(GtkWidget *parent, const gchar *uri) {
  g_return_if_fail(GTK_IS_WIDGET(parent));
  PairingFlow *f = g_new0(PairingFlow, 1); f->dialog = adw_dialog_new(); f->cancel = g_cancellable_new();
  g_weak_ref_init(&f->parent, parent);
  GApplication *app = g_application_get_default();
  GDBusConnection *bus = app ? g_application_get_dbus_connection(app) : NULL; if (bus) f->bus = g_object_ref(bus);
  g_object_set_data_full(G_OBJECT(f->dialog), "pairing-flow", f, flow_free);
  g_signal_connect(f->dialog, "closed", G_CALLBACK(flow_closed), f);
  adw_dialog_set_title(f->dialog, "Connect an Application"); adw_dialog_set_content_width(f->dialog, 520);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(box, 18); gtk_widget_set_margin_end(box, 18); gtk_widget_set_margin_top(box, 18); gtk_widget_set_margin_bottom(box, 18);
  f->status = gtk_label_new("Local GNOME applications do not need pairing. They ask for permission the first time they call Grotto.");
  gtk_label_set_wrap(GTK_LABEL(f->status), TRUE);
  if (uri) {
    f->entry = gtk_label_new(NULL); gtk_label_set_wrap(GTK_LABEL(f->entry), TRUE); gtk_label_set_selectable(GTK_LABEL(f->entry), TRUE);
    gtk_box_append(GTK_BOX(box), f->entry);
    f->identities = gtk_drop_down_new(NULL, NULL); gtk_widget_set_tooltip_text(f->identities, "Connect with which identity?"); gtk_box_append(GTK_BOX(box), f->identities);
    f->submit = gtk_button_new_with_label("Review Connection"); gtk_widget_set_sensitive(f->submit, FALSE);
    g_signal_connect(f->submit, "clicked", G_CALLBACK(request_review), f); gtk_box_append(GTK_BOX(box), f->submit);
    preview_uri(f, uri);
  } else {
    f->entry = gtk_entry_new(); gtk_entry_set_placeholder_text(GTK_ENTRY(f->entry), "Paste a nostrconnect:// link"); gtk_box_append(GTK_BOX(box), f->entry);
    const gchar *labels[] = {"Preview Pasted Link", "Scan a QR Code…", "Create a Pairing Link / Relay Preferences…"};
    GCallback callbacks[] = {G_CALLBACK(pasted), G_CALLBACK(scan), G_CALLBACK(create)};
    for (guint i = 0; i < 3; i++) { GtkWidget *b = gtk_button_new_with_label(labels[i]); g_signal_connect(b, "clicked", callbacks[i], f); gtk_box_append(GTK_BOX(box), b); }
  }
  gtk_box_append(GTK_BOX(box), f->status); adw_dialog_set_child(f->dialog, box); adw_dialog_present(f->dialog, parent);
}
