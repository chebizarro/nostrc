/*
 * auth_porthome.h — broker glue for portable-home provisioning
 * (Phase 2, bead nostrc-89rj). All symbols here are compiled ONLY
 * when NH_AUTH_BROKER_ENABLE_PORTHOME is defined; otherwise the
 * broker returns NH_AUTH_RESULT_NOT_SUPPORTED for the corresponding
 * request operations and the base link-closure does not pull in
 * libnostr_porthome / libhanami.
 *
 * The provisioning job registry lives in this module. A `PROVISION_HOME`
 * request starts a job (fork-and-drop-privs subprocess that materializes
 * into a staged descriptor); a `WAIT_HOME` request polls that job. The
 * broker's single-transaction-per-connection invariant is preserved:
 * WAIT_HOME does not require an active transaction (the job is
 * addressable by uid + account), and no queueing happens — a second
 * PROVISION_HOME for the same account returns IN_PROGRESS.
 */

#ifndef NH_AUTH_BROKER_PORTHOME_H
#define NH_AUTH_BROKER_PORTHOME_H

#include "auth_provider.h"
#include "nostr_auth_protocol.h"
#include "nostr_identity.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque job handle. Broker-owned; ref-counted (created by
 * nh_auth_porthome_start, released by nh_auth_porthome_release). */
typedef struct nh_auth_porthome_job nh_auth_porthome_job;

/* Per-broker configuration snapshot. Populated from auth.conf
 * (nh_auth_conf) at broker startup and passed by pointer into
 * nh_auth_porthome_start. Borrowed lifetime — must outlive the job. */
typedef struct nh_auth_porthome_config {
  /* Space-padded arrays of URLs so the broker can walk them in a
   * fixed layout. Callers copy from nh_auth_conf.home_relays /
   * blossom_servers. */
  const char *const *home_relays;
  size_t             home_relays_count;
  const char *const *blossom_servers;
  size_t             blossom_servers_count;
  /* Per-file bandwidth cap (bytes). 0 -> defaults from provisioner. */
  size_t   bandwidth_bytes_per_load;
  /* Per-file wall-clock cap (seconds). 0 -> defaults. */
  unsigned load_timeout_sec;
  /* Total per-home cap (bytes). 0 -> defaults. */
  size_t   max_home_bytes;
  /* If non-zero, on first login with no wrapped_home_key on the
   * account, ask the signer to nip44_encrypt(fresh_seed) and persist
   * the ciphertext (design §4.3 first-login enrollment). Off by
   * default; enabled via porthome_enroll_wrap_key=on in auth.conf. */
  int      enroll_wrap_key;
} nh_auth_porthome_config;

typedef enum {
  NH_AUTH_PORTHOME_JOB_PENDING = 0,
  NH_AUTH_PORTHOME_JOB_RUNNING,
  NH_AUTH_PORTHOME_JOB_OK,
  NH_AUTH_PORTHOME_JOB_LIMITED,
  NH_AUTH_PORTHOME_JOB_FAILED,
} nh_auth_porthome_job_state;

typedef struct nh_auth_porthome_progress_hint {
  uint64_t bytes;
  uint64_t total_bytes;
  uint32_t files;
  uint32_t total_files;
} nh_auth_porthome_progress_hint;

/* Global registry lifecycle. Called once at broker startup / shutdown.
 * Idempotent. */
int  nh_auth_porthome_registry_init(void);
void nh_auth_porthome_registry_shutdown(void);

/* Look up an existing job for `account_id`; returns NULL if none. Does
 * NOT bump the refcount — caller must not free. */
nh_auth_porthome_job *nh_auth_porthome_lookup(const char *account_id);

/* Start a new provisioning job for `account`, using `wrap_seed` (32
 * bytes; the broker owns / mlocks this — the job wipes its copy on
 * completion). Returns 0 on success and sets *out_job (borrowed).
 * Returns -1 if a job for this account is already in flight (caller
 * should return IN_PROGRESS to the client, not queue). */
int nh_auth_porthome_start(const nh_auth_porthome_config *config,
                           const nh_identity_account *account,
                           const uint8_t wrap_seed[32],
                           const char *tx_id,
                           nh_identity_store *store,
                           nh_auth_porthome_job **out_job);

/* Poll a job (bounded wait). Returns the current state and the latest
 * progress hint. `timeout_ms` is a MAX — the broker never blocks longer.
 * On terminal states (OK/LIMITED/FAILED) the job is retired and
 * subsequent lookup returns NULL. */
nh_auth_porthome_job_state
nh_auth_porthome_wait(nh_auth_porthome_job *job, uint32_t timeout_ms,
                      nh_auth_porthome_progress_hint *hint_out);

/* Cancel an in-flight job (best-effort). Terminal jobs are ignored. */
void nh_auth_porthome_cancel(nh_auth_porthome_job *job);

/* Directory for the per-tx progress artifact
 * (/run/nostr-auth/greeter/<tx>/porthome.json). Passing NULL/"" resets
 * to the compile-time default. Test seam so headless tests can write
 * to a tmpdir. */
void nh_auth_porthome_set_progress_dir(const char *dir);

/* Test seam: for headless unit / integration tests inject a synthetic
 * job outcome. If `sealed_manifest` is non-NULL, the next
 * nh_auth_porthome_start call SKIPS the real network fetch and
 * materialises `sealed_manifest` directly under the account's staged
 * home using the provided blob-fetcher. Every call consumes ONE
 * injection; NULL = restore real path. */
typedef int (*nh_auth_porthome_fetch_hook_fn)(void *ctx, const char *sha256_hex,
                                              uint8_t **out_ct,
                                              size_t *out_ct_len);

void nh_auth_porthome_set_test_hook(const uint8_t *sealed_manifest,
                                    size_t sealed_len,
                                    nh_auth_porthome_fetch_hook_fn fetch,
                                    void *fetch_ctx);

/* Wrap-seed cache (bead nostrc-ck6i). The provider's post-verify
 * callback calls deposit_wrap_seed(account_id, seed) after a
 * successful auth; the broker's PROVISION_HOME handler calls
 * take_wrap_seed to consume it (single-use, TTL 300 s, mlock'd).
 * Both return 0 on success. */
int nh_auth_broker_porthome_deposit_wrap_seed(const char *account_id,
                                              const uint8_t seed[32]);
int nh_auth_broker_porthome_take_wrap_seed(const char *account_id,
                                           uint8_t out_seed[32]);

/* Bead nostrc-pvha: install broker-scoped state that the NIP-46 provider
 * post-verify hook needs (identity-store handle for reading/writing
 * wrapped_home_key, and the auth.conf porthome_enroll_wrap_key flag).
 * Called once from nostr-authd.c after config parse. `store` is a
 * borrowed pointer; the broker owns it. Idempotent — a second call
 * replaces the previous state. Passing store=NULL disables the hook. */
void nh_auth_broker_porthome_install(nh_identity_store *store,
                                     int enroll_wrap_key);

/* Called by provider_nip46 / provider_nip46_qr after a successful
 * sign_event verify, BEFORE emitting SIGNED_EVENT to the runtime. When
 * PORTHOME is enabled at build time AND the broker has installed a
 * store handle (nh_auth_broker_porthome_install), this helper:
 *
 *   1. Reads the account's enabled NIP-46 provider record.
 *   2. If wrapped_home_key is non-empty: calls
 *      nostr_nip46_client_nip44_decrypt_rpc(peer=account_pubkey_hex,
 *      ct=<wrapped_home_key as ASCII>) to obtain the 32-byte plaintext
 *      seed, deposits it into the wrap-seed cache (mlock'd).
 *   3. Else, if enroll_wrap_key is on: RAND_bytes a fresh 32-byte seed,
 *      calls nostr_nip46_client_nip44_encrypt_rpc(peer=account_pubkey_hex,
 *      pt=<64-hex of seed>), persists the ciphertext to the provider
 *      record, and deposits the plaintext into the cache.
 *   4. Else: no-op (PROVISION_HOME will map to NOT_SUPPORTED).
 *
 * The session is borrowed. All secret plaintext buffers are
 * OPENSSL_cleanse'd before free. NEVER logs the seed or the plaintext.
 * Returns 0 on success (including the "nothing to do" case: PORTHOME
 * not built, no store installed, no NIP-46 provider record, or
 * enrollment disabled with no existing ciphertext). Returns -1 on a
 * hard signer failure that the caller should surface as UNAVAILABLE.
 *
 * The session pointer is typed void* here to keep this header free of
 * a NIP-46 build dependency — both callers already include the NIP-46
 * headers. */
int nh_auth_broker_porthome_maybe_enroll_or_unwrap_nip46(
    void *nip46_session, const char *account_id,
    const char *account_pubkey_hex);

/* Phase 2.5B (bead nostrc-ww50): fork+drop-privs sandbox around the
 * portable-home NETWORK FETCH. The broker never dials relays or
 * Blossom itself; it exec()s the `nostr-home-fetch` helper (bead
 * nostrc-9k4g) under nh_porthome_spawn_sandboxed. This function is
 * the broker's public seam — the concurrent 9k4g agent calls it from
 * inside job_run's fetch closure, before the materialisation step
 * that follows (materialisation stays as root because it writes
 * through the identity-layer staging dirfd).
 *
 *   helper_path  Absolute path resolved from auth.conf
 *                (porthome_fetch_helper), or NULL for the compile-time
 *                default /usr/libexec/nostr-homed/nostr-home-fetch.
 *   drop_user    Preferred unprivileged uid; NULL selects the
 *                sandbox default (nostr-home-fetch, falling back to
 *                nobody if that account is missing).
 *   argv_tail    NULL-terminated extra args appended after argv[0].
 *                Copied by value — the caller owns the storage.
 *   stdin_fd/stdout_fd/stderr_fd  Passed through to the child; the
 *                caller retains their side of any pipe(). Use -1 for
 *                /dev/null (stdin/stdout) or "inherit journal" (stderr).
 *   deadline_ms  Wall-clock deadline enforced by the sandbox waiter
 *                (SIGTERM then SIGKILL on overrun). 0 disables.
 *   out_exit     Set to the helper's exit code (0 on clean exit) on
 *                any successful spawn+reap. -1 if the helper died to
 *                a signal.
 *   out_signal   Set to the signal that terminated the helper, or 0.
 *   out_timed_out  Set to 1 if the sandbox reaper killed the helper
 *                for exceeding @deadline_ms.
 *
 * Returns 0 on success (helper spawned and reaped), -1 on any spawn
 * failure — the caller maps the helper's exit code onto
 * LIMITED_MODE vs FAILED. Exit codes 64-69 come from the helper
 * (network / decode / SSRF-refused / decode-failure classes — treat
 * as LIMITED_MODE); anything else (70+ or a signal) is FAILED so the
 * broker's WAIT_HOME response is honest about the split. */
int nh_auth_broker_porthome_run_fetch(const char *helper_path,
                                      const char *drop_user,
                                      char *const argv_tail[],
                                      int stdin_fd, int stdout_fd,
                                      int stderr_fd,
                                      uint32_t deadline_ms,
                                      int *out_exit,
                                      int *out_signal,
                                      int *out_timed_out);

/* Classify a helper exit code onto the WAIT_HOME job-state axis.
 *   0             -> NH_AUTH_PORTHOME_JOB_OK
 *   64..69        -> NH_AUTH_PORTHOME_JOB_LIMITED (network/decode/SSRF)
 *   anything else -> NH_AUTH_PORTHOME_JOB_FAILED (internal / signal)
 *
 * A helper terminated by signal (exit == -1) is FAILED. Non-zero
 * @signal (even alongside a good exit) is FAILED — we never treat a
 * KILL'd helper as a merely-limited outcome. */
int nh_auth_broker_porthome_classify_fetch_exit(int exit_code, int signal);

/* Compile-time default helper path (matches design §7.2 install rule).
 * The concurrent 9k4g agent may override this via its config lookup;
 * the constant is exported so tests share the same string. */
#define NH_AUTH_BROKER_PORTHOME_DEFAULT_HELPER \
    "/usr/libexec/nostr-homed/nostr-home-fetch"


/* W(3) — per-user runtime seed drop (bead nostrc-p6qp).
 * See auth_porthome.c for the design rationale. */
int  nh_auth_broker_porthome_drop_seed_for_uid(uid_t uid,
                                               const uint8_t seed[32]);
void nh_auth_broker_porthome_set_session_dir(const char *dir);

#ifdef __cplusplus
}
#endif

#endif /* NH_AUTH_BROKER_PORTHOME_H */
