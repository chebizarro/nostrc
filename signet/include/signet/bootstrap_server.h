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

typedef struct SignetBootstrapServer SignetBootstrapServer;

struct SignetKeyStore;
struct SignetStore;
struct SignetAuditLogger;

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
SignetBootstrapServer *signet_bootstrap_server_new(const SignetBootstrapServerConfig *cfg);

/* Free server. Safe on NULL. */
void signet_bootstrap_server_free(SignetBootstrapServer *bs);

/* Start background listener. Returns 0 on success, -1 on failure. */
int signet_bootstrap_server_start(SignetBootstrapServer *bs);

/* Stop listener. Safe to call multiple times. */
void signet_bootstrap_server_stop(SignetBootstrapServer *bs);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_BOOTSTRAP_SERVER_H */
