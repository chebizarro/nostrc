/* SPDX-License-Identifier: MIT
 *
 * health_server_stub.c - No-op /health server used when libmicrohttpd is
 * unavailable at build time. Compiled in place of health_server.c so signet
 * links cleanly without MHD_* symbol references (bead nostrc-y4u4).
 *
 * The public API in signet/health_server.h stays intact; the stub simply
 * reports failure from signet_health_server_start() and no-ops the rest.
 */

#include "signet/health_server.h"

#include <stdlib.h>
#include <string.h>

/* Process-global atomic counters -- the header exports these as extern, so we
 * must provide the storage even when the HTTP endpoint itself is disabled.
 * Subsystems still increment these counters; without the stub definition all
 * signet TUs referencing g_signet_metrics would fail to link. */
SignetMetricsCounters g_signet_metrics;

struct SignetHealthServer {
  int disabled; /* purely a marker so returned pointers are non-NULL */
};

bool signet_health_snapshot_is_ready(const SignetHealthSnapshot *snap) {
  return snap && snap->db_open && snap->key_store_available &&
         snap->policy_store_loaded && snap->relay_connected &&
         snap->fleet_synced;
}

SignetHealthServer *signet_health_server_new(const SignetHealthServerConfig *cfg) {
  (void)cfg;
  SignetHealthServer *hs = (SignetHealthServer *)calloc(1, sizeof(*hs));
  if (hs) {
    hs->disabled = 1;
  }
  return hs;
}

void signet_health_server_free(SignetHealthServer *hs) {
  free(hs);
}

int signet_health_server_start(SignetHealthServer *hs) {
  (void)hs;
  /* Built without libmicrohttpd: no HTTP endpoint. Return failure so the
   * daemon reports the disabled state at startup rather than silently
   * pretending the endpoint is live. */
  return -1;
}

void signet_health_server_stop(SignetHealthServer *hs) {
  (void)hs;
}

void signet_health_server_set_snapshot(SignetHealthServer *hs,
                                       const SignetHealthSnapshot *snap) {
  (void)hs;
  (void)snap;
}
