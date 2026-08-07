#define G_LOG_DOMAIN "gnostr-bug-report-dialog"

#include "gnostr-bug-report-dialog.h"
#include "gnostr-main-window-private.h"

#include "../util/blossom.h"
#include "../../../../nips/nip34/include/nip34.h"
#include "nostr-event.h"

#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <sys/stat.h>

#define BUG_REPORT_BLOSSOM_SERVER "https://blossom.sharegap.net"
#define BUG_REPORT_REPO_ID "nostrc"

/*
 * npub1ehhfg09mr8z34wz85ek46a6rww4f7c7jsujxhdvmpqnl5hnrwsqq2szjqv
 * decoded with NIP-19. Keeping the npub beside the build-time value makes the
 * target identity auditable without adding runtime decoding failure modes.
 */
#define BUG_REPORT_REPO_OWNER_HEX \
  "cdee943cbb19c51ab847a66d5d774373aa9f63d287246bb59b0827fa5e637400"

typedef struct {
  char *path;
  gint64 mtime;
} CrashLogEntry;

struct _GnostrBugReportDialog {
  AdwDialog parent_instance;

  GWeakRef main_window_ref;
  GtkEntry *subject_entry;
  GtkTextView *description_view;
  GtkListBox *crash_list;
  GtkButton *cancel_button;
  GtkButton *send_button;
  GtkSpinner *spinner;
  GtkLabel *status_label;

  GPtrArray *upload_paths;
  GPtrArray *uploaded_urls;
  guint upload_index;
  char *subject;
  char *description;
  gboolean send_without_logs;
  gboolean publishing;
  gboolean closing;
  GCancellable *cancellable;
};

G_DEFINE_FINAL_TYPE(GnostrBugReportDialog, gnostr_bug_report_dialog,
                    ADW_TYPE_DIALOG)

static void start_publish(GnostrBugReportDialog *self, gboolean include_logs);

static void
crash_log_entry_free(CrashLogEntry *entry)
{
  if (!entry)
    return;
  g_free(entry->path);
  g_free(entry);
}

static gint
compare_crash_logs_newest_first(gconstpointer a, gconstpointer b)
{
  const CrashLogEntry *ea = *(CrashLogEntry * const *)a;
  const CrashLogEntry *eb = *(CrashLogEntry * const *)b;
  if (ea->mtime > eb->mtime)
    return -1;
  if (ea->mtime < eb->mtime)
    return 1;
  return g_strcmp0(ea->path, eb->path);
}

static GPtrArray *
scan_crash_logs(const char *directory)
{
  GPtrArray *entries =
      g_ptr_array_new_with_free_func((GDestroyNotify)crash_log_entry_free);
  g_autoptr(GDir) dir = g_dir_open(directory, 0, NULL);
  if (!dir)
    return entries;

  const char *name = NULL;
  while ((name = g_dir_read_name(dir)) != NULL) {
    if (!g_str_has_prefix(name, "gnostr") || !g_str_has_suffix(name, ".ips"))
      continue;

    g_autofree char *path = g_build_filename(directory, name, NULL);
    GStatBuf st;
    if (g_stat(path, &st) != 0 || !S_ISREG(st.st_mode))
      continue;

    CrashLogEntry *entry = g_new0(CrashLogEntry, 1);
    entry->path = g_steal_pointer(&path);
    entry->mtime = (gint64)st.st_mtime;
    g_ptr_array_add(entries, entry);
  }

  g_ptr_array_sort(entries, compare_crash_logs_newest_first);
  return entries;
}

GPtrArray *
gnostr_bug_report_discover_crash_logs(const char *directory)
{
  g_autofree char *default_directory = NULL;

  if (!directory) {
#ifdef __APPLE__
    default_directory =
        g_build_filename(g_get_home_dir(), "Library", "Logs",
                         "DiagnosticReports", NULL);
    directory = default_directory;
#else
    return g_ptr_array_new_with_free_func(g_free);
#endif
  }

  g_autoptr(GPtrArray) entries = scan_crash_logs(directory);
  GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; i < entries->len; i++) {
    CrashLogEntry *entry = g_ptr_array_index(entries, i);
    g_ptr_array_add(paths, g_strdup(entry->path));
  }
  return paths;
}

static GnostrMainWindow *
get_main_window(GnostrBugReportDialog *self)
{
  GObject *object = g_weak_ref_get(&self->main_window_ref);
  if (!object)
    return NULL;
  if (!GNOSTR_IS_MAIN_WINDOW(object)) {
    g_object_unref(object);
    return NULL;
  }
  return GNOSTR_MAIN_WINDOW(object);
}

static void
show_main_toast(GnostrBugReportDialog *self, const char *message)
{
  GnostrMainWindow *window = get_main_window(self);
  if (!window)
    return;
  gnostr_main_window_show_toast_internal(window, message);
  g_object_unref(window);
}

static void
set_processing(GnostrBugReportDialog *self,
               gboolean processing,
               const char *status)
{
  self->publishing = processing;
  gtk_widget_set_sensitive(GTK_WIDGET(self->subject_entry), !processing);
  gtk_widget_set_sensitive(GTK_WIDGET(self->description_view), !processing);
  gtk_widget_set_sensitive(GTK_WIDGET(self->crash_list), !processing);
  gtk_widget_set_sensitive(GTK_WIDGET(self->send_button), !processing);
  gtk_spinner_set_spinning(self->spinner, processing);
  gtk_widget_set_visible(GTK_WIDGET(self->spinner), processing);
  gtk_widget_set_visible(GTK_WIDGET(self->status_label), processing);
  if (status)
    gtk_label_set_text(self->status_label, status);
}

static void
clear_submission(GnostrBugReportDialog *self)
{
  g_clear_pointer(&self->subject, g_free);
  g_clear_pointer(&self->description, g_free);
  g_clear_pointer(&self->upload_paths, g_ptr_array_unref);
  g_clear_pointer(&self->uploaded_urls, g_ptr_array_unref);
  self->upload_index = 0;
  self->send_without_logs = FALSE;
}

static void
on_publish_complete(gboolean success, gpointer user_data)
{
  GnostrBugReportDialog *self = GNOSTR_BUG_REPORT_DIALOG(user_data);
  if (self->closing) {
    clear_submission(self);
    return;
  }

  set_processing(self, FALSE, NULL);
  gtk_button_set_label(self->send_button, _("Send"));

  if (success) {
    show_main_toast(self, _("Bug report sent"));
    adw_dialog_close(ADW_DIALOG(self));
  } else {
    show_main_toast(self, _("Failed to send bug report"));
    clear_submission(self);
  }
}

static char *
build_issue_content(GnostrBugReportDialog *self, gboolean include_logs)
{
  GString *content = g_string_new(self->description ? self->description : "");
  if (include_logs && self->uploaded_urls && self->uploaded_urls->len > 0) {
    g_string_append(content, "\n\n## Crash logs:\n");
    for (guint i = 0; i < self->uploaded_urls->len; i++) {
      const char *url = g_ptr_array_index(self->uploaded_urls, i);
      g_string_append_printf(content, "\n- %s", url);
    }
  }
  return g_string_free(content, FALSE);
}

static void
start_publish(GnostrBugReportDialog *self, gboolean include_logs)
{
  g_autofree char *content = build_issue_content(self, include_logs);
  const char *labels[] = {"bug", NULL};
  NostrEvent *event =
      nip34_create_issue(BUG_REPORT_REPO_OWNER_HEX, BUG_REPORT_REPO_ID,
                         self->subject, content, labels);
  if (!event) {
    set_processing(self, FALSE, NULL);
    show_main_toast(self, _("Failed to build bug report"));
    clear_submission(self);
    return;
  }

  char *event_json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  if (!event_json) {
    set_processing(self, FALSE, NULL);
    show_main_toast(self, _("Failed to serialize bug report"));
    clear_submission(self);
    return;
  }

  GnostrMainWindow *window = get_main_window(self);
  if (!window) {
    g_free(event_json);
    set_processing(self, FALSE, NULL);
    clear_submission(self);
    return;
  }

  set_processing(self, TRUE, _("Signing and publishing…"));
  gnostr_main_window_publish_event_json_async_internal(
      window, event_json, self->cancellable, on_publish_complete,
      g_object_ref(self), g_object_unref);
  g_object_unref(window);
  g_free(event_json);
}

static void upload_next(GnostrBugReportDialog *self);

static void
on_crash_log_uploaded(GnostrBlossomBlob *blob,
                      GError *error,
                      gpointer user_data)
{
  GnostrBugReportDialog *self = GNOSTR_BUG_REPORT_DIALOG(user_data);

  if (self->closing) {
    if (blob)
      gnostr_blossom_blob_free(blob);
    g_object_unref(self);
    return;
  }

  if (blob && blob->url && *blob->url) {
    g_ptr_array_add(self->uploaded_urls, g_strdup(blob->url));
    gnostr_blossom_blob_free(blob);
    self->upload_index++;
    upload_next(self);
    g_object_unref(self);
    return;
  }

  if (blob)
    gnostr_blossom_blob_free(blob);

  g_warning("Crash log upload failed: %s",
            error && error->message ? error->message : "unknown error");
  g_ptr_array_set_size(self->uploaded_urls, 0);
  self->send_without_logs = TRUE;
  set_processing(self, FALSE, NULL);
  gtk_button_set_label(self->send_button, _("Send without logs"));
  show_main_toast(self,
                  _("Crash log upload failed. You can send without logs."));
  g_object_unref(self);
}

static void
upload_next(GnostrBugReportDialog *self)
{
  if (!self->upload_paths || self->upload_index >= self->upload_paths->len) {
    start_publish(self, TRUE);
    return;
  }

  g_autofree char *status =
      g_strdup_printf(_("Uploading crash log %u of %u…"),
                      self->upload_index + 1, self->upload_paths->len);
  set_processing(self, TRUE, status);

  const char *path = g_ptr_array_index(self->upload_paths, self->upload_index);
  gnostr_blossom_upload_async(BUG_REPORT_BLOSSOM_SERVER, path,
                              "application/json", on_crash_log_uploaded,
                              g_object_ref(self), self->cancellable);
}

static char *
get_description(GnostrBugReportDialog *self)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(self->description_view);
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  return gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
}

static void
collect_selected_logs(GnostrBugReportDialog *self)
{
  self->upload_paths = g_ptr_array_new_with_free_func(g_free);
  for (GtkWidget *row = gtk_widget_get_first_child(GTK_WIDGET(self->crash_list));
       row != NULL; row = gtk_widget_get_next_sibling(row)) {
    GtkCheckButton *check =
        GTK_CHECK_BUTTON(g_object_get_data(G_OBJECT(row), "crash-check"));
    const char *path = g_object_get_data(G_OBJECT(row), "crash-path");
    if (check && path && gtk_check_button_get_active(check))
      g_ptr_array_add(self->upload_paths, g_strdup(path));
  }
}

static void
on_send_clicked(GtkButton *button, gpointer user_data)
{
  (void)button;
  GnostrBugReportDialog *self = GNOSTR_BUG_REPORT_DIALOG(user_data);
  if (self->publishing)
    return;

  if (self->send_without_logs) {
    self->send_without_logs = FALSE;
    gtk_button_set_label(self->send_button, _("Send"));
    set_processing(self, TRUE, _("Preparing bug report…"));
    start_publish(self, FALSE);
    return;
  }

  clear_submission(self);
  self->subject =
      g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->subject_entry)));
  self->description = get_description(self);
  g_strstrip(self->subject);
  g_strstrip(self->description);

  if (!*self->subject) {
    show_main_toast(self, _("Enter a subject for the bug report"));
    gtk_widget_grab_focus(GTK_WIDGET(self->subject_entry));
    return;
  }
  if (!*self->description) {
    show_main_toast(self, _("Describe the problem before sending"));
    gtk_widget_grab_focus(GTK_WIDGET(self->description_view));
    return;
  }

  collect_selected_logs(self);
  self->uploaded_urls = g_ptr_array_new_with_free_func(g_free);
  self->upload_index = 0;
  set_processing(self, TRUE, _("Preparing bug report…"));

  if (self->upload_paths->len > 0)
    upload_next(self);
  else
    start_publish(self, FALSE);
}

static void
on_dialog_closed(AdwDialog *dialog, gpointer user_data)
{
  (void)dialog;
  GnostrBugReportDialog *self = GNOSTR_BUG_REPORT_DIALOG(user_data);
  self->closing = TRUE;
  if (self->cancellable)
    g_cancellable_cancel(self->cancellable);
}

static void
on_cancel_clicked(GtkButton *button, gpointer user_data)
{
  (void)button;
  GnostrBugReportDialog *self = GNOSTR_BUG_REPORT_DIALOG(user_data);
  adw_dialog_close(ADW_DIALOG(self));
}

static GtkWidget *
make_field_label(const char *text)
{
  GtkWidget *label = gtk_label_new(text);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_widget_add_css_class(label, "heading");
  return label;
}

static void
populate_crash_logs(GnostrBugReportDialog *self)
{
  g_autoptr(GPtrArray) paths = gnostr_bug_report_discover_crash_logs(NULL);
  if (paths->len == 0) {
    GtkWidget *label =
        gtk_label_new(_("No recent gnostr crash reports were found."));
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_widget_set_margin_top(label, 8);
    gtk_widget_set_margin_bottom(label, 8);
    gtk_list_box_append(self->crash_list, label);
    return;
  }

  for (guint i = 0; i < paths->len; i++) {
    const char *path = g_ptr_array_index(paths, i);
    g_autofree char *name = g_path_get_basename(path);

    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(box, 8);
    gtk_widget_set_margin_bottom(box, 8);
    gtk_widget_set_margin_start(box, 10);
    gtk_widget_set_margin_end(box, 10);

    GtkWidget *check = gtk_check_button_new();
    gtk_check_button_set_active(GTK_CHECK_BUTTON(check), FALSE);
    gtk_box_append(GTK_BOX(box), check);

    GtkWidget *labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(labels, TRUE);
    GtkWidget *filename = gtk_label_new(name);
    gtk_label_set_xalign(GTK_LABEL(filename), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(filename), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_append(GTK_BOX(labels), filename);

    GStatBuf st;
    if (g_stat(path, &st) == 0) {
      g_autoptr(GDateTime) date =
          g_date_time_new_from_unix_local((gint64)st.st_mtime);
      g_autofree char *formatted =
          date ? g_date_time_format(date, "%b %e, %Y %H:%M") : NULL;
      if (formatted) {
        GtkWidget *date_label = gtk_label_new(formatted);
        gtk_label_set_xalign(GTK_LABEL(date_label), 0.0f);
        gtk_widget_add_css_class(date_label, "dim-label");
        gtk_box_append(GTK_BOX(labels), date_label);
      }
    }

    gtk_box_append(GTK_BOX(box), labels);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    g_object_set_data(G_OBJECT(row), "crash-check", check);
    g_object_set_data_full(G_OBJECT(row), "crash-path", g_strdup(path), g_free);
    gtk_list_box_append(self->crash_list, row);
  }
}

static void
gnostr_bug_report_dialog_dispose(GObject *object)
{
  GnostrBugReportDialog *self = GNOSTR_BUG_REPORT_DIALOG(object);
  if (self->cancellable)
    g_cancellable_cancel(self->cancellable);
  g_clear_object(&self->cancellable);
  clear_submission(self);
  g_weak_ref_clear(&self->main_window_ref);
  G_OBJECT_CLASS(gnostr_bug_report_dialog_parent_class)->dispose(object);
}

static void
gnostr_bug_report_dialog_class_init(GnostrBugReportDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = gnostr_bug_report_dialog_dispose;
}

static void
gnostr_bug_report_dialog_init(GnostrBugReportDialog *self)
{
  g_weak_ref_init(&self->main_window_ref, NULL);
  self->cancellable = g_cancellable_new();
  g_signal_connect(self, "closed", G_CALLBACK(on_dialog_closed), self);

  adw_dialog_set_title(ADW_DIALOG(self), _("Send Bug Report"));
  adw_dialog_set_content_width(ADW_DIALOG(self), 560);
  adw_dialog_set_content_height(ADW_DIALOG(self), 640);

  GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *header = adw_header_bar_new();
  adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), TRUE);
  gtk_box_append(GTK_BOX(root), header);

  GtkWidget *scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(scroller, TRUE);

  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_top(content, 18);
  gtk_widget_set_margin_bottom(content, 18);
  gtk_widget_set_margin_start(content, 18);
  gtk_widget_set_margin_end(content, 18);

  gtk_box_append(GTK_BOX(content), make_field_label(_("Subject")));
  self->subject_entry = GTK_ENTRY(gtk_entry_new());
  gtk_entry_set_placeholder_text(self->subject_entry,
                                 _("Briefly summarize the problem"));
  gtk_entry_set_max_length(self->subject_entry, 160);
  gtk_box_append(GTK_BOX(content), GTK_WIDGET(self->subject_entry));

  gtk_box_append(GTK_BOX(content), make_field_label(_("Description")));
  GtkWidget *description_frame = gtk_frame_new(NULL);
  gtk_widget_set_size_request(description_frame, -1, 180);
  self->description_view = GTK_TEXT_VIEW(gtk_text_view_new());
  gtk_text_view_set_wrap_mode(self->description_view, GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_top_margin(self->description_view, 8);
  gtk_text_view_set_bottom_margin(self->description_view, 8);
  gtk_text_view_set_left_margin(self->description_view, 8);
  gtk_text_view_set_right_margin(self->description_view, 8);
  gtk_frame_set_child(GTK_FRAME(description_frame),
                      GTK_WIDGET(self->description_view));
  gtk_box_append(GTK_BOX(content), description_frame);

  GtkWidget *crash_group = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(crash_group),
                                  _("Crash logs"));
  adw_preferences_group_set_description(
      ADW_PREFERENCES_GROUP(crash_group),
      _("Optional. Nothing is selected by default; selected reports are "
        "uploaded publicly to Blossom."));
  self->crash_list = GTK_LIST_BOX(gtk_list_box_new());
  gtk_list_box_set_selection_mode(self->crash_list, GTK_SELECTION_NONE);
  gtk_widget_add_css_class(GTK_WIDGET(self->crash_list), "boxed-list");
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(crash_group),
                            GTK_WIDGET(self->crash_list));
  gtk_box_append(GTK_BOX(content), crash_group);
  populate_crash_logs(self);

  GtkWidget *status_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(status_box, GTK_ALIGN_CENTER);
  self->spinner = GTK_SPINNER(gtk_spinner_new());
  self->status_label = GTK_LABEL(gtk_label_new(NULL));
  gtk_box_append(GTK_BOX(status_box), GTK_WIDGET(self->spinner));
  gtk_box_append(GTK_BOX(status_box), GTK_WIDGET(self->status_label));
  gtk_widget_set_visible(GTK_WIDGET(self->spinner), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(self->status_label), FALSE);
  gtk_box_append(GTK_BOX(content), status_box);

  GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(actions, GTK_ALIGN_END);
  self->cancel_button = GTK_BUTTON(gtk_button_new_with_label(_("Cancel")));
  self->send_button = GTK_BUTTON(gtk_button_new_with_label(_("Send")));
  gtk_widget_add_css_class(GTK_WIDGET(self->send_button), "suggested-action");
  g_signal_connect(self->cancel_button, "clicked",
                   G_CALLBACK(on_cancel_clicked), self);
  g_signal_connect(self->send_button, "clicked",
                   G_CALLBACK(on_send_clicked), self);
  gtk_box_append(GTK_BOX(actions), GTK_WIDGET(self->cancel_button));
  gtk_box_append(GTK_BOX(actions), GTK_WIDGET(self->send_button));
  gtk_box_append(GTK_BOX(content), actions);

  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), content);
  gtk_box_append(GTK_BOX(root), scroller);
  adw_dialog_set_child(ADW_DIALOG(self), root);
}

GnostrBugReportDialog *
gnostr_bug_report_dialog_new(GnostrMainWindow *parent)
{
  g_return_val_if_fail(GNOSTR_IS_MAIN_WINDOW(parent), NULL);
  GnostrBugReportDialog *self =
      g_object_new(GNOSTR_TYPE_BUG_REPORT_DIALOG, NULL);
  g_weak_ref_set(&self->main_window_ref, G_OBJECT(parent));
  return self;
}

void
gnostr_bug_report_dialog_present(GnostrBugReportDialog *self,
                                 GtkWidget *parent)
{
  g_return_if_fail(GNOSTR_IS_BUG_REPORT_DIALOG(self));
  g_return_if_fail(GTK_IS_WIDGET(parent));
  adw_dialog_present(ADW_DIALOG(self), parent);
}

void
gnostr_main_window_on_bug_report_requested_internal(
    GnostrSessionView *session_view,
    gpointer user_data)
{
  (void)session_view;
  GnostrMainWindow *window = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(window))
    return;

  GnostrBugReportDialog *dialog = gnostr_bug_report_dialog_new(window);
  if (!dialog)
    return;
  gnostr_bug_report_dialog_present(dialog, GTK_WIDGET(window));
}
