/* SPDX-License-Identifier: MIT */

#include "signet/memory_hardening.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <glib.h>

#if defined(__linux__)
#include <sys/mman.h>
#include <sys/resource.h>
#endif

#if defined(__GLIBC__) && defined(__linux__)
#include <malloc.h>
#define SIGNET_HAVE_MALLOC_TRIM 1
#endif

#define SIGNET_MALLOC_TRIM_EVERY_REQUESTS 64

SignetMlockMode signet_mlock_mode_from_env(const char *raw, int *out_valid) {
  if (out_valid) *out_valid = 1;
  if (!raw || !raw[0] || g_ascii_strcasecmp(raw, "future") == 0 ||
      g_ascii_strcasecmp(raw, "full") == 0) {
    return SIGNET_MLOCK_MODE_FUTURE;
  }
  if (g_ascii_strcasecmp(raw, "current") == 0) {
    return SIGNET_MLOCK_MODE_CURRENT;
  }
  if (g_ascii_strcasecmp(raw, "off") == 0 || g_ascii_strcasecmp(raw, "none") == 0 ||
      g_ascii_strcasecmp(raw, "false") == 0 || strcmp(raw, "0") == 0) {
    return SIGNET_MLOCK_MODE_OFF;
  }
  if (out_valid) *out_valid = 0;
  return SIGNET_MLOCK_MODE_FUTURE;
}

const char *signet_mlock_mode_to_string(SignetMlockMode mode) {
  switch (mode) {
  case SIGNET_MLOCK_MODE_FUTURE:
    return "future";
  case SIGNET_MLOCK_MODE_CURRENT:
    return "current";
  case SIGNET_MLOCK_MODE_OFF:
    return "off";
  default:
    return "future";
  }
}

void signet_memory_apply_allocator_policy(void) {
#if defined(SIGNET_HAVE_MALLOC_TRIM)
#ifdef M_ARENA_MAX
  (void)mallopt(M_ARENA_MAX, 2);
#endif
#ifdef M_TRIM_THRESHOLD
  (void)mallopt(M_TRIM_THRESHOLD, 128 * 1024);
#endif
#ifdef M_TOP_PAD
  (void)mallopt(M_TOP_PAD, 64 * 1024);
#endif
#endif
}

static void signet_memory_raise_memlock_limit(void) {
#if defined(__linux__)
  struct rlimit rl;
  if (getrlimit(RLIMIT_MEMLOCK, &rl) != 0 || rl.rlim_cur == RLIM_INFINITY) return;

  struct rlimit want = rl;
  want.rlim_cur = RLIM_INFINITY;
  want.rlim_max = RLIM_INFINITY;
  if (setrlimit(RLIMIT_MEMLOCK, &want) == 0) return;

  rlim_t fallback = 512ULL * 1024 * 1024;
  if (rl.rlim_max != RLIM_INFINITY && rl.rlim_max < fallback) fallback = rl.rlim_max;
  if (fallback > rl.rlim_cur) {
    want = rl;
    want.rlim_cur = fallback;
    (void)setrlimit(RLIMIT_MEMLOCK, &want);
  }
#endif
}

int signet_memory_lock_process(SignetMlockMode mode, char *err_buf, size_t err_buf_len) {
  if (err_buf && err_buf_len) err_buf[0] = '\0';
  if (mode == SIGNET_MLOCK_MODE_OFF) return 0;

#if defined(__linux__)
  signet_memory_raise_memlock_limit();

  int flags = MCL_CURRENT;
  if (mode == SIGNET_MLOCK_MODE_FUTURE) flags |= MCL_FUTURE;

  if (mlockall(flags) == 0) return 0;
  int first_errno = errno;

  if (mode == SIGNET_MLOCK_MODE_FUTURE && mlockall(MCL_CURRENT) == 0) {
    if (err_buf && err_buf_len) {
      snprintf(err_buf, err_buf_len,
               "mlockall(MCL_CURRENT|MCL_FUTURE) failed: %s; degraded to MCL_CURRENT",
               strerror(first_errno));
    }
    return 1;
  }

  if (err_buf && err_buf_len) {
    snprintf(err_buf, err_buf_len, "mlockall() failed: %s", strerror(first_errno));
  }
  return -1;
#else
  if (err_buf && err_buf_len) {
    snprintf(err_buf, err_buf_len, "mlockall() unsupported on this platform");
  }
  return -1;
#endif
}

void signet_memory_trim_after_request(void) {
#if defined(SIGNET_HAVE_MALLOC_TRIM)
  static int request_count = 0;
  int after = g_atomic_int_add(&request_count, 1) + 1;
  if ((after % SIGNET_MALLOC_TRIM_EVERY_REQUESTS) == 0) {
    (void)malloc_trim(0);
  }
#endif
}
