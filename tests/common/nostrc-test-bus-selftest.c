/* nostrc-test-bus-selftest.c — the macOS EBADF tolerance of
 * nostrc-test-bus.h, as the tests that use it see it.
 *
 * GTest clears g_test_log_set_fatal_handler() before every test case, so the
 * tolerance must be armed per case (W13 review, non-blocking #1). Here one
 * bus is up before g_test_run(), as the header recommends, and the warning
 * GLib's select()-based poll logs on macOS is emitted directly (same domain
 * and text), so every case is deterministic:
 *
 *  - macOS: two consecutive cases added with nostrc_test_bus_add_func() each
 *    survive one; two cases that each bring a bus up themselves survive one
 *    too; a case survives NOSTRC_TEST_BUS_EBADF_TOLERANCE of them, and one
 *    more aborts; any other message stays fatal.
 *  - elsewhere: the warning stays fatal (Linux poll() never logs it).
 *
 * Aborting cases run in a g_test_trap_subprocess() child.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "nostrc-test-bus.h"

#define EBADF_MESSAGE "poll(2) failed due to: Bad file descriptor."

static void
emit_ebadf(guint count)
{
  for (guint i = 0; i < count; i++)
    g_log("GLib", G_LOG_LEVEL_WARNING, "%s", EBADF_MESSAGE);
}

/* Runs the current test path in a child and expects it to abort on a
 * warning carrying needle. */
static void
assert_child_aborts(const gchar *needle)
{
  g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
  g_test_trap_assert_failed();
  g_autofree gchar *pattern = g_strdup_printf("*%s*", needle);
  g_test_trap_assert_stderr(pattern);
}

#ifdef __APPLE__
static void
test_first_case(void)
{
  emit_ebadf(1);
}

/* The review's failing probe: the second case after a bus that came up
 * before g_test_run(). */
static void
test_second_case(void)
{
  emit_ebadf(1);
}

/* A bus brought up inside the case arms the case itself, in every case. */
static void
test_bus_in_case(void)
{
  NostrcTestBus *bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NOT_SESSION);
  nostrc_test_bus_up(bus);
  emit_ebadf(1);
  nostrc_test_bus_down(bus);
}

/* Bounded per case: the limit passes, one more aborts. */
static void
test_bound(void)
{
  if (g_test_subprocess()) {
    emit_ebadf(NOSTRC_TEST_BUS_EBADF_TOLERANCE + 1);
    return;
  }
  emit_ebadf(NOSTRC_TEST_BUS_EBADF_TOLERANCE);
  assert_child_aborts("Bad file descriptor");
}

/* Exactly that GLib warning: another poll error, or the same text from
 * another domain, is fatal. */
static void
test_other_warning(void)
{
  if (g_test_subprocess()) {
    g_log("GLib", G_LOG_LEVEL_WARNING, "poll(2) failed due to: Invalid argument.");
    return;
  }
  assert_child_aborts("Invalid argument");
}

static void
test_other_domain(void)
{
  if (g_test_subprocess()) {
    g_log("GLib-GIO", G_LOG_LEVEL_WARNING, "%s", EBADF_MESSAGE);
    return;
  }
  assert_child_aborts("Bad file descriptor");
}
#else
/* Nothing is forgiven off macOS. */
static void
test_fatal_elsewhere(void)
{
  if (g_test_subprocess()) {
    nostrc_test_bus_tolerate_ebadf();
    emit_ebadf(1);
    return;
  }
  assert_child_aborts("Bad file descriptor");
}
#endif

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  if (!nostrc_test_bus_available()) {
    g_printerr("nostrc-test-bus-selftest needs dbus-daemon\n");
    return 1;
  }
  NostrcTestBus *bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NONE);
  nostrc_test_bus_up(bus);
#ifdef __APPLE__
  nostrc_test_bus_add_func("/nostrc-test-bus/ebadf/first-case", test_first_case);
  nostrc_test_bus_add_func("/nostrc-test-bus/ebadf/second-case", test_second_case);
  g_test_add_func("/nostrc-test-bus/ebadf/bus-in-case", test_bus_in_case);
  g_test_add_func("/nostrc-test-bus/ebadf/bus-in-another-case", test_bus_in_case);
  nostrc_test_bus_add_func("/nostrc-test-bus/ebadf/bound", test_bound);
  nostrc_test_bus_add_func("/nostrc-test-bus/ebadf/other-warning", test_other_warning);
  nostrc_test_bus_add_func("/nostrc-test-bus/ebadf/other-domain", test_other_domain);
#else
  nostrc_test_bus_add_func("/nostrc-test-bus/ebadf/fatal-elsewhere", test_fatal_elsewhere);
#endif
  int status = g_test_run();
  nostrc_test_bus_down(bus);
  return status;
}
