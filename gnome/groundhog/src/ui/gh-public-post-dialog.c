#include "gh-public-post-dialog.h"
#include "gh-public-note-post.h"
#include "gh-auth-policy.h"
#include "gh-identity.h"
#include "gh-relay-publish.h"
#include <glib/gi18n.h>

struct _GhPublicPostDialog {
  AdwDialog parent_instance;
  GhAccountController *accounts;
  GhAccountRelays *relays;
  GhPublicNote *original;
  GnNostrReference *reference;
  GhPublicPostSnapshot *reviewed;
  GhRelayPublish *publish;
  GCancellable *cancel;
  GtkTextView *quote_view;
  GtkLabel *status;
  GtkButton *review_button;
  gchar *reviewed_comment;
  guint64 generation;
  gboolean quote;
  gboolean reviewing;
  gboolean submitted;
  gboolean closed;
};

typedef struct _GhPublicPostDialog GhPublicPostDialog;
typedef AdwDialogClass GhPublicPostDialogClass;
G_DEFINE_FINAL_TYPE(GhPublicPostDialog, gh_public_post_dialog, ADW_TYPE_DIALOG)

static void
stop(GhPublicPostDialog *self)
{
  if (self->cancel) g_cancellable_cancel(self->cancel);
  if (self->publish) gh_relay_publish_cancel(self->publish);
}

static void
closed(AdwDialog *dialog)
{
  GhPublicPostDialog *self = (GhPublicPostDialog *)dialog;
  self->closed = TRUE;
  stop(self);
  ADW_DIALOG_CLASS(gh_public_post_dialog_parent_class)->closed(dialog);
}

static gchar *
quote_text(GhPublicPostDialog *self)
{
  if (!self->quote_view) return NULL;
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(self->quote_view);
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  return gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
}

static const gchar *const *
current_write_relays(GhPublicPostDialog *self)
{
  if (!gh_account_controller_is_current(self->accounts, self->generation) ||
      gh_account_relays_get_generation(self->relays) != self->generation)
    return NULL;
  return gh_account_relays_get_write_relays(self->relays);
}

static void
update_ready(GhPublicPostDialog *self)
{
  if (self->submitted || self->closed) return;
  const gchar *const *urls = current_write_relays(self);
  gboolean ready = urls && urls[0];
  gtk_widget_set_sensitive(GTK_WIDGET(self->review_button), ready);
  gtk_label_set_text(self->status,
    !gh_account_controller_is_current(self->accounts, self->generation)
      ? _("The account changed. Close this public post and start again.")
      : !ready ? _("No NIP-65 write relays are configured for this account. Public posting is disabled.")
               : "");
}

static void
account_changed(GhPublicPostDialog *self)
{
  if (!gh_account_controller_is_current(self->accounts, self->generation)) {
    stop(self);
    g_clear_pointer(&self->reviewed, gh_public_post_snapshot_free);
    self->submitted = TRUE;
    gtk_widget_set_sensitive(GTK_WIDGET(self->review_button), FALSE);
    gtk_label_set_text(self->status, _("The account changed. Close this public post and start again."));
    return;
  }
  update_ready(self);
}

static void
published(GhRelayPublish *publish, const GhRelayPublishSummary *summary, gpointer data)
{
  (void)publish;
  GhPublicPostDialog *self = data;
  if (self->closed || !gh_account_controller_is_current(self->accounts, self->generation))
    return;
  g_autofree gchar *message = NULL;
  if (summary->accepted == summary->total)
    message = g_strdup_printf(_("Public post accepted by %u of %u selected relays. Other relays and readers may not have received it."),
                              summary->accepted, summary->total);
  else if (summary->any_accepted)
    message = g_strdup_printf(_("Public post accepted by %u of %u selected relays; %u rejected it and %u did not confirm. It is public where accepted."),
                              summary->accepted, summary->total, summary->rejected,
                              summary->connection_failed + summary->auth_required);
  else
    message = g_strdup_printf(_("No selected relay confirmed this public post (%u rejected, %u did not confirm). It may still have reached a relay."),
                              summary->rejected,
                              summary->connection_failed + summary->auth_required);
  gtk_label_set_text(self->status, message);
}

static void
signed_post(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  g_autoptr(GObject) owner = data;
  GhPublicPostDialog *self = (GhPublicPostDialog *)owner;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *signed_json = gh_account_controller_sign_finish(result, &error);
  if (self->closed || g_cancellable_is_cancelled(self->cancel) ||
      !gh_account_controller_is_current(self->accounts, self->generation))
    return;
  if (!signed_json) {
    gtk_label_set_text(self->status, error ? error->message : _("The signer did not sign this public post."));
    return;
  }
  const gchar *const *current = current_write_relays(self);
  if (!self->reviewed || !current ||
      !g_strv_equal(current, (const gchar *const *)self->reviewed->write_relays) ||
      !gh_public_post_snapshot_matches_signed(self->reviewed, signed_json, &error)) {
    gtk_label_set_text(self->status, error ? error->message
      : _("The account or write relays changed. This public post was not sent."));
    return;
  }
  self->publish = gh_relay_publish_new(self->generation, signed_json, NULL, published, self, &error);
  if (self->publish) {
    GhAuthPolicy *policy = gh_auth_policy_get_for_accounts(self->accounts);
    for (guint i = 0; self->reviewed->write_relays[i]; i++) {
      const gchar *url = self->reviewed->write_relays[i];
      if (!gh_relay_publish_add_url(self->publish, url, &error) ||
          !gh_auth_policy_apply_publish(policy, self->publish,
              GH_AUTH_PURPOSE_PUBLIC_POST, url, &error))
        break;
    }
    if (!error) {
      gtk_label_set_text(self->status, _("Publishing public post…"));
      if (gh_relay_publish_start(self->publish, &error)) return;
    }
  }
  stop(self);
  gtk_label_set_text(self->status, error ? error->message : _("The public post could not be published."));
}

static void
consent_response(AdwAlertDialog *alert, const gchar *response, GhPublicPostDialog *self)
{
  (void)alert;
  self->reviewing = FALSE;
  if (!g_str_equal(response, "publish") || self->submitted || self->closed) {
    g_clear_pointer(&self->reviewed, gh_public_post_snapshot_free);
    return;
  }
  g_autofree gchar *comment = quote_text(self);
  const gchar *const *current = current_write_relays(self);
  if (!self->reviewed || !current ||
      !g_strv_equal(current, (const gchar *const *)self->reviewed->write_relays) ||
      (self->quote && g_strcmp0(comment, self->reviewed_comment) != 0)) {
    g_clear_pointer(&self->reviewed, gh_public_post_snapshot_free);
    gtk_label_set_text(self->status, _("The account, write relays or quote changed after review. Review again."));
    return;
  }
  self->submitted = TRUE;
  gtk_widget_set_sensitive(GTK_WIDGET(self->review_button), FALSE);
  if (self->quote_view) gtk_widget_set_sensitive(GTK_WIDGET(self->quote_view), FALSE);
  gtk_label_set_text(self->status, _("Waiting for the account signer…"));
  gh_account_controller_sign_with_cancellable_async(self->accounts,
    self->reviewed->unsigned_json, self->cancel, signed_post, g_object_ref(self));
}

static void
review(GtkButton *button, GhPublicPostDialog *self)
{
  (void)button;
  if (self->reviewing || self->submitted || self->closed) return;
  const gchar *const *urls = current_write_relays(self);
  if (!urls || !urls[0]) { update_ready(self); return; }
  g_autofree gchar *account = gh_identity_pubkey_hex(
    gh_account_controller_get_active_npub(self->accounts));
  g_autofree gchar *comment = quote_text(self);
  g_autoptr(GError) error = NULL;
  g_clear_pointer(&self->reviewed, gh_public_post_snapshot_free);
  self->reviewed = gh_public_post_snapshot_new(self->original, self->reference,
    self->quote, comment, account, g_get_real_time() / G_USEC_PER_SEC, urls, &error);
  if (!self->reviewed) {
    gtk_label_set_text(self->status, error ? error->message : _("Could not build the public post."));
    return;
  }
  g_free(self->reviewed_comment);
  self->reviewed_comment = g_strdup(comment);
  g_autofree gchar *relay_list = g_strjoinv("\n", self->reviewed->write_relays);
  g_autofree gchar *preview = g_strdup_printf(
    _("Exact unsigned event to sign and publish:\n%s\n\nAccount pubkey:\n%s\n\nNIP-65 write relays:\n%s"),
    self->reviewed->unsigned_json, self->reviewed->account, relay_list);
  AdwAlertDialog *alert = ADW_ALERT_DIALOG(adw_alert_dialog_new(
    self->quote ? _("Publish This Quote Publicly?") : _("Publish This Repost Publicly?"),
    _("This is not a private chat message. Anyone can read and copy it and associate it with your public key. Deletion cannot be guaranteed.")));
  GtkWidget *label = gtk_label_new(preview);
  gtk_label_set_selectable(GTK_LABEL(label), TRUE);
  gtk_label_set_wrap(GTK_LABEL(label), TRUE);
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 220);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 340);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), label);
  adw_alert_dialog_set_extra_child(alert, scroll);
  adw_alert_dialog_add_responses(alert, "cancel", _("Cancel"), "publish", _("Publish Publicly"), NULL);
  adw_alert_dialog_set_default_response(alert, "cancel");
  adw_alert_dialog_set_close_response(alert, "cancel");
  adw_alert_dialog_set_response_appearance(alert, "publish", ADW_RESPONSE_DESTRUCTIVE);
  self->reviewing = TRUE;
  g_signal_connect_object(alert, "response", G_CALLBACK(consent_response), self, 0);
  adw_dialog_present(ADW_DIALOG(alert), GTK_WIDGET(self));
}

static void
unroot(GtkWidget *widget)
{
  stop((GhPublicPostDialog *)widget);
  GTK_WIDGET_CLASS(gh_public_post_dialog_parent_class)->unroot(widget);
}

static void
dispose(GObject *object)
{
  GhPublicPostDialog *self = (GhPublicPostDialog *)object;
  stop(self);
  g_clear_pointer(&self->publish, gh_relay_publish_unref);
  g_clear_pointer(&self->reviewed, gh_public_post_snapshot_free);
  g_clear_pointer(&self->original, gh_public_note_free);
  g_clear_pointer(&self->reference, gn_nostr_reference_free);
  g_clear_pointer(&self->reviewed_comment, g_free);
  g_clear_object(&self->cancel);
  g_clear_object(&self->accounts);
  g_clear_object(&self->relays);
  G_OBJECT_CLASS(gh_public_post_dialog_parent_class)->dispose(object);
}

static void
gh_public_post_dialog_class_init(GhPublicPostDialogClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = dispose;
  ADW_DIALOG_CLASS(klass)->closed = closed;
  GTK_WIDGET_CLASS(klass)->unroot = unroot;
}

static void
gh_public_post_dialog_init(GhPublicPostDialog *self)
{
  self->cancel = g_cancellable_new();
  adw_dialog_set_content_width(ADW_DIALOG(self), 500);
  adw_dialog_set_content_height(ADW_DIALOG(self), 360);
  AdwToolbarView *toolbar = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
  adw_toolbar_view_add_top_bar(toolbar, adw_header_bar_new());
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_top(box, 18);
  gtk_widget_set_margin_bottom(box, 18);
  gtk_widget_set_margin_start(box, 18);
  gtk_widget_set_margin_end(box, 18);
  GtkWidget *notice = gtk_label_new(_("Reposts and quotes are public Nostr events, not messages in this chat."));
  gtk_label_set_wrap(GTK_LABEL(notice), TRUE);
  gtk_label_set_xalign(GTK_LABEL(notice), 0);
  gtk_box_append(GTK_BOX(box), notice);
  self->quote_view = GTK_TEXT_VIEW(gtk_text_view_new());
  gtk_text_view_set_wrap_mode(self->quote_view, GTK_WRAP_WORD_CHAR);
  gtk_widget_set_vexpand(GTK_WIDGET(self->quote_view), TRUE);
  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 100);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(self->quote_view));
  gtk_box_append(GTK_BOX(box), scroll);
  self->review_button = GTK_BUTTON(gtk_button_new_with_label(_("Review Public Post…")));
  gtk_widget_set_halign(GTK_WIDGET(self->review_button), GTK_ALIGN_END);
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(self->review_button));
  self->status = GTK_LABEL(gtk_label_new(""));
  gtk_label_set_wrap(self->status, TRUE);
  gtk_label_set_xalign(self->status, 0);
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(self->status));
  adw_toolbar_view_set_content(toolbar, box);
  adw_dialog_set_child(ADW_DIALOG(self), GTK_WIDGET(toolbar));
  g_signal_connect(self->review_button, "clicked", G_CALLBACK(review), self);
}

AdwDialog *
gh_public_post_dialog_new(GhAccountController *accounts, GhAccountRelays *relays,
    const GhPublicNote *original, const gchar *reference_uri, gboolean quote)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_CONTROLLER(accounts) && GH_IS_ACCOUNT_RELAYS(relays), NULL);
  g_autoptr(GnNostrReference) reference = gn_nostr_reference_parse(reference_uri);
  if (!original || !reference) return NULL;
  g_autoptr(GhPublicNote) verified = gh_public_note_from_signed_json(original->event_json, NULL);
  if (!verified || g_strcmp0(verified->id, original->id) != 0) return NULL;
  GhPublicPostDialog *self = g_object_new(gh_public_post_dialog_get_type(), NULL);
  self->accounts = g_object_ref(accounts);
  self->relays = g_object_ref(relays);
  self->original = g_steal_pointer(&verified);
  self->reference = g_steal_pointer(&reference);
  self->generation = gh_account_controller_get_generation(accounts);
  self->quote = quote;
  adw_dialog_set_title(ADW_DIALOG(self), quote ? _("Quote Public Note") : _("Repost Public Note"));
  gtk_widget_set_visible(GTK_WIDGET(gtk_widget_get_parent(GTK_WIDGET(self->quote_view))), quote);
  g_signal_connect_object(accounts, "changed", G_CALLBACK(account_changed), self, G_CONNECT_SWAPPED);
  g_signal_connect_object(relays, "changed", G_CALLBACK(update_ready), self, G_CONNECT_SWAPPED);
  update_ready(self);
  return ADW_DIALOG(self);
}
