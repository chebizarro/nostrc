#ifndef GH_CRASH_HARNESS_H
#define GH_CRASH_HARNESS_H

#include <glib.h>

G_BEGIN_DECLS

/* Charter H8 crash harness (no sleeps). gh_crash_harness_run() forks; the
 * child arms the store cut point @cut_point (gh_store_test_crash_at(), test
 * builds only) and runs @script, which opens the store itself and performs the
 * scripted operation. Reaching the cut point SIGKILLs the child mid-operation.
 * The parent waits for it and then reopens the store to check invariants.
 *
 * Rules for callers: close every GhStore before calling (a connection must
 * never cross fork()), keep the script free of test output, and treat any
 * outcome other than GH_CRASH_KILLED as a failure: COMPLETED means the cut
 * point was never reached, so the scenario tested nothing. */

typedef void (*GhCrashScript)(gpointer user_data);

typedef enum {
  GH_CRASH_KILLED,    /* SIGKILLed at the armed cut point */
  GH_CRASH_COMPLETED, /* the script returned without reaching the cut point */
  GH_CRASH_FAILED     /* any other exit: an assertion, another signal */
} GhCrashOutcome;

GhCrashOutcome gh_crash_harness_run(const gchar *cut_point, guint nth,
                                    GhCrashScript script, gpointer user_data);
const gchar *gh_crash_outcome_to_string(GhCrashOutcome outcome);

G_END_DECLS
#endif
