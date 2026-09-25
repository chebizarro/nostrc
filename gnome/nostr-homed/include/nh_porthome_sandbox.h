/*
 * nh_porthome_sandbox.h — fork+drop-privs sandbox for the portable-home
 * network fetch helper. Bead nostrc-ww50 (Phase 2.5B).
 *
 * SPDX-License-Identifier: MIT
 * EXPERIMENTAL. Gated behind NOSTR_HOMED_ENABLE_PORTHOME_EXPERIMENTAL.
 *
 * The broker (nostr-authd) runs as root because it materializes new
 * home directories via nh_identity_home_prepare's staging dirfd. But
 * the network side of a PROVISION_HOME job — the relay REQ / kind-30078
 * fetch and the Blossom GETs — has NO business running as root. This
 * module is a thin, opinionated wrapper around fork() that spawns a
 * helper (bead nostrc-9k4g's `nostr-home-fetch` binary) with:
 *   - EUID/EGID lowered to a dedicated `nostr-home-fetch` system user
 *     (or "nobody" if the sysusers.d snippet has not been applied)
 *   - all supplementary groups dropped (setgroups(1,&gid))
 *   - PR_SET_NO_NEW_PRIVS=1 and PR_SET_DUMPABLE=0
 *   - RLIMIT_AS / RLIMIT_CPU / RLIMIT_FSIZE / RLIMIT_NOFILE caps
 *   - closed non-listed fds (only the caller-supplied stdio triple)
 *   - env cleared except a small allow-list (PATH, TZ, LANG, HOME)
 *   - refusal to run at all unless the parent is currently root (belt
 *     and braces — the caller is already root inside nostr-authd)
 *
 * This mirrors src/profile/profile_image.c exactly. The SSRF guard
 * itself lives inside the helper (bead nostrc-9k4g), which knows how
 * to speak libcurl; this sandbox is the un-privileged perimeter that
 * gates the helper's ambient authority.
 *
 * See design docs/designs/home-from-relay.md §7.2 (package split) and
 * §8.2 (SSRF via Blossom server list). The sibling profile-image
 * pattern lives at src/profile/nostr-homed-profile-image.c.
 */
#ifndef NH_PORTHOME_SANDBOX_H
#define NH_PORTHOME_SANDBOX_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Return codes. Positive numbers reserved for future ambient failure
 * modes; anything != 0 means "did not spawn". */
typedef enum {
    NH_PORTHOME_SANDBOX_OK       =  0,
    NH_PORTHOME_SANDBOX_ARG      = -1, /* argv/fd sanity refused */
    NH_PORTHOME_SANDBOX_NOT_ROOT = -2, /* parent EUID != 0 — refuse */
    NH_PORTHOME_SANDBOX_NO_USER  = -3, /* drop_user and fallback both missing */
    NH_PORTHOME_SANDBOX_FORK     = -4,
    NH_PORTHOME_SANDBOX_IO       = -5,
} nh_porthome_sandbox_rc;

/* Tunables for the sandbox. Every field with a 0 value gets a compiled-in
 * default (see nh_porthome_sandbox.c). Broker resets/overrides these at
 * startup via nh_porthome_sandbox_set_defaults; test seams override for
 * specific test bodies. */
typedef struct nh_porthome_sandbox_limits {
    const char *drop_user;         /* NULL/"" -> "nostr-home-fetch" then "nobody" */
    unsigned    rlimit_cpu_sec;    /* wall CPU cap; 0 -> 30 s */
    size_t      rlimit_as_bytes;   /* address space cap; 0 -> 512 MiB */
    size_t      rlimit_fsize_bytes;/* per-file write cap; 0 -> 128 MiB */
    unsigned    rlimit_nofile;     /* fd limit; 0 -> 64 */
} nh_porthome_sandbox_limits;

/* Override the compiled-in defaults process-wide. Broker-owned, called
 * once at startup (typically from an auth.conf load); pass NULL to
 * reset. Fields left 0 in @lim keep the compiled defaults.
 * Not thread-safe against a concurrent spawn on other threads. */
void nh_porthome_sandbox_set_defaults(const nh_porthome_sandbox_limits *lim);

/* Per-spawn override for the drop-user resolution. Overrides both
 * @lim->drop_user (nh_porthome_sandbox_set_defaults) and the compiled
 * NH_SANDBOX_DEFAULT_USER for as long as it is set; pass NULL to
 * clear. The broker uses this to route auth.conf's
 * `porthome_fetch_user` into the sandbox on a per-fetch basis; tests
 * use it to pin uid resolution to a known-existent account. Persists
 * process-wide until cleared. */
void nh_porthome_sandbox_set_drop_user(const char *name);

/* If the last spawn had to fall back to "nobody" because the preferred
 * drop_user was missing from /etc/passwd, returns 1 (and resets to 0
 * for the next spawn). Test seam so CI can assert the fallback path
 * on hosts without the sysusers.d snippet applied. */
int  nh_porthome_sandbox_last_used_fallback_user(void);

/* Spawn @argv under the sandbox.
 *
 *   argv        NULL-terminated; argv[0] MUST be an absolute path (we
 *               use execve, no PATH resolution — the config lookup
 *               resolves the helper path up-front). Caller-owned.
 *   envp        NULL-terminated environment; NULL selects an internal
 *               minimal allow-list (PATH, TZ, LANG, HOME=/nonexistent).
 *   stdin_fd    dup2()'d onto fd 0 in the child. -1 -> /dev/null.
 *   stdout_fd   dup2()'d onto fd 1. -1 -> /dev/null.
 *   stderr_fd   dup2()'d onto fd 2. -1 -> inherit (journal).
 *   deadline_ms Advisory; stored as a hint for nh_porthome_sandbox_wait.
 *               This function does NOT block — the caller reaps the
 *               child via nh_porthome_sandbox_wait.
 *   out_pid     Set to the child pid on OK. Untouched on error.
 *
 * The three provided fds are dup2()'d in the child; every other fd is
 * closed (walked via /proc/self/fd, best-effort O(N) close on any
 * that isn't stdio or one of the passed-through fds). Callers must
 * NOT rely on CLOEXEC alone — some libraries clear it on inherited
 * fds during their startup.
 *
 * Returns NH_PORTHOME_SANDBOX_OK on success. On any failure the child
 * is either never started or already _exit()'d; @out_pid is untouched. */
nh_porthome_sandbox_rc nh_porthome_spawn_sandboxed(
    char *const argv[],
    char *const envp[],
    int stdin_fd, int stdout_fd, int stderr_fd,
    uint32_t deadline_ms,
    pid_t *out_pid);

/* Same as nh_porthome_spawn_sandboxed but additionally preserves
 * @keep_fds[0..@n_keep_fds) in the child. The caller MUST clear
 * FD_CLOEXEC on each fd it wants to survive execve() BEFORE calling
 * (execve honors CLOEXEC even for fds skipped by close_fds_except).
 * A NULL / zero-length keep list is equivalent to the base function.
 * Bead nostrc-ww50: required so the fetch spawner can pass a
 * staging dirfd inheritable through to `nostr-home-fetch`. */
nh_porthome_sandbox_rc nh_porthome_spawn_sandboxed_ex(
    char *const argv[],
    char *const envp[],
    int stdin_fd, int stdout_fd, int stderr_fd,
    const int *keep_fds, size_t n_keep_fds,
    uint32_t deadline_ms,
    pid_t *out_pid);


/* Wait for a sandboxed child with a wall-clock deadline. If the child
 * exceeds @deadline_ms this function SIGKILLs it (via a two-step
 * SIGTERM/SIGKILL) and reaps the corpse. Passing 0 disables the timer
 * and blocks until the child exits.
 *
 * On success (child reaped, no error path) returns 0 and fills:
 *   *out_exit_code  = WEXITSTATUS(status) if WIFEXITED, else -1
 *   *out_signal     = WTERMSIG(status)    if WIFSIGNALED, else 0
 *   *out_timed_out  = 1 if we killed it for exceeding @deadline_ms
 *
 * Any of @out_exit_code / @out_signal / @out_timed_out may be NULL. */
int nh_porthome_sandbox_wait(pid_t pid, uint32_t deadline_ms,
                             int *out_exit_code, int *out_signal,
                             int *out_timed_out);

#ifdef __cplusplus
}
#endif

#endif /* NH_PORTHOME_SANDBOX_H */
