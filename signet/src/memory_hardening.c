/* SPDX-License-Identifier: MIT */
/* Adapted from Stew's memory budget hardening (a7d83cd35). */
#include "memory_hardening.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <glib.h>

#if defined(__GLIBC__) && defined(__linux__)
#include <malloc.h>
#endif

SignetMlockMode signet_mlock_mode_parse(const char *raw) {
  if (!raw || !raw[0] || g_ascii_strcasecmp(raw, "best-effort") == 0)
    return SIGNET_MLOCK_MODE_BEST_EFFORT;
  if (g_ascii_strcasecmp(raw, "required") == 0)
    return SIGNET_MLOCK_MODE_REQUIRED;
  if (g_ascii_strcasecmp(raw, "off") == 0)
    return SIGNET_MLOCK_MODE_OFF;
  return SIGNET_MLOCK_MODE_INVALID;
}

void signet_memory_apply_allocator_policy(void) {
#if defined(__GLIBC__) && defined(__linux__) && defined(M_ARENA_MAX)
  /* Limit per-thread arena retention, not the amount of live/locked memory.
   * Advisory only; do not couple allocator maintenance to signing requests. */
  (void)mallopt(M_ARENA_MAX, 2);
#endif
}

static int get_limit(void *data, struct rlimit *limit) {
  (void)data;
  return getrlimit(RLIMIT_MEMLOCK, limit);
}

static int set_limit(void *data, const struct rlimit *limit) {
  (void)data;
  return setrlimit(RLIMIT_MEMLOCK, limit);
}

static int lock_all(void *data, int flags) {
  (void)data;
  return mlockall(flags);
}

int signet_memory_harden(const char *raw, const SignetMemoryOps *ops,
                         char *message, size_t message_len) {
  static const SignetMemoryOps system_ops = {get_limit, set_limit, lock_all, NULL};
  SignetMlockMode mode = signet_mlock_mode_parse(raw);
  if (!ops) ops = &system_ops;
  if (mode == SIGNET_MLOCK_MODE_INVALID) {
    snprintf(message, message_len,
             "invalid SIGNET_MLOCK_MODE (expected off, best-effort, or required)");
    return -1;
  }
  if (mode == SIGNET_MLOCK_MODE_OFF) {
    snprintf(message, message_len, "process memory locking disabled (off)");
    return 0;
  }

  struct rlimit limit;
  int unlimited = 0;
  char budget[160];
  if (ops->get_limit(ops->data, &limit) != 0) {
    snprintf(budget, sizeof(budget), "getrlimit(RLIMIT_MEMLOCK) failed: %s", strerror(errno));
  } else {
    /* Respect the operator's hard ceiling. Raising it needs privilege and
     * would silently override the configured budget. */
    if (limit.rlim_cur != limit.rlim_max) {
      struct rlimit want = limit;
      want.rlim_cur = want.rlim_max;
      if (ops->set_limit(ops->data, &want) == 0) limit = want;
    }
    unlimited = limit.rlim_cur == RLIM_INFINITY;
    snprintf(budget, sizeof(budget), "RLIMIT_MEMLOCK is finite");
  }

  /* A successful MCL_FUTURE with a finite limit can still make later malloc,
   * mmap, thread creation or stack growth fail. Do not enable that trap, even
   * if today's capabilities would bypass the ceiling. */
  if (mode == SIGNET_MLOCK_MODE_REQUIRED && !unlimited) {
    snprintf(message, message_len,
             "required memory locking refused: %s; set an unlimited soft/hard "
             "memlock limit (Docker --ulimit memlock=-1:-1, systemd LimitMEMLOCK=infinity)", budget);
    return -1;
  }

  char reason[192];
  if (unlimited) {
    if (ops->lock_all(ops->data, MCL_CURRENT | MCL_FUTURE) == 0) {
      snprintf(message, message_len, "process memory locking active: MCL_CURRENT|MCL_FUTURE");
      return 0;
    }
    snprintf(reason, sizeof(reason), "mlockall(MCL_CURRENT|MCL_FUTURE) failed: %s", strerror(errno));
    if (mode == SIGNET_MLOCK_MODE_REQUIRED) {
      snprintf(message, message_len, "required memory locking refused: %s", reason);
      return -1;
    }
  } else {
    snprintf(reason, sizeof(reason), "%s; MCL_FUTURE not enabled", budget);
  }

  if (ops->lock_all(ops->data, MCL_CURRENT) == 0) {
    snprintf(message, message_len,
             "best-effort memory locking degraded: %s; MCL_CURRENT only, future pages may be swappable", reason);
  } else {
    snprintf(message, message_len,
             "best-effort memory locking degraded: %s; mlockall(MCL_CURRENT) failed: %s; "
             "process pages may be swappable", reason, strerror(errno));
  }
  return 1;
}
