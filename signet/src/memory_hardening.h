/* SPDX-License-Identifier: MIT */
#ifndef SIGNET_MEMORY_HARDENING_H
#define SIGNET_MEMORY_HARDENING_H

#include <stddef.h>
#include <sys/resource.h>

typedef enum {
  SIGNET_MLOCK_MODE_INVALID = -1,
  SIGNET_MLOCK_MODE_BEST_EFFORT = 0,
  SIGNET_MLOCK_MODE_REQUIRED,
  SIGNET_MLOCK_MODE_OFF,
} SignetMlockMode;

/* Private syscall seam; callbacks follow syscall return/errno conventions. */
typedef struct {
  int (*get_limit)(void *data, struct rlimit *limit);
  int (*set_limit)(void *data, const struct rlimit *limit);
  int (*lock_all)(void *data, int flags);
  void *data;
} SignetMemoryOps;

SignetMlockMode signet_mlock_mode_parse(const char *raw);
void signet_memory_apply_allocator_policy(void);
/* NULL ops selects real syscalls. Returns -1 to refuse startup, 0 for full
 * locking or explicit off, 1 for best-effort degradation. Always sets message. */
int signet_memory_harden(const char *raw, const SignetMemoryOps *ops,
                         char *message, size_t message_len);

#endif
