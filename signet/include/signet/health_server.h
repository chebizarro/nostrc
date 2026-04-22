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
 * Serves GET /health, GET /ready, and GET /metrics.
 */

#ifndef SIGNET_HEALTH_SERVER_H
#define SIGNET_HEALTH_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <glib.h>

/* ---- Process-global atomic counters ---- */

/* Shared counter block incremented by subsystem handlers (NIP-46, bootstrap,
 * D-Bus, NIP-5L, revocation).  The main loop reads these into the health
 * snapshot on each tick.  All fields use GLib atomic operations. */
typedef struct {
  volatile gint sign_total;
  volatile gint auth_ok;
  volatile gint auth_denied;
  volatile gint auth_error;
  volatile gint bootstrap_total;
  volatile gint revoke_total;
  volatile gint active_sessions;
  volatile gint active_leases;
} SignetMetricsCounters;

/* Process-global counters.  Defined in health_server.c. */
extern SignetMetricsCounters g_signet_metrics;

typedef struct SignetHealthServer SignetHealthServer;

typedef struct {
  const char *listen; /* "ip:port", e.g. "127.0.0.1:9486" */
} SignetHealthServerConfig;

typedef struct {
  bool relay_connected;
  bool db_open;              /* true if SQLCipher database is open */
  uint64_t uptime_sec;
  uint32_t agents_active;    /* number of active agents */
  uint32_t cache_entries;    /* entries in hot key cache */
  uint32_t relay_count;      /* number of connected relays */
  bool policy_store_loaded;  /* true if policy store loaded/usable */
  bool key_store_available;  /* true if key store initialized/usable */
  bool fleet_synced;         /* true if fleet registry synced at least once */

  /* Prometheus counters (monotonic). */
  uint64_t bootstrap_total;
  uint64_t auth_total_ok;
  uint64_t auth_total_denied;
  uint64_t auth_total_error;
  uint64_t sign_total;
  uint64_t revoke_total;
  int64_t fleet_sync_last_ts;  /* unix timestamp of last fleet sync */
  uint32_t active_sessions;
  uint32_t active_leases;
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