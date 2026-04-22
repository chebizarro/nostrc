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
#include "signet/bootstrap_server.h"
#include "signet/nostr_auth.h"
#include "signet/revocation.h"
#include "signet/store.h"
#include "signet/capability.h"
#include "signet/dbus_unix.h"
#include "signet/dbus_tcp.h"
#include "signet/nip5l_transport.h"
#include "signet/ssh_agent.h"

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
#include <sys/resource.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif

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

/* ---- Fleet registry adapter -------------------------------------------- */

/* Forward declaration — struct defined below. */
typedef struct {
  const SignetConfig *cfg;
  SignetNip46Server *nip46;
  SignetMgmtHandler *mgmt;
  SignetDenyList *deny;
  SignetKeyStore *keys;
} SignetDaemonCtx;

/* Fleet membership: a pubkey is in-fleet if it belongs to a provisioned agent.
 * Full NIP-51 fleet list sync is a future enhancement, but checking the agents
 * table is a real authorization gate — unprovisioned pubkeys are rejected. */
static bool signet_fleet_is_in_fleet(const char *pubkey_hex, void *user_data) {
  SignetDaemonCtx *ctx = (SignetDaemonCtx *)user_data;
  if (!ctx || !ctx->keys || !pubkey_hex) return false;

  /* Check if any agent has this pubkey registered. */
  char **ids = NULL;
  size_t count = 0;
  if (signet_key_store_list_agents(ctx->keys, &ids, &count) != 0)
    return false;

  bool found = false;
  for (size_t i = 0; i < count && !found; i++) {
    char pk[65];
    if (signet_key_store_get_agent_pubkey(ctx->keys, ids[i], pk, sizeof(pk))) {
      if (strcmp(pk, pubkey_hex) == 0)
        found = true;
    }
  }
  for (size_t i = 0; i < count; i++) g_free(ids[i]);
  g_free(ids);
  return found;
}

/* Check the deny list. */
static bool signet_fleet_is_denied(const char *pubkey_hex, void *user_data) {
  SignetDaemonCtx *ctx = (SignetDaemonCtx *)user_data;
  if (!ctx || !ctx->deny) return false;
  return signet_deny_list_contains(ctx->deny, pubkey_hex);
}

/* Look up an agent's pubkey from the key store. */
static char *signet_fleet_get_agent_pubkey(const char *agent_id, void *user_data) {
  SignetDaemonCtx *ctx = (SignetDaemonCtx *)user_data;
  if (!ctx || !ctx->keys || !agent_id) return NULL;

  char pubkey[65];
  if (signet_key_store_get_agent_pubkey(ctx->keys, agent_id, pubkey, sizeof(pubkey))) {
    return g_strdup(pubkey);
  }
  return NULL;
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
#if defined(__linux__)
  if (prctl(PR_SET_DUMPABLE, 0) != 0) {
    fprintf(stderr, "signetd: warning: prctl(PR_SET_DUMPABLE, 0) failed: %s\n",
            strerror(errno));
  }
#endif

  /* Lock all current and future pages in memory — prevents key material
   * from being swapped to disk.  sodium_malloc() locks individual allocs,
   * but this covers GLib internals and stack frames that transiently hold
   * secret bytes.
   *
   * Even with CAP_IPC_LOCK, mlockall can fail if RLIMIT_MEMLOCK is too low
   * (Docker defaults to 64 KB).  Try to raise it first. */
  {
    struct rlimit rl;
    if (getrlimit(RLIMIT_MEMLOCK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY) {
      rl.rlim_cur = RLIM_INFINITY;
      rl.rlim_max = RLIM_INFINITY;
      if (setrlimit(RLIMIT_MEMLOCK, &rl) != 0) {
        /* CAP_IPC_LOCK allows unlimited mlock but may not grant setrlimit.
         * Try a generous but finite limit instead. */
        rl.rlim_cur = 512ULL * 1024 * 1024; /* 512 MiB */
        rl.rlim_max = 512ULL * 1024 * 1024;
        (void)setrlimit(RLIMIT_MEMLOCK, &rl);
      }
    }

    int mlock_flags = MCL_CURRENT | MCL_FUTURE;
    if (mlockall(mlock_flags) != 0) {
      /* MCL_FUTURE is the aggressive flag — locks every future mmap/malloc.
       * Fall back to MCL_CURRENT which just locks the pages already mapped. */
      if (mlockall(MCL_CURRENT) != 0) {
        fprintf(stderr, "signetd: warning: mlockall() failed: %s\n"
                "  Per-allocation sodium_malloc mlock is still in effect, but GLib\n"
                "  heap and stack frames may be swappable.  To fix:\n"
                "    Docker:  cap_add: [IPC_LOCK] + ulimits.memlock: -1\n"
                "    systemd: MemoryDenyWriteExecute=no, LimitMEMLOCK=infinity\n"
                "    bare:    setcap cap_ipc_lock+ep signetd\n",
                strerror(errno));
      } else {
        g_message("[signetd] mlockall(MCL_CURRENT) OK (MCL_FUTURE unavailable)");
      }
    } else {
      g_message("[signetd] mlockall(MCL_CURRENT|MCL_FUTURE) OK — all pages locked");
    }
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

  /* Validate required configuration before proceeding. */
  {
    char err_buf[256];
    if (signet_config_validate(&cfg, err_buf, sizeof(err_buf)) != 0) {
      fprintf(stderr, "signetd: configuration error: %s\n", err_buf);
      signet_config_clear(&cfg);
      return 1;
    }
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

  /* 5b) Capability-based policy registry for transport-level access control.
   * Transports (D-Bus, NIP-5L, SSH) use this to enforce per-agent capabilities
   * and rate limits. A default policy is registered with core capabilities
   * and assigned via wildcard ("*") so all agents get baseline enforcement.
   * Provisioned agents can later be assigned more specific policies. */
  SignetPolicyRegistry *cap_registry = signet_policy_registry_new();
  {
    /* Default policy: allow signing and encryption with rate limiting. */
    char *default_caps[] = {
      (char *)SIGNET_CAP_NOSTR_SIGN,
      (char *)SIGNET_CAP_NOSTR_ENCRYPT,
      (char *)SIGNET_CAP_SSH_SIGN,
      (char *)SIGNET_CAP_SSH_LIST_KEYS,
    };
    SignetAgentPolicy default_pol = {
      .name = (char *)"default",
      .capabilities = default_caps,
      .n_capabilities = G_N_ELEMENTS(default_caps),
      .allowed_event_kinds = NULL,   /* all kinds allowed */
      .n_allowed_kinds = 0,
      .disallowed_credential_types = NULL,
      .n_disallowed_types = 0,
      .rate_limit_per_hour = 1000,
    };
    signet_policy_registry_add(cap_registry, &default_pol);
    signet_policy_registry_assign(cap_registry, "*", "default");
  }

  /* 6) Relay pool */
  SignetDaemonCtx dctx;
  memset(&dctx, 0, sizeof(dctx));
  dctx.cfg = &cfg;
  dctx.keys = keys;

  SignetRelayPoolConfig rp_cfg = {
    .relays = (const char *const *)cfg.relays,
    .n_relays = cfg.n_relays,
    .on_event = signet_on_relay_event,
    .user_data = &dctx,
    /* NIP-42: sign AUTH challenges with the bunker's own key */
    .auth_sk_hex = cfg.remote_signer_secret_key_hex[0]
                   ? cfg.remote_signer_secret_key_hex
                   : NULL,
    /* NIP-42: override relay tag URL (e.g. connect via internal address,
     * sign AUTH with public URL). Reads SIGNET_AUTH_RELAY_URL env var. */
    .auth_relay_tag_url = g_getenv("SIGNET_AUTH_RELAY_URL"),
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
    .relay_urls = (const char *const *)cfg.relays,
    .n_relay_urls = cfg.n_relays,
  };
  SignetMgmtHandler *mgmt = signet_mgmt_handler_new(keys, relays, audit, store, &mgmt_cfg);
  dctx.mgmt = mgmt;

  /* 8a) Challenge store (shared by bootstrap, D-Bus TCP, NIP-5L) */
  SignetChallengeStore *challenges = signet_challenge_store_new();

  /* 8b) Deny list + fleet registry for auth */
  SignetStore *base_store = signet_key_store_get_store(keys);
  SignetDenyList *deny = base_store ? signet_deny_list_new(base_store) : NULL;
  dctx.deny = deny;

  /* Build a fleet registry adapter.
   * is_in_fleet: all provisioned agents are fleet members.
   * is_denied: check the deny list.
   * get_agent_pubkey: look up from key store. */
  SignetFleetRegistry fleet_reg;
  memset(&fleet_reg, 0, sizeof(fleet_reg));
  fleet_reg.user_data = &dctx;
  fleet_reg.is_in_fleet = signet_fleet_is_in_fleet;
  fleet_reg.is_denied = signet_fleet_is_denied;
  fleet_reg.get_agent_pubkey = signet_fleet_get_agent_pubkey;

  /* 8c) Bootstrap server */
  SignetBootstrapServer *bootstrap = NULL;
  if (cfg.bootstrap_port > 0) {
    char bs_listen[64];
    snprintf(bs_listen, sizeof(bs_listen), "0.0.0.0:%d", cfg.bootstrap_port);
    SignetBootstrapServerConfig bs_cfg = {
      .listen = bs_listen,
      .keys = keys,
      .store = base_store,
      .challenges = challenges,
      .audit = audit,
      .fleet = &fleet_reg,
      .bunker_pubkey_hex = cfg.remote_signer_pubkey_hex,
      .relay_urls = (const char *const *)cfg.relays,
      .n_relay_urls = cfg.n_relays,
    };
    bootstrap = signet_bootstrap_server_new(&bs_cfg);
    if (bootstrap && signet_bootstrap_server_start(bootstrap) != 0) {
      fprintf(stderr, "signetd: failed to start bootstrap server on %s\n", bs_listen);
      signet_bootstrap_server_free(bootstrap);
      bootstrap = NULL;
    } else if (bootstrap) {
      g_message("[signetd] bootstrap server listening on %s", bs_listen);
    }
  }

  /* 8d) D-Bus Unix transport */
  SignetDbusServer *dbus_unix = NULL;
  if (cfg.dbus_unix_enabled) {
    SignetDbusServerConfig du_cfg = {
      .keys = keys,
      .policy = cap_registry,
      .store = base_store,
      .audit = audit,
      .uid_resolver = NULL,
      .uid_resolver_data = NULL,
      .use_system_bus = true,
    };
    dbus_unix = signet_dbus_server_new(&du_cfg);
    if (dbus_unix && signet_dbus_server_start(dbus_unix) != 0) {
      fprintf(stderr, "signetd: failed to start D-Bus Unix transport\n");
      signet_dbus_server_free(dbus_unix);
      dbus_unix = NULL;
    } else if (dbus_unix) {
      g_message("[signetd] D-Bus Unix transport started");
    }
  }

  /* 8e) D-Bus TCP transport */
  SignetDbusTcpServer *dbus_tcp = NULL;
  if (cfg.dbus_tcp_enabled) {
    char tcp_addr[128];
    snprintf(tcp_addr, sizeof(tcp_addr), "tcp:host=0.0.0.0,port=%d",
             cfg.dbus_tcp_port > 0 ? cfg.dbus_tcp_port : 47472);
    SignetDbusTcpServerConfig dt_cfg = {
      .listen_address = tcp_addr,
      .keys = keys,
      .policy = cap_registry,
      .store = base_store,
      .challenges = challenges,
      .audit = audit,
      .fleet = &fleet_reg,
    };
    dbus_tcp = signet_dbus_tcp_server_new(&dt_cfg);
    if (dbus_tcp && signet_dbus_tcp_server_start(dbus_tcp) != 0) {
      fprintf(stderr, "signetd: failed to start D-Bus TCP transport on %s\n", tcp_addr);
      signet_dbus_tcp_server_free(dbus_tcp);
      dbus_tcp = NULL;
    } else if (dbus_tcp) {
      g_message("[signetd] D-Bus TCP transport started on %s", tcp_addr);
    }
  }

  /* 8f) NIP-5L transport */
  SignetNip5lServer *nip5l = NULL;
  if (cfg.nip5l_enabled) {
    const char *nip5l_path = cfg.nip5l_socket_path[0]
        ? cfg.nip5l_socket_path : "/run/signet/nip5l.sock";
    SignetNip5lServerConfig n5_cfg = {
      .socket_path = nip5l_path,
      .keys = keys,
      .policy = cap_registry,
      .store = base_store,
      .challenges = challenges,
      .audit = audit,
      .fleet = &fleet_reg,
      .relays = relays,
    };
    nip5l = signet_nip5l_server_new(&n5_cfg);
    if (nip5l && signet_nip5l_server_start(nip5l) != 0) {
      fprintf(stderr, "signetd: failed to start NIP-5L transport on %s\n", nip5l_path);
      signet_nip5l_server_free(nip5l);
      nip5l = NULL;
    } else if (nip5l) {
      g_message("[signetd] NIP-5L transport started on %s", nip5l_path);
    }
  }

  /* 8g) SSH agent */
  SignetSshAgent *ssh_agent = NULL;
  if (cfg.ssh_agent_enabled) {
    const char *ssh_path = cfg.ssh_agent_socket_path[0]
        ? cfg.ssh_agent_socket_path : "/run/signet/ssh-agent.sock";
    SignetSshAgentConfig sa_cfg = {
      .socket_path = ssh_path,
      .keys = keys,
      .policy = cap_registry,
      .audit = audit,
      .uid_resolver = NULL,
      .uid_resolver_data = NULL,
    };
    ssh_agent = signet_ssh_agent_new(&sa_cfg);
    if (ssh_agent && signet_ssh_agent_start(ssh_agent) != 0) {
      fprintf(stderr, "signetd: failed to start SSH agent on %s\n", ssh_path);
      signet_ssh_agent_free(ssh_agent);
      ssh_agent = NULL;
    } else if (ssh_agent) {
      g_message("[signetd] SSH agent started on %s", ssh_path);
    }
  }

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

  /* Subscribe to management event kinds (28000-28090) and NIP-46 requests (24133).
   * Without this subscription the daemon connects to the relay but never receives
   * incoming management commands or signing requests.
   *
   * NPA-04: Use scoped subscribe with our bunker pubkey as #p tag filter.
   * This tells the relay to only send us events tagged with our pubkey,
   * dramatically reducing bandwidth on shared relays. */
  {
    static const int signet_kinds[] = {
      24133,                       /* NIP-46 signing requests      */
      SIGNET_KIND_PROVISION_AGENT, /* 28000 */
      SIGNET_KIND_REVOKE_AGENT,    /* 28010 */
      SIGNET_KIND_SET_POLICY,      /* 28020 */
      SIGNET_KIND_GET_STATUS,      /* 28030 */
      SIGNET_KIND_LIST_AGENTS,     /* 28040 */
      SIGNET_KIND_ROTATE_KEY,      /* 28050 */
      /* NOTE: 28090 (MGMT_ACK) intentionally excluded — signetd publishes
       * acks but does not need to receive them; subscribing wastes relay BW. */
    };
    const char *bunker_pk = cfg.remote_signer_pubkey_hex[0]
                              ? cfg.remote_signer_pubkey_hex : NULL;
    if (signet_relay_pool_subscribe_scoped(relays, signet_kinds,
                                           G_N_ELEMENTS(signet_kinds),
                                           bunker_pk, 0) != 0) {
      fprintf(stderr, "signetd: failed to subscribe to event kinds\n");
      goto cleanup;
    }
    g_message("[signetd] subscribed to %zu event kinds on relay pool (p-tag=%s)",
              G_N_ELEMENTS(signet_kinds),
              bunker_pk ? bunker_pk : "unscoped");
  }

  /* Main loop: update health snapshot and wait for shutdown. */
  int64_t started_at = signet_now_unix();
  int64_t last_reconnect_attempt = 0;
  while (!g_shutdown_requested) {
    /* Drain the GLib main context so libnostr's reconnect timers can fire.
     * Without this, NostrSimplePool's GLib-based reconnect logic never runs
     * because g_usleep() does not iterate the main context event queue. */
    g_main_context_iteration(NULL, FALSE);

    if (health) {
      SignetHealthSnapshot snap;
      memset(&snap, 0, sizeof(snap));
      snap.relay_connected = signet_relay_pool_is_connected(relays);

      /* NPA-06: If a relay sent CLOSED for our subscription, re-subscribe.
       * This detects silent subscription termination while the WebSocket
       * stays connected. */
      if (snap.relay_connected && signet_relay_pool_check_sub_closed(relays)) {
        g_warning("[signetd] subscription CLOSED by relay — re-subscribing");
        static const int resub_kinds[] = {
          24133,
          SIGNET_KIND_PROVISION_AGENT,
          SIGNET_KIND_REVOKE_AGENT,
          SIGNET_KIND_SET_POLICY,
          SIGNET_KIND_GET_STATUS,
          SIGNET_KIND_LIST_AGENTS,
          SIGNET_KIND_ROTATE_KEY,
        };
        signet_relay_pool_subscribe_kinds(relays, resub_kinds,
                                          G_N_ELEMENTS(resub_kinds));
      }

      /* Explicit reconnect: if the relay connection dropped, restart the pool
       * and re-subscribe. LibNostr's internal reconnect relies on GLib timers
       * that may not fire reliably when the relay closes an idle socket.
       * Throttle attempts to once every 30 seconds. */
      if (!snap.relay_connected) {
        int64_t now_ts = signet_now_unix();
        if (now_ts - last_reconnect_attempt >= 30) {
          last_reconnect_attempt = now_ts;
          g_message("[signetd] relay disconnected — restarting pool");
          /* NPA-03: Update since to latest event timestamp before reconnect.
           * This ensures we only backfill events from after the last one we
           * processed, avoiding replay of events the cache may have evicted. */
          signet_relay_pool_update_since_from_latest(relays);
          signet_relay_pool_stop(relays);
          if (signet_relay_pool_start(relays) == 0) {
            static const int signet_kinds[] = {
              24133,
              SIGNET_KIND_PROVISION_AGENT,
              SIGNET_KIND_REVOKE_AGENT,
              SIGNET_KIND_SET_POLICY,
              SIGNET_KIND_GET_STATUS,
              SIGNET_KIND_LIST_AGENTS,
              SIGNET_KIND_ROTATE_KEY,
            };
            /* NPA-04: Re-subscribe with scoped filter (bunker pubkey + since).
             * The relay pool caches these params, so subscribe_kinds suffices
             * here — it delegates to subscribe_scoped with stored params. */
            signet_relay_pool_subscribe_kinds(relays, signet_kinds,
                                              G_N_ELEMENTS(signet_kinds));
            g_message("[signetd] relay pool restarted and resubscribed");
          }
          /* Re-check after reconnect attempt */
          snap.relay_connected = signet_relay_pool_is_connected(relays);
        }
      }
      snap.db_open = signet_key_store_is_open(keys);
      snap.agents_active = signet_key_store_cache_count(keys);
      snap.cache_entries = signet_key_store_cache_count(keys);
      snap.relay_count = snap.relay_connected ? (uint32_t)cfg.n_relays : 0;
      snap.policy_store_loaded = (store != NULL);      /* non-blocking hint */
      snap.key_store_available = (keys != NULL);       /* non-blocking hint */

      /* Read atomic counters from subsystem handlers. */
      snap.sign_total = (uint64_t)g_atomic_int_get(&g_signet_metrics.sign_total);
      snap.auth_total_ok = (uint64_t)g_atomic_int_get(&g_signet_metrics.auth_ok);
      snap.auth_total_denied = (uint64_t)g_atomic_int_get(&g_signet_metrics.auth_denied);
      snap.auth_total_error = (uint64_t)g_atomic_int_get(&g_signet_metrics.auth_error);
      snap.bootstrap_total = (uint64_t)g_atomic_int_get(&g_signet_metrics.bootstrap_total);
      snap.revoke_total = (uint64_t)g_atomic_int_get(&g_signet_metrics.revoke_total);
      snap.active_sessions = (uint32_t)g_atomic_int_get(&g_signet_metrics.active_sessions);
      snap.active_leases = (uint32_t)g_atomic_int_get(&g_signet_metrics.active_leases);

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

  /* Shutdown v2 transports (reverse order). */
  if (ssh_agent) {
    signet_ssh_agent_stop(ssh_agent);
    signet_ssh_agent_free(ssh_agent);
    ssh_agent = NULL;
  }
  if (nip5l) {
    signet_nip5l_server_stop(nip5l);
    signet_nip5l_server_free(nip5l);
    nip5l = NULL;
  }
  if (dbus_tcp) {
    signet_dbus_tcp_server_stop(dbus_tcp);
    signet_dbus_tcp_server_free(dbus_tcp);
    dbus_tcp = NULL;
  }
  if (dbus_unix) {
    signet_dbus_server_stop(dbus_unix);
    signet_dbus_server_free(dbus_unix);
    dbus_unix = NULL;
  }
  if (bootstrap) {
    signet_bootstrap_server_stop(bootstrap);
    signet_bootstrap_server_free(bootstrap);
    bootstrap = NULL;
  }

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
  signet_policy_registry_free(cap_registry);
  signet_policy_store_free(store);

  signet_key_store_free(keys);

  signet_replay_cache_free(replay);

  signet_deny_list_free(deny);
  signet_challenge_store_free(challenges);

  signet_audit_logger_free(audit);

  signet_config_clear(&cfg);

  return exit_code;
}