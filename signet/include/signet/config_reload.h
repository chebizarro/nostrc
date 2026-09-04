/* SPDX-License-Identifier: MIT
 *
 * config_reload.h - SIGHUP-driven whole-config reload for signetd (fp-56t).
 *
 * Follows the pattern already shipped in loom-worker and grasp-gitea, and
 * refined in bahia: SIGHUP re-reads the WHOLE config, validates it, and either
 * applies it or retains the last-valid configuration. A declared subset is
 * live-reloadable; the remainder stays restart-only and logs an explicit
 * warning when it changes, rather than being silently ignored (decision D1).
 *
 * Live-reloadable:
 *   - [nostr] relays              (relay pool reconfigure, subscriptions kept)
 *   - [nostr] provisioner_pubkeys (reconciled against persisted authorization)
 *   - [nostr] reconnect_interval_s
 *   - [policy_defaults] default_decision, allowed_kinds, rate_limit_rpm
 *   - the policy file's contents  (eager reload, not deferred to next lookup)
 *   - [server] log_level
 *
 * Restart-only (warned, never silently dropped):
 *   - listeners: health_port, bootstrap port, dbus tcp/unix, nip5l, ssh agent
 *   - storage bindings: db_path, audit path/stdout, policy_file path
 *   - identity: identity name, bunker pubkey, uid_map, passkeys settings
 *
 * PROVISIONER RECONCILIATION (decision D2) — the security-critical part.
 * Config is the desired state, so its provisioner set wins on reload. Taken
 * alone that rule is a security regression: runtime revocation persists into
 * SQLCipher, so a reload against a config that still lists the revoked pubkey
 * would silently restore the authority an operator had just removed.
 * Reconciliation here is therefore write-back, not last-writer-wins:
 *   - every runtime revocation leaves a durable tombstone (store.h), and a
 *     config entry that predates its tombstone is REFUSED, never re-granted;
 *   - the effective set is written back into the config file, so config and
 *     SQLCipher converge instead of drifting;
 *   - when the provisioner list comes from SIGNET_PROVISIONER_PUBKEYS, write
 *     back cannot correct it, so the env set is treated as older than any
 *     runtime revocation and the operator is told to remove the override.
 */

#ifndef SIGNET_CONFIG_RELOAD_H
#define SIGNET_CONFIG_RELOAD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "signet/signet_config.h"
#include "signet/store.h"

struct SignetRelayPool;
struct SignetMgmtHandler;
struct SignetPolicyStore;
struct SignetPolicyEngine;

/**
 * SignetReloadCtx:
 * @config_path: (nullable): config file to re-read; %NULL reloads defaults+env.
 * @live: (not nullable): the last-valid config, updated in place on success.
 * @relays: (nullable): relay pool to reconfigure.
 * @mgmt: (nullable): management handler whose relay URLs follow the config.
 * @policy: (nullable): policy store to reload eagerly.
 * @engine: (nullable): policy engine whose default decision follows the config.
 * @store: (nullable): SQLCipher store holding provisioner authorization.
 * @on_relays_changed: (nullable): invoked after a successful relay-set change
 *   so the caller can re-establish anything keyed to the old connections.
 * @user_data: passed to @on_relays_changed.
 *
 * Everything a reload needs. Any %NULL component is simply skipped, which is
 * what makes the reload path testable without a running daemon.
 *
 * Since: 1.2
 */
typedef struct {
  const char *config_path;
  SignetConfig *live;
  struct SignetRelayPool *relays;
  struct SignetMgmtHandler *mgmt;
  struct SignetPolicyStore *policy;
  struct SignetPolicyEngine *engine;
  SignetStore *store;
  void (*on_relays_changed)(void *user_data);
  void *user_data;
} SignetReloadCtx;

/**
 * SignetReloadReport:
 * @applied: whether the candidate config was accepted and applied.
 * @error: failure reason when @applied is %false.
 * @relays_changed: whether the relay set was replaced.
 * @policy_reloaded: whether the policy file was re-read.
 * @default_decision_changed: whether the policy engine default changed.
 * @restart_only_changed: number of restart-only keys that changed and were
 *   warned about rather than applied.
 * @provisioners: reconciliation counters (see #SignetProvisionerReconcile).
 * @provisioner_writeback_failed: config write-back could not be completed.
 * @provisioner_env_shadowed: the provisioner set came from the environment,
 *   so write-back cannot converge config with persisted state.
 *
 * Outcome of a reload attempt.
 *
 * Since: 1.2
 */
typedef struct {
  bool applied;
  char error[256];
  bool relays_changed;
  bool policy_reloaded;
  bool default_decision_changed;
  size_t restart_only_changed;
  SignetProvisionerReconcile provisioners;
  bool provisioner_writeback_failed;
  bool provisioner_env_shadowed;
} SignetReloadReport;

/**
 * signet_config_reload_adopt_persisted:
 * @ctx: (not nullable): must carry a @store, and a @config_path to write to.
 * @now: current unix time in seconds.
 * @out_adopted: (nullable) (out): %true when this call performed the adoption.
 *
 * Establish the "config == persisted authorization" invariant exactly once,
 * at startup, before any reload can run.
 *
 * A database created before revocation tombstones existed may already contain
 * runtime revocations that left no trace. Treating config as desired state on
 * such a database would resurrect precisely the authority decision D2 forbids
 * restoring. This one-shot adoption instead takes the PERSISTED set as the
 * desired state and writes it back into the config file — an operation that
 * can only remove entries from config, never add authority — after which every
 * reload starts from a converged state and the tombstone rule is sufficient on
 * its own.
 *
 * Idempotent: subsequent calls are no-ops.
 *
 * Returns: 0 on success (including "already adopted"), -1 if the persisted set
 * could not be read or written back
 *
 * Since: 1.2
 */
int signet_config_reload_adopt_persisted(const SignetReloadCtx *ctx, int64_t now,
                                         bool *out_adopted);

/**
 * signet_config_reload_apply:
 * @ctx: (not nullable): components to reconfigure.
 * @now: current unix time in seconds.
 * @out_report: (nullable) (out): outcome details.
 *
 * Re-read, validate and apply the configuration.
 *
 * If the candidate config fails to load or fails validation, NOTHING is
 * applied and @ctx->live is left exactly as it was, so a bad edit or a
 * half-written file degrades to "keep running on the last good config"
 * instead of taking the daemon down.
 *
 * Returns: 0 when the candidate was applied, -1 when the last-valid config
 * was retained
 *
 * Since: 1.2
 */
int signet_config_reload_apply(const SignetReloadCtx *ctx, int64_t now,
                               SignetReloadReport *out_report);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_CONFIG_RELOAD_H */
