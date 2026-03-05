/* SPDX-License-Identifier: MIT
 *
 * test_signet_smoke.c - Smoke test for Signet module initialization/teardown.
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

#include <stdlib.h>
#include <string.h>

int main(void) {
  SignetConfig cfg;
  if (signet_config_load(NULL, &cfg) != 0) return 1;

  SignetAuditLoggerConfig alc = {
    .path = NULL,
    .to_stdout = false,
    .flush_each_write = false,
  };
  SignetAuditLogger *audit = signet_audit_logger_new(&alc);

  SignetReplayCacheConfig rcc = {
    .max_entries = 128,
    .ttl_seconds = 60,
    .skew_seconds = 10,
  };
  SignetReplayCache *replay = signet_replay_cache_new(&rcc);

  SignetKeyStoreConfig ksc = { .placeholder = NULL };
  SignetKeyStore *ks = signet_key_store_new(audit, &ksc);

  SignetPolicyStore *ps = signet_policy_store_file_new(NULL);

  SignetPolicyEngineConfig pec = { .default_decision = SIGNET_POLICY_DECISION_DENY };
  SignetPolicyEngine *pe = signet_policy_engine_new(ps, audit, &pec);

  SignetRelayPoolConfig rpc = { .relays = NULL, .n_relays = 0, .on_event = NULL, .user_data = NULL };
  SignetRelayPool *rp = signet_relay_pool_new(&rpc);

  SignetNip46ServerConfig n46c = { .identity = "default" };
  SignetNip46Server *n46 = signet_nip46_server_new(rp, pe, ks, replay, audit, &n46c);

  SignetHealthServerConfig hsc = { .listen = "127.0.0.1:0" };
  SignetHealthServer *hs = signet_health_server_new(&hsc);

  if (hs) {
    SignetHealthSnapshot snap = {
      .relay_connected = false,
      .db_open = false,
      .uptime_sec = 0,
      .agents_active = 0,
      .cache_entries = 0,
      .relay_count = 0,
      .policy_store_loaded = false,
      .key_store_available = false,
    };
    signet_health_server_set_snapshot(hs, &snap);
    (void)signet_health_server_start(hs);
    signet_health_server_stop(hs);
    signet_health_server_free(hs);
  }

  signet_nip46_server_free(n46);
  signet_relay_pool_free(rp);

  signet_policy_engine_free(pe);
  signet_policy_store_free(ps);

  signet_key_store_free(ks);
  signet_replay_cache_free(replay);
  signet_audit_logger_free(audit);

  signet_config_clear(&cfg);
  return 0;
}