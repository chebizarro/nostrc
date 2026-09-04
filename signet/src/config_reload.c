/* SPDX-License-Identifier: MIT
 *
 * config_reload.c - SIGHUP-driven whole-config reload (fp-56t).
 *
 * See config_reload.h for the reloadable/restart-only boundary (D1) and the
 * provisioner write-back rule (D2).
 */

#include "signet/config_reload.h"

#include "signet/mgmt_protocol.h"
#include "signet/policy_engine.h"
#include "signet/policy_store.h"
#include "signet/relay_pool.h"

#include <stdio.h>
#include <string.h>

#include <glib.h>

/* ------------------------- restart-only diffing --------------------------- */

static size_t reload_warn_str(const char *key, const char *live, const char *cand) {
  if (g_strcmp0(live, cand) == 0) return 0;
  g_warning("[signetd] reload: '%s' changed ('%s' -> '%s') but is restart-only; "
            "keeping '%s'. Restart signetd to apply.",
            key, live ? live : "", cand ? cand : "", live ? live : "");
  return 1;
}

static size_t reload_warn_int(const char *key, long long live, long long cand) {
  if (live == cand) return 0;
  g_warning("[signetd] reload: '%s' changed (%lld -> %lld) but is restart-only; "
            "keeping %lld. Restart signetd to apply.",
            key, live, cand, live);
  return 1;
}

static size_t reload_warn_bool(const char *key, bool live, bool cand) {
  if (live == cand) return 0;
  g_warning("[signetd] reload: '%s' changed (%s -> %s) but is restart-only; "
            "keeping %s. Restart signetd to apply.",
            key, live ? "true" : "false", cand ? "true" : "false",
            live ? "true" : "false");
  return 1;
}

/* Warn for every restart-only key that differs. Per decision D1 these keys —
 * listening sockets, durable storage bindings and process identity — stay
 * restart-only, but they must never be dropped in silence: an operator who
 * edited db_path and sent SIGHUP has to learn that the daemon is still using
 * the old database. */
static size_t reload_warn_restart_only(const SignetConfig *live,
                                       const SignetConfig *cand) {
  size_t n = 0;

  /* Durable storage bindings. */
  n += reload_warn_str("[store] db_path", live->db_path, cand->db_path);
  n += reload_warn_str("[audit] path", live->audit_path, cand->audit_path);
  n += reload_warn_bool("[audit] stdout", live->audit_stdout, cand->audit_stdout);
  n += reload_warn_str("[policy_defaults] policy_file", live->policy_file_path,
                       cand->policy_file_path);
  n += reload_warn_int("[replay] max_entries", (long long)live->replay_max_entries,
                       (long long)cand->replay_max_entries);
  n += reload_warn_int("[replay] ttl_seconds", (long long)live->replay_ttl_seconds,
                       (long long)cand->replay_ttl_seconds);
  n += reload_warn_int("[replay] skew_seconds", (long long)live->replay_skew_seconds,
                       (long long)cand->replay_skew_seconds);

  /* Listening sockets. */
  n += reload_warn_int("[server] health_port", live->health_port, cand->health_port);
  n += reload_warn_int("[bootstrap] port", live->bootstrap_port, cand->bootstrap_port);
  n += reload_warn_bool("[dbus] unix_enabled", live->dbus_unix_enabled,
                        cand->dbus_unix_enabled);
  n += reload_warn_bool("[dbus] tcp_enabled", live->dbus_tcp_enabled,
                        cand->dbus_tcp_enabled);
  n += reload_warn_int("[dbus] tcp_port", live->dbus_tcp_port, cand->dbus_tcp_port);
  n += reload_warn_bool("[nip5l] enabled", live->nip5l_enabled, cand->nip5l_enabled);
  n += reload_warn_str("[nip5l] socket_path", live->nip5l_socket_path,
                       cand->nip5l_socket_path);
  n += reload_warn_bool("[ssh_agent] enabled", live->ssh_agent_enabled,
                        cand->ssh_agent_enabled);
  n += reload_warn_str("[ssh_agent] socket_path", live->ssh_agent_socket_path,
                       cand->ssh_agent_socket_path);

  /* Process identity. */
  n += reload_warn_str("[nostr] identity", live->identity, cand->identity);
  n += reload_warn_str("[nostr] bunker_pubkey", live->remote_signer_pubkey_hex,
                       cand->remote_signer_pubkey_hex);
  if (live->n_uid_map != cand->n_uid_map) {
    n += reload_warn_int("[uid_map] entries", (long long)live->n_uid_map,
                         (long long)cand->n_uid_map);
  }

  /* Passkey authenticator identity/backend. */
  n += reload_warn_bool("[passkeys] enabled", live->passkeys_enabled,
                        cand->passkeys_enabled);
  n += reload_warn_str("[passkeys] backend", live->passkeys_backend,
                       cand->passkeys_backend);
  n += reload_warn_str("[passkeys] aaguid", live->passkeys_aaguid,
                       cand->passkeys_aaguid);
  n += reload_warn_str("[passkeys] attestation", live->passkeys_attestation,
                       cand->passkeys_attestation);
  n += reload_warn_bool("[passkeys] virtual_ctap", live->passkeys_virtual_ctap,
                        cand->passkeys_virtual_ctap);

  return n;
}

/* --------------------------- relay set diffing ---------------------------- */

static bool reload_relays_equal(const SignetConfig *a, const SignetConfig *b) {
  if (a->n_relays != b->n_relays) return false;
  for (size_t i = 0; i < a->n_relays; i++) {
    bool found = false;
    for (size_t j = 0; j < b->n_relays && !found; j++) {
      if (g_strcmp0(a->relays[i], b->relays[j]) == 0) found = true;
    }
    if (!found) return false;
  }
  return true;
}

/* ----------------------- provisioner reconciliation ----------------------- */

/* Apply the config's provisioner set to persisted authorization, honouring
 * revocation tombstones, then mirror the result back into the config file so
 * the two stores converge. */
static void reload_reconcile_provisioners(const SignetReloadCtx *ctx,
                                          const SignetConfig *cand,
                                          int64_t now,
                                          SignetReloadReport *report) {
  if (!ctx->store) return;

  /* An env-sourced list cannot be corrected by write-back and cannot have
   * been authored after the process started, so it must never outrank a
   * runtime revocation: source mtime 0 makes every tombstone win. */
  const bool from_env = signet_config_provisioners_from_env();
  const int64_t source_mtime =
      from_env ? 0 : signet_config_source_mtime(ctx->config_path);
  report->provisioner_env_shadowed = from_env;

  SignetProvisionerReconcile stats;
  memset(&stats, 0, sizeof(stats));
  if (signet_store_reconcile_provisioners(
          ctx->store, (const char *const *)cand->provisioner_pubkeys,
          cand->n_provisioner_pubkeys, source_mtime, now, &stats) != 0) {
    g_warning("[signetd] reload: provisioner reconciliation failed; persisted "
              "authorization is unchanged");
    return;
  }
  report->provisioners = stats;

  if (stats.refused_resurrect > 0) {
    g_critical("[signetd] reload: REFUSED to restore %zu provisioner(s) that "
               "were revoked at runtime and are still listed in %s. Config is "
               "the desired state, but a revocation is never undone by a "
               "reload — re-add the pubkey with config/grant-provisioner if "
               "the authority is genuinely intended.",
               stats.refused_resurrect,
               from_env ? "SIGNET_PROVISIONER_PUBKEYS" :
                 (ctx->config_path ? ctx->config_path : "the configuration"));
  }
  if (stats.granted > 0 || stats.revoked > 0) {
    g_message("[signetd] reload: provisioners reconciled (granted=%zu "
              "revoked=%zu unchanged=%zu refused=%zu)",
              stats.granted, stats.revoked, stats.unchanged,
              stats.refused_resurrect);
  }

  /* Write-back: make the config file state what is actually authorized. */
  const bool diverged = stats.refused_resurrect > 0 || stats.granted > 0 ||
                        stats.revoked > 0;
  if (!diverged) return;

  if (from_env) {
    report->provisioner_writeback_failed = true;
    g_critical("[signetd] reload: SIGNET_PROVISIONER_PUBKEYS overrides the "
               "config file, so the authorized provisioner set cannot be "
               "written back. Revocations remain enforced via their "
               "tombstones, but the environment override will keep "
               "disagreeing with persisted state until it is removed.");
    return;
  }

  if (!ctx->config_path || !ctx->config_path[0]) return;

  char **authorized = NULL;
  size_t n_authorized = 0;
  if (signet_store_list_provisioners(ctx->store, &authorized, &n_authorized) != 0) {
    report->provisioner_writeback_failed = true;
    g_warning("[signetd] reload: cannot read persisted provisioner set for "
              "config write-back");
    return;
  }

  char err[256];
  if (signet_config_write_provisioners(ctx->config_path,
                                       (const char *const *)authorized,
                                       n_authorized, err, sizeof(err)) != 0) {
    report->provisioner_writeback_failed = true;
    g_critical("[signetd] reload: provisioner config write-back to '%s' "
               "FAILED: %s. Persisted authorization is correct and revocation "
               "tombstones still block resurrection, but the config file is "
               "now stale.", ctx->config_path, err);
  } else {
    g_message("[signetd] reload: provisioner set written back to %s "
              "(%zu authorized)", ctx->config_path, n_authorized);
  }

  signet_store_free_provisioner_list(authorized, n_authorized);
}

/* ------------------------------ public API -------------------------------- */

#define SIGNET_PROVISIONER_ADOPTION_MARKER "provisioner_desired_state"

/* Order-insensitive, case-insensitive set comparison of pubkey lists. */
static bool reload_pubkey_sets_equal(const char *const *a, size_t na,
                                     const char *const *b, size_t nb) {
  if (na != nb) return false;
  for (size_t i = 0; i < na; i++) {
    bool found = false;
    for (size_t j = 0; j < nb && !found; j++) {
      if (a[i] && b[j] && g_ascii_strcasecmp(a[i], b[j]) == 0) found = true;
    }
    if (!found) return false;
  }
  return true;
}

int signet_config_reload_adopt_persisted(const SignetReloadCtx *ctx, int64_t now,
                                         bool *out_adopted) {
  if (out_adopted) *out_adopted = false;
  if (!ctx || !ctx->store) return -1;

  int marked = signet_store_policy_state_mark_once(
      ctx->store, SIGNET_PROVISIONER_ADOPTION_MARKER, now);
  if (marked < 0) return -1;
  if (marked == 1) return 0; /* invariant already established */

  /* First run under desired-state semantics. This database may already carry
   * runtime revocations performed before tombstones existed, so applying the
   * config over it could resurrect exactly the authority D2 forbids restoring.
   * Adopt the persisted set as the desired state once — this can only ever
   * REMOVE entries from config, never add authority — and write it back, so
   * every later reload starts from config and store already agreeing. */
  char **authorized = NULL;
  size_t n_authorized = 0;
  if (signet_store_list_provisioners(ctx->store, &authorized, &n_authorized) != 0)
    return -1;

  /* If the config file already names exactly the authorized set there is
   * nothing to converge — don't rewrite (and reformat) a file needlessly on
   * every upgrade. */
  {
    SignetConfig current;
    if (signet_config_load(ctx->config_path, &current) == 0) {
      bool same = reload_pubkey_sets_equal(
          (const char *const *)current.provisioner_pubkeys,
          current.n_provisioner_pubkeys,
          (const char *const *)authorized, n_authorized);
      signet_config_clear(&current);
      if (same) {
        signet_store_free_provisioner_list(authorized, n_authorized);
        if (out_adopted) *out_adopted = true;
        return 0;
      }
    }
  }

  int rc = 0;
  if (signet_config_provisioners_from_env()) {
    g_warning("[signetd] provisioner desired-state adoption: "
              "SIGNET_PROVISIONER_PUBKEYS shadows the config file, so the "
              "persisted set (%zu authorized) could not be written back. "
              "Remove the environment override so config can express "
              "revocations.", n_authorized);
    rc = -1;
  } else if (ctx->config_path && ctx->config_path[0]) {
    char err[256];
    if (signet_config_write_provisioners(ctx->config_path,
                                        (const char *const *)authorized,
                                        n_authorized, err, sizeof(err)) != 0) {
      g_warning("[signetd] provisioner desired-state adoption: write-back to "
                "'%s' failed: %s", ctx->config_path, err);
      rc = -1;
    } else {
      g_message("[signetd] provisioner desired-state adopted from persisted "
                "authorization (%zu authorized) and written to %s",
                n_authorized, ctx->config_path);
    }
  }

  signet_store_free_provisioner_list(authorized, n_authorized);
  if (out_adopted) *out_adopted = true;
  return rc;
}

int signet_config_reload_apply(const SignetReloadCtx *ctx, int64_t now,
                               SignetReloadReport *out_report) {
  SignetReloadReport report;
  memset(&report, 0, sizeof(report));

  if (!ctx || !ctx->live) {
    if (out_report) {
      memset(out_report, 0, sizeof(*out_report));
      snprintf(out_report->error, sizeof(out_report->error),
               "reload context is incomplete");
    }
    return -1;
  }

  SignetConfig *live = ctx->live;

  /* 1) Load the candidate. A failed load leaves the running config untouched. */
  SignetConfig cand;
  if (signet_config_load(ctx->config_path, &cand) != 0) {
    snprintf(report.error, sizeof(report.error),
             "failed to load config from '%s'",
             ctx->config_path ? ctx->config_path : "(defaults+env)");
    g_warning("[signetd] reload: %s — retaining last-valid configuration",
              report.error);
    if (out_report) *out_report = report;
    return -1;
  }

  /* 2) Validate before touching anything. Retaining the last-valid config on
   * an invalid candidate is the whole point: a typo in the config file must
   * not disarm a running signer. */
  {
    char err_buf[200];
    if (signet_config_validate(&cand, err_buf, sizeof(err_buf)) != 0) {
      snprintf(report.error, sizeof(report.error), "invalid config: %s", err_buf);
      g_warning("[signetd] reload: %s — retaining last-valid configuration",
                report.error);
      signet_config_clear(&cand);
      if (out_report) *out_report = report;
      return -1;
    }
  }

  /* 3) Restart-only keys: warn, never apply, never silently ignore (D1). */
  report.restart_only_changed = reload_warn_restart_only(live, &cand);

  /* 4) Provisioner authorization first: it is the security-critical stage and
   * it is transactional, so a failure here leaves persisted authority intact
   * before anything else has moved. */
  reload_reconcile_provisioners(ctx, &cand, now, &report);

  /* 5) Relay set. On failure the pool keeps serving the previous relays, and
   * we retain the last-valid config rather than half-applying the candidate. */
  if (!reload_relays_equal(live, &cand)) {
    if (ctx->relays) {
      int rc = signet_relay_pool_set_relays(
          ctx->relays, (const char *const *)cand.relays, cand.n_relays);
      if (rc < 0) {
        snprintf(report.error, sizeof(report.error),
                 "relay pool reconfigure failed");
        g_warning("[signetd] reload: %s — retaining last-valid configuration",
                  report.error);
        signet_config_clear(&cand);
        if (out_report) *out_report = report;
        return -1;
      }
      report.relays_changed = (rc == 0);
    } else {
      report.relays_changed = true;
    }

    if (report.relays_changed) {
      /* fp-e08y: the switch is immediate but the connections are dialled in
       * the background, so this reload returns without waiting on a relay
       * that may be unreachable. Say so, rather than implying connectivity. */
      g_message("[signetd] reload: relay set changed (%zu -> %zu relays); "
                "connecting in background",
                live->n_relays, cand.n_relays);
      if (ctx->mgmt) {
        (void)signet_mgmt_handler_set_relay_urls(
            ctx->mgmt, (const char *const *)cand.relays, cand.n_relays);
      }
      if (ctx->on_relays_changed) ctx->on_relays_changed(ctx->user_data);
    }
  }

  /* 6) Policy: apply eagerly instead of waiting for the next lookup. */
  if (ctx->policy) {
    report.policy_reloaded = (signet_policy_store_reload(ctx->policy, now) == 0);
    if (!report.policy_reloaded)
      g_warning("[signetd] reload: policy store reload failed");
  }

  if (ctx->engine &&
      g_strcmp0(live->policy_default_decision, cand.policy_default_decision) != 0) {
    SignetPolicyDecision d =
        (strcmp(cand.policy_default_decision, "allow") == 0)
            ? SIGNET_POLICY_DECISION_ALLOW
            : SIGNET_POLICY_DECISION_DENY;
    signet_policy_engine_set_default_decision(ctx->engine, d);
    report.default_decision_changed = true;
    g_message("[signetd] reload: policy default decision -> %s",
              cand.policy_default_decision);
  }

  /* 7) Commit the safe scalars and the reloadable arrays into the live config.
   * Only reloadable fields move; restart-only fields keep their running values
   * so components still holding `live` never observe a change they cannot
   * honour. Ownership of the moved arrays transfers to `live`, and the
   * candidate's pointers are nulled so clearing it does not free them. */
  if (live->log_level != cand.log_level) {
    g_message("[signetd] reload: log_level %d -> %d", live->log_level,
              cand.log_level);
    live->log_level = cand.log_level;
  }
  live->reconnect_interval_s = cand.reconnect_interval_s;
  live->rate_limit_rpm = cand.rate_limit_rpm;
  g_strlcpy(live->policy_default_decision, cand.policy_default_decision,
            sizeof(live->policy_default_decision));

  for (size_t i = 0; i < live->n_relays; i++) free(live->relays[i]);
  free(live->relays);
  live->relays = cand.relays;
  live->n_relays = cand.n_relays;
  cand.relays = NULL;
  cand.n_relays = 0;

  for (size_t i = 0; i < live->n_provisioner_pubkeys; i++)
    free(live->provisioner_pubkeys[i]);
  free(live->provisioner_pubkeys);
  live->provisioner_pubkeys = cand.provisioner_pubkeys;
  live->n_provisioner_pubkeys = cand.n_provisioner_pubkeys;
  cand.provisioner_pubkeys = NULL;
  cand.n_provisioner_pubkeys = 0;

  free(live->allowed_kinds);
  live->allowed_kinds = cand.allowed_kinds;
  live->n_allowed_kinds = cand.n_allowed_kinds;
  cand.allowed_kinds = NULL;
  cand.n_allowed_kinds = 0;

  signet_config_clear(&cand);

  report.applied = true;
  g_message("[signetd] reload: configuration applied (restart-only changes "
            "warned: %zu)", report.restart_only_changed);

  if (out_report) *out_report = report;
  return 0;
}
