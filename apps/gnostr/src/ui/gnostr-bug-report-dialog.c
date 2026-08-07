#define G_LOG_DOMAIN "gnostr-bug-report-dialog"

#include "gnostr-bug-report-dialog.h"
#include "gnostr-bug-report-assembly.h"
#include "gnostr-main-window-private.h"

#include "../util/blossom.h"
#include "../util/utils.h"
#include "gnostr-build-info.h"
#include "../../../../nips/nip34/include/nip34.h"
#include "nostr-event.h"
#include "nostr-filter.h"

#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <nostr-gobject-1.0/gnostr-relays.h>
#include <nostr-gobject-1.0/nostr_pool.h>
#include <nostr-gobject-1.0/storage_ndb.h>
#include <sys/stat.h>

#define BUG_REPORT_BLOSSOM_SERVER "https://blossom.sharegap.net"
#define BUG_REPORT_REPO_ID "nostrc"
#define MAX_ATTACHMENT_BYTES (25 * 1024 * 1024)

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

typedef struct {
  GPtrArray *maintainers; /* char* */
  GPtrArray *relays;      /* char* */
  gint64 created_at;
} RepoMetadata;

typedef enum {
  UPLOAD_CRASH_LOG,
  UPLOAD_ATTACHMENT,
} UploadKind;

typedef struct {
  char *path;
  UploadKind kind;
  GtkLabel *status_label; /* owned by the dialog widget tree */
} UploadItem;

struct _GnostrBugReportDialog {
  AdwDialog parent_instance;

  GWeakRef main_window_ref;
  GtkEntry *subject_entry;
  GtkEntry *labels_entry;
  GtkEntry *related_commits_entry;
  GtkTextView *description_view;
  GtkTextView *system_info_view;
  GtkListBox *crash_list;
  GtkListBox *attachment_list;
  GtkButton *attach_button;
  GtkButton *cancel_button;
  GtkButton *send_button;
  GtkSpinner *spinner;
  GtkLabel *status_label;

  GPtrArray *repo_maintainers; /* char* */
  GPtrArray *repo_relays;      /* char* */
  GPtrArray *upload_items;     /* UploadItem* */
  GPtrArray *uploaded_crash_urls;
  GPtrArray *uploaded_attachment_urls;
  guint upload_index;
  guint attachment_failures;

  char *subject;
  char *description;
  char *labels;
  char *related_commits;
  char *system_info;

  gboolean send_without_logs;
  gboolean skip_crash_logs;
  gboolean publishing;
  gboolean closing;
  GCancellable *cancellable;
};

G_DEFINE_FINAL_TYPE(GnostrBugReportDialog, gnostr_bug_report_dialog,
                    ADW_TYPE_DIALOG)

static void start_publish(GnostrBugReportDialog *self, gboolean include_logs);
static void upload_next(GnostrBugReportDialog *self);

static void
crash_log_entry_free(CrashLogEntry *entry)
{
  if (!entry)
    return;
  g_free(entry->path);
  g_free(entry);
}

static void
repo_metadata_free(RepoMetadata *metadata)
{
  if (!metadata)
    return;
  g_clear_pointer(&metadata->maintainers, g_ptr_array_unref);
  g_clear_pointer(&metadata->relays, g_ptr_array_unref);
  g_free(metadata);
}

static void
upload_item_free(UploadItem *item)
{
  if (!item)
    return;
  g_free(item->path);
  g_free(item);
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

static char *
get_text_view_text(GtkTextView *view)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(view);
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  return gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
}

static const char *
hardware_arch(void)
{
#if defined(__aarch64__) || defined(_M_ARM64)
  return "aarch64";
#elif defined(__x86_64__) || defined(_M_X64)
  return "x86_64";
#elif defined(__i386__) || defined(_M_IX86)
  return "x86";
#elif defined(__arm__) || defined(_M_ARM)
  return "arm";
#elif defined(__riscv)
  return "riscv";
#else
  return "unknown";
#endif
}

static char *
collect_system_info(void)
{
  g_autofree char *os_name = g_get_os_info("NAME");
  g_autofree char *os_version = g_get_os_info("VERSION_ID");

  return g_strdup_printf(
      "App: gnostr %s\n"
      "Build commit: %s\n"
      "OS: %s%s%s\n"
      "Architecture: %s\n"
      "GTK runtime: %u.%u.%u\n"
      "libadwaita runtime: %u.%u.%u",
      GNOSTR_BUILD_VERSION, GNOSTR_BUILD_COMMIT,
      os_name ? os_name : "unknown",
      os_version && *os_version ? " " : "",
      os_version && *os_version ? os_version : "",
      hardware_arch(),
      gtk_get_major_version(), gtk_get_minor_version(), gtk_get_micro_version(),
      adw_get_major_version(), adw_get_minor_version(), adw_get_micro_version());
}

static void
set_processing(GnostrBugReportDialog *self,
               gboolean processing,
               const char *status)
{
  self->publishing = processing;
  gtk_widget_set_sensitive(GTK_WIDGET(self->subject_entry), !processing);
  gtk_widget_set_sensitive(GTK_WIDGET(self->labels_entry), !processing);
  gtk_widget_set_sensitive(GTK_WIDGET(self->related_commits_entry), !processing);
  gtk_widget_set_sensitive(GTK_WIDGET(self->description_view), !processing);
  gtk_widget_set_sensitive(GTK_WIDGET(self->system_info_view), !processing);
  gtk_widget_set_sensitive(GTK_WIDGET(self->crash_list), !processing);
  gtk_widget_set_sensitive(GTK_WIDGET(self->attachment_list), !processing);
  gtk_widget_set_sensitive(GTK_WIDGET(self->attach_button), !processing);
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
  g_clear_pointer(&self->labels, g_free);
  g_clear_pointer(&self->related_commits, g_free);
  g_clear_pointer(&self->system_info, g_free);
  g_clear_pointer(&self->upload_items, g_ptr_array_unref);
  g_clear_pointer(&self->uploaded_crash_urls, g_ptr_array_unref);
  g_clear_pointer(&self->uploaded_attachment_urls, g_ptr_array_unref);
  self->upload_index = 0;
  self->attachment_failures = 0;
  self->send_without_logs = FALSE;
  self->skip_crash_logs = FALSE;
}

static GStrv
ptr_array_to_strv(const GPtrArray *array)
{
  if (!array || array->len == 0)
    return g_new0(char *, 1);

  GStrv values = g_new0(char *, array->len + 1);
  for (guint i = 0; i < array->len; i++)
    values[i] = g_strdup(g_ptr_array_index((GPtrArray *)array, i));
  return values;
}

static RepoMetadata *
repo_metadata_from_json(const char *event_json)
{
  if (!event_json || !*event_json)
    return NULL;

  NostrEvent *event = nostr_event_new();
  if (!event || nostr_event_deserialize_compact(event, event_json, NULL) != 1) {
    if (event)
      nostr_event_free(event);
    return NULL;
  }

  const char *author = nostr_event_get_pubkey(event);
  if (nostr_event_get_kind(event) != NIP34_KIND_REPOSITORY ||
      !author || g_ascii_strcasecmp(author, BUG_REPORT_REPO_OWNER_HEX) != 0) {
    nostr_event_free(event);
    return NULL;
  }

  nip34_repository_t *repo = NULL;
  if (nip34_parse_repository(event, &repo) != NIP34_OK ||
      !repo || g_strcmp0(repo->id, BUG_REPORT_REPO_ID) != 0) {
    nip34_repository_free(repo);
    nostr_event_free(event);
    return NULL;
  }

  RepoMetadata *metadata = g_new0(RepoMetadata, 1);
  metadata->maintainers = g_ptr_array_new_with_free_func(g_free);
  metadata->relays = g_ptr_array_new_with_free_func(g_free);
  metadata->created_at = nostr_event_get_created_at(event);

  for (size_t i = 0; i < repo->maintainer_count; i++) {
    if (repo->maintainers[i] && *repo->maintainers[i])
      g_ptr_array_add(metadata->maintainers, g_strdup(repo->maintainers[i]));
  }
  for (size_t i = 0; i < repo->relay_count; i++) {
    if (repo->relays[i] && *repo->relays[i])
      g_ptr_array_add(metadata->relays, g_strdup(repo->relays[i]));
  }

  nip34_repository_free(repo);
  nostr_event_free(event);
  return metadata;
}

static RepoMetadata *
newest_repo_metadata_from_results(char **results, int count)
{
  RepoMetadata *newest = NULL;
  for (int i = 0; results && i < count; i++) {
    RepoMetadata *candidate = repo_metadata_from_json(results[i]);
    if (!candidate)
      continue;
    if (!newest || candidate->created_at > newest->created_at) {
      repo_metadata_free(newest);
      newest = candidate;
    } else {
      repo_metadata_free(candidate);
    }
  }
  return newest;
}

static RepoMetadata *
newest_repo_metadata_from_ptr_array(GPtrArray *results)
{
  RepoMetadata *newest = NULL;
  for (guint i = 0; results && i < results->len; i++) {
    RepoMetadata *candidate =
        repo_metadata_from_json(g_ptr_array_index(results, i));
    if (!candidate)
      continue;
    if (!newest || candidate->created_at > newest->created_at) {
      repo_metadata_free(newest);
      newest = candidate;
    } else {
      repo_metadata_free(candidate);
    }
  }
  return newest;
}

static void
apply_repo_metadata(GnostrBugReportDialog *self, RepoMetadata *metadata)
{
  if (!metadata)
    return;
  g_clear_pointer(&self->repo_maintainers, g_ptr_array_unref);
  g_clear_pointer(&self->repo_relays, g_ptr_array_unref);
  self->repo_maintainers = g_steal_pointer(&metadata->maintainers);
  self->repo_relays = g_steal_pointer(&metadata->relays);
  g_debug("Issue dialog loaded repository announcement: %u maintainer(s), "
          "%u relay(s)",
          self->repo_maintainers->len, self->repo_relays->len);
  repo_metadata_free(metadata);
}

static void
repo_ndb_query_thread(GTask *task,
                      gpointer source_object,
                      gpointer task_data,
                      GCancellable *cancellable)
{
  (void)source_object;
  (void)task_data;

  if (g_task_return_error_if_cancelled(task))
    return;

  void *txn = NULL;
  if (storage_ndb_begin_query_retry(&txn, 3, 10, NULL) != 0 || !txn) {
    g_task_return_pointer(task, NULL, NULL);
    return;
  }

  const char *filter =
      "{\"kinds\":[30617],"
      "\"authors\":[\"" BUG_REPORT_REPO_OWNER_HEX "\"],"
      "\"#d\":[\"" BUG_REPORT_REPO_ID "\"],\"limit\":5}";
  char **results = NULL;
  int count = 0;
  RepoMetadata *metadata = NULL;
  if (storage_ndb_query(txn, filter, &results, &count, NULL) == 0)
    metadata = newest_repo_metadata_from_results(results, count);
  if (results)
    storage_ndb_free_results(results, count);
  storage_ndb_end_query(txn);

  g_task_return_pointer(task, metadata, (GDestroyNotify)repo_metadata_free);
}

static void
on_repo_relay_query_done(GObject *source,
                         GAsyncResult *result,
                         gpointer user_data)
{
  GnostrBugReportDialog *self = GNOSTR_BUG_REPORT_DIALOG(user_data);
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) results =
      gnostr_pool_query_finish(GNOSTR_POOL(source), result, &error);

  if (!self->closing && !error && results) {
    RepoMetadata *metadata = newest_repo_metadata_from_ptr_array(results);
    if (metadata) {
      apply_repo_metadata(self, metadata);
      GPtrArray *to_ingest = g_ptr_array_new_with_free_func(g_free);
      for (guint i = 0; i < results->len; i++)
        g_ptr_array_add(to_ingest,
                        g_strdup(g_ptr_array_index(results, i)));
      storage_ndb_ingest_events_async(to_ingest);
    }
  } else if (error && !g_error_matches(error, G_IO_ERROR,
                                        G_IO_ERROR_CANCELLED)) {
    g_debug("Repository announcement relay query failed: %s", error->message);
  }

  g_object_unref(self);
}

static void
fetch_repo_from_relays(GnostrBugReportDialog *self)
{
  g_autoptr(GPtrArray) relay_urls = gnostr_get_read_relay_urls();
  GNostrPool *pool = gnostr_get_shared_query_pool();
  if (!pool || !relay_urls || relay_urls->len == 0) {
    g_debug("Repository announcement unavailable; using owner/write-relay fallback");
    return;
  }

  const char **urls = g_new0(const char *, relay_urls->len);
  for (guint i = 0; i < relay_urls->len; i++)
    urls[i] = g_ptr_array_index(relay_urls, i);

  NostrFilter *filter = nostr_filter_new();
  int kinds[] = {NIP34_KIND_REPOSITORY};
  const char *authors[] = {BUG_REPORT_REPO_OWNER_HEX};
  nostr_filter_set_kinds(filter, kinds, 1);
  nostr_filter_set_authors(filter, authors, 1);
  nostr_filter_tags_append(filter, "d", BUG_REPORT_REPO_ID, NULL);
  nostr_filter_set_limit(filter, 5);

  NostrFilters *filters = nostr_filters_new();
  nostr_filters_add(filters, filter);
  nostr_filter_free(filter);

  gnostr_pool_query_urls_async(pool, (const gchar **)urls, relay_urls->len,
                               filters, self->cancellable,
                               on_repo_relay_query_done, g_object_ref(self));
  g_free(urls);
}

static void
on_repo_ndb_query_done(GObject *source,
                       GAsyncResult *result,
                       gpointer user_data)
{
  (void)user_data;
  GnostrBugReportDialog *self = GNOSTR_BUG_REPORT_DIALOG(source);
  g_autoptr(GError) error = NULL;
  RepoMetadata *metadata = g_task_propagate_pointer(G_TASK(result), &error);

  if (self->closing) {
    repo_metadata_free(metadata);
    return;
  }

  if (metadata) {
    apply_repo_metadata(self, metadata);
  } else if (!error ||
             !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    fetch_repo_from_relays(self);
  }
}

static void
fetch_repository_metadata(GnostrBugReportDialog *self)
{
  GTask *task = g_task_new(self, self->cancellable,
                           on_repo_ndb_query_done, NULL);
  g_task_run_in_thread(task, repo_ndb_query_thread);
  g_object_unref(task);
}

static void
finish_issue_success(GnostrBugReportDialog *self)
{
  if (self->closing) {
    clear_submission(self);
    return;
  }

  set_processing(self, FALSE, NULL);
  gtk_button_set_label(self->send_button, _("Send"));
  show_main_toast(self, _("Issue sent"));
  adw_dialog_close(ADW_DIALOG(self));
}

static void
publish_open_status(GnostrBugReportDialog *self, const char *issue_id)
{
  g_autoptr(GPtrArray) participants =
      g_ptr_array_new_with_free_func(g_free);
  g_autoptr(GHashTable) seen =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  if (g_hash_table_add(seen, g_strdup(BUG_REPORT_REPO_OWNER_HEX)))
    g_ptr_array_add(participants, g_strdup(BUG_REPORT_REPO_OWNER_HEX));
  for (guint i = 0; self->repo_maintainers &&
                        i < self->repo_maintainers->len; i++) {
    const char *pubkey = g_ptr_array_index(self->repo_maintainers, i);
    if (pubkey && *pubkey && g_hash_table_add(seen, g_strdup(pubkey)))
      g_ptr_array_add(participants, g_strdup(pubkey));
  }

  GnostrMainWindow *window = get_main_window(self);
  if (!window) {
    g_warning("Issue sent, but no window remained for the open-status publish");
    finish_issue_success(self);
    return;
  }
  if (window->user_pubkey_hex && *window->user_pubkey_hex &&
      g_hash_table_add(seen, g_strdup(window->user_pubkey_hex)))
    g_ptr_array_add(participants, g_strdup(window->user_pubkey_hex));
  g_ptr_array_add(participants, NULL);

  g_autofree char *repo_addr =
      g_strdup_printf("30617:%s:%s", BUG_REPORT_REPO_OWNER_HEX,
                      BUG_REPORT_REPO_ID);
  NostrEvent *status =
      nip34_create_status(issue_id, NIP34_STATUS_OPEN, "", repo_addr,
                          (const char *const *)participants->pdata);
  if (!status) {
    g_warning("Issue sent, but the open-status event could not be built");
    g_object_unref(window);
    finish_issue_success(self);
    return;
  }

  g_autofree char *event_json = nostr_event_serialize_compact(status);
  nostr_event_free(status);
  if (!event_json) {
    g_warning("Issue sent, but the open-status event could not be serialized");
    g_object_unref(window);
    finish_issue_success(self);
    return;
  }

  /* Best effort: the issue is already accepted. Copying additional relays in
   * the publisher lets this continue after the dialog closes and cancels its
   * own in-flight UI operations. */
  gnostr_main_window_publish_event_json_to_relays_async_internal(
      window, event_json, self->repo_relays, NULL, NULL, NULL, NULL);
  g_object_unref(window);
  finish_issue_success(self);
}

static void
on_issue_publish_complete(gboolean success,
                          const char *event_id,
                          gpointer user_data)
{
  GnostrBugReportDialog *self = GNOSTR_BUG_REPORT_DIALOG(user_data);
  if (self->closing) {
    clear_submission(self);
    return;
  }

  if (!success || !event_id || !*event_id) {
    set_processing(self, FALSE, NULL);
    gtk_button_set_label(self->send_button, _("Send"));
    show_main_toast(self, _("Failed to send issue"));
    clear_submission(self);
    return;
  }

  publish_open_status(self, event_id);
}

static void
start_publish(GnostrBugReportDialog *self, gboolean include_logs)
{
  g_auto(GStrv) maintainers = ptr_array_to_strv(self->repo_maintainers);
  NostrEvent *event = gnostr_bug_report_build_issue_event(
      BUG_REPORT_REPO_OWNER_HEX, BUG_REPORT_REPO_ID, self->subject,
      self->description, self->labels,
      (const char *const *)maintainers,
      include_logs ? self->uploaded_crash_urls : NULL,
      self->uploaded_attachment_urls, self->system_info,
      self->related_commits);
  if (!event) {
    set_processing(self, FALSE, NULL);
    show_main_toast(self, _("Failed to build issue"));
    clear_submission(self);
    return;
  }

  g_autofree char *event_json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  if (!event_json) {
    set_processing(self, FALSE, NULL);
    show_main_toast(self, _("Failed to serialize issue"));
    clear_submission(self);
    return;
  }

  GnostrMainWindow *window = get_main_window(self);
  if (!window) {
    set_processing(self, FALSE, NULL);
    clear_submission(self);
    return;
  }

  set_processing(self, TRUE, _("Signing and publishing issue…"));
  gnostr_main_window_publish_event_json_to_relays_async_internal(
      window, event_json, self->repo_relays, self->cancellable,
      on_issue_publish_complete, g_object_ref(self), g_object_unref);
  g_object_unref(window);
}

static void
set_upload_item_status(UploadItem *item, const char *text, gboolean error)
{
  if (!item || !item->status_label)
    return;
  gtk_label_set_text(item->status_label, text);
  if (error)
    gtk_widget_add_css_class(GTK_WIDGET(item->status_label), "error");
  else
    gtk_widget_remove_css_class(GTK_WIDGET(item->status_label), "error");
}

static void
on_file_uploaded(GnostrBlossomBlob *blob,
                 GError *error,
                 gpointer user_data)
{
  GnostrBugReportDialog *self = GNOSTR_BUG_REPORT_DIALOG(user_data);
  UploadItem *item =
      self->upload_items && self->upload_index < self->upload_items->len
          ? g_ptr_array_index(self->upload_items, self->upload_index)
          : NULL;

  if (self->closing) {
    if (blob)
      gnostr_blossom_blob_free(blob);
    g_object_unref(self);
    return;
  }

  if (item && blob && blob->url && *blob->url) {
    GPtrArray *urls = item->kind == UPLOAD_CRASH_LOG
                          ? self->uploaded_crash_urls
                          : self->uploaded_attachment_urls;
    g_ptr_array_add(urls, g_strdup(blob->url));
    set_upload_item_status(item, _("Uploaded"), FALSE);
    gnostr_blossom_blob_free(blob);
    self->upload_index++;
    upload_next(self);
    g_object_unref(self);
    return;
  }

  if (blob)
    gnostr_blossom_blob_free(blob);
  set_upload_item_status(item, _("Upload failed"), TRUE);
  g_warning("Issue attachment upload failed: %s",
            error && error->message ? error->message : "unknown error");

  if (item && item->kind == UPLOAD_CRASH_LOG) {
    g_ptr_array_set_size(self->uploaded_crash_urls, 0);
    self->send_without_logs = TRUE;
    set_processing(self, FALSE, NULL);
    gtk_button_set_label(self->send_button, _("Send without logs"));
    show_main_toast(self,
                    _("Crash log upload failed. You can send without logs."));
  } else {
    self->attachment_failures++;
    self->upload_index++;
    upload_next(self);
  }

  g_object_unref(self);
}

static void
upload_next(GnostrBugReportDialog *self)
{
  while (self->upload_items && self->upload_index < self->upload_items->len) {
    UploadItem *item = g_ptr_array_index(self->upload_items,
                                         self->upload_index);
    if (self->skip_crash_logs && item->kind == UPLOAD_CRASH_LOG) {
      set_upload_item_status(item, _("Skipped"), FALSE);
      self->upload_index++;
      continue;
    }

    g_autofree char *basename = g_path_get_basename(item->path);
    g_autofree char *status =
        g_strdup_printf(_("Uploading %s (%u of %u)…"), basename,
                        self->upload_index + 1, self->upload_items->len);
    set_processing(self, TRUE, status);
    set_upload_item_status(item, _("Uploading…"), FALSE);
    gnostr_blossom_upload_async(
        BUG_REPORT_BLOSSOM_SERVER, item->path,
        item->kind == UPLOAD_CRASH_LOG ? "application/json" : NULL,
        on_file_uploaded, g_object_ref(self), self->cancellable);
    return;
  }

  if (self->attachment_failures > 0)
    show_main_toast(self,
                    _("Some attachments failed to upload; sending the rest."));
  start_publish(self, !self->skip_crash_logs);
}

static void
collect_upload_items(GnostrBugReportDialog *self)
{
  self->upload_items =
      g_ptr_array_new_with_free_func((GDestroyNotify)upload_item_free);

  for (GtkWidget *row = gtk_widget_get_first_child(GTK_WIDGET(self->crash_list));
       row != NULL; row = gtk_widget_get_next_sibling(row)) {
    GtkCheckButton *check =
        GTK_CHECK_BUTTON(g_object_get_data(G_OBJECT(row), "crash-check"));
    const char *path = g_object_get_data(G_OBJECT(row), "crash-path");
    GtkLabel *status =
        GTK_LABEL(g_object_get_data(G_OBJECT(row), "upload-status"));
    if (check && path && gtk_check_button_get_active(check)) {
      UploadItem *item = g_new0(UploadItem, 1);
      item->path = g_strdup(path);
      item->kind = UPLOAD_CRASH_LOG;
      item->status_label = status;
      g_ptr_array_add(self->upload_items, item);
    }
  }

  for (GtkWidget *row =
           gtk_widget_get_first_child(GTK_WIDGET(self->attachment_list));
       row != NULL; row = gtk_widget_get_next_sibling(row)) {
    const char *path = g_object_get_data(G_OBJECT(row), "attachment-path");
    GtkLabel *status =
        GTK_LABEL(g_object_get_data(G_OBJECT(row), "upload-status"));
    if (path) {
      UploadItem *item = g_new0(UploadItem, 1);
      item->path = g_strdup(path);
      item->kind = UPLOAD_ATTACHMENT;
      item->status_label = status;
      g_ptr_array_add(self->upload_items, item);
    }
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
    self->skip_crash_logs = TRUE;
    self->upload_index = 0;
    g_ptr_array_set_size(self->uploaded_crash_urls, 0);
    g_ptr_array_set_size(self->uploaded_attachment_urls, 0);
    gtk_button_set_label(self->send_button, _("Send"));
    set_processing(self, TRUE, _("Preparing issue…"));
    upload_next(self);
    return;
  }

  clear_submission(self);
  self->subject =
      g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->subject_entry)));
  self->labels =
      g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->labels_entry)));
  self->related_commits = g_strdup(
      gtk_editable_get_text(GTK_EDITABLE(self->related_commits_entry)));
  self->description = get_text_view_text(self->description_view);
  self->system_info = get_text_view_text(self->system_info_view);
  g_strstrip(self->subject);
  g_strstrip(self->description);
  g_strstrip(self->labels);
  g_strstrip(self->related_commits);
  g_strstrip(self->system_info);

  if (!*self->subject) {
    show_main_toast(self, _("Enter a subject for the issue"));
    gtk_widget_grab_focus(GTK_WIDGET(self->subject_entry));
    return;
  }
  if (!*self->description) {
    show_main_toast(self, _("Describe the issue before sending"));
    gtk_widget_grab_focus(GTK_WIDGET(self->description_view));
    return;
  }

  collect_upload_items(self);
  self->uploaded_crash_urls = g_ptr_array_new_with_free_func(g_free);
  self->uploaded_attachment_urls = g_ptr_array_new_with_free_func(g_free);
  self->upload_index = 0;
  set_processing(self, TRUE, _("Preparing issue…"));

  if (self->upload_items->len > 0)
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
on_label_suggestion_clicked(GtkButton *button, gpointer user_data)
{
  GnostrBugReportDialog *self = GNOSTR_BUG_REPORT_DIALOG(user_data);
  const char *suggestion = gtk_button_get_label(button);
  const char *current =
      gtk_editable_get_text(GTK_EDITABLE(self->labels_entry));
  g_auto(GStrv) labels = gnostr_bug_report_parse_labels(current);
  for (guint i = 0; labels[i]; i++) {
    if (g_strcmp0(labels[i], suggestion) == 0)
      return;
  }

  g_autofree char *updated =
      current && *current
          ? g_strdup_printf("%s, %s", current, suggestion)
          : g_strdup(suggestion);
  gtk_editable_set_text(GTK_EDITABLE(self->labels_entry), updated);
}

static GtkWidget *
create_label_suggestions(GnostrBugReportDialog *self)
{
  static const char *suggestions[] = {
      "bug", "feature", "enhancement", "question",
      "crash", "ui", "performance", NULL};
  GtkWidget *flow = gtk_flow_box_new();
  gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
  gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 7);
  gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(flow), 6);
  gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(flow), 6);

  for (guint i = 0; suggestions[i]; i++) {
    GtkWidget *button = gtk_button_new_with_label(suggestions[i]);
    gtk_widget_add_css_class(button, "pill");
    g_signal_connect(button, "clicked",
                     G_CALLBACK(on_label_suggestion_clicked), self);
    gtk_flow_box_append(GTK_FLOW_BOX(flow), button);
  }
  return flow;
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

    GtkWidget *upload_status = gtk_label_new("");
    gtk_widget_add_css_class(upload_status, "dim-label");
    gtk_box_append(GTK_BOX(box), labels);
    gtk_box_append(GTK_BOX(box), upload_status);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    g_object_set_data(G_OBJECT(row), "crash-check", check);
    g_object_set_data(G_OBJECT(row), "upload-status", upload_status);
    g_object_set_data_full(G_OBJECT(row), "crash-path", g_strdup(path), g_free);
    gtk_list_box_append(self->crash_list, row);
  }
}

static void
on_remove_attachment_clicked(GtkButton *button, gpointer user_data)
{
  GnostrBugReportDialog *self = GNOSTR_BUG_REPORT_DIALOG(user_data);
  GtkWidget *row = g_object_get_data(G_OBJECT(button), "attachment-row");
  if (row)
    gtk_list_box_remove(self->attachment_list, row);
}

static void
append_attachment_row(GnostrBugReportDialog *self,
                      const char *path,
                      goffset size)
{
  g_autofree char *name = g_path_get_basename(path);
  g_autofree char *size_text = g_format_size((guint64)size);
  GtkWidget *row = gtk_list_box_row_new();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_set_margin_top(box, 8);
  gtk_widget_set_margin_bottom(box, 8);
  gtk_widget_set_margin_start(box, 10);
  gtk_widget_set_margin_end(box, 10);

  GtkWidget *labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand(labels, TRUE);
  GtkWidget *filename = gtk_label_new(name);
  gtk_label_set_xalign(GTK_LABEL(filename), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(filename), PANGO_ELLIPSIZE_MIDDLE);
  GtkWidget *details = gtk_label_new(size_text);
  gtk_label_set_xalign(GTK_LABEL(details), 0.0f);
  gtk_widget_add_css_class(details, "dim-label");
  gtk_box_append(GTK_BOX(labels), filename);
  gtk_box_append(GTK_BOX(labels), details);

  GtkWidget *upload_status = gtk_label_new(_("Ready"));
  gtk_widget_add_css_class(upload_status, "dim-label");
  GtkWidget *remove = gtk_button_new_from_icon_name("user-trash-symbolic");
  gtk_widget_add_css_class(remove, "flat");
  gtk_widget_set_tooltip_text(remove, _("Remove attachment"));
  g_object_set_data(G_OBJECT(remove), "attachment-row", row);
  g_signal_connect(remove, "clicked",
                   G_CALLBACK(on_remove_attachment_clicked), self);

  gtk_box_append(GTK_BOX(box), labels);
  gtk_box_append(GTK_BOX(box), upload_status);
  gtk_box_append(GTK_BOX(box), remove);
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
  g_object_set_data(G_OBJECT(row), "upload-status", upload_status);
  g_object_set_data_full(G_OBJECT(row), "attachment-path",
                         g_strdup(path), g_free);
  gtk_list_box_append(self->attachment_list, row);
}

static void
on_files_selected(GObject *source, GAsyncResult *result, gpointer user_data)
{
  GnostrBugReportDialog *self = GNOSTR_BUG_REPORT_DIALOG(user_data);
  g_autoptr(GError) error = NULL;
  g_autoptr(GListModel) files =
      gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(source),
                                           result, &error);
  gboolean rejected_large = FALSE;

  if (files) {
    guint count = g_list_model_get_n_items(files);
    for (guint i = 0; i < count; i++) {
      g_autoptr(GFile) file = g_list_model_get_item(files, i);
      g_autofree char *path = g_file_get_path(file);
      if (!path)
        continue;
      g_autoptr(GFileInfo) info =
          g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_SIZE,
                            G_FILE_QUERY_INFO_NONE, NULL, NULL);
      goffset size = info ? g_file_info_get_size(info) : 0;
      if (size > MAX_ATTACHMENT_BYTES) {
        rejected_large = TRUE;
        continue;
      }
      append_attachment_row(self, path, size);
    }
  } else if (error &&
             !g_error_matches(error, GTK_DIALOG_ERROR,
                              GTK_DIALOG_ERROR_CANCELLED)) {
    g_warning("File selection failed: %s", error->message);
  }

  if (rejected_large)
    show_main_toast(self, _("Files larger than 25 MB were not added."));
  g_object_unref(self);
}

static void
on_attach_clicked(GtkButton *button, gpointer user_data)
{
  (void)button;
  GnostrBugReportDialog *self = GNOSTR_BUG_REPORT_DIALOG(user_data);
  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
  if (!GTK_IS_WINDOW(root))
    return;

  GtkFileDialog *dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog, _("Attach files"));
  gtk_file_dialog_set_modal(dialog, TRUE);
  gtk_file_dialog_open_multiple(dialog, GTK_WINDOW(root), self->cancellable,
                                on_files_selected, g_object_ref(self));
  g_object_unref(dialog);
}

static void
gnostr_bug_report_dialog_dispose(GObject *object)
{
  GnostrBugReportDialog *self = GNOSTR_BUG_REPORT_DIALOG(object);
  if (self->cancellable)
    g_cancellable_cancel(self->cancellable);
  g_clear_object(&self->cancellable);
  clear_submission(self);
  g_clear_pointer(&self->repo_maintainers, g_ptr_array_unref);
  g_clear_pointer(&self->repo_relays, g_ptr_array_unref);
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
  self->repo_maintainers = g_ptr_array_new_with_free_func(g_free);
  self->repo_relays = g_ptr_array_new_with_free_func(g_free);
  g_signal_connect(self, "closed", G_CALLBACK(on_dialog_closed), self);

  adw_dialog_set_title(ADW_DIALOG(self), _("Report an Issue"));
  adw_dialog_set_content_width(ADW_DIALOG(self), 600);
  adw_dialog_set_content_height(ADW_DIALOG(self), 720);

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
                                 _("Briefly summarize the issue"));
  gtk_entry_set_max_length(self->subject_entry, 160);
  gtk_box_append(GTK_BOX(content), GTK_WIDGET(self->subject_entry));

  gtk_box_append(GTK_BOX(content), make_field_label(_("Labels")));
  self->labels_entry = GTK_ENTRY(gtk_entry_new());
  gtk_entry_set_placeholder_text(self->labels_entry,
                                 _("Comma-separated labels"));
  gtk_editable_set_text(GTK_EDITABLE(self->labels_entry), "bug");
  gtk_box_append(GTK_BOX(content), GTK_WIDGET(self->labels_entry));
  gtk_box_append(GTK_BOX(content), create_label_suggestions(self));

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

  gtk_box_append(GTK_BOX(content),
                 make_field_label(_("Related commit SHAs")));
  self->related_commits_entry = GTK_ENTRY(gtk_entry_new());
  gtk_entry_set_placeholder_text(
      self->related_commits_entry,
      _("Optional, separated by spaces or commas"));
  gtk_box_append(GTK_BOX(content),
                 GTK_WIDGET(self->related_commits_entry));

  GtkWidget *system_expander = gtk_expander_new(_("System info"));
  gtk_expander_set_expanded(GTK_EXPANDER(system_expander), FALSE);
  GtkWidget *system_frame = gtk_frame_new(NULL);
  gtk_widget_set_margin_top(system_frame, 8);
  gtk_widget_set_size_request(system_frame, -1, 150);
  self->system_info_view = GTK_TEXT_VIEW(gtk_text_view_new());
  gtk_text_view_set_wrap_mode(self->system_info_view, GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_monospace(self->system_info_view, TRUE);
  gtk_text_view_set_top_margin(self->system_info_view, 8);
  gtk_text_view_set_bottom_margin(self->system_info_view, 8);
  gtk_text_view_set_left_margin(self->system_info_view, 8);
  gtk_text_view_set_right_margin(self->system_info_view, 8);
  g_autofree char *system_info = collect_system_info();
  gtk_text_buffer_set_text(
      gtk_text_view_get_buffer(self->system_info_view), system_info, -1);
  gtk_frame_set_child(GTK_FRAME(system_frame),
                      GTK_WIDGET(self->system_info_view));
  gtk_expander_set_child(GTK_EXPANDER(system_expander), system_frame);
  gtk_widget_set_tooltip_text(
      system_expander,
      _("This information is included only after you review or edit it."));
  gtk_box_append(GTK_BOX(content), system_expander);

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

  GtkWidget *attachment_group = adw_preferences_group_new();
  adw_preferences_group_set_title(
      ADW_PREFERENCES_GROUP(attachment_group), _("Attachments"));
  adw_preferences_group_set_description(
      ADW_PREFERENCES_GROUP(attachment_group),
      _("Optional files are uploaded publicly to Blossom (25 MB maximum)."));
  self->attach_button =
      GTK_BUTTON(gtk_button_new_with_label(_("Attach files…")));
  gtk_widget_set_halign(GTK_WIDGET(self->attach_button), GTK_ALIGN_START);
  g_signal_connect(self->attach_button, "clicked",
                   G_CALLBACK(on_attach_clicked), self);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(attachment_group),
                            GTK_WIDGET(self->attach_button));
  self->attachment_list = GTK_LIST_BOX(gtk_list_box_new());
  gtk_list_box_set_selection_mode(self->attachment_list, GTK_SELECTION_NONE);
  gtk_widget_add_css_class(GTK_WIDGET(self->attachment_list), "boxed-list");
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(attachment_group),
                            GTK_WIDGET(self->attachment_list));
  gtk_box_append(GTK_BOX(content), attachment_group);

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

  fetch_repository_metadata(self);
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
