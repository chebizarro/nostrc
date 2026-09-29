/* H7 canary scanner and PT-10 log capture; see canary-scan.h. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* memmem */
#endif
#include "canary-scan.h"

#include <errno.h>
#include <fcntl.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
  gchar *label;
  const gchar *encoding;
  GBytes *bytes;
} Pattern;

struct _CanaryScan {
  GPtrArray *patterns; /* Pattern */
  GPtrArray *hits;     /* gchar * */
};

static void
pattern_free(gpointer data)
{
  Pattern *pattern = data;
  g_free(pattern->label);
  g_bytes_unref(pattern->bytes);
  g_free(pattern);
}

static void
add_pattern(CanaryScan *scan, const gchar *label, const gchar *encoding, gconstpointer data,
            gsize length)
{
  Pattern *pattern = g_new0(Pattern, 1);
  pattern->label = g_strdup(label);
  pattern->encoding = encoding;
  pattern->bytes = g_bytes_new(data, length);
  g_ptr_array_add(scan->patterns, pattern);
}

CanaryScan *
canary_scan_new(void)
{
  CanaryScan *scan = g_new0(CanaryScan, 1);
  scan->patterns = g_ptr_array_new_with_free_func(pattern_free);
  scan->hits = g_ptr_array_new_with_free_func(g_free);
  return scan;
}

void
canary_scan_free(CanaryScan *scan)
{
  if (!scan)
    return;
  g_ptr_array_unref(scan->patterns);
  g_ptr_array_unref(scan->hits);
  g_free(scan);
}

void
canary_scan_add_literal(CanaryScan *scan, const gchar *label, const gchar *needle)
{
  g_return_if_fail(scan && label && needle && strlen(needle) >= 8);
  add_pattern(scan, label, "raw", needle, strlen(needle));
}

static gchar *
hex_of(const guint8 *data, gsize length, gboolean upper)
{
  static const gchar lower_digits[] = "0123456789abcdef", upper_digits[] = "0123456789ABCDEF";
  const gchar *digits = upper ? upper_digits : lower_digits;
  gchar *out = g_malloc(length * 2 + 1);
  for (gsize i = 0; i < length; i++) {
    out[2 * i] = digits[data[i] >> 4];
    out[2 * i + 1] = digits[data[i] & 0xf];
  }
  out[length * 2] = '\0';
  return out;
}

void
canary_scan_add(CanaryScan *scan, const gchar *label, const gchar *needle)
{
  g_return_if_fail(scan && label && needle);
  gsize length = strlen(needle);
  g_return_if_fail(length >= 8);
  const guint8 *bytes = (const guint8 *)needle;
  add_pattern(scan, label, "raw", needle, length);
  g_autofree gchar *lower = hex_of(bytes, length, FALSE);
  add_pattern(scan, label, "hex", lower, strlen(lower));
  g_autofree gchar *upper = hex_of(bytes, length, TRUE);
  add_pattern(scan, label, "HEX", upper, strlen(upper));
  /* Base64 of the needle starting at each alignment, cut to whole 3-byte
   * groups: whatever precedes it in a longer base64 text, one of these three
   * appears verbatim. */
  static const gchar *const alignments[] = { "base64+0", "base64+1", "base64+2" };
  for (gsize skip = 0; skip < 3; skip++) {
    gsize whole = (length - skip) / 3 * 3;
    if (whole < 6)
      continue;
    g_autofree gchar *b64 = g_base64_encode(bytes + skip, whole);
    add_pattern(scan, label, alignments[skip], b64, strlen(b64));
  }
  g_autofree guint8 *utf16 = g_malloc(length * 2);
  for (gsize i = 0; i < length; i++) {
    utf16[2 * i] = bytes[i];
    utf16[2 * i + 1] = 0;
  }
  add_pattern(scan, label, "utf-16le", utf16, length * 2);
}

guint
canary_scan_bytes(CanaryScan *scan, const gchar *source, gconstpointer data, gsize length)
{
  g_return_val_if_fail(scan && source, 0);
  guint found = 0;
  const guint8 *base = data;
  for (guint p = 0; p < scan->patterns->len; p++) {
    Pattern *pattern = g_ptr_array_index(scan->patterns, p);
    gsize needle_length = 0;
    const guint8 *needle = g_bytes_get_data(pattern->bytes, &needle_length);
    gsize offset = 0;
    while (base && offset + needle_length <= length) {
      const guint8 *hit = memmem(base + offset, length - offset, needle, needle_length);
      if (!hit)
        break;
      gsize at = (gsize)(hit - base);
      g_ptr_array_add(scan->hits, g_strdup_printf("%s: %s as %s at byte %" G_GSIZE_FORMAT,
                                                  source, pattern->label, pattern->encoding,
                                                  at));
      found++;
      offset = at + 1;
    }
  }
  return found;
}

guint
canary_scan_text(CanaryScan *scan, const gchar *source, const gchar *text)
{
  return text ? canary_scan_bytes(scan, source, text, strlen(text)) : 0;
}

guint
canary_scan_file(CanaryScan *scan, const gchar *path)
{
  g_autofree gchar *contents = NULL;
  gsize length = 0;
  g_autoptr(GError) error = NULL;
  if (!g_file_get_contents(path, &contents, &length, &error))
    g_error("canary scan cannot read %s: %s", path, error->message);
  return canary_scan_bytes(scan, path, contents, length);
}

guint
canary_scan_tree(CanaryScan *scan, const gchar *root, guint *n_files)
{
  guint found = 0;
  GStatBuf st;
  if (g_lstat(root, &st) != 0)
    return 0;
  if (S_ISLNK(st.st_mode)) {
    g_ptr_array_add(scan->hits, g_strdup_printf("%s: a symlink (never followed)", root));
    return 1;
  }
  if (S_ISREG(st.st_mode)) {
    if (n_files)
      (*n_files)++;
    return canary_scan_file(scan, root);
  }
  if (!S_ISDIR(st.st_mode))
    return 0; /* sockets and FIFOs hold no data at rest */
  GDir *dir = g_dir_open(root, 0, NULL);
  if (!dir)
    g_error("canary scan cannot list %s", root);
  const gchar *name;
  while ((name = g_dir_read_name(dir))) {
    g_autofree gchar *child = g_build_filename(root, name, NULL);
    found += canary_scan_tree(scan, child, n_files);
  }
  g_dir_close(dir);
  return found;
}

guint
canary_scan_settings(CanaryScan *scan, const gchar *source, GSettings *settings)
{
  g_autoptr(GSettingsSchema) schema = NULL;
  g_object_get(settings, "settings-schema", &schema, NULL);
  g_auto(GStrv) keys = g_settings_schema_list_keys(schema);
  g_autoptr(GString) dump = g_string_new(NULL);
  for (guint i = 0; keys[i]; i++) {
    g_autoptr(GVariant) value = g_settings_get_value(settings, keys[i]);
    g_autofree gchar *printed = g_variant_print(value, TRUE);
    g_string_append_printf(dump, "%s=%s\n", keys[i], printed);
  }
  return canary_scan_bytes(scan, source, dump->str, dump->len);
}

GPtrArray *
canary_scan_get_hits(CanaryScan *scan)
{
  return scan->hits;
}

void
canary_scan_clear_hits(CanaryScan *scan)
{
  g_ptr_array_set_size(scan->hits, 0);
}

gboolean
canary_scan_check_clean(CanaryScan *scan, const gchar *what)
{
  if (scan->hits->len == 0)
    return TRUE;
  g_printerr("H7: %u canary hit(s) in %s:\n", scan->hits->len, what);
  for (guint i = 0; i < scan->hits->len && i < 64; i++)
    g_printerr("  %s\n", (const gchar *)g_ptr_array_index(scan->hits, i));
  g_test_message("H7: %u canary hit(s) in %s, first: %s", scan->hits->len, what,
                 (const gchar *)g_ptr_array_index(scan->hits, 0));
  g_test_fail();
  return FALSE;
}

/* ---- log capture --------------------------------------------------------------- */

#define SYNC_MARK '\x1e'
#define SYNC_PREFIX "canary-sync:"

typedef struct {
  int fd;        /* 1 or 2, now the write end of the pipe */
  int original;  /* where that fd pointed before */
  int pipe_read;
  guint64 seen;  /* last sync marker read */
  GThread *thread;
} Tee;

static GMutex capture_mutex;
static GCond capture_cond;
static GString *captured;
static Tee tees[2];
static guint64 sync_serial;
static gboolean installed;

static void
write_all(int fd, const gchar *data, gsize length)
{
  while (length > 0) {
    ssize_t n = write(fd, data, length);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return;
    }
    data += n;
    length -= (gsize)n;
  }
}

static void
capture_append(const gchar *data, gsize length)
{
  g_mutex_lock(&capture_mutex);
  g_string_append_len(captured, data, (gssize)length);
  g_mutex_unlock(&capture_mutex);
}

/* Forwards and keeps plain output; consumes sync markers. Returns how many
 * bytes of pending were handled (the rest waits for more input). */
static gsize
tee_consume(Tee *tee, const gchar *pending, gsize length)
{
  gsize done = 0;
  while (done < length) {
    const gchar *mark = memchr(pending + done, SYNC_MARK, length - done);
    gsize plain = mark ? (gsize)(mark - (pending + done)) : length - done;
    if (plain > 0) {
      capture_append(pending + done, plain);
      write_all(tee->original, pending + done, plain);
      done += plain;
    }
    if (!mark)
      break;
    const gchar *end = memchr(mark + 1, SYNC_MARK, length - (gsize)(mark + 1 - pending));
    gsize prefix = strlen(SYNC_PREFIX);
    gsize available = length - (gsize)(mark + 1 - pending);
    if (available < prefix) {
      if (memcmp(mark + 1, SYNC_PREFIX, available) == 0)
        return done; /* a marker may be arriving */
    } else if (memcmp(mark + 1, SYNC_PREFIX, prefix) == 0) {
      if (!end)
        return done;
      g_autofree gchar *serial = g_strndup(mark + 1 + prefix, (gsize)(end - (mark + 1 + prefix)));
      g_mutex_lock(&capture_mutex);
      tee->seen = g_ascii_strtoull(serial, NULL, 10);
      g_cond_broadcast(&capture_cond);
      g_mutex_unlock(&capture_mutex);
      done = (gsize)(end + 1 - pending);
      continue;
    }
    /* A stray separator: plain output. */
    capture_append(mark, 1);
    write_all(tee->original, mark, 1);
    done++;
  }
  return done;
}

static gpointer
tee_thread(gpointer data)
{
  Tee *tee = data;
  g_autoptr(GString) pending = g_string_new(NULL);
  gchar buffer[4096];
  for (;;) {
    ssize_t n = read(tee->pipe_read, buffer, sizeof buffer);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      break;
    g_string_append_len(pending, buffer, n);
    gsize done = tee_consume(tee, pending->str, pending->len);
    g_string_erase(pending, 0, (gssize)done);
  }
  if (pending->len > 0) {
    capture_append(pending->str, pending->len);
    write_all(tee->original, pending->str, pending->len);
  }
  close(tee->pipe_read);
  return NULL;
}

/* GLib's own printing (TAP lines, g_printerr) is kept and written at once to
 * the real descriptor, so nothing is lost if the test aborts right after. */
static void
print_handler(const gchar *string)
{
  capture_append(string, strlen(string));
  write_all(tees[0].original, string, strlen(string));
}

static void
printerr_handler(const gchar *string)
{
  capture_append(string, strlen(string));
  write_all(tees[1].original, string, strlen(string));
}

static GLogWriterOutput
capture_writer(GLogLevelFlags level, const GLogField *fields, gsize n_fields, gpointer data)
{
  (void)data;
  g_autoptr(GString) line = g_string_new(NULL);
  for (gsize i = 0; i < n_fields; i++) {
    if (fields[i].length < 0)
      g_string_append_printf(line, "%s=%s ", fields[i].key, (const gchar *)fields[i].value);
    else
      g_string_append_printf(line, "%s=%.*s ", fields[i].key, (int)fields[i].length,
                             (const gchar *)fields[i].value);
  }
  g_string_append_c(line, '\n');
  capture_append(line->str, line->len);
  if (level & (G_LOG_LEVEL_DEBUG | G_LOG_LEVEL_INFO))
    return G_LOG_WRITER_HANDLED;
  /* Warnings and worse reach the real stderr at once. */
  g_autofree gchar *formatted = g_log_writer_format_fields(level, fields, n_fields, FALSE);
  write_all(tees[1].original, formatted, strlen(formatted));
  write_all(tees[1].original, "\n", 1);
  return G_LOG_WRITER_HANDLED;
}

void
canary_log_capture_install(void)
{
  g_return_if_fail(!installed);
  installed = TRUE;
  captured = g_string_new(NULL);
  fflush(stdout);
  fflush(stderr);
  for (int i = 0; i < 2; i++) {
    Tee *tee = &tees[i];
    int fds[2];
    g_autoptr(GError) error = NULL;
    if (!g_unix_open_pipe(fds, O_CLOEXEC, &error))
      g_error("canary log capture: %s", error->message);
    tee->fd = i + 1;
    tee->original = fcntl(tee->fd, F_DUPFD_CLOEXEC, 3);
    g_assert_cmpint(tee->original, >=, 0);
    g_assert_cmpint(dup2(fds[1], tee->fd), ==, tee->fd);
    close(fds[1]);
    tee->pipe_read = fds[0];
    tee->thread = g_thread_new(i == 0 ? "canary-stdout" : "canary-stderr", tee_thread, tee);
  }
  g_set_print_handler(print_handler);
  g_set_printerr_handler(printerr_handler);
  g_log_set_writer_func(capture_writer, NULL, NULL);
  /* g_test_init() prints every g_log() message itself; GLib's own default
   * handler hands each to the writer above instead. */
  g_log_set_default_handler(g_log_default_handler, NULL);
}

void
canary_log_capture_sync(void)
{
  g_return_if_fail(installed);
  fflush(stdout);
  fflush(stderr);
  g_mutex_lock(&capture_mutex);
  guint64 serial = ++sync_serial;
  g_mutex_unlock(&capture_mutex);
  g_autofree gchar *mark = g_strdup_printf("%c" SYNC_PREFIX "%" G_GUINT64_FORMAT "%c",
                                           SYNC_MARK, serial, SYNC_MARK);
  for (int i = 0; i < 2; i++)
    write_all(tees[i].fd, mark, strlen(mark));
  gint64 deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
  g_mutex_lock(&capture_mutex);
  while (tees[0].seen < serial || tees[1].seen < serial) {
    if (!g_cond_wait_until(&capture_cond, &capture_mutex, deadline)) {
      g_mutex_unlock(&capture_mutex);
      g_error("canary log capture: output was not drained within 10 s");
    }
  }
  g_mutex_unlock(&capture_mutex);
}

void
canary_log_capture_uninstall(void)
{
  g_return_if_fail(installed);
  canary_log_capture_sync();
  g_set_print_handler(NULL);
  g_set_printerr_handler(NULL);
  for (int i = 0; i < 2; i++) {
    /* The pipe's only write end goes: the reader sees EOF and ends. */
    g_assert_cmpint(dup2(tees[i].original, tees[i].fd), ==, tees[i].fd);
    g_thread_join(g_steal_pointer(&tees[i].thread));
  }
  /* GLib keeps this writer (it cannot be replaced); it still keeps what is
   * logged and prints warnings to the real stderr. */
}

gchar *
canary_log_capture_dup(void)
{
  g_return_val_if_fail(installed, NULL);
  g_mutex_lock(&capture_mutex);
  gchar *copy = g_strndup(captured->str, captured->len);
  g_mutex_unlock(&capture_mutex);
  return copy;
}

void
canary_log_capture_reset(void)
{
  g_return_if_fail(installed);
  g_mutex_lock(&capture_mutex);
  g_string_truncate(captured, 0);
  g_mutex_unlock(&capture_mutex);
}
