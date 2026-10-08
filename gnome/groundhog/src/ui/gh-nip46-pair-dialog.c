#include "gh-nip46-pair-dialog.h"
#include "gh-nip46-auth-url.h"
#include "gh-net-session.h"
#include "nostr/nip19/nip19.h"
#include "nostr/nip46/nip46_uri.h"

#include <glib/gi18n.h>
#include <qrencode.h>
#include <string.h>
#include <stdlib.h>

static const gchar * const amber_relays[] = {
  "wss://auth.nostr1.com", "wss://bucket.coracle.social",
  "wss://nrs.primal.net", "wss://relay.nip46.com", NULL
};

typedef enum { PAIR_EDITING, PAIR_WAITING, PAIR_CONFIRMING, PAIR_SAVING,
               PAIR_SELECTING, PAIR_COMPLETE } PairState;

struct _GhNip46PairDialog {
  AdwDialog parent_instance;
  GtkStackSwitcher *mode_switcher;
  GtkStack *mode_stack;
  GtkLabel *qr_status;
  GtkPicture *qr_picture;
  AdwEntryRow *uri_row;
  AdwEntryRow *relay_row;
  GtkButton *copy_button;
  GtkButton *regenerate_button;
  AdwEntryRow *bunker_row;
  GtkButton *paste_button;
  GtkLabel *bunker_details;
  GtkButton *connect_button;
  GtkLabel *error_label;
  GtkButton *cancel_button;

  GhAccountController *accounts;
  GSettings *settings;
  GhNip46CredentialStore *credentials;
  GhNip46PairNameFunc display_name;
  GObject *name_source;
  GhRelayTransport scope_transport;
  GhRelayAuthTransport scope_auth;
  GhRelayPublishTransport publish_transport;
  GhRelayPublishAuthTransport publish_auth;
  gpointer transport_data;
  GhNip46PairTestSaveFunc test_save;
  gpointer test_save_data;
  gboolean custom_scope, custom_scope_auth, custom_publish, custom_publish_auth;

  GhNip46Session *session;
  GCancellable *attempt_cancel;
  gchar *uri;
  gchar **relays;
  gchar *user_pubkey;
  gchar *pending_npub;
  GhNip46AuthUrl *auth_prompt;
  PairState state;
  gboolean closing;
  guint select_timeout;
};

enum { SIGNAL_CONFIRMATION_PRESENTED, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(GhNip46PairDialog, gh_nip46_pair_dialog, ADW_TYPE_DIALOG)

static void
wipe_free(gchar **value)
{
  if (!*value) return;
  volatile gchar *p = (volatile gchar *)*value;
  for (gsize i = 0, n = strlen(*value); i < n; i++) p[i] = 0;
  g_clear_pointer(value, g_free);
}

static void
clear_sensitive_fields(GhNip46PairDialog *self)
{
  gtk_editable_set_text(GTK_EDITABLE(self->uri_row), "");
  gtk_editable_set_text(GTK_EDITABLE(self->bunker_row), "");
  wipe_free(&self->uri);
  if (self->auth_prompt) gh_nip46_auth_url_clear(self->auth_prompt);
}

static void
stop_attempt(GhNip46PairDialog *self)
{
  if (self->attempt_cancel) g_cancellable_cancel(self->attempt_cancel);
  if (self->session) gh_nip46_session_cancel(self->session);
  g_clear_object(&self->session);
  g_clear_object(&self->attempt_cancel);
  g_clear_pointer(&self->relays, g_strfreev);
  g_clear_pointer(&self->user_pubkey, g_free);
  clear_sensitive_fields(self);
  gtk_picture_set_paintable(self->qr_picture, NULL);
  gtk_widget_set_visible(GTK_WIDGET(self->qr_picture), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(self->copy_button), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(self->connect_button), FALSE);
}

static void
show_error(GhNip46PairDialog *self, const gchar *message)
{
  gtk_label_set_text(self->error_label, message ? message : "");
  gtk_widget_set_visible(GTK_WIDGET(self->error_label), message != NULL);
}

static gchar *
invalid_relay(GError **error)
{
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                      _("Enter a valid pairing relay address."));
  return NULL;
}

static gchar *
normalize_relay(const gchar *input, GhNetMode mode, GError **error)
{
  g_autofree gchar *text = g_strstrip(g_strdup(input));
  if (!*text || strlen(text) > 2048) return invalid_relay(error);
  g_autoptr(GUri) uri = g_uri_parse(text, G_URI_FLAGS_NONE, NULL);
  if (!uri || !g_uri_get_host(uri) || !*g_uri_get_host(uri) ||
      !g_uri_get_scheme(uri) || g_uri_get_userinfo(uri) ||
      g_uri_get_query(uri) || g_uri_get_fragment(uri)) return invalid_relay(error);
  g_autofree gchar *scheme = g_ascii_strdown(g_uri_get_scheme(uri), -1);
  g_autofree gchar *host = g_ascii_strdown(g_uri_get_host(uri), -1);
  if (!g_str_equal(scheme, "wss") && !g_str_equal(scheme, "ws")) return invalid_relay(error);
  gint port = g_uri_get_port(uri);
  if ((g_str_equal(scheme, "wss") && port == 443) ||
      (g_str_equal(scheme, "ws") && port == 80)) port = -1;
  const gchar *path = g_uri_get_path(uri);
  if (!path || g_str_equal(path, "/")) path = "";
  g_autoptr(GUri) canonical = g_uri_build(G_URI_FLAGS_NONE, scheme, NULL, host,
                                           port, path, NULL, NULL);
  g_autofree gchar *url = g_uri_to_string(canonical);
  if (!gh_net_relay_url_allowed(mode, url, error)) return NULL;
  return g_steal_pointer(&url);
}

static GhNetMode
network_mode(GhNip46PairDialog *self)
{
  g_autofree gchar *value = g_settings_get_string(self->settings, "network-mode");
  return gh_net_mode_from_string(value);
}

static gchar **
validate_relays(GhNip46PairDialog *self, const gchar * const *input, GError **error)
{
  g_autoptr(GPtrArray) urls = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; input && input[i]; i++) {
    g_autofree gchar *normalized = normalize_relay(input[i], network_mode(self), error);
    if (!normalized) return NULL;
    if (!g_ptr_array_find_with_equal_func(urls, normalized,
                                           (GEqualFunc)g_str_equal, NULL))
      g_ptr_array_add(urls, g_steal_pointer(&normalized));
    if (urls->len > 4) break;
  }
  if (urls->len < 1 || urls->len > 4) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        _("Choose one to four pairing relays."));
    return NULL;
  }
  g_ptr_array_add(urls, NULL);
  return (gchar **)g_ptr_array_free(g_steal_pointer(&urls), FALSE);
}

static gchar **
qr_relays(GhNip46PairDialog *self, GError **error)
{
  const gchar *text = gtk_editable_get_text(GTK_EDITABLE(self->relay_row));
  g_auto(GStrv) parts = g_strsplit_set(text, ",; \n", -1);
  g_autoptr(GPtrArray) nonempty = g_ptr_array_new();
  for (guint i = 0; parts[i]; i++)
    if (*parts[i]) g_ptr_array_add(nonempty, parts[i]);
  g_ptr_array_add(nonempty, NULL);
  return validate_relays(self, (const gchar *const *)nonempty->pdata, error);
}

static GdkTexture *
qr_texture(const gchar *uri)
{
  QRcode *qr = QRcode_encodeString(uri, 0, QR_ECLEVEL_M, QR_MODE_8, 1);
  if (!qr) return NULL;
  const int border = 4, scale = 4;
  const int size = (qr->width + border * 2) * scale;
  guchar *pixels = g_malloc((gsize)size * size * 3);
  memset(pixels, 255, (gsize)size * size * 3);
  for (int y = 0; y < qr->width; y++)
    for (int x = 0; x < qr->width; x++)
      if (qr->data[y * qr->width + x] & 1)
        for (int sy = 0; sy < scale; sy++)
          for (int sx = 0; sx < scale; sx++) {
            gsize offset = ((gsize)(y + border) * scale + sy) * size * 3 +
                           ((gsize)(x + border) * scale + sx) * 3;
            memset(pixels + offset, 0, 3);
          }
  QRcode_free(qr);
  g_autoptr(GBytes) bytes = g_bytes_new_take(pixels, (gsize)size * size * 3);
  return gdk_memory_texture_new(size, size, GDK_MEMORY_R8G8B8, bytes, size * 3);
}

static void
on_auth_launch_failed(GhNip46AuthUrl *prompt, GError *error, gpointer data)
{
  (void)prompt; (void)error;
  GhNip46PairDialog *self = data;
  if (!self->closing && self->session) {
    stop_attempt(self);
    self->state = PAIR_EDITING;
    show_error(self, _("Could not open the signer's authorization page. Try pairing again."));
  }
}

static gboolean
on_auth_url(GhNip46Session *session, const gchar *url, gpointer data)
{
  GhNip46PairDialog *self = data;
  if (self->closing || self->session != session) return FALSE;
  return self->auth_prompt &&
    gh_nip46_auth_url_handle(self->auth_prompt, url, self->attempt_cancel);
}

static void
on_ready(GhNip46Session *session, gpointer data)
{
  GhNip46PairDialog *self = data;
  if (self->closing || self->session != session || self->state != PAIR_WAITING ||
      !self->uri) return;
  g_autoptr(GdkTexture) texture = qr_texture(self->uri);
  if (!texture) {
    stop_attempt(self);
    self->state = PAIR_EDITING;
    show_error(self, _("Could not draw the pairing code. Try again."));
    return;
  }
  gtk_picture_set_paintable(self->qr_picture, GDK_PAINTABLE(texture));
  gtk_widget_set_visible(GTK_WIDGET(self->qr_picture), TRUE);
  gtk_editable_set_text(GTK_EDITABLE(self->uri_row), self->uri);
  gtk_widget_set_sensitive(GTK_WIDGET(self->copy_button), TRUE);
  gtk_label_set_text(self->qr_status, _("Waiting for your signer…"));
}

static void
on_offline(GhNip46Session *session, gpointer data)
{
  GhNip46PairDialog *self = data;
  if (self->closing || self->session != session || self->state != PAIR_WAITING) return;
  gtk_picture_set_paintable(self->qr_picture, NULL);
  gtk_widget_set_visible(GTK_WIDGET(self->qr_picture), FALSE);
  gtk_editable_set_text(GTK_EDITABLE(self->uri_row), "");
  gtk_widget_set_sensitive(GTK_WIDGET(self->copy_button), FALSE);
  gtk_label_set_text(self->qr_status, _("Reconnecting to pairing relays…"));
}

static gchar *
npub_for_hex(const gchar *hex)
{
  if (!hex || strlen(hex) != 64) return NULL;
  guint8 bytes[32];
  for (guint i = 0; i < 32; i++) {
    gchar pair[3] = { hex[i * 2], hex[i * 2 + 1], 0 };
    bytes[i] = (guint8)strtoul(pair, NULL, 16);
  }
  char *npub = NULL;
  if (nostr_nip19_encode_npub(bytes, &npub) != 0) return NULL;
  return npub;
}

static gboolean
selection_timed_out(gpointer data)
{
  GhNip46PairDialog *self = data;
  self->select_timeout = 0;
  if (!self->closing && self->state == PAIR_SELECTING) {
    self->state = PAIR_COMPLETE;
    show_error(self, _("The signer was saved, but its account could not be selected. Refresh accounts in the switcher to try again."));
    gtk_button_set_label(self->cancel_button, _("Close"));
  }
  return G_SOURCE_REMOVE;
}

static void
selection_changed(GhAccountController *accounts, gpointer data)
{
  GhNip46PairDialog *self = data;
  if (self->closing || self->state != PAIR_SELECTING || self->accounts != accounts ||
      !self->pending_npub) return;
  g_autoptr(GError) error = NULL;
  if (gh_account_controller_select_backend(accounts, GH_SIGNER_BACKEND_NIP46,
                                           self->pending_npub, &error)) {
    g_clear_handle_id(&self->select_timeout, g_source_remove);
    self->state = PAIR_COMPLETE;
    if (gtk_widget_get_root(GTK_WIDGET(self)))
      adw_dialog_close(ADW_DIALOG(self));
  }
}

static void
stored(GObject *source, GAsyncResult *result, gpointer data)
{
  GhNip46PairDialog *self = data;
  g_autoptr(GError) error = NULL;
  gboolean saved = self->test_save ? g_task_propagate_boolean(G_TASK(result), &error) :
    gh_nip46_credential_store_store_finish(GH_NIP46_CREDENTIAL_STORE(source),
                                             result, &error);
  adw_dialog_set_can_close(ADW_DIALOG(self), TRUE);
  gtk_widget_set_visible(GTK_WIDGET(self->cancel_button), TRUE);
  if (self->closing) { g_object_unref(self); return; }
  stop_attempt(self);
  if (!saved) {
    self->state = PAIR_EDITING;
    show_error(self, error ? error->message : _("Could not save the remote signer."));
    gtk_widget_set_sensitive(GTK_WIDGET(self->mode_switcher), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(self->regenerate_button), TRUE);
  } else {
    self->state = PAIR_SELECTING;
    gtk_label_set_text(self->qr_status, _("Saved. Adding your account…"));
    gh_account_controller_refresh(self->accounts);
    self->select_timeout = g_timeout_add_seconds(15, selection_timed_out, self);
  }
  g_object_unref(self);
}

static void
confirm_closed(AdwDialog *alert, gpointer data)
{
  (void)alert;
  GhNip46PairDialog *self = data;
  if (self->closing || self->state != PAIR_CONFIRMING) return;
  stop_attempt(self);
  self->state = PAIR_EDITING;
  gtk_widget_set_sensitive(GTK_WIDGET(self->mode_switcher), TRUE);
  gtk_label_set_text(self->qr_status, _("Pairing cancelled. Regenerate to try again."));
}

static void
confirm_response(AdwAlertDialog *alert, const gchar *response, gpointer data)
{
  GhNip46PairDialog *self = data;
  if (self->closing || self->state != PAIR_CONFIRMING) return;
  if (!g_str_equal(response, "save")) {
    confirm_closed(ADW_DIALOG(alert), self);
    return;
  }
  g_autofree gchar *secret = gh_nip46_session_dup_client_secret(self->session);
  g_autoptr(GError) error = NULL;
  g_autoptr(GhNip46Credential) credential = gh_nip46_credential_new(
    self->user_pubkey, gh_nip46_session_get_remote_pubkey(self->session),
    secret, (const gchar *const *)self->relays, &error);
  wipe_free(&secret);
  if (!credential) {
    stop_attempt(self);
    self->state = PAIR_EDITING;
    show_error(self, error ? error->message : _("Could not prepare the remote signer."));
    return;
  }
  self->state = PAIR_SAVING;
  gtk_widget_set_sensitive(GTK_WIDGET(self->mode_switcher), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(self->connect_button), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(self->regenerate_button), FALSE);
  adw_dialog_set_can_close(ADW_DIALOG(self), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(self->cancel_button), FALSE);
  show_error(self, NULL);
  if (self->test_save)
    self->test_save(credential, stored, g_object_ref(self), self->test_save_data);
  else
    gh_nip46_credential_store_store_async(self->credentials, credential, TRUE, NULL,
                                          stored, g_object_ref(self));
}

static void
pair_finished(GObject *source, GAsyncResult *result, gpointer data)
{
  GhNip46PairDialog *self = data;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *user = gh_nip46_session_pair_finish(GH_NIP46_SESSION(source),
                                                        result, &error);
  if (self->closing || self->session != GH_NIP46_SESSION(source)) {
    g_object_unref(self);
    return;
  }
  if (!user) {
    stop_attempt(self);
    self->state = PAIR_EDITING;
    gtk_widget_set_sensitive(GTK_WIDGET(self->mode_switcher), TRUE);
    show_error(self, error ? error->message : _("Could not pair with your signer."));
    gtk_label_set_text(self->qr_status, _("Pairing failed. Regenerate to try again."));
    g_object_unref(self);
    return;
  }
  self->state = PAIR_CONFIRMING;
  self->user_pubkey = g_steal_pointer(&user);
  clear_sensitive_fields(self);
  gtk_picture_set_paintable(self->qr_picture, NULL);
  gtk_widget_set_visible(GTK_WIDGET(self->qr_picture), FALSE);
  g_clear_pointer(&self->pending_npub, g_free);
  self->pending_npub = npub_for_hex(self->user_pubkey);
  g_autofree gchar *short_npub = self->pending_npub ?
    g_strdup_printf("%.16s…", self->pending_npub) : g_strdup(self->user_pubkey);
  const gchar *name = self->display_name && self->name_source ?
    self->display_name(self->name_source, self->user_pubkey) : NULL;
  gboolean replacing = FALSE;
  GPtrArray *identities = gh_account_controller_get_identities(self->accounts);
  for (guint i = 0; identities && i < identities->len; i++) {
    const GhIdentityInfo *info = g_ptr_array_index(identities, i);
    if (info->backend == GH_SIGNER_BACKEND_NIP46 &&
        g_strcmp0(info->npub, self->pending_npub) == 0) {
      replacing = TRUE;
      break;
    }
  }
  g_autofree gchar *heading = g_strdup_printf(replacing ?
    _("Replace signer for %s?") : _("Connect %s?"),
    name && *name ? name : short_npub);
  g_autofree gchar *joined = g_strjoinv(", ", self->relays);
  g_autofree gchar *body = g_strdup_printf(replacing ?
    _("Confirm this is the account shown by your new signer. Saving replaces the previous remote signer connection, but keeps encrypted messages. Account: %s. Pairing relays: %s.") :
    _("Confirm this is the account shown by your signer. Account: %s. Pairing relays: %s. The signer may ask you to approve actions later."),
    self->pending_npub ? self->pending_npub : self->user_pubkey, joined);
  AdwDialog *alert = adw_alert_dialog_new(heading, body);
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(alert), "cancel", _("Cancel"),
                                 "save", _("Save Remote Signer"), NULL);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(alert), "cancel");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(alert), "cancel");
  g_signal_connect_object(alert, "response", G_CALLBACK(confirm_response), self, 0);
  /* Older libadwaita can unmap a dismissed alert without either signal. */
  g_signal_connect_object(alert, "closed", G_CALLBACK(confirm_closed), self, 0);
  g_signal_connect_object(alert, "unmap", G_CALLBACK(confirm_closed), self, 0);
  adw_dialog_present(alert, GTK_WIDGET(self));
  g_signal_emit(self, signals[SIGNAL_CONFIRMATION_PRESENTED], 0, alert);
  g_object_unref(self);
}

static void
start_pair(GhNip46PairDialog *self, GhNip46Session *session, gchar **relays,
           gchar *uri)
{
  stop_attempt(self);
  self->session = session;
  self->relays = relays;
  self->uri = uri;
  self->attempt_cancel = g_cancellable_new();
  self->state = PAIR_WAITING;
  show_error(self, NULL);
  gtk_widget_set_sensitive(GTK_WIDGET(self->mode_switcher), TRUE);
  if (uri) {
    gtk_editable_set_text(GTK_EDITABLE(self->uri_row), "");
    gtk_widget_set_sensitive(GTK_WIDGET(self->copy_button), FALSE);
    gtk_label_set_text(self->qr_status, _("Connecting to pairing relays…"));
    g_signal_connect(session, "ready", G_CALLBACK(on_ready), self);
    g_signal_connect(session, "offline", G_CALLBACK(on_offline), self);
  } else {
    gtk_widget_set_sensitive(GTK_WIDGET(self->connect_button), FALSE);
  }
  gh_nip46_session_set_auth_url_handler(session, on_auth_url, self);
  gh_nip46_session_pair_async(session, self->attempt_cancel, pair_finished, g_object_ref(self));
  if (uri && gh_nip46_session_is_ready(session)) on_ready(session, self);
}

static void
start_qr(GhNip46PairDialog *self)
{
  g_autoptr(GError) error = NULL;
  g_auto(GStrv) relays = qr_relays(self, &error);
  if (!relays) { show_error(self, error->message); return; }
  gchar *uri = NULL;
  GhNip46Session *session = gh_nip46_session_new_qr((const gchar *const *)relays,
    self->custom_scope ? &self->scope_transport : NULL,
    self->custom_scope_auth ? &self->scope_auth : NULL,
    self->custom_publish ? &self->publish_transport : NULL,
    self->custom_publish_auth ? &self->publish_auth : NULL,
    self->transport_data, &uri, &error);
  if (!session) { show_error(self, error ? error->message : _("Could not start pairing.")); return; }
  start_pair(self, session, g_steal_pointer(&relays), uri);
}

static void
on_bunker_changed(GtkEditable *entry, gpointer data)
{
  GhNip46PairDialog *self = data;
  if (self->closing || self->state != PAIR_EDITING) return;
  const gchar *text = gtk_editable_get_text(entry);
  gtk_widget_set_sensitive(GTK_WIDGET(self->connect_button), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(self->bunker_details), FALSE);
  if (!*text) { show_error(self, NULL); return; }
  if (g_str_has_prefix(text, "nostrconnect://")) {
    show_error(self, _("That link is for your signer to scan — use the QR tab."));
    return;
  }
  NostrNip46BunkerURI parsed = {0};
  if (nostr_nip46_uri_parse_bunker(text, &parsed) != 0 || !parsed.n_relays) {
    nostr_nip46_uri_bunker_free(&parsed);
    show_error(self, _("Paste a complete bunker:// link with at least one relay."));
    return;
  }
  g_autoptr(GError) error = NULL;
  g_auto(GStrv) relays = validate_relays(self, (const gchar *const *)parsed.relays, &error);
  if (relays) {
    g_autofree gchar *details = g_strdup_printf(_("Signer %.12s… via %u relay(s)"),
      parsed.remote_signer_pubkey_hex, (guint)g_strv_length(relays));
    gtk_label_set_text(self->bunker_details, details);
    gtk_widget_set_visible(GTK_WIDGET(self->bunker_details), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(self->connect_button), TRUE);
    show_error(self, NULL);
  } else show_error(self, error ? error->message : _("Invalid bunker relays."));
  nostr_nip46_uri_bunker_free(&parsed);
}

static void
start_bunker(GhNip46PairDialog *self)
{
  const gchar *text = gtk_editable_get_text(GTK_EDITABLE(self->bunker_row));
  if (g_str_has_prefix(text, "nostrconnect://")) {
    show_error(self, _("That link is for your signer to scan — use the QR tab."));
    return;
  }
  NostrNip46BunkerURI parsed = {0};
  if (nostr_nip46_uri_parse_bunker(text, &parsed) != 0 || !parsed.n_relays) {
    nostr_nip46_uri_bunker_free(&parsed);
    show_error(self, _("Paste a complete bunker:// link with at least one relay."));
    return;
  }
  g_autoptr(GError) error = NULL;
  g_auto(GStrv) relays = validate_relays(self, (const gchar *const *)parsed.relays, &error);
  if (!relays) {
    nostr_nip46_uri_bunker_free(&parsed);
    show_error(self, error ? error->message : _("Invalid bunker relays."));
    return;
  }
  g_autofree gchar *details = g_strdup_printf(
    _("Signer %.12s… via %u relay(s) — connecting…"),
    parsed.remote_signer_pubkey_hex, (guint)g_strv_length(relays));
  gtk_label_set_text(self->bunker_details, details);
  gtk_widget_set_visible(GTK_WIDGET(self->bunker_details), TRUE);
  /* Preserve the exact one-use token only in the attempt session. */
  GhNip46Session *session = gh_nip46_session_new_bunker(text,
    self->custom_scope ? &self->scope_transport : NULL,
    self->custom_scope_auth ? &self->scope_auth : NULL,
    self->custom_publish ? &self->publish_transport : NULL,
    self->custom_publish_auth ? &self->publish_auth : NULL,
    self->transport_data, &error);
  nostr_nip46_uri_bunker_free(&parsed);
  if (!session) { show_error(self, error ? error->message : _("Could not use that bunker link.")); return; }
  start_pair(self, session, g_steal_pointer(&relays), NULL);
  gtk_label_set_text(self->bunker_details, details);
  gtk_widget_set_visible(GTK_WIDGET(self->bunker_details), TRUE);
  gtk_editable_set_text(GTK_EDITABLE(self->bunker_row), "");
}

static void
on_mode_changed(GtkStack *stack, GParamSpec *pspec, gpointer data)
{
  (void)pspec;
  GhNip46PairDialog *self = data;
  if (self->closing || self->state == PAIR_SAVING || self->state == PAIR_SELECTING ||
      self->state == PAIR_CONFIRMING || self->state == PAIR_COMPLETE) return;
  stop_attempt(self);
  self->state = PAIR_EDITING;
  show_error(self, NULL);
  if (g_strcmp0(gtk_stack_get_visible_child_name(stack), "qr") == 0)
    start_qr(self);
}

static void
on_regenerate(GtkButton *button, gpointer data)
{
  (void)button;
  GhNip46PairDialog *self = data;
  if (self->state == PAIR_SAVING || self->state == PAIR_SELECTING) return;
  stop_attempt(self);
  self->state = PAIR_EDITING;
  start_qr(self);
}

static void
on_connect(GtkButton *button, gpointer data)
{
  (void)button;
  GhNip46PairDialog *self = data;
  if (self->state == PAIR_EDITING) start_bunker(self);
}

static void
on_copy(GtkButton *button, gpointer data)
{
  (void)button;
  GhNip46PairDialog *self = data;
  if (!self->uri) return;
  GdkClipboard *clipboard = gtk_widget_get_clipboard(GTK_WIDGET(self));
  gdk_clipboard_set_text(clipboard, self->uri);
  show_error(self, _("Pairing link copied. Your clipboard now holds pairing access."));
}

static void
pasted(GObject *source, GAsyncResult *result, gpointer data)
{
  GhNip46PairDialog *self = data;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(source), result, &error);
  if (!self->closing && text) gtk_editable_set_text(GTK_EDITABLE(self->bunker_row), text);
  g_object_unref(self);
}

static void
on_paste(GtkButton *button, gpointer data)
{
  (void)button;
  GhNip46PairDialog *self = data;
  gdk_clipboard_read_text_async(gtk_widget_get_clipboard(GTK_WIDGET(self)), NULL,
                                pasted, g_object_ref(self));
}

static void
on_cancel(GtkButton *button, gpointer data)
{
  (void)button;
  adw_dialog_close(ADW_DIALOG(data));
}

static void
on_closed(AdwDialog *dialog, gpointer data)
{
  (void)data;
  GhNip46PairDialog *self = GH_NIP46_PAIR_DIALOG(dialog);
  self->closing = TRUE;
  g_clear_handle_id(&self->select_timeout, g_source_remove);
  stop_attempt(self);
  if (self->auth_prompt) gh_nip46_auth_url_clear(self->auth_prompt);
}

static void
unroot(GtkWidget *widget)
{
  GhNip46PairDialog *self = GH_NIP46_PAIR_DIALOG(widget);
  if (self->state != PAIR_SAVING) {
    self->closing = TRUE;
    g_clear_handle_id(&self->select_timeout, g_source_remove);
    stop_attempt(self);
  }
  GTK_WIDGET_CLASS(gh_nip46_pair_dialog_parent_class)->unroot(widget);
}

static void
dispose(GObject *object)
{
  GhNip46PairDialog *self = GH_NIP46_PAIR_DIALOG(object);
  self->closing = TRUE;
  g_clear_handle_id(&self->select_timeout, g_source_remove);
  stop_attempt(self);
  g_clear_object(&self->accounts);
  g_clear_object(&self->settings);
  g_clear_object(&self->credentials);
  g_clear_object(&self->auth_prompt);
  g_clear_object(&self->name_source);
  g_clear_pointer(&self->pending_npub, g_free);
  G_OBJECT_CLASS(gh_nip46_pair_dialog_parent_class)->dispose(object);
}

static void
gh_nip46_pair_dialog_class_init(GhNip46PairDialogClass *klass)
{
  GObjectClass *object = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget = GTK_WIDGET_CLASS(klass);
  object->dispose = dispose;
  widget->unroot = unroot;
  gtk_widget_class_set_template_from_resource(widget,
    "/org/nostr/Groundhog/ui/gh-nip46-pair-dialog.ui");
#define BIND(name) gtk_widget_class_bind_template_child(widget, GhNip46PairDialog, name)
  BIND(mode_switcher); BIND(mode_stack); BIND(qr_status); BIND(qr_picture);
  BIND(uri_row); BIND(relay_row); BIND(copy_button); BIND(regenerate_button);
  BIND(bunker_row); BIND(paste_button); BIND(bunker_details); BIND(connect_button);
  BIND(error_label); BIND(cancel_button);
#undef BIND
  signals[SIGNAL_CONFIRMATION_PRESENTED] = g_signal_new("confirmation-presented",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, ADW_TYPE_ALERT_DIALOG);
}

static void
gh_nip46_pair_dialog_init(GhNip46PairDialog *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));
  self->state = PAIR_EDITING;
  g_autofree gchar *joined = g_strjoinv(", ", (gchar **)amber_relays);
  gtk_editable_set_text(GTK_EDITABLE(self->relay_row), joined);
  gtk_widget_set_sensitive(GTK_WIDGET(self->copy_button), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(self->connect_button), FALSE);
  g_signal_connect(self->bunker_row, "changed", G_CALLBACK(on_bunker_changed), self);
  g_signal_connect(self->mode_stack, "notify::visible-child-name", G_CALLBACK(on_mode_changed), self);
  g_signal_connect(self->regenerate_button, "clicked", G_CALLBACK(on_regenerate), self);
  g_signal_connect(self->connect_button, "clicked", G_CALLBACK(on_connect), self);
  g_signal_connect(self->copy_button, "clicked", G_CALLBACK(on_copy), self);
  g_signal_connect(self->paste_button, "clicked", G_CALLBACK(on_paste), self);
  g_signal_connect(self->cancel_button, "clicked", G_CALLBACK(on_cancel), self);
  g_signal_connect(self, "closed", G_CALLBACK(on_closed), NULL);
}

GhNip46PairDialog *
gh_nip46_pair_dialog_new(const GhNip46PairConfig *config)
{
  g_return_val_if_fail(config && GH_IS_ACCOUNT_CONTROLLER(config->accounts) &&
                       G_IS_SETTINGS(config->settings), NULL);
  GhNip46PairDialog *self = g_object_new(GH_TYPE_NIP46_PAIR_DIALOG, NULL);
  self->accounts = g_object_ref(config->accounts);
  self->settings = g_object_ref(config->settings);
  self->credentials = config->credentials ? g_object_ref(config->credentials) :
                                          gh_nip46_credential_store_new();
  GApplication *app = g_application_get_default();
  if (GTK_IS_APPLICATION(app)) {
    self->auth_prompt = gh_nip46_auth_url_new(GTK_APPLICATION(app), GTK_WIDGET(self));
    g_signal_connect(self->auth_prompt, "launch-failed",
                     G_CALLBACK(on_auth_launch_failed), self);
  }
  if (config->display_name && config->name_source) {
    self->display_name = config->display_name;
    self->name_source = g_object_ref(config->name_source);
  }
  if (config->scope_transport) {
    self->scope_transport = *config->scope_transport;
    self->custom_scope = TRUE;
  }
  if (config->scope_auth) { self->scope_auth = *config->scope_auth; self->custom_scope_auth = TRUE; }
  if (config->publish_transport) {
    self->publish_transport = *config->publish_transport;
    self->custom_publish = TRUE;
  }
  if (config->publish_auth) {
    self->publish_auth = *config->publish_auth;
    self->custom_publish_auth = TRUE;
  }
  self->transport_data = config->transport_data;
  self->test_save = config->test_save;
  self->test_save_data = config->test_save_data;
  g_signal_connect_object(self->accounts, "changed", G_CALLBACK(selection_changed), self, 0);
  start_qr(self);
  return self;
}

gboolean
gh_nip46_pair_dialog_qr_is_visible(GhNip46PairDialog *self)
{
  g_return_val_if_fail(GH_IS_NIP46_PAIR_DIALOG(self), FALSE);
  return gtk_widget_get_visible(GTK_WIDGET(self->qr_picture));
}
