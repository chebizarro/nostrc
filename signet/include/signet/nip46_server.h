/* SPDX-License-Identifier: MIT
 *
 * nip46_server.h - NIP-46 bunker request handling for Signet.
 *
 * This module is responsible for:
 * - validating and replay-protecting incoming NIP-46 events
 * - decrypting/parsing NIP-46 requests
 * - policy evaluation (per client/method/kind)
 * - loading custody keys (hot cache) and producing signatures
 * - emitting encrypted NIP-46 responses to relays
 *
 * Full implementation: replay protection, NIP-44 v2 encryption,
 * policy gating, event signing, and NIP-46 message handling.
 */

#ifndef SIGNET_NIP46_SERVER_H
#define SIGNET_NIP46_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

#include <nostr/nip46/nip46_types.h>

struct SignetRelayPool;
struct SignetPolicyEngine;
struct SignetKeyStore;
struct SignetReplayCache;
struct SignetAuditLogger;

typedef struct SignetNip46Server SignetNip46Server;

typedef struct {
  const char *identity; /* borrowed */
} SignetNip46ServerConfig;

/* Create NIP-46 server. Returns NULL on OOM. */
SignetNip46Server *signet_nip46_server_new(struct SignetRelayPool *relays,
                                           struct SignetPolicyEngine *policy,
                                           struct SignetKeyStore *keys,
                                           struct SignetReplayCache *replay,
                                           struct SignetAuditLogger *audit,
                                           const SignetNip46ServerConfig *cfg);

/* Free server. Safe on NULL. */
void signet_nip46_server_free(SignetNip46Server *s);

/* Handle an incoming relay event view. Returns true if the event was recognized/handled. */
bool signet_nip46_server_handle_event(SignetNip46Server *s,
                                      const char *remote_signer_pubkey_hex,
                                      const char *remote_signer_secret_key_hex,
                                      const char *client_pubkey_hex,
                                      const char *ciphertext,
                                      int64_t created_at,
                                      const char *event_id_hex,
                                      int64_t now);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_NIP46_SERVER_H */