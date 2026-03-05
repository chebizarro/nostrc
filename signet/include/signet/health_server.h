/* SPDX-License-Identifier: MIT
 *
 * health_server.h - Minimal HTTP /health endpoint for Signet.
 *
 * The health server supports ONLY:
 *   GET /health
 *
 * No management operations are exposed via HTTP. The daemon uses this endpoint
 * solely for monitoring probes.
 *
 * Phase 1: API + stub implementation.
 */

#ifndef SIGNET_HEALTH_SERVER_H
#define SIGNET_HEALTH_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

typedef struct SignetHealthServer SignetHealthServer;

typedef struct {
  const char *listen; /* "ip:port", e.g. "127.0.0.1:9486" */
} SignetHealthServerConfig;

typedef struct {
  /* Existing fields kept for compatibility. */
  bool relay_connected;
  bool vault_reachable;
  uint64_t uptime_sec;

  /* REPOMARK:SCOPE: 1 - Add component-level health flags used by /health JSON response */
  bool policy_store_loaded;  /* true if policy store loaded/usable */
  bool key_store_available;  /* true if key store initialized/usable */
} SignetHealthSnapshot;

/* Create health server. Returns NULL on OOM. */
SignetHealthServer *signet_health_server_new(const SignetHealthServerConfig *cfg);

/* Free server. Safe on NULL. */
void signet_health_server_free(SignetHealthServer *hs);

/* Start background listener. Returns 0 on success, -1 on failure. */
int signet_health_server_start(SignetHealthServer *hs);

/* Stop listener. Safe to call multiple times. */
void signet_health_server_stop(SignetHealthServer *hs);

/* Update snapshot used to answer /health. */
void signet_health_server_set_snapshot(SignetHealthServer *hs, const SignetHealthSnapshot *snap);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_HEALTH_SERVER_H */