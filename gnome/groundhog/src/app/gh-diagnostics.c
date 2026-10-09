#include "gh-diagnostics.h"

#include <dirent.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_KEYS 96
#define MAX_REPORT_BYTES 8000
#define MAX_FILE_BYTES (256 * 1024)

static const gchar *const components[] = {
  "relay", "store", "signer", "nip17", "nip29", "marmot", "ui"
};
static const gchar *const events[] = {
  "connect_failed", "publish_rejected", "history_partial", "render_fallback"
};
static const gchar *const results[] = { "ok", "retry", "failed" };

struct _GhDiagnostics {
  GSettings *settings;
  gchar *state_home;
  GHashTable *counts; /* reviewed day/component/event/result -> guint32 */
  guint flush_source;
  gboolean enabled;
  gboolean disk_failed;
  gchar *save_error;
  gchar *delete_error;
  GhDiagnosticsStatusFunc status_callback;
  gpointer status_data;
};

static GhDiagnostics *process_diagnostics;

static gboolean
valid_day(const gchar *day)
{
  if (!day || strlen(day) != 10 || day[4] != '-' || day[7] != '-')
    return FALSE;
  for (guint i = 0; i < 10; i++)
    if (i != 4 && i != 7 && !g_ascii_isdigit(day[i]))
      return FALSE;
  g_autoptr(GDateTime) date = g_date_time_new_local(
    atoi(day), atoi(day + 5), atoi(day + 8), 12, 0, 0);
  return date && g_date_time_get_year(date) == atoi(day) &&
         g_date_time_get_month(date) == atoi(day + 5) &&
         g_date_time_get_day_of_month(date) == atoi(day + 8);
}

static gboolean
recent_day(const gchar *day)
{
  if (!valid_day(day))
    return FALSE;
  g_autoptr(GDateTime) today = g_date_time_new_now_local();
  GDate old_date, current_date;
  g_date_clear(&old_date, 1);
  g_date_clear(&current_date, 1);
  g_date_set_dmy(&old_date, (GDateDay)atoi(day + 8),
                 (GDateMonth)atoi(day + 5), (GDateYear)atoi(day));
  g_date_set_dmy(&current_date, (GDateDay)g_date_time_get_day_of_month(today),
                 (GDateMonth)g_date_time_get_month(today),
                 (GDateYear)g_date_time_get_year(today));
  gint days = g_date_days_between(&old_date, &current_date);
  return days >= 0 && days < 7;
}

static gchar *
today_string(void)
{
  g_autoptr(GDateTime) now = g_date_time_new_now_local();
  return g_date_time_format(now, "%Y-%m-%d");
}

static int
open_directory(int parent, const gchar *name, gboolean create, gboolean private,
               GError **error)
{
  if (create && mkdirat(parent, name, 0700) < 0 && errno != EEXIST) {
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                "Could not create local diagnostics directory: %s", g_strerror(errno));
    return -1;
  }
  int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (!create && fd < 0 && errno == ENOENT) return -1;
  struct stat st;
  if (fd < 0 || fstat(fd, &st) < 0 || !S_ISDIR(st.st_mode) ||
      st.st_uid != getuid() || (private && (st.st_mode & 077) != 0)) {
    gint saved = errno;
    if (fd >= 0) close(fd);
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved ? saved : EACCES),
                "Local diagnostics directory is not private or is a symlink");
    return -1;
  }
  return fd;
}

static int
open_state(GhDiagnostics *self, gboolean create, GError **error)
{
  if (create && g_mkdir_with_parents(self->state_home, 0700) != 0) {
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                "Could not create state directory: %s", g_strerror(errno));
    return -1;
  }
  int home = open(self->state_home, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (home < 0) {
    if (!create && errno == ENOENT) return -1;
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                "Could not open state directory: %s", g_strerror(errno));
    return -1;
  }
  int groundhog = open_directory(home, "groundhog", create, FALSE, error);
  close(home);
  if (groundhog < 0) return -1;
  int diagnostics = open_directory(groundhog, "diagnostics", create, TRUE, error);
  close(groundhog);
  return diagnostics;
}

static gboolean
is_daily_file(const gchar *name)
{
  if (strlen(name) != 14 || !g_str_has_suffix(name, ".tsv")) return FALSE;
  g_autofree gchar *day = g_strndup(name, 10);
  return valid_day(day);
}

static gboolean
remove_files(int dirfd, GError **error)
{
  DIR *dir = fdopendir(dup(dirfd));
  if (!dir) {
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                "Could not list diagnostics files: %s", g_strerror(errno));
    return FALSE;
  }
  gboolean ok = TRUE;
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (g_str_equal(entry->d_name, ".") || g_str_equal(entry->d_name, "..")) continue;
    if (unlinkat(dirfd, entry->d_name, 0) < 0) {
      if (ok)
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                    "Could not delete diagnostics file: %s", g_strerror(errno));
      ok = FALSE;
    }
  }
  closedir(dir);
  if (ok && fsync(dirfd) < 0) {
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                "Could not sync diagnostics deletion: %s", g_strerror(errno));
    ok = FALSE;
  }
  return ok;
}

static gint
compare_keys(gconstpointer a, gconstpointer b)
{
  return g_strcmp0(a, b);
}

static gint
compare_string_pointers(gconstpointer a, gconstpointer b)
{
  return g_strcmp0(*(const gchar *const *)a, *(const gchar *const *)b);
}

static void
prune_counts(GhDiagnostics *self)
{
  GHashTableIter iter;
  gpointer key;
  g_hash_table_iter_init(&iter, self->counts);
  while (g_hash_table_iter_next(&iter, &key, NULL)) {
    const gchar *s = key;
    g_autofree gchar *day = g_strndup(s, 10);
    if (!recent_day(day)) g_hash_table_iter_remove(&iter);
  }
}

static gboolean
write_all(int fd, const gchar *data, gsize length)
{
  while (length > 0) {
    ssize_t n = write(fd, data, length);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return FALSE;
    data += n;
    length -= (gsize)n;
  }
  return TRUE;
}

static gboolean
persist_day(GhDiagnostics *self, int dirfd, const gchar *day, GError **error)
{
  GList *keys = g_hash_table_get_keys(self->counts);
  keys = g_list_sort(keys, compare_keys);
  g_autoptr(GString) body = g_string_new("schema\t1\n");
  for (GList *l = keys; l; l = l->next) {
    const gchar *key = l->data;
    if (strncmp(key, day, 10) != 0 || key[10] != '\t') continue;
    guint32 *count = g_hash_table_lookup(self->counts, key);
    g_string_append_printf(body, "%s\t%u\n", key, *count);
  }
  g_list_free(keys);
  if (body->len > MAX_FILE_BYTES) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                        "Diagnostics file exceeded its size limit");
    return FALSE;
  }
  g_autofree gchar *filename = g_strdup_printf("%s.tsv", day);
  g_autofree gchar *tmp = g_strdup_printf(".%s-%u-%u.tmp", day,
                                            (guint)getpid(), g_random_int());
  int fd = openat(dirfd, tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) {
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                "Could not create diagnostics file: %s", g_strerror(errno));
    return FALSE;
  }
  gboolean ok = write_all(fd, body->str, body->len) && fsync(fd) == 0;
  gint saved = errno;
  if (close(fd) < 0) { ok = FALSE; saved = errno; }
  if (ok && renameat(dirfd, tmp, dirfd, filename) < 0) {
    ok = FALSE; saved = errno;
  }
  if (ok && fsync(dirfd) < 0) { ok = FALSE; saved = errno; }
  if (!ok) {
    unlinkat(dirfd, tmp, 0);
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved),
                "Could not save diagnostics: %s", g_strerror(saved));
  }
  return ok;
}

static gboolean
rotate_files(int dirfd, GError **error)
{
  DIR *dir = fdopendir(dup(dirfd));
  if (!dir) {
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                "Could not rotate diagnostics: %s", g_strerror(errno));
    return FALSE;
  }
  GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL)
    if (is_daily_file(entry->d_name))
      g_ptr_array_add(names, g_strdup(entry->d_name));
  closedir(dir);
  g_ptr_array_sort(names, (GCompareFunc)compare_string_pointers);
  guint retained = 0;
  gboolean ok = TRUE;
  for (gint i = (gint)names->len - 1; i >= 0; i--) {
    const gchar *name = g_ptr_array_index(names, i);
    g_autofree gchar *day = g_strndup(name, 10);
    if (recent_day(day) && retained++ < 3) continue;
    if (unlinkat(dirfd, name, 0) < 0) {
      if (ok) g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                          "Could not rotate diagnostics: %s", g_strerror(errno));
      ok = FALSE;
    }
  }
  g_ptr_array_unref(names);
  return ok;
}

static gboolean
flush_cb(gpointer data)
{
  GhDiagnostics *self = data;
  self->flush_source = 0;
  if (!self->enabled || self->disk_failed) return G_SOURCE_REMOVE;
  prune_counts(self);
  g_autofree gchar *day = today_string();
  g_autoptr(GError) error = NULL;
  int dirfd = open_state(self, TRUE, &error);
  if (dirfd >= 0) {
    if (!persist_day(self, dirfd, day, &error) || !rotate_files(dirfd, &error))
      self->disk_failed = TRUE;
    close(dirfd);
  } else {
    self->disk_failed = TRUE;
  }
  if (self->disk_failed) {
    g_free(self->save_error);
    self->save_error = g_strdup("Diagnostics could not be saved");
    if (self->status_callback) self->status_callback(self, self->status_data);
  }
  return G_SOURCE_REMOVE;
}

static gint
index_of(const gchar *value, const gchar *const *table, guint n)
{
  for (guint i = 0; i < n; i++)
    if (g_str_equal(value, table[i])) return (gint)i;
  return -1;
}

static void
load_file(GhDiagnostics *self, int dirfd, const gchar *name)
{
  int fd = openat(dirfd, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  struct stat st;
  if (fd < 0 || fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) ||
      st.st_uid != getuid() || (st.st_mode & 077) || st.st_size > MAX_FILE_BYTES) {
    if (fd >= 0) close(fd);
    return;
  }
  g_autofree gchar *text = g_malloc((gsize)st.st_size + 1);
  gsize total = 0;
  while (total < (gsize)st.st_size) {
    ssize_t n = read(fd, text + total, (gsize)st.st_size - total);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    total += (gsize)n;
  }
  close(fd);
  if (total != (gsize)st.st_size) return;
  text[total] = '\0';
  if (!g_str_has_prefix(text, "schema\t1\n")) return;
  g_auto(GStrv) lines = g_strsplit(text + strlen("schema\t1\n"), "\n", -1);
  for (guint i = 0; lines[i] && g_hash_table_size(self->counts) < MAX_KEYS; i++) {
    g_auto(GStrv) fields = g_strsplit(lines[i], "\t", 5);
    if (g_strv_length(fields) != 5 || !recent_day(fields[0]) ||
        strncmp(name, fields[0], 10) != 0 ||
        index_of(fields[1], components, G_N_ELEMENTS(components)) < 0 ||
        index_of(fields[2], events, G_N_ELEMENTS(events)) < 0 ||
        index_of(fields[3], results, G_N_ELEMENTS(results)) < 0)
      continue;
    gchar *end = NULL;
    guint64 count = g_ascii_strtoull(fields[4], &end, 10);
    if (!*fields[4] || *end || count == 0 || count > G_MAXUINT32) continue;
    g_autofree gchar *key = g_strdup_printf("%s\t%s\t%s\t%s", fields[0], fields[1],
                                            fields[2], fields[3]);
    guint32 *value = g_new(guint32, 1);
    *value = (guint32)count;
    g_hash_table_replace(self->counts, g_steal_pointer(&key), value);
  }
}

static void
load_existing(GhDiagnostics *self)
{
  int dirfd = open_state(self, FALSE, NULL);
  if (dirfd < 0) return;
  DIR *dir = fdopendir(dup(dirfd));
  if (dir) {
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
      if (is_daily_file(entry->d_name)) load_file(self, dirfd, entry->d_name);
    closedir(dir);
  }
  close(dirfd);
}

static void
settings_changed(GSettings *settings, const gchar *key, GhDiagnostics *self)
{
  (void)key;
  gboolean enabled = g_settings_get_boolean(settings, "diagnostics-enabled");
  if (enabled == self->enabled) return;
  self->enabled = enabled;
  if (enabled) {
    load_existing(self);
  } else {
    if (self->flush_source) g_source_remove(self->flush_source);
    self->flush_source = 0;
    g_autoptr(GError) error = NULL;
    gh_diagnostics_clear(self, &error);
    g_free(self->delete_error);
    self->delete_error = error ? g_strdup("Diagnostics could not be deleted") : NULL;
    if (self->status_callback) self->status_callback(self, self->status_data);
  }
}

GhDiagnostics *
gh_diagnostics_new(GSettings *settings, const gchar *state_home)
{
  g_return_val_if_fail(G_IS_SETTINGS(settings), NULL);
  GhDiagnostics *self = g_new0(GhDiagnostics, 1);
  self->settings = g_object_ref(settings);
  self->state_home = g_strdup(state_home ? state_home : g_get_user_state_dir());
  self->counts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  self->enabled = g_settings_get_boolean(settings, "diagnostics-enabled");
  if (self->enabled) load_existing(self);
  g_signal_connect(settings, "changed::diagnostics-enabled", G_CALLBACK(settings_changed), self);
  return self;
}

void
gh_diagnostics_free(GhDiagnostics *self)
{
  if (!self) return;
  if (self->flush_source) {
    g_source_remove(self->flush_source);
    self->flush_source = 0;
    flush_cb(self);
  }
  g_signal_handlers_disconnect_by_func(self->settings, settings_changed, self);
  if (process_diagnostics == self) process_diagnostics = NULL;
  g_hash_table_unref(self->counts);
  g_object_unref(self->settings);
  g_free(self->state_home);
  g_free(self->save_error);
  g_free(self->delete_error);
  g_free(self);
}

GhDiagnostics *
gh_diagnostics_get_default(void)
{
  return process_diagnostics;
}

void
gh_diagnostics_set_default(GhDiagnostics *self)
{
  process_diagnostics = self;
}

void
gh_diagnostics_record(GhDiagnostics *self, GhDiagnosticComponent component,
                      GhDiagnosticEvent event, GhDiagnosticResult result)
{
  if (!self || !self->enabled || component < 0 || component >= GH_DIAGNOSTIC_COMPONENT_N ||
      event < 0 || event >= GH_DIAGNOSTIC_EVENT_N ||
      result < 0 || result >= GH_DIAGNOSTIC_RESULT_N)
    return;
  prune_counts(self);
  g_autofree gchar *day = today_string();
  g_autofree gchar *key = g_strdup_printf("%s\t%s\t%s\t%s", day,
    components[component], events[event], results[result]);
  guint32 *count = g_hash_table_lookup(self->counts, key);
  if (!count) {
    if (g_hash_table_size(self->counts) >= MAX_KEYS) return;
    count = g_new0(guint32, 1);
    g_hash_table_insert(self->counts, g_steal_pointer(&key), count);
  }
  if (*count < G_MAXUINT32) ++*count;
  if (!self->disk_failed && !self->flush_source)
    self->flush_source = g_idle_add_full(G_PRIORITY_LOW, flush_cb, self, NULL);
}

void
gh_diagnostics_record_default(GhDiagnosticComponent component,
                              GhDiagnosticEvent event, GhDiagnosticResult result)
{
  gh_diagnostics_record(process_diagnostics, component, event, result);
}

gchar *
gh_diagnostics_snapshot(GhDiagnostics *self)
{
  g_return_val_if_fail(self != NULL, NULL);
  prune_counts(self);
  GList *keys = g_hash_table_get_keys(self->counts);
  keys = g_list_sort(keys, compare_keys);
  GString *report = g_string_new("Groundhog local diagnostics (schema 1; daily counts only)\n"
                                  "day\tcomponent\tevent\tresult\tcount\n");
  for (GList *l = keys; l; l = l->next) {
    const gchar *key = l->data;
    guint32 *count = g_hash_table_lookup(self->counts, key);
    g_autofree gchar *line = g_strdup_printf("%s\t%u\n", key, *count);
    if (report->len + strlen(line) > MAX_REPORT_BYTES) break;
    g_string_append(report, line);
  }
  g_list_free(keys);
  return g_string_free(report, FALSE);
}

gboolean
gh_diagnostics_clear(GhDiagnostics *self, GError **error)
{
  g_return_val_if_fail(self != NULL, FALSE);
  if (self->flush_source) g_source_remove(self->flush_source);
  self->flush_source = 0;
  g_hash_table_remove_all(self->counts);
  int dirfd = open_state(self, FALSE, error);
  if (dirfd < 0) return !error || !*error; /* no directory means nothing to delete */
  gboolean ok = remove_files(dirfd, error);
  close(dirfd);
  return ok;
}

const gchar *
gh_diagnostics_get_save_error(GhDiagnostics *self)
{
  return self ? self->save_error : NULL;
}

const gchar *
gh_diagnostics_get_delete_error(GhDiagnostics *self)
{
  return self ? self->delete_error : NULL;
}

void
gh_diagnostics_set_status_callback(GhDiagnostics *self,
                                   GhDiagnosticsStatusFunc callback, gpointer data)
{
  if (!self) return;
  self->status_callback = callback;
  self->status_data = data;
}
