#ifndef NH_AUTH_WORKER_H
#define NH_AUTH_WORKER_H

/*
 * auth_worker: run a bounded, cancellable unit of work in a forked child
 * process. Used to isolate the CPU/memory-heavy scrypt vault unlock (and
 * the follow-up event sign) from the broker's main context so a hostile
 * or slow unlock cannot block the broker.
 *
 * The worker:
 *   - forks a child with two anonymous pipes (parent->child input,
 *     child->parent output),
 *   - closes unused file descriptors in the child (in particular the
 *     inherited listening socket) so a compromised child cannot serve
 *     traffic,
 *   - enforces a hard millisecond timeout in the parent using poll(),
 *   - on timeout, SIGKILLs the child and waitpid()s to reap it, so no
 *     zombie is left,
 *   - on crash (non-zero exit or signal) reports FAILED with the raw
 *     exit code / termination signal,
 *   - on success reads the child's result blob and returns it.
 *
 * The parent's `input` buffer is wiped after being handed to the child.
 * The child function is expected to wipe its own copies of any secret
 * material before exiting; core dumps are disabled in the child.
 *
 * A process-wide single-slot guard is enforced. This is the placeholder
 * for the eventual max-2 concurrent scrypt worker pool; today the
 * accept-one/handle-one broker cannot reach the second slot anyway
 * (see nostrc-zcll.2 -- the concurrent broker event loop lands as a
 * follow-up).
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum nh_auth_worker_status {
  NH_AUTH_WORKER_OK = 0,
  NH_AUTH_WORKER_FAILED,      /* child exited non-zero or was killed by a signal
                                 other than SIGKILL that we sent for timeout */
  NH_AUTH_WORKER_TIMEOUT,     /* parent SIGKILLed the child after timeout_ms */
  NH_AUTH_WORKER_BUSY,        /* single-slot guard: another worker in flight */
  NH_AUTH_WORKER_INTERNAL     /* parent-side setup error (fork/pipe/OOM) */
} nh_auth_worker_status;

typedef struct nh_auth_worker_result {
  nh_auth_worker_status status;
  int exit_code;              /* valid when child exited normally */
  int term_signal;            /* valid when child was killed by a signal */
} nh_auth_worker_result;

/*
 * Child-side entry point.
 *   input, input_len : the payload the parent sent (already fully read)
 *   out_fd           : write the result blob here; closed by the runtime
 *                      on return
 *   user_data        : passed through from nh_auth_worker_run
 *
 * Return value becomes the child's exit code (0 = success). Non-zero
 * exit codes are surfaced to the parent as NH_AUTH_WORKER_FAILED with
 * the exit code preserved so callers can distinguish domain-level
 * failures (e.g. wrong passphrase) from crashes.
 */
typedef int (*nh_auth_worker_fn)(const uint8_t *input, size_t input_len,
                                 int out_fd, void *user_data);

/*
 * Run `fn` in a forked child with the given input and a hard timeout.
 *
 *   input, input_len : payload to send to the child. If non-NULL, the
 *                      buffer is wiped in the parent after being sent
 *                      (treated as secret). Pass NULL for no payload.
 *   timeout_ms       : maximum wall time before the child is SIGKILLed.
 *                      0 means "no timeout" (still uses poll(-1)).
 *   max_output       : upper bound on bytes the parent will read from
 *                      the child (defence against runaway output).
 *                      Extra bytes are discarded and the call fails.
 *   out, out_len     : on success (NH_AUTH_WORKER_OK) receive a newly
 *                      malloc()ed buffer with the child's output. The
 *                      caller owns and must free() it. May be set to
 *                      NULL/0 on non-OK statuses.
 *   result_out       : optional, receives detailed status.
 *
 * Returns the same status as result_out->status for convenience.
 */
nh_auth_worker_status nh_auth_worker_run(nh_auth_worker_fn fn, void *user_data,
                                         uint8_t *input, size_t input_len,
                                         uint32_t timeout_ms, size_t max_output,
                                         uint8_t **out, size_t *out_len,
                                         nh_auth_worker_result *result_out);

/*
 * Test/introspection hook: current number of in-flight workers (0 or 1
 * today; up to 2 once the concurrent broker lands).
 */
int nh_auth_worker_inflight(void);

#ifdef __cplusplus
}
#endif

#endif /* NH_AUTH_WORKER_H */
