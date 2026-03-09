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

typedef struct SignetNip5lServer SignetNip5lServer;

struct SignetKeyStore;
struct SignetPolicyRegistry;
struct SignetStore;
struct SignetChallengeStore;
struct SignetAuditLogger;
struct SignetFleetRegistry;

typedef struct {
  const char *socket_path;  /* e.g. "/run/signet/nip5l.sock" */
  struct SignetKeyStore *keys;
  struct SignetPolicyRegistry *policy;
  struct SignetStore *store;
  struct SignetChallengeStore *challenges;
  struct SignetAuditLogger *audit;
  const struct SignetFleetRegistry *fleet;
} SignetNip5lServerConfig;

/* Create NIP-5L server. Returns NULL on OOM. */
SignetNip5lServer *signet_nip5l_server_new(const SignetNip5lServerConfig *cfg);

/* Free NIP-5L server. Safe on NULL. */
void signet_nip5l_server_free(SignetNip5lServer *ns);

/* Start listening. Returns 0 on success, -1 on failure. */
int signet_nip5l_server_start(SignetNip5lServer *ns);

/* Stop listening. Safe to call multiple times. */
void signet_nip5l_server_stop(SignetNip5lServer *ns);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_NIP5L_TRANSPORT_H */
