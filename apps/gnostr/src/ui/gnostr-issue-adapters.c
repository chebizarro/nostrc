#define G_LOG_DOMAIN "gnostr-issue"

/* gnostr services for the portable NIP-34 issue form (nostrc-8xfib.5).
 * Ported from the former gnostr-bug-report-dialog.c. */
#include "gnostr-issue-adapters.h"
#include "gnostr-main-window-private.h"

#include "../util/blossom.h"
#include "gnostr-build-info.h"
#include "../util/utils.h"
#include "../../../../nips/nip34/include/nip34.h"
#include "nostr-event.h"
#include "nostr-filter.h"

#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <nostr-gobject-1.0/gnostr-relays.h>
#include <nostr-gobject-1.0/nostr_pool.h>
#include <nostr-gobject-1.0/storage_ndb.h>
#include <sys/stat.h>

#define BUG_REPORT_REPO_ID "nostrc"
/*
 * npub1ehhfg09mr8z34wz85ek46a6rww4f7c7jsujxhdvmpqnl5hnrwsqq2szjqv
 * decoded with NIP-19. Keeping the npub beside the build-time value makes the
 * target identity auditable without adding runtime decoding failure modes.
 */
#define BUG_REPORT_REPO_OWNER_HEX \
  "cdee943cbb19c51ab847a66d5d774373aa9f63d287246bb59b0827fa5e637400"

/* ---- Crash logs ---- */

typedef struct {
  char *path;
  gint64 mtime;
} CrashLogEntry;

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

GPtrArray *
gnostr_issue_discover_crash_logs(const char *directory)
{
  g_autofree char *default_directory = NULL;
  if (!directory) {
#ifdef __APPLE__
    default_directory = g_build_filename(g_get_home_dir(), "Library", "Logs",
                                         "DiagnosticReports", NULL);
    directory = default_directory;
#else
    return g_ptr_array_new_with_free_func(g_free);
#endif
  }

  g_autoptr(GPtrArray) entries =
      g_ptr_array_new_with_free_func((GDestroyNotify)crash_log_entry_free);
  g_autoptr(GDir) dir = g_dir_open(directory, 0, NULL);
  const char *name = NULL;
  while (dir && (name = g_dir_read_name(dir)) != NULL) {
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

  GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; i < entries->len; i++)
    g_ptr_array_add(paths, g_strdup(((CrashLogEntry *)g_ptr_array_index(entries, i))->path));
  return paths;
}

/* ---- System info ---- */

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

char *
gnostr_issue_collect_system_info(void)
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

static char *
system_info_func(gpointer data)
{
  (void)data;
  return gnostr_issue_collect_system_info();
}

/* ---- Publisher: main-window signer + pool ---- */

#define GNOSTR_TYPE_ISSUE_PUBLISHER (gnostr_issue_publisher_get_type())
G_DECLARE_FINAL_TYPE(GnostrIssuePublisher, gnostr_issue_publisher, GNOSTR, ISSUE_PUBLISHER, GObject)

struct _GnostrIssuePublisher {
  GObject parent_instance;
  GWeakRef window;
};

static void publisher_iface_init(GnIssuePublisherInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE(GnostrIssuePublisher, gnostr_issue_publisher, G_TYPE_OBJECT,
  G_IMPLEMENT_INTERFACE(GN_TYPE_ISSUE_PUBLISHER, publisher_iface_init))

static void
issue_published(gboolean success, const char *event_id, gpointer data)
{
  GTask *task = data;
  if (g_task_had_error(task) || g_task_get_completed(task))
    return;
  if (success && event_id && *event_id)
    g_task_return_pointer(task, g_strdup(event_id), g_free);
  else
    g_task_return_new_error(task, GN_ISSUE_PUBLISH_ERROR, GN_ISSUE_PUBLISH_ERROR_NOT_SENT,
                            _("Failed to send issue"));
}

static void
publisher_sign_and_publish_async(GnIssuePublisher *publisher, const gchar *unsigned_json,
                                 const gchar *const *relays, GCancellable *cancellable,
                                 GAsyncReadyCallback callback, gpointer user_data)
{
  GnostrIssuePublisher *self = GNOSTR_ISSUE_PUBLISHER(publisher);
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_autoptr(GObject) window = g_weak_ref_get(&self->window);
  if (!window) {
    g_task_return_new_error(task, GN_ISSUE_PUBLISH_ERROR, GN_ISSUE_PUBLISH_ERROR_NOT_SENT,
                            _("Failed to send issue"));
    g_object_unref(task);
    return;
  }
  /* The announced and user-listed relays are added to gnostr's write relays. */
  g_autoptr(GPtrArray) extra = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; relays && relays[i]; i++)
    g_ptr_array_add(extra, g_strdup(relays[i]));
  gnostr_main_window_publish_event_json_to_relays_async_internal(
      GNOSTR_MAIN_WINDOW(window), unsigned_json, extra, cancellable,
      issue_published, task, g_object_unref);
}

static gboolean
publisher_sign_and_publish_finish(GnIssuePublisher *publisher, GAsyncResult *result,
                                  gchar **event_id, gchar **message, GError **error)
{
  (void)publisher;
  (void)message;
  gchar *id = g_task_propagate_pointer(G_TASK(result), error);
  if (!id)
    return FALSE;
  if (event_id)
    *event_id = id;
  else
    g_free(id);
  return TRUE;
}

static gchar *
publisher_normalize_relay(GnIssuePublisher *publisher, const gchar *url, GError **error)
{
  (void)publisher;
  /* gnostr's relay lists may hold ws:// relays (e.g. a local relay). */
  g_autofree gchar *text = g_strstrip(g_strdup(url ? url : ""));
  g_autoptr(GUri) uri = *text ? g_uri_parse(text, G_URI_FLAGS_NONE, NULL) : NULL;
  const char *scheme = uri ? g_uri_get_scheme(uri) : NULL;
  if (!uri || (g_ascii_strcasecmp(scheme, "wss") != 0 && g_ascii_strcasecmp(scheme, "ws") != 0) ||
      !g_uri_get_host(uri) || !*g_uri_get_host(uri) || g_uri_get_userinfo(uri)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        _("Enter WebSocket relay URLs (wss://), separated by spaces."));
    return NULL;
  }
  return g_steal_pointer(&text);
}

static void
publisher_iface_init(GnIssuePublisherInterface *iface)
{
  iface->sign_and_publish_async = publisher_sign_and_publish_async;
  iface->sign_and_publish_finish = publisher_sign_and_publish_finish;
  iface->normalize_relay = publisher_normalize_relay;
}

static void
gnostr_issue_publisher_finalize(GObject *object)
{
  g_weak_ref_clear(&GNOSTR_ISSUE_PUBLISHER(object)->window);
  G_OBJECT_CLASS(gnostr_issue_publisher_parent_class)->finalize(object);
}

static void
gnostr_issue_publisher_class_init(GnostrIssuePublisherClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = gnostr_issue_publisher_finalize;
}

static void
gnostr_issue_publisher_init(GnostrIssuePublisher *self)
{
  g_weak_ref_init(&self->window, NULL);
}

GnIssuePublisher *
gnostr_issue_publisher_new(GnostrMainWindow *window)
{
  GnostrIssuePublisher *self = g_object_new(GNOSTR_TYPE_ISSUE_PUBLISHER, NULL);
  g_weak_ref_set(&self->window, window);
  return GN_ISSUE_PUBLISHER(self);
}

/* ---- Uploader: Blossom ---- */

#define GNOSTR_TYPE_ISSUE_UPLOADER (gnostr_issue_uploader_get_type())
G_DECLARE_FINAL_TYPE(GnostrIssueUploader, gnostr_issue_uploader, GNOSTR, ISSUE_UPLOADER, GObject)

struct _GnostrIssueUploader {
  GObject parent_instance;
  char *server;
};

static void uploader_iface_init(GnIssueUploaderInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE(GnostrIssueUploader, gnostr_issue_uploader, G_TYPE_OBJECT,
  G_IMPLEMENT_INTERFACE(GN_TYPE_ISSUE_UPLOADER, uploader_iface_init))

static const gchar *
uploader_describe(GnIssueUploader *uploader)
{
  return GNOSTR_ISSUE_UPLOADER(uploader)->server;
}

static void
file_uploaded(GnostrBlossomBlob *blob, GError *error, gpointer data)
{
  GTask *task = data;
  if (blob && blob->url && *blob->url)
    g_task_return_pointer(task, g_strdup(blob->url), g_free);
  else if (error)
    g_task_return_error(task, g_error_copy(error));
  else
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, _("Upload failed"));
  if (blob)
    gnostr_blossom_blob_free(blob);
  g_object_unref(task);
}

static void
uploader_upload_async(GnIssueUploader *uploader, GFile *file, const gchar *content_type,
                      GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data)
{
  GnostrIssueUploader *self = GNOSTR_ISSUE_UPLOADER(uploader);
  GTask *task = g_task_new(self, cancellable, callback, data);
  g_autofree char *path = g_file_get_path(file);
  if (!path) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, _("Upload failed"));
    g_object_unref(task);
    return;
  }
  gnostr_blossom_upload_async(self->server, path, content_type, file_uploaded, task, cancellable);
}

static gchar *
uploader_upload_finish(GnIssueUploader *uploader, GAsyncResult *result, GError **error)
{
  (void)uploader;
  return g_task_propagate_pointer(G_TASK(result), error);
}

static void
uploader_iface_init(GnIssueUploaderInterface *iface)
{
  iface->describe_destination = uploader_describe;
  iface->upload_async = uploader_upload_async;
  iface->upload_finish = uploader_upload_finish;
}

static void
gnostr_issue_uploader_finalize(GObject *object)
{
  g_free(GNOSTR_ISSUE_UPLOADER(object)->server);
  G_OBJECT_CLASS(gnostr_issue_uploader_parent_class)->finalize(object);
}

static void
gnostr_issue_uploader_class_init(GnostrIssueUploaderClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = gnostr_issue_uploader_finalize;
}

static void gnostr_issue_uploader_init(GnostrIssueUploader *self) { (void)self; }

GnIssueUploader *
gnostr_issue_uploader_new(const char *server_url)
{
  GnostrIssueUploader *self = g_object_new(GNOSTR_TYPE_ISSUE_UPLOADER, NULL);
  self->server = g_strdup(server_url);
  return GN_ISSUE_UPLOADER(self);
}

/* ---- Resolver: NDB first, then read relays ---- */

#define GNOSTR_TYPE_ISSUE_RESOLVER (gnostr_issue_resolver_get_type())
G_DECLARE_FINAL_TYPE(GnostrIssueResolver, gnostr_issue_resolver, GNOSTR, ISSUE_RESOLVER, GObject)

struct _GnostrIssueResolver {
  GObject parent_instance;
};

static void resolver_iface_init(GnIssueRepoResolverInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE(GnostrIssueResolver, gnostr_issue_resolver, G_TYPE_OBJECT,
  G_IMPLEMENT_INTERFACE(GN_TYPE_ISSUE_REPO_RESOLVER, resolver_iface_init))

static void
ndb_query_thread(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
  (void)source;
  (void)cancellable;
  const GnIssueTarget *hint = task_data;
  if (g_task_return_error_if_cancelled(task))
    return;
  void *txn = NULL;
  if (storage_ndb_begin_query_retry(&txn, 3, 10, NULL) != 0 || !txn) {
    g_task_return_pointer(task, NULL, NULL);
    return;
  }
  g_autofree char *filter = g_strdup_printf(
      "{\"kinds\":[30617],\"authors\":[\"%s\"],\"#d\":[\"%s\"],\"limit\":5}",
      hint->owner_hex, hint->repo_id);
  char **results = NULL;
  int count = 0;
  GnIssueTarget *target = NULL;
  if (storage_ndb_query(txn, filter, &results, &count, NULL) == 0)
    target = gn_issue_target_from_announcements(hint, (const gchar *const *)results, count);
  if (results)
    storage_ndb_free_results(results, count);
  storage_ndb_end_query(txn);
  g_task_return_pointer(task, target, (GDestroyNotify)gn_issue_target_free);
}

static void
relay_query_done(GObject *source, GAsyncResult *result, gpointer data)
{
  GTask *task = data;
  const GnIssueTarget *hint = g_task_get_task_data(task);
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) results = gnostr_pool_query_finish(GNOSTR_POOL(source), result, &error);
  GnIssueTarget *target = NULL;
  if (results) {
    target = gn_issue_target_from_announcements(hint, (const gchar *const *)results->pdata,
                                                results->len);
    if (target) {
      GPtrArray *to_ingest = g_ptr_array_new_with_free_func(g_free);
      for (guint i = 0; i < results->len; i++)
        g_ptr_array_add(to_ingest, g_strdup(g_ptr_array_index(results, i)));
      storage_ndb_ingest_events_async(to_ingest);
    }
  } else if (error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    g_debug("Repository announcement relay query failed: %s", error->message);
  }
  if (error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_task_return_error(task, g_steal_pointer(&error));
  else
    g_task_return_pointer(task, target, (GDestroyNotify)gn_issue_target_free);
  g_object_unref(task);
}

static void
fetch_from_relays(GTask *task)
{
  const GnIssueTarget *hint = g_task_get_task_data(task);
  g_autoptr(GPtrArray) relay_urls = gnostr_get_read_relay_urls();
  GNostrPool *pool = gnostr_get_shared_query_pool();
  if (!pool || !relay_urls || relay_urls->len == 0) {
    g_debug("Repository announcement unavailable; using the built-in address");
    g_task_return_pointer(task, NULL, NULL);
    g_object_unref(task);
    return;
  }
  const char **urls = g_new0(const char *, relay_urls->len);
  for (guint i = 0; i < relay_urls->len; i++)
    urls[i] = g_ptr_array_index(relay_urls, i);
  NostrFilter *filter = nostr_filter_new();
  int kinds[] = {NIP34_KIND_REPOSITORY};
  const char *authors[] = {hint->owner_hex};
  nostr_filter_set_kinds(filter, kinds, 1);
  nostr_filter_set_authors(filter, authors, 1);
  nostr_filter_tags_append(filter, "d", hint->repo_id, NULL);
  nostr_filter_set_limit(filter, 5);
  NostrFilters *filters = nostr_filters_new();
  nostr_filters_add(filters, filter);
  nostr_filter_free(filter);
  gnostr_pool_query_urls_async(pool, (const gchar **)urls, relay_urls->len, filters,
                               g_task_get_cancellable(task), relay_query_done, task);
  g_free(urls);
}

static void
ndb_query_done(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  GTask *outer = data;
  g_autoptr(GError) error = NULL;
  GnIssueTarget *target = g_task_propagate_pointer(G_TASK(result), &error);
  if (target) {
    g_task_return_pointer(outer, target, (GDestroyNotify)gn_issue_target_free);
    g_object_unref(outer);
  } else if (error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    g_task_return_error(outer, g_steal_pointer(&error));
    g_object_unref(outer);
  } else {
    fetch_from_relays(outer);
  }
}

static void
resolver_resolve_async(GnIssueRepoResolver *resolver, const GnIssueTarget *hint,
                       GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data)
{
  GTask *outer = g_task_new(resolver, cancellable, callback, data);
  g_task_set_task_data(outer, gn_issue_target_copy(hint), (GDestroyNotify)gn_issue_target_free);
  GTask *inner = g_task_new(NULL, cancellable, ndb_query_done, outer);
  g_task_set_task_data(inner, gn_issue_target_copy(hint), (GDestroyNotify)gn_issue_target_free);
  g_task_run_in_thread(inner, ndb_query_thread);
  g_object_unref(inner);
}

static GnIssueTarget *
resolver_resolve_finish(GnIssueRepoResolver *resolver, GAsyncResult *result, GError **error)
{
  (void)resolver;
  return g_task_propagate_pointer(G_TASK(result), error);
}

static void
resolver_iface_init(GnIssueRepoResolverInterface *iface)
{
  iface->resolve_async = resolver_resolve_async;
  iface->resolve_finish = resolver_resolve_finish;
}

static void gnostr_issue_resolver_class_init(GnostrIssueResolverClass *klass) { (void)klass; }
static void gnostr_issue_resolver_init(GnostrIssueResolver *self) { (void)self; }

GnIssueRepoResolver *
gnostr_issue_resolver_new(void)
{
  return g_object_new(GNOSTR_TYPE_ISSUE_RESOLVER, NULL);
}

/* ---- The form ---- */

static void
on_toast(GnNip34IssueView *view, const char *message, gpointer data)
{
  (void)view;
  g_autoptr(GObject) window = g_weak_ref_get(data);
  if (window && message && *message)
    gnostr_main_window_show_toast_internal(GNOSTR_MAIN_WINDOW(window), message);
}

static void
on_published(GnNip34IssueView *view, const char *issue_id, gpointer data)
{
  g_autoptr(GObject) window = g_weak_ref_get(data);
  if (window && issue_id && *issue_id) {
    /* Best effort: the issue is already accepted. The publisher copies the
     * relays, so this continues after the form closes. */
    const GnIssueTarget *target = gn_nip34_issue_view_get_target(view);
    NostrEvent *status = gn_issue_build_open_status(issue_id, target,
                                                    gn_nip34_issue_view_get_pubkey(view));
    g_autofree char *json = status ? nostr_event_serialize_compact(status) : NULL;
    if (status)
      nostr_event_free(status);
    if (json) {
      g_autoptr(GPtrArray) relays = g_ptr_array_new_with_free_func(g_free);
      for (guint i = 0; target->relays[i]; i++)
        g_ptr_array_add(relays, g_strdup(target->relays[i]));
      gnostr_main_window_publish_event_json_to_relays_async_internal(
          GNOSTR_MAIN_WINDOW(window), json, relays, NULL, NULL, NULL, NULL);
    } else {
      g_warning("Issue sent, but the open-status event could not be built");
    }
    gnostr_main_window_show_toast_internal(GNOSTR_MAIN_WINDOW(window), _("Issue sent"));
  }
  gn_nip34_issue_view_close(view);
}

static void
weak_ref_free(gpointer data, GClosure *closure)
{
  (void)closure;
  g_weak_ref_clear(data);
  g_free(data);
}

static GWeakRef *
window_ref(GnostrMainWindow *window)
{
  GWeakRef *ref = g_new0(GWeakRef, 1);
  g_weak_ref_init(ref, window);
  return ref;
}

GnNip34IssueView *
gnostr_issue_view_new(GnostrMainWindow *window)
{
  g_return_val_if_fail(GNOSTR_IS_MAIN_WINDOW(window), NULL);
  g_autoptr(GnIssueTarget) target =
      gn_issue_target_new(BUG_REPORT_REPO_OWNER_HEX, BUG_REPORT_REPO_ID, NULL, NULL);
  g_autoptr(GnIssuePublisher) publisher = gnostr_issue_publisher_new(window);
  g_autoptr(GnIssueUploader) uploader = gnostr_issue_uploader_new(GNOSTR_ISSUE_BLOSSOM_SERVER);
  g_autoptr(GnIssueRepoResolver) resolver = gnostr_issue_resolver_new();
  GnNip34IssueView *view = gn_nip34_issue_view_new(target, publisher, resolver, uploader);

  gn_nip34_issue_view_set_pubkey(view, window->user_pubkey_hex);
  GnNip34IssueFieldsSnapshot labels = { .labels = (gchar *)"bug" };
  gn_nip34_issue_fields_set_snapshot(gn_nip34_issue_view_get_fields(view), &labels);
  g_autoptr(GPtrArray) write_relays = gnostr_get_write_relay_urls();
  if (write_relays && write_relays->len) {
    g_ptr_array_add(write_relays, NULL);
    gn_nip34_issue_view_set_relays(view, (const gchar *const *)write_relays->pdata);
  }
  gn_nip34_issue_view_set_diagnostics(view, "System info", _("Include System Info"),
      _("App and toolkit versions, OS and architecture. You can edit it before sending."),
      GN_ISSUE_DIAGNOSTICS_DEFAULT_ON | GN_ISSUE_DIAGNOSTICS_EDITABLE,
      system_info_func, NULL, NULL);

  g_autoptr(GPtrArray) paths = gnostr_issue_discover_crash_logs(NULL);
  g_autoptr(GListStore) logs = g_list_store_new(G_TYPE_FILE);
  for (guint i = 0; i < paths->len; i++) {
    g_autoptr(GFile) file = g_file_new_for_path(g_ptr_array_index(paths, i));
    g_list_store_append(logs, file);
  }
  gn_nip34_issue_view_set_crash_logs(view, G_LIST_MODEL(logs));

  g_signal_connect_data(view, "toast", G_CALLBACK(on_toast), window_ref(window), weak_ref_free, 0);
  g_signal_connect_data(view, "published", G_CALLBACK(on_published), window_ref(window),
                        weak_ref_free, 0);
  /* gnostr's behaviour: look the announcement up (NDB, then relays) on open. */
  gn_nip34_issue_view_lookup_repository(view);
  return view;
}

void
gnostr_main_window_on_bug_report_requested_internal(GnostrSessionView *session_view,
                                                    gpointer user_data)
{
  (void)session_view;
  GnostrMainWindow *window = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(window))
    return;
  GnNip34IssueView *view = gnostr_issue_view_new(window);
  if (view)
    gn_nip34_issue_view_present(view, GTK_WIDGET(window));
}
