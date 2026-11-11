/* SPDX-License-Identifier: MIT */
#pragma once
#include "gh-call-capabilities.h"

/* One policy for direct binaries, CTest and the process-isolated harness.
 * Only absent baseline runtime may skip; failures after admission remain errors. */
static inline int gh_call_test_runtime_status(void)
{
  const gchar *required = g_getenv("GROUNDHOG_CALL_TEST_REQUIRE_RUNTIME");
  if (required && g_strcmp0(required, "0") && g_strcmp0(required, "1")) {
    g_printerr("GROUNDHOG_CALL_TEST_REQUIRE_RUNTIME must be 0 or 1\n");
    return 1;
  }
  GError *error = NULL;
  if (gh_call_capabilities_check(FALSE, &error)) return 0;
  gboolean hard_failure = !g_strcmp0(required, "1");
  g_printerr("%s: %s\n", hard_failure ? "REQUIRED RUNTIME MISSING" : "SKIP", error->message);
  g_clear_error(&error);
  return hard_failure ? 1 : 77;
}
