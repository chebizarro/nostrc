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

typedef struct SignetDbusTcpServer SignetDbusTcpServer;

struct SignetKeyStore;
struct SignetPolicyRegistry;
struct SignetStore;
struct SignetChallengeStore;
struct SignetAuditLogger;
struct SignetFleetRegistry;

typedef struct {
  const char *listen_address;  /* e.g. "tcp:host=0.0.0.0,port=47472" */
  struct SignetKeyStore *keys;
  struct SignetPolicyRegistry *policy;
  struct SignetStore *store;
  struct SignetChallengeStore *challenges;
  struct SignetAuditLogger *audit;
  const struct SignetFleetRegistry *fleet;
} SignetDbusTcpServerConfig;

/* Create D-Bus TCP server. Returns NULL on OOM. */
SignetDbusTcpServer *signet_dbus_tcp_server_new(const SignetDbusTcpServerConfig *cfg);

/* Free D-Bus TCP server. Safe on NULL. */
void signet_dbus_tcp_server_free(SignetDbusTcpServer *ds);

/* Start listening. Returns 0 on success, -1 on failure. */
int signet_dbus_tcp_server_start(SignetDbusTcpServer *ds);

/* Stop listening. Safe to call multiple times. */
void signet_dbus_tcp_server_stop(SignetDbusTcpServer *ds);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_DBUS_TCP_H */
