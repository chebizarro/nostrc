#include "gh-issue-dialog.h"
#include "gh-identity.h"
#include "gh-inbox-setup.h"
#include "gh-relay-publish.h"
#include "gh-relay-scope.h"
#include <nip34.h>
#include <nostr-event.h>
#include <glib/gi18n.h>

/* gnostr-bug-report-dialog.c targets this same kind-30617 announcement. */
#define REPO_OWNER "cdee943cbb19c51ab847a66d5d774373aa9f63d287246bb59b0827fa5e637400"
#define REPO_ID "nostrc"
#define REPO_ADDRESS "30617:" REPO_OWNER ":" REPO_ID

struct _GhIssueDialog {
  AdwDialog parent_instance;
  AdwEntryRow *title_row;
  AdwEntryRow *relays_row;
  GtkTextView *body_view;
  GtkLabel *status;
  GtkButton *review_button;
  GhAccountController *accounts;
  GCancellable *cancel;
  guint64 generation;
  gchar *pubkey;
  gchar *title;
  gchar *body;
  GStrv relays;
  GhRelayPublish *publish;
  gboolean closed;
  gboolean reviewing;
  gboolean submitted;
};

G_DEFINE_FINAL_TYPE(GhIssueDialog, gh_issue_dialog, ADW_TYPE_DIALOG)

static void
stop(GhIssueDialog *self)
{
  if (self->cancel)
    g_cancellable_cancel(self->cancel);
  if (self->publish)
    gh_relay_publish_cancel(self->publish);
}

static void
closed(AdwDialog *dialog)
{
  GhIssueDialog *self = GH_ISSUE_DIALOG(dialog);
  self->closed = TRUE;
  stop(self);
  ADW_DIALOG_CLASS(gh_issue_dialog_parent_class)->closed(dialog);
}

static void
account_changed(GhIssueDialog *self)
{
  if (!gh_account_controller_is_current(self->accounts, self->generation)) {
    stop(self);
    self->submitted = TRUE;
    gtk_widget_set_sensitive(GTK_WIDGET(self->review_button), FALSE);
    gtk_label_set_text(self->status, _("The account changed. Close this report and open a new one."));
  }
}

static void
published(GhRelayPublish *publish, const GhRelayPublishSummary *summary, gpointer data)
{
  (void)publish;
  GhIssueDialog *self = data;
  if (self->closed)
    return;
  if (summary->accepted == summary->total)
    gtk_label_set_text(self->status, _("Issue published. It is public on Nostr."));
  else if (summary->any_accepted)
    gtk_label_set_text(self->status, _("Issue published on some relays, but not all. It is public on Nostr."));
  else
    gtk_label_set_text(self->status, _("No relay confirmed publication. The issue may still have reached a relay."));
  /* Never rebuild and re-sign on retry: that could create a duplicate issue. */
}

static void
signed_issue(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  g_autoptr(GhIssueDialog) self = data;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *json = gh_account_controller_sign_finish(result, &error);
  if (self->closed || g_cancellable_is_cancelled(self->cancel) ||
      !gh_account_controller_is_current(self->accounts, self->generation))
    return;
  /* GhSigner checks signature, pubkey and every unsigned field against the
   * requested event. The publisher verifies again and requires relay OKs. */
  if (json)
    self->publish = gh_relay_publish_new(self->generation, json, NULL, published, self, &error);
  if (self->publish) {
    for (guint i = 0; self->relays[i]; i++)
      if (!gh_relay_publish_add_url(self->publish, self->relays[i], &error))
        break;
    if (!error) {
      gtk_label_set_text(self->status, _("Publishing issue…"));
      if (gh_relay_publish_start(self->publish, &error))
        return;
    }
  }
  stop(self);
  gtk_label_set_text(self->status, error ? error->message : _("The issue could not be signed."));
}

static void
consent_response(AdwAlertDialog *alert, const gchar *response, GhIssueDialog *self)
{
  (void)alert;
  self->reviewing = FALSE;
  if (!g_str_equal(response, "publish") || self->closed || self->submitted ||
      !gh_account_controller_is_current(self->accounts, self->generation))
    return;
  self->submitted = TRUE;
  gtk_widget_set_sensitive(GTK_WIDGET(self->review_button), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(self->title_row), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(self->relays_row), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(self->body_view), FALSE);
  /* Same shared NIP-34 builder used by gnostr. Deliberately after consent. */
  const char *labels[] = { "bug", "groundhog", NULL };
  NostrEvent *event = nip34_create_issue(REPO_OWNER, REPO_ID, self->title, self->body,
                                        labels, NULL);
  if (!event) {
    gtk_label_set_text(self->status, _("Could not build the issue."));
    return;
  }
  nostr_event_set_pubkey(event, self->pubkey);
  g_autofree gchar *json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  if (!json) {
    gtk_label_set_text(self->status, _("Could not build the issue."));
    return;
  }
  gtk_label_set_text(self->status, _("Waiting for Nostr Signer…"));
  gh_account_controller_sign_with_cancellable_async(self->accounts, json, self->cancel,
                                                     signed_issue, g_object_ref(self));
}

static void
review(GtkButton *button, GhIssueDialog *self)
{
  (void)button;
  if (self->reviewing || self->submitted || self->closed)
    return;
  if (!self->pubkey || !gh_account_controller_is_current(self->accounts, self->generation)) {
    gtk_label_set_text(self->status, _("Select an account, then open Report an Issue again."));
    return;
  }
  g_clear_pointer(&self->title, g_free);
  g_clear_pointer(&self->body, g_free);
  g_clear_pointer(&self->relays, g_strfreev);
  self->title = g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->title_row)));
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(self->body_view);
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  self->body = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  g_autofree gchar *title_check = g_strdup(self->title);
  g_autofree gchar *body_check = g_strdup(self->body);
  if (!*g_strstrip(title_check) || !*g_strstrip(body_check) ||
      strlen(self->title) > 640 || strlen(self->body) > 16000) {
    gtk_label_set_text(self->status, _("Enter a title and description (up to 16000 bytes)."));
    return;
  }
  g_auto(GStrv) parts = g_strsplit_set(gtk_editable_get_text(GTK_EDITABLE(self->relays_row)),
                                      " ,\n\t\r", -1);
  g_autoptr(GPtrArray) urls = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; parts[i]; i++) {
    if (!*parts[i] || g_ptr_array_find_with_equal_func(urls, parts[i], g_str_equal, NULL))
      continue;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *url = gh_inbox_setup_normalize_url(parts[i], &error);
    if (!url || urls->len == 16) {
      gtk_label_set_text(self->status, _("Enter up to 16 secure WebSocket relay URLs, separated by spaces."));
      return;
    }
    if (!g_ptr_array_find_with_equal_func(urls, url, g_str_equal, NULL))
      g_ptr_array_add(urls, g_steal_pointer(&url));
  }
  if (!urls->len) {
    gtk_label_set_text(self->status, _("Choose at least one relay for the public issue."));
    return;
  }
  g_ptr_array_add(urls, NULL);
  self->relays = (GStrv)g_ptr_array_free(g_steal_pointer(&urls), FALSE);
  g_autofree gchar *relays = g_strjoinv("\n", self->relays);
  g_autofree gchar *preview = g_strdup_printf(
    _("Title:\n%s\n\nBody:\n%s\n\nRelays:\n%s\n\nYour public key:\n%s\n\nRepository:\n%s\n\nHashtags and labels: bug, groundhog (org.gnostr.issue)\nAlt text: git repository issue\nMaintainer: %s\n\nA kind-1621 event with the current time and your signature will be published."),
    self->title, self->body, relays, self->pubkey, REPO_ADDRESS, REPO_OWNER);
  AdwDialog *alert = adw_alert_dialog_new(_("Publish This Issue Publicly?"),
    _("This is not a private message. Anyone can read and copy this report and associate it with your public key. Deletion cannot be guaranteed. No logs, files or device details are added automatically."));
  GtkWidget *label = gtk_label_new(preview);
  gtk_label_set_selectable(GTK_LABEL(label), TRUE);
  gtk_label_set_wrap(GTK_LABEL(label), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 220);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 320);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), label);
  adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(alert), scroll);
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(alert), "cancel", _("Cancel"),
                                 "publish", _("Publish Public Issue"), NULL);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(alert), "cancel");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(alert), "cancel");
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(alert), "publish", ADW_RESPONSE_DESTRUCTIVE);
  self->reviewing = TRUE;
  g_signal_connect_object(alert, "response", G_CALLBACK(consent_response), self, 0);
  adw_dialog_present(alert, GTK_WIDGET(self));
}

static void
unroot(GtkWidget *widget)
{
  /* A parent window can disappear without AdwDialog::closed being emitted.
   * Cancel any in-flight signer or relay work before its UI is detached. */
  stop(GH_ISSUE_DIALOG(widget));
  GTK_WIDGET_CLASS(gh_issue_dialog_parent_class)->unroot(widget);
}

static void
dispose(GObject *object)
{
  GhIssueDialog *self = GH_ISSUE_DIALOG(object);
  stop(self);
  g_clear_pointer(&self->publish, gh_relay_publish_unref);
  g_clear_object(&self->accounts);
  g_clear_object(&self->cancel);
  g_clear_pointer(&self->pubkey, g_free);
  g_clear_pointer(&self->title, g_free);
  g_clear_pointer(&self->body, g_free);
  g_clear_pointer(&self->relays, g_strfreev);
  gtk_widget_dispose_template(GTK_WIDGET(self), GH_TYPE_ISSUE_DIALOG);
  G_OBJECT_CLASS(gh_issue_dialog_parent_class)->dispose(object);
}

static void
gh_issue_dialog_class_init(GhIssueDialogClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = dispose;
  ADW_DIALOG_CLASS(klass)->closed = closed;
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  widget_class->unroot = unroot;
  gtk_widget_class_set_template_from_resource(widget_class, "/org/nostr/Groundhog/ui/gh-issue-dialog.ui");
  gtk_widget_class_bind_template_child(widget_class, GhIssueDialog, title_row);
  gtk_widget_class_bind_template_child(widget_class, GhIssueDialog, relays_row);
  gtk_widget_class_bind_template_child(widget_class, GhIssueDialog, body_view);
  gtk_widget_class_bind_template_child(widget_class, GhIssueDialog, status);
  gtk_widget_class_bind_template_child(widget_class, GhIssueDialog, review_button);
}

static void
gh_issue_dialog_init(GhIssueDialog *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));
  self->cancel = g_cancellable_new();
  g_signal_connect(self->review_button, "clicked", G_CALLBACK(review), self);
}

GhIssueDialog *
gh_issue_dialog_new(GhAccountController *accounts, GSettings *settings)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_CONTROLLER(accounts), NULL);
  GhIssueDialog *self = g_object_new(GH_TYPE_ISSUE_DIALOG, NULL);
  self->accounts = g_object_ref(accounts);
  self->generation = gh_account_controller_get_generation(accounts);
  self->pubkey = gh_identity_pubkey_hex(gh_account_controller_get_active_npub(accounts));
  g_signal_connect_object(accounts, "changed", G_CALLBACK(account_changed), self, G_CONNECT_SWAPPED);
  if (settings) {
    g_auto(GStrv) relays = g_settings_get_strv(settings, "discovery-relays");
    g_autofree gchar *joined = g_strjoinv(" ", relays);
    gtk_editable_set_text(GTK_EDITABLE(self->relays_row), joined);
  }
  if (!self->pubkey)
    gtk_label_set_text(self->status, _("Select an account, then open Report an Issue again."));
  return self;
}
