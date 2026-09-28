#include "crash-harness.h"

#include "gh-store.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

GhCrashOutcome
gh_crash_harness_run(const gchar *cut_point, guint nth, GhCrashScript script,
                     gpointer user_data)
{
  g_return_val_if_fail(cut_point != NULL, GH_CRASH_FAILED);
  g_return_val_if_fail(script != NULL, GH_CRASH_FAILED);

  /* Buffered output would otherwise be written twice. */
  fflush(stdout);
  fflush(stderr);
  pid_t pid = fork();
  if (pid < 0)
    g_error("fork() failed: %s", g_strerror(errno));
  if (pid == 0) {
    gh_store_test_crash_at(cut_point, nth);
    script(user_data);
    /* Never unwind into the test runner in the child. */
    _exit(0);
  }

  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR)
      g_error("waitpid() failed: %s", g_strerror(errno));
  }
  if (WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL)
    return GH_CRASH_KILLED;
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
    return GH_CRASH_COMPLETED;
  return GH_CRASH_FAILED;
}

const gchar *
gh_crash_outcome_to_string(GhCrashOutcome outcome)
{
  switch (outcome) {
  case GH_CRASH_KILLED:
    return "killed at the cut point";
  case GH_CRASH_COMPLETED:
    return "completed without reaching the cut point";
  case GH_CRASH_FAILED:
  default:
    return "failed before the cut point";
  }
}
