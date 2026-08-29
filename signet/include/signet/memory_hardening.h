/* SPDX-License-Identifier: MIT */

#ifndef SIGNET_MEMORY_HARDENING_H
#define SIGNET_MEMORY_HARDENING_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  SIGNET_MLOCK_MODE_FUTURE = 0,
  SIGNET_MLOCK_MODE_CURRENT,
  SIGNET_MLOCK_MODE_OFF,
} SignetMlockMode;

SignetMlockMode signet_mlock_mode_from_env(const char *raw, int *out_valid);
const char *signet_mlock_mode_to_string(SignetMlockMode mode);

void signet_memory_apply_allocator_policy(void);
int signet_memory_lock_process(SignetMlockMode mode, char *err_buf, size_t err_buf_len);
void signet_memory_trim_after_request(void);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_MEMORY_HARDENING_H */
