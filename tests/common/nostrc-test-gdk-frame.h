/* Tolerate GTK 4.24's harmless "gdk_frame_timings_presented() called on
 * skipped frame" warning on macOS (nostrc-ykxf). GTK's macOS backend began
 * logging this in 4.24.1; under g_test_init()'s fatal-warnings policy it
 * becomes a SIGTRAP. The warning is a timing artefact of the Quartz frame
 * clock, not a code defect: the code under test never causes it and cannot
 * prevent it. Linux (Wayland / X11) and the CI image (GTK 4.14) are
 * unaffected.
 *
 * GLib 2.90's structured logging path (g_warning() → g_log_structured_array())
 * does NOT consult g_test_log_set_fatal_handler before aborting, so the
 * nip29-relay.h two-hook pattern (fatal handler + default handler) does not
 * work here. Instead:
 *
 *   1. Remove G_LOG_LEVEL_WARNING from g_log_always_fatal (keep CRITICAL),
 *      so the structured path no longer OR's G_LOG_FLAG_FATAL into warnings.
 *
 *   2. Install a g_log_set_writer_func that re-enforces fatal-warnings for
 *      every warning EXCEPT the forgiven one: non-forgiven warnings print
 *      with G_LOG_FLAG_FATAL and abort(); the forgiven message prints as a
 *      plain warning and the process continues.
 *
 * Usage: call nostrc_test_tolerate_gdk_frame_warning() once after g_test_init()
 * in every GUI test that presents a window. Header-only; include once per
 * test executable.
 *
 * Pattern origin: gnome/groundhog/tests/nip29/nip29-relay.h (the libsoup
 * accept race, nostrc-79mi), adapted for GLib 2.90's structured logger. */
#ifndef NOSTRC_TEST_GDK_FRAME_H
#define NOSTRC_TEST_GDK_FRAME_H

#include <glib.h>
#include <stdlib.h>
#include <string.h>

/* The exact domain and message text GTK 4.24's macOS backend logs.
 * GTK may or may not append a trailing period; match both forms with
 * two strcmp comparisons — no substring matching. */
#define NOSTRC_TEST_GDK_FRAME_DOMAIN "Gdk"
#define NOSTRC_TEST_GDK_FRAME_MESSAGE "gdk_frame_timings_presented() called on skipped frame"

static G_GNUC_UNUSED gboolean
nostrc_test_is_gdk_frame_warning(const gchar *domain, GLogLevelFlags level, const gchar *message)
{
  return (level & G_LOG_LEVEL_WARNING) && g_strcmp0(domain, NOSTRC_TEST_GDK_FRAME_DOMAIN) == 0 &&
         message != NULL &&
         (strcmp(message, NOSTRC_TEST_GDK_FRAME_MESSAGE) == 0 ||
          strcmp(message, NOSTRC_TEST_GDK_FRAME_MESSAGE ".") == 0);
}

/* The writer function: called for every structured and legacy log message.
 * For the forgiven Gdk frame warning: strip G_LOG_FLAG_FATAL and log
 * normally (no abort). For any other warning: log with G_LOG_FLAG_FATAL
 * and abort() (re-enforcing the fatal-warnings policy that g_test_init()
 * would have applied). Everything else passes through unchanged. */
static GLogWriterOutput
nostrc_test_gdk_frame_writer(GLogLevelFlags level, const GLogField *fields, gsize n_fields,
                             gpointer data)
{
  if (level & G_LOG_LEVEL_WARNING) {
    const gchar *domain = NULL;
    const gchar *message = NULL;
    for (gsize i = 0; i < n_fields; i++) {
      if (g_strcmp0(fields[i].key, "GLIB_DOMAIN") == 0)
        domain = fields[i].value;
      else if (g_strcmp0(fields[i].key, "MESSAGE") == 0)
        message = fields[i].value;
    }
    if (nostrc_test_is_gdk_frame_warning(domain, level, message))
      return g_log_writer_default(level & ~G_LOG_FLAG_FATAL, fields, n_fields, data);
    /* Any other warning: fatal, as g_test_init() intends. */
    g_log_writer_default(level | G_LOG_FLAG_FATAL, fields, n_fields, data);
    abort();
  }
  return g_log_writer_default(level, fields, n_fields, data);
}

/* Call once after g_test_init(). Safe to call more than once (idempotent). */
static G_GNUC_UNUSED void
nostrc_test_tolerate_gdk_frame_warning(void)
{
  static gsize installed;
  if (g_once_init_enter(&installed)) {
    /* Remove WARNING from always-fatal; keep CRITICAL and ERROR. The writer
     * above re-enforces fatal-warnings for every warning except the one. */
    g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);
    g_log_set_writer_func(nostrc_test_gdk_frame_writer, NULL, NULL);
    g_once_init_leave(&installed, 1);
  }
}

#endif
