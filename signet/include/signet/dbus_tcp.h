/* SPDX-License-Identifier: MIT
 *
 * dbus_tcp.h - D-Bus TCP transport for Signet v2 cross-host LAN access.
 *
 * Signet-managed D-Bus daemon on TCP port 47472.
 * SASL ANONYMOUS + immediate Nostr-signed challenge via nostr_auth.c.
 * Agent must prove identity before any method call is accepted.
 * Same dispatch interface as dbus_unix.c.
 */

#ifndef SIGNET_DBUS_TCP_H
#define SIGNET_DBUS_TCP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

/**
 * SignetDbusTcpServer:
 * Opaque D-Bus TCP transport server.
 *
 * Since: 1.0
 */
typedef struct SignetDbusTcpServer SignetDbusTcpServer;

struct SignetKeyStore;
struct SignetPolicyRegistry;
struct SignetStore;
struct SignetChallengeStore;
struct SignetAuditLogger;
struct SignetFleetRegistry;
struct SignetFidoService;

/**
 * SignetDbusTcpServerConfig:
 * @listen_address: e.g. "tcp:host=0.0.0.0,port=47472".
 * @keys: borrowed key-store dependency.
 * @policy: borrowed policy dependency.
 * @store: borrowed store dependency.
 * @challenges: borrowed challenge store dependency.
 * @audit: borrowed audit logger dependency.
 * @fido: borrowed FIDO service dependency.
 * @fleet: borrowed fleet registry dependency.
 *
 * Configuration for the D-Bus TCP transport.
 *
 * Since: 1.0
 */
typedef struct {
  const char *listen_address;  /* e.g. "tcp:host=0.0.0.0,port=47472" */
  struct SignetKeyStore *keys;
  struct SignetPolicyRegistry *policy;
  struct SignetStore *store;
  struct SignetChallengeStore *challenges;
  struct SignetAuditLogger *audit;
  struct SignetFidoService *fido;
  const struct SignetFleetRegistry *fleet;
} SignetDbusTcpServerConfig;

/* Create D-Bus TCP server. Returns NULL on OOM. */
/**
 * signet_dbus_tcp_server_new:
 * @cfg: (nullable): configuration to use
 *
 * Create D-Bus TCP server. Returns NULL on OOM.
 *
 * Returns: (transfer full) (nullable): a newly allocated object, or %NULL on failure
 *
 * Since: 1.0
 */
SignetDbusTcpServer *signet_dbus_tcp_server_new(const SignetDbusTcpServerConfig *cfg);

/* Free D-Bus TCP server. Safe on NULL. */
/**
 * signet_dbus_tcp_server_free:
 * @ds: (nullable): a D-Bus server
 *
 * Free D-Bus TCP server. Safe on NULL.
 *
 * Since: 1.0
 */
void signet_dbus_tcp_server_free(SignetDbusTcpServer *ds);

/* Start listening. Returns 0 on success, -1 on failure. */
/**
 * signet_dbus_tcp_server_start:
 * @ds: (not nullable): a D-Bus server
 *
 * Start listening. Returns 0 on success, -1 on failure.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_dbus_tcp_server_start(SignetDbusTcpServer *ds);

/* Stop listening. Safe to call multiple times. */
/**
 * signet_dbus_tcp_server_stop:
 * @ds: (nullable): a D-Bus server
 *
 * Stop listening. Safe to call multiple times.
 *
 * Since: 1.0
 */
void signet_dbus_tcp_server_stop(SignetDbusTcpServer *ds);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_DBUS_TCP_H */
