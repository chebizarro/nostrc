/* SPDX-License-Identifier: MIT
 *
 * signetd_main.c - Signet daemon entrypoint.
 *
 * Phase 7:
 * - Full daemon lifecycle with graceful shutdown (SIGINT/SIGTERM).
 * - Module initialization order + reverse-order cleanup.
 * - Relay event callback dispatches NIP-46 request events to nip46_server.
 * - Health server serves GET /health using a fast snapshot (no blocking probes).
 * - Startup/shutdown audit entries.
 *
 * Notes:
 * - SIGHUP handling for audit log rotation / policy reload is implemented in
 *   those modules; signetd does not intercept SIGHUP here.
 */

#include "signet/signet_config.h"
#include "signet/audit_logger.h"
#include "signet/replay_cache.h"
#include "signet/key_store.h"
#include "signet/policy_store.h"
#include "signet/policy_engine.h"
#include "signet/relay_pool.h"
#include "signet/nip46_server.h"
#include "signet/mgmt_protocol.h"
#include "signet/health_server.h"

/* Management event kind range */
#define SIGNET_MGMT_KIND_MIN 28000
#define SIGNET_MGMT_KIND_MAX 28090

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Process hardening */
#include <sys/mman.h>
#include <sys/prctl.h>

#include <glib.h>
#include <json-glib/json-glib.h>

#define SIGNET_VERSION "0.1.0"
#define SIGNET_NIP46_KIND_CIPHERTEXT 24133

static volatile sig_atomic_t g_shutdown_requested = 0;

static void signet_on_term(int signo) {
  (void)signo;
  g_shutdown_requested = 1;
}

static int64_t signet_now_unix(void) {
  return (int64_t)time(NULL);
}

static void signet_usage(FILE *out) {
  fprintf(out, "Usage: signetd [-c <config_path>]\n");
}



static void signet_audit_daemon_event(SignetAuditLogger *audit,
                                      SignetAuditEventType type,
                                      int64_t now,
                                      const char *action,
                                      const char *version,
                                      const char *config_path) {
  if (!audit) return;

  JsonBuilder *b = json_builder_new();
  if (!b) return;

  json_builder_begin_object(b);

  json_builder_set_member_name(b, "ts");
  json_builder_add_int_value(b, (gint64)now);

  json_builder_set_member_name(b, "component");
  json_builder_add_string_value(b, "signetd");

  json_builder_set_member_name(b, "action");
  json_builder_add_string_value(b, action ? action : "");

  json_builder_set_member_name(b, "version");
  json_builder_add_string_value(b, version ? version : "");

  if (config_path) {
    json_builder_set_member_name(b, "config_path");
    json_builder_add_string_value(b, config_path);
  }

  json_builder_end_object(b);

  JsonGenerator *g = json_generator_new();
  if (!g) {
    g_object_unref(b);
    return;
  }

  JsonNode *root = json_builder_get_root(b);
  json_generator_set_root(g, root);
  json_generator_set_pretty(g, FALSE);
  char *json = json_generator_to_data(g, NULL);

  json_node_free(root);
  g_object_unref(g);
  g_object_unref(b);

  if (!json) return;

  (void)signet_audit_log_json(audit, type, json);
  g_free(json);
}

typedef struct {
  const SignetConfig *cfg;
  SignetNip46Server *nip46;
  SignetMgmtHandler *mgmt;
} SignetDaemonCtx;

static void signet_on_relay_event(const SignetRelayEventView *ev, void *user_data) {
  SignetDaemonCtx *ctx = (SignetDaemonCtx *)user_data;
  if (!ctx || !ctx->cfg || !ctx->nip46 || !ev) return;

  if (ev->kind == SIGNET_NIP46_KIND_CIPHERTEXT) {
    (void)signet_nip46_server_handle_event(ctx->nip46,
                                          ctx->cfg->remote_signer_pubkey_hex,
                                          ctx->cfg->remote_signer_secret_key_hex,
                                          ev->pubkey_hex ? ev->pubkey_hex : "",
                                          ev->content ? ev->content : "",
                                          ev->created_at,
                                          ev->event_id_hex ? ev->event_id_hex : "",
                                          signet_now_unix());
    return;
  }

  /* Management event dispatch (kinds 28000-28090). */
  if (ev->kind >= SIGNET_MGMT_KIND_MIN && ev->kind <= SIGNET_MGMT_KIND_MAX && ctx->mgmt) {
    (void)signet_mgmt_handler_handle_event(ctx->mgmt,
                                          ev->pubkey_hex ? ev->pubkey_hex : "",
                                          ev->content ? ev->content : "",
                                          ev->kind,
                                          ev->event_id_hex ? ev->event_id_hex : "",
                                          signet_now_unix());
    return;
  }
}

int main(int argc, char **argv) {
  const char *config_path = NULL;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-c") == 0 && (i + 1) < argc) {
      config_path = argv[++i];
    } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      signet_usage(stdout);
      return 0;
    } else {
      signet_usage(stderr);
      return 2;
    }
  }

  /* ---- Process hardening (before any secrets are loaded) ---- */

  /* Prevent core dumps and /proc/self/mem reads that could leak key material. */
  if (prctl(PR_SET_DUMPABLE, 0) != 0) {
    fprintf(stderr, "signetd: warning: prctl(PR_SET_DUMPABLE, 0) failed: %s\n",
            strerror(errno));
  }

  /* Lock all current and future pages in memory — prevents key material
   * from being swapped to disk.  sodium_malloc() locks individual allocs,
   * but this covers GLib internals and stack frames that transiently hold
   * secret bytes. */
  if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
    /* Non-fatal: may fail without CAP_IPC_LOCK or in containers without
     * the capability. sodium_malloc per-alloc mlock is still in effect. */
    fprintf(stderr, "signetd: warning: mlockall() failed: %s "
            "(consider CAP_IPC_LOCK or --privileged)\n",
            strerror(errno));
  }

  /* ---- Signal handling ---- */

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = signet_on_term;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  (void)sigaction(SIGINT, &sa, NULL);
  (void)sigaction(SIGTERM, &sa, NULL);

  int exit_code = 1;

  SignetConfig cfg;
  if (signet_config_load(config_path, &cfg) != 0) {
    fprintf(stderr, "signetd: failed to load config\n");
    return 1;
  }

  /* 1) Audit logger */
  SignetAuditLoggerConfig audit_cfg = {
    .path = cfg.audit_path,
    .to_stdout = cfg.audit_stdout,
    .flush_each_write = true,
  };
  SignetAuditLogger *audit = signet_audit_logger_new(&audit_cfg);
  signet_audit_daemon_event(audit, SIGNET_AUDIT_EVENT_STARTUP, signet_now_unix(),
                            "startup", SIGNET_VERSION, config_path);

  /* 2) Replay cache */
  SignetReplayCacheConfig replay_cfg = {
    .max_entries = cfg.replay_max_entries,
    .ttl_seconds = cfg.replay_ttl_seconds,
    .skew_seconds = cfg.replay_skew_seconds,
  };
  SignetReplayCache *replay = signet_replay_cache_new(&replay_cfg);

  /* 3) Key store (SQLCipher + mlock'd hot cache) */
  const char *db_key = g_getenv("SIGNET_DB_KEY");
  SignetKeyStoreConfig ks_cfg = {
    .db_path = cfg.db_path,
    .master_key = db_key ? db_key : "",
  };
  SignetKeyStore *keys = signet_key_store_new(audit, &ks_cfg);

  /* 4) Policy store (file-backed) */
  SignetPolicyStore *store = signet_policy_store_file_new(cfg.policy_file_path);

  /* 5) Policy engine */
  SignetPolicyEngineConfig pe_cfg = {
    .default_decision = (strcmp(cfg.policy_default_decision, "allow") == 0)
                            ? SIGNET_POLICY_DECISION_ALLOW
                            : SIGNET_POLICY_DECISION_DENY,
  };
  SignetPolicyEngine *policy = signet_policy_engine_new(store, audit, &pe_cfg);

  /* 6) Relay pool */
  SignetDaemonCtx dctx;
  memset(&dctx, 0, sizeof(dctx));
  dctx.cfg = &cfg;

  SignetRelayPoolConfig rp_cfg = {
    .relays = (const char *const *)cfg.relays,
    .n_relays = cfg.n_relays,
    .on_event = signet_on_relay_event,
    .user_data = &dctx,
  };
  SignetRelayPool *relays = signet_relay_pool_new(&rp_cfg);

  /* 7) NIP-46 server */
  SignetNip46ServerConfig n46_cfg = {
    .identity = cfg.identity,
  };
  SignetNip46Server *nip46 = signet_nip46_server_new(relays, policy, keys, replay, audit, &n46_cfg);
  dctx.nip46 = nip46;

  /* 7b) Management handler */
  SignetMgmtHandlerConfig mgmt_cfg = {
    .provisioner_pubkeys = (const char *const *)cfg.provisioner_pubkeys,
    .n_provisioner_pubkeys = cfg.n_provisioner_pubkeys,
    .bunker_secret_key_hex = cfg.remote_signer_secret_key_hex,
    .bunker_pubkey_hex = cfg.remote_signer_pubkey_hex,
  };
  SignetMgmtHandler *mgmt = signet_mgmt_handler_new(keys, relays, audit, &mgmt_cfg);
  dctx.mgmt = mgmt;

  /* 8) Health server */
  SignetHealthServer *health = NULL;
  if (cfg.health_port > 0) {
    char listen_buf[64];
    snprintf(listen_buf, sizeof(listen_buf), "127.0.0.1:%d", cfg.health_port);
    SignetHealthServerConfig hs_cfg = { .listen = listen_buf };
    health = signet_health_server_new(&hs_cfg);
    if (!health || signet_health_server_start(health) != 0) {
      fprintf(stderr, "signetd: failed to start health server on %s\n", listen_buf);
      if (health) {
        signet_health_server_free(health);
        health = NULL;
      }
    }
  }

  /* Start relay pool after everything is ready. */
  if (!relays || !nip46 || !policy || !store || !keys || !replay) {
    fprintf(stderr, "signetd: initialization failed\n");
    goto cleanup;
  }

  if (signet_relay_pool_start(relays) != 0) {
    fprintf(stderr, "signetd: failed to start relay pool\n");
    goto cleanup;
  }

  /* Main loop: update health snapshot and wait for shutdown. */
  int64_t started_at = signet_now_unix();
  while (!g_shutdown_requested) {
    if (health) {
      SignetHealthSnapshot snap;
      memset(&snap, 0, sizeof(snap));
      snap.relay_connected = signet_relay_pool_is_connected(relays);
      snap.db_open = signet_key_store_is_open(keys);
      snap.agents_active = signet_key_store_cache_count(keys);
      snap.cache_entries = signet_key_store_cache_count(keys);
      snap.relay_count = snap.relay_connected ? (uint32_t)cfg.n_relays : 0;
      snap.policy_store_loaded = (store != NULL);      /* non-blocking hint */
      snap.key_store_available = (keys != NULL);       /* non-blocking hint */

      int64_t now = signet_now_unix();
      snap.uptime_sec = (now >= started_at) ? (uint64_t)(now - started_at) : 0;

      signet_health_server_set_snapshot(health, &snap);
    }

    g_usleep(250 * 1000);
  }

  exit_code = 0;

cleanup:
  signet_audit_daemon_event(audit, SIGNET_AUDIT_EVENT_SHUTDOWN, signet_now_unix(),
                            "shutdown", SIGNET_VERSION, config_path);

  if (health) {
    signet_health_server_stop(health);
    signet_health_server_free(health);
    health = NULL;
  }

  if (relays) {
    signet_relay_pool_stop(relays);
  }

  signet_mgmt_handler_free(mgmt);
  signet_nip46_server_free(nip46);

  if (relays) signet_relay_pool_free(relays);

  signet_policy_engine_free(policy);
  signet_policy_store_free(store);

  signet_key_store_free(keys);

  signet_replay_cache_free(replay);

  signet_audit_logger_free(audit);

  signet_config_clear(&cfg);

  return exit_code;
}