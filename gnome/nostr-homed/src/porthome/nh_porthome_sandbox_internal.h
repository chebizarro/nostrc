#ifndef NH_PORTHOME_SANDBOX_INTERNAL_H
#define NH_PORTHOME_SANDBOX_INTERNAL_H
#include "nh_porthome_sandbox.h"

/* Broker-only workspace handoff. private_stage_fd must name a fresh root-owned
 * 0700 directory below a root-only parent, not the installer's stage itself.
 * The child gives this directory to the exact resolved drop UID before setuid.
 * The caller reclaims it after reaping the child, including on failure. */
nh_porthome_sandbox_rc nh_porthome_spawn_staged(
    char *const argv[], char *const envp[],
    int stdin_fd, int stdout_fd, int stderr_fd,
    const int *keep_fds, size_t n_keep_fds,
    uint32_t deadline_ms, pid_t *out_pid, int private_stage_fd);
#endif
