/* SPDX-License-Identifier: MIT
 *
 * nip5l_transport.h - NIP-5L Unix socket transport for Signet v2.
 *
 * Listens on /run/signet/nip5l.sock with Nostr challenge auth via
 * nostr_auth.c. NIP-46 JSON framing over the Unix socket. Same method
 * set as D-Bus. For environments where D-Bus TCP is not practical or
 * where an existing NIP-5L client is already in use.
 *
 * Supports systemd socket activation (SD_LISTEN_FDS).
 */

#ifndef SIGNET_NIP5L_TRANSPORT_H
#define SIGNET_NIP5L_TRANSPORT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

/**
 * SignetNip5lServer:
 * Opaque NIP-5L Unix socket transport server.
 *
 * Since: 1.0
 */
typedef struct SignetNip5lServer SignetNip5lServer;

struct SignetKeyStore;
struct SignetPolicyRegistry;
struct SignetStore;
struct SignetChallengeStore;
struct SignetAuditLogger;
struct SignetFleetRegistry;
struct SignetRelayPool;

/**
 * SignetNip5lServerConfig:
 * @socket_path: e.g. "/run/signet/nip5l.sock".
 * @keys: borrowed key-store dependency.
 * @policy: borrowed policy dependency.
 * @store: borrowed store dependency.
 * @challenges: borrowed challenge store dependency.
 * @audit: borrowed audit logger dependency.
 * @fleet: borrowed fleet registry dependency.
 * @relays: for get_relays method.
 *
 * Configuration for the NIP-5L Unix socket transport.
 *
 * Since: 1.0
 */
typedef struct {
  const char *socket_path;  /* e.g. "/run/signet/nip5l.sock" */
  struct SignetKeyStore *keys;
  struct SignetPolicyRegistry *policy;
  struct SignetStore *store;
  struct SignetChallengeStore *challenges;
  struct SignetAuditLogger *audit;
  const struct SignetFleetRegistry *fleet;
  struct SignetRelayPool *relays;  /* for get_relays method */
} SignetNip5lServerConfig;

/* Create NIP-5L server. Returns NULL on OOM. */
/**
 * signet_nip5l_server_new:
 * @cfg: (nullable): configuration to use
 *
 * Create NIP-5L server. Returns NULL on OOM.
 *
 * Returns: (transfer full) (nullable): a newly allocated object, or %NULL on failure
 *
 * Since: 1.0
 */
SignetNip5lServer *signet_nip5l_server_new(const SignetNip5lServerConfig *cfg);

/* Free NIP-5L server. Safe on NULL. */
/**
 * signet_nip5l_server_free:
 * @ns: (nullable): a #SignetNip5lServer
 *
 * Free NIP-5L server. Safe on NULL.
 *
 * Since: 1.0
 */
void signet_nip5l_server_free(SignetNip5lServer *ns);

/* Start listening. Returns 0 on success, -1 on failure. */
/**
 * signet_nip5l_server_start:
 * @ns: (not nullable): a #SignetNip5lServer
 *
 * Start listening. Returns 0 on success, -1 on failure.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_nip5l_server_start(SignetNip5lServer *ns);

/* Stop listening. Safe to call multiple times. */
/**
 * signet_nip5l_server_stop:
 * @ns: (nullable): a #SignetNip5lServer
 *
 * Stop listening. Safe to call multiple times.
 *
 * Since: 1.0
 */
void signet_nip5l_server_stop(SignetNip5lServer *ns);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_NIP5L_TRANSPORT_H */
