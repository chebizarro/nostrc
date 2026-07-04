/* SPDX-License-Identifier: MIT
 *
 * bootstrap_server.h - HTTP endpoints for Signet v2 bootstrap and re-auth.
 *
 * Endpoints served:
 *   POST /bootstrap  - Verify bootstrap token + pubkey, return existing bunker:// handoff URI
 *   GET  /challenge  - Issue challenge for agent re-authentication
 *   POST /auth       - Verify signed auth event, persist and return a session lease/token
 *
 * Uses libmicrohttpd, same pattern as health_server.
 */

#ifndef SIGNET_BOOTSTRAP_SERVER_H
#define SIGNET_BOOTSTRAP_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "signet/nostr_auth.h"

/**
 * SignetBootstrapServer:
 * Opaque HTTP bootstrap and re-authentication endpoint server.
 *
 * Since: 1.0
 */
typedef struct SignetBootstrapServer SignetBootstrapServer;

struct SignetKeyStore;
struct SignetStore;
struct SignetAuditLogger;

/**
 * SignetBootstrapServerConfig:
 * @listen: "ip:port", e.g. "127.0.0.1:9487".
 * @keys: borrowed key-store dependency.
 * @store: borrowed store dependency.
 * @challenges: borrowed challenge store dependency.
 * @audit: borrowed audit logger dependency.
 * @fleet: for /auth verification.
 * @bunker_pubkey_hex: bunker pubkey hex value.
 * @relay_urls: relay urls value.
 * @n_relay_urls: n relay urls value.
 *
 * Configuration for bootstrap HTTP endpoints.
 *
 * Since: 1.0
 */
typedef struct {
  const char *listen;               /* "ip:port", e.g. "127.0.0.1:9487" */
  struct SignetKeyStore *keys;
  struct SignetStore *store;
  struct SignetChallengeStore *challenges;
  struct SignetAuditLogger *audit;
  const SignetFleetRegistry *fleet;  /* for /auth verification */
  const char *bunker_pubkey_hex;
  const char *const *relay_urls;
  size_t n_relay_urls;
} SignetBootstrapServerConfig;

/* Create bootstrap server. Returns NULL on OOM. */
/**
 * signet_bootstrap_server_new:
 * @cfg: (nullable): configuration to use
 *
 * Create bootstrap server. Returns NULL on OOM.
 *
 * Returns: (transfer full) (nullable): a newly allocated object, or %NULL on failure
 *
 * Since: 1.0
 */
SignetBootstrapServer *signet_bootstrap_server_new(const SignetBootstrapServerConfig *cfg);

/* Free server. Safe on NULL. */
/**
 * signet_bootstrap_server_free:
 * @bs: (nullable): a #SignetBootstrapServer
 *
 * Free server. Safe on NULL.
 *
 * Since: 1.0
 */
void signet_bootstrap_server_free(SignetBootstrapServer *bs);

/* Start background listener. Returns 0 on success, -1 on failure. */
/**
 * signet_bootstrap_server_start:
 * @bs: (not nullable): a #SignetBootstrapServer
 *
 * Start background listener. Returns 0 on success, -1 on failure.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_bootstrap_server_start(SignetBootstrapServer *bs);

/* Stop listener. Safe to call multiple times. */
/**
 * signet_bootstrap_server_stop:
 * @bs: (nullable): a #SignetBootstrapServer
 *
 * Stop listener. Safe to call multiple times.
 *
 * Since: 1.0
 */
void signet_bootstrap_server_stop(SignetBootstrapServer *bs);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_BOOTSTRAP_SERVER_H */
