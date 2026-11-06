#include "pairing-flow.h"
#include "dialog-actions.h"
#include "../pairing_uri.h"
#include "page-connect.h"
#include <secure_buf.h>
#include <glib/gi18n.h>

typedef struct {
  AdwDialog *dialog;
  GDBusConnection *bus;
  GWeakRef parent;
  GCancellable *cancel;
  GtkWidget *entry, *summary, *status, *identities, *submit;
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
static void identity_selected(GObject *object, GParamSpec *pspec, gpointer data) {
  (void)pspec; PairingFlow *f = data;
  gtk_widget_set_sensitive(f->submit, gtk_drop_down_get_selected(GTK_DROP_DOWN(object)) != GTK_INVALID_LIST_POSITION);
}
static void identities_ready(GObject *source, GAsyncResult *result, gpointer data) {
  AdwDialog *d = data; PairingFlow *f = g_object_get_data(G_OBJECT(d), "pairing-flow");
  g_autoptr(GError) error = NULL; g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
  if (!f->closed) {
    if (!reply) gtk_label_set_text(GTK_LABEL(f->status), _("Identities could not be loaded. Check that Grotto is running and unlocked."));
    else {
      g_auto(GStrv) identities = NULL; g_variant_get(reply, "(^as)", &identities);
      GtkStringList *model = gtk_string_list_new((const gchar *const *)identities);
      gtk_drop_down_set_model(GTK_DROP_DOWN(f->identities), G_LIST_MODEL(model)); g_object_unref(model);
      gtk_drop_down_set_selected(GTK_DROP_DOWN(f->identities), GTK_INVALID_LIST_POSITION);
      gtk_label_set_text(GTK_LABEL(f->status), identities[0] ? _("Choose an identity, then review what the app may do before connecting.") : _("Create or import an identity first."));
      gtk_widget_set_sensitive(f->submit, FALSE);
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
    gtk_label_set_text(GTK_LABEL(f->status), _("Pairing is unavailable. Check Remote Signing, then try again."));
    gtk_widget_set_sensitive(f->submit, TRUE);
    gtk_widget_set_sensitive(f->identities, TRUE);
  }
  g_object_unref(d);
}
static void request_review(GtkButton *button, gpointer data) {
  (void)button; PairingFlow *f = data;
  guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(f->identities));
  if (selected == GTK_INVALID_LIST_POSITION) { gtk_label_set_text(GTK_LABEL(f->status), _("Choose an identity to connect.")); return; }
  GtkStringObject *item = GTK_STRING_OBJECT(gtk_drop_down_get_selected_item(GTK_DROP_DOWN(f->identities)));
  gtk_widget_set_sensitive(f->submit, FALSE);
  gtk_widget_set_sensitive(f->identities, FALSE);
  /* Do not cancel an in-flight mutating call without learning its request ID:
   * the callback cancels that exact pairing if the sheet was closed. */
  g_dbus_connection_call(f->bus, "org.nostr.Signer", "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1", "AcceptConnectURI",
      g_variant_new("(ss)", f->uri, gtk_string_object_get_string(item)), G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 10000, NULL, accepted, g_object_ref(f->dialog));
}
static void preview_uri(PairingFlow *f, const gchar *text) {
  NostrNip46ConnectURI uri; g_autoptr(GError) error = NULL;
  if (!grotto_connect_parse(text, &uri, &error)) {
    gtk_label_set_text(GTK_LABEL(f->status), g_str_has_prefix(text, "bunker://") ?
        _("This link belongs to another signer. Use a connection link from an app instead.") :
        _("This connection link is invalid. Copy a new link from your app.")); return;
  }
  f->uri = g_strdup(text);
  adw_dialog_set_title(f->dialog, _("Connect an App"));
  g_autofree char *summary = g_strdup_printf(_("App name (not verified): %s\nRelays can see your network address and connection timing. No connection has been made yet."), uri.name ? uri.name : _("Not supplied"));
  gtk_label_set_text(GTK_LABEL(f->summary), summary);
  g_autofree gchar *relays = g_strjoinv("\n", uri.relays);
  g_autofree gchar *preview = g_strdup_printf(_("App key: %s\nRequested permissions (not granted): %s\nRelays:\n%s"),
      uri.client_pubkey_hex, uri.perms_csv ? uri.perms_csv : _("None"), relays);
  gtk_label_set_text(GTK_LABEL(f->entry), preview); grotto_connect_clear(&uri);
  if (!f->bus) { gtk_label_set_text(GTK_LABEL(f->status), _("Grotto could not reach the signer.")); return; }
  g_dbus_connection_call(f->bus, "org.nostr.Signer", "/org/nostr/signer", "org.nostr.Signer", "ListIdentities", NULL,
      G_VARIANT_TYPE("(as)"), G_DBUS_CALL_FLAGS_NONE, 10000, f->cancel, identities_ready, g_object_ref(f->dialog));
}
void grotto_pairing_present(GtkWidget *parent, const gchar *uri) {
  g_return_if_fail(GTK_IS_WIDGET(parent));
  PairingFlow *f = g_new0(PairingFlow, 1); f->dialog = adw_dialog_new(); f->cancel = g_cancellable_new();
  g_weak_ref_init(&f->parent, parent);
  GApplication *app = g_application_get_default();
  GDBusConnection *bus = app ? g_application_get_dbus_connection(app) : NULL; if (bus) f->bus = g_object_ref(bus);
  g_object_set_data_full(G_OBJECT(f->dialog), "pairing-flow", f, flow_free);
  g_signal_connect(f->dialog, "closed", G_CALLBACK(flow_closed), f);
  adw_dialog_set_title(f->dialog, _("Add App")); adw_dialog_set_content_width(f->dialog, 520);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(box, 18); gtk_widget_set_margin_end(box, 18); gtk_widget_set_margin_top(box, 18); gtk_widget_set_margin_bottom(box, 18);
  f->status = gtk_label_new(_("Apps on this device ask for permission automatically. Use a link or QR code for an app on another device."));
  gtk_label_set_wrap(GTK_LABEL(f->status), TRUE);
  if (uri) {
    f->entry = gtk_label_new(NULL); gtk_label_set_wrap(GTK_LABEL(f->entry), TRUE); gtk_label_set_selectable(GTK_LABEL(f->entry), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(f->entry), PANGO_WRAP_WORD_CHAR);
    f->summary = gtk_label_new(NULL); gtk_label_set_wrap(GTK_LABEL(f->summary), TRUE); gtk_label_set_xalign(GTK_LABEL(f->summary), 0);
    gtk_box_append(GTK_BOX(box), f->summary);
    GtkWidget *details = gtk_expander_new(_("Details")); gtk_expander_set_child(GTK_EXPANDER(details), f->entry);
    gtk_box_append(GTK_BOX(box), details);
    GtkWidget *identity_label = gtk_label_new(_("Identity")); gtk_label_set_xalign(GTK_LABEL(identity_label), 0);
    gtk_box_append(GTK_BOX(box), identity_label);
    f->identities = gtk_drop_down_new(NULL, NULL); gtk_widget_set_tooltip_text(f->identities, _("Identity to Connect"));
    gtk_accessible_update_property(GTK_ACCESSIBLE(f->identities), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Identity to Connect"), -1);
    gtk_box_append(GTK_BOX(box), f->identities);
    f->submit = gtk_button_new_with_mnemonic(_("_Review")); gtk_widget_set_sensitive(f->submit, FALSE);
    g_signal_connect(f->submit, "clicked", G_CALLBACK(request_review), f);
    g_signal_connect(f->identities, "notify::selected", G_CALLBACK(identity_selected), f);
    preview_uri(f, uri);
  } else {
    GtkWidget *page = g_object_new(GROTTO_TYPE_PAGE_CONNECT, NULL);
    gtk_box_append(GTK_BOX(box), page);
  }
  if (uri) gtk_box_append(GTK_BOX(box), f->status);
  else { g_object_ref_sink(f->status); g_clear_object(&f->status); }
  GtkWidget *scroll = gtk_scrolled_window_new(); gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
  GtkWidget *toolbar = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), grotto_dialog_header(f->dialog, f->submit));
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), scroll);
  adw_dialog_set_child(f->dialog, toolbar);
  if (uri) adw_dialog_set_focus(f->dialog, f->identities);
  adw_dialog_present(f->dialog, parent);
}
