/* SPDX-License-Identifier: MIT */
#include "../src/memory_hardening.h"
#include "test_check.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

typedef struct {
  struct rlimit limit;
  int get_error, set_error, full_error, current_error;
  int gets, sets, locks;
  int flags[2];
} Fake;

static int fake_get(void *data, struct rlimit *limit) {
  Fake *f = data;
  f->gets++;
  if (f->get_error) { errno = f->get_error; return -1; }
  *limit = f->limit;
  return 0;
}

static int fake_set(void *data, const struct rlimit *limit) {
  Fake *f = data;
  f->sets++;
  CHECK(limit->rlim_max == f->limit.rlim_max);
  CHECK(limit->rlim_cur == limit->rlim_max);
  if (f->set_error) { errno = f->set_error; return -1; }
  f->limit = *limit;
  return 0;
}

static int fake_lock(void *data, int flags) {
  Fake *f = data;
  CHECK(f->locks < 2);
  f->flags[f->locks++] = flags;
  int error = flags & MCL_FUTURE ? f->full_error : f->current_error;
  if (error) { errno = error; return -1; }
  return 0;
}

static void run(const char *mode, Fake *f, int rc, const char *diagnostic,
                int gets, int sets, int locks, int first_flags) {
  SignetMemoryOps ops = {fake_get, fake_set, fake_lock, f};
  char message[512];
  CHECK(signet_memory_harden(mode, &ops, message, sizeof(message)) == rc);
  CHECK(strstr(message, diagnostic) != NULL);
  CHECK(f->gets == gets);
  CHECK(f->sets == sets);
  CHECK(f->locks == locks);
  if (locks) CHECK(f->flags[0] == first_flags);
  if (locks == 2) CHECK(f->flags[1] == MCL_CURRENT);
  /* Diagnostics remain terminated even in a tiny caller buffer. */
  char tiny[2] = {'x', 'x'};
  f->gets = f->sets = f->locks = 0;
  (void)signet_memory_harden(mode, &ops, tiny, sizeof(tiny));
  CHECK(tiny[1] == '\0');
}

int main(void) {
  CHECK(signet_mlock_mode_parse(NULL) == SIGNET_MLOCK_MODE_BEST_EFFORT);
  CHECK(signet_mlock_mode_parse("") == SIGNET_MLOCK_MODE_BEST_EFFORT);
  CHECK(signet_mlock_mode_parse("BEST-EFFORT") == SIGNET_MLOCK_MODE_BEST_EFFORT);
  CHECK(signet_mlock_mode_parse("Required") == SIGNET_MLOCK_MODE_REQUIRED);
  CHECK(signet_mlock_mode_parse("OFF") == SIGNET_MLOCK_MODE_OFF);
  const char *invalid[] = {"bogus", "future", "current", "full", "false", "0", " required"};
  for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
    CHECK(signet_mlock_mode_parse(invalid[i]) == SIGNET_MLOCK_MODE_INVALID);
    Fake f = {0};
    run(invalid[i], &f, -1, "invalid SIGNET_MLOCK_MODE", 0, 0, 0, 0);
  }
  Fake f = {0};
  run("off", &f, 0, "disabled", 0, 0, 0, 0);

  f = (Fake){.limit = {RLIM_INFINITY, RLIM_INFINITY}};
  run(NULL, &f, 0, "active: MCL_CURRENT|MCL_FUTURE", 1, 0, 1, MCL_CURRENT|MCL_FUTURE);
  f = (Fake){.limit = {RLIM_INFINITY, RLIM_INFINITY}};
  run("required", &f, 0, "active:", 1, 0, 1, MCL_CURRENT|MCL_FUTURE);
  f = (Fake){.limit = {65536, RLIM_INFINITY}};
  run("required", &f, 0, "active:", 1, 1, 1, MCL_CURRENT|MCL_FUTURE);
  f = (Fake){.limit = {65536, RLIM_INFINITY}, .set_error = EPERM};
  run("required", &f, -1, "unlimited soft/hard", 1, 1, 0, 0);
  f = (Fake){.limit = {65536, RLIM_INFINITY}, .set_error = EPERM};
  run("best-effort", &f, 1, "MCL_CURRENT only", 1, 1, 1, MCL_CURRENT);
  f = (Fake){.limit = {32768, 65536}};
  run("best-effort", &f, 1, "MCL_FUTURE not enabled", 1, 1, 1, MCL_CURRENT);
  f = (Fake){.limit = {65536, 65536}};
  run("required", &f, -1, "RLIMIT_MEMLOCK is finite", 1, 0, 0, 0);
  f = (Fake){.limit = {65536, 65536}, .current_error = ENOMEM};
  run("best-effort", &f, 1, "process pages may be swappable", 1, 0, 1, MCL_CURRENT);

  f = (Fake){.get_error = EINVAL};
  run("required", &f, -1, "getrlimit(RLIMIT_MEMLOCK) failed", 1, 0, 0, 0);
  f = (Fake){.get_error = EINVAL, .current_error = ENOSYS};
  run("best-effort", &f, 1, "mlockall(MCL_CURRENT) failed", 1, 0, 1, MCL_CURRENT);
  f = (Fake){.limit = {RLIM_INFINITY, RLIM_INFINITY}, .full_error = ENOSYS};
  run("required", &f, -1, "mlockall(MCL_CURRENT|MCL_FUTURE) failed", 1, 0, 1, MCL_CURRENT|MCL_FUTURE);
  f = (Fake){.limit = {RLIM_INFINITY, RLIM_INFINITY}, .full_error = EPERM};
  run("best-effort", &f, 1, "MCL_CURRENT only", 1, 0, 2, MCL_CURRENT|MCL_FUTURE);
  f = (Fake){.limit = {RLIM_INFINITY, RLIM_INFINITY}, .full_error = EPERM, .current_error = ENOMEM};
  run("best-effort", &f, 1, "process pages may be swappable", 1, 0, 2, MCL_CURRENT|MCL_FUTURE);
  signet_memory_apply_allocator_policy();
  puts("memory hardening: parser and syscall decisions PASS");
  return 0;
}
