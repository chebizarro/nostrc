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
/**
 * SignetMetricsCounters:
 * @sign_total: sign total value.
 * @auth_ok: auth ok value.
 * @auth_denied: auth denied value.
 * @auth_error: auth error value.
 * @bootstrap_total: bootstrap total value.
 * @revoke_total: revoke total value.
 * @active_sessions: active sessions value.
 * @active_leases: active leases value.
 *
 * Process-global atomic counters exported by health reporting.
 *
 * Since: 1.0
 */
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
/**
 * g_signet_metrics:
 * Process-global Signet metrics counters.
 *
 * Since: 1.0
 */
extern SignetMetricsCounters g_signet_metrics;

/**
 * SignetHealthServer:
 * Opaque HTTP health endpoint server.
 *
 * Since: 1.0
 */
typedef struct SignetHealthServer SignetHealthServer;

/**
 * SignetHealthServerConfig:
 * @listen: "ip:port", e.g. "127.0.0.1:9486".
 *
 * Configuration for the health endpoint listener.
 *
 * Since: 1.0
 */
typedef struct {
  const char *listen; /* "ip:port", e.g. "127.0.0.1:9486" */
} SignetHealthServerConfig;

/**
 * SignetHealthSnapshot:
 * @relay_connected: relay connected value.
 * @db_open: true if SQLCipher database is open.
 * @uptime_sec: uptime sec value.
 * @agents_active: number of active agents.
 * @cache_entries: entries in hot key cache.
 * @relay_count: number of connected relays.
 * @policy_store_loaded: true if policy store loaded/usable.
 * @key_store_available: true if key store initialized/usable.
 * @fleet_synced: true if fleet registry synced at least once.
 * @bootstrap_total: bootstrap total value.
 * @auth_total_ok: auth total ok value.
 * @auth_total_denied: auth total denied value.
 * @auth_total_error: auth total error value.
 * @sign_total: sign total value.
 * @revoke_total: revoke total value.
 * @fleet_sync_last_ts: unix timestamp of last fleet sync.
 * @active_sessions: active sessions value.
 * @active_leases: active leases value.
 *
 * Point-in-time health, readiness, and metrics snapshot.
 *
 * Since: 1.0
 */
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
/**
 * signet_health_server_new:
 * @cfg: (nullable): configuration to use
 *
 * Create health server. Returns NULL on OOM.
 *
 * Returns: (transfer full) (nullable): a newly allocated object, or %NULL on failure
 *
 * Since: 1.0
 */
SignetHealthServer *signet_health_server_new(const SignetHealthServerConfig *cfg);

/* Free server. Safe on NULL. */
/**
 * signet_health_server_free:
 * @hs: (nullable): a #SignetHealthServer
 *
 * Free server. Safe on NULL.
 *
 * Since: 1.0
 */
void signet_health_server_free(SignetHealthServer *hs);

/* Start background listener. Returns 0 on success, -1 on failure. */
/**
 * signet_health_server_start:
 * @hs: (not nullable): a #SignetHealthServer
 *
 * Start background listener. Returns 0 on success, -1 on failure.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_health_server_start(SignetHealthServer *hs);

/* Stop listener. Safe to call multiple times. */
/**
 * signet_health_server_stop:
 * @hs: (nullable): a #SignetHealthServer
 *
 * Stop listener. Safe to call multiple times.
 *
 * Since: 1.0
 */
void signet_health_server_stop(SignetHealthServer *hs);

/* Update snapshot used to answer /health. */
/**
 * signet_health_server_set_snapshot:
 * @hs: (not nullable): a #SignetHealthServer
 * @snap: (not nullable): snap
 *
 * Update snapshot used to answer /health.
 *
 * Since: 1.0
 */
void signet_health_server_set_snapshot(SignetHealthServer *hs, const SignetHealthSnapshot *snap);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_HEALTH_SERVER_H */
