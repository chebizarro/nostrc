/* SPDX-License-Identifier: MIT */

#include "signet/health_server.h"

#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { \
  if (!(expr)) { \
    fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    return 1; \
  } \
} while (0)

int main(void) {
  SignetHealthSnapshot snap;
  memset(&snap, 0, sizeof(snap));

  CHECK(!signet_health_snapshot_is_ready(NULL));
  CHECK(!signet_health_snapshot_is_ready(&snap));

  snap.db_open = true;
  snap.key_store_available = true;
  snap.policy_store_loaded = true;
  snap.relay_connected = true;
  CHECK(!signet_health_snapshot_is_ready(&snap));

  snap.fleet_synced = true;
  CHECK(signet_health_snapshot_is_ready(&snap));

  snap.relay_connected = false;
  CHECK(!signet_health_snapshot_is_ready(&snap));

  snap.relay_connected = true;
  snap.policy_store_loaded = false;
  CHECK(!signet_health_snapshot_is_ready(&snap));

  return 0;
}
