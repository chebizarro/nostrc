/* H7 (privacy charter §9.1): the canary scanner of the G24 privacy harness.
 *
 * A test seeds unique canaries (message plaintext, a fake nsec, a fake
 * bunker URI, ...) into the system under test, then asks the scanner whether
 * any of them surfaced where the charter forbids plaintext: files under the
 * test's XDG and temporary directories (raw bytes, so SQLite -wal/-shm
 * files, journals and caches count), GSettings dumps, captured logs, desktop
 * notification payloads and relay traffic.
 *
 * Encodings. A plaintext leak need not be verbatim. Each needle added with
 * canary_scan_add() is also searched for as lowercase and uppercase hex, as
 * base64 at each of the three byte alignments (so it is found inside any
 * longer base64 text), and as UTF-16LE. canary_scan_add_literal() searches
 * the exact bytes only (for needles whose re-encodings would match
 * unrelated data). Every hit is kept with its source, needle label, encoding
 * and byte offset; a scan never stops at the first.
 *
 * Log capture (PT-10). canary_log_capture_install() routes every GLib log
 * message (any domain, any level, debug included) through a structured
 * writer that keeps its fields, and tees the process's stdout and stderr
 * file descriptors through pipes, so output of libraries that print directly
 * (libwebsockets, libnostr) is kept too. Warnings and worse still reach the
 * real stderr, and everything written to stdout/stderr is forwarded. Install
 * it once, right after g_test_init() (whose own default log handler it
 * replaces with GLib's, which feeds the writer).
 *
 * Main thread only, except that the log capture accepts messages from any
 * thread. */
#ifndef GH_TEST_CANARY_SCAN_H
#define GH_TEST_CANARY_SCAN_H

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _CanaryScan CanaryScan;

CanaryScan *canary_scan_new(void);
void canary_scan_free(CanaryScan *scan);

/* A needle (at least 8 bytes) that must not appear, in any encoding above. */
void canary_scan_add(CanaryScan *scan, const gchar *label, const gchar *needle);
/* A binary needle (at least 8 bytes, e.g. a key), in every encoding above. */
void canary_scan_add_bytes(CanaryScan *scan, const gchar *label, gconstpointer needle,
                           gsize length);
/* A needle searched for as its exact bytes only. */
void canary_scan_add_literal(CanaryScan *scan, const gchar *label, const gchar *needle);

/* Each scan returns the hits it added. source names what was scanned. */
guint canary_scan_bytes(CanaryScan *scan, const gchar *source, gconstpointer data,
                        gsize length);
guint canary_scan_text(CanaryScan *scan, const gchar *source, const gchar *text);
/* Raw bytes of one file (it must be readable). */
guint canary_scan_file(CanaryScan *scan, const gchar *path);
/* Every regular file below root, recursively; symlinks are neither followed
 * nor read (a symlink is itself reported as a hit: nothing Groundhog writes
 * is one). *n_files (nullable) receives how many files were read. A missing
 * root scans nothing. */
guint canary_scan_tree(CanaryScan *scan, const gchar *root, guint *n_files);
/* Every key of the settings' schema, printed (the memory backend's dump). */
guint canary_scan_settings(CanaryScan *scan, const gchar *source, GSettings *settings);

/* "source: needle-label as encoding at byte N", oldest first. Borrowed. */
GPtrArray *canary_scan_get_hits(CanaryScan *scan);
void canary_scan_clear_hits(CanaryScan *scan);
/* Prints every hit and fails the test (g_test_fail()) if there is any;
 * TRUE when clean. what names the check in the message. */
gboolean canary_scan_check_clean(CanaryScan *scan, const gchar *what);

/* ---- log capture (PT-10) ------------------------------------------------------- */

void canary_log_capture_install(void);
/* Restores stdout and stderr (call before exit, so that a sanitizer's final
 * report reaches the real stderr). GLib messages are still kept. */
void canary_log_capture_uninstall(void);
/* Waits until everything written to stdout/stderr so far was captured. */
void canary_log_capture_sync(void);
/* A copy of what was captured since the last reset (after a sync). */
gchar *canary_log_capture_dup(void);
void canary_log_capture_reset(void);

G_END_DECLS
#endif
