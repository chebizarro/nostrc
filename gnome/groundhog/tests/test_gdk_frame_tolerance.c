/* Test that nostrc-test-gdk-frame.h tolerates exactly the GTK 4.24
 * skipped-frame warning and nothing else (nostrc-ykxf). Two subprocess checks:
 *
 *   1. The forgiven message: a synthetic Gdk-WARNING with the exact text
 *      GTK 4.24 logs does NOT abort. The subprocess exits 0.
 *
 *   2. Any other Gdk-WARNING: a different message DOES abort. The subprocess
 *      is killed by abort() in the writer (exit != 0).
 *
 * The parent uses g_test_trap_subprocess() to run each child path and assert
 * the exit condition. No display needed. */
#include "nostrc-test-gdk-frame.h"

/* --- subprocess entry points -------------------------------------------- */

static void
subprocess_forgiven(void)
{
  nostrc_test_tolerate_gdk_frame_warning();
  /* Emit the exact warning GTK 4.24's macOS backend logs, through both
   * the structured path (g_log_structured, as g_warning() would) and the
   * legacy path (g_log). Both must survive.
   * Test both the period and no-period forms. */
  g_log_structured("Gdk", G_LOG_LEVEL_WARNING,
                   "MESSAGE", "gdk_frame_timings_presented() called on skipped frame.");
  g_log_structured("Gdk", G_LOG_LEVEL_WARNING,
                   "MESSAGE", "gdk_frame_timings_presented() called on skipped frame");
  g_log("Gdk", G_LOG_LEVEL_WARNING,
        "gdk_frame_timings_presented() called on skipped frame.");
  g_log("Gdk", G_LOG_LEVEL_WARNING,
        "gdk_frame_timings_presented() called on skipped frame");
  /* If the tolerance works, we get here and exit normally. */
}

static void
subprocess_other_warning(void)
{
  nostrc_test_tolerate_gdk_frame_warning();
  /* A different Gdk warning must still be fatal. Use the structured path
   * (g_log_structured) since that is the path g_warning() takes. */
  g_log_structured("Gdk", G_LOG_LEVEL_WARNING,
                   "MESSAGE", "something else entirely");
  /* Should never reach here. */
  g_assert_not_reached();
}

/* --- parent test cases -------------------------------------------------- */

static void
test_forgiven_does_not_abort(void)
{
  g_test_trap_subprocess("/gdk-frame-tolerance/forgiven/subprocess", 0,
                         G_TEST_SUBPROCESS_DEFAULT);
  g_test_trap_assert_passed();
}

static void
test_other_warning_does_abort(void)
{
  g_test_trap_subprocess("/gdk-frame-tolerance/other-aborts/subprocess", 0,
                         G_TEST_SUBPROCESS_DEFAULT);
  g_test_trap_assert_failed();
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/gdk-frame-tolerance/forgiven", test_forgiven_does_not_abort);
  g_test_add_func("/gdk-frame-tolerance/forgiven/subprocess", subprocess_forgiven);
  g_test_add_func("/gdk-frame-tolerance/other-aborts", test_other_warning_does_abort);
  g_test_add_func("/gdk-frame-tolerance/other-aborts/subprocess", subprocess_other_warning);
  return g_test_run();
}
