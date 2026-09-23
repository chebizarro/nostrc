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

#ifdef __cplusplus
}
#endif

#endif /* NH_AUTH_BROKER_PORTHOME_H */
