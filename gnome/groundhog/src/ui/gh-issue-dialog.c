#include "gh-issue-dialog.h"
#include "gh-diagnostics.h"
#include "gh-identity.h"
#include "gh-inbox-setup.h"
#include "gh-relay-publish.h"
#include "gh-relay-scope.h"
#include <nip34.h>
#include <nostr-event.h>
#include <nostr-tag.h>
#include <glib/gi18n.h>
#include <string.h>

/* gnostr-bug-report-dialog.c targets this same kind-30617 announcement. */
#define REPO_OWNER "cdee943cbb19c51ab847a66d5d774373aa9f63d287246bb59b0827fa5e637400"
#define REPO_ID "nostrc"
#define REPO_ADDRESS "30617:" REPO_OWNER ":" REPO_ID
#define MAX_USER_LABELS 16
#define MAX_LABEL_BYTES 64

/* ---- Canonical draft builder (preview and Publish share it) ---- */

static gboolean
invalid(GError **error, const gchar *message)
{
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, message);
  return FALSE;
}

static gchar *
stripped(const gchar *text)
{
  return g_strstrip(g_strdup(text ? text : ""));
}

static void
append_section(GString *body, const gchar *heading, const gchar *text)
{
  g_autofree gchar *copy = stripped(text);
  if (*copy)
    g_string_append_printf(body, "\n\n## %s\n\n%s", heading, copy);
}

static gboolean
label_ok(const gchar *label)
{
  if (strlen(label) > MAX_LABEL_BYTES || !g_utf8_validate(label, -1, NULL))
    return FALSE;
  for (const gchar *p = label; *p; p = g_utf8_next_char(p)) {
    gunichar c = g_utf8_get_char(p);
    if (g_unichar_isspace(c) || g_unichar_iscntrl(c))
      return FALSE;
  }
  return TRUE;
}

static gboolean
parse_labels(const gchar *csv, GPtrArray *labels, GError **error)
{
  g_ptr_array_add(labels, g_strdup("bug"));
  g_ptr_array_add(labels, g_strdup("groundhog"));
  g_auto(GStrv) parts = g_strsplit(csv ? csv : "", ",", -1);
  for (guint i = 0; parts[i]; i++) {
    gchar *label = g_strstrip(parts[i]);
    if (!*label || g_ptr_array_find_with_equal_func(labels, label, g_str_equal, NULL))
      continue;
    if (!label_ok(label))
      return invalid(error, _("Labels are separated by commas. Each label can be up to 64 bytes, without spaces."));
    if (labels->len == 2 + MAX_USER_LABELS)
      return invalid(error, _("Add up to 16 labels."));
    g_ptr_array_add(labels, g_strdup(label));
  }
  return TRUE;
}

static gboolean
is_object_id(const gchar *id)
{
  gsize len = strlen(id);
  if (len != 40 && len != 64) /* SHA-1 or SHA-256 repository object format */
    return FALSE;
  for (gsize i = 0; i < len; i++)
    if (!g_ascii_isxdigit(id[i]))
      return FALSE;
  return TRUE;
}

static gboolean
append_commits(GString *body, const gchar *text, GError **error)
{
  g_auto(GStrv) parts = g_strsplit_set(text ? text : "", ", \t\r\n", -1);
  g_autoptr(GPtrArray) seen = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; parts[i]; i++) {
    if (!*parts[i])
      continue;
    if (!is_object_id(parts[i]))
      return invalid(error, _("Related commits must be full Git commit IDs (40 or 64 hexadecimal characters)."));
    gchar *id = g_ascii_strdown(parts[i], -1);
    if (g_ptr_array_find_with_equal_func(seen, id, g_str_equal, NULL)) {
      g_free(id);
      continue;
    }
    if (!seen->len)
      g_string_append(body, "\n\n## Related commits\n");
    g_string_append_printf(body, "\n- `%s`", id);
    g_ptr_array_add(seen, id);
  }
  return TRUE;
}

static gboolean
attachment_ok(const gchar *text)
{
  g_autoptr(GUri) uri = g_uri_parse(text, G_URI_FLAGS_NONE, NULL);
  if (!uri)
    return FALSE;
  const gchar *scheme = g_uri_get_scheme(uri);
  const gchar *host = g_uri_get_host(uri);
  /* A reference for readers, never fetched or uploaded by Groundhog. No
   * credentials: a public report must not carry userinfo. */
  return (g_ascii_strcasecmp(scheme, "https") == 0 || g_ascii_strcasecmp(scheme, "http") == 0) &&
         host && *host && !g_uri_get_userinfo(uri);
}

static gboolean
append_attachments(GString *body, const gchar *text, GError **error)
{
  g_auto(GStrv) parts = g_strsplit_set(text ? text : "", ", \t\r\n", -1);
  g_autoptr(GPtrArray) seen = g_ptr_array_new();
  for (guint i = 0; parts[i]; i++) {
    if (!*parts[i] || g_ptr_array_find_with_equal_func(seen, parts[i], g_str_equal, NULL))
      continue;
    if (!attachment_ok(parts[i]))
      return invalid(error, _("Attachment URLs must be web addresses without a user name or password. Groundhog does not upload files."));
    if (!seen->len)
      g_string_append(body, "\n\n## Attachments\n");
    g_string_append_printf(body, "\n- %s", parts[i]);
    g_ptr_array_add(seen, parts[i]);
  }
  return TRUE;
}

GhIssueDraft *
gh_issue_draft_new(const gchar *title, const gchar *description,
                   const GnNip34IssueFieldsSnapshot *fields,
                   const gchar *diagnostics, GError **error)
{
  g_return_val_if_fail(error == NULL || *error == NULL, NULL);
  g_autofree gchar *subject = stripped(title);
  g_autofree gchar *text = stripped(description);
  if (!*subject || !*text) {
    invalid(error, _("Enter a title and description."));
    return NULL;
  }
  if (strlen(subject) > GH_ISSUE_TITLE_MAX_BYTES) {
    invalid(error, _("The title is too long. Shorten it to 640 bytes or fewer."));
    return NULL;
  }
  if (diagnostics && (strlen(diagnostics) > GH_ISSUE_DIAGNOSTICS_MAX_BYTES ||
                      !g_utf8_validate(diagnostics, -1, NULL))) {
    invalid(error, _("The local diagnostics are larger than 8,000 bytes. Leave them out of this report."));
    return NULL;
  }
  g_autoptr(GPtrArray) labels = g_ptr_array_new_with_free_func(g_free);
  if (!parse_labels(fields ? fields->labels : NULL, labels, error))
    return NULL;
  g_autoptr(GString) body = g_string_new(text);
  if (fields) {
    append_section(body, "Steps to Reproduce", fields->steps);
    append_section(body, "Expected Result", fields->expected);
    append_section(body, "Actual Result", fields->actual);
    if (!append_attachments(body, fields->attachment_urls, error) ||
        !append_commits(body, fields->related_commits, error))
      return NULL;
  }
  if (diagnostics && *diagnostics) {
    g_string_append(body, "\n\n## Local diagnostics\n\n```text\n");
    g_string_append(body, diagnostics);
    if (body->str[body->len - 1] != '\n')
      g_string_append_c(body, '\n');
    g_string_append(body, "```");
  }
  /* Checked on the assembled body: never trim anything to make it fit. */
  if (body->len > GH_ISSUE_BODY_MAX_BYTES) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                _("The report is %" G_GSIZE_FORMAT " bytes; the limit is %d. Shorten it or leave out local diagnostics."),
                body->len, GH_ISSUE_BODY_MAX_BYTES);
    return NULL;
  }
  GhIssueDraft *draft = g_new0(GhIssueDraft, 1);
  draft->title = g_steal_pointer(&subject);
  draft->body = g_string_free(g_steal_pointer(&body), FALSE);
  g_ptr_array_add(labels, NULL);
  draft->labels = (GStrv)g_ptr_array_free(g_steal_pointer(&labels), FALSE);
  return draft;
}

void
gh_issue_draft_free(GhIssueDraft *draft)
{
  if (!draft)
    return;
  g_free(draft->title);
  g_free(draft->body);
  g_strfreev(draft->labels);
  g_free(draft);
}

gboolean
gh_issue_draft_equal(const GhIssueDraft *a, const GhIssueDraft *b)
{
  return a && b && g_str_equal(a->title, b->title) && g_str_equal(a->body, b->body) &&
         g_strv_equal((const gchar *const *)a->labels, (const gchar *const *)b->labels);
}

static NostrEvent *
draft_event(const GhIssueDraft *draft, const gchar *pubkey)
{
  /* Same shared NIP-34 builder used by gnostr. */
  NostrEvent *event = nip34_create_issue(REPO_OWNER, REPO_ID, draft->title, draft->body,
                                         (const char *const *)draft->labels, NULL);
  if (event && pubkey)
    nostr_event_set_pubkey(event, pubkey);
  return event;
}

gchar *
gh_issue_draft_to_unsigned_json(const GhIssueDraft *draft, const gchar *pubkey)
{
  g_return_val_if_fail(draft != NULL, NULL);
  NostrEvent *event = draft_event(draft, pubkey);
  if (!event)
    return NULL;
  gchar *json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  return json;
}

/* ---- Dialog ---- */

struct _GhIssueDialog {
  AdwDialog parent_instance;
  AdwEntryRow *title_row;
  AdwEntryRow *relays_row;
  GtkTextView *body_view;
  GnNip34IssueFields *issue_fields;
  GtkCheckButton *diagnostics_check;
  GtkLabel *diagnostics_preview;
  GtkLabel *status;
  GtkButton *review_button;
  GhAccountController *accounts;
  GCancellable *cancel;
  guint64 generation;
  gchar *pubkey;
  gchar *diagnostics;    /* snapshot taken when the box was checked */
  GhIssueDraft *reviewed; /* frozen at Review; Publish refuses if it differs */
  gchar *reviewed_json;  /* the exact unsigned event shown in the preview */
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
clear_review(GhIssueDialog *self)
{
  g_clear_pointer(&self->reviewed, gh_issue_draft_free);
  g_clear_pointer(&self->reviewed_json, g_free);
  g_clear_pointer(&self->relays, g_strfreev);
}

static void
account_changed(GhIssueDialog *self)
{
  if (!gh_account_controller_is_current(self->accounts, self->generation)) {
    stop(self);
    clear_review(self);
    self->submitted = TRUE;
    gtk_widget_set_sensitive(GTK_WIDGET(self->review_button), FALSE);
    gtk_label_set_text(self->status, _("The account changed. Close this report and open a new one."));
  }
}

static void
diagnostics_toggled(GtkCheckButton *check, GhIssueDialog *self)
{
  g_clear_pointer(&self->diagnostics, g_free);
  GhDiagnostics *diagnostics = gh_diagnostics_get_default();
  if (gtk_check_button_get_active(check) && diagnostics)
    self->diagnostics = gh_diagnostics_snapshot(diagnostics);
  gtk_label_set_text(self->diagnostics_preview, self->diagnostics ? self->diagnostics : "");
  gtk_widget_set_visible(GTK_WIDGET(self->diagnostics_preview), self->diagnostics != NULL);
}

static GhIssueDraft *
collect_draft(GhIssueDialog *self, GError **error)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(self->body_view);
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  g_autofree gchar *body = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  g_autoptr(GnNip34IssueFieldsSnapshot) fields = gn_nip34_issue_fields_snapshot(self->issue_fields);
  const gchar *diagnostics = gtk_check_button_get_active(self->diagnostics_check) ? self->diagnostics : NULL;
  return gh_issue_draft_new(gtk_editable_get_text(GTK_EDITABLE(self->title_row)), body,
                            fields, diagnostics, error);
}

static GStrv
collect_relays(GhIssueDialog *self, GError **error)
{
  g_auto(GStrv) parts = g_strsplit_set(gtk_editable_get_text(GTK_EDITABLE(self->relays_row)),
                                      " ,\n\t\r", -1);
  g_autoptr(GPtrArray) urls = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; parts[i]; i++) {
    if (!*parts[i] || g_ptr_array_find_with_equal_func(urls, parts[i], g_str_equal, NULL))
      continue;
    g_autofree gchar *url = gh_inbox_setup_normalize_url(parts[i], NULL);
    if (!url || urls->len == 16) {
      invalid(error, _("Enter up to 16 secure WebSocket relay URLs, separated by spaces."));
      return NULL;
    }
    if (!g_ptr_array_find_with_equal_func(urls, url, g_str_equal, NULL))
      g_ptr_array_add(urls, g_steal_pointer(&url));
  }
  if (!urls->len) {
    invalid(error, _("Choose at least one relay for the public issue."));
    return NULL;
  }
  g_ptr_array_add(urls, NULL);
  return (GStrv)g_ptr_array_free(g_steal_pointer(&urls), FALSE);
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
   * requested (reviewed) event. The publisher verifies again and requires
   * relay OKs. */
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

static gboolean
review_still_current(GhIssueDialog *self)
{
  g_autoptr(GhIssueDraft) current = collect_draft(self, NULL);
  g_auto(GStrv) relays = collect_relays(self, NULL);
  return self->reviewed_json && gh_issue_draft_equal(current, self->reviewed) && relays &&
         g_strv_equal((const gchar *const *)relays, (const gchar *const *)self->relays);
}

static void
consent_response(AdwAlertDialog *alert, const gchar *response, GhIssueDialog *self)
{
  (void)alert;
  self->reviewing = FALSE;
  if (!g_str_equal(response, "publish") || self->closed || self->submitted ||
      !gh_account_controller_is_current(self->accounts, self->generation)) {
    if (!self->submitted)
      clear_review(self);
    return;
  }
  /* Any edit after Review invalidates it: publish only what was shown. */
  if (!review_still_current(self)) {
    clear_review(self);
    gtk_label_set_text(self->status, _("The report changed after review. Review it again before publishing."));
    return;
  }
  self->submitted = TRUE;
  gtk_widget_set_sensitive(GTK_WIDGET(self->review_button), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(self->title_row), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(self->relays_row), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(self->body_view), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(self->issue_fields), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(self->diagnostics_check), FALSE);
  gtk_label_set_text(self->status, _("Waiting for Grotto…"));
  gh_account_controller_sign_with_cancellable_async(self->accounts, self->reviewed_json, self->cancel,
                                                     signed_issue, g_object_ref(self));
}

static gchar *
format_tags(NostrEvent *event)
{
  GString *out = g_string_new(NULL);
  const NostrTags *tags = nostr_event_get_tags(event);
  for (gsize i = 0; tags && i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    for (gsize j = 0; j < nostr_tag_size(tag); j++) {
      if (j)
        g_string_append(out, j == 1 ? ": " : ", ");
      g_string_append(out, nostr_tag_get(tag, j));
    }
    g_string_append_c(out, '\n');
  }
  return g_string_free(out, FALSE);
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
  clear_review(self);
  g_autoptr(GError) error = NULL;
  g_autoptr(GhIssueDraft) draft = collect_draft(self, &error);
  g_auto(GStrv) relays = draft ? collect_relays(self, &error) : NULL;
  if (!relays) {
    gtk_label_set_text(self->status, error->message);
    return;
  }
  /* Freeze the exact unsigned event; the preview is rendered from it and
   * Publish signs it unchanged. */
  NostrEvent *event = draft_event(draft, self->pubkey);
  g_autofree gchar *json = event ? nostr_event_serialize_compact(event) : NULL;
  if (!json) {
    if (event)
      nostr_event_free(event);
    gtk_label_set_text(self->status, _("Could not build the issue."));
    return;
  }
  g_autofree gchar *tags = format_tags(event);
  nostr_event_free(event);
  gtk_label_set_text(self->status, "");
  self->reviewed = g_steal_pointer(&draft);
  self->reviewed_json = g_steal_pointer(&json);
  self->relays = g_steal_pointer(&relays);
  g_autofree gchar *relay_list = g_strjoinv("\n", self->relays);
  g_autofree gchar *preview = g_strdup_printf(
    _("Title:\n%s\n\nBody:\n%s\n\nTags:\n%s\nRelays:\n%s\n\nYour public key:\n%s\n\nRepository:\n%s\n\nThis kind-1621 event, timestamped now and signed by your account, will be published."),
    self->reviewed->title, self->reviewed->body, tags, relay_list, self->pubkey, REPO_ADDRESS);
  gboolean with_diagnostics = gtk_check_button_get_active(self->diagnostics_check) &&
                              self->diagnostics && *self->diagnostics;
  AdwDialog *alert = adw_alert_dialog_new(_("Publish This Issue Publicly?"),
    with_diagnostics
      ? _("This is not a private message. Anyone can read and copy this report and associate it with your public key. Deletion cannot be guaranteed. The local diagnostics shown in the body are included. No files are uploaded.")
      : _("This is not a private message. Anyone can read and copy this report and associate it with your public key. Deletion cannot be guaranteed. No logs, files or device details are added."));
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
  g_clear_pointer(&self->diagnostics, g_free);
  clear_review(self);
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
  g_type_ensure(GN_TYPE_NIP34_ISSUE_FIELDS);
  gtk_widget_class_set_template_from_resource(widget_class, "/org/nostr/Groundhog/ui/gh-issue-dialog.ui");
  gtk_widget_class_bind_template_child(widget_class, GhIssueDialog, title_row);
  gtk_widget_class_bind_template_child(widget_class, GhIssueDialog, relays_row);
  gtk_widget_class_bind_template_child(widget_class, GhIssueDialog, body_view);
  gtk_widget_class_bind_template_child(widget_class, GhIssueDialog, issue_fields);
  gtk_widget_class_bind_template_child(widget_class, GhIssueDialog, diagnostics_check);
  gtk_widget_class_bind_template_child(widget_class, GhIssueDialog, diagnostics_preview);
  gtk_widget_class_bind_template_child(widget_class, GhIssueDialog, status);
  gtk_widget_class_bind_template_child(widget_class, GhIssueDialog, review_button);
}

static void
gh_issue_dialog_init(GhIssueDialog *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));
  self->cancel = g_cancellable_new();
  g_signal_connect(self->review_button, "clicked", G_CALLBACK(review), self);
  g_signal_connect(self->diagnostics_check, "toggled", G_CALLBACK(diagnostics_toggled), self);
  /* Off for every new issue, even when collection is on. */
  gtk_check_button_set_active(self->diagnostics_check, FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(self->diagnostics_check), gh_diagnostics_get_default() != NULL);
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
