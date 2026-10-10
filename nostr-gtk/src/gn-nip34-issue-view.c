/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Shared NIP-34 "Report an Issue" form (nostrc-8xfib.5). The body is gnostr's
 * proven bug-report dialog (crash-log list, attachments, sequential upload
 * queue with partial-failure handling and "Send without logs", label chips,
 * processing state, repository lookup); the review flow is Groundhog's
 * (frozen unsigned event, consent prompt, stale-review rejection).
 */
#include <nostr-gtk-1.0/gn-nip34-issue-view.h>
#include <nostr-gtk-1.0/gn-nip34-issue-fields.h>
#include "gn-portable-i18n-private.h"

#include <adwaita.h>
#include <nostr-event.h>
#include <nostr-tag.h>
#include <string.h>

typedef struct {
  GFile *file;
  gboolean crash_log;
  GtkLabel *status; /* owned by the widget tree */
  gchar *url;
  gboolean failed;
} UploadItem;

struct _GnNip34IssueView {
  GtkBox parent_instance;

  GtkLabel *intro;
  GtkWidget *title_row;
  GtkTextView *body_view;
  GnNip34IssueFields *fields;
  GtkWidget *chips;
  GtkWidget *diag_box;
  GtkCheckButton *diag_check;
  GtkLabel *diag_description;
  GtkLabel *diag_preview;
  GtkWidget *diag_editor_frame;
  GtkTextView *diag_editor;
  GtkWidget *crash_group;
  GtkListBox *crash_list;
  GtkWidget *attach_group;
  GtkButton *attach_button;
  GtkListBox *attachment_list;
  GtkWidget *relays_row;
  GtkButton *lookup_button;
  GtkSpinner *spinner;
  GtkLabel *status;
  GtkButton *review_button;

  GnIssueTarget *target;
  GnIssuePublisher *publisher;
  GnIssueRepoResolver *resolver;
  GnIssueUploader *uploader;
  gchar *pubkey;
  GStrv required_labels;

  gchar *diag_heading;
  GnIssueDiagnosticsFunc diag_func;
  gpointer diag_data;
  GDestroyNotify diag_destroy;
  GnIssueDiagnosticsFlags diag_flags;
  gchar *diag_text;

  GCancellable *cancel;

  /* Frozen at Review; Publish (or Upload) refuses if anything differs. */
  GnIssueDraft *reviewed;
  gchar *reviewed_json;
  GStrv reviewed_relays;
  GnIssueTarget *reviewed_target;
  gchar *reviewed_pubkey;
  GStrv reviewed_files;

  GPtrArray *uploads; /* UploadItem */
  guint upload_index;
  gboolean uploads_done;
  gboolean crash_failed;
  gboolean skip_crash_logs;
  guint attachment_failures;

  gboolean busy;
  gboolean reviewing;
  gboolean submitted;
  gboolean closed;
  GtkWidget *container;
};

G_DEFINE_FINAL_TYPE(GnNip34IssueView, gn_nip34_issue_view, GTK_TYPE_BOX)

enum { SIGNAL_PUBLISHED, SIGNAL_TOAST, N_SIGNALS };
static guint signals[N_SIGNALS];

static void review(GnNip34IssueView *self);
static void upload_next(GnNip34IssueView *self);

static void
upload_item_free(UploadItem *item)
{
  if (!item)
    return;
  g_clear_object(&item->file);
  g_free(item->url);
  g_free(item);
}

static gchar *
text_view_text(GtkTextView *view)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(view);
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  return gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
}

static void
set_status(GnNip34IssueView *self, const gchar *text)
{
  gtk_label_set_text(self->status, text ? text : "");
}

static void
toast(GnNip34IssueView *self, const gchar *text)
{
  set_status(self, text);
  g_signal_emit(self, signals[SIGNAL_TOAST], 0, text);
}

static void
set_form_sensitive(GnNip34IssueView *self, gboolean sensitive)
{
  GtkWidget *widgets[] = {
    self->title_row, GTK_WIDGET(self->body_view), GTK_WIDGET(self->fields), self->chips,
    GTK_WIDGET(self->diag_check), GTK_WIDGET(self->diag_editor), self->relays_row,
  };
  for (guint i = 0; i < G_N_ELEMENTS(widgets); i++)
    gtk_widget_set_sensitive(widgets[i], sensitive);
  /* Uploaded files stay as reviewed: their URLs are already in the report. */
  gboolean files = sensitive && !self->uploads_done;
  gtk_widget_set_sensitive(GTK_WIDGET(self->crash_list), files);
  gtk_widget_set_sensitive(GTK_WIDGET(self->attachment_list), files);
  gtk_widget_set_sensitive(GTK_WIDGET(self->attach_button), files);
}

static void
set_busy(GnNip34IssueView *self, gboolean busy, const gchar *status)
{
  self->busy = busy;
  gtk_spinner_set_spinning(self->spinner, busy);
  gtk_widget_set_visible(GTK_WIDGET(self->spinner), busy);
  gtk_widget_set_sensitive(GTK_WIDGET(self->review_button), !busy && !self->submitted);
  gtk_widget_set_sensitive(GTK_WIDGET(self->lookup_button), !busy && !self->submitted);
  set_form_sensitive(self, !busy && !self->submitted);
  if (status)
    set_status(self, status);
}

static void
stop(GnNip34IssueView *self)
{
  if (self->cancel)
    g_cancellable_cancel(self->cancel);
}

static void
clear_review(GnNip34IssueView *self)
{
  g_clear_pointer(&self->reviewed, gn_issue_draft_free);
  g_clear_pointer(&self->reviewed_json, g_free);
  g_clear_pointer(&self->reviewed_relays, g_strfreev);
  g_clear_pointer(&self->reviewed_target, gn_issue_target_free);
  g_clear_pointer(&self->reviewed_pubkey, g_free);
  g_clear_pointer(&self->reviewed_files, g_strfreev);
}

/* ---- Collecting the report ---- */

static GStrv
url_list(GnNip34IssueView *self, gboolean crash_log)
{
  g_autoptr(GPtrArray) urls = g_ptr_array_new();
  for (guint i = 0; self->uploads_done && self->uploads && i < self->uploads->len; i++) {
    UploadItem *item = g_ptr_array_index(self->uploads, i);
    if (item->crash_log == crash_log && item->url && !(crash_log && self->skip_crash_logs))
      g_ptr_array_add(urls, g_strdup(item->url));
  }
  g_ptr_array_add(urls, NULL);
  return (GStrv)g_ptr_array_free(g_steal_pointer(&urls), FALSE);
}

static gchar *
current_diagnostics(GnNip34IssueView *self)
{
  if (!self->diag_func || !gtk_check_button_get_active(self->diag_check))
    return NULL;
  if (self->diag_flags & GN_ISSUE_DIAGNOSTICS_EDITABLE) {
    g_autofree gchar *text = text_view_text(self->diag_editor);
    g_strstrip(text);
    return *text ? g_steal_pointer(&text) : NULL;
  }
  return g_strdup(self->diag_text);
}

static GnIssueDraft *
collect_draft(GnNip34IssueView *self, gchar **diagnostics_out, GError **error)
{
  g_autofree gchar *body = text_view_text(self->body_view);
  g_autoptr(GnNip34IssueFieldsSnapshot) fields = gn_nip34_issue_fields_snapshot(self->fields);
  g_autofree gchar *diagnostics = current_diagnostics(self);
  g_auto(GStrv) crash = url_list(self, TRUE);
  g_auto(GStrv) attachments = url_list(self, FALSE);
  GnIssueDraftInput input = {
    .title = gtk_editable_get_text(GTK_EDITABLE(self->title_row)),
    .description = body,
    .fields = fields,
    .diagnostics = diagnostics,
    .diagnostics_heading = self->diag_heading,
    .required_labels = (const gchar *const *)self->required_labels,
    .crash_log_urls = (const gchar *const *)crash,
    .attachment_urls = (const gchar *const *)attachments,
  };
  GnIssueDraft *draft = gn_issue_draft_new(&input, error);
  if (diagnostics_out)
    *diagnostics_out = g_steal_pointer(&diagnostics);
  return draft;
}

static gboolean
add_relay(GPtrArray *urls, gchar *url)
{
  if (g_ptr_array_find_with_equal_func(urls, url, g_str_equal, NULL)) {
    g_free(url);
    return TRUE;
  }
  if (urls->len == GN_ISSUE_MAX_RELAYS) {
    g_free(url);
    return FALSE;
  }
  g_ptr_array_add(urls, url);
  return TRUE;
}

static GStrv
collect_relays(GnNip34IssueView *self, GError **error)
{
  g_autoptr(GPtrArray) urls = g_ptr_array_new_with_free_func(g_free);
  /* Relays announced by the repository first (best effort: a malformed one
   * is skipped), then the user's. */
  for (guint i = 0; self->target->relays[i]; i++) {
    gchar *url = gn_issue_publisher_normalize_relay(self->publisher, self->target->relays[i], NULL);
    if (url)
      add_relay(urls, url);
  }
  g_auto(GStrv) parts = g_strsplit_set(gtk_editable_get_text(GTK_EDITABLE(self->relays_row)),
                                      " ,\n\t\r", -1);
  for (guint i = 0; parts[i]; i++) {
    if (!*parts[i])
      continue;
    gchar *url = gn_issue_publisher_normalize_relay(self->publisher, parts[i], error);
    if (!url)
      return NULL;
    if (!add_relay(urls, url)) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                          _("Enter up to 16 relay URLs, separated by spaces."));
      return NULL;
    }
  }
  if (!urls->len) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        _("Choose at least one relay for the public issue."));
    return NULL;
  }
  g_ptr_array_add(urls, NULL);
  return (GStrv)g_ptr_array_free(g_steal_pointer(&urls), FALSE);
}

/* Files the user selected that are not uploaded yet, in queue order. */
static GPtrArray *
pending_uploads(GnNip34IssueView *self)
{
  GPtrArray *items = g_ptr_array_new_with_free_func((GDestroyNotify)upload_item_free);
  if (!self->uploader || self->uploads_done)
    return items;
  for (GtkWidget *row = gtk_widget_get_first_child(GTK_WIDGET(self->crash_list)); row;
       row = gtk_widget_get_next_sibling(row)) {
    GtkCheckButton *check = g_object_get_data(G_OBJECT(row), "gn-crash-check");
    GFile *file = g_object_get_data(G_OBJECT(row), "gn-file");
    if (!self->skip_crash_logs && check && file && gtk_check_button_get_active(check)) {
      UploadItem *item = g_new0(UploadItem, 1);
      item->file = g_object_ref(file);
      item->crash_log = TRUE;
      item->status = g_object_get_data(G_OBJECT(row), "gn-upload-status");
      g_ptr_array_add(items, item);
    }
  }
  for (GtkWidget *row = gtk_widget_get_first_child(GTK_WIDGET(self->attachment_list)); row;
       row = gtk_widget_get_next_sibling(row)) {
    GFile *file = g_object_get_data(G_OBJECT(row), "gn-file");
    if (file) {
      UploadItem *item = g_new0(UploadItem, 1);
      item->file = g_object_ref(file);
      item->status = g_object_get_data(G_OBJECT(row), "gn-upload-status");
      g_ptr_array_add(items, item);
    }
  }
  return items;
}

static GStrv
item_names(GPtrArray *items)
{
  GStrv names = g_new0(gchar *, items->len + 1);
  for (guint i = 0; i < items->len; i++)
    names[i] = g_file_get_parse_name(((UploadItem *)g_ptr_array_index(items, i))->file);
  return names;
}

static gboolean
review_still_current(GnNip34IssueView *self)
{
  g_autoptr(GnIssueDraft) current = collect_draft(self, NULL, NULL);
  g_auto(GStrv) relays = collect_relays(self, NULL);
  g_autoptr(GPtrArray) items = pending_uploads(self);
  g_auto(GStrv) files = item_names(items);
  return self->reviewed_json && gn_issue_draft_equal(current, self->reviewed) && relays &&
         g_strv_equal((const gchar *const *)relays, (const gchar *const *)self->reviewed_relays) &&
         gn_issue_target_equal(self->target, self->reviewed_target) &&
         g_strcmp0(self->pubkey, self->reviewed_pubkey) == 0 &&
         g_strv_equal((const gchar *const *)files, (const gchar *const *)self->reviewed_files);
}

/* ---- Publish ---- */

static void
published(GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GnNip34IssueView) self = data;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *event_id = NULL;
  g_autofree gchar *message = NULL;
  gboolean ok = gn_issue_publisher_sign_and_publish_finish(GN_ISSUE_PUBLISHER(source), result,
                                                           &event_id, &message, &error);
  if (self->closed)
    return;
  if (ok) {
    self->busy = FALSE;
    gtk_spinner_set_spinning(self->spinner, FALSE);
    gtk_widget_set_visible(GTK_WIDGET(self->spinner), FALSE);
    set_status(self, message ? message : _("Issue published. It is public on Nostr."));
    g_signal_emit(self, signals[SIGNAL_PUBLISHED], 0, event_id);
    return;
  }
  if (g_error_matches(error, GN_ISSUE_PUBLISH_ERROR, GN_ISSUE_PUBLISH_ERROR_NOT_SENT)) {
    /* Nothing left the device: the user may review and try again. */
    self->submitted = FALSE;
    clear_review(self);
    set_busy(self, FALSE, NULL);
    toast(self, message ? message : error->message);
    return;
  }
  /* Never rebuild and re-sign on retry: that could create a duplicate issue. */
  self->busy = FALSE;
  gtk_spinner_set_spinning(self->spinner, FALSE);
  gtk_widget_set_visible(GTK_WIDGET(self->spinner), FALSE);
  if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    set_status(self, message ? message : error ? error->message : _("The issue could not be published."));
}

static void
start_publish(GnNip34IssueView *self)
{
  self->submitted = TRUE;
  set_busy(self, TRUE, _("Signing and publishing issue…"));
  gn_issue_publisher_sign_and_publish_async(self->publisher, self->reviewed_json,
                                            (const gchar *const *)self->reviewed_relays,
                                            self->cancel, published, g_object_ref(self));
}

/* ---- Uploads (only after consent) ---- */

static void
set_item_status(UploadItem *item, const gchar *text, gboolean error)
{
  if (!item || !item->status)
    return;
  gtk_label_set_text(item->status, text);
  if (error)
    gtk_widget_add_css_class(GTK_WIDGET(item->status), "error");
  else
    gtk_widget_remove_css_class(GTK_WIDGET(item->status), "error");
}

static void
uploaded(GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GnNip34IssueView) self = data;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *url = gn_issue_uploader_upload_finish(GN_ISSUE_UPLOADER(source), result, &error);
  if (self->closed || !self->uploads || self->upload_index >= self->uploads->len)
    return;
  UploadItem *item = g_ptr_array_index(self->uploads, self->upload_index);
  if (url && *url) {
    item->url = g_steal_pointer(&url);
    set_item_status(item, _("Uploaded"), FALSE);
    self->upload_index++;
    upload_next(self);
    return;
  }
  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;
  set_item_status(item, _("Upload failed"), TRUE);
  item->failed = TRUE;
  if (item->crash_log) {
    /* gnostr behaviour: stop, and offer to send without crash logs. */
    self->crash_failed = TRUE;
    set_busy(self, FALSE, NULL);
    gtk_button_set_label(self->review_button, _("Send Without Logs"));
    toast(self, _("Crash log upload failed. You can send without logs."));
    return;
  }
  self->attachment_failures++;
  self->upload_index++;
  upload_next(self);
}

static void
upload_next(GnNip34IssueView *self)
{
  while (self->upload_index < self->uploads->len) {
    UploadItem *item = g_ptr_array_index(self->uploads, self->upload_index);
    if (item->url || (item->crash_log && self->skip_crash_logs)) {
      if (!item->url)
        set_item_status(item, _("Skipped"), FALSE);
      self->upload_index++;
      continue;
    }
    g_autofree gchar *name = g_file_get_basename(item->file);
    g_autofree gchar *status = g_strdup_printf(_("Uploading %s (%u of %u)…"), name,
                                               self->upload_index + 1, self->uploads->len);
    set_busy(self, TRUE, status);
    set_item_status(item, _("Uploading…"), FALSE);
    gn_issue_uploader_upload_async(self->uploader, item->file,
                                   item->crash_log ? "application/json" : NULL,
                                   self->cancel, uploaded, g_object_ref(self));
    return;
  }
  self->uploads_done = TRUE;
  set_busy(self, FALSE, "");
  if (self->attachment_failures > 0)
    toast(self, _("Some attachments failed to upload; sending the rest."));
  /* The report now contains the public URLs: show the final event again. */
  review(self);
}

static void
start_uploads(GnNip34IssueView *self)
{
  g_clear_pointer(&self->uploads, g_ptr_array_unref);
  self->uploads = pending_uploads(self);
  self->upload_index = 0;
  self->attachment_failures = 0;
  upload_next(self);
}

/* ---- Review and consent ---- */

static void
consent_response(GObject *alert, const gchar *response, GnNip34IssueView *self)
{
  (void)alert;
  self->reviewing = FALSE;
  gboolean publish = g_str_equal(response, "publish");
  gboolean upload = g_str_equal(response, "upload");
  if ((!publish && !upload) || self->closed || self->submitted || self->busy) {
    if (!self->submitted)
      clear_review(self);
    return;
  }
  /* Any edit after Review invalidates it: act only on what was shown. */
  if (!review_still_current(self)) {
    clear_review(self);
    set_status(self, _("The report changed after review. Review it again before publishing."));
    return;
  }
  if (upload)
    start_uploads(self);
  else
    start_publish(self);
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
present_consent(GnNip34IssueView *self, const gchar *heading, const gchar *body,
                const gchar *preview, const gchar *accept_id, const gchar *accept_label)
{
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
  self->reviewing = TRUE;
#if ADW_CHECK_VERSION(1, 5, 0)
  AdwDialog *alert = adw_alert_dialog_new(heading, body);
  AdwAlertDialog *a = ADW_ALERT_DIALOG(alert);
  adw_alert_dialog_set_extra_child(a, scroll);
  adw_alert_dialog_add_responses(a, "cancel", _("Cancel"), accept_id, accept_label, NULL);
  adw_alert_dialog_set_default_response(a, "cancel");
  adw_alert_dialog_set_close_response(a, "cancel");
  adw_alert_dialog_set_response_appearance(a, accept_id, ADW_RESPONSE_DESTRUCTIVE);
  g_signal_connect_object(alert, "response", G_CALLBACK(consent_response), self, 0);
  adw_dialog_present(alert, GTK_WIDGET(self));
#else
  /* libadwaita 1.2 floor: AdwMessageDialog (deprecated only from 1.6). */
  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
  GtkWidget *alert = adw_message_dialog_new(GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL,
                                            heading, body);
  AdwMessageDialog *a = ADW_MESSAGE_DIALOG(alert);
  adw_message_dialog_set_extra_child(a, scroll);
  adw_message_dialog_add_responses(a, "cancel", _("Cancel"), accept_id, accept_label, NULL);
  adw_message_dialog_set_default_response(a, "cancel");
  adw_message_dialog_set_close_response(a, "cancel");
  adw_message_dialog_set_response_appearance(a, accept_id, ADW_RESPONSE_DESTRUCTIVE);
  g_signal_connect_object(alert, "response", G_CALLBACK(consent_response), self, 0);
  gtk_window_present(GTK_WINDOW(alert));
#endif
}

static void
review(GnNip34IssueView *self)
{
  if (self->reviewing || self->submitted || self->closed || self->busy)
    return;
  if (!self->pubkey) {
    set_status(self, _("Select an account, then open Report an Issue again."));
    return;
  }
  clear_review(self);
  g_autoptr(GError) error = NULL;
  g_autofree gchar *diagnostics = NULL;
  g_autoptr(GnIssueDraft) draft = collect_draft(self, &diagnostics, &error);
  g_auto(GStrv) relays = draft ? collect_relays(self, &error) : NULL;
  if (!relays) {
    toast(self, error->message);
    return;
  }
  NostrEvent *event = gn_issue_draft_build_event(draft, self->target, self->pubkey);
  g_autofree gchar *json = event ? nostr_event_serialize_compact(event) : NULL;
  if (!json) {
    if (event)
      nostr_event_free(event);
    set_status(self, _("Could not build the issue."));
    return;
  }
  g_autofree gchar *tags = format_tags(event);
  nostr_event_free(event);
  g_autoptr(GPtrArray) items = pending_uploads(self);
  set_status(self, "");
  self->reviewed = g_steal_pointer(&draft);
  self->reviewed_json = g_steal_pointer(&json);
  self->reviewed_relays = g_steal_pointer(&relays);
  self->reviewed_target = gn_issue_target_copy(self->target);
  self->reviewed_pubkey = g_strdup(self->pubkey);
  self->reviewed_files = item_names(items);

  g_autofree gchar *address = gn_issue_target_get_address(self->target);
  g_autofree gchar *relay_list = g_strjoinv("\n", self->reviewed_relays);
  g_autofree gchar *preview = g_strdup_printf(
    _("Title:\n%s\n\nBody:\n%s\n\nTags:\n%s\nRelays:\n%s\n\nYour public key:\n%s\n\nRepository:\n%s\n\nThis kind-1621 event, timestamped now and signed by your account, will be published."),
    self->reviewed->title, self->reviewed->body, tags, relay_list, self->pubkey, address);
  gboolean with_diagnostics = diagnostics && *diagnostics;
  if (items->len) {
    g_autofree gchar *files = g_strjoinv("\n", self->reviewed_files);
    g_autofree gchar *body = g_strdup_printf(
      _("These files will be uploaded publicly to %s, where anyone can download them:\n%s\n\nThen you review the final issue, with their addresses, before it is published."),
      gn_issue_uploader_describe_destination(self->uploader), files);
    present_consent(self, _("Upload Files for This Issue?"), body, preview, "upload",
                    _("Upload Files"));
    return;
  }
  const gchar *uploads = self->uploads_done
    ? _("The uploaded files listed in the body are public.")
    : _("No files are uploaded.");
  g_autofree gchar *body = with_diagnostics
    ? g_strdup_printf(_("This is not a private message. Anyone can read and copy this report and associate it with your public key. Deletion cannot be guaranteed. The “%s” section shown in the body is included. %s"),
                      self->diag_heading ? self->diag_heading : _("Diagnostics"), uploads)
    : g_strdup_printf(_("This is not a private message. Anyone can read and copy this report and associate it with your public key. Deletion cannot be guaranteed. No logs or device details are added. %s"),
                      uploads);
  present_consent(self, _("Publish This Issue Publicly?"), body, preview, "publish",
                  _("Publish Public Issue"));
}

static void
review_clicked(GtkButton *button, GnNip34IssueView *self)
{
  (void)button;
  if (self->crash_failed && !self->busy) {
    /* "Send Without Logs": continue the queue without crash reports. */
    self->crash_failed = FALSE;
    self->skip_crash_logs = TRUE;
    gtk_button_set_label(self->review_button, _("Review Public Issue…"));
    upload_next(self);
    return;
  }
  review(self);
}

/* ---- Repository lookup ---- */

static void
resolved(GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GnNip34IssueView) self = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GnIssueTarget) target =
    gn_issue_repo_resolver_resolve_finish(GN_ISSUE_REPO_RESOLVER(source), result, &error);
  if (self->closed || g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;
  set_busy(self, FALSE, "");
  if (!target) {
    set_status(self, error ? error->message : _("No repository announcement was found; the built-in address is used."));
    return;
  }
  if (!gn_issue_target_equal(target, self->target)) {
    gn_issue_target_free(self->target);
    self->target = g_steal_pointer(&target);
    clear_review(self);
  }
  g_autofree gchar *status = g_strdup_printf(
    _("Repository found: %u maintainer(s), %u relay(s)."),
    g_strv_length(self->target->maintainers), g_strv_length(self->target->relays));
  set_status(self, status);
}

void
gn_nip34_issue_view_lookup_repository(GnNip34IssueView *self)
{
  g_return_if_fail(GN_IS_NIP34_ISSUE_VIEW(self));
  if (!self->resolver || self->busy || self->submitted || self->closed)
    return;
  set_busy(self, TRUE, _("Looking up the repository…"));
  gn_issue_repo_resolver_resolve_async(self->resolver, self->target, self->cancel, resolved,
                                       g_object_ref(self));
}

static void
lookup_clicked(GtkButton *button, GnNip34IssueView *self)
{
  (void)button;
  gn_nip34_issue_view_lookup_repository(self);
}

/* ---- Diagnostics ---- */

static void
diagnostics_toggled(GtkCheckButton *check, GnNip34IssueView *self)
{
  g_clear_pointer(&self->diag_text, g_free);
  gboolean on = gtk_check_button_get_active(check) && self->diag_func;
  if (on)
    self->diag_text = self->diag_func(self->diag_data);
  gboolean editable = (self->diag_flags & GN_ISSUE_DIAGNOSTICS_EDITABLE) != 0;
  if (editable) {
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(self->diag_editor),
                             self->diag_text ? self->diag_text : "", -1);
    gtk_widget_set_visible(self->diag_editor_frame, on);
    gtk_widget_set_visible(GTK_WIDGET(self->diag_preview), FALSE);
  } else {
    gtk_label_set_text(self->diag_preview, self->diag_text ? self->diag_text : "");
    gtk_widget_set_visible(GTK_WIDGET(self->diag_preview), self->diag_text != NULL);
    gtk_widget_set_visible(self->diag_editor_frame, FALSE);
  }
}

void
gn_nip34_issue_view_set_diagnostics(GnNip34IssueView *self, const gchar *heading,
                                    const gchar *check_label, const gchar *description,
                                    GnIssueDiagnosticsFlags flags, GnIssueDiagnosticsFunc func,
                                    gpointer user_data, GDestroyNotify destroy)
{
  g_return_if_fail(GN_IS_NIP34_ISSUE_VIEW(self));
  if (self->diag_destroy)
    self->diag_destroy(self->diag_data);
  self->diag_func = func;
  self->diag_data = user_data;
  self->diag_destroy = destroy;
  self->diag_flags = flags;
  g_free(self->diag_heading);
  self->diag_heading = g_strdup(heading);
  gtk_check_button_set_label(self->diag_check, check_label ? check_label : _("Include Diagnostics"));
  gtk_label_set_text(self->diag_description, description ? description : "");
  gtk_widget_set_visible(GTK_WIDGET(self->diag_description), description && *description);
  if (heading)
    gtk_accessible_update_property(GTK_ACCESSIBLE(self->diag_editor),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, heading, -1);
  gtk_widget_set_visible(self->diag_box, func != NULL);
  gboolean active = func && (flags & GN_ISSUE_DIAGNOSTICS_DEFAULT_ON);
  if (gtk_check_button_get_active(self->diag_check) != active)
    gtk_check_button_set_active(self->diag_check, active);
  else
    diagnostics_toggled(self->diag_check, self);
  clear_review(self);
}

/* ---- Files ---- */

static GtkWidget *
file_row(GFile *file, GtkWidget *leading, const gchar *detail, GtkWidget *trailing,
         GtkLabel **status_out)
{
  g_autofree gchar *name = g_file_get_basename(file);
  GtkWidget *row = gtk_list_box_row_new();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_set_margin_top(box, 8);
  gtk_widget_set_margin_bottom(box, 8);
  gtk_widget_set_margin_start(box, 10);
  gtk_widget_set_margin_end(box, 10);
  if (leading)
    gtk_box_append(GTK_BOX(box), leading);
  GtkWidget *labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand(labels, TRUE);
  GtkWidget *filename = gtk_label_new(name);
  gtk_label_set_xalign(GTK_LABEL(filename), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(filename), PANGO_ELLIPSIZE_MIDDLE);
  gtk_box_append(GTK_BOX(labels), filename);
  if (detail) {
    GtkWidget *details = gtk_label_new(detail);
    gtk_label_set_xalign(GTK_LABEL(details), 0.0f);
    gtk_widget_add_css_class(details, "dim-label");
    gtk_box_append(GTK_BOX(labels), details);
  }
  gtk_box_append(GTK_BOX(box), labels);
  GtkWidget *status = gtk_label_new("");
  gtk_widget_add_css_class(status, "dim-label");
  gtk_box_append(GTK_BOX(box), status);
  if (trailing)
    gtk_box_append(GTK_BOX(box), trailing);
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
  g_object_set_data_full(G_OBJECT(row), "gn-file", g_object_ref(file), g_object_unref);
  g_object_set_data(G_OBJECT(row), "gn-upload-status", status);
  *status_out = GTK_LABEL(status);
  return row;
}

static void
files_changed(GnNip34IssueView *self)
{
  clear_review(self);
}

void
gn_nip34_issue_view_set_crash_logs(GnNip34IssueView *self, GListModel *files)
{
  g_return_if_fail(GN_IS_NIP34_ISSUE_VIEW(self));
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->crash_list))))
    gtk_list_box_remove(self->crash_list, child);
  gtk_widget_set_visible(self->crash_group, self->uploader && files);
  if (!self->uploader || !files)
    return;
  guint n = g_list_model_get_n_items(files);
  if (!n) {
    GtkWidget *label = gtk_label_new(_("No recent crash reports were found."));
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_widget_set_margin_top(label, 8);
    gtk_widget_set_margin_bottom(label, 8);
    gtk_list_box_append(self->crash_list, label);
    return;
  }
  for (guint i = 0; i < n; i++) {
    g_autoptr(GFile) file = g_list_model_get_item(files, i);
    g_autofree gchar *name = g_file_get_basename(file);
    g_autoptr(GFileInfo) info = g_file_query_info(file, G_FILE_ATTRIBUTE_TIME_MODIFIED,
                                                  G_FILE_QUERY_INFO_NONE, NULL, NULL);
    g_autofree gchar *date = NULL;
    if (info) {
      g_autoptr(GDateTime) mtime = g_file_info_get_modification_date_time(info);
      g_autoptr(GDateTime) local = mtime ? g_date_time_to_local(mtime) : NULL;
      date = local ? g_date_time_format(local, "%b %e, %Y %H:%M") : NULL;
    }
    /* Nothing is selected by default. */
    GtkWidget *check = gtk_check_button_new();
    gtk_accessible_update_property(GTK_ACCESSIBLE(check), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
    g_signal_connect_swapped(check, "toggled", G_CALLBACK(files_changed), self);
    GtkLabel *status = NULL;
    GtkWidget *row = file_row(file, check, date, NULL, &status);
    g_object_set_data(G_OBJECT(row), "gn-crash-check", check);
    gtk_list_box_append(self->crash_list, row);
  }
}

static void
remove_attachment(GtkButton *button, GnNip34IssueView *self)
{
  GtkWidget *row = g_object_get_data(G_OBJECT(button), "gn-row");
  if (row)
    gtk_list_box_remove(self->attachment_list, row);
  files_changed(self);
}

gboolean
gn_nip34_issue_view_add_attachment(GnNip34IssueView *self, GFile *file)
{
  g_return_val_if_fail(GN_IS_NIP34_ISSUE_VIEW(self), FALSE);
  g_return_val_if_fail(G_IS_FILE(file), FALSE);
  if (!self->uploader || self->uploads_done || self->submitted)
    return FALSE;
  g_autoptr(GFileInfo) info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_SIZE,
                                                G_FILE_QUERY_INFO_NONE, NULL, NULL);
  goffset size = info ? g_file_info_get_size(info) : 0;
  if (!info || size > GN_ISSUE_ATTACHMENT_MAX_BYTES)
    return FALSE;
  g_autofree gchar *size_text = g_format_size((guint64)size);
  g_autofree gchar *name = g_file_get_basename(file);
  GtkWidget *remove = gtk_button_new_from_icon_name("user-trash-symbolic");
  gtk_widget_add_css_class(remove, "flat");
  gtk_widget_set_tooltip_text(remove, _("Remove attachment"));
  g_autofree gchar *remove_label = g_strdup_printf(_("Remove %s"), name);
  gtk_accessible_update_property(GTK_ACCESSIBLE(remove), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 remove_label, -1);
  GtkLabel *status = NULL;
  GtkWidget *row = file_row(file, NULL, size_text, remove, &status);
  gtk_label_set_text(status, _("Ready"));
  g_object_set_data(G_OBJECT(remove), "gn-row", row);
  g_signal_connect(remove, "clicked", G_CALLBACK(remove_attachment), self);
  gtk_list_box_append(self->attachment_list, row);
  files_changed(self);
  return TRUE;
}

G_GNUC_BEGIN_IGNORE_DEPRECATIONS
static void
files_chosen(GtkNativeDialog *dialog, gint response, gpointer data)
{
  g_autoptr(GnNip34IssueView) self = data;
  if (response == GTK_RESPONSE_ACCEPT && !self->closed) {
    g_autoptr(GListModel) files = gtk_file_chooser_get_files(GTK_FILE_CHOOSER(dialog));
    gboolean rejected = FALSE;
    for (guint i = 0; files && i < g_list_model_get_n_items(files); i++) {
      g_autoptr(GFile) file = g_list_model_get_item(files, i);
      if (!gn_nip34_issue_view_add_attachment(self, file))
        rejected = TRUE;
    }
    if (rejected)
      toast(self, _("Files larger than 25 MB were not added."));
  }
  gtk_native_dialog_destroy(dialog);
  g_object_unref(dialog);
}

static void
attach_clicked(GtkButton *button, GnNip34IssueView *self)
{
  (void)button;
  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
  /* GtkFileChooserNative: GtkFileDialog is GTK 4.10, above the 4.6 floor. */
  GtkFileChooserNative *chooser = gtk_file_chooser_native_new(
    _("Attach Files"), GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL,
    GTK_FILE_CHOOSER_ACTION_OPEN, _("_Attach"), _("_Cancel"));
  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(chooser), TRUE);
  gtk_native_dialog_set_modal(GTK_NATIVE_DIALOG(chooser), TRUE);
  g_signal_connect(chooser, "response", G_CALLBACK(files_chosen), g_object_ref(self));
  gtk_native_dialog_show(GTK_NATIVE_DIALOG(chooser));
}
G_GNUC_END_IGNORE_DEPRECATIONS

/* ---- Labels ---- */

static void
chip_clicked(GtkButton *button, GnNip34IssueView *self)
{
  gn_nip34_issue_fields_add_label(self->fields, gtk_button_get_label(button));
}

static GtkWidget *
create_label_suggestions(GnNip34IssueView *self)
{
  static const gchar *suggestions[] = {
    "bug", "feature", "enhancement", "question", "crash", "ui", "performance", NULL};
  GtkWidget *flow = gtk_flow_box_new();
  gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
  gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 7);
  gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(flow), 6);
  gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(flow), 6);
  gtk_accessible_update_property(GTK_ACCESSIBLE(flow), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 _("Suggested labels"), -1);
  for (guint i = 0; suggestions[i]; i++) {
    GtkWidget *button = gtk_button_new_with_label(suggestions[i]);
    gtk_widget_add_css_class(button, "pill");
    g_signal_connect(button, "clicked", G_CALLBACK(chip_clicked), self);
    gtk_flow_box_append(GTK_FLOW_BOX(flow), button);
  }
  return flow;
}

/* ---- Lifecycle ---- */

static void
mark_closed(GnNip34IssueView *self)
{
  self->closed = TRUE;
  stop(self);
}

static gboolean
still_unrooted(gpointer data)
{
  GnNip34IssueView *self = data;
  if (!gtk_widget_get_root(GTK_WIDGET(self)))
    stop(self);
  return G_SOURCE_REMOVE;
}

static void
unroot(GtkWidget *widget)
{
  /* A parent window can disappear without its dialog's closed signal:
   * cancel in-flight lookup, upload or signer work then. An adaptive dialog
   * that only moves the form (floating <-> bottom sheet) re-roots it at once,
   * so check after the current main-loop dispatch. */
  GTK_WIDGET_CLASS(gn_nip34_issue_view_parent_class)->unroot(widget);
  g_idle_add_full(G_PRIORITY_HIGH, still_unrooted, g_object_ref(widget), g_object_unref);
}

static void
dispose(GObject *object)
{
  GnNip34IssueView *self = GN_NIP34_ISSUE_VIEW(object);
  mark_closed(self);
  if (self->diag_destroy)
    self->diag_destroy(self->diag_data);
  self->diag_destroy = NULL;
  self->diag_func = NULL;
  g_clear_object(&self->cancel);
  g_clear_object(&self->publisher);
  g_clear_object(&self->resolver);
  g_clear_object(&self->uploader);
  clear_review(self);
  g_clear_pointer(&self->uploads, g_ptr_array_unref);
  G_OBJECT_CLASS(gn_nip34_issue_view_parent_class)->dispose(object);
}

static void
finalize(GObject *object)
{
  GnNip34IssueView *self = GN_NIP34_ISSUE_VIEW(object);
  gn_issue_target_free(self->target);
  g_free(self->pubkey);
  g_strfreev(self->required_labels);
  g_free(self->diag_heading);
  g_free(self->diag_text);
  G_OBJECT_CLASS(gn_nip34_issue_view_parent_class)->finalize(object);
}

static void
gn_nip34_issue_view_class_init(GnNip34IssueViewClass *klass)
{
  gn_portable_gettext_domain();
  G_OBJECT_CLASS(klass)->dispose = dispose;
  G_OBJECT_CLASS(klass)->finalize = finalize;
  GTK_WIDGET_CLASS(klass)->unroot = unroot;
  /* published(event_id): the issue was signed and accepted by the publisher. */
  signals[SIGNAL_PUBLISHED] = g_signal_new("published", G_TYPE_FROM_CLASS(klass),
                                           G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                           G_TYPE_NONE, 1, G_TYPE_STRING);
  /* toast(message): a notice the host may also show outside the form. */
  signals[SIGNAL_TOAST] = g_signal_new("toast", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
                                       0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
}

static GtkWidget *
heading_label(const gchar *text)
{
  GtkWidget *label = gtk_label_new(text);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_widget_add_css_class(label, "heading");
  return label;
}

static void
gn_nip34_issue_view_init(GnNip34IssueView *self)
{
  self->cancel = g_cancellable_new();
  GtkBox *box = GTK_BOX(self);
  gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
  gtk_box_set_spacing(box, 16);
  gtk_widget_set_margin_top(GTK_WIDGET(self), 16);
  gtk_widget_set_margin_bottom(GTK_WIDGET(self), 16);
  gtk_widget_set_margin_start(GTK_WIDGET(self), 16);
  gtk_widget_set_margin_end(GTK_WIDGET(self), 16);

  self->intro = GTK_LABEL(gtk_label_new(
    _("File a public NIP-34 issue. Describe the problem without including private messages or keys. Nothing is sent until you review and confirm.")));
  gtk_label_set_wrap(self->intro, TRUE);
  gtk_label_set_xalign(self->intro, 0);
  gtk_box_append(box, GTK_WIDGET(self->intro));

  GtkWidget *title_group = adw_preferences_group_new();
  self->title_row = adw_entry_row_new();
  gtk_widget_set_name(self->title_row, "title_row");
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->title_row), _("Title"));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(title_group), self->title_row);
  gtk_box_append(box, title_group);

  GtkWidget *description = heading_label(_("Description"));
  gtk_box_append(box, description);
  GtkWidget *frame = gtk_frame_new(NULL);
  self->body_view = GTK_TEXT_VIEW(gtk_text_view_new());
  gtk_widget_set_name(GTK_WIDGET(self->body_view), "body_view");
  gtk_widget_set_size_request(GTK_WIDGET(self->body_view), -1, 180);
  gtk_text_view_set_wrap_mode(self->body_view, GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_top_margin(self->body_view, 8);
  gtk_text_view_set_bottom_margin(self->body_view, 8);
  gtk_text_view_set_left_margin(self->body_view, 8);
  gtk_text_view_set_right_margin(self->body_view, 8);
  gtk_label_set_mnemonic_widget(GTK_LABEL(description), GTK_WIDGET(self->body_view));
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->body_view), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 _("Description"), -1);
  gtk_frame_set_child(GTK_FRAME(frame), GTK_WIDGET(self->body_view));
  gtk_box_append(box, frame);

  self->fields = gn_nip34_issue_fields_new();
  gtk_widget_set_name(GTK_WIDGET(self->fields), "issue_fields");
  gtk_box_append(box, GTK_WIDGET(self->fields));
  self->chips = create_label_suggestions(self);
  gtk_box_append(box, self->chips);

  /* Diagnostics: hidden until the host supplies them. */
  self->diag_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_visible(self->diag_box, FALSE);
  self->diag_check = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("Include Diagnostics")));
  gtk_widget_set_name(GTK_WIDGET(self->diag_check), "diagnostics_check");
  g_signal_connect(self->diag_check, "toggled", G_CALLBACK(diagnostics_toggled), self);
  gtk_box_append(GTK_BOX(self->diag_box), GTK_WIDGET(self->diag_check));
  self->diag_description = GTK_LABEL(gtk_label_new(NULL));
  gtk_label_set_wrap(self->diag_description, TRUE);
  gtk_label_set_xalign(self->diag_description, 0);
  gtk_widget_add_css_class(GTK_WIDGET(self->diag_description), "dim-label");
  gtk_box_append(GTK_BOX(self->diag_box), GTK_WIDGET(self->diag_description));
  self->diag_preview = GTK_LABEL(gtk_label_new(NULL));
  gtk_widget_set_name(GTK_WIDGET(self->diag_preview), "diagnostics_preview");
  gtk_widget_set_visible(GTK_WIDGET(self->diag_preview), FALSE);
  gtk_label_set_wrap(self->diag_preview, TRUE);
  gtk_label_set_wrap_mode(self->diag_preview, PANGO_WRAP_WORD_CHAR);
  gtk_label_set_xalign(self->diag_preview, 0);
  gtk_label_set_selectable(self->diag_preview, TRUE);
  gtk_widget_add_css_class(GTK_WIDGET(self->diag_preview), "monospace");
  gtk_box_append(GTK_BOX(self->diag_box), GTK_WIDGET(self->diag_preview));
  self->diag_editor_frame = gtk_frame_new(NULL);
  gtk_widget_set_visible(self->diag_editor_frame, FALSE);
  self->diag_editor = GTK_TEXT_VIEW(gtk_text_view_new());
  gtk_widget_set_name(GTK_WIDGET(self->diag_editor), "diagnostics_editor");
  gtk_widget_set_size_request(GTK_WIDGET(self->diag_editor), -1, 120);
  gtk_text_view_set_monospace(self->diag_editor, TRUE);
  gtk_text_view_set_wrap_mode(self->diag_editor, GTK_WRAP_WORD_CHAR);
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->diag_editor), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 _("Diagnostics"), -1);
  gtk_frame_set_child(GTK_FRAME(self->diag_editor_frame), GTK_WIDGET(self->diag_editor));
  gtk_box_append(GTK_BOX(self->diag_box), self->diag_editor_frame);
  gtk_box_append(box, self->diag_box);

  /* Files: only with an uploader. */
  self->crash_group = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(self->crash_group), _("Crash Logs"));
  adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(self->crash_group),
    _("Optional. Nothing is selected by default; selected reports are uploaded publicly after you confirm."));
  self->crash_list = GTK_LIST_BOX(gtk_list_box_new());
  gtk_widget_set_name(GTK_WIDGET(self->crash_list), "crash_list");
  gtk_list_box_set_selection_mode(self->crash_list, GTK_SELECTION_NONE);
  gtk_widget_add_css_class(GTK_WIDGET(self->crash_list), "boxed-list");
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(self->crash_group), GTK_WIDGET(self->crash_list));
  gtk_widget_set_visible(self->crash_group, FALSE);
  gtk_box_append(box, self->crash_group);

  self->attach_group = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(self->attach_group), _("Attachments"));
  adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(self->attach_group),
    _("Optional files are uploaded publicly after you confirm (25 MB maximum each)."));
  self->attach_button = GTK_BUTTON(gtk_button_new_with_label(_("Attach Files…")));
  gtk_widget_set_name(GTK_WIDGET(self->attach_button), "attach_button");
  gtk_widget_set_halign(GTK_WIDGET(self->attach_button), GTK_ALIGN_START);
  g_signal_connect(self->attach_button, "clicked", G_CALLBACK(attach_clicked), self);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(self->attach_group), GTK_WIDGET(self->attach_button));
  self->attachment_list = GTK_LIST_BOX(gtk_list_box_new());
  gtk_widget_set_name(GTK_WIDGET(self->attachment_list), "attachment_list");
  gtk_list_box_set_selection_mode(self->attachment_list, GTK_SELECTION_NONE);
  gtk_widget_add_css_class(GTK_WIDGET(self->attachment_list), "boxed-list");
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(self->attach_group), GTK_WIDGET(self->attachment_list));
  gtk_widget_set_visible(self->attach_group, FALSE);
  gtk_box_append(box, self->attach_group);

  GtkWidget *relay_group = adw_preferences_group_new();
  adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(relay_group),
    _("The repository's announced relays are added after a lookup. You can choose the relays for this public report."));
  self->relays_row = adw_entry_row_new();
  gtk_widget_set_name(self->relays_row, "relays_row");
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->relays_row), _("Publish to Relays"));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(relay_group), self->relays_row);
  gtk_box_append(box, relay_group);

  GtkWidget *status_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  self->spinner = GTK_SPINNER(gtk_spinner_new());
  gtk_widget_set_visible(GTK_WIDGET(self->spinner), FALSE);
  self->status = GTK_LABEL(gtk_label_new(NULL));
  gtk_widget_set_name(GTK_WIDGET(self->status), "status");
  gtk_label_set_wrap(self->status, TRUE);
  gtk_label_set_xalign(self->status, 0);
  gtk_label_set_selectable(self->status, TRUE);
  gtk_widget_set_hexpand(GTK_WIDGET(self->status), TRUE);
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->status), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 _("Status"), -1);
  gtk_box_append(GTK_BOX(status_box), GTK_WIDGET(self->spinner));
  gtk_box_append(GTK_BOX(status_box), GTK_WIDGET(self->status));
  gtk_box_append(box, status_box);

  GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(actions, GTK_ALIGN_END);
  self->lookup_button = GTK_BUTTON(gtk_button_new_with_label(_("Look Up Repository")));
  gtk_widget_set_name(GTK_WIDGET(self->lookup_button), "lookup_button");
  gtk_widget_set_visible(GTK_WIDGET(self->lookup_button), FALSE);
  g_signal_connect(self->lookup_button, "clicked", G_CALLBACK(lookup_clicked), self);
  self->review_button = GTK_BUTTON(gtk_button_new_with_label(_("Review Public Issue…")));
  gtk_widget_set_name(GTK_WIDGET(self->review_button), "review_button");
  gtk_widget_add_css_class(GTK_WIDGET(self->review_button), "suggested-action");
  g_signal_connect(self->review_button, "clicked", G_CALLBACK(review_clicked), self);
  gtk_box_append(GTK_BOX(actions), GTK_WIDGET(self->lookup_button));
  gtk_box_append(GTK_BOX(actions), GTK_WIDGET(self->review_button));
  gtk_box_append(box, actions);
}

GnNip34IssueView *
gn_nip34_issue_view_new(const GnIssueTarget *target, GnIssuePublisher *publisher,
                        GnIssueRepoResolver *resolver, GnIssueUploader *uploader)
{
  g_return_val_if_fail(target != NULL, NULL);
  g_return_val_if_fail(GN_IS_ISSUE_PUBLISHER(publisher), NULL);
  g_return_val_if_fail(resolver == NULL || GN_IS_ISSUE_REPO_RESOLVER(resolver), NULL);
  g_return_val_if_fail(uploader == NULL || GN_IS_ISSUE_UPLOADER(uploader), NULL);
  GnNip34IssueView *self = g_object_new(GN_TYPE_NIP34_ISSUE_VIEW, NULL);
  self->target = gn_issue_target_copy(target);
  self->publisher = g_object_ref(publisher);
  self->resolver = resolver ? g_object_ref(resolver) : NULL;
  self->uploader = uploader ? g_object_ref(uploader) : NULL;
  gtk_widget_set_visible(GTK_WIDGET(self->lookup_button), resolver != NULL);
  gtk_widget_set_visible(self->attach_group, uploader != NULL);
  return self;
}

void
gn_nip34_issue_view_set_pubkey(GnNip34IssueView *self, const gchar *pubkey_hex)
{
  g_return_if_fail(GN_IS_NIP34_ISSUE_VIEW(self));
  if (g_strcmp0(self->pubkey, pubkey_hex) == 0)
    return;
  g_free(self->pubkey);
  self->pubkey = pubkey_hex && *pubkey_hex ? g_strdup(pubkey_hex) : NULL;
  /* A review shows the author: it no longer matches. */
  clear_review(self);
}

const gchar *
gn_nip34_issue_view_get_pubkey(GnNip34IssueView *self)
{
  g_return_val_if_fail(GN_IS_NIP34_ISSUE_VIEW(self), NULL);
  return self->pubkey;
}

const GnIssueTarget *
gn_nip34_issue_view_get_target(GnNip34IssueView *self)
{
  g_return_val_if_fail(GN_IS_NIP34_ISSUE_VIEW(self), NULL);
  return self->target;
}

void
gn_nip34_issue_view_set_intro(GnNip34IssueView *self, const gchar *text)
{
  g_return_if_fail(GN_IS_NIP34_ISSUE_VIEW(self));
  gtk_label_set_text(self->intro, text ? text : "");
}

void
gn_nip34_issue_view_set_required_labels(GnNip34IssueView *self, const gchar *const *labels)
{
  g_return_if_fail(GN_IS_NIP34_ISSUE_VIEW(self));
  g_strfreev(self->required_labels);
  self->required_labels = labels ? g_strdupv((gchar **)labels) : NULL;
  clear_review(self);
}

void
gn_nip34_issue_view_set_relays(GnNip34IssueView *self, const gchar *const *relays)
{
  g_return_if_fail(GN_IS_NIP34_ISSUE_VIEW(self));
  g_autofree gchar *joined = relays ? g_strjoinv(" ", (gchar **)relays) : g_strdup("");
  gtk_editable_set_text(GTK_EDITABLE(self->relays_row), joined);
}

void
gn_nip34_issue_view_invalidate(GnNip34IssueView *self, const gchar *message)
{
  g_return_if_fail(GN_IS_NIP34_ISSUE_VIEW(self));
  stop(self);
  clear_review(self);
  self->submitted = TRUE;
  set_busy(self, FALSE, message);
}

static void
container_closed(GnNip34IssueView *self)
{
  mark_closed(self);
}

GtkWidget *
gn_nip34_issue_view_present(GnNip34IssueView *self, GtkWidget *parent)
{
  g_return_val_if_fail(GN_IS_NIP34_ISSUE_VIEW(self), NULL);
  g_return_val_if_fail(self->container == NULL, NULL);
  GtkWidget *scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(scroller, TRUE);
  GtkWidget *clamp = adw_clamp_new();
  adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 600);
  adw_clamp_set_child(ADW_CLAMP(clamp), GTK_WIDGET(self));
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), clamp);
#if ADW_CHECK_VERSION(1, 5, 0)
  AdwDialog *dialog = adw_dialog_new();
  adw_dialog_set_title(dialog, _("Report an Issue"));
  adw_dialog_set_content_width(dialog, 600);
  adw_dialog_set_content_height(dialog, 720);
  GtkWidget *toolbar = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), adw_header_bar_new());
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), scroller);
  adw_dialog_set_child(dialog, toolbar);
  g_signal_connect_object(dialog, "closed", G_CALLBACK(container_closed), self, G_CONNECT_SWAPPED);
  self->container = GTK_WIDGET(dialog);
  adw_dialog_present(dialog, parent);
#else
  GtkRoot *root = parent ? gtk_widget_get_root(parent) : NULL;
  GtkWidget *window = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(window), _("Report an Issue"));
  gtk_window_set_default_size(GTK_WINDOW(window), 600, 720);
  gtk_window_set_modal(GTK_WINDOW(window), TRUE);
  if (GTK_IS_WINDOW(root))
    gtk_window_set_transient_for(GTK_WINDOW(window), GTK_WINDOW(root));
  gtk_window_set_destroy_with_parent(GTK_WINDOW(window), TRUE);
  gtk_window_set_titlebar(GTK_WINDOW(window), adw_header_bar_new());
  gtk_window_set_child(GTK_WINDOW(window), scroller);
  g_signal_connect_object(window, "destroy", G_CALLBACK(container_closed), self, G_CONNECT_SWAPPED);
  self->container = window;
  gtk_window_present(GTK_WINDOW(window));
#endif
  return self->container;
}

void
gn_nip34_issue_view_close(GnNip34IssueView *self)
{
  g_return_if_fail(GN_IS_NIP34_ISSUE_VIEW(self));
  if (!self->container)
    return;
#if ADW_CHECK_VERSION(1, 5, 0)
  adw_dialog_close(ADW_DIALOG(self->container));
#else
  gtk_window_close(GTK_WINDOW(self->container));
#endif
}
